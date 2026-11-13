/* Booting the SHARC: ADI's kernel off the eMMC, then our program. docs/10
 * section 3 has the sequence and where each figure was measured; this is the
 * sequence the parent project's lab image booted the pass-through with on
 * 2026-11-13 (its variant 0: 20.4 MHz, no pauses, no per-word read-back). */

#include "sharc.h"

#include "board.h"
#include "emmc.h"
#include "log.h"
#include "storage.h"
#include "sharc_program.h"

#include "midi_control/midi_control_crc.h"

#define REG(a) (*(volatile uint32_t *)(a))

/* ---- the kernel ------------------------------------------------------------ */

/* The first 1,536 bytes of the factory SHARC image at EMMC_SHARC_LBA: ADI's
 * SPI boot kernel, 384 words. The CRC32 is of those bytes in the v2.50 update
 * image, and the eMMC of the unit this was developed on matches it. A unit
 * whose kernel differs is not booted: nothing here knows what a different
 * kernel expects. */
#define KERNEL_SECTORS  3u
#define KERNEL_WORDS    384u
#define KERNEL_CRC32    0x1F618A94u

/* ---- SSP0 ------------------------------------------------------------------ */

#define SSP0_BASE       0x40083000u
#define SSP0_CR0        REG(SSP0_BASE + 0x00)
#define SSP0_CR1        REG(SSP0_BASE + 0x04)
#define SSP0_DR         REG(SSP0_BASE + 0x08)
#define SSP0_SR         REG(SSP0_BASE + 0x0C)
#define SSP0_CPSR       REG(SSP0_BASE + 0x10)

#define SSP_SR_TNF      (1u << 1)       /* transmit FIFO not full */
#define SSP_SR_RNE      (1u << 2)       /* receive FIFO not empty */
#define SSP_SR_BSY      (1u << 4)

/* SSP0 is clocked twice (UM10503 Table 967): its registers through CCU1
 * CLK_M4_SSP0 (0x518, STAT 0x51C) and its shifter off BASE_SSP0_CLK through
 * CCU2 CLK_APB0_SSP0 (0x700, STAT 0x704). Both branches reset to running; they
 * are set anyway rather than trusting that across a NOR or DFU hand-over. */
#define CGU_BASE_SSP0_CLK  REG(0x40050094u)
#define CCU1_SSP0_CFG      REG(0x40051518u)
#define CCU1_SSP0_STAT     REG(0x4005151Cu)
#define CCU2_SSP0_CFG      REG(0x40052700u)
#define CCU2_SSP0_STAT     REG(0x40052704u)

/* PLL1 (204 MHz), autoblock: the encoding emc.c leaves the core on. / 10 is
 * 20.4 MHz on SCK. */
#define V_BASE_SSP0     0x09000800u
#define SSP0_PRESCALE   10u

/* ---- pins: the ones the stock firmware's SHARC boot drives (docs/01) ------- */

#define SCU_SFS(port, pin) REG(0x40086000u + (port) * 0x80u + (pin) * 4u)
#define GPIO_DIR(port)  REG(0x400F6000u + (port) * 4u)
#define GPIO_PIN(port)  REG(0x400F6100u + (port) * 4u)
#define GPIO_SET(port)  REG(0x400F6200u + (port) * 4u)
#define GPIO_CLR(port)  REG(0x400F6280u + (port) * 4u)

#define RESET_PORT      0u              /* GPIO0[11], P1_4: RESET_DSP, low = reset */
#define RESET_BIT       11u
#define FLAG0_PORT      1u              /* GPIO1[8], P1_5: DSP_TO_MCU_INT, the SHARC's FLAG0 */
#define FLAG0_BIT       8u
#define CS_PORT         4u              /* GPIO4[12], P9_0: the SHARC's SPI select */
#define CS_BIT          12u

/* ---- timing ---------------------------------------------------------------- */

#define US(n)           ((uint32_t)(n) * 20u)      /* board_delay_crude, ~20 per us */
#define SPIN_MAX        2000000u                   /* ~0.1 s of SSP polls */
#define FLAG0_SAMPLES   1000u                      /* 100 ms at 100 us */
#define FLAG0_EDGES     2u                         /* one full FLAG0 period */

