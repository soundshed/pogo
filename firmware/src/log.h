#ifndef MIDI_CONTROL_LOG_H
#define MIDI_CONTROL_LOG_H

#include <stdint.h>

/* A diagnostic log that comes out over the MIDI port we already have.
 *
 * Reading results off the panel does not scale. It costs a round trip through a
 * human for every number, the interesting values are eight hex digits where a
 * misread '8' for a '0' sends the next hour in the wrong direction, and it has
 * already happened. This puts the same information on the wire.
 *
 * The awkward part is timing: everything worth logging happens *before* USB is
 * up. So lines are buffered in RAM as they are produced and streamed out once
 * the host has enumerated us, which means the log survives a slow start but not
 * a hard stall - and a hard stall is exactly what the on-screen stage marker is
 * still there for. The two are complementary, not redundant.
 *
 * Transport is SysEx: F0 7D <ascii> F7. 0x7D is the manufacturer ID reserved
 * for non-commercial and educational use, which is precisely what this is.
 * tools/midi-listen.ps1 already captures SysEx and now renders 7D payloads as
 * text.
 */

/* Append one line. The newline is implied; `s` must be plain 7-bit ASCII. */
void log_line(const char *s);

/* `label` then the value. Hex is zero-padded to eight digits. */
void log_hex(const char *label, uint32_t value);
void log_dec(const char *label, uint32_t value);

/* Drain at most one buffered line to each host reader. Call from the main
 * loop; a reader that is not mounted simply waits.
 *
 * There are two readers and they are independent, each with its own position
 * in the buffer: the MIDI SysEx stream this was built on, and a vendor bulk
 * endpoint added after winmm proved too fragile to rely on (tusb_config.h).
 * Reading one does not consume the other, so both can be attached at once and
 * either can be ignored. */
void log_task(void);

/* Replay the whole buffer from the start for both readers, so a listener
 * attached after boot still gets the early lines. */
void log_rewind(void);

/* True once everything buffered has been sent. */
int log_drained(void);

/* Push everything buffered out of the vendor endpoint before returning.
 *
 * The log is a RAM buffer that only drains from the main loop, which is fine
 * until the thing being logged is the thing that stops the main loop. Then the
 * lines that would say where it stopped are sitting in the buffer with nothing
 * left running to send them, and the host sees a firmware that went silent for
 * no stated reason. That is exactly how a register read that stalls the core
 * hides the offset that caused it.
 *
 * Call this immediately before anything that might not return. It pumps the
 * USB stack itself rather than waiting for the main loop, and gives up after a
 * bounded spin so it cannot become the hang it exists to diagnose. */
void log_flush(void);

/* Send raw bytes out the vendor IN endpoint, waiting for FIFO room and pumping
 * USB as it goes.
 *
 * This lives here because log.c owns that endpoint. Anything that needs to move
 * more than a single result packet - a flash dump, an eMMC backup - goes through
 * this rather than growing its own copy of the same loop.
 *
 * Bounded: if the host stops reading, the wait for room gives up and the
 * transfer abandons rather than hanging the main loop. The caller frames its own
 * data; this moves bytes and nothing else.
 *
 * The "never fill a whole packet" rule in docs/10 section 5 does not apply to a
 * transfer whose length the host already knows - such a reader counts bytes
 * instead of waiting for a short packet to end the transfer. */
void log_send_raw(const void *data, uint32_t len);

/* Service USB once, from inside a long-running piece of work.
 *
 * Anything that runs for more than a few milliseconds without returning to the
 * main loop makes the device deaf: the USB stack is not polled, so IN transfers
 * are not completed and OUT data is not collected. From the host that is
 * indistinguishable from a firmware that has died - and the eMMC read is 785
 * sector transfers followed by two CRC passes over 400 KB, which is easily long
 * enough to look like exactly that.
 *
 * Call this every so often inside such a loop. It is one poll, not a drain, so
 * it costs almost nothing per call. */
void log_pump(void);

#endif
