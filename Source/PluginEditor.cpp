#include "PluginEditor.h"
#include "PluginProcessor.h"
#include <cmath>

// ============== COLORS ==============
namespace C {
    const juce::Colour bg        { 0xff0e1117 };
    const juce::Colour card      { 0xff161a22 };
    const juce::Colour cardBot   { 0xff12151c };
    const juce::Colour well      { 0xff0a0d12 };
    const juce::Colour border    { 0xff252a34 };
    const juce::Colour accent    { 0xff6fb89c };
    const juce::Colour white     { 0xffd8dae0 };
    const juce::Colour label     { 0xff8a8ea0 };  // was 5c6070 — 50% brighter
    const juce::Colour dim       { 0xff606878 };  // was 3c4050 — 50% brighter
    const juce::Colour scaleTxt  { 0xff9aa0a8 };  // was 707580 — 40% brighter
    const juce::Colour gr        { 0xffcc3c3c };
    const juce::Colour clipCol   { 0xffd4a843 };
    const juce::Colour peak      { 0xffaaaaaa };
    const juce::Colour shadow    { 0xff060810 };
    const juce::Colour knobBody  { 0xff1e2128 };
    const juce::Colour knobEdge  { 0xff32363e };
    const juce::Colour knobFace  { 0xff14171e };
    const juce::Colour mGreen    { 0xff50c878 };
    const juce::Colour mYellow   { 0xffddc840 };
    const juce::Colour mRed      { 0xffee5555 };
}

// ============== LOOK AND FEEL ==============
SmartCompEditor::SmartCompLookAndFeel::SmartCompLookAndFeel()
{
    setColour(juce::Slider::rotarySliderFillColourId, C::accent);
    setColour(juce::Slider::rotarySliderOutlineColourId, C::well);
    setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    setColour(juce::TextButton::textColourOffId, C::label);
}

void SmartCompEditor::SmartCompLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& btn, const juce::Colour&, bool, bool)
{
    if (btn.getButtonText() == "MATCH") {
        auto b = btn.getLocalBounds().toFloat();
        auto* editor = dynamic_cast<SmartCompEditor*>(btn.getParentComponent());
        bool matchOn = editor ? editor->processor.gainMatchEnabled.load() : false;
        g.setColour(matchOn ? C::accent.withAlpha(0.12f) : C::card);
        g.fillRoundedRectangle(b, 5.0f);
        g.setColour(matchOn ? C::accent.withAlpha(0.5f) : C::border);
        g.drawRoundedRectangle(b, 5.0f, 1.0f);
        btn.setColour(juce::TextButton::textColourOffId, matchOn ? C::accent : C::label);
    }
}

void SmartCompEditor::SmartCompLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& btn, bool over, bool down)
{
    auto b = btn.getLocalBounds().toFloat().reduced(2);
    bool on = btn.getToggleState();
    g.setColour(on ? C::accent : C::label);
    float cx = b.getCentreX(), cy = b.getCentreY(), r = 8.0f;
    g.drawEllipse(cx - r, cy - r + 1, r * 2, r * 2, 1.5f);
    g.drawLine(cx, cy - r - 1, cx, cy + 1, 1.5f);
}

// Small knobs — matching big knob design (knurled edge, dome body, teal arc, pointer dot)
void SmartCompEditor::SmartCompLookAndFeel::drawRotarySlider(
    juce::Graphics& g, int x, int y, int width, int height,
    float sliderPos, float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider)
{
    if (slider.getName() == "Compression") return;

    float cx = x + width * 0.5f, cy = y + height * 0.5f;
    float outerR = juce::jmin(width, height) * 0.5f - 1.0f;
    float arcR = outerR - 6.0f;
    float bodyR = outerR - 10.0f;
    float innerR = bodyR - 5.0f;
    float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    auto ptc = [&](float r, float a) -> juce::Point<float> {
        float rad = a - juce::MathConstants<float>::halfPi;
        return { cx + r * std::cos(rad), cy + r * std::sin(rad) };
    };

    // Drop shadow
    g.setColour(juce::Colour(0x25000000));
    g.fillEllipse(cx - outerR - 2, cy - outerR + 1, (outerR + 2) * 2, (outerR + 2) * 2);

    // Knurled edge — 36 teeth
    {
        juce::Path teeth;
        int TEETH = 36;
        for (int i = 0; i < TEETH; ++i) {
            float a1 = (float)i / TEETH * juce::MathConstants<float>::twoPi;
            float a2 = ((float)i + 0.5f) / TEETH * juce::MathConstants<float>::twoPi;
            auto o = juce::Point<float>(cx + outerR * std::cos(a1), cy + outerR * std::sin(a1));
            auto inn = juce::Point<float>(cx + (outerR - 2.5f) * std::cos(a2), cy + (outerR - 2.5f) * std::sin(a2));
            if (i == 0) teeth.startNewSubPath(o);
            else teeth.lineTo(o);
            teeth.lineTo(inn);
        }
        teeth.closeSubPath();
        juce::ColourGradient eg(juce::Colour(0xff4a4e5a), cx, cy - outerR,
                                juce::Colour(0xff1e2128), cx, cy + outerR, false);
        g.setGradientFill(eg);
        g.fillPath(teeth);
    }
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.drawEllipse(cx - outerR + 1, cy - outerR + 1, (outerR - 1) * 2, (outerR - 1) * 2, 0.5f);

    // Track arc
    {
        juce::Path track;
        track.addCentredArc(cx, cy, arcR, arcR, 0, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(C::well);
        g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Active arc
    if (sliderPos > 0.01f) {
        juce::Path active;
        active.addCentredArc(cx, cy, arcR, arcR, 0, rotaryStartAngle, angle, true);
        g.setColour(C::accent.withAlpha(0.2f));
        g.strokePath(active, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(C::accent.withAlpha(0.85f));
        g.strokePath(active, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Knob body — convex dome
    {
        juce::ColourGradient bg(juce::Colour(0xff484e5c), cx - bodyR * 0.15f, cy - bodyR * 0.35f,
                                juce::Colour(0xff12151c), cx, cy + bodyR, true);
        bg.addColour(0.3, juce::Colour(0xff3c4250));
        bg.addColour(0.55, juce::Colour(0xff2a2e38));
        bg.addColour(0.8, juce::Colour(0xff1a1e26));
        g.setGradientFill(bg);
        g.fillEllipse(cx - bodyR, cy - bodyR, bodyR * 2, bodyR * 2);

        // Brushed metal — concentric rings
        for (float r = bodyR - 1; r > innerR + 1; r -= 1.5f) {
            float alpha = 0.012f + 0.008f * std::sin(r * 2.3f);
            g.setColour(juce::Colours::white.withAlpha(alpha));
            g.drawEllipse(cx - r, cy - r, r * 2, r * 2, 0.3f);
        }

        // Convex highlight
        juce::ColourGradient hl(juce::Colours::white.withAlpha(0.10f),
                                 cx - bodyR * 0.3f, cy - bodyR * 0.3f,
                                 juce::Colours::transparentBlack,
                                 cx + bodyR * 0.3f, cy + bodyR * 0.1f, true);
        g.setGradientFill(hl);
        g.fillEllipse(cx - bodyR, cy - bodyR, bodyR * 2, bodyR * 2);
    }

    // Edge highlight (top) + shadow (bottom)
    {
        juce::Path hl;
        hl.addCentredArc(cx, cy, bodyR, bodyR, 0, -2.8f, -0.35f, true);
        g.setColour(juce::Colours::white.withAlpha(0.10f));
        g.strokePath(hl, juce::PathStrokeType(1.0f));
    }
    {
        juce::Path sh;
        sh.addCentredArc(cx, cy, bodyR, bodyR, 0, 0.35f, 2.8f, true);
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.strokePath(sh, juce::PathStrokeType(1.5f));
    }

    // Inner face
    {
        juce::ColourGradient faceBg(juce::Colour(0xff1c2028), cx, cy - innerR,
                                     juce::Colour(0xff101418), cx, cy + innerR, false);
        g.setGradientFill(faceBg);
        g.fillEllipse(cx - innerR, cy - innerR, innerR * 2, innerR * 2);
    }
    g.setColour(juce::Colours::black.withAlpha(0.4f));
    g.drawEllipse(cx - innerR, cy - innerR, innerR * 2, innerR * 2, 0.8f);

    // Pointer line + dot (teal)
    auto p1 = ptc(innerR - 2.0f, angle), p2 = ptc(bodyR - 2.0f, angle);
    g.setColour(C::accent.withAlpha(0.12f));
    g.drawLine(p1.x, p1.y, p2.x, p2.y, 4.0f);
    g.setColour(C::accent.withAlpha(0.4f));
    g.drawLine(p1.x, p1.y, p2.x, p2.y, 2.0f);
    g.setColour(C::accent);
    g.drawLine(p1.x, p1.y, p2.x, p2.y, 1.2f);
    // Dot
    auto pd = ptc(bodyR - 3.5f, angle);
    g.setColour(C::accent.withAlpha(0.2f));
    g.fillEllipse(pd.x - 4.0f, pd.y - 4.0f, 8.0f, 8.0f);
    g.setColour(C::accent);
    g.fillEllipse(pd.x - 2.0f, pd.y - 2.0f, 4.0f, 4.0f);

    // Value in center
    float fval = (float)slider.getValue();
    int val = (int)fval;
    juce::String name = slider.getName();
    g.setFont(juce::Font("Arial", 11.0f, juce::Font::plain));
    g.setColour(C::white);
    if (name == "SC HPF") {
        g.drawText(val < 1 ? "OFF" : juce::String(val), x, y, width, height, juce::Justification::centred);
    } else if (name == "Gate") {
        g.drawText(val <= -79 ? "OFF" : juce::String(val), x, y, width, height, juce::Justification::centred);
    } else if (name == "In Trim" || name == "Gain") {
        // Both are bipolar trims, so the sign has to be visible
        juce::String txt = (fval > 0.05f ? "+" : "") + juce::String(fval, 1);
        g.drawText(txt, x, y, width, height, juce::Justification::centred);
    } else {
        g.drawText(juce::String(val), x, y, width, height, juce::Justification::centred);
    }
}

// ============== EDITOR CONSTRUCTOR ==============
SmartCompEditor::SmartCompEditor(SmartCompProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    setLookAndFeel(&goldLNF);
    setSize(BASE_W, getBaseHeight());
    setResizable(true, true);
    // No fixed aspect ratio — height changes with ADV panel
    getConstrainer()->setMinimumWidth(BASE_W * 3 / 4);    // 0.75x
    getConstrainer()->setMaximumWidth(BASE_W * 2);          // 2.0x


    // Setup all sliders as rotary
    auto setup = [&](juce::Slider& s, juce::Label& l, const juce::String& name, const juce::String& lbl) {
        s.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        s.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        s.setName(name); addAndMakeVisible(s);
        l.setText(lbl, juce::dontSendNotification);
        l.setFont(juce::Font("Arial", 11.0f, juce::Font::bold));
        l.setJustificationType(juce::Justification::centred);
        l.setColour(juce::Label::textColourId, C::label);
        addAndMakeVisible(l);
    };

    setup(compSlider, compLabel, "Compression", "COMP");
    compSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    compSlider.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f,
                                    juce::MathConstants<float>::pi * 2.75f, true);
    compSlider.setMouseDragSensitivity(150);
    compSlider.setSliderSnapsToMousePosition(false);
    compSlider.onValueChange = [this] {
        float v = (float)compSlider.getValue();
        // The magnet is a manual-drag aid. With AUTO on it would fight the
        // value AUTO is writing every frame — snapping to a boundary, AUTO
        // pulling back to the midpoint, and so on.
        if (processor.rideMode.load()) { lastCompValue = v; return; }
        // Dynamic magnet stops at vocal state boundaries
        // Only snap when moving TOWARD the point, not when dragging through
        float ssLow = processor.sweetSpotLow.load();
        float ssHigh = processor.sweetSpotHigh.load();
        float crushed = ssHigh + 6.0f;
        for (float sp : { ssLow, ssHigh, crushed }) {
            float dist = std::abs(v - sp);
            if (dist < 1.0f && dist > 0.05f) {
                // Only snap if we're closer than last frame (approaching)
                float prevDist = std::abs(lastCompValue - sp);
                if (dist < prevDist) {
                    compSlider.setValue(sp, juce::sendNotificationAsync);
                    lastCompValue = sp;
                    return;
                }
            }
        }
        lastCompValue = v;
    };
    setup(inTrimSlider, inTrimLabel, "In Trim", "IN TRIM");
    setup(gainSlider, gainLabel, "Gain", "OUT GAIN");
    setup(mixSlider, mixLabel, "Mix", "MIX");

    // Double-click reset
    compSlider.setDoubleClickReturnValue(true, 0.0f);
    inTrimSlider.setDoubleClickReturnValue(true, 0.0f);
    gainSlider.setDoubleClickReturnValue(true, 0.0f);
    mixSlider.setDoubleClickReturnValue(true, 100.0f);

    // Attachments
    compAttach  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.apvts, "comp",  compSlider);
    gateAttach  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.apvts, "gate",  gateSlider);
    inTrimAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.apvts, "inTrim", inTrimSlider);
    gainAttach  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.apvts, "gain",  gainSlider);
    mixAttach   = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.apvts, "mix",   mixSlider);


    // Bypass
    bypassBtn.setName("Bypass");
    addAndMakeVisible(bypassBtn);
    bypassAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.apvts, "bypass", bypassBtn);

    // Match button (hidden — we use pill rects)
    matchBtn.setButtonText("MATCH");
    matchBtn.onClick = [this] { bool on = !processor.gainMatchEnabled.load(); processor.gainMatchEnabled.store(on); repaint(); };
    addAndMakeVisible(matchBtn);
    matchBtn.setVisible(false);

    // ADV panel sliders
    auto setupAdv = [&](juce::Slider& s, juce::Label& l, const juce::String& name, const juce::String& lbl) {
        s.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        s.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        s.setName(name); addAndMakeVisible(s); s.setVisible(false);
        l.setText(lbl, juce::dontSendNotification);
        l.setFont(juce::Font("Arial", 12.0f, juce::Font::bold));
        l.setJustificationType(juce::Justification::centred);
        l.setColour(juce::Label::textColourId, C::label.brighter(0.3f));
        addAndMakeVisible(l); l.setVisible(false);
    };

    setupAdv(gateSlider, gateLabel, "Gate", "GATE");
    gateSlider.setDoubleClickReturnValue(true, -80.0f);


    setupAdv(scHpfSlider, scHpfLabel, "SC HPF", "SC HPF");
    scHpfAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.apvts, "schpf", scHpfSlider);
    scHpfSlider.setDoubleClickReturnValue(true, 0.0f);

    startTimerHz(60);
}

