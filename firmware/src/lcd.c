/* The POD Go's 480x272 TFT: controller bring-up.
 *
 * The numbers here are properties of the panel fitted to this board,
 * established by observing how the hardware configures itself and checked
 * against the LPC4350 user manual (docs/01-hardware.md section 5).
 *
 * Two of the panel's parameters are deliberately set differently from the
 * vendor's configuration on the cold path:
 *
 * 1. Pixel format is chosen by the caller: mono needs no SDRAM, colour does.
 *
 * 2. Pixel clock source. The vendor runs the M4 at 204 MHz and divides PLL1
 *    for the LCD. On the cold path the clock is taken straight from the
 *    12 MHz crystal with the panel divider bypassed, which lands on the same
 *    12.0 MHz pixel clock without depending on the core clock.
 */

#include <stdint.h>
#include "board.h"
#include "lcd.h"

#define REG(a) (*(volatile uint32_t *)(a))

#define LCD_BASE      0x40008000u
#define LCD_TIMH      REG(LCD_BASE + 0x000)
#define LCD_TIMV      REG(LCD_BASE + 0x004)
#define LCD_POL       REG(LCD_BASE + 0x008)
#define LCD_LE        REG(LCD_BASE + 0x00C)
#define LCD_UPBASE    REG(LCD_BASE + 0x010)
#define LCD_LPBASE    REG(LCD_BASE + 0x014)
#define LCD_CTRL      REG(LCD_BASE + 0x018)
#define LCD_INTMSK    REG(LCD_BASE + 0x01C)
#define LCD_UPCURR    REG(LCD_BASE + 0x02C)   /* live DMA read address, read-only */
#define LCD_PAL(i)    REG(LCD_BASE + 0x200 + (i) * 4)
#define LCD_CRSR_CTRL REG(LCD_BASE + 0xC00)

#define CGU_BASE      0x40050000u
#define BASE_LCD_CLK  REG(CGU_BASE + 0x088)

#define CCU1_BASE     0x40051000u
#define CLK_M4_LCD_CFG  REG(CCU1_BASE + 0x418)
#define CLK_M4_LCD_STAT REG(CCU1_BASE + 0x41C)

#define SCU_BASE      0x40086000u
#define SCU_SFS(port, pin)  REG(SCU_BASE + (port) * 0x80 + (pin) * 4)

#define CTRL_LCDEN    (1u << 0)
#define CTRL_BPP(n)   ((uint32_t)(n) << 1)
#define CTRL_TFT      (1u << 5)
#define CTRL_BGR      (1u << 8)
#define CTRL_BEBO     (1u << 9)
#define CTRL_BEPO     (1u << 10)
#define CTRL_LCDPWR   (1u << 11)
#define CTRL_WATERMARK (1u << 16)

/* CTRL's BPP field is the PL111 encoding, not a bit count:
 * 0=1bpp 1=2 2=4 3=8 4=16(1:5:5:5) 5=24 6=16(5:6:5) 7=12(4:4:4). */
#define BPP_1          0u
#define BPP_RGB565     6u

/* Panel timings. Horizontal total 480+1+2+42 = 525 clocks, vertical
 * 272+1+2+2 = 277 lines; at 12 MHz that is 82.5 Hz. */
#define PANEL_HSW      1
#define PANEL_HFP      2
#define PANEL_HBP      42
#define PANEL_VSW      1
#define PANEL_VFP      2
#define PANEL_VBP      2

#define MS100          2000000u
#define MS250          5000000u

static uint8_t *g_fb;
static enum midi_control_gfx_format g_format = MIDI_CONTROL_GFX_MONO;
static int g_cold;
static int g_force_cold;

int lcd_was_cold_started(void) { return g_cold; }
int lcd_format_is_colour(void) { return g_format == MIDI_CONTROL_GFX_RGB565; }

/* The LCD data and control pins, as the vendor's display init configures them.
 * PF_0 at FUNC1 is not an LCD_VD line; it is muxed because the vendor's working
 * init muxes it. */
