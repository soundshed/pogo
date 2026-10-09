#include <string.h>
#include "midi_control/midi_control_protocol.h"

uint32_t midi_control_pack7(const uint8_t *in, uint32_t len, uint8_t *out)
{
    uint32_t o = 0;

    for (uint32_t i = 0; i < len; i += 7u) {
        uint32_t n = len - i < 7u ? len - i : 7u;
        uint8_t hi = 0;

        for (uint32_t k = 0; k < n; k++)
            if (in[i + k] & 0x80u)
                hi |= (uint8_t)(1u << k);
        out[o++] = hi;
        for (uint32_t k = 0; k < n; k++)
            out[o++] = in[i + k] & 0x7Fu;
    }
    return o;
}

uint32_t midi_control_unpack7(const uint8_t *in, uint32_t len, uint8_t *out, uint32_t max_out)
{
    uint32_t o = 0;
    uint32_t i = 0;

    while (i < len) {
        uint32_t n = len - i - 1u;
        if (n == 0)
            return 0;                       /* a lone high byte with nothing after it */
        if (n > 7u)
            n = 7u;
        uint8_t hi = in[i++];
        if (hi & 0x80u)
            return 0;
        for (uint32_t k = 0; k < n; k++) {
            if (o >= max_out || (in[i] & 0x80u))
                return 0;
            out[o++] = (uint8_t)(in[i++] | (((hi >> k) & 1u) << 7));
        }
    }
    return o;
}

/* ---- building -------------------------------------------------------------- */

static uint32_t header(uint8_t *out, uint8_t command)
{
    out[0] = 0xF0;
    out[1] = MIDI_CONTROL_SYSEX_MFR;
    out[2] = MIDI_CONTROL_SYSEX_ID0;
    out[3] = MIDI_CONTROL_SYSEX_ID1;
    out[4] = command & 0x7Fu;
    return MIDI_CONTROL_SYSEX_HEADER;
}

uint32_t midi_control_build_simple(uint8_t *out, uint8_t command)
{
    uint32_t n = header(out, command);
    out[n++] = 0xF7;
    return n;
}

uint32_t midi_control_build_config(uint8_t *out, uint8_t command,
                                   const uint8_t *record, uint32_t record_len)
{
    if (record_len > MIDI_CONTROL_CONFIG_MAX_BYTES)
        return 0;
    uint32_t n = header(out, command);
    out[n++] = (uint8_t)((record_len >> 7) & 0x7Fu);
    out[n++] = (uint8_t)(record_len & 0x7Fu);
    n += midi_control_pack7(record, record_len, out + n);
    out[n++] = 0xF7;
    return n;
}

uint32_t midi_control_build_result(uint8_t *out, uint8_t for_command, uint8_t status)
{
    uint32_t n = header(out, MIDI_CONTROL_RPL_RESULT);
    out[n++] = for_command & 0x7Fu;
    out[n++] = status & 0x7Fu;
    out[n++] = 0xF7;
    return n;
}

uint32_t midi_control_build_identity(uint8_t *out, const struct midi_control_identity *id)
{
    uint32_t n = header(out, MIDI_CONTROL_RPL_IDENTITY);
    out[n++] = id->protocol_version & 0x7Fu;
    out[n++] = id->fw_major & 0x7Fu;
    out[n++] = id->fw_minor & 0x7Fu;
    out[n++] = id->fw_patch & 0x7Fu;
    out[n++] = id->platform & 0x7Fu;
    out[n++] = id->storage_state & 0x7Fu;
    out[n++] = id->dirty ? 1u : 0u;
    out[n++] = id->config_version & 0x7Fu;
    out[n++] = (uint8_t)((id->config_bytes >> 7) & 0x7Fu);
    out[n++] = (uint8_t)(id->config_bytes & 0x7Fu);
    for (unsigned i = 0; i < 16u; i++) {
        unsigned char c = (unsigned char)id->name[i];
        if (c == 0)
            break;
        out[n++] = (uint8_t)(c & 0x7Fu);
    }
    out[n++] = 0xF7;
    return n;
}

uint32_t midi_control_build_status(uint8_t *out, const struct midi_control_status_reply *st)
{
    uint32_t n = header(out, MIDI_CONTROL_RPL_STATUS);
    out[n++] = st->storage_state & 0x7Fu;
    out[n++] = st->dirty ? 1u : 0u;
    out[n++] = st->program & 0x7Fu;
    out[n++] = (uint8_t)((st->sequence >> 21) & 0x7Fu);
    out[n++] = (uint8_t)((st->sequence >> 14) & 0x7Fu);
    out[n++] = (uint8_t)((st->sequence >> 7) & 0x7Fu);
    out[n++] = (uint8_t)(st->sequence & 0x7Fu);
    out[n++] = 0xF7;
    return n;
}

