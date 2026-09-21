/* Analog auto-ranging: raw converter counts to a 0..127 position, and the
 * learned travel that makes both ends of that position reachable.
 *
 * A pedal's raw travel is not the same on two units, and a calibration baked
 * into a build is wrong for every unit but the one it was measured on. Worse,
 * a window that can only ever widen keeps whatever a stray sample put in it
 * for as long as the unit stays powered. So the travel is learned from use and
 * stored with the configuration (docs/05-config-model.md section 6):
 *
 *   - the window widens the instant a sample lands outside it, so a sweep past
 *     what was stored is never clipped;
 *   - it narrows onto the ends a sweep actually reached, once that sweep has
 *     covered most of the window, or once each of those ends has been left and
 *     come back to - which is what corrects a window left too wide by a stray
 *     sample, by another unit's calibration, or by a seed;
 *   - what gets stored is only ever written after a real sweep, so a transient
 *     at power-on cannot become this unit's calibration.
 *
 * No allocation, no I/O and no clock: the caller feeds samples and decides
 * when to store what this says is worth storing.
 */
#ifndef MIDI_CONTROL_ANALOG_H
#define MIDI_CONTROL_ANALOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The widest raw count the stored form can hold. The LPC4350's converter is
 * 10-bit; this limit is the record's, not the converter's, so a platform with
 * a wider one still fits. */
#define MIDI_CONTROL_CAL_RAW_MAX      0xFFFEu

/* The travel measured on the verified unit (docs/01 section 4): the window the
 * firmware seeds each control with until this unit has been swept or
 * calibrated, and the travel the emulator's pedal has unless told otherwise. A
 * starting point, never a calibration. */
#define MIDI_CONTROL_SEED_VOLUME_LO   20u
#define MIDI_CONTROL_SEED_VOLUME_HI   400u
#define MIDI_CONTROL_SEED_PEDAL_LO    280u
#define MIDI_CONTROL_SEED_PEDAL_HI    664u

/* A window narrower than this is not travel, it is noise: nothing is reported
 * from it and nothing is stored from it. */
#define MIDI_CONTROL_CAL_MIN_SPAN     32u

/* Raw counts at each end that read as the stop, so a stop is reachable without
 * its last count having to be hit exactly. */
#define MIDI_CONTROL_CAL_END_ZONE     4u

/* Raw counts of movement needed before a new position is reported, which holds
 * back the dither of a source at rest. Never applied to a move onto a stop or
 * to the first position after the window moved. */
#define MIDI_CONTROL_CAL_HYSTERESIS   8u

/* How far the learned window has to differ from the stored one, at either end,
 * before it is worth a write. */
#define MIDI_CONTROL_CAL_STORE_MARGIN 8u

/* Samples that have to agree before the window widens to take one in. A sweep
 * confirms in this many samples because each one is further out than the last,
 * and a control held against its stop confirms because the count stops moving;
 * a single sample thrown out by a transient never does. While it is
 * unconfirmed the position reads as the stop, which is where the control
 * appears to be, so nothing is lost by waiting. */
#define MIDI_CONTROL_CAL_CONFIRM      3u

/* The fraction of the window a sweep must cover before its ends are believed:
 * three quarters. Below it the control may simply not have been moved far
 * enough for its ends to be known, and adopting what it reached would turn a
 * rock over the middle of the travel into the whole of it.
 *
 * Erring low is the cheaper mistake. A window left too narrow widens again the
 * instant the control goes past it, and the position tracks through the widening;
 * a window left too wide is the fault this whole file exists to fix - the ends
 * never arrive, and nothing the user does brings them back. */
#define MIDI_CONTROL_CAL_SWEEP_NUM    3u
#define MIDI_CONTROL_CAL_SWEEP_DEN    4u

