// gh_audio.h -- Xbox headset capture (WinRT) → PCM ring → waveIn beds.
//
// Included once from guest_loader.cpp after Diag/Hex/g_dispatcher. All WinRT
// audio objects live on the UI thread (same rule as WGI). Guest waveIn* calls
// only touch the ring + WAVEHDR queue under g_audioMu.
#pragma once

#ifndef MMSYSERR_NOERROR
typedef UINT MMRESULT;
#define MMSYSERR_NOERROR      0
#define MMSYSERR_ERROR        1
#define MMSYSERR_BADDEVICEID  2
#define MMSYSERR_NOTENABLED   3
#define MMSYSERR_ALLOCATED    4
#define MMSYSERR_INVALHANDLE  5
#define MMSYSERR_NODRIVER     6
#define MMSYSERR_NOMEM        7
#define MMSYSERR_NOTSUPPORTED 8
#define MMSYSERR_BADERRNUM    9
#define MMSYSERR_INVALFLAG    10
#define MMSYSERR_INVALPARAM   11
#define WAVERR_BADFORMAT      32
#define WAVERR_STILLPLAYING   33
#define WAVERR_UNPREPARED     34
#define WAVE_MAPPER           ((UINT)-1)
#define WAVE_FORMAT_PCM       1
#define CALLBACK_TYPEMASK     0x00070000
#define CALLBACK_NULL         0x00000000
#define CALLBACK_WINDOW       0x00010000
#define CALLBACK_TASK         0x00020000
#define CALLBACK_FUNCTION     0x00030000
#define CALLBACK_THREAD       CALLBACK_TASK
#define CALLBACK_EVENT        0x00050000
#define WIM_OPEN              0x3BE
#define WIM_CLOSE             0x3BF
#define WIM_DATA              0x3C0
#define WHDR_DONE             0x00000001
#define WHDR_PREPARED         0x00000002
#define WHDR_BEGINLOOP        0x00000004
#define WHDR_ENDLOOP          0x00000008
#define WHDR_INQUEUE          0x00000010
#define MAXPNAMELEN           32
#define WAVE_FORMAT_1M08      0x00000001
#define WAVE_FORMAT_1S08      0x00000002
#define WAVE_FORMAT_1M16      0x00000004
#define WAVE_FORMAT_1S16      0x00000008
#define WAVE_FORMAT_2M08      0x00000010
#define WAVE_FORMAT_2S08      0x00000020
#define WAVE_FORMAT_2M16      0x00000040
#define WAVE_FORMAT_2S16      0x00000080
#define WAVE_FORMAT_4M08      0x00000100
#define WAVE_FORMAT_4S08      0x00000200
#define WAVE_FORMAT_4M16      0x00000400
#define WAVE_FORMAT_4S16      0x00000800
#define WAVE_FORMAT_48M08     0x00001000
#define WAVE_FORMAT_48S08     0x00002000
#define WAVE_FORMAT_48M16     0x00004000
#define WAVE_FORMAT_48S16     0x00008000
#define WAVE_FORMAT_96M08     0x00010000
#define WAVE_FORMAT_96S08     0x00020000
#define WAVE_FORMAT_96M16     0x00040000
#define WAVE_FORMAT_96S16     0x00080000
typedef struct tWAVEFORMATEX {
    WORD  wFormatTag, nChannels;
    DWORD nSamplesPerSec, nAvgBytesPerSec;
    WORD  nBlockAlign, wBitsPerSample, cbSize;
} WAVEFORMATEX;
typedef struct wavehdr_tag {
    LPSTR                 lpData;
    DWORD                 dwBufferLength, dwBytesRecorded;
    DWORD_PTR             dwUser;
    DWORD                 dwFlags, dwLoops;
    struct wavehdr_tag*   lpNext;
    DWORD_PTR             reserved;
} WAVEHDR;
typedef struct { WORD wMid, wPid; UINT vDriverVersion; CHAR szPname[MAXPNAMELEN];
                 DWORD dwFormats; WORD wChannels, wReserved1; } WAVEINCAPSA;
