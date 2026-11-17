// gh_x86_wave.h -- waveOut for a 32-bit guest on the native track, over XAudio2.
//
// The console's winmm has no wave output, and Adobe AIR plays every sound
// through waveOut: one device, 16-bit stereo PCM, CALLBACK_FUNCTION, a handful
// of WAVEHDRs it refills when its waveOutProc sees WOM_DONE. Until this file
// the bed said MMSYSERR_NODRIVER and AIR games (Super Fancy Pants Adventure)
// ran silent. Each open device is one XAudio2 source voice on gh_dsound.h's
// engine; a WAVEHDR's bytes are queued straight from guest memory, which on
// the native track is ordinary process memory, and stay put until WOM_DONE
// hands the header back.
//
// The notification is the part with threads in it. XAudio2 reports a finished
// buffer on its own thread, which has no guest context, so:
//
//   * CALLBACK_FUNCTION: a guest thread of ours (started like any CreateThread,
//     its start routine the ghWaveOutPump bed) waits for finished buffers and
//     calls the guest's waveOutProc. Real winmm calls it from a thread of its
//     own too, so a guest already copes with that. WOM_OPEN and WOM_CLOSE are
//     sent from inside waveOutOpen/waveOutClose, as winmm does.
//   * CALLBACK_EVENT, _WINDOW, _THREAD need no guest code: SetEvent, or a
//     posted MM_WOM_DONE, from the XAudio2 thread.
//
// A buffer's end is matched to its header by a per-submission number, not the
// header address: AIR resubmits a header the moment it gets it back, and
// waveOutReset completes everything queued itself, so a late OnBufferEnd from
// XAudio2 for an earlier submission must not complete the new one.
//
// The interpreter track keeps the "no driver" beds in gh_x86_beds.h: these
// are registered only by RegisterNativeBeds.

#pragma once

#ifdef GH_LAUNCHER

