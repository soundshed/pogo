/* The eMMC, over the LPC4350 SD/MMC controller. Read and write paths.
 *
 * Hardware configuration
 * ----------------------
 * The pin mux uses twelve SCU writes. Eight data lines are muxed, identifying
 * the part as an eMMC on an 8-bit bus rather than an SD card.
 *
 * The clock is `BASE_SDIO_CLK` at CGU `0x40050090`, consistent with the
 * documented CGU peripheral map.
 *
 * The `0xbdc2` error mask covers every error bit and no completion bit, which
 * confirms the bit numbering used here.
 *
 * Transfers poll the FIFO rather than using chained internal-DMA descriptors.
 * For reading 785 sectors once that is fast enough, and it has no failure mode
 * that hangs silently: every wait here is a bounded count that returns an error
 * code the log can carry out.
 *
 * The write path is CMD24 single-block, polled, with a busy wait; see emmc.h
 * for the rules on using it.
 */

#include <stdint.h>
#include "board.h"
#include "emmc.h"
#include "log.h"

#define REG(a) (*(volatile uint32_t *)(a))

/* ---- the controller ------------------------------------------------------ */

#define SD_BASE        0x40004000u
#define SD_CTRL        REG(SD_BASE + 0x000)
#define SD_PWREN       REG(SD_BASE + 0x004)
#define SD_CLKDIV      REG(SD_BASE + 0x008)
#define SD_CLKSRC      REG(SD_BASE + 0x00C)
#define SD_CLKENA      REG(SD_BASE + 0x010)
#define SD_TMOUT       REG(SD_BASE + 0x014)
#define SD_CTYPE       REG(SD_BASE + 0x018)
#define SD_BLKSIZ      REG(SD_BASE + 0x01C)
#define SD_BYTCNT      REG(SD_BASE + 0x020)
#define SD_INTMASK     REG(SD_BASE + 0x024)
#define SD_CMDARG      REG(SD_BASE + 0x028)
#define SD_CMD         REG(SD_BASE + 0x02C)
#define SD_RESP(n)     REG(SD_BASE + 0x030 + (n) * 4)
#define SD_RINTSTS     REG(SD_BASE + 0x044)
#define SD_STATUS      REG(SD_BASE + 0x048)
#define SD_FIFOTH      REG(SD_BASE + 0x04C)
#define SD_BMOD        REG(SD_BASE + 0x080)
#define SD_DATA        REG(SD_BASE + 0x100)

#define CTRL_RESET     (1u << 0)
#define CTRL_FIFO_RST  (1u << 1)
#define CTRL_DMA_RST   (1u << 2)
#define CTRL_INT_EN    (1u << 4)
#define CTRL_USE_IDMAC (1u << 25)

#define CMD_START      (1u << 31)
#define CMD_UPDATE_CLK (1u << 21)
#define CMD_SEND_INIT  (1u << 15)
#define CMD_WAIT_PRV   (1u << 13)
#define CMD_WRITE      (1u << 10)
#define CMD_DATA_EXP   (1u << 9)
#define CMD_CHECK_CRC  (1u << 8)
#define CMD_RESP_LONG  (1u << 7)
#define CMD_RESP_EXP   (1u << 6)

#define INT_CDONE      (1u << 2)
#define INT_DTO        (1u << 3)
#define INT_RXDR       (1u << 5)
/* RE, RCRC, DCRC, RTO, DRTO, HTO, FRUN, HLE, SBE and EBE. */
#define INT_ERRORS     0x0000BDC2u

#define STATUS_FIFO_COUNT(s)  (((s) >> 17) & 0x1FFFu)

/* ---- clock and pins ------------------------------------------------------ */

#define CGU_BASE_SDIO_CLK   REG(0x40050090u)
#define CCU1_SDIO_CFG       REG(0x40051438u)
#define CCU1_SDIO_STAT      REG(0x4005143Cu)

#define SCU_BASE            0x40086000u
#define SCU_SFS(port, pin)  REG(SCU_BASE + (port) * 0x80 + (pin) * 4)

/* PLL1 at 204 MHz, autoblock, powered - the same CLK_SEL encoding board.c and
 * emc.c use. emc_init() has already locked PLL1 and put the core on it by the
 * time anything here runs. */
#define V_BASE_SDIO         0x09000800u
#define SDIO_INPUT_HZ       204000000u

