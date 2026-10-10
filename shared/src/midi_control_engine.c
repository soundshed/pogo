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

unsigned midi_control_engine_bank_count(const struct midi_control_engine *e)
{
    const unsigned n = bank_size(e);
    return (PROGRAM_COUNT + n - 1u) / n;
}

unsigned midi_control_engine_bank(const struct midi_control_engine *e)
{
    return e->bank_waiting ? e->bank_pending : e->program / bank_size(e);
}

/* Banks wrap as a whole - the bank after the last is the first - rather than
 * programs wrapping in 0..127, which with a size that does not divide 128
 * would carry Bank Up from the last bank into the middle of the first. */
static unsigned bank_step(const struct midi_control_engine *e, unsigned bank, int dir)
{
    const unsigned count = midi_control_engine_bank_count(e);
    return (bank + count + (unsigned)(dir > 0 ? 1 : count - 1u)) % count;
}

unsigned midi_control_engine_bank_target(const struct midi_control_engine *e, int dir)
{
    return bank_step(e, midi_control_engine_bank(e), dir);
}

uint8_t midi_control_engine_bank_program(const struct midi_control_engine *e, int dir)
{
    const unsigned n = bank_size(e);
    const unsigned p = bank_step(e, e->program / n, dir) * n + e->program % n;
    return (uint8_t)(p < PROGRAM_COUNT ? p : PROGRAM_COUNT - 1u);
}

int midi_control_engine_preset_program(const struct midi_control_engine *e,
                                       const struct midi_control_button_map *m)
{
    const unsigned n = bank_size(e);
    const unsigned p = midi_control_engine_bank(e) * n + m->number % n;
    return p < PROGRAM_COUNT ? (int)p : -1;
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
        return midi_control_engine_preset_program(e, m) == (int)e->program;
    case MIDI_CONTROL_ACT_PROGRAM:
        /* Lit on the program it chose, as a preset switch is, not only while
         * it is held. */
        return midi_control_engine_button_program(e, m) == e->program;
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

static void analog_send(struct midi_control_engine *e, unsigned a);

void midi_control_engine_config_changed(struct midi_control_engine *e, const struct midi_control_config *cfg)
{
    e->cfg = cfg;
    /* Nothing in this mapping could move them back, so they go back now. */
    if (!midi_control_config_moves_programs(cfg))
        e->program_offset = 0;
    /* A bank waits only while banks wait, and only for a bank there still is. */
    if (!cfg->global.bank_wait || e->bank_pending >= midi_control_engine_bank_count(e))
        e->bank_waiting = 0;
    /* The mapping may send the analog controls somewhere else, or something
     * else: the host hears where they are now under it. analog_send() sends
     * nothing when neither the value nor where it goes has changed. */
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++)
        if (e->analog_seen[i])
            analog_send(e, i);
    update_leds(e);
}

void midi_control_engine_set_program(struct midi_control_engine *e, uint8_t program)
{
    e->program = program & 0x7Fu;
    e->bank_waiting = 0;
    update_leds(e);
}

void midi_control_engine_set_bank_waiting(struct midi_control_engine *e, int waiting, uint8_t bank)
{
    e->bank_waiting = waiting && bank < midi_control_engine_bank_count(e) ? 1u : 0u;
    e->bank_pending = e->bank_waiting ? bank : 0u;
    update_leds(e);
}

/* A control's channel, 0..15, with "global" resolved against `global`. */
static uint8_t resolved_channel(uint8_t channel, uint8_t global)
{
    return (uint8_t)((channel == MIDI_CONTROL_CHANNEL_GLOBAL || channel > 16u ? global : channel - 1u) & 0x0Fu);
}

