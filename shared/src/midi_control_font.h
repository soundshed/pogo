/* The stroke font's data, for midi_control_gfx.c. Not a public header.
 *
 * Each glyph is a handful of strokes - straight lines and circular arcs - on a
 * grid of font units with the capital height MIDI_CONTROL_FONT_CAP. y runs down:
 * capitals and ascenders from 0 to the baseline at 40, lower case from 12,
 * descenders to 52. Strokes are centre lines; the renderer gives them width
 * and round ends, so one outline serves every size and weight.
 */
#ifndef MIDI_CONTROL_FONT_H
#define MIDI_CONTROL_FONT_H

#include <stdint.h>

#define MIDI_CONTROL_FONT_CAP   40   /* capital height, font units */
#define MIDI_CONTROL_FONT_GAP   9    /* between the ink of adjacent glyphs, font units */

enum {
    MIDI_CONTROL_STROKE_LINE = 0,    /* a b to c d */
    MIDI_CONTROL_STROKE_ARC          /* centre a b, radius c, from direction d through e 64ths clockwise */
};

struct midi_control_font_stroke {
    uint8_t kind;
    int8_t a, b, c, d, e;
};

struct midi_control_font_glyph {
    const struct midi_control_font_stroke *strokes;
    uint8_t count;
    uint8_t width;                   /* centre-line width, font units */
};

/* The glyph for a character, or the space for one the font does not carry. */
const struct midi_control_font_glyph *midi_control_font_glyph(unsigned char c);

#endif
