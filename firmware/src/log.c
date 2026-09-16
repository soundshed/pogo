/* Buffered diagnostic log, sent over the vendor endpoint - and as SysEx on the
 * MIDI stream too, where MIDI_CONTROL_LOG_SYSEX asks for it. See log.h. */

#include <stdint.h>
#include "log.h"
#include "tusb.h"

/* Sized for a whole bring-up run: the entry register dump, a line per init
 * step, and the test results is a few dozen lines. It lives in .bss, which
 * midi_control.ld puts in AHB SRAM, and there is ~40 KB spare there after the
 * framebuffer.
 *
 * When it fills it stops accepting rather than wrapping. A ring buffer would
 * discard the *earliest* lines, and the earliest lines - what state the machine
 * was in before we touched it - have been the most valuable ones every time.
 *
 * 4096 was enough until the audio path arrived and then quietly was not. A boot
 * now costs the EMC dump, two full I2S censuses and a capture report - about
 * 4 KB on its own - so the first CC#86 after boot emitted its entry census and
 * then hit the ceiling mid-sentence, which read exactly like the firmware
 * having stopped. It had not. That is the failure mode "stops accepting" is
 * supposed to avoid being mistaken for, so the fix is the size, not the
 * policy. */
#define LOG_CAPACITY 16384
#define LOG_LINE_MAX 100

static char g_buf[LOG_CAPACITY];
static unsigned g_len;          /* bytes used */
static unsigned g_sent;         /* bytes already sent as SysEx */
static unsigned g_sent_vendor;  /* bytes already sent over the bulk endpoint */
static unsigned g_overflow;

/* The vendor endpoint stays silent until asked.
 *
 * Results and the log share one IN endpoint, so a log that streams of its own
 * accord turns every result packet into "whatever happened to be in the pipe".
 * The first run of the result command read the SDRAM bring-up log instead of
 * its answer, for exactly that reason.
 *
 * So the log is on demand: nothing goes out until CC#126 asks for it, which is
 * also the command that rewinds to the start. A result packet is then the only
 * thing that can be in flight when one is requested. */
static unsigned g_streaming;

static void put(char c)
{
    if (g_len < LOG_CAPACITY)
        g_buf[g_len++] = c;
    else
        g_overflow = 1;
}

void log_line(const char *s)
{
    for (unsigned n = 0; *s && n < LOG_LINE_MAX; s++, n++)
        put((char)(*s & 0x7F));      /* SysEx data bytes must stay 7-bit */
    put('\n');
}

static void put_str(const char *s)
{
    for (unsigned n = 0; *s && n < LOG_LINE_MAX; s++, n++)
        put((char)(*s & 0x7F));
}

void log_hex(const char *label, uint32_t value)
{
    put_str(label);
    put(' ');
    for (int i = 7; i >= 0; i--) {
        unsigned nib = (value >> (i * 4)) & 0xF;
        put((char)(nib < 10 ? '0' + nib : 'A' + nib - 10));
    }
    put('\n');
}

