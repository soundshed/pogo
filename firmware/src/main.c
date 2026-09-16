/* The POD Go as a configurable USB MIDI controller.
 *
 * This file is the board side of the device. Everything that decides what a
 * control *does* - the mapping, the protocol, the persistence policy, the
 * screen layout - is in shared/ and runs unchanged in the emulator. What is
 * left here is what only the hardware can do:
 *
 *   - bring the board up: panel, LCD, USB clocks, SDRAM, eMMC, USB
 *   - scan the switch matrix and the encoders, debounce, read the ADCs
 *   - move MIDI bytes between TinyUSB and midi_control_device
 *   - drive the LEDs from the engine's colours, inside the scan
 *   - present the shared UI on the real framebuffer, without tearing
 *   - keep time, and switch the panel's power, for the display timeout
 *   - answer the diagnostic commands on the vendor endpoint
 *
 * Boot order matters and is explained inline; the short version is that each
 * stage writes a marker on the screen before it starts, so a stall leaves the
 * name of the stage that stalled (docs/02-firmware-platform.md section 5).
 */

#include <stdint.h>
#include "board.h"
#include "emc.h"
#include "emmc.h"
#include "lcd.h"
#include "log.h"
#include "result.h"
#include "storage.h"
#include "tusb.h"

#include "midi_control/midi_control_bootimage.h"
#include "midi_control/midi_control_device.h"
#include "midi_control/midi_control_gfx.h"
#include "midi_control/midi_control_ui.h"
#include "midi_control/midi_control_version.h"

#define SEC          20000000u      /* board_delay_crude iterations, ~1 s */

/* ---- vendor endpoint commands ------------------------------------------ */
/* Two bytes, {command, value}, on the vendor bulk OUT endpoint. The same
 * numbers as the reference firmware, so tools/midi_control_usb.py works unchanged. */
#define CC_REBOOT_DFU       68   /* value 127: hand the unit to the boot ROM's DFU loader */
#define CC_STORAGE_STATUS   70   /* a result packet describing the eMMC state */
#define CC_LCD_STATUS       74   /* LCD controller registers, to the log */
#define CC_EMMC_RESTORE     76   /* value 127 arms an image write; payload follows */
#define CC_EMMC_DUMP        79   /* stream sectors 0..1364 out */
#define CC_EMMC_PEEK        89   /* one sector's first words, value = LBA (0..127) */
#define CC_RESULT           93   /* re-send the last result packet */
#define CC_ADC_SNAPSHOT     95   /* every ADC channel and each analog control's range */
#define CC_LOG_REPLAY      126   /* replay the log from the start */

#define CC_EMMC_RESTORE_VAL 127
#define CC_REBOOT_DFU_VAL   127

/* ---- the device ---------------------------------------------------------- */

static struct midi_control_device g_dev;

void USB0_IRQHandler(void)
{
    tud_int_handler(0);
}

/* Push a whole message into the MIDI TX FIFO, pumping USB while it fills.
 * Bounded: if the host stops taking data the message is dropped rather than
 * the main loop hung. */
static void midi_write_all(const uint8_t *bytes, uint32_t len)
{
    uint32_t sent = 0;
    for (uint32_t spins = 0; sent < len && spins < 200000u; spins++) {
        uint32_t n = tud_midi_stream_write(0, bytes + sent, len - sent);
        sent += n;
        if (n == 0)
            tud_task();
    }
    tud_midi_stream_write(0, bytes, 0);      /* no-op, keeps the call shape uniform */
}

static void platform_midi_out(void *ctx, const uint8_t *bytes, uint32_t len)
{
    (void)ctx;
    if (!tud_midi_mounted())
        return;
    midi_write_all(bytes, len);
}

/* ---- panel scanning ---------------------------------------------------- */

#define SWITCH_COUNT   (BOARD_COLUMNS * BOARD_ROWS)
#define DEBOUNCE_SCANS 4

static uint32_t g_stable;
static uint8_t g_panel_primed;
static uint8_t g_count[SWITCH_COUNT];
static uint8_t g_colours[BOARD_COLUMNS];

/* Two quadrature tables, indexed by (previous code << 2) | current code: the
 * full table for the five parameter encoders (four counts per detent) and a
 * one-count-per-detent table for Home and Menu, which makes the feel consistent
 * across the unit. */
static const int8_t k_quad_full[16] = {
    0, -1,  1,  0,
    1,  0,  0, -1,
   -1,  0,  0,  1,
    0,  1, -1,  0,
};

