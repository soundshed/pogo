#include "EmulatorLink.h"
#include "midi_control/midi_control_version.h"

EmulatorLink::EmulatorLink()
{
    initDevice();
    lastTick = juce::Time::getMillisecondCounter();
    startTimer (100);
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

/* Time passing, which the display timeout counts. The app sees the screen go
 * dark in the next GET_STATE, as it does a unit's. */
void EmulatorLink::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
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
