// gh_x86_native_gates.h -- the hand-assembled code at the edges of native x86.
//
// The native runtime (gh_x86_native.h) runs guest code on the CPU in
// compatibility mode (CS 0x23), the way WoW64 does, inside our own 64-bit
// process. Everything here is the machine code that crosses that boundary.
// It has no Windows dependency on purpose, so it can be assembled and
// disassembled on the build host and every byte checked before it ever runs
// (toolchain/x86native/gates_dump.cpp).
//
// What N0 established (GREENHOUSE.md §58), and what each piece below exists for:
//
//  - In compat mode GS is never reloaded and its base is low32(TEB64), so each
//    thread's TEB32 lives at low32(TEB64), and a guest fs: access is turned into
//    a gs: access by flipping its prefix byte once (the VEH does that). The same
//    alias gives 32-bit code a per-thread scratch block with no registers spent:
//    the Ctl block, at TEB32+0x1000, is gs:[0x1000+x].
//  - Exceptions raised in compat mode are dispatched on the GUEST stack, so the
//    TEB64 stack bounds (gs:[8], gs:[0x10] in 64-bit mode) must describe the
//    guest stack while the guest runs, or any 64-bit handler that probes its
//    stack walks off the end. Every gate swaps them.
//  - NtContinue always resumes in 64-bit mode, so a VEH that wants the guest
//    back sends it through `resume64`, which far-returns into 0x23.
//
// Layout of the gates page (all of it below 4 GB, RWX):
//   enter64   host -> guest. rcx = Ctl*. Saves the Win64 callee-saved state,
//             links a host frame, installs the guest stack bounds and resumes
//             the guest at Ctl.eip with Ctl's registers.
//   exit32[d] 32-bit return sentinel for host->guest call number d on this
//             thread (d = Ctl.depth at the time of the call). Stores eax/edx and
//             d, then goes to exit64.
//   exit64    unwinds to the host frame for call d and returns from enter64.
//             If the guest abandoned inner calls (an SEH handler that jumped to
//             its __except block instead of returning), their host frames are
//             dropped here rather than resumed by mistake.
//   common32  where every import thunk slot goes: saves the guest registers to
//             Ctl, then far-jumps to hc64.
//   hc64      switches to the host stack and calls host_dispatch(Ctl*).
//             0 = resume the guest from Ctl, otherwise leave through exit64
//             (Ctl.ret_depth says how far).
//   fault64   the same for host_fault(Ctl*), entered from the VEH.
//   resume64  far-returns into compat mode at Ctl.resume_eip; registers and
//             flags untouched.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ghx86 {

// Per-thread control block at TEB32+0x1000 (gs:[0x1000] in compat mode, and
// low32(TEB64)+0x1000 in 64-bit mode). Field offsets are part of the gate code.
struct NativeCtl {
    uint64_t host_rsp;      // 0x00  innermost enter64 frame
    uint64_t eip;           // 0x08  where the guest resumes (zero-extended)
    uint32_t eax, ecx, edx, ebx;      // 0x10
    uint32_t esp, ebp, esi, edi;      // 0x20
    uint64_t eflags;        // 0x30  popfq'd as a qword, so the top half stays 0
    uint64_t host_base, host_limit;   // 0x38  TEB64 stack bounds while in the host
    uint64_t guest_base, guest_limit; // 0x48  ...and while in the guest
    uint32_t idx;           // 0x58  thunk slot index of the current import call
    uint32_t depth;         // 0x5C  live enter64 frames on this thread
    uint32_t ret_depth;     // 0x60  exit64 unwinds until depth == ret_depth + 1
    uint32_t pad0;          // 0x64
    uint64_t resume_eip;    // 0x68  resume64's target
    uint64_t thread;        // 0x70  NativeThread*
    // Written by the VEH for host_fault; never touched by gate code.
    uint32_t f_code, f_eip, f_addr, f_rw;   // 0x78
    uint32_t f_count;
};
static_assert(offsetof(NativeCtl, eip) == 0x08, "gate layout");
static_assert(offsetof(NativeCtl, eax) == 0x10, "gate layout");
static_assert(offsetof(NativeCtl, edi) == 0x2C, "gate layout");
static_assert(offsetof(NativeCtl, eflags) == 0x30, "gate layout");
static_assert(offsetof(NativeCtl, guest_limit) == 0x50, "gate layout");
static_assert(offsetof(NativeCtl, idx) == 0x58, "gate layout");
static_assert(offsetof(NativeCtl, ret_depth) == 0x60, "gate layout");
static_assert(offsetof(NativeCtl, resume_eip) == 0x68, "gate layout");
static_assert(offsetof(NativeCtl, thread) == 0x70, "gate layout");

