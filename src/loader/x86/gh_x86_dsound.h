// gh_x86_dsound.h -- the DirectSound surface a 32-bit guest sees.
//
// HL2 plays every sound through dsound.dll: snd_dma's CAudioDirectSound mixes
// into one looping secondary buffer. The console has no dsound.dll, and the
// 64-bit path already answers with gh_dsound.h's device over XAudio2 (GMod runs
// on it). This file puts a 32-bit guest on that SAME device. Its interfaces are
// two short vtables, written out here rather than generated: gh_dsound.h's
// classes are not dsound.h's, and there are only 36 slots.
//
// Three methods cannot pass straight through:
//
//   * CreateSoundBuffer takes a DSBUFFERDESC, which holds a WAVEFORMATEX
//     pointer: 36 bytes and the pointer at 16 on x86, 40 and 16 on x64;
//   * Lock hands back pointers into the buffer's host memory, which is above
//     4GB as often as not. The guest gets one block of its own heap per buffer,
//     filled at Lock and written back at Unlock, as D3D9 locks are;
//   * Release frees that block with the buffer's last reference.
//
// Whether the device is real or "no sound card" is still the title's gh_dsound
// knob: Bed_DirectSoundOutCreate decides, for both bitnesses. Capture stays
// DSERR_NODRIVER, as it does on x64.

#pragma once

#include <mutex>

#include "gh_x86_com.h"

