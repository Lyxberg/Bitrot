cmake_minimum_required(VERSION 3.22)

project(Bitrot VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MINGW)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    message(FATAL_ERROR "JUCE not found. Put the JUCE source in ${CMAKE_CURRENT_SOURCE_DIR}/JUCE")
endif()

juce_add_plugin(Bitrot
    COMPANY_NAME            "BergAudio"
    BUNDLE_ID               "com.bergaudio.bitrot"
    PLUGIN_MANUFACTURER_CODE Berg
    PLUGIN_CODE             Btrt
    FORMATS                 VST3 Standalone
    PRODUCT_NAME            "Bitrot"
    DESCRIPTION             "Bitrot - spectral codec-artifact model (prototype)."
    IS_SYNTH                FALSE
    IS_MIDI_EFFECT          FALSE
    NEEDS_MIDI_INPUT        FALSE
    NEEDS_MIDI_OUTPUT       FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_IS_FREEWARE      TRUE
    VST3_CATEGORIES         Fx Distortion)

juce_generate_juce_header(Bitrot)

target_include_directories(Bitrot PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../RiderSystemKits/_shared")

target_sources(Bitrot PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Bitrot PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

target_link_libraries(Bitrot
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags)

#===============================================================================
# Offline self-test: runs the actual AudioProcessor on synthetic signals and
# prints latency / gain / hole statistics. Not shipped.
option(SPECTRAL_LOSSY_BUILD_TEST "Build the offline DSP self-test" ON)

if(SPECTRAL_LOSSY_BUILD_TEST)
    juce_add_console_app(BitrotTest PRODUCT_NAME "BitrotTest")
    juce_generate_juce_header(BitrotTest)

    target_include_directories(BitrotTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Source"
        "${CMAKE_CURRENT_SOURCE_DIR}/../RiderSystemKits/_shared")

    target_sources(BitrotTest PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Test/EngineTest.cpp
        Test/Analyzer.cpp)

    target_compile_definitions(BitrotTest PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_REPORT_APP_USAGE=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JucePlugin_Name="Bitrot")

    target_link_libraries(BitrotTest
        PRIVATE
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_dsp
            juce::juce_gui_basics
        PUBLIC
            juce::juce_recommended_config_flags)
endif()
