#include <string.h>
#include "midi_control/midi_control_device.h"

static void midi_out(void *ctx, const uint8_t *bytes, uint32_t len)
{
    struct midi_control_device *d = (struct midi_control_device *)ctx;
    if (d->ops.midi_out)
        d->ops.midi_out(d->ops.ctx, bytes, len);
}

int midi_control_device_storage_state(const struct midi_control_device *d)
{
    if (!d->ops.storage_state || !d->ops.storage_read || !d->ops.storage_write)
        return MIDI_CONTROL_STORAGE_NONE;
    return d->ops.storage_state(d->ops.ctx);
}

/* Read both slots, keep the valid one with the higher sequence. Returns 1 if
 * one was found. */
static int load_newest(struct midi_control_device *d, struct midi_control_config *out, unsigned *slot_out)
{
    int found = 0;
    uint8_t buf[MIDI_CONTROL_CONFIG_BYTES];
    struct midi_control_config c;

    for (unsigned s = 0; s < MIDI_CONTROL_STORAGE_SLOTS; s++) {
        if (!d->ops.storage_read(d->ops.ctx, s, buf, MIDI_CONTROL_CONFIG_BYTES))
            continue;
        if (midi_control_config_read(&c, buf, MIDI_CONTROL_CONFIG_BYTES) != MIDI_CONTROL_STATUS_OK)
            continue;
        if (!found || c.sequence > out->sequence) {
            *out = c;
            *slot_out = s;
            found = 1;
        }
    }
    return found;
}

void midi_control_device_init(struct midi_control_device *d, const struct midi_control_platform_ops *ops)
{
    memset(d, 0, sizeof(*d));
    d->ops = *ops;

    midi_control_config_defaults(&d->config);
    d->newest_slot = MIDI_CONTROL_STORAGE_SLOTS - 1u;   /* so the first save lands in slot 0 */

    int state = midi_control_device_storage_state(d);
    if (state == MIDI_CONTROL_STORAGE_READY || state == MIDI_CONTROL_STORAGE_LOCKED) {
        /* Locked storage can still be read; the slots may hold a config from
         * a time when it was not locked. */
        struct midi_control_config c;
        unsigned slot = 0;
        if (load_newest(d, &c, &slot)) {
            d->config = c;
            d->newest_slot = slot;
            d->loaded_from_storage = 1;
        }
    }

    midi_control_engine_init(&d->engine, &d->config, midi_out, d);
}

/* A control was used, or the configuration replaced: the display lights, and
 * the display timeout counts from now. */
static void wake(struct midi_control_device *d)
{
    d->idle_ms = 0;
    d->display_off = 0;
}

void midi_control_device_set_config(struct midi_control_device *d, const struct midi_control_config *cfg)
{
    uint32_t seq = d->config.sequence;
    d->config = *cfg;
    d->config.sequence = seq;
    midi_control_config_sanitise(&d->config);
    d->dirty = 1;
    wake(d);
    midi_control_engine_config_changed(&d->engine, &d->config);
}

int midi_control_device_save(struct midi_control_device *d)
{
    int state = midi_control_device_storage_state(d);
    int rc;

    if (state == MIDI_CONTROL_STORAGE_NONE)
        rc = MIDI_CONTROL_STATUS_NO_STORAGE;
    else if (state != MIDI_CONTROL_STORAGE_READY)
        rc = MIDI_CONTROL_STATUS_STORAGE_LOCKED;
    else {
        uint8_t buf[MIDI_CONTROL_CONFIG_BYTES];
        struct midi_control_config c = d->config;
        unsigned slot = (d->newest_slot + 1u) % MIDI_CONTROL_STORAGE_SLOTS;

        c.sequence = d->config.sequence + 1u;
        midi_control_config_write(&c, buf);
        rc = d->ops.storage_write(d->ops.ctx, slot, buf, MIDI_CONTROL_CONFIG_BYTES);
        if (rc == MIDI_CONTROL_STATUS_OK) {
            d->config.sequence = c.sequence;
            d->newest_slot = slot;
            d->dirty = 0;
            d->loaded_from_storage = 1;
        }
    }
    d->last_save_status = (uint8_t)rc;
    return rc;
}