constexpr uint32_t kCtlOffset    = 0x1000;   // from TEB32
constexpr uint32_t kExit32Stride = 32;
constexpr uint32_t kMaxNativeDepth = 64;      // nested host->guest calls per thread
constexpr uint32_t kNativeSlotBytes = 16;     // == Machine::kThunkStride
constexpr uint32_t kGatesBytes   = 0x1000;

struct NativeGates {
    uint32_t enter64 = 0, exit64 = 0, hc64 = 0, fault64 = 0, resume64 = 0;
    uint32_t common32 = 0, exit32 = 0, resume_tail = 0;
    uint32_t end = 0;
    uint32_t exit32_for(uint32_t depth) const { return exit32 + depth * kExit32Stride; }
};

namespace gates_detail {

struct Asm {
    std::vector<uint8_t> b;
    uint32_t base;
    explicit Asm(uint32_t at) : base(at) {}
    uint32_t here() const { return base + (uint32_t)b.size(); }
    void op(std::initializer_list<uint8_t> x) { b.insert(b.end(), x.begin(), x.end()); }
    void u8(uint8_t v)   { b.push_back(v); }
    void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8((uint8_t)(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) u8((uint8_t)(v >> (8 * i))); }
    void align(uint32_t a) { while (here() % a) u8(0xCC); }

    // rel32 to an address known now
    void rel32_to(uint32_t target) { u32(target - (here() + 4)); }
    // forward references: returns the patch position
    size_t rel32_fwd() { size_t at = b.size(); u32(0); return at; }
    void patch32(size_t at, uint32_t target)
    {
        uint32_t v = target - (base + (uint32_t)at + 4);
        std::memcpy(&b[at], &v, 4);
    }
    size_t rel8_fwd() { size_t at = b.size(); u8(0); return at; }
    void patch8(size_t at, uint32_t target)
    {
        b[at] = (uint8_t)(int8_t)(int32_t)(target - (base + (uint32_t)at + 1));
    }

    // -- 64-bit ------------------------------------------------------------
    // r11 = low32(TEB64) + 0x1000. lea, not add: resume64 must not touch flags.
    void ctl_r11()
    {
        op({0x65, 0x4C, 0x8B, 0x1C, 0x25}); u32(0x30);   // mov r11, gs:[0x30]
        op({0x45, 0x89, 0xDB});                          // mov r11d, r11d
        op({0x4D, 0x8D, 0x9B}); u32(kCtlOffset);         // lea r11, [r11+0x1000]
    }
    void ctl_rcx()
    {
        op({0x65, 0x48, 0x8B, 0x0C, 0x25}); u32(0x30);   // mov rcx, gs:[0x30]
        op({0x89, 0xC9});                                // mov ecx, ecx
        op({0x48, 0x8D, 0x89}); u32(kCtlOffset);         // lea rcx, [rcx+0x1000]
    }
    void rax_from_gs(uint32_t off) { op({0x65, 0x48, 0x8B, 0x04, 0x25}); u32(off); }
    void gs_from_rax(uint32_t off) { op({0x65, 0x48, 0x89, 0x04, 0x25}); u32(off); }
    void rax_from_r11(uint8_t d)   { op({0x49, 0x8B, 0x43, d}); }
    void r11_from_rax(uint8_t d)   { op({0x49, 0x89, 0x43, d}); }
    void rax_from_rcx(uint8_t d)   { op({0x48, 0x8B, 0x41, d}); }
    void rcx_from_rax(uint8_t d)   { op({0x48, 0x89, 0x41, d}); }

