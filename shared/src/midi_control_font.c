/* The stroke font: a geometric sans drawn from lines and circular arcs, so it
 * scales from status text to the program number without a bitmap per size.
 * An original design for this project. Grid and conventions in
 * midi_control_font.h; directions are 64ths of a turn clockwise from +x, so an
 * arc from 32 through 32 is the top half of its circle. */
#include <stddef.h>
#include "midi_control_font.h"

#define L(x0, y0, x1, y1)         { MIDI_CONTROL_STROKE_LINE, x0, y0, x1, y1, 0 }
#define A(cx, cy, r, from, sweep) { MIDI_CONTROL_STROKE_ARC, cx, cy, r, from, sweep }
#define DOT(x, y)                 L(x, y, x, y)

/* ---- figures ----------------------------------------------------------------- */

static const struct midi_control_font_stroke k_0[] = { A(11, 11, 11, 32, 32), L(22, 11, 22, 29), A(11, 29, 11, 0, 32), L(0, 29, 0, 11) };
static const struct midi_control_font_stroke k_1[] = { L(3, 8, 12, 0), L(12, 0, 12, 40) };
static const struct midi_control_font_stroke k_2[] = { A(11, 11, 11, 32, 40), L(19, 19, 0, 40), L(0, 40, 22, 40) };
static const struct midi_control_font_stroke k_3[] = { A(11, 10, 10, 36, 44), A(11, 30, 10, 48, 44) };
static const struct midi_control_font_stroke k_4[] = { L(16, 40, 16, 0), L(16, 0, 0, 28), L(0, 28, 22, 28) };
static const struct midi_control_font_stroke k_5[] = { L(20, 0, 2, 0), L(2, 0, 3, 21), A(11, 29, 11, 40, 48) };
static const struct midi_control_font_stroke k_6[] = { A(11, 29, 11, 0, 64), L(3, 21, 15, 1) };
static const struct midi_control_font_stroke k_7[] = { L(0, 0, 22, 0), L(22, 0, 7, 40) };
static const struct midi_control_font_stroke k_8[] = { A(11, 10, 9, 0, 64), A(11, 29, 11, 0, 64) };
static const struct midi_control_font_stroke k_9[] = { A(11, 11, 11, 0, 64), L(19, 19, 7, 39) };

/* ---- capitals ------------------------------------------------------------------ */

static const struct midi_control_font_stroke k_A[] = { L(0, 40, 12, 0), L(12, 0, 24, 40), L(4, 28, 20, 28) };
static const struct midi_control_font_stroke k_B[] = { L(0, 0, 0, 40), L(0, 0, 13, 0), A(13, 10, 10, 48, 32), L(13, 20, 0, 20),
                                                       L(0, 20, 14, 20), A(14, 30, 10, 48, 32), L(14, 40, 0, 40) };
static const struct midi_control_font_stroke k_C[] = { A(13, 13, 13, 32, 26), L(0, 13, 0, 27), A(13, 27, 13, 6, 26) };
static const struct midi_control_font_stroke k_D[] = { L(0, 0, 0, 40), L(0, 0, 11, 0), A(11, 13, 13, 48, 16), L(24, 13, 24, 27),
                                                       A(11, 27, 13, 0, 16), L(11, 40, 0, 40) };
static const struct midi_control_font_stroke k_E[] = { L(22, 0, 0, 0), L(0, 0, 0, 40), L(0, 40, 22, 40), L(0, 20, 18, 20) };
static const struct midi_control_font_stroke k_F[] = { L(22, 0, 0, 0), L(0, 0, 0, 40), L(0, 20, 18, 20) };
static const struct midi_control_font_stroke k_G[] = { A(13, 13, 13, 32, 26), L(0, 13, 0, 27), A(13, 27, 13, 0, 32),
                                                       L(26, 27, 26, 22), L(26, 22, 15, 22) };
