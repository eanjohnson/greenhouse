// gh_x86_beds.h -- the Win32 surface a 32-bit guest sees.
//
// Three kinds of entry, and which kind a function gets is a real decision:
//
//   FORWARD   The host's x64 export already has the right semantics for a 32-bit
//             caller. The marshaller derives the stack layout from the real C
//             signature, so the callee-cleanup size cannot drift.
//
//   BED       Anything that (a) returns a pointer -- which must come from guest
//             memory, since the guest can only hold 32 bits -- (b) takes a
//             callback into guest code, (c) has a 32/64-bit struct layout
//             difference, or (d) needs Greenhouse's own behaviour (the VFS, the
//             synthetic command line, stdout capture).
//
//   MISSING   Left unregistered on purpose. Landing on one reports
//             "kernel32.dll!Foo" by name rather than jumping to zero, which is
//             how the surface gets discovered: run the guest, read the log, add
//             what it actually asked for.
//
// Handles are safe to pass straight through: Win32 kernel handles are defined to
// fit in 32 bits. Pointers are not, which is why every allocator is a BED.

#pragma once

// winsock2 must precede windows.h or the old winsock.h leaks in and conflicts.
// These headers exist purely so the bulk FORWARD table can see real
// declarations: every name the compiler can resolve here becomes a forward with
// a correct, type-derived stack layout instead of an unimplemented import.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <shlwapi.h>
#include <objbase.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <setupapi.h>
#include <imm.h>
#include <shlobj.h>

#include "gh_x86_win.h"
// The COM boundary. Included here rather than beside the run loop so that the
// D3D entry points register alongside every other bed -- and so that they
// register BEFORE the generated forward table, whose dynamically-resolved
// d3d11.dll entries would otherwise shadow them under first-registration-wins.
#include "gh_x86_com_d3d.h"
#include "gh_x86_com_d3d9.h"
#include "gh_x86_gdi.h"
#include "gh_x86_dsound.h"
#include "gh_x86_ddraw.h"
#include "gh_x86_input.h"
#include "gh_x86_wndq.h"

namespace ghx86 {

// ---------------------------------------------------------------------------
// A new guest session
// ---------------------------------------------------------------------------
// Everything above that outlives a Machine and holds guest addresses, emptied
// before the next guest runs (see x86_session() in gh_x86_win.h). Called by
// both runners while no guest code is live. The HOST objects the last guest
// never released -- its D3D9 device, its DirectSound buffers -- are left as
// they are: that guest's process is gone, and releasing on its behalf is the
// refcounting it never got to finish. Only our pointers to them are dropped.
inline void x86_begin_session()
{
    ++x86_session();
    com() = ComState{};                 // wrappers, vtables, thunks: all guest
    com_live_d3d9() = nullptr;          // the device the LAST guest's SDL made
    d9_bounces().clear();               // guest heap blocks
    map_bounces().clear();
#ifdef GH_LAUNCHER                         // dsound and dinput8 exist only there
    ds_id_device() = ds_id_device8() = ds_id_buffer() = ds_id_buffer8() = -1;
    di8_id_a() = di8_id_w() = -1;
#endif
    dd_mode() = DdMode{};
    {
        WndQ& q = wndq();
        std::lock_guard<std::mutex> lk(q.mu);
        q.timers.clear();               // guest HWNDs and TIMERPROCs, and
        q.notes.clear();                // thread ids Windows reuses
        q.broadcast = 0;                // a guest DEV_BROADCAST block
    }
}

// ---------------------------------------------------------------------------
// SECURITY_ATTRIBUTES: 12 bytes on x86, 24 on x64
// ---------------------------------------------------------------------------
//   x86: { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; }
//        offsets 0, 4, 8
//   x64: same fields at offsets 0, 8, 16 (pointer alignment)
//
// So the host reads lpSecurityDescriptor from offset 8, which on the guest's
// struct is bInheritHandle. A guest passing the ordinary {12, NULL, TRUE} hands
// the host a security descriptor at address 1, and the call fails with
// ERROR_NOACCESS -- which is how every mono assembly open failed while the same
// path opened fine from the host a line later.
//
// Done as an Arg32 specialisation rather than one bed at a time so that all
// eleven forwards taking one (CreateEvent, CreateMutex, CreateSemaphore,
// CreateDirectory, CreateFileMapping, CreateWaitableTimer, ...) are corrected
// together; a bed-by-bed fix leaves the next one to be discovered the same
// expensive way. Storage is thread-local because Arg32::get returns by value
// and the pointer must outlive it: no Win32 entry point takes two.
inline SECURITY_ATTRIBUTES* sa32_translate(uint32_t g)
{
    if (!g) return nullptr;
    thread_local SECURITY_ATTRIBUTES host{};
    uint32_t f[3] = {};
    std::memcpy(f, (void*)(uintptr_t)g, sizeof(f));
    host.nLength              = sizeof(host);
    host.lpSecurityDescriptor = (LPVOID)(uintptr_t)f[1];
    host.bInheritHandle       = (BOOL)f[2];
    return &host;
}

template <>
struct Arg32<LPSECURITY_ATTRIBUTES> {
    static constexpr bool ptr_sized_int = false;
    static constexpr int  slots = 1;
    static LPSECURITY_ATTRIBUTES get(const uint32_t* p) { return sa32_translate(p[0]); }
};

// ---------------------------------------------------------------------------
// File mappings
// ---------------------------------------------------------------------------
// Synthetic handles are tagged so CloseHandle can tell them from real ones. A
// real kernel handle is a small multiple of four well below this, so the tag
// cannot collide in practice.
// Event / mutex / semaphore construction, shared by the A and W beds. Free
// functions rather than local lambdas because a bed's invoke slot is a plain
// function pointer, which a capturing lambda cannot become.
inline uint64_t make_event32(Machine& m, uint32_t manual, uint32_t initial,
                             const std::wstring& name)
{
    if (SyncObj* have = m.sync_by_name(name)) {
        SetLastError(ERROR_ALREADY_EXISTS);
        return have->handle;
    }
    m.syncs.push_back(SyncObj{});
    SyncObj& o = m.syncs.back();
    o.kind         = SyncObj::Kind::Event;
    o.handle       = Machine::kSyncTag | (++m.next_sync & 0xFFFFu);
    o.manual_reset = manual != 0;
    o.signalled    = initial != 0;
    o.name         = name;
    return o.handle;
}

inline uint64_t make_mutex32(Machine& m, uint32_t owned, const std::wstring& name)
{
    if (SyncObj* have = m.sync_by_name(name)) {
        SetLastError(ERROR_ALREADY_EXISTS);
        return have->handle;
    }
    m.syncs.push_back(SyncObj{});
    SyncObj& o = m.syncs.back();
    o.kind   = SyncObj::Kind::Mutex;
    o.handle = Machine::kSyncTag | (++m.next_sync & 0xFFFFu);
    o.name   = name;
    if (owned) { o.owner = m.cur_tid(); o.recursion = 1; }
    return o.handle;
}

inline uint64_t make_sem32(Machine& m, int32_t init, int32_t max, const std::wstring& name)
{
    if (SyncObj* have = m.sync_by_name(name)) {
        SetLastError(ERROR_ALREADY_EXISTS);
        return have->handle;
    }
    m.syncs.push_back(SyncObj{});
    SyncObj& o = m.syncs.back();
    o.kind      = SyncObj::Kind::Semaphore;
    o.handle    = Machine::kSyncTag | (++m.next_sync & 0xFFFFu);
    o.count     = init;
    o.max_count = max;
    o.name      = name;
    return o.handle;
}

// A critical section, owned by us rather than by the kernel.
//
// A host CRITICAL_SECTION cannot work here: with every guest thread on one host
// thread, a contended EnterCriticalSection would block the host thread, and the
// owner -- also on that thread -- could never run to release it. Guaranteed
// deadlock on the first contention.
//
// The guest's own CRITICAL_SECTION is opaque to it (we store a pointer to this
// in its first four bytes, as the host-backed version already did), so the
// layout is ours to choose.
struct GuestCrit {
    uint32_t owner;       // owning tid, 0 when free
    int32_t  recursion;
};

inline void crit_enter(Machine& m, uint32_t p)
{
    GuestCrit* c = (GuestCrit*)(uintptr_t)p;
    const uint32_t me = m.cur_tid();

    // Held by somebody else: park and let the call be re-executed. It cannot
    // spin here -- the owner is a guest thread on this same host thread, so a
    // spin inside the bed would be the deadlock it is trying to avoid.
    //
    // Handing the section to block_for_retry is what keeps the PARKING from
    // becoming a spin of its own. The scheduler can read `owner` directly, so
    // it leaves this thread Blocked until the lock is genuinely free instead
    // of promoting it every pass to re-discover that it is not.
    if (c->owner && c->owner != me) { m.block_for_retry(p); return; }

    // Recursive acquisition by the owner never blocks -- that is the whole
    // difference between a critical section and a mutex.
    m.cur_thread().wait_crit = 0;
    c->owner = me;
    c->recursion++;
}

inline uint32_t crit_try_enter(Machine& m, uint32_t p)
{
    GuestCrit* c = (GuestCrit*)(uintptr_t)p;
    const uint32_t me = m.cur_tid();
    if (c->owner && c->owner != me) return 0;
    c->owner = me;
    c->recursion++;
    return 1;
}

inline void crit_leave(Machine& m, uint32_t p)
{
    (void)m;
    GuestCrit* c = (GuestCrit*)(uintptr_t)p;
    if (--c->recursion <= 0) { c->recursion = 0; c->owner = 0; }
}

inline constexpr uint32_t kMappingTag = 0x4D4D0000u;   // 'MM'

inline BedState::Mapping* mapping_of(Machine& m, uint32_t h)
{
    if ((h & 0xFFFF0000u) != kMappingTag) return nullptr;
    for (auto& mp : m.bed.mappings)
        if (mp.handle == h) return &mp;
    return nullptr;
}

inline void release_mapping(Machine& m, BedState::Mapping& mp)
{
    if (mp.file) { CloseHandle(mp.file); mp.file = nullptr; }
    for (size_t i = 0; i < m.bed.mappings.size(); ++i)
        if (m.bed.mappings[i].handle == mp.handle) {
            m.bed.mappings.erase(m.bed.mappings.begin() + (ptrdiff_t)i);
            return;
        }
}

// Copies a NUL-terminated string into guest memory. Used by every bed that
// returns a string, because a host pointer would not fit.
inline uint32_t guest_strdup_a(Machine& m, const char* s)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t p = m.heap_alloc(n, false);
    if (p) std::memcpy((void*)(uintptr_t)p, s, n);
    return p;
}
inline uint32_t guest_strdup_w(Machine& m, const wchar_t* s)
{
    uint32_t n = (uint32_t)(wcslen(s) + 1) * 2;
    uint32_t p = m.heap_alloc(n, false);
    if (p) std::memcpy((void*)(uintptr_t)p, s, n);
    return p;
}

// A guest can only hold 32 bits, so a real HMODULE (a host base address well
// above 4GB) can never be handed back. DLLs that Greenhouse implements as beds
// get a synthetic, guest-representable handle instead. DLLs that the game ships
// are mapped for real and use their own base as the handle -- see
// Machine::load_guest_dll.
inline uint32_t gh_synth_module(Machine& m, const std::string& want);

inline std::wstring widen(const char* s)
{
    std::wstring w;
    for (; s && *s; ++s) w += (wchar_t)(unsigned char)*s;
    return w;
}

inline std::string lower(std::string s)
{
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// The single entry point every LoadLibrary variant funnels through. A DLL that
// exists on disk beside the game is genuinely mapped; anything else -- the OS
// DLLs, the api-ms-win-* contract names -- gets a synthetic handle, because for
// those the implementation is a bed and there is nothing to map.
inline uint32_t gh_load_library(Machine& m, const std::wstring& want)
{
    std::string nm = Machine::base_name_lower(want);
    if (Machine::GuestDll* have = m.dll_by_name(nm)) { have->refs++; return have->base; }
    for (auto& mod : m.bed.modules) if (mod.name == nm) return mod.handle;

    std::wstring path = m.resolve_dll_path(want);
    if (!path.empty()) {
        if (uint32_t b = m.load_guest_dll(path)) return b;
        return 0;                              // it IS there and it did not load
    }
    // Only an OS DLL gets a synthetic handle: one we have beds for, an API-set
    // contract, or one the host itself ships. Anything else is a game plugin
    // that is not installed, and NULL is the answer the game is written for --
    // HL2's launcher probes for sourcevr.dll, which current builds no longer
    // ship, and a synthetic handle sent it straight into GetProcAddress(...)()
    // on a NULL CreateInterface.
    if (!m.registry.has_dll(nm) && nm.rfind("api-ms-", 0) != 0 && nm.rfind("ext-ms-", 0) != 0) {
        wchar_t sys[MAX_PATH];
        const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        const std::wstring host = (n && n < MAX_PATH) ? std::wstring(sys, n) + L"\\" + widen(nm.c_str())
                                                      : std::wstring();
        if (host.empty() || GetFileAttributesW(host.c_str()) == INVALID_FILE_ATTRIBUTES) {
            log_capped("ll:" + nm, "X86 LoadLibrary(" + nm + ") -> 0 (not installed, no beds)");
            SetLastError(ERROR_MOD_NOT_FOUND);
            return 0;
        }
    }
    return gh_synth_module(m, nm);
}

inline uint32_t gh_synth_module(Machine& m, const std::string& want)
{
    BedState& b = m.bed;
    for (auto& mod : b.modules) if (mod.name == want) return mod.handle;
    if (!b.next_module_handle) b.next_module_handle = 0x60000000u;
    GuestModule mod{want, b.next_module_handle};
    b.next_module_handle += 0x10000;
    b.modules.push_back(mod);
    log("X86 LoadLibrary(" + want + ") -> synthetic " + hex(mod.handle));
    return mod.handle;
}

// ---------------------------------------------------------------------------
// Helpers shared by more than one bed.
//
// Free functions rather than lambdas inside RegisterStdBeds, because a bed body
// is stored as a plain function pointer -- a capturing lambda will not convert,
// and the error it produces points at the macro rather than at the capture.
// ---------------------------------------------------------------------------

// STARTUPINFO is 68 bytes on x86 and 104 on x64.
inline void fill_startupinfo32(uint32_t p)
{
    if (!p) return;
    uint8_t* s = (uint8_t*)(uintptr_t)p;
    std::memset(s, 0, 68);
    uint32_t cb = 68;
    std::memcpy(s + 0, &cb, 4);                  // cb
    uint16_t show = SW_SHOWDEFAULT;
    std::memcpy(s + 48, &show, 2);               // wShowWindow
    // dwFlags, cbReserved2 and lpReserved2 stay zero: no inherited handle
    // table, which is what makes the CRT's lowio init open fresh ones.
}

// How many processors the guest is told it has, and it is ONE.
//
// This is not a conservative fudge, it is the truth about this execution model:
// Greenhouse runs every guest thread as a cooperative green thread on a SINGLE
// host thread, so exactly one guest instruction stream is live at a time.
//
// Reporting the host's real count (the console has eight) is a lie with teeth.
// Unity sizes its spin-wait limits from this number, and a spin-wait on a
// cooperative scheduler is not merely wasteful -- it is a deadlock generator.
// The spinning thread OWNS the single execution resource, so the producer it is
// waiting for physically cannot run until the spinner's 200,000-instruction
// slice expires, and ThreadedStreamBuffer's wait loop (exe+0x5E6180) resets its
// spin counter every time its progress callback returns true, so it can spin
// without ever reaching the blocking path that would hand the turn over.
//
// Measured, not assumed: the win11 differential trace (toolchain/vmdiff) has
// Stick Fight's t5 doing WaitForSingleObjectEx(INFINITE, alertable) at exactly
// the point our run skips it, and real Windows makes 307 WaitForSingleObject
// calls during startup against our 11, 156 ResetEvent against our 2, and zero
// TryEnterCriticalSection against our 827. See #119.
//
// One processor makes the guest choose the block-immediately paths, and
// blocking is the operation this scheduler models WELL: it is a yield.
inline constexpr uint32_t kGuestProcessorCount = 1;
inline constexpr uint32_t kGuestAffinityMask   = 1;

// SYSTEM_INFO is 36 bytes on x86 and 48 on x64, and two of its fields are
// addresses the host cannot answer honestly -- its application address range
// does not fit in 32 bits. The guest is told about its own address space, which
// for the guest is the truthful answer.
inline void fill_sysinfo32(uint32_t p)
{
    if (!p) return;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uint8_t* d = (uint8_t*)(uintptr_t)p;
    std::memset(d, 0, 36);
    uint16_t arch = PROCESSOR_ARCHITECTURE_INTEL;    // as seen by a 32-bit caller
    std::memcpy(d + 0,  &arch, 2);
    std::memcpy(d + 4,  &si.dwPageSize, 4);
    uint32_t lo = 0x00010000u, hi = 0xFFFE0000u;
    std::memcpy(d + 8,  &lo, 4);                     // lpMinimumApplicationAddress
    std::memcpy(d + 12, &hi, 4);                     // lpMaximumApplicationAddress
    // Not the host's mask or count -- see kGuestProcessorCount above.
    uint32_t mask = kGuestAffinityMask, ncpu = kGuestProcessorCount;
    std::memcpy(d + 16, &mask, 4);
    std::memcpy(d + 20, &ncpu, 4);
    uint32_t ptype = PROCESSOR_INTEL_PENTIUM;
    std::memcpy(d + 24, &ptype, 4);
    std::memcpy(d + 28, &si.dwAllocationGranularity, 4);
    std::memcpy(d + 32, &si.wProcessorLevel, 2);
    std::memcpy(d + 34, &si.wProcessorRevision, 2);
}

// Which path a module handle names. Unity derives its whole _Data directory
// from GetModuleFileName, so a wrong answer here is a game that cannot find its
// own assets.
inline std::wstring module_path32(Machine& m, uint32_t hmod)
{
    if (hmod == 0 || hmod == m.image_base) return m.image_path;
    if (Machine::GuestDll* d = m.dll_by_base(hmod)) return d->path;
    for (auto& mod : m.bed.modules)
        if (mod.handle == hmod) return std::wstring(mod.name.begin(), mod.name.end());
    return m.image_path;
}

// Formats a wsprintf-style call whose variadic arguments are sitting on the
// GUEST stack. The host's own vsnprintf cannot be used for this: it wants a
// va_list, and there is no portable way to synthesise one from a foreign stack
// -- and even if there were, every pointer argument in it would be 32-bit.
// Walking the format string and pulling one dword per conversion is both simpler
// and exactly right, because on x86 every wsprintf argument is dword-sized.
//
// Only the conversions Win32's wsprintf actually documents are handled (it is
// not printf: no floating point, no %n).
inline std::wstring gh_format32(const std::wstring& fmt, const uint32_t* va)
{
    std::wstring out;
    wchar_t buf[64];

    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != L'%') { out += fmt[i]; continue; }
        if (++i >= fmt.size()) break;
        if (fmt[i] == L'%') { out += L'%'; continue; }

        bool left = false;
        while (i < fmt.size() && (fmt[i] == L'-' || fmt[i] == L'0' || fmt[i] == L'#' ||
                                  fmt[i] == L'+' || fmt[i] == L' ')) {
            if (fmt[i] == L'-') left = true;
            ++i;
        }
        int width = 0;
        while (i < fmt.size() && fmt[i] >= L'0' && fmt[i] <= L'9')
            width = width * 10 + (fmt[i++] - L'0');
        int prec = -1;
        if (i < fmt.size() && fmt[i] == L'.') {
            ++i; prec = 0;
            while (i < fmt.size() && fmt[i] >= L'0' && fmt[i] <= L'9')
                prec = prec * 10 + (fmt[i++] - L'0');
        }
        bool wide_arg = false, narrow_arg = false;
        while (i < fmt.size() && (fmt[i] == L'l' || fmt[i] == L'h' || fmt[i] == L'w' ||
                                  fmt[i] == L'I' || fmt[i] == L'3' || fmt[i] == L'2' ||
                                  fmt[i] == L'6' || fmt[i] == L'4')) {
            if (fmt[i] == L'w' || fmt[i] == L'l') wide_arg = true;
            if (fmt[i] == L'h') narrow_arg = true;
            ++i;
        }
        if (i >= fmt.size()) break;

        std::wstring piece;
        uint32_t arg = *va++;
        switch (fmt[i]) {
            case L'd': case L'i':
                std::swprintf(buf, 64, L"%d", (int)arg); piece = buf; break;
            case L'u':
                std::swprintf(buf, 64, L"%u", (unsigned)arg); piece = buf; break;
            case L'x':
                std::swprintf(buf, 64, L"%x", (unsigned)arg); piece = buf; break;
            case L'X':
                std::swprintf(buf, 64, L"%X", (unsigned)arg); piece = buf; break;
            case L'p':
                std::swprintf(buf, 64, L"%08X", (unsigned)arg); piece = buf; break;
            case L'c':
                piece = std::wstring(1, (wchar_t)(arg & 0xFFFF)); break;
            case L's': case L'S': {
                // %s means "the caller's own character width" -- wide for
                // wsprintfW, narrow for wsprintfA -- and %S means the other one.
                bool as_wide = (fmt[i] == L's') ? !narrow_arg : true;
                if (fmt[i] == L'S') as_wide = !wide_arg;
                if (!arg) { piece = L"(null)"; break; }
                if (as_wide) piece = (const wchar_t*)(uintptr_t)arg;
                else { const char* s = (const char*)(uintptr_t)arg;
                       while (*s) piece += (wchar_t)(unsigned char)*s++; }
                if (prec >= 0 && (int)piece.size() > prec) piece.resize(prec);
                break;
            }
            default:
                piece = std::wstring(1, fmt[i]); --va;      // not a conversion after all
                break;
        }
        if ((int)piece.size() < width) {
            std::wstring pad((size_t)width - piece.size(), L' ');
            piece = left ? piece + pad : pad + piece;
        }
        out += piece;
    }
    return out;
}


// ---------------------------------------------------------------------------
// The guest environment
// ---------------------------------------------------------------------------
inline const std::wstring* env_find(Machine& m, const std::wstring& name)
{
    for (auto& kv : m.bed.env) if (_wcsicmp(kv.first.c_str(), name.c_str()) == 0)
        return &kv.second;
    return nullptr;
}

inline void env_set(Machine& m, const std::wstring& name, const std::wstring& value)
{
    for (auto& kv : m.bed.env)
        if (_wcsicmp(kv.first.c_str(), name.c_str()) == 0) { kv.second = value; return; }
    m.bed.env.emplace_back(name, value);
}

// Copies a value out under the GetEnvironmentVariable contract: the return is
// the length WITHOUT the terminator when it fitted, and WITH it when it did not,
// which is how a caller knows how big a buffer to allocate on the second try.
inline uint32_t env_copy_out(const std::wstring& v, uint32_t buf, uint32_t cap, bool wide)
{
    if (cap == 0 || !buf) return (uint32_t)v.size() + 1;
    if (v.size() + 1 > cap) return (uint32_t)v.size() + 1;
    if (wide) {
        std::memcpy((void*)(uintptr_t)buf, v.c_str(), (v.size() + 1) * 2);
    } else {
        char* d = (char*)(uintptr_t)buf;
        for (size_t i = 0; i < v.size(); ++i) d[i] = (char)(v[i] < 128 ? v[i] : '?');
        d[v.size()] = 0;
    }
    return (uint32_t)v.size();
}

// %VAR% substitution. An unknown name is left as written, which is what Windows
// does and what callers testing for "did this expand" rely on.
inline std::wstring env_expand(Machine& m, const std::wstring& in)
{
    std::wstring out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != L'%') { out += in[i]; continue; }
        size_t end = in.find(L'%', i + 1);
        if (end == std::wstring::npos) { out += in[i]; continue; }
        std::wstring name = in.substr(i + 1, end - i - 1);
        if (const std::wstring* v = env_find(m, name)) out += *v;
        else                                           out += in.substr(i, end - i + 1);
        i = end;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Investigative probes into mono.dll
// ---------------------------------------------------------------------------
// Stick Fight dies in mono's AOT image writer, which is only reachable with
// xdebug enabled, which mini_init only enables from the CRT's own
// getenv("MONO_XDEBUG") or from debug_options.gdb -- and the latter is only
// written after the CRT's getenv("MONO_DEBUG") returns non-NULL. Our
// GetEnvironmentVariableW says neither is set, so one of two things is true:
// the CRT's environment is not what we handed it, or its lookup is answering
// wrongly. These probes tell the two apart, and cost one deploy instead of a
// guess per deploy.
//
// The addresses are RVAs read off a disassembly of the shipped mono.dll
// (Unity 5.6.3p4). They are specific to that file and mean nothing for any
// other build, which is why they live here rather than in the interpreter.
namespace mono_probe {

constexpr uint32_t kGetenvCore    = 0x112efd;   // the _environ scan, no locks
constexpr uint32_t kGetenvHit     = 0x112f7c;   // its "matched" exit
constexpr uint32_t kMonoDebugTest = 0x0f1e28;   // mini_init: getenv("MONO_DEBUG") result
constexpr uint32_t kXdebugTest    = 0x0f1e3d;   // mini_init: getenv("MONO_XDEBUG") result
constexpr uint32_t kGdbTest       = 0x0f1e55;   // mini_init: debug_options.gdb
constexpr uint32_t kXdebugInit    = 0x0939fe;   // mono_xdebug_init(opts)
constexpr uint32_t kImgWriter     = 0x0b5f9f;   // mono_img_writer_create(fp, use_bin)
constexpr uint32_t kParseOpts     = 0x0ef227;   // mini_parse_debug_options()
constexpr uint32_t kParseOptsStr  = 0x0b12ee;   // the comma-separated option parser (opts in eax)
constexpr uint32_t kXdebugFlag    = 0x093a36;   // mono_xdebug_init: token was "gdb"
constexpr uint32_t kEnvironPtr    = 0x1f72b4;   // char** _environ
constexpr uint32_t kDebugOptions  = 0x1f6d70;   // static MonoDebugOptions
constexpr int      kGetenvBudget  = 8;

inline uint32_t dw(Machine& m, uint32_t at)
{
    bool ok = true;
    uint32_t v = m.cpu.read(at, 4, ok);
    return ok ? v : 0;
}

// [esp+4] on entry to a cdecl function.
inline uint32_t arg1(Machine& m) { return dw(m, m.cpu.gpr[REG_ESP] + 4); }

inline void on_getenv(Machine& m, const Machine::Probe& p)
{
    log("X86   getenv(\"" + m.guest_str(arg1(m)) + "\")");
    if (p.left != kGetenvBudget - 1) return;    // dump the environment once

    // The environment as the CRT actually sees it. If this does not match what
    // the beds published, the bug is in the block we hand out; if it does, the
    // bug is in the lookup.
    uint32_t env = dw(m, p.arg);
    log("X86   _environ = " + hex(env));
    for (int i = 0; env && i < 32; ++i) {
        uint32_t e = dw(m, env + 4u * (uint32_t)i);
        if (!e) { log("X86   env[" + std::to_string(i) + "] = NULL (end)"); break; }
        log("X86   env[" + std::to_string(i) + "] = " + hex(e) + " \"" + m.guest_str(e) + "\"");
    }
}

inline void on_getenv_hit(Machine& m, const Machine::Probe&)
{
    // esi points at the char** slot that matched; edi is the name length.
    log("X86   getenv MATCHED entry \"" + m.guest_str(dw(m, m.cpu.gpr[REG_ESI])) +
        "\" (name length " + std::to_string(m.cpu.gpr[REG_EDI]) + ")");
}

inline void on_eax_str(Machine& m, const Machine::Probe& p)
{
    log("X86   " + std::string(p.label) + " -> " +
        (m.cpu.gpr[REG_EAX] ? "\"" + m.guest_str(m.cpu.gpr[REG_EAX]) + "\"" : "NULL"));
}

inline void on_gdb_test(Machine& m, const Machine::Probe&)
{
    // eax = &debug_options; the gdb flag is at +0x24.
    log("X86   debug_options=" + hex(m.cpu.gpr[REG_EAX]) +
        " .gdb=" + hex(dw(m, m.cpu.gpr[REG_EAX] + 0x24)) +
        " (esi=" + hex(m.cpu.gpr[REG_ESI]) + ")");
}

inline void on_arg1_str(Machine& m, const Machine::Probe& p)
{
    log("X86   " + std::string(p.label) + " arg1=\"" + m.guest_str(arg1(m)) + "\"");
}

inline void on_img_writer(Machine& m, const Machine::Probe&)
{
    log("X86   mono_img_writer_create(fp=" + hex(arg1(m)) +
        ", use_bin_writer=" + hex(dw(m, m.cpu.gpr[REG_ESP] + 8)) + ")");
    m.dump_guest_stack("mono_img_writer_create");
}

inline void install(Machine& m, Machine::GuestDll& d)
{
    if (d.name != "mono.dll") return;
    const uint32_t b = d.base;
    m.add_probe(b + kGetenvCore,    "crt_getenv",  on_getenv, b + kEnvironPtr, kGetenvBudget);
    m.add_probe(b + kGetenvHit,     "getenv_hit",  on_getenv_hit,  0, 8);
    m.add_probe(b + kMonoDebugTest, "MONO_DEBUG",  on_eax_str,     0, 2);
    m.add_probe(b + kXdebugTest,    "MONO_XDEBUG", on_eax_str,     0, 2);
    m.add_probe(b + kGdbTest,       "gdb flag",    on_gdb_test,    0, 2);
    m.add_probe(b + kXdebugInit,    "xdebug_init", on_arg1_str,    0, 2);
    m.add_probe(b + kImgWriter,     "img_writer",  on_img_writer,  0, 2);
    m.add_probe(b + kParseOpts,     "mini_parse_debug_options", nullptr, 0, 2);
    m.add_probe(b + kParseOptsStr,  "parse_opts",  on_eax_str,     0, 4);
    m.add_probe(b + kXdebugFlag,    "xdebug gdb flag set", nullptr, 0, 2);
    log("X86 mono probes installed at base " + hex(b));
}

}   // namespace mono_probe

// ---------------------------------------------------------------------------
// The synthetic window and its message queue
// ---------------------------------------------------------------------------
// Handles the guest can hold but nothing can dereference: they sit above the
// guest's 1 GB region, so they can never be confused with a guest pointer, and
// they are tagged the way thread and sync handles already are.
constexpr uint32_t kWndTag   = 0x57480000u;   // 'WH'
constexpr uint32_t kFakeHwnd = kWndTag | 1u;
constexpr uint32_t kFakeHdc  = 0x44430001u;   // 'DC'
constexpr uint32_t kFakeMonitor = 0x4D4E0001u;   // 'MN'

// MSG is 28 bytes on x86 and 48 on x64: hwnd 0, message 4, wParam 8, lParam 12,
// time 16, pt.x 20, pt.y 24. Written field by field at the x86 offsets, because
// a forwarded PeekMessage would put twenty bytes of host MSG past the end of the
// guest's -- and a guest's MSG is a local, so what follows it is a saved
// register or a return address. That is rule 10 again, from the other side.
inline void write_msg32(uint32_t at, uint32_t message, uint32_t wparam, uint32_t lparam,
                        int32_t ptx = 0, int32_t pty = 0)
{
    if (!at) return;
    const uint32_t v[7] = { kFakeHwnd, message, wparam, lparam,
                            (uint32_t)GetTickCount(), (uint32_t)ptx, (uint32_t)pty };
    std::memcpy((void*)(uintptr_t)at, v, sizeof(v));
}

// One activation sequence, then the queue is empty forever.
//
// WM_ACTIVATEAPP(TRUE) + WM_ACTIVATE(WA_ACTIVE) + WM_SETFOCUS is what flips a
// Unity player's `gAppActive` true. Without it the player runs its loop and
// renders nothing at all, because it believes it is in the background -- a
// failure that looks like a graphics bug and is not one. WM_NCACTIVATE is
// harmless padding that some versions also read.
//
// After it, in the Launcher, the queue is the host's keyboard/mouse queue
// (KM_Push, fed by the CoreWindow handlers) -- the same one the x64 pump drains
// in FillInjectedMsg. Until s855 it was empty forever, so HL2 never saw a key:
// ESC did nothing. WM_INPUT goes through too: its lParam is one of our own
// synthetic HRAWINPUTs (0x7A00xxxx, PushRawMouse), which fits 32 bits, and the
// x86 GetRawInputData bed below hands back the 32-bit layout of that packet.
// Source reads its mouse ONLY from raw input, so without it the pad's right
// stick turned nothing in HL2.
// Input messages (WM_INPUT, keys) that reached a guest pump or window procedure, for
// "who ate it" questions: ret is the guest return address (a[-1] on a stdcall frame).
inline bool is_input_msg(uint32_t msg) { return msg == WM_INPUT || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST); }
inline void log_input_hop(const char* where, uint32_t msg, uint32_t wp, uint32_t target, uint32_t ret)
{
    // Separate budgets: a few seconds of mouse look is 48 WM_INPUTs on its own.
    static std::atomic<int> n_raw{0}, n_key{0};
    if (is_input_msg(msg) && (msg == WM_INPUT ? n_raw : n_key).fetch_add(1) < 48)
        log(std::string("X86 INPUTHOP ") + where + " msg=" + hex(msg) + " wp=" + hex(wp) +
            " to=" + hex(target) + " ret=" + hex(ret));
}

// The procedure that receives a message for `hwnd`: a SubWindow's own, else the
// game window's.
inline uint32_t wndproc_for(Machine& m, uint32_t hwnd)
{
    auto it = m.bed.sub_windows.find(hwnd);
    return it == m.bed.sub_windows.end() ? m.bed.wndproc : it->second.proc;
}

// A class name as the key RegisterClass and CreateWindowEx agree on: lowercased,
// since Win32 class names are case-insensitive.
inline std::string x86_class_key(uint32_t p, bool wide)
{
    std::string k;
    if (wide) { for (const uint16_t* w = (const uint16_t*)(uintptr_t)p; *w && k.size() < 256; ++w) k += (char)std::tolower(*w & 0x7F); }
    else      { for (const char* c = (const char*)(uintptr_t)p; *c && k.size() < 256; ++c) k += (char)std::tolower((unsigned char)*c); }
    return k;
}

// A window-property name as SetProp and GetProp agree on it. Below 0x10000 the
// argument IS the atom (the MAKEINTATOM form); above it, a string. Win32 keeps
// property names as atoms, and atoms are case-insensitive, so lowercasing is
// what matches the real behaviour.
inline std::string x86_prop_key(uint32_t p, bool wide)
{
    if (p < 0x10000) return "#" + std::to_string(p);
    return x86_class_key(p, wide);
}

inline uint64_t x86_get_prop(Machine& m, uint32_t hwnd, uint32_t name, bool wide)
{
    auto w = m.bed.win_props.find(hwnd);
    if (w == m.bed.win_props.end()) return 0;
    auto it = w->second.find(x86_prop_key(name, wide));
    return it == w->second.end() ? 0 : it->second;
}

inline uint64_t x86_set_prop(Machine& m, uint32_t hwnd, uint32_t name, uint32_t data, bool wide)
{
    m.bed.win_props[hwnd][x86_prop_key(name, wide)] = data;
    return 1;
}

inline uint64_t x86_remove_prop(Machine& m, uint32_t hwnd, uint32_t name, bool wide)
{
    auto w = m.bed.win_props.find(hwnd);
    if (w == m.bed.win_props.end()) return 0;
    auto it = w->second.find(x86_prop_key(name, wide));
    if (it == w->second.end()) return 0;
    const uint32_t v = it->second;
    w->second.erase(it);
    return v;
}

inline uint64_t x86_register_class(Machine& m, uint32_t proc, uint32_t name, bool wide, const char* who)
{
    const uint32_t atom = m.bed.next_atom++;
    m.bed.class_proc["#" + std::to_string(atom)] = proc;
    if (name >= 0x10000) m.bed.class_proc[x86_class_key(name, wide)] = proc;
    if (proc && !m.bed.main_window_made) m.bed.wndproc = proc;
    log(std::string("X86 ") + who + " " + (name >= 0x10000 ? x86_class_key(name, wide) : "#" + std::to_string(name)) +
        " WndProc " + hex(proc) + (m.bed.main_window_made ? " (recorded)" : " (captured)"));
    return atom;
}

