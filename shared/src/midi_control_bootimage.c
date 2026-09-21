#include <string.h>
#include "midi_control/midi_control_bootimage.h"
#include "midi_control/midi_control_crc.h"
#include "midi_control/midi_control_protocol.h"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* The four ranges accepted by the NOR bootloader; anything else is rejected
 * before a byte is written. */
static const struct { uint32_t lo, hi; const char *name; } k_ranges[4] = {
    { 0x10000000u, 0x10020000u, "local SRAM bank 0" },
    { 0x10080000u, 0x10092000u, "local SRAM bank 2" },
    { 0x20000000u, 0x20010000u, "AHB SRAM" },
    { 0x28000000u, 0x29F00000u, "SDRAM" },
};

const char *midi_control_boot_range_name(uint32_t lo, uint32_t hi)
{
    /* Callers pass lo + length, so an end below the start wrapped past 4 GB. */
    if (hi < lo)
        return NULL;
    for (unsigned i = 0; i < 4; i++)
        if (k_ranges[i].lo <= lo && lo < k_ranges[i].hi && hi < k_ranges[i].hi)
            return k_ranges[i].name;
    return NULL;
}

int midi_control_boot_inspect(const uint8_t *img, uint32_t len, struct midi_control_boot_info *info)
{
    memset(info, 0, sizeof(*info));

    if (len >= MIDI_CONTROL_SECTOR_BYTES + MIDI_CONTROL_BOOT_HEADER_LEN && rd32(img) == MIDI_CONTROL_DESCRIPTOR_MAGIC) {
        info->has_descriptor = 1;
        info->desc_word1 = rd32(img + 4);
        info->desc_crc = rd32(img + 8);
        info->desc_size = rd32(img + 12);
        if (info->desc_size <= len - MIDI_CONTROL_SECTOR_BYTES)
            info->desc_crc_ok = midi_control_crc32(img + MIDI_CONTROL_SECTOR_BYTES, info->desc_size) == info->desc_crc;
        img += MIDI_CONTROL_SECTOR_BYTES;
        len -= MIDI_CONTROL_SECTOR_BYTES;
    }

    if (len < MIDI_CONTROL_BOOT_HEADER_LEN)
        return 0;

    info->magic = rd32(img + 0x00);
    info->size = rd32(img + 0x04);
    info->crc = rd32(img + 0x08);
    info->version = rd32(img + 0x0C);
    info->records = rd32(img + 0x10);
    info->entry = rd32(img + 0x14);
    info->handoff = rd32(img + 0x18);
    info->handoff_len = rd32(img + 0x1C);
    info->flags = rd32(img + 0x20);
    info->magic_ok = info->magic == MIDI_CONTROL_BOOT_MAGIC;
    info->version_ok = info->version == 2u;

    if (info->size >= MIDI_CONTROL_BOOT_HEADER_LEN && info->size <= len)
        info->crc_ok = midi_control_crc32(img + MIDI_CONTROL_BOOT_HEADER_LEN, info->size - MIDI_CONTROL_BOOT_HEADER_LEN)
                       == info->crc;

    /* Every length below comes from the file, so each bound is a subtraction
     * from what is left: a sum of two of them can wrap. */
    info->records_in_range = 1;
    const uint32_t limit = info->size < len ? info->size : len;
    uint32_t off = MIDI_CONTROL_BOOT_HEADER_LEN;
    for (uint32_t i = 0; i < info->records && i < 8u; i++) {
        if (off > limit || limit - off < MIDI_CONTROL_BOOT_RECORD_LEN)
            break;
        struct midi_control_boot_record *r = &info->record[i];
        r->len = rd32(img + off);
        r->dest = rd32(img + off + 4);
        r->run = rd32(img + off + 8);
        info->record_count++;
        if (r->len && !midi_control_boot_range_name(r->dest, r->dest + r->len))
            info->records_in_range = 0;
        off += MIDI_CONTROL_BOOT_RECORD_LEN;
        if (r->len > limit - off)
            break;                  /* its data runs past the image: nothing follows */
        off += r->len;
    }

    info->sectors = (info->has_descriptor ? 1u : 0u)
                  + info->size / MIDI_CONTROL_SECTOR_BYTES
                  + (info->size % MIDI_CONTROL_SECTOR_BYTES ? 1u : 0u);
    return 1;
}

