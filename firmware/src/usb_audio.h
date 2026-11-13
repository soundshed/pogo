#ifndef MIDI_CONTROL_USB_AUDIO_H
#define MIDI_CONTROL_USB_AUDIO_H

/* USB Audio Class 2.0 between the host and the SHARC link (docs/10 section 8).
 *
 * The unit is a 48 kHz, 24-bit, two-in two-out interface. Playback is an
 * asynchronous isochronous OUT endpoint with an explicit feedback endpoint;
 * the record stream is an asynchronous isochronous IN endpoint. The sample
 * clock is the SHARC's (its crystal drives the link), so the host follows the
 * unit, never the other way round.
 *
 * TinyUSB moves the packets in the USB interrupt into two software FIFOs. The
 * link's interrupt calls usb_audio_frame() once per 48 kHz frame, which takes
 * one frame from the playback FIFO and puts one in the record FIFO. Each FIFO
 * has one writer and one reader, one in each interrupt. */

#include <stdint.h>

#include "midi_control/midi_control_audio.h"

/* The function's numbers. usb_descriptors.c builds the descriptors from these. */
#define USB_ITF_AUDIO_CONTROL   3u
#define USB_ITF_AUDIO_PLAY      4u
#define USB_ITF_AUDIO_RECORD    5u
#define USB_ITF_AUDIO_COUNT     3u

#define USB_EP_AUDIO_PLAY       0x04u   /* isochronous OUT */
#define USB_EP_AUDIO_FEEDBACK   0x84u   /* isochronous IN, 4 bytes, 16.16 samples per microframe */
#define USB_EP_AUDIO_RECORD     0x85u   /* isochronous IN */

#define USB_AUDIO_ENTITY_CLOCK  0x10u
#define USB_AUDIO_TERM_PLAY_USB 0x11u   /* input terminal: the host's stream */
#define USB_AUDIO_TERM_PLAY_OUT 0x12u   /* output terminal: the outputs */
#define USB_AUDIO_TERM_REC_IN   0x13u   /* input terminal: the guitar input */
#define USB_AUDIO_TERM_REC_USB  0x14u   /* output terminal: the stream to the host */

#define USB_AUDIO_RATE          48000u
#define USB_AUDIO_SUBSLOT       4u      /* bytes per sample */
#define USB_AUDIO_BITS          24u
#define USB_AUDIO_FRAME_BYTES   (MIDI_CONTROL_AUDIO_HOST_CHANNELS * USB_AUDIO_SUBSLOT)

/* Largest packet: one more frame than the 6 a microframe carries at 48 kHz. */
#define USB_AUDIO_EP_SIZE       ((USB_AUDIO_RATE / 8000u + 1u) * USB_AUDIO_FRAME_BYTES)

/* Playback starts once this much waits in the FIFO, and the feedback holds the
 * FIFO near it: 1 ms, the playback latency the unit adds. The record FIFO is
 * held near the same level. */
#define USB_AUDIO_TARGET_BYTES  (48u * USB_AUDIO_FRAME_BYTES)

/* From the link's interrupt, once per frame, after midi_control_audio_word()
 * has completed one. Sets a->host and a->host_in; sends a->capture. */
void usb_audio_frame(struct midi_control_audio *a);

/* From the main loop. Stops the streams when the host has gone. */
void usb_audio_task(void);

/* Counters for the status packet. */
struct usb_audio_status {
    uint8_t play;           /* 0 closed, 1 filling, 2 playing */
    uint8_t record;         /* 0 closed, 1 open */
    uint32_t play_frames;   /* frames taken from the host */
    uint32_t underruns;     /* frames the host's stream did not have ready */
    uint32_t record_frames; /* frames sent to the host */
    uint32_t rx_packets;    /* playback packets received */
    uint32_t level;         /* playback FIFO bytes now */
    uint32_t rate_sets;     /* sample-rate SET CUR requests (only 48000 is accepted) */
};

void usb_audio_status(struct usb_audio_status *st);

#endif
