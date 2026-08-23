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

#include "gh_x86_win.h"
// The COM boundary. Included here rather than beside the run loop so that the
// D3D entry points register alongside every other bed -- and so that they
// register BEFORE the generated forward table, whose dynamically-resolved
// d3d11.dll entries would otherwise shadow them under first-registration-wins.
#include "gh_x86_com_d3d.h"

namespace ghx86 {

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
    if (c->owner && c->owner != me) { m.block_for_retry(); return; }

    // Recursive acquisition by the owner never blocks -- that is the whole
    // difference between a critical section and a mutex.
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
    uint32_t mask = (uint32_t)si.dwActiveProcessorMask;
    std::memcpy(d + 16, &mask, 4);
    std::memcpy(d + 20, &si.dwNumberOfProcessors, 4);
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

// MSG is 28 bytes on x86 and 48 on x64: hwnd 0, message 4, wParam 8, lParam 12,
// time 16, pt.x 20, pt.y 24. Written field by field at the x86 offsets, because
// a forwarded PeekMessage would put twenty bytes of host MSG past the end of the
// guest's -- and a guest's MSG is a local, so what follows it is a saved
// register or a return address. That is rule 10 again, from the other side.
inline void write_msg32(uint32_t at, uint32_t message, uint32_t wparam, uint32_t lparam)
{
    if (!at) return;
    const uint32_t v[7] = { kFakeHwnd, message, wparam, lparam,
                            (uint32_t)GetTickCount(), 0, 0 };
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
// Real input does not come through here. It arrives via raw input, which is its
// own piece of work; this queue exists to answer the one question the player
// asks before it will draw.
inline uint64_t peek_message32(Machine& m, uint32_t msg_ptr, bool remove)
{
    static const UINT kSeq[] = { WM_ACTIVATEAPP, WM_ACTIVATE, WM_SETFOCUS, WM_NCACTIVATE };
    if (!msg_ptr || !m.bed.wndproc) return 0;
    if (m.bed.inject_idx >= (int)(sizeof(kSeq) / sizeof(kSeq[0]))) return 0;

    const UINT msg = kSeq[m.bed.inject_idx];
    // WA_ACTIVE and TRUE are both 1, but they are different things being said.
    write_msg32(msg_ptr, msg, (msg == WM_ACTIVATE) ? (uint32_t)WA_ACTIVE : 1u, 0);
    if (remove) {
        if (m.bed.inject_idx == 0) log("X86 injecting activation sequence -> guest WndProc");
        ++m.bed.inject_idx;
    }
    return 1;
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

inline void RegisterPtrToPtrBeds(ThunkRegistry& R)
{
    static_assert(thunk_bytes_of(&::GetProcessAffinityMask) == 12, "GetProcessAffinityMask");
    GH_X86_BED(R, "kernel32.dll", "GetProcessAffinityMask", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            DWORD_PTR proc = 0, sys = 0;
            const BOOL rc = GetProcessAffinityMask((HANDLE)(intptr_t)(int32_t)a[0], &proc, &sys);
            if (!rc) return 0;
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
            const LONG rc = RegOpenKeyExW(hkey32(a[0]), (LPCWSTR)(uintptr_t)a[1], a[2], a[3], &out);
            if (rc != ERROR_SUCCESS) return (uint32_t)rc;
            if (!put_ptr_out(a[4], (uint64_t)(uintptr_t)out, "RegOpenKeyExW")) {
                RegCloseKey(out);
                return (uint32_t)ERROR_INVALID_HANDLE;
            }
            return 0;
        });

    static_assert(thunk_bytes_of(&::RegCreateKeyW) == 12, "RegCreateKeyW");
    GH_X86_BED(R, "advapi32.dll", "RegCreateKeyW", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HKEY out = nullptr;
            const LONG rc = RegCreateKeyW(hkey32(a[0]), (LPCWSTR)(uintptr_t)a[1], &out);
            if (rc != ERROR_SUCCESS) return (uint32_t)rc;
            if (!put_ptr_out(a[2], (uint64_t)(uintptr_t)out, "RegCreateKeyW")) {
                RegCloseKey(out);
                return (uint32_t)ERROR_INVALID_HANDLE;
            }
            return 0;
        });

    static_assert(thunk_bytes_of(&::RegCreateKeyExW) == 36, "RegCreateKeyExW");
    GH_X86_BED(R, "advapi32.dll", "RegCreateKeyExW", 36,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HKEY  out  = nullptr;
            DWORD disp = 0;
            const LONG rc = RegCreateKeyExW(hkey32(a[0]), (LPCWSTR)(uintptr_t)a[1], a[2],
                                            (LPWSTR)(uintptr_t)a[3], a[4], a[5],
                                            sa32_translate(a[6]), &out, a[8] ? &disp : nullptr);
            if (rc != ERROR_SUCCESS) return (uint32_t)rc;
            if (a[8]) { const uint32_t d = disp; std::memcpy((void*)(uintptr_t)a[8], &d, 4); }
            if (!put_ptr_out(a[7], (uint64_t)(uintptr_t)out, "RegCreateKeyExW")) {
                RegCloseKey(out);
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
    // fit in 32 bits. A guest that cannot be given a working context is better
    // told so: every caller of these has a failure path, and none of them has a
    // path for a context that is half a pointer.
    static_assert(thunk_bytes_of(&::CryptAcquireContextA) == 20, "CryptAcquireContextA");
    GH_X86_BED(R, "advapi32.dll", "CryptAcquireContextA", 20,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[0]) { const uint32_t z = 0; std::memcpy((void*)(uintptr_t)a[0], &z, 4); }
            SetLastError(NTE_NOT_FOUND);
            return 0;
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
// Registration
// ---------------------------------------------------------------------------
inline void RegisterStdBeds(Machine& m)
{
    m.dll_loaded_hook = &mono_probe::install;
    ThunkRegistry& R = m.registry;
    Machine* mp = &m;

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
    GH_X86_FWD(R, "kernel32.dll", DeleteFileA);
    GH_X86_FWD(R, "kernel32.dll", DeleteFileW);
    // Beds rather than forwards purely so a FAILED open is always logged, no
    // matter how long ago the startup trace budget ran out. A guest that cannot
    // open a file it needs almost always reports that as something vaguer
    // further downstream ("assembly not found", "could not load"), and the
    // failed open is the fact that actually explains it. Successful opens stay
    // on the ordinary trace, because there are thousands of them.
    GH_X86_BED(R, "kernel32.dll", "CreateFileW", 28,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            HANDLE h = CreateFileW((LPCWSTR)(uintptr_t)a[0], a[1], a[2],
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
            HANDLE h = CreateFileA((LPCSTR)(uintptr_t)a[0], a[1], a[2],
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

    GH_X86_BED(R, "kernel32.dll", "GetCommandLineA", 0,
        [](Machine& m, const uint32_t*) -> uint64_t {
            BedState& b = m.bed;
            if (!b.cmdline_a) b.cmdline_a = guest_strdup_a(m, "guest.exe");
            return b.cmdline_a;
        });
    GH_X86_BED(R, "kernel32.dll", "GetCommandLineW", 0,
        [](Machine& m, const uint32_t*) -> uint64_t {
            BedState& b = m.bed;
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
            log("X86 GetModuleHandleA(" + want + ") -> 0 (nothing implements it)");
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
            log("X86 GetModuleHandleW(" + nm + ") -> 0 (nothing implements it)");
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
            if (s) log(std::string("X86 dbg: ") + s);
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

    // OVERLAPPED is 20 bytes on x86 and 32 on x64. Rather than translate it,
    // refuse it: no guest reaches a menu through asynchronous file I/O, and a
    // named refusal is worth more than a silent 12-byte overwrite. The
    // synchronous path -- the only one anything has used so far -- is unchanged.
    GH_X86_BED(R, "kernel32.dll", "ReadFile", 20,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (a[4]) log("X86 ReadFile with OVERLAPPED -- unsupported; forcing synchronous");
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
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[4]) log("X86 WriteFile with OVERLAPPED -- unsupported; forcing synchronous");
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
        [](Machine&, const uint32_t* a) -> uint64_t {
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
        env_set(m, L"NUMBER_OF_PROCESSORS", L"4");
        env_set(m, L"PROCESSOR_ARCHITECTURE", L"x86");
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
            const std::wstring* v = env_find(m, widen((const char*)(uintptr_t)a[0]));
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
    GH_X86_BED(R, "user32.dll", "RegisterClassW", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (a[0]) {
                const uint32_t proc = *(const uint32_t*)(uintptr_t)(a[0] + 4);
                if (proc) { m.bed.wndproc = proc; log("X86 RegisterClassW captured WndProc " + hex(proc)); }
            }
            return 1;                                   // any non-zero ATOM
        });
    GH_X86_BED(R, "user32.dll", "RegisterClassExW", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (a[0]) {
                const uint32_t proc = *(const uint32_t*)(uintptr_t)(a[0] + 8);
                if (proc) { m.bed.wndproc = proc; log("X86 RegisterClassExW captured WndProc " + hex(proc)); }
            }
            return 1;
        });
    GH_X86_BED(R, "user32.dll", "RegisterClassA", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (a[0]) {
                const uint32_t proc = *(const uint32_t*)(uintptr_t)(a[0] + 4);
                if (proc) m.bed.wndproc = proc;
            }
            return 1;
        });
    GH_X86_BED(R, "user32.dll", "RegisterClassExA", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (a[0]) {
                const uint32_t proc = *(const uint32_t*)(uintptr_t)(a[0] + 8);
                if (proc) m.bed.wndproc = proc;
            }
            return 1;
        });
    GH_X86_BED(R, "user32.dll", "UnregisterClassW", 8,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });

    // CreateWindowExW takes 12 arguments; getting that count wrong is a silent
    // 4-byte stack drift per call, so it is asserted like the WS2_32 table.
    static_assert(thunk_bytes_of(&::CreateWindowExW) == 48, "CreateWindowExW");
    GH_X86_BED(R, "user32.dll", "CreateWindowExW", 48,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            (void)m;
            log("X86 CreateWindowExW " + std::to_string((int32_t)a[6]) + "x" +
                std::to_string((int32_t)a[7]) + " -> synthetic HWND " + hex(kFakeHwnd));
            return kFakeHwnd;
        });
    GH_X86_BED(R, "user32.dll", "CreateWindowExA", 48,
        [](Machine&, const uint32_t*) -> uint64_t { return kFakeHwnd; });

    // The rest of the window lifecycle: there is nothing to do, and saying so
    // truthfully (TRUE, "it worked") is what keeps the guest on its happy path.
    for (const char* n : { "DestroyWindow", "UpdateWindow", "SetForegroundWindow",
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

    // GetWindowLong/SetWindowLong on a window that does not exist. Returning 0
    // and accepting every store is right for the two things a player uses them
    // for: reading GWL_STYLE (it only ever compares against what it set) and
    // stashing a `this` pointer in GWLP_USERDATA, which it re-reads through the
    // same pair. Anything more would need a real window object; nothing asks.
    for (const char* n : { "GetWindowLongA", "GetWindowLongW" })
        GH_X86_BED(R, "user32.dll", n, 8,
            [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    for (const char* n : { "SetWindowLongA", "SetWindowLongW" })
        GH_X86_BED(R, "user32.dll", n, 12,
            [](Machine&, const uint32_t*) -> uint64_t { return 0; });

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
                return peek_message32(m, a[0], (a[4] & PM_REMOVE) != 0);
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
                if (!a[0] || !m.bed.wndproc) return 0;
                const uint32_t* msg = (const uint32_t*)(uintptr_t)a[0];
                uint32_t args[4] = { msg[0], msg[1], msg[2], msg[3] };   // hwnd, message, wParam, lParam
                return m.call_guest32(m.bed.wndproc, args, 4);
            });
    GH_X86_BED(R, "user32.dll", "DefWindowProcW", 16,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "user32.dll", "DefWindowProcA", 16,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });

    // SendMessage delivers synchronously to the window procedure, so it is the
    // same host->guest hop as dispatch and cannot be a forward either.
    GH_X86_BED(R, "user32.dll", "SendMessageW", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!m.bed.wndproc) return 0;
            uint32_t args[4] = { a[0], a[1], a[2], a[3] };
            return m.call_guest32(m.bed.wndproc, args, 4);
        });
    GH_X86_BED(R, "user32.dll", "SendMessageA", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!m.bed.wndproc) return 0;
            uint32_t args[4] = { a[0], a[1], a[2], a[3] };
            return m.call_guest32(m.bed.wndproc, args, 4);
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

    // The eighteen entry points the rule-10 audit named. Registered here, ahead
    // of the generated forward table, so the beds shadow the forwards that
    // would otherwise write eight bytes through four-byte guest objects.
    RegisterPtrToPtrBeds(R);

    // DXGI and D3D11. Kept in their own file: they are not Win32 calls but a
    // COM vtable boundary, and the marshalling they need has nothing in common
    // with the rest of this table.
    RegisterD3DBeds(R);

    log("X86 beds registered: " + std::to_string(R.size()));
}

}   // namespace ghx86
