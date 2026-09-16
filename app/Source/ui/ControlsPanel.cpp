#include <cstring>
#include "ControlsPanel.h"
#include "Captions.h"
#include "ConfigJson.h"
#include "MappingText.h"
#include "Theme.h"

namespace
{
/* How long a switch pressed and released between two state reports is drawn down. */
constexpr juce::uint32 kTapMs = 150;

juce::String subtitleFor (HardwareView::Part p)
{
    using Part = HardwareView::Part;
    if (p == Part::screen)
        return "What every control shares, and the text on the screen.";
    const auto ids = HardwareView::idsOf (p);
    juce::StringArray names;
    if (ids.encoder >= 0)
        names.add (midi_control_encoder_name ((midi_control_encoder) ids.encoder));
    if (ids.analog >= 0)
        names.add (midi_control_analog_name ((midi_control_analog) ids.analog));
    if (ids.button >= 0)
        names.add (midi_control_button_name ((midi_control_button) ids.button));
    auto s = "Firmware: " + names.joinIntoString (", ");
    if (p == Part::action || p == Part::pagePrevious)
        s << ". Which switch this is has not been confirmed on a unit.";
    return s;
}

juce::String numberLabel (unsigned action)
{
    switch (action)
    {
        case MIDI_CONTROL_ACT_NOTE_MOMENTARY:
        case MIDI_CONTROL_ACT_NOTE_TOGGLE:    return "Note";
        case MIDI_CONTROL_ACT_CC_MOMENTARY:
        case MIDI_CONTROL_ACT_CC_TOGGLE:      return "CC number";
        case MIDI_CONTROL_ACT_PROGRAM:        return "Program";
        case MIDI_CONTROL_ACT_PROGRAM_UP:
        case MIDI_CONTROL_ACT_PROGRAM_DOWN:   return "Step";
        case MIDI_CONTROL_ACT_PRESET_IN_BANK: return "Preset in bank";
        default:                              return {};
    }
}

/* "Never", "30 seconds", "5 minutes". */
juce::String durationName (int seconds)
{
    if (seconds <= 0)
        return "Never";
    if (seconds % 60 != 0)
        return juce::String (seconds) + (seconds == 1 ? " second" : " seconds");
    const int minutes = seconds / 60;
    return juce::String (minutes) + (minutes == 1 ? " minute" : " minutes");
}

juce::StringArray channelNames (const midi_control_config& cfg)
{
    juce::StringArray s;
    s.add ("Global (channel " + juce::String (cfg.global.midi_channel + 1) + ")");
    for (int i = 1; i <= 16; ++i)
        s.add ("Channel " + juce::String (i));
    return s;
}
} // namespace

ControlsPanel::ControlsPanel (ConfigSession& s, EmulatorLink& e)
    : session (s), emulator (e)
{
    hardware.onSelect = [this] (Part p) { select (p); };
    hardware.onButton = [this] (int b, bool down) { session.pressButton (b, down); };
    hardware.onEncoder = [this] (int enc, int detents) { session.turnEncoder (enc, detents); };
    hardware.onAnalog = [this] (int a, int value) { session.moveAnalog (a, value); };
    hardware.setSelected (selected);
    addAndMakeVisible (hardware);

    strip.onPowerCycled = [this]
    {
        haveCounts = false;     /* the emulator's counters start again from nothing */
        if (session.getLink() == &emulator)
            session.connect();
    };
    addChildComponent (strip);

    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);

    readButton.onClick = [this] { session.readConfig(); };
    applyButton.onClick = [this] { session.applyConfig(); };
    saveButton.onClick = [this] { session.save(); };
    reloadButton.onClick = [this] { session.reloadFromStorage(); };
    defaultsButton.onClick = [this] { session.loadDefaults(); };
    importButton.onClick = [this] { importJson(); };
    exportButton.onClick = [this] { exportJson(); };
    liveToggle.setToggleState (session.autoApply, juce::dontSendNotification);
    liveToggle.onClick = [this] { session.autoApply = liveToggle.getToggleState(); };

    readButton.setTooltip ("Replace the settings here with the device's.");
    applyButton.setTooltip ("Send these settings to the device. It uses them until it is power-cycled, unless saved.");
    saveButton.setTooltip ("Save the device's live settings on the device, so they survive a power cycle.");
    reloadButton.setTooltip ("Throw away the device's unsaved changes and use what it last saved.");
    defaultsButton.setTooltip ("Put the factory mapping on the device (not saved until you save).");
    liveToggle.setTooltip ("Send every edit to the device as it is made.");
    theme::setVariant (saveButton, "primary");
    for (auto* b : { &importButton, &exportButton })
        theme::setVariant (*b, "ghost");

    for (auto* b : { &readButton, &applyButton, &saveButton, &reloadButton, &defaultsButton, &importButton,
                     &exportButton })
        addAndMakeVisible (b);
    addAndMakeVisible (liveToggle);

    session.addListener (this);
    rebuildInspector();
    linkChanged();
    updateView();
    updateButtons();
}

