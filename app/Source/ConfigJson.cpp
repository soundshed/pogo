#include "ConfigJson.h"
#include "midi_control/midi_control_version.h"

namespace ConfigJson
{

juce::StringArray buttonKeys()
{
    return { "fs_a", "fs_b", "fs_c", "fs_d", "tap", "bank_up", "bank_down",
             "view", "bypass", "menu", "mode", "home_enc_push", "menu_enc_push",
             "enc1_push", "enc2_push", "enc3_push", "enc4_push", "enc5_push",
             "toe", "page" };
}

juce::StringArray encoderKeys()
{
    return { "enc1", "enc2", "enc3", "enc4", "enc5", "home", "menu" };
}

juce::StringArray analogKeys()
{
    return { "volume", "pedal" };
}

juce::StringArray actionKeys()
{
    return { "none", "note_momentary", "note_toggle", "cc_momentary", "cc_toggle",
             "program", "program_up", "program_down", "bank_up", "bank_down",
             "preset_in_bank" };
}

juce::StringArray encoderModeKeys()
{
    return { "none", "cc_relative_twos", "cc_relative_offset", "cc_absolute", "program" };
}

juce::StringArray analogModeKeys()
{
    return { "none", "cc", "pitch_bend", "channel_pressure" };
}

static juce::String keyOf (const juce::StringArray& keys, unsigned index)
{
    return index < (unsigned) keys.size() ? keys[(int) index] : keys[0];
}

static int indexOf (const juce::StringArray& keys, const juce::var& v, int fallback)
{
    if (v.isString())
    {
        const int i = keys.indexOf (v.toString());
        return i >= 0 ? i : fallback;
    }
    if (v.isInt() || v.isDouble())
        return juce::jlimit (0, keys.size() - 1, (int) v);
    return fallback;
}

static int intOf (const juce::var& obj, const char* name, int fallback, int lo, int hi)
{
    if (auto* o = obj.getDynamicObject())
    {
        const auto v = o->getProperty (name);
        if (v.isInt() || v.isDouble() || v.isBool())
            return juce::jlimit (lo, hi, (int) v);
    }
    return fallback;
}

/* One mapping block at a time, so the live mapping and every profile are
 * written and read by the same code - there is no second place for a field to
 * be forgotten. */

static juce::var buttonsToJson (const midi_control_button_map* buttons)
{
    auto* o = new juce::DynamicObject();
    for (unsigned i = 0; i < MIDI_CONTROL_BUTTON_COUNT; ++i)
    {
        const auto& b = buttons[i];
        auto* j = new juce::DynamicObject();
        j->setProperty ("action", keyOf (actionKeys(), b.action));
        j->setProperty ("channel", (int) b.channel);
        j->setProperty ("number", (int) b.number);
        j->setProperty ("value_on", (int) b.value_on);
        j->setProperty ("value_off", (int) b.value_off);
        j->setProperty ("led_on", (int) b.led_on);
        j->setProperty ("led_off", (int) b.led_off);
        o->setProperty (keyOf (buttonKeys(), i), juce::var (j));
    }
    return juce::var (o);
}

static juce::var encodersToJson (const midi_control_encoder_map* encoders)
{
    auto* o = new juce::DynamicObject();
    for (unsigned i = 0; i < MIDI_CONTROL_ENCODER_COUNT; ++i)
    {
        const auto& e = encoders[i];
        auto* j = new juce::DynamicObject();
        j->setProperty ("mode", keyOf (encoderModeKeys(), e.mode));
        j->setProperty ("channel", (int) e.channel);
        j->setProperty ("number", (int) e.number);
        j->setProperty ("step", (int) e.step);
        j->setProperty ("value", (int) e.value);
        o->setProperty (keyOf (encoderKeys(), i), juce::var (j));
    }
    return juce::var (o);
}

/* `cal` is the unit's own learned travel and belongs to the live block alone;
 * a profile is a mapping and carries none (docs/05 section 6). */
static juce::var analogsToJson (const midi_control_analog_map* analogs, const midi_control_analog_cal* cal)
{
    auto* o = new juce::DynamicObject();
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; ++i)
    {
        const auto& a = analogs[i];
        auto* j = new juce::DynamicObject();
        j->setProperty ("mode", keyOf (analogModeKeys(), a.mode));
        j->setProperty ("channel", (int) a.channel);
        j->setProperty ("number", (int) a.number);
        j->setProperty ("invert", a.invert != 0);
        j->setProperty ("out_min", (int) a.out_min);
        j->setProperty ("out_max", (int) a.out_max);

        // The travel the unit has learned for itself, written out so it can be
        // read, compared between units and reported in a bug. A unit ignores
        // one it is sent - the calibration belongs to the pedal in front of
        // you, not to the file (docs/05 section 6).
        if (cal != nullptr && (cal[i].flags & MIDI_CONTROL_CAL_VALID))
        {
            auto* cj = new juce::DynamicObject();
            cj->setProperty ("raw_lo", (int) cal[i].raw_lo);
            cj->setProperty ("raw_hi", (int) cal[i].raw_hi);
            j->setProperty ("calibration", juce::var (cj));
        }
        o->setProperty (keyOf (analogKeys(), i), juce::var (j));
    }
    return juce::var (o);
}

