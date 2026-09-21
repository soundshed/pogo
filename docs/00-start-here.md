# 00 — Start here

What this project is, what it is safe to do to the unit, and what has actually been verified.

---

## 1. The device, in one screen

The Line 6 POD Go is a guitar multi-effects pedal with two processors. An **NXP LPC4350**
(Cortex-M4F, 204 MHz, no flash, 264 KB SRAM, 32 MiB SDRAM) owns the front panel, the 480x272 LCD,
USB, a 1 MiB QSPI NOR flash and a 4 GB eMMC. An **Analog Devices SHARC** DSP owns every audio
converter and does all the audio; the LPC never touches an analog audio pin.

This project replaces what runs on the LPC. It leaves the SHARC alone: the SHARC keeps running
whatever preset was loaded and the guitar still comes out of the outputs, but nothing changes it.
The unit becomes a MIDI controller that happens to have a guitar amp inside it.

```
   front panel  ---> LPC4350 running POGO firmware ---> USB MIDI ---> host
   (7 columns of switches / LEDs / encoders, 2 pots)       |
                                                           v
                                             480x272 LCD: bank, program, host text
```

## 2. The safety contract

Everything rests on three facts, all measured on a unit:

1. **The LPC4350 has no flash.** Its mask-ROM boot loader loads an image into RAM over USB DFU and
   runs it. A DFU load is gone at the next power cycle. Every experiment that only loads over DFU
   is undone by pulling the power.
2. **Only two things survive a power cycle: the QSPI NOR and the eMMC.** The NOR holds the
   bootloader. The eMMC holds the application image the bootloader loads, at sector 1, and the
   SHARC's image far away at sector 0x8803.
3. **Holding `PAGE >` while powering on enters DFU** whatever is on the eMMC, because the NOR
   bootloader is intact and the ROM's DFU path is mask ROM. That is the way back.

From these, the rules:

| Action | Undone by a power cycle? | Rule |
|---|---|---|
| Loading any image over DFU | yes | fine, with the user at the desk |
| Driving any pin or peripheral | yes | fine, within the pin map in docs/01 |
| Writing the QSPI NOR | **no** | **never** — nothing in this project can |
| Writing the eMMC | **no** | only sectors 0..1364, only with a verified backup, only with a read-back — docs/06 |

The eMMC is written by exactly two code paths: the configuration slots, sixteen sectors each at
1300 and 1316 of which a save now uses five (`firmware/src/storage.c`), and the image write used to
install and to restore (`main.c`, vendor command 76). Both refuse anything past sector 1364, and
the slot writes refuse anything past the sixteen sectors the slot owns. The slots are only written when the image at
sector 1 is one of ours, so the factory image's checksum can never be broken by a configuration
save.

**Two things need a person at the desk**: anything that reboots the unit, and anything that
writes the eMMC. Ask before either.

## 3. What is verified, and what is not

Verified on the unit, in the parent project, with the drivers this firmware is built from:

- The boot chain: DFU entry from software, DFU load, the boot-image format, and a custom image
  booting persistently from the eMMC through the NOR bootloader.
- Panel scan, encoders, LEDs, both analog inputs, the LCD in colour from SDRAM, USB MIDI in both
  directions, the vendor endpoint, eMMC read and write with read-back, backup and restore.

**Not yet verified on the unit** (this repository's own additions):

- The POGO firmware as a whole. It compiles and its logic passes 4,900 host checks, but it
  has not been loaded. Expect the first DFU load to need at least one fix.
- The configuration slots: reading and writing sectors 1300–1304 and 1316–1320, and the
  classification of sector 1 that gates the writes. The record outgrew one sector when profiles
  were added (docs/05 §7), so a save is now five sectors written and read back one at a time
  (docs/06 §1); the multi-sector path has run only on the host.
- Profiles: the whole of docs/05 §7 on a unit — the Mode hold, the picker's LEDs and screen, and
  the upgrade of an existing saved configuration into a named profile on the first read after the
  update. All of it passes on the host and none of it has been seen on the panel.
- eMMC initialisation when started by the bootloader rather than by the ROM (the parent project only
  ever used the eMMC from DFU-loaded images).
- The app's WinUSB code. It is a line-by-line port of a proven Python tool, but has not been run.
- The display timeout's backlight switch (docs/01 §5). The timeout itself has run: the screen went
  black at the timeout with the backlight still on (measured, `LCDPWR` only). Switching the
  backlight off drives `PD_16`, the one PWM outside the LED matrix, and that pin has never yet
  been driven by this firmware. Try it with a 30-second timeout: the backlight going
  out confirms it. If the backlight stays on and the footswitch LED rings dim or go out instead,
  `PD_16` is the LED ring brightness; remove the two `board_backlight()` calls in `main.c`. The log
  says `display: dark` and `display: lit`.
- The reboot into DFU from POGO (vendor command 68, docs/01 §3). It performs the NOR bootloader's
  post-reset handoff sequence and has never run. If it fails the unit hangs until
  the power is cycled; `PAGE >` at power-on is still the way into DFU.

Facts in the docs are marked **measured** (observed on a unit), **verified** (confirmed by more than
one independent check) or **derived** (reasoned). Where a document does not say, treat the claim as
an implementation requirement that still needs hardware confirmation.

## 4. The first hardware run

Do it in this order and do not skip a step. Each step is undone by a power cycle until step 6.

1. Build: `firmware/build.py` → `firmware/build/midi-control.bin`.
2. With the factory system fully booted (its normal screen, then 20 seconds more), reboot it into
   DFU: the app's Firmware tab, step 1, or `tools/midi_control_usb.py reboot-dfu --i-mean-it`.
3. Load the image into RAM: step 2, or `dfu-load`.
4. Watch the screen. It narrates the bring-up stage by stage (`CLOCKS`, `EMC`, `COLOUR`, `EMMC`,
   `CONFIG`, `USB`, `RUN`). A stall leaves the last stage on screen; the LEDs going dark with the
   screen still lit is the signature of a stalled core (docs/09).
5. From the app: connect over MIDI, read the configuration, press footswitches, watch the monitor.
   Watch the drawn unit follow the panel (footswitches, knobs, pedal, screen), then click a
   footswitch in the drawing and watch the unit follow it (docs/08 §2.2). Hold Mode for a second
   and watch the picker open on both screens; page with Bank Up and Bank Down, choose with A–D.
   Read the device log (Firmware tab). Try a save: with the factory image still on the eMMC the
   device must answer `storage locked` and write nothing.
6. Only now: back up the eMMC (twice, compared), install, power-cycle, and confirm it boots. Then
   save a configuration and power-cycle again to confirm it persists.

## 5. Document map

| Document | Covers |
|---|---|
| [01 — Hardware](01-hardware.md) | Processors, memory, boot, front panel, display, USB, pins |
| [02 — Firmware platform](02-firmware-platform.md) | What the boot chain leaves, what startup must do, the drivers, the main loop |
| [03 — USB and MIDI](03-usb-and-midi.md) | What the unit presents to the host, the default mapping, the vendor commands |
| [04 — Configuration protocol](04-config-protocol.md) | The SysEx messages, byte by byte |
| [05 — Configuration model](05-config-model.md) | The record, its layout, every field, the defaults |
| [06 — Persistence](06-persistence.md) | The eMMC layout, the slots, install, restore, recovery |
| [07 — Building](07-building.md) | Toolchains, commands, tests, tools |
| [08 — Emulator and app](08-emulator-and-app.md) | The app's structure, what the emulator does and does not model |
| [09 — Hazards](09-hazards.md) | Failure signatures that do not resemble their causes |
