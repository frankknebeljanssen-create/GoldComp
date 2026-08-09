cmake_minimum_required(VERSION 3.15)

project(SmartComp VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE Framework — kept outside the repo (and outside Dropbox) by default
set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/JUCE" CACHE PATH "Path to the JUCE checkout")
add_subdirectory("${JUCE_PATH}" "${CMAKE_BINARY_DIR}/JUCE")

juce_add_plugin(SmartComp
    COMPANY_NAME "Frank Knebel"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE FkAu
    PLUGIN_CODE SmCp
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "SmartComp")

target_sources(SmartComp
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
        Source/DSP/RVoxCompressor.cpp
        Source/DSP/RVoxCompressor.h
        Source/DSP/LookaheadLimiter.cpp
        Source/DSP/LookaheadLimiter.h)

target_compile_definitions(SmartComp
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(SmartComp
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Measurement harness for the DSP classes — lets us put real numbers on
# limiter overshoot, transient response and timing instead of guessing.
juce_add_console_app(dsp_bench PRODUCT_NAME "dsp_bench")
target_sources(dsp_bench PRIVATE tests/dsp_bench.cpp)
target_compile_definitions(dsp_bench PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(dsp_bench
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)

# README screenshot generator. Runs the real processor and editor through a
# genuine JUCE event loop against synthetic audio (see tests/ui_shot.cpp for
# why), then saves a PNG. Links against the SmartComp shared-code target
# rather than recompiling PluginProcessor/PluginEditor, so it inherits the
# JucePlugin_* compile definitions juce_add_plugin generates for it — those
# headers use JucePlugin_Name, and recompiling them in a fresh target without
# juce_add_plugin's generated definitions would need reproducing that by hand.
juce_add_gui_app(ui_shot PRODUCT_NAME "ui_shot")
target_sources(ui_shot PRIVATE tests/ui_shot.cpp)
target_compile_definitions(ui_shot PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(ui_shot
    PRIVATE
        SmartComp
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)

# Behavioural probe for AUTO. dsp_bench covers the DSP in isolation; this one
# drives the whole processor and checks that the comp parameter converges on the
# sweet-spot band, springs back after a manual drag, and recovers after being
# parked in the red with AUTO off — the three cases that were broken when AUTO
# tracked an offset instead of an absolute target.
juce_add_console_app(auto_probe PRODUCT_NAME "auto_probe")
target_sources(auto_probe PRIVATE tests/auto_probe.cpp)
target_compile_definitions(auto_probe PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(auto_probe
    PRIVATE
        SmartComp
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)
