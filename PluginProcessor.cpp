#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>

namespace
{
    constexpr float kSilenceDb = -90.0f;

    inline float sqr (float x) noexcept { return x * x; }
}

//==============================================================================
BitrotAudioProcessor::BitrotAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout
BitrotAudioProcessor::createParameterLayout()
{
    using namespace juce;
    using Range = NormalisableRange<float>;

    AudioProcessorValueTreeState::ParameterLayout layout;

    const auto fmt = [] (int decimals, const String& suffix)
    {
        return [decimals, suffix] (float v, int) { return String (v, decimals) + suffix; };
    };

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "loss", 1 }, "Loss",
        Range (0.0f, 100.0f, 0.1f), 35.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (fmt (1, " %"))));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "speed", 1 }, "Loss Speed",
        Range (0.0f, 100.0f, 0.1f), 100.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (fmt (1, " %"))));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "fft", 1 }, "FFT Size",
        StringArray { "128", "256", "512", "1024", "2048", "4096", "8192" }, 4));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "fill", 1 }, "Noise Fill",
        Range (0.0f, 100.0f, 0.1f), 70.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (fmt (0, " %"))));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "holes", 1 }, "Holes",
        Range (0.0f, 100.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (fmt (0, " %"))));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "stereo", 1 }, "Stereo",
        StringArray { "Stereo", "Joint", "Mono" }, 0));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix",
        Range (0.0f, 100.0f, 0.1f), 100.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (fmt (1, " %"))));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 }, "Output",
        Range (-24.0f, 24.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (fmt (1, " dB"))));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode",
        StringArray { "Standard", "Inverse", "Packet Loss", "Packet Repeat" }, 0));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "weighting", 1 }, "Weighting",
        StringArray { "Perceptual", "Flat" }, 0));

    return layout;
}

//==============================================================================
float BitrotAudioProcessor::hzToBark (float hz)
{
    return 13.0f * std::atan (0.00076f * hz)
         + 3.5f  * std::atan (juce::square (hz / 7500.0f));
}

float BitrotAudioProcessor::barkToHz (float bark, float maxHz)
{
    float lo = 0.0f, hi = maxHz;

    for (int i = 0; i < 42; ++i)
    {
        const float mid = 0.5f * (lo + hi);
        if (hzToBark (mid) < bark) lo = mid;
        else                       hi = mid;
    }

    return 0.5f * (lo + hi);
}

//==============================================================================
void BitrotAudioProcessor::buildTables (double sampleRate)
{
    const float nyq      = (float) (sampleRate * 0.5);
    const float barkMax  = hzToBark (nyq);

    for (int s = 0; s < numFftSizes; ++s)
    {
        auto& t = tables[(size_t) s];

        t.n        = 1 << (minFftOrder + s);
        t.hop      = t.n / 4;
        t.halfBins = t.n / 2;

        ffts[(size_t) s] = std::make_unique<juce::dsp::FFT> (minFftOrder + s);

        // sqrt(periodic Hann): analysis x synthesis = Hann. Four of them at 75%
        // overlap (hop = N/4) sum to exactly 2, hence the 0.5 OLA gain.
        t.window.resize ((size_t) t.n);

        for (int n = 0; n < t.n; ++n)
            t.window[(size_t) n] = (float) std::sqrt (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi
                                                                           * (double) n / (double) t.n));

        t.edges[0] = 1;   // skip DC

        for (int b = 1; b <= numBands; ++b)
        {
            const float f   = barkToHz (barkMax * (float) b / (float) numBands, nyq);
            const int   bin = (int) std::lround (f / nyq * (float) t.halfBins);
            t.edges[(size_t) b] = juce::jmax (t.edges[(size_t) b - 1],
                                              juce::jmin (t.halfBins, bin));
        }

        for (int b = 0; b < numBands; ++b)
        {
            const float f0 = (float) t.edges[(size_t) b]     * nyq / (float) t.halfBins;
            const float f1 = (float) t.edges[(size_t) b + 1] * nyq / (float) t.halfBins;
            const float fc = std::sqrt (juce::jmax (20.0f, f0) * juce::jmax (21.0f, f1));

            // Perceptual weighting: bits are cheapest around 1.5 kHz and get
            // expensive at the extremes, so the highs die first - like an MP3.
            const float lf = std::log2 (juce::jmax (fc, 1.0f) / 1600.0f);
            t.centre[(size_t) b] = fc;
            t.weight[(size_t) b] = juce::jlimit (0.22f, 1.30f,
                                                 0.30f + 0.95f * std::exp (-0.5f * lf * lf / 1.3225f));
        }

        // UI scope is fed straight from the FFT bins (see renderFrame), so the
        // editor can draw an honest staircase at any resolution.
    }
}

