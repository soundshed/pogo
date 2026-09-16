/* Framebuffer drawing for the 480x272 panel, in the two pixel formats the
 * firmware uses. Pure memory: the firmware points it at a buffer in SDRAM, the
 * emulator at a buffer it then paints into a window, and both get the same
 * pixels.
 *
 * Formats:
 *   MIDI_CONTROL_GFX_RGB565  16 bpp 5:6:5, red in the high bits, 261,120 bytes.
 *                            The panel's native format; needs SDRAM on the unit.
 *   MIDI_CONTROL_GFX_MONO    1 bpp, 16,320 bytes, bit 0 of each byte is the leftmost
 *                            pixel. The firmware's fallback while SDRAM is not up.
 *
 * The panel is fitted 180 degrees round in the chassis, so the firmware sets
 * `rotate180` and every pixel lands where the eye expects; the emulator draws
 * the buffer as-is with rotation off. (docs/01-hardware.md section 5.)
 *
 * Shapes are anti-aliased: coverage is worked out per pixel in integer 1/16
 * pixel units and blended with what is already in the buffer. Coordinates of
 * lines, arcs and discs are in those units (MIDI_CONTROL_Q4); rectangles and
 * text are in whole pixels. Nothing uses floating point.
 */
#ifndef MIDI_CONTROL_GFX_H
#define MIDI_CONTROL_GFX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_CONTROL_LCD_WIDTH        480
#define MIDI_CONTROL_LCD_HEIGHT       272
#define MIDI_CONTROL_LCD_MONO_STRIDE  (MIDI_CONTROL_LCD_WIDTH / 8)
#define MIDI_CONTROL_LCD_MONO_BYTES   (MIDI_CONTROL_LCD_MONO_STRIDE * MIDI_CONTROL_LCD_HEIGHT)
#define MIDI_CONTROL_LCD_RGB565_BYTES (MIDI_CONTROL_LCD_WIDTH * MIDI_CONTROL_LCD_HEIGHT * 2)

enum midi_control_gfx_format {
    MIDI_CONTROL_GFX_MONO = 0,
    MIDI_CONTROL_GFX_RGB565
};

#define MIDI_CONTROL_RGB(r, g, b)                          \
    ((uint16_t)((((unsigned)(r) & 0xF8u) << 8)             \
              | (((unsigned)(g) & 0xFCu) << 3)             \
              | (((unsigned)(b) & 0xF8u) >> 3)))

#define MIDI_CONTROL_BLACK   MIDI_CONTROL_RGB(0, 0, 0)
#define MIDI_CONTROL_WHITE   MIDI_CONTROL_RGB(255, 255, 255)
#define MIDI_CONTROL_RED     MIDI_CONTROL_RGB(255, 0, 0)
#define MIDI_CONTROL_GREEN   MIDI_CONTROL_RGB(0, 255, 0)
#define MIDI_CONTROL_BLUE    MIDI_CONTROL_RGB(0, 0, 255)
#define MIDI_CONTROL_YELLOW  MIDI_CONTROL_RGB(255, 255, 0)
#define MIDI_CONTROL_CYAN    MIDI_CONTROL_RGB(0, 255, 255)
#define MIDI_CONTROL_MAGENTA MIDI_CONTROL_RGB(255, 0, 255)
#define MIDI_CONTROL_GREY    MIDI_CONTROL_RGB(128, 128, 128)
#define MIDI_CONTROL_DIM     MIDI_CONTROL_RGB(64, 64, 64)

/* Whole pixels to the 1/16 pixel units of lines, arcs and discs. A pixel's
 * centre is MIDI_CONTROL_Q4(x) + 8. */
#define MIDI_CONTROL_Q4(px)  ((px) * 16)

/* Directions for arcs, in 64ths of a turn clockwise on the panel from +x:
 * 0 right, 16 down, 32 left, 48 up. */
#define MIDI_CONTROL_TURN    64

struct midi_control_gfx {
    uint8_t *fb;
    uint8_t format;      /* enum midi_control_gfx_format */
    uint8_t rotate180;
    int16_t clip_x0, clip_y0, clip_x1, clip_y1;   /* drawable area; x1, y1 exclusive */
};

