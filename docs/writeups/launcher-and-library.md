# From a launcher to a library: downloads, packages and Steam as an add-on

What this covers: how Greenhouse went from a single hard-wired guest to a controller-driven library. It covers the launcher as a UI phase of the same process, sign-in on the TV, a depot downloader written from scratch that runs inside the app container, and then the general design: per-title data, library sources, a signed package repository, add-ons, and Steam moved out of the loader into an add-on. It ends with two Steam-engine bugs that turned out to be ours.
Where the code is: `src/loader/launcher_ui.h` (`LauncherPickGuest`, `LibrarySource`, `kLuiSources`, `LuiEnumerate`, `LuiAddonsLoad`, `LuiAddonSignInScreen`, `GhSteamRuntimeDir`, `kSteamRuntimePin`), `src/loader/gh_json.h`, `src/loader/gh_repo.h` (`FetchIndex`, `FetchFrom`, `CleanOrphans`), `src/loader/gh_inflate.h`, `src/loader/gh_addon_abi.h`, `corpus/downloader/cm_downloader.h` (`ghdl::DownloadApp`, `decode_chunk`, `CmClient::qr_begin`), `corpus/addons/steam/steam_addon.cpp`, `corpus/addons/sample/sample_addon.cpp`, `corpus/shims/gh_steam_engine.h` (`gh_probe_read`), `Bed_GetClientBootstrapMetrics` in `src/loader/guest_loader.cpp`, `toolchain/ghpkg.py`, `toolchain/build-addon.sh`.
Related: [launcher/README.md](../launcher/README.md) · [launcher/library.md](../launcher/library.md) · [packages.md](../packages.md) · [steam/library-and-downloads.md](../steam/library-and-downloads.md) · [steam/sign-in.md](../steam/sign-in.md) · [steam/runtime-fetch.md](../steam/runtime-fetch.md) · [architecture/pe-loading.md](../architecture/pe-loading.md)
Period: 2026-08-15 to 2026-10-19, Launcher 0.0.9.0 to 0.0.697.0. Notebook sections: §29, §34–§42, §48–§49.

## Summary

- The launcher is not a second app. It is a Direct2D/DirectWrite UI phase that runs in `App::Run` before the guest thread is started. It draws over the same swapchain and hands the chosen exe to the same process.
- A Steam depot downloader written from scratch runs inside the UWP container. It speaks to a CM server and resolves app info (PICS), then fetches each manifest and chunk, decrypts with AES-256, decompresses VSZa (zstd), VZ (LZMA) or ZIP, and checks every chunk's SHA-1. Its output was byte-exact on the console. Shelling out to `zstd.exe` / `7za.exe` was a dead end, so the decoders are compiled in.
- Library sources are a table of `LibrarySource` entries. Per-title knowledge is data (`greenhouse.title/1`). Packages come from a repository whose index is signed with ECDSA P-256 and carries a rollback serial.
- Add-ons are plain-C DLLs. They are installed only from that repository and hashed again before each load. Steam's sign-in, library and downloader moved into one such add-on, so the loader keeps no Steam client code. The `GH_NO_STEAM` build flag was retired in favour of a runtime gate.
- Valve's client runtime is never shipped by Greenhouse. The console fetches it from Valve's own CDN, by byte range, pinned by hashes in the signed index and again by a table compiled into the loader.
- Two Steam-engine crashes were our bugs. A `VirtualQuery`-then-`memcpy` probe raced a heap decommit; it now copies with `ReadProcessMemory`. steamclient's "OUT OF MEMORY" came from our bootstrap stub, which never wrote an out-parameter.

## The problem

Up to August 2026 each game was its own MSIX "slot". The guest exe came from a `ghguest.txt`, its files were pushed by hand, and the Steam engine was set up per slot. The aim was to let a non-developer with a Dev Mode console do it alone: sign in on the TV, see the games the account owns, press A to download one, press A again to play it.

There were two constraints. Nothing Valve wrote may be redistributed. The app container blocks most of what an ordinary Windows downloader takes for granted: it cannot create real child processes, file paths are virtualised, and removable drives cannot be listed.

Once that worked, a second problem appeared. Steam had become wired through the loader: the downloader, the QR code, zlib, zstd, LZMA and Valve's DLLs were all in the package. The project needed Steam to become one optional source among several, delivered and verified like any other package.

## The investigation

### Phase A: the launcher as a pre-guest phase (2026-08-15)

The decision that made this cheap came from three existing facts:

- guest selection was already data (`ghguest.txt`);
- the guest already ran on a detached worker thread;
- the UI thread already owned the D3D11-on-12 bridge and pumped input.

So the launcher is a loop inserted into `App::Run` before `RunGuest` is ever started. It forces `BuildBridgeOnUi` early (normally lazy), draws its own UI and returns a guest path through `g_launcherSelectedGuest`. There is no relaunch and no second process. Everything is behind `/DGH_LAUNCHER`.

