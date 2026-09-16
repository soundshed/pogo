#include "midi_control/midi_control_crc.h"

/* Bitwise rather than table-driven: 32 iterations a byte is nothing at the
 * sizes this project deals in, and it keeps 1 KB of table out of a
 * RAM-resident image. */
uint32_t midi_control_crc32_update(uint32_t crc, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = ~crc;

    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

uint32_t midi_control_crc32(const void *data, uint32_t len)
{
    return midi_control_crc32_update(0, data, len);
}