static const struct midi_control_font_stroke k_H[] = { L(0, 0, 0, 40), L(24, 0, 24, 40), L(0, 20, 24, 20) };
static const struct midi_control_font_stroke k_I[] = { L(0, 0, 0, 40) };
static const struct midi_control_font_stroke k_J[] = { L(20, 0, 20, 30), A(10, 30, 10, 0, 32) };
static const struct midi_control_font_stroke k_K[] = { L(0, 0, 0, 40), L(22, 0, 0, 26), L(7, 18, 24, 40) };
static const struct midi_control_font_stroke k_L[] = { L(0, 0, 0, 40), L(0, 40, 20, 40) };
static const struct midi_control_font_stroke k_M[] = { L(0, 40, 0, 0), L(0, 0, 14, 26), L(14, 26, 28, 0), L(28, 0, 28, 40) };
static const struct midi_control_font_stroke k_N[] = { L(0, 40, 0, 0), L(0, 0, 24, 40), L(24, 40, 24, 0) };
static const struct midi_control_font_stroke k_O[] = { A(13, 13, 13, 32, 32), L(26, 13, 26, 27), A(13, 27, 13, 0, 32), L(0, 27, 0, 13) };
static const struct midi_control_font_stroke k_P[] = { L(0, 40, 0, 0), L(0, 0, 13, 0), A(13, 11, 11, 48, 32), L(13, 22, 0, 22) };
static const struct midi_control_font_stroke k_Q[] = { A(13, 13, 13, 32, 32), L(26, 13, 26, 27), A(13, 27, 13, 0, 32), L(0, 27, 0, 13),
                                                       L(16, 31, 27, 43) };
static const struct midi_control_font_stroke k_R[] = { L(0, 40, 0, 0), L(0, 0, 13, 0), A(13, 11, 11, 48, 32), L(13, 22, 0, 22),
                                                       L(12, 22, 24, 40) };
static const struct midi_control_font_stroke k_S[] = { A(11, 10, 10, 16, 42), A(11, 30, 10, 48, 42) };
static const struct midi_control_font_stroke k_T[] = { L(0, 0, 24, 0), L(12, 0, 12, 40) };
static const struct midi_control_font_stroke k_U[] = { L(0, 0, 0, 28), A(12, 28, 12, 0, 32), L(24, 28, 24, 0) };
static const struct midi_control_font_stroke k_V[] = { L(0, 0, 12, 40), L(12, 40, 24, 0) };
static const struct midi_control_font_stroke k_W[] = { L(0, 0, 8, 40), L(8, 40, 16, 10), L(16, 10, 24, 40), L(24, 40, 32, 0) };
static const struct midi_control_font_stroke k_X[] = { L(0, 0, 24, 40), L(24, 0, 0, 40) };
static const struct midi_control_font_stroke k_Y[] = { L(0, 0, 12, 20), L(24, 0, 12, 20), L(12, 20, 12, 40) };
static const struct midi_control_font_stroke k_Z[] = { L(0, 0, 24, 0), L(24, 0, 0, 40), L(0, 40, 24, 40) };

/* ---- lower case ---------------------------------------------------------------- */
/* The bowl of a, b, d, g, o, p, q: a circle of radius 12 on the x-height, its
 * sides stretched by 4 units. */
#define BOWL(x) A((x) + 12, 24, 12, 32, 32), L((x) + 24, 24, (x) + 24, 28), A((x) + 12, 28, 12, 0, 32), L(x, 28, x, 24)

static const struct midi_control_font_stroke k_a[] = { BOWL(0), L(24, 12, 24, 40) };
static const struct midi_control_font_stroke k_b[] = { L(0, 0, 0, 40), BOWL(0) };
static const struct midi_control_font_stroke k_c[] = { A(12, 24, 12, 32, 26), L(0, 24, 0, 28), A(12, 28, 12, 6, 26) };
static const struct midi_control_font_stroke k_d[] = { BOWL(0), L(24, 0, 24, 40) };
static const struct midi_control_font_stroke k_e[] = { A(12, 24, 12, 32, 32), L(24, 24, 24, 26), L(24, 26, 0, 26), L(0, 24, 0, 28),
                                                       A(12, 28, 12, 6, 26) };
