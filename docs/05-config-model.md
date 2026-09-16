# 05 — Configuration model

One 280-byte record describes everything the unit does. It is the same bytes on the wire, in the
eMMC slots, in the emulator's files, and is what `midi_control_config_write()` / `midi_control_config_read()`
produce and consume. `MIDI_CONTROL_CONFIG_VERSION` is 1.

---

## 1. Byte layout (little-endian)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"PODG"` (`50 4F 44 47`) |
| 4 | 2 | version = 1 |
| 6 | 2 | length = 280 |
| 8 | 4 | sequence — save counter |
| 12 | 4 | CRC32 (zlib polynomial) of bytes 16..279 |
| 16 | 32 | global block (§2) |
| 48 | 20 × 8 | buttons, in `enum midi_control_button` order (§3) |
| 208 | 7 × 8 | encoders, in `enum midi_control_encoder` order (§4) |
| 264 | 2 × 8 | analog controls, in `enum midi_control_analog` order (§5) |

A reader rejects the wrong magic, version or length, and a CRC mismatch, each with its own
status (docs/04 §2). Every field is then clamped into range by `midi_control_config_sanitise()`, so an
out-of-range value from an older app can never reach the engine.

## 2. Global block

| Offset | Field | Range | Default | Meaning |
|---|---|---|---|---|
| 0 | `midi_channel` | 0–15 | 0 (channel 1) | the channel for controls set to "global" |
| 1 | `bank_size` | 1–16 | 4 | presets per bank: Bank Up/Down step, preset-in-bank base, display letters |
| 2 | `bank_select` | 0/1 | 1 | send CC#0 = 0 and CC#32 = 0 before every Program Change |
| 3 | `follow_program_change` | 0/1 | 1 | a Program Change from the host moves the display and LEDs |
| 4 | `startup_program` | 0–127 | 0 | program at power-on |
| 5 | `led_brightness` | 0–255 | 255 | reserved for PWM brightness; not used yet |
| 6 | `display_timeout` | 0–3600, u16 | 0 (never) | seconds with no control used before the display goes dark |
| 8 | `name[16]` | ASCII, NUL padded | `POGO` (`MIDI_CONTROL_PRODUCT_NAME`) | the display title; also returned in IDENTITY |
| 24 | reserved | | 0 | |

`display_timeout` took two bytes that were reserved, so a record written before it reads as 0,
never, and the version stayed 1 (§8). Once no control has been used for that long the display goes
dark; any control, on the panel or by EVENT and whatever it is mapped to, lights it again and still
does what it does. A new configuration lights it too. What each side does is in docs/04 §4 and
docs/01 §5.

## 3. Buttons — 8 bytes each

```
action channel number value_on value_off led_on led_off reserved
```

`channel` on every control: **0 = the global channel, 1..16 = that channel**.

| `action` | Name | `number` | `value_on` | `value_off` | Active (for the LED) |
|---|---|---|---|---|---|
| 0 | None | | | | never |
| 1 | Note momentary | note | velocity (0 → 1) | Note Off velocity | while held |
| 2 | Note toggle | note | velocity | Note Off velocity | while toggled on |
| 3 | CC momentary | CC | value at press | value at release | while held |
| 4 | CC toggle | CC | value when on | value when off | while toggled on |
| 5 | Program Change | program | | | while held |
| 6 | Program up | step (0 → 1) | | | while held |
| 7 | Program down | step | | | while held |
| 8 | Bank up | | | | while held |
| 9 | Bank down | | | | while held |
| 10 | Preset in bank | index 0..bank_size-1 | | | **when the current program is that preset** |

Programs wrap in 0..127. Preset in bank sets `program = (program / bank_size) * bank_size +
index`.

`led_on` / `led_off` are 3-bit colours — bit 0 red, 1 green, 2 blue — for the LED in the button's
matrix column, and only the seven SWROW2 buttons have one (Bank Up, Bank Down, A, B, C, D, Tap).
On the Tap column the three bits are three lamps: 1 Vol, 2 Wah, 4 **Tap**. For the other thirteen
buttons the fields are stored and ignored.

Button order (`enum midi_control_button`): A, B, C, D, Tap, Bank Up, Bank Down, View, Bypass, Menu, Mode,
Home push, Menu push, Enc 1–5 push, Toe switch, Page.

## 4. Encoders — 8 bytes each

```
mode channel number step value reserved[3]
```

| `mode` | Name | Sends per detent |
|---|---|---|
| 0 | None | nothing |
| 1 | CC relative, two's complement | CC `number` = `step` clockwise, `128 - step` anticlockwise (1 / 127 by default) |
| 2 | CC relative, offset | CC `number` = `64 + step` / `64 - step` |
| 3 | CC absolute | an accumulator 0..127 starting at `value`, ± `step`, sent when it changes |
| 4 | Program up / down | program ± `step` |

`step` is 1..64 (0 reads as 1). Order: encoders 1–5, Home, Menu.

## 5. Analog controls — 8 bytes each

```
mode channel number invert out_min out_max reserved[2]
```

| `mode` | Name | Sends |
|---|---|---|
| 0 | None | nothing |
| 1 | CC | CC `number` = position scaled onto `out_min..out_max` |
| 2 | Pitch bend | 14-bit, position scaled onto the range then × 129 |
| 3 | Channel pressure | position scaled onto the range |

The platform delivers a position 0..127 (the firmware auto-ranges the raw ADC and inverts the
pedal's falling voltage first); `invert` flips it; the result is scaled onto `out_min..out_max`
(swapped if reversed) and sent only when the output value changes. Order: volume knob, pedal.

## 6. Defaults

`midi_control_config_defaults()` is the mapping in docs/03 §2: notes 60–77 on the buttons, relative
CC#16–22 on the encoders, CC#7 and CC#11 on the knob and the pedal, banks of four with Bank Select
on, footswitch LEDs white while held and the Tap lamp for Tap.

## 7. JSON form

The app reads and writes the same record as JSON (`app/Source/ConfigJson.cpp`), with enumerations
as identifiers rather than numbers:

```json
{ "format": "midi-control-config", "version": 1,
  "global": { "name": "POGO", "midi_channel": 1, "bank_size": 4,
              "bank_select": true, "follow_program_change": true, "startup_program": 0,
              "display_timeout": 0 },
  "buttons": { "fs_a": { "action": "note_momentary", "channel": 0, "number": 60,
                         "value_on": 127, "value_off": 0, "led_on": 7, "led_off": 0 }, ... },
  "encoders": { "enc1": { "mode": "cc_relative_twos", "channel": 0, "number": 16, "step": 1, "value": 64 }, ... },
  "analogs":  { "pedal": { "mode": "cc", "channel": 0, "number": 11, "invert": false,
                           "out_min": 0, "out_max": 127 }, ... } }
```

`midi_channel` is 1–16 in JSON; per-control `channel` keeps 0 = global. `display_timeout` is in
seconds, 0 for never. Missing fields keep their
current values, so a partial file is a valid overlay.

## 8. Changing the format

Add fields to reserved bytes without changing the version if every existing reader can treat them
as zero. Anything else is a new version: bump `MIDI_CONTROL_CONFIG_VERSION`, teach `midi_control_config_read()`
to convert the old layout (or document that the unit falls back to defaults), add the round-trip
to `tests/test_shared.c`, and update this page.
