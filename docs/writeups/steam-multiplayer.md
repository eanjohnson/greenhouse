# Steam multiplayer on the console, and the Join button

What this covers: how a game running in Greenhouse gets a real Steam session (lobbies, invites,
peer-to-peer play) with no PC involved, and the long hunt for why a friend's invite had no working
Join button.
Where the code is: `corpus/shims/steam_api64.cpp` (`GhBringUpEngineNow`, `GhInviteFriend`,
`gh_set_connect_richpresence`, `gh_invite_targeted`), `corpus/shims/gh_steam_engine.h`
(`gh_send_changestatus_online`, `gh_send_games_played`, `gh_register_game_enabled`,
`gh_fix_app_context`, `gh_target_appid`), `corpus/shims/gh_steam_qr.h` (`gh_qr_upgrade`),
`src/loader/guest_loader.cpp` (`Bed_LoadLibraryExW`, `Bed_CreateProcessW`, `GH_LOADER_ENGINE_FIRST`),
`src/spikes/gh_gp_harness.cpp` (the standalone comparison harness).
Related: [../steam/README.md](../steam/README.md) · [../steam/in-game.md](../steam/in-game.md) ·
[../steam/sign-in.md](../steam/sign-in.md) · [../steam/runtime-fetch.md](../steam/runtime-fetch.md) ·
[../launcher/overlay.md](../launcher/overlay.md)
Period: 2026-07-25 to 2026-08-15, per-title development packages (Cheese Rolling package
0.0.54.0 to 0.0.110, Lethal Company package 0.0.16.0 onward). Notebook sections: §21, §24–§28.

## Summary

- Greenhouse does not reimplement Steam. It loads Valve's own client engine (the native DLL under
  the Steam client, without its web UI) into the game's process and forwards the game's Steam calls
  to it. That one decision gives real lobbies, real invites and Valve's real relay transport.
- With it, unmodified Cheese Rolling browsed, joined and hosted public lobbies on the console, and
  unmodified Lethal Company hosted a lobby, sent an invite, and a friend on a desktop PC joined and
  played co-op with the console.
- The invite at first arrived as plain text with no Join button. Steam only makes an invite
  joinable, and only uploads the rich-presence connect string that powers the button, while the
  inviter is shown as playing the game.
- The "playing" status surfaces only when the client first announces an online status change and
  then sends games-played, in that order. The minimal in-process engine never made the announcement
  on its own; the shim now sends it, then lets the engine send games-played.
- It took three weeks and five retracted explanations to get there. The decisive move was reproducing
  the full retail client's bring-up order rather than tuning the games-played message.

## The problem

Cheese Rolling and REPO reached their menus early, but the original `steam_api64` shim was an
offline stand-in: `SteamAPI_Init` succeeded, no callback ever fired, and every lobby or socket call
returned zero. That is enough for a menu and nothing more. Online play needs three things the stand-in
could not give: a signed-in Steam account, lobbies registered on Valve's servers, and a transport that
reaches another player.

Two constraints framed every option:

- **No companion PC.** The point of Greenhouse is unmodified software on the console itself.
- **No Steamworks SDK and no Valve files in the repository.** Valve's runtime may be used only if the
  console fetches it from Valve's own CDN ([../steam/runtime-fetch.md](../steam/runtime-fetch.md)).

## The investigation

### Phase 1: the design space (§21.1 to §21.4)

What the games need was read from Cheese Rolling's own managed code: lobbies for discovery
(create, join, list with filters, member data, the join-requested event) and the modern Steam
networking-sockets API for transport. Peers are addressed by Steam identity, never by IP address, so
the game does not care how bytes travel.

Every option first reduced to two poles:

| | A: a PC runs the real client | B: reimplement on the console |
|---|---|---|
| Real Valve sign-in | yes | would need our own client |
| Play with ordinary Steam players | yes | no, a private network |
| Companion PC | required | not required |

A was ruled out by the no-PC constraint. Running the whole retail client on the console was ruled out
too: its UI is a multi-process Chromium application that needs isolated, concurrent processes, and
part of it is a Windows service. The container allows neither, and Greenhouse's child-process support
runs a child in the parent's address space, which is the opposite of what Chromium needs.

The plan written at this point was "B-light": reimplement only the game-facing API, borrow Valve's
open-source networking library for transport, and discover lobbies by LAN broadcast. It was never
built.

