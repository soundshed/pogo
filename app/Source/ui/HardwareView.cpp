#include <cmath>
#include <cstring>
#include "HardwareView.h"
#include "Theme.h"
#include "midi_control/midi_control_version.h"

/* ---- the canvas ---------------------------------------------------------------
 * Every coordinate below is on one fixed canvas, laid out after a top-down
 * photograph of the unit; resized() fits the canvas to the component. */

namespace
{
using Part = HardwareView::Part;

const juce::Rectangle<float> kCanvas { 18.0f, 74.0f, 1504.0f, 884.0f };
const juce::Rectangle<float> kTopPanel { 52.0f, 94.0f, 1431.0f, 390.0f };
const juce::Rectangle<float> kDeck { 38.0f, 476.0f, 1462.0f, 446.0f };
const juce::Rectangle<float> kScreenBezel { 212.0f, 122.0f, 422.0f, 244.0f };
const juce::Rectangle<float> kToe { 1180.0f, 287.0f, 289.0f, 106.0f };
const juce::Rectangle<float> kLampWah { 975.0f, 295.0f, 26.0f, 8.0f };
const juce::Rectangle<float> kLampVol { 975.0f, 322.0f, 26.0f, 8.0f };

constexpr float kKnobY = 420.0f, kKnobR = 31.0f;
const float kKnobX[5] = { 228.0f, 327.0f, 426.0f, 525.0f, 624.0f };
const juce::Point<float> kUpperKnob { 787.0f, 190.0f }, kLowerKnob { 787.0f, 314.0f };
constexpr float kNavKnobR = 28.0f;
const juce::Point<float> kVolume { 987.0f, 184.0f };
constexpr float kVolumeR = 42.0f;
constexpr float kSwitchR = 33.0f;

/* How long the volume knob or pedal keeps a position given it here, after the
 * gesture, while the device's report of it catches up. */
constexpr juce::uint32 kAnalogSettleMs = 300;

const juce::Colour kPrint { 0xffd9dce1 };
const juce::Colour kLamp { 0xff4ae05e };
const juce::Colour kTapLed { 0xffff3b30 };

bool isSwitch (Part p)      { return p >= Part::bankUp && p <= Part::tap; }
bool isParamKnob (Part p)   { return p >= Part::knob1 && p <= Part::knob5; }
bool isEncoderKnob (Part p) { return p >= Part::knob1 && p <= Part::lowerKnob; }
bool isNavButton (Part p)   { return p >= Part::view && p <= Part::pageNext; }

juce::Rectangle<float> circle (juce::Point<float> c, float r)
{
    return { c.x - r, c.y - r, r * 2.0f, r * 2.0f };
}

juce::Point<float> switchCentre (Part p)
{
    switch (p)
    {
        case Part::bankUp:   return { 145.0f, 575.0f };
        case Part::fsC:      return { 425.0f, 575.0f };
        case Part::fsD:      return { 705.0f, 575.0f };
        case Part::mode:     return { 987.0f, 575.0f };
        case Part::bankDown: return { 145.0f, 835.0f };
        case Part::fsA:      return { 425.0f, 835.0f };
        case Part::fsB:      return { 705.0f, 835.0f };
        case Part::tap:      return { 987.0f, 835.0f };
        default:             return {};
    }
}

/* The strip beside each footswitch saying what it does. */
juce::Rectangle<float> captionBounds (Part p)
{
    const auto c = switchCentre (p);
    return { c.x + 46.0f, c.y - 30.0f, 184.0f, 60.0f };
}

juce::Rectangle<float> navButtonBounds (Part p)
{
    switch (p)
    {
        case Part::view:         return { 690.0f, 158.0f, 85.0f, 64.0f };
        case Part::action:       return { 800.0f, 158.0f, 85.0f, 64.0f };
        case Part::pagePrevious: return { 690.0f, 282.0f, 85.0f, 64.0f };
        case Part::pageNext:     return { 800.0f, 282.0f, 85.0f, 64.0f };
        default:                 return {};
    }
}

juce::Point<float> knobCentre (Part p, float& radius)
{
    radius = kKnobR;
    if (isParamKnob (p))
        return { kKnobX[(int) p - (int) Part::knob1], kKnobY };
    radius = p == Part::volumeKnob ? kVolumeR : kNavKnobR;
    return p == Part::upperKnob ? kUpperKnob : p == Part::lowerKnob ? kLowerKnob : kVolume;
}

juce::Path polygon (std::initializer_list<juce::Point<float>> points, float corner)
{
    juce::Path p;
    bool first = true;
    for (auto pt : points)
    {
        if (first)
            p.startNewSubPath (pt);
        else
            p.lineTo (pt);
        first = false;
    }
    p.closeSubPath();
    return p.createPathWithRoundedCorners (corner);
}

juce::Path pedalOutline()
{
    return polygon ({ { 1165, 272 }, { 1484, 272 }, { 1484, 937 }, { 1240, 937 }, { 1240, 560 }, { 1165, 488 } }, 16.0f);
}

juce::Path pedalRim()
{
    return polygon ({ { 1171, 278 }, { 1478, 278 }, { 1478, 931 }, { 1246, 931 }, { 1246, 557 }, { 1171, 485 } }, 13.0f);
}

juce::Path pedalTread()
{
    return polygon ({ { 1180, 287 }, { 1469, 287 }, { 1469, 922 }, { 1255, 922 }, { 1255, 553 }, { 1180, 481 } }, 10.0f);
}

/* Heights of the rubber tread's ribs, toe to heel. */
const float kTreadBands[][2] = {
    { 287, 393 }, { 401, 493 }, { 501, 588 }, { 596, 676 }, { 684, 756 }, { 764, 822 }, { 830, 876 }, { 884, 922 },
};

void strokeLine (juce::Graphics& g, juce::Point<float> a, juce::Point<float> b, float width)
{
    juce::Path p;
    p.startNewSubPath (a);
    p.lineTo (b);
    g.strokePath (p, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void printText (juce::Graphics& g, const juce::String& text, juce::Point<float> centre, float size, bool bold = false)
{
    g.setColour (kPrint);
    g.setFont (theme::heading (size, bold));
    g.drawText (text, juce::Rectangle<float> (240.0f, size * 1.4f).withCentre (centre), juce::Justification::centred,
                false);
}

enum class Pointing { up, down, left, right };

/* A filled triangle `w` across its base and `h` from base to tip. */
void triangle (juce::Graphics& g, juce::Point<float> c, float w, float h, Pointing towards)
{
    juce::Path p;
    switch (towards)
    {
        case Pointing::up:
            p.addTriangle (c.x - w / 2.0f, c.y + h / 2.0f, c.x + w / 2.0f, c.y + h / 2.0f, c.x, c.y - h / 2.0f);
            break;
        case Pointing::down:
            p.addTriangle (c.x - w / 2.0f, c.y - h / 2.0f, c.x + w / 2.0f, c.y - h / 2.0f, c.x, c.y + h / 2.0f);
            break;
        case Pointing::left:
            p.addTriangle (c.x + h / 2.0f, c.y - w / 2.0f, c.x + h / 2.0f, c.y + w / 2.0f, c.x - h / 2.0f, c.y);
            break;
        case Pointing::right:
            p.addTriangle (c.x - h / 2.0f, c.y - w / 2.0f, c.x - h / 2.0f, c.y + w / 2.0f, c.x + h / 2.0f, c.y);
            break;
    }
    g.fillPath (p);
}

void drawKnobBody (juce::Graphics& g, juce::Point<float> c, float r)
{
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillEllipse (circle (c.translated (0.0f, r * 0.14f), r * 1.06f));

    g.setColour (juce::Colour (0xff0b0b0c));
    g.fillEllipse (circle (c, r));
    g.setColour (juce::Colour (0xff2a2b2e));
    const int ridges = 40;
    for (int i = 0; i < ridges; ++i)
    {
        const float a = juce::MathConstants<float>::twoPi * (float) i / (float) ridges;
        g.drawLine (juce::Line<float> (c.getPointOnCircumference (r * 0.82f, a), c.getPointOnCircumference (r * 0.97f, a)),
                    juce::jmax (1.0f, r * 0.045f));
    }

    const auto cap = circle (c, r * 0.76f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3c3d41), cap.getX(), cap.getY(), juce::Colour (0xff09090a),
                                             cap.getRight(), cap.getBottom(), false));
    g.fillEllipse (cap);
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.drawEllipse (cap.reduced (0.5f), 1.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.22f), c.x - r * 0.28f, c.y - r * 0.42f,
                                             juce::Colours::transparentWhite, c.x - r * 0.28f + r * 0.55f, c.y - r * 0.42f,
                                             true));
    g.fillEllipse (cap);
}

