#include "FirmwarePanel.h"
#include "Theme.h"
#include "midi_control/midi_control_bootimage.h"
#include "midi_control/midi_control_protocol.h"
#include "midi_control/midi_control_version.h"

#include <algorithm>
#include <cmath>

#if JUCE_WINDOWS
 #include "usb/MidiControlUsb.h"
#endif

namespace
{
enum class Badge { reversible, reads, writes };

struct StepText
{
    const char* title;
    const char* what;
    const char* badge;
    Badge kind;
    const char* needs;
};

const StepText kSteps[6] = {
    { "Enter Firmware Update Mode", "The factory system or " MIDI_CONTROL_PRODUCT_NAME " hands over to the boot ROM's USB loader.",
      "POWER CYCLE UNDOES", Badge::reversible, "the factory system fully booted, or " MIDI_CONTROL_PRODUCT_NAME " running" },
    { "Load into RAM", "The boot ROM runs the image from RAM. Nothing is written.", "RAM ONLY", Badge::reversible,
      "the unit in Firmware Update Mode, and an image" },
    { "Back up the eMMC", "Sectors 0 to 1364, read twice, compared, saved and read back.", "READS ONLY", Badge::reads,
      MIDI_CONTROL_PRODUCT_NAME " running on the unit" },
    { "Install to the eMMC", "The image at sector 0: the unit boots it at every power-on.", "WRITES eMMC",
      Badge::writes, MIDI_CONTROL_PRODUCT_NAME " running, an image, and a backup from this session" },
    { "Restore a backup", "A saved backup written back over the start of the eMMC.", "WRITES eMMC", Badge::writes,
      MIDI_CONTROL_PRODUCT_NAME " running on the unit" },
    { "Restore factory system", "The factory release file, checked against the unit, over sectors 0 to 1364.",
      "WRITES eMMC", Badge::writes,
      MIDI_CONTROL_PRODUCT_NAME " running or Firmware Update Mode, an image, and the factory .hxf" },
};

juce::Colour badgeColour (Badge b)
{
    return b == Badge::writes ? theme::warn : b == Badge::reads ? theme::info : theme::good;
}

#if JUCE_WINDOWS
/* What a backup or restore file holds, named from its sector 1. A factory
 * image must have valid CRCs and keep every record in range. */
juce::String describeImage (const uint8_t* data, size_t size)
{
    midi_control_boot_info info;
    if (size < 2 * MIDI_CONTROL_SECTOR_BYTES || ! midi_control_boot_inspect (data, (uint32_t) size, &info)
        || ! info.has_descriptor || ! info.magic_ok)
        return "unrecognised contents";

    const bool intact = info.desc_crc_ok && info.crc_ok && info.records_in_range;
    juce::String what = midi_control_boot_classify (data + MIDI_CONTROL_SECTOR_BYTES) == MIDI_CONTROL_STORAGE_READY
                            ? "a " MIDI_CONTROL_PRODUCT_NAME " image"
                            : intact ? "a factory image" : "a damaged or unrecognised image";
    what << " (" << (int) info.records << " record" << (info.records == 1 ? "" : "s") << ", " << (int) info.size
         << " bytes, CRC " << (info.crc_ok ? "ok" : "MISMATCH") << ")";
    return what;
}

/* The build string at the tail of the factory image. */
juce::String buildString (const std::vector<uint8_t>& image)
{
    static const char tag[] = "GitVersion:";
    auto it = std::search (image.begin(), image.end(), tag, tag + sizeof (tag) - 1);
    if (it == image.end())
        return "<unknown>";
    juce::String s;
    for (it = std::next (it, (long) sizeof (tag) - 1); it != image.end() && *it >= 0x20 && *it < 0x7f && s.length() < 48; ++it)
        s += (juce::juce_wchar) *it;
    return s;
}

/* The factory eMMC region from a release file: the container parsed and
 * the region checked by shared/, the inflating done here. */
bool buildFactoryImage (const juce::File& release, std::vector<uint8_t>& image, juce::String& error)
{
    juce::MemoryBlock file;
    if (! release.loadFileAsData (file))
    {
        error = "cannot read the file";
        return false;
    }
    midi_control_hxf rel;
    const char* err = nullptr;
    if (! midi_control_hxf_parse ((const uint8_t*) file.getData(), (uint32_t) file.getSize(), &rel, &err))
    {
        error = err;
        return false;
    }

    /* The application section opens the main content and the bootloader
     * refuses an image of 4 MB or more, so the start of the payload is enough. */
    const size_t want = std::min<size_t> (rel.main_len, 4u * 1024u * 1024u);
    juce::MemoryInputStream compressed (static_cast<const char*> (file.getData()) + rel.data_offset, rel.data_len, false);
    juce::GZIPDecompressorInputStream inflater (&compressed, false, juce::GZIPDecompressorInputStream::zlibFormat);
    std::vector<uint8_t> content (want);
    size_t got = 0;
    while (got < want)
    {
        const int n = inflater.read (content.data() + got, (int) (want - got));
        if (n <= 0)
            break;
        got += (size_t) n;
    }
    if (got < want)
    {
        error = "the compressed payload ended early";
        return false;
    }

    image.assign (MIDI_CONTROL_EMMC_SAFE_SECTORS * MIDI_CONTROL_SECTOR_BYTES, 0);
    if (midi_control_hxf_factory_emmc (content.data(), (uint32_t) got, image.data(), (uint32_t) image.size(), &err) == 0)
    {
        error = err;
        return false;
    }
    return true;
}
#endif
} // namespace

