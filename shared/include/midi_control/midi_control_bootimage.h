/* The image formats the POD Go's boot chain uses, and the eMMC layout this
 * project commits to.
 *
 * Two formats (docs/02-firmware-platform.md section 3):
 *
 *   NXP DFU image      16-byte header `1a 00 <blocks:u16 LE> 00*12`, then the
 *                      image body padded to 512-byte blocks. What the boot ROM
 *                      accepts over USB DFU; runs from RAM, gone at power-off.
 *
 *   Bootloader image   0x34-byte header + scatter-load records, CRC32 checked
 *                      and loaded from eMMC LBA 1 on every power-on. Persistent.
 *
 * eMMC layout (docs/06-persistence.md):
 *
 *   LBA 0              section descriptor for the image at LBA 1
 *   LBA 1..            the bootloader image (ours is ~40 sectors)
 *   LBA 1300, 1316     the two configuration slots
 *   LBA 0..1364        the region a verified factory backup exists for; nothing
 *                      in this project writes outside it
 *   LBA 0x8803         the SHARC's boot image. Never touched.
 */
#ifndef MIDI_CONTROL_BOOTIMAGE_H
#define MIDI_CONTROL_BOOTIMAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_CONTROL_SECTOR_BYTES            512u

#define MIDI_CONTROL_BOOT_MAGIC              0x230B0C5Eu
#define MIDI_CONTROL_BOOT_HEADER_LEN         0x34u
#define MIDI_CONTROL_BOOT_RECORD_LEN         12u
#define MIDI_CONTROL_BOOT_HANDOFF_SPAN       0x14u
#define MIDI_CONTROL_DESCRIPTOR_MAGIC        0x56112603u
#define MIDI_CONTROL_DESCRIPTOR_WORD1        0x02500000u   /* as shipped; meaning not established */

#define MIDI_CONTROL_DFU_HEADER_LEN          16u

/* Where our things live on the eMMC, in sectors. */
#define MIDI_CONTROL_EMMC_IMAGE_LBA          1u
#define MIDI_CONTROL_EMMC_IMAGE_MAX_SECTORS  1299u   /* sectors 1..1299; the slots start at 1300 */
#define MIDI_CONTROL_EMMC_CONFIG_SLOT_LBA(n) (1300u + 16u * (n))
#define MIDI_CONTROL_EMMC_CONFIG_SLOT_SECTORS 16u
#define MIDI_CONTROL_EMMC_SAFE_SECTORS       1365u   /* 0..1364: the backed-up region */

/* The factory image, for recognising it: seven records, ~698 KB. Anything with
 * one record under 256 KB is one of ours. */
#define MIDI_CONTROL_BOOT_OURS_MAX_SIZE      0x40000u

struct midi_control_boot_record {
    uint32_t len, dest, run;
};

struct midi_control_boot_info {
    int has_descriptor;          /* input began with sector 0 */
    uint32_t desc_word1, desc_crc, desc_size;
    int desc_crc_ok;

    uint32_t magic, size, crc, version, records, entry, handoff, handoff_len, flags;
    int magic_ok, crc_ok, version_ok;
    uint32_t record_count;       /* records parsed, up to 8 */
    struct midi_control_boot_record record[8];
    int records_in_range;        /* every record inside a range the loader accepts */
    uint32_t sectors;            /* descriptor + image, rounded up */
};

/* Parse an image with or without the sector-0 descriptor. Returns 1 if a
 * header was found at all (check the *_ok fields), 0 if too short. */
int midi_control_boot_inspect(const uint8_t *img, uint32_t len, struct midi_control_boot_info *info);

/* Is `sector1` (512 bytes) the header of one of our images, the factory image,
 * or something else? Returns MIDI_CONTROL_STORAGE_READY, MIDI_CONTROL_STORAGE_LOCKED or
 * MIDI_CONTROL_STORAGE_UNKNOWN from midi_control_protocol.h. */
int midi_control_boot_classify(const uint8_t *sector1);