static const int8_t k_quad_detent[16] = {
    0,  0,  1,  0,
    0,  0,  0,  0,
   -1,  0,  0,  0,
    0,  0,  0,  0,
};

#define ENC_DETENT_FIRST   5
#define ENC_DIVISOR_FULL   4
#define ENC_DIVISOR_DETENT 1

static uint8_t g_enc_prev[BOARD_COLUMNS];
static int8_t  g_enc_accum[BOARD_COLUMNS];
static uint8_t g_enc_primed;

static uint8_t g_direct_stable[BOARD_DIRECT_COUNT];
static uint8_t g_direct_count[BOARD_DIRECT_COUNT];
static uint8_t g_direct_primed;

static const uint8_t k_direct_button[BOARD_DIRECT_COUNT] = {
    MIDI_CONTROL_BTN_TOE,      /* BOARD_DIRECT_TOE  */
    MIDI_CONTROL_BTN_PAGE,     /* BOARD_DIRECT_PAGE */
};

static void encoder_task(const struct panel_state *st)
{
    if (!g_enc_primed) {
        for (unsigned i = 0; i < BOARD_COLUMNS; i++)
            g_enc_prev[i] = st->enc[i];
        g_enc_primed = 1;
        return;
    }

    for (unsigned i = 0; i < BOARD_COLUMNS; i++) {
        uint8_t code = st->enc[i];
        if (code == g_enc_prev[i])
            continue;

        const int8_t *table = (i >= ENC_DETENT_FIRST) ? k_quad_detent : k_quad_full;
        int8_t step = table[(g_enc_prev[i] << 2) | code];
        g_enc_prev[i] = code;
        if (step == 0)
            continue;

        g_enc_accum[i] = (int8_t)(g_enc_accum[i] + step);

        int divisor = (i >= ENC_DETENT_FIRST) ? ENC_DIVISOR_DETENT : ENC_DIVISOR_FULL;
        while (g_enc_accum[i] >= divisor) {
            g_enc_accum[i] = (int8_t)(g_enc_accum[i] - divisor);
            midi_control_device_encoder(&g_dev, (enum midi_control_encoder)i, 1);
        }
        while (g_enc_accum[i] <= -divisor) {
            g_enc_accum[i] = (int8_t)(g_enc_accum[i] + divisor);
            midi_control_device_encoder(&g_dev, (enum midi_control_encoder)i, -1);
        }
    }
}

static void panel_task(void)
{
    struct panel_state st;

    /* The LEDs are the engine's, driven only inside the column strobe. */
    const uint8_t *led = midi_control_device_leds(&g_dev);
    for (unsigned col = 0; col < BOARD_COLUMNS; col++)
        g_colours[col] = led[col];

    board_panel_scan(&st, g_colours);

    /* Establish the resting state on the first pass rather than assuming every
     * switch is open: one misread on the first scan otherwise sends a phantom
     * event before the host is even listening. */
    if (!g_panel_primed) {
        g_stable = st.switches;
        g_panel_primed = 1;
        encoder_task(&st);
        return;
    }

    encoder_task(&st);

    uint32_t raw = st.switches;

    for (unsigned i = 0; i < SWITCH_COUNT; i++) {
        uint32_t bit = 1u << i;
        int now = (raw & bit) != 0;
        int was = (g_stable & bit) != 0;

        if (now == was) {
            g_count[i] = 0;
            continue;
        }
        if (++g_count[i] < DEBOUNCE_SCANS)
            continue;

        g_count[i] = 0;
        g_stable ^= bit;

        int button = midi_control_button_at(i / BOARD_ROWS, i % BOARD_ROWS);
        if (button >= 0)
            midi_control_device_button(&g_dev, (enum midi_control_button)button, now);
    }
}

static void direct_task(void)
{
    if (!g_direct_primed) {
        for (unsigned i = 0; i < BOARD_DIRECT_COUNT; i++)
            g_direct_stable[i] = board_direct_read(i) ? 1u : 0u;
        g_direct_primed = 1;
        return;
    }

    for (unsigned i = 0; i < BOARD_DIRECT_COUNT; i++) {
        uint8_t now = board_direct_read(i) ? 1u : 0u;

        if (now == g_direct_stable[i]) {
            g_direct_count[i] = 0;
            continue;
        }
        if (++g_direct_count[i] < DEBOUNCE_SCANS)
            continue;

        g_direct_count[i] = 0;
        g_direct_stable[i] = now;
        midi_control_device_button(&g_dev, (enum midi_control_button)k_direct_button[i], now);
    }
}