uint32_t midi_control_engine_remap(struct midi_control_engine *e, const struct midi_control_button_map *next,
                                   const struct midi_control_hold_map *next_holds, uint8_t next_channel)
{
    uint32_t released = 0;

    /* What a switch sends, for these purposes: two mappings that send the same
     * message are the same switch to the host. The press and the hold are
     * taken apart, so a new hold leaves a toggled press alone. */
    for (unsigned b = 0; b < MIDI_CONTROL_BUTTON_COUNT; b++) {
        const struct midi_control_button_map *m = &e->cfg->buttons[b];
        const struct midi_control_hold_map *h = &e->cfg->holds[b];
        const uint32_t bit = 1uL << b;
        const uint8_t was_ch = resolved_channel(m->channel, e->cfg->global.midi_channel);
        const int same_channel = was_ch == resolved_channel(next[b].channel, next_channel);
        const int press_same = same_channel && m->action == next[b].action && m->number == next[b].number;
        const int hold_same = same_channel && h->action == next_holds[b].action && h->number == next_holds[b].number;

        if (!hold_same) {
            if (e->hold_active & bit) {
                midi_control_engine_hold(e, (enum midi_control_button)b, 0);
                released |= bit;
            }
            if (e->hold_toggled[b] && h->action == MIDI_CONTROL_ACT_NOTE_TOGGLE)
                send3(e, 0x80, was_ch, h->number, h->value_off);
            e->hold_toggled[b] = 0;
        }
        if (!press_same) {
            /* Held by its press, not by a hold left engaged above. */
            if (e->held[b] && !(e->hold_active & bit)) {
                midi_control_engine_button(e, (enum midi_control_button)b, 0);
                released |= bit;
            }
            if (e->toggled[b] && m->action == MIDI_CONTROL_ACT_NOTE_TOGGLE)
                send3(e, 0x80, was_ch, m->number, m->value_off);
            e->toggled[b] = 0;
        }
    }
    update_leds(e);
    return released;
}

uint8_t midi_control_engine_button_program(const struct midi_control_engine *e,
                                           const struct midi_control_button_map *m)
{
    return (uint8_t)((m->number + e->program_offset) & 0x7Fu);
}

/* Programs wrap in 0..127, whichever way they are stepped. */
static uint8_t wrap_program(int p)
{
    p %= (int)PROGRAM_COUNT;
    if (p < 0)
        p += (int)PROGRAM_COUNT;
    return (uint8_t)p;
}

static void step_program(struct midi_control_engine *e, int delta, uint8_t ch)
{
    e->program = wrap_program((int)e->program + delta);
    send_program(e, ch);
}

/* Bank Up or Down: a bank either way, and the Program Change buttons with it
 * when the button says so. They move by whole banks and wrap the same way, so
 * a Program Change button pressed after a bank lands a bank from where it did
 * before.
 *
 * With the global bank_wait on, the bank is chosen and nothing is sent: the
 * preset-in-bank switches then choose from it, and the program changes when
 * one of them is pressed - so stepping past three banks does not load three
 * presets. Coming back to the program's own bank is the same as never having
 * left it. Without it, the program moves to the same place in the new bank and
 * is sent at once. */
static void step_bank(struct midi_control_engine *e, const struct midi_control_button_map *m, int dir, uint8_t ch)
{
    const unsigned n = bank_size(e);

    if (m->flags & MIDI_CONTROL_BUTTON_MOVES_PROGRAMS)
        e->program_offset = (uint8_t)(bank_step(e, e->program_offset / n, dir) * n);

    if (e->cfg->global.bank_wait) {
        const unsigned to = midi_control_engine_bank_target(e, dir);
        e->bank_pending = (uint8_t)to;
        e->bank_waiting = to != e->program / n;
        return;
    }
    e->program = midi_control_engine_bank_program(e, dir);
    send_program(e, ch);
}

/* One edge of a button action, from the press map or a hold's. `toggled` is
 * the state a toggle action flips: the switch's own, or its hold's. */
