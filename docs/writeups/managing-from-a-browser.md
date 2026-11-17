# Your own games, and managing the console from a phone

What this covers: how the launcher got a small web server on the home network so a PC browser can add a game folder and run it on the TV (§65), and how the same idea was carried off the home network through an encrypted mailbox that a phone signs every change for (§66). It covers the security reasoning, the browser behaviour that decided the design, two loader bugs that only a user's own exe could expose, and what is and is not in today's open-source tree.
Where the code is: `src/loader/gh_web.h` (`Gate`, the server), `src/loader/gh_custom.h` (`SafeRel`, `InspectExe`, `kPairSeconds`, `kKeepFreeBytes`), the web and remote sections of `src/loader/launcher_ui.h` (`LuiWebLanAddr`, `LuiWebPairModal`, `LuiRemoteCheck`, `LuiRemoteTurnOff`), `src/loader/gh_remote.h` (`VerifyPair`, `DeriveKeys`, `VerifyAssertion`, `CmdChallenge`, `kOps`), `corpus/web/console.html`, `toolchain/ghweb_host.sh`, `toolchain/test_gh_web.sh`, `toolchain/test_gh_remote.sh`. In `guest_loader.cpp`: `g_customRun`, `GhCustomSteamBase`, `GhCustomSteamLoad`, `GhClassRegisterA`, `GhClassSelect`.
Related: [launcher/web-manage.md](../launcher/web-manage.md) · [launcher/remote-manage.md](../launcher/remote-manage.md) · [launcher/library.md](../launcher/library.md) · [security.md](../security.md) · [architecture/window-input.md](../architecture/window-input.md) · [architecture/filesystem-registry.md](../architecture/filesystem-registry.md)
Period: 2026-11-11 to 2026-11-12, Launcher 0.0.948.0–0.0.955.0, Steam add-on 1.0.17.0. Notebook sections: §65–§66 (and the §67 regression the ANSI-class fix caused).

## Summary

- "Manage from a browser" is an opt-in HTTP server inside the launcher, on TCP port 8880 (or the next free port up to 8884). A paired PC browser can upload a game folder, pick its exe, run it on the TV, read its last log and delete it. It is off by default and stays on until switched off.
- The security model is aimed at a hostile web page open in the user's own browser, not at a neighbour guessing a code. `Gate()` refuses non-private peers, refuses any `Host` that is not an IPv4 literal (which defeats DNS rebinding), requires `Origin` to match on every write, and the session travels in a custom header, which would need a CORS preflight the server never grants.
- Pairing is a 4-digit code shown in the browser and on the TV; it proves the browser in front of you is the one asking, and A on the controller is the approval.
- An uploaded game gets no Steam from Greenhouse: no title config, no Steam art, no Steam runtime. If it ships its own Steam DLLs they load from its folder, as on a PC with no Steam client.
- §65 also had the console announce its private address to the project's site so a browser could find it without typing. That announce rode on a per-console credential that no longer exists, and it is gone from the open-source tree: today you type the address shown on the TV or scan its QR code, and the console tells nobody that the server is running.
- "Manage from anywhere" (§66) carries library management off the home network: the console only polls an encrypted mailbox outward, messages are AES-256-GCM under keys from ECDH and HKDF, every change is signed by the phone's passkey and the console verifies the WebAuthn assertion itself, and the command list is closed. It worked end to end in November 2026, but **it is compiled out by default** (`/DGH_REMOTE_RELAY`): the relay authenticated consoles with a per-console credential that no longer exists, and no replacement has been designed.
- Two loader bugs came out of running arbitrary user exes: `fs::relative` fails inside the app container, and the x64 window bed ignored ANSI window classes.

## The problem

Every game the launcher ran before §65 came through Steam: an owned app id, a depot download, a title config. The ask was to let a user run an `.exe` that did not come through Steam, with three constraints:

1. Nobody should have to type a LAN IP address on a TV keyboard or remember one.
2. The project's servers must not carry the game data. A folder of several gigabytes goes from the PC to the console directly.
3. Compatibility is not promised. A user's own game is "unverified", but crash reports from it are welcome.

Two product decisions were fixed before any code: a folder that looks like a Steam game (a `steam_appid.txt`, a `steam_api*.dll`) still runs as a plain custom game with no Steam, and the browser feature is a setting, off by default, that persists until it is turned off.

