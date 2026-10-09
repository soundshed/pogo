/* Host tests for shared/. No framework: a check macro, a counter, and an
 * exit code. Run with `ctest` or directly.
 *
 * These exercise the exact code the firmware runs, so a behaviour proven here
 * is the unit's behaviour - the one thing they cannot see is the board layer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "midi_control/midi_control_analog.h"
#include "midi_control/midi_control_bootimage.h"
#include "midi_control/midi_control_config.h"
#include "midi_control/midi_control_crc.h"
#include "midi_control/midi_control_device.h"
#include "midi_control/midi_control_gfx.h"
#include "midi_control/midi_control_protocol.h"
#include "midi_control/midi_control_ui.h"
#include "midi_control/midi_control_version.h"

static int g_fail, g_pass;

#define CHECK(cond) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* ---- a MIDI sink that records everything the device sends ---------------- */

static uint8_t g_out[2u * MIDI_CONTROL_SYSEX_MAX];    /* a CONFIG reply, and room after it */
static uint32_t g_out_len;

static void sink(void *ctx, const uint8_t *bytes, uint32_t len)
{
    (void)ctx;
    if (g_out_len + len <= sizeof(g_out)) {
        memcpy(g_out + g_out_len, bytes, len);
        g_out_len += len;
    }
}

static void sink_reset(void)
{
    g_out_len = 0;
}

static int sink_is(const uint8_t *want, uint32_t len)
{
    return g_out_len == len && memcmp(g_out, want, len) == 0;
}

/* ---- an in-memory storage with two slots ----------------------------------- */
/* Each slot is the sixteen sectors docs/06 reserves, which is what the
 * firmware's storage.c offers and more than the record needs. */

#define SLOT_BYTES (16u * 512u)

static uint8_t g_slot[2][SLOT_BYTES];
static int g_slot_written[2];
static int g_storage_state = MIDI_CONTROL_STORAGE_READY;
static int g_fail_write;

static int st_state(void *ctx) { (void)ctx; return g_storage_state; }

static int st_read(void *ctx, unsigned slot, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (slot > 1u || len > SLOT_BYTES)
        return 0;
    memcpy(buf, g_slot[slot], len);
    return 1;
}

static int st_write(void *ctx, unsigned slot, const uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (slot > 1u || len > SLOT_BYTES)
        return MIDI_CONTROL_STATUS_WRITE_FAILED;
    if (g_fail_write)
        return MIDI_CONTROL_STATUS_WRITE_FAILED;
    memcpy(g_slot[slot], buf, len);
    g_slot_written[slot]++;
    return MIDI_CONTROL_STATUS_OK;
}

static struct midi_control_platform_ops ops_with_storage(void)
{
    struct midi_control_platform_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.midi_out = sink;
    ops.storage_state = st_state;
    ops.storage_read = st_read;
    ops.storage_write = st_write;
    ops.platform = MIDI_CONTROL_PLATFORM_EMULATOR;
    ops.fw_major = 1;
    ops.fw_minor = 2;
    ops.fw_patch = 3;
    return ops;
}

/* ---- tests ------------------------------------------------------------------ */

static void test_crc(void)
{
    CHECK(midi_control_crc32("123456789", 9) == 0xCBF43926u);
    CHECK(midi_control_crc32("", 0) == 0u);
    uint32_t a = midi_control_crc32_update(0, "1234", 4);
    CHECK(midi_control_crc32_update(a, "56789", 5) == 0xCBF43926u);
}

static void test_pack7(void)
{
    uint8_t in[300], packed[400], out[300];
    for (unsigned i = 0; i < sizeof(in); i++)
        in[i] = (uint8_t)(i * 37u + 11u);
    for (uint32_t len = 0; len <= 300u; len += 13u) {
        uint32_t p = midi_control_pack7(in, len, packed);
        CHECK(p == MIDI_CONTROL_PACKED_BYTES(len) || (len % 7u && p == (len / 7u) * 8u + 1u + len % 7u));
        for (uint32_t i = 0; i < p; i++)
            CHECK(!(packed[i] & 0x80u));
        uint32_t u = midi_control_unpack7(packed, p, out, sizeof(out));
        CHECK(u == len);
        CHECK(memcmp(in, out, len) == 0);
    }
    /* Malformed: a high bit in the data, and a trailing lone header. */
    uint8_t bad[9] = { 0, 1, 2, 3, 4, 5, 6, 7, 0 };
    CHECK(midi_control_unpack7(bad, 9, out, sizeof(out)) == 0);
    bad[8] = 0x80;
    CHECK(midi_control_unpack7(bad, 8, out, 3) == 0);   /* overflow */
}

static void test_config_roundtrip(void)
{
    struct midi_control_config a, b;
    uint8_t buf[MIDI_CONTROL_CONFIG_BYTES];

    midi_control_config_defaults(&a);
    a.sequence = 42;
    a.buttons[MIDI_CONTROL_BTN_FS_A].action = MIDI_CONTROL_ACT_CC_TOGGLE;
    a.buttons[MIDI_CONTROL_BTN_FS_A].number = 80;
    a.encoders[3].mode = MIDI_CONTROL_ENCM_CC_ABSOLUTE;
    a.analogs[1].invert = 1;
    strcpy(a.global.name, "TEST RIG");
    a.global.display_timeout = 300;

    midi_control_config_write(&a, buf);
    CHECK(buf[0] == 'P' && buf[1] == 'O' && buf[2] == 'D' && buf[3] == 'G');
    CHECK(buf[16 + 6] == 0x2C && buf[16 + 7] == 0x01);       /* the display timeout, little-endian */
    CHECK(midi_control_config_read(&b, buf, sizeof(buf)) == MIDI_CONTROL_STATUS_OK);
    CHECK(b.sequence == 42);
    CHECK(b.global.display_timeout == 300);
    CHECK(midi_control_config_equal(&a, &b));
    CHECK(b.buttons[MIDI_CONTROL_BTN_FS_A].action == MIDI_CONTROL_ACT_CC_TOGGLE);
    CHECK(b.buttons[MIDI_CONTROL_BTN_FS_A].number == 80);
    CHECK(b.encoders[3].mode == MIDI_CONTROL_ENCM_CC_ABSOLUTE);
    CHECK(b.analogs[1].invert == 1);
    CHECK(strcmp(b.global.name, "TEST RIG") == 0);

    /* The learned calibration rides in the record... */
    a.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_lo = 300;
    a.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_hi = 620;
    a.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].flags = MIDI_CONTROL_CAL_VALID;
    midi_control_config_write(&a, buf);
    CHECK(midi_control_config_read(&b, buf, sizeof(buf)) == MIDI_CONTROL_STATUS_OK);
    CHECK(b.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_lo == 300);
    CHECK(b.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_hi == 620);
    /* ...but two units with the same mapping and different pedals are
     * configured the same. */
    b.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_lo = 310;
    CHECK(midi_control_config_equal(&a, &b));

    /* Every kind of damage is reported as its own status. */
    CHECK(midi_control_config_read(&b, buf, 10) == MIDI_CONTROL_STATUS_BAD_LENGTH);
    buf[100] ^= 1;
    CHECK(midi_control_config_read(&b, buf, sizeof(buf)) == MIDI_CONTROL_STATUS_BAD_CRC);
    buf[100] ^= 1;
    buf[4] = 9;
    CHECK(midi_control_config_read(&b, buf, sizeof(buf)) == MIDI_CONTROL_STATUS_BAD_VERSION);
    buf[4] = 1;
    buf[0] = 'X';
    CHECK(midi_control_config_read(&b, buf, sizeof(buf)) == MIDI_CONTROL_STATUS_BAD_MAGIC);

    /* The display timeout is off by default, and in a record from before it
     * the two bytes were reserved, so zero, which reads as off. */
    midi_control_config_defaults(&a);
    CHECK(a.global.display_timeout == 0);
    midi_control_config_write(&a, buf);
    CHECK(buf[16 + 6] == 0 && buf[16 + 7] == 0);

    /* Sanitise clamps rather than rejects. */
    midi_control_config_defaults(&a);
    a.global.bank_size = 99;
    a.global.display_timeout = 60000;
    a.buttons[0].action = 200;
    a.buttons[0].number = 200;
    a.encoders[0].step = 0;
    midi_control_config_sanitise(&a);
    CHECK(a.global.bank_size == 16);
    CHECK(a.global.display_timeout == MIDI_CONTROL_DISPLAY_TIMEOUT_MAX);
    CHECK(a.buttons[0].action == MIDI_CONTROL_ACT_NONE);
    CHECK(a.buttons[0].number == 127);
    CHECK(a.encoders[0].step == 1);

    /* A button's flags are its last byte, which was reserved: the defaults
     * have none, so a record from before them reads as none. Bank Up's flag
     * round-trips, in the live mapping and in the profile it came from. */
    midi_control_config_defaults(&a);
    midi_control_config_write(&a, buf);
    for (unsigned i = 0; i < MIDI_CONTROL_BUTTON_COUNT; i++)
        CHECK(buf[48u + 8u * i + 7u] == 0);
    a.buttons[MIDI_CONTROL_BTN_BANK_UP].flags = MIDI_CONTROL_BUTTON_MOVES_PROGRAMS;
    midi_control_config_sanitise(&a);
    midi_control_config_write(&a, buf);
    CHECK(buf[48u + 8u * MIDI_CONTROL_BTN_BANK_UP + 7u] == MIDI_CONTROL_BUTTON_MOVES_PROGRAMS);
    CHECK(midi_control_config_read(&b, buf, sizeof(buf)) == MIDI_CONTROL_STATUS_OK);
    CHECK(b.buttons[MIDI_CONTROL_BTN_BANK_UP].flags == MIDI_CONTROL_BUTTON_MOVES_PROGRAMS);
    CHECK(b.profiles[0].buttons[MIDI_CONTROL_BTN_BANK_UP].flags == MIDI_CONTROL_BUTTON_MOVES_PROGRAMS);
    CHECK(b.buttons[MIDI_CONTROL_BTN_BANK_DOWN].flags == 0);
    CHECK(midi_control_config_equal(&a, &b));
    CHECK(midi_control_config_moves_programs(&b));
    b.buttons[MIDI_CONTROL_BTN_BANK_UP].flags = 0;
    CHECK(!midi_control_config_equal(&a, &b));
    CHECK(!midi_control_config_moves_programs(&b));
    /* Only a bank action's flag counts, before sanitise has had a chance to
     * clear it anywhere else. */
    b.buttons[MIDI_CONTROL_BTN_FS_A].flags = MIDI_CONTROL_BUTTON_MOVES_PROGRAMS;
    CHECK(!midi_control_config_moves_programs(&b));
    /* Sanitise keeps the flag only where it means something - Bank up and
     * Bank down - and only the bits there are. */
    a.buttons[MIDI_CONTROL_BTN_BANK_UP].flags = 0xFF;
    a.buttons[MIDI_CONTROL_BTN_BANK_DOWN].flags = MIDI_CONTROL_BUTTON_MOVES_PROGRAMS;
    a.buttons[MIDI_CONTROL_BTN_FS_A].flags = MIDI_CONTROL_BUTTON_MOVES_PROGRAMS;
    midi_control_config_sanitise(&a);
    CHECK(a.buttons[MIDI_CONTROL_BTN_BANK_UP].flags == MIDI_CONTROL_BUTTON_MOVES_PROGRAMS);
    CHECK(a.buttons[MIDI_CONTROL_BTN_BANK_DOWN].flags == MIDI_CONTROL_BUTTON_MOVES_PROGRAMS);
    CHECK(a.buttons[MIDI_CONTROL_BTN_FS_A].flags == 0);
}

static void test_engine_buttons(void)
{
    struct midi_control_config cfg;
    struct midi_control_engine e;

    midi_control_config_defaults(&cfg);
    midi_control_engine_init(&e, &cfg, sink, NULL);

    /* Footswitch A chooses preset A of the bank: Bank Select, then the Program
     * Change, on channel 1. Its LED is lit while that is the preset in use,
     * held or not - and at power-on, program 0, it is. */
    CHECK(e.led[2] == MIDI_CONTROL_LED_WHITE && e.led[5] == MIDI_CONTROL_LED_OFF);
    sink_reset();
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_FS_A, 1);
    CHECK(sink_is((const uint8_t[]){ 0xB0, 0, 0, 0xB0, 32, 0, 0xC0, 0 }, 8));
    CHECK(e.presses[MIDI_CONTROL_BTN_FS_A] == 1);
    sink_reset();
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_FS_A, 0);
    CHECK(g_out_len == 0 && e.led[2] == MIDI_CONTROL_LED_WHITE);
    /* D chooses preset D, and the light moves with it. */
    sink_reset();
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_FS_D, 1);
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_FS_D, 0);
    CHECK(sink_is((const uint8_t[]){ 0xB0, 0, 0, 0xB0, 32, 0, 0xC0, 3 }, 8));
    CHECK(e.program == 3 && e.led[5] == MIDI_CONTROL_LED_WHITE && e.led[2] == MIDI_CONTROL_LED_OFF);

    /* Footswitch A as the reference controller mapped it: note 60 momentary
     * on channel 1, lit while held. */
    cfg.buttons[MIDI_CONTROL_BTN_FS_A].action = MIDI_CONTROL_ACT_NOTE_MOMENTARY;
    cfg.buttons[MIDI_CONTROL_BTN_FS_A].number = 60;
    midi_control_engine_init(&e, &cfg, sink, NULL);
    sink_reset();
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_FS_A, 1);
    CHECK(sink_is((const uint8_t[]){ 0x90, 60, 127 }, 3));
    CHECK(e.led[2] == MIDI_CONTROL_LED_WHITE);
    CHECK(e.presses[MIDI_CONTROL_BTN_FS_A] == 1);
    sink_reset();
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_FS_A, 0);
    CHECK(sink_is((const uint8_t[]){ 0x80, 60, 0 }, 3));
    CHECK(e.led[2] == MIDI_CONTROL_LED_OFF);
    CHECK(e.presses[MIDI_CONTROL_BTN_FS_A] == 1);        /* a release is not a press */

    /* Tap lights the Tap lamp alone. */
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_TAP, 1);
    CHECK(e.led[6] == MIDI_CONTROL_LAMP_TAP);
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_TAP, 0);

    /* Bank up from 0: CC0, CC32, then PC 4. Bank down from 0 wraps to 124. */
    sink_reset();
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_BANK_UP, 1);
    CHECK(sink_is((const uint8_t[]){ 0xB0, 0, 0, 0xB0, 32, 0, 0xC0, 4 }, 8));
    CHECK(e.program == 4);
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_BANK_UP, 0);
    midi_control_engine_set_program(&e, 0);
    sink_reset();
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_BANK_DOWN, 1);
    CHECK(e.program == 124);
    CHECK(g_out_len == 8 && g_out[7] == 124);
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_BANK_DOWN, 0);

    /* Without bank select, just the PC. */
    cfg.global.bank_select = 0;
    midi_control_engine_config_changed(&e, &cfg);
    sink_reset();
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_BANK_UP, 1);
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_BANK_UP, 0);
    CHECK(sink_is((const uint8_t[]){ 0xC0, 0 }, 2));   /* 124 + 4 wraps to 0 */

    /* Toggles: CC 80 on channel 3 explicitly. */
    cfg.buttons[MIDI_CONTROL_BTN_FS_B].action = MIDI_CONTROL_ACT_CC_TOGGLE;
    cfg.buttons[MIDI_CONTROL_BTN_FS_B].channel = 3;
    cfg.buttons[MIDI_CONTROL_BTN_FS_B].number = 80;
    cfg.buttons[MIDI_CONTROL_BTN_FS_B].value_on = 127;
    cfg.buttons[MIDI_CONTROL_BTN_FS_B].value_off = 0;
    cfg.buttons[MIDI_CONTROL_BTN_FS_B].led_on = MIDI_CONTROL_LED_GREEN;
    cfg.buttons[MIDI_CONTROL_BTN_FS_B].led_off = MIDI_CONTROL_LED_RED;
    midi_control_engine_config_changed(&e, &cfg);
    CHECK(e.led[3] == MIDI_CONTROL_LED_RED);
    sink_reset();
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_FS_B, 1);
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_FS_B, 0);
    CHECK(sink_is((const uint8_t[]){ 0xB2, 80, 127 }, 3));
    CHECK(e.led[3] == MIDI_CONTROL_LED_GREEN);
    CHECK(midi_control_engine_button_active(&e, MIDI_CONTROL_BTN_FS_B));
    sink_reset();
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_FS_B, 1);
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_FS_B, 0);
    CHECK(sink_is((const uint8_t[]){ 0xB2, 80, 0 }, 3));
    CHECK(e.led[3] == MIDI_CONTROL_LED_RED);
    CHECK(!midi_control_engine_button_active(&e, MIDI_CONTROL_BTN_FS_B));
    CHECK(!midi_control_engine_button_active(&e, MIDI_CONTROL_BUTTON_COUNT));

    /* Note toggle. */
    cfg.buttons[MIDI_CONTROL_BTN_FS_C].action = MIDI_CONTROL_ACT_NOTE_TOGGLE;
    cfg.buttons[MIDI_CONTROL_BTN_FS_C].number = 40;
    midi_control_engine_config_changed(&e, &cfg);
    sink_reset();
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_FS_C, 1);
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_FS_C, 0);
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_FS_C, 1);
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_FS_C, 0);
    CHECK(sink_is((const uint8_t[]){ 0x90, 40, 127, 0x80, 40, 0 }, 6));

    /* Preset in bank: A-D select within the current bank and light the
     * selected one. */
    for (unsigned i = 0; i < 4; i++) {
        cfg.buttons[MIDI_CONTROL_BTN_FS_A + i].action = MIDI_CONTROL_ACT_PRESET_IN_BANK;
        cfg.buttons[MIDI_CONTROL_BTN_FS_A + i].number = (uint8_t)i;
        cfg.buttons[MIDI_CONTROL_BTN_FS_A + i].led_on = MIDI_CONTROL_LED_BLUE;
        cfg.buttons[MIDI_CONTROL_BTN_FS_A + i].led_off = MIDI_CONTROL_LED_OFF;
    }
    cfg.global.bank_select = 0;
    midi_control_engine_config_changed(&e, &cfg);
    midi_control_engine_set_program(&e, 9);          /* bank 3, preset B */
    CHECK(e.led[3] == MIDI_CONTROL_LED_BLUE && e.led[2] == MIDI_CONTROL_LED_OFF);
    CHECK(midi_control_engine_button_active(&e, MIDI_CONTROL_BTN_FS_B));
    CHECK(!midi_control_engine_button_active(&e, MIDI_CONTROL_BTN_FS_A));
    sink_reset();
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_FS_D, 1);
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_FS_D, 0);
    CHECK(sink_is((const uint8_t[]){ 0xC0, 11 }, 2));
    CHECK(e.led[5] == MIDI_CONTROL_LED_BLUE && e.led[3] == MIDI_CONTROL_LED_OFF);

    /* Program change from the host moves the program and the LEDs. */
    midi_control_engine_midi_in(&e, (const uint8_t[]){ 0xC5, 8 }, 2);
    CHECK(e.program == 8 && e.led[2] == MIDI_CONTROL_LED_BLUE);
    cfg.global.follow_program_change = 0;
    midi_control_engine_config_changed(&e, &cfg);
    midi_control_engine_midi_in(&e, (const uint8_t[]){ 0xC0, 3 }, 2);
    CHECK(e.program == 8);

    /* Program up/down with a step. */
    cfg.buttons[MIDI_CONTROL_BTN_VIEW].action = MIDI_CONTROL_ACT_PROGRAM_UP;
    cfg.buttons[MIDI_CONTROL_BTN_VIEW].number = 10;
    midi_control_engine_config_changed(&e, &cfg);
    sink_reset();
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_VIEW, 1);
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_VIEW, 0);
    CHECK(e.program == 18 && sink_is((const uint8_t[]){ 0xC0, 18 }, 2));

    /* None sends nothing. */
    cfg.buttons[MIDI_CONTROL_BTN_MENU].action = MIDI_CONTROL_ACT_NONE;
    midi_control_engine_config_changed(&e, &cfg);
    sink_reset();
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_MENU, 1);
    midi_control_engine_button(&e, MIDI_CONTROL_BTN_MENU, 0);
    CHECK(g_out_len == 0);

    /* ...but every press is counted, whatever the action, modulo 128. */
    CHECK(e.presses[MIDI_CONTROL_BTN_MENU] == 1);
    for (unsigned i = 0; i < 129u; i++) {
        midi_control_engine_button(&e, MIDI_CONTROL_BTN_MENU, 1);
        midi_control_engine_button(&e, MIDI_CONTROL_BTN_MENU, 0);
    }
    CHECK(e.presses[MIDI_CONTROL_BTN_MENU] == 2);
}

static void press(struct midi_control_engine *e, enum midi_control_button b)
{
    midi_control_engine_button(e, b, 1);
    midi_control_engine_button(e, b, 0);
}

/* Bank Up and Down that move the Program Change buttons (docs/05 section 3):
 * each press moves them a bank along with the program, so a Program Change
 * button lands a bank from where it did; without the flag they stay put. */
static void test_engine_bank_moves_programs(void)
{
    struct midi_control_config cfg;
    struct midi_control_engine e;

    midi_control_config_defaults(&cfg);
    cfg.global.bank_select = 0;
    /* Two Program Change buttons, out of order and not in the first bank -
     * what Preset in bank cannot do. */
    cfg.buttons[MIDI_CONTROL_BTN_FS_A].action = MIDI_CONTROL_ACT_PROGRAM;
    cfg.buttons[MIDI_CONTROL_BTN_FS_A].number = 10;
    cfg.buttons[MIDI_CONTROL_BTN_FS_B].action = MIDI_CONTROL_ACT_PROGRAM;
    cfg.buttons[MIDI_CONTROL_BTN_FS_B].number = 3;
    midi_control_engine_init(&e, &cfg, sink, NULL);
    CHECK(e.program_offset == 0);

    /* Without the flag, as in every record from before it: Bank Up steps the
     * program and the Program Change buttons send what they are set to. */
    sink_reset();
    press(&e, MIDI_CONTROL_BTN_BANK_UP);
    press(&e, MIDI_CONTROL_BTN_FS_A);
    CHECK(sink_is((const uint8_t[]){ 0xC0, 4, 0xC0, 10 }, 4));
    CHECK(e.program_offset == 0);

    /* With it: Bank Up sends the next bank's program as before, and the
     * buttons move a bank with it - and stay there, press after press. */
    cfg.buttons[MIDI_CONTROL_BTN_BANK_UP].flags = MIDI_CONTROL_BUTTON_MOVES_PROGRAMS;
    midi_control_engine_config_changed(&e, &cfg);
    sink_reset();
    press(&e, MIDI_CONTROL_BTN_BANK_UP);
    CHECK(sink_is((const uint8_t[]){ 0xC0, 14 }, 2) && e.program_offset == 4);
    sink_reset();
    press(&e, MIDI_CONTROL_BTN_FS_A);
    press(&e, MIDI_CONTROL_BTN_FS_A);
    press(&e, MIDI_CONTROL_BTN_FS_B);
    CHECK(sink_is((const uint8_t[]){ 0xC0, 14, 0xC0, 14, 0xC0, 7 }, 6));
    CHECK(e.program == 7);
    CHECK(midi_control_engine_button_program(&e, &cfg.buttons[MIDI_CONTROL_BTN_FS_A]) == 14);
    press(&e, MIDI_CONTROL_BTN_BANK_UP);
    press(&e, MIDI_CONTROL_BTN_BANK_UP);
    CHECK(e.program == 15 && e.program_offset == 12);
    sink_reset();
    press(&e, MIDI_CONTROL_BTN_FS_A);
    CHECK(sink_is((const uint8_t[]){ 0xC0, 22 }, 2));

    /* Each bank button moves them only if it says to: this Bank Down steps
     * the program alone. */
    press(&e, MIDI_CONTROL_BTN_BANK_DOWN);
    CHECK(e.program == 18 && e.program_offset == 12);

    /* Once it does, the two undo each other, and the offset wraps in 0..127
     * as the program does: a bank down from 0 is 124, which puts 3 on 127. */
    cfg.buttons[MIDI_CONTROL_BTN_BANK_DOWN].flags = MIDI_CONTROL_BUTTON_MOVES_PROGRAMS;
    midi_control_engine_config_changed(&e, &cfg);
    CHECK(e.program_offset == 12);                  /* a live edit leaves it where it was */
    for (unsigned i = 0; i < 4u; i++)
        press(&e, MIDI_CONTROL_BTN_BANK_DOWN);
    CHECK(e.program == 2 && e.program_offset == 124);
    sink_reset();
    press(&e, MIDI_CONTROL_BTN_FS_B);
    press(&e, MIDI_CONTROL_BTN_FS_A);
    CHECK(sink_is((const uint8_t[]){ 0xC0, 127, 0xC0, 6 }, 4));
    press(&e, MIDI_CONTROL_BTN_BANK_UP);
    CHECK(e.program_offset == 0 && e.program == 10);

    /* A bank size that does not divide 128 wraps both the same way. */
    cfg.global.bank_size = 3;
    midi_control_engine_config_changed(&e, &cfg);
    midi_control_engine_set_program(&e, 126);
    e.program_offset = 126;
    press(&e, MIDI_CONTROL_BTN_BANK_UP);
    CHECK(e.program == 1 && e.program_offset == 1);
    cfg.global.bank_size = 4;

    /* Other ways of moving the program move the program only: a Program
     * Change from the host, Program up, an encoder. */
    midi_control_engine_config_changed(&e, &cfg);
    e.program_offset = 8;
    midi_control_engine_midi_in(&e, (const uint8_t[]){ 0xC0, 50 }, 2);
    cfg.buttons[MIDI_CONTROL_BTN_VIEW].action = MIDI_CONTROL_ACT_PROGRAM_UP;
    cfg.buttons[MIDI_CONTROL_BTN_VIEW].number = 1;
    cfg.encoders[MIDI_CONTROL_ENC_1].mode = MIDI_CONTROL_ENCM_PROGRAM;
    midi_control_engine_config_changed(&e, &cfg);
    press(&e, MIDI_CONTROL_BTN_VIEW);
    midi_control_engine_encoder(&e, MIDI_CONTROL_ENC_1, 2);
    CHECK(e.program == 53 && e.program_offset == 8);

    /* A mapping with nothing to move them back puts them back. A flag left on
     * another action, before sanitise has cleared it, is not something that
     * moves them. */
    cfg.buttons[MIDI_CONTROL_BTN_BANK_UP].flags = 0;
    cfg.buttons[MIDI_CONTROL_BTN_BANK_DOWN].flags = 0;
    cfg.buttons[MIDI_CONTROL_BTN_FS_C].flags = MIDI_CONTROL_BUTTON_MOVES_PROGRAMS;
    midi_control_engine_config_changed(&e, &cfg);
    CHECK(e.program_offset == 0);
    sink_reset();
    press(&e, MIDI_CONTROL_BTN_FS_A);
    CHECK(sink_is((const uint8_t[]){ 0xC0, 10 }, 2));

    /* Any button mapped to Bank up moves them if it says so, not only the
     * Bank Up switch. */
    cfg.buttons[MIDI_CONTROL_BTN_PAGE].action = MIDI_CONTROL_ACT_BANK_UP;
    cfg.buttons[MIDI_CONTROL_BTN_PAGE].flags = MIDI_CONTROL_BUTTON_MOVES_PROGRAMS;
    midi_control_engine_config_changed(&e, &cfg);
    press(&e, MIDI_CONTROL_BTN_PAGE);
    CHECK(e.program_offset == 4);
}

