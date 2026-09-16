#include <string.h>
#include "midi_control/midi_control_config.h"
#include "midi_control/midi_control_crc.h"
#include "midi_control/midi_control_protocol.h"
#include "midi_control/midi_control_version.h"

/* Byte layout - see docs/05-config-model.md. Every offset here is part of the
 * format and changing one means a new MIDI_CONTROL_CONFIG_VERSION. */
#define OFF_MAGIC     0u
#define OFF_VERSION   4u
#define OFF_LENGTH    6u
#define OFF_SEQUENCE  8u
#define OFF_CRC       12u
#define OFF_GLOBAL    16u
#define OFF_BUTTONS   48u
#define OFF_ENCODERS  (OFF_BUTTONS + 8u * MIDI_CONTROL_BUTTON_COUNT)     /* 208 */
#define OFF_ANALOGS   (OFF_ENCODERS + 8u * MIDI_CONTROL_ENCODER_COUNT)   /* 264 */
#define OFF_END       (OFF_ANALOGS + 8u * MIDI_CONTROL_ANALOG_COUNT)     /* 280 */

typedef char midi_control_config_layout_check[(OFF_END == MIDI_CONTROL_CONFIG_BYTES) ? 1 : -1];

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* ---- defaults ------------------------------------------------------------ */

static void button(struct midi_control_button_map *b, uint8_t action, uint8_t number,
                   uint8_t led_on)
{
    memset(b, 0, sizeof(*b));
    b->action = action;
    b->channel = MIDI_CONTROL_CHANNEL_GLOBAL;
    b->number = number;
    b->value_on = 127;
    b->value_off = 0;
    b->led_on = led_on;
    b->led_off = MIDI_CONTROL_LED_OFF;
}

void midi_control_config_defaults(struct midi_control_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->sequence = 0;
    cfg->global.midi_channel = 0;
    cfg->global.bank_size = 4;
    cfg->global.bank_select = 1;
    cfg->global.follow_program_change = 1;
    cfg->global.startup_program = 0;
    cfg->global.led_brightness = 255;
    cfg->global.display_timeout = 0;            /* never */
    strncpy(cfg->global.name, MIDI_CONTROL_PRODUCT_NAME, MIDI_CONTROL_CONFIG_NAME_LEN);

    /* The verified mapping of the reference controller: footswitches A-D are
     * notes 60-63, Tap is 64, the top-panel buttons follow from 65, the encoder
     * pushes from 69, and the two non-matrix switches are 76 and 77. Bank
     * Up/Down step the program by a bank. Footswitch LEDs light white while
     * held; Tap lights its own lamp. */
    button(&cfg->buttons[MIDI_CONTROL_BTN_FS_A],      MIDI_CONTROL_ACT_NOTE_MOMENTARY, 60, MIDI_CONTROL_LED_WHITE);
    button(&cfg->buttons[MIDI_CONTROL_BTN_FS_B],      MIDI_CONTROL_ACT_NOTE_MOMENTARY, 61, MIDI_CONTROL_LED_WHITE);
    button(&cfg->buttons[MIDI_CONTROL_BTN_FS_C],      MIDI_CONTROL_ACT_NOTE_MOMENTARY, 62, MIDI_CONTROL_LED_WHITE);
    button(&cfg->buttons[MIDI_CONTROL_BTN_FS_D],      MIDI_CONTROL_ACT_NOTE_MOMENTARY, 63, MIDI_CONTROL_LED_WHITE);
    button(&cfg->buttons[MIDI_CONTROL_BTN_TAP],       MIDI_CONTROL_ACT_NOTE_MOMENTARY, 64, MIDI_CONTROL_LAMP_TAP);
    button(&cfg->buttons[MIDI_CONTROL_BTN_BANK_UP],   MIDI_CONTROL_ACT_BANK_UP,        0,  MIDI_CONTROL_LED_WHITE);
    button(&cfg->buttons[MIDI_CONTROL_BTN_BANK_DOWN], MIDI_CONTROL_ACT_BANK_DOWN,      0,  MIDI_CONTROL_LED_WHITE);
    button(&cfg->buttons[MIDI_CONTROL_BTN_VIEW],      MIDI_CONTROL_ACT_NOTE_MOMENTARY, 65, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_BYPASS],    MIDI_CONTROL_ACT_NOTE_MOMENTARY, 66, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_MENU],      MIDI_CONTROL_ACT_NOTE_MOMENTARY, 67, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_MODE],      MIDI_CONTROL_ACT_NOTE_MOMENTARY, 68, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_HOME_ENC],  MIDI_CONTROL_ACT_NOTE_MOMENTARY, 69, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_MENU_ENC],  MIDI_CONTROL_ACT_NOTE_MOMENTARY, 70, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_ENC_1],     MIDI_CONTROL_ACT_NOTE_MOMENTARY, 71, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_ENC_2],     MIDI_CONTROL_ACT_NOTE_MOMENTARY, 72, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_ENC_3],     MIDI_CONTROL_ACT_NOTE_MOMENTARY, 73, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_ENC_4],     MIDI_CONTROL_ACT_NOTE_MOMENTARY, 74, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_ENC_5],     MIDI_CONTROL_ACT_NOTE_MOMENTARY, 75, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_TOE],       MIDI_CONTROL_ACT_NOTE_MOMENTARY, 76, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_PAGE],      MIDI_CONTROL_ACT_NOTE_MOMENTARY, 77, 0);

    /* Encoders: relative CC#16-22, one message per detent. */
    for (unsigned i = 0; i < MIDI_CONTROL_ENCODER_COUNT; i++) {
        struct midi_control_encoder_map *e = &cfg->encoders[i];
        e->mode = MIDI_CONTROL_ENCM_CC_RELATIVE_TWOS;
        e->channel = MIDI_CONTROL_CHANNEL_GLOBAL;
        e->number = (uint8_t)(16 + i);
        e->step = 1;
        e->value = 64;
    }

    /* Volume knob CC#7, expression pedal CC#11 with toe-down high. The
     * pedal's sensor voltage falls toe-down, so the firmware's raw reading is
     * inverted before it reaches the engine; `invert` here is the user's
     * choice on top of that. */
    cfg->analogs[MIDI_CONTROL_ANALOG_VOLUME].mode = MIDI_CONTROL_ANM_CC;
    cfg->analogs[MIDI_CONTROL_ANALOG_VOLUME].number = 7;
    cfg->analogs[MIDI_CONTROL_ANALOG_VOLUME].out_max = 127;
    cfg->analogs[MIDI_CONTROL_ANALOG_PEDAL].mode = MIDI_CONTROL_ANM_CC;
    cfg->analogs[MIDI_CONTROL_ANALOG_PEDAL].number = 11;
    cfg->analogs[MIDI_CONTROL_ANALOG_PEDAL].out_max = 127;
}

