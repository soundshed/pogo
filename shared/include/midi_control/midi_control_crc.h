/* CRC32, the standard reflected form (polynomial 0xEDB88320, initial and final
 * inversion) - the same function zlib's crc32() computes, and the one the POD
 * Go's NOR bootloader checks images with. Verified against the reference
 * vector "123456789" -> 0xCBF43926 in tests/test_shared.c. */
#ifndef MIDI_CONTROL_CRC_H
#define MIDI_CONTROL_CRC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t midi_control_crc32(const void *data, uint32_t len);

/* Incremental form: start with 0, feed chunks, and the result of the last call
 * is the CRC of the concatenation. */
uint32_t midi_control_crc32_update(uint32_t crc, const void *data, uint32_t len);

#ifdef __cplusplus
}
#endif
#endif