inline uint64_t x86_create_window(Machine& m, const uint32_t* a, bool wide, const char* who)
{
    const uint32_t cls = a[1];
    auto it = m.bed.class_proc.find(cls >= 0x10000 ? x86_class_key(cls, wide) : "#" + std::to_string(cls));
    const uint32_t proc = it == m.bed.class_proc.end() ? 0 : it->second;
    const int32_t w = (int32_t)a[6], h = (int32_t)a[7];
    const bool game_sized = w == (int32_t)CW_USEDEFAULT || h == (int32_t)CW_USEDEFAULT ||
                            ((uint32_t)w >= m.bed.win_w && (uint32_t)h >= m.bed.win_h);
    const bool child = (a[3] & WS_CHILD) != 0 || a[8] == (uint32_t)-3;   // HWND_MESSAGE
    uint32_t hwnd = kFakeHwnd;
    if (child || (m.bed.main_window_made && !game_sized)) {
        hwnd = kWndTag | m.bed.next_sub_hwnd++;
        m.bed.sub_windows[hwnd].proc = proc;
    } else {
        if (proc) m.bed.wndproc = proc;
        m.bed.main_window_made = true;
    }
    log(std::string("X86 ") + who + " " + std::to_string(w) + "x" + std::to_string(h) +
        " style=" + hex(a[3]) + " proc=" + hex(proc) + " -> " +
        (hwnd == kFakeHwnd ? "game HWND " : "sub HWND ") + hex(hwnd));
    return hwnd;
}

inline uint64_t peek_message32(Machine& m, uint32_t msg_ptr, uint32_t mn, uint32_t mx, bool remove,
                               uint32_t ret = 0, uint32_t hwnd_filter = 0)
{
    static const UINT kSeq[] = { WM_ACTIVATEAPP, WM_ACTIVATE, WM_SETFOCUS, WM_NCACTIVATE };
    if (!msg_ptr || !m.bed.wndproc) return 0;
    auto admits = [&](UINT msg) { return (!mn && !mx) || (msg >= mn && msg <= mx); };
    if (m.bed.inject_idx < (int)(sizeof(kSeq) / sizeof(kSeq[0]))) {
        const UINT msg = kSeq[m.bed.inject_idx];
        if (!admits(msg)) return 0;
        // WA_ACTIVE and TRUE are both 1, but they are different things being said.
        write_msg32(msg_ptr, msg, (msg == WM_ACTIVATE) ? (uint32_t)WA_ACTIVE : 1u, 0);
        if (remove) {
            if (m.bed.inject_idx == 0) log("X86 injecting activation sequence -> guest WndProc");
            ++m.bed.inject_idx;
        }
        return 1;
    }
    // Timers and device notifications rank below input (gh_x86_wndq.h).
    auto system_msg = [&]() -> uint64_t {
        uint32_t s[4];
        if (!wndq_next(m, hwnd_filter, mn, mx, remove, s)) return 0;
        write_msg32(msg_ptr, s[1], s[2], s[3]);
        std::memcpy((void*)(uintptr_t)msg_ptr, &s[0], 4);              // MSG.hwnd
        return 1;
    };
#ifdef GH_LAUNCHER
    std::unique_lock<std::mutex> lk(g_kmMutex);
    if (g_kmQueue.empty() || !admits(g_kmQueue.front().message)) { lk.unlock(); return system_msg(); }
    const MSG q = g_kmQueue.front();
    write_msg32(msg_ptr, q.message, (uint32_t)q.wParam, (uint32_t)q.lParam, g_kmCurX, g_kmCurY);
    if (remove) {
        g_kmQueue.pop_front();
        static int drained = 0;
        if (q.message != WM_MOUSEMOVE && q.message != WM_INPUT && ++drained <= 24)
            log("X86 guest drained input msg=" + hex(q.message) + " wp=" + hex((uint32_t)q.wParam));
        log_input_hop("peek", q.message, (uint32_t)q.wParam, (mn << 16) | (mx & 0xFFFF), ret);
    }
    return 1;
#else
    return system_msg();
#endif
}

// ---------------------------------------------------------------------------
// Winsock last-error
// ---------------------------------------------------------------------------
// Every WS2_32 bed that fails goes through here, so that "what went wrong" is
// recorded per GUEST thread (see GuestThread::wsa_err) rather than in the host
// thread-local slot WSASetLastError would use -- which all green threads share.
constexpr uint64_t kSockError32   = 0xFFFFFFFFull;   // SOCKET_ERROR, and INVALID_SOCKET
constexpr uint64_t kWsaWaitFailed = 0xFFFFFFFFull;   // WSA_WAIT_FAILED

inline uint64_t wsa_fail(Machine& m, int err, uint64_t ret)
{
    m.cur_thread().wsa_err = (uint32_t)err;
    return ret;
}


// ---------------------------------------------------------------------------
// The rule-10 surface: entry points that write through a pointer to something
// pointer-sized
// ---------------------------------------------------------------------------
// Every one of these was named by the startup audit before it was ever called,
// and one of them then proved the audit right on the console. Stick Fight's CRT
// calls GetProcessAffinityMask(-1, &a, &b) with the two out-parameters in
// ADJACENT stack slots; the forward wrote eight bytes into each, so the process
// mask overwrote the system mask's slot and the system mask overwrote the four
// bytes after it. Two instructions later the guest did `mov [edx+0x7B8], eax`
// with edx zeroed by that write, and faulted 149 million instructions into
// startup with nothing pointing at the call.
//
// A "pointer-sized OUT value" is not always representable: the host may produce
// something above 4GB that a 32-bit guest simply cannot hold. Truncating it
// would be the same class of silent wrong answer, so the value is CHECKED and
// the call reported as failed instead.
inline bool put_ptr_out(uint32_t slot, uint64_t v, const char* who)
{
    if (v > 0xFFFFFFFFull) {
        log(std::string("X86 ") + who + " produced " + hex(v) +
            ", which does not fit in a 32-bit guest");
        return false;
    }
    const uint32_t g = (uint32_t)v;
    if (slot) std::memcpy((void*)(uintptr_t)slot, &g, 4);
    return true;
}

// A predefined registry root is a NEGATIVE constant widened by sign extension:
// HKEY_LOCAL_MACHINE is (HKEY)(LONG)0x80000002, i.e. 0xFFFFFFFF80000002 on the
// host. Zero-extending it hands the host 0x0000000080000002, which is not any
// key at all.
inline HKEY hkey32(uint32_t v) { return (HKEY)(intptr_t)(int32_t)v; }

// The registry a guest sees. On its own (x86host on the VM) that is the real
// advapi32; the Launcher points every entry at its virtual hive in
// guest_loader.cpp, the one x64 guests already use. The container's own HKCU
// refuses a KEY_ALL_ACCESS create, and HL2's engine treats exactly that failure
// (RegCreateKeyEx of ...\Settings) as a fatal init error. The hive's handles are
// small sentinels, so they survive the 4-byte out-param.
struct RegApi {
    decltype(&::RegOpenKeyExW)    OpenKeyExW    = &::RegOpenKeyExW;
    decltype(&::RegOpenKeyExA)    OpenKeyExA    = &::RegOpenKeyExA;
    decltype(&::RegOpenKeyA)      OpenKeyA      = &::RegOpenKeyA;
    decltype(&::RegCreateKeyW)    CreateKeyW    = &::RegCreateKeyW;
    decltype(&::RegCreateKeyExW)  CreateKeyExW  = &::RegCreateKeyExW;
    decltype(&::RegCreateKeyExA)  CreateKeyExA  = &::RegCreateKeyExA;
    decltype(&::RegCloseKey)      CloseKey      = &::RegCloseKey;
    decltype(&::RegSetValueExA)   SetValueExA   = &::RegSetValueExA;
    decltype(&::RegSetValueExW)   SetValueExW   = &::RegSetValueExW;
    decltype(&::RegQueryValueExA) QueryValueExA = &::RegQueryValueExA;
    decltype(&::RegQueryValueExW) QueryValueExW = &::RegQueryValueExW;
    decltype(&::RegDeleteValueA)  DeleteValueA  = &::RegDeleteValueA;
};
inline RegApi g_reg;

inline void RegisterPtrToPtrBeds(ThunkRegistry& R)
{
    static_assert(thunk_bytes_of(&::GetProcessAffinityMask) == 12, "GetProcessAffinityMask");
    GH_X86_BED(R, "kernel32.dll", "GetProcessAffinityMask", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            // The guest's affinity is one processor, matching what
            // GetSystemInfo tells it -- a guest that saw eight here and one
            // there would be entitled to believe either. See
            // kGuestProcessorCount.
            DWORD_PTR proc = kGuestAffinityMask, sys = kGuestAffinityMask;
            // Four bytes each, and the two slots are adjacent -- which is the
            // whole reason this bed exists.
            const bool ok = put_ptr_out(a[1], proc, "GetProcessAffinityMask") &&
                            put_ptr_out(a[2], sys,  "GetProcessAffinityMask");
            return ok ? 1u : 0u;
        });

    // lpFilePart points INTO the guest's own output buffer, so the address it
    // receives is a guest address that fits -- but the host still writes eight
    // bytes to deliver it.
    static_assert(thunk_bytes_of(&::GetFullPathNameW) == 16, "GetFullPathNameW");
    GH_X86_BED(R, "kernel32.dll", "GetFullPathNameW", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            LPWSTR part = nullptr;
            const DWORD n = GetFullPathNameW((LPCWSTR)(uintptr_t)a[0], a[1],
                                             (LPWSTR)(uintptr_t)a[2], a[3] ? &part : nullptr);
            if (a[3]) put_ptr_out(a[3], (uint64_t)(uintptr_t)part, "GetFullPathNameW");
            return n;
        });

    static_assert(thunk_bytes_of(&::GetFullPathNameA) == 16, "GetFullPathNameA");
    GH_X86_BED(R, "kernel32.dll", "GetFullPathNameA", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            LPSTR part = nullptr;
            const DWORD n = GetFullPathNameA((LPCSTR)(uintptr_t)a[0], a[1],
                                             (LPSTR)(uintptr_t)a[2], a[3] ? &part : nullptr);
            if (a[3]) put_ptr_out(a[3], (uint64_t)(uintptr_t)part, "GetFullPathNameA");
            return n;
        });

    static_assert(thunk_bytes_of(&::OpenProcessToken) == 12, "OpenProcessToken");
    GH_X86_BED(R, "advapi32.dll", "OpenProcessToken", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HANDLE tok = nullptr;
            const BOOL rc = OpenProcessToken((HANDLE)(intptr_t)(int32_t)a[0], a[1], &tok);
            if (!rc) return 0;
            return put_ptr_out(a[2], (uint64_t)(uintptr_t)tok, "OpenProcessToken") ? 1u : 0u;
        });

    // The Reg* family returns a LONG error code, where ZERO means success -- so
    // a generic "return 0 on refusal" stub would tell the guest the call worked
    // and leave it using an uninitialised key. That asymmetry is why these are
    // written out rather than stubbed.
    static_assert(thunk_bytes_of(&::RegOpenKeyExW) == 20, "RegOpenKeyExW");
    GH_X86_BED(R, "advapi32.dll", "RegOpenKeyExW", 20,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HKEY out = nullptr;
            const LONG rc = g_reg.OpenKeyExW(hkey32(a[0]), (LPCWSTR)(uintptr_t)a[1], a[2], a[3], &out);
            if (rc != ERROR_SUCCESS) return (uint32_t)rc;
            if (!put_ptr_out(a[4], (uint64_t)(uintptr_t)out, "RegOpenKeyExW")) {
                g_reg.CloseKey(out);
                return (uint32_t)ERROR_INVALID_HANDLE;
            }
            return 0;
        });

    static_assert(thunk_bytes_of(&::RegCreateKeyW) == 12, "RegCreateKeyW");
    GH_X86_BED(R, "advapi32.dll", "RegCreateKeyW", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HKEY out = nullptr;
            const LONG rc = g_reg.CreateKeyW(hkey32(a[0]), (LPCWSTR)(uintptr_t)a[1], &out);
            if (rc != ERROR_SUCCESS) return (uint32_t)rc;
            if (!put_ptr_out(a[2], (uint64_t)(uintptr_t)out, "RegCreateKeyW")) {
                g_reg.CloseKey(out);
                return (uint32_t)ERROR_INVALID_HANDLE;
            }
            return 0;
        });

    static_assert(thunk_bytes_of(&::RegCreateKeyExW) == 36, "RegCreateKeyExW");
    GH_X86_BED(R, "advapi32.dll", "RegCreateKeyExW", 36,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HKEY  out  = nullptr;
            DWORD disp = 0;
            const LONG rc = g_reg.CreateKeyExW(hkey32(a[0]), (LPCWSTR)(uintptr_t)a[1], a[2],
                                            (LPWSTR)(uintptr_t)a[3], a[4], a[5],
                                            sa32_translate(a[6]), &out, a[8] ? &disp : nullptr);
            if (rc != ERROR_SUCCESS) return (uint32_t)rc;
            if (a[8]) { const uint32_t d = disp; std::memcpy((void*)(uintptr_t)a[8], &d, 4); }
            if (!put_ptr_out(a[7], (uint64_t)(uintptr_t)out, "RegCreateKeyExW")) {
                g_reg.CloseKey(out);
                return (uint32_t)ERROR_INVALID_HANDLE;
            }
            return 0;
        });

    // The buffer VerQueryValue hands back points INSIDE the version block the
    // guest passed in, so it is a guest address by construction.
    static_assert(thunk_bytes_of(&::VerQueryValueA) == 16, "VerQueryValueA");
    GH_X86_BED(R, "version.dll", "VerQueryValueA", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            LPVOID buf = nullptr;
            UINT   len = 0;
            const BOOL rc = VerQueryValueA((LPCVOID)(uintptr_t)a[0], (LPCSTR)(uintptr_t)a[1],
                                           &buf, &len);
            if (!rc) return 0;
            if (a[3]) { const uint32_t l = len; std::memcpy((void*)(uintptr_t)a[3], &l, 4); }
            return put_ptr_out(a[2], (uint64_t)(uintptr_t)buf, "VerQueryValueA") ? 1u : 0u;
        });

    static_assert(thunk_bytes_of(&::SendMessageTimeoutA) == 28, "SendMessageTimeoutA");
    GH_X86_BED(R, "user32.dll", "SendMessageTimeoutA", 28,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            // There is no window tree here, so there is nothing to send to and
            // nobody to time out. The guest's own WndProc is the only receiver
            // that exists, and it is guest code -- reached through the same
            // call_guest32 path SendMessage uses.
            uint32_t r = 0;
            if (m.bed.wndproc) {
                uint32_t args[4] = { a[0], a[1], a[2], a[3] };
                r = m.call_guest32(m.bed.wndproc, args, 4);
            }
            if (a[6]) std::memcpy((void*)(uintptr_t)a[6], &r, 4);
            return 1;
        });

    static_assert(thunk_bytes_of(&::HeapQueryInformation) == 20, "HeapQueryInformation");
    GH_X86_BED(R, "kernel32.dll", "HeapQueryInformation", 20,
        [](Machine&, const uint32_t* a) -> uint64_t {
            // The guest heap is ours, not the host's, so there is nothing to
            // ask the host about. Reporting "no information" is both true and
            // the answer every caller of this is prepared for.
            if (a[4]) { const uint32_t z = 0; std::memcpy((void*)(uintptr_t)a[4], &z, 4); }
            SetLastError(ERROR_INVALID_PARAMETER);
            return 0;
        });

    // A DIB's bits live in host memory that a 32-bit guest cannot address, and
    // there is no GDI surface behind it here anyway. Failing is the only honest
    // answer; corrupting the guest's ppvBits slot is what it did before.
    static_assert(thunk_bytes_of(&::CreateDIBSection) == 24, "CreateDIBSection");
    GH_X86_BED(R, "gdi32.dll", "CreateDIBSection", 24,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[3]) { const uint32_t z = 0; std::memcpy((void*)(uintptr_t)a[3], &z, 4); }
            return 0;
        });

    // An app container has no event log to report to. Succeeding silently is
    // what the x64 side does for the same call.
    GH_X86_BED(R, "advapi32.dll", "ReportEventA", 36,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });

    // CryptoAPI contexts, keys and hashes are HCRYPTPROV/HCRYPTKEY/HCRYPTHASH
    // -- pointer-sized VALUES, not kernel handles, so nothing guarantees they
    // fit in 32 bits. So no host context is ever handed over. What a guest
    // acquires a context FOR is almost always CryptGenRandom -- Crypto++'s
    // AutoSeededX917RNG does it in a static initializer and throws OS_RNG_Err
    // when it cannot (HL2's filesystem_stdio, before DllMain returns) -- and
    // that needs no provider: the context is a token, and the random bytes come
    // from the system RNG. Keys and hashes still fail (below), and every caller
    // of those has a failure path.
    static_assert(thunk_bytes_of(&::CryptAcquireContextA) == 20, "CryptAcquireContextA");
    static_assert(thunk_bytes_of(&::CryptAcquireContextW) == 20, "CryptAcquireContextW");
    static_assert(thunk_bytes_of(&::CryptGenRandom) == 12, "CryptGenRandom");
    static_assert(thunk_bytes_of(&::CryptReleaseContext) == 8, "CryptReleaseContext");
    // A and W differ only in the two names they are given, which are ignored.
    static uint64_t (*const acquire)(Machine&, const uint32_t*) =
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (!a[0]) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
            const uint32_t tok = 0x43505256u;          // "VRPC": any nonzero value
            std::memcpy((void*)(uintptr_t)a[0], &tok, 4);
            return 1;
        };
    GH_X86_BED(R, "advapi32.dll", "CryptAcquireContextA", 20, acquire);
    GH_X86_BED(R, "advapi32.dll", "CryptAcquireContextW", 20, acquire);
    GH_X86_BED(R, "advapi32.dll", "CryptGenRandom", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            using Fn = LONG (WINAPI*)(void*, PUCHAR, ULONG, ULONG);
            static Fn gen = (Fn)dyn_resolve("bcrypt.dll", "BCryptGenRandom");
            if (!a[0] || (a[1] && !a[2])) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
            if (!a[1]) return 1;
            if (!gen || gen(nullptr, (PUCHAR)(uintptr_t)a[2], a[1], 2 /*SYSTEM_PREFERRED_RNG*/) != 0) {
                SetLastError(NTE_FAIL);
                return 0;
            }
            return 1;
        });
    GH_X86_BED(R, "advapi32.dll", "CryptReleaseContext", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (!a[0]) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
            return 1;
        });
    // SystemFunction036 is RtlGenRandom: BOOLEAN(PVOID buf, ULONG len), and it
    // is Chromium's only randomness source on Windows, so libcef cannot start
    // without it. windows.h does not declare it, so the 8 popbytes are written
    // out rather than taken from GH_X86_POP. It returns TRUE/FALSE and sets no
    // last error of its own.
    GH_X86_BED(R, "advapi32.dll", "SystemFunction036", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            using Fn = LONG (WINAPI*)(void*, PUCHAR, ULONG, ULONG);
            static Fn gen = (Fn)dyn_resolve("bcrypt.dll", "BCryptGenRandom");
            if (!a[1]) return 1;                      // zero bytes: trivially done
            if (!a[0] || !gen) return 0;
            return gen(nullptr, (PUCHAR)(uintptr_t)a[0], a[1],
                       2 /*BCRYPT_USE_SYSTEM_PREFERRED_RNG*/) == 0 ? 1 : 0;
        });
    static_assert(thunk_bytes_of(&::CryptCreateHash) == 20, "CryptCreateHash");
    GH_X86_BED(R, "advapi32.dll", "CryptCreateHash", 20,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[4]) { const uint32_t z = 0; std::memcpy((void*)(uintptr_t)a[4], &z, 4); }
            SetLastError(NTE_BAD_ALGID);
            return 0;
        });
    static_assert(thunk_bytes_of(&::CryptImportKey) == 24, "CryptImportKey");
    GH_X86_BED(R, "advapi32.dll", "CryptImportKey", 24,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[5]) { const uint32_t z = 0; std::memcpy((void*)(uintptr_t)a[5], &z, 4); }
            SetLastError(NTE_BAD_KEY);
            return 0;
        });

    // Rule 9 as well as rule 10: this blocks, and a blocking host call under
    // cooperative green threads stops every guest thread, not one.
    static_assert(thunk_bytes_of(&::GetQueuedCompletionStatus) == 20, "GetQueuedCompletionStatus");
    GH_X86_BED(R, "kernel32.dll", "GetQueuedCompletionStatus", 20,
        [](Machine&, const uint32_t* a) -> uint64_t {
            for (int k = 1; k <= 3; ++k)
                if (a[k]) { const uint32_t z = 0; std::memcpy((void*)(uintptr_t)a[k], &z, 4); }
            SetLastError(WAIT_TIMEOUT);
            return 0;
        });

    // waveOutOpen/waveInOpen are rule 10 (LPHWAVEOUT) and rule 7 (dwCallback can
    // be a guest function pointer) at once. Audio is its own piece of work; what
    // matters here is that they stop corrupting the guest's stack in the
    // meantime, and a game that cannot open a wave device just runs silent.
    static_assert(thunk_bytes_of(&::waveOutOpen) == 24, "waveOutOpen");
    GH_X86_BED(R, "winmm.dll", "waveOutOpen", 24,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[0]) { const uint32_t z = 0; std::memcpy((void*)(uintptr_t)a[0], &z, 4); }
            return MMSYSERR_NODRIVER;
        });
    static_assert(thunk_bytes_of(&::waveInOpen) == 24, "waveInOpen");
    GH_X86_BED(R, "winmm.dll", "waveInOpen", 24,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[0]) { const uint32_t z = 0; std::memcpy((void*)(uintptr_t)a[0], &z, 4); }
            return MMSYSERR_NODRIVER;
        });
}

// ---------------------------------------------------------------------------
// Source engine gaps (HL2, app 220)
// ---------------------------------------------------------------------------
// What HL2's 56 modules import that nothing above implements and that cannot
// simply be forwarded: the startup audit's three rules (callback, pointer to a
// pointer-sized object, host pointer returned) plus the cases the audit cannot
// see, where a STRUCT changes width (PAINTSTRUCT, TRACKMOUSEEVENT, the CPU
// topology record, the x86 SLIST_HEADER). The plain ones are forwards at the
// end of the generated table.
//
// Pop sizes come from the real declarations through decltype, never from an
// address: taking &::SetupDiGetClassDevsA would make the Launcher import
// setupapi.dll, and a DLL the console lacks fails the whole package.
#define GH_X86_POP(F) thunk_bytes_of((decltype(&::F))nullptr)

// One display mode for EnumDisplaySettings(Ex)A/W: mode 0, ENUM_CURRENT_SETTINGS
// (-1) and ENUM_REGISTRY_SETTINGS (-2); anything else ends the enumeration.
template <typename DM>
inline uint64_t display_mode32(Machine& m, uint32_t mode, uint32_t p)
{
    if (mode != 0 && mode != 0xFFFFFFFFu && mode != 0xFFFFFFFEu) return 0;
    if (DM* dm = (DM*)(uintptr_t)p) {
        dm->dmPelsWidth = m.bed.win_w; dm->dmPelsHeight = m.bed.win_h;
        dm->dmBitsPerPel = 32; dm->dmDisplayFrequency = 60;
        dm->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    }
    return 1;
}

// OVERLAPPED, as the guest lays it out: Internal, InternalHigh, Offset,
// OffsetHigh, hEvent -- 20 bytes (32 on x64).
struct Overlapped32 { uint32_t internal, internal_high, offset, offset_high, event; };
static_assert(sizeof(Overlapped32) == 20, "OVERLAPPED is 20 bytes on x86");

// Signals a guest event: a green-thread SyncObj in the interpreter, a real
// host event in native mode.
inline void signal_guest_event(Machine& m, uint32_t h)
{
    if (!h) return;
    if (SyncObj* o = m.sync_by_handle(h)) { o->signalled = true; return; }
    SetEvent((HANDLE)(intptr_t)(int32_t)h);
}

// ReadFile/WriteFile(h, buf, n, done, ovl) with a guest OVERLAPPED. The I/O is
// real overlapped I/O on the host's own struct at the guest's offset, waited
// for here, and the result written back the way a request that completed
// immediately leaves it: Internal = NTSTATUS, InternalHigh = bytes, hEvent
// signalled. GetOverlappedResult (below) then only reads that back. Source's
// VPK reader opens its archives FILE_FLAG_OVERLAPPED and reads at explicit
// offsets; reading at the file pointer instead returned the wrong bytes.
inline uint64_t overlapped_io32(Machine& m, const uint32_t* a, bool write)
{
    Overlapped32& g = *(Overlapped32*)(uintptr_t)a[4];
    thread_local HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED ov{};
    ov.Offset = g.offset; ov.OffsetHigh = g.offset_high; ov.hEvent = ev;
    ResetEvent(ev);
    const HANDLE h = (HANDLE)(intptr_t)(int32_t)a[0];
    DWORD done = 0;
    BOOL rc = write ? WriteFile(h, (LPCVOID)(uintptr_t)a[1], a[2], nullptr, &ov)
                    : ReadFile(h, (LPVOID)(uintptr_t)a[1], a[2], nullptr, &ov);
    DWORD err = rc ? 0 : GetLastError();
    if (rc || err == ERROR_IO_PENDING) {
        rc = GetOverlappedResult(h, &ov, &done, TRUE);
        err = rc ? 0 : GetLastError();
    }
    g.internal = rc ? 0u : err == ERROR_HANDLE_EOF ? 0xC0000011u   // STATUS_END_OF_FILE
                                                   : 0xC0000001u;  // STATUS_UNSUCCESSFUL
    g.internal_high = done;
    if (a[3]) std::memcpy((void*)(uintptr_t)a[3], &done, 4);
    signal_guest_event(m, g.event);
    if (!rc) SetLastError(err);
    return (uint32_t)rc;
}

