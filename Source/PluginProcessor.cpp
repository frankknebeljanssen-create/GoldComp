#include "PluginProcessor.h"
#include "PluginEditor.h"

SmartCompProcessor::SmartCompProcessor()
    : AudioProcessor(BusesProperties()
                     .withInput("Input", juce::AudioChannelSet::stereo(), true)
                     .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, &undoManager, "Parameters", createParameterLayout())
{
}

SmartCompProcessor::~SmartCompProcessor() {}

juce::AudioProcessorValueTreeState::ParameterLayout SmartCompProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Only parameters the processor actually reads. Eleven others used to be
    // registered here — clipmode, lookahead, boost, attackMs, relFastMs,
    // relSlowMs, kneeW, grRange, oversample, ratioMult, transProtect — none of
    // which were ever read: processBlock hardcoded their effects. They still
    // showed up in the host's automation list and got saved into sessions, so a
    // user could automate a control that did nothing.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID("bypass", 1), "Bypass", false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("comp", 1), "Compression",
        juce::NormalisableRange<float>(0.0f, 36.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("gain", 1), "Out Gain",
        juce::NormalisableRange<float>(-24.0f, 12.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("hpf", 1), "HP Filter",
        juce::NormalisableRange<float>(0.0f, 300.0f, 1.0f, 0.4f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("schpf", 1), "SC HP Filter",
        juce::NormalisableRange<float>(0.0f, 400.0f, 1.0f, 0.4f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("clip", 1), "Soft Clip",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("mix", 1), "Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 100.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("gate", 1), "Gate",
        juce::NormalisableRange<float>(-80.0f, -20.0f, 0.1f), -80.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("inTrim", 1), "In Trim",
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f));

    return { params.begin(), params.end() };
}

void SmartCompProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    // The compressor is fed the 2x oversampled block, so it must be prepared at
    // that rate. It was being given the host rate, which halved every time
    // constant (50 ms RMS window ran as 25 ms, releases at half their labels) and
    // put every detector filter an octave high — the SC HP Filter knob read
    // 100 Hz and delivered 200 Hz. Latency accounting already accounted for the
    // 2x domain; only the coefficients were missed.
    compressor.prepare(sampleRate * compOSFactor, samplesPerBlock * compOSFactor);
    limiter.prepare(sampleRate, samplesPerBlock);   // limiter runs at base rate

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32)samplesPerBlock, 2 };
    // Allocate the coefficients object once, here, where allocation is allowed.
    // processBlock then only ever writes into it in place.
    sigHpFilter.state = juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, 100.0f);
    sigHpFilter.prepare(spec); sigHpFilter.reset();

    // Integer latency: without this getLatencyInSamples() returns a fractional
    // group delay (3.137 and 4.433 samples here) which the (int) cast below
    // silently truncated, leaving the dry/wet mix misaligned by a fraction of a
    // sample. JUCE inserts the fractional part itself when asked to.
    oversampler.setUsingIntegerLatency(true);
    oversampler.initProcessing((size_t)samplesPerBlock);
    oversampler.reset();
    compOS.setUsingIntegerLatency(true);
    compOS.initProcessing((size_t)samplesPerBlock);
    compOS.reset();

    // Wet path latency. The clipper's 4x oversampler was missing from this sum,
    // and because it only ran when Clip was up, engaging Clip lengthened the wet
    // path by 4.43 samples while the plugin kept reporting the old figure —
    // measured as a full null at 4.8 kHz once Mix was pulled back. It runs
    // unconditionally now, so its latency is constant and counted.
    totalWetLatency = compressor.getLatencySamples() / compOSFactor
                    + (int)compOS.getLatencyInSamples()
                    + (int)oversampler.getLatencyInSamples()
                    + limiter.getLatencySamples();
    setLatencySamples(totalWetLatency);

    // Dry delay for latency-compensated mix
    preparedBlockSize = samplesPerBlock;
    int delaySize = totalWetLatency + samplesPerBlock + 16;
    dryDelayL.assign(delaySize, 0.0f);
    dryDelayR.assign(delaySize, 0.0f);
    dryDelayWritePos = 0;
    bypassDelayL.assign(delaySize, 0.0f);
    bypassDelayR.assign(delaySize, 0.0f);
    bypassDelayWritePos = 0;
    // Crossfade scratch, preallocated: it used to be a 2048-sample stack array,
    // so on a larger block the fade simply stopped after 2048 samples and the
    // output jumped.
    fadeFromL.assign((size_t)samplesPerBlock, 0.0f);
    fadeFromR.assign((size_t)samplesPerBlock, 0.0f);
    monoScratch.assign((size_t)samplesPerBlock, 0.0f);

    // Force the signal HPF to redesign its coefficients. Without this the knob
    // deadband below keeps the *previous* sample rate's coefficients, so after a
    // rate change a 300 Hz setting actually sits at 653 Hz (44.1k -> 96k) until
    // the user happens to nudge the knob.
    lastHpfFreq = -1.0f;

    // Ramp and filter state: initialised at construction but previously never
    // re-initialised here, so a re-prepare replayed stale gain and DC state.
    prevTrimLin = 1.0f;
    prevMakeupLin = 1.0f;
    prevOffsetLin = 1.0f;
    prevMixWet = 1.0f;
    prevOutTrimLin = 1.0f;
    smoothedMakeupGR = 0.0f;
    smoothedHpfFreq = 0.0f;
    smoothedClipAmt = 0.0f;
    dcBlockL = dcBlockR = dcPrevInL = dcPrevInR = 0.0f;
    prevBypassed = false;

    smoothedInMS = 0.0f;
    smoothedOutMS = 0.0f;
    smoothedInLUFS = 0.0f;
    smoothedOutLUFS = 0.0f;
    smoothedPeakDB = -60.0f;
    smoothedRMSDB = -60.0f;
    smoothedCrestDB = 12.0f;
    analysisBlockCount = 0;
    dcBlockCoeff = 1.0f - (float)(2.0 * juce::MathConstants<double>::pi * 5.0 / sampleRate); // ~5Hz HPF
    kShelfIn = {}; kHPIn = {}; kShelfOut = {}; kHPOut = {};
    computeKWeightingCoeffs(sampleRate);


    // RIDE: auto-leveling coefficients
    rideEnvCoeff = std::exp(-(float)1.0f / (float)(sampleRate * 2.0));     // ~2 sec RMS tracking
    rideOffsetSmoothCoeff = std::exp(-1.0f / (float)(sampleRate * 0.5));   // ~0.5 sec smooth movement
    rideEnvDB = -60.0f;
    rideRefDB = -60.0f;
    rideRefSet = false;
    rideSmoothedOffset = 0.0f;
    rideOffsetComp.store(0.0f);
}

