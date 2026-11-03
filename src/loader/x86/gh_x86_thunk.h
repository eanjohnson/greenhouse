// gh_x86_thunk.h -- the guest-x86 -> host-x64 calling boundary.
//
// A 32-bit guest's imports CANNOT resolve against real OS DLLs: there is no
// 32-bit kernel32 in this process and never will be. Every single import has to
// land in our code. That sounds like a cost and is actually the reuse story --
// it means one uniform boundary that we control, through which the existing x64
// beds are already reachable.
//
// The mechanics:
//   * Each import gets a 16-byte slot in a "thunk region" of guest memory. The
//     IAT entry points at the slot. There is no code there -- Cpu::step() sees
//     EIP land in the region and returns Status::HostCall, so a guest cannot
//     skip past the boundary by jumping into the middle of a trampoline.
//   * On dispatch, the guest stack is [esp]=return address, [esp+4...]=args.
//     We read the args, call the host function, put the result in EAX (EDX:EAX
//     for 64-bit returns), pop the frame per the calling convention, and set EIP
//     back to the return address.
//
// Argument counts are NOT guessed. Each bed is declared with its real C
// signature and the marshaller derives the 32-bit stack layout from the types at
// compile time -- so a stdcall bed's "bytes to pop" is correct by construction
// rather than by table lookup. That matters: get it wrong and the guest's stack
// silently drifts, which surfaces as a crash thousands of instructions later.
//
// Kept free of windows.h so the marshaller itself is unit-testable natively; the
// beds that actually call Win32 live in gh_x86_beds.h.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "gh_x86_cpu.h"

#if defined(_WIN32)
#  define GH_X86_STDCALL __stdcall
#else
#  define GH_X86_STDCALL          // ignored on x86-64 SysV; lets tests build on Linux
#endif

namespace ghx86 {

struct Machine;   // defined in gh_x86_win.h

// ---------------------------------------------------------------------------
// 32-bit argument layout
// ---------------------------------------------------------------------------
// On the 32-bit Windows ABI every argument occupies a whole number of 4-byte
// stack slots. A pointer is ONE slot even though the host type is 8 bytes --
// getting that wrong is the single easiest way to corrupt a call, so pointers
// are handled explicitly rather than by sizeof().
//
// The same is true of a pointer-sized INTEGER -- SIZE_T, ULONG_PTR, DWORD_PTR,
// LPARAM, WPARAM, LRESULT, INT_PTR -- which is 8 bytes on the x64 host and 4 on
// the guest. Missing that cost a long hunt: VirtualProtect(LPVOID, SIZE_T,
// DWORD, PDWORD) derived a 20-byte stdcall cleanup for a call the guest had
// pushed 16 bytes for, so every VirtualProtect left the guest's ESP four bytes
// high. mono_mprotect's own `pop esi` then restored a stale ESI, mini_init
// (which keeps a zero there and compares debug_options.gdb against it) decided
// xdebug was on, and mono walked into an AOT image writer this build does not
// contain -- six million instructions after the actual mistake.
//
// The catch: on x64 SIZE_T and ULONGLONG are the SAME TYPE (unsigned __int64),
// so no template can tell "pointer-sized" from "genuinely 64-bit" apart. The
// rule below assumes pointer-sized, because across the Win32 surface a 32-bit
// game touches that is overwhelmingly the common case -- every heap call, every
// window message, every Virtual* call. An entry point that really does take a
// 64-bit value must be a hand-written bed with an explicit pop size instead;
// RegisterGeneratedBeds logs every forward this rule changes so that list can
// be read rather than assumed.
template <typename T>
struct Arg32 {
    static constexpr bool ptr_sized_int =
        (std::is_integral_v<T> || std::is_enum_v<T>) && sizeof(T) == 8;

    static constexpr int slots =
        (std::is_pointer_v<T> || ptr_sized_int) ? 1 : (int)((sizeof(T) + 3) / 4);

