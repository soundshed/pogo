#ifndef MIDI_CONTROL_LCD_H
#define MIDI_CONTROL_LCD_H

#include <stdint.h>
#include "midi_control/midi_control_gfx.h"

/* The LCD controller bring-up. Drawing lives in shared/ (midi_control_gfx.h,
 * midi_control_ui.h) so the emulator renders the same pixels; this file only points
 * the controller at a buffer in the right format.
 *
 * Two pixel formats, because the panel has to work whether or not SDRAM does:
 * MIDI_CONTROL_GFX_RGB565 is the real one (261,120 bytes, SDRAM only) and
 * MIDI_CONTROL_GFX_MONO is the fallback (16,320 bytes, fits in AHB SRAM and needs no
 * EMC at all). docs/01-hardware.md section 5. */

/* Bring the LCD controller up and start scanning `framebuffer` in `format`.
 *
 * The buffer must be 8-byte aligned and reachable by the LCD controller's DMA:
 * AHB SRAM at 0x20000000 for mono, SDRAM for RGB565. Local SRAM at 0x10000000
 * will not work. */
void lcd_init(void *framebuffer, enum midi_control_gfx_format format);

/* The same, but programming the panel from scratch even when a configuration
 * is already running: pin mux, the pixel clock from the crystal, timings. */
void lcd_init_cold(void *framebuffer, enum midi_control_gfx_format format);

/* The panel's power output for the display timeout: CTRL's LCDPWR bit and
 * nothing else. The controller stays enabled, scanning the same buffer in the
 * same format, so setting it again undoes clearing it. Cleared, the data lines
 * are held low (UM10503); what LCD_PWR switches on this board is not measured
 * (docs/01 section 5). Setting it is refused unless LCDEN is set, the order the
 * controller's power-up needs. */
void lcd_set_power(int on);

/* 0 if lcd_init() adopted the panel setup it found already running (the normal
 * case for a DFU-loaded image), 1 if it had to program the panel from scratch. */
int lcd_was_cold_started(void);

int lcd_format_is_colour(void);

/* Raw read of one LCD controller register, by byte offset from its base.
 * UPBASE (0x10), CTRL (0x18), INTRAW (0x20) and UPCURR (0x2C) are the ones
 * that tell a frozen picture from a refreshed one. */
uint32_t lcd_reg(unsigned offset);

#endif
