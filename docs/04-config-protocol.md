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
holding the seven high bits (bit *i* for byte *i*) followed by the seven low-7 bytes. 280 bytes pack
to 320.

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
| anything else | — | `43` RESULT with status `unknown command` |

`09` and `0A` are protocol version 2. A host checks `protocol_version` in IDENTITY before sending
them; a version 1 device answers them `unknown command`. Version 3 adds one byte to the end of
STATE, the display's state, and changes nothing else.

### `41` IDENTITY

```
protocol_version   3
fw_major fw_minor fw_patch
platform           0 hardware, 1 emulator
storage_state      see §3
dirty              1 if the live configuration differs from what was loaded or saved
config_version     1
config_bytes       hi7 lo7  (280)
name               up to 16 ASCII bytes, the configured device name
```

### `42` CONFIG

`len_hi7 len_lo7` (280) then the packed record. The record is docs/05, CRC included; the host
validates it the same way the device does. Sent in reply to GET_CONFIG; the same payload shape is
what SET_CONFIG carries.

### `43` RESULT

`command status`. `command` is the request being answered. Status codes:

| | | |
|---|---|---|
| 0 | OK | |
| 1 | bad length | record shorter than 280, or declared length ≠ unpacked length |
| 2 | bad magic | |
| 3 | bad version | a record from a different `MIDI_CONTROL_CONFIG_VERSION` |
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
```

With both lines full the message is 118 bytes (derived; checked in `tests/test_shared.c`). A
protocol 2 device's STATE ends after line 2, and a reader takes the missing byte as 0, lit; a
protocol 2 reader stops before the byte. Either side can be the older one.

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

## 3. Storage states

| | | |
|---|---|---|
| 0 | none | no backend |
| 1 | locked | backend present, holds the factory system image at sector 1 |
| 2 | ready | our image is installed; the slots may be written |
| 3 | unknown | backend present, sector 1 not recognised; treated as locked |

A locked device still **reads** its slots at boot: a configuration saved before a factory restore is
still used, and still shows as `saved`.

## 4. Semantics

- **SET_CONFIG applies immediately** (live), marks the device dirty, and keeps the previous
  sequence number. Toggle states and the current program are kept across a SET_CONFIG, so editing
  during a performance does not reset anything.
- **SAVE** writes the slot that does *not* hold the current newest record, with the sequence
  incremented, then reads it back. A failed save leaves the previous save and the live
  configuration intact. On success `dirty` clears.
- **LOAD_DEFAULTS** applies the factory record live (dirty); it does not save.
- **RELOAD** discards live changes and re-reads the newest valid slot (or the defaults if none).
- **SET_TEXT** sets one of the two display lines; control characters become spaces. Not persisted.
- **Program Change** from the host (a normal channel message, not SysEx) moves the program when the
  global `follow_program_change` is on.
- **The display timeout** (global `display_timeout`, docs/05 §2) counts time since a control was
  last used; at the timeout the display goes dark and STATE's `display` byte is 1. A control used on
  the panel or by EVENT lights it and restarts the count, whatever the control is mapped to, and
  still does what it does. A refused EVENT does nothing, so it lights nothing. SET_CONFIG,
  LOAD_DEFAULTS and RELOAD light it too, so a changed timeout is seen at once. Channel messages from
  the host, SET_TEXT, GET_STATE and SAVE do not; what they change is on the screen when it lights.
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
```

The app waits 2 s for each reply; a missing reply reports "no reply" and drops the rest of the
queue. A refused EVENT is reported and drops nothing. Both sides are exercised end to end in
`tests/test_shared.c` (`test_device_protocol`, `test_device_state_and_events`).