static void act(struct midi_control_engine *e, const struct midi_control_button_map *m, int pressed,
                uint8_t *toggled)
{
    uint8_t ch = channel_of(e, m->channel);
    uint8_t step;

    switch (m->action) {
    case MIDI_CONTROL_ACT_NOTE_MOMENTARY:
        if (pressed)
            send3(e, 0x90, ch, m->number, m->value_on ? m->value_on : 1u);
        else
            send3(e, 0x80, ch, m->number, m->value_off);
        break;

    case MIDI_CONTROL_ACT_NOTE_TOGGLE:
        if (pressed) {
            *toggled ^= 1u;
            if (*toggled)
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
            *toggled ^= 1u;
            send3(e, 0xB0, ch, m->number, *toggled ? m->value_on : m->value_off);
        }
        break;

    case MIDI_CONTROL_ACT_PROGRAM:
        if (pressed) {
            e->program = midi_control_engine_button_program(e, m);
            e->bank_waiting = 0;
            send_program(e, ch);
        }
        break;

    case MIDI_CONTROL_ACT_PROGRAM_UP:
    case MIDI_CONTROL_ACT_PROGRAM_DOWN:
        if (pressed) {
            step = m->number ? m->number : 1u;
            e->bank_waiting = 0;
            step_program(e, m->action == MIDI_CONTROL_ACT_PROGRAM_UP ? (int)step : -(int)step, ch);
        }
        break;

    case MIDI_CONTROL_ACT_BANK_UP:
    case MIDI_CONTROL_ACT_BANK_DOWN:
        if (pressed)
            step_bank(e, m, m->action == MIDI_CONTROL_ACT_BANK_UP ? 1 : -1, ch);
        break;

    case MIDI_CONTROL_ACT_PRESET_IN_BANK:
        if (pressed) {
            /* From the bank waiting, if one is. A place past the end of a
             * short last bank has no program, and sends nothing - wrapping it
             * onto program 0 would jump to the first bank from the last. */
            const int p = midi_control_engine_preset_program(e, m);
            if (p >= 0) {
                e->program = (uint8_t)p;
                e->bank_waiting = 0;
                send_program(e, ch);
            }
        }
        break;

    case MIDI_CONTROL_ACT_TAP_TEMPO:
        /* A tap is the press: nothing at the release, which a receiver
         * counting messages would take for a second tap. The device times
         * the taps (it has the clock) and flashes the tempo. */
        if (pressed) {
            send3(e, 0xB0, ch, m->number, m->value_on);
            e->taps++;
        }
        break;

    default:
        break;
    }
}

void midi_control_engine_button(struct midi_control_engine *e, enum midi_control_button b, int pressed)
{
    if ((unsigned)b >= MIDI_CONTROL_BUTTON_COUNT)
        return;

    e->held[b] = pressed ? 1u : 0u;
    if (pressed)
        e->presses[b] = (uint8_t)((e->presses[b] + 1u) & 0x7Fu);
    act(e, &e->cfg->buttons[b], pressed, &e->toggled[b]);
    update_leds(e);
}

/* A hold as a button map: its own action and values, on the switch's channel. */
static void hold_map(const struct midi_control_engine *e, unsigned b, struct midi_control_button_map *m)
{
    const struct midi_control_hold_map *h = &e->cfg->holds[b];
    memset(m, 0, sizeof(*m));
    m->action = h->action;
    m->channel = e->cfg->buttons[b].channel;
    m->number = h->number;
    m->value_on = h->value_on;
    m->value_off = h->value_off;
}

void midi_control_engine_hold(struct midi_control_engine *e, enum midi_control_button b, int pressed)
{
    struct midi_control_button_map m;
    const uint32_t bit = 1uL << (unsigned)b;

    if ((unsigned)b >= MIDI_CONTROL_BUTTON_COUNT)
        return;
    hold_map(e, (unsigned)b, &m);
    if (m.action == MIDI_CONTROL_ACT_NONE)
        return;
    if (pressed ? (e->hold_active & bit) != 0 : !(e->hold_active & bit))
        return;                          /* engaged twice, or released when it never was */
    if (pressed) {
        /* One physical press: counted here unless the press action counted it
         * already, as a Tap tempo press that is not kept waiting does. */
        if (!e->held[b])
            e->presses[b] = (uint8_t)((e->presses[b] + 1u) & 0x7Fu);
        e->hold_active |= bit;
    } else {
        e->hold_active &= ~bit;
    }
    e->held[b] = pressed ? 1u : 0u;
    act(e, &m, pressed, &e->hold_toggled[b]);
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

        case MIDI_CONTROL_ENCM_CC_RELATIVE_SIGNED:
            /* Sign and magnitude: bit 6 says anticlockwise. */
            send3(e, 0xB0, ch, m->number, (uint8_t)(dir > 0 ? rel : 64 + rel));
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
            e->bank_waiting = 0;
            step_program(e, dir * step, ch);
            break;

        default:
            return;
        }
    }
    update_leds(e);
}

