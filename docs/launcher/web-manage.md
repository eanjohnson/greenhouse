# Manage from a browser

What this covers: the small web server the launcher can run on the home network so a PC browser can add, run and remove the user's own games; its endpoints, pairing, and security model.
Where the code is: `src/loader/gh_web.h` (server), `src/loader/gh_custom.h` (store and API), the web section of `src/loader/launcher_ui.h`, the page `corpus/web/console.html`, and the host tools `toolchain/ghweb_host.*` and `toolchain/test_gh_web.*`.
How it was found: [managing-from-a-browser.md](../writeups/managing-from-a-browser.md)

## What it is

An off-by-default setting. When on, the launcher listens on TCP port 8880 (or the next free port up to 8884) and serves one HTML page. From that page a paired browser can:

- upload a game folder, resumably;
- pick which exe to run and set a name and arguments;
- start the game on the TV;
- read the last run's log and send it as a crash report;
- delete a game it added.

It cannot touch Steam titles, installed packages, settings, or any file outside the custom-games store. What it creates shows up in the library as custom rows; see [library.md](library.md).

The server runs inside the launcher process, which is also the game process, so it is deliberately small (`gh_web.h`):

| Limit | Value |
|---|---|
| Concurrent connections (`kMaxConns`) | 4, one thread each, one request per connection |
| Request head (`kMaxHead`) | 16 KiB |
| Bodies | `Content-Length` required; no chunked encoding |
| Streaming buffer (`kBufBytes`) | 256 KiB; uploads go straight to disk |
| Socket timeout (`kTimeoutS`) | 30 s |

It is plain HTTP. There is no TLS because a console on a home network has no name a certificate could be issued for.

## Turning it on

RB on the wall opens "From a PC" (`LauncherFromPc`). In a build with `GH_REMOTE_RELAY` the same action is labelled "PC & phone" and the page also offers Y for [Manage from anywhere](remote-manage.md); a default build has neither. A toggles the server (`LuiWebStart` / `LuiWebStop`); the choice persists as `LocalState\gh_web_on.txt`. The page shows the address as text and as a QR code. X forgets all paired browsers.

The address shown is the console's private IPv4 address, found by `LuiWebLanAddr` (it asks the network stack which interface it would use to reach the internet, without sending anything). If the console has no RFC 1918 address the page says so.

The console tells nobody else that the server is running: nothing about it is sent to the project's site or anywhere else. Type the address shown on the TV, or scan its QR code, from a browser on the same network. The feature needs no account and no site.

The server only works while the launcher is open on the TV. While a game runs, every request that would change something answers 409.

## Pairing

A browser must be approved on the TV once.

1. The page calls `POST /api/pair`. The console makes a random pair id and a four-digit code and returns both; the page displays the code.
2. The TV shows a modal (`LuiWebPairModal`): the browser's label (for example "Chrome on Windows", derived from the User-Agent by `BrowserLabel`) and the same code. A allows, B denies.
3. The page polls `GET /api/pair/<pairId>`. On "allowed" the response carries a session token, exactly once.

Details that matter:

- The code is not a secret that grants access. It lets the person at the TV confirm that the browser asking is the one in front of them.
- A on the TV only counts after the question has been on screen for 500 ms, so a button press already in flight cannot approve a question nobody read.
- A pending request expires after 120 s (`kPairSeconds`). Only one can be pending; a new request within 5 s of the last is refused with 429.
- The token is 256 random bits. The console stores only its SHA-256 (`LocalState\gh_web_sessions.json`), so the file cannot be replayed as a session. At most 8 browsers are remembered (`kMaxSessions`); the least recently seen is dropped.
- The page keeps the token in the browser's local storage and sends it as the `X-GH-Session` header.

## Endpoints

All under the console's own origin. Everything except the page and the two pairing calls needs a valid `X-GH-Session`; otherwise 401. `POST` bodies must be `application/json`. Errors are `{"error":"<plain sentence>"}`.

