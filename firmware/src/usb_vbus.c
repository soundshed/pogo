/* USB0 and VBUS: when the device goes on the bus, and what the controller saw
 * (usb_vbus.h). */

#include <stdint.h>
#include "usb_vbus.h"
#include "board.h"
#include "log.h"
#include "result.h"
#include "tusb.h"
#include "device/dcd.h"
#include "midi_control/midi_control_usb_attach.h"

#define REG(a) (*(volatile uint32_t *)(a))

#define USB0_USBCMD   REG(0x40006140u)
#define USB0_PORTSC1  REG(0x40006184u)
#define USB0_OTGSC    REG(0x400061A4u)

/* OTGSC (UM10503 Table 487). Bits 8..12 are the debounced VBUS/ID inputs;
 * 16..22 latch on a change whether or not the matching enable (24..30) is set,
 * and clear on a write of 1. */
#define OTGSC_INPUTS_SHIFT 8u
#define OTGSC_INPUTS_MASK  0x1Fu            /* ID AVV ASV BSV BSE */
#define OTGSC_AVVIS        (1u << 17)
#define OTGSC_ASVIS        (1u << 18)
#define OTGSC_BSVIS        (1u << 19)
#define OTGSC_BSEIS        (1u << 20)
#define OTGSC_VBUS_LATCHES (OTGSC_AVVIS | OTGSC_ASVIS | OTGSC_BSVIS | OTGSC_BSEIS)
/* What a write must carry back unchanged: the controls (0..7) less HADP (6),
 * which starts a data pulse on a 1, and the interrupt enables (24..30). */
#define OTGSC_KEEP         0x7F0000BFu

#define PORTSC1_CCS   (1u << 0)
#define PORTSC1_SUSP  (1u << 7)
#define PORTSC1_HSP   (1u << 9)
#define USBCMD_RS     (1u << 0)

#define EVENTS     8u          /* the last eight changes, oldest overwritten */
#define LOG_EVENTS 24u         /* changes logged before going quiet: the log does not wrap */

struct usb_vbus {
    uint32_t otgsc_boot, portsc_boot, usbcmd_boot;
    uint32_t otgsc, portsc;    /* the last sample, raw */
    uint32_t state;            /* the last sample, packed */
    uint32_t uptime_ms;        /* since usb_vbus_init(), from the main loop's clock */
    uint32_t bsv_up, bsv_down, ccs_up, ccs_down;
    uint32_t avvis, asvis, bsvis, bseis;
    uint32_t changes;
    uint32_t restarts;         /* controller resets on VBUS loss */
    uint32_t event[EVENTS];    /* (seconds << 12) | state */
};

static struct usb_vbus g_v MIDI_CONTROL_LOCAL;   /* no DMA reads it */

#define LOG_ATTACH 40u         /* connects and disconnects logged before going quiet */

static struct midi_control_usb_attach g_attach MIDI_CONTROL_LOCAL;
static int g_on;               /* what the pull-up was last set to: RS */
static uint32_t g_attach_logged;

static uint32_t pack(uint32_t otgsc, uint32_t portsc, uint32_t usbcmd)
{
    uint32_t s = (otgsc >> OTGSC_INPUTS_SHIFT) & OTGSC_INPUTS_MASK;
    if (portsc & PORTSC1_CCS)  s |= USB_VBUS_CCS;
    if (portsc & PORTSC1_SUSP) s |= USB_VBUS_SUSP;
    if (portsc & PORTSC1_HSP)  s |= USB_VBUS_HSP;
    if (usbcmd & USBCMD_RS)    s |= USB_VBUS_RS;
    if (tud_mounted())         s |= USB_VBUS_MOUNTED;
    if (tud_suspended())       s |= USB_VBUS_SUSPENDED;
    return s;
}

