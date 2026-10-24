#include "PluginEditor.h"

#include <array>
#include <cmath>

namespace
{
    const juce::Colour kPink      { 0xfff5007d };   // background
    const juce::Colour kDisplay   { 0xffff6fa8 };   // spectrum panel
    const juce::Colour kStrip     { 0xff1a1a1a };   // control bar
    const juce::Colour kStripBox  { 0xff262626 };
    const juce::Colour kKnob      { 0xffb3005c };
    const juce::Colour kKnobArc   { 0xffffb3d1 };
    const juce::Colour kInk       { 0xff24000f };
    const juce::Colour kInkDim    { 0xff7a0038 };
}

//==============================================================================
void BitrotAudioProcessorEditor::LnF::drawRotarySlider (juce::Graphics& g,
                                                              int x, int y, int width, int height,
                                                              float sliderPos, float startAngle, float endAngle,
                                                              juce::Slider& slider)
{
    auto area = juce::Rectangle<int> (x, y, width, height).toFloat();

    const float d = juce::jmin (area.getWidth(), area.getHeight()) - 2.0f;
    const auto  circle = juce::Rectangle<float> (d, d).withCentre (area.getCentre());

    const auto body  = slider.findColour (juce::Slider::backgroundColourId);
    const auto accent = slider.findColour (juce::Slider::thumbColourId);

    g.setColour (body);
    g.fillEllipse (circle);

    const float angle  = startAngle + sliderPos * (endAngle - startAngle);
    const float radius = d * 0.5f - 4.0f;

    juce::Path arc;
    arc.addCentredArc (circle.getCentreX(), circle.getCentreY(), radius, radius, 0.0f,
                       startAngle, angle, true);

    g.setColour (accent);
    g.strokePath (arc, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));

    const float px = circle.getCentreX() + radius * std::sin (angle);
    const float py = circle.getCentreY() - radius * std::cos (angle);

    g.drawLine (circle.getCentreX(), circle.getCentreY(), px, py, 2.5f);

    g.setColour (juce::Colours::white.withAlpha (0.92f));
    g.setFont (juce::Font (11.5f, juce::Font::bold));
    g.drawText (slider.getTextFromValue (slider.getValue()),
                circle.withTrimmedTop (d * 0.56f).toNearestInt(),
                juce::Justification::centredTop, false);
}

void BitrotAudioProcessorEditor::LnF::drawLinearSlider (juce::Graphics& g,
                                                              int x, int y, int width, int height,
                                                              float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
                                                              juce::Slider::SliderStyle, juce::Slider& slider)
{
    auto r = juce::Rectangle<int> (x, y, width, height).toFloat();
    auto track = r.withSizeKeepingCentre (r.getWidth(), 4.0f);

    g.setColour (juce::Colours::white.withAlpha (0.22f));
    g.fillRoundedRectangle (track, 2.0f);

    auto fill = track.withRight (sliderPos);
    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.fillRoundedRectangle (fill, 2.0f);

    g.setColour (juce::Colours::white);
    g.fillEllipse (juce::Rectangle<float> (10.0f, 10.0f).withCentre ({ sliderPos, track.getCentreY() }));
}

