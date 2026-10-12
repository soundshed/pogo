/* POD Go board support: USB0 clock/PHY bring-up, and the footswitch LEDs as a
 * status channel.
 *
 * Everything here is measured rather than assumed. The clock sequence and
 * peripheral setup have been run from this code and verified on hardware; see
 * firmware/usb-probe and docs/10 section 1.
 */

#include <stdint.h>
#include "board.h"

#define REG(a) (*(volatile uint32_t *)(a))

/* ---- CGU / CCU1 / CREG ------------------------------------------------- */
#define CGU_BASE          0x40050000u
#define PLL0USB_STAT      REG(CGU_BASE + 0x1C)
#define PLL0USB_CTRL      REG(CGU_BASE + 0x20)
#define PLL0USB_MDIV      REG(CGU_BASE + 0x24)
#define PLL0USB_NP_DIV    REG(CGU_BASE + 0x28)
#define BASE_USB0_CLK     REG(CGU_BASE + 0x60)

#define CCU1_BASE         0x40051000u
#define CLK_M4_USB0_CFG   REG(CCU1_BASE + 0x428)
#define CLK_M4_USB0_STAT  REG(CCU1_BASE + 0x42C)

#define CREG0             REG(0x40043004u)
#define CREG0_USB0PHY     (1u << 5)          /* 0 = PHY enabled */

/* 12 MHz crystal -> 480 MHz. The divider values this crystal requires,
 * established by observation and confirmed against the LPC4350 clock chapter. */
#define V_NP_DIV          0x00302062u
#define V_MDIV            0x06167FFAu
#define V_CTRL            0x0600001Cu        /* CLK_SEL=XTAL, DIRECTI|DIRECTO|CLKEN */
#define V_BASE_USB0       0x07000800u        /* CLK_SEL=PLL0USB, AUTOBLOCK, powered */

/* ---- USB0 -------------------------------------------------------------- */
#define USB0_BASE         0x40006000u
#define USB0_SBUSCFG      REG(USB0_BASE + 0x090)
#define USB0_USBCMD       REG(USB0_BASE + 0x140)
#define USB0_OTGSC        REG(USB0_BASE + 0x1A4)
#define USB0_USBMODE      REG(USB0_BASE + 0x1A8)

/* Bounded, for the reason given in firmware/usb-probe: an unbounded spin on a
 * bit that never sets hangs the unit and says nothing about which stage failed. */
#define POLL_LIMIT 1000000u

/* How long to let a column line settle before sampling the rows.
 *
 * board_delay_crude costs roughly 5 cycles an iteration, so ~19 iterations per
 * microsecond at the ~96 MHz the boot ROM leaves and ~40 at the 204 MHz the EMC
 * stage raises the core to. The scan runs after that stage, so 10000 is about
 * 250 us in practice (derived from the clock ratio, not timed), and the scan is
 * measured working there. Treat every count in this file as a minimum.
 *
 * This is not a guessed margin. An earlier version waited 200 iterations (~10
 * us) and the scan never once saw a closed switch, while a column held
 * permanently high was detected immediately - the exact signature of sampling
 * before the line has settled. Reliable scans need hundreds of microseconds,
 * not tens, which points at deliberate RC filtering on the switch lines.
 *
 * Seven columns at 250-500 us plus the discharge is 2-4 ms a scan, i.e. 250-500
 * Hz, far faster than a foot. */
#define SETTLE_ITERATIONS    10000u
#define DISCHARGE_ITERATIONS  4000u

static int poll_until(volatile uint32_t *reg, uint32_t mask, uint32_t want)
{
    for (uint32_t i = 0; i < POLL_LIMIT; i++)
        if ((*reg & mask) == want)
            return 1;
    return 0;
}

int board_usb_clock_init(void)
{
    int ok = 1;

    /* The ROM leaves this branch running - measured, all six LEDs lit on page 1
     * of the probe - which is why the update-mode loader gets away with never
     * enabling it.
     * Enabling and waiting anyway costs nothing and turns an assumption into a
     * guarantee, and no USB0 register may be touched before it holds. */
    CLK_M4_USB0_CFG |= 1u;
    ok &= poll_until(&CLK_M4_USB0_STAT, 1u, 1u);

    PLL0USB_CTRL |= 1u;                       /* PD = 1, power down to reprogram */
    PLL0USB_NP_DIV = V_NP_DIV;
    PLL0USB_MDIV = V_MDIV;
    PLL0USB_CTRL = V_CTRL;                    /* PD = 0, run */
    ok &= poll_until(&PLL0USB_STAT, 1u, 1u);  /* LOCK */

    BASE_USB0_CLK = V_BASE_USB0;
    CREG0 &= ~CREG0_USB0PHY;                  /* PHY on */

    USB0_USBCMD &= ~1u;                       /* RS = 0, stop */
    USB0_USBCMD = 2u;                         /* RST = 1 */
    ok &= poll_until(&USB0_USBCMD, 2u, 0u);   /* reset self-clears */

    USB0_SBUSCFG = 7u;                        /* AHB burst config, as the hardware requires */
    USB0_USBMODE = 0x0Au;                     /* device mode + setup lockout off */
    USB0_OTGSC = 9u;

    return ok;
}

