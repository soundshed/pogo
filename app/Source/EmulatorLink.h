#pragma once

#include "DeviceLink.h"
#include "midi_control/midi_control_device.h"

/* The unit, emulated: the same midi_control_device the firmware runs, with the
 * two configuration slots kept as files in the user's application-data folder
 * instead of on the eMMC. The app draws and plays it the way it draws and plays
 * a unit, over the link (GET_STATE and EVENT), so what the window shows of the
 * emulator is what it would show of the unit. A timer is its clock, as SysTick
 * is the unit's, so its display timeout runs.
 *
 * What it does not model is the board layer: ADC scaling and auto-ranging,
 * debounce, the LED multiplex timing, USB enumeration. Those are firmware
 * matters and are tested on the unit. */
class EmulatorLink : public DeviceLink,
                     private juce::Timer
{
public:
    enum class StorageMode { ready, locked, none };

    EmulatorLink();
    ~EmulatorLink() override;

    juce::String getName() const override { return "Emulator"; }
    bool isOpen() const override          { return true; }
    bool isEmulator() const override      { return true; }
    void sendMidi (const juce::MidiMessage& m) override;

    StorageMode getStorageMode() const { return mode; }
    void setStorageMode (StorageMode m) { mode = m; }

    /* Re-initialise from storage, as a power cycle would. */
    void powerCycle();

    juce::File storageDirectory() const;

private:
    static void midiOutThunk (void* ctx, const uint8_t* bytes, uint32_t len);
    static int storageStateThunk (void* ctx);
    static int storageReadThunk (void* ctx, unsigned slot, uint8_t* buf, uint32_t len);
    static int storageWriteThunk (void* ctx, unsigned slot, const uint8_t* buf, uint32_t len);

    juce::File slotFile (unsigned slot) const;
    void initDevice();
    void timerCallback() override;

    midi_control_device dev {};
    StorageMode mode = StorageMode::ready;
    juce::uint32 lastTick = 0;

    JUCE_DECLARE_WEAK_REFERENCEABLE (EmulatorLink)
};