/* ---- serialisation ------------------------------------------------------- */

void midi_control_config_write(const struct midi_control_config *cfg, uint8_t *out)
{
    memset(out, 0, MIDI_CONTROL_CONFIG_BYTES);

    put32(out + OFF_MAGIC, MIDI_CONTROL_CONFIG_MAGIC);
    put16(out + OFF_VERSION, (uint16_t)MIDI_CONTROL_CONFIG_VERSION);
    put16(out + OFF_LENGTH, (uint16_t)MIDI_CONTROL_CONFIG_BYTES);
    put32(out + OFF_SEQUENCE, cfg->sequence);

    uint8_t *g = out + OFF_GLOBAL;
    g[0] = cfg->global.midi_channel;
    g[1] = cfg->global.bank_size;
    g[2] = cfg->global.bank_select;
    g[3] = cfg->global.follow_program_change;
    g[4] = cfg->global.startup_program;
    g[5] = cfg->global.led_brightness;
    put16(g + 6, cfg->global.display_timeout);
    memcpy(g + 8, cfg->global.name, MIDI_CONTROL_CONFIG_NAME_LEN);

    for (unsigned i = 0; i < MIDI_CONTROL_BUTTON_COUNT; i++) {
        const struct midi_control_button_map *b = &cfg->buttons[i];
        uint8_t *p = out + OFF_BUTTONS + 8u * i;
        p[0] = b->action;
        p[1] = b->channel;
        p[2] = b->number;
        p[3] = b->value_on;
        p[4] = b->value_off;
        p[5] = b->led_on;
        p[6] = b->led_off;
    }
    for (unsigned i = 0; i < MIDI_CONTROL_ENCODER_COUNT; i++) {
        const struct midi_control_encoder_map *e = &cfg->encoders[i];
        uint8_t *p = out + OFF_ENCODERS + 8u * i;
        p[0] = e->mode;
        p[1] = e->channel;
        p[2] = e->number;
        p[3] = e->step;
        p[4] = e->value;
    }
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++) {
        const struct midi_control_analog_map *a = &cfg->analogs[i];
        uint8_t *p = out + OFF_ANALOGS + 8u * i;
        p[0] = a->mode;
        p[1] = a->channel;
        p[2] = a->number;
        p[3] = a->invert;
        p[4] = a->out_min;
        p[5] = a->out_max;
    }

    put32(out + OFF_CRC, midi_control_crc32(out + OFF_GLOBAL, MIDI_CONTROL_CONFIG_BYTES - OFF_GLOBAL));
}