void SmartCompProcessor::releaseResources()
{
    compressor.reset(); limiter.reset();
    sigHpFilter.reset();
    oversampler.reset();
    compOS.reset();
}

bool SmartCompProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in.isDisabled() || out.isDisabled())
        return false;
    // Mono is allowed now. Stereo-only meant Logic would not offer the AU on
    // mono tracks, which is the primary case for a vocal compressor.
    const bool inOK  = in  == juce::AudioChannelSet::mono() || in  == juce::AudioChannelSet::stereo();
    const bool outOK = out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
    // Never fewer output channels than input
    return inOK && outOK && out.size() >= in.size();
}

// Writes RBJ high-pass coefficients into an existing Coefficients object.
// Allocation-free, so it is safe to call from processBlock.
void SmartCompProcessor::setHighPassCoefficients(juce::dsp::IIR::Coefficients<float>& c,
                                                double sampleRate, float freq)
{
    const double w0 = 2.0 * juce::MathConstants<double>::pi
                    * juce::jlimit(1.0, sampleRate * 0.49, (double)freq) / sampleRate;
    const double cosW0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * 0.70710678118654752);  // Q = 1/sqrt(2)
    const double a0 = 1.0 + alpha;

    // JUCE stores coefficients as { b0, b1, b2, a1, a2 }, already normalised by a0
    auto& raw = c.coefficients;
    raw.getReference(0) = (float)(((1.0 + cosW0) * 0.5) / a0);
    raw.getReference(1) = (float)((-(1.0 + cosW0)) / a0);
    raw.getReference(2) = raw.getReference(0);
    raw.getReference(3) = (float)((-2.0 * cosW0) / a0);
    raw.getReference(4) = (float)((1.0 - alpha) / a0);
}