static const struct midi_control_font_stroke k_f[] = { L(7, 40, 7, 8), A(15, 8, 8, 32, 24), L(0, 14, 16, 14) };
static const struct midi_control_font_stroke k_g[] = { BOWL(0), L(24, 12, 24, 42), A(14, 42, 10, 0, 26) };
static const struct midi_control_font_stroke k_h[] = { L(0, 0, 0, 40), A(12, 24, 12, 32, 32), L(24, 24, 24, 40) };
static const struct midi_control_font_stroke k_i[] = { L(0, 12, 0, 40), DOT(0, 2) };
static const struct midi_control_font_stroke k_j[] = { L(10, 12, 10, 44), A(3, 44, 7, 0, 24), DOT(10, 2) };
static const struct midi_control_font_stroke k_k[] = { L(0, 0, 0, 40), L(20, 12, 0, 30), L(8, 23, 22, 40) };
static const struct midi_control_font_stroke k_l[] = { L(0, 0, 0, 34), A(6, 34, 6, 16, 16) };
static const struct midi_control_font_stroke k_m[] = { L(0, 12, 0, 40), A(9, 21, 9, 32, 32), L(18, 21, 18, 40), A(27, 21, 9, 32, 32),
                                                       L(36, 21, 36, 40) };
static const struct midi_control_font_stroke k_n[] = { L(0, 12, 0, 40), A(12, 24, 12, 32, 32), L(24, 24, 24, 40) };
static const struct midi_control_font_stroke k_o[] = { BOWL(0) };
static const struct midi_control_font_stroke k_p[] = { L(0, 12, 0, 52), BOWL(0) };
static const struct midi_control_font_stroke k_q[] = { BOWL(0), L(24, 12, 24, 52) };
static const struct midi_control_font_stroke k_r[] = { L(0, 12, 0, 40), A(12, 24, 12, 32, 20) };
static const struct midi_control_font_stroke k_s[] = { A(9, 19, 7, 16, 42), A(9, 33, 7, 48, 42) };
static const struct midi_control_font_stroke k_t[] = { L(7, 0, 7, 34), A(13, 34, 6, 16, 16), L(0, 12, 16, 12) };
static const struct midi_control_font_stroke k_u[] = { L(0, 12, 0, 28), A(12, 28, 12, 0, 32), L(24, 12, 24, 40) };
static const struct midi_control_font_stroke k_v[] = { L(0, 12, 11, 40), L(11, 40, 22, 12) };
static const struct midi_control_font_stroke k_w[] = { L(0, 12, 8, 40), L(8, 40, 16, 18), L(16, 18, 24, 40), L(24, 40, 32, 12) };
static const struct midi_control_font_stroke k_x[] = { L(0, 12, 22, 40), L(22, 12, 0, 40) };
static const struct midi_control_font_stroke k_y[] = { L(0, 12, 11, 38), L(22, 12, 5, 52) };
static const struct midi_control_font_stroke k_z[] = { L(0, 12, 22, 12), L(22, 12, 0, 40), L(0, 40, 22, 40) };

/* ---- punctuation --------------------------------------------------------------- */

