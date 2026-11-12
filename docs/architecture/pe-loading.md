# PE loading

What this covers: how a guest executable and its DLLs get into the process and become
runnable: mapping, relocations, import binding, DLL loading, static TLS, unwind data,
exception handling order, and the two forms of "child process".
Where the code is: `src/loader/guest_loader.cpp`, banners "PE loader", "Generic trampoline",
"TLS bed", "Self-mapped guest DLLs", "First-chance crash observer", "CreateProcess
surrogate"; the function `MapAndRun`; shim DLL sources in `corpus/shims/`.

Related: [overview.md](overview.md), [api-shims.md](api-shims.md),
[filesystem-registry.md](filesystem-registry.md), [memory.md](memory.md),
[x86-guest.md](x86-guest.md), [engine-notes.md](engine-notes.md),
[../reference/diagnostics.md](../reference/diagnostics.md).

This document is about 64-bit (PE32+) guests. A PE32 image is detected in `MapAndRun` and
handed to `MapAndRun32`; everything after that is in [x86-guest.md](x86-guest.md).

## 1. Why the loader maps the exe itself

The OS loader will not start a second executable inside a UWP process, and the guest's
imports cannot all be satisfied by the container's system DLLs. So the main image is
mapped by hand, which also gives the loader control of every import slot. The guest's
DLLs are a different matter: wherever possible they are loaded by the real OS loader
(`LoadLibraryExW`), because that gives them correct TLS, loader-lock semantics, and
`GetModuleHandle` visibility for free. Hand-mapping a DLL is the fallback (section 6).

| Image | Mapped by | Imports bound by |
|---|---|---|
| Guest `.exe` | `MapAndRun` | `ResolveImport` (beds, real exports, stubs) |
| Guest DLL, normal case | OS loader via `LoadGuestDll` | OS loader, then selected slots re-pointed by `PatchModuleForGuest` |
| Guest DLL the OS loader rejects | `GhSelfMapDll` (uses `MapChildImage`) | `ResolveImport` |
| Child exe from `CreateProcess` | `MapChildImage` | `ResolveImport` |

## 2. `MapAndRun`: mapping the main image

`struct LoadedImage { base, size, entryRva, pdata, pdataCount }` describes a mapped image.
`MapAndRun(std::vector<uint8_t>& file)` takes the whole exe file in memory.

| Step | What happens | Diag line |
|---|---|---|
| Validate | DOS and NT signatures; optional-header magic must be PE32+. A PE32/i386 image goes to `MapAndRun32` when built with `GH_X86_GUEST`, otherwise the launch fails with "not PE32+ ... x64 guests only". | `STAGE=guest_is_pe32` |
| Allocate | `VirtualAlloc` of `SizeOfImage` at the preferred `ImageBase`, read-write. If that address is taken, allocate anywhere. | `STAGE=image_alloc ... preferred` or `RELOCATED` |
| Copy | Headers, then each section's raw data to its virtual address. | |
| Release the file | The raw file buffer is freed immediately; a large exe would otherwise be held twice against the memory cap. | `STAGE=guest_file_released` |
| Relocate | Section 3. | `STAGE=relocated` |
| Bind imports | Section 4. | `STAGE=imports shimmed= real= stubbed=` |
| Patch CRT IATs | `PatchModuleForGuest` on `ucrtbase.dll` and `msvcrt.dll`. | `STAGE=iat_patch <module> x<N>` |
| TLS | Section 7. | `STAGE=tls_*` |
| Protections | Each section gets its final protection from its characteristics. | `STAGE=protections_set` |
| Tracepoints, title patches | Optional instruments are armed now that code is in place. | |
| Unwind data | `RtlAddFunctionTable` on the exception directory. | `STAGE=seh_registered count=` |
| TLS callbacks | `RunGuestTlsCallbacks` with `DLL_PROCESS_ATTACH`. | `STAGE=tls_callbacks ran=` |
| Stack cookie | `GhSeedGuestCookie`, section 9.3. | `GSSEED ...` |
| Entry | `CallGuestEntry` under SEH. | `STAGE=calling_entry`, `STAGE=entry_returned rc=`, `STAGE=guest_seh code=` |

