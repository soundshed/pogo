# 10 — Audio

How the firmware boots the SHARC, what the SHARC program does, the link between the two, and USB
audio. The code is `firmware/src/sharc.c` (the boot), `firmware/src/i2s.c` (the LPC end of the
link), `shared/src/midi_control_audio.c` (what happens to each word), `firmware/sharc/` (the SHARC
program) and `firmware/src/usb_audio.c` (USB audio).

---

## 1. What it does

At start-up, after `CONFIG`, the `AUDIO` stage resets the SHARC and boots it with our own program.
This program moves samples and does nothing else:

```
 guitar -> ADC A left (24 dB down) -> SHARC -> link slot 0 -> LPC: x gain (volume knob) -> link slots 0..5
        -> SHARC -> every DAC channel: Amp Out, Main L/R and Phones, FX Send
```

No factory DSP code runs. Everything between the converters is done by the LPC and the host.
The volume knob sets the level of Main and Phones; by default it sends no MIDI (§8).

While a USB host plays to the unit, the outputs carry the host's two channels instead of the
guitar, and the host can record the guitar, dry, at the record trim's level (§8):

```
 guitar -> ADC A left (24 dB down) -> SHARC -> link slot 0 -> LPC: x record trim -> USB record, both channels
 USB playback L, R -> LPC: x gain -> link slots 0, 2, 4 (L) and 1, 3, 5 (R) -> SHARC -> DACs
```

If the boot fails at any step, nothing more is done and the unit runs as a silent MIDI controller,
as builds before this one did. The reasons are a card that is not up, a kernel that does not
match, an SSP0 wait that runs out, or no FLAG0.

**Status.** Measured 2026-11-13, with this firmware loaded over DFU (RAM only):

- **Boot.** The kernel CRC was `0x1F618A94`. All 1,092 words were sent with no timeouts. FLAG0's
  first edge came 13.1 ms after the stream.
- **Link.** It ran at about 48,000 frames a second. The first ~20 s showed one desync and one
  overrun, both at start-up; neither grew afterwards.
- **Signals.** Slot 1 (the guitar) peaked at about −8 dBFS while played, and slot 0 about 20 dB
  below it.
- **Outputs.** The guitar was heard at the outputs, and the knob set the level.

Installed to the eMMC and started from a power-up, measured the same day, it gave the same boot
figures and the same link counts. The guitar peaked at about −5 dBFS on slot 1, was heard at the
outputs, and the knob set the level. An installed POGO is no longer silent.

## 2. The link

The SHARC clocks the link. I2S0 on the LPC is the slave and uses four pins (§5). The word select
runs at 192 kHz with 32-bit words, giving eight words each way per 48 kHz frame. Each sample is 24
bits in the top of its word. Bit 0 of a word marks slot 0, in both directions.

| Slot | SHARC → LPC | LPC → SHARC |
|---|---|---|
| 0 | ADC A left, the marker — **the guitar, 24 dB down**: monitored and recorded | SPORT4 A left (Amp Out), the marker |
| 1 | ADC A right — **the guitar** at full gain; clips when played hard (§8) | SPORT4 A right (Amp Out) |
| 2 | ADC B left | SPORT4 B left (Main L, Phones L) |
| 3 | ADC B right | SPORT4 B right (Main R, Phones R) |
| 4 | 0 | SPORT5 A left (FX Send) |
| 5 | 0 | SPORT5 A right (FX Send) |
| 6–7 | 0 | 0 |

The jacks are measured (§4).

Slot 0 carries the guitar too, 24 dB down (measured). The SHARC sends a frame's inputs one frame
later. It aligns on the LPC's marker, so the LPC needs no other agreement about phase. Until the
LPC has found a marker, it sends zeros, and the outputs stay silent.

Every word received is one word transmitted, in the interrupt, as in the stock handler. This keeps
the two FIFOs in step. `midi_control_audio_word()` does the work:

- A marked word starts a frame. If the previous frame ended on slot 7, `frames` is counted, peaks
  are taken, and the output for the next frame is computed.
- A marker that arrives early, or one that does not arrive, counts a `desync` and the module
  hunts for the next marker.
- In the monitor mode (the default) every output is `in[monitor] × gain >> 6`, clipped to 24
  bits. The monitor is slot 0, the guitar 24 dB down: slot 1 clips at the converter when the
  guitar is played hard, at any knob position (measured, §8). The gain is q6 (64 = unity, 255 the
  most); the knob's gain never exceeds unity.