struct midi_control_rect {
    int16_t x, y, w, h;
};

/* The clip starts as the whole panel. */
void midi_control_gfx_init(struct midi_control_gfx *g, void *framebuffer, enum midi_control_gfx_format format,
                           int rotate180);

/* Restrict every drawing call, text included, to a rectangle (clamped to the
 * panel). midi_control_gfx_clear() is the one call that ignores it. */
void midi_control_gfx_set_clip(struct midi_control_gfx *g, int x, int y, int w, int h);

/* In RGB565 a colour is a MIDI_CONTROL_RGB() value. In mono a colour is lit when
 * any channel is at 40% or more, so bright ink on a dark ground and dark ink on
 * a bright ground both survive, and drawing code is written once for both. */
void midi_control_gfx_clear(struct midi_control_gfx *g, unsigned colour);
void midi_control_gfx_pixel(struct midi_control_gfx *g, int x, int y, unsigned colour);

/* `alpha` 0 (nothing) .. 256 (opaque). In mono, half coverage or more paints. */
void midi_control_gfx_blend(struct midi_control_gfx *g, int x, int y, unsigned colour, unsigned alpha);

/* A colour `amount`/256 of the way from `a` to `b`. */
uint16_t midi_control_gfx_mix(unsigned a, unsigned b, unsigned amount);

void midi_control_gfx_fill_rect(struct midi_control_gfx *g, int x, int y, int w, int h, unsigned colour);
void midi_control_gfx_frame_rect(struct midi_control_gfx *g, int x, int y, int w, int h, unsigned colour);
void midi_control_gfx_fill_round_rect(struct midi_control_gfx *g, int x, int y, int w, int h, int radius,
                                      unsigned colour);

/* A line `width` wide (1/16 px) with round ends; equal ends draw a disc. */
void midi_control_gfx_line(struct midi_control_gfx *g, int x0, int y0, int x1, int y1, int width,
                           unsigned colour);

/* A circular arc of radius `r` about (cx, cy), from direction `from` clockwise
 * through `sweep` 64ths of a turn (1..64), `width` wide with round ends. All
 * lengths in 1/16 px. */
void midi_control_gfx_arc(struct midi_control_gfx *g, int cx, int cy, int r, int from, int sweep, int width,
                          unsigned colour);

/* cos and sin of a direction, scaled by 4096. */
int midi_control_gfx_cos(int direction);
int midi_control_gfx_sin(int direction);

/* Read a pixel back as RGB565 (mono is expanded to black / white), for the
 * emulator's painter. Honours the rotation, so the caller sees the panel the
 * right way up. */
uint16_t midi_control_gfx_read(const struct midi_control_gfx *g, int x, int y);

/* Copy a rectangle of the panel from one buffer to another: how the firmware
 * shows a picture drawn off screen without the panel ever scanning a half-drawn
 * one. Either buffer may be in either format and rotation; the panel
 * coordinates are what match. Ignores the clip. */
void midi_control_gfx_copy(struct midi_control_gfx *dst, const struct midi_control_gfx *src, int x, int y,
                           int w, int h);

/* ---- text ------------------------------------------------------------------ */
/* One stroke font (midi_control_font.c), drawn at any size: `size` is the
 * capital height in pixels, and the ink of a capital fills exactly that from y
 * down. Printable ASCII 0x20..0x7E, plus the symbols below; anything else draws
 * as a blank. `weight` is the stroke width in 1/16 px, 0 for the size's own. */

#define MIDI_CONTROL_SYM_UP      "\x01"    /* filled triangles, for steps */
#define MIDI_CONTROL_SYM_DOWN    "\x02"
#define MIDI_CONTROL_SYM_DOT     "\x03"    /* a filled circle */

int midi_control_gfx_text(struct midi_control_gfx *g, int x, int y, int size, int weight, const char *s,
                          unsigned colour);
int midi_control_gfx_text_width(int size, int weight, const char *s);

/* The stroke width `weight` 0 stands for at a given size. */
int midi_control_gfx_text_weight(int size);

#ifdef __cplusplus
}
#endif
#endif