The entry point is the image's CRT startup (`mainCRTStartup` / `WinMainCRTStartup`), so the
guest's own CRT initialises itself, runs its static constructors and calls `main`.
`CallGuestEntry` contains only the SEH frame and the call; functions that use `__try` must
not hold C++ objects that need unwinding (the compiler rejects it), which is why SEH is
confined to small helpers such as `CallGuestEntry`, `GhSehCall`, `SafeQword` and
`GhSafeReadCode`.

## 3. Relocations

If the image landed at its preferred base, the relocation directory is ignored. Otherwise
the loader walks `IMAGE_DIRECTORY_ENTRY_BASERELOC` and applies `IMAGE_REL_BASED_DIR64`
entries (add the 64-bit delta); `ABSOLUTE` padding entries are skipped. An image with
stripped relocations that cannot get its preferred base cannot run.

In practice the preferred base (usually `0x140000000`) is free when the first guest of a
process is mapped. Later sessions in the same process may relocate.

## 4. Import resolution

### 4.1 The three outcomes

Every import slot of the main image ends as one of:

| Outcome | Meaning | Counter |
|---|---|---|
| shimmed | The slot points at a bed (`Bed_*`) from `ShimTable()`. | `shimmed=` |
| real | The slot points at the real export of an OS DLL or a guest DLL. | `real=` |
| stubbed | Nothing provides it. The slot points at a generated stub. | `stubbed=`, one `STUB dll!fn` line each |

This is "selective IAT": the default is the real function, and only names present in the
shim table are replaced. A function the container implements correctly needs no entry.

### 4.2 `ResolveImport(dll, fn)`

Order of lookup for an import by name:

1. Window functions for a guest-embedded Chromium, when that surrogate is active
   (`gh_cefwin.h`).
2. `ShimTable()` by function name only. The table is keyed by name and ignores which DLL
   the import names; the same bed serves `kernel32!X`, `kernelbase!X` and
   `api-ms-win-*!X`. See [api-shims.md](api-shims.md).
3. `_beginthreadex` / `_beginthread` imported from the UCRT: `Bed_beginthreadex` /
   `Bed_beginthread` (they forward, and record the thread id).
4. `LoadGuestDll(dll)` (section 5), then:
   - if the module was self-mapped, `GhSelfMapExport`;
   - else `PinGuestModule` and the real `GetProcAddress`.
5. If the DLL cannot be loaded at all, a small set of built-in providers by DLL name
   (DirectSound's exports, for example).

If all of that fails the caller creates a stub (4.4).

### 4.3 Imports by ordinal

For an ordinal import the loader first asks `GhImportOrdinalName` whether it knows the
name behind the ordinal for that DLL (it knows the ordinal-only exports of `d3d12.dll` and
`xinput1_*.dll` that games actually use). A known name goes through the shim table like
any other. Otherwise the DLL is loaded and the ordinal resolved with `GetProcAddress`;
failing that a stub named `#<ordinal>` is created.

### 4.4 Unresolved imports: the stub trampoline

`MakeStub(name)` emits a small thunk into a 64 KB page: load the address of the import's
name string, jump to `MissingStubLog`. That function logs `STUB-CALLED dll!fn` once per
import and returns 0. `FinalizeStubs` makes the page executable; a full or finalised page
is left alone and a new one started, because DLLs self-mapped later need stubs too.

What this buys:

- A game that imports 30 functions it never calls still starts.
- A game that does call one is diagnosed by name instead of by a fault at address 0.

Limits of a stub:

- It returns 0 and writes no out-parameters. For many APIs 0 means success, so a caller
  may go on to read uninitialised memory. When that matters, write a real bed that returns
  the API's honest failure code and fills every out-parameter (`corpus/shims/tbs_stub.c`
  is the model).
- The launcher treats any `STUB-CALLED` line as a failed launch and shows "guest called a
  stubbed import".

