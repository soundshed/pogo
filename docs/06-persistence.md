# 06 — Persistence

The eMMC: what is on it, where this project writes, how the firmware is installed and removed, and
how to get back from anything.

---

## 1. Layout

No filesystem; raw 512-byte sectors, addressed by sector number (the card is sector-addressed —
OCR bit 30, measured).

| Sector | Contents | Who writes it |
|---|---|---|
| 0 | section descriptor: `56112603`, `02500000`, CRC32 of the image, image size | install / restore |
| 1 … | the application image in the bootloader's format (§2). Factory: 1,364 sectors. Ours: ~55 | install / restore |
| 1300–1315 | **configuration slot 0** (one sector used) | `storage.c` |
| 1316–1331 | **configuration slot 1** | `storage.c` |
| 0–1364 | the region a verified factory backup covers; the firmware refuses writes past it | |
| 0x8803 … | the SHARC's boot image (785 sectors). **Never written** | — |
| elsewhere | ~66 MiB of manufacturer-provided content, layout not established. **Never written** | — |

The slots sit inside the factory image's body. With the factory image installed, the bootloader's
CRC covers sectors 1..1364, so a slot write would stop the unit booting. That is why a save
is **refused unless sector 1 holds one of our images** — `midi_control_boot_classify()`: magic
`0x230B0C5E`, exactly one scatter-load record, size under 256 KB. The factory image has seven records
and 698,148 bytes. The firmware reads the slots regardless, so a configuration saved before a
factory restore is still used afterwards.

A restore of the factory image therefore **must write all 1,365 sectors** (it overwrites the slots
too — that is correct, the factory body has to be complete). A restore of one of our images may be
any length ≥ its own sector count.

## 2. The bootloader's image format (verified by CRC)

```
0x00  u32  magic 0x230B0C5E
0x04  u32  total size including this 0x34-byte header (refused at ≥ 4 MB)
0x08  u32  CRC32 of the (size - 0x34) bytes that follow
0x0C  u32  2
0x10  u32  record count
0x14  u32  entry point (Thumb bit set) — word 1 of our vector table
0x18  u32  handoff block address: 0x14 bytes of RAM the loader writes just before branching. Not a stack
0x1C  u32  handoff size (0)
0x20  u32  flags: bit 0 set → refused
0x24..0x33 zero

records: u32 len, u32 dest, u32 running offset, then len bytes
```

Every record destination must lie inside one of `0x10000000–0x10020000`, `0x10080000–0x10092000`,
`0x20000000–0x20010000`, `0x28000000–0x29F00000`, or the loader rejects the image. Ours is one
record to `0x10000000`. **The loader branches without setting SP** and with SysTick running
(docs/02 §1). `midi_control_boot_build_emmc()` and `tools/midi_control_boot_image.py build --emmc` produce
sector 0 + image; `midi_control_boot_inspect()` / `info` check one.

## 3. Installing (persistent)

Two tools do the same thing; the app is the one that needs nothing installed.