inline void RegisterSourceGapBeds(ThunkRegistry& R)
{
    // The A forms of the registry beds in RegisterPtrToPtrBeds, same rules.
    static_assert(GH_X86_POP(RegOpenKeyA) == 12, "RegOpenKeyA");
    GH_X86_BED(R, "advapi32.dll", "RegOpenKeyA", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HKEY out = nullptr;
            const LONG rc = g_reg.OpenKeyA(hkey32(a[0]), (LPCSTR)(uintptr_t)a[1], &out);
            if (rc != ERROR_SUCCESS) return (uint32_t)rc;
            if (!put_ptr_out(a[2], (uint64_t)(uintptr_t)out, "RegOpenKeyA")) {
                g_reg.CloseKey(out);
                return (uint32_t)ERROR_INVALID_HANDLE;
            }
            return 0;
        });
    static_assert(GH_X86_POP(RegOpenKeyExA) == 20, "RegOpenKeyExA");
    GH_X86_BED(R, "advapi32.dll", "RegOpenKeyExA", 20,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HKEY out = nullptr;
            const LONG rc = g_reg.OpenKeyExA(hkey32(a[0]), (LPCSTR)(uintptr_t)a[1], a[2], a[3], &out);
            if (rc != ERROR_SUCCESS) return (uint32_t)rc;
            if (!put_ptr_out(a[4], (uint64_t)(uintptr_t)out, "RegOpenKeyExA")) {
                g_reg.CloseKey(out);
                return (uint32_t)ERROR_INVALID_HANDLE;
            }
            return 0;
        });
    static_assert(GH_X86_POP(RegCreateKeyExA) == 36, "RegCreateKeyExA");
    GH_X86_BED(R, "advapi32.dll", "RegCreateKeyExA", 36,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HKEY  out  = nullptr;
            DWORD disp = 0;
            const LONG rc = g_reg.CreateKeyExA(hkey32(a[0]), (LPCSTR)(uintptr_t)a[1], a[2],
                                            (LPSTR)(uintptr_t)a[3], a[4], a[5],
                                            sa32_translate(a[6]), &out, a[8] ? &disp : nullptr);
            if (rc != ERROR_SUCCESS) return (uint32_t)rc;
            if (a[8]) { const uint32_t d = disp; std::memcpy((void*)(uintptr_t)a[8], &d, 4); }
            if (!put_ptr_out(a[7], (uint64_t)(uintptr_t)out, "RegCreateKeyExA")) {
                g_reg.CloseKey(out);
                return (uint32_t)ERROR_INVALID_HANDLE;
            }
            return 0;
        });

    // The rest of the family, so a key opened in the hive is also read, written
    // and closed there (these were plain forwards to the real advapi32).
    static_assert(GH_X86_POP(RegCloseKey) == 4, "RegCloseKey");
    GH_X86_BED(R, "advapi32.dll", "RegCloseKey", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)g_reg.CloseKey(hkey32(a[0]));
        });
    static_assert(GH_X86_POP(RegSetValueExA) == 24, "RegSetValueExA");
    GH_X86_BED(R, "advapi32.dll", "RegSetValueExA", 24,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)g_reg.SetValueExA(hkey32(a[0]), (LPCSTR)(uintptr_t)a[1], a[2], a[3],
                                               (const BYTE*)(uintptr_t)a[4], a[5]);
        });
    static_assert(GH_X86_POP(RegSetValueExW) == 24, "RegSetValueExW");
    GH_X86_BED(R, "advapi32.dll", "RegSetValueExW", 24,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)g_reg.SetValueExW(hkey32(a[0]), (LPCWSTR)(uintptr_t)a[1], a[2], a[3],
                                               (const BYTE*)(uintptr_t)a[4], a[5]);
        });
    static_assert(GH_X86_POP(RegQueryValueExA) == 24, "RegQueryValueExA");
    GH_X86_BED(R, "advapi32.dll", "RegQueryValueExA", 24,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)g_reg.QueryValueExA(hkey32(a[0]), (LPCSTR)(uintptr_t)a[1],
                                                 (LPDWORD)(uintptr_t)a[2], (LPDWORD)(uintptr_t)a[3],
                                                 (LPBYTE)(uintptr_t)a[4], (LPDWORD)(uintptr_t)a[5]);
        });
    static_assert(GH_X86_POP(RegQueryValueExW) == 24, "RegQueryValueExW");
    GH_X86_BED(R, "advapi32.dll", "RegQueryValueExW", 24,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)g_reg.QueryValueExW(hkey32(a[0]), (LPCWSTR)(uintptr_t)a[1],
                                                 (LPDWORD)(uintptr_t)a[2], (LPDWORD)(uintptr_t)a[3],
                                                 (LPBYTE)(uintptr_t)a[4], (LPDWORD)(uintptr_t)a[5]);
        });
    static_assert(GH_X86_POP(RegDeleteValueA) == 8, "RegDeleteValueA");
    GH_X86_BED(R, "advapi32.dll", "RegDeleteValueA", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)g_reg.DeleteValueA(hkey32(a[0]), (LPCSTR)(uintptr_t)a[1]);
        });

    // RegNotifyChangeKeyValue(hKey, bWatchSubtree, dwFilter, hEvent, fAsync).
    // The hive is ours and lives in this process: nothing outside it can change
    // a key, so no notification can ever be due. Accepting the registration and
    // never signalling the event is therefore the true answer, not a stub --
    // Chromium (libcef, inside Half-Life's gameui) registers for changes to
    // "Internet Settings" to watch for a proxy change that will never come.
    //
    // The synchronous form is refused instead of answered: it is defined to
    // block until something changes, and here nothing ever will, so returning
    // would either hang that thread forever or send the caller round a loop
    // re-reading an unchanged key.
    static_assert(GH_X86_POP(RegNotifyChangeKeyValue) == 20, "RegNotifyChangeKeyValue");
    GH_X86_BED(R, "advapi32.dll", "RegNotifyChangeKeyValue", 20,
        [](Machine&, const uint32_t* a) -> uint64_t {
            static bool once = false;
            if (!once) {
                once = true;
                log("X86 REG RegNotifyChangeKeyValue filter=" + hex(a[2]) +
                    " subtree=" + std::to_string(a[1]) +
                    (a[4] ? " async -> ERROR_SUCCESS; the event is never signalled"
                          : " SYNCHRONOUS -> ERROR_CALL_NOT_IMPLEMENTED (it would block forever)"));
            }
            if (!a[4]) return (uint32_t)ERROR_CALL_NOT_IMPLEMENTED;
            return (uint32_t)ERROR_SUCCESS;
        });

    // Two PHANDLE outs (vstdlib's process pipes).
    static_assert(GH_X86_POP(CreatePipe) == 16, "CreatePipe");
    GH_X86_BED(R, "kernel32.dll", "CreatePipe", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HANDLE rd = nullptr, wr = nullptr;
            if (!CreatePipe(&rd, &wr, sa32_translate(a[2]), a[3])) return 0;
            if (!put_ptr_out(a[0], (uint64_t)(uintptr_t)rd, "CreatePipe") ||
                !put_ptr_out(a[1], (uint64_t)(uintptr_t)wr, "CreatePipe")) {
                CloseHandle(rd); CloseHandle(wr);
                SetLastError(ERROR_INVALID_HANDLE);
                return 0;
            }
            return 1;
        });

    // A 32-bit child could not run here anyway, and STARTUPINFOA and
    // PROCESS_INFORMATION both change width. Logged: a game that wants a child
    // process says what it wanted.
    static_assert(GH_X86_POP(CreateProcessA) == 40, "CreateProcessA");
    GH_X86_BED(R, "kernel32.dll", "CreateProcessA", 40,
        [](Machine&, const uint32_t* a) -> uint64_t {
            const char* app = (const char*)(uintptr_t)a[0];
            const char* cmd = (const char*)(uintptr_t)a[1];
            log(std::string("X86 CreateProcessA refused: ") + (app ? app : "") + " | " +
                std::string(cmd ? cmd : "").substr(0, 200));
            SetLastError(ERROR_NOT_SUPPORTED);
            return 0;
        });

    // PROCESSENTRY32 carries a ULONG_PTR (296 bytes on x86, 304 on x64). The
    // snapshot itself is a plain forward; the guest is told its walk is empty.
    static_assert(GH_X86_POP(Process32First) == 8, "Process32First");
    for (const char* n : { "Process32First", "Process32Next" })
        GH_X86_BED(R, "kernel32.dll", n, 8,
            [](Machine&, const uint32_t*) -> uint64_t {
                SetLastError(ERROR_NO_MORE_FILES);
                return 0;
            });

    // SYSTEM_LOGICAL_PROCESSOR_INFORMATION is 24 bytes on x86 (ULONG_PTR mask)
    // and 32 on x64. Synthesised rather than translated, so the topology agrees
    // with the one processor GetSystemInfo reports (kGuestProcessorCount): one
    // package holding one core.
    static_assert(GH_X86_POP(GetLogicalProcessorInformation) == 8, "GetLogicalProcessorInformation");
    GH_X86_BED(R, "kernel32.dll", "GetLogicalProcessorInformation", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            constexpr uint32_t kRec = 24, kCount = 2;
            if (!a[1]) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
            uint32_t have = 0;
            std::memcpy(&have, (void*)(uintptr_t)a[1], 4);
            const uint32_t need = kRec * kCount;
            std::memcpy((void*)(uintptr_t)a[1], &need, 4);
            if (!a[0] || have < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
            uint8_t* d = (uint8_t*)(uintptr_t)a[0];
            std::memset(d, 0, need);
            const uint32_t mask = kGuestAffinityMask;
            const uint32_t core = RelationProcessorCore, pkg = RelationProcessorPackage;
            std::memcpy(d + 0,  &mask, 4); std::memcpy(d + 4,  &core, 4);
            std::memcpy(d + 24, &mask, 4); std::memcpy(d + 28, &pkg, 4);
            return 1;
        });

    // PROCESS_HEAP_ENTRY is full of pointers, and the guest's heap handles name
    // OUR heap (gh_x86_win.h), which the host cannot walk. An empty walk.
    static_assert(GH_X86_POP(HeapWalk) == 8, "HeapWalk");
    GH_X86_BED(R, "kernel32.dll", "HeapWalk", 8,
        [](Machine&, const uint32_t*) -> uint64_t {
            SetLastError(ERROR_NO_MORE_ITEMS);
            return 0;
        });

    // CONTEXT is a different structure at each width (tier0 sets hardware
    // breakpoints with it). Refused rather than mistranslated.
    static_assert(GH_X86_POP(SetThreadContext) == 8, "SetThreadContext");
    GH_X86_BED(R, "kernel32.dll", "SetThreadContext", 8,
        [](Machine&, const uint32_t*) -> uint64_t {
            SetLastError(ERROR_NOT_SUPPORTED);
            return 0;
        });

    // The x86 SLIST_HEADER is 8 bytes -- {Next, Depth:16, Sequence:16} -- and
    // the x64 one is 16 and must be 16-aligned, so the forwards in the
    // generated table write past the guest's header. Done here on the guest's
    // own layout with one 64-bit compare-exchange, which is exactly what the
    // x86 kernel32 does (cmpxchg8b). GameUI and steam_api use these.
    static_assert(GH_X86_POP(InitializeSListHead) == 4, "InitializeSListHead");
    GH_X86_BED(R, "kernel32.dll", "InitializeSListHead", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[0]) std::memset((void*)(uintptr_t)a[0], 0, 8);
            return 0;
        });
    static_assert(GH_X86_POP(InterlockedPushEntrySList) == 8, "InterlockedPushEntrySList");
    GH_X86_BED(R, "kernel32.dll", "InterlockedPushEntrySList", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            volatile LONG64* h = (volatile LONG64*)(uintptr_t)a[0];
            for (;;) {
                const LONG64 old = *h;
                const uint32_t first = (uint32_t)old;
                std::memcpy((void*)(uintptr_t)a[1], &first, 4);
                const uint32_t hi = (uint32_t)(old >> 32);
                const uint32_t nhi = ((hi & 0xFFFF) + 1) | (((hi >> 16) + 1) << 16);   // depth+1, seq+1
                const LONG64 nv = (LONG64)(((uint64_t)nhi << 32) | a[1]);
                if (InterlockedCompareExchange64(h, nv, old) == old) return first;
            }
        });
    static_assert(GH_X86_POP(InterlockedPopEntrySList) == 4, "InterlockedPopEntrySList");
    GH_X86_BED(R, "kernel32.dll", "InterlockedPopEntrySList", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            volatile LONG64* h = (volatile LONG64*)(uintptr_t)a[0];
            for (;;) {
                const LONG64 old = *h;
                const uint32_t first = (uint32_t)old;
                if (!first) return 0;
                uint32_t next = 0;
                std::memcpy(&next, (void*)(uintptr_t)first, 4);
                const uint32_t hi = (uint32_t)(old >> 32);
                const uint32_t nhi = ((hi - 1) & 0xFFFF) | (hi & 0xFFFF0000u);         // depth-1
                const LONG64 nv = (LONG64)(((uint64_t)nhi << 32) | next);
                if (InterlockedCompareExchange64(h, nv, old) == old) return first;
            }
        });
    static_assert(GH_X86_POP(InterlockedFlushSList) == 4, "InterlockedFlushSList");
    GH_X86_BED(R, "kernel32.dll", "InterlockedFlushSList", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            volatile LONG64* h = (volatile LONG64*)(uintptr_t)a[0];
            for (;;) {
                const LONG64 old = *h;
                if (!(uint32_t)old) return 0;
                const uint32_t hi = (uint32_t)(old >> 32);
                const LONG64 nv = (LONG64)((uint64_t)(hi & 0xFFFF0000u) << 32);        // empty, seq kept
                if (InterlockedCompareExchange64(h, nv, old) == old) return (uint32_t)old;
            }
        });
    static_assert(GH_X86_POP(QueryDepthSList) == 4, "QueryDepthSList");
    GH_X86_BED(R, "kernel32.dll", "QueryDepthSList", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            uint16_t depth = 0;
            std::memcpy(&depth, (const uint8_t*)(uintptr_t)a[0] + 4, 2);
            return depth;
        });

    // Activation contexts (GameUI asking for comctl32 v6). There is no side-by-
    // side assembly store for a 32-bit guest; the create fails and the
    // activate/deactivate pair succeeds with cookie 0, which is what callers
    // that ignore the create's failure need.
    static_assert(GH_X86_POP(CreateActCtxA) == 4, "CreateActCtxA");
    GH_X86_BED(R, "kernel32.dll", "CreateActCtxA", 4,
        [](Machine&, const uint32_t*) -> uint64_t {
            SetLastError(ERROR_NOT_SUPPORTED);
            return 0xFFFFFFFFu;
        });
    static_assert(GH_X86_POP(ActivateActCtx) == 8, "ActivateActCtx");
    GH_X86_BED(R, "kernel32.dll", "ActivateActCtx", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[1]) std::memset((void*)(uintptr_t)a[1], 0, 4);
            return 1;
        });
    static_assert(GH_X86_POP(DeactivateActCtx) == 8, "DeactivateActCtx");
    GH_X86_BED(R, "kernel32.dll", "DeactivateActCtx", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });

    // A code pointer is good if the guest says so; the host cannot probe it
    // any better than the guest's own fault would.
    static_assert(GH_X86_POP(IsBadCodePtr) == 4, "IsBadCodePtr");
    GH_X86_BED(R, "kernel32.dll", "IsBadCodePtr", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });

    // -- user32 ---------------------------------------------------------------
    // CallWindowProc's first argument is a window procedure the guest got from
    // GetWindowLong(GWL_WNDPROC) -- guest code (or one of our import thunks,
    // which is guest-callable too). Entered the way dispatch enters it.
    static_assert(GH_X86_POP(CallWindowProcA) == 20, "CallWindowProcA");
    for (const char* n : { "CallWindowProcA", "CallWindowProcW" })
        GH_X86_BED(R, "user32.dll", n, 20,
            [](Machine& m, const uint32_t* a) -> uint64_t {
                if (!a[0]) return 0;
                const uint32_t args[4] = { a[1], a[2], a[3], a[4] };
                log_input_hop("callwndproc", a[2], a[3], a[0], a[-1]);
                return m.call_guest32(a[0], args, 4);
            });

    // There is no window tree (see CreateWindowExW): no child to visit.
    static_assert(GH_X86_POP(EnumChildWindows) == 12, "EnumChildWindows");
    GH_X86_BED(R, "user32.dll", "EnumChildWindows", 12,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });

    // PAINTSTRUCT: HDC first, so every later field moves (64 bytes on x86, 72
    // on x64). The paint DC is the host's; handles fit in 32 bits.
    // These three used to pass the guest's HWND to the host, which has never
    // heard of it: BeginPaint answered a NULL HDC and TrackMouseEvent FALSE.
    // The window is ours, so they are answered here instead -- the paint DC is
    // the same one GetDC hands out, and the whole window is always the paint
    // rectangle because nothing here keeps an update region.
    static_assert(GH_X86_POP(BeginPaint) == 8, "BeginPaint");
    GH_X86_BED(R, "user32.dll", "BeginPaint", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (a[1]) {
                uint8_t* d = (uint8_t*)(uintptr_t)a[1];
                std::memset(d, 0, 64);
                const uint32_t v[6] = { kFakeHdc, 0, 0, 0, m.bed.win_w, m.bed.win_h };
                std::memcpy(d, v, sizeof(v));   // hdc, fErase, then rcPaint
            }
            return kFakeHdc;
        });
    static_assert(GH_X86_POP(EndPaint) == 8, "EndPaint");
    GH_X86_BED(R, "user32.dll", "EndPaint", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });

    // TRACKMOUSEEVENT {cbSize, dwFlags, HWND, dwHoverTime}: 16 bytes on x86.
    // There is no cursor leaving or hovering over a surrogate window, so the
    // request is accepted and no WM_MOUSELEAVE is ever posted.
    static_assert(GH_X86_POP(TrackMouseEvent) == 4, "TrackMouseEvent");
    GH_X86_BED(R, "user32.dll", "TrackMouseEvent", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return a[0] ? 1 : 0; });

    // Structures with handles in them, for features with no console analogue.
    static_assert(GH_X86_POP(FlashWindowEx) == 4, "FlashWindowEx");
    GH_X86_BED(R, "user32.dll", "FlashWindowEx", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    static_assert(GH_X86_POP(GetGUIThreadInfo) == 8, "GetGUIThreadInfo");
    GH_X86_BED(R, "user32.dll", "GetGUIThreadInfo", 8,
        [](Machine&, const uint32_t*) -> uint64_t { SetLastError(ERROR_NOT_SUPPORTED); return 0; });
    static_assert(GH_X86_POP(GetIconInfo) == 8, "GetIconInfo");
    GH_X86_BED(R, "user32.dll", "GetIconInfo", 8,
        [](Machine&, const uint32_t*) -> uint64_t { SetLastError(ERROR_INVALID_CURSOR_HANDLE); return 0; });
    // WNDCLASSEXW is all pointers. "Not registered" sends SDL to RegisterClass,
    // which is the bed that knows about guest window procedures.
    static_assert(GH_X86_POP(GetClassInfoExW) == 12, "GetClassInfoExW");
    GH_X86_BED(R, "user32.dll", "GetClassInfoExW", 12,
        [](Machine&, const uint32_t*) -> uint64_t { SetLastError(ERROR_CLASS_DOES_NOT_EXIST); return 0; });
    static_assert(GH_X86_POP(GetKeyboardLayoutList) == 8, "GetKeyboardLayoutList");
    GH_X86_BED(R, "user32.dll", "GetKeyboardLayoutList", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HKL host[32];
            const int n = GetKeyboardLayoutList(32, host);
            if (!a[0] || !a[1]) return (uint32_t)n;
            const int k = n < (int)a[0] ? n : (int)a[0];
            for (int i = 0; i < k; ++i) {
                const uint32_t v = (uint32_t)(uintptr_t)host[i];
                std::memcpy((uint8_t*)(uintptr_t)a[1] + 4 * i, &v, 4);
            }
            return (uint32_t)k;
        });
    static_assert(GH_X86_POP(DialogBoxIndirectParamW) == 20, "DialogBoxIndirectParamW");
    GH_X86_BED(R, "user32.dll", "DialogBoxIndirectParamW", 20,
        [](Machine&, const uint32_t*) -> uint64_t { return 0xFFFFFFFFu; });
    static_assert(GH_X86_POP(SetWindowsHookExW) == 16, "SetWindowsHookExW");
    GH_X86_BED(R, "user32.dll", "SetWindowsHookExW", 16,
        [](Machine&, const uint32_t*) -> uint64_t { SetLastError(ERROR_ACCESS_DENIED); return 0; });

    // -- gdi32: font enumeration --------------------------------------------
    // vgui asks "does this face exist" by enumerating it. The callback is
    // guest code, so each answer is copied to guest memory (ENUMLOGFONTEXA and
    // NEWTEXTMETRICEXA hold no pointers -- same bytes at both widths) and the
    // guest entered once per face, stopping when it returns 0.
    static_assert(GH_X86_POP(EnumFontFamiliesExA) == 20, "EnumFontFamiliesExA");
    GH_X86_BED(R, "gdi32.dll", "EnumFontFamiliesExA", 20,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            struct Ctx { Machine* m; uint32_t fn, lparam, scratch, last; } c{ &m, a[2], a[3], 0, 0 };
            if (!a[1] || !a[2]) return 0;
            c.scratch = m.heap_alloc(sizeof(ENUMLOGFONTEXA) + sizeof(NEWTEXTMETRICEXA), true);
            if (!c.scratch) return 0;
            LOGFONTA lf;
            std::memcpy(&lf, (void*)(uintptr_t)a[1], sizeof(lf));
            EnumFontFamiliesExA((HDC)(uintptr_t)a[0], &lf,
                [](const LOGFONTA* f, const TEXTMETRICA* t, DWORD type, LPARAM lp) -> int {
                    Ctx& c = *(Ctx*)lp;
                    uint8_t* s = (uint8_t*)(uintptr_t)c.scratch;
                    std::memcpy(s, f, sizeof(ENUMLOGFONTEXA));
                    std::memcpy(s + sizeof(ENUMLOGFONTEXA), t, sizeof(NEWTEXTMETRICEXA));
                    const uint32_t args[4] = { c.scratch, c.scratch + (uint32_t)sizeof(ENUMLOGFONTEXA),
                                               (uint32_t)type, c.lparam };
                    c.last = c.m->call_guest32(c.fn, args, 4);
                    return (int)c.last;
                }, (LPARAM)&c, a[4]);
            m.heap_free(c.scratch);
            return c.last;
        });

    // -- user32/gdi32: the one display ---------------------------------------
    // The container has no desktop: its user32 enumerates no monitors and no
    // display devices, and answers 0 for the screen size. These are the x86
    // twins of the x64 display beds -- one primary monitor exactly the size of
    // the surrogate window (m.bed.win_w x win_h). Every struct here is pointer
    // free, so the guest's own memory is filled in place.
    static_assert(GH_X86_POP(EnumDisplayMonitors) == 16, "EnumDisplayMonitors");
    GH_X86_BED(R, "user32.dll", "EnumDisplayMonitors", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            log("X86 BED EnumDisplayMonitors -> 1 monitor " + std::to_string(m.bed.win_w) + "x" +
                std::to_string(m.bed.win_h));
            if (!a[2]) return 1;
            const uint32_t rc = m.heap_alloc(16, true);
            if (!rc) return 0;
            const int32_t v[4] = { 0, 0, (int32_t)m.bed.win_w, (int32_t)m.bed.win_h };
            std::memcpy((void*)(uintptr_t)rc, v, sizeof(v));
            const uint32_t args[4] = { kFakeMonitor, a[0], rc, a[3] };
            m.call_guest32(a[2], args, 4);
            m.heap_free(rc);
            return 1;
        });
    static_assert(GH_X86_POP(MonitorFromWindow) == 8, "MonitorFromWindow");
    static_assert(GH_X86_POP(MonitorFromRect) == 8, "MonitorFromRect");
    for (const char* n : { "MonitorFromWindow", "MonitorFromRect" })
        GH_X86_BED(R, "user32.dll", n, 8,
            [](Machine&, const uint32_t*) -> uint64_t { return kFakeMonitor; });
    static_assert(GH_X86_POP(MonitorFromPoint) == 12, "MonitorFromPoint");   // POINT by value
    GH_X86_BED(R, "user32.dll", "MonitorFromPoint", 12,
        [](Machine&, const uint32_t*) -> uint64_t { return kFakeMonitor; });
    static_assert(GH_X86_POP(GetMonitorInfoW) == 8, "GetMonitorInfoW");
    for (const char* n : { "GetMonitorInfoA", "GetMonitorInfoW" })
        GH_X86_BED(R, "user32.dll", n, 8,
            [](Machine& m, const uint32_t* a) -> uint64_t {
                MONITORINFO* mi = (MONITORINFO*)(uintptr_t)a[1];
                if (!mi || mi->cbSize < sizeof(MONITORINFO)) return 0;
                mi->rcMonitor = { 0, 0, (LONG)m.bed.win_w, (LONG)m.bed.win_h };
                mi->rcWork    = mi->rcMonitor;
                mi->dwFlags   = MONITORINFOF_PRIMARY;
                if (mi->cbSize == sizeof(MONITORINFOEXW))
                    std::memcpy(((MONITORINFOEXW*)mi)->szDevice, L"\\\\.\\DISPLAY1", sizeof(L"\\\\.\\DISPLAY1"));
                else if (mi->cbSize == sizeof(MONITORINFOEXA))
                    std::memcpy(((MONITORINFOEXA*)mi)->szDevice, "\\\\.\\DISPLAY1", sizeof("\\\\.\\DISPLAY1"));
                return 1;
            });
    // One adapter, one mode. SDL walks adapters by index and then each
    // adapter's monitors with the adapter's name; both answer index 0 only.
    static_assert(GH_X86_POP(EnumDisplayDevicesW) == 16, "EnumDisplayDevicesW");
    GH_X86_BED(R, "user32.dll", "EnumDisplayDevicesW", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            DISPLAY_DEVICEW* dd = (DISPLAY_DEVICEW*)(uintptr_t)a[2];
            if (a[1] != 0 || !dd) return 0;
            wcscpy_s(dd->DeviceName, 32, L"\\\\.\\DISPLAY1");
            wcscpy_s(dd->DeviceString, 128, L"Greenhouse Display");
            dd->StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
            return 1;
        });
    GH_X86_BED(R, "user32.dll", "EnumDisplayDevicesA", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            DISPLAY_DEVICEA* dd = (DISPLAY_DEVICEA*)(uintptr_t)a[2];
            if (a[1] != 0 || !dd) return 0;
            strcpy_s(dd->DeviceName, 32, "\\\\.\\DISPLAY1");
            strcpy_s(dd->DeviceString, 128, "Greenhouse Display");
            dd->StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
            return 1;
        });
    static_assert(GH_X86_POP(EnumDisplaySettingsW) == 12, "EnumDisplaySettingsW");
    static_assert(GH_X86_POP(EnumDisplaySettingsExW) == 16, "EnumDisplaySettingsExW");
    GH_X86_BED(R, "user32.dll", "EnumDisplaySettingsW", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t { return display_mode32<DEVMODEW>(m, a[1], a[2]); });
    GH_X86_BED(R, "user32.dll", "EnumDisplaySettingsA", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t { return display_mode32<DEVMODEA>(m, a[1], a[2]); });
    GH_X86_BED(R, "user32.dll", "EnumDisplaySettingsExW", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t { return display_mode32<DEVMODEW>(m, a[1], a[2]); });
    GH_X86_BED(R, "user32.dll", "EnumDisplaySettingsExA", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t { return display_mode32<DEVMODEA>(m, a[1], a[2]); });
    // Screen-size metrics are ours; the rest still come from the real user32.
    static_assert(GH_X86_POP(GetSystemMetrics) == 4, "GetSystemMetrics");
    GH_X86_BED(R, "user32.dll", "GetSystemMetrics", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            switch ((int)a[0]) {
                case SM_CXSCREEN: case SM_CXFULLSCREEN: case SM_CXVIRTUALSCREEN: return m.bed.win_w;
                case SM_CYSCREEN: case SM_CYFULLSCREEN: case SM_CYVIRTUALSCREEN: return m.bed.win_h;
                case SM_CMONITORS: return 1;
                case SM_XVIRTUALSCREEN: case SM_YVIRTUALSCREEN: return 0;
                default: return (uint32_t)GetSystemMetrics((int)a[0]);
            }
        });
    // Any DC the guest holds is a DC on the one display (the x64 bed's #154
    // lesson: answer the physical size too, at 96 DPI, never a silent 0).
    static_assert(GH_X86_POP(GetDeviceCaps) == 8, "GetDeviceCaps");
    GH_X86_BED(R, "gdi32.dll", "GetDeviceCaps", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            const uint32_t w = m.bed.win_w, h = m.bed.win_h;
            switch ((int)a[1]) {
                case HORZRES:   return w;
                case VERTRES:   return h;
                case HORZSIZE:  return (w * 254 + 480) / 960;
                case VERTSIZE:  return (h * 254 + 480) / 960;
                case BITSPIXEL: return 32;
                case PLANES:    return 1;
                case COLORRES:  return 24;
                case VREFRESH:  return 60;
                case LOGPIXELSX: case LOGPIXELSY: return 96;
                default: {
                    static std::atomic<uint64_t> seen{0};
                    const uint64_t bit = a[1] < 64 ? 1ull << a[1] : 0;
                    if (bit && !(seen.fetch_or(bit) & bit))
                        log("X86 BED GetDeviceCaps UNANSWERED index " + std::to_string(a[1]) + " -> 0");
                    return 0;
                }
            }
        });

    // -- callbacks a plain forward would hand the host as a 64-bit pointer ----
    // Each of these takes a guest function the real API would call in 64-bit
    // mode. None has a desktop to act on here, so each answers the way an empty
    // session would without ever calling it -- except the locale walk, which
    // has a real answer and gets it through the guest call path.
    static_assert(GH_X86_POP(SetConsoleCtrlHandler) == 8, "SetConsoleCtrlHandler");
    GH_X86_BED(R, "kernel32.dll", "SetConsoleCtrlHandler", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });   // no console, no Ctrl+C
    static_assert(GH_X86_POP(EnumSystemLocalesA) == 8, "EnumSystemLocalesA");
    GH_X86_BED(R, "kernel32.dll", "EnumSystemLocalesA", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0]) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
            const uint32_t s = guest_strdup_a(m, "00000409");   // en-US, the one we report
            if (!s) return 0;
            const uint32_t args[1] = { s };
            m.call_guest32(a[0], args, 1);
            m.heap_free(s);
            return 1;
        });
    // The completion routine would be an APC into guest code; the timer itself
    // is real and still signals, which is all a waiter needs.
    static_assert(GH_X86_POP(SetWaitableTimer) == 24, "SetWaitableTimer");
    GH_X86_BED(R, "kernel32.dll", "SetWaitableTimer", 24,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[3]) log("X86 BED SetWaitableTimer drops its completion routine");
            return SetWaitableTimer((HANDLE)(intptr_t)(int32_t)a[0],
                                    (const LARGE_INTEGER*)(uintptr_t)a[1], (LONG)a[2],
                                    nullptr, nullptr, (BOOL)a[5]);
        });
    // A window's timer posts WM_TIMER through PeekMessage (gh_x86_wndq.h). A
    // thread timer (hwnd 0) is an id that never fires: no window procedure
    // would see it, and its TIMERPROC would run on whichever thread pumped.
    static_assert(GH_X86_POP(SetTimer) == 16, "SetTimer");
    GH_X86_BED(R, "user32.dll", "SetTimer", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[0]) return wndq_set_timer(a[0], a[1], a[2], a[3]);
            static std::atomic<uint32_t> next{0x7100};
            log("X86 BED SetTimer " + std::to_string(a[2]) + " ms proc=" + std::to_string(a[3] != 0) +
                " (thread timer, never fires)");
            return a[1] ? a[1] : next.fetch_add(1);
        });
    static_assert(GH_X86_POP(KillTimer) == 8, "KillTimer");
    GH_X86_BED(R, "user32.dll", "KillTimer", 8,
        [](Machine&, const uint32_t* a) -> uint64_t { if (a[0]) wndq_kill_timer(a[0], a[1]); return 1; });
    // RegisterDeviceNotification(recipient, filter, flags). Forwarded, it failed
    // on the surrogate HWND and SDL2 gave up on finding pads after SDL_Init.
    for (const char* n : { "RegisterDeviceNotificationA", "RegisterDeviceNotificationW" })
        GH_X86_BED(R, "user32.dll", n, 12,
            [](Machine&, const uint32_t* a) -> uint64_t {
                if (!a[0] || (a[2] & 1 /*DEVICE_NOTIFY_SERVICE_HANDLE*/)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
                return wndq_register_notify(a[0]);
            });
    GH_X86_BED(R, "user32.dll", "UnregisterDeviceNotification", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return wndq_unregister_notify(a[0]) ? 1 : 0; });
    static_assert(GH_X86_POP(EnumWindows) == 8, "EnumWindows");
    GH_X86_BED(R, "user32.dll", "EnumWindows", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });   // no top-level windows
    // Dialogs: -1 / NULL is "could not create", the answer a game's error path
    // already handles (it falls back to a message box or the log).
    static_assert(GH_X86_POP(DialogBoxParamW) == 20, "DialogBoxParamW");
    for (const char* n : { "DialogBoxParamA", "DialogBoxParamW" })
        GH_X86_BED(R, "user32.dll", n, 20,
            [](Machine&, const uint32_t*) -> uint64_t { return 0xFFFFFFFFu; });
    static_assert(GH_X86_POP(CreateDialogParamW) == 20, "CreateDialogParamW");
    for (const char* n : { "CreateDialogParamA", "CreateDialogParamW" })
        GH_X86_BED(R, "user32.dll", n, 20,
            [](Machine&, const uint32_t*) -> uint64_t { return 0; });

    // The request already completed inside overlapped_io32; this only reads
    // the guest's OVERLAPPED back.
    static_assert(GH_X86_POP(GetOverlappedResult) == 16, "GetOverlappedResult");
    GH_X86_BED(R, "kernel32.dll", "GetOverlappedResult", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (!a[1]) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
            const Overlapped32& g = *(const Overlapped32*)(uintptr_t)a[1];
            if (a[2]) std::memcpy((void*)(uintptr_t)a[2], &g.internal_high, 4);
            if (g.internal == 0) return 1;
            SetLastError(g.internal == 0xC0000011u ? ERROR_HANDLE_EOF : ERROR_GEN_FAILURE);
            return 0;
        });
    static_assert(GH_X86_POP(GetOverlappedResultEx) == 20, "GetOverlappedResultEx");
    GH_X86_BED(R, "kernel32.dll", "GetOverlappedResultEx", 20,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (!a[1]) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
            const Overlapped32& g = *(const Overlapped32*)(uintptr_t)a[1];
            if (a[2]) std::memcpy((void*)(uintptr_t)a[2], &g.internal_high, 4);
            if (g.internal == 0) return 1;
            SetLastError(g.internal == 0xC0000011u ? ERROR_HANDLE_EOF : ERROR_GEN_FAILURE);
            return 0;
        });

    // -- shell32 / ole32 ----------------------------------------------------
    // SHFILEINFO and NOTIFYICONDATA hold HICON/HWND (width changes); no shell.
    static_assert(GH_X86_POP(SHGetFileInfoA) == 20, "SHGetFileInfoA");
    for (const char* n : { "SHGetFileInfoA", "SHGetFileInfoW" })
        GH_X86_BED(R, "shell32.dll", n, 20,
            [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    static_assert(GH_X86_POP(Shell_NotifyIconA) == 8, "Shell_NotifyIconA");
    GH_X86_BED(R, "shell32.dll", "Shell_NotifyIconA", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    static_assert(GH_X86_POP(ExtractIconExW) == 20, "ExtractIconExW");
    GH_X86_BED(R, "shell32.dll", "ExtractIconExW", 20,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    // The drop target is a guest COM object the host would call into. There is
    // no drag and drop on a console, so registration "succeeds" and nothing is
    // ever dropped.
    static_assert(GH_X86_POP(RegisterDragDrop) == 8, "RegisterDragDrop");
    GH_X86_BED(R, "ole32.dll", "RegisterDragDrop", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return S_OK; });
    static_assert(GH_X86_POP(RevokeDragDrop) == 4, "RevokeDragDrop");
    GH_X86_BED(R, "ole32.dll", "RevokeDragDrop", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return S_OK; });
    static_assert(GH_X86_POP(ReleaseStgMedium) == 4, "ReleaseStgMedium");
    GH_X86_BED(R, "ole32.dll", "ReleaseStgMedium", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });

    // -- rpcrt4 ---------------------------------------------------------------
    // The string comes back through a pointer the guest frees with
    // RpcStringFreeA, so it is allocated from the guest heap.
    static_assert(GH_X86_POP(UuidToStringA) == 8, "UuidToStringA");
    GH_X86_BED(R, "rpcrt4.dll", "UuidToStringA", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0] || !a[1]) return RPC_S_INVALID_ARG;
            GUID g;
            std::memcpy(&g, (void*)(uintptr_t)a[0], sizeof(g));
            const uint32_t s = m.heap_alloc(40, true);
            if (!s) return RPC_S_OUT_OF_MEMORY;
            snprintf((char*)(uintptr_t)s, 40, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
                     g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
            std::memcpy((void*)(uintptr_t)a[1], &s, 4);
            return RPC_S_OK;
        });
    static_assert(GH_X86_POP(RpcStringFreeA) == 4, "RpcStringFreeA");
    GH_X86_BED(R, "rpcrt4.dll", "RpcStringFreeA", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0]) return RPC_S_INVALID_ARG;
            uint32_t s = 0;
            std::memcpy(&s, (void*)(uintptr_t)a[0], 4);
            if (s) m.heap_free(s);
            std::memset((void*)(uintptr_t)a[0], 0, 4);
            return RPC_S_OK;
        });

    // -- winmm mixer ----------------------------------------------------------
    // MIXERLINECONTROLS and MIXERCONTROLDETAILS embed pointers, and the volume
    // mixer is not something a game on a console should drive. No mixers.
    static_assert(GH_X86_POP(mixerOpen) == 20, "mixerOpen");
    GH_X86_BED(R, "winmm.dll", "mixerGetNumDevs", 0,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "winmm.dll", "mixerOpen", 20,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[0]) std::memset((void*)(uintptr_t)a[0], 0, 4);
            return MMSYSERR_NODRIVER;
        });
    static_assert(GH_X86_POP(mixerGetDevCapsA) == 12, "mixerGetDevCapsA");
    GH_X86_BED(R, "winmm.dll", "mixerGetDevCapsA", 12,
        [](Machine&, const uint32_t*) -> uint64_t { return MMSYSERR_BADDEVICEID; });
    static_assert(GH_X86_POP(mixerClose) == 4, "mixerClose");
    GH_X86_BED(R, "winmm.dll", "mixerClose", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return MMSYSERR_INVALHANDLE; });
    static_assert(GH_X86_POP(mixerGetLineInfoA) == 12 && GH_X86_POP(mixerGetLineControlsA) == 12 &&
                  GH_X86_POP(mixerGetControlDetailsA) == 12 && GH_X86_POP(mixerSetControlDetails) == 12,
                  "mixer*");
    for (const char* n : { "mixerGetLineInfoA", "mixerGetLineControlsA",
                           "mixerGetControlDetailsA", "mixerSetControlDetails" })
        GH_X86_BED(R, "winmm.dll", n, 12,
            [](Machine&, const uint32_t*) -> uint64_t { return MMSYSERR_INVALHANDLE; });

    // -- setupapi -------------------------------------------------------------
    // SDL2, valvedeviceapi and xinput1_3 enumerate HID devices this way. Every
    // structure involved embeds pointers or ULONG_PTRs; the pad reaches the
    // guest as keys instead (builtin-wasd), so the device list is empty.
    static_assert(GH_X86_POP(SetupDiGetClassDevsA) == 16, "SetupDiGetClassDevsA");
    for (const char* n : { "SetupDiGetClassDevsA", "SetupDiGetClassDevsW" })
        GH_X86_BED(R, "setupapi.dll", n, 16,
            [](Machine&, const uint32_t*) -> uint64_t {
                SetLastError(ERROR_INVALID_DATA);
                return 0xFFFFFFFFu;                     // INVALID_HANDLE_VALUE
            });
    static_assert(GH_X86_POP(SetupDiEnumDeviceInfo) == 12, "SetupDiEnumDeviceInfo");
    GH_X86_BED(R, "setupapi.dll", "SetupDiEnumDeviceInfo", 12,
        [](Machine&, const uint32_t*) -> uint64_t { SetLastError(ERROR_NO_MORE_ITEMS); return 0; });
    static_assert(GH_X86_POP(SetupDiEnumDeviceInterfaces) == 20, "SetupDiEnumDeviceInterfaces");
    GH_X86_BED(R, "setupapi.dll", "SetupDiEnumDeviceInterfaces", 20,
        [](Machine&, const uint32_t*) -> uint64_t { SetLastError(ERROR_NO_MORE_ITEMS); return 0; });
    static_assert(GH_X86_POP(SetupDiGetDeviceInterfaceDetailA) == 24, "SetupDiGetDeviceInterfaceDetailA");
    for (const char* n : { "SetupDiGetDeviceInterfaceDetailA", "SetupDiGetDeviceInterfaceDetailW" })
        GH_X86_BED(R, "setupapi.dll", n, 24,
            [](Machine&, const uint32_t*) -> uint64_t { SetLastError(ERROR_INVALID_HANDLE); return 0; });
    static_assert(GH_X86_POP(SetupDiGetDeviceRegistryPropertyA) == 28, "SetupDiGetDeviceRegistryPropertyA");
    GH_X86_BED(R, "setupapi.dll", "SetupDiGetDeviceRegistryPropertyA", 28,
        [](Machine&, const uint32_t*) -> uint64_t { SetLastError(ERROR_INVALID_HANDLE); return 0; });
    static_assert(GH_X86_POP(SetupDiDestroyDeviceInfoList) == 4, "SetupDiDestroyDeviceInfoList");
    GH_X86_BED(R, "setupapi.dll", "SetupDiDestroyDeviceInfoList", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });

    // -- wininet --------------------------------------------------------------
    // engine.dll's update/news fetches. wininet.h cannot be included beside the
    // loader's winhttp.h, so the pop sizes are written out (5, 6, 4, 2, 1, 4
    // and 5 stdcall arguments). No network session is ever opened, which every
    // caller already handles: it is what an offline PC returns.
    GH_X86_BED(R, "wininet.dll", "InternetOpenA", 20,
        [](Machine&, const uint32_t*) -> uint64_t { SetLastError(12029); return 0; });
    GH_X86_BED(R, "wininet.dll", "InternetOpenUrlA", 24,
        [](Machine&, const uint32_t*) -> uint64_t { SetLastError(12029); return 0; });
    GH_X86_BED(R, "wininet.dll", "InternetCrackUrlA", 16,
        [](Machine&, const uint32_t*) -> uint64_t { SetLastError(12006); return 0; });
    GH_X86_BED(R, "wininet.dll", "InternetSetStatusCallback", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 0xFFFFFFFFu; });
    GH_X86_BED(R, "wininet.dll", "InternetCloseHandle", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    GH_X86_BED(R, "wininet.dll", "InternetReadFile", 16,
        [](Machine&, const uint32_t*) -> uint64_t { SetLastError(ERROR_INVALID_HANDLE); return 0; });
    GH_X86_BED(R, "wininet.dll", "HttpQueryInfoA", 20,
        [](Machine&, const uint32_t*) -> uint64_t { SetLastError(ERROR_INVALID_HANDLE); return 0; });

    // -- the Half-Life 1 gap --------------------------------------------------
    //
    // Nineteen names the HL1 binaries on the -soft path import and no bed
    // implemented; four of those (the condition variables) turned out to be
    // native-mode beds already, leaving these fifteen. They go in one batch on
    // purpose: an unresolved import is only fatal at the moment it is called,
    // so finding them one console deploy at a time costs a deploy per name.
    //
    // None of them are in the bulk-generated forward table because the UWP
    // filtering in the SDK headers does not declare them, and GH_X86_FWD_DYN
    // needs a declaration for decltype. dyn_resolve by string does not, which
    // is why several of these are hand beds over a call that is in fact present
    // on the console -- the same shape as the MapVirtualKeyExA bed below.
    //
    // Pop sizes are written out rather than static_assert'ed for the same
    // reason: the function the assert would measure is not declared here.

    // SDL2 builds its scancode table with this (WIN_UpdateKeymap), and with an
    // empty table every WM_KEYDOWN becomes SDLK_UNKNOWN -- so the pad, which
    // reaches the guest as keys, would press nothing. The console's user32 does
    // export it; only the header hides it.
    GH_X86_BED(R, "user32.dll", "MapVirtualKeyW", 8,            // (uCode, uMapType)
        [](Machine&, const uint32_t* a) -> uint64_t {
            using Fn = UINT (WINAPI*)(UINT, UINT);
            static Fn p = (Fn)dyn_resolve("user32.dll", "MapVirtualKeyW");
            static bool warned = false;
            if (!p) {
                if (!warned) { warned = true; log("X86 MapVirtualKeyW: absent, keymap will be empty"); }
                return 0;
            }
            return (uint32_t)p(a[0], a[1]);
        });

    // CharLowerW has two signatures in one: a pointer when the high word is
    // set, and a single character packed into the low word when it is not.
    // Lowering ASCII and Latin-1 in place covers every caller here (vgui's
    // text fields) and keeps the bed free of a locale the console does not have.
    GH_X86_BED(R, "user32.dll", "CharLowerW", 4,                // (lpsz)
        [](Machine&, const uint32_t* a) -> uint64_t {
            auto low = [](wchar_t c) -> wchar_t {
                if (c >= L'A' && c <= L'Z') return (wchar_t)(c - L'A' + L'a');
                if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (wchar_t)(c + 0x20);
                return c;
            };
            if (!(a[0] >> 16)) return (uint32_t)low((wchar_t)(a[0] & 0xFFFFu));   // a character, not a string
            if (!a[0]) return 0;
            for (wchar_t* w = (wchar_t*)(uintptr_t)a[0]; *w; ++w) *w = low(*w);
            return a[0];
        });

    GH_X86_BED(R, "user32.dll", "IntersectRect", 12,            // (dst, src1, src2)
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (!a[0] || !a[1] || !a[2]) return 0;
            const RECT* p = (const RECT*)(uintptr_t)a[1];
            const RECT* q = (const RECT*)(uintptr_t)a[2];
            RECT* d = (RECT*)(uintptr_t)a[0];
            d->left   = p->left   > q->left   ? p->left   : q->left;
            d->top    = p->top    > q->top    ? p->top    : q->top;
            d->right  = p->right  < q->right  ? p->right  : q->right;
            d->bottom = p->bottom < q->bottom ? p->bottom : q->bottom;
            if (d->right <= d->left || d->bottom <= d->top) { *d = RECT{}; return 0; }
            return 1;
        });

    // FoldStringW's only job for these callers is case/compatibility folding
    // ahead of a comparison. Without the real call, passing the string through
    // unchanged is the one wrong answer that cannot break a comparison the
    // guest then makes against another string folded the same way.
    GH_X86_BED(R, "kernel32.dll", "FoldStringW", 20,            // (flags, src, cchSrc, dst, cchDst)
        [](Machine&, const uint32_t* a) -> uint64_t {
            using Fn = int (WINAPI*)(DWORD, LPCWSTR, int, LPWSTR, int);
            static Fn p = (Fn)dyn_resolve("kernel32.dll", "FoldStringW");
            if (p) return (uint32_t)p(a[0], (LPCWSTR)(uintptr_t)a[1], (int)a[2],
                                      (LPWSTR)(uintptr_t)a[3], (int)a[4]);
            const wchar_t* src = (const wchar_t*)(uintptr_t)a[1];
            if (!src) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
            int n = (int)a[2];
            if (n < 0) { n = 0; while (src[n]) ++n; ++n; }       // negative: NUL-terminated, NUL included
            if (!(int)a[4]) return (uint32_t)n;                  // size query
            if ((int)a[4] < n || !a[3]) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
            std::memcpy((void*)(uintptr_t)a[3], src, (size_t)n * 2);
            return (uint32_t)n;
        });

    // Every guest address below 4GB in this process is ours, and a bed that
    // genuinely probed would have to fault to find out. A null pointer is the
    // only answer this can give honestly, and it is the one callers test for.
    GH_X86_BED(R, "kernel32.dll", "IsBadStringPtrA", 8,         // (lpsz, ucchMax)
        [](Machine&, const uint32_t* a) -> uint64_t { return a[0] ? 0 : 1; });

    // The 16-bit-era open. HL1's filesystem goes through CreateFile; this is
    // here so the import resolves, and it logs if it is ever really called so
    // the next session knows to implement it rather than guess.
    GH_X86_BED(R, "kernel32.dll", "OpenFile", 12,               // (name, ofstruct, style)
        [](Machine&, const uint32_t* a) -> uint64_t {
            log(std::string("X86 OpenFile(legacy) style=") + hex(a[2]) + " " +
                ((const char*)(uintptr_t)a[0] ? (const char*)(uintptr_t)a[0] : "(null)") +
                " -- not implemented, returning HFILE_ERROR");
            if (a[1]) {
                // OFSTRUCT is BYTE cBytes, BYTE fFixedDisk, WORD nErrCode,
                // WORD Reserved1, WORD Reserved2, CHAR szPathName[128]: so
                // nErrCode sits at offset 2 and the whole thing is 136 bytes.
                std::memset((void*)(uintptr_t)a[1], 0, 136);
                *(uint8_t*)(uintptr_t)a[1] = 136;               // cBytes
                const uint16_t e = 2;                           // ERROR_FILE_NOT_FOUND
                std::memcpy((void*)(uintptr_t)(a[1] + 2), &e, 2);
            }
            return 0xFFFFFFFFu;                                 // HFILE_ERROR
        });

    // The Ex forms of the font calls, degraded to the non-Ex ones that the
    // console does export (AddFontResourceA is already a generated forward).
    // Dropping the flags loses FR_PRIVATE, which only means a font the guest
    // added stays visible process-wide -- harmless in a container with one app.
    GH_X86_BED(R, "gdi32.dll", "AddFontResourceExA", 12,        // (name, fl, res)
        [](Machine&, const uint32_t* a) -> uint64_t {
            using Ex = int (WINAPI*)(LPCSTR, DWORD, PVOID);
            using Fn = int (WINAPI*)(LPCSTR);
            static Ex pex = (Ex)dyn_resolve("gdi32.dll", "AddFontResourceExA");
            if (pex) return (uint32_t)pex((LPCSTR)(uintptr_t)a[0], a[1], (PVOID)(uintptr_t)a[2]);
            static Fn p = (Fn)dyn_resolve("gdi32.dll", "AddFontResourceA");
            return p ? (uint32_t)p((LPCSTR)(uintptr_t)a[0]) : 0;
        });
    GH_X86_BED(R, "gdi32.dll", "RemoveFontResourceExA", 12,     // (name, fl, pdv)
        [](Machine&, const uint32_t* a) -> uint64_t {
            using Ex = BOOL (WINAPI*)(LPCSTR, DWORD, PVOID);
            using Fn = BOOL (WINAPI*)(LPCSTR);
            static Ex pex = (Ex)dyn_resolve("gdi32.dll", "RemoveFontResourceExA");
            if (pex) return (uint32_t)pex((LPCSTR)(uintptr_t)a[0], a[1], (PVOID)(uintptr_t)a[2]);
            static Fn p = (Fn)dyn_resolve("gdi32.dll", "RemoveFontResourceA");
            return p ? (uint32_t)p((LPCSTR)(uintptr_t)a[0]) : 1;
        });

    // No colour management on the console: there is no ICM profile to name, and
    // FALSE with ERROR_FILE_NOT_FOUND is what a display without one returns.
    GH_X86_BED(R, "gdi32.dll", "GetICMProfileW", 12,            // (hdc, pBufSize, pszFilename)
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[1]) { const uint32_t z = 0; std::memcpy((void*)(uintptr_t)a[1], &z, 4); }
            SetLastError(ERROR_FILE_NOT_FOUND);
            return 0;
        });

    // -- cfgmgr32, reached through setupapi -----------------------------------
    // SDL2's HIDAPI walks a device's parent chain for its VID/PID this way.
    // The device list above is already empty, so the only consistent answer is
    // that there is no such devnode -- and Registry::find matches on the name
    // alone, so one registration serves both setupapi and cfgmgr32 imports.
    GH_X86_BED(R, "setupapi.dll", "CM_Locate_DevNodeA", 12,     // (pdnDevInst, pDeviceID, ulFlags)
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[0]) { const uint32_t z = 0; std::memcpy((void*)(uintptr_t)a[0], &z, 4); }
            return 0x0000000Du;                                 // CR_NO_SUCH_DEVNODE
        });
    GH_X86_BED(R, "setupapi.dll", "CM_Get_Parent", 12,          // (pdnDevInst, dnDevInst, ulFlags)
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[0]) { const uint32_t z = 0; std::memcpy((void*)(uintptr_t)a[0], &z, 4); }
            return 0x0000000Du;                                 // CR_NO_SUCH_DEVNODE
        });
    GH_X86_BED(R, "setupapi.dll", "CM_Get_Device_IDA", 16,      // (dnDevInst, Buffer, BufferLen, ulFlags)
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[1] && a[2]) *(char*)(uintptr_t)a[1] = '\0';
            return 0x0000000Du;                                 // CR_NO_SUCH_DEVNODE
        });

    // -- ddraw ----------------------------------------------------------------
    // 0.0.914.0 answered DDERR_GENERIC here, and that one failure cost HL1 the
    // renderer it already had: sw.dll reported "unsupported video", hl.exe
    // reloaded itself with hw.dll, and hw.dll wants 31 OpenGL entry points this
    // console does not have. sw.dll blits through SDL2 (and so through our
    // D3D9 layer); DirectDraw is only how GoldSrc claims the display. So the
    // object is ours now -- see gh_x86_ddraw.h for what each method answers and
    // why.
    GH_X86_BED(R, "ddraw.dll", "DirectDrawCreate", 12,          // (lpGUID, lplpDD, pUnkOuter)
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[1] || !m.cpu.ptr(a[1], 4)) {
                log("X86 DirectDrawCreate with no output pointer -> DDERR_INVALIDPARAMS");
                return kDdErrInvalidParams;
            }
            const uint32_t obj = ddraw_create(m);
            std::memcpy((void*)(uintptr_t)a[1], &obj, 4);
            if (!obj) {
                log("X86 DirectDrawCreate could not build the object -> DDERR_GENERIC");
                return 0x80004005u;
            }
            log("X86 DirectDrawCreate -> DD_OK, synthetic IDirectDraw at " + hex(obj));
            return kDdOk;
        });
    // Same object for the Ex entry point: its extra argument is the IID, and
    // the only one this vtable IS is IID_IDirectDraw. Anything else has to say
    // so rather than be handed a vtable whose SetDisplayMode pops 16 bytes.
    GH_X86_BED(R, "ddraw.dll", "DirectDrawCreateEx", 16,        // (lpGUID, lplpDD, iid, pUnkOuter)
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (a[1] && m.cpu.ptr(a[1], 4)) {
                const uint32_t z = 0; std::memcpy((void*)(uintptr_t)a[1], &z, 4);
            }
            log("X86 DirectDrawCreateEx -> E_NOINTERFACE (only IID_IDirectDraw is answered here)");
            return kDdErrNoInterface;
        });

    // -- winmm: MCI and the wave-in error text --------------------------------
    // MCI is HL1's CD audio. There is no CD, and "invalid device" is what a
    // machine without one returns, which the engine already handles by playing
    // no track music.
    GH_X86_BED(R, "winmm.dll", "mciSendCommandA", 16,           // (id, msg, dwParam1, dwParam2)
        [](Machine&, const uint32_t*) -> uint64_t { return 257; });   // MCIERR_INVALID_DEVICE_ID
    GH_X86_BED(R, "winmm.dll", "mciGetErrorStringA", 12,        // (mcierr, pszText, cchText)
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[1] && a[2]) *(char*)(uintptr_t)a[1] = '\0';
            return 0;                                           // FALSE: no text for this code
        });
    // An empty string with NOERROR, not a failure: the caller is formatting a
    // message, and a failure here would make it print its own garbage instead.
    GH_X86_BED(R, "winmm.dll", "waveInGetErrorTextA", 12,       // (mmrError, pszText, cchText)
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[1] && a[2]) *(char*)(uintptr_t)a[1] = '\0';
            return MMSYSERR_NOERROR;
        });
}


// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// The 32-bit Steam bridge (gh_x86_steam=1)
// ---------------------------------------------------------------------------
// Valve's 32-bit steam_api.dll cannot work on this console: SteamAPI_Init looks
// for a running Steam client, does not find one, and says so --
//
//   [S_API] SteamAPI_Init(): SteamAPI_IsSteamRunning() did not locate a running
//           instance of Steam.
//   [S_API] SteamAPI_Init(): Could not determine Steam client install directory.
//
// -- at which point Half-Life's engine prints "Failed to initalize
// authentication interface. Exiting..." and calls ExitProcess(-1). The console
// DOES have a Steam session; it belongs to the x64 engine inside the loader,
// which a 32-bit guest has no way to call. These beds are that bridge, and
// g_x86_bed_only_steam stops the game's own copy being mapped so they are
// reached at all (resolve_for_module prefers a mapped guest DLL to a bed).
//
// The whole surface is cdecl: Steamworks' S_CALLTYPE is __cdecl, so the CALLER
// pops and GH_X86_BED_CDECL is the right macro. Getting that wrong would
// unbalance the guest's stack on the very first call.
//
// This first pass answers the flat entry points only and returns no interfaces.
// That is deliberate: it is what turns "we do not know what Half-Life wants
// from Steam" into a diag that NAMES each interface version it asks for, which
// is what the proxy vtables have to be built against. Expect the engine to get
// past its authentication check and then find a null interface.
// -- 32-bit Steam interface stubs -------------------------------------------
//
// The 0.0.909.0 probe answered the question it was built to ask: HL1 gets past
// the authentication gate once SteamAPI_Init says yes, and then asks for
// SteamInput006 and STEAMAPPS_INTERFACE_VERSION008 -- exactly the two the
// engine consults first. Returning null for them is survivable only where the
// engine happens to test the pointer, and a static read of sw.dll says it
// usually does not: 26 of its 49 interface uses dereference the pointer with no
// null check at all. So the next step cannot be another null.
//
// These stubs are a guest object whose vtable is ours. Zero is the default
// answer, and the right one nearly everywhere -- it is "no", "none", "invalid
// handle" and "unavailable" across this API. Two slots are the exception, and
// they are the whole reason HL1 would not start; see `ret` below.
//
// The ONLY thing a stub has to get right is the stack. Windows Steam interfaces
// are thiscall: `this` is in ECX (which a stub that ignores it never needs) and
// the CALLEE pops the arguments. Pop the wrong number and the guest's stack is
// silently wrong from then on. So the pop counts below are not guesses: each
// one was read off sw.dll's own call sites by counting the bytes pushed between
// the accessor and the vtable call, and every site for a given slot agreed.
//
// This table was built WRONG twice, and both times the gap was the bug.
//
// The first version matched `call dword ptr [reg+disp]`, so it saw only the
// sites where the compiler called straight through the vtable and missed every
// site that loads the slot into a register first (`mov eax,[reg+0x18]; call
// eax`) -- which is how the ownership gate is called. The second followed the
// pointer instead of matching one instruction shape, and resolved 25 slots.
//
// 25 was still not all of them, because both scans anchored on the CALL to
// SteamInternal_ContextInit, found by searching for the bytes `FF 15 <iat
// slot>`. sw.dll also loads that import into a register once and calls it
// through the register -- `mov esi,[iat]` and then ten times `push ctx; call
// esi` -- and no byte search for FF 15 can see any of those. That is the whole
// of ISteamInput, and cell 910 died on the first call through it: the stub had
// no pop count for the slot, popped nothing, and the guest's stack was wrong
// from that instruction on.
//
// The third scan anchors on the ctx ARRAY instead, which cannot hide. Valve's
// accessor is `static void* ctx[3] = { &Init_<iface>, 0, 0 }; return
// *(T**)SteamInternal_ContextInit(ctx);`, so: find the Init_ bodies (the only
// functions that call FindOrCreate*Interface, each pushing its version string),
// find the dwords in the image that point at one, and find every `push <that
// address>` in .text. Whatever shape the call takes, eax afterwards is &iface.
// That is 113 accessor uses and 56 slots, every slot agreeing with itself
// across all of its sites.
//
// Getting the pop counts to agree took three corrections, each of which had
// been silently wrong before:
//   - `sub esp, N` is how MSVC passes a small struct by value. Those bytes are
//     arguments too (ISteamGameServer slot 41 is 16, not 8).
//   - `add esp, N` after an intervening cdecl helper is that helper's cleanup.
//     Counting its arguments made STEAMHTTP slot 0 look like 20 when it is 8 --
//     CreateHTTPRequest(EHTTPMethod, const char*).
//   - a helper with NO such cleanup is callee-pop, and its pushes have to be
//     dropped (ISteamNetworkingUtils slot 14 is 4 at both of its sites).
// A blanket reset at every intervening call is wrong in the other direction: a
// compiler pushes some arguments, calls a helper to compute another, and pushes
// the result after, so the run spans the call.
//
// Shape-matching a compiler's output tells you what it usually emits; only
// following the data tells you what it emitted here.
//
// Slot numbers are vtable indices, which are identical between the x86 and x64
// builds of one interface version; the version strings come from the image, not
// from an SDK header.
//
// `ret` is what the slot answers in EAX. It is 0 unless there is a reason, and
// there is exactly one reason here: ISteamApps slots 0 and 6 are the engine
// asking "do you own this game?", and a false answer is fatal in a way that
// leaves no trace. The chain, read out of sw.dll:
//
//   hl.exe                      calls IEngineAPI::Run, exits with what it gets
//   CEngineAPI::Run     1021ea20  __except returns 0 -> silent, clean exit
//   RunListenServer     1021efb0  FileSystem_Init == 0 -> return 0
//   FileSystem_Init     101c6610  tail call == 0 -> return 0
//                       101b92e0  parses -basedir/-game, then
//                       101c6840  the gamedir/appid setup, which ends in
//                       10217330  if (!SteamApps()) return false;
//                                 return SteamApps()->BIsSubscribedApp(appid)
//
// The appid comes from a table in sw.dll mapping gamedir to app: valve->70,
// cstrike->10, gearbox->50, bshift->130, and so on. So "false" here means the
// engine believes this account does not own Half-Life, and every layer above
// turns that into ExitProcess(0) with no message, no log line and no fault.
//
// The x64 shim already reached this exact conclusion from the other direction:
// cell ENGOFF2 watched a 64-bit guest call ISteamApps slots 0 and 6, get false
// twice, and quit cleanly, and corpus/shims/steam_api64.cpp has answered TRUE
// there ever since. This is the same answer on the 32-bit side, for the same
// reason, and it is a true one: the title was downloaded from Steam with the
// user's own sign-in, so the account does own it.
//
// `retstr` is for the handful of slots that answer with a `const char*`. Zero
// is NOT a safe answer for those: the caller hands the result straight to
// strlen or strcpy, so a null is an access violation one frame later. When it
// is set, the slot answers a pointer to a guest copy of this string instead.
//
// `custom` is for a slot whose answer has to be computed from its arguments.
// It runs instead of `ret`/`retstr`, and `argbytes` still decides the pop, so
// a custom slot is exactly as safe as a constant one.

// ---------------------------------------------------------------------------
// Half-Life 1's listen-server authentication handshake
// ---------------------------------------------------------------------------
// GoldSrc single-player IS a listen server, and its CLIENT half will not send
// its own connect packet until Steam has given it a certificate:
//
//     len = ISteamUser::InitiateGameConnection(blob, 1024, gsID, ip, port, sec)
//
// A zero length prints "STEAM certificate length error! 0/1024" and the engine
// unloads the map it has just finished loading. That is what 921 did, with
// c0a0.bsp, its .nod graph and every model already in memory.
//
// `sv_lan 1` does NOT avoid it. 921 proved that on the device: the cvar reached
// the command line (the diag shows "+sv_lan 1" in CMDLINE) and the call still
// happened. sv_lan only changes which auth protocol the SERVER advertises; the
// client asks for its certificate whenever SteamUser() is non-null, and leaving
// SteamUser() null is not an option -- the engine fatals without it.
//
// So we issue a certificate of our own. The engine treats it as opaque: it
// copies the bytes into the connect packet and the server hands them straight
// back to ISteamGameServer. Nothing reads inside it but us. Two consequences
// shape the format:
//
//  - It is printable ASCII with no NUL, no quote and no backslash, because
//    GoldSrc appends it to a text connect packet.
//  - It carries a magic, which is how the game-server side identifies itself
//    below without guessing at a vtable layout.
inline constexpr char     kGhAuthMagic[]  = "GHSTEAM1";
inline constexpr uint32_t kGhAuthBlobLen  = 64;
// The same identity the x64 shim answers with: Public/Individual/Desktop/1.
// See the kFakeSteamID identity defect -- one shared id is wrong for real
// multiplayer and exactly right for a single-player listen server.
inline constexpr uint64_t kGhAuthSteamID  = 0x0110000100000001ULL;
inline int g_gh_auth_issued = 0;
inline int g_gh_auth_trace  = 0;

// True for an address that is safe to touch in the guest: every guest mapping
// lives below 4 GB and above the NULL page, and in native mode a guest pointer
// is also a host pointer, so this is the whole of the check.
inline bool gh_guest_ptr_ok(uint32_t p, uint32_t len)
{
    return p >= 0x10000u && (uint64_t)p + len <= 0xFFFF0000ull;
}

// ISteamUser::InitiateGameConnection(void* pAuthBlob, int cbMaxAuthBlob,
//     CSteamID steamIDGameServer, uint32 unIPServer, uint16 usPortServer,
//     bool bSecure) -- SteamUser023 slot 3, 28 argument bytes
//     (4 + 4 + 8 + 4 + 4 + 4; `this` rides in ECX and is not on the stack).
// Answers the number of bytes written, which is what the engine compares.
inline uint64_t gh_steam_initiate_game_connection(Machine&, const uint32_t* a)
{
    const uint32_t blob = a[0];
    const int32_t  cap  = (int32_t)a[1];
    if (!gh_guest_ptr_ok(blob, kGhAuthBlobLen) || cap < (int32_t)kGhAuthBlobLen) {
        log("X86 STEAM InitiateGameConnection: no room for a certificate (buf " +
            hex(blob) + " cap " + std::to_string(cap) + ") -> 0");
        return 0;
    }
    char buf[kGhAuthBlobLen];
    std::memcpy(buf, kGhAuthMagic, 8);
    // The id goes in as 16 hex characters rather than 8 raw bytes, because the
    // raw form contains NULs and this blob travels inside a text packet.
    static const char kHex[] = "0123456789ABCDEF";
    for (int i = 0; i < 16; ++i)
        buf[8 + i] = kHex[(kGhAuthSteamID >> (60 - 4 * i)) & 0xF];
    for (uint32_t i = 24; i < kGhAuthBlobLen; ++i)
        buf[i] = (char)('a' + (i % 26));
    std::memcpy((void*)(uintptr_t)blob, buf, kGhAuthBlobLen);
    ++g_gh_auth_issued;
    log("X86 STEAM InitiateGameConnection -> a " + std::to_string(kGhAuthBlobLen) +
        " byte Greenhouse certificate at " + hex(blob) + " (cap " +
        std::to_string(cap) + ")");
    return kGhAuthBlobLen;
}

// ISteamGameServer::SendUserConnectAndAuthenticate(uint32 unIPClient,
//     const void* pvAuthBlob, uint32 cubAuthBlobSize, CSteamID* pSteamIDUser)
//     -- 16 argument bytes.
//
// It is slot 38, which 922 established at run time: the trace caught it being
// called with 0x7F000001 one instruction after the certificate went out.
//
// The handler stays on the other two 16-byte slots as well, because the
// certificate is what identifies the call rather than the slot index. Those two
// are the packet relay, which carries a length where this one carries a
// pointer, so the test cannot confuse them, and for them the handler answers 0
// exactly as the table did.
inline uint64_t gh_steam_gs_connect_and_auth(Machine&, const uint32_t* a)
{
    const uint32_t ip = a[0], blob = a[1], len = a[2], out = a[3];
    const bool ours = len == kGhAuthBlobLen &&
                      gh_guest_ptr_ok(blob, kGhAuthBlobLen) &&
                      std::memcmp((const void*)(uintptr_t)blob, kGhAuthMagic, 8) == 0;
    if (!ours) return 0;
    if (gh_guest_ptr_ok(out, 8))
        std::memcpy((void*)(uintptr_t)out, &kGhAuthSteamID, 8);
    // hex() prefixes "0x" itself, so the two halves of the id are joined by
    // hand rather than concatenated through it.
    char id[24];
    std::snprintf(id, sizeof id, "0x%08X%08X",
                  (unsigned)(kGhAuthSteamID >> 32), (unsigned)kGhAuthSteamID);
    log("X86 STEAM SendUserConnectAndAuthenticate: our own certificate came "
        "back from client ip " + hex(ip) + " -- approving it as " + id);
    return 1;
}

struct SteamSlotSpec {
    const char* iface; uint16_t slot; uint16_t argbytes; uint32_t ret;
    const char* retstr = nullptr;
    uint64_t (*custom)(Machine&, const uint32_t*) = nullptr;
};

inline constexpr SteamSlotSpec kSteamSlots[] = {
    // STEAMAPPS_INTERFACE_VERSION008
        // BIsSubscribed. No call site reaches it, but the signature is
        // certain and it asks the same question as slot 6, so it must not
        // answer differently if some path we did not find gets here.
    { "STEAMAPPS_INTERFACE_VERSION008",   0,   0, 1 },
        // BIsLowViolence(). Found at run time, not by the scan: cell 911 logged
        // it as an unknown slot called from 0x101c6923, inside the same gamedir
        // setup that holds the ownership gate. It takes no arguments, so the
        // unknown-slot thunk's "pop nothing" happened to be right -- this time.
    { "STEAMAPPS_INTERFACE_VERSION008",   1,   0, 0 },
        // GetCurrentGameLanguage. Null is safe: the call site falls back to
        // "english" when it is 0.
    { "STEAMAPPS_INTERFACE_VERSION008",   4,   0, 0 },
        // BIsSubscribedApp(AppId_t) -- the ownership gate. See above.
    { "STEAMAPPS_INTERFACE_VERSION008",   6,   4, 1 },
    { "STEAMAPPS_INTERFACE_VERSION008",  26,   8, 0 },
    // STEAMHTTP_INTERFACE_VERSION003
    { "STEAMHTTP_INTERFACE_VERSION003",   0,   8, 0 },
    { "STEAMHTTP_INTERFACE_VERSION003",   3,  12, 0 },
    { "STEAMHTTP_INTERFACE_VERSION003",   5,   8, 0 },
    { "STEAMHTTP_INTERFACE_VERSION003",  11,   8, 0 },
    { "STEAMHTTP_INTERFACE_VERSION003",  12,  12, 0 },
    { "STEAMHTTP_INTERFACE_VERSION003",  14,   4, 0 },
    { "STEAMHTTP_INTERFACE_VERSION003",  15,   8, 0 },
    // SteamFriends017
        // GetPersonaName. Returns a string, so see `retstr`: a null here
        // faults in the caller's strlen.
    { "SteamFriends017",   0,   0, 0, "Player" },
    { "SteamFriends017",  43,   8, 0 },
    // SteamInput006
        // SteamInput006 is here only because of the rescan that found it:
        // sw.dll calls ContextInit for it through a register, so no byte
        // search for `FF 15 <iat>` ever saw these five slots.
    { "SteamInput006",   0,   4, 0 },
    { "SteamInput006",   9,   4, 0 },
    { "SteamInput006",  10,  16, 0 },
    { "SteamInput006",  12,  16, 0 },
    { "SteamInput006",  14,   8, 0 },
    // SteamNetworkingSockets012
    { "SteamNetworkingSockets012",  42,   4, 0 },
    { "SteamNetworkingSockets012",  43,   8, 0 },
    { "SteamNetworkingSockets012",  46,   4, 0 },
    // SteamNetworkingUtils004
    { "SteamNetworkingUtils004",  13,   8, 0 },
    { "SteamNetworkingUtils004",  14,   4, 0 },
    { "SteamNetworkingUtils004",  20,  16, 0 },
    { "SteamNetworkingUtils004",  23,  12, 0 },
    // SteamGameServer015
    { "SteamGameServer015",   3,   4, 0 },
    { "SteamGameServer015",   4,   4, 0 },
    { "SteamGameServer015",   6,   0, 0 },
    { "SteamGameServer015",   7,   0, 0 },
    { "SteamGameServer015",   8,   0, 0 },
    { "SteamGameServer015",   9,   0, 0 },
    { "SteamGameServer015",  10,   4, 0 },
    { "SteamGameServer015",  11,   0, 0 },
    { "SteamGameServer015",  12,   4, 0 },
    { "SteamGameServer015",  13,   4, 0 },
    { "SteamGameServer015",  14,   4, 0 },
    { "SteamGameServer015",  15,   4, 0 },
    { "SteamGameServer015",  16,   4, 0 },
    { "SteamGameServer015",  19,   0, 0 },
    { "SteamGameServer015",  20,   8, 0 },
    { "SteamGameServer015",  24,   4, 0 },
        // The three 16-byte slots. One of them is SendUserConnectAndAuthenticate
        // and the other two are the packet relay; the certificate decides which
        // is which at the call, so no slot here is guessed at. See
        // gh_steam_gs_connect_and_auth.
    { "SteamGameServer015",  34,  16, 0, nullptr, &gh_steam_gs_connect_and_auth },
    { "SteamGameServer015",  35,  16, 0, nullptr, &gh_steam_gs_connect_and_auth },
        // SendUserConnectAndAuthenticate, and the only slot here that the
        // static scan got WRONG: it read 4 bytes, and 922 showed what that
        // costs. The run traced the call as `slot 38 args 0x7F000001` -- the
        // loopback client's own address, arriving one instruction after the
        // certificate went out -- and popped 4 of its 16 bytes, so the guest
        // stack was short by 12 from that return on and the very next engine
        // line was "EDICT_NUM: bad number 5350113". 16 is the signature:
        // (uint32 unIPClient, const void* pvAuthBlob, uint32 cubAuthBlobSize,
        //  CSteamID* pSteamIDUser).
    { "SteamGameServer015",  38,  16, 0, nullptr, &gh_steam_gs_connect_and_auth },
    { "SteamGameServer015",  39,   4, 0 },
    { "SteamGameServer015",  40,   4, 0 },
    { "SteamGameServer015",  41,  16, 0, nullptr, &gh_steam_gs_connect_and_auth },
    // SteamUser023
        // InitiateGameConnection -- the certificate the client will not connect
        // without. See gh_steam_initiate_game_connection.
    { "SteamUser023",   3,  28, 0, nullptr, &gh_steam_initiate_game_connection },
    { "SteamUser023",   4,   8, 0 },
    { "SteamUser023",   7,   0, 0 },
    { "SteamUser023",   8,   0, 0 },
    { "SteamUser023",   9,  12, 0 },
    { "SteamUser023",  10,  36, 0 },
    { "SteamUser023",  11,  24, 0 },
    { "SteamUser023",  20,  16, 0 },
    // SteamUtils010
    { "SteamUtils010",  34,   0, 0 },
    { "SteamUtils010",  35,  20, 0 },
};
inline constexpr size_t kSteamSlotCount =
    sizeof(kSteamSlots) / sizeof(kSteamSlots[0]);

inline int g_steam_slot_hits[kSteamSlotCount] = { 0 };

template <size_t I>
inline uint64_t steam_slot_invoke(Machine& m, const uint32_t* args)
{
    // Once a certificate is out, the next thing to happen is the connect packet
    // and the server's answer to it. Trace the game-server calls over that
    // window with their arguments: if the approval does not land on one of the
    // three 16-byte slots, this is what names the slot that it did land on,
    // without another run.
    if (g_gh_auth_issued && g_gh_auth_trace < 32 &&
        std::strcmp(kSteamSlots[I].iface, "SteamGameServer015") == 0) {
        ++g_gh_auth_trace;
        std::string s = "X86 STEAM auth-window SteamGameServer015 slot " +
                        std::to_string(kSteamSlots[I].slot) + " args";
        for (uint32_t i = 0; i * 4 < kSteamSlots[I].argbytes && i < 4; ++i)
            s += " " + hex(args[i]);
        log(s);
    }
    if (kSteamSlots[I].custom) return kSteamSlots[I].custom(m, args);
    uint32_t r = kSteamSlots[I].ret;
    if (kSteamSlots[I].retstr) {
        // One guest copy per slot, made on first use and kept for the life of
        // the session: the guest may hold the pointer for as long as it likes.
        // `static` here is per-instantiation, so each slot gets its own.
        static uint32_t cached = 0;
        static uint32_t stamp = 0;
        if (x86_session_changed(stamp)) cached = 0;
        if (!cached) {
            const size_t n = std::strlen(kSteamSlots[I].retstr) + 1;
            cached = m.heap_alloc((uint32_t)n, true);
            if (cached) std::memcpy((void*)(uintptr_t)cached, kSteamSlots[I].retstr, n);
            else log("X86 STEAM no guest heap for a string return");
        }
        r = cached;
    }
    // Loud for the first few, then quiet: some of these sit in the frame loop
    // and the point of the log is to show WHICH slots a run reaches, once.
    if (++g_steam_slot_hits[I] <= 4)
        log(std::string("X86 STEAM call ") + kSteamSlots[I].iface + " slot " +
            std::to_string(kSteamSlots[I].slot) + " (pops " +
            std::to_string(kSteamSlots[I].argbytes) + ") -> " +
            (kSteamSlots[I].retstr ? std::string("\"") + kSteamSlots[I].retstr + "\" at " + hex(r)
                                   : std::to_string(r)));
    return r;
}

// A slot no call site in sw.dll reaches. We do not know the slot's argument
// count, so we cannot pop for it, and popping nothing leaves the guest's stack
// wrong from then on. Cell 910 is what that looks like: the first unknown slot
// was called, the stack went sideways, a C++ throw arrived with no handler, and
// the run spun on one faulting instruction for 88,000 diag lines.
//
// So this log has to be enough to FIX the slot without another run. It names
// the slot index and the guest return address; `args` points one dword past the
// return address the call pushed, so args[-1] is the call site, and that is the
// address to disassemble to count the pushes.
inline int g_steam_unknown_hits = 0;
template <uint32_t S>
inline uint64_t steam_slot_unknown(Machine&, const uint32_t* args)
{
    if (++g_steam_unknown_hits <= 16)
        log("X86 STEAM call to UNKNOWN slot " + std::to_string(S) +
            " from guest " + hex(args[-1]) +
            " -- returning 0 and popping nothing; the guest's stack is now "
            "suspect. Disassemble that address to read the pop count.");
    return 0;
}

using SteamInvokeFn0 = uint64_t (*)(Machine&, const uint32_t*);
template <uint32_t... Ss>
inline const SteamInvokeFn0* steam_unknowns(std::integer_sequence<uint32_t, Ss...>)
{
    static const SteamInvokeFn0 t[] = { &steam_slot_unknown<Ss>... };
    return t;
}

using SteamInvokeFn = uint64_t (*)(Machine&, const uint32_t*);
template <size_t... Is>
inline const SteamInvokeFn* steam_invokes(std::index_sequence<Is...>)
{
    static const SteamInvokeFn t[] = { &steam_slot_invoke<Is>... };
    return t;
}

// 80 slots covers every version here with room to spare; the largest index any
// call site uses is 46, and a vtable costs four bytes a slot.
inline constexpr uint32_t kSteamVtableSlots = 80;

inline uint32_t steam_stub_for(Machine& m, const char* ver)
{
    if (!ver || !*ver) return 0;
    static std::vector<std::pair<std::string, uint32_t>> cache;
    static uint32_t stamp = 0;                  // guest objects: one session's
    if (x86_session_changed(stamp)) cache.clear();
    for (auto& e : cache) if (e.first == ver) return e.second;

    // An interface we know NO slot of is better left null, and that is measured,
    // not assumed. Cell 909 answered every accessor null: the engine asked for
    // SteamInput006, took the null without complaint, went on to ask ISteamApps
    // the ownership question and exited cleanly there. Cell 910 gave the same
    // SteamInput006 an 80-slot stub with no known pop counts, and the first call
    // through it destroyed the guest stack before the engine ever reached the
    // ownership question. A stub is only safer than null where we can pop
    // correctly; where we cannot, it is strictly more dangerous.
    int cover = 0;
    for (size_t i = 0; i < kSteamSlotCount; ++i)
        if (std::strcmp(kSteamSlots[i].iface, ver) == 0) ++cover;
    if (!cover) {
        log(std::string("X86 STEAM ") + ver +
            ": no slot of this interface is in the table, so returning null "
            "rather than a stub we cannot pop for (909 survived the null here)");
        cache.emplace_back(ver, 0u);
        return 0;
    }

    const uint32_t vt = m.heap_alloc(kSteamVtableSlots * 4, true);
    if (!vt) { log(std::string("X86 STEAM no guest heap for ") + ver); return 0; }

    const SteamInvokeFn* inv =
        steam_invokes(std::make_index_sequence<kSteamSlotCount>{});
    const SteamInvokeFn0* unk =
        steam_unknowns(std::make_integer_sequence<uint32_t, kSteamVtableSlots>{});
    int known = 0;
    for (uint32_t s = 0; s < kSteamVtableSlots; ++s) {
        // HostFn and its name must outlive the guest, so they are allocated and
        // never freed on purpose: one per slot, for the life of the process.
        HostFn* f = new HostFn();
        f->dll  = "steam_api.dll";
        f->name = (new std::string(std::string(ver) + "::slot" +
                                   std::to_string(s)))->c_str();
        f->invoke = unk[s];
        f->stack_bytes = 0;
        for (size_t i = 0; i < kSteamSlotCount; ++i)
            if (kSteamSlots[i].slot == s && std::strcmp(kSteamSlots[i].iface, ver) == 0) {
                f->invoke = inv[i];
                f->stack_bytes = kSteamSlots[i].argbytes;
                ++known;
                break;
            }
        const uint32_t t = m.make_thunk("steam_api.dll", f->name, f);
        if (!t) { log(std::string("X86 STEAM thunk pool exhausted on ") + ver); return 0; }
        std::memcpy((void*)(uintptr_t)(vt + s * 4), &t, 4);
    }

    const uint32_t obj = m.heap_alloc(4, true);
    if (!obj) { log(std::string("X86 STEAM no guest heap for a ") + ver); return 0; }
    std::memcpy((void*)(uintptr_t)obj, &vt, 4);

    log(std::string("X86 STEAM stub ") + ver + " at " + hex(obj) + " vtable " + hex(vt) +
        " (" + std::to_string(known) + " slots with a known stack, " +
        std::to_string(kSteamVtableSlots - (uint32_t)known) + " without)");
    cache.emplace_back(ver, obj);
    return obj;
}

