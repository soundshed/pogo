/* Minimal stand-in for NXP LPCOpen's chip.h.
 *
 * TinyUSB's LPC18xx/43xx port (src/portable/chipidea/ci_hs/ci_hs_lpc18_43.h)
 * does `#include "chip.h"` and expects LPCOpen. Vendoring the whole LPCOpen
 * SDK to satisfy it would be absurd: the port needs exactly four symbols and
 * two NVIC calls, all of which we already know from our own analysis.
 *
 * USB0 at 0x40006000 is established from the firmware, not a datasheet - the
 * PORTSC1 is at +0x184 there, and the LCD controller uses 0x40008000, which
 * pins the surrounding peripheral map (docs/10 section 1). USB0 is IRQ8
 * (docs/05).
 */

#ifndef MIDI_CONTROL_CHIP_H
#define MIDI_CONTROL_CHIP_H

#include <stdint.h>

#define LPC_USB0_BASE  0x40006000UL
#define LPC_USB1_BASE  0x40007000UL

typedef enum {
    USB0_IRQn = 8,
    USB1_IRQn = 9,
} IRQn_Type;

/* Cortex-M NVIC, straight out of the ARMv7-M architecture. Hand-rolled rather
 * than pulled from CMSIS so this file has no dependencies at all. */
#define NVIC_ISER ((volatile uint32_t *)0xE000E100UL)
#define NVIC_ICER ((volatile uint32_t *)0xE000E180UL)

static inline void NVIC_EnableIRQ(IRQn_Type irq)
{
    uint32_t n = (uint32_t)irq;
    NVIC_ISER[n >> 5] = 1UL << (n & 0x1F);
}

static inline void NVIC_DisableIRQ(IRQn_Type irq)
{
    uint32_t n = (uint32_t)irq;
    NVIC_ICER[n >> 5] = 1UL << (n & 0x1F);
    __asm__ volatile("dsb 0xF" ::: "memory");
    __asm__ volatile("isb 0xF" ::: "memory");
}

#endif