ControlsPanel::~ControlsPanel()
{
    stopTimer();
    session.removeListener (this);
    refreshers.clear();
    cards.clear();
}

/* ---- layout --------------------------------------------------------------- */

void ControlsPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);
    g.setColour (theme::rule);
    g.fillRect (0, 57, getWidth(), 1);

    auto h = inspectorHeader;
    g.setColour (theme::ink);
    g.setFont (theme::heading (26.0f));
    g.drawText (selected == Part::screen ? juce::String ("Global settings") : HardwareView::nameOf (selected),
                h.removeFromTop (38), juce::Justification::bottomLeft, true);
    g.setColour (theme::inkDim);
    g.setFont (theme::ui (13.5f));
    g.drawFittedText (subtitleFor (selected), h.withTrimmedBottom (4), juce::Justification::centredLeft, 2);

    g.setColour (theme::inkFaint);
    g.setFont (theme::ui (13.5f));
    g.drawText (playable ? "Click a control to edit it. The drawing is the " + juce::String (emulated ? "emulator" : "unit")
                               + ", live: hold a switch, drag or scroll a knob, click a knob to push it."
                         : juce::String ("Click a control to edit it. The screen shows what these settings put on the unit's screen."),
                hintArea, juce::Justification::centredLeft, true);
}

void ControlsPanel::resized()
{
    auto r = getLocalBounds();
    auto bar = r.removeFromTop (58).reduced (16, 12);
    readButton.setBounds (bar.removeFromLeft (76));
    bar.removeFromLeft (8);
    applyButton.setBounds (bar.removeFromLeft (76));
    bar.removeFromLeft (18);
    saveButton.setBounds (bar.removeFromLeft (136));
    bar.removeFromLeft (8);
    reloadButton.setBounds (bar.removeFromLeft (136));
    bar.removeFromLeft (8);
    defaultsButton.setBounds (bar.removeFromLeft (92));
    exportButton.setBounds (bar.removeFromRight (96));
    bar.removeFromRight (8);
    importButton.setBounds (bar.removeFromRight (96));
    bar.removeFromRight (18);
    liveToggle.setBounds (bar.removeFromRight (120));

    r.removeFromTop (6);
    const int inspectorWidth = juce::jlimit (360, 460, r.getWidth() * 33 / 100);
    auto right = r.removeFromRight (inspectorWidth + 16).withTrimmedRight (16).withTrimmedBottom (12);
    inspectorHeader = right.removeFromTop (74);
    viewport.setBounds (right);

    hintArea = r.removeFromTop (22).withTrimmedLeft (22);
    if (emulated)
        strip.setBounds (r.removeFromBottom (124).reduced (16, 0).withTrimmedBottom (12));
    hardware.setBounds (r.reduced (8, 0).withTrimmedBottom (8));
    layoutInspector();
}