static void test_engine_encoders_and_analog(void)
{
    struct midi_control_config cfg;
    struct midi_control_engine e;

    midi_control_config_defaults(&cfg);
    midi_control_engine_init(&e, &cfg, sink, NULL);

    sink_reset();
    midi_control_engine_encoder(&e, MIDI_CONTROL_ENC_1, 1);
    CHECK(sink_is((const uint8_t[]){ 0xB0, 16, 1 }, 3));
    sink_reset();
    midi_control_engine_encoder(&e, MIDI_CONTROL_ENC_MENU, -2);
    CHECK(sink_is((const uint8_t[]){ 0xB0, 22, 127, 0xB0, 22, 127 }, 6));
    CHECK(e.enc_turns[MIDI_CONTROL_ENC_1] == 1 && e.enc_turns[MIDI_CONTROL_ENC_MENU] == 126);   /* -2, modulo 128 */

    cfg.encoders[MIDI_CONTROL_ENC_2].mode = MIDI_CONTROL_ENCM_CC_RELATIVE_OFFSET;
    cfg.encoders[MIDI_CONTROL_ENC_3].mode = MIDI_CONTROL_ENCM_CC_ABSOLUTE;
    cfg.encoders[MIDI_CONTROL_ENC_3].step = 10;
    cfg.encoders[MIDI_CONTROL_ENC_4].mode = MIDI_CONTROL_ENCM_PROGRAM;
    cfg.global.bank_select = 0;
    midi_control_engine_config_changed(&e, &cfg);

    sink_reset();
    midi_control_engine_encoder(&e, MIDI_CONTROL_ENC_2, -1);
    CHECK(sink_is((const uint8_t[]){ 0xB0, 17, 63 }, 3));

    /* The largest step, 64, is 63 in both relative modes: two's complement has
     * no +64, and masking it to six bits used to send 0 clockwise. */
    cfg.encoders[MIDI_CONTROL_ENC_1].step = 64;
    cfg.encoders[MIDI_CONTROL_ENC_2].step = 64;
    midi_control_engine_config_changed(&e, &cfg);
    sink_reset();
    midi_control_engine_encoder(&e, MIDI_CONTROL_ENC_1, 1);
    midi_control_engine_encoder(&e, MIDI_CONTROL_ENC_1, -1);
    CHECK(sink_is((const uint8_t[]){ 0xB0, 16, 63, 0xB0, 16, 65 }, 6));
    sink_reset();
    midi_control_engine_encoder(&e, MIDI_CONTROL_ENC_2, 1);
    midi_control_engine_encoder(&e, MIDI_CONTROL_ENC_2, -1);
    CHECK(sink_is((const uint8_t[]){ 0xB0, 17, 127, 0xB0, 17, 1 }, 6));

    /* Absolute: from the initial 64, up by 10 twice, and it clamps at 127. */
    sink_reset();
    midi_control_engine_encoder(&e, MIDI_CONTROL_ENC_3, 2);
    CHECK(sink_is((const uint8_t[]){ 0xB0, 18, 74, 0xB0, 18, 84 }, 6));
    midi_control_engine_encoder(&e, MIDI_CONTROL_ENC_3, 10);
    CHECK(e.enc_value[MIDI_CONTROL_ENC_3] == 127);
    sink_reset();
    midi_control_engine_encoder(&e, MIDI_CONTROL_ENC_3, 1);
    CHECK(g_out_len == 0);                    /* already at the top: nothing sent */

    sink_reset();
    midi_control_engine_encoder(&e, MIDI_CONTROL_ENC_4, -1);
    CHECK(sink_is((const uint8_t[]){ 0xC0, 127 }, 2));

    /* A knob mapped to nothing sends nothing, but its turning is counted. */
    cfg.encoders[MIDI_CONTROL_ENC_5].mode = MIDI_CONTROL_ENCM_NONE;
    midi_control_engine_config_changed(&e, &cfg);
    sink_reset();
    midi_control_engine_encoder(&e, MIDI_CONTROL_ENC_5, 3);
    CHECK(g_out_len == 0 && e.enc_turns[MIDI_CONTROL_ENC_5] == 3);

    /* Analog: CC#11, de-duplicated, then inverted and ranged. */
    sink_reset();
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 0);
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 0);
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 127);
    CHECK(sink_is((const uint8_t[]){ 0xB0, 11, 0, 0xB0, 11, 127 }, 6));

    cfg.analogs[MIDI_CONTROL_ANALOG_PEDAL].invert = 1;
    cfg.analogs[MIDI_CONTROL_ANALOG_PEDAL].out_min = 20;
    cfg.analogs[MIDI_CONTROL_ANALOG_PEDAL].out_max = 100;
    midi_control_engine_config_changed(&e, &cfg);
    sink_reset();
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 127);   /* inverted -> 0 -> out_min */
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 0);     /* -> out_max */
    CHECK(sink_is((const uint8_t[]){ 0xB0, 11, 20, 0xB0, 11, 100 }, 6));

    cfg.analogs[MIDI_CONTROL_ANALOG_VOLUME].mode = MIDI_CONTROL_ANM_PITCH_BEND;
    cfg.analogs[MIDI_CONTROL_ANALOG_VOLUME].invert = 0;
    midi_control_engine_config_changed(&e, &cfg);
    sink_reset();
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_VOLUME, 127);
    CHECK(sink_is((const uint8_t[]){ 0xE0, 0x7F, 0x7F }, 3));
    sink_reset();
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_VOLUME, 0);
    CHECK(sink_is((const uint8_t[]){ 0xE0, 0, 0 }, 3));

    cfg.analogs[MIDI_CONTROL_ANALOG_VOLUME].mode = MIDI_CONTROL_ANM_CHANNEL_PRESSURE;
    midi_control_engine_config_changed(&e, &cfg);
    sink_reset();
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_VOLUME, 64);
    CHECK(sink_is((const uint8_t[]){ 0xD0, 64 }, 2));
}

/* Send a host->device SysEx, return the reply parsed as a message. */
static int roundtrip(struct midi_control_device *d, const uint8_t *req, uint32_t len,
                     struct midi_control_message *reply)
{
    sink_reset();
    midi_control_device_midi_in(d, req, len);
    return midi_control_parse_message(g_out, g_out_len, reply);
}

/* Send an EVENT. `sent` gets the number of bytes of channel MIDI that came
 * before the reply (they start at g_out); returns the reply's status, or -1
 * without a well-formed reply for EVENT. */
static int send_event(struct midi_control_device *d, uint8_t kind, uint8_t id, int value, uint32_t *sent)
{
    uint8_t req[MIDI_CONTROL_SYSEX_MAX];
    struct midi_control_message m;
    uint8_t cmd, status;
    uint32_t i = 0;

    sink_reset();
    midi_control_device_midi_in(d, req, midi_control_build_event(req, kind, id, value));
    while (i < g_out_len && g_out[i] != 0xF0)
        i++;
    *sent = i;
    if (!midi_control_parse_message(g_out + i, g_out_len - i, &m) || !midi_control_parse_result(&m, &cmd, &status)
        || cmd != MIDI_CONTROL_CMD_EVENT)
        return -1;
    return status;
}

static void test_device_protocol(void)
{
    struct midi_control_device d;
    struct midi_control_platform_ops ops = ops_with_storage();
    uint8_t req[MIDI_CONTROL_SYSEX_MAX];
    struct midi_control_message m;

    memset(g_slot, 0, sizeof(g_slot));
    memset(g_slot_written, 0, sizeof(g_slot_written));
    g_storage_state = MIDI_CONTROL_STORAGE_READY;
    g_fail_write = 0;

    midi_control_device_init(&d, &ops);
    CHECK(!d.loaded_from_storage);
    CHECK(d.config.global.bank_size == 4);

    /* IDENTIFY */
    struct midi_control_identity id;
    CHECK(roundtrip(&d, req, midi_control_build_simple(req, MIDI_CONTROL_CMD_IDENTIFY), &m));
    CHECK(midi_control_parse_identity(&m, &id));
    CHECK(id.protocol_version == MIDI_CONTROL_PROTOCOL_VERSION);
    CHECK(id.fw_major == 1 && id.fw_minor == 2 && id.fw_patch == 3);
    CHECK(id.platform == MIDI_CONTROL_PLATFORM_EMULATOR);
    CHECK(id.storage_state == MIDI_CONTROL_STORAGE_READY);
    CHECK(id.dirty == 0);
    CHECK(id.config_bytes == MIDI_CONTROL_CONFIG_BYTES);
    CHECK(strlen(MIDI_CONTROL_PRODUCT_NAME) < MIDI_CONTROL_CONFIG_NAME_LEN);
    CHECK(strcmp(id.name, MIDI_CONTROL_PRODUCT_NAME) == 0);

    /* GET_CONFIG round trips to an equal record. */
    uint8_t rec[MIDI_CONTROL_CONFIG_BYTES];
    struct midi_control_config got;
    CHECK(roundtrip(&d, req, midi_control_build_simple(req, MIDI_CONTROL_CMD_GET_CONFIG), &m));
    CHECK(m.command == MIDI_CONTROL_RPL_CONFIG);
    CHECK(midi_control_parse_config_payload(&m, rec, sizeof(rec)) == MIDI_CONTROL_CONFIG_BYTES);
    CHECK(midi_control_config_read(&got, rec, sizeof(rec)) == MIDI_CONTROL_STATUS_OK);
    CHECK(midi_control_config_equal(&got, &d.config));

    /* SET_CONFIG applies live and marks dirty. */
    got.buttons[MIDI_CONTROL_BTN_FS_A].action = MIDI_CONTROL_ACT_NOTE_MOMENTARY;
    got.buttons[MIDI_CONTROL_BTN_FS_A].number = 99;
    strcpy(got.global.name, "NEW NAME");
    midi_control_config_write(&got, rec);
    uint8_t cmd, status;
    CHECK(roundtrip(&d, req, midi_control_build_config(req, MIDI_CONTROL_CMD_SET_CONFIG, rec, sizeof(rec)), &m));
    CHECK(midi_control_parse_result(&m, &cmd, &status));
    CHECK(cmd == MIDI_CONTROL_CMD_SET_CONFIG && status == MIDI_CONTROL_STATUS_OK);
    CHECK(d.dirty == 1);
    CHECK(d.config.buttons[MIDI_CONTROL_BTN_FS_A].number == 99);
    sink_reset();
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_A, 1);
    CHECK(sink_is((const uint8_t[]){ 0x90, 99, 127 }, 3));
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_A, 0);

    /* A corrupted SET_CONFIG is refused and changes nothing. */
    rec[50] ^= 0x55;
    CHECK(roundtrip(&d, req, midi_control_build_config(req, MIDI_CONTROL_CMD_SET_CONFIG, rec, sizeof(rec)), &m));
    CHECK(midi_control_parse_result(&m, &cmd, &status) && status == MIDI_CONTROL_STATUS_BAD_CRC);
    CHECK(d.config.buttons[MIDI_CONTROL_BTN_FS_A].number == 99);

    /* SAVE: slot 0 first, sequence 1, clean afterwards. */
    CHECK(roundtrip(&d, req, midi_control_build_simple(req, MIDI_CONTROL_CMD_SAVE), &m));
    CHECK(midi_control_parse_result(&m, &cmd, &status) && status == MIDI_CONTROL_STATUS_OK);
    CHECK(g_slot_written[0] == 1 && g_slot_written[1] == 0);
    CHECK(d.config.sequence == 1 && d.dirty == 0);

    /* STATUS */
    struct midi_control_status_reply st;
    CHECK(roundtrip(&d, req, midi_control_build_simple(req, MIDI_CONTROL_CMD_GET_STATUS), &m));
    CHECK(midi_control_parse_status(&m, &st));
    CHECK(st.storage_state == MIDI_CONTROL_STORAGE_READY && st.dirty == 0 && st.sequence == 1);

    /* Second save alternates to slot 1 with sequence 2. */
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_A, 0);
    d.config.global.bank_size = 8;
    d.dirty = 1;
    CHECK(midi_control_device_save(&d) == MIDI_CONTROL_STATUS_OK);
    CHECK(g_slot_written[1] == 1 && d.config.sequence == 2 && d.newest_slot == 1);

    /* A fresh device finds the newest slot. */
    struct midi_control_device d2;
    midi_control_device_init(&d2, &ops);
    CHECK(d2.loaded_from_storage);
    CHECK(d2.config.sequence == 2 && d2.config.global.bank_size == 8);
    CHECK(strcmp(d2.config.global.name, "NEW NAME") == 0);
    CHECK(d2.newest_slot == 1);

    /* ...and with slot 1 corrupted, falls back to slot 0. */
    g_slot[1][40] ^= 1;
    midi_control_device_init(&d2, &ops);
    CHECK(d2.loaded_from_storage && d2.config.sequence == 1 && d2.config.global.bank_size == 4);
    g_slot[1][40] ^= 1;

    /* A failed write leaves the live config and the old save intact. */
    g_fail_write = 1;
    d.config.global.bank_size = 2;
    CHECK(midi_control_device_save(&d) == MIDI_CONTROL_STATUS_WRITE_FAILED);
    CHECK(d.config.sequence == 2);
    g_fail_write = 0;

    /* Locked storage: reads still work, saves are refused. */
    g_storage_state = MIDI_CONTROL_STORAGE_LOCKED;
    midi_control_device_init(&d2, &ops);
    CHECK(d2.loaded_from_storage && d2.config.sequence == 2);
    CHECK(roundtrip(&d2, req, midi_control_build_simple(req, MIDI_CONTROL_CMD_SAVE), &m));
    CHECK(midi_control_parse_result(&m, &cmd, &status) && status == MIDI_CONTROL_STATUS_STORAGE_LOCKED);

    /* A factory restore writes its own bytes over both slots: no valid record,
     * so the defaults, at start-up and on RELOAD. */
    uint8_t kept[2][SLOT_BYTES];
    memcpy(kept, g_slot, sizeof(kept));
    for (unsigned i = 0; i < sizeof(g_slot[0]); i++) {
        g_slot[0][i] = (uint8_t)(i * 7u + 1u);
        g_slot[1][i] = (uint8_t)(i * 11u + 5u);
    }
    struct midi_control_config defaults;
    midi_control_config_defaults(&defaults);
    midi_control_device_init(&d2, &ops);
    CHECK(!d2.loaded_from_storage && d2.config.sequence == 0 && midi_control_config_equal(&d2.config, &defaults));
    d2.config.global.bank_size = 8;
    CHECK(midi_control_device_reload(&d2) == MIDI_CONTROL_STATUS_OK);
    CHECK(!d2.loaded_from_storage && midi_control_config_equal(&d2.config, &defaults));
    memcpy(g_slot, kept, sizeof(kept));
    g_storage_state = MIDI_CONTROL_STORAGE_READY;

    /* No storage at all. */
    struct midi_control_platform_ops none = ops_with_storage();
    none.storage_write = NULL;
    midi_control_device_init(&d2, &none);
    CHECK(midi_control_device_storage_state(&d2) == MIDI_CONTROL_STORAGE_NONE);
    CHECK(midi_control_device_save(&d2) == MIDI_CONTROL_STATUS_NO_STORAGE);

    /* LOAD_DEFAULTS, SET_TEXT, RELOAD, unknown command. */
    CHECK(roundtrip(&d, req, midi_control_build_simple(req, MIDI_CONTROL_CMD_LOAD_DEFAULTS), &m));
    CHECK(d.config.global.bank_size == 4 && d.dirty == 1);
    CHECK(roundtrip(&d, req, midi_control_build_text(req, 1, "Big Lead"), &m));
    CHECK(midi_control_parse_result(&m, &cmd, &status) && status == MIDI_CONTROL_STATUS_OK);
    CHECK(strcmp(d.text[1], "Big Lead") == 0);
    CHECK(roundtrip(&d, req, midi_control_build_text(req, 5, "x"), &m));
    CHECK(midi_control_parse_result(&m, &cmd, &status) && status == MIDI_CONTROL_STATUS_INVALID_VALUE);
    CHECK(roundtrip(&d, req, midi_control_build_simple(req, MIDI_CONTROL_CMD_RELOAD), &m));
    CHECK(d.dirty == 0 && d.config.sequence == 2 && d.config.global.bank_size == 8);
    CHECK(roundtrip(&d, req, midi_control_build_simple(req, 0x7E), &m));
    CHECK(midi_control_parse_result(&m, &cmd, &status) && status == MIDI_CONTROL_STATUS_UNKNOWN_COMMAND);

    /* Foreign SysEx is ignored silently; channel messages reach the engine
     * even one byte at a time and with running status. */
    sink_reset();
    midi_control_device_midi_in(&d, (const uint8_t[]){ 0xF0, 0x43, 0x10, 0xF7 }, 4);
    CHECK(g_out_len == 0);
    const uint8_t pcs[] = { 0xC0, 5, 6, 0xF8, 7 };
    for (unsigned i = 0; i < sizeof(pcs); i++)
        midi_control_device_midi_in(&d, &pcs[i], 1);
    CHECK(d.engine.program == 7);

    /* A SysEx split across calls is reassembled. */
    uint32_t n = midi_control_build_simple(req, MIDI_CONTROL_CMD_GET_STATUS);
    sink_reset();
    midi_control_device_midi_in(&d, req, 2);
    midi_control_device_midi_in(&d, req + 2, n - 2);
    CHECK(midi_control_parse_message(g_out, g_out_len, &m) && m.command == MIDI_CONTROL_RPL_STATUS);
}

/* EVENT and GET_STATE: a host plays the device and reads back what it is
 * doing, and a device of the host's own mirroring that draws the same screen. */
