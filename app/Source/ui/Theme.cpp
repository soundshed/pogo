#include "Theme.h"

namespace theme
{
juce::Colour led (int bits)
{
    static const juce::uint32 k[8] = {
        0x00000000, 0xffff483c, 0xff46dc64, 0xffffce32, 0xff4682ff, 0xffe25aec, 0xff32d6ec, 0xfff0f4f8
    };
    return juce::Colour (k[bits & 7]);
}

juce::String ledName (int bits)
{
    static const char* const k[8] = { "Off", "Red", "Green", "Yellow", "Blue", "Magenta", "Cyan", "White" };
    return k[bits & 7];
}

juce::Font ui (float height, bool bold)
{
   #if JUCE_WINDOWS
    return juce::Font (juce::FontOptions ("Segoe UI", bold ? "Semibold" : "Regular", height));
   #else
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
   #endif
}

juce::Font heading (float height, bool bold)
{
   #if JUCE_WINDOWS
    return juce::Font (juce::FontOptions ("Bahnschrift", bold ? "SemiBold" : "Regular", height));
   #else
    return ui (height, bold);
   #endif
}

juce::Font mono (float height)
{
   #if JUCE_WINDOWS
    return juce::Font (juce::FontOptions ("Consolas", height, juce::Font::plain));
   #else
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height, juce::Font::plain));
   #endif
}

float textWidth (const juce::Font& font, const juce::String& text)
{
    return juce::GlyphArrangement::getStringWidth (font, text);
}

static juce::Font chipFont (float height)
{
    return heading (height * 0.52f, true);
}

float chipWidth (const juce::String& text, float height, bool dot)
{
    return textWidth (chipFont (height), text) + height * 0.9f + (dot ? height * 0.55f : 0.0f);
}

void drawChip (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text, juce::Colour textColour,
               juce::Colour dot, juce::Colour fill)
{
    const float h = area.getHeight();
    g.setColour (fill);
    g.fillRoundedRectangle (area, h / 2.0f);
    g.setColour (rule);
    g.drawRoundedRectangle (area.reduced (0.5f), h / 2.0f, 1.0f);

    auto inner = area.reduced (h * 0.45f, 0.0f);
    if (! dot.isTransparent())
    {
        const float d = h * 0.3f;
        const juce::Point<float> c { inner.getX() + d * 0.5f, area.getCentreY() };
        g.setColour (dot.withAlpha (0.25f));
        g.fillEllipse (juce::Rectangle<float> (d * 2.0f, d * 2.0f).withCentre (c));
        g.setColour (dot);
        g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (c));
        inner.removeFromLeft (h * 0.55f);
    }
    g.setColour (textColour);
    g.setFont (chipFont (h));
    g.drawText (text, inner, juce::Justification::centredLeft, false);
}

void setVariant (juce::Component& c, const juce::String& variant)
{
    c.getProperties().set ("variant", variant);
    c.repaint();
}
} // namespace theme

/* ---- AppLookAndFeel ------------------------------------------------------ */

