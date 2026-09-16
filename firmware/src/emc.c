/* SDRAM on the LPC4350's external memory controller.
 *
 * The values here are the ones the memory fitted to this board requires: its
 * geometry, its timings, the mode register it must be programmed with. The BOM
 * names the part only loosely and we have no datasheet for it, so they were
 * established by observing how the hardware initialises its own memory at
 * startup, then checked field by field against UM10503 - the address mapping
 * against Table 430, the mode register against the manual's own formula in
 * section 22.8.5.2, and every timing against what a standard SDRAM part needs
 * at the clock in use. They are facts about this hardware; docs/02 section 2
 * shows the working.
 *
 * One value is deliberately *not* taken from the hardware's own setup: the
 * refresh rate. See below.
 */

#include <stdint.h>
#include "board.h"
#include "emc.h"
#include "log.h"

#define REG(a) (*(volatile uint32_t *)(a))

#define EMC_BASE            0x40005000u
#define EMC_CONTROL         REG(EMC_BASE + 0x000)
#define EMC_STATUS          REG(EMC_BASE + 0x004)
#define EMC_DYNAMICCONTROL  REG(EMC_BASE + 0x020)
#define EMC_DYNAMICREFRESH  REG(EMC_BASE + 0x024)
#define EMC_DYNAMICREADCFG  REG(EMC_BASE + 0x028)
#define EMC_DYNAMICRP       REG(EMC_BASE + 0x030)
#define EMC_DYNAMICRAS      REG(EMC_BASE + 0x034)
#define EMC_DYNAMICSREX     REG(EMC_BASE + 0x038)
#define EMC_DYNAMICAPR      REG(EMC_BASE + 0x03C)
#define EMC_DYNAMICDAL      REG(EMC_BASE + 0x040)
#define EMC_DYNAMICWR       REG(EMC_BASE + 0x044)
#define EMC_DYNAMICRC       REG(EMC_BASE + 0x048)
#define EMC_DYNAMICRFC      REG(EMC_BASE + 0x04C)
#define EMC_DYNAMICXSR      REG(EMC_BASE + 0x050)
#define EMC_DYNAMICRRD      REG(EMC_BASE + 0x054)
#define EMC_DYNAMICMRD      REG(EMC_BASE + 0x058)
#define EMC_DYNAMICCONFIG0  REG(EMC_BASE + 0x100)
#define EMC_DYNAMICRASCAS0  REG(EMC_BASE + 0x104)

#define CREG_EMC_CLK        REG(0x4004312Cu)     /* bit 16 selects the EMC clock */
#define CCU1_EMC_CFG        REG(0x40051430u)
#define CCU1_EMC_STAT       REG(0x40051434u)
#define CCU1_EMCDIV_CFG     REG(0x40051478u)

#define SCU_BASE            0x40086000u
#define SCU_SFS(port, pin)  REG(SCU_BASE + (port) * 0x80 + (pin) * 4)
#define SCU_SFSCLK(n)       REG(SCU_BASE + 0xC00 + (n) * 4)
#define SCU_EMCDELAYCLK     REG(SCU_BASE + 0xD00)

/* Dynamic memory commands, DYNAMICCONTROL[8:7] plus the enable and CLKOUT bits
 * held asserted throughout. */
#define DYN_CMD_NORMAL      0x0003u
#define DYN_CMD_MODE        0x0083u
#define DYN_CMD_PRECHARGE   0x0103u
#define DYN_CMD_NOP         0x0183u

/* Timings. Each field is (cycles - 1); the cycle counts and what they work out
 * to in nanoseconds are in docs/02, where they are checked against what a
 * standard -7 grade SDRAM requires. They are minimums, so they stay valid at
 * any clock at or below the 102 MHz they were characterised at. */
#define T_RP    1
#define T_RAS   4
#define T_SREX  7
#define T_APR   1
#define T_DAL   5
#define T_WR    2
#define T_RC    6
#define T_RFC   6
#define T_XSR   7
#define T_RRD   1
#define T_MRD   2