//==============================================================================
BitrotAudioProcessorEditor::BitrotAudioProcessorEditor (BitrotAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    setLookAndFeel (&lnf);

    skin = AudioDev::Skin::load ("Bitrot");
    AudioDev::Skin::ensureFolder ("Bitrot");

    //--- knobs -------------------------------------------------------------
    const auto setupKnob = [this] (juce::Slider& s, juce::Label& l, const juce::String& name)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        s.setColour (juce::Slider::backgroundColourId, kKnob);
        s.setColour (juce::Slider::thumbColourId, kKnobArc);
        s.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colours::transparentBlack);
        s.setColour (juce::Slider::rotarySliderFillColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (s);

        l.setText (name, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centred);
        l.setColour (juce::Label::textColourId, kInk);
        l.setFont (juce::Font (10.0f, juce::Font::bold));
        addAndMakeVisible (l);
    };

    setupKnob (lossSlider,  lossLabel,  "LOSS");
    setupKnob (speedSlider, speedLabel, "SPEED");
    setupKnob (fillSlider,  fillLabel,  "FILL");
    setupKnob (holesSlider, holesLabel, "HOLES");
    setupKnob (mixSlider,   mixLabel,   "MIX");

    lossAtt  = std::make_unique<SliderAtt> (processorRef.apvts, "loss",  lossSlider);
    speedAtt = std::make_unique<SliderAtt> (processorRef.apvts, "speed", speedSlider);
    fillAtt  = std::make_unique<SliderAtt> (processorRef.apvts, "fill",  fillSlider);
    holesAtt = std::make_unique<SliderAtt> (processorRef.apvts, "holes", holesSlider);
    mixAtt   = std::make_unique<SliderAtt> (processorRef.apvts, "mix",   mixSlider);

    //--- combo boxes + output ----------------------------------------------
    const auto setupCombo = [this] (juce::ComboBox& c, juce::Label& l, const juce::String& name,
                                    const juce::String& paramId)
    {
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (processorRef.apvts.getParameter (paramId)))
            c.addItemList (choice->choices, 1);

        c.setColour (juce::ComboBox::backgroundColourId, kStripBox);
        c.setColour (juce::ComboBox::textColourId, juce::Colours::white);
        c.setColour (juce::ComboBox::outlineColourId, juce::Colour (0x22ffffff));
        c.setColour (juce::ComboBox::arrowColourId, juce::Colour (0xbbffffff));
        addAndMakeVisible (c);

        l.setText (name, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centredLeft);
        l.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
        l.setFont (juce::Font (9.5f, juce::Font::bold));
        addAndMakeVisible (l);
    };

    setupCombo (modeBox,   modeLabel,   "MODE",      "mode");
    setupCombo (weightBox, weightLabel, "WEIGHTING", "weighting");
    setupCombo (fftBox,    fftLabel,    "FFT",       "fft");
    setupCombo (stereoBox, stereoLabel, "STEREO",    "stereo");

    modeAtt   = std::make_unique<ComboAtt> (processorRef.apvts, "mode",      modeBox);
    weightAtt = std::make_unique<ComboAtt> (processorRef.apvts, "weighting", weightBox);
    fftAtt    = std::make_unique<ComboAtt> (processorRef.apvts, "fft",       fftBox);
    stereoAtt = std::make_unique<ComboAtt> (processorRef.apvts, "stereo",    stereoBox);

    outSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    outSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 16);
    outSlider.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
    outSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    outSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (outSlider);
    outAtt = std::make_unique<SliderAtt> (processorRef.apvts, "output", outSlider);

    outLabel.setText ("OUTPUT", juce::dontSendNotification);
    outLabel.setJustificationType (juce::Justification::centredLeft);
    outLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
    outLabel.setFont (juce::Font (9.5f, juce::Font::bold));
    addAndMakeVisible (outLabel);

    setResizable (true, true);
    setResizeLimits (700, 440, 1400, 900);
    setSize (780, 480);

    dispIn.assign (2048, -90.0f);
    dispOut.assign (2048, -90.0f);

    startTimerHz (30);
}

BitrotAudioProcessorEditor::~BitrotAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

//==============================================================================
void BitrotAudioProcessorEditor::timerCallback()
{
    repaint (scopeBounds.toNearestInt());
}