int midi_control_config_read(struct midi_control_config *cfg, const uint8_t *in, uint32_t len)
{
    if (len < MIDI_CONTROL_CONFIG_BYTES)
        return MIDI_CONTROL_STATUS_BAD_LENGTH;
    if (get32(in + OFF_MAGIC) != MIDI_CONTROL_CONFIG_MAGIC)
        return MIDI_CONTROL_STATUS_BAD_MAGIC;
    if (get16(in + OFF_VERSION) != MIDI_CONTROL_CONFIG_VERSION)
        return MIDI_CONTROL_STATUS_BAD_VERSION;
    if (get16(in + OFF_LENGTH) != MIDI_CONTROL_CONFIG_BYTES)
        return MIDI_CONTROL_STATUS_BAD_LENGTH;
    if (get32(in + OFF_CRC) != midi_control_crc32(in + OFF_GLOBAL, MIDI_CONTROL_CONFIG_BYTES - OFF_GLOBAL))
        return MIDI_CONTROL_STATUS_BAD_CRC;

    struct midi_control_config c;
    memset(&c, 0, sizeof(c));
    c.sequence = get32(in + OFF_SEQUENCE);

    const uint8_t *g = in + OFF_GLOBAL;
    c.global.midi_channel = g[0];
    c.global.bank_size = g[1];
    c.global.bank_select = g[2];
    c.global.follow_program_change = g[3];
    c.global.startup_program = g[4];
    c.global.led_brightness = g[5];
    c.global.display_timeout = get16(g + 6);    /* reserved, so zero, in records written before it */
    memcpy(c.global.name, g + 8, MIDI_CONTROL_CONFIG_NAME_LEN);

    for (unsigned i = 0; i < MIDI_CONTROL_BUTTON_COUNT; i++) {
        const uint8_t *p = in + OFF_BUTTONS + 8u * i;
        struct midi_control_button_map *b = &c.buttons[i];
        b->action = p[0];
        b->channel = p[1];
        b->number = p[2];
        b->value_on = p[3];
        b->value_off = p[4];
        b->led_on = p[5];
        b->led_off = p[6];
    }
    for (unsigned i = 0; i < MIDI_CONTROL_ENCODER_COUNT; i++) {
        const uint8_t *p = in + OFF_ENCODERS + 8u * i;
        struct midi_control_encoder_map *e = &c.encoders[i];
        e->mode = p[0];
        e->channel = p[1];
        e->number = p[2];
        e->step = p[3];
        e->value = p[4];
    }
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++) {
        const uint8_t *p = in + OFF_ANALOGS + 8u * i;
        struct midi_control_analog_map *a = &c.analogs[i];
        a->mode = p[0];
        a->channel = p[1];
        a->number = p[2];
        a->invert = p[3];
        a->out_min = p[4];
        a->out_max = p[5];
    }

    midi_control_config_sanitise(&c);
    *cfg = c;
    return MIDI_CONTROL_STATUS_OK;
}

static uint8_t clamp7(uint8_t v)
{
    return v > 127u ? 127u : v;
}

