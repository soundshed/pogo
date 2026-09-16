#pragma once

#include <deque>
#include <juce_gui_basics/juce_gui_basics.h>
#include "MidiText.h"

/* The traffic between the app and the device, one row a message: when, which
 * way, what kind, the channel, what it says and its bytes. Session notes (a
 * result, an error) are rows too. */
class MidiMonitor : public juce::Component,
                    private juce::ListBoxModel,
                    private juce::Timer
{
public:
    enum class Direction { in, out, note };

    MidiMonitor();
    ~MidiMonitor() override;

    void addMidi (Direction direction, const juce::MidiMessage& m);
    void addNote (const juce::String& text);
    void clear();

    /* Hide analog CC traffic, which otherwise buries everything else. */
    bool hideAnalog() const { return hideAnalogToggle.getToggleState(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Entry
    {
        juce::String time;
        Direction direction = Direction::note;
        MidiKind kind = MidiKind::other;
        int channel = 0;
        juce::String summary, hex;
        bool problem = false;
    };

    int getNumRows() override { return (int) entries.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void timerCallback() override;
    void add (Entry e);

    std::deque<Entry> entries;
    juce::ListBox list;
    juce::TextButton clearButton { "Clear" };
    juce::ToggleButton followToggle { "Follow" };
    juce::ToggleButton hideAnalogToggle { "Hide pedal and knob CC" };
    juce::ToggleButton hideSysexToggle { "Hide configuration SysEx" };
    int inCount = 0, outCount = 0;
    bool pending = false;
};