SmartCompEditor::~SmartCompEditor() { setLookAndFeel(nullptr); stopTimer(); }

// ============== TIMER ==============
void SmartCompEditor::timerCallback()
{
    auto bal = [](float& c, float t) { c = (t > c) ? c * 0.3f + t * 0.7f : c * 0.88f + t * 0.12f; };
    bal(displayGR, processor.compGainReductionDB.load());
    bal(displayInL, processor.inputPeakL.load()); bal(displayInR, processor.inputPeakR.load());
    bal(displayOutL, processor.outputPeakL.load()); bal(displayOutR, processor.outputPeakR.load());

    // Tell the audio thread whether the user is actively dragging, every frame
    // regardless of AUTO's state — the DSP side uses this to pin its internal
    // target to the live knob during a drag rather than gliding in the
    // background, so the return-to-sweet-spot always starts from wherever the
    // user actually let go.
    const bool draggingComp = compSlider.isMouseButtonDown();
    processor.compKnobDragging.store(draggingComp);

    // AUTO drives the real knob, so what you see is always what the compressor
    // is using. While the user is dragging we leave it alone — release the
    // mouse and it glides back to the sweet spot rather than sitting wherever
    // it was let go.
    if (processor.rideMode.load() && ! draggingComp)
    {
        const float target = processor.rideTargetComp.load();
        if (std::abs((float)compSlider.getValue() - target) > 0.01f)
            compSlider.setValue(target, juce::sendNotificationSync);
    }

    // Sweet spot entry detection
    {
        float comp = processor.apvts.getRawParameterValue("comp")->load();
        float ssL = processor.sweetSpotLow.load();
        float ssH = processor.sweetSpotHigh.load();
        bool inSS = comp >= ssL && comp <= ssH && comp > 1.0f;
        if (inSS && !wasInSweetSpot) ssFlashFrames = 20;  // ~0.7s flash
        wasInSweetSpot = inSS;
        if (ssFlashFrames > 0) ssFlashFrames--;
    }
    // LUFS metering for TRUE display
    {
        float inRMS = processor.inputRMS.load();
        float outRMS = processor.outputRMS.load();
        float inLUFS = inRMS > 1e-8f ? 20.0f * std::log10(inRMS) : -60.0f;
        float outLUFS = outRMS > 1e-8f ? 20.0f * std::log10(outRMS) : -60.0f;
        displayInLUFS = displayInLUFS * 0.85f + inLUFS * 0.15f;
        displayOutLUFS = displayOutLUFS * 0.85f + outLUFS * 0.15f;
        displayOffsetDB = displayOffsetDB * 0.9f + processor.gainMatchOffsetDB.load() * 0.1f;
    }
    peakHoldCounter++;
    auto uph = [&](float& ph, float c) { if (c > ph) { ph = c; peakHoldCounter = 0; } };
    uph(peakHoldInL, displayInL); uph(peakHoldInR, displayInR);
    uph(peakHoldOutL, displayOutL); uph(peakHoldOutR, displayOutR); uph(peakHoldGR, displayGR);
    if (peakHoldCounter > PEAK_HOLD_FRAMES) {
        peakHoldInL *= 0.85f; peakHoldInR *= 0.85f; peakHoldOutL *= 0.85f; peakHoldOutR *= 0.85f; peakHoldGR *= 0.85f;
    }
    frameCounter = (frameCounter + 1) % 3770;  // wraps every ~63s (LCM of common periods)

    // Update ADV knob labels — unit only (value shown inside knob)
    if (advOpen) {
        float gateVal = processor.apvts.getRawParameterValue("gate")->load();
        gateLabel.setText(gateVal <= -79.5f ? "OFF" : "dB", juce::dontSendNotification);
        float scHpfVal = processor.apvts.getRawParameterValue("schpf")->load();
        scHpfLabel.setText(scHpfVal < 1.0f ? "OFF" : "Hz", juce::dontSendNotification);
    }

    // Tooltip hover detection in timer (works over child components)
    if (infoMode) {
        juce::String prev = hoveredElement;
        hoveredElement = "";
        auto p = getMouseXYRelative();       // actual pixel position
        auto bp = unscalePt(p);              // base-coordinate position (for manually drawn rects)
        if (getLocalBounds().contains(p)) {
            if (compSlider.getBounds().contains(p)) hoveredElement = "comp";
            else if (inTrimSlider.getBounds().contains(p)) hoveredElement = "intrim";
            else if (gainSlider.getBounds().contains(p)) hoveredElement = "gain";
            else if (mixSlider.getBounds().contains(p)) hoveredElement = "mix";
            else if (gateSlider.isVisible() && gateSlider.getBounds().contains(p)) hoveredElement = "gate";
            else if (scHpfSlider.isVisible() && scHpfSlider.getBounds().contains(p)) hoveredElement = "schpf";
            else if (honestBtnRect.contains(bp)) hoveredElement = "true";
            else if (rideRect.contains(bp)) hoveredElement = "ride";
            else if (advToggleRect.contains(bp)) hoveredElement = "adv";
            else if (grBarRect.contains(bp)) hoveredElement = "gr";
            else if (grTimelineRect.contains(bp)) hoveredElement = "grtimeline";
        }
    } else {
        hoveredElement = "";
    }

    // Dim all knobs when bypassed
    bool bypassed = processor.apvts.getRawParameterValue("bypass")->load() > 0.5f;
    float knobAlpha = bypassed ? 0.3f : 1.0f;
    gateSlider.setAlpha(knobAlpha); gainSlider.setAlpha(knobAlpha); mixSlider.setAlpha(knobAlpha);
    scHpfSlider.setAlpha(knobAlpha);
    inTrimSlider.setAlpha(knobAlpha);

    repaint();
}

// ============== MOUSE ==============
void SmartCompEditor::mouseDown(const juce::MouseEvent& e)
{
    auto bp = unscalePt(e.getPosition());  // base-coordinate mouse position

    if (showInfo) { showInfo = false; setControlsInteractive(true); repaint(); return; }
    if (logoRect.contains(bp)) { showInfo = true; setControlsInteractive(false); repaint(); return; }

    // Info mode toggle
    if (infoBtnRect.contains(bp)) {
        infoMode = !infoMode;
        if (!infoMode) hoveredElement = "";
        repaint(); return;
    }

    // A/B switch
    // ADV panel toggle
    if (advToggleRect.contains(bp)) {
        advOpen = !advOpen;
        int totalBaseH = getBaseHeight() + (advOpen ? ADV_PANEL_H + 12 : 0);
        float s = getScale();
        setSize((int)(BASE_W * s), (int)(totalBaseH * s));
        resized();
        repaint(); return;
    }

    // GR Timeline speed toggle
    if (grSpeedToggleRect.contains(bp)) {
        grTimelineFast = !grTimelineFast;
        repaint(); return;
    }

    // DE-CLICK toggle
    // TRUE mode toggle
    if (honestBtnRect.contains(bp)) {
        bool on = !processor.honestMode.load();
        processor.honestMode.store(on);
        repaint(); return;
    }

    // RIDE mode toggle
    if (rideRect.contains(bp)) {
        processor.rideMode.store(!processor.rideMode.load());
        repaint(); return;
    }
}

void SmartCompEditor::mouseDrag(const juce::MouseEvent&) {}

void SmartCompEditor::mouseUp(const juce::MouseEvent&)
{
    displayDragParam = "";
}

void SmartCompEditor::mouseDoubleClick(const juce::MouseEvent&) {}

// ============== CENTRALIZED LAYOUT ==============
void SmartCompEditor::recalcLayout()
{
    L.w = BASE_W;
    L.headerH = 48;
    L.contentY = L.headerH + 28;
    L.contentCx = L.w / 2;
    L.meterH = 198; L.meterW = 9; L.meterGap = 3;  // +10%
    L.knobSize = 220;                                 // +10%
    L.meterX = L.contentCx - L.knobSize / 2 - 120;        // 20px further left
    L.meterY = L.contentY + 30;                          // +20px top padding around knob
    L.rideW = 33;                                     // +10%
    L.rideX = L.meterX + L.meterW * 2 + L.meterGap + 20;
    // Right side mirrors left
    int knobRight = L.contentCx + L.knobSize / 2;
    int knobLeft = L.contentCx - L.knobSize / 2;
    int rideRight = L.rideX + L.rideW;
    int gapRideToKnob = knobLeft - rideRight;
    int gapInToRide = L.rideX - (L.meterX + L.meterW * 2 + L.meterGap);
    L.grMeterW = 33;                                  // +10%
    L.grMeterX = knobRight + gapRideToKnob;
    L.outMeterX = L.grMeterX + L.grMeterW + gapInToRide;
    L.trueBarY = L.contentY + L.meterH + 74;              // +20px bottom padding around knob
    L.trueBarH = 24;
    L.kcY = L.trueBarY + L.trueBarH + 22;              // 12px + 10px extra padding
    L.kcH = 104;
    L.tlY = L.kcY + L.kcH + 12;
    L.tlH = 155;
    L.tlW = L.w - 32;
    L.panelY = L.tlY + L.tlH + 12;
    L.panelH = 140;
    L.barW = 200;
    L.barX = L.contentCx - L.barW / 2 - 30;
    L.readoutX = L.w - 16 - 10 - 46;
}