#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace ghx86 {
namespace native {

constexpr uint32_t kMmNoError = 0, kMmBadDeviceId = 2, kMmNoDriver = 6, kMmInvalHandle = 5,
                   kMmNotSupported = 8, kMmInvalParam = 11, kWaveBadFormat = 32, kWaveStillPlaying = 33;
constexpr uint32_t kWhdrDone = 0x1, kWhdrPrepared = 0x2, kWhdrInQueue = 0x10;
constexpr uint32_t kWomOpen = 0x3BB, kWomClose = 0x3BC, kWomDone = 0x3BD;
constexpr uint32_t kCbTypeMask = 0x70000, kCbWindow = 0x10000, kCbThread = 0x20000,
                   kCbFunction = 0x30000, kCbEvent = 0x50000, kWaveFormatQuery = 0x1;

// WAVEHDR as the guest lays it out: lpData, dwBufferLength, dwBytesRecorded,
// dwUser, dwFlags, dwLoops, lpNext, reserved -- 32 bytes (48 on x64).
struct WaveHdr32 { uint32_t data, length, recorded, user, flags, loops, next, reserved; };
static_assert(sizeof(WaveHdr32) == 32, "WAVEHDR is 32 bytes on x86");

struct WaveDev;

struct WaveVoiceCb : IXAudio2VoiceCallback {
    WaveDev* d = nullptr;
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnBufferEnd(void* ctx) noexcept override;
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {}
};

struct WaveDev {
    uint32_t handle = 0;
    uint32_t cb_type = 0, cb = 0, inst = 0;
    WAVEFORMATEX fmt{};
    IXAudio2SourceVoice* voice = nullptr;
    WaveVoiceCb vcb;
    std::mutex mu;
    std::map<uint64_t, uint32_t> inflight;     // submission number -> guest WAVEHDR
    uint64_t next_seq = 1;
    std::deque<uint32_t> done;                 // finished headers the pump has not delivered
    uint64_t base_samples = 0;                 // SamplesPlayed at the last waveOutReset
    uint32_t volume = 0xFFFFFFFFu;
    bool paused = false, closing = false;
    uint64_t delivered = 0;                    // WOM_DONEs the pump has delivered (log only)
    HANDLE wake = nullptr;                     // pump: something is in `done`, or closing
    HANDLE pump = nullptr;                     // our handle to the pump's host thread
};

struct WaveState {
    std::mutex mu;
    std::map<uint32_t, WaveDev*> devs;
    uint32_t next = 0x57A00001u;
};
inline WaveState& wave_state() { static WaveState s; return s; }
inline WaveDev* wave_dev(uint32_t h)
{
    WaveState& s = wave_state();
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.devs.find(h);
    return it == s.devs.end() ? nullptr : it->second;
}

// Marks a header finished and tells whoever the guest asked to be told. Called
// with d->mu NOT held (a posted message or an event is fine either way; the
// pump takes the lock itself).
inline void wave_complete(WaveDev* d, uint32_t hdr)
{
    WaveHdr32* h = (WaveHdr32*)(uintptr_t)hdr;
    h->flags = (h->flags & ~kWhdrInQueue) | kWhdrDone;
    switch (d->cb_type) {
    case kCbFunction: {
        { std::lock_guard<std::mutex> lk(d->mu); d->done.push_back(hdr); }
        SetEvent(d->wake);
        break;
    }
    case kCbEvent:  if (d->cb) SetEvent((HANDLE)(intptr_t)(int32_t)d->cb); break;
    case kCbWindow: wndq_post(d->cb, kWomDone, d->handle, hdr); break;
    case kCbThread: wndq_post(0, kWomDone, d->handle, hdr, d->cb); break;
    default: break;
    }
}

inline void WaveVoiceCb::OnBufferEnd(void* ctx) noexcept
{
    uint32_t hdr = 0;
    {
        std::lock_guard<std::mutex> lk(d->mu);
        auto it = d->inflight.find((uint64_t)(uintptr_t)ctx);
        if (it == d->inflight.end()) return;   // completed already by waveOutReset
        hdr = it->second;
        d->inflight.erase(it);
    }
    wave_complete(d, hdr);
}

inline uint64_t wave_samples(WaveDev* d)
{
    XAUDIO2_VOICE_STATE st{};
    d->voice->GetState(&st, 0);
    return st.SamplesPlayed >= d->base_samples ? st.SamplesPlayed - d->base_samples : 0;
}

inline void RegisterWaveBeds(Machine& m)
{
    ThunkRegistry& R = m.registry;

    // The pump: the start routine of the guest thread a CALLBACK_FUNCTION
    // device gets. Lockless, since it waits; the guest's waveOutProc runs from
    // here exactly as from any guest thread.
    GH_X86_NBED(R, "winmm.dll", "ghWaveOutPump", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            WaveDev* d = wave_dev(a[0]);
            if (!d) return 0;
            for (;;) {
                WaitForSingleObject(d->wake, INFINITE);
                for (;;) {
                    uint32_t hdr = 0;
                    {
                        std::lock_guard<std::mutex> lk(d->mu);
                        if (d->done.empty()) break;
                        hdr = d->done.front(); d->done.pop_front();
                    }
                    const uint32_t args[5] = { d->handle, kWomDone, d->inst, hdr, 0 };
                    m.call_guest32(d->cb, args, 5);
                    // Evidence that the guest keeps refilling, at powers of two so a
                    // long run costs a few dozen lines (no one can hear the console).
                    const uint64_t n = ++d->delivered;
                    // (Not the voice: waveOutReset may be replacing it on another thread.)
                    if ((n & (n - 1)) == 0)
                        log("X86 waveOut WOM_DONE n=" + std::to_string(n) + " bytes=" +
                            std::to_string(((WaveHdr32*)(uintptr_t)hdr)->length) + " t=" +
                            std::to_string(GetTickCount64() / 1000) + "s");
                }
                std::lock_guard<std::mutex> lk(d->mu);
                if (d->closing && d->done.empty()) return 0;
            }
        });

    GH_X86_NBED(R, "winmm.dll", "waveOutGetNumDevs", 0,
        [](Machine&, const uint32_t*) -> uint64_t { return ghds::Engine() ? 1 : 0; });

