#pragma once

#include <vector>
#include <juce_graphics/juce_graphics.h>
#include "midi_control/midi_control_device.h"
#include "midi_control/midi_control_gfx.h"
#include "midi_control/midi_control_ui.h"

/* The unit's screen and LEDs, drawn by the shared UI code from a device of the
 * app's own that sends nothing and stores nothing. Given a configuration and
 * the state a device reported, it mirrors that device, so the picture is what
 * the device shows now; given `atRest`, it is what the configuration puts on a
 * unit nobody is touching. */
class ScreenPreview
{
public:
    ScreenPreview();

    /* Redraws only the regions that changed. Returns true when the picture did. */
    bool render (const midi_control_config& cfg, const midi_control_state_reply& state, bool usb);
    const juce::Image& image() const { return picture; }

    /* The device the picture is of: its LEDs, and which buttons count as active. */
    const midi_control_device& device() const { return dev; }

    /* A unit at `program`, with nothing held, no knob moved and no host text. */
    static midi_control_state_reply atRest (const midi_control_config& cfg, int program, int storageState, bool dirty);

private:
    static int storageStateThunk (void* ctx);
    static int storageReadThunk (void* ctx, unsigned slot, uint8_t* buf, uint32_t len);
    static int storageWriteThunk (void* ctx, unsigned slot, const uint8_t* buf, uint32_t len);

    midi_control_device dev {};
    std::vector<uint8_t> framebuffer;
    midi_control_gfx gfx {};
    midi_control_ui_cache cache {};
    juce::Image picture;
    int storage = MIDI_CONTROL_STORAGE_NONE;

    JUCE_DECLARE_NON_COPYABLE (ScreenPreview)
};
