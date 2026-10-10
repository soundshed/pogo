# 05 — Configuration model

One 5,120-byte record describes everything the unit does. It is the same bytes on the wire, in the
eMMC slots, in the emulator's files, and is what `midi_control_config_write()` / `midi_control_config_read()`
produce and consume. `MIDI_CONTROL_CONFIG_VERSION` is 5.

---

## 1. Byte layout (little-endian)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"PODG"` (`50 4F 44 47`) |
| 4 | 2 | version = 5 |
| 6 | 2 | length = 5120 |
| 8 | 4 | sequence — save counter |
| 12 | 4 | CRC32 (zlib polynomial) of bytes 16..5119 |
| 16 | 32 | global block (§2) |
| 48 | 20 × 8 | buttons, in `enum midi_control_button` order (§3) |
| 208 | 7 × 8 | encoders, in `enum midi_control_encoder` order (§4) |
| 264 | 2 × 8 | analog controls, in `enum midi_control_analog` order (§5) |
| 280 | 2 × 8 | analog calibration, in `enum midi_control_analog` order (§6) |
| 296 | 8 | profiles: count, active, then six reserved (§7) |
| 304 | 8 × 256 | the eight profiles (§7) |
| 2352 | 128 × 16 | program names, in program order (§8) |
| 4400 | 20 × 4 | hold actions of the live mapping, in `enum midi_control_button` order (§3) |
| 4480 | 8 × 80 | each profile's hold actions, profile 1's first (§3, §7) |

Each profile entry is 256 bytes: a 16-byte name, eight reserved, then the same button, encoder and
analog blocks as §3–§5 at offsets 24, 184 and 240 within the entry. That fills the entry, so a
profile's hold actions are in a block of their own after the program names, written from the live
mapping for the active profile as its buttons are.

A reader rejects the wrong magic, version or length, and a CRC mismatch, each with its own
status (docs/04 §2). Every field is then clamped into range by `midi_control_config_sanitise()`, so an
out-of-range value from an older app can never reach the engine.

**Versions 1 to 4** are still read. Version 4 is the same record without the hold actions, 4,400
bytes with a CRC over 16..4399: every switch comes back with no hold, acting at its press as it did,
and the hold time is the default. Version 3 is version 4 without the program names, 2,352
bytes with a CRC over 16..2351, and every program comes back unnamed (§8). Version 2 is version 3
without the profile block, 296 bytes with a CRC over 16..295; version 1 is version 2 without the
calibration block, 280 bytes with a CRC over 16..279. The mapping either of those carries becomes
the unit's first profile (§7), and a version 1 record's calibration comes back empty so the unit
learns its own travel again. The next save writes version 5. Nothing writes an old version, so a
unit downgraded to firmware from before a block rejects the record it finds (`BAD_VERSION`) and
starts from the defaults — reload the mapping from the app after a downgrade.

A slot is read whole, so an older record arrives followed by whatever the sectors after it hold.
The reader takes the length from the version and ignores the rest (`test_program_names`,
`test_hold_actions`).

## 2. Global block

| Offset | Field | Range | Default | Meaning |
|---|---|---|---|---|
| 0 | `midi_channel` | 0–15 | 0 (channel 1) | the channel for controls set to "global" |
| 1 | `bank_size` | 1–16 | 4 | presets per bank: Bank Up/Down step, preset-in-bank base, display letters |
| 2 | `bank_select` | 0/1 | 1 | send CC#0 = 0 and CC#32 = 0 before every Program Change |
| 3 | `follow_program_change` | 0/1 | 1 | follow the host: its Program Changes move the program, its CCs and notes set the toggles and absolute encoders that send them (below) |
| 4 | `startup_program` | 0–127 | 0 | program at power-on |
| 5 | `led_brightness` | 0–255 | 255 | reserved for PWM brightness; not used yet |
| 6 | `display_timeout` | 0–3600, u16 | 0 (never) | seconds with no control used before the display goes dark |
| 8 | `name[16]` | ASCII, NUL padded | `POGO` (`MIDI_CONTROL_PRODUCT_NAME`) | the display title; also returned in IDENTITY |
| 24 | `bank_wait` | 0/1 | 1 | Bank Up / Down choose a bank and send nothing; a preset in it sends when chosen (§3) |
| 25 | `hold_time` | 20–250, in 10 ms | 50 (500 ms) | how long a switch with a hold action is held before the hold is what it does (§3); 0 reads as 50 |
| 26 | reserved | | 0 | |

`display_timeout` took two bytes that were reserved, so a record written before it reads as 0,
never, and the version stayed 1 (§11). Once no control has been used for that long the display goes
dark; any control, on the panel or by EVENT and whatever it is mapped to, lights it again and still
does what it does. A new configuration lights it too. What each side does is in docs/04 §4 and
docs/01 §5.

`bank_wait` took a byte that was reserved, so a record written before it reads as 0 — banks that
send at once, as they did — and the version stayed 4 (§11). The defaults have it on. Firmware from
before it clears the byte. `hold_time` took the next one, read as the default when it is 0, and
came with version 5.

**Following the host.** With `follow_program_change` on, the host's channel messages keep the
switches showing what the host has:

