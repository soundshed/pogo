/* The configuration: what every control does, and the global settings.
 *
 * One fixed-size record, MIDI_CONTROL_CONFIG_BYTES long, with an explicit
 * little-endian byte layout (docs/05-config-model.md) that is the same on the
 * wire, on the eMMC and in the emulator's file. midi_control_config_write() and
 * midi_control_config_read() convert between that layout and the struct below, so no
 * code anywhere depends on how a compiler packs the struct.
 *
 * The struct is what the engine reads. It is plain data: copy it, compare it,
 * keep several.
 */
#ifndef MIDI_CONTROL_CONFIG_H
#define MIDI_CONTROL_CONFIG_H

#include <stdint.h>
#include "midi_control_analog.h"
#include "midi_control_controls.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_CONTROL_CONFIG_MAGIC    0x47444F50u    /* "PODG" as little-endian bytes */
#define MIDI_CONTROL_CONFIG_VERSION  4u
#define MIDI_CONTROL_CONFIG_BYTES    4400u

/* The older versions, all still read and upgraded in place, though every
 * record written is version 4 (docs/05-config-model.md sections 8 and 11).
 * Version 3 had no program names; version 2 no profile block either, so the
 * mapping it carries becomes profile 1; version 1 no analog calibration. */
#define MIDI_CONTROL_CONFIG_VERSION_V1 1u
#define MIDI_CONTROL_CONFIG_BYTES_V1   280u
#define MIDI_CONTROL_CONFIG_VERSION_V2 2u
#define MIDI_CONTROL_CONFIG_BYTES_V2   296u
#define MIDI_CONTROL_CONFIG_VERSION_V3 3u
#define MIDI_CONTROL_CONFIG_BYTES_V3   2352u

#define MIDI_CONTROL_CONFIG_NAME_LEN 16u
#define MIDI_CONTROL_DISPLAY_TIMEOUT_MAX 3600u   /* seconds: an hour */

/* Profiles: whole named mappings the unit switches between, chosen on the
 * panel by holding Mode (docs/05 section 7). Eight of them are always present
 * in the record; `profile_count` says how many are in use, and only those can
 * be selected. Four fit a page of footswitches, so eight is two pages.
 *
 * The count is what the record is built for, so raising it is a new
 * MIDI_CONTROL_CONFIG_VERSION. It is bounded by the firmware's AHB SRAM, which
 * the USB controller's DMA structures share (firmware/midi_control.ld). */
#define MIDI_CONTROL_PROFILE_COUNT    8u
#define MIDI_CONTROL_PROFILE_PAGE     4u    /* footswitches A-D: one page of the picker */
#define MIDI_CONTROL_PROFILE_NAME_LEN 16u

/* Program names: one for each program the unit can select, 0..127, shown as
 * the title beside the bank and preset while that program is the current one
 * (docs/05 section 8). Keyed by the program, not by the bank and letter, so a
 * name stays with its program when the bank size changes. An empty name is
 * no name. They are global, like the bank size that labels them: they name
 * the presets of whatever receives the Program Changes, whichever profile
 * sends them. */
#define MIDI_CONTROL_PROGRAM_COUNT     128u
#define MIDI_CONTROL_PROGRAM_NAME_LEN  16u    /* 15 characters and the NUL */

/* What a button does. The value fields mean different things per action; the
 * names below say which. */
enum midi_control_button_action {
    MIDI_CONTROL_ACT_NONE = 0,
    MIDI_CONTROL_ACT_NOTE_MOMENTARY,  /* Note On (number, value_on) at press, Note Off at release */
    MIDI_CONTROL_ACT_NOTE_TOGGLE,     /* alternate Note On / Note Off on each press */
    MIDI_CONTROL_ACT_CC_MOMENTARY,    /* CC number = value_on at press, value_off at release */
    MIDI_CONTROL_ACT_CC_TOGGLE,       /* alternate CC number = value_on / value_off per press */
    MIDI_CONTROL_ACT_PROGRAM,         /* Program Change to `number` + the engine's program_offset */
    MIDI_CONTROL_ACT_PROGRAM_UP,      /* program += number (1 if 0), wrapping in 0..127 */
    MIDI_CONTROL_ACT_PROGRAM_DOWN,    /* program -= number (1 if 0) */
    MIDI_CONTROL_ACT_BANK_UP,         /* program += global bank_size; flags may move the Program Changes too */
    MIDI_CONTROL_ACT_BANK_DOWN,       /* program -= global bank_size; likewise */
    MIDI_CONTROL_ACT_PRESET_IN_BANK,  /* program = current bank * bank_size + number (0..bank_size-1) */
    MIDI_CONTROL_BUTTON_ACTION_COUNT
};

