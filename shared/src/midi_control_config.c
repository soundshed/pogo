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
#define OFF_CAL       (OFF_ANALOGS + 8u * MIDI_CONTROL_ANALOG_COUNT)     /* 280 */
#define OFF_PROFILES  (OFF_CAL + 8u * MIDI_CONTROL_ANALOG_COUNT)         /* 296: count, active, reserved[6] */
#define OFF_PROFILE_0 (OFF_PROFILES + 8u)                                /* 304 */

/* One profile entry, and the offsets inside it. */
#define PROF_NAME     0u
#define PROF_BUTTONS  24u                                                /* 16 name + 8 reserved */
#define PROF_ENCODERS (PROF_BUTTONS + 8u * MIDI_CONTROL_BUTTON_COUNT)    /* 184 */
#define PROF_ANALOGS  (PROF_ENCODERS + 8u * MIDI_CONTROL_ENCODER_COUNT)  /* 240 */
#define PROF_BYTES    256u

#define OFF_NAMES     (OFF_PROFILE_0 + PROF_BYTES * MIDI_CONTROL_PROFILE_COUNT)   /* 2352 */
#define OFF_HOLDS     (OFF_NAMES + MIDI_CONTROL_PROGRAM_NAME_LEN * MIDI_CONTROL_PROGRAM_COUNT)   /* 4400 */
#define HOLDS_BYTES   (4u * MIDI_CONTROL_BUTTON_COUNT)                   /* 80: one block of hold actions */
#define OFF_PROFILE_HOLDS (OFF_HOLDS + HOLDS_BYTES)                      /* 4480: profile 0's, then 1's... */
#define OFF_END       (OFF_PROFILE_HOLDS + HOLDS_BYTES * MIDI_CONTROL_PROFILE_COUNT)   /* 5120 */

typedef char midi_control_config_layout_check[(OFF_END == MIDI_CONTROL_CONFIG_BYTES) ? 1 : -1];
typedef char midi_control_profile_layout_check[
    (PROF_ANALOGS + 8u * MIDI_CONTROL_ANALOG_COUNT == PROF_BYTES) ? 1 : -1];
typedef char midi_control_config_v1_check[(OFF_CAL == MIDI_CONTROL_CONFIG_BYTES_V1) ? 1 : -1];
typedef char midi_control_config_v2_check[(OFF_PROFILES == MIDI_CONTROL_CONFIG_BYTES_V2) ? 1 : -1];
typedef char midi_control_config_v3_check[(OFF_NAMES == MIDI_CONTROL_CONFIG_BYTES_V3) ? 1 : -1];
typedef char midi_control_config_v4_check[(OFF_HOLDS == MIDI_CONTROL_CONFIG_BYTES_V4) ? 1 : -1];
/* The two slots are whole sectors of the eMMC, and the record has to fit the
 * sixteen that docs/06 reserves for each. */
typedef char midi_control_config_slot_check[(MIDI_CONTROL_CONFIG_BYTES <= 16u * 512u) ? 1 : -1];
/* The CONFIG message carries the record 8-to-7 packed, and refuses one longer
 * than it is built for. */
typedef char midi_control_sysex_fits_check[
    (MIDI_CONTROL_CONFIG_BYTES <= MIDI_CONTROL_CONFIG_MAX_BYTES) ? 1 : -1];

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

/* ---- the three mapping blocks, shared by the live copy and every profile --- */
/* Each pair is one block of the layout: the same bytes at OFF_BUTTONS and at
 * PROF_BUTTONS inside a profile entry, so the mapping is written exactly once
 * however many copies of it the record carries. */

static void put_buttons(uint8_t *p, const struct midi_control_button_map *in)
{
    for (unsigned i = 0; i < MIDI_CONTROL_BUTTON_COUNT; i++, p += 8u) {
        p[0] = in[i].action;
        p[1] = in[i].channel;
        p[2] = in[i].number;
        p[3] = in[i].value_on;
        p[4] = in[i].value_off;
        p[5] = in[i].led_on;
        p[6] = in[i].led_off;
        p[7] = in[i].flags;
    }
}

static void get_buttons(const uint8_t *p, struct midi_control_button_map *out)
{
    for (unsigned i = 0; i < MIDI_CONTROL_BUTTON_COUNT; i++, p += 8u) {
        out[i].action = p[0];
        out[i].channel = p[1];
        out[i].number = p[2];
        out[i].value_on = p[3];
        out[i].value_off = p[4];
        out[i].led_on = p[5];
        out[i].led_off = p[6];
        out[i].flags = p[7];       /* reserved, so zero, in records written before it */
    }
}