typedef struct { WORD wMid, wPid; UINT vDriverVersion; WCHAR szPname[MAXPNAMELEN];
                 DWORD dwFormats; WORD wChannels, wReserved1; } WAVEINCAPSW;
DECLARE_HANDLE(HWAVEIN);
typedef void (CALLBACK *WAVEINPROC)(HWAVEIN, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);
#endif

static const HWAVEIN kGhWaveIn = (HWAVEIN)(uintptr_t)0x1A0D1001;
static const int     kGhCapRate = 48000;
static const int     kGhRingN   = 48000 / 5;   // 200 ms s16 mono

MIDL_INTERFACE("5B0D3235-4DBA-4D44-865E-8F1D0E4FD04D")
IGhMemoryBufferByteAccess : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(BYTE** value, UINT32* capacity) = 0;
};

static std::mutex            g_audioMu;
static std::vector<int16_t>  g_audioRing;
static int                   g_audioR = 0, g_audioW = 0, g_audioAvail = 0;
static volatile long         g_audioWant = 0;
static volatile long         g_audioReady = 0;
static volatile long         g_audioFailed = 0;
static bool                  g_audioProbeStarted = false;
static bool                  g_audioBusy = false;
static int                   g_audioDevN = -1;
static DWORD                 g_audioLastRms = 0;

static bool                  g_winOpen = false, g_winStarted = false;
static DWORD                 g_winFlags = 0;
static DWORD_PTR             g_winCb = 0, g_winInst = 0;
static WAVEFORMATEX          g_winFmt{};
static std::deque<WAVEHDR*>  g_winQ;

static winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
    g_audioEnumOp{ nullptr };
static winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Media::Audio::CreateAudioGraphResult>
    g_audioGraphOp{ nullptr };
static winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Media::Audio::CreateAudioDeviceInputNodeResult>
    g_audioInOp{ nullptr };
static winrt::Windows::Foundation::IAsyncAction g_audioMcOp{ nullptr };
static winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Media::Capture::Frames::MediaFrameReader>
    g_audioFrOp{ nullptr };
static winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Media::Capture::Frames::MediaFrameReaderStartStatus>
    g_audioFrStartOp{ nullptr };

static winrt::Windows::Media::Audio::AudioGraph g_audioGraph{ nullptr };
static winrt::Windows::Media::Audio::AudioDeviceInputNode g_audioInNode{ nullptr };
static winrt::Windows::Media::Audio::AudioFrameOutputNode g_audioOutNode{ nullptr };
static winrt::Windows::Media::Capture::MediaCapture g_audioMc{ nullptr };
static winrt::Windows::Media::Capture::Frames::MediaFrameReader g_audioReader{ nullptr };
static winrt::event_token g_audioQTok{}, g_audioFTok{};

static void GhAudioPushS16(const int16_t* s, int n)
{
    if (!s || n <= 0) return;
    std::lock_guard<std::mutex> lk(g_audioMu);
    if (g_audioRing.empty()) g_audioRing.assign((size_t)kGhRingN, 0);
    for (int i = 0; i < n; ++i) {
        g_audioRing[(size_t)g_audioW] = s[i];
        g_audioW = (g_audioW + 1) % kGhRingN;
        if (g_audioAvail < kGhRingN) g_audioAvail++;
        else g_audioR = (g_audioR + 1) % kGhRingN;   // drop oldest
    }
}

