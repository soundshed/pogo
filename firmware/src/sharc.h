#ifndef MIDI_CONTROL_SHARC_H
#define MIDI_CONTROL_SHARC_H

/* Booting the SHARC (docs/10 section 3).
 *
 * The SHARC holds no program of its own; whatever drives it after reset has
 * sent it one over SSP0. This sends ADI's boot kernel, read off the eMMC where
 * the factory image keeps it, and then firmware/sharc/'s pass-through program
 * (src/sharc_program.h), and watches FLAG0 to see the program run.
 *
 * Reads the eMMC; writes nothing to it. Resets the SHARC, which a power cycle
 * restores. */

#include <stdint.h>

enum {
    SHARC_OK = 0,
    SHARC_FAIL_EMMC,        /* the card is not up, or the kernel's sectors did not read */
    SHARC_FAIL_KERNEL,      /* the kernel's CRC32 is not the one docs/10 records */
    SHARC_FAIL_SSP,         /* an SSP0 wait ran out; the stream is incomplete */
    SHARC_FAIL_FLAG0,       /* sent whole, but FLAG0 did not toggle */
};

struct sharc_status {
    int rc;                 /* SHARC_OK or a SHARC_FAIL_ code */
    uint32_t kernel_crc;    /* CRC32 of the kernel as read, 0 if it was not */
    uint32_t words;         /* stream words sent, kernel included */
    uint32_t edges;         /* FLAG0 transitions seen on GPIO1[8] */
    uint32_t first_edge_us; /* when the first came, from the end of the stream */
};

/* Reset the SHARC and boot the pass-through. The eMMC must be initialised.
 * Takes about 150 ms. */
int sharc_boot(struct sharc_status *st);

#endif
