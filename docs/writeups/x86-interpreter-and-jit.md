# 32-bit games, attempts one and two: an interpreter and a JIT

What this covers: the first two ways Greenhouse tried to run 32-bit (PE32, i386) Windows games,
a portable interpreter and then BoxedWine's JIT, what each measured on the console, and why
neither became the shipping engine.
Where the code is: `src/loader/x86/gh_x86_cpu.h` (interpreter), `gh_x86_sse.h`, `gh_x86_x87.h`,
`gh_x86_cpuid.h` (`gh_cpuid`, `gh_rdtsc`, `kGhX87InitCW`), `gh_x86_win.h` (`Machine`, `run_turn`,
`run_jit`, `guest_region_query`), `gh_x86_beds.h` (`fill_sysinfo32`, `kGuestProcessorCount`),
`src/loader/x86/jit/` (`gh_jit_api.h`, `gh_bw_shim.cpp`), `toolchain/build_bwjit.sh`,
`toolchain/boxedwine.patch`, `toolchain/vmdiff/`, `toolchain/x86sum.py`, `tests/x86/`.
Related: [x86-guest.md](../architecture/x86-guest.md) · [x86-native.md](x86-native.md) ·
[testing.md](../contributing/testing.md) · [building.md](../building.md#optional-the-boxedwine-jit-build)
Period: 2026-08-20 to 2026-08-23, loader spike builds 0.0.182.0–0.0.263.0. Follow-up that
superseded both: 2026-11-01, Launcher 0.0.821.0. Notebook sections: §30 (30.1–30.28), §58.

> **Read this first.** Of the four substrates examined in August, three were declared dead.
> One of them, running x86 code natively in the CPU's 32-bit compatibility mode, was dead for a
> reason that turned out to be only half true. In November it was re-measured, made to work, and
> it is now the shipping design: every 32-bit title in `corpus/titles/` runs on the native
> runtime (`gh_x86_native`). This write-up is the history of the two engines that came before
> it. The native runtime has its own write-up, [x86-native.md](x86-native.md), and its own
> reference, [x86-guest.md section 3](../architecture/x86-guest.md#3-the-native-runtime).

## Summary

- The console cannot hand a 32-bit program to the OS: no x86 app packages, no WOW64. In August
  2026 compatibility mode was also ruled out because the FS segment base cannot be set from user
  mode. That left binary translation.
- Guest memory is reserved below 4 GB, so a guest pointer is a host pointer. No engine in the
  tree has a soft MMU. This was the most valuable early result and both later engines kept it.
- The portable interpreter is correct and well tested, and runs 6.4–8.8 M guest instructions per
  second on the console: about 429 times slower than a native core (3–5 G instructions per second). Stick Fight: The Game needs
  about 244 G instructions to load its first scene's assets. That is about 11 hours.
- BoxedWine's GPL-2.0-or-later JIT was adopted with its soft MMU removed. On a benchmark guest it was 138
  times faster than our interpreter, and BoxedWine's plain interpreter alone was 11 times faster
  than ours.
- On the real game the JIT was a net loss: it needed 7.2 times more instructions to reach the same
  first frame, because it ran a different program path. That divergence was never bisected.
- Two headline numbers did not survive: a "4.1x" gain from reporting one processor was really
  1.07–1.38x, and the theory that the cooperative scheduler had inflated the 244 G was wrong.
- The interpreter remains the code default and the CPU reference. The JIT remains a research build
  that default builds do not link. Neither runs a shipped title.

## The problem

Every guest before August 2026 was x64. The loader rejected a PE32 outright with
`x64 guests only`, and that closed off a large part of a typical Steam library: many 2010s indie
and mid-size games shipped x86-only, or shipped x86 as the default build. The first target was
Stick Fight: The Game, a 32-bit Unity 5.6 title with Mono and a D3D11 renderer, chosen because it
rides the existing D3D11 path and keeps Direct3D 9 off the critical path. (An earlier pick turned
out to be a custom D3D9 engine and was dropped.)

### Four substrates, and why three were declared dead

| Substrate | Verdict in August 2026 | Reason given | Final state |
|---|---|---|---|
| Ship an x86 app package | Dead | Xbox accepts x64 packages only | Still dead |
| WOW64 | Dead | A process becomes WOW64 at creation, by the kernel; Greenhouse cannot create that kind of process, and the OS image almost certainly has no SysWOW64 | Still dead |
| Compatibility mode ("Heaven's Gate": far transfer to the 32-bit code selector inside our own x64 process) | Dead | The mode switch worked on the console, but `rdfsbase` raised #UD, so FSGSBASE is off and the FS base cannot be pointed at a 32-bit TEB. Every 32-bit Windows program reads `fs:[0]`, `fs:[0x2C]` and `fs:[0x30]` | **Shipping design since November 2026** |
| Binary translation | Chosen | Owning the CPU makes FS an ordinary field in a struct | Superseded |

The compatibility-mode verdict was right about FS and wrong about the conclusion. The November
probe (`src/spikes/x86_n0_probe.cpp`) confirmed the FS base is zero and is reset on every context
switch, but found that in compatibility mode the GS base is the low 32 bits of the 64-bit TEB
address. Placing the 32-bit TEB at that address and rewriting each `fs:` prefix to `gs:` the first
time it faults gives the guest its TEB. The details are in
[x86-guest.md section 3.2](../architecture/x86-guest.md#32-fs-without-an-fs-base-the-gs-alias).
In August the question "is there another segment register whose base we already know?" was not
asked.

### The sequencing decision

The developer chose to write an interpreter first and defer the JIT to a measured decision. The
reasons: an interpreter is small and debuggable, it carries no licence, and it lets the whole
non-CPU stack (loader, imports, threads, COM, windowing) be brought up before anyone knows how fast
the CPU has to be. Reusing BoxedWine's x86-to-x64 JIT, which already ran inside a UWP container on
Xbox, or Unicorn's QEMU core stayed open. Both are GPL-2.0, which at the time meant a
source-disclosure obligation on a signed MSIX handed to another person. That was to be weighed
with profile data, not before.

## The investigation

### Guest pointer equals host pointer

The first console probe asked `VirtualAlloc2` for memory with an address requirement of
`0x10000` to `0xFFFFFFFF`. The app container granted it, including a 256 MB block, and code made
executable in that range ran. So guest memory is one reservation below 4 GB
(`gh_x86_win.h: low_reserve`), and any 32-bit guest pointer can be dereferenced by host code as
it stands. Beds pass guest buffers straight to real Win32 (`ReadFile` into a guest buffer, a
D3D11 call on a guest-allocated struct) with no copy. There is no address translation anywhere,
and this property later decided how BoxedWine was ported.

The second property was a code-layout rule: `gh_x86_cpu.h` and `gh_pe32.h` include no Windows
headers. Only `gh_x86_win.h` does. That is what makes `tests/x86/` possible on a Linux machine,
and nearly every CPU bug was found there in a second instead of in a deploy round trip.

### The interpreter and its test loop

The interpreter decodes and executes one instruction at a time. MMX, SSE and SSE2 live in
`gh_x86_sse.h`; the opcode set was chosen by compiling test C with clang for a Pentium 4 target and
counting the mnemonics it emitted. x87 lives in `gh_x86_x87.h` and is not optional: the 32-bit
Windows C ABI returns `float` and `double` in `ST(0)`, so even an SSE-only build runs `fld` and
`fstp` around every float-returning call. x87 registers are held as `double`, the same 53-bit
precision the Windows CRT sets at startup, with explicit conversion for 80-bit loads and stores.

The test suite (`tests/x86/test_cpu.cpp`, `cases.c`) grew to about 11,800 assertions by the JIT
decision. Its main tricks:

- The host is a free oracle for integer semantics: 32-bit ALU results and flags on x86-64 are
  identical to 32-bit x86, so thousands of operand combinations are run on both.
- `cases.c` is compiled to freestanding i386 and to native code at `-O0`, `-O1` and `-O2`, and the
  results must agree. SSE cases are bit-exact against the host because SSE is IEEE-754.
- x87 is tested behaviourally (which register, whether the stack popped, the reversed `DC`/`DE`
  subtract and divide encodings, condition bits, rounding modes), because 80-bit internals make
  host disagreement a property of x87, not a bug. The suite is mutation-checked.
- Self-verifying console guests (`m2guest.c`, `m3bguest.c`) give each property one bit of the exit
  code, so a failure names itself. `m3bguest` first returned 991 instead of 1023: one bit short,
  which said "the auto-reset event is wrong" and nothing else.

The opcode-coverage tool (`tests/x86/test_coverage.cpp` with `toolchain/x86_opcode_corpus.py`)
lists every instruction encoding in a binary and reports which ones the decoder rejects, before
anything is deployed. See [testing.md](../contributing/testing.md) for how to run all of these.

### Guest threads are green

Guest threads are cooperative green threads on one host thread (`Machine::run_turn`), switched at
a fixed instruction slice through `Cpu::slice_end`. The reasons were determinism (a run is exactly
reproducible, and fixes were confirmed by the instruction count coming back bit-identical), keeping
`Machine` single-threaded, and the fact that parallelism is worth nothing at 300 times off native.

The cost is that every blocking API must yield, not block: `Sleep`, the wait functions, events,
mutexes, semaphores and critical sections all became beds. Two bugs fixed the shape. A thread must
never be left parked on the host stack, so a blocking bed asks to be re-executed on the next turn
(`GuestThread::wait_pending` keeps the deadline across retries). And the scheduler's wake must not
consume the signal: `could_proceed` only checks, the bed's wait then checks and consumes. A later
bug in the same area is worth knowing: a `Sleep` was treated as a wait with an empty handle set and
re-promoted on every scheduler pass, turning two sleeping threads into tens of thousands of host
calls a second. Fixing it took the steady state from 0.95 to 6.90 M instructions per second.

### The first timing: 8.8 M instructions per second

Until v0.0.182 the track had no timing at all, and the instruction count itself was wrong:
`call_guest32` saved and restored the whole CPU, including `insn_count`, so every instruction run
inside a guest callback was rolled back. Mono's `DllMain` alone ran 107 M such instructions. Counts
recorded before that build are undercounts.

Measured from launch to the thread-creation stop on Stick Fight:

| Build state | Instructions | Wall | Rate |
|---|---|---|---|
| Before the `VirtualQuery` fix | 128,871,022 | 30.5 s | 4.22 M/s |
| After | 128,871,022 | 15.6 s | 8.27 M/s |
| After, diagnostics off (`quiet`) | 128,871,022 | 14.7 s | 8.79 M/s |

Half the wall clock had not been the interpreter. Mono's garbage collector walks the address space
with `VirtualQuery`, and the bed forwarded to the kernel, which walked a quarter of a million page
table entries per call across our single 1 GB region: 0.889 ms a call, 16,978 calls. The region is
ours and its state is known exactly, so `Machine::guest_region_query` now answers in constant time.
The run stayed bit-identical, which was the evidence that both answers agree. With three live
threads the rate was 7.4 M/s. Turning diagnostics off bought 6%.

That put the interpreter 300–500 times slower than one native core, on an estimate of 2,000–4,000 M
instructions per second for a console core. §30.21 later measured native at 3–5 G per second, about
429 times the interpreter. The conclusion at the time: reaching a main
menu was possible, playing was not.

### Eleven hours of asset loading

The interpreter carried Stick Fight through Mono startup, a real D3D11 device, a swapchain and one
presented frame. That frame was the splash screen. The main thread then waits for the asset
preloader, and the preloader was making steady progress: monotonic reads across 5.42 G
instructions, at about 1,700 interpreted instructions per byte.

Two fixes on the way changed the cost but not the conclusion. Our `VirtualQuery` reply had told
the collector that committed memory ran up to the top of the arena, so it scanned every thread
stack that far. Fixing that (v0.0.229.0) cut instructions to reach asset streaming from 1,250 M to
700 M and wall clock from 231 s to 129 s. The streaming rate did not move. The freed budget went to
another thread's polling loop, because a cooperative scheduler hands out turns by count, not by
usefulness.

The decision number (v0.0.229–0.0.234): the interpreter ran the game at 6.4–7.9 M instructions per
second, and windows with no host calls (pure interpretation) at 6.37–6.57 M/s. The scene's two
asset files total 143.7 MB. At about 1,700 instructions per byte that is 244 G instructions:

| Engine speed | Rate | Time for 244 G |
|---|---|---|
| Our interpreter | 7 M/s | 9.7 h |
| JIT at 10x | 70 M/s | 58 min |
| JIT at 50x | 350 M/s | 12 min |
| JIT at 150x | 1.05 G/s | 3.9 min |
| JIT at 300x | 2.1 G/s | 1.9 min |

Natively, 244 G is 49–81 seconds on one core, an ordinary load time. Unity's deserialisation is
expensive per byte, and the work is real. The table said a JIT was required and a mediocre one
would not do: only the 150–300x band gives a tolerable load.

### Decision: BoxedWine's JIT, without its memory model

On 2026-08-22 the developer chose reuse: BoxedWine's x86-to-x64 JIT, accepting its GPL-2.0-or-later licence. The
pieces taken were its decoder, JIT frontend, x86/x64 code generator, the shared lazy-flag and
FPU/SSE helpers, and asmjit (zlib licence) as the encoder.

The one thing not taken was its memory model. Upstream emits a soft-MMU page-table lookup on every
guest memory access: shift to a page number, a dependent load of the page's host base, a permission
test, a mask. Adopting that would break every bed that hands a guest pointer to real Win32. So the
port replaced six memory-access emitters with identity addressing: the lookup collapses to a 32-bit
register move, the register that held the page table is freed, and page-straddling accesses become
ordinary unaligned host accesses. The divergence from upstream is kept as
`toolchain/boxedwine.patch`, applied by `toolchain/build_bwjit.sh` to a clone fetched at a pinned
commit. BoxedWine is never vendored.

The seam (`x86/jit/gh_jit_api.h`, namespace `ghjit`) turned out small:

- A kernel shim (`gh_bw_shim.cpp`, about 450 lines) stands in for BoxedWine's emulated kernel.
  Parts Greenhouse replaces outright (its Linux syscall entry, OpenGL and X11 bridges, VFS) are
  named aborts, not plausible stubs.
- Preemption: upstream leaves compiled code when another OS thread posts a signal. Green threads
  have no other thread, so a compiled loop that never exits would be a deadlock. The signal flag
  became a countdown decremented at backward branches and block boundaries.
- Host calls needed no bridge. A guest call into the thunk region is an indirect jump to code that
  was never compiled, so it always returns to the run loop, where one range check catches it.
- The plan was to keep the interpreter as the reference and compare architectural state with the
  JIT at block boundaries.

### 138x, and the 11x nobody was looking for

Measured on the console with `tests/x86/benchguest.c` (v0.0.240.0, 2026-08-22). The guest times
four fixed loops itself with `GetTickCount`, so no instruction counter is needed. All engines
returned the same result, and totals reproduced to 0.05% on a re-run of the unchanged build.

| Phase | Greenhouse interpreter | BoxedWine interpreter | BoxedWine JIT | JIT vs ours |
|---|---|---|---|---|
| Dependent integer ALU | 14,610 ms | 1,234 ms | 94 ms | 155x |
| Pointer-chasing loads | 9,953 ms | 875 ms | 141 ms | 71x |
| Byte codec | 16,703 ms | 1,562 ms | 125 ms | 134x |
| SSE float | 21,469 ms | 2,016 ms | 93 ms | 231x |
| **Total** | **62,735 ms** | **5,687 ms** | **453 ms** | **138x** |

That is about 985 M guest instructions per second on hot code, and it put the eleven hours at
roughly five minutes on paper.

The middle column mattered more. BoxedWine's interpreter, with no code generation at all, is 11
times faster than ours. The reason is not clever handlers. BoxedWine decodes each instruction once
into a `DecodedOp` with a direct handler pointer and caches it per guest address. Ours re-decodes
every instruction every time it runs. About nine tenths of our interpreter's work was decoding
instructions it had already decoded.

### Making the JIT run the game at all

The game under the JIT exited during CRT startup, 5.8 M instructions in. The `jitemu` option
(interpret a range of BoxedWine instruction kinds instead of compiling them; bare `jitemu` compiles
nothing) showed it failed identically with zero blocks compiled, so the defect was in the port,
not in code generation.

A diff of host-call traces between the engines appeared to diverge at a `LeaveCriticalSection`
call. That was a false lead. An instruction-trace sampler (`eips=`, `eipfrom=`/`eipto=`, built with
`GH_X86_EIPTRACE`), emitting identical lines from both engines, showed BoxedWine making the same
call. The real cause was in the log all along: the guest's C runtime had reported that it needs an
SSE2 CPU and exited. The shim had set BoxedWine's `pentiumLevel` to 6 thinking it was a family
number; it is a preset selector, and only 4 advertises SSE2. The guest asked CPUID once and
believed the answer.

The fix was not a different constant. `gh_x86_cpuid.h` now holds `gh_cpuid` and `gh_rdtsc`, and
both engines read it, because two engines answering CPUID differently are running two different
programs. The same pass found BoxedWine's `RDTSC` running backwards at every block boundary
(nothing in the shim advanced its instruction count); it now accumulates and both engines scale it
the same way.

Then the JIT hung in a floating-point loop that never terminated, at 270 M instructions per
second. Interpreting all x87 instruction kinds (BoxedWine kinds 499–631, `ghjit::kX87InstFirst` to
`kX87InstLast`) removed the hang, and that became the default. The next stop was the guest raising
`STATUS_FLOAT_OVERFLOW` from its own CRT math error path. Cause: BoxedWine's CPU constructor
initialises the FPU and then zeroes it two lines later, leaving the x87 control word at 0, with
every exception unmasked. Upstream never notices because Wine's ntdll sets the control word on
every thread. The fix is in our shim's `thread_new`, using `kGhX87InitCW = 0x027F`, the value a
Windows thread starts with (53-bit precision, not `FNINIT`'s `0x037F`). Two lines took the run
from 37.9 M instructions to 2.7 G (71 times; 3.7 G on a longer run), through Mono initialisation and into managed game code, and to
the same splash frame the interpreter had reached.

The JIT then crashed with a null write. Crash reports from JIT code named no module, so the JIT now
records which guest block each piece of generated code came from (`host_rip_to_guest`), and the
crash observer prints `JIT code for guest block <addr>`. That named a copy into memory the game's
own allocator had failed to provide. Our allocators were at 188 MB of 1024 MB and not failing. The
same exercise showed that with identity addressing a guest memory fault arrives as a host access
violation in BoxedWine's interpreter too, not only its JIT. Only our interpreter routes real memory
faults into the guest's `fs:[0]` SEH chain.

### A real-Windows oracle, and one honest processor

Every bug so far had been found by deploying and reading where the guest stopped. `toolchain/vmdiff/`
added the other half: a 32-bit tracer DLL (`ghtrace32.c`) injected into the real game on a Windows
11 control VM, and `ghdiff.py`, which lines its trace up against ours per thread. Three design
points: hook only modules under the game's directory (`in_game_tree`), which is also where our beds
sit; match threads by start routine and creation order, never by thread id; and no 64-bit division
in the freestanding tracer. Our side is the `synctrace` option (`is_sync_call`); its function set
and the tracer's `kHooks` must be kept in step.

The first diff showed real Windows making 307 `WaitForSingleObject` calls during startup to our
11, and 0 `TryEnterCriticalSection` calls to our 827. `fill_sysinfo32` had passed the host's
processor count, 8, to the guest. Unity sizes its spin limits from that number. On green threads a
spinner owns the only execution resource, and the producer it waits for cannot run until the
spinner's 200,000-instruction slice expires. The guest is now told there is one processor
everywhere it can ask (`kGuestProcessorCount`, `kGuestAffinityMask`, the affinity mask API and
`NUMBER_OF_PROCESSORS`). The polling loop that had dominated the profile dropped to a single 0.8%
appearance.

That fix was first written up as a 4.1x gain. Re-measured at milestones both builds actually reach:

| Milestone | Interpreter, 8 processors | Interpreter, 1 processor | Gain |
|---|---|---|---|
| First asset file at 1.1 MB | 2.54 G instructions / 333.0 s | 1.84 G / 248.3 s | 1.38x |
| Second asset file at 100 MB | 1.19 G / 162.2 s | 1.09 G / 151.0 s | 1.09x |
| First presented frame | 535.9 M / 76.1 s | 499.4 M / 71.6 s | 1.07x |

On the JIT the same fix changed the first frame by 21,551 instructions out of 3.6 G and wall clock
went from 131.5 s to 137.6 s.

### The JIT ran a different program

Same build, same options, same guest, only the `jit` option differing (spike 0.0.263.0):

| | Interpreter, 1 processor | JIT, 1 processor |
|---|---|---|
| Main asset file read at about 1.0 G instructions | 91.4 MB | never opened |
| Main asset file read at about 1.8 G instructions | 109.4 MB | never opened |
| First presented frame | 499,441,365 instructions / 71.6 s | 3,616,415,599 instructions / 137.6 s |

The JIT needed 7.2 times the instructions to reach the same first frame and did not open the main
asset file until just before it crashed. This was checked against a counting artifact: the
benchmark guest run on both interpreters (7.08 and 76.38 M/s) stopped 16 instructions apart out of
446 M, so the counters agree. The profiles were disjoint. The interpreter sat in asset decoding
(texture format conversion, audio decoding). The JIT sat in per-frame rendering code with no assets
loaded. About 3.5 times faster per instruction, about 1.9 times slower to the milestone.

The track was pinned there on 2026-08-23. The next step written down was to bisect where the two
paths separate (before 1.0 G instructions) with the instruction-trace sampler and its `eipfpu`
column, suspecting state the guest reads once at startup, like CPUID and the x87 control word. That
bisection was never done. In November the native-runtime probe made the question moot: running the
guest's own code on the real CPU has no instruction rate to improve and no second CPU model to
disagree with.

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| Compatibility mode is dead for real programs | FS base cannot be set from user mode (`rdfsbase` raised #UD) | The November probe found the GS base in compatibility mode is low32(TEB64); a TEB32 placed there plus a one-time `fs:`-to-`gs:` prefix patch works. It is the shipping runtime ([x86-native.md](x86-native.md)) |
| Reporting one processor gave a 4.1x speedup | A windowed bytes-per-second rate before and after | Instructions and seconds to milestones both arms reach: 1.07x to 1.38x on the interpreter, 1.00x on the JIT. Windowed rates drift about 20% between identical builds |
| 244 G instructions is inflated by the cooperative scheduler feeding waiting threads | It seemed far too large for a game's load | The native baseline behind that claim was wrong. At 3–5 G instructions per second a native core does 244 G in 49–81 s, an ordinary load time. The work is real |
| The JIT and interpreter diverge at a `LeaveCriticalSection` call | A host-call trace diff appeared to show the JIT skipping it | The per-instruction sampler showed both engines make the call. The real cause was CPUID not advertising SSE2 |
| Yielding on `PAUSE` is a net loss | A whole-run byte rate fell | Inconclusive: whole-run rates drift. Later the guest was shown to execute zero `PAUSE` instructions; the "spin" was a work-stealing wait that a 4 KB profiler bucket had blamed on a nearby function |
| A per-instruction test in the dispatch switch costs 21% | Successive builds were slower | Rebuilding the unchanged source reproduced the result; windows with no host calls ran at the same rate on every build. The drift was the collector's scan growing with the arena |
| The asset loader is looping, not progressing | One file handle stopped advancing | The guest had moved on to the other asset file, whose position kept climbing. Check every open handle |
| A guest fault arriving as a host access violation is a JIT problem | It was first seen under the JIT | It reproduced with compilation off: identity addressing puts the same gap in BoxedWine's interpreter |
| JIT x87 code-generation defects near the integer-load and compare instructions | Bisected with `jitemu` ranges | Not overturned but undermined: they were bisected while the control word was 0, which also sent every x87 divide down a guarded slow path. Unverified since |
| Instruction counts before v0.0.182 | The counter was trusted | `call_guest32` rolled `insn_count` back on every callback return; earlier counts are undercounts |

## What shipped

Verified in the tree on 2026-11-13. Reference detail is in
[x86-guest.md](../architecture/x86-guest.md) sections 4 and 5.

- The interpreter (`gh_x86_cpu.h`, `gh_x86_sse.h`, `gh_x86_x87.h`) is built in and is the code
  default: `gh_x86_native` defaults to 0, which selects `ghx86::RunPe32Guest`. All three shipped
  32-bit title configs set `gh_x86_native` to 1, so no shipped title runs on it.
- The JIT is optional: `toolchain/build_bwjit.sh` fetches and patches BoxedWine, `GH_X86_JIT` links
  it, and `jit` in LocalState `ghx86opts.txt` selects it. Default builds do not link it
  ([building.md](../building.md#optional-the-boxedwine-jit-build)). x87 kinds 499–631 are
  interpreted by default (`ghjit::kX87InstFirst`, `kX87InstLast`); `jitemu=lo-hi` overrides.
- Shared CPU identity: `gh_x86_cpuid.h` (`gh_cpuid`, `gh_rdtsc`, `kGhX87InitCW`). It advertises
  SSE2 and nothing newer. The native runtime uses it too.
- One processor: `kGuestProcessorCount` and `kGuestAffinityMask` in `gh_x86_beds.h`, used by
  `fill_sysinfo32`.
- Constant-time `VirtualQuery` for the guest region: `Machine::guest_region_query`.
- Interpreter diagnostics in `ghx86opts.txt`: `quiet`, `stall=`, `wprof`/`ptid=`, `synctrace`,
  `eips=`/`eipfrom=`/`eipto=`/`eipfpu` (with `GH_X86_EIPTRACE`), `spin=` (with
  `GH_X86_SPIN_YIELD`; 0 means `PAUSE` is a plain no-op).
- Allocation failures are logged with size and reason, and both guest allocators refuse sizes that
  would overflow their rounding instead of returning a short block.
- Tools: `toolchain/vmdiff/` (real-Windows API trace diff), `toolchain/x86sum.py` (engine,
  milestones, asset offsets, profile from a diag log), `tests/x86/benchguest.c` (self-timing
  benchmark), `toolchain/iterate-x86spike.sh` (deploys a test guest to a separate package slot;
  it touches a device).
- Everything above the CPU (the PE32 mapper, thunks and beds, green threads, COM, windowing) was
  built for these engines and is shared with the native runtime.

## Still open

- Why the JIT runs a different program path from the interpreter on Stick Fight. Never bisected;
  the null-write crash under the JIT is probably a symptom of it.
- Whether the two x87 code-generation defects exist with a correct control word.
- The interpreter's 11x gap to BoxedWine's interpreter. A decoded-instruction cache would close
  most of it. Nobody has built one, because the native runtime made interpreter speed matter only
  for tests.
- Stick Fight itself never got past its splash screen on either engine. It has no entry in
  `corpus/compat/compat.json` and no title config, and this write-up's sources do not record a
  run on the native runtime.

## Lessons

- **A windowed rate is not a milestone.** Measure instructions and seconds to a point both arms
  reach; windowed rates on this console drift about 20% between identical builds and produced a
  4.1x that was really 1.07–1.38x.
- **Report one processor to a green-threaded guest.** A guest that believes it has spare cores
  spins instead of blocking, and on green threads the spinner holds the only core the producer
  needs.
- **Re-run the unchanged build before blaming a change.** The 21% slowdown and its explanation
  were both invented; a version-number-only rebuild would have shown it in one run.
- **A JIT's speed per instruction says nothing until it runs the same program.** 138x on a
  benchmark became a net loss on the game because the path diverged.
- **Two engines must share one answer to "what CPU is this".** CPUID, the time stamp counter and
  the initial x87 control word each made the JIT run a different program until they came from one
  header.
- **Decode once.** Most of an interpreter's cost can be re-decoding; the 11x gap to BoxedWine's
  interpreter was that and little else.
- **A bed that forwards to the host inherits the host's cost model.** `VirtualQuery` was half the
  wall clock because the kernel was answering a question we could answer ourselves.
- **A profiler bucket wider than a function names a neighbourhood.** The 4 KB bucket blamed a spin
  loop that never executed its `PAUSE`; count the instruction you think is hot.
- **Re-measure the assumption that killed an option.** Compatibility mode was ruled out on one
  measured fact and one unasked question; three months later it became the design.
