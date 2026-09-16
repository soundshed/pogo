/* Vector table and reset handler for the RAM-resident POD Go image.
 *
 * The LPC43xx M4 has 53 external interrupts, so the table is 16 + 53 entries.
 * The verified IRQ layout places M0APP at IRQ1, DMA at IRQ2, SDIO at IRQ6,
 * LCD at IRQ7, TIMER0-3 at IRQ12-15, I2C0 at IRQ18, and USB0 at IRQ8
 * (docs/05).
 */

#include <stdint.h>

extern uint32_t _data_load, _data_start, _data_end, _bss_start, _bss_end;
extern uint32_t _stack_top;

extern int main(void);
void Reset_Handler(void);
void reset_continue(void);
void Default_Handler(void);

#define ALIAS(f) __attribute__((weak, alias(#f)))

void NMI_Handler(void)        ALIAS(Default_Handler);
void HardFault_Handler(void)  ALIAS(Default_Handler);
void MemManage_Handler(void)  ALIAS(Default_Handler);
void BusFault_Handler(void)   ALIAS(Default_Handler);
void UsageFault_Handler(void) ALIAS(Default_Handler);
void SVC_Handler(void)        ALIAS(Default_Handler);
void DebugMon_Handler(void)   ALIAS(Default_Handler);
void PendSV_Handler(void)     ALIAS(Default_Handler);
void SysTick_Handler(void)    ALIAS(Default_Handler);
void USB0_IRQHandler(void)    ALIAS(Default_Handler);
void I2S0_IRQHandler(void)    ALIAS(Default_Handler);

typedef void (*vector_t)(void);

__attribute__((section(".isr_vector"), used))
const vector_t g_vectors[16 + 53] = {
    (vector_t)&_stack_top,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0, 0, 0, 0,                 /* 7-10 reserved; the ROM does not checksum a
                                 * DFU-loaded image, but keep the shape right */
    SVC_Handler,
    DebugMon_Handler,
    0,                          /* 13 reserved */
    PendSV_Handler,
    SysTick_Handler,

    /* IRQ 0..52 */
    Default_Handler,            /*  0 DAC        */
    Default_Handler,            /*  1 M0APP      */
    Default_Handler,            /*  2 DMA        */
    Default_Handler,            /*  3 reserved   */
    Default_Handler,            /*  4 FLASH/EE   */
    Default_Handler,            /*  5 ETH        */
    Default_Handler,            /*  6 SDIO       */
    Default_Handler,            /*  7 LCD        */
    USB0_IRQHandler,            /*  8 USB0       */
    Default_Handler,            /*  9 USB1       */
    Default_Handler,            /* 10 SCT        */
    Default_Handler,            /* 11 RITIMER    */
    Default_Handler,            /* 12 TIMER0     */
    Default_Handler,            /* 13 TIMER1     */
    Default_Handler,            /* 14 TIMER2     */
    Default_Handler,            /* 15 TIMER3     */
    Default_Handler,            /* 16 MCPWM      */
    Default_Handler,            /* 17 ADC0       */
    Default_Handler,            /* 18 I2C0       */
    Default_Handler,            /* 19 I2C1       */
    Default_Handler,            /* 20 SPI        */
    Default_Handler,            /* 21 ADC1       */
    Default_Handler,            /* 22 SSP0       */
    Default_Handler,            /* 23 SSP1       */
    Default_Handler,            /* 24 USART0     */
    Default_Handler,            /* 25 UART1      */
    Default_Handler,            /* 26 USART2     */
    Default_Handler,            /* 27 USART3     */
    I2S0_IRQHandler,            /* 28 I2S0       */
    Default_Handler,            /* 29 I2S1       */
    Default_Handler,            /* 30 SPIFI      */
    Default_Handler,            /* 31 SGPIO      */
    Default_Handler,            /* 32 PIN_INT0   */
    Default_Handler,            /* 33 PIN_INT1   */
    Default_Handler,            /* 34 PIN_INT2   */
    Default_Handler,            /* 35 PIN_INT3   */
    Default_Handler,            /* 36 PIN_INT4   */
    Default_Handler,            /* 37 PIN_INT5   */
    Default_Handler,            /* 38 PIN_INT6   */
    Default_Handler,            /* 39 PIN_INT7   */
    Default_Handler,            /* 40 GINT0      */
    Default_Handler,            /* 41 GINT1      */
    Default_Handler,            /* 42 EVENTROUTER*/
    Default_Handler,            /* 43 C_CAN1     */
    Default_Handler,            /* 44 reserved   */
    Default_Handler,            /* 45 ADCHS      */
    Default_Handler,            /* 46 ATIMER     */
    Default_Handler,            /* 47 RTC        */
    Default_Handler,            /* 48 reserved   */
    Default_Handler,            /* 49 WWDT       */
    Default_Handler,            /* 50 reserved   */
    Default_Handler,            /* 51 C_CAN0     */
    Default_Handler,            /* 52 QEI        */
};

