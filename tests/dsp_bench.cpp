// Measurement harness for the DSP classes. Runs the real headers, not a
// re-implementation, so before/after numbers can be trusted.
//
// Build:  cmake --build <builddir> --target dsp_bench
// Run:    <builddir>/dsp_bench_artefacts/dsp_bench

#include "../Source/DSP/RVoxCompressor.h"
#include "../Source/DSP/LookaheadLimiter.h"

#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <utility>

namespace {

constexpr double SR = 44100.0;

double dB (double lin) { return 20.0 * std::log10 (std::max (lin, 1.0e-12)); }
double lin (double db)  { return std::pow (10.0, db / 20.0); }

//==============================================================================
// Does the limiter hold its ceiling? It is the last thing before the safety
// clip, so anything it lets through is handled by a static waveshaper instead.
void benchLimiter()
{
    constexpr float ceilingDB = -0.3f;
    const double ceiling = lin (ceilingDB);
    const int N = 8000, jump = 500;

    std::vector<float> L (N), R (N);
    for (int n = 0; n < N; ++n) {
        double amp = (n < jump) ? lin (-12.0) : lin (6.0);
        L[(size_t) n] = R[(size_t) n] = (float) (amp * std::sin (2.0 * M_PI * 1000.0 * n / SR));
    }

    LookaheadLimiter lim;
    lim.prepare (SR, 512);
    for (int off = 0; off < N; off += 512) {
        int nn = std::min (512, N - off);
        lim.process (L.data() + off, R.data() + off, nn, ceilingDB);
    }

    double peak = 0.0;
    int over = 0, lastOver = -1, tailOver = 0;
    for (int n = 0; n < N; ++n) {
        double a = std::abs ((double) L[(size_t) n]);
        peak = std::max (peak, a);
        if (a > ceiling * 1.0005) {
            ++over;
            lastOver = n;
            if (n > jump + (int) (0.045 * SR)) ++tailOver;
        }
    }

    std::printf ("LIMITER  ceiling %.1f dBFS, 1 kHz sine jumping to +6 dBFS\n", ceilingDB);
    std::printf ("  true peak out        %+7.2f dBFS  (overshoot %+.2f dB)\n",
                 dB (peak), dB (peak) - ceilingDB);
    std::printf ("  samples over ceiling %7d\n", over);
    std::printf ("  last one at          %7d  (%+d rel. to jump)\n",
                 lastOver, lastOver < 0 ? 0 : lastOver - jump);
    std::printf ("  still over after 45ms%7d   <- must be 0\n\n", tailOver);
}

//==============================================================================
// Do transients escape the compressor? Prepared at the host rate but fed the
// 2x oversampled block, exactly as PluginProcessor does it.
void benchCompressorTransient()
{
    const int  osN     = 16000;          // samples at the 2x rate
    const int  burstAt = 8000;
    const int  burstLen = 400;
    const float compDB = -29.0f;         // knob fully up (internal range)

    std::vector<float> L ((size_t) osN), R ((size_t) osN);
    for (int n = 0; n < osN; ++n) {
        double amp = (n >= burstAt && n < burstAt + burstLen) ? lin (-4.0) : lin (-20.0);
        L[(size_t) n] = R[(size_t) n] = (float) (amp * std::sin (2.0 * M_PI * 1000.0 * n / (SR * 2.0)));
    }

    RVoxCompressor comp;
    comp.prepare (SR, 512);              // note: base rate, as in PluginProcessor
    comp.userKneeWidth = 6.0f;
    comp.maxGainReductionDB = 36.0f;
    comp.setAttackTime (0.0001f);
    for (int off = 0; off < osN; off += 1024) {
        int nn = std::min (1024, osN - off);
        comp.process (L.data() + off, R.data() + off, nn, compDB, -80.0f, 0);
    }

    // The compressor delays by LOOKAHEAD_SAMPLES, so the burst leaves the
    // output that much later.
    const int outStart = burstAt + RVoxCompressor::LOOKAHEAD_SAMPLES;
    auto peakIn = [&] (int a, int b) {
        double p = 0.0;
        for (int n = a; n < b && n < osN; ++n) p = std::max (p, std::abs ((double) L[(size_t) n]));
        return p;
    };

    double settled  = peakIn (outStart - 2000, outStart - 100);   // quiet part, settled
    double atArrival = peakIn (outStart, outStart + 60);          // first ~0.7ms of the burst
    double burstPeak = peakIn (outStart, outStart + burstLen);
    double burstEnd  = peakIn (outStart + burstLen - 100, outStart + burstLen);

    std::printf ("COMPRESSOR  knob max, quiet -20 dBFS with a +16 dB burst\n");
    std::printf ("  settled output before burst %+7.2f dBFS\n", dB (settled));
    std::printf ("  peak in first 0.7ms of burst%+7.2f dBFS\n", dB (atArrival));
    std::printf ("  peak over whole burst       %+7.2f dBFS\n", dB (burstPeak));
    std::printf ("  output at end of burst      %+7.2f dBFS\n", dB (burstEnd));
    std::printf ("  transient escape            %+7.2f dB   <- attack overshoot\n",
                 dB (burstPeak) - dB (burstEnd));
    std::printf ("  GR reported                 %+7.2f dB\n\n", comp.getGainReductionDB());
}

//==============================================================================
// Effective time constants. prepare() gets the host rate while process() runs
// on the 2x block, so everything is nominally half its labelled value. This
// measures the release actually delivered.
void benchCompressorTiming()
{
    const int osN = 40000;
    std::vector<float> L ((size_t) osN), R ((size_t) osN);
    // 200ms loud at the 2x rate, then silence, so we can watch GR recover
    const int loudLen = (int) (0.200 * SR * 2.0);
    for (int n = 0; n < osN; ++n) {
        double amp = (n < loudLen) ? lin (-3.0) : 0.0;
        L[(size_t) n] = R[(size_t) n] = (float) (amp * std::sin (2.0 * M_PI * 1000.0 * n / (SR * 2.0)));
    }

    RVoxCompressor comp;
    comp.prepare (SR, 512);
    comp.userKneeWidth = 6.0f;
    comp.setAttackTime (0.0001f);

    // Sample GR after every block. Blocks never land exactly on loudLen, so
    // record the trace and analyse it afterwards rather than testing for an
    // exact boundary hit.
    constexpr int step = 64;
    std::vector<std::pair<int, double>> trace;
    for (int off = 0; off < osN; off += step) {
        int nn = std::min (step, osN - off);
        comp.process (L.data() + off, R.data() + off, nn, -29.0f, -80.0f, 0);
        trace.emplace_back (off + nn, (double) comp.getGainReductionDB());
    }

    double grAtEnd = 0.0;
    for (auto& p : trace)
        if (p.first <= loudLen) grAtEnd = p.second;

    int recoverSamples = -1;
    for (auto& p : trace)
        if (p.first > loudLen && recoverSamples < 0 && grAtEnd > 1.0
            && p.second < grAtEnd * 0.368)
            recoverSamples = p.first - loudLen;

    std::printf ("TIMING  GR at end of a 200ms loud passage %.2f dB\n", grAtEnd);
    if (recoverSamples > 0)
        std::printf ("  release to 1/e took %.1f ms of real time  (label says 60 ms)\n\n",
                     recoverSamples / (SR * 2.0) * 1000.0);
    else
        std::printf ("  release to 1/e not reached inside %.0f ms\n\n",
                     (osN - loudLen) / (SR * 2.0) * 1000.0);
}

} // namespace

int main()
{
    std::printf ("\n=== GoldComp DSP bench @ %.0f Hz ===\n\n", SR);
    benchLimiter();
    benchCompressorTransient();
    benchCompressorTiming();
    return 0;
}