static void test_device_state_and_events(void)
{
    struct midi_control_device d;
    struct midi_control_platform_ops ops = ops_with_storage();
    uint8_t req[MIDI_CONTROL_SYSEX_MAX];
    struct midi_control_message m;
    uint8_t cmd, status, kind, id;
    int value;
    uint32_t n, sent;

    memset(g_slot, 0, sizeof(g_slot));
    g_storage_state = MIDI_CONTROL_STORAGE_READY;
    g_fail_write = 0;
    midi_control_device_init(&d, &ops);

    /* EVENT framing: an encoder's detents are signed; each kind's value is clamped. */
    n = midi_control_build_event(req, MIDI_CONTROL_EVENT_ENCODER, MIDI_CONTROL_ENC_3, -2);
    CHECK(n == 9 && req[4] == MIDI_CONTROL_CMD_EVENT && req[7] == 0x7E);
    CHECK(midi_control_parse_message(req, n, &m) && midi_control_parse_event(&m, &kind, &id, &value));
    CHECK(kind == MIDI_CONTROL_EVENT_ENCODER && id == MIDI_CONTROL_ENC_3 && value == -2);
    midi_control_build_event(req, MIDI_CONTROL_EVENT_ENCODER, 0, -500);
    CHECK(midi_control_parse_message(req, n, &m) && midi_control_parse_event(&m, &kind, &id, &value) && value == -64);
    midi_control_build_event(req, MIDI_CONTROL_EVENT_ENCODER, 0, 500);
    CHECK(midi_control_parse_message(req, n, &m) && midi_control_parse_event(&m, &kind, &id, &value) && value == 63);
    midi_control_build_event(req, MIDI_CONTROL_EVENT_ANALOG, 0, 300);
    CHECK(midi_control_parse_message(req, n, &m) && midi_control_parse_event(&m, &kind, &id, &value) && value == 127);

    /* A footswitch pressed from the host sends what the panel would, then the reply. */
    CHECK(send_event(&d, MIDI_CONTROL_EVENT_BUTTON, MIDI_CONTROL_BTN_FS_A, 1, &sent) == MIDI_CONTROL_STATUS_OK);
    CHECK(sent == 8 && memcmp(g_out, (const uint8_t[]){ 0xB0, 0, 0, 0xB0, 32, 0, 0xC0, 0 }, 8) == 0);
    CHECK(d.engine.held[MIDI_CONTROL_BTN_FS_A] && d.engine.presses[MIDI_CONTROL_BTN_FS_A] == 1);
    CHECK(d.engine.led[2] == MIDI_CONTROL_LED_WHITE);

    /* Bank Up moves the program; its release sends nothing. */
    CHECK(send_event(&d, MIDI_CONTROL_EVENT_BUTTON, MIDI_CONTROL_BTN_BANK_UP, 1, &sent) == MIDI_CONTROL_STATUS_OK);
    CHECK(sent == 8 && g_out[6] == 0xC0 && g_out[7] == 4);
    CHECK(send_event(&d, MIDI_CONTROL_EVENT_BUTTON, MIDI_CONTROL_BTN_BANK_UP, 0, &sent) == MIDI_CONTROL_STATUS_OK);
    CHECK(sent == 0 && d.engine.program == 4);

    /* PAGE, the last switch, held: its flag is in the third byte. */
    CHECK(send_event(&d, MIDI_CONTROL_EVENT_BUTTON, MIDI_CONTROL_BTN_PAGE, 1, &sent) == MIDI_CONTROL_STATUS_OK);

    /* Knob 1 two detents back; the pedal to half way, which the factory
     * padding - a percent at the heel, fifteen at the toe - stretches to 75. */
    CHECK(send_event(&d, MIDI_CONTROL_EVENT_ENCODER, MIDI_CONTROL_ENC_1, -2, &sent) == MIDI_CONTROL_STATUS_OK);
    CHECK(sent == 6 && memcmp(g_out, (const uint8_t[]){ 0xB0, 16, 127, 0xB0, 16, 127 }, 6) == 0);
    CHECK(d.engine.enc_turns[MIDI_CONTROL_ENC_1] == 126);
    CHECK(send_event(&d, MIDI_CONTROL_EVENT_ANALOG, MIDI_CONTROL_ANALOG_PEDAL, 64, &sent) == MIDI_CONTROL_STATUS_OK);
    CHECK(sent == 3 && memcmp(g_out, (const uint8_t[]){ 0xB0, 11, 75 }, 3) == 0);
    CHECK(d.engine.analog_in[MIDI_CONTROL_ANALOG_PEDAL] == 64);

    /* Refused, and nothing done: ids past the end, a switch value other than
     * 0 or 1, an unknown kind, a payload cut short. */
    CHECK(send_event(&d, MIDI_CONTROL_EVENT_BUTTON, MIDI_CONTROL_BUTTON_COUNT, 1, &sent) == MIDI_CONTROL_STATUS_INVALID_VALUE && sent == 0);
    CHECK(send_event(&d, MIDI_CONTROL_EVENT_BUTTON, MIDI_CONTROL_BTN_FS_B, 5, &sent) == MIDI_CONTROL_STATUS_INVALID_VALUE && sent == 0);
    CHECK(!d.engine.held[MIDI_CONTROL_BTN_FS_B] && d.engine.presses[MIDI_CONTROL_BTN_FS_B] == 0);
    CHECK(send_event(&d, MIDI_CONTROL_EVENT_ENCODER, MIDI_CONTROL_ENCODER_COUNT, 1, &sent) == MIDI_CONTROL_STATUS_INVALID_VALUE && sent == 0);
    CHECK(send_event(&d, MIDI_CONTROL_EVENT_ANALOG, MIDI_CONTROL_ANALOG_COUNT, 1, &sent) == MIDI_CONTROL_STATUS_INVALID_VALUE && sent == 0);
    CHECK(send_event(&d, 3, 0, 0, &sent) == MIDI_CONTROL_STATUS_INVALID_VALUE && sent == 0);
    CHECK(roundtrip(&d, (const uint8_t[]){ 0xF0, 0x7D, 0x53, 0x47, MIDI_CONTROL_CMD_EVENT, 0, 0xF7 }, 7, &m));
    CHECK(midi_control_parse_result(&m, &cmd, &status) && cmd == MIDI_CONTROL_CMD_EVENT
          && status == MIDI_CONTROL_STATUS_INVALID_VALUE);

    /* A toggle on, an absolute knob moved, and both lines of text at their
     * longest. D is a Program Change that Bank Up has moved two banks. */
    struct midi_control_config cfg = d.config;
    cfg.buttons[MIDI_CONTROL_BTN_FS_C].action = MIDI_CONTROL_ACT_CC_TOGGLE;
    cfg.buttons[MIDI_CONTROL_BTN_FS_D].action = MIDI_CONTROL_ACT_PROGRAM;
    cfg.buttons[MIDI_CONTROL_BTN_FS_D].number = 2;
    cfg.buttons[MIDI_CONTROL_BTN_BANK_UP].flags = MIDI_CONTROL_BUTTON_MOVES_PROGRAMS;
    cfg.encoders[MIDI_CONTROL_ENC_2].mode = MIDI_CONTROL_ENCM_CC_ABSOLUTE;
    midi_control_device_set_config(&d, &cfg);
    d.engine.program_offset = 8;
    CHECK(send_event(&d, MIDI_CONTROL_EVENT_BUTTON, MIDI_CONTROL_BTN_FS_C, 1, &sent) == MIDI_CONTROL_STATUS_OK);
    CHECK(send_event(&d, MIDI_CONTROL_EVENT_BUTTON, MIDI_CONTROL_BTN_FS_C, 0, &sent) == MIDI_CONTROL_STATUS_OK);
    CHECK(send_event(&d, MIDI_CONTROL_EVENT_ENCODER, MIDI_CONTROL_ENC_2, 3, &sent) == MIDI_CONTROL_STATUS_OK);
    const char *longest = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
    CHECK(strlen(longest) == MIDI_CONTROL_TEXT_LEN);
    CHECK(roundtrip(&d, req, midi_control_build_text(req, 0, longest), &m));
    CHECK(roundtrip(&d, req, midi_control_build_text(req, 1, longest), &m));

    /* GET_STATE reports all of it, in the largest STATE there is. */
    struct midi_control_state_reply st;
    CHECK(roundtrip(&d, req, midi_control_build_simple(req, MIDI_CONTROL_CMD_GET_STATE), &m));
    CHECK(g_out_len == 138u && g_out_len <= MIDI_CONTROL_SYSEX_MAX);
    CHECK(midi_control_parse_state(&m, &st));
    CHECK(st.storage_state == MIDI_CONTROL_STORAGE_READY && st.dirty == 1 && st.program == 4);
    /* The tail, in order: display, the three profile bytes, the sixteen of the
     * calibration and the pedal's reading, the Program Change offset, then F7. */
    CHECK(st.display_off == 0 && g_out[g_out_len - 22u] == 0);
    CHECK(st.profile_active == 0 && st.profile_picking == 0 && st.profile_page == 0);
    CHECK(st.program_offset == 8 && g_out[g_out_len - 2u] == 8);
    CHECK(st.held[MIDI_CONTROL_BTN_FS_A] == 1 && st.held[MIDI_CONTROL_BTN_PAGE] == 1);
    CHECK(st.held[MIDI_CONTROL_BTN_FS_C] == 0 && st.held[MIDI_CONTROL_BTN_TOE] == 0 && st.held[MIDI_CONTROL_BTN_FS_B] == 0);
    CHECK(st.toggled[MIDI_CONTROL_BTN_FS_C] == 1 && st.toggled[MIDI_CONTROL_BTN_FS_A] == 0);
    CHECK(st.presses[MIDI_CONTROL_BTN_FS_A] == 1 && st.presses[MIDI_CONTROL_BTN_BANK_UP] == 1);
    CHECK(st.presses[MIDI_CONTROL_BTN_FS_C] == 1 && st.presses[MIDI_CONTROL_BTN_FS_B] == 0);
    CHECK(st.enc_turns[MIDI_CONTROL_ENC_1] == 126 && st.enc_turns[MIDI_CONTROL_ENC_2] == 3);
    CHECK(st.enc_value[MIDI_CONTROL_ENC_2] == d.engine.enc_value[MIDI_CONTROL_ENC_2]);
    CHECK(st.analog[MIDI_CONTROL_ANALOG_PEDAL] == 64 && st.analog[MIDI_CONTROL_ANALOG_VOLUME] == 0);
    CHECK(strcmp(st.text[0], longest) == 0 && strcmp(st.text[1], longest) == 0);

    /* Without the offset it is a protocol 5 device's reply; without the
     * calibration bytes as well, a protocol 4 one; without the profile bytes
     * too, a protocol 3 one; and without the display byte, a protocol 2 one.
     * All are read, and what such a device does not send reads as what it
     * has: Program Change buttons where they are set, nothing calibrating, no
     * raw pedal to calibrate, profile 1, no picker, lit. */
    struct midi_control_state_reply junk;
    struct midi_control_message damaged = m;
    damaged.payload_len -= 1u;
    CHECK(midi_control_parse_state(&damaged, &junk) && junk.program_offset == 0
          && junk.pedal_hi == st.pedal_hi && junk.pedal_flags == st.pedal_flags);
    damaged.payload_len -= 16u;
    CHECK(midi_control_parse_state(&damaged, &junk) && junk.cal_stage == MIDI_CONTROL_CAL_OFF
          && junk.pedal_flags == 0 && junk.pedal_hi == 0 && strcmp(junk.text[1], longest) == 0);
    damaged.payload_len -= 3u;
    CHECK(midi_control_parse_state(&damaged, &junk) && junk.display_off == 0
          && junk.profile_active == 0 && junk.profile_picking == 0 && junk.profile_page == 0);
    damaged.payload_len -= 1u;
    junk.display_off = 1;
    CHECK(midi_control_parse_state(&damaged, &junk) && junk.display_off == 0 && strcmp(junk.text[1], longest) == 0);

    /* Damaged replies are refused: cut short, or a line longer than a line can be. */
    damaged.payload_len -= 1u;
    CHECK(!midi_control_parse_state(&damaged, &junk));
    g_out[5 + 45] = 33;                           /* line 1's length */
    CHECK(!midi_control_parse_state(&m, &junk));
    g_out[5 + 45] = 32;
    CHECK(midi_control_parse_state(&m, &junk));

    /* A device of the host's own, with the same configuration, given the
     * reply: the same screen, the same LEDs, and nothing sent. */
    struct midi_control_device mirror;
    midi_control_device_init(&mirror, &ops);
    midi_control_device_set_config(&mirror, &d.config);
    sink_reset();
    midi_control_device_mirror(&mirror, &st);
    CHECK(g_out_len == 0);
    struct midi_control_ui_model dm, mm;
    struct midi_control_ui_view dv, mv;
    memset(&dm, 0, sizeof(dm));
    memset(&mm, 0, sizeof(mm));
    dm.device = &d;
    mm.device = &mirror;
    dm.usb_mounted = mm.usb_mounted = 1;
    midi_control_ui_view(&dv, &dm);
    midi_control_ui_view(&mv, &mm);
    CHECK(memcmp(&dv, &mv, sizeof(dv)) == 0);
    CHECK(strcmp(mv.program.line2, longest) == 0 && strcmp(mv.program.label, "2A") == 0);
    /* D, program 2 moved two banks of four, is drawn where it lands: 10, 3C. */
    CHECK(mirror.engine.program_offset == 8 && strcmp(mv.switches[5].label, "3C") == 0);
    uint8_t dled[MIDI_CONTROL_COLUMNS], mled[MIDI_CONTROL_COLUMNS];
    midi_control_device_leds(&d, dled);
    midi_control_device_leds(&mirror, mled);
    CHECK(memcmp(dled, mled, MIDI_CONTROL_COLUMNS) == 0);
    for (unsigned b = 0; b < MIDI_CONTROL_BUTTON_COUNT; b++)
        CHECK(midi_control_engine_button_active(&d.engine, (enum midi_control_button)b)
              == midi_control_engine_button_active(&mirror.engine, (enum midi_control_button)b));

    /* A shorter text replaces a longer one whole, and an empty line is empty. */
    CHECK(roundtrip(&d, req, midi_control_build_text(req, 0, "Plexi Drive"), &m));
    CHECK(roundtrip(&d, req, midi_control_build_text(req, 1, ""), &m));
    CHECK(roundtrip(&d, req, midi_control_build_simple(req, MIDI_CONTROL_CMD_GET_STATE), &m));
    CHECK(midi_control_parse_state(&m, &st));
    CHECK(strcmp(st.text[0], "Plexi Drive") == 0 && st.text[1][0] == 0);
    midi_control_device_mirror(&mirror, &st);
    CHECK(strcmp(mirror.text[0], "Plexi Drive") == 0 && mirror.text[1][0] == 0);

    /* The mirror keeps its own platform's storage state. */
    struct midi_control_platform_ops none = ops_with_storage();
    none.storage_state = NULL;
    midi_control_device_init(&mirror, &none);
    midi_control_device_mirror(&mirror, &st);
    CHECK(midi_control_device_storage_state(&mirror) == MIDI_CONTROL_STORAGE_NONE && mirror.engine.program == 4);
}

/* The display timeout: idle time on the platform's clock darkens the display;
 * a control used, however it arrives, lights it and still does what it does. */
static void test_display_timeout(void)
{
    static struct midi_control_device d, mirror, pedal;
    struct midi_control_platform_ops ops = ops_with_storage();
    uint8_t req[MIDI_CONTROL_SYSEX_MAX], rec[MIDI_CONTROL_CONFIG_BYTES];
    struct midi_control_message m;
    struct midi_control_state_reply st;
    struct midi_control_config c;
    uint8_t cmd, status;
    uint32_t sent;

    memset(g_slot, 0, sizeof(g_slot));
    g_storage_state = MIDI_CONTROL_STORAGE_READY;
    g_fail_write = 0;
    midi_control_device_init(&d, &ops);

    /* Never, by default, however long nothing is touched. */
    CHECK(d.config.global.display_timeout == 0 && midi_control_device_display_on(&d));
    midi_control_device_tick(&d, 0xFFFFFFFFu);
    midi_control_device_tick(&d, 0xFFFFFFFFu);
    CHECK(midi_control_device_display_on(&d));

    /* Ten seconds: lit at 9.999 s, dark at 10 s, and nothing is sent. */
    c = d.config;
    c.global.display_timeout = 10;
    midi_control_device_set_config(&d, &c);
    CHECK(midi_control_device_display_on(&d));
    sink_reset();
    midi_control_device_tick(&d, 9999);
    CHECK(midi_control_device_display_on(&d));
    midi_control_device_tick(&d, 1);
    CHECK(!midi_control_device_display_on(&d) && g_out_len == 0);

    /* A footswitch lights it and still chooses its preset; the release is a use too. */
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_A, 1);
    CHECK(midi_control_device_display_on(&d) && sink_is((const uint8_t[]){ 0xB0, 0, 0, 0xB0, 32, 0, 0xC0, 0 }, 8));
    midi_control_device_tick(&d, 9000);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_A, 0);
    midi_control_device_tick(&d, 9999);
    CHECK(midi_control_device_display_on(&d));
    midi_control_device_tick(&d, 1);
    CHECK(!midi_control_device_display_on(&d));

    /* So does a knob, the pedal, and a switch mapped to nothing, which sends nothing. */
    midi_control_device_encoder(&d, MIDI_CONTROL_ENC_1, 1);
    CHECK(midi_control_device_display_on(&d));
    midi_control_device_tick(&d, 10000);
    CHECK(!midi_control_device_display_on(&d));
    midi_control_device_analog(&d, MIDI_CONTROL_ANALOG_PEDAL, 12);
    CHECK(midi_control_device_display_on(&d));
    c.buttons[MIDI_CONTROL_BTN_VIEW].action = MIDI_CONTROL_ACT_NONE;
    midi_control_device_set_config(&d, &c);
    midi_control_device_tick(&d, 10000);
    CHECK(!midi_control_device_display_on(&d));
    sink_reset();
    midi_control_device_button(&d, MIDI_CONTROL_BTN_VIEW, 1);
    CHECK(midi_control_device_display_on(&d) && g_out_len == 0);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_VIEW, 0);

    /* The host does not light it: its Program Change, its text, GET_STATE. The
     * program moves all the same, and STATE says the display is dark. */
    midi_control_device_tick(&d, 10000);
    midi_control_device_midi_in(&d, (const uint8_t[]){ 0xC0, 9 }, 2);
    CHECK(d.engine.program == 9 && !midi_control_device_display_on(&d));
    CHECK(roundtrip(&d, req, midi_control_build_text(req, 0, "Plexi Drive"), &m));
    CHECK(roundtrip(&d, req, midi_control_build_simple(req, MIDI_CONTROL_CMD_GET_STATE), &m));
    CHECK(midi_control_parse_state(&m, &st) && st.display_off == 1 && st.program == 9);
    CHECK(!midi_control_device_display_on(&d));

    /* A device of the host's own takes the dark display from the report. */
    midi_control_device_init(&mirror, &ops);
    midi_control_device_set_config(&mirror, &d.config);
    midi_control_device_mirror(&mirror, &st);
    CHECK(!midi_control_device_display_on(&mirror));

    /* A control used from the host lights it, as on the panel; a refused one does
     * not. The next report says so, and the mirror lights. */
    CHECK(send_event(&d, MIDI_CONTROL_EVENT_ENCODER, MIDI_CONTROL_ENCODER_COUNT, 1, &sent)
          == MIDI_CONTROL_STATUS_INVALID_VALUE);
    CHECK(!midi_control_device_display_on(&d));
    CHECK(send_event(&d, MIDI_CONTROL_EVENT_ENCODER, MIDI_CONTROL_ENC_2, 1, &sent) == MIDI_CONTROL_STATUS_OK && sent == 3);
    CHECK(midi_control_device_display_on(&d));
    CHECK(roundtrip(&d, req, midi_control_build_simple(req, MIDI_CONTROL_CMD_GET_STATE), &m));
    CHECK(midi_control_parse_state(&m, &st) && st.display_off == 0);
    midi_control_device_mirror(&mirror, &st);
    CHECK(midi_control_device_display_on(&mirror));

    /* Saving is not a use. A new configuration is: RELOAD, LOAD_DEFAULTS, SET_CONFIG.
     * Never, sent while dark, lights it for good. */
    midi_control_device_tick(&d, 10000);
    CHECK(midi_control_device_save(&d) == MIDI_CONTROL_STATUS_OK && !midi_control_device_display_on(&d));
    CHECK(roundtrip(&d, req, midi_control_build_simple(req, MIDI_CONTROL_CMD_RELOAD), &m));
    CHECK(midi_control_parse_result(&m, &cmd, &status) && status == MIDI_CONTROL_STATUS_OK);
    CHECK(midi_control_device_display_on(&d) && d.config.global.display_timeout == 10);
    midi_control_device_tick(&d, 10000);
    CHECK(roundtrip(&d, req, midi_control_build_simple(req, MIDI_CONTROL_CMD_LOAD_DEFAULTS), &m));
    CHECK(midi_control_device_display_on(&d) && d.config.global.display_timeout == 0);
    midi_control_device_set_config(&d, &c);
    midi_control_device_tick(&d, 10000);
    CHECK(!midi_control_device_display_on(&d));
    c.global.display_timeout = 0;
    midi_control_config_write(&c, rec);
    CHECK(roundtrip(&d, req, midi_control_build_config(req, MIDI_CONTROL_CMD_SET_CONFIG, rec, sizeof(rec)), &m));
    CHECK(midi_control_parse_result(&m, &cmd, &status) && cmd == MIDI_CONTROL_CMD_SET_CONFIG
          && status == MIDI_CONTROL_STATUS_OK);
    midi_control_device_tick(&d, 0xFFFFFFFFu);
    CHECK(midi_control_device_display_on(&d));

    /* Idle time saturates: a tick that would wrap it back under the timeout
     * still reaches the timeout. */
    c.global.display_timeout = MIDI_CONTROL_DISPLAY_TIMEOUT_MAX;
    midi_control_device_set_config(&d, &c);
    midi_control_device_tick(&d, MIDI_CONTROL_DISPLAY_TIMEOUT_MAX * 1000u - 1000u);
    CHECK(midi_control_device_display_on(&d));
    midi_control_device_tick(&d, 0xFFFFFFFFu);
    CHECK(!midi_control_device_display_on(&d) && d.idle_ms == 0xFFFFFFFFu);

    /* The pedal as the unit itself reads it: raw converter counts, not a
     * position handed in. This is the only path a POD Go's own expression
     * pedal takes, so it is the one that decides whether a pedal move wakes a
     * dark screen. The volume knob arrives the same way.
     *
     * It wakes on a move the pedal reports, which is not every move: the
     * position has to change and the count has to have travelled the analog
     * hysteresis. A pedal at rest dithering by a count or two leaves the
     * screen dark, which is the point - otherwise nothing would ever go dark
     * with a pedal plugged in. */
    memset(g_slot, 0, sizeof(g_slot));
    midi_control_device_init(&pedal, &ops);
    midi_control_device_analog_seed(&pedal, MIDI_CONTROL_ANALOG_PEDAL, 280u, 664u, 1);
    c = pedal.config;
    c.global.display_timeout = 10;
    midi_control_device_set_config(&pedal, &c);

    /* The first count of the session is a position, so it lights it. */
    midi_control_device_analog_raw(&pedal, MIDI_CONTROL_ANALOG_PEDAL, 480u);
    CHECK(midi_control_device_display_on(&pedal));
    CHECK(pedal.engine.analog_in[MIDI_CONTROL_ANALOG_PEDAL] == 61);
    midi_control_device_tick(&pedal, 10000);
    CHECK(!midi_control_device_display_on(&pedal));

    /* Dither does not. Neither the same count nor one three counts away
     * reports a position, so neither is a use. */
    sink_reset();
    midi_control_device_analog_raw(&pedal, MIDI_CONTROL_ANALOG_PEDAL, 480u);
    midi_control_device_analog_raw(&pedal, MIDI_CONTROL_ANALOG_PEDAL, 483u);
    midi_control_device_analog_raw(&pedal, MIDI_CONTROL_ANALOG_PEDAL, 477u);
    CHECK(!midi_control_device_display_on(&pedal) && g_out_len == 0);

    /* A move past the hysteresis does, and still sends its CC. */
    midi_control_device_analog_raw(&pedal, MIDI_CONTROL_ANALOG_PEDAL, 490u);
    CHECK(midi_control_device_display_on(&pedal));
    CHECK(g_out_len == 3 && g_out[0] == 0xB0 && g_out[1] == 11);

    /* Toe down is a stop, which reports whatever the hysteresis says, so the
     * end of a sweep onto a dark screen always lights it. */
    midi_control_device_tick(&pedal, 10000);
    CHECK(!midi_control_device_display_on(&pedal));
    midi_control_device_analog_raw(&pedal, MIDI_CONTROL_ANALOG_PEDAL, 282u);
    CHECK(midi_control_device_display_on(&pedal)
          && pedal.engine.analog_in[MIDI_CONTROL_ANALOG_PEDAL] == 127);

    /* A pedal that is not there at all - an unseeded control - is not an
     * input, and a count arriving for it lights nothing. */
    midi_control_device_tick(&pedal, 10000);
    CHECK(!midi_control_device_display_on(&pedal));
    midi_control_device_analog_raw(&pedal, MIDI_CONTROL_ANALOG_VOLUME, 210u);
    CHECK(!midi_control_device_display_on(&pedal));
}

static void test_bootimage(void)
{
    /* A fake raw image: a vector table with a Thumb entry, then filler. */
    uint8_t raw[3000];
    for (unsigned i = 0; i < sizeof(raw); i++)
        raw[i] = (uint8_t)(i * 7u);
    raw[0] = 0x00; raw[1] = 0x20; raw[2] = 0x09; raw[3] = 0x10;   /* SP 0x10092000 */
    raw[4] = 0x01; raw[5] = 0x02; raw[6] = 0x00; raw[7] = 0x10;   /* entry 0x10000201 */

    uint8_t img[8192];
    const char *err = "unset";
    uint32_t n = midi_control_boot_build_emmc(raw, sizeof(raw), 0x10000000u, img, sizeof(img), &err);
    CHECK(n == 512u * 7u);               /* 0x34 + 12 + 3000 = 3064 -> 6 sectors, plus the descriptor */
    CHECK(err == NULL);

    struct midi_control_boot_info info;
    CHECK(midi_control_boot_inspect(img, n, &info));
    CHECK(info.has_descriptor && info.desc_crc_ok);
    CHECK(info.magic_ok && info.crc_ok && info.version_ok);
    CHECK(info.records == 1 && info.record_count == 1);
    CHECK(info.record[0].len == 3000 && info.record[0].dest == 0x10000000u);
    CHECK(info.record[0].run == 0x34u + 12u + 3000u);
    CHECK(info.entry == 0x10000201u);
    CHECK(info.handoff == 0x10000000u + 3000u);
    CHECK(info.records_in_range);
    CHECK(info.desc_size == info.size);
    CHECK(info.sectors == 7);
    CHECK(midi_control_boot_classify(img + 512) == MIDI_CONTROL_STORAGE_READY);

    /* The same image wrapped in a DFU header builds identically. */
    uint8_t dfu[4096], img2[8192];
    uint32_t dn = midi_control_dfu_wrap(raw, sizeof(raw), dfu, sizeof(dfu));
    CHECK(dn == 16u + 6u * 512u);
    CHECK(dfu[0] == 0x1A && dfu[1] == 0 && dfu[2] == 6 && dfu[3] == 0);
    uint32_t n2 = midi_control_boot_build_emmc(dfu, dn, 0x10000000u, img2, sizeof(img2), &err);
    /* The DFU body is block padded, so the record is 3072 bytes, not 3000 -
     * still the same sectors and a valid image. */
    CHECK(n2 == 512u * 8u);
    CHECK(midi_control_boot_inspect(img2, n2, &info) && info.crc_ok && info.record[0].len == 3072);

    /* The factory header shape classifies as locked; garbage as unknown. */
    uint8_t s1[512];
    memset(s1, 0, sizeof(s1));
    memcpy(s1, img + 512, 512);
    s1[0x10] = 7;                          /* seven records */
    s1[0x04] = 0x24; s1[0x05] = 0xA7; s1[0x06] = 0x0A; s1[0x07] = 0x00;   /* 698,148 */
    CHECK(midi_control_boot_classify(s1) == MIDI_CONTROL_STORAGE_LOCKED);
    memset(s1, 0xFF, 16);
    CHECK(midi_control_boot_classify(s1) == MIDI_CONTROL_STORAGE_UNKNOWN);

    /* Sizes read from the file, which the app opens from wherever the user
     * points it. Near 4 GB, a sum of two wraps and the checks passed a read
     * far past the buffer. */
    uint8_t bad[8192];
    uint8_t *hdr = bad + 512, *rec = hdr + MIDI_CONTROL_BOOT_HEADER_LEN;

    memcpy(bad, img, n);
    bad[12] = 0x00; bad[13] = 0xFF; bad[14] = 0xFF; bad[15] = 0xFF;      /* descriptor size 0xFFFFFF00 */
    CHECK(midi_control_boot_inspect(bad, n, &info) && info.has_descriptor && !info.desc_crc_ok);

    memcpy(bad, img, n);
    hdr[0x10] = 2;                                                    /* two records... */
    rec[0] = 0xB8; rec[1] = 0xFF; rec[2] = 0xFF; rec[3] = 0xFF;          /* ...the first 0xFFFFFFB8 long */
    CHECK(midi_control_boot_inspect(bad, n, &info) && info.record_count == 1 && !info.records_in_range);

    memcpy(bad, img, n);
    memset(hdr + 0x04, 0xFF, 4);                                       /* image size 0xFFFFFFFF */
    CHECK(midi_control_boot_inspect(bad, n, &info) && !info.crc_ok && info.record_count == 1);
    CHECK(info.sectors == 1u + 0x800000u);

    CHECK(midi_control_boot_range_name(0x10000000u, 0x10000100u) != NULL);
    CHECK(midi_control_boot_range_name(0x10000000u, 0x10000000u + 0xFFFFFF00u) == NULL);

    /* Refusals. */
    CHECK(midi_control_boot_build_emmc(raw, sizeof(raw), 0x30000000u, img, sizeof(img), &err) == 0);
    CHECK(err != NULL);
    raw[4] = 0x00;                         /* entry without the Thumb bit */
    CHECK(midi_control_boot_build_emmc(raw, sizeof(raw), 0x10000000u, img, sizeof(img), &err) == 0);
    CHECK(midi_control_boot_build_emmc(raw, sizeof(raw), 0x10000000u, img, 100, &err) == 0);
}