/* ---- the UI matrix ----------------------------------------------------- */
/* One set of column lines serves both the LEDs and the switches, so the scan
 * loop handles LEDs and inputs in a single pass (docs/06). Driving a column high lights that switch's LED white,
 * because the RGB row lines idle active while we are not driving the SCT
 * (docs/06), and simultaneously presents that column's switches on the rows.
 *
 * All seven columns are established and confirmed on hardware: driving PF_1
 * blinks the Vol/Wah/Tap LED. */
#define SCU_BASE  0x40086000u
#define GPIO_DIR  0x400F6000u
#define GPIO_SET  0x400F6200u
#define GPIO_CLR  0x400F6280u
#define GPIO_PIN  0x400F6100u

struct pin {
    uint8_t scu_port, scu_pin, func, gpio_port, gpio_bit;
};

static const struct pin k_columns[BOARD_COLUMNS] = {
    { 13, 11, 4, 6, 25 },      /* COL1  Bank Up          */
    {  9,  4, 4, 5, 17 },      /* COL2  Bank Down        */
    {  1, 20, 0, 0, 15 },      /* COL3  F/S "A"          */
    { 13, 12, 4, 6, 26 },      /* COL4  F/S "B"          */
    {  1, 18, 0, 0, 13 },      /* COL5  F/S "C"          */
    {  1, 15, 0, 0,  2 },      /* COL6  F/S "D"          */
    { 15,  1, 4, 7, 16 },      /* COL7  Vol/Wah/Tap      */
};

/* The three switch rows and the two encoder phase lines. All five are inputs
 * with pull-downs, so a column drives *high* to select and a closed switch or
 * an asserted encoder phase reads back high. The required SFS values are 0x58
 * for the FUNC0 pins and 0x5c for the FUNC4 pins (docs/06); the low bits are
 * the function, so ORing the func in gives the same values. */
static const struct pin k_rows[BOARD_ROWS] = {
    { 10,  1, 0, 4,  8 },      /* SWROW1  PA_1  */
    { 10,  2, 0, 4,  9 },      /* SWROW2  PA_2  */
    { 15,  2, 4, 7, 17 },      /* SWROW3  PF_2, the five encoder buttons */
};

/* ENC_A and ENC_B are multiplexed across the columns exactly like the switches
 * - one encoder per column, seven in total. */
static const struct pin k_enc[2] = {
    { 15, 10, 4, 7, 24 },      /* ENC_A  PF_10 */
    { 15, 11, 4, 7, 25 },      /* ENC_B  PF_11 */
};

#define SFS_INPUT_PULLDOWN  0x58u   /* EPD | EPUN | EZI, func in low bits */

/* The three RGB row lines, driven as plain GPIO for now rather than through the
 * SCT.
 *
 * The PWM mapping is MATCH3 -> CTOUT_13 red, MATCH1 -> CTOUT_8 green and
 * MATCH2 -> CTOUT_11 blue (docs/06), which gives full colour. On/off per
 * channel gives seven colours plus off, needs none of the SCT event and output
 * configuration, and proves the muxing, timing and colour order first. PWM is
 * then a contained upgrade rather than a pile of new things failing together.
 *
 * Pin assignment is from the schematic: RED on PD_13, GREEN on PD_15, BLUE on
 * PD_14. That schematic has been wrong twice already, but the failure mode here
 * is benign and obvious - the three lines are certainly the RGB rows, so the
 * worst case is permuted colours, visible instantly and fixed by reordering
 * this table. */
static const struct pin k_rgb[3] = {
    { 13, 13, 4, 6, 27 },      /* RED    PD_13 / GPIO6[27] */
    { 13, 15, 4, 6, 29 },      /* GREEN  PD_15 / GPIO6[29] */
    { 13, 14, 4, 6, 28 },      /* BLUE   PD_14 / GPIO6[28] */
};

/* Which level lights an LED. Measured: active HIGH.
 *
 * Two readings of the evidence disagreed. docs/06 records the LEDs glowing white with
 * a column driven and the rows left undriven, which argues the rows idle
 * conducting and so are active low. But the SCT config sets each output
 * at its MATCH and clears it at the period, so on-time grows with the requested
 * value, which argues active high.
 *
 * The SCT reading was the right one. Driving a test pattern of seven distinct
 * colours settled it in a single load: every column came back as the exact
 * bitwise complement of what was asked for, and the three columns whose
 * complement was a single bit showed pure red, green and blue in the order this
 * table already had them. So the pin order was right all along and only the
 * polarity was inverted; that white was undriven rows floating high, not
 * idling conducting. */
