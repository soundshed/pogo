#include "ScreenPreview.h"
#include "ScreenImage.h"

ScreenPreview::ScreenPreview()
    : framebuffer (MIDI_CONTROL_LCD_RGB565_BYTES, 0)
{
    midi_control_gfx_init (&gfx, framebuffer.data(), MIDI_CONTROL_GFX_RGB565, 0);

    /* Storage callbacks that report the state they are told and hold nothing:
     * the status chips then match the device, and the device starts from the
     * defaults until the first render gives it the configuration. */
    midi_control_platform_ops ops {};
    ops.ctx = this;
    ops.storage_state = storageStateThunk;
    ops.storage_read = storageReadThunk;
    ops.storage_write = storageWriteThunk;
    ops.platform = MIDI_CONTROL_PLATFORM_EMULATOR;
    midi_control_device_init (&dev, &ops);
}

bool ScreenPreview::render (const midi_control_config& cfg, const midi_control_state_reply& state, bool usb)
{
    storage = state.storage_state;
    if (! midi_control_config_equal (&cfg, &dev.config))
        midi_control_device_set_config (&dev, &cfg);
    midi_control_device_mirror (&dev, &state);

    midi_control_ui_model m {};
    m.device = &dev;
    m.usb_mounted = usb ? 1 : 0;
    midi_control_rect drawn {};
    const bool changed = midi_control_ui_update (&gfx, &m, &cache, &drawn) != 0;
    if (changed || ! picture.isValid())
        copyPanelToImage (gfx, picture, drawn);
    return changed;
}

midi_control_state_reply ScreenPreview::atRest (const midi_control_config& cfg, int program, int storageState, bool dirty)
{
    midi_control_state_reply st {};
    st.storage_state = (uint8_t) storageState;
    st.dirty = dirty ? 1 : 0;
    st.program = (uint8_t) juce::jlimit (0, 127, program);
    for (int i = 0; i < MIDI_CONTROL_ENCODER_COUNT; ++i)
        st.enc_value[i] = (uint8_t) (cfg.encoders[i].value & 0x7f);
    return st;
}

int ScreenPreview::storageStateThunk (void* ctx)
{
    return static_cast<ScreenPreview*> (ctx)->storage;
}

int ScreenPreview::storageReadThunk (void*, unsigned, uint8_t*, uint32_t)
{
    return 0;
}

int ScreenPreview::storageWriteThunk (void*, unsigned, const uint8_t*, uint32_t)
{
    return MIDI_CONTROL_STATUS_WRITE_FAILED;
}