- Drawing is Direct2D + DirectWrite into an offscreen texture on the bridge's D3D11 device, copied to the back buffer with the same acquire/present sequence as a guest frame (`LuiInit`, `LuiPresent`). It was the first UI Greenhouse ever drew itself.
- The first on-device run logged `GFX ready level=11_1`, `LAUNCHER LuiInit OK 1920x1080` and `LAUNCHER picker items=N`. A synthetic pad press moved the highlight.
- One build trap: `windows.h` defines `DrawText` as a macro, which renames `ID2D1RenderTarget::DrawText` while `d2d1.h` is parsed, and the method seems to vanish. The fix is `#undef DrawText` before the D2D includes.
- Config files written by the loader are UTF-16, but a hand-edited `ghlibrary.txt` is UTF-8. `LuiDecode` detects which (BOM, or embedded NULs).
- `ghlibrary.txt` exists because the container cannot enumerate a USB root. It can only open paths it is given.
- Packaging moved from one MSIX per game to one thin `Greenhouse.Launcher` package whose storage holds the library.

### Phase B0: can the content system be driven at all? (2026-08-16)

The plan was to rebuild Steam's content download (SteamPipe) rather than drive the real client's installer. The first spike still used the real engine, but only to make two service calls: the content-server list and the per-manifest request code. Both completed. The rest was done by hand on a control VM:

1. GET the manifest from a content server. It arrives as a ZIP holding three sections (payload, metadata, signature) and lists files and chunks, with file names encrypted.
2. GET a chunk. Its size matched the manifest.
3. Decrypt it with AES-256. The first 16 bytes are an ECB-encrypted IV; the body is CBC with PKCS7.
4. The plaintext starts with a container magic: `VSZa` (zstd, newer) or `VZ` (LZMA, older). A zstd frame is followed by a 10-byte Steam footer (crc, size, magic).
5. Decompress. The SHA-1 matched the manifest, byte-exact.

`toolchain/steam_chunk_decode.py` is the reference decoder from this step. It is still in the tree as a host-side debugging aid.

The one piece B0 could not do on a fresh console was obtaining the depot key. The spike had borrowed one from a client install's cache.

### B1: the engine is not needed at all (2026-08-16)

Driving the real client's install path was examined and rejected. Inside steamclient, depot keys are reachable only through the download manager's internal jobs. There is no clean interface method for them, and that job machinery had already proved unreliable on the minimal in-process engine during the multiplayer work. The install entry points also sit next to destructive ones such as uninstall, so probing for them was unsafe.

Instead, an open-source SteamKit port showed that a plain CM client with no Valve code can log on and fetch a depot key. The client was then ported to C++ as `corpus/downloader/cm_tcp_test.cpp`, using BCrypt for RSA-OAEP, AES and HMAC and Winsock for TCP. Built with the project's clang-cl and run under Wine, it returned the same depot key as the reference, byte for byte. Because it is a pure network client, it needed no VM and no Steam install to test.

Each wire fact below cost one debug cycle:

- **TCP only.** The WebSocket CM transport (`cm_ws_test.cpp`) is a dead end: the server closes the socket right after logon. TCP with the channel-encryption handshake works.
- **The anonymous SteamID needs the AnonUser account type.** The first attempt used the type number that actually means Clan, and the logon was refused.
- **Anonymous logon must be minimal.** Any optional field beyond the protocol and package versions, such as OS type, language or machine id, also got the logon refused.
- **The depot-key message number** taken from one reference implementation was wrong for the current protocol. The right one came from a second implementation.

The same client could also make the two content service calls over the CM, anonymously. That removed the engine from every download step. `cm_tcp_test.cpp` then did the whole pipeline: manifest over HTTPS (content servers now require it), ZIP via zlib, manifest parse, chunk, AES, `VSZa`, SHA-1 match.

### B1c: a full depot, and the shell-out dead end (2026-08-16)

`corpus/downloader/gh_downloader.cpp` downloaded a whole public depot (the TF2 dedicated server) under Wine. It fetched, decoded, verified and reassembled all 179 chunks into 42 files, 156 MB, with 0 failures and correctly decrypted file names. Two facts surfaced:

- One depot mixes three chunk formats: `VSZa` (zstd), `VZ` (LZMA-alone) and legacy ZIP (deflate). Dispatch is by magic on the decrypted bytes.
- `zstd.exe` decompresses correctly but exits nonzero on the trailing Steam footer. Trust the SHA-1, not the exit code.

The first in-container design kept `zstd.exe` and `7za.exe` as tools, launched through the loader's in-process `CreateProcess` surrogate (`Bed_CreateProcessW`), because the container forbids real child processes. That was peeled four layers deep and is a confirmed dead end:

1. Each surrogate child gets a private file-system overlay, so a scratch file cannot pass between the loader and the child.
2. Standard output could be captured.
3. Standard input could never be delivered. The tools link the single shared `msvcrt.dll` of the process, whose stdin descriptor is cached before any redirect.
4. `PatchPebStdHandles` was added to the surrogate along the way. It is kept as a correct general fix for child stdio, but it cannot help a CRT that initialised earlier.

So the decoders run in-process. `corpus/downloader/vendor/` holds a single-file zstd decoder (`zstddeclib.c`) and the public-domain `LzmaDec.c`. `decode_chunk` calls one of them or zlib, depending on the magic. Nothing is spawned.

