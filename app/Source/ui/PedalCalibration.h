#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ConfigSession.h"
#include "midi_control/midi_control_analog.h"

/* The pedal calibrated by capture (docs/05 section 6), walked through: hold the
 * pedal still at one end, then at the other, and the unit takes the counts it
 * settles on as its travel. The unit runs it - this starts it, cancels it and
 * follows it from the state reports - so a calibration begun on the unit, with
 * the toe switch held, is followed here just the same.
 *
 * It takes the inspector's place, so the drawing stays beside it: a unit's
 * screen, drawn there, shows the same steps, and in the emulator the drawn
 * pedal is the one to move. */
class PedalCalibration : public juce::Component,
                         private ConfigSession::Listener
{
public:
    explicit PedalCalibration (ConfigSession& session);
    ~PedalCalibration() override;

    /* Dismissed. A calibration still running on the device is left running:
     * its own screen carries on, and Mode or the timeout ends it there. */
    std::function<void()> onClose;

    /* Shown afresh, starting a calibration if `start` and the device can. */
    void begin (bool start);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum class Outcome { none, taken, stopped };
    enum class Step { pending, active, done };

    void sessionChanged (ConfigSession&) override;
    void sessionState (ConfigSession&) override;
    void update();

    juce::String unavailableReason() const;
    juce::String outcomeText (juce::Colour& colour) const;
    void drawScale (juce::Graphics&, juce::Rectangle<int>) const;
    void drawStep (juce::Graphics&, juce::Rectangle<int>, int number, const juce::String& text,
                   const juce::String& detail, Step step, int progress) const;

    ConfigSession& session;
    juce::TextButton startButton { "Start" }, cancelButton { "Cancel" }, saveButton { "Save to device" },
                     closeButton { "Close" };

    /* How the last calibration went, kept after the unit has put it away. */
    Outcome outcome = Outcome::none;
    int takenFirst = 0, takenSecond = 0;
    int lastStage = MIDI_CONTROL_CAL_OFF;
    bool laidOutRunning = false, laidOutSave = false;

    juce::Rectangle<int> header, card, scaleArea, stepsArea, noteArea;
};
