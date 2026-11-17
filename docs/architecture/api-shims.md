# API shims ("beds")

What this covers: how a Windows API call made by a guest is intercepted and answered, how to
add a new shim, the logging conventions, the thread/COM and system-information shims, how
crash reporters are neutralised, and the limits of the UWP app container that the shims
exist to hide.
Where the code is: `src/loader/guest_loader.cpp` (`ShimTable`, `PatchModuleForGuest`,
`PatchModuleImport`, `Bed_GetProcAddress`, `InstallOneHook`, `Diag`); 32-bit equivalents in
`src/loader/x86/gh_x86_beds.h`.

Related: [overview.md](overview.md), [pe-loading.md](pe-loading.md),
[filesystem-registry.md](filesystem-registry.md), [window-input.md](window-input.md),
[audio-media.md](audio-media.md), [memory.md](memory.md),
[../reference/knobs.md](../reference/knobs.md),
[../reference/diagnostics.md](../reference/diagnostics.md),
[../reference/known-limits.md](../reference/known-limits.md).
How it was found: [feasibility-and-first-loader.md](../writeups/feasibility-and-first-loader.md) · [a-freeze-that-was-a-crash-dump.md](../writeups/a-freeze-that-was-a-crash-dump.md)

## 1. Model

A guest reaches the operating system through function pointers. Greenhouse replaces a
selected set of those pointers with its own functions. A replacement is called a bed and is
named `Bed_<ApiName>` (`Bed_CreateFileW`, `Bed_GetSystemInfo`). Everything not replaced
goes to the real function in the container's own system DLLs.

The design rule is "shim as little as possible": a bed exists because the container's
version of the API is missing, fails, lies, or needs to see a different world (paths,
window, display) than the host process does.

A bed has the exact signature and calling convention of the API it replaces. Typical
shapes:

| Shape | Example | Behaviour |
|---|---|---|
| Translate and forward | `Bed_CreateFileW` | Map the guest's path to a real one, call the real API. |
| Forward and correct | `Bed_GetSystemInfo` | Call the real API, adjust the answer. |
| Synthesise | `Bed_EnumDisplaySettingsW`, `Bed_CreateWindowExW` | No real call; answer from loader state. |
| Refuse honestly | `Bed_crs_start_handler` | Return the API's documented failure so the caller takes its fallback path. |
| Observe | `Bed_SetThreadAffinityMask` | Forward, log, and hide a refusal the guest would treat as fatal. |

## 2. The four interception channels

A function pointer can reach guest code in four ways. A shim that covers only one of them
is a common source of "it works from the exe but not from the plugin".

