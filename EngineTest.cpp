//==============================================================================
//  Offline self-test for Bitrot.
//
//  Runs the real AudioProcessor (no host, no audio device) on synthetic signals
//  and checks the things that are easy to get wrong:
//
//    * latency / impulse alignment
//    * unity gain at Loss = 0
//    * dry-path alignment at Mix = 0
//    * that Loss actually punches spectral holes
//    * packet modes do something sane
//
//  Build & run:
//      powershell -File build.ps1
//      .\build\Bitrot_artefacts\Release\BitrotTest.exe
//==============================================================================

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Analyzer.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    constexpr double kSampleRate = 44100.0;
    constexpr int    kBlockSize  = 512;

    void setParam (BitrotAudioProcessor& p, const juce::String& id, float value)
    {
        if (auto* param = p.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (value));
    }

    std::vector<float> render (BitrotAudioProcessor& p, const std::vector<float>& in)
    {
        std::vector<float> out (in.size(), 0.0f);
        size_t pos = 0;

        while (pos < in.size())
        {
            const int n = (int) juce::jmin ((size_t) kBlockSize, in.size() - pos);

            juce::AudioBuffer<float> buf (2, n);
            buf.clear();

            for (int i = 0; i < n; ++i)
            {
                buf.setSample (0, i, in[pos + (size_t) i]);
                buf.setSample (1, i, in[pos + (size_t) i]);
            }

            juce::MidiBuffer midi;
            p.processBlock (buf, midi);

            for (int i = 0; i < n; ++i)
                out[pos + (size_t) i] = buf.getSample (0, i);

            pos += (size_t) n;
        }

        return out;
    }

    float rms (const std::vector<float>& v, size_t from, size_t to)
    {
        double acc = 0.0;
        int n = 0;

        for (size_t i = from; i < juce::jmin (to, v.size()); ++i)
        {
            acc += (double) v[i] * (double) v[i];
            ++n;
        }

        return n > 0 ? (float) std::sqrt (acc / (double) n) : 0.0f;
    }

    int countHoles (BitrotAudioProcessor& p)
    {
        BitrotAudioProcessor::ScopeSnapshot snap;
        if (! p.readScope (snap))
            return -1;

        int holes = 0;
        for (int b = 0; b < snap.numBins; ++b)
            if (snap.outDb[(size_t) b] <= -75.0f)
                ++holes;

        return holes;
    }

    struct TestReport
    {
        int   failures = 0;
        void check (bool ok, const juce::String& what, const juce::String& detail)
        {
            std::printf ("  [%s] %-42s %s\n", ok ? " ok " : "FAIL",
                         what.toRawUTF8(), detail.toRawUTF8());
            if (! ok)
                ++failures;
        }
    };

    //==========================================================================
    //  Octave band gains for pink-ish noise: shows exactly which part of the
    //  spectrum the quantiser eats at each Loss setting.
    void printSpectrumReport (BitrotAudioProcessor& p)
    {
        const int n = 1 << 15;

        std::vector<float> in ((size_t) n);
        juce::Random rng (0x1234);
        float lp1 = 0.0f, lp2 = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const float w = rng.nextFloat() * 2.0f - 1.0f;
            lp1 += 0.05f  * (w - lp1);
            lp2 += 0.001f * (w - lp2);
            in[(size_t) i] = 0.35f * w + 0.9f * lp1 + 1.5f * lp2;
        }

        float peak = 1.0e-6f;
        for (float v : in) peak = juce::jmax (peak, std::abs (v));
        for (auto& v : in) v *= 0.4f / peak;

        const double centres[] { 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

        const auto bandGains = [&] (const std::vector<float>& x, size_t start,
                                    std::array<double, 9>& dest)
        {
            const int order = 14;
            juce::dsp::FFT fft (order);
            std::vector<float> buf ((size_t) (2 << order), 0.0f);

            for (size_t i = 0; i < (size_t) (1 << order); ++i)
                buf[i] = x[start + i]
                       * (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi
                                                        * (double) i / (double) (1 << order)));

            fft.performRealOnlyForwardTransform (buf.data(), true);

            for (int b = 0; b < 9; ++b)
            {
                const double lo = centres[b] / std::sqrt (2.0);
                const double hi = centres[b] * std::sqrt (2.0);
                double acc = 0.0;
                int    cnt = 0;

                for (int k = 1; k < (1 << (order - 1)); ++k)
                {
                    const double f = (double) k * kSampleRate / (double) (1 << order);
                    if (f < lo || f > hi) continue;
                    acc += (double) buf[2 * k] * buf[2 * k] + (double) buf[2 * k + 1] * buf[2 * k + 1];
                    ++cnt;
                }

                dest[(size_t) b] = cnt > 0 ? std::sqrt (acc / (double) cnt) : 0.0;
            }
        };

        std::array<double, 9> ref {};
        bandGains (in, 2048, ref);

        std::printf ("\nOctave band gain vs input (pink-ish noise, Perceptual)\n");
        std::printf ("  %-7s", "loss");

        for (double c : centres)
        {
            char label[16];
            if (c >= 1000.0) std::snprintf (label, sizeof (label), "%.0fk", c / 1000.0);
            else             std::snprintf (label, sizeof (label), "%.0f",  c);
            std::printf ("%8s", label);
        }

        std::printf ("%9s\n", "rms");

        for (float loss : { 0.0f, 10.0f, 20.0f, 35.0f, 60.0f, 100.0f })
        {
            setParam (p, "loss", loss);
            setParam (p, "speed", 100.0f);
            setParam (p, "mix", 100.0f);
            setParam (p, "mode", 0.0f);
            p.prepareToPlay (kSampleRate, kBlockSize);

            auto out = render (p, in);

            std::printf ("  [dbg] loss raw = %.2f  mix raw = %.2f  inRms = %.4f  outRms = %.4f\n",
                         p.apvts.getRawParameterValue ("loss")->load(),
                         p.apvts.getRawParameterValue ("mix")->load(),
                         rms (in, 4096, in.size()),
                         rms (out, (size_t) p.getLatencySamples() + 4096, out.size()));

            std::array<double, 9> g {};
            bandGains (out, (size_t) p.getLatencySamples() + 4096, g);

            std::printf ("  %5.0f%% ", loss);

            for (int b = 0; b < 9; ++b)
            {
                const double db = 20.0 * std::log10 ((g[(size_t) b] + 1.0e-9) / (ref[(size_t) b] + 1.0e-9));
                std::printf ("%8.1f", db);
            }

            const float r = rms (out, (size_t) p.getLatencySamples() + 4096, out.size());
            std::printf ("%8.1f\n", 20.0f * std::log10 (r + 1.0e-9f));
        }

        std::printf ("\n");
    }

    //==========================================================================
    int renderUiShot (const juce::File& out, int fftIndex, float loss)
    {
        BitrotAudioProcessor proc;

        setParam (proc, "fft",   (float) fftIndex);
        setParam (proc, "loss",  loss);
        setParam (proc, "speed", 100.0f);
        setParam (proc, "mix",   100.0f);
        setParam (proc, "mode",  0.0f);

        proc.prepareToPlay (48000.0, 512);

        juce::AudioBuffer<float> b (2, 512);
        juce::MidiBuffer         m;
        juce::Random             rng (7);

        for (int blk = 0; blk < 140; ++blk)
        {
            for (int i = 0; i < 512; ++i)
            {
                const float t = (float) (blk * 512 + i) / 48000.0f;
                const float s = 0.30f * std::sin (2.0f * juce::MathConstants<float>::pi * 220.0f  * t)
                              + 0.18f * std::sin (2.0f * juce::MathConstants<float>::pi * 880.0f  * t)
                              + 0.10f * std::sin (2.0f * juce::MathConstants<float>::pi * 3300.0f * t)
                              + 0.02f * (rng.nextFloat() * 2.0f - 1.0f);

                b.setSample (0, i, s);
                b.setSample (1, i, s);
            }

            proc.processBlock (b, m);
        }

        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        ed->setSize (780, 480);

        if (auto* sle = dynamic_cast<BitrotAudioProcessorEditor*> (ed.get()))
        {
            const auto sb = sle->getScopeBounds();
            std::printf ("  editor %d x %d, scope %.0f,%.0f %.0f x %.0f\n",
                         ed->getWidth(), ed->getHeight(),
                         sb.getX(), sb.getY(), sb.getWidth(), sb.getHeight());
        }

        auto img = ed->createComponentSnapshot (ed->getLocalBounds());

        out.deleteFile();
        juce::FileOutputStream os (out);

        if (! os.openedOk())
            return 1;

        juce::PNGImageFormat().writeImageToStream (img, os);
        std::printf ("wrote %s\n", out.getFullPathName().toRawUTF8());
        return 0;
    }
}

