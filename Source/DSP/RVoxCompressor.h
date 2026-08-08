#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>
#include <array>

class RVoxCompressor
{
public:
    static constexpr int   LOOKAHEAD_SAMPLES = 64;
    static constexpr float MAX_RATIO         = 15.0f;
    static constexpr int   GR_HISTORY_SIZE   = 2048;
    static constexpr float KNEE_WIDTH_MIN    = 6.0f;
    static constexpr float KNEE_WIDTH_MAX    = 16.0f;

    // Smooth attack mode: interpolates gain across the lookahead window
    bool smoothAttack = true;

    RVoxCompressor() = default;

    void prepare(double sampleRate, int /*maxBlockSize*/)
    {
        sr = sampleRate;
        invSr = 1.0 / sampleRate;

        releaseCoeffFast = std::exp(-1.0f / (float(sr) * 0.060f));
        releaseCoeffSlow = std::exp(-1.0f / (float(sr) * 0.700f));
        rmsCoeff         = std::exp(-1.0f / (float(sr) * 0.050f));  // 50ms RMS — consonants (2-10ms) barely affect it
        attackCoeff      = std::exp(-1.0f / (float(sr) * 0.0015f)); // default 1.5ms
        peakAttackCoeff  = std::exp(-1.0f / (float(sr) * 0.001f));  // 1ms peak attack (was 0.2ms, tracked individual bass cycles)
        peakReleaseCoeff = std::exp(-1.0f / (float(sr) * 0.005f));  // 5ms peak release (was 2ms)
        expanderReleaseCoeff = std::exp(-1.0f / (float(sr) * 0.5f));
        gateOpenCoeff = std::exp(-1.0f / (float(sr) * 0.002f));   // 2ms open
        gateCloseCoeff = std::exp(-1.0f / (float(sr) * 0.030f));  // 30ms close
        gateDetAttackCoeff = 1.0f - std::exp(-1.0f / (float(sr) * 0.002f)); // 2ms gate detector attack (smoother open)

        // Detector low shelf: 1-pole LP at 200Hz, -3dB cut
        detLowCoeff = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * 200.0f / (float)sr);
        detLowCutGain = 0.29f;

        // SC HPF state
        scHpfSampleRate = (float)sampleRate;
        scHpfX1 = scHpfX2 = scHpfY1 = scHpfY2 = 0.0f;
        scHpfLastFreq = -1.0f;

        warmDcCoeff = 1.0f - (2.0f * juce::MathConstants<float>::pi * 5.0f / (float)sampleRate);
        warmDcCoeff = juce::jlimit(0.995f, 0.9999f, warmDcCoeff);
        warmDcPrevInL = warmDcPrevInR = warmDcPrevOutL = warmDcPrevOutR = 0.0f;

