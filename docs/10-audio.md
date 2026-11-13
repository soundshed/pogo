# 10 — Audio

How the firmware boots the SHARC, what the SHARC program does, and the link between the two. The
code is `firmware/src/sharc.c` (the boot), `firmware/src/i2s.c` (the LPC end of the link),
`shared/src/midi_control_audio.c` (what happens to each word) and `firmware/sharc/` (the SHARC
program).

---

## 1. What it does

At start-up, after `CONFIG`, the `AUDIO` stage resets the SHARC and boots it with our own program.
This program moves samples and does nothing else:

```
 guitar -> ADC A right -> SHARC -> link slot 1 -> LPC: x gain (volume knob) -> link slots 0, 1
        -> SHARC -> left and right of every DAC
```

No factory DSP code runs. Everything between the converters is done by the LPC and, later, the
host. The volume knob keeps sending MIDI as before and also sets the level.

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
| 0 | ADC A left, the marker | output left, the marker |
| 1 | ADC A right — **the guitar** (measured, about −10 dBFS) | output right |
| 2 | ADC B left | 0 |
| 3 | ADC B right | 0 |
| 4–7 | 0 | 0 |

Slot 0 carries the guitar too, 24 dB down (measured). The SHARC sends a frame's inputs one frame
later. It aligns on the LPC's marker, so the LPC needs no other agreement about phase. Until the
LPC has found a marker, it sends zeros, and the outputs stay silent.

Every word received is one word transmitted, in the interrupt, as in the stock handler. This keeps
the two FIFOs in step. `midi_control_audio_word()` does the work:

- A marked word starts a frame. If the previous frame ended on slot 7, `frames` is counted, peaks
  are taken, and the output for the next frame is computed.
- A marker that arrives early, or one that does not arrive, counts a `desync` and the module
  hunts for the next marker.
- The output is `in[monitor] × gain >> 6`, clipped to 24 bits, on both sides. The monitor is slot
  1. The gain is q6 (64 = unity, 255 the most).

`tests/test_shared.c` (`test_audio`) covers alignment, loss, the marker, sign, gain, clipping and
the knob curve.

**The knob.** Position 0..127 (the device layer's calibrated position) maps to
`gain = (p² + 32) / 64`: 0 at 0, unity at 64, 252 at 127. This is a square law, so the knob feels
roughly even in loudness. The gain is 0 until the knob has been read.

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
6. **Program.** `firmware/src/sharc_program.h` (708 words) is sent back to back, then the line
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
| `{5, 212, 0x8C100}` + 318 words | the program, 212 48-bit instructions packed three words to two |
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
| `words` | stream words sent (1,092 when whole) |
| `edges`, `edge1us` | FLAG0 edges seen, and when the first came |
| `frames`, `desyncs`, `overrun` | link counters: frames should climb at 48,000 a second, with the other two still |
| `gain`, `monitor` | the gain now, the monitored slot |
| `peak0..3` | each input's largest magnitude since the last packet |

The boot log (`log --replay`) has a `sharc:` and an `i2s:` line for every step.

## 7. Not known

- Which jack each DAC port (DAI 5, 6/16, 7) feeds, and the left/right order within each. Every
  DAC gets the same pair for now.
- Whether the kernel is the same on other units and other factory versions. A mismatch is
  refused, not guessed at.
- The level of ADC B, and what is on ADC A left besides the guitar's −24 dB copy.
