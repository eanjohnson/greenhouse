// gh_x86_com.h -- COM vtable thunking for a 32-bit guest.
//
// A D3D11 or DXGI interface is a vtable of 64-bit method pointers. A 32-bit
// guest cannot hold one of those, so it cannot be handed a host interface at
// all -- not truncated, not tagged, not any other trick. What it can be handed
// is a 16-byte object OF OUR OWN in guest memory whose first dword points at a
// guest-side vtable, every slot of which is a thunk address. The guest's
// `pDevice->lpVtbl->CreateBuffer(pDevice, ...)` then lands in the thunk region
// exactly like an import does, Cpu::step() returns Status::HostCall, and the
// dispatcher finds a HostFn that knows which host method this slot means.
//
//   guest object (16 bytes, in guest memory)      guest vtable (4 bytes/slot)
//   +0  vtbl  -------------------------------->   [0] thunk -> QueryInterface
//   +4  host_lo \  the real host interface         [1] thunk -> AddRef
//   +8  host_hi /  pointer, split in two           [2] thunk -> Release
//   +12 tag        'CO' | interface id             ...
//
// Three things make this safe rather than merely plausible:
//
//   * The vtable ORDER is generated from the SDK headers and cross-checked
//     against MIDL's own C vtable structs (toolchain/gen_com_tables.py). A
//     mis-ordered slot calls the wrong method with the right arguments, which
//     is the worst failure this layer could produce and the hardest to see.
//
//   * Interface ARGUMENTS are unwrapped automatically. `OMSetRenderTargets`
//     receives guest objects, and passing one to the host as if it were an
//     ID3D11RenderTargetView would have the host read our tag as a vtable
//     pointer. Any parameter whose type derives from IUnknown goes through
//     unwrap(), by type, with no per-method decision to forget.
//
//   * Interface RESULTS are wrapped automatically. Nearly every COM method
//     returns its result through a trailing `X**`, which is rule 10 (a host
//     8-byte write through a 4-byte guest slot) in its purest form -- so the
//     trailing out-pointer is recognised by type, the host writes into a HOST
//     local, and only the wrapped 32-bit handle reaches guest memory.
//
// What cannot be automated is flagged rather than guessed. A method that
// returns a raw host pointer, that writes 8 bytes through some OTHER argument,
// that passes a struct holding a pointer (different size AND offsets on each
// side), or that takes a counted array of interface pointers is POISONED at
// registration: calling it logs the reason once and returns E_FAIL instead of
// corrupting the guest. Overriding it by hand clears the poison. The list of
// still-poisoned methods is printed at startup and is the remaining work.

#pragma once

#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "gh_x86_win.h"

namespace ghx86 {

// Tag in the guest object's last dword: 'CO' in the high half, the interface id
// in the low. Checked on every unwrap, so a guest that hands us a stack address
// or a stale pointer is reported rather than dereferenced.
constexpr uint32_t kComTag  = 0x434F0000u;
constexpr uint32_t kComMask = 0xFFFF0000u;

struct ComObj32 { uint32_t vtbl, host_lo, host_hi, tag; };

// Maps a host interface TYPE to its registered interface id, so that a wrapped
// out-parameter knows which vtable to give the guest without the call site
// having to say. Set by com_register_<Iface>(); -1 until then, which is how an
// unregistered interface reports itself instead of silently handing back a
// vtable for something else.
template <typename T> struct ComTypeId { static inline int v = -1; };

// -- type traits -------------------------------------------------------------
// Split so is_base_of is only ever asked about a class type. Asked about
// `char` (from LPCSTR) it would not compile, and && does not short-circuit
// template instantiation.
template <typename P, bool = std::is_class_v<P>>
struct DerivesUnknown { static constexpr bool value = false; };
template <typename P>
struct DerivesUnknown<P, true> {
    static constexpr bool value = std::is_base_of_v<IUnknown, P>;
};

// `ID3D11Buffer*` -- an interface passed IN, which must be unwrapped.
template <typename T>
struct IsIfacePtr {
    using P = std::remove_cv_t<std::remove_pointer_t<T>>;
    static constexpr bool value = std::is_pointer_v<T> && DerivesUnknown<P>::value;
};

// `ID3D11Buffer**` -- an interface returned OUT, which must be wrapped.
template <typename T>
struct IsOutIfacePP {
    using P = std::remove_cv_t<std::remove_pointer_t<T>>;
    static constexpr bool value = std::is_pointer_v<T> && IsIfacePtr<P>::value;
};

template <bool Empty, typename... A> struct LastArg { using type = void; };
template <typename... A>
struct LastArg<false, A...> {
    using type = std::tuple_element_t<sizeof...(A) - 1, std::tuple<A...>>;
};

// -- the registry ------------------------------------------------------------
struct ComIfaceDesc {
    std::string        name;
    const void*        iid = nullptr;      // 16 bytes, compared with memcmp
    std::deque<HostFn> methods;            // deque: HostFn* handed to make_thunk
    std::vector<std::string> mnames;       // short names, parallel to methods
    std::vector<std::pair<std::string, std::string>> poisoned;   // name, why
    uint32_t           vtbl = 0;           // guest address, 0 until materialised
};

struct ComState {
    std::deque<ComIfaceDesc> ifaces;
    std::deque<std::string>  strings;      // stable storage for HostFn::name
    // Host interface pointer + interface id -> guest object. COM identity is
    // observable: a guest that fetches the same back buffer twice compares the
    // two pointers, so the same host object must always come back as the same
    // guest address. Keyed by id as well as pointer because QueryInterface for
    // a different interface on the same object legitimately yields a different
    // host pointer -- and, on a multiply-inherited implementation, sometimes
    // the same one.
    std::map<std::pair<const void*, int>, uint32_t> live;
    uint64_t wraps = 0, unwrap_bad = 0;
    bool     built = false;
    // Set on first wrap; unwrap needs it to bounds-check a guest address
    // before reading the tag out of it.
    Machine* mach = nullptr;