static void lcd_pins(void)
{
    for (unsigned pin = 1; pin <= 10; pin++)
        SCU_SFS(4, pin) = 0x32;             /* FUNC2, EPUN | EHS */
    SCU_SFS(4, 7) = 0x30;                   /* FUNC0 - LCD_DCLK */

    for (unsigned pin = 0; pin <= 7; pin++)
        SCU_SFS(7, pin) = 0x33;             /* FUNC3 */

    for (unsigned pin = 3; pin <= 7; pin++)
        SCU_SFS(8, pin) = 0x33;             /* FUNC3 */

    for (unsigned pin = 0; pin <= 6; pin++)
        SCU_SFS(11, pin) = 0x32;            /* PB_0..PB_6, FUNC2 */

    SCU_SFS(15, 0) = 0x31;                  /* PF_0, FUNC1 */
}

static uint32_t lcd_frame_bytes(uint32_t bpp_field)
{
    static const uint8_t k_bits[8] = { 1, 2, 4, 8, 16, 24, 16, 12 };
    return (uint32_t)MIDI_CONTROL_LCD_WIDTH * MIDI_CONTROL_LCD_HEIGHT * k_bits[bpp_field & 7u] / 8u;
}

/* Point the running controller at `base` and wait until it is actually
 * scanning there. A new UPBASE takes effect at the next frame, about 12 ms
 * off, and UPCURR shows when it has. Bounded. */
static void lcd_move_base(uint32_t base, uint32_t bytes)
{
    LCD_UPBASE = base;
    LCD_LPBASE = base;
    for (unsigned i = 0; i < 400u; i++) {
        const uint32_t cur = LCD_UPCURR;
        if (cur >= base && cur < base + bytes)
            return;
        board_delay_crude(10000u);
    }
}

static void fb_clear(uint32_t bytes)
{
    uint32_t *p = (uint32_t *)(void *)g_fb;
    for (uint32_t i = 0; i < bytes / 4u; i++)
        p[i] = 0;
}

void lcd_init_cold(void *framebuffer, enum midi_control_gfx_format format)
{
    g_force_cold = 1;
    lcd_init(framebuffer, format);
    g_force_cold = 0;
}

