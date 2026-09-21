#include <string.h>
#include "midi_control/midi_control_device.h"
#include "midi_control/midi_control_protocol.h"
#include "midi_control/midi_control_ui.h"
#include "midi_control/midi_control_version.h"

void midi_control_fmt_uint(char *out, unsigned value)
{
    char tmp[11];
    unsigned n = 0;

    do {
        tmp[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && n < sizeof(tmp) - 1u);

    for (unsigned i = 0; i < n; i++)
        out[i] = tmp[n - 1u - i];
    out[n] = 0;
}

void midi_control_ui_program_label(uint8_t program, uint8_t bank_size, char *out)
{
    if (bank_size < 1u)
        bank_size = 1u;
    midi_control_fmt_uint(out, (unsigned)(program / bank_size) + 1u);
    unsigned n = 0;
    while (out[n])
        n++;
    unsigned within = program % bank_size;
    /* Letters for the first 26 presets in a bank; digits past that would only
     * matter for a bank size nobody uses. */
    out[n++] = (char)(within < 26u ? 'A' + within : '0' + (within - 26u));
    out[n] = 0;
}

/* ---- the view ------------------------------------------------------------------ */

/* Text into a fixed field: at most n - 1 bytes, the rest NUL, so equal words
 * compare equal byte for byte. */
static void set_text(char *dst, unsigned n, const char *src)
{
    unsigned i = 0;
    if (src)
        for (; i + 1u < n && src[i]; i++)
            dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
}

static void add_text(char *dst, unsigned n, const char *src)
{
    unsigned i = 0;
    while (i < n && dst[i])
        i++;
    for (; i + 1u < n && *src; i++, src++)
        dst[i] = *src;
    if (i < n)
        dst[i] = 0;
}

static void add_uint(char *dst, unsigned n, unsigned value)
{
    char num[11];
    midi_control_fmt_uint(num, value);
    add_text(dst, n, num);
}

static void set_colour(uint8_t *out, unsigned colour)
{
    out[0] = (uint8_t)(colour >> 8);
    out[1] = (uint8_t)colour;
}

/* The footswitches, left to right as their LED columns run (docs/01 section 4). */
static const struct {
    uint8_t button;
    const char *name;
} k_switches[MIDI_CONTROL_UI_SWITCHES] = {
    { MIDI_CONTROL_BTN_BANK_UP,   "BANK" MIDI_CONTROL_SYM_UP },
    { MIDI_CONTROL_BTN_BANK_DOWN, "BANK" MIDI_CONTROL_SYM_DOWN },
    { MIDI_CONTROL_BTN_FS_A,      "A" },
    { MIDI_CONTROL_BTN_FS_B,      "B" },
    { MIDI_CONTROL_BTN_FS_C,      "C" },
    { MIDI_CONTROL_BTN_FS_D,      "D" },
    { MIDI_CONTROL_BTN_TAP,       "TAP" },
};

static void switch_label(char *out, unsigned n, const struct midi_control_device *d,
                         const struct midi_control_button_map *b)
{
    const uint8_t bank = d->config.global.bank_size < 1u ? 1u : d->config.global.bank_size;
    const unsigned step = b->number ? b->number : 1u;
    char program[8];

    switch (b->action) {
    case MIDI_CONTROL_ACT_NOTE_MOMENTARY:
    case MIDI_CONTROL_ACT_NOTE_TOGGLE:
        set_text(out, n, "NOTE ");
        add_uint(out, n, b->number);
        break;
    case MIDI_CONTROL_ACT_CC_MOMENTARY:
    case MIDI_CONTROL_ACT_CC_TOGGLE:
        set_text(out, n, "CC ");
        add_uint(out, n, b->number);
        break;
    case MIDI_CONTROL_ACT_PROGRAM:
        midi_control_ui_program_label(b->number, bank, program);
        set_text(out, n, program);
        break;
    case MIDI_CONTROL_ACT_PROGRAM_UP:
    case MIDI_CONTROL_ACT_PROGRAM_DOWN:
    case MIDI_CONTROL_ACT_BANK_UP:
    case MIDI_CONTROL_ACT_BANK_DOWN: {
        /* Where the step lands, wrapping as the engine does. */
        const int up = b->action == MIDI_CONTROL_ACT_PROGRAM_UP || b->action == MIDI_CONTROL_ACT_BANK_UP;
        const int by_bank = b->action == MIDI_CONTROL_ACT_BANK_UP || b->action == MIDI_CONTROL_ACT_BANK_DOWN;
        const unsigned delta = (by_bank ? bank : step) % 128u;
        const unsigned target = (d->engine.program + (up ? delta : 128u - delta)) % 128u;
        midi_control_ui_program_label((uint8_t)target, bank, program);
        set_text(out, n, up ? MIDI_CONTROL_SYM_UP : MIDI_CONTROL_SYM_DOWN);
        add_text(out, n, program);
        break;
    }
    case MIDI_CONTROL_ACT_PRESET_IN_BANK: {
        const unsigned base = (unsigned)(d->engine.program / bank) * bank;
        midi_control_ui_program_label((uint8_t)((base + b->number % bank) & 0x7Fu), bank, program);
        set_text(out, n, program);
        break;
    }
    default:
        set_text(out, n, "");
        break;
    }
}

/* The picker, over the program region, and what it makes of the six
 * footswitches it takes: A-D name the profiles on the page, Bank Up and Bank
 * Down the pages either side. Tap is left as the mapping has it. */
static void view_picker(struct midi_control_ui_view *v, const struct midi_control_device *d)
{
    const struct midi_control_config *c = &d->config;
    const unsigned pages = midi_control_device_profile_pages(d);

    v->picker.open = 1;
    set_text(v->picker.page, sizeof(v->picker.page), "");
    add_uint(v->picker.page, sizeof(v->picker.page), d->profile_page + 1u);
    add_text(v->picker.page, sizeof(v->picker.page), "/");
    add_uint(v->picker.page, sizeof(v->picker.page), pages);
    set_text(v->picker.title, sizeof(v->picker.title), midi_control_config_profile_name(c, c->profile_active));
    set_text(v->picker.hint, sizeof(v->picker.hint), pages > 1u ? "A-D CHOOSE   BANK PAGES" : "A-D CHOOSE");

    for (unsigned i = 0; i < MIDI_CONTROL_UI_SWITCHES; i++) {
        const enum midi_control_button id = (enum midi_control_button)k_switches[i].button;
        struct midi_control_ui_switch *s = &v->switches[i];
        const int profile = midi_control_device_picker_profile(d, id);

        if (id == MIDI_CONTROL_BTN_BANK_UP || id == MIDI_CONTROL_BTN_BANK_DOWN) {
            const int up = id == MIDI_CONTROL_BTN_BANK_UP;
            const unsigned to = (d->profile_page + (up ? 1u : pages - 1u)) % pages;
            set_text(s->label, sizeof(s->label), up ? MIDI_CONTROL_SYM_UP "PAGE " : MIDI_CONTROL_SYM_DOWN "PAGE ");
            add_uint(s->label, sizeof(s->label), to + 1u);
            s->mapped = pages > 1u;
            s->active = 0u;
        } else if ((unsigned)id - (unsigned)MIDI_CONTROL_BTN_FS_A < MIDI_CONTROL_PROFILE_PAGE) {
            set_text(s->label, sizeof(s->label), profile >= 0 ? midi_control_config_profile_name(c, (unsigned)profile) : "");
            s->mapped = profile >= 0;
            s->active = profile >= 0 && (unsigned)profile == c->profile_active;
        }
    }
}

/* A calibration, over the program region: the step, what to do about it, and
 * the counts - so a stop that will not be taken can be seen not settling. */
static void view_calibrate(struct midi_control_ui_view *v, const struct midi_control_device *d)
{
    const struct midi_control_analog_capture *c = &d->capture;
    struct midi_control_ui_calibrate *k = &v->calibrate;

    k->open = 1;
    k->stage = c->stage;
    k->progress = (uint8_t)midi_control_analog_capture_progress(c);
    set_text(k->title, sizeof(k->title), "CALIBRATE PEDAL");

    switch (c->stage) {
    case MIDI_CONTROL_CAL_FIRST:
        set_text(k->step, sizeof(k->step), "1/2");
        set_text(k->hint, sizeof(k->hint), "HOLD IT AT ONE END");
        set_text(k->detail, sizeof(k->detail), "NOW ");
        add_uint(k->detail, sizeof(k->detail), c->raw);
        break;
    case MIDI_CONTROL_CAL_SECOND:
        set_text(k->step, sizeof(k->step), "2/2");
        set_text(k->hint, sizeof(k->hint), "NOW HOLD THE OTHER END");
        set_text(k->detail, sizeof(k->detail), "END ");
        add_uint(k->detail, sizeof(k->detail), c->first);
        add_text(k->detail, sizeof(k->detail), "   NOW ");
        add_uint(k->detail, sizeof(k->detail), c->raw);
        break;
    case MIDI_CONTROL_CAL_DONE:
        set_text(k->step, sizeof(k->step), "OK");
        set_text(k->hint, sizeof(k->hint), "CALIBRATED");
        set_text(k->detail, sizeof(k->detail), "");
        add_uint(k->detail, sizeof(k->detail), c->first < c->second ? c->first : c->second);
        add_text(k->detail, sizeof(k->detail), " - ");
        add_uint(k->detail, sizeof(k->detail), c->first < c->second ? c->second : c->first);
        /* Written at once unless the user has edits pending, which it will not
         * commit for them: then it goes with their save. */
        if (!d->cal_unsaved)
            add_text(k->detail, sizeof(k->detail), "   SAVED");
        else if (d->dirty)
            add_text(k->detail, sizeof(k->detail), "   SAVE TO KEEP");
        break;
    default:
        set_text(k->step, sizeof(k->step), "--");
        set_text(k->hint, sizeof(k->hint), "CANCELLED");
        set_text(k->detail, sizeof(k->detail), "THE PEDAL IS UNCHANGED");
        break;
    }
}

static void view_running(struct midi_control_ui_view *v, const struct midi_control_ui_model *m)
{
    const struct midi_control_device *d = m->device;
    const struct midi_control_config *c = &d->config;
    uint8_t leds[MIDI_CONTROL_COLUMNS];

    v->running = 1;
    midi_control_device_leds(d, leds);

    set_text(v->status.title, sizeof(v->status.title), c->global.name[0] ? c->global.name : MIDI_CONTROL_PRODUCT_NAME);
    /* One profile is no choice at all, and a unit set up before profiles
     * existed looks as it always did. */
    if (c->profile_count > 1u)
        set_text(v->status.profile, sizeof(v->status.profile), midi_control_config_profile_name(c, c->profile_active));
    set_text(v->status.channel, sizeof(v->status.channel), "CH ");
    add_uint(v->status.channel, sizeof(v->status.channel), (c->global.midi_channel & 0x0Fu) + 1u);
    v->status.usb = m->usb_mounted ? 1u : 0u;
    v->status.storage = (uint8_t)midi_control_device_storage_state(d);
    v->status.dirty = d->dirty ? 1u : 0u;
    if (m->diag && m->diag[0]) {
        set_text(v->status.warning, sizeof(v->status.warning), m->diag);
        set_colour(v->status.warning_colour, m->diag_colour ? m->diag_colour : MIDI_CONTROL_RED);
    }

    /* The picker or a calibration takes the program region's place: the view
     * holds one of the three, never two, so the region redraws when either
     * opens and closes. */
    if (!midi_control_device_picking_profile(d) && midi_control_device_calibration(d) == MIDI_CONTROL_CAL_OFF) {
        midi_control_ui_program_label(d->engine.program, c->global.bank_size, v->program.label);
        set_text(v->program.number, sizeof(v->program.number), "PC ");
        add_uint(v->program.number, sizeof(v->program.number), d->engine.program);
        set_text(v->program.bank, sizeof(v->program.bank), "Bank ");
        add_uint(v->program.bank, sizeof(v->program.bank),
                 d->engine.program / (c->global.bank_size < 1u ? 1u : c->global.bank_size) + 1u);
        set_text(v->program.preset, sizeof(v->program.preset), "Preset ");
        add_text(v->program.preset, sizeof(v->program.preset), v->program.label + strlen(v->program.label) - 1u);
        set_text(v->program.line1, sizeof(v->program.line1), d->text[0]);
        set_text(v->program.line2, sizeof(v->program.line2), d->text[1]);
    }

    for (unsigned i = 0; i < MIDI_CONTROL_UI_KNOBS; i++) {
        const struct midi_control_encoder_map *e = &c->encoders[i];
        struct midi_control_ui_knob *k = &v->knobs[i];
        k->mode = e->mode;
        if (e->mode == MIDI_CONTROL_ENCM_PROGRAM) {
            set_text(k->label, sizeof(k->label), "PRESET");
        } else if (e->mode != MIDI_CONTROL_ENCM_NONE) {
            set_text(k->label, sizeof(k->label), "CC ");
            add_uint(k->label, sizeof(k->label), e->number);
        }
        if (e->mode == MIDI_CONTROL_ENCM_CC_ABSOLUTE)
            k->value = d->engine.enc_value[i];
    }

    static const char *const k_meter[2] = { "VOL", "EXP" };
    for (unsigned i = 0; i < 2u; i++) {
        set_text(v->meters[i].label, sizeof(v->meters[i].label), k_meter[i]);
        v->meters[i].mapped = c->analogs[i].mode != MIDI_CONTROL_ANM_NONE;
        v->meters[i].value = d->engine.analog_in[i] & 0x7Fu;
    }

    for (unsigned i = 0; i < MIDI_CONTROL_UI_SWITCHES; i++) {
        const enum midi_control_button id = (enum midi_control_button)k_switches[i].button;
        const struct midi_control_button_map *b = &c->buttons[id];
        struct midi_control_ui_switch *s = &v->switches[i];
        const int col = midi_control_button_led_column(id);
        set_text(s->name, sizeof(s->name), k_switches[i].name);
        switch_label(s->label, sizeof(s->label), d, b);
        s->mapped = b->action != MIDI_CONTROL_ACT_NONE;
        s->active = midi_control_engine_button_active(&d->engine, id) ? 1u : 0u;
        s->led = col >= 0 ? leds[col] : 0u;
    }

    if (midi_control_device_calibration(d) != MIDI_CONTROL_CAL_OFF)
        view_calibrate(v, d);
    else if (midi_control_device_picking_profile(d))
        view_picker(v, d);
}

void midi_control_ui_view(struct midi_control_ui_view *v, const struct midi_control_ui_model *m)
{
    memset(v, 0, sizeof(*v));
    if (m->device && !midi_control_device_display_on(m->device)) {
        v->display_off = 1;
        return;
    }
    if (m->device) {
        view_running(v, m);
        return;
    }
    set_text(v->start.stage, sizeof(v->start.stage), m->stage);
    set_text(v->start.note, sizeof(v->start.note), m->note);
    if (m->diag && m->diag[0]) {
        set_text(v->start.diag, sizeof(v->start.diag), m->diag);
        set_colour(v->start.diag_colour, m->diag_colour ? m->diag_colour : MIDI_CONTROL_GREY);
    }
    v->start.colour_bars = m->colour_bars ? 1u : 0u;
}

/* ---- drawing ------------------------------------------------------------------- */

#define W MIDI_CONTROL_LCD_WIDTH
#define H MIDI_CONTROL_LCD_HEIGHT

#define BG        MIDI_CONTROL_RGB(10, 12, 16)
#define PANEL     MIDI_CONTROL_RGB(28, 32, 40)
#define RULE      MIDI_CONTROL_RGB(44, 50, 60)
#define INK       MIDI_CONTROL_RGB(236, 240, 244)
#define INK_DIM   MIDI_CONTROL_RGB(150, 158, 170)
#define INK_FAINT MIDI_CONTROL_RGB(84, 92, 104)
#define ACCENT    MIDI_CONTROL_RGB(255, 168, 38)
#define GOOD      MIDI_CONTROL_RGB(70, 214, 110)
#define WARN      MIDI_CONTROL_RGB(255, 196, 40)

#define Q4 MIDI_CONTROL_Q4

/* The regions of the running screen. They tile the panel, and each draws its
 * own background, so none depends on what a neighbour drew. */
enum {
    R_STATUS,
    R_PROGRAM,
    R_KNOB,
    R_METERS = R_KNOB + MIDI_CONTROL_UI_KNOBS,
    R_SWITCH,
    R_COUNT = R_SWITCH + MIDI_CONTROL_UI_SWITCHES
};

#define STATUS_H  32
#define PROGRAM_H 118
#define KNOBS_Y   (STATUS_H + PROGRAM_H)
#define KNOBS_H   58
#define KNOB_W    80
#define SWITCH_Y  (KNOBS_Y + KNOBS_H)

static struct midi_control_rect rect(int x, int y, int w, int h)
{
    struct midi_control_rect r;
    r.x = (int16_t)x;
    r.y = (int16_t)y;
    r.w = (int16_t)w;
    r.h = (int16_t)h;
    return r;
}

static struct midi_control_rect region(unsigned id)
{
    if (id == R_STATUS)
        return rect(0, 0, W, STATUS_H);
    if (id == R_PROGRAM)
        return rect(0, STATUS_H, W, PROGRAM_H);
    if (id < R_METERS)
        return rect((int)(id - R_KNOB) * KNOB_W, KNOBS_Y, KNOB_W, KNOBS_H);
    if (id == R_METERS)
        return rect(MIDI_CONTROL_UI_KNOBS * KNOB_W, KNOBS_Y, W - MIDI_CONTROL_UI_KNOBS * KNOB_W, KNOBS_H);
    const int i = (int)(id - R_SWITCH);
    const int x = i * W / MIDI_CONTROL_UI_SWITCHES;
    return rect(x, SWITCH_Y, (i + 1) * W / MIDI_CONTROL_UI_SWITCHES - x, H - SWITCH_Y);
}

static int region_differs(unsigned id, const struct midi_control_ui_view *a, const struct midi_control_ui_view *b)
{
    if (id == R_STATUS)
        return memcmp(&a->status, &b->status, sizeof(a->status)) != 0;
    if (id == R_PROGRAM)
        return memcmp(&a->program, &b->program, sizeof(a->program)) != 0
            || memcmp(&a->picker, &b->picker, sizeof(a->picker)) != 0
            || memcmp(&a->calibrate, &b->calibrate, sizeof(a->calibrate)) != 0;
    if (id < R_METERS)
        return memcmp(&a->knobs[id - R_KNOB], &b->knobs[id - R_KNOB], sizeof(a->knobs[0])) != 0;
    if (id == R_METERS)
        return memcmp(a->meters, b->meters, sizeof(a->meters)) != 0;
    return memcmp(&a->switches[id - R_SWITCH], &b->switches[id - R_SWITCH], sizeof(a->switches[0])) != 0;
}

static unsigned colour_of(const uint8_t c[2])
{
    return ((unsigned)c[0] << 8) | c[1];
}

static unsigned led_colour(uint8_t bits)
{
    static const uint16_t k[8] = {
        INK_FAINT,
        MIDI_CONTROL_RGB(255, 72, 60),    /* red */
        MIDI_CONTROL_RGB(70, 220, 100),   /* green */
        MIDI_CONTROL_RGB(255, 206, 50),   /* yellow */
        MIDI_CONTROL_RGB(70, 130, 255),   /* blue */
        MIDI_CONTROL_RGB(226, 90, 236),   /* magenta */
        MIDI_CONTROL_RGB(50, 214, 236),   /* cyan */
        MIDI_CONTROL_RGB(240, 244, 248),  /* white */
    };
    return k[bits & 7u];
}

static void text_centred(struct midi_control_gfx *g, int x, int w, int y, int size, const char *s, unsigned colour)
{
    midi_control_gfx_text(g, x + (w - midi_control_gfx_text_width(size, 0, s)) / 2, y, size, 0, s, colour);
}

/* The first of `sizes` (largest first, 0 ends the list) at which `s` fits `w`;
 * the smallest if none does. */
static int fit(const uint8_t *sizes, int w, const char *s)
{
    int size = sizes[0];
    for (unsigned i = 0; sizes[i]; i++) {
        size = sizes[i];
        if (midi_control_gfx_text_width(size, 0, s) <= w)
            break;
    }
    return size;
}

/* A rounded tag ending at `right`, with an optional coloured dot. Returns its left edge. */
static int chip(struct midi_control_gfx *g, int right, const char *s, unsigned ink, unsigned dot)
{
    const int w = midi_control_gfx_text_width(10, 0, s) + 20 + (dot ? 12 : 0);
    const int x = right - w;

    midi_control_gfx_fill_round_rect(g, x, 6, w, 20, 10, PANEL);
    if (dot)
        midi_control_gfx_line(g, Q4(x + 13), Q4(16), Q4(x + 13), Q4(16), Q4(7), dot);
    midi_control_gfx_text(g, x + 10 + (dot ? 12 : 0), 11, 10, 0, s, ink);
    return x;
}

static void draw_status(struct midi_control_gfx *g, const struct midi_control_ui_status *s)
{
    const char *storage;
    unsigned ink;

    switch (s->storage) {
    case MIDI_CONTROL_STORAGE_READY:
        storage = s->dirty ? "UNSAVED" : "SAVED";
        ink = s->dirty ? WARN : GOOD;
        break;
    case MIDI_CONTROL_STORAGE_LOCKED:
        storage = "LOCKED";
        ink = WARN;
        break;
    case MIDI_CONTROL_STORAGE_UNKNOWN:
        storage = "STORAGE ?";
        ink = WARN;
        break;
    default:
        storage = s->dirty ? "NOT SAVED" : "NO STORAGE";
        ink = INK_FAINT;
        break;
    }

    midi_control_gfx_fill_rect(g, 0, STATUS_H - 1, W, 1, RULE);
    int x = chip(g, W - 8, storage, ink, ink == INK_FAINT ? 0u : ink);
    x = chip(g, x - 6, s->usb ? "USB" : "NO HOST", s->usb ? INK : INK_FAINT, s->usb ? GOOD : 0u);
    x = chip(g, x - 6, s->channel, INK_DIM, 0u);
    if (s->profile[0])
        x = chip(g, x - 6, s->profile, INK, ACCENT);

    midi_control_gfx_set_clip(g, 0, 0, x - 8, STATUS_H);
    if (s->warning[0])
        midi_control_gfx_text(g, 12, 10, 12, 0, s->warning, colour_of(s->warning_colour));
    else
        midi_control_gfx_text(g, 12, 9, 14, 0, s->title, INK);
}

/* The picker, in the program region's frame: the page in the box where the
 * program's letter usually is, the live profile beside it, and what the
 * switches do now under that. */
static void draw_picker(struct midi_control_gfx *g, const struct midi_control_ui_picker *p)
{
    static const uint8_t k_title[] = { 36, 30, 24, 20, 16, 0 };
    const int top = STATUS_H + 8, bx = 10, bw = 170, bh = 102;
    const int tx = 196, tw = W - 10 - tx;

    midi_control_gfx_fill_round_rect(g, bx, top, bw, bh, 14, PANEL);
    midi_control_gfx_fill_round_rect(g, bx + 14, top, bw - 28, 4, 2, GOOD);
    text_centred(g, bx, bw, top + 22, 44, p->page, INK);
    text_centred(g, bx, bw, top + bh - 20, 10, "PROFILES", INK_DIM);

    midi_control_gfx_set_clip(g, tx, STATUS_H, tw, PROGRAM_H);
    midi_control_gfx_text(g, tx, top + 4, 10, 0, "PROFILE", INK_FAINT);
    midi_control_gfx_text(g, tx, top + 22, fit(k_title, tw, p->title), 0, p->title, INK);
    midi_control_gfx_text(g, tx, top + 72, 12, 0, p->hint, INK_DIM);
}

/* A calibration, in the program region's frame: the step in the box, and
 * beside it what to do, a bar for the hold, and the counts. */
static void draw_calibrate(struct midi_control_gfx *g, const struct midi_control_ui_calibrate *k)
{
    static const uint8_t k_hint[] = { 26, 22, 18, 15, 12, 0 };
    const int top = STATUS_H + 8, bx = 10, bw = 170, bh = 102;
    const int tx = 196, tw = W - 10 - tx;
    const int running = k->stage == MIDI_CONTROL_CAL_FIRST || k->stage == MIDI_CONTROL_CAL_SECOND;
    const unsigned tone = k->stage == MIDI_CONTROL_CAL_DONE ? GOOD : running ? WARN : INK_FAINT;
    const int bar_y = top + 60, bar_h = 8;

    midi_control_gfx_fill_round_rect(g, bx, top, bw, bh, 14, PANEL);
    midi_control_gfx_fill_round_rect(g, bx + 14, top, bw - 28, 4, 2, tone);
    text_centred(g, bx, bw, top + 22, 44, k->step, INK);
    text_centred(g, bx, bw, top + bh - 20, 10, running ? "MODE CANCELS" : "PEDAL", INK_DIM);

    midi_control_gfx_set_clip(g, tx, STATUS_H, tw, PROGRAM_H);
    midi_control_gfx_text(g, tx, top + 4, 10, 0, k->title, INK_FAINT);
    midi_control_gfx_text(g, tx, top + 24, fit(k_hint, tw, k->hint), 0, k->hint, INK);
    midi_control_gfx_fill_round_rect(g, tx, bar_y, tw, bar_h, bar_h / 2, RULE);
    if (k->progress > 0u) {
        const int fw = (int)k->progress * tw / 127;
        midi_control_gfx_fill_round_rect(g, tx, bar_y, fw < bar_h ? bar_h : fw, bar_h, bar_h / 2, tone);
    }
    midi_control_gfx_text(g, tx, top + 78, 12, 0, k->detail, INK_DIM);
}

static void draw_program(struct midi_control_gfx *g, const struct midi_control_ui_program *p)
{
    static const uint8_t k_label[] = { 64, 54, 44, 0 };
    static const uint8_t k_line1[] = { 36, 30, 24, 20, 16, 0 };
    static const uint8_t k_line2[] = { 18, 15, 12, 0 };
    const int top = STATUS_H + 8, bx = 10, bw = 170, bh = 102;
    const int tx = 196, tw = W - 10 - tx;

    midi_control_gfx_fill_round_rect(g, bx, top, bw, bh, 14, PANEL);
    midi_control_gfx_fill_round_rect(g, bx + 14, top, bw - 28, 4, 2, ACCENT);
    const int size = fit(k_label, bw - 20, p->label);
    text_centred(g, bx, bw, top + 6 + (bh - 30 - size) / 2, size, p->label, INK);
    text_centred(g, bx, bw, top + bh - 20, 10, p->number, INK_DIM);

    midi_control_gfx_set_clip(g, tx, STATUS_H, tw, PROGRAM_H);
    if (p->line1[0])
        midi_control_gfx_text(g, tx, top + 22, fit(k_line1, tw, p->line1), 0, p->line1, INK);
    else
        midi_control_gfx_text(g, tx, top + 22, 30, 0, p->bank, INK_FAINT);
    if (p->line2[0])
        midi_control_gfx_text(g, tx, top + 72, fit(k_line2, tw, p->line2), 0, p->line2, INK_DIM);
    else if (!p->line1[0])
        midi_control_gfx_text(g, tx, top + 72, 18, 0, p->preset, INK_FAINT);
}

static void draw_knob(struct midi_control_gfx *g, struct midi_control_rect r, const struct midi_control_ui_knob *k)
{
    /* A 270-degree dial, open at the bottom like the knob's travel. */
    const int cx = Q4(r.x + KNOB_W / 2), cy = Q4(r.y + 23), rad = Q4(17), from = 24, travel = 48;
    char num[11];

    midi_control_gfx_arc(g, cx, cy, rad, from, travel, Q4(4), RULE);
    switch (k->mode) {
    case MIDI_CONTROL_ENCM_CC_ABSOLUTE: {
        const int sweep = (int)k->value * travel / 127;
        if (sweep > 0)
            midi_control_gfx_arc(g, cx, cy, rad, from, sweep, Q4(4), ACCENT);
        const int dir = from + sweep;
        const int px = cx + rad * midi_control_gfx_cos(dir) / 4096, py = cy + rad * midi_control_gfx_sin(dir) / 4096;
        midi_control_gfx_line(g, px, py, px, py, Q4(8), INK);
        midi_control_fmt_uint(num, k->value);
        text_centred(g, r.x, KNOB_W, r.y + 18, 11, num, INK);
        break;
    }
    case MIDI_CONTROL_ENCM_CC_RELATIVE_TWOS:
    case MIDI_CONTROL_ENCM_CC_RELATIVE_OFFSET:
        text_centred(g, r.x, KNOB_W, r.y + 19, 9, "REL", INK_DIM);
        break;
    case MIDI_CONTROL_ENCM_PROGRAM:
        text_centred(g, r.x, KNOB_W, r.y + 19, 9, "PC", INK_DIM);
        break;
    default:
        break;
    }
    text_centred(g, r.x, KNOB_W, r.y + 45, 9, k->label[0] ? k->label : "OFF", k->label[0] ? INK_DIM : INK_FAINT);
}

static void draw_meters(struct midi_control_gfx *g, struct midi_control_rect r, const struct midi_control_ui_meter *m)
{
    for (int i = 0; i < 2; i++) {
        const int bw = 12, bh = 34, bx = r.x + 16 + i * 36, by = r.y + 6;
        midi_control_gfx_fill_round_rect(g, bx, by, bw, bh, bw / 2, PANEL);
        if (m[i].mapped) {
            const int fh = (int)m[i].value * bh / 127;
            if (fh > 0)
                midi_control_gfx_fill_round_rect(g, bx, by + bh - fh, bw, fh, bw / 2, ACCENT);
        }
        text_centred(g, bx - 12, bw + 24, by + bh + 5, 9, m[i].label, m[i].mapped ? INK_DIM : INK_FAINT);
    }
}

static void draw_switch(struct midi_control_gfx *g, struct midi_control_rect r, unsigned index,
                        const struct midi_control_ui_switch *s)
{
    /* Down to 7 for a profile name, which can be fifteen characters. */
    static const uint8_t k_label[] = { 12, 10, 9, 8, 7, 0 };
    const int x = r.x + 3, y = r.y + 2, w = r.w - 6, h = r.h - 6;
    /* The Tap column's bits are lamps, not a colour. */
    const unsigned lit = s->led ? (index == MIDI_CONTROL_UI_SWITCHES - 1u ? ACCENT : led_colour(s->led))
                                : (s->active ? INK : 0u);

    midi_control_gfx_fill_round_rect(g, x, y, w, h, 8, s->active && lit ? midi_control_gfx_mix(PANEL, lit, 72) : PANEL);
    midi_control_gfx_fill_round_rect(g, x + 8, y + 5, w - 16, 4, 2,
                                     !lit ? RULE : s->active ? lit : midi_control_gfx_mix(PANEL, lit, 120));
    text_centred(g, x, w, y + 16, 8, s->name, INK_DIM);
    const char *label = s->mapped ? s->label : "-";
    text_centred(g, x, w, y + 34, fit(k_label, w - 8, label), label, s->mapped ? INK : INK_FAINT);
}

static void draw_region(struct midi_control_gfx *g, unsigned id, const struct midi_control_ui_view *v)
{
    const struct midi_control_rect r = region(id);

    midi_control_gfx_set_clip(g, r.x, r.y, r.w, r.h);
    midi_control_gfx_fill_rect(g, r.x, r.y, r.w, r.h, BG);
    if (id == R_STATUS)
        draw_status(g, &v->status);
    else if (id == R_PROGRAM && v->calibrate.open)
        draw_calibrate(g, &v->calibrate);
    else if (id == R_PROGRAM && v->picker.open)
        draw_picker(g, &v->picker);
    else if (id == R_PROGRAM)
        draw_program(g, &v->program);
    else if (id < R_METERS)
        draw_knob(g, r, &v->knobs[id - R_KNOB]);
    else if (id == R_METERS)
        draw_meters(g, r, v->meters);
    else
        draw_switch(g, r, id - R_SWITCH, &v->switches[id - R_SWITCH]);
    midi_control_gfx_set_clip(g, 0, 0, W, H);
}

static void draw_start(struct midi_control_gfx *g, const struct midi_control_ui_start *s)
{
    static const struct { uint16_t c; const char *name; } k_bars[3] = {
        { MIDI_CONTROL_RED, "R" }, { MIDI_CONTROL_GREEN, "G" }, { MIDI_CONTROL_BLUE, "B" },
    };
    char version[40] = "MIDI CONTROLLER  V";

    midi_control_gfx_set_clip(g, 0, 0, W, H);
    midi_control_gfx_clear(g, BG);
    /* A one-pixel border proves both edges in both axes and turns a wrong
     * stride into obviously slanted verticals. */
    midi_control_gfx_frame_rect(g, 0, 0, W, H, MIDI_CONTROL_GREY);

    text_centred(g, 0, W, 34, 56, MIDI_CONTROL_PRODUCT_NAME, INK);
    midi_control_gfx_fill_round_rect(g, W / 2 - 30, 104, 60, 4, 2, ACCENT);
    add_uint(version, sizeof(version), MIDI_CONTROL_VERSION_MAJOR);
    add_text(version, sizeof(version), ".");
    add_uint(version, sizeof(version), MIDI_CONTROL_VERSION_MINOR);
    add_text(version, sizeof(version), ".");
    add_uint(version, sizeof(version), MIDI_CONTROL_VERSION_PATCH);
    text_centred(g, 0, W, 120, 10, version, INK_DIM);

    text_centred(g, 0, W, 146, 20, s->stage, ACCENT);

    if (s->colour_bars) {
        for (int i = 0; i < 3; i++) {
            const int x = W / 2 - 82 + i * 56;
            midi_control_gfx_fill_round_rect(g, x, 184, 52, 24, 6, k_bars[i].c);
            text_centred(g, x, 52, 190, 12, k_bars[i].name, MIDI_CONTROL_BLACK);
        }
    }
    if (s->diag[0])
        text_centred(g, 0, W, 222, 12, s->diag, colour_of(s->diag_colour));

    midi_control_gfx_text(g, 10, H - 18, 10, 0, s->stage, MIDI_CONTROL_GREY);
    midi_control_gfx_text(g, W - 10 - midi_control_gfx_text_width(10, 0, s->note), H - 18, 10, 0, s->note,
                          MIDI_CONTROL_GREY);
}

static void draw_view(struct midi_control_gfx *g, const struct midi_control_ui_view *v)
{
    if (v->display_off) {
        midi_control_gfx_set_clip(g, 0, 0, W, H);
        midi_control_gfx_clear(g, MIDI_CONTROL_BLACK);
        return;
    }
    if (!v->running) {
        draw_start(g, &v->start);
        return;
    }
    for (unsigned id = 0; id < R_COUNT; id++)
        draw_region(g, id, v);
}

void midi_control_ui_draw(struct midi_control_gfx *g, const struct midi_control_ui_model *m)
{
    struct midi_control_ui_view v;
    midi_control_ui_view(&v, m);
    draw_view(g, &v);
}

int midi_control_ui_update(struct midi_control_gfx *g, const struct midi_control_ui_model *m,
                           struct midi_control_ui_cache *cache, struct midi_control_rect *drawn)
{
    struct midi_control_ui_view v;
    int x0 = W, y0 = H, x1 = 0, y1 = 0;

    midi_control_ui_view(&v, m);
    *drawn = rect(0, 0, 0, 0);

    /* The start-up screen is one piece, and so is the dark one; so is a change of screen. */
    if (!cache->valid || v.running != cache->shown.running || !v.running) {
        if (cache->valid && memcmp(&v, &cache->shown, sizeof(v)) == 0)
            return 0;
        draw_view(g, &v);
        cache->shown = v;
        cache->valid = 1;
        *drawn = rect(0, 0, W, H);
        return 1;
    }

    for (unsigned id = 0; id < R_COUNT; id++) {
        if (!region_differs(id, &v, &cache->shown))
            continue;
        draw_region(g, id, &v);
        const struct midi_control_rect r = region(id);
        x0 = r.x < x0 ? r.x : x0;
        y0 = r.y < y0 ? r.y : y0;
        x1 = r.x + r.w > x1 ? r.x + r.w : x1;
        y1 = r.y + r.h > y1 ? r.y + r.h : y1;
    }
    cache->shown = v;
    if (x0 >= x1)
        return 0;
    *drawn = rect(x0, y0, x1 - x0, y1 - y0);
    return 1;
}
