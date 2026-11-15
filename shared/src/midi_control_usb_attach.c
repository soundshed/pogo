#include "midi_control/midi_control_usb_attach.h"

void midi_control_usb_attach_init(struct midi_control_usb_attach *a)
{
    a->phase = MIDI_CONTROL_USB_ATTACH_IDLE;
    a->tries = 0;
    a->ms = 0;
    a->reseats = 0;
}

static void enter(struct midi_control_usb_attach *a, uint8_t phase)
{
    a->phase = phase;
    a->ms = 0;
}

int midi_control_usb_attach_step(struct midi_control_usb_attach *a, int vbus, int configured,
                                 uint32_t ms)
{
    if (!vbus) {
        a->tries = 0;
        if (a->phase != MIDI_CONTROL_USB_ATTACH_IDLE)
            enter(a, MIDI_CONTROL_USB_ATTACH_IDLE);
        return 0;
    }

    a->ms = (a->ms + ms < a->ms) ? UINT32_MAX : a->ms + ms;

    switch (a->phase) {
    case MIDI_CONTROL_USB_ATTACH_IDLE:
        enter(a, MIDI_CONTROL_USB_ATTACH_WAIT);
        return 0;

    case MIDI_CONTROL_USB_ATTACH_WAIT:
        if (a->ms < MIDI_CONTROL_USB_ATTACH_DELAY_MS)
            return 0;
        enter(a, MIDI_CONTROL_USB_ATTACH_ON);
        return 1;

    case MIDI_CONTROL_USB_ATTACH_RESEAT:
        if (a->ms < MIDI_CONTROL_USB_ATTACH_OFF_MS)
            return 0;
        enter(a, MIDI_CONTROL_USB_ATTACH_ON);
        return 1;

    case MIDI_CONTROL_USB_ATTACH_ON:
        if (configured) {
            a->tries = 0;
            enter(a, MIDI_CONTROL_USB_ATTACH_CONFIGURED);
            return 1;
        }
        if (a->ms < MIDI_CONTROL_USB_ATTACH_CONFIGURE_MS)
            return 1;
        if (a->tries >= MIDI_CONTROL_USB_ATTACH_RETRIES) {
            enter(a, MIDI_CONTROL_USB_ATTACH_HOLD);
            return 1;
        }
        a->tries++;
        a->reseats++;
        enter(a, MIDI_CONTROL_USB_ATTACH_RESEAT);
        return 0;

    case MIDI_CONTROL_USB_ATTACH_CONFIGURED:
        if (!configured)
            enter(a, MIDI_CONTROL_USB_ATTACH_ON);
        return 1;

    case MIDI_CONTROL_USB_ATTACH_HOLD:
    default:
        if (configured) {
            a->tries = 0;
            enter(a, MIDI_CONTROL_USB_ATTACH_CONFIGURED);
        }
        return 1;
    }
}
