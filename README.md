<h1 align="center">SOURCE:MVM</h1>

<p align="center">
  <strong>A Windows toolkit for cinematics, demo replays and camera work inside the game.</strong><br>
  Built on <a href="https://github.com/advancedfx/advancedfx">HLAE / advancedfx</a>, with native Panorama UI drawn inside the running game.
</p>

<p align="center">
  <a href="https://github.com/Wockstajit/Source-MVM/releases"><img src="https://img.shields.io/badge/Download-Releases-2ea44f?style=for-the-badge&logo=github" alt="Download releases"></a>
  <a href="https://github.com/Wockstajit/Source-MVM/issues/new/choose"><img src="https://img.shields.io/badge/Report%20an%20Issue-d73a4a?style=for-the-badge&logo=github" alt="Report an issue"></a>
  <a href="https://github.com/sponsors/Wockstajit"><img src="https://img.shields.io/badge/Sponsor-db61a2?style=for-the-badge&logo=githubsponsors" alt="Sponsor"></a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/License-MIT-blue?style=flat-square" alt="MIT license">
  <img src="https://img.shields.io/badge/Platform-Windows%2010%2F11-0078d4?style=flat-square" alt="Windows 10 and 11">
  <img src="https://img.shields.io/badge/Game-Counter--Strike%202-1b2838?style=flat-square" alt="Counter-Strike 2">
  <img src="https://img.shields.io/badge/Deadlock-planned-lightgrey?style=flat-square" alt="Deadlock planned">
</p>

<p align="center">
  <img src="Assets/readme/trailer.webp" alt="SOURCE:MVM trailer: custom Panorama UI, scene graph, animated sun, lens flares and depth of field, weapon recoil and bob" width="800">
</p>

---

## What it is

SOURCE:MVM turns Counter-Strike 2 into a cinematic workspace. You browse demos, build camera
moves, light the map, tune weapon feel and record your movie without leaving the game. The interface
is native Panorama UI, drawn inside CS2 by the hook DLL.

Everything here is for **offline demo and movie work**. The hook is never meant for online or
VAC-protected servers.

## Features

**Camera and direction**

- **Camera Editor** with a live preview, camera timeline, path editor, graph editor and persistent
  workspace preferences. See the [camera editor guide](Docs/features/camera-editor-feature.md).
- **Follow and Attach cameras** that track a player through the demo. See the
  [follow camera docs](Docs/follow-camera/README.md).
- **Movie director controls**: first-person, third-person orbit and free camera.
  See [movie controls](Docs/features/filmmaker_movie_controls.md).
- **Live demo scrubbing**, so the world follows timeline and demo-bar drags.

**Look and lighting**

- **Environment workspace** with runtime map lighting, sun shadows, smoke illumination and
  event-driven VFX lights. It works on official and Workshop maps, and the hook extracts each map's
  lights and geometry on first load. See the [environment editor](Docs/features/environment-scene-editor.md)
  and [whole-map relighting](Docs/effects/whole-map-relighting.md).
- **Lens system**: depth of field with a bokeh iris, plus in-engine lens flares.
  See [lens](Docs/features/filmmaker_lens.md).
- **Particle FX, ragdolls and death animations**, precached so toggles do not hitch.
  See [death animations](Docs/features/filmmaker_death_animations.md) and
  [ragdoll physics](Docs/effects/ragdoll-physics.md).

**Feel and motion**

- **Weapon recoil and weapon bob** tuned in the config panel, plus Smooth View, handheld shake,
  fisheye and camera shake. See the [config panel](Docs/features/filmmaker_config_panel.md).

**Rendering**

- **Render workspace** for range, camera, game HUD, output and validation setup.
- **Render passes**: Beauty, Depth and Normal, with visibility control and synchronized multipass
  capture. See [render passes](Docs/features/filmmaker_render_passes.md).
- **Recording FPS control** for the capture rate. See [render FPS](Docs/features/filmmaker_render_fps.md).

**Demos and cosmetics**

- **Demo browser** with scoreboards, event metadata, repair support and saved folders.
  See [demo browser](Docs/features/filmmaker_demos_feature.md).
- **Offline cosmetics** for weapon skins, knives, gloves and agents, applied per player in demos.

> [!NOTE]
> Features are still in active development. Each one needs live CS2 verification after a game
> update. If several systems break at once, run `mirv_filmmaker health` first.

## Game support

| Game | Status | Launcher |
| --- | --- | --- |
| Counter-Strike 2 | Active development. The main target today. | Electron launcher and CS2 automation scripts |
| Deadlock | Planned. Work is on the roadmap, with a separate automation launcher today. | [Deadlock port notes](Docs/deadlock/README.md) |
| Counter-Strike: Source, Counter-Strike 1.6 and other Source 1 games | Planned. Will eventually run through the SOURCE:MVM launcher. | Not yet available |

## Getting started

**Players:** download the latest build from [Releases](https://github.com/Wockstajit/Source-MVM/releases),
keep Steam open and signed in, then launch CS2 through the SOURCE:MVM launcher.

**Developers:** clone the repository with Git LFS and submodules, then build:

```batch
git lfs install
git clone --recurse-submodules https://github.com/Wockstajit/Source-MVM.git
cd Source-MVM
build.bat
```

The full setup, prerequisites and the launcher-only build are in
[collaborator setup](Docs/setup/collaborator-setup.md) and [BUILDING.md](BUILDING.md).

## Documentation

- [Documentation index](Docs/README.md): every feature contract, grouped by area.
- [Local quickstart](Docs/features/local-quickstart.md): your first launch.
- [Repository guidance](CLAUDE.md): architecture, threading model and runtime diagnosis.

## Contributing

Bug reports, fixes, documentation and feature ideas are welcome. Read
[CONTRIBUTING.md](CONTRIBUTING.md) and the [GitHub workflow policy](Docs/setup/git-workflow.md)
first. AI-assisted contributions are welcome if you disclose them and test your changes.

To report a security issue, use [GitHub Security Advisories](https://github.com/Wockstajit/Source-MVM/security/advisories/new)
instead of a public issue. See [SECURITY.md](SECURITY.md).

## Safety

> [!WARNING]
> **VAC:** this tool injects a hook DLL into the game. Use it only for offline demo and movie work,
> and never join VAC-protected or online servers while it is loaded.

> [!CAUTION]
> **Epilepsy:** the tool can produce fast-changing images and colours. Use caution while previewing
> or recording.

## License and credits

SOURCE:MVM is released under the [MIT License](LICENSE). It builds on the advancedfx code, which is
also MIT. Attribution is in [CREDITS.md](CREDITS.md). Third-party libraries keep their own licenses,
listed in [Misc/THIRDPARTY.yml](Misc/THIRDPARTY.yml) and the bundled license files.

Counter-Strike 2, Deadlock, Stratum2 and Panorama belong to Valve. Game material is not included in
this repository.

For upstream HLAE releases, the manual and community support, see
[advancedfx/advancedfx](https://github.com/advancedfx/advancedfx).