The hard part is that the server runs inside the launcher process, which is also the game process, and is reachable by anything on the home network, including any web page the user has open. The design had to make that safe without TLS, because a console on a home network has no name a certificate could be issued for.

§66 then asked the same question without the home network: manage installs from a phone away from home, with no VPN and no port forwarding, while the console still accepts no inbound connection.

## The investigation

### A server small on purpose

The server (`gh_web.h`) is HTTP/1.1, one request per connection, at most 4 connections (`kMaxConns`) with a thread each, a 16 KiB request head, a mandatory `Content-Length` and no chunked bodies, a 256 KiB streaming buffer that writes uploads straight to disk, and 30 s socket timeouts. Each limit exists because a stalled or hostile browser must not be able to pin threads or memory in a process that may be running a game. The same header builds on Linux with BSD sockets, and with `GH_WEB_CORE_ONLY` it builds only the parser, the gates and SHA-256 for the host test.

Uploads are one resumable `PUT .../files?path=<rel>&offset=<n>` per file into a staging folder under `D:\DevelopmentFiles\gh_custom\`, and `finish` moves the folder into the library with a `gh_custom.json` manifest. An `offset` that does not equal the size already stored is refused with 409 and the size the console has, so a browser that lost its place resumes from the console's count, not its own. The store always keeps 1 GiB free (`kKeepFreeBytes`); past that the answer is 507.

Every path from the browser goes through `SafeRel`, which applies the zip extractor's `ghzip::SafeName` rules: relative only, no `..`, no drive or device names, length and count limits. `InspectExe` reads each candidate PE and sends x64 to the main path and x86 to the [x86 track](../architecture/x86-guest.md). It refuses ARM64, 16-bit, .NET-only images and installers with a reason written for the user ("an installer -- install the game on your PC, then add the installed folder").

### Gates instead of TLS

The threat considered first was not someone on the LAN guessing a code; anyone on the LAN can ask to pair, and approval still needs the controller. The threat is a page on the internet, open in a browser on the same network, sending requests to the console. That page can make the browser send requests to a private address, and with DNS rebinding it can make those requests look same-origin to the page.

`Gate()` runs before any handler:

1. The peer must be RFC 1918, link-local or loopback IPv4. Anything routed from outside gets 403.
2. `Host` must be an IPv4 literal; a DNS name gets 421. A rebinding attack depends on the browser sending the attacker's hostname, and the attacker's page cannot change that, so refusing every hostname refuses every rebinding.
3. Any method other than GET or HEAD must carry `Origin` equal to `http://` plus `Host`. A form post or fetch from another site carries that site's origin and gets 403.
4. The session token travels in `X-GH-Session`, a custom header. A cross-origin request with a custom header needs a CORS preflight, and `OPTIONS` is always refused, so no other page can attach a session even if it had one.

Responses add `no-store`, `nosniff`, `X-Frame-Options: DENY`, `no-referrer` and a Content-Security-Policy that lets the page connect only to itself. JSON output escapes `<` so a stored game name cannot close a script block.

### Pairing with a code

A new browser calls `POST /api/pair` and gets a pair id and a four-digit code. The TV shows the same code with the browser's label (derived from the User-Agent by `BrowserLabel`) and Allow/Deny (`LuiWebPairModal`). The code is not a secret that grants access; it lets the person at the TV check that the browser asking is the one in front of them. Three details came from thinking about how the prompt could be misused:

- A only counts after the prompt has been on screen for 500 ms, so a button press already in flight (someone mid-game on the wall) cannot approve a question nobody read.
- A request expires after 120 s (`kPairSeconds`), only one can be pending, and a second within 5 s is refused with 429, so another device cannot keep the TV covered in prompts.
- On approval the browser receives a 256-bit token exactly once. The console stores only its SHA-256 (`gh_web_sessions.json`), so the file is not a list of usable sessions. At most 8 browsers are remembered (`kMaxSessions`).

### Finding the console without typing (built, then removed)

The first constraint, no typing, produced the one piece that is no longer in the tree. While the switch was on, the launcher posted the console's private address and port to the project's site at start, when the address changed, and every 6 hours. The site accepted only RFC 1918 IPv4 and ports 1024–65535, so it could never be made to redirect a browser somewhere public, and a link-local address was never sent. A page on the site then redirected a signed-in browser to `http://<console-ip>:8880/` when it had seen one console recently, or listed several.

