#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "midi_control/midi_control_config.h"
#include "midi_control/midi_control_ui.h"

/* What a control does, in a few words: the drawn unit's captions, its
 * tooltips and the editor's headings. The screen's own, shorter words for the
 * same thing are built in shared/src/midi_control_ui.c. */
namespace mapping
{
struct Text
{
    juce::String what;      /* "NOTE 60", "CC 64", "PC 12", "BANK +" */
    juce::String detail;    /* "C4, hold", "toggle 127 / 0" */
    bool assigned = false;
};

inline juce::String separator()
{
    return " " + juce::String::charToString ((juce::juce_wchar) 0x00b7) + " ";
}

inline juce::String channel (const midi_control_config& cfg, int ch)
{
    return "ch " + juce::String (ch == 0 ? cfg.global.midi_channel + 1 : ch);
}

inline juce::String noteName (int note)
{
    return juce::MidiMessage::getMidiNoteName (note, true, true, 4);
}

/* "12B": the bank and letter the unit shows for a program. */
inline juce::String programLabel (const midi_control_config& cfg, int program)
{
    char label[8];
    midi_control_ui_program_label ((uint8_t) juce::jlimit (0, 127, program), cfg.global.bank_size, label);
    return label;
}

inline Text button (const midi_control_config& cfg, int id)
{
    const auto& b = cfg.buttons[id];
    const int step = b.number ? b.number : 1;
    const int bank = juce::jmax (1, (int) cfg.global.bank_size);
    const auto sep = separator();
    Text t;
    t.assigned = b.action != MIDI_CONTROL_ACT_NONE;
    switch (b.action)
    {
        case MIDI_CONTROL_ACT_NOTE_MOMENTARY:
            t.what = "NOTE " + juce::String (b.number);
            t.detail = noteName (b.number) + sep + "hold";
            break;
        case MIDI_CONTROL_ACT_NOTE_TOGGLE:
            t.what = "NOTE " + juce::String (b.number);
            t.detail = noteName (b.number) + sep + "toggle";
            break;
        case MIDI_CONTROL_ACT_CC_MOMENTARY:
            t.what = "CC " + juce::String (b.number);
            t.detail = "hold " + juce::String (b.value_on) + " / " + juce::String (b.value_off);
            break;
        case MIDI_CONTROL_ACT_CC_TOGGLE:
            t.what = "CC " + juce::String (b.number);
            t.detail = "toggle " + juce::String (b.value_on) + " / " + juce::String (b.value_off);
            break;
        case MIDI_CONTROL_ACT_PROGRAM:
            t.what = "PC " + juce::String (b.number);
            t.detail = "preset " + programLabel (cfg, b.number);
            break;
        case MIDI_CONTROL_ACT_PROGRAM_UP:
            t.what = "PC +" + juce::String (step);
            t.detail = "program up";
            break;
        case MIDI_CONTROL_ACT_PROGRAM_DOWN:
            t.what = "PC -" + juce::String (step);
            t.detail = "program down";
            break;
        case MIDI_CONTROL_ACT_BANK_UP:
            t.what = "BANK +";
            t.detail = "up " + juce::String (bank) + " presets";
            break;
        case MIDI_CONTROL_ACT_BANK_DOWN:
            t.what = "BANK -";
            t.detail = "down " + juce::String (bank) + " presets";
            break;
        case MIDI_CONTROL_ACT_PRESET_IN_BANK:
            t.what = "PRESET " + programLabel (cfg, b.number % bank).getLastCharacters (1);
            t.detail = "in the current bank";
            break;
        default:
            t.what = "OFF";
            t.detail = "does nothing";
            break;
    }
    if (t.assigned)
        t.detail << sep << channel (cfg, b.channel);
    return t;
}

inline Text encoder (const midi_control_config& cfg, int id)
{
    const auto& e = cfg.encoders[id];
    const int step = e.step ? e.step : 1;
    const auto sep = separator();
    Text t;
    t.assigned = e.mode != MIDI_CONTROL_ENCM_NONE;
    switch (e.mode)
    {
        case MIDI_CONTROL_ENCM_CC_RELATIVE_TWOS:
        case MIDI_CONTROL_ENCM_CC_RELATIVE_OFFSET:
            t.what = "CC " + juce::String (e.number);
            t.detail = "relative";
            break;
        case MIDI_CONTROL_ENCM_CC_ABSOLUTE:
            t.what = "CC " + juce::String (e.number);
            t.detail = "absolute";
            break;
        case MIDI_CONTROL_ENCM_PROGRAM:
            t.what = "PROGRAM";
            t.detail = "program " + juce::String::charToString ((juce::juce_wchar) 0x00b1) + juce::String (step);
            break;
        default:
            t.what = "OFF";
            t.detail = "does nothing";
            break;
    }
    if (t.assigned)
        t.detail << sep << channel (cfg, e.channel);
    return t;
}

inline Text analog (const midi_control_config& cfg, int id)
{
    const auto& a = cfg.analogs[id];
    const auto sep = separator();
    const juce::String range = juce::String (a.out_min) + "-" + juce::String (a.out_max);
    Text t;
    t.assigned = a.mode != MIDI_CONTROL_ANM_NONE;
    switch (a.mode)
    {
        case MIDI_CONTROL_ANM_CC:
            t.what = "CC " + juce::String (a.number);
            t.detail = range;
            break;
        case MIDI_CONTROL_ANM_PITCH_BEND:
            t.what = "BEND";
            t.detail = range;
            break;
        case MIDI_CONTROL_ANM_CHANNEL_PRESSURE:
            t.what = "PRESSURE";
            t.detail = range;
            break;
        default:
            t.what = "OFF";
            t.detail = "does nothing";
            break;
    }
    if (t.assigned)
    {
        if (a.invert)
            t.detail << sep << "inverted";
        t.detail << sep << channel (cfg, a.channel);
    }
    return t;
}
} // namespace mapping
