# Building SOURCE:MVM

SOURCE:MVM is a Windows project. The game-side development build covers only the CS2 / Source 2
stack: `AfxHookSource2`, its shaders, snippets and runtime, the demo helpers, and FX staging. The
standalone `SourceMovieMaker.exe` launcher builds separately, so you can change it without
rebuilding CS2.

The inherited full HLAE multi-arch package (Win32 GUI, GoldSrc, Source 1 and Source 2) is still
available through `build-hlae.bat` and `CMake/MultiBuild.cmake`.

Use [launch.bat](launch.bat) for the built GUI, [live.bat](live.bat) for direct CS2/NetCon, or
[demo.bat](demo.bat) for demo automation. None of them requires or installs DLSS 5. The direct and
demo paths bootstrap standard runtime packages when local binaries are missing.
[Portable setup](Automation/setup/README.md) covers first-run downloads, private-repository
credentials and updating the packaged build.

## Prerequisites

The environment ray backend also requires the Windows SDK x64 `dxc.exe` compiler (shader model
6.5). CMake finds it in the installed Windows Kits. Set `MVM_DXC` if it is installed elsewhere.
DXC is used at build time only. Hardware ray tests require a DXR 1.1 GPU and skip otherwise.

Install:

- Git, including submodule support, and **Git LFS** (`git lfs install` once per machine, before
  cloning), which fetches the large Assets audio and images listed in `.gitattributes`;
- Visual Studio 2022 with:
  - **Desktop development with C++**,
  - **.NET desktop development**,
  - the **.NET Framework 4.6.2 targeting pack**;
- CMake 3.24 or newer;
- Node.js 24 LTS;
- Python 3.8 or newer with the `py` launcher and `pip`;
- GNU gettext for Windows;
- Rust/Cargo plus the Win32 MSVC target. Some inherited tooling still needs it. The Source 2
  preset itself builds the x64 Rust hook crate:

  ```batch
  rustup target add --toolchain stable-x86_64-pc-windows-msvc i686-pc-windows-msvc
  ```

- Go, used by the demo-info and legacy-demo-repair helpers.

Clone this repository, not the upstream advancedfx repository URL, and initialize submodules:

```batch
git clone --recurse-submodules <this-repository-url>
cd "<clone-directory>"
```

For an existing checkout:

```batch
git submodule update --init --recursive
git lfs pull
```

[Collaborator setup](Docs/setup/collaborator-setup.md) is the short version of this page for a new
machine. It has a table of where each asset lives.

## Machine paths

You do not need any of this for a normal build. The build is self-contained. The launcher and the
`Automation\launch\*` scripts find Counter-Strike 2 on their own by reading Steam's registered
install path and every library listed in `steamapps\libraryfolders.vdf`. Set a variable only when
that answer is wrong, or when a tool needs something Steam does not know about.

