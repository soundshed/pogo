#include "PedalCalibration.h"
#include "Theme.h"
#include "midi_control/midi_control_engine.h"

namespace
{
/* The LPC4350's converter is 10-bit: the scale the pedal's counts are drawn on,
 * unless a count past it turns up. */
constexpr int kConverterMax = 1023;

bool running (int stage)
{
    return stage == MIDI_CONTROL_CAL_FIRST || stage == MIDI_CONTROL_CAL_SECOND;
}
} // namespace

PedalCalibration::PedalCalibration (ConfigSession& s)
    : session (s)
{
    setOpaque (true);

    startButton.onClick = [this]
    {
        outcome = Outcome::none;
        session.calibratePedal (true);
        update();
    };
    cancelButton.onClick = [this] { session.calibratePedal (false); };
    saveButton.onClick = [this] { session.save(); };
    closeButton.onClick = [this]
    {
        if (onClose)
            onClose();
    };

    startButton.setTooltip ("Start it on the device, then hold the pedal still at one end and then the other.");
    cancelButton.setTooltip ("Stop without changing anything: the pedal keeps the calibration it had.");
    saveButton.setTooltip ("Save the device's settings, and the pedal's new travel with them.");
    closeButton.setTooltip ("Back to the settings. A calibration under way carries on on the unit's screen.");
    theme::setVariant (startButton, "primary");
    theme::setVariant (saveButton, "primary");
    theme::setVariant (closeButton, "ghost");
    for (auto* b : { &startButton, &cancelButton, &saveButton, &closeButton })
        addChildComponent (b);
    closeButton.setVisible (true);

    session.addListener (this);
    update();
}

PedalCalibration::~PedalCalibration()
{
    session.removeListener (this);
}

void PedalCalibration::begin (bool start)
{
    outcome = Outcome::none;
    lastStage = session.hasState() ? session.state().cal_stage : MIDI_CONTROL_CAL_OFF;
    if (start && session.canCalibrate())
        session.calibratePedal (true);
    update();
}

void PedalCalibration::sessionChanged (ConfigSession&)
{
    update();
}

void PedalCalibration::sessionState (ConfigSession&)
{
    update();
}

/* Why it cannot be done here, or nothing when it can. */
juce::String PedalCalibration::unavailableReason() const
{
    if (! session.isConnected())
        return "Connect to the unit, or to the emulator, to calibrate its pedal.";
    if (session.identity().protocol_version < 5)
        return "The firmware on this device is older than pedal calibration. Load the current firmware "
               "from the Firmware view, then come back.";
    if (! session.hasState())
        return "Waiting for the device to report its state...";
    if (! session.canCalibrate())
        return "This device is handed the pedal's position rather than its raw counts, so there is "
               "nothing here to calibrate.";
    return {};
}

void PedalCalibration::update()
{
    const bool can = session.canCalibrate();
    const auto& st = session.state();
    const int stage = can ? st.cal_stage : MIDI_CONTROL_CAL_OFF;

    /* The unit shows how it ended for a few seconds and then puts it away;
     * this keeps it until the next one begins. */
    if (stage == MIDI_CONTROL_CAL_FIRST && ! running (lastStage))
        outcome = Outcome::none;
    if (stage == MIDI_CONTROL_CAL_SECOND)
        takenFirst = st.cal_first;
    if (stage == MIDI_CONTROL_CAL_DONE)
    {
        outcome = Outcome::taken;
        takenFirst = st.cal_first;
        takenSecond = st.cal_second;
    }
    else if (stage == MIDI_CONTROL_CAL_FAILED)
        outcome = Outcome::stopped;
    lastStage = stage;

    const bool isRunning = running (stage);
    const bool unsaved = can && (st.pedal_flags & MIDI_CONTROL_PEDAL_UNSAVED) != 0;
    const bool showSave = outcome == Outcome::taken && unsaved;
    startButton.setVisible (! isRunning);
    startButton.setEnabled (can);
    startButton.setButtonText (outcome == Outcome::none ? "Start" : "Calibrate again");
    cancelButton.setVisible (isRunning);
    saveButton.setVisible (showSave);
    saveButton.setEnabled (session.identity().storage_state == MIDI_CONTROL_STORAGE_READY);

    if (isRunning != laidOutRunning || showSave != laidOutSave)
        resized();
    repaint();
}

