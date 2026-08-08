#include "PluginProcessor.h"
#include "PluginEditor.h"

GoldCompProcessor::GoldCompProcessor()
    : AudioProcessor(BusesProperties()
                     .withInput("Input", juce::AudioChannelSet::stereo(), true)
                     .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, &undoManager, "Parameters", createParameterLayout())
{
}

GoldCompProcessor::~GoldCompProcessor() {}

juce::AudioProcessorValueTreeState::ParameterLayout GoldCompProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID("bypass", 1), "Bypass", false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("comp", 1), "Compression",
        juce::NormalisableRange<float>(0.0f, 36.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("gain", 1), "Gain",
        juce::NormalisableRange<float>(-36.0f, 0.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("hpf", 1), "HP Filter",
        juce::NormalisableRange<float>(0.0f, 300.0f, 1.0f, 0.4f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("schpf", 1), "SC HP Filter",
        juce::NormalisableRange<float>(0.0f, 400.0f, 1.0f, 0.4f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("clip", 1), "Soft Clip",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID("clipmode", 1), "Clip Mode",
        juce::StringArray { "Clean", "Tube", "Tape", "Hard" }, 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("mix", 1), "Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 100.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("gate", 1), "Gate",
        juce::NormalisableRange<float>(-80.0f, -20.0f, 0.1f), -80.0f));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID("lookahead", 1), "Lookahead", true));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID("boost", 1), "Boost", 0, 2, 0));
    // ADV panel parameters
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("attackMs", 1), "Attack",
        juce::NormalisableRange<float>(0.01f, 10.0f, 0.01f, 0.4f), 0.1f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("relFastMs", 1), "Rel Fast",
        juce::NormalisableRange<float>(10.0f, 200.0f, 1.0f), 60.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("relSlowMs", 1), "Rel Slow",
        juce::NormalisableRange<float>(200.0f, 2000.0f, 10.0f), 700.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("kneeW", 1), "Knee",
        juce::NormalisableRange<float>(2.0f, 20.0f, 0.5f), 10.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("grRange", 1), "GR Range",
        juce::NormalisableRange<float>(1.0f, 36.0f, 0.5f), 36.0f));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID("oversample", 1), "Oversample", 0, 2, 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("inTrim", 1), "In Trim",
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("ratioMult", 1), "Ratio",
        juce::NormalisableRange<float>(0.3f, 3.0f, 0.01f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("transProtect", 1), "Trans Protect",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 55.0f));
    return { params.begin(), params.end() };
}

void GoldCompProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    compressor.prepare(sampleRate, samplesPerBlock);
    limiter.prepare(sampleRate, samplesPerBlock);

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32)samplesPerBlock, 2 };
    sigHpFilter.prepare(spec); sigHpFilter.reset();

    oversampler.initProcessing((size_t)samplesPerBlock);
    oversampler.reset();
    compOS.initProcessing((size_t)samplesPerBlock);
    compOS.reset();

    // Wet path latency: compressor lookahead (in 2x samples → /2) + OS latency + limiter
    totalWetLatency = compressor.getLatencySamples() / 2
                    + (int)compOS.getLatencyInSamples()
                    + limiter.getLatencySamples();
    setLatencySamples(totalWetLatency);

    // Dry delay for latency-compensated mix
    int delaySize = totalWetLatency + samplesPerBlock + 16;
    dryDelayL.assign(delaySize, 0.0f);
    dryDelayR.assign(delaySize, 0.0f);
    dryDelayWritePos = 0;

    smoothedInLUFS = 0.0f;
    smoothedOutLUFS = 0.0f;
    smoothedPeakDB = -60.0f;
    smoothedRMSDB = -60.0f;
    smoothedCrestDB = 12.0f;
    analysisBlockCount = 0;
    dcBlockCoeff = 1.0f - (float)(2.0 * juce::MathConstants<double>::pi * 5.0 / sampleRate); // ~5Hz HPF
    kShelfIn = {}; kHPIn = {}; kShelfOut = {}; kHPOut = {};
    computeKWeightingCoeffs(sampleRate);

    // De-click: slow envelope for "normal level" tracking
    // Attack: ~5ms (doesn't follow transients — they exceed the envelope)
    // Release: ~20ms (holds level between syllables)
    dcAttCoeff = std::exp(-1.0f / (float)(sampleRate * 0.005));
    dcRelCoeff = std::exp(-1.0f / (float)(sampleRate * 0.005));  // same as attack for smooth tracking
    dcSlowRelCoeff = std::exp(-1.0f / (float)(sampleRate * 0.020));
    dcEnvL = 0.0f; dcEnvR = 0.0f;

    // RIDE: auto-leveling coefficients
    rideEnvCoeff = std::exp(-(float)1.0f / (float)(sampleRate * 2.0));     // ~2 sec RMS tracking
    rideOffsetSmoothCoeff = std::exp(-1.0f / (float)(sampleRate * 0.5));   // ~0.5 sec smooth movement
    rideEnvDB = -60.0f;
    rideRefDB = -60.0f;
    rideRefSet = false;
    rideSmoothedOffset = 0.0f;
    rideOffsetComp.store(0.0f);
}

