#ifndef MIDI_CONTROL_VERSION_H
#define MIDI_CONTROL_VERSION_H

/* The firmware and the app report this over the protocol's IDENTIFY reply,
 * and the app shows both so a mismatch is visible rather than mysterious. */
#define MIDI_CONTROL_VERSION_MAJOR 0
#define MIDI_CONTROL_VERSION_MINOR 2
#define MIDI_CONTROL_VERSION_PATCH 0

/* The name people see, written once for the C code: the USB manufacturer,
 * product and interface strings - so also the MIDI port the app looks for -
 * and the default device name, which is the LCD title. app/CMakeLists.txt
 * sets the app's own name; keep the two equal. Shorter than
 * MIDI_CONTROL_CONFIG_NAME_LEN (tests/test_shared.c). */
#define MIDI_CONTROL_PRODUCT_NAME "POGO"

#endif