int midi_control_device_reload(struct midi_control_device *d)
{
    int state = midi_control_device_storage_state(d);
    if (state == MIDI_CONTROL_STORAGE_NONE)
        return MIDI_CONTROL_STATUS_NO_STORAGE;

    struct midi_control_config c;
    unsigned slot = 0;
    if (!load_newest(d, &c, &slot)) {
        midi_control_config_defaults(&d->config);
        d->loaded_from_storage = 0;
    } else {
        d->config = c;
        d->newest_slot = slot;
        d->loaded_from_storage = 1;
    }
    d->dirty = 0;
    wake(d);
    midi_control_engine_config_changed(&d->engine, &d->config);
    return MIDI_CONTROL_STATUS_OK;
}

/* ---- the protocol ---------------------------------------------------------- */

static void reply(struct midi_control_device *d, uint32_t len)
{
    if (len)
        midi_out(d, d->tx, len);
}

/* An EVENT: the control used as the board layer would report it. Returns a
 * MIDI_CONTROL_STATUS_* code. */
static int handle_event(struct midi_control_device *d, uint8_t kind, uint8_t id, int value)
{
    switch (kind) {
    case MIDI_CONTROL_EVENT_BUTTON:
        if (id >= MIDI_CONTROL_BUTTON_COUNT || value < 0 || value > 1)
            return MIDI_CONTROL_STATUS_INVALID_VALUE;
        midi_control_device_button(d, (enum midi_control_button)id, value);
        return MIDI_CONTROL_STATUS_OK;

    case MIDI_CONTROL_EVENT_ENCODER:
        if (id >= MIDI_CONTROL_ENCODER_COUNT)
            return MIDI_CONTROL_STATUS_INVALID_VALUE;
        midi_control_device_encoder(d, (enum midi_control_encoder)id, value);
        return MIDI_CONTROL_STATUS_OK;

    case MIDI_CONTROL_EVENT_ANALOG:
        if (id >= MIDI_CONTROL_ANALOG_COUNT || value < 0 || value > 127)
            return MIDI_CONTROL_STATUS_INVALID_VALUE;
        midi_control_device_analog(d, (enum midi_control_analog)id, (uint8_t)value);
        return MIDI_CONTROL_STATUS_OK;

    default:
        return MIDI_CONTROL_STATUS_INVALID_VALUE;
    }
}

