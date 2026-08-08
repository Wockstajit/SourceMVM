# SOURCE:MVM

Make movies out of your CS2 demos, without leaving the game.

Browse your demos, fly a camera around, put it on a path, mess with skins and effects, and line up
a render — all from a proper UI built right into CS2's menus. It's a fork of
[HLAE / advancedfx](https://github.com/advancedfx/advancedfx), pointed squarely at CS2.

**This is a beta.** Stuff will break. Tell me when it does.

---

## Get it

Grab one from [Releases](https://github.com/Wockstajit/SourceMVM/releases). Two flavors, same app:

| | |
| --- | --- |
| **`SourceMovieMaker-Setup-*.exe`** | Normal installer. Start menu shortcut, updates itself. Get this one. |
| **`SourceMovieMaker-Portable-*.exe`** | One file, no install. Keeps everything in a folder next to itself, so it runs fine off a USB stick. Doesn't self-update — just download a new one. |

Then:

1. Run it. It finds CS2 on its own.
2. Hit **INSTALL RUNTIME**. It pulls down the hook and the FX pack for you.
3. Hit **GO**.

That's it. Nothing to copy into your game folder, nothing to run as admin, no separate injector to
babysit — the launcher does all of that itself.

## You'll need

- Windows 10 or 11, 64-bit
- CS2 on Steam
- ~400 MB of disk for the runtime and FX pack
- Only if you want **Dev mode**: the free
  [CS2 Workshop Tools](steam://install/2279721) DLC. The launcher notices if it's missing and
  gives you a one-click install button.

## Heads up

**Offline demos only.** This injects a DLL, so it launches CS2 with `-insecure` and can't join
secured servers. Don't try to use it online — that's on you.

**Flashing images.** Some of the effects and render passes strobe. Worth knowing if that's a
problem for you.

## Updating

**CHECK FOR UPDATES** at the bottom of the launcher handles everything — it grabs a newer runtime
if there is one, then checks whether the launcher itself is out of date.

The beta ships as GitHub pre-releases, so this page's "Latest release" badge will look empty for a
while. That's on purpose. The launcher knows to look for pre-releases.

## What it actually downloads

The installer is just the launcher. Everything that goes *into* CS2 comes down separately, so a
launcher update doesn't mean re-downloading a few hundred megs of particles:

- **`source-mvm-runtime-*.zip`** — the hook, the injector, shaders and script snippets
- **`source-mvm-fx-*.zip`** — the particle FX pack
- **`payload-manifest.json`** — sizes and SHA-256s for the two above

Everything gets checked against its hash before it's installed, and your old copy only gets
replaced once the new one passes. It all lands in `%APPDATA%\SourceMovieMaker\payload\` (or next
to the exe, if you're running portable), so uninstalling takes it with it.

Prefer doing it by hand? Unpack `source-mvm-runtime` into `…\payload\runtime\` and
`source-mvm-fx` into `…\payload\fx\`.

## Something broke?

Open an [issue](https://github.com/Wockstajit/SourceMVM/issues). Helps a lot if you include:

- your launcher version
- the runtime version it shows after an update
- if the game itself misbehaved, the newest log from `%APPDATA%\HLAE\debuglogs\`

## About the source

This repo is just the releases — the builds and the runtime, no code. The source isn't public yet;
it will be once the beta settles down. Nothing shady, it's just messy in there right now and I'd
rather not ship a mess.

Built on [advancedfx](https://github.com/advancedfx/advancedfx), which is MIT — see
[LICENSE](LICENSE). Huge thanks to that project; none of this exists without it.
