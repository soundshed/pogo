/* USB audio, Audio Class 2.0 at high speed and 1.0 at full speed - see
 * usb_audio.h and docs/10 section 8. */

#include "usb_audio.h"

#include "tusb.h"
#include "log.h"
#include "i2s.h"

#if CFG_TUD_AUDIO

enum { PLAY_CLOSED, PLAY_FILLING, PLAY_RUNNING };

/* Written by the main loop with interrupts off, and by the link's interrupt. */
static volatile uint8_t g_play;
static volatile uint8_t g_record;

static volatile uint32_t g_play_frames;
static volatile uint32_t g_underruns;
static volatile uint32_t g_record_frames;
static volatile uint32_t g_rx_packets;
static uint32_t g_rate_sets;

/* What each host does with the packets, to tell a click's cause from the
 * counters (docs/10 section 8). Sizes are in frames against the speed's
 * nominal, 6 a microframe or 48 a frame: empty, one short, nominal, one long,
 * anything else. Received are the host's playback packets; sent are the
 * record packets. */
enum { SIZE_EMPTY, SIZE_SHORT, SIZE_NOMINAL, SIZE_LONG, SIZE_OTHER, SIZE_CLASSES };
static volatile uint32_t g_rx_sizes[SIZE_CLASSES];
static volatile uint32_t g_tx_sizes[SIZE_CLASSES];
static volatile uint32_t g_rec_drops;       /* record frames the FIFO had no room for */
static volatile uint32_t g_rec_min;         /* the record FIFO's level at each packet, frames, */
static volatile uint32_t g_rec_max;         /*   since the stream opened */
static volatile uint32_t g_play_min;        /* the playback FIFO's lowest level while playing, frames, since it opened */
static volatile uint32_t g_underrun_s;      /* the link's seconds at the last underrun */

/* The feedback: written in the USB interrupt, reset by the main loop with
 * interrupts off. */
static struct midi_control_audio_rate g_rate;

/* The bus speed's playback target and frame size, set when a stream opens: the
 * link's interrupt waits for the target before it plays (usb_audio.h), and a
 * frame is two 4-byte subslots at high speed, two 16-bit samples at full
 * speed. */
static volatile uint32_t g_play_target_frames = USB_AUDIO_PLAY_TARGET_FRAMES;
static volatile uint8_t g_fs_format;

/* At full speed: the first start-of-frame indexes seen, logged once from the
 * main loop, since how this controller counts FRINDEX at full speed is taken
 * from the manual and not yet seen. */
#define FS_SOF_SEEN 4u
static volatile uint16_t g_fs_sof[FS_SOF_SEEN];
static volatile uint8_t g_fs_sof_count;
static uint8_t g_fs_sof_logged;

static uint32_t now_s(void);

static bool full_speed(void)
{
    return tud_speed_get() == TUSB_SPEED_FULL;
}

/* The feedback is computed in frames a 125 us microframe, 16.16, at either
 * speed. At high speed that is what the host is sent. At full speed the host
 * wants frames a 1 ms frame: eight times as many, which TinyUSB then sends as
 * Audio 1.0's 10.14 in 3 bytes (the value shifted right by 2). */
static void feedback_send(uint32_t per_microframe)
{
    tud_audio_fb_set(full_speed() ? per_microframe * 8u : per_microframe);
}

static inline uint32_t irq_save(void)
{
    uint32_t primask;
    __asm__ volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask) :: "memory");
    return primask;
}

static inline void irq_restore(uint32_t primask)
{
    __asm__ volatile("msr primask, %0" :: "r"(primask) : "memory");
}

/* ---- the link's interrupt ------------------------------------------------ */

static uint32_t frame_bytes(void)
{
    return g_fs_format ? USB_AUDIO_FRAME_BYTES_FS : USB_AUDIO_FRAME_BYTES;
}

