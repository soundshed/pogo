# POGO

Replacement firmware that turns a **Line 6 POD Go** into a configurable **USB MIDI controller**,
plus a desktop app that to configure, emulate, and install the firmware. 

**Note: This app is in no way affiliated with, authorized by or endorsed by Line 6 or Yamaha Music Group.**

**Warning: Using this app or custom firmware may destroy your unit, void your warranty and make you sad. The author accepts no liability. Use at your own risk.**

Tested working on a unit that originally has the 2.50 version of the Line 6 firmware.

![alt text](docs/screenshots/pogo-desktop.webp)

- **Firmware** (`firmware/`): every footswitch, button, encoder, the volume knob and the expression
  pedal send MIDI. Each control is configurable over MIDI SysEx and saved on the unit. The LCD shows the preset and the names the host sends, what every
  footswitch and encoder does and its state, and where the pedal and volume sit.
- **App** (`app/`): a JUCE standalone (Windows first). Edits every mapping and global setting,
  applies edits live, saves them on the unit, imports and exports JSON. Contains an **emulator**
  running the same code as the firmware, so the app can be developed and tested with no unit
  attached. On Windows it also **installs the firmware** on the unit — reboot into DFU (Device Firmware Update), load, back
  up the eMMC, install, restore — with nothing else installed.
- **Shared code** (`shared/`): the configuration model, the controller engine, the SysEx protocol,
  the screen drawing and the boot-image format, in plain C, compiled into both the firmware and
  the app. `tests/` exercises it on the host.
- **Docs** (`docs/`): a self-contained specification of the hardware and the software, written so
  that this project can be built on and improved safely without repeating the original hardware
  research. Start at [docs/00-start-here.md](docs/00-start-here.md).


## Quick start

```bash
# Everything: host tests, then the firmware (copied into the app's assets), then the app.
# On Windows, build.bat runs the same with whichever Python 3 it finds
python build.py

# Firmware (needs arm-none-eabi-gcc; see docs/07-building.md for the Python path on this machine)
python firmware/build.py

# Shared library and tests (any CMake + C compiler)
cmake -S . -B build-host -DMIDI_CONTROL_BUILD_APP=OFF && cmake --build build-host && ctest --test-dir build-host

# App (JUCE 8; fetched automatically, or point at a checkout)
cmake -S . -B build-app -DMIDI_CONTROL_JUCE_DIR=/path/to/JUCE && cmake --build build-app --target MidiControlApp
```

