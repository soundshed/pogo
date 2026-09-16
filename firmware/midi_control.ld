/* Linker script for a RAM-resident POD Go image loaded over the NXP boot ROM's
 * USB0 DFU.
 *
 * Two facts drive this layout, and both are established rather than assumed:
 *
 * 1. The ROM loads the image to 0x10000000 (docs/04). The LPC4350 has 128 KB of
 *    local SRAM there and 72 KB at 0x10080000. Nothing else in this image uses
 *    the 72 KB bank, so all of it is the stack. Reset_Handler loads MSP from
 *    _stack_top itself, because neither thing that starts this image does: the
 *    boot ROM ignores vector word 0 (docs/04 section 2), and the NOR bootloader
 *    branches with SP still on its own task stack in SDRAM (startup.c).
 *
 * 2. **The USB controller's DMA cannot reach local SRAM.** Every USB-visible
 *    structure - the endpoint queue heads, transfer descriptors and buffers -
 *    must live in AHB SRAM at 0x20000000. The USB DMA layout uses 0x10000000
 *    for code and 0x2000xxxx for data, including descriptors and buffers
 *    (docs/05 load records, docs/10 section 1).
 *
 * So: all code and read-only data at 0x10000000, and all writable data - which
 * includes everything TinyUSB touches with DMA - at 0x20000000.
 */

MEMORY
{
  CODE (rx)  : ORIGIN = 0x10000000, LENGTH = 128K   /* local SRAM, ROM loads here */
  AHB  (rwx) : ORIGIN = 0x20000000, LENGTH = 64K    /* AHB SRAM, USB DMA can reach this */
}

_stack_top = 0x10092000;                            /* top of the 72 KB bank */

ENTRY(Reset_Handler)

SECTIONS
{
  .isr_vector : ALIGN(4)
  {
    KEEP(*(.isr_vector))
  } > CODE

  .text : ALIGN(4)
  {
    *(.text .text.*)
    *(.rodata .rodata.*)
    *(.glue_7 .glue_7t)
    KEEP(*(.eh_frame*))
    . = ALIGN(4);
  } > CODE

  /* .data lives in AHB SRAM but is carried in the image after .text, and the
   * reset handler copies it across. */
  _data_load = LOADADDR(.data);

  .data : ALIGN(4)
  {
    _data_start = .;
    *(.data .data.*)
    . = ALIGN(4);
    _data_end = .;
  } > AHB AT > CODE

  .bss (NOLOAD) : ALIGN(4)
  {
    _bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(4);
    _bss_end = .;
  } > AHB

  _heap_start = .;

  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}

/* TinyUSB's endpoint queue heads live in `_dcd_data`, which is zero-initialised
 * and already carries TU_ATTR_ALIGNED(2048) of its own. So it lands in .bss and
 * needs no special section - .bss is in AHB SRAM, which is the requirement.
 *
 * Deliberately *not* using CFG_TUD_MEM_SECTION to force it somewhere: a section
 * attribute turns zero-initialised data into PROGBITS, which a (NOLOAD) section
 * would then silently drop, leaving the queue heads as garbage. Letting it fall
 * into .bss is both simpler and safer. This assert is what guarantees it. */
ASSERT(_bss_start >= 0x20000000 && _bss_end <= 0x20010000,
       "USB DMA structures live in .bss, which must be in AHB SRAM - the USB controller cannot reach local SRAM")

/* board_enter_dfu() (board.c) runs its ROM calls from a copy at 0x1001FF40
 * while the boot ROM downloads the next image from 0x10000000, so an image
 * reaching that address would overwrite them mid-call. The next image is
 * normally this one, which therefore ends below it - and below the reserved
 * DFU flag at 0x1001FF00 - and the copy fits in the 192 bytes left of the
 * bank. */
ASSERT(LOADADDR(.data) + SIZEOF(.data) <= 0x1001FF00,
       "the image reaches the DFU trampoline's address, 0x1001FF40 (board.c)")
ASSERT(board_dfu_stub_end - board_dfu_stub <= 0xC0,
       "the DFU trampoline no longer fits between 0x1001FF40 and the end of the bank (board.c)")
