#include "MidiMonitor.h"
#include "Theme.h"

static constexpr size_t kMaxEntries = 5000;

namespace
{
juce::Colour kindColour (MidiKind k)
{
    switch (k)
    {
        case MidiKind::noteOn:   return juce::Colour (0xffc58af9);
        case MidiKind::noteOff:  return juce::Colour (0xff8f72b8);
        case MidiKind::cc:       return theme::info;
        case MidiKind::program:  return theme::accent;
        case MidiKind::ours:     return juce::Colour (0xff32d6ec);
        case MidiKind::bend:     return theme::warn;
        case MidiKind::pressure: return juce::Colour (0xffff7eb6);
        default:                 return theme::inkDim;
    }
}

void tag (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, juce::Colour c)
{
    g.setColour (c.withAlpha (0.14f));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (c);
    g.setFont (theme::heading (12.0f));
    g.drawText (text, r, juce::Justification::centred, false);
}

/* Column positions, shared by the header and the rows. */
constexpr int kTimeX = 14, kDirX = 112, kKindX = 162, kChannelX = 262, kTextX = 312, kHexWidth = 300;
} // namespace

MidiMonitor::MidiMonitor()
{
    list.setModel (this);
    list.setRowHeight (26);
    list.setColour (juce::ListBox::backgroundColourId, theme::surface);
    addAndMakeVisible (list);

    clearButton.onClick = [this] { clear(); };
    theme::setVariant (clearButton, "ghost");
    addAndMakeVisible (clearButton);

    followToggle.setToggleState (true, juce::dontSendNotification);
    followToggle.onClick = [this] { pending = true; };
    hideAnalogToggle.setToggleState (true, juce::dontSendNotification);
    for (auto* t : { &followToggle, &hideAnalogToggle, &hideSysexToggle })
        addAndMakeVisible (t);

    startTimerHz (15);
}

MidiMonitor::~MidiMonitor()
{
    list.setModel (nullptr);
}

void MidiMonitor::add (Entry e)
{
    const auto now = juce::Time::getCurrentTime();
    e.time = now.formatted ("%H:%M:%S") + "." + juce::String (now.getMilliseconds()).paddedLeft ('0', 3);
    entries.push_back (std::move (e));
    while (entries.size() > kMaxEntries)
        entries.pop_front();
    pending = true;
}

void MidiMonitor::addMidi (Direction direction, const juce::MidiMessage& m)
{
    if (isLiveViewTraffic (m))
        return;
    Entry e;
    e.direction = direction;
    e.kind = midiKind (m);
    if (e.kind == MidiKind::ours && hideSysexToggle.getToggleState())
        return;
    e.channel = m.getChannel();
    e.summary = midiSummary (m);
    e.hex = midiHex (m);
    e.problem = e.summary.containsIgnoreCase ("RESULT") && ! e.summary.endsWithIgnoreCase (": ok");
    (direction == Direction::in ? inCount : outCount)++;
    add (std::move (e));
}

void MidiMonitor::addNote (const juce::String& text)
{
    Entry e;
    e.summary = text;
    for (auto* word : { "fail", "reject", "no reply", "could not", "refus", "locked", "error" })
        e.problem = e.problem || text.containsIgnoreCase (word);
    add (std::move (e));
}

void MidiMonitor::clear()
{
    entries.clear();
    inCount = outCount = 0;
    list.updateContent();
    repaint();
}

void MidiMonitor::timerCallback()
{
    if (! pending)
        return;
    pending = false;
    list.updateContent();
    if (followToggle.getToggleState() && ! entries.empty())
        list.scrollToEnsureRowIsOnscreen ((int) entries.size() - 1);
    repaint (getLocalBounds().removeFromTop (60));
}