FirmwarePanel::FirmwarePanel()
{
    chooseButton.onClick = [this] { chooseFirmware(); };
    refreshButton.onClick = [this] { refreshDetection(); };
    rebootButton.onClick = [this] { doRebootToDfu(); };
    loadButton.onClick = [this] { doDfuLoad(); };
    backupButton.onClick = [this] { doBackup(); };
    installButton.onClick = [this] { doInstall(); };
    restoreButton.onClick = [this] { doRestore(); };
    factoryButton.onClick = [this] { doFactoryRestore(); };
    readLogButton.onClick = [this] { doReadLog(); };
    openBackupsButton.onClick = [this]
    {
        backupsDirectory().createDirectory();
        backupsDirectory().revealToUser();
    };
    for (auto* b : { &chooseButton, &refreshButton, &readLogButton, &openBackupsButton })
        theme::setVariant (*b, "ghost");
    for (auto* b : { &installButton, &restoreButton, &factoryButton })
        theme::setVariant (*b, "danger");
    for (auto* b : { &chooseButton, &refreshButton, &rebootButton, &loadButton, &backupButton,
                     &installButton, &restoreButton, &factoryButton, &readLogButton, &openBackupsButton })
        addAndMakeVisible (b);

    logText.setMultiLine (true);
    logText.setReadOnly (true);
    logText.setCaretVisible (false);
    logText.setScrollbarsShown (true);
    logText.setFont (theme::mono (13.0f));
    logText.setIndents (12, 10);
    logText.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0a0c0f));
    logText.setColour (juce::TextEditor::textColourId, juce::Colour (0xffc4ccd6));
    logText.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    logText.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (logText);

    locateFirmware();

#if JUCE_WINDOWS
    log ("Firmware operations use WinUSB. The unit in Firmware Update Mode (the boot ROM, 1FC9:000C) binds to WinUSB through "
         "Line 6's driver package, installed with POD Go Edit; without it step 2 cannot see the unit.");
    log ("Steps 1-3 are undone by a power cycle. Steps 4 and 5 and the factory restore write the eMMC and are not.");
    refreshDetection();
    startTimer (2000);
#else
    detectionText = "USB detection is Windows-only";
    log ("Firmware operations are only implemented on Windows so far. Use tools/midi_control_usb.py.");
#endif
    updateButtons();
}

FirmwarePanel::~FirmwarePanel()
{
    stopTimer();
}

/* ---- layout ------------------------------------------------------------------ */

void FirmwarePanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);

    auto head = headerArea;
    auto titleArea = head.removeFromLeft (head.getWidth() / 2);
    g.setColour (theme::ink);
    g.setFont (theme::heading (22.0f));
    g.drawText ("Firmware", titleArea.removeFromTop (34), juce::Justification::bottomLeft, false);
    g.setColour (theme::inkDim);
    g.setFont (theme::ui (13.5f));
    g.drawText ("Put " MIDI_CONTROL_PRODUCT_NAME " on the unit, back the unit up, and put it back.", titleArea,
                juce::Justification::centredLeft, true);

    /* What is on the bus, as chips; the detail underneath. */
    auto chips = head.withTrimmedRight (96).removeFromTop (34).toFloat().withTrimmedTop (6.0f);
    auto chip = [&] (const juce::String& text, bool on)
    {
        const float w = theme::chipWidth (text, chips.getHeight(), true);
        theme::drawChip (g, chips.removeFromRight (w), text, on ? theme::ink : theme::inkFaint,
                         on ? theme::good : theme::ruleStrong);
        chips.removeFromRight (8.0f);
    };
    chip (MIDI_CONTROL_PRODUCT_NAME, midiControlDetected);
    chip ("FIRMWARE UPDATE MODE", dfuDetected);
    chip ("FACTORY", factoryDetected);
    g.setColour (busy ? theme::accent : theme::inkFaint);
    g.setFont (theme::ui (12.5f));
    g.drawText (busy ? juce::String ("working...") : detectionText, head.withTrimmedTop (34),
                juce::Justification::centredRight, true);

    auto card = [&] (juce::Rectangle<int> r, juce::Colour edge)
    {
        g.setColour (theme::surface);
        g.fillRoundedRectangle (r.toFloat(), 12.0f);
        g.setColour (edge);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 12.0f, 1.0f);
    };

    /* The image the steps load and install. */
    card (imageCard, theme::rule);
    auto img = imageCard.reduced (16, 10).withTrimmedRight (110);
    g.setColour (theme::inkFaint);
    g.setFont (theme::heading (12.0f));
    g.drawText ("FIRMWARE IMAGE", img.removeFromTop (18), juce::Justification::centredLeft, false);
    if (firmwareFile.existsAsFile())
    {
        g.setColour (theme::ink);
        g.setFont (theme::ui (15.0f, true));
        g.drawText (firmwareFile.getFileName() + "   " + juce::File::descriptionOfSizeInBytes (firmwareFile.getSize()),
                    img.removeFromTop (22), juce::Justification::centredLeft, true);
        g.setColour (theme::inkDim);
        g.setFont (theme::ui (12.5f));
        g.drawText (firmwareFile.getParentDirectory().getFullPathName(), img, juce::Justification::centredLeft, true);
    }
    else
    {
        g.setColour (theme::warn);
        g.setFont (theme::ui (15.0f, true));
        g.drawText ("Not found: choose midi-control.bin", img.removeFromTop (22), juce::Justification::centredLeft, true);
    }

    for (int i = 0; i < kStepCount; ++i)
    {
        const auto& step = kSteps[i];
        const bool ready = stepReady (i);
        const auto result = results[i];
        auto r = stepCards[i];
        card (r, ready && ! busy ? theme::ruleStrong : theme::rule);

        auto inner = r.reduced (16, 12).withTrimmedRight (132);
        const auto disc = inner.removeFromLeft (34).withSizeKeepingCentre (32, 32).toFloat();
        const auto discColour = result == Result::ok ? theme::good
                              : result == Result::failed ? theme::bad
                              : ready ? theme::accent : theme::ruleStrong;
        g.setColour (discColour.withAlpha (result == Result::none && ! ready ? 1.0f : 0.18f));
        g.fillEllipse (disc);
        g.setColour (discColour);
        g.drawEllipse (disc.reduced (0.75f), 1.5f);
        if (result == Result::ok)
        {
            juce::Path tick;
            tick.startNewSubPath (disc.getCentreX() - 7.0f, disc.getCentreY());
            tick.lineTo (disc.getCentreX() - 2.0f, disc.getCentreY() + 5.0f);
            tick.lineTo (disc.getCentreX() + 7.0f, disc.getCentreY() - 5.0f);
            g.strokePath (tick, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else if (i == kFactoryStep && result == Result::none)
        {
            /* Not a numbered step but the whole way back at once: a turn anticlockwise. */
            const auto c = disc.getCentre();
            const float rad = 7.5f, from = 0.9f, to = juce::MathConstants<float>::twoPi - 0.3f;
            juce::Path arc;
            arc.addCentredArc (c.x, c.y, rad, rad, 0.0f, from, to, true);
            g.setColour (ready ? theme::accent : theme::inkDim);
            g.strokePath (arc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            const juce::Point<float> radial (std::sin (from), -std::cos (from));
            const juce::Point<float> ahead (-std::cos (from), -std::sin (from));
            const auto base = c + radial * rad;
            juce::Path arrowHead;
            arrowHead.addTriangle (base + ahead * 4.5f, base - radial * 3.5f, base + radial * 3.5f);
            g.fillPath (arrowHead);
        }
        else
        {
            g.setColour (result == Result::failed ? theme::bad : ready ? theme::accent : theme::inkDim);
            g.setFont (theme::heading (16.0f));
            g.drawText (result == Result::failed ? juce::String ("!") : juce::String (i + 1), disc,
                        juce::Justification::centred, false);
        }
        inner.removeFromLeft (14);

        auto top = inner.removeFromTop (24);
        const auto badgeFont = theme::heading (11.5f);
        const float badgeWidth = theme::textWidth (badgeFont, step.badge) + 18.0f;
        const auto badgeArea = top.removeFromRight ((int) badgeWidth).toFloat().withSizeKeepingCentre (badgeWidth, 20.0f);
        const auto bc = badgeColour (step.kind);
        g.setColour (bc.withAlpha (0.13f));
        g.fillRoundedRectangle (badgeArea, 5.0f);
        g.setColour (bc);
        g.setFont (badgeFont);
        g.drawText (step.badge, badgeArea, juce::Justification::centred, false);

        g.setColour (theme::ink);
        g.setFont (theme::heading (17.0f));
        g.drawText (step.title, top, juce::Justification::centredLeft, true);

        g.setColour (theme::inkDim);
        g.setFont (theme::ui (13.0f));
        g.drawText (step.what, inner.removeFromTop (20), juce::Justification::centredLeft, true);

        auto needs = inner.removeFromTop (20);
        g.setColour (ready ? theme::good : theme::bad.withAlpha (0.8f));
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (needs.removeFromLeft (10).getCentre().toFloat()));
        g.setColour (theme::inkFaint);
        g.setFont (theme::ui (12.5f));
        g.drawText (juce::String ("Needs ") + step.needs, needs.withTrimmedLeft (4), juce::Justification::centredLeft, true);
    }

    card (logCard, theme::rule);
    g.setColour (theme::ink);
    g.setFont (theme::heading (16.0f));
    g.drawText ("Log", logCard.reduced (16, 0).removeFromTop (48), juce::Justification::centredLeft, false);
}

void FirmwarePanel::resized()
{
    auto r = getLocalBounds().reduced (16, 12);
    headerArea = r.removeFromTop (62);
    refreshButton.setBounds (headerArea.getRight() - 84, headerArea.getY() + 8, 84, 28);
    r.removeFromTop (10);

    auto left = r.removeFromLeft (juce::jlimit (460, 620, r.getWidth() / 2));
    r.removeFromLeft (14);
    logCard = r;

    imageCard = left.removeFromTop (74);
    chooseButton.setBounds (imageCard.getRight() - 112, imageCard.getCentreY() - 15, 96, 30);
    left.removeFromTop (10);

    const int gap = 8;
    const int stepHeight = juce::jlimit (72, 96, (left.getHeight() - (kStepCount - 1) * gap) / kStepCount);
    juce::TextButton* stepButtons[kStepCount] = { &rebootButton, &loadButton, &backupButton, &installButton, &restoreButton,
                                                  &factoryButton };
    for (int i = 0; i < kStepCount; ++i)
    {
        stepCards[i] = left.removeFromTop (stepHeight);
        left.removeFromTop (gap);
        stepButtons[i]->setBounds (stepCards[i].getRight() - 124, stepCards[i].getCentreY() - 17, 108, 34);
    }

    auto logInner = logCard.reduced (12, 0);
    auto logHead = logInner.removeFromTop (48).withSizeKeepingCentre (logInner.getWidth(), 30);
    openBackupsButton.setBounds (logHead.removeFromRight (132));
    logHead.removeFromRight (8);
    readLogButton.setBounds (logHead.removeFromRight (140));
    logText.setBounds (logInner.withTrimmedBottom (12));
}

/* ---- state -------------------------------------------------------------------- */

juce::File FirmwarePanel::backupsDirectory() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("midi-control")
               .getChildFile ("backups");
}