inline void RegisterSteamBridgeBeds(ThunkRegistry& R)
{
    // SteamAPI_Init: the call the whole exit path hangs on. The loader's own
    // engine is already signed in by the time a guest runs -- the title config
    // requires the steam-runtime service -- so the honest answer is yes.
    GH_X86_BED_CDECL(R, "steam_api.dll", "SteamAPI_Init",
        [](Machine&, const uint32_t*) -> uint64_t {
            log("X86 STEAM SteamAPI_Init -> true (bridged to the loader's x64 engine)");
            return 1;
        });
    GH_X86_BED_CDECL(R, "steam_api.dll", "SteamAPI_Shutdown",
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });

    // A pipe/user handle has to be non-zero to read as valid; 1 is what a
    // single-user client hands out, and nothing in the guest dereferences it.
    GH_X86_BED_CDECL(R, "steam_api.dll", "SteamAPI_GetHSteamUser",
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    // Single-player GoldSrc IS a listen server: the engine builds a CSteam3Server,
    // and CSteam3Server::Activate() raises a fatal Sys_Error("Unable to initialize
    // Steam.") the moment the game-server init says no. Saying no therefore kills
    // Half-Life at the end of its first map load, after the .bsp is already in.
    // So the server half answers exactly as the client half does. 2 keeps it a
    // distinct non-zero handle from the client's 1, which is all any caller reads
    // it for; the interface it goes on to ask for is already stubbed by
    // SteamInternal_FindOrCreateGameServerInterface.
    GH_X86_BED_CDECL(R, "steam_api.dll", "SteamGameServer_GetHSteamUser",
        [](Machine&, const uint32_t*) -> uint64_t { return 2; });
    // Which of these two the engine calls depends on how old its copy of the SDK
    // header is -- the flat SteamGameServer_Init is an inline wrapper over
    // SteamInternal_GameServer_Init in recent ones -- so both are bedded and both
    // say which was used.
    for (const char* n : { "SteamInternal_GameServer_Init", "SteamGameServer_Init" })
        GH_X86_BED_CDECL(R, "steam_api.dll", n,
            [](Machine&, const uint32_t*) -> uint64_t {
                log("X86 STEAM GameServer_Init -> true (bridged to the loader's x64 engine)");
                return 1;
            });
    GH_X86_BED_CDECL(R, "steam_api.dll", "SteamGameServer_RunCallbacks",
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED_CDECL(R, "steam_api.dll", "SteamGameServer_Shutdown",
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });

    // Callbacks. Nothing is dispatched yet, but WHICH callbacks the engine
    // registers is exactly the list the next pass has to be able to deliver, so
    // each id is logged once.
    GH_X86_BED_CDECL(R, "steam_api.dll", "SteamAPI_RegisterCallback",
        [](Machine&, const uint32_t* a) -> uint64_t {
            log("X86 STEAM RegisterCallback id=" + std::to_string(a[1]) + " at " + hex(a[0]));
            return 0;
        });
    GH_X86_BED_CDECL(R, "steam_api.dll", "SteamAPI_UnregisterCallback",
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED_CDECL(R, "steam_api.dll", "SteamAPI_RegisterCallResult",
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED_CDECL(R, "steam_api.dll", "SteamAPI_UnregisterCallResult",
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED_CDECL(R, "steam_api.dll", "SteamAPI_RunCallbacks",
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });

    // Breakpad. The engine installs its own crash handler and hands Steam the
    // app id and a minidump comment; none of it has anywhere to go on a console,
    // and the loader has its own crash observer.
    GH_X86_BED_CDECL(R, "steam_api.dll", "SteamAPI_SetBreakpadAppID",
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED_CDECL(R, "steam_api.dll", "SteamAPI_SetMiniDumpComment",
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED_CDECL(R, "steam_api.dll", "SteamAPI_UseBreakpadCrashHandler",
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED_CDECL(R, "steam_api.dll", "SteamAPI_WriteMiniDump",
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });

    // SteamInternal_ContextInit( void *pContextInitData )
    //
    //   struct { void (*pFn)( void *pCtx ); uintp counter; CSteamAPIContext ctx; }
    //
    // It calls pFn once per counter generation to fill ctx, then returns &ctx.
    // The callback is GUEST code, so this is a host->guest call back across the
    // boundary -- call_guest32 with the guest address of ctx, which is the
    // struct plus two 32-bit fields.
    //
    // Running it even though the interfaces below are still null is the point:
    // it is what makes the engine ASK for them by name.
    GH_X86_BED_CDECL(R, "steam_api.dll", "SteamInternal_ContextInit",
        [](Machine& m, const uint32_t* a) -> uint64_t {
            const uint32_t p = a[0];
            if (!p) return 0;
            uint32_t fn = 0, counter = 0;
            std::memcpy(&fn, (void*)(uintptr_t)p, 4);
            std::memcpy(&counter, (void*)(uintptr_t)(p + 4), 4);
            const uint32_t ctx = p + 8;
            if (fn && !counter) {
                const uint32_t args[1] = { ctx };
                log("X86 STEAM ContextInit -> calling guest filler " + hex(fn) + " ctx " + hex(ctx));
                m.call_guest32(fn, args, 1, false /* cdecl callee */);
                const uint32_t one = 1;
                std::memcpy((void*)(uintptr_t)(p + 4), &one, 4);
            }
            return ctx;
        });

    // The two that hand out interfaces. The previous pass returned null here to
    // make the engine NAME what it wants; it named them, so now it gets an
    // object. The game-server interface is stubbed on the same terms as the
    // client ones: the engine already learns there is no server from
    // SteamGameServer_GetHSteamUser returning 0, and sw.dll dereferences the
    // game-server pointer without a null check at six sites, so null is the
    // more dangerous answer of the two.
    for (const char* n : { "SteamInternal_FindOrCreateUserInterface",
                           "SteamInternal_FindOrCreateGameServerInterface" })
        GH_X86_BED_CDECL(R, "steam_api.dll", n,
            [](Machine& m, const uint32_t* a) -> uint64_t {
                const char* v = (const char*)(uintptr_t)a[1];
                log(std::string("X86 STEAM interface WANTED: ") + (v ? v : "(null)") +
                    " user=" + std::to_string(a[0]));
                return steam_stub_for(m, v);
            });
}

inline void RegisterStdBeds(Machine& m)
{
    m.dll_loaded_hook = &mono_probe::install;
    ThunkRegistry& R = m.registry;
    Machine* mp = &m;
    const size_t std0 = R.size(), drop0 = R.dropped().size();

    // -- pass-through kernel32 -------------------------------------------
    // These take and return only handles and scalars, so the x64 export is
    // already correct for a 32-bit caller.
    // CloseHandle has to know about synthetic mapping handles, so it cannot be
    // a plain forward: handing 0x4D4D0001 to the real CloseHandle would either
    // fail or, worse, close something unrelated.
    GH_X86_BED(R, "kernel32.dll", "CloseHandle", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (BedState::Mapping* mp = mapping_of(m, a[0])) {
                mp->closed = true;
                if (mp->views == 0) release_mapping(m, *mp);
                return 1;
            }
            if (GuestThread* t = m.thread_by_handle(a[0])) {
                t->handle_closed = true;    // the thread itself keeps running
                return 1;
            }
            if (SyncObj* o = m.sync_by_handle(a[0])) {
                o->closed = true;
                return 1;
            }
            return CloseHandle((HANDLE)(uintptr_t)a[0]);
        });
    GH_X86_FWD(R, "kernel32.dll", GetLastError);
    GH_X86_FWD(R, "kernel32.dll", SetLastError);
    GH_X86_FWD(R, "kernel32.dll", GetTickCount);
    GH_X86_FWD(R, "kernel32.dll", GetCurrentProcessId);
    // Sleep and GetCurrentThreadId used to be forwards here. They are beds now
    // (see the thread section): registration is first-wins, so leaving these
    // would silently shadow the beds and hand the guest the HOST's answer.
    // ReadFile/WriteFile are beds further down -- LPOVERLAPPED changes shape.
    GH_X86_FWD(R, "kernel32.dll", SetFilePointer);
    GH_X86_FWD(R, "kernel32.dll", GetFileSize);
    GH_X86_FWD(R, "kernel32.dll", FlushFileBuffers);
    // Beds for the same reason as the opens below: a failed delete is logged.
    // Files go through the loader's VFS beds, as a 64-bit guest's do; a create
    // through the VFS and a delete around it name two different files.
    GH_X86_BED(R, "kernel32.dll", "DeleteFileA", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            const char* p = (const char*)(uintptr_t)a[0];
            wchar_t w[1024];
            const BOOL ok = p && MultiByteToWideChar(CP_ACP, 0, p, -1, w, 1024) && Bed_DeleteFileW(w);
            if (!ok) {
                const DWORD e = GetLastError();
                log("X86 DeleteFileA FAILED err=" + std::to_string(e) + " " + std::string(p ? p : ""));
                SetLastError(e);
            }
            return ok;
        });
    GH_X86_BED(R, "kernel32.dll", "DeleteFileW", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            const wchar_t* w = (const wchar_t*)(uintptr_t)a[0];
            const BOOL ok = Bed_DeleteFileW(w);
            if (!ok) {
                const DWORD e = GetLastError();
                std::string p;
                for (; w && *w; ++w) p += (char)(*w < 128 ? *w : '?');
                log("X86 DeleteFileW FAILED err=" + std::to_string(e) + " " + p);
                SetLastError(e);
            }
            return ok;
        });
    // Beds rather than forwards purely so a FAILED open is always logged, no
    // matter how long ago the startup trace budget ran out. A guest that cannot
    // open a file it needs almost always reports that as something vaguer
    // further downstream ("assembly not found", "could not load"), and the
    // failed open is the fact that actually explains it. Successful opens stay
    // on the ordinary trace, because there are thousands of them.
    GH_X86_BED(R, "kernel32.dll", "CreateFileW", 28,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            HANDLE h = Bed_CreateFileW((LPCWSTR)(uintptr_t)a[0], a[1], a[2],
                                   sa32_translate(a[3]),
                                   a[4], a[5], (HANDLE)(uintptr_t)a[6]);
            if (h != INVALID_HANDLE_VALUE && m.window_prof) {
                std::string p;
                for (const wchar_t* w = (const wchar_t*)(uintptr_t)a[0]; w && *w; ++w)
                    p += (char)(*w < 128 ? *w : '?');
                m.note_file_open((uint32_t)(uintptr_t)h, p);
            }
            if (h == INVALID_HANDLE_VALUE) {
                DWORD e = GetLastError();
                std::string p;
                for (const wchar_t* w = (const wchar_t*)(uintptr_t)a[0]; w && *w; ++w)
                    p += (char)(*w < 128 ? *w : '?');
                log("X86 CreateFileW FAILED err=" + std::to_string(e) +
                    " access=" + hex(a[1]) + " disp=" + hex(a[4]) + " " + p);
            }
            return (uint64_t)(uintptr_t)h;
        });
    GH_X86_BED(R, "kernel32.dll", "CreateFileA", 28,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HANDLE h = Bed_CreateFileA((LPCSTR)(uintptr_t)a[0], a[1], a[2],
                                   sa32_translate(a[3]),
                                   a[4], a[5], (HANDLE)(uintptr_t)a[6]);
            if (h == INVALID_HANDLE_VALUE)
                log("X86 CreateFileA FAILED err=" + std::to_string(GetLastError()) + " " +
                    std::string((const char*)(uintptr_t)a[0] ? (const char*)(uintptr_t)a[0] : ""));
            return (uint64_t)(uintptr_t)h;
        });
    // -- directory enumeration ---------------------------------------------
    //
    // Beds rather than forwards because a find handle is NOT a kernel handle:
    // it is a pointer into the host's heap, above 4GB, and the M2 rule is that
    // anything returning a pointer has to be a bed. The loader's own detector
    // caught this one -- "returned a host pointer 0x1CA539831A0 above 4GB" --
    // which is the entire reason forwards check their return value.
    //
    // WIN32_FIND_DATA{A,W} needs no translation: every field is a DWORD, a
    // FILETIME or an inline character array, so the struct is byte-identical in
    // both bitnesses. Only the handle crosses.
    GH_X86_BED(R, "kernel32.dll", "FindFirstFileW", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return m.host_handle_token(Bed_FindFirstFileW((LPCWSTR)(uintptr_t)a[0],
                                                          (LPWIN32_FIND_DATAW)(uintptr_t)a[1]));
        });
    GH_X86_BED(R, "kernel32.dll", "FindFirstFileExW", 24,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            // Unity asks for FindExInfoBasic/FindExSearchNameMatch, which the
            // plain form answers identically apart from cAlternateFileName --
            // and routing through Bed_FindFirstFileW is what puts the pattern
            // through the VFS, which the Ex form otherwise would not get.
            return m.host_handle_token(Bed_FindFirstFileW((LPCWSTR)(uintptr_t)a[0],
                                                          (LPWIN32_FIND_DATAW)(uintptr_t)a[2]));
        });
    GH_X86_BED(R, "kernel32.dll", "FindFirstFileA", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return m.host_handle_token(FindFirstFileA((LPCSTR)(uintptr_t)a[0],
                                                      (LPWIN32_FIND_DATAA)(uintptr_t)a[1]));
        });
    GH_X86_BED(R, "kernel32.dll", "FindFirstFileExA", 24,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return m.host_handle_token(FindFirstFileA((LPCSTR)(uintptr_t)a[0],
                                                      (LPWIN32_FIND_DATAA)(uintptr_t)a[2]));
        });
    GH_X86_BED(R, "kernel32.dll", "FindNextFileW", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return FindNextFileW(m.host_handle_of(a[0]),
                                 (LPWIN32_FIND_DATAW)(uintptr_t)a[1]) ? 1u : 0u;
        });
    GH_X86_BED(R, "kernel32.dll", "FindNextFileA", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return FindNextFileA(m.host_handle_of(a[0]),
                                 (LPWIN32_FIND_DATAA)(uintptr_t)a[1]) ? 1u : 0u;
        });
    GH_X86_BED(R, "kernel32.dll", "FindClose", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            HANDLE h = m.host_handle_of(a[0]);
            m.host_handle_drop(a[0]);
            return (h != INVALID_HANDLE_VALUE && FindClose(h)) ? 1u : 0u;
        });

    GH_X86_FWD(R, "kernel32.dll", GetFileAttributesA);
    GH_X86_FWD(R, "kernel32.dll", GetFileAttributesW);
    GH_X86_FWD(R, "kernel32.dll", MultiByteToWideChar);
    GH_X86_FWD(R, "kernel32.dll", WideCharToMultiByte);
    GH_X86_FWD(R, "kernel32.dll", lstrlenA);
    GH_X86_FWD(R, "kernel32.dll", lstrlenW);
    GH_X86_FWD(R, "kernel32.dll", QueryPerformanceCounter);
    GH_X86_FWD(R, "kernel32.dll", QueryPerformanceFrequency);
    GH_X86_FWD(R, "kernel32.dll", IsProcessorFeaturePresent);
    GH_X86_FWD(R, "kernel32.dll", IsDebuggerPresent);

    // GetStdHandle deliberately forwards: the x64 loader already redirects the
    // std handles to its capture file, and a 32-bit guest should land in the
    // same place.
    GH_X86_FWD(R, "kernel32.dll", GetStdHandle);

    // -- allocation: must produce guest-addressable memory ----------------
    GH_X86_BED(R, "kernel32.dll", "GetProcessHeap", 0,
        [](Machine& m, const uint32_t*) -> uint64_t {
            return 0x48454150u;                    // 'HEAP' -- an opaque token
        });

    GH_X86_BED(R, "kernel32.dll", "HeapAlloc", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            // (HANDLE hHeap, DWORD flags, SIZE_T bytes)
            return m.heap_alloc(a[2], (a[1] & HEAP_ZERO_MEMORY) != 0);
        });

    GH_X86_BED(R, "kernel32.dll", "HeapFree", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return m.heap_free(a[2]) ? 1u : 0u;
        });

    GH_X86_BED(R, "kernel32.dll", "HeapReAlloc", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            uint32_t old = a[2], want = a[3];
            uint32_t have = m.heap_size_of(old);
            if (have >= want) return old;
            uint32_t np = m.heap_alloc(want, (a[1] & HEAP_ZERO_MEMORY) != 0);
            if (!np) return 0;
            if (old && have) std::memcpy((void*)(uintptr_t)np, (void*)(uintptr_t)old, have);
            m.heap_free(old);
            return np;
        });

    GH_X86_BED(R, "kernel32.dll", "HeapSize", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t { return m.heap_size_of(a[2]); });

    GH_X86_BED(R, "kernel32.dll", "VirtualAlloc", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            // (addr, size, allocType, protect). The guest region is ours to lay
            // out, so a request with no address is just a bump allocation.
            //
            // A request WITH an address is nearly always the second half of the
            // reserve-then-commit pattern -- commit a sub-range of a reservation
            // we already handed out -- and returning it unchanged is exactly
            // right, because the whole region is committed at reserve time.
            //
            // But it is only right when we really do own that address. Handing
            // back an arbitrary hint we never allocated gives the caller memory
            // that either is not committed or, far worse, already belongs to
            // something else -- two allocators writing to one arena, which
            // surfaces as corruption arbitrarily far from here. Outside our
            // bump area the hint is refused and a fresh region substituted.
            if (a[0]) {
                if (m.owns_bump(a[0], a[1] ? a[1] : 1)) return a[0];
                log("X86 VirtualAlloc hint " + hex(a[0]) + " +" + hex(a[1]) +
                    " is not memory we handed out (type=" + hex(a[2]) +
                    ") -- substituting a fresh region");
            }
            return m.alloc(a[1] ? a[1] : 0x1000);
        });

    GH_X86_BED(R, "kernel32.dll", "VirtualFree", 12,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });   // bump allocator: no-op

    // -- process/environment ---------------------------------------------
    GH_X86_BED(R, "kernel32.dll", "ExitProcess", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            m.exit_requested = true;
            m.exit_code = a[0];
            log("X86 guest ExitProcess(" + std::to_string(a[0]) + ")");
            return 0;
        });

    // The launcher's command line: the exe path, the title's args and any
    // ghcmd.txt override, exactly as an x64 guest gets it. Until HL2 s859 this
    // answered "guest.exe" in the launcher too, so no 32-bit title ever saw an
    // argument -- HL2 ran without -game, -novid and every +convar.
    GH_X86_BED(R, "kernel32.dll", "GetCommandLineA", 0,
        [](Machine& m, const uint32_t*) -> uint64_t {
            BedState& b = m.bed;
#ifdef GH_LAUNCHER
            if (!b.cmdline_a) {
                std::string acp;
                const int n = WideCharToMultiByte(CP_ACP, 0, g_guestCmdLineW.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (n > 1) {
                    acp.resize((size_t)n - 1);
                    WideCharToMultiByte(CP_ACP, 0, g_guestCmdLineW.c_str(), -1, acp.data(), n, nullptr, nullptr);
                }
                b.cmdline_a = guest_strdup_a(m, acp.empty() ? "guest.exe" : acp.c_str());
            }
#endif
            if (!b.cmdline_a) b.cmdline_a = guest_strdup_a(m, "guest.exe");
            return b.cmdline_a;
        });
    GH_X86_BED(R, "kernel32.dll", "GetCommandLineW", 0,
        [](Machine& m, const uint32_t*) -> uint64_t {
            BedState& b = m.bed;
#ifdef GH_LAUNCHER
            if (!b.cmdline_w && !g_guestCmdLineW.empty()) b.cmdline_w = guest_strdup_w(m, g_guestCmdLineW.c_str());
#endif
            if (!b.cmdline_w) b.cmdline_w = guest_strdup_w(m, L"guest.exe");
            return b.cmdline_w;
        });

    // -- modules: synthetic handles the guest can hold in 32 bits ---------
    GH_X86_BED(R, "kernel32.dll", "GetModuleHandleA", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0]) return m.image_base;                 // NULL == the exe itself
            BedState& b = m.bed;
            std::string want = Machine::base_name_lower(widen((const char*)(uintptr_t)a[0]));
            if (Machine::GuestDll* d = m.dll_by_name(want)) return d->base;
            for (auto& mod : b.modules) if (mod.name == want) return mod.handle;
            if (m.registry.has_dll(want)) return gh_synth_module(m, want);
            log_capped("gmha:" + want, "X86 GetModuleHandleA(" + want + ") -> 0 (nothing implements it)");
            return 0;
        });
    // A handle is granted for any DLL Greenhouse actually services, whether or
    // not the guest ever called LoadLibrary on it -- on a real system those
    // modules would already be mapped, and a guest that gets NULL here will
    // conclude the OS is missing and give up. A DLL with no beds at all still
    // answers NULL, because for that one the honest answer is "not loaded".
    GH_X86_BED(R, "kernel32.dll", "GetModuleHandleW", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0]) return m.image_base;
            const wchar_t* w = (const wchar_t*)(uintptr_t)a[0];
            std::string nm;
            for (; w && *w; ++w) nm += (char)*w;
            nm = Machine::base_name_lower(std::wstring((const wchar_t*)(uintptr_t)a[0]));
            if (Machine::GuestDll* d = m.dll_by_name(nm)) return d->base;
            for (auto& mod : m.bed.modules) if (mod.name == nm) return mod.handle;
            if (m.registry.has_dll(nm)) return gh_synth_module(m, nm);
            log_capped("gmhw:" + nm, "X86 GetModuleHandleW(" + nm + ") -> 0 (nothing implements it)");
            return 0;
        });

    // GetModuleHandleEx(flags, name-or-address, HMODULE* out). The CRT's exit
    // path asks for mscoree.dll by name; FROM_ADDRESS (4) asks which module
    // holds a code address, which is the exe or one of our mapped DLLs.
    GH_X86_BED(R, "kernel32.dll", "GetModuleHandleExW", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            uint32_t h = 0;
            if (a[0] & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
                const uint32_t va = a[1];
                if (va >= m.image_base && va - m.image_base < m.image_size) h = m.image_base;
                for (auto& d : m.dlls) if (va >= d.base && va - d.base < d.size) h = d.base;
            } else if (!a[1]) {
                h = m.image_base;
            } else {
                std::string nm = Machine::base_name_lower(std::wstring((const wchar_t*)(uintptr_t)a[1]));
                if (Machine::GuestDll* d = m.dll_by_name(nm)) h = d->base;
                else for (auto& mod : m.bed.modules) if (mod.name == nm) h = mod.handle;
            }
            if (a[2]) std::memcpy((void*)(uintptr_t)a[2], &h, 4);
            if (!h) { SetLastError(ERROR_MOD_NOT_FOUND); return 0; }
            return 1;
        });
    GH_X86_BED(R, "kernel32.dll", "OutputDebugStringW", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            const wchar_t* w = (const wchar_t*)(uintptr_t)a[0];
            std::string s;
            for (; w && *w; ++w) s += (*w < 0x80) ? (char)*w : '?';
            if (a[0]) log_capped("dbg:" + s, "X86 dbg: " + s);
            return 0;
        });

    GH_X86_BED(R, "kernel32.dll", "LoadLibraryW", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return a[0] ? gh_load_library(m, (const wchar_t*)(uintptr_t)a[0]) : 0;
        });
    GH_X86_BED(R, "kernel32.dll", "LoadLibraryExW", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return a[0] ? gh_load_library(m, (const wchar_t*)(uintptr_t)a[0]) : 0;
        });
    GH_X86_BED(R, "kernel32.dll", "LoadLibraryA", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return a[0] ? gh_load_library(m, widen((const char*)(uintptr_t)a[0])) : 0;
        });
    GH_X86_BED(R, "kernel32.dll", "LoadLibraryExA", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return a[0] ? gh_load_library(m, widen((const char*)(uintptr_t)a[0])) : 0;
        });
    // Nothing is ever unmapped. A guest DLL's code, data and any pointers the
    // guest still holds into it stay valid, which is strictly safer than
    // honouring an unload -- and no game depends on the address being reused.
    GH_X86_BED(R, "kernel32.dll", "FreeLibrary", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (Machine::GuestDll* d = m.dll_by_base(a[0])) if (d->refs > 0) d->refs--;
            return 1;
        });

    // A bed rather than a forward, and not only because the module handle is
    // ours: this is a DLL opting out of the thread notifications the loader now
    // sends, so ignoring it would mean running someone's DllMain forty times
    // after they asked us not to.
    GH_X86_BED(R, "kernel32.dll", "DisableThreadLibraryCalls", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            Machine::GuestDll* d = m.dll_by_base(a[0]);
            if (!d) return 1;                      // one of our synthetic modules
            d->no_thread_calls = true;
            log("X86 DisableThreadLibraryCalls(" + d->name + ")");
            return 1;
        });

    // The address handed back is a THUNK slot, not a host function: it is code
    // the interpreter recognises and traps on, which is the only kind of address
    // a guest can usefully call. Slots are minted on demand, so an API the guest
    // resolves dynamically works exactly as well as one in its import table --
    // and a great deal of Win32 is reached this way.
    GH_X86_BED(R, "kernel32.dll", "GetProcAddress", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if ((a[1] & 0xFFFF0000u) == 0) {                // imported by ordinal
                log("X86 GetProcAddress(ordinal #" + std::to_string(a[1]) + ") unsupported");
                return 0;
            }
            const char* nm = (const char*)(uintptr_t)a[1];
            if (!nm) return 0;

            // A mapped guest DLL answers from its own export table -- a real
            // address inside real guest code, not a thunk.
            if (Machine::GuestDll* d = m.dll_by_base(a[0])) {
                uint32_t addr = m.export_addr(*d, nm, 0);
                if (!addr) log("X86 GetProcAddress(" + d->name + "!" + nm + ") -> 0 (not exported)");
                return addr;
            }
            std::string dll;
            for (auto& mod : m.bed.modules) if (mod.handle == a[0]) dll = mod.name;

            uint32_t th = m.thunk_for(dll, undecorate(nm));
            if (!th) log("X86 GetProcAddress(" + dll + "!" + nm + ") -> 0 (no bed)");
            return th;
        });

    // -- misc --------------------------------------------------------------
    GH_X86_BED(R, "kernel32.dll", "OutputDebugStringA", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            const char* s = (const char*)(uintptr_t)a[0];
            if (s) log_capped(std::string("dbg:") + s, std::string("X86 dbg: ") + s);
            return 0;
        });

    GH_X86_BED(R, "kernel32.dll", "GetSystemTimeAsFileTime", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[0]) GetSystemTimeAsFileTime((LPFILETIME)(uintptr_t)a[0]);
            return 0;
        });

    // -- allocators that hand back a POINTER ---------------------------------
    // Caught by the dispatcher's above-4GB check rather than by inspection: the
    // host heap lives far above 4GB, so every one of these must allocate from
    // guest memory instead of forwarding. Heap handles are opaque tokens to the
    // guest, so one shared guest heap behind every handle is behaviourally fine.
    GH_X86_BED(R, "kernel32.dll", "HeapCreate", 12,
        [](Machine&, const uint32_t*) -> uint64_t { return 0x48454150u; });
    GH_X86_BED(R, "kernel32.dll", "HeapDestroy", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    GH_X86_BED(R, "kernel32.dll", "HeapValidate", 12,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    GH_X86_BED(R, "kernel32.dll", "HeapCompact", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 0x10000; });

    GH_X86_BED(R, "kernel32.dll", "LocalAlloc", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return m.heap_alloc(a[1], (a[0] & 0x0040) != 0);      // LMEM_ZEROINIT
        });
    GH_X86_BED(R, "kernel32.dll", "LocalFree", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t { m.heap_free(a[0]); return 0; });
    GH_X86_BED(R, "kernel32.dll", "GlobalAlloc", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return m.heap_alloc(a[1], (a[0] & 0x0040) != 0);      // GMEM_ZEROINIT
        });
    GH_X86_BED(R, "kernel32.dll", "GlobalFree", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t { m.heap_free(a[0]); return 0; });
    // GMEM_FIXED memory is its own handle, so lock/unlock are identity.
    GH_X86_BED(R, "kernel32.dll", "GlobalLock", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return a[0]; });
    GH_X86_BED(R, "kernel32.dll", "GlobalUnlock", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });

    GH_X86_BED(R, "kernel32.dll", "VirtualAllocEx", 20,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (a[1]) return a[1];
            return m.alloc(a[2] ? a[2] : 0x1000);
        });

    // -- pointer obfuscation --------------------------------------------------
    // The host's EncodePointer rotates against a 64-bit process cookie, so its
    // result does not fit in 32 bits. The guest only requires that Decode undoes
    // Encode, so a 32-bit cookie satisfies the contract.
    GH_X86_BED(R, "kernel32.dll", "EncodePointer", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return a[0] ^ 0x5A3C96E1u; });
    GH_X86_BED(R, "kernel32.dll", "DecodePointer", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return a[0] ^ 0x5A3C96E1u; });
    GH_X86_BED(R, "kernel32.dll", "EncodeSystemPointer", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return a[0] ^ 0x5A3C96E1u; });
    GH_X86_BED(R, "kernel32.dll", "DecodeSystemPointer", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return a[0] ^ 0x5A3C96E1u; });

    // -- CRITICAL_SECTION: a STRUCT-LAYOUT hazard, not a pointer one -----------
    // sizeof(CRITICAL_SECTION) is 24 bytes on x86 and 40 on x64. Forwarding
    // these would have the host write 40 bytes into the guest's 24-byte object
    // and corrupt whatever followed it -- and unlike a bad pointer RETURN, the
    // dispatcher's 4GB check cannot see that happen. Any API whose struct
    // contains a pointer or SIZE_T needs this treatment (STARTUPINFO, OVERLAPPED,
    // SYSTEM_INFO, WIN32_FIND_DATA ...).
    //
    // The fix is cheap: keep a real host CRITICAL_SECTION in GUEST memory (the
    // host API is happy to operate on any address) and stash its address in the
    // guest object's first dword.
    GH_X86_BED(R, "kernel32.dll", "InitializeCriticalSection", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            uint32_t p = m.heap_alloc(sizeof(GuestCrit), true);
            if (p) std::memcpy((void*)(uintptr_t)a[0], &p, 4);
            return 0;
        });
    GH_X86_BED(R, "kernel32.dll", "InitializeCriticalSectionAndSpinCount", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            uint32_t p = m.heap_alloc(sizeof(GuestCrit), true);
            if (!p) return 0;
            std::memcpy((void*)(uintptr_t)a[0], &p, 4);
            return 1;
        });
    GH_X86_BED(R, "kernel32.dll", "InitializeCriticalSectionEx", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            uint32_t p = m.heap_alloc(sizeof(GuestCrit), true);
            if (!p) return 0;
            std::memcpy((void*)(uintptr_t)a[0], &p, 4);
            return 1;
        });
    GH_X86_BED(R, "kernel32.dll", "EnterCriticalSection", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            uint32_t p; std::memcpy(&p, (void*)(uintptr_t)a[0], 4);
            if (p) crit_enter(m, p);
            return 0;
        });
    GH_X86_BED(R, "kernel32.dll", "LeaveCriticalSection", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            uint32_t p; std::memcpy(&p, (void*)(uintptr_t)a[0], 4);
            if (p) crit_leave(m, p);
            return 0;
        });
    GH_X86_BED(R, "kernel32.dll", "TryEnterCriticalSection", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            uint32_t p; std::memcpy(&p, (void*)(uintptr_t)a[0], 4);
            return p ? (uint64_t)crit_try_enter(m, p) : 1;
        });
    GH_X86_BED(R, "kernel32.dll", "DeleteCriticalSection", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            uint32_t p; std::memcpy(&p, (void*)(uintptr_t)a[0], 4);
            if (p) m.heap_free(p);
            return 0;
        });

    // -- atomics -------------------------------------------------------------
    // Interlocked* are intrinsics on x64, not exported functions, so they cannot
    // be forwarded. Implementing them directly is both easy and correct: the
    // guest's memory IS host memory, so a host atomic on the same address has
    // exactly the semantics the guest expects -- which will matter as soon as
    // there is more than one guest thread.
    GH_X86_BED(R, "kernel32.dll", "InterlockedIncrement", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)InterlockedIncrement((volatile LONG*)(uintptr_t)a[0]);
        });
    GH_X86_BED(R, "kernel32.dll", "InterlockedDecrement", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)InterlockedDecrement((volatile LONG*)(uintptr_t)a[0]);
        });
    GH_X86_BED(R, "kernel32.dll", "InterlockedExchange", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)InterlockedExchange((volatile LONG*)(uintptr_t)a[0], (LONG)a[1]);
        });
    GH_X86_BED(R, "kernel32.dll", "InterlockedExchangeAdd", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)InterlockedExchangeAdd((volatile LONG*)(uintptr_t)a[0], (LONG)a[1]);
        });
    GH_X86_BED(R, "kernel32.dll", "InterlockedCompareExchange", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)InterlockedCompareExchange((volatile LONG*)(uintptr_t)a[0],
                                                        (LONG)a[1], (LONG)a[2]);
        });

    // -- thread-local storage ------------------------------------------------
    // Host TLS is exactly wrong here. Every guest thread runs on ONE host
    // thread, so TlsSetValue backed by the host would give all of them a single
    // shared slot -- and TLS is the one thing whose entire purpose is not to be
    // shared. mono keeps its per-thread runtime state here, so this would fail
    // in the least obvious way possible.
    //
    // The slots live where Windows puts them, at TEB32+0xE10, so a guest that
    // reads them directly (the MSVC CRT does) agrees with the API.
    GH_X86_BED(R, "kernel32.dll", "TlsAlloc", 0,
        [](Machine& m, const uint32_t*) -> uint64_t {
            for (uint32_t i = 0; i < Machine::kTlsSlots; ++i)
                if (!m.tls_used[i]) {
                    m.tls_used[i] = true;
                    // A freshly allocated index reads as zero in EVERY thread,
                    // including ones that already exist.
                    for (auto& t : m.threads)
                        std::memset((void*)(uintptr_t)(t.teb + Machine::kTlsBase + i * 4), 0, 4);
                    return i;
                }
            log("X86 TlsAlloc: out of slots");
            SetLastError(ERROR_NO_MORE_ITEMS);
            return 0xFFFFFFFFu;                       // TLS_OUT_OF_INDEXES
        });
    GH_X86_BED(R, "kernel32.dll", "TlsFree", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (a[0] >= Machine::kTlsSlots) return 0;
            m.tls_used[a[0]] = false;
            return 1;
        });
    GH_X86_BED(R, "kernel32.dll", "TlsGetValue", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (a[0] >= Machine::kTlsSlots) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
            uint32_t v;
            std::memcpy(&v, (void*)(uintptr_t)(m.teb + Machine::kTlsBase + a[0] * 4), 4);
            SetLastError(0);
            return v;
        });
    GH_X86_BED(R, "kernel32.dll", "TlsSetValue", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (a[0] >= Machine::kTlsSlots) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
            std::memcpy((void*)(uintptr_t)(m.teb + Machine::kTlsBase + a[0] * 4), &a[1], 4);
            return 1;
        });

    // -- fiber-local storage -------------------------------------------------
    // Both the UCRT and mono probe for these by name and fall back when they are
    // missing, so the guest survives without them -- but the fallback is the
    // slow path, and mono in particular uses FLS for its per-thread runtime
    // state. With one fiber per thread they are the same thing, so they share
    // the TLS slots. The destructor callback is deliberately dropped: it is
    // guest code, and nothing here is in a position to call it at the right
    // moment.
    GH_X86_BED(R, "kernel32.dll", "FlsAlloc", 4,
        [](Machine& m, const uint32_t*) -> uint64_t {
            const HostFn* f = m.registry.find("kernel32.dll", "TlsAlloc");
            return f ? f->invoke(m, nullptr) : 0xFFFFFFFFu;
        });
    GH_X86_BED(R, "kernel32.dll", "FlsFree", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            const HostFn* f = m.registry.find("kernel32.dll", "TlsFree");
            return f ? f->invoke(m, a) : 0;
        });
    GH_X86_BED(R, "kernel32.dll", "FlsGetValue", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            const HostFn* f = m.registry.find("kernel32.dll", "TlsGetValue");
            return f ? f->invoke(m, a) : 0;
        });
    GH_X86_BED(R, "kernel32.dll", "FlsSetValue", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            const HostFn* f = m.registry.find("kernel32.dll", "TlsSetValue");
            return f ? f->invoke(m, a) : 0;
        });

    // -- VirtualQuery: 28 bytes on x86, 48 on x64 -----------------------------
    // Forwarded, the host sees dwLength=28 for a 48-byte structure and returns
    // 0 without writing anything. mono's Boehm collector uses this to find the
    // stack bounds, decides the answer is impossible, prints "Fatal error in
    // gc: Weird VirtualQuery result" and breaks into the debugger -- which is a
    // remarkably clear message for a failure three layers away from its cause.
    //
    // Answered from our own bookkeeping when the address is inside the guest
    // region, which is nearly always -- see Machine::guest_region_query for why
    // that is worth half the startup time rather than a micro-optimisation.
    GH_X86_BED(R, "kernel32.dll", "VirtualQuery", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[1] || a[2] < 28) return 0;
            {
                uint32_t f[7];
                if (m.guest_region_query(a[0], f)) {
                    std::memcpy((void*)(uintptr_t)a[1], f, sizeof(f));
                    return 28;
                }
            }
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery((LPCVOID)(uintptr_t)a[0], &mbi, sizeof(mbi))) return 0;
            auto lo32 = [](uint64_t v) -> uint32_t {
                return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v;
            };
            uint32_t f[7] = {
                lo32((uint64_t)(uintptr_t)mbi.BaseAddress),
                lo32((uint64_t)(uintptr_t)mbi.AllocationBase),
                mbi.AllocationProtect,
                lo32((uint64_t)mbi.RegionSize),
                mbi.State, mbi.Protect, mbi.Type,
            };
            std::memcpy((void*)(uintptr_t)a[1], f, sizeof(f));
            return 28;                                   // the x86 sizeof, as promised
        });
    GH_X86_BED(R, "kernel32.dll", "VirtualQueryEx", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            const uint32_t sub[3] = { a[1], a[2], a[3] };   // drop the process handle
            const HostFn* vq = m.registry.find("kernel32.dll", "VirtualQuery");
            return vq ? vq->invoke(m, sub) : 0;
        });

    // A guest that puts up a message box is telling us why it is about to die,
    // and on this console nobody will ever see it. Into the log it goes.
    GH_X86_BED(R, "user32.dll", "MessageBoxA", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            log(std::string("X86 MessageBox: [") +
                (a[2] ? (const char*)(uintptr_t)a[2] : "") + "] " +
                (a[1] ? (const char*)(uintptr_t)a[1] : ""));
            return 1;                                    // IDOK
        });
    GH_X86_BED(R, "user32.dll", "MessageBoxW", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            auto narrow = [](uint32_t p) -> std::string {
                std::string s;
                for (const wchar_t* w = (const wchar_t*)(uintptr_t)p; p && *w; ++w)
                    s += (char)(*w < 128 ? *w : '?');
                return s;
            };
            log("X86 MessageBox: [" + narrow(a[2]) + "] " + narrow(a[1]));
            return 1;
        });

    // -- pseudo-handles -------------------------------------------------------
    // GetCurrentProcess does not return a handle at all -- it returns the
    // constant -1, which on x64 is 0xFFFFFFFFFFFFFFFF and on x86 is 0xFFFFFFFF.
    // Forwarded, it trips the dispatcher's above-4GB check, which is the check
    // working correctly on a value that is not actually a pointer. The right
    // answer is the same constant at the guest's width.
    GH_X86_BED(R, "kernel32.dll", "GetCurrentProcess", 0,
        [](Machine&, const uint32_t*) -> uint64_t { return 0xFFFFFFFFu; });
    GH_X86_BED(R, "kernel32.dll", "GetCurrentThread", 0,
        [](Machine&, const uint32_t*) -> uint64_t { return 0xFFFFFFFEu; });

    // -- structs whose 32-bit layout is NOT the 64-bit one --------------------
    // The CRITICAL_SECTION note above generalises: any struct holding a pointer
    // or a SIZE_T is a different shape on the two sides, and forwarding it lets
    // the host scribble past the end of the guest's object. Nothing catches
    // that -- not the 4GB return check, not the guest, not immediately. These
    // are written out field by field against the x86 layout instead.
    //
    // Nothing here queries the host for a shape it could not represent: the
    // address-space answers in particular are deliberately the GUEST's, since
    // the host's would not fit in the field they go in.

    // STARTUPINFO: 68 bytes on x86, 104 on x64. The CRT reads lpReserved2 /
    // cbReserved2 during lowio init to pick up inherited stdio handles, so a
    // wrong shape here is read as a garbage handle table -- the prime suspect
    // for a CRT that aborts before main().
    GH_X86_BED(R, "kernel32.dll", "GetStartupInfoA", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { fill_startupinfo32(a[0]); return 0; });
    GH_X86_BED(R, "kernel32.dll", "GetStartupInfoW", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { fill_startupinfo32(a[0]); return 0; });

    GH_X86_BED(R, "kernel32.dll", "GetSystemInfo", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { fill_sysinfo32(a[0]); return 0; });
    GH_X86_BED(R, "kernel32.dll", "GetNativeSystemInfo", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { fill_sysinfo32(a[0]); return 0; });

    // MEMORYSTATUS is six SIZE_T fields: 32 bytes on x86, 56 on x64. Values are
    // clamped below 2GB so that a guest doing signed arithmetic on them -- which
    // is exactly why Microsoft deprecated this API -- does not see a negative
    // number. (MEMORYSTATUSEX is all DWORDLONG, identical on both sides, so it
    // stays a forward.)
    GH_X86_BED(R, "kernel32.dll", "GlobalMemoryStatus", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (!a[0]) return 0;
            MEMORYSTATUSEX mx{}; mx.dwLength = sizeof(mx);
            GlobalMemoryStatusEx(&mx);
            auto clamp = [](DWORDLONG v) -> uint32_t {
                return v > 0x7FF00000ull ? 0x7FF00000u : (uint32_t)v;
            };
            uint8_t* d = (uint8_t*)(uintptr_t)a[0];
            uint32_t f[8] = { 32, (uint32_t)mx.dwMemoryLoad,
                              clamp(mx.ullTotalPhys),     clamp(mx.ullAvailPhys),
                              clamp(mx.ullTotalPageFile), clamp(mx.ullAvailPageFile),
                              clamp(mx.ullTotalVirtual),  clamp(mx.ullAvailVirtual) };
            std::memcpy(d, f, sizeof(f));
            return 0;
        });

    // DuplicateHandle: the bug that proved the third width trap is real.
    //
    // `lpTargetHandle` is a HANDLE* -- void** -- and the host writes eight bytes
    // through it. The guest allocated four. In the MSVC CRT's `dup2` the
    // out-parameter is `&[ebp-4]`, which puts the handle's (always zero) high
    // half exactly on top of the saved EBP at `[ebp]`; `leave` then pops a null
    // frame pointer and the CALLER faults on its next `mov [ebp-0x1c],eax`.
    // Nothing in that fault points here -- DuplicateHandle returned TRUE, the
    // handle it produced was correct, and the call site is a different function
    // 0x24F bytes away. It took 140 million instructions of startup to reach and
    // a callee-saved audit to name.
    //
    // Handles fit in 32 bits by definition, so truncating is safe; what is not
    // safe is letting the host choose how wide to write. See
    // ArgLayout::has_ptr_to_ptr_sized for the audit that now lists the rest.
    GH_X86_BED(R, "kernel32.dll", "DuplicateHandle", 28,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HANDLE out = nullptr;
            const BOOL rc = DuplicateHandle((HANDLE)(intptr_t)(int32_t)a[0],
                                            (HANDLE)(intptr_t)(int32_t)a[1],
                                            (HANDLE)(intptr_t)(int32_t)a[2],
                                            a[3] ? &out : nullptr,
                                            a[4], (BOOL)a[5], a[6]);
            // Four bytes, and only four. The guest's neighbouring stack slot is
            // none of this function's business.
            if (a[3]) *(uint32_t*)(uintptr_t)a[3] = (uint32_t)(uintptr_t)out;
            return (uint32_t)rc;
        });

    // WSADATA does not merely change size across bitness -- it changes FIELD
    // ORDER. The x64 header hoists iMaxSockets/iMaxUdpDg/lpVendorInfo ahead of
    // the two character arrays; the x86 header puts them after. A forward would
    // write the description string straight over the guest's version words.
    GH_X86_BED(R, "ws2_32.dll", "WSAStartup", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            WSADATA hd{};
            int rc = WSAStartup((WORD)a[0], &hd);
            if (a[1]) {
                uint8_t* d = (uint8_t*)(uintptr_t)a[1];
                std::memset(d, 0, 400);                  // sizeof(WSADATA) on x86
                std::memcpy(d + 0,   &hd.wVersion, 2);
                std::memcpy(d + 2,   &hd.wHighVersion, 2);
                std::memcpy(d + 4,   hd.szDescription, 257);
                std::memcpy(d + 261, hd.szSystemStatus, 129);
                std::memcpy(d + 390, &hd.iMaxSockets, 2);
                std::memcpy(d + 392, &hd.iMaxUdpDg, 2);
                // lpVendorInfo at +396 stays NULL: it is a host pointer.
            }
            return (uint32_t)rc;
        });

    // OVERLAPPED is 20 bytes on x86 and 32 on x64, so an overlapped request is
    // carried out by overlapped_io32 (completed before the call returns, at the
    // guest's offset) and never reaches the host with the guest's struct.
    GH_X86_BED(R, "kernel32.dll", "ReadFile", 20,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (a[4]) return overlapped_io32(m, a, false);
            DWORD got = 0;
            const BOOL rc = ReadFile((HANDLE)(uintptr_t)a[0], (LPVOID)(uintptr_t)a[1],
                                     a[2], &got, nullptr);
            if (a[3]) std::memcpy((void*)(uintptr_t)a[3], &got, 4);
            // Only while the file-progress instrument is armed: this is an
            // extra syscall per read, and the position is the whole point --
            // bytes alone cannot tell a reader advancing through an archive
            // from one seeking back to re-read the same block.
            if (rc && m.window_prof) {
                LARGE_INTEGER zero{}, pos{};
                if (!SetFilePointerEx((HANDLE)(uintptr_t)a[0], zero, &pos, FILE_CURRENT))
                    pos.QuadPart = -1;
                m.note_file_read(a[0], got, pos.QuadPart);
            }
            return (uint32_t)rc;
        });
    GH_X86_BED(R, "kernel32.dll", "WriteFile", 20,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (a[4]) return overlapped_io32(m, a, true);
            // A short, printable write is almost always the guest telling us
            // what went wrong -- Unity's output_log and mono's own diagnostics
            // both come through here, and on this console nobody will ever open
            // those files. Mirroring them into loader_diag turns the guest's
            // error messages into ours, which is worth far more than the noise.
            if (a[1] && a[2] && a[2] <= 512) {
                const unsigned char* p = (const unsigned char*)(uintptr_t)a[1];
                std::string txt; bool printable = true;
                for (uint32_t i = 0; i < a[2] && printable; ++i) {
                    unsigned char c = p[i];
                    if (c == '\r') continue;
                    if (c == '\n') { txt += ' '; continue; }
                    if (c < 0x20 || c > 0x7E) printable = false;
                    else txt += (char)c;
                }
                if (printable && txt.size() > 2) log("X86 guest wrote: " + txt);
            }
            return (uint32_t)WriteFile((HANDLE)(uintptr_t)a[0], (LPCVOID)(uintptr_t)a[1],
                                       a[2], (LPDWORD)(uintptr_t)a[3], nullptr);
        });

    // -- identity: the guest must see ITS OWN image, not the host's -----------
    // GetModuleFileName forwarded would answer with the loader's own exe for
    // NULL, and fail outright for a synthetic module handle. Unity derives the
    // whole _Data directory from this string, so a wrong answer here is a game
    // that cannot find its own assets.
    GH_X86_BED(R, "kernel32.dll", "GetModuleFileNameW", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            std::wstring p = module_path32(m, a[0]);
            if (!a[1] || !a[2]) return 0;
            uint32_t n = (uint32_t)p.size();
            if (n >= a[2]) { n = a[2] - 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); }
            wchar_t* d = (wchar_t*)(uintptr_t)a[1];
            std::memcpy(d, p.c_str(), n * 2);
            d[n] = 0;
            return n;
        });
    GH_X86_BED(R, "kernel32.dll", "GetModuleFileNameA", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            std::wstring p = module_path32(m, a[0]);
            if (!a[1] || !a[2]) return 0;
            char* d = (char*)(uintptr_t)a[1];
            int n = WideCharToMultiByte(CP_ACP, 0, p.c_str(), (int)p.size(),
                                        d, (int)a[2] - 1, nullptr, nullptr);
            if (n < 0) n = 0;
            d[n] = 0;
            return (uint32_t)n;
        });

    // -- guest callbacks the host must never be handed directly ---------------
    // These take a function pointer INTO GUEST CODE. Forwarding one would give
    // the host x64 CPU a 32-bit address to jump to, which is the same class of
    // mistake as handing the guest a 64-bit pointer -- just in the other
    // direction, and fatal rather than merely wrong. The filter is remembered
    // so M3's exception dispatch can call it through call_guest32.
    GH_X86_BED(R, "kernel32.dll", "SetUnhandledExceptionFilter", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            uint32_t prev = m.bed.unhandled_filter;
            m.bed.unhandled_filter = a[0];
            return prev;
        });

    // -- threads (not yet; but never as a forward) ---------------------------
    // lpStartAddress is GUEST code. Forwarding this starts a real host thread
    // whose first instruction is 32-bit x86 at a sub-4GB address, and the
    // process dies with an access violation inside the host -- which is exactly
    // what happened in mono's GC startup. Until the interpreter can schedule a
    // second guest context (M3b), the honest answer is failure with a named
    // reason, so the guest reports something about threads rather than the host
    // reporting nothing at all.
    // (lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter,
    //  dwCreationFlags, lpThreadId)
    GH_X86_BED(R, "kernel32.dll", "CreateThread", 24,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            const bool suspended = (a[4] & 0x4) != 0;          // CREATE_SUSPENDED
            GuestThread* t = m.spawn_thread(a[2], a[3], a[1], suspended);
            if (!t) {
                log("X86 CreateThread(start=" + hex(a[2]) + ") FAILED -- out of guest memory");
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
            if (a[5]) { uint32_t id = t->id; std::memcpy((void*)(uintptr_t)a[5], &id, 4); }
            log("X86 CreateThread -> tid " + std::to_string(t->id) +
                " start=" + hex(a[2]) + " stack=" + hex(t->stack_limit) + "-" + hex(t->stack_top) +
                (suspended ? " (suspended)" : ""));
            return t->handle;
        });
    GH_X86_BED(R, "kernel32.dll", "ExitThread", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            m.exit_current(a[0]);
            return 0;
        });
    GH_X86_BED(R, "kernel32.dll", "GetCurrentThreadId", 0,
        [](Machine& m, const uint32_t*) -> uint64_t { return m.cur_tid(); });
    // The real pseudo-handle, sign-extended by rule 5. It never names a slot in
    // the thread table, so anything that can take it resolves it explicitly.
    GH_X86_BED(R, "kernel32.dll", "GetCurrentThread", 0,
        [](Machine&, const uint32_t*) -> uint64_t { return 0xFFFFFFFEu; });
    GH_X86_BED(R, "kernel32.dll", "GetThreadId", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            GuestThread* t = m.thread_by_handle(a[0]);
            return t ? t->id : (a[0] == 0xFFFFFFFEu ? m.cur_tid() : 0);
        });
    GH_X86_BED(R, "kernel32.dll", "GetExitCodeThread", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            GuestThread* t = m.thread_by_handle(a[0]);
            if (!t || !a[1]) return 0;
            uint32_t v = (t->state == GuestThread::St::Done) ? t->exit_code : 0x103u; // STILL_ACTIVE
            std::memcpy((void*)(uintptr_t)a[1], &v, 4);
            return 1;
        });
    GH_X86_BED(R, "kernel32.dll", "ResumeThread", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            GuestThread* t = m.thread_by_handle(a[0]);
            if (!t) return 0xFFFFFFFFu;
            uint32_t prev = (uint32_t)(t->suspend_count > 0 ? t->suspend_count : 0);
            if (t->suspend_count > 0 && --t->suspend_count == 0 &&
                t->state == GuestThread::St::Blocked && t->wait_on.empty())
                t->state = GuestThread::St::Ready;
            return prev;
        });
    GH_X86_BED(R, "kernel32.dll", "SuspendThread", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            GuestThread* t = m.thread_by_handle(a[0]);
            if (!t) return 0xFFFFFFFFu;
            uint32_t prev = (uint32_t)t->suspend_count;
            t->suspend_count++;
            if (t->state == GuestThread::St::Ready) t->state = GuestThread::St::Blocked;
            return prev;
        });
    // Priorities are meaningless to a round-robin scheduler, but a guest that
    // cannot set one sometimes decides something is badly wrong. Accept and
    // ignore.
    GH_X86_BED(R, "kernel32.dll", "SetThreadPriority", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    GH_X86_BED(R, "kernel32.dll", "GetThreadPriority", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });   // NORMAL

    // -- yielding ------------------------------------------------------------
    // Sleep CANNOT be a forward. Every guest thread lives on this one host
    // thread, so a host Sleep stops all of them -- and a thread sleeping while
    // waiting for another to make progress would never wake up to see it.
    GH_X86_BED(R, "kernel32.dll", "Sleep", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t { m.yield_current(a[0]); return 0; });
    GH_X86_BED(R, "kernel32.dll", "SleepEx", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t { m.yield_current(a[0]); return 0; });
    GH_X86_BED(R, "kernel32.dll", "SwitchToThread", 0,
        [](Machine& m, const uint32_t*) -> uint64_t {
            size_t before = m.threads.size();
            m.yield_current(0);
            return before > 1 ? 1 : 0;
        });

    // -- waits ---------------------------------------------------------------
    GH_X86_BED(R, "kernel32.dll", "WaitForSingleObject", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            uint32_t h = a[0];
            return m.wait_current(&h, 1, false, a[1]);
        });
    GH_X86_BED(R, "kernel32.dll", "WaitForSingleObjectEx", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            uint32_t h = a[0];
            return m.wait_current(&h, 1, false, a[1]);
        });
    GH_X86_BED(R, "kernel32.dll", "WaitForMultipleObjects", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            const uint32_t n = a[0];
            if (!n || n > 64 || !a[1]) return Machine::kWaitFailed;
            uint32_t h[64];
            std::memcpy(h, (void*)(uintptr_t)a[1], n * 4);
            return m.wait_current(h, (int)n, a[2] != 0, a[3]);
        });
    GH_X86_BED(R, "kernel32.dll", "WaitForMultipleObjectsEx", 20,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            const uint32_t n = a[0];
            if (!n || n > 64 || !a[1]) return Machine::kWaitFailed;
            uint32_t h[64];
            std::memcpy(h, (void*)(uintptr_t)a[1], n * 4);
            return m.wait_current(h, (int)n, a[2] != 0, a[3]);
        });

    // -- events / mutexes / semaphores ---------------------------------------
    // Modelled here rather than forwarded for the same reason as Sleep: a host
    // object can only be signalled by a host thread, and there is only one.
    GH_X86_BED(R, "kernel32.dll", "CreateEventW", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return make_event32(m, a[1], a[2],
                                a[3] ? std::wstring((const wchar_t*)(uintptr_t)a[3]) : std::wstring());
        });
    GH_X86_BED(R, "kernel32.dll", "CreateEventA", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return make_event32(m, a[1], a[2],
                                a[3] ? widen((const char*)(uintptr_t)a[3]) : std::wstring());
        });
    GH_X86_BED(R, "kernel32.dll", "SetEvent", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            SyncObj* o = m.sync_by_handle(a[0]);
            if (!o) return 0;
            o->signalled = true;
            return 1;
        });
    GH_X86_BED(R, "kernel32.dll", "ResetEvent", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            SyncObj* o = m.sync_by_handle(a[0]);
            if (!o) return 0;
            o->signalled = false;
            return 1;
        });
    // Releases everyone waiting right now, then clears. Our waiters are only
    // re-examined at a scheduling point, so "release all waiters" is exactly
    // "signal, let the scheduler run, reset" -- which wake_ready does for us.
    GH_X86_BED(R, "kernel32.dll", "PulseEvent", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            SyncObj* o = m.sync_by_handle(a[0]);
            if (!o) return 0;
            o->signalled = true;
            m.wake_ready();
            o->signalled = false;
            return 1;
        });
    GH_X86_BED(R, "kernel32.dll", "CreateMutexW", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return make_mutex32(m, a[1],
                                a[2] ? std::wstring((const wchar_t*)(uintptr_t)a[2]) : std::wstring());
        });
    GH_X86_BED(R, "kernel32.dll", "CreateMutexA", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return make_mutex32(m, a[1],
                                a[2] ? widen((const char*)(uintptr_t)a[2]) : std::wstring());
        });
    GH_X86_BED(R, "kernel32.dll", "ReleaseMutex", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            SyncObj* o = m.sync_by_handle(a[0]);
            if (!o || o->owner != m.cur_tid()) return 0;
            if (--o->recursion <= 0) { o->recursion = 0; o->owner = 0; }
            return 1;
        });
    GH_X86_BED(R, "kernel32.dll", "CreateSemaphoreW", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return make_sem32(m, (int32_t)a[1], (int32_t)a[2],
                              a[3] ? std::wstring((const wchar_t*)(uintptr_t)a[3]) : std::wstring());
        });
    GH_X86_BED(R, "kernel32.dll", "CreateSemaphoreA", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return make_sem32(m, (int32_t)a[1], (int32_t)a[2],
                              a[3] ? widen((const char*)(uintptr_t)a[3]) : std::wstring());
        });
    GH_X86_BED(R, "kernel32.dll", "ReleaseSemaphore", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            SyncObj* o = m.sync_by_handle(a[0]);
            if (!o) return 0;
            int32_t prev = o->count;
            o->count += (int32_t)a[1];
            if (o->max_count && o->count > o->max_count) o->count = o->max_count;
            if (a[2]) std::memcpy((void*)(uintptr_t)a[2], &prev, 4);
            return 1;
        });

    // -- file mappings -------------------------------------------------------
    // A view is guest memory holding a copy of the mapped range; see the note
    // on BedState::Mapping for why a real mapping cannot be handed back.
    {
        auto create_mapping = [](Machine& m, const uint32_t* a) -> uint64_t {
            HANDLE src = (HANDLE)(intptr_t)(int32_t)a[0];       // may be INVALID_HANDLE_VALUE
            uint64_t want = ((uint64_t)a[3] << 32) | a[4];

            BedState::Mapping mp;
            mp.protect = a[2];
            if (src && src != INVALID_HANDLE_VALUE) {
                // Duplicated, because the guest is free to close its own handle
                // while the mapping is still alive -- which is exactly what a
                // loader does after mapping an image.
                if (!DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(),
                                     &mp.file, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                    log("X86 CreateFileMapping: DuplicateHandle failed err=" +
                        std::to_string(GetLastError()));
                    return 0;
                }
                LARGE_INTEGER sz{};
                if (!GetFileSizeEx(mp.file, &sz)) { CloseHandle(mp.file); return 0; }
                mp.size = want ? want : (uint64_t)sz.QuadPart;
                if (!mp.size) {                       // a zero-length file cannot be mapped
                    CloseHandle(mp.file);
                    SetLastError(ERROR_FILE_INVALID);
                    return 0;
                }
            } else {
                if (!want) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
                mp.size = want;
            }
            mp.handle = kMappingTag | (++m.bed.next_mapping & 0xFFFFu);
            m.bed.mappings.push_back(mp);
            return mp.handle;
        };
        GH_X86_BED(R, "kernel32.dll", "CreateFileMappingW", 24, create_mapping);
        GH_X86_BED(R, "kernel32.dll", "CreateFileMappingA", 24, create_mapping);

        auto map_view = [](Machine& m, const uint32_t* a) -> uint64_t {
            BedState::Mapping* mp = mapping_of(m, a[0]);
            if (!mp) { SetLastError(ERROR_INVALID_HANDLE); return 0; }

            uint64_t off = ((uint64_t)a[2] << 32) | a[3];
            if (off >= mp->size) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
            uint64_t avail = mp->size - off;
            uint64_t bytes = a[4] ? a[4] : avail;
            if (bytes > avail) bytes = avail;

            uint32_t base = m.heap_alloc((uint32_t)bytes, true);   // zeroed
            if (!base) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }

            if (mp->file) {
                LARGE_INTEGER at{}; at.QuadPart = (LONGLONG)off;
                if (!SetFilePointerEx(mp->file, at, nullptr, FILE_BEGIN)) {
                    m.heap_free(base); return 0;
                }
                uint32_t done = 0;
                while (done < bytes) {
                    DWORD got = 0;
                    if (!ReadFile(mp->file, (void*)(uintptr_t)(base + done),
                                  (DWORD)(bytes - done), &got, nullptr) || !got) break;
                    done += got;
                }
                if (done < bytes)
                    log("X86 MapViewOfFile: short read " + std::to_string(done) +
                        "/" + std::to_string(bytes));
            }

            const uint32_t kWrite = 0x0002 /*FILE_MAP_WRITE*/ | 0x0020 /*FILE_MAP_EXECUTE*/;
            m.bed.views.push_back(BedState::View{ base, (uint32_t)bytes, mp->handle, off,
                                                 (a[1] & kWrite) != 0 });
            mp->views++;
            return base;
        };
        GH_X86_BED(R, "kernel32.dll", "MapViewOfFile", 20, map_view);
        // MapViewOfFileEx's lpBaseAddress is a request, not a requirement; we
        // ignore it, which is the documented behaviour when it cannot be met.
        GH_X86_BED(R, "kernel32.dll", "MapViewOfFileEx", 24, map_view);

        GH_X86_BED(R, "kernel32.dll", "UnmapViewOfFile", 4,
            [](Machine& m, const uint32_t* a) -> uint64_t {
                for (size_t i = 0; i < m.bed.views.size(); ++i) {
                    BedState::View v = m.bed.views[i];
                    if (v.base != a[0]) continue;
                    BedState::Mapping* mp = mapping_of(m, v.mapping);
                    if (mp && mp->file && v.writable) {
                        LARGE_INTEGER at{}; at.QuadPart = (LONGLONG)v.offset;
                        DWORD wrote = 0;
                        if (SetFilePointerEx(mp->file, at, nullptr, FILE_BEGIN))
                            WriteFile(mp->file, (void*)(uintptr_t)v.base, v.bytes, &wrote, nullptr);
                    }
                    m.bed.views.erase(m.bed.views.begin() + (ptrdiff_t)i);
                    m.heap_free(v.base);
                    if (mp && --mp->views == 0 && mp->closed) release_mapping(m, *mp);
                    return 1;
                }
                SetLastError(ERROR_INVALID_ADDRESS);
                return 0;
            });
        // Nothing to flush: the view IS the memory, and write-back happens at
        // unmap. Reported as success so a guest checking the result carries on.
        GH_X86_BED(R, "kernel32.dll", "FlushViewOfFile", 8,
            [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    }

    // -- vectored exception handlers -----------------------------------------
    // Beds, not forwards, for the usual reason: the handler is GUEST code, and
    // the host's own VEH would be called on the host stack with a host CONTEXT
    // that means nothing to it. dispatch_guest_exception invokes these instead,
    // ahead of the frame-based chain. The returned cookie only has to be a
    // distinct non-null value the guest can hand back to Remove.
    GH_X86_BED(R, "kernel32.dll", "AddVectoredExceptionHandler", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[1]) return 0;
            uint32_t cookie = ++m.bed.next_veh_cookie;
            m.bed.veh.push_back(BedState::Veh{ cookie, a[1], a[0] != 0 });
            log("X86 AddVectoredExceptionHandler(" + std::string(a[0] ? "first" : "last") +
                ", " + hex(a[1]) + ") -> " + hex(cookie));
            return cookie;
        });
    GH_X86_BED(R, "kernel32.dll", "RemoveVectoredExceptionHandler", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            for (size_t i = 0; i < m.bed.veh.size(); ++i)
                if (m.bed.veh[i].cookie == a[0]) {
                    m.bed.veh.erase(m.bed.veh.begin() + (ptrdiff_t)i);
                    return 1;
                }
            return 0;
        });
    // Continue handlers run only when no exception handler took the exception;
    // nothing in this guest depends on them yet, so they are accepted and never
    // invoked -- which is honest, and better than an unimplemented-import stop.
    GH_X86_BED(R, "kernel32.dll", "AddVectoredContinueHandler", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t { (void)m; return a[1] ? 1 : 0; });
    GH_X86_BED(R, "kernel32.dll", "RemoveVectoredContinueHandler", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });

    // -- the two forwards whose 64-bit arguments really are 64 bits -----------
    // Arg32 assumes an 8-byte integer is pointer-sized, which is right for
    // SIZE_T and wrong for DWORDLONG. These are the only two entry points in
    // the generated table where that matters, so they get explicit pop sizes.
    GH_X86_BED(R, "kernel32.dll", "VerSetConditionMask", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            ULONGLONG mask = (ULONGLONG)a[0] | ((ULONGLONG)a[1] << 32);
            return VerSetConditionMask(mask, (DWORD)a[2], (BYTE)a[3]);
        });
    GH_X86_BED(R, "kernel32.dll", "VerifyVersionInfoW", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            ULONGLONG cond = (ULONGLONG)a[2] | ((ULONGLONG)a[3] << 32);
            return VerifyVersionInfoW((LPOSVERSIONINFOEXW)(uintptr_t)a[0], (DWORD)a[1], cond);
        });
    GH_X86_BED(R, "kernel32.dll", "VerifyVersionInfoA", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            ULONGLONG cond = (ULONGLONG)a[2] | ((ULONGLONG)a[3] << 32);
            return VerifyVersionInfoA((LPOSVERSIONINFOEXA)(uintptr_t)a[0], (DWORD)a[1], cond);
        });

    // GetEnvironmentStrings hands back a pointer, so it allocates from the
    // guest heap. The block is the double-NUL terminated form both ANSI and
    // wide callers expect; the guest is given a minimal, stable environment
    // rather than the container's, which it has no business seeing.
    // The block is built once and never freed. Two reasons, and the second is
    // the one that cost a day: a C runtime is entitled to keep pointers INTO
    // this block rather than copying out of it, and the guest heap will happily
    // hand the same bytes to the next allocation -- after which getenv answers
    // with whatever the guest most recently allocated. An environment that
    // silently becomes something else is a spectacularly confusing failure.
    // Never freeing costs a few dozen bytes for the life of the process.
    GH_X86_BED(R, "kernel32.dll", "GetEnvironmentStringsW", 0,
        [](Machine& m, const uint32_t*) -> uint64_t {
            if (m.bed.env_block_w) return m.bed.env_block_w;
            std::wstring blk;
            for (auto& kv : m.bed.env) { blk += kv.first; blk += L'='; blk += kv.second; blk += L'\0'; }
            blk += L'\0';
            uint32_t n = (uint32_t)(blk.size() + 1) * 2;
            uint32_t p = m.heap_alloc(n, true);
            if (p) std::memcpy((void*)(uintptr_t)p, blk.data(), blk.size() * 2);
            m.bed.env_block_w = p;
            return p;
        });
    GH_X86_BED(R, "kernel32.dll", "GetEnvironmentStrings", 0,
        [](Machine& m, const uint32_t*) -> uint64_t {
            if (m.bed.env_block_a) return m.bed.env_block_a;
            std::string blk;
            for (auto& kv : m.bed.env) {
                for (wchar_t c : kv.first)  blk += (char)(c < 128 ? c : '?');
                blk += '=';
                for (wchar_t c : kv.second) blk += (char)(c < 128 ? c : '?');
                blk += '\0';
            }
            blk += '\0';
            uint32_t n = (uint32_t)blk.size() + 1;
            uint32_t p = m.heap_alloc(n, true);
            if (p) std::memcpy((void*)(uintptr_t)p, blk.data(), blk.size());
            m.bed.env_block_a = p;
            return p;
        });
    GH_X86_BED(R, "kernel32.dll", "FreeEnvironmentStringsW", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });   // deliberately no free
    GH_X86_BED(R, "kernel32.dll", "FreeEnvironmentStringsA", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });

    // -- OpenGL: absent by design, and the guest must be told so ------------
    // The Xbox container has no opengl32.dll (Greenhouse already carries
    // corpus/shims/opengl32_stub.cpp for the x64 guests). A Unity player probes
    // for a GL context, and a clean failure is what makes it fall back to D3D11
    // -- which is the renderer this whole track is aiming at. Leaving these
    // unresolved would instead kill the guest at the probe.
    GH_X86_BED(R, "opengl32.dll", "wglGetCurrentContext", 0,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "opengl32.dll", "wglGetCurrentDC", 0,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "opengl32.dll", "wglCreateContext", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "opengl32.dll", "wglDeleteContext", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    GH_X86_BED(R, "opengl32.dll", "wglMakeCurrent", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "opengl32.dll", "wglGetProcAddress", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });

    // -- HID: report that there are no HID devices --------------------------
    // Unity enumerates raw HID gamepads before falling back to XInput, which is
    // the path that actually works on this console. Failing the enumeration
    // cleanly is therefore the correct answer, not a placeholder.
    // HIDP_STATUS_INVALID_PREPARSED_DATA is the status a caller is required to
    // handle, so it is the safe one to return.
    {
        static const uint32_t kBadPreparsed = 0xC0110001u;
        struct { const char* name; uint32_t pop; bool ntstatus; } hid[] = {
            {"HidD_GetPreparsedData",      8,  false},
            {"HidD_FreePreparsedData",     4,  false},
            {"HidD_GetProductString",     12,  false},
            {"HidD_GetManufacturerString",12,  false},
            {"HidD_GetSerialNumberString",12,  false},
            {"HidD_GetIndexedString",     16,  false},
            {"HidP_GetCaps",               8,  true},
            {"HidP_GetButtonCaps",        16,  true},
            {"HidP_GetValueCaps",         16,  true},
            {"HidP_GetData",              24,  true},
            {"HidP_MaxDataListLength",     8,  false},
        };
        for (auto& e : hid) {
            HostFn f;
            f.dll = "hid.dll"; f.name = e.name; f.stack_bytes = e.pop;
            f.invoke = e.ntstatus
                ? +[](Machine&, const uint32_t*) -> uint64_t { return kBadPreparsed; }
                : +[](Machine&, const uint32_t*) -> uint64_t { return 0; };
            R.add(f);
        }
    }
    GH_X86_BED(R, "hid.dll", "HidD_GetHidGuid", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            // The one HID entry that writes through its pointer. A caller that
            // got no GUID at all would pass uninitialised stack to SetupDi*.
            if (a[0]) std::memset((void*)(uintptr_t)a[0], 0, 16);
            return 0;
        });

    // -- COM: enough to be told "no" ----------------------------------------
    // Real COM is M4 (the vtable thunking problem: a guest cannot hold the
    // 64-bit method pointers a host interface is made of). Until then,
    // initialising succeeds -- guests treat a failed CoInitialize as fatal --
    // and object creation fails with the error every caller already handles.
    GH_X86_BED(R, "ole32.dll", "CoInitialize", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });          // S_OK
    GH_X86_BED(R, "ole32.dll", "CoInitializeEx", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "ole32.dll", "CoUninitialize", 0,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "ole32.dll", "CoCreateInstance", 20,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (uint64_t hr; di8_cocreate(m, a, hr)) return hr;          // gh_x86_input.h
            if (a[4]) { uint32_t z = 0; std::memcpy((void*)(uintptr_t)a[4], &z, 4); }
            return 0x80040154u;                                            // REGDB_E_CLASSNOTREG
        });
    GH_X86_BED(R, "ole32.dll", "CoTaskMemAlloc", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t { return m.heap_alloc(a[0], false); });
    GH_X86_BED(R, "ole32.dll", "CoTaskMemFree", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t { m.heap_free(a[0]); return 0; });

    // -- OLEAUT32: BSTR and VARIANT, both of which change shape --------------
    // A BSTR is a pointer, so it must come from guest memory; a VARIANT is 16
    // bytes on x86 and 24 on x64. Neither could be forwarded even if the host
    // export were reachable. The length prefix lives in the four bytes BEFORE
    // the pointer, which is the whole trick to allocating one.
    GH_X86_BED(R, "oleaut32.dll", "SysAllocString", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            const wchar_t* s = (const wchar_t*)(uintptr_t)a[0];
            if (!s) return 0;
            uint32_t chars = (uint32_t)wcslen(s);
            uint32_t p = m.heap_alloc(4 + (chars + 1) * 2, false);
            if (!p) return 0;
            uint32_t bytes = chars * 2;
            std::memcpy((void*)(uintptr_t)p, &bytes, 4);
            std::memcpy((void*)(uintptr_t)(p + 4), s, bytes + 2);
            return p + 4;
        });
    GH_X86_BED(R, "oleaut32.dll", "SysStringLen", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (!a[0]) return 0;
            uint32_t bytes; std::memcpy(&bytes, (void*)(uintptr_t)(a[0] - 4), 4);
            return bytes / 2;
        });
    GH_X86_BED(R, "oleaut32.dll", "SysFreeString", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (a[0]) m.heap_free(a[0] - 4);
            return 0;
        });
    GH_X86_BED(R, "oleaut32.dll", "VariantInit", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[0]) std::memset((void*)(uintptr_t)a[0], 0, 16);           // sizeof on x86
            return 0;
        });
    GH_X86_BED(R, "oleaut32.dll", "VariantClear", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[0]) std::memset((void*)(uintptr_t)a[0], 0, 16);
            return 0;
        });
    GH_X86_BED(R, "oleaut32.dll", "VariantChangeType", 16,
        [](Machine&, const uint32_t*) -> uint64_t { return 0x80020005u; }); // DISP_E_TYPEMISMATCH

    // -- FormatMessage: a pointer hazard the 4GB check cannot see ------------
    // With FORMAT_MESSAGE_ALLOCATE_BUFFER the lpBuffer argument is not a buffer
    // at all -- it is an LPWSTR* that receives a LocalAlloc'd HOST pointer. That
    // pointer is written straight into the guest's 4-byte variable and silently
    // truncated; the dispatcher's return-value check never sees it because the
    // pointer never passes through the return value. Stick Fight found this by
    // failing a CreateDirectory, formatting the error, and then dereferencing
    // the truncated result.
    //
    // Inserts are refused rather than translated: the host would walk a guest
    // va_list as an array of 64-bit values. IGNORE_INSERTS turns a wrong message
    // into a slightly less specific one, which is the right trade.
    GH_X86_BED(R, "kernel32.dll", "FormatMessageW", 28,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            std::vector<wchar_t> tmp(4096);
            DWORD flags = (a[0] & ~(DWORD)FORMAT_MESSAGE_ALLOCATE_BUFFER)
                          | FORMAT_MESSAGE_IGNORE_INSERTS;
            DWORD n = FormatMessageW(flags, (LPCVOID)(uintptr_t)a[1], a[2], a[3],
                                     tmp.data(), (DWORD)tmp.size(), nullptr);
            if (!n || !a[4]) return n;
            if (a[0] & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
                uint32_t p = m.heap_alloc((n + 1) * 2, false);
                if (!p) return 0;
                std::memcpy((void*)(uintptr_t)p, tmp.data(), (n + 1) * 2);
                std::memcpy((void*)(uintptr_t)a[4], &p, 4);      // lpBuffer is LPWSTR*
                return n;
            }
            if (n >= a[5]) n = a[5] ? a[5] - 1 : 0;
            wchar_t* out = (wchar_t*)(uintptr_t)a[4];
            std::memcpy(out, tmp.data(), n * 2);
            out[n] = 0;
            return n;
        });
    GH_X86_BED(R, "kernel32.dll", "FormatMessageA", 28,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            std::vector<char> tmp(4096);
            DWORD flags = (a[0] & ~(DWORD)FORMAT_MESSAGE_ALLOCATE_BUFFER)
                          | FORMAT_MESSAGE_IGNORE_INSERTS;
            DWORD n = FormatMessageA(flags, (LPCVOID)(uintptr_t)a[1], a[2], a[3],
                                     tmp.data(), (DWORD)tmp.size(), nullptr);
            if (!n || !a[4]) return n;
            if (a[0] & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
                uint32_t p = m.heap_alloc(n + 1, false);
                if (!p) return 0;
                std::memcpy((void*)(uintptr_t)p, tmp.data(), n + 1);
                std::memcpy((void*)(uintptr_t)a[4], &p, 4);
                return n;
            }
            if (n >= a[5]) n = a[5] ? a[5] - 1 : 0;
            char* out = (char*)(uintptr_t)a[4];
            std::memcpy(out, tmp.data(), n);
            out[n] = 0;
            return n;
        });

    // CommandLineToArgvW hands back an array of pointers, so both the array and
    // every string in it have to live in guest memory. The quoting rules are
    // Microsoft's own: a run of backslashes is literal unless it precedes a
    // quote, in which case pairs collapse and an odd one escapes the quote.
    GH_X86_BED(R, "shell32.dll", "CommandLineToArgvW", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            const wchar_t* s = (const wchar_t*)(uintptr_t)a[0];
            std::vector<std::wstring> args;
            if (s) {
                std::wstring cur;
                bool in_arg = false, quoted = false;
                int slashes = 0;
                auto flush_slashes = [&](bool before_quote) {
                    if (before_quote) { cur.append(slashes / 2, L'\\'); }
                    else              { cur.append(slashes, L'\\'); }
                    slashes = 0;
                };
                for (; *s; ++s) {
                    if (*s == L'\\') { slashes++; in_arg = true; continue; }
                    if (*s == L'"') {
                        bool escaped = (slashes % 2) != 0;
                        flush_slashes(true);
                        if (escaped) cur += L'"';
                        else         quoted = !quoted;
                        in_arg = true;
                        continue;
                    }
                    flush_slashes(false);
                    if ((*s == L' ' || *s == L'\t') && !quoted) {
                        if (in_arg) { args.push_back(cur); cur.clear(); in_arg = false; }
                        continue;
                    }
                    cur += *s;
                    in_arg = true;
                }
                flush_slashes(false);
                if (in_arg) args.push_back(cur);
            }
            if (args.empty()) args.push_back(L"guest.exe");

            uint32_t chars = 0;
            for (auto& w : args) chars += (uint32_t)w.size() + 1;
            uint32_t n = (uint32_t)args.size();
            uint32_t block = m.heap_alloc(n * 4 + chars * 2, false);
            if (!block) return 0;

            uint32_t str = block + n * 4;
            for (uint32_t i = 0; i < n; ++i) {
                std::memcpy((void*)(uintptr_t)(block + i * 4), &str, 4);
                uint32_t bytes = (uint32_t)(args[i].size() + 1) * 2;
                std::memcpy((void*)(uintptr_t)str, args[i].c_str(), bytes);
                str += bytes;
            }
            if (a[1]) std::memcpy((void*)(uintptr_t)a[1], &n, 4);
            return block;
        });

    // -- exceptions ----------------------------------------------------------
    // RaiseException MUST be a bed. Forwarded, it raises a HOST exception, which
    // unwinds the interpreter's own C++ frames and never reaches a single guest
    // handler. mono raises one during runtime init; the forward took the whole
    // process down with it.
    GH_X86_BED(R, "kernel32.dll", "RaiseException", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            // Copy the arguments out FIRST. `a` points into the guest's stack
            // just above the current ESP, and everything below is about to be
            // used by the handler this bed is going to call. Reading a[0] after
            // dispatch returns the handler's own arguments -- which is exactly
            // what happened, and cost a deploy chasing an "exception code" that
            // was really an EXCEPTION_RECORD pointer.
            const uint32_t code = a[0], flags = a[1], nargs = a[2], argv = a[3];

            // The set-thread-name exception is a debugger convention and nothing
            // else: dwType 0x1000, a name, a tid. With a debugger attached
            // Windows swallows it; with none and no handler it would take the
            // process down, and a thread created straight through CreateThread
            // has no handler because we do not install the top-level frame that
            // BaseThreadInitThunk gives a real thread. Take the name and move on
            // -- the guest cannot tell the difference, and the log gets better.
            if (code == 0x406D1388u && argv && nargs >= 3) {
                const uint32_t* info = (const uint32_t*)(uintptr_t)argv;
                uint32_t name_ptr = info[1], want_tid = info[2];
                std::string nm = name_ptr ? m.guest_str(name_ptr) : std::string();
                for (auto& t : m.threads)
                    if (t.id == want_tid || want_tid == 0xFFFFFFFFu) {
                        t.name = nm;
                        log("X86 tid " + std::to_string(t.id) + " named \"" + nm + "\"");
                        break;
                    }
                return 0;
            }

            const uint32_t* params = argv ? (const uint32_t*)(uintptr_t)argv : nullptr;
            uint32_t np = params ? nargs : 0;

            // Say that a raise happened at all. Only the UNHANDLED case was
            // logged before, so an exception that a guest frame swallowed left
            // no trace -- and when the process then died for another reason,
            // there was no way to tell whether a throw had been involved.
            // A C++ throw carries the magic, the object and its throw_info.
            if (static int n = 0; n++ < 12)
                log("X86 RaiseException code=" + hex(code) + " flags=" + hex(flags) +
                    " from guest " + hex(m.hostcall_ret) +
                    " on tid " + std::to_string(m.cur_tid()) +
                    (code == 0xE06D7363u && np >= 3
                         ? " -- C++ throw, object " + hex(params[1]) +
                               " throw_info " + hex(params[2])
                         : std::string()));

            // Present the CALLER, not the thunk, as the faulting location and as
            // the state to resume from. A handler that continues execution would
            // otherwise resume at the RaiseException thunk and raise again,
            // forever.
            uint32_t save_eip = m.cpu.eip, save_esp = m.cpu.gpr[REG_ESP];
            m.cpu.eip          = m.hostcall_ret;
            m.cpu.gpr[REG_ESP] = m.hostcall_esp;

            if (m.dispatch_guest_exception(code, flags, np, params)) {
                m.hostcall_took_control = true;          // the handler chose where to resume
                return 0;
            }
            m.cpu.eip = save_eip; m.cpu.gpr[REG_ESP] = save_esp;

            log("X86 unhandled RaiseException code=" + hex(code) +
                " flags=" + hex(flags) + " nargs=" + hex(nargs) + " args=" + hex(argv) +
                " on tid " + std::to_string(m.cur_tid()) +
                " esp=" + hex(m.cpu.gpr[REG_ESP]) +
                " (stack " + hex(m.stack_limit) + "-" + hex(m.stack_top) + ")" +
                " -- no SEH handler took it");
            m.dump_guest_stack("unhandled RaiseException");
            m.exit_requested = true;
            m.exit_code = code;
            return 0;
        });
    // The other half: __except_handler4 calls this to run the __finally blocks
    // between here and the frame that is handling the exception, then jumps to
    // its own except block itself. So returning normally is the contract.
    GH_X86_BED(R, "kernel32.dll", "RtlUnwind", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            m.unwind_seh(a[0], a[2]);
            return 0;
        });

    // UnhandledExceptionFilter takes an EXCEPTION_POINTERS -- two pointers, so
    // 8 bytes on x86 and 16 on x64 -- and the host would walk the guest's pair
    // as 64-bit values. It also has to be able to call the guest's own filter,
    // which is guest code the host cannot invoke at all.
    GH_X86_BED(R, "kernel32.dll", "UnhandledExceptionFilter", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            uint32_t code = 0;
            if (a[0]) {
                uint32_t rec = 0;
                std::memcpy(&rec, (void*)(uintptr_t)a[0], 4);
                if (rec) std::memcpy(&code, (void*)(uintptr_t)rec, 4);
            }
            if (m.bed.unhandled_filter) {
                uint32_t args[1] = { a[0] };
                uint32_t r = m.call_guest32(m.bed.unhandled_filter, args, 1);
                log("X86 unhandled exception " + hex(code) + "; guest filter returned " +
                    std::to_string((int32_t)r));
                return r;
            }
            log("X86 unhandled exception " + hex(code) + "; no guest filter installed");
            m.dump_guest_stack("unhandled exception");
            return 1;                                        // EXCEPTION_EXECUTE_HANDLER
        });

    // -- environment ---------------------------------------------------------
    // Seeded, not inherited. TEMP and TMP matter: mono writes there, and the
    // container's own temp directory is not somewhere a guest path can reach.
    {
        std::wstring dir = m.image_path;
        size_t cut = dir.find_last_of(L'\\');
        dir = (cut == std::wstring::npos) ? std::wstring(L"C:\\") : dir.substr(0, cut);
        env_set(m, L"TEMP", dir);
        env_set(m, L"TMP", dir);
        env_set(m, L"USERPROFILE", dir);
        env_set(m, L"SystemDrive", L"C:");
        env_set(m, L"SystemRoot", L"C:\\Windows");
        env_set(m, L"OS", L"Windows_NT");
        env_set(m, L"NUMBER_OF_PROCESSORS", L"1");   // see kGuestProcessorCount
        env_set(m, L"PROCESSOR_ARCHITECTURE", L"x86");
        // SDL2 prefers its raw-input joysticks to XInput once that driver
        // starts, and it starts with no devices here (gh_x86_input.h).
        env_set(m, L"SDL_JOYSTICK_RAWINPUT", L"0");
    }
    GH_X86_BED(R, "kernel32.dll", "GetEnvironmentVariableW", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0]) return 0;
            std::wstring name = (const wchar_t*)(uintptr_t)a[0];
            const std::wstring* v = env_find(m, name);
            // Logged unconditionally: environment lookups are rare, and which
            // switches a runtime believes it was handed changes what it does.
            // mono asks for MONO_XDEBUG here, and a wrong answer sends it into
            // its AOT image writer -- which this build does not contain.
            std::string nm8; for (wchar_t c : name) nm8 += (char)(c < 128 ? c : '?');
            log("X86 getenv(" + nm8 + ") -> " + (v ? "set" : "not set"));
            if (!v) { SetLastError(ERROR_ENVVAR_NOT_FOUND); return 0; }
            return env_copy_out(*v, a[1], a[2], true);
        });
    GH_X86_BED(R, "kernel32.dll", "GetEnvironmentVariableA", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0]) return 0;
            const char* nm = (const char*)(uintptr_t)a[0];
            const std::wstring* v = env_find(m, widen(nm));
            // Once per name: SDL reads its hints through here on hot paths.
            static std::mutex mu; static std::set<std::string> seen;
            { std::lock_guard<std::mutex> lk(mu);
              if (seen.insert(nm).second) log(std::string("X86 getenvA(") + nm + ") -> " + (v ? "set" : "not set")); }
            if (!v) { SetLastError(ERROR_ENVVAR_NOT_FOUND); return 0; }
            return env_copy_out(*v, a[1], a[2], false);
        });
    GH_X86_BED(R, "kernel32.dll", "SetEnvironmentVariableW", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0]) return 0;
            env_set(m, (const wchar_t*)(uintptr_t)a[0],
                    a[1] ? (const wchar_t*)(uintptr_t)a[1] : L"");
            return 1;
        });
    GH_X86_BED(R, "kernel32.dll", "SetEnvironmentVariableA", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0]) return 0;
            env_set(m, widen((const char*)(uintptr_t)a[0]),
                    a[1] ? widen((const char*)(uintptr_t)a[1]) : L"");
            return 1;
        });
    GH_X86_BED(R, "kernel32.dll", "ExpandEnvironmentStringsW", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0]) return 0;
            std::wstring r = env_expand(m, (const wchar_t*)(uintptr_t)a[0]);
            if (!a[1] || r.size() + 1 > a[2]) return (uint32_t)r.size() + 1;
            std::memcpy((void*)(uintptr_t)a[1], r.c_str(), (r.size() + 1) * 2);
            return (uint32_t)r.size() + 1;
        });
    GH_X86_BED(R, "kernel32.dll", "ExpandEnvironmentStringsA", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0]) return 0;
            std::wstring r = env_expand(m, widen((const char*)(uintptr_t)a[0]));
            if (!a[1] || r.size() + 1 > a[2]) return (uint32_t)r.size() + 1;
            char* d = (char*)(uintptr_t)a[1];
            for (size_t i = 0; i < r.size(); ++i) d[i] = (char)(r[i] < 128 ? r[i] : '?');
            d[r.size()] = 0;
            return (uint32_t)r.size() + 1;
        });

    // -- varargs ------------------------------------------------------------
    // cdecl, not stdcall: a variadic callee cannot know how much to pop, which
    // is exactly why these need their own macro. Getting that wrong would leave
    // the guest's stack pointer off by the argument count on every call.
    GH_X86_BED_CDECL(R, "user32.dll", "wsprintfA",
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (!a[0] || !a[1]) return 0;
            const char* f = (const char*)(uintptr_t)a[1];
            std::wstring wf; while (*f) wf += (wchar_t)(unsigned char)*f++;
            std::wstring r = gh_format32(wf, a + 2);
            char* out = (char*)(uintptr_t)a[0];
            size_t n = 0;
            for (; n < r.size() && n < 1023; ++n) out[n] = (char)r[n];
            out[n] = 0;
            return (uint32_t)n;
        });
    GH_X86_BED_CDECL(R, "user32.dll", "wsprintfW",
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (!a[0] || !a[1]) return 0;
            std::wstring r = gh_format32((const wchar_t*)(uintptr_t)a[1], a + 2);
            if (r.size() > 1023) r.resize(1023);
            wchar_t* out = (wchar_t*)(uintptr_t)a[0];
            std::memcpy(out, r.c_str(), (r.size() + 1) * 2);
            return (uint32_t)r.size();
        });

    // -- the windowing surrogate --------------------------------------------
    // Stick Fight got here on its own and said so: `[Fatal error] Failed to
    // create window`. Until now RegisterClassW, CreateWindowExW and the whole
    // message pump were FORWARDS to the container's real user32 -- on a console
    // that has no desktop window manager at all. The x64 path replaced every one
    // of them with beds years ago (guest_loader.cpp:2264); none of it was ported,
    // and nothing had failed only because no guest had got this far.
    //
    // There is no window. There is a synthetic HWND, a client rect the size of
    // the console's framebuffer, and a queue that delivers one activation
    // sequence and is empty forever after. That is enough for a Unity player,
    // because what it actually wants from the window system is (a) something to
    // hand D3D as an output target and (b) the four messages that flip its
    // `gAppActive` true. Everything else it asks for, it asks defensively.
    //
    // Two traps here that other sections of this file have already paid for:
    //
    //   * WNDCLASS carries `lpfnWndProc` INSIDE the struct, so a forwarded
    //     RegisterClass hands the host a guest code address to call later --
    //     the CreateThread crash exactly (rule 7), arriving by a route the
    //     compile-time callback audit cannot see, because that audit only
    //     inspects top-level parameter types. This is the known hole; the bed
    //     closes this instance of it.
    //   * WNDCLASSW is 40 bytes on x86 and 72 on x64; WNDCLASSEXW is 48 and 80;
    //     MSG is 28 and 48. Rule 3, three more times. Every one is read and
    //     written field by field at its x86 offset rather than as a host struct.

    // x86 WNDCLASSW:   style 0, lpfnWndProc 4  ... 40 bytes
    // x86 WNDCLASSEXW: cbSize 0, style 4, lpfnWndProc 8 ... 48 bytes
    // A class is remembered by name (and by the atom we hand back, which is how
    // CreateWindowEx may name it). Until the game window exists the newest
    // class procedure is also the dispatch target, which is all a Unity player
    // ever needed; after that a registration only records -- the window
    // created from the class decides where its messages go.
    // x86 WNDCLASS:   style 0, lpfnWndProc 4 ... lpszClassName 36; 40 bytes
    // x86 WNDCLASSEX: cbSize 0, style 4, lpfnWndProc 8 ... lpszClassName 40; 48 bytes
    GH_X86_BED(R, "user32.dll", "RegisterClassW", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0]) return 0;
            const uint32_t* wc = (const uint32_t*)(uintptr_t)a[0];
            return x86_register_class(m, wc[1], wc[9], true, "RegisterClassW");
        });
    GH_X86_BED(R, "user32.dll", "RegisterClassExW", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0]) return 0;
            const uint32_t* wc = (const uint32_t*)(uintptr_t)a[0];
            return x86_register_class(m, wc[2], wc[10], true, "RegisterClassExW");
        });
    GH_X86_BED(R, "user32.dll", "RegisterClassA", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0]) return 0;
            const uint32_t* wc = (const uint32_t*)(uintptr_t)a[0];
            return x86_register_class(m, wc[1], wc[9], false, "RegisterClassA");
        });
    GH_X86_BED(R, "user32.dll", "RegisterClassExA", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0]) return 0;
            const uint32_t* wc = (const uint32_t*)(uintptr_t)a[0];
            return x86_register_class(m, wc[2], wc[10], false, "RegisterClassExA");
        });
    for (const char* n : { "UnregisterClassW", "UnregisterClassA" })
        GH_X86_BED(R, "user32.dll", n, 8,
            [](Machine&, const uint32_t*) -> uint64_t { return 1; });

    // CreateWindowExW takes 12 arguments; getting that count wrong is a silent
    // 4-byte stack drift per call, so it is asserted like the WS2_32 table.
    //
    // The game window is kFakeHwnd and every guest window that IS the game
    // shares it (HL2 makes one at CW_USEDEFAULT, then the real 1920x1080 one).
    // A child, a message-only window, or a small window made once the game
    // window exists is a SubWindow with an HWND of its own.
    static_assert(thunk_bytes_of(&::CreateWindowExW) == 48, "CreateWindowExW");
    GH_X86_BED(R, "user32.dll", "CreateWindowExW", 48,
        [](Machine& m, const uint32_t* a) -> uint64_t { return x86_create_window(m, a, true, "CreateWindowExW"); });
    GH_X86_BED(R, "user32.dll", "CreateWindowExA", 48,
        [](Machine& m, const uint32_t* a) -> uint64_t { return x86_create_window(m, a, false, "CreateWindowExA"); });
    GH_X86_BED(R, "user32.dll", "DestroyWindow", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            m.bed.sub_windows.erase(a[0]);
            m.bed.win_props.erase(a[0]);
            return 1;
        });

    // Window properties. Our windows are synthetic, so their properties must be
    // ours as well: see BedState::win_props for what forwarding these cost.
    // GetProp on a name nobody set is NULL, and NULL is exactly what a caller
    // like SDL2 tests for.
    GH_X86_BED(R, "user32.dll", "GetPropW", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t { return x86_get_prop(m, a[0], a[1], true); });
    GH_X86_BED(R, "user32.dll", "GetPropA", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t { return x86_get_prop(m, a[0], a[1], false); });
    GH_X86_BED(R, "user32.dll", "SetPropW", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t { return x86_set_prop(m, a[0], a[1], a[2], true); });
    GH_X86_BED(R, "user32.dll", "SetPropA", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t { return x86_set_prop(m, a[0], a[1], a[2], false); });
    GH_X86_BED(R, "user32.dll", "RemovePropW", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t { return x86_remove_prop(m, a[0], a[1], true); });
    GH_X86_BED(R, "user32.dll", "RemovePropA", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t { return x86_remove_prop(m, a[0], a[1], false); });

    // -- the rest of the window surface, for the same reason ----------------
    // Everything below used to forward to the host user32 with one of OUR
    // HWNDs. There is no such window on the host, so each of these asked the
    // real window manager about a handle it has never seen: the answer is at
    // best a failure the guest did not expect, and at worst -- GetPropW -- a
    // value the guest trusts and dereferences. The window is ours, so the
    // answers about it have to be ours. Each one answers for the surrogate:
    // one window, exactly the framebuffer, always visible, never minimised,
    // with nothing to repaint.
    GH_X86_BED(R, "user32.dll", "IsWindow", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            return a[0] == kFakeHwnd || m.bed.sub_windows.count(a[0]) ? 1 : 0;
        });
    // One at a time, because a wrong pop count is a silent four-byte stack
    // drift per call and these five do not agree on one.
    GH_X86_BED(R, "user32.dll", "IsWindowEnabled", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    GH_X86_BED(R, "user32.dll", "SetWindowRgn", 12,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    GH_X86_BED(R, "user32.dll", "MoveWindow", 24,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    GH_X86_BED(R, "user32.dll", "InvalidateRect", 12,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    GH_X86_BED(R, "user32.dll", "SetLayeredWindowAttributes", 16,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    // Client space IS screen space here, so points pass through untouched and
    // the offset the function reports is zero.
    GH_X86_BED(R, "user32.dll", "MapWindowPoints", 16,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    for (const char* n : { "GetTopWindow", "GetMenu", "GetWindowTextLengthW", "GetWindowTextLengthA" })
        GH_X86_BED(R, "user32.dll", n, 4,
            [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "user32.dll", "SetActiveWindow", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return kFakeHwnd; });
    GH_X86_BED(R, "user32.dll", "SetCapture", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });   // nobody had it
    GH_X86_BED(R, "user32.dll", "ReleaseCapture", 0,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    GH_X86_BED(R, "user32.dll", "GetWindow", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });   // no siblings, no owner
    GH_X86_BED(R, "user32.dll", "GetClassLongA", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "user32.dll", "GetClassLongW", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "user32.dll", "FlashWindow", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    // The window belongs to this process and to the thread that made it. A
    // forward answered 0 and wrote nothing, which reads as "no such window".
    GH_X86_BED(R, "user32.dll", "GetWindowThreadProcessId", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[1]) *(uint32_t*)(uintptr_t)a[1] = GetCurrentProcessId();
            return GetCurrentThreadId();
        });
    // Nothing is ever invalid: we do not paint through GDI, D3D presents.
    GH_X86_BED(R, "user32.dll", "GetUpdateRect", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[1]) std::memset((void*)(uintptr_t)a[1], 0, 16);
            return 0;
        });
    for (const char* n : { "GetWindowTextW", "GetWindowTextA" })
        GH_X86_BED(R, "user32.dll", n, 12,
            [](Machine&, const uint32_t* a) -> uint64_t {
                if (a[1] && (int32_t)a[2] > 0) std::memset((void*)(uintptr_t)a[1], 0, 2);
                return 0;
            });
    // WINDOWPLACEMENT is 44 bytes at 32-bit: length, flags, showCmd, two POINTs,
    // then the normal RECT.
    GH_X86_BED(R, "user32.dll", "GetWindowPlacement", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[1]) return 0;
            const uint32_t v[11] = { 44, 0, SW_SHOWNORMAL, 0, 0, 0, 0,
                                     0, 0, m.bed.win_w, m.bed.win_h };
            std::memcpy((void*)(uintptr_t)a[1], v, sizeof(v));
            return 1;
        });

    // The rest of the window lifecycle: there is nothing to do, and saying so
    // truthfully (TRUE, "it worked") is what keeps the guest on its happy path.
    for (const char* n : { "UpdateWindow", "SetForegroundWindow",
                           "IsWindowVisible", "ValidateRect" })
        GH_X86_BED(R, "user32.dll", n, 4,
            [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    GH_X86_BED(R, "user32.dll", "ShowWindow", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    GH_X86_BED(R, "user32.dll", "EnableWindow", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    GH_X86_BED(R, "user32.dll", "SetWindowTextW", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    GH_X86_BED(R, "user32.dll", "SetWindowPos", 28,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    GH_X86_BED(R, "user32.dll", "IsIconic", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });   // never minimised
    GH_X86_BED(R, "user32.dll", "SetFocus", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return kFakeHwnd; });
    GH_X86_BED(R, "user32.dll", "GetFocus", 0,
        [](Machine&, const uint32_t*) -> uint64_t { return kFakeHwnd; });
    for (const char* n : { "GetDesktopWindow", "GetActiveWindow", "GetForegroundWindow" })
        GH_X86_BED(R, "user32.dll", n, 0,
            [](Machine&, const uint32_t*) -> uint64_t { return kFakeHwnd; });
    GH_X86_BED(R, "user32.dll", "GetParent", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });   // top-level
    GH_X86_BED(R, "user32.dll", "GetAncestor", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return kFakeHwnd; });
    GH_X86_BED(R, "user32.dll", "WindowFromPoint", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return kFakeHwnd; });

    // A device context the guest can hold but never draw through. Unity asks for
    // one during display enumeration and releases it immediately; D3D is what
    // actually renders.
    GH_X86_BED(R, "user32.dll", "GetDC", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return kFakeHdc; });
    GH_X86_BED(R, "user32.dll", "ReleaseDC", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });

    // Client == screen: the surrogate window is exactly the framebuffer, so
    // there is no non-client area and AdjustWindowRect has nothing to adjust.
    // RECT is 16 bytes at both widths, so these are the rare user32 entries
    // whose STRUCT would have survived a forward -- it is the HWND that would
    // not have.
    GH_X86_BED(R, "user32.dll", "GetClientRect", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[1]) return 0;
            int32_t v[4] = { 0, 0, (int32_t)m.bed.win_w, (int32_t)m.bed.win_h };
            std::memcpy((void*)(uintptr_t)a[1], v, sizeof(v));
            return 1;
        });
    GH_X86_BED(R, "user32.dll", "GetWindowRect", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[1]) return 0;
            int32_t v[4] = { 0, 0, (int32_t)m.bed.win_w, (int32_t)m.bed.win_h };
            std::memcpy((void*)(uintptr_t)a[1], v, sizeof(v));
            return 1;
        });
    GH_X86_BED(R, "user32.dll", "AdjustWindowRect", 12,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    GH_X86_BED(R, "user32.dll", "AdjustWindowRectEx", 16,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    for (const char* n : { "ClientToScreen", "ScreenToClient" })
        GH_X86_BED(R, "user32.dll", n, 8,
            [](Machine&, const uint32_t*) -> uint64_t { return 1; });   // identity

    // GetWindowLong/SetWindowLong on a window that does not exist: the values
    // are kept per index and read back. GWL_WNDPROC is the window procedure
    // itself -- Source's inputsystem subclasses the game window with it and
    // chains to the old proc through CallWindowProc. Answering 0 and dropping
    // the store (as this did until s855) left every key with the engine's own
    // proc, which never hands one to the input system.
    // GWL_WNDPROC (-4) is only declared for 32-bit builds; this one is 64-bit.
    constexpr int32_t kGwlWndProc32 = -4;
    for (const char* n : { "GetWindowLongA", "GetWindowLongW" })
        GH_X86_BED(R, "user32.dll", n, 8,
            [](Machine& m, const uint32_t* a) -> uint64_t {
                const int32_t i = (int32_t)a[1];
                auto sw = m.bed.sub_windows.find(a[0]);
                if (sw != m.bed.sub_windows.end()) {
                    if (i == kGwlWndProc32) return sw->second.proc;
                    auto it = sw->second.longs.find(i);
                    return it == sw->second.longs.end() ? 0 : it->second;
                }
                if (i == kGwlWndProc32) return m.bed.wndproc;
                auto it = m.bed.win_long.find(i);
                return it == m.bed.win_long.end() ? 0 : it->second;
            });
    for (const char* n : { "SetWindowLongA", "SetWindowLongW" })
        GH_X86_BED(R, "user32.dll", n, 12,
            [](Machine& m, const uint32_t* a) -> uint64_t {
                const int32_t i = (int32_t)a[1];
                uint32_t old;
                auto sw = m.bed.sub_windows.find(a[0]);
                if (sw != m.bed.sub_windows.end()) {
                    uint32_t& slot = (i == kGwlWndProc32) ? sw->second.proc : sw->second.longs[i];
                    old = slot;
                    if (i != kGwlWndProc32 || a[2]) slot = a[2];
                    return old;
                }
                if (i == kGwlWndProc32) {
                    old = m.bed.wndproc;
                    if (a[2]) m.bed.wndproc = a[2];
                    log("X86 SetWindowLong(GWL_WNDPROC) " + hex(old) + " -> " + hex(a[2]));
                } else {
                    old = m.bed.win_long[i];
                    m.bed.win_long[i] = a[2];
                }
                return old;
            });

#ifdef GH_LAUNCHER
    // Polled keyboard/mouse: the x64 beds' state (g_vkDown, g_kmCur*), which the
    // CoreWindow handlers keep. Forwarded, the container's user32 reports
    // nothing pressed and the cursor at 0,0. Same arguments at both widths.
    GH_X86_BED(R, "user32.dll", "GetAsyncKeyState", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return (uint16_t)Bed_GetAsyncKeyState((int)a[0]); });
    GH_X86_BED(R, "user32.dll", "GetKeyState", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return (uint16_t)Bed_GetKeyState((int)a[0]); });
    GH_X86_BED(R, "user32.dll", "GetKeyboardState", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return Bed_GetKeyboardState((PBYTE)(uintptr_t)a[0]); });
    GH_X86_BED(R, "user32.dll", "GetCursorPos", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return Bed_GetCursorPos((LPPOINT)(uintptr_t)a[0]); });
    // The cursor the guest moves and hides is the x64 beds' too. Forwarded, the
    // recentre each mouse-look frame never reached g_kmCur*, and the crosshair
    // stamp stayed on screen in gameplay because the hide went to the real user32.
    GH_X86_BED(R, "user32.dll", "SetCursorPos", 8,
        [](Machine&, const uint32_t* a) -> uint64_t { return Bed_SetCursorPos((int)a[0], (int)a[1]); });
    GH_X86_BED(R, "user32.dll", "ShowCursor", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return (uint32_t)Bed_ShowCursor((BOOL)a[0]); });
    GH_X86_BED(R, "user32.dll", "SetCursor", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)(uintptr_t)Bed_SetCursor((HCURSOR)(uintptr_t)a[0]);
        });
    GH_X86_BED(R, "user32.dll", "ClipCursor", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return Bed_ClipCursor((const RECT*)(uintptr_t)a[0]); });

    // Raw input: the x64 beds' synthetic packets in the 32-bit layout. Forwarded,
    // these reached the container's user32 with x86 strides (RAWINPUTDEVICE is 12
    // bytes here, 16 there), so registration failed and the device count could
    // come back as (UINT)-1. RAWINPUTHEADER is 16 bytes on x86 (hDevice and wParam
    // narrow) and 24 on x64; RAWMOUSE and RAWKEYBOARD are the same at both widths.
    GH_X86_BED(R, "user32.dll", "RegisterRawInputDevices", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            static bool once = false;
            if (!once) { once = true; log("X86 RegisterRawInputDevices n=" + std::to_string(a[1]) + " cb=" + std::to_string(a[2])); }
            return TRUE;
        });
    GH_X86_BED(R, "user32.dll", "GetRawInputDeviceList", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[1]) *(uint32_t*)(uintptr_t)a[1] = 0;     // no devices; never (UINT)-1 (see the x64 bed)
            return 0;
        });
    GH_X86_BED(R, "user32.dll", "GetRawInputData", 20,
        [](Machine&, const uint32_t* a) -> uint64_t {
            constexpr UINT kHdr32 = 16;
            RAWINPUT ri{};
            UINT cb = sizeof ri;
            if (Bed_GetRawInputData((HRAWINPUT)(uintptr_t)a[0], RID_INPUT, &ri, &cb, sizeof(RAWINPUTHEADER)) == 0 ||
                cb == (UINT)-1 || cb < sizeof(RAWINPUTHEADER)) {
                if (!a[2] && a[3]) *(uint32_t*)(uintptr_t)a[3] = 0;
                return 0;
            }
            const UINT body = cb - (UINT)sizeof(RAWINPUTHEADER);
            const UINT want = (a[1] == RID_HEADER) ? kHdr32 : kHdr32 + body;
            uint32_t* pcb = (uint32_t*)(uintptr_t)a[3];
            if (!a[2]) { if (pcb) *pcb = want; return 0; }
            if (!pcb || *pcb < want) return (uint32_t)-1;
            const uint32_t h32[4] = { ri.header.dwType, kHdr32 + body,
                                      (uint32_t)(uintptr_t)ri.header.hDevice, (uint32_t)ri.header.wParam };
            uint8_t* out = (uint8_t*)(uintptr_t)a[2];
            std::memcpy(out, h32, kHdr32);
            if (a[1] != RID_HEADER) std::memcpy(out + kHdr32, &ri.data, body);
            static int n = 0;
            if (++n <= 4) log("X86 GetRawInputData served type=" + std::to_string(ri.header.dwType) + " bytes=" + std::to_string(want));
            return want;
        });
