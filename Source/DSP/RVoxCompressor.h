#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "SlidingExtremum.h"
#include <cmath>
#include <vector>
#include <array>
#include <atomic>

class RVoxCompressor
{
public:
    // Lookahead in seconds rather than samples, so the attack character does not
    // change with the session sample rate. It used to be a fixed 64 samples,
    // which meant 0.73 ms at 44.1 kHz but only 0.17 ms at 192 kHz.
    static constexpr float LOOKAHEAD_SECONDS = 0.0015f;
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
        lookahead = std::max(8, (int)std::lround(sr * (double)LOOKAHEAD_SECONDS));

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




        // Gain smoothing in dB domain: ~3ms time constant
        gainSmoothCoeff = std::exp(-1.0f / (float(sr) * 0.003f));

        // Lookahead gain smoothing. Retuned to settle inside the 1.5 ms window:
        // the old 0.27/0.74 ms pair only fitted because the coefficients were
        // being computed for half the rate the loop actually ran at.
        smoothCoeff1 = std::exp(-1.0f / (float(sr) * 0.00020f));
        smoothCoeff2 = std::exp(-1.0f / (float(sr) * 0.00045f));
        punchySmoothCoeff = std::exp(-1.0f / (float(sr) * 0.00035f));
        // GR metering: ~8ms smoothing for display
        grMeterCoeff = std::exp(-1.0f / (float(sr) * 0.008f));

        // Auto-threshold programme tracker. Asymmetric: adapts to a louder
        // section reasonably quickly, lets go slowly so short pauses and
        // sustained quiet passages do not move the threshold much.
        autoLevelRiseCoeff = std::exp(-1.0f / (float(sr) * 0.800f));
        autoLevelFallCoeff = std::exp(-1.0f / (float(sr) * 3.000f));

        delayBufferL.assign(lookahead + 1, 0.0f);
        delayBufferR.assign(lookahead + 1, 0.0f);
        gainDBBuffer.assign(lookahead + 1, 0.0f);
        gainMin.prepare(lookahead);

        grHistorySamplesPerSlot = std::max(1, (int)(sr / 86.0));