#define LED_ACTIVE_LOW 0

/* Switches that are not in the column matrix at all - each has its own pin and
 * is read outside the strobe.
 *
 * Both are active low: their SFS values (0x40 and 0x44) set EZI but leave both
 * EPD and EPUN clear, which leaves the internal pull-up enabled. A pull-up only
 * makes sense with a switch closing to ground. Every matrix row is the opposite
 * - SFS 0x58, pull-down on, closed reads high.
 *
 * PAGE is also the DFU boot strap, which is why the schematic calls the net
 * PAGE/DFU. Reading it is harmless; it only matters at power-on. */
struct direct_switch {
    uint8_t scu_port, scu_pin, sfs, gpio_port, gpio_bit, active_low;
};

/* Polarity is per switch, and measured rather than derived.
 *
 * Both SFS values leave the internal pull-up enabled, which argues both should
 * be active low - and that is right for the toe switch. It is wrong for PAGE:
 * on hardware that one sends its note-off on press and note-on on release, so
 * it reads *high* when pressed. Something external must override the internal
 * pull-up on that net; the schematic shows a pull-down on PAGE/DFU's divider,
 * which fits, since the pin doubles as the DFU boot strap and has to sit at a
 * defined level at power-on.
 *
 * The lesson is that the SFS value tells you what the pin's own pull does, not
 * what the board does to it. */
static const struct direct_switch k_direct[BOARD_DIRECT_COUNT] = {
    {  6,  1, 0x40u, 3,  0, 1 },   /* PEDAL_TOE_SW  P6_1  / GPIO3[0]  */
    { 13, 10, 0x44u, 6, 24, 0 },   /* PAGE / DFU    PD_10 / GPIO6[24] */
};

void board_panel_init(void)
{
    for (unsigned i = 0; i < BOARD_COLUMNS; i++) {
        const struct pin *c = &k_columns[i];
        REG(SCU_BASE + c->scu_port * 0x80 + c->scu_pin * 4)
            = c->func | (1u << 4) | (1u << 5);          /* func, EPUN, EHS */
        REG(GPIO_CLR + c->gpio_port * 4) = 1u << c->gpio_bit;
        REG(GPIO_DIR + c->gpio_port * 4) |= 1u << c->gpio_bit;
    }

    for (unsigned i = 0; i < BOARD_ROWS; i++) {
        const struct pin *r = &k_rows[i];
        REG(SCU_BASE + r->scu_port * 0x80 + r->scu_pin * 4)
            = r->func | SFS_INPUT_PULLDOWN;
        REG(GPIO_DIR + r->gpio_port * 4) &= ~(1u << r->gpio_bit);
    }

    for (unsigned i = 0; i < 2; i++) {
        const struct pin *e = &k_enc[i];
        REG(SCU_BASE + e->scu_port * 0x80 + e->scu_pin * 4)
            = e->func | SFS_INPUT_PULLDOWN;
        REG(GPIO_DIR + e->gpio_port * 4) &= ~(1u << e->gpio_bit);
    }

    /* RGB rows as outputs, parked inactive. Normal startup muxes these before
     * handing off to the installed image, but a DFU-loaded image skips that
     * path, so it must do the work here. */
    for (unsigned i = 0; i < 3; i++) {
        const struct pin *r = &k_rgb[i];
        REG(SCU_BASE + r->scu_port * 0x80 + r->scu_pin * 4)
            = r->func | (1u << 4) | (1u << 5);          /* func, EPUN, EHS */
#if LED_ACTIVE_LOW
        REG(GPIO_SET + r->gpio_port * 4) = 1u << r->gpio_bit;
#else
        REG(GPIO_CLR + r->gpio_port * 4) = 1u << r->gpio_bit;
#endif
        REG(GPIO_DIR + r->gpio_port * 4) |= 1u << r->gpio_bit;
    }

    /* The direct switches - see k_direct. Not in the column matrix, own pins,
     * and active low where every matrix row is active high. */
    for (unsigned i = 0; i < BOARD_DIRECT_COUNT; i++) {
        const struct direct_switch *d = &k_direct[i];
        REG(SCU_BASE + d->scu_port * 0x80 + d->scu_pin * 4) = d->sfs;
        REG(GPIO_DIR + d->gpio_port * 4) &= ~(1u << d->gpio_bit);
    }
}

int board_direct_read(unsigned index)
{
    const struct direct_switch *d = &k_direct[index];
    int high = (REG(GPIO_PIN + d->gpio_port * 4) & (1u << d->gpio_bit)) != 0;
    return d->active_low ? !high : high;
}