/* Card clock = SDIO_INPUT_HZ / (2 * CLKDIV), and CLKDIV 0 means undivided.
 * 400 kHz for identification because that is what the specification requires
 * of every card before it has been told otherwise; 12.75 MHz for the transfer,
 * which reads the whole SHARC image in about a third of a second on a 1-bit
 * bus and asks nothing of the timing. */
#define CLKDIV_SLOW         255u        /* 400 kHz */
#define CLKDIV_FAST         8u          /* 12.75 MHz */

struct emmc_info g_emmc;

static int g_ready;

/* Bounded waits. The numbers are iteration counts, not time: what matters is
 * that every one of them ends. */
#define SPIN_SHORT   200000u
#define SPIN_LONG    2000000u

/* ---- primitives ---------------------------------------------------------- */

static int wait_cmd_taken(void)
{
    for (uint32_t i = 0; i < SPIN_SHORT; i++)
        if (!(SD_CMD & CMD_START))
            return 1;
    return 0;
}

/* The clock registers are not written directly - they are loaded by a command
 * with UPDATE_CLOCK_REGISTERS_ONLY set, and the card clock has to be off while
 * it happens. Three of those per change is the documented dance and it is not
 * optional: writing CLKDIV alone changes nothing. */
static int clock_set(uint32_t div)
{
    SD_CLKENA = 0;
    SD_CMD = CMD_START | CMD_UPDATE_CLK | CMD_WAIT_PRV;
    if (!wait_cmd_taken())
        return 0;

    SD_CLKDIV = div;
    SD_CMD = CMD_START | CMD_UPDATE_CLK | CMD_WAIT_PRV;
    if (!wait_cmd_taken())
        return 0;

    SD_CLKENA = 1;                       /* card 0, clock always on */
    SD_CMD = CMD_START | CMD_UPDATE_CLK | CMD_WAIT_PRV;
    if (!wait_cmd_taken())
        return 0;

    g_emmc.clock_hz = div ? SDIO_INPUT_HZ / (2u * div) : SDIO_INPUT_HZ;
    return 1;
}

/* One command, polled to completion. Returns 1 on a clean CDONE, 0 on any
 * error bit or on running out of patience. */
static int command(uint32_t index, uint32_t arg, uint32_t flags)
{
    SD_RINTSTS = 0xFFFFFFFFu;
    SD_CMDARG = arg;
    SD_CMD = CMD_START | flags | (index & 0x3Fu);

    if (!wait_cmd_taken())
        return 0;

    for (uint32_t i = 0; i < SPIN_LONG; i++) {
        uint32_t s = SD_RINTSTS;
        if (s & INT_ERRORS)
            return 0;
        if (s & INT_CDONE)
            return 1;
    }
    return 0;
}

static void save_response(uint32_t *out, int long_response)
{
    out[0] = SD_RESP(0);
    if (long_response) {
        out[1] = SD_RESP(1);
        out[2] = SD_RESP(2);
        out[3] = SD_RESP(3);
    } else {
        out[1] = out[2] = out[3] = 0;
    }
}

/* ---- bring-up ------------------------------------------------------------ */

/* The twelve required pin-mux writes, in register order.
 *
 *   PC_0  SD_CLK    PC_2  SD_VOLT0   PC_9  SD_POW    PC_10 SD_CMD
 *   PC_4..PC_7   SD_DAT0..3
 *   PC_11..PC_14 SD_DAT4..7
 *
 * All FUNC7. 0xf7 is high speed with the input buffer on and the glitch filter
 * off, which is what a bidirectional data line needs; the clock gets 0x77 and
 * the two control lines 0x17. */
static void pins_init(void)
{
    SCU_SFS(0xC, 0)  = 0x77u;
    SCU_SFS(0xC, 4)  = 0xF7u;
    SCU_SFS(0xC, 5)  = 0xF7u;
    SCU_SFS(0xC, 6)  = 0xF7u;
    SCU_SFS(0xC, 7)  = 0xF7u;
    SCU_SFS(0xC, 11) = 0xF7u;
    SCU_SFS(0xC, 12) = 0xF7u;
    SCU_SFS(0xC, 13) = 0xF7u;
    SCU_SFS(0xC, 14) = 0xF7u;
    SCU_SFS(0xC, 2)  = 0x17u;
    SCU_SFS(0xC, 10) = 0xF7u;
    SCU_SFS(0xC, 9)  = 0x17u;
}

