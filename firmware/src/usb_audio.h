#ifndef MIDI_CONTROL_USB_AUDIO_H
#define MIDI_CONTROL_USB_AUDIO_H

/* USB audio between the host and the SHARC link (docs/10 section 8): Audio
 * Class 2.0 at high speed, Audio Class 1.0 at full speed (usb_descriptors.c).
 *
 * The unit is a 48 kHz two-in two-out interface, 24-bit at high speed and
 * 16-bit at full speed. Playback is an asynchronous isochronous OUT endpoint
 * with an explicit feedback endpoint; the record stream is an asynchronous
 * isochronous IN endpoint. The sample
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
#define USB_ITF_AUDIO_CONTROL   0u      /* first: docs/03 section 1 */
#define USB_ITF_AUDIO_PLAY      1u
#define USB_ITF_AUDIO_RECORD    2u
#define USB_ITF_AUDIO_COUNT     3u

#define USB_EP_AUDIO_PLAY       0x04u   /* isochronous OUT */
#define USB_EP_AUDIO_FEEDBACK   0x84u   /* isochronous IN: 16.16 per microframe in 4 bytes (high), 10.14 per frame in 3 (full) */
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

/* At full speed, 16 bits in 2 bytes: Windows' Audio 1.0 driver makes the
 * devices for 24 bits in 4 but cannot open a stream on them (measured
 * 2026-11-15, docs/10 section 8). */
#define USB_AUDIO_SUBSLOT_FS     2u
#define USB_AUDIO_BITS_FS        16u
#define USB_AUDIO_FRAME_BYTES_FS (MIDI_CONTROL_AUDIO_HOST_CHANNELS * USB_AUDIO_SUBSLOT_FS)

/* Largest packet: one more frame than a packet carries at 48 kHz - 6 each
 * high-speed microframe, 48 each full-speed frame. */
#define USB_AUDIO_EP_SIZE       ((USB_AUDIO_RATE / 8000u + 1u) * USB_AUDIO_FRAME_BYTES)
#define USB_AUDIO_EP_SIZE_FS    ((USB_AUDIO_RATE / 1000u + 1u) * USB_AUDIO_FRAME_BYTES_FS)

/* Playback starts once this much waits in the FIFO, and the feedback's trim
 * holds the FIFO near it. At high speed 0.5 ms, four packets, the playback
 * latency the unit adds. At full speed a packet is 1 ms, so the FIFO's
 * sawtooth is 48 frames deep: 2 ms keeps about 1.5 ms in hand at its
 * lowest. */
#define USB_AUDIO_PLAY_TARGET_FRAMES     24u
#define USB_AUDIO_PLAY_TARGET_FRAMES_FS  96u

/* TinyUSB sizes each record packet to hold the record FIFO near this when the
 * packet is made. At high speed a packet's 6 frames plus 6 more, so about
 * 125 us of guitar waits behind each packet sent. At full speed it has to be
 * more than the 47 frames of the smallest packet, or TinyUSB sends an empty
 * one: 60 frames, so about 250 us waits behind each. Both in frames; the
 * bytes follow the speed's format. */
#define USB_AUDIO_RECORD_THRESHOLD_FRAMES     12u
#define USB_AUDIO_RECORD_THRESHOLD_FRAMES_FS  60u

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
    uint32_t feedback;      /* last feedback, 16.16 frames per microframe */
    uint32_t fb_min, fb_max;    /* its range once the rate window filled; 0 before */
    uint32_t rate_mhz;      /* the link's rate measured against the host, mHz */
};

void usb_audio_status(struct usb_audio_status *st);

#endif
