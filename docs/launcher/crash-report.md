# Crash screen and crash report

What this covers: what the launcher does when a game crashes or fails to start (the crash
observer, the crash screen, the modded-crash screen drawn by a restarted host and its "Start
without mods" offer), and the opt-in crash report: what it contains, how the whitelist scrubber
in `gh_crash.h` cleans it, where it is posted, and how the scrubber is tested.
Where the code is: `src/loader/gh_crash.h` (`ghcrash::ScrubLine`, `ghcrash::Scrub`,
`ghcrash::Upload`, `ghcrash::AddSensitive`); `src/loader/launcher_ui.h` (`LauncherShowCrash`,
`LuiCrashTitleName`, `LuiModsCrashSave`, `LuiModsCrashLoad`, `LuiModsCrashScreen`,
`LuiModsRecoverAtStart`, `LuiModsStreakGate`, `LuiCustomReportTitle`, `LuiCustomReportInfo`,
`GhCustomEnd`); `src/loader/guest_loader.cpp` (`CrashObserverVeh`, `GhCrashObserverVehBody`,
`DiagRingCopy`, the `STAGE=guest_done` block); `src/loader/gh_site.h` (`GH_SITE_URL`,
`ghsite::Server`); `toolchain/test_gh_crash.cpp`.
Related: [README.md](README.md) · [../architecture/pe-loading.md](../architecture/pe-loading.md) · [../reference/diagnostics.md](../reference/diagnostics.md) · [web-manage.md](web-manage.md) · [../mods.md](../mods.md) · [../security.md](../security.md)
Period: 2026-10-24 to 2026-11-11, Launcher 0.0.721.0–0.0.950.0. Notebook sections: §52, §64, §65.

## Summary

- When a game ends, the launcher shows one of three screens: SESSION ENDED, GAME CRASHED or
  LAUNCH FAILED, with the tail of the loader's in-memory log. A quit from the overlay shows
  nothing and restarts the app instead.
- A crash is detected by Greenhouse itself, mainly by the first-chance crash observer
  (`CrashObserverVeh`). The game's own crash reporters are kept from starting.
- A crash during a modded launch is shown by the next process, not the one the game died in:
  the dying host saves the screen to a file and asks the OS to restart the app. That screen
  offers "Start without mods" for one launch.
- From any of these screens, Y offers to send a crash report. Nothing is sent unless the user
  presses Y and then A on a consent screen that lists what goes and what is removed. It is the
  console's only path that sends information about a play session anywhere.
- The report is the end of the log after a whitelist scrub: a word survives only if it matches
  an allowed shape (number, identifier, short punctuation, key=value of those). Quoted text,
  long hex or base64 runs, e-mail addresses and directory names never survive; lines that
  mention a credential are dropped whole.
- It is posted as JSON to `<site>/api/crash-report`, where `<site>` is `GH_SITE_URL` or the
  per-console override. A build with no site never offers it.
- `toolchain/test_gh_crash.cpp` runs the scrubber on the development PC, on synthetic secrets
  and on any real diag you give it.

## When a game ends

### How a crash is recognised

The session ends on the guest worker thread, and the UI thread then reads four pieces of state
in the `STAGE=guest_done` block of `guest_loader.cpp`:

| State | Set by |
|---|---|
| `g_guestCrashed` | The crash observer (unhandled fault on the guest worker thread, in image code); `CallGuestEntry` catching an exception out of the entry point (`STAGE=guest_seh`); the 32-bit track returning a reason that starts with `FAULT ` |
| `g_guestFailWhy` | A human-readable reason: the observer's fault line, a refused import, "not PE32+", and similar |
| `g_guestExitCode` | The guest's exit code; `-999` is the loader's own "did not run" code |
| `g_guestUserQuit` | The user quit from the in-game overlay |

`STUB-CALLED` anywhere in the log ring also counts as a bad run: the guest called an import
nothing implements.

The screen title follows from these:

| Title | Condition |
|---|---|
| (no screen; the app restarts) | `g_guestUserQuit` |
| `SESSION ENDED` | none of the conditions below |
| `GAME CRASHED` | `g_guestCrashed` |
| `LAUNCH FAILED` | a bad run that did not crash: a fail reason, exit code `-999`, or a stub call |

The line under the title is `g_guestFailWhy`, converted from UTF-8, or `exit code N`. A guest
that faulted ran its own code, so it is reported as crashed, never as a failed launch.

### The crash observer

`CrashObserverVeh` is a vectored exception handler that `RunGuest` installs as the first handler
before any guest code runs. It does far more than report crashes (the pagers, the TLS repair
and several driver repairs live in it); see
[pe-loading.md, section 9](../architecture/pe-loading.md#9-exceptions-the-first-chance-crash-observer)
for its order of handlers and [diagnostics.md, section 5](../reference/diagnostics.md#5-the-crash-observer)
for reading its `VEH fault` lines.

The part that matters here is the last step of `GhCrashObserverVehBody`. After logging the
fault, in a launcher build:

- A fault whose instruction pointer is outside every loaded image is treated as JIT code and
  left to the guest's own handler. Mono implements C# null checks as access violations and
  turns them into `NullReferenceException`; ending the session there killed a Unity game on
  its first managed null.
- Any other fault on the guest worker thread sets `g_guestCrashed`, stores the fault line as
  the reason, and ends the session through `GhGuestExit`. The launcher survives; the screen
  follows.
- Faults on other threads are logged and passed on (`EXCEPTION_CONTINUE_SEARCH`).

Games often ship their own crash reporter. Greenhouse keeps them from starting, because it
observes crashes itself and because those helpers cannot run in the app container:
`GhHideCrashpad` hides `crashpad_handler.exe` from file probes (knob `gh_hide_crashpad`), the
CreateProcess surrogate refuses crash-handler executables (`CP: refusing crash handler`), and a
publisher's in-process reporter start call is answered "not started"
(`BED crs_start_handler`). The game then runs with its own crash reporting switched off.

### What the screen shows

`LauncherShowCrash` draws the title, the reason, and the log, scrolled to the end ("What the
game reported, most recent last"). The log is `DiagRingCopy()`: the last 80 `Diag` entries,
held in memory. One entry can be a multi-line stack dump; each line is drawn as one row, as
printable ASCII, cut at 200 characters. The ring exists in release builds too (`GH_RELEASE`),
which write no log file, so the screen and the report work the same in a release build.

Controls:

| Button | Action |
|---|---|
| A or B | Back to the library (with an alternative action, A takes it and B goes back) |
| D-pad, left stick | Scroll the log |
| Y | Send a report (shown only when a site is configured and the title has a name; hidden after a report was sent) |

The screen follows a dead guest whose suspended threads may still hold locks. A watchdog
thread logs `CRASHSCREEN mem ...` every 2 s for the first 3 minutes (app memory, commit, video
memory, worst frame time), and, when a frame stalls for more than 2 s, logs
`CRASHSCREEN frame stalled` with the UI thread's live stack, at most once every 5 s. It never
dumps all threads: walking a frozen Mono thread's JIT frames deadlocked the watchdog itself.

### Modded launches: the screen comes from a restarted host

A launch with a mod profile armed (see [../mods.md](../mods.md)) that ends badly takes a
different path:

1. `GhModRunEnd` records the failure in the run record `LocalState\gh_mod_run.txt`.
2. The dying host writes the screen (title, reason, and up to 256 KB of the log ring) to
   `LocalState\gh_mod_crash.txt` (`LuiModsCrashSave`) and calls `RequestRestartAsync`.
3. The new process, before the library appears, reads and deletes that file
   (`LuiModsCrashLoad` in `LuiModsRecoverAtStart`) and draws the screen.
4. `LuiModsCrashScreen` appends the last 120 lines of the mod loader's own log
   (`GhModLogTail`: BepInEx's `LogOutput.log` or MelonLoader's `Latest.log`) under a separator,
   and calls `LauncherShowCrash` with the alternative action **A Start without mods**.
5. A launches the same game once without mods, in that fresh process. The profile stays
   selected for next time.

If the restart has not happened after 8 s, or throws, the old process shows the screen itself
(`STAGE=restart_timeout — crash screen here`) and drops the saved copy; choosing "Start without
mods" there writes `LocalState\gh_launch_once.txt` and restarts.

Two more cases use the run record:

- A record still in state `running` when the app starts means the whole process died while the
  modded game was starting. The start counts as a failure and the screen says "the app closed
  during startup".
- After two or more failed starts in a row with the same profile, the next launch of that game
  asks first: **A Start without mods · Y Start with mods anyway · B Back**
  (`LuiModsStreakGate`). Changing the profile clears the streak.

Unmodded crashes are still shown in the process the game died in.

## The crash report

### Consent

Y on the crash screen opens a "SEND A REPORT" modal that names the title and says what goes and
what is taken out:

> Sends the end of this log, the game's name, the console model and the Greenhouse version.
> Account names, file paths and anything key-shaped are removed first.

A sends; B cancels (`CRASHREPORT declined`). For a game the user added themselves the text names
the program's file name and a fingerprint of it instead of the game's name. Then:

- The upload runs on its own thread, so a blocking network call on a screen that follows a
  dead guest cannot look like the launcher dying too. B does not cancel a request in flight.
- "REPORT SENT" or "NOT SENT" follows, with the reason in plain text. After a success the Y
  hint is gone and the subline says "report sent"; after a failure Y comes back, because the
  usual failure is the network.
- The console keeps nothing about the report. Only the log lines `CRASHREPORT sending ...` and
  `CRASHREPORT ok` or `CRASHREPORT failed: <why>` record that it happened.

The comment above `#include "gh_crash.h"` in `launcher_ui.h` states the rule: this is the only
outbound path of its kind, and it never opens by itself. Everything else the console fetches
from the site is a read (the package repository index, the compatibility dataset as a signed
package). The optional phone relay in `gh_remote.h` is compiled out unless `GH_REMOTE_RELAY` is
defined; see [remote-manage.md](remote-manage.md).

### Custom games and the browser page

A game the user uploaded from a browser (see [web-manage.md](web-manage.md)) has two ways to
send the same report:

- The crash screen's Y, as above.
- The browser page's report button (`POST /api/games/<id>/report` on the console's local web
  server). When a custom run ends, `GhCustomEnd` stores the already-scrubbed log with the game's
  run record, so the page shows exactly the text that would be sent. The upload scrubs it again,
  which is safe because the scrub is idempotent.

A custom game's report is filed under `custom:<exe base name>` (`LuiCustomReportTitle`), passed
through the same word scrub and never the name the user typed, and carries the exe's SHA-256,
size and architecture (`LuiCustomReportInfo`) so reports about the same file group together
without the file or its folder name being sent.

### What is sent

`ghcrash::Upload` builds one JSON object:

| Field | Value |
|---|---|
| `titleName` | `LuiCrashTitleName()`: the picked row's display name, else the compatibility dataset's name for the app id, else the mod run record's title, else `app <id>`; at most 120 characters. Empty means nothing was picked, and the upload is refused |
| `log` | `ghcrash::Scrub(entries)`: the scrubbed end of the log ring (plus the mod loader's log tail on a modded screen), at most 192 KiB |
| `greenhouseVersion` | The app's own package version |
| `consoleModel` | `ghcompat::ModelName(ghcompat::GetModel())`, the console family |
| `origin`, `exeSha256`, `exeSize`, `exeArch` | Custom games only: `"custom"` and the file's fingerprint |

Nothing else: no device id, no account, no IP address in the body, no list of installed games.

### Where it goes

`ghsite::Server()` decides the base URL:

- `LocalState\gh_site_url.txt` on that console, if present, else the build's `GH_SITE_URL`
  (the project website by default; a fork sets its own, see
  [../building.md](../building.md#forking-your-own-site-and-package-repository)).
- Only an `https://` URL is accepted. Anything else, or `GH_SITE_URL=""`, means no site: the
  crash screen does not show Y at all, and there is no default package repository (one set in `gh_repo_url.txt` still works).

The request is `POST <site>/api/crash-report` with `Content-Type: application/json`, sent through
WinHTTP (`ghrepo::HttpRequest`), response capped at 16 KiB. Status handling:

| Status | Shown as |
|---|---|
| 201 | sent |
| 401 | the site refused this build's token |
| 413 | report too large |
| 429 | too many reports just now -- try later |
| other | the site answered N |
| no answer | the WinHTTP reason, or "could not reach the site" |

On the site the report is stored and listed for maintainers; custom-game reports are marked as
such and get no public page. The site rejects a log over 256 KiB outright, which is why the
console caps it at 192 KiB.

## The scrubber

### Why a whitelist

A log tail is not safe to upload as it is. This was measured, not assumed: the add-on sign-in
line in `launcher_ui.h` logs

```
ADDON <pkg> signIn -> 1 in 812 ms label='<the account name>'
```

so the signed-in account name is in the very ring a report would post. Other lines carry
absolute paths, the console's device id, package key ids and serials.

A blacklist (a list of the secrets someone thought of) misses exactly that account name, because
a user name looks like any other short word. So `gh_crash.h` is a whitelist at word granularity:
each space-separated word survives only by matching an allowed shape, and anything else becomes
`<x>`. The account name above is removed not because the scrubber knows it but because quoted
text is never allowed: `label='...'` cannot survive whatever is inside the quotes.

### One line: `ScrubLine`

Each line goes through these steps in order. An empty result means the line is dropped.

1. **Normalise.** Printable ASCII only; tab becomes space, any other byte becomes `?`. The line
   is cut at 400 characters (`kMaxLineChars`).
2. **Known identifying strings.** Every string registered with `AddSensitive` is replaced by
   `<user>` wherever it appears, whatever shape it is in. The launcher registers every add-on's
   sign-in label. Strings shorter than 3 characters or without a letter or digit are ignored
   (redacting them would blank half the log); at most 8 are kept. This runs first so a known
   string cannot survive inside a shape that would otherwise pass.
3. **Credential lines.** A line whose lower-cased text contains `cred`, `token`, `secret`,
   `password`, `bearer`, `jwt` or `refresh` is dropped whole. The whitelist would very likely
   redact the value anyway; dropping the line means that does not have to be right. No such
   line has ever helped diagnose a game.
4. **Words.** Up to three leading `( [ { <` and trailing `) ] } > , ; . ! ?` are kept so the line
   stays readable, and the inside is classified:

| Class | Rule | Examples that survive |
|---|---|---|
| Quoted | contains `'` or `"`: always `<x>` | none |
| Path | contains `\`, or two `/`, or one `/` with a `:` | reduced to `<path>\<last component>` if that component is itself a number or identifier, else `<path>` |
| Number | at most 24 characters; optional sign; `0x` with 1–15 hex digits, or decimal with at most one dot and fewer than 13 digits; optional unit of up to 4 letters or digits | `12`, `-3.5`, `0x80070002`, `512MB`, `16ms`, `1762800000` |
| Identifier | at most 48 characters; starts with a letter, digit or `_`; only letters, digits, `_ - .`; no unbroken letter/digit run of 20 or more; not 12 or more hex digits with nothing else | `LAUNCHER`, `app_1966720`, `0.0.939.0`, `UnityPlayer.dll`, `GPUHEARTBEAT` |
| Punctuation | at most 8 characters, printable, no letters or digits | `->`, `::`, `...` |
| Pair | `key=value` or `key:value`, key an identifier, value scrubbed by the same rules | `dt=16ms`, `err=0x80070002`, `label=<x>` |
| Anything else | | becomes `<x>` |

Each limit has a reason recorded in the source comments:

- The 13-digit decimal limit: a 64-bit Steam id is 17 digits, and the host test caught one
  passing as a "number". Epoch seconds (10 digits) and byte counts stay under the limit.
- The 15-digit hex limit and the 12-hex-digit identifier rule: a long hex run is a device id,
  key id or hash, never a quantity. The general 20-character run limit alone let a 16-digit
  repository key id through in the host test. Testing for hex-only is what separates those from
  long ordinary words such as `GPUHEARTBEAT`, which contain non-hex letters.
- The 20-character run limit: an unbroken run that long is a token, hash or base64 blob; real
  log words and dotted versions break well before it.
- The path rule: a single `/` is usually a ratio in this log (`3/4 ok`), so a path needs two
  separators or a drive colon. The file name is the diagnostic value; the directories above it
  are the user's machine. `D:\games\app_70\hl2.exe` becomes `<path>\hl2.exe`.

Examples, from the host test's synthetic cases:

```
ADDON steam signIn -> 1 in 812 ms label='someoneelse'   ->  ADDON steam signIn -> 1 in 812 ms <x>
LIBRARY owner <registered label> has 161 titles          ->  LIBRARY owner <user> has 161 titles
LAUNCHER resume guest=D:\gh_games\app_70\hl2.exe         ->  LAUNCHER resume guest=<path>\hl2.exe
REPO index ok key <16 hex digits> serial=1000            ->  REPO index ok key <x> serial=1000
AUTH token stored iat=... exp=...                        ->  (line dropped)
FRAME dt=16ms appused=2314MB viduse=812MB                ->  unchanged
```

### The whole report: `Scrub`

`Scrub` turns the ring into the uploaded text:

1. Split every entry on newlines (a stack dump is one entry).
2. Walk from the newest line backwards, scrubbing each. The death is at the end, so the end is
   what is kept when the budget runs out.
3. Collapse runs of lines that differ only in their numbers. The run key is the line with every
   decimal number, and every hex number after `0x`, replaced by `#` (`RepeatKey`); letters are
   never blanked, so `GPUHEARTBEAT` does not collide with `FRAME`. The first 3 lines of a run are
   kept (`kKeepRepeat`: the numbers in them show the trend), then one marker
   `... <N more like this>`, placed just before the samples it stands for. Measured on two real
   500 KB console diags: the tail was almost entirely `MEMORY sample` heartbeat lines, and an
   uncollapsed report spent its whole budget on them and never reached the death.
4. Stop at 192 KiB (`kMaxLogBytes`), below the site's 256 KiB limit with room for the rest of
   the JSON.

### Idempotence

`Scrub(Scrub(x)) == Scrub(x)`, and every placeholder (`<x>`, `<path>`, `<user>`, the repeat
marker) passes through unchanged. This matters because the browser page shows a stored,
already-scrubbed log and the upload scrubs it again. An early version stripped the `<` of
`<path>` as a bracket and grew `<<path>` on every pass; `Word` now recognises its own
placeholders.

### What it does not protect

- **The local log file.** `loader_diag.txt` on the console (development builds only) is not
  scrubbed. Never log secrets or account identifiers in the first place; see
  [diagnostics.md](../reference/diagnostics.md).
- **Identifier-shaped personal words that were not registered.** An unquoted user name that
  looks like an ordinary word passes the shape test. Only `AddSensitive` catches it, and only
  for strings the launcher knows about. A new log line that prints a name should quote it, and
  a new source of identifying strings should register them.
- **File names.** The last path component survives by design. A file named after a person would
  be sent.
- **The title.** The game's name is sent; for a custom game, its exe's base name.

## Testing

`toolchain/test_gh_crash.cpp` builds the scrubber alone. With `GH_CRASH_SCRUB_ONLY` defined,
`gh_crash.h` compiles with no Windows headers and no `Upload`, so it runs on the development PC
with plain `g++`:

```
cd toolchain
g++ -std=c++20 -O1 -DGH_CRASH_SCRUB_ONLY -I../src/loader test_gh_crash.cpp -o /tmp/test_gh_crash
/tmp/test_gh_crash                        # synthetic cases only
/tmp/test_gh_crash path/to/loader_diag.txt # plus a real pulled diag
```

It has two halves:

- **Synthetic cases** plant known secrets (an account name, quoted and unquoted, with and
  without `AddSensitive`; drive paths; a 32-hex device id; 16- and 12-hex key ids; a 17-digit
  Steam id; a base64 blob; an e-mail address; bearer, token and credential-file lines) and
  assert each is gone. They also assert that ordinary text survives: tags, `key=value` pairs,
  versions, HRESULTs, units, DLL names, `->`, app ids and epoch times. A scrub that redacts
  everything is safe and useless, so these checks matter as much as the leak checks. It also
  checks repeat collapsing (500 heartbeat lines plus a fault become a few lines that keep the
  fault; 20 lines that differ in more than numbers are all kept) and placeholder stability.
- **A real diag**, given on the command line, is checked structurally: no surviving quote
  characters, no drive-letter or UNC paths, no 20-character alphanumeric runs, the result under
  the cap, and `Scrub(Scrub(x)) == Scrub(x)`. It prints only counts and the last six scrubbed
  lines, never the raw diag.

Exit status is 0 on pass. Run it after any change to `gh_crash.h`, and run it on a fresh diag
after adding a log line that prints anything a user might consider theirs. The upload itself,
the consent modals and the modded restart path are only exercised on a console.

## Knobs and files

| Name | Kind | Default | Effect |
|---|---|---|---|
| `GH_SITE_URL` | build define | the project website | Base URL for reports and the default repository. `""` builds with no site: no Y on the crash screen |
| `LocalState\gh_site_url.txt` | per-console file | absent | Overrides `GH_SITE_URL` on one console. Must be `https://`; anything else disables the site |
| `GH_CRASH_TOKEN` | build define | empty | A write-only token sent as `Authorization: Bearer` when set; it gates writes on a site that requires one and cannot read a report back. Comes from the build environment; its value is never in the tree |
| `GH_RELEASE` | build define | off | No log file; the in-memory ring, the crash screen and the report are unchanged |
| `gh_veh_max` | knob | 4 | How many first-chance faults the crash observer logs in full |
| `gh_veh_stack` | knob | 1 | Add a stack walk to each logged fault |
| `gh_hide_crashpad` | knob | 1 | Hide the game's crash-handler helper so it is not started |
| `LocalState\gh_mod_crash.txt` | state file | | The saved modded crash screen, read and deleted by the next process |
| `LocalState\gh_launch_once.txt` | state file | | "Start without mods" requested from the old process |
| `LocalState\gh_mod_run.txt` | state file | | The mod run record: state and the failed-start streak |
| `kMaxLogBytes`, `kMaxLineChars`, `kKeepRepeat` | constants in `gh_crash.h` | 192 KiB, 400, 3 | Report size, line length, lines kept per repeated run |

There is no knob that turns on automatic reporting, and none should be added.
Knob reference: [../reference/knobs.md](../reference/knobs.md).

## Why it is this way

- **Opt-in, per crash.** When the compatibility record was made a single reviewed file (§64,
  the notebook section on compatibility parity), a launcher that posted every session's result
  was rejected on purpose: it would mean the console telling a server which games were played
  and when, which is a consent question, not a technical one. The crash report added in the
  same release is the deliberate exception: one report, about one failure, sent only after the
  person has seen what goes. Its purpose is that a stranger's failure becomes a compatibility
  result instead of being lost.
- **The consent text lists what is removed.** "We remove personal data" with no list is a
  promise the person cannot check.
- **A whitelist, scrubbed on the console.** The secret that was actually found in the log (the
  account name) is the kind a blacklist misses. Scrubbing before upload means the site never
  holds the unscrubbed text.
- **Testable on the host.** The rule was to write the whitelist first and test it against a
  real diag before shipping. A scrub that can only run on a console does not get exercised, so
  the scrub half has no Windows dependencies.
- **The modded crash screen lives in a new process** (§52, mod safety). In the process the game
  died in, the whole app stalled for about 15 s at a time, the UI thread and the watchdog
  together, with memory far below the limit (275 MB in use), until the shell suspended and
  killed it. The guest's suspended threads hold whatever locks they held. In a restarted
  process the same screen held for 50 s with a worst frame of 32 ms. A fresh process is also
  required for "Start without mods", because Unity cannot start twice in one process.
- **Upload on its own thread** for the same reason: a blocking call on that screen would look
  like the launcher dying too.