        // Clear runtime state through reset() rather than repeating the list
        // here — the two copies had already drifted apart, leaving stale gain
        // and filter state to click on the first sample after a re-prepare.
        reset();
    }

    void reset()
    {
        std::fill(delayBufferL.begin(), delayBufferL.end(), 0.0f);
        std::fill(delayBufferR.begin(), delayBufferR.end(), 0.0f);
        std::fill(gainDBBuffer.begin(), gainDBBuffer.end(), 0.0f);
        gainMin.reset(0.0f);
        delayWritePos = 0;
        envDB = -100.0f;
        rmsSquaredSum = 0.0f;
        peakEnv = 0.0f;
        expanderEnvDB = -100.0f;
        gainReductionDB = 0.0f;
        smoothGR = 0.0f;
        smoothExpanderGainDB = 0.0f;
        smoothedGainDB = 0.0f;
        smoothedGainDB2 = 0.0f;
        prevAppliedGainLin = 1.0f;
        detLowLP = 0.0f;
        autoLevelDB = -18.0f;
        autoLevelPrimed = false;
        gateOpen = true;
        scHpfX1 = scHpfX2 = scHpfY1 = scHpfY2 = 0.0f;
        prevGrDB = 0.0f;
        grHistory.fill(0.0f);
        inputHistory.fill(-100.0f);
        outputHistory.fill(-100.0f);
        grHistoryWritePos.store(0, std::memory_order_release);
        grHistorySampleCounter = 0;
        grHistoryBlockAccum = 0.0f;
        inputHistoryBlockAccum = -100.0f;
        outputHistoryBlockAccum = -100.0f;
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

    void process(float* bufferL, float* bufferR, int numSamples, float compDB,
                 float gateThreshDB = -80.0f,
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
                // Keep the window fed while the knob sits at zero, otherwise
                // turning it up would look back at stale gain values.
                gainMin.pushAndGet(0.0f);
                int readPos = (delayWritePos - lookahead + (int)delayBufferL.size()) % (int)delayBufferL.size();
                float delayedL = delayBufferL[readPos];
                float delayedR = delayBufferR[readPos];

                // Fade smoothed gain toward 0dB (unity) using SR-dependent coefficients
                smoothedGainDB = smoothCoeff1 * smoothedGainDB;  // → 0dB
                smoothedGainDB2 = smoothCoeff2 * smoothedGainDB2;
                float targetGain = std::exp(smoothedGainDB2 * dB2LinFactor);
                float appliedGain = prevAppliedGainLin + (targetGain - prevAppliedGainLin) * 0.5f;
                prevAppliedGainLin = targetGain;

                bufferL[i] = delayedL * appliedGain;
                bufferR[i] = delayedR * appliedGain;
                delayWritePos = (delayWritePos + 1) % (int)delayBufferL.size();
                float bypassInDB = 20.0f * std::log10(std::max(1e-10f, std::abs(bufferL[i]) + std::abs(bufferR[i])) * 0.5f);
                pushGRHistory(0.0f, bypassInDB);
                smoothGR = grMeterCoeff * smoothGR; // per-sample fade to zero
            }
            gainReductionDB = 0.0f;
            prevGrDB = 0.0f;
            return;
        }

        // ===== AUTO THRESHOLD =====
        // The threshold sits a knob-controlled distance below a slow average of
        // the programme level, rather than at a fixed dBFS value. With a fixed
        // threshold the knob only did anything on hot tracks: on a -20 dBFS
        // source the threshold was still above the signal at knob 12, so the
        // first third of the range produced no gain reduction at all.
        //
        // depth goes from 6 dB *above* the average (no compression) to 12 dB
        // below it, so knob position means the same thing regardless of how hot
        // the incoming track is. Calibrated against measurement: at the top of
        // the range this lands around 17 dB of reduction, which is already a lot
        // for a vocal. A steeper curve was tried first and produced 27 dB, which
        // drove makeup into its ceiling and left nothing but squash.
        //
        // autoLevelDB is measured from the detector, i.e. pre-compression, so it
        // cannot feed back from the gain being applied.
        float depthDB = -6.0f + compAmount * 18.0f;
        float thresholdDB = autoLevelDB - depthDB;

        float ratio = 1.0f + compAmount * compAmount * (MAX_RATIO - 1.0f);
        ratio = 1.0f + (ratio - 1.0f) * ratioMultiplier; // Scale ratio around 1:1
        float baseKneeDB = userKneeWidth;
        // RMS-dominant detection
        float peakBlend = 0.08f + compAmount * 0.12f;

        for (int i = 0; i < numSamples; ++i)
        {
            float inL = bufferL[i];
            float inR = bufferR[i];
            float detL = (scL != nullptr) ? scL[i] : inL;
            float detR = (scR != nullptr) ? scR[i] : inR;

            // ===== DETECTOR =====
            // Sidechain filtering happens on the WAVEFORM, before rectification.
            // It used to run after sqrt(L^2+R^2), i.e. on an already-rectified
            // signal — filtering an envelope rather than audio, so neither the
            // SC HPF knob nor the 200 Hz shelf did what its label said. A 50 Hz
            // tone rectifies to a ~100 Hz ripple riding on DC, so high-passing
            // afterwards removed the DC but left the ripple, and the knob's
            // frequency had no straightforward meaning at all.
            float mono = (detL + detR) * 0.5f;

            // Sidechain HPF: keeps bass out of the detector (audio untouched)
            if (scHpfFreq > 1.0f) {
                // 2nd order Butterworth HPF via biquad
                float scHpfOut = scHpfB0 * mono + scHpfB1 * scHpfX1 + scHpfB2 * scHpfX2
                               - scHpfA1 * scHpfY1 - scHpfA2 * scHpfY2;
                scHpfX2 = scHpfX1; scHpfX1 = mono;
                scHpfY2 = scHpfY1; scHpfY1 = scHpfOut;
                mono = scHpfOut;
            }

            // Fixed low shelf: -3 dB below 200 Hz, keeps rumble out of the detector
            detLowLP = detLowLP + detLowCoeff * (mono - detLowLP);
            mono = mono - detLowCutGain * detLowLP;

            // ===== HYBRID RMS/PEAK DETECTOR =====
            float monoAbs = std::abs(mono);

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

            // ===== SLOW PROGRAMME LEVEL (drives the auto threshold) =====
            // Deliberately far slower than the compressor's own release, so the
            // threshold adapts between sections without chasing the gain
            // reduction and flattening the compression out. Only tracked while
            // there is signal, otherwise a pause would drag it down and the
            // vocal would get slammed on the way back in.
            if (detDB > -60.0f) {
                if (! autoLevelPrimed) { autoLevelDB = detDB; autoLevelPrimed = true; }
                float c = (detDB > autoLevelDB) ? autoLevelRiseCoeff : autoLevelFallCoeff;
                autoLevelDB = c * autoLevelDB + (1.0f - c) * detDB;
            }

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

            int readPos = (delayWritePos - lookahead + (int)delayBufferL.size()) % (int)delayBufferL.size();
            float delayedL = delayBufferL[readPos];
            float delayedR = delayBufferR[readPos];

            float appliedGainDB;

            if (smoothAttack)
            {
                // ===== LOOKAHEAD =====
                // Target the lowest gain required anywhere in the window, so
                // reduction is already underway when the peak reaches the
                // output. The two one-pole stages below turn that step into the
                // actual ramp.
                //
                // This used to cosine-interpolate between the window minimum
                // and the gain at the read position, weighted by how far away
                // the minimum was — but the weighting ran backwards: the closer
                // the peak, the *less* reduction was applied. On an isolated
                // transient the target dropped to full depth immediately, drifted
                // back toward unity over the window, then snapped down again as
                // the peak landed, which is a gain ripple rather than a ramp.
                float windowMinDB = gainMin.pushAndGet(totalGainDB);

                // Stage 1: primary envelope smoothing
                smoothedGainDB = smoothCoeff1 * smoothedGainDB + (1.0f - smoothCoeff1) * windowMinDB;
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

            bufferL[i] = delayedL * appliedGain;
            bufferR[i] = delayedR * appliedGain;

            gainReductionDB = grDB;
            prevGrDB = grDB;
            pushGRHistory(grDB, envDB);

            // Per-sample GR metering — must be inside loop for correct timing
            smoothGR = grMeterCoeff * smoothGR + (1.0f - grMeterCoeff) * grDB;
        }

        sanitizeState();
    }

    float getGainReductionDB() const { return smoothGR; }
    // Gate state, already computed internally but not previously exposed —
    // needed to draw the threshold/attenuation on the IN meter.
    bool isGateOpen() const { return gateOpen; }
    float getGateReductionDB() const { return smoothExpanderGainDB; }
    int getLatencySamples() const { return lookahead; }
    const std::array<float, GR_HISTORY_SIZE>& getGRHistory() const { return grHistory; }
    int getGRHistoryWritePos() const { return grHistoryWritePos.load(std::memory_order_acquire); }

    // Public for OS mode switch state preservation
    float smoothGR = 0.0f;
    std::array<float, GR_HISTORY_SIZE> grHistory {};
    std::array<float, GR_HISTORY_SIZE> inputHistory {};
    std::array<float, GR_HISTORY_SIZE> outputHistory {};  // input - GR for before/after display
    // Read by the editor's 60 Hz timer while the audio thread writes it. Atomic
    // with release/acquire so the compiler cannot cache or reorder it — with LTO
    // enabled a plain int could be hoisted out of the paint loop entirely.
    std::atomic<int> grHistoryWritePos { 0 };

