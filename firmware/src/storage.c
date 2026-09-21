#include <stdint.h>
#include "storage.h"
#include "board.h"
#include "emmc.h"
#include "log.h"
#include "midi_control/midi_control_bootimage.h"
#include "midi_control/midi_control_protocol.h"

static int g_inited;
static int g_card_ok;
static int g_state = MIDI_CONTROL_STORAGE_NONE;

static uint8_t g_sector[EMMC_SECTOR_BYTES] __attribute__((aligned(4)));

int storage_reclassify(void)
{
    if (!g_card_ok) {
        g_state = MIDI_CONTROL_STORAGE_NONE;
        return g_state;
    }
    if (emmc_read(MIDI_CONTROL_EMMC_IMAGE_LBA, 1, g_sector) != EMMC_OK) {
        log_line("storage: LBA 1 unreadable");
        g_state = MIDI_CONTROL_STORAGE_UNKNOWN;
        return g_state;
    }
    g_state = midi_control_boot_classify(g_sector);
    log_dec("storage: state", (uint32_t)g_state);
    return g_state;
}

int storage_init(void)
{
    if (g_inited)
        return g_state;
    g_inited = 1;

    emmc_census();

    /* A few attempts rather than one. The card init is a chain of commands with
     * bounded waits; a transient miss on the first CMD (e.g. straight after the
     * clock has just been brought up) should not condemn the whole session to
     * NO STORAGE when a retry on a settled clock succeeds. */
    int rc = EMMC_FAIL_CLOCK;
    for (unsigned attempt = 0; attempt < 3u; attempt++) {
        rc = emmc_init();
        log_dec("storage: emmc_init rc", (uint32_t)rc);
        if (rc == EMMC_OK)
            break;
        board_delay_crude(20000);
    }
    g_card_ok = rc == EMMC_OK;
    if (g_card_ok)
        emmc_log_card();
    return storage_reclassify();
}

int storage_state(void)
{
    return g_state;
}

int storage_card_ready(void)
{
    return g_card_ok;
}

int storage_ops_state(void *ctx)
{
    (void)ctx;
    return g_state;
}

/* Sectors a record of `len` bytes occupies, and whether that many fit the slot
 * docs/06 reserves. A record has grown past one sector (the profile block), so
 * a slot is read and written a sector at a time - through the same single
 * sector buffer, so the code below is the only RAM this costs however long the
 * record becomes. */
static uint32_t slot_sectors(uint32_t len)
{
    uint32_t n = (len + EMMC_SECTOR_BYTES - 1u) / EMMC_SECTOR_BYTES;
    return n <= MIDI_CONTROL_EMMC_CONFIG_SLOT_SECTORS ? n : 0u;
}

int storage_ops_read(void *ctx, unsigned slot, uint8_t *buf, uint32_t len)
{
    const uint32_t sectors = slot_sectors(len);

    (void)ctx;
    if (!g_card_ok || slot >= 2u || sectors == 0u)
        return 0;
    for (uint32_t s = 0; s < sectors; s++) {
        const uint32_t off = s * EMMC_SECTOR_BYTES;
        const uint32_t n = len - off < EMMC_SECTOR_BYTES ? len - off : EMMC_SECTOR_BYTES;
        if (emmc_read(MIDI_CONTROL_EMMC_CONFIG_SLOT_LBA(slot) + s, 1, g_sector) != EMMC_OK)
            return 0;
        for (uint32_t i = 0; i < n; i++)
            buf[off + i] = g_sector[i];
    }
    return 1;
}

int storage_ops_write(void *ctx, unsigned slot, const uint8_t *buf, uint32_t len)
{
    const uint32_t sectors = slot_sectors(len);

    (void)ctx;
    if (!g_card_ok)
        return MIDI_CONTROL_STATUS_NO_STORAGE;
    if (slot >= 2u || sectors == 0u)
        return MIDI_CONTROL_STATUS_INVALID_VALUE;
    /* The device layer checks the state before calling; check again here so
     * no path can write a slot over the factory image. */
    if (g_state != MIDI_CONTROL_STORAGE_READY)
        return MIDI_CONTROL_STATUS_STORAGE_LOCKED;

    /* Sector by sector, ascending, each read back before the next is written.
     * A write that stops part-way leaves this slot failing its record's CRC,
     * and the device only ever writes the slot that does not hold the newest
     * record - so the other slot still does, and is what the next boot loads
     * (docs/06 section 1). slot_sectors() has already held the last sector
     * inside the sixteen this slot owns, so this never reaches the next slot
     * or anything past sector 1331. */
    for (uint32_t s = 0; s < sectors; s++) {
        const uint32_t lba = MIDI_CONTROL_EMMC_CONFIG_SLOT_LBA(slot) + s;
        const uint32_t off = s * EMMC_SECTOR_BYTES;
        const uint32_t n = len - off < EMMC_SECTOR_BYTES ? len - off : EMMC_SECTOR_BYTES;

        for (uint32_t i = 0; i < EMMC_SECTOR_BYTES; i++)
            g_sector[i] = i < n ? buf[off + i] : 0u;
        log_dec("storage: write LBA", lba);
        if (emmc_write(lba, 1, g_sector) != EMMC_OK) {
            log_dec("storage: write failed, slot", slot);
            return MIDI_CONTROL_STATUS_WRITE_FAILED;
        }

        /* A write that returns success and a write that landed are two
         * different claims. Read it back. */
        for (uint32_t i = 0; i < EMMC_SECTOR_BYTES; i++)
            g_sector[i] = 0xFFu;
        if (emmc_read(lba, 1, g_sector) != EMMC_OK)
            return MIDI_CONTROL_STATUS_VERIFY_FAILED;
        for (uint32_t i = 0; i < n; i++)
            if (g_sector[i] != buf[off + i]) {
                log_dec("storage: verify failed, slot", slot);
                return MIDI_CONTROL_STATUS_VERIFY_FAILED;
            }
    }
    log_dec("storage: saved slot", slot);
    return MIDI_CONTROL_STATUS_OK;
}