void midi_control_config_sanitise(struct midi_control_config *cfg)
{
    struct midi_control_global *g = &cfg->global;

    if (g->midi_channel > 15u)
        g->midi_channel = 15u;
    if (g->bank_size < 1u)
        g->bank_size = 1u;
    if (g->bank_size > 16u)
        g->bank_size = 16u;
    g->bank_select = g->bank_select ? 1u : 0u;
    g->follow_program_change = g->follow_program_change ? 1u : 0u;
    g->startup_program = clamp7(g->startup_program);
    for (unsigned i = 0; i < MIDI_CONTROL_CONFIG_NAME_LEN; i++) {
        unsigned char ch = (unsigned char)g->name[i];
        if (ch == 0)
            break;
        if (ch < 0x20u || ch > 0x7Eu)
            g->name[i] = '?';
    }
    g->name[MIDI_CONTROL_CONFIG_NAME_LEN - 1u] = 0;
    if (g->display_timeout > MIDI_CONTROL_DISPLAY_TIMEOUT_MAX)
        g->display_timeout = (uint16_t)MIDI_CONTROL_DISPLAY_TIMEOUT_MAX;
    memset(g->reserved1, 0, sizeof(g->reserved1));

    for (unsigned i = 0; i < MIDI_CONTROL_BUTTON_COUNT; i++) {
        struct midi_control_button_map *b = &cfg->buttons[i];
        if (b->action >= MIDI_CONTROL_BUTTON_ACTION_COUNT)
            b->action = MIDI_CONTROL_ACT_NONE;
        if (b->channel > 16u)
            b->channel = 0u;
        b->number = clamp7(b->number);
        b->value_on = clamp7(b->value_on);
        b->value_off = clamp7(b->value_off);
        b->led_on &= 7u;
        b->led_off &= 7u;
        b->reserved = 0;
    }
    for (unsigned i = 0; i < MIDI_CONTROL_ENCODER_COUNT; i++) {
        struct midi_control_encoder_map *e = &cfg->encoders[i];
        if (e->mode >= MIDI_CONTROL_ENCODER_MODE_COUNT)
            e->mode = MIDI_CONTROL_ENCM_NONE;
        if (e->channel > 16u)
            e->channel = 0u;
        e->number = clamp7(e->number);
        if (e->step < 1u)
            e->step = 1u;
        if (e->step > 64u)
            e->step = 64u;
        e->value = clamp7(e->value);
        memset(e->reserved, 0, sizeof(e->reserved));
    }
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++) {
        struct midi_control_analog_map *a = &cfg->analogs[i];
        if (a->mode >= MIDI_CONTROL_ANALOG_MODE_COUNT)
            a->mode = MIDI_CONTROL_ANM_NONE;
        if (a->channel > 16u)
            a->channel = 0u;
        a->number = clamp7(a->number);
        a->invert = a->invert ? 1u : 0u;
        a->out_min = clamp7(a->out_min);
        a->out_max = clamp7(a->out_max);
        memset(a->reserved, 0, sizeof(a->reserved));
    }
}

int midi_control_config_equal(const struct midi_control_config *a, const struct midi_control_config *b)
{
    uint8_t x[MIDI_CONTROL_CONFIG_BYTES], y[MIDI_CONTROL_CONFIG_BYTES];

    midi_control_config_write(a, x);
    midi_control_config_write(b, y);
    /* Everything after the header except the sequence; the CRC covers exactly
     * that, so comparing the bodies is comparing what matters. */
    return memcmp(x + OFF_GLOBAL, y + OFF_GLOBAL, MIDI_CONTROL_CONFIG_BYTES - OFF_GLOBAL) == 0;
}

/* ---- names --------------------------------------------------------------- */

const char *midi_control_button_action_name(unsigned action)
{
    static const char *const k[MIDI_CONTROL_BUTTON_ACTION_COUNT] = {
        "None", "Note (momentary)", "Note (toggle)", "CC (momentary)",
        "CC (toggle)", "Program Change", "Program up", "Program down",
        "Bank up", "Bank down", "Preset in bank",
    };
    return action < MIDI_CONTROL_BUTTON_ACTION_COUNT ? k[action] : "?";
}

const char *midi_control_encoder_mode_name(unsigned mode)
{
    static const char *const k[MIDI_CONTROL_ENCODER_MODE_COUNT] = {
        "None", "CC relative (1 / 127)", "CC relative (65 / 63)",
        "CC absolute", "Program up / down",
    };
    return mode < MIDI_CONTROL_ENCODER_MODE_COUNT ? k[mode] : "?";
}

const char *midi_control_analog_mode_name(unsigned mode)
{
    static const char *const k[MIDI_CONTROL_ANALOG_MODE_COUNT] = {
        "None", "CC", "Pitch bend", "Channel pressure",
    };
    return mode < MIDI_CONTROL_ANALOG_MODE_COUNT ? k[mode] : "?";
}
