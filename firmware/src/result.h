#ifndef MIDI_CONTROL_RESULT_H
#define MIDI_CONTROL_RESULT_H

#include <stdint.h>

/* A short answer, sent as a single packet.
 *
 * The diagnostic log is a stream, and a stream is the wrong shape for "what
 * did that experiment measure". To read sixteen lines of census the host has
 * to pull five kilobytes of boot log through a pipe where one dropped beat
 * strands a transfer and the endpoint stays dead until the unit is power
 * cycled. That cost most of a day and eight power cycles, and every fix made
 * it better without making it reliable (docs/10 section 5).
 *
 * So results do not go through the log at all. An experiment fills a small
 * fixed buffer, and one command sends the whole of it in a single write that
 * is always shorter than the endpoint's 512 bytes. One packet, one read, and
 * nothing left queued afterwards - which removes the entire failure class
 * rather than narrowing it again.
 *
 * The log stays as it is. It is the right tool for watching a boot, and it is
 * no longer on the critical path for getting an answer out.
 */

/* Start a new answer. `tag` names the experiment and is sent first, so a
 * reader can tell a fresh result from a stale one. */
void result_begin(const char *tag);

/* Append `name=value`, value in hex. Costs strlen(name) + 10 bytes of the 390
 * a result holds - about 27 four-character fields once the header is paid for.
 * Silently ignored past that, which is reported by result_emit() rather than
 * being allowed to truncate quietly. */
void result_put(const char *name, uint32_t value);

/* Append a bare word - for a verdict rather than a measurement. Bounded at 40
 * characters, because a bad pointer would otherwise be read until it happened
 * to hit a zero; a verdict clipped by that bound is marked truncated too. */
void result_note(const char *text);

/* Send it. One write, one flush, then the USB stack is pumped until it has
 * gone out or a bounded spin gives up. Safe to call repeatedly; the buffer is
 * not cleared, so the same answer can be asked for again. */
void result_emit(void);

#endif