void SmartCompEditor::paint(juce::Graphics& g)
{
    recalcLayout();
    float s = getScale();
    g.addTransform(juce::AffineTransform::scale(s));
    int w = L.w;
    int h = getBaseHeight() + (advOpen ? ADV_PANEL_H + 12 : 0);

    // Background
    {
        juce::ColourGradient bg(juce::Colour(0xff13161e), 0, 0, C::bg, (float)w * 0.4f, (float)h, false);
        bg.addColour(0.4, C::bg);
        bg.addColour(1.0, juce::Colour(0xff0b0d13));
        g.setGradientFill(bg);
        g.fillRect(0, 0, w, h);
    }
    // Texture
    for (int bx = 0; bx < w; bx += 3) { g.setColour(juce::Colours::white.withAlpha(0.005f)); g.drawVerticalLine(bx, 0, (float)h); }
    for (int by = 0; by < h; by += 4) { g.setColour(juce::Colours::white.withAlpha(0.003f)); g.drawHorizontalLine(by, 0, (float)w); }

    // === HEADER ===
    int headerH = L.headerH;
    {
        juce::ColourGradient hg(juce::Colour(0xff1a1e28), 0, 0, juce::Colour(0xff14171f), 0, (float)headerH, false);
        g.setGradientFill(hg);
        g.fillRect(0, 0, w, headerH);
    }
    g.setColour(C::border); g.fillRect(0, headerH - 1, w, 1);

    // Logo block — SMARTCOMP / bar / tagline
    {
        int logoX = 16;
        int logoY = (headerH - 36) / 2;

        // Widths are measured rather than hardcoded, so the two halves stay
        // flush and the GR bar underneath matches the wordmark exactly whatever
        // the name is. They used to be fixed at 54/53/107, tuned by hand for
        // "GOLD".
        juce::Font logoFont("Arial", 20.0f, juce::Font::bold);
        const int wA = (int)std::ceil(logoFont.getStringWidthFloat(LOGO_A));
        const int wB = (int)std::ceil(logoFont.getStringWidthFloat(LOGO_B));

        int blockW = wA + wB + 6;
        logoRect = juce::Rectangle<int>(logoX, logoY, blockW, 36);

        g.setFont(logoFont);
        g.setColour(C::accent);
        g.drawText(LOGO_A, logoX, logoY - 1, wA + 2, 20, juce::Justification::centredLeft);
        g.setColour(C::white);
        g.drawText(LOGO_B, logoX + wA, logoY - 1, wB + 2, 20, juce::Justification::centredLeft);

        // GR meter bar — spans the wordmark
        float logoBarW = (float)(wA + wB);
        float logoGR = juce::jlimit(0.0f, 1.0f, displayGR / 24.0f);
        float logoFillW = logoBarW * (1.0f - logoGR);
        g.setColour(C::well); g.fillRoundedRectangle((float)logoX, (float)(logoY + 20), logoBarW, 2.0f, 1.0f);
        if (logoFillW > 1.0f) {
            juce::Colour logoCol = logoGR < 0.3f ? C::accent : (logoGR < 0.6f ? C::clipCol : C::gr);
            g.setColour(logoCol.withAlpha(0.85f));
            g.fillRoundedRectangle((float)logoX, (float)(logoY + 20), logoFillW, 2.0f, 1.0f);
        }

        // Tagline
        g.setFont(juce::Font("Arial", 9.5f, juce::Font::plain));
        g.setColour(C::scaleTxt.withAlpha(0.8f));
        g.drawText("confidence compressor", logoX, logoY + 24, blockW, 13, juce::Justification::centredLeft);
    }

    // === MAIN CONTENT ===
    int contentCx = L.contentCx;
    int meterH = L.meterH, meterW = L.meterW, meterGap = L.meterGap;
    int knobSize = L.knobSize;
    int meterX = L.meterX;
    int meterY = L.meterY;

    g.setFont(juce::Font("Arial", 11.0f, juce::Font::bold)); g.setColour(C::label);
    g.drawText("IN", meterX - 4, meterY - 24, meterW * 2 + meterGap + 8, 12, juce::Justification::centred);
    drawMeter(g, meterX, meterY, meterW, meterH, displayInL, peakHoldInL);
    drawMeter(g, meterX + meterW + meterGap, meterY, meterW, meterH, displayInR, peakHoldInR);

    // Scale labels for IN
    g.setFont(juce::Font("Arial", 10.0f, juce::Font::plain));
    for (float db : { 0.0f, -12.0f, -24.0f, -48.0f }) {
        float n = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
        float yp = (float)(meterY + meterH) - n * (float)meterH;
        g.setColour(C::scaleTxt);
        g.drawText(juce::String((int)db), meterX - 26, (int)yp - 5, 22, 10, juce::Justification::centredRight);
    }

    // === RIDE METER (between IN and knob) ===
    {
        bool isRide = processor.rideMode.load();
        // The meter used to show AUTO's offset from the user's knob position.
        // AUTO writes the knob directly now, so there is no offset to show —
        // instead this tracks the sweet-spot band and where the knob sits inside
        // it, which is what makes AUTO's live movement visible.
        const float ssLowV = processor.sweetSpotLow.load();
        const float ssHighV = processor.sweetSpotHigh.load();
        const float compNow = processor.apvts.getRawParameterValue("comp")->load();
        int rideW = L.rideW;
        int rideX = L.rideX;
        int rideTop = L.meterY;
        int rideH = L.meterH;
        float rideCenterY = (float)rideTop + (float)rideH * 0.5f;

        // AUTO is a real button, not a clickable meter. Toggling a mode by
        // clicking its readout is not something a user discovers on their own,
        // and this is the plugin's headline feature — it has to look switchable.
        const juce::Colour rideOn(0xff50c050);
        rideRect = juce::Rectangle<int>(rideX - 6, rideTop - 26, rideW + 12, 18);
        g.setColour(isRide ? rideOn.withAlpha(0.22f) : juce::Colour(0xff1e2330));
        g.fillRoundedRectangle(rideRect.toFloat(), 4.0f);
        g.setColour(isRide ? rideOn.withAlpha(0.75f) : juce::Colour(0xff444a58));
        g.drawRoundedRectangle(rideRect.toFloat(), 4.0f, 1.0f);
        g.setFont(juce::Font("Arial", 10.0f, juce::Font::bold));
        g.setColour(isRide ? rideOn : C::label);
        g.drawText("AUTO", rideRect, juce::Justification::centred);

        // Background bar
        g.setColour(juce::Colour(0xff0d0f14));
        g.fillRoundedRectangle((float)rideX, (float)rideTop, (float)rideW, (float)rideH, 5.0f);
        g.setColour(isRide ? juce::Colour(0xff50c050).withAlpha(0.35f) : C::border);
        g.drawRoundedRectangle((float)rideX, (float)rideTop, (float)rideW, (float)rideH, 5.0f, 1.0f);

        // Scale: knob units, 0 at the bottom to 36 at the top
        const float trackTop = (float)rideTop + 12.0f;
        const float trackBot = (float)(rideTop + rideH) - 12.0f;
        auto yForComp = [&](float v) {
            return trackBot - juce::jlimit(0.0f, 1.0f, v / 36.0f) * (trackBot - trackTop);
        };

        for (float v : { 9.0f, 18.0f, 27.0f }) {
            g.setColour(C::label.withAlpha(0.35f));
            g.drawLine((float)(rideX + 6), yForComp(v), (float)(rideX + rideW - 6), yForComp(v), 0.7f);
        }
        g.setFont(juce::Font("Arial", 7.0f, juce::Font::bold));
        g.setColour(C::label.withAlpha(0.5f));
        g.drawText("36", rideX - 2, rideTop + 2, rideW + 4, 8, juce::Justification::centred);
        g.drawText("0", rideX - 2, rideTop + rideH - 10, rideW + 4, 8, juce::Justification::centred);

        // Sweet-spot band, always drawn: it is the thing AUTO is aiming at, and
        // seeing it drift is what tells you the analysis is live.
        {
            float yHi = yForComp(ssHighV), yLo = yForComp(ssLowV);
            g.setColour(C::accent.withAlpha(isRide ? 0.28f : 0.14f));
            g.fillRoundedRectangle((float)(rideX + 4), yHi,
                                   (float)(rideW - 8), std::max(2.0f, yLo - yHi), 3.0f);
        }

        if (isRide) {
            // Current position — sits inside the band while AUTO is settled, and
            // visibly travels back into it after a manual drag.
            float dotY = yForComp(compNow);
            g.setColour(juce::Colour(0xff50c050).withAlpha(0.30f));
            g.fillEllipse((float)(rideX + rideW / 2 - 6), dotY - 6, 12.0f, 12.0f);
            g.setColour(juce::Colour(0xff50c050));
            g.fillEllipse((float)(rideX + rideW / 2 - 3), dotY - 3, 6.0f, 6.0f);

            g.setFont(juce::Font("Arial", 9.0f, juce::Font::bold));
            g.setColour(juce::Colour(0xff50c050));
            g.drawText(juce::String(compNow, 1), rideX, (int)rideCenterY - 5, rideW, 10,
                       juce::Justification::centred);
        } else {
            // OFF indicator
            g.setFont(juce::Font("Arial", 9.0f, juce::Font::bold));
            g.setColour(C::dim);
            g.drawText("OFF", rideX, (int)rideCenterY - 5, rideW, 10, juce::Justification::centred);
        }

        // Bottom: status dot
        g.setColour(isRide ? juce::Colour(0xff50c050) : C::dim);
        g.fillEllipse((float)(rideX + rideW / 2 - 3), (float)(rideTop + rideH + 6), 6.0f, 6.0f);
    }

    // OUT meters (right of knob)
    int outMeterX = L.outMeterX;
    g.setFont(juce::Font("Arial", 11.0f, juce::Font::bold)); g.setColour(C::label);
    g.drawText("OUT", outMeterX - 4, meterY - 24, meterW * 2 + meterGap + 8, 12, juce::Justification::centred);
    drawMeter(g, outMeterX, meterY, meterW, meterH, displayOutL, peakHoldOutL);
    drawMeter(g, outMeterX + meterW + meterGap, meterY, meterW, meterH, displayOutR, peakHoldOutR);

    // Scale labels for OUT
    g.setFont(juce::Font("Arial", 10.0f, juce::Font::plain));
    for (float db : { 0.0f, -12.0f, -24.0f, -48.0f }) {
        float n = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
        float yp = (float)(meterY + meterH) - n * (float)meterH;
        g.setColour(C::scaleTxt);
        g.drawText(juce::String((int)db), outMeterX + meterW * 2 + meterGap + 4, (int)yp - 5, 22, 10, juce::Justification::centredLeft);
    }

    // Clipping indicator
    bool isClipping = processor.outputClipping.load();
    if (isClipping) {
        float pulseAlpha = 0.5f + 0.5f * std::sin((float)frameCounter * 0.3f);
        g.setColour(C::gr.withAlpha(pulseAlpha));
        g.fillEllipse((float)(outMeterX + meterW / 2 + meterGap / 2 - 3), (float)(meterY - 3), 6.0f, 6.0f);
    }

    // === BIG KNOB (custom drawn) ===
    // AUTO writes the real "comp" parameter, so the knob position IS the value
    // driving the compressor — no separate offset to fold in.
    float effectiveComp = juce::jlimit(0.0f, 36.0f,
        processor.apvts.getRawParameterValue("comp")->load());
    float compNorm = effectiveComp / 36.0f;
    drawBigKnob(g, contentCx, meterY + meterH / 2, knobSize / 2, compNorm, displayGR);

    // === GR METER (vertical, right of knob — mirrors RIDE) ===
    {
        int grX = L.grMeterX;
        int grW = L.grMeterW;
        int grTop = L.meterY;
        int grH = L.meterH;

        grBarRect = juce::Rectangle<int>(grX - 2, grTop - 16, grW + 4, grH + 30);
        vocalStateRect = {};

        // Label
        g.setFont(juce::Font("Arial", 11.0f, juce::Font::bold));
        g.setColour(displayGR > 1.0f ? C::gr : C::label);
        g.drawText("GR", grX - 4, grTop - 24, grW + 8, 12, juce::Justification::centred);

        // Background
        g.setColour(juce::Colour(0xff0d0f14));
        g.fillRoundedRectangle((float)grX, (float)grTop, (float)grW, (float)grH, 5.0f);
        g.setColour(displayGR > 1.0f ? C::gr.withAlpha(0.35f) : C::border);
        g.drawRoundedRectangle((float)grX, (float)grTop, (float)grW, (float)grH, 5.0f, 1.0f);

        // Scale marks at 6, 12, 24 dB
        for (float db : { 6.0f, 12.0f, 24.0f }) {
            float markNorm = std::sqrt(db / 36.0f);  // sqrt-scaled like the old bar
            float markY = (float)grTop + markNorm * (float)grH;
            g.setColour(C::label.withAlpha(0.35f));
            g.drawLine((float)(grX + 6), markY, (float)(grX + grW - 6), markY, 0.5f);
        }

        // Scale labels
        g.setFont(juce::Font("Arial", 7.0f, juce::Font::bold));
        g.setColour(C::label.withAlpha(0.5f));
        g.drawText("0", grX - 2, grTop + 2, grW + 4, 8, juce::Justification::centred);
        g.drawText("-36", grX - 4, grTop + grH - 10, grW + 8, 8, juce::Justification::centred);

        // GR fill (from top down — more GR = taller red bar)
        float grNorm = juce::jlimit(0.0f, 1.0f, std::sqrt(displayGR / 36.0f));
        if (grNorm > 0.005f) {
            float fillH = grNorm * (float)grH;
            g.setColour(C::gr.withAlpha(0.5f));
            g.fillRoundedRectangle((float)(grX + 5), (float)grTop, (float)(grW - 10), fillH, 3.0f);
            g.setColour(C::gr.withAlpha(0.15f));
            g.fillRoundedRectangle((float)(grX + 3), (float)grTop, (float)(grW - 6), fillH + 1, 4.0f);
        }

        // Peak hold
        float grPeakNorm = juce::jlimit(0.0f, 1.0f, std::sqrt(peakHoldGR / 36.0f));
        if (grPeakNorm > 0.01f) {
            float peakY = (float)grTop + grPeakNorm * (float)grH;
            g.setColour(juce::Colour(0xffff6666));
            g.fillRoundedRectangle((float)(grX + 4), peakY - 1, (float)(grW - 8), 2.0f, 1.0f);
        }

        // Value readout
        g.setFont(juce::Font("Arial", 9.0f, juce::Font::bold));
        g.setColour(displayGR > 4.0f ? C::gr : C::label);
        g.drawText(juce::String(-displayGR, 1), grX - 4, grTop + grH / 2 - 5, grW + 8, 10, juce::Justification::centred);

        // Bottom: status indicator
        g.setColour(displayGR > 1.0f ? C::gr : C::dim);
        g.fillEllipse((float)(grX + grW / 2 - 3), (float)(grTop + grH + 6), 6.0f, 6.0f);
    }

    // Shared bar positioning for TRUE bar
    int barW = L.barW;
    int barX = L.barX;
    int readoutX = L.readoutX;

    // === TRUE METER — aligned with IN (left) to OUT (right), compact ===
    int trueCardY = L.trueBarY;
    int trueCardH = 24;  // thinner
    {
        bool trueOn = processor.honestMode.load();
        juce::Colour inactiveCol = juce::Colour(0xff5a6070);

        // Align left edge with IN meters, right edge with OUT meters right
        int trueX = L.meterX - 2;
        int trueRight = L.outMeterX + L.meterW * 2 + L.meterGap + 2;
        int trueW = trueRight - trueX;
        honestBtnRect = juce::Rectangle<int>(trueX, trueCardY, trueW, trueCardH);

        // Background
        g.setColour(juce::Colour(0xff0d0f14));
        g.fillRoundedRectangle((float)trueX, (float)trueCardY, (float)trueW, (float)trueCardH, 4.0f);
        g.setColour(trueOn ? C::accent.withAlpha(0.35f) : C::border);
        g.drawRoundedRectangle((float)trueX, (float)trueCardY, (float)trueW, (float)trueCardH, 4.0f, 1.0f);

        // TRUE label + an explicit switch, for the same reason as AUTO above:
        // the card used to toggle on click with nothing indicating it could.
        g.setFont(juce::Font("Arial", 10.0f, juce::Font::bold));
        g.setColour(trueOn ? C::accent : inactiveCol);
        g.drawText("TRUE LEVEL", trueX + 6, trueCardY, 68, trueCardH, juce::Justification::centredLeft);

        {
            auto sw = juce::Rectangle<float>((float)(trueX + 74), (float)trueCardY + 6.0f, 26.0f, 12.0f);
            g.setColour(trueOn ? C::accent.withAlpha(0.30f) : juce::Colour(0xff1e2330));
            g.fillRoundedRectangle(sw, 6.0f);
            g.setColour(trueOn ? C::accent.withAlpha(0.75f) : juce::Colour(0xff444a58));
            g.drawRoundedRectangle(sw, 6.0f, 1.0f);
            // Knob slides right when on — reads as a switch at a glance
            float kx = trueOn ? sw.getRight() - 10.0f : sw.getX() + 2.0f;
            g.setColour(trueOn ? C::accent : inactiveCol);
            g.fillEllipse(kx, sw.getY() + 2.0f, 8.0f, 8.0f);
        }

        // Inner meter area
        int innerX = trueX + 108;
        int innerRight = trueX + trueW - 52;
        int innerW = innerRight - innerX;
        int innerY = trueCardY + 5;
        int innerH = trueCardH - 10;

        // Scale marks inside
        for (float db : { -30.0f, -20.0f, -10.0f }) {
            float n = juce::jlimit(0.0f, 1.0f, (db + 40.0f) / 40.0f);
            float markX = (float)innerX + n * (float)innerW;
            g.setColour(C::label.withAlpha(0.25f));
            g.drawLine(markX, (float)innerY + 1, markX, (float)(innerY + innerH) - 1, 0.5f);
        }

        auto lufsNorm = [](float lufs) {
            return juce::jlimit(0.0f, 1.0f, (lufs + 40.0f) / 40.0f);
        };
        float inN = lufsNorm(displayInLUFS);
        float outN = lufsNorm(displayOutLUFS);

        if (inN > 0.005f) {
            g.setColour(trueOn ? C::accent.withAlpha(0.2f) : inactiveCol.withAlpha(0.12f));
            g.fillRoundedRectangle((float)innerX, (float)innerY, (float)(innerW * inN), (float)innerH, 2.0f);
        }
        if (outN > 0.005f) {
            g.setColour(trueOn ? C::accent.withAlpha(0.55f) : inactiveCol.withAlpha(0.25f));
            g.fillRoundedRectangle((float)innerX, (float)innerY, (float)(innerW * outN), (float)innerH, 2.0f);
        }

        if (trueOn && inN > 0.01f && outN > 0.01f) {
            float inEndX = (float)innerX + inN * (float)innerW;
            g.setColour(C::accent.withAlpha(0.6f));
            g.drawLine(inEndX, (float)innerY, inEndX, (float)(innerY + innerH), 1.5f);
        }

        // OFFSET readout — right inside
        g.setFont(juce::Font("Arial", 12.0f, juce::Font::bold));
        if (trueOn) {
            juce::String offStr = (displayOffsetDB >= 0 ? "+" : "") + juce::String(displayOffsetDB, 1);
            g.setColour(C::accent);
            g.drawText(offStr, trueX + trueW - 48, trueCardY, 42, trueCardH, juce::Justification::centredRight);
        } else {
            g.setColour(inactiveCol.withAlpha(0.5f));
            g.drawText("OFF", trueX + trueW - 48, trueCardY, 42, trueCardH, juce::Justification::centredRight);
        }
    }

    // === SECONDARY KNOBS CARD ===
    int kcY = L.kcY, kcH = L.kcH;
    {
        auto r = juce::Rectangle<float>(16.0f, (float)kcY, (float)(w - 32), (float)kcH);
        juce::ColourGradient cg(C::card, r.getX(), r.getY(), C::cardBot, r.getX(), r.getBottom(), false);
        g.setGradientFill(cg); g.fillRoundedRectangle(r, 8.0f);
        g.setColour(C::border); g.drawRoundedRectangle(r, 8.0f, 1.0f);
    }

    // ADV button — left side, tall enough to read as a panel toggle
    {
        int pillW2 = 46, pillH2 = 20, pillGap2 = 4;
        int rightTotalH = pillH2 * 3 + pillGap2 * 2;  // 68px
        int pillX2 = 16 + 12;
        int pillY2 = kcY + (kcH - rightTotalH) / 2 + 6;

        advToggleRect = juce::Rectangle<int>(pillX2, pillY2, pillW2, rightTotalH);
        g.setColour(advOpen ? C::accent.withAlpha(0.18f) : juce::Colour(0xff1e2330));
        g.fillRoundedRectangle(advToggleRect.toFloat(), 5.0f);
        g.setColour(advOpen ? C::accent.withAlpha(0.5f) : juce::Colour(0xff444a58));
        g.drawRoundedRectangle(advToggleRect.toFloat(), 5.0f, 1.0f);
        g.setFont(juce::Font("Arial", 11.0f, juce::Font::bold));
        g.setColour(advOpen ? C::accent : C::label);
        g.drawText("ADV", advToggleRect, juce::Justification::centred);

    }

    // === GR TIMELINE + ADV PANEL (both inside collapsible ADV) ===
    grTimelineRect = {};
    grSpeedToggleRect = {};

    if (advOpen)
    {
        // Timeline at top of ADV area
        int tlH = L.tlH;
        int tlW = L.tlW;
        int tlY = L.tlY;
        grTimelineRect = juce::Rectangle<int>(16, tlY, tlW, tlH);
        {
            auto r = juce::Rectangle<float>(16.0f, (float)tlY, (float)tlW, (float)tlH);
            juce::ColourGradient cg(C::card, r.getX(), r.getY(), C::cardBot, r.getX(), r.getBottom(), false);
            g.setGradientFill(cg); g.fillRoundedRectangle(r, 6.0f);
            g.setColour(C::border); g.drawRoundedRectangle(r, 6.0f, 1.0f);
        }
        drawGRTimeline(g, juce::Rectangle<int>(16, tlY, tlW, tlH));

        // ADV knobs panel below timeline
        int panelY = L.panelY;
        int panelH = L.panelH;
        auto panelR = juce::Rectangle<float>(16.0f, (float)panelY, (float)(w - 32), (float)panelH);
        juce::ColourGradient cg2(C::card, panelR.getX(), panelR.getY(), C::cardBot, panelR.getX(), panelR.getBottom(), false);
        g.setGradientFill(cg2); g.fillRoundedRectangle(panelR, 8.0f);
        g.setColour(C::border); g.drawRoundedRectangle(panelR, 8.0f, 1.0f);

        int panelInnerX = 24, panelInnerW = w - 48;

        // Knob area: centred in the panel now that the preset row is gone
        int knobSzAdv = 56;
        int knobBlockH = knobSzAdv + 2 + 14;  // knob + gap + label
        int knobTopY = panelY + (panelH - knobBlockH) / 2 + 6;
        int knobCenterY = knobTopY + knobSzAdv / 2;

        // Knob labels + knobs. The left 66px used to hold the Delta and A/B
        // pills; with those gone the knobs use the full panel width.
        // Two knobs now that HPF is gone — Gate and SC HPF are the only ADV
        // controls left, so they get half the panel width each instead of a third.
        int knobAreaLeft = panelInnerX + 8;
        int knobAreaRight = panelInnerX + panelInnerW;
        int knobAreaW2 = knobAreaRight - knobAreaLeft;
        int colW = knobAreaW2 / 2;
        int row1Y = knobTopY;
        g.setFont(juce::Font("Arial", 10.0f, juce::Font::bold));
        g.setColour(C::label.brighter(0.3f));
        g.drawText("GATE", knobAreaLeft, row1Y - 14, colW, 12, juce::Justification::centred);
        g.drawText("SC HPF", knobAreaLeft + colW, row1Y - 14, colW, 12, juce::Justification::centred);

        gateSlider.setVisible(true); gateLabel.setVisible(true);
        scHpfSlider.setVisible(true); scHpfLabel.setVisible(true);
    }
    else
    {
        gateSlider.setVisible(false); gateLabel.setVisible(false);
        scHpfSlider.setVisible(false); scHpfLabel.setVisible(false);
    }

    // === INFO BUTTON ===
    infoBtnRect = juce::Rectangle<int>(w - 66, (headerH - 18) / 2, 18, 18);
    g.setColour(infoMode ? C::accent.withAlpha(0.7f) : C::accent.withAlpha(0.06f));
    g.fillRoundedRectangle(infoBtnRect.toFloat(), 9.0f);
    g.setColour(infoMode ? C::accent : C::accent.withAlpha(0.3f));
    g.drawRoundedRectangle(infoBtnRect.toFloat(), 9.0f, 1.0f);
    g.setFont(juce::Font("Arial", 11.0f, juce::Font::bold));
    g.setColour(infoMode ? C::white : C::label);
    g.drawText("i", infoBtnRect, juce::Justification::centred);

    // === TRUE MODE GLOW — subtle teal border when active ===
    bool bypassed = processor.apvts.getRawParameterValue("bypass")->load() > 0.5f;
    if (processor.honestMode.load() && !bypassed) {
        float pulse = 0.5f + 0.2f * std::sin((float)frameCounter * 0.08f);
        g.setColour(C::accent.withAlpha(0.08f * pulse));
        g.drawRect(0, 0, w, h, 2);
    }

    // === BYPASS OVERLAY ===
    if (bypassed && !showInfo) {
        g.setColour(juce::Colour(0x90000000)); g.fillRect(0, 0, w, h);
        g.setFont(juce::Font("Arial", 18.0f, juce::Font::bold));
        g.setColour(C::white.withAlpha(0.3f));
        g.drawText("BYPASS", 0, 0, w, h, juce::Justification::centred);
    }

    // === INFO OVERLAY — drawn in paintOverChildren() so it covers the knobs ===

    // === TOOLTIP — painted LAST, always on top of everything ===
    if (infoMode && hoveredElement.isNotEmpty())
    {
        auto tip = getTooltipFor(hoveredElement);
        if (tip.title.isNotEmpty())
        {
            int tipW = 320;
            int tw2 = tipW - 28;

            // Auto-size: measure actual text heights
            juce::Font titleFont("Arial", 16.0f, juce::Font::bold);
            juce::Font descFont("Arial", 14.0f, juce::Font::plain);
            juce::Font techFont("Arial", 13.0f, juce::Font::plain);

            int titleH2 = 22;
            int descH2 = (int)std::ceil(descFont.getStringWidthFloat(tip.desc) / (float)tw2) * 18 + 4;
            descH2 = juce::jmax(descH2, 36);
            int techH2 = (int)std::ceil(techFont.getStringWidthFloat(tip.tech) / (float)tw2) * 16 + 4;
            techH2 = juce::jmax(techH2, 20);
            int tipH = 14 + titleH2 + 8 + descH2 + 12 + techH2 + 14;

            juce::Rectangle<int> elBounds;
            auto unscaleRect = [&](juce::Rectangle<int> r) {
                float sc = getScale();
                return juce::Rectangle<int>((int)(r.getX() / sc), (int)(r.getY() / sc),
                                             (int)(r.getWidth() / sc), (int)(r.getHeight() / sc));
            };
            if (hoveredElement == "comp") elBounds = unscaleRect(compSlider.getBounds());
            else if (hoveredElement == "gate") elBounds = unscaleRect(gateSlider.getBounds());
            else if (hoveredElement == "gain") elBounds = unscaleRect(gainSlider.getBounds());
            else if (hoveredElement == "mix") elBounds = unscaleRect(mixSlider.getBounds());
            else if (hoveredElement == "intrim") elBounds = unscaleRect(inTrimSlider.getBounds());
            else if (hoveredElement == "schpf") elBounds = unscaleRect(scHpfSlider.getBounds());
            else if (hoveredElement == "true") elBounds = honestBtnRect;
            else if (hoveredElement == "ride") elBounds = rideRect;
            else if (hoveredElement == "adv") elBounds = advToggleRect;
            else if (hoveredElement == "gr") elBounds = grBarRect;
            else if (hoveredElement == "grtimeline") elBounds = grTimelineRect;
            else if (hoveredElement == "vocalstate") elBounds = vocalStateRect;

            // RULE: Tooltips must not overlap with JUCE slider components
            // (they render as children ON TOP of paint).
            // Compute knob card Y in base coordinates to check overlap.
            int tipX = juce::jlimit(8, w - tipW - 8, elBounds.getCentreX() - tipW / 2);
            int tipY = elBounds.getY() - tipH - 6;
            if (tipY < 52) tipY = 52;
            // Don't overlap knob card (JUCE sliders render on top)
            if (tipY + tipH > L.kcY)
                tipY = juce::jmax(52, L.kcY - tipH - 6);

            g.setColour(juce::Colour(0xf5101420));
            g.fillRoundedRectangle((float)tipX, (float)tipY, (float)tipW, (float)tipH, 8.0f);
            g.setColour(C::accent.withAlpha(0.35f));
            g.drawRoundedRectangle((float)tipX, (float)tipY, (float)tipW, (float)tipH, 8.0f, 1.0f);

            int ttx = tipX + 14, tty = tipY + 14;
            g.setFont(titleFont);
            g.setColour(C::white);
            g.drawText(tip.title, ttx, tty, tw2, titleH2, juce::Justification::centredLeft);
            tty += titleH2 + 8;
            g.setFont(descFont);
            g.setColour(C::scaleTxt);
            g.drawFittedText(tip.desc, ttx, tty, tw2, descH2, juce::Justification::topLeft, 8);
            tty += descH2 + 5;
            g.setColour(C::accent.withAlpha(0.25f)); g.fillRect(ttx, tty, tw2, 1); tty += 7;
            g.setFont(techFont);
            g.setColour(C::accent.withAlpha(0.7f));
            g.drawFittedText(tip.tech, ttx, tty, tw2, techH2, juce::Justification::topLeft, 6);
        }
    }

    // === REDRAW PILL BUTTONS ON TOP OF TOOLTIP ===
    // Pills are paint()-drawn and get covered by tooltip background.
    // Redraw them so they're always visible.
    {
        auto drawPill = [&](juce::Rectangle<int> r, const juce::String& text, bool active, juce::Colour activeCol) {
            g.setColour(active ? activeCol.withAlpha(0.2f) : juce::Colour(0xff1e2330));
            g.fillRoundedRectangle(r.toFloat(), 5.0f);
            g.setColour(active ? activeCol.withAlpha(0.6f) : juce::Colour(0xff444a58));
            g.drawRoundedRectangle(r.toFloat(), 5.0f, 1.0f);
            g.setFont(juce::Font("Arial", 9.0f, juce::Font::bold));
            g.setColour(active ? activeCol : C::label);
            g.drawText(text, r, juce::Justification::centred);
        };


        // Left pills
        drawPill(advToggleRect, "ADV", advOpen, C::accent);

    }
}