//==============================================================================
void BitrotAudioProcessor::resetChannels()
{
    for (auto& s : chans)
    {
        std::fill (s.circ.begin(),    s.circ.end(),    0.0f);
        std::fill (s.ola.begin(),     s.ola.end(),     0.0f);
        std::fill (s.hopOut.begin(),  s.hopOut.end(),  0.0f);
        std::fill (s.lastHop.begin(), s.lastHop.end(), 0.0f);
        std::fill (s.bitAlloc.begin(),s.bitAlloc.end(),0.0f);
        std::fill (s.dryDelay.begin(),s.dryDelay.end(),0.0f);

        s.circPos  = 0;
        s.outIndex = 0;
        s.dryPos   = 0;
        s.primed   = false;
    }
}

void BitrotAudioProcessor::resetProcessing()
{
    for (auto& s : chans)
    {
        std::fill (s.circ.begin(),    s.circ.end(),    0.0f);
        std::fill (s.ola.begin(),     s.ola.end(),     0.0f);
        std::fill (s.hopOut.begin(),  s.hopOut.end(),  0.0f);
        std::fill (s.lastHop.begin(), s.lastHop.end(), 0.0f);
        std::fill (s.bitAlloc.begin(),s.bitAlloc.end(),0.0f);

        s.circPos  = 0;
        s.outIndex = 0;
        s.primed   = false;
    }
}

void BitrotAudioProcessor::switchFftSize (int index)
{
    index = juce::jlimit (0, numFftSizes - 1, index);

    if (index == sizeIndex && curN == tables[(size_t) index].n)
        return;

    sizeIndex = index;
    curN      = tables[(size_t) index].n;
    curHop    = tables[(size_t) index].hop;

    resetChannels();
    setLatencySamples (curN);
}

//==============================================================================
void BitrotAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;

    buildTables (currentSampleRate);

    fftBuf.assign ((size_t) maxFftSize * 2, 0.0f);
    residualBuf.assign ((size_t) maxFftSize * 2, 0.0f);

    pruneScratch.assign ((size_t) maxFftSize / 2 + 2, 0.0f);

    for (auto& s : chans)
    {
        s.circ.assign     ((size_t) maxFftSize,      0.0f);
        s.ola.assign      ((size_t) maxFftSize,      0.0f);
        s.hopOut.assign   ((size_t) maxHopSize,      0.0f);
        s.lastHop.assign  ((size_t) maxHopSize,      0.0f);
        s.bitAlloc.assign ((size_t) numBands,        0.0f);
        s.dryDelay.assign ((size_t) maxFftSize,      0.0f);
    }

    sizeIndex = juce::jlimit (0, numFftSizes - 1,
                              (int) apvts.getRawParameterValue ("fft")->load());
    curN   = tables[(size_t) sizeIndex].n;
    curHop = tables[(size_t) sizeIndex].hop;

    lastStereoMode = (int) apvts.getRawParameterValue ("stereo")->load();

    resetChannels();

    mixSm.reset (currentSampleRate, 0.02);
    gainSm.reset (currentSampleRate, 0.02);
    mixSm.setCurrentAndTargetValue (apvts.getRawParameterValue ("mix")->load() * 0.01f);
    gainSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("output")->load()));

    setLatencySamples (curN);
}

bool BitrotAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();

    if (in != out)
        return false;

    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