void emmc_census(void)
{
    log_line("emmc census (nothing written yet)");
    log_hex("  BASE_SDIO_CLK", CGU_BASE_SDIO_CLK);
    log_hex("  CCU1_SDIO_CFG", CCU1_SDIO_CFG);
    log_hex("  CCU1_SDIO_STA", CCU1_SDIO_STAT);
    log_hex("  SFSPC_0  clk ", SCU_SFS(0xC, 0));
    log_hex("  SFSPC_10 cmd ", SCU_SFS(0xC, 10));
    log_hex("  SFSPC_4  dat0", SCU_SFS(0xC, 4));
    log_hex("  SFSPC_9  pow ", SCU_SFS(0xC, 9));
}

void emmc_log_regs(void)
{
    log_hex("  SD_CTRL      ", SD_CTRL);
    log_hex("  SD_PWREN     ", SD_PWREN);
    log_hex("  SD_CLKDIV    ", SD_CLKDIV);
    log_hex("  SD_CLKENA    ", SD_CLKENA);
    log_hex("  SD_CTYPE     ", SD_CTYPE);
    log_hex("  SD_STATUS    ", SD_STATUS);
    log_hex("  SD_RINTSTS   ", SD_RINTSTS);
    log_hex("  SD_FIFOTH    ", SD_FIFOTH);
}

void emmc_log_card(void)
{
    log_hex("  OCR          ", g_emmc.ocr);
    log_hex("  CID 0        ", g_emmc.cid[0]);
    log_hex("  CID 1        ", g_emmc.cid[1]);
    log_hex("  CID 2        ", g_emmc.cid[2]);
    log_hex("  CID 3        ", g_emmc.cid[3]);
    log_hex("  CSD 0        ", g_emmc.csd[0]);
    log_hex("  CSD 1        ", g_emmc.csd[1]);
    log_hex("  CSD 2        ", g_emmc.csd[2]);
    log_hex("  CSD 3        ", g_emmc.csd[3]);
    log_dec("  sector addr  ", (uint32_t)g_emmc.sector_addressed);
    log_dec("  card clock   ", g_emmc.clock_hz);

    /* The manufacturer ID and the product name, which is the part number we
     * have never had. CID is a 128-bit register presented in RESP0..3 with the
     * low 8 bits (the CRC) shifted off, so MID is the top byte of RESP3 and the
     * six product-name characters run from RESP3 bit 7 down through RESP2. */
    char name[8];
    uint32_t hi = g_emmc.cid[3], mid = g_emmc.cid[2];
    name[0] = (char)((hi >> 0) & 0xFF);
    name[1] = (char)((mid >> 24) & 0xFF);
    name[2] = (char)((mid >> 16) & 0xFF);
    name[3] = (char)((mid >> 8) & 0xFF);
    name[4] = (char)((mid >> 0) & 0xFF);
    name[5] = (char)((g_emmc.cid[1] >> 24) & 0xFF);
    name[6] = 0;
    for (unsigned i = 0; i < 6; i++)
        if (name[i] < 0x20 || name[i] > 0x7E)
            name[i] = '?';
    log_line("  product name follows, best effort");
    log_line(name);
    log_hex("  manufacturer ", (hi >> 24) & 0xFFu);
}

