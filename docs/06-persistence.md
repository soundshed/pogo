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
| 1300–1315 | **configuration slot 0** (nine sectors used) | `storage.c` |
| 1316–1331 | **configuration slot 1** | `storage.c` |
| 0–1364 | the region a verified factory backup covers; the firmware refuses writes past it | |
| 0x8803 … | the SHARC's boot image (785 sectors). **Never written** | — |
| elsewhere | ~66 MiB of manufacturer-provided content, layout not established. **Never written** | — |

The slots sit inside the factory image's body. With the factory image installed, the bootloader's
CRC covers sectors 1..1364, so a slot write would stop the unit booting. That is why a save
is **refused unless sector 1 holds one of our images** — `midi_control_boot_classify()`: magic
`0x230B0C5E`, exactly one scatter-load record, size under 256 KB. The factory image has seven records
and 698,148 bytes.

A slot is sixteen sectors and the record is 5,120 bytes, so a save writes ten of them
(`slot_sectors()` in `storage.c`), ascending, each read back before the next is written. A record
used to fit one sector; the profile block of docs/05 §7 made it five, the program names of
docs/05 §8 nine, and the hold actions of docs/05 §3 ten. Nothing else about the layout changed: the same two slots, at the same sectors, and the last sector a save can touch is
still 1331. A write that stops part-way — a failed sector, or power lost — leaves *that* slot
failing its record's CRC, and a save only ever writes the slot that does **not** hold the newest
record, so the newest record is in the other one and is what the next boot loads. The invariant is
the two-slot scheme's, not the write's, which is why growing the record did not need a commit
order.

The firmware reads the slots whatever sector 1 holds. Of the two, it uses the valid record (magic,
version, length and CRC: docs/05 §1) with the higher sequence, and starts from the defaults when
neither slot holds one (`midi_control_device_init()`, and `midi_control_device_reload()` for RELOAD;
derived, and checked in `tests/test_shared.c` with locked storage and neither slot a record). A
factory restore writes the factory image's own bytes over the slots, so **a configuration saved
before a factory restore does not survive it**: POGO loaded over DFU afterwards starts from the
defaults. To keep it, export it from the app's Configure view first (README, Guide).
**Measured, v2.50:** both slots begin with the record's magic in the dumps taken before the restore
of 2026-09-15, and neither does in the dump taken after it. A locked eMMC still holds a saved
configuration only when a factory restore did not reach both slots: one that stopped part-way (§4),
or one shorter than the 1,365 sectors it must be.

A restore of the factory image therefore **must write all 1,365 sectors** (it overwrites the slots
too — that is correct, the factory body has to be complete). A restore of one of our images may be
any length ≥ its own sector count.

### Saves the user did not ask for

SAVE is not the only thing that writes a slot. The analog calibration — the raw travel the unit has
measured for its own pedal, docs/05 §6 — rides in the same record, and
`midi_control_device_tick()` saves it once it has changed and then settled for
`MIDI_CONTROL_CAL_SETTLE_MS` (10 s). This is still `storage.c` and still the two alternating slots
with their read-back, so it is not a third write path; but it is the first write that happens with
nobody at the desk, so it is bounded on every side:

- only after a sweep has confirmed both ends of the travel, so a transient at power-on is never
  written;
- only when the travel differs from what is stored by `MIDI_CONTROL_CAL_STORE_MARGIN` (8 counts) or
  more, so an unchanged pedal writes nothing however long the unit runs;
- only when the live configuration is **not** dirty, so a save nobody asked for never commits edits
  nobody committed;
- only when the state is `READY`, which is the same gate as any other save — the factory image is
  never written over.

In practice that is one write the first time a unit is swept and then nothing until the pedal or
its sensor actually changes. `midi_control_device_save()`'s own rules are unchanged: the slot that
does not hold the newest record, sequence incremented, written and read back, so a failed write
leaves the previous save intact. A failed automatic write leaves the live record saying what is
actually stored, and the next settle tries again.

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

**A refused write still takes the whole transfer.** Both hosts send `{76, 127}`, wait 200 ms, and
send the header and the payload without waiting for an answer, so command 76 always arms, and
whatever follows is consumed by the write path whether or not it will be written. Unarmed, or
abandoned part way, those bytes would reach the command parser as `{command, value}` pairs: any
`{68, 127}` in an image reboots the unit into DFU mid-transfer, and a `{76, ≥127}` arms a write on
garbage (derived from `main.c`, not seen on a unit). Every refusal answers with an `emmc-write`
result whose note starts `REFUSED`, and no sector is written; the hosts treat any answer without
`WRITTEN AND VERIFIED` as failure.

