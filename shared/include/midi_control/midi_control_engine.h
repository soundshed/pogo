/* The controller engine: control events in, MIDI out, LED and display state.
 *
 * Pure logic. It knows nothing about pins, ADCs, USB or screens, which is what
 * lets the firmware and the emulator run the same code. The platform feeds it
 * debounced button edges, encoder detents and analog positions already scaled
 * to 0..127, and takes MIDI bytes back through one callback.
 */
#ifndef MIDI_CONTROL_ENGINE_H
#define MIDI_CONTROL_ENGINE_H

#include <stdint.h>
#include "midi_control_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*midi_control_midi_out_fn)(void *ctx, const uint8_t *bytes, uint32_t len);

struct midi_control_engine {
    const struct midi_control_config *cfg;
    midi_control_midi_out_fn out;
    void *out_ctx;

    uint8_t program;                                 /* 0..127 */
    /* Added to the program of every Program Change button, 0..127: a bank for
     * each Bank Up, less one for each Bank Down, counting only the presses of
     * those that move the Program Change buttons (MIDI_CONTROL_BUTTON_MOVES_PROGRAMS). */
    uint8_t program_offset;
    /* A bank Bank Up or Down has chosen and nothing has been loaded from yet,
     * with the global bank_wait on: preset-in-bank switches choose from it,
     * and the program stays where it is until one does. */
    uint8_t bank_waiting;                            /* 1: bank_pending is chosen */
    uint8_t bank_pending;                            /* that bank, 0-based */
    uint8_t held[MIDI_CONTROL_BUTTON_COUNT];         /* physically down */
    uint8_t toggled[MIDI_CONTROL_BUTTON_COUNT];      /* toggle actions: on / off */
    uint8_t hold_toggled[MIDI_CONTROL_BUTTON_COUNT]; /* the same, for a hold action's toggle */
    uint32_t hold_active;                            /* a bit per switch whose hold action is engaged */
    uint8_t taps;                                    /* Tap tempo presses, modulo 256 */
    uint8_t presses[MIDI_CONTROL_BUTTON_COUNT];      /* press edges, modulo 128, whatever the action */
    uint8_t enc_value[MIDI_CONTROL_ENCODER_COUNT];   /* absolute-mode accumulators */
    uint8_t enc_turns[MIDI_CONTROL_ENCODER_COUNT];   /* signed detents, modulo 128, whatever the mode */
    int16_t analog_sent[MIDI_CONTROL_ANALOG_COUNT];  /* last transmitted value, -1 = none */
    uint8_t analog_dest[MIDI_CONTROL_ANALOG_COUNT][3]; /* ...and its mode, channel and number */
    uint8_t analog_in[MIDI_CONTROL_ANALOG_COUNT];    /* last input 0..127, for redisplay */
    uint8_t analog_seen[MIDI_CONTROL_ANALOG_COUNT];  /* 1: analog_in is a real position */
    uint8_t led[MIDI_CONTROL_COLUMNS];               /* MIDI_CONTROL_LED_* per column */
};

void midi_control_engine_init(struct midi_control_engine *e, const struct midi_control_config *cfg,
                              midi_control_midi_out_fn out, void *out_ctx);

/* The configuration was replaced (same pointer, new contents, or a new
 * pointer). Re-derives the LEDs and clamps state; toggles, the program and
 * the Program Change buttons' offset are kept so a live edit does not disturb
 * a performance - except that a mapping with no Bank Up or Down that moves the
 * Program Change buttons has them where they are set, and the offset is 0.
 * An analog control whose output the new mapping changes - another CC, another
 * channel, other padding - sends its position under it now, rather than
 * leaving the host with the old one until the control moves. */
void midi_control_engine_config_changed(struct midi_control_engine *e, const struct midi_control_config *cfg);

/* The buttons are about to be mapped as `next`, their holds as `next_holds`,
 * with `next_channel` the global channel. The press and the hold of a switch
 * are taken separately: each whose message changes is finished as below. Call it before the configuration changes, while e->cfg is still
 * the old one. Each button whose message changes (its action, channel or
 * number) is finished under the mapping it started in: one held is released,
 * and a Note toggle that is on sends its Note Off - otherwise that note sounds
 * until the host is reset. Their toggles start again from off, since the state
 * belonged to a message the button no longer sends. Returns a bit per button
 * released here, whose physical release the caller has to swallow. */
