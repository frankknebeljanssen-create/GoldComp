#pragma once

#include <juce_dsp/juce_dsp.h>
#include "SlidingExtremum.h"
#include <cmath>
#include <vector>

/**
 * LookaheadLimiter — brickwall peak limiter.
 *
 * The gain target is the lowest gain required anywhere in the lookahead window,
 * so the ramp is already underway before the peak arrives; two one-pole stages
 * turn that step into a smooth ramp. For this to actually hold the ceiling the
 * smoothing has to settle *within* the lookahead window — the previous version
 * had 0.8 ms of smoothing behind a 0.73 ms window and overshot by 3 dB, which
 * left the un-oversampled safety clip downstream doing the real peak control.
 *
 * Design goal: invisible. If you can hear the limiter, it's too loud.
 */
class LookaheadLimiter
{
public:
    // Lookahead in seconds. Long enough that the smoothing below settles inside
    // it, short enough to stay cheap in latency. Defined in time, not samples,
    // so the limiter behaves identically at 44.1 and 192 kHz.
    static constexpr float LOOKAHEAD_SECONDS = 0.002f;

    LookaheadLimiter() = default;

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        sr = sampleRate;
        lookahead = std::max (8, (int) std::lround (sr * (double) LOOKAHEAD_SECONDS));

        int bufSize = lookahead + 1;
        delayL.assign ((size_t) bufSize, 0.0f);
        delayR.assign ((size_t) bufSize, 0.0f);
        gainMin.prepare (lookahead);

        // Release: two-stage program-dependent
        relFastCoeff = std::exp (-1.0f / (float (sr) * 0.020f));   // 20ms fast
        relSlowCoeff = std::exp (-1.0f / (float (sr) * 0.200f));   // 200ms slow

        // Gain smoothing, deliberately faster than the lookahead window so the
        // ramp completes before the peak lands. Still slow enough to keep
        // gain-modulation sidebands far down (~-59 dB at 20 kHz).
        smoothCoeff1 = std::exp (-1.0f / (float (sr) * 0.00015f)); // 0.15ms
        smoothCoeff2 = std::exp (-1.0f / (float (sr) * 0.00040f)); // 0.40ms

        // GR meter ballistics — 2.3ms, correct at every sample rate
        grMeterCoeff = std::exp (-1.0f / (float (sr) * 0.0023f));

        reset();
    }

    void reset()
    {
        std::fill (delayL.begin(), delayL.end(), 0.0f);
        std::fill (delayR.begin(), delayR.end(), 0.0f);
        gainMin.reset (0.0f);
        writePos = 0;
        envelope = 0.0f;
        currentGR = 0.0f;
        prevAppliedGain = 1.0f;
        smoothedGainDB = 0.0f;
        smoothedGainDB2 = 0.0f;
    }

    void process (float* bufferL, float* bufferR, int numSamples, float ceilingDB)
    {
        const float ceilingLin = std::pow (10.0f, ceilingDB / 20.0f);
        const int bufSize = lookahead + 1;

        for (int i = 0; i < numSamples; ++i)
        {
            delayL[(size_t) writePos] = bufferL[i];
            delayR[(size_t) writePos] = bufferR[i];

            float peak = std::max (std::abs (bufferL[i]), std::abs (bufferR[i]));

            // Instant attack, program-dependent release
            if (peak > envelope) {
                envelope = peak;
            } else {
                float grAmount = (envelope > ceilingLin && ceilingLin > 1.0e-10f)
                               ? (envelope - ceilingLin) / ceilingLin : 0.0f;
                float relBlend = juce::jlimit (0.0f, 1.0f, grAmount * 4.0f);
                float rc = relFastCoeff * (1.0f - relBlend) + relSlowCoeff * relBlend;
                envelope = rc * envelope + (1.0f - rc) * peak;
            }

            float targetGainDB = 0.0f;
            if (envelope > ceilingLin && envelope > 1.0e-10f)
                targetGainDB = 20.0f * std::log10 (ceilingLin / envelope);

            // Lowest gain required anywhere in the window, so reduction is
            // already in progress when the peak reaches the output.
            float windowMinDB = gainMin.pushAndGet (targetGainDB);

            smoothedGainDB  = smoothCoeff1 * smoothedGainDB  + (1.0f - smoothCoeff1) * windowMinDB;
            smoothedGainDB2 = smoothCoeff2 * smoothedGainDB2 + (1.0f - smoothCoeff2) * smoothedGainDB;

            static constexpr float dB2LinFactor = 0.11512925464970229f; // ln(10)/20
            float targetGainLin = std::exp (smoothedGainDB2 * dB2LinFactor);
            if (targetGainLin > 1.0f) targetGainLin = 1.0f;  // never boosts
            // Two-tap average on the control signal: one zero at Nyquist, which
            // keeps the gain multiply from generating its own aliasing.
            float appliedGain = 0.5f * (prevAppliedGain + targetGainLin);
            prevAppliedGain = targetGainLin;

            int readPos = (writePos - lookahead + bufSize) % bufSize;
            float dL = delayL[(size_t) readPos];
            float dR = delayR[(size_t) readPos];
            writePos = (writePos + 1) % bufSize;

            bufferL[i] = dL * appliedGain;
            bufferR[i] = dR * appliedGain;

            float grDB = (appliedGain > 1.0e-10f && appliedGain < 0.999f)
                       ? -20.0f * std::log10 (appliedGain) : 0.0f;
            currentGR = currentGR * grMeterCoeff + grDB * (1.0f - grMeterCoeff);
        }

        // envelope and both smoothing stages are pure IIR state. A single Inf
        // sample would drive smoothedGainDB to -Inf and leave the limiter
        // outputting permanent silence, so recover rather than latch.
        if (! (std::isfinite (envelope) && std::isfinite (smoothedGainDB)
            && std::isfinite (smoothedGainDB2) && std::isfinite (prevAppliedGain)
            && std::isfinite (currentGR)))
            reset();
    }

    float getGainReductionDB() const { return currentGR; }
    int   getLatencySamples()  const { return lookahead; }

private:
    double sr = 44100.0;
    int lookahead = 88;
    float relFastCoeff = 0.0f, relSlowCoeff = 0.0f;
    float smoothCoeff1 = 0.0f, smoothCoeff2 = 0.0f;
    float grMeterCoeff = 0.0f;
    float envelope = 0.0f;
    float prevAppliedGain = 1.0f;
    float smoothedGainDB = 0.0f, smoothedGainDB2 = 0.0f;
    float currentGR = 0.0f;

    SlidingMinimum gainMin;
    std::vector<float> delayL, delayR;
    int writePos = 0;
};