void ControlsPanel::layoutInspector()
{
    const int w = juce::jmax (100, viewport.getWidth() - viewport.getScrollBarThickness() - 4);
    int y = 0;
    for (auto& c : cards)
    {
        const int h = c->getIdealHeight();
        c->setBounds (0, y, w, h);
        y += h + 12;
    }
    content.setSize (w, juce::jmax (0, y - 12));
}

/* ---- selection and the inspector --------------------------------------------- */

void ControlsPanel::select (Part p)
{
    if (p == Part::toe)
        p = Part::pedal;
    selected = p;
    hardware.setSelected (p);
    viewport.setViewPosition (0, 0);
    rebuildInspector();
    repaint (inspectorHeader);
}

/* What the inspector's rows depend on: when this changes the cards are built
 * again; otherwise their values are refreshed in place, so an editor being
 * dragged or typed in is never destroyed under the pointer. */
juce::String ControlsPanel::layoutKey() const
{
    const auto& cfg = session.config();
    const auto ids = HardwareView::idsOf (selected);
    juce::String key;
    key << (int) selected << "/" << (int) cfg.global.midi_channel << "/" << (int) cfg.global.bank_size
        << "/" << (int) cfg.global.display_timeout << "/" << (session.isConnected() ? 1 : 0);
    if (ids.button >= 0)
        key << "/b" << (int) cfg.buttons[ids.button].action;
    if (ids.encoder >= 0)
        key << "/e" << (int) cfg.encoders[ids.encoder].mode;
    if (ids.analog >= 0)
        key << "/a" << (int) cfg.analogs[ids.analog].mode;
    return key;
}

void ControlsPanel::rebuildInspector()
{
    refreshers.clear();
    cards.clear();
    builtKey = layoutKey();

    const auto ids = HardwareView::idsOf (selected);
    if (selected == Part::screen)
        addGlobalCards();
    else
    {
        if (ids.encoder >= 0)
            addEncoderCard (ids.encoder);
        if (ids.analog >= 0)
            addAnalogCard (ids.analog);
        if (ids.button >= 0)
            addButtonCards (ids.button, ids.encoder >= 0 ? "Push" : ids.analog >= 0 ? "Toe switch" : "Switch");
    }

    for (auto& c : cards)
        content.addAndMakeVisible (*c);
    layoutInspector();
    refreshInspector();
}

void ControlsPanel::refreshInspector()
{
    for (auto& r : refreshers)
        r();
}

InspectorCard& ControlsPanel::addCard (const juce::String& title, const juce::String& subtitle)
{
    cards.push_back (std::make_unique<InspectorCard> (title, subtitle));
    return *cards.back();
}

void ControlsPanel::edited()
{
    updateView();
    session.configEdited();
    triggerAsyncUpdate();
}

/* ---- rows --------------------------------------------------------------------- */

void ControlsPanel::addChoice (InspectorCard& card, const juce::String& label, const juce::StringArray& names,
                               std::function<int()> get, std::function<void (int)> set)
{
    auto& box = card.add (label, std::make_unique<juce::ComboBox>());
    box.addItemList (names, 1);
    auto* b = &box;
    box.onChange = [this, b, set]
    {
        set (b->getSelectedId() - 1);
        edited();
    };
    refreshers.push_back ([b, get] { b->setSelectedId (get() + 1, juce::dontSendNotification); });
}

void ControlsPanel::addNumber (InspectorCard& card, const juce::String& label, int lo, int hi,
                               std::function<int()> get, std::function<void (int)> set,
                               std::function<juce::String (int)> format)
{
    auto& slider = card.add (label, std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal,
                                                                     juce::Slider::TextBoxRight));
    slider.setRange (lo, hi, 1);
    slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, format ? 84 : 50, 24);
    if (format)
    {
        slider.textFromValueFunction = [format] (double v) { return format ((int) v); };
        slider.valueFromTextFunction = [] (const juce::String& t) { return (double) t.getIntValue(); };
    }
    auto* s = &slider;
    slider.onValueChange = [this, s, set]
    {
        set ((int) s->getValue());
        edited();
    };
    refreshers.push_back ([s, get]
    {
        if (! s->isMouseButtonDown())
            s->setValue (get(), juce::dontSendNotification);
        s->updateText();
    });
}

