#!/usr/bin/env python3
"""Build and inspect the image the POD Go's NOR bootloader loads.

The bootloader on the QSPI NOR reads this format off the eMMC at LBA 1, checks
it, scatter-loads it into RAM and branches to its entry point. The format is
confirmed three ways: against the `.hxf` payload, against the CRC it stores,
and against the bytes read back off the eMMC.

    0x00  u32  magic 0x230B0C5E
    0x04  u32  total size, including this 0x34 header
    0x08  u32  CRC32 of the (size - 0x34) bytes that follow
    0x0c  u32  2
    0x10  u32  record count
    0x14  u32  entry point, Thumb bit set
    0x18  u32  handoff block address (below) - not a stack
    0x1c  u32  handoff block size, used only in its range check
    0x20..0x33  reserved and zero; bit 0 of 0x20
                makes the loader refuse the image, and a non-zero 0x28 arms a
                further check (0x14004edc, over 0x2c/0x28, against 0x24) whose
                meaning is not established

then `record count` records, each:

    u32  len          bytes of data that follow
    u32  dest         where they are copied to
    u32  run          running offset: the image offset just past this data
    ...  len bytes

**Every destination is range-checked by the loader** and a record outside these
is rejected outright, so a built image that ignores them simply will not run:

    0x10000000..0x10020000   local SRAM bank 0
    0x10080000..0x10092000   local SRAM bank 2
    0x20000000..0x20010000   AHB SRAM
    0x28000000..0x29f00000   SDRAM

**The handoff block.** Just before the branch (0x14001bb8) the loader writes
[v+4] = v+0x10 and [v+0x10] = [0x29f00004], sets r0 = 3 and jumps. It does not
load SP from the header, or at all: the image starts on the loader task's own
stack, in SDRAM, and has to set its own. The handoff block is placed directly
after the final local-SRAM record, and `build`
does the same.

**Sector 0.** The eMMC carries the `.hxf` section descriptor for the image in
the sector before it:

    0x00  u32  0x56112603
    0x04  u32  0x02500000 - as shipped; meaning not established
    0x08  u32  CRC32 of the `size` bytes starting at LBA 1
    0x0c  u32  size - equal to the image header's own total size

Confirmed by the image-size arithmetic (docs/14). Whether the bootloader reads
it is not established; `build --emmc` writes one that follows the rule, so
what is on the eMMC stays self-consistent.

Usage:
    midi_control_boot_image.py info  <image>
    midi_control_boot_image.py build <raw.bin> <out.img> [--dest 0x10000000] [--emmc]

`build` takes a *raw* image - firmware/build/midi-control.raw, not the .bin,
which carries a 16-byte NXP boot header this format does not want. The entry
point defaults to word 1 of its vector table. With --emmc the output starts at
sector 0 - descriptor, then the image - ready for `midi_control_usb.py write-emmc
<out> 0`. `info` takes either layout.

This writes files. It does not touch the device: putting an image on the eMMC is
a separate, irreversible step.
"""
import argparse
import struct
import sys
import zlib

MAGIC = 0x230B0C5E
HEADER_LEN = 0x34
RECORD_LEN = 12
SECTOR = 512

DESCRIPTOR_MAGIC = 0x56112603
DESCRIPTOR_WORD1 = 0x02500000      # as shipped; meaning not established

# The loader writes [v+4] and [v+0x10], so the block needs 0x14 bytes of RAM.
HANDOFF_SPAN = 0x14

RANGES = [
    (0x10000000, 0x10020000, "local SRAM bank 0"),
    (0x10080000, 0x10092000, "local SRAM bank 2"),
    (0x20000000, 0x20010000, "AHB SRAM"),
    (0x28000000, 0x29F00000, "SDRAM"),
]


def in_range(lo, hi):
    """The loader's own check: the whole span must sit inside one range."""
    for a, b, name in RANGES:
        if a <= lo and lo < b and hi < b:
            return name
    return None