void FirmwarePanel::log (const juce::String& line)
{
    auto add = [this, line]
    {
        logText.moveCaretToEnd();
        logText.insertTextAtCaret (juce::Time::getCurrentTime().formatted ("%H:%M:%S  ") + line + "\n");
    };
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        add();
    else
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<FirmwarePanel> (this), add]
        {
            if (safe != nullptr)
                add();
        });
}

void FirmwarePanel::markStep (int step, bool ok)
{
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<FirmwarePanel> (this), step, ok]
    {
        if (safe != nullptr)
        {
            safe->results[step] = ok ? Result::ok : Result::failed;
            safe->repaint();
        }
    });
}

void FirmwarePanel::locateFirmware()
{
    const auto exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    juce::Array<juce::File> candidates;
    /* Shipped beside the executable from app/assets (app/CMakeLists.txt). */
    candidates.add (exeDir.getChildFile ("firmware").getChildFile ("midi-control.bin"));
    candidates.add (exeDir.getChildFile ("midi-control.bin"));
    /* A development tree: walk up looking for firmware/build. */
    auto dir = exeDir;
    for (int i = 0; i < 6 && dir.exists(); ++i)
    {
        candidates.add (dir.getChildFile ("firmware").getChildFile ("build").getChildFile ("midi-control.bin"));
        dir = dir.getParentDirectory();
    }
    for (auto& f : candidates)
        if (f.existsAsFile())
        {
            firmwareFile = f;
            break;
        }
    repaint();
}

void FirmwarePanel::chooseFirmware()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose the firmware image (midi-control.bin)",
                                                   firmwareFile.existsAsFile() ? firmwareFile.getParentDirectory()
                                                                               : juce::File(),
                                                   "*.bin");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f.existsAsFile())
        {
            firmwareFile = f;
            log ("firmware: " + f.getFullPathName());
            updateButtons();
            repaint();
        }
    });
}

bool FirmwarePanel::loadFirmware (std::vector<uint8_t>& out, juce::String& error) const
{
    juce::MemoryBlock mb;
    if (! firmwareFile.existsAsFile() || ! firmwareFile.loadFileAsData (mb))
    {
        error = "firmware file not readable";
        return false;
    }
    out.assign ((const uint8_t*) mb.getData(), (const uint8_t*) mb.getData() + mb.getSize());
    if (out.size() < 16 || out[0] != 0x1A || out[1] != 0x00)
    {
        error = "not an NXP boot image (expected a 1a 00 header)";
        return false;
    }
    return true;
}