Run the app. It starts connected to the emulator: press the virtual footswitches, watch the MIDI
monitor, edit a mapping and see it take effect. With a unit running POGO attached, pick its
MIDI port in the link box; the editor then talks to the real thing. The [Guide](#guide) puts
POGO on a unit and [takes it off again](#restore-the-factory-firmware).

## Safety

A DFU load only ever touches RAM: pull the power and the unit returns to its previous state. The two things that
are **not** undone by a power cycle are the eMMC and the QSPI NOR. This project never writes the
NOR, and writes the eMMC in exactly two places — the firmware image at sector 0 and the two
configuration slots at sectors 1300 and 1316 — never past sector 1364, the region a verified backup
exists for, and never without reading every sector back. The factory bootloader on the NOR is kept
intact, so holding **PAGE >** while powering on always reaches DFU, from which the backup can be
restored. Full rules: [docs/00-start-here.md](docs/00-start-here.md).

## Guide

Putting POGO on a unit, and taking it off again, with the app. Read [Safety](#safety) first, and
turn the unit's volume down before you start.

**You need:** Windows (the Firmware tab is Windows-only); the app, whose **FIRMWARE IMAGE** card
should name `midi-control.bin` (the build puts it in `firmware\` beside `POGO.exe`; **Choose...**
picks another); the POD Go driver package, which POD Go Edit installs — without it the app cannot
see the unit in Firmware Update Mode ([docs/08-emulator-and-app.md](docs/08-emulator-and-app.md) §3);
and, to go back, the factory release file (`.hxf`) for the firmware version the unit had. The
`.hxf` is manufacturer software, not part of this project.

Everything below happens in the **Firmware** tab unless it says otherwise. The chips at its top
light for what is on USB — **FACTORY**, **FIRMWARE UPDATE MODE**, **POGO** — and each card's button
is enabled only when the unit is in the state that card needs. Every card that changes the unit
asks first, and the log shows each stage.

### Try POGO (nothing is written)

1. Power the unit on and wait for its normal screen, then 20 seconds more.
2. **Enter Firmware Update Mode** → **Reboot**. **FIRMWARE UPDATE MODE** lights.
3. **Load into RAM** → **Load**. The unit's screen names each start-up stage; **POGO** lights.
4. The link box switches to POGO's MIDI port (if it does not, pick it). **Configure** now shows
   the unit live: change what a control sends, press it, and watch the drawing follow.

A power cycle brings the factory firmware back. Until POGO is installed, the header shows
**STORAGE LOCKED** and **Save to device** is off: settings last until the power goes.

### Install POGO (writes the eMMC)

1. Try POGO first, as above, and leave it running.
2. **Back up the eMMC** → **Back up**. The log should say the eMMC holds *a factory image*. Keep
   this first backup: it is your factory firmware. **Backups folder** opens
   `%APPDATA%\midi-control\backups\`.
3. **Install to the eMMC** → **Install...** (it needs a backup from this session).
4. Power-cycle. The unit now starts POGO at every power-on, and **Save to device** keeps settings.

### Restore the factory firmware

1. To keep your settings, **Export...** them in **Configure** first: the restore writes over the
   sectors they are saved in.
2. Power the unit on so that it runs POGO. If it does not, power it off and hold **PAGE >** while
   powering it on: it comes up in Firmware Update Mode, which works too. (If it starts its factory
   firmware, there is nothing to restore.)
3. **Restore factory system** → **Factory...**, and choose the `.hxf`. The log names the build it
   holds.
4. Read the confirmation and **Continue**. The app then, stopping at the first thing that fails:
   - takes POGO into Firmware Update Mode, if it is running;
   - loads the firmware image into RAM;
   - backs up the eMMC into the backups folder;
   - checks that the `.hxf` is the version the unit had, against the part of the eMMC that POGO
     never writes, and writes nothing if it is not;
   - writes sectors 0 to 1364, then reads them back and compares them with the release.
5. Do not power off until the log says `factory system restored`. Then power-cycle: the unit
   starts its factory firmware.

If the eMMC already holds exactly that release, the app writes nothing and says so.

| The log says | Then |
|---|---|
| `FAILED: …` | Nothing was written. Run the card again; if the unit is not answering, first power it off and hold **PAGE >** while powering it on |
| `REFUSED: …` | Nothing was written. The line says why; usually the `.hxf` is not the version the unit had |
| `… DID NOT VERIFY …` or `… DOES NOT MATCH THE RELEASE …` | Run the card again before powering off. If it is refused, restore a backup (below) |

If all else fails: **PAGE >** at power-on reaches Firmware Update Mode, **Load into RAM** starts
POGO, and **Restore a backup** → **Restore...** writes a saved backup back. The backup taken before
the install puts the factory firmware back as it was. The same by hand, with
`tools/midi_control_usb.py`: [docs/06-persistence.md](docs/06-persistence.md) §4.

## Layout

```
shared/     the device, minus the hardware: config, engine, protocol, screen, boot image   (C)
firmware/   the board layer: bring-up, scan, ADC, USB, eMMC, and main.c                     (C, ARM)
app/        the JUCE app: links, session, editor, emulator, monitor, firmware installer     (C++)
tests/      host tests for shared/
tools/      midi_control_usb.py (device access from Python) and midi_control_boot_image.py (image format)
docs/       the specification
```

## License

Unless otherwise noted, project-authored files are licensed under the
[GNU Affero General Public License, version 3](LICENSE) (`AGPL-3.0-only`). Third-party components
remain under their respective licences; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Manufacturer-provided software and content remain copyrighted by their owner. eMMC backups contain
that material: keep them local (the `backups/` and `captures/` patterns are gitignored).
