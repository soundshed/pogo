# 08 — The emulator and the app

---

## 1. What is shared, and what is not

```
                       shared/  (C, compiled into both)
        ┌─────────────────────────────────────────────────────────────┐
        │ midi_control_config   the record, defaults, (de)serialise    │
        │ midi_control_engine   events in -> MIDI out, LEDs, program   │
        │ midi_control_protocol SysEx build / parse, 7-bit packing     │
        │ midi_control_device   config + engine + protocol + storage   │
        │ midi_control_gfx/ui   the screen, pixel for pixel            │
        │ midi_control_bootimage the eMMC image format                 │
        └─────────────────────────────────────────────────────────────┘
   firmware/src/main.c                           app/Source/EmulatorLink.cpp
   scan matrix -> midi_control_device_button     (none: the drawn unit plays the emulator
   quadrature  -> midi_control_device_encoder     with EVENT over the link, as it plays
   ADC + range -> midi_control_device_analog      a unit - §2.2)
   TinyUSB     <-> midi_control_device_midi_in   MidiMessage  <-> midi_control_device_midi_in
   eMMC slots  <-> storage ops                   two files    <-> storage ops
   SysTick     ->  midi_control_device_tick      a timer      ->  midi_control_device_tick
   LCD DMA     <-  midi_control_ui_update        (none: the app draws every device from
                                                  GET_STATE - §2.2)
```

The emulator models the *device*, not the *board*. It does not model: ADC scaling, auto-ranging
and hysteresis; switch debounce; the LED multiplex; USB enumeration and timing; the vendor endpoint
and the eMMC commands; the boot sequence. A feature that lives in any of those is tested on the
unit only.

## 2. The app

`app/Source/`:

| File | Role |
|---|---|
| `DeviceLink.h` | the interface a device presents to the app: `sendMidi`, a listener for received MIDI. Message thread only |
| `MidiDeviceLink` | a MIDI in/out pair (JUCE `MidiInput`/`MidiOutput`); input re-posted to the message thread |
| `EmulatorLink` | the in-process device: a `midi_control_device` and its storage as two files under `%APPDATA%\midi-control\emulator\`. It has no screen of its own; the app draws and plays it over the link, as it does a unit. Replies are posted asynchronously, like a real port. A 100 ms timer is its clock, so its display timeout runs |
| `ConfigSession` | the protocol client: a request queue, one in flight, replies matched by order, a 2 s timeout. Owns the editing copy of the configuration and the `autoApply` policy, and carries the live view's GET_STATE and EVENT traffic (§2.2). A STATE that names a different live profile moves the editing copy onto it, keeping whatever has been done to the one being left and sending nothing back — so a profile chosen on the panel is the one the editor shows, and the next edit does not switch the unit away from it (docs/05 §7) |
| `ConfigJson` | the JSON form of the record |
| `ui/HardwareView` | the unit drawn from above (§2.1): screen, knobs, buttons, footswitches and their LED rings, lamps, pedal. Vector drawing on a fixed canvas scaled to the window; the unchanging parts are cached as an image at the display's pixel density. A click selects a control; while the link can be played the same gestures play it |
| `ui/ControlsPanel` | the Configure view: the drawn unit, each control captioned with what it sends, to pick a control from and, when the link reports its state, to watch and to play (§2.2); the picked control's settings as cards; the device buttons (read / send / save / revert / defaults / import / export) |
| | Its *Profiles* card, on the screen's own settings, chooses which profile is being edited — which is the one the unit plays, because the mapping every other card edits is that profile's (docs/05 §7). It renames the profile and sets how many are in use. Holding the drawn Mode switch long enough opens the picker on the device, as holding the real one does: the EVENT press and release are the panel's, and the hold is the device's to time |
| `ui/EmulatorStrip` | under the drawn unit while the emulator is the link: the MIDI the emulator sends, the storage-mode switch and a power cycle |
| `ui/ScreenPreview` | a `midi_control_device` of the editor's own that sends and stores nothing. It mirrors the state the link last reported (`midi_control_device_mirror`), or a unit at rest when there is none, under the configuration being edited; its screen, drawn by `midi_control_ui`, and its LEDs are what the drawn unit shows |
| `ui/Inspector` | the settings cards and the LED colour picker |
| `ui/Captions`, `MappingText` | what a control does under a configuration, in words |
| `ui/Theme` | the palette (the unit screen's own colours), the fonts, and the LookAndFeel every widget draws with |
| `ui/MidiMonitor` | traffic both ways and session notes, one row a message; the live view's own traffic is left out (§2.2) |
| `ui/FirmwarePanel` | the installer (§3), a card per step |
| `usb/WinUsbDevice`, `usb/MidiControlUsb` | the WinUSB layer and the port of `tools/midi_control_usb.py` |
| `MainComponent` | the header (the views, the link selector) and the status bar |

The link box lists the emulator and every MIDI port pair. The app connects to the emulator at
start; when a port with *POGO* in its name appears (`MIDI_CONTROL_PRODUCT_NAME`), it switches to it.
Selecting a link runs IDENTIFY + GET_CONFIG; edits are sent as they are made when *Apply live* is
on. When the device on the link reports its state, the drawn unit in the Configure view is that
device, live, and the clicks that select a control also play it (a switch held, a knob dragged,
scrolled or pushed, the volume knob and the pedal dragged): §2.2.

The emulator's **storage mode** switch (ready / locked / none) reproduces the three states the
unit can be in, so the app's handling of a refused save can be exercised without a unit. **Power
cycle** re-runs `midi_control_device_init`, i.e. re-reads the slot files, and the app then reads the
configuration again — the way to confirm a save persisted.

`POGO --snapshot <dir> [--scale n]` draws every view off screen, against the emulator alone, into
`<dir>` as PNG files at n times the window's size, then quits. It opens no window and no MIDI
port and touches nothing on the bus: it is how the look is checked without a screen, and how
screenshots are made. Before drawing it plays the emulator over the link, as the drawing plays a
unit (footswitch B held, knob 1 turned three detents, the volume knob to 96, the pedal to 72 in
three steps), so the Configure pictures also show the live view (§2.2) working.

The icon is a wah pedal: `app/icons/wah-pedal.svg` for 48 px and up, and `wah-pedal-small.svg`, the
same pedal with the detail taken out, for 16 and 32 px. juceaide renders both into the executable's
icon when CMake configures. JUCE's generated resource script reads the same whatever the drawing,
so `app/CMakeLists.txt` makes a changed drawing re-run the configure and a remade `icon.ico` rebuild
the resources; without that the executable keeps the old icon (seen on this build, 2026-09-15).

### 2.1 The drawn unit

`HardwareView` names the parts a player touches and maps each to the firmware's ids
(`HardwareView::idsOf`):

| Part | Firmware control |
|---|---|
| Footswitches ▲ ▼ A B C D | Bank Up, Bank Down, A, B, C, D; LED columns 1–6 |
| TAP footswitch | Tap; its ring is the Tap lamp, column 7 bit 2 |
| MODE footswitch | Mode; the firmware drives no LED for it |
| VIEW | View |
| ACTION | Bypass — **derived** |
| PAGE < | Menu — **derived** |
| PAGE > | Page, on its own pin |
| Upper and lower knobs | the Home and Menu encoders, and their pushes |
| Knobs 1–5 | encoders 1–5, and their pushes |
| Volume knob, pedal, toe | Volume knob, Expression pedal, Pedal toe switch |
| WAH and VOL lamps | column 7 bits 1 and 0 |

ACTION and PAGE < are derived, not measured: matrix row SWROW1 reads View, Bypass, Menu, Mode, and
the panel reads VIEW, ACTION, PAGE <, MODE. Under the factory mapping Bypass sends note 66 and Menu
note 67; pressing ACTION on a unit settles it, and with the live view (§2.2) the drawing shows which
button went down.

### 2.2 The live view

A device that answers IDENTITY with protocol 2 or later is drawn as it is and played, the emulator
and a unit the same way, through `ConfigSession` (docs/04 §2).

- **Watching.** While the Configure view shows, `ControlsPanel` asks for GET_STATE thirty times a
  second. `ScreenPreview`'s device mirrors each STATE under the configuration being edited, and the
  screen, LEDs, footswitch rings, pressed switches, volume knob, pedal and toe switch are drawn from
  it. Asking is not using a control, so the device's display timeout still runs, and the drawn
  screen goes black when the device's does. A knob's pointer turns by the difference in `enc_turns` between two reports. A switch whose
  `presses` changed is drawn down for 150 ms, so a tap between two reports still shows. The first
  report after a connection sets where the counters are counted from.
- **Playing.** A gesture on the drawing sends EVENT, then GET_STATE, so the drawing catches up in a
  round trip. Two things are drawn without waiting: a switch looks pressed from the click, and the
  volume knob or pedal being dragged or scrolled keeps the position given it, while it moves and for
  300 ms after, so an older report does not pull it back. When the mouse outruns the device, the
  queue merges: an analog position still waiting to be sent is moved, and detents still waiting are
  added to. Switches never merge.
- **What does not match.** The drawn volume knob and pedal show the last position the device took,
  from either side. After the drawing moved one, the unit's own knob or pedal is somewhere else
  until it is moved, and then it wins (docs/04 §4). Knob pointers show turning, not a position:
  each starts where it was last drawn. With *Apply live* off, the drawing is the device's state
  under the settings being edited, which need not be its live settings.
- **Left out of the monitor.** GET_STATE, STATE, EVENT and an event's OK RESULT would bury
  everything else; the MIDI monitor and the emulator's activity list skip them
  (`isLiveViewTraffic`, `MidiText.h`). A refused event, a missing reply, and the channel messages an
  event makes the device send are shown.

A device on protocol 1, or one that refuses GET_STATE, gets a picture instead: the screen at rest
under the settings being edited, at the program GET_STATUS last reported, and a drawing that only
selects.

## 3. The Firmware tab

Windows only. It enumerates three things every two seconds: the factory system (the device WinUSB GUID,
VID `0E41`), the boot ROM in DFU (same GUID, VID `1FC9`), and our firmware's log interface (our
GUID). The tab, and the unit's screen on the way in, call the boot ROM's DFU mode **Firmware Update
Mode**; the code and these documents otherwise say DFU. Each button is enabled only
when the unit is in the state that step needs:

| Step | Needs | Does |
|---|---|---|
| 1 Enter Firmware Update Mode | factory system, or POGO | factory system: the nine-message prelude on the message pipe. POGO: vendor command 68, then up to 20 s for `1FC9:000C`; done only when it appears (docs/01 §3) |
| 2 Load firmware into RAM | DFU device + a `.bin` | `DFU_DNLOAD` in 2048-byte blocks, `GETSTATUS` after each, then a zero-length `DFU_DNLOAD` and the `GETSTATUS` that starts the image; done only when our firmware enumerates, within 30 s |
| 3 Back up eMMC | POGO | command 79 twice, compare, save, read back, classify sector 1 |
| 4 Install to eMMC | POGO + `.bin` + **a backup verified in this session** | build the eMMC image with `midi_control_boot_build_emmc`, command 76 at sector 0, expect `WRITTEN AND VERIFIED` |
| 5 Restore backup | POGO + a file of whole sectors ≤ 1365 | command 76 at sector 0 |
| Restore factory system | POGO or the DFU device + a `.bin` + a user-supplied factory `.hxf` | step 1 from POGO, step 2, step 3's backup; the factory image from the release (`midi_control_hxf_factory_emmc`), refused unless the factory image left on the eMMC, less the configuration slots, matches it (`midi_control_boot_residual_compare`), and when sectors 0 and 1 already hold the release the refusal names an earlier restore that may not have finished; command 76 at sector 0 for all 1,365 sectors, then command 79 and an exact comparison with the release (docs/06 §4) |
| Read device log | POGO | command 126 and read until quiet; then the storage packet (70) |

Every step, and the factory restore, confirms with a dialog that says what is about to happen and
whether a power cycle undoes it. Long operations run on a background thread and log as they go. A
backup's contents are named from its sector 1: a POGO image, a factory image only when both CRCs
hold and every record is in range, otherwise damaged or unrecognised.

The `.bin` for steps 2 and 4 and the factory restore is `firmware/midi-control.bin` beside the executable, shipped from
`app/assets/firmware/`, which `firmware/build.py` fills and the app build copies (docs/07 §2).
Without it the tab walks up from the executable looking for `firmware/build/midi-control.bin`;
*Choose firmware...* picks any other.

The DFU device binds to WinUSB through the POD Go driver package installed with POD Go Edit. Without
it, step 2 cannot see the unit; Zadig binding `1FC9:000C` to WinUSB is the alternative.

## 4. Platforms

The MIDI half of the app is portable JUCE. `WinUsbDevice`/`MidiControlUsb` are Windows; the Firmware tab
says so on other platforms. A libusb port of `MidiControlUsb.cpp` is the whole of what macOS and Linux
need.

## 5. The screen

`midi_control_ui` draws two screens. **Start-up** (the model has no device) names the bring-up
stage, large and at the bottom left, with the SDRAM verdict and the R G B swatches, inside a
one-pixel border that turns a wrong stride into slanted verticals. **Running** is five kinds of
region: the status bar (name, the live profile when more than one is in use, MIDI channel, USB,
storage); the program (bank + letter, `PC n`, the
two `SET_TEXT` lines, or *Bank n* / *Preset x* without them — or, while the profile picker is open,
the picker in its place: the page, the live profile's name and what the switches do); a dial per encoder 1–5, with its CC
and, in the absolute mode, its value; the volume and pedal meters; and a strip per footswitch
naming what it does — the preset it recalls, where a step lands, the CC or note — lit in its LED's
colour while `midi_control_engine_button_active()` says so. While the device's display timeout has
it dark, the running screen is black, all of it, drawn once and not again until a control lights
it; the firmware also switches the panel's power (docs/01 §5).

Drawing goes through a *view*: every word and number the screen shows, copied out of the device as
plain bytes. Each region paints its own background and reads only its own part of the view, so
`midi_control_ui_update()` redraws exactly the regions whose part changed, returns the rectangle it
touched, and builds the same picture a full draw makes. The tests check that through a run of
changes. The app's picture of a device (`ScreenPreview`) uses the update path as the firmware does,
so a region that fails to redraw on the unit fails in the window too.

Text is one stroke font (`shared/src/midi_control_font.c`, an original design): straight lines and
circular arcs on a grid of 40 units to the capital height, drawn with round ends at any size.
Lines, arcs, discs and rounded corners are anti-aliased in integer 1/16 px arithmetic and blended
with what is already in the buffer. In mono a colour is lit when any channel is at 40% or more, so
both screens stay legible without SDRAM, if plainer.