int midi_control_boot_classify(const uint8_t *sector1)
{
    if (rd32(sector1) != MIDI_CONTROL_BOOT_MAGIC)
        return MIDI_CONTROL_STORAGE_UNKNOWN;
    uint32_t size = rd32(sector1 + 0x04);
    uint32_t records = rd32(sector1 + 0x10);
    if (records == 1u && size <= MIDI_CONTROL_BOOT_OURS_MAX_SIZE)
        return MIDI_CONTROL_STORAGE_READY;
    return MIDI_CONTROL_STORAGE_LOCKED;
}

uint32_t midi_control_boot_build_emmc(const uint8_t *raw, uint32_t raw_len, uint32_t dest,
                                      uint8_t *out, uint32_t out_max, const char **err)
{
    const char *e = NULL;

    /* A .bin from build.py carries the NXP DFU header; strip it, and the
     * block padding that goes with it is harmless. */
    if (raw_len > MIDI_CONTROL_DFU_HEADER_LEN && raw[0] == 0x1A && raw[1] == 0x00) {
        uint32_t blocks = (uint32_t)raw[2] | ((uint32_t)raw[3] << 8);
        raw += MIDI_CONTROL_DFU_HEADER_LEN;
        raw_len -= MIDI_CONTROL_DFU_HEADER_LEN;
        if (blocks * MIDI_CONTROL_SECTOR_BYTES < raw_len)
            raw_len = blocks * MIDI_CONTROL_SECTOR_BYTES;
    }

    if (raw_len < 8u)
        e = "image too short to hold a vector table";
    else if (!midi_control_boot_range_name(dest, dest + raw_len))
        e = "destination is outside every range the loader accepts";

    uint32_t entry = raw_len >= 8u ? rd32(raw + 4) : 0;
    if (!e && !midi_control_boot_range_name(entry, entry))
        e = "entry point (vector word 1) is outside every range the loader accepts";
    if (!e && (entry & 1u) == 0)
        e = "entry point has no Thumb bit";

    uint32_t handoff = (dest + raw_len + 3u) & ~3u;
    if (!e && !midi_control_boot_range_name(handoff, handoff + MIDI_CONTROL_BOOT_HANDOFF_SPAN))
        e = "handoff block would fall outside RAM the loader accepts";

    uint32_t body_len = MIDI_CONTROL_BOOT_RECORD_LEN + raw_len;
    uint32_t size = MIDI_CONTROL_BOOT_HEADER_LEN + body_len;
    uint32_t image_sectors = (size + MIDI_CONTROL_SECTOR_BYTES - 1u) / MIDI_CONTROL_SECTOR_BYTES;
    uint32_t total = MIDI_CONTROL_SECTOR_BYTES * (1u + image_sectors);

    if (!e && image_sectors > MIDI_CONTROL_EMMC_IMAGE_MAX_SECTORS)
        e = "image would overlap the configuration slots at sector 1300";
    if (!e && total > out_max)
        e = "output buffer too small";
    if (e) {
        if (err)
            *err = e;
        return 0;
    }

    memset(out, 0, total);
    uint8_t *img = out + MIDI_CONTROL_SECTOR_BYTES;

    wr32(img + 0x00, MIDI_CONTROL_BOOT_MAGIC);
    wr32(img + 0x04, size);
    wr32(img + 0x0C, 2u);
    wr32(img + 0x10, 1u);
    wr32(img + 0x14, entry);
    wr32(img + 0x18, handoff);
    wr32(img + 0x1C, 0u);

    uint8_t *rec = img + MIDI_CONTROL_BOOT_HEADER_LEN;
    wr32(rec + 0, raw_len);
    wr32(rec + 4, dest);
    wr32(rec + 8, MIDI_CONTROL_BOOT_HEADER_LEN + MIDI_CONTROL_BOOT_RECORD_LEN + raw_len);
    memcpy(rec + MIDI_CONTROL_BOOT_RECORD_LEN, raw, raw_len);

    wr32(img + 0x08, midi_control_crc32(img + MIDI_CONTROL_BOOT_HEADER_LEN, body_len));

    wr32(out + 0, MIDI_CONTROL_DESCRIPTOR_MAGIC);
    wr32(out + 4, MIDI_CONTROL_DESCRIPTOR_WORD1);
    wr32(out + 8, midi_control_crc32(img, size));
    wr32(out + 12, size);

    if (err)
        *err = NULL;
    return total;
}

