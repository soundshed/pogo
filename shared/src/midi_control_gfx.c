#include <string.h>
#include "midi_control/midi_control_gfx.h"
#include "midi_control_font.h"

#define W MIDI_CONTROL_LCD_WIDTH
#define H MIDI_CONTROL_LCD_HEIGHT

void midi_control_gfx_init(struct midi_control_gfx *g, void *framebuffer, enum midi_control_gfx_format format,
                           int rotate180)
{
    g->fb = (uint8_t *)framebuffer;
    g->format = (uint8_t)format;
    g->rotate180 = rotate180 ? 1u : 0u;
    midi_control_gfx_set_clip(g, 0, 0, W, H);
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void midi_control_gfx_set_clip(struct midi_control_gfx *g, int x, int y, int w, int h)
{
    g->clip_x0 = (int16_t)clamp(x, 0, W);
    g->clip_y0 = (int16_t)clamp(y, 0, H);
    g->clip_x1 = (int16_t)clamp(x + (w > 0 ? w : 0), g->clip_x0, W);
    g->clip_y1 = (int16_t)clamp(y + (h > 0 ? h : 0), g->clip_y0, H);
}

/* ---- pixels ---------------------------------------------------------------- */

static int mono_lit(unsigned c)
{
    return ((c >> 11) & 31u) >= 12u || ((c >> 6) & 31u) >= 12u || (c & 31u) >= 12u;
}

/* (x, y) on the panel; no clipping. */
static void put(struct midi_control_gfx *g, int x, int y, unsigned colour)
{
    if (g->rotate180) {
        x = W - 1 - x;
        y = H - 1 - y;
    }
    if (g->format == MIDI_CONTROL_GFX_RGB565) {
        ((uint16_t *)(void *)g->fb)[y * W + x] = (uint16_t)colour;
        return;
    }
    uint8_t *p = &g->fb[y * MIDI_CONTROL_LCD_MONO_STRIDE + (x >> 3)];
    uint8_t mask = (uint8_t)(1u << (x & 7));
    if (mono_lit(colour))
        *p |= mask;
    else
        *p = (uint8_t)(*p & ~mask);
}

static uint16_t get(const struct midi_control_gfx *g, int x, int y)
{
    if (g->rotate180) {
        x = W - 1 - x;
        y = H - 1 - y;
    }
    if (g->format == MIDI_CONTROL_GFX_RGB565)
        return ((const uint16_t *)(const void *)g->fb)[y * W + x];
    return (g->fb[y * MIDI_CONTROL_LCD_MONO_STRIDE + (x >> 3)] >> (x & 7)) & 1u ? MIDI_CONTROL_WHITE
                                                                                : MIDI_CONTROL_BLACK;
}

static int in_clip(const struct midi_control_gfx *g, int x, int y)
{
    return x >= g->clip_x0 && x < g->clip_x1 && y >= g->clip_y0 && y < g->clip_y1;
}

void midi_control_gfx_pixel(struct midi_control_gfx *g, int x, int y, unsigned colour)
{
    if (in_clip(g, x, y))
        put(g, x, y, colour);
}

uint16_t midi_control_gfx_read(const struct midi_control_gfx *g, int x, int y)
{
    if (x < 0 || x >= W || y < 0 || y >= H)
        return 0;
    return get(g, x, y);
}

uint16_t midi_control_gfx_mix(unsigned a, unsigned b, unsigned amount)
{
    if (amount >= 256u)
        return (uint16_t)b;
    const unsigned keep = 256u - amount;
    const unsigned r = (((a >> 11) & 31u) * keep + ((b >> 11) & 31u) * amount + 128u) >> 8;
    const unsigned gr = (((a >> 5) & 63u) * keep + ((b >> 5) & 63u) * amount + 128u) >> 8;
    const unsigned bl = ((a & 31u) * keep + (b & 31u) * amount + 128u) >> 8;
    return (uint16_t)((r << 11) | (gr << 5) | bl);
}

/* In the clip already. */
static void blend_in(struct midi_control_gfx *g, int x, int y, unsigned colour, unsigned alpha)
{
    if (alpha == 0u)
        return;
    if (alpha >= 256u)
        put(g, x, y, colour);
    else if (g->format != MIDI_CONTROL_GFX_RGB565)
        put(g, x, y, alpha >= 128u ? colour : get(g, x, y));
    else
        put(g, x, y, midi_control_gfx_mix(get(g, x, y), colour, alpha));
}

void midi_control_gfx_blend(struct midi_control_gfx *g, int x, int y, unsigned colour, unsigned alpha)
{
    if (in_clip(g, x, y))
        blend_in(g, x, y, colour, alpha);
}

/* ---- rectangles -------------------------------------------------------------- */

void midi_control_gfx_clear(struct midi_control_gfx *g, unsigned colour)
{
    uint32_t *p = (uint32_t *)(void *)g->fb;
    uint32_t v;
    unsigned words;

    if (g->format == MIDI_CONTROL_GFX_RGB565) {
        v = ((uint32_t)(colour & 0xFFFFu) << 16) | (colour & 0xFFFFu);
        words = MIDI_CONTROL_LCD_RGB565_BYTES / 4;
    } else {
        v = mono_lit(colour) ? 0xFFFFFFFFu : 0u;
        words = MIDI_CONTROL_LCD_MONO_BYTES / 4;
    }
    for (unsigned i = 0; i < words; i++)
        p[i] = v;
}

void midi_control_gfx_fill_rect(struct midi_control_gfx *g, int x, int y, int w, int h, unsigned colour)
{
    int x0 = x > g->clip_x0 ? x : g->clip_x0;
    int y0 = y > g->clip_y0 ? y : g->clip_y0;
    int x1 = x + w < g->clip_x1 ? x + w : g->clip_x1;
    int y1 = y + h < g->clip_y1 ? y + h : g->clip_y1;

    if (x0 >= x1 || y0 >= y1)
        return;

    if (g->format != MIDI_CONTROL_GFX_RGB565) {
        for (int row = y0; row < y1; row++)
            for (int col = x0; col < x1; col++)
                put(g, col, row, colour);
        return;
    }

    /* A rectangle rotates to a rectangle, so the rotation costs nothing per pixel. */
    if (g->rotate180) {
        const int t = x0;
        x0 = W - x1;
        x1 = W - t;
        const int u = y0;
        y0 = H - y1;
        y1 = H - u;
    }
    uint16_t *fb = (uint16_t *)(void *)g->fb;
    for (int row = y0; row < y1; row++) {
        uint16_t *p = fb + row * W;
        for (int col = x0; col < x1; col++)
            p[col] = (uint16_t)colour;
    }
}

void midi_control_gfx_frame_rect(struct midi_control_gfx *g, int x, int y, int w, int h, unsigned colour)
{
    if (w <= 0 || h <= 0)
        return;
    midi_control_gfx_fill_rect(g, x, y, w, 1, colour);
    midi_control_gfx_fill_rect(g, x, y + h - 1, w, 1, colour);
    midi_control_gfx_fill_rect(g, x, y, 1, h, colour);
    midi_control_gfx_fill_rect(g, x + w - 1, y, 1, h, colour);
}

void midi_control_gfx_copy(struct midi_control_gfx *dst, const struct midi_control_gfx *src, int x, int y,
                           int w, int h)
{
    int x0 = clamp(x, 0, W), y0 = clamp(y, 0, H);
    int x1 = clamp(x + w, x0, W), y1 = clamp(y + h, y0, H);

    if (dst->format == MIDI_CONTROL_GFX_RGB565 && src->format == MIDI_CONTROL_GFX_RGB565
        && dst->rotate180 == src->rotate180) {
        if (src->rotate180) {
            const int t = x0;
            x0 = W - x1;
            x1 = W - t;
            const int u = y0;
            y0 = H - y1;
            y1 = H - u;
        }
        for (int row = y0; row < y1; row++)
            memcpy(dst->fb + (row * W + x0) * 2, src->fb + (row * W + x0) * 2, (size_t)(x1 - x0) * 2u);
        return;
    }
    for (int row = y0; row < y1; row++)
        for (int col = x0; col < x1; col++)
            put(dst, col, row, get(src, col, row));
}

/* ---- anti-aliased shapes ----------------------------------------------------- */
/* Lengths in 1/16 px. A shape is a centre line - a segment or an arc of at most
 * half a turn - and a half-width; a pixel's coverage comes from how far its
 * centre is from the centre line. Several shapes drawn in one pass take the
 * best coverage at each pixel, so strokes that meet do not double up. */

static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;

    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* r * dir / 4096, rounded, for either sign. */
static int32_t along(int32_t r, int dir)
{
    const int32_t v = r * dir;
    return v >= 0 ? (v + 2048) / 4096 : -((-v + 2048) / 4096);
}

static int floor16(int32_t v)
{
    return v >= 0 ? (int)(v / 16) : -(int)((-v + 15) / 16);
}

/* cos of the first quarter turn in 64ths, scaled by 4096. */
static const int16_t k_cos[17] = {
    4096, 4076, 4017, 3920, 3784, 3612, 3406, 3166, 2896,
    2598, 2276, 1931, 1567, 1189, 799, 401, 0,
};

int midi_control_gfx_cos(int direction)
{
    const int k = ((direction % 64) + 64) % 64;
    const int r = k % 16;
    switch (k / 16) {
    case 0:  return k_cos[r];
    case 1:  return -k_cos[16 - r];
    case 2:  return -k_cos[r];
    default: return k_cos[16 - r];
    }
}

int midi_control_gfx_sin(int direction)
{
    return midi_control_gfx_cos(direction - 16);
}

struct shape {
    uint8_t arc;
    int32_t ax, ay, bx, by;          /* the ends */
    int32_t vx, vy, len2, len;       /* segment: b - a */
    int32_t cx, cy, r;               /* arc */
    int32_t u0x, u0y, u1x, u1y;      /* arc: the two end directions, scaled by 4096 */
    int32_t hw;                      /* half the width */
    int x0, y0, x1, y1;              /* pixels it can touch; x1, y1 exclusive */
};

static void shape_bounds(struct shape *s, int32_t minx, int32_t miny, int32_t maxx, int32_t maxy)
{
    const int32_t m = s->hw + 16;
    s->x0 = floor16(minx - m);
    s->y0 = floor16(miny - m);
    s->x1 = floor16(maxx + m) + 1;
    s->y1 = floor16(maxy + m) + 1;
}

static void shape_segment(struct shape *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t hw)
{
    s->arc = 0;
    s->ax = x0;
    s->ay = y0;
    s->bx = x1;
    s->by = y1;
    s->vx = x1 - x0;
    s->vy = y1 - y0;
    s->len2 = s->vx * s->vx + s->vy * s->vy;
    s->len = (int32_t)isqrt((uint32_t)s->len2);
    if (s->len < 1)
        s->len = 1;
    s->hw = hw;
    shape_bounds(s, x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, x0 > x1 ? x0 : x1, y0 > y1 ? y0 : y1);
}

/* `sweep` 1..32. */
static void shape_arc(struct shape *s, int32_t cx, int32_t cy, int32_t r, int from, int sweep, int32_t hw)
{
    const int to = from + sweep;

    s->arc = 1;
    s->cx = cx;
    s->cy = cy;
    s->r = r;
    s->hw = hw;
    s->u0x = midi_control_gfx_cos(from);
    s->u0y = midi_control_gfx_sin(from);
    s->u1x = midi_control_gfx_cos(to);
    s->u1y = midi_control_gfx_sin(to);
    s->ax = cx + along(r, s->u0x);
    s->ay = cy + along(r, s->u0y);
    s->bx = cx + along(r, s->u1x);
    s->by = cy + along(r, s->u1y);

    int32_t minx = s->ax < s->bx ? s->ax : s->bx, maxx = s->ax > s->bx ? s->ax : s->bx;
    int32_t miny = s->ay < s->by ? s->ay : s->by, maxy = s->ay > s->by ? s->ay : s->by;
    /* The extremes of the circle the arc passes through. */
    for (int k = 0; k < 64; k += 16) {
        if (((k - from) % 64 + 64) % 64 > sweep)
            continue;
        if (k == 0)
            maxx = cx + r;
        else if (k == 16)
            maxy = cy + r;
        else if (k == 32)
            minx = cx - r;
        else
            miny = cy - r;
    }
    shape_bounds(s, minx, miny, maxx, maxy);
}

/* Distance from (px, py) to the shape's centre line, or `limit` if it is at
 * least that far. */
static int32_t shape_distance(const struct shape *s, int32_t px, int32_t py, int32_t limit)
{
    int32_t d2;

    if (s->arc) {
        const int32_t dx = px - s->cx, dy = py - s->cy;
        if (s->u0x * dy - s->u0y * dx >= 0 && dx * s->u1y - dy * s->u1x >= 0) {
            const int32_t q2 = dx * dx + dy * dy;
            const int32_t lo = s->r - limit, hi = s->r + limit;
            if (q2 >= hi * hi || (lo > 0 && q2 <= lo * lo))
                return limit;
            int32_t d = (int32_t)isqrt((uint32_t)q2) - s->r;
            if (d < 0)
                d = -d;
            return d < limit ? d : limit;
        }
        const int32_t ax = px - s->ax, ay = py - s->ay;
        const int32_t bx = px - s->bx, by = py - s->by;
        const int32_t da = ax * ax + ay * ay, db = bx * bx + by * by;
        d2 = da < db ? da : db;
    } else {
        const int32_t wx = px - s->ax, wy = py - s->ay;
        const int32_t t = wx * s->vx + wy * s->vy;
        if (t <= 0) {
            d2 = wx * wx + wy * wy;
        } else if (t >= s->len2) {
            const int32_t ex = px - s->bx, ey = py - s->by;
            d2 = ex * ex + ey * ey;
        } else {
            int32_t cross = wx * s->vy - wy * s->vx;
            if (cross < 0)
                cross = -cross;
            const int32_t d = cross / s->len;
            return d < limit ? d : limit;
        }
    }
    if (d2 >= limit * limit)
        return limit;
    return (int32_t)isqrt((uint32_t)d2);
}

/* Coverage 0..256 of a pixel whose centre is `d` from the middle of a stroke
 * `hw` either side of it: linear across the edge, and never more than the
 * stroke's width for a stroke under a pixel wide. */
static unsigned coverage(int32_t d, int32_t hw)
{
    int32_t c = (hw + 8 - d) * 16;
    const int32_t most = hw * 32 < 256 ? hw * 32 : 256;
    if (c <= 0)
        return 0;
    return (unsigned)(c < most ? c : most);
}

static void draw_shapes(struct midi_control_gfx *g, const struct shape *s, unsigned n, unsigned colour)
{
    int x0 = g->clip_x1, y0 = g->clip_y1, x1 = g->clip_x0, y1 = g->clip_y0;

    for (unsigned i = 0; i < n; i++) {
        x0 = s[i].x0 < x0 ? s[i].x0 : x0;
        y0 = s[i].y0 < y0 ? s[i].y0 : y0;
        x1 = s[i].x1 > x1 ? s[i].x1 : x1;
        y1 = s[i].y1 > y1 ? s[i].y1 : y1;
    }
    x0 = x0 > g->clip_x0 ? x0 : g->clip_x0;
    y0 = y0 > g->clip_y0 ? y0 : g->clip_y0;
    x1 = x1 < g->clip_x1 ? x1 : g->clip_x1;
    y1 = y1 < g->clip_y1 ? y1 : g->clip_y1;

    for (int y = y0; y < y1; y++) {
        const int32_t py = (int32_t)y * 16 + 8;
        for (int x = x0; x < x1; x++) {
            const int32_t px = (int32_t)x * 16 + 8;
            unsigned best = 0;
            for (unsigned i = 0; i < n && best < 256u; i++) {
                const struct shape *t = &s[i];
                if (x < t->x0 || x >= t->x1 || y < t->y0 || y >= t->y1)
                    continue;
                const unsigned c = coverage(shape_distance(t, px, py, t->hw + 8), t->hw);
                best = c > best ? c : best;
            }
            blend_in(g, x, y, colour, best);
        }
    }
}

void midi_control_gfx_line(struct midi_control_gfx *g, int x0, int y0, int x1, int y1, int width,
                           unsigned colour)
{
    struct shape s;
    shape_segment(&s, x0, y0, x1, y1, width / 2);
    draw_shapes(g, &s, 1, colour);
}

void midi_control_gfx_arc(struct midi_control_gfx *g, int cx, int cy, int r, int from, int sweep, int width,
                          unsigned colour)
{
    struct shape s[2];
    unsigned n = 0;

    if (sweep <= 0)
        return;
    if (sweep > 64)
        sweep = 64;
    if (sweep > 32) {
        shape_arc(&s[n++], cx, cy, r, from, 32, width / 2);
        from += 32;
        sweep -= 32;
    }
    shape_arc(&s[n++], cx, cy, r, from, sweep, width / 2);
    draw_shapes(g, s, n, colour);
}

void midi_control_gfx_fill_round_rect(struct midi_control_gfx *g, int x, int y, int w, int h, int radius,
                                      unsigned colour)
{
    int r = radius;

    if (w <= 0 || h <= 0)
        return;
    r = r < w / 2 ? r : w / 2;
    r = r < h / 2 ? r : h / 2;
    if (r <= 0) {
        midi_control_gfx_fill_rect(g, x, y, w, h, colour);
        return;
    }
    midi_control_gfx_fill_rect(g, x, y + r, w, h - 2 * r, colour);
    midi_control_gfx_fill_rect(g, x + r, y, w - 2 * r, r, colour);
    midi_control_gfx_fill_rect(g, x + r, y + h - r, w - 2 * r, r, colour);

    for (int corner = 0; corner < 4; corner++) {
        const int left = (corner & 1) == 0, top = (corner & 2) == 0;
        const int bx = left ? x : x + w - r, by = top ? y : y + h - r;
        const int32_t cx = (int32_t)(left ? x + r : x + w - r) * 16;
        const int32_t cy = (int32_t)(top ? y + r : y + h - r) * 16;
        for (int row = by; row < by + r; row++) {
            for (int col = bx; col < bx + r; col++) {
                if (!in_clip(g, col, row))
                    continue;
                const int32_t dx = (int32_t)col * 16 + 8 - cx, dy = (int32_t)row * 16 + 8 - cy;
                const int32_t d = (int32_t)isqrt((uint32_t)(dx * dx + dy * dy));
                const int32_t c = ((int32_t)r * 16 + 8 - d) * 16;
                blend_in(g, col, row, colour, c <= 0 ? 0u : c >= 256 ? 256u : (unsigned)c);
            }
        }
    }
}

/* ---- text ---------------------------------------------------------------------- */

#define MAX_GLYPH_SHAPES 24

int midi_control_gfx_text_weight(int size)
{
    return size * 2 + 6;
}

/* Font units to 1/16 px, rounded, for a glyph `span` 1/16 px tall at the centre line. */
static int32_t units(int u, int32_t span)
{
    const int32_t v = (int32_t)u * span;
    return v >= 0 ? (v + MIDI_CONTROL_FONT_CAP / 2) / MIDI_CONTROL_FONT_CAP
                  : -((-v + MIDI_CONTROL_FONT_CAP / 2) / MIDI_CONTROL_FONT_CAP);
}

static void style(int size, int *weight, int32_t *span)
{
    if (*weight <= 0)
        *weight = midi_control_gfx_text_weight(size);
    *span = (int32_t)size * 16 - *weight;
    if (*span < 16)
        *span = 16;
}

static void draw_glyph(struct midi_control_gfx *g, const struct midi_control_font_glyph *gl, int32_t ox, int32_t oy,
                       int32_t span, int weight, unsigned colour)
{
    struct shape s[MAX_GLYPH_SHAPES];
    unsigned n = 0;
    const int32_t hw = weight / 2;

    for (unsigned i = 0; i < gl->count && n + 2u <= MAX_GLYPH_SHAPES; i++) {
        const struct midi_control_font_stroke *k = &gl->strokes[i];
        if (k->kind == MIDI_CONTROL_STROKE_LINE) {
            shape_segment(&s[n++], ox + units(k->a, span), oy + units(k->b, span),
                          ox + units(k->c, span), oy + units(k->d, span), hw);
            continue;
        }
        int from = k->d, sweep = k->e > 64 ? 64 : k->e;
        const int32_t cx = ox + units(k->a, span), cy = oy + units(k->b, span), r = units(k->c, span);
        if (sweep > 32) {
            shape_arc(&s[n++], cx, cy, r, from, 32, hw);
            from += 32;
            sweep -= 32;
        }
        shape_arc(&s[n++], cx, cy, r, from, sweep, hw);
    }
    if (n)
        draw_shapes(g, s, n, colour);
}

static int32_t advance(const struct midi_control_font_glyph *gl, int32_t span, int weight)
{
    return units(gl->width, span) + weight;
}

int midi_control_gfx_text_width(int size, int weight, const char *s)
{
    int32_t span, pen = 0;

    style(size, &weight, &span);
    for (const char *p = s; *p; p++) {
        if (p != s)
            pen += units(MIDI_CONTROL_FONT_GAP, span);
        pen += advance(midi_control_font_glyph((unsigned char)*p), span, weight);
    }
    return (int)((pen + 15) / 16);
}

int midi_control_gfx_text(struct midi_control_gfx *g, int x, int y, int size, int weight, const char *s,
                          unsigned colour)
{
    int32_t span, pen = (int32_t)x * 16;
    const int32_t top = (int32_t)y * 16;

    style(size, &weight, &span);
    for (const char *p = s; *p; p++) {
        const struct midi_control_font_glyph *gl = midi_control_font_glyph((unsigned char)*p);
        if (p != s)
            pen += units(MIDI_CONTROL_FONT_GAP, span);
        const int32_t w = advance(gl, span, weight);
        /* Skip glyphs wholly outside the clip; descenders reach 1.3 capitals down. */
        if (floor16(pen + w) >= g->clip_x0 && floor16(pen) < g->clip_x1
            && y < g->clip_y1 && y + size * 3 / 2 >= g->clip_y0)
            draw_glyph(g, gl, pen + weight / 2, top + weight / 2, span, weight, colour);
        pen += w;
    }
    return (int)((pen + 15) / 16);
}
