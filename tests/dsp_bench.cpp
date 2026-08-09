// Measurement harness for the DSP classes. Runs the real headers, not a
// re-implementation, so before/after numbers can be trusted.
//
// Build:  cmake --build <builddir> --target dsp_bench
// Run:    <builddir>/dsp_bench_artefacts/dsp_bench

#include "../Source/DSP/RVoxCompressor.h"
#include "../Source/DSP/LookaheadLimiter.h"
#include "../Source/DSP/SlidingExtremum.h"

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
    const float compDB = -36.0f;         // knob fully up

    std::vector<float> L ((size_t) osN), R ((size_t) osN);
    for (int n = 0; n < osN; ++n) {
        double amp = (n >= burstAt && n < burstAt + burstLen) ? lin (-4.0) : lin (-20.0);
        L[(size_t) n] = R[(size_t) n] = (float) (amp * std::sin (2.0 * M_PI * 1000.0 * n / (SR * 2.0)));
    }

    RVoxCompressor comp;
    comp.prepare (SR * 2.0, 1024);       // 2x rate, as PluginProcessor does
    comp.userKneeWidth = 6.0f;
    comp.maxGainReductionDB = 36.0f;
    comp.setAttackTime (0.0001f);
    for (int off = 0; off < osN; off += 1024) {
        int nn = std::min (1024, osN - off);
        comp.process (L.data() + off, R.data() + off, nn, compDB, -80.0f);
    }

    // The compressor delays by its lookahead, so the burst leaves the output
    // that much later.
    const int outStart = burstAt + comp.getLatencySamples();
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
    comp.prepare (SR * 2.0, 1024);
    comp.userKneeWidth = 6.0f;
    comp.setAttackTime (0.0001f);

    // Sample GR after every block. Blocks never land exactly on loudLen, so
    // record the trace and analyse it afterwards rather than testing for an
    // exact boundary hit.
    constexpr int step = 64;
    std::vector<std::pair<int, double>> trace;
    for (int off = 0; off < osN; off += step) {
        int nn = std::min (step, osN - off);
        comp.process (L.data() + off, R.data() + off, nn, -36.0f, -80.0f);
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

//==============================================================================
// Gain staging: how hard is the limiter working at each knob position?
//
// Makeup used to be derived from the knob rather than from the reduction the
// compressor actually delivered, which pushed the limiter into continuous
// double-digit reduction from Comp 12 up. This replicates the real chain —
// compressor at 2x, makeup, limiter at base rate — for both makeup laws.
void benchGainStaging()
{
    const int   blockSize = 512;
    const int   blocks    = 220;                 // ~2.5 s, long enough for the
    const float ceilingDB = -0.3f;               // 500 ms makeup average to settle
    const double srcLevels[] = { -30.0, -15.0 };  // quiet and hot source
    const int   knobs[]   = { 4, 8, 12, 20, 28, 36 };

    std::printf ("GAIN STAGING  ceiling %.1f dBFS. Knob position should mean the same\n", ceilingDB);
    std::printf ("              amount of compression at both source levels.\n");
    std::printf ("  %-5s  %-24s  %-24s\n", "knob", "source -30 dBFS", "source -15 dBFS");
    std::printf ("  %-5s  %-8s %-7s %-7s  %-8s %-7s %-7s\n",
                 "", "compGR", "makeup", "limGR", "compGR", "makeup", "limGR");

    for (int knob : knobs)
    {
        double results[2][3];
        for (int law = 0; law < 2; ++law)
        {
            const double srcDB = srcLevels[law];
            RVoxCompressor comp;
            LookaheadLimiter lim;
            comp.prepare (SR * 2.0, blockSize * 2);
            comp.userKneeWidth = 6.0f;
            comp.maxGainReductionDB = 36.0f;
            comp.setAttackTime (0.0001f);
            lim.prepare (SR, blockSize);

            const float compDB = -(float) knob;   // knob maps directly now
            double makeupGRAvg = 0.0, makeupSum = 0.0, limGRSum = 0.0, compGRSum = 0.0;
            int counted = 0;

            std::vector<float> osL ((size_t) blockSize * 2), osR ((size_t) blockSize * 2);
            std::vector<float> bL ((size_t) blockSize), bR ((size_t) blockSize);

            for (int b = 0; b < blocks; ++b)
            {
                // syllable-like envelope so crest factor is realistic
                for (int i = 0; i < blockSize * 2; ++i) {
                    double t = (b * blockSize * 2 + i) / (SR * 2.0);
                    double env = 0.35 + 0.65 * std::pow (std::abs (std::sin (2.0 * M_PI * 2.5 * t)), 2.0);
                    double s = std::sin (2.0 * M_PI * 220.0 * t) * 0.7
                             + std::sin (2.0 * M_PI * 1400.0 * t) * 0.3;
                    osL[(size_t) i] = osR[(size_t) i] = (float) (lin (srcDB) * env * s);
                }

                comp.process (osL.data(), osR.data(), blockSize * 2, compDB, -80.0f);

                // naive decimate — good enough for level bookkeeping
                for (int i = 0; i < blockSize; ++i) {
                    bL[(size_t) i] = osL[(size_t) (i * 2)];
                    bR[(size_t) i] = osR[(size_t) (i * 2)];
                }

                double sm = std::exp (-(double) blockSize / (SR * 0.500));
                makeupGRAvg = makeupGRAvg * sm + comp.getGainReductionDB() * (1.0 - sm);
                float makeupDB = (float) std::max (0.0, std::min (24.0, makeupGRAvg));
                float mk = std::pow (10.0f, makeupDB / 20.0f);
                for (int i = 0; i < blockSize; ++i) { bL[(size_t) i] *= mk; bR[(size_t) i] *= mk; }

                lim.process (bL.data(), bR.data(), blockSize, ceilingDB);

                if (b > blocks / 2) {                 // settled half only
                    makeupSum += makeupDB;
                    limGRSum  += lim.getGainReductionDB();
                    compGRSum += comp.getGainReductionDB();
                    ++counted;
                }
            }
            results[law][0] = compGRSum / counted;
            results[law][1] = makeupSum / counted;
            results[law][2] = limGRSum / counted;
        }
        std::printf ("  %-5d  %6.2f  %+6.2f  %5.2f   %6.2f  %+6.2f  %5.2f\n",
                     knob, results[0][0], results[0][1], results[0][2],
                           results[1][0], results[1][1], results[1][2]);
    }
    std::printf ("  All in dB. compGR should track the knob and match across levels.\n\n");
}

//==============================================================================
// The sliding minimum replaces the per-sample window scan in both lookahead
// stages, so it has to agree with brute force on every sample or it will
// introduce gain errors that are very hard to hear out.
bool testSlidingMinimum()
{
    const int windows[] = { 1, 2, 7, 32, 88, 129 };
    int failures = 0, checked = 0;

    for (int w : windows)
    {
        SlidingMinimum sm;
        sm.prepare (w);
        sm.reset (0.0f);

        std::vector<float> history;
        // deterministic pseudo-random walk with plateaus and spikes, since
        // equal neighbouring values are what break naive deque code
        unsigned state = 12345u;
        auto next = [&] {
            state = state * 1103515245u + 12345u;
            int r = (int) ((state >> 16) & 0x7fff);
            if (r % 11 == 0) return -40.0f;              // spike
            if (r % 5 == 0)  return 0.0f;                // plateau
            return -(float) (r % 25);
        };

        for (int n = 0; n < 4000; ++n)
        {
            float v = next();
            float got = sm.pushAndGet (v);
            history.push_back (v);

            // brute force over the same window, padded with the reset fill
            float want = v;
            for (int k = 0; k < w; ++k)
            {
                int idx = (int) history.size() - 1 - k;
                float hv = idx >= 0 ? history[(size_t) idx] : 0.0f;
                want = std::min (want, hv);
            }
            ++checked;
            if (std::abs (got - want) > 1.0e-6f)
            {
                if (failures < 3)
                    std::printf ("  MISMATCH w=%d n=%d got %.2f want %.2f\n", w, n, got, want);
                ++failures;
            }
        }
    }

    std::printf ("SLIDING MINIMUM  %d samples checked across %zu window sizes: %s\n\n",
                 checked, sizeof (windows) / sizeof (windows[0]),
                 failures == 0 ? "all match brute force" : "FAILURES");
    return failures == 0;
}

} // namespace

int main()
{
    std::printf ("\n=== SmartComp DSP bench @ %.0f Hz ===\n\n", SR);
    if (! testSlidingMinimum())
        return 1;
    benchGainStaging();
    benchLimiter();
    benchCompressorTransient();
    benchCompressorTiming();
    return 0;
}