/* ---- analog controls --------------------------------------------------- */
/* The raw-to-0..127 scaling is a hardware matter and lives here; what the
 * value then does is the engine's.
 *
 * min/max are seeds that only ever widen: a calibration baked in from one
 * unit is wrong for another, and wrong for this one if it was never pushed to
 * both stops. The seeds sit inside the real travel (the pedal's measured
 * ~280..664, the knob's 0..472 with the EMC up) so the ends are reached. The
 * pedal's sensor voltage falls toe-down, so it is inverted here to make
 * toe-down high; the configuration's own `invert` sits on top of that. */

#define ANALOG_HYSTERESIS 8
#define ANALOG_OVERSAMPLE 8
#define ANALOG_END_ZONE   4u

struct analog_ctrl {
    uint8_t  adc, channel, invert;
    uint16_t min, max;
    uint16_t raw_sent;
    int16_t  sent;
};

static struct analog_ctrl g_analog[MIDI_CONTROL_ANALOG_COUNT] = {
    /* adc ch inv   min    max */
    {   0,  0,  0,   20u,  400u, 0, -1 },    /* MIDI_CONTROL_ANALOG_VOLUME */
    {   0,  7,  1,  280u,  664u, 0, -1 },    /* MIDI_CONTROL_ANALOG_PEDAL  */
};

static void analog_task(void)
{
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++) {
        struct analog_ctrl *a = &g_analog[i];

        uint32_t sum = 0;
        for (unsigned n = 0; n < ANALOG_OVERSAMPLE; n++)
            sum += board_adc_read(a->adc, a->channel);
        uint16_t raw = (uint16_t)(sum / ANALOG_OVERSAMPLE);

        if (raw < a->min)
            a->min = raw;
        if (raw > a->max)
            a->max = raw;

        uint16_t span = (uint16_t)(a->max - a->min);
        if (span < 32)
            continue;

        uint32_t scaled;
        if (raw >= a->max - ANALOG_END_ZONE)
            scaled = 127u;
        else if (raw <= a->min + ANALOG_END_ZONE)
            scaled = 0u;
        else
            scaled = ((uint32_t)(raw - a->min) * 127u + span / 2u) / span;
        if (scaled > 127u)
            scaled = 127u;
        if (a->invert)
            scaled = 127u - scaled;

        if ((int16_t)scaled == a->sent)
            continue;

        /* Hysteresis holds back small moves, but never the move onto a stop. */
        int diff = (int)raw - (int)a->raw_sent;
        if (diff < 0)
            diff = -diff;
        if (diff < ANALOG_HYSTERESIS && scaled != 0u && scaled != 127u)
            continue;

        a->sent = (int16_t)scaled;
        a->raw_sent = raw;
        midi_control_device_analog(&g_dev, (enum midi_control_analog)i, (uint8_t)scaled);
    }
}

/* ---- the display --------------------------------------------------------- */

/* 1 bpp, 16,320 bytes, in .bss - which midi_control.ld puts in AHB SRAM. The LCD
 * controller is an AHB master and cannot reach the local SRAM the code runs
 * from. Eight-byte aligned because UPBASE ignores the low three bits. */
static uint8_t g_framebuffer[MIDI_CONTROL_LCD_MONO_BYTES] __attribute__((aligned(8)));

/* The UI draws into g_gfx. In colour that is a back buffer in SDRAM, and each
 * update copies only the rectangle it redrew onto g_panel, the buffer the LCD
 * scans - so the panel never shows a region cleared and not yet redrawn. In
 * mono there is no SDRAM for a second buffer and the UI draws straight into
 * the scanned one. */
#define UI_BACK_BUFFER (EMC_SDRAM_BASE + 0x00100000u)   /* clear of the panel's buffer and EMMC_RX_BUFFER */

static struct midi_control_gfx g_gfx;
static struct midi_control_gfx g_panel;
static int g_colour;
static struct midi_control_ui_cache g_ui;
static uint32_t g_fb_addr;
static const char *g_stage = "BOOT";
static int g_emc_state = -1;
static struct emc_result g_emc;
static struct emc_snapshot g_emc_entry;
static char g_diag[40];
static int g_booting = 1;

static void set_diag_sdram(void)
{
    static const char *k_emc[] = { "SDRAM OK", "SDRAM ADDR FAIL", "SDRAM DATA FAIL",
                                   "SDRAM REFRESH FAIL" };
    const char *s = k_emc[g_emc_state & 3];
    unsigned n = 0;
    while (s[n] && n < sizeof(g_diag) - 12u) {
        g_diag[n] = s[n];
        n++;
    }
    g_diag[n++] = ' ';
    midi_control_fmt_uint(&g_diag[n], g_emc.size >> 20);
    while (g_diag[n])
        n++;
    g_diag[n++] = 'M';
    g_diag[n] = 0;
}