    // WAVEOUTCAPS: wMid, wPid, vDriverVersion, szPname[32], dwFormats,
    // wChannels, wReserved1, dwSupport -- 84 bytes wide, 52 narrow.
    for (int wide = 0; wide < 2; ++wide)
        GH_X86_NBED(R, "winmm.dll", wide ? "waveOutGetDevCapsW" : "waveOutGetDevCapsA", 12,
            wide ? +[](Machine&, const uint32_t* a) -> uint64_t {
                if (a[0] != 0 && a[0] != 0xFFFFFFFFu) return kMmBadDeviceId;
                if (!ghds::Engine()) return kMmNoDriver;
                uint8_t c[84] = {};
                const char* nm = "Greenhouse Audio";
                for (int i = 0; nm[i]; ++i) c[8 + 2 * i] = (uint8_t)nm[i];
                const uint32_t formats = 0xFFFFF, support = 0x4 | 0x8 | 0x20;   // VOLUME | LRVOLUME | SAMPLEACCURATE
                std::memcpy(c + 72, &formats, 4); c[76] = 2; std::memcpy(c + 80, &support, 4);
                std::memcpy((void*)(uintptr_t)a[1], c, a[2] < 84 ? a[2] : 84);
                return kMmNoError;
            } : +[](Machine&, const uint32_t* a) -> uint64_t {
                if (a[0] != 0 && a[0] != 0xFFFFFFFFu) return kMmBadDeviceId;
                if (!ghds::Engine()) return kMmNoDriver;
                uint8_t c[52] = {};
                std::memcpy(c + 8, "Greenhouse Audio", 16);
                const uint32_t formats = 0xFFFFF, support = 0x4 | 0x8 | 0x20;
                std::memcpy(c + 40, &formats, 4); c[44] = 2; std::memcpy(c + 48, &support, 4);
                std::memcpy((void*)(uintptr_t)a[1], c, a[2] < 52 ? a[2] : 52);
                return kMmNoError;
            });