uint32_t midi_control_dfu_wrap(const uint8_t *raw, uint32_t raw_len, uint8_t *out, uint32_t out_max)
{
    uint32_t blocks = (raw_len + MIDI_CONTROL_SECTOR_BYTES - 1u) / MIDI_CONTROL_SECTOR_BYTES;
    uint32_t total = MIDI_CONTROL_DFU_HEADER_LEN + blocks * MIDI_CONTROL_SECTOR_BYTES;

    if (blocks == 0 || blocks > 0xFFFFu || total > out_max)
        return 0;
    memset(out, 0, total);
    out[0] = 0x1A;
    out[1] = 0x00;
    out[2] = (uint8_t)blocks;
    out[3] = (uint8_t)(blocks >> 8);
    memcpy(out + MIDI_CONTROL_DFU_HEADER_LEN, raw, raw_len);
    return total;
}

/* ---- factory release files ----------------------------------------------- */

static uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

#define HXF_CHUNK_HEADER    8u
#define HXF_MAIN_ID         0u

/* The application section opens the main content - at 0x3c in v2.50 - so a
 * descriptor further in than this is not it. */
#define HXF_SECTION_SEARCH  0x10000u

/* The first sector past anything one of our images can occupy. */
#define RESIDUAL_FIRST_SECTOR \
    (1u + (MIDI_CONTROL_BOOT_OURS_MAX_SIZE + MIDI_CONTROL_SECTOR_BYTES - 1u) / MIDI_CONTROL_SECTOR_BYTES)

int midi_control_hxf_parse(const uint8_t *file, uint32_t len, struct midi_control_hxf *out, const char **err)
{
    const char *e = NULL;
    int have_main = 0;

    memset(out, 0, sizeof(*out));
    if (len < 12u || memcmp(file, "FORM", 4) != 0 || memcmp(file + 8, "L6FF", 4) != 0)
        e = "not a factory release file (no FORM L6FF header)";
    else if (rd32be(file + 4) > len - 8u)
        e = "the release file is truncated";

    const uint32_t end = e ? 0u : 8u + rd32be(file + 4);
    uint32_t off = 12u;
    while (!e && off + HXF_CHUNK_HEADER <= end) {
        const uint8_t *c = file + off;
        const uint32_t size = rd32be(c + 4);
        if (size > end - off - HXF_CHUNK_HEADER) {
            e = "a chunk runs past the end of the release file";
            break;
        }
        /* dinf: id, flags, offset, size. The main content is id 0, at offset 0. */
        if (memcmp(c, "dinf", 4) == 0 && size >= 16u
            && rd32be(c + 8) == HXF_MAIN_ID && rd32be(c + 16) == 0u) {
            out->main_len = rd32be(c + 20);
            have_main = 1;
        } else if (memcmp(c, "data", 4) == 0) {
            out->data_offset = off + HXF_CHUNK_HEADER;
            out->data_len = size;
        }
        off += HXF_CHUNK_HEADER + size + (size & 1u);
    }

    if (!e && !have_main)
        e = "the release file has no layout record for its main content";
    if (!e && out->data_len == 0u)
        e = "the release file has no compressed payload";
    if (err)
        *err = e;
    return e == NULL;
}