- In tone mode *n* (1..6) DAC channel *n*, link slot *n* − 1, carries a 750 Hz triangle at about
  −18 dBFS times the gain, and every other output is silent. CC 72 picks the mode (§6). This is
  how the jacks in §4 were found.

`tests/test_shared.c` (`test_audio`) covers alignment, loss, the marker, sign, gain, clipping,
the six outputs, the tone modes and the knob curve.

**The knob.** Position 0..127 (the device layer's calibrated position) maps to
`gain = (p + 1) / 2`: 0 at 0, unity at 127. The knob only attenuates, so it cannot take what it is
given past full scale. The law is linear because the pot is logarithmic. On a learned travel of 0
to 474, it read 19 at half rotation (4 %), 62 at three quarters (13 %) and 473 fully up (measured
2026-11-14). So the gain at those positions is about −27 dB, −17 dB and 0 dB (derived). The
gain is 0 until the knob has been read.

Two laws came before. The square law, `(p² + 32) / 64` with unity at 64, tapered the reading a
second time: it gave gain 0 to 1 at half rotation and 5 at three quarters, so the headphones were
silent below about four fifths of the rotation. `2p`, with +12 dB at the top, was used for one
DFU-loaded build. Both reached above unity because the outputs were quiet, and the outputs were
quiet because `MUTE` was half on (§6), not for want of gain.

## 3. The boot

The sequence is the stock loader's (parent project, docs/09 §3–4). The values are those of the
lab-image run (its variant 0).

1. **The kernel.** Read the first 3 sectors at eMMC LBA `0x8803`, the factory SHARC image. Its
   first 1,536 bytes (384 words) are ADI's SPI boot kernel. Their CRC32 must be `0x1F618A94`,
   computed from the v2.50 update image and matched by the eMMC of the development unit. A
   different kernel is not sent. The kernel is never stored in this repository. It is read off the
   unit it runs on, and this is a read only (docs/06).
2. **SSP0.** `BASE_SSP0_CLK` = `0x09000800` (PLL1, 204 MHz, autoblock). Both branches are set
   running: CCU1 `0x518` and CCU2 `0x700`. `CPSR` = 10, which gives 20.4 MHz. `CR0` = 16-bit
   frames, SPI, CPOL 1, CPHA 1. Master.
3. **Reset.** `GPIO0[11]` (`P1_4`, FUNC0) is driven low for 10 ms, then high, then 10 ms passes.
4. **Select.** `GPIO4[12]` (`P9_0`, FUNC0) is driven low, then 500 µs passes. The select stays low
   from here on, as in the stock loader.
5. **Kernel.** The 384 words are sent, the line drains, then 10 ms passes.
6. **Program.** `firmware/src/sharc_program.h` (747 words) is sent back to back, then the line
   drains.
7. **FLAG0.** `GPIO1[8]` (`P1_5`, input) is sampled every 100 µs for up to 100 ms. Two edges mean
   the program runs. It toggles FLAG0 every 480 frames (10 ms). In the lab runs it first went low
   13 ms after the stream.

**On the wire.** Words are stored big-endian. Each word goes out as two 16-bit frames, high
half first, with every byte bit-reversed. SSP0 shifts the MSB first and the SHARC takes each byte
LSB first.

**The program stream** is `tools/midi_control_sharc.py`'s output, in the factory stream's block
format:

| Block | Contents |
|---|---|
| `{5, 238, 0x8C100}` + 357 words | the program, 238 48-bit instructions packed three words to two |
| `{0, w1, w2}` + 384 words | FINAL_INIT: 256 instructions over `0x8C000..0x8C0FF`. `RTI` is at the SPI vector `0x8C030`, `JUMP 0x8C100` in every other slot |

The kernel ends by DMAing the FINAL_INIT payload over itself. A jump in every slot reaches the
program whichever way the kernel leaves.

## 4. The SHARC program

`firmware/sharc/passthru.asm`, with `audio_init.asm` appended. `audio_init` repeats the stock
program's set-up, as read out of the image (parent project, docs/09):

- **Routing.** Fifteen SRU read-modify-writes.
- **Clocks.** The PCG runs from CLKIN (24.576 MHz, derived) and produces two domains:
  - Domain A, 12.288 MHz / 192 kHz, is the LPC link. DAI 12 is the bit clock, 13 the word select,
    9 carries SHARC→LPC (SPORT6) and 10 carries LPC→SHARC (SPORT1).
  - Domain B, 48 kHz, is the converters. DAI 1 is MCLK at 12.288 MHz, 2 is BCLK and 3 is LRCK. The
    ADCs are on DAI 4 and 8 (SPORT0 A/B). The DACs are on DAI 5, 6/16 and 7 (SPORT4 A/B, SPORT5 A).
- **Converter reset.** DPI 14 goes low, then high once the PCG runs.

The PCG writes are `CTLA1 0x00100002`, `CTLB1 0x00400008`, `PW 0x01000040`, `CTLA0 0xC0000080`
and `CTLB0 0xC0000200`.

The SPORTs use the stock control words less their DMA bits:

| SPORT | Direction | Control word |
|---|---|---|
| 0 | receive, A and B | `0x010129F1` |
| 1 | receive, A | `0x000129F1` |
| 4 | transmit, A and B | `0x030129F1` |
| 5, 6 | transmit, A | `0x020129F1` |

The program polls the buffer status in place of DMA.

Each frame has two halves:

1. Link slots 0–3 out, ADC words in, the DACs' left.
2. Four zero link words, the other ADC words, the DACs' right.

`link` keeps each received slot's sample: slot 0 in R8, slot 1 in R14, slots 2–5 in USTAT1–4.
`dacs` writes each port its own pair. In the simulator, each port received exactly its own two
slots, with the left in the first half.

**The outputs.** Measured 2026-11-13, one channel at a time with tone modes 1–6 (§2):

| DAC channel | Port | Link slot | Jack |
|---|---|---|---|
| 1 | SPORT4 A (DAI 5) | 0 | Amp Out |
| 2 | SPORT4 A (DAI 5) | 1 | Amp Out |
| 3 | SPORT4 B (DAI 6/16) | 2 | Main Out L/Mono, Phones left |
| 4 | SPORT4 B (DAI 6/16) | 3 | Main Out R, Phones right |
| 5 | SPORT5 A (DAI 7) | 4 | FX Send |
| 6 | SPORT5 A (DAI 7) | 5 | FX Send |

The Phones follow the Main pair; they have no DAC of their own. The first half of the frame is
the left channel, as Main L and Phones left confirm. Amp Out and FX Send are mono jacks, and both
channels of their port reached each of them. How the two combine at the jack is not established
(§7).

The `link` routine keeps the slot count modulo 8 with `LSHIFT 29` then `LSHIFT −29`. This form
replaced `BCLR R12 BY 3` because the simulator models only `LSHIFT`. For a count of 0..8 the two
give the same result.

**Rebuilding.** Edit `firmware/sharc/`, then run:

```bash
$PY tools/midi_control_sharc.py --hx <hx-stomp-nam checkout>
```

This rewrites `firmware/src/sharc_program.h`, which is committed. The assembler is hx-stomp-nam's
`tools/sharc/sasm.py` (GPL-3.0). It runs from that checkout, and nothing of it is copied here. The
header holds only the assembled output of our own sources.

## 5. Pins and peripherals

Every pin here is one the stock firmware drives the same way (parent project, docs/09 §2).

| Pin | Function | SFS | Use |
|---|---|---|---|
| `P3_0` | SSP0_SCK, FUNC4 | `0x24` | boot clock |
| `P9_2` | SSP0_MOSI, FUNC7 | `0x27` | boot data |
| `P9_1` | SSP0_MISO, FUNC7 | `0xE7` | set as the stock firmware sets it; not read |
| `P9_0` | GPIO4[12] | `0x00` | SHARC SPI select, low from the boot on |
| `P1_4` | GPIO0[11] | `0x00` | RESET_DSP, low = reset |
| `P1_5` | GPIO1[8] | `0x40` | DSP_TO_MCU_INT = the SHARC's FLAG0, input |
| `P6_0` | I2S0_RX_SCK, FUNC4 | `0x54` | link bit clock, in |
| `P3_1` | I2S0_RX_WS, FUNC1 | `0x51` | link word select, in |
| `P6_2` | I2S0_RX_SDA, FUNC3 | `0x53` | SHARC → LPC |
| `P3_2` | I2S0_TX_SDA, FUNC0 | `0x10` | LPC → SHARC |

**I2S0** (`0x400A2000`):

- **Clocks.** `BASE_APB1_CLK` follows `BASE_M4_CLK`'s source (PLL1) with autoblock. Left on the
  12 MHz IRC, the interrupt keeps up but nothing else runs (seen in the parent project as a
  frozen screen). CCU1 `CLK_APB1_I2S` (`0x218`) is set to RUN and AUTO.
- **DAO and DAI.** 32-bit words, slave, half period 31.
- **Modes.** `TXMODE` = 6: transmit uses the receive clocks, which is why there are only four
  pins. `RXMODE` = 0.
- **Start order (stock).** Release reset, prime two zeros into the stopped FIFO, then clear STOP
  and MUTE.
- **Interrupt.** IRQ 28 (`I2S0_IRQHandler`), raised at four received words.

## 6. Checking it

```bash
$PY tools/midi_control_usb.py result --run 71
```

This returns the `audio` packet:

| Field | Meaning |
|---|---|
| `up` | 1 if the link is running |
| `sharc` | 0 = running. Otherwise 1 = no eMMC, 2 = kernel CRC, 3 = SSP0 timeout, 4 = no FLAG0 |
| `kcrc` | the kernel's CRC32 as read |
| `words` | stream words sent (1,131 when whole; 1,092 before the six-channel program) |
| `edges`, `edge1us` | FLAG0 edges seen, and when the first came |
| `frames`, `desyncs`, `overrun` | link counters: frames should climb at 48,000 a second, with the other two still |
| `gain`, `mode`, `monitor` | the gain now, the mode (0 monitor, 1..6 tone on that DAC channel), the monitored slot |
| `recgain` | the record trim now, q16 (65,536 is unity; 46,396 is −3 dB) |
| `peak0..3` | each input's largest magnitude since the last packet, before the gain and the trim |
| `hpeak` | the largest sample the USB host has sent since the last packet |
| `kpeak`, `kclips` | Main and Phones since the last packet: the largest sample after the knob's gain, and how many samples the gain took past full scale |

The boot log (`log --replay`) has a `sharc:` and an `i2s:` line for every step.

To put the tone on one output, and back to the guitar:

```bash
$PY tools/midi_control_usb.py cc 72 3      # DAC channel 3 alone: Main L and Phones left
$PY tools/midi_control_usb.py cc 72 0      # the guitar on every output
```

The mode is not stored, so every start begins in the monitor mode.

**`MUTE`.** `P8_2` (`GPIO4[2]`) is the analog outputs' mute: low mutes them, high lets them
through. The stock firmware drives it high (parent project, docs/09 section 9, its board-output
setter's index 3, written inverted). The firmware drives it low from the start of the audio
bring-up, before the SHARC boots, and high once the link runs (`audio: MUTE released` in the
log). It drives it low again before handing the unit to DFU. Before 2026-11-14 nothing drove it:
it was an input, and the outputs were half muted.

**Measured 2026-11-14, the half mute.** The tone of mode 3 at knob gain 254 (about −6 dBFS at the
DAC, derived) was only moderately loud in the headphones with `MUTE` an input (user, listening).
At gain 38, about −23 dBFS, it was very loud with `MUTE` driven high, and unchanged from the
input state with every pin low or with `AMPCTRL1` high. So with `MUTE` undriven the Phones were
about 17 dB or more down (derived from the two gains and the user's comparison, not measured at
the jack). That is what had the knob needing to be near the top, and the gain then clipping.
With `MUTE` driven high at start-up and the knob's gain held to unity, loaded over DFU, the
guitar and an amp simulator on Windows were loud and clean in the headphones across the knob's
range (user, listening, 2026-11-14).

To drive the stock board outputs, `MUTE` and `AMPCTRL1` among them (parent project, docs/09
section 9: `P1_17`, `P8_0`, `P1_16`, `PF_9`, `P4_0`, `P8_2` `MUTE`, `PF_8` `AMPCTRL1`, bits 0 to 6
in that order):

```bash
$PY tools/midi_control_usb.py cc 75 32     # 0..126: drive all seven, to these bits (32: MUTE high, the rest low)
$PY tools/midi_control_usb.py cc 75 127    # release them: inputs again, and MUTE back to the firmware's own level
```

**Measured 2026-11-14: none of the other six changes the Phones.** The tone of mode 3 played at a
knob gain of 36 to 38 (about −23 dBFS at the DAC) with the first five all low, each alone high,
all high, and released, and then with all seven low and with `AMPCTRL1` alone high. Only `MUTE`
high changed the level (user, listening on headphones). The knob's reading did not move with any
of them.

USB audio has its own packet, §8.

## 7. Not known

- How the two channels of SPORT4 A and of SPORT5 A combine at the mono Amp Out and FX Send
  jacks: summed, or tip and ring of a balanced or TRS jack. Both were heard at the jack. Until
  this is known, both channels of those ports get the same signal.
- Whether the kernel is the same on other units and other factory versions. A mismatch is
  refused, not guessed at.
- The level of ADC B, and what is on ADC A left besides the guitar's −24 dB copy.
- The round-trip latency through a host, guitar to output. The unit's FIFOs hold about 0.5 ms on
  playback and 0.13 ms on record (§8); the converters', the SHARC's and the host's share are not
  measured.
- USB audio with any host but Windows 11. With the first build an iPad played and recorded, with
  high latency and an audible warble (reported, not measured), and an Android host showed no
  audio device (reported). The likely cause is the order of the functions, MIDI's control
  interface ahead of audio's, which Linux before 6.12 cannot parse; the audio function now comes
  first (docs/03 §1). With that order (`bcdDevice` `0x0500`), traced 2026-11-14 over DFU on an
Android phone (model not recorded): it configured the unit at high speed, read the rate range
and probed both streams, opening each with a clock-validity and current-rate request, as
`snd-usb-audio` does for formats it has accepted. It then held playback open for 87 s (4,177,602
frames, no underrun until the unplug) and opened the record stream six times. No request was
refused and the rate was never set. What the user heard and saw is not recorded here. With the measured-rate feedback the warble was not heard
  on the iPad (reported). The counters over that session, read afterwards from Windows without a
  power cycle, show about 110 s of playback and at most one underrun (the other of the two was
  Windows' end of stream). The iPad's own feedback range and latency are not measured.

## 8. USB audio

The unit is a USB audio interface beside MIDI and the log (docs/03 §1): Audio Class 2.0 at high
speed, Audio Class 1.0 at full speed. `firmware/src/usb_audio.c` is the glue to TinyUSB's audio
class; `shared/src/midi_control_audio.c` decides what the outputs carry.

| | High speed (Audio 2.0) | Full speed (Audio 1.0) |
|---|---|---|
| Rate | 48 kHz only: the SHARC's clock, which the host follows. The clock source is declared internal and fixed; a request to set any other rate is refused | 48 kHz only, the format's one discrete rate. Each data endpoint has a sampling-frequency control that answers 48000 and refuses any other |
| Format | PCM, 24 bits in 4-byte subslots, the sample in the top three bytes | PCM, 16 bits in 2-byte subslots. Windows' Audio 1.0 driver makes the endpoints for 24 bits in 4 but `waveOutOpen` and `waveInOpen` refuse every format on them (`WAVERR_BADFORMAT`) and no request reaches the device (measured 2026-11-15). The unit rounds the link's 24 bits to 16 on record and widens 16 to 24 on playback (`midi_control_audio_to_s16()`, `_from_s16()`) |
| Playback | interface 1: two channels, isochronous OUT `0x04`, asynchronous, up to 56 bytes (7 frames) each microframe; explicit feedback on IN `0x84` every 1 ms, in 16.16 frames per microframe | the same interface and endpoints: up to 196 bytes (49 frames) each 1 ms frame; feedback every frame, in 10.14 frames per frame in 3 bytes |
| Record | interface 2: two channels, isochronous IN `0x85`, asynchronous, up to 56 bytes each microframe | up to 196 bytes each frame |
| Controls | the clock's frequency and validity only. No feature unit: the volume knob is the level | the endpoints' sampling frequency only |

**Full speed.** The feedback is measured the same way at both speeds, in frames per microframe
(below). At full speed it is sent as eight times that, frames per 1 ms frame, and TinyUSB packs it
as 10.14. FRINDEX holds the frame number in bits 13..3 at either speed, so at full speed the
measurement drops bits 2..0: the four values logged on the development unit were `0x1720`,
`0x1728`, `0x1730`, `0x1738`, one step of 8 a frame (measured 2026-11-15). The first four FRINDEX values seen at
full speed go to the log (`usb audio: full speed, the first FRINDEX values`). A packet is 1 ms, so
the playback FIFO's sawtooth is 48 frames deep. The playback target is therefore 2 ms (96 frames),
and the record threshold is 60 frames, above the 47 of the smallest packet: below that, TinyUSB
sends an empty packet. Each software FIFO is 3,136 bytes: 16 full-speed packets of 196 bytes, or
392 high-speed frames. At full speed the unit adds about 2 ms on playback and 1.25 ms on record,
plus a packet each way (derived).

**Full speed, measured.** 2026-11-15, Windows 11, the development unit over DFU with the test image
that holds the port to full speed (docs/07): Windows bound `usbaudio` (Audio 1.0), the MIDI port and
the log, and set 48000 on each data endpoint (two rate requests). A 20 s 440 Hz tone at −30 dBFS
played with no underrun until the stream closed; the FIFO held 392 bytes, 98 frames, against the
96-frame target; the feedback stayed between `0x5FFF0` and `0x6000A` (16.16 per microframe, ×8 on
the wire); the link measured 47,999.02 Hz against the host, as at high speed. The record stream
ran the same 20 s, the guitar input's noise at −23.1 dBFS peak. Not measured on macOS, Linux, an
iPad, Android, or a hardware MIDI host.

**The data path.** TinyUSB copies each packet between its endpoint buffer in AHB SRAM and a
3,136-byte software FIFO, in the USB interrupt. At each frame boundary the I2S0 interrupt takes one
frame from the playback FIFO and puts one in the record FIFO (`usb_audio_frame()`). Each FIFO has
one writer and one reader, one in each interrupt. I2S0 has the higher priority: it has four words,
21 µs, before its receive FIFO overruns, and the USB interrupt now copies audio.

**Playback.** When the host opens the stream, the outputs go silent until the FIFO holds 0.5 ms
(24 frames, 192 bytes, four packets). Then they carry the host's audio: left on DAC channels 1, 3 and 5
(link slots 0, 2, 4: Amp Out, Main L and Phones left, FX Send), right on 2, 4 and 6. The volume
knob scales Main and Phones only (link slots 2, 3); Amp Out and FX Send stay at unity, as
line-level feeds, for the host and the guitar alike. The knob sends no MIDI by default (docs/03
§2), so an app that obeys CC#7 does not turn itself down with it. A FIFO that runs dry counts an underrun and fills to 0.5 ms again before it plays. When
the stream closes, or the host unmounts or suspends the device, the outputs go back to the guitar.
A tone mode (§2) overrides both.

**Feedback.** The value sent on `0x84` is the link's frame rate measured against the host's own
microframes (`midi_control_audio_rate_sof()`, from the USB start-of-frame interrupt). Every 256 ms
it divides the link frames counted since the oldest of 16 snapshots by the microframes since then,
FRINDEX's count, so the window is 3.84 s once full and one frame of quantisation is 2 units of
16.16, about 5 ppm. A trim of 1 unit, about 2.5 ppm, for each frame the playback FIFO's 64 ms
average is off its target steers the FIFO back with a time constant of about 8 s. The rate is held
to ±0.5 % of 6.0 frames a microframe and the trim to ±1,024 units (derived; tests/test_shared.c).
The trim was 8 units a frame until 2026-11-15. On an iPad that swung the feedback over 38 units,
about 100 ppm, around a rate of −25 ppm, and Soundshed Guitar Nano Q, guitar in and amp out, clicked
every 5 to 10 s while plain music playback did not (reported); the USB counters showed no underrun,
no empty record packet and no link fault (measured).

The first build used TinyUSB's `FIFO_COUNT` method instead. That method recomputes the value
from the FIFO's level at every packet received. The level moves in steps of one frame (8 bytes),
and each step moves the value by 168 units, about 430 ppm (derived from TinyUSB's
`rate_const`: 65,536 / 384 bytes / 8). The host was therefore told about a clock that
moved by hundreds of ppm from one millisecond to the next. A host that times its own audio clock
from the device follows that wobble. A host that resamples, as iOS does between an app and the
device, hears it as pitch.

**Record.** Both channels carry the guitar from link slot 0, ADC A left, the copy 24 dB below
slot 1, times the record trim and not the knob's gain. Slot 1 clips at the converter when the
guitar is played hard (measured below); slot 0 does not, and the outputs monitor it too. The trim is the
configuration's `record_gain` (docs/05 §2), −48 to +12 dB in whole dB, −3 dB unless set; the LPC
multiplies by a q16 table of 10^(dB/20) and clips at full scale. At −3 dB the hard-played peaks
measured on slot 0, −14.7 dBFS, arrive near −18 dBFS, the level an amp simulator expects of a DI
(derived). A new configuration changes it at once. TinyUSB sends 5, 6 or 7
frames a packet to hold the record FIFO near 12 frames when each packet is made
(`tud_audio_set_ep_in_fifo_threshold`), so about 6 frames, 125 µs, wait behind each packet sent.

**Latency the unit adds** (derived): 0.5 ms on playback and about 0.13 ms on record, plus up to a
packet each way. The first build held 1 ms each way. The host's own buffers are separate and
usually larger: on iOS and Android the app's I/O buffer, and any resampling the host does when
the app's rate is not 48 kHz.

**Measured** 2026-11-13 with the first build (`FIFO_COUNT` feedback, 1 ms each way), loaded over
DFU, Windows 11 as the host:

- **Enumeration.** All four functions started, and Windows made a playback and a recording device,
  both `Line (POGO Audio)`. The link ran on with no desyncs or overruns.
- **Playback.** A 440 Hz tone at −30 dBFS, 4 s and then 8 s, was heard at the outputs. Halfway
  through, the stream was running with no underrun and the FIFO held 344 and 368 bytes against
  the 384 aimed at. Each stream ended with one underrun: the host stops sending before it closes
  the stream. The guitar came back afterwards.
- **Record.** 40 s recorded at 48 kHz while the guitar was played: peak −12.1 dBFS, RMS −33.7 dB,
  both channels the same up to the host's own 16-bit dither. After the playing began there was no
  run of more than 8 zero samples and no step over 1.2 % of full scale.
- **Installed.** Written to the eMMC the same day, and started after 15 s off (docs/09: a quicker
  cycle started the image before it): the boot sent all 1,131 words, Windows made the same two
  devices, a 4 s tone played with no underrun mid-stream and the FIFO at 368 bytes, and a 3 s
  recording came in whole.

**Measured** 2026-11-13 with the measured-rate feedback and 0.5 ms playback target, loaded over
DFU, Windows 11 as the host:

- **Rate.** The link ran at 47,999.023 Hz against the PC's USB clock (`ratemhz`), 20 ppm slow. The
  feedback settled at 393,208 (6.0 − 8/65,536), all of it rate and no trim.
- **Steadiness.** Over the 16 s of a 20 s tone after the window filled, the feedback stayed within
  393,195 to 393,224, a spread of 29 units (about 74 ppm). One frame of level error moved the
  `FIFO_COUNT` value by 168 units.
- **Playback.** A 440 Hz tone at −30 dBFS for 20 s played with no underrun mid-stream; the FIFO read
  224 and 232 bytes against the 192 aimed at, read at random points of the sawtooth. As before, one
  underrun at the end.
- **Record.** 8 s recorded whole, 8.00 s at 48 kHz, no link overruns. No guitar was played, so gaps
  were not looked for.

**Measured** 2026-11-14 with the first record-trim build, which recorded slot 1 at −10 dB, loaded
over DFU, Windows 11 recording 30 s at 24 bits while the guitar was played hard:

- **Slot 1 clips.** Its peak counter read full scale (`0x800000`), and slot 0, the same guitar 24 dB
  down, peaked at `0x1770C6`, −14.7 dBFS. The recording peaked at −9.8 dBFS, full scale less the
  trim, with 17,733 samples within 2 % of that peak and 799 flat runs of three or more (the longest
  37 samples, 0.77 ms). RMS was −18.6 dB, a crest factor of 8.8 dB. The clipping happens before the
  LPC, so the trim lowers it but cannot remove it. The record source moved to slot 0 for this.

**Measured** 2026-11-14 with the record taken from slot 0, loaded over DFU, the same 30 s recording
while the guitar was played hard. The trim read −10 dB (`recgain` 20,724), not the new −3 dB
default: the build before had saved its own default into the record (docs/05 §2).

- **Slot 0 does not clip.** It peaked at `0x1C0C45`, −13.2 dBFS, while slot 1 again read full
  scale. The recording peaked at −23.2 dBFS, slot 0's peak less the trim, with RMS −43.1 dB, a
  crest factor of 19.9 dB. Eleven samples lay within 2 % of the peak, the longest run 6 samples:
  a waveform's crest, not a flat top.
- **Level.** The two hard-played takes peaked at −14.7 and −13.2 dBFS on slot 0, so at the −3 dB
  default they would arrive at −17.7 and −16.2 dBFS (derived).

**Measured** 2026-11-14, the same build with `hpeak`, `kpeak` and `kclips` added, read every 5 s
for a minute while the guitar was played, first alone and then through an amp simulator on
Windows. The user had reported the Phones distorting without being loud.

- **The monitor was the distortion.** With no host playing, the outputs carried slot 1, which read
  0.0 dBFS in every 5 s window while slot 0 peaked between −16.5 and −4.6 dBFS. The clipping was
  in the signal before any gain. Turning the knob up to gain 110 to 116 (+4.7 to +5.2 dB) clipped
  it again at the LPC: `kclips` counted up to 142,938 samples in one window. The monitor moved to
  slot 0 for this.
- **The host's path was clean.** With the simulator playing, it sent peaks of −7.3 to −13.9 dBFS
  (`hpeak`); at +4.9 to +5.2 dB the Phones peaked at −2.0 to −8.7 dBFS with no clipped sample,
  and one underrun in 30 s.

**Measured** 2026-11-15, iPad, Soundshed Guitar Nano Q (guitar in, amp out), over DFU with the
packet counters below. With the trim at 8 units a frame: about 3 clicks in 43 s, 5 to 10 s apart
(reported); plain music playback did not click (reported). The counters for that session showed no
underrun before the stream ended, 53 playback packets one frame short and one long (the iPad
following the feedback), no empty record packet beyond the one each stream opens with, never fewer
than 5 frames behind a record packet, and no link desync or overrun. The feedback ranged over 38
units (`0x5FFD9`..`0x5FFFF`, about 100 ppm) around a link rate of 47,998.78 Hz against the iPad.
With the trim at 1 unit a frame, the same use for about 125 s over three streams: no clicks
(reported), no underrun before each stream ended, and the last stream's feedback over 15 units
(`0x5FFFA`..`0x60009`). One session each, so the trim is the likely cause, not a proven one. On
Windows the old trim's spread was 29 units over 16 s and the new one's 12.

Each record stream starts with about 375 to 385 frames, 8 ms, in its FIFO (`recmax`): the link
fills it from the moment the interface opens, before the host polls. TinyUSB's flow control sends
a 7-frame packet every 11th microframe until the level is back near 12 frames, about 0.5 s.

```bash
$PY tools/midi_control_usb.py result --run 73
```

This returns the `usbaudio` packet:

| Field | Meaning |
|---|---|
| `play` | 0 closed, 1 filling to 0.5 ms, 2 playing |
| `record` | 1 while the record stream is open |
| `host` | 1 while the outputs carry the host (filling included) |
| `pframes`, `underrun` | frames taken from the host, and times the FIFO ran dry |
| `rxpkts`, `level` | playback packets received (8,000 a second while open), and the FIFO's bytes now |
| `rframes` | frames sent to the host |
| `ratesets` | requests to set the rate; only 48,000 is accepted |
| `fb` | the feedback last sent, 16.16 frames a microframe (393,216 is 6.0) |
| `fbmin`, `fbmax` | its range since the rate window filled, about 4 s after the stream opened; 0 before |
| `ratemhz` | the link's rate measured against the host's clock, in millihertz |
| `overrun` | the link's FIFO overruns, as in the `audio` packet |

`result --run 73 --runval 2` returns the `usbpkts` packet, counted since the unit started:

| Field | Meaning |
|---|---|
| `rx0`, `rxshort`, `rxnom`, `rxlong`, `rxodd` | playback packets received: empty, one frame short of the nominal (6 a microframe, 48 a frame), nominal, one long, anything else |
| `tx0`, `txshort`, `txnom`, `txlong`, `txodd` | record packets sent, the same way; each record stream starts with one empty packet, the one TinyUSB primes the endpoint with |
| `recdrops` | record frames TinyUSB's FIFO refused. The FIFO overwrites when full, so a host that stops polling is not counted. Frames written while the function is unconfigured are, as when the cable moves to another host before the unit sees the first one go |
| `recmin`, `recmax` | frames waiting in the record FIFO behind each packet sent, since the stream opened |
| `playmin` | the playback FIFO's lowest level while playing, since the stream opened; it reaches 0 as every stream ends |
| `underrun`, `underrun_s` | as in `usbaudio`, and the link's seconds at the last one |