static unsigned size_class(uint32_t n_bytes)
{
    uint32_t fb = frame_bytes();
    uint32_t nominal = g_fs_format ? USB_AUDIO_RATE / 1000u : USB_AUDIO_RATE / 8000u;

    if (n_bytes == 0u)
        return SIZE_EMPTY;
    if (n_bytes % fb != 0u)
        return SIZE_OTHER;
    uint32_t frames = n_bytes / fb;
    if (frames + 1u == nominal)
        return SIZE_SHORT;
    if (frames == nominal)
        return SIZE_NOMINAL;
    if (frames == nominal + 1u)
        return SIZE_LONG;
    return SIZE_OTHER;
}

void usb_audio_frame(struct midi_control_audio *a)
{
    if (g_record) {
        if (g_fs_format) {
            uint16_t w[MIDI_CONTROL_AUDIO_HOST_CHANNELS];
            for (unsigned c = 0; c < MIDI_CONTROL_AUDIO_HOST_CHANNELS; c++)
                w[c] = midi_control_audio_to_s16(a->capture[c]);
            if (tud_audio_write(w, sizeof(w)) != sizeof(w))
                g_rec_drops++;
        } else {
            uint32_t w[MIDI_CONTROL_AUDIO_HOST_CHANNELS];
            for (unsigned c = 0; c < MIDI_CONTROL_AUDIO_HOST_CHANNELS; c++)
                w[c] = midi_control_audio_to_subslot(a->capture[c]);
            if (tud_audio_write(w, sizeof(w)) != sizeof(w))
                g_rec_drops++;
        }
        g_record_frames++;
    }

    switch (g_play) {
    case PLAY_FILLING:
        /* Silence until the FIFO holds the target, so the first frames out are
         * not the ones that run it dry. */
        a->host = 1u;
        a->host_in[0] = a->host_in[1] = 0;
        if (tud_audio_available() >= g_play_target_frames * frame_bytes())
            g_play = PLAY_RUNNING;
        break;

    case PLAY_RUNNING:
        a->host = 1u;
        /* This interrupt is the only reader, so what is available stays so.
         * The writer only ever adds whole frames. */
        uint32_t avail = tud_audio_available();
        if (avail >= frame_bytes()) {
            avail = avail / frame_bytes() - 1u;
            if (avail < g_play_min)
                g_play_min = avail;
            if (g_fs_format) {
                uint16_t w[MIDI_CONTROL_AUDIO_HOST_CHANNELS];
                tud_audio_read(w, sizeof(w));
                for (unsigned c = 0; c < MIDI_CONTROL_AUDIO_HOST_CHANNELS; c++)
                    a->host_in[c] = midi_control_audio_from_s16(w[c]);
            } else {
                uint32_t w[MIDI_CONTROL_AUDIO_HOST_CHANNELS];
                tud_audio_read(w, sizeof(w));
                for (unsigned c = 0; c < MIDI_CONTROL_AUDIO_HOST_CHANNELS; c++)
                    a->host_in[c] = midi_control_audio_from_subslot(w[c]);
            }
            g_play_frames++;
        } else {
            a->host_in[0] = a->host_in[1] = 0;
            g_underruns++;
            g_underrun_s = now_s();
            g_play = PLAY_FILLING;
        }
        break;

    default:
        a->host = 0u;
        break;
    }
}

/* ---- the USB interrupt --------------------------------------------------- */

bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id,
                           uint8_t ep_out, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;
    g_rx_packets++;
    g_rx_sizes[size_class(n_bytes_received)]++;
    return true;
}

/* After each record packet: n_bytes_sent is the packet that went, and the
 * next is already taken from the FIFO, so its level now is what waits behind
 * the packet. */
bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent, uint8_t func_id,
                           uint8_t ep_in, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)func_id;
    (void)ep_in;
    (void)cur_alt_setting;
    g_tx_sizes[size_class(n_bytes_sent)]++;
    uint32_t level = tu_fifo_count(tud_audio_get_ep_in_ff()) / frame_bytes();
    if (level < g_rec_min)
        g_rec_min = level;
    if (level > g_rec_max)
        g_rec_max = level;
    return true;
}