void FirmwarePanel::timerCallback()
{
    if (! busy)
        refreshDetection();
}

void FirmwarePanel::refreshDetection()
{
#if JUCE_WINDOWS
    const auto d = MidiControlUsb::detect();
    const bool changed = d.factory != factoryDetected || d.dfu != dfuDetected || d.midiControl != midiControlDetected;
    factoryDetected = d.factory;
    dfuDetected = d.dfu;
    midiControlDetected = d.midiControl;
    detectionText = "On the bus: " + juce::String (d.describe());
    if (changed)
        log ("detected: " + juce::String (d.describe()));
#endif
    updateButtons();
    repaint (headerArea);
}

void FirmwarePanel::setBusy (bool b)
{
    busy = b;
    updateButtons();
    repaint();
}

bool FirmwarePanel::stepReady (int step) const
{
#if JUCE_WINDOWS
    const bool fw = firmwareFile.existsAsFile();
    switch (step)
    {
        case 0: return factoryDetected || midiControlDetected;
        case 1: return dfuDetected && fw;
        case 2: return midiControlDetected;
        case 3: return midiControlDetected && fw && backupVerified;
        case 4: return midiControlDetected;
        case kFactoryStep: return (midiControlDetected || dfuDetected) && fw;
        default: return false;
    }
#else
    juce::ignoreUnused (step);
    return false;
#endif
}

void FirmwarePanel::updateButtons()
{
#if JUCE_WINDOWS
    juce::TextButton* stepButtons[kStepCount] = { &rebootButton, &loadButton, &backupButton, &installButton, &restoreButton,
                                                  &factoryButton };
    for (int i = 0; i < kStepCount; ++i)
        stepButtons[i]->setEnabled (! busy && stepReady (i));
    readLogButton.setEnabled (! busy && midiControlDetected);
    installButton.setTooltip (backupVerified ? "" : "Take and verify a backup first (step 3)");
    factoryButton.setTooltip (factoryDetected && ! midiControlDetected && ! dfuDetected
                                  ? "The unit is already running its factory system"
                                  : "Choose the factory release file (.hxf) for the version this unit had");
#else
    for (auto* b : { &rebootButton, &loadButton, &backupButton, &installButton, &restoreButton, &factoryButton,
                     &readLogButton })
        b->setEnabled (false);
#endif
    repaint();
}

void FirmwarePanel::runInBackground (std::function<void()> work)
{
    setBusy (true);
    juce::Thread::launch ([safe = juce::Component::SafePointer<FirmwarePanel> (this), work]
    {
        work();
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
            {
                safe->setBusy (false);
                safe->refreshDetection();
            }
        });
    });
}

void FirmwarePanel::confirm (const juce::String& title, const juce::String& message, std::function<void()> onYes)
{
    auto options = juce::MessageBoxOptions()
                       .withIconType (juce::MessageBoxIconType::WarningIcon)
                       .withTitle (title)
                       .withMessage (message)
                       .withButton ("Continue")
                       .withButton ("Cancel")
                       .withAssociatedComponent (this);
    juce::AlertWindow::showAsync (options, [onYes] (int result)
    {
        /* The first button reports 1, the second 0. */
        if (result == 1)
            onYes();
    });
}

/* ---- the steps ---------------------------------------------------------------- */

void FirmwarePanel::doRebootToDfu()
{
#if JUCE_WINDOWS
    if (midiControlDetected)
    {
        confirm ("Put the unit into Firmware Update Mode?",
                 MIDI_CONTROL_PRODUCT_NAME " will hand the unit to the NXP boot ROM, which comes back in Firmware "
                 "Update Mode. Nothing is written; a power cycle returns the unit to whatever is on its eMMC.\n\n"
                 "If the unit is not in Firmware Update Mode within 20 seconds, power it off and hold PAGE > "
                 "while powering it on.",
                 [this]
        {
            runInBackground ([this]
            {
                log ("--- " MIDI_CONTROL_PRODUCT_NAME " into Firmware Update Mode ---");
                auto r = MidiControlUsb::rebootMidiControlToDfu ([this] (const std::string& s) { log (s); });
                log (juce::String (r.ok ? "ok: " : "FAILED: ") + r.message);
                markStep (0, r.ok);
            });
        });
        return;
    }

    confirm ("Put the unit into Firmware Update Mode?",
             "The POD Go will drop off the bus and come back as the NXP boot ROM in Firmware Update Mode. "
             "Nothing is written; a power cycle returns it to normal.\n\n"
             "Wait until the unit has fully booted (its normal screen, plus 20 seconds) before doing this.",
             [this]
    {
        runInBackground ([this]
        {
            log ("--- factory system into Firmware Update Mode ---");
            auto r = MidiControlUsb::rebootToDfu ([this] (const std::string& s) { log (s); });
            log (juce::String (r.ok ? "ok: " : "FAILED: ") + r.message);
            markStep (0, r.ok);
        });
    });
#endif
}