/* The padding at each end of the travel (docs/05 section 5): a position within
 * `pad_lo` percent of the bottom reads as 0, one within `pad_hi` percent of the
 * top as 127, and what lies between is stretched back over 0..127. Worked in
 * hundredths of a position so the padding is exact and the identity at 0/0.
 * The limits are applied again here because the engine reads whatever
 * configuration it is handed. */
static unsigned padded(unsigned position, unsigned pad_lo, unsigned pad_hi)
{
    unsigned x = position * 100u, lo, hi, live;

    if (pad_lo > MIDI_CONTROL_ANALOG_PAD_MAX)
        pad_lo = MIDI_CONTROL_ANALOG_PAD_MAX;
    if (pad_hi > MIDI_CONTROL_ANALOG_PAD_MAX)
        pad_hi = MIDI_CONTROL_ANALOG_PAD_MAX;
    lo = pad_lo * 127u;
    hi = (100u - pad_hi) * 127u;
    live = 100u - pad_lo - pad_hi;

    if (x <= lo)
        return 0u;
    if (x >= hi)
        return 127u;
    return (x - lo + live / 2u) / live;     /* x - lo < live * 127, so at most 127 */
}

uint8_t midi_control_engine_analog_value(const struct midi_control_analog_map *m, uint8_t position)
{
    /* Padding first, on the physical travel, so the heel keeps its padding
     * whichever output it is inverted onto. */
    unsigned v = padded(position & 0x7Fu, m->pad_lo, m->pad_hi);
    if (m->invert)
        v = 127u - v;
    unsigned lo = m->out_min, hi = m->out_max;
    if (lo > hi) {
        unsigned t = lo;
        lo = hi;
        hi = t;
    }
    /* Scale 0..127 onto lo..hi, rounding to nearest. */
    unsigned out = lo + (v * (hi - lo) + 63u) / 127u;
    return (uint8_t)(out > 127u ? 127u : out);
}

/* A 7-bit value spread over pitch bend's fourteen bits so that both ends and
 * the centre are exact: 0 -> 0, 64 -> 8192 (no bend), 127 -> 16383. A plain
 * multiply by 129 reaches both ends but puts 64 at 8256, sharp of centre - at
 * a 12-semitone bend range about 9 cents, at the heel of an up-only bend. */
static unsigned bend14(unsigned v)
{
    if (v <= 64u)
        return v * 128u;
    return 8192u + ((v - 64u) * 8191u + 31u) / 63u;
}

/* Send analog control `a`'s position as its mapping has it, unless the host
 * has that already: the same value, to the same place. */