namespace ghx86 {

#ifdef GH_LAUNCHER

constexpr uint32_t kDsErrNoDriver = 0x88780078u;
constexpr uint32_t kDsErrInvalidParam = 0x80070057u;

// One guest block per buffer that has been locked, kept across locks: Source
// locks its mixing buffer every frame.
struct DsBounce {
    uint32_t block = 0, cap = 0;   // guest heap
    uint8_t* h1 = nullptr; uint32_t b1 = 0;
    uint8_t* h2 = nullptr; uint32_t b2 = 0;
    bool     locked = false;
};
inline std::mutex& ds_mu() { static std::mutex m; return m; }
inline std::map<void*, DsBounce>& ds_bounces() { static std::map<void*, DsBounce> v; return v; }

inline int& ds_id_device()  { static int v = -1; return v; }
inline int& ds_id_device8() { static int v = -1; return v; }
inline int& ds_id_buffer()  { static int v = -1; return v; }
inline int& ds_id_buffer8() { static int v = -1; return v; }

inline ghds::Device* ds_dev(uint32_t g) { return (ghds::Device*)com().unwrap(g); }
inline ghds::Buffer* ds_buf(uint32_t g) { return (ghds::Buffer*)com().unwrap(g); }

inline void ds_method(ComState& c, int id, const char* iface, const char* mname,
                      uint32_t bytes, uint64_t (*fn)(Machine&, const uint32_t*))
{
    HostFn f;
    f.dll  = c.intern(iface);
    f.name = c.intern(std::string(iface) + "::" + mname);
    f.stack_bytes = bytes;
    f.invoke = fn;
    c.add_method(id, mname, f, nullptr);
}

inline void ds_iunknown(ComState& c, int id, const char* iface,
                        uint64_t (*release)(Machine&, const uint32_t*))
{
    ds_method(c, id, iface, "QueryInterface", 12, &com_qi);
    ds_method(c, id, iface, "AddRef", 4, &com_addref);
    ds_method(c, id, iface, "Release", 4, release);
}

#define GH_DS_DEV(EXPR)                                                   \
    [](Machine& m, const uint32_t* a) -> uint64_t {                       \
        (void)m; ghds::Device* d = ds_dev(a[0]);                          \
        if (!d) return 0x80004003u;                                       \
        return (uint64_t)(uint32_t)(EXPR);                                \
    }
#define GH_DS_BUF(EXPR)                                                   \
    [](Machine& m, const uint32_t* a) -> uint64_t {                       \
        (void)m; ghds::Buffer* b = ds_buf(a[0]);                          \
        if (!b) return 0x80004003u;                                       \
        return (uint64_t)(uint32_t)(EXPR);                                \
    }
#define GH_DS_PTR(T, I) ((T)(uintptr_t)a[I])

// IDirectSound::CreateSoundBuffer(const DSBUFFERDESC*, IDirectSoundBuffer**, IUnknown*)
inline uint64_t ds_create_buffer(Machine& m, const uint32_t* a)
{
    ghds::Device* d = ds_dev(a[0]);
    if (!d) return 0x80004003u;
    if (a[2]) p32(a[2], 0, 0);
    if (!a[1] || !a[2]) return kDsErrInvalidParam;
    ghds::BufferDesc h{};
    h.dwSize        = g32(a[1], 0);
    h.dwFlags       = g32(a[1], 4);
    h.dwBufferBytes = g32(a[1], 8);
    h.dwReserved    = g32(a[1], 12);
    h.lpwfxFormat   = (WAVEFORMATEX*)(uintptr_t)g32(a[1], 16);   // WAVEFORMATEX is the same on both sides
    IUnknown* out = nullptr;
    const HRESULT hr = d->CreateSoundBuffer(&h, &out, a[3] ? (IUnknown*)1 : nullptr);
    if (SUCCEEDED(hr) && out) p32(a[2], 0, com().wrap(m, out, ds_id_buffer()));
    return (uint64_t)(uint32_t)hr;
}

// IDirectSoundBuffer::Lock(Offset, Bytes, void** p1, DWORD* b1, void** p2, DWORD* b2, Flags)
inline uint64_t ds_lock(Machine& m, const uint32_t* a)
{
    ghds::Buffer* b = ds_buf(a[0]);
    if (!b) return 0x80004003u;
    if (a[3]) p32(a[3], 0, 0);
    if (a[5]) p32(a[5], 0, 0);
    if (!a[3] || !a[4]) return kDsErrInvalidParam;
    void* p1 = nullptr; void* p2 = nullptr; DWORD n1 = 0, n2 = 0;
    const HRESULT hr = b->Lock(a[1], a[2], &p1, &n1, a[5] ? &p2 : nullptr, a[6] ? &n2 : nullptr, a[7]);
    if (FAILED(hr)) return (uint64_t)(uint32_t)hr;

    std::lock_guard<std::mutex> lk(ds_mu());
    DsBounce& s = ds_bounces()[b];
    const uint32_t need = n1 + n2;
    if (need > s.cap) {
        if (s.block) m.heap_free(s.block);
        s.block = m.heap_alloc(need, false);
        s.cap = s.block ? need : 0;
        if (!s.block) { b->Unlock(p1, n1, p2, n2); return 0x8007000Eu; }   // E_OUTOFMEMORY
    }
    s.h1 = (uint8_t*)p1; s.b1 = n1; s.h2 = (uint8_t*)p2; s.b2 = n2; s.locked = true;
    std::memcpy((void*)(uintptr_t)s.block, p1, n1);
    if (n2) std::memcpy((void*)(uintptr_t)(s.block + n1), p2, n2);
    p32(a[3], 0, s.block);
    p32(a[4], 0, n1);
    if (a[5]) p32(a[5], 0, n2 ? s.block + n1 : 0);
    if (a[6]) p32(a[6], 0, n2);
    return 0;
}

// IDirectSoundBuffer::Unlock(p1, b1, p2, b2): the guest's bytes go back where
// Lock found them. b1/b2 are what the guest wrote, never more than it was lent.
inline uint64_t ds_unlock(Machine&, const uint32_t* a)
{
    ghds::Buffer* b = ds_buf(a[0]);
    if (!b) return 0x80004003u;
    std::lock_guard<std::mutex> lk(ds_mu());
    auto it = ds_bounces().find(b);
    if (it == ds_bounces().end() || !it->second.locked) return b->Unlock(nullptr, 0, nullptr, 0);
    DsBounce& s = it->second;
    const uint32_t w1 = (std::min)(a[2], s.b1), w2 = (std::min)(a[4], s.b2);
    if (s.h1 && w1) std::memcpy(s.h1, (const void*)(uintptr_t)s.block, w1);
    if (s.h2 && w2) std::memcpy(s.h2, (const void*)(uintptr_t)(s.block + s.b1), w2);
    s.locked = false;
    return (uint64_t)(uint32_t)b->Unlock(s.h1, w1, s.h2, w2);
}

inline uint64_t ds_buffer_release(Machine& m, const uint32_t* a)
{
    ghds::Buffer* b = ds_buf(a[0]);
    if (!b) return 0;
    const ULONG n = b->Release();
    if (n == 0) {
        com().forget(a[0]);
        std::lock_guard<std::mutex> lk(ds_mu());
        auto it = ds_bounces().find(b);
        if (it != ds_bounces().end()) {
            if (it->second.block) m.heap_free(it->second.block);
            ds_bounces().erase(it);
        }
    }
    return n;
}

inline int ds_register_device(ComState& c, const char* name, const GUID* iid)
{
    const int id = c.add(name, iid);
    ds_iunknown(c, id, name, &com_release);
    ds_method(c, id, name, "CreateSoundBuffer", 16, &ds_create_buffer);
    ds_method(c, id, name, "GetCaps", 8, GH_DS_DEV(d->GetCaps(GH_DS_PTR(DWORD*, 1))));
    ds_method(c, id, name, "DuplicateSoundBuffer", 12, GH_DS_DEV(
        ((a[2] ? p32(a[2], 0, 0) : void()), (HRESULT)0x80004001)));   // E_NOTIMPL, as on x64
    ds_method(c, id, name, "SetCooperativeLevel", 12, GH_DS_DEV(d->SetCooperativeLevel(nullptr, a[2])));
    ds_method(c, id, name, "Compact", 4, GH_DS_DEV(d->Compact()));
    ds_method(c, id, name, "GetSpeakerConfig", 8, GH_DS_DEV(d->GetSpeakerConfig(GH_DS_PTR(DWORD*, 1))));
    ds_method(c, id, name, "SetSpeakerConfig", 8, GH_DS_DEV(d->SetSpeakerConfig(a[1])));
    ds_method(c, id, name, "Initialize", 8, GH_DS_DEV(d->Initialize(GH_DS_PTR(const GUID*, 1))));
    ds_method(c, id, name, "VerifyCertification", 8, GH_DS_DEV(d->VerifyCertification(GH_DS_PTR(DWORD*, 1))));
    return id;
}

inline int ds_register_buffer(ComState& c, const char* name, const GUID* iid)
{
    const int id = c.add(name, iid);
    ds_iunknown(c, id, name, &ds_buffer_release);
    ds_method(c, id, name, "GetCaps", 8, GH_DS_BUF(b->GetCaps(GH_DS_PTR(ghds::BufferCaps*, 1))));
    ds_method(c, id, name, "GetCurrentPosition", 12, GH_DS_BUF(
        b->GetCurrentPosition(GH_DS_PTR(DWORD*, 1), GH_DS_PTR(DWORD*, 2))));
    ds_method(c, id, name, "GetFormat", 16, GH_DS_BUF(
        b->GetFormat(GH_DS_PTR(WAVEFORMATEX*, 1), a[2], GH_DS_PTR(DWORD*, 3))));
    ds_method(c, id, name, "GetVolume", 8, GH_DS_BUF(b->GetVolume(GH_DS_PTR(LONG*, 1))));
    ds_method(c, id, name, "GetPan", 8, GH_DS_BUF(b->GetPan(GH_DS_PTR(LONG*, 1))));
    ds_method(c, id, name, "GetFrequency", 8, GH_DS_BUF(b->GetFrequency(GH_DS_PTR(DWORD*, 1))));
    ds_method(c, id, name, "GetStatus", 8, GH_DS_BUF(b->GetStatus(GH_DS_PTR(DWORD*, 1))));
    ds_method(c, id, name, "Initialize", 12, GH_DS_BUF(b->Initialize(nullptr, nullptr)));
    ds_method(c, id, name, "Lock", 32, &ds_lock);
    ds_method(c, id, name, "Play", 16, GH_DS_BUF(b->Play(a[1], a[2], a[3])));
    ds_method(c, id, name, "SetCurrentPosition", 8, GH_DS_BUF(b->SetCurrentPosition(a[1])));
    ds_method(c, id, name, "SetFormat", 8, GH_DS_BUF(b->SetFormat(GH_DS_PTR(const WAVEFORMATEX*, 1))));
    ds_method(c, id, name, "SetVolume", 8, GH_DS_BUF(b->SetVolume((LONG)a[1])));
    ds_method(c, id, name, "SetPan", 8, GH_DS_BUF(b->SetPan((LONG)a[1])));
    ds_method(c, id, name, "SetFrequency", 8, GH_DS_BUF(b->SetFrequency(a[1])));
    ds_method(c, id, name, "Stop", 4, GH_DS_BUF(b->Stop()));
    ds_method(c, id, name, "Unlock", 20, &ds_unlock);
    ds_method(c, id, name, "Restore", 4, GH_DS_BUF(b->Restore()));
    // DSEFFECTDESC holds a DWORD_PTR; no effects are offered, so only "none" succeeds.
    ds_method(c, id, name, "SetFX", 16, GH_DS_BUF(a[1] ? (HRESULT)0x80004001 : S_OK));
    ds_method(c, id, name, "AcquireResources", 16, GH_DS_BUF(
        b->AcquireResources(a[1], a[2], GH_DS_PTR(DWORD*, 3))));
    ds_method(c, id, name, "GetObjectInPath", 20, GH_DS_BUF(
        ((a[4] ? p32(a[4], 0, 0) : void()), (HRESULT)0x80004001)));
    return id;
}

#undef GH_DS_DEV
#undef GH_DS_BUF
#undef GH_DS_PTR

// Registered once, the first time the guest creates a device. QueryInterface
// finds the 8 interfaces by IID; the host objects answer both.
inline void ds_bringup(Machine& m)
{
    static bool done = false;
    if (done) return;
    done = true;
    com_bringup(m);
    ComState& c = com();
    ds_id_device()  = ds_register_device(c, "IDirectSound", &ghds::kIID_DS);
    ds_id_device8() = ds_register_device(c, "IDirectSound8", &ghds::kIID_DS8);
    ds_id_buffer()  = ds_register_buffer(c, "IDirectSoundBuffer", &ghds::kIID_DSB);
    ds_id_buffer8() = ds_register_buffer(c, "IDirectSoundBuffer8", &ghds::kIID_DSB8);
}

// DirectSoundCreate(8)(const GUID*, IDirectSound(8)**, IUnknown*)
inline uint64_t ds_create(Machine& m, const uint32_t* a, bool eight)
{
    ds_bringup(m);
    if (a[1]) p32(a[1], 0, 0);
    if (!a[1]) return kDsErrInvalidParam;
    void* out = nullptr;
    const HRESULT hr = Bed_DirectSoundOutCreate((const GUID*)(uintptr_t)a[0], &out,
                                                a[2] ? (void*)(uintptr_t)a[2] : nullptr);
    log(std::string("X86 DirectSoundCreate") + (eight ? "8" : "") + " -> " + hex((uint32_t)hr));
    if (SUCCEEDED(hr) && out)
        p32(a[1], 0, com().wrap(m, out, eight ? ds_id_device8() : ds_id_device()));
    return (uint64_t)(uint32_t)hr;
}

// DirectSoundEnumerateA/W(callback, context): one device, the default, as
// "Primary Sound Driver" with no GUID -- what dsound itself reports first.
inline uint64_t ds_enumerate(Machine& m, const uint32_t* a, bool wide)
{
    if (!a[0]) return kDsErrInvalidParam;
    static uint32_t desc_a = 0, desc_w = 0, empty = 0;
    static const char kName[] = "Primary Sound Driver";
    if (!empty) {
        empty = m.heap_alloc(4, true);
        desc_a = m.heap_alloc(sizeof(kName), true);
        desc_w = m.heap_alloc(sizeof(kName) * 2, true);
        if (!empty || !desc_a || !desc_w) return 0;
        std::memcpy((void*)(uintptr_t)desc_a, kName, sizeof(kName));
        for (size_t i = 0; i < sizeof(kName); ++i)
            ((uint16_t*)(uintptr_t)desc_w)[i] = (uint16_t)(uint8_t)kName[i];
    }
    const uint32_t args[4] = { 0, wide ? desc_w : desc_a, empty, a[1] };
    m.call_guest32(a[0], args, 4);
    return 0;
}

inline void RegisterDsoundBeds(ThunkRegistry& R)
{
    GH_X86_BED(R, "dsound.dll", "DirectSoundCreate", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t { return ds_create(m, a, false); });
    GH_X86_BED(R, "dsound.dll", "DirectSoundCreate8", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t { return ds_create(m, a, true); });
    GH_X86_BED(R, "dsound.dll", "DirectSoundEnumerateA", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t { return ds_enumerate(m, a, false); });
    GH_X86_BED(R, "dsound.dll", "DirectSoundEnumerateW", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t { return ds_enumerate(m, a, true); });
    for (const char* n : { "DirectSoundCaptureCreate", "DirectSoundCaptureCreate8" })
        GH_X86_BED(R, "dsound.dll", n, 12,
            [](Machine&, const uint32_t* a) -> uint64_t {
                if (a[1]) p32(a[1], 0, 0);
                return kDsErrNoDriver;
            });
    GH_X86_BED(R, "dsound.dll", "DirectSoundFullDuplexCreate", 40,
        [](Machine&, const uint32_t* a) -> uint64_t {
            for (int k : { 7, 8, 9 }) if (a[k]) p32(a[k], 0, 0);
            return kDsErrNoDriver;
        });
    for (const char* n : { "DirectSoundCaptureEnumerateA", "DirectSoundCaptureEnumerateW" })
        GH_X86_BED(R, "dsound.dll", n, 8, [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    // GetDeviceID(src, dest): every alias names the one device, which has no GUID.
    GH_X86_BED(R, "dsound.dll", "GetDeviceID", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (!a[1]) return kDsErrInvalidParam;
            std::memset((void*)(uintptr_t)a[1], 0, 16);
            return 0;
        });
    GH_X86_BED(R, "dsound.dll", "DllCanUnloadNow", 0,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });   // S_FALSE
    GH_X86_BED(R, "dsound.dll", "DllGetClassObject", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[2]) p32(a[2], 0, 0);
            return 0x80040111u;   // CLASS_E_CLASSNOTAVAILABLE
        });
}

#else
inline void RegisterDsoundBeds(ThunkRegistry&) {}
#endif

}   // namespace ghx86