/* Redraws whatever on the screen has changed - nothing, most calls - and
 * shows it. The start-up screen until the loop runs, then the device's. */
static void display_draw(void)
{
    struct midi_control_ui_model m;
    struct midi_control_rect r;

    m.device = g_booting ? 0 : &g_dev;
    m.stage = g_stage;
    m.note = lcd_was_cold_started() ? "COLD" : "INHERITED";
    m.diag = (g_emc_state >= 0 && (g_booting || g_emc_state != EMC_OK)) ? g_diag : "";
    m.diag_colour = g_emc_state == EMC_OK ? MIDI_CONTROL_GREEN : MIDI_CONTROL_RED;
    m.usb_mounted = tud_midi_mounted() ? 1u : 0u;
    m.colour_bars = g_booting ? 1u : 0u;

    /* The display timeout. Going dark, the backlight and the panel's power go
     * first and the black screen is drawn behind them; lit again, the picture
     * is drawn first, so it is whole when they return. docs/01 section 5. */
    static int powered = 1;
    const int lit = g_booting || midi_control_device_display_on(&g_dev);
    if (!lit && powered) {
        log_line("display: dark, backlight PD_16 low, clearing LCD CTRL LCDPWR");
        board_backlight(0);
        lcd_set_power(0);
        powered = 0;
    }

    if (midi_control_ui_update(&g_gfx, &m, &g_ui, &r) && g_colour)
        midi_control_gfx_copy(&g_panel, &g_gfx, r.x, r.y, r.w, r.h);

    if (lit && !powered) {
        log_line("display: lit, setting LCD CTRL LCDPWR, backlight PD_16 high");
        lcd_set_power(1);
        board_backlight(1);
        powered = 1;
    }
}

/* ---- diagnostics on the vendor endpoint ---------------------------------- */

static void log_snapshot(const char *tag, const struct emc_snapshot *snap)
{
    log_line(tag);
    log_hex("  CTL", snap->control);
    log_hex("  STA", snap->status);
    log_hex("  CFG", snap->config0);
    log_hex("  RAS", snap->rascas0);
    log_hex("  DYN", snap->dyncontrol);
    log_hex("  REF", snap->refresh);
}

static void log_emc_result(int state, const struct emc_result *r)
{
    static const char *k_name[] = { "OK", "ADDR FAIL", "DATA FAIL", "REFRESH FAIL" };
    log_line(k_name[state & 3]);
    log_dec("  size MB", r->size >> 20);
    log_hex("  lanes bad", (uint32_t)r->lanes);
    log_hex("  at", r->addr);
    log_hex("  want", r->want);
    log_hex("  got", r->got);
}

static void lcd_status_task(void)
{
    const uint32_t branch = *(volatile uint32_t *)0x4005141Cu;   /* CLK_M4_LCD_STAT */

    log_line("--- lcd now ---");
    log_hex("  BASE_LCD_CLK", *(volatile uint32_t *)0x40050088u);
    log_hex("  M4_LCD stat ", branch);
    if (!(branch & 1u)) {
        log_line("  branch clock off - LCD registers not read");
        return;
    }
    log_hex("  CTRL        ", lcd_reg(0x18));
    log_hex("  UPBASE      ", lcd_reg(0x10));
    log_hex("  INTRAW      ", lcd_reg(0x20));
    log_hex("  UPCURR      ", lcd_reg(0x2C));
    board_delay_crude(60000u);
    log_hex("  UPCURR      ", lcd_reg(0x2C));
    log_hex("  fb scanned  ", g_fb_addr);
}

static void adc_snapshot(void)
{
    char name[5] = { 'a', '0', '0', 0, 0 };

    result_begin("adc");
    for (unsigned adc = 0; adc < 2; adc++) {
        for (unsigned ch = 0; ch < 8; ch++) {
            name[1] = (char)('0' + adc);
            name[2] = (char)('0' + ch);
            result_put(name, board_adc_read(adc, ch));
        }
    }
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; i++) {
        const struct analog_ctrl *a = &g_analog[i];
        name[0] = 'c';
        name[1] = (char)('0' + i);
        name[2] = 'l'; name[3] = 'o';
        result_put(name, a->min);
        name[2] = 'h'; name[3] = 'i';
        result_put(name, a->max);
        name[2] = 't'; name[3] = 'x';
        result_put(name, (uint32_t)(int32_t)a->sent);
    }
    result_emit();
}

