#ifndef MIDI_CONTROL_I2S_H
#define MIDI_CONTROL_I2S_H

/* The LPC end of the link to the SHARC (docs/10 section 2): I2S0 as a slave,
 * eight 32-bit words a 48 kHz frame each way, every word through
 * midi_control_audio_word() in the interrupt. */

#include <stdint.h>

#include "midi_control/midi_control_audio.h"

/* Clocks, pins and registers; receive running, transmit held. Returns 0, or -1
 * if the I2S branch clock would not start (nothing else is touched then). */
int i2s_init(void);

/* Transmit on and the interrupt enabled. The link must be clocked: the SHARC
 * generates both clocks, so call this after sharc_boot() has succeeded. */
void i2s_start(void);

/* 0..255, 64 unity. Read by the interrupt at the next frame. */
void i2s_set_gain(uint16_t gain);

/* q16, on what the host records. Read by the interrupt at the next frame. */
void i2s_set_record_gain(uint32_t gain);

/* MIDI_CONTROL_AUDIO_MODE_: 0 the guitar on every output, 1..6 a tone on DAC
 * channel 1..6 alone; anything else is the monitor mode. */
void i2s_set_mode(uint8_t mode);

/* The shared state, read-only from outside the interrupt. */
const struct midi_control_audio *i2s_audio(void);

/* Interrupts that found the receive FIFO full: words may have been lost. */
uint32_t i2s_overruns(void);

/* Peak of input `slot` since the last call, then cleared. */
uint32_t i2s_peak_take(unsigned slot);

/* The host's peak, Main and Phones' peak after the gain, and how many of their
 * samples clipped, each since the last call; all three restart at 0. */
void i2s_levels_take(uint32_t *host_peak, uint32_t *knob_peak, uint32_t *knob_clips);

#endif