| Method and path | Purpose |
|---|---|
| `GET /` | The page (`console.html`), with a restrictive Content-Security-Policy |
| `POST /api/pair` | Start pairing; returns `pairId`, `code`, `expires` |
| `GET /api/pair/<pairId>` | Poll; `state` is `pending`, `allowed` (with `token`), `denied` or `expired` |
| `GET /api/status` | `running`, `free` bytes, `games` count, `version` |
| `POST /api/session/forget` | Forget this browser |
| `GET /api/games` | Installed custom games and uploads in progress |
| `POST /api/games` | `{name}`; start an upload, returns a 12-hex `id` |
| `GET /api/games/<id>/files` | Sizes of files received so far, for resuming |
| `PUT /api/games/<id>/files?path=<rel>&offset=<n>` | Append bytes to one file. `offset` must equal the size already stored, else 409 with the size the console has. |
| `GET /api/games/<id>/exes` | Candidate executables with architecture, whether each is runnable and why not, and the default pick |
| `POST /api/games/<id>/finish` | `{exe}`; move the upload into the library |
| `POST /api/games/<id>` | Edit `name`, `args`, `exe` |
| `DELETE /api/games/<id>?confirm=<id>` | Delete; the id must be repeated |
| `POST /api/games/<id>/play` | Ask the TV to start it (202). Refused unless the TV is on the library wall. |
| `GET /api/games/<id>/log` | Scrubbed tail of the last run's log |
| `POST /api/games/<id>/report` | Send that log as a crash report to the build's site (`POST <site>/api/crash-report`, see `gh_site.h`). A build with no site answers that none is configured |

Status codes used with specific meaning: 409 (a game is running, or a file is already being sent, or files are still arriving), 422 (a name, path or exe the console will not accept), 507 (out of space; the store always keeps 1 GiB free, `kKeepFreeBytes`).

"Play" does not launch from the server thread. It leaves a request that the wall's loop picks up, and it is refused if the wall has not drawn within the last 1.5 s, so a browser cannot start a game while the TV is on some other page.

## Security model

The threat is not someone on the home network guessing a code; it is a web page on the internet, open in the user's browser, trying to reach a device on the LAN. `Gate()` in `gh_web.h` runs before any handler:

1. **Private peers only.** The connecting address must be RFC 1918, link-local or loopback IPv4. Anything else gets 403.
2. **Host must be an IPv4 literal.** A request whose `Host` is a DNS name gets 421. This defeats DNS rebinding: an attacker's page can only ever send its own hostname.
3. **Same-origin writes.** Any method other than GET/HEAD must carry `Origin` equal to `http://` + `Host`. A form or fetch from another site carries that site's origin and gets 403.
4. **No CORS.** `OPTIONS` is always refused. Since the session travels in a custom header, a cross-origin request would need a preflight, and never gets one.

On top of that:

- Responses carry `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`. The page's CSP allows only inline script and style, `data:` images, and connections to itself.
- JSON output escapes `<` so a stored name cannot close a script block.
- Upload paths go through `SafeRel`: relative only, no `..`, no drive or device names, at most 200 characters, at most 100000 files per game. Names and arguments are cleaned to single-line text with length limits.
- Token comparison is constant time (`ConstEq`).
- Uploaded executables are inspected (`InspectExe`) but not trusted. They run in the same app container as any other guest and get no title-specific treatment and no Steam runtime.
- Crash reports from custom games are filed under the exe's base name and hash, never the name the user typed, and the log is scrubbed before it is stored or shown.
- The diagnostic log records method, a truncated path and status for changes and refusals. It never records tokens or codes.

Known limits, stated plainly:

- Traffic is unencrypted on the LAN. Anyone who can sniff the home network can read uploads and capture a session token.
- Anyone on the LAN can request pairing; approval still requires someone holding the controller.
- IPv6 is not served.

## Working on it off-console

Two host tools build the same `gh_web.h` and `gh_custom.h` on Linux:

- `toolchain/ghweb_host.sh [root-dir] [port]` serves the real API on 127.0.0.1 with the TV's half stubbed, for developing `console.html` in a real browser. Pairing auto-approves after 1.5 s; "Play" pretends a game ran for 8 s and records a fake crashed run; creating `<root>/.running` makes the API answer as if a game were running. The page is re-read from disk on every request.
- `toolchain/test_gh_web.sh` builds `test_gh_web.cpp` with `-DGH_WEB_CORE_ONLY` under ASan and UBSan and exercises the request parser, the gates, SHA-256, path rules, PE inspection, and the whole pair, upload, finish, play, delete flow.

Both scripts find the repository relative to themselves. They need `g++` with C++20.

When changing an endpoint, change `gh_custom.h`, `console.html` and `test_gh_web.cpp` together.
