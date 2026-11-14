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
 * the parent project, docs/09 section 9 there), and 24 dB down on ADC A left,
 * slot 0. Slot 1 clips when the guitar is played hard, so slot 0 is the one
 * monitored and recorded (MIDI_CONTROL_AUDIO_DI).
 *
 * This takes one received word and returns the word to send in its place, the
 * stock interrupt handler's one-for-one, which keeps the two directions in
 * step. The board layer calls it from the I2S interrupt for every word; the
 * main loop sets the monitor input and the gain.
 *
 * In the monitor mode every output carries the monitored input: Main and Phones
 * (DAC channels 2 and 3) times the gain, the volume knob's; Amp Out and FX Send
 * at unity, as the line-level feeds they are. The gain is q6: 64 is unity, 255
 * the most. In a tone mode one DAC channel carries a steady tone and the rest
 * are silent, so a jack can be matched to its channel by ear; the gain applies
 * to the tone on any channel.
 *
 * USB audio (docs/10 section 8). While a host plays to the unit, `host` is set
 * and the outputs carry the host's two channels instead of the monitor: left on
 * the even DAC channels (link slots 0, 2, 4), right on the odd ones, with the
 * gain on Main and Phones only, as for the monitor. The board layer hands over the host's next frame in `host_in` at each
 * frame boundary, and takes `capture`, the frame just finished for the host to
 * record: the guitar, dry, on both channels - slot 0, the copy 24 dB down,
 * which does not clip - times `record_gain`, the record trim (q16; the knob
 * does not touch it). Samples cross USB as 4-byte
 * subslots with 24 bits used, the sample in the top three bytes. */
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
#define MIDI_CONTROL_AUDIO_GUITAR    1u      /* the guitar at full gain, ADC A right, measured */
/* The guitar 24 dB down, ADC A left: what the outputs monitor and the host
 * records. Slot 1 clips at the converter when the guitar is played hard; slot 0
 * peaked between -16.5 and -4.6 dBFS then (measured, docs/10 sections 2, 8). */
#define MIDI_CONTROL_AUDIO_DI        0u
#define MIDI_CONTROL_AUDIO_MONITOR_OFF 0xFFu
#define MIDI_CONTROL_AUDIO_HOST_CHANNELS 2u  /* each way over USB */
#define MIDI_CONTROL_AUDIO_Q16_UNITY 65536u  /* record_gain of 1 */
#define MIDI_CONTROL_AUDIO_DB_MIN    (-48)   /* midi_control_audio_db_gain()'s range */
#define MIDI_CONTROL_AUDIO_DB_MAX    12
/* The DAC channels the gain scales: Main L/R and Phones, SPORT4 B (docs/10
 * section 4). Amp Out (0, 1) and FX Send (4, 5) stay at unity. */
#define MIDI_CONTROL_AUDIO_KNOB_CHANNELS ((1u << 2) | (1u << 3))

/* The tone modes' tone: a 750 Hz triangle at TONE peak (about -18 dBFS) before
 * the gain. */
#define MIDI_CONTROL_AUDIO_TONE      0x100000

/* `mode`: MONITOR, or TONE_FIRST + k for the tone on DAC channel k alone
 * (k = 0..5; channel k is link slot k). */
#define MIDI_CONTROL_AUDIO_MODE_MONITOR    0u
#define MIDI_CONTROL_AUDIO_MODE_TONE_FIRST 1u
#define MIDI_CONTROL_AUDIO_MODE_TONE_LAST  (MIDI_CONTROL_AUDIO_MODE_TONE_FIRST + MIDI_CONTROL_AUDIO_OUTPUTS - 1u)

struct midi_control_audio {
    /* Set by the main loop. Each a single byte or an aligned (half-)word, so
     * the interrupt can never see half of an update. */
    uint8_t mode;                       /* MIDI_CONTROL_AUDIO_MODE_ */
    uint8_t monitor;                    /* input slot sent to the outputs, or _OFF */
    uint16_t gain;                      /* q6 */
    uint32_t record_gain;               /* q16, on what the host records */

    /* Set by the board layer at each frame boundary, in the link's interrupt. */
    uint8_t host;                       /* nonzero: the outputs carry host_in */
    int32_t host_in[MIDI_CONTROL_AUDIO_HOST_CHANNELS];   /* the host's next frame */

    /* The link, touched only per word. */
    uint8_t slot;                       /* the slot this word is, or _HUNTING */
    int32_t in[MIDI_CONTROL_AUDIO_INPUTS];   /* this frame so far, 24-bit signed */
    int32_t out[MIDI_CONTROL_AUDIO_OUTPUTS];  /* the frame being sent, per DAC channel */
    int32_t capture[MIDI_CONTROL_AUDIO_HOST_CHANNELS];   /* the last frame, for the host */

