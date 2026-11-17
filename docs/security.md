# Security model

What this covers: one threat model for the whole app: what is worth protecting, every boundary
where outside data or code enters, what each boundary checks, what runs with full rights anyway,
the known weaknesses, how to report a vulnerability, and which host tests cover which boundary.
Where the code is: `src/loader/gh_repo.h` (`VerifyIndexSig`, `ParseIndex`, `ParseManifest`,
`FetchIndex`, `Install`, `CheckThemeBytes`), `src/loader/launcher_ui.h` (`LuiAddonsLoad`,
`LuiServiceFile`, `GhSteamRuntimeDir`, `kSteamRuntimePin`, `LuiCmdStart`), `src/loader/gh_web.h`
(`Gate`), `src/loader/gh_custom.h` (`SafeRel`, `InspectExe`), `src/loader/gh_remote.h`,
`src/loader/gh_crash.h` (`ghcrash::Scrub`), `src/loader/gh_crypto.h`, `src/loader/gh_site.h`,
`src/loader/gh_compat.h`, `src/loader/guest_loader.cpp` (`GhTitleFromJson`, `GhTitlesLoadDir`),
`corpus/addons/steam/steam_addon.cpp`, `corpus/downloader/cm_downloader.h`.
Related: [packages.md](packages.md) · [addons.md](addons.md) ·
[launcher/web-manage.md](launcher/web-manage.md) ·
[launcher/remote-manage.md](launcher/remote-manage.md) ·
[launcher/crash-report.md](launcher/crash-report.md) · [steam/sign-in.md](steam/sign-in.md) ·
[steam/runtime-fetch.md](steam/runtime-fetch.md) · [mods.md](mods.md) · [deploying.md](deploying.md)
Period: current as of 2026-11-13, Launcher 0.0.956.0. Notebook sections: §63e (theme packages),
§64 (the compatibility record and the crash report), §65 (your own games and the browser page),
§66 (the phone relay).

## Summary

Greenhouse is one process. The launcher, every add-on, the game, its mods and any game the user
uploads all run in the same UWP app container with the same rights. So the real security
boundaries are the points where *code* gets into that process, and there are few of them: the
MSIX itself, add-ons from a signed repository, games from Steam, mods from Thunderstore, and
games the user uploads. Everything else that arrives from outside (title configs, themes, the
compatibility dataset, browser requests, relay messages) is data. Each data path is parsed
strictly, with size limits and allow-lists, so it cannot become code. Code paths are checked for
*origin* (a signature, a pinned hash, an approved browser), never for *behaviour*: once code is
loaded it can read everything the app can, including the saved Steam sign-in.

## Assets

