/* Short answers, one packet each. See result.h. */

#include <stdint.h>
#include "result.h"
#include "tusb.h"

/* Comfortably under the endpoint's 512 bytes, so what goes out is always a
 * short packet and therefore always terminates the host's read by itself. That
 * property is the entire point of this file, so the size is a deliberate
 * choice rather than a round number: leave room for the tag, a dozen or so
 * name=value pairs and a verdict, and stop well before the limit that would
 * turn a result back into a stream. */
#define RESULT_MAX 400u

/* The last RESULT_MARK_LEN bytes of the buffer belong to the marker and to
 * nothing else, so a full buffer still has room to say it is full. Without
 * that reservation the marker is written into whatever single byte the body
 * left, and the answer ends in a bare 'T' - which reads as a field name, not
 * as a warning. Observed on a unit: an adc snapshot whose last line was
 * two characters of a field name and then a lone 'T', with the field
 * itself gone and nothing left to say so. */
#define RESULT_MARK     "TRUNCATED\n"
#define RESULT_MARK_LEN (sizeof(RESULT_MARK) - 1u)

/* What the caller actually gets to fill: 390 bytes.
 *
 * The header costs 4 + strlen(tag). A result_put() costs strlen(name) + 10
 * (the '=', eight hex digits, the newline). A result_note() costs its length
 * capped at RESULT_STR_MAX, plus the newline. So a three-character tag leaves
 * room for about 29 three-character fields, or 27 four-character ones, and a
 * verdict needs one of them back.
 *
 * adc_snapshot() in main.c wants 411 and is the one caller over the line; it
 * now says so instead of stopping mid-field. */
#define RESULT_BODY_MAX (RESULT_MAX - RESULT_MARK_LEN)

/* A string argument is bounded rather than trusted, because a bad pointer here
 * would be read until it happened to hit a zero. */
#define RESULT_STR_MAX 40u

_Static_assert(RESULT_BODY_MAX + RESULT_MARK_LEN == RESULT_MAX,
               "the marker must fit in the buffer without displacing the body");

static char g_buf[RESULT_MAX];
static unsigned g_len;
static unsigned g_full;

static void put(char c)
{
    if (g_len < RESULT_BODY_MAX)
        g_buf[g_len++] = c;
    else
        g_full = 1;
}

static void put_str(const char *s)
{
    unsigned n = 0;

    for (; *s && n < RESULT_STR_MAX; s++, n++)
        put(*s);

    /* Hitting the cap drops characters just as surely as filling the buffer
     * does, so it is the same kind of lie and gets the same marker. */
    if (*s)
        g_full = 1;
}

void result_begin(const char *tag)
{
    g_len = 0;
    g_full = 0;
    put_str("== ");
    put_str(tag);
    put('\n');
}

void result_put(const char *name, uint32_t value)
{
    put_str(name);
    put('=');
    for (int i = 7; i >= 0; i--) {
        unsigned nib = (value >> (i * 4)) & 0xFu;
        put((char)(nib < 10u ? '0' + nib : 'A' + nib - 10u));
    }
    put('\n');
}

void result_note(const char *text)
{
    put_str(text);
    put('\n');
}

void result_emit(void)
{
    if (!tud_vendor_mounted())
        return;

    /* If anything was dropped, say so in the answer itself. A result that is
     * quietly short is worse than no result: it reads as a measurement.
     *
     * put() stops at RESULT_BODY_MAX, so g_len + RESULT_MARK_LEN is at most
     * RESULT_MAX and the marker always goes in whole. */
    unsigned len = g_len;
    if (g_full) {
        for (unsigned i = 0; i < RESULT_MARK_LEN; i++)
            g_buf[len++] = RESULT_MARK[i];
    }

    /* Wait for room before writing, rather than writing into a full FIFO and
     * losing the answer silently.
     *
     * That is not hypothetical: the census emitted after every measurement and
     * each emit carried the whole buffer so far, so five of them overflowed
     * the 512-byte FIFO and the last two - the ones with the measurement that
     * mattered and the "done" - were dropped. The firmware was fine and the
     * answer simply never left. */
    for (uint32_t i = 0; i < 2000u && tud_vendor_write_available() < len; i++)
        tud_task();

    /* One write, one flush. Nothing here loops over the buffer in chunks,
     * because a chunked result is a stream again. */
    tud_vendor_write(g_buf, len);
    tud_vendor_write_flush();

    /* Push it out rather than waiting for the main loop, so the answer is on
     * the wire before whatever the caller does next - which may be the thing
     * that stops the main loop. Bounded, for the reason in log.c. */
    for (uint32_t i = 0; i < 2000u; i++)
        tud_task();
}