/* ---- factory release files ----------------------------------------------------- */

#define EMMC_BYTES     (MIDI_CONTROL_EMMC_SAFE_SECTORS * MIDI_CONTROL_SECTOR_BYTES)
#define HXF_TEST_SIZE  690000u     /* image bytes: past the slots, inside the backed-up region */
#define HXF_TEST_AT    0x3cu       /* where v2.50 puts the application section */

static void hxf_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void hxf_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Every 32-bit word byte-reversed, the way a release stores its main content. */
static void hxf_swap(uint8_t *dst, const uint8_t *src, uint32_t len)
{
    for (uint32_t i = 0; i + 4u <= len; i += 4u) {
        dst[i] = src[i + 3u];
        dst[i + 1u] = src[i + 2u];
        dst[i + 2u] = src[i + 1u];
        dst[i + 3u] = src[i];
    }
}

/* Main content holding a factory-shaped application section at HXF_TEST_AT - a
 * descriptor sector, then a two-record image of HXF_TEST_SIZE bytes, then a
 * trailer and the next descriptor - as the eMMC holds it in `le` and as a
 * release stores it in `stored`. Returns the content's length. */
static uint32_t make_release_content(uint8_t *le, uint8_t *stored)
{
    const uint32_t len = HXF_TEST_AT + 512u + HXF_TEST_SIZE + 36u + 16u;
    const uint32_t len1 = 600u;
    const uint32_t len2 = HXF_TEST_SIZE - 0x34u - 12u - len1 - 12u;

    memset(le, 0x11, len);
    uint8_t *sec = le + HXF_TEST_AT;
    for (unsigned i = 16; i < 512; i++)
        sec[i] = (uint8_t)(i * 3u);

    uint8_t *img = sec + 512;
    memset(img, 0, 0x34);
    hxf_le32(img + 0x00, MIDI_CONTROL_BOOT_MAGIC);
    hxf_le32(img + 0x04, HXF_TEST_SIZE);
    hxf_le32(img + 0x0C, 2u);
    hxf_le32(img + 0x10, 2u);
    hxf_le32(img + 0x14, 0x10000101u);
    hxf_le32(img + 0x18, 0x10001000u);

    uint8_t *r = img + 0x34;
    hxf_le32(r, len1);
    hxf_le32(r + 4, 0x10000000u);
    hxf_le32(r + 8, 0x34u + 12u + len1);
    for (uint32_t i = 0; i < len1; i++)
        r[12 + i] = (uint8_t)(i * 5u + 1u);
    r += 12u + len1;
    hxf_le32(r, len2);
    hxf_le32(r + 4, 0x28000000u);
    hxf_le32(r + 8, HXF_TEST_SIZE);
    for (uint32_t i = 0; i < len2; i++)
        r[12 + i] = (uint8_t)(i * 13u + 7u);

    hxf_le32(img + 0x08, midi_control_crc32(img + 0x34, HXF_TEST_SIZE - 0x34u));
    hxf_le32(sec + 0, MIDI_CONTROL_DESCRIPTOR_MAGIC);
    hxf_le32(sec + 4, MIDI_CONTROL_DESCRIPTOR_WORD1);
    hxf_le32(sec + 8, midi_control_crc32(img, HXF_TEST_SIZE));
    hxf_le32(sec + 12, HXF_TEST_SIZE);
    hxf_le32(img + HXF_TEST_SIZE + 36u, MIDI_CONTROL_DESCRIPTOR_MAGIC);

    hxf_swap(stored, le, len);
    return len;
}

static void test_hxf(void)
{
    static uint8_t le[EMMC_BYTES], stored[EMMC_BYTES], out[EMMC_BYTES], emmc[EMMC_BYTES];
    const char *err = "unset";

    /* The container: FORM L6FF, a HEAD, the two layout records, a payload with a pad byte. */
    uint8_t file[96];
    memset(file, 0, sizeof(file));
    memcpy(file, "FORM", 4);
    memcpy(file + 8, "L6FF", 4);
    memcpy(file + 12, "HEAD", 4);
    hxf_be32(file + 16, 8u);
    memcpy(file + 28, "dinf", 4);
    hxf_be32(file + 32, 16u);
    hxf_be32(file + 36, 0u);
    hxf_be32(file + 40, 0x00010000u);
    hxf_be32(file + 44, 0u);
    hxf_be32(file + 48, 0x0445C910u);
    memcpy(file + 52, "dinf", 4);
    hxf_be32(file + 56, 16u);
    hxf_be32(file + 60, 5u);
    hxf_be32(file + 64, 0x00010000u);
    hxf_be32(file + 68, 0x0445C910u);
    hxf_be32(file + 72, 0x17010u);
    memcpy(file + 76, "data", 4);
    hxf_be32(file + 80, 5u);
    file[84] = 0x78;
    file[85] = 0xDA;
    const uint32_t flen = 90u;             /* 84 + 5 bytes + the pad byte */
    hxf_be32(file + 4, flen - 8u);

    struct midi_control_hxf rel;
    CHECK(midi_control_hxf_parse(file, flen, &rel, &err) && err == NULL);
    CHECK(rel.main_len == 0x0445C910u && rel.data_offset == 84u && rel.data_len == 5u);
    CHECK(!midi_control_hxf_parse(file, flen - 1u, &rel, &err) && err != NULL);   /* truncated */
    file[8] = 'X';
    CHECK(!midi_control_hxf_parse(file, flen, &rel, &err));
    file[8] = 'L';
    memcpy(file + 76, "junk", 4);
    CHECK(!midi_control_hxf_parse(file, flen, &rel, &err));                        /* no payload */
    memcpy(file + 76, "data", 4);
    hxf_be32(file + 80, 200u);
    CHECK(!midi_control_hxf_parse(file, flen, &rel, &err));                        /* past the end */

    /* The factory region: the section, byte order restored, then zeros. */
    const uint32_t len = make_release_content(le, stored);
    CHECK(midi_control_hxf_factory_emmc(stored, len, out, sizeof(out), &err) == EMMC_BYTES && err == NULL);
    CHECK(memcmp(out, le + HXF_TEST_AT, 512u + HXF_TEST_SIZE) == 0);
    int tail_zero = 1;
    for (uint32_t i = 512u + HXF_TEST_SIZE; i < EMMC_BYTES; i++)
        tail_zero &= out[i] == 0;
    CHECK(tail_zero);
    struct midi_control_boot_info info;
    CHECK(midi_control_boot_inspect(out, EMMC_BYTES, &info) && info.desc_crc_ok && info.crc_ok && info.records == 2);

    /* Refusals: a damaged byte, a short buffer, the words out of step, one of ours. */
    stored[HXF_TEST_AT + 512u + 4000u] ^= 0x01;
    CHECK(midi_control_hxf_factory_emmc(stored, len, out, sizeof(out), &err) == 0 && err != NULL);
    stored[HXF_TEST_AT + 512u + 4000u] ^= 0x01;
    CHECK(midi_control_hxf_factory_emmc(stored, len, out, EMMC_BYTES - 1u, &err) == 0);
    CHECK(midi_control_hxf_factory_emmc(stored + 1, len - 1u, out, sizeof(out), &err) == 0);

    uint8_t raw[3000], ours[8192], ours_le[8192], ours_stored[8192];
    for (unsigned i = 0; i < sizeof(raw); i++)
        raw[i] = (uint8_t)(i * 7u);
    raw[4] = 0x01; raw[5] = 0x02; raw[6] = 0x00; raw[7] = 0x10;   /* entry 0x10000201 */
    const uint32_t on = midi_control_boot_build_emmc(raw, sizeof(raw), 0x10000000u, ours, sizeof(ours), &err);
    CHECK(on == 512u * 7u);
    memset(ours_le, 0x11, sizeof(ours_le));
    memcpy(ours_le + HXF_TEST_AT, ours, on);
    hxf_swap(ours_stored, ours_le, HXF_TEST_AT + on);
    CHECK(midi_control_hxf_factory_emmc(ours_stored, HXF_TEST_AT + on, out, sizeof(out), &err) == 0 && err != NULL);

    /* The residual: sector 513 to the end of the image, less the slots' 32
     * sectors, whatever sector 1 holds. A unit holding exactly the factory image. */
    CHECK(midi_control_hxf_factory_emmc(stored, len, out, sizeof(out), &err) == EMMC_BYTES);
    const uint32_t residual = 512u + HXF_TEST_SIZE - 513u * 512u - 32u * 512u;
    struct midi_control_residual res;
    memcpy(emmc, out, EMMC_BYTES);
    midi_control_boot_residual_compare(emmc, out, EMMC_BYTES, &res);
    CHECK(res.identical && res.head_identical && res.emmc_state == MIDI_CONTROL_STORAGE_LOCKED);
    CHECK(res.from == 513u * 512u && res.to == 512u + HXF_TEST_SIZE);
    CHECK(res.compared == residual && res.differing == 0);

    /* One of ours installed over it, both slots saved, and the leftovers of a
     * bigger image installed before: the residual still agrees. */
    static uint8_t installed[EMMC_BYTES];
    memcpy(emmc, ours, on);
    memset(emmc + 7u * 512u, 0xEE, 100u * 512u);
    memset(emmc + MIDI_CONTROL_EMMC_CONFIG_SLOT_LBA(0u) * 512u, 0x5A, 512u);
    memset(emmc + MIDI_CONTROL_EMMC_CONFIG_SLOT_LBA(1u) * 512u, 0xA5, 512u);
    memcpy(installed, emmc, EMMC_BYTES);
    midi_control_boot_residual_compare(emmc, out, EMMC_BYTES, &res);
    CHECK(!res.identical && !res.head_identical && res.emmc_state == MIDI_CONTROL_STORAGE_READY);
    CHECK(res.compared == residual && res.differing == 0);

    /* A restore over that which stopped after sector k - power lost, or WRITE
     * FAILED - has written sectors 0..k and left the rest, the saved
     * configuration included. From k = 1 sector 1 holds the factory header,
     * and the release still agrees; from the second slot on, nothing of ours
     * is left. */
    static const uint32_t stops[] = { 0u, 1u, 2u, 106u, 512u, 513u, 900u, 1299u, 1300u, 1315u, 1316u, 1364u };
    for (unsigned n = 0; n < sizeof(stops) / sizeof(stops[0]); n++) {
        const uint32_t k = stops[n];
        memcpy(emmc, installed, EMMC_BYTES);
        memcpy(emmc, out, (k + 1u) * 512u);
        midi_control_boot_residual_compare(emmc, out, EMMC_BYTES, &res);
        CHECK(res.emmc_state == (k >= 1u ? MIDI_CONTROL_STORAGE_LOCKED : MIDI_CONTROL_STORAGE_READY));
        CHECK(res.head_identical == (k >= 1u));
        CHECK(res.identical == (k >= MIDI_CONTROL_EMMC_CONFIG_SLOT_LBA(1u)));
        CHECK(res.compared == residual && res.differing == 0);
    }

    /* A different release: one byte of the residual differs, with one of ours
     * installed, and under the factory header of a restore of the wrong
     * release that stopped short of that byte. */
    memcpy(emmc, installed, EMMC_BYTES);
    emmc[600u * 512u + 3u] ^= 0x40;
    midi_control_boot_residual_compare(emmc, out, EMMC_BYTES, &res);
    CHECK(res.differing == 1u && res.first_difference == 600u * 512u + 3u);
    memcpy(emmc, out, 400u * 512u);
    midi_control_boot_residual_compare(emmc, out, EMMC_BYTES, &res);
    CHECK(res.emmc_state == MIDI_CONTROL_STORAGE_LOCKED && res.head_identical);
    CHECK(res.differing == 1u && res.first_difference == 600u * 512u + 3u);

    /* This release written with one sector wrong - VERIFY FAILED - differs
     * too; sectors 0 and 1 are what tell it from a different release. */
    memcpy(emmc, out, EMMC_BYTES);
    emmc[900u * 512u + 17u] ^= 0x01;
    midi_control_boot_residual_compare(emmc, out, EMMC_BYTES, &res);
    CHECK(!res.identical && res.head_identical);
    CHECK(res.differing == 1u && res.first_difference == 900u * 512u + 17u);

    /* The slots are never compared, even under the factory header... */
    memcpy(emmc, out, EMMC_BYTES);
    emmc[MIDI_CONTROL_EMMC_CONFIG_SLOT_LBA(0u) * 512u + 5u] ^= 0xFF;
    emmc[MIDI_CONTROL_EMMC_CONFIG_SLOT_LBA(2u) * 512u - 1u] ^= 0xFF;
    midi_control_boot_residual_compare(emmc, out, EMMC_BYTES, &res);
    CHECK(!res.identical && res.head_identical && res.emmc_state == MIDI_CONTROL_STORAGE_LOCKED);
    CHECK(res.compared == residual && res.differing == 0);

    /* ...and the bytes either side of them are. */
    emmc[MIDI_CONTROL_EMMC_CONFIG_SLOT_LBA(0u) * 512u - 1u] ^= 0xFF;
    emmc[MIDI_CONTROL_EMMC_CONFIG_SLOT_LBA(2u) * 512u] ^= 0xFF;
    midi_control_boot_residual_compare(emmc, out, EMMC_BYTES, &res);
    CHECK(res.differing == 2u && res.first_difference == MIDI_CONTROL_EMMC_CONFIG_SLOT_LBA(0u) * 512u - 1u);

    /* The same release, damaged at the front: not identical, and the residual
     * agrees. The head is sectors 0 and 1 and no more. */
    memcpy(emmc, out, EMMC_BYTES);
    emmc[512u * 20u] ^= 0xFF;
    midi_control_boot_residual_compare(emmc, out, EMMC_BYTES, &res);
    CHECK(!res.identical && res.head_identical && res.emmc_state == MIDI_CONTROL_STORAGE_LOCKED && res.differing == 0);
    emmc[512u + 100u] ^= 0xFF;
    midi_control_boot_residual_compare(emmc, out, EMMC_BYTES, &res);
    CHECK(!res.head_identical && res.emmc_state == MIDI_CONTROL_STORAGE_LOCKED && res.differing == 0);
    emmc[512u + 100u] ^= 0xFF;
    emmc[3u] ^= 0xFF;
    midi_control_boot_residual_compare(emmc, out, EMMC_BYTES, &res);
    CHECK(!res.head_identical && res.differing == 0);
}

#define LCD_W MIDI_CONTROL_LCD_WIDTH
#define LCD_H MIDI_CONTROL_LCD_HEIGHT

/* The bounding box of every pixel that is not `ground`; x1 < 0 when there is none. */
static void ink_box(const struct midi_control_gfx *g, unsigned ground, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = LCD_W;
    *y0 = LCD_H;
    *x1 = -1;
    *y1 = -1;
    for (int y = 0; y < LCD_H; y++) {
        for (int x = 0; x < LCD_W; x++) {
            if (midi_control_gfx_read(g, x, y) == ground)
                continue;
            *x0 = x < *x0 ? x : *x0;
            *y0 = y < *y0 ? y : *y0;
            *x1 = x > *x1 ? x : *x1;
            *y1 = y > *y1 ? y : *y1;
        }
    }
}

static void test_gfx(void)
{
    static uint8_t fb[MIDI_CONTROL_LCD_RGB565_BYTES], fb2[MIDI_CONTROL_LCD_RGB565_BYTES];
    static uint8_t mono[MIDI_CONTROL_LCD_MONO_BYTES + 2];
    const uint16_t *px = (const uint16_t *)(const void *)fb;
    struct midi_control_gfx g, g2, gm;
    int x0, y0, x1, y1;
    char label[8];

    midi_control_ui_program_label(0, 4, label);
    CHECK(strcmp(label, "1A") == 0);
    midi_control_ui_program_label(127, 4, label);
    CHECK(strcmp(label, "32D") == 0);
    midi_control_ui_program_label(5, 1, label);
    CHECK(strcmp(label, "6A") == 0);

    /* Rotated, as the firmware draws: the panel's origin is the far corner of
     * the buffer, and read honours the rotation. */
    midi_control_gfx_init(&g, fb, MIDI_CONTROL_GFX_RGB565, 1);
    midi_control_gfx_clear(&g, MIDI_CONTROL_BLACK);
    midi_control_gfx_fill_rect(&g, 0, 0, 1, 1, MIDI_CONTROL_WHITE);
    CHECK(px[LCD_W * LCD_H - 1] == MIDI_CONTROL_WHITE && px[0] == MIDI_CONTROL_BLACK);
    CHECK(midi_control_gfx_read(&g, 0, 0) == MIDI_CONTROL_WHITE);

    /* The clip holds rectangles, lines and text alike. */
    midi_control_gfx_init(&g, fb, MIDI_CONTROL_GFX_RGB565, 0);
    midi_control_gfx_clear(&g, MIDI_CONTROL_BLACK);
    midi_control_gfx_set_clip(&g, 10, 10, 5, 5);
    midi_control_gfx_fill_rect(&g, 0, 0, LCD_W, LCD_H, MIDI_CONTROL_WHITE);
    midi_control_gfx_line(&g, 0, 0, MIDI_CONTROL_Q4(LCD_W), MIDI_CONTROL_Q4(LCD_H), MIDI_CONTROL_Q4(20), MIDI_CONTROL_RED);
    midi_control_gfx_text(&g, 0, 0, 60, 0, "WWW", MIDI_CONTROL_RED);
    ink_box(&g, MIDI_CONTROL_BLACK, &x0, &y0, &x1, &y1);
    CHECK(x0 == 10 && y0 == 10 && x1 == 14 && y1 == 14);
    midi_control_gfx_set_clip(&g, 0, 0, LCD_W, LCD_H);

    /* Anti-aliasing: a 2 px line along pixel centres fills its row and half
     * covers the rows either side. */
    midi_control_gfx_clear(&g, MIDI_CONTROL_BLACK);
    midi_control_gfx_line(&g, MIDI_CONTROL_Q4(20) + 8, MIDI_CONTROL_Q4(30) + 8, MIDI_CONTROL_Q4(60) + 8,
                          MIDI_CONTROL_Q4(30) + 8, MIDI_CONTROL_Q4(2), MIDI_CONTROL_WHITE);
    CHECK(px[30 * LCD_W + 40] == MIDI_CONTROL_WHITE);
    CHECK(px[29 * LCD_W + 40] != MIDI_CONTROL_WHITE && px[29 * LCD_W + 40] != MIDI_CONTROL_BLACK);
    CHECK(px[27 * LCD_W + 40] == MIDI_CONTROL_BLACK);
    CHECK(midi_control_gfx_mix(MIDI_CONTROL_BLACK, MIDI_CONTROL_WHITE, 256) == MIDI_CONTROL_WHITE);
    CHECK(midi_control_gfx_mix(MIDI_CONTROL_BLACK, MIDI_CONTROL_WHITE, 0) == MIDI_CONTROL_BLACK);
    CHECK(midi_control_gfx_cos(0) == 4096 && midi_control_gfx_sin(16) == 4096 && midi_control_gfx_cos(32) == -4096);

    /* Copy matches panel coordinates whatever the buffers' rotations. */
    midi_control_gfx_init(&g2, fb2, MIDI_CONTROL_GFX_RGB565, 1);
    midi_control_gfx_clear(&g2, MIDI_CONTROL_BLACK);
    midi_control_gfx_fill_rect(&g2, 100, 50, 30, 20, MIDI_CONTROL_RED);
    midi_control_gfx_clear(&g, MIDI_CONTROL_BLACK);
    midi_control_gfx_copy(&g, &g2, 90, 40, 60, 40);
    ink_box(&g, MIDI_CONTROL_BLACK, &x0, &y0, &x1, &y1);
    CHECK(x0 == 100 && y0 == 50 && x1 == 129 && y1 == 69);
    midi_control_gfx_init(&g, fb, MIDI_CONTROL_GFX_RGB565, 1);
    midi_control_gfx_clear(&g, MIDI_CONTROL_BLACK);
    midi_control_gfx_copy(&g, &g2, 0, 0, LCD_W, LCD_H);
    CHECK(memcmp(fb, fb2, sizeof(fb)) == 0);

    /* Mono lights bright colours and not dark ones, inside its buffer. */
    mono[0] = 0x5A;
    mono[MIDI_CONTROL_LCD_MONO_BYTES + 1] = 0xA5;
    midi_control_gfx_init(&gm, mono + 1, MIDI_CONTROL_GFX_MONO, 1);
    midi_control_gfx_clear(&gm, MIDI_CONTROL_DIM);
    CHECK(midi_control_gfx_read(&gm, 5, 5) == MIDI_CONTROL_BLACK);
    midi_control_gfx_pixel(&gm, 5, 5, MIDI_CONTROL_GREY);
    CHECK(midi_control_gfx_read(&gm, 5, 5) == MIDI_CONTROL_WHITE);
    midi_control_gfx_line(&gm, -800, -800, 9000, 9000, MIDI_CONTROL_Q4(9), MIDI_CONTROL_WHITE);
    CHECK(mono[0] == 0x5A && mono[MIDI_CONTROL_LCD_MONO_BYTES + 1] == 0xA5);

    /* Text: every glyph's ink stays near the box its width promises (a typo
     * in a stroke would not), and a capital fills exactly its size. */
    midi_control_gfx_init(&g, fb, MIDI_CONTROL_GFX_RGB565, 0);
    for (int c = 0x01; c <= 0x7E; c++) {
        const char s[2] = { (char)c, 0 };
        const int w = midi_control_gfx_text_width(20, 0, s);
        midi_control_gfx_clear(&g, MIDI_CONTROL_BLACK);
        midi_control_gfx_text(&g, 200, 100, 20, 0, s, MIDI_CONTROL_WHITE);
        ink_box(&g, MIDI_CONTROL_BLACK, &x0, &y0, &x1, &y1);
        if (x1 < 0)
            continue;
        CHECK(x0 >= 200 - 10 && x1 < 200 + w + 10 && y0 >= 100 - 5 && y1 < 100 + 30);
    }
    midi_control_gfx_clear(&g, MIDI_CONTROL_BLACK);
    const int hw = midi_control_gfx_text_width(20, 0, "H");
    CHECK(midi_control_gfx_text(&g, 200, 100, 20, 0, "H", MIDI_CONTROL_WHITE) == 200 + hw);
    ink_box(&g, MIDI_CONTROL_BLACK, &x0, &y0, &x1, &y1);
    CHECK(x0 == 200 && y0 == 100 && x1 >= 200 + hw - 2 && x1 <= 200 + hw && y1 >= 118 && y1 <= 120);
    CHECK(midi_control_gfx_text_width(40, 0, "ABC") > midi_control_gfx_text_width(20, 0, "ABC"));
    CHECK(midi_control_gfx_text_width(20, 0, "") == 0);
}