/* The rule above is measured against the window, which is the thing in doubt,
 * so on its own it deadlocks: a unit whose travel is shorter than
 * MIDI_CONTROL_CAL_SWEEP_NUM/DEN of the window it started from can never cover
 * enough of that window for its ends to be believed, so they never are, and
 * its stops stay unreachable for as long as that window lasts. Against the
 * 280..664 pedal seed, a unit that reaches only 470 at the toe stop reads 63
 * there - half travel - however many times it is swept.
 *
 * So a second rule, which asks nothing of the window: a stop is a place the
 * control comes back to. Reach an end, cross the middle of what has been
 * covered, come back to within MIDI_CONTROL_CAL_END_AGREE of where that end
 * was, and that end is mechanical - a turn made in free travel does not land
 * on the same count twice. One return at each end is enough, which a sweep
 * down, back, and down again supplies.
 *
 * The margin is wider than the end zone because a source at rest dithers, and
 * a stop that read a count or two past itself once should still be recognised
 * when it is reached again.
 *
 * It applies only to a window that started from the seeds, and only until a
 * sweep has confirmed one. A stored calibration is this unit's own travel,
 * measured by a sweep that covered it, and a session that rocks a control over
 * part of that travel without ever reaching a stop must not be allowed to
 * adopt the part as the whole; a seed has no such standing. Throwing away a
 * stored calibration is the user's to ask for -
 * midi_control_device_analog_recalibrate(). */
#define MIDI_CONTROL_CAL_END_AGREE    8u

/* What is stored, per analog control: this unit's measured travel. It belongs
 * to the hardware, not to the user's mapping, which is why a configuration
 * pushed from the app never carries it (docs/05 section 6). */
#define MIDI_CONTROL_CAL_VALID 0x01u

struct midi_control_analog_cal {
    uint16_t raw_lo;     /* raw count at the low end of the travel */
    uint16_t raw_hi;     /* raw count at the high end */
    uint8_t  flags;      /* bit 0: this unit's travel has been learned */
    uint8_t  reserved[3];
};

/* The live state. Not stored: a session starts from the calibration and the
 * platform's seeds and learns again from there. */
struct midi_control_analog_learn {
    uint16_t lo, hi;              /* the working window - what scaling uses */
    uint16_t seen_lo, seen_hi;    /* the extremes this session has reached */
    uint16_t seed_lo, seed_hi;    /* the platform's window, for an uncalibrated start */
    uint16_t pend_lo, pend_hi;    /* an edge outside the window, waiting to be confirmed */
    uint8_t  pend_lo_n, pend_hi_n;/* samples that have agreed with it so far */
    uint16_t raw_sent;            /* the raw count at the last reported position */
    int16_t  position;            /* the last reported 0..127; -1 before the first */
    uint8_t  away_lo, away_hi;    /* the control has left that end since it was last there */
    uint8_t  hit_lo, hit_hi;      /* a return agreed with the furthest that end has been */
    uint8_t  seeded;              /* the window is the platform's seed, not a stored travel */
    uint8_t  invert;              /* 1: a falling raw count is a rising position */
    uint8_t  swept;               /* 1 once a sweep has confirmed the window's ends */
    uint8_t  window_moved;        /* the last update widened or narrowed the window */
};

/* Start from `cal` if it has been calibrated, from `seed_lo`..`seed_hi`
 * otherwise. The seeds are kept, so a reset can go back to them. `cal` may be
 * NULL, which reads as uncalibrated. */
void midi_control_analog_learn_init(struct midi_control_analog_learn *l,
                                    const struct midi_control_analog_cal *cal,
                                    uint16_t seed_lo, uint16_t seed_hi, int invert);

/* Forget the calibration and the session, back to the seeds. */
void midi_control_analog_learn_reset(struct midi_control_analog_learn *l);

/* Feed one raw count. Returns 1 when the position changed, in which case
 * `l->position` is the new 0..127; 0 when there is nothing to report. Either
 * way `l->window_moved` says whether this sample moved the window, which is
 * how a caller tells a sweep still under way from one that has finished. */
int midi_control_analog_learn_update(struct midi_control_analog_learn *l, uint16_t raw);

/* Fills `out` with what the learned window would store, and returns 1 when
 * that is worth writing over `cal`: a sweep has confirmed the window's ends,
 * and they differ from the stored ones by MIDI_CONTROL_CAL_STORE_MARGIN or
 * more - in either direction, so a travel narrower than what is stored is
 * corrected as readily as a wider one. Nothing is ever stored from a window a
 * sweep has not confirmed, so a transient cannot become a calibration. `cal`
 * may be NULL. */