//==============================================================================
void BitrotAudioProcessor::renderFrame (ChannelState& s, int channel, const FrameParams& p)
{
    const auto& t  = tables[(size_t) sizeIndex];
    const int   N  = t.n;
    const int   halfBins = t.halfBins;
    const int   mask = N - 1;

    float* buf = fftBuf.data();

    for (int n = 0; n < N; ++n)
        buf[n] = s.circ[(size_t) ((s.circPos + n) & mask)] * t.window[(size_t) n];

    std::fill (buf + N, buf + 2 * N, 0.0f);

    ffts[(size_t) sizeIndex]->performRealOnlyForwardTransform (buf, true);

    const bool wantScope = (channel == 0);
    // 0 dB on the scope = full-scale sine: a Hann-windowed sine peaks at N/4.
    const float scopeNorm = 4.0f / (float) N;

    if (p.mode == modeInverse)
        std::copy (buf, buf + 2 * N, residualBuf.begin());

    ScopeSnapshot local;

    if (wantScope)
    {
        for (int k = 0; k <= halfBins; ++k)
            local.inDb[(size_t) k] = juce::Decibels::gainToDecibels (scopeNorm * 
                                         std::sqrt (sqr (buf[2 * k]) + sqr (buf[2 * k + 1])), kSilenceDb);
    }

    //--- the actual "lossy codec" ------------------------------------------
    for (int b = 0; b < numBands; ++b)
    {
        const int i0 = t.edges[(size_t) b];
        const int i1 = t.edges[(size_t) b + 1];

        if (i1 <= i0)
            continue;

        float energy = 0.0f, peakMag = 0.0f;
        for (int k = i0; k < i1; ++k)
        {
            const float re = buf[2 * k];
            const float im = buf[2 * k + 1];
            const float m  = std::sqrt (re * re + im * im);

            energy  += m * m;
            peakMag  = juce::jmax (peakMag, m);
        }

        const float rms = std::sqrt (energy / (float) (i1 - i0)) + 1.0e-12f;

        // A codec normalises the band by its PEAK (the scalefactor), not by its
        // RMS. With an RMS reference a loud partial is never really quantised,
        // which is why an RMS-based version sounds like faint added noise.
        const float amp = juce::jmax (peakMag, 1.0e-12f);

        // fewer bits where hearing is less sensitive, but never a hard switch:
        // this only *biases* the loss, it does not band-limit the signal.
        const float w   = p.perceptual ? t.weight[(size_t) b] : 1.0f;
        const float eff = juce::jlimit (0.0f, 1.0f, p.loss * (1.45f - 0.45f * w));

        const float bandDb   = juce::Decibels::gainToDecibels (rms, -120.0f);
        const float activity = juce::jlimit (0.0f, 1.0f, (bandDb + 70.0f) / 60.0f);

        // exponential: every 10% of Loss costs roughly 4 dB of SNR, so 25% is a
        // genuinely bad MP3, 50% is "never existed", 100% is unrecognisable.
        float targetBits = p.maxBits * std::exp (-4.0f * eff)
                         + 0.8f * activity * eff;

        float& bits = s.bitAlloc[(size_t) b];

        if (p.joint && channel == 1)
        {
            // Joint stereo: the side channel shares the mid channel's bit
            // allocation, so both channels lose the same bands at the same time.
            bits = chans[0].bitAlloc[(size_t) b];
        }
        else if (! s.primed)
        {
            bits = targetBits;
        }
        else
        {
            bits += (targetBits - bits) * p.smoothing;
        }

        bits = juce::jlimit (-3.0f, 18.0f, bits);

        // Real low-bitrate codecs simply stop coding bands above a bandwidth
        // limit. That hard edge is a big part of the "MP3" identity.
        const float bandwidth = 0.5f * (float) currentSampleRate
                                  * juce::jlimit (0.06f, 1.0f, 1.0f - 1.15f * p.loss);

        const bool aboveBandwidth = t.centre[(size_t) b] > bandwidth;
        const bool gated          = bits < 0.9f;

        const int   bandBins = i1 - i0;
        const float q = juce::jmax (1.0e-12f, amp * std::pow (2.0f, -bits));

        //--- random band holes --------------------------------------------------
        // A band goes completely silent for a few frames: the swiss-cheese gaps
        // a failing codec leaves behind. The hold makes it read as a hole rather
        // than a single-frame glitch.
        auto& hold = s.holeHold[(size_t) b];

        if (hold > 0)
            --hold;
        else if (p.holes > 0.001f && s.rng.nextFloat() < p.holes * p.holes * 0.30f)
            hold = 2 + (int) (s.rng.nextFloat() * 12.0f);

        const bool holed = hold > 0;

        if (aboveBandwidth || gated || holed)
        {
            for (int k = i0; k < i1; ++k)
            {
                buf[2 * k]     = 0.0f;
                buf[2 * k + 1] = 0.0f;
            }
        }
        else
        {
            //--- line pruning ---------------------------------------------------
            // A real codec only transmits the strongest lines in a band; every
            // other bin is never coded at all. That is what makes low bitrates
            // sound hollow instead of "a bit crushed".
            for (int k = 0; k < bandBins; ++k)
            {
                const int idx = i0 + k;
                pruneScratch[(size_t) k] = std::sqrt (sqr (buf[2 * idx]) + sqr (buf[2 * idx + 1]));
            }

            const float frac = juce::jlimit (0.0f, 1.0f, bits / 6.0f);
            const int   keep = juce::jlimit (1, bandBins,
                                             (int) std::round ((float) bandBins
                                                               * std::pow (frac, 1.5f)));

            float cutoff = 0.0f;

            if (keep < bandBins)
            {
                auto* base = pruneScratch.data();
                std::nth_element (base, base + (bandBins - keep), base + bandBins);
                cutoff = base[bandBins - keep];
            }

            for (int k = 0; k < bandBins; ++k)
            {
                const int idx = i0 + k;
                float& re = buf[2 * idx];
                float& im = buf[2 * idx + 1];

                const float m = pruneScratch[(size_t) k];

                if (m < cutoff || m <= q)
                {
                    re = 0.0f;
                    im = 0.0f;
                }
                else
                {
                    const float g = (std::round (m / q) * q) / m;
                    re *= g;
                    im *= g;
                }
            }
        }

        //--- noise bed ----------------------------------------------------------
        // Fill used to *replace* dead bands, which erased the holes. It is now a
        // low bed added underneath them, so the gaps stay visible.
        if (p.fill > 0.001f)
        {
            const float dead = juce::jlimit (0.0f, 1.0f, (2.0f - bits) / 2.0f) * p.fill;

            if (dead > 0.0001f)
            {
                const float a = rms * 0.14f * dead * 1.2247449f;   // ~ -17 dB bed

                for (int k = i0; k < i1; ++k)
                {
                    buf[2 * k]     += (s.rng.nextFloat() * 2.0f - 1.0f) * a;
                    buf[2 * k + 1] += (s.rng.nextFloat() * 2.0f - 1.0f) * a;
                }
            }
        }
    }

    //--- intensity stereo -----------------------------------------------------
    // Joint stereo streams stop coding the side channel above fIS, so the image
    // collapses to mono in the top end while the lows stay wide.
    if (p.joint && channel == 1 && p.intensity > 0.001f)
    {
        const float bw  = 0.5f * (float) currentSampleRate
                            * juce::jlimit (0.06f, 1.0f, 1.0f - 1.15f * p.loss);
        const float fIS = juce::jmax (200.0f, bw * (0.85f - 0.55f * p.intensity));
        const float binHz = (float) currentSampleRate / (float) N;

        for (int k = 1; k <= halfBins; ++k)
        {
            const float f = (float) k * binHz;
            const float t = juce::jlimit (0.0f, 1.0f, std::log2 (f / fIS) * 3.0f);
            const float g = 1.0f - (t * t * (3.0f - 2.0f * t));   // 1/3 octave knee

            buf[2 * k]     *= g;
            buf[2 * k + 1] *= g;
        }
    }

    s.primed = true;

    if (wantScope)
    {
        for (int k = 0; k <= halfBins; ++k)
            local.outDb[(size_t) k] = juce::Decibels::gainToDecibels (scopeNorm * 
                                          std::sqrt (sqr (buf[2 * k]) + sqr (buf[2 * k + 1])), kSilenceDb);

        local.numBins    = halfBins + 1;
        local.sampleRate = currentSampleRate;
        local.valid      = true;

        const juce::SpinLock::ScopedTryLockType sl (scopeLock);
        if (sl.isLocked())
            scope = local;
    }

    //--- Inverse = whatever the quantiser removed --------------------------
    if (p.mode == modeInverse)
        for (int i = 0; i < 2 * N; ++i)
            buf[i] = residualBuf[(size_t) i] - buf[i];

    ffts[(size_t) sizeIndex]->performRealOnlyInverseTransform (buf);

    for (int n = 0; n < N; ++n)
        s.ola[(size_t) n] += buf[n] * t.window[(size_t) n] * 0.5f;

    std::copy (s.ola.begin(), s.ola.begin() + curHop, s.hopOut.begin());
    std::move (s.ola.begin() + curHop, s.ola.begin() + N, s.ola.begin());
    std::fill (s.ola.begin() + (N - curHop), s.ola.begin() + N, 0.0f);

    //--- packet modes ------------------------------------------------------
    if (p.mode == modePacketLoss || p.mode == modePacketRepeat)
    {
        if (s.rng.nextFloat() < p.loss * 0.55f)
        {
            if (p.mode == modePacketRepeat)
            {
                std::copy (s.lastHop.begin(), s.lastHop.begin() + curHop, s.hopOut.begin());
            }
            else
            {
                const float prev = s.lastHop[(size_t) curHop - 1];
                const int   fade = juce::jmin (32, curHop);

                for (int n = 0; n < curHop; ++n)
                    s.hopOut[(size_t) n] = n < fade
                                             ? prev * (1.0f - (float) n / (float) fade)
                                             : 0.0f;
            }
        }

        std::copy (s.hopOut.begin(), s.hopOut.begin() + curHop, s.lastHop.begin());
    }
}