void FirmwarePanel::doDfuLoad()
{
#if JUCE_WINDOWS
    std::vector<uint8_t> image;
    juce::String err;
    if (! loadFirmware (image, err))
    {
        log ("cannot load firmware: " + err);
        return;
    }
    confirm ("Load the firmware into the unit's RAM?",
             "The boot ROM will run " + firmwareFile.getFileName() + " (" + juce::String ((int) image.size())
                 + " bytes) from RAM. Nothing is written; a power cycle restores whatever is installed.\n\n"
                 "Turn the unit's volume down first.",
             [this, image]
    {
        runInBackground ([this, image]
        {
            log ("--- load into RAM ---");
            auto r = MidiControlUsb::dfuLoad (image, [this] (const std::string& s) { log (s); });
            log (juce::String (r.ok ? "ok: " : "FAILED: ") + r.message);
            markStep (1, r.ok);
        });
    });
#endif
}

void FirmwarePanel::doBackup()
{
#if JUCE_WINDOWS
    runInBackground ([this]
    {
        log ("--- eMMC backup ---");
        MidiControlUsb::VendorSession s;
        std::string err;
        if (! s.open (err))
        {
            log (err);
            markStep (2, false);
            return;
        }
        std::vector<uint8_t> data;
        juce::File saved;
        const bool ok = backupEmmc (s, data, saved);
        if (ok)
            publishBackup (saved);
        markStep (2, ok);
    });
#endif
}

#if JUCE_WINDOWS
/* Sectors 0..1364 read twice, compared, saved into the backups folder and read
 * back from disk. Logs every failure; worker thread. */
bool FirmwarePanel::backupEmmc (MidiControlUsb::VendorSession& s, std::vector<uint8_t>& data, juce::File& saved)
{
    auto logger = [this] (const std::string& t) { log (t); };
    std::vector<uint8_t> again;
    log ("reading sectors 0..1364 (first pass)");
    if (! s.dumpEmmc (data, logger))
    {
        log ("FAILED: first pass");
        return false;
    }
    log ("reading again (second pass, to prove the read repeats)");
    if (! s.dumpEmmc (again, logger))
    {
        log ("FAILED: second pass");
        return false;
    }
    if (data != again)
    {
        log ("FAILED: the two reads differ - not saving. Try again; if it persists, the card is unreliable");
        return false;
    }
    if (data.size() != (size_t) MIDI_CONTROL_EMMC_SAFE_SECTORS * 512)
    {
        log ("FAILED: unexpected size " + juce::String ((int) data.size()));
        return false;
    }
    log ("the eMMC holds " + describeImage (data.data(), data.size()));

    auto dir = backupsDirectory();
    dir.createDirectory();
    saved = dir.getChildFile ("emmc-backup-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".bin");
    if (! saved.replaceWithData (data.data(), data.size()))
    {
        log ("FAILED: could not write " + saved.getFullPathName());
        return false;
    }
    juce::MemoryBlock back;
    if (! saved.loadFileAsData (back) || back.getSize() != data.size()
        || memcmp (back.getData(), data.data(), data.size()) != 0)
    {
        log ("FAILED: the saved file does not read back identically");
        return false;
    }
    log ("backup verified and saved: " + saved.getFullPathName());
    return true;
}
#endif

void FirmwarePanel::publishBackup (const juce::File& saved)
{
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<FirmwarePanel> (this), saved]
    {
        if (safe != nullptr)
        {
            safe->verifiedBackup = saved;
            safe->backupVerified = true;
            safe->updateButtons();
        }
    });
}

