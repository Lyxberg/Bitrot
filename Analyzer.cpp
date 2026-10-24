#include "Analyzer.h"
#include "PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>

namespace
{
    constexpr int kAnaOrder = 12;                 // 4096
    constexpr int kAnaSize  = 1 << kAnaOrder;
    constexpr int kAnaHop   = 512;

    //==========================================================================
    struct Spectrum
    {
        double sampleRate = 44100.0;
        int numBins = 0, numFrames = 0;
        std::vector<float> mag;                   // dB, [frame][bin]

        float& at (int f, int b)             { return mag[(size_t) f * (size_t) numBins + (size_t) b]; }
        const float& at (int f, int b) const { return mag[(size_t) f * (size_t) numBins + (size_t) b]; }
    };

    //==========================================================================
    juce::Colour heatColour (float t)
    {
        t = juce::jlimit (0.0f, 1.0f, t);

        struct Stop { float p, r, g, b; };
        static const Stop stops[]
        {
            { 0.00f,   4,   2,  18 },
            { 0.18f,  40,  11,  84 },
            { 0.38f, 101,  21, 110 },
            { 0.58f, 159,  42,  99 },
            { 0.78f, 212,  72,  66 },
            { 0.90f, 245, 125,  21 },
            { 1.00f, 252, 255, 164 }
        };

        for (int i = 0; i < 6; ++i)
        {
            if (t <= stops[i + 1].p)
            {
                const float k = (t - stops[i].p) / (stops[i + 1].p - stops[i].p);
                return juce::Colour ((juce::uint8) juce::jmap (k, stops[i].r, stops[i + 1].r),
                                     (juce::uint8) juce::jmap (k, stops[i].g, stops[i + 1].g),
                                     (juce::uint8) juce::jmap (k, stops[i].b, stops[i + 1].b));
            }
        }

        return juce::Colour ((juce::uint8) 252, (juce::uint8) 255, (juce::uint8) 164);
    }

    //==========================================================================
    Spectrum analyse (const std::vector<float>& x, double sr)
    {
        Spectrum s;
        s.sampleRate = sr;
        s.numBins = kAnaSize / 2 + 1;
        s.numFrames = x.size() > (size_t) kAnaSize
                        ? (int) ((x.size() - (size_t) kAnaSize) / (size_t) kAnaHop) + 1
                        : 1;
        s.mag.assign ((size_t) s.numFrames * (size_t) s.numBins, -140.0f);

        juce::dsp::FFT fft (kAnaOrder);
        std::vector<float> buf ((size_t) kAnaSize * 2, 0.0f);
        std::vector<float> win ((size_t) kAnaSize);

        for (int n = 0; n < kAnaSize; ++n)
            win[(size_t) n] = (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi
                                                            * (double) n / (double) kAnaSize));

        for (int f = 0; f < s.numFrames; ++f)
        {
            const size_t start = (size_t) f * (size_t) kAnaHop;

            for (int n = 0; n < kAnaSize; ++n)
                buf[(size_t) n] = (start + (size_t) n < x.size() ? x[start + (size_t) n] : 0.0f)
                                  * win[(size_t) n];

            std::fill (buf.begin() + kAnaSize, buf.end(), 0.0f);
            fft.performRealOnlyForwardTransform (buf.data(), true);

            for (int b = 0; b < s.numBins; ++b)
            {
                const float re = buf[(size_t) (2 * b)];
                const float im = buf[(size_t) (2 * b + 1)];

                // normalise so a full-scale sine reads ~0 dB
                s.at (f, b) = juce::Decibels::gainToDecibels (std::sqrt (re * re + im * im)
                                                              / (float) (kAnaSize / 4), -140.0f);
            }
        }