    static T get(const uint32_t* p)
    {
        if constexpr (ptr_sized_int) {
            // One guest slot widened to the host type. Signed types must sign-
            // extend: an LPARAM of -1 is -1 at both widths, and zero-extending
            // it hands the host 0x00000000FFFFFFFF.
            if constexpr (std::is_signed_v<T>) return (T)(int64_t)(int32_t)p[0];
            else                               return (T)(uint64_t)p[0];
        } else if constexpr (std::is_pointer_v<T>) {
            // Guest pointer == host pointer (the M0 sub-4GB property). A zero
            // guest pointer must stay null, not become mem_base.
            //
            // The exception is a pseudo-handle. GetCurrentProcess/Thread return
            // the constants -1 and -2, which are -1 and -2 at BOTH widths -- so
            // they must be sign-extended, not zero-extended, or the host sees
            // 0x00000000FFFFFFFF and rejects it as an invalid handle. This is
            // safe to do unconditionally because the guest's address space ends
            // far below 4GB: no real guest pointer can have these values.
            if (p[0] >= 0xFFFFFFF0u) return (T)(intptr_t)(int32_t)p[0];
            return (T)(uintptr_t)p[0];
        } else {
            T v{};
            std::memcpy(&v, p, sizeof(T));
            return v;
        }
    }
};

// Reference parameters are passed as pointers by the ABI, so they occupy one
// slot and dereference to the guest's object. Common in COM signatures
// (`const GUID&`), where the default sizeof()-based path would try to
// default-construct a reference.
template <typename T>
struct Arg32<T&> {
    static constexpr bool ptr_sized_int = false;
    static constexpr int slots = 1;
    static T& get(const uint32_t* p) { return *(T*)(uintptr_t)p[0]; }
};

// Is this type something the host stores in 8 bytes and the guest in 4?
// Split in two so `sizeof` is only ever asked of an integral or enum type,
// which is always complete -- asking it of an opaque Win32 struct would not
// compile.
template <typename P, bool = std::is_integral_v<P> || std::is_enum_v<P>>
struct EightByteScalar { static constexpr bool value = false; };
template <typename P>
struct EightByteScalar<P, true> { static constexpr bool value = sizeof(P) == 8; };

// An argument that is a POINTER TO something pointer-sized: HANDLE* (i.e.
// void**), SIZE_T*, ULONG_PTR*, LPVOID*, char**. The pointer itself marshals
// perfectly -- it is one guest slot and the address is valid on both sides,
// which is exactly why the other two audits see nothing wrong with it. What
// breaks is what the host does THROUGH it: it writes eight bytes into an
// object the guest allocated four for, silently destroying whatever the guest
// put in the next four. On a stack out-parameter that neighbour is a saved
// register or a return address.
template <typename T>
struct PtrToPtrSized {
    using P = std::remove_cv_t<std::remove_pointer_t<T>>;
    static constexpr bool value =
        std::is_pointer_v<T> && (std::is_pointer_v<P> || EightByteScalar<P>::value);
};

template <typename... A>
struct ArgLayout {
    static constexpr int n = (int)sizeof...(A);
    static constexpr int slot_of[(n > 0 ? n : 1)] = { Arg32<A>::slots... };

    static constexpr int offset(int i)
    {
        int s = 0;
        for (int k = 0; k < i; ++k) s += slot_of[k];
        return s;
    }
    static constexpr uint32_t bytes()
    {
        int s = 0;
        for (int k = 0; k < n; ++k) s += slot_of[k];
        return (uint32_t)s * 4;
    }

    // True when some argument is an 8-byte integer that this layout has assumed
    // is pointer-sized. Reported at registration so the assumption is visible;
    // see the note on Arg32.
    static constexpr bool has_ptr_sized_int()
    {
        return (false || ... || Arg32<A>::ptr_sized_int);
    }

    // True when some argument is a function pointer -- i.e. a CALLBACK. Such a
    // function must never be a forward: the pointer is GUEST code, and handing
    // it to the host means the host CPU jumps to a 32-bit address and executes
    // whatever x86 is there. That is not a subtle failure (it is an immediate
    // access violation on the host), but it is a silent one to WRITE, because
    // the signature compiles perfectly. CreateThread was forwarded for exactly
    // this reason and took the process down inside mono's GC startup.
    static constexpr bool has_callback()
    {
        return (false || ... ||
                (std::is_pointer_v<A> &&
                 std::is_function_v<std::remove_pointer_t<std::remove_cv_t<A>>>));
    }