/* The backlight shares the SCT pin group with the RGB rows: PD_13, PD_15 and
 * PD_14 are CTOUT_13, CTOUT_8 and CTOUT_11, while PD_16 is CTOUT_12, the
 * SCT's one output on its independent high counter. That output's duty is
 * expressed as value/255 of the period. Driving the pin as a GPIO therefore
 * puts nothing else into contention.
 *
 * Nothing else in this image touches PD_16. After the bootloader it is at
 * reset, pull-up on; after a warm DFU load it may still belong to the SCT,
 * which is left running and simply loses the pin. The level is set, then the
 * direction, then the mux, so the pin never shows the other level. */
#define BACKLIGHT_SFS       REG(SCU_BASE + 13u * 0x80u + 16u * 4u)   /* SFSPD_16 */
#define BACKLIGHT_PORT      6u
#define BACKLIGHT_BIT       30u

void board_backlight(int on)
{
    REG((on ? GPIO_SET : GPIO_CLR) + BACKLIGHT_PORT * 4u) = 1u << BACKLIGHT_BIT;
    REG(GPIO_DIR + BACKLIGHT_PORT * 4u) |= 1u << BACKLIGHT_BIT;
    BACKLIGHT_SFS = 4u | (1u << 4);               /* FUNC4 GPIO6[30], EPUN */
}

void board_leds_init(void)
{
    board_panel_init();
}

void board_leds_set(uint32_t mask)
{
    for (unsigned i = 0; i < BOARD_COLUMNS; i++) {
        const struct pin *c = &k_columns[i];
        uint32_t base = (mask & (1u << i)) ? GPIO_SET : GPIO_CLR;
        REG(base + c->gpio_port * 4) = 1u << c->gpio_bit;
    }
}

/* Drive the three RGB rows for one column. bit 0 red, bit 1 green, bit 2 blue. */
static void rgb_drive(uint8_t colour)
{
    for (unsigned i = 0; i < 3; i++) {
        const struct pin *r = &k_rgb[i];
        int on = (colour >> i) & 1;
#if LED_ACTIVE_LOW
        uint32_t base = on ? GPIO_CLR : GPIO_SET;
#else
        uint32_t base = on ? GPIO_SET : GPIO_CLR;
#endif
        REG(base + r->gpio_port * 4) = 1u << r->gpio_bit;
    }
}

void board_panel_scan(struct panel_state *out, const uint8_t *colours)
{
    out->switches = 0;

    /* Every column low before we start. A column left high by anything else -
     * an LED display, an aborted earlier scan - makes one closed switch pull
     * its row high during *every* column's sample, so the whole row reads as
     * pressed. That is exactly what a held status LED caused, and the scan
     * should not depend on its caller having tidied up. */
    for (unsigned i = 0; i < BOARD_COLUMNS; i++) {
        const struct pin *c = &k_columns[i];
        REG(GPIO_CLR + c->gpio_port * 4) = 1u << c->gpio_bit;
    }
    board_delay_crude(DISCHARGE_ITERATIONS);

    for (unsigned col = 0; col < BOARD_COLUMNS; col++) {
        const struct pin *c = &k_columns[col];

        /* Colour for this column goes up with the column and comes down with
         * it. That is what makes LED output safe here: exactly one column is
         * ever driven, so there is no path from a high column through two
         * closed switches on a row into a low one. Holding a colour outside the
         * strobe is what previously ghosted the whole matrix. */
        rgb_drive(colours ? colours[col] : 0u);

        REG(GPIO_SET + c->gpio_port * 4) = 1u << c->gpio_bit;

        /* Let the line settle before sampling - see SETTLE_ITERATIONS. Reading
         * too early does not give phantom presses, it gives no presses at all:
         * the row has not charged through the closed switch yet. This doubles
         * as the LED on-time, so each column gets about 1/7 duty. */
        board_delay_crude(SETTLE_ITERATIONS);

        for (unsigned row = 0; row < BOARD_ROWS; row++) {
            const struct pin *r = &k_rows[row];
            if (REG(GPIO_PIN + r->gpio_port * 4) & (1u << r->gpio_bit))
                out->switches |= 1u << (col * BOARD_ROWS + row);
        }

        /* This column's encoder uses bit 1 for ENC_A and bit 0 for ENC_B, the
         * order expected by the quadrature tables. Reversing these would only
         * reverse every encoder rather than fail loudly. */
        uint8_t code = 0;
        if (REG(GPIO_PIN + k_enc[0].gpio_port * 4) & (1u << k_enc[0].gpio_bit))
            code |= 2u;
        if (REG(GPIO_PIN + k_enc[1].gpio_port * 4) & (1u << k_enc[1].gpio_bit))
            code |= 1u;
        out->enc[col] = code;

        REG(GPIO_CLR + c->gpio_port * 4) = 1u << c->gpio_bit;
    }

    rgb_drive(0);          /* leave the rows parked between scans */
}