AppLookAndFeel::AppLookAndFeel()
{
    using S = juce::LookAndFeel_V4::ColourScheme;
    auto scheme = getDarkColourScheme();
    scheme.setUIColour (S::windowBackground, theme::window);
    scheme.setUIColour (S::widgetBackground, theme::field);
    scheme.setUIColour (S::menuBackground, theme::raised);
    scheme.setUIColour (S::outline, theme::ruleStrong);
    scheme.setUIColour (S::defaultText, theme::ink);
    scheme.setUIColour (S::defaultFill, theme::accent);
    scheme.setUIColour (S::highlightedText, theme::ink);
    scheme.setUIColour (S::highlightedFill, theme::accent.withAlpha (0.28f));
    scheme.setUIColour (S::menuText, theme::ink);
    setColourScheme (scheme);

    const std::pair<int, juce::Colour> palette[] = {
        { juce::ResizableWindow::backgroundColourId, theme::window },
        { juce::DocumentWindow::textColourId, theme::ink },
        { juce::Label::textColourId, theme::ink },
        { juce::TextButton::buttonColourId, theme::field },
        { juce::TextButton::buttonOnColourId, theme::accent },
        { juce::TextButton::textColourOffId, theme::ink },
        { juce::TextButton::textColourOnId, theme::onAccent },
        { juce::ToggleButton::textColourId, theme::ink },
        { juce::ToggleButton::tickColourId, theme::accent },
        { juce::ComboBox::backgroundColourId, theme::field },
        { juce::ComboBox::textColourId, theme::ink },
        { juce::ComboBox::outlineColourId, theme::ruleStrong },
        { juce::ComboBox::arrowColourId, theme::inkDim },
        { juce::ComboBox::focusedOutlineColourId, theme::accent },
        { juce::PopupMenu::backgroundColourId, theme::raised },
        { juce::PopupMenu::textColourId, theme::ink },
        { juce::PopupMenu::highlightedBackgroundColourId, theme::accent.withAlpha (0.18f) },
        { juce::PopupMenu::highlightedTextColourId, theme::ink },
        { juce::TextEditor::backgroundColourId, theme::field },
        { juce::TextEditor::textColourId, theme::ink },
        { juce::TextEditor::highlightColourId, theme::accent.withAlpha (0.3f) },
        { juce::TextEditor::highlightedTextColourId, theme::ink },
        { juce::TextEditor::outlineColourId, theme::ruleStrong },
        { juce::TextEditor::focusedOutlineColourId, theme::accent },
        { juce::CaretComponent::caretColourId, theme::accent },
        { juce::ListBox::backgroundColourId, theme::surface },
        { juce::ListBox::outlineColourId, juce::Colours::transparentBlack },
        { juce::ListBox::textColourId, theme::ink },
        { juce::ScrollBar::thumbColourId, theme::ruleStrong },
        { juce::Slider::backgroundColourId, theme::field },
        { juce::Slider::trackColourId, theme::accent },
        { juce::Slider::thumbColourId, theme::ink },
        { juce::Slider::textBoxTextColourId, theme::ink },
        { juce::Slider::textBoxBackgroundColourId, theme::field },
        { juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack },
        { juce::Slider::textBoxHighlightColourId, theme::accent.withAlpha (0.3f) },
        { juce::AlertWindow::backgroundColourId, theme::surface },
        { juce::AlertWindow::textColourId, theme::ink },
        { juce::AlertWindow::outlineColourId, theme::ruleStrong },
        { juce::TooltipWindow::backgroundColourId, theme::raised },
        { juce::TooltipWindow::textColourId, theme::ink },
        { juce::TooltipWindow::outlineColourId, theme::ruleStrong },
    };
    for (const auto& [id, colour] : palette)
        setColour (id, colour);

   #if JUCE_WINDOWS
    setDefaultSansSerifTypefaceName ("Segoe UI");
   #endif
}

juce::Font AppLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return theme::ui (juce::jlimit (12.0f, 15.0f, (float) buttonHeight * 0.46f), true);
}

void AppLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                                           bool over, bool down)
{
    const auto variant = b.getProperties()["variant"].toString();
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const float radius = juce::jmin (8.0f, r.getHeight() / 2.0f);
    const bool on = b.getToggleState();

    if (variant == "nav")
    {
        if (on || over)
        {
            g.setColour (on ? theme::field : theme::raised);
            g.fillRoundedRectangle (r, radius);
        }
        if (on)
        {
            g.setColour (theme::accent);
            g.fillRoundedRectangle (r.removeFromBottom (3.0f).reduced (r.getWidth() * 0.3f, 0.0f), 1.5f);
        }
        return;
    }

    juce::Colour fill = theme::field, edge = theme::ruleStrong;
    if (variant == "primary")
    {
        fill = theme::accent;
        edge = theme::accent.brighter (0.25f);
    }
    else if (variant == "danger")
    {
        fill = theme::bad.withAlpha (0.12f);
        edge = theme::bad.withAlpha (0.65f);
    }
    else if (variant == "ghost")
    {
        fill = juce::Colours::transparentBlack;
        edge = theme::rule;
    }
    else if (variant == "segment")
    {
        fill = on ? theme::accent.withAlpha (0.16f) : theme::field;
        edge = on ? theme::accent : theme::rule;
    }

    if (! b.isEnabled())
    {
        fill = fill.withMultipliedAlpha (0.4f);
        edge = edge.withMultipliedAlpha (0.4f);
    }
    else if (down)
        fill = variant == "ghost" ? theme::field : fill.darker (0.18f);
    else if (over)
        fill = variant == "ghost" ? theme::raised : fill.brighter (0.1f);

    g.setColour (fill);
    g.fillRoundedRectangle (r, radius);
    g.setColour (edge);
    g.drawRoundedRectangle (r, radius, 1.0f);
}

void AppLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    const auto variant = b.getProperties()["variant"].toString();
    juce::Colour c = theme::ink;
    if (variant == "primary")
        c = theme::onAccent;
    else if (variant == "nav")
        c = b.getToggleState() ? theme::ink : theme::inkDim;
    else if (variant == "danger")
        c = theme::bad.brighter (0.35f);
    else if (variant == "segment" && b.getToggleState())
        c = theme::accent;
    if (! b.isEnabled())
        c = c.withMultipliedAlpha (0.45f);

    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.setColour (c);
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (10, 2), juce::Justification::centred, 1, 0.9f);
}

void AppLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool)
{
    auto r = b.getLocalBounds().toFloat();
    const float h = juce::jmin (20.0f, r.getHeight() - 4.0f);
    const float w = h * 1.8f;
    const auto track = juce::Rectangle<float> (w, h).withPosition (r.getX() + 1.0f, r.getCentreY() - h / 2.0f);
    const bool on = b.getToggleState();
    const float alpha = b.isEnabled() ? 1.0f : 0.4f;

    g.setColour ((on ? theme::accent : (over ? theme::ruleStrong : theme::field)).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (track, h / 2.0f);
    g.setColour ((on ? theme::accent.brighter (0.25f) : theme::ruleStrong).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (track.reduced (0.5f), h / 2.0f, 1.0f);

    const float d = h - 6.0f;
    const juce::Point<float> c { on ? track.getRight() - h / 2.0f : track.getX() + h / 2.0f, track.getCentreY() };
    g.setColour ((on ? juce::Colours::white : theme::inkDim).withMultipliedAlpha (alpha));
    g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (c));

    if (b.getButtonText().isNotEmpty())
    {
        g.setColour (theme::ink.withMultipliedAlpha (alpha));
        g.setFont (theme::ui (14.0f));
        g.drawFittedText (b.getButtonText(), r.withTrimmedLeft (w + 10.0f).toNearestInt(),
                          juce::Justification::centredLeft, 2);
    }
}

void AppLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int,
                                   juce::ComboBox& box)
{
    auto r = juce::Rectangle<float> ((float) width, (float) height).reduced (0.5f);
    const float alpha = box.isEnabled() ? 1.0f : 0.45f;
    g.setColour (theme::field.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (r, 7.0f);
    g.setColour ((box.hasKeyboardFocus (true) ? theme::accent : theme::ruleStrong).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (r, 7.0f, 1.0f);

    const auto arrow = r.removeFromRight (28.0f).withSizeKeepingCentre (9.0f, 5.0f);
    juce::Path p;
    p.startNewSubPath (arrow.getX(), arrow.getY());
    p.lineTo (arrow.getCentreX(), arrow.getBottom());
    p.lineTo (arrow.getRight(), arrow.getY());
    g.setColour (theme::inkDim.withMultipliedAlpha (alpha));
    g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font AppLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return theme::ui (14.0f);
}

void AppLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (8, 1, box.getWidth() - 34, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

void AppLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (theme::raised);
    g.setColour (theme::ruleStrong);
    g.drawRect (0, 0, width, height, 1);
}

void AppLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator,
                                        bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu,
                                        const juce::String& text, const juce::String& shortcutKeyText,
                                        const juce::Drawable* icon, const juce::Colour* textColour)
{
    juce::ignoreUnused (hasSubMenu, icon);
    if (isSeparator)
    {
        g.setColour (theme::rule);
        g.fillRect (area.withSizeKeepingCentre (area.getWidth() - 16, 1));
        return;
    }

    auto r = area.reduced (4, 1);
    if (isHighlighted && isActive)
    {
        g.setColour (theme::accent.withAlpha (0.16f));
        g.fillRoundedRectangle (r.toFloat(), 5.0f);
    }
    const auto tick = r.removeFromLeft (22).toFloat();
    if (isTicked)
    {
        g.setColour (theme::accent);
        g.fillEllipse (tick.withSizeKeepingCentre (7.0f, 7.0f));
    }
    g.setColour (textColour != nullptr ? *textColour : (isActive ? theme::ink : theme::inkFaint));
    g.setFont (getPopupMenuFont());
    g.drawFittedText (text, r.reduced (2, 0), juce::Justification::centredLeft, 1);
    if (shortcutKeyText.isNotEmpty())
    {
        g.setColour (theme::inkFaint);
        g.drawText (shortcutKeyText, r.reduced (8, 0), juce::Justification::centredRight, true);
    }
}

juce::Font AppLookAndFeel::getPopupMenuFont()
{
    return theme::ui (14.5f);
}

void AppLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                       float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                                       juce::Slider& s)
{
    const bool two = style == juce::Slider::TwoValueHorizontal;
    if (style != juce::Slider::LinearHorizontal && ! two)
    {
        juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos,
                                                style, s);
        return;
    }

    const float cy = (float) y + (float) height * 0.5f;
    const float alpha = s.isEnabled() ? 1.0f : 0.4f;
    g.setColour (theme::field.brighter (0.08f).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle ((float) x, cy - 2.0f, (float) width, 4.0f, 2.0f);

    const float from = two ? minSliderPos : (float) x;
    const float to = two ? maxSliderPos : sliderPos;
    g.setColour (theme::accent.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (from, cy - 2.0f, juce::jmax (0.0f, to - from), 4.0f, 2.0f);

    auto thumb = [&] (float px)
    {
        const auto t = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ px, cy });
        g.setColour (juce::Colours::black.withAlpha (0.35f * alpha));
        g.fillEllipse (t.translated (0.0f, 1.0f).expanded (1.0f));
        g.setColour (theme::ink.withMultipliedAlpha (alpha));
        g.fillEllipse (t);
        g.setColour (theme::accent.withMultipliedAlpha (alpha));
        g.drawEllipse (t.reduced (0.75f), 1.5f);
    };
    if (two)
    {
        thumb (minSliderPos);
        thumb (maxSliderPos);
    }
    else
        thumb (sliderPos);
}

