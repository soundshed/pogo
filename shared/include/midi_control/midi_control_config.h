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
#include "midi_control_controls.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_CONTROL_CONFIG_MAGIC    0x47444F50u    /* "PODG" as little-endian bytes */
#define MIDI_CONTROL_CONFIG_VERSION  1u
#define MIDI_CONTROL_CONFIG_BYTES    280u
#define MIDI_CONTROL_CONFIG_NAME_LEN 16u
#define MIDI_CONTROL_DISPLAY_TIMEOUT_MAX 3600u   /* seconds: an hour */

/* What a button does. The value fields mean different things per action; the
 * names below say which. */
enum midi_control_button_action {
    MIDI_CONTROL_ACT_NONE = 0,
    MIDI_CONTROL_ACT_NOTE_MOMENTARY,  /* Note On (number, value_on) at press, Note Off at release */
    MIDI_CONTROL_ACT_NOTE_TOGGLE,     /* alternate Note On / Note Off on each press */
    MIDI_CONTROL_ACT_CC_MOMENTARY,    /* CC number = value_on at press, value_off at release */
    MIDI_CONTROL_ACT_CC_TOGGLE,       /* alternate CC number = value_on / value_off per press */
    MIDI_CONTROL_ACT_PROGRAM,         /* Program Change to `number` */
    MIDI_CONTROL_ACT_PROGRAM_UP,      /* program += number (1 if 0), wrapping in 0..127 */
    MIDI_CONTROL_ACT_PROGRAM_DOWN,    /* program -= number (1 if 0) */
    MIDI_CONTROL_ACT_BANK_UP,         /* program += global bank_size */
    MIDI_CONTROL_ACT_BANK_DOWN,       /* program -= global bank_size */
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

struct midi_control_button_map {
    uint8_t action;      /* enum midi_control_button_action */
    uint8_t channel;     /* 0 = global, 1..16 */
    uint8_t number;      /* note / CC number / program / step / preset index */
    uint8_t value_on;    /* velocity, or CC value when pressed / on */
    uint8_t value_off;   /* CC value when released / off */
    uint8_t led_on;      /* MIDI_CONTROL_LED_* while active (held, or toggled on) */
    uint8_t led_off;     /* MIDI_CONTROL_LED_* otherwise */
    uint8_t reserved;
};

struct midi_control_encoder_map {
    uint8_t mode;        /* enum midi_control_encoder_mode */
    uint8_t channel;
    uint8_t number;      /* CC number */
    uint8_t step;        /* units per detent, 1..64 (0 reads as 1) */
    uint8_t value;       /* initial value for the absolute mode */
    uint8_t reserved[3];
};

struct midi_control_analog_map {
    uint8_t mode;        /* enum midi_control_analog_mode */
    uint8_t channel;
    uint8_t number;      /* CC number */
    uint8_t invert;      /* 1: heel / anticlockwise reads 127 */
    uint8_t out_min;     /* 0..127 */
    uint8_t out_max;     /* 0..127 */
    uint8_t reserved[2];
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

struct midi_control_config {
    uint32_t sequence;     /* incremented by every save; picks the newer of two slots */
    struct midi_control_global global;
    struct midi_control_button_map  buttons[MIDI_CONTROL_BUTTON_COUNT];
    struct midi_control_encoder_map encoders[MIDI_CONTROL_ENCODER_COUNT];
    struct midi_control_analog_map  analogs[MIDI_CONTROL_ANALOG_COUNT];
};

/* The factory mapping - the one the original controller firmware shipped with
 * and verified by MIDI capture (docs/05-config-model.md section 3). */
void midi_control_config_defaults(struct midi_control_config *cfg);

/* Serialise to exactly MIDI_CONTROL_CONFIG_BYTES bytes, header and CRC included. */
void midi_control_config_write(const struct midi_control_config *cfg, uint8_t *out);

/* Parse a record. Returns 0 on success or a MIDI_CONTROL_STATUS_* code from
 * midi_control_protocol.h (bad length, magic, version or CRC), in which case `cfg`
 * is untouched. */
int midi_control_config_read(struct midi_control_config *cfg, const uint8_t *in, uint32_t len);

/* Clamp every field into its legal range. Called after a read and before a
 * write, so an out-of-range value from an older app cannot reach the engine. */
void midi_control_config_sanitise(struct midi_control_config *cfg);

/* Exact comparison of the parts that matter (everything but the sequence
 * number). Returns 1 when equal. */
int midi_control_config_equal(const struct midi_control_config *a, const struct midi_control_config *b);

/* Names for the enumerations, for the app. Never NULL. */
const char *midi_control_button_action_name(unsigned action);
const char *midi_control_encoder_mode_name(unsigned mode);
const char *midi_control_analog_mode_name(unsigned mode);

#ifdef __cplusplus
}
#endif
#endif