/* Refresh is the one value that cannot be copied, and the one that fails
 * silently if it is wrong.
 *
 * Every other timing is a minimum: run the clock slower and you only add
 * margin. Refresh is a maximum. The update-mode loader's 0x64 means 0x64 x 16 =
 * 1600 EMC
 * cycles, which at its 102 MHz is 15.7 us and correct for a 4096-row part. We
 * do not run at 102 MHz - the update-mode loader reprograms PLL1 to 204 MHz and we
 * deliberately leave the core where the boot ROM puts it, ~96 MHz (docs/01), so
 * our EMC clock is ~48 MHz and those same 1600 cycles would be 33 us. That is
 * twice the interval the parts need, and it is exactly the failure the earlier
 * lcd-test image saw: a picture that stayed up and slowly decayed.
 *
 * So refresh is chosen rather than copied, and chosen to be safe across the
 * uncertainty in our own clock rather than tuned to a number we have not
 * measured. 24 x 16 = 384 cycles is under 15.6 us for any EMC clock at or above
 * 24.6 MHz, i.e. it tolerates the core being half what we think it is. The cost
 * of over-refreshing is bandwidth: at 48 MHz it refreshes every 8 us against a
 * 15.6 us requirement, which is around 2% of the bus. That is a good trade for
 * not having to be right about the clock.
 *
 * If the core clock is ever raised to the update-mode loader's 204 MHz, this can go
 * back to
 * 0x64 - and emc_test()'s hold check is what would catch it either way. */
#define EMC_REFRESH  24

/* ~50 us and ~200 us at the ~20e6 board_delay_crude iterations per second the
 * boot ROM's clock gives us (docs/01). The update-mode loader waits 50 us between
 * commands;
 * the initial NOP period is stretched because JEDEC wants 100 us of stable
 * clock before an SDRAM will accept anything. */
#define US50   1000u
#define US200  4000u

/* Crystal settle before locking PLL1 on the cold path. The crystal is normally
 * already running (both the boot ROM and the NOR bootloader leave it up, since
 * PLL0USB and the bootloader's own PLL1 lock off it), so this is margin rather
 * than a from-cold start - but a generous settle is what keeps the following
 * lock from being marginal, and it costs nothing on a path taken once at boot.
 * Run partly on the 12 MHz IRC, where board_delay_crude is slower, so the real
 * time is several milliseconds. */
#define XTAL_SETTLE  20000u

/* PLL1 registers and the exact value the running 204 MHz / direct configuration
 * reads back as on this board (measured: PLL1_CTRL 0x06100080, core CLK_SEL
 * 0x09). XTAL source, MSEL 16 (M = 17), N = 1, P = 1: 12 x 17 = 204 MHz. */
#define BASE_M4_CLK_REG    0x4005006Cu
#define XTAL_OSC_CTRL_REG  0x40050018u
#define PLL1_CTRL_REG      0x40050044u
#define PLL1_STAT_REG      0x40050040u
#define PLL1_CTRL_204      0x06100000u        /* locked, not yet direct */
#define PLL1_CTRL_204_DIR  0x06100080u        /* the full-rate running value */
#define CLK_SEL_PLL1       0x09u

/* Pin mux, exactly as the update-mode loader writes it.
 *
 * Two things are worth reading off this list rather than the register values,
 * because they settle what docs/02 could only infer from the mode-register
 * shift and the refresh rate:
 *
 * 1. **The data bus is 32 bits wide.** All four byte lanes are muxed - D0-D7 on
 *    P1_7..P1_14, D8-D15 on P5_0..P5_7, D16-D23 on PD_2..PD_9, D24-D31 on
 *    PE_5..PE_12 - and so are four byte-mask lines rather than two.
 * 2. The list is selective, not a blanket "mux the whole EMC". PD_1 is skipped,
 *    and only one of the chip selects and one clock enable are set up. So the
 *    presence of all 32 data lines is a statement about the board, not laziness. */
struct pin { uint8_t port, pin; };

static const struct pin k_f2[] = {
    { 1, 0 }, { 1, 1 }, { 1, 2 },
    { 2, 0 }, { 2, 1 }, { 2, 2 }, { 2, 6 },
    { 5, 0 }, { 5, 1 }, { 5, 2 }, { 5, 3 },
    { 5, 4 }, { 5, 5 }, { 5, 6 }, { 5, 7 },
    { 13, 0 },
    { 13, 2 }, { 13, 3 }, { 13, 4 }, { 13, 5 },
    { 13, 6 }, { 13, 7 }, { 13, 8 }, { 13, 9 },
};

static const struct pin k_f3[] = {
    { 1, 6 }, { 1, 7 }, { 1, 8 }, { 1, 9 }, { 1, 10 },
    { 1, 11 }, { 1, 12 }, { 1, 13 }, { 1, 14 },
    { 2, 7 }, { 2, 8 }, { 2, 9 }, { 2, 10 }, { 2, 11 }, { 2, 12 }, { 2, 13 },
    { 6, 4 }, { 6, 5 }, { 6, 9 }, { 6, 10 }, { 6, 11 }, { 6, 12 },
    { 14, 5 }, { 14, 6 }, { 14, 7 }, { 14, 8 }, { 14, 9 },
    { 14, 10 }, { 14, 11 }, { 14, 12 }, { 14, 13 },
};