uint32_t midi_control_build_text(uint8_t *out, uint8_t line, const char *text)
{
    uint32_t n = header(out, MIDI_CONTROL_CMD_SET_TEXT);
    out[n++] = line & 0x7Fu;
    for (unsigned i = 0; text[i] && i < 32u; i++)
        out[n++] = (uint8_t)((unsigned char)text[i] & 0x7Fu);
    out[n++] = 0xF7;
    return n;
}

/* Flags as bits, seven to a byte: flag i is bit i % 7 of byte i / 7. */
#define FLAG_BYTES(count) (((count) + 6u) / 7u)

static uint32_t put_flags(uint8_t *out, const uint8_t *flags, unsigned count)
{
    for (unsigned k = 0; k < FLAG_BYTES(count); k++)
        out[k] = 0;
    for (unsigned i = 0; i < count; i++)
        if (flags[i])
            out[i / 7u] |= (uint8_t)(1u << (i % 7u));
    return FLAG_BYTES(count);
}

static uint32_t get_flags(const uint8_t *in, uint8_t *flags, unsigned count)
{
    for (unsigned i = 0; i < count; i++)
        flags[i] = (uint8_t)((in[i / 7u] >> (i % 7u)) & 1u);
    return FLAG_BYTES(count);
}

static uint32_t put_values(uint8_t *out, const uint8_t *values, unsigned count)
{
    for (unsigned i = 0; i < count; i++)
        out[i] = values[i] & 0x7Fu;
    return count;
}

static uint32_t get_values(const uint8_t *in, uint8_t *values, unsigned count)
{
    for (unsigned i = 0; i < count; i++)
        values[i] = in[i];
    return count;
}

/* A raw count as two 7-bit bytes, high first. */
static uint32_t put_raw(uint8_t *out, uint16_t v)
{
    if (v > MIDI_CONTROL_STATE_RAW_MAX)
        v = MIDI_CONTROL_STATE_RAW_MAX;
    out[0] = (uint8_t)((v >> 7) & 0x7Fu);
    out[1] = (uint8_t)(v & 0x7Fu);
    return 2u;
}

static uint16_t get_raw(const uint8_t *in)
{
    return (uint16_t)(((unsigned)(in[0] & 0x7Fu) << 7) | (in[1] & 0x7Fu));
}

/* What protocol 5 adds to the end of STATE. */
#define STATE_V5_BYTES 16u

/* Everything in a STATE payload before the two text lines. */
#define STATE_FIXED_BYTES (3u + 2u * FLAG_BYTES(MIDI_CONTROL_BUTTON_COUNT) + MIDI_CONTROL_BUTTON_COUNT \
                           + 2u * MIDI_CONTROL_ENCODER_COUNT + MIDI_CONTROL_ANALOG_COUNT)

uint32_t midi_control_build_state(uint8_t *out, const struct midi_control_state_reply *st)
{
    uint32_t n = header(out, MIDI_CONTROL_RPL_STATE);
    out[n++] = st->storage_state & 0x7Fu;
    out[n++] = st->dirty ? 1u : 0u;
    out[n++] = st->program & 0x7Fu;
    n += put_flags(out + n, st->held, MIDI_CONTROL_BUTTON_COUNT);
    n += put_flags(out + n, st->toggled, MIDI_CONTROL_BUTTON_COUNT);
    n += put_values(out + n, st->presses, MIDI_CONTROL_BUTTON_COUNT);
    n += put_values(out + n, st->enc_value, MIDI_CONTROL_ENCODER_COUNT);
    n += put_values(out + n, st->enc_turns, MIDI_CONTROL_ENCODER_COUNT);
    n += put_values(out + n, st->analog, MIDI_CONTROL_ANALOG_COUNT);
    for (unsigned line = 0; line < 2u; line++) {
        const char *t = st->text[line];
        unsigned len = 0;
        while (len < 32u && t[len])
            len++;
        out[n++] = (uint8_t)len;
        for (unsigned i = 0; i < len; i++)
            out[n++] = (uint8_t)((unsigned char)t[i] & 0x7Fu);
    }
    /* After the lines, where a protocol 2 reader has stopped reading. */
    out[n++] = st->display_off ? 1u : 0u;
    /* Protocol 4: which profile is live, and whether its picker is on screen. */
    out[n++] = st->profile_active & 0x7Fu;
    out[n++] = st->profile_picking ? 1u : 0u;
    out[n++] = st->profile_page & 0x7Fu;
    /* Protocol 5: the calibration, and what the pedal reads. */
    out[n++] = st->cal_stage & 0x7Fu;
    out[n++] = st->cal_analog & 0x7Fu;
    out[n++] = st->cal_progress & 0x7Fu;
    n += put_raw(out + n, st->cal_raw);
    n += put_raw(out + n, st->cal_first);
    n += put_raw(out + n, st->cal_second);
    out[n++] = st->pedal_flags & 0x7Fu;
    n += put_raw(out + n, st->pedal_raw);
    n += put_raw(out + n, st->pedal_lo);
    n += put_raw(out + n, st->pedal_hi);
    /* Protocol 6: how far the Program Change buttons have been moved. */
    out[n++] = st->program_offset & 0x7Fu;
    out[n++] = 0xF7;
    return n;
}