static const struct midi_control_font_stroke k_excl[] = { L(0, 0, 0, 28), DOT(0, 40) };
static const struct midi_control_font_stroke k_dquote[] = { L(0, 0, 0, 10), L(8, 0, 8, 10) };
static const struct midi_control_font_stroke k_hash[] = { L(8, 4, 4, 36), L(18, 4, 14, 36), L(2, 14, 22, 14), L(0, 26, 20, 26) };
static const struct midi_control_font_stroke k_dollar[] = { A(11, 11, 9, 16, 42), A(11, 29, 9, 48, 42), L(11, -3, 11, 43) };
static const struct midi_control_font_stroke k_percent[] = { A(5, 7, 5, 0, 64), A(19, 33, 5, 0, 64), L(22, 0, 2, 40) };
static const struct midi_control_font_stroke k_amp[] = { A(11, 9, 7, 0, 64), L(6, 14, 25, 40), A(11, 29, 11, 56, 48), L(3, 21, 16, 14) };
static const struct midi_control_font_stroke k_quote[] = { L(0, 0, 0, 10) };
static const struct midi_control_font_stroke k_lparen[] = { A(24, 20, 24, 21, 22) };
static const struct midi_control_font_stroke k_rparen[] = { A(-12, 20, 24, 53, 22) };
static const struct midi_control_font_stroke k_star[] = { L(6, 0, 6, 14), L(0, 3, 12, 11), L(12, 3, 0, 11) };
static const struct midi_control_font_stroke k_plus[] = { L(0, 24, 20, 24), L(10, 14, 10, 34) };
static const struct midi_control_font_stroke k_comma[] = { L(2, 38, 0, 46) };
static const struct midi_control_font_stroke k_minus[] = { L(0, 24, 14, 24) };
static const struct midi_control_font_stroke k_period[] = { DOT(0, 40) };
static const struct midi_control_font_stroke k_slash[] = { L(16, -2, 0, 42) };
static const struct midi_control_font_stroke k_colon[] = { DOT(0, 16), DOT(0, 40) };
static const struct midi_control_font_stroke k_semi[] = { DOT(2, 16), L(2, 38, 0, 46) };
static const struct midi_control_font_stroke k_less[] = { L(18, 10, 0, 24), L(0, 24, 18, 38) };
static const struct midi_control_font_stroke k_equal[] = { L(0, 18, 18, 18), L(0, 30, 18, 30) };
static const struct midi_control_font_stroke k_greater[] = { L(0, 10, 18, 24), L(18, 24, 0, 38) };
static const struct midi_control_font_stroke k_question[] = { A(10, 10, 10, 32, 40), L(17, 17, 10, 25), L(10, 25, 10, 29), DOT(10, 40) };
static const struct midi_control_font_stroke k_at[] = { A(15, 22, 15, 10, 56), A(15, 22, 6, 0, 64), L(21, 16, 21, 26), A(25, 26, 4, 16, 16) };
static const struct midi_control_font_stroke k_lbracket[] = { L(8, -2, 0, -2), L(0, -2, 0, 42), L(0, 42, 8, 42) };
static const struct midi_control_font_stroke k_backslash[] = { L(0, -2, 16, 42) };
static const struct midi_control_font_stroke k_rbracket[] = { L(0, -2, 8, -2), L(8, -2, 8, 42), L(8, 42, 0, 42) };
static const struct midi_control_font_stroke k_caret[] = { L(0, 12, 10, 0), L(10, 0, 20, 12) };
static const struct midi_control_font_stroke k_under[] = { L(0, 46, 24, 46) };
static const struct midi_control_font_stroke k_grave[] = { L(0, 0, 6, 6) };
static const struct midi_control_font_stroke k_lbrace[] = { L(10, -2, 5, -2), L(5, -2, 5, 17), L(5, 17, 0, 20), L(0, 20, 5, 23),
                                                            L(5, 23, 5, 42), L(5, 42, 10, 42) };
static const struct midi_control_font_stroke k_bar[] = { L(0, -2, 0, 42) };
static const struct midi_control_font_stroke k_rbrace[] = { L(0, -2, 5, -2), L(5, -2, 5, 17), L(5, 17, 10, 20), L(10, 20, 5, 23),
                                                            L(5, 23, 5, 42), L(5, 42, 0, 42) };
static const struct midi_control_font_stroke k_tilde[] = { L(0, 26, 6, 20), L(6, 20, 12, 26), L(12, 26, 18, 20) };

/* ---- symbols ------------------------------------------------------------------- */

