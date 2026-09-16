#pragma once

#include <juce_graphics/juce_graphics.h>
#include "midi_control/midi_control_gfx.h"

/* The panel's framebuffer as a picture: rectangle `r` of `gfx` copied into a
 * 480x272 ARGB image. An image that is not one is remade and filled whole.
 * The emulator's screen and the editor's preview both go through this, so
 * both show the pixels the unit would. */
inline void copyPanelToImage (const midi_control_gfx& gfx, juce::Image& target, midi_control_rect r)
{
    if (target.getWidth() != MIDI_CONTROL_LCD_WIDTH || target.getHeight() != MIDI_CONTROL_LCD_HEIGHT
        || target.getFormat() != juce::Image::ARGB)
    {
        target = juce::Image (juce::Image::ARGB, MIDI_CONTROL_LCD_WIDTH, MIDI_CONTROL_LCD_HEIGHT, false);
        r = { 0, 0, (int16_t) MIDI_CONTROL_LCD_WIDTH, (int16_t) MIDI_CONTROL_LCD_HEIGHT };
    }
    if (r.w <= 0 || r.h <= 0)
        return;

    juce::Image::BitmapData bd (target, r.x, r.y, r.w, r.h, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < r.h; ++y)
    {
        for (int x = 0; x < r.w; ++x)
        {
            const uint16_t p = midi_control_gfx_read (&gfx, r.x + x, r.y + y);
            const auto red = (uint8_t) (((p >> 11) & 0x1F) * 255 / 31);
            const auto green = (uint8_t) (((p >> 5) & 0x3F) * 255 / 63);
            const auto blue = (uint8_t) ((p & 0x1F) * 255 / 31);
            reinterpret_cast<juce::PixelARGB*> (bd.getPixelPointer (x, y))->setARGB (255, red, green, blue);
        }
    }
}