/* Each start of frame while playback is open (TinyUSB calls this from its SOF
 * handler because the feedback method below is a frequency one). The frame
 * number is the controller's FRINDEX, in microframes: bits 13..3 are the frame
 * number at either speed (LPC43xx user manual, USB0 FRINDEX). At full speed
 * there are no microframes, so bits 2..0 are dropped, and FRINDEX counts 8 a
 * frame as the rate measurement expects. */
void tud_audio_feedback_interval_isr(uint8_t func_id, uint32_t frame_number, uint8_t interval_shift)
{
    (void)func_id;
    (void)interval_shift;
    if (full_speed()) {
        if (g_fs_sof_count < FS_SOF_SEEN)
            g_fs_sof[g_fs_sof_count++] = (uint16_t)frame_number;
        frame_number &= ~7u;
    }
    uint32_t fb = midi_control_audio_rate_sof(&g_rate, (uint16_t)frame_number,
                                              i2s_audio()->frames,
                                              tud_audio_available() / frame_bytes(),
                                              g_play_target_frames);
    feedback_send(fb);
}

/* ---- the main loop (TinyUSB's task) -------------------------------------- */

/* A trace of what each host did, read back afterwards from another (docs/10
 * section 8): every line carries the seconds since the link started, from its
 * frame count, since the log has no clock of its own. */
static uint32_t now_s(void)
{
    return i2s_audio()->frames / USB_AUDIO_RATE;
}

void tud_mount_cb(void)
{
    log_dec("usb: configured, s", now_s());
    log_dec("usb: speed (2 high, 0 full)", (uint32_t)tud_speed_get());
}

void tud_umount_cb(void)
{
    log_dec("usb: unconfigured, s", now_s());
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    log_dec("usb: suspended, s", now_s());
}

void tud_resume_cb(void)
{
    log_dec("usb: resumed, s", now_s());
}

/* A request the function does not answer, which the host sees as a stall:
 * bRequest, the control selector (wValue's high byte) and wIndex (entity or
 * endpoint, and interface). */
static bool refused(const char *what, tusb_control_request_t const *request)
{
    log_hex(what, ((uint32_t)request->bRequest << 24) |
                  ((uint32_t)tu_u16_high(request->wValue) << 16) | request->wIndex);
    return false;
}

bool tud_audio_get_req_itf_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    return refused("usb audio: refused get itf, req/cs/wIndex", request);
}

bool tud_audio_set_req_itf_cb(uint8_t rhport, tusb_control_request_t const *request, uint8_t *buf)
{
    (void)rhport;
    (void)buf;
    return refused("usb audio: refused set itf, req/cs/wIndex", request);
}

/* Endpoint requests come only at full speed, Audio 1.0: each data endpoint's
 * sampling frequency, 3 bytes. Its one value is 48000. */
static bool is_data_ep(tusb_control_request_t const *request)
{
    uint8_t ep = tu_u16_low(request->wIndex);
    return ep == USB_EP_AUDIO_PLAY || ep == USB_EP_AUDIO_RECORD;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    if (is_data_ep(request) && tu_u16_high(request->wValue) == AUDIO10_EP_CTRL_SAMPLING_FREQ &&
        (request->bRequest == AUDIO10_CS_REQ_GET_CUR || request->bRequest == AUDIO10_CS_REQ_GET_MIN ||
         request->bRequest == AUDIO10_CS_REQ_GET_MAX || request->bRequest == AUDIO10_CS_REQ_GET_RES)) {
        uint32_t hz = request->bRequest == AUDIO10_CS_REQ_GET_RES ? 0u : USB_AUDIO_RATE;
        uint8_t v[3] = { (uint8_t)hz, (uint8_t)(hz >> 8), (uint8_t)(hz >> 16) };
        log_dec("usb audio: rate asked (Audio 1.0), s", now_s());
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request, v, sizeof(v));
    }
    return refused("usb audio: refused get ep, req/cs/wIndex", request);
}

bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *request, uint8_t *buf)
{
    (void)rhport;
    if (is_data_ep(request) && tu_u16_high(request->wValue) == AUDIO10_EP_CTRL_SAMPLING_FREQ &&
        request->bRequest == AUDIO10_CS_REQ_SET_CUR && request->wLength == 3u) {
        uint32_t hz = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16);
        g_rate_sets++;
        log_dec("usb audio: rate set to (Audio 1.0)", hz);
        return hz == USB_AUDIO_RATE;
    }
    return refused("usb audio: refused set ep, req/cs/wIndex", request);
}

static void play_set(uint8_t state)
{
    uint32_t p = irq_save();
    if (state != PLAY_CLOSED) {
        g_fs_format = full_speed() ? 1u : 0u;
        g_play_target_frames = full_speed() ? USB_AUDIO_PLAY_TARGET_FRAMES_FS
                                            : USB_AUDIO_PLAY_TARGET_FRAMES;
        tud_audio_clear_ep_out_ff();
        g_play_min = UINT32_MAX;
        midi_control_audio_rate_init(&g_rate, g_play_target_frames);
        feedback_send(g_rate.feedback);
    }
    g_play = state;
    irq_restore(p);
}

static void record_set(uint8_t on)
{
    uint32_t p = irq_save();
    if (on) {
        g_fs_format = full_speed() ? 1u : 0u;
        tud_audio_clear_ep_in_ff();
        g_rec_min = UINT32_MAX;
        g_rec_max = 0u;
    }
    g_record = on;
    irq_restore(p);
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    uint8_t itf = tu_u16_low(request->wIndex);
    uint8_t alt = tu_u16_low(request->wValue);

    if (alt == 0u)
        return true;
    if (itf == USB_ITF_AUDIO_PLAY) {
        play_set(PLAY_FILLING);
        log_dec("usb audio: playback open, s", now_s());
    } else if (itf == USB_ITF_AUDIO_RECORD) {
        record_set(1u);
        /* Hold the record FIFO a packet ahead, rather than TinyUSB's half a
         * FIFO. */
        tud_audio_set_ep_in_fifo_threshold((uint16_t)((full_speed() ? USB_AUDIO_RECORD_THRESHOLD_FRAMES_FS
                                                                    : USB_AUDIO_RECORD_THRESHOLD_FRAMES) *
                                                      frame_bytes()));
        log_dec("usb audio: record open, s", now_s());
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    uint8_t itf = tu_u16_low(request->wIndex);

    if (itf == USB_ITF_AUDIO_PLAY) {
        play_set(PLAY_CLOSED);
        log_dec("usb audio: playback closed, s", now_s());
    } else if (itf == USB_ITF_AUDIO_RECORD) {
        record_set(0u);
        log_dec("usb audio: record closed, s", now_s());
    }
    return true;
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *param)
{
    (void)func_id;
    (void)alt_itf;
    /* The link's rate measured against the host's microframes, with a trim
     * from the FIFO's level (midi_control_audio_rate_sof, docs/10 section 8).
     * Not TinyUSB's FIFO_COUNT: that moves about 430 ppm for each frame the
     * FIFO's level is off, and the level steps a frame at a time, so the host
     * was told a clock that wobbled. A frequency method is named only so that
     * TinyUSB enables the SOF interrupt and calls
     * tud_audio_feedback_interval_isr(); the core clock passes its check that
     * the measurement could be precise enough, and is otherwise unused. */
    param->method = AUDIO_FEEDBACK_METHOD_FREQUENCY_FIXED;
    param->sample_freq = USB_AUDIO_RATE;
    param->frequency.mclk_freq = 204000000u;     /* the core clock */
}

/* The clock source is the only entity with controls: its frequency (one, and
 * fixed) and whether it is valid. */
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    uint8_t entity = tu_u16_high(request->wIndex);
    uint8_t control = tu_u16_high(request->wValue);

    if (entity != USB_AUDIO_ENTITY_CLOCK)
        return refused("usb audio: refused get entity, req/cs/wIndex", request);

    if (control == AUDIO20_CS_CTRL_SAM_FREQ) {
        if (request->bRequest == AUDIO20_CS_REQ_CUR) {
            log_dec("usb audio: rate asked, s", now_s());
            audio20_control_cur_4_t cur = { .bCur = (int32_t)USB_AUDIO_RATE };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &cur, sizeof(cur));
        }
        if (request->bRequest == AUDIO20_CS_REQ_RANGE) {
            log_dec("usb audio: rate range asked, s", now_s());
            audio20_control_range_4_n_t(1) range = {
                .wNumSubRanges = 1,
                .subrange = { { .bMin = (int32_t)USB_AUDIO_RATE,
                                .bMax = (int32_t)USB_AUDIO_RATE,
                                .bRes = 0 } },
            };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &range, sizeof(range));
        }
    } else if (control == AUDIO20_CS_CTRL_CLK_VALID && request->bRequest == AUDIO20_CS_REQ_CUR) {
        log_dec("usb audio: clock validity asked, s", now_s());
        audio20_control_cur_1_t valid = { .bCur = 1 };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &valid, sizeof(valid));
    }
    return refused("usb audio: refused get entity, req/cs/wIndex", request);
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request, uint8_t *buf)
{
    (void)rhport;
    uint8_t entity = tu_u16_high(request->wIndex);
    uint8_t control = tu_u16_high(request->wValue);

    if (entity != USB_AUDIO_ENTITY_CLOCK || control != AUDIO20_CS_CTRL_SAM_FREQ ||
        request->bRequest != AUDIO20_CS_REQ_CUR || request->wLength != 4u)
        return refused("usb audio: refused set entity, req/cs/wIndex", request);
    g_rate_sets++;
    log_dec("usb audio: rate set to", tu_unaligned_read32(buf));
    return tu_unaligned_read32(buf) == USB_AUDIO_RATE;
}