uint32_t midi_control_engine_remap(struct midi_control_engine *e, const struct midi_control_button_map *next,
                                   const struct midi_control_hold_map *next_holds, uint8_t next_channel);

/* The host has just started listening (USB came up): send every analog
 * control's position as it is now, which anything sent before went nowhere. */
void midi_control_engine_resend_analogs(struct midi_control_engine *e);

/* Debounced edges: the switch's press action. A switch with a hold action is
 * timed by the device, which calls this for a short press (both edges, at the
 * release) and midi_control_engine_hold() for a long one. */
void midi_control_engine_button(struct midi_control_engine *e, enum midi_control_button b, int pressed);

/* Switch `b`'s hold action (docs/05 section 3): engaged (pressed) once the
 * switch has been held for the hold time, released with the switch. It acts
 * as its action does on a press and a release, on the switch's channel, with
 * a toggle state of its own. Nothing when the switch has no hold action. */
void midi_control_engine_hold(struct midi_control_engine *e, enum midi_control_button b, int pressed);

/* Signed detents: +1 per clockwise click. */
void midi_control_engine_encoder(struct midi_control_engine *e, enum midi_control_encoder enc, int detents);

/* Position 0..127, already ranged by the platform. The engine de-duplicates,
 * so calling this every scan is fine. */
void midi_control_engine_analog(struct midi_control_engine *e, enum midi_control_analog a, uint8_t value);

/* The value a position 0..127 sends under `m`: the padding, then `invert`,
 * then out_min..out_max (docs/05 section 5). Pitch bend's is the 7-bit value
 * before it is spread over fourteen bits. What the engine sends, and what a
 * display showing "the value" has to show - the position is not it. */
uint8_t midi_control_engine_analog_value(const struct midi_control_analog_map *m, uint8_t position);

/* A complete channel message from the host (1..3 bytes). With the global
 * follow_program_change on, a Program Change on a channel the mapping sends
 * programs on moves the program, and a CC or note that a toggle or an absolute
 * encoder sends sets its state, so the switches show what the host has after
 * a preset change of its own. Other channels are someone else's. */
void midi_control_engine_midi_in(struct midi_control_engine *e, const uint8_t *msg, uint32_t len);

/* Set the program directly (startup, or a host command) without sending. A
 * bank waiting is given up, as it is for any program change. */
void midi_control_engine_set_program(struct midi_control_engine *e, uint8_t program);

/* Have bank `bank` (0-based) waiting, or none, without sending: for a mirror
 * taking on another device's state. A bank past the last is none. */
void midi_control_engine_set_bank_waiting(struct midi_control_engine *e, int waiting, uint8_t bank);

/* The program a Program Change button sends now: its `number` moved by the
 * banks Bank Up and Down have moved it (docs/05 section 3). */
uint8_t midi_control_engine_button_program(const struct midi_control_engine *e,
                                           const struct midi_control_button_map *m);

/* Banks (docs/05 section 3). There are ceil(128 / bank_size) of them, the last
 * short when the size does not divide 128, and they wrap as a whole, so a
 * bank always starts on a multiple of the size. */
unsigned midi_control_engine_bank_count(const struct midi_control_engine *e);

/* The bank preset-in-bank switches choose from: the one waiting, or the
 * program's. 0-based. */
unsigned midi_control_engine_bank(const struct midi_control_engine *e);

/* The bank a Bank Up (dir 1) or Bank Down (-1) press chooses next. */
unsigned midi_control_engine_bank_target(const struct midi_control_engine *e, int dir);

/* The program a Bank Up or Down press sends when the bank does not wait: the
 * same place in the next bank, or the last program of a short last bank. */
uint8_t midi_control_engine_bank_program(const struct midi_control_engine *e, int dir);

/* The program a preset-in-bank switch sends now, or -1 when its place is past
 * the end of a short last bank, where a press sends nothing. */
int midi_control_engine_preset_program(const struct midi_control_engine *e,
                                       const struct midi_control_button_map *m);

/* Whether a button counts as active - held, toggled on, or the selected preset,
 * by its action (docs/05-config-model.md section 3). Its LED shows led_on when
 * it is, and the screen lights its strip. */
int midi_control_engine_button_active(const struct midi_control_engine *e, enum midi_control_button b);

#ifdef __cplusplus
}
#endif
#endif