    // See PtrToPtrSized. Cost a hunt on its own: DuplicateHandle's
    // lpTargetHandle is &[ebp-4] in the CRT's dup2, so the host's 8-byte write
    // put the handle's zero high half straight over the saved EBP, and `leave`
    // popped a null frame pointer 140 million instructions into startup.
    static constexpr bool has_ptr_to_ptr_sized()
    {
        return (false || ... || PtrToPtrSized<A>::value);
    }
};
template <typename... A>
constexpr int ArgLayout<A...>::slot_of[];

// ---------------------------------------------------------------------------
// A host implementation of one guest import.
// ---------------------------------------------------------------------------
struct HostFn {
    const char* dll  = nullptr;      // lowercase with extension, e.g. "kernel32.dll"
    const char* name = nullptr;
    uint32_t    stack_bytes = 0;     // callee-cleaned bytes (0 for cdecl)
    bool        caller_cleans = false;
    // True when the host function returns a pointer. A host pointer does not
    // fit in a 32-bit guest register, so such a function must allocate from
    // guest memory -- i.e. be a BED, not a FORWARD. The dispatcher checks the
    // returned value against 4GB and reports by name, turning what would be a
    // silent truncation into a diagnostic.
    bool        returns_pointer = false;
    // True when the derived stack layout treated an 8-byte integer argument as
    // pointer-sized (see Arg32). Correct for SIZE_T/ULONG_PTR/LPARAM and wrong
    // for a genuine 64-bit argument, so these are listed at startup.
    bool        ptr_sized_int_args = false;
    // True when an argument is a function pointer, i.e. a guest callback. Such
    // an entry point MUST be a bed; see ArgLayout::has_callback.
    bool        callback_args = false;

    // True when some argument is a pointer to a pointer-sized object, so the
    // host reads or writes 8 bytes through a guest object that is 4 wide.
    // See ArgLayout::has_ptr_to_ptr_sized.
    bool        ptr_to_ptr_sized_args = false;

    // True when invoke touches nothing in the Machine, so the native runtime
    // (gh_x86_native.h) may run it on the calling thread without the bed lock.
    // Every forward qualifies; a bed has to say so.
    bool        lockless = false;

    // args points at [esp+4] in guest memory. Returns the 64-bit result; the
    // dispatcher splits it into EDX:EAX.
    uint64_t (*invoke)(Machine&, const uint32_t* args) = nullptr;
};

// -- typed forwarding -------------------------------------------------------
namespace detail {

template <typename R, typename... A, size_t... I>
uint64_t call_typed(R(GH_X86_STDCALL* fn)(A...), const uint32_t* sp, std::index_sequence<I...>)
{
    if constexpr (std::is_void_v<R>) {
        fn(Arg32<A>::get(sp + ArgLayout<A...>::offset((int)I))...);
        return 0;
    } else {
        R r = fn(Arg32<A>::get(sp + ArgLayout<A...>::offset((int)I))...);
        if constexpr (std::is_pointer_v<R>) {
            // A host pointer handed back to a 32-bit guest MUST be below 4GB.
            // Anything that can allocate is a hand-written bed that allocates
            // from guest memory; this catches the case where one slipped
            // through as a plain forward.
            return (uint64_t)(uintptr_t)r;
        } else {
            uint64_t v = 0;
            std::memcpy(&v, &r, sizeof(R) > 8 ? 8 : sizeof(R));
            return v;
        }
    }
}

}   // namespace detail

// Wraps a host function of matching signature into a HostFn. The stack size is
// computed from the parameter types, so it cannot disagree with reality.
template <typename R, typename... A>
struct StdcallThunk {
    using Fn = R(GH_X86_STDCALL*)(A...);
    static constexpr uint32_t bytes = ArgLayout<A...>::bytes();
};

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
// Beds are registered by (dll, name). The dll is matched case-insensitively and
// is advisory: as in the x64 loader's ShimTable, a name match wins regardless of
// which DLL the guest claims it comes from, because guests import the same
// function from api-ms-win-* apisets, kernel32 and kernelbase interchangeably.
class ThunkRegistry {
public:
    // First registration of a name wins. RegisterStdBeds runs before the bulk
    // generated table, so a hand-written bed always shadows the same-named
    // forward -- and does so by rule rather than by search order, which matters
    // because the generated table is machine-written and will keep re-emitting
    // forwards for names that have since become beds.
    void add(const HostFn& f)
    {
        for (size_t i = 0; i < fns_.size(); ++i)
            if (std::strcmp(fns_[i].name, f.name) == 0) { dropped_.push_back({ f.name, i }); return; }
        fns_.push_back(f);
    }