static void put_encoders(uint8_t *p, const struct midi_control_encoder_map *in)
{
    for (unsigned i = 0; i < MIDI_CONTROL_ENCODER_COUNT; i++, p += 8u) {
        p[0] = in[i].mode;
        p[1] = in[i].channel;
        p[2] = in[i].number;
        p[3] = in[i].step;
        p[4] = in[i].value;
    }
}

static void get_encoders(const uint8_t *p, struct midi_control_encoder_map *out)
{
    for (unsigned i = 0; i < MIDI_CONTROL_ENCODER_COUNT; i++, p += 8u) {
        out[i].mode = p[0];
        out[i].channel = p[1];
        out[i].number = p[2];
        out[i].step = p[3];
        out[i].value = p[4];
    }
}

static void put_analogs(uint8_t *p, const struct midi_control_analog_map *in)
{
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++, p += 8u) {
        p[0] = in[i].mode;
        p[1] = in[i].channel;
        p[2] = in[i].number;
        p[3] = in[i].invert;
        p[4] = in[i].out_min;
        p[5] = in[i].out_max;
        p[6] = in[i].pad_lo;
        p[7] = in[i].pad_hi;
    }
}

static void get_analogs(const uint8_t *p, struct midi_control_analog_map *out)
{
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++, p += 8u) {
        out[i].mode = p[0];
        out[i].channel = p[1];
        out[i].number = p[2];
        out[i].invert = p[3];
        out[i].out_min = p[4];
        out[i].out_max = p[5];
        out[i].pad_lo = p[6];      /* reserved, so zero, in records written before it */
        out[i].pad_hi = p[7];
    }
}

/* The hold actions: four bytes a switch, after everything else, because the
 * profile entries had no room left for them. */
static void put_holds(uint8_t *p, const struct midi_control_hold_map *in)
{
    for (unsigned i = 0; i < MIDI_CONTROL_BUTTON_COUNT; i++, p += 4u) {
        p[0] = in[i].action;
        p[1] = in[i].number;
        p[2] = in[i].value_on;
        p[3] = in[i].value_off;
    }
}

static void get_holds(const uint8_t *p, struct midi_control_hold_map *out)
{
    for (unsigned i = 0; i < MIDI_CONTROL_BUTTON_COUNT; i++, p += 4u) {
        out[i].action = p[0];
        out[i].number = p[1];
        out[i].value_on = p[2];
        out[i].value_off = p[3];
    }
}

/* ---- defaults ------------------------------------------------------------ */

/* "PROFILE 1". The firmware has no printf and the count is small, but the
 * loop is written for any of them rather than for one digit. */