    // TEB64 bounds <- Ctl (r11 or rcx based)
    void bounds_from_r11(uint8_t base_off)
    {
        rax_from_r11(base_off);     gs_from_rax(0x08);
        rax_from_r11(base_off + 8); gs_from_rax(0x10);
    }
    void bounds_from_rcx(uint8_t base_off)
    {
        rax_from_rcx(base_off);     gs_from_rax(0x08);
        rax_from_rcx(base_off + 8); gs_from_rax(0x10);
    }

    // -- 32-bit (compat mode), Ctl reached through gs ------------------------
    void st32_eax(uint32_t f) { op({0x65, 0xA3}); u32(kCtlOffset + f); }
    void st32(uint8_t modrm_reg, uint32_t f)        // mov gs:[ctl+f], r32
    {
        op({0x65, 0x89, (uint8_t)(0x05 | (modrm_reg << 3))}); u32(kCtlOffset + f);
    }
    void st32_imm(uint32_t f, uint32_t v) { op({0x65, 0xC7, 0x05}); u32(kCtlOffset + f); u32(v); }
    // push 0x33; push target; retf -- the 32->64 transition N0 proved
    void far_to64(uint32_t target) { op({0x6A, 0x33, 0x68}); u32(target); u8(0xCB); }
};

enum : uint8_t { R_EAX = 0, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };

// host stack -> guest, rcx = Ctl*
inline void emit_resume_tail(Asm& a)
{
    // Reload SS from the GDT before going back to compat mode. A host call that
    // blocks gets context-switched, and on AMD the kernel's SYSRET can come back
    // with SS's selector right but its hidden attributes "unusable" (Linux:
    // X86_BUG_SYSRET_SS_ATTRS). 64-bit code never looks; retfq to the same
    // privilege does not reload SS, so the guest's first [ebp-x] after
    // WaitForMultipleObjects took an AV with no real address (s839/s840).
    // WoW64 does this itself; nothing does it for us.
    a.op({0xB8, 0x2B, 0x00, 0x00, 0x00});   // mov eax, 0x2B
    a.op({0x8E, 0xD0});                     // mov ss, eax
    a.bounds_from_rcx(0x48);
    a.op({0xFF, 0x71, 0x30});           // push qword [rcx+eflags]
    a.u8(0x9D);                         // popfq
    a.op({0x8B, 0x41, 0x10});           // mov eax, [rcx+eax]
    a.op({0x8B, 0x51, 0x18});           // mov edx, [rcx+edx]
    a.op({0x8B, 0x59, 0x1C});           // mov ebx, [rcx+ebx]
    a.op({0x8B, 0x69, 0x24});           // mov ebp, [rcx+ebp]
    a.op({0x8B, 0x71, 0x28});           // mov esi, [rcx+esi]
    a.op({0x8B, 0x79, 0x2C});           // mov edi, [rcx+edi]
    a.op({0x8B, 0x61, 0x20});           // mov esp, [rcx+esp]   (zero-extends rsp)
    a.op({0x6A, 0x23});                 // push 0x23
    a.op({0xFF, 0x71, 0x08});           // push qword [rcx+eip]
    a.op({0x8B, 0x49, 0x14});           // mov ecx, [rcx+ecx]
    a.op({0x48, 0xCB});                 // retfq
}

// Shared by hc64 and fault64: on the guest stack in 64-bit mode, call fn(Ctl*)
// on the host stack, then resume or leave.
inline void emit_host_call(Asm& a, uint64_t fn, uint32_t resume_tail, uint32_t exit64_body)
{
    a.ctl_r11();
    a.op({0x49, 0x8B, 0x63, 0x00});     // mov rsp, [r11+host_rsp]
    a.bounds_from_r11(0x38);
    a.u8(0xFC);                         // cld
    a.op({0x48, 0x83, 0xEC, 0x20});     // sub rsp, 0x20 (shadow space)
    a.op({0x4C, 0x89, 0xD9});           // mov rcx, r11
    a.op({0x48, 0xB8}); a.u64(fn);      // mov rax, fn
    a.op({0xFF, 0xD0});                 // call rax
    // Take the host bounds back from the TEB before anything restores them.
    // The kernel lowers StackLimit as the host stack grows through its guard
    // page, and enter64 only sampled it once: putting that stale limit back on
    // the next hc64 left every frame below it "out of bounds", so RtlDispatch-
    // Exception found no handler -- not even OutputDebugStringA's own __except
    // -- and the process died with nothing logged (HL2 s846-s850: the driver's
    // first deep shader compile grew the stack, a later one printed).
    a.op({0x49, 0x89, 0xC2});           // mov r10, rax
    a.ctl_r11();
    a.rax_from_gs(0x08); a.r11_from_rax(0x38);
    a.rax_from_gs(0x10); a.r11_from_rax(0x40);
    a.op({0x4C, 0x89, 0xD0});           // mov rax, r10
    a.op({0x85, 0xC0});                 // test eax, eax
    a.op({0x0F, 0x85});                 // jnz leave
    size_t leave = a.rel32_fwd();
    a.ctl_rcx();
    a.u8(0xE9); a.rel32_to(resume_tail);
    a.patch32(leave, a.here());
    a.ctl_r11();
    a.u8(0xE9); a.rel32_to(exit64_body);
}

}   // namespace gates_detail