void ControlsPanel::addToggle (InspectorCard& card, const juce::String& label, std::function<bool()> get,
                               std::function<void (bool)> set)
{
    auto& toggle = card.add (label, std::make_unique<juce::ToggleButton>());
    auto* t = &toggle;
    toggle.onClick = [this, t, set]
    {
        set (t->getToggleState());
        edited();
    };
    refreshers.push_back ([t, get] { t->setToggleState (get(), juce::dontSendNotification); });
}

void ControlsPanel::addLeds (InspectorCard& card, const juce::String& label, bool lamps, std::function<int()> get,
                             std::function<void (int)> set)
{
    auto& picker = card.add (label, std::make_unique<LedPicker> (lamps), 34, true);
    auto* p = &picker;
    picker.onChange = [this, set] (int v)
    {
        set (v);
        edited();
    };
    refreshers.push_back ([p, get] { p->setValue (get()); });
}

/* ---- cards -------------------------------------------------------------------- */

void ControlsPanel::addGlobalCards()
{
    auto& device = addCard ("Device", "shared by every control");

    auto& name = device.add ("Name on the screen", std::make_unique<juce::TextEditor>());
    name.setInputRestrictions ((int) MIDI_CONTROL_CONFIG_NAME_LEN - 1);
    name.setFont (theme::ui (14.0f));
    name.setIndents (8, 6);
    auto* n = &name;
    name.onTextChange = [this, n]
    {
        auto& cfg = session.config();
        const auto text = n->getText().toUpperCase().substring (0, (int) MIDI_CONTROL_CONFIG_NAME_LEN - 1);
        std::memset (cfg.global.name, 0, MIDI_CONTROL_CONFIG_NAME_LEN);
        std::memcpy (cfg.global.name, text.toRawUTF8(), juce::jmin ((size_t) MIDI_CONTROL_CONFIG_NAME_LEN - 1,
                                                                   text.getNumBytesAsUTF8()));
        edited();
    };
    refreshers.push_back ([this, n]
    {
        if (! n->hasKeyboardFocus (true))
            n->setText (juce::String (session.config().global.name, MIDI_CONTROL_CONFIG_NAME_LEN)
                            .trimCharactersAtEnd (juce::String::charToString (0)),
                        juce::dontSendNotification);
    });

    juce::StringArray channels;
    for (int i = 1; i <= 16; ++i)
        channels.add ("Channel " + juce::String (i));
    addChoice (device, "MIDI channel", channels, [this] { return (int) session.config().global.midi_channel; },
               [this] (int v) { session.config().global.midi_channel = (uint8_t) v; });
    addNumber (device, "Presets per bank", 1, 16, [this] { return (int) session.config().global.bank_size; },
               [this] (int v) { session.config().global.bank_size = (uint8_t) v; });
    addNumber (device, "Program at power-on", 0, 127,
               [this] { return (int) session.config().global.startup_program; },
               [this] (int v) { session.config().global.startup_program = (uint8_t) v; },
               [this] (int v) { return juce::String (v) + "  " + mapping::programLabel (session.config(), v); });
    addToggle (device, "Bank Select first", [this] { return session.config().global.bank_select != 0; },
               [this] (bool v) { session.config().global.bank_select = v ? 1 : 0; });
    device.addNote ("Sends CC#0 and CC#32 = 0 before every Program Change, for receivers that expect them.");
    addToggle (device, "Follow the host", [this] { return session.config().global.follow_program_change != 0; },
               [this] (bool v) { session.config().global.follow_program_change = v ? 1 : 0; });
    device.addNote ("A Program Change from the host moves the screen and the preset LEDs.");

    /* Seconds, 0 for never. A time set some other way, from a file say, is listed as itself. */
    std::vector<int> timeouts { 0, 30, 60, 120, 300, 600, 1800, 3600 };
    const int timeout = (int) session.config().global.display_timeout;
    if (std::find (timeouts.begin(), timeouts.end(), timeout) == timeouts.end())
        timeouts.insert (std::upper_bound (timeouts.begin(), timeouts.end(), timeout), timeout);
    juce::StringArray timeoutNames;
    for (const int t : timeouts)
        timeoutNames.add (durationName (t));
    addChoice (device, "Screen off after", timeoutNames,
               [this, timeouts]
               {
                   const auto it = std::find (timeouts.begin(), timeouts.end(),
                                              (int) session.config().global.display_timeout);
                   return it == timeouts.end() ? -1 : (int) (it - timeouts.begin());
               },
               [this, timeouts] (int v)
               {
                   if (v >= 0 && v < (int) timeouts.size())
                       session.config().global.display_timeout = (uint16_t) timeouts[(size_t) v];
               });
    device.addNote ("Dark when no control is used for this long. Any control lights it again, and still does its job.", 40);

    auto& text = addCard ("Screen text", "sent now, never saved");
    text.addNote ("The host normally sends these two lines, such as the preset's name. Try them here.", 40);
    for (int line = 0; line < 2; ++line)
    {
        auto& editor = text.add ("Line " + juce::String (line + 1), std::make_unique<juce::TextEditor>());
        editor.setInputRestrictions ((int) MIDI_CONTROL_TEXT_LEN);
        editor.setFont (theme::ui (14.0f));
        editor.setIndents (8, 6);
        editor.setText (hostText[line], juce::dontSendNotification);
        auto* e = &editor;
        editor.onTextChange = [this, e, line] { hostText[line] = e->getText(); };
    }
    auto& send = text.add ({}, std::make_unique<juce::TextButton> ("Send to the screen"), 38);
    send.setEnabled (session.isConnected());
    send.onClick = [this]
    {
        for (int line = 0; line < 2; ++line)
            session.setText (line, hostText[line]);
    };
}