static void emc_pins(uint16_t delay_clk)
{
    for (unsigned i = 0; i < sizeof(k_f2) / sizeof(k_f2[0]); i++)
        SCU_SFS(k_f2[i].port, k_f2[i].pin) = 0xF2u;

    for (unsigned i = 0; i < sizeof(k_f3) / sizeof(k_f3[0]); i++)
        SCU_SFS(k_f3[i].port, k_f3[i].pin) = 0xF3u;

    SCU_SFS(6, 8) = 0xF1u;

    /* The SDRAM clock outputs. All four must select an EMC_CLK function -
     * UM10503 section 22.2: "All four EMC_CLK clock signals must be configured
     * for all SDRAM devices independently of their size by selecting the
     * EMC_CLK function and enabling the input buffer (EZI = 1)".
     *
     * MODE 5 is required here. The update-mode loader writes 0xF7 to SFSCLK_0,
     * and the CLK pin
     * function table gives MODE 7 on CLK0 as ENET_TX_CLK - the Ethernet
     * transmit clock. Not an EMC clock at all. MODE 5 is EMC_CLK01, "SDRAM
     * clock 0 and clock 1 combined", which is what the manual's requirement
     * actually asks for.
     *
     * Matching the update-mode configuration exactly was the right instinct
     * everywhere else and
     * wrong here: it is a RAM-resident loader that runs for a few seconds after the
     * real
     * application has already configured the board, while a persistent image
     * has to bring the machine up from cold and get every pin right. When the two
     * disagree, the one that boots the product wins.
     *
     *   CLK0: MODE 5 = EMC_CLK01      CLK1: MODE 0 = EMC_CLK1
     *   CLK2: MODE 5 = EMC_CLK23      CLK3: MODE 0 = EMC_CLK2
     */
    SCU_SFSCLK(0) = 0xF5u;
    SCU_SFSCLK(1) = 0xF0u;
    SCU_SFSCLK(2) = 0xF5u;
    SCU_SFSCLK(3) = 0xF0u;

    /* The update-mode loader writes 0x5555 here. The main firmware writes 0x21 (docs/02).
     * This is a data-capture delay trim, and it is one of the things the sweep
     * varies, so it comes from the config rather than being fixed. */
    SCU_EMCDELAYCLK = delay_clk;
}

/* Adopting the running configuration - the trick that made the LCD work first
 * time - is *not* possible here, and that is measured rather than assumed.
 *
 * A DFU load does not reset the chip, so the LCD controller and the SCU pin mux
 * both survive it and can be inherited. The EMC does not: it is reset by
 * EMC_RST (reset #21), and a snapshot taken before anything is written confirms
 * it, every register at its documented power-on value:
 *
 *     CTL 00000003  STA 00000005  CFG 00000000
 *     RAS 00000303  DYN 00000006  REF 00000000
 *
 * CFG at zero means no address mapping at all, and STA bit 2 means the
 * controller is sitting in self-refresh. There is nothing there to adopt, so
 * the EMC always has to be established from scratch. Worth stating because the
 * opposite assumption cost several hardware loads.
 */

/* Did the controller actually leave self-refresh when asked? It arrives in
 * self-refresh from reset and everything else depends on this, so it is
 * recorded rather than assumed. */
int g_emc_left_selfrefresh;

/* What emc_clock_init() did with PLL1, recorded so the boot log can show it:
 *   2  adopted a PLL1 the bootloader (or a previous run) already had locked at
 *      204 MHz driving the core - the normal case on the eMMC boot path
 *   1  brought PLL1 up from cold and it locked - the normal case under DFU
 *   0  cold bring-up and PLL1 never locked - SDRAM and the eMMC both fail,
 *      because both clock off it
 * This is the single most useful line if a boot comes up mono / NO STORAGE. */
int g_emc_pll1_status;

/* Per-offset logging: far too much output across a whole sweep, and exactly
 * what is wanted for the one configuration being examined closely. */
int g_emc_verbose;

/* Wait for the controller to actually be out of self-refresh.
 *
 * UM10503 Table 412: STATUS bit 2 is SA, self-refresh acknowledge, and like the
 * SR request bit it comes out of reset **high**. Clearing SR is a *request*; the
 * manual says the acknowledge "must be polled to discover the current operating
 * mode of the EMC". Skipping that loses every write issued while the controller
 * is still in self-refresh - they go nowhere and read back as zero - which is a
 * failure that looks exactly like broken memory.
 *
 * Bounded, for the reason every poll in this firmware is bounded: an unbounded
 * spin on a bit that never sets is indistinguishable from a hung machine. */
static int emc_wait_normal(void)
{
    for (uint32_t i = 0; i < 1000000u; i++)
        if (!(EMC_STATUS & (1u << 2)))
            return 1;
    return 0;
}

