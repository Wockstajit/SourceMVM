# Repository contents

SOURCE:MVM keeps the multi-engine advancedfx tree. Current filmmaker work is in the CS2 and
Deadlock hooks.

## Active SOURCE:MVM areas

- [`AfxHookSource2/`](AfxHookSource2): x64 Source 2 / CS2 hook DLL.
- [`AfxHookSource2/Filmmaker/`](AfxHookSource2/Filmmaker): demo browser, main-menu Play page and
  workshop map sorting, Panorama workspaces, camera systems, cosmetics, render preflight,
  effects, and settings. [`MapData/`](AfxHookSource2/Filmmaker/MapData) is the native map
  extractor (lights, collision, shadow casters). It also builds `MapDataTool.exe`.
- [`Automation/`](Automation): launchers, netcon clients, live verifiers, capture tools, and
  source-level tests. Native tests are registered in [`CMake/Source2Tests.cmake`](CMake/Source2Tests.cmake).
  Launch scripts share hook-build selection and staging through [`AutomationCommon.ps1`](Automation/lib/AutomationCommon.ps1).
- [`AfxHookSource2/Filmmaker/Debug/`](AfxHookSource2/Filmmaker/Debug): project-wide runtime logging
  and bounded event deduplication, shared by CS2 and Deadlock. It is not cosmetics-specific.
- [`UiMockup/`](UiMockup) and [`UiMockupDeadlock/`](UiMockupDeadlock): browser verification
  of the embedded Panorama scripts. Neither is an independent implementation or a design-handoff export.
- [`Docs/`](Docs): maintained feature documentation, agent contracts, archived research, and
  inherited engine notes. Start at [`Docs/README.md`](Docs/README.md).
- [`FX/`](FX): CS2 particle FX conversion tools, our own particles and render materials, and
  the death-animation metadata. The donor material, the compiled pack and the baked animation
  library are not in the repository ([FX/README.md](FX/README.md)).
- [`FilmmakerDemoInfoGo/`](FilmmakerDemoInfoGo): demo scoreboard and event parser, plus standalone
  verification scanners that share its Go module. [`smokescan/`](FilmmakerDemoInfoGo/smokescan) owns
  smoke-tick scanning, with no duplicate implementation under `Automation/verify/`.
- [`FilmmakerDemoRepairGo/`](FilmmakerDemoRepairGo): legacy CS2 demo repair helper.
- [`FilmmakerDemoKeyframesGo/`](FilmmakerDemoKeyframesGo): writes keyframed demo copies for one-frame backward seeks.
- [`Assets/`](Assets): versioned SOURCE:MVM media masters.

- [`SourceMovieMakerLauncher/`](SourceMovieMakerLauncher): standalone Electron/React CS2
  launcher and native injection host (`SourceMovieMaker.exe`).
- [`AfxHookSource2/Deadlock/`](AfxHookSource2/Deadlock): Deadlock integration. Its features live
  in [`Filmmaker/Deadlock/`](AfxHookSource2/Filmmaker/Deadlock).

## Shared and inherited advancedfx areas
- [`HLAE/`](HLAE): inherited C# HLAE GUI (legacy package only).
- [`AfxHookSource/`](AfxHookSource): inherited Source 1 hooks.
- [`AfxHookGoldSrc/`](AfxHookGoldSrc): inherited GoldSource hooks.
- [`AfxCppCli/`](AfxCppCli): legacy C++/CLI utilities.
- [`Shared/`](Shared): native code shared by hook families.
- [`Lib/`](Lib) and [`Misc/`](Misc): Rust libraries, mirv-script, and developer utilities.
- [`ShaderBuilder/`](ShaderBuilder), [`ShaderDisassembler/`](ShaderDisassembler), and
  [`Shaders/`](Shaders): shader build and inspection tools.
- [`Installer/`](Installer) and [`Resources/`](Resources): packaging and installed resources.
- [`Deps/`](Deps): submodules and vendored/upstream dependencies ([list](Deps/README.md)). Their
  documentation and licenses are third-party material and should not be rewritten as SOURCE:MVM docs.

Generated output belongs under ignored `build/`, `Automation/runs/`, or `Automation/output/`
paths and must not be committed. Large binary assets that are committed (the compiled FX pack,
the ragdoll animations and their donor files) go through Git LFS per
[`.gitattributes`](.gitattributes); see [collaborator setup](Docs/setup/collaborator-setup.md).
