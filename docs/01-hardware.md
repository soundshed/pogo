# 01 — Hardware

The parts of the POD Go this firmware uses, and the facts about them the code depends on. Every
address and pin here is one the firmware writes; the drivers in `firmware/src/` are the executable
form of this document.

---

## 1. Processors

| | LPC4350 (this firmware) | ADSP-2148x SHARC (untouched) |
|---|---|---|
| Role | UI, LCD, USB, storage | all audio |
| Clock | ~96 MHz as left by the boot ROM; this firmware raises PLL1 to 204 MHz for the SDRAM | 400 MHz |
| Cores | Cortex-M4F + Cortex-M0; only the M4 is used, the M0 stays halted | one |
| Flash | **none** | none; booted by the LPC over SSP0 |
| FPU | present but **disabled at reset**; the firmware is built `-mfloat-abi=soft` and uses none | — |

The SHARC keeps running the factory DSP image across a DFU load and across our image booting from
the eMMC. It clocks an I2S link to the LPC that this firmware does not use.

## 2. Memory

| Region | Base | Size | Notes |
|---|---|---|---|
| Local SRAM bank 0 | `0x10000000` | 128 KB | code and `.rodata`; the DFU load address and the bootloader's destination |
| Local SRAM bank 2 | `0x10080000` | 72 KB | the stack (`_stack_top = 0x10092000`) |
| AHB SRAM | `0x20000000` | 64 KB | `.data` and `.bss`; **the only SRAM the USB and LCD DMA can reach** |
| SDRAM | `0x28000000` | 32 MiB | colour framebuffer at the base; the UI's back buffer at +1 MiB; image receive buffer at +2 MiB |
| QSPI NOR (SPIFI) | `0x14000000` | 1 MiB | NOR bootloader. Never written |
| eMMC | SD/MMC controller | 4 GB | raw sectors, no filesystem — docs/06 |

**The USB controller and the LCD controller are AHB masters and cannot reach local SRAM.** Every
buffer either touches must be in AHB SRAM or SDRAM. `firmware/midi_control.ld` puts all writable data in
AHB SRAM for this reason, and asserts it.

SDRAM (measured): 32 MiB, 32-bit bus, 4 banks, row 12, column 9, run at 102 MHz (half the core
clock), CAS 2, burst 4. The bring-up sequence is in docs/02 §3 and `emc.c`.

## 3. Boot and DFU

The mask-ROM boot loader selects SPIFI on a normal power-on (the NOR bootloader then loads the
application from the eMMC) or **USB0 DFU** when strapped. In DFU mode the unit enumerates as
`VID_1FC9&PID_000C`, DFU 1.1, 2048-byte blocks, **upload not supported**, no authentication of any
kind (the LPC4350 is the non-secure part).

Three ways into DFU, all run on a unit:

- **From the factory system**: a nine-message prelude on its vendor USB pipe ending in message type
  `0x0002` at address `0x1036`. The factory system writes `"BootIntoDfu!"` at `0x1001ff00` and
  resets; the bootloader sees the string and takes the ROM's USB path.
- **`PAGE >` held at power-on.** Works with anything on the eMMC.
- **From POGO**: vendor command 68, value 127 (docs/03 §4). The normal reset
  route is unavailable because no reset brings back a unit running a full image (§8, docs/09).
  So `board_enter_dfu()` performs the bootloader's post-reset handoff sequence without a reset:

  ```
  0x14001990  cpsid i; MSP = 0x1008169c; VTOR = 0x14000000; CPACR |= 0x00F00000; SHCSR = 0
  0x14004e5c  if the 13 bytes at 0x1001ff00 are "BootIntoDfu!\0", clear them and go on
  0x14004e74  a delay; FP_REMAP = 0x20000000; FP_COMP5 = 0x10400F64;
              *(0x20000014) = 0x20152015 (movs r0, #0x15, twice); FP_COMP5 |= 1; FP_CTRL = 3;
              call 0x104014B9, call 0x10400A05, jump 0x10400A27, each with r0 = 0x40045000
  ```

  The FPB patch replaces the word where the ROM settles its boot source with `0x15`, USB0;
  `0x40045000` is the OTP controller. Before the same writes and calls, POGO masks interrupts and
  clears the NVIC, puts VTOR on the ROM's own table (`0x10400000`), takes USB0 off the bus for about
  a second and resets it, and moves the core onto the IRC with PLL1 powered down, so the ROM locks
  PLL1 from cold rather than under the running core (docs/09). The screen shows `FIRMWARE UPDATE
  MODE` on the start-up screen before it goes. The calls run from a copy at
  `0x1001FF40`, on the bootloader's stack: the download writes over POGO's code at `0x10000000`,
  and the first call is expected to return after it (derived: the bootloader makes two more).