- A **Program Change** moves the program — the screen, the preset LEDs, a waiting bank ended (§3) —
  when it arrives on the global channel, or on a channel a control that moves the program is set to
  (Program Change, Program up / down, Bank up / down, Preset in bank, a Program encoder). One on any
  other channel is for another device, and is ignored.
- A **CC** sets every *CC toggle* that sends that CC on that channel: on when the value is nearer
  its `value_on` than its `value_off`, a tie on. A *CC absolute* encoder that sends it carries on
  from the value.
- A **Note On** with a velocity above 0 turns on, and a **Note Off** or a velocity-0 Note On turns
  off, every *Note toggle* that sends that note on that channel.

So after a host changes preset and sends the controller what the new one has on — a plugin's MIDI
feedback, or a DAW's echo — a toggle's LED shows it and its next press turns it the other way,
instead of sending what the host already has. Nothing is sent back. `test_engine_follow_host`.

## 3. Buttons — 8 bytes each

```
action channel number value_on value_off led_on led_off flags
```

`channel` on every control: **0 = the global channel, 1..16 = that channel**.

| `action` | Name | `number` | `value_on` | `value_off` | `flags` | Active (for the LED) |
|---|---|---|---|---|---|---|
| 0 | None | | | | | never |
| 1 | Note momentary | note | velocity (0 → 1) | Note Off velocity | | while held |
| 2 | Note toggle | note | velocity | Note Off velocity | | while toggled on |
| 3 | CC momentary | CC | value at press | value at release | | while held |
| 4 | CC toggle | CC | value when on | value when off | | while toggled on |
| 5 | Program Change | program, moved by the banks below | | | | **when the current program is the one it sends** |
| 6 | Program up | step (0 → 1) | | | | while held |
| 7 | Program down | step | | | | while held |
| 8 | Bank up | | | | bit 0: move the Program Changes | while held |
| 9 | Bank down | | | | bit 0: move the Program Changes | while held |
| 10 | Preset in bank | index 0..bank_size-1 | | | | **when the current program is that preset** |
| 11 | Tap tempo | CC | value sent at each press | | | while held; **with a tempo, flashing it** |

**Banks.** There are ⌈128 / `bank_size`⌉ banks, bank *b* starting at program *b* × `bank_size`.
When the size does not divide 128 the last bank is short: with three to a bank there are 43, and
the 43rd holds 126 and 127 alone. Banks wrap as a whole — Bank Up from the last bank is the first,
Bank Down from the first is the last — so a bank always starts on a multiple of the size. Program
up / down and a *Program* encoder step programs, not banks, and wrap in 0..127.

- **With `bank_wait`** (§2, the default) Bank Up and Down choose a bank and send nothing. Preset in
  bank chooses from the bank chosen, and its press sends the Program Change and ends the wait, so
  stepping past three banks loads one preset, not three. This is how the POD Go's own switches
  behave. While a bank waits, the preset-in-bank switches flash their `led_on` colour, 250 ms lit
  and 250 ms not (`MIDI_CONTROL_BANK_BLINK_MS`), from lit at each bank chosen; the screen shows the
  bank's number with no letter, `Preset -` and *CHOOSE A PRESET* on its second line, and the strips
  name the presets A–D will load. The program, its name and its PC number stay as they are, since
  that is what is still playing. Coming back to the program's own bank ends the wait, and so does
  any other change of program: a Program Change button, Program up / down, a Program encoder, a
  Program Change from the host. So does a new configuration with `bank_wait` off, or with a bank
  size that leaves the waiting bank past the last. STATE carries the waiting bank from protocol 7
  (docs/04), so a mirror draws it.
- **Without it**, Bank Up and Down move the program to the same place in the next or previous bank
  and send it at once. Into a short last bank, a place past its end lands on its last program.
- **Preset in bank** sets `program = bank × bank_size + index`, the bank being the one waiting or
  the program's own. A place past the end of a short last bank has no program, and a press there
  sends nothing. Before 0.3.0 it wrapped onto program 0, a jump from the last bank to the first,
  and a Bank Up from the short last bank carried on into the middle of the first.

`test_engine_bank_wait` covers both modes, the short bank, the flashing, the screen and STATE.

**Tap tempo.** Each press sends CC `number` = `value_on` — at the press only, since a receiver
counting messages would take a release for a second tap — and is a tap of the unit's tempo. The
first tap of a run only starts it; each after it is a beat, and the tempo is the mean of the last
four beats (`MIDI_CONTROL_TAP_AVERAGE`). A beat off that mean by more than half is a new tempo
being tapped and starts the mean again; one under 200 ms is a bounce and counts for nothing; a tap
more than 2 s after the last starts a new run, and the tempo stands until its second tap. Tempos
run from 30 to 300 BPM (2,000 to 200 ms a beat). Every Tap tempo switch's LED then flashes
`led_on` for 100 ms at each beat (half a beat if that is less) and shows `led_off` between, and
its strip on the screen reads `TAP 120`. The time is the platform's tick (`midi_control_device_tick`),
so on the unit a tap is placed to within one panel scan, about 4 ms (derived from the main loop),
which the mean of four beats makes small against a beat.