static void default_profile_name(char *out, unsigned index)
{
    static const char k[] = "PROFILE ";
    unsigned n = 0, value = index + 1u, scale = 1u;

    for (; k[n]; n++)
        out[n] = k[n];
    while (value / scale >= 10u)
        scale *= 10u;
    for (; scale && n + 1u < MIDI_CONTROL_PROFILE_NAME_LEN; scale /= 10u)
        out[n++] = (char)('0' + (value / scale) % 10u);
    for (; n < MIDI_CONTROL_PROFILE_NAME_LEN; n++)
        out[n] = 0;
}

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
    /* Bank Up/Down choose a bank and A-D load from it, as the POD Go's own
     * switches do: scrolling past three banks does not load three presets. */
    cfg->global.bank_wait = 1;
    cfg->global.hold_time = MIDI_CONTROL_HOLD_TIME_DEFAULT;

    /* Footswitches A-D choose the four presets of the bank, as the POD Go's
     * own do, and Bank Up/Down choose the bank; A-D's LEDs light white on the
     * preset in use, the bank switches' while held.
     *
     * Tap is tap tempo on CC 64, and held it is the tuner on CC 68 - Line 6's
     * numbers for both, on the unit's own footswitch - with the Tap lamp
     * flashing the tempo. The other switches send CCs from 102, in the range
     * MIDI leaves undefined, so nothing a host gives a meaning by default
     * answers them: CCs because effect plugins learn CCs where many ignore
     * notes. The toe switch toggles, as a wah's switch does; the panel buttons
     * are momentary, 127 down and 0 up, since they have no light to show a
     * toggle's state. The reference controller sent notes 60-77 from all of
     * these, which is what these defaults were before 0.3.0 (docs/03 section 2,
     * and upgrade_profiles() below). */
    for (unsigned i = 0; i < 4u; i++)
        button(&cfg->buttons[MIDI_CONTROL_BTN_FS_A + i], MIDI_CONTROL_ACT_PRESET_IN_BANK, (uint8_t)i,
               MIDI_CONTROL_LED_WHITE);
    button(&cfg->buttons[MIDI_CONTROL_BTN_TAP],       MIDI_CONTROL_ACT_TAP_TEMPO,      64,  MIDI_CONTROL_LAMP_TAP);
    button(&cfg->buttons[MIDI_CONTROL_BTN_BANK_UP],   MIDI_CONTROL_ACT_BANK_UP,        0,   MIDI_CONTROL_LED_WHITE);
    button(&cfg->buttons[MIDI_CONTROL_BTN_BANK_DOWN], MIDI_CONTROL_ACT_BANK_DOWN,      0,   MIDI_CONTROL_LED_WHITE);
    button(&cfg->buttons[MIDI_CONTROL_BTN_VIEW],      MIDI_CONTROL_ACT_CC_MOMENTARY,   102, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_BYPASS],    MIDI_CONTROL_ACT_CC_MOMENTARY,   103, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_MENU],      MIDI_CONTROL_ACT_CC_MOMENTARY,   104, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_MODE],      MIDI_CONTROL_ACT_CC_MOMENTARY,   105, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_HOME_ENC],  MIDI_CONTROL_ACT_CC_MOMENTARY,   106, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_MENU_ENC],  MIDI_CONTROL_ACT_CC_MOMENTARY,   107, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_ENC_1],     MIDI_CONTROL_ACT_CC_MOMENTARY,   108, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_ENC_2],     MIDI_CONTROL_ACT_CC_MOMENTARY,   109, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_ENC_3],     MIDI_CONTROL_ACT_CC_MOMENTARY,   110, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_ENC_4],     MIDI_CONTROL_ACT_CC_MOMENTARY,   111, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_ENC_5],     MIDI_CONTROL_ACT_CC_MOMENTARY,   112, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_TOE],       MIDI_CONTROL_ACT_CC_TOGGLE,      113, 0);
    button(&cfg->buttons[MIDI_CONTROL_BTN_PAGE],      MIDI_CONTROL_ACT_CC_MOMENTARY,   114, 0);
    cfg->holds[MIDI_CONTROL_BTN_TAP].action = MIDI_CONTROL_ACT_CC_TOGGLE;
    cfg->holds[MIDI_CONTROL_BTN_TAP].number = 68;
    cfg->holds[MIDI_CONTROL_BTN_TAP].value_on = 127;
    cfg->holds[MIDI_CONTROL_BTN_TAP].value_off = 0;

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
    /* Padding (docs/05 section 5): the toe's is wide, because the pedal comes
     * back off its stop as the foot leaves it and 127 has to survive that;
     * the heel's is a percent, enough that 0 does not need the last count. */
    cfg->analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_lo = 1;
    cfg->analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi = 15;

    /* No calibration: the memset above already left every analog_cal zeroed,
     * which is what "this unit has not been swept yet" is. The platform's
     * seeds carry it until it has been (docs/05 section 6). */

    /* One profile in use, holding the mapping just built; the other seven are
     * the same mapping under their own names, waiting to be edited. A fresh
     * unit therefore shows one profile in the picker rather than eight
     * identical ones, and adding a profile in the app is raising the count
     * (docs/05 section 7). */
    cfg->profile_count = 1u;
    cfg->profile_active = 0u;
    for (unsigned i = 0; i < MIDI_CONTROL_PROFILE_COUNT; i++) {
        default_profile_name(cfg->profiles[i].name, i);
        memcpy(cfg->profiles[i].buttons, cfg->buttons, sizeof(cfg->buttons));
        memcpy(cfg->profiles[i].encoders, cfg->encoders, sizeof(cfg->encoders));
        memcpy(cfg->profiles[i].analogs, cfg->analogs, sizeof(cfg->analogs));
        memcpy(cfg->profiles[i].holds, cfg->holds, sizeof(cfg->holds));
    }
}

unsigned midi_control_config_hold_ms(const struct midi_control_config *cfg)
{
    const unsigned t = cfg->global.hold_time ? cfg->global.hold_time : MIDI_CONTROL_HOLD_TIME_DEFAULT;
    return t * 10u;
}

int midi_control_button_can_hold(unsigned b)
{
    return b < MIDI_CONTROL_BUTTON_COUNT && b != MIDI_CONTROL_BTN_MODE && b != MIDI_CONTROL_BTN_TOE;
}

