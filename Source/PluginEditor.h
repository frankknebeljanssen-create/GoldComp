#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class SmartCompEditor : public juce::AudioProcessorEditor, private juce::Timer
{
    // Wordmark, split into the accent-coloured and white halves. Kept in one
    // place so the header and the credits panel cannot drift apart, and so the
    // sibling plugin only has to change these two strings.
    static constexpr const char* LOGO_A = "SMART";
    static constexpr const char* LOGO_B = "COMP";

public:
    explicit SmartCompEditor(SmartCompProcessor&);
    ~SmartCompEditor() override;
    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;
    // Blocks knob/button clicks while the credits overlay covers them
    void setControlsInteractive(bool);
    void timerCallback() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;

    SmartCompProcessor& processor;

    juce::Slider compSlider, gainSlider, gateSlider, hpfSlider, mixSlider;
    juce::Label compLabel, gainLabel, gateLabel, hpfLabel, mixLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        compAttach, gainAttach, gateAttach, hpfAttach, mixAttach;

    juce::ToggleButton bypassBtn;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttach;


    juce::TextButton matchBtn;
    bool showInfo = false;
    juce::Rectangle<int> logoRect, infoBtnRect, advToggleRect;
    
    // Interactive compression display
    juce::Rectangle<int> compDisplayRect;
    juce::String displayDragParam;
    float displayDragStartVal = 0;
    float displayDragValue = 0;

    // HONEST mode button
    juce::Rectangle<int> honestBtnRect;
    juce::Rectangle<int> grBarRect, grTimelineRect, vocalStateRect;
    bool grTimelineFast = false;  // false=slow (full history), true=fast (zoomed in)
    juce::Rectangle<int> grSpeedToggleRect;

    void drawBigKnob(juce::Graphics& g, int cx, int cy, int radius, float normVal, float grDB);
    void drawMeter(juce::Graphics& g, int x, int y, int w, int h, float level, float peak);
    void drawGRTimeline(juce::Graphics& g, juce::Rectangle<int> area);

    // === CENTRALIZED LAYOUT — single source of truth ===
    struct Layout {
        int w = 480;                // base width
        int headerH = 48;
        int contentY = 84;          // headerH + padding
        int contentCx = 240;        // w / 2
        int meterH = 180, meterW = 8, meterGap = 3;
        int knobSize = 200;
        int meterX = 0;             // IN meter X
        int meterY = 0;             // meter top Y
        int rideX = 0, rideW = 30;  // RIDE meter
        int grMeterX = 0, grMeterW = 30;  // GR meter (vertical, mirrors RIDE)
        int outMeterX = 0;          // OUT meter X
        int trueBarY = 0, trueBarH = 24;
        int kcY = 0, kcH = 104;     // knob card
        int tlY = 0, tlH = 155, tlW = 0;  // timeline
        int panelY = 0, panelH = 140;     // ADV knobs panel
        // Shared bar positioning
        int barX = 0, barW = 200, readoutX = 0;
        // Knob card margins
        int knobLeftMargin = 78, knobRightMargin = 50;
    } L;
    void recalcLayout();

    float displayGR = 0, displayOutL = 0, displayOutR = 0, displayInL = 0, displayInR = 0;
    float displayInLUFS = -60.0f, displayOutLUFS = -60.0f, displayOffsetDB = 0.0f;
    float lastCompValue = 0.0f;  // for magnet snap direction tracking
    bool wasInSweetSpot = false;
    int ssFlashFrames = 0;  // countdown for sweet spot entry flash
    float dragStartComp = 0.0f, dragStartRatioMult = 1.0f;
    int dragStartX = 0, dragStartY = 0;
    float peakHoldInL = 0, peakHoldInR = 0, peakHoldOutL = 0, peakHoldOutR = 0, peakHoldGR = 0;
    int peakHoldCounter = 0;
    static constexpr int PEAK_HOLD_FRAMES = 20;  // ~0.7s at 30Hz
    int frameCounter = 0;
    float sweetSpotGlow = 0.0f;  // smooth 0→1 for breathing glow fade

    // Tooltip system
    bool infoMode = false;
    struct TooltipInfo { juce::String title, desc, tech; };
    juce::String hoveredElement;
    TooltipInfo getTooltipFor(const juce::String& element);

    // A/B comparison
    juce::Rectangle<int> rideRect;

    // ADV panel
    bool advOpen = false;
    static constexpr int ADV_PANEL_H = 319;  // tlGap(12) + tl(155) + panelGap(12) + panel(170) - overlap(10)
    juce::Slider inTrimSlider, scHpfSlider;
    juce::Label inTrimLabel, scHpfLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        inTrimAttach, scHpfAttach;
    int getBaseHeight() const {
        // +40px for knob padding (20 top + 20 bottom)
        return 536;
    }
    static constexpr int BASE_W = 600;
    float getScale() const { return (float)getWidth() / (float)BASE_W; }
    juce::Point<int> unscalePt(juce::Point<int> p) const {
        float s = getScale();
        return { (int)(p.x / s), (int)(p.y / s) };
    }
    int S(int v) const { return (int)(v * getScale()); }  // scale helper for setBounds

    class SmartCompLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        SmartCompLookAndFeel();
        void drawRotarySlider(juce::Graphics&, int, int, int, int, float, float, float, juce::Slider&) override;
        void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
        void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool, bool) override;
    };

    SmartCompLookAndFeel goldLNF;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SmartCompEditor)
};