/* Local SRAM: the eMMC driver fills it by polling, not DMA. */
static uint8_t g_kernel[KERNEL_SECTORS * EMMC_SECTOR_BYTES] __attribute__((aligned(4))) MIDI_CONTROL_LOCAL;
static uint8_t g_bitrev[256];
static uint32_t g_timeouts;

static void bitrev_init(void)
{
    for (unsigned i = 0; i < 256u; i++) {
        unsigned v = i, r = 0;
        for (unsigned b = 0; b < 8u; b++) {
            r = (r << 1) | (v & 1u);
            v >>= 1;
        }
        g_bitrev[i] = (uint8_t)r;
    }
}

static int ssp0_init(void)
{
    /* Pins as the stock SSP0 set-up writes them. The select is a GPIO driven by
     * hand: it stays low across the whole stream, which the SSP's own SSEL
     * would not. */
    SCU_SFS(3, 0) = 0x24u;               /* SSP0_SCK,  FUNC4, fast */
    SCU_SFS(9, 1) = 0xE7u;               /* SSP0_MISO, FUNC7, input buffer on */
    SCU_SFS(9, 2) = 0x27u;               /* SSP0_MOSI, FUNC7, fast */
    SCU_SFS(9, 0) = 0x00u;               /* GPIO4[12], the select */

    CGU_BASE_SSP0_CLK = V_BASE_SSP0;
    log_line("sharc: CCU1 CLK_M4_SSP0, CCU2 CLK_APB0_SSP0 on");
    CCU1_SSP0_CFG |= 1u;
    for (uint32_t i = 0; i < 100000u && !(CCU1_SSP0_STAT & 1u); i++)
        ;
    CCU2_SSP0_CFG |= 1u;
    for (uint32_t i = 0; i < 100000u && !(CCU2_SSP0_STAT & 1u); i++)
        ;
    if (!(CCU1_SSP0_STAT & 1u) || !(CCU2_SSP0_STAT & 1u)) {
        log_line("sharc: an SSP0 branch is not running - not touching SSP0");
        return -1;
    }

    log_line("sharc: SSP0 CR1, CPSR, CR0");
    SSP0_CR1 = 0;                        /* off while it is configured */
    SSP0_CPSR = SSP0_PRESCALE;
    SSP0_CR0 = 0x000Fu | (1u << 6) | (1u << 7);   /* 16-bit, SPI, CPOL 1, CPHA 1 */
    SSP0_CR1 = (1u << 1);                /* on, master */
    log_line("sharc: SSP0 SR");
    for (unsigned i = 0; i < 16u && (SSP0_SR & SSP_SR_RNE); i++)
        (void)SSP0_DR;                   /* whatever an earlier owner left */
    return 0;
}

static void ssp0_send16(uint16_t v)
{
    if (g_timeouts)
        return;                          /* one timeout ends the stream */
    for (uint32_t n = 0; !(SSP0_SR & SSP_SR_TNF); n++)
        if (n >= SPIN_MAX) {
            g_timeouts++;
            return;
        }
    SSP0_DR = v;
}

static void ssp0_drain(void)
{
    for (uint32_t n = 0; !g_timeouts && (SSP0_SR & SSP_SR_BSY); n++)
        if (n >= SPIN_MAX)
            g_timeouts++;
}

/* Words are stored big-endian, b0 b1 b2 b3. They go out as two frames, each
 * byte bit-reversed: SSP0 shifts most significant bit first, and the SHARC
 * takes each byte least significant bit first. On the wire, b3 b2 b1 b0.
 * docs/10 section 3. */
static uint32_t send_words(const uint8_t *p, uint32_t words)
{
    uint32_t sent = 0;

    for (; sent < words && !g_timeouts; sent++, p += 4) {
        ssp0_send16((uint16_t)((g_bitrev[p[3]] << 8) | g_bitrev[p[2]]));
        ssp0_send16((uint16_t)((g_bitrev[p[1]] << 8) | g_bitrev[p[0]]));
    }
    return sent;
}