| Asset | Where it lives | What protects it | What can still reach it |
|---|---|---|---|
| The user's Steam sign-in | A plain-text file in the app's `LocalState` ([steam/sign-in.md](steam/sign-in.md)) | The app container: no other app can read `LocalState` | Every guest game, mod, add-on and uploaded game (same process); anyone with Device Portal access |
| Saves | `LocalState` (redirected save folders) and `D:\DevelopmentFiles\gh_saves\` ([architecture/filesystem-registry.md](architecture/filesystem-registry.md)) | The container; the remote op list cannot express "delete a save" | Same as above; uninstalling the app deletes `LocalState` |
| The console | A Dev Mode Xbox | The app container; Greenhouse never asks for more than its package capabilities | Device Portal, which is effectively root over sideloaded apps |
| The user's PC and phone | Their own browsers | Browser origin isolation; the console never pushes anything but its own page | A browser on the LAN that the person at the TV approves |
| The repository signing key | Outside the source tree, on the publisher's machine | Never reaches the client, the repository or the console | Whoever holds the private half can ship code to every console that trusts it |

The MSIX signing certificate is a separate asset with the same shape: whoever holds it can build
an app the console will install over Greenhouse. Neither key, nor any path to one, belongs in the
tree ([AGENTS.md](../AGENTS.md), hard rule 5).

## The outer boundary: app container and Dev Mode

Everything below sits inside two facts that Greenhouse cannot change.

- **The app container.** The process cannot reach other apps' data, load a kernel driver, or use a
  capability its package manifest does not declare. The launcher manifest declares
  `internetClient`, `internetClientServer`, `privateNetworkClientServer` and the microphone. A game
  gets all of these, because it is the same process. The microphone still needs the user's answer
  to the OS prompt.
- **Dev Mode.** Greenhouse only runs on a console in Developer Mode. Windows Device Portal on that
  console can upload and read any file in the app's `LocalState` and `DevelopmentFiles`, install and
  remove apps, and send input. Device Portal authentication is optional. Treat
  "has Device Portal access" as "has everything listed in the assets table". See
  [deploying.md](deploying.md) for setting a Device Portal password.

## Trust boundaries

Each subsection says what crosses, what is checked, and what the code that crosses can do
afterwards. The per-feature documents carry the full detail.

### Package repository

What crosses: `index.json`, `index.sig`, per-package `manifest.json` and files, from a static web
server named by `GH_SITE_URL` (`gh_site.h`) or a URL in `LocalState\gh_repo_url.txt`.

What is checked (`gh_repo.h`, spec in its header comment; [packages.md](packages.md#trust-chain)):

- `index.sig` is ECDSA P-256 over SHA-256 of the exact index bytes, verified with CNG
  (`VerifyIndexSig`, `VerifyP256`) against public keys compiled into the build (`kGhRepoKeys`). An
  empty key table fails every verification closed. A development key is trusted only in a build
  with `GH_REPO_DEV_KEY`; a release build (`GH_RELEASE=1`) refuses to build with it.
- The index pins each manifest's SHA-256; each manifest pins each file's size and SHA-256.
- Rollback: the index carries a `serial`. `FetchIndex` refuses a serial lower than the highest the
  console has accepted. The serial is recorded only when something is installed, and there is one
  high-water mark per console, not per repository.
- Parsers are strict and bounded: documents 1 MiB (`kMaxDoc`), 256 files per package
  (`kMaxFiles`), 512 MiB per file (`kMaxFile`), JSON nesting capped (`gh_json.h`). Ids pass
  `SafeId`; file paths pass `SafeRelPath` (relative, no `.` or `..` segments, no drive, no
  alternate data stream); relative URLs cannot climb out of their document's directory
  (`ResolveUrl`). Encrypted packages from older builds are refused by name.
- Nothing is written to its final place until every byte has matched. Files stage as `*.part`;
  a package stages as `<id>\<version>.part\` and is renamed in one step.

Because integrity comes from the signature chain, plain HTTP is safe for integrity but not for
privacy. The default site is HTTPS only (`ghsite::Server` drops anything else). Pointing
`gh_repo_url.txt` at another server cannot add a trusted key; at worst it denies service.

### Add-ons

What crosses: a native DLL, `addon.dll`, delivered as an `addon` package.

What is checked: only add-ons recorded in the installed-package state are loaded. `LuiAddonsLoad`
re-reads `addon.dll` and compares its SHA-256 with the `entrySha256` recorded at install time
before `LoadLibraryExW`. Other files an add-on hands to the loader (the Steam add-on's
engine-enabled `steam_api64.dll`) are re-hashed the same way (`LuiServiceFile`). At most four
add-ons load (`kLuiMaxAddons`); `GhAddonInit` runs under a structured exception handler; an add-on
that claims a source id already taken is refused.

What the hash buys: an add-on file that was swapped on the `DevelopmentFiles` volume after
install does not load. What it does not buy: any limit on what a loaded add-on does. **An add-on
runs with the app's full rights.** It receives the `LocalState` and `DevelopmentFiles` paths in
`GhHostApi`, can read the saved sign-in, and can open network connections. The only control is
the repository key: install add-ons only from a repository whose key you trust. The ABI is in
[addons.md](addons.md) and `gh_addon_abi.h`.

### Themes and compatibility packages

Both are inert by construction, checked twice, at parse time and at install time, by allow-list:

- **Theme packages** (`ParseManifest`, `ThemePartOf`): every path must be `theme.json`, a font
  (`.ttf`, `.otf`, `.ttc`) or an image (`.png`, `.jpg`, `.jpeg`, `.bmp`). Anything else, a `.dll`
  included, refuses the whole package. Exactly one top-level `theme.json`; 16 MiB per file, 64 MiB
  per theme. Before commit, `CheckThemeBytes` proves each staged file is what its extension claims
  (JSON parses; font and image magics). A theme names a layout compiled into the loader; it cannot
  supply one.
- **Compatibility packages** (`IsCompatPath`): exactly one file, named `compat.json`, at most
  1 MiB, parsed by the loader's own reader (`checkCompat`) before it replaces the current dataset.
  `gh_compat.h` can only answer a status, a version and a date per Steam app id.

Residual surface: fonts and images are decoded by system components (DirectWrite, WIC) inside
the process. A theme is as safe as those decoders. Loose themes in `LocalState\gh_theme\` are for
development and get the theme parser but not the package checks.

### Title configs

What crosses: `*.ghtitle.json` files from a `title` package, or placed by hand in
`LocalState\titles\`.

What is checked: a title package may hold only flat `*.ghtitle.json` files and `seed/<Name>/...`
files, each at most 1 MiB. `GhTitlesLoadDir` and `GhTitleFromJson` refuse a file over 1 MiB or
with a wrong schema, an `id` or `seed` that is not a safe name, a `match.exe` that is not a bare
file name, an absolute `launch` or `installProbe` path, a knob that is not `gh_*` with a uint32
value, or a `requires.loader` newer than the build. A seed only fills missing files; it never
overwrites.

What a config can still do: it is data, but it steers the loader. It sets knobs, appends
command-line arguments (`args`) and chooses a settings seed. Trust a config as you trust the
repository it came from. Games the user uploads never receive a title config. Format:
[packages.md](packages.md#title-configs).

### Mods

What crosses: mod loaders and mods, downloaded on the console by the user from Thunderstore.
The project never hosts or mirrors them ([mods.md](mods.md#rules)).

What is checked: archives are capped at 1 GiB and unpacked by the project's own zip reader
(`gh_zip.h`), which rejects unsafe paths. Profile codes are validated (`ValidCode`). Thunderstore is
reached over HTTPS with normal certificate validation.

What is not checked: what a mod does. **A mod runs with full rights in the game's process**, which
is the app's process. Greenhouse shows what Thunderstore publishes and does not review it.

### Your own games (uploaded over the LAN)

What crosses: an executable and its folder, uploaded from a paired browser
([launcher/web-manage.md](launcher/web-manage.md), [launcher/library.md](launcher/library.md#custom-games-gh_customh)).

What is checked: every upload path goes through `SafeRel` (relative, no `..`, no drive or device
names, no alternate data streams, at most 200 characters, at most 100000 files per game), so an
upload cannot write outside its own folder. `InspectExe` reads the PE header to report the
architecture and whether the file can run. The store keeps 1 GiB free (`kKeepFreeBytes`).

What is not checked, said plainly: **an uploaded game runs in the launcher's process with the app's
full rights. It can read everything in `LocalState`, including the saved Steam sign-in, and
everything on the `DevelopmentFiles` volume.** It gets no title config and no Steam runtime, even if
the folder looks like a Steam game (`LooksSteamy` only warns), but that is a compatibility choice,
not a sandbox. Upload only executables you would run on your own PC.

### The LAN web server

What crosses: HTTP requests from browsers on the home network. Off by default; turned on from the
TV. Plain HTTP on port 8880 to 8884.

What is checked, in `Gate()` (`gh_web.h`) before any handler:

1. The peer address must be RFC 1918, link-local or loopback IPv4 (`IsPrivateV4`), else 403.
2. `Host` must be an IPv4 literal, else 421. A DNS-rebinding page can only send its own hostname.
3. Any method other than GET or HEAD must carry `Origin` equal to `http://` + `Host`, else 403.
4. `OPTIONS` is always refused. The session travels in the custom `X-GH-Session` header, so a
   cross-origin request would need a preflight that is never granted.

