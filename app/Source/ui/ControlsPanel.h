#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ConfigSession.h"
#include "EmulatorLink.h"
#include "EmulatorStrip.h"
#include "HardwareView.h"
#include "Inspector.h"
#include "PedalCalibration.h"
#include "ScreenPreview.h"

/* The Configure view: the unit, to set up and to play. The unit is drawn on the
 * left and every control on it can be clicked; the clicked control's settings
 * open on the right, and the captions beside the controls say what each sends.
 *
 * When the device on the link reports its state (protocol 2), the drawing is
 * that device, live, the emulator and a unit alike: the state is asked for
 * thirty times a second and the screen, LEDs, switches, knobs and pedal are
 * drawn from it, so using the unit shows here; and the clicks that select a
 * control also use it on the device - a switch held, a knob dragged, scrolled
 * or pushed, the volume knob and the pedal moved. Otherwise the drawing only
 * selects, and shows what the configuration being edited puts on the unit's
 * screen. Either way the screen is drawn by the shared UI code.
 *
 * Edits go into the session's editing copy and, with live apply on, straight to
 * the device. The device operations run along the top; the strip under the
 * drawing holds the emulator's own controls while it is the link.
 *
 * Calibrating the pedal takes the inspector's place while it runs, opened from
 * the pedal's settings or by a calibration begun on the unit, so the drawing -
 * the unit's screen showing the same steps, or the emulator's pedal to move -
 * stays beside it. */
class ControlsPanel : public juce::Component,
                      private ConfigSession::Listener,
                      private juce::AsyncUpdater,
                      private juce::Timer
{
public:
    ControlsPanel (ConfigSession& session, EmulatorLink& emulator);
    ~ControlsPanel() override;

    /* Import and export results, for the status line. */
    std::function<void (const juce::String&)> onMessage;

    /* Opens a control's settings, as clicking it does. */
    void showControl (HardwareView::Part p) { select (p); }

    /* Catches up with the session now rather than on the next pass of the
     * message loop. */
    void syncWithSession() { handleUpdateNowIfNeeded(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using Part = HardwareView::Part;

    void sessionChanged (ConfigSession&) override;
    void sessionState (ConfigSession&) override;
    void handleAsyncUpdate() override;
    void timerCallback() override;

    void linkChanged();
    void showCalibration (bool show, bool start);
    void select (Part p);
    juce::String layoutKey() const;
    void rebuildInspector();
    void refreshInspector();
    void layoutInspector();
    void updateView();
    void updateButtons();
    void edited();

    InspectorCard& addCard (const juce::String& title, const juce::String& subtitle = {});
    void addProfileCard();
    void addGlobalCards();
    void addProgramNamesCard();
    int currentProgram() const;
    void addButtonCards (int button, const juce::String& title);
    void addHoldCard (int button);
    void addEncoderCard (int encoder);
    void addAnalogCard (int analog);

    void addChoice (InspectorCard&, const juce::String& label, const juce::StringArray& names,
                    std::function<int()> get, std::function<void (int)> set);
    void addNumber (InspectorCard&, const juce::String& label, int lo, int hi, std::function<int()> get,
                    std::function<void (int)> set, std::function<juce::String (int)> format = {});
    void addToggle (InspectorCard&, const juce::String& label, std::function<bool()> get,
                    std::function<void (bool)> set);
    void addLeds (InspectorCard&, const juce::String& label, bool lamps, std::function<int()> get,
                  std::function<void (int)> set);

    void importJson();
    void exportJson();

    ConfigSession& session;
    EmulatorLink& emulator;
    HardwareView hardware;
    EmulatorStrip strip { emulator };
    ScreenPreview preview;
    PedalCalibration calibration { session };
    bool calibrationRunning = false;   /* the last report had one taking the pedal's stops */
    bool emulated = false;      /* the emulator is the link: its strip shows */
    bool playable = false;      /* the link reports its state: the drawing is the device */
    Part selected = Part::fsA;

    /* The knobs' pointers in detents, and the counters the last report gave,
     * which the next one's are compared with. */
    int turns[MIDI_CONTROL_ENCODER_COUNT] {};
    uint8_t lastTurns[MIDI_CONTROL_ENCODER_COUNT] {};
    uint8_t lastPresses[MIDI_CONTROL_BUTTON_COUNT] {};
    juce::uint32 tapUntil[MIDI_CONTROL_BUTTON_COUNT] {};
    bool haveCounts = false;

    juce::Rectangle<int> inspectorHeader, hintArea;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<InspectorCard>> cards;
    std::vector<std::function<void()>> refreshers;
    juce::String builtKey;
    juce::String hostText[2];

    /* The program the preset names card is naming, and the unit's program as
     * last reported, which the card follows when it moves. */
    int namingProgram = 0;
    int followedProgram = -1;
    juce::TextEditor* programNameEditor = nullptr;   /* the card's, while it is built */

    juce::TextButton readButton { "Read" };
    juce::TextButton applyButton { "Send" };
    juce::TextButton saveButton { "Save to device" };
    juce::TextButton reloadButton { "Revert to saved" };
    juce::TextButton defaultsButton { "Defaults" };
    juce::TextButton importButton { "Import..." };
    juce::TextButton exportButton { "Export..." };
    juce::ToggleButton liveToggle { "Apply live" };

    std::unique_ptr<juce::FileChooser> chooser;
};