    // (phwo, uDeviceID, pwfx, dwCallback, dwInstance, fdwOpen)
    GH_X86_NBED(R, "winmm.dll", "waveOutOpen", 24,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (a[0]) put32(a[0], 0);
            if (a[1] != 0 && a[1] != 0xFFFFFFFFu) return kMmBadDeviceId;
            if (!a[2]) return kMmInvalParam;
            IXAudio2* xa = ghds::Engine();
            if (!xa) return kMmNoDriver;
            const WAVEFORMATEX* f = (const WAVEFORMATEX*)(uintptr_t)a[2];
            const bool pcm = f->wFormatTag == WAVE_FORMAT_PCM && (f->wBitsPerSample == 8 || f->wBitsPerSample == 16);
            const bool flt = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && f->wBitsPerSample == 32;
            if (!(pcm || flt) || f->nChannels < 1 || f->nChannels > 2 || f->nSamplesPerSec < 1000 ||
                f->nSamplesPerSec > 192000 || f->nBlockAlign != f->nChannels * f->wBitsPerSample / 8)
                return kWaveBadFormat;
            if (a[5] & kWaveFormatQuery) return kMmNoError;
            const uint32_t cbt = a[5] & kCbTypeMask;
            if (cbt != 0 && cbt != kCbFunction && cbt != kCbEvent && cbt != kCbWindow && cbt != kCbThread)
                return kMmInvalParam;

            WaveDev* d = new WaveDev;
            d->vcb.d = d;
            d->fmt = *f; d->fmt.cbSize = 0;
            d->cb_type = cbt; d->cb = a[3]; d->inst = a[4];
            HRESULT hr = xa->CreateSourceVoice(&d->voice, &d->fmt, 0, 2.0f, &d->vcb, nullptr, nullptr);
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 4)
                log("X86 waveOutOpen " + std::to_string(f->nSamplesPerSec) + "Hz ch=" + std::to_string(f->nChannels) +
                    " bits=" + std::to_string(f->wBitsPerSample) + " cb=" + hex(cbt) + " hr=" + hex((uint32_t)hr));
            if (FAILED(hr)) { delete d; return kWaveBadFormat; }
            {
                WaveState& s = wave_state();
                std::lock_guard<std::mutex> lk(s.mu);
                d->handle = s.next++;
                s.devs[d->handle] = d;
            }
            if (cbt == kCbFunction) {
                d->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                // A guest thread, made the way the guest's own CreateThread makes
                // one; it needs the gil for that, and this bed does not hold it.
                // thunk_for may add a thunk slot, which is also gil work.
                const HostFn* ct = m.registry.find("kernel32.dll", "CreateThread");
                uint32_t h = 0;
                S.gil.lock();
                const uint32_t start = m.thunk_for("winmm.dll", "ghWaveOutPump");
                if (ct && start) {
                    const uint32_t args[6] = { 0, 0x10000, start, d->handle, 0, 0 };
                    h = (uint32_t)ct->invoke(m, args);
                }
                S.gil.unlock();
                if (!h) {
                    log("X86 waveOutOpen: no pump thread, so no WOM_DONE -- silent");
                    { WaveState& s = wave_state(); std::lock_guard<std::mutex> lk(s.mu); s.devs.erase(d->handle); }
                    d->voice->DestroyVoice(); CloseHandle(d->wake); delete d;
                    return kMmNoDriver;
                }
                d->pump = (HANDLE)(intptr_t)(int32_t)h;    // the guest never saw this handle; it is ours
            }
            d->voice->Start(0);
            if (a[0]) put32(a[0], d->handle);
            if (cbt == kCbFunction) {
                const uint32_t args[5] = { d->handle, kWomOpen, d->inst, 0, 0 };
                m.call_guest32(d->cb, args, 5);
            } else if (cbt == kCbEvent && d->cb) {
                SetEvent((HANDLE)(intptr_t)(int32_t)d->cb);
            }
            return kMmNoError;
        });

    GH_X86_NBED(R, "winmm.dll", "waveOutPrepareHeader", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (!wave_dev(a[0])) return kMmInvalHandle;
            if (!a[1] || a[2] < sizeof(WaveHdr32)) return kMmInvalParam;
            WaveHdr32* h = (WaveHdr32*)(uintptr_t)a[1];
            h->flags |= kWhdrPrepared;
            return kMmNoError;
        });
    GH_X86_NBED(R, "winmm.dll", "waveOutUnprepareHeader", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (!wave_dev(a[0])) return kMmInvalHandle;
            if (!a[1] || a[2] < sizeof(WaveHdr32)) return kMmInvalParam;
            WaveHdr32* h = (WaveHdr32*)(uintptr_t)a[1];
            if (h->flags & kWhdrInQueue) return kWaveStillPlaying;
            h->flags &= ~kWhdrPrepared;
            return kMmNoError;
        });

    GH_X86_NBED(R, "winmm.dll", "waveOutWrite", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            WaveDev* d = wave_dev(a[0]);
            if (!d) return kMmInvalHandle;
            if (!a[1] || a[2] < sizeof(WaveHdr32)) return kMmInvalParam;
            WaveHdr32* h = (WaveHdr32*)(uintptr_t)a[1];
            if (!(h->flags & kWhdrPrepared)) return 34;               // WAVERR_UNPREPARED
            if (h->flags & kWhdrInQueue) return kWaveStillPlaying;
            const uint32_t len = h->length - h->length % d->fmt.nBlockAlign;
            h->flags = (h->flags & ~kWhdrDone) | kWhdrInQueue;
            if (!len || !h->data) { wave_complete(d, a[1]); return kMmNoError; }
            uint64_t seq;
            {
                std::lock_guard<std::mutex> lk(d->mu);
                seq = d->next_seq++;
                d->inflight[seq] = a[1];
            }
            XAUDIO2_BUFFER b{};
            b.AudioBytes = len;
            b.pAudioData = (const BYTE*)(uintptr_t)h->data;
            b.pContext = (void*)(uintptr_t)seq;
            if (FAILED(d->voice->SubmitSourceBuffer(&b))) {
                { std::lock_guard<std::mutex> lk(d->mu); d->inflight.erase(seq); }
                wave_complete(d, a[1]);                               // dropped, but the guest gets it back
            }
            return kMmNoError;
        });

    GH_X86_NBED(R, "winmm.dll", "waveOutPause", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            WaveDev* d = wave_dev(a[0]);
            if (!d) return kMmInvalHandle;
            d->voice->Stop(0); d->paused = true;
            return kMmNoError;
        });
    GH_X86_NBED(R, "winmm.dll", "waveOutRestart", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            WaveDev* d = wave_dev(a[0]);
            if (!d) return kMmInvalHandle;
            d->voice->Start(0); d->paused = false;
            return kMmNoError;
        });

    // Every queued header comes back done, in order, and the position goes to 0.
    GH_X86_NBED(R, "winmm.dll", "waveOutReset", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            WaveDev* d = wave_dev(a[0]);
            if (!d) return kMmInvalHandle;
            // A new voice rather than FlushSourceBuffers, which keeps the buffer
            // that is playing: the guest refills a header the moment it is
            // back, and the old voice would go on reading it.
            IXAudio2SourceVoice* fresh = nullptr;
            if (FAILED(ghds::Engine()->CreateSourceVoice(&fresh, &d->fmt, 0, 2.0f, &d->vcb, nullptr, nullptr)))
                fresh = nullptr;
            if (fresh) {
                d->voice->DestroyVoice();             // waits for its callbacks
                d->voice = fresh;
                d->base_samples = 0;
                if (d->volume != 0xFFFFFFFFu)
                    d->voice->SetVolume(((d->volume & 0xFFFF) + (d->volume >> 16)) / (2.0f * 0xFFFF));
            } else {
                d->voice->Stop(0);
                d->voice->FlushSourceBuffers();
                XAUDIO2_VOICE_STATE st{};
                d->voice->GetState(&st, 0);
                d->base_samples = st.SamplesPlayed;
            }
            std::vector<uint32_t> back;
            {
                std::lock_guard<std::mutex> lk(d->mu);
                for (auto& kv : d->inflight) back.push_back(kv.second);
                d->inflight.clear();
            }
            for (uint32_t h : back) wave_complete(d, h);
            if (!d->paused) d->voice->Start(0);
            return kMmNoError;
        });

    GH_X86_NBED(R, "winmm.dll", "waveOutClose", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            WaveDev* d = wave_dev(a[0]);
            if (!d) return kMmInvalHandle;
            { std::lock_guard<std::mutex> lk(d->mu); if (!d->inflight.empty()) return kWaveStillPlaying; }
            { WaveState& s = wave_state(); std::lock_guard<std::mutex> lk(s.mu); s.devs.erase(d->handle); }
            d->voice->DestroyVoice();                 // waits for its callbacks
            if (d->pump) {
                { std::lock_guard<std::mutex> lk(d->mu); d->closing = true; }
                SetEvent(d->wake);
                // Bounded: a waveOutProc that waits on the thread closing the
                // device would otherwise hang both.
                if (WaitForSingleObject(d->pump, 2000) != WAIT_OBJECT_0) log("X86 waveOutClose: the pump did not stop");
                CloseHandle(d->pump);
            }
            if (d->cb_type == kCbFunction) {
                const uint32_t args[5] = { d->handle, kWomClose, d->inst, 0, 0 };
                m.call_guest32(d->cb, args, 5);
            } else if (d->cb_type == kCbEvent && d->cb) {
                SetEvent((HANDLE)(intptr_t)(int32_t)d->cb);
            }
            if (d->wake) CloseHandle(d->wake);
            delete d;
            return kMmNoError;
        });

    // (hwo, MMTIME*, cb). MMTIME is wType then a union; bytes when the guest
    // asks for a format we do not keep.
    GH_X86_NBED(R, "winmm.dll", "waveOutGetPosition", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            WaveDev* d = wave_dev(a[0]);
            if (!d) return kMmInvalHandle;
            if (!a[1] || a[2] < 8) return kMmInvalParam;
            const uint64_t s = wave_samples(d);
            uint32_t* t = (uint32_t*)(uintptr_t)a[1];
            switch (t[0]) {
            case 1:  t[1] = (uint32_t)(s * 1000 / d->fmt.nSamplesPerSec); break;     // TIME_MS
            case 2:  t[1] = (uint32_t)s; break;                                       // TIME_SAMPLES
            default: t[0] = 4; t[1] = (uint32_t)(s * d->fmt.nBlockAlign); break;     // TIME_BYTES
            }
            return kMmNoError;
        });

    // Low word left, high word right; one gain for the voice is enough here.
    GH_X86_NBED(R, "winmm.dll", "waveOutSetVolume", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            WaveDev* d = wave_dev(a[0]);
            if (!d) return a[0] ? kMmInvalHandle : kMmNoError;
            d->volume = a[1];
            const float g = ((a[1] & 0xFFFF) + (a[1] >> 16)) / (2.0f * 0xFFFF);
            d->voice->SetVolume(g);
            return kMmNoError;
        });
    GH_X86_NBED(R, "winmm.dll", "waveOutGetVolume", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            WaveDev* d = wave_dev(a[0]);
            if (a[1]) put32(a[1], d ? d->volume : 0xFFFFFFFFu);
            return kMmNoError;
        });
    GH_X86_NBED(R, "winmm.dll", "waveOutGetID", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (!wave_dev(a[0])) return kMmInvalHandle;
            if (a[1]) put32(a[1], 0);
            return kMmNoError;
        });
    GH_X86_NBED(R, "winmm.dll", "waveOutBreakLoop", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return wave_dev(a[0]) ? kMmNoError : kMmInvalHandle; });
    GH_X86_NBED(R, "winmm.dll", "waveOutMessage", 16,
        [](Machine&, const uint32_t*) -> uint64_t { return kMmNotSupported; });
}

}   // namespace native
}   // namespace ghx86

#else
namespace ghx86 { namespace native { inline void RegisterWaveBeds(Machine&) {} } }
#endif