This depended on one unknown, labelled C0 in the notebook: would Chrome's Local Network Access protection block a public HTTPS page from sending the user to a private address? Measured in Chrome 154:

| From a public HTTPS page to `http://<console-ip>:8880/` | Local Network Access prompt |
|---|---|
| A clicked link | none; it navigated |
| Script setting `location.href` | none; it navigated |
| A cross-origin `fetch` | prompted (the prompt was closed, never granted) |

So the design rule was: the site navigates and never fetches. The page the browser lands on is served by the console itself and talks only to its own origin.

The announce worked against the site on 0.0.950.0 (`WEB announce on ok`, then `off ok`). It rode on a per-console credential that no longer exists, and the open-source release removed it together with that credential. The `LuiWebLanAddr` routine that found the address remains: it opens a UDP socket toward a public address without sending anything and reads which local interface the stack would use, keeping only an RFC 1918 result. Today the TV shows that address as text and as a QR code, the user types or scans it, and nothing about the server is sent to the site or anywhere else. A comment at the top of `gh_web.h` still mentions "a redirect to this address"; that is a leftover from this design.

### Running a game that is not a Steam game

A custom run sets `g_customRun`. Under it:

- no title config is matched by exe name, because nothing known about a title is true of an arbitrary exe with the same name;
- no Steam art is fetched; the library row gets a plate;
- the Steam preload is skipped (`STAGE=preload_steam_api64 skipped -- your own game, no Steam`);
- a request for any of the Steam modules (`GhCustomSteamBase` lists the API shim, the client engine and its companions) never resolves to the launcher's own copies. `GhCustomSteamLoad` loads the game's own copy by full path from its folder if it ships one, and otherwise answers "not found", as Windows would on a PC with no Steam client.

The second rule exists because the launcher's Steam modules are the signed-in session's front door. An uploaded exe runs in the same process; handing it the session would let any upload act as the user's Steam account.

"Play on TV" does not launch from the server thread. It leaves a request that the wall's loop picks up, and it is refused unless the wall has drawn within the last 1.5 s, so a browser cannot start a game while the TV shows some other page. The title screen says "Unverified — your own file. Greenhouse has not tested it, and it runs without Steam." Crash reports from custom games are filed under the exe's base name and SHA-256, never the name the user typed, through the same scrub as every other log.

### Two loader bugs only a user's own exe could find

The test fixtures for §65 were four tiny programs built from source in the tree: one that exits with code 7 and one that crashes, each as x64 and x86. They were deliberately unlike any Steam game, and they found two bugs that years of Steam titles had not.

**`fs::relative` fails in the app container.** On the first console run, every upload's `finish` answered "no .exe" while the upload listing showed the files. MSVC's `fs::relative` goes through `weakly_canonical`, which calls `GetFinalPathNameByHandleW`, and the container refuses that on `D:\DevelopmentFiles`. Directory iteration and `file_size` work, so the failure was silent: an `error_code` set, an empty result, no exe. The Linux host test could not catch it because canonicalisation works there. Fixed in 0.0.949.0 with `path.lexically_relative(base)`, which needs no OS call since the iterator's paths already start with the base. The rule is now in the [container API gaps table](../architecture/filesystem-registry.md).

**The x64 window bed ignored ANSI window classes.** The `ok` x64 fixture died at once with 0xC0000005 right after the activation burst. The window bed captured only `RegisterClassW` and `RegisterClassExW`; `RegisterClassA` was not bedded at all, and `Bed_RegisterClassExA` dropped the procedure. So `CreateWindowExA` found no class, and the bed kept whatever procedure it already had: one belonging to a window class the launcher's own Steam engine registers at boot. By the time a guest ran, that procedure's page was no longer executable, and the burst jumped into it. Every Steam title tested so far had used wide classes.

Fixed in 0.0.950.0 in two parts:

- `Bed_RegisterClassA` and `Bed_RegisterClassExA` capture the class through `GhClassRegisterA`, byte-widening the name the same way `Bed_CreateWindowExA` widens the one it looks up.
- `GhClassSelect` checks a leftover procedure with `VirtualQuery` and drops it if its page is not executable, logging `has no registered proc -- DROPPING non-executable`.

