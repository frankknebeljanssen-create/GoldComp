#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "DSP/RVoxCompressor.h"
#include "DSP/LookaheadLimiter.h"

class GoldCompProcessor : public juce::AudioProcessor
{
public:
    GoldCompProcessor();
    ~GoldCompProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::UndoManager undoManager;
    juce::AudioProcessorValueTreeState apvts;

    std::atomic<float> compGainReductionDB { 0.0f };
    std::atomic<float> limiterGainReductionDB { 0.0f };
    std::atomic<float> outputPeakL { 0.0f }, outputPeakR { 0.0f };
    std::atomic<float> inputPeakL { 0.0f }, inputPeakR { 0.0f };

    // Gain match
    std::atomic<float> inputRMS { 0.0f };
    std::atomic<float> outputRMS { 0.0f };
    std::atomic<float> gainMatchOffsetDB { 0.0f };
    std::atomic<bool> gainMatchEnabled { false };
    std::atomic<bool> honestMode { false };  // Auto loudness match — hear only character, not volume

    // Clip activity: 0-1 how hard the clipper is saturating
    std::atomic<float> clipActivity { 0.0f };
    // True when output exceeds 0dBFS
    std::atomic<bool> outputClipping { false };
    std::atomic<bool> inputClipping { false };
    std::atomic<bool> deltaMode { false };
    std::atomic<bool> deClickMode { false };
    std::atomic<bool> rideMode { false };           // auto-leveling ride
    std::atomic<float> rideOffsetComp { 0.0f };     // offset in comp units (0-36) for editor display

    // Signal analysis for Sweet Spot / Confidence features
    std::atomic<float> inputDynamicRange { 0.0f };   // smoothed crest factor (dB)
    std::atomic<float> inputRMSdB { -60.0f };        // current input RMS level
    std::atomic<float> inputPeakDBSmoothed { -60.0f };// smoothed peak level of active signal
    std::atomic<float> sweetSpotLow { 8.0f };         // computed sweet spot range (comp knob value)
    std::atomic<float> sweetSpotHigh { 22.0f };       // computed sweet spot range (comp knob value)
    int analysisBlockCount = 0;  // for fast learn in first ~2 seconds
    float smoothedPeakDB = -60.0f;   // smoothed peak level (internal)
    float smoothedRMSDB = -60.0f;    // smoothed RMS level (internal)
    float smoothedCrestDB = 12.0f;   // smoothed crest factor (internal)

    // (oversamplers removed — only clipper OS remains)

    // Public for GR timeline access
    RVoxCompressor compressor;

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    LookaheadLimiter limiter;

    using HPFilter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    HPFilter sigHpFilter;

    double currentSampleRate = 44100.0;
    float lastHpfFreq = -1.0f;

    juce::dsp::Oversampling<float> oversampler { 2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true };  // 4x for clipper
    juce::dsp::Oversampling<float> compOS { 2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true };     // fixed 2x for compressor

    static float softClipNormalized(float sample, float amount,
                                    float drive, float slopeNorm);

    // DC blocker after asymmetric clipper
    float dcBlockL = 0.0f, dcBlockR = 0.0f;
    float dcPrevInL = 0.0f, dcPrevInR = 0.0f;
    float dcBlockCoeff = 0.9995f; // computed from SR in prepareToPlay

    bool prevBypassed = false;  // for bypass crossfade

    std::vector<float> dryDelayL, dryDelayR;
    int dryDelayWritePos = 0;
    int totalWetLatency = 0;

    // Per-sample interpolation state (anti-zipper)
    float prevTrimLin = 1.0f;
    float prevMakeupLin = 1.0f;
    float prevOffsetLin = 1.0f;
    float prevMixWet = 1.0f;
    float prevOutTrimLin = 1.0f;
    float smoothedMakeupGR = 0.0f;   // slow average of delivered GR, drives makeup

    // K-weighted loudness (LUFS) for gain match
    // Stage 1: high-shelf +4dB @ 1681Hz (pre-filter)
    // Stage 2: high-pass 38Hz (RLB weighting)
    struct BiquadState { float x1=0,x2=0,y1=0,y2=0; };
    struct BiquadCoeffs { float b0=1,b1=0,b2=0,a1=0,a2=0; };

    BiquadCoeffs kShelfCoeffs, kHPCoeffs;
    BiquadState kShelfIn, kHPIn;   // input measurement chain
    BiquadState kShelfOut, kHPOut;  // output measurement chain
    float smoothedInLUFS = 0.0f;
    float smoothedOutLUFS = 0.0f;

    void computeKWeightingCoeffs(double sampleRate);
    float applyBiquad(float x, BiquadState& s, const BiquadCoeffs& c);

    // De-click: envelope for transient detection
    float dcEnvL = 0.0f, dcEnvR = 0.0f;
    float dcAttCoeff = 0.0f, dcRelCoeff = 0.0f, dcSlowRelCoeff = 0.0f;

    // RIDE: auto-leveling — slow RMS tracker that adjusts comp offset
    float rideEnvDB = -60.0f;           // slow RMS envelope (~3 sec)
    float rideRefDB = -60.0f;           // reference level (first measured input)
    bool rideRefSet = false;            // has reference been captured
    float rideSmoothedOffset = 0.0f;    // smoothed output offset in comp units
    float rideEnvCoeff = 0.0f;          // ~3 sec time constant
    float rideOffsetSmoothCoeff = 0.0f; // ~0.5 sec for smooth knob movement

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GoldCompProcessor)
};