int midi_control_config_load_profile(struct midi_control_config *cfg, unsigned n)
{
    if (n >= cfg->profile_count || n >= MIDI_CONTROL_PROFILE_COUNT)
        return 0;
    cfg->profile_active = (uint8_t)n;
    memcpy(cfg->buttons, cfg->profiles[n].buttons, sizeof(cfg->buttons));
    memcpy(cfg->encoders, cfg->profiles[n].encoders, sizeof(cfg->encoders));
    memcpy(cfg->analogs, cfg->profiles[n].analogs, sizeof(cfg->analogs));
    memcpy(cfg->holds, cfg->profiles[n].holds, sizeof(cfg->holds));
    return 1;
}

int midi_control_config_select_profile(struct midi_control_config *cfg, unsigned n)
{
    if (n >= cfg->profile_count || n >= MIDI_CONTROL_PROFILE_COUNT)
        return 0;

    /* Keep whatever was done to the profile being left. The live mapping is
     * where edits land, so without this an edit made and then not saved would
     * be lost by switching away - which is not what leaving a profile means.
     * This is sanitise()'s rule applied at the one moment it matters. */
    if (cfg->profile_active < MIDI_CONTROL_PROFILE_COUNT) {
        struct midi_control_profile *was = &cfg->profiles[cfg->profile_active];
        memcpy(was->buttons, cfg->buttons, sizeof(cfg->buttons));
        memcpy(was->encoders, cfg->encoders, sizeof(cfg->encoders));
        memcpy(was->analogs, cfg->analogs, sizeof(cfg->analogs));
        memcpy(was->holds, cfg->holds, sizeof(cfg->holds));
    }
    return midi_control_config_load_profile(cfg, n);
}

const char *midi_control_config_profile_name(const struct midi_control_config *cfg, unsigned n)
{
    return n < MIDI_CONTROL_PROFILE_COUNT ? cfg->profiles[n].name : "";
}

const char *midi_control_config_program_name(const struct midi_control_config *cfg, unsigned program)
{
    return program < MIDI_CONTROL_PROGRAM_COUNT ? cfg->program_names[program] : "";
}

int midi_control_config_set_program_name(struct midi_control_config *cfg, unsigned program, const char *name)
{
    unsigned i = 0;

    if (program >= MIDI_CONTROL_PROGRAM_COUNT)
        return 0;
    char *out = cfg->program_names[program];
    if (name)
        for (; i + 1u < MIDI_CONTROL_PROGRAM_NAME_LEN && name[i]; i++)
            out[i] = name[i];
    for (; i < MIDI_CONTROL_PROGRAM_NAME_LEN; i++)
        out[i] = 0;
    return 1;
}

/* A record written before profiles existed carries one mapping and no profile
 * block, and the unit that saved it has a mapping its owner set up. That
 * mapping becomes the one profile in use, named IMPORTED when it is not simply
 * the factory one, so the first thing the picker shows after the upgrade says
 * where it came from. The remaining seven are the factory mapping under their
 * own names, waiting to be edited.
 *
 * Only the names, the count and which profile is active are set here: the
 * mapping itself arrives when midi_control_config_sanitise() copies the live
 * blocks into the active profile, which it does on every read. The comparison
 * is made before that sanitising, so a record holding values out of range
 * counts as changed - which it is. */