void ControlsPanel::addButtonCards (int id, const juce::String& title)
{
    const auto& cfg = session.config();
    const auto& b = cfg.buttons[id];
    auto& card = addCard (title);

    juce::StringArray actions;
    for (unsigned a = 0; a < MIDI_CONTROL_BUTTON_ACTION_COUNT; ++a)
        actions.add (midi_control_button_action_name (a));
    addChoice (card, "Action", actions, [this, id] { return (int) session.config().buttons[id].action; },
               [this, id] (int v) { session.config().buttons[id].action = (uint8_t) v; });
    if (b.action == MIDI_CONTROL_ACT_NONE)
        return;

    addChoice (card, "Channel", channelNames (cfg), [this, id] { return (int) session.config().buttons[id].channel; },
               [this, id] (int v) { session.config().buttons[id].channel = (uint8_t) v; });

    const auto label = numberLabel (b.action);
    if (label.isNotEmpty())
    {
        const bool inBank = b.action == MIDI_CONTROL_ACT_PRESET_IN_BANK;
        const bool note = b.action == MIDI_CONTROL_ACT_NOTE_MOMENTARY || b.action == MIDI_CONTROL_ACT_NOTE_TOGGLE;
        const bool program = b.action == MIDI_CONTROL_ACT_PROGRAM;
        std::function<juce::String (int)> format;
        if (note)
            format = [] (int v) { return juce::String (v) + "  " + mapping::noteName (v); };
        else if (program)
            format = [this] (int v) { return juce::String (v) + "  " + mapping::programLabel (session.config(), v); };
        else if (inBank)
            format = [this] (int v) { return mapping::programLabel (session.config(), v).getLastCharacters (1); };
        addNumber (card, label, inBank ? 0 : (b.action == MIDI_CONTROL_ACT_PROGRAM_UP || b.action == MIDI_CONTROL_ACT_PROGRAM_DOWN ? 1 : 0),
                   inBank ? juce::jmax (0, cfg.global.bank_size - 1) : 127,
                   [this, id] { return (int) session.config().buttons[id].number; },
                   [this, id] (int v) { session.config().buttons[id].number = (uint8_t) v; }, format);
    }

    const bool isNote = b.action == MIDI_CONTROL_ACT_NOTE_MOMENTARY || b.action == MIDI_CONTROL_ACT_NOTE_TOGGLE;
    const bool isCC = b.action == MIDI_CONTROL_ACT_CC_MOMENTARY || b.action == MIDI_CONTROL_ACT_CC_TOGGLE;
    if (isNote || isCC)
        addNumber (card, isNote ? "Velocity" : "Value when on", 0, 127,
                   [this, id] { return (int) session.config().buttons[id].value_on; },
                   [this, id] (int v) { session.config().buttons[id].value_on = (uint8_t) v; });
    if (isCC)
        addNumber (card, "Value when off", 0, 127,
                   [this, id] { return (int) session.config().buttons[id].value_off; },
                   [this, id] (int v) { session.config().buttons[id].value_off = (uint8_t) v; });

    const int column = midi_control_button_led_column ((midi_control_button) id);
    if (column < 0)
        return;
    const bool lamps = column == 6;
    auto& leds = addCard (lamps ? "Lamps" : "LED", lamps ? "the Vol, Wah and Tap lamps" : "the ring round the switch");
    addLeds (leds, "While active", lamps, [this, id] { return (int) session.config().buttons[id].led_on; },
             [this, id] (int v) { session.config().buttons[id].led_on = (uint8_t) v; });
    addLeds (leds, "Otherwise", lamps, [this, id] { return (int) session.config().buttons[id].led_off; },
             [this, id] (int v) { session.config().buttons[id].led_off = (uint8_t) v; });
    leds.addNote ("Active means held, toggled on, or the preset in use, depending on the action.");
}

