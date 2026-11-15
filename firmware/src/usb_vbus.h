#ifndef MIDI_CONTROL_USB_VBUS_H
#define MIDI_CONTROL_USB_VBUS_H

#include <stdint.h>

/* USB0's VBUS: when the device goes on the bus, and a record of what the
 * controller saw, for the "phone sees nothing until the cable is reseated at
 * the POGO end" hazard (docs/09).
 *
 * usb_vbus_init() turns OTGSC VC and VD off, undoing TinyUSB 0.21's
 * dcd_init(), so BSV follows the host's VBUS (the board wires USB0_VBUS to the
 * socket, measured), and takes the device off the bus. usb_vbus_task() then
 * connects and disconnects by midi_control_usb_attach (shared/). The D+ pull-up
 * also needs USB0_VBUS above ~1.8 V in hardware (UM10503 Table 456).
 *
 * Polled, no OTGSC interrupt is enabled: TinyUSB's handler does not
 * acknowledge OTG status, so an enabled one would never clear. */

/* Packed state word, one per recorded change. */
#define USB_VBUS_ID        (1u << 0)    /* OTGSC ID   */
#define USB_VBUS_AVV       (1u << 1)    /* OTGSC AVV  */
#define USB_VBUS_ASV       (1u << 2)    /* OTGSC ASV  */
#define USB_VBUS_BSV       (1u << 3)    /* OTGSC BSV  */
#define USB_VBUS_BSE       (1u << 4)    /* OTGSC BSE  */
#define USB_VBUS_CCS       (1u << 5)    /* PORTSC1 CCS  */
#define USB_VBUS_SUSP      (1u << 6)    /* PORTSC1 SUSP */
#define USB_VBUS_HSP       (1u << 7)    /* PORTSC1 HSP  */
#define USB_VBUS_RS        (1u << 8)    /* USBCMD RS    */
#define USB_VBUS_MOUNTED   (1u << 9)    /* tud_mounted()   */
#define USB_VBUS_SUSPENDED (1u << 10)   /* tud_suspended() */

/* Right after tusb_init(): log OTGSC, PORTSC1 and USBCMD as they stand and
 * take the first sample. */
void usb_vbus_init(void);

/* From the main loop's slow pass, with the milliseconds since the previous
 * pass. Samples the registers, counts OTGSC's latched VBUS edges, and records
 * and logs any change in the packed state. */
void usb_vbus_task(uint32_t ms);

/* One line for the screen, so the state can be read when USB is what failed:
 * "USB P<phase> V<BSV> R<RS> C<CCS> M<mounted> S<suspended> N<reseats> K<connects>",
 * the counts modulo 100. The buffer is rewritten on each call. */
const char *usb_vbus_line(void);

/* The `usbvbus` result packet (vendor command 77). */
void usb_vbus_status(void);

/* Vendor command 78, value 1: VC and VD off for ~10 ms, OTGSC read, then
 * put back. A `vbusprobe` result packet says whether BSV held, i.e. whether
 * USB0_VBUS is fed by the host. If it is not, the pull-up drops for those
 * milliseconds and the host sees a disconnect and a reconnect. */
void usb_vbus_probe(void);

/* Vendor command 78, values 2..4: OTGSC's VBUS controls until the next power
 * cycle or VBUS loss. 2: VC and VD off, as start-up sets them. 3: VC on, as
 * TinyUSB leaves it; BSV then reads valid with no host. 4: VD on, as
 * board_usb_clock_init() sets it; BSV can then miss a PC's VBUS. */
void usb_vbus_otgsc(uint8_t value);

#endif