// Assembles the gates page into `out` (kGatesBytes, to be copied to `base`).
inline bool EmitNativeGates(uint32_t base, uint64_t host_dispatch, uint64_t host_fault,
                            std::vector<uint8_t>& out, NativeGates& g)
{
    using namespace gates_detail;
    Asm a(base);

    // resume_tail first: everything else jumps to it
    g.resume_tail = a.here();
    emit_resume_tail(a);

    // -- enter64 -----------------------------------------------------------
    a.align(16);
    g.enter64 = a.here();
    a.op({0x53, 0x55, 0x57, 0x56});                       // push rbx, rbp, rdi, rsi
    a.op({0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});  // push r12..r15
    a.op({0x48, 0x83, 0xEC, 0x28});                       // sub rsp, 0x28
    a.op({0xF3, 0x0F, 0x7F, 0x34, 0x24});                 // movdqu [rsp], xmm6
    a.op({0xF3, 0x0F, 0x7F, 0x7C, 0x24, 0x10});           // movdqu [rsp+0x10], xmm7
    a.op({0xFF, 0x71, 0x00});                             // push qword [rcx+host_rsp]
    a.op({0x48, 0x83, 0xEC, 0x08});                       // sub rsp, 8  (host_rsp 16-aligned)
    a.op({0x48, 0x89, 0x61, 0x00});                       // mov [rcx+host_rsp], rsp
    a.op({0xFF, 0x41, 0x5C});                             // inc dword [rcx+depth]
    a.rax_from_gs(0x08); a.rcx_from_rax(0x38);            // host bounds -> Ctl
    a.rax_from_gs(0x10); a.rcx_from_rax(0x40);
    a.u8(0xE9); a.rel32_to(g.resume_tail);

    // -- exit64 ------------------------------------------------------------
    a.align(16);
    g.exit64 = a.here();
    a.ctl_r11();
    uint32_t exit64_body = a.here();
    a.op({0x49, 0x8B, 0x63, 0x00});                       // mov rsp, [r11+host_rsp]
    uint32_t loop = a.here();
    a.op({0x41, 0x8B, 0x43, 0x5C});                       // mov eax, [r11+depth]
    a.op({0xFF, 0xC8});                                   // dec eax
    a.op({0x41, 0x3B, 0x43, 0x60});                       // cmp eax, [r11+ret_depth]
    a.u8(0x76); size_t done = a.rel8_fwd();               // jbe done
    a.op({0x48, 0x8B, 0x64, 0x24, 0x08});                 // mov rsp, [rsp+8]  (abandoned frame)
    a.op({0x41, 0xFF, 0x4B, 0x5C});                       // dec dword [r11+depth]
    a.u8(0xEB); size_t back = a.rel8_fwd(); a.patch8(back, loop);
    a.patch8(done, a.here());
    a.bounds_from_r11(0x38);
    a.op({0x48, 0x8B, 0x44, 0x24, 0x08});                 // mov rax, [rsp+8]
    a.r11_from_rax(0x00);                                 // host_rsp = outer frame
    a.op({0x41, 0xFF, 0x4B, 0x5C});                       // dec dword [r11+depth]
    a.op({0x48, 0x83, 0xC4, 0x10});                       // add rsp, 0x10
    a.op({0xF3, 0x0F, 0x6F, 0x34, 0x24});                 // movdqu xmm6, [rsp]
    a.op({0xF3, 0x0F, 0x6F, 0x7C, 0x24, 0x10});           // movdqu xmm7, [rsp+0x10]
    a.op({0x48, 0x83, 0xC4, 0x28});                       // add rsp, 0x28
    a.op({0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C});  // pop r15..r12
    a.op({0x5E, 0x5F, 0x5D, 0x5B});                       // pop rsi, rdi, rbp, rbx
    a.u8(0xFC);                                           // cld
    a.u8(0xC3);                                           // ret

    // -- hc64 / fault64 ----------------------------------------------------
    a.align(16);
    g.hc64 = a.here();
    emit_host_call(a, host_dispatch, g.resume_tail, exit64_body);
    a.align(16);
    g.fault64 = a.here();
    emit_host_call(a, host_fault, g.resume_tail, exit64_body);

    // -- resume64 ----------------------------------------------------------
    a.align(16);
    g.resume64 = a.here();
    a.ctl_r11();
    a.op({0x6A, 0x23});                                   // push 0x23
    a.op({0x41, 0xFF, 0x73, 0x68});                       // push qword [r11+resume_eip]
    a.op({0x48, 0xCB});                                   // retfq

    // -- common32 (compat mode) ----------------------------------------------
    a.align(16);
    g.common32 = a.here();
    a.st32_eax(0x10);
    a.st32(R_ECX, 0x14); a.st32(R_EDX, 0x18); a.st32(R_EBX, 0x1C);
    a.st32(R_EBP, 0x24); a.st32(R_ESI, 0x28); a.st32(R_EDI, 0x2C);
    a.u8(0x9C);                                           // pushfd
    a.op({0x65, 0x8F, 0x05}); a.u32(kCtlOffset + 0x30);   // pop dword gs:[ctl+eflags]
    a.st32(R_ESP, 0x20);                                  // esp -> [ret][args...]
    a.far_to64(g.hc64);

    // -- exit32[d] (compat mode) -------------------------------------------
    a.align(kExit32Stride);
    g.exit32 = a.here();
    for (uint32_t d = 0; d < kMaxNativeDepth; ++d) {
        uint32_t start = a.here();
        a.st32_eax(0x10);
        a.st32(R_EDX, 0x18);
        a.st32_imm(0x60, d);
        a.far_to64(g.exit64);
        while (a.here() - start < kExit32Stride) a.u8(0xCC);
    }
    g.end = a.here();
    if (a.b.size() > kGatesBytes) return false;
    a.b.resize(kGatesBytes, 0xCC);
    out.swap(a.b);
    return true;
}

// One import thunk slot (kNativeSlotBytes): record which import, go to common32.
//   mov dword gs:[ctl+idx], i ; jmp common32
inline void EmitNativeSlot(uint8_t* at, uint32_t slot_addr, uint32_t idx, uint32_t common32)
{
    uint8_t s[kNativeSlotBytes] = {0x65, 0xC7, 0x05};
    uint32_t disp = kCtlOffset + 0x58;
    std::memcpy(s + 3, &disp, 4);
    std::memcpy(s + 7, &idx, 4);
    s[11] = 0xE9;
    uint32_t rel = common32 - (slot_addr + 16);
    std::memcpy(s + 12, &rel, 4);
    std::memcpy(at, s, sizeof s);
}

}   // namespace ghx86
