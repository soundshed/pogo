#pragma once

#include <deque>
#include <juce_events/juce_events.h>
#include "DeviceLink.h"
#include "midi_control/midi_control_config.h"
#include "midi_control/midi_control_protocol.h"

/* The host side of the configuration protocol: one request in flight at a
 * time, replies matched by order, a timeout so a silent device is reported
 * rather than waited for. Holds the app's editing copy of the configuration.
 *
 * The editing copy is the app's, not the device's: `config()` is what the
 * editor changes, `applyConfig()` sends it, `readConfig()` replaces it with
 * the device's. With `autoApply` on, every edit is sent as it is made.
 *
 * It also carries the live view's traffic: the state requests the drawn unit
 * is drawn from, and the control events it plays the device with. Those leave
 * the session as it was, so listeners hear of a state through `sessionState`
 * alone, and a refused event does not drop the requests behind it. */
class ConfigSession : private DeviceLink::Listener,
                      private juce::Timer
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        /* Identity, status or the configuration changed. */
        virtual void sessionChanged (ConfigSession&) = 0;
        /* A line for the user: a result, an error, a note. */
        virtual void sessionMessage (ConfigSession&, const juce::String&) {}
        /* A request is going to the device. */
        virtual void sessionSent (ConfigSession&, const juce::MidiMessage&) {}
        /* The device reported its state (requestState). */
        virtual void sessionState (ConfigSession&) {}
    };

    ConfigSession();
    ~ConfigSession() override;

    void setLink (DeviceLink* link);
    DeviceLink* getLink() const { return link; }

    bool isConnected() const { return connected; }
    bool hasConfig() const   { return haveConfig; }
    bool isBusy() const      { return ! queue.empty(); }

    const midi_control_identity& identity() const     { return ident; }
    const midi_control_status_reply& status() const   { return stat; }
    midi_control_config& config()                      { return cfg; }
    const midi_control_config& config() const          { return cfg; }

    bool autoApply = true;

    /* IDENTIFY, then GET_CONFIG. */
    void connect();
    void readConfig();
    void applyConfig();
    void save();
    void loadDefaults();
    void reloadFromStorage();
    void refreshStatus();
    void setText (int line, const juce::String& text);

    /* The editor changed `config()`. Sends it if autoApply is on. */
    void configEdited();

    /* Replace the editing copy (from a file, say) and send it. */
    void replaceConfig (const midi_control_config& c);

    /* The device reports its state and takes control events: protocol 2 on. */
    bool canPlay() const { return connected && ident.protocol_version >= 2 && ! stateRefused; }

    /* The state the device last reported since it was connected. */
    bool hasState() const { return haveState; }
    const midi_control_state_reply& state() const { return live; }

    /* GET_STATE, unless one is already waiting to be sent. */
    void requestState();

    /* The drawn unit played: a switch pressed or released, detents on an
     * encoder, an analog position. Each is followed by a state request, so
     * the drawing catches up at once. Nothing is sent unless canPlay(). */
    void pressButton (int button, bool down);
    void turnEncoder (int encoder, int detents);
    void moveAnalog (int analog, int value);

    /* The pedal calibrated by capture (docs/05 section 6): the user holds it
     * at one end and then the other, and the device takes the two counts as
     * its travel. The device runs it and reports it in its state; this starts
     * or cancels it. Protocol 5 on. */
    void calibratePedal (bool start);

    /* The device can do that: protocol 5, and it reads the pedal's raw counts
     * rather than being handed a position - a unit, or the emulator, whose
     * pedal is emulated down to the converter. Known from the first state
     * report after connecting. */
    bool canCalibrate() const
    {
        return canPlay() && ident.protocol_version >= 5 && haveState
            && (live.pedal_flags & MIDI_CONTROL_PEDAL_RAW) != 0;
    }

    void addListener (Listener* l)    { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

private:
    enum class Kind { identify, getConfig, setConfig, save, defaults, reload, status, text, state, event, calibrate };

    struct Request
    {
        Kind kind;
        std::vector<uint8_t> bytes;
        int eventKind = -1, eventId = -1, eventValue = 0;   /* an event's, so a drag's can be merged */
    };

    void enqueue (Kind kind, const uint8_t* bytes, uint32_t len);
    void enqueue (Request r);
    void enqueueEvent (uint8_t kind, int id, int value);
    void sendNext();
    /* The request in flight is answered. A failure drops the ones queued
     * behind it; `notify` is false for replies that change nothing a
     * listener shows. */
    void finish (bool ok, const juce::String& message, bool notify = true);

    void deviceMidiReceived (DeviceLink&, const juce::MidiMessage&) override;
    void deviceLinkChanged (DeviceLink&) override;
    void timerCallback() override;

    void notifyChanged();
    void message (const juce::String& s);

    DeviceLink* link = nullptr;
    std::deque<Request> queue;
    bool inFlight = false;
    bool connected = false;
    bool haveConfig = false;
    bool haveState = false;
    bool stateRefused = false;          /* it answered GET_STATE with a refusal: not asked again */
    midi_control_identity ident {};
    midi_control_status_reply stat {};
    midi_control_state_reply live {};
    midi_control_config cfg {};
    juce::ListenerList<Listener> listeners;
};