| # | Channel | Who binds it | Where the bed is installed |
|---|---|---|---|
| 1 | The guest exe's import table | `MapAndRun` via `ResolveImport` | `ShimTable()` |
| 2 | Import tables of DLLs loaded by the OS loader (the CRTs, the game's DLLs) | The OS loader, then `PatchModuleForGuest` | `redirs[]` in `PatchModuleForGuest`, or a targeted `PatchModuleImport` call |
| 3 | `GetProcAddress` at run time | `Bed_GetProcAddress` | `ShimTable()` (looked up by name) |
| 4 | Callers that bind the real export directly (managed P/Invoke, system DLLs calling each other) | nobody | An inline hook on the real export, `InstallOneHook` |

### 2.1 Channel 1 and 3: `ShimTable()`

`ShimTable()` returns a `std::unordered_map<std::string, void*>` from API name to bed. It
is keyed by function name only, not by DLL: an import of `GetSystemInfo` gets the bed
whether the guest names `kernel32.dll`, `kernelbase.dll` or an API-set DLL. This is
deliberate; the same function is exported under several DLL names and guests are
inconsistent about which they import.

The cost of name-only keys: two different DLLs exporting different functions under one
name cannot be told apart. So far that has only mattered for generic names in guest-private
DLLs, which is why the table holds Win32/CRT/DirectX names and nothing else.

`ResolveImport` consults the table first for every named import of the exe and of every
hand-mapped image (self-mapped DLLs, surrogate child processes). `Bed_GetProcAddress`
consults it too and logs `GPA shim <name>` when it substitutes a bed, so a guest that
resolves an API dynamically gets the same answer as one that imports it.

`Bed_GetProcAddress` lookup order, for reference: mod-loader hooks on specific runtime
exports; store-client handling; window functions for an embedded Chromium; `ShimTable()`;
ordinals with a known name; exports of self-mapped DLLs; exports of the guest exe itself;
DirectSound alias; finally the real `GetProcAddress`.

### 2.2 Channel 2: `PatchModuleImport` and `PatchModuleForGuest`

A DLL the OS loader mapped already has its imports bound to real functions.
`PatchModuleImport(mod, "ApiName", bed)` rewrites the matching import slot of one module.
`PatchModuleForGuest(h, tag)` applies a fixed list of replacements and logs
`STAGE=iat_patch <tag> x<count>`. It runs on `ucrtbase.dll`, `msvcrt.dll`, every DLL
loaded through `LoadGuestDll`, and those DLLs' same-directory static imports. See
[pe-loading.md](pe-loading.md) section 4.5.

Two things to know:

- The list is a subset of `ShimTable()`: path, registry, command line, shell folders,
  display enumeration, vectored handlers. A new bed that guest DLLs must also see needs an
  entry here as well.
- Some replacements must not be applied to every module. `PatchModuleForGuest` has a
  second list applied only when `tag` names the Unity player DLL (cursor and pointer
  functions). Applying those to a store client DLL crashed it. When in doubt, patch one
  module by name with `PatchModuleImport` instead of adding to the shared list.

### 2.3 Channel 4: inline hooks on real exports

Some callers never go through an import table the loader controls. A managed runtime
resolves P/Invoke targets with its own symbol lookup and calls the real `kernel32` export;
`kernel32` itself calls `ntdll`. For those the loader patches the first bytes of the real
function.

- `InstallInlineHook(target, hook, &trampoline)` copies whole instructions from the
  function's prologue until it has at least 14 bytes (`X64PrologueInsnLen` is a small
  length decoder that refuses anything it does not know, and any RIP-relative
  instruction), writes an absolute `jmp [rip+0]` to the hook, and builds a trampoline of
  the stolen bytes plus a jump back.
- `InstallOneHook("Name", hook, &trampoline)` finds the export in `kernelbase.dll`, falls
  back to `kernel32.dll`, hooks it, and also hooks the `kernel32` copy when that is a
  separate stub rather than a forwarder. It logs `HOOK <Name> installed @ <addr>` or
  `HOOK <Name>: install FAILED (prologue)`.

Current trampoline hooks: `CreateFileW`, `CreateFile2`, `NtCreateFile`, `NtOpenFile`,
`MoveFileExW`, `DeleteFileW` (path redirects for writes that would land on a read-only
drive root, and hiding files; see [filesystem-registry.md](filesystem-registry.md)) and
`GetFileType` (section 7.2). Memory and black-box instruments install a few more on
`ntdll` exports ([memory.md](memory.md), [../reference/diagnostics.md](../reference/diagnostics.md)).

There is also a second, simpler kind: `InlineHook(target, replacement)` overwrites the
first 12 bytes with an absolute jump and keeps no trampoline (the original is never
called). `HookCommandLine` uses it to replace the real `GetCommandLineA/W` exports for the
duration of a session, so callers that bypass every import table still see the guest's
command line. The overwritten bytes are saved and `RestoreHooks` (from
`FinishGuestSession`) puts them back; the host's own WinRT activation fails with
`E_INVALIDARG` if it sees a guest command line.

Rules for inline hooks:

- They affect every caller in the process, including the host's own WinRT code and the
  loader. A hook must pass through untouched everything it does not specifically own. The
  file hooks only act on a narrow, recognisable path shape.
- A hook runs inside arbitrary callers' lock contexts. It must not log unboundedly (the
  log itself calls `CreateFileW`), allocate large amounts, or call back into the hooked
  API except through the trampoline.