static void buttonsFromJson (const juce::var& v, midi_control_button_map* buttons)
{
    auto* bo = v.getDynamicObject();
    if (bo == nullptr)
        return;
    const auto keys = buttonKeys();
    for (unsigned i = 0; i < MIDI_CONTROL_BUTTON_COUNT; ++i)
    {
        const auto e = bo->getProperty (keys[(int) i]);
        auto* o = e.getDynamicObject();
        if (o == nullptr)
            continue;
        auto& b = buttons[i];
        b.action = (uint8_t) indexOf (actionKeys(), o->getProperty ("action"), b.action);
        b.channel = (uint8_t) intOf (e, "channel", b.channel, 0, 16);
        b.number = (uint8_t) intOf (e, "number", b.number, 0, 127);
        b.value_on = (uint8_t) intOf (e, "value_on", b.value_on, 0, 127);
        b.value_off = (uint8_t) intOf (e, "value_off", b.value_off, 0, 127);
        b.led_on = (uint8_t) intOf (e, "led_on", b.led_on, 0, 7);
        b.led_off = (uint8_t) intOf (e, "led_off", b.led_off, 0, 7);
    }
}

static void encodersFromJson (const juce::var& v, midi_control_encoder_map* encoders)
{
    auto* eo = v.getDynamicObject();
    if (eo == nullptr)
        return;
    const auto keys = encoderKeys();
    for (unsigned i = 0; i < MIDI_CONTROL_ENCODER_COUNT; ++i)
    {
        const auto item = eo->getProperty (keys[(int) i]);
        auto* o = item.getDynamicObject();
        if (o == nullptr)
            continue;
        auto& e = encoders[i];
        e.mode = (uint8_t) indexOf (encoderModeKeys(), o->getProperty ("mode"), e.mode);
        e.channel = (uint8_t) intOf (item, "channel", e.channel, 0, 16);
        e.number = (uint8_t) intOf (item, "number", e.number, 0, 127);
        e.step = (uint8_t) intOf (item, "step", e.step, 1, 64);
        e.value = (uint8_t) intOf (item, "value", e.value, 0, 127);
    }
}

