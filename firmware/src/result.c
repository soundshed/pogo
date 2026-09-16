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

static char g_buf[RESULT_MAX];
static unsigned g_len;
static unsigned g_full;

static void put(char c)
{
    if (g_len < RESULT_MAX - 1u)
        g_buf[g_len++] = c;
    else
        g_full = 1;
}

static void put_str(const char *s)
{
    for (unsigned n = 0; *s && n < 40u; s++, n++)
        put(*s);
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
     * quietly short is worse than no result: it reads as a measurement. */
    unsigned len = g_len;
    if (g_full) {
        const char *w = "TRUNCATED\n";
        while (*w && len < RESULT_MAX)
            g_buf[len++] = *w++;
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
