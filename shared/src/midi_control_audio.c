/* The LPC's end of the audio link - see midi_control_audio.h and docs/10. */
#include "midi_control/midi_control_audio.h"

#include <string.h>

void midi_control_audio_init(struct midi_control_audio *a)
{
    memset(a, 0, sizeof(*a));
    a->monitor = MIDI_CONTROL_AUDIO_GUITAR;
    a->gain = MIDI_CONTROL_AUDIO_UNITY;
    a->slot = MIDI_CONTROL_AUDIO_HUNTING;
}

static int32_t clip(int64_t x)
{
    if (x > MIDI_CONTROL_AUDIO_FULL)
        return MIDI_CONTROL_AUDIO_FULL;
    if (x < -MIDI_CONTROL_AUDIO_FULL)
        return -MIDI_CONTROL_AUDIO_FULL;
    return (int32_t)x;
}

/* At each frame's marker, with `in` holding the frame that has just finished. */
static void frame_complete(struct midi_control_audio *a)
{
    unsigned m = a->monitor;
    int32_t v = 0;

    for (unsigned s = 0; s < MIDI_CONTROL_AUDIO_INPUTS; s++) {
        int32_t x = a->in[s];
        uint32_t mag = (uint32_t)(x < 0 ? -(int64_t)x : x);
        if (mag > a->peak[s])
            a->peak[s] = mag;
    }

    if (m < MIDI_CONTROL_AUDIO_INPUTS)
        v = clip(((int64_t)a->in[m] * a->gain) >> 6);
    a->out[0] = v;
    a->out[1] = v;
}

uint32_t midi_control_audio_word(struct midi_control_audio *a, uint32_t rx)
{
    if (rx & 1u) {
        /* A marker. On time only if the count has just wrapped to 0. */
        if (a->slot == 0u) {
            a->frames++;
            frame_complete(a);
        } else if (a->slot != MIDI_CONTROL_AUDIO_HUNTING) {
            a->desyncs++;
        }
        a->slot = 0u;
    } else if (a->slot == MIDI_CONTROL_AUDIO_HUNTING) {
        return 0u;                      /* silence until aligned */
    } else if (a->slot == 0u) {
        a->desyncs++;                   /* a marker was due and did not come */
        a->slot = MIDI_CONTROL_AUDIO_HUNTING;
        a->out[0] = a->out[1] = 0;
        return 0u;
    }

    unsigned s = a->slot;
    uint32_t tx = 0u;

    if (s < MIDI_CONTROL_AUDIO_INPUTS)
        a->in[s] = (int32_t)rx >> 8;    /* arithmetic: the sample is signed */
    if (s < 2u)
        tx = ((uint32_t)a->out[s] << 8) | (s == 0u ? 1u : 0u);

    a->slot = (uint8_t)((s + 1u) & (MIDI_CONTROL_AUDIO_SLOTS - 1u));
    return tx;
}

uint16_t midi_control_audio_knob_gain(uint8_t position)
{
    uint32_t p = position > 127u ? 127u : position;

    /* p^2 / 64: exactly unity at 64, 252 (just under four times) at 127. */
    return (uint16_t)((p * p + 32u) / 64u);
}