- Trampoline hooks stay installed for the life of the process; write them so that is safe
  when no guest is running.
- Prefer channels 1 to 3. Use a hook only when a caller demonstrably bypasses them.

## 3. Adding a shim, end to end

Example: a guest fails because `FooBarW` returns an error in the container.

1. Find out which channel the call uses. Search the diag for `STUB FooBarW` (imported,
   nothing provides it), `STUB-CALLED` (called), `GPA` lines (resolved dynamically), or
   use a tracepoint ([../reference/diagnostics.md](../reference/diagnostics.md)).
2. Write the bed in `src/loader/guest_loader.cpp`, in the banner region for its subject
   (file APIs with the VFS bed, window APIs with the windowing bed, and so on), above
   `ShimTable()`:

   ```cpp
   static BOOL WINAPI Bed_FooBarW(LPCWSTR name, DWORD* out)
   {
       static LONG once = 0;
       if (!InterlockedExchange(&once, 1)) Diag("BED FooBarW first call");
       if (out) *out = 0;                      // write every out-parameter
       SetLastError(ERROR_NOT_SUPPORTED);      // the API's honest failure
       return FALSE;
   }
   ```

3. Add `{"FooBarW", (void*)&Bed_FooBarW},` to the initialiser in `ShimTable()`. If the
   bed is defined later in the file than the table, forward-declare it above the table
   (several existing beds do this).
4. If guest DLLs or the CRT call it, add the same pair to `redirs[]` in
   `PatchModuleForGuest`.
5. If the 32-bit track needs it, add the equivalent to `src/loader/x86/gh_x86_beds.h`
   ([x86-guest.md](x86-guest.md)); the two tables are separate.
6. Build the slot, run, and confirm in the diag: the `stubbed=` count drops or the
   `BED FooBarW` line appears, and no `STUB-CALLED` remains.

Guidelines learned the hard way:

| Guideline | Reason |
|---|---|
| Match the real signature exactly, including `WINAPI` / `__cdecl`. | A wrong argument count corrupts nothing on x64 but returns garbage; a wrong return width does corrupt. |
| Write every out-parameter on every path. | Callers read them without checking the return value more often than expected. |
| Return the real API's documented failure, not 0. | 0 is "success" for `HRESULT`, `LSTATUS`, `NTSTATUS` and many handle-less APIs. |
| Set last-error the way the real API does. | Runtimes map last-error to exception types; the wrong code takes the wrong `catch`. |
| Keep per-call work small; log with a counter. | Some beds are called millions of times per second. |
| Do not hold a lock across a call into guest code or a blocking OS call. | The guest's threads are suspended at arbitrary points during exit and by instruments. |
| No guest-specific checks by exe name in a bed. | Put per-title behaviour behind a knob and set its default in the title config. |
| Prefer fixing a class of failure over one caller. | The same container gap usually hits the next engine too. |

## 4. Logging conventions

`Diag(const std::string&)` appends one line to `loader_diag.txt` and to an in-memory ring
that the launcher's crash screen shows.

### 4.1 `Diag` is transparent to last-error

`Diag` saves `GetLastError()` on entry and restores it on exit. This is required, not a
nicety: beds log after calling the real API, and the log's own file open sets
`ERROR_ALREADY_EXISTS` on success. Before this rule a guest runtime read "already exists"
where the real answer was "file not found", took the wrong exception path and died during
start-up. Any helper you write that runs inside a bed and calls Win32 must do the same.

Other properties of `Diag`:

- It is capped. A guest in a retry loop can otherwise write hundreds of megabytes a
  minute. When the cap is reached the head of the log is kept and the end continues into
  two alternating tail files, because the end of a run is where the failure is.