void board_delay_crude(uint32_t iterations)
{
    /* No timer is configured yet, so this is a spin. ~5 cycles per iteration at
     * the ~96 MHz the boot ROM leaves us at (docs/01), i.e. ~20e6 per second. */
    for (volatile uint32_t i = 0; i < iterations; i++)
        __asm__ volatile("");
}

/* PF_3 / GPIO7[18] asserts a system reset - proven on hardware (docs/01), but
 * not from this firmware, where it is accepted and nothing happens (see the
 * correction in docs/01).
 *
 * The software path is intentionally minimal: SCU `0x4008678C` = 0x34,
 * `CLR[7]` = 1<<18 and `DIR[7]` |= 1<<18. Nothing else in this image touches
 * `PF_3` or that GPIO bit. The unit stops answering afterwards, confirming
 * that execution reaches this function's final spin.
 *
 * Interrupts are disabled first and a delay precedes the assertion. Neither has
 * an obvious mechanism - a reset line is level triggered - but both are kept
 * as part of the verified board sequence. */
void board_reset(void)
{
    __asm__ volatile("cpsid i" ::: "memory");

    /* Keep the verified delay count. */
    for (volatile uint32_t i = 0; i < 100000u; i++)
        ;

    REG(SCU_BASE + 15 * 0x80 + 3 * 4) = 0x34;
    REG(GPIO_CLR + 7 * 4) = 1u << 18;
    REG(GPIO_DIR + 7 * 4) |= 1u << 18;
    for (;;)
        ;
}

/* ---- into the boot ROM's DFU loader ------------------------------------ */
/* The normal application path reaches DFU by writing "BootIntoDfu!" at
 * 0x1001ff00 and resetting through PF_3. The NOR bootloader looks for the string before it
 * does anything else and, finding it, hands the boot ROM its USB0 path. The
 * reset is the part this image cannot do: PF_3, SYSRESETREQ and the RGU reset
 * all leave a unit running a full image silent (docs/09). So this skips the
 * reset and the string, and does from here what the bootloader does after
 * them.
 *
 * The NOR bootloader handoff sequence is (docs/01 section 3):
 *
 *   0x14001990  cpsid i; MSP = 0x1008169c; VTOR = 0x14000000;
 *               CPACR |= 0x00F00000; SHCSR = 0; then the gate, 0x14004e5c
 *   0x14004e74  push {r4, r5, r6, lr}; a delay of 1000 x 24500 iterations;
 *               FP_REMAP = 0x20000000             the remap table
 *               FP_COMP5 = 0x10400F64             one word of the ROM ...
 *               *(0x20000014) = 0x20152015        ... becomes movs r0, #0x15 twice
 *               FP_COMP5 |= 1; FP_CTRL = 3        KEY | ENABLE
 *               call 0x104014B9, call 0x10400A05, jump 0x10400A27,
 *               each with r0 = 0x40045000, the OTP controller
 *
 * The patched word is where the ROM settles its boot source; 0x15 is USB0.
 * The three calls are the ROM's boot from there on.
 *
 * The bootloader runs that straight out of a reset. This image runs it with
 * the board up, so first it puts back what the ROM's path finds at power-on:
 *
 *  - Interrupts masked, every NVIC enable and pend cleared, SysTick stopped,
 *    fault handlers off (SHCSR = 0, as the bootloader leaves them), and VTOR
 *    on the ROM's own table at 0x10400000: ours is at 0x10000000, which the
 *    download overwrites while the ROM runs.
 *  - USB0 off the bus (RS = 0) for about a second, so the host sees the unit
 *    leave, then reset, as board_usb_clock_init() resets it.
 *  - The core on the IRC and PLL1 powered down. The ROM's USB path locks PLL1
 *    itself (it hands over at ~96 MHz, docs/02), and re-locking the PLL the
 *    core runs on is the marginal case in docs/09; from the IRC it locks from
 *    cold, as after power-on. The eMMC's clock stops with PLL1, and the SDRAM
 *    refresh, set for 204 MHz, falls behind, so the picture on the screen may
 *    decay; nothing reads either before the next image brings them up. The
 *    LCD's pixel clock is on the crystal.
 *  - The FPU left off. The PAGE > power-on path runs the same loader with it
 *    off, so the bootloader's CPACR write is not needed.
 *
 * The ROM calls cannot run from here. 0x104014B9 returns once the image is in
 * (or the bootloader would not make two more calls), and the image has just
 * been written over this one from 0x10000000; the bootloader returns into the
 * NOR, which nothing overwrites. So they run from a copy at 0x1001FF40, above
 * any image smaller than 130,880 bytes (midi_control.ld checks this one), on
 * the bootloader's stack.
 *
 * This has been run on a unit. A failure costs a hang until the power is
 * cycled; nothing here writes anything a power cycle does not undo. */

