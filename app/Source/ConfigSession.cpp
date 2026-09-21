#include "ConfigSession.h"
#include "midi_control/midi_control_version.h"

static constexpr int kReplyTimeoutMs = 2000;

ConfigSession::ConfigSession()
{
    midi_control_config_defaults (&cfg);
}

ConfigSession::~ConfigSession()
{
    setLink (nullptr);
}

void ConfigSession::setLink (DeviceLink* newLink)
{
    if (link != nullptr)
        link->removeListener (this);
    stopTimer();
    queue.clear();
    inFlight = false;
    connected = false;
    haveConfig = false;
    haveState = false;
    stateRefused = false;
    link = newLink;
    if (link != nullptr)
        link->addListener (this);
    notifyChanged();
}

void ConfigSession::enqueue (Kind kind, const uint8_t* bytes, uint32_t len)
{
    Request r;
    r.kind = kind;
    r.bytes.assign (bytes, bytes + len);
    enqueue (std::move (r));
}

void ConfigSession::enqueue (Request r)
{
    queue.push_back (std::move (r));
    if (! inFlight)
        sendNext();
}

void ConfigSession::sendNext()
{
    if (queue.empty() || link == nullptr || ! link->isOpen())
    {
        queue.clear();
        inFlight = false;
        return;
    }
    inFlight = true;
    const auto& r = queue.front();
    const juce::MidiMessage m (r.bytes.data(), (int) r.bytes.size());
    listeners.call ([&] (Listener& l) { l.sessionSent (*this, m); });
    link->sendMidi (m);
    startTimer (kReplyTimeoutMs);
}

void ConfigSession::finish (bool ok, const juce::String& msg, bool notify)
{
    stopTimer();
    if (msg.isNotEmpty())
        message (msg);
    if (! queue.empty())
        queue.pop_front();
    inFlight = false;
    if (! ok)
    {
        /* A failed step invalidates the ones queued behind it. */
        queue.clear();
    }
    if (notify)
        notifyChanged();
    sendNext();
}

void ConfigSession::timerCallback()
{
    if (! inFlight)
    {
        stopTimer();
        return;
    }
    connected = false;
    haveState = false;
    finish (false, "no reply from the device - is it running the " MIDI_CONTROL_PRODUCT_NAME " firmware?");
}

/* ---- commands --------------------------------------------------------------- */

void ConfigSession::connect()
{
    uint8_t buf[MIDI_CONTROL_SYSEX_MAX];
    enqueue (Kind::identify, buf, midi_control_build_simple (buf, MIDI_CONTROL_CMD_IDENTIFY));
    enqueue (Kind::getConfig, buf, midi_control_build_simple (buf, MIDI_CONTROL_CMD_GET_CONFIG));
}

void ConfigSession::readConfig()
{
    uint8_t buf[MIDI_CONTROL_SYSEX_MAX];
    enqueue (Kind::getConfig, buf, midi_control_build_simple (buf, MIDI_CONTROL_CMD_GET_CONFIG));
}

void ConfigSession::applyConfig()
{
    uint8_t rec[MIDI_CONTROL_CONFIG_BYTES];
    uint8_t buf[MIDI_CONTROL_SYSEX_MAX];
    midi_control_config_sanitise (&cfg);
    midi_control_config_write (&cfg, rec);
    enqueue (Kind::setConfig, buf, midi_control_build_config (buf, MIDI_CONTROL_CMD_SET_CONFIG, rec, MIDI_CONTROL_CONFIG_BYTES));
    enqueue (Kind::status, buf, midi_control_build_simple (buf, MIDI_CONTROL_CMD_GET_STATUS));
}

void ConfigSession::save()
{
    uint8_t buf[MIDI_CONTROL_SYSEX_MAX];
    enqueue (Kind::save, buf, midi_control_build_simple (buf, MIDI_CONTROL_CMD_SAVE));
    enqueue (Kind::status, buf, midi_control_build_simple (buf, MIDI_CONTROL_CMD_GET_STATUS));
}

void ConfigSession::loadDefaults()
{
    uint8_t buf[MIDI_CONTROL_SYSEX_MAX];
    enqueue (Kind::defaults, buf, midi_control_build_simple (buf, MIDI_CONTROL_CMD_LOAD_DEFAULTS));
    enqueue (Kind::getConfig, buf, midi_control_build_simple (buf, MIDI_CONTROL_CMD_GET_CONFIG));
}