static const struct midi_control_font_stroke k_sym_up[] = { L(10, 12, 0, 32), L(10, 12, 20, 32), L(0, 32, 20, 32),
                                                            L(10, 14, 10, 32), L(5, 26, 15, 26), L(7, 20, 13, 20) };
static const struct midi_control_font_stroke k_sym_down[] = { L(10, 32, 0, 12), L(10, 32, 20, 12), L(0, 12, 20, 12),
                                                              L(10, 30, 10, 12), L(5, 18, 15, 18), L(7, 24, 13, 24) };
static const struct midi_control_font_stroke k_sym_dot[] = { A(8, 24, 4, 0, 64), DOT(8, 24) };

#define G(k, width) { k, (uint8_t)(sizeof(k) / sizeof(k[0])), width }

static const struct midi_control_font_glyph k_space = { NULL, 0, 12 };

/* 0x21..0x60, then 0x7B..0x7E. */
static const struct midi_control_font_glyph k_ascii[] = {
    G(k_excl, 0), G(k_dquote, 8), G(k_hash, 22), G(k_dollar, 22), G(k_percent, 24), G(k_amp, 24),
    G(k_quote, 0), G(k_lparen, 12), G(k_rparen, 12), G(k_star, 12), G(k_plus, 20), G(k_comma, 2),
    G(k_minus, 14), G(k_period, 0), G(k_slash, 16),
    G(k_0, 22), G(k_1, 22), G(k_2, 22), G(k_3, 22), G(k_4, 22), G(k_5, 22), G(k_6, 22), G(k_7, 22),
    G(k_8, 22), G(k_9, 22),
    G(k_colon, 0), G(k_semi, 2), G(k_less, 18), G(k_equal, 18), G(k_greater, 18), G(k_question, 20),
    G(k_at, 30),
    G(k_A, 24), G(k_B, 24), G(k_C, 25), G(k_D, 24), G(k_E, 22), G(k_F, 22), G(k_G, 26), G(k_H, 24),
    G(k_I, 0), G(k_J, 20), G(k_K, 24), G(k_L, 20), G(k_M, 28), G(k_N, 24), G(k_O, 26), G(k_P, 24),
    G(k_Q, 27), G(k_R, 24), G(k_S, 22), G(k_T, 24), G(k_U, 24), G(k_V, 24), G(k_W, 32), G(k_X, 24),
    G(k_Y, 24), G(k_Z, 24),
    G(k_lbracket, 8), G(k_backslash, 16), G(k_rbracket, 8), G(k_caret, 20), G(k_under, 24), G(k_grave, 6),
};

static const struct midi_control_font_glyph k_lower[] = {
    G(k_a, 24), G(k_b, 24), G(k_c, 23), G(k_d, 24), G(k_e, 24), G(k_f, 18), G(k_g, 24), G(k_h, 24),
    G(k_i, 0), G(k_j, 10), G(k_k, 22), G(k_l, 6), G(k_m, 36), G(k_n, 24), G(k_o, 24), G(k_p, 24),
    G(k_q, 24), G(k_r, 17), G(k_s, 18), G(k_t, 17), G(k_u, 24), G(k_v, 22), G(k_w, 32), G(k_x, 22),
    G(k_y, 22), G(k_z, 22),
    G(k_lbrace, 10), G(k_bar, 0), G(k_rbrace, 10), G(k_tilde, 18),
};

static const struct midi_control_font_glyph k_symbols[] = {
    G(k_sym_up, 20), G(k_sym_down, 20), G(k_sym_dot, 16),
};

const struct midi_control_font_glyph *midi_control_font_glyph(unsigned char c)
{
    if (c >= 0x01 && c <= 0x03)
        return &k_symbols[c - 0x01];
    if (c >= 0x21 && c <= 0x60)
        return &k_ascii[c - 0x21];
    if (c >= 0x61 && c <= 0x7E)
        return &k_lower[c - 0x61];
    return &k_space;
}
