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

Everything here is configurable — docs/04, docs/05. These are the defaults, on channel 1:

| Control | Sends |
|---|---|
| Footswitches A–D | Note On/Off 60–63, velocity 127; LED white while held |
| Tap / Tuner | Note 64; the Tap lamp while held |
| Bank Up / Bank Down | program ± 4, sent as CC#0 = 0, CC#32 = 0, Program Change |
| View, Bypass, Menu, Mode | Notes 65–68 |
| Home encoder push, Menu encoder push | Notes 69, 70 |
| Encoder 1–5 push | Notes 71–75 |
| Pedal toe switch, PAGE | Notes 76, 77 |
| Encoders 1–5, Home, Menu | relative CC#16–22: `1` clockwise, `127` counter-clockwise, one per detent |
| Volume knob | CC#7, 0–127 |
| Expression pedal | CC#11, 0–127, toe-down = 127 |
| Host → device | Program Change moves the display and the preset-in-bank LEDs. Our SysEx is answered, including EVENT, which uses a control as though on the panel (docs/04). Everything else is ignored |

The display shows, along the top, the configured name, the MIDI channel, USB and the storage
state; the program as bank number + letter (`1A` … `32D` with four per bank) with `PC n` and two
lines of host-supplied text (SysEx `SET_TEXT`); a dial for each of encoders 1–5; the volume knob
and pedal positions; and a strip per footswitch saying what it does, lit in its LED's colour while
it is active. While the firmware starts it shows the bring-up stage instead. docs/08 §5. With the
global display timeout set, the screen goes dark once no control has been used for that long; any
control lights it again and still sends what it sends (docs/05 §2).

## 3. Program numbers

Programs are 0–127. "Bank" is a display and stepping convention over that range: bank size is a
global setting (default 4), Bank Up/Down step by it, *preset in bank* actions select within it,
and the display letters presets within a bank. Bank Select CC#0/CC#32 are sent as zero before every
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
| 76 | **127** | result packet after the payload | arm an eMMC write; then `"SSWRIMG!"`, u32 LBA, u32 length, u32 CRC (advisory), payload. Written, then every sector read back: `WRITTEN AND VERIFIED` or not. Refused past sector 1364 |

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
