/* When the USB device should be on the bus (docs/09, "A USB-C phone can see
 * nothing until the cable is reseated at the POGO end").
 *
 * The board layer feeds this the host's VBUS as the controller sees it (OTGSC
 * BSV), whether the host has configured the device, and the milliseconds since
 * the last call; it answers whether the D+ pull-up should be on, and the board
 * layer connects or disconnects to match.
 *
 *   No VBUS                 off. VBUS gone resets everything below.
 *   VBUS appears            off for ATTACH_DELAY_MS, then on: the host's
 *                           controller is up before the device appears.
 *   On, not configured      after CONFIGURE_MS, off for OFF_MS and on again,
 *                           a reseat done in software; at most RETRIES times
 *                           for one VBUS session, then left on.
 *   Configured              stays on. If the host drops the configuration
 *                           (a bus reset) the configure wait starts again,
 *                           with the retries renewed.
 *
 * Measured 2026-11-15 on a Galaxy S21: with the device attached the moment
 * VBUS rose, the phone reset it about 100 times in 110 s and never configured
 * it; a reseat of the cable at the device end fixed it at once. That a delay
 * avoids it is derived, not yet measured; the retry is the reseat itself. */
#ifndef MIDI_CONTROL_USB_ATTACH_H
#define MIDI_CONTROL_USB_ATTACH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_CONTROL_USB_ATTACH_DELAY_MS     1000u
#define MIDI_CONTROL_USB_ATTACH_CONFIGURE_MS 5000u
#define MIDI_CONTROL_USB_ATTACH_OFF_MS        500u
#define MIDI_CONTROL_USB_ATTACH_RETRIES         3u

enum midi_control_usb_attach_phase {
    MIDI_CONTROL_USB_ATTACH_IDLE = 0,    /* no VBUS: off */
    MIDI_CONTROL_USB_ATTACH_WAIT,        /* VBUS, the delay running: off */
    MIDI_CONTROL_USB_ATTACH_ON,          /* on, waiting to be configured */
    MIDI_CONTROL_USB_ATTACH_RESEAT,      /* off for a retry */
    MIDI_CONTROL_USB_ATTACH_CONFIGURED,  /* on, configured */
    MIDI_CONTROL_USB_ATTACH_HOLD         /* on, retries spent; no more until VBUS goes */
};

struct midi_control_usb_attach {
    uint8_t phase;          /* enum midi_control_usb_attach_phase */
    uint8_t tries;          /* reseats in this VBUS session since the last configuration */
    uint32_t ms;            /* in the current phase, saturating */
    uint32_t reseats;       /* in all, for the status packet */
};

void midi_control_usb_attach_init(struct midi_control_usb_attach *a);

/* One step. Returns 1 when the pull-up should be on, 0 when off. */
int midi_control_usb_attach_step(struct midi_control_usb_attach *a, int vbus, int configured,
                                 uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif
