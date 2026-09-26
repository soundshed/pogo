/* The configuration protocol: MIDI System Exclusive, both directions.
 *
 * Every message is
 *
 *     F0 7D 53 47 <command> <payload...> F7
 *
 * 0x7D is the manufacturer id reserved for non-commercial use; 'S' 'G'
 * identifies this device family. Payload bytes are 7-bit; binary blobs are
 * carried 8-to-7 packed (midi_control_pack7 / midi_control_unpack7). Full detail in
 * docs/04-config-protocol.md.
 *
 * Host -> device                       Device -> host
 *   0x01 IDENTIFY                        0x41 IDENTITY  [see midi_control_identity]
 *   0x02 GET_CONFIG                      0x42 CONFIG    [len_hi len_lo packed...]
 *   0x03 SET_CONFIG [len_hi len_lo packed...]
 *                                        0x43 RESULT    [command status]
 *   0x04 SAVE                            0x43 RESULT
 *   0x05 LOAD_DEFAULTS                   0x43 RESULT
 *   0x06 GET_STATUS                      0x46 STATUS    [see midi_control_status_reply]
 *   0x07 SET_TEXT [line text...]         0x43 RESULT
 *   0x08 RELOAD (from storage)           0x43 RESULT
 *   0x09 GET_STATE                       0x49 STATE     [see midi_control_state_reply]
 *   0x0A EVENT [kind id value]           0x43 RESULT
 *   0x0B CALIBRATE [analog start]        0x43 RESULT
 *
 * The device answers every command it recognises with exactly one reply, and
 * never sends anything unsolicited over SysEx, so a host can pair replies to
 * requests by order. An EVENT also sends whatever MIDI the control sends,
 * before its reply.
 *
 * Nothing here selects a profile. A player picks one with Mode held and then
 * A-D, so an EVENT that holds Mode picks one the same way, and the app that
 * wants to switch without the panel sends the configuration with another
 * profile active (docs/05 section 7).
 */
#ifndef MIDI_CONTROL_PROTOCOL_H
#define MIDI_CONTROL_PROTOCOL_H

#include <stdint.h>
#include "midi_control_controls.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_CONTROL_SYSEX_MFR  0x7Du
#define MIDI_CONTROL_SYSEX_ID0  0x53u   /* 'S' */
#define MIDI_CONTROL_SYSEX_ID1  0x47u   /* 'G' */

/* 2 added GET_STATE and EVENT; 3 added the display's state to STATE; 4 added
 * the active profile and the profile picker to the end of STATE. No command
 * selects a profile: the app sends the configuration with another profile
 * active, or holds the drawn Mode switch with EVENT as a player holds the real
 * one. 5 added CALIBRATE, and the calibration and the pedal's raw reading to
 * the end of STATE. */
#define MIDI_CONTROL_PROTOCOL_VERSION 5u

enum midi_control_command {
    MIDI_CONTROL_CMD_IDENTIFY      = 0x01,
    MIDI_CONTROL_CMD_GET_CONFIG    = 0x02,
    MIDI_CONTROL_CMD_SET_CONFIG    = 0x03,
    MIDI_CONTROL_CMD_SAVE          = 0x04,
    MIDI_CONTROL_CMD_LOAD_DEFAULTS = 0x05,
    MIDI_CONTROL_CMD_GET_STATUS    = 0x06,
    MIDI_CONTROL_CMD_SET_TEXT      = 0x07,
    MIDI_CONTROL_CMD_RELOAD        = 0x08,
    MIDI_CONTROL_CMD_GET_STATE     = 0x09,
    MIDI_CONTROL_CMD_EVENT         = 0x0A,
    MIDI_CONTROL_CMD_CALIBRATE     = 0x0B,

    MIDI_CONTROL_RPL_IDENTITY      = 0x41,
    MIDI_CONTROL_RPL_CONFIG        = 0x42,
    MIDI_CONTROL_RPL_RESULT        = 0x43,
    MIDI_CONTROL_RPL_STATUS        = 0x46,
    MIDI_CONTROL_RPL_STATE         = 0x49
};

/* What an EVENT does: a control used as though on the panel. */
enum midi_control_event_kind {
    MIDI_CONTROL_EVENT_BUTTON = 0,   /* id enum midi_control_button;  value 1 pressed, 0 released */
    MIDI_CONTROL_EVENT_ENCODER,      /* id enum midi_control_encoder; value signed detents, -64..63 */
    MIDI_CONTROL_EVENT_ANALOG        /* id enum midi_control_analog;  value the position, 0..127 */
};

