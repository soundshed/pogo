#pragma once

#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

/* A titled group of settings in the editor: each row a label on the left and
 * its editor on the right, a wide row with its label above, or a line of
 * explanation. Rows own their editors. */
class InspectorCard : public juce::Component
{
public:
    explicit InspectorCard (const juce::String& title, const juce::String& subtitle = {});

    template <typename T>
    T& add (const juce::String& label, std::unique_ptr<T> editor, int height = 34, bool wide = false)
    {
        auto* raw = editor.get();
        addRow (label, std::move (editor), height, wide);
        return *raw;
    }

    void addNote (const juce::String& text, int height = 36);
    int getIdealHeight() const;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void addRow (const juce::String& label, std::unique_ptr<juce::Component> editor, int height, bool wide);

    struct Row
    {
        juce::String label;
        std::unique_ptr<juce::Component> editor;
        int height = 34;
        bool wide = false;
    };

    std::vector<Row> rows;
    juce::String title, subtitle;

    static constexpr int headerHeight = 46, padding = 16, labelWidth = 138, wideLabel = 22;
};

/* A footswitch LED's colour: off and the seven its three bits make. On the Tap
 * column (`lamps`), the three lamps it drives, each on or off. */
class LedPicker : public juce::Component,
                  public juce::TooltipClient
{
public:
    explicit LedPicker (bool lamps);

    /* Without notification. */
    void setValue (int bits);
    int getValue() const { return value; }

    std::function<void (int)> onChange;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    juce::String getTooltip() override;

private:
    int cellCount() const { return lamps ? 3 : 8; }
    juce::Rectangle<float> cell (int i) const;
    int cellAt (juce::Point<float> p) const;

    bool lamps;
    int value = 0;
    int hovered = -1;
};
