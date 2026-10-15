# Collaborator setup

The repository holds everything needed to build and launch SOURCE:MVM: source, vendored
dependencies, git submodules, the particle-FX donor trees and their compiled pack, the render
materials, and the ragdoll death-animation library with the donor animations it is built from.
Map lighting data is not in git at all. The hook extracts it from your installed CS2 maps the
first time each map loads ([map data extraction](../effects/map-data-extraction.md)).

## 1. Install

- Git for Windows **with Git LFS** (included in the standard installer). Run this once per machine:

  ```batch
  git lfs install
  ```

- Visual Studio 2022 (Desktop C++ and .NET desktop workloads, .NET Framework 4.6.2 targeting
  pack), CMake 3.24+, Node.js 24 LTS, Python 3.8+ with `py`, GNU gettext, Rust/Cargo and Go.
  [BUILDING.md](../../BUILDING.md) has the details.
- Steam with Counter-Strike 2 installed and signed in.

## 2. Clone

```batch
git clone --recurse-submodules https://github.com/Wockstajit/SourceMVM.git
cd SourceMVM
git lfs pull
```

`git lfs pull` does nothing when LFS was installed before cloning. If the osu! easter-egg files
are tiny text pointers, LFS was not active. Run `git lfs install`, then `git lfs pull`. For a
checkout made without `--recurse-submodules`, run `git submodule update --init --recursive`.

The particle packs and death animations are other games' material and are not in the repository
([FX/README.md](../../FX/README.md)). A clone builds and runs without them, using CS2's stock
particles and no death-animation clips. To work on them, copy `FX/sources/`, `FX/compiled/` and
`FX/death-animations/ragdoll_animations.mvmanim` from the maintainer's private copy.

## 3. Build and launch

```batch
build.bat
```

This builds the hook, demo helpers and render materials into `build/source2-release`, stages the
committed FX pack, and launches CS2 with netcon on port 29010. Answer **No** to the FX rebuild
prompt unless you changed FX sources. Afterwards, use [`live.bat`](../../live.bat),
[`demo.bat`](../../demo.bat) or [`launch.bat`](../../launch.bat). The launcher GUI needs
`SourceMovieMakerLauncher\build-app.bat -Package` once. See [first launch](../features/local-quickstart.md).

The first time you open a map, the Environment panel shows `Preparing map data` for 3-20 seconds
while lighting data is extracted. Later loads of that map are instant.

## What lives where

| Need | Location | Rebuild |
| --- | --- | --- |
| Hook and runtime | `AfxHookSource2/`, `Deps/` | `build.bat` |
| Compiled particle FX, render materials | `FX/compiled/source_mvm_fx/` (local only) | `FX/tools/convert-fx-source1.ps1 -Compile`, `FX/tools/build-render-materials.ps1` (both need CS2 Workshop Tools for `resourcecompiler.exe`) |
| FX donor assets | `FX/sources/` (local only) | see [FX/README.md](../../FX/README.md) |
| Ragdoll death animations | `FX/death-animations/ragdoll_animations.mvmanim` (local only) | `py -3 FX/tools/extract-death-anims.py` from the local `FX/sources/*-deaths` |
| Map lights, collision, shadow casters | `%APPDATA%\HLAE\source_mvm\environment\occlusion` (generated) | automatic on map load, or `MapDataTool extract <map>` |
| Demo projects (cameras, shakes, loops, scene keys) | `%APPDATA%\HLAE\source_mvm\demos\<demo>\project.mvm` (generated) | authored in game |

Generated output goes to the git-ignored `build/` and `Automation/runs/` folders. Large binary
files are tracked through LFS by the rules in [`.gitattributes`](../../.gitattributes). Add a rule
there before committing a new large binary type.

## Optional

- **DLSS 5** stays a manual experiment. Nothing installs it ([DLSS setup](../../Automation/dlss/README.md)).
- **Prebuilt runtime without compiling**: `live.bat` and `demo.bat` download the pinned runtime
  package when no local build exists ([portable setup](../../Automation/setup/README.md)). FX still
  comes from the local `FX/compiled`, when there is one.