/* Drive P6_7 / GPIO5[15] low.
 *
 * This is the step that was missing, and it was missing because the EMC init
 * was understood as a *function* rather than as a *sequence*. The update-mode
 * loader's board
 * init at 0x10007b38 is four calls:
 *
 *     bl 0x10007d84    enable DWT_CYCCNT
 *     bl 0x10007da4    this - P6_7 low
 *     bl 0x10007b50    clock init, PLL1 to 204 MHz
 *     b.w 0x10007dc8   the EMC init, which is all that was ever copied
 *
 * A GPIO asserted immediately before a memory controller is brought up is not
 * decoration. docs/02 records this same write from the *main* firmware too
 * and could only record it as "driven low at init", purpose unknown - so both
 * vendor images do it, in the same place, before touching SDRAM. On the
 * evidence its purpose is to enable something the memory needs: a supply, a
 * bus buffer, a transceiver. Whatever it is, no amount of correct controller
 * configuration substitutes for it, which is consistent with everything
 * measured - registers perfect, self-refresh exited, timings swept, and a
 * device that never answers.
 *
 * It costs three writes and it is what the hardware's own firmware does. */
static void emc_enable_line(void)
{
    SCU_SFS(6, 7) = 0x14u;                       /* FUNC4 -> GPIO5[15] */
    REG(0x400F6014u) |= 1u << 15;                /* DIR[5]: output */
    REG(0x400F6294u) = 1u << 15;                 /* CLR[5]: drive low */
}

/* The update-mode loader's clock init, the third of its four board-init calls.
 *
 * It takes the core to 204 MHz off the crystal and,
 * critically, does so *before* the EMC is configured - so the EMC has only ever
 * been brought up on this board at 102 MHz, never at the 48 MHz our boot-ROM
 * clock gives it.
 *
 * That is now the last difference between our sequence and the device's. Every
 * other candidate has been checked against UM10503 and found correct: the
 * address mapping decodes to exactly the fitted geometry (Table 430), the mode
 * register value and its shift match the manual's own worked formula
 * (section 22.8.5.2), all eleven timings fit a standard part at 102 MHz, and
 * the command sequence is the documented one.
 *
 * The manual also notes the EMCDELAYCLK trim is needed "for most SDRAM devices
 * running at frequencies above 96 MHz", which says the delay line is calibrated
 * around that region rather than around 48 MHz - another reason the device's
 * clock is not an incidental detail.
 *
 * The hardening, after a bootloader-started boot came up with SDRAM *and* the
 * eMMC both dead (docs/09): those two are the whole of what runs off PLL1, so a
 * single marginal PLL1 bring-up takes both down together. Two changes address
 * exactly that:
 *
 *  1. **Adopt a PLL1 that is already good rather than tearing it down.** On the
 *     eMMC boot path the NOR bootloader hands over with the core already on a
 *     locked PLL1 at 204 MHz. Re-locking a PLL the core is running off is what
 *     was marginal; adopting the running clock removes the risk on the exact
 *     path where it bit - the same trick lcd.c uses for the panel. The DFU path
 *     arrives at ~96 MHz on a differently-configured PLL1, so it still takes the
 *     cold bring-up.
 *  2. **On the cold path, verify the lock and retry - never switch the core to
 *     an unlocked PLL1.** The old code polled a bounded loop and then proceeded
 *     regardless, so a failed lock left the core (and the EMC and eMMC clocks)
 *     on garbage.
 *
 * Sequence for the cold path: park the core on the IRC, settle the 12 MHz
 * crystal, lock PLL1 (retried and verified), switch the core to it, then set
 * DIRECT for the full rate. Returns 1 if the core ends on a locked PLL1. */
static int pll1_already_good(void)
{
    return (REG(PLL1_STAT_REG) & 1u)                                  /* locked */
        && (((REG(BASE_M4_CLK_REG) >> 24) & 0x1Fu) == CLK_SEL_PLL1)   /* core on it */
        && (REG(PLL1_CTRL_REG) == PLL1_CTRL_204_DIR);                 /* 204 MHz, direct */
}