static void analog_send(struct midi_control_engine *e, unsigned a)
{
    const struct midi_control_analog_map *m = &e->cfg->analogs[a];
    const uint8_t ch = channel_of(e, m->channel);
    uint8_t *dest = e->analog_dest[a];

    if (m->mode == MIDI_CONTROL_ANM_NONE)
        return;

    unsigned out = midi_control_engine_analog_value(m, e->analog_in[a]);
    if ((int16_t)out == e->analog_sent[a] && dest[0] == m->mode && dest[1] == ch && dest[2] == m->number)
        return;
    e->analog_sent[a] = (int16_t)out;
    dest[0] = m->mode;
    dest[1] = ch;
    dest[2] = m->number;

    switch (m->mode) {
    case MIDI_CONTROL_ANM_CC:
        send3(e, 0xB0, ch, m->number, (uint8_t)out);
        break;
    case MIDI_CONTROL_ANM_PITCH_BEND: {
        unsigned pb = bend14(out);
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

void midi_control_engine_analog(struct midi_control_engine *e, enum midi_control_analog a, uint8_t value)
{
    if ((unsigned)a >= MIDI_CONTROL_ANALOG_COUNT)
        return;
    e->analog_in[a] = value & 0x7Fu;
    e->analog_seen[a] = 1u;
    analog_send(e, (unsigned)a);
}

void midi_control_engine_resend_analogs(struct midi_control_engine *e)
{
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++) {
        if (!e->analog_seen[i])
            continue;
        e->analog_sent[i] = -1;
        analog_send(e, i);
    }
}

/* Whether the mapping sends programs on channel `ch` (0..15): the global
 * channel always, and any channel a program-moving control is set to. A
 * Program Change on another channel is for another device. */
static int sends_programs_on(const struct midi_control_engine *e, uint8_t ch)
{
    if (ch == (e->cfg->global.midi_channel & 0x0Fu))
        return 1;
    for (unsigned b = 0; b < MIDI_CONTROL_BUTTON_COUNT; b++) {
        const struct midi_control_button_map *m = &e->cfg->buttons[b];
        if (m->action >= MIDI_CONTROL_ACT_PROGRAM && m->action <= MIDI_CONTROL_ACT_PRESET_IN_BANK
            && channel_of(e, m->channel) == ch)
            return 1;
    }
    for (unsigned i = 0; i < MIDI_CONTROL_ENCODER_COUNT; i++) {
        const struct midi_control_encoder_map *m = &e->cfg->encoders[i];
        if (m->mode == MIDI_CONTROL_ENCM_PROGRAM && channel_of(e, m->channel) == ch)
            return 1;
    }
    return 0;
}

/* The host says CC `cc` on `ch` is now `value`: a toggle that sends it is on
 * when the value is nearer its on value than its off one, and an absolute
 * encoder that sends it carries on from there. */
static void follow_cc(struct midi_control_engine *e, uint8_t ch, uint8_t cc, uint8_t value)
{
    for (unsigned b = 0; b < MIDI_CONTROL_BUTTON_COUNT; b++) {
        const struct midi_control_button_map *m = &e->cfg->buttons[b];
        if (m->action != MIDI_CONTROL_ACT_CC_TOGGLE || m->number != cc || channel_of(e, m->channel) != ch)
            continue;
        const unsigned to_on = value > m->value_on ? value - m->value_on : m->value_on - value;
        const unsigned to_off = value > m->value_off ? value - m->value_off : m->value_off - value;
        e->toggled[b] = to_on <= to_off ? 1u : 0u;
    }
    for (unsigned i = 0; i < MIDI_CONTROL_ENCODER_COUNT; i++) {
        const struct midi_control_encoder_map *m = &e->cfg->encoders[i];
        if (m->mode == MIDI_CONTROL_ENCM_CC_ABSOLUTE && m->number == cc && channel_of(e, m->channel) == ch)
            e->enc_value[i] = value & 0x7Fu;
    }
}

/* Likewise a note: a Note toggle that sends it is on while the host has it on. */
static void follow_note(struct midi_control_engine *e, uint8_t ch, uint8_t note, int on)
{
    for (unsigned b = 0; b < MIDI_CONTROL_BUTTON_COUNT; b++) {
        const struct midi_control_button_map *m = &e->cfg->buttons[b];
        if (m->action == MIDI_CONTROL_ACT_NOTE_TOGGLE && m->number == note && channel_of(e, m->channel) == ch)
            e->toggled[b] = on ? 1u : 0u;
    }
}

void midi_control_engine_midi_in(struct midi_control_engine *e, const uint8_t *msg, uint32_t len)
{
    if (len < 2u || !e->cfg->global.follow_program_change)
        return;

    const uint8_t ch = msg[0] & 0x0Fu;
    switch (msg[0] & 0xF0u) {
    case 0xC0:
        if (!sends_programs_on(e, ch))
            return;
        e->program = msg[1] & 0x7Fu;
        e->bank_waiting = 0;
        break;
    case 0xB0:
        if (len < 3u)
            return;
        follow_cc(e, ch, msg[1] & 0x7Fu, msg[2] & 0x7Fu);
        break;
    case 0x90:
        if (len < 3u)
            return;
        follow_note(e, ch, msg[1] & 0x7Fu, (msg[2] & 0x7Fu) != 0u);
        break;
    case 0x80:
        if (len < 3u)
            return;
        follow_note(e, ch, msg[1] & 0x7Fu, 0);
        break;
    default:
        return;
    }
    update_leds(e);
}