void log_dec(const char *label, uint32_t value)
{
    char tmp[12];
    unsigned n = 0;

    put_str(label);
    put(' ');
    do {
        tmp[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && n < sizeof(tmp));
    while (n--)
        put(tmp[n]);
    put('\n');
}

/* Replay the whole buffer from the beginning.
 *
 * Without this the log is a race the host cannot win: the buffer drains the
 * moment USB is mounted, which is long before anyone can attach a listener, so
 * the interesting part - everything captured before enumeration - is already
 * gone by the time you are watching. Rewinding on request makes the capture
 * repeatable and lets it be taken whenever it suits. */
void log_rewind(void)
{
    g_sent = 0;
    g_sent_vendor = 0;
    g_streaming = 1;
}

int log_drained(void)
{
    return g_sent >= g_len;
}

/* The bulk endpoint's share of the work.
 *
 * Plain 8-bit text, newline separated, no framing of any kind - the host reads
 * bytes off a pipe and prints them. That is the whole advantage over the SysEx
 * path: nothing to parse, nothing to get out of step, and no 7-bit limit.
 *
 * A whole packet at a time rather than a line, because the endpoint is 512
 * bytes and there is no small FIFO to overrun the way the MIDI one has. */
static void log_task_vendor(void)
{
    if (!g_streaming || !tud_vendor_mounted() || g_sent_vendor >= g_len)
        return;

    unsigned avail = g_len - g_sent_vendor;
    unsigned room = tud_vendor_write_available();

    if (room == 0)
        return;
    if (avail > room)
        avail = room;

    /* Never fill a whole packet.
     *
     * 512 is the endpoint's max packet size, and USB only considers a transfer
     * finished when it sees a packet shorter than that. Write exactly 512 and
     * the host's read never completes on its own: it waits for a continuation
     * that only arrives when more log is produced. The transfer stays
     * outstanding, and if the host gives up and closes, it is stranded on the
     * device for good - the endpoint then looks dead until the LPC resets.
     *
     * Capping at 511 makes every packet a short one, so every read completes
     * the moment it arrives and nothing is ever left in flight. One byte of
     * throughput for the difference between a log that can be read twice and
     * one that cannot. */
    if (avail > 511u)
        avail = 511u;

    unsigned n = tud_vendor_write(g_buf + g_sent_vendor, avail);
    g_sent_vendor += n;
    tud_vendor_write_flush();
}

void log_send_raw(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    while (len) {
        uint32_t room = 0;

        for (uint32_t i = 0; i < 200000u; i++) {
            tud_task();
            room = tud_vendor_write_available();
            if (room)
                break;
        }
        if (!room)
            return;                 /* host gone - give up rather than hang */

        uint32_t chunk = len < room ? len : room;
        uint32_t w = tud_vendor_write(p, chunk);
        tud_vendor_write_flush();
        tud_task();
        p += w;
        len -= w;
    }
}

void log_pump(void)
{
    tud_task();
    log_task_vendor();
}

void log_flush(void)
{
    /* Bounded: a host that is not reading must not turn this into a stall of
     * its own. A few hundred thousand iterations is far longer than a packet
     * takes and still finite. */
    /* 2,000, not 400,000.
     *
     * The larger cap was chosen as "far longer than a packet takes", which
     * missed that this runs with the host mid-read: if the reader is even
     * briefly behind, the spin is long enough for it to declare the stream
     * finished and close - and a close mid-stream strands the transfer. A
     * flush that outlasts the reader's patience defeats itself.
     *
     * 2,000 iterations is still ample for the one packet this needs to get
     * out, and short enough that the main loop keeps servicing USB. */
    for (uint32_t i = 0; i < 2000u && g_sent_vendor < g_len; i++) {
        tud_task();
        log_task_vendor();
    }
}

void log_task(void)
{
    log_task_vendor();

#if !(MIDI_CONTROL_ENABLE_MIDI && MIDI_CONTROL_LOG_SYSEX)
    /* With no SysEx reader - no MIDI interface, or the SysEx copy switched off
     * in tusb_config.h - the buffer would never drain past that cursor and
     * log_drained() would never come true. Keep it moving with the one that
     * does. */
    g_sent = g_sent_vendor;
    if (g_overflow && log_drained()) {
        g_overflow = 0;
        log_line("LOG OVERFLOW - buffer full, lines were dropped");
    }
    return;
}
#else
    if (!tud_midi_mounted() || g_sent >= g_len)
        return;

    /* One line per call. The MIDI endpoint FIFO is small, and a burst of forty
     * SysEx messages in one go would simply be dropped on the floor - which is
     * a particularly annoying way to lose a diagnostic. The main loop runs
     * thousands of times a second, so drip-feeding costs nothing. */
    uint8_t pkt[LOG_LINE_MAX + 8];
    unsigned n = 0;

    pkt[n++] = 0xF0;
    pkt[n++] = 0x7D;                 /* non-commercial / educational use */

    while (g_sent < g_len && n < sizeof(pkt) - 1) {
        char c = g_buf[g_sent++];
        if (c == '\n')
            break;
        pkt[n++] = (uint8_t)c;
    }

    pkt[n++] = 0xF7;

    tud_midi_stream_write(0, pkt, (uint16_t)n);

    if (g_overflow && log_drained()) {
        g_overflow = 0;
        log_line("LOG OVERFLOW - buffer full, lines were dropped");
    }
}
#endif