void drawPointer (juce::Graphics& g, juce::Point<float> c, float r, float angle, float width)
{
    g.setColour (juce::Colour (0xffeceef1));
    strokeLine (g, c.getPointOnCircumference (r * 0.2f, angle), c.getPointOnCircumference (r * 0.68f, angle), width);
}

float encoderAngle (int detents)
{
    return (float) detents * juce::MathConstants<float>::twoPi / 24.0f;
}

float volumeAngle (int value)
{
    return juce::degreesToRadians (-135.0f + 270.0f * (float) juce::jlimit (0, 127, value) / 127.0f);
}

void drawNavButton (juce::Graphics& g, Part p, bool down)
{
    const auto base = navButtonBounds (p);
    const auto r = down ? base.translated (0.0f, 2.0f) : base;
    g.setColour (juce::Colours::black.withAlpha (0.65f));
    g.fillRoundedRectangle (base.translated (0.0f, 4.0f), 9.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (down ? 0xff151618 : 0xff2b2d31), 0.0f, r.getY(),
                                             juce::Colour (down ? 0xff0f1011 : 0xff17181b), 0.0f, r.getBottom(), false));
    g.fillRoundedRectangle (r, 9.0f);
    g.setColour (juce::Colour (down ? 0xff34363a : 0xff4a4c52));
    g.drawRoundedRectangle (r.reduced (0.75f), 9.0f, 1.5f);

    /* The label sits in the part the knob does not cover. */
    const bool left = p == Part::view || p == Part::pagePrevious;
    const float cx = left ? r.getX() + 35.0f : r.getRight() - 35.0f;
    const float cy = r.getCentreY();
    g.setColour (kPrint);
    g.setFont (theme::heading (16.0f, true));
    switch (p)
    {
        case Part::view:
        {
            juce::Path house;
            house.startNewSubPath (cx - 8.0f, cy - 7.0f);
            house.lineTo (cx, cy - 15.0f);
            house.lineTo (cx + 8.0f, cy - 7.0f);
            house.startNewSubPath (cx - 5.5f, cy - 9.0f);
            house.lineTo (cx - 5.5f, cy - 1.0f);
            house.lineTo (cx + 5.5f, cy - 1.0f);
            house.lineTo (cx + 5.5f, cy - 9.0f);
            g.strokePath (house, juce::PathStrokeType (1.8f));
            g.drawText ("VIEW", juce::Rectangle<float> (74.0f, 22.0f).withCentre ({ cx, cy + 11.0f }),
                        juce::Justification::centred, false);
            break;
        }
        case Part::action:
            g.drawText ("ACTION", juce::Rectangle<float> (74.0f, 22.0f).withCentre ({ cx, cy }),
                        juce::Justification::centred, false);
            break;
        case Part::pagePrevious:
            triangle (g, { cx - 26.0f, cy }, 10.0f, 8.0f, Pointing::left);
            g.drawText ("PAGE", juce::Rectangle<float> (54.0f, 22.0f).withCentre ({ cx + 6.0f, cy }),
                        juce::Justification::centred, false);
            break;
        case Part::pageNext:
            g.drawText ("PAGE", juce::Rectangle<float> (54.0f, 22.0f).withCentre ({ cx - 6.0f, cy }),
                        juce::Justification::centred, false);
            triangle (g, { cx + 26.0f, cy }, 10.0f, 8.0f, Pointing::right);
            break;
        default:
            break;
    }
}

