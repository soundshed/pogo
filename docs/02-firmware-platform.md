# 02 — Firmware platform

What the firmware inherits, what it must do before anything works, and how `firmware/` is put
together.

---

## 1. Two ways in, two different machine states

| | Boot ROM after a DFU load | NOR bootloader after loading our image from the eMMC |
|---|---|---|
| Core clock | ~96 MHz (PLL1 via IDIVC) | 204 MHz |
| Interrupts | masked (`PRIMASK`) | masked |
| SP | **not set** from vector word 0 | **not set**: still the bootloader's task stack, in SDRAM that we are about to reconfigure |
| SysTick | off | **running with TICKINT**; PendSV may be pending |
| LCD | still scanning the previous framebuffer | still scanning |
| EMC (SDRAM) | at power-on reset values, in self-refresh | configured by the bootloader; we reconfigure anyway |
| SD/MMC pin mux, `BASE_SDIO_CLK` | cleared / on the 12 MHz IRC | as the bootloader left them; we set them anyway |
| USB0 clocks and PHY | configured and running | configured |

`startup.c` handles the differences before `main()`:

1. `Reset_Handler` is assembly: `cpsid i`, load MSP from `_stack_top`, `CONTROL = 0`, then branch —
   nothing is pushed before SP is ours.
2. `reset_continue()` stops SysTick, clears every NVIC enable and pending bit, clears PendSV/SysTick
   pends, copies `.data`, zeroes `.bss`, sets `VTOR`, calls `main()`.

Skipping either hangs the installed image before the screen comes up while the DFU-loaded image
works perfectly. **Test the shipped path on the shipped path.**

## 2. Boot order in `main.c`

Each stage writes its name on the screen before it starts, because a stall in a bus access does
not fault — the core simply stops, and the last marker drawn is the only report.

| Stage | Does | Undone by power cycle |
|---|---|---|
| `BOOT` | panel pins, mono framebuffer in AHB SRAM, `lcd_init` (inherit or cold), a first draw | yes |
| — | `cpsie i` — nothing interrupt-driven works until this | — |
| `CLOCKS` | USB clock/PHY bring-up (harmless if already done), ADC burst mode | yes |
| `EMC` | drive `P6_7` low, PLL1 to 204 MHz, EMC pins and registers, JEDEC init, then a 1 MiB pattern test held 200 ms for refresh | yes |
| `COLOUR` | if SDRAM passed: re-point the LCD at `0x28000000` in RGB565, in the safe order; the UI draws off screen at `0x28100000` from here on | yes |
| `EMMC` | SD/MMC pins, `BASE_SDIO_CLK` from PLL1, controller reset, CMD0/1/2/3/9/7/16 at 400 kHz then 12.75 MHz, read sector 1 and classify it | yes |
| `CONFIG` | `midi_control_device_init`: read both slots, keep the newest valid, else defaults | yes |
| `USB` | 0.5 s with the pull-up off so the host drops the DFU device, then TinyUSB | yes |
| `RUN` | the loop | — |

