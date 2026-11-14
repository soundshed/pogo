#ifndef MIDI_CONTROL_BOARD_H
#define MIDI_CONTROL_BOARD_H

#include <stdint.h>

/* A zero-initialised buffer that no DMA touches goes in local SRAM bank 2
 * (.bss_local, midi_control.ld), which leaves AHB SRAM for the USB and LCD DMA.
 * Nothing the USB controller reads or writes may carry this. */
#define MIDI_CONTROL_LOCAL __attribute__((section(".bss.local")))

#define BOARD_COLUMNS   7      /* COL1..COL7, all confirmed - see board.c */
#define BOARD_ROWS      3      /* SWROW1, SWROW2, SWROW3 */

#define BOARD_LED_COUNT BOARD_COLUMNS

/* One pass of the UI matrix.
 *
 * The encoders are multiplexed across the same column lines as the switches and
 * the LEDs - one encoder per column - so a single strobe collects everything
 * (docs/06). */
struct panel_state {
    /* bit (col * BOARD_ROWS + row) set when that switch is closed */
    uint32_t switches;
    /* two-bit quadrature code per column: bit 1 is ENC_A, bit 0 is ENC_B */
    uint8_t enc[BOARD_COLUMNS];
};

/* Bring up CCU1 CLK_M4_USB0, PLL0USB (480 MHz), BASE_USB0_CLK, the USB0 PHY,
 * and reset the USB0 controller into device mode.
 *
 * Returns 1 if every stage came up, 0 if any bounded poll timed out. Measured
 * on hardware: the boot ROM already leaves all of this configured for a
 * DFU-loaded image, so this is belt-and-braces there - but it is what a
 * SPIFI-booted image would need, and it is verified to work. See docs/10. */
int board_usb_clock_init(void);

/* Configure the seven column outputs, the three switch-row inputs and the two
 * encoder lines. */
void board_panel_init(void);

/* Strobe every column in turn, light its LED, and sample the rows and encoder
 * lines.
 *
 * `colours` is BOARD_COLUMNS bytes, one per column: bit 0 red, bit 1 green,
 * bit 2 blue - so 0 is off and 7 is white. Pass NULL for no LED output.
 *
 * The colour is driven only while that column is high, which is what keeps LED
 * output safe: exactly one column is ever driven at a time, so no closed switch
 * can bridge a high column to a low one. Do not drive the rows outside this
 * function.
 *
 * Columns drive high and the inputs have pull-downs, so closed reads as 1.
 * Undebounced and undecoded - the caller owns both. */
void board_panel_scan(struct panel_state *out, const uint8_t *colours);

#define LED_OFF     0u
#define LED_RED     1u
#define LED_GREEN   2u
#define LED_YELLOW  3u
#define LED_BLUE    4u
#define LED_MAGENTA 5u
#define LED_CYAN    6u
#define LED_WHITE   7u

/* Switches outside the column matrix, each on its own pin. Polarity differs
 * between them and is measured rather than derived - see k_direct in board.c.
 * Returns 1 when pressed, inversion already applied. Undebounced.
 *
 *   0  PEDAL_TOE_SW   P6_1  / GPIO3[0]
 *   1  PAGE           PD_10 / GPIO6[24], also the DFU boot strap
 *
 * The other page button is in the matrix; this one is not, which is why it sent
 * nothing until it was read here. */
#define BOARD_DIRECT_TOE   0
#define BOARD_DIRECT_PAGE  1
#define BOARD_DIRECT_COUNT 2

int board_direct_read(unsigned index);

/* The LCD backlight: PD_16 taken as GPIO6[30], driven high (on) or low (off).
 * The pin can also be driven as the CTOUT_12 PWM (board.c, docs/01 section 5).
 * Nothing touches the pin until the first call.
 *
 * The pedal's count follows this pin (docs/01 section 4) - the pedal is an IR
 * reflective sensor, and the pin most likely sets its emitter's drive - so
 * main() drives it high once at start-up and nothing ever drives it low. */
void board_backlight(int on);

/* Free-running burst conversion on all eight channels of both ADCs. */
void board_adc_init(void);

/* Latest 10-bit sample for one channel. adc is 0 or 1, channel 0..7. */
uint16_t board_adc_read(unsigned adc, unsigned channel);

/* Knob diagnostics (board.c): both converters' clock divider, never below the
 * default; one converter's burst on or off; and a converter's CR as it reads. */
void board_adc_set_clkdiv(unsigned clkdiv);
void board_adc_burst(unsigned adc, int on);
uint32_t board_adc_cr(unsigned adc);

void board_leds_init(void);              /* alias for board_panel_init */
void board_leds_set(uint32_t mask);      /* bit i -> column i driven high */
void board_delay_crude(uint32_t iterations);   /* ~20e6 iterations per second */

/* A millisecond clock: SysTick, polled, no interrupt (board.c). board_ms_start()
 * starts it and returns the core frequency it counts, or 0 when the clock
 * registers name a source it cannot rate, in which case no time ever passes.
 * board_ms_elapsed() returns the whole milliseconds since its previous call.
 * Called less often than SysTick wraps - 82 ms at 204 MHz - it counts short,
 * never long. */
uint32_t board_ms_start(void);
uint32_t board_ms_elapsed(void);
void board_reset(void);                        /* assert PF_3; does not return */

/* The analog-side outputs the stock board init drives and its board-output
 * setter later writes (parent project docs/09 section 9): bit 0 P1_17 (setter
 * index 0), 1 P8_0 (1), 2 P1_16 (2), 3 PF_9 (5), 4 P4_0 (6), 5 P8_2 MUTE (3),
 * 6 PF_8 AMPCTRL1 (7). What the first five switch is not established. Each pin
 * is driven to its bit's level, in the order level, direction, SFS.
 * board_analog_release() makes them inputs again, as the SCU and GPIO reset
 * leave them, which is what POGO has always done. */
#define BOARD_ANALOG_OUTPUTS 7u
void board_analog_drive(uint32_t mask);
void board_analog_release(void);       /* MUTE goes back to board_mute()'s setting, if any */

/* MUTE, P8_2 GPIO4[2]: low mutes the analog outputs, high lets them through,
 * as the stock firmware drives it (written inverted by its setter, index 3).
 * Left an input, as POGO did before 2026-11-14, it half mutes them: the Phones
 * are quiet and the level is lost (measured, docs/10 section 6). */
void board_mute(int muted);

/* Hand the unit to the boot ROM's USB DFU loader without the preceding reset
 * (board.c). Masks interrupts,
 * takes USB0 off the bus and the core off PLL1 first. Does not return: the
 * unit re-enumerates as 1FC9:000C. Flush anything the host should see before
 * calling it. */
void board_enter_dfu(void);

#endif