void ControlsPanel::addEncoderCard (int id)
{
    const auto& cfg = session.config();
    const auto& e = cfg.encoders[id];
    auto& card = addCard ("Turn");

    juce::StringArray modes;
    for (unsigned m = 0; m < MIDI_CONTROL_ENCODER_MODE_COUNT; ++m)
        modes.add (midi_control_encoder_mode_name (m));
    addChoice (card, "Mode", modes, [this, id] { return (int) session.config().encoders[id].mode; },
               [this, id] (int v) { session.config().encoders[id].mode = (uint8_t) v; });
    if (e.mode == MIDI_CONTROL_ENCM_NONE)
        return;

    addChoice (card, "Channel", channelNames (cfg), [this, id] { return (int) session.config().encoders[id].channel; },
               [this, id] (int v) { session.config().encoders[id].channel = (uint8_t) v; });
    if (e.mode != MIDI_CONTROL_ENCM_PROGRAM)
        addNumber (card, "CC number", 0, 127, [this, id] { return (int) session.config().encoders[id].number; },
                   [this, id] (int v) { session.config().encoders[id].number = (uint8_t) v; });
    addNumber (card, "Step per detent", 1, 64, [this, id] { return juce::jmax (1, (int) session.config().encoders[id].step); },
               [this, id] (int v) { session.config().encoders[id].step = (uint8_t) v; });
    if (e.mode == MIDI_CONTROL_ENCM_CC_ABSOLUTE)
        addNumber (card, "Value at power-on", 0, 127, [this, id] { return (int) session.config().encoders[id].value; },
                   [this, id] (int v) { session.config().encoders[id].value = (uint8_t) v; });
}