enum midi_control_encoder_mode {
    MIDI_CONTROL_ENCM_NONE = 0,
    MIDI_CONTROL_ENCM_CC_RELATIVE_TWOS,    /* CC: +n -> n, -n -> 128-n  (1 / 127 for one step) */
    MIDI_CONTROL_ENCM_CC_RELATIVE_OFFSET,  /* CC: 64 + n                (65 / 63 for one step) */
    MIDI_CONTROL_ENCM_CC_ABSOLUTE,         /* CC: an accumulated 0..127 value, `step` per detent */
    MIDI_CONTROL_ENCM_PROGRAM,             /* Program Change, stepping the program by `step` */
    MIDI_CONTROL_ENCODER_MODE_COUNT
};

enum midi_control_analog_mode {
    MIDI_CONTROL_ANM_NONE = 0,
    MIDI_CONTROL_ANM_CC,                   /* CC number, out_min..out_max */
    MIDI_CONTROL_ANM_PITCH_BEND,           /* 14-bit pitch bend, out_min..out_max scaled to 0..16383 */
    MIDI_CONTROL_ANM_CHANNEL_PRESSURE,     /* channel aftertouch, out_min..out_max */
    MIDI_CONTROL_ANALOG_MODE_COUNT
};

/* `channel` on every control: 0 means "the global channel", 1..16 is an
 * explicit channel. */
#define MIDI_CONTROL_CHANNEL_GLOBAL 0u

/* A button's flags, in its last byte (docs/05 section 3). The byte was
 * reserved, and every record written before the flags has it zero, which is
 * none of them. */
#define MIDI_CONTROL_BUTTON_MOVES_PROGRAMS 0x01u   /* Bank up / down: move the Program Change buttons a bank too */

struct midi_control_button_map {
    uint8_t action;      /* enum midi_control_button_action */
    uint8_t channel;     /* 0 = global, 1..16 */
    uint8_t number;      /* note / CC number / program / step / preset index */
    uint8_t value_on;    /* velocity, or CC value when pressed / on */
    uint8_t value_off;   /* CC value when released / off */
    uint8_t led_on;      /* MIDI_CONTROL_LED_* while active (held, or toggled on) */
    uint8_t led_off;     /* MIDI_CONTROL_LED_* otherwise */
    uint8_t flags;       /* MIDI_CONTROL_BUTTON_*; only Bank up and Bank down have any */
};

struct midi_control_encoder_map {
    uint8_t mode;        /* enum midi_control_encoder_mode */
    uint8_t channel;
    uint8_t number;      /* CC number */
    uint8_t step;        /* units per detent, 1..64 (0 reads as 1) */
    uint8_t value;       /* initial value for the absolute mode */
    uint8_t reserved[3];
};

/* The most of the travel either end's padding may take, in percent. At the
 * limit a fifth of the travel is still left to sweep through. */
#define MIDI_CONTROL_ANALOG_PAD_MAX 40u

struct midi_control_analog_map {
    uint8_t mode;        /* enum midi_control_analog_mode */
    uint8_t channel;
    uint8_t number;      /* CC number */
    uint8_t invert;      /* 1: heel / anticlockwise reads 127 */
    uint8_t out_min;     /* 0..127 */
    uint8_t out_max;     /* 0..127 */
    /* Padding: the percentage of the travel at each end that reads as that
     * end, so a pedal that settles a little off its stop as the foot comes
     * away still sends exactly the end value. 0..MIDI_CONTROL_ANALOG_PAD_MAX.
     * They are the physical ends, taken before `invert`: pad_lo is heel /
     * anticlockwise, pad_hi toe / clockwise (docs/05 section 5). */
    uint8_t pad_lo;
    uint8_t pad_hi;
};

struct midi_control_global {
    uint8_t midi_channel;          /* 0..15 (shown as 1..16) */
    uint8_t bank_size;             /* presets per bank, 1..16; the POD Go's UI uses 4 */
    uint8_t bank_select;           /* 1: send CC#0 / CC#32 = 0 before each Program Change */
    uint8_t follow_program_change; /* 1: a Program Change from the host moves the display */
    uint8_t startup_program;       /* program shown (and used for bank arithmetic) at power-on */
    uint8_t led_brightness;        /* reserved: PWM master brightness, 0..255 */
    uint16_t display_timeout;      /* seconds with no control used before the display goes dark; 0 never */
    char    name[MIDI_CONTROL_CONFIG_NAME_LEN]; /* shown as the display title; NUL padded */
    uint8_t reserved1[8];
};

/* One named mapping. Everything a control does and nothing about the device:
 * the MIDI channel, the bank size, the display timeout and the learned
 * calibration stay in the global block, because they describe the unit rather
 * than the rig (docs/05 section 7). */
struct midi_control_profile {
    char name[MIDI_CONTROL_PROFILE_NAME_LEN];   /* shown in the picker; NUL padded */
    struct midi_control_button_map  buttons[MIDI_CONTROL_BUTTON_COUNT];
    struct midi_control_encoder_map encoders[MIDI_CONTROL_ENCODER_COUNT];
    struct midi_control_analog_map  analogs[MIDI_CONTROL_ANALOG_COUNT];
};

struct midi_control_config {
    uint32_t sequence;     /* incremented by every save; picks the newer of two slots */
    struct midi_control_global global;