/* The ROM leaves VTOR pointing wherever it pleases. Our table is at the load
 * base, so say so explicitly rather than relying on it. */
#define SCB_VTOR     (*(volatile uint32_t *)0xE000ED08)
#define SCB_ICSR     (*(volatile uint32_t *)0xE000ED04)
#define SYST_CSR     (*(volatile uint32_t *)0xE000E010)
#define NVIC_ICER(n) (*(volatile uint32_t *)(0xE000E180u + 4u * (n)))
#define NVIC_ICPR(n) (*(volatile uint32_t *)(0xE000E280u + 4u * (n)))

#define ICSR_PENDSTCLR (1u << 25)
#define ICSR_PENDSVCLR (1u << 27)

/* Entry, from either of the two things that start this image. Neither leaves a
 * stack this image can keep, for different reasons:
 *
 * - The boot ROM, after a DFU load, does not load vector word 0 into MSP
 *   (docs/04 section 2).
 * - The NOR bootloader, after loading the image off the eMMC, branches to it
 *   from inside its loader task (0x14001bb8) without touching SP. That task
 *   runs in thread mode on PSP - its main selects it at 0x14001b74 - and both
 *   stacks are in SDRAM: the task's at 0x29f40608..0x29f41608 and MSP at
 *   0x29f42608 (0x14001a44..0x14001a58). main() reconfigures the EMC, and a
 *   stack inside the memory being reconfigured does not survive that.
 *
 * So the first instructions put thread mode on MSP and load our own, before
 * anything is pushed. It has to be assembly: a C function's prologue would
 * already have pushed onto the stack being abandoned. */
__asm__(
    ".pushsection .text.Reset_Handler, \"ax\", %progbits\n"
    ".syntax unified\n"
    ".thumb\n"
    ".global Reset_Handler\n"
    ".type Reset_Handler, %function\n"
    ".thumb_func\n"
    "Reset_Handler:\n"
    "    cpsid   i\n"
    "    ldr     r0, =_stack_top\n"
    "    msr     msp, r0\n"
    "    movs    r0, #0\n"
    "    msr     control, r0\n"             /* thread mode on MSP, FPCA clear */
    "    isb\n"
    "    msr     basepri, r0\n"
    "    b       reset_continue\n"
    ".ltorg\n"
    ".popsection\n"
);

/* Everything else the NOR bootloader leaves armed, stopped before anything can
 * unmask interrupts.
 *
 * Its kernel start (0x14004a98) programs SysTick with TICKINT set (0x14004cac,
 * SYST_CSR = 6) and then enables it (0x14004cdc). Those two functions are the
 * only code that touches SYST_CSR, so it is still ticking at the handover,
 * which only sets PRIMASK. PendSV is its context switch (vector 14,
 * 0x14001be1) and may be pending as well.
 *
 * main() unmasks interrupts early, and every vector in the table above except
 * USB0 and I2S0 is Default_Handler, which hangs by design - so one inherited
 * tick hangs the unit before the panel is ever lit. After a DFU load none of
 * this is set and every write here is a no-op. */
__attribute__((used))
void reset_continue(void)
{
    SYST_CSR = 0;
    for (unsigned n = 0; n < 2; n++) {           /* 53 external IRQs: two words */
        NVIC_ICER(n) = 0xFFFFFFFFu;
        NVIC_ICPR(n) = 0xFFFFFFFFu;
    }
    SCB_ICSR = ICSR_PENDSTCLR | ICSR_PENDSVCLR;

    const uint32_t *src = &_data_load;
    uint32_t *dst = &_data_start;
    while (dst < &_data_end)
        *dst++ = *src++;

    for (dst = &_bss_start; dst < &_bss_end; dst++)
        *dst = 0;

    SCB_VTOR = (uint32_t)g_vectors;

    main();

    for (;;)
        ;
}

void Default_Handler(void)
{
    /* Hang deliberately. A crash must not be mistakable for success - the same
     * reasoning as firmware/dfu-test (docs/04): if a fault silently returned,
     * "the device did nothing" would be ambiguous. */
    for (;;)
        ;
}