void GoldCompProcessor::releaseResources()
{
    compressor.reset(); limiter.reset();
    sigHpFilter.reset();
    oversampler.reset();
    compOS.reset();
}

bool GoldCompProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
}

float GoldCompProcessor::softClip(float sample, float amount)
{
    return softClipWithMode(sample, amount, currentClipMode);
}

float GoldCompProcessor::softClipWithMode(float sample, float amount, int /*mode*/)
{
    if (amount < 0.001f) return sample;
    float drive = 1.0f + amount * 4.0f;

    // Sophisticated asymmetric clipper:
    // - Positive half: soft exponential saturation (tube-like)
    // - Negative half: gentler tanh curve (less distortion)
    // - Generates even harmonics (2nd, 4th) → warm, musical character
    // - Frequency-dependent: bass clips softer via the drive scaling
    float x = sample * drive;
    float clipped;

    if (x > 0.0f) {
        // Tube-style positive: 1 - exp(-x) with gentle knee
        clipped = 1.0f - std::exp(-x);
    } else {
        // Gentler negative half: softer curve preserves asymmetry
        clipped = -std::tanh(-x * 0.7f);
    }

    // Normalize to preserve unity gain at drive level
    float normP = 1.0f - std::exp(-(float)drive);
    float normN = std::tanh((float)drive * 0.7f);
    float normFactor = std::max(normP, normN);
    if (normFactor > 0.001f) clipped /= normFactor;

    // Subtle 2nd harmonic sweetener — scales with amount
    // x² term adds even harmonics without changing the fundamental
    float harmonicAmount = amount * 0.06f;
    clipped += harmonicAmount * (clipped * std::abs(clipped) - clipped);

    return sample * (1.0f - amount) + clipped * amount;
}

void GoldCompProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    for (auto i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear(i, 0, buffer.getNumSamples());
    if (buffer.getNumChannels() < 2) return;

    int numSamples = buffer.getNumSamples();
    if (numSamples == 0) return;
    float* left = buffer.getWritePointer(0);
    float* right = buffer.getWritePointer(1);

    bool bypassed = apvts.getRawParameterValue("bypass")->load() > 0.5f;

    // Save dry input for bypass crossfade (stack buffer, max typical block size)
    constexpr int MAX_BUF = 2048;
    float dryL[MAX_BUF], dryR[MAX_BUF];
    int ns = juce::jmin(numSamples, MAX_BUF);
    if (bypassed != prevBypassed) {
        for (int i = 0; i < ns; ++i) { dryL[i] = left[i]; dryR[i] = right[i]; }
        // Reset oversampler on bypass toggle to prevent stale buffer artifacts
        compOS.reset();
    }

    float compDB   = -apvts.getRawParameterValue("comp")->load(); // param is 0-36, negate for processing
    // Remap: knob 0-36 → internal 0-29 (sweet spot at 29 is max effective compression)
    compDB = compDB * (29.0f / 36.0f);
    float gainDB   = apvts.getRawParameterValue("gain")->load();
    float hpfFreq  = apvts.getRawParameterValue("hpf")->load();
    float scHpFreq = apvts.getRawParameterValue("schpf")->load();
    float mixAmt   = apvts.getRawParameterValue("mix")->load() / 100.0f;

    // Input metering + K-weighted LUFS
    float inPL = 0, inPR = 0, inKSumSq = 0;
    for (int i = 0; i < numSamples; ++i) {
        inPL = std::max(inPL, std::abs(left[i]));
        inPR = std::max(inPR, std::abs(right[i]));
        float mono = (left[i] + right[i]) * 0.5f;
        // K-weight the mono signal
        float kFiltered = applyBiquad(mono, kShelfIn, kShelfCoeffs);
        kFiltered = applyBiquad(kFiltered, kHPIn, kHPCoeffs);
        inKSumSq += kFiltered * kFiltered;
    }
    inputPeakL.store(inPL); inputPeakR.store(inPR);
    float blockInLUFS = std::sqrt(inKSumSq / (float)numSamples);
    // Block-size independent smoothing: ~800ms time constant (slow enough for knob changes)
    float lufsSmooth = std::exp(-(float)numSamples / (float)(currentSampleRate * 0.800));
    smoothedInLUFS = smoothedInLUFS * lufsSmooth + blockInLUFS * (1.0f - lufsSmooth);
    inputRMS.store(smoothedInLUFS);

    // === Signal analysis for Sweet Spot ===
    // Improved algorithm: uses input level + crest factor to compute
    // the comp knob range that produces 3-8dB GR on vocal peaks.
    // Silence-gated to avoid noise floor corrupting measurements.
    {
        float blockRMS = 0.0f;
        float blockPeak = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            float mono = std::abs((left[i] + right[i]) * 0.5f);
            blockRMS += mono * mono;
            blockPeak = std::max(blockPeak, mono);
        }
        blockRMS = std::sqrt(blockRMS / (float)numSamples);
        float rmsDB = blockRMS > 1e-6f ? 20.0f * std::log10(blockRMS) : -60.0f;
        float peakDB = blockPeak > 1e-6f ? 20.0f * std::log10(blockPeak) : -60.0f;

        // Silence gate: only analyze blocks with vocal signal (> -35dB peak)
        // Breaths (-30 to -40dB) and pauses are ignored — sweet spot stays stable
        bool isActive = peakDB > -35.0f;

        analysisBlockCount++;
        int learnBlocks = (int)(currentSampleRate * 1.0 / (double)numSamples);
        float smoothCoeff = analysisBlockCount < learnBlocks ? 0.12f : 0.03f;

        if (isActive)
        {
            // Track peak and RMS of active signal separately
            // Peak uses asymmetric smoothing: fast attack, slow release (like a meter)
            if (peakDB > smoothedPeakDB)
                smoothedPeakDB = smoothedPeakDB * 0.7f + peakDB * 0.3f; // fast rise
            else
                smoothedPeakDB = smoothedPeakDB * (1.0f - smoothCoeff * 0.5f) + peakDB * smoothCoeff * 0.5f; // slow fall

            smoothedRMSDB = smoothedRMSDB * (1.0f - smoothCoeff) + rmsDB * smoothCoeff;

            float crestDB = smoothedPeakDB - smoothedRMSDB;
            crestDB = juce::jlimit(3.0f, 30.0f, crestDB);
            smoothedCrestDB = smoothedCrestDB * (1.0f - smoothCoeff) + crestDB * smoothCoeff;
        }

        inputRMSdB.store(rmsDB);
        inputDynamicRange.store(smoothedCrestDB);
        inputPeakDBSmoothed.store(smoothedPeakDB);

        // === Compute sweet spot from signal characteristics ===
        // The comp knob maps to:
        //   threshold = -6 - (comp/36) * 30
        //   ratio = 1 + (comp/36)^2 * 9
        // We reverse-engineer: for a given target GR on peaks, what comp value?
        //
        // Strategy: find comp values where estimated GR = target
        // ssLow  = comp where GR on peaks ≈ 3dB  (entering sweet spot)
        // ssHigh = comp where GR on peaks ≈ 8dB  (leaving sweet spot)

        float peakLevel = smoothedPeakDB;

        // Estimate GR for a given comp knob value against our measured peak level
        auto estimateGR = [&](float comp) -> float {
            float compAmt = comp / 36.0f;
            float thresh = -6.0f - (compAmt * 30.0f) - (compAmt * compAmt * 12.0f) - (compAmt * compAmt * compAmt * 6.0f);
            float ratio = 1.0f + compAmt * compAmt * 14.0f;  // matches MAX_RATIO - 1
            float kneeDB = 6.0f; // matches compressor knee
            float halfKnee = kneeDB / 2.0f;

            if (peakLevel < thresh - halfKnee)
                return 0.0f; // below knee — no compression
            else if (peakLevel > thresh + halfKnee)
                return (peakLevel - thresh) * (1.0f - 1.0f / ratio); // above knee
            else {
                // In knee region — smoothstep approximation
                float x = peakLevel - thresh + halfKnee;
                float t = x / kneeDB;
                float tSat = t * t * (3.0f - 2.0f * t);
                return tSat * (peakLevel - thresh) * (1.0f - 1.0f / ratio);
            }
        };

        // Search for comp values that give target GR amounts
        // Uses the actual compressor transfer function, not approximations
        float grTargetLow = 3.0f;   // entering sweet spot: gentle GR
        float grTargetHigh = 8.0f;  // leaving sweet spot: firm GR

        // Adjust targets based on crest factor:
        // High crest (dynamic vocal): slightly higher GR targets (more headroom to compress)
        // Low crest (already compressed): lower GR targets (less room before artifacts)
        float crestScale = juce::jlimit(0.8f, 1.3f, smoothedCrestDB / 12.0f);
        grTargetLow *= crestScale;
        grTargetHigh *= crestScale;

        // Find comp value for each target via linear search (0.5 step, cheap)
        float ssLowVal = 8.0f, ssHighVal = 22.0f; // defaults
        bool foundLow = false, foundHigh = false;
        for (float c = 0.5f; c <= 36.0f; c += 0.5f) {
            float gr = estimateGR(c);
            if (!foundLow && gr >= grTargetLow) {
                ssLowVal = c;
                foundLow = true;
            }
            if (!foundHigh && gr >= grTargetHigh) {
                ssHighVal = c;
                foundHigh = true;
            }
        }
        if (!foundHigh) ssHighVal = juce::jmin(ssLowVal + 10.0f, 34.0f);

        // Ensure minimum sweet spot width (at least 4dB range on knob)
        if (ssHighVal - ssLowVal < 4.0f)
            ssHighVal = ssLowVal + 4.0f;

        // Clamp to valid range
        ssLowVal = juce::jlimit(2.0f, 30.0f, ssLowVal);
        ssHighVal = juce::jlimit(6.0f, 34.0f, ssHighVal);

        // Smooth the output to prevent jitter
        float ssSmooth = analysisBlockCount < learnBlocks ? 0.08f : 0.018f;
        sweetSpotLow.store(sweetSpotLow.load() * (1.0f - ssSmooth) + ssLowVal * ssSmooth);
        sweetSpotHigh.store(sweetSpotHigh.load() * (1.0f - ssSmooth) + ssHighVal * ssSmooth);
    }

    // === RIDE: Auto-follow Sweet Spot ===
    // On activation: knob moves to Sweet Spot middle.
    // As input changes (verse→chorus), Sweet Spot shifts → knob follows.
    // Asymmetric: fast up (louder needs more comp), slow down (holds during breaths/pauses).
    if (rideMode.load() && !bypassed) {
        float ssLow = sweetSpotLow.load();
        float ssHigh = sweetSpotHigh.load();
        float ssMid = (ssLow + ssHigh) * 0.5f;

        // User's knob position
        float userComp = -compDB * (36.0f / 29.0f);

        // Target offset
        float targetOffset = ssMid - userComp;
        targetOffset = juce::jlimit(-18.0f, 18.0f, targetOffset);

        // Asymmetric smoothing:
        // Moving UP (more comp needed, louder passage) → fast (0.1s)
        // Moving DOWN (less comp, breath/pause) → slow (0.4s) with hold
        float direction = targetOffset - rideSmoothedOffset;
        float smoothTime;
        if (direction > 0.2f) {
            // Target is higher than current → need more comp → fast follow
            smoothTime = 0.1f;
        } else if (direction < -0.2f) {
            // Target is lower → could be breath/pause → slow, hold position
            smoothTime = 0.4f;
        } else {
            // Near target → gentle tracking
            smoothTime = 0.15f;
        }
        float offSmooth = std::exp(-(float)numSamples / (currentSampleRate * smoothTime));
        rideSmoothedOffset = rideSmoothedOffset * offSmooth + targetOffset * (1.0f - offSmooth);

        rideOffsetComp.store(rideSmoothedOffset);
    } else {
        // RIDE off: smoothly return to zero (~0.3 sec)
        if (std::abs(rideSmoothedOffset) > 0.01f) {
            float offSmooth = std::exp(-(float)numSamples / (currentSampleRate * 0.3));
            rideSmoothedOffset *= offSmooth;
            rideOffsetComp.store(rideSmoothedOffset);
        } else {
            rideSmoothedOffset = 0.0f;
            rideOffsetComp.store(0.0f);
        }
    }

    // Apply RIDE offset to compDB (scale to internal range)
    float rideOff = rideSmoothedOffset * (29.0f / 36.0f);
    compDB -= rideOff;
    compDB = juce::jlimit(-29.0f, 0.0f, compDB);

    // dlySize needed for dry/wet mix
    int dlySize = (int)dryDelayL.size();

    if (!bypassed)
    {
        // 0. Input Trim — per-sample interpolated to prevent zipper noise
        float inTrimDB = apvts.getRawParameterValue("inTrim")->load();
        float trimLin = std::pow(10.0f, inTrimDB / 20.0f);
        if (std::abs(trimLin - 1.0f) > 0.0001f || std::abs(prevTrimLin - 1.0f) > 0.0001f) {
            float trimDelta = (trimLin - prevTrimLin) / (float)numSamples;
            float curTrim = prevTrimLin;
            for (int i = 0; i < numSamples; ++i) {
                curTrim += trimDelta;
                left[i] *= curTrim;
                right[i] *= curTrim;
            }
        }
        prevTrimLin = trimLin;
        // Detect input clipping after trim
        bool inClip = false;
        for (int i = 0; i < numSamples; ++i) {
            if (std::abs(left[i]) >= 1.0f || std::abs(right[i]) >= 1.0f) { inClip = true; break; }
        }
        inputClipping.store(inClip);

        // 1. Signal HPF (coefficients cached — only recalc on change)
        if (hpfFreq > 1.0f) {
            if (std::abs(hpfFreq - lastHpfFreq) > 0.01f) {
                *sigHpFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentSampleRate, hpfFreq);
                lastHpfFreq = hpfFreq;
            }
            juce::dsp::AudioBlock<float> block(buffer);
            sigHpFilter.process(juce::dsp::ProcessContextReplacing<float>(block));
        }

        // (Sidechain HPF is handled internally by the compressor's detector)

        // (Input saturation removed — causes audible distortion on sub bass)

        // 2c. De-Click: catches transient artifacts and hard consonants
        // Two-stage approach:
        //   Stage 1: Slow RMS envelope tracks the "normal" level (~10ms)
        //   Stage 2: Anything exceeding that by >4dB gets soft-limited
        if (deClickMode.load()) {
            for (int i = 0; i < numSamples; ++i) {
                float absL = std::abs(left[i]);
                float absR = std::abs(right[i]);

                // Slow envelope: tracks sustained level, ignores transients
                // Attack ~2ms (fast enough to follow speech), Release ~15ms
                float slowAttack = dcRelCoeff;   // ~2ms (reuse rel coeff)
                float slowRelease = dcSlowRelCoeff;  // ~15ms

                dcEnvL = (absL > dcEnvL) ? slowAttack * dcEnvL + (1.0f - slowAttack) * absL
                                         : slowRelease * dcEnvL + (1.0f - slowRelease) * absL;
                dcEnvR = (absR > dcEnvR) ? slowAttack * dcEnvR + (1.0f - slowAttack) * absR
                                         : slowRelease * dcEnvR + (1.0f - slowRelease) * absR;

                // Ceiling: 4dB above slow envelope (~1.6x)
                float ceilL = juce::jmax(dcEnvL * 1.6f, 0.002f);
                float ceilR = juce::jmax(dcEnvR * 1.6f, 0.002f);

                // Hard soft-clip: tanh with steep curve
                if (absL > ceilL) {
                    float over = (absL - ceilL) / (ceilL * 0.5f);
                    left[i] = (left[i] > 0 ? 1.0f : -1.0f) * (ceilL + std::tanh(over) * ceilL * 0.15f);
                }
                if (absR > ceilR) {
                    float over = (absR - ceilR) / (ceilR * 0.5f);
                    right[i] = (right[i] > 0 ? 1.0f : -1.0f) * (ceilR + std::tanh(over) * ceilR * 0.15f);
                }
            }
        }

        // Write dry signal into latency-compensated delay AFTER all pre-processing
        // This ensures dry and wet match in level, frequency content, and phase
        for (int i = 0; i < numSamples; ++i) {
            dryDelayL[dryDelayWritePos] = left[i];
            dryDelayR[dryDelayWritePos] = right[i];
            dryDelayWritePos = (dryDelayWritePos + 1) % dlySize;
        }

        // 3. Compressor — internally optimized timing (no user attack/release)
        int boostMode = 0;
        compressor.smoothAttack = true;
        float gateThresh = apvts.getRawParameterValue("gate")->load();

        // Attack/Release driven by comp amount — like RVox, always optimal
        // Fast attack + RMS detector + lookahead = consonants preserved naturally
        float compAmt01 = juce::jlimit(0.0f, 1.0f, -compDB / 29.0f);  // 0→1 (internal range)
        float attackMs = 0.1f;  // near-instant, lookahead handles smoothing
        float relFastMs = 40.0f + (1.0f - compAmt01) * 40.0f;  // 40-80ms: tighter at high comp
        float relSlowMs = 400.0f + (1.0f - compAmt01) * 600.0f;  // 400-1000ms: shorter at high comp
        float kneeW = 6.0f;
        compressor.setAttackTime(attackMs / 1000.0f);
        compressor.setReleaseTimes(relFastMs / 1000.0f, relSlowMs / 1000.0f);
        compressor.userKneeWidth = kneeW;
        compressor.maxGainReductionDB = 36.0f;
        compressor.ratioMultiplier = 1.0f;

        // Sidechain HPF: filters detector only, bass passes through to output
        compressor.setScHpfFreq(scHpFreq);

        // Delta mode: compressor outputs what compression removes instead of compressed signal
        bool isDelta = deltaMode.load() && compDB < -0.5f;
        compressor.outputDelta = isDelta;  // compressor outputs delayed*(1-gain) when true

        // Compressor — fixed 2x oversampling for alias-free gain modulation
        {
            juce::dsp::AudioBlock<float> block(buffer);
            auto osBlock = compOS.processSamplesUp(block);
            int osN = (int)osBlock.getNumSamples();
            float* osL = osBlock.getChannelPointer(0);
            float* osR = osBlock.getChannelPointer(1);
            compressor.process(osL, osR, osN, compDB, gateThresh, boostMode);
            compOS.processSamplesDown(block);
        }

        if (isDelta)
        {
            // === DELTA MODE ===
            // Compressor already output what compression removes (delayed*(1-gain))
            // Run limiter for consistent latency (won't actually limit — delta is quiet)
            limiter.process(left, right, numSamples, 0.0f);
            // Skip makeup, clipper, gain match, mix — pure compression delta
        }
        else
        {
            // === NORMAL OUTPUT PATH ===

            // 4. Makeup gain — linear with gentle taper above 18dB
            // Full range compression: keeps getting louder all the way to 36
            float rawMakeup = -compDB;  // 0 to 36
            float makeupDB;
            if (rawMakeup <= 18.0f) {
                makeupDB = rawMakeup * 1.15f;  // linear region: 0→20.7dB
            } else {
                // Gentle taper: pow(0.75) above 18dB — still rises meaningfully
                float excess = rawMakeup - 18.0f;  // 0→18
                makeupDB = 18.0f * 1.15f + std::pow(excess / 18.0f, 0.75f) * 18.0f * 1.0f;
                // At 18dB: 20.7dB. At 36dB: 20.7 + 18.0 = 38.7dB
            }
            float makeupLin = std::pow(10.0f, makeupDB / 20.0f);
            {
                float mkDelta = (makeupLin - prevMakeupLin) / (float)numSamples;
                float curMk = prevMakeupLin;
                for (int i = 0; i < numSamples; ++i) {
                    curMk += mkDelta;
                    left[i] *= curMk;
                    right[i] *= curMk;
                }
            }
            prevMakeupLin = makeupLin;

            // 5. Limiter
            limiter.process(left, right, numSamples, gainDB);

            // 5b. Safety soft-clip — multi-stage soft knee, more transparent than single tanh
            // Starts gently at 75% ceiling, full limiting at 100%
            {
                float ceiling = std::pow(10.0f, gainDB / 20.0f);
                float knee = ceiling * 0.25f;       // 25% knee width
                float kneeStart = ceiling - knee;   // starts at 75% ceiling
                for (int i = 0; i < numSamples; ++i) {
                    for (float* ch : { &left[i], &right[i] }) {
                        float absVal = std::abs(*ch);
                        if (absVal > kneeStart) {
                            float sign = (*ch > 0) ? 1.0f : -1.0f;
                            float over = (absVal - kneeStart) / knee;  // 0→1→∞
                            // Soft curve: approaches ceiling asymptotically
                            *ch = sign * (kneeStart + knee * (1.0f - 1.0f / (1.0f + over)));
                        }
                    }
                }
            }

            // 6. Soft Clipper (adds harmonic saturation)
            float clipAmt = apvts.getRawParameterValue("clip")->load() / 100.0f;
            if (clipAmt > 0.001f) {
                // Measure RMS before clipping
                float preClipRMS = 0.0f;
                for (int i = 0; i < numSamples; ++i)
                    preClipRMS += left[i] * left[i] + right[i] * right[i];
                preClipRMS = std::sqrt(preClipRMS / (float)(numSamples * 2));

                juce::dsp::AudioBlock<float> block(buffer);
                auto osBlock = oversampler.processSamplesUp(block);
                int osN = (int)osBlock.getNumSamples();
                float totalDistortion = 0.0f;
                int distCount = 0;
                for (int ch = 0; ch < 2; ++ch) {
                    float* d = osBlock.getChannelPointer((size_t)ch);
                    for (int i = 0; i < osN; ++i) {
                        float before = d[i];
                        d[i] = softClip(d[i], clipAmt);
                        if (std::abs(before) > 0.05f) {
                            // Measure nonlinearity: how much did the waveshape change?
                            float diff = std::abs(d[i] - before) / std::abs(before);
                            totalDistortion += diff;
                            distCount++;
                        }
                    }
                }
                oversampler.processSamplesDown(block);
                float avgDist = distCount > 0 ? totalDistortion / (float)distCount : 0.0f;
                clipActivity.store(juce::jlimit(0.0f, 1.0f, avgDist * 8.0f));

                // Gain compensation: match post-clip RMS to pre-clip RMS
                float postClipRMS = 0.0f;
                for (int i = 0; i < numSamples; ++i)
                    postClipRMS += left[i] * left[i] + right[i] * right[i];
                postClipRMS = std::sqrt(postClipRMS / (float)(numSamples * 2));

                if (postClipRMS > 1e-8f) {
                    float compGain = preClipRMS / postClipRMS;
                    compGain = juce::jlimit(0.5f, 1.5f, compGain);  // safety
                    for (int i = 0; i < numSamples; ++i) {
                        left[i] *= compGain;
                        right[i] *= compGain;
                    }
                }
            } else {
                clipActivity.store(0.0f);
            }

            // 7. Measure output LUFS BEFORE gain match (prevents feedback loop)
            {
                float preMatchKSumSq = 0;
                for (int i = 0; i < numSamples; ++i) {
                    float mono = (left[i] + right[i]) * 0.5f;
                    float kFiltered = applyBiquad(mono, kShelfOut, kShelfCoeffs);
                    kFiltered = applyBiquad(kFiltered, kHPOut, kHPCoeffs);
                    preMatchKSumSq += kFiltered * kFiltered;
                }
                float preMatchLUFS = std::sqrt(preMatchKSumSq / (float)numSamples);

                // Slow smoothing: 800ms — survives rapid knob movement
                float outLufsSmooth = std::exp(-(float)numSamples / (float)(currentSampleRate * 0.800));
                smoothedOutLUFS = smoothedOutLUFS * outLufsSmooth + preMatchLUFS * (1.0f - outLufsSmooth);

                // Only update offset when both signals are above noise floor
                float inLevelDB = (smoothedInLUFS > 1e-10f) ? 20.0f * std::log10(smoothedInLUFS) : -100.0f;
                float outLevelDB = (smoothedOutLUFS > 1e-10f) ? 20.0f * std::log10(smoothedOutLUFS) : -100.0f;
                if (inLevelDB > -50.0f && outLevelDB > -50.0f) {
                    float diffDB = 20.0f * std::log10(smoothedInLUFS / smoothedOutLUFS);
                    diffDB = juce::jlimit(-18.0f, 6.0f, diffDB);
                    // Slew-limit: max 0.5dB per 50ms — ultra-smooth even during fast knob turns
                    float prevOffset = gainMatchOffsetDB.load();
                    float blockTimeMs = (float)numSamples / (float)currentSampleRate * 1000.0f;
                    float maxDelta = 0.5f * blockTimeMs / 50.0f;
                    diffDB = juce::jlimit(prevOffset - maxDelta, prevOffset + maxDelta, diffDB);
                    gainMatchOffsetDB.store(diffDB);
                } else {
                    float prevOffset = gainMatchOffsetDB.load();
                    float fadeCoeff = std::exp(-(float)numSamples / (float)(currentSampleRate * 0.100f));
                    gainMatchOffsetDB.store(prevOffset * fadeCoeff);
                }
            }

            // 8. Apply gain match (HONEST mode) — per-sample interpolated with slew limit
            if (gainMatchEnabled.load() || honestMode.load()) {
                float offsetDB = gainMatchOffsetDB.load();
                // Slew-limit in dB: max 0.5dB per 50ms — completely inaudible
                float prevDB = (prevOffsetLin > 1e-10f) ? 20.0f * std::log10(prevOffsetLin) : 0.0f;
                float blockTimeMs = (float)numSamples / (float)currentSampleRate * 1000.0f;
                float maxDeltaDB = 0.5f * blockTimeMs / 50.0f;
                offsetDB = juce::jlimit(prevDB - maxDeltaDB, prevDB + maxDeltaDB, offsetDB);
                float offsetLin = std::pow(10.0f, offsetDB / 20.0f);
                float offDelta = (offsetLin - prevOffsetLin) / (float)numSamples;
                float curOff = prevOffsetLin;
                for (int i = 0; i < numSamples; ++i) {
                    curOff += offDelta;
                    left[i] *= curOff;
                    right[i] *= curOff;
                }
                prevOffsetLin = offsetLin;

                // Safety: soft-clip after HONEST to prevent distortion from boosting
                if (offsetLin > 1.01f) {
                    float ceil = std::pow(10.0f, gainDB / 20.0f);
                    float ct = ceil * 0.92f;
                    for (int i = 0; i < numSamples; ++i) {
                        for (float* ch : { &left[i], &right[i] }) {
                            float a = std::abs(*ch);
                            if (a > ct) {
                                float s = (*ch > 0) ? 1.0f : -1.0f;
                                float o = (a - ct) / (ceil * 0.08f);
                                *ch = s * (ct + std::tanh(o) * ceil * 0.08f);
                            }
                        }
                    }
                }
            } else {
                prevOffsetLin = 1.0f;
                gainMatchOffsetDB.store(0.0f);  // reset so HONEST starts clean
            }

            // 9. Latency-compensated Dry/Wet Mix — per-sample interpolated
            int effectiveWetLatency = compressor.getLatencySamples() / 2
                                   + (int)compOS.getLatencyInSamples()
                                   + limiter.getLatencySamples();

            if (mixAmt < 0.999f || prevMixWet < 0.999f) {
                float mixDelta = (mixAmt - prevMixWet) / (float)numSamples;
                float curWet = prevMixWet;
                int writeBase = (dryDelayWritePos - numSamples + dlySize * 2) % dlySize;
                for (int i = 0; i < numSamples; ++i) {
                    curWet += mixDelta;
                    float wet = curWet, dry = 1.0f - curWet;
                    int readPos = (writeBase + i - effectiveWetLatency + dlySize * 2) % dlySize;
                    left[i]  = dryDelayL[(size_t)readPos] * dry + left[i] * wet;
                    right[i] = dryDelayR[(size_t)readPos] * dry + right[i] * wet;
                }
            }
            prevMixWet = mixAmt;
        }
    }
    else
    {
        // Bypassed: delay signal by totalWetLatency to match DAW's latency compensation
        // Without this, bypassed signal arrives early vs other tracks
        int dlyS = (int)dryDelayL.size();
        for (int i = 0; i < numSamples; ++i) {
            dryDelayL[(size_t)dryDelayWritePos] = left[i];
            dryDelayR[(size_t)dryDelayWritePos] = right[i];
            int readPos = (dryDelayWritePos - totalWetLatency + dlyS * 2) % dlyS;
            left[i] = dryDelayL[(size_t)readPos];
            right[i] = dryDelayR[(size_t)readPos];
            dryDelayWritePos = (dryDelayWritePos + 1) % dlyS;
        }
    }

    // DC Blocker — always on, catches DC from input saturation and any processing
    // 1st-order HPF at ~5Hz: y[n] = x[n] - x[n-1] + R * y[n-1]
    {
        for (int i = 0; i < numSamples; ++i) {
            float outL = left[i] - dcPrevInL + dcBlockCoeff * dcBlockL;
            float outR = right[i] - dcPrevInR + dcBlockCoeff * dcBlockR;
            dcPrevInL = left[i]; dcPrevInR = right[i];
            dcBlockL = outL; dcBlockR = outR;
            left[i] = outL; right[i] = outR;
        }
    }

    // Bypass crossfade: smooth transition when toggling bypass
    if (bypassed != prevBypassed) {
        for (int i = 0; i < ns; ++i) {
            float t = (float)i / (float)ns;
            // Cosine crossfade: smooth S-curve
            float fade = 0.5f * (1.0f - std::cos(t * juce::MathConstants<float>::pi));
            if (bypassed) {
                // Fading TO bypass: processed → dry
                left[i]  = left[i] * (1.0f - fade) + dryL[i] * fade;
                right[i] = right[i] * (1.0f - fade) + dryR[i] * fade;
            } else {
                // Fading FROM bypass: dry → processed
                left[i]  = dryL[i] * (1.0f - fade) + left[i] * fade;
                right[i] = dryR[i] * (1.0f - fade) + right[i] * fade;
            }
        }
        prevBypassed = bypassed;
    }

    // Output peak metering (LUFS already measured pre-match above)
    float outPL = 0, outPR = 0;
    for (int i = 0; i < numSamples; ++i) {
        outPL = std::max(outPL, std::abs(left[i]));
        outPR = std::max(outPR, std::abs(right[i]));
    }
    outputPeakL.store(outPL); outputPeakR.store(outPR);
    outputClipping.store(outPL > 0.999f || outPR > 0.999f);
    outputRMS.store(smoothedOutLUFS);

    compGainReductionDB.store(bypassed ? 0.0f : compressor.getGainReductionDB());
    limiterGainReductionDB.store(bypassed ? 0.0f : limiter.getGainReductionDB());
}

