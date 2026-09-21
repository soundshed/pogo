#include "EmulatorLink.h"
#include "midi_control/midi_control_version.h"

EmulatorLink::EmulatorLink()
{
    initDevice();
    lastTick = juce::Time::getMillisecondCounter();
    startTimer (50);
}

EmulatorLink::~EmulatorLink() = default;

void EmulatorLink::initDevice()
{
    midi_control_platform_ops ops {};
    ops.ctx = this;
    ops.midi_out = midiOutThunk;
    ops.storage_state = storageStateThunk;
    ops.storage_read = storageReadThunk;
    ops.storage_write = storageWriteThunk;
    ops.platform = MIDI_CONTROL_PLATFORM_EMULATOR;
    ops.fw_major = MIDI_CONTROL_VERSION_MAJOR;
    ops.fw_minor = MIDI_CONTROL_VERSION_MINOR;
    ops.fw_patch = MIDI_CONTROL_VERSION_PATCH;
    midi_control_device_init (&dev, &ops);

    /* Seeded as the firmware seeds it, after init, so a calibration stored in
     * the slot files wins over the seeds exactly as it does on the unit. */
    midi_control_device_analog_seed (&dev, MIDI_CONTROL_ANALOG_PEDAL, MIDI_CONTROL_SEED_PEDAL_LO,
                                     MIDI_CONTROL_SEED_PEDAL_HI, 1);
    feedPedal();
}

void EmulatorLink::feedPedal()
{
    const int span = travelHeel - travelToe;
    const int raw = travelHeel - (pedalPosition * span + 63) / 127;
    midi_control_device_analog_raw (&dev, MIDI_CONTROL_ANALOG_PEDAL, (uint16_t) juce::jlimit (0, 0xFFFF, raw));
}

void EmulatorLink::movePedal (int position)
{
    pedalPosition = juce::jlimit (0, 127, position);
    feedPedal();
}

void EmulatorLink::setPedalTravel (int toe, int heel)
{
    toe = juce::jlimit (0, 1023, toe);
    heel = juce::jlimit (0, 1023, heel);
    if (toe > heel)
        std::swap (toe, heel);
    travelToe = toe;
    travelHeel = heel;
    feedPedal();
}

void EmulatorLink::powerCycle()
{
    initDevice();
    dispatchChanged();
}

juce::File EmulatorLink::storageDirectory() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("midi-control")
               .getChildFile ("emulator");
}

juce::File EmulatorLink::slotFile (unsigned slot) const
{
    return storageDirectory().getChildFile ("config-slot-" + juce::String (slot) + ".bin");
}

void EmulatorLink::sendMidi (const juce::MidiMessage& m)
{
    midi_control_device_midi_in (&dev, m.getRawData(), (uint32_t) m.getRawDataSize());
}

/* Time passing, which the display timeout counts, and the board's scan: the
 * pedal's count arrives on every pass whether it moved or not, which is what a
 * hold at a stop is made of. The app sees the screen go dark, or a calibration
 * step taken, in the next GET_STATE, as it does a unit's. */
void EmulatorLink::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    feedPedal();
    midi_control_device_tick (&dev, now - lastTick);
    lastTick = now;
}

/* ---- platform callbacks ------------------------------------------------ */

void EmulatorLink::midiOutThunk (void* ctx, const uint8_t* bytes, uint32_t len)
{
    auto* self = static_cast<EmulatorLink*> (ctx);
    juce::MidiMessage m (bytes, (int) len);
    juce::WeakReference<EmulatorLink> safe (self);
    /* Asynchronously, so a reply never arrives inside the call that
     * provoked it - the same shape as a real MIDI port. */
    juce::MessageManager::callAsync ([safe, m]
    {
        if (auto* s = safe.get())
            s->dispatchReceived (m);
    });
}

int EmulatorLink::storageStateThunk (void* ctx)
{
    auto* self = static_cast<EmulatorLink*> (ctx);
    switch (self->mode)
    {
        case StorageMode::ready:  return MIDI_CONTROL_STORAGE_READY;
        case StorageMode::locked: return MIDI_CONTROL_STORAGE_LOCKED;
        default:                  return MIDI_CONTROL_STORAGE_NONE;
    }
}

int EmulatorLink::storageReadThunk (void* ctx, unsigned slot, uint8_t* buf, uint32_t len)
{
    auto* self = static_cast<EmulatorLink*> (ctx);
    auto f = self->slotFile (slot);
    memset (buf, 0, len);
    if (! f.existsAsFile())
        return 1;                       /* an empty slot: the CRC will reject it */
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb))
        return 0;
    memcpy (buf, mb.getData(), juce::jmin ((size_t) len, mb.getSize()));
    return 1;
}

int EmulatorLink::storageWriteThunk (void* ctx, unsigned slot, const uint8_t* buf, uint32_t len)
{
    auto* self = static_cast<EmulatorLink*> (ctx);
    auto f = self->slotFile (slot);
    f.getParentDirectory().createDirectory();
    if (! f.replaceWithData (buf, len))
        return MIDI_CONTROL_STATUS_WRITE_FAILED;
    juce::MemoryBlock back;
    if (! f.loadFileAsData (back) || back.getSize() != len || memcmp (back.getData(), buf, len) != 0)
        return MIDI_CONTROL_STATUS_VERIFY_FAILED;
    return MIDI_CONTROL_STATUS_OK;
}
