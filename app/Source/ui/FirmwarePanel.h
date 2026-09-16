#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace MidiControlUsb
{
    class VendorSession;
}

/* Putting the firmware on the unit, with nothing but this app. The steps are
 * the ones docs/06-persistence.md describes, in order, each gated on the
 * state the previous one leaves the unit in:
 *
 *   1. reboot into DFU                               (needs: factory system, or our firmware)
 *   2. load the firmware into RAM over DFU           (needs: DFU device)
 *   3. back up the eMMC, twice, compare              (needs: our firmware)
 *   4. install the firmware to the eMMC              (needs: 3 in this session)
 *   5. restore a backup to the eMMC                  (needs: our firmware)
 *
 * and one card that does the whole way back in one go:
 *
 *   restore the factory system from its release file
 *                                                    (needs: our firmware, or DFU)
 *
 * Steps 1, 2 and 3 are reversible by a power cycle. Steps 4 and 5 and the
 * factory restore are not, and are the only things in the app that write
 * persistent storage.
 *
 * Each step is a card saying what it does, whether a power cycle undoes it,
 * what it needs and whether the unit has that now; the log runs beside them. */
class FirmwarePanel : public juce::Component,
                      private juce::Timer
{
public:
    FirmwarePanel();
    ~FirmwarePanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum class Result { none, ok, failed };

    static constexpr int kStepCount = 6;
    static constexpr int kFactoryStep = 5;

    void timerCallback() override;
    void refreshDetection();
    void locateFirmware();
    void chooseFirmware();
    void log (const juce::String& line);
    void setBusy (bool busy);
    void updateButtons();
    void runInBackground (std::function<void()> work);
    void confirm (const juce::String& title, const juce::String& message, std::function<void()> onYes);
    void markStep (int step, bool ok);
    bool stepReady (int step) const;

    void doRebootToDfu();
    void doDfuLoad();
    void doBackup();
    void doInstall();
    void doRestore();
    void doFactoryRestore();
    void doReadLog();

    /* Worker-thread parts. */
    bool backupEmmc (MidiControlUsb::VendorSession& session, std::vector<uint8_t>& data, juce::File& saved);
    void publishBackup (const juce::File& saved);
    bool runFactoryRestore (const std::vector<uint8_t>& ramImage, const std::vector<uint8_t>& factory);

    juce::File backupsDirectory() const;
    bool loadFirmware (std::vector<uint8_t>& out, juce::String& error) const;

    juce::String detectionText;
    juce::TextButton chooseButton { "Choose..." };
    juce::TextButton refreshButton { "Refresh" };
    juce::TextButton rebootButton { "Reboot" };
    juce::TextButton loadButton { "Load" };
    juce::TextButton backupButton { "Back up" };
    juce::TextButton installButton { "Install..." };
    juce::TextButton restoreButton { "Restore..." };
    juce::TextButton factoryButton { "Factory..." };
    juce::TextButton readLogButton { "Read device log" };
    juce::TextButton openBackupsButton { "Backups folder" };
    juce::TextEditor logText;

    juce::Rectangle<int> headerArea, imageCard, logCard;
    juce::Rectangle<int> stepCards[kStepCount];
    Result results[kStepCount] {};

    juce::File firmwareFile;
    juce::File factoryRelease;
    juce::File verifiedBackup;
    bool backupVerified = false;
    bool busy = false;
    bool factoryDetected = false, dfuDetected = false, midiControlDetected = false;

    std::unique_ptr<juce::FileChooser> chooser;
};