static int emc_clock_init(void)
{
    volatile uint32_t *base_m4 = (volatile uint32_t *)BASE_M4_CLK_REG;
    volatile uint32_t *xtal = (volatile uint32_t *)XTAL_OSC_CTRL_REG;
    volatile uint32_t *pll1_ctrl = (volatile uint32_t *)PLL1_CTRL_REG;
    volatile uint32_t *pll1_stat = (volatile uint32_t *)PLL1_STAT_REG;

    if (pll1_already_good()) {
        g_emc_pll1_status = 2;
        return 1;
    }

    *base_m4 = (*base_m4 & ~0x1F000000u) | 0x01000000u;   /* core <- IRC */
    board_delay_crude(US50);

    *xtal &= ~(1u << 2);                                  /* HF = 0, 12 MHz */
    *xtal &= ~(1u << 0);                                  /* enable */
    board_delay_crude(XTAL_SETTLE);

    int locked = 0;
    for (unsigned attempt = 0; attempt < 8u && !locked; attempt++) {
        *pll1_ctrl = PLL1_CTRL_204;                       /* XTAL, M=17, PD=0 */
        for (uint32_t i = 0; i < 2000000u; i++) {
            if (*pll1_stat & 1u) {
                locked = 1;
                break;
            }
        }
        if (!locked)
            board_delay_crude(US200);
    }
    if (!locked) {
        g_emc_pll1_status = 0;
        return 0;                                         /* do NOT switch the core */
    }

    *base_m4 = (*base_m4 & ~0x1F000000u) | 0x09000000u;   /* core <- PLL1 */
    board_delay_crude(US200);

    *pll1_ctrl |= 1u << 7;                                /* DIRECT: full rate */
    board_delay_crude(US200);

    g_emc_pll1_status = (*pll1_stat & 1u) ? 1 : 0;
    return g_emc_pll1_status != 0;
}