**The app, Firmware tab** (Windows): step 1 reboot into DFU → step 2 load `midi-control.bin` into
RAM → step 3 back up (two reads, compared, saved under `%APPDATA%\midi-control\backups\`) → step 4
install (builds the eMMC image in memory from the same `.bin`, writes at sector 0, requires a
backup verified *in this session*). Every step has a confirmation dialog and its log.

**The command line:**

```bash
$PY firmware/build.py
$PY tools/midi_control_usb.py reboot-dfu --i-mean-it              # factory system fully booted + 20 s, or POGO
$PY tools/midi_control_usb.py dfu-load firmware/build/midi-control.bin --i-mean-it
$PY tools/midi_control_usb.py dump-emmc backup-a.bin              # twice, and cmp them
$PY tools/midi_control_usb.py dump-emmc backup-b.bin && cmp backup-a.bin backup-b.bin
$PY tools/midi_control_usb.py write-emmc firmware/build/midi-control-emmc.img 0 --i-mean-it
```

The write path buffers the whole image in SDRAM, writes it, reads every sector back, and answers
`WRITTEN AND VERIFIED` or a failure. Then power-cycle: the unit boots straight into POGO.
The firmware reclassifies sector 1 after every write, so a configuration can be saved in the same
session as the install.

## 4. Restoring the factory system

The DFU-loaded POGO image *is* the recovery tool: it carries the dump and write commands.

**From a factory release file, with no backup.** The app's *Restore factory system* card takes the
`.hxf` for the version the unit had — supplied by the user, never shipped or committed here — and does the
whole procedure below behind one confirmation: into DFU from POGO, `midi-control.bin` into RAM, a
verified backup, the release check (below), all 1,365 sectors written, then command 79 and an exact
comparison with the release. A unit whose eMMC already holds exactly the release is left
alone.

By hand, from a backup:

1. Get into DFU: from a unit running POGO, step 1 or `reboot-dfu`, which hands the unit to the boot
   ROM without a reset (docs/01 §3). If that does not bring up the DFU device within 20 s, or the
   unit is not answering, **hold `PAGE >` while powering on**, which always works. The unit appears
   as `1FC9:000C`.
2. Load `midi-control.bin` into RAM (step 2 / `dfu-load`).
3. Restore the backup: app step 5 (pick the file; the app checks it is whole sectors and warns if
   it is short), or `write-emmc backup-a.bin 0 --i-mean-it`. The file must be the **full 1,365
   sectors** for a factory restore, and a factory backup is one whose sector 1 is a factory image: the
   backup step saves whatever is installed, so every backup taken after an install holds POGO.
4. Power-cycle. The unit runs the factory system, including the sectors the slots occupied.

This loop — install, restore, both verified by read-back — was exercised four times in the parent
project with its controller image; the code paths are the same ones.

### The factory image, from a release

A release (`POD_Go_v2.50.0_0x6e984472.hxf`, 44,693,806 bytes) is an IFF container, big-endian, form
type `L6FF`: a HEAD chunk, two `dinf` layout records and one zlib `data` chunk. The main content,
`dinf` id 0, is stored with the bytes of every 32-bit word reversed, and is a chain of sections,
each a 16-byte descriptor and a 0x1f0-byte header — one sector together — then the section data.
The first section, at 0x3c, is the application. `midi_control_hxf_factory_emmc()` copies it from
its descriptor with the byte order restored, pads it with zeros to 1,365 sectors, and refuses it
unless both CRCs hold, the version word is 2, every record is in range and there is more than one
record.

**Measured, 2026-09-15:** from the v2.50 release it produces 1,365 sectors byte-identical to this
unit's verified factory backup — descriptor sector, image and zero tail.

**Which release.** Nothing outside sectors 0–1364 is written — the SHARC image at 0x8803 and ~66 MiB
of content stay as they are — so the application has to come from the release the rest of the unit
came from. Installing POGO writes at most sectors 0–512 (anything larger is not classified as ours)
and the two slots, and the factory image stays everywhere else. `midi_control_boot_residual_compare()`
compares sector 513 to the end of the release's image against the unit's eMMC, the slots left out
unless the eMMC holds a factory image, and the app writes nothing unless every byte agrees.

**Measured against v2.50 on this unit:** 419,620 bytes compared, none differing, in dumps taken with
POGO installed and with the parent project's controller installed; the factory backup and a dump after
a restore are identical to the release outright.

## 5. Rules

- **A verified backup before any write.** Verified means: two reads identical, saved, read back
  from disk identical, and the header at sector 1 parses. The app enforces this; the command line
  relies on you.
- **Never past sector 1364.** The firmware refuses; the tools refuse first.
- **Read back after writing.** `emmc_write()` does not; every caller does.
- **The QSPI NOR is never written.** It holds the bootloader that makes `PAGE >` work.
- **Keep backups local.** They contain copyrighted manufacturer-provided software and content.
- A power cycle is the reset. From a running full image, nothing else resets the unit; POGO's way
  into DFU is not a reset (docs/01 §3).
