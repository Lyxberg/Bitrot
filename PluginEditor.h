#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Skin.h"

//==============================================================================
class BitrotAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                                private juce::Timer
{
public:
    explicit BitrotAudioProcessorEditor (BitrotAudioProcessor&);
    ~BitrotAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    juce::Rectangle<float> getScopeBounds() const { return scopeBounds; }

private:
    void timerCallback() override;
    void drawScope (juce::Graphics&, juce::Rectangle<float>);

    //==========================================================================
    struct LnF final : juce::LookAndFeel_V4
    {
        void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                               float sliderPos, float startAngle, float endAngle,
                               juce::Slider&) override;

        void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                               float sliderPos, float minSliderPos, float maxSliderPos,
                               juce::Slider::SliderStyle, juce::Slider&) override;
    };

    BitrotAudioProcessor& processorRef;
    AudioDev::Skin skin;
    LnF lnf;

    juce::Slider lossSlider, speedSlider, fillSlider, holesSlider, mixSlider, outSlider;
    juce::Label  lossLabel, speedLabel, fillLabel, holesLabel, mixLabel;
    juce::Label  modeLabel, weightLabel, fftLabel, stereoLabel, outLabel;
    juce::ComboBox modeBox, weightBox, fftBox, stereoBox;

    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    std::unique_ptr<SliderAtt> lossAtt, speedAtt, fillAtt, holesAtt, mixAtt, outAtt;
    std::unique_ptr<ComboAtt>  modeAtt, weightAtt, fftAtt, stereoAtt;

    juce::Rectangle<float> scopeBounds;
    juce::Rectangle<float> stripBounds;
    BitrotAudioProcessor::ScopeSnapshot snapshot;

    std::vector<float> dispIn, dispOut;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BitrotAudioProcessorEditor)
};