#define SCB_ICSR          REG(0xE000ED04u)
#define SCB_VTOR          REG(0xE000ED08u)
#define SCB_SHCSR         REG(0xE000ED24u)
#define SYST_CSR          REG(0xE000E010u)
#define NVIC_ICER(n)      REG(0xE000E180u + 4u * (n))
#define NVIC_ICPR(n)      REG(0xE000E280u + 4u * (n))
#define ICSR_PENDSTCLR    (1u << 25)
#define ICSR_PENDSVCLR    (1u << 27)

#define BASE_M4_CLK       REG(CGU_BASE + 0x6C)
#define PLL1_CTRL         REG(CGU_BASE + 0x44)
#define CLK_SEL_MASK      0x1F000000u
#define CLK_SEL_IRC       0x01000000u
#define PLL1_PD           (1u << 0)

#define ROM_VECTORS       0x10400000u
#define DFU_STUB_ADDR     0x1001FF40u
/* ~1 s: board_delay_crude runs about twice its nominal rate at 204 MHz. */
#define DETACH_ITERATIONS 40000000u

/* Position independent: PC-relative literals only, 4-byte aligned, and copied
 * to a 4-byte aligned address, so the literal offsets hold. */
__asm__(
    ".pushsection .text.board_dfu_stub, \"ax\", %progbits\n"
    ".syntax unified\n"
    ".thumb\n"
    ".align 2\n"
    ".global board_dfu_stub\n"
    ".global board_dfu_stub_end\n"
    "board_dfu_stub:\n"
    "    cpsid   i\n"
    "    ldr     r0, =0x1008169c\n"             /* the bootloader's MSP */
    "    msr     msp, r0\n"
    "    push    {r4, r5, r6, lr}\n"            /* its DFU path's frame: the same SP at the ROM */
    "    ldr     r3, =0xE0002000\n"             /* FP_CTRL */
    "    ldr     r5, =0x20000000\n"
    "    str     r5, [r3, #4]\n"                /* FP_REMAP */
    "    ldr     r4, =0x10400F64\n"
    "    str     r4, [r3, #28]\n"               /* FP_COMP5 */
    "    ldr     r2, =0x20000014\n"
    "    ldr     r0, =0x20152015\n"
    "    str     r0, [r2]\n"                    /* remap table entry 5 */
    "    ldr     r2, [r3, #28]\n"
    "    orr     r2, r2, #1\n"
    "    str     r2, [r3, #28]\n"               /* FP_COMP5 enabled */
    "    movs    r4, #3\n"
    "    str     r4, [r3]\n"                    /* FP_CTRL = KEY | ENABLE */
    "    ldr     r0, =0x40045000\n"
    "    ldr     r1, =0x104014B9\n"
    "    blx     r1\n"
    "    ldr     r0, =0x40045000\n"
    "    ldr     r1, =0x10400A05\n"
    "    blx     r1\n"
    "    ldr     r0, =0x40045000\n"
    "    ldr     r1, =0x10400A27\n"
    "    blx     r1\n"
    "    b       .\n"
    ".ltorg\n"
    "board_dfu_stub_end:\n"
    ".popsection\n"
);

extern const uint8_t board_dfu_stub[];
extern const uint8_t board_dfu_stub_end[];

void board_enter_dfu(void)
{
    __asm__ volatile("cpsid i" ::: "memory");

    SYST_CSR = 0;
    for (unsigned n = 0; n < 2u; n++) {           /* 53 external IRQs: two words */
        NVIC_ICER(n) = 0xFFFFFFFFu;
        NVIC_ICPR(n) = 0xFFFFFFFFu;
    }
    SCB_ICSR = ICSR_PENDSTCLR | ICSR_PENDSVCLR;
    SCB_SHCSR = 0;
    SCB_VTOR = ROM_VECTORS;

    USB0_USBCMD &= ~1u;                           /* RS = 0: off the bus */
    board_delay_crude(DETACH_ITERATIONS);
    USB0_USBCMD = 2u;                             /* RST = 1 */
    (void)poll_until(&USB0_USBCMD, 2u, 0u);       /* reset self-clears */

    BASE_M4_CLK = (BASE_M4_CLK & ~CLK_SEL_MASK) | CLK_SEL_IRC;   /* core <- IRC */
    board_delay_crude(1000u);
    PLL1_CTRL |= PLL1_PD;

    const uint32_t len = (uint32_t)(board_dfu_stub_end - board_dfu_stub);
    uint8_t *dst = (uint8_t *)DFU_STUB_ADDR;
    for (uint32_t i = 0; i < len; i++)
        dst[i] = board_dfu_stub[i];
    ((void (*)(void))(DFU_STUB_ADDR | 1u))();

    for (;;)
        ;
}