static void record(uint32_t state)
{
    const uint32_t old = g_v.state;
    if ((state ^ old) & USB_VBUS_BSV) {
        if (state & USB_VBUS_BSV)
            g_v.bsv_up++;
        else
            g_v.bsv_down++;
    }
    if ((state ^ old) & USB_VBUS_CCS) {
        if (state & USB_VBUS_CCS)
            g_v.ccs_up++;
        else
            g_v.ccs_down++;
    }

    g_v.state = state;
    g_v.event[g_v.changes % EVENTS] = ((g_v.uptime_ms / 1000u) << 12) | (state & 0xFFFu);
    g_v.changes++;

    if (g_v.changes <= LOG_EVENTS) {
        log_dec("usbvbus: change at ms", g_v.uptime_ms);
        log_hex("  state (BSV 008 CCS 020 RS 100 MOUNTED 200)", state);
        log_hex("  OTGSC", g_v.otgsc);
        log_hex("  PORTSC1", g_v.portsc);
    } else if (g_v.changes == LOG_EVENTS + 1u) {
        log_line("usbvbus: more changes, no longer logged (result --run 77 has the last eight)");
    }
}

static uint32_t sample(void)
{
    const uint32_t otgsc = USB0_OTGSC;
    const uint32_t latched = otgsc & OTGSC_VBUS_LATCHES;

    /* A VBUS edge that came and went between two samples still shows here. */
    if (latched) {
        if (latched & OTGSC_AVVIS) g_v.avvis++;
        if (latched & OTGSC_ASVIS) g_v.asvis++;
        if (latched & OTGSC_BSVIS) g_v.bsvis++;
        if (latched & OTGSC_BSEIS) g_v.bseis++;
        USB0_OTGSC = (otgsc & OTGSC_KEEP) | latched;
    }

    g_v.otgsc = otgsc;
    g_v.portsc = USB0_PORTSC1;
    return pack(otgsc, g_v.portsc, USB0_USBCMD);
}

/* OTGSC's VBUS controls. TinyUSB 0.21's dcd_init() tests the option with
 * #ifdef while tusb_option.h always defines it (to 0), so it always writes
 * VC | OT and overwrites board_usb_clock_init()'s VD | OT. Measured: OTGSC read
 * 0x002D2D0A at attach on 2026-11-15. */
#define OTGSC_VD        (1u << 0)
#define OTGSC_VC        (1u << 1)
#define OTGSC_OT        (1u << 3)
#define OTGSC_ENABLES   0x7F000000u
#define PROBE_ITERATIONS 400000u   /* board_delay_crude: ~10 ms at 204 MHz, ~20 ms at 96 */

static void otgsc_controls(uint32_t controls)
{
    log_hex("usbvbus: writing USB0 OTGSC controls", controls);
    USB0_OTGSC = (USB0_OTGSC & OTGSC_ENABLES) | controls;
}

void usb_vbus_init(void)
{
    /* The same three registers board_usb_clock_init() and TinyUSB have just
     * written, so a read cannot be the first touch; named anyway, and later
     * samples read nothing these have not. */
    log_line("usbvbus: reading USB0 OTGSC, PORTSC1, USBCMD");
    g_v.otgsc_boot = USB0_OTGSC;
    g_v.portsc_boot = USB0_PORTSC1;
    g_v.usbcmd_boot = USB0_USBCMD;
    log_hex("  OTGSC at attach", g_v.otgsc_boot);
    log_hex("  PORTSC1 at attach", g_v.portsc_boot);
    log_hex("  USBCMD at attach", g_v.usbcmd_boot);
    log_line(g_v.otgsc_boot & (1u << 11)
             ? "  BSV set: USB0_VBUS is above the B-session threshold"
             : "  BSV clear: USB0_VBUS reads no host power");

    /* TinyUSB 0.21's dcd_init() has just set VC (see usb_vbus_otgsc()). With
     * it set, USB0_VBUS reads valid with no host, so BSV says nothing, and the
     * part charges its VBUS pin, which the board wires to the socket (measured
     * 2026-11-15). A Galaxy S21 never attached as host to a POGO-end-first
     * cable with VC set (three times) and did with it clear (docs/09).
     *
     * VD stays clear too. With VD set (board_usb_clock_init()'s value) BSV
     * chattered and, after a PC was unplugged, did not come back when it was
     * plugged in again, while a phone's VBUS still read valid: the pin is
     * behind a divider (AVV never set on a PC) and VD's discharge resistor
     * loads it to about the B-session threshold (measured 2026-11-15; the
     * load is reported as ~1.5 k on NXP's forum). The pin's own 64 k
     * pull-down (UM10503 Table 456) is left to discharge it. */
    otgsc_controls(OTGSC_OT);

    /* Off the bus until midi_control_usb_attach says otherwise: the first
     * connect comes a delay after VBUS, as every later one does. */
    tud_disconnect();
    g_on = 0;
    midi_control_usb_attach_init(&g_attach);
    log_line("usbvbus: VC and VD off; pull-up off until VBUS has been up 1 s");

    /* The first sample is a record but not an edge. */
    g_v.state = sample();
    record(g_v.state);
}