void emc_init(const struct emc_config *cfg)
{
    emc_enable_line();
    emc_clock_init();

    EMC_CONTROL = 0;

    /* Both branch clocks, always, before CREG chooses between them.
     *
     * This is not tidiness. The update-mode loader only ever enables the divided
     * branch,
     * because half-rate is all it wants, and selecting the undivided one in CREG
     * without enabling it points the EMC at a clock that is not running. An AHB
     * access to a peripheral with no clock does not fault - it never completes,
     * and the core stalls with no fault handler to report it. That is exactly
     * how the first sweep build died: silently, before USB, on configuration
     * zero, needing a power cycle to recover.
     *
     * Enabling a branch that then goes unused costs nothing. */
    CCU1_EMC_CFG |= 1u;
    for (uint32_t i = 0; i < 100000u && !(CCU1_EMC_STAT & 1u); i++)
        ;

    /* The divided branch has to be *stopped* before its divider will change.
     *
     * Writing RUN|DIV in one go - which is what the update-mode loader appears to
     * do, and
     * what this did for six loads - sets RUN and silently drops DIV. Measured:
     * write 0x21, read back 0x08000001. Bit 0 took, bits 7:5 did not.
     *
     * That single dropped field explains everything that has been confusing
     * about this. The EMC's registers are clocked by CLK_M4_EMC and were always
     * perfect - the after-init dump matches the intended values exactly. The
     * memory interface runs off this divided branch, and with CREG6 saying
     * divide-by-two while the branch was still divide-by-one, the two disagreed
     * in exactly the way UM10503 section 22.2 warns they must not. The
     * controller was configured correctly and clocked wrongly, so the SDRAM
     * never answered and the only thing that ever read back was the EMC's own
     * write buffer - which is why precisely four or five words worked. */
    CCU1_EMCDIV_CFG = 0x21u;             /* RUN | DIV=/2, as the update-mode loader writes */

    /* Half-rate EMC clock, set consistently in both places.
     *
     * UM10503 section 22.2 is explicit: if EMC_CCLK is half BASE_M4_CLK, the
     * divider "must be configured for half-frequency clock operation in **both**
     * the CREG6 register and the CCU1 CLK_EMCDIV_CFG register". Both, not
     * either.
     *
     * That sentence is the whole story behind three lost power cycles. An
     * earlier version made the divider selectable and, in the full-rate case,
     * cleared CREG6 bit 16 while leaving CLK_M4_EMCDIV_CFG at divide-by-two -
     * a mismatched pair - and the unit stalled dead every time. The conclusion
     * drawn at the time, that 0x4004312c was probably misidentified, was wrong:
     * the manual confirms CREG6 is at 0x4004312c and bit 16 is EMC_CLK_SEL, and
     * confirms CLK_M4_EMCDIV_CFG is CCU1 offset 0x478. Both guesses had been
     * right; the bug was setting one without the other.
     *
     * It is fixed rather than made selectable again. Half rate is what the
     * loader uses, it is what every timing in this file was measured at, and
     * the manual notes the EMC is typically limited to ~120 MHz for SDRAM
     * anyway. There is nothing to gain from the full-rate case. */
    /* Exactly what the update-mode loader writes, in the order it writes it.
     *
     * The DIV field of CLK_EMCDIV_CFG demonstrably does not take on this
     * silicon - written as 0x21 it reads back 0x08000001, RUN set and DIV
     * clear, whether the branch is running or stopped. That looked like a bug
     * worth working around, and a good deal of time went into working around
     * it. It is not: the update-mode loader writes the same 0x21 and the
     * update-mode loader works, so
     * whatever the readback means, it is not what stands between us and a
     * functioning SDRAM. Match what the hardware is known to accept and stop
     * improving on it. */
    CREG_EMC_CLK |= 1u << 16;            /* EMC_CLK_SEL: divide by 2 */
    CCU1_EMCDIV_CFG = 0x21u;             /* RUN, and the matching divide by 2 */

    emc_pins(cfg->delay_clk);

    EMC_CONTROL = 1;                     /* enable, little-endian, normal mode */

    EMC_DYNAMICCONFIG0 = 0x00004480u;    /* address mapping; see docs/02 */
    EMC_DYNAMICRASCAS0 = 0x00000202u;    /* RAS 2, CAS 2 */
    EMC_DYNAMICREADCFG = cfg->read_config;

    EMC_DYNAMICRP = T_RP;
    EMC_DYNAMICRAS = T_RAS;
    EMC_DYNAMICSREX = T_SREX;
    EMC_DYNAMICAPR = T_APR;
    EMC_DYNAMICDAL = T_DAL;
    EMC_DYNAMICWR = T_WR;
    EMC_DYNAMICRC = T_RC;
    EMC_DYNAMICRFC = T_RFC;
    EMC_DYNAMICXSR = T_XSR;
    EMC_DYNAMICRRD = T_RRD;
    EMC_DYNAMICMRD = T_MRD;

    EMC_DYNAMICREFRESH = EMC_REFRESH;

    /* The JEDEC power-up sequence. NOP holds CKE and the clock steady while the
     * part wakes, then all banks are precharged, then the mode register is
     * loaded, then normal operation. */
    EMC_DYNAMICCONTROL = DYN_CMD_NOP;

    /* Confirm the controller has actually left self-refresh before issuing any
     * further command.
     *
     * Measured at entry (docs/02): the EMC arrives at its power-on reset state,
     * STATUS = 0x5, so SA is set and the controller is in self-refresh, and
     * DYNAMICCONTROL = 0x6, so SR is set and clock enable is off. Writing NOP
     * above clears SR and sets CE, but that is a *request*. The manual is
     * explicit that the acknowledge must be polled to know the operating mode -
     * and if PALL and MODE are issued while the controller is still in
     * self-refresh, the SDRAM never gets initialised at all, which is what
     * reads returning zero look like.
     *
     * The result is recorded rather than acted on: if it does not clear, the
     * rest of the sequence is worth running anyway, and the flag says so on
     * screen instead of silently producing a wrong verdict. */
    g_emc_left_selfrefresh = emc_wait_normal();

    board_delay_crude(US200);

    EMC_DYNAMICCONTROL = DYN_CMD_PRECHARGE;
    board_delay_crude(US50);

    /* Loading the mode register is a *read*, and the mode register value rides
     * on the address rather than the data: the device's own initialisation reads
     * from 0x28044000,
     * an offset of 0x44000, which is 0x22 << 13 - burst length 4, CAS latency 2.
     * The shift is column bits + bank bits + log2 of the bus width in bytes,
     * so 9 + 2 + 2 for the 32-bit bus the pin list shows. CAS 2 agrees with the
     * CAS field of DYNAMICRASCAS0 above, which is a cross-check those two
     * numbers did not have to pass. */
    EMC_DYNAMICCONTROL = DYN_CMD_MODE;
    (void)*(volatile uint8_t *)(EMC_SDRAM_BASE + 0x44000u);
    board_delay_crude(US50);

    EMC_DYNAMICCONTROL = DYN_CMD_NORMAL;
    emc_wait_normal();                   /* SA must clear before any access */

    EMC_DYNAMICCONFIG0 = 0x00084480u;    /* same mapping, buffer now enabled */
}

/* ---- proving it actually works ----------------------------------------- */

/* A configured EMC and a working EMC are not the same thing, and the difference
 * is not visible from the registers - a wrong address mapping reads back
 * perfectly as long as you only ever touch one address.
 *
 * The first version of this assumed 32 MiB and reported a bare "address fault"
 * when the walk disagreed, which was two mistakes at once. The size was an
 * inference from a framebuffer address 31 MiB in (docs/07), and a test that
 * asserts an inference reports a fault when the inference is wrong.
 * Worse, writing every offset before verifying any of them means an aliased
 * write clobbers a *lower* address, so the first mismatch found is the victim
 * and not the culprit - the reported address pointed away from the problem.
 *
 * So the size is now measured rather than assumed, and the order is fixed:
 *
 * 1. **Does it read back at all.** One word, written and read immediately. If
 *    this fails nothing else is worth running, and it is a data fault, not an
 *    address one - the distinction the old ordering could not make, because the
 *    address walk was simply the first check that happened to read.
 * 2. **How big is it.** Put a marker at the base, then walk the address lines
 *    upwards writing a distinct value at each power of two. The first offset
 *    that disturbs the marker is where the memory wraps, and that is its size.
 *    A fault and a smaller-than-expected part produce the same signal here, so
 *    the size is reported either way and judged by whether it is a sane number.
 * 3. **Do the address lines below that size hold distinct values.** Now a
 *    genuine aliasing fault, with the size question already answered.
 * 4. **Data**, over a window, immediately verified.
 * 5. **Refresh**: the same window re-checked after a pause. Refresh that is too
 *    slow reads back perfectly straight away and rots over milliseconds, which
 *    is exactly what the earlier lcd-test image saw. */