        // Transformer coloration coefficients
        // LP at ~16kHz (treble softening — subtle analog rolloff)
        xfmrLpCoeff = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * 16000.0f / (float)sampleRate);
        xfmrLpL = xfmrLpR = 0.0f;
        // HP at ~25Hz (bass tightening — transformer coupling cap)
        xfmrHpCoeff = 1.0f - (2.0f * juce::MathConstants<float>::pi * 25.0f / (float)sampleRate);
        xfmrHpCoeff = juce::jlimit(0.995f, 0.9999f, xfmrHpCoeff);
        xfmrHpPrevInL = xfmrHpPrevInR = xfmrHpPrevOutL = xfmrHpPrevOutR = 0.0f;

        // Vocal presence: LP at ~3kHz for high-shelf extraction
        presLpCoeff = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * 3000.0f / (float)sampleRate);
        presLpL = presLpR = 0.0f;

        // Gain smoothing in dB domain: ~3ms time constant
        gainSmoothCoeff = std::exp(-1.0f / (float(sr) * 0.003f));

        // Lookahead gain smoothing — SR-dependent (designed at 44.1kHz)
        // Stage 1: ~0.27ms primary envelope
        smoothCoeff1 = std::exp(-1.0f / (float(sr) * 0.00027f));
        // Stage 2: ~0.74ms micro-jitter removal
        smoothCoeff2 = std::exp(-1.0f / (float(sr) * 0.00074f));
        // Punchy mode (no lookahead): ~0.44ms
        punchySmoothCoeff = std::exp(-1.0f / (float(sr) * 0.00044f));
        // GR metering: ~8ms smoothing for display
        grMeterCoeff = std::exp(-1.0f / (float(sr) * 0.008f));

        delayBufferL.assign(LOOKAHEAD_SAMPLES + 1, 0.0f);
        delayBufferR.assign(LOOKAHEAD_SAMPLES + 1, 0.0f);
        gainDBBuffer.assign(LOOKAHEAD_SAMPLES + 1, 0.0f);
        delayWritePos = 0;

        envDB = -100.0f;
        rmsSquaredSum = 0.0f;
        peakEnv = 0.0f;
        expanderEnvDB = -100.0f;
        gainReductionDB = 0.0f;
        smoothGR = 0.0f;
        smoothExpanderGainDB = 0.0f;
        smoothedGainDB = 0.0f;
        prevGrDB = 0.0f;

        grHistory.fill(0.0f);
        inputHistory.fill(-100.0f);
        outputHistory.fill(-100.0f);
        grHistoryWritePos = 0;
        grHistorySampleCounter = 0;
        grHistoryBlockAccum = 0.0f;
        inputHistoryBlockAccum = -100.0f;
        outputHistoryBlockAccum = -100.0f;
        grHistorySamplesPerSlot = std::max(1, (int)(sr / 86.0));
    }

    void reset()
    {
        std::fill(delayBufferL.begin(), delayBufferL.end(), 0.0f);
        std::fill(delayBufferR.begin(), delayBufferR.end(), 0.0f);
        std::fill(gainDBBuffer.begin(), gainDBBuffer.end(), 0.0f);
        delayWritePos = 0;
        envDB = -100.0f;
        rmsSquaredSum = 0.0f;
        peakEnv = 0.0f;
        expanderEnvDB = -100.0f;
        gainReductionDB = 0.0f;
        smoothGR = 0.0f;
        smoothExpanderGainDB = 0.0f;
        smoothedGainDB = 0.0f;
        prevGrDB = 0.0f;
        grHistory.fill(0.0f);
        inputHistory.fill(-100.0f);
        outputHistory.fill(-100.0f);
        grHistoryWritePos = 0;
        grHistorySampleCounter = 0;
        grHistoryBlockAccum = 0.0f;
        inputHistoryBlockAccum = -100.0f;
        outputHistoryBlockAccum = -100.0f;
        warmDcPrevInL = warmDcPrevInR = warmDcPrevOutL = warmDcPrevOutR = 0.0f;
        xfmrLpL = xfmrLpR = 0.0f;
        xfmrHpPrevInL = xfmrHpPrevInR = xfmrHpPrevOutL = xfmrHpPrevOutR = 0.0f;
        presLpL = presLpR = 0.0f;
    }

    void setReleaseTimes(float fastSec, float slowSec)
    {
        releaseCoeffFast = std::exp(-1.0f / (float(sr) * fastSec));
        releaseCoeffSlow = std::exp(-1.0f / (float(sr) * slowSec));
    }

    void setAttackTime(float attackSec)
    {
        attackCoeff = std::exp(-1.0f / (float(sr) * attackSec));
    }

    void setScHpfFreq(float freq)
    {
        scHpfFreq = freq;
        if (freq < 1.0f) return;  // OFF
        if (std::abs(freq - scHpfLastFreq) < 0.5f) return;  // no change
        scHpfLastFreq = freq;

        // 2nd order Butterworth HPF biquad coefficients
        float w0 = 2.0f * juce::MathConstants<float>::pi * freq / scHpfSampleRate;
        float cosW0 = std::cos(w0);
        float sinW0 = std::sin(w0);
        float alpha = sinW0 / (2.0f * 0.7071f);  // Q = 0.7071 (Butterworth)
        float a0 = 1.0f + alpha;
        scHpfB0 = ((1.0f + cosW0) / 2.0f) / a0;
        scHpfB1 = (-(1.0f + cosW0)) / a0;
        scHpfB2 = scHpfB0;
        scHpfA1 = (-2.0f * cosW0) / a0;
        scHpfA2 = (1.0f - alpha) / a0;
    }

    float userKneeWidth = 10.0f;
    float maxGainReductionDB = 36.0f;
    float ratioMultiplier = 1.0f; // Adjustable via ADV display dot (Y axis)
    bool outputDelta = false;

    void process(float* bufferL, float* bufferR, int numSamples, float compDB,
                 float gateThreshDB = -80.0f, int boostMode = 0,
                 const float* scL = nullptr, const float* scR = nullptr)
    {
        float compAmount = -compDB / 36.0f;
        if (compAmount < 0.001f)
        {
            // Smooth transition to unity gain per-sample to avoid clicks
            static constexpr float dB2LinFactor = 0.11512925464970229f;
            for (int i = 0; i < numSamples; ++i)
            {
                delayBufferL[delayWritePos] = bufferL[i];
                delayBufferR[delayWritePos] = bufferR[i];
                gainDBBuffer[delayWritePos] = 0.0f;
                int readPos = (delayWritePos - LOOKAHEAD_SAMPLES + (int)delayBufferL.size()) % (int)delayBufferL.size();
                float delayedL = delayBufferL[readPos];
                float delayedR = delayBufferR[readPos];

                // Fade smoothed gain toward 0dB (unity) using SR-dependent coefficients
                smoothedGainDB = smoothCoeff1 * smoothedGainDB;  // → 0dB
                smoothedGainDB2 = smoothCoeff2 * smoothedGainDB2;
                float targetGain = std::exp(smoothedGainDB2 * dB2LinFactor);
                float appliedGain = prevAppliedGainLin + (targetGain - prevAppliedGainLin) * 0.5f;
                prevAppliedGainLin = targetGain;

                if (outputDelta) {
                    bufferL[i] = delayedL - delayedL * appliedGain;
                    bufferR[i] = delayedR - delayedR * appliedGain;
                } else {
                    bufferL[i] = delayedL * appliedGain;
                    bufferR[i] = delayedR * appliedGain;
                }
                delayWritePos = (delayWritePos + 1) % (int)delayBufferL.size();
                float bypassInDB = 20.0f * std::log10(std::max(1e-10f, std::abs(bufferL[i]) + std::abs(bufferR[i])) * 0.5f);
                pushGRHistory(0.0f, bypassInDB);
                smoothGR = grMeterCoeff * smoothGR; // per-sample fade to zero
            }
            gainReductionDB = 0.0f;
            prevGrDB = 0.0f;
            return;
        }

        float thresholdDB = -6.0f - (compAmount * 30.0f) - (compAmount * compAmount * 12.0f) - (compAmount * compAmount * compAmount * 6.0f);
        float ratio = 1.0f + compAmount * compAmount * (MAX_RATIO - 1.0f);
        ratio = 1.0f + (ratio - 1.0f) * ratioMultiplier; // Scale ratio around 1:1
        if (boostMode == 1) {
            thresholdDB -= 6.0f;   // 6dB lower threshold
            ratio *= 1.5f;         // 50% more ratio
        } else if (boostMode == 2) {
            thresholdDB -= 12.0f;  // 12dB lower — catches everything
            ratio *= 2.5f;         // massive ratio
        }
        float baseKneeDB = userKneeWidth;
        // RMS-dominant detection
        float peakBlend = 0.08f + compAmount * 0.12f;
        // Harmonic warmth: scales quadratically with compression (gentle at low, present at high)
        float warmthAmount = compAmount * compAmount * 0.05f;  // 0 → 0.05 at max (~0.7% THD)

        for (int i = 0; i < numSamples; ++i)
        {
            float inL = bufferL[i];
            float inR = bufferR[i];
            float detL = (scL != nullptr) ? scL[i] : inL;
            float detR = (scR != nullptr) ? scR[i] : inR;

            // ===== DETECTOR =====
            // True stereo summing
            float detMono = std::sqrt(detL * detL + detR * detR) * 0.7071f;

            // Sidechain HPF: removes bass from detector only (audio untouched)
            // Prevents bass pumping the compressor
            if (scHpfFreq > 1.0f) {
                // 2nd order Butterworth HPF via biquad
                float scHpfOut = scHpfB0 * detMono + scHpfB1 * scHpfX1 + scHpfB2 * scHpfX2
                               - scHpfA1 * scHpfY1 - scHpfA2 * scHpfY2;
                scHpfX2 = scHpfX1; scHpfX1 = detMono;
                scHpfY2 = scHpfY1; scHpfY1 = scHpfOut;
                detMono = scHpfOut;
            }

            // Low shelf: cut below 200Hz (fixed, removes rumble from detector)
            detLowLP = detLowLP + detLowCoeff * (detMono - detLowLP);
            float lowContent = detLowLP;
            detMono = detMono - detLowCutGain * lowContent;

            // ===== HYBRID RMS/PEAK DETECTOR =====
            float monoAbs = std::abs(detMono);

            if (monoAbs > peakEnv)
                peakEnv = peakAttackCoeff * peakEnv + (1.0f - peakAttackCoeff) * monoAbs;
            else
                peakEnv = peakReleaseCoeff * peakEnv + (1.0f - peakReleaseCoeff) * monoAbs;

            float squared = monoAbs * monoAbs;
            rmsSquaredSum = rmsCoeff * rmsSquaredSum + (1.0f - rmsCoeff) * squared;
            float rmsLevel = std::sqrt(rmsSquaredSum);

            float detLevel = rmsLevel * (1.0f - peakBlend) + peakEnv * peakBlend;
            if (detLevel < 1e-15f) detLevel = 0.0f;
            // 20*log10(x) = 8.6858896*ln(x) — std::log is ~2x faster than log10
            static constexpr float ln2dB = 8.685889638065037f; // 20/ln(10)
            float detDB = (detLevel > 1e-10f) ? ln2dB * std::log(detLevel) : -100.0f;

            // ===== PROGRAM-DEPENDENT ENVELOPE =====
            if (detDB > envDB)
            {
                // Attack: user-configurable time constant
                envDB = attackCoeff * envDB + (1.0f - attackCoeff) * detDB;
            }
            else
            {
                // More GR = slower release (anti-pump)
                // But cap the slowdown so extreme settings still breathe
                float grBlendLin = juce::jlimit(0.0f, 1.0f, prevGrDB / 20.0f);  // was /12 — now needs 20dB to go fully slow
                float grBlend = grBlendLin * grBlendLin;  // quadratic
                // Blend: high grBlend (heavy compression) → slow release
                float rc = releaseCoeffFast * (1.0f - grBlend) + releaseCoeffSlow * grBlend;
                // Less slowdown at extreme comp — let it breathe
                float compSlowdown = 1.0f + compAmount * 0.15f; // was 0.3 — now max 15% slower
                rc = 1.0f - (1.0f - rc) / compSlowdown;
                envDB = rc * envDB + (1.0f - rc) * detDB;
            }

            // ===== ADAPTIVE KNEE =====
            // Near threshold: wider knee (gentle transition)
            // Far above threshold: narrower knee (precise control)
            float distFromThresh = std::abs(envDB - thresholdDB);
            float kneeScale = juce::jlimit(0.7f, 1.3f, 1.3f - distFromThresh / 24.0f);
            float kneeDB = baseKneeDB * kneeScale;

            float grDB = computeGainReduction(envDB, thresholdDB, ratio, kneeDB);
            // Apply GR Range limit
            grDB = juce::jmin(grDB, maxGainReductionDB);

            // ===== NOISE GATE with hysteresis =====
            // gateThreshDB: -80 = off, -60..-20 = active range
            // Fast attack (opens quickly), medium release (closes smoothly)
            float gateRelCoeff = expanderReleaseCoeff; // ~500ms release

            if (detDB > expanderEnvDB)
                expanderEnvDB = expanderEnvDB + gateDetAttackCoeff * (detDB - expanderEnvDB);
            else
                expanderEnvDB = gateRelCoeff * expanderEnvDB + (1.0f - gateRelCoeff) * detDB;
            if (std::abs(expanderEnvDB) < 1e-10f) expanderEnvDB = -100.0f;

            float expanderTargetDB = 0.0f;
            if (gateThreshDB > -79.0f)  // gate is active
            {
                // Hysteresis: opens at thresh, closes at thresh - 6dB
                float openThresh = gateThreshDB;
                float closeThresh = gateThreshDB - 6.0f;
                float gateRange = 30.0f; // max attenuation in dB (softer than 40)

                if (expanderEnvDB < closeThresh)
                {
                    float below = closeThresh - expanderEnvDB;
                    float ea = juce::jlimit(0.0f, 1.0f, below / 15.0f);
                    expanderTargetDB = ea * -gateRange;
                    gateOpen = false;
                }
                else if (expanderEnvDB > openThresh)
                {
                    expanderTargetDB = 0.0f;
                    gateOpen = true;
                }
                else
                {
                    // In hysteresis zone — hold previous state
                    if (!gateOpen)
                    {
                        float below = closeThresh - expanderEnvDB;
                        float ea = juce::jlimit(0.0f, 1.0f, below / 15.0f);
                        expanderTargetDB = ea * -gateRange;
                    }
                }
            }
            // Asymmetric smoothing: fast open (2ms), slow close (30ms)
            float gateSmooth = (expanderTargetDB > smoothExpanderGainDB) ? gateOpenCoeff : gateCloseCoeff;
            smoothExpanderGainDB = smoothExpanderGainDB * gateSmooth + expanderTargetDB * (1.0f - gateSmooth);
            if (std::abs(smoothExpanderGainDB) < 0.01f) smoothExpanderGainDB = 0.0f;

            // ===== STORE GAIN IN dB DOMAIN =====
            float totalGainDB = -grDB + smoothExpanderGainDB;

            delayBufferL[delayWritePos] = inL;
            delayBufferR[delayWritePos] = inR;
            gainDBBuffer[delayWritePos] = totalGainDB;

            int readPos = (delayWritePos - LOOKAHEAD_SAMPLES + (int)delayBufferL.size()) % (int)delayBufferL.size();
            float delayedL = delayBufferL[readPos];
            float delayedR = delayBufferR[readPos];

            float appliedGainDB;

            if (smoothAttack)
            {
                // ===== COSINE-INTERPOLATED LOOKAHEAD =====
                // Scan the lookahead window for the minimum upcoming gain.
                // Then cosine-interpolate from the read position gain toward
                // that minimum. This creates an ultra-smooth, artifact-free
                // attack ramp — the hallmark of top-tier compressors.
                float minGainDB = totalGainDB;
                int minPos = 0;
                for (int j = 0; j < LOOKAHEAD_SAMPLES; ++j) {
                    int scanPos = (delayWritePos - j + (int)gainDBBuffer.size()) % (int)gainDBBuffer.size();
                    if (gainDBBuffer[scanPos] < minGainDB) {
                        minGainDB = gainDBBuffer[scanPos];
                        minPos = j;
                    }
                }
                float readGainDB = gainDBBuffer[readPos];

                // Cosine fade: smoothly transition from readGain toward minimum
                // t = how far through the window we are (0 = just written, 1 = read pos)
                float t = (minPos > 0) ? (float)(LOOKAHEAD_SAMPLES - minPos) / (float)LOOKAHEAD_SAMPLES : 1.0f;
                // Cosine curve: 0→1 smoothly (starts slow, accelerates, ends slow)
                float cosT = 0.5f * (1.0f - std::cos(t * juce::MathConstants<float>::pi));
                appliedGainDB = readGainDB * (1.0f - cosT) + minGainDB * cosT;

                // Stage 1: primary envelope smoothing
                smoothedGainDB = smoothCoeff1 * smoothedGainDB + (1.0f - smoothCoeff1) * appliedGainDB;
                // Stage 2: micro-jitter removal
                smoothedGainDB2 = smoothCoeff2 * smoothedGainDB2 + (1.0f - smoothCoeff2) * smoothedGainDB;
                appliedGainDB = smoothedGainDB2;
            }
            else
            {
                // ===== PUNCHY MODE =====
                float targetGainDB = gainDBBuffer[readPos];
                smoothedGainDB = punchySmoothCoeff * smoothedGainDB + (1.0f - punchySmoothCoeff) * targetGainDB;
                smoothedGainDB2 = smoothCoeff2 * smoothedGainDB2 + (1.0f - smoothCoeff2) * smoothedGainDB;
                appliedGainDB = smoothedGainDB2;
            }

            // ===== ANTI-ALIASED GAIN APPLICATION =====
            static constexpr float dB2LinFactor = 0.11512925464970229f; // ln(10)/20
            float targetGain = std::exp(appliedGainDB * dB2LinFactor);
            float appliedGain = prevAppliedGainLin + (targetGain - prevAppliedGainLin) * 0.5f;
            prevAppliedGainLin = targetGain;

            delayWritePos = (delayWritePos + 1) % (int)delayBufferL.size();

            bufferL[i] = outputDelta ? (delayedL - delayedL * appliedGain) : (delayedL * appliedGain);
            bufferR[i] = outputDelta ? (delayedR - delayedR * appliedGain) : (delayedR * appliedGain);

            // ===== CHARACTER STAGE (bypassed in CLEAN mode) =====
            if (!outputDelta && characterMode && warmthAmount > 0.001f) {
                // 1. Second harmonic warmth: x + k*x²
                float wL = bufferL[i];
                float wR = bufferR[i];
                bufferL[i] = wL + warmthAmount * wL * wL;
                bufferR[i] = wR + warmthAmount * wR * wR;
                // DC offset from x² removed by 1-pole HP
                float outL = bufferL[i] - warmDcPrevInL + warmDcCoeff * warmDcPrevOutL;
                warmDcPrevInL = bufferL[i]; warmDcPrevOutL = outL;
                bufferL[i] = outL;
                float outR = bufferR[i] - warmDcPrevInR + warmDcCoeff * warmDcPrevOutR;
                warmDcPrevInR = bufferR[i]; warmDcPrevOutR = outR;
                bufferR[i] = outR;

                // 2. Transformer coloration: gentle bass rolloff + treble rolloff
                // LP at ~16kHz (treble softening)
                xfmrLpL += xfmrLpCoeff * (bufferL[i] - xfmrLpL);
                xfmrLpR += xfmrLpCoeff * (bufferR[i] - xfmrLpR);
                bufferL[i] = xfmrLpL;
                bufferR[i] = xfmrLpR;
                // HP at ~25Hz (bass tightening)
                float hpOutL = bufferL[i] - xfmrHpPrevInL + xfmrHpCoeff * xfmrHpPrevOutL;
                xfmrHpPrevInL = bufferL[i]; xfmrHpPrevOutL = hpOutL;
                bufferL[i] = hpOutL;
                float hpOutR = bufferR[i] - xfmrHpPrevInR + xfmrHpCoeff * xfmrHpPrevOutR;
                xfmrHpPrevInR = bufferR[i]; xfmrHpPrevOutR = hpOutR;
                bufferR[i] = hpOutR;

                // 3. Vocal presence: subtle 2-4kHz boost via 1-pole shelf
                // presLP tracks the low-frequency component; presence = input - LP
                float presAmount = warmthAmount * 2.5f;  // scales with compression
                presLpL += presLpCoeff * (bufferL[i] - presLpL);
                presLpR += presLpCoeff * (bufferR[i] - presLpR);
                bufferL[i] += (bufferL[i] - presLpL) * presAmount;
                bufferR[i] += (bufferR[i] - presLpR) * presAmount;
            }

            gainReductionDB = grDB;
            prevGrDB = grDB;
            pushGRHistory(grDB, envDB);

            // Per-sample GR metering — must be inside loop for correct timing
            smoothGR = grMeterCoeff * smoothGR + (1.0f - grMeterCoeff) * grDB;
        }
    }

    float getGainReductionDB() const { return smoothGR; }
    int getLatencySamples() const { return LOOKAHEAD_SAMPLES; }
    const std::array<float, GR_HISTORY_SIZE>& getGRHistory() const { return grHistory; }
    int getGRHistoryWritePos() const { return grHistoryWritePos; }

    // Public for OS mode switch state preservation
    float smoothGR = 0.0f;
    std::array<float, GR_HISTORY_SIZE> grHistory {};
    std::array<float, GR_HISTORY_SIZE> inputHistory {};
    std::array<float, GR_HISTORY_SIZE> outputHistory {};  // input - GR for before/after display
    int grHistoryWritePos = 0;
    bool characterMode = false;  // true = CHARACTER (warmth+transformer+presence), false = CLEAN