/* What became of the last calibration, for the third step. */
juce::String PedalCalibration::outcomeText (juce::Colour& colour) const
{
    const auto& st = session.state();
    const int lo = juce::jmin (takenFirst, takenSecond), hi = juce::jmax (takenFirst, takenSecond);
    const auto travel = juce::String (lo) + " to " + juce::String (hi);

    colour = theme::inkDim;
    switch (outcome)
    {
        case Outcome::taken:
            if ((st.pedal_flags & MIDI_CONTROL_PEDAL_UNSAVED) == 0)
            {
                colour = theme::good;
                return "Saved on the device. The pedal's travel is " + travel + ".";
            }
            colour = theme::warn;
            if (session.identity().storage_state != MIDI_CONTROL_STORAGE_READY)
                return "Calibrated to " + travel + " for now. This device cannot save (storage "
                     + juce::String (midi_control_storage_state_name (session.identity().storage_state))
                     + "), so it lasts until the next power cycle.";
            if (st.dirty)
                return "Calibrated to " + travel + ", not saved yet: you have other unsaved changes, and "
                       "saving those is yours to do. Save to device keeps both.";
            return "Calibrated to " + travel + ". Saving...";
        case Outcome::stopped:
            return "Stopped, and nothing was changed: the pedal keeps the calibration it had. "
                   "(It stops by itself if nothing is held still for 30 seconds.)";
        case Outcome::none:
        default:
            break;
    }
    return running (lastStage) ? juce::String() : juce::String ("Press Start, or hold the toe switch on the unit for two seconds.");
}

void PedalCalibration::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);

    auto h = header;
    g.setColour (theme::ink);
    g.setFont (theme::heading (26.0f));
    g.drawText ("Calibrate the pedal", h.removeFromTop (38), juce::Justification::bottomLeft, true);
    g.setColour (theme::inkDim);
    g.setFont (theme::ui (13.5f));
    g.drawFittedText ("Hold the pedal still at each end in turn. Where it stops becomes its travel, so "
                      "both ends send the full range.",
                      h.withTrimmedBottom (4), juce::Justification::centredLeft, 2);

    g.setColour (theme::surface);
    g.fillRoundedRectangle (card.toFloat(), 12.0f);
    g.setColour (theme::rule);
    g.drawRoundedRectangle (card.toFloat().reduced (0.5f), 12.0f, 1.0f);

    const auto reason = unavailableReason();
    if (reason.isNotEmpty())
    {
        g.setColour (theme::inkDim);
        g.setFont (theme::ui (14.0f));
        g.drawFittedText (reason, card.reduced (18, 16), juce::Justification::topLeft, 5);
        return;
    }

    drawScale (g, scaleArea);

    const auto& st = session.state();
    const int stage = st.cal_stage;
    const int progress = st.cal_progress;
    const bool taken = outcome == Outcome::taken;

    auto rows = stepsArea;
    const Step one = stage == MIDI_CONTROL_CAL_FIRST ? Step::active
                   : stage == MIDI_CONTROL_CAL_SECOND || stage == MIDI_CONTROL_CAL_DONE || taken ? Step::done
                                                                                                 : Step::pending;
    const Step two = stage == MIDI_CONTROL_CAL_SECOND ? Step::active
                   : stage == MIDI_CONTROL_CAL_DONE || taken ? Step::done : Step::pending;
    drawStep (g, rows.removeFromTop (64), 1, "Hold the pedal still at one end: toe down or heel down.",
              one == Step::active ? "Reading " + juce::String (st.cal_raw) + ". Keep it there."
              : one == Step::done ? "Taken at " + juce::String (takenFirst) : juce::String(),
              one, progress);
    drawStep (g, rows.removeFromTop (64), 2, "Now hold it still at the other end.",
              two == Step::active ? "Reading " + juce::String (st.cal_raw) + ". Keep it there."
              : two == Step::done ? "Taken at " + juce::String (takenSecond) : juce::String(),
              two, progress);

    juce::Colour colour;
    const auto text = outcomeText (colour);
    if (text.isNotEmpty())
    {
        rows.removeFromTop (6);
        g.setColour (colour);
        g.setFont (theme::ui (14.0f));
        g.drawFittedText (text, rows.withTrimmedLeft (40), juce::Justification::topLeft, 4);
    }

    juce::String note ("On the unit: hold the toe switch for two seconds to start, and press Mode to cancel.");
    if (session.identity().platform == MIDI_CONTROL_PLATFORM_EMULATOR)
        note << " In the emulator, drag the drawn pedal to move it.";
    g.setColour (theme::inkFaint);
    g.setFont (theme::ui (13.0f));
    g.drawFittedText (note, noteArea, juce::Justification::centredLeft, 2);
}

