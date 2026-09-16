#include "midi_control/midi_control_controls.h"

/* [column][row]; rows are SWROW1, SWROW2, SWROW3. Column 5 has no SWROW1
 * switch and columns 6 and 7 have no SWROW3 switch - those positions exist in
 * the matrix with nothing fitted (docs/01-hardware.md section 4). */
static const int8_t k_matrix[MIDI_CONTROL_COLUMNS][MIDI_CONTROL_ROWS] = {
    { MIDI_CONTROL_BTN_VIEW,     MIDI_CONTROL_BTN_BANK_UP,   MIDI_CONTROL_BTN_ENC_1 },
    { MIDI_CONTROL_BTN_BYPASS,   MIDI_CONTROL_BTN_BANK_DOWN, MIDI_CONTROL_BTN_ENC_2 },
    { MIDI_CONTROL_BTN_MENU,     MIDI_CONTROL_BTN_FS_A,      MIDI_CONTROL_BTN_ENC_3 },
    { MIDI_CONTROL_BTN_MODE,     MIDI_CONTROL_BTN_FS_B,      MIDI_CONTROL_BTN_ENC_4 },
    { -1,                        MIDI_CONTROL_BTN_FS_C,      MIDI_CONTROL_BTN_ENC_5 },
    { MIDI_CONTROL_BTN_HOME_ENC, MIDI_CONTROL_BTN_FS_D,      -1                     },
    { MIDI_CONTROL_BTN_MENU_ENC, MIDI_CONTROL_BTN_TAP,       -1                     },
};

int midi_control_button_at(unsigned col, unsigned row)
{
    if (col >= MIDI_CONTROL_COLUMNS || row >= MIDI_CONTROL_ROWS)
        return -1;
    return k_matrix[col][row];
}

int midi_control_button_led_column(enum midi_control_button b)
{
    switch (b) {
    case MIDI_CONTROL_BTN_BANK_UP:   return 0;
    case MIDI_CONTROL_BTN_BANK_DOWN: return 1;
    case MIDI_CONTROL_BTN_FS_A:      return 2;
    case MIDI_CONTROL_BTN_FS_B:      return 3;
    case MIDI_CONTROL_BTN_FS_C:      return 4;
    case MIDI_CONTROL_BTN_FS_D:      return 5;
    case MIDI_CONTROL_BTN_TAP:       return 6;
    default:                         return -1;
    }
}

static const char *const k_button_names[MIDI_CONTROL_BUTTON_COUNT] = {
    "Footswitch A", "Footswitch B", "Footswitch C", "Footswitch D",
    "Tap / Tuner", "Bank Up", "Bank Down",
    "View", "Bypass", "Menu", "Mode",
    "Home encoder push", "Menu encoder push",
    "Encoder 1 push", "Encoder 2 push", "Encoder 3 push",
    "Encoder 4 push", "Encoder 5 push",
    "Pedal toe switch", "Page",
};

static const char *const k_encoder_names[MIDI_CONTROL_ENCODER_COUNT] = {
    "Encoder 1", "Encoder 2", "Encoder 3", "Encoder 4", "Encoder 5",
    "Home encoder", "Menu encoder",
};

static const char *const k_analog_names[MIDI_CONTROL_ANALOG_COUNT] = {
    "Volume knob", "Expression pedal",
};

static const char *const k_led_names[MIDI_CONTROL_COLUMNS] = {
    "Bank Up LED", "Bank Down LED", "A LED", "B LED", "C LED", "D LED",
    "Vol / Wah / Tap lamps",
};

const char *midi_control_button_name(enum midi_control_button b)
{
    return (unsigned)b < MIDI_CONTROL_BUTTON_COUNT ? k_button_names[b] : "?";
}

const char *midi_control_encoder_name(enum midi_control_encoder e)
{
    return (unsigned)e < MIDI_CONTROL_ENCODER_COUNT ? k_encoder_names[e] : "?";
}

const char *midi_control_analog_name(enum midi_control_analog a)
{
    return (unsigned)a < MIDI_CONTROL_ANALOG_COUNT ? k_analog_names[a] : "?";
}

const char *midi_control_led_name(unsigned column)
{
    return column < MIDI_CONTROL_COLUMNS ? k_led_names[column] : "?";
}