Then: a browser must be approved on the TV once (a four-digit code shown on both screens; A only
counts after the prompt has been up 500 ms). Session tokens are 256 random bits from the system
RNG; the console stores only their SHA-256 and compares in constant time (`ConstEq`). At most four
connections, a 16 KiB header cap, 30 s timeouts, uploads streamed to disk. Responses carry
`no-store`, `nosniff`, `X-Frame-Options: DENY` and a restrictive Content-Security-Policy. A browser
can only manage games it uploaded: no Steam titles, packages, settings or other files.

Known limits (also in [web-manage.md](launcher/web-manage.md#security-model)): traffic is
unencrypted, so anyone who can sniff the home network can read uploads and capture a session
token; anyone on the LAN can ask to pair, though approval needs the controller.

### The remote relay ("Manage from anywhere")

Compiled out by default. It exists only in a build with `/DGH_REMOTE_RELAY`, and even then it
cannot poll: the console credential it needs (`Host::token`) is not designed yet and returns an
empty string. A default build has no switch, no poller and no device id.

The protocol as designed and implemented in `gh_remote.h`
([launcher/remote-manage.md](launcher/remote-manage.md)):

- The console only makes outbound HTTPS requests. Nothing connects to it.
- Pairing: a QR code whose secret is in the URL fragment, which browsers never send to a server;
  the relay sees only its SHA-256. The phone proves it saw the code with an HMAC; both screens then
  show a six-digit code derived from both public keys, and pairing completes only on A.
- Messages: AES-256-GCM under HKDF over ECDH P-256, one key per direction, with device, phone,
  sequence and time in the additional data. Old sequences and times more than 10 minutes off are
  refused.
- Every command that changes something carries a passkey (WebAuthn ES256) signature over the
  command, with user verification required, verified on the console.
- The op list is closed (`kOps`, `ValidArg`): refresh, and install, resume, verify, cancel, dequeue
  or uninstall by Steam app id (or uninstall one uploaded game). No op can launch a game, name a
  path, pass arguments, read or write a file, or change a setting.

The relay can drop, delay and time messages and see metadata. It cannot read, forge, replay or
widen a command. A compromised relay-served phone page could misuse the phone's keys while the user
approves prompts, within the closed op list.

### Crash reports

What crosses: a scrubbed tail of the diagnostic log, from the console to the project's site, only
when the user asks for it on a consent screen after a crash, or from the browser page for an
uploaded game ([launcher/crash-report.md](launcher/crash-report.md)).

What is checked: `ghcrash::Scrub` is a whitelist at word granularity. A word survives only if it is
a number, an identifier, short punctuation, or a `key=value` pair of those. Quoted text never
survives, which is what removes the Steam account name the log carries in an `ADDON` sign-in line.
Paths are cut to their last component. Any line that mentions a credential word (token, secret,
password and the like) is dropped whole. Strings the launcher knows identify the user are removed
wherever they appear (`AddSensitive`). The report is capped at 192 KiB. A report about an uploaded
game is filed under the executable's hash, size and architecture, never the folder or the name the
user typed.

A build may carry a write-only ingest credential for the site (`GH_CRASH_TOKEN`). It ships inside
the binary, so it is public in effect: it lets the site filter and rate-limit, it does not
authenticate a console, and it cannot read reports back.

### The Valve runtime fetch

What crosses: three Valve DLLs and `steam.exe`, fetched by the console from Valve's own CDN,
never from a Greenhouse server ([steam/runtime-fetch.md](steam/runtime-fetch.md)).

What is checked: two independent pins must agree with the bytes. The signed package manifest
carries `from` entries (a byte range in Valve's archive and the SHA-256 the result must have);
`FetchFrom` writes nothing unless the hash matches. Before use, `GhSteamRuntimeDir` hashes the
installed files again against `kSteamRuntimePin`, a table compiled into the loader, and refuses any
mismatch. Only add-on packages may carry `from` entries. `steam.exe` is never executed.

### Steam sign-in and downloads

What crosses: the QR sign-in, the saved sign-in, the owned-games list, depot manifests and chunks
([steam/sign-in.md](steam/sign-in.md), [steam/library-and-downloads.md](steam/library-and-downloads.md)).

- The QR challenge URL is a short-lived credential. The add-on logs only its length. Token buffers
  are wiped after use (`Scrub` in `steam_addon.cpp`); log lines never contain the token.
- The saved sign-in is plain text in `LocalState`. That was a deliberate trade-off so the user is
  not asked to scan a code at every launch. Signing out deletes it.
- In the engine build, Valve's runtime logs on with the saved sign-in inside the game's process.
  The game holds a live Steam session by design.
- The weaknesses of the download path are under [Known weaknesses](#known-weaknesses).

### Shared crypto

All cryptography goes through Windows CNG (BCrypt); nothing is hand-rolled except encodings.

| Primitive | Where | Used by |
|---|---|---|
| SHA-256, ECDSA P-256 verify | `gh_repo.h` (`Sha256`, `VerifyP256`) | Index signature, every package and add-on hash |
| HMAC-SHA256, HKDF-SHA256, AES-256-GCM, ECDH P-256, base64url | `gh_crypto.h` (`Hmac256`, `Hkdf32`, `AesGcmOpen`, `Ecdh`, `B64UrlDecode`) | The relay (`gh_remote.h`) only, today |
| Random bytes | `BCryptGenRandom` (`RandomHex` in `gh_web.h`, `Random` in `gh_remote.h`) | Session tokens, upload ids, pairing tokens, nonces |
| DPAPI | `CryptProtectData` in `gh_remote.h` | The relay's private key only |

DPAPI protects nothing else. Where it is unavailable the relay key is masked with a
device-derived value, and that is logged as obfuscation, not protection. DPAPI binds to the app's
user context, so code running in the app's process (a guest game) can unprotect it too.
`gh_web.h` carries its own portable SHA-256 so the server's core builds and tests on Linux.

## What Greenhouse never does

- **Log credentials.** No sign-in token, no QR sign-in URL, no session token, no pairing code. The
  web server's log records method, a truncated path and status, never tokens or codes. A release
  build (`GH_RELEASE`) writes no diagnostic log to disk at all.
- **Send compatibility reports.** `gh_compat.h` reads a dataset that flows from the repository to
  the console; nothing tells a server which games were launched. The only upload is the crash
  report, per crash, after consent.
- **Accept inbound connections from the internet.** The LAN server answers private source
  addresses only; the relay is outbound polling. (A guest game that hosts a multiplayer session
  listens with the app's network capabilities; that is the game's code, not Greenhouse's.)
- **Host mods.** Mods come from Thunderstore to the console directly.
- **Bypass DRM or anti-cheat.** A protected game is recorded as `WONT_RUN` with a `wall` and left
  alone ([reference/known-limits.md](reference/known-limits.md)).
- **Redistribute Valve's runtime.** The repository carries hashes; the console fetches the bytes.

## Known weaknesses

- **One process, one set of rights.** Any game, mod, add-on or uploaded executable can read the
  saved Steam sign-in, the saves, the paired-browser and relay state, and the whole
  `DevelopmentFiles` volume, and can use the network. There is no isolation between guests and the
  launcher, and none is possible in this design.
- **The sign-in is stored in plain text.** Anyone with Device Portal access, and any code in the
  process, can read it ([steam/sign-in.md](steam/sign-in.md#what-is-saved-on-the-console)).
- **Dev Mode is the platform.** Device Portal can read and replace any of the app's files. Adding
  a Device Portal password is the user's main lever; Greenhouse cannot enforce it.
- **The Steam downloader turns off TLS certificate validation** for the CM server list and for
  CDN requests (search `SECURITY_FLAG_IGNORE_UNKNOWN_CA` in `corpus/downloader/cm_downloader.h`),
  does not check the signature of a depot manifest (`parse_manifest`), and does not refuse `..`
  in depot file names before writing. What limits a network attacker today: the CM session
  key is encrypted to a Valve public key compiled into the downloader, so a substituted server
  cannot read the sign-in, and chunk contents are encrypted with a depot key that arrives over
  that session and are checked against the manifest's checksums. This path has not been reviewed
  as a security boundary; enabling certificate checks and validating depot paths is open work.
- **The LAN page is plain HTTP.** Sniffable on the home network, as above.
- **The crash ingest credential is public.** It is in the binary; see [Crash reports](#crash-reports).
- **Fonts and images are decoded by system libraries** in the process (themes, cover art).
- **No over-the-air key revocation.** Trusted repository keys are compiled in. Replacing a
  compromised key needs a new MSIX; until then a holder of the old key can sign indexes the
  console accepts (subject to the rollback serial).
- **The rollback serial is per console, not per repository**, and is cleared by uninstalling the
  app.
- **Development builds are weaker by design.** Without `GH_RELEASE` the loader writes the
  diagnostic log to disk (it can contain the signed-in account name, never the token), runs the
  `gh_cmd.txt` command channel that can install and uninstall games (`LuiCmdStart`), and may trust
  a development repository key. Do not distribute a development build. See
  [building.md](building.md#release-builds-versus-dev-builds).

## Reporting a vulnerability

If the repository has private vulnerability reporting enabled, open a GitHub security advisory on
the repository ("Security" tab, "Report a vulnerability"). That keeps the details private until a
fix ships.

If it is not enabled, open an ordinary issue that says a security problem exists and which
component it is in (for example "package repository" or "LAN web server"), **without** exploit
detail, proof-of-concept code or reproduction steps. A maintainer will arrange a private channel.

In either case, never include a sign-in token, a QR code or challenge URL, a Device Portal
password, a session token, a key, or a log that has not been scrubbed. If a credential of yours
was exposed, sign out on the console (which deletes the saved sign-in) and revoke the session from
your Steam account.

## Testing

Run the tests for the boundary you touched ([contributing/testing.md](contributing/testing.md)).

| Boundary | Test | What it proves |
|---|---|---|
| Package repository, themes | `toolchain/test_gh_repo.sh` (Wine) | Parser and path rules; good, tampered and unknown-key signatures; a tampered index or file installs nothing; a lower serial is refused; encrypted packages refused; theme allow-list (a theme carrying `addon.dll` is refused) and `CheckThemeBytes`; the vendor `from` fetch (network; `GH_REPO_TEST_VENDOR=0` skips it) |
| JSON reader | `toolchain/test_gh_json.cpp` (`g++`, command in its header) | The parser under ASan/UBSan; with file arguments, validates title configs |
| Zip, inflate | `toolchain/test_gh_zip.sh`, `toolchain/test_gh_inflate.sh` | Unsafe-path rejection in the zip reader; the inflater |
| Mods | `toolchain/test_gh_mods.sh`, `toolchain/test_gh_ovl.sh` (Wine) | `..` in mod file lists and profiles refused; profile code validation; the loose-file overlay |
| LAN web server, uploaded games | `toolchain/test_gh_web.sh` | Each gate (public peer, DNS-name `Host`, wrong or missing `Origin`, `OPTIONS`); `ConstEq`; `SafeRel`; PE inspection; the whole pair, upload, finish, play, delete flow, under ASan/UBSan |
| Remote relay | `toolchain/test_gh_remote.sh` (Wine) | Pairing MAC, six-digit code, key derivation, envelopes and passkey-signed commands agree with the site's WebCrypto vectors; tampered, wrong-direction and wrong-phone messages refused; sequence counters persist. Runs whether or not the relay is compiled into the launcher |
| Crash report scrub | `toolchain/test_gh_crash.cpp` (`g++`, command in its header) | Planted secrets are removed and ordinary diagnostic text survives; run against a pulled diag, nothing secret-shaped gets through |
| Ownership list | `toolchain/test_gh_owned.cpp` (Wine) | The owned-games list parser, including junk lines and a user's own game |
| Release packaging | `toolchain/iterate-launcher.sh` with `GH_RELEASE=1` | Refuses a development key, an empty release key table, or a Valve runtime file in the stage |

Not covered by a host test: the compatibility-package allow-list (`IsCompatPath`, the one-file
rule), add-on hash-on-load (`LuiAddonsLoad`), the title-config field rules in `GhTitleFromJson`,
the Steam add-on and downloader, and the TV-side pairing modals. Those run only on a console; say
so when you change them.