void ConfigSession::reloadFromStorage()
{
    uint8_t buf[MIDI_CONTROL_SYSEX_MAX];
    enqueue (Kind::reload, buf, midi_control_build_simple (buf, MIDI_CONTROL_CMD_RELOAD));
    enqueue (Kind::getConfig, buf, midi_control_build_simple (buf, MIDI_CONTROL_CMD_GET_CONFIG));
}

void ConfigSession::refreshStatus()
{
    uint8_t buf[MIDI_CONTROL_SYSEX_MAX];
    enqueue (Kind::status, buf, midi_control_build_simple (buf, MIDI_CONTROL_CMD_GET_STATUS));
}

void ConfigSession::setText (int line, const juce::String& text)
{
    uint8_t buf[MIDI_CONTROL_SYSEX_MAX];
    enqueue (Kind::text, buf, midi_control_build_text (buf, (uint8_t) line, text.toRawUTF8()));
}

void ConfigSession::requestState()
{
    if (! canPlay())
        return;
    /* One waiting is enough. One already sent is not: it may have left before
     * the change it is now wanted for. */
    if (queue.size() > (inFlight ? 1u : 0u) && queue.back().kind == Kind::state)
        return;
    uint8_t buf[MIDI_CONTROL_SYSEX_MAX];
    enqueue (Kind::state, buf, midi_control_build_simple (buf, MIDI_CONTROL_CMD_GET_STATE));
}

void ConfigSession::pressButton (int button, bool down)
{
    enqueueEvent (MIDI_CONTROL_EVENT_BUTTON, button, down ? 1 : 0);
}

void ConfigSession::turnEncoder (int encoder, int detents)
{
    enqueueEvent (MIDI_CONTROL_EVENT_ENCODER, encoder, juce::jlimit (-64, 63, detents));
}

void ConfigSession::moveAnalog (int analog, int value)
{
    enqueueEvent (MIDI_CONTROL_EVENT_ANALOG, analog, juce::jlimit (0, 127, value));
}

void ConfigSession::enqueueEvent (uint8_t kind, int id, int value)
{
    if (! canPlay() || id < 0 || id > 127)
        return;
    uint8_t buf[MIDI_CONTROL_SYSEX_MAX];

    /* A drag makes events faster than a unit answers them. Rather than queue
     * every step, a position still waiting to be sent is moved, and detents
     * still waiting are added to. Switches are never merged, and nothing is
     * merged across another request but a state request. */
    if (kind != MIDI_CONTROL_EVENT_BUTTON)
    {
        const size_t firstUnsent = inFlight ? 1u : 0u;
        size_t i = queue.size();
        while (i > firstUnsent && queue[i - 1].kind == Kind::state)
            --i;
        if (i > firstUnsent)
        {
            auto& r = queue[i - 1];
            const int merged = kind == MIDI_CONTROL_EVENT_ENCODER ? r.eventValue + value : value;
            const bool fits = kind != MIDI_CONTROL_EVENT_ENCODER || (merged >= -64 && merged <= 63);
            if (r.kind == Kind::event && r.eventKind == kind && r.eventId == id && fits)
            {
                r.eventValue = merged;
                const auto n = midi_control_build_event (buf, kind, (uint8_t) id, merged);
                r.bytes.assign (buf, buf + n);
                requestState();
                return;
            }
        }
    }

    Request r;
    r.kind = Kind::event;
    r.eventKind = kind;
    r.eventId = id;
    r.eventValue = value;
    const auto n = midi_control_build_event (buf, kind, (uint8_t) id, value);
    r.bytes.assign (buf, buf + n);
    enqueue (std::move (r));
    requestState();
}

void ConfigSession::configEdited()
{
    if (autoApply && connected)
        applyConfig();
    else
        notifyChanged();
}

void ConfigSession::replaceConfig (const midi_control_config& c)
{
    const auto seq = cfg.sequence;
    cfg = c;
    cfg.sequence = seq;
    midi_control_config_sanitise (&cfg);
    haveConfig = true;
    if (connected)
        applyConfig();
    else
        notifyChanged();
}

/* ---- replies ---------------------------------------------------------------- */