static void analogsFromJson (const juce::var& v, midi_control_analog_map* analogs, midi_control_analog_cal* cal)
{
    auto* ao = v.getDynamicObject();
    if (ao == nullptr)
        return;
    const auto keys = analogKeys();
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; ++i)
    {
        const auto item = ao->getProperty (keys[(int) i]);
        auto* o = item.getDynamicObject();
        if (o == nullptr)
            continue;
        auto& a = analogs[i];
        a.mode = (uint8_t) indexOf (analogModeKeys(), o->getProperty ("mode"), a.mode);
        a.channel = (uint8_t) intOf (item, "channel", a.channel, 0, 16);
        a.number = (uint8_t) intOf (item, "number", a.number, 0, 127);
        a.invert = (uint8_t) intOf (item, "invert", a.invert, 0, 1);
        a.out_min = (uint8_t) intOf (item, "out_min", a.out_min, 0, 127);
        a.out_max = (uint8_t) intOf (item, "out_max", a.out_max, 0, 127);

        // Read back so a round trip through a file is lossless. It reaches
        // no unit: SET_CONFIG keeps whichever calibration the unit already
        // has, and only a recalibration clears that.
        const auto cv = o->getProperty ("calibration");
        if (cal != nullptr && cv.getDynamicObject() != nullptr)
        {
            cal[i].raw_lo = (uint16_t) intOf (cv, "raw_lo", cal[i].raw_lo, 0, (int) MIDI_CONTROL_CAL_RAW_MAX);
            cal[i].raw_hi = (uint16_t) intOf (cv, "raw_hi", cal[i].raw_hi, 0, (int) MIDI_CONTROL_CAL_RAW_MAX);
            cal[i].flags = MIDI_CONTROL_CAL_VALID;
        }
    }
}

juce::String toJson (const midi_control_config& cfg)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "midi-control-config");
    root->setProperty ("version", (int) MIDI_CONTROL_CONFIG_VERSION);

    auto* g = new juce::DynamicObject();
    g->setProperty ("name", juce::String (cfg.global.name, MIDI_CONTROL_CONFIG_NAME_LEN).trimCharactersAtEnd (juce::String::charToString (0)));
    g->setProperty ("midi_channel", (int) cfg.global.midi_channel + 1);
    g->setProperty ("bank_size", (int) cfg.global.bank_size);
    g->setProperty ("bank_select", cfg.global.bank_select != 0);
    g->setProperty ("follow_program_change", cfg.global.follow_program_change != 0);
    g->setProperty ("startup_program", (int) cfg.global.startup_program);
    g->setProperty ("led_brightness", (int) cfg.global.led_brightness);
    g->setProperty ("display_timeout", (int) cfg.global.display_timeout);
    root->setProperty ("global", juce::var (g));

    root->setProperty ("buttons", buttonsToJson (cfg.buttons));
    root->setProperty ("encoders", encodersToJson (cfg.encoders));
    root->setProperty ("analogs", analogsToJson (cfg.analogs, cfg.analog_cal));

    // The named mappings. The blocks above are the live one, which is a copy
    // of whichever profile is active, so a file holds it twice - deliberately,
    // because that is how the record is laid out (docs/05 section 7) and it is
    // what a reader that knows nothing of profiles still understands.
    auto* profiles = new juce::DynamicObject();
    profiles->setProperty ("active", (int) cfg.profile_active);
    profiles->setProperty ("in_use", (int) cfg.profile_count);
    juce::Array<juce::var> list;
    for (unsigned i = 0; i < MIDI_CONTROL_PROFILE_COUNT; ++i)
    {
        const auto& p = cfg.profiles[i];
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", juce::String (p.name, MIDI_CONTROL_PROFILE_NAME_LEN)
                                    .trimCharactersAtEnd (juce::String::charToString (0)));
        o->setProperty ("buttons", buttonsToJson (p.buttons));
        o->setProperty ("encoders", encodersToJson (p.encoders));
        o->setProperty ("analogs", analogsToJson (p.analogs, nullptr));
        list.add (juce::var (o));
    }
    profiles->setProperty ("list", list);
    root->setProperty ("profiles", juce::var (profiles));

    return juce::JSON::toString (juce::var (root), false);
}