//==============================================================================
void BitrotAudioProcessorEditor::resized()
{
    auto b = getLocalBounds().reduced (16);
    b.removeFromTop (36);                              // title

    auto knobRow = b.removeFromBottom (108);
    b.removeFromBottom (10);
    auto strip = b.removeFromBottom (34);
    b.removeFromBottom (12);

    scopeBounds = b.toFloat();
    stripBounds = strip.toFloat();

    //--- strip -------------------------------------------------------------
    auto s = strip.reduced (8, 5);

    const auto placeCombo = [&s] (juce::Label& l, juce::ComboBox& c, int labelW, int boxW)
    {
        l.setBounds (s.removeFromLeft (labelW));
        c.setBounds (s.removeFromLeft (boxW).reduced (0, 1));
        s.removeFromLeft (8);
    };

    placeCombo (modeLabel,   modeBox,   36, 88);
    placeCombo (weightLabel, weightBox, 62, 96);
    placeCombo (fftLabel,    fftBox,    26, 72);
    placeCombo (stereoLabel, stereoBox, 46, 76);

    auto outArea = s;
    outLabel.setBounds (outArea.removeFromLeft (46));
    outSlider.setBounds (outArea);

    //--- knobs -------------------------------------------------------------
    juce::Slider* knobs[5]  { &lossSlider, &speedSlider, &fillSlider, &holesSlider, &mixSlider };
    juce::Label*  labels[5] { &lossLabel,  &speedLabel,  &fillLabel,  &holesLabel,  &mixLabel  };

    const int cell = knobRow.getWidth() / 5;

    for (int i = 0; i < 5; ++i)
    {
        auto area = knobRow.removeFromLeft (cell);

        labels[i]->setBounds (area.removeFromTop (14));
        knobs[i]->setBounds (area.reduced (10, 0));
    }
}