/* A host that has gone - unplugged, asleep, or reset - closes nothing, so the
 * outputs would stay on its silence. */
void usb_audio_task(void)
{
    if (!g_fs_sof_logged && g_fs_sof_count >= FS_SOF_SEEN) {
        g_fs_sof_logged = 1u;
        log_line("usb audio: full speed, the first FRINDEX values at a start of frame");
        for (unsigned i = 0; i < FS_SOF_SEEN; i++)
            log_hex("  FRINDEX", g_fs_sof[i]);
    }

    if (!tud_mounted() || tud_suspended()) {
        if (g_play != PLAY_CLOSED)
            play_set(PLAY_CLOSED);
        if (g_record)
            record_set(0u);
    }
}

void usb_audio_status(struct usb_audio_status *st)
{
    st->play = g_play;
    st->record = g_record;
    st->play_frames = g_play_frames;
    st->underruns = g_underruns;
    st->record_frames = g_record_frames;
    st->rx_packets = g_rx_packets;
    st->level = tud_audio_available();
    st->rate_sets = g_rate_sets;
    st->feedback = g_rate.feedback;
    st->fb_min = g_rate.fb_min;
    st->fb_max = g_rate.fb_max;
    st->rate_mhz = midi_control_audio_rate_mhz(g_rate.rate);
    for (unsigned i = 0; i < SIZE_CLASSES; i++) {
        st->rx_sizes[i] = g_rx_sizes[i];
        st->tx_sizes[i] = g_tx_sizes[i];
    }
    st->record_drops = g_rec_drops;
    st->record_min = g_rec_min == UINT32_MAX ? 0u : g_rec_min;
    st->record_max = g_rec_max;
    st->play_min = g_play_min == UINT32_MAX ? 0u : g_play_min;
    st->underrun_s = g_underrun_s;
}

#else  /* the lab image: no audio function */

void usb_audio_frame(struct midi_control_audio *a)
{
    a->host = 0u;
}

void usb_audio_task(void)
{
}

void usb_audio_status(struct usb_audio_status *st)
{
    *st = (struct usb_audio_status){ 0 };
}

#endif