/* ---- elapsed time -------------------------------------------------------- */
/* Nothing else in this image keeps time: board_delay_crude() runs at whatever
 * rate the core does (docs/09). SysTick counts the core clock down from a
 * 24-bit reload; at the largest reload it wraps every 16,777,216 core cycles,
 * 82 ms at 204 MHz, and a caller that polls more often than that sees every
 * cycle. A longer pass loses whole wraps, so an interval only ever reads short.
 *
 * TICKINT stays clear. The counter never raises the SysTick exception, whose
 * vector is Default_Handler and hangs (startup.c); board_enter_dfu() stops the
 * counter again before the ROM runs.
 *
 * The rate is read from the clock registers rather than assumed from what the
 * bring-up did (emc_log_clocks() logs the same two): BASE_M4_CLK names the core's
 * source, and for PLL1, PLL1_CTRL holds the dividers (UM10503). The crystal and
 * the IRC are both 12 MHz. Any other source is unknown, and time does not pass. */

#define SYST_RVR          REG(0xE000E014u)
#define SYST_CVR          REG(0xE000E018u)
#define SYST_ENABLE       (1u << 0)
#define SYST_CLKSOURCE    (1u << 2)           /* the core clock */
#define SYST_COUNTFLAG    (1u << 16)          /* wrapped since CSR was last read */
#define SYST_RELOAD       0x00FFFFFFu

#define CLK_SEL_XTAL      0x06000000u
#define CLK_SEL_PLL1      0x09000000u
#define PLL1_BYPASS       (1u << 1)
#define PLL1_FBSEL        (1u << 6)
#define PLL1_DIRECT       (1u << 7)

static uint32_t g_ms_per;                     /* core cycles in a millisecond; 0 if unknown */
static uint32_t g_ms_last;                    /* SYST_CVR as last read */
static uint32_t g_ms_counts;                  /* cycles counted, short of a whole millisecond */

static uint32_t core_hz(void)
{
    const uint32_t sel = BASE_M4_CLK & CLK_SEL_MASK;
    if (sel == CLK_SEL_IRC || sel == CLK_SEL_XTAL)
        return 12000000u;
    if (sel != CLK_SEL_PLL1)
        return 0;

    const uint32_t ctrl = PLL1_CTRL;
    const uint32_t in = ctrl & CLK_SEL_MASK;
    if ((in != CLK_SEL_IRC && in != CLK_SEL_XTAL) || (ctrl & (PLL1_PD | PLL1_BYPASS)))
        return 0;

    /* M x 12 MHz / N; divided by 2P unless DIRECT or FBSEL. 204 MHz is M 17,
     * N 1, DIRECT. At most 256 x 12e6, inside 32 bits. */
    const uint32_t m = ((ctrl >> 16) & 0xFFu) + 1u;
    const uint32_t n = ((ctrl >> 12) & 3u) + 1u;
    uint32_t hz = 12000000u / n * m;
    if (!(ctrl & (PLL1_DIRECT | PLL1_FBSEL)))
        hz /= 2u << ((ctrl >> 8) & 3u);
    return hz;
}

uint32_t board_ms_start(void)
{
    const uint32_t hz = core_hz();

    SYST_CSR = 0;
    SYST_RVR = SYST_RELOAD;
    SYST_CVR = 0;                                 /* any write zeroes it and COUNTFLAG */
    SYST_CSR = SYST_CLKSOURCE | SYST_ENABLE;      /* TICKINT clear */
    g_ms_last = SYST_CVR;
    g_ms_counts = 0;
    g_ms_per = hz / 1000u;
    return hz;
}

uint32_t board_ms_elapsed(void)
{
    if (!g_ms_per)
        return 0;

    uint32_t now = SYST_CVR;
    uint32_t counts;
    if (SYST_CSR & SYST_COUNTFLAG) {
        now = SYST_CVR;                           /* the wrap may have come after the first read */
        counts = g_ms_last + (SYST_RELOAD + 1u - now);
    } else {
        counts = now <= g_ms_last ? g_ms_last - now : 0u;   /* the first reload after start */
    }
    g_ms_last = now;

    g_ms_counts += counts;
    const uint32_t ms = g_ms_counts / g_ms_per;
    g_ms_counts -= ms * g_ms_per;
    return ms;
}

/* ---- ADC: the expression pedal ----------------------------------------- */
/* ADC configuration uses burst mode over all eight channels with interrupts
 * disabled, i.e. free-running and polled. Both ADC0 and ADC1 are set up here.
 *
 * Nothing needs pin muxing. The pedal, volume and sensor lines are on the
 * LPC4350's dedicated ADC balls (ADC0_x / ADC1_x per the schematic), which is
 * the real reason no SCU write ever appeared to configure them and why an
 * earlier decode concluded the pedal was "unfound".
 *
 * Which channel actually carries the pedal is deliberately not assumed - the
 * schematic labels PEDAL1, PEDAL2, VOLUME and PEDAL_SENSOR_VOLTAGE, but it has
 * already been mis-read twice on adjacent rows. Read them all and see which one
 * moves. */