static void storage_status_task(void)
{
    result_begin("storage");
    result_put("card", (uint32_t)storage_card_ready());
    result_put("state", (uint32_t)storage_state());
    result_put("loaded", (uint32_t)g_dev.loaded_from_storage);
    result_put("seq", g_dev.config.sequence);
    result_put("slot", g_dev.newest_slot);
    result_put("dirty", g_dev.dirty);
    result_put("lastsave", g_dev.last_save_status);
    result_put("program", g_dev.engine.program);
    result_emit();
}

/* One sector's first words as a result packet. */
static void emmc_peek_task(uint8_t lba)
{
    static uint8_t sector[EMMC_SECTOR_BYTES] __attribute__((aligned(4)));

    result_begin("emmc-peek");
    result_put("lba", lba);
    if (!storage_card_ready()) {
        result_note("emmc not ready");
        result_emit();
        return;
    }
    int rc = emmc_read(lba, 1, sector);
    result_put("rc", (uint32_t)rc);
    if (rc == EMMC_OK) {
        const uint32_t *w = (const uint32_t *)(const void *)sector;
        char name[5] = { 'w', '0', 0, 0, 0 };
        for (unsigned i = 0; i < 8; i++) {
            name[1] = (char)('0' + i);
            result_put(name, w[i]);
        }
        result_note(w[0] == MIDI_CONTROL_BOOT_MAGIC ? "MAGIC MATCH" : "no magic at this lba");
    }
    result_emit();
}

/* The whole backed-up region, sectors 0..1364, streamed out. Framing:
 * "SSEMMCDP", u32 LE length, u32 reserved, then the bytes. No device-side
 * checksum, deliberately: the host proves a backup by taking it twice and
 * comparing, which tests the thing that matters. */
static void emmc_dump_task(void)
{
    static uint8_t blk[EMMC_SECTOR_BYTES] __attribute__((aligned(4)));

    if (!storage_card_ready()) {
        log_line("emmc not ready - not dumping");
        log_rewind();
        return;
    }

    uint32_t total = MIDI_CONTROL_EMMC_SAFE_SECTORS * EMMC_SECTOR_BYTES;
    uint8_t hdr[16];
    hdr[0] = 'S'; hdr[1] = 'S'; hdr[2] = 'E'; hdr[3] = 'M';
    hdr[4] = 'M'; hdr[5] = 'C'; hdr[6] = 'D'; hdr[7] = 'P';
    hdr[8]  = (uint8_t)total;
    hdr[9]  = (uint8_t)(total >> 8);
    hdr[10] = (uint8_t)(total >> 16);
    hdr[11] = (uint8_t)(total >> 24);
    hdr[12] = hdr[13] = hdr[14] = hdr[15] = 0;
    log_send_raw(hdr, sizeof(hdr));

    for (uint32_t s = 0; s < MIDI_CONTROL_EMMC_SAFE_SECTORS; s++) {
        if (emmc_read(s, 1, blk) != EMMC_OK) {
            /* Stop rather than send stale bytes: a short dump is visible, a
             * silently wrong sector is not. */
            log_dec("emmc dump: read failed at sector", s);
            break;
        }
        log_send_raw(blk, EMMC_SECTOR_BYTES);
    }
    for (uint32_t i = 0; i < 4000u; i++)
        tud_task();
}

/* ---- the eMMC write path -------------------------------------------------- */
/* Armed by {76, 127}; the bytes that follow are a 20-byte header - "SSWRIMG!",
 * u32 LE LBA, u32 LE length, u32 LE CRC32 (advisory) - then the payload,
 * buffered in SDRAM. Nothing is decided on the CRC: every sector is read back
 * after writing and compared with what the host sent. The target is clamped
 * to the backed-up region. This is the path that installs the firmware and
 * restores the factory image, one code path for both directions. */

#define EMMC_RX_MAGIC_0  0x52575353u          /* "SSWR" */
#define EMMC_RX_MAGIC_1  0x21474D49u          /* "IMG!" */
#define EMMC_RX_HEADER   20u
#define EMMC_RX_MAX      (1024u * 1024u)
#define EMMC_RX_BUFFER   (EMC_SDRAM_BASE + 0x00200000u)   /* clear of the framebuffer */