int midi_control_analog_learn_store(const struct midi_control_analog_learn *l,
                                    const struct midi_control_analog_cal *cal,
                                    struct midi_control_analog_cal *out);

/* ---- calibration by capture ------------------------------------------------ */
/* The learning above works the travel out from ordinary use. This asks for it
 * instead: the user holds the control at one end, then at the other, and each
 * end is taken as the count the control settles on there. Nothing it does is
 * measured against the window, so it cannot deadlock the way coverage can, and
 * it is the way out of a stored window that no amount of sweeping will narrow
 * (docs/05 section 6, docs/09).
 *
 * Either end may come first. What is stored is a low count and a high one;
 * which of them is the toe is the platform's `invert`, not the user's to say.
 *
 * A stop taken short of the real one is the cheap mistake here too: the window
 * widens past it the first time the control goes further, and the position
 * tracks through the widening. */

/* How long the count has to stay put for a stop to be taken. */
#define MIDI_CONTROL_CAL_HOLD_MS  1000u

/* How far the count may wander, either way from where it came to rest, and
 * still be at rest: a source standing still dithers a count or two, and the
 * pedal's is averaged over eight conversions before it gets here. */
#define MIDI_CONTROL_CAL_STILL    6u

/* A capture that has taken nothing for this long is abandoned, and whatever
 * calibration was there before stands. */
#define MIDI_CONTROL_CAL_IDLE_MS  30000u

enum midi_control_cal_stage {
    MIDI_CONTROL_CAL_OFF = 0,     /* not calibrating */
    MIDI_CONTROL_CAL_FIRST,       /* waiting for the control to be held at one end */
    MIDI_CONTROL_CAL_SECOND,      /* one end taken; waiting for the other */
    MIDI_CONTROL_CAL_DONE,        /* both ends taken */
    MIDI_CONTROL_CAL_FAILED       /* cancelled or abandoned: nothing was taken */
};

struct midi_control_analog_capture {
    uint8_t  stage;               /* enum midi_control_cal_stage */
    uint8_t  anchored;            /* the count has come to rest at `anchor` */
    uint16_t raw;                 /* the latest count */
    uint16_t anchor;              /* where it came to rest */
    uint16_t still_lo, still_hi;  /* the counts it has dithered over since */
    uint32_t still_ms;            /* how long it has been at rest there */
    uint32_t idle_ms;             /* since this stage began */
    uint16_t first, second;       /* the ends taken, in the order they were taken */
};

/* From the first stage, with nothing taken. */
void midi_control_analog_capture_start(struct midi_control_analog_capture *c);

/* Feed one raw count. A count further than MIDI_CONTROL_CAL_STILL from where
 * the control came to rest means it is moving again, and the time at rest
 * starts over from the new count. */
void midi_control_analog_capture_sample(struct midi_control_analog_capture *c, uint16_t raw);

/* Time passing, which is what a hold is made of. `may_rest` is 0 while
 * something is pressing the control past where it rests - the pedal with its
 * toe switch held - so that a stop is taken where the control stays, not where
 * a foot forces it; the rest is then measured, and the hold timed, from when
 * that stops. An end is not
 * taken twice: at the second stage, resting within MIDI_CONTROL_CAL_MIN_SPAN
 * of the first end is not the other end and counts for nothing. Returns 1 when
 * the stage changed. */
int midi_control_analog_capture_tick(struct midi_control_analog_capture *c, uint32_t elapsed_ms, int may_rest);

/* How far through the current hold, 0..127: for a progress bar. */
unsigned midi_control_analog_capture_progress(const struct midi_control_analog_capture *c);

/* The travel the two ends make, low count first, marked valid. Returns 1 once
 * both have been taken, 0 before - when `out` is left alone. */
int midi_control_analog_capture_result(const struct midi_control_analog_capture *c,
                                       struct midi_control_analog_cal *out);

#ifdef __cplusplus
}
#endif
#endif