#define TEST_WINDOW  (256u * 1024u)          /* words, i.e. 1 MiB */
#define MARKER       0x5A5A5A5Au

/* Value written at a given address. Tied to the address so that an alias shows
 * up as a wrong value rather than a coincidentally right one - and so that the
 * value found identifies which address clobbered it, since
 * got ^ 0xA5A5A5A5 is the address whose write landed there. */
static uint32_t pattern(uint32_t addr)
{
    return addr ^ 0xA5A5A5A5u;
}

/* Which byte lanes survive a round trip. Bit n set means lane n failed.
 *
 * Added because the first failure on hardware was not the one the verdict
 * claimed. The marker went in as 5A5A5A5A and came back as 5A5A5A00: three
 * lanes perfect, the bottom one zero. That is not aliasing and not a value
 * anything had written - it is one byte lane not reading back - and no amount
 * of address-walking would have said so.
 *
 * The four patterns go to four different addresses and are read back only after
 * touching a distant row, because reading straight back can be served out of
 * the EMC's own write buffer and prove nothing. */
static unsigned lane_check(void)
{
    volatile uint32_t *sdram = (volatile uint32_t *)EMC_SDRAM_BASE;
    static const uint32_t k_lane[4] = {
        0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u,
    };
    unsigned bad = 0;

    for (unsigned i = 0; i < 4; i++)
        sdram[i] = k_lane[i];

    (void)*(volatile uint32_t *)(EMC_SDRAM_BASE + 0x00100000u);

    for (unsigned i = 0; i < 4; i++)
        if (sdram[i] != k_lane[i])
            bad |= 1u << i;

    return bad;
}

/* Fill a window, read it back, wait, read it back again.
 *
 * The previous version of this was clever and wrong, and it cost more than the
 * bug it was looking for. It kept a marker word at the base to detect address
 * wrap, walked powers of two, measured the size, and classified failures into
 * three kinds - and every one of those interacting parts was a chance to report
 * a fault that was really the test tripping over itself. It did exactly that,
 * twice: once when a later step overwrote the marker an earlier step depended
 * on, and once when it reported the *victim* of an aliased write rather than
 * the culprit. Both were read as hardware faults and chased as such.
 *
 * The evidence that finally settled it was the dumbest output in the log: a
 * dump of thirty-two consecutive words, every one correct, at the very address
 * the walk was calling broken.
 *
 * So this is now the simplest thing that can distinguish working memory from
 * broken memory, and nothing more. Each word holds a value derived from its own
 * address, so a misdirected access shows up as the wrong value rather than a
 * plausible one. Size detection is gone - UM10503 Table 430 gives the geometry
 * outright, and a test that measures what the manual already states is a test
 * that can disagree with it. */
static int run_test(struct emc_result *r, uint32_t words, int hold)
{
    volatile uint32_t *sdram = (volatile uint32_t *)EMC_SDRAM_BASE;

    r->addr = 0;
    r->want = 0;
    r->got = 0;
    r->size = EMC_SDRAM_SIZE;
    r->lanes = lane_check();

    for (uint32_t i = 0; i < words; i++)
        sdram[i] = pattern(EMC_SDRAM_BASE + i * 4);

    for (uint32_t i = 0; i < words; i++) {
        uint32_t want = pattern(EMC_SDRAM_BASE + i * 4);
        if (sdram[i] != want) {
            r->addr = EMC_SDRAM_BASE + i * 4;
            r->want = want;
            r->got = sdram[i];
            return EMC_FAIL_DATA;
        }
    }

    if (g_emc_verbose) {
        log_line("first 32 words:");
        for (uint32_t i = 0; i < 32; i++)
            log_hex("  w", sdram[i]);
    }

    /* Refresh: the same window again after a pause. Too slow a refresh reads
     * back perfectly straight away and rots over milliseconds. */
    if (!hold)
        return EMC_OK;

    board_delay_crude(4000000u);            /* ~200 ms */

    for (uint32_t i = 0; i < words; i++) {
        uint32_t want = pattern(EMC_SDRAM_BASE + i * 4);
        if (sdram[i] != want) {
            r->addr = EMC_SDRAM_BASE + i * 4;
            r->want = want;
            r->got = sdram[i];
            return EMC_FAIL_REFRESH;
        }
    }

    return EMC_OK;
}