    // What add() threw away, and the index of the entry that kept the name.
    // Shadowing a forward or a green-thread bed is the point of the rule; one
    // hand-written bed shadowing another is dead code nobody sees (HL2 s860:
    // two gdi32 beds hid the DirectWrite ones, so vgui never made a font).
    struct Dropped { const char* name; size_t winner; };
    const std::vector<Dropped>& dropped() const { return dropped_; }

    const HostFn* find(const std::string& dll, const std::string& name) const
    {
        (void)dll;
        for (const auto& f : fns_)
            if (name == f.name) return &f;
        return nullptr;
    }

    // Whether anything at all is implemented for this DLL. GetModuleHandle uses
    // it to decide whether the guest may hold a handle to it.
    bool has_dll(const std::string& dll) const
    {
        for (const auto& f : fns_) if (dll == f.dll) return true;
        return false;
    }

    size_t size() const { return fns_.size(); }
    const std::vector<HostFn>& all() const { return fns_; }

private:
    std::vector<HostFn> fns_;
    std::vector<Dropped> dropped_;
};

// Strips the 32-bit decoration a guest might carry: `_Foo@8` (stdcall) or
// `@Foo@8` (fastcall). System DLLs export undecorated names, so this is
// defensive -- but a statically-linked helper lib can carry decorated imports
// and a silent miss would look like a missing export.
inline std::string undecorate(const std::string& s)
{
    std::string t = s;
    if (!t.empty() && (t[0] == '_' || t[0] == '@')) {
        size_t at = t.rfind('@');
        if (at != std::string::npos && at > 0) {
            bool digits = at + 1 < t.size();
            for (size_t i = at + 1; i < t.size() && digits; ++i)
                if (t[i] < '0' || t[i] > '9') digits = false;
            if (digits) return t.substr(1, at - 1);
        }
        if (t[0] == '_') return t.substr(1);
    }
    return t;
}

}   // namespace ghx86

// ---------------------------------------------------------------------------
// Declaration macros
// ---------------------------------------------------------------------------
// GH_X86_FWD  -- forward straight to the host function of the same name and
//                signature. Use for anything whose x64 semantics are already
//                correct for a 32-bit caller.
// GH_X86_BED  -- a hand-written implementation, for anything that returns a
//                pointer (must come from guest memory), takes a callback, or
//                needs Greenhouse's own behaviour.
#define GH_X86_FWD(REG, DLL, FUNC)                                                    \
    do {                                                                              \
        using T = decltype(&FUNC);                                                    \
        ghx86::HostFn f;                                                              \
        f.dll = DLL; f.name = #FUNC;                                                  \
        f.stack_bytes = ghx86::thunk_bytes_of((T)nullptr);                            \
        f.returns_pointer = ghx86::thunk_returns_pointer((T)nullptr);                 \
        f.ptr_sized_int_args = ghx86::thunk_has_ptr_sized_int((T)nullptr);            \
        f.callback_args = ghx86::thunk_has_callback((T)nullptr);                      \
        f.ptr_to_ptr_sized_args = ghx86::thunk_has_ptr_to_ptr_sized((T)nullptr);      \
        f.lockless = true;                                                            \
        f.invoke = [](ghx86::Machine&, const uint32_t* sp) -> uint64_t {              \
            return ghx86::detail::call_typed(&FUNC, sp,                               \
                       std::make_index_sequence<ghx86::thunk_argc_of((T)nullptr)>{}); \
        };                                                                            \
        (REG).add(f);                                                                 \
    } while (0)

