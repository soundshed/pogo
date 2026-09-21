#include <string.h>
#include "midi_control/midi_control_analog.h"

static uint16_t clamp_raw(uint16_t raw)
{
    return raw > MIDI_CONTROL_CAL_RAW_MAX ? (uint16_t)MIDI_CONTROL_CAL_RAW_MAX : raw;
}

static unsigned apart(unsigned a, unsigned b)
{
    return a > b ? a - b : b - a;
}

/* Zero is not a travel, so a record of zeroes reads as "never calibrated", and
 * so does one too narrow to scale from. */
static int calibrated(const struct midi_control_analog_cal *cal)
{
    return cal && (cal->flags & MIDI_CONTROL_CAL_VALID)
        && cal->raw_hi > cal->raw_lo
        && (unsigned)(cal->raw_hi - cal->raw_lo) >= MIDI_CONTROL_CAL_MIN_SPAN;
}

static void start(struct midi_control_analog_learn *l, uint16_t lo, uint16_t hi)
{
    l->lo = lo;
    l->hi = hi;
    /* Nothing seen yet: an empty range the first sample collapses onto itself,
     * so seen_hi - seen_lo is only ever a span this session has reached. */
    l->seen_lo = (uint16_t)MIDI_CONTROL_CAL_RAW_MAX;
    l->seen_hi = 0u;
    l->pend_lo = lo;
    l->pend_hi = hi;
    l->pend_lo_n = 0u;
    l->pend_hi_n = 0u;
    /* No end has been left yet, so the position the control is in at the start
     * is a first visit and not a return: a control that has not moved cannot
     * have confirmed anything. */
    l->away_lo = 0u;
    l->away_hi = 0u;
    l->hit_lo = 0u;
    l->hit_hi = 0u;
    l->raw_sent = 0u;
    l->position = -1;
    l->swept = 0u;
    l->window_moved = 0u;
}

void midi_control_analog_learn_init(struct midi_control_analog_learn *l,
                                    const struct midi_control_analog_cal *cal,
                                    uint16_t seed_lo, uint16_t seed_hi, int invert)
{
    memset(l, 0, sizeof(*l));

    seed_lo = clamp_raw(seed_lo);
    seed_hi = clamp_raw(seed_hi);
    if (seed_lo > seed_hi) {
        uint16_t t = seed_lo;
        seed_lo = seed_hi;
        seed_hi = t;
    }
    l->seed_lo = seed_lo;
    l->seed_hi = seed_hi;
    l->invert = invert ? 1u : 0u;

    if (calibrated(cal)) {
        start(l, cal->raw_lo, cal->raw_hi);
        l->seeded = 0u;
    } else {
        start(l, seed_lo, seed_hi);
        l->seeded = 1u;
    }
}

void midi_control_analog_learn_reset(struct midi_control_analog_learn *l)
{
    start(l, l->seed_lo, l->seed_hi);
    l->seeded = 1u;
}

/* Widening, one end at a time, once MIDI_CONTROL_CAL_CONFIRM samples have
 * agreed that the travel really goes out there. A sweep agrees with itself -
 * each sample is further out than the last, and once the control is against
 * its stop the count stops moving - so the window is out within a few samples
 * of the end of the travel being reached. A lone sample thrown out by a
 * transient is replaced by the next one and never counts. */
static int widen(uint16_t raw, uint16_t *edge, uint16_t *pend, uint8_t *n, int below)
{
    int further = below ? raw <= *pend : raw >= *pend;

    if (!further) {
        *pend = raw;
        *n = 1u;
        return 0;
    }
    *pend = raw;
    if (*n < 0xFFu)
        (*n)++;
    if (*n < MIDI_CONTROL_CAL_CONFIRM)
        return 0;
    *edge = raw;
    return 1;
}

