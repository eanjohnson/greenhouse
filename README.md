<!-- Draft of the top-level README.md. Paths assume it sits at the repository root. -->

# Greenhouse

Greenhouse runs unmodified Windows PC games on an Xbox console in Dev Mode.

It is an ordinary UWP app. It does not modify the console, does not need an exploit
and does not leave retail mode's protections disabled: Dev Mode is Microsoft's own
supported way to run your own apps. Inside its app container, Greenhouse maps a
game's executable into its own process and supplies the parts of Windows the
container does not provide.

## What is in it

- **A loader** for 64-bit Windows games, and a 32-bit route for older ones.
- **Graphics support**: Direct3D 11 and 12 on the console's driver, a Direct3D 9
  implementation, and a shader translator that lowers newer shaders to what the
  console accepts.
- **A launcher** driven by the controller: a library wall, themes, an in-game
  overlay.
- **An on-device Steam client** so you can sign in with your own account and
  download games you own straight to the console. It is written from scratch; no
  Steamworks SDK is used.
- **Mod support** for games that support mods, with mods fetched by the user from
  Thunderstore.
- **A signed package repository** that delivers per-game configs, themes and the
  compatibility list without a new app build.

## Status

Experimental. A small number of games are playable, more reach their menus, many do
not start. The list of what has actually been verified, per console model, is
[`corpus/compat/compat.json`](corpus/compat/compat.json); see
[docs/compatibility.md](docs/compatibility.md) for how to read it.

Honest limits:

- **Memory.** A Dev Mode app gets about 5 GB. Large modern games do not fit, and
  no setting changes that.
- **The graphics driver.** The console driver accepts an older shader model than
  current PC games target. Translation covers a lot, not everything.
- **DRM and anti-cheat.** Games wrapped in third-party DRM or protected by kernel
  anti-cheat do not run. This project does not attempt to defeat either and will
  not accept contributions that do.
- **Performance** varies from native-like to unplayable, by game.
- **Build scripts** take machine-specific values from environment variables, but
  a few helper scripts they call (the Device Portal client `wdp.py` and the Steam
  shim's `.def` generator `gen_steamdef.py`, among others) are not in the repository
  yet. [docs/building.md](docs/building.md) lists them as a known gap, to be added,
  along with every other assumption.
- Details: [docs/reference/known-limits.md](docs/reference/known-limits.md).

## Requirements

- An Xbox One, Series S or Series X **in Dev Mode** (requires a Microsoft developer
  account).
- **Your own legally owned games.** Greenhouse includes no games and no way to
  obtain games you do not own.
- A Linux machine to build on: LLVM (`clang-cl`, `lld-link`), Python 3,
  `osslsigncode`, the Windows SDK fetched with `xwin`, and `makemsix` built from
  Microsoft's msix-packaging sources.
- The Microsoft VC++ runtime and DirectX Shader Compiler DLLs in `corpus/redist/`
  (downloaded from Microsoft; see [docs/building.md](docs/building.md#corpusredist)).
- The console and the build machine on the same network.

## Quick start

```
# 1. Fetch the prerequisites that are not in the repository (Windows SDK,
#    makemsix, corpus/redist/, the stage directory, your signing key, and the
#    helpers in $GH_TOOLS): see docs/building.md

# 2. Build, pack and sign, without touching a console
export GH_TOOLS=<dir with gen_steamdef.py>   # see docs/building.md, known gap
export GH_XBOX=<console-ip>:11443
export GH_PFX_PASS=<your signing key passphrase>
GH_PACK_ONLY=1 GH_BUNDLE_FIRSTPARTY=0 toolchain/iterate-launcher.sh 0.1.0.0 0.0.0.0

# 3. Install artifacts/GreenhouseLauncher.signed.msix through the console's
#    Device Portal (https://<console-ip>:11443), then start "Greenhouse".
```

Then sign in to Steam from the launcher by scanning the QR code with the Steam
mobile app, pick a game you own, and install it. Full instructions:
[docs/building.md](docs/building.md) and [docs/deploying.md](docs/deploying.md).

## Documentation

Start at [docs/README.md](docs/README.md). It has reading orders for building and
running, for fixing a game, and for AI coding agents.

| Topic | Document |
|---|---|
| Build | [docs/building.md](docs/building.md) |
| Deploy and debug on a console | [docs/deploying.md](docs/deploying.md) |
| How it works | [docs/architecture/overview.md](docs/architecture/overview.md) |
| Fixing a game | [docs/contributing/fixing-a-game.md](docs/contributing/fixing-a-game.md) |
| Launcher and themes | [docs/launcher/README.md](docs/launcher/README.md) |
| Steam | [docs/steam/README.md](docs/steam/README.md) |
| Packages and title configs | [docs/packages.md](docs/packages.md) |
| Mods | [docs/mods.md](docs/mods.md) |
| Compatibility list | [docs/compatibility.md](docs/compatibility.md) |

## Contributing

The most useful contribution is making one more game work and recording the result.
Read [docs/contributing/fixing-a-game.md](docs/contributing/fixing-a-game.md).
Fixes that help a whole class of games are preferred to special cases for one.

Contributions must not include game files, proprietary binaries, Steamworks SDK
material, credentials, or anything aimed at DRM or anti-cheat.

## Legal

- Greenhouse is an independent project. It is **not affiliated with, endorsed by or
  sponsored by Microsoft, Xbox, Valve, Steam**, or any game publisher or developer.
  Product names are trademarks of their owners and are used only to describe
  compatibility.
- The repository contains **no game content and no proprietary binaries**. Runtime
  components that belong to other companies are obtained by the user from the
  vendor, or fetched on the console from the vendor's own servers; they are not
  redistributed here.
- Greenhouse **does not circumvent DRM or anti-cheat**. Games that depend on such
  protection do not run and are listed as such.
- Use it only with software you have the right to run, and in accordance with the
  agreements that apply to your console and your accounts. You are responsible for
  that.
- The software is provided as is, without warranty.

## License

Greenhouse is free software, licensed under the **GNU General Public License, version 2 or (at your option)
any later version** (GPL-2.0-or-later). See [LICENSE](LICENSE).

Third-party code in the tree keeps its own licence: the QR code generator
(`corpus/vendor/qrcodegen/`) is MIT, the zstd decoder
(`corpus/downloader/vendor/zstddeclib.c`) is BSD-3-Clause (dual-licensed with
GPL-2.0), the LZMA SDK files in `corpus/downloader/vendor/` are public domain, and
`src/loader/d3d9on12ddi.h` is MIT. The optional JIT build links BoxedWine, which is
GPL-2.0-or-later and is fetched by `toolchain/build_bwjit.sh` rather than vendored;
our changes to it are `toolchain/boxedwine.patch`. Full list:
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
