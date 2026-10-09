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

/* The stored calibration has been replaced under the learners (init, RELOAD, a
 * recalibration): start them again from it, keeping the platform's seeds and
 * which way round each control reads. A control the platform never seeded has
 * nothing to re-seed - it is fed positions, not raw counts. */
static void reseed_analog(struct midi_control_device *d)
{
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++) {
        struct midi_control_analog_learn *l = &d->analog_learn[i];
        if (!d->analog_raw[i])
            continue;
        midi_control_analog_learn_init(l, &d->config.analog_cal[i],
                                       l->seed_lo, l->seed_hi, l->invert);
    }
    d->cal_dirty = 0u;
    d->cal_settle_ms = 0u;
    /* A capture under way was measuring for the calibration just replaced,
     * and one taken but not stored has been replaced with it. */
    d->capture.stage = MIDI_CONTROL_CAL_OFF;
    d->cal_unsaved = 0u;
    d->cal_write_now = 0u;
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

/* The whole configuration has been replaced, so the profiles under the picker
 * are not the ones it was showing. It closes rather than keeping a page that
 * may no longer exist. */
static void close_picker(struct midi_control_device *d)
{
    d->profile_picking = 0u;
    d->profile_page = 0u;
    d->pick_idle_ms = 0u;
}

/* A capture is taking the pedal's stops, as against showing how one ended. */
static int capture_running(const struct midi_control_device *d)
{
    return d->capture.stage == MIDI_CONTROL_CAL_FIRST || d->capture.stage == MIDI_CONTROL_CAL_SECOND;
}

