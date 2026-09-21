#include <string.h>
#include "midi_control/midi_control_engine.h"

#define PROGRAM_COUNT 128u

static uint8_t channel_of(const struct midi_control_engine *e, uint8_t ch)
{
    if (ch == MIDI_CONTROL_CHANNEL_GLOBAL || ch > 16u)
        return e->cfg->global.midi_channel & 0x0Fu;
    return (uint8_t)((ch - 1u) & 0x0Fu);
}

static void send3(struct midi_control_engine *e, uint8_t status, uint8_t ch, uint8_t d1, uint8_t d2)
{
    uint8_t m[3];
    m[0] = (uint8_t)(status | ch);
    m[1] = (uint8_t)(d1 & 0x7Fu);
    m[2] = (uint8_t)(d2 & 0x7Fu);
    if (e->out)
        e->out(e->out_ctx, m, 3);
}

static void send2(struct midi_control_engine *e, uint8_t status, uint8_t ch, uint8_t d1)
{
    uint8_t m[2];
    m[0] = (uint8_t)(status | ch);
    m[1] = (uint8_t)(d1 & 0x7Fu);
    if (e->out)
        e->out(e->out_ctx, m, 2);
}

/* Bank Select MSB then LSB, then the Program Change - the order the MIDI
 * specification requires, since a bank only takes effect on the following
 * Program Change. Programs are 0..127, so both bank bytes are zero; they are
 * sent because some receivers expect the triple. */
static void send_program(struct midi_control_engine *e, uint8_t ch)
{
    if (e->cfg->global.bank_select) {
        send3(e, 0xB0, ch, 0, 0);
        send3(e, 0xB0, ch, 32, 0);
    }
    send2(e, 0xC0, ch, e->program);
}

static uint8_t bank_size(const struct midi_control_engine *e)
{
    uint8_t n = e->cfg->global.bank_size;
    return n < 1u ? 1u : n;
}

int midi_control_engine_button_active(const struct midi_control_engine *e, enum midi_control_button b)
{
    if ((unsigned)b >= MIDI_CONTROL_BUTTON_COUNT)
        return 0;

    const struct midi_control_button_map *m = &e->cfg->buttons[b];

    switch (m->action) {
    case MIDI_CONTROL_ACT_NOTE_TOGGLE:
    case MIDI_CONTROL_ACT_CC_TOGGLE:
        return e->toggled[b] != 0;
    case MIDI_CONTROL_ACT_PRESET_IN_BANK:
        return (e->program % bank_size(e)) == (m->number % bank_size(e));
    case MIDI_CONTROL_ACT_NONE:
        return 0;
    default:
        return e->held[b] != 0;
    }
}

static void update_leds(struct midi_control_engine *e)
{
    memset(e->led, 0, sizeof(e->led));
    for (unsigned b = 0; b < MIDI_CONTROL_BUTTON_COUNT; b++) {
        int col = midi_control_button_led_column((enum midi_control_button)b);
        if (col < 0)
            continue;
        const struct midi_control_button_map *m = &e->cfg->buttons[b];
        e->led[col] = midi_control_engine_button_active(e, (enum midi_control_button)b) ? (uint8_t)(m->led_on & 7u)
                                                                                  : (uint8_t)(m->led_off & 7u);
    }
}

void midi_control_engine_init(struct midi_control_engine *e, const struct midi_control_config *cfg,
                              midi_control_midi_out_fn out, void *out_ctx)
{
    memset(e, 0, sizeof(*e));
    e->cfg = cfg;
    e->out = out;
    e->out_ctx = out_ctx;
    e->program = cfg->global.startup_program & 0x7Fu;
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++)
        e->analog_sent[i] = -1;
    for (unsigned i = 0; i < MIDI_CONTROL_ENCODER_COUNT; i++)
        e->enc_value[i] = cfg->encoders[i].value & 0x7Fu;
    update_leds(e);
}