static void test_ui(void)
{
    static struct midi_control_device d;
    static uint8_t fb[MIDI_CONTROL_LCD_RGB565_BYTES], full[MIDI_CONTROL_LCD_RGB565_BYTES];
    static uint8_t before[MIDI_CONTROL_LCD_RGB565_BYTES], mono[MIDI_CONTROL_LCD_MONO_BYTES + 2];
    const struct midi_control_platform_ops ops = ops_with_storage();
    struct midi_control_gfx g, gf, gb, gm;
    struct midi_control_ui_model m;
    struct midi_control_ui_view v;
    struct midi_control_ui_cache cache;
    struct midi_control_rect r;

    g_storage_state = MIDI_CONTROL_STORAGE_READY;
    g_fail_write = 0;
    memset(g_slot, 0, sizeof(g_slot));
    midi_control_device_init(&d, &ops);

    /* A-D recall presets in the bank, Tap is a CC toggle, encoder 1 absolute,
     * the pedal unmapped, preset 45 with a name from the host. */
    struct midi_control_config c = d.config;
    for (unsigned i = 0; i < 4; i++) {
        c.buttons[MIDI_CONTROL_BTN_FS_A + i].action = MIDI_CONTROL_ACT_PRESET_IN_BANK;
        c.buttons[MIDI_CONTROL_BTN_FS_A + i].number = (uint8_t)i;
        c.buttons[MIDI_CONTROL_BTN_FS_A + i].led_on = MIDI_CONTROL_LED_GREEN;
    }
    c.buttons[MIDI_CONTROL_BTN_TAP].action = MIDI_CONTROL_ACT_CC_TOGGLE;
    c.encoders[MIDI_CONTROL_ENC_1].mode = MIDI_CONTROL_ENCM_CC_ABSOLUTE;
    c.analogs[MIDI_CONTROL_ANALOG_PEDAL].mode = MIDI_CONTROL_ANM_NONE;
    midi_control_device_set_config(&d, &c);
    midi_control_engine_set_program(&d.engine, 45);
    strcpy(d.text[0], "Plexi Drive");

    /* The start-up screen needs no device. */
    memset(&m, 0, sizeof(m));
    m.stage = "EMMC";
    midi_control_ui_view(&v, &m);
    CHECK(!v.running && strcmp(v.start.stage, "EMMC") == 0);

    /* The longest stage the firmware shows arrives whole, fits the screen
     * large, and fits the bottom line small beside the note. */
    m.stage = "FIRMWARE UPDATE MODE";
    m.note = "INHERITED";
    midi_control_ui_view(&v, &m);
    CHECK(strcmp(v.start.stage, "FIRMWARE UPDATE MODE") == 0);
    CHECK(midi_control_gfx_text_width(20, 0, v.start.stage) <= MIDI_CONTROL_LCD_WIDTH - 20);
    CHECK(midi_control_gfx_text_width(10, 0, v.start.stage) + midi_control_gfx_text_width(10, 0, v.start.note) + 40
          <= MIDI_CONTROL_LCD_WIDTH);

    m.device = &d;
    m.usb_mounted = 1;
    midi_control_ui_view(&v, &m);
    CHECK(v.running);
    CHECK(strcmp(v.status.title, MIDI_CONTROL_PRODUCT_NAME) == 0 && strcmp(v.status.channel, "CH 1") == 0);
    CHECK(v.status.storage == MIDI_CONTROL_STORAGE_READY && v.status.dirty && v.status.usb);
    CHECK(strcmp(v.program.label, "12B") == 0 && strcmp(v.program.number, "PC 45") == 0);
    CHECK(strcmp(v.program.bank, "Bank 12") == 0 && strcmp(v.program.preset, "Preset B") == 0);
    CHECK(strcmp(v.program.line1, "Plexi Drive") == 0 && v.program.line2[0] == 0);
    /* Left to right: where Bank Up and Down land, the preset each of A-D recalls, Tap. */
    CHECK(strcmp(v.switches[0].label, MIDI_CONTROL_SYM_UP "13B") == 0);
    CHECK(strcmp(v.switches[1].label, MIDI_CONTROL_SYM_DOWN "11B") == 0);
    CHECK(strcmp(v.switches[2].label, "12A") == 0 && strcmp(v.switches[5].label, "12D") == 0);
    CHECK(v.switches[3].active && !v.switches[2].active && v.switches[3].led == MIDI_CONTROL_LED_GREEN);
    CHECK(strcmp(v.switches[6].label, "CC 64") == 0 && !v.switches[6].active);
    CHECK(strcmp(v.knobs[0].label, "CC 16") == 0 && v.knobs[0].value == 64);
    CHECK(strcmp(v.knobs[1].label, "CC 17") == 0 && v.knobs[1].value == 0);
    CHECK(v.meters[0].mapped && !v.meters[1].mapped);

    /* The first update draws everything; with nothing changed the next draws nothing. */
    memset(&cache, 0, sizeof(cache));
    midi_control_gfx_init(&g, fb, MIDI_CONTROL_GFX_RGB565, 1);
    midi_control_gfx_init(&gf, full, MIDI_CONTROL_GFX_RGB565, 1);
    midi_control_gfx_init(&gb, before, MIDI_CONTROL_GFX_RGB565, 1);
    CHECK(midi_control_ui_update(&g, &m, &cache, &r) == 1 && r.w == LCD_W && r.h == LCD_H);
    CHECK(midi_control_ui_update(&g, &m, &cache, &r) == 0 && r.w == 0);

    /* A turned knob redraws a small rectangle, and nothing outside it changes. */
    memcpy(before, fb, sizeof(fb));
    midi_control_device_encoder(&d, MIDI_CONTROL_ENC_1, 3);
    CHECK(midi_control_ui_update(&g, &m, &cache, &r) == 1);
    CHECK(r.w > 0 && r.w * r.h < LCD_W * LCD_H / 8);
    int inside = 0, outside = 0;
    for (int y = 0; y < LCD_H; y++) {
        for (int x = 0; x < LCD_W; x++) {
            if (midi_control_gfx_read(&g, x, y) == midi_control_gfx_read(&gb, x, y))
                continue;
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
                inside++;
            else
                outside++;
        }
    }
    CHECK(inside > 0 && outside == 0);

    /* Through a run of changes, the picture updates build is the one a full draw makes. */
    for (int step = 0; step < 10; step++) {
        switch (step) {
        case 0: midi_control_device_button(&d, MIDI_CONTROL_BTN_TAP, 1); break;
        case 1: midi_control_device_button(&d, MIDI_CONTROL_BTN_TAP, 0); break;
        case 2: strcpy(d.text[1], "a very long second line of text"); break;
        case 3: midi_control_device_analog(&d, MIDI_CONTROL_ANALOG_VOLUME, 90); break;
        case 4: m.usb_mounted = 0; break;
        case 5: g_storage_state = MIDI_CONTROL_STORAGE_LOCKED; break;
        case 6: midi_control_device_midi_in(&d, (const uint8_t[]){ 0xC0, 3 }, 2); break;
        case 7: m.diag = "SDRAM DATA FAIL 32M"; m.diag_colour = MIDI_CONTROL_RED; break;
        case 8: d.text[0][0] = 0; break;
        default: midi_control_device_encoder(&d, MIDI_CONTROL_ENC_1, -100); break;
        }
        midi_control_ui_update(&g, &m, &cache, &r);
        midi_control_ui_draw(&gf, &m);
        CHECK(memcmp(fb, full, sizeof(fb)) == 0);
    }
    midi_control_ui_view(&v, &m);
    CHECK(strcmp(v.switches[6].label, "CC 64") == 0 && v.switches[6].active
          && v.switches[6].led == MIDI_CONTROL_LAMP_TAP);
    CHECK(strcmp(v.program.label, "1D") == 0 && v.switches[5].active);
    CHECK(strcmp(v.status.warning, "SDRAM DATA FAIL 32M") == 0 && v.status.storage == MIDI_CONTROL_STORAGE_LOCKED);

    /* The display timeout: black, all of it, drawn in one piece. Nothing redraws
     * while dark, whatever the host changes; a control lights it, whole. */
    c = d.config;
    c.global.display_timeout = 1;
    midi_control_device_set_config(&d, &c);
    midi_control_ui_update(&g, &m, &cache, &r);
    midi_control_device_tick(&d, 1000);
    midi_control_ui_view(&v, &m);
    CHECK(v.display_off && !v.running && v.status.title[0] == 0);
    CHECK(midi_control_ui_update(&g, &m, &cache, &r) == 1 && r.w == LCD_W && r.h == LCD_H);
    int lit = 0;
    for (unsigned i = 0; i < sizeof(fb); i++)
        lit |= fb[i];
    CHECK(lit == 0);
    strcpy(d.text[0], "Changed while dark");
    midi_control_device_midi_in(&d, (const uint8_t[]){ 0xC0, 20 }, 2);
    CHECK(midi_control_ui_update(&g, &m, &cache, &r) == 0);
    midi_control_device_encoder(&d, MIDI_CONTROL_ENC_1, 1);
    CHECK(midi_control_ui_update(&g, &m, &cache, &r) == 1 && r.w == LCD_W && r.h == LCD_H);
    midi_control_ui_draw(&gf, &m);
    CHECK(memcmp(fb, full, sizeof(fb)) == 0);
    midi_control_ui_view(&v, &m);
    CHECK(v.running && strcmp(v.program.line1, "Changed while dark") == 0);

    /* Back to the start-up screen is a full redraw. */
    m.device = NULL;
    CHECK(midi_control_ui_update(&g, &m, &cache, &r) == 1 && r.w == LCD_W);

    /* Mono draws both screens inside its buffer. */
    mono[0] = 0x5A;
    mono[MIDI_CONTROL_LCD_MONO_BYTES + 1] = 0xA5;
    midi_control_gfx_init(&gm, mono + 1, MIDI_CONTROL_GFX_MONO, 1);
    midi_control_ui_draw(&gm, &m);
    CHECK(mono[MIDI_CONTROL_LCD_MONO_BYTES] & 0x80);             /* the border at the rotated origin */
    m.device = &d;
    midi_control_ui_draw(&gm, &m);
    CHECK(mono[0] == 0x5A && mono[MIDI_CONTROL_LCD_MONO_BYTES + 1] == 0xA5);
}

/* `test_shared --build-emmc <raw-or-bin> <out>` writes the C builder's eMMC
 * image, so it can be compared byte for byte against tools/midi_control_boot_image.py
 * on a real firmware build. */
static int build_emmc_cli(const char *in_path, const char *out_path)
{
    FILE *f = fopen(in_path, "rb");
    if (!f) {
        printf("cannot open %s\n", in_path);
        return 2;
    }
    static uint8_t raw[256 * 1024], out[256 * 1024 + 2048];
    size_t n = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    const char *err = NULL;
    uint32_t len = midi_control_boot_build_emmc(raw, (uint32_t)n, 0x10000000u, out, sizeof(out), &err);
    if (!len) {
        printf("build failed: %s\n", err ? err : "?");
        return 1;
    }
    f = fopen(out_path, "wb");
    if (!f)
        return 2;
    fwrite(out, 1, len, f);
    fclose(f);
    printf("wrote %u bytes to %s\n", (unsigned)len, out_path);
    return 0;
}

/* `test_shared --factory-emmc <main> <out> [<emmc-dump>]` runs the release
 * extractor on a release's inflated main content - inflating is the app's -
 * and, given a 1,365-sector eMMC dump, the residual comparison, so both can be
 * checked against a real release and a real unit. */
static int factory_emmc_cli(const char *main_path, const char *out_path, const char *dump_path)
{
    static uint8_t content[4u * 1024u * 1024u], out[EMMC_BYTES], dump[EMMC_BYTES];
    FILE *f = fopen(main_path, "rb");
    if (!f) {
        printf("cannot open %s\n", main_path);
        return 2;
    }
    size_t n = fread(content, 1, sizeof(content), f);
    fclose(f);
    const char *err = NULL;
    uint32_t len = midi_control_hxf_factory_emmc(content, (uint32_t)n, out, sizeof(out), &err);
    if (!len) {
        printf("extract failed: %s\n", err ? err : "?");
        return 1;
    }
    f = fopen(out_path, "wb");
    if (!f)
        return 2;
    fwrite(out, 1, len, f);
    fclose(f);
    printf("wrote %u bytes to %s\n", (unsigned)len, out_path);
    if (!dump_path)
        return 0;

    f = fopen(dump_path, "rb");
    if (!f) {
        printf("cannot open %s\n", dump_path);
        return 2;
    }
    n = fread(dump, 1, sizeof(dump), f);
    fclose(f);
    if (n != sizeof(dump)) {
        printf("%s is not %u bytes\n", dump_path, (unsigned)sizeof(dump));
        return 2;
    }
    struct midi_control_residual r;
    midi_control_boot_residual_compare(dump, out, len, &r);
    printf("residual: state %d, identical %d, head identical %d, bytes %u..%u, compared %u, differing %u, first at %u\n",
           r.emmc_state, r.identical, r.head_identical, (unsigned)r.from, (unsigned)r.to, (unsigned)r.compared,
           (unsigned)r.differing, (unsigned)r.first_difference);
    return 0;
}

/* ---- the analog auto-range and its stored calibration -------------------- */
/* The board layer cannot be asked to hold a pedal against its stop, so this is
 * where the scaling is actually exercised. Every case below is one that was
 * seen on a unit or is the direct cause of one that was.
 */

/* A sweep, as the converter sees one: every count from `from` to `to`. */
static void sweep(struct midi_control_analog_learn *l, int from, int to)
{
    int step = from <= to ? 1 : -1;
    for (int raw = from; ; raw += step) {
        midi_control_analog_learn_update(l, (uint16_t)raw);
        if (raw == to)
            break;
    }
}

/* Still, as a control against its stop is. */
static void hold(struct midi_control_analog_learn *l, int raw, int n)
{
    for (int i = 0; i < n; i++)
        midi_control_analog_learn_update(l, (uint16_t)raw);
}

static void test_analog_calibration(void)
{
    struct midi_control_analog_learn l;
    struct midi_control_analog_cal cal, want;
    uint16_t at;

    /* The verified unit's seeds, and its pedal: the sensor voltage falls
     * toe-down, so the raw count is inverted to make toe-down 127. Both stops
     * are reached because the travel is the one the seeds were measured on. */
    midi_control_analog_learn_init(&l, NULL, 280u, 664u, 1);
    CHECK(l.lo == 280u && l.hi == 664u && l.position == -1);
    sweep(&l, 664, 280);
    CHECK(l.position == 127);
    sweep(&l, 280, 664);
    CHECK(l.position == 0);

    /* The fault this exists to fix. A pedal whose travel is narrower than the
     * window - another unit, or the same one after a transient widened it -
     * cannot reach either stop, and a window that can only widen never gets it
     * back. Part of a sweep proves nothing, so nothing is assumed from it. */
    midi_control_analog_learn_init(&l, NULL, 280u, 664u, 0);
    sweep(&l, 480, 300);
    CHECK(l.lo == 280u && l.hi == 664u && !l.swept);
    CHECK(l.position > 0);                       /* the stop is out of reach */

    /* A sweep that covers enough of the window is believed, and the ends it
     * reached become the travel. Now the stops are the stops. */
    sweep(&l, 300, 620);
    CHECK(l.lo == 300u && l.hi == 620u && l.swept);
    CHECK(l.position == 127);
    sweep(&l, 620, 300);
    CHECK(l.position == 0);

    /* One sample thrown out by a transient is not travel: the window only
     * takes an edge MIDI_CONTROL_CAL_CONFIRM samples agree on. */
    midi_control_analog_learn_init(&l, NULL, 280u, 664u, 0);
    midi_control_analog_learn_update(&l, 0u);
    CHECK(l.lo == 280u);
    hold(&l, 500, 4);
    CHECK(l.lo == 280u && l.hi == 664u);

    /* A control actually pushed past the window is, within those samples. */
    hold(&l, 700, MIDI_CONTROL_CAL_CONFIRM);
    CHECK(l.hi == 700u);

    /* Hysteresis holds back the dither of a control at rest... */
    midi_control_analog_learn_init(&l, NULL, 280u, 664u, 0);
    sweep(&l, 280, 664);
    sweep(&l, 664, 500);
    at = l.raw_sent;
    CHECK(midi_control_analog_learn_update(&l, (uint16_t)(at + MIDI_CONTROL_CAL_HYSTERESIS - 1u)) == 0);
    CHECK(midi_control_analog_learn_update(&l, (uint16_t)(at + MIDI_CONTROL_CAL_HYSTERESIS)) == 1);
    /* ...but never the move onto a stop, which is the one position that has to
     * be reachable. */
    CHECK(midi_control_analog_learn_update(&l, 663u) == 1 && l.position == 127);

    /* Nothing is stored until a sweep has confirmed the ends, so a unit that
     * is powered on and not touched never writes a calibration. */
    memset(&cal, 0, sizeof(cal));
    midi_control_analog_learn_init(&l, NULL, 280u, 664u, 0);
    hold(&l, 500, 50);
    CHECK(!midi_control_analog_learn_store(&l, &cal, &want));
    sweep(&l, 500, 280);
    sweep(&l, 280, 664);
    CHECK(midi_control_analog_learn_store(&l, &cal, &want));
    CHECK(want.raw_lo == 280u && want.raw_hi == 664u && (want.flags & MIDI_CONTROL_CAL_VALID));

    /* The same travel again is not worth another write. */
    cal = want;
    CHECK(!midi_control_analog_learn_store(&l, &cal, &want));

    /* A stored calibration beats the seeds at start-up, and a travel that
     * turns out to differ from it - in either direction - replaces it. */
    midi_control_analog_learn_init(&l, &cal, 0u, 1023u, 0);
    CHECK(l.lo == 280u && l.hi == 664u);
    sweep(&l, 480, 300);
    sweep(&l, 300, 620);
    CHECK(l.lo == 300u && l.hi == 620u);
    CHECK(midi_control_analog_learn_store(&l, &cal, &want));
    CHECK(want.raw_lo == 300u && want.raw_hi == 620u);

    /* A travel shorter than MIDI_CONTROL_CAL_SWEEP_NUM/DEN of the seed. The
     * coverage rule alone cannot reach this one - no sweep of a 194-count
     * travel covers three quarters of a 384-count window - so before the
     * return rule the toe stop read 63 of 127 and stayed there however many
     * times the pedal was swept. This is the "all the way forward is half
     * travel" report.
     *
     * Inverted, as the pedal is: the low raw count is the toe stop. */
    midi_control_analog_learn_init(&l, NULL, 280u, 664u, 1);
    sweep(&l, 664, 470);
    hold(&l, 470, 4);
    CHECK(l.lo == 280u && l.hi == 664u && !l.swept);
    CHECK(l.position == 63);                     /* the stop reads half travel */

    /* Coming back to the heel is the first end to repeat, and it alone
     * settles nothing: one end says nothing about where the other is. */
    sweep(&l, 470, 664);
    hold(&l, 664, 4);
    CHECK(l.hit_hi && !l.hit_lo && !l.swept && l.lo == 280u);

    /* Going back to the toe lands on the same count a second time, which a
     * turn made in free travel does not do. Both ends have now repeated, so
     * they are the travel, and the stop is a stop. */
    sweep(&l, 664, 470);
    hold(&l, 470, 4);
    CHECK(l.hit_lo && l.swept && l.lo == 470u && l.hi == 664u);
    CHECK(l.position == 127);
    sweep(&l, 470, 664);
    CHECK(l.position == 0);

    /* And it is this unit's travel, so it is written. */
    memset(&cal, 0, sizeof(cal));
    CHECK(midi_control_analog_learn_store(&l, &cal, &want));
    CHECK(want.raw_lo == 470u && want.raw_hi == 664u);

    /* The return rule answers for a seed, which is a guess, and not for a
     * stored travel, which this unit measured. A calibrated pedal worked over
     * the middle of its travel all session - a wah rocked between the same two
     * places, never touching a stop - keeps the travel it was calibrated with,
     * however many times the rock repeats. */
    memset(&cal, 0, sizeof(cal));
    cal.raw_lo = 280u;
    cal.raw_hi = 664u;
    cal.flags = MIDI_CONTROL_CAL_VALID;
    midi_control_analog_learn_init(&l, &cal, 280u, 664u, 1);
    CHECK(!l.seeded);
    for (int i = 0; i < 4; i++) {
        sweep(&l, 560, 400);
        hold(&l, 400, 4);
        sweep(&l, 400, 560);
        hold(&l, 560, 4);
    }
    CHECK(l.lo == 280u && l.hi == 664u && !l.swept);
    CHECK(!midi_control_analog_learn_store(&l, &cal, &want));

    /* Throwing that calibration away is the user's to ask for, and it puts the
     * pedal back on the seeds with the return rule live again. */
    midi_control_analog_learn_reset(&l);
    CHECK(l.seeded && l.lo == 280u && l.hi == 664u && !l.swept);

    /* A calibration too narrow to scale from is not one: it is dropped rather
     * than left holding the control at a single value. */
    memset(&cal, 0, sizeof(cal));
    cal.raw_lo = 500u;
    cal.raw_hi = (uint16_t)(500u + MIDI_CONTROL_CAL_MIN_SPAN - 1u);
    cal.flags = MIDI_CONTROL_CAL_VALID;
    midi_control_analog_learn_init(&l, &cal, 280u, 664u, 0);
    CHECK(l.lo == 280u && l.hi == 664u);
}

static void test_analog_calibration_device(void)
{
    struct midi_control_platform_ops ops = ops_with_storage();
    struct midi_control_device d, d2;
    struct midi_control_config c;
    struct midi_control_message m;
    uint8_t req[MIDI_CONTROL_SYSEX_MAX];
    int before;

    memset(g_slot, 0, sizeof(g_slot));
    memset(g_slot_written, 0, sizeof(g_slot_written));
    g_storage_state = MIDI_CONTROL_STORAGE_READY;
    g_fail_write = 0;

    midi_control_device_init(&d, &ops);
    midi_control_device_analog_seed(&d, MIDI_CONTROL_ANALOG_PEDAL, 280u, 664u, 1);
    /* Seeded and then left alone, to prove that writing one control's travel
     * does not invent one for the other. */
    midi_control_device_analog_seed(&d, MIDI_CONTROL_ANALOG_VOLUME, 20u, 400u, 0);
    midi_control_device_analog_raw(&d, MIDI_CONTROL_ANALOG_VOLUME, 210u);
    CHECK(!(d.config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].flags & MIDI_CONTROL_CAL_VALID));

    /* A pedal that only travels 300..620, swept stop to stop. */
    for (int raw = 480; raw >= 300; raw--)
        midi_control_device_analog_raw(&d, MIDI_CONTROL_ANALOG_PEDAL, (uint16_t)raw);
    for (int raw = 300; raw <= 620; raw++)
        midi_control_device_analog_raw(&d, MIDI_CONTROL_ANALOG_PEDAL, (uint16_t)raw);
    CHECK(d.cal_dirty);
    CHECK(g_slot_written[0] == 0 && g_slot_written[1] == 0);   /* not mid-sweep */

    /* Written once it has settled, and not a moment before. */
    midi_control_device_tick(&d, MIDI_CONTROL_CAL_SETTLE_MS - 1u);
    CHECK(g_slot_written[0] == 0);
    midi_control_device_tick(&d, 1u);
    CHECK(g_slot_written[0] == 1 && !d.cal_dirty);
    CHECK(d.config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_lo == 300u);
    CHECK(d.config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_hi == 620u);
    CHECK(!(d.config.analog_cal[MIDI_CONTROL_ANALOG_VOLUME].flags & MIDI_CONTROL_CAL_VALID));
    CHECK(d.config.analog_cal[MIDI_CONTROL_ANALOG_VOLUME].raw_hi == 0u);

    /* The next power-on starts from it: both stops, no sweep needed. This is
     * the whole point - what the pedal can reach no longer depends on where it
     * happened to be when the unit came up. */
    midi_control_device_init(&d2, &ops);
    midi_control_device_analog_seed(&d2, MIDI_CONTROL_ANALOG_PEDAL, 280u, 664u, 1);
    CHECK(d2.analog_learn[MIDI_CONTROL_ANALOG_PEDAL].lo == 300u);
    CHECK(d2.analog_learn[MIDI_CONTROL_ANALOG_PEDAL].hi == 620u);
    midi_control_device_analog_raw(&d2, MIDI_CONTROL_ANALOG_PEDAL, 300u);
    CHECK(d2.engine.analog_in[MIDI_CONTROL_ANALOG_PEDAL] == 127);
    midi_control_device_analog_raw(&d2, MIDI_CONTROL_ANALOG_PEDAL, 620u);
    CHECK(d2.engine.analog_in[MIDI_CONTROL_ANALOG_PEDAL] == 0);

    /* A configuration from the app, or from a file written on another unit,
     * replaces the mapping and leaves this unit's pedal alone. */
    midi_control_config_defaults(&c);
    c.global.bank_size = 8;
    midi_control_device_set_config(&d, &c);
    CHECK(d.config.global.bank_size == 8);
    CHECK(d.config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_lo == 300u);

    /* A write nobody asked for never commits edits nobody committed. */
    d.cal_dirty = 1u;
    d.cal_settle_ms = 0u;
    before = g_slot_written[0] + g_slot_written[1];
    midi_control_device_tick(&d, MIDI_CONTROL_CAL_SETTLE_MS);
    CHECK(d.dirty && g_slot_written[0] + g_slot_written[1] == before && d.cal_dirty);

    /* LOAD_DEFAULTS is the one command that forgets the calibration. */
    CHECK(roundtrip(&d, req, midi_control_build_simple(req, MIDI_CONTROL_CMD_LOAD_DEFAULTS), &m));
    CHECK(!(d.config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].flags & MIDI_CONTROL_CAL_VALID));
    CHECK(d.analog_learn[MIDI_CONTROL_ANALOG_PEDAL].lo == 280u);   /* back to the seeds */
    CHECK(!d.cal_dirty);
}

/* ---- calibration by capture ------------------------------------------------ */
/* The numbers are the unit's: its pedal reads 584 at the toe stop and 977 at
 * the heel, and it had stored 249..977 - a window that sweep could never
 * narrow, so the toe read 69 (docs/09). */

/* Resting at `raw` for `ms`, as a platform delivers it: a count, then a tick. */
static void capture_rest(struct midi_control_analog_capture *c, int raw, uint32_t ms, int may_rest)
{
    for (uint32_t t = 0; t < ms; t += 10u) {
        midi_control_analog_capture_sample(c, (uint16_t)raw);
        midi_control_analog_capture_tick(c, 10u, may_rest);
    }
}

/* The counts a move from `from` to `to` goes through, `step` apart and a
 * tick each, arriving at `to` by a whole step - the first is `from` itself. */
static int move_count(int from, int to, int step, int k)
{
    const int n = ((from > to ? from - to : to - from) + step - 1) / step;
    const int raw = from > to ? to + (n - k) * step : to - (n - k) * step;
    if (k == 0)
        return from;
    return raw;
}

static int move_steps(int from, int to, int step)
{
    return ((from > to ? from - to : to - from) + step - 1) / step;
}

/* Moving from one count to another, ten counts a tick, so never at rest. */
static void capture_move(struct midi_control_analog_capture *c, int from, int to)
{
    for (int k = 0; k <= move_steps(from, to, 10); k++) {
        midi_control_analog_capture_sample(c, (uint16_t)move_count(from, to, 10, k));
        midi_control_analog_capture_tick(c, 10u, 1);
    }
}