### The download runs in the container (Launcher 0.0.9.0)

The downloader became a header, `corpus/downloader/cm_downloader.h` (namespace `ghdl`, `DownloadDepot` plus a progress callback). It was compiled both into the CLI and into the loader. On the console, a picker item ran it on the UI thread with a D2D progress bar. Result: `DownloadDepot rc=0`. All 179 chunks were fetched, decrypted, decompressed, verified and written. Spot checks of the output (a shell script's size and header, a valid ELF) were correct.

Every step ran inside the UWP container:

- WinHTTP for CM discovery and the content servers;
- the raw-socket CM with its encryption handshake;
- AES-256 through BCrypt;
- the in-process decoders.

The one open question going in, whether WinHTTP works in the container, was answered yes.

### Phase C: sign-in on the TV (Launcher 0.0.12.0)

The first on-screen QR sign-in reused the engine's existing QR machinery in the `steam_api64` shim (`GhStartQrLoginAsync`). The launcher only drew the code, with the MIT `qrcodegen` encoder and integer-scaled black squares on a white quiet zone for camera contrast, and redrew it when Steam rotated the challenge. The scan was approved, the credential was saved and the launcher moved on to the picker.

One on-device fix was needed: staging `steamclient64.dll` alone failed with `ERROR_MOD_NOT_FOUND`, because it imports `tier0_s64.dll` and `vstdlib_s64.dll`. The rule held throughout: only the public challenge URL is ever shown on screen, and the refresh token is never logged or drawn.

This engine-driven sign-in was later replaced (see "Sign-in without Valve's binary" below).

### The owned library, and A to download (Launcher 0.0.33.0 and 0.0.34.0)

The owned library is fetched over the downloader's own CM client, not the engine. It does a token logon, then calls `Player.GetOwnedGames`. On the console it listed 159 games with names. Three wire details mattered:

- the SteamID in the request body is a varint, while in the message header it is fixed64; mixing them up made the service read SteamID 0;
- a token logon must carry the account name to get a full session;
- after logon there is a flood of persona messages, so replies are matched by job id with a time bound.

Pressing A on an owned row needed one more step, from app id to depots and manifest ids. That comes from PICS: an access token, then product info. The reply is the app's text VDF, inline or as a gzip'd file. A small KeyValues parser (`parse_app_depots_vdf`) walks depots, branches and manifest ids, filters by OS, and skips depots shared from other apps. `ghdl::DownloadApp` does everything over one CM connection, in two passes:

1. Get each depot's key and manifest, and count chunks so the progress bar is honest.
2. Fetch, decrypt, decompress, verify and write every chunk into one tree.

Message field numbers were checked against an open-source implementation before coding. That caught one message number that had been misremembered. The flow was de-risked under Wine with an anonymous logon on a public app (1.5 GB, byte-exact), then run on the console: A on 20XX started a 4,045-file download with the bar moving.

### The 0-byte files (Launcher 0.0.35.0)

That first 20XX download finished its bar but left a mixed tree. Some files were real; others, including the game's exe, were 0 bytes. Launching it looked like an incompatibility: `STAGE=guest_read size=0`, then an access violation at address 0. It was not. The loader had mapped a 0-byte exe and faulted while parsing it.

The cause was in `decode_chunk`. A hand-rolled `zstd_frame_len` mis-sized some `VSZa` frames, so `ZSTD_decompress` failed. The file had already been opened with `CREATE_ALWAYS`, so it was left empty.

The fixes:

- size the frame with `ZSTD_findFrameCompressedSize`, falling back to "remainder minus the 10-byte footer";
- refuse A on a guest smaller than 64 bytes, so the picker stays up;
- make `RunGuest` stop on an empty PE.

Two display bugs went with it: the installed row had a blank name, and 20XX was still listed as "owned". Rows are now named by folder, prefer `<folder>.exe` over helper exes, skip 0-byte leftovers, and de-duplicate owned titles against folder, exe and display name.

Today a chunk that fails its hash is never written, and an install carries a `gh_install.txt` marker with `complete=0|1`, so a half-written tree shows as incomplete and can be resumed ([library-and-downloads.md](../steam/library-and-downloads.md)).

### Loading a vanilla Unity tree (§29.12: designed, not built)

A Steam-downloaded Unity game (Lethal Company) closed at once. `UnityPlayer.dll` imports two `user32` functions the container's slim `user32.dll` does not export, and it needs an `opengl32.dll` the container does not have. The OS loader refuses the whole DLL before any of our code can answer. The dedicated per-game slots had avoided this by rewriting import names in those files at pack time.

The notebook then set out a design to replace on-disk rewriting: map such DLLs ourselves, as the exe already is, and bind the missing imports to our beds in memory, leaving the retail bytes untouched. The reasons given were that an on-disk rewrite is undone by Steam's file verify and looks like tampering to integrity checks.

**What the tree shows today.** That design was not built for Unity. Unity DLLs are still handled by an on-disk import-name rewrite at launch (`PatchGuestTreeForContainer`, logged `STAGE=guest_import_patch`), documented in [pe-loading.md](../architecture/pe-loading.md). A related mechanism was built for a different failure: `GhSelfMapDll` maps a DLL itself when every `LoadLibrary` attempt fails, for example on a DLL whose `DllMain` fails inside the container. Later it also took over loading Valve's runtime unmodified (see "What was retracted"). One visible cost of the on-disk rewrite showed up in §35: every Verify of a Unity title "repairs" exactly the two rewritten files, and the next launch rewrites them again. That is harmless, but it is noise.

### Step 1: per-title knowledge is data (Launcher 0.0.669.0, 2026-10-18)

The second arc began with a four-step plan: (1) a title config schema, (2) a library-source interface, (3) a package format with a signed index and an on-console client, (4) Steam as a loadable add-on.

Step 1 removed the compiled per-title tables, including a 50-knob table for one title and a per-exe extra-arguments table. In their place is one JSON file per title, `corpus/titles/*.ghtitle.json`, schema `greenhouse.title/1`. It is read by `src/loader/gh_json.h`, a small reader with no dependencies that throws nothing:

- errors read `line L col C: <what>`;
- duplicate keys are an error;
- depth is capped at 64;
- integers are exact int64;
- files over 1 MiB are refused.

`GhTitles()` loads package configs first, then `LocalState\titles\`, which overrides by `id`. Each file logs one `TITLE file ...` line. A config is REJECTED if it is malformed, if its exe is already claimed by another id, or if it requires a newer loader. Unknown keys are tolerated and logged.

Verification: one title's `CFG`/`TITLE`/`SEED`/`CMDLINE` lines matched the previous build 195 of 197. The two missing lines were reads that happen only when a physical pad is connected or a debug string arrives. Four deliberately bad files each produced the expected REJECTED line. The schema reference is [packages.md](../packages.md#title-configs).

### Step 2: the `LibrarySource` interface (Launcher 0.0.670.0 / 0.0.671.0)

`LibrarySource` is `{id, enumerate, enrich, nameFor, acquire, canVerify, service}`, held in `kLuiSources`. The built-in sources are `installed`, `configured` and `usb`; Steam was at first a built-in fourth. `LuiEnumerate` runs every `enumerate`, then every `enrich`, then a service pass.

The service pass is the important part. A title config can declare `"services": ["steam-runtime"]`. If no source provides that service, the row stays visible, its sub-label says what it needs, and A refuses with a logged `LAUNCHER blocked ... service=steam-runtime` line. Special guest strings in the A handler became `acq->acquire(it, NEW|RESUME|VERIFY)`.

Four defects found while doing this were fixed:

- a credential reader that read one line where the writer wrote three;
- a silent no-op on resuming an incomplete install with no capable source, which now shows a notice;
- a stale owned-list read;
- a wrong comment.

A side finding fixed here (#257): with the engine up but nobody signed in, the engine logs on anonymously, and the shim was handing the game the anonymous session's SteamID. One title then looked for its saves under that ID, found none and quit. An anonymous account type now counts as "no user" (`gh_sid_is_anon`, `kFakeSteamID` in `corpus/shims/`).

### Step 3: packages and a signed repository (Launcher 0.0.673.0 / 0.0.674.0)

The formats and trust chain are in [packages.md](../packages.md#trust-chain).

- `index.json` (`greenhouse.index/1`) has a serial and a list of packages, each with the SHA-256 of its manifest.
- `index.sig` is ECDSA P-256 over SHA-256 of the exact index bytes, verified with BCrypt against public keys compiled into the loader. No crypto library is vendored.
- Each `manifest.json` (`greenhouse.package/1`) lists every file's size and SHA-256.

There is no archive format. Files are fetched one at a time to `*.part`, hashed as they arrive, and moved into place only when every file matches.

`gh_repo.h` refuses an index whose serial is lower than the last one accepted. Title packages land in `LocalState\titles\`. Add-ons land under `DevelopmentFiles` because LocalState is a small shared volume. A Packages page (START on the wall) installs, updates and removes. A startup check reports available updates and never installs on its own.

Host tests (`toolchain/test_gh_repo.sh`, under Wine against a local server) cover parsing, a good, tampered or unknown-key signature, install and re-install, a tampered index, a tampered file and rollback.

The first console update test found a use-after-free in `Install`. A pointer to the old install record pointed into a vector that the commit replaced, and the final log line read through it. Every update (never a first install) hung or killed the worker after the commit had already succeeded. The fix copies the old version string first, and the host test now re-installs over an existing record.

On the console:

- the update went through;
- remove deleted the file;
- an index edited without re-signing was refused with "signature does not verify", and the page listed nothing.

### Step 4: the add-on ABI and its host (Launcher 0.0.674.0)

`src/loader/gh_addon_abi.h` (`GH_ADDON_ABI 1`): an add-on is a package whose root holds `addon.dll`, which exports `GhAddonInit(const GhHostApi*, uint32_t, GhAddonApi*)`. The design choices:

- **Plain C.** No C++ type, exception or allocator crosses the boundary, so an add-on may use another compiler or CRT.
- **Every struct starts with its own size.** Either side appends fields. The ABI number changes only when an existing field changes meaning. `GH_HOST_HAS` and `GH_ADDON_HAS` check that a field is both covered by the size and non-null.

The host (`LuiAddonsLoad`) does these steps:

1. Run `CleanOrphans`, which deletes version folders left by a remove or update.
2. Re-read each installed `addon.dll` and compare its SHA-256 with the hash recorded at install.
3. Only then call `LoadLibraryExW`.
4. Run init under SEH.

An add-on is refused, with a reason, for any of these: a hash mismatch, a missing export, a wrong ABI, a nonzero return, a source id already taken, or more than four add-ons (`kLuiMaxAddons`).

Console cells with `corpus/addons/sample/sample_addon.cpp`:

- install and load: the picker grew by one row;
- a byte flipped in `addon.dll`: REFUSED with both hashes logged, and the picker shrank back;
- remove while loaded: deleted at the next start;
- remove while not loaded: deleted at once.

A side fact: Device Portal returns 500 when uploading over a mapped `addon.dll`. A loaded add-on is write-locked.

The step ended with the Steam pieces split by one question: does this piece need Valve's binary?

- The library and the downloader did not. They were blocked only by a missing progress callback.
- Sign-in and the game-facing runtime did. They were gated on how Valve's three DLLs would reach the console, because a dev package staging them could not be published.

### Valve's runtime from Valve's CDN (#258, Launcher 0.0.675.0)

The chosen route: the console downloads `steamclient64.dll`, `tier0_s64.dll` and `vstdlib_s64.dll` itself, from Valve's public client-update CDN. The loader's engine code was written against one exact build. That build turned out to be SteamCMD's own Windows package, still fetchable, and older packages stayed available too. The CDN honours `Range`, so the console fetches only the three entries: about 10.3 MB of a 23.4 MB zip.

The package format gained a `from` file entry. It holds a URL, an offset and length, the method (deflate), the vendor's SHA-256 and the final SHA-256, and holds no bytes. `FetchFrom` in `gh_repo.h` works in this order:

1. Ranged GET; only a 206 is accepted.
2. Raw inflate with the new `gh_inflate.h` (no zlib) into exactly the stated size.
3. Check the vendor hash.
4. Check the final hash.
5. Write the `.part` file only after every check passes.

The signed index covers the manifest, so a changed CDN can only cause a refusal. `ghpkg.py vendor` builds the manifest from `corpus/addons/steam-runtime/vendor.json` and stores no vendor bytes. `toolchain/test_gh_inflate.sh` runs the inflater under ASan/UBSan against every zlib level, every entry of the real zip, truncations and 20,000 garbage inputs.

The loader pins the runtime a second time. `GhSteamRuntimeDir` hashes all three files once per process against `kSteamRuntimePin`, compiled in, so the loader refuses any other build whatever the index says. A verified folder wins over any packaged or game-folder copy. `GH_STEAM_RUNTIME_FETCH=1` builds a package with no Valve DLL and fails the build if one is found in the stage.

Console cells:

| Cell | Result |
|---|---|
| R1, no runtime | `STEAMRT not installed`, sign-in not available, picker still up |
| R1, install | Three files fetched from Valve's CDN; the local repository server logged only the index, signature and manifest. No Valve byte came from Greenhouse |
| R2, restart | `STEAMRT ... verified (3 files)`, engine loaded, sign-in OK |
| R3, one byte flipped | `STEAMRT ... REFUSED ... is not the pinned ...`, no crash |
| R4, bytes restored | Verified, sign-in OK |

A guest game (Lethal Company) was then started on the fetched runtime (§39.3) and reached its first screen.

### Progress for add-ons, and sign-in without Valve's binary (Launcher 0.0.676.0)

Two prerequisites for moving Steam out:

- **`GhHostApi::progress` and `status`**, appended to the ABI. They draw a frame, present and pump. They work only on the thread inside an `acquire()` call; anywhere else they are ignored and logged once, so an add-on cannot paint over the picker from a worker. The sample add-on drew 41 progress frames; screenshots showed the bar and the status line.
- **QR sign-in over Greenhouse's own CM client.** `CmClient::qr_begin` and `qr_poll` call the same two authentication service methods the engine used, with challenge rotation followed in place. A host test (`cm_qr_test.cpp`, under Wine, nobody scanning) got a challenge and pending polls. Two shortcuts failed: with no logon, or with a SteamID of 0, the CM closes the socket. An anonymous logon is required first, as it is for the engine. The unauthenticated path was deleted rather than kept.

With that, everything the launcher itself does with Steam (sign-in, library, download) ran without Valve's binary. The engine is needed only by games that call the Steamworks API.

### Steam becomes an add-on (#260, Launcher 0.0.677.0 / 0.0.678.0)

`corpus/addons/steam/steam_addon.cpp`, built by `toolchain/build-addon.sh --steam`, provides source `steam` and service `steam-runtime`. The 0.0.677.0 `addon.dll` was 691,712 B and imported only KERNEL32, WS2_32, WINHTTP and bcrypt. The loader dropped `cm_downloader.h`, zlib, zstd and LZMA from its compile; `strings` on the loader found none of the Steam service names.

The ABI gained, append-only:

- **Host side:** `progressFiles`, `signInScreen` and `installDir`. With `signInScreen` the host draws a QR code from a module matrix, so the add-on needs no graphics.
- **Add-on side:** `signIn`, called once before the picker, and `claims`, which lets the add-on take acquire and verify for a row another source listed.

The add-on's `signIn` runs in this order:

1. With a saved sign-in, fetch the library at once. This happens before the engine starts, so it is the account's only session.
2. Otherwise, or if the logon is refused, run up to five QR sessions.
3. On approval, take the SteamID from the token's `sub` claim (`JwtSubject`).
4. Save the credential before the engine starts, so the engine logs in from it without a second scan.
5. Scrub the token from memory.

Console cells (signed in): the add-on logged `ListOwnedGames rc=0 games=161` and `signIn -> 1 in 2172 ms`, the engine was signed in after 4,657 ms, and Lethal Company reached its first screen. A harness trap surfaced here. The synthetic pad's `DOWN` is the left stick, and the Packages page read only the d-pad, so presses landed on the wrong row. 0.0.678.0 makes the page take the stick too.

### One build, gated at runtime (#261, Launcher 0.0.679.0)

`GH_NO_STEAM` had produced a second, Steam-free package. It was retired: `iterate-launcher.sh` exits 1 if it is set. Every build is the same binary, and Steam is decided at runtime:

- **Default deny.** `g_ghSteamRuntimeProvided` is set from `LuiServiceProvided("steam-runtime")` after add-on sign-in. Until then, `GhEngineOff()` reports off and `GhEngineDenied` refuses `steamclient64.dll` for the launcher and every guest.
- **The shim is split.** The package carries only an inert `steam_api64.dll` (`/DGH_SHIM_INERT`), which is still preloaded so that a game can never load Valve's own `steam_api64` from its folder. The engine-enabled shim ships as a second file of the `steam` add-on. `LuiServiceFile` uses it only if its SHA-256 matches the per-file hash recorded in `installed.json` at install.
- **Fail closed.** Install records written before per-file hashes existed have no hash for the shim, so the shim is refused and the engine stays down. That was checked on the console before the update; after the update the add-on's shim verified and the engine signed in.

Sign out (#262) was added with it. The add-on exports `account` and `signOut`; the Packages page asks for confirmation before calling `signOut`, which deletes the saved sign-in and the cached library.

### Supported titles in the picker (Launcher 0.0.680.0 / 0.0.681.0)

The first download through the add-on stalled before it began: Cheese Rolling had no row. Steam's owned-games reply leaves out a free-to-play game with no playtime, and the flags meant to include such games did not bring it back.

The fix is a title-config pass in `LuiEnumerate`. Every config with a `steamAppId` that no source listed, and whose services are all provided, gets a "not installed" acquire row. The add-on's `claims` attaches its downloader to that row. With no add-on, no such rows appear. Results:

- Cheese Rolling downloaded: `GHDL END rc=0`, 142 files, about 257 MB.
- R.E.P.O. downloaded: 208 files, 1,702 chunks.

Launching Cheese Rolling then died about 30 s in on an access violation outside every image. That was Mono's JIT code: Mono implements C# null checks as first-chance faults and turns them into exceptions in its own vectored handler. Our crash observer, kept ahead of guest handlers for the pager, ended the run before Mono saw the fault. Since 0.0.681.0, the crash observer (`CrashObserverVeh`) logs a fault in committed non-image memory as `fault rip is outside every image (JIT code) -- left to the guest's handler` and lets it pass. Cheese Rolling and R.E.P.O. then reached their main menus. R.E.P.O. logged three such faults that 0.0.680.0 would have killed it on.

### Bug: a probe that raced a heap decommit (#271, steam add-on 1.0.2.0)

At Launcher 0.0.691.0 the launcher vanished at the picker. The crash observer put the fault in a `memcpy` inside the engine shim's connection-manager heap scan (`gh_find_connected_ccm`). `gh_probe_read` asked `VirtualQuery` whether a page was readable and then copied from it. Between the two calls, another steamclient thread decommitted the page. A query is only a hint, since any thread can change the mapping before the copy.

The fix in `corpus/shims/gh_steam_engine.h` keeps the query as a cheap pre-check and copies with `ReadProcessMemory(GetCurrentProcess(), ...)`. The kernel takes the fault and the call fails instead of raising one.

Because the engine shim ships only in the add-on, the fix was a package version (`steam` 1.0.2.0), not a launcher build. The launcher needed no change for the new hash: `Install` writes the hashes from the signed manifest into `installed.json`, and `LuiServiceFile` checks against that record. On the console, the update installed, the shim verified, and the scan completed.

### Bug: steamclient's "OUT OF MEMORY" (#267, Launcher 0.0.697.0)

With the engine signed in, one title died about 5 minutes in. tier0 printed `OUT OF MEMORY! attempted allocation size: N` and took its fatal-assert exit. The container was nearly full, but no allocation of that size exists: N was 1.6 to 1.9 GB, and once it was the low half of a pointer. The size was garbage. The allocation stack named only steamclient. The earlier suspect, the shim's connection-manager fan-out, was not involved; a gate on it had changed nothing.

The cause: steamclient looks up `GetClientBootstrapMetrics` in the host exe with `GetProcAddress(GetModuleHandle(NULL), ...)`. In our process that lands in `Bed_GetProcAddress`'s bootstrap table. steamclient then uses the usual two-call size protocol: call with a null buffer to learn the size, allocate, call again. Our stub took no arguments and returned null, so it never wrote the size, and steamclient allocated whatever was on its stack. Usually that was small. Sometimes it was a pointer fragment, which is fatal in a full container.

The fix gives `Bed_GetClientBootstrapMetrics` the real signature. It writes `*pcbRequired = 0`, returns false and logs `BOOTSTRAP GetClientBootstrapMetrics ... -> 0 bytes` once. A size of 0 takes steamclient's own skip branch.

A class check covered every other host-exe export steamclient looks up. Each stub matched its caller's use. Names we do not stub fall through to the real `GetProcAddress`, which returns NULL, and steamclient handles that. So this was the only stub with an out-parameter it failed to write.

On the console the title ran 12 minutes with 0 OUT OF MEMORY lines and steady commit; both earlier crashes had come at about 5 minutes.

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| Downloads should drive the real client's installer, or use the engine as an oracle for content calls | The engine was already in-process and completed service calls | Depot keys are reachable only inside its download-manager jobs, which were unreliable in-process. A standalone CM client did every step without it (§29.4–§29.5) |
| The CM can be reached over WebSocket | Modern clients use it | The server closed the socket right after logon; TCP with channel encryption works |
| A depot-key message number from one reference implementation | It was in a working client | It was wrong for the current protocol; a second implementation and a live test gave the right one |
| `zstd.exe` / `7za.exe` can run through the `CreateProcess` surrogate | Output capture worked | Children get a private file overlay and share a CRT whose stdin is fixed before any redirect, so input can never arrive. Decoders moved in-process |
| 20XX's launch crash was a game incompatibility | It faulted at address 0 right after mapping | The exe was a 0-byte file, left by a zstd frame-size bug in the downloader |
| A data-only add-on package is REFUSED | §37's refusal list said so | Corrected in §38: it logs `data-only (no addon.dll) -- nothing to load` |
| Steam sign-in needs Valve's `steamclient64.dll` | The working QR flow ran through the engine | §39.2: the same service calls work over Greenhouse's own CM client after an anonymous logon |
| The fetched `steamclient64.dll` must carry a one-import rename, applied as a patch in the `from` entry | The OS loader refuses the pristine DLL in the container | Later (#277, after this period) the patch was dropped: the file stays byte-identical to Valve's and the loader self-maps it with `GhSelfMapDll`, bedding that import in memory. `vendor.json` records this; [runtime-fetch.md](../steam/runtime-fetch.md) describes the current format |
| `qrcodegen` left the loader with Steam (§40) | The add-on builds the Steam QR matrix | The loader compiles `qrcodegen` again for its own QR screens (`LuiQrMatrix`); the Steam code is still built inside the add-on |
| Owned-games flags bring back unplayed free-to-play titles | The request has flags for them | They do not; title configs supply those rows instead (§42.1) |
| steamclient's OUT OF MEMORY came from the shim's connection-manager fan-out | It was the newest engine-side code | Gating it changed nothing; the size was an unwritten out-parameter in our bootstrap stub (§49) |

## What shipped

All items below were checked against the tree.

| Area | What to search for |
|---|---|
| Pre-guest launcher phase | `LauncherPickGuest`, `g_launcherSelectedGuest`, `BuildBridgeOnUi`, `LuiInit`, `LuiPresent`, `LuiDecode` |
| Depot downloader (add-on side) | `corpus/downloader/cm_downloader.h`: `ghdl::DownloadApp`, `pics_access_token`, `pics_product_info`, `parse_app_depots_vdf`, `decode_chunk`, `ZSTD_findFrameCompressedSize`; `corpus/downloader/vendor/zstddeclib.c`, `LzmaDec.c`; `gh_install.txt` marker |
| Host tests and spikes | `corpus/downloader/cm_tcp_test.cpp`, `cm_ws_test.cpp` (the dead WebSocket route), `cm_qr_test.cpp`, `gh_downloader.cpp`, `build_cm_test.sh`; `toolchain/steam_chunk_decode.py` |
| Title configs | `src/loader/gh_json.h`, `GhTitles`, `GhTitleMatch`, `corpus/titles/*.ghtitle.json`, `toolchain/test_gh_json.cpp` |
| Library sources | `LibrarySource`, `kLuiSources`, `LuiEnumerate`, `LuiAddonEnum`, `LuiCanVerify` |
| Repository | `src/loader/gh_repo.h`: `FetchIndex`, `VerifyIndexSig`, `FetchFrom`, `CleanStaging`, `CleanOrphans`, `InstalledSha`, `kGhRepoKeys`; `LuiRepoStartupCheck`, `LauncherPackages`; `toolchain/ghpkg.py`, `toolchain/test_gh_repo.sh` |
| Inflate without zlib | `src/loader/gh_inflate.h`, `toolchain/test_gh_inflate.sh` |
| Add-on ABI and host | `src/loader/gh_addon_abi.h` (`GH_ADDON_ABI`, `GhAddonInit`, `GH_HOST_HAS`, `GH_ADDON_HAS`), `LuiAddonsLoad`, `kLuiMaxAddons`, `LuiAddonSignInScreen`, `LauncherAddonsSignIn`, `LuiDrawProgress`; `corpus/addons/sample/sample_addon.cpp`; `toolchain/build-addon.sh` |
| Steam add-on | `corpus/addons/steam/steam_addon.cpp` (`QrMatrix`, `JwtSubject`, `SignOut`, `claims`) |
| Runtime fetch and pin | `corpus/addons/steam-runtime/vendor.json`, `GhSteamRuntimeDir`, `kSteamRuntimePin`, build variable `GH_STEAM_RUNTIME_FETCH` |
| Runtime gate | `g_ghSteamRuntimeProvided`, `LuiServiceProvided`, `LuiServiceFile`, `GhEngineOff`, `GhEngineDenied`, `LauncherEngineWait`, `/DGH_SHIM_INERT` |
| Anonymous session is no user | `gh_sid_is_anon` in `corpus/shims/gh_steam_engine.h`, `kFakeSteamID` |
| Mono JIT faults pass through | `CrashObserverVeh`, diag line `fault rip is outside every image (JIT code)` |
| Probe race fix | `gh_probe_read` in `corpus/shims/gh_steam_engine.h` |
| Bootstrap stub fix | `Bed_GetClientBootstrapMetrics` in `src/loader/guest_loader.cpp`, diag line `BOOTSTRAP GetClientBootstrapMetrics` |

These names from the period are gone from the tree, because their work moved into the add-on: `LauncherSteamLogin`, `LuiBlitQr`, `LauncherFetchLibrary`, `LauncherRunAppDownload`, `LauncherDlCallback`. The `GH_NO_STEAM` build is retired.

Reference documentation: [launcher/README.md](../launcher/README.md), [launcher/library.md](../launcher/library.md), [packages.md](../packages.md), [steam/sign-in.md](../steam/sign-in.md), [steam/library-and-downloads.md](../steam/library-and-downloads.md), [steam/runtime-fetch.md](../steam/runtime-fetch.md).

## Still open

- **Unity DLLs are still rewritten on disk.** §29.12's in-memory mapping for a vanilla Unity tree was not built. The visible cost is that Verify always "repairs" the two rewritten files.
- **The rollback serial is one number per console, not per repository.** §36 predicted the problem and it later happened: a test repository with a high serial locked a console out of a repository with a lower one. Keep test serials low ([packages.md](../packages.md#trust-chain)).
- **The runtime pin is one Valve build.** If Valve removes it from the CDN, new installs refuse at the ranged GET; consoles that already installed it keep working.
- **Downloads trust the per-chunk hash, not the transport.** `http_fetch` does not treat a certificate problem as fatal, so reuse it only for hash-checked data ([library-and-downloads.md](../steam/library-and-downloads.md#transport-note)).
- **A game that asks for the user while the engine is still anonymous** keeps the built-in user for that object after a later sign-in. Sign-in happens before any game starts, so this matters only for a mid-game sign-in.

## Lessons

- Insert a new UI as a phase of the existing process, not as a second app. The swapchain, input pump and guest thread already existed, so the launcher needed no relaunch or IPC.
- Reimplement a protocol from open-source clients rather than drive a vendor binary's internals. The standalone CM client was testable under Wine in minutes, and the engine route was fragile and partly unreachable.
- Verify protocol field and message numbers against a second implementation before coding. Two numbers taken from memory or from one source were wrong.
- In the app container, run the work in-process instead of spawning a tool. Surrogate children cannot share files or receive stdin, so a decoder compiled into the binary beat a working `zstd.exe`.
- Never create the output file before the data that fills it has decoded and verified. A failed decode after `CREATE_ALWAYS` produced a 0-byte exe that looked like a game incompatibility.
- Check content hashes, not tool exit codes. `zstd.exe` exited nonzero on correct output, and the SHA-1 was the only reliable verdict.
- Put per-title behaviour in versioned, strictly parsed data. Configs could then be shipped, overridden and rejected with a reason, without a loader build.
- Pin third-party bytes twice: in the signed manifest and in the loader. Either alone can be wrong; both together turn a changed CDN or a tampered file into a clean refusal.
- Design a plugin ABI as plain C with a size field leading every struct. Every addition in this period (progress, sign-in screen, install dir, claims, sign-out) was an append under ABI 1.
- Make a missing capability a runtime gate that fails closed, not a second build. One binary with a default-deny service check replaced `GH_NO_STEAM` and refused old install records safely.
- A `VirtualQuery` answer is stale by the time you use it. Copy through `ReadProcessMemory` when another thread may change the mapping.
- Give every stub the real signature and write every out-parameter. A stub that "returns nothing" leaves the caller reading its own uninitialised stack, and the crash appears far away in someone else's allocator.
- Gate harness key presses on a log line or pixel check that proves the page is open. A lost press after a restart turns the next A or X into a launch or an uninstall on the wrong page.