- The log lives in the dev scratch folder when that is available, otherwise in LocalState
  (which has a small quota shared with the game's own files).
- In `GH_RELEASE` builds only the ring is kept; nothing is written to disk.

### 4.2 Line prefixes

Lines start with an upper-case tag so the log can be filtered with a plain text search.

| Prefix | Meaning |
|---|---|
| `STAGE=<name>` | A milestone of the launch or exit sequence. One per event. |
| `BED <Api> ...` | A bed reporting what it did or what the real API answered. Usually once, or counted. |
| `CALL <name>` | A call was observed: the first call of a few sentinel beds (`CALL GetCommandLineW`), and the first three calls of any import wrapped by the call-log thunk (`CALL dll!fn #n tid=`). |
| `GPA ...` | `Bed_GetProcAddress` substituted or traced a lookup. |
| `HOOK ...` | An inline hook was installed or acted. |
| `STUB dll!fn` / `STUB-CALLED dll!fn` | An import nothing provides was bound / was actually called. |
| `GUESTDLL ...`, `WHY ...`, `SELFMAP ...` | DLL loading ([pe-loading.md](pe-loading.md)). |
| `GUESTTHREAD #n ...` | The guest created a thread. |
| `CFG <file> = <value> (<source>)` | First read of a knob, and where the value came from. |
| `VEH ...`, `CXXTHROW`, `TLS REPAIR`, `STRICTHANDLE` | The first-chance exception observer. |

Conventions for new lines:

- Log the first occurrence, or the first N with an `InterlockedIncrement` counter. Never
  log unconditionally on a hot path.
- Log the real answer before overriding it (`BED GetSystemInfo REAL procs=...`). Half of
  the value of a bed is learning what the container actually says.
- Narrow strings only; convert paths with the existing helpers.
- Never log secrets or account identifiers.

### 4.3 Knobs

Most beds with a behavioural choice read a knob: `GhCfgNum(L"gh_name.txt", "GH_NAME",
default)`. The value comes from the environment variable, else the LocalState file, else
the title config's default, else the compiled default. The first read logs a `CFG` line.

`GhCfgNum` has a historical trap: a file containing `0` returns the default, not zero. Use
`GhCfgNumZ` for any knob where zero is a meaningful value. See
[../reference/knobs.md](../reference/knobs.md).

## 5. Threads and COM apartments

### 5.1 Thread creation beds

| Bed | Behaviour |
|---|---|
| `Bed_CreateThread` | Creates the thread (through `GuestThreadStub` when the legacy TLS path is active, so TLS is installed before guest code runs), records the thread id, logs `GUESTTHREAD #n fn=<module+offset> stack= flags= -> tid=`. |
| `Bed_beginthreadex`, `Bed_beginthread` | Forward to the real UCRT function and record the id (`GUESTTHREAD #n crt fn=`). Needed because the CRT calls `CreateThread` from inside `ucrtbase.dll`, past the exe's import table. |
| `Bed_SetThreadAffinityMask`, `Bed_SetThreadIdealProcessor` | Forward and log; report success if the container refuses, because job systems treat the failure as fatal for the worker. |

Why ids are recorded: at guest exit the loader must suspend exactly the guest's threads
before freeing the image ([overview.md](overview.md) section 3.4). Threads are identified
by this list plus heuristics on start addresses; host threads are excluded by a snapshot
taken before launch.

A failed `CreateThread` is logged with its error. A pool that asked for eight workers and
got one looks, from outside, exactly like a hang.

### 5.2 UCRT threads are in the MTA

In a packaged app, a thread started through the UCRT's `_beginthreadex` (which is what
`std::thread` uses) is implicitly a member of the multithreaded apartment. A guest whose
main thread then calls `CoInitializeEx(COINIT_APARTMENTTHREADED)` gets
`RPC_E_CHANGED_MODE` and, typically, exits without a message. On desktop Windows the
guest's main thread is a plain OS thread with no apartment.

Therefore the loader creates the guest worker with `CreateThread`
(`GhStartGuestWorker`), and logs the apartment it observes (`APT worker ...`, `APT entry
...`). Do not move guest entry onto a `std::thread`.

### 5.3 `CoInitialize` is not implemented