    /* Counters, read by the main loop. */
    uint32_t frames;                    /* complete frames */
    uint32_t desyncs;                   /* markers missing or early */
    uint32_t peak[MIDI_CONTROL_AUDIO_INPUTS];   /* largest magnitude seen per input */
    uint32_t host_peak;                 /* largest magnitude the host has sent */
    uint32_t knob_peak;                 /* largest on Main and Phones, after the gain */
    uint32_t knob_clips;                /* Main and Phones samples the gain took past full scale */
};

/* Hunting, silent, monitoring the guitar (slot 0) at unity on every output. */
void midi_control_audio_init(struct midi_control_audio *a);

/* One received word in; the word to transmit in its place out. */
uint32_t midi_control_audio_word(struct midi_control_audio *a, uint32_t rx);

/* The volume knob's position, 0..127, as a q6 gain: (position + 1) / 2,
 * linear. The pot is logarithmic (its reading is 4 % of its travel at half
 * rotation, 13 % at three quarters, measured, docs/10 section 2), so a linear
 * law gives an audio taper. Silence at 0, exactly unity at 127: the knob only
 * attenuates, so it cannot clip what it is given. */
uint16_t midi_control_audio_knob_gain(uint8_t position);

/* A level in whole dB as a q16 gain, from a table: 10^(dB/20) x 65536,
 * rounded. Clamped to _DB_MIN.._DB_MAX; 0 dB is exactly unity. */
uint32_t midi_control_audio_db_gain(int db);

/* A 24-bit sample as a USB subslot (4 bytes, 24 bits used), and back. The
 * subslot is little-endian on the wire, as uint32_t is on both cores. */
uint32_t midi_control_audio_to_subslot(int32_t sample);
int32_t midi_control_audio_from_subslot(uint32_t subslot);

/* USB playback feedback (docs/10 section 8): how many frames the host is to
 * send each 125 us microframe, in 16.16. It is the link's frame rate measured
 * against the host's own microframe count over the last few seconds, plus a
 * small trim that steers the playback FIFO to its target.
 *
 * The board layer calls _rate_sof() from the USB start-of-frame interrupt with
 * the controller's microframe index (14 bits, wrapping), the link's frame
 * count, and the playback FIFO's level in frames. The rate comes from whole
 * seconds of frames, so it moves by a few parts per million, not by the
 * FIFO's sawtooth: the host's clock estimate, and any resampling it does on
 * the strength of it, stays steady. */
#define MIDI_CONTROL_AUDIO_FB_NOMINAL   (6ul << 16)     /* 48 kHz / 8,000 microframes */
#define MIDI_CONTROL_AUDIO_FB_SNAPS     16u             /* rate window: 15 x 256 ms */
#define MIDI_CONTROL_AUDIO_FB_SNAP_UF   2048u           /* microframes between snapshots */
#define MIDI_CONTROL_AUDIO_FB_UF_MASK   0x3FFFu         /* the microframe index's width */
#define MIDI_CONTROL_AUDIO_FB_RATE_SPAN (MIDI_CONTROL_AUDIO_FB_NOMINAL / 200u)   /* rate held to +-0.5 % */
#define MIDI_CONTROL_AUDIO_FB_TRIM_GAIN 8               /* per frame of level error */
#define MIDI_CONTROL_AUDIO_FB_TRIM_SPAN 1024            /* trim held to +-0.26 % */

struct midi_control_audio_rate {
    uint32_t snap_frames[MIDI_CONTROL_AUDIO_FB_SNAPS];
    uint32_t snap_uf[MIDI_CONTROL_AUDIO_FB_SNAPS];
    uint8_t snaps;          /* snapshots held, up to _SNAPS */
    uint8_t head;           /* the next slot written */
    uint8_t started;
    uint16_t last_uf;       /* the last microframe index seen */
    uint32_t uf;            /* microframes since the start */
    uint32_t next_ms_uf;    /* when the level is next averaged */
    int32_t level_q16;      /* the FIFO level, frames, averaged, 16.16 */
    uint32_t rate;          /* measured, 16.16 frames per microframe */
    int32_t trim;           /* from the level */
    uint32_t feedback;      /* rate + trim: what the host is sent */
    uint32_t fb_min, fb_max;    /* feedback's range once the window is full */
};

/* Nominal rate, the level at the target. */
void midi_control_audio_rate_init(struct midi_control_audio_rate *r, uint32_t target_frames);

/* One start of frame. Returns the feedback to send. */
uint32_t midi_control_audio_rate_sof(struct midi_control_audio_rate *r, uint16_t uframe,
                                     uint32_t link_frames, uint32_t level_frames,
                                     uint32_t target_frames);

/* A 16.16 frames-per-microframe value as a sample rate in millihertz. */
uint32_t midi_control_audio_rate_mhz(uint32_t rate);

#ifdef __cplusplus
}
#endif
#endif
