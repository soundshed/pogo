/* The LPC's end of the audio link - see midi_control_audio.h and docs/10. */
#include "midi_control/midi_control_audio.h"

#include <string.h>

void midi_control_audio_init(struct midi_control_audio *a)
{
    memset(a, 0, sizeof(*a));
    a->monitor = MIDI_CONTROL_AUDIO_DI;
    a->gain = MIDI_CONTROL_AUDIO_UNITY;
    a->record_gain = MIDI_CONTROL_AUDIO_Q16_UNITY;
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

static uint32_t magnitude(int64_t x)
{
    return (uint32_t)(x < 0 ? -x : x);
}

/* DAC channel k's sample: times the gain on Main and Phones, unity elsewhere.
 * Main and Phones' level and clipping are counted. */
static int32_t knob(struct midi_control_audio *a, unsigned k, int32_t x)
{
    if (!(MIDI_CONTROL_AUDIO_KNOB_CHANNELS & (1u << k)))
        return x;
    int64_t v = ((int64_t)x * a->gain) >> 6;
    int32_t y = clip(v);
    if (y != v)
        a->knob_clips++;
    if (magnitude(y) > a->knob_peak)
        a->knob_peak = magnitude(y);
    return y;
}

/* At each frame's marker, with `in` holding the frame that has just finished. */
static void frame_complete(struct midi_control_audio *a)
{
    unsigned m = a->monitor;
    int32_t v = 0;

    for (unsigned s = 0; s < MIDI_CONTROL_AUDIO_INPUTS; s++) {
        int32_t x = a->in[s];
        uint32_t mag = magnitude(x);
        if (mag > a->peak[s])
            a->peak[s] = mag;
    }

    /* For the host to record: the guitar from the slot that does not clip,
     * times the record trim, not the knob's gain, on both channels. */
    int32_t rec = clip(((int64_t)a->in[MIDI_CONTROL_AUDIO_DI] * a->record_gain) >> 16);
    for (unsigned c = 0; c < MIDI_CONTROL_AUDIO_HOST_CHANNELS; c++)
        a->capture[c] = rec;

    if (a->mode >= MIDI_CONTROL_AUDIO_MODE_TONE_FIRST && a->mode <= MIDI_CONTROL_AUDIO_MODE_TONE_LAST) {
        /* 1024 / 65536 of a cycle a frame: 750 Hz. A triangle, +-16384. */
        uint32_t ph = (a->frames * 1024u) & 0xFFFFu;
        int32_t tri = ph < 0x8000u ? (int32_t)ph - 0x4000 : 0xBFFF - (int32_t)ph;
        int32_t tone = clip(((int64_t)tri * (MIDI_CONTROL_AUDIO_TONE / 0x4000) * a->gain) >> 6);
        unsigned on = a->mode - MIDI_CONTROL_AUDIO_MODE_TONE_FIRST;

        for (unsigned k = 0; k < MIDI_CONTROL_AUDIO_OUTPUTS; k++)
            a->out[k] = k == on ? tone : 0;
        return;
    }

    if (a->host) {
        for (unsigned c = 0; c < MIDI_CONTROL_AUDIO_HOST_CHANNELS; c++)
            if (magnitude(a->host_in[c]) > a->host_peak)
                a->host_peak = magnitude(a->host_in[c]);
        /* Left on the even DAC channels, right on the odd: each port's pair. */
        for (unsigned k = 0; k < MIDI_CONTROL_AUDIO_OUTPUTS; k++)
            a->out[k] = knob(a, k, a->host_in[k & 1u]);
        return;
    }

    if (m < MIDI_CONTROL_AUDIO_INPUTS)
        v = a->in[m];
    for (unsigned k = 0; k < MIDI_CONTROL_AUDIO_OUTPUTS; k++)
        a->out[k] = knob(a, k, v);
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
        memset(a->out, 0, sizeof(a->out));
        return 0u;
    }

    unsigned s = a->slot;
    uint32_t tx = 0u;

    if (s < MIDI_CONTROL_AUDIO_INPUTS)
        a->in[s] = (int32_t)rx >> 8;    /* arithmetic: the sample is signed */
    if (s < MIDI_CONTROL_AUDIO_OUTPUTS)
        tx = ((uint32_t)a->out[s] << 8) | (s == 0u ? 1u : 0u);

    a->slot = (uint8_t)((s + 1u) & (MIDI_CONTROL_AUDIO_SLOTS - 1u));
    return tx;
}

/* 10^(dB/20) x 65536, rounded, for -48..+12 dB (generated, not measured). */
static const uint32_t db_q16[MIDI_CONTROL_AUDIO_DB_MAX - MIDI_CONTROL_AUDIO_DB_MIN + 1] = {
    261u, 293u, 328u, 369u, 414u, 464u, 521u, 584u,
    655u, 735u, 825u, 926u, 1039u, 1165u, 1308u, 1467u,
    1646u, 1847u, 2072u, 2325u, 2609u, 2927u, 3285u, 3685u,
    4135u, 4640u, 5206u, 5841u, 6554u, 7353u, 8250u, 9257u,
    10387u, 11654u, 13076u, 14672u, 16462u, 18471u, 20724u, 23253u,
    26090u, 29274u, 32846u, 36854u, 41350u, 46396u, 52057u, 58409u,
    65536u, 73533u, 82505u, 92572u, 103868u, 116541u, 130762u, 146717u,
    164619u, 184706u, 207243u, 232531u, 260904u,
};

uint32_t midi_control_audio_db_gain(int db)
{
    if (db < MIDI_CONTROL_AUDIO_DB_MIN)
        db = MIDI_CONTROL_AUDIO_DB_MIN;
    if (db > MIDI_CONTROL_AUDIO_DB_MAX)
        db = MIDI_CONTROL_AUDIO_DB_MAX;
    return db_q16[db - MIDI_CONTROL_AUDIO_DB_MIN];
}