void drawFootswitch (juce::Graphics& g, juce::Point<float> c, juce::Colour led, bool down)
{
    const bool lit = ! led.isTransparent();
    if (lit)
    {
        g.setGradientFill (juce::ColourGradient (led.withAlpha (0.42f), c.x, c.y, led.withAlpha (0.0f),
                                                 c.x + kSwitchR * 2.0f, c.y, true));
        g.fillEllipse (circle (c, kSwitchR * 2.0f));
    }

    const auto ring = circle (c, kSwitchR);
    if (lit)
        g.setGradientFill (juce::ColourGradient (led.brighter (0.4f), c.x, c.y - kSwitchR, led.darker (0.2f), c.x,
                                                 c.y + kSwitchR, false));
    else
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3d4045), c.x, c.y - kSwitchR,
                                                 juce::Colour (0xff222427), c.x, c.y + kSwitchR, false));
    g.fillEllipse (ring);
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.drawEllipse (ring.reduced (0.75f), 1.5f);
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillEllipse (circle (c, 23.5f));

    const float capR = down ? 19.0f : 20.5f;
    const auto cc = c.translated (0.0f, down ? 1.5f : 0.0f);
    const auto cap = circle (cc, capR);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff4f5f7), cc.x - capR * 0.35f, cc.y - capR * 0.4f,
                                             juce::Colour (down ? 0xff6f7277 : 0xff8e9197), cc.x + capR, cc.y + capR,
                                             true));
    g.fillEllipse (cap);
    g.setColour (juce::Colour (0xff5f6268));
    g.drawEllipse (cap.reduced (0.5f), 1.0f);
    g.setColour (juce::Colours::white.withAlpha (down ? 0.25f : 0.6f));
    g.fillEllipse (circle (cc.translated (-capR * 0.32f, -capR * 0.36f), capR * 0.26f));
}
} // namespace

/* ---- the parts, named ---------------------------------------------------------- */

HardwareView::Ids HardwareView::idsOf (Part p)
{
    Ids ids;
    switch (p)
    {
        case Part::knob1: case Part::knob2: case Part::knob3: case Part::knob4: case Part::knob5:
        {
            const int i = (int) p - (int) Part::knob1;
            ids.encoder = MIDI_CONTROL_ENC_1 + i;
            ids.button = MIDI_CONTROL_BTN_ENC_1 + i;
            break;
        }
        case Part::upperKnob:    ids.encoder = MIDI_CONTROL_ENC_HOME; ids.button = MIDI_CONTROL_BTN_HOME_ENC; break;
        case Part::lowerKnob:    ids.encoder = MIDI_CONTROL_ENC_MENU; ids.button = MIDI_CONTROL_BTN_MENU_ENC; break;
        case Part::volumeKnob:   ids.analog = MIDI_CONTROL_ANALOG_VOLUME; break;
        case Part::view:         ids.button = MIDI_CONTROL_BTN_VIEW; break;
        /* ACTION and PAGE <: which of the firmware's Bypass and Menu is which is
         * derived from the matrix order, not measured (docs/08 section 2). */
        case Part::action:       ids.button = MIDI_CONTROL_BTN_BYPASS; break;
        case Part::pagePrevious: ids.button = MIDI_CONTROL_BTN_MENU; break;
        case Part::pageNext:     ids.button = MIDI_CONTROL_BTN_PAGE; break;
        case Part::bankUp:       ids.button = MIDI_CONTROL_BTN_BANK_UP; break;
        case Part::bankDown:     ids.button = MIDI_CONTROL_BTN_BANK_DOWN; break;
        case Part::fsA:          ids.button = MIDI_CONTROL_BTN_FS_A; break;
        case Part::fsB:          ids.button = MIDI_CONTROL_BTN_FS_B; break;
        case Part::fsC:          ids.button = MIDI_CONTROL_BTN_FS_C; break;
        case Part::fsD:          ids.button = MIDI_CONTROL_BTN_FS_D; break;
        case Part::mode:         ids.button = MIDI_CONTROL_BTN_MODE; break;
        case Part::tap:          ids.button = MIDI_CONTROL_BTN_TAP; break;
        case Part::pedal:        ids.analog = MIDI_CONTROL_ANALOG_PEDAL; ids.button = MIDI_CONTROL_BTN_TOE; break;
        case Part::toe:          ids.button = MIDI_CONTROL_BTN_TOE; break;
        default: break;
    }
    if (ids.button >= 0)
        ids.ledColumn = midi_control_button_led_column ((midi_control_button) ids.button);
    return ids;
}

juce::String HardwareView::nameOf (Part p)
{
    if (isParamKnob (p))
        return "Knob " + juce::String ((int) p - (int) Part::knob1 + 1);
    switch (p)
    {
        case Part::screen:       return "Screen";
        case Part::upperKnob:    return "Upper knob";
        case Part::lowerKnob:    return "Lower knob";
        case Part::volumeKnob:   return "Volume knob";
        case Part::view:         return "VIEW button";
        case Part::action:       return "ACTION button";
        case Part::pagePrevious: return "PAGE < button";
        case Part::pageNext:     return "PAGE > button";
        case Part::bankUp:       return "Bank up footswitch";
        case Part::bankDown:     return "Bank down footswitch";
        case Part::fsA:          return "Footswitch A";
        case Part::fsB:          return "Footswitch B";
        case Part::fsC:          return "Footswitch C";
        case Part::fsD:          return "Footswitch D";
        case Part::mode:         return "MODE footswitch";
        case Part::tap:          return "TAP footswitch";
        case Part::pedal:        return "Expression pedal";
        case Part::toe:          return "Toe switch";
        default:                 return {};
    }
}