`tools/midi_control_usb.py reboot-dfu` and the app's Firmware tab take whichever route the unit on
the bus has.

The DFU image format is a 16-byte header `1a 00 <blocks:u16 LE> 00*12` then the body in 512-byte
blocks. The ROM loads the body to `0x10000000` and branches through **word 1 of the vector table**;
it does **not** load SP from word 0. The bootloader's own image format is different (docs/06 §2).

## 4. Front panel

One microcontroller, no port expanders: everything is wired straight to LPC balls. A 7-column
matrix carries the RGB LEDs, three rows of switches and one encoder per column.

```
               COL1     COL2     COL3   COL4   COL5   COL6      COL7
             Bank Up  Bank Dn    A      B      C      D     Vol/Wah/Tap
SWROW1        View    Bypass   Menu   Mode    -    Home push Menu push
SWROW2       Bank Up  Bank Dn    A      B      C      D        Tap
SWROW3        Enc1     Enc2    Enc3   Enc4   Enc5    -          -
ENC_A/B       Enc1     Enc2    Enc3   Enc4   Enc5   Home      Menu
LED           RGB      RGB     RGB    RGB    RGB    RGB   Vol / Wah / Tap lamps
```

### Pins (measured and verified; reference setup at `0x28c6bd14`)

| Signal | Pin | SFS | GPIO | Direction |
|---|---|---|---|---|
| COL1..COL7 | `PD_11 P9_4 P1_20 PD_12 P1_18 P1_15 PF_1` | `0x34`/`0x30` (func, EPUN, EHS) | `6[25] 5[17] 0[15] 6[26] 0[13] 0[2] 7[16]` | out, one high at a time |
| SWROW1, SWROW2, SWROW3 | `PA_1 PA_2 PF_2` | `0x58`/`0x5c` (pull-down) | `4[8] 4[9] 7[17]` | in; closed reads **high** |
| ENC_A, ENC_B | `PF_10 PF_11` | `0x5c` | `7[24] 7[25]` | in; bit 1 = A, bit 0 = B |
| RED, GREEN, BLUE rows | `PD_13 PD_15 PD_14` | `0x34` | `6[27] 6[29] 6[28]` | out, **active high** (measured) |
| PEDAL_TOE_SW | `P6_1` | `0x40` (pull-up) | `3[0]` | in, **active low** |
| PAGE | `PD_10` | `0x44` (pull-up) | `6[24]` | in, reads **high when pressed** (measured; the board pulls it down) |

SCU register address is `0x40086000 + port*0x80 + pin*4`; ports A–F are 10–15. GPIO `DIR/PIN/SET/CLR`
are at `0x400F6000/6100/6200/6280 + port*4`.

### The scan (`board_panel_scan`)

Per column: drive the RGB rows for that column's colour, raise the column, **wait ~500 µs** (the
switch lines are RC filtered; 10 µs sees no closed switch, ever), read the three rows and the two
encoder lines, drop the column. ~4 ms a scan. Rules that are electrical, not stylistic:

- **Drive the rows only inside the strobe.** A column held high outside its strobe pulls a row high
  through any closed switch (the whole row reads pressed) and shorts a driven high to a driven low
  through two closed switches. LED feedback is therefore multiplexed inside the scan, at 1/7 duty.
- **Prime every "previous state" on the first scan** or every control emits a phantom event.
- Debounce: 4 consecutive agreeing scans.

