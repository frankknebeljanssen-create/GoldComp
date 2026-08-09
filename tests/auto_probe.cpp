// Behavioural regression test for AUTO. Drives the real processor headlessly
// (no editor, no audio device) through three cases that were all broken when
// AUTO tracked an offset from the knob instead of an absolute target:
//   1. switching AUTO on glides the knob into the sweet-spot band
//   2. dragging the knob away while AUTO stays on springs it back
//   3. parking the knob in the red with AUTO off, then re-enabling AUTO,
//      recovers fully rather than getting stuck partway (the old ±18 offset
//      clamp made this mathematically impossible from a knob far enough out)
//
// Build:  cmake --build <builddir> --target auto_probe
// Run:    <builddir>/auto_probe_artefacts/Release/auto_probe

#include "../Source/PluginProcessor.h"
#include <cstdio>
#include <cmath>

static constexpr double SR = 44100.0;
static constexpr int BS = 512;
static long long idx = 0;

static void fill(float* l, float* r, int n) {
    for (int i = 0; i < n; ++i) {
        double t = (double)idx / SR;
        double wt = std::fmod(t, 0.55);
        double env = (wt < 0.30) ? std::max(0.5 - 0.5*std::cos((wt/0.30)*6.28318530718), 0.15) : 0.10;
        int wi = (int)(t / 0.55);
        double amp = (wi % 3 == 0) ? 0.20 : 0.13;
        double s = amp*env*(0.60*std::sin(6.28318530718*145.0*t)
                          + 0.25*std::sin(6.28318530718*290.0*t)
                          + 0.15*std::sin(6.28318530718*478.5*t));
        l[i] = r[i] = (float)s; ++idx;
    }
}

int main() {
    SmartCompProcessor p;
    p.prepareToPlay(SR, BS);
    auto* comp = p.apvts.getParameter("comp");
    auto compVal = [&]{ return p.apvts.getRawParameterValue("comp")->load(); };

    juce::AudioBuffer<float> buf(2, BS);
    juce::MidiBuffer midi;
    auto run = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) {
            fill(buf.getWritePointer(0), buf.getWritePointer(1), BS);
            p.processBlock(buf, midi);
            // emulate the editor's 30Hz pull: every ~3 blocks
            if (p.rideMode.load() && b % 3 == 0)
                comp->setValueNotifyingHost(p.rideTargetComp.load() / 36.0f);
        }
    };

    std::printf("AUTO off, knob at 0. Warming up analysis...\n");
    run(200);
    std::printf("  sweet spot: %.1f .. %.1f   knob %.1f\n",
        p.sweetSpotLow.load(), p.sweetSpotHigh.load(), compVal());

    std::printf("\n[1] Switch AUTO on -> should glide into the sweet spot\n");
    p.rideMode.store(true);
    for (int step = 0; step < 5; ++step) { run(20); 
        std::printf("  after %2d blocks: knob %5.2f  (band %.1f..%.1f)\n",
            (step+1)*20, compVal(), p.sweetSpotLow.load(), p.sweetSpotHigh.load()); }

    std::printf("\n[2] Drag knob to 34 (crush) with AUTO still on -> should spring back\n");
    comp->setValueNotifyingHost(34.0f / 36.0f);
    std::printf("  immediately after drag: knob %5.2f\n", compVal());
    for (int step = 0; step < 5; ++step) { run(20);
        std::printf("  after %2d blocks: knob %5.2f\n", (step+1)*20, compVal()); }

    std::printf("\n[3] AUTO off, drag to 34, AUTO back on -> must recover, not stick\n");
    p.rideMode.store(false);
    run(30);
    comp->setValueNotifyingHost(34.0f / 36.0f);
    run(30);
    std::printf("  AUTO off, knob parked at %5.2f\n", compVal());
    p.rideMode.store(true);
    for (int step = 0; step < 6; ++step) { run(20);
        std::printf("  after %2d blocks: knob %5.2f  (band %.1f..%.1f)\n",
            (step+1)*20, compVal(), p.sweetSpotLow.load(), p.sweetSpotHigh.load()); }

    float lo = p.sweetSpotLow.load(), hi = p.sweetSpotHigh.load(), v = compVal();
    std::printf("\nRESULT: %s\n", (v >= lo - 0.5f && v <= hi + 0.5f)
        ? "knob ended inside the sweet spot band" : "FAILED - knob outside band");
    return 0;
}