static void GhAudioPushFloat(const float* f, UINT nch, UINT rate, UINT frames)
{
    if (!f || !frames || !nch) return;
    std::vector<int16_t> tmp;
    tmp.resize(frames);
    int64_t acc = 0;
    for (UINT i = 0; i < frames; ++i) {
        float m = 0.f;
        for (UINT c = 0; c < nch; ++c) m += f[i * nch + c];
        m /= (float)nch;
        if (m > 1.f) m = 1.f;
        if (m < -1.f) m = -1.f;
        int16_t s = (int16_t)(m * 32767.f);
        tmp[i] = s;
        acc += (int)s * (int)s;
    }
    if (rate == 0 || rate == (UINT)kGhCapRate) {
        GhAudioPushS16(tmp.data(), (int)tmp.size());
    } else {
        // linear resample to 48 kHz
        uint64_t outN = ((uint64_t)frames * (uint32_t)kGhCapRate) / rate;
        if (outN < 1) outN = 1;
        std::vector<int16_t> rs((size_t)outN);
        for (uint64_t o = 0; o < outN; ++o) {
            double src = (double)o * (double)rate / (double)kGhCapRate;
            UINT i0 = (UINT)src;
            if (i0 >= frames) i0 = frames - 1;
            UINT i1 = i0 + 1; if (i1 >= frames) i1 = frames - 1;
            double t = src - (double)i0;
            rs[(size_t)o] = (int16_t)((1.0 - t) * tmp[i0] + t * tmp[i1]);
        }
        GhAudioPushS16(rs.data(), (int)rs.size());
    }
    DWORD now = GetTickCount();
    if (now - g_audioLastRms > 2000) {
        g_audioLastRms = now;
        double rms = frames ? sqrt((double)acc / (double)frames) : 0;
        Diag("AUDIO cap rms=" + std::to_string((int)rms) +
             " frames=" + std::to_string(frames) +
             " hz=" + std::to_string(rate) +
             " ch=" + std::to_string(nch));
    }
}

static int GhAudioPull(void* dst, int dstBytes, int rate, int ch, int bits)
{
    if (!dst || dstBytes <= 0) return 0;
    if (rate < 8000) rate = kGhCapRate;
    if (ch < 1) ch = 1;
    if (ch > 2) ch = 2;
    if (bits != 8 && bits != 16) bits = 16;
    int bpf = ch * (bits / 8);
    int nOut = dstBytes / bpf;
    if (nOut <= 0) return 0;
    uint8_t* p = (uint8_t*)dst;
    std::lock_guard<std::mutex> lk(g_audioMu);
    if (g_audioRing.empty()) g_audioRing.assign((size_t)kGhRingN, 0);
    for (int o = 0; o < nOut; ++o) {
        double src = (double)o * (double)kGhCapRate / (double)rate;
        int i0 = (int)src;
        int16_t s = 0;
        if (g_audioAvail > 0) {
            int idx = (g_audioR + (i0 < g_audioAvail ? i0 : g_audioAvail - 1)) % kGhRingN;
            s = g_audioRing[(size_t)idx];
        }
        if (bits == 16) {
            int16_t* d = (int16_t*)(p + o * bpf);
            d[0] = s;
            if (ch == 2) d[1] = s;
        } else {
            uint8_t u = (uint8_t)((s / 256) + 128);
            p[o * bpf] = u;
            if (ch == 2) p[o * bpf + 1] = u;
        }
    }
    int consume = (int)((int64_t)nOut * kGhCapRate / rate);
    if (consume > g_audioAvail) consume = g_audioAvail;
    g_audioR = (g_audioR + consume) % kGhRingN;
    g_audioAvail -= consume;
    return nOut * bpf;
}

static void GhAudioFire(UINT msg, DWORD_PTR p1, DWORD_PTR p2)
{
    DWORD kind = g_winFlags & CALLBACK_TYPEMASK;
    if (kind == CALLBACK_FUNCTION && g_winCb) {
        ((WAVEINPROC)g_winCb)(kGhWaveIn, msg, g_winInst, p1, p2);
    } else if (kind == CALLBACK_EVENT && g_winCb) {
        SetEvent((HANDLE)g_winCb);
    } else if (kind == CALLBACK_WINDOW && g_winCb) {
        PostMessageW((HWND)g_winCb, msg, (WPARAM)kGhWaveIn, (LPARAM)p1);
    }
}

