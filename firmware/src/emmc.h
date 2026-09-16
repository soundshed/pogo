#ifndef MIDI_CONTROL_EMMC_H
#define MIDI_CONTROL_EMMC_H

#include <stdint.h>

/* The 4 GB eMMC.
 *
 * This file was read-only for the whole investigation, and its header said so:
 * "there is no write path and there is not going to be one". That was the right
 * rule while nothing needed writing, and it is recorded here rather than quietly
 * deleted, because the reasoning behind it has not changed - **the eMMC is the
 * only storage on this unit that a power cycle does not restore.**
 *
 * A write path exists now because persistent firmware requires one: the NOR
 * bootloader loads the application from LBA 1 (docs/05 section 3), so replacing
 * what the unit runs means writing there. That was allowed only once three
 * things were true, and none of them is optional if this is ever revisited:
 *
 *   1. The region is backed up, byte-exact, and the backup is verified against
 *      an independently obtained copy - not merely against a second read of
 *      itself, since a deterministic read error reproduces perfectly.
 *   2. There is a proven way back in if what gets written does not run.
 *      `PAGE >` held at power-on enters the boot ROM's DFU mode, measured on
 *      this unit (docs/14 section 1).
 *   3. The QSPI NOR is not touched, so the bootloader and its recovery path
 *      survive whatever happens here.
 *
 * What the eMMC holds: the section descriptor at LBA 0, the M4 application
 * image from LBA 1, and the SHARC's boot image at LBA 0x8803, all at raw sector
 * offsets with no filesystem (docs/02 section 4).
 */

#define EMMC_SECTOR_BYTES   512u

/* Where the SHARC image sits, from the LPC's own boot code at 0x28c5c3b6. */
#define EMMC_SHARC_LBA      0x8803u
#define EMMC_SHARC_BYTES    401744u
#define EMMC_SHARC_SECTORS  785u

enum {
    EMMC_OK = 0,
    EMMC_FAIL_CLOCK,      /* the CCU branch never reported itself running */
    EMMC_FAIL_RESET,      /* CTRL reset bits never cleared */
    EMMC_FAIL_IDLE,       /* CMD0 did not complete */
    EMMC_FAIL_OPCOND,     /* CMD1 never came out of busy - no card, or no power */
    EMMC_FAIL_CID,        /* CMD2 */
    EMMC_FAIL_RCA,        /* CMD3 */
    EMMC_FAIL_CSD,        /* CMD9 */
    EMMC_FAIL_SELECT,     /* CMD7 */
    EMMC_FAIL_BLOCKLEN,   /* CMD16 */
    EMMC_FAIL_READ,       /* CMD17, or the data phase after it */
    EMMC_FAIL_NOT_READY,  /* read attempted before a successful init */
    EMMC_FAIL_WRITE,      /* CMD24, its data phase, or the busy wait after it */
};

struct emmc_info {
    uint32_t ocr;
    uint32_t cid[4];
    uint32_t csd[4];
    uint32_t rca;
    int sector_addressed;     /* OCR bit 30: CMD17 takes an LBA, not a byte offset */
    uint32_t clock_hz;        /* what the card clock ended up at */
};

extern struct emmc_info g_emmc;

/* Log the clock tree and pin mux feeding the controller *without touching the
 * controller itself*.
 *
 * Deliberately split from emmc_init(): reading a peripheral register whose CCU
 * branch is not running does not fault on this part, it stalls the bus with no
 * way to report it. That failure mode cost four builds during the EMC work
 * (docs/13 section 5), so the census reads only registers that are always
 * clocked, and the controller's own state is logged by emmc_log_regs() after
 * the branch has been enabled and seen to run. */
void emmc_census(void);

/* Pins, clock, controller reset, then the card: CMD0, CMD1, CMD2, CMD3, CMD9,
 * CMD7, CMD16. Leaves the bus 1-bit and the clock modest - this is for reading
 * 400 KB once, not for throughput. Returns 0 or one of the codes above. */
int emmc_init(void);

/* `count` sectors from `lba` into `dst`, as `count` single-block reads. */
int emmc_read(uint32_t lba, uint32_t count, void *dst);

/* `count` sectors from `src` to `lba`, as `count` single-block writes (CMD24).
 *
 * This cannot be undone by a power cycle. Read the header of this file before
 * calling it, and never call it without a verified backup of the sectors it
 * will land on. It does not verify what it wrote - the caller should read the
 * sectors back and compare, because a write that reports success and a write
 * that landed are not the same claim. */
int emmc_write(uint32_t lba, uint32_t count, const void *src);

/* Controller registers, safe only once emmc_init() has run. */
void emmc_log_regs(void);

/* Decoded CID fields, which name the part - a hardware fact we do not have
 * from anywhere else. */
void emmc_log_card(void);

#endif
