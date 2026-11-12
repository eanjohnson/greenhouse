# Library, art and downloads

What this covers: listing the games an account owns, cover art, play time, downloading and verifying a
game from Steam's CDN, and Workshop items. Where the code is: `corpus/downloader/cm_downloader.h` and
`cm_workshop.h` (`namespace ghdl`), `corpus/addons/steam/steam_addon.cpp`, `src/loader/gh_art.h`.

Everything here needs a signed-in session ([sign-in.md](sign-in.md)). Nothing here uses Valve's runtime.

## Owned library

`FetchLibrary` calls `ghdl::ListOwnedGames`, which logs on with the saved token and calls
`Player.GetOwnedGames#1` (`list_owned_games`) asking for app info, played free games and free
subscriptions. Each game comes back with app id, name, play time and last-played time.

The result is cached in `LocalState\gh_owned.txt`, one game per line, tab separated:

```
appid <TAB> name <TAB> playtime_forever <TAB> playtime_2weeks <TAB> rtime_last_played
```

Older two-column files (app id and name) are still read. `OwnedLoad` reads the cache, so the library
shows at once and offline.

| `ListOwnedGames` result | Meaning |
| --- | --- |
| `0` | List fetched |
| `-1` | Could not connect |
| `-2` | Logon refused (token no longer valid; the add-on runs QR sign-in again) |
| `-3`, `-10` | Request failed or reply could not be read |

`Enumerate` turns the list into `GH_ROW_ACQUIRE` rows with the sub-label "owned".

**Free-to-play gap.** Steam leaves free-to-play games with no recorded play time out of this reply. So
that such a game can still be downloaded, a title config that carries a `steamAppId` produces its own
acquire row, and the add-on's `claims` function accepts any row with a Steam app id. If the account does
not actually have a licence, the download finds no usable depots and stops.

### Play time

Play time and last-played arrive in the same reply and are passed to the launcher in the row
(`playForever`, `play2Weeks`, `lastPlayed`). There is no separate request for them. How the launcher
shows and sorts by them is in [../launcher/library.md](../launcher/library.md).

## Cover art

`src/loader/gh_art.h` (`ghart`) fetches art by app id from Steam's public store CDN. No account data is
sent. Rules:

- Art is fetched only for app ids in the allow set (`Allow()`): titles the signed-in account owns or
  has installed. Other ids are never requested.
- First try the flat path: `https://cdn.cloudflare.steamstatic.com/steam/apps/<appid>/library_600x900.jpg`.
- Newer titles moved to hashed paths, so a 4xx on the flat path is not "no art". `ResolveAssetUrl` asks
  `IStoreBrowseService/GetItems/v1/` (app id only) for the asset names and `FetchAsset` requests
  `https://shared.steamstatic.com/store_item_assets/steam/apps/<appid>/<hash>/<file>`.
- Fetch and WIC decode run on a worker thread. `Warm` and `Prefetch` queue covers a couple of rows ahead
  of what is on screen.
- The disk cache is capped: 48 MB under `DevelopmentFiles` (`kDiskCapDev`), 6 MB when it has to live in
  `LocalState` (`kDiskCapLocal`). At most 44 covers and 3 hero images are kept decoded.
- A title with no art gets an "absent" marker so it is not asked for on every start. Markers expire
  after 14 days (`kAbsentDays`, `ForgetStaleAbsences`).

## Downloading a game

`Acquire` in the add-on calls `ghdl::DownloadApp(app, outdir, branch, token, sid, account, cb, ctx,
verify)`. One CM connection is used for the whole download.

### Steps

1. **App info.** `pics_access_token` then `pics_product_info` fetch the app's product info (PICS). The
   reply is either inline or a pointer to a gzip'd text file that is fetched over HTTP. The text VDF is
   parsed by `parse_app_depots_vdf`.
2. **Depot selection.** `get_app_depots` keeps depots for the chosen branch and skips depots marked for
   other operating systems, non-English language depots and low-violence variants. A depot that points
   at another app (`depotfromapp`) is resolved under the owning app. Depot ids listed in the title
   config's `steamSkipDepots` are left out. The branch comes from the title config's `steamBranch`
   (default `public`).
3. **Keys and manifests, per depot.** `get_depot_key` asks Steam for the depot's decryption key. A depot
   Steam will not give a key for (not owned, or unowned DLC) is skipped. `get_manifest_code` calls
   `ContentServerDirectory.GetManifestRequestCode#1`; `get_content_servers` calls
   `ContentServerDirectory.GetServersForSteamPipe#1`. The manifest is then a plain GET:
   `/depot/<depot>/manifest/<gid>/5/<code>`. It arrives zipped; `parse_manifest` reads the file list,
   decrypting file names with the depot key when they are encrypted.
4. **Merge.** `merge_file_entries` combines the depots' file lists into one.
5. **Space check.** See "Disk space" below.
6. **Files.** Files are written in priority order (`file_write_prio`): what the game needs to start
   comes first. Each file is a list of chunks; each chunk is `GET /depot/<depot>/chunk/<sha1 hex>`.