static void GhAudioFillWaveIn()
{
    if (!g_winStarted) return;
    for (;;) {
        WAVEHDR* h = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_audioMu);
            if (g_winQ.empty()) return;
            h = g_winQ.front();
            g_winQ.pop_front();
        }
        if (!h || !h->lpData || !h->dwBufferLength) continue;
        int n = GhAudioPull(h->lpData, (int)h->dwBufferLength,
                            (int)g_winFmt.nSamplesPerSec,
                            (int)g_winFmt.nChannels,
                            (int)g_winFmt.wBitsPerSample);
        h->dwBytesRecorded = (DWORD)n;
        h->dwFlags = (h->dwFlags | WHDR_DONE) & ~WHDR_INQUEUE;
        GhAudioFire(WIM_DATA, (DWORD_PTR)h, 0);
    }
}

static void GhAudioIngestAudioFrame(winrt::Windows::Media::AudioFrame const& frame,
                                    UINT nch, UINT rate)
{
    if (!frame) return;
    auto buf = frame.LockBuffer(winrt::Windows::Media::AudioBufferAccessMode::Read);
    auto pref = buf.CreateReference();
    auto acc = pref.as<IGhMemoryBufferByteAccess>();
    BYTE* data = nullptr; UINT32 cap = 0;
    if (FAILED(acc->GetBuffer(&data, &cap)) || !data || !cap) return;
    UINT frames = cap / (nch * sizeof(float));
    if (frames) GhAudioPushFloat((const float*)data, nch, rate, frames);
}

static void GhAudioOnQuantum(winrt::Windows::Media::Audio::AudioGraph const&,
                             winrt::Windows::Foundation::IInspectable const&)
{
    if (!g_audioOutNode) return;
    try {
        auto frame = g_audioOutNode.GetFrame();
        UINT nch = 1, rate = kGhCapRate;
        if (g_audioGraph) {
            auto ep = g_audioGraph.EncodingProperties();
            nch = ep.ChannelCount();
            rate = ep.SampleRate();
        }
        GhAudioIngestAudioFrame(frame, nch, rate);
    } catch (...) {}
}

static void GhAudioOnFrame(winrt::Windows::Media::Capture::Frames::MediaFrameReader const& sender,
                           winrt::Windows::Media::Capture::Frames::MediaFrameArrivedEventArgs const&)
{
    try {
        auto ref = sender.TryAcquireLatestFrame();
        if (!ref) return;
        auto amf = ref.AudioMediaFrame();
        if (!amf) return;
        auto props = amf.AudioEncodingProperties();
        UINT nch = props.ChannelCount();
        UINT rate = props.SampleRate();
        GhAudioIngestAudioFrame(amf.GetAudioFrame(), nch, rate);
    } catch (...) {}
}

static void GhAudioTryMediaCapture();  // fwd

static void GhAudioGraphStarted()
{
    InterlockedExchange(&g_audioReady, 1);
    InterlockedExchange(&g_audioFailed, 0);
    Diag("AUDIO graph ready");
}