static int g_rx_active;
static uint32_t g_rx_got;
static uint32_t g_rx_lba;
static uint32_t g_rx_len;
static uint32_t g_rx_crc;
static uint8_t g_rx_hdr[EMMC_RX_HEADER];

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void emmc_rx_commit(void)
{
    static uint8_t back[EMMC_SECTOR_BYTES] __attribute__((aligned(4)));
    const uint8_t *img = (const uint8_t *)EMMC_RX_BUFFER;
    uint32_t sectors = (g_rx_len + EMMC_SECTOR_BYTES - 1u) / EMMC_SECTOR_BYTES;

    result_begin("emmc-write");
    result_put("lba", g_rx_lba);
    result_put("len", g_rx_len);
    result_put("sectors", sectors);
    result_put("host_crc", g_rx_crc);
    result_put("got_bytes", g_rx_got);

    if (g_rx_lba + sectors > MIDI_CONTROL_EMMC_SAFE_SECTORS) {
        result_note("REFUSED - outside the sectors we hold a backup of");
        result_emit();
        return;
    }
    if (!storage_card_ready()) {
        result_note("emmc not ready - nothing written");
        result_emit();
        return;
    }

    int rc = emmc_write(g_rx_lba, sectors, img);
    result_put("write_rc", (uint32_t)rc);
    if (rc != EMMC_OK) {
        result_note("WRITE FAILED - read the log, then restore from the backup");
        result_emit();
        return;
    }

    uint32_t bad = 0;
    for (uint32_t s = 0; s < sectors; s++) {
        if (emmc_read(g_rx_lba + s, 1, back) != EMMC_OK) {
            bad++;
            continue;
        }
        const uint8_t *want = img + s * EMMC_SECTOR_BYTES;
        uint32_t n = g_rx_len - s * EMMC_SECTOR_BYTES;
        if (n > EMMC_SECTOR_BYTES)
            n = EMMC_SECTOR_BYTES;
        for (uint32_t i = 0; i < n; i++) {
            if (back[i] != want[i]) {
                bad++;
                break;
            }
        }
    }

    result_put("badsectors", bad);
    result_note(bad == 0 ? "WRITTEN AND VERIFIED"
                         : "VERIFY FAILED - restore from the backup");
    result_emit();

    /* What is at LBA 1 may just have changed; report it as it now is. */
    storage_reclassify();
}

static uint32_t emmc_rx_feed(const uint8_t *b, uint32_t n)
{
    uint32_t used = 0;

    while (used < n && g_rx_got < EMMC_RX_HEADER) {
        g_rx_hdr[g_rx_got++] = b[used++];
        if (g_rx_got == EMMC_RX_HEADER) {
            if (rd32(g_rx_hdr) != EMMC_RX_MAGIC_0 || rd32(g_rx_hdr + 4) != EMMC_RX_MAGIC_1) {
                log_line("emmc rx: bad magic - transfer abandoned");
                g_rx_active = 0;
                return used;
            }
            g_rx_lba = rd32(g_rx_hdr + 8);
            g_rx_len = rd32(g_rx_hdr + 12);
            g_rx_crc = rd32(g_rx_hdr + 16);
            if (g_rx_len == 0u || g_rx_len > EMMC_RX_MAX) {
                log_dec("emmc rx: implausible length", g_rx_len);
                g_rx_active = 0;
                return used;
            }
        }
    }

    while (used < n && g_rx_got < EMMC_RX_HEADER + g_rx_len) {
        uint32_t off = g_rx_got - EMMC_RX_HEADER;
        uint32_t room = g_rx_len - off;
        uint32_t take = n - used;
        if (take > room)
            take = room;
        uint8_t *dst = (uint8_t *)EMMC_RX_BUFFER + off;
        for (uint32_t i = 0; i < take; i++)
            dst[i] = b[used + i];
        used += take;
        g_rx_got += take;
    }

    if (g_rx_got >= EMMC_RX_HEADER + g_rx_len) {
        g_rx_active = 0;
        emmc_rx_commit();
    }
    return used;
}

/* Into the boot ROM's DFU loader, from the host, with no button and no power
 * cycle (board_enter_dfu). The log goes out first and the answer last, so the
 * answer is the last packet the host reads; nothing is sent after it. The
 * screen names the boot ROM's DFU mode for as long as it holds the picture. */
static void reboot_dfu_task(void)
{
    log_line("dfu: USBCMD RS off, BASE_M4_CLK to IRC, PLL1 down, FPB patch, ROM 0x104014B9");
    log_flush();

    g_stage = "FIRMWARE UPDATE MODE";
    g_booting = 1;
    display_draw();
    board_leds_set(0);

    result_begin("dfu");
    result_note("REBOOTING INTO DFU");
    result_emit();

    board_enter_dfu();
}