// ============== INFO OVERLAY ==============
void SmartCompEditor::setControlsInteractive(bool on)
{
    for (auto* s : { &compSlider, &gateSlider, &gainSlider,
                     &mixSlider, &inTrimSlider, &scHpfSlider })
        s->setInterceptsMouseClicks(on, on);
    bypassBtn.setInterceptsMouseClicks(on, on);
}

// Painted after child components so it covers the knobs (In Trim, Mix, Out).
void SmartCompEditor::paintOverChildren(juce::Graphics& g)
{
    if (!showInfo) return;

    g.addTransform(juce::AffineTransform::scale(getScale()));
    int w = L.w;
    int h = getBaseHeight() + (advOpen ? ADV_PANEL_H + 12 : 0);

    g.setColour(juce::Colour(0xe0080a10)); g.fillRect(0, 0, w, h);
    auto baseBounds = juce::Rectangle<float>(0, 0, (float)w, (float)h);
    auto oc = baseBounds.withSizeKeepingCentre(230, 160);
    g.setColour(C::card); g.fillRoundedRectangle(oc, 8.0f);
    g.setColour(C::border); g.drawRoundedRectangle(oc, 8.0f, 1.0f);
    float cy = oc.getY() + 16;
    g.setFont(juce::Font("Arial", 20.0f, juce::Font::bold));
    g.setColour(C::accent); g.drawText(LOGO_A, (int)oc.getX(), (int)cy, (int)oc.getWidth(), 20, juce::Justification::centred);
    g.setColour(C::white); g.drawText(LOGO_B, (int)oc.getX(), (int)(cy + 24), (int)oc.getWidth(), 20, juce::Justification::centred);
    g.setFont(juce::Font("Arial", 11.0f, juce::Font::plain)); g.setColour(C::label);
    g.drawText("Frank Knebel-Janssen", (int)oc.getX(), (int)(cy + 60), (int)oc.getWidth(), 14, juce::Justification::centred);
    g.setColour(C::dim);
    g.drawText(juce::CharPointer_UTF8("\xc2\xa9 2026"), (int)oc.getX(), (int)(cy + 76), (int)oc.getWidth(), 14, juce::Justification::centred);
    g.setFont(juce::Font("Arial", 9.0f, juce::Font::plain)); g.setColour(C::dim);
    g.drawText("Click anywhere to close", (int)oc.getX(), (int)(cy + 110), (int)oc.getWidth(), 12, juce::Justification::centred);
}