static void handle_sysex(struct midi_control_device *d, const uint8_t *sysex, uint32_t len)
{
    struct midi_control_message msg;
    if (!midi_control_parse_message(sysex, len, &msg))
        return;

    switch (msg.command) {
    case MIDI_CONTROL_CMD_IDENTIFY: {
        struct midi_control_identity id;
        memset(&id, 0, sizeof(id));
        id.protocol_version = MIDI_CONTROL_PROTOCOL_VERSION;
        id.fw_major = d->ops.fw_major;
        id.fw_minor = d->ops.fw_minor;
        id.fw_patch = d->ops.fw_patch;
        id.platform = d->ops.platform;
        id.storage_state = (uint8_t)midi_control_device_storage_state(d);
        id.dirty = d->dirty;
        id.config_version = MIDI_CONTROL_CONFIG_VERSION;
        id.config_bytes = MIDI_CONTROL_CONFIG_BYTES;
        memcpy(id.name, d->config.global.name, MIDI_CONTROL_CONFIG_NAME_LEN);
        id.name[16] = 0;
        reply(d, midi_control_build_identity(d->tx, &id));
        break;
    }

    case MIDI_CONTROL_CMD_GET_CONFIG: {
        uint8_t buf[MIDI_CONTROL_CONFIG_BYTES];
        midi_control_config_write(&d->config, buf);
        reply(d, midi_control_build_config(d->tx, MIDI_CONTROL_RPL_CONFIG, buf, MIDI_CONTROL_CONFIG_BYTES));
        break;
    }

    case MIDI_CONTROL_CMD_SET_CONFIG: {
        uint8_t buf[MIDI_CONTROL_CONFIG_BYTES];
        struct midi_control_config c;
        uint32_t n = midi_control_parse_config_payload(&msg, buf, sizeof(buf));
        int rc = n ? midi_control_config_read(&c, buf, n) : MIDI_CONTROL_STATUS_BAD_LENGTH;
        if (rc == MIDI_CONTROL_STATUS_OK)
            midi_control_device_set_config(d, &c);
        reply(d, midi_control_build_result(d->tx, msg.command, (uint8_t)rc));
        break;
    }

    case MIDI_CONTROL_CMD_SAVE:
        reply(d, midi_control_build_result(d->tx, msg.command, (uint8_t)midi_control_device_save(d)));
        break;

    case MIDI_CONTROL_CMD_LOAD_DEFAULTS: {
        struct midi_control_config c;
        midi_control_config_defaults(&c);
        midi_control_device_set_config(d, &c);
        reply(d, midi_control_build_result(d->tx, msg.command, MIDI_CONTROL_STATUS_OK));
        break;
    }

    case MIDI_CONTROL_CMD_GET_STATUS: {
        struct midi_control_status_reply st;
        st.storage_state = (uint8_t)midi_control_device_storage_state(d);
        st.dirty = d->dirty;
        st.program = d->engine.program;
        st.sequence = d->config.sequence;
        reply(d, midi_control_build_status(d->tx, &st));
        break;
    }

    case MIDI_CONTROL_CMD_SET_TEXT: {
        int rc = MIDI_CONTROL_STATUS_INVALID_VALUE;
        if (msg.payload_len >= 1u && msg.payload[0] < MIDI_CONTROL_TEXT_LINES) {
            char *line = d->text[msg.payload[0]];
            uint32_t n = msg.payload_len - 1u;
            if (n > MIDI_CONTROL_TEXT_LEN)
                n = MIDI_CONTROL_TEXT_LEN;
            for (uint32_t i = 0; i < n; i++) {
                uint8_t c = msg.payload[1 + i];
                line[i] = (char)((c < 0x20u) ? ' ' : c);
            }
            line[n] = 0;
            rc = MIDI_CONTROL_STATUS_OK;
        }
        reply(d, midi_control_build_result(d->tx, msg.command, (uint8_t)rc));
        break;
    }

    case MIDI_CONTROL_CMD_RELOAD:
        reply(d, midi_control_build_result(d->tx, msg.command, (uint8_t)midi_control_device_reload(d)));
        break;

    case MIDI_CONTROL_CMD_GET_STATE: {
        struct midi_control_state_reply st;
        midi_control_device_state(d, &st);
        reply(d, midi_control_build_state(d->tx, &st));
        break;
    }

    case MIDI_CONTROL_CMD_EVENT: {
        uint8_t kind = 0, id = 0;
        int value = 0;
        int rc = midi_control_parse_event(&msg, &kind, &id, &value) ? handle_event(d, kind, id, value)
                                                                   : MIDI_CONTROL_STATUS_INVALID_VALUE;
        /* Whatever the control sent has gone out already; the reply follows it. */
        reply(d, midi_control_build_result(d->tx, msg.command, (uint8_t)rc));
        break;
    }

    default:
        reply(d, midi_control_build_result(d->tx, msg.command, MIDI_CONTROL_STATUS_UNKNOWN_COMMAND));
        break;
    }
}

/* Data bytes a channel status expects. */
static uint8_t data_bytes(uint8_t status)
{
    switch (status & 0xF0u) {
    case 0xC0: case 0xD0: return 1;
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
    default: return 0;
    }
}

void midi_control_device_midi_in(struct midi_control_device *d, const uint8_t *bytes, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = bytes[i];

        if (b == 0xF0) {
            d->rx_in_sysex = 1;
            d->rx_len = 0;
            d->rx[d->rx_len++] = b;
            continue;
        }
        if (d->rx_in_sysex) {
            if (b == 0xF7) {
                if (d->rx_len < sizeof(d->rx)) {
                    d->rx[d->rx_len++] = b;
                    handle_sysex(d, d->rx, d->rx_len);
                }
                d->rx_in_sysex = 0;
                d->rx_len = 0;
            } else if (b & 0x80u) {
                /* A real-time byte inside SysEx is legal and ignored; any
                 * other status aborts the SysEx and is then processed. */
                if (b < 0xF8u) {
                    d->rx_in_sysex = 0;
                    d->rx_len = 0;
                    i--;
                }
            } else if (d->rx_len < sizeof(d->rx)) {
                d->rx[d->rx_len++] = b;
            } else {
                d->rx_in_sysex = 0;          /* too long to be ours */
                d->rx_len = 0;
            }
            continue;
        }

        if (b >= 0xF8u)
            continue;                        /* real-time */
        if (b & 0x80u) {
            if (b >= 0xF0u) {                /* other system common: no running status */
                d->rx_status = 0;
                d->rx_need = 0;
                continue;
            }
            d->rx_status = b;
            d->rx_need = data_bytes(b);
            d->rx_len = 0;
            d->rx[d->rx_len++] = b;
            continue;
        }
        if (!d->rx_status)
            continue;
        if (d->rx_need == 0) {               /* running status: a new message */
            d->rx_len = 0;
            d->rx[d->rx_len++] = d->rx_status;
            d->rx_need = data_bytes(d->rx_status);
        }
        d->rx[d->rx_len++] = b;
        if (--d->rx_need == 0)
            midi_control_engine_midi_in(&d->engine, d->rx, d->rx_len);
    }
}