- **The host's MIDI clock**, with `follow_program_change` on, sets the tempo instead: every 24th
  clock is a beat, Start (`FA`) makes the next clock the first, and the tempo is measured over the
  last four beats kept. While clocks keep coming, no more than 2 s apart, taps still send their CC
  but do not move the tempo, because the host's is the one that plays; once they stop, the next
  taps do. Clocks are taken anywhere in the stream, inside a SysEx or between a status and its
  data, as MIDI allows. Nothing is sent back: the unit does not send clock.
- STATE carries the tempo from protocol 8 (docs/04). `test_tap_tempo`.

**Hold actions.** Any switch but Mode and the toe switch — whose holds are the profile picker (§7)
and calibration (§6) — can have a second action for when it is held: 4 bytes a switch,

```
action number value_on value_off
```

any button action, on the switch's own channel, with no flags and no LEDs of its own. Action 0 is
no hold. A switch without one acts at the press exactly as before. A switch with one is timed by
the device (`midi_control_device_button`, `hold_tick`):

- Let go before the global `hold_time` (§2, 500 ms by default), and it is a **short press**: the
  press action, its press and its release together, at the release. That moves the press action
  later by however long the switch was down, which is the price of the hold; a switch that needs
  to act at the press has no hold.
- Held to the `hold_time`, and the **hold action** engages then, and the press action never runs.
  It is released with the switch, so a momentary hold lasts as long as the foot does. A hold's
  toggle has a state of its own, apart from the press action's.
- **Tap tempo is never kept waiting**: its tap goes at the press, and a hold adds the hold action
  on top. A tap late by the hold time would be no use as a tap. So the factory's Tap is tap tempo
  at the press and the tuner when held, as on Line 6 units.
- A new mapping finishes an engaged hold under the old mapping, as it does a held press (below),
  and a press still being timed is dropped with its release, since what it would have done
  belongs to the mapping being left.

The LED shows the press action's state, whatever the hold is doing; a press counts once in STATE's
`presses`, short or long. `test_hold_actions`.

**Program Change buttons stay lit on their program**, as preset-in-bank switches do: the LED shows
`led_on` while the program is the one the button sends now, however the program got there. Before
0.3.0 they were lit only while held.

**A new mapping finishes what the old one started.** `SET_CONFIG`, `LOAD_DEFAULTS`, `RELOAD` and a
profile chosen (§7) all replace the mapping. For each switch whose message changes — its action,
its channel once *global* is resolved, or its number — what the old mapping started is finished
under the old mapping: a switch held is released, and its physical release then sends nothing; a
Note toggle that is on sends its Note Off, rather than leaving the note sounding until the host is
reset. That switch's toggle starts again from off, since the state belonged to a message it no
longer sends. A switch whose message is unchanged keeps its toggle and sends nothing, so
recolouring one mid-song changes nothing. `test_device_remap`.

**Bank up and Bank down can move the Program Change buttons.** With bit 0 of `flags` set
(`MIDI_CONTROL_BUTTON_MOVES_PROGRAMS`), each press moves every Program Change button a bank the
same way as the program: a Program Change button sends `(number + offset) mod 128`, where the
offset gains `bank_size` at each such Bank up and loses it at each such Bank down, wrapping by
whole banks as the banks do. The bank button still does what it does without the flag: with
`bank_wait` it chooses the bank and sends nothing, and the Program Change buttons send from where
they have moved when pressed; without it, it sends the program a bank away. With banks of four, a button set to program 0 sends 4 after one Bank up and 0
again after a Bank down; set to 10 it sends 14, press after press, which *preset in bank* cannot
do, since it only chooses within the bank the program is in.

- **The offset is the engine's, not the record's.** It is 0 at power-on. A Program Change from the
  host, Program up / down and a *Program* encoder move the program alone. A new configuration
  keeps the offset, so a live edit does not move the buttons under a player's feet — unless no Bank
  up or Bank down in it has the flag, when it goes back to 0, because nothing is left to move it
  back. STATE carries it from protocol 6 (docs/04), so a mirror draws the buttons where the unit
  has them, and the screen's strip for a Program Change button names the preset it sends now.
- **Each bank button has its own flag.** Set on Bank up alone, the buttons only ever move up. Any
  button mapped to Bank up or Bank down can have it, not only the two bank footswitches.
- **`flags` was reserved**, so it is zero in every record written before it, which is no flags,
  and the version stayed 4 (§11). Firmware from before it clears the byte, and its Bank up and Bank
  down step the program alone. The flag has a byte of its own rather than the bank buttons' unused
  `number` because the app leaves `number` holding whatever an earlier action put there, which
  would have turned it on in records nobody meant it for. `midi_control_config_sanitise()` clears
  every other bit, and the whole byte on every other action.

`test_engine_bank_moves_programs` covers the stepping, the wrap and the reset; the STATE test
covers the mirror.

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
| 5 | CC relative, sign and magnitude | CC `number` = `step` clockwise, `64 + step` anticlockwise (1 / 65 by default) |

