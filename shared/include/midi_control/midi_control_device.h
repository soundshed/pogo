/* The device: configuration, engine, protocol and storage, tied together.
 *
 * This is the whole of what the controller *is*, minus the hardware. The
 * firmware wraps it in a board layer (scan the panel, read the ADCs, move USB
 * bytes); the emulator wraps it in a window. Both call exactly these
 * functions, so a behaviour that works in the emulator is the behaviour the
 * unit has.
 *
 * Threading: none. Every call is expected from one thread (the firmware's
 * main loop; the app's message thread).
 */
#ifndef MIDI_CONTROL_DEVICE_H
#define MIDI_CONTROL_DEVICE_H

#include <stdint.h>
#include "midi_control_analog.h"
#include "midi_control_config.h"
#include "midi_control_engine.h"
#include "midi_control_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_CONTROL_STORAGE_SLOTS   2u
#define MIDI_CONTROL_TEXT_LINES      2u
#define MIDI_CONTROL_TEXT_LEN        32u

/* How long a learned analog travel has to sit unchanged before it is written.
 * Learning happens on every sample; the card is written once the sweep that
 * moved it is over, not while it is under way. */
#define MIDI_CONTROL_CAL_SETTLE_MS   10000u

/* The profile picker (docs/05 section 7). Mode held this long opens it; it
 * closes itself again after this long with nothing pressed, so a unit is never
 * left on stage with its footswitches answering a menu nobody meant to open. */
#define MIDI_CONTROL_PROFILE_HOLD_MS   800u
#define MIDI_CONTROL_PROFILE_IDLE_MS  5000u

/* Calibrating the pedal by capture (docs/05 section 6). The toe switch held
 * this long starts it, as CALIBRATE does - longer than the picker's hold,
 * because a toe switch mapped to a momentary action is held on purpose and
 * this must not open in the middle of a song. The outcome, taken or not, stays
 * on the screen this long before the performance view comes back. */
#define MIDI_CONTROL_CAL_ENTER_HOLD_MS 2000u
#define MIDI_CONTROL_CAL_SHOW_MS       3000u

/* What the platform provides. Any function may be NULL: a NULL midi_out
 * discards output; a NULL storage makes the device report
 * MIDI_CONTROL_STORAGE_NONE and refuse to save. */
struct midi_control_platform_ops {
    void *ctx;

    /* MIDI bytes to the host: complete channel messages or a complete SysEx. */
    void (*midi_out)(void *ctx, const uint8_t *bytes, uint32_t len);

    /* enum midi_control_storage_state - asked at init and before every save. */
    int (*storage_state)(void *ctx);

    /* Read a slot into `buf` (`len` bytes). Returns 1 on success. A slot that
     * has never been written may return 1 with garbage; the record's own CRC
     * decides whether it is valid. */
    int (*storage_read)(void *ctx, unsigned slot, uint8_t *buf, uint32_t len);

    /* Write `len` bytes to a slot, read them back and compare. Returns a
     * MIDI_CONTROL_STATUS_* code: OK, WRITE_FAILED or VERIFY_FAILED. */
    int (*storage_write)(void *ctx, unsigned slot, const uint8_t *buf, uint32_t len);

    /* Identity the device reports. */
    uint8_t platform;               /* enum midi_control_platform */
    uint8_t fw_major, fw_minor, fw_patch;
};

struct midi_control_device {
    struct midi_control_platform_ops ops;
    struct midi_control_config config;         /* the live configuration */
    struct midi_control_engine engine;

    uint8_t dirty;                      /* config differs from the loaded/saved copy */
    uint8_t loaded_from_storage;        /* 1 if init found a valid slot */
    uint8_t last_save_status;           /* MIDI_CONTROL_STATUS_* of the last SAVE */
    unsigned newest_slot;               /* slot the live config was loaded from / saved to */

    /* Analog auto-ranging, for a platform that reads raw converter counts. A
     * platform that delivers a position 0..127 already - the emulator, an
     * EVENT from the app - never seeds these and never uses them. */
    struct midi_control_analog_learn analog_learn[MIDI_CONTROL_ANALOG_COUNT];
    uint8_t analog_raw[MIDI_CONTROL_ANALOG_COUNT];   /* 1: seeded, fed raw counts */
    uint8_t cal_dirty;                  /* a learned travel is worth writing */
    uint32_t cal_settle_ms;             /* since cal_dirty was set, or since the last attempt */

    /* The display timeout (global display_timeout, docs/05 section 2). */
    uint32_t idle_ms;                   /* since a control was last used; saturates */
    uint8_t display_off;                /* 1 once idle_ms has reached the timeout */

