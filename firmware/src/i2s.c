/* I2S0 as the slave end of the SHARC link. docs/10 section 2 has the frame;
 * the register values are the stock firmware's, as the parent project's lab
 * image ran them under the pass-through on 2026-11-13. */

#include "i2s.h"

#include "log.h"
#include "usb_audio.h"

#define REG(a) (*(volatile uint32_t *)(a))

#define I2S0_BASE      0x400A2000u
#define I2S_DAO        REG(I2S0_BASE + 0x00)
#define I2S_DAI        REG(I2S0_BASE + 0x04)
#define I2S_TXFIFO     REG(I2S0_BASE + 0x08)
#define I2S_RXFIFO     REG(I2S0_BASE + 0x0C)
#define I2S_STATE      REG(I2S0_BASE + 0x10)
#define I2S_DMA1       REG(I2S0_BASE + 0x14)
#define I2S_DMA2       REG(I2S0_BASE + 0x18)
#define I2S_IRQ        REG(I2S0_BASE + 0x1C)
#define I2S_TXRATE     REG(I2S0_BASE + 0x20)
#define I2S_RXRATE     REG(I2S0_BASE + 0x24)
#define I2S_TXBITRATE  REG(I2S0_BASE + 0x28)
#define I2S_RXBITRATE  REG(I2S0_BASE + 0x2C)
#define I2S_TXMODE     REG(I2S0_BASE + 0x30)
#define I2S_RXMODE     REG(I2S0_BASE + 0x34)

/* DAO/DAI: 32-bit words, slave, word select half period 31. */
#define DA_WIDTH_32    0x3u
#define DA_STOP        (1u << 3)
#define DA_RESET       (1u << 4)
#define DA_SLAVE       (1u << 5)
#define DA_MUTE        (1u << 15)
#define DA_BASE        (DA_WIDTH_32 | DA_SLAVE | (31u << 6))

#define RX_LEVEL(s)    (((s) >> 8) & 0xFu)
#define TX_LEVEL(s)    (((s) >> 16) & 0xFu)
#define FIFO_DEPTH     8u

#define IRQ_RX_EN      (1u << 0)
#define IRQ_RX_DEPTH(n) ((uint32_t)(n) << 8)

/* Transmit takes the receive bit clock and word select (TXCLKSEL 2, TX4PIN),
 * so four pins carry the link. */
#define TXMODE_SHARED  0x6u

#define CGU_BASE_M4_CLK    REG(0x4005006Cu)
#define CGU_BASE_APB1_CLK  REG(0x40050080u)
#define CCU1_I2S_CFG       REG(0x40051218u)   /* CLK_APB1_I2S */
#define CCU1_I2S_STAT      REG(0x4005121Cu)
#define CLK_SEL_MASK       0x1F000000u
#define CLK_AUTOBLOCK      (1u << 11)

/* Three inputs with the input buffer on, one output with it off. P3_0, the
 * pin I2S0 would otherwise clock on, is SSP0_SCK. docs/01. */
#define SCU_P3_1       REG(0x40086184u)   /* 0x51 FUNC1 in:  I2S0_RX_WS */
#define SCU_P3_2       REG(0x40086188u)   /* 0x10 FUNC0 out: I2S0_TX_SDA */
#define SCU_P6_0       REG(0x40086300u)   /* 0x54 FUNC4 in:  I2S0_RX_SCK */
#define SCU_P6_2       REG(0x40086308u)   /* 0x53 FUNC3 in:  I2S0_RX_SDA */

#define I2S0_IRQ_NUMBER 28u
#define NVIC_ISER ((volatile uint32_t *)0xE000E100u)
#define NVIC_ICPR ((volatile uint32_t *)0xE000E280u)
#define NVIC_IPR  ((volatile uint8_t *)0xE000E400u)

/* The M4 implements the top three bits of each priority byte. */
#define USB0_IRQ_NUMBER 8u
#define PRIORITY_USB0   0x20u

static struct midi_control_audio g_audio;
static volatile uint32_t g_overruns;

