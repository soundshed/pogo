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
    uint8_t held[MIDI_CONTROL_BUTTON_COUNT];         /* physically down */
    uint8_t toggled[MIDI_CONTROL_BUTTON_COUNT];      /* toggle actions: on / off */
    uint8_t presses[MIDI_CONTROL_BUTTON_COUNT];      /* press edges, modulo 128, whatever the action */
    uint8_t enc_value[MIDI_CONTROL_ENCODER_COUNT];   /* absolute-mode accumulators */
    uint8_t enc_turns[MIDI_CONTROL_ENCODER_COUNT];   /* signed detents, modulo 128, whatever the mode */
    int16_t analog_sent[MIDI_CONTROL_ANALOG_COUNT];  /* last transmitted value, -1 = none */
    uint8_t analog_in[MIDI_CONTROL_ANALOG_COUNT];    /* last input 0..127, for redisplay */
    uint8_t led[MIDI_CONTROL_COLUMNS];               /* MIDI_CONTROL_LED_* per column */
};

void midi_control_engine_init(struct midi_control_engine *e, const struct midi_control_config *cfg,
                              midi_control_midi_out_fn out, void *out_ctx);

/* The configuration was replaced (same pointer, new contents, or a new
 * pointer). Re-derives the LEDs and clamps state; toggles, the program and
 * the Program Change buttons' offset are kept so a live edit does not disturb
 * a performance - except that a mapping with no Bank Up or Down that moves the
 * Program Change buttons has them where they are set, and the offset is 0. */
void midi_control_engine_config_changed(struct midi_control_engine *e, const struct midi_control_config *cfg);

/* Debounced edges. */
void midi_control_engine_button(struct midi_control_engine *e, enum midi_control_button b, int pressed);

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

/* A complete channel message from the host (1..3 bytes). Program Change moves
 * the program when the configuration says to follow it. */
void midi_control_engine_midi_in(struct midi_control_engine *e, const uint8_t *msg, uint32_t len);

/* Set the program directly (startup, or a host command) without sending. */
void midi_control_engine_set_program(struct midi_control_engine *e, uint8_t program);

/* The program a Program Change button sends now: its `number` moved by the
 * banks Bank Up and Down have moved it (docs/05 section 3). */
uint8_t midi_control_engine_button_program(const struct midi_control_engine *e,
                                           const struct midi_control_button_map *m);

/* Whether a button counts as active - held, toggled on, or the selected preset,
 * by its action (docs/05-config-model.md section 3). Its LED shows led_on when
 * it is, and the screen lights its strip. */
int midi_control_engine_button_active(const struct midi_control_engine *e, enum midi_control_button b);

#ifdef __cplusplus
}
#endif
#endif
