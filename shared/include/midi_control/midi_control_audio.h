/* The LPC's end of the audio link to the SHARC (docs/10).
 *
 * The SHARC runs our own pass-through program. It clocks an I2S link to the
 * LPC at 192 kHz word select, 32-bit words, eight words to a 48 kHz frame each
 * way. Bit 0 of a word marks slot 0 of a frame, in both directions.
 *
 *   SHARC -> LPC   slot 0..3  ADC A left, ADC A right, ADC B left, ADC B right
 *                  slot 4..7  zero
 *   LPC -> SHARC   slot 0     output left (and the marker)
 *                  slot 1     output right
 *                  slot 2..7  zero
 *
 * Samples are 24-bit, in the top of each word; the bottom byte is free, and bit
 * 0 of it is the marker. The guitar is on ADC A right, link slot 1 (measured in
 * the parent project, docs/09 section 9 there).
 *
 * This takes one received word and returns the word to send in its place, the
 * stock interrupt handler's one-for-one, which keeps the two directions in
 * step. The board layer calls it from the I2S interrupt for every word; the
 * main loop sets the monitor input and the gain.
 *
 * The output is the monitored input times the gain, the same on both sides.
 * The gain is q6: 64 is unity, 255 the most. */
#ifndef MIDI_CONTROL_AUDIO_H
#define MIDI_CONTROL_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_CONTROL_AUDIO_SLOTS     8u      /* words per frame, each way */
#define MIDI_CONTROL_AUDIO_INPUTS    4u      /* audio slots from the SHARC */
#define MIDI_CONTROL_AUDIO_HUNTING   0xFFu   /* `slot` before the first marker */
#define MIDI_CONTROL_AUDIO_UNITY     64u     /* q6 gain of 1 */
#define MIDI_CONTROL_AUDIO_GAIN_MAX  255u
#define MIDI_CONTROL_AUDIO_FULL      0x7FFFFF
#define MIDI_CONTROL_AUDIO_GUITAR    1u      /* the guitar's slot, measured */
#define MIDI_CONTROL_AUDIO_MONITOR_OFF 0xFFu

struct midi_control_audio {
    /* Set by the main loop. Single bytes and a half-word, so the interrupt can
     * never see half of an update. */
    uint8_t monitor;                    /* input slot sent to the outputs, or _OFF */
    uint16_t gain;                      /* q6 */

    /* The link, touched only per word. */
    uint8_t slot;                       /* the slot this word is, or _HUNTING */
    int32_t in[MIDI_CONTROL_AUDIO_INPUTS];   /* this frame so far, 24-bit signed */
    int32_t out[2];                     /* the frame being sent, left and right */

    /* Counters, read by the main loop. */
    uint32_t frames;                    /* complete frames */
    uint32_t desyncs;                   /* markers missing or early */
    uint32_t peak[MIDI_CONTROL_AUDIO_INPUTS];   /* largest magnitude seen per input */
};

/* Hunting, silent, monitoring the guitar at unity. */
void midi_control_audio_init(struct midi_control_audio *a);

/* One received word in; the word to transmit in its place out. */
uint32_t midi_control_audio_word(struct midi_control_audio *a, uint32_t rx);

/* The volume knob's position, 0..127, as a q6 gain: square law, position^2 / 64
 * rounded. Silence at 0, exactly unity at 64, 252 (just under four times) at
 * 127. */
uint16_t midi_control_audio_knob_gain(uint8_t position);

#ifdef __cplusplus
}
#endif
#endif