void midi_control_device_set_config(struct midi_control_device *d, const struct midi_control_config *cfg)
{
    uint32_t seq = d->config.sequence;
    struct midi_control_analog_cal cal[MIDI_CONTROL_ANALOG_COUNT];

    /* The calibration describes this unit's pedal, not what the user asked
     * for. A record from a file, or from another unit, does not carry it -
     * only midi_control_device_analog_recalibrate() clears it. */
    memcpy(cal, d->config.analog_cal, sizeof(cal));
    d->config = *cfg;
    d->config.sequence = seq;
    memcpy(d->config.analog_cal, cal, sizeof(cal));
    midi_control_config_sanitise(&d->config);
    d->dirty = 1;
    close_picker(d);
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
            d->cal_unsaved = 0u;         /* the record written carries a captured travel */
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
    reseed_analog(d);
    close_picker(d);
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
        midi_control_device_analog_recalibrate(d);   /* the one command that does */
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

    case MIDI_CONTROL_CMD_CALIBRATE: {
        uint8_t analog = 0;
        int start = 0;
        int rc = midi_control_parse_calibrate(&msg, &analog, &start)
               ? midi_control_device_calibrate(d, (enum midi_control_analog)analog, start)
               : MIDI_CONTROL_STATUS_INVALID_VALUE;
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

/* ---- profiles -------------------------------------------------------------- */

unsigned midi_control_device_profile_pages(const struct midi_control_device *d)
{
    const unsigned n = d->config.profile_count < 1u ? 1u : d->config.profile_count;
    return (n + MIDI_CONTROL_PROFILE_PAGE - 1u) / MIDI_CONTROL_PROFILE_PAGE;
}

int midi_control_device_picking_profile(const struct midi_control_device *d)
{
    return d->profile_picking != 0u;
}

/* A page of the picker is the four footswitches A-D, in that order. */
typedef char midi_control_picker_page_check[
    (MIDI_CONTROL_BTN_FS_D - MIDI_CONTROL_BTN_FS_A + 1 == (int)MIDI_CONTROL_PROFILE_PAGE) ? 1 : -1];

int midi_control_device_picker_profile(const struct midi_control_device *d, enum midi_control_button b)
{
    const unsigned place = (unsigned)b - (unsigned)MIDI_CONTROL_BTN_FS_A;
    unsigned index;

    if (place >= MIDI_CONTROL_PROFILE_PAGE)
        return -1;
    index = (unsigned)d->profile_page * MIDI_CONTROL_PROFILE_PAGE + place;
    return index < d->config.profile_count ? (int)index : -1;
}

/* What the footswitch LEDs show while the picker is open: A-D blue for a
 * profile, white for the live one and dark where the page has none; Bank Up
 * and Bank Down green when there is another page to go to. Tap keeps its
 * lamps, because the picker does not take it. Worked out from the state each
 * time it is asked for, so it can never lag behind it. */
static void picker_leds(const struct midi_control_device *d, uint8_t *led)
{
    static const uint8_t k_buttons[] = {
        MIDI_CONTROL_BTN_FS_A, MIDI_CONTROL_BTN_FS_B, MIDI_CONTROL_BTN_FS_C, MIDI_CONTROL_BTN_FS_D,
        MIDI_CONTROL_BTN_BANK_UP, MIDI_CONTROL_BTN_BANK_DOWN,
    };
    const int paging = midi_control_device_profile_pages(d) > 1u;

    for (unsigned i = 0; i < sizeof(k_buttons); i++) {
        const enum midi_control_button b = (enum midi_control_button)k_buttons[i];
        const int col = midi_control_button_led_column(b);
        const int profile = midi_control_device_picker_profile(d, b);

        if (col < 0)
            continue;
        if (b == MIDI_CONTROL_BTN_BANK_UP || b == MIDI_CONTROL_BTN_BANK_DOWN)
            led[col] = paging ? MIDI_CONTROL_LED_GREEN : MIDI_CONTROL_LED_OFF;
        else if (profile < 0)
            led[col] = MIDI_CONTROL_LED_OFF;
        else
            led[col] = (unsigned)profile == d->config.profile_active ? MIDI_CONTROL_LED_WHITE
                                                                     : MIDI_CONTROL_LED_BLUE;
    }
}

void midi_control_device_show_profiles(struct midi_control_device *d, int open)
{
    d->profile_picking = open ? 1u : 0u;
    d->pick_idle_ms = 0u;
    if (open)
        d->profile_page = (uint8_t)(d->config.profile_active / MIDI_CONTROL_PROFILE_PAGE);
    wake(d);
}

int midi_control_device_select_profile(struct midi_control_device *d, unsigned n)
{
    if (n >= d->config.profile_count)
        return MIDI_CONTROL_STATUS_INVALID_VALUE;

    /* A switch still down was pressed under the old mapping, and its release
     * has to be the old mapping's too: sent under the new one it would end a
     * different note, or none, and leave the old one sounding. Mode is always
     * down when the picker opens, so this is the usual case, not the rare one.
     * Each is released now and its physical release swallowed when it comes. */
    for (unsigned b = 0; b < MIDI_CONTROL_BUTTON_COUNT; b++) {
        if (!d->engine.held[b])
            continue;
        midi_control_engine_button(&d->engine, (enum midi_control_button)b, 0);
        d->picker_took |= 1uL << b;
    }

    /* Same shape as SET_CONFIG: the mapping is replaced live, the toggles and
     * the program are left where the performance put them, and the record now
     * differs from what is stored - which it does, because which profile is
     * live is part of it. */
    midi_control_config_select_profile(&d->config, n);
    midi_control_config_sanitise(&d->config);
    d->dirty = 1;
    wake(d);
    midi_control_engine_config_changed(&d->engine, &d->config);
    return MIDI_CONTROL_STATUS_OK;
}

/* A switch the open picker owns. Returns 1 if it took the press, in which case
 * the engine never sees it and the switch sends nothing. */
static int picker_press(struct midi_control_device *d, enum midi_control_button b)
{
    const unsigned pages = midi_control_device_profile_pages(d);
    int profile;

    d->pick_idle_ms = 0u;

    switch (b) {
    case MIDI_CONTROL_BTN_FS_A:
    case MIDI_CONTROL_BTN_FS_B:
    case MIDI_CONTROL_BTN_FS_C:
    case MIDI_CONTROL_BTN_FS_D:
        /* A switch past the last profile in use is not an error and not a
         * reason to close: the page simply has nothing there. */
        profile = midi_control_device_picker_profile(d, b);
        if (profile >= 0 && midi_control_device_select_profile(d, (unsigned)profile) == MIDI_CONTROL_STATUS_OK)
            midi_control_device_show_profiles(d, 0);
        return 1;

    case MIDI_CONTROL_BTN_BANK_UP:
        d->profile_page = (uint8_t)((d->profile_page + 1u) % pages);
        return 1;

    case MIDI_CONTROL_BTN_BANK_DOWN:
        d->profile_page = (uint8_t)((d->profile_page + pages - 1u) % pages);
        return 1;

    default:
        return 0;
    }
}

/* Mode held opens the picker and Mode pressed again closes it, both without
 * disturbing what Mode is mapped to: the press and the release still reach the
 * engine, so a Mode mapped to a note still sends that note. The flag is what
 * stops one hold doing both. */
static void mode_edge(struct midi_control_device *d, int pressed)
{
    d->mode_down = pressed ? 1u : 0u;
    d->mode_held_ms = 0u;
    if (!pressed) {
        /* Letting go of Mode is where the picker's own timeout starts, so a
         * long hold does not eat into the time to choose. */
        d->pick_idle_ms = 0u;
        return;
    }
    d->mode_hold_used = 0u;
    /* Mode puts away a calibration on the screen, cancelling one still taking
     * its stops, and the same hold does not go on to open the picker. */
    if (d->capture.stage != MIDI_CONTROL_CAL_OFF) {
        if (capture_running(d)) {
            d->capture.stage = MIDI_CONTROL_CAL_FAILED;
            d->capture_shown_ms = 0u;
        } else {
            d->capture.stage = MIDI_CONTROL_CAL_OFF;
        }
        d->mode_hold_used = 1u;
        return;
    }
    if (d->profile_picking) {
        midi_control_device_show_profiles(d, 0);
        d->mode_hold_used = 1u;
    }
}

/* The toe switch held starts calibrating the pedal; midi_control_device_tick()
 * times the hold. Like Mode's, the press and the release still reach the
 * engine, so a toe switch mapped to something still does it. */
static void toe_edge(struct midi_control_device *d, int pressed)
{
    d->toe_down = pressed ? 1u : 0u;
    d->toe_held_ms = 0u;
    if (pressed)
        d->toe_hold_used = 0u;
}

void midi_control_device_button(struct midi_control_device *d, enum midi_control_button b, int pressed)
{
    const uint32_t bit = (unsigned)b < MIDI_CONTROL_BUTTON_COUNT ? 1uL << (unsigned)b : 0u;

    wake(d);
    if (b == MIDI_CONTROL_BTN_MODE)
        mode_edge(d, pressed);
    else if (b == MIDI_CONTROL_BTN_TOE)
        toe_edge(d, pressed);
    else if (pressed && d->profile_picking && picker_press(d, b)) {
        d->picker_took |= bit;
        return;
    }

    /* Only the release of a press that never reached the engine is swallowed:
     * one the picker took, or one a profile change has already released. A
     * switch that was down when the picker opened has sent its Note On, and
     * swallowing its release would leave that note sounding for good. */
    if (!pressed && (d->picker_took & bit)) {
        d->picker_took &= ~bit;
        return;
    }
    /* A press the engine does see starts afresh: a release still owed from an
     * earlier one (an EVENT that never sent its release) must not swallow this
     * press's own. */
    if (pressed)
        d->picker_took &= ~bit;
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

void midi_control_device_analog_seed(struct midi_control_device *d, enum midi_control_analog a,
                                     uint16_t seed_lo, uint16_t seed_hi, int invert)
{
    if ((unsigned)a >= MIDI_CONTROL_ANALOG_COUNT)
        return;
    d->analog_raw[a] = 1u;
    midi_control_analog_learn_init(&d->analog_learn[a], &d->config.analog_cal[a],
                                   seed_lo, seed_hi, invert);
}

void midi_control_device_analog_raw(struct midi_control_device *d, enum midi_control_analog a, uint16_t raw)
{
    struct midi_control_analog_learn *l;
    struct midi_control_analog_cal want;

    if ((unsigned)a >= MIDI_CONTROL_ANALOG_COUNT || !d->analog_raw[a])
        return;
    d->analog_last[a] = raw;

    /* Being measured, not played: the capture takes the count, and neither
     * the learner nor the engine sees it. */
    if (a == d->capture_analog && capture_running(d)) {
        midi_control_analog_capture_sample(&d->capture, raw);
        return;
    }

    l = &d->analog_learn[a];
    if (midi_control_analog_learn_update(l, raw))
        midi_control_device_analog(d, a, (uint8_t)l->position);

    /* Only a sample that moved the window restarts the settle, so a sweep that
     * is still going keeps putting the write off and a sweep that has finished
     * lets it happen. */
    if (l->window_moved && midi_control_analog_learn_store(l, &d->config.analog_cal[a], &want)) {
        d->cal_dirty = 1u;
        d->cal_settle_ms = 0u;
    }
}

void midi_control_device_analog_recalibrate(struct midi_control_device *d)
{
    memset(d->config.analog_cal, 0, sizeof(d->config.analog_cal));
    reseed_analog(d);
}

int midi_control_device_calibrate(struct midi_control_device *d, enum midi_control_analog a, int start)
{
    if (!start) {
        if (capture_running(d)) {
            d->capture.stage = MIDI_CONTROL_CAL_FAILED;
            d->capture_shown_ms = 0u;
            wake(d);
        }
        return MIDI_CONTROL_STATUS_OK;
    }
    if (a != MIDI_CONTROL_ANALOG_PEDAL || !d->analog_raw[a])
        return MIDI_CONTROL_STATUS_INVALID_VALUE;

    midi_control_analog_capture_start(&d->capture);
    /* From where the pedal is now, so a pedal already resting at a stop is
     * timed from the start rather than from the next count to arrive. */
    midi_control_analog_capture_sample(&d->capture, d->analog_last[a]);
    d->capture_analog = (uint8_t)a;
    d->capture_shown_ms = 0u;
    close_picker(d);
    wake(d);
    return MIDI_CONTROL_STATUS_OK;
}

int midi_control_device_calibration(const struct midi_control_device *d)
{
    return d->capture.stage;
}

/* Write a learned travel, once it has stopped moving. */
static void calibration_tick(struct midi_control_device *d, uint32_t elapsed_ms)
{
    struct midi_control_analog_cal want[MIDI_CONTROL_ANALOG_COUNT];
    struct midi_control_analog_cal was[MIDI_CONTROL_ANALOG_COUNT];
    uint8_t user_status;
    int any = 0;

    if (!d->cal_dirty)
        return;
    /* A travel the user asked for is written now; one learned from use waits
     * until the sweep that moved it is over. */
    if (!d->cal_write_now) {
        d->cal_settle_ms = elapsed_ms > 0xFFFFFFFFu - d->cal_settle_ms
                         ? 0xFFFFFFFFu : d->cal_settle_ms + elapsed_ms;
        if (d->cal_settle_ms < MIDI_CONTROL_CAL_SETTLE_MS)
            return;
    }
    d->cal_settle_ms = 0u;
    d->cal_write_now = 0u;

    /* Never over the user's own unsaved edits: a write they did not ask for
     * must not commit changes they have not committed. They will be written
     * with the next SAVE, which carries the calibration too. */
    if (d->dirty || midi_control_device_storage_state(d) != MIDI_CONTROL_STORAGE_READY)
        return;

    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++) {
        struct midi_control_analog_cal one;

        /* learn_store() fills `one` either way and returns whether it is worth
         * writing, so only a yes is allowed to replace what is stored. A
         * control that has not been swept keeps the calibration it has -
         * writing one control must not invent one for the other. */
        want[i] = d->config.analog_cal[i];
        if (d->analog_raw[i]
            && midi_control_analog_learn_store(&d->analog_learn[i], &d->config.analog_cal[i], &one)) {
            want[i] = one;
            any = 1;
        }
    }
    /* A captured travel is in the live record already: it needs no `want` of
     * its own, only the write. */
    if (d->cal_unsaved)
        any = 1;
    if (!any) {
        d->cal_dirty = 0u;
        return;
    }

    /* Adopt it only once the card has taken it: after a failed write the live
     * record still says what is actually stored, so the next settle tries the
     * same thing again rather than believing it is already there. The status
     * the app shows is the user's own last SAVE, not this. */
    user_status = d->last_save_status;
    memcpy(was, d->config.analog_cal, sizeof(was));
    memcpy(d->config.analog_cal, want, sizeof(want));
    if (midi_control_device_save(d) == MIDI_CONTROL_STATUS_OK)
        d->cal_dirty = 0u;
    else
        memcpy(d->config.analog_cal, was, sizeof(was));
    d->last_save_status = user_status;
}

/* Saturating, because a count that wrapped back under its threshold would
 * never reach it. */
static uint32_t add_ms(uint32_t now, uint32_t elapsed)
{
    return elapsed > 0xFFFFFFFFu - now ? 0xFFFFFFFFu : now + elapsed;
}

/* Mode held long enough opens the picker; nothing pressed for long enough
 * closes it again. Both are here rather than on a button edge because they are
 * time passing, which is the one thing a switch cannot report. */
static void profile_tick(struct midi_control_device *d, uint32_t elapsed_ms)
{
    if (d->mode_down && !d->mode_hold_used) {
        d->mode_held_ms = add_ms(d->mode_held_ms, elapsed_ms);
        if (d->mode_held_ms >= MIDI_CONTROL_PROFILE_HOLD_MS) {
            d->mode_hold_used = 1u;
            midi_control_device_show_profiles(d, 1);
            return;      /* the tick that opened it does not also age it */
        }
    }
    /* The timeout is for a picker left open with nobody at the unit, and a
     * finger still on Mode is not that: while it is held the picker stays,
     * however long the hold goes on. */
    if (!d->profile_picking || d->mode_down)
        return;
    d->pick_idle_ms = add_ms(d->pick_idle_ms, elapsed_ms);
    if (d->pick_idle_ms >= MIDI_CONTROL_PROFILE_IDLE_MS)
        d->profile_picking = 0u;
}

/* Both stops taken: they are the pedal's travel from now on. The learner
 * starts again from them, as it does from a stored calibration at power-on,
 * and calibration_tick() writes the record at the next tick unless something
 * of the user's is pending. */
static void finish_capture(struct midi_control_device *d)
{
    const unsigned a = d->capture_analog;
    struct midi_control_analog_learn *l = &d->analog_learn[a];
    struct midi_control_analog_cal cal;

    if (!midi_control_analog_capture_result(&d->capture, &cal))
        return;
    d->config.analog_cal[a] = cal;
    midi_control_analog_learn_init(l, &cal, l->seed_lo, l->seed_hi, l->invert);
    d->cal_unsaved = 1u;
    d->cal_write_now = 1u;
    d->cal_dirty = 1u;
}

/* The toe switch held long enough starts a capture; a capture running is fed
 * the time its holds are made of; how it ended is shown for a moment and then
 * put away. The display stays lit throughout: the pedal is being measured, not
 * played, so nothing else would keep it on. */
static void capture_tick(struct midi_control_device *d, uint32_t elapsed_ms)
{
    struct midi_control_analog_capture *c = &d->capture;

    if (d->toe_down && !d->toe_hold_used) {
        d->toe_held_ms = add_ms(d->toe_held_ms, elapsed_ms);
        if (d->toe_held_ms >= MIDI_CONTROL_CAL_ENTER_HOLD_MS) {
            d->toe_hold_used = 1u;
            if (!capture_running(d)
                && midi_control_device_calibrate(d, MIDI_CONTROL_ANALOG_PEDAL, 1) == MIDI_CONTROL_STATUS_OK)
                return;      /* the tick that started it does not also time it */
        }
    }

    if (c->stage == MIDI_CONTROL_CAL_OFF)
        return;
    wake(d);
    if (capture_running(d)) {
        /* The toe switch presses the pedal past where it rests. */
        const int may_rest = !(d->toe_down && d->capture_analog == MIDI_CONTROL_ANALOG_PEDAL);
        if (midi_control_analog_capture_tick(c, elapsed_ms, may_rest)) {
            d->capture_shown_ms = 0u;
            if (c->stage == MIDI_CONTROL_CAL_DONE)
                finish_capture(d);
        }
        return;
    }
    d->capture_shown_ms = add_ms(d->capture_shown_ms, elapsed_ms);
    if (d->capture_shown_ms >= MIDI_CONTROL_CAL_SHOW_MS)
        c->stage = MIDI_CONTROL_CAL_OFF;
}

void midi_control_device_tick(struct midi_control_device *d, uint32_t elapsed_ms)
{
    const uint32_t timeout_ms = (uint32_t)d->config.global.display_timeout * 1000u;

    profile_tick(d, elapsed_ms);
    capture_tick(d, elapsed_ms);

    d->idle_ms = add_ms(d->idle_ms, elapsed_ms);
    if (timeout_ms && d->idle_ms >= timeout_ms)
        d->display_off = 1;

    calibration_tick(d, elapsed_ms);
}

int midi_control_device_display_on(const struct midi_control_device *d)
{
    return !d->display_off;
}

void midi_control_device_leds(const struct midi_control_device *d, uint8_t *out)
{
    memcpy(out, d->engine.led, MIDI_CONTROL_COLUMNS);
    if (d->profile_picking)
        picker_leds(d, out);
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
    st->profile_active = d->config.profile_active;
    st->profile_picking = d->profile_picking;
    st->profile_page = d->profile_page;

    st->cal_stage = d->capture.stage;
    st->cal_analog = d->capture_analog;
    st->cal_progress = (uint8_t)midi_control_analog_capture_progress(&d->capture);
    st->cal_raw = d->capture.raw;
    st->cal_first = d->capture.first;
    st->cal_second = d->capture.second;
    if (d->analog_raw[MIDI_CONTROL_ANALOG_PEDAL]) {
        const struct midi_control_analog_learn *l = &d->analog_learn[MIDI_CONTROL_ANALOG_PEDAL];
        st->pedal_flags |= MIDI_CONTROL_PEDAL_RAW;
        if (l->seeded)
            st->pedal_flags |= MIDI_CONTROL_PEDAL_SEEDED;
        st->pedal_raw = d->analog_last[MIDI_CONTROL_ANALOG_PEDAL];
        st->pedal_lo = l->lo;
        st->pedal_hi = l->hi;
    }
    if (d->config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].flags & MIDI_CONTROL_CAL_VALID)
        st->pedal_flags |= MIDI_CONTROL_PEDAL_CALIBRATED;
    if (d->cal_unsaved)
        st->pedal_flags |= MIDI_CONTROL_PEDAL_UNSAVED;
    st->program_offset = e->program_offset;
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

    /* The picker as the reporting device has it. Which profile is live is the
     * configuration's, not the state's, so it is taken only as far as this
     * device's own configuration has that profile - a host drawing a unit
     * whose profiles it has not read back yet shows the page it is on, not a
     * profile it knows nothing about. */
    d->profile_picking = st->profile_picking ? 1u : 0u;
    d->profile_page = st->profile_page < midi_control_device_profile_pages(d)
                    ? st->profile_page : 0u;
    midi_control_config_select_profile(&d->config, st->profile_active);

    /* A calibration as the reporting device has it: what its screen needs,
     * which is all a mirror draws. The hold is kept as the time that gives
     * the same progress back. */
    memset(&d->capture, 0, sizeof(d->capture));
    d->capture.stage = st->cal_stage <= MIDI_CONTROL_CAL_FAILED ? st->cal_stage : (uint8_t)MIDI_CONTROL_CAL_OFF;
    d->capture_analog = st->cal_analog < MIDI_CONTROL_ANALOG_COUNT ? st->cal_analog : 0u;
    d->capture.raw = st->cal_raw;
    d->capture.first = st->cal_first;
    d->capture.second = st->cal_second;
    d->capture.still_ms = ((uint32_t)(st->cal_progress & 0x7Fu) * MIDI_CONTROL_CAL_HOLD_MS + 126u) / 127u;
    d->cal_unsaved = (st->pedal_flags & MIDI_CONTROL_PEDAL_UNSAVED) ? 1u : 0u;

    /* Taken as it is, whatever this device's mapping: the screen draws the
     * Program Change buttons where the reporting device has them. */
    e->program_offset = st->program_offset & 0x7Fu;
    midi_control_engine_set_program(e, st->program);   /* last: it re-derives the LEDs */
}