static void handle_cc(uint8_t cc, uint8_t value)
{
    switch (cc) {
    case CC_REBOOT_DFU:
        if (value >= CC_REBOOT_DFU_VAL)
            reboot_dfu_task();
        break;
    case CC_LOG_REPLAY:
        log_rewind();
        break;
    case CC_RESULT:
        result_emit();
        break;
    case CC_ADC_SNAPSHOT:
        adc_snapshot();
        break;
    case CC_LCD_STATUS:
        lcd_status_task();
        break;
    case CC_STORAGE_STATUS:
        storage_status_task();
        break;
    case CC_EMMC_PEEK:
        emmc_peek_task(value);
        break;
    case CC_EMMC_DUMP:
        emmc_dump_task();
        break;
    case CC_EMMC_RESTORE:
        if (value >= CC_EMMC_RESTORE_VAL) {
            if (g_emc_state != EMC_OK) {
                log_line("emmc rx: no SDRAM to buffer an image - refused");
                break;
            }
            g_rx_active = 1;
            g_rx_got = 0;
            log_line("emmc rx: armed, expecting a header");
        }
        break;
    default:
        break;
    }
}

/* The vendor OUT stream: {command, value} pairs, or - while a write is armed -
 * the image payload. Odd trailing bytes are held over so a split transfer
 * cannot lose a command. */
static void vendor_rx_task(void)
{
    static uint8_t pending;
    static int have_pending;
    uint8_t buf[64];

    while (tud_vendor_available()) {
        uint32_t n = tud_vendor_read(buf, sizeof(buf));
        if (n == 0)
            break;

        uint32_t i = 0;
        if (g_rx_active) {
            i += emmc_rx_feed(buf, n);
            have_pending = 0;
        }
        for (; i < n; i++) {
            if (!have_pending) {
                pending = buf[i];
                have_pending = 1;
            } else {
                have_pending = 0;
                handle_cc(pending, buf[i]);
            }
        }
    }
}

/* Everything the host sends on the MIDI OUT endpoint goes to the device
 * layer, which sorts channel messages from our SysEx. The stream has to be
 * drained regardless: an unread RX FIFO eventually stops the endpoint. */
static void midi_rx_task(void)
{
    uint8_t buf[64];

    while (tud_midi_available()) {
        uint32_t n = tud_midi_stream_read(buf, sizeof(buf));
        if (n == 0)
            break;
        midi_control_device_midi_in(&g_dev, buf, n);
    }
}

/* ---- main ------------------------------------------------------------------ */

