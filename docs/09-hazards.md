# 09 — Hazards

Failure signatures that do not resemble their causes. Each of these cost real time in the parent
project; the fix for each is in the code, and the entry here is so nobody removes it.

---

## On the unit

**Lights out, screen on, is a stalled core.** The LEDs are strobed by the main loop and go dark the
moment it stops; the LCD is refreshed by its own DMA from SDRAM and shows the last picture
indefinitely. Windows keeps reporting the device as `Status OK` from cached enumeration. Read the
stage marker at the bottom left of the screen: it names the stage that did not finish.

**Reading a register whose clock branch is off stalls the bus.** No fault, no report. Read the CCU
`CFG`/`STAT` pair first; log the register's name *before* the read so a stall leaves it as the last
log line. And read only named offsets: CCU1 `0x4B8`, guessed as an SSP0 clock, is a reserved hole
and stalls the part every time.

**The boot ROM leaves interrupts masked.** Without `cpsie i`, the USB interrupt never fires and
EP0 is silent while everything else looks healthy — the host even sees the attach. Windows says
"Device Descriptor Request Failed".

**Taking the unit into DFU as soon as it appears on the bus hands a half-initialised board to the
loaded image**, which stalls in the EMC stage with the screen reading `COLD`. Wait for the factory
system's normal screen, then 20 seconds more.

**An image proven under DFU can still hang when the bootloader starts it.** The bootloader branches
without setting SP (on a stack in the SDRAM we reconfigure) and leaves SysTick running; the first
`cpsie i` takes an inherited tick into a catch-all handler. `startup.c` fixes both; neither was
visible until the install.

**Re-locking a PLL the core is already running on is marginal — adopt it instead.** Measured
2026-09-14: a bootloader-started boot came up with the screen in mono (SDRAM test failed) *and*
"NO STORAGE" (eMMC init failed), and did not enumerate. SDRAM and the eMMC are the whole of what
runs off PLL1 at 204 MHz, so one shaky PLL1 bring-up takes both down together. The DFU path (which
arrives at ~96 MHz and locks PLL1 from a different state) was reliable every time; the failure was
specific to the bootloader entry, where PLL1 arrives *already* locked and driving the core and the
old `emc_clock_init()` tore it down to re-lock it. The fix in `emc.c`: **adopt an already-locked
204 MHz PLL1 rather than reprogramming it** (checked exactly, the way `lcd.c` adopts the running
panel), and on the cold path **verify the lock and retry — never switch the core to an unlocked
PLL1**, which the old bounded-poll-then-proceed code could do. `main.c` also retries the whole EMC
bring-up (clock included) a few times before the read-config sweep, since the sweep only varies the
capture strategy and cannot re-lock the clock. `g_emc_pll1_status` (in the boot log's `clocks`
block, and via the LCD status command) is 2 when adopted, 1 when cold-locked, 0 when it never
locked — the first thing to read if a boot is mono / NO STORAGE.

**Growing the LCD pixel format on a running controller runs its DMA off the old buffer and
latches the panel white** — with the core, USB and the scan all still running. `INTRAW` bit 4 set,
`UPCURR` frozen at `0x20010000`. `lcd_init()` orders the base move and the format change by which
way the size goes.

**`P6_7` low before the EMC**, and the EMC divider set in **both** CREG6 and `CLK_EMCDIV_CFG`.
Skip the first and the SDRAM never answers with every register perfect; mismatch the second and
the core stalls dead.

**Refresh that is too slow reads back perfectly and rots over milliseconds.** `emc_test()` re-reads
after 200 ms for exactly this.

**Holding a column high outside the scan** ghosts the switch matrix and shorts driven outputs
through closed switches. LED feedback goes inside the strobe only.

**A settle time of tens of microseconds sees no closed switch, ever.** Hundreds are needed.

**Un-primed baselines emit a phantom event from every control at startup**, into a host that is
not yet listening — which is why it goes unnoticed.

**`SCU_ENAIO` unset returns plausible ADC values for a pin connected to nothing.** And enabling
`ADC0_5` fights `ENC_A` on `PF_10`.

**The direct switches are the other polarity from the matrix**, and PAGE is the other polarity
from the toe switch. Wrong, a note sits on at rest and releases when pressed.

**A software reset does not re-enumerate the unit from a full image.** `SYSRESETREQ`, RGU reset
and the `PF_3` line all fail to bring it back. Only a power cycle. An image whose only sign of life
is a reset is indistinguishable from one that never ran. So POGO's way into DFU does not reset: it
hands the boot ROM its USB path directly (docs/01 §3).

## On the host

**A DFU download that reports success can leave the image not running.** The boot ROM accepts every
block and the zero-length `DFU_DNLOAD` that ends the transfer, then waits in `dfuMANIFEST-SYNC`
until the host sends `DFU_GETSTATUS`; that request is what starts the image. Observed 2026-09-14:
the app's port left it out, logged the download as complete, and detection still showed the boot
ROM afterwards. `tools/midi_control_usb.py` always sent it. The app now sends it, and counts a load
as done only once our firmware's interface enumerates.

**Closing the vendor handle mid-stream strands the transfer on the device.** The endpoint then
reads nothing, forever, until the LPC resets. Read until quiet before closing; treat a read window
as a budget, not a duration.

**A full 512-byte packet never completes a read.** USB ends a transfer on a *short* packet. The log
path caps writes at 511 bytes.

**One accepted write proves nothing.** The first lands in the peripheral's FIFO. Do two, or wait for
an answer.

**`winmm` wedges on a second opener and takes minutes to say so** (`MMSYSERR_NODRIVER` after
~5 minutes), and killing the holder does not clear it; only re-enumeration does. One MIDI process
per port. This is why the log is on the vendor endpoint and not on SysEx.

**Windows caches whether a device has MS OS descriptors** under VID/PID/`bcdDevice`. A layout change
without a `bcdDevice` bump gets the old cached answer, which looks like the descriptors being wrong.

**Git Bash rewrites POSIX-looking paths in a native program's arguments but not inside `python -c`
strings.** Use `C:/...` inside embedded source.

## In the code

**A hand-written CRC that disagrees with correct data has happened twice.** Neither storage path
gates on a checksum computed by the same code that did the reading: the write path reads every
sector back, the config record's CRC is checked against a known vector in the tests.

**A checksum computed by the code being tested proves less than an independent copy.** Backups are
verified by taking two and comparing, then comparing the saved file with what was read.

**Board-timed delays change with the clock.** `board_delay_crude()` is calibrated at the ROM's
~96 MHz and runs ~2x fast after PLL1 goes to 204 MHz. Every use of it is a minimum wait or a
settle, never a measured interval.