static void upgrade_profiles(struct midi_control_config *c)
{
    static const char k_imported[] = "IMPORTED";
    struct midi_control_config d;
    int changed;

    midi_control_config_defaults(&d);
    /* Those records had no padding - the bytes were reserved - so it is no
     * part of what their owner set up, and the mapping keeps none: the unit
     * plays as it did before the upgrade (docs/05 section 5). */
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++) {
        d.analogs[i].pad_lo = c->analogs[i].pad_lo;
        d.analogs[i].pad_hi = c->analogs[i].pad_hi;
    }
    /* And every switch but the bank pair sent notes 60-77 in the factory
     * mapping of the time - A-D 60-63, Tap 64, the rest from 65 - so a record
     * holding those is one nobody set up. It keeps them - the unit plays as
     * it did - while the seven profiles waiting to be edited have today's. */
    for (unsigned i = 0; i < 4u; i++)
        button(&d.buttons[MIDI_CONTROL_BTN_FS_A + i], MIDI_CONTROL_ACT_NOTE_MOMENTARY, (uint8_t)(60u + i),
               MIDI_CONTROL_LED_WHITE);
    button(&d.buttons[MIDI_CONTROL_BTN_TAP], MIDI_CONTROL_ACT_NOTE_MOMENTARY, 64, MIDI_CONTROL_LAMP_TAP);
    for (unsigned b = MIDI_CONTROL_BTN_VIEW; b < MIDI_CONTROL_BUTTON_COUNT; b++)
        button(&d.buttons[b], MIDI_CONTROL_ACT_NOTE_MOMENTARY, (uint8_t)(65u + b - MIDI_CONTROL_BTN_VIEW), 0);
    changed = memcmp(c->buttons, d.buttons, sizeof(c->buttons)) != 0
           || memcmp(c->encoders, d.encoders, sizeof(c->encoders)) != 0
           || memcmp(c->analogs, d.analogs, sizeof(c->analogs)) != 0;

    memcpy(c->profiles, d.profiles, sizeof(c->profiles));
    c->profile_count = 1u;
    c->profile_active = 0u;
    if (changed) {
        memset(c->profiles[0].name, 0, sizeof(c->profiles[0].name));
        memcpy(c->profiles[0].name, k_imported, sizeof(k_imported) - 1u);
    }
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
    g[24] = cfg->global.bank_wait;
    g[25] = cfg->global.hold_time;

    put_buttons(out + OFF_BUTTONS, cfg->buttons);
    put_encoders(out + OFF_ENCODERS, cfg->encoders);
    put_analogs(out + OFF_ANALOGS, cfg->analogs);

    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++) {
        const struct midi_control_analog_cal *c = &cfg->analog_cal[i];
        uint8_t *p = out + OFF_CAL + 8u * i;
        put16(p + 0, c->raw_lo);
        put16(p + 2, c->raw_hi);
        p[4] = c->flags;
    }

    /* The active profile is written from the live mapping rather than from its
     * own copy, so a record is self-consistent whatever a caller did to the
     * struct: editing cfg->buttons and serialising without sanitising first is
     * an ordinary thing to do, and it must not put a stale profile on the card
     * or on the wire. Which profile that is is clamped here for the same
     * reason - this function promises a valid record, not a faithful one. */
    const unsigned active = cfg->profile_active < MIDI_CONTROL_PROFILE_COUNT ? cfg->profile_active : 0u;

    out[OFF_PROFILES + 0u] = cfg->profile_count;
    out[OFF_PROFILES + 1u] = (uint8_t)active;
    for (unsigned i = 0; i < MIDI_CONTROL_PROFILE_COUNT; i++) {
        const struct midi_control_profile *pr = &cfg->profiles[i];
        uint8_t *p = out + OFF_PROFILE_0 + PROF_BYTES * i;
        memcpy(p + PROF_NAME, pr->name, MIDI_CONTROL_PROFILE_NAME_LEN);
        put_buttons(p + PROF_BUTTONS, i == active ? cfg->buttons : pr->buttons);
        put_encoders(p + PROF_ENCODERS, i == active ? cfg->encoders : pr->encoders);
        put_analogs(p + PROF_ANALOGS, i == active ? cfg->analogs : pr->analogs);
    }

    memcpy(out + OFF_NAMES, cfg->program_names, sizeof(cfg->program_names));

    put_holds(out + OFF_HOLDS, cfg->holds);
    for (unsigned i = 0; i < MIDI_CONTROL_PROFILE_COUNT; i++)
        put_holds(out + OFF_PROFILE_HOLDS + HOLDS_BYTES * i, i == active ? cfg->holds : cfg->profiles[i].holds);

    put32(out + OFF_CRC, midi_control_crc32(out + OFF_GLOBAL, MIDI_CONTROL_CONFIG_BYTES - OFF_GLOBAL));
}

