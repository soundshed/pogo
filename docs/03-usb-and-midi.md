# 03 — USB and MIDI

What the unit presents to a host while running POGO, what it sends by default, and the
diagnostic channel beside it.

---

## 1. Identity

| | |
|---|---|
| VID / PID | `0x1209` / `0x0001` — pid.codes, the PID reserved for testing. Line 6's `0x0E41` is not ours to use |
| `bcdUSB` | `0x0201` — so Windows asks for the BOS descriptor |
| `bcdDevice` | `0x0300` — Windows caches MS OS descriptor results per VID/PID/bcdDevice; bump it whenever the interface layout changes |
| Interface 0–1 | USB-MIDI: AudioControl + MIDIStreaming, bulk EP `0x02` OUT / `0x82` IN, 512 bytes. One cable, embedded jacks wired directly to the endpoints so the interface remains active without a physical DIN input |
| Interface 2 | vendor-specific, bulk EP `0x03` OUT / `0x83` IN, 512 bytes — the log and the eMMC commands |
| Strings | manufacturer and product `POGO`, serial `000001`, interfaces `POGO MIDI` and `POGO Log`; the name is `MIDI_CONTROL_PRODUCT_NAME` |

On Windows the MIDI interface binds to the class driver and appears as a MIDI in/out pair named
**POGO MIDI**. The vendor interface binds to WinUSB by itself through the MS OS 2.0
descriptor set, device interface GUID `{6E4B1F2A-0C3D-4A5E-9B7C-8D1E2F3A4B5C}` — the same GUID
`tools/midi_control_usb.py` and the app's `MidiControlUsb.cpp` enumerate. Three things about that set fail
silently if wrong: `bcdUSB ≥ 0x0201`, a fresh `bcdDevice`, and the *composite* shape (configuration
subset → function subset → compatible ID + property).

## 2. What the controls send (factory configuration)

Everything here is configurable — docs/04, docs/05. These are the defaults, on channel 1. The
reference controller, whose mapping was verified by MIDI capture, sent notes 60–77 from every
switch but the bank pair, and POGO did too until 0.3.0. Since then A–D choose presets, because the
screen names the preset in use (docs/05 §8), which notes never change; Tap taps the tempo; and
the other switches send CCs, because effect plugins learn CCs where many ignore notes. The CCs are
102–114, in the range MIDI leaves undefined, so nothing a host gives a meaning by default answers
them; Tap's 64 and the tuner's 68 are Line 6's numbers for the same two jobs. A unit with saved
settings keeps the mapping it saved. To send the notes again, set the switches to
*Note (momentary)* 60–77 in that order.

| Control | Sends |
|---|---|
| Footswitches A–D | the four presets of the bank (*preset in bank* 0–3): Program Change to that preset, after CC#0 = 0 and CC#32 = 0; LED white on the preset in use |
| Tap / Tuner | *Tap tempo*: CC#64 = 127 at each press, and the Tap lamp flashes the tempo tapped, or the host's MIDI clock (docs/05 §3). Held 500 ms: the tuner, CC#68 toggled 127 / 0 |
| Bank Up / Bank Down | choose the next / previous bank and send nothing; A–D flash until one of them loads a preset from it (docs/05 §3). With the global `bank_wait` off: program ± 4, sent as CC#0 = 0, CC#32 = 0, Program Change |
| View, Bypass, Menu, Mode | CC#102–105, 127 while down and 0 at the release |
| Home encoder push, Menu encoder push | CC#106, 107, the same |
| Encoder 1–5 push | CC#108–112, the same |
| Pedal toe switch | CC#113, toggled 127 / 0 at each press, as a wah's switch is |
| PAGE | CC#114, 127 while down and 0 at the release |
| Encoders 1–5, Home, Menu | relative CC#16–22: `1` clockwise, `127` counter-clockwise, one per detent |
| Volume knob | CC#7, 0–127 |
| Expression pedal | CC#11, 0–127, toe-down = 127. Both analog controls send their position again when USB comes up (docs/05 §5) |
| Mode held | after 800 ms, opens the profile picker: A–D then choose a profile and Bank Up/Down page through them, sending nothing while it is open (docs/05 §7). Mode still sends CC#105 at the press |
| Toe switch held | after 2 s, starts calibrating the pedal: hold it still at one end, then the other, and those become its travel; the pedal sends nothing meanwhile, and Mode cancels (docs/05 §6). The toe switch still sends CC#113 at the press |
| Host → device | Program Change on channel 1 (or a channel a preset control is set to) moves the display and the preset LEDs; a CC or note sets the toggles and absolute encoders that send it (docs/05 §2); MIDI clock sets the tempo the Tap lamp flashes (docs/05 §3). Our SysEx is answered, including EVENT, which uses a control as though on the panel (docs/04). Everything else is ignored |