### 4.5 Imports of other modules: `PatchModuleImport` and `PatchModuleForGuest`

Binding the exe's IAT is not enough. A DLL the OS loader mapped has its own import table,
bound to the real functions. `fopen` inside `ucrtbase.dll` calls the real `CreateFileW`,
not the bed the exe was given.

- `PatchModuleImport(mod, target, repl)` walks one loaded module's import descriptors by
  name, and overwrites the matching IAT slot (with `VirtualProtect` around the write).
  Ordinal imports are skipped.
- `PatchModuleForGuest(h, tag)` applies a fixed list (`redirs[]`) of such replacements:
  vectored-handler registration, command line, file and directory APIs, named pipes,
  registry, known-folder and shell APIs, standard handles, display enumeration.

It is applied to:

| Module | When |
|---|---|
| `ucrtbase.dll`, `msvcrt.dll` | In `MapAndRun`, after the exe's imports are bound. |
| Every DLL loaded through `LoadGuestDll` | Right after it loads. |
| Static imports of those DLLs that live in the same folder | `GhPatchSiblingImports` (logged `SIBLING import ...`), up to four levels deep. |

It is deliberately not applied to arbitrary system DLLs: only to the CRTs and to modules
that live in the guest's directory (`IsGuestOwnedModule`). Patching system DLLs changes the
behaviour of the host's own WinRT code.

The list in `PatchModuleForGuest` is shorter than `ShimTable()`. A bed that must also apply
to calls made from inside guest DLLs has to be added to both.

## 5. `LoadGuestDll`: loading a guest's DLL

Every path by which a guest asks for a DLL ends here: static imports (`ResolveImport`),
`Bed_LoadLibraryA/W/ExA/ExW`, and the loader's own preloads. `Bed_LoadLibraryExA` matters
more than it looks: MSVC's delay-load helper calls it, and when it fails the helper raises
exception `0xC06D007E`, which the crash observer decodes as `DELAYLOAD FAILED <dll>`.

### 5.1 Order

| # | Check | Notes |
|---|---|---|
| 1 | Deny list | Basenames in `LocalState\ghdenydll.txt` are refused (`DENYDLL`). A diagnostic tool. |
| 2 | Path clean-up | Some guests build a doubled absolute path; it is un-doubled. |
| 3 | Already loaded | `GetModuleHandleExA`; takes a reference and returns. |
| 4 | Already self-mapped | `GhSelfMapFind`. |
| 5 | Steam API singleton | Any `steam_api64.dll` by basename resolves to the one preloaded shim. Documented with the Steam shim. |
| 6 | Aliases | Section 5.3. |
| 7 | Resolve a path | Absolute path as given; else `<guest dir>\<name>`; else the directory the guest passed to `SetDllDirectoryW` (packaged apps ignore the real call, so `Bed_SetDllDirectoryW` records it). |
| 8 | Overlays | A mod profile or the flat `LocalState\ovl\<basename>` can shadow the file. |
| 9 | Package fallbacks | If the game does not ship the DLL: `<install>\<basename>`, then the package's VC++ runtime folder. Logged `GUESTDLL ... not shipped by the game -- using the package's`. |
| 10 | Load attempts | `LoadLibraryExW` with `LOAD_WITH_ALTERED_SEARCH_PATH`; then with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`; then `LoadLibraryW`; then the bare name. |
| 11 | Self-map fallback | If the file exists and every attempt failed (knob `gh_selfmap`, default on). |
| 12 | After load | `RepublishGuestTls`, `PatchModuleForGuest`, `GhPatchSiblingImports`, engine-specific boot hooks. |

Diag lines: `GUESTDLL attempts ... altered= searchdirs= plain= altered_ms= total_ms=`,
`GUESTDLL loaded <path>` or `GUESTDLL FAILED <name> err= ms=`, `GUESTDLL self-mapped`.

### 5.2 `ReportWhyDllFailed`