    const char* intern(const std::string& s)
    {
        strings.push_back(s);
        return strings.back().c_str();
    }

    int add(const char* name, const void* iid)
    {
        ifaces.emplace_back();
        ifaces.back().name = name;
        ifaces.back().iid  = iid;
        return (int)ifaces.size() - 1;
    }

    void add_method(int id, const char* mname, const HostFn& f, const char* why)
    {
        ComIfaceDesc& d = ifaces[(size_t)id];
        d.methods.push_back(f);
        d.mnames.push_back(mname);
        if (why) d.poisoned.emplace_back(mname, why);
    }

    void poison(int id, const char* mname, const char* why)
    {
        ifaces[(size_t)id].poisoned.emplace_back(mname, why);
    }

    bool is_poisoned(int id, const std::string& mname) const
    {
        for (const auto& p : ifaces[(size_t)id].poisoned)
            if (p.first == mname) return true;
        return false;
    }

    // Replaces one method with a hand-written marshaller and clears its poison.
    // Returns false if the name is not in the interface at all, which is a typo
    // rather than a runtime condition -- reported at registration, where it is
    // cheap to see, rather than at the call.
    bool override_method(int id, const char* mname, uint32_t stack_bytes,
                         uint64_t (*invoke)(Machine&, const uint32_t*))
    {
        ComIfaceDesc& d = ifaces[(size_t)id];
        for (size_t i = 0; i < d.mnames.size(); ++i) {
            if (d.mnames[i] != mname) continue;
            d.methods[i].stack_bytes = stack_bytes;
            d.methods[i].invoke      = invoke;
            // EVERY matching entry, not the first. A method can be poisoned
            // twice -- once by the header scan (a counted array of interfaces)
            // and once by the type-driven check (rule 10 sees the same `T**`) --
            // and clearing one of the two left PSSetShader and its five
            // siblings still refusing to run after they had been overridden.
            for (size_t k = d.poisoned.size(); k-- > 0;)
                if (d.poisoned[k].first == mname)
                    d.poisoned.erase(d.poisoned.begin() + (long)k);
            return true;
        }
        return false;
    }

    int id_of_iid(const void* iid) const
    {
        if (!iid) return -1;
        for (size_t i = 0; i < ifaces.size(); ++i)
            if (ifaces[i].iid && std::memcmp(ifaces[i].iid, iid, 16) == 0) return (int)i;
        return -1;
    }

    const char* name_of(int id) const
    {
        return (id >= 0 && (size_t)id < ifaces.size()) ? ifaces[(size_t)id].name.c_str()
                                                       : "<unregistered>";
    }