    /* The profile picker (docs/05 section 7). Mode is watched here rather than
     * in the engine because holding a switch is a gesture on top of whatever
     * the mapping makes that switch do, not a mapping of its own. */
    uint8_t profile_picking;            /* 1: the picker is open; A-D and Bank Up/Down are its own */
    uint8_t profile_page;               /* the page of four it shows */
    uint8_t mode_down;                  /* Mode is held */
    uint8_t mode_hold_used;             /* this hold has already opened or closed the picker */
    uint32_t mode_held_ms;              /* how long Mode has been down */
    uint32_t pick_idle_ms;              /* since the last press while picking */
    uint32_t picker_took;               /* one bit per switch whose press the picker swallowed */

    /* Calibrating the pedal by capture (docs/05 section 6): its two stops,
     * taken as the user holds it at each. While a capture runs the pedal is
     * measured, not played - it sends nothing and its learner is not fed. */
    struct midi_control_analog_capture capture;
    uint8_t capture_analog;             /* the control being captured: the pedal */
    uint32_t capture_shown_ms;          /* how long DONE or FAILED has been on the screen */
    uint8_t cal_unsaved;                /* a captured travel is live but not stored yet */
    uint8_t cal_write_now;              /* ...and is written at the next tick, not after a settle */
    uint8_t toe_down;                   /* the toe switch is held */
    uint8_t toe_hold_used;              /* this hold has already started a capture */
    uint32_t toe_held_ms;               /* how long the toe switch has been down */
    uint16_t analog_last[MIDI_CONTROL_ANALOG_COUNT];   /* each raw-fed control's latest count */

    /* Host-supplied display text, e.g. the preset name. */
    char text[MIDI_CONTROL_TEXT_LINES][MIDI_CONTROL_TEXT_LEN + 1u];

    /* Incoming MIDI byte-stream parser state. */
    uint8_t rx[MIDI_CONTROL_SYSEX_MAX];
    uint32_t rx_len;
    uint8_t rx_in_sysex;
    uint8_t rx_status;                  /* running status for channel messages */
    uint8_t rx_need;                    /* data bytes still expected */

    uint8_t tx[MIDI_CONTROL_SYSEX_MAX];        /* reply scratch */
};

/* Loads the newest valid slot from storage, or the defaults. */
void midi_control_device_init(struct midi_control_device *d, const struct midi_control_platform_ops *ops);

/* Bytes from the host, in any chunking - a whole message, or one byte at a
 * time. Channel messages go to the engine; our SysEx is handled and answered;
 * other SysEx is ignored. */
void midi_control_device_midi_in(struct midi_control_device *d, const uint8_t *bytes, uint32_t len);

/* Control events, forwarded to the engine. */
void midi_control_device_button(struct midi_control_device *d, enum midi_control_button b, int pressed);
void midi_control_device_encoder(struct midi_control_device *d, enum midi_control_encoder e, int detents);
void midi_control_device_analog(struct midi_control_device *d, enum midi_control_analog a, uint8_t value);

/* ---- raw analog inputs ---------------------------------------------------- */
/* Seed one analog control's auto-range: the window to scale from until this
 * unit has been swept, and whether its raw count falls as the position rises.
 * A platform that reads raw converter counts calls this once per control after
 * init. The stored calibration, if there is one, takes precedence over the
 * seeds - and the device re-seeds itself whenever storage replaces it. */
void midi_control_device_analog_seed(struct midi_control_device *d, enum midi_control_analog a,
                                     uint16_t seed_lo, uint16_t seed_hi, int invert);

/* One raw converter count for a seeded control. Learns this unit's travel
 * (midi_control_analog.h), scales it onto 0..127, and forwards the position to
 * the engine when it changes. A count that changes the position is a control
 * used, so it lights the display like any other; one that does not - a pedal
 * dithering where it was left - is not, and leaves it dark. When what has been
 * learned differs enough from what is stored, the next quiet moment writes it -
 * see midi_control_device_tick(). */
void midi_control_device_analog_raw(struct midi_control_device *d, enum midi_control_analog a, uint16_t raw);

/* Forget every learned travel, live and stored, and start again from the
 * seeds. LOAD_DEFAULTS does this; SET_CONFIG never does, so a configuration
 * pushed from the app or loaded from a file cannot put another unit's pedal
 * calibration on this one. */
void midi_control_device_analog_recalibrate(struct midi_control_device *d);

/* Start (start != 0) or cancel calibrating analog control `a` by capture: the
 * user holds it at one end, then at the other, and those two counts become its
 * calibration (midi_control_analog.h). Only the pedal is calibrated this way,
 * and only on a platform that feeds its raw counts; anything else is
 * INVALID_VALUE, as there is nothing to measure. Starting again while one runs
 * starts over. Cancelling leaves the calibration exactly as it was, and is OK
 * when nothing is running. Returns a MIDI_CONTROL_STATUS_* code.
 *
 * On the panel, holding the toe switch for MIDI_CONTROL_CAL_ENTER_HOLD_MS
 * starts it and Mode cancels it. Both switches still send what they are mapped
 * to: the hold is a gesture on top of the mapping, as Mode's is for the
 * profile picker.
 *
 * A travel taken is live at once. midi_control_device_tick() writes it at the
 * next tick when storage is READY and the user has no unsaved edits of their
 * own; otherwise it goes with their next SAVE, which is what a RELOAD first
 * would throw away. */