void midi_control_engine_config_changed(struct midi_control_engine *e, const struct midi_control_config *cfg)
{
    e->cfg = cfg;
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++)
        e->analog_sent[i] = -1;          /* the mapping may have changed; resend on next move */
    update_leds(e);
}

void midi_control_engine_set_program(struct midi_control_engine *e, uint8_t program)
{
    e->program = program & 0x7Fu;
    update_leds(e);
}

static void step_program(struct midi_control_engine *e, int delta, uint8_t ch)
{
    int p = (int)e->program + delta;
    p %= (int)PROGRAM_COUNT;
    if (p < 0)
        p += (int)PROGRAM_COUNT;
    e->program = (uint8_t)p;
    send_program(e, ch);
}

void midi_control_engine_button(struct midi_control_engine *e, enum midi_control_button b, int pressed)
{
    if ((unsigned)b >= MIDI_CONTROL_BUTTON_COUNT)
        return;

    const struct midi_control_button_map *m = &e->cfg->buttons[b];
    uint8_t ch = channel_of(e, m->channel);
    uint8_t step;

    e->held[b] = pressed ? 1u : 0u;
    if (pressed)
        e->presses[b] = (uint8_t)((e->presses[b] + 1u) & 0x7Fu);

    switch (m->action) {
    case MIDI_CONTROL_ACT_NOTE_MOMENTARY:
        if (pressed)
            send3(e, 0x90, ch, m->number, m->value_on ? m->value_on : 1u);
        else
            send3(e, 0x80, ch, m->number, m->value_off);
        break;

    case MIDI_CONTROL_ACT_NOTE_TOGGLE:
        if (pressed) {
            e->toggled[b] ^= 1u;
            if (e->toggled[b])
                send3(e, 0x90, ch, m->number, m->value_on ? m->value_on : 1u);
            else
                send3(e, 0x80, ch, m->number, m->value_off);
        }
        break;

    case MIDI_CONTROL_ACT_CC_MOMENTARY:
        send3(e, 0xB0, ch, m->number, pressed ? m->value_on : m->value_off);
        break;

    case MIDI_CONTROL_ACT_CC_TOGGLE:
        if (pressed) {
            e->toggled[b] ^= 1u;
            send3(e, 0xB0, ch, m->number, e->toggled[b] ? m->value_on : m->value_off);
        }
        break;

    case MIDI_CONTROL_ACT_PROGRAM:
        if (pressed) {
            e->program = m->number & 0x7Fu;
            send_program(e, ch);
        }
        break;

    case MIDI_CONTROL_ACT_PROGRAM_UP:
    case MIDI_CONTROL_ACT_PROGRAM_DOWN:
        if (pressed) {
            step = m->number ? m->number : 1u;
            step_program(e, m->action == MIDI_CONTROL_ACT_PROGRAM_UP ? (int)step : -(int)step, ch);
        }
        break;

    case MIDI_CONTROL_ACT_BANK_UP:
        if (pressed)
            step_program(e, (int)bank_size(e), ch);
        break;

    case MIDI_CONTROL_ACT_BANK_DOWN:
        if (pressed)
            step_program(e, -(int)bank_size(e), ch);
        break;

    case MIDI_CONTROL_ACT_PRESET_IN_BANK:
        if (pressed) {
            uint8_t n = bank_size(e);
            uint8_t base = (uint8_t)((e->program / n) * n);
            uint8_t p = (uint8_t)(base + (m->number % n));
            e->program = (uint8_t)(p % PROGRAM_COUNT);
            send_program(e, ch);
        }
        break;

    default:
        break;
    }

    update_leds(e);
}