// ============== BIG KNOB ==============
void SmartCompEditor::drawBigKnob(juce::Graphics& g, int cx, int cy, int radius, float normVal, float grDB)
{
    float startAngle = -2.356f; // -135 deg
    float endAngle = 2.356f;    // +135 deg
    float angle = startAngle + normVal * (endAngle - startAngle);
    float outerR = (float)radius;
    float arcR = outerR - 14.0f;
    float bodyR = outerR - 24.0f;
    float innerR = bodyR - 8.0f;
    int TEETH = 72;

    auto ptc = [&](float r, float a) -> juce::Point<float> {
        float rad = a - juce::MathConstants<float>::halfPi;
        return { (float)cx + r * std::cos(rad), (float)cy + r * std::sin(rad) };
    };

    // Drop shadow
    g.setColour(juce::Colour(0x30000000));
    g.fillEllipse((float)cx - outerR - 4, (float)cy - outerR + 2, (outerR + 4) * 2, (outerR + 4) * 2);

    // Knurled edge — 72 teeth
    {
        juce::Path teeth;
        for (int i = 0; i < TEETH; ++i) {
            float a1 = (float)i / TEETH * juce::MathConstants<float>::twoPi;
            float a2 = ((float)i + 0.5f) / TEETH * juce::MathConstants<float>::twoPi;
            auto o = juce::Point<float>((float)cx + outerR * std::cos(a1), (float)cy + outerR * std::sin(a1));
            auto inn = juce::Point<float>((float)cx + (outerR - 4.0f) * std::cos(a2), (float)cy + (outerR - 4.0f) * std::sin(a2));
            if (i == 0) teeth.startNewSubPath(o);
            else teeth.lineTo(o);
            teeth.lineTo(inn);
        }
        teeth.closeSubPath();
        juce::ColourGradient eg(juce::Colour(0xff4a4e5a), (float)cx, (float)(cy - outerR),
                                juce::Colour(0xff1e2128), (float)cx, (float)(cy + outerR), false);
        g.setGradientFill(eg);
        g.fillPath(teeth);
    }
    // Edge highlight
    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.drawEllipse((float)cx - outerR + 2, (float)cy - outerR + 2, (outerR - 2) * 2, (outerR - 2) * 2, 0.5f);

    // Track arc
    {
        juce::Path track;
        track.addCentredArc((float)cx, (float)cy, arcR, arcR, 0, startAngle, endAngle, true);
        g.setColour(C::well);
        g.strokePath(track, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Determine current zone and arc color
    float ssLow = processor.sweetSpotLow.load() / 36.0f;
    float ssHigh = processor.sweetSpotHigh.load() / 36.0f;
    float crushedStart = juce::jmin((processor.sweetSpotHigh.load() + 6.0f) / 36.0f, 1.0f);

    juce::Colour arcCol;
    bool inSweetSpot = false;
    if (normVal < 0.01f) {
        arcCol = C::dim;
    } else if (normVal < ssLow) {
        float approach = normVal / ssLow;
        arcCol = C::dim.interpolatedWith(C::accent, approach * 0.3f);
    } else if (normVal <= ssHigh) {
        arcCol = C::accent;
        inSweetSpot = true;
    } else if (normVal <= crushedStart) {
        arcCol = C::clipCol;
    } else {
        arcCol = C::gr;
    }

    // Smooth sweet spot glow: fades in/out over ~0.3s instead of hard on/off
    float ssTarget = inSweetSpot ? 1.0f : 0.0f;
    sweetSpotGlow += (ssTarget - sweetSpotGlow) * 0.08f;  // ~12 frames to converge
    bool showGlow = sweetSpotGlow > 0.02f;

    // Active arc + glow — color matches zone
    if (normVal > 0.005f) {
        juce::Path active;
        active.addCentredArc((float)cx, (float)cy, arcR, arcR, 0, startAngle, angle, true);
        g.setColour(arcCol.withAlpha(0.25f));
        g.strokePath(active, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(arcCol.withAlpha(0.9f));
        g.strokePath(active, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Sweet Spot breathing glow — smooth fade in/out
        if (showGlow) {
            float breathe = 0.5f + 0.5f * std::sin((float)frameCounter * 0.10f);
            float gf = sweetSpotGlow;  // 0→1 smooth
            g.setColour(C::accent.withAlpha(0.20f * breathe * gf));
            g.strokePath(active, juce::PathStrokeType(24.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour(C::accent.withAlpha((0.12f + 0.18f * breathe) * gf));
            g.strokePath(active, juce::PathStrokeType(14.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    // GR arc (inner, red)
    float grNorm = juce::jlimit(0.0f, 1.0f, grDB / 36.0f);
    if (grNorm > 0.01f && normVal > 0.01f) {
        float grAngle = startAngle + (grNorm * normVal) * (endAngle - startAngle);
        juce::Path grArc;
        grArc.addCentredArc((float)cx, (float)cy, arcR - 8, arcR - 8, 0, startAngle, grAngle, true);
        g.setColour(C::gr.withAlpha(0.4f));
        g.strokePath(grArc, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // ===== SWEET SPOT ARC =====
    // Shows the signal-adaptive optimal compression zone as a glowing arc segment
    {
        float ssLowN = processor.sweetSpotLow.load() / 36.0f;
        float ssHighN = processor.sweetSpotHigh.load() / 36.0f;
        float crushedN = juce::jmin((processor.sweetSpotHigh.load() + 6.0f) / 36.0f, 1.0f);
        float ssR = arcR + 16.0f;  // moved 2px further out from active arc

        // === TIGHT zone arc (gold) — from ssHigh to crushed ===
        {
            float tightStart = startAngle + ssHighN * (endAngle - startAngle);
            float tightEnd = startAngle + crushedN * (endAngle - startAngle);
            bool inTight = normVal > ssHighN && normVal <= crushedN;

            juce::Path tightArc;
            tightArc.addCentredArc((float)cx, (float)cy, ssR, ssR, 0, tightStart, tightEnd, true);
            g.setColour(C::clipCol.withAlpha(inTight ? 0.65f : 0.22f));
            g.strokePath(tightArc, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // === CRUSHED zone arc (red) — from crushed to end ===
        if (crushedN < 0.99f)
        {
            float crushStart = startAngle + crushedN * (endAngle - startAngle);
            float crushEnd = endAngle;
            bool inCrush = normVal > crushedN;

            juce::Path crushArc;
            crushArc.addCentredArc((float)cx, (float)cy, ssR, ssR, 0, crushStart, crushEnd, true);
            g.setColour(C::gr.withAlpha(inCrush ? 0.65f : 0.18f));
            g.strokePath(crushArc, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // === SWEET SPOT zone arc (teal) — the main zone ===
        float ssStartAngle = startAngle + ssLowN * (endAngle - startAngle);
        float ssEndAngle = startAngle + ssHighN * (endAngle - startAngle);

        // Soft glow behind (same radius as arc, wider stroke)
        juce::Path ssGlow;
        ssGlow.addCentredArc((float)cx, (float)cy, ssR, ssR, 0, ssStartAngle, ssEndAngle, true);
        g.setColour(C::accent.withAlpha(0.10f));
        g.strokePath(ssGlow, juce::PathStrokeType(12.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Sweet spot arc
        juce::Path ssArc;
        ssArc.addCentredArc((float)cx, (float)cy, ssR, ssR, 0, ssStartAngle, ssEndAngle, true);

        // Color: brighter when knob is in the zone
        bool inZone = normVal >= ssLowN && normVal <= ssHighN;
        float ssAlpha = inZone ? 0.8f : 0.35f;
        g.setColour(C::accent.withAlpha(ssAlpha));
        g.strokePath(ssArc, juce::PathStrokeType(4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Small marker dots at zone boundaries
        auto ssPt1 = ptc(ssR, ssStartAngle);
        auto ssPt2 = ptc(ssR, ssEndAngle);
        g.setColour(C::accent.withAlpha(ssAlpha * 0.8f));
        g.fillEllipse(ssPt1.x - 2.5f, ssPt1.y - 2.5f, 5, 5);
        g.fillEllipse(ssPt2.x - 2.5f, ssPt2.y - 2.5f, 5, 5);

        // Crushed boundary dot
        float crushAngle = startAngle + crushedN * (endAngle - startAngle);
        auto crushPt = ptc(ssR, crushAngle);
        g.setColour(C::gr.withAlpha(normVal > crushedN ? 0.6f : 0.2f));
        g.fillEllipse(crushPt.x - 2.5f, crushPt.y - 2.5f, 5, 5);
    }

    // ===== COMPRESSION INTENSITY RING =====
    // Color matches ZONE, not GR amount — so teal only in sweet spot
    if (grNorm > 0.005f || ssFlashFrames > 0) {
        float intensityR = outerR + 6.0f;
        float pulse = 0.7f + 0.3f * std::sin((float)frameCounter * 0.15f);

        // Zone-based color (same as arc)
        juce::Colour intensityCol = arcCol;

        // Sweet spot entry flash — bright burst that fades
        float flashBoost = 0.0f;
        if (ssFlashFrames > 0) {
            flashBoost = (float)ssFlashFrames / 20.0f;  // 1.0 → 0.0 over 20 frames
            flashBoost = flashBoost * flashBoost;  // ease-out curve
        }

        float intensityAlpha = juce::jmax(grNorm * 0.5f * pulse, flashBoost * 0.8f);

        // Outer glow
        g.setColour(intensityCol.withAlpha(intensityAlpha * 0.3f));
        g.drawEllipse((float)cx - intensityR - 3, (float)cy - intensityR - 3,
                       (intensityR + 3) * 2, (intensityR + 3) * 2, 6.0f);
        // Mid glow
        g.setColour(intensityCol.withAlpha(intensityAlpha * 0.5f));
        g.drawEllipse((float)cx - intensityR - 1, (float)cy - intensityR - 1,
                       (intensityR + 1) * 2, (intensityR + 1) * 2, 2.5f);
        // Sharp ring
        g.setColour(intensityCol.withAlpha(intensityAlpha * 0.8f));
        g.drawEllipse((float)cx - intensityR, (float)cy - intensityR,
                       intensityR * 2, intensityR * 2, 1.0f);

        // Flash burst — extra wide glow on entry
        if (flashBoost > 0.1f) {
            g.setColour(C::accent.withAlpha(flashBoost * 0.15f));
            g.drawEllipse((float)cx - intensityR - 8, (float)cy - intensityR - 8,
                           (intensityR + 8) * 2, (intensityR + 8) * 2, 10.0f);
        }
    }

    // Scale ticks
    for (int i = 0; i <= 10; ++i) {
        float a = startAngle + (float)i / 10.0f * (endAngle - startAngle);
        bool major = (i % 5 == 0);
        auto p1 = ptc(arcR + 6, a), p2 = ptc(arcR + (major ? 12.0f : 9.0f), a);
        g.setColour(major ? C::white.withAlpha(0.15f) : C::dim.withAlpha(0.3f));
        g.drawLine(p1.x, p1.y, p2.x, p2.y, major ? 1.5f : 0.8f);
    }

    // Knob body — convex dome with brushed metal
    {
        // Base body fill — radial gradient simulating dome (lighter center-top, dark edges)
        juce::ColourGradient bg(juce::Colour(0xff484e5c), (float)cx - bodyR * 0.15f, (float)(cy - bodyR * 0.35f),
                                juce::Colour(0xff12151c), (float)cx, (float)(cy + bodyR), true);
        bg.addColour(0.3, juce::Colour(0xff3c4250));
        bg.addColour(0.55, juce::Colour(0xff2a2e38));
        bg.addColour(0.8, juce::Colour(0xff1a1e26));
        g.setGradientFill(bg);
        g.fillEllipse((float)cx - bodyR, (float)cy - bodyR, bodyR * 2, bodyR * 2);

        // Brushed metal texture — concentric rings
        for (float r = bodyR - 2; r > innerR + 2; r -= 1.5f) {
            float alpha = 0.015f + 0.01f * std::sin(r * 2.3f); // slight shimmer variation
            g.setColour(juce::Colours::white.withAlpha(alpha));
            g.drawEllipse((float)cx - r, (float)cy - r, r * 2, r * 2, 0.3f);
        }

        // Convex highlight — upper left (dome reflection)
        {
            juce::ColourGradient hl(juce::Colours::white.withAlpha(0.12f),
                                     (float)cx - bodyR * 0.3f, (float)(cy - bodyR * 0.3f),
                                     juce::Colours::transparentBlack,
                                     (float)cx + bodyR * 0.3f, (float)(cy + bodyR * 0.1f), true);
            g.setGradientFill(hl);
            g.fillEllipse((float)cx - bodyR, (float)cy - bodyR, bodyR * 2, bodyR * 2);
        }

        // Secondary dome highlight — subtle wide crescent on top
        {
            juce::Path crescentClip;
            crescentClip.addEllipse((float)cx - bodyR, (float)cy - bodyR, bodyR * 2, bodyR * 2);
            g.saveState();
            g.reduceClipRegion(crescentClip);
            juce::ColourGradient topShine(juce::Colours::white.withAlpha(0.08f),
                                          (float)cx, (float)(cy - bodyR * 0.8f),
                                          juce::Colours::transparentBlack,
                                          (float)cx, (float)(cy - bodyR * 0.1f), false);
            g.setGradientFill(topShine);
            g.fillEllipse((float)cx - bodyR * 0.85f, (float)cy - bodyR * 1.1f, bodyR * 1.7f, bodyR * 1.0f);
            g.restoreState();
        }

        // Bottom shadow — darker at the bottom edge
        {
            juce::ColourGradient botSh(juce::Colours::transparentBlack,
                                        (float)cx, (float)(cy + bodyR * 0.2f),
                                        juce::Colours::black.withAlpha(0.25f),
                                        (float)cx, (float)(cy + bodyR), false);
            juce::Path botClip;
            botClip.addEllipse((float)cx - bodyR, (float)cy - bodyR, bodyR * 2, bodyR * 2);
            g.saveState();
            g.reduceClipRegion(botClip);
            g.setGradientFill(botSh);
            g.fillRect((float)cx - bodyR, (float)cy, bodyR * 2, bodyR);
            g.restoreState();
        }
    }

    // Edge highlight arc (top)
    {
        juce::Path hl;
        hl.addCentredArc((float)cx, (float)cy, bodyR, bodyR, 0, -2.8f, -0.35f, true);
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.strokePath(hl, juce::PathStrokeType(1.5f));
    }
    // Edge shadow arc (bottom)
    {
        juce::Path sh;
        sh.addCentredArc((float)cx, (float)cy, bodyR, bodyR, 0, 0.35f, 2.8f, true);
        g.setColour(juce::Colours::black.withAlpha(0.4f));
        g.strokePath(sh, juce::PathStrokeType(2.0f));
    }

    // Grip rings — machined grooves
    for (int i = 0; i < 3; ++i) {
        float r = bodyR - 3.0f - i * 5.0f;
        g.setColour(juce::Colours::black.withAlpha(0.06f));
        g.drawEllipse((float)cx - r, (float)cy - r, r * 2, r * 2, 0.7f);
        g.setColour(juce::Colours::white.withAlpha(0.03f - i * 0.005f));
        g.drawEllipse((float)cx - (r - 0.5f), (float)cy - (r - 0.5f), (r - 0.5f) * 2, (r - 0.5f) * 2, 0.3f);
    }

    // Inner face — recessed with dome shadow
    {
        juce::ColourGradient faceBg(juce::Colour(0xff1c2028), (float)cx, (float)(cy - innerR),
                                     juce::Colour(0xff101418), (float)cx, (float)(cy + innerR), false);
        g.setGradientFill(faceBg);
        g.fillEllipse((float)cx - innerR, (float)cy - innerR, innerR * 2, innerR * 2);
    }
    // Inset shadow ring
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.drawEllipse((float)cx - innerR, (float)cy - innerR, innerR * 2, innerR * 2, 1.2f);
    // Inner highlight (subtle)
    g.setColour(juce::Colours::white.withAlpha(0.03f));
    g.drawEllipse((float)cx - innerR + 1, (float)cy - innerR + 1, (innerR - 1) * 2, (innerR - 1) * 2, 0.5f);

    // Pointer line + glowing dot — color matches zone
    auto p1 = ptc(innerR - 4.0f, angle), p2 = ptc(bodyR - 4.0f, angle);
    g.setColour(arcCol.withAlpha(0.15f));
    g.drawLine(p1.x, p1.y, p2.x, p2.y, 6.0f);
    g.setColour(arcCol.withAlpha(0.4f));
    g.drawLine(p1.x, p1.y, p2.x, p2.y, 3.0f);
    g.setColour(arcCol);
    g.drawLine(p1.x, p1.y, p2.x, p2.y, 2.0f);
    // Dot with glow
    auto pd = ptc(bodyR - 6.0f, angle);
    g.setColour(arcCol.withAlpha(0.2f));
    g.fillEllipse(pd.x - 6.0f, pd.y - 6.0f, 12.0f, 12.0f);
    g.setColour(arcCol);
    g.fillEllipse(pd.x - 3.5f, pd.y - 3.5f, 7.0f, 7.0f);

    float compVal2 = juce::jlimit(0.0f, 36.0f,
        processor.apvts.getRawParameterValue("comp")->load());

    g.setFont(juce::Font("Helvetica Neue", 37.0f, juce::Font::plain));
    g.setColour(C::white);
    g.drawText(juce::String(compVal2, 1), cx - 44, cy - 35, 88, 32, juce::Justification::centred);
    g.setFont(juce::Font("Arial", 10.0f, juce::Font::bold));
    g.setColour(C::label);
    g.drawText("C O M P R E S S I O N", cx - 60, cy + 1, 120, 12, juce::Justification::centred);

    // Zone state text — below COMPRESSION, color matches arc
    {
        float ssLowVal = processor.sweetSpotLow.load();
        float ssHighVal = processor.sweetSpotHigh.load();
        juce::String zoneText;
        if (compVal2 < 1.0f) zoneText = "NATURAL";
        else if (compVal2 < ssLowVal) zoneText = "GENTLE";
        else if (compVal2 <= ssHighVal) zoneText = "SWEET SPOT";
        else if (compVal2 < ssHighVal + 6) zoneText = "TIGHT";
        else zoneText = "CRUSHED";

        g.setFont(juce::Font("Arial", 17.0f, juce::Font::bold));
        g.setColour(inSweetSpot ? C::accent : arcCol);
        g.drawText(zoneText, cx - 66, cy + 15, 132, 17, juce::Justification::centred);
    }
}

// ============== METER ==============
void SmartCompEditor::drawMeter(juce::Graphics& g, int x, int y, int w, int h, float level, float peak)
{
    g.setColour(C::shadow);
    g.fillRoundedRectangle((float)x - 1, (float)y - 1, (float)w + 2, (float)h + 2, 3.5f);
    g.setColour(C::well);
    g.fillRoundedRectangle((float)x, (float)y, (float)w, (float)h, 3.0f);
    g.setColour(juce::Colour(0xff14171d));
    g.drawRoundedRectangle((float)x, (float)y, (float)w, (float)h, 3.0f, 1.0f);

    float db = (level > 1e-10f) ? 20.0f * std::log10(level) : -80.0f;
    float n = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
    float fH = n * (float)h;
    if (fH > 1.0f) {
        juce::ColourGradient mg(C::mRed, (float)x, (float)y, C::mGreen, (float)x, (float)(y + h), false);
        mg.addColour(0.12, C::mRed); mg.addColour(0.25, C::mYellow); mg.addColour(0.45, C::mGreen);
        g.setGradientFill(mg);
        g.fillRoundedRectangle((float)x, (float)(y + h) - fH, (float)w, fH, 3.0f);
    }
    float pdb = (peak > 1e-10f) ? 20.0f * std::log10(peak) : -80.0f;
    float pn = juce::jlimit(0.0f, 1.0f, (pdb + 60.0f) / 60.0f);
    if (pn > 0.01f) {
        g.setColour(C::peak.withAlpha(0.6f));
        g.fillRect((float)x, (float)(y + h) - pn * h, (float)w, 2.0f);
    }
}

// ============== GR TIMELINE ==============
void SmartCompEditor::drawGRTimeline(juce::Graphics& g, juce::Rectangle<int> area)
{
    int x = area.getX(), y = area.getY(), w = area.getWidth(), h = area.getHeight();
    int padTop = 12, padBot = 4, scaleW = 30;
    int plotX = x + scaleW, plotW = w - scaleW - 6;
    int plotY = y + padTop, plotH = h - padTop - padBot;
    if (plotW < 10 || plotH < 10) return;

    float maxGR = 36.0f;
    auto grToNorm = [&](float db) { return juce::jlimit(0.0f, 1.0f, std::sqrt(std::abs(db) / maxGR)); };

    // Split: top 30% GR, bottom 70% waveform
    int grZoneH = (int)(plotH * 0.30f);
    int waveZoneY = plotY + grZoneH;
    int waveZoneH = plotH - grZoneH;
    float wcY = (float)waveZoneY + (float)waveZoneH * 0.5f;  // wave center

    auto dbToH = [&](float db) -> float {
        if (db < -55.0f) return 0.0f;
        return juce::jlimit(0.0f, 1.0f, (db + 55.0f) / 49.0f) * (float)waveZoneH * 0.47f;
    };

    // === BACKGROUND ===
    // Sweet spot band
    float ssTop = (float)plotY + grToNorm(2.0f) * (float)grZoneH;
    float ssBot = (float)plotY + grToNorm(6.0f) * (float)grZoneH;
    g.setColour(C::accent.withAlpha(0.06f));
    g.fillRect((float)plotX, ssTop, (float)plotW, ssBot - ssTop);
    g.setColour(C::accent.withAlpha(0.10f));
    g.drawLine((float)plotX, ssTop, (float)(plotX + plotW), ssTop, 0.5f);
    g.drawLine((float)plotX, ssBot, (float)(plotX + plotW), ssBot, 0.5f);

    // Dividers
    g.setColour(juce::Colour(0x0affffff));
    g.drawLine((float)plotX, (float)waveZoneY, (float)(plotX + plotW), (float)waveZoneY, 0.5f);
    g.setColour(juce::Colour(0x0cffffff));
    g.drawLine((float)plotX, wcY, (float)(plotX + plotW), wcY, 0.5f);

    // Grid
    g.setColour(juce::Colour(0x05ffffff));
    for (int gx = plotX; gx < plotX + plotW; gx += 24)
        g.drawVerticalLine(gx, (float)plotY, (float)(plotY + plotH));

    // GR scale
    g.setFont(juce::Font("Arial", 9.0f, juce::Font::plain));
    for (float db : { 0.0f, -3.0f, -6.0f, -12.0f }) {
        float ly = (float)plotY + grToNorm(db) * (float)grZoneH;
        g.setColour(juce::Colour(0x08ffffff));
        g.drawLine((float)plotX, ly, (float)(plotX + plotW), ly, 0.3f);
        if (db == 0.0f || db == -6.0f || db == -12.0f) {
            g.setColour(C::scaleTxt);
            g.drawText(juce::String((int)db), x + 1, (int)ly - 5, scaleW - 3, 10, juce::Justification::centredRight);
        }
    }

    // === DATA — continuous paths, no gaps, no vectors ===
    const auto& grHist = processor.compressor.getGRHistory();
    const auto& inHist = processor.compressor.inputHistory;
    const auto& outHist = processor.compressor.outputHistory;
    int writePos = processor.compressor.getGRHistoryWritePos();
    int histSize = RVoxCompressor::GR_HISTORY_SIZE;
    int viewSlots = grTimelineFast ? 256 : histSize;


    // Build continuous paths — NO gaps, NO vector allocations
    juce::Path inTop, inBot, outTop, outBot, grPath;
    float peakGRval = 0.0f;
    int peakGRx = 0;

    // Also store heights for fill paths (stack array, max ~500px wide)
    constexpr int MAX_PLOT = 600;
    float inHArr[MAX_PLOT], outHArr[MAX_PLOT];
    int pw = juce::jmin(plotW, MAX_PLOT);

    for (int i = 0; i < pw; ++i) {
        int histIdx = (int)((float)i / (float)pw * (float)viewSlots);
        int absIdx = (writePos - viewSlots + histIdx + histSize * 2) % histSize;
        float px = (float)(plotX + i);

        float inDB = inHist[(size_t)absIdx];
        float ooDB = outHist[(size_t)absIdx];
        float grDB = grHist[(size_t)absIdx];

        // Heights: 0 when no signal (path stays at center = flat line, no artifacts)
        float ih = dbToH(inDB);
        float oh = dbToH(ooDB > -55.0f ? ooDB : inDB);
        inHArr[i] = ih;
        outHArr[i] = oh;

        // Input envelope
        if (i == 0) {
            inTop.startNewSubPath(px, wcY - ih);
            inBot.startNewSubPath(px, wcY + ih);
        } else {
            inTop.lineTo(px, wcY - ih);
            inBot.lineTo(px, wcY + ih);
        }

        // Output envelope
        if (i == 0) {
            outTop.startNewSubPath(px, wcY - oh);
            outBot.startNewSubPath(px, wcY + oh);
        } else {
            outTop.lineTo(px, wcY - oh);
            outBot.lineTo(px, wcY + oh);
        }

        // GR curve
        float grNorm = (inDB > -55.0f) ? grToNorm(grDB) : 0.0f;
        float gy = (float)plotY + grNorm * (float)grZoneH;
        if (i == 0) grPath.startNewSubPath(px, gy);
        else grPath.lineTo(px, gy);

        if (grDB > peakGRval && inDB > -55.0f) { peakGRval = grDB; peakGRx = plotX + i; }
    }

    // === DRAW: Input fill ===
    {
        juce::Path fill(inTop);
        for (int i = pw - 1; i >= 0; --i)
            fill.lineTo((float)(plotX + i), wcY + inHArr[i]);
        fill.closeSubPath();
        g.setColour(juce::Colour(0x10ffffff));
        g.fillPath(fill);
    }
    g.setColour(juce::Colour(0x30ffffff));
    g.strokePath(inTop, juce::PathStrokeType(0.8f, juce::PathStrokeType::curved));
    g.strokePath(inBot, juce::PathStrokeType(0.8f, juce::PathStrokeType::curved));

    // === DRAW: Diff fill (gold, between input and output) ===
    {
        juce::Path diffT(inTop);
        for (int i = pw - 1; i >= 0; --i)
            diffT.lineTo((float)(plotX + i), wcY - outHArr[i]);
        diffT.closeSubPath();
        g.setColour(C::clipCol.withAlpha(0.22f));
        g.fillPath(diffT);

        juce::Path diffB(inBot);
        for (int i = pw - 1; i >= 0; --i)
            diffB.lineTo((float)(plotX + i), wcY + outHArr[i]);
        diffB.closeSubPath();
        g.fillPath(diffB);
    }

    // === DRAW: Output waveform (hero) ===
    {
        juce::Colour outCol = C::accent;

        juce::Path fill(outTop);
        for (int i = pw - 1; i >= 0; --i)
            fill.lineTo((float)(plotX + i), wcY + outHArr[i]);
        fill.closeSubPath();
        g.setColour(outCol.withAlpha(0.10f));
        g.fillPath(fill);

        g.setColour(outCol.withAlpha(0.15f));
        g.strokePath(outTop, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));
        g.strokePath(outBot, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));
        g.setColour(outCol.withAlpha(0.80f));
        g.strokePath(outTop, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved));
        g.strokePath(outBot, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved));
    }

    // === DRAW: GR curve ===
    {
        juce::Path grFill(grPath);
        grFill.lineTo((float)(plotX + pw - 1), (float)plotY);
        grFill.lineTo((float)plotX, (float)plotY);
        grFill.closeSubPath();
        g.setColour(C::gr.withAlpha(0.06f));
        g.fillPath(grFill);
        g.setColour(C::gr.withAlpha(0.20f));
        g.strokePath(grPath, juce::PathStrokeType(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(C::gr.withAlpha(0.80f));
        g.strokePath(grPath, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (peakGRval > 1.0f) {
            float peakPy = (float)plotY + grToNorm(peakGRval) * (float)grZoneH;
            g.setColour(C::gr.withAlpha(0.12f));
            g.fillEllipse((float)peakGRx - 4, peakPy - 4, 8.0f, 8.0f);
            g.setColour(C::gr);
            g.fillEllipse((float)peakGRx - 2, peakPy - 2, 4.0f, 4.0f);
        }
    }

    // Sweet Spot label
    {
        float ssY = (ssTop + ssBot) * 0.5f;
        g.setFont(juce::Font("Arial", 8.0f, juce::Font::bold));
        g.setColour(C::accent.withAlpha(0.10f));
        g.drawText("SWEET SPOT", plotX + plotW - 70, (int)ssY - 5, 66, 10, juce::Justification::centredRight);
    }

    // Legend
    {
        int lx = plotX + 9, ly = plotY + plotH - 12;
        g.setFont(juce::Font("Arial", 9.0f, juce::Font::bold));
        g.setColour(juce::Colour(0x30ffffff));
        g.fillRect((float)lx, (float)(ly + 1), 12.0f, 8.0f);
        g.setColour(juce::Colour(0x70ffffff));
        g.drawText("IN", lx + 15, ly, 16, 10, juce::Justification::centredLeft);
        g.setColour(C::accent.withAlpha(0.5f));
        g.fillRect((float)(lx + 34), (float)(ly + 1), 12.0f, 8.0f);
        g.setColour(C::accent);
        g.drawText("OUT", lx + 49, ly, 24, 10, juce::Justification::centredLeft);
        g.setColour(C::clipCol.withAlpha(0.35f));
        g.fillRect((float)(lx + 76), (float)(ly + 1), 12.0f, 8.0f);
        g.setColour(C::clipCol);
        g.drawText("DIFF", lx + 91, ly, 28, 10, juce::Justification::centredLeft);
        g.setColour(C::gr.withAlpha(0.85f));
        g.drawLine((float)(lx + 122), (float)(ly + 5), (float)(lx + 134), (float)(ly + 5), 1.5f);
        g.setColour(C::gr);
        g.drawText("GR", lx + 137, ly, 18, 10, juce::Justification::centredLeft);
    }

    // Speed toggle
    {
        int togW = 36, togH = 14;
        int togX = plotX + plotW - togW - 2;
        int togY = y + h - togH - 10;
        grSpeedToggleRect = juce::Rectangle<int>(togX, togY, togW, togH);
        g.setColour(grTimelineFast ? C::accent.withAlpha(0.15f) : juce::Colour(0x10ffffff));
        g.fillRoundedRectangle(grSpeedToggleRect.toFloat(), 3.0f);
        g.setColour(grTimelineFast ? C::accent.withAlpha(0.4f) : C::border.withAlpha(0.4f));
        g.drawRoundedRectangle(grSpeedToggleRect.toFloat(), 3.0f, 0.5f);
        g.setFont(juce::Font("Arial", 8.0f, juce::Font::bold));
        g.setColour(grTimelineFast ? C::accent : C::label);
        g.drawText(grTimelineFast ? "FAST" : "SLOW", grSpeedToggleRect, juce::Justification::centred);
    }
}

// ============== RESIZED ==============
void SmartCompEditor::resized()
{
    recalcLayout();
    float s = getScale();
    int totalBaseH = getBaseHeight() + (advOpen ? ADV_PANEL_H + 12 : 0);
    int targetH = (int)(totalBaseH * s);
    if (std::abs(getHeight() - targetH) > 1)
        setSize(getWidth(), targetH);

    bypassBtn.setBounds(S(L.w - 40), S(9), S(30), S(30));

    int knobCy = L.meterY + L.meterH / 2;
    compSlider.setBounds(S(L.contentCx - L.knobSize / 2), S(knobCy - L.knobSize / 2), S(L.knobSize), S(L.knobSize));
    compSlider.setAlpha(0.0f);
    compLabel.setVisible(false);

    // Secondary knobs — the main row under the big knob
    int knobSz = 66;
    int numKnobs = 3;
    int knobAreaW = L.w - 32 - L.knobLeftMargin - L.knobRightMargin;
    int colW = knobAreaW / numKnobs;
    int kBaseY = L.kcY + (L.kcH - knobSz - 16) / 2 + 6;
    int knobStartX = 16 + L.knobLeftMargin;

    auto placeKnob = [&](juce::Slider& sl, juce::Label& l, int col) {
        int kx = knobStartX + col * colW + (colW - knobSz) / 2;
        sl.setBounds(S(kx), S(kBaseY), S(knobSz), S(knobSz));
        l.setBounds(S(kx - 10), S(kBaseY + knobSz + 2), S(knobSz + 20), S(14));
    };

    placeKnob(inTrimSlider, inTrimLabel, 0);
    placeKnob(mixSlider, mixLabel, 1);
    placeKnob(gainSlider, gainLabel, 2);

    if (!advOpen) {
        gateSlider.setVisible(false); gateLabel.setVisible(false);
    }

    // ADV panel — two knobs (Gate, SC HPF) since HPF was removed
    if (advOpen) {
        int panelTopY = L.panelY;
        int advKnobSz = 56;
        int panelX = 24;
        int advPanelW = L.w - 48;
        int knobAreaLeft = panelX + 8;
        int knobAreaRight = panelX + advPanelW;
        int knobAreaW2 = knobAreaRight - knobAreaLeft;
        int colW = knobAreaW2 / 2;

        int knobBlockH = advKnobSz + 2 + 14;
        int knobTopY = panelTopY + (L.panelH - knobBlockH) / 2 + 6;

        auto placeAdvKnob = [&](juce::Slider& sl, juce::Label& l, int col) {
            int kx = knobAreaLeft + col * colW + (colW - advKnobSz) / 2;
            sl.setBounds(S(kx), S(knobTopY), S(advKnobSz), S(advKnobSz));
            l.setBounds(S(kx - 14), S(knobTopY + advKnobSz + 2), S(advKnobSz + 28), S(14));
        };

        placeAdvKnob(gateSlider, gateLabel, 0);
        placeAdvKnob(scHpfSlider, scHpfLabel, 1);

        gateSlider.setVisible(true); gateLabel.setVisible(true);
        scHpfSlider.setVisible(true); scHpfLabel.setVisible(true);
    }
}

// ============== A/B COMPARISON ==============
// ============== TOOLTIP INFO ==============
SmartCompEditor::TooltipInfo SmartCompEditor::getTooltipFor(const juce::String& el)
{
    if (el == "comp") return {
        "Compression",
        "Single-knob control for threshold, ratio, knee, and auto-makeup gain. "
        "Internally maps to a multi-stage DSP chain: adaptive soft knee, "
        "program-dependent dual release, cosine-interpolated lookahead, "
        "anti-aliased gain staging. The Sweet Spot Arc indicates the "
        "signal-adaptive optimal compression zone.",
        "Threshold: -6 to -36 dB (quadratic) | Ratio: 1:1 to 10:1\n"
        "Adaptive knee: 6 dB | RMS-dominant detector (consonant-safe)\n"
        "64-sample cosine lookahead | Internal attack: 0.1ms\n"
        "Program-dependent release scales with comp amount\n"
        "Subtle 2nd harmonic warmth scales with compression\n"
        "Fixed 2x oversampling | Magnetic snap at zone boundaries"
    };
    if (el == "gate") return {
        "Gate",
        "Noise gate with 6 dB hysteresis. Attenuates signal below "
        "the threshold by up to 30 dB. Useful for removing background noise "
        "between vocal phrases.",
        "Threshold: -80 (OFF) to -20 dB | Hysteresis: 6 dB\n"
        "Open: 0.5ms | Close: 30ms | Range: 30 dB"
    };
    if (el == "gain") return {
        "Output Gain",
        "Manual output level adjustment applied after compression, "
        "limiter stages. Additive to auto-makeup gain.",
        "Range: -36 to 0 dB | Post-limiter in signal chain"
    };
    if (el == "mix") return {
        "Dry/Wet Mix",
        "Blends unprocessed and compressed signal for parallel compression. "
        "The dry path is latency-compensated for phase-coherent mixing.",
        "Range: 0-100% | Latency-compensated dry path\n"
        "50% = standard parallel compression blend"
    };
    if (el == "gr") return {
        "Gain Reduction",
        "Displays current gain reduction in dB with peak hold. "
        "The bar is sqrt-scaled for better visibility in the 2-6 dB range "
        "where most vocal compression happens.",
        "Range: 0 to -36 dB | Sqrt-scaled display\n"
        "Peak hold: 20 frames with 0.85 decay"
    };
    if (el == "grtimeline") return {
        "Confidence Display",
        "Scrolling timeline with four layers: GR curve (red, top), "
        "input envelope (grey), output envelope (teal), and the difference "
        "fill (gold) showing what compression removed. "
        "The Sweet Spot band marks the 2-6 dB GR zone. "
        "Toggle FAST/SLOW for different time windows.",
        "2048-slot ring buffer | GR zone: sqrt-scaled 0-36 dB\n"
        "Waveform zone: mirrored envelope around center line\n"
        "SLOW: ~24s visible | FAST: ~3s visible"
    };
    if (el == "vocalstate") return {
        "Vocal State",
        "Analyzes the input signal's crest factor and dynamic range to "
        "determine the current compression zone. Boundaries adapt to the "
        "specific signal over a ~2 second analysis window.",
        "NATURAL: no compression | GENTLE: light leveling\n"
        "SWEET SPOT: signal-adaptive optimal zone\n"
        "TIGHT: audible compression | CRUSHED: extreme limiting\n"
        "Boundaries update via crest factor analysis"
    };
    if (el == "true") return {
        "True Level Match",
        "Matches output loudness to input using K-weighted LUFS measurement. "
        "Removes the loudness bias so you evaluate only the tonal and dynamic "
        "changes the compressor introduces. The OFFSET readout shows "
        "how many dB of compensation is applied.",
        "ITU-R BS.1770 K-weighted LUFS | 300ms smoothing window\n"
        "Slew limit: 1 dB/50ms | Safety clamp: +6 dB max boost\n"
        "Measurement point: pre-match (no feedback loop)"
    };
    if (el == "ride") return {
        "AUTO Mode",
        "Automatically keeps the compressor in the Sweet Spot. "
        "Tracks input dynamics and moves the knob in real time. "
        "Drag the knob while AUTO is on and it springs back to the "
        "Sweet Spot the moment you let go. "
        "Ignores breaths and pauses to stay stable.",
        "Tracking: asymmetric (fast up 0.1s, slow down 0.4s)\n"
        "Gate: -35 dBFS (ignores breaths)\n"
        "Target: Sweet Spot center, continuously updated"
    };
    if (el == "adv") return {
        "Advanced Panel",
        "Additional controls: Gate and Sidechain HPF.",
        "All settings are saved with the DAW session"
    };
    if (el == "meters") return {
        "Level Meters",
        "Input and output peak levels with peak hold indicators.",
        "Range: -60 to 0 dBFS | 60fps update rate"
    };
    if (el == "intrim") return {
        "Input Trim",
        "Adjusts input level before all processing stages. "
        "Use to optimize the compressor's operating point for "
        "different source levels.",
        "Range: -12 to +12 dB | First stage in signal chain"
    };
    if (el == "schpf") return {
        "Sidechain HPF",
        "Filters low frequencies from the compressor's detector only. "
        "The audio signal passes through unaffected. Prevents bass content "
        "from triggering gain reduction, reducing pumping on bass-heavy material.",
        "Type: 12 dB/oct Butterworth (2nd order) | Range: OFF to 400 Hz\n"
        "Detector-only: audio path is not filtered\n"
        "Typical: 80-120 Hz for vocals, 150-200 Hz for full mix"
    };
    return { "", "", "" };
}

// ============== MOUSE MOVE / EXIT ==============
void SmartCompEditor::mouseMove(const juce::MouseEvent&) {}
void SmartCompEditor::mouseExit(const juce::MouseEvent&) {}
