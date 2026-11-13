#ifndef MIDI_CONTROL_TUSB_CONFIG_H
#define MIDI_CONTROL_TUSB_CONFIG_H

/* TinyUSB configuration for the POD Go.
 *
 * Device only, USB0, High Speed. USB0 is the only controller with an on-chip
 * HS PHY.
 */

#define CFG_TUSB_MCU                OPT_MCU_LPC43XX
#define CFG_TUSB_OS                 OPT_OS_NONE
#define CFG_TUSB_DEBUG              0

#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_HIGH_SPEED

/* No memory section override on purpose. TinyUSB's DMA structures are
 * zero-initialised, so they land in .bss - and midi_control.ld puts .bss in AHB SRAM
 * at 0x20000000, which is where the USB controller's DMA can reach. */

#define CFG_TUD_ENDPOINT0_SIZE      64

/* The MIDI interface carries the controls and the configuration protocol; the
 * vendor interface carries the diagnostic log and the eMMC commands; the audio
 * function makes the unit a two-in two-out USB audio interface (usb_audio.h,
 * docs/10 section 8). All three are always present. */
#define MIDI_CONTROL_ENABLE_MIDI           1
#define MIDI_CONTROL_LOG_SYSEX             0

#define CFG_TUD_MIDI                1
#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_AUDIO               MIDI_CONTROL_ENABLE_MIDI
#define CFG_TUD_VENDOR              1

/* Every receive FIFO must be at least one High-Speed bulk packet, 512 bytes:
 * the class driver refuses to open an interface whose FIFO cannot hold one,
 * SET_CONFIGURATION then fails, and Windows reports the composite parent as
 * unable to start. */
#define CFG_TUD_VENDOR_RX_BUFSIZE   512
#define CFG_TUD_VENDOR_TX_BUFSIZE   512
#define CFG_TUD_VENDOR_EPSIZE       512

#define CFG_TUD_MIDI_EP_BUFSIZE     512
#define CFG_TUD_MIDI_RX_BUFSIZE     512
#define CFG_TUD_MIDI_TX_BUFSIZE     512

/* USB audio: 48 kHz, two channels each way, 4-byte subslots, so a packet is
 * at most 7 frames of 8 bytes (usb_audio.h has the derivation; usb_descriptors.c
 * checks it). Each software FIFO holds 32 packets, about 4.7 ms, of which the
 * feedback and the record flow control keep 1 ms filled. TinyUSB copies each
 * packet between its endpoint buffer (DMA, AHB SRAM) and the FIFO in the USB
 * interrupt. */
#define CFG_TUD_AUDIO_ENABLE_EP_IN              1
#define CFG_TUD_AUDIO_ENABLE_EP_OUT             1
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP        1
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX       56
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX      56
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ    (32 * 56)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ   (32 * 56)

#endif
