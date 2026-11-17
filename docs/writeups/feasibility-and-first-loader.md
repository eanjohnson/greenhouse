# What the feasibility study got wrong, and the first loader

What this covers: the feasibility study written before any code (what an x64 PE inside the
Dev Mode app container could and could not do), which of its "no"s turned out wrong, the
executable-memory spike, the first PE loader as it was built against command-line tools, and
the in-process `CreateProcess` surrogate with its later limits.
Where the code is: `src/host/App.cpp` (the original spike host); `src/loader/guest_loader.cpp`:
`MapAndRun`, `ResolveImport`, `ShimTable()`, `PatchModuleForGuest`, `Bed_GetProcAddress`,
`GhGuestExit`, `FinishGuestSession`, `Diag`, `SetupGuestTls`, `InstallGuestTlsForThread`,
`TryRealGuestTls`, `LoadGuestDll`, `Bed_CreateProcessW`, `MapChildImage`, `ChildThreadProc`,
`Bed_MelonCreateProcessW`; test guests `corpus/proc/parent.cpp`, `child.cpp`, `minishell.cpp`.
Related: [overview.md](../architecture/overview.md) · [pe-loading.md](../architecture/pe-loading.md) · [api-shims.md](../architecture/api-shims.md) · [x86-guest.md](../architecture/x86-guest.md) · [memory.md](../architecture/memory.md) · [graphics.md](../architecture/graphics.md)
Period: 2026-05-24 (study) to 2026-07-24 (shell script through the surrogate), with corrections
up to 2026-11-01. Loader 0.0.51.0–0.0.87.0. Notebook sections: §0–§12, §17–§18, prior art and
appendices A–B; later corrections from §13.1, §15, §29.7, §30, §33, §54, §58.

## Summary

- The study's core bet held. In the Dev Mode app container a page can be written, flipped from
  read-write to execute-read and called. The spike returned 42 on the first on-device run, and
  every guest since runs on that one capability.
- Its picture of the import surface was wrong in a way that shaped the whole loader. The study
  planned one dispatch table. In practice a guest reaches Win32 through its own import table,
  through its CRT's import table (the CRTs are already loaded by the host and shared), and
  through `GetProcAddress` at run time. Later a fourth route was added: callers that bind the
  real export directly.
- Two of its hard "no"s fell. `CreateProcess` fell first (an in-process surrogate, §17), then
  real child processes turned out to work (§54). The missing window tree fell to a synthetic
  window (§15). Other assumptions were measured wrong: native D3D11 in the container is feature
  level 10_1 (§13.1), the memory cap does not lift under a debugger (§33), and the x86 track
  ended in hardware compatibility mode, which §30 had declared useless (§58).
- The first loader ran eight unmodified CLI tools across msvcrt, UCRT, static CRT, Rust, Go and
  CPython, all with real exit codes, by loader 0.0.62.0. busybox-w32 was left broken on purpose.

## The problem

The study (2026-05-24) asked whether a single UWP app on a retail Xbox in Developer Mode could
load an unmodified Windows PE into its own address space and answer the APIs the container does
not. Its argument rested on three facts:

1. The consoles are x64, so an x64 PE's instructions run directly. The job is a loader and an
   API translator, not a CPU emulator.
2. Dev Mode relaxes the sandbox enough that community emulator ports JIT on it.
3. Most "blocked" Win32 functions exist on the box. The container blocks them at link time (they
   are outside the app partition), not necessarily at run time, so resolving them dynamically
   often reaches a working implementation.

From those it drew a verdict: "partially plausible", with the realistic target being
console-class software (CLI tools, D3D apps, emulators), and a list of hard blockers "no shim
removes": no second process, no desktop window manager or HWND tree, no kernel drivers, no
DRM or anti-cheat software, no Store distribution. 32-bit PEs were filed as a separate
emulation track. It rated each capability and each graphics API, and named executable memory
"the single biggest external risk", to be tested in week one.

Fact 3 proved right and became the loader's default policy: an import binds to the real export
unless a bed exists for it. Fact 2 proved right in the spike. The rest of this write-up is how
the plan met the container.

## The investigation

### The study's ratings against the final state