int midi_control_config_read(struct midi_control_config *cfg, const uint8_t *in, uint32_t len)
{
    uint32_t bytes;

    /* Enough for the header, whatever the body turns out to be. */
    if (len < OFF_GLOBAL)
        return MIDI_CONTROL_STATUS_BAD_LENGTH;
    if (get32(in + OFF_MAGIC) != MIDI_CONTROL_CONFIG_MAGIC)
        return MIDI_CONTROL_STATUS_BAD_MAGIC;

    /* Versions 1 to 4 are still read: a unit that has saved before this
     * change has one on its card, and rejecting it would throw the user's
     * mapping away to gain blocks the unit can fill in itself. */
    switch (get16(in + OFF_VERSION)) {
    case MIDI_CONTROL_CONFIG_VERSION:    bytes = MIDI_CONTROL_CONFIG_BYTES; break;
    case MIDI_CONTROL_CONFIG_VERSION_V4: bytes = MIDI_CONTROL_CONFIG_BYTES_V4; break;
    case MIDI_CONTROL_CONFIG_VERSION_V3: bytes = MIDI_CONTROL_CONFIG_BYTES_V3; break;
    case MIDI_CONTROL_CONFIG_VERSION_V2: bytes = MIDI_CONTROL_CONFIG_BYTES_V2; break;
    case MIDI_CONTROL_CONFIG_VERSION_V1: bytes = MIDI_CONTROL_CONFIG_BYTES_V1; break;
    default:                             return MIDI_CONTROL_STATUS_BAD_VERSION;
    }
    if (len < bytes || get16(in + OFF_LENGTH) != (uint16_t)bytes)
        return MIDI_CONTROL_STATUS_BAD_LENGTH;
    if (get32(in + OFF_CRC) != midi_control_crc32(in + OFF_GLOBAL, bytes - OFF_GLOBAL))
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
    c.global.bank_wait = g[24];                 /* reserved, so zero - send at once - before it */
    c.global.hold_time = g[25];                 /* reserved, so zero - the default - before it */

    get_buttons(in + OFF_BUTTONS, c.buttons);
    get_encoders(in + OFF_ENCODERS, c.encoders);
    get_analogs(in + OFF_ANALOGS, c.analogs);

    /* A version 1 record stops short of the calibration block. `c` was zeroed,
     * which is what "never calibrated" is, so the unit learns its travel again
     * from the platform's seeds. */
    if (bytes >= OFF_PROFILES) {
        for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++) {
            const uint8_t *p = in + OFF_CAL + 8u * i;
            struct midi_control_analog_cal *cl = &c.analog_cal[i];
            cl->raw_lo = get16(p + 0);
            cl->raw_hi = get16(p + 2);
            cl->flags = p[4];
        }
    }

    if (bytes >= OFF_NAMES) {
        c.profile_count = in[OFF_PROFILES + 0u];
        c.profile_active = in[OFF_PROFILES + 1u];
        for (unsigned i = 0; i < MIDI_CONTROL_PROFILE_COUNT; i++) {
            struct midi_control_profile *pr = &c.profiles[i];
            const uint8_t *p = in + OFF_PROFILE_0 + PROF_BYTES * i;
            memcpy(pr->name, p + PROF_NAME, MIDI_CONTROL_PROFILE_NAME_LEN);
            get_buttons(p + PROF_BUTTONS, pr->buttons);
            get_encoders(p + PROF_ENCODERS, pr->encoders);
            get_analogs(p + PROF_ANALOGS, pr->analogs);
        }
    } else {
        upgrade_profiles(&c);
    }

    /* A version 3 record stops short of the names, and `c` was zeroed, which
     * is every program unnamed: the screen shows the bank and preset as it
     * did before (docs/05 section 8). */
    if (bytes >= OFF_HOLDS)
        memcpy(c.program_names, in + OFF_NAMES, sizeof(c.program_names));

    /* A version 4 record stops short of the holds, and `c` was zeroed, which
     * is no switch held for anything: every switch acts at its press, as it
     * did (docs/05 section 3). */
    if (bytes >= OFF_END) {
        get_holds(in + OFF_HOLDS, c.holds);
        for (unsigned i = 0; i < MIDI_CONTROL_PROFILE_COUNT; i++)
            get_holds(in + OFF_PROFILE_HOLDS + HOLDS_BYTES * i, c.profiles[i].holds);
    }

    midi_control_config_sanitise(&c);
    *cfg = c;
    return MIDI_CONTROL_STATUS_OK;
}

static uint8_t clamp7(uint8_t v)
{
    return v > 127u ? 127u : v;
}

/* Printable ASCII, NUL padded, and the last byte always NUL so the name can be
 * handed out as a C string. An empty one is filled in, because a blank line in
 * the picker says nothing about which profile it is. */
static void clamp_name(char *name, unsigned len, unsigned index)
{
    for (unsigned i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (ch == 0)
            break;
        if (ch < 0x20u || ch > 0x7Eu)
            name[i] = '?';
    }
    name[len - 1u] = 0;
    if (!name[0])
        default_profile_name(name, index);
}