uint32_t midi_control_build_calibrate(uint8_t *out, uint8_t analog, int start)
{
    uint32_t n = header(out, MIDI_CONTROL_CMD_CALIBRATE);
    out[n++] = analog & 0x7Fu;
    out[n++] = start ? 1u : 0u;
    out[n++] = 0xF7;
    return n;
}

uint32_t midi_control_build_event(uint8_t *out, uint8_t kind, uint8_t id, int value)
{
    int lo = kind == MIDI_CONTROL_EVENT_ENCODER ? -64 : 0;
    int hi = kind == MIDI_CONTROL_EVENT_ENCODER ? 63 : 127;
    if (value < lo)
        value = lo;
    if (value > hi)
        value = hi;

    uint32_t n = header(out, MIDI_CONTROL_CMD_EVENT);
    out[n++] = kind & 0x7Fu;
    out[n++] = id & 0x7Fu;
    out[n++] = (uint8_t)(value & 0x7F);          /* an encoder's detents in 7-bit two's complement */
    out[n++] = 0xF7;
    return n;
}

/* ---- parsing --------------------------------------------------------------- */

int midi_control_parse_message(const uint8_t *sysex, uint32_t len, struct midi_control_message *msg)
{
    if (len < MIDI_CONTROL_SYSEX_HEADER + 1u)
        return 0;
    if (sysex[0] != 0xF0 || sysex[len - 1u] != 0xF7)
        return 0;
    if (sysex[1] != MIDI_CONTROL_SYSEX_MFR || sysex[2] != MIDI_CONTROL_SYSEX_ID0 || sysex[3] != MIDI_CONTROL_SYSEX_ID1)
        return 0;
    msg->command = sysex[4];
    msg->payload = sysex + MIDI_CONTROL_SYSEX_HEADER;
    msg->payload_len = len - MIDI_CONTROL_SYSEX_HEADER - 1u;
    return 1;
}

uint32_t midi_control_parse_config_payload(const struct midi_control_message *msg, uint8_t *out, uint32_t max_out)
{
    if (msg->payload_len < 2u)
        return 0;
    uint32_t declared = ((uint32_t)msg->payload[0] << 7) | msg->payload[1];
    uint32_t got = midi_control_unpack7(msg->payload + 2, msg->payload_len - 2u, out, max_out);
    if (got == 0 || got != declared)
        return 0;
    return got;
}

int midi_control_parse_identity(const struct midi_control_message *msg, struct midi_control_identity *id)
{
    if (msg->command != MIDI_CONTROL_RPL_IDENTITY || msg->payload_len < 10u)
        return 0;
    const uint8_t *p = msg->payload;
    memset(id, 0, sizeof(*id));
    id->protocol_version = p[0];
    id->fw_major = p[1];
    id->fw_minor = p[2];
    id->fw_patch = p[3];
    id->platform = p[4];
    id->storage_state = p[5];
    id->dirty = p[6];
    id->config_version = p[7];
    id->config_bytes = (uint16_t)(((uint16_t)p[8] << 7) | p[9]);
    uint32_t n = msg->payload_len - 10u;
    if (n > 16u)
        n = 16u;
    for (uint32_t i = 0; i < n; i++)
        id->name[i] = (char)p[10 + i];
    id->name[n] = 0;
    return 1;
}

int midi_control_parse_status(const struct midi_control_message *msg, struct midi_control_status_reply *st)
{
    if (msg->command != MIDI_CONTROL_RPL_STATUS || msg->payload_len < 7u)
        return 0;
    const uint8_t *p = msg->payload;
    st->storage_state = p[0];
    st->dirty = p[1];
    st->program = p[2];
    st->sequence = ((uint32_t)p[3] << 21) | ((uint32_t)p[4] << 14)
                 | ((uint32_t)p[5] << 7) | p[6];
    return 1;
}

int midi_control_parse_result(const struct midi_control_message *msg, uint8_t *for_command, uint8_t *status)
{
    if (msg->command != MIDI_CONTROL_RPL_RESULT || msg->payload_len < 2u)
        return 0;
    *for_command = msg->payload[0];
    *status = msg->payload[1];
    return 1;
}

