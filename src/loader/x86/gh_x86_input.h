// gh_x86_input.h -- the pad a 32-bit guest can see: XInput, and a DirectInput
// that has nothing attached.
//
// HL2's joysticks come from SDL2, which asks for SDL_INIT_GAMECONTROLLER and
// SDL_INIT_HAPTIC in one call. The haptic half creates an IDirectInput8 with
// CoCreateInstance and fails the WHOLE call when that fails ("Haptic error
// CoCreateInstance"), so the game never polls XInput at all. Two pieces:
//
//   * xinput1_4 / 1_3 / 9_1_0: the x64 beds already serve the console pad
//     (with remapped buttons masked, so a keyboard-and-mouse profile is not
//     seen twice). Every XInput struct has the same layout on both bitnesses,
//     so these are straight forwards;
//   * IDirectInput8 that enumerates no devices. DirectInput's own answer on a
//     machine with no legacy controllers; callers fall through to XInput.
//
// SDL also prefers its raw-input joystick driver over XInput whenever that
// driver starts, and it starts because hid.dll answers -- with no devices
// (see the HID beds). The guest environment sets SDL_JOYSTICK_RAWINPUT=0 for
// that reason; XInput is the only path with a pad behind it.

#pragma once

#include "gh_x86_com.h"

namespace ghx86 {

#ifdef GH_LAUNCHER

constexpr GUID kCLSID_DirectInput8 = {0x25E609E4, 0xB259, 0x11CF, {0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
constexpr GUID kIID_DI8A           = {0xBF798030, 0x483A, 0x4DA2, {0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00}};
constexpr GUID kIID_DI8W           = {0xBF798031, 0x483A, 0x4DA2, {0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00}};
constexpr uint32_t kDiErrDeviceNotReg = 0x80040154u;   // DIERR_DEVICENOTREG

// The host side of the guest's IDirectInput8: only an identity and a refcount.
// Every method is answered from the slot table below.
struct Di8Null final : IUnknown {
    LONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override
    {
        if (!o) return E_POINTER;
        if (r == IID_IUnknown || r == kIID_DI8A || r == kIID_DI8W) { *o = this; AddRef(); return S_OK; }
        *o = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG n = InterlockedDecrement(&refs);
        if (n == 0) delete this;
        return (ULONG)n;
    }
};

inline int& di8_id_a() { static int v = -1; return v; }
inline int& di8_id_w() { static int v = -1; return v; }

inline void di8_method(ComState& c, int id, const char* iface, const char* mname,
                       uint32_t bytes, uint64_t (*fn)(Machine&, const uint32_t*))
{
    HostFn f;
    f.dll  = c.intern(iface);
    f.name = c.intern(std::string(iface) + "::" + mname);
    f.stack_bytes = bytes;
    f.invoke = fn;
    c.add_method(id, mname, f, nullptr);
}

// Every out-pointer is zeroed before the failure that leaves it unset.
inline int di8_register(ComState& c, const char* name, const GUID* iid)
{
    const int id = c.add(name, iid);
    di8_method(c, id, name, "QueryInterface", 12, &com_qi);
    di8_method(c, id, name, "AddRef", 4, &com_addref);
    di8_method(c, id, name, "Release", 4, &com_release);
    di8_method(c, id, name, "CreateDevice", 16,
        [](Machine&, const uint32_t* a) -> uint64_t { if (a[2]) p32(a[2], 0, 0); return kDiErrDeviceNotReg; });
    di8_method(c, id, name, "EnumDevices", 20,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });            // nothing attached
    di8_method(c, id, name, "GetDeviceStatus", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return kDiErrDeviceNotReg; });
    di8_method(c, id, name, "RunControlPanel", 12,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    di8_method(c, id, name, "Initialize", 12,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    di8_method(c, id, name, "FindDevice", 16,
        [](Machine&, const uint32_t*) -> uint64_t { return kDiErrDeviceNotReg; });
    di8_method(c, id, name, "EnumDevicesBySemantics", 24,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    di8_method(c, id, name, "ConfigureDevices", 20,
        [](Machine&, const uint32_t*) -> uint64_t { return 0x80004001u; });  // E_NOTIMPL
    return id;
}

inline void di8_bringup(Machine& m)
{
    static bool done = false;
    if (done) return;
    done = true;
    com_bringup(m);
    di8_id_a() = di8_register(com(), "IDirectInput8A", &kIID_DI8A);
    di8_id_w() = di8_register(com(), "IDirectInput8W", &kIID_DI8W);
}

// Creates the null IDirectInput8 for `iid` into guest slot `out`.
inline uint32_t di8_create(Machine& m, const GUID* iid, uint32_t out)
{
    if (out) p32(out, 0, 0);
    if (!out || !iid) return 0x80070057u;                                    // E_INVALIDARG
    di8_bringup(m);
    const bool wide = (*iid == kIID_DI8W);
    if (!wide && *iid != kIID_DI8A) return 0x80004002u;                      // E_NOINTERFACE
    p32(out, 0, com().wrap(m, new Di8Null, wide ? di8_id_w() : di8_id_a()));
    log(std::string("X86 DirectInput8 (null) created ") + (wide ? "W" : "A"));
    return 0;
}

// CoCreateInstance(clsid, outer, ctx, iid, out): true when this file owns clsid.
inline bool di8_cocreate(Machine& m, const uint32_t* a, uint64_t& hr)
{
    const GUID* clsid = (const GUID*)(uintptr_t)a[0];
    if (!clsid || *clsid != kCLSID_DirectInput8) return false;
    hr = di8_create(m, (const GUID*)(uintptr_t)a[3], a[4]);
    return true;
}

inline void RegisterInputBeds(ThunkRegistry& R)
{
    // DirectInput8Create(hinst, version, iid, out, outer)
    GH_X86_BED(R, "dinput8.dll", "DirectInput8Create", 20,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return di8_create(m, (const GUID*)(uintptr_t)a[2], a[3]);
        });

    for (const char* dll : { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" }) {
        GH_X86_BED(R, dll, "XInputGetState", 8,
            [](Machine&, const uint32_t* a) -> uint64_t {
                return Bed_XInputGetState(a[0], (XINPUT_STATE*)(uintptr_t)a[1]);
            });
        GH_X86_BED(R, dll, "XInputGetCapabilities", 12,
            [](Machine&, const uint32_t* a) -> uint64_t {
                return Bed_XInputGetCapabilities(a[0], a[1], (XINPUT_CAPABILITIES*)(uintptr_t)a[2]);
            });
        GH_X86_BED(R, dll, "XInputSetState", 8,
            [](Machine&, const uint32_t* a) -> uint64_t {
                return Bed_XInputSetState(a[0], (XINPUT_VIBRATION*)(uintptr_t)a[1]);
            });
        GH_X86_BED(R, dll, "XInputEnable", 4,
            [](Machine&, const uint32_t* a) -> uint64_t { Bed_XInputEnable(a[0]); return 0; });
        // Wired, full: the console pad is never on a battery the guest can see.
        GH_X86_BED(R, dll, "XInputGetBatteryInformation", 12,
            [](Machine&, const uint32_t* a) -> uint64_t {
                if (a[0] != 0) return ERROR_DEVICE_NOT_CONNECTED;
                if (a[2]) { ((uint8_t*)(uintptr_t)a[2])[0] = 0x01; ((uint8_t*)(uintptr_t)a[2])[1] = 0x03; }
                return ERROR_SUCCESS;
            });
        GH_X86_BED(R, dll, "XInputGetKeystroke", 12,
            [](Machine&, const uint32_t*) -> uint64_t { return ERROR_EMPTY; });
        GH_X86_BED(R, dll, "XInputGetDSoundAudioDeviceGuids", 12,
            [](Machine&, const uint32_t*) -> uint64_t { return ERROR_DEVICE_NOT_CONNECTED; });
        GH_X86_BED(R, dll, "XInputGetAudioDeviceIds", 20,
            [](Machine&, const uint32_t*) -> uint64_t { return ERROR_DEVICE_NOT_CONNECTED; });
    }
}

#else
inline bool di8_cocreate(Machine&, const uint32_t*, uint64_t&) { return false; }
inline void RegisterInputBeds(ThunkRegistry&) {}
#endif

}   // namespace ghx86
