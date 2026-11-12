# The 32-bit x86 guest track

What this covers: how Greenhouse runs an unmodified 32-bit (PE32, i386) Windows game inside its
own 64-bit process on an Xbox in Dev Mode: the execution engines, the PE32 loader, the 32-bit API
surface ("beds"), windowing, COM, DirectDraw/DirectSound/DirectInput, the 32-bit CRT corpus, the
Steam stand-in, the tests, and notes for the three titles that run on it.

Where the code is: `src/loader/x86/` (about 26,000 lines of headers), `src/loader/x86/jit/`,
the entry point `MapAndRun32` in `src/loader/guest_loader.cpp`, `corpus/x86sys/`, `tests/x86/`,
and the tools named in each section. Direct3D 9 has its own page: [x86-d3d9.md](x86-d3d9.md).

Related pages: [overview.md](overview.md), [pe-loading.md](pe-loading.md),
[api-shims.md](api-shims.md), [window-input.md](window-input.md),
[audio-media.md](audio-media.md), [graphics.md](graphics.md),
[../reference/knobs.md](../reference/knobs.md),
[../reference/diagnostics.md](../reference/diagnostics.md),
[../reference/known-limits.md](../reference/known-limits.md).

---

## 1. Why this track exists, and what ships

The console is x64 only. There is no WOW64, no 32-bit system DLL set, and an app package cannot
be built for x86. A 32-bit game therefore cannot be handed to the OS the way a 64-bit guest is
(see [pe-loading.md](pe-loading.md)). Everything a 32-bit guest touches is ours: the mapper, every
imported function, every COM vtable, the CRT.

Three execution engines exist in the tree. They share the loader, the bed table and the COM
layer, and differ only in who executes guest instructions.

| Engine | Code | How it runs guest code | Status |
|---|---|---|---|
| Interpreter | `gh_x86_cpu.h`, `gh_x86_sse.h`, `gh_x86_x87.h`, driver in `gh_x86_win.h` / `gh_x86_run.h` | Decodes and executes one instruction at a time; guest threads are cooperative green threads on one host thread | Built in. The code default. Roughly 7-9 million instructions per second: enough for test guests, too slow for a game |
| BoxedWine JIT | `x86/jit/*`, `toolchain/build_bwjit.sh` | Recompiles guest basic blocks to x64 with a patched BoxedWine binary translator | Optional. Compiled only with `GH_X86_JIT`; not linked in normal builds |
| Native (compatibility mode) | `gh_x86_native.h`, `gh_x86_native_gates.h` | The CPU executes the guest's own x86 code in 32-bit compatibility mode, on real host threads | Built in. What every shipped 32-bit title uses |