| Capability | Study's verdict | Final state |
|---|---|---|
| x64 CLI tool | High, MVP | Done by loader 0.0.62.0: eight tools (below) |
| x64 D3D11/D3D12 game | Medium-High, "retarget the swapchain" | Done, but D3D11 is served by D3D11On12 on a D3D12 device, not by the native D3D11 runtime ([graphics.md](../architecture/graphics.md)) |
| Executable memory | Medium-High, biggest risk | Confirmed by the spike, 2026-07-22 |
| Memory | 5 GB in Game mode; caps "don't apply under the Visual Studio debugger" | 5,120 MB confirmed; the debugger claim is false for Game mode (§33) |
| Filesystem | VFS over LocalState | Done; still the model ([filesystem-registry.md](../architecture/filesystem-registry.md)) |
| Registry | In-process hive | First answered "not found" for everything; a persisted per-game hive came later |
| Winsock | Forward to the real `ws2_32` | Correct: no beds were needed for curl |
| `CreateProcess` | No; single process only | Surrogate (§17); real children also work (§54) |
| Desktop window / HWND tree | No real windows (win32k lockdown, Appendix A) | Synthetic window (§15). A real `CreateWindowEx` was never needed |
| 32-bit x86 PE | Separate emulation track | Interpreter first (§30), then native compatibility mode (§58) |
| Kernel drivers | No | Still no |
| DRM / anti-cheat | No | Still no: such titles are recorded `WONT_RUN` with a wall |
| Store distribution | No | Still no; Dev Mode only |

### P0: read-write to execute-read, result 42 (2026-07-22)

The spike host (`src/host/App.cpp`, the minimal CoreApplication entry point the project grew
from) allocates a page read-write, writes six bytes of our own machine code (`mov eax, 42;
ret`), changes the page to `PAGE_EXECUTE_READ` with `VirtualProtect`, calls it, and writes the
result to a file in the app's local folder so the harness can pull it over the Device Portal
without a debugger. The same sequence had been run under Wine first; Wine cannot answer the one
question that mattered, which was the container's policy.

On the console the trace read `entered_main`, `about_to_call_exec_mem`, `PASS result=42`,
`mta_initialized`, `run_returned_ok`. Dynamic code is allowed in the Dev Mode container.

Two host findings from getting the spike to activate at all, both still true:

- A CoreApplication host must enter the multithreaded apartment
  (`winrt::init_apartment(multi_threaded)`) before it first touches `CoreApplication`.
  Otherwise activation fails with `RPC_E_WRONG_THREAD`, which the console reports only as an
  opaque launch error. The implicit apartment fallback in C++/WinRT was not enough. The exe must
  also be built `/SUBSYSTEM:WINDOWS` and `/APPCONTAINER`.
- A package without a valid `resources.pri` fails activation with `0x80070002` before the
  process starts. The tool that makes it does not run under Wine, so it ran on a Windows
  machine driven over SSH.

### P1: the first tool (7-Zip)

`guest_loader.cpp` started as the stages it still has ([pe-loading.md](../architecture/pe-loading.md),
section 2): read the PE, `VirtualAlloc` its image at the preferred base, copy headers and
sections, relocate if needed, bind imports, set per-section protections (the spike's capability
in production), register `.pdata` with `RtlAddFunctionTable` (required: the CRT wraps `main` in
`__try`), and call the entry point.

Import binding was "selective IAT" from the first day: the shim table wins by function name,
otherwise the real OS DLL's export. For 7-Zip that was 145 imports: 10 shimmed, 135 real, 0
missing. That ratio is the study's fact 3 in numbers.

The first beds were the minimum to see output and survive exit:

- `GetCommandLineW` returned a command line read from `ghcmd.txt` in LocalState, so scenarios
  changed without a rebuild.
- `GetStdHandle` returned sentinel handles, and `WriteFile`, `fputs`, `fputc`, `fflush` on them
  were captured to a file.
- `exit` and `_exit` recorded the code and `longjmp`ed back to the loader, so the guest could not
  tear down the host. This choice was wrong, and Go exposed it later.

Unmodified 7-Zip 26.02 printed its banner and compressed a file; the archive extracted
byte-identical on a PC.

### The rest of the P1 ladder: VFS, registry, Winsock, curl, sqlite3

