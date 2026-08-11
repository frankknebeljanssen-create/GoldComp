// Behavioural regression test for AUTO. Drives the real processor headlessly
// (no editor, no audio device) through three cases that were all broken when
// AUTO tracked an offset from the knob instead of an absolute target:
//   1. switching AUTO on glides the knob into the sweet-spot band
//   2. dragging the knob away holds it there while the drag is live, then
//      releasing it glides back — visibly, not a snap
//   3. parking the knob in the red with AUTO off, then re-enabling AUTO,
//      recovers fully rather than getting stuck partway (the old +-18 offset
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

    // Mirrors what the editor actually does each UI frame: tell the audio
    // thread whether the user is dragging (every frame, unconditionally), and
    // when AUTO is on and the user is NOT dragging, push rideTargetComp into
    // the real "comp" parameter. ~3 blocks per tick approximates the editor's
    // 30Hz timer against a 512-sample block at 44.1kHz (~34.8ms).
    auto run = [&](int blocks, bool dragging) {
        for (int b = 0; b < blocks; ++b) {
            fill(buf.getWritePointer(0), buf.getWritePointer(1), BS);
            p.compKnobDragging.store(dragging);
            p.processBlock(buf, midi);
            if (p.rideMode.load() && ! dragging && b % 3 == 0)
                comp->setValueNotifyingHost(p.rideTargetComp.load() / 36.0f);
        }
    };

    std::printf("AUTO off, knob at 0. Warming up analysis...\n");
    run(200, false);
    std::printf("  sweet spot: %.1f .. %.1f   knob %.1f\n",
        p.sweetSpotLow.load(), p.sweetSpotHigh.load(), compVal());

    std::printf("\n[1] Switch AUTO on -> should glide into the sweet spot\n");
    p.rideMode.store(true);
    for (int step = 0; step < 5; ++step) { run(20, false);
        std::printf("  after %2d blocks: knob %5.2f  (band %.1f..%.1f)\n",
            (step+1)*20, compVal(), p.sweetSpotLow.load(), p.sweetSpotHigh.load()); }

    std::printf("\n[2] Drag to 34 (crush) with AUTO on: must hold while dragging, "
                "then glide back visibly on release\n");
    comp->setValueNotifyingHost(34.0f / 36.0f);
    p.compKnobDragging.store(true);
    run(15, true);   // hold the drag — should stay pinned near 34, not drift toward the sweet spot
    std::printf("  still dragging, after 15 blocks: knob %5.2f  (should still be ~34)\n", compVal());
    std::printf("  release -> glide back:\n");
    for (int step = 0; step < 15; ++step) { run(20, false);
        std::printf("  after %3d blocks (%.2fs): knob %5.2f\n",
            (step+1)*20, (step+1)*20*BS/SR, compVal()); }

    std::printf("\n[3] AUTO off, drag to 34, AUTO back on -> must recover, not stick\n");
    p.rideMode.store(false);
    run(30, false);
    comp->setValueNotifyingHost(34.0f / 36.0f);
    run(30, false);
    std::printf("  AUTO off, knob parked at %5.2f\n", compVal());
    p.rideMode.store(true);
    for (int step = 0; step < 20; ++step) { run(20, false);
        std::printf("  after %3d blocks (%.2fs): knob %5.2f  (band %.1f..%.1f)\n",
            (step+1)*20, (step+1)*20*BS/SR, compVal(), p.sweetSpotLow.load(), p.sweetSpotHigh.load()); }

    float lo = p.sweetSpotLow.load(), hi = p.sweetSpotHigh.load(), v = compVal();
    std::printf("\nRESULT: %s\n", (v >= lo - 0.5f && v <= hi + 0.5f)
        ? "knob ended inside the sweet spot band" : "FAILED - knob outside band");
    return 0;
}