int AppLookAndFeel::getSliderThumbRadius (juce::Slider&)
{
    return 7;
}

int AppLookAndFeel::getDefaultScrollbarWidth()
{
    return 10;
}

void AppLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height,
                                    bool vertical, int thumbStart, int thumbSize, bool over, bool down)
{
    const auto thumb = vertical ? juce::Rectangle<int> (x, thumbStart, width, thumbSize)
                                : juce::Rectangle<int> (thumbStart, y, thumbSize, height);
    const auto t = thumb.toFloat().reduced (vertical ? 2.5f : 1.0f, vertical ? 1.0f : 2.5f);
    g.setColour ((over || down) ? theme::inkFaint : theme::ruleStrong);
    g.fillRoundedRectangle (t, juce::jmin (t.getWidth(), t.getHeight()) / 2.0f);
}

void AppLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& te)
{
    const auto bg = te.findColour (juce::TextEditor::backgroundColourId);
    if (dynamic_cast<juce::AlertWindow*> (te.getParentComponent()) != nullptr)
    {
        g.fillAll (bg);
        return;
    }
    g.setColour (bg);
    g.fillRoundedRectangle (juce::Rectangle<float> ((float) width, (float) height), 7.0f);
}

void AppLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& te)
{
    if (! te.isEnabled())
        return;
    const auto c = te.hasKeyboardFocus (true) && ! te.isReadOnly()
                       ? te.findColour (juce::TextEditor::focusedOutlineColourId)
                       : te.findColour (juce::TextEditor::outlineColourId);
    if (c.isTransparent())
        return;
    g.setColour (c);
    g.drawRoundedRectangle (juce::Rectangle<float> ((float) width, (float) height).reduced (0.5f), 7.0f, 1.0f);
}

static juce::TextLayout tooltipLayout (const juce::String& text, juce::Colour colour)
{
    juce::AttributedString s;
    s.setJustification (juce::Justification::centredLeft);
    s.append (text, theme::ui (13.5f), colour);
    juce::TextLayout layout;
    layout.createLayoutWithBalancedLineLengths (s, 340.0f);
    return layout;
}

juce::Rectangle<int> AppLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                                       juce::Rectangle<int> parentArea)
{
    const auto layout = tooltipLayout (tipText, juce::Colours::black);
    const int w = (int) (layout.getWidth() + 22.0f);
    const int h = (int) (layout.getHeight() + 14.0f);
    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 20,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 8) : screenPos.y + 8,
                                 w, h)
               .constrainedWithin (parentArea);
}

void AppLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto bounds = juce::Rectangle<int> (width, height).toFloat();
    g.fillAll (theme::raised);
    g.setColour (theme::ruleStrong);
    g.drawRect (bounds, 1.0f);
    tooltipLayout (text, theme::ink).draw (g, bounds.reduced (11.0f, 7.0f));
}

juce::Font AppLookAndFeel::getLabelFont (juce::Label& label)
{
    return label.getFont();
}

juce::Font AppLookAndFeel::getAlertWindowTitleFont()
{
    return theme::heading (20.0f);
}

juce::Font AppLookAndFeel::getAlertWindowMessageFont()
{
    return theme::ui (14.5f);
}

juce::Font AppLookAndFeel::getAlertWindowFont()
{
    return theme::ui (14.0f);
}