static void sanitise_maps(struct midi_control_button_map *buttons,
                          struct midi_control_encoder_map *encoders,
                          struct midi_control_analog_map *analogs,
                          struct midi_control_hold_map *holds)
{
    for (unsigned i = 0; i < MIDI_CONTROL_BUTTON_COUNT; i++) {
        struct midi_control_hold_map *h = &holds[i];
        /* Mode's hold opens the profile picker and the toe switch's starts a
         * calibration: a hold action there would never be reached. */
        if (h->action >= MIDI_CONTROL_BUTTON_ACTION_COUNT || !midi_control_button_can_hold(i))
            h->action = MIDI_CONTROL_ACT_NONE;
        if (h->action == MIDI_CONTROL_ACT_NONE)
            h->number = h->value_on = h->value_off = 0;
        h->number = clamp7(h->number);
        h->value_on = clamp7(h->value_on);
        h->value_off = clamp7(h->value_off);
    }
    for (unsigned i = 0; i < MIDI_CONTROL_BUTTON_COUNT; i++) {
        struct midi_control_button_map *b = &buttons[i];
        if (b->action >= MIDI_CONTROL_BUTTON_ACTION_COUNT)
            b->action = MIDI_CONTROL_ACT_NONE;
        if (b->channel > 16u)
            b->channel = 0u;
        b->number = clamp7(b->number);
        b->value_on = clamp7(b->value_on);
        b->value_off = clamp7(b->value_off);
        b->led_on &= 7u;
        b->led_off &= 7u;
        /* The one flag there is means something to Bank up and Bank down
         * only. Left on another action it would be a difference between two
         * records that do the same thing. */
        if (b->action == MIDI_CONTROL_ACT_BANK_UP || b->action == MIDI_CONTROL_ACT_BANK_DOWN)
            b->flags &= MIDI_CONTROL_BUTTON_MOVES_PROGRAMS;
        else
            b->flags = 0;
    }
    for (unsigned i = 0; i < MIDI_CONTROL_ENCODER_COUNT; i++) {
        struct midi_control_encoder_map *e = &encoders[i];
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
        struct midi_control_analog_map *a = &analogs[i];
        if (a->mode >= MIDI_CONTROL_ANALOG_MODE_COUNT)
            a->mode = MIDI_CONTROL_ANM_NONE;
        if (a->channel > 16u)
            a->channel = 0u;
        a->number = clamp7(a->number);
        a->invert = a->invert ? 1u : 0u;
        a->out_min = clamp7(a->out_min);
        a->out_max = clamp7(a->out_max);
        if (a->pad_lo > MIDI_CONTROL_ANALOG_PAD_MAX)
            a->pad_lo = MIDI_CONTROL_ANALOG_PAD_MAX;
        if (a->pad_hi > MIDI_CONTROL_ANALOG_PAD_MAX)
            a->pad_hi = MIDI_CONTROL_ANALOG_PAD_MAX;
    }
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
    g->name[MIDI_CONTROL_CONFIG_NAME_LEN - 1u] = 0;   /* may be empty: the screen falls back to the product name */
    if (g->display_timeout > MIDI_CONTROL_DISPLAY_TIMEOUT_MAX)
        g->display_timeout = (uint16_t)MIDI_CONTROL_DISPLAY_TIMEOUT_MAX;
    g->bank_wait = g->bank_wait ? 1u : 0u;
    if (g->hold_time == 0u)
        g->hold_time = MIDI_CONTROL_HOLD_TIME_DEFAULT;
    if (g->hold_time < MIDI_CONTROL_HOLD_TIME_MIN)
        g->hold_time = MIDI_CONTROL_HOLD_TIME_MIN;
    if (g->hold_time > MIDI_CONTROL_HOLD_TIME_MAX)
        g->hold_time = MIDI_CONTROL_HOLD_TIME_MAX;
    memset(g->reserved1, 0, sizeof(g->reserved1));

