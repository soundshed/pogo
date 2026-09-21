/* The screen: what the panel shows, laid out once for the firmware and the
 * emulator. The caller describes the device and the board; this file owns
 * every coordinate and every word.
 *
 * Two screens. Start-up (no device yet) narrates the bring-up: the stage, the
 * SDRAM verdict, the colour swatches. Running shows the performance view:
 *
 *   status     title, MIDI channel, USB, storage
 *   program    the bank and preset, big, with the host's two text lines
 *   knobs      encoders 1-5: CC number, and the value in the absolute mode
 *   meters     the volume knob and the expression pedal
 *   switches   Bank Up, Bank Down, A-D and Tap: what each does, lit like its LED
 *
 * With more than one profile in use the status line names the live one. While
 * the profile picker is open (Mode held, docs/05 section 7) the program region
 * shows the picker instead and A-D are labelled with the profiles on the page,
 * Bank Up and Bank Down with the pages either side.
 *
 * While the device's display timeout has it dark (midi_control_device_display_on)
 * the running screen is black instead, all of it.
 *
 * Drawing goes through a view: a plain-data copy of every word and number the
 * screen shows, derived from the model. Each region draws from its part of the
 * view alone, so midi_control_ui_update() redraws exactly the regions whose part
 * changed and a picture built by updates is the picture a full draw makes.
 */
#ifndef MIDI_CONTROL_UI_H
#define MIDI_CONTROL_UI_H

#include <stdint.h>
#include "midi_control_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

struct midi_control_device;

struct midi_control_ui_model {
    const struct midi_control_device *device;   /* NULL: the start-up screen */
    const char *stage;          /* start-up: the bring-up stage, bottom left */
    const char *note;           /* start-up: e.g. "INHERITED", bottom right */
    const char *diag;           /* optional line in `diag_colour`: the SDRAM verdict; running, a warning */
    unsigned diag_colour;
    uint8_t usb_mounted;        /* host has configured the device */
    uint8_t colour_bars;        /* start-up: the labelled R G B swatches */
};

/* ---- the view ----------------------------------------------------------------- */
/* Only char and uint8_t members, so no padding and a view compares with memcmp. */

#define MIDI_CONTROL_UI_KNOBS    5
#define MIDI_CONTROL_UI_SWITCHES 7

struct midi_control_ui_start {
    char stage[24];             /* up to "FIRMWARE UPDATE MODE" */
    char note[12];
    char diag[40];
    uint8_t diag_colour[2];     /* RGB565, high byte first */
    uint8_t colour_bars;
};

struct midi_control_ui_status {
    char title[17];
    char profile[17];           /* the live profile's name; empty while only one is in use */
    char channel[6];            /* "CH 1" */
    uint8_t usb;
    uint8_t storage;            /* enum midi_control_storage_state */
    uint8_t dirty;
    char warning[40];           /* running with a diag line */
    uint8_t warning_colour[2];
};

struct midi_control_ui_program {
    char label[8];              /* "12A" */
    char number[8];             /* "PC 45" */
    char bank[12];              /* "Bank 12" and "Preset A", shown when the host has sent no text */
    char preset[12];
    char line1[33];
    char line2[33];
};

struct midi_control_ui_knob {
    char label[10];             /* "CC 21"; empty when unmapped */
    uint8_t mode;               /* enum midi_control_encoder_mode */
    uint8_t value;              /* the absolute mode's 0..127 */
};

struct midi_control_ui_meter {
    char label[4];              /* "VOL", "EXP" */
    uint8_t mapped;
    uint8_t value;              /* the position, 0..127 */
};

struct midi_control_ui_switch {
    char name[8];               /* which switch: "A", "BANK UP" */
    char label[17];             /* what it does: "CC 69", "NOTE 60", the preset it recalls
                                 * ("12C"), or where a step lands (up arrow, "13B"); in the
                                 * profile picker, the name of the profile it chooses */
    uint8_t led;                /* MIDI_CONTROL_LED_* showing on the switch now */
    uint8_t active;
    uint8_t mapped;
};

/* The profile picker, drawn in the program region while it is open. */
struct midi_control_ui_picker {
    uint8_t open;
    char page[8];               /* "1/2": the page shown, of how many */
    char title[17];             /* the live profile's name */
    char hint[33];              /* what the switches do now */
};

struct midi_control_ui_view {
    uint8_t running;
    uint8_t display_off;        /* dark after the display timeout: nothing else is set */
    struct midi_control_ui_start start;
    struct midi_control_ui_status status;
    struct midi_control_ui_program program;
    struct midi_control_ui_picker picker;       /* open: drawn instead of the program */
    struct midi_control_ui_knob knobs[MIDI_CONTROL_UI_KNOBS];
    struct midi_control_ui_meter meters[2];
    struct midi_control_ui_switch switches[MIDI_CONTROL_UI_SWITCHES];
};

/* What is on the panel. Zero it, or clear `valid`, to have the next update
 * draw everything - after pointing the gfx at a different buffer, say. */
struct midi_control_ui_cache {
    uint8_t valid;
    struct midi_control_ui_view shown;
};

void midi_control_ui_view(struct midi_control_ui_view *v, const struct midi_control_ui_model *m);

/* Everything, from scratch. */
void midi_control_ui_draw(struct midi_control_gfx *g, const struct midi_control_ui_model *m);

/* Only the regions that differ from what `cache` says is shown. Returns 1 and
 * the rectangle that covers every pixel drawn, or 0 when nothing changed. */
int midi_control_ui_update(struct midi_control_gfx *g, const struct midi_control_ui_model *m,
                           struct midi_control_ui_cache *cache, struct midi_control_rect *drawn);

/* "12A" style: bank number (1-based) and preset letter within the bank. `out`
 * needs 8 bytes. */
void midi_control_ui_program_label(uint8_t program, uint8_t bank_size, char *out);

/* Decimal, no padding, NUL terminated; `out` needs 11 bytes. Here because the
 * firmware has no printf. */
void midi_control_fmt_uint(char *out, unsigned value);

#ifdef __cplusplus
}
#endif
#endif
