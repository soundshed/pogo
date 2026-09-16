#pragma once

#include <deque>
#include <juce_gui_basics/juce_gui_basics.h>
#include "EmulatorLink.h"

/* Under the drawn unit while the emulator is the link: the MIDI the emulator
 * sends as it is played, the storage state it pretends to have (so a refused
 * save can be tried without a unit), and a power cycle. */
class EmulatorStrip : public juce::Component,
                      private DeviceLink::Listener,
                      private juce::Timer
{
public:
    explicit EmulatorStrip (EmulatorLink& emulator);
    ~EmulatorStrip() override;

    /* After a power cycle, so the owner can read the configuration again. */
    std::function<void()> onPowerCycled;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Activity
    {
        juce::String text, key;
        juce::Colour colour;
        juce::uint32 time = 0;
        bool quiet = false;     /* protocol replies, drawn dimmer than what a player sends */
    };

    void deviceMidiReceived (DeviceLink&, const juce::MidiMessage&) override;
    void timerCallback() override;
    void addActivity (const juce::String& text, juce::Colour colour, const juce::String& key = {}, bool quiet = false);

    EmulatorLink& emu;
    std::deque<Activity> activity;
    juce::Rectangle<int> activityArea, storageArea;
    juce::TextButton readyButton { "Ready" }, lockedButton { "Locked" }, noneButton { "None" };
    juce::TextButton powerCycle { "Power cycle" };
    juce::TextButton openFolder { "Open folder" };
};
