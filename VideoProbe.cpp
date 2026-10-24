//==============================================================================
//  VideoProbe - dump frame timing / motion / a few frames from an MP4 using
//  Media Foundation. Standalone tool (not part of the plugin build).
//
//  build:
//    g++ -O2 -o VideoProbe.exe VideoProbe.cpp ^
//        -lmfplat -lmfreadwrite -lmfuuid -lole32 -luuid -static ^
//        -static-libgcc -static-libstdc++
//
//  usage: VideoProbe <file.mp4> <outPrefix>
//==============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

template <typename T>
static void release (T*& p)
{
    if (p != nullptr) { p->Release(); p = nullptr; }
}

static void writeBMP (const wchar_t* path, const BYTE* bgrx, int w, int h, bool bottomUp)
{
    const int rowSize = (w * 3 + 3) & ~3;
    const DWORD imageSize = (DWORD) (rowSize * h);
    const DWORD fileSize  = 54 + imageSize;

    FILE* f = _wfopen (path, L"wb");
    if (f == nullptr)
        return;

    BYTE header[54] {};
    header[0] = 'B'; header[1] = 'M';
    memcpy (header + 2, &fileSize, 4);
    DWORD off = 54; memcpy (header + 10, &off, 4);
    DWORD hdrSize = 40; memcpy (header + 14, &hdrSize, 4);
    memcpy (header + 18, &w, 4);
    memcpy (header + 22, &h, 4);
    WORD planes = 1; memcpy (header + 26, &planes, 2);
    WORD bpp = 24; memcpy (header + 28, &bpp, 2);
    memcpy (header + 34, &imageSize, 4);

    fwrite (header, 1, 54, f);

    std::vector<BYTE> row ((size_t) rowSize, 0);

    for (int y = 0; y < h; ++y)
    {
        const int srcY = bottomUp ? y : (h - 1 - y);
        const BYTE* src = bgrx + (size_t) srcY * (size_t) w * 4;
        for (int x = 0; x < w; ++x)
        {
            row[(size_t) x * 3 + 0] = src[x * 4 + 0];
            row[(size_t) x * 3 + 1] = src[x * 4 + 1];
            row[(size_t) x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite (row.data(), 1, (size_t) rowSize, f);
    }

    fclose (f);
}

int main()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW (GetCommandLineW(), &argc);

    if (argc < 3)
    {
        std::printf ("usage: VideoProbe <file.mp4> <outPrefix>\n");
        return 1;
    }

    const std::wstring inPath  = argv[1];
    const std::wstring outPre  = argv[2];

    CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
    MFStartup (MF_VERSION);

    IMFSourceReader* reader = nullptr;
    HRESULT hr = E_FAIL;

    // MF needs video processing enabled to hand us RGB32 instead of compressed
    // or NV12 samples. Try the advanced processor first, then the basic one.
    for (int attempt = 0; attempt < 3 && reader == nullptr; ++attempt)
    {
        IMFAttributes* attrs = nullptr;
        MFCreateAttributes (&attrs, 2);

        if (attempt == 0)
        {
            attrs->SetUINT32 (MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
            attrs->SetUINT32 (MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        }
        else if (attempt == 1)
        {
            attrs->SetUINT32 (MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        }

        hr = MFCreateSourceReaderFromURL (inPath.c_str(), attrs, &reader);
        release (attrs);
    }

    if (FAILED (hr))
    {
        std::printf ("cannot open file (hr=0x%08lx)\n", (unsigned long) hr);
        return 1;
    }

    // duration
    {
        PROPVARIANT v; PropVariantInit (&v);
        if (SUCCEEDED (reader->GetPresentationAttribute (MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v)))
            std::printf ("duration      : %.3f s\n", (double) v.uhVal.QuadPart / 1.0e7);
        PropVariantClear (&v);
    }

    reader->SetStreamSelection (MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection (MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    IMFMediaType* mt = nullptr;
    MFCreateMediaType (&mt);
    mt->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt->SetGUID (MF_MT_SUBTYPE, MFVideoFormat_NV12);
    HRESULT hrType = reader->SetCurrentMediaType (MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt);
    std::printf ("set RGB32     : hr=0x%08lx\n", (unsigned long) hrType);

    IMFMediaType* cur = nullptr;
    reader->GetCurrentMediaType (MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);

    UINT32 w = 0, h = 0;
    MFGetAttributeSize (cur, MF_MT_FRAME_SIZE, &w, &h);

    UINT32 frNum = 0, frDen = 1;
    MFGetAttributeRatio (cur, MF_MT_FRAME_RATE, &frNum, &frDen);

    std::printf ("frame size    : %u x %u\n", w, h);
    std::printf ("frame rate    : %u/%u (%.3f fps)\n", frNum, frDen,
                 (double) frNum / (double) frDen);
    std::fflush (stdout);

    std::vector<double> motion;
    std::vector<BYTE>   prev;
    std::vector<std::vector<unsigned char>> thumbs;   // downsampled luma per frame
    int frameIndex = 0;

    while (true)
    {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = nullptr;

        hr = reader->ReadSample (MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample);

        if (FAILED (hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) || sample == nullptr)
        {
            release (sample);
            break;
        }

        IMFMediaBuffer* buf = nullptr;
        HRESULT hrConv = sample->ConvertToContiguousBuffer (&buf);

        BYTE* data = nullptr; DWORD len = 0;
        HRESULT hrLock = (buf != nullptr) ? buf->Lock (&data, nullptr, &len) : E_FAIL;

        if (frameIndex == 0 || frameIndex == 1)
        {
            std::printf ("frame %d: conv=0x%08lx lock=0x%08lx len=%lu need=%lu\\n",
                         frameIndex, (unsigned long) hrConv, (unsigned long) hrLock,
                         (unsigned long) len, (unsigned long) (w * h * 4));
            std::fflush (stdout);
        }

        if (FAILED (hrLock) || (size_t) len < (size_t) w * (size_t) h * 3 / 2) { release (buf); release (sample); break; }

        const size_t stride = (size_t) w * 4;

        // mean absolute luma difference vs the previous frame
        double m = 0.0;
        if (! prev.empty())
        {
            long long acc = 0;
            for (UINT32 y = 0; y < h; y += 4)
            {
                const BYTE* a = prev.data() + (size_t) y * stride;
                const BYTE* b = data + (size_t) y * stride;
                for (UINT32 x = 0; x < w; x += 4)
                {
                    const int la = (a[x * 4 + 1] * 2 + a[x * 4 + 0] + a[x * 4 + 2]) >> 2;
                    const int lb = (b[x * 4 + 1] * 2 + b[x * 4 + 0] + b[x * 4 + 2]) >> 2;
                    acc += std::abs (la - lb);
                }
            }
            const long long n = (long long) ((h + 3) / 4) * ((w + 3) / 4);
            m = (double) acc / (double) n;
        }

        if (frameIndex == 0)
        {
            UINT32 defStride = 0; cur->GetUINT32 (MF_MT_DEFAULT_STRIDE, &defStride);
            GUID sub {}; cur->GetGUID (MF_MT_SUBTYPE, &sub);
            std::printf ("subtype: %08lx-%04x-%04x  stride=%d  len=%lu\\n",
                         (unsigned long) sub.Data1, sub.Data2, sub.Data3, (int)(INT32)defStride, (unsigned long) len);
            const BYTE* uv = data + (size_t)((defStride < 0 ? -defStride : defStride) * h);
            std::printf ("corner Y=%u  UV=%u,%u\\n", data[0], uv[0], uv[1]);
            std::fflush (stdout);
        }

        motion.push_back (m);

        if (frameIndex % 4 == 0)
        {
            wchar_t p[512];
            _snwprintf (p, 512, L"%ls_%03d.bmp", outPre.c_str(), frameIndex);
            writeBMP (p, data, (int) w, (int) h, true);
        }

        prev.assign (data, data + stride * h);

        // downsampled luma for pose-similarity analysis (144 x 112)
        {
            const int tw = 144, th = 112;
            std::vector<unsigned char> t ((size_t) tw * th);
            for (int y = 0; y < th; ++y)
            {
                for (int x = 0; x < tw; ++x)
                {
                    const int sx = x * (int) w / tw;
                    const int sy = y * (int) h / th;
                    const BYTE* p = data + (size_t) sy * stride + (size_t) sx * 4;
                    t[(size_t) y * tw + x] = (unsigned char) ((p[1] * 2 + p[0] + p[2]) >> 2);
                }
            }
            thumbs.push_back (std::move (t));
        }

        buf->Unlock();
        release (buf);
        release (sample);

        ++frameIndex;
    }

    std::printf ("frames read   : %d\n", frameIndex);
    std::printf ("\nmotion (mean abs luma diff per frame):\n");

    for (size_t i = 0; i < motion.size(); ++i)
        std::printf ("%2d: %7.3f%s", (int) i, motion[i], (i % 8 == 7) ? "\n" : "   ");

    std::printf ("\n\n");

    // autocorrelation on the mean-removed motion signal
    const int n = (int) motion.size();
    if (n > 8)
    {
        double mean = 0.0;
        for (double v : motion) mean += v;
        mean /= (double) n;

        std::vector<double> x ((size_t) n);
        double e0 = 0.0;
        for (int i = 0; i < n; ++i) { x[(size_t) i] = motion[(size_t) i] - mean; e0 += x[(size_t) i] * x[(size_t) i]; }

        std::printf ("autocorrelation (normalised):\n");
        for (int lag = 2; lag < n / 2; ++lag)
        {
            double acc = 0.0;
            for (int i = 0; i + lag < n; ++i) acc += x[(size_t) i] * x[(size_t) i + lag];
            const double r = acc / (e0 + 1e-12);
            std::printf ("lag %2d : %6.3f\n", lag, r);
        }
    }

    // pose similarity: for each lag, mean abs difference of downsampled luma.
    // A local minimum = the loop repeats every <lag> frames.
    if (thumbs.size() > 8)
    {
        std::printf ("pose difference by lag (lower = better loop):\n");

        int    bestLag = 0;
        double bestVal = 1e9;

        for (int lag = 2; lag < (int) thumbs.size() / 2; ++lag)
        {
            double acc = 0.0;
            long long cnt = 0;

            for (size_t i = 0; i + (size_t) lag < thumbs.size(); ++i)
            {
                const auto& a = thumbs[i];
                const auto& b = thumbs[i + (size_t) lag];
                for (size_t k = 0; k < a.size(); ++k)
                {
                    acc += std::abs ((int) a[k] - (int) b[k]);
                    ++cnt;
                }
            }

            const double v = acc / (double) (cnt > 0 ? cnt : 1);
            std::printf ("  lag %2d (%5.1f ms, %5.1f BPM if one beat): %7.3f\n",
                         lag, 1000.0 * lag / 30.0, 1800.0 / (double) lag, v);

            if (v < bestVal) { bestVal = v; bestLag = lag; }
        }

        std::printf ("\nbest loop lag  : %d frames = %.1f ms  (%.1f BPM)\n",
                     bestLag, 1000.0 * bestLag / 30.0, 1800.0 / (double) bestLag);
    }

    release (cur);
    release (mt);
    release (reader);
    MFShutdown();
    CoUninitialize();
    return 0;
}