`step` is 1..64 (0 reads as 1). The three relative modes send at most 63 per detent: seven-bit two's
complement has no +64, `64 + 64` is not a data byte, and sign and magnitude has six bits of
magnitude. A `step` of 64 sends 63 / 65 in mode 1, 127 / 1 in mode 2 and 63 / 127 in mode 5. Mode 5
is the "signed bit" encoding some hosts offer beside the other two; it is new in 0.3.0, needed no
new version since the byte was always there, and firmware from before it reads it as None. Order:
encoders 1–5, Home, Menu.

## 5. Analog controls — 8 bytes each

```
mode channel number invert out_min out_max pad_lo pad_hi
```

| `mode` | Name | Sends |
|---|---|---|
| 0 | None | nothing |
| 1 | CC | CC `number` = position scaled onto `out_min..out_max` |
| 2 | Pitch bend | 14-bit: the scaled value 0..64 onto 0..8192 and 64..127 onto 8192..16383 |
| 3 | Channel pressure | position scaled onto the range |

The platform delivers a position 0..127 (the firmware auto-ranges the raw ADC and inverts the
pedal's falling voltage first, §6); the padding takes in its ends; `invert` flips it; the result is
scaled onto `out_min..out_max` (swapped if reversed) and sent only when the output value changes,
or where it goes does. Order: volume knob, pedal.

- **Pitch bend's centre is exact.** 64 is 8192, no bend, and both ends reach 0 and 16383. Before
  0.3.0 the value was multiplied by 129, which reaches both ends but puts 64 at 8256: an up-only
  bend (`out_min` 64) started 64 steps sharp, about 9 cents at a 12-semitone bend range (derived).
- **A new mapping is heard at once.** When a configuration or a profile changes what a control's
  position sends, or where — another CC, another channel, other padding — the engine sends the
  position under the new mapping straight away, rather than leaving the host with the old one
  until the control moves. A change that leaves the message alone sends nothing.
- **The host hears where they are when it arrives.** The firmware reads the controls long before
  USB is up, and what it sent then went nowhere; when USB comes up it sends each control's position
  again (`midi_control_device_host_connected`). Without that, the host did not know where the pedal
  was until it moved.

**Padding.** `pad_lo` and `pad_hi` are 0–40 (`MIDI_CONTROL_ANALOG_PAD_MAX`): the percentage of
the travel at each end that reads as that end. The defaults (§9) pad the pedal 1 at the heel and
15 at the toe, and leave the knob at 0. A position within `pad_lo` % of the
heel (fully anticlockwise, for the knob) is 0, one within `pad_hi` % of the toe (clockwise) is 127,
and the travel between is stretched back over 0..127. It is for effects that need the end value
exactly — a pitch shift at 0 or 127 — where a pedal settling a few counts off its stop as the foot
comes away would otherwise move the value. How far a pedal settles has not been measured;
`test_analog_padding` uses 12 counts of a 300..620 travel, 3.75 %, which 5 % takes in at either end.

- The ends are the physical ones, taken **before** `invert`, so the heel's padding stays at the heel
  whichever output it sends.
- It is applied in the engine (`midi_control_engine_analog`), so it acts on a position set by
  EVENT or by the emulator the same as on the pedal itself. The calibration (§6) is untouched: the
  learner still reports the pedal's true position, which is what the screen's meter and STATE show.
- **The position is not the value.** With 10 % at the toe, a pedal settled to position 114 sends
  127. So anything that shows a number as what the pedal sends computes it from the position with
  `midi_control_engine_analog_value()`, the function the engine sends from: the app's pedal caption
  (`CC 11 = 127`) and its calibration view (`at 114 of 127, sending 127`). Showing the position
  there would make working padding look like a fault. The travel track beside the drawn pedal
  shows the position, and each end's padding on it as a sleeve with a line where it starts — toe
  at the top, heel at the bottom, whatever `invert` is — lit while the pedal is inside it.
- The stretch has the same 127 steps to make over less travel, so a sweep through the middle skips
  a value now and then — derived: at 10 % each end, about one value in five.
- The two bytes were reserved, so padding needed no version of its own (§11), and a record written
  before it — version 3 or older — reads as no padding, not the defaults' 1 and 15: a unit that
  has saved plays as it did, and has padding once it is set or the defaults are loaded. Firmware
  older than version 4 records does not read them at all (§1), so it never sees the padding.
- The toe's default is wide because the settle it is for happens at the toe, and 127 is the value a
  pitch shift at full travel needs to hold; the heel's percent only spares 0 the last count. With
  them, the pedal at half travel (64) sends 75, and a sweep skips about one value in six (derived).

A profile carries its own padding, like the rest of the mapping (§7).

## 6. Analog calibration — 8 bytes each

```
raw_lo(u16) raw_hi(u16) flags reserved[3]
```

| Field | Meaning |
|---|---|
| `raw_lo`, `raw_hi` | the raw converter counts at the two ends of **this unit's** travel |
| `flags` | bit 0 `MIDI_CONTROL_CAL_VALID`: the unit has learned a travel. Everything zero means it has not |

This is measured, not configured. A pedal's raw travel differs between units, so a window baked
into a build is right for at most one of them, and the fault it causes — the ends of the sweep
never arriving — looks like a pedal problem rather than a firmware one (docs/09, "In the code").

`shared/src/midi_control_analog.c` learns it from use, and the rules are its whole point:

- **Widening** takes a sample outside the window, once `MIDI_CONTROL_CAL_CONFIRM` (3) samples agree
  on it. A sweep agrees with itself and a control held against its stop agrees with itself; a lone
  sample thrown out by a transient does not. While an edge is unconfirmed the position reads as the
  stop, which is where the control appears to be, so waiting costs nothing.
- **Narrowing** takes the ends a sweep actually reached, once that sweep has covered three quarters
  of the window (`MIDI_CONTROL_CAL_SWEEP_NUM/DEN`). This is the half a window that can only widen
  never had, and it is what brings a seed, another unit's calibration, or a window a transient left
  too wide back onto this unit's travel. Erring low is the cheaper mistake: a window left too narrow
  widens again the instant the control goes past it, and the position tracks through the widening.
- **Narrowing on a repeated end** is the second way in, and it asks nothing of the window. Coverage
  is measured against the window, which is the thing in doubt, so on its own it deadlocks: a travel
  shorter than three quarters of the window it started from can never cover enough of that window
  for its ends to be believed. Against the 280..664 pedal seed, a unit reaching only 470 at the toe
  stop reads 63 of 127 there — half travel — however many times it is swept. So an end that is
  reached, left across the middle of what has been covered, and reached again to within
  `MIDI_CONTROL_CAL_END_AGREE` (8 counts) is taken as mechanical: a turn made in free travel does
  not land on the same count twice. One return at each end is enough, which a sweep down, back and
  down again supplies. It applies only to a window that started from the seeds, and only until a
  sweep has confirmed one — a stored travel is this unit's own, and a session that rocks a control
  over part of it without ever reaching a stop must not adopt the part as the whole.
- **Storing** happens only after a sweep has confirmed both ends, and only when they differ from
  what is stored by `MIDI_CONTROL_CAL_STORE_MARGIN` (8 counts) or more, in either direction. So a
  power-on transient never becomes a calibration, and an unchanged travel is never written again.
- The write itself is `midi_control_device_tick()`'s: `MIDI_CONTROL_CAL_SETTLE_MS` (10 s) after the
  window last moved, so the card is written when the sweep is over rather than during it. It is
  deferred while the live configuration is dirty — **a save nobody asked for never commits edits
  nobody committed** — and the user's own next SAVE carries the calibration with it.

Until a unit has been swept, the platform's seeds stand in: `firmware/src/main.c` holds them, the
travel measured on the verified unit (docs/01 §4). They are a starting point and nothing more.

`SET_CONFIG`, and a record loaded from a file, **never** change the calibration — the pedal it
describes is the one in front of you, not the one the file came from. `LOAD_DEFAULTS` is the one
command that clears it (`midi_control_device_analog_recalibrate()`), after which the next sweep
learns it again. `midi_control_config_equal()` ignores the block for the same reason: two units with
the same mapping and different pedals are configured the same.

**Calibration by capture** is the other way to set it, and the way out when the rules above
cannot help: a stored window wider than the pedal reaches, which the repeated-end rule declines to
narrow because a stored travel outranks a session (docs/09). The user holds the pedal still at one
end, then at the other; the device takes each end as the count it settles on, and those two counts
are the calibration. `CALIBRATE` (docs/04) starts it, and so does the toe switch held for 2 s
(`MIDI_CONTROL_CAL_ENTER_HOLD_MS`) — longer than Mode's 0.8 s for the profile picker, because a toe
switch mapped to a momentary action is held on purpose. Mode, or `CALIBRATE` with `start` 0,
cancels it. `shared/src/midi_control_analog.c` takes the ends; `midi_control_device.c` runs the rest.

- **An end is taken** once the count has stayed within `MIDI_CONTROL_CAL_STILL` (6) counts of where
  it came to rest for `MIDI_CONTROL_CAL_HOLD_MS` (1 s). The count taken is the middle of what it
  dithered over in the second half of the hold, so the last counts of the approach — already inside
  the rest — do not put the stop short of itself (found in the host tests: a stop at 977 approached
  in 3-count steps was taken as 975 before this).
- **Either end may come first.** The second must be at least `MIDI_CONTROL_CAL_MIN_SPAN` (32) counts
  from the first, so resting at the same end again is not the other end. What is stored is a low
  count and a high one; which of them is the toe is the platform's `invert`.
- **While the toe switch is held nothing is timed**, and the rest is measured afresh from its
  release: a foot on the switch presses the pedal past where it rests, and a stop taken there would
  be a window wider than the pedal reaches unpressed. Whether the switch's travel moves the count
  at all has not been measured; this is a precaution.
- **While it runs, the pedal is measured, not played.** It sends nothing, and its learner is not fed.
- **When both are taken** they are the pedal's travel at once: the learner restarts from them as it
  does from a stored calibration at power-on, and the record is written in the same tick — unless
  the device is dirty, because a write the user did not ask for must not commit edits they have not
  committed. Then it goes with their next SAVE, and a RELOAD before that throws it away.
- **A capture that takes nothing for 30 s** (`MIDI_CONTROL_CAL_IDLE_MS`) is abandoned, and the
  calibration is exactly what it was. So is one cancelled.
- A stop taken short of the real one — a pause mid-sweep read as an end, say, since a pedal stays
  where it is left — is the cheap mistake: the window widens past it the first time the pedal goes
  further, under the widening rule above.

Exercised in `test_analog_capture` and `test_pedal_calibration*` with the unit's numbers (a stored
249..977 against a pedal reaching 584..977).