void midi_control_engine_encoder(struct midi_control_engine *e, enum midi_control_encoder enc, int detents)
{
    if ((unsigned)enc >= MIDI_CONTROL_ENCODER_COUNT || detents == 0)
        return;

    const struct midi_control_encoder_map *m = &e->cfg->encoders[enc];
    uint8_t ch = channel_of(e, m->channel);
    int step = m->step ? m->step : 1;
    int dir = detents > 0 ? 1 : -1;
    int count = detents > 0 ? detents : -detents;

    /* A relative CC carries at most 63 either way: seven-bit two's complement
     * has no +64, and 64 + 64 is past a data byte. */
    int rel = step > 63 ? 63 : step;

    /* Counted before the mode is looked at: an unmapped knob still turned. */
    e->enc_turns[enc] = (uint8_t)(((int)e->enc_turns[enc] + detents) & 0x7F);

    for (int i = 0; i < count; i++) {
        switch (m->mode) {
        case MIDI_CONTROL_ENCM_CC_RELATIVE_TWOS:
            send3(e, 0xB0, ch, m->number, (uint8_t)(dir > 0 ? rel : 128 - rel));
            break;

        case MIDI_CONTROL_ENCM_CC_RELATIVE_OFFSET:
            send3(e, 0xB0, ch, m->number, (uint8_t)(64 + dir * rel));
            break;

        case MIDI_CONTROL_ENCM_CC_ABSOLUTE: {
            int v = (int)e->enc_value[enc] + dir * step;
            if (v < 0)
                v = 0;
            if (v > 127)
                v = 127;
            if ((uint8_t)v != e->enc_value[enc]) {
                e->enc_value[enc] = (uint8_t)v;
                send3(e, 0xB0, ch, m->number, (uint8_t)v);
            }
            break;
        }

        case MIDI_CONTROL_ENCM_PROGRAM:
            step_program(e, dir * step, ch);
            break;

        default:
            return;
        }
    }
    update_leds(e);
}

void midi_control_engine_analog(struct midi_control_engine *e, enum midi_control_analog a, uint8_t value)
{
    if ((unsigned)a >= MIDI_CONTROL_ANALOG_COUNT)
        return;

    const struct midi_control_analog_map *m = &e->cfg->analogs[a];
    uint8_t ch = channel_of(e, m->channel);

    value &= 0x7Fu;
    e->analog_in[a] = value;

    if (m->mode == MIDI_CONTROL_ANM_NONE)
        return;

    unsigned v = m->invert ? 127u - value : value;
    unsigned lo = m->out_min, hi = m->out_max;
    if (lo > hi) {
        unsigned t = lo;
        lo = hi;
        hi = t;
    }
    /* Scale 0..127 onto lo..hi, rounding to nearest. */
    unsigned out = lo + (v * (hi - lo) + 63u) / 127u;
    if (out > 127u)
        out = 127u;

    if ((int16_t)out == e->analog_sent[a])
        return;
    e->analog_sent[a] = (int16_t)out;

    switch (m->mode) {
    case MIDI_CONTROL_ANM_CC:
        send3(e, 0xB0, ch, m->number, (uint8_t)out);
        break;
    case MIDI_CONTROL_ANM_PITCH_BEND: {
        /* 7-bit position spread over the 14-bit range: 0 -> 0, 127 -> 16383. */
        unsigned pb = out * 129u;        /* 127 * 129 = 16383 */
        send3(e, 0xE0, ch, (uint8_t)(pb & 0x7Fu), (uint8_t)((pb >> 7) & 0x7Fu));
        break;
    }
    case MIDI_CONTROL_ANM_CHANNEL_PRESSURE:
        send2(e, 0xD0, ch, (uint8_t)out);
        break;
    default:
        break;
    }
}

void midi_control_engine_midi_in(struct midi_control_engine *e, const uint8_t *msg, uint32_t len)
{
    if (len < 2u)
        return;
    if ((msg[0] & 0xF0u) == 0xC0u && e->cfg->global.follow_program_change) {
        e->program = msg[1] & 0x7Fu;
        update_leds(e);
    }
}
