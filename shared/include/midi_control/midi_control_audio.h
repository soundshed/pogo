/* The LPC's end of the audio link to the SHARC (docs/10).
 *
 * The SHARC runs our own pass-through program. It clocks an I2S link to the
 * LPC at 192 kHz word select, 32-bit words, eight words to a 48 kHz frame each
 * way. Bit 0 of a word marks slot 0 of a frame, in both directions.
 *
 *   SHARC -> LPC   slot 0..3  ADC A left, ADC A right, ADC B left, ADC B right
 *                  slot 4..7  zero
 *   LPC -> SHARC   slot 0, 1  SPORT4 A (DAI 5) left (and the marker), right: Amp Out
 *                  slot 2, 3  SPORT4 B (DAI 6/16) left, right: Main L, R and Phones
 *                  slot 4, 5  SPORT5 A (DAI 7) left, right: FX Send
 *                  slot 6, 7  zero
 * (the jacks measured, docs/10 section 4)
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
 * In the monitor mode every output is the monitored input times the gain. The
 * gain is q6: 64 is unity, 255 the most. In a tone mode one DAC channel carries
 * a steady tone and the rest are silent, so a jack can be matched to its
 * channel by ear; the gain applies to the tone too. */
#ifndef MIDI_CONTROL_AUDIO_H
#define MIDI_CONTROL_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_CONTROL_AUDIO_SLOTS     8u      /* words per frame, each way */
#define MIDI_CONTROL_AUDIO_INPUTS    4u      /* audio slots from the SHARC */
#define MIDI_CONTROL_AUDIO_OUTPUTS   6u      /* DAC channels, slots 0..5 to the SHARC */
#define MIDI_CONTROL_AUDIO_HUNTING   0xFFu   /* `slot` before the first marker */
#define MIDI_CONTROL_AUDIO_UNITY     64u     /* q6 gain of 1 */
#define MIDI_CONTROL_AUDIO_GAIN_MAX  255u
#define MIDI_CONTROL_AUDIO_FULL      0x7FFFFF
#define MIDI_CONTROL_AUDIO_GUITAR    1u      /* the guitar's slot, measured */
#define MIDI_CONTROL_AUDIO_MONITOR_OFF 0xFFu

/* The tone modes' tone: a 750 Hz triangle at TONE peak (about -18 dBFS) before
 * the gain. */
#define MIDI_CONTROL_AUDIO_TONE      0x100000

/* `mode`: MONITOR, or TONE_FIRST + k for the tone on DAC channel k alone
 * (k = 0..5; channel k is link slot k). */
#define MIDI_CONTROL_AUDIO_MODE_MONITOR    0u
#define MIDI_CONTROL_AUDIO_MODE_TONE_FIRST 1u
#define MIDI_CONTROL_AUDIO_MODE_TONE_LAST  (MIDI_CONTROL_AUDIO_MODE_TONE_FIRST + MIDI_CONTROL_AUDIO_OUTPUTS - 1u)

struct midi_control_audio {
    /* Set by the main loop. Single bytes and a half-word, so the interrupt can
     * never see half of an update. */
    uint8_t mode;                       /* MIDI_CONTROL_AUDIO_MODE_ */
    uint8_t monitor;                    /* input slot sent to the outputs, or _OFF */
    uint16_t gain;                      /* q6 */

    /* The link, touched only per word. */
    uint8_t slot;                       /* the slot this word is, or _HUNTING */
    int32_t in[MIDI_CONTROL_AUDIO_INPUTS];   /* this frame so far, 24-bit signed */
    int32_t out[MIDI_CONTROL_AUDIO_OUTPUTS];  /* the frame being sent, per DAC channel */

    /* Counters, read by the main loop. */
    uint32_t frames;                    /* complete frames */
    uint32_t desyncs;                   /* markers missing or early */
    uint32_t peak[MIDI_CONTROL_AUDIO_INPUTS];   /* largest magnitude seen per input */
};

/* Hunting, silent, monitoring the guitar at unity on every output. */
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