### Encoders (verified by operation)

Two 16-entry transition tables indexed by `old_code*4 + new_code`. Columns 1–5 use the full
quadrature table (±1 per transition, four per detent, divisor 4); Home and Menu use a table with
one count per detent (divisor 1). Both are in `main.c`.

### LEDs

Columns 1–6: one RGB LED each, three on/off bits. This firmware drives the rows as GPIO; the SCT
mapping supports full-colour PWM on `CTOUT_13/8/11`, with master brightness on `CTOUT_12`.
Column 7: three separate lamps — bit 0 Vol (red), bit 1 Wah (green), bit 2 the **Tap Tempo** lamp.

### Analog inputs (measured)

| Control | ADC | Range seen | Notes |
|---|---|---|---|
| Volume knob | ADC0 channel 0 | 0..~472 with SDRAM up (~1000 before) | dithers a count or two at rest |
| Expression pedal | ADC0 channel 7 | ~280..664 on this unit | voltage **falls** toe-down; the firmware inverts |

Dedicated ADC balls, no pin mux, but `SCU_ENAIO0/1` (`0x40086C88/C8C`) must enable channels
`0x8D` (0, 2, 3, 7) — and **only** those: `ADC0_5` shares `PF_10`, which is `ENC_A`. Both ADCs run
burst mode on all channels (`CR = 0xFF | 40<<8 | 1<<16 | 1<<21`) and are polled.

The ranges above are **this unit's**, and are only the seeds a unit starts from: the travel is
learned from use, stored with the configuration, and read back at the next power-on, so neither the
range nor the position the pedal is in when the unit comes up decides what it can reach. The seeds
are in `k_analog` in `firmware/src/main.c`; the learning, the scaling, the hysteresis and the end
zones are in `shared/src/midi_control_analog.c`, and what is stored is docs/05 §6.

## 5. Display

480x272 TFT on the LPC's PL111 LCD controller at `0x40008000`, **16 bpp 5:6:5** (CTRL bpp field 6),
plain RGB565 with red in the high bits (the BGR bit is set and needs no software swap — measured).
Timings HSW/HFP/HBP 1/2/42, VSW/VFP/VBP 1/2/2, pixel clock 12 MHz (82.5 Hz). **The panel is fitted
180° round**; `midi_control_gfx` rotates every pixel.

The controller is usually still scanning when our image starts (a DFU load does not reset the
chip), so `lcd.c` reads back `TIMH/TIMV/POL/CTRL`, checks they describe 480x272, and keeps them —
changing only the framebuffer base, the pixel format and the palette. Otherwise it programs the
panel from cold with the pixel clock from the 12 MHz crystal (`BASE_LCD_CLK = 0x06<<24 | 1<<11`,
`POL` with BCD set). The backlight is left as found until the display timeout first takes it.

**The backlight line is `PD_16`** (not yet measured on hardware). The SCT configuration at
`0x28c6971c` maps `PD_13`, `PD_15`, `PD_14` and `PD_16` to `CTOUT_13`, `CTOUT_8`,
`CTOUT_11` and `CTOUT_12`. The first three are the LED rows (§4). `CTOUT_12` is the SCT's one
output on its independent high counter: a PWM, high for value/255 of each period. The value at
`0x28c69710` starts at `0x54`; its caller at `0x28c78e7a` supplies a 0–100% setting, with 2
for 0%. Its position outside the LED matrix identifies it as the backlight control.
`board_backlight()` sets the GPIO level before changing the mux, and nothing else in this image
touches the pin.

**The display timeout** (docs/05 §2) switches two things when the display goes dark, and back when it
lights. It drives `PD_16` low as `GPIO6[30]`, then high again, at full brightness rather than a PWM duty
(`board_backlight`); the pin is untouched until the first time the display goes dark. And it clears
`CTRL` bit 11, `LCDPWR`, then sets it (`lcd_set_power`), with `LCDEN`, the base and the format left
alone, so the controller keeps scanning; `lcd_init()` makes the same write on every start. Per
UM10503, `LCDPWR` low holds the data lines low and drops the controller's `LCD_PWR` output. The UI
also draws the dark screen black. Going dark, the backlight and the power go first; lighting, the
picture is redrawn first and they follow, so it returns whole.