/* Build the eMMC layout - descriptor sector, then the bootloader image with a
 * single record loading `raw` at `dest`, padded to whole sectors - from a raw
 * image. If `raw` begins with the 16-byte NXP DFU header it is stripped, so a
 * built .bin can be used directly. The entry point is word 1 of the vector
 * table; the handoff block is placed just past the image.
 *
 * Returns the number of bytes written to `out` (a multiple of 512), or 0 with
 * `*err` naming why. `out` needs raw_len + 2 * MIDI_CONTROL_SECTOR_BYTES + 64. */
uint32_t midi_control_boot_build_emmc(const uint8_t *raw, uint32_t raw_len, uint32_t dest,
                                      uint8_t *out, uint32_t out_max, const char **err);

/* Wrap a raw image in the NXP DFU header. Returns bytes written; `out` needs
 * raw_len + 16 + 511. */
uint32_t midi_control_dfu_wrap(const uint8_t *raw, uint32_t raw_len, uint8_t *out, uint32_t out_max);

/* The loader's range check: the name of the RAM region holding [lo, hi), or
 * NULL if the loader would refuse it. */
const char *midi_control_boot_range_name(uint32_t lo, uint32_t hi);

/* ---- factory release files -------------------------------------------------
 *
 * A POD Go firmware release (`.hxf`) is an IFF container, big-endian, form type
 * `L6FF`: a HEAD chunk, `dinf` records laying out the inflated payload, and one
 * zlib-compressed `data` chunk. The main content (the `dinf` record with id 0)
 * is stored with the bytes of every 32-bit word reversed, and is a chain of
 * sections: a 16-byte descriptor and a 0x1f0-byte header - one sector together -
 * then the section data. The first section is the application, and it is what
 * the eMMC holds from sector 0 (docs/06-persistence.md section 4).
 *
 * Inflating is the caller's; these only parse and copy. */

struct midi_control_hxf {
    uint32_t data_offset, data_len;  /* the zlib stream, within the file */
    uint32_t main_len;               /* inflated length of the main content */
};

/* Find the compressed payload and the main content's length in a release
 * file. Returns 1, or 0 with `*err` naming why. */
int midi_control_hxf_parse(const uint8_t *file, uint32_t len, struct midi_control_hxf *out, const char **err);

/* The factory eMMC region, from the start of the inflated main content as the
 * release stores it: the application section's descriptor sector and data with
 * the byte order restored, zero padded to MIDI_CONTROL_EMMC_SAFE_SECTORS
 * sectors. Refused unless it is a factory image the bootloader would start: both
 * CRCs, the version word, every record in range, and more than one record (so
 * not one of ours).
 *
 * Returns MIDI_CONTROL_EMMC_SAFE_SECTORS * 512, the bytes written to `out`, or
 * 0 with `*err` naming why. */
uint32_t midi_control_hxf_factory_emmc(const uint8_t *main_content, uint32_t main_len,
                                       uint8_t *out, uint32_t out_max, const char **err);

/* Whether a factory image is the release a unit had, from a dump of its eMMC.
 *
 * Installing one of our images writes its own sectors - at most the descriptor
 * and MIDI_CONTROL_BOOT_OURS_MAX_SIZE, so nothing past sector 512 - and the
 * configuration slots. The factory image it replaced stays everywhere else. So the
 * bytes from sector 513 to the end of the factory image, less the slots, say
 * which release the unit had, whatever has been installed since. The slots are
 * left out even when sector 1 holds a factory image: a restore writes upward from
 * sector 0, so one that stopped part-way leaves the factory header at sector 1
 * and a saved configuration still in the slots.
 *
 * `emmc` and `factory` are both `len` bytes from sector 0. */
struct midi_control_residual {
    int emmc_state;              /* midi_control_boot_classify() of the eMMC's sector 1 */
    int identical;               /* the eMMC already holds exactly the factory image */
    int head_identical;          /* sectors 0 and 1 already hold it, as a restore of it that
                                    stopped part-way leaves them */
    uint32_t from, to;           /* the residual compared, in bytes from sector 0 */
    uint32_t compared;           /* bytes compared */
    uint32_t differing;          /* of those, how many differ */
    uint32_t first_difference;   /* the offset of the first, when any differ */
};

void midi_control_boot_residual_compare(const uint8_t *emmc, const uint8_t *factory, uint32_t len,
                                        struct midi_control_residual *out);

#ifdef __cplusplus
}
#endif
#endif
