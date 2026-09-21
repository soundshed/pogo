#include "MainComponent.h"
#include "ui/Theme.h"
#include "midi_control/midi_control_version.h"

MainComponent::MainComponent (bool offline)
{
    for (auto* b : { &navConfigure, &navMonitor, &navFirmware })
    {
        theme::setVariant (*b, "nav");
        b->setClickingTogglesState (true);
        b->setRadioGroupId (0x7a1);
        addAndMakeVisible (b);
    }
    navConfigure.onClick = [this] { showView (View::configure); };
    navMonitor.onClick = [this] { showView (View::monitor); };
    navFirmware.onClick = [this] { showView (View::firmware); };

    linkBox.onChange = [this] { selectLink(); };
    linkBox.setTooltip ("What to configure: the emulator in this app, or a unit on a MIDI port.");
    addAndMakeVisible (linkBox);
    connectButton.onClick = [this] { session.connect(); };
    connectButton.setTooltip ("Ask the device who it is and read its configuration.");
    addAndMakeVisible (connectButton);
    refreshButton.onClick = [this] { refreshLinks(); };
    refreshButton.setTooltip ("Look for MIDI ports again.");
    theme::setVariant (refreshButton, "ghost");
    addAndMakeVisible (refreshButton);

    for (auto* c : std::initializer_list<juce::Component*> { &controls, &monitor, &firmware })
        addChildComponent (c);
    controls.onMessage = [this] (const juce::String& s)
    {
        monitor.addNote (s);
        setMessage (s);
    };

    emulator.addListener (this);
    midiLink.addListener (this);
    session.addListener (this);

    if (offline)
        linkBox.addItem ("Emulator (in this app)", 1);
    else
        refreshLinks();

    /* refreshLinks() has already connected to a unit it found; only then is
     * the emulator not the default. */
    if (linkBox.getSelectedId() <= 1)
    {
        linkBox.setSelectedId (1, juce::dontSendNotification);
        selectLink();
    }

    showView (View::configure);
    setSize (1360, 880);
    if (! offline)
        startTimer (5000);
}

void MainComponent::stageSnapshots()
{
    session.setText (0, "Plexi Drive");
    session.setText (1, "Verse - neck pickup");

    /* Played over the link once the emulator has answered, the way the drawing
     * plays a unit, so the pictures show the live view at work: the volume
     * knob moved in steps that merge, knob 1 turned, a footswitch held. The
     * pedal is the emulated board's own, so it is moved as a foot moves it. */
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::Timer::callAfterDelay (500, [safe]
    {
        if (safe == nullptr)
            return;
        auto& s = safe->session;
        for (int v : { 32, 64, 96 })
            s.moveAnalog (MIDI_CONTROL_ANALOG_VOLUME, v);
        safe->emulator.movePedal (72);
        s.turnEncoder (MIDI_CONTROL_ENC_1, 2);
        s.turnEncoder (MIDI_CONTROL_ENC_1, 1);
        s.pressButton (MIDI_CONTROL_BTN_FS_B, true);
    });
}

void MainComponent::saveSnapshots (const juce::File& dir, float scale)
{
    dir.createDirectory();
    auto save = [&] (const juce::String& name)
    {
        const auto image = createComponentSnapshot (getLocalBounds(), true, scale);
        const auto file = dir.getChildFile (name + ".png");
        file.deleteFile();
        juce::FileOutputStream out (file);
        if (out.openedOk())
            juce::PNGImageFormat().writeImageToStream (image, out);
    };

    showView (View::configure);
    save ("configure");
    const std::pair<HardwareView::Part, const char*> parts[] = {
        { HardwareView::Part::knob1, "configure-knob" }, { HardwareView::Part::pedal, "configure-pedal" },
        { HardwareView::Part::tap, "configure-tap" },    { HardwareView::Part::screen, "configure-global" },
    };
    for (const auto& [part, name] : parts)
    {
        controls.showControl (part);
        save (name);
    }
    controls.showControl (HardwareView::Part::fsA);

    showView (View::monitor);
    save ("monitor");
    showView (View::firmware);
    save ("firmware");

    /* Last, because it drops the link: the Configure view as it is with a unit
     * rather than the emulator. */
    session.setLink (nullptr);
    controls.syncWithSession();
    showView (View::configure);
    save ("configure-unit");
}