        return s;
    }

    //==========================================================================
    void renderPanel (juce::Image& img, const Spectrum& s, juce::Rectangle<int> area, bool logFreq, float dbOffset)
    {
        const double nyq = s.sampleRate * 0.5;
        const double fMin = logFreq ? 30.0 : 0.0;
        const double fMax = logFreq ? juce::jmin (20000.0, nyq) : juce::jmin (22000.0, nyq);

        for (int px = 0; px < area.getWidth(); ++px)
        {
            const int f = juce::jlimit (0, s.numFrames - 1,
                                        (int) ((juce::int64) px * (juce::int64) s.numFrames / area.getWidth()));
            for (int py = 0; py < area.getHeight(); ++py)
            {
                const double t = 1.0 - (py + 0.5) / (double) area.getHeight();
                const double hz = logFreq ? fMin * std::pow (fMax / fMin, t)
                                          : fMin + (fMax - fMin) * t;
                const int b = juce::jlimit (0, s.numBins - 1,
                                            (int) std::lround (hz / nyq * (double) (s.numBins - 1)));

                img.setPixelAt (area.getX() + px, area.getY() + py,
                                heatColour (juce::jmap (s.at (f, b) + dbOffset, -105.0f, -5.0f, 0.0f, 1.0f)));
            }
        }
    }

    //==========================================================================
    void writeSpectrogram (const Spectrum& s, const juce::File& file, const juce::String& title, float dbOffset)
    {
        const int W = 1480, Hlog = 400, Hlin = 340;
        const int ml = 66, mt = 46, mb = 30, gap = 30;
        const int imgW = W + ml + 18;
        const int imgH = mt + Hlog + gap + Hlin + mb;

        juce::Image img (juce::Image::RGB, imgW, imgH, true);

        {
            juce::Graphics g (img);
            g.fillAll (juce::Colours::black);
        }

        auto logArea = juce::Rectangle<int> (ml, mt, W, Hlog);
        auto linArea = juce::Rectangle<int> (ml, mt + Hlog + gap, W, Hlin);

        renderPanel (img, s, logArea, true, dbOffset);
        renderPanel (img, s, linArea, false, dbOffset);

        juce::Graphics g (img);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (15.0f, juce::Font::bold));
        g.drawText (title, ml, 12, imgW - ml - 10, 22, juce::Justification::centredLeft, false);

        g.setFont (juce::Font (10.0f));

        g.setColour (juce::Colour (0xffbbbbbb));
        g.drawText ("log f", 8, mt + Hlog / 2 - 8, 52, 16, juce::Justification::centredRight, false);
        g.drawText ("lin f", 8, mt + Hlog + gap + Hlin / 2 - 8, 52, 16, juce::Justification::centredRight, false);

        const double nyq = s.sampleRate * 0.5;
        const double fMin = 30.0;
        const double fMax = juce::jmin (20000.0, nyq);

        const int fTicks[] { 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };

        for (int ft : fTicks)
        {
            if ((double) ft > fMax)
                continue;

            const double t = std::log ((double) ft / fMin) / std::log (fMax / fMin);
            const int y = logArea.getBottom() - (int) (t * logArea.getHeight());

            g.setColour (juce::Colour (0x60ffffff));
            g.drawHorizontalLine (y, (float) logArea.getX(), (float) logArea.getRight());
            g.drawText (ft >= 1000 ? juce::String (ft / 1000) + "k" : juce::String (ft),
                        2, y - 6, ml - 8, 12, juce::Justification::centredRight, false);
        }

        for (int fk = 0; fk <= 20000; fk += 5000)
        {
            if ((double) fk > fMax)
                continue;

            const int y = linArea.getBottom() - (int) ((double) fk / fMax * linArea.getHeight());
            g.setColour (juce::Colour (0x60ffffff));
            g.drawHorizontalLine (y, (float) linArea.getX(), (float) linArea.getRight());
            g.drawText (fk >= 1000 ? juce::String (fk / 1000) + "k" : juce::String (fk),
                        2, y - 6, ml - 8, 12, juce::Justification::centredRight, false);
        }

        const double duration = (double) s.numFrames * (double) kAnaHop / s.sampleRate;
        const int step = duration > 20.0 ? 5 : (duration > 8.0 ? 2 : 1);

        for (int t = 0; (double) t <= duration; t += step)
        {
            const int x = logArea.getX() + (int) ((double) t / duration * logArea.getWidth());
            g.setColour (juce::Colour (0x50ffffff));
            g.drawVerticalLine (x, (float) logArea.getY(), (float) logArea.getBottom());
            g.drawVerticalLine (x, (float) linArea.getY(), (float) linArea.getBottom());
            g.drawText (juce::String (t) + "s", x - 12, linArea.getBottom() + 2, 40, 12,
                        juce::Justification::centredLeft, false);
        }

        file.deleteFile();
        juce::FileOutputStream os (file);

        if (os.openedOk())
            juce::PNGImageFormat().writeImageToStream (img, os);
    }

    //==========================================================================
    void printStats (const Spectrum& s, const juce::String& label)
    {
        const double nyq = s.sampleRate * 0.5;

        std::vector<float> peak ((size_t) s.numBins, -140.0f);
        for (int b = 0; b < s.numBins; ++b)
            for (int f = 0; f < s.numFrames; ++f)
                peak[(size_t) b] = juce::jmax (peak[(size_t) b], s.at (f, b));

        std::vector<float> smooth ((size_t) s.numBins, -140.0f);
        for (int b = 0; b < s.numBins; ++b)
        {
            float acc = 0.0f;
            int   n = 0;
            for (int k = juce::jmax (0, b - 4); k <= juce::jmin (s.numBins - 1, b + 4); ++k)
            {
                acc += peak[(size_t) k];
                ++n;
            }
            smooth[(size_t) b] = acc / (float) n;
        }

        const auto binToHz = [&] (int b) { return (double) b * nyq / (double) (s.numBins - 1); };

        float ref = -140.0f;
        for (int b = 0; b < s.numBins; ++b)
        {
            const double f = binToHz (b);
            if (f >= 500.0 && f <= 2000.0)
                ref = juce::jmax (ref, smooth[(size_t) b]);
        }

        int cutBin = 0;
        for (int b = s.numBins - 1; b >= 0; --b)
            if (binToHz (b) > 3000.0 && smooth[(size_t) b] > ref - 55.0f) { cutBin = b; break; }

        // octave band averages (power)
        const double centres[] { 31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

        std::printf ("\n  %s\n", label.toRawUTF8());
        std::printf ("    frames %d  (%.2f s, %.1f fps)\n", s.numFrames,
                     (double) s.numFrames * kAnaHop / s.sampleRate,
                     s.sampleRate / kAnaHop);
        std::printf ("    HF cutoff (max -55 dB, smoothed): %.0f Hz\n", binToHz (cutBin));

        std::printf ("    octave:");
        for (double c : centres)
        {
            const double lo = c / std::sqrt (2.0), hi = c * std::sqrt (2.0);
            double p = 0.0;
            int    n = 0;

            for (int b = 0; b < s.numBins; ++b)
            {
                const double f = binToHz (b);
                if (f < lo || f > hi) continue;

                for (int k = 0; k < s.numFrames; ++k)
                {
                    p += std::pow (10.0, (double) s.at (k, b) / 10.0);
                    ++n;
                }
            }

            std::printf (" %6.1f", n > 0 ? 10.0 * std::log10 (p / (double) n + 1.0e-12) : -140.0);
        }
        std::printf ("\n    %-8s", "");
        for (double c : centres)
            std::printf (" %6s", c >= 1000.0 ? (juce::String (c / 1000.0, 0) + "k").toRawUTF8()
                                             : juce::String (c, 0).toRawUTF8());
        std::printf ("\n");

        // per-frame high band envelope, look for frame-rate modulation
        std::vector<float> env ((size_t) s.numFrames, -140.0f);
        for (int f = 0; f < s.numFrames; ++f)
        {
            double p = 0.0;
            int    n = 0;
            for (int b = 0; b < s.numBins; ++b)
                if (binToHz (b) > 6000.0) { p += std::pow (10.0, (double) s.at (f, b) / 10.0); ++n; }

            env[(size_t) f] = (float) (10.0 * std::log10 (p / juce::jmax (1, n) + 1.0e-12));
        }

        double mean = 0.0;
        for (float v : env) mean += v;
        mean /= (double) env.size();

        float mn = 1.0e9f, mx = -1.0e9f;
        for (float v : env) { mn = juce::jmin (mn, v); mx = juce::jmax (mx, v); }

        std::printf ("    HF (>6k) envelope: range %.1f dB, std %.1f dB\n", mx - mn,
                     (float) std::sqrt ([&]
                     {
                         double a = 0.0;
                         for (float v : env) a += (double) (v - mean) * (double) (v - mean);
                         return a / (double) env.size();
                     }()));

        // quiet-frame noise floor (the quietest 25% of frames, averaged)
        {
            std::vector<float> pow ((size_t) s.numFrames, 0.0f);
            std::vector<int>   order ((size_t) s.numFrames);

            for (int f = 0; f < s.numFrames; ++f)
            {
                double p = 0.0;
                for (int b = 0; b < s.numBins; ++b)
                    p += std::pow (10.0, (double) s.at (f, b) / 10.0);
                pow[(size_t) f] = (float) p;
                order[(size_t) f] = f;
            }

            std::sort (order.begin(), order.end(), [&] (int a, int b) { return pow[(size_t) a] < pow[(size_t) b]; });

            const int keep = juce::jmax (1, s.numFrames / 4);
            std::vector<double> floorSpec ((size_t) s.numBins, 0.0);

            for (int i = 0; i < keep; ++i)
                for (int b = 0; b < s.numBins; ++b)
                    floorSpec[(size_t) b] += std::pow (10.0, (double) s.at (order[(size_t) i], b) / 10.0);

            std::printf ("    quiet-frame floor:");
            for (double c : centres)
            {
                const double lo = c / std::sqrt (2.0), hi = c * std::sqrt (2.0);
                double p = 0.0;
                int    n = 0;

                for (int b = 0; b < s.numBins; ++b)
                {
                    const double f = binToHz (b);
                    if (f < lo || f > hi) continue;
                    p += floorSpec[(size_t) b] / (double) keep;
                    ++n;
                }

                std::printf (" %6.1f", n > 0 ? 10.0 * std::log10 (p / (double) n + 1.0e-12) : -140.0);
            }
            std::printf ("\n");
        }
    }

    //==========================================================================
    void setP (BitrotAudioProcessor& p, const char* id, float v)
    {
        if (auto* param = p.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (v));
    }

    std::vector<float> monoMix (const juce::AudioBuffer<float>& b)
    {
        std::vector<float> out ((size_t) b.getNumSamples(), 0.0f);

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            const float* d = b.getReadPointer (ch);
            for (int i = 0; i < b.getNumSamples(); ++i)
                out[(size_t) i] += d[i];
        }

        const float k = 1.0f / (float) juce::jmax (1, b.getNumChannels());
        for (auto& v : out) v *= k;

        return out;
    }
}

