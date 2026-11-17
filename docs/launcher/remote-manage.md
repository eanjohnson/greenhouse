# Manage from anywhere

> **Status: compiled out by default.** This feature is built only when the Launcher is compiled with `/DGH_REMOTE_RELAY` (`EXTRACL=/DGH_REMOTE_RELAY toolchain/iterate-launcher.sh ...`). The relay used to accept a per-console credential that no longer exists, and a new console authentication scheme has not been designed yet. Until it is, a build with the define still has nothing to present to the relay (`Host::token` returns an empty string) and the relay refuses its polls. In a default build there is no Settings entry, no poller and no `LocalState\gh_device.txt`; the wall's RB action reads "From a PC" and leads only to [Manage from a browser](web-manage.md), which works on the local network without any of this.
>
> This file documents the end-to-end protocol as designed and as the console implements it behind the define: pairing, key agreement, envelopes and signed commands are complete and tested (see [Testing](#testing)). Anything about how the console authenticates its polls to the relay is pending that new scheme.

What this covers: managing a console's library from a phone over the internet through a relay that cannot read or forge the messages: pairing, key agreement, message envelopes, signed commands, replay rules, and what each party can see.
Where the code is: `src/loader/gh_remote.h` (protocol and poller), the remote section of `src/loader/launcher_ui.h` (what commands do), `toolchain/test_gh_remote.*` and `toolchain/vectors/remote_vectors.json` (tests). The relay and the phone's page live in the separate site repository (`src/lib/remote*.ts`, `src/app/api/remote/`).
How it was found: [managing-from-a-browser.md](../writeups/managing-from-a-browser.md)

## Shape

Three parties:

- **Console.** Holds a long-term ECDH key. Only ever makes outbound HTTPS requests; nothing connects to it.
- **Phone.** A web page on the relay's origin. Holds its own ECDH key and a passkey (WebAuthn credential).
- **Relay.** A mailbox. Stores small encrypted envelopes for a short time and decides who may post to which mailbox.

In a build with `GH_REMOTE_RELAY`, the feature is off until the user turns it on (wall, RB for "PC & phone", then Y; `LauncherRemote`). In a default build that entry does not exist. The switch is `LocalState\gh_remote_on.txt`. Turning it off deletes the console's key and paired phones, drops the queue, and tells the relay to purge the mailbox (`LuiRemoteTurnOff`).

The launcher must be open on the TV for anything to happen.

## What a phone can ask for

A closed list (`kOps` in `gh_remote.h`):

| Op | Argument | Needs passkey signature |
|---|---|---|
| `refresh` | empty | no |
| `install` | Steam app id | yes |
| `resume` | Steam app id | yes |
| `verify` | Steam app id | yes |
| `cancel` | Steam app id | yes |
| `dequeue` | Steam app id | yes |
| `uninstall` | Steam app id, or `custom:<12 hex>` | yes |

`ValidArg` accepts only: an empty string for `refresh`; a decimal app id from 1 to 4294967295 with no leading zero; or, for `uninstall` only, `custom:` plus a custom game id. There is no op that launches a game, takes a path, passes arguments, reads or writes a file, or changes a setting, and none can be expressed.

The protocol layer only authenticates a command. Whether it makes sense is the launcher's decision (`LuiRemoteCheck`), made against the rows the wall last listed: an install must be an owned, not-installed title with Steam signed in and at least 2 GB free; a resume must be a partial install; an uninstall must be something the launcher manages and not the game currently running; and so on. Accepted changes go into a queue of at most 32 (`LocalState\gh_remote_queue.json`, kept across restarts) that the wall runs one at a time when idle, never while a game runs, re-checking each before it runs.

## Identities and storage

| Item | Form |
|---|---|
| Console key | ECDH P-256. Public key as the 65-byte uncompressed point, base64url. |
| Phone key (`epub`) | ECDH P-256, 65-byte uncompressed point |
| Phone id | Lowercase hex of the first 16 bytes of SHA-256(`epub`) (32 hex chars) |
| Passkey (`wpub`, `cred`) | ES256 (ECDSA P-256) public key as a 65-byte point, plus the raw credential id |
| Device id (`dev`) | The console's identifier at the relay: 32 random hex digits in `LocalState\gh_device.txt`, created on first use. Only the relay uses it |

On the console, in `LocalState\gh_remote\` (exists only while the switch is on):

- `key.bin`: the private key, protected with DPAPI with a context string bound to the device id. Where DPAPI is unavailable it is masked with a device-derived value instead and that is logged; this is obfuscation, not protection.
- `phones.json`: for each paired phone its id, label, `epub`, `wpub`, credential id, WebAuthn sign counter, and the last inbound and outbound sequence numbers. At most 5 phones (`kMaxPhones`).

All base64url in the protocol is unpadded and must be canonical: `B64In` re-encodes and compares, so one value has exactly one spelling.

## Pairing

1. **TV shows a QR code.** `Session::StartPairing` makes a random 16-byte token, valid 5 minutes (`kPairTtlS`), and builds

   ```
   https://<site>/remote/pair#d=<dev>&k=<console public key, b64url>&t=<token, b64url>
   ```

   Everything after `#` is a URL fragment, which browsers do not send to servers. The console tells the relay only the **pair id**, hex SHA-256(token), in its next poll (`"offer"`), so the relay can tell when a pairing window is open without learning the token.

2. **Phone answers.** After scanning, the page creates its ECDH key and a passkey and posts a pairing message, relayed to the console unchanged:

   ```json
   {"v":1,"phone":"<id>","epub":"<b64url>","cred":"<b64url>","wpub":"<b64url>","label":"<name>","ts":<unix s>,"mac":"<b64url>"}
   ```

   `mac` is HMAC-SHA256 keyed with the 16-byte token over the transcript (`PairTranscript`), fields joined by a zero byte:

   ```
   "greenhouse.remote.pair/1" \0 dev \0 consolePubB64 \0 phone \0 epubB64 \0 credB64 \0 wpubB64 \0 label \0 ts
   ```

3. **Console verifies** (`VerifyPair`): the phone id matches `epub`; `ts` is within 600 s of the console's clock; the MAC matches (constant-time compare); `epub` and `wpub` are valid P-256 points. The label must be 1 to 40 bytes with no control characters. A message is refused if the window is closed, another phone is already waiting, or the phone is already paired.

   Only a device that saw the QR code can compute the MAC. The relay cannot substitute its own keys without failing it.

4. **Both screens show a six-digit code** (`Sas`): the first four bytes of

   ```
   SHA-256( "greenhouse.remote.sas/1" \0 dev \0 consolePubB64 \0 epubB64 \0 wpubB64 )
   ```

   read as a big-endian integer, modulo 1000000, zero-padded. The TV shows the phone's label and the code (`LuiRemotePairModal`). The user compares and presses A to allow or B to deny. A only counts after the prompt has been up for 500 ms.

5. **Result.** `ResolvePair` stores the phone or records a denial; the next poll tells the relay. The token is wiped either way, so a QR code is single-use.

A phone can be forgotten on the TV (X on the page) or removed at the relay; the console drops phones the relay reports as revoked.

## Message keys

Each console/phone pair derives two AES-256 keys, one per direction (`DeriveKeys`):

```
shared = ECDH(P-256): the 32-byte x-coordinate, big-endian
salt   = "greenhouse.remote/1"
info   = "greenhouse.remote/1" \0 <dir> \0 dev \0 phone        dir is "c2p" or "p2c"
key    = HKDF-SHA256(ikm = shared, salt, info), 32 bytes
```

`c2p` is console to phone, `p2c` is phone to console. Keys are derived when needed and not stored.

## Envelopes

Every message in either direction is one JSON object (`Seal` / `Open`):

```json
{"v":1,"p":"<phone id>","s":<seq>,"t":<unix s>,"iv":"<b64url, 12 bytes>","c":"<b64url, ciphertext || 16-byte tag>"}
```

- Cipher: AES-256-GCM, random 96-bit nonce per message, 128-bit tag appended to the ciphertext.
- Additional authenticated data (`MsgAad`), fields joined by a zero byte, numbers in decimal:

  ```
  "greenhouse.remote.msg/1" \0 <dir> \0 dev \0 phone \0 seq \0 ts
  ```

  So the direction, both identities, the sequence number and the timestamp are all bound to the ciphertext. Changing any of the cleartext fields `p`, `s` or `t` makes decryption fail.

Size limits: an inbound envelope larger than about 16.5 KB is refused (`kMaxCmdPt` is 8 KiB of plaintext); a snapshot's plaintext is at most 192 KiB (`kMaxSnapPt`).

## Commands (phone to console)

Plaintext inside a `p2c` envelope:

```json
{"op":"install","arg":"<app id>","a":{"cred":"<b64url>","ad":"<b64url>","cd":"<b64url>","sig":"<b64url>"}}
```

`a` is a WebAuthn assertion and is required for every op except `refresh`. `Session::OnCommand` checks, in order:

1. The phone is paired; the envelope decrypts under that phone's `p2c` key.
2. **Replay:** `seq` must be strictly greater than the last sequence accepted from that phone.
3. **Freshness:** `ts` must be within 600 s (`kSkewS`) of the console's clock.
4. `op` is in the closed list and `arg` is valid for it.
5. For signed ops, the credential id equals the one stored at pairing, and the assertion verifies.

The assertion's challenge is (`CmdChallenge`):

```
SHA-256( "greenhouse.remote.cmd/1" \0 dev \0 phone \0 seq \0 op \0 arg )
```

so a signature covers exactly one command, for one console, from one phone, at one sequence number. `VerifyAssertion` checks:

- the first 32 bytes of authenticator data equal SHA-256(relying-party id);
- the user-present and **user-verified** flags are both set (biometric or device PIN, every time);
- client data has `type` `webauthn.get`, `origin` exactly the site's origin, is not cross-origin, has no `topOrigin` (not in a frame), and `challenge` equals the base64url of the expected hash;
- the signature is strict DER (`DerToRaw` rejects non-minimal encodings) and verifies as ECDSA P-256 over SHA-256(authenticatorData || SHA-256(clientDataJSON));
- the sign counter: if either the stored or the presented counter is non-zero, the presented one must be greater. Synced passkeys always report zero, which is accepted; a counter that has ever moved must keep moving.

Only after every check passes are the phone's sequence number and counter advanced and saved. A failure is logged without contents and reported back to that phone as a result; the relay's copy is acknowledged either way so nothing loops.

The relying-party id and origin are the site the console is built to talk to (`LuiRemoteOrigin`, `LuiRemoteRpId`). A release build has it fixed in code.

## Snapshots (console to phone)

The console sends each phone a sealed JSON snapshot (`LuiRemoteSnapshot`, then `SealSnapshots`): free space rounded to 256 MB, whether Steam is signed in (yes/no only, never the account), what is playing, the download in progress, the queue, the last 10 finished changes, installed titles with sizes, custom games, the owned-but-not-installed list (trimmed if it would exceed the size cap, with a count of what was left out), the last 8 results for that phone's own commands, the phone's label, and a timestamp.

Only launcher-managed installs are listed. USB folders and configured paths are never shown to a phone.

The outbound sequence number is the wall clock in milliseconds, or the previous one plus one if the clock has not advanced. A sequence is spent whether or not the message arrives.

Snapshots are sent when a phone is looking, when something changed (at most every 30 s while nobody looks), when a command has a new result, and at least every 6 hours.

## The poll

All console traffic is one call, `POST /api/remote/poll` (`Poller::PollOnce`):

```json
{"v":1,"enabled":true,"phones":["<id>"],"offer":"<pair id>","paired":[{"id":"","label":""}],
 "denied":["<id>"],"forgot":["<id>"],"acks":["<msg id>"],"states":[{"phone":"<id>","env":"<envelope>"}],
 "playing":false,"downloading":false}
```

```json
{"next_ms":60000,"watching":false,"cmds":[{"id":"","phone":"","env":""}],
 "pairs":[{"phone":"","msg":""}],"revoked":["<id>"]}
```

- `next_ms` is the relay's suggested wait, clamped by the console to between 2 s and 15 min. After receiving a command the console polls again within 2 s to return the result.
- On failure the console backs off from 10 s up to 5 min. Pairing decisions taken out for a failed poll are put back so they are not lost.
- `{"v":1,"enabled":false}` is sent once when the feature is switched off, so the relay purges.
- The request is meant to carry a bearer credential for the console (`Host::token`), and the answer may include a short-lived replacement credential for later polls. The console credential is the part with no current design: `Host::token` returns an empty string, so today every poll is refused. A new console authentication scheme is needed before the define is worth turning on.

The poller runs on its own thread and never touches the UI. It reads mirrors the UI thread maintains and leaves accepted commands in the queue.

## What the relay can and cannot see

Can see:

- That a console exists, when it polls, and its network address.
- Phone ids, phone labels, and which phones are paired with which console.
- Whether a pairing window is open (the pair id), and the pairing message, which contains public keys only.
- Envelope metadata: phone id, sequence number, timestamp, size.
- The two booleans `playing` and `downloading`.

Cannot do:

- Read a snapshot or a command. It never holds a message key.
- Forge or alter a command. It cannot produce a valid envelope, and even with a message key it could not produce the passkey signature.
- Replay a command. Sequence numbers only move forward and the signature binds the sequence.
- Pair a device of its own. It never sees the token; and if it swapped keys in transit, the six-digit codes on the two screens would differ.
- Make the console do anything outside the closed op list.

Can still do: drop or delay messages, refuse service, and observe timing. A stale command is refused by the console after 10 minutes, and the relay itself discards undelivered commands after 15.

Residual risks to keep in mind when changing this code:

- The six-digit comparison is the defence against a relay that tampers with pairing. A user who presses A without comparing gets only the MAC's protection, which already requires the attacker to have seen the QR code.
- The QR code is a short-lived credential. Do not log or store it; see [overlay.md](overlay.md).
- Both clocks must be within 10 minutes. A phone with a wrong clock gets "message too old".
- A compromised phone page (the relay serves it) could misuse the phone's keys while the user is approving prompts. Passkey user verification on every change limits this to actions the user actually confirms, within the closed op list.

## Testing

`toolchain/test_gh_remote.sh` builds `test_gh_remote.cpp` with `clang-cl` and runs it under wine against `toolchain/vectors/remote_vectors.json`. The vectors are produced by the site repository's generator script using the browser's WebCrypto, so the test proves the two implementations agree on pairing MACs, the six-digit code, key derivation, envelopes and signed commands, and that tampered inputs are refused. Every key in the vector file is a throwaway generated for that file; never put a real key there.

The script needs `clang-cl`, `lld-link`, wine and the Windows SDK at `toolchain/xwin-sdk`. It finds the repository relative to itself. The test covers the protocol code in `gh_remote.h`, which builds whether or not the Launcher compiles the relay in.

If you change any constant string, field order or limit in `gh_remote.h`, the site's implementation and the vectors must change in the same commit series, and the version suffix (`/1`) in the affected label should be bumped.