int i2s_init(void)
{
    midi_control_audio_init(&g_audio);

    /* APB1 follows the core's source. Left on the 12 MHz IRC, every FIFO
     * access crosses a bridge at a seventeenth of the core clock, three per
     * word, 384,000 words a second: the interrupt keeps up and nothing else
     * runs (seen in the parent project as a frozen screen). */
    log_line("i2s: CGU BASE_APB1_CLK from BASE_M4_CLK's source");
    CGU_BASE_APB1_CLK = (CGU_BASE_M4_CLK & CLK_SEL_MASK) | CLK_AUTOBLOCK;
    log_line("i2s: CCU1 CLK_APB1_I2S");
    if (!(CCU1_I2S_STAT & 1u)) {
        CCU1_I2S_CFG |= 3u;              /* RUN, AUTO */
        for (uint32_t i = 0; i < 2000000u && !(CCU1_I2S_STAT & 1u); i++)
            ;
        if (!(CCU1_I2S_STAT & 1u)) {
            log_line("i2s: the branch clock did not start - not touching I2S0");
            return -1;
        }
    }

    log_line("i2s: I2S0 DAO, DAI, modes, pins");
    /* Both directions in reset while the pins change, so nothing half
     * configured is shifted out at the SHARC. */
    I2S_DAO = DA_BASE | DA_MUTE | DA_STOP | DA_RESET;
    I2S_DAI = DA_BASE | DA_STOP | DA_RESET;

    SCU_P6_0 = 0x54u;
    SCU_P3_1 = 0x51u;
    SCU_P6_2 = 0x53u;
    SCU_P3_2 = 0x10u;

    I2S_TXMODE = TXMODE_SHARED;
    I2S_RXMODE = 0;
    I2S_TXRATE = 0;
    I2S_RXRATE = 0;
    I2S_TXBITRATE = 0;
    I2S_RXBITRATE = 0;
    I2S_DMA1 = 0;
    I2S_DMA2 = 0;
    I2S_IRQ = 0;
    return 0;
}

void i2s_start(void)
{
    /* The stock order: out of reset, prime the transmit FIFO while it is still
     * stopped, then run. Releasing reset after priming would flush the primer. */
    I2S_DAO &= ~DA_RESET;
    I2S_DAI &= ~DA_RESET;
    I2S_TXFIFO = 0;
    I2S_TXFIFO = 0;
    I2S_DAO &= ~(DA_MUTE | DA_STOP);
    I2S_DAI &= ~DA_STOP;

    g_overruns = 0;

    /* The link's interrupt has four words, 21 us, before its FIFO overruns,
     * and the USB interrupt now copies audio packets. So the link pre-empts
     * USB rather than queueing behind it. The two share only the audio FIFOs,
     * one writer and one reader each. */
    log_line("i2s: NVIC IPR, USB0 below I2S0");
    NVIC_IPR[USB0_IRQ_NUMBER] = PRIORITY_USB0;
    NVIC_IPR[I2S0_IRQ_NUMBER] = 0u;

    NVIC_ICPR[I2S0_IRQ_NUMBER >> 5] = 1u << (I2S0_IRQ_NUMBER & 31u);
    /* At half a FIFO, as the stock firmware does: half a FIFO of slack. */
    I2S_IRQ = IRQ_RX_EN | IRQ_RX_DEPTH(4);
    NVIC_ISER[I2S0_IRQ_NUMBER >> 5] = 1u << (I2S0_IRQ_NUMBER & 31u);
    log_line("i2s: running");
}

void I2S0_IRQHandler(void)
{
    unsigned n = RX_LEVEL(I2S_STATE);

    if (n >= FIFO_DEPTH)
        g_overruns++;

    /* One word out for every word in, always - hunting included - so the two
     * FIFOs stay in step. */
    while (n--) {
        uint32_t frames = g_audio.frames;
        uint32_t tx = midi_control_audio_word(&g_audio, I2S_RXFIFO);
        if (TX_LEVEL(I2S_STATE) < FIFO_DEPTH)
            I2S_TXFIFO = tx;
        if (g_audio.frames != frames)
            usb_audio_frame(&g_audio);      /* a frame boundary: swap with the host */
    }
}

void i2s_set_gain(uint16_t gain)
{
    g_audio.gain = gain > MIDI_CONTROL_AUDIO_GAIN_MAX ? MIDI_CONTROL_AUDIO_GAIN_MAX : gain;
}

void i2s_set_record_gain(uint32_t gain)
{
    g_audio.record_gain = gain;
}

void i2s_set_mode(uint8_t mode)
{
    g_audio.mode = mode <= MIDI_CONTROL_AUDIO_MODE_TONE_LAST ? mode : MIDI_CONTROL_AUDIO_MODE_MONITOR;
}

const struct midi_control_audio *i2s_audio(void)
{
    return &g_audio;
}

uint32_t i2s_overruns(void)
{
    return g_overruns;
}

void i2s_levels_take(uint32_t *host_peak, uint32_t *knob_peak, uint32_t *knob_clips)
{
    *host_peak = g_audio.host_peak;
    g_audio.host_peak = 0;
    *knob_peak = g_audio.knob_peak;
    g_audio.knob_peak = 0;
    *knob_clips = g_audio.knob_clips;
    g_audio.knob_clips = 0;
}

uint32_t i2s_peak_take(unsigned slot)
{
    if (slot >= MIDI_CONTROL_AUDIO_INPUTS)
        return 0;
    uint32_t v = g_audio.peak[slot];
    g_audio.peak[slot] = 0;
    return v;
}
