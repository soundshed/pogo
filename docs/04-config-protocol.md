# 04 — Configuration protocol

MIDI System Exclusive, over the same MIDI interface the controls use. Implemented once, in
`shared/src/midi_control_protocol.c` (messages) and `shared/src/midi_control_device.c` (behaviour), and used by
the firmware, the emulator and the app's `ConfigSession`.

---

## 1. Framing

```
F0 7D 53 47 <command> <payload ...> F7
```

`7D` is the manufacturer id reserved for non-commercial use; `53 47` is `"SG"`. Every payload byte
is 7-bit. Binary records travel **8-to-7 packed**: each group of up to seven bytes becomes one byte
holding the seven high bits (bit *i* for byte *i*) followed by the seven low-7 bytes. The 296-byte
configuration record packs to 339. `MIDI_CONTROL_CONFIG_MAX_BYTES` (296) bounds what a CONFIG
message will carry, and `MIDI_CONTROL_SYSEX_MAX` sizes every buffer from it.

The device answers every command it recognises with **exactly one reply** and never sends SysEx
unsolicited, so a host pairs replies with requests by order. An `EVENT` also makes the device send
the channel messages the control sends, before its reply; a host pairing by order passes over them.
The device parses a SysEx that arrives in any chunking, ignores real-time bytes inside it, and
ignores any SysEx that is not ours.

## 2. Messages

| Command (host → device) | Payload | Reply |
|---|---|---|
| `01` IDENTIFY | — | `41` IDENTITY |
| `02` GET_CONFIG | — | `42` CONFIG |
| `03` SET_CONFIG | `len_hi7 len_lo7` then the packed record | `43` RESULT |
| `04` SAVE | — | `43` RESULT |
| `05` LOAD_DEFAULTS | — | `43` RESULT |
| `06` GET_STATUS | — | `46` STATUS |
| `07` SET_TEXT | `line` (0 or 1), then up to 32 ASCII bytes | `43` RESULT |
| `08` RELOAD | — | `43` RESULT |
| `09` GET_STATE | — | `49` STATE |
| `0A` EVENT | `kind id value` | `43` RESULT |
| `0B` CALIBRATE | `analog start` | `43` RESULT |
| anything else | — | `43` RESULT with status `unknown command` |

`09` and `0A` are protocol version 2. A host checks `protocol_version` in IDENTITY before sending
them; a version 1 device answers them `unknown command`. Version 3 adds one byte to the end of
STATE, the display's state, and changes nothing else. Version 4 adds three more, the active profile
and the profile picker, and changes nothing else. Version 5 adds `0B` CALIBRATE and sixteen bytes to
the end of STATE, the calibration and the pedal's raw reading; a version 4 device answers CALIBRATE
`unknown command`. Version 6 adds one byte to the end of STATE, how far Bank Up and Down have moved
the Program Change buttons (docs/05 §3), and changes nothing else.

**Nothing here selects a profile.** A player chooses one with Mode held and then A–D (docs/05 §7),
so an `EVENT` that holds Mode chooses one on a device the same way, through the same code; and a
host that wants to switch without the panel sends `SET_CONFIG` with another profile active. Adding
a command for it would be a second way to do one thing, and the second way is the one that would
rot.

### `41` IDENTITY

```
protocol_version   6
fw_major fw_minor fw_patch
platform           0 hardware, 1 emulator
storage_state      see §3
dirty              1 if the live configuration differs from what was loaded or saved
config_version     4
config_bytes       hi7 lo7  (4400)
name               up to 16 ASCII bytes, the configured device name
```

### `42` CONFIG

`len_hi7 len_lo7` (4400) then the packed record, which packs 8-to-7 to 5,029 bytes. The record is
docs/05, CRC included; the host validates it the same way the device does. Sent in reply to
GET_CONFIG; the same payload shape is what SET_CONFIG carries. A 2,352-byte version 3 record is
still accepted, with every program unnamed (docs/05 §8), and so are a 296-byte version 2 record and
a 280-byte version 1 record, both of which come back with their mapping as the unit's first profile
(docs/05 §1, §7).