// Variadic on purpose: a bed body is a lambda, and any comma not inside
// parentheses -- a multi-declarator `uint32_t a = x, b = y;`, a braced init
// `Mod m{p, q};` -- would otherwise be read as another macro argument.
// GH_X86_FWD_DYN -- like GH_X86_FWD, but resolves the host function with
// GetProcAddress at registration time instead of importing it statically.
//
// This is the default for the bulk-generated table, for two reasons. First, a
// static import would make the HOST binary depend on that DLL, and several of
// the DLLs a guest imports are simply absent from the Xbox container
// (opengl32, winmm, d3d9...) -- the host would then fail to load at all, which
// is a far worse failure than a missing import. Second, it keeps the link line
// fixed no matter how large the forward table grows. A DLL that is not present
// just yields no registration, and the import reports itself by name when the
// guest calls it.
#define GH_X86_FWD_DYN(REG, DLL, FUNC)                                                \
    do {                                                                              \
        using T = decltype(&FUNC);                                                    \
        static T pfn_ = (T)ghx86::dyn_resolve(DLL, #FUNC);                            \
        if (pfn_) {                                                                   \
            ghx86::HostFn f;                                                          \
            f.dll = DLL; f.name = #FUNC;                                              \
            f.stack_bytes = ghx86::thunk_bytes_of((T)nullptr);                        \
            f.returns_pointer = ghx86::thunk_returns_pointer((T)nullptr);             \
            f.ptr_sized_int_args = ghx86::thunk_has_ptr_sized_int((T)nullptr);        \
            f.callback_args = ghx86::thunk_has_callback((T)nullptr);                  \
            f.ptr_to_ptr_sized_args = ghx86::thunk_has_ptr_to_ptr_sized((T)nullptr);  \
            f.lockless = true;                                                        \
            f.invoke = [](ghx86::Machine&, const uint32_t* sp) -> uint64_t {          \
                return ghx86::detail::call_typed(pfn_, sp,                            \
                    std::make_index_sequence<ghx86::thunk_argc_of((T)nullptr)>{});    \
            };                                                                        \
            (REG).add(f);                                                             \
        }                                                                             \
    } while (0)

#define GH_X86_BED(REG, DLL, NAME, POPBYTES, ...)                                     \
    do {                                                                              \
        ghx86::HostFn f;                                                              \
        f.dll = DLL; f.name = NAME; f.stack_bytes = (POPBYTES);                       \
        f.invoke = __VA_ARGS__;                                                       \
        (REG).add(f);                                                                 \
    } while (0)

// Same, but cdecl: the CALLER pops, so the thunk must not. Used for the varargs
// entry points (wsprintf and friends), which cannot be stdcall precisely because
// the callee does not know how many arguments there were.
#define GH_X86_BED_CDECL(REG, DLL, NAME, ...)                                         \
    do {                                                                              \
        ghx86::HostFn f;                                                              \
        f.dll = DLL; f.name = NAME; f.stack_bytes = 0; f.caller_cleans = true;         \
        f.invoke = __VA_ARGS__;                                                       \
        (REG).add(f);                                                                 \
    } while (0)

namespace ghx86 {

// Helpers so the macros can ask a function pointer type for its 32-bit stack
// footprint and argument count without naming the parameter pack twice.
template <typename R, typename... A>
constexpr uint32_t thunk_bytes_of(R(GH_X86_STDCALL*)(A...)) { return ArgLayout<A...>::bytes(); }

template <typename R, typename... A>
constexpr size_t thunk_argc_of(R(GH_X86_STDCALL*)(A...)) { return sizeof...(A); }

template <typename R, typename... A>
constexpr bool thunk_returns_pointer(R(GH_X86_STDCALL*)(A...)) { return std::is_pointer_v<R>; }

template <typename R, typename... A>
constexpr bool thunk_has_ptr_sized_int(R(GH_X86_STDCALL*)(A...))
{
    return ArgLayout<A...>::has_ptr_sized_int();
}

template <typename R, typename... A>
constexpr bool thunk_has_callback(R(GH_X86_STDCALL*)(A...))
{
    return ArgLayout<A...>::has_callback();
}

template <typename R, typename... A>
constexpr bool thunk_has_ptr_to_ptr_sized(R(GH_X86_STDCALL*)(A...))
{
    return ArgLayout<A...>::has_ptr_to_ptr_sized();
}

}   // namespace ghx86
