/* Persistent configuration on the eMMC.
 *
 * Two slots of one sector each at LBA 1300 and 1316 (midi_control_bootimage.h),
 * inside the sectors 0..1364 region a verified factory backup exists for and
 * well clear of our own image at LBA 1. The slots are only ever written when
 * the image at LBA 1 is one of ours: with the factory image installed, sectors
 * 1300+ are inside its CRC-checked body, and writing them would stop the unit
 * booting until it was restored. midi_control_boot_classify()
 * makes that decision from the header at LBA 1 (docs/06-persistence.md).
 */
#ifndef MIDI_CONTROL_STORAGE_H
#define MIDI_CONTROL_STORAGE_H

#include <stdint.h>

/* Bring up the eMMC and classify what is on it. Returns a
 * MIDI_CONTROL_STORAGE_* state from midi_control_protocol.h. Safe to call more than once. */
int storage_init(void);

/* The state established by storage_init(), without touching the card again. */
int storage_state(void);

/* 1 if the card initialised, whatever is on it - the dump and restore
 * commands need the card, not our image. */
int storage_card_ready(void);

/* The midi_control_platform_ops callbacks. */
int storage_ops_state(void *ctx);
int storage_ops_read(void *ctx, unsigned slot, uint8_t *buf, uint32_t len);
int storage_ops_write(void *ctx, unsigned slot, const uint8_t *buf, uint32_t len);

/* Re-read LBA 1 and classify again - after an install or a restore, so the
 * reported state follows what is actually on the card. */
int storage_reclassify(void);

#endif