Firmware from before version 4 cannot take a version 4 SET_CONFIG. The message is 5,037 bytes and
its receive buffer holds 2,696, so it drops the message and sends no reply. The app therefore
checks `config_version` in IDENTITY and does not send a record to firmware that reads an older
one. It says to install the firmware from the same release instead. GET_CONFIG from that firmware
still works: its version 3 record is read.

SET_CONFIG replaces the mapping and **never** the analog calibration: that block describes the
pedal wired to this unit, not the one the record was written on, so the device keeps its own
(docs/05 §6). LOAD_DEFAULTS is the one command that clears it.

The record carries all eight profiles and all 128 program names, so a SET_CONFIG sent for a
one-field edit is 5,029 payload bytes rather than 339. That is the price of one record describing everything the unit does, and
over USB MIDI it is not a price anybody notices.

### `43` RESULT

`command status`. `command` is the request being answered. Status codes:

| | | |
|---|---|---|
| 0 | OK | |
| 1 | bad length | record shorter than its version says, or declared length ≠ unpacked length |
| 2 | bad magic | |
| 3 | bad version | a `MIDI_CONTROL_CONFIG_VERSION` this build does not read; 1 and 2 are both read |
| 4 | bad CRC | |
| 5 | no storage | this device has nowhere to persist (emulator with storage off; hardware whose eMMC failed to initialise) |
| 6 | storage locked | the eMMC holds the factory image (or unrecognised contents); saving would corrupt it — refused |
| 7 | write failed | |
| 8 | verify failed | written, but the read-back differed |
| 9 | unknown command | |
| 10 | invalid value | e.g. SET_TEXT with a line number other than 0 or 1 |

A SET_CONFIG that fails leaves the live configuration untouched.

### `46` STATUS

```
storage_state  dirty  program  sequence (4 x 7 bits, 28-bit)
```

`sequence` is the record's save counter; it increments on every successful save.

### `49` STATE

What the controls and the screen are doing now. Switches, encoders and analog inputs are in the
order of `midi_control_controls.h`: 20 switches, 7 encoders, 2 analog inputs.

```
storage_state  dirty  program
held       3 bytes   switch i is bit (i mod 7) of byte (i / 7); 1 = down
toggled    3 bytes   the same, for toggle actions; 1 = on
presses   20 bytes   each switch's press edges, counted modulo 128, whatever its action
enc_value  7 bytes   each encoder's absolute-mode value, 0..127
enc_turns  7 bytes   each encoder's detents, signed, counted modulo 128, whatever its mode
analog     2 bytes   the volume knob's and the pedal's last position, 0..127
line 1     length 0..32, then that many ASCII bytes (SET_TEXT line 0)
line 2     the same (line 1)
display    1 byte    1 = dark after the display timeout (docs/05 §2); protocol 3
profile    1 byte    the profile the live mapping came from (docs/05 §7); protocol 4
picking    1 byte    1 = the profile picker is on the screen; protocol 4
page       1 byte    the page of four the picker is showing; protocol 4
cal_stage  1 byte    0 off, 1 first end, 2 second end, 3 done, 4 cancelled or abandoned; protocol 5
cal_analog 1 byte    the control being calibrated: 1, the pedal; protocol 5
cal_hold   1 byte    how far through the current hold, 0..127; protocol 5
cal_raw    2 bytes   the calibrated control's latest raw count; protocol 5
cal_first  2 bytes   the first end taken, 0 until it is; protocol 5
cal_second 2 bytes   the second end taken, 0 until it is; protocol 5
pedal      1 byte    bit 0 its raw counts are read, so it can be calibrated; bit 1 the live record
                     holds a calibration for it; bit 2 its window is still the seed; bit 3 a travel
                     taken by CALIBRATE is not stored yet; protocol 5
pedal_raw  2 bytes   the pedal's latest raw count; protocol 5
pedal_lo   2 bytes   the low end of the window it is scaled from now; protocol 5
pedal_hi   2 bytes   the high end; protocol 5
offset     1 byte    how far Bank Up and Down have moved the Program Change buttons, 0..127
                     (docs/05 §3); protocol 6
```