void midi_control_device_button(struct midi_control_device *d, enum midi_control_button b, int pressed)
{
    wake(d);
    midi_control_engine_button(&d->engine, b, pressed);
}

void midi_control_device_encoder(struct midi_control_device *d, enum midi_control_encoder e, int detents)
{
    wake(d);
    midi_control_engine_encoder(&d->engine, e, detents);
}

void midi_control_device_analog(struct midi_control_device *d, enum midi_control_analog a, uint8_t value)
{
    wake(d);
    midi_control_engine_analog(&d->engine, a, value);
}

void midi_control_device_tick(struct midi_control_device *d, uint32_t elapsed_ms)
{
    const uint32_t timeout_ms = (uint32_t)d->config.global.display_timeout * 1000u;

    /* Saturating: a count that wrapped back under the timeout would never reach it. */
    d->idle_ms = elapsed_ms > 0xFFFFFFFFu - d->idle_ms ? 0xFFFFFFFFu : d->idle_ms + elapsed_ms;
    if (timeout_ms && d->idle_ms >= timeout_ms)
        d->display_off = 1;
}

int midi_control_device_display_on(const struct midi_control_device *d)
{
    return !d->display_off;
}

const uint8_t *midi_control_device_leds(const struct midi_control_device *d)
{
    return d->engine.led;
}

void midi_control_device_state(const struct midi_control_device *d, struct midi_control_state_reply *st)
{
    const struct midi_control_engine *e = &d->engine;

    memset(st, 0, sizeof(*st));
    st->storage_state = (uint8_t)midi_control_device_storage_state(d);
    st->dirty = d->dirty;
    st->program = e->program;
    memcpy(st->held, e->held, sizeof(st->held));
    memcpy(st->toggled, e->toggled, sizeof(st->toggled));
    memcpy(st->presses, e->presses, sizeof(st->presses));
    memcpy(st->enc_value, e->enc_value, sizeof(st->enc_value));
    memcpy(st->enc_turns, e->enc_turns, sizeof(st->enc_turns));
    memcpy(st->analog, e->analog_in, sizeof(st->analog));
    for (unsigned line = 0; line < MIDI_CONTROL_TEXT_LINES; line++)
        memcpy(st->text[line], d->text[line], MIDI_CONTROL_TEXT_LEN + 1u);
    st->display_off = d->display_off;
}

void midi_control_device_mirror(struct midi_control_device *d, const struct midi_control_state_reply *st)
{
    struct midi_control_engine *e = &d->engine;

    d->dirty = st->dirty ? 1u : 0u;
    d->idle_ms = 0;
    d->display_off = st->display_off ? 1u : 0u;
    for (unsigned i = 0; i < MIDI_CONTROL_BUTTON_COUNT; i++) {
        e->held[i] = st->held[i] ? 1u : 0u;
        e->toggled[i] = st->toggled[i] ? 1u : 0u;
        e->presses[i] = st->presses[i] & 0x7Fu;
    }
    for (unsigned i = 0; i < MIDI_CONTROL_ENCODER_COUNT; i++) {
        e->enc_value[i] = st->enc_value[i] & 0x7Fu;
        e->enc_turns[i] = st->enc_turns[i] & 0x7Fu;
    }
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++)
        e->analog_in[i] = st->analog[i] & 0x7Fu;
    for (unsigned line = 0; line < MIDI_CONTROL_TEXT_LINES; line++) {
        memcpy(d->text[line], st->text[line], MIDI_CONTROL_TEXT_LEN);
        d->text[line][MIDI_CONTROL_TEXT_LEN] = 0;
    }
    midi_control_engine_set_program(e, st->program);   /* last: it re-derives the LEDs */
}