int emmc_init(void)
{
    g_ready = 0;

    pins_init();

    CGU_BASE_SDIO_CLK = V_BASE_SDIO;

    CCU1_SDIO_CFG |= 1u;
    {
        uint32_t i;
        for (i = 0; i < SPIN_SHORT && !(CCU1_SDIO_STAT & 1u); i++)
            ;
        if (i >= SPIN_SHORT)
            return EMMC_FAIL_CLOCK;
    }

    /* Only now is it safe to touch the controller at all. */
    SD_BMOD = 1u;                        /* reset the internal DMA we will not use */
    SD_CTRL = CTRL_RESET | CTRL_FIFO_RST | CTRL_DMA_RST;
    {
        uint32_t i;
        for (i = 0; i < SPIN_SHORT &&
             (SD_CTRL & (CTRL_RESET | CTRL_FIFO_RST | CTRL_DMA_RST)); i++)
            ;
        if (i >= SPIN_SHORT)
            return EMMC_FAIL_RESET;
    }

    SD_PWREN = 1u;                       /* SD_POW on PC_9 */
    SD_CTRL = 0;                         /* polled: no interrupts, no IDMAC */
    SD_INTMASK = 0;
    SD_RINTSTS = 0xFFFFFFFFu;
    SD_TMOUT = 0xFFFFFFFFu;
    SD_CTYPE = 0;                        /* 1 bit - the width every card boots in */
    SD_FIFOTH = 0x100F0010u;             /* RX/TX FIFO watermarks */
    SD_CLKSRC = 0;
    SD_BLKSIZ = EMMC_SECTOR_BYTES;
    SD_BYTCNT = EMMC_SECTOR_BYTES;

    if (!clock_set(CLKDIV_SLOW))
        return EMMC_FAIL_CLOCK;

    board_delay_crude(20000);            /* a millisecond of clock before CMD0 */

    /* CMD0 GO_IDLE_STATE, with the 80-cycle init sequence the card expects. */
    if (!command(0, 0, CMD_SEND_INIT | CMD_WAIT_PRV))
        return EMMC_FAIL_IDLE;

    /* CMD1 SEND_OP_COND, repeated until the card stops reporting busy.
     *
     * 0x40FF8080 asks for every supported voltage *and* sector addressing.
     * A 4 GB part will grant the second, which is what makes an LBA an LBA in
     * CMD17 rather than a byte offset - and 0x8803 sectors is 17.8 MB, well
     * past where a byte offset in 32 bits would still work for the rest of the
     * device. */
    {
        uint32_t i;
        for (i = 0; i < 2000u; i++) {
            if (!command(1, 0x40FF8080u, CMD_RESP_EXP))
                return EMMC_FAIL_OPCOND;
            g_emmc.ocr = SD_RESP(0);
            if (g_emmc.ocr & 0x80000000u)
                break;
            board_delay_crude(20000);
        }
        if (i >= 2000u)
            return EMMC_FAIL_OPCOND;
    }
    g_emmc.sector_addressed = (g_emmc.ocr & 0x40000000u) ? 1 : 0;

    /* CMD2 ALL_SEND_CID - a long response, so no CRC check flag: the card CRCs
     * the 128 bits itself and the controller checks the frame. */
    if (!command(2, 0, CMD_RESP_EXP | CMD_RESP_LONG))
        return EMMC_FAIL_CID;
    save_response(g_emmc.cid, 1);

    /* CMD3 SET_RELATIVE_ADDR. On eMMC the host chooses the address, unlike SD
     * where the card offers one. 1 is as good as any non-zero value. */
    g_emmc.rca = 1;
    if (!command(3, g_emmc.rca << 16, CMD_RESP_EXP | CMD_CHECK_CRC))
        return EMMC_FAIL_RCA;

    if (!command(9, g_emmc.rca << 16, CMD_RESP_EXP | CMD_RESP_LONG))
        return EMMC_FAIL_CSD;
    save_response(g_emmc.csd, 1);

    /* CMD7 SELECT_CARD: stand-by to transfer state. */
    if (!command(7, g_emmc.rca << 16, CMD_RESP_EXP | CMD_CHECK_CRC))
        return EMMC_FAIL_SELECT;

    if (!clock_set(CLKDIV_FAST))
        return EMMC_FAIL_CLOCK;

    if (!command(16, EMMC_SECTOR_BYTES, CMD_RESP_EXP | CMD_CHECK_CRC))
        return EMMC_FAIL_BLOCKLEN;

    g_ready = 1;
    return EMMC_OK;
}

/* ---- reading ------------------------------------------------------------- */

static int read_one(uint32_t lba, uint8_t *dst)
{
    uint32_t arg = g_emmc.sector_addressed ? lba : lba * EMMC_SECTOR_BYTES;
    uint32_t got = 0;

    SD_CTRL |= CTRL_FIFO_RST;
    for (uint32_t i = 0; i < SPIN_SHORT && (SD_CTRL & CTRL_FIFO_RST); i++)
        ;

    SD_RINTSTS = 0xFFFFFFFFu;
    SD_BLKSIZ = EMMC_SECTOR_BYTES;
    SD_BYTCNT = EMMC_SECTOR_BYTES;
    SD_CMDARG = arg;
    SD_CMD = CMD_START | CMD_RESP_EXP | CMD_CHECK_CRC | CMD_DATA_EXP |
             CMD_WAIT_PRV | 17u;

    if (!wait_cmd_taken())
        return 0;

    /* Drain the FIFO as it fills, and stop on DTO - not on having read 512
     * bytes, because a short transfer that ends early has to be caught rather
     * than looked past. The byte count is checked at the end. */
    for (uint32_t i = 0; i < SPIN_LONG; i++) {
        uint32_t s = SD_RINTSTS;

        if (s & INT_ERRORS)
            return 0;

        uint32_t words = STATUS_FIFO_COUNT(SD_STATUS);
        while (words-- && got < EMMC_SECTOR_BYTES) {
            uint32_t w = SD_DATA;
            dst[got + 0] = (uint8_t)(w >> 0);
            dst[got + 1] = (uint8_t)(w >> 8);
            dst[got + 2] = (uint8_t)(w >> 16);
            dst[got + 3] = (uint8_t)(w >> 24);
            got += 4;
            i = 0;                       /* progress resets the patience */
        }

        if ((s & INT_DTO) && STATUS_FIFO_COUNT(SD_STATUS) == 0)
            break;
    }

    return got == EMMC_SECTOR_BYTES;
}