The loop: `tud_task()` every iteration; every 2000 iterations the log drain, MIDI in, vendor in,
panel scan (with the engine's LED colours), direct switches, analog inputs, the milliseconds since
the last pass to `midi_control_device_tick()` for the display timeout (SysTick, started just before
`RUN`; docs/01 §5), and a display update:
`midi_control_ui_update()` redraws only the regions of the screen whose content changed, and the
rectangle it drew is copied to the panel (docs/01 §5, docs/08 §5). A turned encoder redraws its
80x58 dial; most passes redraw nothing.

## 3. The drivers

All of them were run on hardware in the parent project. Details that are not obvious:

**`board.c`** — panel matrix, direct switches, USB clock, ADC, delay, a millisecond clock from SysTick
(polled, docs/01 §5), and `board_enter_dfu()`, the way
into the boot ROM's DFU loader without a reset (docs/01 §3). `board_delay_crude()` is a
spin at ~20e6 iterations/s at the ROM's clock; after the EMC raises the core to 204 MHz it is ~2x
faster than its comments say, which only makes waits shorter and settle times still ample.

**`emc.c`** — `emc_init()` does the whole sequence: the enable line, PLL1 (park on IRC, enable
crystal, `PLL1_CTRL = 0x06100000`, wait lock, switch, then DIRECT), both EMC CCU branches, CREG6
bit 16 and `CLK_EMCDIV_CFG = 0x21` (half rate in **both** places, or the core stalls), pins,
`DYNAMICCONFIG0 = 0x4480`, `RASCAS = 0x202`, read config 1, timings, refresh 24 (conservative),
NOP → wait SA clear → PALL → MODE by a byte read from `0x28044000` → NORMAL → buffer enable.
`emc_test()` proves it with an address-tied pattern.

**`lcd.c`** — bring-up only; drawing is in `shared/`. See docs/01 §5 and docs/09 for the format
switch ordering.

**`emmc.c`** — polled FIFO transfers, no DMA, every wait bounded and returning a code. 1-bit bus at
12.75 MHz: reading the whole backed-up region (1365 sectors) takes a few seconds. `emmc_write()`
is CMD24 single-block with the busy wait; it does not verify — callers read back.

**`log.c` / `result.c`** — the diagnostic channel on the vendor bulk endpoint: a 16 KB non-wrapping
log that is replayed on request (command 126), and single-packet answers under 400 bytes
(command 93 re-sends). Rules in docs/03 §4.

**`storage.c`** — the two configuration slots; docs/06.

**`usb_descriptors.c`** — MIDI + vendor composite with MS OS 2.0 descriptors so Windows binds WinUSB
to the vendor interface without an INF; docs/03.

## 4. Memory layout (`midi_control.ld`)

```
CODE  0x10000000  128K   .isr_vector .text .rodata, and .data's load image; ends below 0x1001FF00
AHB   0x20000000   64K   .data .bss  (framebuffer 16 KB, log 16 KB, TinyUSB's DMA structures,
                                      the device: its configuration and two SysEx buffers)
stack 0x10092000         top of bank 2, set by Reset_Handler
      0x1001FF40         board_enter_dfu() copies its ROM calls here, clear of the next image
```

The linker script asserts both the image's end and that the copy fits in the bank.

`.bss` is asserted to lie in AHB SRAM: TinyUSB's queue heads are zero-initialised and land there.
Forcing them into a named section would make them loadable data and break the (NOLOAD) region.

**This 64 KB bank is the binding constraint on the configuration record.** `struct
midi_control_device` lives here and holds the record once plus two buffers of
`MIDI_CONTROL_SYSEX_MAX`, so a byte added to the record costs about 3.3 in this bank. With the
eight profiles of docs/05 §7, `.bss` ends at 0x2000cc80 and about 13 KB of the bank is free
(measured, 2026-09-21: `_bss_end` in `firmware/build/midi-control.map`). Check that figure before
raising `MIDI_CONTROL_PROFILE_COUNT` or adding another block — the linker's `ASSERT` is what fails
if it is wrong, and it fails at the link rather than on the unit.
`build.py` refuses a raw image over 128 KB, which is the signature of a section missing its
`AT>` and objcopy padding to `0x20000000`.

## 5. Build flags

`-mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -ffunction-sections -fdata-sections -fno-builtin
-nostdlib -nostartfiles -Wall -Wextra -Werror -std=gnu11`. `libc_shim.c` provides `memcpy`,
`memmove`, `memset`, `memcmp`, `strlen`, `strncpy` — GCC emits calls to these for struct copies
whatever the flags say. No heap, no `printf`, no floating point.

## 6. The image formats

`build.py` writes three files:

| File | Format | Used for |
|---|---|---|
| `midi-control.raw` | flat, link address `0x10000000` | input to the image builders |
| `midi-control.bin` | 16-byte NXP DFU header + body in 512-byte blocks | `dfu-load`, the app's step 2 |
| `midi-control-emmc.img` | sector 0 descriptor + persistent boot image (docs/06 §2) | `write-emmc <img> 0`, the app's step 4 |

`shared/src/midi_control_bootimage.c` builds the third from either of the first two and is byte-identical
to `tools/midi_control_boot_image.py` on the real image (checked).
