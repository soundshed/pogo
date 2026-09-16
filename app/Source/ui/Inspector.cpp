#include "Inspector.h"
#include "Theme.h"

/* ---- InspectorCard ------------------------------------------------------- */

InspectorCard::InspectorCard (const juce::String& t, const juce::String& s)
    : title (t), subtitle (s)
{
}

void InspectorCard::addRow (const juce::String& label, std::unique_ptr<juce::Component> editor, int height, bool wide)
{
    addAndMakeVisible (*editor);
    Row row;
    row.label = label;
    row.editor = std::move (editor);
    row.height = height;
    row.wide = wide;
    rows.push_back (std::move (row));
}

void InspectorCard::addNote (const juce::String& text, int height)
{
    Row row;
    row.label = text;
    row.height = height;
    rows.push_back (std::move (row));
}

int InspectorCard::getIdealHeight() const
{
    int h = headerHeight + padding - 4;
    for (const auto& row : rows)
        h += row.height + (row.wide ? wideLabel : 0);
    return h;
}

void InspectorCard::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (theme::surface);
    g.fillRoundedRectangle (bounds, 12.0f);
    g.setColour (theme::rule);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 12.0f, 1.0f);

    auto header = getLocalBounds().reduced (padding, 0).removeFromTop (headerHeight);
    g.setColour (theme::ink);
    g.setFont (theme::heading (17.0f));
    g.drawText (title, header, juce::Justification::centredLeft, true);
    if (subtitle.isNotEmpty())
    {
        g.setColour (theme::inkFaint);
        g.setFont (theme::ui (12.5f));
        g.drawText (subtitle, header, juce::Justification::centredRight, true);
    }
    g.setColour (theme::rule);
    g.fillRect (padding, headerHeight - 6, getWidth() - padding * 2, 1);

    auto r = getLocalBounds().reduced (padding, 0);
    r.removeFromTop (headerHeight);
    for (const auto& row : rows)
    {
        auto line = r.removeFromTop (row.height + (row.wide ? wideLabel : 0));
        if (row.editor == nullptr)
        {
            g.setColour (theme::inkFaint);
            g.setFont (theme::ui (13.0f));
            g.drawFittedText (row.label, line, juce::Justification::centredLeft, 3, 1.0f);
            continue;
        }
        g.setColour (theme::inkDim);
        g.setFont (theme::ui (14.0f));
        if (row.wide)
            g.drawText (row.label, line.removeFromTop (wideLabel), juce::Justification::centredLeft, true);
        else
            g.drawFittedText (row.label, line.removeFromLeft (labelWidth - 8), juce::Justification::centredLeft, 2, 0.9f);
    }
}

void InspectorCard::resized()
{
    auto r = getLocalBounds().reduced (padding, 0);
    r.removeFromTop (headerHeight);
    for (auto& row : rows)
    {
        auto line = r.removeFromTop (row.height + (row.wide ? wideLabel : 0));
        if (row.editor == nullptr)
            continue;
        if (row.wide)
            line.removeFromTop (wideLabel);
        else
            line.removeFromLeft (labelWidth);
        row.editor->setBounds (line.reduced (0, 3));
    }
}

/* ---- LedPicker ------------------------------------------------------------ */

namespace
{
const char* const kLampNames[3] = { "VOL", "WAH", "TAP" };

juce::Colour lampColour (int i)
{
    return i == 2 ? juce::Colour (0xffff3b30) : juce::Colour (0xff4ae05e);
}
} // namespace

LedPicker::LedPicker (bool l)
    : lamps (l)
{
}

void LedPicker::setValue (int bits)
{
    if (bits == value)
        return;
    value = bits & 7;
    repaint();
}

juce::Rectangle<float> LedPicker::cell (int i) const
{
    const auto b = getLocalBounds().toFloat();
    if (lamps)
    {
        const float w = juce::jmin (78.0f, (b.getWidth() - 16.0f) / 3.0f);
        return { b.getX() + (float) i * (w + 8.0f), b.getY() + 2.0f, w, b.getHeight() - 4.0f };
    }
    const float gap = 6.0f;
    const float d = juce::jmin (b.getHeight() - 6.0f, (b.getWidth() - gap * 7.0f) / 8.0f);
    return { b.getX() + 3.0f + (float) i * (d + gap), b.getCentreY() - d / 2.0f, d, d };
}

int LedPicker::cellAt (juce::Point<float> p) const
{
    for (int i = 0; i < cellCount(); ++i)
        if (cell (i).expanded (3.0f).contains (p))
            return i;
    return -1;
}

void LedPicker::paint (juce::Graphics& g)
{
    for (int i = 0; i < cellCount(); ++i)
    {
        const auto r = cell (i);
        if (lamps)
        {
            const bool on = (value & (1 << i)) != 0;
            const auto c = lampColour (i);
            g.setColour (on ? c.withAlpha (0.18f) : theme::field);
            g.fillRoundedRectangle (r, 7.0f);
            g.setColour (on ? c : (i == hovered ? theme::inkFaint : theme::rule));
            g.drawRoundedRectangle (r.reduced (0.5f), 7.0f, 1.0f);
            const auto dot = juce::Rectangle<float> (14.0f, 6.0f).withCentre ({ r.getX() + 18.0f, r.getCentreY() });
            g.setColour (on ? c : theme::ruleStrong);
            g.fillRoundedRectangle (dot, 3.0f);
            g.setColour (on ? theme::ink : theme::inkDim);
            g.setFont (theme::heading (13.0f));
            g.drawText (kLampNames[i], r.withTrimmedLeft (30.0f), juce::Justification::centredLeft, false);
            continue;
        }

        const bool selected = i == value;
        if (selected)
        {
            g.setColour (theme::accent);
            g.drawEllipse (r.expanded (2.5f), 2.0f);
        }
        else if (i == hovered)
        {
            g.setColour (theme::inkFaint);
            g.drawEllipse (r.expanded (2.5f), 1.0f);
        }
        if (i == 0)
        {
            g.setColour (theme::field);
            g.fillEllipse (r.reduced (1.0f));
            g.setColour (theme::ruleStrong);
            g.drawEllipse (r.reduced (1.5f), 1.0f);
            g.drawLine (juce::Line<float> (r.getX() + r.getWidth() * 0.28f, r.getBottom() - r.getHeight() * 0.28f,
                                           r.getRight() - r.getWidth() * 0.28f, r.getY() + r.getHeight() * 0.28f), 1.5f);
        }
        else
        {
            const auto c = theme::led (i);
            g.setColour (c.withAlpha (0.25f));
            g.fillEllipse (r);
            g.setColour (c);
            g.fillEllipse (r.reduced (3.0f));
        }
    }
}

void LedPicker::mouseDown (const juce::MouseEvent& e)
{
    const int i = cellAt (e.position);
    if (i < 0)
        return;
    value = lamps ? (value ^ (1 << i)) : i;
    repaint();
    if (onChange)
        onChange (value);
}

void LedPicker::mouseMove (const juce::MouseEvent& e)
{
    const int i = cellAt (e.position);
    if (i != hovered)
    {
        hovered = i;
        setMouseCursor (i < 0 ? juce::MouseCursor::NormalCursor : juce::MouseCursor::PointingHandCursor);
        repaint();
    }
}

void LedPicker::mouseExit (const juce::MouseEvent&)
{
    hovered = -1;
    repaint();
}

juce::String LedPicker::getTooltip()
{
    if (hovered < 0)
        return {};
    if (lamps)
        return juce::String (kLampNames[hovered]) + " lamp";
    return theme::ledName (hovered);
}