static int flag0(void)
{
    return (GPIO_PIN(FLAG0_PORT) & (1u << FLAG0_BIT)) != 0;
}

int sharc_boot(struct sharc_status *st)
{
    st->rc = SHARC_FAIL_EMMC;
    st->kernel_crc = 0;
    st->words = 0;
    st->edges = 0;
    st->first_edge_us = 0xFFFFFFFFu;

    if (!storage_card_ready()) {
        log_line("sharc: no eMMC - not booting the SHARC");
        return st->rc;
    }
    log_hex("sharc: reading the kernel at LBA", EMMC_SHARC_LBA);
    int rc = emmc_read(EMMC_SHARC_LBA, KERNEL_SECTORS, g_kernel);
    if (rc != EMMC_OK) {
        log_dec("sharc: kernel read failed, rc", (uint32_t)rc);
        return st->rc;
    }
    st->kernel_crc = midi_control_crc32(g_kernel, KERNEL_WORDS * 4u);
    log_hex("sharc: kernel crc32", st->kernel_crc);
    if (st->kernel_crc != KERNEL_CRC32) {
        log_hex("sharc: not the kernel recorded in docs/10, expected", KERNEL_CRC32);
        st->rc = SHARC_FAIL_KERNEL;
        return st->rc;
    }

    bitrev_init();
    g_timeouts = 0;
    st->rc = SHARC_FAIL_SSP;
    if (ssp0_init() != 0)
        return st->rc;

    /* Reset, FLAG0 and the select. */
    log_line("sharc: reset pulse");
    SCU_SFS(1, 4) = 0x00u;               /* GPIO0[11], RESET_DSP */
    SCU_SFS(1, 5) = 0x40u;               /* GPIO1[8], input buffer on */
    GPIO_SET(CS_PORT) = 1u << CS_BIT;
    GPIO_DIR(CS_PORT) |= 1u << CS_BIT;
    GPIO_DIR(FLAG0_PORT) &= ~(1u << FLAG0_BIT);
    GPIO_CLR(RESET_PORT) = 1u << RESET_BIT;
    GPIO_DIR(RESET_PORT) |= 1u << RESET_BIT;
    board_delay_crude(US(10000));
    GPIO_SET(RESET_PORT) = 1u << RESET_BIT;
    board_delay_crude(US(10000));

    /* The select goes low and stays low: the stock loader never raises it. */
    GPIO_CLR(CS_PORT) = 1u << CS_BIT;
    board_delay_crude(US(500));

    st->words = send_words(g_kernel, KERNEL_WORDS);
    ssp0_drain();
    board_delay_crude(US(10000));        /* the kernel sets itself up */
    st->words += send_words(k_sharc_program, SHARC_PROGRAM_WORDS);
    ssp0_drain();
    log_dec("sharc: words sent", st->words);
    if (g_timeouts || st->words != KERNEL_WORDS + SHARC_PROGRAM_WORDS) {
        log_line("sharc: an SSP0 wait ran out - the stream is incomplete");
        return st->rc;
    }

    /* The program toggles FLAG0 every 480 frames, 10 ms. It first went low
     * 13 ms after the stream in the lab runs. */
    int last = flag0();
    for (uint32_t s = 0; s < FLAG0_SAMPLES && st->edges < FLAG0_EDGES; s++) {
        board_delay_crude(US(100));
        int now = flag0();
        if (now != last) {
            if (!st->edges)
                st->first_edge_us = (s + 1u) * 100u;
            st->edges++;
            last = now;
        }
    }
    log_dec("sharc: FLAG0 edges", st->edges);
    log_dec("sharc: first edge, us", st->first_edge_us);
    st->rc = st->edges >= FLAG0_EDGES ? SHARC_OK : SHARC_FAIL_FLAG0;
    log_line(st->rc == SHARC_OK ? "sharc: running" : "sharc: FLAG0 did not toggle");
    return st->rc;
}