    sanitise_maps(cfg->buttons, cfg->encoders, cfg->analogs, cfg->holds);

    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++) {
        struct midi_control_analog_cal *cl = &cfg->analog_cal[i];
        if (cl->raw_lo > cl->raw_hi) {
            uint16_t t = cl->raw_lo;
            cl->raw_lo = cl->raw_hi;
            cl->raw_hi = t;
        }
        if (cl->raw_hi > MIDI_CONTROL_CAL_RAW_MAX)
            cl->raw_hi = (uint16_t)MIDI_CONTROL_CAL_RAW_MAX;
        cl->flags &= MIDI_CONTROL_CAL_VALID;
        /* A window too narrow to scale from is not a calibration. Drop it and
         * let the unit learn again, rather than leave the control stuck. */
        if ((unsigned)(cl->raw_hi - cl->raw_lo) < MIDI_CONTROL_CAL_MIN_SPAN)
            cl->flags = 0;
        if (!cl->flags)
            cl->raw_lo = cl->raw_hi = 0;
        memset(cl->reserved, 0, sizeof(cl->reserved));
    }

    if (cfg->profile_count < 1u)
        cfg->profile_count = 1u;
    if (cfg->profile_count > MIDI_CONTROL_PROFILE_COUNT)
        cfg->profile_count = (uint8_t)MIDI_CONTROL_PROFILE_COUNT;
    if (cfg->profile_active >= cfg->profile_count)
        cfg->profile_active = 0u;
    for (unsigned i = 0; i < MIDI_CONTROL_PROFILE_COUNT; i++) {
        clamp_name(cfg->profiles[i].name, MIDI_CONTROL_PROFILE_NAME_LEN, i);
        sanitise_maps(cfg->profiles[i].buttons, cfg->profiles[i].encoders, cfg->profiles[i].analogs,
                      cfg->profiles[i].holds);
    }

    /* Program names as the profile names are kept, except that an empty one
     * stays empty - it is a program with no name, not a name to fill in - and
     * everything after the NUL is cleared, so a name compares by what it says. */
    for (unsigned p = 0; p < MIDI_CONTROL_PROGRAM_COUNT; p++) {
        char *name = cfg->program_names[p];
        unsigned i = 0;
        for (; i + 1u < MIDI_CONTROL_PROGRAM_NAME_LEN && name[i]; i++) {
            const unsigned char ch = (unsigned char)name[i];
            if (ch < 0x20u || ch > 0x7Eu)
                name[i] = '?';
        }
        for (; i < MIDI_CONTROL_PROGRAM_NAME_LEN; i++)
            name[i] = 0;
    }

    /* Last, and in this direction only: the live mapping is the one every
     * caller edits - the app's inspector, SET_CONFIG, a file - so an edit to it
     * is an edit to the profile it came from. Writing it back here is what
     * makes that true without a single call site having to remember, and what
     * stops the two copies in the record ever disagreeing (docs/05 section 7). */
    memcpy(cfg->profiles[cfg->profile_active].buttons, cfg->buttons, sizeof(cfg->buttons));
    memcpy(cfg->profiles[cfg->profile_active].encoders, cfg->encoders, sizeof(cfg->encoders));
    memcpy(cfg->profiles[cfg->profile_active].analogs, cfg->analogs, sizeof(cfg->analogs));
    memcpy(cfg->profiles[cfg->profile_active].holds, cfg->holds, sizeof(cfg->holds));
}

int midi_control_config_moves_programs(const struct midi_control_config *cfg)
{
    for (unsigned i = 0; i < MIDI_CONTROL_BUTTON_COUNT; i++) {
        const struct midi_control_button_map *b = &cfg->buttons[i];
        if ((b->action == MIDI_CONTROL_ACT_BANK_UP || b->action == MIDI_CONTROL_ACT_BANK_DOWN)
            && (b->flags & MIDI_CONTROL_BUTTON_MOVES_PROGRAMS))
            return 1;
    }
    return 0;
}

int midi_control_config_equal(const struct midi_control_config *a, const struct midi_control_config *b)
{
    uint8_t x[MIDI_CONTROL_CONFIG_BYTES], y[MIDI_CONTROL_CONFIG_BYTES];

    midi_control_config_write(a, x);
    midi_control_config_write(b, y);
    /* The body, minus the calibration block in the middle of it: two units with
     * the same mapping and different pedals are configured the same, and a
     * calibration learned while the app is open is not an edit the user has to
     * be told about. Everything either side of it does count - a renamed or
     * re-mapped profile, or a program named, is an edit like any other. */
    return memcmp(x + OFF_GLOBAL, y + OFF_GLOBAL, OFF_CAL - OFF_GLOBAL) == 0
        && memcmp(x + OFF_PROFILES, y + OFF_PROFILES, OFF_END - OFF_PROFILES) == 0;
}

/* ---- names --------------------------------------------------------------- */

const char *midi_control_button_action_name(unsigned action)
{
    static const char *const k[MIDI_CONTROL_BUTTON_ACTION_COUNT] = {
        "None", "Note (momentary)", "Note (toggle)", "CC (momentary)",
        "CC (toggle)", "Program Change", "Program up", "Program down",
        "Bank up", "Bank down", "Preset in bank", "Tap tempo",
    };
    return action < MIDI_CONTROL_BUTTON_ACTION_COUNT ? k[action] : "?";
}

const char *midi_control_encoder_mode_name(unsigned mode)
{
    static const char *const k[MIDI_CONTROL_ENCODER_MODE_COUNT] = {
        "None", "CC relative (1 / 127)", "CC relative (65 / 63)",
        "CC absolute", "Program up / down", "CC relative (1 / 65)",
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