static void test_analog_capture(void)
{
    struct midi_control_analog_capture c;
    struct midi_control_analog_cal cal;

    /* Nothing is taken until the control has rested for the whole hold. */
    midi_control_analog_capture_start(&c);
    CHECK(c.stage == MIDI_CONTROL_CAL_FIRST && !midi_control_analog_capture_result(&c, &cal));
    capture_rest(&c, 584, MIDI_CONTROL_CAL_HOLD_MS - 10u, 1);
    CHECK(c.stage == MIDI_CONTROL_CAL_FIRST);
    CHECK(midi_control_analog_capture_progress(&c) > 100u && midi_control_analog_capture_progress(&c) < 127u);
    capture_rest(&c, 584, 10u, 1);
    CHECK(c.stage == MIDI_CONTROL_CAL_SECOND && c.first == 584u);

    /* Resting at the same end again is not the other end, however long. */
    capture_rest(&c, 588, 3u * MIDI_CONTROL_CAL_HOLD_MS, 1);
    CHECK(c.stage == MIDI_CONTROL_CAL_SECOND && midi_control_analog_capture_progress(&c) == 0u);

    /* On the way to the other end nothing rests, so nothing is taken. */
    capture_move(&c, 588, 977);
    CHECK(c.stage == MIDI_CONTROL_CAL_SECOND);

    /* Dithering a count either way at the far end is resting there, and the
     * stop is the middle of the dither. */
    for (uint32_t t = 0; t < MIDI_CONTROL_CAL_HOLD_MS; t += 10u) {
        midi_control_analog_capture_sample(&c, (uint16_t)(976u + (t / 10u) % 3u));
        midi_control_analog_capture_tick(&c, 10u, 1);
    }
    CHECK(c.stage == MIDI_CONTROL_CAL_DONE && c.second == 977u);
    CHECK(midi_control_analog_capture_progress(&c) == 127u);
    CHECK(midi_control_analog_capture_result(&c, &cal));
    CHECK(cal.raw_lo == 584u && cal.raw_hi == 977u && cal.flags == MIDI_CONTROL_CAL_VALID);

    /* Arriving slowly, a count a tick, the last counts of the way in are inside
     * the rest already - and still no part of the stop. */
    midi_control_analog_capture_start(&c);
    for (int raw = 600; raw >= 584; raw--) {
        midi_control_analog_capture_sample(&c, (uint16_t)raw);
        midi_control_analog_capture_tick(&c, 10u, 1);
    }
    capture_rest(&c, 584, MIDI_CONTROL_CAL_HOLD_MS, 1);
    CHECK(c.stage == MIDI_CONTROL_CAL_SECOND && c.first == 584u);

    /* Either end first: which is the toe is the platform's invert, not the
     * order they were held in. */
    midi_control_analog_capture_start(&c);
    capture_rest(&c, 977, MIDI_CONTROL_CAL_HOLD_MS, 1);
    capture_move(&c, 977, 584);
    capture_rest(&c, 584, MIDI_CONTROL_CAL_HOLD_MS, 1);
    CHECK(midi_control_analog_capture_result(&c, &cal) && cal.raw_lo == 584u && cal.raw_hi == 977u);

    /* While the control is pressed past where it rests - the toe switch held -
     * nothing is timed, and what it was forced to is no part of the stop. */
    midi_control_analog_capture_start(&c);
    capture_rest(&c, 580, 3u * MIDI_CONTROL_CAL_HOLD_MS, 0);
    CHECK(c.stage == MIDI_CONTROL_CAL_FIRST && midi_control_analog_capture_progress(&c) == 0u);
    capture_rest(&c, 584, MIDI_CONTROL_CAL_HOLD_MS, 1);
    CHECK(c.stage == MIDI_CONTROL_CAL_SECOND && c.first == 584u);

    /* A capture that never rests is abandoned, and has nothing to give. */
    midi_control_analog_capture_start(&c);
    for (uint32_t t = 0; t < MIDI_CONTROL_CAL_IDLE_MS; t += 10u) {
        midi_control_analog_capture_sample(&c, (uint16_t)(300u + (t / 10u) % 2u * 20u));
        midi_control_analog_capture_tick(&c, 10u, 1);
    }
    CHECK(c.stage == MIDI_CONTROL_CAL_FAILED && !midi_control_analog_capture_result(&c, &cal));
    CHECK(midi_control_analog_capture_tick(&c, 10u, 1) == 0 && midi_control_analog_capture_progress(&c) == 0u);
}

/* The pedal at `raw` for `ms`, as the board layer feeds it. */
static void pedal_rest(struct midi_control_device *d, int raw, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10u) {
        midi_control_device_analog_raw(d, MIDI_CONTROL_ANALOG_PEDAL, (uint16_t)raw);
        midi_control_device_tick(d, 10u);
    }
}

/* At `raw` until the calibration leaves `stage`, or two holds have gone by. */
static void pedal_rest_through(struct midi_control_device *d, int raw, int stage)
{
    for (uint32_t t = 0; t < 2u * MIDI_CONTROL_CAL_HOLD_MS && midi_control_device_calibration(d) == stage; t += 10u) {
        midi_control_device_analog_raw(d, MIDI_CONTROL_ANALOG_PEDAL, (uint16_t)raw);
        midi_control_device_tick(d, 10u);
    }
}

/* A sweep, ten counts a tick, arriving by a whole step - more than the
 * learner's hysteresis, so where it arrives is reported. */
static void pedal_move(struct midi_control_device *d, int from, int to)
{
    for (int k = 0; k <= move_steps(from, to, 10); k++) {
        midi_control_device_analog_raw(d, MIDI_CONTROL_ANALOG_PEDAL, (uint16_t)move_count(from, to, 10, k));
        midi_control_device_tick(d, 10u);
    }
}

/* CALIBRATE over the protocol; the RESULT's status, or -1 without one. */
static int calibrate_cmd(struct midi_control_device *d, uint8_t analog, int start)
{
    uint8_t req[MIDI_CONTROL_SYSEX_MAX];
    struct midi_control_message m;
    uint8_t cmd = 0, status = 0;

    if (!roundtrip(d, req, midi_control_build_calibrate(req, analog, start), &m)
        || !midi_control_parse_result(&m, &cmd, &status) || cmd != MIDI_CONTROL_CMD_CALIBRATE)
        return -1;
    return status;
}

/* The unit as it was: the too-wide window stored, the pedal seeded as the
 * firmware seeds it. */
static void unit_with_wide_window(struct midi_control_device *d, const struct midi_control_platform_ops *ops)
{
    memset(g_slot, 0, sizeof(g_slot));
    memset(g_slot_written, 0, sizeof(g_slot_written));
    g_storage_state = MIDI_CONTROL_STORAGE_READY;
    g_fail_write = 0;

    midi_control_device_init(d, ops);
    d->config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_lo = 249u;
    d->config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_hi = 977u;
    d->config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].flags = MIDI_CONTROL_CAL_VALID;
    midi_control_device_save(d);
    midi_control_device_init(d, ops);
    midi_control_device_analog_seed(d, MIDI_CONTROL_ANALOG_PEDAL,
                                    MIDI_CONTROL_SEED_PEDAL_LO, MIDI_CONTROL_SEED_PEDAL_HI, 1);
    memset(g_slot_written, 0, sizeof(g_slot_written));
}

static void test_pedal_calibration(void)
{
    struct midi_control_platform_ops ops = ops_with_storage();
    struct midi_control_device d, d2;
    struct midi_control_config c;
    struct midi_control_ui_view v;
    struct midi_control_ui_model m = { 0 };
    const struct midi_control_analog_cal *cal = &d.config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL];

    unit_with_wide_window(&d, &ops);
    CHECK(d.analog_learn[MIDI_CONTROL_ANALOG_PEDAL].lo == 249u && !d.analog_learn[MIDI_CONTROL_ANALOG_PEDAL].seeded);

    /* The fault: swept stop to stop as often as you like, the toe reads 69. */
    for (int i = 0; i < 3; i++) {
        pedal_move(&d, 977, 584);
        pedal_rest(&d, 584, 200u);
        pedal_move(&d, 584, 977);
        pedal_rest(&d, 977, 200u);
    }
    pedal_move(&d, 977, 584);
    CHECK(d.engine.analog_in[MIDI_CONTROL_ANALOG_PEDAL] == 69);

    /* Only the pedal, and only one fed raw counts, can be calibrated. */
    CHECK(calibrate_cmd(&d, MIDI_CONTROL_ANALOG_VOLUME, 1) == MIDI_CONTROL_STATUS_INVALID_VALUE);
    CHECK(calibrate_cmd(&d, MIDI_CONTROL_ANALOG_COUNT, 1) == MIDI_CONTROL_STATUS_INVALID_VALUE);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_OFF);

    /* Started over the protocol at the toe, where the pedal is resting: that
     * end is timed from the start. */
    CHECK(calibrate_cmd(&d, MIDI_CONTROL_ANALOG_PEDAL, 1) == MIDI_CONTROL_STATUS_OK);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_FIRST);
    sink_reset();
    pedal_rest(&d, 584, MIDI_CONTROL_CAL_HOLD_MS);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_SECOND && d.capture.first == 584u);
    pedal_move(&d, 584, 977);
    pedal_rest_through(&d, 977, MIDI_CONTROL_CAL_SECOND);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_DONE);
    /* Measured, not played: the whole sweep sent nothing. */
    CHECK(g_out_len == 0);

    /* Live at once, and written at once - nothing of the user's was pending -
     * so the next power-on has it. */
    CHECK(cal->raw_lo == 584u && cal->raw_hi == 977u && (cal->flags & MIDI_CONTROL_CAL_VALID));
    CHECK(d.analog_learn[MIDI_CONTROL_ANALOG_PEDAL].lo == 584u && d.analog_learn[MIDI_CONTROL_ANALOG_PEDAL].hi == 977u);
    CHECK(!d.cal_unsaved && !d.dirty && g_slot_written[0] + g_slot_written[1] == 1);
    midi_control_device_init(&d2, &ops);
    CHECK(d2.config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_lo == 584u);
    CHECK(d2.config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_hi == 977u);

    /* The screen says so, then goes back to the performance view. */
    m.device = &d;
    midi_control_ui_view(&v, &m);
    CHECK(v.calibrate.open && strcmp(v.calibrate.step, "OK") == 0 && strcmp(v.calibrate.detail, "584 - 977   SAVED") == 0);

    /* Both stops are the stops. */
    pedal_move(&d, 977, 584);
    CHECK(d.engine.analog_in[MIDI_CONTROL_ANALOG_PEDAL] == 127);
    pedal_move(&d, 584, 977);
    CHECK(d.engine.analog_in[MIDI_CONTROL_ANALOG_PEDAL] == 0);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_DONE);
    pedal_rest(&d, 977, MIDI_CONTROL_CAL_SHOW_MS);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_OFF);
    midi_control_ui_view(&v, &m);
    CHECK(!v.calibrate.open && v.program.label[0] != 0);

    /* With edits of the user's pending, the travel is live but is not written:
     * that would commit their edits for them. Their own save carries both. */
    c = d.config;
    c.analogs[MIDI_CONTROL_ANALOG_PEDAL].out_max = 100;
    midi_control_device_set_config(&d, &c);
    memset(g_slot_written, 0, sizeof(g_slot_written));
    CHECK(calibrate_cmd(&d, MIDI_CONTROL_ANALOG_PEDAL, 1) == MIDI_CONTROL_STATUS_OK);
    pedal_rest_through(&d, 977, MIDI_CONTROL_CAL_FIRST);
    pedal_move(&d, 977, 590);
    pedal_rest_through(&d, 590, MIDI_CONTROL_CAL_SECOND);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_DONE);
    CHECK(cal->raw_lo == 590u && cal->raw_hi == 977u && d.cal_unsaved && d.dirty);
    midi_control_ui_view(&v, &m);
    CHECK(strcmp(v.calibrate.detail, "590 - 977   SAVE TO KEEP") == 0);
    pedal_rest(&d, 590, 2u * MIDI_CONTROL_CAL_SETTLE_MS);
    CHECK(g_slot_written[0] == 0 && g_slot_written[1] == 0);
    CHECK(midi_control_device_save(&d) == MIDI_CONTROL_STATUS_OK && !d.cal_unsaved);
    midi_control_device_init(&d2, &ops);
    CHECK(d2.config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_lo == 590u);
    CHECK(d2.config.analogs[MIDI_CONTROL_ANALOG_PEDAL].out_max == 100);
    /* ...and after it, nothing is left to write. */
    memset(g_slot_written, 0, sizeof(g_slot_written));
    pedal_rest(&d, 590, 2u * MIDI_CONTROL_CAL_SETTLE_MS);
    CHECK(g_slot_written[0] == 0 && g_slot_written[1] == 0 && !d.cal_dirty);

    /* Mode cancels: the calibration is exactly what it was, the pedal plays
     * again, and the same hold does not go on to open the profile picker. */
    pedal_rest(&d, 590, MIDI_CONTROL_CAL_SHOW_MS);
    CHECK(calibrate_cmd(&d, MIDI_CONTROL_ANALOG_PEDAL, 1) == MIDI_CONTROL_STATUS_OK);
    pedal_rest_through(&d, 590, MIDI_CONTROL_CAL_FIRST);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_SECOND);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_MODE, 1);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_FAILED);
    pedal_rest(&d, 590, MIDI_CONTROL_PROFILE_HOLD_MS + 100u);
    CHECK(!midi_control_device_picking_profile(&d));
    midi_control_device_button(&d, MIDI_CONTROL_BTN_MODE, 0);
    CHECK(cal->raw_lo == 590u && cal->raw_hi == 977u && !d.cal_unsaved);
    midi_control_ui_view(&v, &m);
    CHECK(strcmp(v.calibrate.hint, "CANCELLED") == 0);
    pedal_move(&d, 590, 977);
    CHECK(d.engine.analog_in[MIDI_CONTROL_ANALOG_PEDAL] == 0);
    /* Mode again puts the result away at once. */
    midi_control_device_button(&d, MIDI_CONTROL_BTN_MODE, 1);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_MODE, 0);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_OFF);

    /* CALIBRATE with start 0 cancels, and cancelling nothing is no error. */
    CHECK(calibrate_cmd(&d, MIDI_CONTROL_ANALOG_PEDAL, 1) == MIDI_CONTROL_STATUS_OK);
    CHECK(calibrate_cmd(&d, MIDI_CONTROL_ANALOG_PEDAL, 0) == MIDI_CONTROL_STATUS_OK);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_FAILED);
    CHECK(calibrate_cmd(&d, MIDI_CONTROL_ANALOG_PEDAL, 0) == MIDI_CONTROL_STATUS_OK);

    /* RELOAD while one runs: the record it was measuring for is gone, so it is
     * put away, and so is a travel taken but not saved. */
    CHECK(calibrate_cmd(&d, MIDI_CONTROL_ANALOG_PEDAL, 1) == MIDI_CONTROL_STATUS_OK);
    CHECK(midi_control_device_reload(&d) == MIDI_CONTROL_STATUS_OK);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_OFF);
}

static void test_pedal_calibration_toe(void)
{
    struct midi_control_platform_ops ops = ops_with_storage();
    struct midi_control_device d;

    unit_with_wide_window(&d, &ops);
    pedal_rest(&d, 977, 100u);

    /* Toe down, the pedal against its toe stop and pressed a little past it.
     * Held short of the hold it is only the toe switch, pressed and still
     * sounding. */
    pedal_move(&d, 977, 575);
    sink_reset();
    midi_control_device_button(&d, MIDI_CONTROL_BTN_TOE, 1);
    CHECK(d.engine.held[MIDI_CONTROL_BTN_TOE] == 1 && g_out_len >= 3u && g_out[0] == 0x90);
    pedal_rest(&d, 575, MIDI_CONTROL_CAL_ENTER_HOLD_MS - 10u);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_OFF);
    pedal_rest(&d, 575, 10u);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_FIRST);

    /* Held on, nothing is timed: the pedal is pressed past where it rests. */
    pedal_rest(&d, 575, 3u * MIDI_CONTROL_CAL_HOLD_MS);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_FIRST);
    CHECK(midi_control_analog_capture_progress(&d.capture) == 0u);

    /* Let go, the pedal settles on its stop, and that is the end taken. The
     * release reached the engine as the press did. */
    midi_control_device_button(&d, MIDI_CONTROL_BTN_TOE, 0);
    CHECK(d.engine.held[MIDI_CONTROL_BTN_TOE] == 0);
    pedal_rest_through(&d, 584, MIDI_CONTROL_CAL_FIRST);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_SECOND && d.capture.first == 584u);
    pedal_move(&d, 584, 977);
    pedal_rest_through(&d, 977, MIDI_CONTROL_CAL_SECOND);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_DONE);
    CHECK(d.config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_lo == 584u);
    CHECK(d.config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_hi == 977u);

    /* One hold starts one calibration: the toe held on through it and past
     * the end does not start another. */
    pedal_rest(&d, 977, MIDI_CONTROL_CAL_SHOW_MS);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_TOE, 1);
    pedal_rest(&d, 977, MIDI_CONTROL_CAL_ENTER_HOLD_MS);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_FIRST);
    CHECK(calibrate_cmd(&d, MIDI_CONTROL_ANALOG_PEDAL, 0) == MIDI_CONTROL_STATUS_OK);
    pedal_rest(&d, 977, MIDI_CONTROL_CAL_SHOW_MS + MIDI_CONTROL_CAL_ENTER_HOLD_MS);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_OFF);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_TOE, 0);

    /* A pedal the platform feeds as a position - the emulator's knob, an
     * EVENT - has no raw counts to measure, and the hold starts nothing. */
    midi_control_device_init(&d, &ops);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_TOE, 1);
    for (uint32_t t = 0; t < 2u * MIDI_CONTROL_CAL_ENTER_HOLD_MS; t += 10u)
        midi_control_device_tick(&d, 10u);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_OFF);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_TOE, 0);
}

static void test_pedal_calibration_state(void)
{
    static uint8_t fb[MIDI_CONTROL_LCD_RGB565_BYTES], full[MIDI_CONTROL_LCD_RGB565_BYTES];
    struct midi_control_platform_ops ops = ops_with_storage();
    struct midi_control_device d, mirror;
    struct midi_control_state_reply st, back;
    struct midi_control_ui_view dv, mv;
    struct midi_control_ui_model dm = { 0 }, mm = { 0 };
    struct midi_control_ui_cache cache;
    struct midi_control_gfx g, gf;
    struct midi_control_rect r;
    uint8_t msg[MIDI_CONTROL_SYSEX_MAX];
    struct midi_control_message parsed;

    /* Before anything: STATE says the pedal can be calibrated, what it reads,
     * and the window it is scaled from - the unit's too-wide one. */
    unit_with_wide_window(&d, &ops);
    pedal_rest(&d, 584, 100u);
    midi_control_device_state(&d, &st);
    CHECK(st.cal_stage == MIDI_CONTROL_CAL_OFF);
    CHECK(st.pedal_flags == (MIDI_CONTROL_PEDAL_RAW | MIDI_CONTROL_PEDAL_CALIBRATED));
    CHECK(st.pedal_raw == 584u && st.pedal_lo == 249u && st.pedal_hi == 977u);

    /* Half way through holding the second end. */
    CHECK(midi_control_device_calibrate(&d, MIDI_CONTROL_ANALOG_PEDAL, 1) == MIDI_CONTROL_STATUS_OK);
    pedal_rest_through(&d, 584, MIDI_CONTROL_CAL_FIRST);
    pedal_move(&d, 584, 977);
    pedal_rest(&d, 977, MIDI_CONTROL_CAL_HOLD_MS / 2u);
    midi_control_device_state(&d, &st);
    CHECK(st.cal_stage == MIDI_CONTROL_CAL_SECOND && st.cal_analog == MIDI_CONTROL_ANALOG_PEDAL);
    CHECK(st.cal_first == 584u && st.cal_second == 0u && st.cal_raw == 977u);
    CHECK(st.cal_progress > 40u && st.cal_progress < 90u);
    CHECK(st.pedal_raw == 977u && st.pedal_lo == 249u);        /* not used until it is done */

    /* Across the wire unchanged, a raw count as two bytes. */
    CHECK(midi_control_parse_message(msg, midi_control_build_state(msg, &st), &parsed));
    CHECK(midi_control_parse_state(&parsed, &back));
    CHECK(back.cal_stage == st.cal_stage && back.cal_analog == st.cal_analog && back.cal_progress == st.cal_progress);
    CHECK(back.cal_raw == 977u && back.cal_first == 584u && back.cal_second == 0u);
    CHECK(back.pedal_flags == st.pedal_flags && back.pedal_raw == 977u && back.pedal_lo == 249u && back.pedal_hi == 977u);
    st.pedal_raw = 20000u;                                      /* past what two 7-bit bytes hold */
    CHECK(midi_control_parse_message(msg, midi_control_build_state(msg, &st), &parsed));
    CHECK(midi_control_parse_state(&parsed, &back) && back.pedal_raw == MIDI_CONTROL_STATE_RAW_MAX);
    st.pedal_raw = 977u;

    /* A host's own device, mirrored, draws the same screen: the calibration
     * in the program region, the same step and the same point in the hold. */
    midi_control_device_init(&mirror, &ops);
    midi_control_device_set_config(&mirror, &d.config);
    midi_control_device_mirror(&mirror, &st);
    dm.device = &d;
    mm.device = &mirror;
    midi_control_ui_view(&dv, &dm);
    midi_control_ui_view(&mv, &mm);
    CHECK(memcmp(&dv, &mv, sizeof(dv)) == 0);
    CHECK(dv.calibrate.open && dv.program.label[0] == 0 && dv.picker.open == 0);
    CHECK(strcmp(dv.calibrate.step, "2/2") == 0 && strcmp(dv.calibrate.detail, "END 584   NOW 977") == 0);
    CHECK(dv.calibrate.progress == st.cal_progress);

    /* Every point of a hold comes back from the mirror as it went in. */
    for (unsigned p = 0; p <= 127u; p++) {
        st.cal_progress = (uint8_t)p;
        midi_control_device_mirror(&mirror, &st);
        CHECK(midi_control_analog_capture_progress(&mirror.capture) == p);
    }

    /* Drawn by updates, the calibration is the picture a full draw makes, and
     * the hold moving redraws the program region and nothing else. */
    memset(&cache, 0, sizeof(cache));
    midi_control_gfx_init(&g, fb, MIDI_CONTROL_GFX_RGB565, 1);
    midi_control_gfx_init(&gf, full, MIDI_CONTROL_GFX_RGB565, 1);
    CHECK(midi_control_ui_update(&g, &dm, &cache, &r) == 1);
    pedal_rest(&d, 977, 100u);
    CHECK(midi_control_ui_update(&g, &dm, &cache, &r) == 1);
    CHECK(r.y >= 32 && r.y + r.h <= 32 + 118);
    pedal_rest_through(&d, 977, MIDI_CONTROL_CAL_SECOND);
    CHECK(midi_control_device_calibration(&d) == MIDI_CONTROL_CAL_DONE);
    CHECK(midi_control_ui_update(&g, &dm, &cache, &r) == 1);
    midi_control_ui_draw(&gf, &dm);
    CHECK(memcmp(fb, full, sizeof(fb)) == 0);
}

/* Padding at the ends of the travel (docs/05 section 5): a pedal that settles
 * a little off its stop as the foot comes away still sends the stop, so an
 * effect that needs exactly 0 or 127 - a pitch shift - does not move. */