### Phase 2: the third option, Valve's engine in-process (§21.5)

The Steam client is layered. Only the web helper is the Chromium part. The client engine underneath is
an ordinary native x64 DLL that handles the connection to Steam, sign-in, the Steamworks backend and
the relay transport, and it has no web dependency. Loading native Windows DLLs is what Greenhouse
already does, so the engine could be loaded where a reimplementation would otherwise be written.

Results on the console, no PC:

- The engine loaded in the container and connected to Steam's connection servers.
- QR sign-in through the Steam mobile app worked through the engine's own authentication service. No
  password is ever entered or handled.
- A lobby was created on Valve's servers and the game received the lobby-created and lobby-entered
  events through the shim's callback pump.
- The networking-sockets calls were forwarded to the engine's real interface. Transport rides Valve's
  relay, so it works over the internet with no LAN-only fallback.

Three general loader changes came out of it: the preloaded shim gets its imports redirected like a
guest DLL, `LoadLibraryA` joined the redirected set, and the shim build links the C runtime properly.
When the engine tries to start its web helper, `Bed_CreateProcessW` refuses it and the engine carries
on without a UI.

One early sign-in defect is worth recording. Sign-in seemed to die after under a minute, and that
looked like a rate limit or a regression. The cause was that Steam rotates the QR challenge and
reports the new client id in its poll replies. The original code ignored that, kept polling under the
old id, and got "expired". Following the rotation (keep the request id, adopt the new client id, redraw
the code) makes the session last as long as needed. Sign-in now lives in the Steam add-on and follows
rotation the same way ([../steam/sign-in.md](../steam/sign-in.md)).

### Phase 3: browse, join, host in a real game (§21.6)

Making the game drive lobbies from its own menus took four fixes. Two are general.

**The engine starts before Mono.** The engine's first local-user creation performs a one-time
initialisation that damaged memory the Mono runtime already owned, and the game crashed on the first
highlight of its online menu. Bringing the engine session up in the loader, before the game image is
mapped and before Mono creates its heap, moved that write out of the way (`GhBringUpEngineNow`,
compiled in with `GH_LOADER_ENGINE_FIRST` for per-title packages). Sign-in is deferred until the menu
is up, so the game boots with offline answers and flips to a live session afterwards. In the launcher
today the same ordering holds by construction: the launcher signs in and starts the engine before the
title picker, long before any game runs.

**App context.** Steam scopes lobbies, invites and presence to an app id. The session had to be bound
to the running game's id after every sign-in, because the sign-in sequence resets it.

The other two fixes were about calling the engine's internal interfaces correctly and are not
described here. After them the game listed real public lobbies, joined one as the seventh player of
six, and hosted with the account's real persona name.

### Phase 4: Lethal Company and the first invite (§24)

Lethal Company uses a different managed Steam wrapper and a different networking stack, and it brought
two new defects.

**Two copies of `steam_api64`.** The game booted to its menu, its managed Steam init reported success,
yet the shim logged nothing. The loader had preloaded the shim from one path; Mono then loaded
`steam_api64.dll` by the full path inside the game folder. On the console's OS, a full-path load is
keyed by path, not by base name, so a second, older copy of the shim loaded with its own globals. The
game talked to that copy, which was never signed in. The fix in `Bed_LoadLibraryExW` and
`LoadGuestDll`: any request whose base name is `steam_api64.dll`, with either path separator, returns
the preloaded module (diagnostic `-> PRELOADED singleton`). What isolated it was logging, in
`Bed_GetProcAddress`, which module each `SteamAPI_*` lookup resolved against: every lookup landed on a
module at a different base than the preload.