//==============================================================================
void BitrotAudioProcessorEditor::drawScope (juce::Graphics& g, juce::Rectangle<float> r)
{
    processorRef.readScope (snapshot);

    g.setColour (kDisplay);
    g.fillRoundedRectangle (r, 4.0f);

    auto inner = r.reduced (12.0f, 10.0f).withTrimmedBottom (14.0f);

    const float fMin = 20.0f, fMax = 20000.0f;
    const float dbMin = -90.0f, dbMax = 0.0f;

    const auto xAt = [&] (float f)
    {
        return inner.getX() + inner.getWidth()
             * std::log (juce::jmax (f, fMin) / fMin) / std::log (fMax / fMin);
    };
    const auto yAt = [&] (float db)
    {
        return inner.getBottom() - inner.getHeight()
             * (juce::jlimit (dbMin, dbMax, db) - dbMin) / (dbMax - dbMin);
    };

    // grid
    const float freqs[] { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f };
    const char* names[] { "50", "100", "200", "500", "1k", "2k", "5k", "10k" };

    g.setFont (juce::Font (9.0f));

    for (int i = 0; i < 8; ++i)
    {
        const float x = xAt (freqs[i]);
        g.setColour (kInk.withAlpha (0.13f));
        g.drawVerticalLine ((int) x, inner.getY(), inner.getBottom());
        g.setColour (kInk.withAlpha (0.55f));
        g.drawText (names[i], (int) x - 12, (int) inner.getBottom() + 1, 24, 11,
                    juce::Justification::centred, false);
    }

    for (float db : { -60.0f, -40.0f, -20.0f })
    {
        const float y = yAt (db);
        g.setColour (kInk.withAlpha (0.13f));
        g.drawHorizontalLine ((int) y, inner.getX(), inner.getRight());
        g.setColour (kInk.withAlpha (0.45f));
        g.drawText (juce::String ((int) db), (int) inner.getX() + 2, (int) y - 11, 26, 10,
                    juce::Justification::left, false);
    }

    if (! snapshot.valid || snapshot.numBins < 4)
    {
        g.setColour (kInk.withAlpha (0.45f));
        g.setFont (juce::Font (12.0f));
        g.drawText ("no signal", inner, juce::Justification::centred, false);
        return;
    }

    const double nyq   = snapshot.sampleRate * 0.5;
    const int    nb    = snapshot.numBins;
    const double binHz = nyq / (double) (nb - 1);

    const int W = juce::jmax (1, (int) inner.getWidth());

    if ((int) dispIn.size() < W)
    {
        dispIn .assign ((size_t) W, -90.0f);
        dispOut.assign ((size_t) W, -90.0f);
    }

    juce::Path inPath, outPath, fillOut;
    bool started = false;

    for (int px = 0; px < W; ++px)
    {
        const float u0 = (float) px       / (float) W;
        const float u1 = (float) (px + 1) / (float) W;
        const float f0 = fMin * std::pow (fMax / fMin, u0);
        const float f1 = fMin * std::pow (fMax / fMin, u1);

        const int b0 = juce::jlimit (1, nb - 1, (int) std::floor (f0 / binHz));
        const int b1 = juce::jlimit (b0, nb - 1, (int) std::ceil (f1 / binHz));

        float pkIn = -90.0f, pkOut = -90.0f;

        for (int b = b0; b <= b1; ++b)
        {
            pkIn  = juce::jmax (pkIn,  snapshot.inDb[(size_t) b]);
            pkOut = juce::jmax (pkOut, snapshot.outDb[(size_t) b]);
        }

        dispIn [(size_t) px] = juce::jmax (pkIn,  dispIn [(size_t) px] - 0.8f);
        dispOut[(size_t) px] = juce::jmax (pkOut, dispOut[(size_t) px] - 0.8f);

        const float x  = inner.getX() + (float) px;
        const float yi = yAt (dispIn [(size_t) px]);
        const float yo = yAt (dispOut[(size_t) px]);

        if (! started)
        {
            inPath .startNewSubPath (x, yi);
            outPath.startNewSubPath (x, yo);
            fillOut.startNewSubPath (x, inner.getBottom());
            fillOut.lineTo (x, yo);
            started = true;
        }
        else
        {
            inPath .lineTo (x, yi);
            outPath.lineTo (x, yo);
            fillOut.lineTo (x, yo);
        }
    }

    if (! started)
        return;

    fillOut.lineTo (inner.getRight(), inner.getBottom());
    fillOut.closeSubPath();

    g.setGradientFill (juce::ColourGradient (kInk.withAlpha (0.28f), 0.0f, inner.getY(),
                                             kInk.withAlpha (0.05f), 0.0f, inner.getBottom(), false));
    g.fillPath (fillOut);

    g.setColour (kInk.withAlpha (0.45f));
    g.strokePath (inPath, juce::PathStrokeType (1.0f));

    g.setColour (kInk);
    g.strokePath (outPath, juce::PathStrokeType (1.6f));

    // resolution readout
    {
        const int    fftLen = (nb - 1) * 2;
        const double hzPer  = snapshot.sampleRate / (double) juce::jmax (1, fftLen);

        g.setColour (kInk.withAlpha (0.55f));
        g.setFont (juce::Font (9.5f));
        g.drawText (juce::String (fftLen) + "-pt FFT  ·  " + juce::String (hzPer, 0) + " Hz/bin",
                    r.reduced (12.0f, 8.0f).removeFromTop (14.0f),
                    juce::Justification::topRight, false);
    }
}

//==============================================================================
void BitrotAudioProcessorEditor::paint (juce::Graphics& g)
{
    if (skin.background.isValid())
    {
        g.drawImage (skin.background, getLocalBounds().toFloat());
    }
    else
    {
        g.fillAll (kPink);

        // big soft pink glow behind the display, like the reference mock-up
        g.setGradientFill (juce::ColourGradient (juce::Colour (0x22ffffff), scopeBounds.getCentreX(),
                                                 scopeBounds.getY(),
                                                 juce::Colour (0x00ffffff), scopeBounds.getCentreX(),
                                                 scopeBounds.getBottom(), false));
        g.fillRect (scopeBounds);
    }

    if (! skin.background.isValid())
    {
        auto header = getLocalBounds().reduced (16).removeFromTop (34).toFloat();

        g.setColour (kInk);
        g.setFont (juce::Font ("Segoe Script", 30.0f, juce::Font::plain));
        g.drawText ("Bitrot", header.translated (0.0f, -2.0f),
                    juce::Justification::centredLeft, false);
    }

    g.setColour (kStrip);
    g.fillRoundedRectangle (stripBounds, 3.0f);

    drawScope (g, scopeBounds);
}