//==============================================================================
float BitrotAudioProcessor::processSample (ChannelState& s, int channel, float x, const FrameParams& p)
{
    if (s.dryDelay.empty())
        return x;

    // dry path, delayed by exactly the current latency (ring of maxFftSize)
    const int rd = (s.dryPos + maxFftSize - curN) & (maxFftSize - 1);
    const float dry = s.dryDelay[(size_t) rd];
    s.dryDelay[(size_t) s.dryPos] = x;
    s.dryPos = (s.dryPos + 1) & (maxFftSize - 1);

    s.circ[(size_t) s.circPos] = x;
    s.circPos = (s.circPos + 1) & (curN - 1);

    float wet = s.hopOut[(size_t) s.outIndex];

    if (++s.outIndex >= curHop)
    {
        s.outIndex = 0;
        renderFrame (s, channel, p);
    }

    wet *= p.gain;

    return dry + (wet - dry) * p.mix;
}

//==============================================================================
void BitrotAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int totalIn    = getTotalNumInputChannels();
    const int totalOut   = getTotalNumOutputChannels();

    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, numSamples);

    // FFT size is a real control: switching resets the STFT state and changes
    // the reported latency, so the host re-aligns the track.
    switchFftSize ((int) apvts.getRawParameterValue ("fft")->load());

    FrameParams p;
    p.loss       = apvts.getRawParameterValue ("loss")->load() * 0.01f;
    p.mode       = (int) apvts.getRawParameterValue ("mode")->load();
    p.perceptual = apvts.getRawParameterValue ("weighting")->load() < 0.5f;
    p.mix        = apvts.getRawParameterValue ("mix")->load() * 0.01f;
    p.gain       = juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("output")->load());

    if (p.mode == modeInverse)
        p.gain *= 4.0f;                       // the ghost is quiet - give it a lift

    const float speed = apvts.getRawParameterValue ("speed")->load() * 0.01f;
    p.smoothing = juce::jmax (0.0005f, speed * speed * speed);

    mixSm.setTargetValue (p.mix);
    gainSm.setTargetValue (p.gain);

    const int stereoMode = (int) apvts.getRawParameterValue ("stereo")->load();

    if (stereoMode != lastStereoMode)
    {
        lastStereoMode = stereoMode;
        resetProcessing();
    }

    p.fill  = apvts.getRawParameterValue ("fill")->load() * 0.01f;
    p.holes = apvts.getRawParameterValue ("holes")->load() * 0.01f;
    p.intensity = juce::jlimit (0.0f, 1.0f, (p.loss - 0.10f) / 0.40f);
    p.joint = (stereoMode == 1);

    const int numCh = juce::jmin (2, buffer.getNumChannels());
    float* dl = buffer.getWritePointer (0);
    float* dr = (numCh > 1) ? buffer.getWritePointer (1) : nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        FrameParams pp = p;
        pp.mix  = mixSm.getNextValue();
        pp.gain = gainSm.getNextValue();

        if (dr == nullptr)
        {
            dl[i] = processSample (chans[0], 0, dl[i], pp);
        }
        else if (stereoMode == 2)                       // Mono
        {
            const float mono = 0.5f * (dl[i] + dr[i]);
            const float y    = processSample (chans[0], 0, mono, pp);
            dl[i] = y;
            dr[i] = y;
        }
        else if (stereoMode == 1)                       // Joint (M/S)
        {
            const float l  = dl[i], r = dr[i];
            const float mo = processSample (chans[0], 0, 0.5f * (l + r), pp);
            const float so = processSample (chans[1], 1, 0.5f * (l - r), pp);

            dl[i] = mo + so;
            dr[i] = mo - so;
        }
        else                                            // Stereo
        {
            dl[i] = processSample (chans[0], 0, dl[i], pp);
            dr[i] = processSample (chans[1], 1, dr[i], pp);
        }
    }
}

//==============================================================================
bool BitrotAudioProcessor::readScope (ScopeSnapshot& dest) const
{
    const juce::SpinLock::ScopedTryLockType sl (scopeLock);

    if (! sl.isLocked() || ! scope.valid)
        return false;

    dest = scope;
    return true;
}

//==============================================================================
juce::AudioProcessorEditor* BitrotAudioProcessor::createEditor()
{
    return new BitrotAudioProcessorEditor (*this);
}

//==============================================================================
void BitrotAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void BitrotAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BitrotAudioProcessor();
}