void ControlsPanel::addAnalogCard (int id)
{
    const auto& cfg = session.config();
    const auto& a = cfg.analogs[id];
    auto& card = addCard (id == MIDI_CONTROL_ANALOG_PEDAL ? "Pedal" : "Knob");

    juce::StringArray modes;
    for (unsigned m = 0; m < MIDI_CONTROL_ANALOG_MODE_COUNT; ++m)
        modes.add (midi_control_analog_mode_name (m));
    addChoice (card, "Mode", modes, [this, id] { return (int) session.config().analogs[id].mode; },
               [this, id] (int v) { session.config().analogs[id].mode = (uint8_t) v; });
    if (a.mode == MIDI_CONTROL_ANM_NONE)
        return;

    addChoice (card, "Channel", channelNames (cfg), [this, id] { return (int) session.config().analogs[id].channel; },
               [this, id] (int v) { session.config().analogs[id].channel = (uint8_t) v; });
    if (a.mode == MIDI_CONTROL_ANM_CC)
        addNumber (card, "CC number", 0, 127, [this, id] { return (int) session.config().analogs[id].number; },
                   [this, id] (int v) { session.config().analogs[id].number = (uint8_t) v; });
    addNumber (card, "Lowest output", 0, 127, [this, id] { return (int) session.config().analogs[id].out_min; },
               [this, id] (int v) { session.config().analogs[id].out_min = (uint8_t) v; });
    addNumber (card, "Highest output", 0, 127, [this, id] { return (int) session.config().analogs[id].out_max; },
               [this, id] (int v) { session.config().analogs[id].out_max = (uint8_t) v; });
    addToggle (card, "Invert", [this, id] { return session.config().analogs[id].invert != 0; },
               [this, id] (bool v) { session.config().analogs[id].invert = v ? 1 : 0; });
    card.addNote (id == MIDI_CONTROL_ANALOG_PEDAL ? "Inverted, heel down sends the highest output."
                                                  : "Inverted, fully anticlockwise sends the highest output.");
}

/* ---- the drawn unit ------------------------------------------------------------ */

void ControlsPanel::updateView()
{
    const auto& cfg = session.config();
    HardwareView::State s = hardware.getState();

    /* The device as it last reported itself; without a report, a unit at rest
     * under these settings. Either is drawn under the configuration being
     * edited, which with live apply on is the device's own. */
    midi_control_state_reply st;
    if (playable && session.hasState())
    {
        st = session.state();
        const auto now = juce::Time::getMillisecondCounter();
        for (int b = 0; b < MIDI_CONTROL_BUTTON_COUNT; ++b)
            if (now < tapUntil[b])
                st.held[b] = 1;
    }
    else
    {
        const auto& id = session.identity();
        const bool on = session.isConnected();
        st = ScreenPreview::atRest (cfg, on ? session.status().program : cfg.global.startup_program,
                                    on ? id.storage_state : MIDI_CONTROL_STORAGE_NONE, on && id.dirty);
    }

    const bool screenChanged = preview.render (cfg, st, session.isConnected());
    s.screen = preview.image();
    const auto& dev = preview.device();
    std::memcpy (s.leds, midi_control_device_leds (&dev), sizeof (s.leds));
    for (int b = 0; b < MIDI_CONTROL_BUTTON_COUNT; ++b)
    {
        s.held[b] = dev.engine.held[b] != 0;
        s.active[b] = midi_control_engine_button_active (&dev.engine, (midi_control_button) b) != 0;
    }
    for (int a = 0; a < MIDI_CONTROL_ANALOG_COUNT; ++a)
        s.analog[a] = dev.engine.analog_in[a];
    std::copy (std::begin (turns), std::end (turns), std::begin (s.turns));

    setCaptions (s, cfg);
    hardware.setState (s, screenChanged);
}

/* A link that reports its state makes the drawing that device, played; anything
 * else makes it a picture of the configuration. The emulator's strip shows
 * while the emulator is the link, whether or not it is connected yet. */
