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
#include "midi_control_config.h"
#include "midi_control_engine.h"
#include "midi_control_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_CONTROL_STORAGE_SLOTS   2u
#define MIDI_CONTROL_TEXT_LINES      2u
#define MIDI_CONTROL_TEXT_LEN        32u

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

    /* The display timeout (global display_timeout, docs/05 section 2). */
    uint32_t idle_ms;                   /* since a control was last used; saturates */
    uint8_t display_off;                /* 1 once idle_ms has reached the timeout */

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

/* Time passing on the platform's clock: `elapsed_ms` since the last call. Once
 * no control has been used for the global display_timeout, the display goes
 * dark. A control used - on the panel or by EVENT, whatever it is mapped to -
 * lights it and restarts the count, and still does what it does; so does a new
 * configuration (SET_CONFIG, LOAD_DEFAULTS, RELOAD). The host's MIDI, SET_TEXT
 * and GET_STATE do not. A timeout of 0 never darkens it. */
void midi_control_device_tick(struct midi_control_device *d, uint32_t elapsed_ms);

/* 0 while the display timeout has the display dark. */
int midi_control_device_display_on(const struct midi_control_device *d);

/* Replace the live configuration (sanitised) without persisting. */
void midi_control_device_set_config(struct midi_control_device *d, const struct midi_control_config *cfg);

/* Persist the live configuration. Returns a MIDI_CONTROL_STATUS_* code. Writes the
 * slot that does not hold the current newest record, with the sequence
 * incremented, so a failed write leaves the previous save intact. */
int midi_control_device_save(struct midi_control_device *d);

/* Re-read storage, discarding live changes. Returns a MIDI_CONTROL_STATUS_* code. */
int midi_control_device_reload(struct midi_control_device *d);

int midi_control_device_storage_state(const struct midi_control_device *d);

/* The LEDs, one MIDI_CONTROL_LED_* byte per column. */
const uint8_t *midi_control_device_leds(const struct midi_control_device *d);

/* What a STATE reply says about this device. */
void midi_control_device_state(const struct midi_control_device *d, struct midi_control_state_reply *st);

/* Take on the state another device reported, sending nothing: the program,
 * the switches, the knobs, the text, the unsaved flag and whether the display
 * is dark. The storage state stays this device's own platform's. A host draws
 * its picture of a unit from
 * a device of its own mirrored like this; with the same configuration it
 * draws the screen and lights the LEDs the unit does. */
void midi_control_device_mirror(struct midi_control_device *d, const struct midi_control_state_reply *st);

#ifdef __cplusplus
}
#endif
#endif
