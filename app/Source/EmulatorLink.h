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
 * Of the board layer it models one thing, the expression pedal's converter:
 * the pedal has a physical position and a travel in raw counts, and the device
 * is fed those counts as the firmware's board layer feeds it, so the pedal's
 * auto-range and its calibration run here as they do on a unit. The rest - the
 * volume knob's converter, debounce, the LED multiplex timing, USB enumeration
 * - it does not model; the knob is handed a position, and the others are
 * firmware matters tested on the unit. */
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

    /* The emulated pedal. Moved as a foot moves it - 0 heel down to 127 toe
     * down - it reads the raw count its travel puts there, falling toe-down
     * like the unit's, and the device is given that count straight away and
     * again at every tick, as the board layer's scan gives it one. This is the
     * board, not the host, so it goes nowhere near the link. */
    void movePedal (int position);
    int getPedalPosition() const { return pedalPosition; }

    /* The raw counts at the pedal's two stops. The verified unit's by default
     * (the firmware's seeds); another unit's to see what the auto-range and
     * calibration make of a pedal that is not the one the seeds came from. */
    void setPedalTravel (int toe, int heel);
    int getPedalToe() const  { return travelToe; }
    int getPedalHeel() const { return travelHeel; }

    juce::File storageDirectory() const;

private:
    static void midiOutThunk (void* ctx, const uint8_t* bytes, uint32_t len);
    static int storageStateThunk (void* ctx);
    static int storageReadThunk (void* ctx, unsigned slot, uint8_t* buf, uint32_t len);
    static int storageWriteThunk (void* ctx, unsigned slot, const uint8_t* buf, uint32_t len);

    juce::File slotFile (unsigned slot) const;
    void initDevice();
    void feedPedal();
    void timerCallback() override;

    midi_control_device dev {};
    StorageMode mode = StorageMode::ready;
    juce::uint32 lastTick = 0;
    int pedalPosition = 0;
    int travelToe = MIDI_CONTROL_SEED_PEDAL_LO, travelHeel = MIDI_CONTROL_SEED_PEDAL_HI;

    JUCE_DECLARE_WEAK_REFERENCEABLE (EmulatorLink)
};
