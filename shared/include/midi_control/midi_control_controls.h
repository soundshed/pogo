/* The POD Go's physical controls, named.
 *
 * This is the vocabulary the whole project shares: the firmware translates
 * matrix positions and ADC channels into these ids, the configuration is
 * indexed by them, the engine acts on them, and the emulator and the
 * management app draw them. Nothing outside the firmware's board layer knows a
 * pin or a column number.
 *
 * Physical layout (docs/01-hardware.md section 4): a 7-column matrix with three
 * switch rows and one encoder per column, plus two switches on their own pins
 * and two analog inputs.
 */
#ifndef MIDI_CONTROL_CONTROLS_H
#define MIDI_CONTROL_CONTROLS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_CONTROL_COLUMNS 7
#define MIDI_CONTROL_ROWS    3

/* Momentary switches. The order is the order the configuration stores them
 * in, so it must not change once a configuration format version is released;
 * add new ids at the end. */
enum midi_control_button {
    MIDI_CONTROL_BTN_FS_A = 0,      /* footswitch A            column 3, SWROW2 */
    MIDI_CONTROL_BTN_FS_B,          /* footswitch B            column 4, SWROW2 */
    MIDI_CONTROL_BTN_FS_C,          /* footswitch C            column 5, SWROW2 */
    MIDI_CONTROL_BTN_FS_D,          /* footswitch D            column 6, SWROW2 */
    MIDI_CONTROL_BTN_TAP,           /* Tap / Tuner footswitch  column 7, SWROW2 */
    MIDI_CONTROL_BTN_BANK_UP,       /* Bank Up footswitch      column 1, SWROW2 */
    MIDI_CONTROL_BTN_BANK_DOWN,     /* Bank Down footswitch    column 2, SWROW2 */
    MIDI_CONTROL_BTN_VIEW,          /* top panel               column 1, SWROW1 */
    MIDI_CONTROL_BTN_BYPASS,        /*                         column 2, SWROW1 */
    MIDI_CONTROL_BTN_MENU,          /*                         column 3, SWROW1 */
    MIDI_CONTROL_BTN_MODE,          /*                         column 4, SWROW1 */
    MIDI_CONTROL_BTN_HOME_ENC,      /* Home encoder push       column 6, SWROW1 */
    MIDI_CONTROL_BTN_MENU_ENC,      /* Menu encoder push       column 7, SWROW1 */
    MIDI_CONTROL_BTN_ENC_1,         /* parameter encoder 1 push  column 1, SWROW3 */
    MIDI_CONTROL_BTN_ENC_2,         /* column 2, SWROW3 */
    MIDI_CONTROL_BTN_ENC_3,         /* column 3, SWROW3 */
    MIDI_CONTROL_BTN_ENC_4,         /* column 4, SWROW3 */
    MIDI_CONTROL_BTN_ENC_5,         /* column 5, SWROW3 */
    MIDI_CONTROL_BTN_TOE,           /* expression pedal toe switch, own pin */
    MIDI_CONTROL_BTN_PAGE,          /* PAGE > button, own pin (also the DFU strap) */
    MIDI_CONTROL_BUTTON_COUNT
};

enum midi_control_encoder {
    MIDI_CONTROL_ENC_1 = 0,         /* leftmost parameter encoder, column 1 */
    MIDI_CONTROL_ENC_2,
    MIDI_CONTROL_ENC_3,
    MIDI_CONTROL_ENC_4,
    MIDI_CONTROL_ENC_5,
    MIDI_CONTROL_ENC_HOME,          /* column 6 - one count per detent in hardware */
    MIDI_CONTROL_ENC_MENU,          /* column 7 - one count per detent in hardware */
    MIDI_CONTROL_ENCODER_COUNT
};

enum midi_control_analog {
    MIDI_CONTROL_ANALOG_VOLUME = 0, /* main volume knob, ADC0 channel 0 */
    MIDI_CONTROL_ANALOG_PEDAL,      /* expression pedal, ADC channel 7 */
    MIDI_CONTROL_ANALOG_COUNT
};

/* LED colour bits, one byte per matrix column. Bit 0 red, bit 1 green, bit 2
 * blue, so 0 is off and 7 is white. On column 7 the three bits are three
 * separate lamps - Vol (red), Wah (green) and Tap Tempo (blue bit) - rather
 * than one RGB LED. */
#define MIDI_CONTROL_LED_OFF     0u
#define MIDI_CONTROL_LED_RED     1u
#define MIDI_CONTROL_LED_GREEN   2u
#define MIDI_CONTROL_LED_YELLOW  3u
#define MIDI_CONTROL_LED_BLUE    4u
#define MIDI_CONTROL_LED_MAGENTA 5u
#define MIDI_CONTROL_LED_CYAN    6u
#define MIDI_CONTROL_LED_WHITE   7u

#define MIDI_CONTROL_LAMP_VOL    1u   /* column 7 only */
#define MIDI_CONTROL_LAMP_WAH    2u
#define MIDI_CONTROL_LAMP_TAP    4u

/* The button at a matrix position, or -1 where nothing is fitted. Columns and
 * rows are zero-based: col 0..6, row 0 = SWROW1, 1 = SWROW2, 2 = SWROW3. */
int midi_control_button_at(unsigned col, unsigned row);

/* The matrix column a button's LED lives in, or -1 for buttons without one.
 * Only the SWROW2 footswitches have LEDs. */
int midi_control_button_led_column(enum midi_control_button b);

/* Human-readable names, for the app and for logs. Never NULL. */
const char *midi_control_button_name(enum midi_control_button b);
const char *midi_control_encoder_name(enum midi_control_encoder e);
const char *midi_control_analog_name(enum midi_control_analog a);
const char *midi_control_led_name(unsigned column);

#ifdef __cplusplus
}
#endif
#endif