Measured 2026-09-15, before `PD_16` was driven: the display went dark at the timeout, and with
`LCDPWR` clear and the screen drawn black the panel showed black with the backlight still on. So
`LCD_PWR` does not switch the backlight on this board.

Time for the timeout comes from SysTick, polled, with `TICKINT` clear so it never raises the
exception (`board_ms_start`, `board_ms_elapsed` in `board.c`). Its rate is the core clock,
read from `BASE_M4_CLK` and `PLL1_CTRL`: 204 MHz on PLL1 after the EMC stage, 12 MHz on the IRC or
the crystal. A source it cannot rate stops time, and the display never goes dark. SysTick wraps
every 82 ms at 204 MHz; a loop pass longer than that loses whole wraps, so the timeout runs long,
never short (derived).

A 1 bpp mode (16,320 bytes, palette entry 0 black / 1 white) fits in AHB SRAM and needs no SDRAM;
the firmware boots on it and moves to colour once SDRAM passes its test. **Changing format on a
running controller has an order that matters** (docs/09).

In colour the UI never draws into the buffer the controller scans. It draws into a back buffer at
`0x28100000` and each update copies only the rectangle it redrew into `0x28000000`
(`midi_control_gfx_copy`), so the panel never shows a region cleared and not yet redrawn. Mono
has no room for a second buffer in AHB SRAM and draws straight into the scanned one. Derived; not
yet seen on the unit.

## 6. USB

USB0 at `0x40006000`, ChipIdea/"transdimension" High-Speed device controller with on-chip PHY,
IRQ 8. TinyUSB's `dcd_ci_hs` port drives it. The boot ROM leaves PLL0USB, `BASE_USB0_CLK`, the PHY
and the CCU branch running, and **leaves interrupts masked** — `cpsie i` is mandatory. The verified
clock bring-up in `board_usb_clock_init()` is kept for a start where the ROM's state is not
inherited.

Factory identity, for recognising the unit on the bus: `VID_0E41&PID_4247`, composite: a vendor bulk
pipe (interface 0, WinUSB, the "message pipe" the editor and the DFU trigger use), UAC2 audio, and a
MIDI interface that never sends. Our identity is in docs/03.

## 7. Audio

Not used. The LPC's only audio peripheral is I2S0, a slave on a link the SHARC clocks, and that link
is the USB-audio path. Sending on it puts samples on the headphone output; this firmware never
enables I2S0 transmit. If a future version does, turn the volume down before loading it.

## 8. Everything else the pin map says

Peripheral pins this firmware never touches: SPIFI (`P3_3..P3_8`), SSP0 to
the SHARC, I2S0, the five jack-detect sense inputs. **Do not drive a pin whose net is unknown**; two
outputs in contention can damage something.

EMC (SDRAM) pins: `P1_0..2` and `P2_0,1,2,6` FUNC2; `P1_6..14`, `P2_7..13`, `P6_4,5,9..12`,
`PE_5..13` FUNC3; `P5_0..7` and `PD_0,2..9` FUNC2; `P6_8` FUNC1; `SFSCLK_0..3 = F5 F0 F5 F0`;
`EMCDELAYCLK = 0x5555`. **`P6_7`/`GPIO5[15]` must be driven low before the EMC is configured**
(the SDRAM does not answer without it). LCD pins: `P4_1..10` FUNC2 except `P4_7`
FUNC0, `P7_0..7` and `P8_3..7` FUNC3, `PB_0..6` FUNC2, `PF_0` FUNC1. SD/MMC: `PC_0` `0x77`,
`PC_4..7` and `PC_11..14` `0xF7` (eight data lines), `PC_10` `0xF7`, `PC_2` and `PC_9` `0x17`.
Hard reset: `PF_3`/`GPIO7[18]` driven low — works from a minimal image, **not** from this one
(measured); a power cycle is the reset.