int emc_test(struct emc_result *r)
{
    return run_test(r, TEST_WINDOW, 1);
}

int emc_quick_test(struct emc_result *r)
{
    return run_test(r, 4096, 0);            /* 16 KiB, no refresh hold */
}

/* The capture trim, swept across its whole useful range. Four nibbles, one per
 * EMC clock output; the update-mode loader uses 0x5555 and the main firmware 0x21,
 * so the
 * board evidently tolerates more than one answer at the update-mode loader's clock. */
/* The update-mode loader's own value first, so sweep index 0 is the configuration with the
 * best evidence behind it rather than an arbitrary corner of the space. */
static const uint16_t k_delays[4] = { 0x5555u, 0x0000u, 0xAAAAu, 0xFFFFu };

/* Sixteen configurations: four capture trims by four read strategies, all at
 * whatever clock setting is already in force. Nothing here changes a clock, so
 * nothing here can stall the unit - which the clock experiment did, three times,
 * costing a power cycle each.
 *
 * The loop that walks these lives in main.c rather than here, so the screen can
 * be redrawn after every step. A sweep whose whole point is to find which
 * settings survive must assume some of them will not, and a result table only
 * shown once everything has finished tells you nothing about the one that hung.
 */
void emc_config_at(unsigned i, struct emc_config *cfg)
{
    /* Read strategy 1 first, again because that is what the update-mode loader uses. */
    static const uint8_t k_rd[4] = { 1, 0, 2, 3 };

    cfg->read_config = k_rd[i & 3u];
    cfg->delay_clk = k_delays[(i >> 2) & 3u];
}

/* What the EMC looked like the moment we arrived, before anything touched it.
 *
 * Every theory about this so far has been a guess about this state - that the
 * earlier configuration was still live, that the controller was sitting in
 * self-refresh, that the boot ROM had or had not reset the block. Guessing has
 * cost four power cycles and six loads. These six registers say which of those
 * is true, and they cost one function call to capture. */
void emc_snapshot(struct emc_snapshot *s)
{
    s->control = EMC_CONTROL;
    s->status = EMC_STATUS;
    s->config0 = EMC_DYNAMICCONFIG0;
    s->rascas0 = EMC_DYNAMICRASCAS0;
    s->dyncontrol = EMC_DYNAMICCONTROL;
    s->refresh = EMC_DYNAMICREFRESH;
}

/* The clock tree behind the EMC, logged rather than reasoned about.
 *
 * The register state after init is perfect and the memory still does not
 * answer, which puts the fault upstream of the controller: in the clock that
 * reaches the part, or in whether the branch feeding it is running at all.
 * Every one of those is a register we can simply read.
 *
 * BASE_M4_CLK's CLK_SEL and PLL1_CTRL together give the core frequency exactly
 * - the crystal is 12 MHz and PLL1's dividers are in the register - and the EMC
 * runs at that or half it. The CCU STAT bits say whether the two EMC branches
 * are actually running rather than merely configured. */
void emc_log_clocks(void)
{
    log_line("clocks");
    log_dec("  pll1 status (2 adopted,1 cold-locked,0 FAILED)", (uint32_t)g_emc_pll1_status);
    log_hex("  BASE_M4_CLK  ", REG(0x4005006Cu));
    log_hex("  PLL1_CTRL    ", REG(0x40050044u));
    log_hex("  PLL1_STAT    ", REG(0x40050040u));
    log_hex("  XTAL_OSC_CTRL", REG(0x40050018u));
    log_hex("  CREG6        ", CREG_EMC_CLK);
    log_hex("  EMC_CFG      ", CCU1_EMC_CFG);
    log_hex("  EMC_STAT     ", CCU1_EMC_STAT);
    log_hex("  EMCDIV_CFG   ", CCU1_EMCDIV_CFG);
    log_hex("  EMCDIV_STAT  ", REG(0x4005147Cu));
    log_hex("  SFSCLK_0     ", SCU_SFSCLK(0));
    log_hex("  SFSCLK_1     ", SCU_SFSCLK(1));
    log_hex("  SFSCLK_2     ", SCU_SFSCLK(2));
    log_hex("  SFSCLK_3     ", SCU_SFSCLK(3));
    log_hex("  EMCDELAYCLK  ", SCU_EMCDELAYCLK);
}
