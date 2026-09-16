#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include "DeviceLink.h"

/* The real unit, over a MIDI input / output pair. Incoming messages arrive on
 * the MIDI thread and are re-posted to the message thread. */
class MidiDeviceLink : public DeviceLink,
                       private juce::MidiInputCallback
{
public:
    MidiDeviceLink() = default;
    ~MidiDeviceLink() override;

    struct PortPair
    {
        juce::MidiDeviceInfo input, output;
        juce::String displayName;
    };

    /* Inputs and outputs paired by name. Ports with no partner are listed
     * too, with the missing half empty, so a one-directional device is at
     * least visible. */
    static juce::Array<PortPair> availablePairs();

    bool open (const PortPair& pair, juce::String& error);
    void close();

    juce::String getName() const override { return name; }
    bool isOpen() const override          { return input != nullptr || output != nullptr; }
    void sendMidi (const juce::MidiMessage& m) override;

private:
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override;

    std::unique_ptr<juce::MidiInput> input;
    std::unique_ptr<juce::MidiOutput> output;
    juce::String name;

    JUCE_DECLARE_WEAK_REFERENCEABLE (MidiDeviceLink)
};