7. **Marker.** `write_install_marker` writes `gh_install.txt` (`app=<id>` and `complete=0|1`) in the
   install folder. The launcher shows a tree with `complete=0` as incomplete and offers to resume.

### Decoding a chunk

`decode_chunk`:

1. AES-256 decrypt with the 32-byte depot key. The first 16 bytes are the IV, itself ECB-encrypted; the
   rest is CBC with PKCS7 padding.
2. Decompress according to the magic at the start of the plaintext:

| Magic | Format | Decoder |
| --- | --- | --- |
| `VSZa` | zstd | `corpus/downloader/vendor/zstddeclib.c` |
| `VZ` | LZMA | `corpus/downloader/vendor/LzmaDec.c` |
| `PK\x03\x04` | zip / deflate | zlib inflate |
| none | stored | — |

3. `fetch_decode_chunk` checks the result against the manifest (SHA-1, or the chunk's Adler-32 checksum).
   A chunk that does not match is discarded and counted as a failure; it is never written.

All decoding happens in-process on the console. `toolchain/steam_chunk_decode.py` is a small host-side
reference decoder for one chunk file, useful when debugging a format change:
`steam_chunk_decode.py <chunk.bin> <depot_key_hex> [expected_sha1]`.

### Resume, verify, repair

`write_or_repair_file` handles all three:

- **Resume.** A file whose size already matches is skipped. For a partial file, every chunk already on
  disk is hashed; good chunks are kept and only the rest are fetched. Existing files are never truncated.
- **Verify** (`GH_ACQ_VERIFY`). Each file is hashed whole against the manifest's content hash. A file
  that differs is repaired in place, chunk by chunk.
- **Cancel.** The host's `cancelRequested` is polled before every network request (`g_cancelFn`). A
  cancelled download leaves `complete=0` and can be resumed.

HTTP requests retry: two tries per host, eight in total across content servers (`http_fetch_retry`).

### Where files land

`installDir(app)` returns the existing folder for that app id if there is one, otherwise a new
`D:\DevelopmentFiles\gh_games\app_<id>\`. The launcher recognises an install again by its
`steam_appid.txt` and `gh_install.txt`.

### Disk space

- Before writing, the download size (minus bytes already present) is compared with free space, keeping
  2 GiB free (`g_dlReserve`). Not enough room returns `-9` before anything is fetched.
- If the drive fills mid-download (`ERROR_DISK_FULL`), the phase becomes "storage full" and the download
  stops, resumable once space is freed.

### Progress and results

Progress phases reported to the host: connecting, signing in, finding files, downloading (or checking
files / repairing), storage full, done, and `error-*`.

| `DownloadApp` result | Meaning |
| --- | --- |
| `0` | Complete |
| `> 0` | That many chunks failed; the tree is marked incomplete |
| `-1` / `-2` | Connect / logon failed |
| `-3` | No app info |
| `-4` | No content servers |
| `-8` | No depots this account can download |
| `-9` | Not enough disk space |

Log lines start with `GHDL` and are rate-limited (`g_logBudget`).

### Transport note

Manifests and chunks travel over HTTP(S) from Steam's content servers, and `http_fetch` does not treat a
certificate problem as fatal. Integrity of game data rests on the per-chunk SHA-1 from a manifest whose
request code came over the encrypted CM channel. Keep that in mind before reusing `http_fetch` for
anything that is not hash-checked.

## Workshop

Code: `corpus/downloader/cm_workshop.h`, and `WorkshopSync` / `WorkshopQuery` / `WorkshopSubscribe` in
the add-on. A title opts in with `"workshop": true` in its title config.

- A Workshop item for a SteamPipe game is a manifest in the app's own workshop depot, so it downloads
  through the same manifest and chunk code as a game.
- Everything goes through `PublishedFile.*` service methods on the user's own session: `GetUserFiles`
  (subscriptions), `GetDetails`, `QueryFiles` (browse and search), `Subscribe`, `Unsubscribe`.
- Items land in `<DevelopmentFiles>\gh_games\workshop\content\<app>\<item id>\`.
- Sync state is written to `LocalState\gh_workshop_<app>.txt`, one item per line:
  `id, manifest, size, time_updated, folder, title` (tab separated).
- Sync keeps a reserve free on the drive (default 4096 MB, minimum 512; override with
  `LocalState\gh_ws_reserve_mb.txt`). An item that does not fit is skipped, not half-written.
- In the launcher, starting a Workshop title syncs subscriptions first, and the Workshop browser lists
  Subscribed, Popular, Trending, Newest and Search. See [../launcher/library.md](../launcher/library.md).

How the running game learns about the synced items is in [in-game.md](in-game.md).

## Side-loading without Steam

`toolchain/deliver_game.py` pushes a game tree you already have from a PC to the console through Device
Portal. It is a development tool, is unrelated to the Steam add-on, and currently imports a helper
module that is not in the repository. See [../deploying.md](../deploying.md).