static void GhAudioTryGraph()
{
    using namespace winrt::Windows::Media::Audio;
    using namespace winrt::Windows::Media::Capture;
    using namespace winrt::Windows::Media::Render;
    try {
        AudioGraphSettings settings(AudioRenderCategory::Communications);
        g_audioGraphOp = AudioGraph::CreateAsync(settings);
        g_audioGraphOp.Completed([](auto const& op, auto const& st) {
            try {
                if (st != winrt::Windows::Foundation::AsyncStatus::Completed) {
                    Diag("AUDIO AudioGraph CreateAsync status=" + std::to_string((int)st));
                    GhAudioTryMediaCapture();
                    return;
                }
                auto r = op.GetResults();
                if (r.Status() != AudioGraphCreationStatus::Success) {
                    Diag("AUDIO AudioGraph status=" + std::to_string((int)r.Status()));
                    GhAudioTryMediaCapture();
                    return;
                }
                g_audioGraph = r.Graph();
                g_audioInOp = g_audioGraph.CreateDeviceInputNodeAsync(MediaCategory::Communications);
                g_audioInOp.Completed([](auto const& iop, auto const& ist) {
                    try {
                        if (ist != winrt::Windows::Foundation::AsyncStatus::Completed) {
                            Diag("AUDIO input node async fail");
                            GhAudioTryMediaCapture();
                            return;
                        }
                        auto ir = iop.GetResults();
                        if (ir.Status() != AudioDeviceNodeCreationStatus::Success) {
                            Diag("AUDIO input node status=" + std::to_string((int)ir.Status()));
                            GhAudioTryMediaCapture();
                            return;
                        }
                        g_audioInNode = ir.DeviceInputNode();
                        g_audioOutNode = g_audioGraph.CreateFrameOutputNode();
                        g_audioInNode.AddOutgoingConnection(g_audioOutNode);
                        g_audioQTok = g_audioGraph.QuantumStarted(GhAudioOnQuantum);
                        g_audioGraph.Start();
                        GhAudioGraphStarted();
                    } catch (winrt::hresult_error const& e) {
                        Diag("AUDIO input node threw hr=" + Hex((uint32_t)e.code()));
                        GhAudioTryMediaCapture();
                    }
                    g_audioInOp = nullptr;
                });
            } catch (winrt::hresult_error const& e) {
                Diag("AUDIO AudioGraph threw hr=" + Hex((uint32_t)e.code()));
                GhAudioTryMediaCapture();
            }
            g_audioGraphOp = nullptr;
        });
    } catch (winrt::hresult_error const& e) {
        Diag("AUDIO AudioGraph CreateAsync threw hr=" + Hex((uint32_t)e.code()));
        GhAudioTryMediaCapture();
    }
}

static void GhAudioTryMediaCapture()
{
    using namespace winrt::Windows::Media::Capture;
    using namespace winrt::Windows::Media::Capture::Frames;
    try {
        g_audioMc = MediaCapture();
        MediaCaptureInitializationSettings s;
        s.StreamingCaptureMode(StreamingCaptureMode::Audio);
        s.MediaCategory(MediaCategory::Communications);
        g_audioMcOp = g_audioMc.InitializeAsync(s);
        g_audioMcOp.Completed([](auto const&, auto const& st) {
            try {
                if (st != winrt::Windows::Foundation::AsyncStatus::Completed) {
                    Diag("AUDIO MediaCapture init status=" + std::to_string((int)st));
                    InterlockedExchange(&g_audioFailed, 1);
                    return;
                }
                MediaFrameSource src{ nullptr };
                for (auto const& kv : g_audioMc.FrameSources()) {
                    auto v = kv.Value();
                    if (v.Info().MediaStreamType() == MediaStreamType::Audio) {
                        src = v; break;
                    }
                }
                if (!src) {
                    Diag("AUDIO MediaCapture no audio FrameSource");
                    InterlockedExchange(&g_audioFailed, 1);
                    return;
                }
                g_audioFrOp = g_audioMc.CreateFrameReaderAsync(src);
                g_audioFrOp.Completed([](auto const& rop, auto const& rst) {
                    try {
                        if (rst != winrt::Windows::Foundation::AsyncStatus::Completed) {
                            Diag("AUDIO FrameReader create fail");
                            InterlockedExchange(&g_audioFailed, 1);
                            return;
                        }
                        g_audioReader = rop.GetResults();
                        g_audioReader.AcquisitionMode(MediaFrameReaderAcquisitionMode::Buffered);
                        g_audioFTok = g_audioReader.FrameArrived(GhAudioOnFrame);
                        g_audioFrStartOp = g_audioReader.StartAsync();
                        g_audioFrStartOp.Completed([](auto const& sop, auto const& sst) {
                            try {
                                if (sst == winrt::Windows::Foundation::AsyncStatus::Completed &&
                                    sop.GetResults() == MediaFrameReaderStartStatus::Success) {
                                    InterlockedExchange(&g_audioReady, 1);
                                    InterlockedExchange(&g_audioFailed, 0);
                                    Diag("AUDIO MediaCapture reader ready");
                                } else {
                                    Diag("AUDIO FrameReader start fail");
                                    InterlockedExchange(&g_audioFailed, 1);
                                }
                            } catch (winrt::hresult_error const& e) {
                                Diag("AUDIO FrameReader start hr=" + Hex((uint32_t)e.code()));
                                InterlockedExchange(&g_audioFailed, 1);
                            }
                            g_audioFrStartOp = nullptr;
                        });
                    } catch (winrt::hresult_error const& e) {
                        Diag("AUDIO FrameReader hr=" + Hex((uint32_t)e.code()));
                        InterlockedExchange(&g_audioFailed, 1);
                    }
                    g_audioFrOp = nullptr;
                });
            } catch (winrt::hresult_error const& e) {
                Diag("AUDIO MediaCapture hr=" + Hex((uint32_t)e.code()));
                InterlockedExchange(&g_audioFailed, 1);
            }
            g_audioMcOp = nullptr;
        });
    } catch (winrt::hresult_error const& e) {
        Diag("AUDIO MediaCapture threw hr=" + Hex((uint32_t)e.code()));
        InterlockedExchange(&g_audioFailed, 1);
    }
}