void lcd_init(void *framebuffer, enum midi_control_gfx_format format)
{
    g_fb = (uint8_t *)framebuffer;
    g_format = format;

    /* The controller is normally already running: a DFU load does not reset
     * the chip and the previous firmware's LCD setup is still live, driving
     * this exact panel. Those timings are worth more than any set of numbers
     * from a datasheet, so read them back and keep them. Change only the
     * framebuffer address, the pixel format and the palette. */
    uint32_t timh = LCD_TIMH;
    uint32_t timv = LCD_TIMV;
    uint32_t pol = LCD_POL;
    uint32_t ctrl = LCD_CTRL;

    /* Inheriting is only safe if what is running is this geometry. */
    unsigned live_w = ((timh >> 2) & 0x3F) * 16 + 16;
    unsigned live_h = (timv & 0x3FF) + 1;

    g_cold = g_force_cold
          || !((ctrl & CTRL_LCDEN) && pol
               && live_w == MIDI_CONTROL_LCD_WIDTH && live_h == MIDI_CONTROL_LCD_HEIGHT);

    if (g_cold) {
        timh = (uint32_t)((MIDI_CONTROL_LCD_WIDTH / 16) - 1) << 2
             | (uint32_t)(PANEL_HSW - 1) << 8
             | (uint32_t)(PANEL_HFP - 1) << 16
             | (uint32_t)(PANEL_HBP - 1) << 24;

        timv = (uint32_t)(MIDI_CONTROL_LCD_HEIGHT - 1)
             | (uint32_t)(PANEL_VSW - 1) << 10
             | (uint32_t)(PANEL_VFP - 1) << 16
             | (uint32_t)(PANEL_VBP - 1) << 24;

        /* CPL = pixels per line - 1, VSYNC and HSYNC both inverted, and BCD to
         * bypass the panel clock divider so LCDDCLK is the 12 MHz input. */
        pol = (uint32_t)(MIDI_CONTROL_LCD_WIDTH - 1) << 16
            | (1u << 11)                     /* IVS */
            | (1u << 12)                     /* IHS */
            | (1u << 26);                    /* BCD */
    }

    const uint32_t base = (uint32_t)framebuffer & ~7u;   /* UPBASE ignores the low 3 bits */
    const uint32_t bpp = g_format == MIDI_CONTROL_GFX_RGB565 ? BPP_RGB565 : BPP_1;
    const uint32_t bytes = lcd_frame_bytes(bpp);

    /* A running controller does not restart from UPBASE when its format
     * changes; it finishes the current frame at the new pixel size from
     * wherever its DMA has reached. Growing the format while it still scans a
     * buffer too small for the new size runs the DMA off the end of that
     * buffer and latches the panel white. So order the two changes by which
     * way the size goes: growing, move the base first and wait for it; else
     * change the format first, then move. docs/09-hazards.md. */
    const int running = (ctrl & CTRL_LCDEN) != 0;
    const int grow = running && bytes > lcd_frame_bytes((ctrl >> 1) & 7u);

    fb_clear(bytes);

    if (grow)
        lcd_move_base(base, bytes);

    LCD_CTRL &= ~CTRL_LCDPWR;
    board_delay_crude(MS250);
    LCD_CTRL &= ~CTRL_LCDEN;

    if (g_cold) {
        CLK_M4_LCD_CFG |= 1u;
        for (uint32_t i = 0; i < 100000u && !(CLK_M4_LCD_STAT & 1u); i++)
            ;
        /* Pixel clock: crystal (CLK_SEL 0x06), AUTOBLOCK while switching. */
        BASE_LCD_CLK = (0x06u << 24) | (1u << 11);
        lcd_pins();
    }

    LCD_INTMSK = 0;
    LCD_CRSR_CTRL = 0;

    LCD_TIMH = timh;
    LCD_TIMV = timv;
    LCD_POL = pol;
    LCD_LE = 0;

    if (!running) {
        LCD_UPBASE = base;
        LCD_LPBASE = base;
    }

    /* The palette only exists below 16 bpp. Entry 0 black, entry 1 white. */
    if (g_format == MIDI_CONTROL_GFX_MONO) {
        for (unsigned i = 0; i < 128; i++)
            LCD_PAL(i) = 0;
        LCD_PAL(0) = 0xFFFFu << 16;
    }

    /* Keep the bits that describe the panel (TFT, BGR, watermark) on the
     * inherited path; BEBO and BEPO describe memory layout and are ours. */
    if (g_cold)
        ctrl = CTRL_TFT | CTRL_BGR | CTRL_WATERMARK;
    else
        ctrl &= ~(CTRL_LCDEN | CTRL_LCDPWR | CTRL_BPP(7) | CTRL_BEBO | CTRL_BEPO);

    LCD_CTRL = ctrl | CTRL_BPP(bpp);
    LCD_CTRL |= CTRL_LCDEN;
    board_delay_crude(MS100);
    LCD_CTRL |= CTRL_LCDPWR;

    if (!grow)
        lcd_move_base(base, bytes);
}

/* lcd_init() clears LCDPWR with LCDEN still set on every start, and leaves both
 * set. This repeats that one step and its reverse on the running controller,
 * never touching the base or the format, whose order is the hazard in docs/09. */
void lcd_set_power(int on)
{
    if (!on)
        LCD_CTRL &= ~CTRL_LCDPWR;
    else if (LCD_CTRL & CTRL_LCDEN)
        LCD_CTRL |= CTRL_LCDPWR;
}

uint32_t lcd_reg(unsigned offset)
{
    return REG(LCD_BASE + offset);
}