int midi_control_device_calibrate(struct midi_control_device *d, enum midi_control_analog a, int start);

/* The capture's enum midi_control_cal_stage: MIDI_CONTROL_CAL_OFF when none is
 * running or being shown. */
int midi_control_device_calibration(const struct midi_control_device *d);

/* Time passing on the platform's clock: `elapsed_ms` since the last call. This
 * is also where a learned analog travel is written: once it has sat unchanged
 * for MIDI_CONTROL_CAL_SETTLE_MS, storage is READY and the user has no unsaved
 * edits of their own, the configuration is saved with the new calibration in
 * it. A save the user has not asked for never commits edits they have not
 * committed, so a dirty configuration defers it until they do.
 *
 * It also times the holds a calibration by capture is made of - the toe
 * switch's, and the pedal's at each stop - and keeps the display lit while one
 * is on the screen.
 *
 * Once no control has been used for the global display_timeout, the display goes
 * dark. A control used - on the panel or by EVENT, whatever it is mapped to -
 * lights it and restarts the count, and still does what it does; so does a new
 * configuration (SET_CONFIG, LOAD_DEFAULTS, RELOAD). The host's MIDI, SET_TEXT
 * and GET_STATE do not. A timeout of 0 never darkens it. */
void midi_control_device_tick(struct midi_control_device *d, uint32_t elapsed_ms);

/* 0 while the display timeout has the display dark. */
int midi_control_device_display_on(const struct midi_control_device *d);

/* ---- profiles ------------------------------------------------------------- */
/* Whole named mappings the unit switches between (docs/05 section 7). On the
 * panel: hold Mode to open the picker, then A-D choose and Bank Up / Bank Down
 * page through them. While the picker is open those six switches are its own
 * and send nothing; every other control still does what it is mapped to, Mode
 * included - the hold is a gesture on top of the mapping, not instead of it.
 *
 * Choosing a profile replaces the live mapping, so the device is dirty
 * afterwards exactly as a SET_CONFIG leaves it: the unit powers up in whatever
 * profile was last saved, and saving is how the player says which that is. */

/* Make profile `n` live, as choosing it on the panel does. Returns a
 * MIDI_CONTROL_STATUS_* code: INVALID_VALUE for a profile that is not in use. */
int midi_control_device_select_profile(struct midi_control_device *d, unsigned n);

/* Open or close the picker without waiting out the hold. Opening shows the
 * page the active profile is on. */
void midi_control_device_show_profiles(struct midi_control_device *d, int open);

/* 1 while the picker is on the screen. */
int midi_control_device_picking_profile(const struct midi_control_device *d);

/* How many pages of MIDI_CONTROL_PROFILE_PAGE the profiles in use fill; at least 1. */
unsigned midi_control_device_profile_pages(const struct midi_control_device *d);

/* The profile footswitch `b` chooses on the page the picker shows, or -1: a
 * switch that is not A-D, or a place on the page past the last profile in use. */
int midi_control_device_picker_profile(const struct midi_control_device *d, enum midi_control_button b);

/* Replace the live configuration (sanitised) without persisting. */
void midi_control_device_set_config(struct midi_control_device *d, const struct midi_control_config *cfg);

/* Persist the live configuration. Returns a MIDI_CONTROL_STATUS_* code. Writes the
 * slot that does not hold the current newest record, with the sequence
 * incremented, so a failed write leaves the previous save intact. */
int midi_control_device_save(struct midi_control_device *d);

/* Re-read storage, discarding live changes. Returns a MIDI_CONTROL_STATUS_* code. */
int midi_control_device_reload(struct midi_control_device *d);

int midi_control_device_storage_state(const struct midi_control_device *d);

/* The LEDs, one MIDI_CONTROL_LED_* byte per column, into `out`
 * (MIDI_CONTROL_COLUMNS bytes): the engine's, or the profile picker's on the
 * switches it has taken while it is open. */
void midi_control_device_leds(const struct midi_control_device *d, uint8_t *out);

/* What a STATE reply says about this device. */
void midi_control_device_state(const struct midi_control_device *d, struct midi_control_state_reply *st);

/* Take on the state another device reported, sending nothing: the program,
 * the switches, the knobs, the text, the unsaved flag, whether the display is
 * dark, and a calibration on its screen. The storage state stays this device's own platform's. A host draws
 * its picture of a unit from
 * a device of its own mirrored like this; with the same configuration it
 * draws the screen and lights the LEDs the unit does. */
void midi_control_device_mirror(struct midi_control_device *d, const struct midi_control_state_reply *st);

#ifdef __cplusplus
}
#endif
#endif