static void test_analog_padding(void)
{
    struct midi_control_platform_ops ops = ops_with_storage();
    struct midi_control_config a, b;
    struct midi_control_engine e;
    struct midi_control_device d;
    uint8_t buf[MIDI_CONTROL_CONFIG_BYTES];
    uint32_t crc;
    int same;

    /* The factory mapping pads the pedal, a percent at the heel and fifteen at
     * the toe, and not the knob. */
    midi_control_config_defaults(&a);
    CHECK(a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_lo == 1 && a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi == 15);
    CHECK(a.analogs[MIDI_CONTROL_ANALOG_VOLUME].pad_lo == 0 && a.analogs[MIDI_CONTROL_ANALOG_VOLUME].pad_hi == 0);
    CHECK(a.profiles[MIDI_CONTROL_PROFILE_COUNT - 1u].analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi == 15);
    midi_control_config_write(&a, buf);
    CHECK(buf[264 + 8 + 6] == 1 && buf[264 + 8 + 7] == 15 && buf[264 + 6] == 0 && buf[264 + 7] == 0);

    /* Two bytes that were reserved, so the version stays as it was and a
     * record from before them - zero there - reads as no padding, whatever
     * the defaults say: a unit that has saved plays as it did. */
    buf[264 + 8 + 6] = 0;
    buf[264 + 8 + 7] = 0;
    crc = midi_control_crc32(buf + 16, MIDI_CONTROL_CONFIG_BYTES - 16u);
    buf[12] = (uint8_t)crc;
    buf[13] = (uint8_t)(crc >> 8);
    buf[14] = (uint8_t)(crc >> 16);
    buf[15] = (uint8_t)(crc >> 24);
    CHECK(midi_control_config_read(&b, buf, sizeof(buf)) == MIDI_CONTROL_STATUS_OK);
    CHECK(b.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_lo == 0 && b.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi == 0);

    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_lo = 5;
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi = 12;
    midi_control_config_write(&a, buf);
    CHECK(buf[4] == MIDI_CONTROL_CONFIG_VERSION && buf[264 + 8 + 6] == 5 && buf[264 + 8 + 7] == 12);
    CHECK(midi_control_config_read(&b, buf, sizeof(buf)) == MIDI_CONTROL_STATUS_OK);
    CHECK(b.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_lo == 5 && b.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi == 12);
    CHECK(midi_control_config_equal(&a, &b));
    b.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi = 11;
    CHECK(!midi_control_config_equal(&a, &b));          /* an edit like any other */

    /* It is part of the mapping, so each profile has its own. */
    a.profile_count = 2;
    a.profiles[1].analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi = 20;
    midi_control_config_write(&a, buf);
    CHECK(midi_control_config_read(&b, buf, sizeof(buf)) == MIDI_CONTROL_STATUS_OK);
    CHECK(b.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi == 12 && b.profiles[1].analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi == 20);
    CHECK(midi_control_config_select_profile(&b, 1));
    CHECK(b.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_lo == 1 && b.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi == 20);

    /* Clamped, not rejected. */
    a.analogs[MIDI_CONTROL_ANALOG_VOLUME].pad_lo = 90;
    a.analogs[MIDI_CONTROL_ANALOG_VOLUME].pad_hi = MIDI_CONTROL_ANALOG_PAD_MAX + 1u;
    midi_control_config_sanitise(&a);
    CHECK(a.analogs[MIDI_CONTROL_ANALOG_VOLUME].pad_lo == MIDI_CONTROL_ANALOG_PAD_MAX);
    CHECK(a.analogs[MIDI_CONTROL_ANALOG_VOLUME].pad_hi == MIDI_CONTROL_ANALOG_PAD_MAX);

    /* In the engine. Without padding every position is its own value. */
    midi_control_config_defaults(&a);
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_lo = 0;
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi = 0;
    midi_control_engine_init(&e, &a, sink, NULL);
    sink_reset();
    for (unsigned p = 0; p <= 127u; p++)
        midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, (uint8_t)p);
    same = g_out_len == 128u * 3u;
    for (unsigned p = 0; same && p <= 127u; p++)
        same = g_out[3u * p + 2u] == p;
    CHECK(same);

    /* Ten percent at each end: 12.7 positions read as the stop, and what is
     * left is stretched back over 0..127. */
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_lo = 10;
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi = 10;
    midi_control_engine_config_changed(&e, &a);
    sink_reset();
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 12);
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 0);
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 13);
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 14);
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 64);
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 113);
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 115);
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 127);
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 118);   /* settling back off the toe */
    CHECK(sink_is((const uint8_t[]){ 0xB0, 11, 0, 0xB0, 11, 2, 0xB0, 11, 64, 0xB0, 11, 125, 0xB0, 11, 127 }, 15));

    /* The case as reported: ten percent at the toe alone, and the pedal
     * settling back to 114 of 127. The padding starts at 114.3, and 114 is
     * stretched to 127 all the same; one position further back is the first
     * that sends less. A display that says what is sent has to show this,
     * not the position, or working padding still reads 114. */
    {
        struct midi_control_analog_map toe = a.analogs[MIDI_CONTROL_ANALOG_PEDAL];
        toe.pad_lo = 0;
        toe.pad_hi = 10;
        CHECK(midi_control_engine_analog_value(&toe, 127) == 127);
        CHECK(midi_control_engine_analog_value(&toe, 115) == 127);
        CHECK(midi_control_engine_analog_value(&toe, 114) == 127);
        CHECK(midi_control_engine_analog_value(&toe, 113) == 126);
        CHECK(midi_control_engine_analog_value(&toe, 0) == 0);
        CHECK(midi_control_engine_analog_value(&toe, 1) == 1);
        /* The engine sends exactly this. */
        CHECK(midi_control_engine_analog_value(&a.analogs[MIDI_CONTROL_ANALOG_PEDAL], 118) == 127);
        CHECK(midi_control_engine_analog_value(&a.analogs[MIDI_CONTROL_ANALOG_PEDAL], 113) == 125);
        CHECK(midi_control_engine_analog_value(&a.analogs[MIDI_CONTROL_ANALOG_PEDAL], 14) == 2);
    }

    /* A full sweep still runs from 0 to 127 in order, skipping a few values on
     * the way because it has less travel to cover them in. */
    sink_reset();
    for (unsigned p = 0; p <= 127u; p++)
        midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, (uint8_t)p);
    same = g_out_len >= 3u && g_out[2] == 0 && g_out[g_out_len - 1u] == 127;
    for (uint32_t i = 5; same && i < g_out_len; i += 3u)
        same = g_out[i] > g_out[i - 3u];
    CHECK(same);

    /* The padding is on the physical travel: inverted, the heel's padding
     * stays at the heel and reads as the highest output. */
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_lo = 10;
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi = 0;
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].invert = 1;
    midi_control_engine_config_changed(&e, &a);
    sink_reset();
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 10);
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 127);
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 120);
    CHECK(sink_is((const uint8_t[]){ 0xB0, 11, 127, 0xB0, 11, 0, 0xB0, 11, 8 }, 9));

    /* Then the output range, and every mode. */
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].invert = 0;
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi = 10;
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].out_min = 20;
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].out_max = 100;
    midi_control_engine_config_changed(&e, &a);
    sink_reset();
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 5);
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 125);
    CHECK(sink_is((const uint8_t[]){ 0xB0, 11, 20, 0xB0, 11, 100 }, 6));
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].mode = MIDI_CONTROL_ANM_PITCH_BEND;
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].out_min = 0;
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].out_max = 127;
    midi_control_engine_config_changed(&e, &a);
    sink_reset();
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 120);
    CHECK(sink_is((const uint8_t[]){ 0xE0, 0x7F, 0x7F }, 3));

    /* A configuration nobody sanitised cannot divide by nothing: the engine
     * applies the limit itself, and the stops are still the stops. */
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].mode = MIDI_CONTROL_ANM_CC;
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_lo = 255;
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi = 255;
    midi_control_engine_config_changed(&e, &a);
    sink_reset();
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 0);
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 64);
    midi_control_engine_analog(&e, MIDI_CONTROL_ANALOG_PEDAL, 127);
    CHECK(sink_is((const uint8_t[]){ 0xB0, 11, 0, 0xB0, 11, 66, 0xB0, 11, 127 }, 9));

    /* On a unit, which is the case this is for: the pedal calibrated
     * 300..620, taken to the toe stop, and settling twelve counts back as the
     * foot comes away - more than the learner's hysteresis, so the position
     * does move. */
    memset(g_slot, 0, sizeof(g_slot));
    memset(g_slot_written, 0, sizeof(g_slot_written));
    g_storage_state = MIDI_CONTROL_STORAGE_READY;
    g_fail_write = 0;
    midi_control_device_init(&d, &ops);
    d.config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_lo = 300u;
    d.config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_hi = 620u;
    d.config.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].flags = MIDI_CONTROL_CAL_VALID;
    midi_control_device_analog_seed(&d, MIDI_CONTROL_ANALOG_PEDAL,
                                    MIDI_CONTROL_SEED_PEDAL_LO, MIDI_CONTROL_SEED_PEDAL_HI, 1);

    /* Without padding the settle is sent. */
    a = d.config;
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_lo = 0;
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi = 0;
    midi_control_device_set_config(&d, &a);
    pedal_move(&d, 620, 300);
    CHECK(d.engine.analog_in[MIDI_CONTROL_ANALOG_PEDAL] == 127);
    sink_reset();
    pedal_rest(&d, 312, 200u);
    CHECK(d.engine.analog_in[MIDI_CONTROL_ANALOG_PEDAL] == 122);
    CHECK(sink_is((const uint8_t[]){ 0xB0, 11, 122 }, 3));

    /* The factory padding takes it in at the toe. Its one percent at the heel
     * is not meant to: the same settle there, 3.75 percent, is sent. */
    midi_control_config_defaults(&b);
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL] = b.analogs[MIDI_CONTROL_ANALOG_PEDAL];
    midi_control_device_set_config(&d, &a);
    pedal_move(&d, 312, 300);
    sink_reset();
    pedal_rest(&d, 312, 200u);
    CHECK(d.engine.analog_in[MIDI_CONTROL_ANALOG_PEDAL] == 122);
    CHECK(g_out_len == 0);
    pedal_move(&d, 312, 620);
    sink_reset();
    pedal_rest(&d, 608, 200u);
    CHECK(d.engine.analog_in[MIDI_CONTROL_ANALOG_PEDAL] == 5);
    CHECK(sink_is((const uint8_t[]){ 0xB0, 11, 4 }, 3));

    /* With five percent at each end it is not sent at either stop. */
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_lo = 5;
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi = 5;
    midi_control_device_set_config(&d, &a);
    pedal_move(&d, 608, 300);
    sink_reset();
    pedal_rest(&d, 312, 200u);
    CHECK(d.engine.analog_in[MIDI_CONTROL_ANALOG_PEDAL] == 122);
    CHECK(g_out_len == 0);
    pedal_move(&d, 312, 620);
    CHECK(d.engine.analog_in[MIDI_CONTROL_ANALOG_PEDAL] == 0);
    sink_reset();
    pedal_rest(&d, 608, 200u);
    CHECK(d.engine.analog_in[MIDI_CONTROL_ANALOG_PEDAL] == 5);
    CHECK(g_out_len == 0);
}

static void test_config_v1(void)
{
    /* A record saved before the calibration block existed. It is still read -
     * rejecting it would throw the user's mapping away to gain a block the
     * unit learns again in one sweep - and the calibration comes back empty. */
    struct midi_control_config a, b;
    uint8_t buf[MIDI_CONTROL_CONFIG_BYTES];
    uint32_t crc;

    midi_control_config_defaults(&a);
    a.global.bank_size = 8;
    a.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_lo = 300u;
    a.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_hi = 620u;
    a.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].flags = MIDI_CONTROL_CAL_VALID;
    midi_control_config_write(&a, buf);

    /* Back to version 1: the old version, the old length, and a CRC over the
     * shorter body. */
    buf[4] = (uint8_t)MIDI_CONTROL_CONFIG_VERSION_V1;
    buf[6] = (uint8_t)(MIDI_CONTROL_CONFIG_BYTES_V1 & 0xFFu);
    buf[7] = (uint8_t)(MIDI_CONTROL_CONFIG_BYTES_V1 >> 8);
    crc = midi_control_crc32(buf + 16, MIDI_CONTROL_CONFIG_BYTES_V1 - 16u);
    buf[12] = (uint8_t)crc;
    buf[13] = (uint8_t)(crc >> 8);
    buf[14] = (uint8_t)(crc >> 16);
    buf[15] = (uint8_t)(crc >> 24);

    CHECK(midi_control_config_read(&b, buf, MIDI_CONTROL_CONFIG_BYTES_V1) == MIDI_CONTROL_STATUS_OK);
    CHECK(b.global.bank_size == 8);
    CHECK(!(b.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].flags & MIDI_CONTROL_CAL_VALID));
    CHECK(b.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_lo == 0u);
    CHECK(b.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].raw_hi == 0u);
}

/* Turn a record into an older one: the old version, the old length and a CRC
 * over the shorter body. What the old firmware saved, byte for byte, for the
 * fields it had. */
static void make_old_record(uint8_t *buf, unsigned version, uint32_t bytes)
{
    uint32_t crc;

    /* The analog padding was reserved in versions 1 and 2, so zero on any
     * real one. */
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT && version < MIDI_CONTROL_CONFIG_VERSION_V3; i++) {
        buf[264u + 8u * i + 6u] = 0;
        buf[264u + 8u * i + 7u] = 0;
    }
    buf[4] = (uint8_t)version;
    buf[5] = 0;
    buf[6] = (uint8_t)(bytes & 0xFFu);
    buf[7] = (uint8_t)(bytes >> 8);
    crc = midi_control_crc32(buf + 16, bytes - 16u);
    buf[12] = (uint8_t)crc;
    buf[13] = (uint8_t)(crc >> 8);
    buf[14] = (uint8_t)(crc >> 16);
    buf[15] = (uint8_t)(crc >> 24);
}

/* ---- profiles ---------------------------------------------------------------- */

static void test_profiles_record(void)
{
    struct midi_control_config a, b, d;
    uint8_t buf[MIDI_CONTROL_CONFIG_BYTES];

    midi_control_config_defaults(&d);

    /* A fresh unit has one profile in use, holding the factory mapping, and
     * seven more waiting under their own names. */
    CHECK(d.profile_count == 1 && d.profile_active == 0);
    CHECK(strcmp(midi_control_config_profile_name(&d, 0), "PROFILE 1") == 0);
    CHECK(strcmp(midi_control_config_profile_name(&d, MIDI_CONTROL_PROFILE_COUNT - 1u), "PROFILE 8") == 0);
    CHECK(memcmp(d.profiles[0].buttons, d.buttons, sizeof(d.buttons)) == 0);
    CHECK(memcmp(d.profiles[3].encoders, d.encoders, sizeof(d.encoders)) == 0);

    /* The record is the size the layout says, and fits the sixteen sectors a
     * slot owns (docs/06 section 1). */
    CHECK(MIDI_CONTROL_CONFIG_BYTES == 4400u);
    CHECK(MIDI_CONTROL_CONFIG_BYTES <= 16u * 512u);
    CHECK(MIDI_CONTROL_CONFIG_BYTES <= MIDI_CONTROL_CONFIG_MAX_BYTES);

    /* Three profiles, each with its own mapping, through a round trip. */
    a = d;
    a.profile_count = 3;
    strcpy(a.profiles[1].name, "LEAD RIG");
    strcpy(a.profiles[2].name, "ACOUSTIC");
    a.profiles[1].buttons[MIDI_CONTROL_BTN_FS_A].action = MIDI_CONTROL_ACT_CC_TOGGLE;
    a.profiles[1].buttons[MIDI_CONTROL_BTN_FS_A].number = 80;
    a.profiles[2].encoders[0].mode = MIDI_CONTROL_ENCM_CC_ABSOLUTE;
    a.profiles[2].analogs[MIDI_CONTROL_ANALOG_PEDAL].number = 4;
    midi_control_config_write(&a, buf);
    CHECK(buf[4] == MIDI_CONTROL_CONFIG_VERSION && buf[6] == (uint8_t)(MIDI_CONTROL_CONFIG_BYTES & 0xFFu));
    CHECK(midi_control_config_read(&b, buf, sizeof(buf)) == MIDI_CONTROL_STATUS_OK);
    CHECK(b.profile_count == 3 && b.profile_active == 0);
    CHECK(strcmp(midi_control_config_profile_name(&b, 1), "LEAD RIG") == 0);
    CHECK(b.profiles[1].buttons[MIDI_CONTROL_BTN_FS_A].action == MIDI_CONTROL_ACT_CC_TOGGLE);
    CHECK(b.profiles[1].buttons[MIDI_CONTROL_BTN_FS_A].number == 80);
    CHECK(b.profiles[2].encoders[0].mode == MIDI_CONTROL_ENCM_CC_ABSOLUTE);
    CHECK(b.profiles[2].analogs[MIDI_CONTROL_ANALOG_PEDAL].number == 4);
    CHECK(midi_control_config_equal(&a, &b));

    /* A renamed or re-mapped profile is an edit like any other. */
    strcpy(b.profiles[2].name, "12 STRING");
    CHECK(!midi_control_config_equal(&a, &b));
    b = a;
    b.profiles[2].buttons[MIDI_CONTROL_BTN_TAP].number = 9;
    CHECK(!midi_control_config_equal(&a, &b));
    b = a;
    b.profile_count = 2;
    CHECK(!midi_control_config_equal(&a, &b));

    /* Selecting one makes it the live mapping; the one being left keeps what
     * was done to it. */
    a.buttons[MIDI_CONTROL_BTN_FS_B].number = 100;      /* an edit to profile 1 */
    CHECK(midi_control_config_select_profile(&a, 1));
    CHECK(a.profile_active == 1);
    CHECK(a.buttons[MIDI_CONTROL_BTN_FS_A].action == MIDI_CONTROL_ACT_CC_TOGGLE);
    CHECK(a.profiles[0].buttons[MIDI_CONTROL_BTN_FS_B].number == 100);
    CHECK(midi_control_config_select_profile(&a, 0));
    CHECK(a.buttons[MIDI_CONTROL_BTN_FS_B].number == 100);

    /* A profile that is not in use cannot be selected. */
    CHECK(!midi_control_config_select_profile(&a, 3));
    CHECK(!midi_control_config_select_profile(&a, 99));
    CHECK(a.profile_active == 0);

    /* The live mapping is the one that counts: an edit to it is an edit to the
     * active profile, whether or not the caller sanitises first. */
    CHECK(midi_control_config_select_profile(&a, 2));
    a.buttons[MIDI_CONTROL_BTN_FS_C].number = 55;
    midi_control_config_write(&a, buf);
    CHECK(midi_control_config_read(&b, buf, sizeof(buf)) == MIDI_CONTROL_STATUS_OK);
    CHECK(b.profiles[2].buttons[MIDI_CONTROL_BTN_FS_C].number == 55);
    midi_control_config_sanitise(&a);
    CHECK(a.profiles[2].buttons[MIDI_CONTROL_BTN_FS_C].number == 55);

    /* Out of range is clamped, not rejected, and a nameless profile is named. */
    a.profile_count = 99;
    a.profile_active = 50;
    memset(a.profiles[4].name, 0, sizeof(a.profiles[4].name));
    midi_control_config_sanitise(&a);
    CHECK(a.profile_count == MIDI_CONTROL_PROFILE_COUNT && a.profile_active == 0);
    CHECK(strcmp(midi_control_config_profile_name(&a, 4), "PROFILE 5") == 0);
    a.profile_count = 0;
    midi_control_config_sanitise(&a);
    CHECK(a.profile_count == 1);
}

static void test_profiles_upgrade(void)
{
    struct midi_control_config a, b;
    uint8_t buf[MIDI_CONTROL_CONFIG_BYTES];

    /* A record saved before profiles existed, with a mapping its owner had
     * changed: the mapping becomes the one profile in use, and its name says
     * where it came from. */
    midi_control_config_defaults(&a);
    a.buttons[MIDI_CONTROL_BTN_FS_A].action = MIDI_CONTROL_ACT_CC_TOGGLE;
    a.buttons[MIDI_CONTROL_BTN_FS_A].number = 80;
    a.global.bank_size = 8;
    midi_control_config_write(&a, buf);
    make_old_record(buf, MIDI_CONTROL_CONFIG_VERSION_V2, MIDI_CONTROL_CONFIG_BYTES_V2);

    CHECK(midi_control_config_read(&b, buf, MIDI_CONTROL_CONFIG_BYTES_V2) == MIDI_CONTROL_STATUS_OK);
    CHECK(b.profile_count == 1 && b.profile_active == 0);
    CHECK(strcmp(midi_control_config_profile_name(&b, 0), "IMPORTED") == 0);
    CHECK(b.buttons[MIDI_CONTROL_BTN_FS_A].action == MIDI_CONTROL_ACT_CC_TOGGLE);
    CHECK(b.profiles[0].buttons[MIDI_CONTROL_BTN_FS_A].action == MIDI_CONTROL_ACT_CC_TOGGLE);
    CHECK(b.profiles[0].buttons[MIDI_CONTROL_BTN_FS_A].number == 80);
    CHECK(b.global.bank_size == 8);                    /* the global block is kept as it was */
    /* The seven not in use are the factory mapping under their own names. */
    CHECK(strcmp(midi_control_config_profile_name(&b, 1), "PROFILE 2") == 0);
    CHECK(b.profiles[1].buttons[MIDI_CONTROL_BTN_FS_A].action == MIDI_CONTROL_ACT_PRESET_IN_BANK);
    CHECK(b.profiles[1].buttons[MIDI_CONTROL_BTN_FS_A].number == 0);

    /* The next save writes the current version, with the profile block. */
    midi_control_config_write(&b, buf);
    CHECK(buf[4] == MIDI_CONTROL_CONFIG_VERSION);
    CHECK(midi_control_config_read(&a, buf, sizeof(buf)) == MIDI_CONTROL_STATUS_OK);
    CHECK(strcmp(midi_control_config_profile_name(&a, 0), "IMPORTED") == 0);

    /* A record holding nothing but the factory mapping was never set up, so
     * there is nothing to say it was imported. The factory mapping of the
     * time, that is: A-D sent notes 60-63 then. */
    midi_control_config_defaults(&a);
    for (unsigned i = 0; i < 4u; i++) {
        a.buttons[MIDI_CONTROL_BTN_FS_A + i].action = MIDI_CONTROL_ACT_NOTE_MOMENTARY;
        a.buttons[MIDI_CONTROL_BTN_FS_A + i].number = (uint8_t)(60u + i);
    }
    a.global.display_timeout = 300;                    /* a global setting is not a mapping */
    midi_control_config_write(&a, buf);
    make_old_record(buf, MIDI_CONTROL_CONFIG_VERSION_V2, MIDI_CONTROL_CONFIG_BYTES_V2);
    CHECK(midi_control_config_read(&b, buf, MIDI_CONTROL_CONFIG_BYTES_V2) == MIDI_CONTROL_STATUS_OK);
    CHECK(b.profile_count == 1);
    CHECK(strcmp(midi_control_config_profile_name(&b, 0), "PROFILE 1") == 0);
    CHECK(b.global.display_timeout == 300);
    /* It keeps its notes, so the unit plays as it did; the profiles waiting
     * to be edited choose presets. */
    CHECK(b.buttons[MIDI_CONTROL_BTN_FS_D].action == MIDI_CONTROL_ACT_NOTE_MOMENTARY);
    CHECK(b.buttons[MIDI_CONTROL_BTN_FS_D].number == 63);
    CHECK(b.profiles[0].buttons[MIDI_CONTROL_BTN_FS_D].number == 63);
    CHECK(b.profiles[1].buttons[MIDI_CONTROL_BTN_FS_D].action == MIDI_CONTROL_ACT_PRESET_IN_BANK);
    CHECK(b.profiles[1].buttons[MIDI_CONTROL_BTN_FS_D].number == 3);
    /* That is so although the factory mapping now has padding: the record
     * could not carry any, so its pedal keeps none and plays as it did. The
     * profiles waiting to be edited are the factory's, padding and all. */
    CHECK(b.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_lo == 0 && b.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi == 0);
    CHECK(b.profiles[0].analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi == 0);
    CHECK(b.profiles[1].analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_lo == 1);
    CHECK(b.profiles[1].analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi == 15);

    /* A version 1 record upgrades the same way, and still arrives with no
     * calibration (docs/05 section 6). */
    midi_control_config_defaults(&a);
    a.buttons[MIDI_CONTROL_BTN_TAP].number = 100;
    midi_control_config_write(&a, buf);
    make_old_record(buf, MIDI_CONTROL_CONFIG_VERSION_V1, MIDI_CONTROL_CONFIG_BYTES_V1);
    CHECK(midi_control_config_read(&b, buf, MIDI_CONTROL_CONFIG_BYTES_V1) == MIDI_CONTROL_STATUS_OK);
    CHECK(strcmp(midi_control_config_profile_name(&b, 0), "IMPORTED") == 0);
    CHECK(b.profiles[0].buttons[MIDI_CONTROL_BTN_TAP].number == 100);
    CHECK(!(b.analog_cal[MIDI_CONTROL_ANALOG_PEDAL].flags & MIDI_CONTROL_CAL_VALID));

    /* A unit that has saved since the upgrade keeps its profiles across a
     * power cycle: the record on the card is the whole of them. */
    struct midi_control_platform_ops ops = ops_with_storage();
    struct midi_control_device dev;
    memset(g_slot, 0, sizeof(g_slot));
    g_storage_state = MIDI_CONTROL_STORAGE_READY;
    midi_control_device_init(&dev, &ops);
    dev.config.profile_count = 4;
    strcpy(dev.config.profiles[2].name, "ACOUSTIC");
    dev.config.profiles[2].buttons[MIDI_CONTROL_BTN_FS_D].number = 44;
    CHECK(midi_control_device_save(&dev) == MIDI_CONTROL_STATUS_OK);
    midi_control_device_init(&dev, &ops);
    CHECK(dev.loaded_from_storage && dev.config.profile_count == 4);
    CHECK(strcmp(midi_control_config_profile_name(&dev.config, 2), "ACOUSTIC") == 0);
    CHECK(dev.config.profiles[2].buttons[MIDI_CONTROL_BTN_FS_D].number == 44);
}

/* A device with three profiles: A-D send different notes in each, so what the
 * panel sends says which profile is live. */