**Invites carry an app id.** Lobby creation and the invite call both returned success, but nothing
reached the friend. The engine's app context was still hard-coded to the previous title's id, so the
lobby was registered under the wrong game and the recipient's client discarded the invite silently.
`gh_fix_app_context` now takes the id from the `SteamAppId` environment variable, which the loader (and
the game's own Steam wrapper) sets before `SteamAPI_Init`. The next invite arrived on the friend's
desktop Steam.

**Invite policy, set by the developer at this point:** no automatic invites of any kind, no sending to
the whole friends list, no repeats, nothing hard-coded. And no filtering on persona state: in the
engine session every friend reads as offline, yet invites are delivered regardless.

### Phase 5: an invite with no Join button (§25)

The delivered invite was plain text. There was no Join button, and the account's profile showed no
game, so the friend could not join through the profile either.

The working explanation, which held to the end in outline: Steam makes a session joinable only for
the game the account is currently playing, and it learns that from a games-played message that a real
launch causes the client to send. Our engine never performs a launch, so Steam never hears it. The
rich-presence connect string that puts the Join button on an invite is gated on the same state.

Iterating on the console was too slow for this (build, sign, deploy, play by hand). From here the
investigation used a control VM running the retail Steam client, signed in to the same test account,
as a known-good reference for what a real client sends and what Steam shows.

The first idea was to build the games-played message ourselves and send it from the console. It was
built, checked against the retail client's own message, and sent from the console successfully and
repeatedly. The profile never changed.

### Phase 6: four explanations that did not survive (§26)

Each of these was believed for a reason and each fell.

1. **"The QR-upgraded session is not trusted for game state."** Our engine signs in anonymously and
   then upgrades to the real account. Perhaps such a session can create lobbies but is not treated as
   the account's playing client. A standalone harness (`src/spikes/gh_gp_harness.cpp`) that loads the
   engine fresh on the control VM made each cycle take seconds instead of minutes, and it disproved
   this: its sign-in message matched the retail client's field for field, it used the same kind of
   cached sign-in as the retail client, and the result was the same.
2. **"The game id was in the wrong field."** Based on second-hand documentation, the harness's message
   was "corrected" to put the game id in a neighbouring field. A capture of the message a real launch
   sends showed the opposite: the original encoding was right, and the "fix" had left the game id
   unset. It was reverted. One negative result had been gathered with the broken message and was void.
3. **"The session drops fifteen seconds after sign-in."** This looked like an engine bug. It was the
   control VM's retail client signed in to the same account at the same time. Steam does not let two
   clients play on one account, and kept ending the console's session. Once the console had the account
   to itself, the session held.
4. **"The friends subsystem never starts without the web helper."** Counting the callback ids the
   engine delivered, instead of discarding them, showed hundreds at startup, including a persona update
   for every friend. The subsystem was alive. What the session never received was friend *state*
   (online, in game), a separate mechanism. That gap is real, but not on the path to games-played.

Along the way the model sharpened. Real launches send the same games-played message and nothing else
that establishes presence, so the message content was never the problem. But the client also keeps
its own list of running games and periodically re-sends its own view of it, so a message that
disagrees with that list is overwritten. The right way to produce games-played is to register the game
with the engine as running, as a real launch does, and let the engine send the message itself.

### Phase 7: registration crashes, then "no client lever exists" (§26.10 to §27)

Registering the game on the console crashed the process. Two causes were found:

- **Thread.** The registration starts asynchronous work that must be created on the engine's own
  thread. Called from one of our threads, the work was freed early and the engine faulted later on its
  own thread. Running the call on the engine's thread fixed it.
- **Partial engine state.** Our engine had already created a placeholder entry for the game when the
  shim advertised it at sign-in, and registration does not attach a process to an existing entry.
  The shim waits for that entry to settle and then registers cleanly (`gh_register_do_call`).

The registration then succeeded and the engine sent a correct games-played. The profile still did not
change. A long elimination on the control VM followed, each item measured: message content (correct),
delivery (accepted, by the same send path that delivers invites), the client's later empty update
(suppressed, no effect), the sign-in message (identical to retail), sign-in type (same as retail),
companion messages at launch (none that relate to presence), connection maintenance (identical), and
setting the persona online through the engine (changed the local value only).

§27 concluded that eligibility to be shown as playing is a server-side property of the session, set
somewhere in the full client's start-up, with no client-side switch. It recommended recording the
missing Join button as a known limit.

### Phase 8: reproducing the full bring-up order (§28)

The decision was to try the one remaining lever: on the control VM the harness and the retail client
use the same cached credential, so with credential and messages held equal, the only remaining
difference is how the client is brought up. Our engine starts a minimal path (engine, local user,
sign-in, public interfaces); retail starts every subsystem. The plan was gated so it could stop early.

**Gate: find a signal that does not need a person to look.** Every test so far had needed the developer
to check a second Steam client by eye. Comparing incoming traffic around games-played found one:
retail's session receives a presence echo within a fraction of a second; ours received nothing. That
gave a fast, binary check.

**Comparison from the first moment of sign-in.** Every earlier capture had started after sign-in.
Capturing from the start showed that both clients subscribe to friends' personas, but only the retail
client sends an online status change after signing in. Our engine never sent one, even when asked to
set the persona online; in the minimal bring-up the engine's own persona publishing is inert.

**The fix.** The harness sent the online status change itself after sign-in, then games-played. The
developer's own Steam client, on another machine, showed the test account in game, in Lethal Company.
It was ported to the console the same day: after sign-in the shim sends the online status change, and
only once that has succeeded does it register the game, so the order on the wire is always status
change, then games-played. The console run showed the same result, with the control VM's Steam fully
shut down so the status could only have come from the console.

Two oracle mistakes cost time and are recorded so they are not repeated:

- Reading the session's own "game being played" value from the sending session is not evidence. Once
  the persona system is live, that value reflects the session's own local list back to it.
- A second session signed in to the same account cannot see that account's game at all. "Playing on
  another computer" is a cross-device notice, not a persona broadcast, and the observer reads nothing
  even while the real game runs.

Only a different account (a friend) or the account's own client on another machine is a valid
observer.

### Phase 9: the Join chain and co-op (§28.4, §28.5)

The Join button needs a second piece: the rich-presence connect string naming the lobby, which Steam
accepts only while the account is shown as playing. Hosting could happen before the status was
established, so the shim re-sets the connect string from its heartbeat once registration is done, and
refreshes it periodically. The online status change is also repeated about every minute, so the status
survives long sessions and reconnects.

The last problem was timing of the invite itself. The first implementation sent it when the shim saw a
lobby created, but the game creates one at its menu, before the player is in a real game lobby. That
automatic send was removed. During bring-up the send was triggered on demand by the developer; the
shipped path is the overlay, where the user picks a friend.

One container lesson came out of the on-demand trigger. The shim derived the trigger file's path from
a `LocalState` folder that the container redirects. Files the shim writes there are visible from
outside, but a file pushed in from outside was invisible to the shim's own reads, so the trigger was
never seen. Files the host pushes for the shim to read must live in the package's own `LocalState`.

The result: with the console hosting, the invite reached a friend's desktop Steam with a working Join
button, the friend joined, and the two played a real co-op session over Steam's peer-to-peer transport.
The console's view of that friend still read "offline" at the time, which is why the shim never
filters on persona state.

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| Only two designs exist: a PC hosting the real client, or a reimplementation | The full retail client cannot run in the container | The engine DLL under the client has no web dependency and loads in-process |
| QR sign-in dies after under a minute (rate limit, or a regression) | It "worked earlier" | Earlier scans happened before the first challenge rotation; following rotation fixed it |
| Sending games-played ourselves makes the account show as playing | A real launch sends it and our engine skips the launch | Sent correctly and repeatedly from the console with no effect |
| The QR-upgraded session is not trusted for game state | Lobbies worked but presence did not | Same failure with a sign-in message and sign-in type identical to retail's |
| The game id was in the wrong message field | Second-hand documentation | A real launch's message showed the original encoding was right; the "fix" left the id unset |
| The message is not the mechanism | A negative result sending it into a real session | That test used the broken encoding; real launches send the same message and nothing else presence-related |
| The session drops shortly after sign-in | Session id fell to zero after about fifteen seconds | Another client was signed in to the same account |
| The friends subsystem is dead without the web helper | Every friend read offline | A callback count showed persona updates for every friend; only friend state is missing |
| Being shown as playing is a server-side property with no client lever (§27) | Every measured difference had been eliminated | Comparing from the start of sign-in found the missing online status change; sending it fixed it |
| The session's own "game being played" value confirms the broadcast | It changed after registration | It reflects local state; a same-account observer is blind; only another account or the real client elsewhere is valid |
| Send the invite when the lobby is created | The hosting moment looked like the right trigger | The game creates a lobby at its menu, before a real game lobby exists |

## What shipped

All names below were checked against the tree.

| Rule | Where | Diagnostic or knob |
|---|---|---|
| Valve's engine runs in-process; its web helper and the games' crash handlers are refused as child processes | `Bed_CreateProcessW` in `src/loader/guest_loader.cpp` | `CP: refusing steamwebhelper`, `CP: refusing crash handler` |
| One `steam_api64` per process, matched by base name | `Bed_LoadLibraryExW`, `LoadGuestDll` | `-> PRELOADED singleton` |
| The engine starts before Mono | Launcher sign-in before the picker; `GhBringUpEngineNow` behind `GH_LOADER_ENGINE_FIRST` for per-title packages | `[gh-shim] GhBringUpEngineNow` |
| App id comes from `SteamAppId` | `gh_target_appid`, `gh_fix_app_context` in `gh_steam_engine.h`; `gh_resolve_appid` and `GhRebindAppContext` in `steam_api64.cpp` | `[gh-shim] SteamAPI_Init: app-context (sync) -> 1 want=<appid>` |
| Online status change first, then registration and games-played; the status change repeats about every minute | `gh_send_changestatus_online`, `gh_gp_rescan_tick`, `gh_register_do_call`, `gh_send_games_played` | `CHANGESTATUS-ONLINE`, `GAMES-PLAYED`; opt out with `gh_no_register.txt` in `LocalState` |
| Connect string re-set once in game, then kept alive | `gh_set_connect_richpresence` | `[gh-shim] connect re-assert (post-in-game)`, then `connect keepalive` |
| Invites only when the user picks a friend | `GhInviteFriend` (overlay friends page) | `[gh-shim] GhInviteFriend` |
| Never filter on persona state | `GhFriendAt` returns state 0 and does not query it | none |
| Accepting an incoming invite delivers the game's own join request | `GhPeekInvite`, `GhAcceptInvite` | `GhAcceptInvite queued` |

The engine path, its conditions (`gh_engine_off`, the installed runtime, a signed-in account) and its
cost of about 67 MB are described in [../steam/in-game.md](../steam/in-game.md). The file-triggered
invite helpers (`gh_invite_target.txt`, `gh_invite_now.txt`, `gh_invite_targeted`,
`toolchain/invite_now.py`) are still in the tree. They are development scaffolding from this bring-up,
not a feature, and should not be extended.

Compatibility records: Cheese Rolling (online lobbies: browse, join, host) and Lethal Company (co-op
with a friend over Steam, invites from the overlay) are both `PLAYABLE` in `corpus/compat/compat.json`.

## Still open

- The presence path depends on the exact runtime build pinned in `kSteamRuntimePin`. Moving to a newer
  runtime means re-validating `gh_steam_engine.h` against it ([../steam/runtime-fetch.md](../steam/runtime-fetch.md)).
- Friend state (online, in game) never reaches the engine session, so the overlay cannot show who is
  online.
- `gh_target_appid` falls back to a built-in id when `SteamAppId` is not set. The loader always sets it
  for a picked title, but a guest started some other way would bind the wrong app.
- Co-op play has been verified in one Unity title (Lethal Company); Cheese Rolling was verified for
  lobby browse, join and host only. Other networking stacks and Steam features
  (stats, inventory) have not been tried through the engine.

## Lessons

- Load the vendor's engine before reimplementing its protocol. The engine was a native DLL in a
  product whose job is loading native DLLs, and it brought interoperability that no reimplementation
  could.
- Follow server-driven rotation in any polled sign-in flow. A client that keeps its first challenge
  looks like it is being rate-limited.
- Treat a module by base name when the game expects one instance. The console's loader keys full-path
  loads by path, and two copies split their globals.
- Take the app id from the running game, never from a constant. A wrong app id makes invites vanish
  without an error on either side.
- When a status depends on a sequence, compare against the reference client from the very first
  message. Every capture that started after sign-in missed the one message that mattered.
- Validate presence only from a different account or the account's real client elsewhere. A session
  reading its own status sees local state, and a second session on the same account sees nothing.
- Give the test account to one client at a time. Steam ends one of two sessions playing on the same
  account, which looks like an engine bug.
- Count callbacks before declaring a subsystem dead. A histogram of event ids is the cheapest liveness
  check there is.
- Never fix an encoding from second-hand documentation. Capture the real thing first, or a "fix" can
  silently void the experiments that follow.
- Calls that start asynchronous engine work must run on the engine's own thread. Started elsewhere, the
  work is freed early and the fault appears later, far from the cause.
- Do not filter invites or friend lists on persona state. In this session every friend reads offline,
  and invites are delivered anyway.
- Never send invites automatically. A game's own lobby events fire at moments the user did not choose,
  and an invite is a message to a real person.
