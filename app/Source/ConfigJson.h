#pragma once

#include <juce_core/juce_core.h>
#include "midi_control/midi_control_config.h"

/* The configuration as a readable JSON document, for saving presets and
 * sharing them. Enumerations are written as identifiers, not numbers, so a
 * file survives a renumbering and a person can read it. */
namespace ConfigJson
{
    juce::String toJson (const midi_control_config& cfg);

    /* Returns false and sets `error` on anything it cannot read. Missing
     * fields keep their default values, so a partial file is usable. */
    bool fromJson (const juce::String& text, midi_control_config& cfg, juce::String& error);

    /* Identifier tables, shared with the editor's choice lists. */
    juce::StringArray buttonKeys();
    juce::StringArray encoderKeys();
    juce::StringArray analogKeys();
    juce::StringArray actionKeys();
    juce::StringArray encoderModeKeys();
    juce::StringArray analogModeKeys();
}
