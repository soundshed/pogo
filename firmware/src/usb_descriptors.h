#ifndef MIDI_CONTROL_USB_DESCRIPTORS_H
#define MIDI_CONTROL_USB_DESCRIPTORS_H

/* Before tusb_init(): reads the part's unique ID and makes the serial number
 * string from it (usb_descriptors.c, docs/03 section 1). */
void usb_descriptors_init(void);

#endif
