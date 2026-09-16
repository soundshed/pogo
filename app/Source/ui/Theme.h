#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/* The app's look. The palette is the unit's own screen (shared/src/midi_control_ui.c:
 * the same accent, the same green for saved, the same LED colours) set on the
 * charcoal of the chassis, so the window, the drawn unit and its screen read
 * as one thing. */
namespace theme
{
    /* Surfaces, darkest first. */
    inline const juce::Colour window     { 0xff0e1013 };
    inline const juce::Colour surface    { 0xff161a1f };
    inline const juce::Colour raised     { 0xff1d2127 };
    inline const juce::Colour field      { 0xff252a31 };
    inline const juce::Colour rule       { 0xff2b3038 };
    inline const juce::Colour ruleStrong { 0xff3a414b };

    inline const juce::Colour ink      { 0xffeceff4 };
    inline const juce::Colour inkDim   { 0xff96a0ac };
    inline const juce::Colour inkFaint { 0xff5c6570 };

    /* ACCENT, GOOD and WARN on the unit's screen. */
    inline const juce::Colour accent { 0xffffa826 };
    inline const juce::Colour good   { 0xff46d66e };
    inline const juce::Colour warn   { 0xffffc428 };
    inline const juce::Colour bad    { 0xffff5a4a };
    inline const juce::Colour info   { 0xff52b8f5 };

    /* Text on an accent fill. */
    inline const juce::Colour onAccent { 0xff1c1305 };

    /* A footswitch LED's colour for its three bits, as the screen draws it.
     * 0 (unlit) gives a transparent colour. */
    juce::Colour led (int bits);
    juce::String ledName (int bits);

    /* Interface text. */
    juce::Font ui (float height, bool bold = false);
    /* Headings and the unit's printed labels: a DIN-like face, as the panel's is. */
    juce::Font heading (float height, bool bold = true);
    juce::Font mono (float height);

    float textWidth (const juce::Font& font, const juce::String& text);

    /* A rounded tag with an optional dot, like the ones along the top of the
     * unit's screen. Returns the width it takes at `height`. */
    float chipWidth (const juce::String& text, float height, bool dot);
    void drawChip (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text,
                   juce::Colour textColour, juce::Colour dot = {}, juce::Colour fill = raised);

    /* Button styles, set as a component property: "primary", "danger", "ghost",
     * "nav" (the header's tabs), "segment" (one of a group of choices). */
    void setVariant (juce::Component& c, const juce::String& variant);
}

class AppLookAndFeel : public juce::LookAndFeel_V4
{
public:
    AppLookAndFeel();

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon,
                            const juce::Colour* textColour) override;
    juce::Font getPopupMenuFont() override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle,
                           juce::Slider&) override;
    int getSliderThumbRadius (juce::Slider&) override;

    int getDefaultScrollbarWidth() override;
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                        bool isScrollbarVertical, int thumbStartPosition, int thumbSize, bool isMouseOver,
                        bool isMouseDown) override;

    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;

    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getAlertWindowTitleFont() override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowFont() override;
};