The display shows, along the top, the configured name, the MIDI channel, USB and the storage
state; the program as bank number + letter (`1A` … `32D` with four per bank) with `PC n` and two
lines of host-supplied text (SysEx `SET_TEXT`), the first giving way to the program's own name when
the configuration gives it one (docs/05 §8); a dial for each of encoders 1–5; the volume knob
and pedal positions; and a strip per footswitch saying what it does, lit in its LED's colour while
it is active. While the firmware starts it shows the bring-up stage instead. docs/08 §5. With more
than one profile in use the top line also names the live one, and while the picker is open it takes
the program's place (docs/05 §7). With the
global display timeout set, the screen goes dark once no control has been used for that long; any
control lights it again and still sends what it sends (docs/05 §2).

**When nothing on the host is reading.** A channel message goes to the MIDI IN endpoint as one
USB-MIDI event packet, written whole or not at all (`midi_write_channel` in `main.c`), and so independent of
the byte stream's state. TinyUSB's stream writer checks for a packet's room before every byte, so
it stops only between packets; but a SysEx reply abandoned part way by a full FIFO leaves the
stream in its SysEx state, and a status byte sent through the stream after that is packed as SysEx
data (derived from `tud_midi_n_stream_write`, TinyUSB 0.21.0). The next SysEx reply through the
stream is still exposed to that: it is appended to the abandoned one (derived, not seen). A message
waits for room for about one panel scan (2,000 `tud_task()`
passes); once one such wait has failed, each message is tried once and dropped until one goes
through again, so a pedal sweep with no listener does not hold the panel up message by message. Our
SysEx replies keep the longer bounded wait, because the host asked for them. Not measured yet: how
long that wait is in milliseconds, and which hosts stop reading the endpoint while no application
has the port open.

## 3. Program numbers

Programs are 0–127. "Bank" is a display and stepping convention over that range: bank size is a
global setting (default 4), Bank Up/Down choose a bank — waiting for a preset switch to load from
it, by default — and, where a bank button is set to, move the *Program Change* buttons by it too
(docs/05 §3); *preset in bank* actions select within it, and the display letters presets within a
bank. A size that does not divide 128 leaves the last bank short, and banks wrap as a whole. Bank Select CC#0/CC#32 are sent as zero before every
Program Change when the global `bank_select` is on, because some receivers expect the triple.

## 4. The vendor endpoint

Plain bytes, no framing. Host → device is a stream of `{command, value}` byte pairs (the same shape
as a control change, deliberately). Device → host is either the log (text, newline separated) or a
single result packet under 400 bytes.

| Command | Value | Answer | Does |
|---|---|---|---|
| 126 | any | log stream | replay the log from the start |
| 93 | any | result packet | re-send the last result |
| 68 | **127** | result packet `dfu REBOOTING INTO DFU` | hand the unit to the boot ROM's DFU loader: about a second off the bus, then `1FC9:000C` (docs/01 §3) |
| 70 | any | result packet | `== storage`: card, state, loaded, seq, slot, dirty, lastsave, program |
| 95 | any | result packet | `== adc`: every ADC channel raw, each analog control's learned range and last value |
| 74 | any | log | LCD controller registers: `CTRL UPBASE INTRAW UPCURR` twice |
| 89 | LBA 0–127 | result packet | first eight words of one eMMC sector; `MAGIC MATCH` if it is a bootloader image header |
| 79 | any | `SSEMMCDP` frame | stream sectors 0..1364 (16-byte header: magic, u32 length, u32 zero) |
| 76 | **127** | result packet after the payload | arm an eMMC write; then `"SSWRIMG!"`, u32 LBA, u32 length, u32 CRC (advisory), payload. Written, then every sector read back: `WRITTEN AND VERIFIED` or not. Refused past sector 1364, with SDRAM failed, or on a bad header, and a refused transfer is still consumed and discarded, never parsed as commands: to the declared length, or after a bad header until 500 ms of silence (docs/06 §3) |

Rules for the host side, each learned the hard way:

- **Read until the device has been quiet for a few seconds before closing the handle.** Closing
  with an IN transfer queued strands it on the device; the endpoint then looks dead until a reset.
- Read one 512-byte packet per call with a short (150 ms) timeout. The firmware never fills a whole
  packet on the log path (511 max) so every read completes by itself.
- **Send a command on the same handle that reads the answer.**
- One accepted write proves nothing: the first lands in the peripheral's FIFO. A second write
  failing, or an answer arriving, is what proves the firmware is alive.
- MIDI on Windows (`winmm`) wedges if two processes open a port at once, and takes minutes to say
  so. The vendor endpoint has none of that, which is why the log is not on MIDI.