enum midi_control_status {
    MIDI_CONTROL_STATUS_OK = 0,
    MIDI_CONTROL_STATUS_BAD_LENGTH,
    MIDI_CONTROL_STATUS_BAD_MAGIC,
    MIDI_CONTROL_STATUS_BAD_VERSION,
    MIDI_CONTROL_STATUS_BAD_CRC,
    MIDI_CONTROL_STATUS_NO_STORAGE,       /* this device has nowhere to persist to */
    MIDI_CONTROL_STATUS_STORAGE_LOCKED,   /* the eMMC holds the factory image; refusing to write */
    MIDI_CONTROL_STATUS_WRITE_FAILED,
    MIDI_CONTROL_STATUS_VERIFY_FAILED,    /* written, but the read-back differed */
    MIDI_CONTROL_STATUS_UNKNOWN_COMMAND,
    MIDI_CONTROL_STATUS_INVALID_VALUE,
    MIDI_CONTROL_STATUS_COUNT
};

/* What the device's storage can do. Reported in IDENTITY and STATUS. */
enum midi_control_storage_state {
    MIDI_CONTROL_STORAGE_NONE = 0,   /* no backend, or it failed to initialise */
    MIDI_CONTROL_STORAGE_LOCKED,     /* backend present but holds the original system image */
    MIDI_CONTROL_STORAGE_READY,      /* our image is installed; the config slots may be written */
    MIDI_CONTROL_STORAGE_UNKNOWN     /* backend present, contents not recognised; treated as locked */
};

enum midi_control_platform {
    MIDI_CONTROL_PLATFORM_HARDWARE = 0,
    MIDI_CONTROL_PLATFORM_EMULATOR = 1
};

/* The IDENTITY reply, decoded. */
struct midi_control_identity {
    uint8_t protocol_version;
    uint8_t fw_major, fw_minor, fw_patch;
    uint8_t platform;         /* enum midi_control_platform */
    uint8_t storage_state;    /* enum midi_control_storage_state */
    uint8_t dirty;            /* live config differs from what was last loaded or saved */
    uint8_t config_version;
    uint16_t config_bytes;
    char name[17];            /* the configured device name, NUL terminated */
};

/* The STATUS reply, decoded. */
struct midi_control_status_reply {
    uint8_t storage_state;
    uint8_t dirty;
    uint8_t program;          /* 0..127 */
    uint32_t sequence;        /* of the config currently live */
};

/* The STATE reply, decoded: what the controls and the screen are doing now.
 * With the same configuration, a device given this (midi_control_device_mirror)
 * draws the screen and lights the LEDs the reporting device does. The two
 * counters run modulo 128, so a host that asks less often than a switch is
 * tapped or a knob clicks still sees that it happened. */
struct midi_control_state_reply {
    uint8_t storage_state;
    uint8_t dirty;
    uint8_t program;                                /* 0..127 */
    uint8_t held[MIDI_CONTROL_BUTTON_COUNT];        /* 0 or 1 */
    uint8_t toggled[MIDI_CONTROL_BUTTON_COUNT];     /* 0 or 1 */
    uint8_t presses[MIDI_CONTROL_BUTTON_COUNT];     /* press edges, modulo 128 */
    uint8_t enc_value[MIDI_CONTROL_ENCODER_COUNT];  /* the absolute mode's 0..127 */
    uint8_t enc_turns[MIDI_CONTROL_ENCODER_COUNT];  /* signed detents, modulo 128 */
    uint8_t analog[MIDI_CONTROL_ANALOG_COUNT];      /* the last position, 0..127 */
    char text[2][33];                               /* the SET_TEXT lines, NUL terminated */
    uint8_t display_off;                            /* 1: dark after the display timeout; 0 from protocol 2 */
    uint8_t profile_active;                         /* the profile the live mapping came from; 0 before protocol 4 */
    uint8_t profile_picking;                        /* 1: the profile picker is open on the screen */
    uint8_t profile_page;                           /* the page of four it is showing */

    /* Protocol 5; all zero from an older device. Raw counts are clamped to
     * MIDI_CONTROL_STATE_RAW_MAX on the wire. */
    uint8_t cal_stage;                              /* enum midi_control_cal_stage */
    uint8_t cal_analog;                             /* the control being calibrated */
    uint8_t cal_progress;                           /* 0..127 through the hold */
    uint16_t cal_raw;                               /* its latest raw count */
    uint16_t cal_first, cal_second;                 /* the ends taken so far; 0 before */
    uint8_t pedal_flags;                            /* MIDI_CONTROL_PEDAL_* */
    uint16_t pedal_raw;                             /* the pedal's latest raw count */
    uint16_t pedal_lo, pedal_hi;                    /* the window it is scaled from now */
};

/* What STATE says about the pedal's reading (protocol 5). */
#define MIDI_CONTROL_PEDAL_RAW        0x01u   /* the device reads its raw counts, so it can be calibrated */
#define MIDI_CONTROL_PEDAL_CALIBRATED 0x02u   /* the live record holds this unit's travel */
#define MIDI_CONTROL_PEDAL_SEEDED     0x04u   /* its window is still the platform's seed */
#define MIDI_CONTROL_PEDAL_UNSAVED    0x08u   /* a travel taken by CALIBRATE is not stored yet */