bool HardwareView::State::sameAs (const State& o) const
{
    return screen == o.screen
        && std::memcmp (leds, o.leds, sizeof (leds)) == 0
        && std::equal (std::begin (held), std::end (held), std::begin (o.held))
        && std::equal (std::begin (active), std::end (active), std::begin (o.active))
        && std::equal (std::begin (analog), std::end (analog), std::begin (o.analog))
        && pedalPhysical == o.pedalPhysical
        && pedalSent == o.pedalSent
        && pedalPadLo == o.pedalPadLo
        && pedalPadHi == o.pedalPadHi
        && std::equal (std::begin (turns), std::end (turns), std::begin (o.turns))
        && captions == o.captions;
}

/* ---- component ------------------------------------------------------------------ */

HardwareView::HardwareView() = default;

HardwareView::~HardwareView() = default;

void HardwareView::setPlayable (bool shouldPlay)
{
    if (shouldPlay == playable)
        return;
    if (! shouldPlay && pressedButton >= 0 && onButton)
        onButton (pressedButton, false);
    pressedButton = -1;
    dragPart = Part::none;
    playable = shouldPlay;
    repaint();
}

void HardwareView::setState (const State& s, bool screenChanged)
{
    auto next = s;
    /* The volume knob or pedal moved from here keeps the position given it,
     * while it is dragged and for a moment after: the owner's state is a round
     * trip behind and would pull it back. */
    const auto now = juce::Time::getMillisecondCounter();
    const int dragged = idsOf (dragPart).analog;
    for (int a = 0; a < MIDI_CONTROL_ANALOG_COUNT; ++a)
    {
        /* A physical pedal is what the drag moves; the device's position is
         * the answer to it, and is shown as it comes. */
        const bool physical = a == MIDI_CONTROL_ANALOG_PEDAL && next.pedalPhysical >= 0;
        if (playable && ! physical && (a == dragged || now < analogLocalUntil[a]))
            next.analog[a] = state.analog[a];
    }

    const bool same = state.sameAs (next);
    if (same && ! screenChanged)
        return;
    state = next;
    if (same)
        repaint (kScreenBezel.transformedBy (transform).getSmallestIntegerContainer().expanded (2));
    else
        repaint();
}

void HardwareView::setSelected (Part p)
{
    if (p == selected)
        return;
    selected = p;
    repaint();
}

void HardwareView::resized()
{
    transform = juce::RectanglePlacement (juce::RectanglePlacement::centred)
                    .getTransformToFit (kCanvas, getLocalBounds().toFloat().reduced (4.0f));
    chassis = {};
}

void HardwareView::paint (juce::Graphics& g)
{
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    const int w = juce::roundToInt ((float) getWidth() * scale);
    const int h = juce::roundToInt ((float) getHeight() * scale);
    if (w <= 0 || h <= 0)
        return;

    if (chassis.getWidth() != w || chassis.getHeight() != h)
    {
        chassis = juce::Image (juce::Image::ARGB, w, h, true);
        juce::Graphics cg (chassis);
        cg.addTransform (transform.scaled (scale));
        drawChassis (cg);
    }
    g.drawImageTransformed (chassis, juce::AffineTransform::scale (1.0f / scale));

    juce::Graphics::ScopedSaveState save (g);
    g.addTransform (transform);
    drawLive (g);
    drawCaptions (g);
    if (hovered != Part::none && hovered != selected)
        drawHighlight (g, hovered, juce::Colours::white.withAlpha (0.4f), false);
    if (selected != Part::none)
        drawHighlight (g, selected, theme::accent, true);
}

/* The parts that never change: the chassis, the printing, the knob bodies, the
 * pedal. Drawn once per size into `chassis`. */
