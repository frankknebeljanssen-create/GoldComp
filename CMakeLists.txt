cmake_minimum_required(VERSION 3.15)

project(GoldComp VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE Framework — kept outside the repo (and outside Dropbox) by default
set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/JUCE" CACHE PATH "Path to the JUCE checkout")
add_subdirectory("${JUCE_PATH}" "${CMAKE_BINARY_DIR}/JUCE")

juce_add_plugin(GoldComp
    COMPANY_NAME "Frank Knebel"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE FkAu
    PLUGIN_CODE GlCp
    FORMATS VST3 AU
    PRODUCT_NAME "GoldComp")

target_sources(GoldComp
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
        Source/DSP/RVoxCompressor.cpp
        Source/DSP/RVoxCompressor.h
        Source/DSP/LookaheadLimiter.cpp
        Source/DSP/LookaheadLimiter.h)

target_compile_definitions(GoldComp
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(GoldComp
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
