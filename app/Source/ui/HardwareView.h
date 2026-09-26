#pragma once

#include <array>
#include <juce_gui_basics/juce_gui_basics.h>
#include "midi_control/midi_control_controls.h"

/* The unit, drawn from above: the top panel with its screen, knobs and
 * buttons, the footswitch deck, the expression pedal. Vector drawing on a fixed
 * canvas scaled to fit, so it is sharp at any size; the parts that never change
 * are drawn once into an image at the display's pixel density.
 *
 * Whatever is clicked is selected, for the editor. While the view is playable
 * (the device on the link can be played) the same gestures also play the unit:
 * a switch is held for as long as the mouse button is, a knob turns when
 * dragged or scrolled and pushes when clicked, the volume knob and the pedal
 * drag, the front of the pedal is the toe switch. The owner says what to draw
 * through `State` and hears the gestures through the callbacks; the view knows
 * no device. Two things are drawn ahead of `State`, because it is a round trip
 * behind: a switch looks pressed from the moment it is clicked, and the volume
 * knob or pedal being dragged or scrolled keeps the position given it, while it
 * moves and for a moment after. */
class HardwareView : public juce::Component,
                     public juce::TooltipClient
{
public:
    /* The things on the panel a player touches. */
    enum class Part
    {
        none,
        screen,
        knob1, knob2, knob3, knob4, knob5,
        upperKnob, lowerKnob, volumeKnob,
        view, action, pagePrevious, pageNext,
        bankUp, bankDown, fsA, fsB, fsC, fsD, mode, tap,
        pedal, toe,
        count
    };

    /* The firmware's ids for a part: its switch, encoder, analog input and LED
     * column, -1 where it has none. The table is in docs/08 section 2.1. */
    struct Ids
    {
        int button = -1, encoder = -1, analog = -1, ledColumn = -1;
    };
    static Ids idsOf (Part);
    static juce::String nameOf (Part);

    struct Caption
    {
        juce::String what, detail;
        bool assigned = false;

        bool operator== (const Caption& o) const { return what == o.what && detail == o.detail && assigned == o.assigned; }
        bool operator!= (const Caption& o) const { return ! operator== (o); }
    };

    struct State
    {
        juce::Image screen;                                  /* 480x272; invalid draws dark glass */
        uint8_t leds[MIDI_CONTROL_COLUMNS] {};               /* MIDI_CONTROL_LED_* per column */
        bool held[MIDI_CONTROL_BUTTON_COUNT] {};
        bool active[MIDI_CONTROL_BUTTON_COUNT] {};           /* the engine's "active" */
        int analog[MIDI_CONTROL_ANALOG_COUNT] {};            /* 0..127 */
        /* Where the pedal physically is, 0..127, when that is not what the
         * device reports - the emulated board's pedal, which a dragged pedal
         * moves. The pedal is drawn there and the device's position shown
         * beside it. -1: the pedal is where the device reports it. */
        int pedalPhysical = -1;
        /* What the pedal's position sends under the mapping - after its
         * padding, invert and range - shown in its caption. The position is
         * not the value: with padding, a pedal settled just off its stop is
         * short of 127 and still sends it. -1: it sends nothing. */
        int pedalSent = -1;
        /* The pedal's padding, percent of the travel at the heel and the toe
         * that sends the end value (docs/05 section 5). Drawn on its travel
         * track. 0 when there is none, or the pedal sends nothing. */
        int pedalPadLo = 0, pedalPadHi = 0;
        int turns[MIDI_CONTROL_ENCODER_COUNT] {};            /* each knob's pointer, in detents */
        std::array<Caption, (size_t) Part::count> captions;  /* what each part does */

        /* Everything but the screen's pixels. */
        bool sameAs (const State& other) const;
    };

    HardwareView();
    ~HardwareView() override;

    /* Replaces what is drawn and repaints if anything differs; `screenChanged`
     * says the screen image's pixels changed in place. */
    void setState (const State& s, bool screenChanged = false);
    const State& getState() const { return state; }

    void setSelected (Part p);
    Part getSelected() const { return selected; }

    /* Whether gestures play the unit as well as select. Turning it off lets go
     * of a switch being held. */
    void setPlayable (bool shouldPlay);
    bool isPlayable() const { return playable; }

    std::function<void (Part)> onSelect;                     /* a different part was touched */
    std::function<void (int button, bool down)> onButton;    /* while playable */
    std::function<void (int encoder, int detents)> onEncoder;
    std::function<void (int analog, int value)> onAnalog;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    juce::String getTooltip() override;

private:
    /* `onCaption` is set when the point is on a footswitch's caption rather than
     * the switch: that selects it without pressing it. */
    Part partAt (juce::Point<float> local, bool* onCaption = nullptr) const;
    void setHovered (Part p);
    void pushEncoder (Part p);
    bool isDown (int button) const;
    void drawChassis (juce::Graphics&) const;
    void drawLive (juce::Graphics&) const;
    void drawCaptions (juce::Graphics&) const;
    void drawHighlight (juce::Graphics&, Part p, juce::Colour colour, bool strong) const;

    bool playable = false;
    State state;
    Part selected = Part::none, hovered = Part::none;

    juce::AffineTransform transform;        /* canvas -> component */
    juce::Image chassis;                    /* the parts that never change */

    Part dragPart = Part::none;
    int pressedButton = -1;
    juce::uint32 analogLocalUntil[MIDI_CONTROL_ANALOG_COUNT] {};   /* moved from here: keep it until then */
    float dragAccum = 0.0f, dragTravel = 0.0f;
    float dragValue = 0.0f;
    juce::Point<float> lastDrag;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HardwareView)
};
