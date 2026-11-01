// gh_dsound.h -- DirectSound output over XAudio2.
//
// The console has no dsound.dll, and Source 1 (GMod's engine.dll) plays every
// sound through it: snd_dma's CAudioDirectSound mixes into ONE looping secondary
// buffer, writing ahead of the play cursor that GetCurrentPosition reports. So
// that is the surface served here: DirectSoundCreate/8, a primary buffer that only
// holds a format, and secondary buffers whose bytes go to an XAudio2 source voice
// in 10 ms chunks taken straight from the buffer itself. The play cursor is the
// voice's own SamplesPlayed; the write cursor is the end of what is queued, so
// DirectSound's contract (never write between the two) is exactly what keeps the
// queued chunks intact. Without a usable XAudio2 the cursor runs on the clock, so
// a game's mixer still advances, silently.
//
// Included from guest_loader.cpp after Diag. Capture stays DSERR_NODRIVER there.
#pragma once
#include <xaudio2.h>

namespace ghds {

static const HRESULT kErrInvalidParam  = E_INVALIDARG;
static const HRESULT kErrInvalidCall   = (HRESULT)0x88780032;
static const HRESULT kErrNoAggregation = CLASS_E_NOAGGREGATION;
static const HRESULT kErrUnsupported   = E_NOTIMPL;
static const HRESULT kErrBadFormat     = (HRESULT)0x88780064;

enum : DWORD {
    kCapsPrimary = 0x1, kCapsStatic = 0x2, kCapsLocSoftware = 0x8,
    kPlayLooping = 0x1,
    kStatusPlaying = 0x1, kStatusLooping = 0x4,
    kLockFromWriteCursor = 0x1, kLockEntireBuffer = 0x2,
    kSpeakerStereo = 4,
};
struct BufferDesc {          // DSBUFFERDESC (DSBUFFERDESC1 stops before guid)
    DWORD dwSize, dwFlags, dwBufferBytes, dwReserved;
    WAVEFORMATEX* lpwfxFormat;
};
struct BufferCaps { DWORD dwSize, dwFlags, dwBufferBytes, dwUnlockTransferRate, dwPlayCpuOverhead; };

// {279AFA83-...} IDirectSound, {C50A7E93-...} IDirectSound8,
// {279AFA85-...} IDirectSoundBuffer, {6825A449-...} IDirectSoundBuffer8.
static const GUID kIID_DS   = {0x279AFA83,0x4981,0x11CE,{0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60}};
static const GUID kIID_DS8  = {0xC50A7E93,0xF395,0x4834,{0x9E,0xF6,0x7F,0xA9,0x9D,0xE5,0x09,0x66}};
static const GUID kIID_DSB  = {0x279AFA85,0x4981,0x11CE,{0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60}};
static const GUID kIID_DSB8 = {0x6825A449,0x7524,0x4D82,{0x92,0x0F,0x50,0xE3,0x6A,0xB3,0xAB,0x1E}};

// The engine: one XAudio2 and its mastering voice for the process, made on the
// first DirectSoundCreate. Null when the console refuses either.
static IXAudio2* g_xa = nullptr;
static HMODULE   g_xaModule = nullptr;
static HMODULE XAudioModule()
{
    static HMODULE m = []{
        HMODULE h = LoadLibraryW(L"xaudio2_9.dll");
        HMODULE pin = nullptr;
        if (h) GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"xaudio2_9.dll", &pin);
        return h;
    }();
    return m;
}
static IXAudio2* Engine()
{
    static IXAudio2* xa = []() -> IXAudio2* {
        using Create = HRESULT (WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
        HMODULE m = XAudioModule();
        auto create = m ? (Create)GetProcAddress(m, "XAudio2Create") : nullptr;
        IXAudio2* x = nullptr;
        HRESULT hr = create ? create(&x, 0, XAUDIO2_DEFAULT_PROCESSOR) : E_NOINTERFACE;
        IXAudio2MasteringVoice* mv = nullptr;
        if (SUCCEEDED(hr))
            hr = x->CreateMasteringVoice(&mv, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0,
                                         nullptr, nullptr, AudioCategory_GameEffects);
        Diag(std::string("DSOUND engine xaudio2=") + (m ? "yes" : "NO") + " hr=" + Hex((uint32_t)hr) +
             (SUCCEEDED(hr) ? "" : " -- buffers run on the clock, silent"));
        if (FAILED(hr)){ if (x) x->Release(); return nullptr; }
        return x;
    }();
    return xa;
}

class Buffer;

// One per buffer; outlives every voice it is handed to (voices are destroyed,
// which waits for their callbacks, before the buffer is freed).
struct VoiceCallback : IXAudio2VoiceCallback {
    Buffer* b = nullptr;
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override;
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) noexcept override;
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {}
};