    // Mints the guest vtable for one interface. Lazy, per interface, on first
    // use: the full table is ~700 methods and a guest that never draws should
    // not spend 700 thunk slots to find that out.
    uint32_t materialize(Machine& m, int id);

    uint32_t wrap(Machine& m, void* host, int id);
    void*    unwrap(uint32_t g);
    void     forget(uint32_t g);

    // Every method still lacking a hand-written marshaller, wrapped the same way
    // the forward audits are. This list IS the remaining work.
    void log_poison_audit();
};

inline ComState& com()
{
    static ComState s;
    return s;
}

// -- the poisoned stub -------------------------------------------------------
// One shared function, but it can still name itself: the dispatcher has already
// put the thunk address in cpu.host_call_target, and the thunk slot carries the
// method name. Reported once per method -- a render loop would otherwise print
// the same line sixty times a second and bury everything after it.
inline uint64_t com_poisoned_call(Machine& m, const uint32_t*)
{
    static std::vector<uint32_t> seen;
    const uint32_t at = m.cpu.host_call_target;
    for (uint32_t s : seen) if (s == at) return 0x80004005u;   // E_FAIL
    seen.push_back(at);
    std::string who = "?";
    if (m.cpu.thunk_end && at >= m.cpu.thunk_base && at < m.cpu.thunk_end) {
        size_t i = (at - m.cpu.thunk_base) / Machine::kThunkStride;
        if (i < m.thunks.size()) who = m.thunks[i].name;
    }
    log("X86 COM " + who + " has no hand-written marshaller -> E_FAIL");
    return 0x80004005u;
}

// -- marshalling -------------------------------------------------------------
namespace detail {

// An interface argument is a guest object and must be unwrapped; everything
// else marshals exactly as an OS forward does.
template <typename T, bool = IsIfacePtr<T>::value>
struct ComArg {
    static T get(Machine&, const uint32_t* p) { return Arg32<T>::get(p); }
};
template <typename T>
struct ComArg<T, true> {
    static T get(Machine& m, const uint32_t* p) { return (T)com().unwrap(p[0]); }
};
template <typename T>
struct ComArg<T&, false> {
    static T& get(Machine&, const uint32_t* p) { return *(T*)(uintptr_t)p[0]; }
};

template <typename S, typename C, typename R, typename... A, size_t... I>
uint64_t com_call(R (C::*pmf)(A...), S* self, Machine& m,
                  const uint32_t* sp, std::index_sequence<I...>)
{
    C* c = static_cast<C*>(self);
    if constexpr (std::is_void_v<R>) {
        (c->*pmf)(ComArg<A>::get(m, sp + ArgLayout<A...>::offset((int)I))...);
        return 0;
    } else {
        R r = (c->*pmf)(ComArg<A>::get(m, sp + ArgLayout<A...>::offset((int)I))...);
        uint64_t v = 0;
        std::memcpy(&v, &r, sizeof(R) > 8 ? 8 : sizeof(R));
        return v;
    }
}

// The trailing-out-interface form. The host writes into a HOST local of the
// right width; only the wrapped 32-bit handle is written into the guest's slot,
// and only four bytes of it. Doing this by type rather than per method is the
// point: there are ~60 of these across d3d11.h and dxgi.h, and each one written
// by hand would be another chance to write eight bytes into four.
template <typename S, typename C, typename R, typename... A, size_t... I>
uint64_t com_call_out(R (C::*pmf)(A...), S* self, Machine& m,
                      const uint32_t* sp, std::index_sequence<I...>)
{
    using Tup = std::tuple<A...>;
    constexpr size_t N = sizeof...(A);
    using OutP = std::remove_pointer_t<std::tuple_element_t<N - 1, Tup>>;   // X*
    using OutT = std::remove_cv_t<std::remove_pointer_t<OutP>>;            // X

    OutP out = nullptr;
    C* c = static_cast<C*>(self);
    // GetImmediateContext and friends return void and still hand back an
    // interface, so the result cannot be assumed to exist.
    uint64_t rv = 0;
    if constexpr (std::is_void_v<R>) {
        (c->*pmf)(ComArg<std::tuple_element_t<I, Tup>>::get(
                      m, sp + ArgLayout<A...>::offset((int)I))...,
                  &out);
    } else {
        R r = (c->*pmf)(ComArg<std::tuple_element_t<I, Tup>>::get(
                            m, sp + ArgLayout<A...>::offset((int)I))...,
                        &out);
        std::memcpy(&rv, &r, sizeof(R) > 8 ? 8 : sizeof(R));
    }

    const uint32_t slot = sp[ArgLayout<A...>::offset((int)N - 1)];
    if (slot) {
        uint32_t g = 0;
        if (out) {
            const int id = ComTypeId<OutT>::v;
            if (id < 0) log("X86 COM out-parameter of an unregistered interface");
            else        g = com().wrap(m, (void*)out, id);
        }
        // FOUR bytes. The guest allocated four; the host pointer is eight.
        std::memcpy((void*)(uintptr_t)slot, &g, 4);
    }
    return rv;
}

template <typename S, typename C, typename R, typename... A>
uint64_t com_invoke(R (C::*pmf)(A...), S* self, Machine& m, const uint32_t* sp)
{
    constexpr size_t N = sizeof...(A);
    using Last = typename LastArg<N == 0, A...>::type;
    if constexpr (N > 0 && IsOutIfacePP<Last>::value)
        return com_call_out(pmf, self, m, sp, std::make_index_sequence<N - 1>{});
    else
        return com_call(pmf, self, m, sp, std::make_index_sequence<N>{});
}

}   // namespace detail

// -- the IUnknown trio -------------------------------------------------------
// These three cannot go through the generic path, and not only because the C++
// IUnknown carries a TEMPLATE overload of QueryInterface that makes
// `&IFace::QueryInterface` ambiguous. QueryInterface returns an interface the
// caller named by IID, which is a type no signature carries; and Release has to
// retire the wrapper when the host object dies, or a later host object landing
// on the same address would be handed to the guest wearing the dead one's
// vtable.

// -- shared IID-out tail ------------------------------------------------------
// QueryInterface, GetParent and GetBuffer all end the same way: an HRESULT and
// a `void**` filled with an interface the caller named by IID. The type is not
// in the signature, so the wrapping has to go through the IID table.
inline uint64_t com_iid_out(Machine& m, uint32_t slot, const GUID* iid,
                            HRESULT hr, void* out)
{
    uint32_t g = 0;
    if (SUCCEEDED(hr) && out) {
        const int id = com().id_of_iid(iid);
        if (id < 0) {
            // Honest answer: we cannot give the guest a vtable we do not have.
            // Releasing first keeps the host refcount straight.
            ((IUnknown*)out)->Release();
            hr = E_NOINTERFACE;
            // Name it. "An unregistered interface" is a fact; the GUID is a
            // decision -- it says whether the answer is to add the interface or
            // to let the guest take its fallback path.
            static int told = 0;
            if (told++ < 16) {
                char b[48];
                std::snprintf(b, sizeof(b), "%08lX-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X",
                              (unsigned long)iid->Data1, iid->Data2, iid->Data3,
                              iid->Data4[0], iid->Data4[1], iid->Data4[2], iid->Data4[3],
                              iid->Data4[4], iid->Data4[5], iid->Data4[6], iid->Data4[7]);
                log(std::string("X86 COM no vtable for IID ") + b);
            }
        } else {
            g = com().wrap(m, out, id);
        }
    }
    if (slot) std::memcpy((void*)(uintptr_t)slot, &g, 4);
    return (uint64_t)(uint32_t)hr;
}

inline uint64_t com_qi(Machine& m, const uint32_t* a)
{
    auto* self = (IUnknown*)com().unwrap(a[0]);
    if (!self) return (uint64_t)(uint32_t)E_POINTER;
    const GUID* iid = (const GUID*)(uintptr_t)a[1];
    if (!iid || (com().mach && !com().mach->cpu.ptr(a[1], 16)))
        return (uint64_t)(uint32_t)E_INVALIDARG;
    void* out = nullptr;
    const HRESULT hr = self->QueryInterface(*iid, &out);
    return com_iid_out(m, a[2], iid, hr, out);
}

inline uint64_t com_release(Machine&, const uint32_t* a)
{
    auto* self = (IUnknown*)com().unwrap(a[0]);
    if (!self) return 0;
    const ULONG n = self->Release();
    // The host object is gone. Its address will be handed out again by the
    // allocator sooner or later, and a stale map entry would then give the
    // guest a wrapper typed as the dead interface.
    if (n == 0) com().forget(a[0]);
    return n;
}

inline uint64_t com_addref(Machine&, const uint32_t* a)
{
    auto* self = (IUnknown*)com().unwrap(a[0]);
    return self ? self->AddRef() : 0;
}

// -- compile-time classification --------------------------------------------
template <typename C, typename R, typename... A>
constexpr uint32_t com_bytes_of(R (C::*)(A...)) { return ArgLayout<A...>::bytes(); }

// Why this method cannot use the generic path, or nullptr. Everything here is
// decided from the TYPES, which is why it cannot be forgotten: the two shapes
// no template can see (a struct holding a pointer, a counted array of
// interfaces) come from the header scan instead and are poisoned by name.
template <typename C, typename R, typename... A>
constexpr const char* com_hand_marshal_reason(R (C::*)(A...))
{
    constexpr size_t N = sizeof...(A);
    if constexpr (std::is_pointer_v<R>)
        return "returns a host pointer, which does not fit in a guest register";
    else if constexpr (N == 0)
        return nullptr;
    else {
        using Last = typename LastArg<N == 0, A...>::type;
        // The trailing out-interface is handled, so it does not count against
        // the rule-10 check that would otherwise flag every Create* method.
        constexpr int npp = (0 + ... + (PtrToPtrSized<A>::value ? 1 : 0));
        constexpr int budget = IsOutIfacePP<Last>::value ? 1 : 0;
        if constexpr (npp > budget)
            return "writes 8 bytes through a 4-byte guest object (rule 10)";
        else
            return nullptr;
    }
}

// -- out-of-line, now that Machine is complete -------------------------------

inline uint32_t ComState::materialize(Machine& m, int id)
{
    ComIfaceDesc& d = ifaces[(size_t)id];
    if (d.vtbl) return d.vtbl;

    const uint32_t n = (uint32_t)d.methods.size();
    const uint32_t vt = m.heap_alloc(n * 4, true);
    if (!vt) { log("X86 COM no guest heap for " + d.name + "'s vtable"); return 0; }

    for (uint32_t i = 0; i < n; ++i) {
        // Poison is applied HERE rather than at registration so that an
        // override registered later still wins no matter what order the
        // registration file happens to run in.
        if (is_poisoned(id, d.mnames[i])) d.methods[i].invoke = &com_poisoned_call;
        const uint32_t t = m.make_thunk(d.name, d.name + "::" + d.mnames[i],
                                        &d.methods[i]);
        if (!t) { log("X86 COM thunk pool exhausted on " + d.name); return 0; }
        std::memcpy((void*)(uintptr_t)(vt + i * 4), &t, 4);
    }
    d.vtbl = vt;
    return vt;
}

inline uint32_t ComState::wrap(Machine& m, void* host, int id)
{
    if (!host) return 0;
    if (id < 0 || (size_t)id >= ifaces.size()) {
        log("X86 COM wrap of an unregistered interface id");
        return 0;
    }
    mach = &m;
    const auto key = std::make_pair((const void*)host, id);
    auto it = live.find(key);
    if (it != live.end()) return it->second;

    const uint32_t vt = materialize(m, id);
    if (!vt) return 0;
    const uint32_t g = m.heap_alloc(sizeof(ComObj32), true);
    if (!g) { log("X86 COM no guest heap for a " + ifaces[(size_t)id].name); return 0; }

    const uint64_t h = (uint64_t)(uintptr_t)host;
    ComObj32* o = (ComObj32*)(uintptr_t)g;
    o->vtbl    = vt;
    o->host_lo = (uint32_t)h;
    o->host_hi = (uint32_t)(h >> 32);
    o->tag     = kComTag | (uint32_t)id;

    live[key] = g;
    ++wraps;
    return g;
}

inline void* ComState::unwrap(uint32_t g)
{
    if (!g) return nullptr;
    // A guest that hands us a stack address, a freed object or plain garbage
    // gets a report, not a dereferenced host pointer built out of whatever was
    // there. The bounds check comes first: the tag read itself must be safe.
    if (mach && !mach->cpu.ptr(g, sizeof(ComObj32))) {
        if (++unwrap_bad <= 8) log("X86 COM unwrap of unreadable " + hex(g));
        return nullptr;
    }
    const ComObj32* o = (const ComObj32*)(uintptr_t)g;
    if ((o->tag & kComMask) != kComTag) {
        if (++unwrap_bad <= 8)
            log("X86 COM unwrap of " + hex(g) + ": not a COM object (tag " +
                hex(o->tag) + ")");
        return nullptr;
    }
    return (void*)(uintptr_t)(((uint64_t)o->host_hi << 32) | o->host_lo);
}

// Release dropped the last reference: the host object is gone, so the guest
// object must stop naming it. Leaving the map entry would hand a LATER host
// object that lands on the same address a guest wrapper typed as the dead one.
inline void ComState::forget(uint32_t g)
{
    if (!g) return;
    for (auto it = live.begin(); it != live.end(); ++it)
        if (it->second == g) { live.erase(it); return; }
}

inline void ComState::log_poison_audit()
{
    int n = 0;
    std::vector<std::string> lines;
    std::string line;
    for (const auto& d : ifaces) {
        for (const auto& p : d.poisoned) {
            ++n;
            std::string one = d.name + "::" + p.first + " ";
            if (line.size() + one.size() > 150) { lines.push_back(line); line.clear(); }
            line += one;
        }
    }
    if (!n) { log("X86 COM every registered method has a marshaller"); return; }
    if (!line.empty()) lines.push_back(line);
    log("X86 " + std::to_string(n) + " COM methods have no hand-written marshaller "
        "and return E_FAIL if called:");
    for (auto& l : lines) log("X86   " + l);
}

}   // namespace ghx86

