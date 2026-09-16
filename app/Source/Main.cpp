#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
#include "ui/Theme.h"

class MidiControlApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return snapshotDirectory (getCommandLineParameters()).isNotEmpty(); }

    void initialise (const juce::String& commandLine) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        /* --snapshot <dir> [--scale n]: draw every view off screen, against the
         * emulator alone, into <dir> as PNGs at n times the size, then quit.
         * For checking the look and for the docs' screenshots. No window, no
         * MIDI port. */
        const auto dir = snapshotDirectory (commandLine);
        if (dir.isNotEmpty())
        {
            offscreen = std::make_unique<MainComponent> (true);
            offscreen->stageSnapshots();
            const float scale = snapshotScale (commandLine);
            juce::Timer::callAfterDelay (2500, [this, dir, scale]
            {
                offscreen->saveSnapshots (juce::File (dir), scale);
                quit();
            });
            return;
        }

        window = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        window = nullptr;
        offscreen = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override { quit(); }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, theme::window, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (1100, 720, 5000, 3200);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    static juce::String snapshotDirectory (const juce::String& commandLine)
    {
        juce::StringArray args;
        args.addTokens (commandLine, true);
        const int i = args.indexOf ("--snapshot");
        return i >= 0 && i + 1 < args.size() ? args[i + 1].unquoted() : juce::String();
    }

    static float snapshotScale (const juce::String& commandLine)
    {
        juce::StringArray args;
        args.addTokens (commandLine, true);
        const int i = args.indexOf ("--scale");
        return i >= 0 && i + 1 < args.size() ? juce::jlimit (1.0f, 4.0f, args[i + 1].getFloatValue()) : 1.0f;
    }

    /* Declared before the components, so it outlives every one that uses it. */
    AppLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<MainComponent> offscreen;
};

START_JUCE_APPLICATION (MidiControlApplication)