void HardwareView::drawChassis (juce::Graphics& g) const
{
    juce::Path body;
    body.addRoundedRectangle (kDeck, 12.0f);
    body.addRoundedRectangle (kTopPanel, 10.0f);
    juce::DropShadow (juce::Colours::black.withAlpha (0.7f), 36, { 0, 16 }).drawForPath (g, body);

    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1c1d20), 0.0f, kDeck.getY(), juce::Colour (0xff121315),
                                             0.0f, kDeck.getBottom(), false));
    g.fillRoundedRectangle (kDeck, 12.0f);
    g.setColour (juce::Colour (0xff2c2e33));
    g.drawRoundedRectangle (kDeck.reduced (0.75f), 12.0f, 1.5f);

    /* The top panel stands above the deck; its front edge throws a shadow. */
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.8f), 0.0f, kTopPanel.getBottom(),
                                             juce::Colours::transparentBlack, 0.0f, kTopPanel.getBottom() + 26.0f, false));
    g.fillRect (kTopPanel.getX(), kTopPanel.getBottom() - 4.0f, kTopPanel.getWidth(), 30.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff24262a), 0.0f, kTopPanel.getY(), juce::Colour (0xff18191c),
                                             0.0f, kTopPanel.getBottom(), false));
    g.fillRoundedRectangle (kTopPanel, 10.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2c2e32), 0.0f, kTopPanel.getBottom() - 18.0f,
                                             juce::Colour (0xff121315), 0.0f, kTopPanel.getBottom(), false));
    g.fillRoundedRectangle (kTopPanel.withTop (kTopPanel.getBottom() - 18.0f), 8.0f);
    g.setColour (juce::Colour (0xff393b40));
    g.drawRoundedRectangle (kTopPanel.reduced (0.75f), 10.0f, 1.5f);
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.fillRect (kTopPanel.getX() + 12.0f, kTopPanel.getY() + 2.0f, kTopPanel.getWidth() - 24.0f, 1.5f);

    /* Screen bezel. */
    g.setColour (juce::Colour (0xff050506));
    g.fillRoundedRectangle (kScreenBezel, 7.0f);
    g.setColour (juce::Colour (0xff3d3f44));
    g.drawRoundedRectangle (kScreenBezel.reduced (1.0f), 7.0f, 2.0f);

    /* Parameter knobs and the volume knob; their pointers are drawn live. */
    for (float x : kKnobX)
        drawKnobBody (g, { x, kKnobY }, kKnobR);
    drawKnobBody (g, kVolume, kVolumeR);

    /* The navigation cluster's printing. */
    g.setColour (kPrint);
    auto bracket = [&] (float y, float gap)
    {
        juce::Path p;
        p.startNewSubPath (727.0f, y);
        p.lineTo (727.0f, y + 8.0f);
        p.lineTo (787.0f - gap / 2.0f, y + 8.0f);
        p.startNewSubPath (787.0f + gap / 2.0f, y + 8.0f);
        p.lineTo (848.0f, y + 8.0f);
        p.lineTo (848.0f, y);
        g.strokePath (p, juce::PathStrokeType (1.6f));
    };
    bracket (228.0f, 48.0f);
    printText (g, "SAVE", { 787.0f, 237.0f }, 15.0f, true);
    g.fillRect (648.0f, 252.0f, 234.0f, 1.6f);
    bracket (352.0f, 30.0f);
    for (int i = 0; i < 3; ++i)
        g.fillRect (780.0f, 356.0f + (float) i * 4.0f, 14.0f, 1.6f);

    /* Expression lamps beside the volume knob. */
    g.setColour (juce::Colour (0xff303237));
    g.fillRoundedRectangle (kLampWah, 4.0f);
    g.fillRoundedRectangle (kLampVol, 4.0f);
    g.setColour (kPrint);
    g.setFont (theme::heading (16.0f, true));
    g.drawText ("WAH", juce::Rectangle<float> (60.0f, 18.0f).withCentre ({ 936.0f, 299.0f }),
                juce::Justification::centredRight, false);
    g.drawText ("VOL", juce::Rectangle<float> (60.0f, 18.0f).withCentre ({ 936.0f, 326.0f }),
                juce::Justification::centredRight, false);
    g.drawText ("EXP 1", juce::Rectangle<float> (60.0f, 18.0f).withCentre ({ 1040.0f, 299.0f }),
                juce::Justification::centredLeft, false);
    g.drawText ("EXP 2", juce::Rectangle<float> (60.0f, 18.0f).withCentre ({ 1040.0f, 326.0f }),
                juce::Justification::centredLeft, false);

    /* The product name where a maker's mark would be. */
    g.setColour (juce::Colour (0xffcfd3d9));
    g.setFont (theme::heading (46.0f, true));
    g.drawText (MIDI_CONTROL_PRODUCT_NAME, juce::Rectangle<float> (220.0f, 52.0f).withCentre ({ 860.0f, 422.0f }),
                juce::Justification::centred, false);
    g.setColour (theme::accent);
    g.fillRoundedRectangle (juce::Rectangle<float> (56.0f, 4.0f).withCentre ({ 860.0f, 452.0f }), 2.0f);

    /* Footswitch sockets and printing. */
    for (int i = (int) Part::bankUp; i <= (int) Part::tap; ++i)
    {
        const auto c = switchCentre ((Part) i);
        g.setColour (juce::Colour (0xff09090a));
        g.fillEllipse (circle (c, kSwitchR + 5.0f));
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawEllipse (circle (c, kSwitchR + 5.0f).translated (0.0f, 1.5f), 1.0f);
    }
    g.setColour (kPrint);
    triangle (g, { 145.0f, 638.0f }, 30.0f, 16.0f, Pointing::up);
    triangle (g, { 145.0f, 762.0f }, 30.0f, 16.0f, Pointing::down);
    printText (g, "C", { 425.0f, 640.0f }, 30.0f);
    printText (g, "D", { 705.0f, 640.0f }, 30.0f);
    printText (g, "MODE", { 987.0f, 638.0f }, 30.0f);
    printText (g, "EDIT / EXIT", { 987.0f, 664.0f }, 16.0f);
    printText (g, "A", { 425.0f, 766.0f }, 30.0f);
    printText (g, "B", { 705.0f, 766.0f }, 30.0f);
    printText (g, "TAP", { 987.0f, 750.0f }, 30.0f);
    printText (g, "TUNER", { 987.0f, 776.0f }, 16.0f);

    /* The pedal: a bright rim round a ribbed rubber tread. */
    const auto outline = pedalOutline();
    juce::DropShadow (juce::Colours::black.withAlpha (0.8f), 26, { 0, 12 }).drawForPath (g, outline);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffc9ccd2), 1165.0f, 272.0f, juce::Colour (0xff595c62),
                                             1484.0f, 937.0f, false));
    g.fillPath (outline);
    g.setColour (juce::Colour (0xff141416));
    g.fillPath (pedalRim());

    const auto tread = pedalTread();
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (tread);
    g.setColour (juce::Colour (0xff070708));
    g.fillPath (tread);
    for (const auto& band : kTreadBands)
    {
        const float left = band[0] >= 553.0f ? 1261.0f : 1186.0f;
        const juce::Rectangle<float> r { left, band[0] + 3.0f, 1463.0f - left, band[1] - band[0] - 6.0f };
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff26272a), 0.0f, r.getY(), juce::Colour (0xff141416),
                                                 0.0f, r.getBottom(), false));
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    }
}

