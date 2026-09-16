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

    auto* buttons = new juce::DynamicObject();
    for (unsigned i = 0; i < MIDI_CONTROL_BUTTON_COUNT; ++i)
    {
        const auto& b = cfg.buttons[i];
        auto* o = new juce::DynamicObject();
        o->setProperty ("action", keyOf (actionKeys(), b.action));
        o->setProperty ("channel", (int) b.channel);
        o->setProperty ("number", (int) b.number);
        o->setProperty ("value_on", (int) b.value_on);
        o->setProperty ("value_off", (int) b.value_off);
        o->setProperty ("led_on", (int) b.led_on);
        o->setProperty ("led_off", (int) b.led_off);
        buttons->setProperty (keyOf (buttonKeys(), i), juce::var (o));
    }
    root->setProperty ("buttons", juce::var (buttons));

    auto* encoders = new juce::DynamicObject();
    for (unsigned i = 0; i < MIDI_CONTROL_ENCODER_COUNT; ++i)
    {
        const auto& e = cfg.encoders[i];
        auto* o = new juce::DynamicObject();
        o->setProperty ("mode", keyOf (encoderModeKeys(), e.mode));
        o->setProperty ("channel", (int) e.channel);
        o->setProperty ("number", (int) e.number);
        o->setProperty ("step", (int) e.step);
        o->setProperty ("value", (int) e.value);
        encoders->setProperty (keyOf (encoderKeys(), i), juce::var (o));
    }
    root->setProperty ("encoders", juce::var (encoders));

    auto* analogs = new juce::DynamicObject();
    for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; ++i)
    {
        const auto& a = cfg.analogs[i];
        auto* o = new juce::DynamicObject();
        o->setProperty ("mode", keyOf (analogModeKeys(), a.mode));
        o->setProperty ("channel", (int) a.channel);
        o->setProperty ("number", (int) a.number);
        o->setProperty ("invert", a.invert != 0);
        o->setProperty ("out_min", (int) a.out_min);
        o->setProperty ("out_max", (int) a.out_max);
        analogs->setProperty (keyOf (analogKeys(), i), juce::var (o));
    }
    root->setProperty ("analogs", juce::var (analogs));

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
        const auto name = go->getProperty ("name").toString().substring (0, MIDI_CONTROL_CONFIG_NAME_LEN - 1);
        memset (c.global.name, 0, MIDI_CONTROL_CONFIG_NAME_LEN);
        memcpy (c.global.name, name.toRawUTF8(), (size_t) name.getNumBytesAsUTF8());
        c.global.midi_channel = (uint8_t) (intOf (g, "midi_channel", c.global.midi_channel + 1, 1, 16) - 1);
        c.global.bank_size = (uint8_t) intOf (g, "bank_size", c.global.bank_size, 1, 16);
        c.global.bank_select = (uint8_t) intOf (g, "bank_select", c.global.bank_select, 0, 1);
        c.global.follow_program_change = (uint8_t) intOf (g, "follow_program_change", c.global.follow_program_change, 0, 1);
        c.global.startup_program = (uint8_t) intOf (g, "startup_program", c.global.startup_program, 0, 127);
        c.global.led_brightness = (uint8_t) intOf (g, "led_brightness", c.global.led_brightness, 0, 255);
        c.global.display_timeout = (uint16_t) intOf (g, "display_timeout", c.global.display_timeout, 0,
                                                     (int) MIDI_CONTROL_DISPLAY_TIMEOUT_MAX);
    }

    if (auto* bo = obj->getProperty ("buttons").getDynamicObject())
    {
        const auto keys = buttonKeys();
        for (unsigned i = 0; i < MIDI_CONTROL_BUTTON_COUNT; ++i)
        {
            const auto v = bo->getProperty (keys[(int) i]);
            auto* o = v.getDynamicObject();
            if (o == nullptr)
                continue;
            auto& b = c.buttons[i];
            b.action = (uint8_t) indexOf (actionKeys(), o->getProperty ("action"), b.action);
            b.channel = (uint8_t) intOf (v, "channel", b.channel, 0, 16);
            b.number = (uint8_t) intOf (v, "number", b.number, 0, 127);
            b.value_on = (uint8_t) intOf (v, "value_on", b.value_on, 0, 127);
            b.value_off = (uint8_t) intOf (v, "value_off", b.value_off, 0, 127);
            b.led_on = (uint8_t) intOf (v, "led_on", b.led_on, 0, 7);
            b.led_off = (uint8_t) intOf (v, "led_off", b.led_off, 0, 7);
        }
    }

    if (auto* eo = obj->getProperty ("encoders").getDynamicObject())
    {
        const auto keys = encoderKeys();
        for (unsigned i = 0; i < MIDI_CONTROL_ENCODER_COUNT; ++i)
        {
            const auto v = eo->getProperty (keys[(int) i]);
            auto* o = v.getDynamicObject();
            if (o == nullptr)
                continue;
            auto& e = c.encoders[i];
            e.mode = (uint8_t) indexOf (encoderModeKeys(), o->getProperty ("mode"), e.mode);
            e.channel = (uint8_t) intOf (v, "channel", e.channel, 0, 16);
            e.number = (uint8_t) intOf (v, "number", e.number, 0, 127);
            e.step = (uint8_t) intOf (v, "step", e.step, 1, 64);
            e.value = (uint8_t) intOf (v, "value", e.value, 0, 127);
        }
    }

    if (auto* ao = obj->getProperty ("analogs").getDynamicObject())
    {
        const auto keys = analogKeys();
        for (unsigned i = 0; i < MIDI_CONTROL_ANALOG_COUNT; ++i)
        {
            const auto v = ao->getProperty (keys[(int) i]);
            auto* o = v.getDynamicObject();
            if (o == nullptr)
                continue;
            auto& a = c.analogs[i];
            a.mode = (uint8_t) indexOf (analogModeKeys(), o->getProperty ("mode"), a.mode);
            a.channel = (uint8_t) intOf (v, "channel", a.channel, 0, 16);
            a.number = (uint8_t) intOf (v, "number", a.number, 0, 127);
            a.invert = (uint8_t) intOf (v, "invert", a.invert, 0, 1);
            a.out_min = (uint8_t) intOf (v, "out_min", a.out_min, 0, 127);
            a.out_max = (uint8_t) intOf (v, "out_max", a.out_max, 0, 127);
        }
    }

    midi_control_config_sanitise (&c);
    cfg = c;
    return true;
}

} // namespace ConfigJson