juce::AudioProcessorEditor* GoldCompProcessor::createEditor() { return new GoldCompEditor(*this); }

void GoldCompProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void GoldCompProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new GoldCompProcessor(); }

// ===== K-Weighting Filters for LUFS (ITU-R BS.1770) =====
void GoldCompProcessor::computeKWeightingCoeffs(double sampleRate)
{
    // Stage 1: Pre-filter (high shelf, ~+4dB above 1681Hz)
    // Derived from ITU-R BS.1770-4 specification
    {
        double Vh = std::pow(10.0, 3.999843853973347 / 20.0);
        double Vb = std::pow(Vh, 0.4996667741545416);
        double fc = 1681.974450955533;
        double Q  = 0.7071752369554196;
        double K  = std::tan(juce::MathConstants<double>::pi * fc / sampleRate);
        double K2 = K * K;
        double a0 = 1.0 + K / (Q * Vb) + K2;
        kShelfCoeffs.b0 = (float)((Vh + Vb * K / Q + K2) / a0);
        kShelfCoeffs.b1 = (float)((2.0 * (K2 - Vh)) / a0);
        kShelfCoeffs.b2 = (float)((Vh - Vb * K / Q + K2) / a0);
        kShelfCoeffs.a1 = (float)((2.0 * (K2 - 1.0)) / a0);
        kShelfCoeffs.a2 = (float)((1.0 - K / (Q * Vb) + K2) / a0);
    }

    // Stage 2: RLB weighting (high-pass, ~38Hz)
    {
        double fc = 38.13547087602444;
        double Q  = 0.5003270373238773;
        double K  = std::tan(juce::MathConstants<double>::pi * fc / sampleRate);
        double K2 = K * K;
        double a0 = 1.0 + K / Q + K2;
        kHPCoeffs.b0 = (float)(1.0 / a0);
        kHPCoeffs.b1 = (float)(-2.0 / a0);
        kHPCoeffs.b2 = (float)(1.0 / a0);
        kHPCoeffs.a1 = (float)((2.0 * (K2 - 1.0)) / a0);
        kHPCoeffs.a2 = (float)((1.0 - K / Q + K2) / a0);
    }
}

float GoldCompProcessor::applyBiquad(float x, BiquadState& s, const BiquadCoeffs& c)
{
    float y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2 - c.a1 * s.y1 - c.a2 * s.y2;
    s.x2 = s.x1; s.x1 = x;
    s.y2 = s.y1; s.y1 = y;
    return y;
}