/* The pedal's raw count on the converter's scale: the window it is scaled from
 * now as a band, the ends taken as ticks, and where it is as a needle. A window
 * wider than the ends the pedal reaches is what leaves them short of the range. */
void PedalCalibration::drawScale (juce::Graphics& g, juce::Rectangle<int> area) const
{
    const auto& st = session.state();
    const int top = juce::jmax ((int) st.pedal_hi, (int) st.pedal_raw, takenFirst, takenSecond);
    const int scaleMax = juce::jmax (kConverterMax, top);
    const bool isRunning = running (st.cal_stage);

    g.setColour (theme::inkFaint);
    g.setFont (theme::heading (12.0f));
    g.drawText ("WHAT THE UNIT READS", area.removeFromTop (18), juce::Justification::centredLeft, false);

    auto track = area.removeFromTop (34).toFloat().withTrimmedTop (12).withHeight (10.0f);
    const auto xOf = [&] (int count) { return track.getX() + track.getWidth() * (float) count / (float) scaleMax; };

    g.setColour (theme::field);
    g.fillRoundedRectangle (track, 5.0f);
    if (st.pedal_hi > st.pedal_lo)
    {
        const auto band = juce::Rectangle<float> (xOf (st.pedal_lo), track.getY(),
                                                  xOf (st.pedal_hi) - xOf (st.pedal_lo), track.getHeight());
        g.setColour (theme::accent.withAlpha (0.35f));
        g.fillRoundedRectangle (band, 5.0f);
        g.setColour (theme::accent);
        g.drawRoundedRectangle (band, 5.0f, 1.0f);
    }
    g.setColour (theme::good);
    for (int end : { takenFirst, takenSecond })
        if (end > 0 && (outcome == Outcome::taken || isRunning))
            g.fillRect (juce::Rectangle<float> (xOf (end) - 1.0f, track.getY() - 5.0f, 2.0f, track.getHeight() + 10.0f));
    const float needle = xOf (isRunning ? st.cal_raw : st.pedal_raw);
    g.setColour (theme::ink);
    g.fillRect (juce::Rectangle<float> (needle - 1.0f, track.getY() - 8.0f, 2.0f, track.getHeight() + 16.0f));

    auto scaleLabels = area.removeFromTop (16);
    g.setColour (theme::inkFaint);
    g.setFont (theme::mono (11.0f));
    g.drawText ("0", scaleLabels, juce::Justification::centredLeft, false);
    g.drawText (juce::String (scaleMax), scaleLabels, juce::Justification::centredRight, false);

    juce::String line;
    line << "Now " << (int) (isRunning ? st.cal_raw : st.pedal_raw);
    if (! isRunning)
    {
        /* The position is what calibration is about; the value is what the
         * mapping makes of it - with padding, 127 short of the stop. */
        const auto& map = session.config().analogs[MIDI_CONTROL_ANALOG_PEDAL];
        const auto position = st.analog[MIDI_CONTROL_ANALOG_PEDAL];
        line << ", at " << (int) position << " of 127";
        if (map.mode != MIDI_CONTROL_ANM_NONE)
            line << ", sending " << (int) midi_control_engine_analog_value (&map, position);
    }
    line << ". Scaled from " << (int) st.pedal_lo << " to " << (int) st.pedal_hi << ": "
         << ((st.pedal_flags & MIDI_CONTROL_PEDAL_SEEDED) != 0 ? "the firmware's starting guess, not yet this unit's."
                                                               : "this unit's calibration.");
    g.setColour (theme::inkDim);
    g.setFont (theme::ui (13.5f));
    g.drawFittedText (line, area.withTrimmedTop (4), juce::Justification::topLeft, 2);
}