// IDirectSoundBuffer8, method order as dsound.h declares it.
class Buffer : public IUnknown {
public:
    Buffer(DWORD flags, const WAVEFORMATEX& f, DWORD bytes) : flags_(flags), fmt_(f)
    {
        cb_.b = this;
        fmt_.cbSize = 0;
        if (!(flags & kCapsPrimary)){
            ring_.assign(bytes, (uint8_t)(f.wBitsPerSample == 8 ? 0x80 : 0));
            chunk_ = (f.nAvgBytesPerSec / 100) / f.nBlockAlign * f.nBlockAlign;
            if (chunk_ < f.nBlockAlign) chunk_ = f.nBlockAlign;
        }
    }
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** pp) override
    {
        if (!pp) return E_POINTER;
        if (iid == IID_IUnknown || iid == kIID_DSB || iid == kIID_DSB8){ *pp = this; AddRef(); return S_OK; }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const long n = --refs_;
        if (n == 0){ DropVoice(); delete this; }
        return (ULONG)n;
    }
    // IDirectSoundBuffer
    virtual HRESULT STDMETHODCALLTYPE GetCaps(BufferCaps* c)
    {
        if (!c || c->dwSize < sizeof(BufferCaps)) return kErrInvalidParam;
        c->dwFlags = flags_ | kCapsLocSoftware;
        c->dwBufferBytes = (DWORD)ring_.size();
        c->dwUnlockTransferRate = 0; c->dwPlayCpuOverhead = 0;
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPosition(DWORD* play, DWORD* write)
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        DWORD p = 0, w = 0;
        Cursors(p, w);
        if (play) *play = p;
        if (write) *write = w;
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE GetFormat(WAVEFORMATEX* f, DWORD size, DWORD* written)
    {
        if (written) *written = sizeof(WAVEFORMATEX);
        if (!f) return written ? S_OK : kErrInvalidParam;
        if (size < sizeof(WAVEFORMATEX)) return kErrInvalidParam;
        *f = fmt_;
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE GetVolume(LONG* v){ if (!v) return kErrInvalidParam; *v = volume_; return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE GetPan(LONG* v){ if (!v) return kErrInvalidParam; *v = pan_; return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE GetFrequency(DWORD* v){ if (!v) return kErrInvalidParam; *v = freq_ ? freq_ : fmt_.nSamplesPerSec; return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE GetStatus(DWORD* s)
    {
        if (!s) return kErrInvalidParam;
        std::lock_guard<std::recursive_mutex> lk(mu_);
        DWORD p, w; Cursors(p, w);   // a clock-run one-shot ends here
        *s = playing_ ? (kStatusPlaying | (looping_ ? kStatusLooping : 0)) : 0;
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE Initialize(IUnknown*, const BufferDesc*){ return kErrInvalidCall; }   // already initialized
    virtual HRESULT STDMETHODCALLTYPE Lock(DWORD off, DWORD bytes, void** p1, DWORD* b1, void** p2, DWORD* b2, DWORD fl)
    {
        if (!p1 || !b1) return kErrInvalidParam;
        if (ring_.empty()) return kErrInvalidCall;   // the primary buffer has no memory to lend
        const DWORD n = (DWORD)ring_.size();
        if (fl & kLockEntireBuffer) bytes = n;
        if (fl & kLockFromWriteCursor){ std::lock_guard<std::recursive_mutex> lk(mu_); DWORD p; Cursors(p, off); }
        if (bytes == 0 || bytes > n || off >= n) return kErrInvalidParam;
        const DWORD first = bytes < n - off ? bytes : n - off;
        *p1 = ring_.data() + off; *b1 = first;
        if (p2) *p2 = first < bytes ? ring_.data() : nullptr;
        if (b2) *b2 = bytes - first;
        else if (first < bytes) return kErrInvalidParam;
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE Play(DWORD, DWORD, DWORD fl)
    {
        if (flags_ & kCapsPrimary) return S_OK;
        std::unique_lock<std::recursive_mutex> lk(mu_);
        if (playing_){ looping_ = (fl & kPlayLooping) != 0; return S_OK; }
        lk.unlock();
        DropVoice();                  // a finished one-shot's voice
        IXAudio2SourceVoice* v = nullptr;
        if (IXAudio2* xa = Engine()){
            HRESULT hr = xa->CreateSourceVoice(&v, &fmt_, 0, 4.0f, &cb_, nullptr, nullptr);
            if (FAILED(hr)){
                static std::atomic<int> logged{0};
                if (logged++ < 4) Diag("DSOUND CreateSourceVoice hr=" + Hex((uint32_t)hr) + " fmt=" + FmtText());
                v = nullptr;
            }
        }
        lk.lock();
        voice_ = v;
        looping_ = (fl & kPlayLooping) != 0;
        playing_ = true;
        anchorPos_ = pos_; submitPos_ = pos_; queued_ = 0; ended_ = false;
        anchorQpc_ = Now();
        if (voice_){
            ApplyVolume();
            if (freq_) voice_->SetFrequencyRatio((float)freq_ / fmt_.nSamplesPerSec);
            Pump();
            voice_->Start(0);
        }
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE SetCurrentPosition(DWORD pos)
    {
        if (ring_.empty()) return S_OK;
        pos = pos % (DWORD)ring_.size() / fmt_.nBlockAlign * fmt_.nBlockAlign;
        std::unique_lock<std::recursive_mutex> lk(mu_);
        const bool was = playing_, loop = looping_;
        lk.unlock();
        if (was) Stop();
        lk.lock(); pos_ = pos; lk.unlock();
        if (was) Play(0, 0, loop ? kPlayLooping : 0);
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE SetFormat(const WAVEFORMATEX* f)
    {
        if (!f) return kErrInvalidParam;
        if (!(flags_ & kCapsPrimary)) return kErrInvalidCall;
        if (f->nChannels == 0 || f->nBlockAlign == 0) return kErrBadFormat;
        fmt_ = *f; fmt_.cbSize = 0;
        Diag("DSOUND primary SetFormat " + FmtText());
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE SetVolume(LONG v)
    {
        if (v < -10000 || v > 0) return kErrInvalidParam;
        std::lock_guard<std::recursive_mutex> lk(mu_);
        volume_ = v; ApplyVolume();
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE SetPan(LONG v)
    {
        if (v < -10000 || v > 10000) return kErrInvalidParam;
        pan_ = v;   // kept, not applied: the stereo stream Source writes is already panned
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE SetFrequency(DWORD hz)
    {
        if (flags_ & kCapsPrimary) return kErrInvalidCall;
        std::lock_guard<std::recursive_mutex> lk(mu_);
        freq_ = hz;
        if (voice_) voice_->SetFrequencyRatio((float)(hz ? hz : fmt_.nSamplesPerSec) / fmt_.nSamplesPerSec);
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE Stop()
    {
        std::unique_lock<std::recursive_mutex> lk(mu_);
        if (!playing_) return S_OK;
        DWORD p, w; Cursors(p, w);
        pos_ = p; playing_ = false;
        lk.unlock();
        DropVoice();
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE Unlock(void*, DWORD, void*, DWORD){ return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE Restore(){ return S_OK; }
    // IDirectSoundBuffer8
    virtual HRESULT STDMETHODCALLTYPE SetFX(DWORD n, void*, DWORD*){ return n ? kErrUnsupported : S_OK; }
    virtual HRESULT STDMETHODCALLTYPE AcquireResources(DWORD, DWORD n, DWORD* r)
    {
        for (DWORD i = 0; r && i < n; ++i) r[i] = 0;
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE GetObjectInPath(REFGUID, DWORD, REFGUID, void** pp)
    {
        if (pp) *pp = nullptr;
        return kErrUnsupported;
    }

    // Callbacks (XAudio2's thread).
    void BufferEnded()
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        if (queued_) --queued_;
        if (playing_) Pump();
    }
    void StreamEnded()
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        if (playing_ && !looping_){ playing_ = false; pos_ = 0; ended_ = true; }
    }
    std::string FmtText() const
    {
        return std::to_string(fmt_.nSamplesPerSec) + "Hz/" + std::to_string(fmt_.wBitsPerSample) + "bit/" +
               std::to_string(fmt_.nChannels) + "ch tag=" + std::to_string(fmt_.wFormatTag);
    }

private:
    ~Buffer() = default;
    static uint64_t Now(){ LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }

    // Queue up to three chunks ahead. A one-shot ends its last chunk with
    // END_OF_STREAM and queues nothing after the buffer's end.
    void Pump()
    {
        const DWORD n = (DWORD)ring_.size();
        while (voice_ && !ended_ && queued_ < 3){
            if (!looping_ && submitPos_ == 0 && queued_ && anchorPos_ != 0) break;
            DWORD len = n - submitPos_ < chunk_ ? n - submitPos_ : chunk_;
            const bool last = !looping_ && submitPos_ + len >= n;
            XAUDIO2_BUFFER xb{};
            xb.Flags = last ? XAUDIO2_END_OF_STREAM : 0;
            xb.AudioBytes = len;
            xb.pAudioData = ring_.data() + submitPos_;
            if (FAILED(voice_->SubmitSourceBuffer(&xb, nullptr))) break;
            ++queued_;
            submitPos_ = (submitPos_ + len) % n;
            if (last){ ended_ = true; break; }
        }
    }
    // Play cursor from what the voice has played (or from the clock without one),
    // write cursor at the end of what is queued.
    void Cursors(DWORD& play, DWORD& write)
    {
        const uint64_t n = ring_.size();
        if (!playing_ || n == 0){ play = write = pos_; return; }
        uint64_t done;
        if (voice_){
            XAUDIO2_VOICE_STATE st{};
            voice_->GetState(&st, 0);
            done = st.SamplesPlayed * fmt_.nBlockAlign;
        } else {
            LARGE_INTEGER f; QueryPerformanceFrequency(&f);
            const double secs = (double)(Now() - anchorQpc_) / (double)f.QuadPart;
            const double hz = freq_ ? freq_ : fmt_.nSamplesPerSec;
            done = (uint64_t)(secs * hz) * fmt_.nBlockAlign;
            if (!looping_ && anchorPos_ + done >= n){ playing_ = false; pos_ = 0; play = write = 0; return; }
        }
        play = (DWORD)((anchorPos_ + done) % n);
        write = voice_ ? submitPos_ : (DWORD)((play + 2 * chunk_) % n);
    }
    void ApplyVolume()
    {
        if (voice_) voice_->SetVolume(volume_ <= -10000 ? 0.0f : powf(10.0f, volume_ / 2000.0f));
    }
    // DestroyVoice waits for the voice's callbacks, which take mu_: never under it.
    void DropVoice()
    {
        std::unique_lock<std::recursive_mutex> lk(mu_);
        IXAudio2SourceVoice* v = voice_;
        voice_ = nullptr; queued_ = 0;
        lk.unlock();
        if (v) v->DestroyVoice();
    }

    std::atomic<long> refs_{1};
    std::recursive_mutex mu_;
    DWORD flags_;
    WAVEFORMATEX fmt_;
    std::vector<uint8_t> ring_;
    DWORD chunk_ = 0;
    VoiceCallback cb_;
    IXAudio2SourceVoice* voice_ = nullptr;
    bool playing_ = false, looping_ = false, ended_ = false;
    DWORD pos_ = 0, anchorPos_ = 0, submitPos_ = 0, queued_ = 0;
    uint64_t anchorQpc_ = 0;
    LONG volume_ = 0, pan_ = 0;
    DWORD freq_ = 0;
};
inline void VoiceCallback::OnBufferEnd(void*) noexcept{ b->BufferEnded(); }
inline void VoiceCallback::OnStreamEnd() noexcept{ b->StreamEnded(); }

// IDirectSound8, method order as dsound.h declares it.
class Device : public IUnknown {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** pp) override
    {
        if (!pp) return E_POINTER;
        if (iid == IID_IUnknown || iid == kIID_DS || iid == kIID_DS8){ *pp = this; AddRef(); return S_OK; }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const long n = --refs_;
        if (n == 0) delete this;
        return (ULONG)n;
    }
    virtual HRESULT STDMETHODCALLTYPE CreateSoundBuffer(const BufferDesc* d, IUnknown** pp, IUnknown* outer)
    {
        if (!pp) return kErrInvalidParam;
        *pp = nullptr;
        if (outer) return kErrNoAggregation;
        if (!d || d->dwSize < 20) return kErrInvalidParam;
        if (d->dwFlags & kCapsPrimary){
            WAVEFORMATEX f{ WAVE_FORMAT_PCM, 2, 22050, 22050 * 4, 4, 16, 0 };
            *pp = new Buffer(d->dwFlags, f, 0);
            Diag("DSOUND primary buffer flags=" + Hex(d->dwFlags));
            return S_OK;
        }
        const WAVEFORMATEX* f = d->lpwfxFormat;
        if (!f || f->nChannels == 0 || f->nBlockAlign == 0 || f->nSamplesPerSec == 0) return kErrInvalidParam;
        if (d->dwBufferBytes < 4 || d->dwBufferBytes > 0x0FFFFFFF) return kErrInvalidParam;
        const DWORD bytes = d->dwBufferBytes / f->nBlockAlign * f->nBlockAlign;
        auto* b = new Buffer(d->dwFlags, *f, bytes);
        static std::atomic<int> logged{0};
        if (logged++ < 8) Diag("DSOUND buffer " + std::to_string(bytes) + " bytes flags=" + Hex(d->dwFlags) + " " + b->FmtText());
        *pp = b;
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE GetCaps(DWORD* caps)   // DSCAPS: dwSize, dwFlags, rates...
    {
        if (!caps || caps[0] < 96) return kErrInvalidParam;
        const DWORD size = caps[0];
        memset(caps, 0, size);
        caps[0] = size;
        caps[1] = 0x2 | 0x8 | 0x10 | 0x100 | 0x200 | 0x400 | 0x800;   // stereo/16-bit, continuous rate, mono+stereo 8/16 secondaries
        caps[2] = 100; caps[3] = 200000;
        caps[4] = 1;
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE DuplicateSoundBuffer(IUnknown*, IUnknown** pp){ if (pp) *pp = nullptr; return kErrUnsupported; }
    virtual HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND, DWORD){ return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE Compact(){ return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE GetSpeakerConfig(DWORD* c){ if (!c) return kErrInvalidParam; *c = kSpeakerStereo; return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE SetSpeakerConfig(DWORD){ return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE Initialize(const GUID*){ return kErrInvalidCall; }   // DirectSoundCreate already did
    virtual HRESULT STDMETHODCALLTYPE VerifyCertification(DWORD* c){ if (!c) return kErrInvalidParam; *c = 0; return S_OK; }
private:
    ~Device() = default;
    std::atomic<long> refs_{1};
};

static HRESULT WINAPI Create(const GUID*, IUnknown** pp, IUnknown* outer)
{
    if (!pp) return kErrInvalidParam;
    *pp = nullptr;
    if (outer) return kErrNoAggregation;
    static bool once = false;
    if (!once){ once = true; Engine(); }
    *pp = new Device();
    return S_OK;
}

}  // namespace ghds

// Pinned like d3d9on12: the alias hands the handle out without a reference, and
// the guest's FreeLibrary must not unload the voices it is still playing.
static HMODULE GhDsoundModule(){ return ghds::XAudioModule(); }