The firmware reports both windows in its ADC snapshot (CC#95): `c1lo`/`c1hi` in use now,
`c1sl`/`c1sh` stored, `c1sw` whether a sweep has confirmed them, `c1sd` whether the window is still
the seed. A pedal that cannot reach 0 or 127 shows it there — sweep it stop to stop and compare the
two. From protocol 5, STATE carries the pedal's raw count and window as well (docs/04), which is
what the app's calibration view draws (docs/08).

## 7. Profiles

A **profile** is a whole named mapping — every button, encoder and analog control — that the unit
switches between from the panel. Eight entries are always present in the record; `profile_count`
(1–8) says how many are in use and `profile_active` (0–`profile_count`−1) which one is live.

Note the two senses of the word *preset* in this project: on the POD Go a preset is a program
within a bank, which is what `MIDI_CONTROL_ACT_PRESET_IN_BANK` and the screen's "Bank 12 / Preset A"
mean. A profile is the other thing — the controller's own mapping.

**A profile is the mapping; the global block is the unit.** The MIDI channel, the bank size, the
screen name, the display timeout and the learned calibration (§6) stay global and do not change
when a profile does: they describe the unit and the pedal wired to it, not the rig. A profile that
needs its own MIDI channel sets one per control (§3, `channel` 1–16).

### The live mapping, and why it is stored twice

Bytes 48–279, with the hold actions at 4400–4479, are the **live mapping**: what the engine plays, what the app's inspector edits, and
what `SET_CONFIG` carries. They are always a copy of `profiles[profile_active]`, and the copying
goes one way only — `midi_control_config_sanitise()` writes the live mapping back over the active
profile, every time it runs, and `midi_control_config_write()` serialises the active profile from
the live blocks whatever the struct says. So:

- an edit to the live mapping *is* an edit to the active profile, with no call site having to
  remember it;
- the two copies in a record can never disagree, and if a damaged or hand-written record has them
  disagree, the live one wins;
- a reader that knows only the mapping still finds it where it has always been.

`midi_control_config_select_profile()` is what choosing a profile means: it keeps whatever has been
done to the profile being left, then loads the new one into the live blocks.
`midi_control_config_load_profile()` is the same without keeping the old one, for a record assembled
from a file where the profile list is the truth and the live blocks hold nothing yet.

### Choosing one on the unit

Holding **Mode** for `MIDI_CONTROL_PROFILE_HOLD_MS` (800 ms) opens the picker. While it is open:

| Control | Does |
|---|---|
| A, B, C, D | choose the four profiles on the page; a place past the last profile in use does nothing |
| Bank Up / Bank Down | page through, wrapping; pages are four profiles each, so eight profiles are two pages |
| Mode | closes it, changing nothing |
| anything else | what it is mapped to, as usual |

Those six footswitches send no MIDI while the picker is open, and their LEDs show it instead: A–D
white for the live profile, blue for another, dark for a place with no profile; Bank Up and Bank
Down green when there is more than one page. The picker closes itself after
`MIDI_CONTROL_PROFILE_IDLE_MS` (5 s) with nothing pressed, so a unit is never left on a stage with
its footswitches answering a menu. A finger still on Mode is not "nothing pressed": the count does
not run while Mode is held, and starts from zero when it is let go, so however long the hold lasts
there are five seconds to choose in afterwards. It also closes whenever the configuration is replaced —
`SET_CONFIG`, `LOAD_DEFAULTS`, `RELOAD` — because the profiles under it are then not the ones it was
showing.

**The hold is a gesture on top of the mapping, not instead of it.** Mode still sends whatever it is
mapped to, at the press, so nothing about Mode's latency or its MIDI changes. Two consequences
follow, and both are deliberate:

- Mode is still *down* when a profile is chosen, so its release would otherwise be sent under the
  new mapping and the old mapping's note would never be turned off. Every switch still held is
  therefore released under the profile it was pressed in, at the moment the profile changes, and its
  physical release then sends nothing. A switch held across a profile change counts as up until it
  is pressed again.
- One hold does one thing. Opening the picker consumes the hold, so continuing to hold Mode does not
  close it again, and closing it with a press of Mode does not reopen it on that same press.

**Choosing a profile leaves the device dirty**, exactly as `SET_CONFIG` does: which profile is live
is part of the record, so the unit powers up in whichever profile was last saved, and saving is how
a player says which that is. A dirty configuration also defers the calibration write of docs/06 §1
until they do — a save nobody asked for still never commits edits nobody committed.

### On the screen

With more than one profile in use, the status line carries the live profile's name in a chip. While
the picker is open it takes the program region: the page (`2/2`) where the program's letter usually
is, the live profile's name beside it, and the four switches labelled with the profiles they
choose. With one profile in use, nothing about profiles appears — a unit set up before they existed
looks exactly as it did.

### Upgrading a unit that has a configuration already

A record written before profiles existed (version 1 or 2, §1) carries one mapping and no profiles.
On the first read after the upgrade `midi_control_config_read()` turns it into a profile set: the
mapping becomes the one profile in use, and profiles 2–8 are the factory mapping under their own
names, waiting to be edited. The global block is kept as it was.

The one profile is named **IMPORTED** when that mapping is not simply the factory one, and
**PROFILE 1** when it is: a unit whose owner had set nothing up has nothing to say it imported. The
comparison is of the mapping alone, so changing only a global setting — the bank size, the display
timeout — is not an import. Padding (§5) is left out of it too: those records had none to carry,
so the imported mapping keeps none, and only profiles 2–8, the factory's, have the default
padding. The switches are compared with the notes they sent in the factory mapping of the time —
A–D 60–63, Tap 64 and the rest 65–77 — not with today's presets, tap tempo and CCs (docs/03 §2).
So a unit nobody set up gets PROFILE 1 and keeps its notes, and only profiles 2–8 have today's
mapping. Nothing is written by the upgrade; it is a read, and the profile set
reaches the card at the next save like any other change.

## 8. Program names

Bytes 2352–4399 hold a name for each program the unit can select, 0–127, sixteen bytes apiece in
program order: printable ASCII, NUL padded, at most 15 characters (`MIDI_CONTROL_PROGRAM_NAME_LEN`
is 16). All zero is no name. No program has one in the defaults.

While a program is the current one, its name is the title of the screen's program region: the large
line beside the box with the bank and preset, where the host's first `SET_TEXT` line otherwise
goes. With banks of four, program 127 named `Fuzz Lead` puts `Fuzz Lead` beside `32D`, however the
program was reached — a footswitch, an encoder, Bank Up or Down, or a Program Change from the host.

- **The name goes over the host's first line.** That line belongs to no program: it stays until the
  host replaces it, so after one `SET_TEXT` it would hide every name. The host's second line still
  shows under the name. A program with no name shows the host's first line, or without one
  *Bank n* / *Preset x*, as before.
- **Keyed by program, not by bank and letter.** The letters come from `bank_size` (§2): program 127
  is `32D` in banks of four and `16H` in banks of eight, and its name stays with it either way,
  because the number is what the receiving device knows the preset by.
- **Global, not per profile.** A name belongs to a preset on whatever receives the Program Changes,
  and that preset is the same whichever profile sends them, as the bank size that labels it is
  (§7). Eight copies would also need 16 KB of SRAM that the firmware does not have (§11).
- **Sanitising** turns a character outside printable ASCII into `?` and clears the bytes after the
  NUL, so equal names make equal records. An empty name stays empty. A profile's is filled in.
- `midi_control_config_program_name()` reads one; `midi_control_config_set_program_name()` writes
  one, cut to 15 bytes. In the app they are edited in the *Preset names* card of the global
  settings (docs/08).

The block is new in version 4. A version 3 record is read with every program unnamed (§1).
`test_program_names` covers the layout, the upgrade, SET_CONFIG, a power cycle and the screen.

## 9. Defaults

`midi_control_config_defaults()` is the mapping in docs/03 §2: footswitches A–D choosing the four
presets of the bank (*preset in bank* 0–3, §3), Tap as tap tempo on CC 64 and, held, the tuner as a
CC 68 toggle, CCs 102–114 on the other switches — momentary, 127 and 0, but for the toe switch's
toggle — a 500 ms hold time, relative
CC#16–22 on the encoders, CC#7 and CC#11 on the knob and the pedal, the pedal padded 1 % at the
heel and 15 % at the toe (§5), banks of four with Bank Select on and Bank Up / Down waiting for
A–D (§2), A–D's LEDs white on the preset in use and Bank Up / Down's while held, neither moving the
Program Change buttons (§3), and the Tap
lamp for Tap. The calibration block is empty in
the defaults: a unit has not measured its own pedal until it has. One profile is in use, named
PROFILE 1 and holding that mapping; the other seven hold it too, under the names PROFILE 2 to
PROFILE 8 (§7). No program has a name (§8).

## 10. JSON form

The app reads and writes the same record as JSON (`app/Source/ConfigJson.cpp`), with enumerations
as identifiers rather than numbers:

```json
{ "format": "midi-control-config", "version": 4,
  "global": { "name": "POGO", "midi_channel": 1, "bank_size": 4,
              "bank_select": true, "follow_program_change": true, "startup_program": 0,
              "display_timeout": 0, "bank_wait": true },
  "buttons": { "fs_a": { "action": "note_momentary", "channel": 0, "number": 60,
                         "value_on": 127, "value_off": 0, "led_on": 7, "led_off": 0 }, ... },
  "encoders": { "enc1": { "mode": "cc_relative_twos", "channel": 0, "number": 16, "step": 1, "value": 64 }, ... },
  "analogs":  { "pedal": { "mode": "cc", "channel": 0, "number": 11, "invert": false,
                           "out_min": 0, "out_max": 127, "pad_lo": 0, "pad_hi": 5,
                           "calibration": { "raw_lo": 300, "raw_hi": 620 } }, ... },
  "profiles": { "active": 0, "in_use": 3,
                "list": [ { "name": "CLEAN", "buttons": { ... }, "encoders": { ... },
                            "analogs": { ... } }, ... ] },
  "program_names": [ { "program": 0, "name": "Clean" },
                     { "program": 127, "name": "Fuzz Lead" } ] }
```

`midi_channel` is 1–16 in JSON; per-control `channel` keeps 0 = global. A switch that can have a
hold action has `"hold": { "action": "cc_toggle", "number": 68, "value_on": 127, "value_off": 0 }`,
`"action": "none"` for no hold; actions include `tap_tempo`. `hold_time_ms` in `global` is
milliseconds, 200–2500, stored to the nearest 10 below. Encoder modes are `none`,
`cc_relative_twos`, `cc_relative_offset`, `cc_absolute`, `program` and `cc_relative_signed`. A file
without `bank_wait` leaves it as it is. `display_timeout` is in
seconds, 0 for never. `pad_lo` and `pad_hi` are percent, 0–40. A button whose action is `bank_up`
or `bank_down` also has `"moves_programs": true` or `false`, the flag of §3; no other action has
it. Missing fields keep their current values, so a partial file is a valid overlay.

`calibration` is written only for a control that has one, and is there to be read — what this unit
measured, to compare against another or to put in a bug report. It round-trips through a file, and
it reaches no unit: `SET_CONFIG` keeps whatever calibration the unit already has (§6).

`list` always holds all eight profiles, whatever `in_use` is, and each carries the same three
mapping blocks as the top level. A profile has no `calibration`: that is the unit's, not the
mapping's (§6). The top-level blocks are the live mapping, which is the active profile's copy (§7),
so a file written by the app holds that mapping twice and the two agree. In a file that has them
disagree the top-level blocks win, as they do in the record. A file from before profiles existed
has no `profiles`, so importing it changes the active profile's mapping and leaves the others —
which is what an overlay should do.

`program_names` lists the programs that have a name, by number, and is written even when it is
empty, because when a file has it, it is the whole set: a program it does not list has no name
after the import, so a file round-trips exactly. A file without it — from before program names —
leaves them as they are. An entry whose `program` is missing or outside 0–127 is skipped, not
clamped onto 127.

## 11. Changing the format

Add fields to reserved bytes without changing the version if every existing reader can treat them
as zero. Anything else is a new version: bump `MIDI_CONTROL_CONFIG_VERSION`, teach `midi_control_config_read()`
to convert the old layout (or document that the unit falls back to defaults), add the round-trip
to `tests/test_shared.c`, and update this page.

Version 2 added §6, which needed sixteen bytes the record did not have; version 3 added §7, which
needed 2,056 more; version 4 added §8, 2,048 more; version 5 added the hold actions of §3, 720 more.
`midi_control_config_read()` takes the length from the version rather than the
other way round, so adding another block is the same shape of change: a new version, a new `OFF_`,
and a case in the `switch`. Every length is checked against the layout at compile time
(`midi_control_config_v1_check` and its neighbours), and `MIDI_CONTROL_CONFIG_MAX_BYTES` in
`midi_control_protocol.h` bounds what a CONFIG message will carry — raising the record above it
fails to compile rather than silently truncating a reply.

**The record is no longer one sector.** Three ceilings now bind it, and all three are checked where
they are broken rather than where they are relied on:

| Ceiling | What it is | Checked by |
|---|---|---|
| 8,192 bytes | the sixteen eMMC sectors a configuration slot owns, docs/06 §1 | `midi_control_config_slot_check` |
| `MIDI_CONTROL_CONFIG_MAX_BYTES` | the longest record a CONFIG message carries | `midi_control_sysex_fits_check` |
| the LPC's AHB SRAM | two SysEx buffers of `MIDI_CONTROL_SYSEX_MAX` sit in every device, in the 64 KB the USB controller's DMA structures share | the linker's `ASSERT` on `_bss_end` |

The third is the one that bites first and the reason `MIDI_CONTROL_PROFILE_COUNT` is 8 rather than
16. With the hold actions of version 5 the firmware's `.bss` ends at 0x2000f480, leaving 2,944
bytes of that bank free (measured from the link map, 2026-10-09). With the program names and
before the holds it ended at 0x2000ec80, 4,992 bytes free (measured, 2026-09-25); before the names
at 0x2000cc80, about 13 KB free (measured, 2026-09-21). The names cost 8,192 bytes: 2,048 in the
configuration, 4,688 in the two buffers, and the rest in alignment. The holds cost 2,048: 720 in
the configuration, about 1,650 in the two buffers, and the rest the device's own hold and tempo
state — less the alignment they took up. TinyUSB's `_dcd_data` is 2,048-byte aligned, and moved
from 0x2000e800 to 0x2000f000 with them; it now has 1,288 bytes of padding in front of it. Growing
the record costs roughly 3.3 times what it grows by, because it grows in the configuration and
twice more in the two buffers. `_dcd_data` is last in the bank, so what comes before it can grow
until it pushes `_dcd_data` to 0x2000f800, where it still ends by the bank's end: 3,336 bytes more,
a record about 1,000 bytes longer (derived from the map). A block bigger than that does not fit.