/* Everything that moves: the screen, pointers, buttons, LEDs, the pedal. */
void HardwareView::drawLive (juce::Graphics& g) const
{
    const auto glass = kScreenBezel.reduced (10.0f);
    g.setColour (juce::Colour (0xff020203));
    g.fillRect (glass);
    if (state.screen.isValid())
    {
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (state.screen, glass, juce::RectanglePlacement::centred);
    }
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.06f), glass.getX(), glass.getY(),
                                             juce::Colours::transparentWhite, glass.getCentreX(), glass.getBottom(), false));
    g.fillRect (glass);

    for (int i = 0; i < 5; ++i)
        drawPointer (g, { kKnobX[i], kKnobY }, kKnobR, encoderAngle (state.turns[MIDI_CONTROL_ENC_1 + i]), 4.5f);

    const int volume = state.analog[MIDI_CONTROL_ANALOG_VOLUME];
    drawPointer (g, kVolume, kVolumeR, volumeAngle (volume), 4.5f);
    if (playable)
    {
        juce::Path arc;
        arc.addCentredArc (kVolume.x, kVolume.y, kVolumeR + 9.0f, kVolumeR + 9.0f, 0.0f, volumeAngle (0),
                           volumeAngle (volume), true);
        g.setColour (theme::accent.withAlpha (0.85f));
        g.strokePath (arc, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    for (auto p : { Part::view, Part::action, Part::pagePrevious, Part::pageNext })
        drawNavButton (g, p, isDown (idsOf (p).button));
    drawKnobBody (g, kUpperKnob, kNavKnobR);
    drawPointer (g, kUpperKnob, kNavKnobR, encoderAngle (state.turns[MIDI_CONTROL_ENC_HOME]), 4.0f);
    drawKnobBody (g, kLowerKnob, kNavKnobR);
    drawPointer (g, kLowerKnob, kNavKnobR, encoderAngle (state.turns[MIDI_CONTROL_ENC_MENU]), 4.0f);

    auto lamp = [&] (juce::Rectangle<float> r, bool lit)
    {
        if (! lit)
            return;
        g.setColour (kLamp.withAlpha (0.3f));
        g.fillRoundedRectangle (r.expanded (6.0f, 5.0f), 9.0f);
        g.setColour (kLamp);
        g.fillRoundedRectangle (r, 4.0f);
    };
    lamp (kLampWah, (state.leds[6] & MIDI_CONTROL_LAMP_WAH) != 0);
    lamp (kLampVol, (state.leds[6] & MIDI_CONTROL_LAMP_VOL) != 0);

    for (int i = (int) Part::bankUp; i <= (int) Part::tap; ++i)
    {
        const auto p = (Part) i;
        const auto ids = idsOf (p);
        juce::Colour led;
        if (p == Part::tap)
            led = (state.leds[6] & MIDI_CONTROL_LAMP_TAP) != 0 ? kTapLed : juce::Colour();
        else if (ids.ledColumn >= 0)
            led = theme::led (state.leds[ids.ledColumn]);
        drawFootswitch (g, switchCentre (p), led, isDown (ids.button));
    }

    /* The pedal rocks: the end that is down is further from the light. */
    const float pos = (float) (state.pedalPhysical >= 0 ? state.pedalPhysical
                                                         : state.analog[MIDI_CONTROL_ANALOG_PEDAL]) / 127.0f;
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (pedalTread());
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.45f * pos), 0.0f, 287.0f,
                                                 juce::Colours::black.withAlpha (0.45f * (1.0f - pos)), 0.0f, 922.0f,
                                                 false));
        g.fillRect (1170.0f, 280.0f, 310.0f, 650.0f);
        if (isDown (MIDI_CONTROL_BTN_TOE))
        {
            g.setColour (theme::accent.withAlpha (0.28f));
            g.fillRect (kToe);
        }
    }
    if (playable)
    {
        const juce::Rectangle<float> track { 1508.0f, 300.0f, 6.0f, 620.0f };

        /* The padding (docs/05 section 5): a sleeve round each end of the
         * track, toe at the top as the fill runs, with a line where it starts.
         * Lit while the pedal is in it, which is while that end's value is
         * what it sends. On the device's position, which is what is padded. */
        const int at = state.analog[MIDI_CONTROL_ANALOG_PEDAL];
        const auto sleeve = [&] (float top, float bottom, float edge, bool lit)
        {
            const auto r = juce::Rectangle<float>::leftTopRightBottom (track.getX() - 6.0f, top,
                                                                       track.getRight() + 6.0f, bottom);
            const auto colour = lit ? theme::accent : theme::inkDim;
            g.setColour (colour.withAlpha (lit ? 0.32f : 0.24f));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (colour.withAlpha (lit ? 1.0f : 0.85f));
            g.fillRect (juce::Rectangle<float> (r.getX(), edge - 1.25f, r.getWidth(), 2.5f));
        };
        if (state.pedalPadHi > 0)
        {
            const float edge = track.getY() + track.getHeight() * (float) state.pedalPadHi / 100.0f;
            sleeve (track.getY() - 5.0f, edge, edge, at * 100 >= (100 - state.pedalPadHi) * 127);
        }
        if (state.pedalPadLo > 0)
        {
            const float edge = track.getBottom() - track.getHeight() * (float) state.pedalPadLo / 100.0f;
            sleeve (edge, track.getBottom() + 5.0f, edge, at * 100 <= state.pedalPadLo * 127);
        }

        g.setColour (juce::Colour (0xff1b1d21));
        g.fillRoundedRectangle (track, 3.5f);
        g.setColour (theme::accent);
        g.fillRoundedRectangle (track.withTop (track.getBottom() - track.getHeight() * pos), 3.5f);
    }
}

