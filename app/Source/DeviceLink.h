/* A device the app talks MIDI to: the real unit over a MIDI port pair, or the
 * emulator in-process. Everything above this line - the protocol session,
 * the monitor, the editor - works through this interface and cannot tell
 * which one it has.
 *
 * All calls and callbacks are on the message thread.
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>

class DeviceLink
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void deviceMidiReceived (DeviceLink&, const juce::MidiMessage&) = 0;
        virtual void deviceLinkChanged (DeviceLink&) {}
    };

    virtual ~DeviceLink() = default;

    virtual juce::String getName() const = 0;
    virtual bool isOpen() const = 0;
    virtual bool isEmulator() const { return false; }

    /* A complete message; SysEx includes its F0 and F7. */
    virtual void sendMidi (const juce::MidiMessage& m) = 0;

    void addListener (Listener* l)    { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

protected:
    void dispatchReceived (const juce::MidiMessage& m)
    {
        listeners.call ([&] (Listener& l) { l.deviceMidiReceived (*this, m); });
    }

    void dispatchChanged()
    {
        listeners.call ([&] (Listener& l) { l.deviceLinkChanged (*this); });
    }

    juce::ListenerList<Listener> listeners;
};