int midi_control_parse_state(const struct midi_control_message *msg, struct midi_control_state_reply *st)
{
    if (msg->command != MIDI_CONTROL_RPL_STATE || msg->payload_len < STATE_FIXED_BYTES + 2u)
        return 0;
    const uint8_t *p = msg->payload;
    uint32_t n = 0;
    memset(st, 0, sizeof(*st));
    st->storage_state = p[n++];
    st->dirty = p[n++];
    st->program = p[n++];
    n += get_flags(p + n, st->held, MIDI_CONTROL_BUTTON_COUNT);
    n += get_flags(p + n, st->toggled, MIDI_CONTROL_BUTTON_COUNT);
    n += get_values(p + n, st->presses, MIDI_CONTROL_BUTTON_COUNT);
    n += get_values(p + n, st->enc_value, MIDI_CONTROL_ENCODER_COUNT);
    n += get_values(p + n, st->enc_turns, MIDI_CONTROL_ENCODER_COUNT);
    n += get_values(p + n, st->analog, MIDI_CONTROL_ANALOG_COUNT);
    for (unsigned line = 0; line < 2u; line++) {
        if (n >= msg->payload_len)
            return 0;
        uint32_t len = p[n++];
        if (len > 32u || n + len > msg->payload_len)
            return 0;
        for (uint32_t i = 0; i < len; i++)
            st->text[line][i] = (char)p[n++];
        st->text[line][len] = 0;
    }
    /* A protocol 2 device stops after the lines and a protocol 3 one after the
     * display byte. Each field a device did not send stays zero, which reads as
     * a lit screen showing profile 1 with no picker - what such a device has. */
    if (n < msg->payload_len)
        st->display_off = p[n++] ? 1u : 0u;
    if (n < msg->payload_len)
        st->profile_active = p[n++];
    if (n < msg->payload_len)
        st->profile_picking = p[n++] ? 1u : 0u;
    if (n < msg->payload_len)
        st->profile_page = p[n++];
    /* A protocol 4 device stops here: nothing is calibrating, and nothing is
     * said about the pedal - not even that it can be calibrated. */
    if (n + STATE_V5_BYTES <= msg->payload_len) {
        st->cal_stage = p[n++];
        st->cal_analog = p[n++];
        st->cal_progress = p[n++];
        st->cal_raw = get_raw(p + n);
        n += 2u;
        st->cal_first = get_raw(p + n);
        n += 2u;
        st->cal_second = get_raw(p + n);
        n += 2u;
        st->pedal_flags = p[n++];
        st->pedal_raw = get_raw(p + n);
        n += 2u;
        st->pedal_lo = get_raw(p + n);
        n += 2u;
        st->pedal_hi = get_raw(p + n);
        n += 2u;
        /* A protocol 5 device stops here, and its Program Change buttons
         * send what they are set to. */
        if (n < msg->payload_len)
            st->program_offset = p[n++];
    }
    return 1;
}

int midi_control_parse_event(const struct midi_control_message *msg, uint8_t *kind, uint8_t *id, int *value)
{
    if (msg->command != MIDI_CONTROL_CMD_EVENT || msg->payload_len < 3u)
        return 0;
    *kind = msg->payload[0];
    *id = msg->payload[1];
    *value = msg->payload[2];
    if (*kind == MIDI_CONTROL_EVENT_ENCODER && *value >= 64)
        *value -= 128;
    return 1;
}

int midi_control_parse_calibrate(const struct midi_control_message *msg, uint8_t *analog, int *start)
{
    if (msg->command != MIDI_CONTROL_CMD_CALIBRATE || msg->payload_len < 2u)
        return 0;
    *analog = msg->payload[0];
    *start = msg->payload[1];
    return 1;
}

const char *midi_control_status_name(unsigned status)
{
    static const char *const k[MIDI_CONTROL_STATUS_COUNT] = {
        "OK", "bad length", "bad magic", "bad version", "bad CRC",
        "no storage", "storage locked", "write failed", "verify failed",
        "unknown command", "invalid value",
    };
    return status < MIDI_CONTROL_STATUS_COUNT ? k[status] : "?";
}

const char *midi_control_storage_state_name(unsigned state)
{
    switch (state) {
    case MIDI_CONTROL_STORAGE_NONE:    return "none";
    case MIDI_CONTROL_STORAGE_LOCKED:  return "locked (factory image present)";
    case MIDI_CONTROL_STORAGE_READY:   return "ready";
    case MIDI_CONTROL_STORAGE_UNKNOWN: return "unknown contents";
    default:                           return "?";
    }
}