/* VBUS gone: start the next session from a controller as fresh as at boot.
 *
 * Taken off the bus by RS alone after a Galaxy S21 had the device configured
 * (and, Android being Android, probably suspended), the next host, a Windows
 * PC, saw it attach and reported "Port Reset Failed" twice running, and a
 * reseat did not help; only a power cycle did (measured 2026-11-15). A build
 * that never cleared RS went phone -> PC without trouble. So the controller
 * is reset and set up again exactly as tusb_init() does, and the stack told
 * the device was unplugged, which it otherwise never hears: tud_mounted()
 * still read 1 after the phone had gone. dcd_init() waits for USBCMD RST
 * without a bound, as it does at every boot; the reset self-clears. With no
 * VBUS the pull-up is off in hardware whatever RS says. */
static void controller_restart(void)
{
    log_line("usbvbus: VBUS gone, USB0 USBCMD RST and dcd_init() again");
    dcd_int_disable(0);
    const tusb_rhport_init_t init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO };
    (void)dcd_init(0, &init);
    otgsc_controls(OTGSC_OT);
    dcd_disconnect(0);
#if MIDI_CONTROL_FORCE_FULL_SPEED
    board_usb_force_full_speed();
#endif
    dcd_event_bus_signal(0, DCD_EVENT_UNPLUGGED, false);
    dcd_int_enable(0);
    g_v.restarts++;
}

static void attach_task(uint32_t ms)
{
    const uint8_t was = g_attach.phase;
    const int on = midi_control_usb_attach_step(&g_attach, (g_v.state & USB_VBUS_BSV) != 0,
                                                tud_mounted(), ms);
    if (was != MIDI_CONTROL_USB_ATTACH_IDLE && g_attach.phase == MIDI_CONTROL_USB_ATTACH_IDLE) {
        controller_restart();
        g_on = 0;
        return;
    }
    if (on == g_on)
        return;

    g_on = on;
    if (on)
        tud_connect();
    else
        tud_disconnect();

    if (g_attach_logged < LOG_ATTACH) {
        g_attach_logged++;
        log_dec(on ? "usb attach: pull-up on, phase" : "usb attach: pull-up off, phase",
                g_attach.phase);
        log_dec("  at ms", g_v.uptime_ms);
    }
}

void usb_vbus_task(uint32_t ms)
{
    g_v.uptime_ms += ms;

    const uint32_t state = sample();
    if (state != g_v.state)
        record(state);
    attach_task(ms);
}