The fix had a consequence found a day later (§67). Garry's Mod creates a hidden zero-size child window with an ANSI class after its game window. Before 0.0.950.0 that class had no procedure on record, so it changed nothing; once ANSI classes were captured, the child took over the game's window procedure, and every click went to a window that only handles one message. The class fix for that was `GhHelperWindow`: a child or message-only window created after the game window never takes the game's procedure.

Two smaller fixes came from the same runs: x86 fault text no longer doubles ("FAULT FAULT ... at ... at"), and a game that ran and then crashed now says GAME CRASHED instead of LAUNCH FAILED.

### Verified on the console (0.0.950.0)

Only the four in-tree fixtures were uploaded; no third-party game was.

| Check | Result |
|---|---|
| Switch survives an update and a restart | `WEB listening on port 8880` with no input; the browser session still valid |
| Uploads | x64 and x86 detected; 4 games added |
| Installer | 422 with the installer message |
| Six traversal or unsafe paths | 422 each |
| No session / foreign `Origin` / hostname `Host` | 401 / 403 / 421 |
| Upload while a game runs | 409 |
| `ok` x64 and x86 via Play on TV | exit 7, SESSION ENDED |
| `crash` x64 | 0xC0000005, GAME CRASHED, `lastRun.crashed=true` |
| `crash` x86 | GAME CRASHED, one FAULT line |
| Library picker | custom rows at the end of Installed, plates, the Unverified line; launch from the picker exits 7 |
| Steam regression (a Unity title) | window class captured, 3,056 frames, main menu, no 0x887A0006 |
| Cleanup | 6 deletes answered 200, session forgotten, switch off, `gh_web_on.txt` gone |

### Off the home network: an outbound-only mailbox

§65 works only on the LAN. §66 kept the property that nothing connects to the console: the console polls one endpoint on a relay (`POST /api/remote/poll`) and the relay is a mailbox for small encrypted envelopes. Game data still comes straight from Steam's CDN; the relay only carries commands and status. The relay's suggested wait is clamped by the console to between 2 s and 15 minutes, and after a command the console polls again within 2 s to return the result.

The design started from what the relay should be unable to do, and every mechanism maps to one item:

| The relay must not be able to | Mechanism |
|---|---|
| Read a command or a status snapshot | AES-256-GCM under keys only the console and phone hold |
| Forge or alter a command | GCM tag over the ciphertext, plus a passkey signature the relay cannot produce even with a message key |
| Replay a command | Sequence numbers only move forward, and the signature covers the sequence |
| Pair a device of its own | The pairing secret is in a URL fragment it never sees, and both screens compare a code derived from both keys |
| Make the console do anything else | A closed command list, re-checked by the launcher |

### Pairing in front of the TV

The TV shows a QR code of a URL on the site whose secret part, a 16-byte token valid for 5 minutes (`kPairTtlS`), is in the URL fragment. Browsers do not send fragments to servers, so the relay learns only SHA-256 of the token, which is enough to know a pairing window is open. The phone generates an ECDH key and a new passkey and answers with both public keys and an HMAC over the pairing transcript, keyed with the token. `VerifyPair` checks the phone id against its key, the clock (600 s), the MAC in constant time, and that both public keys are valid P-256 points.

Both screens then show a six-digit code (`Sas`) derived from the console's key, the phone's ECDH key and its passkey key. If the relay had swapped keys in transit, the codes would differ. Only A on the TV completes pairing, after the same 500 ms delay as the LAN prompt, and the token is wiped either way, so a QR code is single-use. At most 5 phones (`kMaxPhones`) are paired. The QR URL is a short-lived credential and is never logged.

### Keys, envelopes, replay and clocks

Each console/phone pair derives two AES-256 keys, one per direction (`DeriveKeys`): the ECDH shared secret through HKDF-SHA256 with a versioned salt and an info string naming the direction, the console and the phone. A key for one direction cannot decrypt the other, and keys are derived when needed, not stored.

Every envelope is AES-256-GCM with a random 96-bit nonce. The direction, both identities, the sequence number and the timestamp are in the additional authenticated data, so changing any cleartext field makes decryption fail. On the console, `Session::OnCommand` refuses a sequence number not strictly greater than the last accepted from that phone, and a timestamp more than 10 minutes (`kSkewS`) from the console's clock. Sequence and sign counter advance only after every check passes. All base64url must be canonical (`B64In` re-encodes and compares), so one value has exactly one spelling and nothing downstream can be confused by a second one.