// Asymmetric tube-style saturator, normalised so that small signals pass at
// unity gain.
//
// The old normalisation divided by the curve's *asymptote* rather than its slope
// at zero, which made the whole stage a gain control: at Clip 100% a -60 dBFS
// signal came out +13.4 dB louder, with 3.1 dB of asymmetry between half-cycles
// and -26.7 dBc of distortion on a -20 dBFS input. Turning the knob up mostly
// meant turning the level up, and the block-rate RMS rematch that tried to hide
// that introduced its own steps of up to 3.8 dB.
//
// Both branches are now scaled to slope 1 at the origin, so the curve is linear
// for quiet material and only bends where it should. The two halves still differ
// in curvature — that asymmetry is what generates the even harmonics — but they
// now agree in gain, so there is no discontinuity at the zero crossing.
float SmartCompProcessor::softClipNormalized(float sample, float amount,
                                            float drive, float slopeNorm)
{
    float x = sample * drive;
    float shaped;
    if (x > 0.0f) {
        // d/dx[1 - exp(-x)] = 1 at x = 0
        shaped = 1.0f - std::exp(-x);
    } else {
        // d/dx[-tanh(-kx)/k] = 1 at x = 0, with k = 0.7 for a gentler negative half
        static constexpr float k = 0.7f;
        shaped = -std::tanh(-x * k) / k;
    }
    float clipped = shaped * slopeNorm;   // slopeNorm = 1/drive → unity at origin
    return sample * (1.0f - amount) + clipped * amount;
}

void SmartCompProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    if (buffer.getNumChannels() < 1) return;

    // Mono in, stereo out: mirror channel 0 rather than leaving channel 1 silent.
    // The stock "clear the extra outputs" idiom would leave the detector summing
    // signal against silence and reading 6 dB low.
    if (getTotalNumInputChannels() < 2 && buffer.getNumChannels() >= 2)
        buffer.copyFrom(1, 0, buffer, 0, 0, buffer.getNumSamples());
    else
        for (auto i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
            buffer.clear(i, 0, buffer.getNumSamples());

    int numSamples = buffer.getNumSamples();
    if (numSamples == 0) return;
    // Everything downstream is sized for the block size prepareToPlay was given.
    // A host handing us more would overrun the oversamplers' internal buffers,
    // which JUCE only guards with a jassert that compiles out in Release.
    numSamples = juce::jmin(numSamples, preparedBlockSize);
    // The chain is stereo throughout. On a mono track we mirror the single
    // channel into scratch, process as dual mono, and hand back channel 0 —
    // cheaper and far less error-prone than making every stage channel-count
    // aware, and the result is identical because both sides see the same input.
    const bool isMono = buffer.getNumChannels() < 2;
    float* left  = buffer.getWritePointer(0);
    float* right = nullptr;
    if (isMono) {
        if ((int)monoScratch.size() < numSamples) return;   // not prepared yet
        std::copy(left, left + numSamples, monoScratch.begin());
        right = monoScratch.data();
    } else {
        right = buffer.getWritePointer(1);
    }

    // Sanitize the input before anything recursive touches it. A single NaN or
    // Inf sample would otherwise poison the envelope followers, DC blocker and
    // LUFS accumulators permanently — they are pure IIR state with no way back,
    // so the plugin would stay silent until it is reloaded. The clamp is far
    // above any real signal (1e6 ~ +120 dBFS) and only exists to keep squares
    // and makeup gain inside float range.
    for (int i = 0; i < numSamples; ++i) {
        if (! std::isfinite(left[i]))  left[i]  = 0.0f;
        if (! std::isfinite(right[i])) right[i] = 0.0f;
        left[i]  = juce::jlimit(-1.0e6f, 1.0e6f, left[i]);
        right[i] = juce::jlimit(-1.0e6f, 1.0e6f, right[i]);
    }

    bool bypassed = apvts.getRawParameterValue("bypass")->load() > 0.5f;

    // Raw input into the bypass delay line, always, so the bypass path and the
    // crossfade always have latency-aligned material available regardless of
    // which state we were in last block.
    {
        const int bdSize = (int)bypassDelayL.size();
        for (int i = 0; i < numSamples; ++i) {
            bypassDelayL[(size_t)bypassDelayWritePos] = left[i];
            bypassDelayR[(size_t)bypassDelayWritePos] = right[i];
            bypassDelayWritePos = (bypassDelayWritePos + 1) % bdSize;
        }
    }

    // Crossfade source: the delayed raw input, i.e. exactly what the bypassed
    // path outputs. It used to be the *undelayed* input, so the fade mixed two
    // signals 67 samples apart — a comb sweep — and then ended on the undelayed
    // one, so the next block jumped back by that much. A click either way.
    const int ns = juce::jmin(numSamples, (int)fadeFromL.size());
    if (bypassed != prevBypassed) {
        const int bdSize = (int)bypassDelayL.size();
        const int base = (bypassDelayWritePos - numSamples + bdSize * 2) % bdSize;
        for (int i = 0; i < ns; ++i) {
            int rp = (base + i - totalWetLatency + bdSize * 2) % bdSize;
            fadeFromL[(size_t)i] = bypassDelayL[(size_t)rp];
            fadeFromR[(size_t)i] = bypassDelayR[(size_t)rp];
        }
        // Clear stale oversampler state so re-engaging does not dump it into the
        // signal. Both need it, not just the compressor's.
        compOS.reset();
        oversampler.reset();
    }

    // Knob 0-36, negated for processing. There used to be a 29/36 remap here
    // while the compressor divided by 36 internally, so the knob only ever
    // reached 80% of the designed range: max ratio came out at 10:1 instead of
    // 15:1. With the auto threshold the remap has no purpose either way.
    float compDB   = -apvts.getRawParameterValue("comp")->load();
    // Out Gain is a true output level now, not the limiter ceiling. Using it as
    // the ceiling meant turning it down made the signal quieter *and* flatter
    // (12.8 dB of limiting at 0, 24.8 dB at -12), so there was no way out of
    // permanent limiting. The ceiling is fixed just below full scale instead.
    float outTrimDB = apvts.getRawParameterValue("gain")->load();
    static constexpr float CEILING_DB = -0.3f;
    float hpfFreq  = apvts.getRawParameterValue("hpf")->load();
    float scHpFreq = apvts.getRawParameterValue("schpf")->load();
    float mixAmt   = apvts.getRawParameterValue("mix")->load() / 100.0f;
    float inTrimDB = apvts.getRawParameterValue("inTrim")->load();

    // Smooth the two parameters that otherwise change discontinuously at block
    // boundaries: the HPF swaps coefficients on a stateful IIR, and Clip feeds
    // both a waveshaper drive and a wet/dry blend. ~30 ms turns a sweep into a
    // glide instead of a staircase.
    {
        const float pSmooth = std::exp(-(float)numSamples / (float)(currentSampleRate * 0.030));
        smoothedHpfFreq = smoothedHpfFreq * pSmooth + hpfFreq * (1.0f - pSmooth);
        if (! std::isfinite(smoothedHpfFreq)) smoothedHpfFreq = hpfFreq;
        // Snap to the endpoints so "off" is really off and the top of the range
        // is reachable
        if (std::abs(hpfFreq - smoothedHpfFreq) < 0.05f) smoothedHpfFreq = hpfFreq;
        hpfFreq = smoothedHpfFreq;
    }

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
    // Integrate MEAN SQUARE, not amplitude. Smoothing sqrt(mean-square) under-
    // reads, and because sqrt is concave the deficit grows with inter-block level
    // variance — so it under-read the dynamic input more than the compressed
    // output, and unlike a filter-shape error that bias does not cancel between
    // the two chains. HONEST was under-matching by up to 0.66 dB, more the harder
    // the compression, which is the opposite of what the mode promises.
    float blockInMS = inKSumSq / (float)numSamples;
    // Block-size independent smoothing: ~800ms time constant (slow enough for knob changes)
    float lufsSmooth = std::exp(-(float)numSamples / (float)(currentSampleRate * 0.800));
    smoothedInMS = smoothedInMS * lufsSmooth + blockInMS * (1.0f - lufsSmooth);
    if (! std::isfinite(smoothedInMS) || smoothedInMS < 0.0f) smoothedInMS = 0.0f;
    // Input is measured before In Trim, but trim is a pure gain, so scaling the
    // result is exact and avoids HONEST fighting the user's trim: pull trim down
    // and it used to try to put up to 6 dB back.
    smoothedInLUFS = std::sqrt(smoothedInMS) * std::pow(10.0f, inTrimDB / 20.0f);
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

        if (analysisBlockCount < 1000000) analysisBlockCount++;
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

        // The threshold is referenced to the programme level now, so distance
        // above it no longer depends on the absolute input level — only on the
        // knob's depth setting plus how far the detector's own peaks rise above
        // its average. That makes the estimate level-independent.
        //
        // This used to compare an absolute threshold against smoothedPeakDB,
        // which was wrong twice over: it used a divisor the compressor did not
        // use, and it fed a peak level into a curve the compressor drives from
        // an RMS-dominant detector. It predicted 20.6 dB of reduction where the
        // real value was 2.9 dB.
        //
        // The detector is RMS-dominant (peak blend 0.08-0.20), so it sees far
        // less crest than the waveform does; the 0.4 factor is an approximation
        // of that, not a derived constant.
        float detectorCrest = juce::jlimit(2.0f, 8.0f, smoothedCrestDB * 0.4f);

        auto estimateGR = [&](float comp) -> float {
            float compAmt = comp / 36.0f;
            float depth = -6.0f + compAmt * 30.0f;          // matches the compressor
            float ratio = 1.0f + compAmt * compAmt * 14.0f; // matches MAX_RATIO - 1
            float aboveThresh = depth + detectorCrest;
            float kneeDB = 6.0f;                            // matches compressor knee
            float halfKnee = kneeDB / 2.0f;

            if (aboveThresh < -halfKnee)
                return 0.0f;
            else if (aboveThresh > halfKnee)
                return aboveThresh * (1.0f - 1.0f / ratio);
            else {
                float t = (aboveThresh + halfKnee) / kneeDB;
                float tSat = t * t * (3.0f - 2.0f * t);
                return tSat * aboveThresh * (1.0f - 1.0f / ratio);
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
        float userComp = -compDB;

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
    float rideOff = rideSmoothedOffset;
    compDB -= rideOff;
    compDB = juce::jlimit(-36.0f, 0.0f, compDB);

    // dlySize needed for dry/wet mix
    int dlySize = (int)dryDelayL.size();

    if (!bypassed)
    {
        // 0. Input Trim — per-sample interpolated to prevent zipper noise
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
                // Written in place rather than via IIR::Coefficients::makeHighPass,
                // which returns a ReferenceCountedObjectPtr: that allocated and
                // freed on the audio thread on every block of an HPF automation
                // ramp, and malloc can block. Same RBJ high-pass design, Q=1/sqrt(2).
                setHighPassCoefficients(*sigHpFilter.state, currentSampleRate, hpfFreq);
                lastHpfFreq = hpfFreq;
            }
            juce::dsp::AudioBlock<float> block(buffer);
            sigHpFilter.process(juce::dsp::ProcessContextReplacing<float>(block));
        }

        // (Sidechain HPF is handled internally by the compressor's detector)

        // (Input saturation removed — causes audible distortion on sub bass)

        // Write dry signal into latency-compensated delay AFTER all pre-processing
        // This ensures dry and wet match in level, frequency content, and phase
        for (int i = 0; i < numSamples; ++i) {
            dryDelayL[dryDelayWritePos] = left[i];
            dryDelayR[dryDelayWritePos] = right[i];
            dryDelayWritePos = (dryDelayWritePos + 1) % dlySize;
        }

        // 3. Compressor — internally optimized timing (no user attack/release)
        compressor.smoothAttack = true;
        float gateThresh = apvts.getRawParameterValue("gate")->load();

        // Attack/Release driven by comp amount — like RVox, always optimal
        // Fast attack + RMS detector + lookahead = consonants preserved naturally
        float compAmt01 = juce::jlimit(0.0f, 1.0f, -compDB / 36.0f);
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

        // Compressor — fixed 2x oversampling for alias-free gain modulation
        {
            juce::dsp::AudioBlock<float> block(buffer);
            auto osBlock = compOS.processSamplesUp(block);
            int osN = (int)osBlock.getNumSamples();
            float* osL = osBlock.getChannelPointer(0);
            float* osR = osBlock.getChannelPointer(1);
            compressor.process(osL, osR, osN, compDB, gateThresh);
            compOS.processSamplesDown(block);
        }

        {
            // === NORMAL OUTPUT PATH ===

            // 4. Makeup gain — follows the gain reduction actually delivered.
            //
            // This used to be derived from the knob position instead, which did
            // not match what the compressor was doing: at Comp 8 the detector
            // produced 0 dB of reduction while makeup added +7.4 dB, so the
            // limiter downstream was already working, and from Comp 12 up it sat
            // in continuous double-digit reduction. That is what made the plugin
            // sound loud and flat regardless of setting.
            //
            // The averaging window matters: following GR instantly would cancel
            // the compression exactly. At ~500 ms it restores level while the
            // dynamics stay compressed. Makeup is applied after the compressor,
            // so it never feeds back into the detector.
            float makeupSmooth = std::exp(-(float)numSamples / (float)(currentSampleRate * 0.500));
            smoothedMakeupGR = smoothedMakeupGR * makeupSmooth
                             + compressor.getGainReductionDB() * (1.0f - makeupSmooth);
            if (! std::isfinite(smoothedMakeupGR)) smoothedMakeupGR = 0.0f;
            float makeupDB = juce::jlimit(0.0f, 24.0f, smoothedMakeupGR);
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

            // 5. Soft Clipper — harmonic saturation, 4x oversampled.
            //
            // This sits BEFORE the limiter now. It used to run after both the
            // limiter and the safety clip with nothing bounding it, so with the
            // ceiling at -6 dBFS and Clip at 100% the output reached -0.53 dBFS,
            // 5.5 dB above what the user asked for. Putting it ahead of the
            // limiter means the ceiling is always respected.
            //
            // The oversampler runs unconditionally so the reported latency stays
            // constant: crossing the clip threshold used to change plugin latency
            // mid-stream and dump the oversampler's stale IIR state into the
            // signal. With clipAmt at 0 the waveshaper is an identity, so this
            // only costs the up/down conversion.
            float clipTarget = apvts.getRawParameterValue("clip")->load() / 100.0f;
            {
                const float pSmooth = std::exp(-(float)numSamples / (float)(currentSampleRate * 0.030));
                smoothedClipAmt = smoothedClipAmt * pSmooth + clipTarget * (1.0f - pSmooth);
                if (! std::isfinite(smoothedClipAmt)) smoothedClipAmt = clipTarget;
                if (std::abs(clipTarget - smoothedClipAmt) < 0.0005f) smoothedClipAmt = clipTarget;
            }
            const float clipAmt = smoothedClipAmt;
            {
                juce::dsp::AudioBlock<float> block(buffer);
                auto osBlock = oversampler.processSamplesUp(block);
                int osN = (int)osBlock.getNumSamples();

                if (clipAmt > 0.001f) {
                    // Loop invariants: drive and both normalisation terms depend
                    // only on clipAmt, which is constant across the block. These
                    // were being recomputed for every one of 4 x numSamples x 2
                    // samples — three transcendentals each.
                    const float drive = 1.0f + clipAmt * 4.0f;
                    const float slopeNorm = 1.0f / drive;
                    float totalDistortion = 0.0f;
                    int distCount = 0;
                    for (int ch = 0; ch < 2; ++ch) {
                        float* d = osBlock.getChannelPointer((size_t)ch);
                        for (int i = 0; i < osN; ++i) {
                            float before = d[i];
                            d[i] = softClipNormalized(before, clipAmt, drive, slopeNorm);
                            if (std::abs(before) > 0.05f) {
                                totalDistortion += std::abs(d[i] - before) / std::abs(before);
                                distCount++;
                            }
                        }
                    }
                    float avgDist = distCount > 0 ? totalDistortion / (float)distCount : 0.0f;
                    clipActivity.store(juce::jlimit(0.0f, 1.0f, avgDist * 8.0f));
                } else {
                    clipActivity.store(0.0f);
                }

                oversampler.processSamplesDown(block);
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
                // Mean square, for the same reason as the input chain above
                float preMatchMS = preMatchKSumSq / (float)numSamples;

                // Slow smoothing: 800ms — survives rapid knob movement
                float outLufsSmooth = std::exp(-(float)numSamples / (float)(currentSampleRate * 0.800));
                smoothedOutMS = smoothedOutMS * outLufsSmooth + preMatchMS * (1.0f - outLufsSmooth);
                if (! std::isfinite(smoothedOutMS) || smoothedOutMS < 0.0f) smoothedOutMS = 0.0f;
                smoothedOutLUFS = std::sqrt(smoothedOutMS);

                // Only update offset when both signals are above noise floor
                float inLevelDB = (smoothedInLUFS > 1e-10f) ? 20.0f * std::log10(smoothedInLUFS) : -100.0f;
                float outLevelDB = (smoothedOutLUFS > 1e-10f) ? 20.0f * std::log10(smoothedOutLUFS) : -100.0f;
                if (inLevelDB > -50.0f && outLevelDB > -50.0f) {
                    float diffDB = 20.0f * std::log10(smoothedInLUFS / smoothedOutLUFS);
                    // jlimit cannot filter NaN — every comparison against it is
                    // false, so it would pass both clamps below and end up
                    // multiplied into the audio.
                    if (! std::isfinite(diffDB)) diffDB = 0.0f;
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

            } else {
                prevOffsetLin = 1.0f;
                gainMatchOffsetDB.store(0.0f);  // reset so HONEST starts clean
            }

            // 8b. Limiter — last gain stage before the mix.
            // It sits after HONEST rather than before it: HONEST can boost by up
            // to 6 dB, so applying it downstream of the limiter let it push past
            // the ceiling, and the tanh waveshaper that used to patch that over
            // was the last un-oversampled nonlinearity in the chain. With the
            // limiter behind it the ceiling is binding again and the waveshaper
            // is unnecessary. The loudness measurement above still happens
            // before any gain match, so there is no feedback path.
            limiter.process(left, right, numSamples, CEILING_DB);

            // 8c. Safety clamp. The limiter holds the ceiling to within 0.03 dB
            // now, so this only has to catch that residue. It used to be a
            // rational saturator starting 2.5 dB *below* the ceiling, which made
            // it the real peak controller — and being un-oversampled it produced
            // -33 dBc of aliasing at 3 kHz, right in the sibilance band. A tight
            // clamp shapes so little that its harmonics are negligible.
            {
                const float ceiling = std::pow(10.0f, CEILING_DB / 20.0f);
                for (int i = 0; i < numSamples; ++i) {
                    left[i]  = juce::jlimit(-ceiling, ceiling, left[i]);
                    right[i] = juce::jlimit(-ceiling, ceiling, right[i]);
                }
            }

            // 9. Latency-compensated Dry/Wet Mix — per-sample interpolated.
            // Must match totalWetLatency exactly, clipper oversampler included,
            // or the two paths comb against each other.
            const int effectiveWetLatency = totalWetLatency;

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

            // 10. Out Gain — a true output level, applied to the finished mix so
            // it cannot shift the dry/wet balance, and per-sample ramped so
            // automating it does not zipper. Placed after everything: it sets
            // level without changing how hard anything upstream works.
            {
                float outTrimLin = std::pow(10.0f, outTrimDB / 20.0f);
                float trimDelta = (outTrimLin - prevOutTrimLin) / (float)numSamples;
                float curTrim = prevOutTrimLin;
                for (int i = 0; i < numSamples; ++i) {
                    curTrim += trimDelta;
                    left[i] *= curTrim;
                    right[i] *= curTrim;
                }
                prevOutTrimLin = outTrimLin;
            }
        }
    }
    else
    {
        // Bypassed: read the raw input back out delayed by the wet latency, so a
        // bypassed instance stays aligned with the rest of the session instead of
        // arriving early. Reads the dedicated bypass line, which always holds raw
        // input — the Mix line is written after trim, HPF and De-Click, so
        // bypassing used to start with up to 12 dB of trim baked in.
        {
            const int bdSize = (int)bypassDelayL.size();
            const int base = (bypassDelayWritePos - numSamples + bdSize * 2) % bdSize;
            for (int i = 0; i < numSamples; ++i) {
                int readPos = (base + i - totalWetLatency + bdSize * 2) % bdSize;
                left[i]  = bypassDelayL[(size_t)readPos];
                right[i] = bypassDelayR[(size_t)readPos];
            }
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
        // This filter has no path back from a non-finite state, so check once
        // per block rather than per sample.
        if (! std::isfinite(dcBlockL) || ! std::isfinite(dcBlockR)
            || ! std::isfinite(dcPrevInL) || ! std::isfinite(dcPrevInR)) {
            dcBlockL = dcBlockR = dcPrevInL = dcPrevInR = 0.0f;
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
                left[i]  = left[i] * (1.0f - fade) + fadeFromL[(size_t)i] * fade;
                right[i] = right[i] * (1.0f - fade) + fadeFromR[(size_t)i] * fade;
            } else {
                // Fading FROM bypass: dry → processed
                left[i]  = fadeFromL[(size_t)i] * (1.0f - fade) + left[i] * fade;
                right[i] = fadeFromR[(size_t)i] * (1.0f - fade) + right[i] * fade;
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

juce::AudioProcessorEditor* SmartCompProcessor::createEditor() { return new SmartCompEditor(*this); }

juce::AudioProcessorParameter* SmartCompProcessor::getBypassParameter() const
{
    return apvts.getParameter("bypass");
}

void SmartCompProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void SmartCompProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SmartCompProcessor(); }

// ===== K-Weighting Filters for LUFS (ITU-R BS.1770) =====
void SmartCompProcessor::computeKWeightingCoeffs(double sampleRate)
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
        // The denominator is 1 + K/Q + K^2 — no Vb. It used to carry a stray Vb
        // in both a0 and a2, which raised the shelf's effective Q from 0.707 to
        // 0.890 and made it resonant: +4.44 dB at 1682 Hz against a +4.00 dB
        // asymptote, a 2 dB error, peaking near +4.95 dB around 1.2 kHz.
        // Without it these match BS.1770-4 Table 1 to 14 digits.
        double a0 = 1.0 + K / Q + K2;
        kShelfCoeffs.b0 = (float)((Vh + Vb * K / Q + K2) / a0);
        kShelfCoeffs.b1 = (float)((2.0 * (K2 - Vh)) / a0);
        kShelfCoeffs.b2 = (float)((Vh - Vb * K / Q + K2) / a0);
        kShelfCoeffs.a1 = (float)((2.0 * (K2 - 1.0)) / a0);
        kShelfCoeffs.a2 = (float)((1.0 - K / Q + K2) / a0);
    }

    // Stage 2: RLB weighting (high-pass, ~38Hz)
    {
        double fc = 38.13547087602444;
        double Q  = 0.5003270373238773;
        double K  = std::tan(juce::MathConstants<double>::pi * fc / sampleRate);
        double K2 = K * K;
        double a0 = 1.0 + K / Q + K2;
        // BS.1770 specifies b = {1, -2, 1} un-normalised, so the passband gain is
        // exactly 1. Dividing these by a0 made the reading sample-rate dependent
        // by about 0.04 dB.
        kHPCoeffs.b0 = 1.0f;
        kHPCoeffs.b1 = -2.0f;
        kHPCoeffs.b2 = 1.0f;
        kHPCoeffs.a1 = (float)((2.0 * (K2 - 1.0)) / a0);
        kHPCoeffs.a2 = (float)((1.0 - K / Q + K2) / a0);
    }
}

float SmartCompProcessor::applyBiquad(float x, BiquadState& s, const BiquadCoeffs& c)
{
    float y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2 - c.a1 * s.y1 - c.a2 * s.y2;
    s.x2 = s.x1; s.x1 = x;
    s.y2 = s.y1; s.y1 = y;
    return y;
}