int midi_control_analog_learn_update(struct midi_control_analog_learn *l, uint16_t raw)
{
    unsigned span, seen, scaled;
    uint16_t cov_lo, cov_hi;

    raw = clamp_raw(raw);
    l->window_moved = 0u;

    if (raw < l->seen_lo)
        l->seen_lo = raw;
    if (raw > l->seen_hi)
        l->seen_hi = raw;

    if (raw < l->lo) {
        if (widen(raw, &l->lo, &l->pend_lo, &l->pend_lo_n, 1))
            l->window_moved = 1u;
    } else {
        l->pend_lo = l->lo;
        l->pend_lo_n = 0u;
    }
    if (raw > l->hi) {
        if (widen(raw, &l->hi, &l->pend_hi, &l->pend_hi_n, 0))
            l->window_moved = 1u;
    } else {
        l->pend_hi = l->hi;
        l->pend_hi_n = 0u;
    }

    span = (unsigned)(l->hi - l->lo);

    /* How much of the window the session has actually covered. The extremes
     * are clamped into it first: a sample still waiting to be confirmed is
     * outside the window, and counting it would make a nudge at one end look
     * like a sweep and pull the other end in behind it. */
    cov_lo = l->seen_lo < l->lo ? l->lo : l->seen_lo;
    cov_hi = l->seen_hi > l->hi ? l->hi : l->seen_hi;
    seen = cov_hi > cov_lo ? (unsigned)(cov_hi - cov_lo) : 0u;

    /* Returns to each end. Leaving an end means crossing the middle of what
     * has been covered, so a dither at the end is not a departure; coming back
     * within MIDI_CONTROL_CAL_END_AGREE of where that end was is the return
     * that makes it a stop (midi_control_analog.h). Both ends are tracked
     * whether or not they will be needed, because which rule ends up believing
     * the window is not known until it is. */
    if (seen >= MIDI_CONTROL_CAL_MIN_SPAN) {
        unsigned mid = (unsigned)cov_lo + seen / 2u;

        if ((unsigned)raw > mid)
            l->away_lo = 1u;
        else if (l->away_lo && (unsigned)raw <= (unsigned)cov_lo + MIDI_CONTROL_CAL_END_AGREE) {
            l->away_lo = 0u;
            l->hit_lo = 1u;
        }

        if ((unsigned)raw < mid)
            l->away_hi = 1u;
        else if (l->away_hi && (unsigned)raw + MIDI_CONTROL_CAL_END_AGREE >= (unsigned)cov_hi) {
            l->away_hi = 0u;
            l->hit_hi = 1u;
        }
    }

    /* Narrow onto the ends a sweep actually reached, on either of the two
     * pieces of evidence that they are the travel's ends rather than wherever
     * the control happened to be rocked between: the sweep covered most of the
     * window, or each end was left and come back to. This is the half that a
     * window which can only widen never had: it is what takes a seed, another
     * unit's calibration, or a window a transient left too wide, and brings it
     * back onto this unit's own travel. The second piece of evidence is only
     * accepted until the first arrives, so it corrects a window and then stops
     * (midi_control_analog.h). */
    if (seen >= MIDI_CONTROL_CAL_MIN_SPAN
        && (seen * MIDI_CONTROL_CAL_SWEEP_DEN >= span * MIDI_CONTROL_CAL_SWEEP_NUM
            || (l->seeded && !l->swept && l->hit_lo && l->hit_hi))) {
        l->swept = 1u;
        if (cov_lo > l->lo || cov_hi < l->hi) {
            l->lo = cov_lo;
            l->hi = cov_hi;
            span = seen;
            l->window_moved = 1u;
        }
    }

    if (span < MIDI_CONTROL_CAL_MIN_SPAN)
        return 0;

    if (raw + MIDI_CONTROL_CAL_END_ZONE >= (unsigned)l->hi)
        scaled = 127u;
    else if (raw <= (unsigned)l->lo + MIDI_CONTROL_CAL_END_ZONE)
        scaled = 0u;
    else
        scaled = ((unsigned)(raw - l->lo) * 127u + span / 2u) / span;
    if (scaled > 127u)
        scaled = 127u;
    if (l->invert)
        scaled = 127u - scaled;

    if ((int16_t)scaled == l->position)
        return 0;

    /* Hysteresis, except where it would cost something that has to be exact:
     * the first position of the session, a move onto a stop, and the step the
     * window itself just moved under. */
    if (l->position >= 0 && !l->window_moved && scaled != 0u && scaled != 127u
        && apart(raw, l->raw_sent) < MIDI_CONTROL_CAL_HYSTERESIS)
        return 0;

    l->position = (int16_t)scaled;
    l->raw_sent = raw;
    return 1;
}

int midi_control_analog_learn_store(const struct midi_control_analog_learn *l,
                                    const struct midi_control_analog_cal *cal,
                                    struct midi_control_analog_cal *out)
{
    memset(out, 0, sizeof(*out));
    out->raw_lo = l->lo;
    out->raw_hi = l->hi;
    out->flags = MIDI_CONTROL_CAL_VALID;

    /* Until a sweep has confirmed the ends, the window is the seed or the
     * stored calibration with at most a transient on it. That is not this
     * unit's travel and it does not go on the card. */
    if (!l->swept || (unsigned)(l->hi - l->lo) < MIDI_CONTROL_CAL_MIN_SPAN)
        return 0;
    if (!calibrated(cal))
        return 1;
    return apart(l->lo, cal->raw_lo) >= MIDI_CONTROL_CAL_STORE_MARGIN
        || apart(l->hi, cal->raw_hi) >= MIDI_CONTROL_CAL_STORE_MARGIN;
}