The container's `ole32` answers the legacy `CoInitialize(NULL)` with `E_NOTIMPL`; only
`CoInitializeEx` works. Some engines call the legacy form and quit on any failure, before
creating a window. `Bed_CoInitialize` calls `CoInitializeEx(reserved,
COINIT_APARTMENTTHREADED)`, which is the documented meaning of `CoInitialize`, and logs
`BED CoInitialize -> CoInitializeEx(STA) hr=` once.

### 5.4 COM proxies must be single-vtable objects

The loader hands guests its own implementations of several COM interfaces (DXGI factory,
adapter, output, swap chain, and others). These must be laid out the way the real objects
are: one object, one vtable, the most derived interface implemented by plain C++
inheritance, every interface in the inheritance chain answered by `QueryInterface` with
the same pointer, one reference count.

Do not implement them with a helper that gives each interface its own sub-object (C++/WinRT
`winrt::implements` does this). Real DXGI interfaces form a single-inheritance chain, so
code written against the real headers may legally call an `IDXGIAdapter` method through a
pointer it obtained as `IDXGIAdapter1`. With per-interface sub-objects that call lands in
the wrong sub-object with the wrong reference-count offset and corrupts the object's
vtable pointer. The failure appears much later as a jump into nothing.

`AdapterProxy`, `SwapProxy`, `OutputProxy` and `FactoryProxy` in
`src/loader/guest_loader.cpp` follow this rule; the comment above `AdapterProxy` records
the full reasoning. They are also allocated from a bump arena that never reuses memory, so
a third-party interposer that releases one too many times hits a dead object rather than a
live unrelated one.

## 6. Process and system information beds

A game sizes its thread pools, memory pools and quality settings from what the system
reports. Inside the container some of those answers are missing or describe the wrong
machine.

| Bed | What it does | Knob |
|---|---|---|
| `Bed_GetSystemInfo`, `Bed_GetNativeSystemInfo` | Call the real API, log the real processor count and mask once, then optionally override count and mask. | `gh_cpus` (0 = report the real value) |
| `Bed_GetActiveProcessorCount` | Same, for the processor-group API. | `gh_cpus` |
| `Bed_GlobalMemoryStatusEx`, `Bed_GlobalMemoryStatus`, `Bed_GetPhysicallyInstalledSystemMemory` | Log the real answer; optionally cap the reported totals so a game that sizes pools from "installed RAM" sizes them for the app's budget. | see [memory.md](memory.md) |
| `Bed_GetCommandLineW/A` | Return the guest's command line, not the host's. | `ghcmd.txt` |
| `Bed_GetModuleFileNameW`, `Bed_GetModuleHandle*` | Describe the guest exe for `NULL` / the guest's base. | |
| `Bed_GetStdHandle`, `Bed_WriteFile`, console-mode beds | Capture console output of command-line guests. | |
| `Bed_EnumDisplay*`, `Bed_GetMonitorInfo*`, `Bed_MonitorFrom*`, `Bed_GetSystemMetrics`, `Bed_GetDeviceCaps` | One primary monitor at the output size with one mode. The container's `user32` has no desktop and reports no monitors; a game that believes that computes a zero-size window. | [window-input.md](window-input.md) |
| SetupAPI display-adapter beds | SetupAPI enumerates no devices in the container. A synthetic display adapter is built from the DXGI adapter description for games that identify the GPU that way. | [graphics.md](graphics.md) |
| `Bed_SHGetKnownFolderPath`, `Bed_SHGetFolderPathW` | Known folders outside the package are access-denied; both beds share one redirect table into LocalState. | [filesystem-registry.md](filesystem-registry.md) |
| `Bed_ShellExecute*` | Logged and not run. | |

The processor-count bed is a good template for a "forward and correct" shim: it logs
`BED GetSystemInfo REAL procs= activemask= pagesize= allocgran=` before changing anything.

## 7. Container behaviours handled in one place

### 7.1 Crash reporters