int main(void)
{
    board_panel_init();
    board_leds_set(0);

    /* First, before anything writes a register: what the boot ROM or the NOR
     * bootloader left behind. */
    emc_snapshot(&g_emc_entry);

    /* The panel next, before anything else that can stall, on the mono buffer
     * that needs no SDRAM. It narrates the bring-up; if a later stage stalls,
     * the last marker drawn names it. The device is not initialised until
     * CONFIG; until the loop runs, the screen is the start-up screen. */
    g_fb_addr = (uint32_t)(uintptr_t)g_framebuffer;
    midi_control_gfx_init(&g_gfx, g_framebuffer, MIDI_CONTROL_GFX_MONO, 1);
    lcd_init(g_framebuffer, MIDI_CONTROL_GFX_MONO);
    display_draw();

    /* The boot ROM leaves interrupts masked; without this the USB interrupt
     * never fires and EP0 goes unanswered. After a NOR boot this is safe only
     * because reset_continue() has stopped SysTick and cleared the pends the
     * bootloader leaves behind. */
    __asm__ volatile("cpsie i" ::: "memory");

    g_stage = "CLOCKS";
    display_draw();
    board_usb_clock_init();
    board_adc_init();

    /* SDRAM: the known-good configuration first, the sweep only if it fails.
     * Index 0 is the baseline timing configuration. */
    struct emc_config cfg;
    struct emc_result r;

    log_line("--- " MIDI_CONTROL_PRODUCT_NAME ", emc bring-up ---");
    log_snapshot("entry", &g_emc_entry);

    g_stage = "EMC";
    display_draw();

    /* Retry the whole bring-up, clock included, a few times before the sweep.
     * The read-config sweep below only varies the capture strategy; it cannot
     * re-lock PLL1, and a marginal PLL1 lock is what took SDRAM and the eMMC
     * down together on the bootloader boot path (docs/09). emc_init() now
     * adopts an already-good PLL1 and verifies a cold lock, so a retry here
     * gives that hardening a second and third chance rather than dropping
     * straight to a sweep that runs on the same bad clock. */
    emc_config_at(0, &cfg);
    g_emc_verbose = 0;
    for (unsigned attempt = 0; attempt < 3u; attempt++) {
        if (attempt) {
            g_stage = "EMC RETRY";
            display_draw();
            log_dec("emc: retrying bring-up, attempt", attempt);
        }
        emc_init(&cfg);
        g_emc_state = emc_test(&g_emc);
        if (g_emc_state == EMC_OK)
            break;
    }
    log_emc_result(g_emc_state, &g_emc);
    emc_log_clocks();
    set_diag_sdram();
    display_draw();

    for (unsigned i = 1; g_emc_state != EMC_OK && i < EMC_SWEEP_COUNT; i++) {
        g_stage = "EMC SWEEP";
        display_draw();
        emc_config_at(i, &cfg);
        emc_init(&cfg);
        if (emc_quick_test(&r) == EMC_OK) {
            g_emc_state = emc_test(&g_emc);
            log_dec("sweep found a working config at index", i);
            log_emc_result(g_emc_state, &g_emc);
        }
        set_diag_sdram();
        display_draw();
    }

    /* Colour, if the memory is there to hold it. The order of the base move
     * and the format change inside lcd_init() is what keeps this from
     * latching the panel white. */
    if (g_emc_state == EMC_OK) {
        g_stage = "COLOUR";
        display_draw();
        g_fb_addr = (uint32_t)EMC_SDRAM_BASE;
        midi_control_gfx_init(&g_panel, (void *)EMC_SDRAM_BASE, MIDI_CONTROL_GFX_RGB565, 1);
        midi_control_gfx_init(&g_gfx, (void *)UI_BACK_BUFFER, MIDI_CONTROL_GFX_RGB565, 1);
        g_colour = 1;
        g_ui.valid = 0;                 /* a new buffer: the next update draws all of it */
        lcd_init((void *)EMC_SDRAM_BASE, MIDI_CONTROL_GFX_RGB565);
        display_draw();
    }

    /* The eMMC, for the configuration. Every wait inside is bounded, so a
     * missing or unhappy card ends in a status rather than a hang, and the
     * controller then simply runs on defaults with storage reported as
     * absent. */
    g_stage = "EMMC";
    display_draw();
    storage_init();

    g_stage = "CONFIG";
    display_draw();
    {
        struct midi_control_platform_ops ops;
        ops.ctx = 0;
        ops.midi_out = platform_midi_out;
        ops.storage_state = storage_ops_state;
        ops.storage_read = storage_ops_read;
        ops.storage_write = storage_ops_write;
        ops.platform = MIDI_CONTROL_PLATFORM_HARDWARE;
        ops.fw_major = MIDI_CONTROL_VERSION_MAJOR;
        ops.fw_minor = MIDI_CONTROL_VERSION_MINOR;
        ops.fw_patch = MIDI_CONTROL_VERSION_PATCH;
        midi_control_device_init(&g_dev, &ops);
    }
    log_dec("config loaded from storage", g_dev.loaded_from_storage);
    log_dec("config sequence", g_dev.config.sequence);
    display_draw();

    /* The ROM had USB0 attached as the DFU device. Hold the pull-up off long
     * enough for the host to tear that down before TinyUSB re-attaches. */
    board_delay_crude(SEC / 2);

    g_stage = "USB";
    display_draw();

    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(0, &dev_init);
    board_leds_set(0);

    /* The display timeout's clock, from here on, when the core clock is final. */
    log_line("clock: SysTick, the rate from CGU BASE_M4_CLK and PLL1_CTRL");
    log_dec("  core Hz (0: unknown, the display never goes dark)", board_ms_start());

    g_stage = "RUN";
    g_booting = 0;

    uint32_t ticks = 0;
    for (;;) {
        tud_task();

        /* Scan far less often than tud_task() runs: each scan strobes seven
         * columns with a settle delay apiece, ~4 ms, which is ample for
         * footswitches and keeps the LED multiplex steady. */
        if (++ticks >= 2000u) {
            ticks = 0;
            log_task();
            midi_rx_task();
            vendor_rx_task();
            panel_task();
            direct_task();
            analog_task();
            midi_control_device_tick(&g_dev, board_ms_elapsed());
            display_draw();
        }
    }
}