//==============================================================================
int runAnalyzer (int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf ("usage: BitrotTest analyze <in.wav> <outPrefix> [loss speed mode weighting]\n");
        return 1;
    }

    const juce::File inFile (juce::String::fromUTF8 (argv[1]));
    const juce::String prefix = juce::String::fromUTF8 (argv[2]);
    const bool process = (argc > 3);

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (inFile));

    if (reader == nullptr)
    {
        std::printf ("cannot read %s\n", argv[1]);
        return 1;
    }

    const int64 maxLen = (int64) (reader->sampleRate * 120.0);
    const int   len    = (int) juce::jmin (reader->lengthInSamples, maxLen);
    const int   nch    = (int) juce::jmin ((unsigned) 2, reader->numChannels);
    const double sr    = reader->sampleRate;

    juce::AudioBuffer<float> buffer (nch, len);
    reader->read (&buffer, 0, len, 0, true, true);

    std::printf ("\nfile: %s\n  %.1f kHz, %d ch, %.2f s\n",
                 inFile.getFileName().toRawUTF8(), sr / 1000.0, nch, (double) len / sr);

    if (process)
    {
        const std::vector<float> dryMono = monoMix (buffer);

        BitrotAudioProcessor proc;
        setP (proc, "loss",      argc > 3 ? (float) std::atof (argv[3]) : 35.0f);
        setP (proc, "speed",     argc > 4 ? (float) std::atof (argv[4]) : 100.0f);
        setP (proc, "mode",      argc > 5 ? (float) std::atof (argv[5]) : 0.0f);
        setP (proc, "weighting", argc > 6 ? (float) std::atof (argv[6]) : 0.0f);
        setP (proc, "fft",       argc > 7 ? (float) std::atof (argv[7]) : 4.0f);
        setP (proc, "fill",      argc > 8 ? (float) std::atof (argv[8]) : 70.0f);
        setP (proc, "stereo",    argc > 9 ? (float) std::atof (argv[9]) : 0.0f);
        setP (proc, "grain",    argc > 10 ? (float) std::atof (argv[10]) : 35.0f);
        setP (proc, "metal",    argc > 11 ? (float) std::atof (argv[11]) : 0.0f);
        setP (proc, "mix",       100.0f);
        proc.prepareToPlay (sr, 512);

        const int latency = proc.getLatencySamples();
        const int block   = 512;

        for (int pos = 0; pos < len; pos += block)
        {
            const int n = juce::jmin (block, len - pos);
            juce::AudioBuffer<float> chunk (nch, n);

            for (int ch = 0; ch < nch; ++ch)
                chunk.copyFrom (ch, 0, buffer, ch, pos, n);

            juce::MidiBuffer midi;
            proc.processBlock (chunk, midi);

            for (int ch = 0; ch < nch; ++ch)
                buffer.copyFrom (ch, pos, chunk, ch, 0, n);
        }

        // realign (remove the reported latency) so A/B pictures line up
        if (latency > 0 && latency < len)
        {
            for (int ch = 0; ch < nch; ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                std::memmove (d, d + latency, (size_t) (len - latency) * sizeof (float));
                std::memset (d + (len - latency), 0, (size_t) latency * sizeof (float));
            }
        }

        // residual = processed - dry: this is the "codec noise" the plugin adds
        {
            const auto procMono = monoMix (buffer);
            std::vector<float> resid (procMono.size(), 0.0f);

            double pDry = 0.0, pRes = 0.0;

            for (size_t i = 0; i < resid.size(); ++i)
            {
                resid[i] = procMono[i] - dryMono[i];
                pDry += (double) dryMono[i] * dryMono[i];
                pRes += (double) resid[i] * resid[i];
            }

            std::printf ("  residual: %.1f dB re dry\n",
                         10.0 * std::log10 ((pRes + 1.0e-20) / (pDry + 1.0e-20)));

            auto resSpec = analyse (resid, sr);
            writeSpectrogram (resSpec, juce::File (prefix + "_resid_spec.png"),
                              "residual (processed - dry), +30 dB view", 30.0f);
            printStats (resSpec, "RESIDUAL");
        }

        // write it out for listening
        const juce::File outFile (prefix + ".wav");
        outFile.deleteFile();

        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (outFile.createOutputStream());

            if (os != nullptr)
            {
                std::unique_ptr<juce::AudioFormatWriter> w (
                    wav.createWriterFor (os.get(), sr, (unsigned) nch, 24, {}, 0));

                if (w != nullptr)
                {
                    os.release();
                    w->writeFromAudioSampleBuffer (buffer, 0, len);
                }
            }
        }

        std::printf ("  processed: Loss %.0f%% / Speed %.0f%% / mode %d / weighting %d / fft %d\n",
                     (float) std::atof (argv[3]), argc > 4 ? (float) std::atof (argv[4]) : 100.0f,
                     argc > 5 ? atoi (argv[5]) : 0, argc > 6 ? atoi (argv[6]) : 0,
                     argc > 7 ? atoi (argv[7]) : 4);
        std::printf ("  wrote %s\n", outFile.getFullPathName().toRawUTF8());
    }

    auto spec = analyse (monoMix (buffer), sr);
    writeSpectrogram (spec, juce::File (prefix + "_spec.png"),
                      inFile.getFileName() + (process ? "  (after Bitrot)" : "  (reference)"), 0.0f);

    printStats (spec, process ? "PROCESSED" : "REFERENCE");

    std::printf ("  wrote %s_spec.png\n\n", prefix.toRawUTF8());
    return 0;
}