int emmc_read(uint32_t lba, uint32_t count, void *dst)
{
    if (!g_ready)
        return EMMC_FAIL_NOT_READY;

    uint8_t *p = (uint8_t *)dst;
    for (uint32_t n = 0; n < count; n++) {
        /* Let USB breathe. 785 sectors back to back is long enough for the
         * host to conclude the device has stopped answering, which is not a
         * failure mode worth having in a read that is otherwise working. */
        if ((n & 0x1F) == 0)
            log_pump();

        if (!read_one(lba + n, p + n * EMMC_SECTOR_BYTES)) {
            log_dec("emmc read failed at sector", lba + n);
            log_hex("  RINTSTS      ", SD_RINTSTS);
            log_hex("  STATUS       ", SD_STATUS);
            return EMMC_FAIL_READ;
        }
    }
    return EMMC_OK;
}

/* ---- writing -------------------------------------------------------------- */
/* See emmc.h before using any of this. CMD24, one block at a time, polled, with
 * the same bounded waits and error mask as the read path - the
 * only differences are the direction of the FIFO and the busy wait afterwards.
 *
 * The card asserts DATA_BUSY after the data phase and holds it while it commits
 * the block. Issuing the next command before it clears is how a write sequence
 * quietly loses blocks, so it is waited for explicitly rather than assumed. */

#define STATUS_FIFO_FULL   (1u << 3)
#define STATUS_DATA_BUSY   (1u << 9)

static int write_one(uint32_t lba, const uint8_t *srcb)
{
    uint32_t arg = g_emmc.sector_addressed ? lba : lba * EMMC_SECTOR_BYTES;
    uint32_t put = 0;

    SD_CTRL |= CTRL_FIFO_RST;
    for (uint32_t i = 0; i < SPIN_SHORT && (SD_CTRL & CTRL_FIFO_RST); i++)
        ;

    SD_RINTSTS = 0xFFFFFFFFu;
    SD_BLKSIZ = EMMC_SECTOR_BYTES;
    SD_BYTCNT = EMMC_SECTOR_BYTES;
    SD_CMDARG = arg;
    SD_CMD = CMD_START | CMD_RESP_EXP | CMD_CHECK_CRC | CMD_DATA_EXP |
             CMD_WRITE | CMD_WAIT_PRV | 24u;

    if (!wait_cmd_taken())
        return 0;

    for (uint32_t i = 0; i < SPIN_LONG; i++) {
        uint32_t s = SD_RINTSTS;

        if (s & INT_ERRORS)
            return 0;

        while (put < EMMC_SECTOR_BYTES && !(SD_STATUS & STATUS_FIFO_FULL)) {
            SD_DATA = (uint32_t)srcb[put]
                    | ((uint32_t)srcb[put + 1] << 8)
                    | ((uint32_t)srcb[put + 2] << 16)
                    | ((uint32_t)srcb[put + 3] << 24);
            put += 4;
            i = 0;                       /* progress resets the patience */
        }

        if ((s & INT_DTO) && put >= EMMC_SECTOR_BYTES)
            break;
    }

    if (put < EMMC_SECTOR_BYTES)
        return 0;

    for (uint32_t i = 0; i < SPIN_LONG; i++)
        if (!(SD_STATUS & STATUS_DATA_BUSY))
            return 1;

    return 0;                            /* still busy - do not issue another */
}

int emmc_write(uint32_t lba, uint32_t count, const void *src)
{
    if (!g_ready)
        return EMMC_FAIL_NOT_READY;

    const uint8_t *p = (const uint8_t *)src;
    for (uint32_t n = 0; n < count; n++) {
        if ((n & 0x1F) == 0)
            log_pump();

        if (!write_one(lba + n, p + n * EMMC_SECTOR_BYTES)) {
            log_dec("emmc WRITE failed at sector", lba + n);
            log_hex("  RINTSTS      ", SD_RINTSTS);
            log_hex("  STATUS       ", SD_STATUS);
            return EMMC_FAIL_WRITE;
        }
    }
    return EMMC_OK;
}
