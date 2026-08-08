#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>

/**
 * LookaheadLimiter — Transparent brickwall peak limiter.
 *
 * Uses cosine-interpolated gain ramp across the lookahead window
 * for artifact-free limiting. Program-dependent release prevents
 * pumping on sustained material while recovering quickly from transients.
 *
 * Design goal: invisible. If you can hear the limiter, it's too loud.
 */
class LookaheadLimiter
{
public:
    static constexpr int LIMITER_LOOKAHEAD = 32;

    LookaheadLimiter() = default;

    void prepare(double sampleRate, int /*maxBlockSize*/)
    {
        sr = sampleRate;
        int bufSize = LIMITER_LOOKAHEAD + 1;
        delayL.assign((size_t)bufSize, 0.0f);
        delayR.assign((size_t)bufSize, 0.0f);
        gainBuffer.assign((size_t)bufSize, 0.0f);
        writePos = 0;

        // Release: two-stage program-dependent
        relFastCoeff = std::exp(-1.0f / (float(sr) * 0.020f));   // 20ms fast
        relSlowCoeff = std::exp(-1.0f / (float(sr) * 0.200f));   // 200ms slow

        // Lookahead gain smoothing (same approach as compressor)
        smoothCoeff1 = std::exp(-1.0f / (float(sr) * 0.0003f));  // 0.3ms
        smoothCoeff2 = std::exp(-1.0f / (float(sr) * 0.0008f));  // 0.8ms

        // GR meter ballistics — 2.3ms, matching the old hardcoded 0.99 at
        // 44.1kHz but now correct at every sample rate
        grMeterCoeff = std::exp(-1.0f / (float(sr) * 0.0023f));

        envelope = 0.0f;
        prevAppliedGain = 1.0f;
        smoothedGainDB = 0.0f;
        smoothedGainDB2 = 0.0f;
        currentGR = 0.0f;
    }

    void reset()
    {
        std::fill(delayL.begin(), delayL.end(), 0.0f);
        std::fill(delayR.begin(), delayR.end(), 0.0f);
        std::fill(gainBuffer.begin(), gainBuffer.end(), 0.0f);
        writePos = 0;
        envelope = 0.0f;
        currentGR = 0.0f;
        prevAppliedGain = 1.0f;
        smoothedGainDB = 0.0f;
        smoothedGainDB2 = 0.0f;
    }

    void process(float* bufferL, float* bufferR, int numSamples, float ceilingDB)
    {
        float ceilingLin = std::pow(10.0f, ceilingDB / 20.0f);
        int bufSize = LIMITER_LOOKAHEAD + 1;

        for (int i = 0; i < numSamples; ++i)
        {
            float inL = bufferL[i];
            float inR = bufferR[i];

            // Write input to delay
            delayL[(size_t)writePos] = inL;
            delayR[(size_t)writePos] = inR;

            // Peak detection
            float peak = std::max(std::abs(inL), std::abs(inR));

            // Envelope: instant attack, program-dependent release
            if (peak > envelope) {
                envelope = peak;
            } else {
                // Blend fast/slow release based on how much limiting is happening
                float grAmount = (envelope > ceilingLin && ceilingLin > 1e-10f)
                    ? (envelope - ceilingLin) / ceilingLin
                    : 0.0f;
                float relBlend = juce::jlimit(0.0f, 1.0f, grAmount * 4.0f);
                float rc = relFastCoeff * (1.0f - relBlend) + relSlowCoeff * relBlend;
                envelope = rc * envelope + (1.0f - rc) * peak;
            }

            // Compute required gain reduction in dB
            float targetGainDB = 0.0f;
            if (envelope > ceilingLin && envelope > 1e-10f)
                targetGainDB = 20.0f * std::log10(ceilingLin / envelope);

            // Store in gain buffer for lookahead
            gainBuffer[(size_t)writePos] = targetGainDB;

            // ===== COSINE-INTERPOLATED LOOKAHEAD =====
            float minGainDB = targetGainDB;
            int minPos = 0;
            for (int j = 0; j < LIMITER_LOOKAHEAD; ++j) {
                int scanPos = (writePos - j + bufSize) % bufSize;
                if (gainBuffer[(size_t)scanPos] < minGainDB) {
                    minGainDB = gainBuffer[(size_t)scanPos];
                    minPos = j;
                }
            }

            int readPos = (writePos - LIMITER_LOOKAHEAD + bufSize) % bufSize;
            float readGainDB = gainBuffer[(size_t)readPos];

            // Cosine interpolation: smooth ramp toward minimum upcoming gain
            float t = (minPos > 0) ? (float)(LIMITER_LOOKAHEAD - minPos) / (float)LIMITER_LOOKAHEAD : 1.0f;
            float cosT = 0.5f * (1.0f - std::cos(t * juce::MathConstants<float>::pi));
            float appliedGainDB = readGainDB * (1.0f - cosT) + minGainDB * cosT;

            // Two-stage smoothing
            smoothedGainDB = smoothCoeff1 * smoothedGainDB + (1.0f - smoothCoeff1) * appliedGainDB;
            smoothedGainDB2 = smoothCoeff2 * smoothedGainDB2 + (1.0f - smoothCoeff2) * smoothedGainDB;
            appliedGainDB = smoothedGainDB2;

            // Convert to linear with per-sample interpolation
            static constexpr float dB2LinFactor = 0.11512925464970229f; // ln(10)/20
            float targetGainLin = std::exp(appliedGainDB * dB2LinFactor);
            if (targetGainLin > 1.0f) targetGainLin = 1.0f;  // limiter never boosts
            float appliedGain = prevAppliedGain + (targetGainLin - prevAppliedGain) * 0.5f;
            prevAppliedGain = targetGainLin;

            // Read delayed signal and apply gain
            float dL = delayL[(size_t)readPos];
            float dR = delayR[(size_t)readPos];
            writePos = (writePos + 1) % bufSize;

            bufferL[i] = dL * appliedGain;
            bufferR[i] = dR * appliedGain;

            // GR metering (smooth)
            float grDB = (appliedGain > 1.0e-10f && appliedGain < 0.999f)
                       ? -20.0f * std::log10(appliedGain) : 0.0f;
            currentGR = currentGR * grMeterCoeff + grDB * (1.0f - grMeterCoeff);
        }

        // envelope and both smoothing stages are pure IIR state. A single Inf
        // sample would drive smoothedGainDB to -Inf and leave the limiter
        // outputting permanent silence, so recover rather than latch.
        if (! (std::isfinite(envelope) && std::isfinite(smoothedGainDB)
            && std::isfinite(smoothedGainDB2) && std::isfinite(prevAppliedGain)
            && std::isfinite(currentGR)))
            reset();
    }

    float getGainReductionDB() const { return currentGR; }
    int getLatencySamples() const { return LIMITER_LOOKAHEAD; }

private:
    double sr = 44100.0;
    float relFastCoeff = 0.0f, relSlowCoeff = 0.0f;
    float smoothCoeff1 = 0.0f, smoothCoeff2 = 0.0f;
    float grMeterCoeff = 0.0f;
    float envelope = 0.0f;
    float prevAppliedGain = 1.0f;
    float smoothedGainDB = 0.0f, smoothedGainDB2 = 0.0f;
    float currentGR = 0.0f;

    std::vector<float> delayL, delayR, gainBuffer;
    int writePos = 0;
};