MainComponent::~MainComponent()
{
    session.setLink (nullptr);
    session.removeListener (this);
    emulator.removeListener (this);
    midiLink.removeListener (this);
}

void MainComponent::showView (View v)
{
    controls.setVisible (v == View::configure);
    monitor.setVisible (v == View::monitor);
    firmware.setVisible (v == View::firmware);
    navConfigure.setToggleState (v == View::configure, juce::dontSendNotification);
    navMonitor.setToggleState (v == View::monitor, juce::dontSendNotification);
    navFirmware.setToggleState (v == View::firmware, juce::dontSendNotification);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);

    g.setColour (theme::surface);
    g.fillRect (header);
    g.setColour (theme::rule);
    g.fillRect (header.getX(), header.getBottom() - 1, header.getWidth(), 1);

    /* The product name, with the accent bar the unit's start-up screen puts under it. */
    const auto brandFont = theme::heading (30.0f, true);
    const auto brand = header.withTrimmedLeft (22).withWidth (140);
    g.setColour (theme::ink);
    g.setFont (brandFont);
    g.drawText (MIDI_CONTROL_PRODUCT_NAME, brand.withTrimmedBottom (6), juce::Justification::centredLeft, false);
    g.setColour (theme::accent);
    g.fillRoundedRectangle ((float) brand.getX() + 1.0f, (float) brand.getCentreY() + 13.0f,
                            juce::jmin (46.0f, theme::textWidth (brandFont, MIDI_CONTROL_PRODUCT_NAME)), 3.5f, 1.75f);

    g.setColour (theme::surface);
    g.fillRect (footer);
    g.setColour (theme::rule);
    g.fillRect (footer.getX(), footer.getY(), footer.getWidth(), 1);

    auto chips = footer.reduced (14, 6).toFloat();
    auto chip = [&] (const juce::String& text, juce::Colour ink, juce::Colour dot)
    {
        const float w = theme::chipWidth (text, chips.getHeight(), ! dot.isTransparent());
        theme::drawChip (g, chips.removeFromLeft (w), text, ink, dot);
        chips.removeFromLeft (8.0f);
    };

    const auto* link = session.getLink();
    if (link == nullptr)
        chip ("NO LINK", theme::inkDim, theme::inkFaint);
    else if (! session.isConnected())
        chip (link->getName().toUpperCase() + "  NOT CONNECTED", theme::inkDim, theme::bad);
    else
    {
        const auto& id = session.identity();
        chip (link->getName().toUpperCase(), theme::ink, theme::good);
        chip ("FIRMWARE " + juce::String (id.fw_major) + "." + juce::String (id.fw_minor) + "." + juce::String (id.fw_patch)
                  + (id.platform == MIDI_CONTROL_PLATFORM_EMULATOR ? "  EMULATED" : ""),
              theme::inkDim, {});
        switch (id.storage_state)
        {
            case MIDI_CONTROL_STORAGE_READY:
                chip (id.dirty ? "UNSAVED" : "SAVED", theme::ink, id.dirty ? theme::warn : theme::good);
                break;
            case MIDI_CONTROL_STORAGE_LOCKED:
                chip ("STORAGE LOCKED", theme::ink, theme::warn);
                break;
            case MIDI_CONTROL_STORAGE_UNKNOWN:
                chip ("STORAGE UNKNOWN", theme::ink, theme::warn);
                break;
            default:
                chip ("NO STORAGE", theme::inkDim, theme::inkFaint);
                break;
        }
    }

    auto right = chips.toNearestInt();
    g.setFont (theme::ui (13.0f));
    g.setColour (theme::inkFaint);
    const juce::String version = "app " + juce::String (MIDI_CONTROL_VERSION_MAJOR) + "." + juce::String (MIDI_CONTROL_VERSION_MINOR)
                               + "." + juce::String (MIDI_CONTROL_VERSION_PATCH);
    g.drawText (version, right.removeFromRight (80), juce::Justification::centredRight, false);
    g.setColour (theme::inkDim);
    g.drawText (message, right.withTrimmedRight (16), juce::Justification::centredRight, true);
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    header = r.removeFromTop (60);
    footer = r.removeFromBottom (34);

    auto h = header.reduced (16, 12);
    h.removeFromLeft (150);
    for (auto* b : { &navConfigure, &navMonitor, &navFirmware })
    {
        b->setBounds (h.removeFromLeft (104));
        h.removeFromLeft (4);
    }
    refreshButton.setBounds (h.removeFromRight (84));
    h.removeFromRight (8);
    connectButton.setBounds (h.removeFromRight (92));
    h.removeFromRight (8);
    linkBox.setBounds (h.removeFromRight (juce::jmin (300, juce::jmax (120, h.getWidth() - 24))));

    for (auto* c : std::initializer_list<juce::Component*> { &controls, &monitor, &firmware })
        c->setBounds (r);
}