void usb_vbus_probe(void)
{
    log_line("usbvbus: probe, VC and VD off for ~10 ms, reading USB0 OTGSC");
    const uint32_t before = USB0_OTGSC;
    otgsc_controls(OTGSC_OT);
    board_delay_crude(PROBE_ITERATIONS);
    const uint32_t during = USB0_OTGSC;
    const uint32_t during_portsc = USB0_PORTSC1;
    otgsc_controls(before & 0xBFu);          /* whatever was there, less HADP */
    board_delay_crude(PROBE_ITERATIONS);
    const uint32_t after = USB0_OTGSC;
    log_hex("  OTGSC before", before);
    log_hex("  OTGSC with OT only", during);
    log_hex("  OTGSC after", after);

    result_begin("vbusprobe");
    result_put("before", before);
    result_put("during", during);
    result_put("portsc", during_portsc);
    result_put("after", after);
    result_put("bsv_novc", (during >> 11) & 1u);
    result_put("asv_novc", (during >> 10) & 1u);
    result_put("avv_novc", (during >> 9) & 1u);
    result_note((during >> 11) & 1u ? "BSV held: host VBUS reaches the pin"
                                    : "BSV fell: pin not fed by the host");
    result_emit();
}

void usb_vbus_otgsc(uint8_t value)
{
    static const uint32_t controls[3] = {
        OTGSC_OT,                   /* 2: start-up's setting */
        OTGSC_VC | OTGSC_OT,        /* 3: TinyUSB's */
        OTGSC_VD | OTGSC_OT,        /* 4: board_usb_clock_init()'s */
    };
    if (value < 2u || value > 4u)
        return;
    otgsc_controls(controls[value - 2u]);
}

static char *put_field(char *p, char tag, uint32_t value)
{
    *p++ = ' ';
    *p++ = tag;
    value %= 100u;
    if (value >= 10u)
        *p++ = (char)('0' + value / 10u);
    *p++ = (char)('0' + value % 10u);
    return p;
}

const char *usb_vbus_line(void)
{
    static char line[40];
    const uint32_t s = g_v.state;
    char *p = line;
    *p++ = 'U'; *p++ = 'S'; *p++ = 'B';
    p = put_field(p, 'P', g_attach.phase);
    p = put_field(p, 'V', (s & USB_VBUS_BSV) ? 1u : 0u);
    p = put_field(p, 'R', (s & USB_VBUS_RS) ? 1u : 0u);
    p = put_field(p, 'C', (s & USB_VBUS_CCS) ? 1u : 0u);
    p = put_field(p, 'M', (s & USB_VBUS_MOUNTED) ? 1u : 0u);
    p = put_field(p, 'S', (s & USB_VBUS_SUSPENDED) ? 1u : 0u);
    p = put_field(p, 'N', g_attach.reseats);
    p = put_field(p, 'K', g_v.ccs_up);
    p = put_field(p, 'X', g_v.restarts);
    *p = '\0';
    return line;
}

void usb_vbus_status(void)
{
    result_begin("usbvbus");
    result_put("otgsc0", g_v.otgsc_boot);
    result_put("portsc0", g_v.portsc_boot);
    result_put("otgsc", g_v.otgsc);
    result_put("portsc", g_v.portsc);
    result_put("state", g_v.state);
    result_put("up_s", g_v.uptime_ms / 1000u);
    result_put("bsvup", g_v.bsv_up);
    result_put("bsvdn", g_v.bsv_down);
    result_put("ccsup", g_v.ccs_up);
    result_put("ccsdn", g_v.ccs_down);
    result_put("bsvis", g_v.bsvis);
    result_put("bseis", g_v.bseis);
    result_put("avvis", g_v.avvis);
    result_put("changes", g_v.changes);
    result_put("attach", g_attach.phase);
    result_put("reseats", g_attach.reseats);
    result_put("restarts", g_v.restarts);

    /* Newest first: e0 is the latest change. Seconds in the top five hex
     * digits, the packed state in the bottom three. */
    static const char names[EVENTS][3] = { "e0", "e1", "e2", "e3", "e4", "e5", "e6", "e7" };
    const uint32_t n = g_v.changes < EVENTS ? g_v.changes : EVENTS;
    for (uint32_t i = 0; i < n; i++)
        result_put(names[i], g_v.event[(g_v.changes - 1u - i) % EVENTS]);
    result_emit();
}