| Variable | Used by | When to set it |
| --- | --- | --- |
| `SOURCEMVM_CS2_DIR` | launch + verify scripts, Python tools | CS2 is somewhere Steam does not list, or several installs exist and you want a specific one. Point it at the install **root** (the folder containing `game\bin\win64\cs2.exe`). |
| `SOURCEMVM_VRF_CLI` | FX conversion and asset-research tools | Only for rebuilding converted FX packs or asset research. Map lighting data does not need it, because the hook extracts maps natively. Defaults to a [ValveResourceFormat](https://github.com/ValveResourceFormat/ValveResourceFormat) checkout sitting **next to** this repository, i.e. `..\ValveResourceFormat\CLI\bin\Release\Source2Viewer-CLI.exe`. |
| `GMOD_DIR`, `INSURGENCY_DIR`, `SOURCEMVM_INSURGENCY2_DIR` | FX source extraction | Only when re-extracting a pack from its donor game into the local `FX\sources\` (not in the repository, see [FX/README.md](FX/README.md)); a normal FX rebuild reads that local tree instead. |

Every one of these is also available as an explicit script parameter (`-Cs2Dir`, `-VrfCli`,
`--game`, …), which takes precedence over the environment.

The shared resolvers behind this are `Automation\lib\Resolve-Cs2Dir.ps1` (PowerShell) and
`Automation\lib\steam_paths.py` (Python). Use them rather than writing a new path guess. A
hardcoded library path works on one machine only.

## Launcher-only build (CS2 untouched)

The standalone Electron/React launcher has its own build path. From the repository root:

```batch
SourceMovieMakerLauncher\build-app.bat
```

This installs the launcher npm dependencies, builds the React renderer, and builds only the small
.NET injection host. It does **not** configure CMake or build `AfxHookSource2`, shaders, Rust, or
any other CS2 target.

To create a Windows installer and unpacked app:

```batch
SourceMovieMakerLauncher\build-app.bat -Package
```

The unpacked executable is written to `SourceMovieMakerLauncher\release\win-unpacked\SourceMovieMaker.exe`.

GitHub release metadata is already configured in `SourceMovieMakerLauncher/package.json`. Tagged
release automation can publish the installer, and `electron-updater` can offer updates from the
launcher footer.

## Normal development build (CS2 / Source 2)

Run from the repository root:

```batch
build.bat
```

`build.bat` delegates to `build-source2.bat`. The script:

1. asks whether to rebuild particle assets;
2. closes running `cs2.exe` and `hlae.exe` so staged files are not locked;
3. runs the Source 2-only CMake coordinator (`CMake/Source2Build.cmake`) into `build/source2-release`;
4. builds and stages `FilmmakerDemoInfo.exe`, `FilmmakerDemoRepair.exe` and `FilmmakerDemoKeyframes.exe`;
5. stages the local FX pack (`FX/compiled/source_mvm_fx`, not in the repository: see
   [FX/README.md](FX/README.md); without it CS2 shows its stock particles), the render materials
   and the SOURCE:MVM startup video;
6. launches the local CS2/netcon workflow (pass `-NoLaunch` to skip).

The optional asset prompts default to **No**. Retail CS2 does not ship `resourcecompiler.exe`,
so rebuilding particles or render materials needs CS2 Workshop Tools. Without them, the build
stages the local compiled pack unchanged. A rebuild republishes into the local `FX/compiled`,
which is never committed.

Map lighting data is not part of the build. The hook extracts each map on first load
([map data extraction](Docs/effects/map-data-extraction.md)). `MapDataTool.exe`, built with the
hook, runs the same extractor from the command line.

Build the launcher separately before using `launch.bat`. It starts the unpacked Electron app from
`SourceMovieMakerLauncher\release\win-unpacked`.

### Standalone launcher CLI

The packaged `SourceMovieMaker.exe` can run without showing its UI. It starts CS2 suspended, uses
the supplied x64 injector to load `AfxHookSource2.dll`, then resumes CS2 without starting HLAE.
Useful flags for automation:

```text
--headless
--cs2 <cs2.exe>
--hook <AfxHookSource2.dll>
--injector <x64\injector.exe>
--cmd-line "<full cs2 command line>"
--env KEY=VALUE   (repeatable)
```

The `-noGui` / `-autoStart` spellings are accepted as headless aliases. Interactive settings
persist under `%APPDATA%\SourceMovieMaker\launcher.json`.

## Fast CS2 hook-only iteration

For changes confined to `AfxHookSource2`:

```batch
cmake --preset x64-source2-release
cmake --build --preset x64-source2-release --parallel --target AfxHookSource2
cmake --install build/x64-source2-release --config Release --prefix build/source2-release --component x64-runtime
cmake --install build/x64-source2-release --config Release --prefix build/source2-release --component x64-source2
```

Available presets:

- `x64-source2-release` / `x64-source2-debug`: default CS2 stack (`AFX_SOURCE2_ONLY=ON`)
- `x64-release` / `x64-debug`: x64 hooks for Source 1 and Source 2
- `win32-release` / `win32-debug`: Win32 HLAE GUI, GoldSrc and Source 1

## Tests

Native test targets are registered in [`CMake/Source2Tests.cmake`](CMake/Source2Tests.cmake),
separately from the hook and runtime targets. Configure and build the Source 2 Release tree, then
run the registered native tests:

```batch
cmake --preset x64-source2-release
cmake --build --preset x64-source2-release --parallel
ctest --test-dir build/x64-source2-release -C Release --output-on-failure
```

Run the repository-owned JavaScript and documentation checks separately:

```batch
node --test Automation/tests/documentation-integrity-tests.mjs
node --test Automation/tests/panorama-interaction-layout-tests.mjs
node --test Automation/tests/launch-demo-tests.mjs
```

The launch-demo test uses local temporary files and a loopback netcon server. It does not launch
CS2. Hook selection and staging have isolated filesystem tests, which also run without a game:

```batch
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File Automation/tests/cs2-hook-staging-tests.ps1
```

Run native builds and CTest sequentially so active test executables do not block the linker.

After changing the map extractor, compare its output with a reference extraction of the same game
files (see [map data extraction](Docs/effects/map-data-extraction.md#verification)):

```batch
py -3 Automation/verify/compare-map-data.py <reference-dir> <candidate-dir> de_inferno de_dust2
```

## Legacy full HLAE package

Cross-architecture HLAE packaging lives in `CMake/MultiBuild.cmake`:

```batch
build-hlae.bat
```

or:

```batch
cmake -DAFX_MULTIBUILD_STAGING=ON -P CMake/MultiBuild.cmake
```

That path stages into `build/staging-release` and produces `HLAE.exe`. Prefer `build.bat`
for SOURCE:MVM CS2 work.

## Rust dependency updates

Cargo builds use `--locked` by default. Only disable that while intentionally updating
`Cargo.lock`:

```batch
cmake --preset x64-source2-debug -DAFX_RUST_CARGO_LOCKED=FALSE
```

The setting stays in that build directory's CMake cache until you change it or remove the cache.

## Release maintenance

Before an upstream-style HLAE release (legacy path):

- update the version in `HLAE/CMakeLists.txt`;
- update the GUIDs in `HLAE/UpdateCheck.cs`;
- update the package/bundle versions in the WiX sources under `Installer/`;
- regenerate `CREDITS.md` with `py -3 Tools\make_credits.py`;
- verify Rust license metadata when dependencies changed;
- run the full native and JavaScript test commands above.
