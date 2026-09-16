#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "midi_control/midi_control_protocol.h"
#include "midi_control/midi_control_version.h"

/* MIDI messages in words, for the monitor and the emulator's activity list. */

enum class MidiKind { noteOn, noteOff, cc, program, ours, sysex, bend, pressure, other };

inline MidiKind midiKind (const juce::MidiMessage& m)
{
    if (m.isSysEx())
    {
        midi_control_message msg;
        return midi_control_parse_message (m.getRawData(), (uint32_t) m.getRawDataSize(), &msg) ? MidiKind::ours
                                                                                               : MidiKind::sysex;
    }
    if (m.isNoteOn())          return MidiKind::noteOn;
    if (m.isNoteOff())         return MidiKind::noteOff;
    if (m.isController())      return MidiKind::cc;
    if (m.isProgramChange())   return MidiKind::program;
    if (m.isPitchWheel())      return MidiKind::bend;
    if (m.isChannelPressure()) return MidiKind::pressure;
    return MidiKind::other;
}

inline juce::String midiKindName (MidiKind k)
{
    switch (k)
    {
        case MidiKind::noteOn:   return "NOTE ON";
        case MidiKind::noteOff:  return "NOTE OFF";
        case MidiKind::cc:       return "CC";
        case MidiKind::program:  return "PROGRAM";
        case MidiKind::ours:     return MIDI_CONTROL_PRODUCT_NAME;
        case MidiKind::sysex:    return "SYSEX";
        case MidiKind::bend:     return "BEND";
        case MidiKind::pressure: return "PRESSURE";
        default:                 return "MIDI";
    }
}

/* The first bytes in hex, with the length when there are more. */
inline juce::String midiHex (const juce::MidiMessage& m, int maxBytes = 12)
{
    juce::String hex;
    const auto* d = m.getRawData();
    const int n = m.getRawDataSize();
    for (int i = 0; i < juce::jmin (n, maxBytes); ++i)
        hex << juce::String::toHexString (d[i]).paddedLeft ('0', 2).toUpperCase() << ' ';
    if (n > maxBytes)
        hex << "... (" << n << " bytes)";
    return hex.trimEnd();
}

/* What the message says, without its kind or channel. */
inline juce::String midiSummary (const juce::MidiMessage& m)
{
    const auto* d = m.getRawData();
    const int n = m.getRawDataSize();
    switch (midiKind (m))
    {
        case MidiKind::ours:
        {
            midi_control_message msg;
            midi_control_parse_message (d, (uint32_t) n, &msg);
            switch (msg.command)
            {
                case MIDI_CONTROL_CMD_IDENTIFY:      return "IDENTIFY";
                case MIDI_CONTROL_CMD_GET_CONFIG:    return "GET_CONFIG";
                case MIDI_CONTROL_CMD_SET_CONFIG:    return "SET_CONFIG";
                case MIDI_CONTROL_CMD_SAVE:          return "SAVE";
                case MIDI_CONTROL_CMD_LOAD_DEFAULTS: return "LOAD_DEFAULTS";
                case MIDI_CONTROL_CMD_GET_STATUS:    return "GET_STATUS";
                case MIDI_CONTROL_CMD_SET_TEXT:      return "SET_TEXT";
                case MIDI_CONTROL_CMD_RELOAD:        return "RELOAD";
                case MIDI_CONTROL_CMD_GET_STATE:     return "GET_STATE";
                case MIDI_CONTROL_CMD_EVENT:
                {
                    uint8_t kind = 0, id = 0;
                    int value = 0;
                    if (! midi_control_parse_event (&msg, &kind, &id, &value))
                        return "EVENT";
                    switch (kind)
                    {
                        case MIDI_CONTROL_EVENT_BUTTON:
                            return juce::String ("EVENT ") + midi_control_button_name ((midi_control_button) id) + (value ? " down" : " up");
                        case MIDI_CONTROL_EVENT_ENCODER:
                            return juce::String ("EVENT ") + midi_control_encoder_name ((midi_control_encoder) id) + " "
                                 + (value > 0 ? "+" : "") + juce::String (value);
                        case MIDI_CONTROL_EVENT_ANALOG:
                            return juce::String ("EVENT ") + midi_control_analog_name ((midi_control_analog) id) + " = " + juce::String (value);
                        default:
                            return "EVENT kind " + juce::String (kind);
                    }
                }
                case MIDI_CONTROL_RPL_IDENTITY:      return "IDENTITY";
                case MIDI_CONTROL_RPL_CONFIG:        return "CONFIG";
                case MIDI_CONTROL_RPL_STATUS:        return "STATUS";
                case MIDI_CONTROL_RPL_STATE:         return "STATE";
                case MIDI_CONTROL_RPL_RESULT:
                {
                    juce::String text = "RESULT";
                    if (msg.payload_len >= 2)
                        text << " for 0x" << juce::String::toHexString (msg.payload[0]) << ": "
                             << midi_control_status_name (msg.payload[1]);
                    return text;
                }
                default: return "0x" + juce::String::toHexString (msg.command);
            }
        }
        case MidiKind::sysex:
            return juce::String (n) + " bytes";
        case MidiKind::noteOn:
            return juce::MidiMessage::getMidiNoteName (m.getNoteNumber(), true, true, 4) + " (" + juce::String (m.getNoteNumber())
                 + ")  velocity " + juce::String ((int) m.getVelocity());
        case MidiKind::noteOff:
            return juce::MidiMessage::getMidiNoteName (m.getNoteNumber(), true, true, 4) + " (" + juce::String (m.getNoteNumber()) + ")";
        case MidiKind::cc:
            return "#" + juce::String (m.getControllerNumber()) + " = " + juce::String (m.getControllerValue());
        case MidiKind::program:
            return juce::String (m.getProgramChangeNumber());
        case MidiKind::bend:
            return juce::String (m.getPitchWheelValue());
        case MidiKind::pressure:
            return juce::String (m.getChannelPressureValue());
        default:
            return m.getDescription();
    }
}

/* The live view's own traffic: its state requests and replies, and the control
 * events the drawn unit plays the device with and their OK replies. Dozens a
 * second while the Configure view is open, so the monitor and the emulator's
 * activity list leave it out. A refused event is not in it; nor is the MIDI an
 * event makes the device send. */
inline bool isLiveViewTraffic (const juce::MidiMessage& m)
{
    midi_control_message msg;
    if (! m.isSysEx() || ! midi_control_parse_message (m.getRawData(), (uint32_t) m.getRawDataSize(), &msg))
        return false;
    switch (msg.command)
    {
        case MIDI_CONTROL_CMD_GET_STATE:
        case MIDI_CONTROL_RPL_STATE:
        case MIDI_CONTROL_CMD_EVENT:
            return true;
        case MIDI_CONTROL_RPL_RESULT:
            return msg.payload_len >= 2 && msg.payload[0] == MIDI_CONTROL_CMD_EVENT
                && msg.payload[1] == MIDI_CONTROL_STATUS_OK;
        default:
            return false;
    }
}

/* One line: kind, channel, summary. */
inline juce::String describeMidi (const juce::MidiMessage& m)
{
    const auto kind = midiKind (m);
    juce::String s = midiKindName (kind);
    if (m.getChannel() > 0)
        s << "  ch" << m.getChannel();
    return s + "  " + midiSummary (m);
}
