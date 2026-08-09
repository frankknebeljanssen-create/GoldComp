// Renders the real editor against synthetic audio and saves a PNG, so a
// README screenshot can show the plugin actually working (meters moving, GR
// arc lit, sweet-spot glow visible) without relying on a DAW, a physical mic,
// or hand-drawn mockups. Runs the real SmartCompProcessor/SmartCompEditor
// through a genuine JUCE message loop and timer, exactly like a host would,
// just fed a deterministic signal instead of live audio.
//
// Build:  cmake --build <builddir> --target ui_shot
// Run:    open <builddir>/ui_shot_artefacts/Release/ui_shot.app --args /tmp/out.png

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_graphics/juce_graphics.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlockSize = 512;

// Deterministic "vocal-like" generator: a repeating word/pause cadence with an
// emphasized syllable every third word, so both the auto-threshold's slow
// average and the fast meters have something realistic to track.
struct VocalSignal
{
    int64_t sampleIndex = 0;

    void fill(float* left, float* right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) sampleIndex / kSampleRate;
            const double wordPeriod = 0.55;
            const double wordT = std::fmod(t, wordPeriod);

            double env;
            if (wordT < 0.30) {
                const double u = wordT / 0.30;
                env = std::max(0.5 - 0.5 * std::cos(u * juce::MathConstants<double>::twoPi), 0.15);
            } else {
                env = 0.10; // breath / room tone between words
            }

            const int wordIndex = (int) (t / wordPeriod);
            // A gentler word-to-word dynamic range than the first pass at this
            // (0.34 vs 0.11, a 10 dB jump) — that swing sent instantaneous GR
            // from 0 dB to 8 dB every third word, and a screenshot taken at
            // the wrong instant showed a spike, not a typical reading.
            const double amp = (wordIndex % 3 == 0) ? 0.20f : 0.13f; // ~-14 dBFS vs ~-18 dBFS

            const double f0 = 145.0; // pitch, roughly a male vocal fundamental
            const double s = amp * env * (
                  0.60 * std::sin(juce::MathConstants<double>::twoPi * f0 * t)
                + 0.25 * std::sin(juce::MathConstants<double>::twoPi * f0 * 2.0 * t)
                + 0.15 * std::sin(juce::MathConstants<double>::twoPi * f0 * 3.3 * t));

            left[i] = right[i] = (float) s;
            ++sampleIndex;
        }
    }
};

} // namespace

class ShotApp : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "ui_shot"; }
    const juce::String getApplicationVersion() override { return "1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override
    {
        auto args = juce::StringArray::fromTokens(commandLine, true);
        if (! args.isEmpty())
            outPath = args[0].unquoted();

        processor = std::make_unique<SmartCompProcessor>();
        processor->prepareToPlay(kSampleRate, kBlockSize);

        // Compression around the middle of the range and Mix fully wet, so the
        // sweet-spot arc and GR meter both have something to show. The exact
        // knob position matters less than landing inside whatever range the
        // signal analysis converges on — see timerCallback.
        processor->apvts.getParameter("comp")->setValueNotifyingHost(18.0f / 36.0f);
        processor->apvts.getParameter("mix")->setValueNotifyingHost(1.0f);

        // AUTO on for the screenshot: it is the plugin's headline feature, and
        // the button state is only meaningful if it is shown engaged.
        processor->rideMode.store(true);

        editor.reset(processor->createEditor());
        editor->setVisible(true);

        window = std::make_unique<juce::DocumentWindow>(
            "ui_shot", juce::Colours::black, juce::DocumentWindow::allButtons);
        window->setUsingNativeTitleBar(true);
        window->setContentNonOwned(editor.get(), true);
        window->setTopLeftPosition(80, 80);
        window->setVisible(true);

        startTimerHz(30);
    }

    void shutdown() override
    {
        stopTimer();
        window = nullptr;
        editor = nullptr;
        processor = nullptr;
    }

    void systemRequestedQuit() override { quit(); }

private:
    void timerCallback() override
    {
        // Several audio blocks per UI tick, so the processor always has fresh
        // atomics by the time the editor's own 60Hz timer reads them.
        for (int b = 0; b < 4; ++b)
        {
            juce::AudioBuffer<float> buf(2, kBlockSize);
            signal.fill(buf.getWritePointer(0), buf.getWritePointer(1), kBlockSize);
            juce::MidiBuffer midi;
            processor->processBlock(buf, midi);
        }

        // Snap the knob into the middle of the sweet spot once it has
        // converged, so the glow (which needs the knob to sit inside the
        // range, not just the range to exist) has time to fade in before the
        // capture. Sweet-spot smoothing settles noticeably inside ~3s of
        // audio-time; this checks after that and adjusts once.
        if (! snapped && ticks == snapTick)
        {
            const float low = processor->sweetSpotLow.load();
            const float high = processor->sweetSpotHigh.load();
            const float mid = juce::jlimit(0.0f, 36.0f, (low + high) * 0.5f);
            processor->apvts.getParameter("comp")->setValueNotifyingHost(mid / 36.0f);
            snapped = true;
        }


        if (++ticks == captureTick)
        {
            auto bounds = editor->getLocalBounds();
            auto image = editor->createComponentSnapshot(bounds, false, 2.0f);
            juce::PNGImageFormat png;
            juce::File outFile(outPath);
            outFile.deleteFile();
            if (auto os = std::unique_ptr<juce::FileOutputStream>(outFile.createOutputStream()))
                png.writeImageToStream(image, *os);
            quit();
        }
    }

    std::unique_ptr<SmartCompProcessor> processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::unique_ptr<juce::DocumentWindow> window;
    VocalSignal signal;
    int ticks = 0;
    bool snapped = false;
    // 30 ticks/sec * 4 blocks * 512 samples / 44100 ~= 0.14s of audio-time per
    // tick. snapTick lands after ~3.5s of audio-time (enough for the sweet-spot
    // learn phase to settle), captureTick during the decay of an emphasized
    // word — verified by logging to land around 4.5 dB of reduction, i.e. a
    // representative reading inside the 3-5 dB sweet-spot band rather than an
    // instantaneous peak.
    static constexpr int snapTick = 175;
    static constexpr int captureTick = 218;
    juce::String outPath { "/tmp/smartcomp_ui_shot.png" };
};

START_JUCE_APPLICATION(ShotApp)