    /* The live mapping: what the engine plays. It is always a copy of
     * profiles[profile_active] - midi_control_config_sanitise() copies it back,
     * so an edit to the live mapping is an edit to the active profile and the
     * two can never drift. The live copy wins, because that is the block every
     * caller edits. */
    struct midi_control_button_map  buttons[MIDI_CONTROL_BUTTON_COUNT];
    struct midi_control_encoder_map encoders[MIDI_CONTROL_ENCODER_COUNT];
    struct midi_control_analog_map  analogs[MIDI_CONTROL_ANALOG_COUNT];

    /* This unit's measured travel per analog control, learned from use. It
     * rides in the configuration record because that is the only verified way
     * onto the card (docs/06), but it is not part of the user's mapping: a
     * configuration pushed from the app or loaded from a file leaves it alone,
     * and midi_control_config_equal() ignores it. */
    struct midi_control_analog_cal  analog_cal[MIDI_CONTROL_ANALOG_COUNT];

    uint8_t profile_count;    /* how many profiles are in use, 1..MIDI_CONTROL_PROFILE_COUNT */
    uint8_t profile_active;   /* which one the live mapping came from, 0..profile_count-1 */
    struct midi_control_profile profiles[MIDI_CONTROL_PROFILE_COUNT];

    /* A name per program, printable ASCII and NUL padded; all zero is no name. */
    char program_names[MIDI_CONTROL_PROGRAM_COUNT][MIDI_CONTROL_PROGRAM_NAME_LEN];
};

/* The factory mapping: the one the original controller firmware shipped with
 * and verified by MIDI capture (docs/05-config-model.md section 3), except that
 * footswitches A-D choose the presets of the bank rather than send notes 60-63
 * (docs/03 section 2). */
void midi_control_config_defaults(struct midi_control_config *cfg);

/* Serialise to exactly MIDI_CONTROL_CONFIG_BYTES bytes, header and CRC included. */
void midi_control_config_write(const struct midi_control_config *cfg, uint8_t *out);

/* Parse a record. Returns 0 on success or a MIDI_CONTROL_STATUS_* code from
 * midi_control_protocol.h (bad length, magic, version or CRC), in which case `cfg`
 * is untouched. */
int midi_control_config_read(struct midi_control_config *cfg, const uint8_t *in, uint32_t len);

/* Clamp every field into its legal range. Called after a read and before a
 * write, so an out-of-range value from an older app cannot reach the engine.
 * It is also what keeps profiles[profile_active] equal to the live mapping:
 * the live mapping is copied over it, every time. */
void midi_control_config_sanitise(struct midi_control_config *cfg);

/* Make profile `n` the live mapping, keeping whatever has been done to the one
 * being left: edits land in the live blocks, so without that an unsaved edit
 * would be lost by switching away. This is what choosing a profile means.
 * Does nothing when `n` is not one of the profiles in use, so a caller may
 * pass anything. Returns 1 if the selection happened. */
int midi_control_config_select_profile(struct midi_control_config *cfg, unsigned n);

/* The same, without keeping the profile being left: for a record assembled
 * from somewhere else - read out of a file - where the profiles are the truth
 * and the live blocks hold nothing worth keeping yet. */
int midi_control_config_load_profile(struct midi_control_config *cfg, unsigned n);

/* The name of profile `n`, NUL terminated, or "" past the end. Points into
 * `cfg`, which sanitise leaves NUL terminated. */
const char *midi_control_config_profile_name(const struct midi_control_config *cfg, unsigned n);

/* The name of program `program` (0..127), NUL terminated: "" when it has none
 * or is out of range. Points into `cfg`. */
const char *midi_control_config_program_name(const struct midi_control_config *cfg, unsigned program);

/* Name program `program`, cut to MIDI_CONTROL_PROGRAM_NAME_LEN - 1 bytes; NULL
 * or "" clears the name. The rest of the field is zeroed, so two equal names
 * are equal records. Characters outside printable ASCII are left for
 * midi_control_config_sanitise(), which makes them '?'. Returns 0 for a
 * program out of range, which changes nothing. */
int midi_control_config_set_program_name(struct midi_control_config *cfg, unsigned program, const char *name);

/* Whether a Bank up or Bank down in the live mapping moves the Program Change
 * buttons (MIDI_CONTROL_BUTTON_MOVES_PROGRAMS). Returns 1 if one does. */
int midi_control_config_moves_programs(const struct midi_control_config *cfg);

/* Exact comparison of the parts that matter: everything but the sequence
 * number and the learned analog calibration, which describes the unit's
 * hardware rather than what the user asked for. Returns 1 when equal. */
int midi_control_config_equal(const struct midi_control_config *a, const struct midi_control_config *b);

/* Names for the enumerations, for the app. Never NULL. */
const char *midi_control_button_action_name(unsigned action);
const char *midi_control_encoder_mode_name(unsigned mode);
const char *midi_control_analog_mode_name(unsigned mode);

#ifdef __cplusplus
}
#endif
#endif