def cmd_info(args):
    d = open(args.image, "rb").read()

    descriptor = None
    if len(d) >= SECTOR + HEADER_LEN and struct.unpack_from("<I", d, 0)[0] == DESCRIPTOR_MAGIC:
        descriptor = struct.unpack_from("<4I", d, 0)
        _, word1, dcrc, dsize = descriptor
        got = zlib.crc32(d[SECTOR:SECTOR + dsize]) & 0xFFFFFFFF
        print("sector 0: section descriptor")
        print("  word 1       %08x" % word1)
        print("  size         %d%s" % (dsize, "" if SECTOR + dsize <= len(d)
                                       else "  LONGER THAN THE FILE"))
        print("  crc32        %08x  over sector 1.. = %08x  %s"
              % (dcrc, got, "MATCH" if got == dcrc else "MISMATCH"))
        print()
        d = d[SECTOR:]

    if len(d) < HEADER_LEN:
        sys.exit("too short to be an image")

    (magic, size, crc, two, nrec, entry, handoff, handoff_len,
     flags, check_value, check_len, check_addr) = struct.unpack_from("<12I", d, 0)
    print("header")
    print("  magic        %08x %s" % (magic, "ok" if magic == MAGIC else "BAD"))
    print("  size         %d (0x%x)%s" % (size, size,
          "" if size <= len(d) else "  LONGER THAN THE FILE"))
    if descriptor and descriptor[3] != size:
        print("  NOTE: the sector 0 descriptor says %d" % descriptor[3])
    print("  crc32        %08x" % crc)
    print("  version word %d %s" % (two, "ok" if two == 2 else "BAD - the loader wants 2"))
    print("  records      %d" % nrec)
    print("  entry        %08x" % entry)
    print("  handoff      %08x  size %d" % (handoff, handoff_len))
    if flags & 1:
        print("  +0x20        %08x  bit 0 SET - the loader refuses this image" % flags)
    if check_len:
        print("  +0x24..+0x2c %08x %08x %08x  non-zero: arms the check at 0x14004edc, "
              "meaning <unknown>" % (check_value, check_len, check_addr))

    body = d[HEADER_LEN:size]
    got = zlib.crc32(body) & 0xFFFFFFFF
    print("  crc over %d bytes = %08x  %s"
          % (len(body), got, "MATCH" if got == crc else "MISMATCH"))

    print("\nrecords")
    off = HEADER_LEN
    for i in range(nrec):
        if off + RECORD_LEN > size:
            print("  record %d runs past the end" % i)
            break
        ln, dest, run = struct.unpack_from("<3I", d, off)
        where = in_range(dest, dest + ln) if ln else "(empty)"
        print("  %2d  len %7d  dest %08x..%08x  run %08x  %s"
              % (i, ln, dest, dest + ln, run, where or "OUT OF RANGE - would be rejected"))
        off += RECORD_LEN + ln

    # The entry and the handoff block are range-checked too, before the branch.
    print("\nentry/handoff checks, as the loader makes them")
    print("  entry   %08x  %s" % (entry, in_range(entry, entry) or "OUT OF RANGE"))
    print("  handoff %08x  %s" % (handoff, in_range(handoff, handoff + handoff_len)
                                  or "OUT OF RANGE"))
    if not in_range(handoff, handoff + HANDOFF_SPAN):
        print("  handoff %08x..%08x: the loader's two writes there leave RAM"
              % (handoff, handoff + HANDOFF_SPAN))
    return 0


def cmd_build(args):
    raw = open(args.raw, "rb").read()
    if raw[:2] == b"\x1a\x00":
        sys.exit("%s starts with an NXP boot header (1a 00). Use the .raw, not the .bin -\n"
                 "this format carries no such header." % args.raw)

    dest = int(args.dest, 0)
    entry = int(args.entry, 0) if args.entry else struct.unpack_from("<I", raw, 4)[0]

    where = in_range(dest, dest + len(raw))
    if not where:
        sys.exit("destination %08x..%08x is outside every range the loader accepts"
                 % (dest, dest + len(raw)))
    if not in_range(entry, entry):
        sys.exit("entry %08x is outside every range the loader accepts" % entry)

    # Directly after the image. The loader writes it after copying the records,
    # so it must not overlap them.
    handoff = int(args.handoff, 0) if args.handoff else (dest + len(raw) + 3) & ~3
    if not in_range(handoff, handoff + HANDOFF_SPAN):
        sys.exit("handoff block %08x..%08x is not in RAM the loader accepts"
                 % (handoff, handoff + HANDOFF_SPAN))
    if handoff < dest + len(raw) and dest < handoff + HANDOFF_SPAN:
        sys.exit("handoff block %08x overlaps the image at %08x..%08x"
                 % (handoff, dest, dest + len(raw)))

    # One record is enough: the image is a flat blob and its own startup code
    # does whatever copying it needs once it is running.
    run = HEADER_LEN + RECORD_LEN + len(raw)
    body = struct.pack("<3I", len(raw), dest, run) + raw
    size = HEADER_LEN + len(body)
    crc = zlib.crc32(body) & 0xFFFFFFFF

    header = struct.pack("<8I", MAGIC, size, crc, 2, 1, entry, handoff, 0)
    header += b"\x00" * (HEADER_LEN - len(header))
    img = header + body

    out = img
    if args.emmc:
        descriptor = struct.pack("<4I", DESCRIPTOR_MAGIC, DESCRIPTOR_WORD1,
                                 zlib.crc32(img) & 0xFFFFFFFF, len(img))
        out = (descriptor + b"\x00" * (SECTOR - len(descriptor))
               + img + b"\x00" * (-len(img) % SECTOR))

    with open(args.out, "wb") as f:
        f.write(out)

    print("wrote %s" % args.out)
    print("  image %d bytes, one record of %d bytes to %08x  [%s]"
          % (len(img), len(raw), dest, where))
    print("  entry %08x  handoff %08x  crc32 %08x" % (entry, handoff, crc))
    if args.emmc:
        print("  eMMC layout: descriptor in sector 0, image from sector 1, %d sectors in all"
              % (len(out) // SECTOR))
    if entry & 1 == 0:
        print("  NOTE: the entry point has no Thumb bit set, which will fault")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info", help="parse and verify an image, with or without sector 0")
    p.add_argument("image")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("build", help="wrap a raw image in the loader's format")
    p.add_argument("raw")
    p.add_argument("out")
    p.add_argument("--dest", default="0x10000000")
    p.add_argument("--entry", default=None)
    p.add_argument("--handoff", default=None,
                   help="handoff block address (default: just past the image)")
    p.add_argument("--emmc", action="store_true",
                   help="put sector 0's descriptor first, for write-emmc <out> 0")
    p.set_defaults(func=cmd_build)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