A two-byte count is seven bits and seven bits, high first, clamped to 16383; the LPC4350's
converter is 10-bit, so nothing is lost. With both lines full the message is 138 bytes (derived;
checked in `tests/test_shared.c`). A protocol 2 device's STATE ends after line 2, a protocol 3
device's after the display byte, a protocol 4 device's after the page byte and a protocol 5
device's after `pedal_hi`; a reader takes each missing byte as 0, which reads as a lit screen
showing the first profile with no picker, nothing calibrating, no raw pedal to calibrate and the
Program Change buttons where they are set — what such a device has. A reader takes the protocol 5
bytes only when all sixteen are there, and the protocol 6 byte only after them. An older reader
stops where its own version ends. Either side can be the older one.

A device given this reply and the same configuration (`midi_control_device_mirror`) draws the
same screen and lights the same LEDs as the device that sent it; the tests compare the two views
byte for byte.

### `0A` EVENT

One control, used as though on the panel.

| `kind` | `id` | `value` |
|---|---|---|
| 0 switch | `midi_control_button`, 0–19 | 1 pressed, 0 released |
| 1 encoder | `midi_control_encoder`, 0–6 | detents, 7-bit two's complement: 1–63 clockwise, 127–64 for −1 to −64 |
| 2 analog | `midi_control_analog`, 0 volume, 1 pedal | the position, 0–127 |

Any other kind, an id past the end, or a switch value other than 0 or 1 is answered `invalid
value` and does nothing.

### `0B` CALIBRATE

Start (`start` 1) or cancel (`start` 0) calibrating an analog control by capture (docs/05 §6): the
user holds it still at one end, then at the other, and the device takes the two counts it settles
on as that control's travel. `analog` is `midi_control_analog`; only 1, the pedal, is accepted
today, and only by a device that reads its raw counts — the unit, or the app's emulator. Anything
else is answered `invalid value`. The byte is there so the volume knob can follow without a new
command.

The reply is immediate: the calibration runs on the device, and the host follows it in STATE
(`cal_*`). Starting while one runs starts over. Cancelling leaves the calibration exactly as it
was, and is `OK` when nothing is running.

## 3. Storage states

| | | |
|---|---|---|
| 0 | none | no backend |
| 1 | locked | backend present, holds the factory system image at sector 1 |
| 2 | ready | our image is installed; the slots may be written |
| 3 | unknown | backend present, sector 1 not recognised; treated as locked |

A locked device still **reads** its slots at boot and on RELOAD, and uses a valid record found there.
After a completed factory restore the slots hold the factory image's own bytes, which are not a
record (measured for v2.50), so the device starts from the defaults: a configuration saved before
the restore is gone. A record survives only a factory restore that did not reach both slots
(docs/06 §1, §4).

## 4. Semantics

- **SET_CONFIG applies immediately** (live), marks the device dirty, and keeps the previous
  sequence number. Toggle states and the current program are kept across a SET_CONFIG, so editing
  during a performance does not reset anything. It closes the profile picker if it is open, because
  the profiles under it are not the ones it was showing.
- **Choosing a profile** on the panel (docs/05 §7) is the same kind of change and leaves the device
  dirty the same way: which profile is live is part of the record. It is not a SysEx command; the
  host sees it in STATE's `profile` byte, and in `dirty`.
- **SAVE** writes the slot that does *not* hold the current newest record, with the sequence
  incremented, then reads it back. A failed save leaves the previous save and the live
  configuration intact. On success `dirty` clears.
- **LOAD_DEFAULTS** applies the factory record live (dirty); it does not save. It is the one
  command that clears the analog calibration (docs/05 §6).
- **RELOAD** discards live changes and re-reads the newest valid slot (or the defaults if none).
- **SET_TEXT** sets one of the two display lines; control characters become spaces. Not persisted.
- **Program Change** from the host (a normal channel message, not SysEx) moves the program when the
  global `follow_program_change` is on.
