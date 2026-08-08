# SOURCE:MVM

A movie-making tool for Counter-Strike 2 demos, built on [HLAE/advancedfx](https://github.com/advancedfx/advancedfx).

This repository is the **release channel**. It carries the installer and the game-side runtime
that the launcher downloads — no source code. Development happens in a separate repository.

> **SOURCE:MVM is in beta.** Releases here are published as GitHub pre-releases. Expect rough
> edges, and expect the runtime to change often.

---

## Install

1. Download **`SourceMovieMaker-Setup-<version>.exe`** from
   [Releases](https://github.com/Wockstajit/SourceMVM/releases) and run it.
2. Open **SourceMovieMaker**. It finds your CS2 install automatically.
3. Press **INSTALL RUNTIME**. The launcher downloads the hook and the FX pack for you.
4. Press **GO**.

That is the whole setup. The launcher starts CS2, injects the hook itself, and mounts the FX
pack — there is nothing to copy into your game folder and nothing to run as administrator.

## Requirements

- Windows 10 or 11, 64-bit
- Counter-Strike 2 installed through Steam
- ~400 MB free disk space for the runtime and FX pack
- **Dev mode only:** the free
  [Counter-Strike 2 Workshop Tools](steam://install/2279721) DLC (appid `2279721`).
  The launcher checks for it and offers a one-click install when it is missing.

## Offline / demo use only

SOURCE:MVM is for offline demo and movie work. It launches CS2 with `-insecure`, so the session
cannot join VAC-secured servers. Do not use it to play online.

---

## What gets downloaded

The installer is just the launcher. Everything that goes *into* CS2 is fetched separately, so a
launcher update is a small download instead of a few hundred megabytes:

| Asset | What it is |
| --- | --- |
| `SourceMovieMaker-Setup-<version>.exe` | The launcher. Auto-updates itself. |
| `source-mvm-runtime-<version>.zip` | Hook DLL, injector, shaders and script snippets. |
| `source-mvm-fx-<version>.zip` | Particle FX pack. |
| `payload-manifest.json` | Versions, sizes and SHA-256 for the two zips above. |

Every download is checked against the SHA-256 in the manifest before it is installed, and the
previous copy is only replaced once the new one has passed. Everything lands under
`%APPDATA%\SourceMovieMaker\payload\`, so uninstalling the launcher removes it too, and no step
needs administrator rights.

You can download the zips by hand if you would rather not use the in-app updater — unpack
`source-mvm-runtime` into `%APPDATA%\SourceMovieMaker\payload\runtime\` and `source-mvm-fx` into
`%APPDATA%\SourceMovieMaker\payload\fx\`.

## Updating

**CHECK FOR UPDATES** in the launcher's bottom rail handles both halves: it installs a newer
runtime if there is one, then checks whether the launcher itself has a newer build. Because the
beta ships as pre-releases, the launcher opts into the pre-release channel — GitHub's "Latest
release" on this page stays empty until 1.0.

## Reporting problems

Open an [issue](https://github.com/Wockstajit/SourceMVM/issues). Please include your launcher
version, the runtime version shown after an update, and — if the game itself misbehaved — the
newest log from `%APPDATA%\HLAE\debuglogs\`.

## License

MIT, inheriting from [advancedfx](https://github.com/advancedfx/advancedfx). See
[LICENSE](LICENSE).