static void GhAudioBeginCapture()
{
    if (g_audioBusy || g_audioReady) return;
    g_audioBusy = true;
    Diag("AUDIO begin capture (graph then MediaCapture)");
    GhAudioTryGraph();
}

static void GhAudioProbe()
{
    if (g_audioProbeStarted) return;
    g_audioProbeStarted = true;
    try {
        using namespace winrt::Windows::Devices::Enumeration;
        using namespace winrt::Windows::Media::Devices;
        g_audioEnumOp = DeviceInformation::FindAllAsync(MediaDevice::GetAudioCaptureSelector());
        g_audioEnumOp.Completed([](auto const& op, auto const& st) {
            try {
                if (st != winrt::Windows::Foundation::AsyncStatus::Completed) {
                    Diag("AUDIO enum async status=" + std::to_string((int)st));
                    g_audioDevN = 0;
                } else {
                    auto devs = op.GetResults();
                    g_audioDevN = (int)devs.Size();
                    Diag("AUDIO devices=" + std::to_string(g_audioDevN));
                    unsigned n = devs.Size();
                    if (n > 8) n = 8;
                    for (unsigned i = 0; i < n; ++i) {
                        auto d = devs.GetAt(i);
                        std::wstring wn = d.Name().c_str();
                        Diag(std::string("AUDIO device[") + std::to_string(i) + "] " +
                             std::string(wn.begin(), wn.end()));
                    }
                }
            } catch (winrt::hresult_error const& e) {
                Diag("AUDIO enum hr=" + Hex((uint32_t)e.code()));
                g_audioDevN = 0;
            }
            g_audioEnumOp = nullptr;
            if (g_audioWant) GhAudioBeginCapture();
        });
    } catch (winrt::hresult_error const& e) {
        Diag("AUDIO enum threw hr=" + Hex((uint32_t)e.code()));
        g_audioDevN = 0;
    }
}

static void GhAudioWant()
{
    // Guest thread may call this. Only set the flag; GhAudioTick starts WinRT.
    InterlockedExchange(&g_audioWant, 1);
}

static void GhAudioStopCapture()
{
    InterlockedExchange(&g_audioWant, 0);
    try {
        if (g_audioGraph) {
            g_audioGraph.QuantumStarted(g_audioQTok);
            g_audioGraph.Stop();
        }
    } catch (...) {}
    try {
        if (g_audioReader) {
            g_audioReader.FrameArrived(g_audioFTok);
            g_audioReader.StopAsync();
        }
    } catch (...) {}
    g_audioOutNode = nullptr;
    g_audioInNode = nullptr;
    g_audioGraph = nullptr;
    g_audioReader = nullptr;
    g_audioMc = nullptr;
    InterlockedExchange(&g_audioReady, 0);
    g_audioBusy = false;
    Diag("AUDIO capture stopped");
}

static void GhAudioTick()
{
    if (!g_audioProbeStarted) GhAudioProbe();
    if (g_audioWant && !g_audioBusy && !g_audioReady && !g_audioFailed)
        GhAudioBeginCapture();
    GhAudioFillWaveIn();
}