bool fromJson (const juce::String& text, midi_control_config& cfg, juce::String& error)
{
    juce::var root;
    const auto result = juce::JSON::parse (text, root);
    if (result.failed())
    {
        error = result.getErrorMessage();
        return false;
    }
    auto* obj = root.getDynamicObject();
    if (obj == nullptr)
    {
        error = "not a JSON object";
        return false;
    }
    if (obj->getProperty ("format").toString() != "midi-control-config")
    {
        error = "not a " MIDI_CONTROL_PRODUCT_NAME " configuration file";
        return false;
    }

    midi_control_config c = cfg;

    const auto g = obj->getProperty ("global");
    if (auto* go = g.getDynamicObject())
    {
        // Bounded by bytes: substring() counts characters and the field counts
        // bytes, so a name of multi-byte characters used to run past the end.
        const auto name = go->getProperty ("name").toString();
        memset (c.global.name, 0, MIDI_CONTROL_CONFIG_NAME_LEN);
        memcpy (c.global.name, name.toRawUTF8(),
                juce::jmin ((size_t) MIDI_CONTROL_CONFIG_NAME_LEN - 1, name.getNumBytesAsUTF8()));
        c.global.midi_channel = (uint8_t) (intOf (g, "midi_channel", c.global.midi_channel + 1, 1, 16) - 1);
        c.global.bank_size = (uint8_t) intOf (g, "bank_size", c.global.bank_size, 1, 16);
        c.global.bank_select = (uint8_t) intOf (g, "bank_select", c.global.bank_select, 0, 1);
        c.global.follow_program_change = (uint8_t) intOf (g, "follow_program_change", c.global.follow_program_change, 0, 1);
        c.global.startup_program = (uint8_t) intOf (g, "startup_program", c.global.startup_program, 0, 127);
        c.global.led_brightness = (uint8_t) intOf (g, "led_brightness", c.global.led_brightness, 0, 255);
        c.global.display_timeout = (uint16_t) intOf (g, "display_timeout", c.global.display_timeout, 0,
                                                     (int) MIDI_CONTROL_DISPLAY_TIMEOUT_MAX);
    }

    // The profiles first, so that the live blocks below still have the last
    // word: which mapping is live is the live block's to say, here as in the
    // record (docs/05 section 7).
    const auto pv = obj->getProperty ("profiles");
    if (auto* po = pv.getDynamicObject())
    {
        c.profile_count = (uint8_t) intOf (pv, "in_use", c.profile_count,
                                           1, (int) MIDI_CONTROL_PROFILE_COUNT);
        c.profile_active = (uint8_t) intOf (pv, "active", c.profile_active,
                                            0, (int) MIDI_CONTROL_PROFILE_COUNT - 1);
        if (auto* list = po->getProperty ("list").getArray())
        {
            for (int i = 0; i < juce::jmin (list->size(), (int) MIDI_CONTROL_PROFILE_COUNT); ++i)
            {
                const auto item = (*list)[i];
                auto* o = item.getDynamicObject();
                if (o == nullptr)
                    continue;
                auto& p = c.profiles[i];
                if (o->hasProperty ("name"))
                {
                    // Bounded by bytes, not by characters: the field is 16
                    // bytes and one character can be four of them. A sequence
                    // cut in half is left for sanitise, which turns anything
                    // outside printable ASCII into '?'.
                    const auto name = o->getProperty ("name").toString();
                    memset (p.name, 0, sizeof (p.name));
                    memcpy (p.name, name.toRawUTF8(),
                            juce::jmin ((size_t) MIDI_CONTROL_PROFILE_NAME_LEN - 1,
                                        name.getNumBytesAsUTF8()));
                }
                buttonsFromJson (o->getProperty ("buttons"), p.buttons);
                encodersFromJson (o->getProperty ("encoders"), p.encoders);
                analogsFromJson (o->getProperty ("analogs"), p.analogs, nullptr);
            }
        }
        // A file that names a profile the count does not reach would leave the
        // record saying two different things; sanitise decides, so agree with
        // it here rather than let it surprise the person who wrote the file.
        if (c.profile_active >= c.profile_count)
            c.profile_active = 0;
        midi_control_config_load_profile (&c, c.profile_active);
    }

    buttonsFromJson (obj->getProperty ("buttons"), c.buttons);
    encodersFromJson (obj->getProperty ("encoders"), c.encoders);
    analogsFromJson (obj->getProperty ("analogs"), c.analogs, c.analog_cal);

    midi_control_config_sanitise (&c);
    cfg = c;
    return true;
}

} // namespace ConfigJson