private:
    // Every follower and filter below is pure IIR state: once it holds a NaN or
    // Inf it can never return to a valid value on its own, and the -100 dB
    // fallback in the detector turns that into "no signal" rather than an
    // audible fault — the compressor would just silently stop compressing.
    // Checked once per block, so the cost is negligible.
    void sanitizeState()
    {
        bool bad = ! (std::isfinite(rmsSquaredSum) && std::isfinite(peakEnv)
                   && std::isfinite(envDB) && std::isfinite(expanderEnvDB)
                   && std::isfinite(smoothedGainDB) && std::isfinite(smoothedGainDB2)
                   && std::isfinite(prevAppliedGainLin) && std::isfinite(detLowLP)
                   && std::isfinite(smoothExpanderGainDB) && std::isfinite(smoothGR));
        if (! bad)
            bad = ! (std::isfinite(scHpfX1) && std::isfinite(scHpfX2)
                  && std::isfinite(scHpfY1) && std::isfinite(scHpfY2));
        if (bad)
            reset();
    }

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
            const int wp = grHistoryWritePos.load(std::memory_order_relaxed);
            grHistory[(size_t)wp] = grHistoryBlockAccum;
            inputHistory[(size_t)wp] = inputHistoryBlockAccum;
            outputHistory[(size_t)wp] = outputHistoryBlockAccum;
            grHistoryWritePos.store((wp + 1) % GR_HISTORY_SIZE, std::memory_order_release);
            grHistorySampleCounter = 0;
            grHistoryBlockAccum = 0.0f;
            inputHistoryBlockAccum = -100.0f;
            outputHistoryBlockAccum = -100.0f;
        }
    }

    double sr = 44100.0, invSr = 1.0 / 44100.0;
    int lookahead = 64;
    std::vector<float> delayBufferL, delayBufferR;
    std::vector<float> gainDBBuffer; // gain stored in dB, not linear
    SlidingMinimum gainMin;          // lowest gain across the lookahead window
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
    // Slow programme level the auto threshold is referenced to
    float autoLevelDB = -18.0f;
    bool  autoLevelPrimed = false;
    float autoLevelRiseCoeff = 0.0f, autoLevelFallCoeff = 0.0f;
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