// Registers one COM method generically. The method's own C++ signature drives
// the whole thing -- stack size, argument widening, interface unwrapping, the
// trailing out-parameter -- so a generated line here cannot disagree with the
// header it came from.
#define GH_X86_COM_ADD(ST, ID, IFACE, METHOD)                                       \
    do {                                                                            \
        using PMF_ = decltype(&IFACE::METHOD);                                       \
        ghx86::HostFn hf_;                                                           \
        hf_.dll  = (ST).intern(#IFACE);                                              \
        hf_.name = (ST).intern(#IFACE "::" #METHOD);                                 \
        /* +4 for `this`: on x86 a COM method takes it as the first stack slot. */   \
        hf_.stack_bytes = 4 + ghx86::com_bytes_of((PMF_)nullptr);                    \
        hf_.invoke = [](ghx86::Machine& m, const uint32_t* a) -> uint64_t {          \
            auto* self_ = (IFACE*)ghx86::com().unwrap(a[0]);                         \
            if (!self_) return 0x80004003u;                 /* E_POINTER */          \
            return ghx86::detail::com_invoke(&IFACE::METHOD, self_, m, a + 1);       \
        };                                                                           \
        (ST).add_method((ID), #METHOD, hf_,                                          \
                        ghx86::com_hand_marshal_reason((PMF_)nullptr));              \
    } while (0)

// A hand-written override, by name, clearing the poison. POPBYTES must include
// the 4 bytes of `this`; a[0] is the guest object, a[1...] the arguments.
#define GH_X86_COM_OVERRIDE(ST, ID, METHOD, POPBYTES, ...)                          \
    do {                                                                            \
        if (!(ST).override_method((ID), METHOD, (POPBYTES), __VA_ARGS__))            \
            log("X86 COM override names no such method: " METHOD);                   \
    } while (0)

// The first three vtable slots of every interface. Emitted as one line by the
// generator so that the ambiguity of `&IFace::QueryInterface` never arises and
// so that Release's bookkeeping cannot be forgotten for a new interface.
#define GH_X86_COM_ADD_IUNKNOWN(ST, ID, IFACE)                                      \
    do {                                                                            \
        ghx86::HostFn qi_, ar_, rl_;                                                 \
        qi_.dll = ar_.dll = rl_.dll = (ST).intern(#IFACE);                           \
        qi_.name = (ST).intern(#IFACE "::QueryInterface"); qi_.stack_bytes = 12;     \
        ar_.name = (ST).intern(#IFACE "::AddRef");         ar_.stack_bytes = 4;      \
        rl_.name = (ST).intern(#IFACE "::Release");        rl_.stack_bytes = 4;      \
        qi_.invoke = &ghx86::com_qi;                                                 \
        ar_.invoke = &ghx86::com_addref;                                             \
        rl_.invoke = &ghx86::com_release;                                            \
        (ST).add_method((ID), "QueryInterface", qi_, nullptr);                       \
        (ST).add_method((ID), "AddRef",         ar_, nullptr);                       \
        (ST).add_method((ID), "Release",        rl_, nullptr);                       \
    } while (0)