uint16_t midi_control_audio_knob_gain(uint8_t position)
{
    uint32_t p = position > 127u ? 127u : position;

    /* (p + 1) / 2: linear in the pot's reading, whose own taper is
     * logarithmic. Exactly unity at 127, the top, so nothing is taken past
     * full scale. */
    return (uint16_t)((p + 1u) / 2u);
}

uint32_t midi_control_audio_to_subslot(int32_t sample)
{
    return (uint32_t)sample << 8;
}

int32_t midi_control_audio_from_subslot(uint32_t subslot)
{
    return (int32_t)subslot >> 8;       /* arithmetic: the sample is signed */
}

/* ---- USB playback feedback ---------------------------------------------- */

void midi_control_audio_rate_init(struct midi_control_audio_rate *r, uint32_t target_frames)
{
    memset(r, 0, sizeof(*r));
    r->level_q16 = (int32_t)(target_frames << 16);
    r->rate = MIDI_CONTROL_AUDIO_FB_NOMINAL;
    r->feedback = MIDI_CONTROL_AUDIO_FB_NOMINAL;
}

static void rate_snapshot(struct midi_control_audio_rate *r, uint32_t link_frames)
{
    r->snap_frames[r->head] = link_frames;
    r->snap_uf[r->head] = r->uf;
    r->head = (uint8_t)((r->head + 1u) % MIDI_CONTROL_AUDIO_FB_SNAPS);
    if (r->snaps < MIDI_CONTROL_AUDIO_FB_SNAPS)
        r->snaps++;
}

uint32_t midi_control_audio_rate_sof(struct midi_control_audio_rate *r, uint16_t uframe,
                                     uint32_t link_frames, uint32_t level_frames,
                                     uint32_t target_frames)
{
    uint32_t d;

    uframe &= MIDI_CONTROL_AUDIO_FB_UF_MASK;
    if (!r->started) {
        r->started = 1u;
        r->last_uf = uframe;
        r->next_ms_uf = 8u;
        rate_snapshot(r, link_frames);
        return r->feedback;
    }
    d = (uint32_t)(uframe - r->last_uf) & MIDI_CONTROL_AUDIO_FB_UF_MASK;
    if (d == 0u)
        return r->feedback;
    r->last_uf = uframe;
    r->uf += d;

    /* The level, once a millisecond whatever the interrupt's rate: a 64 ms
     * average, which takes out the FIFO's sawtooth of one packet. */
    if ((int32_t)(r->uf - r->next_ms_uf) >= 0) {
        r->next_ms_uf = r->uf + 8u;
        r->level_q16 += ((int32_t)(level_frames << 16) - r->level_q16) / 64;
    }

    /* The rate, from the oldest snapshot to now, every 256 ms. */
    {
        uint8_t newest = (uint8_t)((r->head + MIDI_CONTROL_AUDIO_FB_SNAPS - 1u) % MIDI_CONTROL_AUDIO_FB_SNAPS);
        if (r->uf - r->snap_uf[newest] >= MIDI_CONTROL_AUDIO_FB_SNAP_UF) {
            uint8_t oldest = r->snaps < MIDI_CONTROL_AUDIO_FB_SNAPS ? 0u : r->head;
            uint32_t frames = link_frames - r->snap_frames[oldest];
            uint32_t span = r->uf - r->snap_uf[oldest];
            uint64_t rate = ((uint64_t)frames << 16) / span;

            if (rate > MIDI_CONTROL_AUDIO_FB_NOMINAL + MIDI_CONTROL_AUDIO_FB_RATE_SPAN)
                rate = MIDI_CONTROL_AUDIO_FB_NOMINAL + MIDI_CONTROL_AUDIO_FB_RATE_SPAN;
            if (rate < MIDI_CONTROL_AUDIO_FB_NOMINAL - MIDI_CONTROL_AUDIO_FB_RATE_SPAN)
                rate = MIDI_CONTROL_AUDIO_FB_NOMINAL - MIDI_CONTROL_AUDIO_FB_RATE_SPAN;
            r->rate = (uint32_t)rate;
            rate_snapshot(r, link_frames);
        }
    }

    /* The trim: a frame of level error is 8 / 65536 of a frame a microframe,
     * so the FIFO comes back to its target with a time constant of about a
     * second, slow beside the level's average. */
    {
        int32_t err = ((int32_t)(target_frames << 16) - r->level_q16) * MIDI_CONTROL_AUDIO_FB_TRIM_GAIN;
        int32_t trim = (err + (err < 0 ? -32768 : 32768)) / 65536;   /* to the nearest */
        if (trim > MIDI_CONTROL_AUDIO_FB_TRIM_SPAN)
            trim = MIDI_CONTROL_AUDIO_FB_TRIM_SPAN;
        if (trim < -MIDI_CONTROL_AUDIO_FB_TRIM_SPAN)
            trim = -MIDI_CONTROL_AUDIO_FB_TRIM_SPAN;
        r->trim = trim;
    }

    r->feedback = (uint32_t)((int32_t)r->rate + r->trim);
    if (r->snaps == MIDI_CONTROL_AUDIO_FB_SNAPS) {
        if (r->fb_min == 0u || r->feedback < r->fb_min)
            r->fb_min = r->feedback;
        if (r->feedback > r->fb_max)
            r->fb_max = r->feedback;
    }
    return r->feedback;
}

uint32_t midi_control_audio_rate_mhz(uint32_t rate)
{
    /* rate / 65536 frames a microframe, 8,000 microframes, 1,000 mHz. */
    return (uint32_t)(((uint64_t)rate * 8000000u + 32768u) >> 16);
}
