# Changelog

What's new in each release of POGO. Downloads are on the
[releases page](https://github.com/soundshed/pogo/releases). Use the app and the firmware from the
same release: the app shows both versions, so you can see if they differ.

## 0.2.0 — 2026-09-21

### New

- **Profiles.** Keep up to eight named sets of mappings on the unit and switch between them with
  your feet: hold **Mode**, press **A**–**D** to pick one, and use **Bank Up** / **Bank Down** to
  see the next four. In the app, click the unit's screen: the **Profiles** card names them and sets how many you use. If you
  switch profiles on the unit, the app switches too. The MIDI channel, the screen name, the display
  timeout and the pedal calibration belong to the unit, so they stay the same in every profile.
  Exported settings files include your profiles.
- **The pedal and the volume knob learn their range.** Each unit's pedal travels a little
  differently. POGO now adjusts to yours as you use it, so heel and toe reach the ends of the range.
  After installing, sweep the pedal from heel to toe and back a couple of times.
- **Calibrate the pedal by hand.** If the pedal still doesn't reach the full range, click the
  pedal in **Configure** and then **Calibrate...** (on the **Travel** row), or hold the toe switch for 2 seconds on the unit. Then hold
  the pedal still at one end, then at the other, and follow the screen. **Mode** cancels.

### Fixed

- The screen no longer flickers off and on every 30 seconds after the display timeout turns it
  off. Moving the pedal now wakes it.
- The pedal no longer loses its calibration after the screen has been dark. This had the same cause
  as the flicker.
- Knobs set to relative steps of 64 sent nothing when turned clockwise. Relative steps now go up
  to 63.
- With a unit plugged in, the app sometimes switched to the emulator a moment after starting. It now
  stays connected to the unit.
- Opening a damaged or wrong file as an eMMC backup could crash the app. The app now rejects the
  file.
- A long device name with accented or non-Latin characters, in an imported settings file, could run
  past its space. It is now cut to fit.
- **Restore factory system:** a second try after a failed restore is no longer refused because of
  your saved POGO settings. If an earlier restore was interrupted, the app says so and tells you
  what to do next.

### Updating from 0.1.0

- Your saved settings carry over. They become your first profile, named *IMPORTED* if you had
  changed anything from the defaults.
- Put the new firmware on the unit the same way as the first time: **Try POGO**, then
  **Install POGO** ([Guide](README.md#guide)). If **Enter Firmware Update Mode** doesn't respond,
  power the unit off and hold **PAGE >** while powering it on.
- Keep the backup you took before your first install. It holds your factory firmware; backups taken
  since then hold POGO.
- Saved POGO settings don't survive **Restore factory system**. To keep them, **Export...** them
  first.

## 0.1.0 — 2026-09-16

First release.

- Every footswitch, button, encoder, the volume knob and the expression pedal send MIDI, and you can
  set what each one sends.
- The screen shows the preset and the names your software sends, what every footswitch and encoder
  does, and where the pedal and volume sit.
- The Windows app edits every control, applies edits as you make them, saves them on the unit, and
  imports and exports settings files.
- A built-in emulator lets you try everything in the app with no unit attached.
- The app loads POGO into the unit's memory to try it without changing anything, installs it,
  backs up the unit, and restores the factory firmware.