The OS reports a failed load as one error code, usually `ERROR_MOD_NOT_FOUND` (0x7E) for
"some dependency, somewhere" or `ERROR_PROC_NOT_FOUND` (0x7F) for "some export". After a
failure the loader reads the DLL's bytes itself, walks its import table and tests each
dependency and each imported name against what the process can actually provide. The
result is a list of `  WHY ...` lines naming the missing DLL or the missing export. This
is the first thing to read when a DLL will not load.

### 5.3 DLL aliases are session-only

In launcher builds some DLL names that do not exist on the console are answered with a
different implementation:

| Requested | Served by |
|---|---|
| `d3d9.dll`, `d3dx9_*.dll` | The loader's D3D9 runtime over the console's `d3d9on12.dll` (`gh_d3d9.h`). |
| `dsound.dll` | DirectSound over XAudio2 (`gh_dsound.h`), behind the knob `gh_dsound`. |
| `mfplay.dll` | `mfplat.dll` (the import only needs to bind; see [audio-media.md](audio-media.md)). |

These aliases apply only while `g_guestSessionLive` is true. The launcher process itself
loads system components that probe for some of the same names; answering those probes
with a guest-facing surrogate broke the launcher's own startup. Any new alias must have
the same gate.

### 5.4 Search-order facts

- The current directory must be the game's directory from the guest's point of view.
  Engines open data files by relative path and locate plugins relative to the cwd. The
  loader keeps a virtual cwd (`VCWD=` in the diag) for the file beds and sets the real cwd
  to a writable folder. See [filesystem-registry.md](filesystem-registry.md).
- `LOAD_LIBRARY_SEARCH_*` flags are unreliable in the container. Preloading a support DLL
  by full path registers its bare name, after which imports of that name bind to it. This
  is how `ghwmm.dll`, the `opengl32` stub and the Steam shim are put in place before the
  guest is mapped.