void PedalCalibration::drawStep (juce::Graphics& g, juce::Rectangle<int> row, int number, const juce::String& text,
                                 const juce::String& detail, Step step, int progress) const
{
    const auto dot = row.removeFromLeft (40).removeFromTop (28).toFloat().withSizeKeepingCentre (26.0f, 26.0f);
    if (step == Step::done)
    {
        g.setColour (theme::good);
        g.fillEllipse (dot);
        juce::Path tick;
        tick.startNewSubPath (dot.getX() + 7.5f, dot.getCentreY() + 0.5f);
        tick.lineTo (dot.getX() + 11.5f, dot.getCentreY() + 4.5f);
        tick.lineTo (dot.getRight() - 7.0f, dot.getCentreY() - 4.5f);
        g.setColour (theme::onAccent);
        g.strokePath (tick, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    else
    {
        g.setColour (step == Step::active ? theme::warn : theme::rule);
        if (step == Step::active)
            g.fillEllipse (dot);
        else
            g.drawEllipse (dot.reduced (0.75f), 1.5f);
        g.setColour (step == Step::active ? theme::onAccent : theme::inkFaint);
        g.setFont (theme::heading (14.0f));
        g.drawText (juce::String (number), dot, juce::Justification::centred, false);
    }

    g.setColour (step == Step::pending ? theme::inkDim : theme::ink);
    g.setFont (theme::ui (14.5f));
    g.drawFittedText (text, row.removeFromTop (28), juce::Justification::centredLeft, 1);
    if (step == Step::active)
    {
        const auto bar = row.removeFromTop (12).toFloat().withTrimmedTop (3).withHeight (6.0f).withTrimmedRight (8);
        g.setColour (theme::field);
        g.fillRoundedRectangle (bar, 3.0f);
        if (progress > 0)
        {
            g.setColour (theme::warn);
            g.fillRoundedRectangle (bar.withWidth (juce::jmax (6.0f, bar.getWidth() * (float) progress / 127.0f)), 3.0f);
        }
    }
    if (detail.isNotEmpty())
    {
        g.setColour (step == Step::done ? theme::good : theme::inkDim);
        g.setFont (theme::mono (12.5f));
        g.drawText (detail, row.removeFromTop (18), juce::Justification::centredLeft, true);
    }
}

void PedalCalibration::resized()
{
    auto r = getLocalBounds();
    header = r.removeFromTop (74);

    auto buttons = r.removeFromBottom (48).withTrimmedTop (8).withTrimmedBottom (4);
    noteArea = r.removeFromBottom (40);
    card = r.withTrimmedBottom (8);
    auto inner = card.reduced (18, 16);
    scaleArea = inner.removeFromTop (112);
    inner.removeFromTop (10);
    stepsArea = inner;

    laidOutRunning = cancelButton.isVisible();
    laidOutSave = saveButton.isVisible();
    closeButton.setBounds (buttons.removeFromRight (90));
    auto& first = laidOutRunning ? cancelButton : startButton;
    first.setBounds (buttons.removeFromLeft (150));
    buttons.removeFromLeft (8);
    saveButton.setBounds (buttons.removeFromLeft (140));
}