### WebAuthn verification in C++

Every command except `refresh` carries a WebAuthn assertion from the phone's passkey, and the console verifies it itself in `VerifyAssertion` instead of trusting the relay's word. The challenge (`CmdChallenge`) is a hash over the console, the phone, the sequence number, the op and its argument, so one signature authorises one command once. The verifier checks the relying-party id hash, that user presence and user verification are both set (biometric or device PIN on every change), the client data's type, exact origin, not cross-origin and not in a frame, the challenge, a strict-DER ES256 signature (`DerToRaw` rejects non-minimal encodings) and the sign counter. Synced passkeys always report a counter of zero, which is accepted; a counter that has ever moved must keep moving.

### A closed command list

`kOps` lists everything a phone can ask for: `refresh`, and `install`, `resume`, `verify`, `cancel` or `dequeue` of a Steam app id, and `uninstall` of an app id or one custom game. `ValidArg` accepts only a decimal app id with no leading zero, or `custom:` plus a 12-hex custom game id for uninstall. There is no op that launches a game, takes a path or arguments, reads or writes a file, or changes a setting, and none can be expressed in the format.

Authentication says only who asked. Whether the request makes sense is the launcher's decision (`LuiRemoteCheck`), made against what the wall last listed: an install must be owned and not installed, with Steam signed in and space free; an uninstall must be something the launcher manages and not the running game. Accepted changes wait in a queue of at most 32 that the wall runs one at a time when idle, never while a game runs, re-checking each before it runs. Cancel from the phone, or B on the TV, stops a download within about 2 seconds and leaves the folder resumable; that needed the downloader's cancel hook to be polled during the acquire phase too, in Steam add-on 1.0.17.0.

### Verified, then compiled out

Tested on Launcher 0.0.951.0 and later against a throwaway copy of the site: pairing with matching codes; a signed `verify`; refused: a replayed command, an install of an unowned game, a command with no passkey signature, and any command after Forget on the TV (page, relay and console each refused it); cancel from the phone and from B; the queue while a game runs; B denying the pairing prompt; switching off removing the key and phone files. `toolchain/test_gh_remote.sh` runs the console's crypto and WebAuthn verifier under wine against vectors the site generates with the browser's WebCrypto (`toolchain/vectors/remote_vectors.json`, throwaway keys), which proves the two implementations agree on MACs, codes, key derivation, envelopes and signatures, and that tampered inputs are refused.

It was then deployed against the project's site. At the open-source release the feature was compiled out: the relay authenticated each console's polls with a per-console credential that no longer exists, and a new console authentication scheme has not been designed. A build with `/DGH_REMOTE_RELAY` still compiles the whole protocol, but `Host::token` returns an empty string and the relay refuses its polls. In a default build there is no entry for it, no poller and no device id file; the wall's RB action reads "From a PC" and leads only to the LAN feature. The host test builds `gh_remote.h` regardless; the Launcher includes it only with the define. See the status box in [remote-manage.md](../launcher/remote-manage.md).

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| The console announces its private address to the site, and the site redirects the browser to it, so nobody types an address | Built and verified in §65 against the site (`WEB announce on ok`) | Superseded at the open-source release: the announce rode on a per-console credential that no longer exists and was removed with it. Today the TV shows the address and a QR code, and nothing is sent anywhere |
| "Manage from anywhere" is shipped and on in production | Deployed and paired end to end in §66 | Compiled out by default (`/DGH_REMOTE_RELAY`) for the same reason; `Host::token` is empty, and the relay refuses polls until a new scheme exists |
| An uploaded game's Steam DLL loads are simply refused | The §65 notebook entry describes a lazy Steam DLL load being refused for the run | The tree loads the game's own copy from its folder by full path if it has one (`GhCustomSteamLoad`) and answers "not found" only when it does not; what is always refused is the launcher's own Steam modules |
| The ANSI-class fix changed nothing for Steam titles, which use wide classes | Every Steam title tested used `RegisterClassW`/`ExW` | Garry's Mod's ANSI child window took over the game's procedure (§67); fixed with `GhHelperWindow` |

## What shipped

All present in the tree:

- `src/loader/gh_web.h`: the server, `Gate()`, the limits `kMaxConns`, `kMaxHead`, `kBufBytes`, `kTimeoutS`; `GH_WEB_CORE_ONLY` for host builds.
- `src/loader/gh_custom.h`: the custom-games store and API, `SafeRel`, `InspectExe`, `ListExes` (with `lexically_relative`), `BrowserLabel`, `ConstEq`, `kPairSeconds`, `kMaxSessions`, `kKeepFreeBytes`.
- `src/loader/launcher_ui.h`: `LauncherFromPc`, `LuiWebStart` / `LuiWebStop`, `LuiWebLanAddr`, `LuiWebPairModal`; behind `GH_REMOTE_RELAY`, `LuiRemoteCheck`, `LuiRemoteTurnOff`, `LuiRemoteSnapshot`.
- `src/loader/gh_remote.h`: `VerifyPair`, `Sas`, `DeriveKeys`, `CmdChallenge`, `VerifyAssertion`, `DerToRaw`, `B64In`, `kOps`, `kMaxPhones`, `kSkewS`, `kPairTtlS`, `kMaxCmdPt`, `kMaxSnapPt`, `Host::token` (empty).
- `src/loader/guest_loader.cpp`: `g_customRun`, `GhCustomSteamBase`, `GhCustomSteamLoad`, the `preload_steam_api64 skipped` stage line, `Bed_RegisterClassA`, `GhClassRegisterA`, the executable-page check in `GhClassSelect`, `GhHelperWindow`.
- `corpus/web/console.html`: the page, one self-contained file.
- `toolchain/ghweb_host.sh` (serves the real API on loopback with the TV half stubbed), `toolchain/test_gh_web.sh` (ASan and UBSan over parser, gates, paths, PE inspection and the whole flow), `toolchain/test_gh_remote.sh` with `toolchain/vectors/remote_vectors.json`.
- Settings files in `LocalState`: `gh_web_on.txt` (the switch), `gh_web_sessions.json` (hashed sessions); with the define, `gh_remote_on.txt` and the `gh_remote\` folder.

Reference: [web-manage.md](../launcher/web-manage.md) for endpoints and limits, [remote-manage.md](../launcher/remote-manage.md) for the full protocol, [security.md](../security.md) for how both fit the project's overall threat model.

## Still open

- **A console authentication scheme for the relay.** Until one is designed (and the site speaks it), "Manage from anywhere" stays compiled out. The rest of the protocol does not need to change.
- **Finding the console without typing**, now that the announce is gone. Any replacement should keep the C0 result in mind: navigate from a public page to the private address, never fetch it.
- **Accepted limits of the LAN feature**: plain HTTP, so anyone who can sniff the home network can read uploads and capture a session token; no IPv6; works only while the launcher is open on the TV.
- **A custom game runs in the launcher's own process** with the same file access as the launcher. The app container is the only boundary.
- Remote install and remote uninstall of a custom game were not exercised on the production relay before it was compiled out.
- Two stale comments: the top of `gh_web.h` still mentions a site redirect, and `LuiWebLanAddr` says "the site refuses anything else".

## Lessons

- Defend a LAN service against the user's own browser first. A web page open on the same network is the realistic attacker, and Host, Origin and a custom header stop it without TLS.
- Refuse every `Host` that is not an IP literal. DNS rebinding depends on the attacker's hostname reaching the server, and the attacker's page cannot change it.
- From a public page, navigate to a private address and never fetch it. Chrome's Local Network Access prompted for the fetch and let both kinds of navigation through.
- Make an approval prompt ignore input for its first 500 ms. A button press already in flight should never answer a question nobody has read.
- Never use `fs::relative`, `fs::canonical` or `weakly_canonical` on console paths. They canonicalise through an API the container refuses, and fail silently.
- Test with fixtures unlike your corpus. Four tiny in-tree exes found two loader bugs that every Steam title had walked past.
- When a shim starts recording something it used to ignore, look for who benefited from it being ignored. Capturing ANSI classes gave a hidden helper window the game's procedure.
- An uploaded program must never be handed the signed-in session's modules. It runs in the same process, so whatever it can load, it can use as the user.
- Let a relay carry only ciphertext and let the endpoint verify the user's signature itself. Then a compromised relay can delay or drop messages but cannot act.
- Make the remote command list closed and unable to express a path, a launch or a setting. Authentication says who asked; it cannot make a dangerous verb safe.
- Do not ship a feature whose authentication rests on a credential that is going away. Compile it out and say so, rather than leaving a switch that cannot work.
