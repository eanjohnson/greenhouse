# Steam sign-in

What this covers: connecting to Steam, the QR sign-in flow, and what is saved on the console afterwards.
Where the code is: `QrSignIn` and `SignIn` in `corpus/addons/steam/steam_addon.cpp`; `ghdl::CmClient` in
`corpus/downloader/cm_downloader.h`.

## Connecting to a CM server

`CmClient::connect_and_secure()`:

1. Fetches the server list from Steam's public Web API (`ISteamDirectory/GetCMList/v1/`).
2. Opens a TCP connection to the first server. Messages are length-prefixed with a `VT01` magic.
3. Completes the channel-encryption handshake. The client generates a session key, sends it
   RSA-OAEP-encrypted to Steam's public key, and from then on every message is AES-256-CBC encrypted
   with an HMAC-derived IV.

Socket receives time out after 25 seconds. Request/response calls go through `um(method, body, resp,
&eresult)`, which sends a unified service method, reads past unrelated messages (there are many right
after logon), matches the reply by job id, and gives up after 30 seconds.

Protobuf messages are encoded and decoded by a small hand-written codec in the same header; there is no
generated code and no `.proto` dependency.

## QR sign-in

There is no password entry anywhere in Greenhouse. The user approves the sign-in in the Steam mobile app.

1. `login_anon()` logs the connection on anonymously.
2. `qr_begin("Greenhouse Xbox")` calls `Authentication.BeginAuthSessionViaQR#1`. The reply holds a
   client id, a request id, a challenge URL and a poll interval.
3. `QrMatrix` (qrcodegen) turns the challenge URL into a module matrix. The add-on passes it to the
   host's `signInScreen(title, modules, size, status, hint)`, and the launcher draws it. The add-on
   never draws anything itself.
4. `qr_poll` calls `Authentication.PollAuthSessionStatus#1` at the interval the server asked for.
5. On approval the poll reply carries the account name and a refresh token. The add-on reads the
   SteamID out of the token's `sub` claim (`JwtSubject`), saves the credential, and fetches the library.

### Rotation

Steam replaces the challenge about every 30 to 45 seconds. A poll reply may contain a new client id and
a new challenge URL; when it does, the add-on keeps the request id, adopts the new client id, and redraws
the QR code. A client that keeps showing the first code will show one that no longer works.

### Limits

- The add-on tries up to five sessions and stops after 600 seconds in total.
- The status line under the code reports progress (waiting, scanned, approved, or an offline message).

### QR codes and challenge URLs are credentials

A challenge URL is a short-lived credential: anyone who scans it and approves signs **their** phone's
account in on the console, and a photo of a live code is enough to do it. Treat screenshots, video
captures and log lines containing a challenge URL as sensitive. Do not paste them into issues.

## Sign-in is not required

`signInScreen` returns 1 when the user presses B, and the add-on stops. The account label becomes
"not signed in · skipped". The launcher still lists installed games and they still start. What is lost:

- the owned-library rows and downloads;
- a Steam session for the game. A game that checks ownership through Steam will refuse to run.

If Steam cannot be reached the label ends in "· offline" and the cached library is used.

## What is saved on the console

| File (in the app's `LocalState`) | Contents | Written by |
| --- | --- | --- |
| `gh_cred.txt` | Three lines: account name, SteamID, refresh token | `CredWrite` |
| `gh_owned.txt` | Cached owned-games list (see [library-and-downloads.md](library-and-downloads.md)) | `FetchLibrary` |

Facts to keep in mind:

- The token is stored as plain text. This was a deliberate trade-off so that the user is not asked to
  scan a code at every launch. Anyone with Device Portal access to the console can read the file, which
  on a console in Developer Mode means anyone who has its Device Portal credentials.
- The token is a secret equivalent to being signed in. It must never leave the device: do not log it,
  do not copy `gh_cred.txt` into a bug report, and do not write tooling that pulls it off the console.
  If you are collecting `LocalState` for diagnostics, exclude this file.
- In memory the add-on wipes token buffers with `SecureZeroMemory` (`Scrub`) once they have been used.
- Log lines from the add-on start with `ADDON steam:` and never contain the token.

## Later launches

`SignIn` reads `gh_cred.txt` and calls `FetchLibrary`, which logs on with `login_token(sid, token,
account)`. The account name must be sent along with the token, otherwise Steam grants a session too
limited for library and download calls.

| Result | What happens |
| --- | --- |
| Logon accepted | Label "signed in · <account>"; no QR code shown |
| Token refused (`-2`) | The QR flow runs again |
| No network | Label ends "· offline"; cached library is used |

## Signing out

`SignOut` deletes `gh_cred.txt` and `gh_owned.txt`. The launcher only calls it after the user confirms
on the Packages page. Installed games are not touched.

## The engine's own logon

The add-on's CM connection is used for sign-in, library, downloads and Workshop. A running game is
served separately: with the engine build of the shim, Valve's client runtime makes its own connection
using the same saved credential. The loader starts it only when an installed source provides
`steam-runtime`, the `gh_engine_off` knob is not set, and `gh_cred.txt` exists. See
[in-game.md](in-game.md).