#define ADC0_BASE     0x400E3000u
#define ADC1_BASE     0x400E4000u
#define ADC_CR        0x00u
#define ADC_INTEN     0x0Cu
#define ADC_DR0       0x10u

#define CLK_APB3_BUS_CFG   REG(CCU1_BASE + 0x100)
#define CLK_APB3_ADC0_CFG  REG(CCU1_BASE + 0x118)
#define CLK_APB3_ADC1_CFG  REG(CCU1_BASE + 0x120)

/* CLKDIV of 40 gives roughly 2.3 MHz from a ~96 MHz APB3, comfortably inside
 * the 4.5 MHz maximum. Eight channels at 11 clocks each is ~38 us a sweep. */
#define ADC_CLKDIV    40u
#define ADC_CR_VALUE  (0xFFu | (ADC_CLKDIV << 8) | (1u << 16) | (1u << 21))

/* SCU analog-function enables. Without these the ADC converts a pin that is not
 * connected to anything, which is exactly the "every channel dead" result the
 * first sweep produced.
 *
 * These are enabled per-channel, as LPCOpen's
 * Chip_SCU_ADC_Channel_Config does.
 *
 * Only channels 0, 2, 3 and 7 are enabled: the schematic's analog block puts
 * VOLUME, PEDAL1, PEDAL2 and PEDAL_SENSOR_VOLTAGE on those. Channels 4 and 5
 * are deliberately left alone because some ADC channels are also reachable on
 * port pins - ADC0_5 appears on PF_10, which is ENC_A - and enabling the analog
 * function there would fight a digital input we need. */
#define SCU_ENAIO0    REG(0x40086C88u)
#define SCU_ENAIO1    REG(0x40086C8Cu)
#define ADC_CHANNELS  0x8Du        /* 0, 2, 3, 7 */

void board_adc_init(void)
{
    CLK_APB3_BUS_CFG |= 1u;
    CLK_APB3_ADC0_CFG |= 1u;
    CLK_APB3_ADC1_CFG |= 1u;

    SCU_ENAIO0 |= ADC_CHANNELS;
    SCU_ENAIO1 |= ADC_CHANNELS;

    REG(ADC0_BASE + ADC_INTEN) = 0;
    REG(ADC1_BASE + ADC_INTEN) = 0;
    REG(ADC0_BASE + ADC_CR) = ADC_CR_VALUE;
    REG(ADC1_BASE + ADC_CR) = ADC_CR_VALUE;
}

uint16_t board_adc_read(unsigned adc, unsigned channel)
{
    uint32_t base = adc ? ADC1_BASE : ADC0_BASE;
    uint32_t dr = REG(base + ADC_DR0 + channel * 4);
    /* 10-bit result sits in bits 15:6; bit 31 is DONE, which burst mode keeps
     * setting on its own. */
    return (uint16_t)((dr >> 6) & 0x3FF);
}

/* Two diagnostics for the volume knob, which reads about half scale in images
 * that bring up the EMC while the pedal reads as before (docs/14 section 3).
 *
 * A source that is not given time to settle before each sample reads low, a
 * slower converter clock gives it that time, and the knob's net is sampled by
 * both converters at once - so the questions are whether slowing the clock
 * restores the scale, and whether taking ADC1 off the net does. Each changes
 * one field of the configuration board_adc_init() wrote and leaves the rest.
 * The divider is never taken below ADC_CLKDIV, so the clock only ever gets
 * slower than the one that has been run. */
void board_adc_set_clkdiv(unsigned clkdiv)
{
    if (clkdiv < ADC_CLKDIV)
        clkdiv = ADC_CLKDIV;
    if (clkdiv > 255u)
        clkdiv = 255u;

    for (unsigned adc = 0; adc < 2; adc++) {
        uint32_t base = adc ? ADC1_BASE : ADC0_BASE;
        uint32_t cr = REG(base + ADC_CR);
        REG(base + ADC_CR) = (cr & ~(0xFFu << 8)) | (clkdiv << 8);
    }
}

void board_adc_burst(unsigned adc, int on)
{
    uint32_t base = adc ? ADC1_BASE : ADC0_BASE;
    uint32_t cr = REG(base + ADC_CR);
    REG(base + ADC_CR) = on ? (cr | (1u << 16)) : (cr & ~(1u << 16));
}

uint32_t board_adc_cr(unsigned adc)
{
    return REG((adc ? ADC1_BASE : ADC0_BASE) + ADC_CR);
}
