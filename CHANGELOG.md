# Changelog

What's new in each release of POGO. Downloads are on the
[releases page](https://github.com/soundshed/pogo/releases). Use the app and the firmware from the
same release: the app shows both versions, so you can see if they differ.

## 0.3.0 — 2026-10-10

### New

- **Pedal padding.** A pedal can settle a little off its stop as your foot comes off it. With an
  effect that needs exactly 0 or 127, such as a pitch shift, the sound then moves. Padding fixes
  this: click the pedal in **Configure** and set **Heel padding** and **Toe padding**, from 0 to
  40% of the travel. That much of the travel at each end sends the end value. Each profile has its
  own padding, and the volume knob has it too. The pedal starts with 1% at the heel and 15% at the
  toe. A unit that already has saved settings keeps its pedal as it was, with no padding, until you
  set some or press **Defaults**. The bar beside the drawn pedal shows
  each padding zone and lights it while the pedal is in it, and the pedal's label shows the value
  it sends.
- **Preset names.** Give any preset a name of up to 15 characters. When you select that preset,
  its name shows in large letters beside the bank and letter. For example, name 32D *Fuzz Lead*
  and the screen shows *Fuzz Lead* next to **32D**. In **Configure**, click the unit's screen,
  then use the **Preset names** card. It starts on the preset the unit is on and follows the unit
  as you change presets, so you can name a whole set by stepping through it. Or pick any preset
  from the list. A name belongs to the preset number, not to a profile, so it is the same in every
  profile and stays with its preset if you change **Presets per bank**. It takes the place of the
  first line of text a host sends. Exported settings files include your names. This release's
  settings format is new: use the app and the firmware from this release together. The app won't
  send settings to older firmware. It tells you to update the firmware instead.
- **Bank Up and Bank Down can move your Program Change switches.** If you set switches to send
  Program Changes, click **Bank Up** in **Configure** and turn on **Move PC buttons**, then do the
  same for **Bank Down**. Each press then moves every Program Change switch by a bank as well. For
  example, with four presets per bank, a switch set to PC 0 sends PC 4 after one Bank Up, and PC 0
  again after a Bank Down. Bank Up and Bank Down still change to the next or previous bank's preset
  as before. The screen shows the preset each switch will now choose. It starts off, so saved
  settings behave as they did.
- **Hold a switch for a second action.** Any switch except **Mode** and the pedal's toe switch can
  do something else when you hold it. Click it in **Configure** and set an action on the **Hold**
  card. A quick press then does the switch's normal action when you let go, and holding it for half
  a second does the hold action instead. Set how long a hold is with **Hold time** in the unit's
  settings. Switches without a hold action still act the moment you press them.
- **Tap tempo.** Set a switch to **Tap tempo** and tap it in time. Each tap sends a CC (CC 64 =
  127 by default), the switch's light flashes the tempo, and the screen shows the BPM, such as
  *TAP 120*. If your DAW sends MIDI clock, the light follows the DAW's tempo instead.
- **Switches follow your plugin.** When your plugin or DAW sends back a CC that a toggle switch
  sends, the switch takes that state. For example, if the new preset turns the delay on and the
  plugin sends CC 80 = 127, the switch set to toggle CC 80 lights. Its next press then turns the
  delay off, instead of sending "on" again. Knobs set to **CC absolute** carry on from the value
  the host sends. Note toggles follow notes the same way. **Follow the host** turns this on and off,
  together with following Program Changes.
- **Another relative knob encoding.** Knobs have a third relative mode, **CC relative (1 / 65)**, for
  hosts that call it *signed bit* or *sign magnitude*.
- **The host knows where the pedal is from the start.** When the unit connects, the pedal and the
  volume knob send their position. When you switch profiles or change a mapping, they send their
  position under the new mapping straight away. Before, the host only heard the pedal after you
  moved it.

### Changed

- **The factory switches send CCs, and Tap taps the tempo.** Many effect plugins learn CCs but
  ignore notes, so the switches that sent notes 64–77 now send CCs. **Tap** is tap tempo on CC 64,
  and holding it toggles the tuner on CC 68, the same numbers Line 6 uses. **View**, **Bypass**,
  **Menu**, **Mode**, **Page** and the knob pushes send CC 102–112 and 114 while held. The pedal's
  toe switch toggles CC 113 on and off. This applies to a new unit and to **Defaults**. A unit with
  saved settings keeps its notes.

- **Bank Up and Bank Down wait for you to choose a preset.** Pressing Bank Up or Bank Down now
  picks a bank and sends nothing. **A**–**D** flash, and the screen shows the bank number. Press one
  of them to load that preset. This is how a POD Go behaves, and you can scroll through several
  banks without loading each one. This applies to a new unit and to **Defaults**. A unit with saved
  settings keeps loading a preset at each bank press. To change it, click the unit's screen
  in **Configure** and set **Banks wait for A-D**.
- **Program Change switches stay lit on their preset.** A switch set to send a Program Change now
  stays lit while its preset is the one in use, like **A**–**D**. Before, it was only lit while you
  held it.

- **A–D choose presets by default.** Footswitches **A**–**D** now select the four presets of the
  current bank, as they do on a POD Go, and the one in use stays lit. Before, they sent MIDI notes
  60–63. This applies to a new unit and to **Defaults**. A unit that has saved settings keeps the
  mapping it saved. To get the notes back, click a footswitch in **Configure** and set its
  **Action** to **Note (momentary)**.

### Fixed

- A long SysEx message from another device, such as a preset dump, could change the preset on the
  screen at random. It came after a Program Change and was read as many more. It is now ignored.
- A Program Change that the host sent to another device, on another MIDI channel, moved POGO's
  preset too. POGO now follows only the channels it sends presets on.
- With a **Presets per bank** that doesn't divide 128, such as 3, the last bank is short. Pressing
  **C** there loaded the first preset of all, and **Bank Up** from there landed in the middle of the
  first bank. **C** there now does nothing, and banks wrap as a whole.
- An expression pedal set to **Pitch bend** was slightly sharp at the halfway point. An up-only
  bend started about 9 cents sharp at a 12-semitone range. Halfway is now exactly no bend.
- Switching profiles with a **Note (toggle)** switch on left its note playing. Its note now ends.
  The same happens when the app changes what a switch sends while you hold it.
- With no app reading the MIDI port, a burst of MIDI, such as a pedal sweep, could hold up the
  unit's switches. It could also corrupt the next MIDI message. Messages that can't be sent are now
  dropped whole, without the wait.

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