void FirmwarePanel::doInstall()
{
#if JUCE_WINDOWS
    std::vector<uint8_t> image;
    juce::String err;
    if (! loadFirmware (image, err))
    {
        log ("cannot load firmware: " + err);
        return;
    }
    if (! backupVerified)
    {
        log ("refusing to install without a verified backup from this session");
        return;
    }

    std::vector<uint8_t> emmc (image.size() + 2048 + 64);
    const char* buildErr = nullptr;
    const auto n = midi_control_boot_build_emmc (image.data(), (uint32_t) image.size(), 0x10000000u,
                                                 emmc.data(), (uint32_t) emmc.size(), &buildErr);
    if (n == 0)
    {
        log (juce::String ("cannot build the eMMC image: ") + (buildErr ? buildErr : "?"));
        return;
    }
    emmc.resize (n);

    midi_control_boot_info info;
    midi_control_boot_inspect (emmc.data(), n, &info);

    confirm ("Install the firmware to the eMMC?",
             "This writes " + juce::String ((int) n / 512) + " sectors at LBA 0 and CANNOT be undone by a power cycle. "
                 "The unit will boot " MIDI_CONTROL_PRODUCT_NAME " on every power-on until the backup is restored (step 5).\n\n"
                 "Entry " + juce::String::toHexString ((int) info.entry) + ", image CRC "
                 + (info.crc_ok ? "ok" : "BAD") + ".\nBackup: " + verifiedBackup.getFileName()
                 + "\n\nRecovery: hold PAGE > while powering on to reach Firmware Update Mode, load the firmware into RAM, then restore.",
             [this, emmc]
    {
        runInBackground ([this, emmc]
        {
            log ("--- install to eMMC ---");
            MidiControlUsb::VendorSession s;
            std::string err2;
            if (! s.open (err2))
            {
                log (err2);
                markStep (3, false);
                return;
            }
            auto text = s.writeEmmc (0, emmc, [this] (const std::string& t) { log (t); });
            log (text);
            const bool ok = text.find ("WRITTEN AND VERIFIED") != std::string::npos;
            if (ok)
                log ("installed. Power-cycle the unit: it should boot straight into " MIDI_CONTROL_PRODUCT_NAME ".");
            else
                log ("INSTALL DID NOT VERIFY - restore the backup (step 5) before powering off.");
            markStep (3, ok);
        });
    });
#endif
}

void FirmwarePanel::doRestore()
{
#if JUCE_WINDOWS
    backupsDirectory().createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Choose the backup to restore", backupsDirectory(), "*.bin");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (! f.existsAsFile())
            return;
        juce::MemoryBlock mb;
        if (! f.loadFileAsData (mb))
        {
            log ("cannot read " + f.getFullPathName());
            return;
        }
        std::vector<uint8_t> data ((const uint8_t*) mb.getData(), (const uint8_t*) mb.getData() + mb.getSize());
        if (data.size() % 512 || data.empty() || data.size() > (size_t) MIDI_CONTROL_EMMC_SAFE_SECTORS * 512)
        {
            log ("refusing: a restore file must be whole sectors, at most 1365 of them");
            return;
        }
        juce::String what = describeImage (data.data(), data.size());
        const int sectors = (int) data.size() / 512;
        if (sectors < (int) MIDI_CONTROL_EMMC_SAFE_SECTORS)
            what << ". NOTE: only " << sectors << " sectors - a full factory restore needs all 1365";

        confirm ("Restore this backup to the eMMC?",
                 "This writes " + juce::String (sectors) + " sectors at LBA 0 from " + f.getFileName()
                     + " and CANNOT be undone by a power cycle.\n\nThe file holds " + what + ".",
                 [this, data, f]
        {
            runInBackground ([this, data, f]
            {
                log ("--- restore " + f.getFileName() + " ---");
                MidiControlUsb::VendorSession s;
                std::string err;
                if (! s.open (err))
                {
                    log (err);
                    markStep (4, false);
                    return;
                }
                auto text = s.writeEmmc (0, data, [this] (const std::string& t) { log (t); });
                log (text);
                const bool ok = text.find ("WRITTEN AND VERIFIED") != std::string::npos;
                if (ok)
                    log ("restored. Power-cycle the unit.");
                else
                    log ("RESTORE DID NOT VERIFY - try again before powering off.");
                markStep (4, ok);
            });
        });
    });
#endif
}

void FirmwarePanel::doReadLog()
{
#if JUCE_WINDOWS
    runInBackground ([this]
    {
        log ("--- device log ---");
        MidiControlUsb::VendorSession s;
        std::string err;
        if (! s.open (err))
        {
            log (err);
            return;
        }
        auto text = s.readLog (true, 20.0);
        juce::StringArray lines;
        lines.addLines (juce::String (text));
        for (auto& l : lines)
            log ("  " + l);
        log ("--- end of device log (" + juce::String (lines.size()) + " lines) ---");
        log ("storage: " + juce::String (s.readResult (70, 1)).replace ("\n", " "));
    });
#endif
}

/* ---- the factory system, from its release file ------------------------------- */