void MainComponent::setMessage (const juce::String& text)
{
    message = text;
    repaint (footer);
}

void MainComponent::refreshLinks()
{
    const int selected = linkBox.getSelectedId();
    linkBox.clear (juce::dontSendNotification);
    linkBox.addItem ("Emulator (in this app)", 1);
    ports = MidiDeviceLink::availablePairs();
    for (int i = 0; i < ports.size(); ++i)
        linkBox.addItem ("MIDI: " + ports[i].displayName, 2 + i);
    linkBox.setSelectedId (selected > 0 ? selected : 1, juce::dontSendNotification);

    /* Prefer the unit when it appears and nothing else was chosen. */
    if (selected <= 1)
        for (int i = 0; i < ports.size(); ++i)
            if (ports[i].displayName.containsIgnoreCase (MIDI_CONTROL_PRODUCT_NAME))
            {
                linkBox.setSelectedId (2 + i, juce::dontSendNotification);
                selectLink();
                break;
            }
}

void MainComponent::selectLink()
{
    const int id = linkBox.getSelectedId();
    if (id <= 1)
    {
        midiLink.close();
        session.setLink (&emulator);
        session.connect();
    }
    else
    {
        const int i = id - 2;
        if (i >= 0 && i < ports.size())
        {
            juce::String err;
            if (! midiLink.open (ports[i], err))
            {
                const auto text = "could not open " + ports[i].displayName + ": " + err;
                monitor.addNote (text);
                setMessage (text);
                session.setLink (nullptr);
            }
            else
            {
                session.setLink (&midiLink);
                session.connect();
            }
        }
    }
    repaint (footer);
}

bool MainComponent::isAnalogTraffic (const juce::MidiMessage& m) const
{
    return monitor.hideAnalog() && m.isController()
        && (m.getControllerNumber() == session.config().analogs[0].number
            || m.getControllerNumber() == session.config().analogs[1].number);
}

void MainComponent::deviceMidiReceived (DeviceLink& link, const juce::MidiMessage& m)
{
    if (&link != session.getLink() || isAnalogTraffic (m))
        return;
    monitor.addMidi (MidiMonitor::Direction::in, m);
}

void MainComponent::sessionSent (ConfigSession&, const juce::MidiMessage& m)
{
    monitor.addMidi (MidiMonitor::Direction::out, m);
}

void MainComponent::deviceLinkChanged (DeviceLink&)
{
    repaint (footer);
}

void MainComponent::sessionChanged (ConfigSession&)
{
    repaint (footer);
}

void MainComponent::sessionMessage (ConfigSession&, const juce::String& s)
{
    monitor.addNote (s);
    setMessage (s);
}

void MainComponent::timerCallback()
{
    /* Ports come and go as the unit reboots; keep the list current without
     * disturbing an open link. */
    if (linkBox.getSelectedId() <= 1)
        refreshLinks();
}
