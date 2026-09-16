#include "EmulatorStrip.h"
#include "MidiText.h"
#include "Theme.h"

static constexpr size_t kActivityRows = 8;

EmulatorStrip::EmulatorStrip (EmulatorLink& e)
    : emu (e)
{
    for (auto* b : { &readyButton, &lockedButton, &noneButton })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (0x5107);
        theme::setVariant (*b, "segment");
        addAndMakeVisible (b);
    }
    readyButton.setToggleState (emu.getStorageMode() == EmulatorLink::StorageMode::ready, juce::dontSendNotification);
    lockedButton.setToggleState (emu.getStorageMode() == EmulatorLink::StorageMode::locked, juce::dontSendNotification);
    noneButton.setToggleState (emu.getStorageMode() == EmulatorLink::StorageMode::none, juce::dontSendNotification);
    readyButton.onClick = [this] { emu.setStorageMode (EmulatorLink::StorageMode::ready); };
    lockedButton.onClick = [this] { emu.setStorageMode (EmulatorLink::StorageMode::locked); };
    noneButton.onClick = [this] { emu.setStorageMode (EmulatorLink::StorageMode::none); };
    readyButton.setTooltip ("Our image is installed: saves are written, to two files standing in for the eMMC slots.");
    lockedButton.setTooltip ("The factory image is on the eMMC: the unit refuses to save.");
    noneButton.setTooltip ("No storage at all: nothing can be saved or reloaded.");

    powerCycle.onClick = [this]
    {
        emu.powerCycle();
        addActivity ("power cycled: storage read again", theme::inkDim);
        if (onPowerCycled)
            onPowerCycled();
    };
    powerCycle.setTooltip ("Start the emulated unit again from its storage, as a power cycle would, and read its configuration.");
    openFolder.onClick = [this]
    {
        emu.storageDirectory().createDirectory();
        emu.storageDirectory().revealToUser();
    };
    openFolder.setTooltip (emu.storageDirectory().getFullPathName());
    theme::setVariant (openFolder, "ghost");
    addAndMakeVisible (powerCycle);
    addAndMakeVisible (openFolder);

    emu.addListener (this);
    startTimerHz (5);
}

EmulatorStrip::~EmulatorStrip()
{
    stopTimer();
    emu.removeListener (this);
}

/* Let the activity list fade. */
void EmulatorStrip::timerCallback()
{
    if (isShowing() && ! activity.empty() && juce::Time::getMillisecondCounter() - activity.back().time < 12000)
        repaint (activityArea);
}

void EmulatorStrip::deviceMidiReceived (DeviceLink&, const juce::MidiMessage& m)
{
    if (isLiveViewTraffic (m))
        return;
    const auto kind = midiKind (m);
    juce::Colour c = theme::inkDim;
    juce::String key;
    switch (kind)
    {
        case MidiKind::noteOn:
        case MidiKind::noteOff:  c = juce::Colour (0xffc58af9); break;
        case MidiKind::cc:       c = theme::info; key << "cc" << m.getChannel() << "/" << m.getControllerNumber(); break;
        case MidiKind::program:  c = theme::accent; break;
        case MidiKind::bend:     c = theme::warn; key << "bend" << m.getChannel(); break;
        case MidiKind::pressure: c = theme::warn; key << "pressure" << m.getChannel(); break;
        case MidiKind::ours:     c = theme::inkFaint; break;
        default: break;
    }
    addActivity (describeMidi (m), c, key, kind == MidiKind::ours);
}

/* A pedal sweep is a stream of one controller: it updates its row rather than
 * pushing everything else off the list. */
void EmulatorStrip::addActivity (const juce::String& text, juce::Colour colour, const juce::String& key, bool quiet)
{
    const auto now = juce::Time::getMillisecondCounter();
    if (key.isNotEmpty() && ! activity.empty() && activity.back().key == key)
        activity.back() = { text, key, colour, now, quiet };
    else
        activity.push_back ({ text, key, colour, now, quiet });
    while (activity.size() > kActivityRows)
        activity.pop_front();
    repaint (activityArea);
}

void EmulatorStrip::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (theme::surface);
    g.fillRoundedRectangle (bounds, 12.0f);
    g.setColour (theme::rule);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 12.0f, 1.0f);
    g.fillRect (storageArea.getX() - 13, storageArea.getY(), 1, storageArea.getHeight());

    g.setColour (theme::inkFaint);
    g.setFont (theme::heading (12.0f));
    g.drawText ("MIDI FROM THE EMULATOR", activityArea.withHeight (18), juce::Justification::centredLeft, false);
    g.drawText ("EMULATED STORAGE", storageArea.withHeight (18), juce::Justification::centredLeft, false);

    auto rows = activityArea.withTrimmedTop (24);
    if (activity.empty())
    {
        g.setColour (theme::inkFaint);
        g.setFont (theme::ui (13.5f));
        g.drawFittedText ("Press a footswitch or turn a knob on the unit: what the emulator sends appears here.",
                          rows, juce::Justification::topLeft, 2, 1.0f);
        return;
    }

    const auto now = juce::Time::getMillisecondCounter();
    const int visible = juce::jmin ((int) activity.size(), rows.getHeight() / 21);
    for (int i = 0; i < visible; ++i)
    {
        const auto& a = activity[activity.size() - 1 - (size_t) i];
        const float age = (float) (now - a.time);
        const float alpha = juce::jlimit (0.35f, 1.0f, 1.0f - (age - 1500.0f) / 8000.0f);
        auto row = rows.removeFromTop (21);
        g.setColour (a.colour.withMultipliedAlpha (alpha));
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (row.removeFromLeft (12).getCentre().toFloat()));
        g.setFont (theme::mono (13.0f));
        g.setColour ((a.quiet ? theme::inkFaint : theme::ink).withMultipliedAlpha (alpha));
        g.drawText (a.text, row, juce::Justification::centredLeft, true);
    }
}

void EmulatorStrip::resized()
{
    auto r = getLocalBounds().reduced (16, 12);
    storageArea = r.removeFromRight (juce::jmin (320, r.getWidth() / 2));
    r.removeFromRight (26);
    activityArea = r;

    auto s = storageArea.withTrimmedTop (24);
    auto segments = s.removeFromTop (30);
    const int w = segments.getWidth() / 3;
    readyButton.setBounds (segments.removeFromLeft (w).reduced (2, 0));
    lockedButton.setBounds (segments.removeFromLeft (w).reduced (2, 0));
    noneButton.setBounds (segments.reduced (2, 0));
    s.removeFromTop (8);
    auto buttons = s.removeFromTop (30);
    powerCycle.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 4));
    buttons.removeFromLeft (8);
    openFolder.setBounds (buttons);
}