Games ship out-of-process crash reporters. They cannot work here (their helper would have
to be a separate process with access to the game's memory) and several abort the game when
their set-up fails. Three mechanisms neutralise them:

| Mechanism | Code | Effect |
|---|---|---|
| Refuse the start call | `Bed_crs_start_handler` (in `ShimTable()`) | One vendor's reporter library exports a "start handler" function that aborts inside the container. The bed returns `false`; the game continues with reporting off. Logged `BED crs_start_handler -> false`. |
| Hide the handler executable | `GhHideCrashpad` | File-existence and open calls for `crashpad_handler.exe` answer "not found" in the file beds and in the inline hooks. The reporting SDK then skips its backend. Logged `CRASHPADHIDE`. Knob `gh_hide_crashpad` (default 1). |
| Refuse to spawn | `Bed_CreateProcessW` | Child images whose name marks them as crash handlers are refused (`CP: refusing crash handler`). |

A related class fix, `GhHideSymbolProbe` (knob `gh_hide_sympath`, default 1), answers "not
found" to read-only probes for debug symbol files. Engines walk their own stacks through
DbgHelp on warnings and hangs; each walk probes many directories per loaded module, none
of which can succeed for a shipped game, and each probe costs real storage I/O on the
console.

Greenhouse's own crash handling is the first-chance observer described in
[pe-loading.md](pe-loading.md) section 9.

### 7.2 Strict handle checks

The app container runs with strict handle checking. A call on a stale or invalid handle
does not return `FALSE` with `ERROR_INVALID_HANDLE` as on desktop; the kernel raises
exception `0xC0000008` (`STATUS_INVALID_HANDLE`). Third-party code that probes handles
(`GetFileType` on a possibly-closed handle is typical) dies with an unhandled exception,
sometimes inside a `DllMain`.

Two layers handle this:

- `Hook_GetFileType` (an inline hook on the real export) calls the original under SEH and
  returns `FILE_TYPE_UNKNOWN` when the raise happens. It is a hook, not a bed, because the
  first offender called it from inside a system-loaded DLL's initialisation.
- The crash observer treats a continuable `0xC0000008` raise with no parameters as "the
  API would have failed" and continues execution; the API then returns its normal failure.
  Logged `STRICTHANDLE #n ... raised at <module+offset>`.

Do not try to validate a handle before using it: every user-mode way of asking "is this
handle valid" goes through the same check and raises too.

### 7.3 Handle probes must not wait

A diagnostic that asks "is this handle signalled?" must not use
`WaitForSingleObject(h, 0)`. That call is not read-only: it acquires an unowned mutex,
consumes an auto-reset event's signal, decrements a semaphore and swallows a timer tick.
A periodic probe built on it wedged the guest it was observing. The loader's handle probe
(`GhPollHandleRequest`, driven by `gh_handles.txt`) uses `NtQueryObject` and the
type-specific `NtQuery*` calls, which only read state.

## 8. Container limits

What the app container does differently from desktop Windows, as far as the loader has
measured. Each row names where it is handled. A fuller list of unsolved limits is in
[../reference/known-limits.md](../reference/known-limits.md).

### 8.1 Missing or reduced APIs

| Area | Container behaviour | Handling |
|---|---|---|
| `user32.dll` | Slim build. No desktop, no real windows, no monitors; several exports absent. | USER32 surrogate ([window-input.md](window-input.md)); on-disk import renames ([pe-loading.md](pe-loading.md) 5.6). |
| `gdi32.dll` | No font engine. | `gh_gdifont.h`. |
| `winmm.dll` | `timeSetEvent` / `timeKillEvent` absent. | `ghwmm.dll`. |
| DirectX runtimes | `d3d11`, `d3d12`, `dxgi`, `d3dcompiler_47`, `xinput1_4`, `xaudio2_9` present. `d3d9`, D3DX, `dinput8`, `xinput1_3`, `dsound`, `opengl32` absent. | Aliases and built-in runtimes ([graphics.md](graphics.md), [audio-media.md](audio-media.md)). |
| COM | `CoInitialize` returns `E_NOTIMPL`. | `Bed_CoInitialize`. |
| SetupAPI | Enumerates nothing. | Synthetic display adapter. |
| Shell folders | Known folders outside the package: access denied. `ShellExecute`: nothing to launch. | Redirect table; logged no-op. |
| Registry | The guest's keys do not exist and most of the hive is not writable. | Virtual hive, persisted ([filesystem-registry.md](filesystem-registry.md)). |
| Process snapshots | `PssCaptureSnapshot` returns `ERROR_NOT_SUPPORTED`. | None needed; affects diagnostics only. |
| TPM | `tbs.dll` absent. | `tbs_stub.c`. |
| DLL search | `SetDllDirectory` / `AddDllDirectory` ignored; `LOAD_LIBRARY_SEARCH_*` unreliable. | `LoadGuestDll` does the search itself. |
| FLS | Fiber-local-storage callbacks never run. | [pe-loading.md](pe-loading.md) section 10. |
| `GetModuleFileNameW` | Returns 0 for hand-mapped images. | Module beds. |

### 8.2 Behavioural differences

| Behaviour | Detail |
|---|---|
| Strict handle checks | Section 7.2. |
| UCRT threads are MTA | Section 5.2. |
| Thread affinity may be refused | Section 5.1. |
| Back button | `BackRequested` leaves the app unless handled; the host swallows it. |
| App lifetime | The OS suspends and may terminate an app that is not visible or not presenting. The `PLM ...` diag lines record each transition. |
| File system | The package install folder is read-only; drive roots are read-only; LocalState is writable but its volume is small (a few GB) and shared. The dev scratch folder is the large writable area. |

### 8.3 Memory

One budget covers the guest image, every CPU allocation and every D3D12 heap. The limit is
on committed memory (about 5 GiB on the consoles measured), not on working set, and the
app cannot raise it. There is no OS pagefile behind the app, so committed memory that the
game never touches still counts in full. Mapped file views have their own much lower
per-process ceiling. Placeholder and `VirtualAlloc2` / `MapViewOfFile3` APIs work.

The loader's answers (reserve-only arenas, demand commit, a user-mode spill pager, memory
status beds) are described in [memory.md](memory.md).