void MidiMonitor::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);
    auto r = getLocalBounds().reduced (16, 0);
    auto top = r.removeFromTop (60);
    g.setColour (theme::ink);
    g.setFont (theme::heading (22.0f));
    g.drawText ("MIDI monitor", top.removeFromLeft (170), juce::Justification::centredLeft, false);

    auto chips = top.removeFromLeft (360).toFloat().withSizeKeepingCentre (360.0f, 26.0f);
    auto chip = [&] (const juce::String& text, juce::Colour dot)
    {
        const float w = theme::chipWidth (text, 26.0f, true);
        theme::drawChip (g, chips.removeFromLeft (w), text, theme::inkDim, dot);
        chips.removeFromLeft (8.0f);
    };
    chip ("IN " + juce::String (inCount), theme::good);
    chip ("OUT " + juce::String (outCount), theme::accent);
    chip (juce::String ((int) entries.size()) + " ROWS", theme::inkFaint);

    /* Column headings over the list. */
    const auto head = list.getBounds().withHeight (24).translated (0, -24);
    g.setColour (theme::inkFaint);
    g.setFont (theme::heading (11.5f));
    auto col = [&] (const char* text, int x, int w)
    {
        g.drawText (text, head.getX() + x, head.getY(), w, head.getHeight(), juce::Justification::centredLeft, false);
    };
    col ("TIME", kTimeX, 90);
    col ("DIR", kDirX + 6, 40);
    col ("TYPE", kKindX + 8, 80);
    col ("CH", kChannelX, 40);
    col ("MESSAGE", kTextX, 100);
    g.drawText ("BYTES", head.getRight() - kHexWidth - 14, head.getY(), kHexWidth, head.getHeight(),
                juce::Justification::centredLeft, false);
}

void MidiMonitor::resized()
{
    auto r = getLocalBounds().reduced (16, 0);
    auto top = r.removeFromTop (60).withSizeKeepingCentre (r.getWidth(), 30);
    clearButton.setBounds (top.removeFromRight (80));
    top.removeFromRight (16);
    hideSysexToggle.setBounds (top.removeFromRight (210));
    hideAnalogToggle.setBounds (top.removeFromRight (190));
    followToggle.setBounds (top.removeFromRight (90));
    r.removeFromTop (24);
    list.setBounds (r.withTrimmedBottom (16));
}

void MidiMonitor::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= (int) entries.size())
        return;
    const auto& e = entries[(size_t) row];

    if (selected)
        g.fillAll (theme::accent.withAlpha (0.12f));
    else if (row % 2)
        g.fillAll (theme::raised.withAlpha (0.5f));

    const float h = (float) height;
    g.setColour (theme::inkFaint);
    g.setFont (theme::mono (13.0f));
    g.drawText (e.time, kTimeX, 0, 96, height, juce::Justification::centredLeft, false);

    if (e.direction == Direction::note)
    {
        tag (g, { (float) kDirX, 4.0f, 40.0f, h - 8.0f }, "APP", theme::inkFaint);
        g.setColour (e.problem ? theme::bad : theme::inkDim);
        g.setFont (theme::ui (14.0f));
        g.drawText (e.summary, kKindX + 8, 0, width - kKindX - 20, height, juce::Justification::centredLeft, true);
        return;
    }

    const bool in = e.direction == Direction::in;
    tag (g, { (float) kDirX, 4.0f, 40.0f, h - 8.0f }, in ? "IN" : "OUT", in ? theme::good : theme::accent);
    tag (g, { (float) kKindX, 4.0f, 88.0f, h - 8.0f }, midiKindName (e.kind), kindColour (e.kind));

    g.setColour (theme::inkDim);
    g.setFont (theme::ui (13.5f));
    if (e.channel > 0)
        g.drawText (juce::String (e.channel), kChannelX, 0, 40, height, juce::Justification::centredLeft, false);

    g.setColour (e.problem ? theme::bad : theme::ink);
    g.setFont (theme::ui (14.0f));
    g.drawText (e.summary, kTextX, 0, width - kTextX - kHexWidth - 24, height, juce::Justification::centredLeft, true);

    g.setColour (theme::inkFaint);
    g.setFont (theme::mono (12.5f));
    g.drawText (e.hex, width - kHexWidth - 14, 0, kHexWidth, height, juce::Justification::centredLeft, true);
}