- The game directory comes first. A DLL in the game folder shadows the package's copy of
  the same name. This is correct for a game that ships its own VC++ runtime, and a trap
  during development: a stale replacement CRT left in a game folder silently wins over a
  newer one in the package. The 32-bit track has the same order
  (`src/loader/x86/gh_x86_win.h: resolve_dll_path` searches the game folders first and the
  package's `guest\x86\` last).
- `GetModuleFileNameW(NULL)` and `GetModuleHandle(NULL)` are bedded to describe the guest
  exe, not the host exe (`Bed_GetModuleFileNameW`, `Bed_GetModuleHandleW`). Engines find
  their data folder from the exe path.

### 5.5 Static-import pinning

`PinGuestModule(m, dll)` calls `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN)` for
every DLL that a static import bound to (one `GUESTDLL pinned` line per module). A
hand-mapped image holds raw pointers into those DLLs and the OS loader does not know it;
if the guest (or a plugin) balanced its own `LoadLibrary`/`FreeLibrary` calls, the
reference count could reach zero and unmap code the exe's IAT still points to.

DLLs loaded at run time by the guest's own `LoadLibrary` are not pinned; the guest owns
that lifetime. `FreeLibrary` is not bedded.

### 5.6 On-disk import renames

A few engine DLLs import functions the container's slim `user32.dll` / `winmm.dll` do not
export, so the OS loader refuses the whole DLL. Because the OS loader binds those imports,
a bed cannot help. `PatchGuestTreeForContainer` (and the equivalent offline tool
`toolchain/patch_guest.py`) rewrites the import name string in the file to a same-length
name that does exist, and `PatchModuleForGuest` then re-points that slot at the real bed:

| File | Import as shipped | Renamed to | Then patched to |
|---|---|---|---|
| `UnityPlayer.dll` | `MapVirtualKeyExA` | `CharUpperW` | `Bed_MapVirtualKeyExA` |
| `UnityPlayer.dll` | `GetRawInputBuffer` | `CharNextW` | `Bed_GetRawInputBuffer` |
| `UnityPlayer.dll` | `AdjustWindowRectExForDpi` | `CharLowerW` | `Bed_AdjustWindowRectExForDpi` |
| `UnityPlayer.dll` | `IsMouseInPointerEnabled` | `CharPrevW` | `Bed_IsMouseInPointerEnabled` |
| `mono-2.0-bdwgc.dll` | DLL name `WINMM.dll` | `ghwmm.dll` | (binds to the shim DLL directly) |

This modifies the installed copy of the game's files. It logs `STAGE=guest_import_patch`.

## 6. Self-mapped DLLs

Some DLLs exist and have all their imports, yet the OS loader fails them with
`ERROR_DLL_INIT_FAILED`: their `DllMain` (typically an old statically linked CRT) fails
inside the container. For these `LoadGuestDll` falls back to mapping the file itself.

- `struct GhSelfMod { base, name, path, size }`, kept in `g_selfMods` under a lock.
- `GhSelfMapDll(full, dll, preMain)` maps with `MapChildImage`: allocate anywhere,
  relocate, bind imports through `ResolveImport` (stubs logged `SELFMAP STUB`), set
  protections, register unwind data. Then it calls `DllMain(base, DLL_PROCESS_ATTACH, 1)`
  under SEH and logs `SELFMAP <dll> DllMain -> TRUE|FALSE`. A `FALSE` drops the module
  unless `gh_selfmap_force=1`.
- `GhSelfMapFind` and `GhSelfMapExport` make the module findable by name and resolve its
  exports (by walking its export directory; ordinals and forwarders are handled). `Bed_GetProcAddress`,
  `Bed_GetModuleHandle*` and `ResolveImport` all consult them, because the OS does not
  know these modules exist.

Consequences to keep in mind:

| Aspect | Self-mapped DLL |
|---|---|
| `GetModuleHandle` / `GetProcAddress` from guest code | Work, through the beds. |
| Same calls from an OS DLL | Do not see the module. |
| Static TLS | Gets its own donor index if one is free (section 7.2); otherwise logged `SELFMAP <dll> has a TLS directory and no index of its own`. Symptom of the latter: function-local statics that appear NULL. |
| TLS callbacks | Run once for process attach. Threads created later get the TLS template but no thread-attach callback. |
| `DllMain` thread notifications | Not delivered. |
| Unload | Never. |

## 7. Static TLS

A hand-mapped image that uses `__declspec(thread)` or `thread_local` has a TLS directory.
Compiled code reads `gs:[0x58]` (the thread's TLS array), indexes it with the image's
`_tls_index`, and expects a per-thread copy of the image's TLS template there. The OS
loader normally assigns the index and maintains the array. For a hand-mapped image nobody
does.

### 7.1 Legacy path: a fake index

`SetupGuestTls` originally chose a fixed index (`kGuestTlsIndex = 128`), allocated its own
256-entry array per thread on the process heap (`InstallGuestTlsForThread`), copied the
thread's existing entries into it, and published it at `TEB+0x58`.

It works until ntdll replaces the array. ntdll reallocates every existing thread's TLS
array whenever a module with a TLS directory loads, and the new array contains only the
slots ntdll knows about. The repairs for that:

- `RepublishGuestTls` after every DLL load (repairs the calling thread only);
- a fault repair in the crash observer for other threads (section 9.2, `TLS REPAIR`);
- `GuestThreadStub`, which wraps every guest-created thread to install the block before
  guest code runs.

The array comes from the process heap rather than `VirtualAlloc` because thread shutdown
in ntdll frees it with the heap. This path remains as the fallback and can be forced with
`gh_no_real_tls=1`.

### 7.2 Current path: a real index from a donor DLL

`corpus/shims/ghtls.cpp` builds to `ghtls0.dll` ... `ghtls15.dll`. Each is a DLL whose only
content is a 16 KB `__declspec(thread)` reserve and a TLS callback. Loading one makes the
OS assign a genuine TLS index, allocate a block at that index for every thread, and carry
it through every reallocation. `TryRealGuestTls`:

1. Loads the donor (found by `FindGuestSupportDll`: the guest directory, then
   `<install>\guest\`).
2. Checks capacity (`GhTlsCapacity`) against the guest's template size.
3. Hands the donor the guest's template (`GhTlsSetTemplate`); the donor's callback copies
   it into each new thread's block at `DLL_THREAD_ATTACH`.
4. Seeds threads that already exist by walking them (`GhSeedExistingThreads`), with bounds
   checks. A thread that predates the donor may have no block at that index; this is
   counted (`no_block=`), not fixed.
5. Writes the donor's index into the guest's `_tls_index` (`AddressOfIndex`).

Diag: `STAGE=tls_real_index`, `STAGE=tls_real_seed`, `STAGE=tls_directory ... (real|fake)`,
`STAGE=tls_none` for an image without a TLS directory.

Slot 0 belongs to the main image. Slots 1 to 15 are handed to self-mapped DLLs in load
order by `GhSelfMapTls`. The slot counter does not reset between sessions in one process.
When the real index is in use (`g_tlsReal`), the legacy republish and repair code stands
down. `UnmapGuestImage` clears the donor's template so threads created after the guest is
gone do not copy from freed memory.

### 7.3 TLS callbacks

- Main image: `RunGuestTlsCallbacks(base)` at process attach;
  `RunGuestTlsCallbacksForThread` from `GuestThreadStub` for threads created through
  `Bed_CreateThread` on the legacy path.
- No thread-detach callback is delivered to guest images. The comment in `GuestThreadStub`
  explains the choice: a thread repaired lazily could never get a matching detach, and a
  small leak is preferred to a half-kept contract.
- The loader's own executable has a TLS callback (`GhTlsCallback`) used to deliver
  thread-detach to one managed runtime that depends on it; see section 10 on FLS.

## 8. Unwind data and SEH

x64 exception handling is table-driven. `MapAndRun` registers the image's `.pdata` with
`RtlAddFunctionTable`; without it, any exception passing through a guest frame (including
every C++ `throw`) terminates the process because the unwinder cannot find the frame. The
table is stored in `g_guestFnTable` and removed with `RtlDeleteFunctionTable` in
`UnmapGuestImage`. `MapChildImage` does the same for self-mapped DLLs and child images.

Code generated at run time (managed JITs, script VMs) registers its own tables through the
real `RtlAddFunctionTable` / `RtlInstallFunctionTableCallback`, which work normally.

## 9. Exceptions: the first-chance crash observer

### 9.1 Installation and ordering

`RunGuest` installs `CrashObserverVeh` as a first vectored handler before any guest code
runs. It is not only a crash reporter: several mechanisms the guest depends on are
implemented as fault handlers and live here.

`CrashObserverVeh` itself: ignores faults raised in 32-bit compatibility mode (they belong
to the x86 runtime); parks threads of a game that has already exited (`PARKED tid=`);
guards against re-entry (`VEH REENTRY`). The body then tries, in order:

| Order | Handler | Returns "continue execution" when |
|---|---|---|
| 1 | Main-thread exception census, `/GS` failure report | never (logging only; `MAINEXC`, `GSFAIL`) |
| 2 | Demand-commit pager, arena view remap, other loader-owned ranges | the fault was in a range the loader manages ([memory.md](memory.md)) |
| 3 | Floating-point trap instrument, watchpoints | the trap was one of ours |
| 4 | C++ throw logging | never (`CXXTHROW`) |
| 5 | Strict-handle fix | code `0xC0000008`, continuable ([api-shims.md](api-shims.md)) |
| 6 | TLS repair | the faulting instruction is a TLS array read (9.2) |
| 7 | Display-driver heap repairs | a known driver commit or mapping fault (`COMMIT REPAIR`, `MAP REPAIR`) |
| 8 | Fault dump | never: logs `VEH fault`, registers, return addresses, `VEHWALK`, and returns "continue search" |

After step 8 the guest's own handlers run. A fault whose instruction pointer is outside
every image is assumed to be JIT-compiled code and is left to the guest's handler
(managed runtimes turn access violations into `NullReferenceException`). In launcher
builds, an unhandled fault on the guest worker thread in image code ends the session and
shows the crash screen instead of killing the app.

Guest vectored handlers. `Bed_AddVectoredExceptionHandler` records every handler the
guest registers (so exit can remove them) and logs `GUESTVEH`. When a guest registers a
"first" handler while the pager is active, the loader re-registers its own handler ahead
of it (knob `gh_veh_front`, default on). The rule: no guest handler may run ahead of the
pager, because a guest handler that sees a pager fault treats it as a real crash.

### 9.2 VEH TLS repair

On the legacy TLS path, a thread whose TLS array was replaced by ntdll faults on its next
`thread_local` access: it reads a NULL (or short) entry and dereferences it. The repair
decodes the faulting instruction's ModRM/SIB bytes, requires that exactly one of the base
or index registers is zero, installs the thread's block, writes the correct pointer into
that register, and resumes at the same instruction. It never rewinds the instruction
pointer to re-execute the earlier load: the compiler may have hoisted it arbitrarily far.
Logged `TLS REPAIR`.

### 9.3 Stack cookie seeding

An MSVC image's `/GS` cookie holds a well-known default constant on disk, and the CRT's
`__security_init_cookie` replaces it with a random value early in startup. An entry point
that is itself `/GS`-protected stores a cookie derived from the default in its own frame
before the CRT randomises the global. From then on that bottom frame disagrees with the
live cookie. Nothing notices until an exception is dispatched all the way down the main
thread's stack: the cookie check on the entry frame fails, the failure report raises a
non-continuable `0xC0000409`, which is dispatched down the same stack and fails again,
until the stack is exhausted.

`GhSeedGuestCookie` finds the cookie's address in the PE load-config directory and plants
a non-default value before calling the entry point. `__security_init_cookie` only
recomputes when it still finds the default, so the planted value is the one the whole
process keeps, entry frame included. Knob `gh_gs_seed` (default 1); the `GSSEED` line
records what was done. A cookie failure later is reported as `GSFAIL` with a per-frame
walk (`GSWALK`); `gh_gs_ignore` exists as a diagnostic only.

### 9.4 C++ exceptions

A C++ `throw` is SEH exception `0xE06D7363`. The observer logs each distinct throw site
(`CXXTHROW`) with the type name read from the throw-info structure. Facts that matter when
reading these:

- The first exception parameter is a magic number with four known values (`0x19930520`,
  `0x19930521`, `0x19930522`, and `0x01994000` for pure/managed code). A decoder that
  accepts only the first reports `UNDECODED` for valid throws.
- For images built with relative throw-info (all x64), the type descriptors are RVAs from
  the image base given in the fourth parameter. The type name can be read statically from
  the image; it does not require the exception object to be valid.
- Engines throw routinely during normal operation. A `CXXTHROW` line is not an error.

## 10. Container facts that affect loading

| Fact | Effect on the loader |
|---|---|
| FLS callbacks never run in the container. | A runtime that relies on an FLS callback for per-thread teardown never gets it. The loader's `GhTlsCallback` (a real TLS callback in the host exe) provides `DLL_THREAD_DETACH` for the one runtime that needs it. |
| `SetDllDirectory` and `AddDllDirectory` are ignored for packaged apps. | The bed records the directory and `LoadGuestDll` searches it. |
| Several system DLLs are absent (`d3d9.dll`, `dinput8.dll`, `xinput1_3.dll`, `opengl32.dll`, `tbs.dll`, D3DX). | Aliases (5.3), stub DLLs (11), or built-in providers. |
| `user32.dll` and `winmm.dll` are slimmed down. | On-disk renames (5.6) and `ghwmm.dll`. |
| `GetModuleFileNameW` on a hand-mapped base returns 0. | Module beds answer for the guest image and self-mapped DLLs. |
| No second process image of the same package can be started for an arbitrary exe. | Section 12. |

## 11. Support DLLs

| Source | Built as | Purpose |
|---|---|---|
| `corpus/shims/ghtls.cpp` | `ghtls0.dll` ... `ghtls15.dll` | TLS index donors (7.2). Exports `GhTlsIndex`, `GhTlsCapacity`, `GhTlsReserveOffset`, `GhTlsSetTemplate`, `GhTlsSeedBlock`, `GhTlsSeedCount`. |
| `corpus/shims/ghwmm.cpp` | `ghwmm.dll` | `timeSetEvent` / `timeKillEvent` implemented on the kernel timer queue, plus `timeGetDevCaps`, `timeBeginPeriod`, `timeEndPeriod`. Reached by an on-disk import rename. |
| `corpus/shims/opengl32_stub.cpp` | `opengl32.dll` | Six `wgl*` exports that all report failure, so an engine's OpenGL probe fails cleanly and it selects Direct3D. |
| `corpus/shims/tbs_stub.c` | `tbs.dll` | TPM Base Services stand-in. Every entry returns "no TPM" and writes every out-parameter. |
| `corpus/shims/steam_api64.cpp` | `steam_api64.dll` | Steamworks API shim. Documented elsewhere. |
| `corpus/redist/vcrt`, `vcrt14` | (binary) | VC++ runtime DLLs used when a game does not ship its own. |
| `corpus/redist/dxc` | (binary) | Shader compiler; needs a newer `msvcp140.dll` than older games ship, so it carries its own. |
| `corpus/x86sys/msvcrt`, `ucrt` | 32-bit DLLs | CRT replacements for 32-bit guests ([x86-guest.md](x86-guest.md)). |

## 12. Child processes

### 12.1 The CreateProcess surrogate

`Bed_CreateProcessW` / `Bed_CreateProcessA` run the child image inside the same process:

1. Refuse helpers that only make sense out of process: crash handlers (`CP: refusing
   crash handler`), Chromium sub-process types (` --type=`), a store client's web helper.
2. Read `<guest dir>\<exe>`, map it with `MapChildImage`.
3. Replace the visible command line with the child's and point the PEB's standard handles
   at the handles from `STARTUPINFO`.
4. Run the entry point on a new thread (`ChildThreadProc`). The returned `hProcess` is the
   thread handle, so `WaitForSingleObject` and `Bed_GetExitCodeProcess` behave.
5. The child's `ExitProcess` becomes `ExitThread` (the exit beds check a per-thread child
   context).

It is enough for the test guests in `corpus/proc/` and for shell scripts in the busybox
test guest. Its limits are real: parent and child share one heap, one CRT, one set of
globals in shared DLLs, one command line (it is not restored for the parent), and a child
crash is a parent crash.

### 12.2 Real child processes

The container does allow real child processes: the real `CreateProcessW` can succeed. One code path uses it today, for a
managed-code tool that an engine mod loader runs as a separate program
(`Bed_MelonCreateProcessW`). Notes from that work:

- The loader's standard handles are non-inheritable sinks; they must be nulled in
  `STARTUPINFO` or the child fails to start.
- A real child is a normal Windows process with none of the beds. It sees the container's
  raw API surface.
- A comment above the surrogate in the source still says multi-process is impossible in
  the container. That comment predates this finding; the code that calls the real API is
  the authority.

## 13. Checklist: "the guest dies before its first line of code"

| Symptom in the diag | Look at |
|---|---|
| No `STAGE=guest_read` | Exe not found. Check `guest source =` and the four search locations. |
| `not PE32+` | 32-bit exe in a build without `GH_X86_GUEST`. |
| `GUESTDLL FAILED` + `WHY` lines | A dependency or export the container lacks. Decide: package fallback, alias, stub DLL, or on-disk rename. |
| `stubbed=` is large | Normal for big engines. Only `STUB-CALLED` lines matter. |
| `STAGE=calling_entry` then `STAGE=guest_seh` at once | Read the `VEH fault` block before it; check `STAGE=tls_*` and `GSSEED`. |
| `DELAYLOAD FAILED` | A delay-loaded DLL; same treatment as `GUESTDLL FAILED`. |
| `SELFMAP ... DllMain -> FALSE` | The DLL's own init refuses the container; see section 6. |