### 8.4 Processes

- A real `CreateProcessW` can succeed in the container, and a real child is an ordinary
  process without any of the beds.
- The general `CreateProcess` bed is an in-process surrogate, because a guest's helper
  executables need the same shims as the guest.
- Helpers that exist only to observe or report on the parent are refused (section 7.1).

Details in [pe-loading.md](pe-loading.md) section 12.

### 8.5 Input devices

- Gamepad: available through `Windows.Gaming.Input`, on the UI thread only. The XInput
  beds serve the polled state.
- Keyboard: a USB keyboard's events reach the CoreWindow.
- Mouse: a hardware USB mouse is not routed to a sideloaded app as relative motion. The
  loader offers pad-to-mouse emulation and a network mouse bridge
  (`toolchain/MOUSE_BRIDGE.md`).

See [window-input.md](window-input.md).

## 9. Reading a failure through the shim layer

| Diag evidence | Likely meaning | Next step |
|---|---|---|
| `STUB-CALLED dll!fn` | The guest called an API nothing implements. | Write a bed (section 3). |
| `GPA` for a name returns NULL, then the guest exits | Dynamic lookup of a missing export. | Bed in `ShimTable()` (channel 3 is covered automatically). |
| An API works from the exe but not from a guest DLL | Channel 2 not covered. | Add to `redirs[]` or patch that module. |
| Same, but the caller is managed code | Channel 4. | Consider an inline hook; keep it narrow. |
| `STRICTHANDLE` lines | A stale handle was used; already handled. | None, unless the count is unbounded. |
| `RPC_E_CHANGED_MODE` in a guest's log | Apartment mismatch. | Check `APT` lines; section 5. |
| Guest computes a 0x0 or 1-pixel window | A display query reached the real `user32`. | Find the unbedded call; add it to both tables. |
| Guest's log shows the wrong error for a failed file call | Something inside a bed clobbered last-error. | Save/restore it, as `Diag` does. |