- **The display timeout** (global `display_timeout`, docs/05 §2) counts time since a control was
  last used; at the timeout the display goes dark and STATE's `display` byte is 1. A control used on
  the panel or by EVENT lights it and restarts the count, whatever the control is mapped to, and
  still does what it does — a switch, an encoder, the volume knob, the expression pedal. On the unit
  the two analog controls arrive as raw converter counts, so what lights the screen is a move they
  report: the position has to change *and* the count has to have travelled the 8-count analog
  hysteresis, except onto a stop, which reports at once. A pedal dithering where it was left
  therefore leaves the screen dark, and a deliberate move of about 2% of the travel lights it
  (derived from `midi_control_analog_learn_update`, with the pedal's seeded 280..664 window). A
  refused EVENT does nothing, so it lights nothing. SET_CONFIG, LOAD_DEFAULTS and RELOAD light it
  too, so a changed timeout is seen at once. Channel messages from the host, SET_TEXT, GET_STATE and
  SAVE do not; what they change is on the screen when it lights.
- **CALIBRATE** takes the pedal off the engine while it runs: its counts go to the capture, the
  pedal sends nothing, and the learned window does not move. The same thing starts when the toe
  switch is held for 2 s on the panel, and Mode cancels it; both switches still send what they are
  mapped to. The screen shows the steps in place of the program. When both ends are taken they are
  the pedal's travel at once, and the record is written in the same tick — unless the device is
  dirty, because that write would commit the user's unsaved edits for them, in which case it goes
  with their next SAVE (and a RELOAD first throws it away). The outcome stays on the screen and in
  `cal_stage` for 3 s. Nothing being held still for 30 s abandons it (`cal_stage` 4) with nothing
  changed. RELOAD and LOAD_DEFAULTS end one that is running. Timings from
  `midi_control_device.h`/`midi_control_analog.h`, exercised in `test_pedal_calibration*`.
- **GET_STATE** changes nothing. The two counters are for a host that asks at intervals: a switch
  tapped between two replies shows as a change in `presses`, a knob turned as a difference in
  `enc_turns`, taken modulo 128 as a step of −64 to 63. Both start from zero at power-on.
- **EVENT** is the control used on the panel, through the same engine call the board layer makes:
  it sends what the control sends, moves the program, flips a toggle, lights the LEDs. It is not a
  configuration change and leaves `dirty` alone. A switch pressed by EVENT stays down until an EVENT
  releases it or the switch is pressed and released on the panel, so a host that loses the link
  mid-press leaves it down. An analog position set by EVENT stands until the physical knob or pedal
  moves past the firmware's hysteresis (8 raw ADC counts, `analog_task` in `main.c`), and then the
  panel wins (derived from the scan code, not measured).

## 5. A session, as the app does it

```
-> IDENTIFY            <- IDENTITY     connected; learn storage state and dirty flag
-> GET_CONFIG          <- CONFIG       the editing copy
   (user edits; with live apply on, each edit is:)
-> SET_CONFIG          <- RESULT 03 0
-> GET_STATUS          <- STATUS       dirty = 1
   (Save on device:)
-> SAVE                <- RESULT 04 0  or 6 if locked
-> GET_STATUS          <- STATUS       dirty = 0, sequence + 1
   (while the Configure view shows, about 30 times a second:)
-> GET_STATE           <- STATE        the drawn unit
   (footswitch A clicked in the drawing:)
-> EVENT 00 00 01      <- 90 3C 7F     note 60 on, then RESULT 0A 0
-> GET_STATE           <- STATE        drawn down at once
   (Calibrate the pedal, then Start:)
-> CALIBRATE 01 01     <- RESULT 0B 0
-> GET_STATE           <- STATE        cal_stage 1, cal_hold rising as the pedal is held at an end
-> GET_STATE           <- STATE        cal_stage 2, cal_first taken
-> GET_STATE           <- STATE        cal_stage 3, both taken; pedal bit 3 clear once written
```

The app waits 2 s for each reply; a missing reply reports "no reply" and drops the rest of the
queue. A refused EVENT is reported and drops nothing. Both sides are exercised end to end in
`tests/test_shared.c` (`test_device_protocol`, `test_device_state_and_events`).