void FirmwarePanel::doFactoryRestore()
{
#if JUCE_WINDOWS
    std::vector<uint8_t> ramImage;
    juce::String err;
    if (! loadFirmware (ramImage, err))
    {
        log ("cannot load firmware: " + err);
        return;
    }

    const auto start = factoryRelease.existsAsFile()
                           ? factoryRelease
                           : juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads");
    chooser = std::make_unique<juce::FileChooser> ("Choose the factory release (.hxf) for the version this unit had",
                                                   start, "*.hxf");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, ramImage] (const juce::FileChooser& fc)
    {
        const auto release = fc.getResult();
        if (! release.existsAsFile())
            return;

        std::vector<uint8_t> factory;
        juce::String why;
        if (! buildFactoryImage (release, factory, why))
        {
            log ("cannot use " + release.getFileName() + ": " + why);
            return;
        }
        factoryRelease = release;
        const auto build = buildString (factory);
        log ("factory image from " + release.getFileName() + ": " + build + ", "
             + juce::String ((int) factory.size() / 512) + " sectors, section and image CRCs ok");

        confirm ("Restore the factory system?",
                 "This restores the unit from " + release.getFileName() + " (" + build + "):\n\n"
                     "- into Firmware Update Mode, if " MIDI_CONTROL_PRODUCT_NAME " is running\n- "
                     + firmwareFile.getFileName() + " loaded into RAM\n"
                     "- a backup of the eMMC, into the backups folder\n"
                     "- a check that this release matches the system image the unit had\n"
                     "- sectors 0 to 1364 written, then read back and compared with the release\n\n"
                     "The write CANNOT be undone by a power cycle; the backup can be put back with step 5. "
                     "Turn the unit's volume down first.",
                 [this, ramImage, factory]
        {
            runInBackground ([this, ramImage, factory]
            {
                log ("--- restore factory firmware ---");
                markStep (kFactoryStep, runFactoryRestore (ramImage, factory));
            });
        });
    });
#endif
}

#if JUCE_WINDOWS
/* The whole way back, on the worker thread. Logs every stage and stops at the
 * first that does not hold; nothing is written until the release has been
 * matched against what the unit had. */
bool FirmwarePanel::runFactoryRestore (const std::vector<uint8_t>& ramImage, const std::vector<uint8_t>& factory)
{
    auto logger = [this] (const std::string& t) { log (t); };
    auto report = [this] (const MidiControlUsb::Result& r)
    {
        log (juce::String (r.ok ? "ok: " : "FAILED: ") + r.message);
        return r.ok;
    };

    auto d = MidiControlUsb::detect();
    if (! d.dfu && d.midiControl)
    {
        log (MIDI_CONTROL_PRODUCT_NAME " into Firmware Update Mode");
        if (! report (MidiControlUsb::rebootMidiControlToDfu (logger)))
            return false;
        d = MidiControlUsb::detect();
    }
    if (! d.dfu)
    {
        log ("FAILED: the unit is not in Firmware Update Mode. Power it off, hold PAGE > while powering it on, "
             "and start again");
        return false;
    }

    log ("loading the firmware into RAM");
    if (! report (MidiControlUsb::dfuLoad (ramImage, logger)))
        return false;

    MidiControlUsb::VendorSession s;
    std::string err;
    if (! s.open (err))
    {
        log ("FAILED: " + juce::String (err));
        return false;
    }

    std::vector<uint8_t> current;
    juce::File saved;
    if (! backupEmmc (s, current, saved))
        return false;
    publishBackup (saved);

    midi_control_residual res;
    midi_control_boot_residual_compare (current.data(), factory.data(), (uint32_t) factory.size(), &res);
    const auto sector = [] (uint32_t byte) { return juce::String (byte / MIDI_CONTROL_SECTOR_BYTES); };
    if (res.identical)
    {
        log ("the eMMC already holds exactly this factory image: nothing to write. Power-cycle the unit.");
        return true;
    }
    if (res.compared < 64u * MIDI_CONTROL_SECTOR_BYTES)
    {
        log ("REFUSED: too little of the factory image is left on the eMMC to tell whether this release is the one "
             "the unit had. Nothing was written.");
        return false;
    }
    if (res.differing != 0)
    {
        log ("REFUSED: the system image left on the eMMC is not this release - " + juce::String (res.differing)
             + " of " + juce::String (res.compared) + " bytes differ, the first in sector " + sector (res.first_difference)
             + ". Choose the .hxf for the version the unit had. Nothing was written.");
        return false;
    }
    log ("the release matches the system image left on the eMMC: sectors " + sector (res.from) + " to "
         + sector (res.to - 1u) + ", " + juce::String (res.compared) + " bytes, all identical");

    log ("writing the factory image: sectors 0 to 1364");
    const auto text = s.writeEmmc (0, factory, logger);
    log (text);
    if (text.find ("WRITTEN AND VERIFIED") == std::string::npos)
    {
        log ("FACTORY RESTORE DID NOT VERIFY - try again before powering off");
        return false;
    }

    log ("reading the eMMC back to compare with the release");
    std::vector<uint8_t> back;
    if (! s.dumpEmmc (back, logger) || back != factory)
    {
        log ("THE READ-BACK DOES NOT MATCH THE RELEASE - try again before powering off");
        return false;
    }
    log ("factory system restored: sectors 0 to 1364 match the release exactly. "
         "Power-cycle the unit to start the restored system.");
    return true;
}
#endif