History in one paragraph: the track started as a portable interpreter so the CPU core could be
tested on a development machine against the host CPU. It was correct but too slow, so a JIT
(BoxedWine's, GPL-2.0, fetched at build time and never vendored) was connected behind a small
seam. It measured well but carries known x87 code-generation defects and a licence that keeps it
out of default builds. The third approach removed the translator entirely: an x64 CPU can run
32-bit code segments, so the guest's code is run as is, and only the boundary between 32-bit guest
code and 64-bit host code is ours. That is the native runtime, and it is the one games run on.

### How the engine is selected (verified in code)

| Selector | Where | Effect |
|---|---|---|
| `GH_X86_GUEST` compile define | `toolchain/iterate-launcher.sh` (on by default; `GH_X86_GUEST=0` opts out) | Compiles the whole 32-bit path. Without it a PE32 fails with `why=not PE32+ (machine=0x14C ...) -- x64 guests only` |
| `gh_x86_native` knob (`GH_X86_NATIVE`), default 0 | `guest_loader.cpp: MapAndRun32` | 1 = `ghx86::native::RunPe32Native`; 0 = `ghx86::RunPe32Guest` (interpreter) |
| `jit` line in LocalState `ghx86opts.txt` | `MapAndRun32` | Non-native path only, and only in a `GH_X86_JIT` build: the JIT becomes the sole executor |
| `GH_X86_JIT` compile define | build flags | Links the JIT objects built by `toolchain/build_bwjit.sh` |

The knob default is the interpreter, but all three shipped 32-bit title configs
(`corpus/titles/hl2.ghtitle.json`, `hl.ghtitle.json`, `portal.ghtitle.json`) set
`"gh_x86_native": 1`. A PE32 launched without a title config runs on the interpreter unless the
knob is set by hand.

A healthy launch logs `STAGE=guest_is_pe32`, then `X86 runtime: native (compat mode)` or
`X86 runtime: interpreter`. It ends with `STAGE=x86_guest_done PASS result=N` or
`STAGE=x86_guest_failed <why>`; a `why` beginning with `FAULT ` is a crash.

---

## 2. The entry point: `MapAndRun32`

`src/loader/guest_loader.cpp: MapAndRun32` is called when the guest image is PE32. It:

1. Points `ghx86::g_log` at the loader's diag log.
2. Points `ghx86::g_reg` (a table of registry functions) at the loader's virtual registry hive
   (`Bed_Reg*`), so the 32-bit guest sees the same registry as a 64-bit one
   ([filesystem-registry.md](filesystem-registry.md)).
3. Sets `ghx86::g_x86_sys_dir` to `<install>\guest\x86\`, the directory holding our own 32-bit
   system DLLs (section 9).
4. Reads the knobs below and `ghx86opts.txt`.
5. Calls the selected runner and reports the stage line.

Knobs read here (file form `gh_<name>.txt` in LocalState, env form upper-case, or the `knobs`
object of a title config; see [../reference/knobs.md](../reference/knobs.md)):

| Knob | Default | Meaning |
|---|---|---|
| `gh_x86_native` | 0 | Run on the native runtime |
| `gh_x86_steam` | 0 | Answer `steam_api.dll` from our beds (section 10). Ignored for user-uploaded games |
| `gh_x86_seh_trace` | 0 | Log every frame of the guest's SEH chain as it is walked |
| `gh_x86_sample` | 0 | Seconds between "where is each guest thread" samples (`X86N sample` lines) |
| `gh_x86_host_stack_mb` | 32 | Host stack reserved per guest thread on the native runtime |

`ghx86opts.txt` (LocalState, one option per line, UTF-16LE or ASCII) holds engine diagnostics,
most of them interpreter-side:

| Option | Meaning |
|---|---|
| `quiet` | Turn off the per-call audit and trace ring (for timing runs) |
| `stall=<s>` | Stall watchdog period, default 15; 0 disarms |
| `resume=<n>`, `synctrace`, `btime` | Host-call re-logging and timing checkpoints |
| `jit`, `jitemu[=lo-hi]` | Select the JIT; interpret a range of instruction kinds instead of compiling them (bisection) |
| `eips=`, `eipfrom=`, `eipto=`, `eipmax=`, `eipfpu` | EIP trace window (needs `GH_X86_EIPTRACE`) |
| `spin=<n>` | Spin-wait yield threshold (needs `GH_X86_SPIN_YIELD`) |
| `wprof`, `ptid=<n>[+n]` | Windowed host-call profile and the thread(s) it counts |
| `watch=<lo>-<hi>[,...]` | Memory write watchpoints (hex) |

---

## 3. The native runtime

### 3.1 The idea

An x64 CPU in long mode executes 32-bit code whenever the code segment selector is a 32-bit one.
On Windows that selector is `0x23`; 64-bit code is `0x33`. A far transfer between the two is an
ordinary user-mode instruction. So the guest image is mapped below 4 GB, and guest code runs on
the real CPU with real threads. Nothing is translated.

What is not free is everything around it:

- The OS believes the process is 64-bit. It gives no 32-bit TEB, no 32-bit system DLLs, and no
  exception dispatch for 32-bit frames.
- Every import must land in our code, switch to 64-bit mode, run a host function, and return.
- Faults raised while the CPU is in 32-bit mode must be turned into the guest's own SEH.

Alternatives that were tried or ruled out: an x86 app package (the console accepts x64 only);
WOW64 (decided by the kernel at process creation, not available); making `FS` point at a 32-bit
TEB (the FS base is zero in a non-WOW64 process, is reset on every context switch, and the
instructions that would set it from user mode are disabled).

### 3.2 `fs:` without an FS base: the GS alias

32-bit Windows code reaches its TEB through `fs:` (`fs:[0]` is the SEH chain head, `fs:[0x18]`
the TEB self pointer, `fs:[0x2C]` the TLS array). We cannot set the FS base. In compatibility
mode, however, the `gs:` base is the low 32 bits of the 64-bit TEB address, and that is a value we
can read.

So for each guest thread:

1. `bind_teb32` places the thread's 32-bit TEB at address `low32(TEB64)`. The pages are committed
   per 64 KB granule (`teb32_pages`), because the TEB32 can straddle two reservations.
   `prereserve_teb_band` reserves the free granules near the image early so the address is
   available when a thread starts.
2. Guest code still says `fs:`. The first execution of each such instruction faults. The handler
   (`patch_fs`, via `write_code_byte`) rewrites that instruction's segment prefix from `0x64`
   (FS) to `0x65` (GS) in place and resumes. Each site is patched once; the cost is a few
   microseconds per distinct site.

The guest image is mapped `PAGE_EXECUTE_READWRITE` on this runtime
(`m.commit_protect`) so the patch can be written. The exit log reports the count as `fs patches`.

### 3.3 The control block and the gates

Each guest thread has a `NativeCtl` at `TEB32 + 0x1000` (`kCtlOffset`), declared in
`gh_x86_native_gates.h`. It holds the saved host stack pointer, the guest register image, host and
guest stack bounds, the index of the import being called, nesting depth, a resume address, and the
last fault's code and address.

`EmitNativeGates` writes one 4 KB page of machine code below 4 GB. Its entry points:

| Gate | Direction | Purpose |
|---|---|---|
| `enter64` | host -> guest | Save host state into the `NativeCtl`, load guest registers, far-return to selector `0x23` |
| `exit32[d]` | guest -> host | 64 return sentinels, one per nesting depth (`kMaxNativeDepth`). A guest function called from the host returns here |
| `exit64` | (64-bit half) | Restores host state; drops frames the guest abandoned (longjmp, unwinding) |
| `common32` | guest -> host | Target of every import slot: saves guest registers and far-jumps to `0x33` |
| `hc64` | (64-bit half) | Calls `host_dispatch(NativeCtl*)` on the host stack, then returns to the guest |
| `fault64` | fault path | Where the vectored handler redirects a guest fault; calls `host_fault` |
| `resume64` | fault path | Far-returns to `0x23` at `NativeCtl.resume_eip` |

Each imported function gets a 16-byte slot (`EmitNativeSlot`): store the import's index into the
control block, jump to `common32`. The guest's import address table points at these slots.

`toolchain/x86native/gates_dump.cpp` emits the gate page on a development machine so it can be
disassembled and checked without a console.

### 3.4 Four traps in the mode switch

These cost the most time to find. Each is handled in the gates.

1. **The OS dispatches a compat-mode exception on the guest stack.** The exception dispatcher
   checks the stack against the 64-bit TEB's `StackBase`/`StackLimit`. Every gate therefore swaps
   those two fields to the guest stack's bounds on the way in and back on the way out. After a
   host call, `emit_host_call` copies the live values back into the control block, because the
   host may have grown its stack. A stale limit made the dispatcher find no handler at all: the
   process died silently, and the tell was a stack capture with zero frames.
2. **Continuing from an exception always resumes in 64-bit mode.** The vectored handler
   (`NativeVeh`) cannot resume the guest directly. It sets `Rip` to `resume64` or `fault64` and
   lets the gate make the far transfer.
3. **The loader's other exception handlers must ignore guest faults.** Handlers written for
   64-bit guests stand aside when `SegCs == 0x23`.
4. **`SS` must be reloaded on the way back in.** On AMD CPUs a system-call return after a context
   switch can leave the stack segment's cached attributes unusable for 32-bit code, and a far
   return does not reload them. `emit_resume_tail` loads `SS` with `0x2B` explicitly. The symptom
   without it was an access violation with `info0=3 info1=0`.

### 3.5 Host calls: `host_dispatch`

`gh_x86_native.h: host_dispatch` runs on the host stack in 64-bit mode for every import call.

- It reclaims frames the guest abandoned.
- Beds flagged `lockless` run without the bed lock. Everything else takes the global bed lock
  (`Gil`, recursive), copies the registers into the shared `Machine` CPU image, and calls
  `Machine::dispatch_hostcall`, the same dispatcher the interpreter uses.
- A bed that returns a host pointer above 4 GB is a bug and is reported
  (`returned host pointer above 4GB -- it must be a BED`).
- An import with no implementation ends the guest with `0xC0000139` and names the import.
- Diagnostics are never written while holding the lock; lines are deferred
  (`t_deferred_log`), because a diag write takes tens of milliseconds.
- `FpuGate` gives host code a sane floating-point environment (MXCSR `0x1F80`, x87 control word
  `0x27F`) and restores the guest's on exit. The guest's own values are logged once as
  `X86N guest fpu cw= mxcsr=`.

Host -> guest calls (window procedures, thread entry points, callbacks, COM callbacks) go through
`native_call`, used by `Machine::call_guest32`. It keeps the guest stack 16-byte aligned at the
callee's arguments, releases the bed lock completely while the guest runs, and refuses when there
is no guest context, when unwinding, or at depth 64. A refusal sets `g_x86_call_refused` so an
SEH walk reports "unhandled" instead of pretending the handler ran.

### 3.6 Faults: `host_fault` and the fault-loop guard

A fault in guest code reaches `host_fault`. It builds an x86 `EXCEPTION_RECORD` and `CONTEXT` in
guest memory and runs the guest's vectored handlers and its `fs:[0]` SEH chain through
`Machine::dispatch_guest_exception`. The guest either continues, unwinds to a handler, or does not
handle it.

Log lines, in order of severity:

| Line | Meaning |
|---|---|
| `X86N dispatching guest exception` | First 8 only. A fault is being offered to the guest |
| `X86N guest exception ... -> guest handler` / `unhandled` | First 48. Outcome |
| `X86N unhandled guest exception`, `X86N fault detail: rip= info0= ... code=` and a stack dump | The guest is about to end |
| `X86N fault loop: ... ending the guest instead of spinning` | The same EIP faulted 64 times in a row |

The last line is the fault-loop guard. A guest handler that answers "continue execution" without
fixing anything would otherwise spin forever at full speed, with a frozen screen and no log
growth. Ending the guest with a named address is more useful.

Reading a fault: an access violation "reading 0xFFFFFFFF" with `info0=0` is usually a
general-protection fault, not a bad pointer (for example a non-temporal SSE store to a misaligned
address).

### 3.7 Threads, locks and TLS

- Each guest thread is a real host thread (`CreateThread`), with a host stack of
  `gh_x86_host_stack_mb` megabytes reserved. Host calls, including Direct3D 9 down through the
  graphics driver's shader compiler, run on the host stack, so it is large.
- Guest `CRITICAL_SECTION` (24 bytes on x86), `SRWLOCK` and `CONDITION_VARIABLE` are implemented
  in place inside the guest's own memory as futex words over `WaitOnAddress` (`GuestCs`,
  `GuestSrw`, `cv_sleep`). The host's versions are a different size and cannot be used.
- TLS: 64 slots in the TEB32 plus 1024 expansion slots, and PE implicit TLS (`Machine::tls_*`).
- `RegisterNativeBeds` registers the thread, lock and wait beds *before* `RegisterStdBeds`.
  Registration is first-wins, so the native versions replace the interpreter's green-thread ones.
- One guest runs at a time. A guest "process" is in-process. `end_run` frees everything except
  the TEB32 granules.
- Exit line: `X86N guest exited ... free calls, locked calls, faults, fs patches, threads`.

Signature: `RunPe32Native(file, result, why, image_path, region_bytes = 1 GiB, timeout)`.

---

## 4. The interpreter

`gh_x86_cpu.h` is a portable C++ i386 interpreter with no Windows headers, so it compiles and is
tested on a development machine. Properties worth knowing:

- Guest pointer == host pointer. Guest memory is one region reserved below 4 GB
  (`gh_x86_win.h: low_reserve`, using `VirtualAlloc2` with address requirements). No translation
  table exists in either engine.
- In interpreter mode guest pages are committed read-write and never executable.
- `gh_x86_sse.h` implements MMX/SSE/SSE2, checked bit-exact against the host. `gh_x86_x87.h`
  implements the x87 stack with registers held as doubles (53-bit precision; 80-bit loads and
  stores convert). Both files belong to the interpreter only; the native runtime uses the real
  FPU.
- `gh_x86_cpuid.h` is shared by all engines. It advertises SSE2 and nothing newer (no SSE3,
  AVX or OSXSAVE), so guests do not pick code paths the interpreter lacks. It also defines
  `kGhX87InitCW = 0x027F`, the x87 control word a Windows thread starts with. A thread created
  with a control word of 0 has every x87 exception unmasked, and the first inexact result raises a
  floating-point exception inside the CRT. That happened with the JIT's thread constructor and is
  fixed in `jit/gh_bw_shim.cpp: thread_new`.
- Threads are cooperative green threads on one host thread (`Machine::run_turn`, `reschedule`,
  `wait_current`). A bed that would block re-executes later instead of parking. The guest is told
  there is one processor.
- Interpreter-side diagnostics: stall watchdog (`report_stall`), callee-saved register audit,
  host-call trace ring, write watchpoints, hot-code sampling, heap audit (`heap_audit`).

## 5. The BoxedWine JIT

The seam is `x86/jit/gh_jit_api.h` (namespace `ghjit`): `init`, `thread_new`/`thread_free`, `run`,
`code_changed`, `host_rip_to_guest`, `set_emulate_range`, `stats`. `run` returns one of
`Slice`, `HostCall`, `Halt`, `Fault`; `Machine::run_jit` in `gh_x86_win.h` consumes it.

| File | Role |
|---|---|
| `x86/jit/gh_bw_shim.cpp` | Stands in for BoxedWine's emulated kernel: threads, memory callbacks, panics (logged as `X86 JIT PANIC`) |
| `x86/jit/gh_bw_kernel.h`, `gh_bw_kernel_post.h` | The minimal declarations BoxedWine's CPU sources need |
| `x86/jit/gh_jit_stubs.h` | No-op versions of the API for builds without the JIT |
| `toolchain/build_bwjit.sh` | Fetches BoxedWine at a pinned commit into `toolchain/boxedwine` (git-ignored), applies `toolchain/boxedwine.patch`, builds it with asmjit and Berkeley SoftFloat |

To use it: run `toolchain/build_bwjit.sh`, build the loader with `GH_X86_JIT` defined and the
resulting objects linked, leave `gh_x86_native` at 0, and put `jit` in `ghx86opts.txt`.

Limits: x87 instruction kinds 499-631 (`ghjit::kX87InstFirst`/`kX87InstLast`) are interpreted by
default because of known code-generation defects; `jitemu=lo-hi` widens or moves that range for
bisection. BoxedWine is GPL-2.0, which is why it is fetched and not vendored and why the JIT is
not part of default builds. `toolchain/bwshim/` holds only a static-analysis stub header
directory.

---

## 6. The PE32 loader and the `Machine`

`gh_pe32.h` parses PE32 without Windows headers. Differences from the 64-bit mapper
([pe-loading.md](pe-loading.md)): relocations are `HIGHLOW`, import thunks are 32-bit with the
ordinal flag in bit 31, TLS directory fields are 4-byte virtual addresses, and there is no
`.pdata`; exception handling is the frame-based `fs:[0]` chain.

`gh_x86_win.h: struct Machine` owns one guest. The members a newcomer needs:

| Member | Purpose |
|---|---|
| `reserve`, `alloc` | The guest address region and its bump allocator |
| `load_image` | Map the EXE, apply relocations |
| `setup_thread`, `init_thread_storage` | Stack, TEB32, PEB32 |
| `init_heap`, `heap_alloc`, `heap_free`, `heap_audit` | The guest process heap (64 MB initial segment, grows on demand) |
| `bind_imports`, `make_thunk`, `thunk_for` | Resolve imports to thunk slots (`kThunkStride` = 16 bytes; 8192 dynamic slots for `GetProcAddress`) |
| `load_guest_dll`, `export_addr` | Map a DLL the game ships: exports, forwarders, recursive imports, `DllMain`, thread attach/detach |
| `resolve_dll_path` | Search order: the requesting module's directory and the game directories first, `g_x86_sys_dir` last |
| `resolve_for_module` | A mapped guest DLL's export wins over a bed of the same name |
| `dispatch_hostcall` | Run the host function behind a thunk |
| `call_guest32` | Call guest code from the host |
| `dispatch_guest_exception`, `unwind_seh` | x86 SEH: vectored handlers, the `fs:[0]` chain, unwinding |
| `tls_add_module`, `tls_thread_init`, `tls_callbacks` | Implicit TLS |
| `describe_addr`, `dump_stack_at`, `region_of` | Turn an address into `module+offset` or a nearest export for logs |
| `log_capped` | A log line a guest can repeat in a retry loop is written a bounded number of times |

`ordinal_name` maps import-by-ordinal to names for the DLLs where games do that (`WSOCK32` and
`WS2_32` have different ordinal tables). `gh_load_library` returns a synthetic module handle only
for names we implement, API-set names, and DLLs present in the host's System32; anything else
fails as it would on Windows.

Rule: a DLL in the game's own directory is mapped as real guest code. Only DLLs the game does not
ship are answered by beds or by `guest\x86\`.

---

## 7. The API surface: thunks, beds, forwards, stubs

"Bed" is the project's word for a function we plant under an import
([api-shims.md](api-shims.md)). For a 32-bit guest every import is one, in three grades.

| Grade | Macro / file | When to use |
|---|---|---|
| Hand-written bed | `GH_X86_BED`, `GH_X86_BED_CDECL` in `gh_x86_beds.h` and the per-area headers | Anything that returns a pointer, writes a pointer-sized value through a pointer, takes a struct whose layout differs between x86 and x64, takes a callback, or needs Greenhouse behaviour |
| Dynamic forward | `GH_X86_FWD_DYN` in `gh_x86_beds_gen.h` (about 550 entries) | The host function has the same meaning and only scalar or handle arguments. Resolved with `GetProcAddress` at run time, never linked statically, so a name missing on the console registers nothing |
| Honest-failure stub | `gh_x86_stubs.h`, generated by `toolchain/genstubs.py` (about 225 entries) | Imports that large middleware binds but rarely calls. Each returns the truthful failure for its API and logs when it is actually reached |

### How a call is marshalled (`gh_x86_thunk.h`)

- A stdcall callee pops its own arguments, so the byte count must be exact. For forwards it is
  derived at compile time from the real C signature (`ArgLayout`): a pointer or pointer-sized
  integer is 4 bytes on the guest stack, a 64-bit integer 8, and so on. For beds it is the number
  written in the macro. `GH_X86_BED_CDECL` is for caller-pops functions.
- `ThunkRegistry::add` is first-registration-wins. A later registration of the same name is
  dropped; if a generated forward was shadowed by a hand-written bed the startup log says
  `X86 bed SHADOWED by an earlier hand-written bed: <name>`. This is intended.
- Compile-time audit `ArgLayout::has_ptr_to_ptr_sized` and the startup list
  (`Machine::log_forward_audits`) name forwards whose signature would let the host write 8 bytes
  through a pointer to a 4-byte guest object. Each must be replaced with a bed
  (`RegisterPtrToPtrBeds`).

### Registration order (`RegisterStdBeds`, end of `gh_x86_beds.h`)

Native thread/lock beds (native runtime only) -> the standard hand-written beds ->
`RegisterGdiBeds` -> `RegisterPtrToPtrBeds` -> `RegisterSourceGapBeds` ->
`RegisterSteamBridgeBeds` (only with `gh_x86_steam`) -> `RegisterD3DBeds` -> `RegisterD3D9Beds` ->
`RegisterDsoundBeds` -> `RegisterInputBeds` -> generated forwards (`RegisterGeneratedBeds`) ->
generated stubs (`RegisterStubBeds`). Order matters because the first registration wins.

### Struct sizes that force a bed

| Type | x86 | x64 |
|---|---|---|
| `CRITICAL_SECTION` | 24 | 40 |
| `MSG` | 28 | 48 |
| `MEMORY_BASIC_INFORMATION` | 28 | 48 |
| `SECURITY_ATTRIBUTES` | 12 | 24 |
| `STARTUPINFO`, `SYSTEM_INFO`, `WNDCLASS*` | pointer fields | differ |

File beds must call the loader's virtual-filesystem beds (`Bed_CreateFileW` and friends), not
the raw OS functions, so path redirection applies to 32-bit guests too
([filesystem-registry.md](filesystem-registry.md)).

### Diagnostic signatures of a wrong bed

| Symptom | Cause |
|---|---|
| A register holds ASCII text shortly after an API call | stdcall byte count wrong; the stack is shifted |
| `ebp` is 0 after an API call | The host wrote past a 4-byte or x86-sized guest out-struct |
| Guest dies with `0xC0000139` and a named import | No bed, forward or stub for that import |
| A later, unrelated-looking engine error | Often a pop-count error several calls earlier |

### Regenerating

`python3 toolchain/genstubs.py` rewrites `gh_x86_stubs.h` from the classified list inside the
script. `gh_x86_beds_gen.h` is also marked generated, but its generator is not in the repository
(see section 13); today it is maintained by hand, including a block of Half-Life 2 additions.

---

## 8. Windowing, GDI, input

There are no real windows in the container. The 64-bit side is described in
[window-input.md](window-input.md); the 32-bit side re-implements the same model with x86 struct
layouts.

- **Synthetic HWNDs.** The main window is `kFakeHwnd` (`0x57480001`); further windows are
  `0x5748000N`. Each has its own class and window procedure (`BedState::sub_windows`,
  `x86_register_class`, `x86_create_window` in `gh_x86_beds.h`).
- **Every user32 entry that takes an HWND must be a bed.** The real user32 does not know these
  handles and answers with garbage or `0xFFFFFFFF` (a forwarded `GetPropW` did exactly that and
  the result was used as a pointer). Window properties live in `BedState::win_props`.
  `BeginPaint` returns a synthetic DC.
- **Message pump.** `peek_message32` drains the loader's shared keyboard/mouse queue and
  synthesizes the activation sequence a game waits for. `gh_x86_wndq.h` adds window `WM_TIMER`
  and a `WM_DEVICECHANGE` per controller connect or disconnect. Thread timers (no HWND) never
  fire.
- **Raw input.** Beds use the x86 layouts (`RAWINPUTHEADER` is 16 bytes).
- **Cursor.** `SetCursorPos`, `ShowCursor`, `ClipCursor` are beds; relative mouse mode works.
- **Command line.** `GetCommandLineA/W` return the loader's guest command line (title-config
  `args` included).
- **GDI text.** The container's gdi32 has no font engine. `gh_x86_gdi.h: RegisterGdiBeds` serves
  fonts, DCs and DIB sections from the loader's DirectWrite-backed implementation
  (`src/loader/gh_gdifont.h`). Handles are small synthetic values; DIB pixel memory is allocated
  below 4 GB so the guest can write to it.
- **Controllers.** `gh_x86_input.h` forwards `xinput1_4`, `xinput1_3` and `xinput9_1_0` to the
  loader's XInput beds (the structs are identical in both bitnesses) and supplies an
  `IDirectInput8` that enumerates no devices, through both `DirectInput8Create` and
  `CoCreateInstance`. An SDL2 game needs the DirectInput object to exist or its whole controller
  init fails. The guest environment sets `SDL_JOYSTICK_RAWINPUT=0` so SDL uses XInput.
- **Mouse-driven menus.** GoldSrc's menu takes mouse input only. The title config's
  `"input": "builtin-wasd"` profile maps the right stick to the mouse, A to left click, and the
  left stick to WASD.

Trace: `X86 INPUTHOP peek|dispatch|callwndproc` follows one input event from the queue to the
guest window procedure.

---

## 9. COM, DirectDraw, DirectSound

### 9.1 The x86 COM layer (`gh_x86_com.h`)

A 32-bit guest calls COM methods through a vtable of 32-bit function pointers. The host objects
are 64-bit. For each host interface pointer handed to the guest, the layer creates a 16-byte guest
object:

| Offset | Content |
|---|---|
| +0 | Guest vtable: an array of thunk addresses, one per method |
| +4, +8 | The 64-bit host interface pointer |
| +12 | Tag: a marker and the interface id |

Vtable order is the only thing a guest uses, so it is generated: `toolchain/gen_com_tables.py`
reads the Windows SDK headers (`toolchain/xwin-sdk`, fetched, git-ignored) and writes
`gh_x86_com_gen.h` as X-macro lists, inherited methods first, cross-checked against the C `Vtbl`
structs in the same headers. Methods whose parameters cross the bitness boundary badly are listed
in `_SUSPECT` macros.

Marshalling rules:

- Interface-typed arguments are unwrapped to the host pointer by type.
- A trailing `X**` out-parameter is wrapped into a new guest object. The form
  `X** out, HANDLE* shared` used by Direct3D 9 `Create*` methods is handled by `com_call_out_sh`.
- A method that cannot be marshalled safely by the generic path is **poisoned**: calling it logs
  once and returns `E_FAIL`. The poisoned list is printed at startup.
- A hand-written override (`ComState::override_method`) replaces the generic path and clears the
  poison.

| File | Covers |
|---|---|
| `gh_x86_com_d3d.h` | DXGI and D3D11 over the loader's 64-bit beds; struct overrides (`DXGI_SWAP_CHAIN_DESC` is 60 bytes on x86, 72 on x64); `Map`/`Unmap` bounce buffers |
| `gh_x86_com_d3d9.h` | Direct3D 9; see [x86-d3d9.md](x86-d3d9.md) |
| `gh_x86_dsound.h` | DirectSound |
| `gh_x86_input.h` | The empty `IDirectInput8` |
| `gh_x86_ddraw.h` | DirectDraw (synthetic objects, not wrappers) |

### 9.2 DirectDraw (`gh_x86_ddraw.h`)

The console has no DirectDraw. A software-rendered game (GoldSrc with `-soft`) needs it: it
creates a primary surface with one back buffer and an off-screen system-memory surface, locks the
off-screen surface, draws into it with the CPU, blits to the back buffer and flips.

The implementation is synthetic, not a wrapper:

- `IDirectDraw` and `IDirectDrawSurface` objects are guest heap blocks (`DDSurf32`). Surface
  pixels are ordinary guest memory in RGB565.
- `Blt` and `BltFast` are memory copies. `Lock` returns the pixel block. The x86
  `DDSURFACEDESC` is 108 bytes and is written with x86 offsets.
- `Flip` swaps the primary and back pixel blocks, then presents: the 565 pixels are converted to
  X8R8G8B8 into a system-memory Direct3D 9 surface, copied to the back buffer with
  `UpdateSurface`, and presented, using a Direct3D 9 device the layer keeps for this purpose
  (`com_live_d3d9`).
- A surface is never reported lost.

Not implemented: clippers, palettes, overlays, `GetDC`. Answering `DirectDrawCreate` with failure
is worse than it looks: GoldSrc then switches to its OpenGL renderer, and there is no OpenGL in
the container.

### 9.3 DirectSound (`gh_x86_dsound.h`)

x86 vtables over the loader's DirectSound-on-XAudio2 device (`src/loader/gh_dsound.h`, see
[audio-media.md](audio-media.md)). `DSBUFFERDESC` is translated (36 bytes on x86, 40 on x64);
`Lock` hands out a guest-heap bounce buffer per sound buffer and copies on `Unlock`; capture
returns `DSERR_NODRIVER`. Enabled per title with the `gh_dsound` knob (default 0).

---

## 10. The 32-bit system DLL corpus (`corpus/x86sys`)

A 32-bit game imports a C runtime, and the console has no 32-bit one. Two are built from source
in this repository and staged into the package at `guest\x86\` by `toolchain/iterate-launcher.sh`:

| Directory | Produces | For |
|---|---|---|
| `corpus/x86sys/msvcrt` (`msvcrt.c`, `msvcrt.def`, `build.sh`) | `msvcrt.dll` | Older games and DLLs linked to the system CRT |
| `corpus/x86sys/ucrt` (`ucrt.c`, `fwd.c`, `gen_defs.py`, `apisets.txt`, `build.sh`) | `ucrtbase.dll`, plus `vcruntime140.dll`, `msvcp140.dll` and the `api-ms-win-crt-*` forwarders | Anything built with Visual C++ 2015 or later (the current GoldSrc build) |

These are real PE32 DLLs loaded by `load_guest_dll`. Their own kernel32 imports are beds.

Facts that cost time:

- **`_except_handler4` frame layout.** In `corpus/x86sys/ucrt/ucrt.c: _except_handler4_common`,
  the establisher frame is `ebp - 0x10`, and the slot where the handler stores the
  `EXCEPTION_POINTERS` for a filter expression is at `EstablisherFrame - 4`. An earlier version
  wrote it elsewhere, and `GetExceptionInformation()` in guest `__except` filters read garbage.
- **A game directory shadows the corpus.** Resolution searches the game's directories before
  `guest\x86\`. A stale copy of one of our DLLs left in a game's install directory wins over the
  packaged one. The tell is a log string showing old wording after a rebuild.
- **A game's own CRT wins too.** If the game ships `msvcr*.dll` it is mapped as guest code and
  ours is not used.

---

## 11. Steam for 32-bit guests

A 32-bit `steam_api.dll` looks for a running 32-bit Steam client, which cannot exist on the
console. The loader has a signed-in Steam session, but it belongs to the 64-bit engine inside the
loader, and 32-bit guest code cannot call it. (The 64-bit design is under `docs/steam/`.)

Two modes:

| `gh_x86_steam` | Behaviour |
|---|---|
| 0 (default) | The game's own `steam_api.dll` is mapped as guest code. `SteamAPI_Init` fails as it would on a PC without Steam. Half-Life 2 and Portal run this way |
| 1 | `load_guest_dll` refuses to map `steam_api.dll` (`g_x86_bed_only_steam`), so its imports fall through to `RegisterSteamBridgeBeds` in `gh_x86_beds.h` |

With the knob on:

- Flat exports (`SteamAPI_Init`, `SteamAPI_RunCallbacks`, `SteamInternal_ContextInit`,
  `SteamInternal_FindOrCreateUserInterface`, ...) are cdecl beds (`GH_X86_BED_CDECL`).
- Interface accessors return a guest object whose vtable is ours (`steam_stub_for`). Windows
  Steam interfaces are thiscall: the callee pops the arguments. Each known method therefore has
  an argument-byte count in the table `kSteamSlots`; the default answer is 0 ("no", "none",
  "invalid handle").
- An interface with no known slots is returned as null, not as a stub that cannot pop
  correctly. A call to an unknown slot logs the slot and return address and warns that the guest
  stack is now suspect.
- Trace: `X86 STEAM interface WANTED: <version>`, `X86 STEAM stub <version> ...`,
  `X86 STEAM call <iface> slot N (pops M)`.
- No Steamworks SDK header is used. Vtable slot indices are the same in x86 and x64 builds of one
  interface version.

What this is and is not, stated plainly because the code comments and the log line
(`bridged to the loader's x64 engine`) suggest more than the code does:

- The 32-bit layer is a **stand-in table**, not a call-through. Its answers are constants or small
  local functions; it does not forward calls to the loader's 64-bit Steam session.
- The knob is forced off for user-uploaded games (`!g_customRun` in `MapAndRun32`). It applies
  only to library titles the Launcher installed through the signed-in account, and the title
  config that sets it also requires the `steam-runtime` service.
- GoldSrc asks Steam whether the account is subscribed to the app and, on "no", exits with code 0
  and no message. The symptom of a missing or wrong answer here is a clean, silent exit just
  after file-system setup. The fixed answers for these queries are in `kSteamSlots`; read the
  comment block above the table before changing them.
- GoldSrc single-player is a listen server: the client half asks Steam for a connection token and
  the server half validates it. Both halves are in the same process. The stand-in issues a local,
  fixed-format token (`gh_steam_initiate_game_connection`) that only its own game-server stub
  recognizes (`gh_steam_gs_connect_and_auth`). It is not a Steam ticket and means nothing to a
  real server.
- Multiplayer, friends, achievements, cloud saves and overlay are not provided on the 32-bit
  track.

A wrong argument-byte count does not fault at the call. The guest stack is shifted and the game
fails later with an unrelated-looking engine error; suspect the last `X86 STEAM call` lines.

---

## 12. Tests and offline tools

Everything here runs on a development machine; none of it needs a console.

| Item | What it does |
|---|---|
| `tests/x86/test_cpu.cpp` + `cases.c` (`build_cases.sh`) | Interpreter unit and differential tests: each operation is also run on the host CPU and all arithmetic flags must match. Build line is in the file header (`g++ -O2 -std=c++20 -I src/loader/x86 ...`) |
| `tests/x86/test_pe32.cpp` + `pe32guest.c` (`build_pe32.sh`) | Maps a real PE32 with relocations and runs it |
| `tests/x86/test_coverage.cpp` + `toolchain/x86_opcode_corpus.py` | Extracts every distinct instruction encoding from a game binary with objdump and reports which the interpreter cannot decode, before anything is deployed |
| `tests/x86/m2guest.c`, `m3bguest.c`, `n1guest.c`, `d9guest.c` | CRT-free PE32 guests for milestones: imports and callbacks, threads and waits, the native runtime, the D3D9 COM layer. Each exits with a bitmask of passed checks (511, 1023, 2047, 511 when all pass) |
| `tests/x86/benchguest.c` | Engine benchmark guest |
| `toolchain/x86native/build_host.sh`, `gh_x86_host.cpp` | Builds `x86host.exe`, the native runtime as a plain x64 console program for a Windows desktop (no Launcher beds: no D3D9, GDI or DirectInput) |
| `toolchain/x86native/gates_dump.cpp`, `build_n0.sh`, `src/spikes/x86_n0_probe.cpp` | Gate page dump; the original probe that proved compatibility mode and the GS alias work on the console |
| `toolchain/vmdiff/` (`ghtrace32.c`, `ghinject32.c`, `ghdiff.py`, `run_vm_trace.sh`) | Differential API tracing: record a 32-bit program's API calls and results on real Windows and compare with our log |
| `toolchain/x86sum.py` | Summarizes an x86 guest diag log: engine, milestones, asset throughput, hot code |
| `toolchain/iterate-x86spike.sh` | Deploys a test guest into a separate package slot, leaving the Launcher untouched |

The fastest loop for a new 32-bit title: run `x86_opcode_corpus.py` and `test_coverage` only if
you intend to use the interpreter; otherwise launch on the native runtime and read the diag for
`unimplemented import`, `X86 bed SHADOWED`, poisoned COM methods and stub hits.

---

## 13. Per-title notes

Title configs are `corpus/titles/*.ghtitle.json` (schema `greenhouse.title/1`).

| | Half-Life 2 | Portal | Half-Life (GoldSrc) |
|---|---|---|---|
| Config | `hl2.ghtitle.json` | `portal.ghtitle.json` | `hl.ghtitle.json` |
| Executable | `hl2.exe` | `hl2.exe` | `hl.exe` |
| Arguments | `-game hl2 -condebug -novid` | `-game portal -condebug -novid` | `-game valve -soft -condebug -novid` |
| Knobs | `gh_x86_native`, `gh_dsound` | `gh_x86_native`, `gh_dsound` | `gh_x86_native`, `gh_dsound`, `gh_x86_steam` |
| Input profile | `builtin-wasd` | (default) | `builtin-wasd` |
| Renderer | Direct3D 9 ([x86-d3d9.md](x86-d3d9.md)) | Direct3D 9 | Software renderer over DirectDraw, presented through Direct3D 9 |
| Steam | Game's own DLL; init fails; game continues | Same | Stand-in (section 11) |
| CRT | Ships its own | Ships its own | `corpus/x86sys/ucrt` |
| Observed | Menu, maps, save/load, sound, pad; roughly 20 fps | Runs; portals render since the fixed-function pixel stage was added | Menu, new game, map transitions; roughly 30 fps |

**Half-Life 2 (Source).** The engine's many DLLs are mapped as guest code. `-condebug` makes the
engine write `console.log`, the best second log. Source's material system locks dynamic vertex
buffers many times a frame and writes them with non-temporal SSE stores, which is why lock bounce
buffers preserve the host pointer's alignment. Controllers arrive through SDL2 and XInput.

**Portal.** Same executable and engine. It needed one renderer addition: draws with a vertex
shader and no pixel shader, which Direct3D 9 defines as fixed-function texture stages. See
"Fixed-function pixel stage 0" in [x86-d3d9.md](x86-d3d9.md).

**Half-Life (GoldSrc).** Only the software renderer works; the OpenGL renderer cannot (no OpenGL
in the container), so `-soft` is required and DirectDraw must succeed. The game UI library
imports a Chromium embedding DLL with hundreds of imports; the generated stubs exist for that.
The menu is mouse-driven. Requires the Steam stand-in.

Performance on this track is bounded by single-thread CPU speed and, for Source, by the cost of
crossing the mode gate on every Direct3D call.

---

## 14. Known gaps and inconsistencies in the tree

- `gh_x86_beds_gen.h` says it is generated by a script that is not in the repository. Treat the
  file as hand-maintained until a generator is added.
- The comment in `.gitignore` describing `toolchain/boxedwine.patch` gives a smaller size than
  the patch file has; the patch file is authoritative.
- A comment near `Machine::commit_protect` in `gh_x86_win.h` says guest code is never executed natively. That is
  true of the interpreter only; the native runtime maps guest images read-write-execute.
- The `SteamAPI_Init` log line says "bridged to the loader's x64 engine". No call-through exists
  on the 32-bit side (section 11).
- The knob default (`gh_x86_native` = 0) selects the engine no game can use at speed. Every
  shipped config overrides it.
- Limits by design: one 32-bit guest at a time; child processes are in-process; no OpenGL; no
  Direct3D 8 or earlier; DirectDraw is the minimal contract described above; thread timers do not
  fire; DirectInput reports no devices.

See also [../reference/known-limits.md](../reference/known-limits.md).