//==============================================================================
int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;

    TestReport res;

    BitrotAudioProcessor proc;
    proc.prepareToPlay (kSampleRate, kBlockSize);

    if (argc > 1 && juce::String (argv[1]) == "spectrum")
    {
        printSpectrumReport (proc);
        return 0;
    }

    if (argc > 1 && juce::String (argv[1]) == "analyze")
        return runAnalyzer (argc - 1, argv + 1);

    if (argc > 1 && juce::String (argv[1]) == "ui")
    {
        if (argc < 3)
        {
            std::printf ("usage: BitrotTest ui <out.png> [fftIndex 0..4] [loss]\n");
            return 1;
        }

        return renderUiShot (juce::File (juce::String::fromUTF8 (argv[2])),
                             argc > 3 ? std::atoi (argv[3]) : 1,
                             argc > 4 ? (float) std::atof (argv[4]) : 35.0f);
    }

    const int latency = proc.getLatencySamples();

    std::printf ("\nBitrot self-test\n");
    std::printf ("  fft = %d, hop = %d, bands = %d, latency = %d samples (%.1f ms)\n\n",
                 proc.getCurrentFftSize(),
                 proc.getCurrentFftSize() / 4,
                 BitrotAudioProcessor::numBands,
                 latency, 1000.0 * latency / kSampleRate);

    //-- 1. impulse ---------------------------------------------------------
    {
        setParam (proc, "loss", 0.0f);
        setParam (proc, "mix", 100.0f);
        setParam (proc, "mode", 0.0f);
        setParam (proc, "weighting", 0.0f);
        proc.prepareToPlay (kSampleRate, kBlockSize);

        std::vector<float> in (1 << 17, 0.0f);
        in[0] = 1.0f;

        auto out = render (proc, in);

        int   peakIndex = 0;
        float peakValue = 0.0f;

        for (size_t i = 0; i < out.size(); ++i)
            if (std::abs (out[i]) > peakValue) { peakValue = std::abs (out[i]); peakIndex = (int) i; }

        res.check (peakIndex == latency, "impulse lands at the reported latency",
                   "peak @" + juce::String (peakIndex) + " / latency " + juce::String (latency));

        res.check (std::abs (peakValue - 1.0f) < 0.02f, "unity gain at Loss = 0",
                   "peak " + juce::String (peakValue, 4));

        float leak = 0.0f;
        for (size_t i = 0; i < out.size(); ++i)
            if (std::abs ((int) i - peakIndex) > 8)
                leak = juce::jmax (leak, std::abs (out[i]));

        res.check (leak < 1.0e-3f, "no stray energy around the impulse",
                   "leak " + juce::String (leak, 7));
    }

    //-- 2. sine transparency at Loss = 0 -----------------------------------
    {
        setParam (proc, "loss", 0.0f);
        proc.prepareToPlay (kSampleRate, kBlockSize);

        std::vector<float> in (1 << 17);
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f * (float) i / (float) kSampleRate);

        auto out = render (proc, in);

        const float rIn  = rms (in,  (size_t) latency + 4096, in.size());
        const float rOut = rms (out, (size_t) latency + 4096, out.size());

        res.check (std::abs (20.0f * std::log10 (rOut / rIn)) < 0.3f, "sine level untouched at Loss = 0",
                   juce::String (20.0f * std::log10 (rOut / rIn), 3) + " dB");

        std::vector<float> diff (out.size());
        for (size_t i = 0; i < out.size(); ++i)
            diff[i] = out[i] - ((i >= (size_t) latency) ? in[i - (size_t) latency] : 0.0f);

        const float rDiff = rms (diff, (size_t) latency + 4096, diff.size());
        res.check (20.0f * std::log10 (rDiff / rIn) < -60.0f, "reconstruction error below -60 dB",
                   juce::String (20.0f * std::log10 (rDiff / rIn), 1) + " dB");
    }

    //-- 3. Loss punches holes ----------------------------------------------
    {
        setParam (proc, "loss", 60.0f);
        proc.prepareToPlay (kSampleRate, kBlockSize);

        std::vector<float> in (1 << 17);
        for (size_t i = 0; i < in.size(); ++i)
        {
            in[i] = 0.35f * std::sin (2.0f * juce::MathConstants<float>::pi * 220.0f * (float) i / (float) kSampleRate)
                  + 0.20f * std::sin (2.0f * juce::MathConstants<float>::pi * 3300.0f * (float) i / (float) kSampleRate)
                  + 0.10f * std::sin (2.0f * juce::MathConstants<float>::pi * 11000.0f * (float) i / (float) kSampleRate);
        }

        auto out = render (proc, in);

        const float rIn  = rms (in,  (size_t) latency + 4096, in.size());
        const float rOut = rms (out, (size_t) latency + 4096, out.size());
        const int   holes = countHoles (proc);

        res.check (holes > 0, "spectral holes appear at Loss = 60%",
                   juce::String (holes) + " / " + juce::String (BitrotAudioProcessor::ScopeSnapshot::maxBins) + " bins");

        res.check (std::isfinite (rOut) && rOut > 0.0f, "output is finite and non-silent",
                   juce::String (20.0f * std::log10 (rOut / rIn), 2) + " dB re input");
    }

    //-- 4. Mix = 0 is exactly the delayed dry signal ------------------------
    {
        setParam (proc, "loss", 80.0f);
        setParam (proc, "mix", 0.0f);
        proc.prepareToPlay (kSampleRate, kBlockSize);

        std::vector<float> in (1 << 15);
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = 0.4f * std::sin (0.03f * (float) i) + 0.1f * std::sin (0.31f * (float) i);

        auto out = render (proc, in);

        float maxErr = 0.0f;
        for (size_t i = (size_t) latency + 64; i < out.size(); ++i)
            maxErr = juce::jmax (maxErr, std::abs (out[i] - in[i - (size_t) latency]));

        res.check (maxErr < 1.0e-6f, "Mix = 0 passes a clean delayed dry signal",
                   "max err " + juce::String (maxErr, 8));
    }

    //-- 5. packet modes -----------------------------------------------------
    {
        setParam (proc, "mix", 100.0f);
        setParam (proc, "loss", 90.0f);
        setParam (proc, "mode", 2.0f);            // Packet Loss
        proc.prepareToPlay (kSampleRate, kBlockSize);

        std::vector<float> in (1 << 15);
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = 0.4f * std::sin (0.05f * (float) i);

        auto out   = render (proc, in);
        const float packetLossRms = rms (out, (size_t) latency + 4096, out.size());

        setParam (proc, "mode", 3.0f);            // Packet Repeat
        proc.prepareToPlay (kSampleRate, kBlockSize);
        out = render (proc, in);
        const float repeatRms = rms (out, (size_t) latency + 4096, out.size());

        res.check (packetLossRms > 0.0f && repeatRms > 0.0f, "packet modes produce audio",
                   "loss " + juce::String (packetLossRms, 4) + " / repeat " + juce::String (repeatRms, 4));

        res.check (packetLossRms < rms (in, 4096, in.size()), "packet loss removes energy",
                   juce::String (packetLossRms, 4) + " vs " + juce::String (rms (in, 4096, in.size()), 4));
    }

    //-- 6. inverse mode -----------------------------------------------------
    {
        setParam (proc, "mode", 1.0f);            // Inverse
        setParam (proc, "loss", 55.0f);
        proc.prepareToPlay (kSampleRate, kBlockSize);

        std::vector<float> in (1 << 15);
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = 0.4f * std::sin (0.02f * (float) i) + 0.2f * std::sin (0.9f * (float) i);

        auto out = render (proc, in);
        const float r = rms (out, (size_t) latency + 4096, out.size());

        res.check (r > 1.0e-4f, "inverse mode outputs the residual",
                   juce::String (20.0f * std::log10 (r + 1.0e-12f), 1) + " dBFS");
    }

    //-- 7. extreme settings stay finite ------------------------------------
    {
        setParam (proc, "mode", 0.0f);
        setParam (proc, "loss", 100.0f);

        std::vector<float> in (1 << 15);
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = 0.5f * std::sin (0.02f * (float) i) + 0.3f * std::sin (1.7f * (float) i);

        struct Case { const char* name; float speed; };

        for (auto c : { Case { "Loss 100% / Speed 100%", 100.0f },
                        Case { "Loss 100% / Speed 0%",    0.0f } })
        {
            setParam (proc, "speed", c.speed);
            proc.prepareToPlay (kSampleRate, kBlockSize);

            auto out = render (proc, in);

            bool finite = true;
            for (float v : out)
                finite = finite && std::isfinite (v);

            const float r = rms (out, (size_t) latency + 4096, out.size());

            res.check (finite && r < 2.0f, c.name,
                       juce::String (20.0f * std::log10 (r + 1.0e-12f), 1) + " dBFS");
        }
    }

    //-- 8. every FFT size keeps its own latency/alignment -------------------
    {
        setParam (proc, "mode", 0.0f);
        setParam (proc, "loss", 0.0f);

        const char* names[7] { "128", "256", "512", "1024", "2048", "4096", "8192" };

        for (int idx = 0; idx < 7; ++idx)
        {
            setParam (proc, "fft", (float) idx);
            proc.prepareToPlay (kSampleRate, kBlockSize);

            std::vector<float> in (1 << 15, 0.0f);
            in[0] = 1.0f;

            auto out = render (proc, in);

            int   peakIndex = 0;
            float peakValue = 0.0f;

            for (size_t i = 0; i < out.size(); ++i)
                if (std::abs (out[i]) > peakValue) { peakValue = std::abs (out[i]); peakIndex = (int) i; }

            const int wantLatency = 1 << (7 + idx);

            res.check (peakIndex == wantLatency && std::abs (peakValue - 1.0f) < 0.02f,
                       juce::String ("FFT ") + names[idx] + " alignment + unity gain",
                       "peak @" + juce::String (peakIndex) + " / " + juce::String (wantLatency)
                         + "  val " + juce::String (peakValue, 3));
        }

        setParam (proc, "fft", 4.0f);
        proc.prepareToPlay (kSampleRate, kBlockSize);
    }

    //-- 9. noise fill + stereo modes ----------------------------------------
    {
        setParam (proc, "mode",   0.0f);
        setParam (proc, "fft",    4.0f);
        setParam (proc, "loss",   100.0f);
        setParam (proc, "speed",  100.0f);
        setParam (proc, "mix",    100.0f);
        setParam (proc, "stereo", 0.0f);

        std::vector<float> in (1 << 15);
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = 0.4f * std::sin (0.02f * (float) i);

        setParam (proc, "fill", 0.0f);
        proc.prepareToPlay (kSampleRate, kBlockSize);
        auto outDry = render (proc, in);
        const float rNoFill = rms (outDry, (size_t) latency + 4096, outDry.size());

        setParam (proc, "fill", 100.0f);
        proc.prepareToPlay (kSampleRate, kBlockSize);
        auto outFill = render (proc, in);
        const float rFill = rms (outFill, (size_t) latency + 4096, outFill.size());

        std::vector<float> diff (outFill.size());
        for (size_t i = 0; i < diff.size(); ++i)
            diff[i] = outFill[i] - outDry[i];

        const float rDiff = rms (diff, (size_t) latency + 4096, diff.size());
        const float rIn   = rms (in, 4096, in.size());

        res.check (20.0f * std::log10 (rDiff / rIn) > -30.0f, "noise fill changes the output",
                   juce::String (20.0f * std::log10 (rDiff / rIn), 1) + " dB re input"
                     + " (level " + juce::String (20.0f * std::log10 (rFill / rNoFill), 1) + " dB)");

        // stereo modes: mono must collapse exactly, joint must stay finite
        setParam (proc, "loss",  60.0f);
        setParam (proc, "fill",  70.0f);

        const auto renderStereo = [&] (int mode)
        {
            setParam (proc, "stereo", (float) mode);
            proc.prepareToPlay (kSampleRate, kBlockSize);

            juce::AudioBuffer<float> b (2, 8192);
            for (int i = 0; i < 8192; ++i)
            {
                b.setSample (0, i, 0.4f * std::sin (0.013f * (float) i));
                b.setSample (1, i, 0.3f * std::sin (0.031f * (float) i + 0.7f));
            }

            juce::MidiBuffer midi;
            proc.processBlock (b, midi);
            return b;
        };

        auto mono = renderStereo (2);
        float maxDiff = 0.0f;
        for (int i = 0; i < mono.getNumSamples(); ++i)
            maxDiff = juce::jmax (maxDiff, std::abs (mono.getSample (0, i) - mono.getSample (1, i)));

        res.check (maxDiff < 1.0e-6f, "Mono mode folds L/R exactly",
                   "max diff " + juce::String (maxDiff, 8));

        auto joint = renderStereo (1);
        bool finite = true;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < joint.getNumSamples(); ++i)
                finite = finite && std::isfinite (joint.getSample (ch, i));

        res.check (finite, "Joint (M/S) mode is stable", "all samples finite");

        setParam (proc, "stereo", 0.0f);
        setParam (proc, "fill",   70.0f);
        proc.prepareToPlay (kSampleRate, kBlockSize);
    }

    //-- band holes ----------------------------------------------------------
    {
        const auto holeDepth = [&] (float holesPct)
        {
            BitrotAudioProcessor p2;
            setParam (p2, "loss", 0.0f);
            setParam (p2, "mix", 100.0f);
            setParam (p2, "fill", 0.0f);
            setParam (p2, "holes", holesPct);
            setParam (p2, "mode", 0.0f);
            setParam (p2, "weighting", 0.0f);
            p2.prepareToPlay (kSampleRate, kBlockSize);

            std::vector<float> in ((size_t) (kSampleRate * 2));

            for (size_t i = 0; i < in.size(); ++i)
                in[i] = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi
                                         * 1000.0f * (float) i / (float) kSampleRate);

            auto out = render (p2, in);

            const int win = (int) (kSampleRate * 0.01);      // 10 ms
            float lo = 1.0e9f, hi = 0.0f;

            for (int s = (int) (kSampleRate * 0.4); s + win < (int) out.size(); s += win)
            {
                double sum = 0.0;

                for (int i = 0; i < win; ++i)
                    sum += out[(size_t) (s + i)] * out[(size_t) (s + i)];

                const float r = (float) std::sqrt (sum / win);
                lo = juce::jmin (lo, r);
                hi = juce::jmax (hi, r);
            }

            return hi > 0.0f ? lo / hi : 1.0f;
        };

        const float clean = holeDepth (0.0f);
        const float holed = holeDepth (100.0f);

        res.check (clean > 0.9f && holed < 0.25f,
                   "holes punch real dropouts",
                   "quiet/peak " + juce::String (clean, 3) + " -> " + juce::String (holed, 3));
    }

    //-- intensity stereo ----------------------------------------------------
    {
        const auto sideLevel = [&] (float stereoMode, float lossAmount)
        {
            BitrotAudioProcessor p2;
            setParam (p2, "loss", lossAmount);
            setParam (p2, "speed", 100.0f);
            setParam (p2, "fill", 0.0f);
            setParam (p2, "holes", 0.0f);
            setParam (p2, "mix", 100.0f);
            setParam (p2, "mode", 0.0f);
            setParam (p2, "weighting", 0.0f);
            setParam (p2, "stereo", stereoMode);
            p2.prepareToPlay (kSampleRate, kBlockSize);

            std::vector<float> in ((size_t) (kSampleRate * 1.5));
            juce::Random rnd (4711);

            for (size_t i = 0; i < in.size(); ++i)
                in[i] = (rnd.nextFloat() * 2.0f - 1.0f) * 0.4f;

            double sum = 0.0;
            size_t pos = 0;

            while (pos < in.size())
            {
                const int n = (int) juce::jmin ((size_t) kBlockSize, in.size() - pos);

                juce::AudioBuffer<float> buf (2, n);
                buf.clear();

                for (int i = 0; i < n; ++i)
                {
                    buf.setSample (0, i,  in[pos + (size_t) i]);
                    buf.setSample (1, i, -in[pos + (size_t) i]);   // pure side
                }

                juce::MidiBuffer midi;
                p2.processBlock (buf, midi);

                if (pos > kSampleRate * 0.3)
                    for (int i = 0; i < n; ++i)
                    {
                        const double v = buf.getSample (0, i);
                        sum += v * v;
                    }

                pos += (size_t) n;
            }

            return (float) std::sqrt (sum / juce::jmax (1.0, (double) in.size()));
        };

        const float stereo = sideLevel (0.0f, 50.0f);
        const float joint  = sideLevel (1.0f, 50.0f);

        res.check (joint < stereo * 0.75f,
                   "intensity stereo collapses the side above fIS",
                   "side " + juce::String (stereo, 4) + " -> " + juce::String (joint, 4));
    }

    std::printf ("\n%s\n\n", res.failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return res.failures == 0 ? 0 : 1;
}