void ConfigSession::deviceMidiReceived (DeviceLink&, const juce::MidiMessage& m)
{
    if (! m.isSysEx() || ! inFlight || queue.empty())
        return;

    midi_control_message msg;
    if (! midi_control_parse_message (m.getRawData(), (uint32_t) m.getRawDataSize(), &msg))
        return;

    const Kind kind = queue.front().kind;

    switch (msg.command)
    {
        case MIDI_CONTROL_RPL_IDENTITY:
        {
            midi_control_identity id;
            if (kind != Kind::identify || ! midi_control_parse_identity (&msg, &id))
                return;
            ident = id;
            connected = true;
            haveState = false;
            stateRefused = false;
            finish (true,juce::String ("connected: firmware ")
                              + juce::String (id.fw_major) + "." + juce::String (id.fw_minor) + "."
                              + juce::String (id.fw_patch)
                              + (id.platform == MIDI_CONTROL_PLATFORM_EMULATOR ? " (emulator)" : " (hardware)")
                              + ", storage " + midi_control_storage_state_name (id.storage_state));
            break;
        }

        case MIDI_CONTROL_RPL_CONFIG:
        {
            uint8_t rec[MIDI_CONTROL_CONFIG_BYTES];
            midi_control_config c {};
            if (kind != Kind::getConfig)
                return;
            const auto n = midi_control_parse_config_payload (&msg, rec, sizeof (rec));
            const int rc = n ? midi_control_config_read (&c, rec, n) : MIDI_CONTROL_STATUS_BAD_LENGTH;
            if (rc != MIDI_CONTROL_STATUS_OK)
            {
                finish (false, juce::String ("configuration from device rejected: ")
                                   + midi_control_status_name ((unsigned) rc));
                return;
            }
            cfg = c;
            haveConfig = true;
            finish (true, "configuration read from device (sequence "
                              + juce::String ((int) c.sequence) + ")");
            break;
        }

        case MIDI_CONTROL_RPL_STATUS:
        {
            midi_control_status_reply st;
            if (kind != Kind::status || ! midi_control_parse_status (&msg, &st))
                return;
            stat = st;
            ident.storage_state = st.storage_state;
            ident.dirty = st.dirty;
            finish (true, {});
            break;
        }

        case MIDI_CONTROL_RPL_STATE:
        {
            midi_control_state_reply st;
            if (kind != Kind::state || ! midi_control_parse_state (&msg, &st))
                return;
            live = st;
            haveState = true;

            /* A profile chosen on the panel moves the editing copy with it, so
             * that the mapping the editor shows is the one the unit is playing
             * and the next edit does not switch the unit back. The profile
             * being left keeps whatever has been done to it (docs/05 §7), and
             * nothing is sent: the device is where this came from. */
            const bool movedProfile = haveConfig && st.profile_active != cfg.profile_active
                                      && midi_control_config_select_profile (&cfg, st.profile_active) != 0;

            /* Of all this, only the storage state, the unsaved flag and the
             * live profile are shown outside the live view. */
            const bool changed = movedProfile
                              || ident.storage_state != st.storage_state || ident.dirty != st.dirty;
            ident.storage_state = stat.storage_state = st.storage_state;
            ident.dirty = stat.dirty = st.dirty;
            stat.program = st.program;
            listeners.call ([this] (Listener& l) { l.sessionState (*this); });
            finish (true, {}, changed);
            break;
        }

        case MIDI_CONTROL_RPL_RESULT:
        {
            uint8_t forCmd = 0, status = 0;
            if (! midi_control_parse_result (&msg, &forCmd, &status))
                return;
            const bool ok = status == MIDI_CONTROL_STATUS_OK;

            /* The live view's own requests. A refused event is reported but
             * leaves the queue alone; a device that refuses GET_STATE is not
             * asked again, and the drawing goes back to a picture. */
            if (kind == Kind::event)
            {
                finish (true, ok ? juce::String() : "control event: " + juce::String (midi_control_status_name (status)), false);
                break;
            }
            if (kind == Kind::state)
            {
                stateRefused = true;
                finish (true, "the device does not report its state: " + juce::String (midi_control_status_name (status)));
                break;
            }

            juce::String what;
            switch (forCmd)
            {
                case MIDI_CONTROL_CMD_SET_CONFIG:    what = "apply"; break;
                case MIDI_CONTROL_CMD_SAVE:          what = "save"; break;
                case MIDI_CONTROL_CMD_LOAD_DEFAULTS: what = "load defaults"; break;
                case MIDI_CONTROL_CMD_RELOAD:        what = "reload from storage"; break;
                case MIDI_CONTROL_CMD_SET_TEXT:      what = "set text"; break;
                default:                             what = "command " + juce::String (forCmd); break;
            }
            finish (ok, what + ": " + midi_control_status_name (status));
            break;
        }

        default:
            break;
    }
}

void ConfigSession::deviceLinkChanged (DeviceLink&)
{
    notifyChanged();
}

void ConfigSession::notifyChanged()
{
    listeners.call ([this] (Listener& l) { l.sessionChanged (*this); });
}

void ConfigSession::message (const juce::String& s)
{
    listeners.call ([&] (Listener& l) { l.sessionMessage (*this, s); });
}
