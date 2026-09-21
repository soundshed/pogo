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

**A range that can only widen keeps the worst sample it ever saw.** The pedal's raw window used to
be seeded from the verified unit and widened by any sample outside it, never narrowing and never
stored. Three separate faults came out of that one rule, and all of them presented as "the pedal
lost its range" rather than as anything to do with firmware: a unit whose travel was narrower than
the seed could never reach 0 or 127 and no amount of sweeping fixed it; one transient sample at
power-on widened the window for the rest of the session; and every power cycle threw away whatever
had been learned. The window now widens only on samples that agree (`MIDI_CONTROL_CAL_CONFIRM`),
narrows onto the ends a real sweep reached, and is stored — docs/05 §6. Any running min/max over
samples from hardware wants all three.

**A rule that measures evidence against the thing in doubt deadlocks.** The first narrowing rule
asked that a sweep cover three quarters of the window before its ends were believed — but the
window is exactly what is wrong when the seed is another unit's. A pedal reaching only 470 against
the 280..664 seed covers half of it, so its ends were never believed, so the window never narrowed,
so the toe stop read 63 of 127 for as long as the unit was uncalibrated. It presented as "all the
way forward is half travel", and sweeping it — the one thing the design asked of the user — could
not fix it, because every sweep was measured against the same wrong window. The second rule
(docs/05 §6) uses evidence with no reference to the window at all: an end reached twice, to within
a few counts, with a crossing of the middle in between. Measured on the host, in
`test_analog_calibration`.

**The same deadlock survives in a stored window.** The repeated-end rule is deliberately off for a
stored calibration — a session that rocks the pedal over part of its travel must not overwrite the
whole — so a stored window that is too wide is measured against itself again. Seen on a unit on
2026-09-21 (CC#95): stored 249..977, a pedal reaching 584 at the toe and 977 at the heel, so the toe
read 69 of 127 — `(584 − 249) × 127 / 728`, inverted — and coverage needed 546 counts of a travel
of 393. Sweeping could not fix it and nothing short of `LOAD_DEFAULTS`, which takes the mapping
with it, would. How 249 got there is not known; the window is almost exactly the union of the
seed's 280..664 and the travel measured that day. Calibration by capture (docs/05 §6) is the way
out that asks nothing of any window: the user holds each stop, and the stops are the travel.

**The last counts of an approach are already inside the rest.** A stop taken as the middle of
everything the count dithered over since it came to rest includes the few counts of the way in that
fell within the rest window, and so lands short of the stop on the side the pedal came from — a
stop at 977 approached in 3-count steps was taken as 975. Found in the host tests before the unit
saw it; the capture now measures the stop over the second half of the hold only
(`test_analog_capture`).

**A hold on a switch that is also mapped fires in the middle of playing it.** The toe switch
starts calibration when held, and it also sends what it is mapped to, and a toe switch mapped to a
momentary action is held on purpose for as long as the effect is wanted. The hold is therefore
2 s, not the picker's 0.8 s, and the calibration it starts takes the pedal off the engine only while
it runs; Mode cancels it at once and nothing is changed. A player who holds a momentary toe switch
for more than 2 s will see the calibration screen: that is the trade, stated here rather than found
on stage.

**A save the user did not ask for must not commit edits the user did not commit.** The calibration
is written by `midi_control_device_tick()`, and it writes the whole configuration record because
that is the only verified way onto the card. It therefore defers while `dirty` is set, rather than
quietly making a half-finished edit in the app permanent.

**A switch released under a mapping it was not pressed under leaves a note sounding.** A profile
change replaces what every switch does (docs/05 §7), and Mode is *held* through the gesture that
does it — so its release would arrive under the new profile, end that profile's note or CC, and
leave the old profile's Note On with nothing to turn it off. It would present as a stuck note after
a profile change and look like a synth fault. `midi_control_device_select_profile()` therefore
releases every switch still down under the mapping it was pressed in, at the moment the profile
changes, and swallows the physical release when it comes. The bookkeeping for that is one bit per
switch (`picker_took`), and it is cleared by any press the engine does see, so a release that never
arrives — an `EVENT` press with no `EVENT` release — cannot swallow a later one and reintroduce the
fault it was added to prevent. `SET_CONFIG` has the same shape of hazard and does *not* do this:
nobody is standing on a footswitch while editing in the app, and taking the same care there would
mean sending MIDI in response to an edit.

**The state a screen shows has to be worked out where it is asked for, not cached beside it.** The
profile picker overrides six footswitch LEDs while it is open, and the seventh keeps the engine's.
A stored copy of that overlay would have had to be refreshed by every one of the dozen calls that
can change either the engine's LEDs or the picker's state, and the one that was forgotten would
have shown a stale LED. `midi_control_device_leds()` takes a buffer and computes the answer from
the state each time instead, which is why it takes one.

**A record that describes the same thing twice will eventually describe it differently.** The live
mapping and `profiles[profile_active]` are the same bytes in two places (docs/05 §7). They are kept
equal by a rule that runs in one direction at one moment — `sanitise()` copies live over the active
profile — and by `midi_control_config_write()` serialising the active profile from the live blocks
whatever the struct holds. Without the second of those, a caller that edited `cfg.buttons` and
serialised without sanitising first would have put a stale profile on the card; the round-trip test
caught exactly that.