/* A raw count in STATE is two 7-bit bytes. */
#define MIDI_CONTROL_STATE_RAW_MAX    0x3FFFu

/* Largest SysEx either side ever builds: the CONFIG message. */
#define MIDI_CONTROL_SYSEX_HEADER  5u   /* F0 7D 53 47 cmd */
#define MIDI_CONTROL_PACKED_BYTES(n)  (((n) + 6u) / 7u * 8u)
/* The longest record a CONFIG message will carry. This is MIDI_CONTROL_CONFIG_BYTES
 * written out, so that this header stays independent of the configuration's;
 * midi_control_config.c fails to compile if the two ever disagree. Two buffers
 * this size sit in every device, in AHB SRAM on the unit, so raising it is a
 * memory decision as well as a format one (docs/05 section 11). */
#define MIDI_CONTROL_CONFIG_MAX_BYTES 4400u
#define MIDI_CONTROL_SYSEX_MAX     (MIDI_CONTROL_SYSEX_HEADER + 2u                                     + MIDI_CONTROL_PACKED_BYTES(MIDI_CONTROL_CONFIG_MAX_BYTES) + 1u)

/* 8-to-7 packing: each group of up to 7 bytes becomes one byte holding the
 * seven high bits followed by the seven low-7 bytes. Returns bytes written.
 * `out` needs MIDI_CONTROL_PACKED_BYTES(len). */
uint32_t midi_control_pack7(const uint8_t *in, uint32_t len, uint8_t *out);

/* The reverse. `out` needs at least `max_out` bytes; returns bytes written, or
 * 0 if the packed length is malformed or the output would overflow. */
uint32_t midi_control_unpack7(const uint8_t *in, uint32_t len, uint8_t *out, uint32_t max_out);

/* ---- building messages ---------------------------------------------------- */
/* Each writes a complete SysEx (F0..F7) into `out`, which must hold
 * MIDI_CONTROL_SYSEX_MAX bytes, and returns its length. */

uint32_t midi_control_build_simple(uint8_t *out, uint8_t command);                       /* no payload */
uint32_t midi_control_build_config(uint8_t *out, uint8_t command,
                                   const uint8_t *record, uint32_t record_len);          /* GET reply or SET request */
uint32_t midi_control_build_result(uint8_t *out, uint8_t for_command, uint8_t status);
uint32_t midi_control_build_identity(uint8_t *out, const struct midi_control_identity *id);
uint32_t midi_control_build_status(uint8_t *out, const struct midi_control_status_reply *st);
uint32_t midi_control_build_text(uint8_t *out, uint8_t line, const char *text);          /* SET_TEXT */
uint32_t midi_control_build_state(uint8_t *out, const struct midi_control_state_reply *st);
/* EVENT. `value` is clamped to what the kind can carry. */
uint32_t midi_control_build_event(uint8_t *out, uint8_t kind, uint8_t id, int value);
/* CALIBRATE: start (1) or cancel (0) calibrating analog control `analog`. */
uint32_t midi_control_build_calibrate(uint8_t *out, uint8_t analog, int start);

/* ---- parsing messages ----------------------------------------------------- */

struct midi_control_message {
    uint8_t command;
    const uint8_t *payload;   /* points into the caller's buffer */
    uint32_t payload_len;
};

/* Recognise a SysEx as one of ours. `sysex` includes F0 and F7. Returns 1 and
 * fills `msg` if it is, 0 otherwise (any other SysEx, which is not an error). */
int midi_control_parse_message(const uint8_t *sysex, uint32_t len, struct midi_control_message *msg);

/* Decode the payload of a CONFIG message into a record. Returns bytes written
 * or 0 on a malformed payload. `out` needs `max_out` bytes. */
uint32_t midi_control_parse_config_payload(const struct midi_control_message *msg, uint8_t *out, uint32_t max_out);

int midi_control_parse_identity(const struct midi_control_message *msg, struct midi_control_identity *id);
int midi_control_parse_status(const struct midi_control_message *msg, struct midi_control_status_reply *st);
int midi_control_parse_result(const struct midi_control_message *msg, uint8_t *for_command, uint8_t *status);
int midi_control_parse_state(const struct midi_control_message *msg, struct midi_control_state_reply *st);
/* The fields as sent; whether the id and value make sense is the device's
 * question. An encoder's value comes back signed. */
int midi_control_parse_event(const struct midi_control_message *msg, uint8_t *kind, uint8_t *id, int *value);
int midi_control_parse_calibrate(const struct midi_control_message *msg, uint8_t *analog, int *start);

const char *midi_control_status_name(unsigned status);
const char *midi_control_storage_state_name(unsigned state);

#ifdef __cplusplus
}
#endif
#endif