void ControlsPanel::linkChanged()
{
    const bool nowEmulated = session.getLink() == &emulator;
    const bool nowPlayable = session.canPlay();
    if (nowEmulated == emulated && nowPlayable == playable)
        return;
    emulated = nowEmulated;
    playable = nowPlayable;
    hardware.setPlayable (playable);
    strip.setVisible (emulated);
    if (playable)
        startTimerHz (30);
    else
        stopTimer();
    resized();
    repaint();
}

/* The live view's pulse: ask for the state, and the reply redraws. Not while
 * another view is showing. */
void ControlsPanel::timerCallback()
{
    if (isVisible())
        session.requestState();
}

void ControlsPanel::sessionChanged (ConfigSession&)
{
    /* A connection's counters start from nothing; the first report after it
     * is where they are counted from, not a burst of turns and taps. */
    if (! session.hasState())
        haveCounts = false;
    triggerAsyncUpdate();
}

/* The device reported its state. Its counters say what happened since the last
 * report: the knobs turn by the detents counted, and a switch pressed since is
 * drawn down for a moment even if it is already up again. */
void ControlsPanel::sessionState (ConfigSession&)
{
    const auto& st = session.state();
    if (haveCounts)
    {
        const auto now = juce::Time::getMillisecondCounter();
        for (int e = 0; e < MIDI_CONTROL_ENCODER_COUNT; ++e)
            turns[e] += (((int) st.enc_turns[e] - (int) lastTurns[e] + 64) & 0x7f) - 64;
        for (int b = 0; b < MIDI_CONTROL_BUTTON_COUNT; ++b)
            if (st.presses[b] != lastPresses[b])
                tapUntil[b] = now + kTapMs;
    }
    std::copy (std::begin (st.enc_turns), std::end (st.enc_turns), std::begin (lastTurns));
    std::copy (std::begin (st.presses), std::end (st.presses), std::begin (lastPresses));
    haveCounts = true;
    updateView();
}

void ControlsPanel::handleAsyncUpdate()
{
    linkChanged();
    if (layoutKey() != builtKey)
        rebuildInspector();
    else
        refreshInspector();
    updateView();
    updateButtons();
}

void ControlsPanel::updateButtons()
{
    const bool on = session.isConnected();
    const auto storage = session.identity().storage_state;
    readButton.setEnabled (on);
    applyButton.setEnabled (on);
    saveButton.setEnabled (on && storage == MIDI_CONTROL_STORAGE_READY);
    reloadButton.setEnabled (on && storage != MIDI_CONTROL_STORAGE_NONE);
    defaultsButton.setEnabled (on);
    saveButton.setButtonText (on && session.identity().dirty ? "Save to device *" : "Save to device");
    theme::setVariant (saveButton, on && session.identity().dirty ? "primary" : "");
}

/* ---- files -------------------------------------------------------------------- */

void ControlsPanel::importJson()
{
    chooser = std::make_unique<juce::FileChooser> ("Import configuration",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   "*.json");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (! f.existsAsFile())
            return;
        midi_control_config c = session.config();
        juce::String err;
        if (! ConfigJson::fromJson (f.loadFileAsString(), c, err))
        {
            if (onMessage)
                onMessage ("import failed: " + err);
            return;
        }
        session.replaceConfig (c);
        triggerAsyncUpdate();
        if (onMessage)
            onMessage ("imported " + f.getFileName());
    });
}

void ControlsPanel::exportJson()
{
    chooser = std::make_unique<juce::FileChooser> ("Export configuration",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                       .getChildFile ("midi-control-config.json"),
                                                   "*.json");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == juce::File())
            return;
        const bool ok = f.replaceWithText (ConfigJson::toJson (session.config()));
        if (onMessage)
            onMessage (ok ? "exported " + f.getFileName() : "could not write " + f.getFullPathName());
    });
}