void HardwareView::drawCaptions (juce::Graphics& g) const
{
    for (int i = (int) Part::bankUp; i <= (int) Part::tap; ++i)
    {
        const auto p = (Part) i;
        const auto& cap = state.captions[(size_t) p];
        if (cap.what.isEmpty())
            continue;
        const auto ids = idsOf (p);
        juce::Colour led = theme::inkFaint;
        if (p == Part::tap)
            led = kTapLed;
        else if (ids.ledColumn >= 0 && state.leds[ids.ledColumn] != 0)
            led = theme::led (state.leds[ids.ledColumn]);
        const bool active = state.active[ids.button];

        auto r = captionBounds (p);
        g.setColour (juce::Colour (0xf00b0c0e));
        g.fillRoundedRectangle (r, 10.0f);
        g.setColour (active ? theme::accent : juce::Colour (0xff2f3237));
        g.drawRoundedRectangle (r.reduced (0.75f), 10.0f, 1.5f);
        g.setColour (cap.assigned ? led : theme::inkFaint.withAlpha (0.5f));
        g.fillRoundedRectangle (juce::Rectangle<float> (r.getX() + 10.0f, r.getY() + 11.0f, 4.0f, r.getHeight() - 22.0f), 2.0f);

        auto text = r.withTrimmedLeft (24.0f).withTrimmedRight (10.0f).reduced (0.0f, 5.0f);
        g.setFont (theme::heading (25.0f, true));
        g.setColour (cap.assigned ? theme::ink : theme::inkFaint);
        g.drawText (cap.what, text.removeFromTop (29.0f), juce::Justification::centredLeft, true);
        g.setFont (theme::ui (17.0f));
        g.setColour (theme::inkDim);
        g.drawText (cap.detail, text, juce::Justification::centredLeft, true);
    }

    auto under = [&] (Part p, juce::Rectangle<float> area)
    {
        const auto& cap = state.captions[(size_t) p];
        if (cap.what.isEmpty())
            return;
        g.setFont (theme::heading (20.0f, true));
        g.setColour (cap.assigned ? juce::Colour (0xffb4bac3) : theme::inkFaint);
        g.drawText (cap.what, area, juce::Justification::centred, false);
    };
    for (int i = 0; i < 5; ++i)
        under ((Part) ((int) Part::knob1 + i),
               juce::Rectangle<float> (120.0f, 24.0f).withCentre ({ kKnobX[i], kKnobY + kKnobR + 17.0f }));
    under (Part::volumeKnob, juce::Rectangle<float> (130.0f, 24.0f).withCentre ({ kVolume.x, kVolume.y + kVolumeR + 24.0f }));

    auto pill = [&] (Part p, juce::Point<float> centre, const juce::String& prefix, const juce::String& suffix)
    {
        const auto& cap = state.captions[(size_t) p];
        if (cap.what.isEmpty())
            return;
        const auto text = prefix + cap.what + suffix;
        const auto font = theme::heading (22.0f, true);
        const float w = juce::jmin (206.0f, theme::textWidth (font, text) + 36.0f);
        const auto r = juce::Rectangle<float> (w, 40.0f).withCentre (centre);
        g.setColour (juce::Colour (0xe00b0c0e));
        g.fillRoundedRectangle (r, 20.0f);
        g.setColour (juce::Colour (0xff3a3d43));
        g.drawRoundedRectangle (r.reduced (0.75f), 20.0f, 1.5f);
        g.setFont (font);
        g.setColour (cap.assigned ? theme::ink : theme::inkFaint);
        g.drawText (text, r.reduced (14.0f, 0.0f), juce::Justification::centred, true);
    };
    pill (Part::toe, { 1324.0f, 340.0f }, "TOE  ", {});
    pill (Part::pedal, { 1362.0f, 720.0f }, {},
          playable && state.pedalSent >= 0 ? " = " + juce::String (state.pedalSent) : juce::String());
}

void HardwareView::drawHighlight (juce::Graphics& g, Part p, juce::Colour colour, bool strong) const
{
    const float width = strong ? 3.0f : 2.0f;
    if (p == Part::pedal || p == Part::toe)
    {
        juce::Path path;
        if (p == Part::pedal)
            path = pedalOutline();
        else
            path.addRoundedRectangle (kToe, 10.0f);
        if (strong)
        {
            g.setColour (colour.withAlpha (0.22f));
            g.strokePath (path, juce::PathStrokeType (14.0f));
        }
        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (width));
        return;
    }

    juce::Rectangle<float> r;
    bool round = true;
    if (p == Part::screen)
    {
        r = kScreenBezel;
        round = false;
    }
    else if (isNavButton (p))
    {
        r = navButtonBounds (p);
        round = false;
    }
    else if (isSwitch (p))
        r = circle (switchCentre (p), kSwitchR);
    else
    {
        float radius = 0.0f;
        const auto c = knobCentre (p, radius);
        r = circle (c, radius);
    }

    r = r.expanded (7.0f);
    if (strong)
    {
        g.setColour (colour.withAlpha (0.2f));
        if (round)
            g.drawEllipse (r.expanded (4.0f), 8.0f);
        else
            g.drawRoundedRectangle (r.expanded (4.0f), 14.0f, 8.0f);
    }
    g.setColour (colour);
    if (round)
        g.drawEllipse (r, width);
    else
        g.drawRoundedRectangle (r, 11.0f, width);
    if (isSwitch (p))
        g.drawRoundedRectangle (captionBounds (p).expanded (4.0f), 13.0f, width);
}

/* ---- gestures -------------------------------------------------------------------- */

HardwareView::Part HardwareView::partAt (juce::Point<float> local, bool* onCaption) const
{
    const auto pt = local.transformedBy (transform.inverted());
    auto within = [&] (juce::Point<float> c, float r) { return pt.getDistanceFrom (c) <= r; };

    for (int i = 0; i < 5; ++i)
        if (within ({ kKnobX[i], kKnobY }, kKnobR + 4.0f))
            return (Part) ((int) Part::knob1 + i);
    if (within (kUpperKnob, kNavKnobR + 2.0f))
        return Part::upperKnob;
    if (within (kLowerKnob, kNavKnobR + 2.0f))
        return Part::lowerKnob;
    if (within (kVolume, kVolumeR + 5.0f))
        return Part::volumeKnob;
    for (auto p : { Part::view, Part::action, Part::pagePrevious, Part::pageNext })
        if (navButtonBounds (p).contains (pt))
            return p;
    for (int i = (int) Part::bankUp; i <= (int) Part::tap; ++i)
    {
        if (within (switchCentre ((Part) i), kSwitchR + 8.0f))
            return (Part) i;
        if (captionBounds ((Part) i).contains (pt))
        {
            if (onCaption != nullptr)
                *onCaption = true;
            return (Part) i;
        }
    }
    if (pedalOutline().contains (pt))
        return playable && kToe.contains (pt) ? Part::toe : Part::pedal;
    if (kScreenBezel.contains (pt))
        return Part::screen;
    return Part::none;
}

void HardwareView::setHovered (Part p)
{
    if (p == hovered)
        return;
    hovered = p;
    const bool drags = playable && (isEncoderKnob (p) || p == Part::volumeKnob || p == Part::pedal);
    setMouseCursor (p == Part::none ? juce::MouseCursor::NormalCursor
                    : drags         ? juce::MouseCursor::UpDownResizeCursor
                                    : juce::MouseCursor::PointingHandCursor);
    repaint();
}

