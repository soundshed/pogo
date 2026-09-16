#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "ConfigSession.h"
#include "EmulatorLink.h"
#include "MidiDeviceLink.h"
#include "ui/ControlsPanel.h"
#include "ui/FirmwarePanel.h"
#include "ui/MidiMonitor.h"

/* The window: a header with the product name, the three views and the device
 * link; the view; and a status bar with the link's state, as the unit's own
 * screen shows its state, and the latest message. */
class MainComponent : public juce::Component,
                      private DeviceLink::Listener,
                      private ConfigSession::Listener,
                      private juce::Timer
{
public:
    /* Offline, the only link is the emulator: no MIDI port is listed or
     * opened, so nothing another process has open is touched. */
    explicit MainComponent (bool offline = false);
    ~MainComponent() override;

    /* The snapshot mode (Main.cpp): play the emulator over the link so it has
     * something to show, then draw every view into `dir` as <view>.png. */
    void stageSnapshots();
    void saveSnapshots (const juce::File& dir, float scale);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum class View { configure, monitor, firmware };

    void showView (View v);
    void refreshLinks();
    void selectLink();
    void setMessage (const juce::String& text);

    void deviceMidiReceived (DeviceLink&, const juce::MidiMessage&) override;
    void deviceLinkChanged (DeviceLink&) override;
    void sessionChanged (ConfigSession&) override;
    void sessionMessage (ConfigSession&, const juce::String&) override;
    void sessionSent (ConfigSession&, const juce::MidiMessage&) override;
    void timerCallback() override;

    bool isAnalogTraffic (const juce::MidiMessage& m) const;

    EmulatorLink emulator;
    MidiDeviceLink midiLink;
    juce::Array<MidiDeviceLink::PortPair> ports;
    ConfigSession session;

    juce::TextButton navConfigure { "Configure" }, navMonitor { "Monitor" }, navFirmware { "Firmware" };
    juce::ComboBox linkBox;
    juce::TextButton connectButton { "Connect" };
    juce::TextButton refreshButton { "Refresh" };

    ControlsPanel controls { session, emulator };
    FirmwarePanel firmware;
    MidiMonitor monitor;

    juce::String message;
    juce::Rectangle<int> header, footer;
    juce::TooltipWindow tooltips { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