- **VFS.** Every path-taking file API was redirected under `LocalState\vfs\<drive>\`. A
  host-bridge drive `H:` mapped to the LocalState root, because the Device Portal could write
  there and not into nested folders. Handle-based APIs needed nothing. 7-Zip compress, list and
  extract round-tripped byte-identical.
- **Registry and security.** `RegOpenKeyExW` and `RegQueryValueExW` answered "not found" (the
  guest takes its defaults); `AdjustTokenPrivileges` reported "not held"; file security calls
  were no-ops. `OpenProcessToken` stayed real so the handle was genuine.
- **Missing imports.** An unresolved import got a generated stub that logs its first call and
  returns 0, instead of failing the load. This is still `MakeStub` / `MissingStubLog`.
- **Winsock.** curl resolved to the container's real `ws2_32.dll` and did HTTP and HTTPS
  (Schannel) with no beds. The study's networking line was right.
- **curl and the shared UCRT.** curl was the first guest whose CRT is the UCRT, and the UCRT is
  already loaded by the host's WinRT dependencies. It had cached the command line and the
  standard handles at host start, before the guest existed, and it calls kernel32 through its own
  import table, which the loader had not touched. Three workarounds: overwrite kernel32's cached
  ANSI and wide command-line buffers in place, inline-hook `GetCommandLineA/W` in kernelbase,
  and patch the PEB's command line; `_wfreopen` the UCRT's stdout and stderr streams onto files;
  intercept `exit`, `_exit`, `_cexit`, `abort` and `TerminateProcess` and flush the guest CRT
  before leaving.
- **CA bundle.** There is no reachable system certificate store. A `cacert.pem` ships in the
  package and the loader sets `CURL_CA_BUNDLE` to it (later also `SSL_CERT_FILE`). Plain
  `curl https://...` then verified the chain with no `-k`.
- **sqlite3** imports only kernel32 (static CRT). It needed an `ExitProcess` bed, because its
  CRT calls that directly rather than `exit`, and console beds. `SetConsoleCtrlHandler` and the
  console queries broker to conhost over COM in the container and fail-fast with
  `REGDB_E_CLASSNOTREG`, so they are neutralised (`Bed_SetConsoleCtrlHandler` and friends).

### Activate first; the hook that hooked itself (2026-07-23)

curl and sqlite3 ran correctly but the Device Portal reported their launches as failed. Two
causes, two fixes, all three tools then reporting launch success (HTTP 200):

1. **Activate first, then run the guest on a worker thread.** The app activates its CoreWindow
   before the guest runs and pumps the dispatcher while the guest runs on its own thread. Launch
   status no longer depends on the guest, and process-wide changes the guest needs (command line,
   standard handles) cannot disturb activation. This is still the shape of every launch
   (`GhStartGuestWorker`, [overview.md](../architecture/overview.md)).
2. **The restore re-applied the hook.** `kernel32!GetCommandLineW` and
   `kernelbase!GetCommandLineW` resolve to the same address. Hooking "both" saved the
   already-hooked bytes the second time, and the restore wrote the hook back. The host's own
   `GetCommandLineW` kept returning the guest's line, and activation hung. `InlineHook` now
   dedupes target addresses. Before this was found, the failure was blamed on the `_wfreopen`.

### The generality sweep: the second channel (2026-07-23)

Three hand-picked tools proved the model. The sweep picked tools by runtime, because the runtime,
not the application, is what stresses a loader:

| Tool | Runtime | Result at the sweep |
|---|---|---|
| 7-Zip 26.02 | msvcrt, dynamic | Pass |
| curl | UCRT, dynamic | Pass, HTTPS verified |
| sqlite3 3.53 | UCRT, static | Pass |
| zstd 1.5.6 | msvcrt, dynamic | Fixed by the sweep |
| jq 1.7.1 | UCRT, dynamic | Fixed by the sweep |
| ripgrep 14.1 | Rust | Abort: the allocator found garbage TLS |
| gron 0.7 | Go | Ran, could not open `H:\` paths |
| busybox-w32 | msvcrt, dynamic | Exited cleanly with no output |

Every failure traced to one pattern: a guest reaches the OS through its CRT, not through its
own import table. The CRTs are preloaded and shared with the host and call kernel32 through
their own import tables. Four fixes followed:

1. The command-line interception, gated to curl because it used to break activation, became
   unconditional. Activation now came first, so it was safe, and zstd, jq and busybox build argv
   inside their CRT.
2. The file APIs and the VFS surface were redirected in the import tables of `ucrtbase.dll` and
   `msvcrt.dll`, so CRT `fopen` honours the VFS.
3. The stdio redirect was applied to msvcrt as well as the UCRT.
4. That broke curl (exit 77): its CA path pointed at the real install folder, which the VFS then
   hid. Paths already rooted in the install folder or LocalState now pass through.

### Rust: the TLS bed (loader 0.0.51.0)

Static thread-locals (`__declspec(thread)`, Rust's `thread_local!`) compile to a direct read of
the TEB's TLS array, indexed by the image's `_tls_index`. There is no import to intercept. The
OS loader normally assigns the index and gives every thread a copy of the module's template; a
hand-mapped image gets neither.

`SetupGuestTls` parsed the TLS directory (after relocation, since its fields are VAs), wrote a
fixed index of 128 into the guest's `_tls_index` (the OS allocator is not exported), and for
each thread built a 256-entry array, copied the host's existing entries into it so host code
still found its own TLS, appended the guest's block, and published the array in the TEB.
`CreateThread` was bedded to wrap the start routine so guest threads got the same.

The finding worth keeping: **the TLS array must be process-heap memory.** Thread shutdown in
ntdll frees whatever the TEB's TLS pointer holds with the heap. A `VirtualAlloc` block there is
not a legal heap free, and the corruption killed curl's resolver threads and swallowed every
guest's exit path. The output still appeared, so it looked like an unrelated hang. The array and
block moved to `GetProcessHeap()`, and the original pointer was restored before a thread unwound.

ripgrep then ran, including `--threads 4` (four installs: main and three workers).

The fixed index did not last. ntdll replaces every thread's TLS array whenever a module with a
TLS directory loads, and the replacement holds only the slots ntdll knows. Repairs piled up
(republish after each DLL load, a fault-time repair) until loader 0.0.387.0 (2026-09-07)
replaced the scheme: small donor DLLs (`corpus/shims/ghtls.cpp`, built as `ghtls0.dll` and up)
each get a genuine OS index, and `TryRealGuestTls` points the guest's `_tls_index` at it. The
fixed-index path remains as a fallback behind `gh_no_real_tls`
([pe-loading.md](../architecture/pe-loading.md), section 7). The process-heap rule still applies
to it.

### Go: the third channel, and exit by `ExitThread` (loader 0.0.53.0)

gron's import table has 39 kernel32 functions and no `CreateFileW`. Go's `syscall` package
resolves every Win32 function at run time with `LoadLibraryW` + `GetProcAddress`, so no import
table bed could see it. The fix was to bed `GetProcAddress` itself: a name in the shim table
returns the bed. This is the load-time policy of `ResolveImport` applied at run time, and it
covers any guest that probes for optional APIs. The diag shows it as `GPA shim CreateFileW`.

The second Go bug was the exit path. `longjmp` is an unwinding operation: it runs the OS
unwinder over every frame between the exit call and the loader's `setjmp`. Go calls
`ExitProcess` from its runtime system stack, through assembly frames the unwinder cannot
describe. The unwind faulted with `0xC00000FF`, and Go's own vectored handler printed a crash
traceback.

The exit bed now finishes the session's bookkeeping (`FinishGuestSession`, made idempotent) and
calls `ExitThread` on the guest worker. `ExitThread` unwinds nothing, so it is safe from any
stack. `GhGuestExit` still ends that way.

This retired two residuals that had been written off as cosmetic. sqlite3's fail-fast in its
own static-CRT teardown, which §12.3 said would need "deep" work to locate, never happens now
because the thread ends before teardown runs. ripgrep's missing exit code came back, and it
carries meaning: no match returns 1, a match 0.

The three channels, as §12.6 stated them: the guest's own import table, its CRT's import table,
and `GetProcAddress` at run time. [api-shims.md](../architecture/api-shims.md) now lists a
fourth, added for managed runtimes: callers that bind the real export directly, which only an
inline hook on that export reaches.

### CPython: four structural bugs (loader 0.0.62.0)

CPython 3.11 embeddable was picked as the highest-information test left. It found four bugs,
two of which had been affecting every guest silently.

1. **Guests that ship their own DLLs.** `python.exe` is a small launcher for `python311.dll`.
   The container's default DLL search path does not include the package's guest folder, so every
   import stubbed out and `Py_Main` returned at once. `LoadGuestDll` gained the fallback it still
   has: load from the guest directory by full path with `LOAD_WITH_ALTERED_SEARCH_PATH`, so the
   DLL's own dependencies resolve from there too. Runtime `LoadLibrary*` got the same. Extension
   modules (`_sqlite3.pyd` and `sqlite3.dll`) then loaded.
2. **Our diagnostics clobbered the guest's `GetLastError`.** CPython aborted startup with
   `FileExistsError` on a `pyvenv.cfg` that was correctly absent. The trace call inside the file
   bed ran after the real API, and the log file's own open (`OPEN_ALWAYS`) set
   `ERROR_ALREADY_EXISTS` on success. The guest read that as the result of its own call. `Diag`
   now saves and restores last-error, and [api-shims.md](../architecture/api-shims.md) makes it
   a rule for every bed. CPython was only the first guest whose control flow depended on the
   distinction.
3. **The command line had an invisible length cap.** The in-place overwrite of kernel32's
   buffers only fits a line no longer than the host's own, and was skipped past that. Longer
   lines inherited the host's arguments, including a UWP activation argument beginning
   `-ServerName:`, which CPython parsed as clustered short options (`Unknown option: -e`).
   Calling `_configure_wide_argv` again does not help: the UCRT cached argv at host start, and
   the guest's own CRT startup recomputes from the cached pointer after the loader finishes. The
   fix builds argv from the guest command line (`BuildGuestArgv`) and inline-hooks
   `_configure_wide_argv` / `_configure_narrow_argv` to publish it.
4. **Guest-loaded DLLs need the beds too.** `open()` honoured the VFS (it goes through the
   UCRT), but `os.listdir()` called `FindFirstFileW` straight from `python311.dll`. The CRT patch
   was generalised into `PatchModuleForGuest`, applied to the CRTs and to every module loaded
   from the guest's directory, `.pyd` files included.

At 0.0.62.0 all eight tools launched with 200 and exited 0: 7-Zip, curl, sqlite3, zstd, jq,
ripgrep, gron and CPython (`-c`, scripts from `H:\`, sqlite3, hashlib and zlib values matching
the PC, and verified HTTPS through `ssl`).

### busybox-w32: left broken on purpose

busybox-w32 launched and exited cleanly, but no applet ran and nothing reached any stream. It
fails inside its own Windows-specific initialisation, before any file I/O. After the Go fixes it
was the one remaining failure, and it was deliberately not pursued: the cause was particular to
busybox-w32 and a fix would not have generalised. It is still not investigated.

### The CreateProcess surrogate (loader 0.0.76.0, 2026-07-24)

The study had listed `CreateProcess` as a hard blocker. The surrogate rests on one observation:
callers need the semantics of a child process, not a second address space. Those semantics are
a callable executable path, redirected standard handles, an exit code and a waitable handle.

`Bed_CreateProcessW` (and the `A` form) extracts the exe name, reads it from the guest folder,
maps it with `MapChildImage` (relocations, imports, protections, unwind data, no TLS) and runs
its entry point on a real Win32 thread (`ChildThreadProc`):

- The thread handle is returned as `hProcess` (and as `hThread`). `WaitForSingleObject` works
  natively, and `GetExitCodeProcess` is `GetExitCodeThread`.
- A thread-local child context (`t_child`) steers the exit and stdio beds. The child's
  `ExitProcess` ends only its own thread. Its `GetStdHandle` returns the handles from
  `STARTUPINFO` (typically a pipe).
- The child's argv is delivered by swapping the process-wide argv and command line for the
  child's before spawning. The parent is blocked in its wait, so there is no race. The child's
  CRT startup picks the new argv up through the same `_configure_wide_argv` hook.

The test pair in `corpus/proc/` uses only standard APIs. `parent.exe` creates an anonymous pipe,
calls `CreateProcessW` for `child.exe` with its stdout on the pipe, waits on `hProcess`, reads
the pipe and calls `GetExitCodeProcess`; `child.exe` echoes its command line and returns 42. The
first on-device run passed every check: the child's line came back through the pipe, the exit
code was 42, and a second spawn in the same run mapped at a different base with its own line and
output.

### A shell script through the surrogate (loader 0.0.87.0, 2026-07-24)

`corpus/proc/minishell.cpp` reads a script file and runs each line with `CreateProcessW`, stdout
piped back, recording output and exit code. The script ran five commands across three CRT
flavours: two sqlite3 queries (a string concatenation and a recursive CTE), `7za` with no
arguments (its 2,855-byte usage text), `zstd --version` and `jq --version`. All five exited 0
and their output arrived through the pipes.

The workload found two holes in existing beds and two in the surrogate:

1. **The UCRT's `ExitProcess` was unpatched.** A dynamic-UCRT guest returns from `main` into
   ucrtbase, which calls `ExitProcess` from its own import table. That went to the real
   kernel32 and killed the process past every exit bed. `ExitProcess` and `TerminateProcess`
   were added to `PatchModuleForGuest`. This was found trying `cmd.exe` as the shell.
2. **Guest DLLs could not see the surrogate.** `CreateProcessW/A` and `GetExitCodeProcess` were
   added to `PatchModuleForGuest`. Python's `subprocess` reached the surrogate immediately.
3. **Child `fputs` went to the parent's capture.** 7-Zip and zstd print through `fputs`, whose
   bed appended to the top-level capture. With `t_child` set, `Bed_fputs` and `Bed_fputc` now
   write to the child's handle.
4. **Some static CRTs read argv from the PEB or kernel32's buffer.** zstd saw the shell's own
   argument as an extra input file. The surrogate now also calls `PatchPebCommandLine` and
   `OverwriteKernel32Cmdline` for the child. The second is limited to the length of the host's
   original command line (about 180 characters), which a long test argument hit.

### Where the surrogate stopped

The surrogate's documented scope limits were called "easily liftable" in §17.1. Some were not.

- **Input to a child that shares msvcrt cannot be delivered** (§29.7, 2026-08-16). The launcher's
  depot downloader first tried to shell `zstd.exe` and `7za.exe` through the surrogate. The tools
  link the one shared `msvcrt.dll`, whose stdin descriptor was cached before any redirect, and
  each child also saw its own private VFS overlay, so scratch files did not cross from loader to
  child. A child could be spawned and its stdout captured, but its input never arrived.
  `PatchPebStdHandles` was added on the way and kept as correct for future children; it cannot
  help a CRT that has already initialised. The downloader now decompresses in process with
  bundled decoders and spawns nothing.
- **Some children are refused.** Mapping an engine's crash-reporter helper into the game's own
  address space, where it attaches to and waits on its "parent", killed one Unity game right
  after launch. Another Unity game had run fine when the helper was simply unreadable, because
  the engine treats a spawn failure gracefully. `Bed_CreateProcessW` now refuses any exe whose
  name contains `crashhandler` or `crashpad_handler` (`CP: refusing crash handler`), any
  Chromium sub-process (`--type=` on the command line), and a store client's web-helper process,
  all with `ERROR_FILE_NOT_FOUND`.
- **Shared everything.** Parent and child share one heap, one CRT, one set of globals in shared
  DLLs, and one command line, which is not restored for the parent. Children get no TLS of their
  own. A child crash is a parent crash.

### Real child processes after all (§54, 2026-10-25)

The surrogate's source comment says multi-process is dead in the container because a real
`CreateProcess` against a binary path returned access denied. Three months later a probe for
the mod loaders, with CoreCLR running inside the launcher, called `Process.Start("cmd.exe",
"/c exit 3")`. It was expected to fail. It started a child and returned 3. An unsigned,
self-contained .NET exe uploaded to the DevelopmentFiles share also ran as a child with
redirected stdout and stderr and exited 0.

The app container is not single-process. Appendix A and §4.4 of the study had assumed it was,
and nobody had tested it directly. What still rules out a browser-style multi-process app is the
missing window tree, not the process count. Two rules came out of that work:

- The loader's standard handles are non-inheritable sink files. A child handed one fails in its
  CRT startup with a strict-handle exception, so `Bed_MelonCreateProcessW` nulls any
  `STARTUPINFO` handle that is one of ours.
- A crash in any process of the package, a child included, makes the shell suspend and terminate
  the app.

One code path uses real children today (`Bed_MelonCreateProcessW`, for a mod loader's
out-of-process tool). Everything else still goes through the surrogate. Why the earlier attempt
was denied is not recorded.

### The other "no"s that fell later

- **Windowing (§15, loader 0.0.72.0, 2026-07-23).** A Win32 D3D11 test game
  (`RegisterClassExW`, `CreateWindowExW`, a `PeekMessage` loop, runtime HLSL through a shipped
  `d3dcompiler_47.dll`) rendered on the console. The loader answers with a synthetic window:
  `Bed_CreateWindowExW` returns one sentinel handle (`kFakeHwnd`), client rectangles report the
  display size, and the swap chain created against that handle is routed to the CoreWindow.
  Appendix A's predicted win32k block on real windows was never tested directly; the synthetic
  window made the question moot. See [window-input.md](../architecture/window-input.md).
- **Native D3D11 (§13.1).** Appendix B rated D3D11 "Very High, the golden path". On the console,
  the same hardware adapter gives feature level 12_0 under D3D12 and only 10_1 under D3D11 in the
  app container: no compute shaders. Every D3D11 guest is served by D3D11On12 over a D3D12
  device at 11_1 ([graphics.md](../architecture/graphics.md)).
- **x86 (§30, §58).** The study left 32-bit PEs to "an extra emulation track". §30 ruled out an
  x86 package and WOW64, and ruled out hardware compatibility mode because the FS base cannot be
  set for a 32-bit TEB. It built an interpreter, which ran but at 7 to 9 million instructions per
  second. §58 (launcher 0.0.821.0, 2026-11-01) made compatibility mode work after all by reaching
  the TEB through a `gs:` alias, and that native runtime is what every shipped 32-bit title uses
  ([x86-guest.md](../architecture/x86-guest.md)).
- **The debugger and the memory cap (§33, 2026-10-17).** The study, quoting Microsoft's resource
  page, said the caps do not apply under the Visual Studio debugger. Under a debug launch in Game
  mode on Xbox One, the limit stayed at 5,120 MB and commit was refused at the same ceiling
  ([memory.md](../architecture/memory.md)).

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| `CreateProcess` is a hard blocker no shim removes (study §0, §2.4, §3.5) | Single-process app model; the study's own §3.5 expected "launch cmd.exe and pipe" to fail | §17: an in-process surrogate gives callers child semantics; §18 ran a five-tool script through it |
| The container is single-process (study §4.4, Appendix A; surrogate source comment) | An earlier real `CreateProcess` was denied | §54: `cmd.exe` and an unsigned exe ran as real children with pipes |
| No desktop window tree is a hard blocker (study §0) | win32k lockdown in sandboxed processes | §15: a synthetic window is enough for a D3D game; real windows were never needed |
| Every import is resolved through one dispatch table; ship or reuse a CRT (study §4.1–§4.3) | A hand-mapped exe gives the loader every import slot | The CRTs are preloaded by the host and shared, and call kernel32 through their own tables; Go resolves everything at run time. Three channels (§12.6), later four |
| D3D11 is the lowest-risk path (Appendix B) | Native in UWP; what emulator ports use | §13.1: native D3D11 in the container is feature level 10_1; D3D11On12 is used instead |
| Memory caps lift under the debugger (study §3.1, §6.2) | Microsoft's resource-allocation page | §33 Stage 0: Game mode keeps 5,120 MB under a debug launch |
| x86 hardware compatibility mode is useless (§30.1, superseding study §3.7) | The FS base cannot be pointed at a 32-bit TEB | §58: a `gs:` alias reaches the TEB; native compatibility mode ships |
| `longjmp` back to the loader is a safe exit | It worked for every C and C++ guest | §12.6: Go's runtime frames cannot be unwound; exit now ends the thread |
| curl's failed launch was the unrestored `_wfreopen` | The redirect was the last thing changed | §12.3: the command-line hook restore re-applied the hook (kernel32 and kernelbase share the address) |
| sqlite3's teardown fail-fast needs deep work to locate (§12.3) | The fail-fast path bypasses VEH and SEH | §12.6: ending the thread before teardown removed it |
| A fixed TLS index with our own array is enough (§12.5) | ripgrep, curl and the others passed | ntdll replaces every thread's array when a TLS module loads; real donor indices from 0.0.387.0 |
| The surrogate's limits are all easily liftable (§17.1) | They were scoped as MVP shortcuts | §29.7: stdin cannot reach a child that shares the host's msvcrt |

## What shipped

Verified in the tree at the time of writing:

| Piece | Where |
|---|---|
| Spike host (RW to RX, result 42) | `src/host/App.cpp` |
| Hand mapper for the exe | `MapAndRun`, `CallGuestEntry`, `RtlAddFunctionTable` on the exception directory |
| Selective IAT, stubs for the missing | `ResolveImport`, `ShimTable()`, `MakeStub`, `MissingStubLog` |
| Channel 2 (CRT and guest-DLL import tables) | `PatchModuleForGuest`, `IsGuestOwnedModule` |
| Channel 3 (run-time lookups) | `Bed_GetProcAddress` |
| Guest DLLs from the guest folder | `LoadGuestDll` |
| Command line for every CRT | `BuildGuestArgv`, the `_configure_wide_argv` hook, `PatchPebCommandLine`, `OverwriteKernel32Cmdline`, `InlineHook` |
| Exit without unwinding | `GhGuestExit`, `FinishGuestSession`, `FlushGuestCrt`, `Bed_ExitProcess` |
| Last-error transparency | `Diag` saves and restores `GetLastError` |
| Static TLS | `TryRealGuestTls` and `corpus/shims/ghtls.cpp`; legacy `SetupGuestTls` / `InstallGuestTlsForThread` behind `gh_no_real_tls`; `GuestThreadStub` |
| Console beds | `Bed_SetConsoleCtrlHandler` and neighbours, `InitStdHandles` |
| CA bundle | `cacert.pem` in the package, `CURL_CA_BUNDLE` and `SSL_CERT_FILE` |
| CreateProcess surrogate | `Bed_CreateProcessW`, `Bed_CreateProcessA`, `MapChildImage`, `ChildThreadProc`, `t_child`, `PatchPebStdHandles`, `Bed_GetExitCodeProcess` |
| Real child processes | `Bed_MelonCreateProcessW` |
| Test guests | `corpus/proc/parent.cpp`, `child.cpp`, `minishell.cpp` |

The CLI tools themselves are not in the repository (`corpus/bin/` is gitignored; see
[overview.md](../architecture/overview.md)). Reference: [pe-loading.md](../architecture/pe-loading.md)
for the mapper, TLS and the surrogate; [api-shims.md](../architecture/api-shims.md) for the
channels and bed rules.

## Still open

- busybox-w32 has never been investigated.
- Surrogate children have no TLS of their own, do not restore the parent's argv, share the
  parent's CRT state, and are limited to the host's command-line length on the PEB and
  kernel32 path. Input to a child that shares msvcrt cannot be delivered.
- The comment above the surrogate in `guest_loader.cpp` still says multi-process is impossible.
  The real-child path contradicts it; the comment predates §54.
- Why the earlier real `CreateProcess` was denied while §54's succeeded is not recorded.
- Executable memory depends on Dev Mode policy. A firmware update could change it; nothing in
  the tree monitors it beyond every launch working.
- Protected titles remain `WONT_RUN`, as the study expected.

## Lessons

- Test a "no" before designing around it. Two of the study's hard blockers fell to a probe or a
  small surrogate, and the single-process assumption stood for five months untested.
- Pick test guests by runtime, not by application. msvcrt, UCRT, static CRT, Rust, Go and
  CPython each exposed a different route to the OS; a second tool on the same runtime exposes
  nothing new.
- A bed in the exe's import table is one of several doors. The CRTs, guest DLLs, run-time
  lookups and direct binders each need covering, or a fix works from the exe and not from the
  plugin.
- Never unwind across guest frames you did not compile. `ExitThread` ends a guest from any stack;
  `longjmp` needs every frame between to be describable.
- Instrumentation must be invisible to the guest. A log write that changes `GetLastError` changes
  which `except` branch a runtime takes.
- Memory the OS will free must come from the allocator the OS frees with. The TEB's TLS array is
  freed with the process heap at thread exit, so it must come from the process heap.
- Hooking two names for one function is hooking it twice. Dedupe hook targets by address before
  saving original bytes.
- Activate the app before the guest runs. Launch status and host state then cannot depend on
  what the guest does.
- A child that shares the host's CRT shares the host's cached state. Redirect before the CRT
  initialises, or do the work in process.