void HardwareView::mouseMove (const juce::MouseEvent& e)
{
    setHovered (partAt (e.position));
}

void HardwareView::mouseExit (const juce::MouseEvent&)
{
    setHovered (Part::none);
}

void HardwareView::mouseDown (const juce::MouseEvent& e)
{
    bool onCaption = false;
    const auto p = partAt (e.position, &onCaption);
    if (p == Part::none)
        return;

    /* What is touched is what the editor shows; the toe switch is part of the pedal. */
    const auto target = p == Part::toe ? Part::pedal : p;
    if (target != selected)
    {
        setSelected (target);
        if (onSelect)
            onSelect (target);
    }
    if (! playable || onCaption)
        return;

    dragPart = p;
    lastDrag = e.position;
    dragAccum = 0.0f;
    dragTravel = 0.0f;
    const auto ids = idsOf (p);
    if (isNavButton (p) || isSwitch (p) || p == Part::toe)
    {
        pressedButton = ids.button;
        if (onButton)
            onButton (pressedButton, true);
        repaint();
    }
    else if (p == Part::pedal && state.pedalPhysical >= 0)
        dragValue = (float) state.pedalPhysical;
    else if (ids.analog >= 0)
        dragValue = (float) state.analog[ids.analog];
}

void HardwareView::mouseDrag (const juce::MouseEvent& e)
{
    if (! playable || pressedButton >= 0)
        return;
    const auto delta = e.position - lastDrag;
    lastDrag = e.position;
    dragTravel += std::abs (delta.x) + std::abs (delta.y);
    const auto ids = idsOf (dragPart);

    if (isEncoderKnob (dragPart))
    {
        dragAccum += delta.x - delta.y;
        const float step = 7.0f;
        int detents = 0;
        for (; dragAccum >= step; dragAccum -= step)
            ++detents;
        for (; dragAccum <= -step; dragAccum += step)
            --detents;
        if (detents != 0 && onEncoder)
            onEncoder (ids.encoder, detents);
    }
    else if (ids.analog >= 0)
    {
        const float pixelsPerCanvas = std::sqrt (std::abs (transform.getDeterminant()));
        const float travel = (dragPart == Part::pedal ? 480.0f : 260.0f) * pixelsPerCanvas;
        dragValue = juce::jlimit (0.0f, 127.0f, dragValue - delta.y * 127.0f / juce::jmax (1.0f, travel));
        const int v = juce::roundToInt (dragValue);
        int& moved = dragPart == Part::pedal && state.pedalPhysical >= 0 ? state.pedalPhysical
                                                                         : state.analog[ids.analog];
        if (v != moved)
        {
            moved = v;
            if (onAnalog)
                onAnalog (ids.analog, v);
            repaint();
        }
    }
}

void HardwareView::mouseUp (const juce::MouseEvent&)
{
    if (! playable)
        return;
    if (pressedButton >= 0)
    {
        if (onButton)
            onButton (pressedButton, false);
        pressedButton = -1;
        repaint();
    }
    else if (isEncoderKnob (dragPart) && dragTravel < 3.0f)
        pushEncoder (dragPart);
    const int dragged = idsOf (dragPart).analog;
    if (dragged >= 0)
        analogLocalUntil[dragged] = juce::Time::getMillisecondCounter() + kAnalogSettleMs;
    dragPart = Part::none;
}

/* Down as the owner last said, or held down by the mouse now. */
bool HardwareView::isDown (int button) const
{
    return button >= 0 && (state.held[button] || button == pressedButton);
}

/* A click on a knob is a push: down now, up a moment later. */
void HardwareView::pushEncoder (Part p)
{
    const int button = idsOf (p).button;
    if (button < 0 || ! onButton)
        return;
    onButton (button, true);
    juce::Component::SafePointer<HardwareView> safe (this);
    juce::Timer::callAfterDelay (120, [safe, button]
    {
        if (safe != nullptr && safe->onButton)
            safe->onButton (button, false);
    });
}

void HardwareView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto p = partAt (e.position);
    const auto ids = idsOf (p);
    const float amount = wheel.deltaY != 0.0f ? wheel.deltaY : -wheel.deltaX;
    if (! playable || amount == 0.0f || (! isEncoderKnob (p) && ids.analog < 0))
    {
        juce::Component::mouseWheelMove (e, wheel);
        return;
    }

    const int direction = amount > 0.0f ? 1 : -1;
    if (isEncoderKnob (p))
    {
        if (onEncoder)
            onEncoder (ids.encoder, direction);
    }
    else
    {
        int& moved = p == Part::pedal && state.pedalPhysical >= 0 ? state.pedalPhysical : state.analog[ids.analog];
        const int v = juce::jlimit (0, 127, moved + direction * 4);
        moved = v;
        analogLocalUntil[ids.analog] = juce::Time::getMillisecondCounter() + kAnalogSettleMs;
        if (onAnalog)
            onAnalog (ids.analog, v);
        repaint();
    }
}

juce::String HardwareView::getTooltip()
{
    if (hovered == Part::none)
        return {};
    juce::String tip = nameOf (hovered);
    const auto& cap = state.captions[(size_t) hovered];
    if (cap.what.isNotEmpty())
        tip << ": " << cap.what << (cap.detail.isNotEmpty() ? " (" + cap.detail + ")" : juce::String());
    if (hovered == Part::screen)
        tip << "\nClick to edit the global settings.";
    else if (! playable)
        tip << "\nClick to edit.";
    else if (isEncoderKnob (hovered))
        tip << "\nClick to edit it and push it; drag or scroll to turn.";
    else if (hovered == Part::volumeKnob || hovered == Part::pedal)
        tip << "\nClick to edit; drag up and down, or scroll, to move.";
    else
        tip << "\nClick to edit; hold to press.";
    return tip;
}
