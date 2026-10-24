#pragma once

#include <JuceHeader.h>
#include <array>
#include <memory>
#include <vector>

//==============================================================================
/**
    Bitrot - a prototype of the "FFT spectral" lossy-codec sound.

    STFT domain multiband quantiser:

        STFT -> Bark-ish band energies -> perceptual bit allocation
             -> per-band quantisation step (a slowly moving variable)
             -> round(magnitude / step) * step, phase untouched
             -> ISTFT

    The FFT size is a first-class control (128 / 256 / 512 / 1024 / 2048).
    It sets the time-frequency trade-off *and* the latency, which is the single
    biggest "character" knob: short = telephone / robotic, long = smeared.
*/
class BitrotAudioProcessor final : public juce::AudioProcessor
{
public:
    BitrotAudioProcessor();
    ~BitrotAudioProcessor() override = default;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==========================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                       { return true; }

    const juce::String getName() const override           { return JucePlugin_Name; }

    bool acceptsMidi() const override                     { return false; }
    bool producesMidi() const override                    { return false; }
    bool isMidiEffect() const override                    { return false; }
    double getTailLengthSeconds() const override          { return 0.0; }

    int getNumPrograms() override                         { return 1; }
    int getCurrentProgram() override                      { return 0; }
    void setCurrentProgram (int) override                 {}
    const juce::String getProgramName (int) override      { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    //==========================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    juce::AudioProcessorValueTreeState apvts;

    static constexpr int maxFftSize = 8192;
    static constexpr int maxHopSize = maxFftSize / 4;
    static constexpr int minFftOrder = 7;                 // 128
    static constexpr int numFftSizes = 7;                 // 128 .. 8192
    static constexpr int numBands = 32;

    static constexpr int defaultBits = 14;

    /** Latency currently reported to the host (= selected FFT size). */
    int getCurrentFftSize() const noexcept { return curN; }

    //==========================================================================
    struct ScopeSnapshot
    {
        static constexpr int maxBins = maxFftSize / 2 + 1;

        std::array<float, maxBins> inDb  {};   // per FFT bin, dB
        std::array<float, maxBins> outDb {};
        int    numBins    = 0;
        double sampleRate = 44100.0;
        bool   valid      = false;
    };

    /** Copies the latest analysis frame for the UI (never blocks the audio thread). */
    bool readScope (ScopeSnapshot& dest) const;

private:
    //==========================================================================
    enum Mode { modeStandard = 0, modeInverse, modePacketLoss, modePacketRepeat };

    struct FrameParams
    {
        float loss       = 0.0f;
        float maxBits    = (float) defaultBits;
        float smoothing  = 1.0f;
        float mix        = 1.0f;
        float gain       = 1.0f;
        float fill       = 0.0f;      // noise bed under the gaps
        float holes      = 0.0f;      // random band dropouts
        float intensity  = 0.0f;      // side channel collapse above fIS
        int   mode       = modeStandard;
        bool  perceptual = true;
        bool  joint      = false;
    };

    struct ChannelState
    {
        std::vector<float> circ, ola, hopOut, lastHop, bitAlloc, dryDelay;
    std::array<int, numBands> holeHold {};

        int  circPos  = 0;
        int  outIndex = 0;
        int  dryPos   = 0;
        bool primed   = false;

        juce::Random rng { 0x5eed1234 };
    };

    struct SizeTables
    {
        int n = 0, hop = 0, halfBins = 0;
        std::vector<float> window;
        std::array<int,   numBands + 1> edges {};
        std::array<float, numBands>     centre {}, weight {};
    };

    //==========================================================================
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void buildTables (double sampleRate);
    void resetChannels();
    void resetProcessing();
    void switchFftSize (int index);

    void renderFrame (ChannelState&, int channel, const FrameParams&);
    float processSample (ChannelState&, int channel, float x, const FrameParams&);

    static float hzToBark (float hz);
    static float barkToHz (float bark, float maxHz);

    //==========================================================================
    std::array<SizeTables, numFftSizes> tables;
    std::array<std::unique_ptr<juce::dsp::FFT>, numFftSizes> ffts;

    std::vector<float> fftBuf;        // 2 * maxFftSize
    std::vector<float> residualBuf;   // 2 * maxFftSize

    std::vector<float> pruneScratch;  // per-band magnitudes for line pruning

    int sizeIndex = numFftSizes - 1;
    int curN      = maxFftSize;
    int curHop    = maxHopSize;
    int lastStereoMode = 0;

    ChannelState chans[2];
    double currentSampleRate = 44100.0;

    juce::SmoothedValue<float> mixSm, gainSm;

    mutable juce::SpinLock scopeLock;
    ScopeSnapshot scope;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BitrotAudioProcessor)
};
