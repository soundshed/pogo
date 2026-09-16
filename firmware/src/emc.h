#ifndef MIDI_CONTROL_EMC_H
#define MIDI_CONTROL_EMC_H

#include <stdint.h>

/* Dynamic memory chip select 0. */
#define EMC_SDRAM_BASE  0x28000000u

/* 32 MiB. UM10503 Table 430 decodes DYNAMICCONFIG0 = 0x4480 as a 32-bit bus,
 * 4 banks, row length 12, column length 9 - so 4096 x 512 x 4 banks x 4 bytes.
 * Stated from the manual rather than probed for: a test that measures what the
 * documentation already fixes is a test that can disagree with it. */
#define EMC_SDRAM_SIZE  (32u * 1024u * 1024u)

/* The settings that depend on the clock the EMC is actually running at, rather
 * than on the memory part itself - so they cannot be carried over unchanged from
 * a system running at a different frequency. Everything else - the address
 * mapping, the timings, the mode register - is a property of the fitted memory
 * and lives in emc.c. */
struct emc_config {
    uint8_t read_config;     /* DYNAMICREADCONFIG, 0..3: the capture strategy */
    uint16_t delay_clk;      /* SCU EMCDELAYCLK, the capture delay trim */
};

/* Bring up the external memory controller and the SDRAM behind it using the
 * verified board sequence (docs/02 section 2).
 *
 * Safe to call repeatedly - the sweep does - and safe to call with the LCD
 * running, provided the LCD is not scanning out of SDRAM: it makes SDRAM
 * contents undefined. */
void emc_init(const struct emc_config *cfg);

enum {
    EMC_OK = 0,
    EMC_FAIL_ADDR,        /* address lines alias, or the wrap is implausible */
    EMC_FAIL_DATA,        /* wrong data straight back - lines or capture */
    EMC_FAIL_REFRESH,     /* right, then rots - refresh interval too long */
};

struct emc_result {
    uint32_t addr, want, got;
    uint32_t size;        /* measured, not assumed; valid even on failure */
    unsigned lanes;       /* bit n set = byte lane n failed a round trip */
};

/* Full check: byte lanes, a size measurement, an address walk, a 1 MiB data
 * pattern, and the same pattern again after a pause to catch refresh that is
 * too slow. Takes roughly half a second, almost all of it the pause. */
int emc_test(struct emc_result *r);

/* The same without the pause and over 16 KiB. Milliseconds. */
int emc_quick_test(struct emc_result *r);

/* Four capture trims by four read strategies. */
#define EMC_SWEEP_COUNT 16

/* The EMC's register state, captured before anything modifies it. */
struct emc_snapshot {
    uint32_t control, status, config0, rascas0, dyncontrol, refresh;
};

void emc_snapshot(struct emc_snapshot *s);

/* Log the clock tree feeding the EMC: base clock, PLL1, both CCU branches and
 * the four EMC_CLK pin configurations. */
void emc_log_clocks(void);

/* Set by emc_init(): did the controller actually leave self-refresh when asked?
 * It arrives in self-refresh from reset, and everything else depends on this. */
extern int g_emc_left_selfrefresh;

/* Set by emc_init()'s clock bring-up: 2 = adopted an already-locked PLL1 (the
 * eMMC boot path), 1 = locked it from cold (the DFU path), 0 = it never locked
 * and both SDRAM and the eMMC will fail. See emc.c. */
extern int g_emc_pll1_status;

/* Set to log every offset of the address walk. Off during the sweep. */
extern int g_emc_verbose;

/* The configuration the sweep tries at index i. */
void emc_config_at(unsigned i, struct emc_config *cfg);

#endif
