#include "MidiDeviceLink.h"

MidiDeviceLink::~MidiDeviceLink()
{
    close();
}

juce::Array<MidiDeviceLink::PortPair> MidiDeviceLink::availablePairs()
{
    juce::Array<PortPair> pairs;
    auto inputs = juce::MidiInput::getAvailableDevices();
    auto outputs = juce::MidiOutput::getAvailableDevices();

    for (auto& in : inputs)
    {
        PortPair p;
        p.input = in;
        p.displayName = in.name;
        for (auto& out : outputs)
            if (out.name == in.name)
            {
                p.output = out;
                break;
            }
        pairs.add (p);
    }
    for (auto& out : outputs)
    {
        bool seen = false;
        for (auto& p : pairs)
            if (p.output.identifier == out.identifier)
                seen = true;
        if (! seen)
        {
            PortPair p;
            p.output = out;
            p.displayName = out.name + " (output only)";
            pairs.add (p);
        }
    }
    return pairs;
}

bool MidiDeviceLink::open (const PortPair& pair, juce::String& error)
{
    close();
    error.clear();

    if (pair.input.identifier.isNotEmpty())
    {
        input = juce::MidiInput::openDevice (pair.input.identifier, this);
        if (input == nullptr)
            error << "could not open MIDI input " << pair.input.name << "\n";
        else
            input->start();
    }
    if (pair.output.identifier.isNotEmpty())
    {
        output = juce::MidiOutput::openDevice (pair.output.identifier);
        if (output == nullptr)
            error << "could not open MIDI output " << pair.output.name << "\n";
    }

    name = pair.displayName;
    if (input == nullptr && output == nullptr)
    {
        if (error.isEmpty())
            error = "no ports to open";
        return false;
    }
    dispatchChanged();
    return true;
}

void MidiDeviceLink::close()
{
    if (input != nullptr)
        input->stop();
    input = nullptr;
    output = nullptr;
    name.clear();
}

void MidiDeviceLink::sendMidi (const juce::MidiMessage& m)
{
    if (output != nullptr)
        output->sendMessageNow (m);
}

void MidiDeviceLink::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m)
{
    juce::MidiMessage copy (m);
    juce::WeakReference<MidiDeviceLink> safe (this);
    juce::MessageManager::callAsync ([safe, copy]
    {
        if (auto* self = safe.get())
            self->dispatchReceived (copy);
    });
}