uint32_t midi_control_hxf_factory_emmc(const uint8_t *main_content, uint32_t main_len,
                                       uint8_t *out, uint32_t out_max, const char **err)
{
    const uint32_t total = MIDI_CONTROL_EMMC_SAFE_SECTORS * MIDI_CONTROL_SECTOR_BYTES;
    const uint32_t words = main_len & ~3u;
    const char *e = out_max < total ? "output buffer too small" : NULL;
    uint32_t at = 0, span = 0;
    int found = 0;

    /* Stored byte-swapped, so a little-endian word reads big-endian here. */
    for (uint32_t off = 0; !e && !found && off < HXF_SECTION_SEARCH
                           && off + MIDI_CONTROL_SECTOR_BYTES + MIDI_CONTROL_BOOT_HEADER_LEN <= words; off += 4u) {
        if (rd32be(main_content + off) != MIDI_CONTROL_DESCRIPTOR_MAGIC
            || rd32be(main_content + off + MIDI_CONTROL_SECTOR_BYTES) != MIDI_CONTROL_BOOT_MAGIC)
            continue;
        const uint32_t size = rd32be(main_content + off + 12u);
        found = 1;
        at = off;
        if (size < MIDI_CONTROL_BOOT_HEADER_LEN || size > total - MIDI_CONTROL_SECTOR_BYTES) {
            e = "the application section does not fit the backed-up region";
        } else {
            span = MIDI_CONTROL_SECTOR_BYTES + ((size + 3u) & ~3u);
            if (span > words - off)
                e = "the application section runs past the end of the payload";
        }
    }
    if (!e && !found)
        e = "no application section at the start of the payload";

    if (!e) {
        memset(out, 0, total);
        for (uint32_t i = 0; i < span; i += 4u) {
            out[i] = main_content[at + i + 3u];
            out[i + 1u] = main_content[at + i + 2u];
            out[i + 2u] = main_content[at + i + 1u];
            out[i + 3u] = main_content[at + i];
        }
        /* The last word can run past the section's data, which is not the section. */
        const uint32_t used = MIDI_CONTROL_SECTOR_BYTES + rd32(out + 12u);
        memset(out + used, 0, span - used);

        struct midi_control_boot_info info;
        midi_control_boot_inspect(out, total, &info);
        if (!info.has_descriptor || !info.desc_crc_ok)
            e = "the section CRC does not match its data";
        else if (!info.magic_ok || !info.version_ok || info.size != info.desc_size)
            e = "the application header is not one the bootloader accepts";
        else if (!info.crc_ok)
            e = "the application image CRC does not match";
        else if (!info.records_in_range || info.record_count != (info.records < 8u ? info.records : 8u))
            e = "a load record is outside the ranges the bootloader accepts";
        else if (info.records < 2u
                 || midi_control_boot_classify(out + MIDI_CONTROL_SECTOR_BYTES) != MIDI_CONTROL_STORAGE_LOCKED)
            e = "the section is not a factory application image";
        if (e)
            memset(out, 0, total);
    }

    if (err)
        *err = e;
    return e ? 0u : total;
}

void midi_control_boot_residual_compare(const uint8_t *emmc, const uint8_t *factory, uint32_t len,
                                        struct midi_control_residual *out)
{
    struct midi_control_boot_info f;

    memset(out, 0, sizeof(*out));
    out->emmc_state = MIDI_CONTROL_STORAGE_UNKNOWN;
    if (len < 2u * MIDI_CONTROL_SECTOR_BYTES || !midi_control_boot_inspect(factory, len, &f)
        || !f.has_descriptor || f.desc_size > len - MIDI_CONTROL_SECTOR_BYTES)
        return;

    out->emmc_state = midi_control_boot_classify(emmc + MIDI_CONTROL_SECTOR_BYTES);
    out->to = MIDI_CONTROL_SECTOR_BYTES + f.desc_size;
    out->identical = memcmp(emmc, factory, out->to) == 0;
    out->head_identical = memcmp(emmc, factory, 2u * MIDI_CONTROL_SECTOR_BYTES) == 0;
    out->from = RESIDUAL_FIRST_SECTOR * MIDI_CONTROL_SECTOR_BYTES;
    if (out->from > out->to)
        out->from = out->to;

    /* The slots are left out whatever sector 1 holds. A restore writes upward
     * from sector 0, so one that stopped short of them leaves the factory
     * header at sector 1 and a saved configuration still in the slots. */
    const uint32_t slots_from = MIDI_CONTROL_EMMC_CONFIG_SLOT_LBA(0u) * MIDI_CONTROL_SECTOR_BYTES;
    const uint32_t slots_to = MIDI_CONTROL_EMMC_CONFIG_SLOT_LBA(2u) * MIDI_CONTROL_SECTOR_BYTES;
    for (uint32_t i = out->from; i < out->to; i++) {
        if (i >= slots_from && i < slots_to)
            continue;
        out->compared++;
        if (emmc[i] != factory[i]) {
            if (out->differing == 0u)
                out->first_difference = i;
            out->differing++;
        }
    }
}