| Refused because | Consumed | Note |
|---|---|---|
| SDRAM failed its boot test: the image could not be buffered, and the read-back compares the card with that same buffer, so garbage could verify | the payload, to the length the header declares, none of it kept | `REFUSED - no SDRAM, nothing written` |
| the header does not start `SSWRIMG!` | everything, until no byte has arrived for 500 ms | `REFUSED - bad magic, nothing written` |
| the header's length is 0 or over 1 MiB | everything, until no byte has arrived for 500 ms | `REFUSED - bad length, nothing written` |
| the target runs past sector 1364 | the payload, buffered and not written | `REFUSED - outside the backed-up sectors` |

A result note is at most 40 characters (`result.c`); a longer one arrives cut short and marked
`TRUNCATED`.

A header that fails its own check says nothing reliable about where the transfer ends: a bad magic
makes the length a guess, and a length over 1 MiB can claim up to 4 GiB, which would deafen the
endpoint until a power cycle. So those two take bytes until the stream goes quiet rather than to the
declared length. Both hosts write the payload back to back and then only read, so 500 ms of silence
is the end of it; their wait for the answer (90 s from the tool, 180 s from the app) is far longer.
Polls of the endpoint bound the wait too: 1,000 empty ones end it when the core clock cannot be
rated and no milliseconds pass (each poll includes a panel scan, about 4 ms, derived). Anything a
host sends during that half second, command or not, is discarded with the rest.

Not covered: a write armed and then left without a complete header (a host that stops after the
arming pair) stays armed, and the next 20 bytes from any host are taken as its header; they then
fail the magic check and are drained as above.

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
compares sector 513 to the end of the release's image against the unit's eMMC, less the slots
(sectors 1300–1331), and the app writes nothing unless every byte agrees.

The slots are left out whatever sector 1 holds. Command 76 writes its image with one `emmc_write()`,
a sector at a time from sector 0 upward, and stops at the first sector that fails (derived from
`main.c` and `emmc.c`). A restore that stops part-way — power lost, or `WRITE FAILED` — therefore
leaves the factory header at sector 1 and, if it stopped short of the slots, a saved configuration
still in them. Were the slots compared, the retry that the app's log asks for would be refused as
the wrong release. The rest of the residual identifies the release on its own (below).

Sectors 0 and 1 are the first a restore writes; `head_identical` says they already hold the release.
When they do and the residual disagrees, a restore of the release was begun and did not finish
right: a sector written wrong (`VERIFY FAILED`, or a read-back that does not match the release), or
one left damaged by a write that stopped (derived). The app refuses, and says that an earlier
restore may have been interrupted or not verified: put back a backup taken before that restore with
*Restore a backup* (step 5), then run the card again. Otherwise the refusal says the `.hxf` is not
the version the unit had.

**Measured against v2.50 on this unit:** 419,620 bytes compared, none differing, in dumps taken with
POGO installed and with the parent project's controller installed; the factory backup and a dump after
a restore are identical to the release outright.

**Simulated from this unit's dumps:** the dump taken before the restore of 2026-09-15 holds POGO
with a configuration in both slots. After a v2.50 restore over it that stopped after sector *k*, the
release check compares the same 419,620 bytes and finds none differing, for every *k* from 0 to
1364; from *k* = 1316 the eMMC is identical to the release. Were the slots compared under a factory
header, every stop from sector 1 to 1315 would be refused (493 bytes differ, the first in sector
1300; 309, the first in sector 1316, once slot 0 is rewritten). `tests/test_shared.c` checks the
same on a synthetic release.

## 5. Rules

- **A verified backup before any write.** Verified means: two reads identical, saved, read back
  from disk identical, and the header at sector 1 parses. The app enforces this; the command line
  relies on you.
- **Never past sector 1364.** The firmware refuses; the tools refuse first.
- **Read back after writing.** `emmc_write()` does not; every caller does.
- **The QSPI NOR is never written.** It holds the bootloader that boots the eMMC image; `PAGE >`
  most likely does not depend on it (docs/00 §2), but nothing here tests that.
- **Keep backups local.** They contain copyrighted manufacturer-provided software and content.
- A power cycle is the reset. From a running full image, nothing else resets the unit; POGO's way
  into DFU is not a reset (docs/01 §3).