static void three_profiles(struct midi_control_device *d, struct midi_control_platform_ops *ops)
{
    struct midi_control_config c;

    memset(g_slot, 0, sizeof(g_slot));
    g_storage_state = MIDI_CONTROL_STORAGE_READY;
    *ops = ops_with_storage();
    midi_control_device_init(d, ops);

    c = d->config;
    c.profile_count = 3;
    for (unsigned p = 0; p < 3u; p++) {
        for (unsigned b = MIDI_CONTROL_BTN_FS_A; b <= MIDI_CONTROL_BTN_FS_D; b++) {
            c.profiles[p].buttons[b].action = MIDI_CONTROL_ACT_NOTE_MOMENTARY;
            c.profiles[p].buttons[b].number = (uint8_t)(10u * (p + 1u) + b);
            c.profiles[p].buttons[b].value_on = 127;
        }
        c.profiles[p].buttons[MIDI_CONTROL_BTN_MODE].action = MIDI_CONTROL_ACT_NOTE_MOMENTARY;
        c.profiles[p].buttons[MIDI_CONTROL_BTN_MODE].number = (uint8_t)(90u + p);
        c.profiles[p].buttons[MIDI_CONTROL_BTN_MODE].value_on = 127;
    }
    strcpy(c.profiles[0].name, "CLEAN");
    strcpy(c.profiles[1].name, "LEAD RIG");
    strcpy(c.profiles[2].name, "ACOUSTIC");

    /* The live mapping is profile 1's, which is what makes it the active one.
     * Copied rather than selected: selecting keeps what was done to the
     * profile being left, and here that is the factory mapping this record
     * started from - it would land on top of the one just built. */
    c.profile_active = 0;
    memcpy(c.buttons, c.profiles[0].buttons, sizeof(c.buttons));
    memcpy(c.encoders, c.profiles[0].encoders, sizeof(c.encoders));
    memcpy(c.analogs, c.profiles[0].analogs, sizeof(c.analogs));

    midi_control_device_set_config(d, &c);
    d->dirty = 0;
}

/* Hold Mode for `ms`, in the steps a platform's clock arrives in. */
static void hold_mode(struct midi_control_device *d, uint32_t ms)
{
    midi_control_device_button(d, MIDI_CONTROL_BTN_MODE, 1);
    for (uint32_t t = 0; t < ms; t += 50u)
        midi_control_device_tick(d, 50u);
}

static void test_profile_picker(void)
{
    struct midi_control_platform_ops ops;
    struct midi_control_device d;
    uint8_t led[MIDI_CONTROL_COLUMNS];

    three_profiles(&d, &ops);

    /* A short press of Mode is just Mode: it sends what it is mapped to and
     * opens nothing. */
    sink_reset();
    midi_control_device_button(&d, MIDI_CONTROL_BTN_MODE, 1);
    midi_control_device_tick(&d, 100u);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_MODE, 0);
    CHECK(sink_is((const uint8_t[]){ 0x90, 90, 127, 0x80, 90, 0 }, 6));
    CHECK(!midi_control_device_picking_profile(&d));

    /* Held, it opens the picker - and still sends its note, because the hold
     * is a gesture on top of the mapping rather than instead of it. */
    sink_reset();
    hold_mode(&d, MIDI_CONTROL_PROFILE_HOLD_MS);
    CHECK(midi_control_device_picking_profile(&d));
    CHECK(sink_is((const uint8_t[]){ 0x90, 90, 127 }, 3));
    CHECK(d.profile_page == 0 && midi_control_device_profile_pages(&d) == 1u);

    /* While it is open, A-D are the picker's: they send nothing, and the one
     * they choose becomes the live mapping. */
    sink_reset();
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_B, 1);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_B, 0);
    CHECK(!midi_control_device_picking_profile(&d));
    CHECK(d.config.profile_active == 1);
    CHECK(strcmp(midi_control_config_profile_name(&d.config, d.config.profile_active), "LEAD RIG") == 0);
    CHECK(d.dirty == 1);                       /* which profile is live is part of the record */

    /* Mode was still down when the profile changed, so its release was sent
     * under the mapping it was pressed in - profile 1's note 90, not profile
     * 2's note 91 - and the release that follows sends nothing more. */
    CHECK(sink_is((const uint8_t[]){ 0x80, 90, 0 }, 3));
    sink_reset();
    midi_control_device_button(&d, MIDI_CONTROL_BTN_MODE, 0);
    CHECK(g_out_len == 0);

    /* The new mapping is what the panel sends now. */
    sink_reset();
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_A, 1);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_A, 0);
    CHECK(sink_is((const uint8_t[]){ 0x90, 20, 127, 0x80, 20, 0 }, 6));

    /* A switch already down when the picker opens keeps its note: the press
     * went to the engine, so the release has to as well. */
    sink_reset();
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_C, 1);
    CHECK(sink_is((const uint8_t[]){ 0x90, 22, 127 }, 3));
    hold_mode(&d, MIDI_CONTROL_PROFILE_HOLD_MS);
    CHECK(midi_control_device_picking_profile(&d));
    sink_reset();
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_C, 0);
    CHECK(sink_is((const uint8_t[]){ 0x80, 22, 0 }, 3));
    CHECK(midi_control_device_picking_profile(&d));      /* a release does not choose */

    /* Mode pressed again closes it, changing nothing. */
    midi_control_device_button(&d, MIDI_CONTROL_BTN_MODE, 0);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_MODE, 1);
    CHECK(!midi_control_device_picking_profile(&d));
    CHECK(d.config.profile_active == 1);
    /* and holding it from there does not reopen it on the same press. */
    for (uint32_t t = 0; t < 2u * MIDI_CONTROL_PROFILE_HOLD_MS; t += 50u)
        midi_control_device_tick(&d, 50u);
    CHECK(!midi_control_device_picking_profile(&d));
    midi_control_device_button(&d, MIDI_CONTROL_BTN_MODE, 0);

    /* Mode still held keeps it open however long the hold goes on: a finger on
     * the switch is not "nothing pressed". */
    hold_mode(&d, MIDI_CONTROL_PROFILE_HOLD_MS);
    for (uint32_t t = 0; t < 4u * MIDI_CONTROL_PROFILE_IDLE_MS; t += 100u)
        midi_control_device_tick(&d, 100u);
    CHECK(midi_control_device_picking_profile(&d));

    /* Nothing pressed for long enough closes it, so a unit is never left
     * with its footswitches answering a menu - and the count starts when Mode
     * is let go, so a long hold does not eat into the time to choose. */
    midi_control_device_button(&d, MIDI_CONTROL_BTN_MODE, 0);
    CHECK(midi_control_device_picking_profile(&d));
    midi_control_device_tick(&d, MIDI_CONTROL_PROFILE_IDLE_MS - 50u);
    CHECK(midi_control_device_picking_profile(&d));
    midi_control_device_tick(&d, 100u);
    CHECK(!midi_control_device_picking_profile(&d));
    CHECK(d.config.profile_active == 1);

    /* The LEDs while it is open: the live profile white, the others blue, a
     * place with no profile dark, and no paging with one page of profiles. */
    hold_mode(&d, MIDI_CONTROL_PROFILE_HOLD_MS);
    midi_control_device_leds(&d, led);
    CHECK(led[midi_control_button_led_column(MIDI_CONTROL_BTN_FS_A)] == MIDI_CONTROL_LED_BLUE);
    CHECK(led[midi_control_button_led_column(MIDI_CONTROL_BTN_FS_B)] == MIDI_CONTROL_LED_WHITE);
    CHECK(led[midi_control_button_led_column(MIDI_CONTROL_BTN_FS_C)] == MIDI_CONTROL_LED_BLUE);
    CHECK(led[midi_control_button_led_column(MIDI_CONTROL_BTN_FS_D)] == MIDI_CONTROL_LED_OFF);
    CHECK(led[midi_control_button_led_column(MIDI_CONTROL_BTN_BANK_UP)] == MIDI_CONTROL_LED_OFF);

    /* A place on the page with no profile chooses nothing and leaves the
     * picker open, rather than closing on a press that did nothing. */
    sink_reset();
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_D, 1);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_D, 0);
    CHECK(midi_control_device_picking_profile(&d) && d.config.profile_active == 1);
    CHECK(g_out_len == 0);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_MODE, 0);
    midi_control_device_show_profiles(&d, 0);
}

static void test_profile_pages(void)
{
    struct midi_control_platform_ops ops;
    struct midi_control_device d;
    uint8_t led[MIDI_CONTROL_COLUMNS];

    three_profiles(&d, &ops);
    d.config.profile_count = 6;                 /* two pages: A-D, then E-F */
    strcpy(d.config.profiles[4].name, "FIFTH");

    hold_mode(&d, MIDI_CONTROL_PROFILE_HOLD_MS);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_MODE, 0);
    CHECK(midi_control_device_profile_pages(&d) == 2u && d.profile_page == 0);
    CHECK(midi_control_device_picker_profile(&d, MIDI_CONTROL_BTN_FS_C) == 2);

    /* Bank Up and Bank Down page through, sending nothing and wrapping. */
    sink_reset();
    midi_control_device_button(&d, MIDI_CONTROL_BTN_BANK_UP, 1);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_BANK_UP, 0);
    CHECK(d.profile_page == 1 && g_out_len == 0);
    CHECK(midi_control_device_picker_profile(&d, MIDI_CONTROL_BTN_FS_A) == 4);
    CHECK(midi_control_device_picker_profile(&d, MIDI_CONTROL_BTN_FS_C) == -1);  /* past the last in use */
    midi_control_device_leds(&d, led);
    CHECK(led[midi_control_button_led_column(MIDI_CONTROL_BTN_BANK_UP)] == MIDI_CONTROL_LED_GREEN);
    CHECK(led[midi_control_button_led_column(MIDI_CONTROL_BTN_FS_C)] == MIDI_CONTROL_LED_OFF);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_BANK_UP, 1);
    CHECK(d.profile_page == 0);                 /* wraps */
    midi_control_device_button(&d, MIDI_CONTROL_BTN_BANK_UP, 0);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_BANK_DOWN, 1);
    CHECK(d.profile_page == 1);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_BANK_DOWN, 0);

    /* The fifth profile is chosen with A on the second page. */
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_A, 1);
    CHECK(!midi_control_device_picking_profile(&d) && d.config.profile_active == 4);
    CHECK(strcmp(midi_control_config_profile_name(&d.config, 4), "FIFTH") == 0);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_A, 0);

    /* Opening it again starts on the page the live profile is on. */
    hold_mode(&d, MIDI_CONTROL_PROFILE_HOLD_MS);
    CHECK(d.profile_page == 1);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_MODE, 0);

    /* A configuration replaced under it closes it: the profiles it was
     * showing are not the ones there now. */
    struct midi_control_config c = d.config;
    c.profile_count = 2;
    midi_control_device_set_config(&d, &c);
    CHECK(!midi_control_device_picking_profile(&d) && d.profile_page == 0);
    /* and the active profile was clamped back into what is in use. */
    CHECK(d.config.profile_active == 0);
}

static void test_profile_state(void)
{
    struct midi_control_platform_ops ops;
    struct midi_control_device d, mirror;
    struct midi_control_state_reply st;
    struct midi_control_ui_view dv, mv;
    struct midi_control_ui_model dm = { 0 }, mm = { 0 };
    uint8_t msg[MIDI_CONTROL_SYSEX_MAX];
    struct midi_control_message parsed;
    uint8_t dled[MIDI_CONTROL_COLUMNS], mled[MIDI_CONTROL_COLUMNS];

    three_profiles(&d, &ops);
    d.config.profile_count = 6;
    strcpy(d.config.profiles[4].name, "FIFTH");

    /* A host asks for the state with the picker open on the second page. */
    hold_mode(&d, MIDI_CONTROL_PROFILE_HOLD_MS);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_MODE, 0);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_BANK_UP, 1);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_BANK_UP, 0);
    midi_control_device_state(&d, &st);
    CHECK(st.profile_active == 0 && st.profile_picking == 1 && st.profile_page == 1);
    CHECK(midi_control_parse_message(msg, midi_control_build_state(msg, &st), &parsed));
    memset(&st, 0, sizeof(st));
    CHECK(midi_control_parse_state(&parsed, &st));
    CHECK(st.profile_active == 0 && st.profile_picking == 1 && st.profile_page == 1);

    /* A device of the host's own, with the same configuration, draws the same
     * screen and lights the same LEDs - the picker included. */
    midi_control_device_init(&mirror, &ops);
    midi_control_device_set_config(&mirror, &d.config);
    midi_control_device_mirror(&mirror, &st);
    dm.device = &d;
    mm.device = &mirror;
    midi_control_ui_view(&dv, &dm);
    midi_control_ui_view(&mv, &mm);
    CHECK(memcmp(&dv, &mv, sizeof(dv)) == 0);
    midi_control_device_leds(&d, dled);
    midi_control_device_leds(&mirror, mled);
    CHECK(memcmp(dled, mled, MIDI_CONTROL_COLUMNS) == 0);

    /* The picker is on the screen where the program is otherwise, and the
     * switches say which profile each one chooses. */
    CHECK(dv.picker.open == 1 && dv.program.label[0] == 0);
    CHECK(strcmp(dv.picker.page, "2/2") == 0);
    CHECK(strcmp(dv.picker.title, "CLEAN") == 0);
    CHECK(strcmp(dv.switches[2].label, "FIFTH") == 0 && dv.switches[2].mapped == 1);
    CHECK(dv.switches[4].label[0] == 0 && dv.switches[4].mapped == 0);   /* nothing at that place */
    CHECK(dv.status.profile[0] != 0);                                    /* more than one profile */

    /* Closed again, the program is back and nothing of the picker is left. */
    midi_control_device_show_profiles(&d, 0);
    midi_control_ui_view(&dv, &dm);
    CHECK(dv.picker.open == 0 && dv.program.label[0] != 0);

    /* With one profile in use the status line says nothing about profiles: a
     * unit set up before they existed looks as it always did. */
    d.config.profile_count = 1;
    midi_control_ui_view(&dv, &dm);
    CHECK(dv.status.profile[0] == 0);
}

/* ---- program names ------------------------------------------------------------ */

static void test_program_names(void)
{
    static struct midi_control_device d;
    struct midi_control_platform_ops ops = ops_with_storage();
    struct midi_control_config a, b, c;
    uint8_t buf[MIDI_CONTROL_CONFIG_BYTES], req[MIDI_CONTROL_SYSEX_MAX];
    struct midi_control_message m;
    struct midi_control_ui_model um;
    struct midi_control_ui_view v;
    uint8_t cmd, status;
    char label[8];

    /* No program has a name until it is given one, and a program past the
     * end never has one. */
    midi_control_config_defaults(&a);
    for (unsigned p = 0; p < MIDI_CONTROL_PROGRAM_COUNT; p++)
        CHECK(midi_control_config_program_name(&a, p)[0] == 0);
    CHECK(strcmp(midi_control_config_program_name(&a, MIDI_CONTROL_PROGRAM_COUNT), "") == 0);

    /* 32D, in banks of four, is program 127. */
    midi_control_ui_program_label(127, 4, label);
    CHECK(strcmp(label, "32D") == 0);
    CHECK(midi_control_config_set_program_name(&a, 127, "Fuzz Lead"));
    CHECK(midi_control_config_set_program_name(&a, 0, "Clean"));
    CHECK(!midi_control_config_set_program_name(&a, MIDI_CONTROL_PROGRAM_COUNT, "Nowhere"));
    CHECK(strcmp(midi_control_config_program_name(&a, 127), "Fuzz Lead") == 0);

    /* Fifteen characters and the NUL: a longer name is cut. NULL or "" takes
     * a name away. */
    CHECK(midi_control_config_set_program_name(&a, 5, "A NAME THAT IS TOO LONG"));
    CHECK(strcmp(midi_control_config_program_name(&a, 5), "A NAME THAT IS ") == 0);
    CHECK(midi_control_config_set_program_name(&a, 5, NULL));
    CHECK(midi_control_config_program_name(&a, 5)[0] == 0);
    CHECK(midi_control_config_set_program_name(&a, 6, "Gone"));
    CHECK(midi_control_config_set_program_name(&a, 6, ""));
    CHECK(midi_control_config_program_name(&a, 6)[0] == 0);

    /* Through a record: version 4, and the names after the profiles, sixteen
     * bytes apiece in program order (docs/05 section 8). */
    midi_control_config_write(&a, buf);
    CHECK(buf[4] == 4u && buf[5] == 0u && buf[6] == (uint8_t)(4400u & 0xFFu) && buf[7] == (uint8_t)(4400u >> 8));
    CHECK(memcmp(buf + 2352u, "Clean\0\0\0\0\0\0\0\0\0\0\0", 16) == 0);
    CHECK(memcmp(buf + 2352u + 127u * 16u, "Fuzz Lead\0\0\0\0\0\0\0", 16) == 0);
    CHECK(midi_control_config_read(&b, buf, sizeof(buf)) == MIDI_CONTROL_STATUS_OK);
    CHECK(strcmp(midi_control_config_program_name(&b, 127), "Fuzz Lead") == 0);
    CHECK(strcmp(midi_control_config_program_name(&b, 0), "Clean") == 0);
    CHECK(midi_control_config_program_name(&b, 1)[0] == 0);
    CHECK(midi_control_config_equal(&a, &b));

    /* A name given, changed or taken away is an edit. */
    midi_control_config_set_program_name(&b, 127, "Fuzz Rhythm");
    CHECK(!midi_control_config_equal(&a, &b));
    midi_control_config_set_program_name(&b, 127, NULL);
    CHECK(!midi_control_config_equal(&a, &b));
    b = a;
    midi_control_config_set_program_name(&b, 64, "New");
    CHECK(!midi_control_config_equal(&a, &b));

    /* Sanitise keeps a name printable and terminated and an empty one empty,
     * and clears what follows the NUL, so a name compares by what it says. */
    b = a;
    memcpy(b.program_names[127], "Fuzz Lead\0junk", 15);
    CHECK(!midi_control_config_equal(&a, &b));
    midi_control_config_sanitise(&b);
    CHECK(midi_control_config_equal(&a, &b));
    memcpy(b.program_names[10], "Tab\tHere", 9);
    memset(b.program_names[11], 'X', MIDI_CONTROL_PROGRAM_NAME_LEN);
    midi_control_config_sanitise(&b);
    CHECK(strcmp(b.program_names[10], "Tab?Here") == 0);
    CHECK(strlen(b.program_names[11]) == MIDI_CONTROL_PROGRAM_NAME_LEN - 1u);
    CHECK(b.program_names[12][0] == 0);

    /* A version 3 record has no names: every program comes back unnamed, and
     * the rest of what it carries - profiles, padding - as it was. The firmware
     * reads a whole slot, so what follows the old record in it is read too,
     * and ignored: here, the names of the record it was made from. */
    a.profile_count = 2;
    strcpy(a.profiles[1].name, "LEAD RIG");
    a.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi = 12;
    midi_control_config_write(&a, buf);
    make_old_record(buf, MIDI_CONTROL_CONFIG_VERSION_V3, MIDI_CONTROL_CONFIG_BYTES_V3);
    CHECK(midi_control_config_read(&b, buf, MIDI_CONTROL_CONFIG_BYTES_V3) == MIDI_CONTROL_STATUS_OK);
    CHECK(midi_control_config_read(&b, buf, sizeof(buf)) == MIDI_CONTROL_STATUS_OK);
    for (unsigned p = 0; p < MIDI_CONTROL_PROGRAM_COUNT; p++)
        CHECK(midi_control_config_program_name(&b, p)[0] == 0);
    CHECK(b.profile_count == 2 && strcmp(midi_control_config_profile_name(&b, 1), "LEAD RIG") == 0);
    CHECK(b.analogs[MIDI_CONTROL_ANALOG_PEDAL].pad_hi == 12);
    CHECK(midi_control_config_read(&b, buf, MIDI_CONTROL_CONFIG_BYTES_V3 - 1u) == MIDI_CONTROL_STATUS_BAD_LENGTH);
    /* The next save writes version 4. */
    midi_control_config_write(&b, buf);
    CHECK(buf[4] == MIDI_CONTROL_CONFIG_VERSION);

    /* Over SET_CONFIG, back over GET_CONFIG, and across a power cycle. */
    memset(g_slot, 0, sizeof(g_slot));
    g_storage_state = MIDI_CONTROL_STORAGE_READY;
    g_fail_write = 0;
    midi_control_device_init(&d, &ops);
    c = d.config;
    midi_control_config_set_program_name(&c, 127, "Fuzz Lead");
    midi_control_config_write(&c, buf);
    CHECK(roundtrip(&d, req, midi_control_build_config(req, MIDI_CONTROL_CMD_SET_CONFIG, buf, sizeof(buf)), &m));
    CHECK(midi_control_parse_result(&m, &cmd, &status));
    CHECK(cmd == MIDI_CONTROL_CMD_SET_CONFIG && status == MIDI_CONTROL_STATUS_OK && d.dirty);
    CHECK(strcmp(midi_control_config_program_name(&d.config, 127), "Fuzz Lead") == 0);
    memset(buf, 0, sizeof(buf));
    CHECK(roundtrip(&d, req, midi_control_build_simple(req, MIDI_CONTROL_CMD_GET_CONFIG), &m));
    CHECK(midi_control_parse_config_payload(&m, buf, sizeof(buf)) == MIDI_CONTROL_CONFIG_BYTES);
    CHECK(midi_control_config_read(&b, buf, sizeof(buf)) == MIDI_CONTROL_STATUS_OK);
    CHECK(strcmp(midi_control_config_program_name(&b, 127), "Fuzz Lead") == 0);
    CHECK(midi_control_device_save(&d) == MIDI_CONTROL_STATUS_OK);
    midi_control_device_init(&d, &ops);
    CHECK(d.loaded_from_storage);
    CHECK(strcmp(midi_control_config_program_name(&d.config, 127), "Fuzz Lead") == 0);

    /* On the screen, the name is the title while its program is the current
     * one - over the host's first line, which belongs to no program. */
    memset(&um, 0, sizeof(um));
    um.device = &d;
    midi_control_engine_set_program(&d.engine, 127);
    midi_control_ui_view(&v, &um);
    CHECK(strcmp(v.program.label, "32D") == 0 && strcmp(v.program.line1, "Fuzz Lead") == 0);
    strcpy(d.text[0], "From the host");
    strcpy(d.text[1], "Verse");
    midi_control_ui_view(&v, &um);
    CHECK(strcmp(v.program.line1, "Fuzz Lead") == 0 && strcmp(v.program.line2, "Verse") == 0);
    /* A program without a name shows the host's line, or with none the bank
     * and preset, as it always has. */
    midi_control_engine_set_program(&d.engine, 126);
    midi_control_ui_view(&v, &um);
    CHECK(strcmp(v.program.label, "32C") == 0 && strcmp(v.program.line1, "From the host") == 0);
    d.text[0][0] = 0;
    d.text[1][0] = 0;
    midi_control_ui_view(&v, &um);
    CHECK(v.program.line1[0] == 0 && strcmp(v.program.bank, "Bank 32") == 0);
    CHECK(strcmp(v.program.preset, "Preset C") == 0);

    /* However the program is chosen: a Program Change from the host... */
    midi_control_device_midi_in(&d, (const uint8_t[]){ 0xC0, 127 }, 2);
    CHECK(d.engine.program == 127);
    midi_control_ui_view(&v, &um);
    CHECK(strcmp(v.program.line1, "Fuzz Lead") == 0);
    /* ...or footswitch D, which chooses preset D of the bank by default. */
    CHECK(d.config.buttons[MIDI_CONTROL_BTN_FS_D].action == MIDI_CONTROL_ACT_PRESET_IN_BANK);
    midi_control_engine_set_program(&d.engine, 124);
    midi_control_ui_view(&v, &um);
    CHECK(strcmp(v.program.label, "32A") == 0 && v.program.line1[0] == 0);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_D, 1);
    midi_control_device_button(&d, MIDI_CONTROL_BTN_FS_D, 0);
    midi_control_ui_view(&v, &um);
    CHECK(strcmp(v.program.label, "32D") == 0 && strcmp(v.program.line1, "Fuzz Lead") == 0);

    /* The name belongs to the program, so it stays with it when the bank
     * size changes what the program is called. */
    c = d.config;
    c.global.bank_size = 8;
    midi_control_device_set_config(&d, &c);
    midi_control_engine_set_program(&d.engine, 127);
    midi_control_ui_view(&v, &um);
    CHECK(strcmp(v.program.label, "16H") == 0 && strcmp(v.program.line1, "Fuzz Lead") == 0);

    /* The longest name fits the title's place at the smallest size the
     * program region draws it. */
    CHECK(midi_control_gfx_text_width(16, 0, "MMMMMMMMMMMMMMM") <= MIDI_CONTROL_LCD_WIDTH - 10 - 196);
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--build-emmc") == 0)
        return build_emmc_cli(argv[2], argv[3]);
    if ((argc == 4 || argc == 5) && strcmp(argv[1], "--factory-emmc") == 0)
        return factory_emmc_cli(argv[2], argv[3], argc == 5 ? argv[4] : NULL);

    test_crc();
    test_pack7();
    test_config_roundtrip();
    test_config_v1();
    test_profiles_record();
    test_profiles_upgrade();
    test_profile_picker();
    test_profile_pages();
    test_profile_state();
    test_program_names();
    test_analog_calibration();
    test_analog_calibration_device();
    test_analog_capture();
    test_pedal_calibration();
    test_pedal_calibration_toe();
    test_pedal_calibration_state();
    test_analog_padding();
    test_engine_buttons();
    test_engine_bank_moves_programs();
    test_engine_encoders_and_analog();
    test_device_protocol();
    test_device_state_and_events();
    test_display_timeout();
    test_bootimage();
    test_hxf();
    test_gfx();
    test_ui();

    printf("%d checks, %d failed\n", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