#endif

    // -- the message pump ---------------------------------------------------
    // MSG on x86 is 28 bytes: hwnd 0, message 4, wParam 8, lParam 12, time 16,
    // pt.x 20, pt.y 24. On x64 it is 48. A forwarded PeekMessage would write
    // twenty bytes past the guest's MSG -- which lives on the stack, next to the
    // return address, and is rule 10's failure exactly.
    //
    // The queue delivers one activation sequence and is then empty forever.
    // WM_ACTIVATEAPP(TRUE) + WM_ACTIVATE(WA_ACTIVE) + WM_SETFOCUS is what flips
    // Unity's gAppActive; without it the player runs but renders nothing,
    // because it believes it is in the background. Input arrives later, through
    // the raw-input beds (a separate piece of work).
    // wRemoveMsg is honoured: a PM_NOREMOVE peek that consumed the message
    // would lose it, and a guest that peeks before it pumps is ordinary.
    for (const char* n : { "PeekMessageW", "PeekMessageA" })
        GH_X86_BED(R, "user32.dll", n, 20,
            [](Machine& m, const uint32_t* a) -> uint64_t {
                return peek_message32(m, a[0], a[2], a[3], (a[4] & PM_REMOVE) != 0, a[-1], a[1]);
            });

    // GetMessage BLOCKS until a message arrives, and on this boundary nothing
    // ever will -- so a faithful implementation is a guaranteed hang of every
    // guest thread (rule 9). Reporting WM_QUIT with a FALSE return is the one
    // answer that leaves the caller's loop well-formed, and it is what the x64
    // path settled on for the same reason.
    for (const char* n : { "GetMessageA", "GetMessageW" })
        GH_X86_BED(R, "user32.dll", n, 16,
            [](Machine& m, const uint32_t* a) -> uint64_t {
                (void)m;
                write_msg32(a[0], WM_QUIT, 0, 0);
                return 0;
            });

    GH_X86_BED(R, "user32.dll", "TranslateMessage", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "user32.dll", "IsDialogMessageW", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "user32.dll", "PostQuitMessage", 4,
        [](Machine& m, const uint32_t*) -> uint64_t { m.bed.quit_posted = true; return 0; });

    // Dispatch is the whole reason RegisterClass had to be a bed: the window
    // procedure is GUEST code, so it is entered through call_guest32 rather
    // than called. This is the same host->guest direction CreateThread and the
    // SEH handlers use, and it is reentrant -- we are already inside a host call
    // made by the guest when we get here.
    for (const char* n : { "DispatchMessageW", "DispatchMessageA" })
        GH_X86_BED(R, "user32.dll", n, 4,
            [](Machine& m, const uint32_t* a) -> uint64_t {
                if (!a[0]) return 0;
                const uint32_t* msg = (const uint32_t*)(uintptr_t)a[0];
                if (msg[1] == WM_TIMER && msg[3]) {                    // TIMERPROC(hwnd, msg, id, time)
                    uint32_t targs[4] = { msg[0], WM_TIMER, msg[2], (uint32_t)GetTickCount() };
                    return m.call_guest32(msg[3], targs, 4);
                }
                const uint32_t proc = wndproc_for(m, msg[0]);
                if (!proc) return 0;
                uint32_t args[4] = { msg[0], msg[1], msg[2], msg[3] };   // hwnd, message, wParam, lParam
                log_input_hop("dispatch", msg[1], msg[2], proc, a[-1]);
                return m.call_guest32(proc, args, 4);
            });
    GH_X86_BED(R, "user32.dll", "DefWindowProcW", 16,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "user32.dll", "DefWindowProcA", 16,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });

    // SendMessage delivers synchronously to the window procedure, so it is the
    // same host->guest hop as dispatch and cannot be a forward either.
    GH_X86_BED(R, "user32.dll", "SendMessageW", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            const uint32_t proc = wndproc_for(m, a[0]);
            if (!proc) return 0;
            uint32_t args[4] = { a[0], a[1], a[2], a[3] };
            return m.call_guest32(proc, args, 4);
        });
    GH_X86_BED(R, "user32.dll", "SendMessageA", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            const uint32_t proc = wndproc_for(m, a[0]);
            if (!proc) return 0;
            uint32_t args[4] = { a[0], a[1], a[2], a[3] };
            return m.call_guest32(proc, args, 4);
        });

    // MsgWaitForMultipleObjects would block the one host thread every guest
    // thread lives on (rule 9), AND it is on the pointer-to-pointer-sized audit
    // list (rule 10). Answering "there is input" sends the caller straight back
    // to PeekMessage -- which is where the activation sequence is waiting -- and
    // then on into its player loop, rather than parking.
    GH_X86_BED(R, "user32.dll", "MsgWaitForMultipleObjects", 20,
        [](Machine&, const uint32_t* a) -> uint64_t { return a[0]; });   // WAIT_OBJECT_0 + nCount
    GH_X86_BED(R, "user32.dll", "MsgWaitForMultipleObjectsEx", 24,
        [](Machine&, const uint32_t* a) -> uint64_t { return a[0]; });

    for (const char* n : { "GetMessagePos", "GetMessageTime", "GetMessageExtraInfo" })
        GH_X86_BED(R, "user32.dll", n, 0,
            [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "user32.dll", "RegisterWindowMessageA", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 0xC001; });   // any private message id

    // -- the few names gh_x86_stubs.h must NOT answer with a constant -------
    // Everything else Chromium binds and nothing implements is answered by the
    // generated stub table, which registers after this one and loses every
    // name already here. These are the ones where a constant would be a lie
    // the caller acts on: a comparison that says "equal", a path helper that
    // returns NULL where the caller will dereference, a wait that returns
    // instantly on a thread whose whole job is to wait.

    // WaitMessage blocks until the queue is not empty. Our queue is fed by the
    // host's window thread, so there is nothing to wait ON here -- but
    // returning immediately turns a CEF pump thread into a spin that eats a
    // core of a four-core console. A short sleep is the honest shape of "wait":
    // the caller loops back into PeekMessage either way.
    GH_X86_BED(R, "user32.dll", "WaitMessage", 0,
        [](Machine&, const uint32_t*) -> uint64_t { Sleep(2); return TRUE; });

    // CharUpperW is two functions behind one name: with a high word, a string
    // uppercased IN PLACE and returned; without one, a single character in the
    // low word. Both have to work -- vgui2 uses the string form on names it
    // then compares.
    GH_X86_BED(R, "user32.dll", "CharUpperW", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if ((a[0] >> 16) == 0) {
                uint32_t c = a[0] & 0xFFFF;
                if (c >= 'a' && c <= 'z') c -= 32;
                return c;
            }
            for (wchar_t* p = (wchar_t*)(uintptr_t)a[0]; p && *p; ++p)
                if (*p >= L'a' && *p <= L'z') *p = (wchar_t)(*p - 32);
            return a[0];
        });

    GH_X86_BED(R, "user32.dll", "InflateRect", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            RECT* r = (RECT*)(uintptr_t)a[0];
            if (!r) return FALSE;
            r->left -= (LONG)(int32_t)a[1]; r->right  += (LONG)(int32_t)a[1];
            r->top  -= (LONG)(int32_t)a[2]; r->bottom += (LONG)(int32_t)a[2];
            return TRUE;
        });

    // A dark-on-light scheme, because every caller of this is drawing a
    // control we are not otherwise painting. A single constant for all indices
    // would put white text on a white background.
    GH_X86_BED(R, "user32.dll", "GetSysColor", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            switch (a[0]) {
                case COLOR_WINDOW:          return 0x00FFFFFF;
                case COLOR_WINDOWTEXT:
                case COLOR_BTNTEXT:
                case COLOR_MENUTEXT:        return 0x00000000;
                case COLOR_BTNFACE:
                case COLOR_MENU:
                case COLOR_SCROLLBAR:       return 0x00F0F0F0;
                case COLOR_HIGHLIGHT:       return 0x00D77800;
                case COLOR_HIGHLIGHTTEXT:   return 0x00FFFFFF;
                case COLOR_GRAYTEXT:        return 0x006D6D6D;
                default:                    return 0x00808080;
            }
        });

    GH_X86_BED(R, "kernel32.dll", "lstrcmpiA", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            const unsigned char* p = (const unsigned char*)(uintptr_t)a[0];
            const unsigned char* q = (const unsigned char*)(uintptr_t)a[1];
            if (!p || !q) return (uint32_t)(p == q ? 0 : (p ? 1 : -1));
            auto low = [](unsigned char c) { return (unsigned char)(c >= 'A' && c <= 'Z' ? c + 32 : c); };
            for (; *p && low(*p) == low(*q); ++p, ++q) {}
            return (uint32_t)(int32_t)(low(*p) - low(*q));
        });

    // The guest never sees the real one, and nothing it does with the answer
    // reaches a host path: the VFS maps what it opens. A plausible, stable
    // string is what callers want -- several build a DLL path from it and then
    // LoadLibrary that, which resolve_dll_path answers from the corpus.
    GH_X86_BED(R, "kernel32.dll", "GetSystemDirectoryW", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            static const wchar_t kSys[] = L"C:\\Windows\\system32";
            const uint32_t need = (uint32_t)(sizeof kSys / sizeof kSys[0]);   // with the NUL
            if (!a[0] || a[1] < need) return need;                            // chars INCLUDING the NUL
            std::memcpy((void*)(uintptr_t)a[0], kSys, sizeof kSys);
            return need - 1;                                                  // chars EXCLUDING it
        });

    // PathFindExtensionW / PathFindFileNameW return a pointer INTO the caller's
    // own string, so they are safe to answer here (guest memory is below 4 GB)
    // and unsafe to answer with a constant: NULL is not a value either of them
    // can return, and the caller dereferences what it gets.
    GH_X86_BED(R, "shlwapi.dll", "PathFindExtensionW", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            const wchar_t* s = (const wchar_t*)(uintptr_t)a[0];
            if (!s) return 0;
            const wchar_t* dot = nullptr;
            for (const wchar_t* p = s; *p; ++p) {
                if (*p == L'\\' || *p == L'/') dot = nullptr;
                else if (*p == L'.') dot = p;
            }
            if (dot) return (uint32_t)(uintptr_t)dot;
            const wchar_t* e = s; while (*e) ++e;
            return (uint32_t)(uintptr_t)e;                 // the NUL, never NULL
        });
    GH_X86_BED(R, "shlwapi.dll", "PathFindFileNameW", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            const wchar_t* s = (const wchar_t*)(uintptr_t)a[0];
            if (!s) return 0;
            const wchar_t* last = s;
            for (const wchar_t* p = s; *p; ++p)
                if (*p == L'\\' || *p == L'/' || *p == L':') last = p + 1;
            return (uint32_t)(uintptr_t)last;
        });
    GH_X86_BED(R, "shlwapi.dll", "PathRemoveExtensionW", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            wchar_t* s = (wchar_t*)(uintptr_t)a[0];
            if (!s) return 0;
            wchar_t* dot = nullptr;
            for (wchar_t* p = s; *p; ++p) {
                if (*p == L'\\' || *p == L'/') dot = nullptr;
                else if (*p == L'.') dot = p;
            }
            if (dot) *dot = 0;
            return 0;
        });

    // -- WS2_32: the whole socket surface, failing by contract --------------
    // Thirty-two WS2_32 imports were unresolved, and Stick Fight walks into
    // `socket` 135M instructions into STARTUP -- not at lobby time, which this
    // document had assumed from which managed assembly holds the networking
    // code. An unresolved import is a hard fault rather than a degraded
    // feature, so the whole set is registered in one go: discovering them one
    // per deploy is thirty-two deploys.
    //
    // They fail rather than forward, and that is a decision with three separate
    // reasons -- any one of which would be enough on its own:
    //
    //   1. BLOCKING. recv, accept, connect and select block the calling thread.
    //      Every guest thread lives on ONE host thread, so a blocking host call
    //      stops the entire guest -- including whichever thread was going to
    //      feed that socket. This is bed-layer rule 9 (see GREENHOUSE.md
    //      SS30.5) and it is the same trap SyncObj exists to avoid. Real
    //      sockets here means forcing every host socket non-blocking and
    //      re-entering through the scheduler's retry path, exactly as
    //      WaitForSingleObject does -- real work, worth doing only once the
    //      guest is known to need it.
    //   2. POINTERS. inet_ntoa, gethostbyname, getprotobyname and getaddrinfo
    //      hand back host pointers, which do not fit in a guest slot.
    //      getaddrinfo is the sharp one: it is a FORWARD in the generated table
    //      today, so the host writes an 8-byte pointer into the guest's 4-byte
    //      output slot -- clobbering the four bytes after it -- and the guest
    //      then dereferences the truncation. Bedding it closes a live
    //      corruption bug whatever the socket policy turns out to be.
    //   3. WIDTH. fd_set is an array of SOCKETs, and SOCKET is UINT_PTR: 4
    //      bytes on the guest, 8 on the host. So select and __WSAFDIsSet can
    //      never be forwards, however the rest is decided.
    //
    // What is NOT stubbed, because being absent is survivable and being WRONG
    // is not: byte order, inet_addr, inet_ntoa, gethostname, WSAStartup (above,
    // which still succeeds -- "Winsock is initialised but no socket can be
    // opened" is an ordinary state a caller must already handle) and the
    // last-error pair.

    // The pop size is the one field in this whole section that fails SILENTLY.
    // Too small and the guest's stack pointer creeps upward on every call until
    // something far away crashes -- that is exactly how VirtualProtect's
    // four-byte overshoot cost six million instructions of hunting (see the
    // Arg32 notes in gh_x86_thunk.h). It is also the field a hand-written table
    // makes easiest to mistype. So every one is checked against the REAL
    // declaration in winsock2.h/ws2tcpip.h, using the same constexpr machinery
    // GH_X86_FWD derives its own from: free at runtime, and it cannot drift
    // from the header the way a comment can.
    static_assert(thunk_bytes_of(&::socket)                   == 12, "socket");
    static_assert(thunk_bytes_of(&::WSASocketA)               == 24, "WSASocketA");
    static_assert(thunk_bytes_of(&::WSASocketW)               == 24, "WSASocketW");
    static_assert(thunk_bytes_of(&::accept)                   == 12, "accept");
    static_assert(thunk_bytes_of(&::bind)                     == 12, "bind");
    static_assert(thunk_bytes_of(&::closesocket)              ==  4, "closesocket");
    static_assert(thunk_bytes_of(&::connect)                  == 12, "connect");
    static_assert(thunk_bytes_of(&::getpeername)              == 12, "getpeername");
    static_assert(thunk_bytes_of(&::getsockname)              == 12, "getsockname");
    static_assert(thunk_bytes_of(&::getsockopt)               == 20, "getsockopt");
    static_assert(thunk_bytes_of(&::ioctlsocket)              == 12, "ioctlsocket");
    static_assert(thunk_bytes_of(&::listen)                   ==  8, "listen");
    static_assert(thunk_bytes_of(&::recv)                     == 16, "recv");
    static_assert(thunk_bytes_of(&::recvfrom)                 == 24, "recvfrom");
    static_assert(thunk_bytes_of(&::send)                     == 16, "send");
    static_assert(thunk_bytes_of(&::sendto)                   == 24, "sendto");
    static_assert(thunk_bytes_of(&::setsockopt)               == 20, "setsockopt");
    static_assert(thunk_bytes_of(&::shutdown)                 ==  8, "shutdown");
    static_assert(thunk_bytes_of(&::select)                   == 20, "select");
    static_assert(thunk_bytes_of(&::__WSAFDIsSet)             ==  8, "__WSAFDIsSet");
    static_assert(thunk_bytes_of(&::WSAEventSelect)           == 12, "WSAEventSelect");
    static_assert(thunk_bytes_of(&::WSAEnumNetworkEvents)     == 12, "WSAEnumNetworkEvents");
    static_assert(thunk_bytes_of(&::WSAIoctl)                 == 36, "WSAIoctl");
    static_assert(thunk_bytes_of(&::WSARecvFrom)              == 36, "WSARecvFrom");
    static_assert(thunk_bytes_of(&::htonl)                    ==  4, "htonl");
    static_assert(thunk_bytes_of(&::ntohl)                    ==  4, "ntohl");
    static_assert(thunk_bytes_of(&::htons)                    ==  4, "htons");
    static_assert(thunk_bytes_of(&::ntohs)                    ==  4, "ntohs");
    static_assert(thunk_bytes_of(&::inet_addr)                ==  4, "inet_addr");
    static_assert(thunk_bytes_of(&::inet_ntoa)                ==  4, "inet_ntoa (in_addr by value)");
    static_assert(thunk_bytes_of(&::gethostname)              ==  8, "gethostname");
    static_assert(thunk_bytes_of(&::gethostbyname)            ==  4, "gethostbyname");
    static_assert(thunk_bytes_of(&::getprotobyname)           ==  4, "getprotobyname");
    static_assert(thunk_bytes_of(&::getaddrinfo)              == 16, "getaddrinfo");
    static_assert(thunk_bytes_of(&::getnameinfo)              == 28, "getnameinfo");
    static_assert(thunk_bytes_of(&::freeaddrinfo)             ==  4, "freeaddrinfo");
    static_assert(thunk_bytes_of(&::WSACleanup)               ==  0, "WSACleanup");
    static_assert(thunk_bytes_of(&::WSAGetLastError)          ==  0, "WSAGetLastError");
    static_assert(thunk_bytes_of(&::WSASetLastError)          ==  4, "WSASetLastError");
    static_assert(thunk_bytes_of(&::WSAAsyncGetHostByName)    == 20, "WSAAsyncGetHostByName");
    static_assert(thunk_bytes_of(&::WSACancelAsyncRequest)    ==  4, "WSACancelAsyncRequest");
    static_assert(thunk_bytes_of(&::WSACreateEvent)           ==  0, "WSACreateEvent");
    static_assert(thunk_bytes_of(&::WSACloseEvent)            ==  4, "WSACloseEvent");
    static_assert(thunk_bytes_of(&::WSASetEvent)              ==  4, "WSASetEvent");
    static_assert(thunk_bytes_of(&::WSAResetEvent)            ==  4, "WSAResetEvent");
    static_assert(thunk_bytes_of(&::WSAWaitForMultipleEvents) == 20, "WSAWaitForMultipleEvents");

    // Pure arithmetic. A guest that byte-swaps wrongly corrupts data on every
    // path that uses these, including the many that never reach a socket, so
    // these are implemented rather than stubbed. Written out rather than
    // forwarded so they cost a bed call and not a DLL call.
    GH_X86_BED(R, "ws2_32.dll", "htonl", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            const uint32_t v = a[0];
            return (uint64_t)(((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
                              ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24));
        });
    GH_X86_BED(R, "ws2_32.dll", "ntohl", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            const uint32_t v = a[0];
            return (uint64_t)(((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
                              ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24));
        });
    GH_X86_BED(R, "ws2_32.dll", "htons", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            const uint32_t v = a[0] & 0xFFFFu;
            return (uint64_t)(((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu));
        });
    GH_X86_BED(R, "ws2_32.dll", "ntohs", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            const uint32_t v = a[0] & 0xFFFFu;
            return (uint64_t)(((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu));
        });

    // Parsed here rather than forwarded because the host's inet_addr is one of
    // the deprecated entry points and, unlike the swaps above, it has an
    // error return a caller tests -- INADDR_NONE for anything that is not a
    // dotted quad. The classic a.b, a.b.c and bare-number forms are refused:
    // no guest uses them, and refusing is better than half-supporting.
    GH_X86_BED(R, "ws2_32.dll", "inet_addr", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0]) return kSockError32;                       // INADDR_NONE
            const std::string s = m.guest_str(a[0], 64);
            uint32_t o[4] = {}; int idx = 0; bool digit = false;
            for (char c : s) {
                if (c >= '0' && c <= '9') {
                    o[idx] = o[idx] * 10u + (uint32_t)(c - '0');
                    if (o[idx] > 255) return kSockError32;
                    digit = true;
                } else if (c == '.' && digit && idx < 3) {
                    ++idx; digit = false;
                } else {
                    return kSockError32;
                }
            }
            if (idx != 3 || !digit) return kSockError32;
            // Network byte order: the first octet is the LOW byte of the value.
            return (uint64_t)(o[0] | (o[1] << 8) | (o[2] << 16) | (o[3] << 24));
        });

    // in_addr is passed BY VALUE -- four bytes, one slot -- so a[0] is the
    // address itself and not a pointer to it.
    GH_X86_BED(R, "ws2_32.dll", "inet_ntoa", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            // Winsock's contract is that the result lives in storage owned by
            // the library and stays valid until this thread calls it again, so
            // the caller does not free it. Ours has to live in GUEST memory: one
            // heap block, allocated on first use and never released.
            static uint32_t buf = 0;
            static uint32_t stamp = 0;          // a guest block: one session's
            if (x86_session_changed(stamp)) buf = 0;
            if (!buf) buf = m.heap_alloc(24, true);
            if (!buf) return 0;
            const uint8_t* b = (const uint8_t*)&a[0];
            char t[24];
            const int n = std::snprintf(t, sizeof(t), "%u.%u.%u.%u",
                                        b[0], b[1], b[2], b[3]);
            std::memcpy((void*)(uintptr_t)buf, t, (size_t)n + 1);
            return buf;
        });

    // Plain char buffer, no width difference, and the answer is the console's
    // real name -- which several guests use to seed a default player name. The
    // buffer is guest memory, which is directly addressable, so the host writes
    // into it in place.
    GH_X86_BED(R, "ws2_32.dll", "gethostname", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[0] || (int32_t)a[1] <= 0) return wsa_fail(m, WSAEFAULT, kSockError32);
            const int rc = ::gethostname((char*)(uintptr_t)a[0], (int)a[1]);
            if (rc != 0) return wsa_fail(m, WSAGetLastError(), kSockError32);
            return 0;
        });

    // Symmetric with the WSAStartup bed above, which does call the host.
    GH_X86_BED(R, "ws2_32.dll", "WSACleanup", 0,
        [](Machine&, const uint32_t*) -> uint64_t { return (uint32_t)::WSACleanup(); });

    GH_X86_BED(R, "ws2_32.dll", "WSAGetLastError", 0,
        [](Machine& m, const uint32_t*) -> uint64_t { return m.cur_thread().wsa_err; });
    GH_X86_BED(R, "ws2_32.dll", "WSASetLastError", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            m.cur_thread().wsa_err = a[0];
            return 0;
        });

    // The one call worth logging: it is rare, it names what the guest wanted,
    // and it is the exact point where the no-socket policy becomes visible to
    // the guest. WSAEAFNOSUPPORT is what a machine with no usable protocol
    // stack returns, so it lands in an error path callers already have.
    GH_X86_BED(R, "ws2_32.dll", "socket", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            log("X86 socket(af=" + std::to_string(a[0]) + " type=" + std::to_string(a[1]) +
                " proto=" + std::to_string(a[2]) + ") -> INVALID_SOCKET (no socket layer)");
            return wsa_fail(m, WSAEAFNOSUPPORT, kSockError32);
        });
    GH_X86_BED(R, "ws2_32.dll", "WSASocketA", 24,
        [](Machine& m, const uint32_t*) -> uint64_t {
            return wsa_fail(m, WSAEAFNOSUPPORT, kSockError32);
        });
    GH_X86_BED(R, "ws2_32.dll", "WSASocketW", 24,
        [](Machine& m, const uint32_t*) -> uint64_t {
            return wsa_fail(m, WSAEAFNOSUPPORT, kSockError32);
        });

    // Everything that TAKES a socket fails identically, because we never handed
    // one out: WSAENOTSOCK is precisely "that is not a descriptor I issued", and
    // it is the branch a caller's error path already has. A table rather than
    // twenty near-identical lambdas -- the pop size is the only thing that
    // genuinely differs between them, and the pop size is the one that silently
    // corrupts the guest stack if it is wrong.
    static const struct { const char* name; uint32_t pop; } kSocketTakers[] = {
        { "accept",               12 },   // returns SOCKET; INVALID_SOCKET is the same value
        { "bind",                 12 },
        { "closesocket",           4 },
        { "connect",              12 },
        { "getpeername",          12 },
        { "getsockname",          12 },
        { "getsockopt",           20 },
        { "ioctlsocket",          12 },
        { "listen",                8 },
        { "recv",                 16 },
        { "recvfrom",             24 },
        { "send",                 16 },
        { "sendto",               24 },
        { "setsockopt",           20 },
        { "shutdown",              8 },
        { "WSAEventSelect",       12 },
        { "WSAEnumNetworkEvents", 12 },
        { "WSAIoctl",             36 },   // also a callback-taking forward: see below
        { "WSARecvFrom",          36 },   // same
    };
    // (named `st`, not `f`: GH_X86_BED declares its own `f` internally, and a
    // loop variable of that name is shadowed by it rather than read.)
    for (const auto& st : kSocketTakers)
        GH_X86_BED(R, "ws2_32.dll", st.name, st.pop,
            [](Machine& m, const uint32_t*) -> uint64_t {
                return wsa_fail(m, WSAENOTSOCK, kSockError32);
            });

    // WSAIoctl and WSARecvFrom are two of the twelve generated forwards the
    // startup audit named as taking a guest callback -- a completion routine
    // the HOST would call, which means the host jumping to guest code. Bedding
    // them for the socket policy closes that too; the other ten are elsewhere.

    // select is the fd_set case: an array of SOCKETs, 4 bytes wide on the guest
    // and 8 on the host, so the host would read the guest's second descriptor
    // as the top half of its first. It could not be forwarded even if sockets
    // worked. __WSAFDIsSet answers honestly instead of failing -- it is a
    // predicate with no error return, and "not in the set" is true.
    GH_X86_BED(R, "ws2_32.dll", "select", 20,
        [](Machine& m, const uint32_t*) -> uint64_t {
            return wsa_fail(m, WSAENOTSOCK, kSockError32);
        });
    GH_X86_BED(R, "ws2_32.dll", "__WSAFDIsSet", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });

    // Name resolution. These return pointers into library-owned storage
    // (hostent, protoent, addrinfo) full of further pointers -- host-width, and
    // so unrepresentable to the guest even if we wanted the answer. Failing
    // "host not found" is a state every caller handles, and matches the DNSAPI
    // and WinHTTP policy below.
    GH_X86_BED(R, "ws2_32.dll", "gethostbyname", 4,
        [](Machine& m, const uint32_t*) -> uint64_t {
            return wsa_fail(m, WSAHOST_NOT_FOUND, 0);
        });
    GH_X86_BED(R, "ws2_32.dll", "getprotobyname", 4,
        [](Machine& m, const uint32_t*) -> uint64_t {
            return wsa_fail(m, WSANO_DATA, 0);
        });
    GH_X86_BED(R, "ws2_32.dll", "getaddrinfo", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (a[3]) *(uint32_t*)(uintptr_t)a[3] = 0;   // four bytes, not eight
            (void)wsa_fail(m, WSAHOST_NOT_FOUND, 0);
            return WSAHOST_NOT_FOUND;                    // returns the error directly
        });
    GH_X86_BED(R, "ws2_32.dll", "getnameinfo", 28,
        [](Machine& m, const uint32_t*) -> uint64_t {
            (void)wsa_fail(m, WSAHOST_NOT_FOUND, 0);
            return WSAHOST_NOT_FOUND;
        });
    GH_X86_BED(R, "ws2_32.dll", "freeaddrinfo", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });

    // The asynchronous name lookups complete by posting a window message, which
    // needs a message pump we do not have at this boundary yet either.
    GH_X86_BED(R, "ws2_32.dll", "WSAAsyncGetHostByName", 20,
        [](Machine& m, const uint32_t*) -> uint64_t {
            return wsa_fail(m, WSAHOST_NOT_FOUND, 0);    // NULL task handle
        });
    GH_X86_BED(R, "ws2_32.dll", "WSACancelAsyncRequest", 4,
        [](Machine& m, const uint32_t*) -> uint64_t {
            return wsa_fail(m, WSAEINVAL, kSockError32); // nothing was ever started
        });

    // WSA event objects. Shadowed rather than forwarded for the same reason
    // SyncObj exists: WSAWaitForMultipleEvents blocks the calling HOST thread,
    // and with every guest thread on that one thread, a wait nobody can signal
    // stops the entire guest rather than one part of it. Failing to create the
    // event in the first place is the honest form of that.
    GH_X86_BED(R, "ws2_32.dll", "WSACreateEvent", 0,
        [](Machine& m, const uint32_t*) -> uint64_t {
            return wsa_fail(m, WSAENOBUFS, 0);           // WSA_INVALID_EVENT
        });
    GH_X86_BED(R, "ws2_32.dll", "WSACloseEvent", 4,
        [](Machine& m, const uint32_t*) -> uint64_t {
            return wsa_fail(m, WSA_INVALID_HANDLE, 0);   // FALSE
        });
    GH_X86_BED(R, "ws2_32.dll", "WSASetEvent", 4,
        [](Machine& m, const uint32_t*) -> uint64_t {
            return wsa_fail(m, WSA_INVALID_HANDLE, 0);
        });
    GH_X86_BED(R, "ws2_32.dll", "WSAResetEvent", 4,
        [](Machine& m, const uint32_t*) -> uint64_t {
            return wsa_fail(m, WSA_INVALID_HANDLE, 0);
        });
    GH_X86_BED(R, "ws2_32.dll", "WSAWaitForMultipleEvents", 20,
        [](Machine& m, const uint32_t*) -> uint64_t {
            return wsa_fail(m, WSA_INVALID_HANDLE, kWsaWaitFailed);
        });

    // -- networking name resolution: absent, and cleanly so -----------------
    // The console reaches Steam through the loader's own x64 stack; a 32-bit
    // guest doing its own DNS or proxy discovery is not on the path to anything
    // this track needs. Failing is honest and, unlike an unresolved import, is
    // an outcome the caller is required to handle.
    GH_X86_BED(R, "dnsapi.dll", "DnsQuery_A", 24,
        [](Machine&, const uint32_t*) -> uint64_t { return 9003; });   // DNS_ERROR_RCODE_NAME_ERROR
    GH_X86_BED(R, "dnsapi.dll", "DnsFree", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "iphlpapi.dll", "GetIpAddrTable", 12,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });      // ERROR_INVALID_FUNCTION
    GH_X86_BED(R, "winhttp.dll", "WinHttpGetIEProxyConfigForCurrentUser", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });      // FALSE: no proxy configured

    // -- odds and ends the guest asked for by name --------------------------
    GH_X86_BED(R, "user32.dll", "MapVirtualKeyExA", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            // The slim Xbox user32 has no ...ExA. The wide, non-Ex form is
            // present and means the same thing for the default keyboard layout,
            // which is the only layout this console has.
            using Fn = UINT (WINAPI*)(UINT, UINT);
            static Fn p = (Fn)dyn_resolve("user32.dll", "MapVirtualKeyW");
            return p ? (uint32_t)p(a[0], a[1]) : 0;
        });

    // SHGetFolderPathW is how a Unity player decides where persistentDataPath
    // goes. Answering with the guest's own directory keeps every write inside
    // the tree the loader's VFS already governs, rather than somewhere the app
    // container cannot reach.
    GH_X86_BED(R, "shell32.dll", "SHGetFolderPathW", 20,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[4]) return 0x80070057u;                                  // E_INVALIDARG
            std::wstring dir = m.image_path;
            size_t cut = dir.find_last_of(L'\\');
            dir = (cut == std::wstring::npos) ? std::wstring(L"C:\\") : dir.substr(0, cut);
            wchar_t* out = (wchar_t*)(uintptr_t)a[4];
            std::memcpy(out, dir.c_str(), (dir.size() + 1) * 2);            // MAX_PATH buffer
            return 0;
        });
    // The A form, registered as shfolder.dll so LoadLibrary("shfolder.dll")
    // succeeds (lookup by name finds it for shell32 imports too). HL2's
    // libfontconfig loads it for its fallback config -- WINDOWSUSERFONTDIR and
    // the cache dir -- and without it that config fails to parse ("SHGetFolderPathA
    // failed", s853), which can leave fontconfig with no fonts: the 2024 main menu
    // is all FreeType text, so it was invisible.
    GH_X86_BED(R, "shfolder.dll", "SHGetFolderPathA", 20,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[4]) return 0x80070057u;                                  // E_INVALIDARG
            std::wstring dir = m.image_path;
            size_t cut = dir.find_last_of(L'\\');
            dir = (cut == std::wstring::npos) ? std::wstring(L"C:\\") : dir.substr(0, cut);
            char buf[MAX_PATH] = {};
            if (!WideCharToMultiByte(CP_ACP, 0, dir.c_str(), -1, buf, MAX_PATH, nullptr, nullptr)) return 0x80004005u;
            std::memcpy((void*)(uintptr_t)a[4], buf, std::strlen(buf) + 1);
            log("X86 SHGetFolderPathA csidl=" + hex(a[1]) + " -> " + buf);
            return 0;
        });

    // -- test hooks ---------------------------------------------------------
    // Not a Windows API. This is the M2 proof for the OTHER direction across the
    // boundary -- host code calling back into guest code -- which otherwise has
    // no natural exerciser until a window procedure or thread start routine
    // appears in M3/M5. Guests that do not import it are unaffected.
    GH_X86_BED(R, "ghtest.dll", "GhTestCallback", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            uint32_t fn = a[0], v = a[1];
            uint32_t arg1 = v;
            uint32_t r1 = m.call_guest32(fn, &arg1, 1);
            uint32_t arg2 = r1;
            uint32_t r2 = m.call_guest32(fn, &arg2, 1);
            log("X86 GhTestCallback(" + hex(fn) + ", " + std::to_string(v) +
                ") -> " + std::to_string(r1) + " -> " + std::to_string(r2));
            return r2;
        });

    // GDI text: the container's gdi32 has no font engine (gh_x86_gdi.h). Ahead
    // of the two tables below, whose CreateDIBSection (always fails) and
    // EnumFontFamiliesExA (asks the real, empty gdi32) are the non-Launcher
    // answers and would otherwise win.
    RegisterGdiBeds(R);

    // The eighteen entry points the rule-10 audit named. Registered here, ahead
    // of the generated forward table, so the beds shadow the forwards that
    // would otherwise write eight bytes through four-byte guest objects.
    RegisterPtrToPtrBeds(R);
    // What HL2's modules need that is neither of the above nor a forward.
    RegisterSourceGapBeds(R);

    // Only when the title asks for it: without the knob the guest keeps using
    // the steam_api.dll it ships, which is right for every title that does not
    // need SteamAPI_Init to SUCCEED (Half-Life 2 runs happily with it failing).
    // Registering these unconditionally would change that for a game whose own
    // copy is simply absent.
    if (g_x86_bed_only_steam) RegisterSteamBridgeBeds(R);

    // DXGI and D3D11. Kept in their own file: they are not Win32 calls but a
    // COM vtable boundary, and the marshalling they need has nothing in common
    // with the rest of this table.
    RegisterD3DBeds(R);
    RegisterD3D9Beds(R);
    // DirectSound onto gh_dsound.h's XAudio2 device (the console has no dsound.dll).
    RegisterDsoundBeds(R);
    // XInput and an empty DirectInput8, for SDL2's controller init.
    RegisterInputBeds(R);

    for (size_t i = drop0; i < R.dropped().size(); ++i)
        if (R.dropped()[i].winner >= std0)
            log(std::string("X86 bed SHADOWED by an earlier hand-written bed: ") + R.dropped()[i].name);
    log("X86 beds registered: " + std::to_string(R.size()));
}

}   // namespace ghx86