private:
    // Warm soft-knee with smoothstep S-curve
    float computeGainReduction(float inputDB, float thresholdDB, float ratio, float kneeDB) const
    {
        float halfKnee = kneeDB / 2.0f;
        float output;
        if (inputDB < (thresholdDB - halfKnee))
        {
            output = inputDB;
        }
        else if (inputDB > (thresholdDB + halfKnee))
        {
            output = thresholdDB + (inputDB - thresholdDB) / ratio;
        }
        else
        {
            float x = inputDB - thresholdDB + halfKnee;
            float t = x / kneeDB;
            float tSat = t * t * (3.0f - 2.0f * t); // smoothstep
            float fullGR = (inputDB - thresholdDB) * (1.0f - 1.0f / ratio);
            output = inputDB - tSat * fullGR;
        }
        return std::max(0.0f, inputDB - output);
    }

    void pushGRHistory(float grDB, float inputDB = -100.0f)
    {
        grHistoryBlockAccum = std::max(grHistoryBlockAccum, grDB);
        inputHistoryBlockAccum = std::max(inputHistoryBlockAccum, inputDB);
        // Output = input level minus gain reduction
        float outDB = inputDB - grDB;
        outputHistoryBlockAccum = std::max(outputHistoryBlockAccum, outDB);
        grHistorySampleCounter++;
        if (grHistorySampleCounter >= grHistorySamplesPerSlot) {
            grHistory[grHistoryWritePos] = grHistoryBlockAccum;
            inputHistory[grHistoryWritePos] = inputHistoryBlockAccum;
            outputHistory[grHistoryWritePos] = outputHistoryBlockAccum;
            grHistoryWritePos = (grHistoryWritePos + 1) % GR_HISTORY_SIZE;
            grHistorySampleCounter = 0;
            grHistoryBlockAccum = 0.0f;
            inputHistoryBlockAccum = -100.0f;
            outputHistoryBlockAccum = -100.0f;
        }
    }

    double sr = 44100.0, invSr = 1.0 / 44100.0;
    std::vector<float> delayBufferL, delayBufferR;
    std::vector<float> gainDBBuffer; // gain stored in dB, not linear
    int delayWritePos = 0;

    float releaseCoeffFast = 0.0f, releaseCoeffSlow = 0.0f;
    float attackCoeff = 0.0f;
    float rmsCoeff = 0.0f;
    float peakAttackCoeff = 0.0f, peakReleaseCoeff = 0.0f;
    float gainSmoothCoeff = 0.0f;
    float smoothCoeff1 = 0.0f, smoothCoeff2 = 0.0f, punchySmoothCoeff = 0.0f;
    float grMeterCoeff = 0.0f;

    float rmsSquaredSum = 0.0f;
    float peakEnv = 0.0f;
    float envDB = -100.0f;
    float expanderEnvDB = -100.0f;
    float expanderReleaseCoeff = 0.0f;
    float gateOpenCoeff = 0.0f, gateCloseCoeff = 0.0f;
    float gateDetAttackCoeff = 0.0f;
    float smoothExpanderGainDB = 0.0f;
    bool gateOpen = true;
    float gainReductionDB = 0.0f;
    // smoothGR, grHistory, grHistoryWritePos are public (above private:)
    float smoothedGainDB = 0.0f; // stage 1 smoothing
    float smoothedGainDB2 = 0.0f; // stage 2 micro-jitter removal
    float prevAppliedGainLin = 1.0f; // for per-sample gain interpolation
    float prevGrDB = 0.0f;

    // Detector low shelf cut
    float detLowLP = 0.0f;
    float detLowCoeff = 0.0f;
    float detLowCutGain = 0.29f;

    // Harmonic warmth DC blocker
    float warmDcPrevInL = 0.0f, warmDcPrevInR = 0.0f;
    float warmDcPrevOutL = 0.0f, warmDcPrevOutR = 0.0f;
    float warmDcCoeff = 0.9995f;

    // Transformer coloration (character mode)
    float xfmrLpL = 0.0f, xfmrLpR = 0.0f;          // LP at ~16kHz
    float xfmrLpCoeff = 0.0f;
    float xfmrHpPrevInL = 0.0f, xfmrHpPrevInR = 0.0f;  // HP at ~25Hz
    float xfmrHpPrevOutL = 0.0f, xfmrHpPrevOutR = 0.0f;
    float xfmrHpCoeff = 0.0f;

    // Vocal presence shelf (character mode)
    float presLpL = 0.0f, presLpR = 0.0f;
    float presLpCoeff = 0.0f;  // LP at ~3kHz for presence extraction

    // Sidechain HPF (detector only, 2nd order Butterworth biquad)
    float scHpfFreq = 0.0f;  // 0 = off
    float scHpfB0 = 1.0f, scHpfB1 = 0.0f, scHpfB2 = 0.0f;
    float scHpfA1 = 0.0f, scHpfA2 = 0.0f;
    float scHpfX1 = 0.0f, scHpfX2 = 0.0f;
    float scHpfY1 = 0.0f, scHpfY2 = 0.0f;
    float scHpfLastFreq = -1.0f;
    float scHpfSampleRate = 44100.0f;

    int grHistorySampleCounter = 0;
    int grHistorySamplesPerSlot = 1;
    float grHistoryBlockAccum = 0.0f;
    float inputHistoryBlockAccum = -100.0f;
    float outputHistoryBlockAccum = -100.0f;
};
