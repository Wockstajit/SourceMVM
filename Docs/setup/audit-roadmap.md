# Repository Audit Roadmap & Batch Progress

This document tracks the multi-batch codebase audit and implementation plan for SOURCE:MVM so work can be resumed seamlessly across sessions or agents.

## Status Summary

| Batch | Branch | Scope | Status | Tracking Issue | Pull Request |
| --- | --- | --- | --- | --- | --- |
| **Batch 1** | `fix/audit-batch-1-stability-safety` | Critical bugs, D3D11 leaks/UAFs, Panorama UAFs, thread races, shutdown hangs, atomic saves, `save-reset` exFAT fix, leftover debug HTTP removal | `in_review` | TBD | TBD |
| **Batch 2** | `perf/audit-batch-2-low-end-pc` | Low-end PC zero-idle-cost rendering, MapData/VPK/TextureCache perf, idle VRAM reaper, single per-frame `FrameEntityCache`, `FilmmakerSettings` dirty check, `PlayingDemoPath` & save debounce | `pending` | — | — |
| **Batch 3** | `feat/audit-batch-3-launcher-perf-qol` | Launcher performance (pause WebM/blurs when CS2 runs, async main thread, remove 2s sleep), reliability (single-instance, update races, timeouts, DPI fix), QoL & unused asset cleanup | `pending` | — | — |
| **Batch 4** | `perf/audit-batch-4-panorama-ui-qol` | In-game Panorama UI speed (`WorkspaceShell` blur/WebM gating, hide-instead-of-teardown, split cursor from state JSON, `RunScriptCached`, `$.Schedule` cleanup, catalog/localization bloat), undo/redo & QoL, CI test wiring & `automation/` path fix | `pending` | — | — |
| **Deferred: Perf Mode** | — | Unified Low-Spec / Performance Mode toggle (`mirv_filmmaker perfmode` + UI) and GPU/VRAM budget HUD | `deferred` | — | — |
| **Deferred: Deadlock** | — | Deadlock bug fixes, performance, cross-game consolidation, and launcher parity | `deferred` | — | — |

See [Deadlock audit backlog](../deadlock/audit-backlog.md) for all deferred Deadlock items.

---

## Batch 1 — Critical Bugs, Crashes, Leaks & Thread-Safety Fixes

**Branch:** `fix/audit-batch-1-stability-safety`

- [x] `AfxHookSource2/RenderSystemDX11Hooks.cpp`: fix `CaptureSmokeDepth` / `CaptureNormalDepth` calling `OMGetRenderTargets` instead of `OMSetRenderTargets` (DSV unbind + 2 COM ref leaks/capture), make `g_bInOwnDraw` `thread_local`, and release `AfxCompositeEnvironmentBeforeScope` static target + `g_SmokeAbsorbance` on target/device teardown.
- [x] `AfxHookSource2/Filmmaker/World/CustomShadowAtlas.cpp`: hold an owning COM ref on `g_trackedAtlas` instead of raw non-owning pointer UAF.
- [x] `AfxHookSource2/Filmmaker/World/SkyGpuRotation.cpp`: guard `s_uploads` / `s_nextUpload` with a mutex and return `CachedUpload` by value from `FindUpload`.
- [x] Panorama HUD `Teardown()` UAFs (8 drivers): clear `m_bridge.SetContextPanel(nullptr)` before `Teardown()` when the HUD pointer is gone or changed (`CameraEditorHud.cpp`, `CameraTimelineHud.cpp`, `GraphEditorExperimentHud.cpp`, `MovieHud.cpp`, `WorkspaceHeaderHud.cpp`, `MarkerHud.cpp`, `AimCrosshairHud.cpp`, `FxDebugHud.cpp`).
- [x] Demo-pause teardown fix: accept `IsDemoPaused()` alongside `IsPlayingDemo()` in `MarkerHud.cpp`, `DemoBarButtons.cpp`, and `FxDebugHud.cpp`.
- [x] `AfxHookSource2/Filmmaker/Localization/Localization.cpp` & `ConfigHudJs.h`: release `AddDenyInputFlagsToGame` / pickers / `CamTimeline.restoreNative` and delete the 12 missing top-level sibling roots during epoch rebuild.
- [x] `AfxHookSource2/Filmmaker/Demo/DemoLibrary.{h,cpp}` & `Filmmaker.cpp`: serialize `StartScan` / `TryStartScan` / `Shutdown` with `m_workerMutex` so render and main threads never race on `m_worker`.
- [x] `AfxHookSource2/Filmmaker/Input/Keybinds.cpp` & `Onboarding/Onboarding.cpp`: replace racy `EnsureReady()` with `std::call_once` and synchronize WndProc-read onboarding state under `g_clickMutex`.
- [x] `AfxHookSource2/Filmmaker/Demo/DemoRepair.cpp`, `DemoKeyframes.cpp`, `DemoInfoHelper.cpp`: make `DemoRepair` and `DemoKeyframes` cancelable via job object termination on shutdown, and restrict `DemoInfoHelper.cpp` inherited handles via `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` + `BELOW_NORMAL_PRIORITY_CLASS`.
- [x] `AfxHookSource2/Filmmaker/Debug/MvmDebugLog.{h,cpp}` & `Cosmetics/*`: remove leftover Cursor localhost HTTP server (`MvmAgentLog`, `winhttp.lib`) and route remaining diagnostics through `MvmDebugLog_LinefAlways` when active.
- [x] `AfxHookSource2/Filmmaker/Platform/AtomicFile.h` rollout: replace raw `std::ofstream(..., std::ios::trunc)` and ad-hoc `.tmp` writers in `FilmmakerSettingsStore.cpp`, `DemoProjectStore.cpp`, `Takes.cpp`, `ParticleFxSettings.cpp`, `EasterEggs.cpp`, `DemoFileStore.cpp`, `DemoFolderStore.cpp`, `HiddenDemoStore.cpp`, `DemoOrganizerStore.cpp`, and `WorkshopOrganizerStore.cpp`.
- [x] `SourceMovieMakerLauncher/electron/save-reset.cjs` & `Automation/tests/launcher-save-reset-tests.mjs`: fall back from `fsp.link` to `fsp.copyFile(..., COPYFILE_EXCL)` on `EPERM`/`ENOTSUP`/`EXDEV` (exFAT/FAT32 drives) and add unit tests.
- [x] Found during verification: a language change while Config or the Camera Editor hid the game HUD left it hidden after closing. `CameraTimelineJs.h` now hands back the previous instance's hidden panels before a rebuild takes over.
- [x] Atomic saves also cover `Props.cpp`, `RelightLightEditor.cpp`, `OsuGame.cpp` and `ActorAnimations.cpp`.

---

## Batch 2 — Low-End PC Zero-Idle-Cost & VRAM Pass

**Target branch:** `perf/audit-batch-2-low-end-pc`

### GPU, Shaders, VRAM & MapData
- [ ] `AfxHookSource2/Filmmaker/Environment/EnvironmentNativeMaterialDerive.cpp`: defer background shader derivation until Environment/Relight is enabled (matching `ScaledColorCodeTag`), and gate/remove the permanent 512 MiB DXBC `programRecord()` dump cache unless `mvm_debug` / dump mode is requested.
- [ ] `AfxHookSource2/Filmmaker/Environment/EnvironmentScene.cpp` & `EnvironmentOcclusion.cpp`: defer `UpdateMapOcclusion` extraction and geometry decompression until an occlusion/lighting consumer is enabled.
- [ ] `AfxHookSource2/Filmmaker/MapData/MapDataIo.cpp`, `MapCasters.cpp`, `MapExtract.cpp`: cache open VPK file handles inside `Vpk::Read`, add an LRU/byte budget cap to `MapCasters::TextureCache` (plus `GdiplusShutdown`), and drop synchronous `MOVEFILE_WRITE_THROUGH` on cache writes.
- [ ] Universal idle VRAM reaper + `EndDevice` cleanup:
  - Split `LensFx::EnsureTargets` (`LensFx.cpp`) into separate DoF and Flare pools and release on idle.
  - Honor `kCubeSize = 1024` (instead of 12× full backbuffer textures) in `WorldWarpRender.cpp` and release on idle.
  - Add `EndDevice()` and idle release to `RenderHdrCapture.cpp` (`s_candidates[8]`), `RenderPassCompositor.cpp`, `CustomShadowAtlas.cpp`, `AutoExposure.cpp`, `SkyGpuRotation.cpp`, `SmokeAppearance.cpp`, `ContactShadowPass.cpp`, `LensRain.cpp`, `WorldDecalsRender.cpp`, `ViewportScaler.cpp`, and `EnvironmentRender.cpp` (`gHdr` when recording stops).
- [ ] Hot-path idle gating (`RenderSystemDX11Hooks.cpp`, `LightFx.cpp`, `SmokeAppearance.cpp`, `EnvironmentFoliage.cpp`, `WorldWarpRender.cpp`):
  - Add a single `std::atomic<bool>` active flag in `LightFx.cpp` (`new_LightSubmit`) to skip SEH, mutex, and 6× `WorldTransient::Number` lookups when idle.
  - Gate `SmokeAppearance::Note*` and `EnvironmentFoliage::InvalidateFoliageConstantCopy` on active flags; avoid `D3DReflect` and full DXBC bytecode retention in `WorldWarpRender.cpp` when unused.
  - Cache `GetModuleHandleW(L"nvapi64.dll")` lookup in `EnsureVendorMultiDrawHooks()`, avoid unconditional `char shaderName[1024] = {}` zeroing in `New_PSSetShader`, and avoid per-frame `g_scene`/`Relight::CaptureScene` deep copies when both Environment and Relight are OFF.
- [ ] Active-pass bottlenecks: remove per-frame staging allocation + dual GPU->CPU `Map(READ)` flushes in `SkyGpuRotation.cpp` (use persistent/ring staging or GPU-only rotation), batch/bound `WorldDecalsRender.cpp` scene copies, and move synchronous `D3DCOMPILE_OPTIMIZATION_LEVEL3` off the render thread where practical.
- [ ] Consolidation opportunities in this pass: evaluate retiring `D3D11` deferred contexts (23 instances) into a shared `D3D11StateGuard`, and consolidate duplicated `D3DReflect` / format helpers / `New_Draw*` scopes into `D3D11Utils.h`.

### Core CPU & Disk I/O
- [ ] `AfxHookSource2/ClientEntitySystem.{h,cpp}` & `Filmmaker.cpp`: build one per-frame `FrameEntityCache` at the top of `RunMainThreadFrame()` to eliminate 8–40+ redundant full entity-list scans across `main.cpp`, `DemoEndHold.cpp`, `CameraShakeStack.cpp`, `ParticleFx.cpp`, `CustomizeTarget.cpp`, `CosmeticUiQueries.cpp`, `CosmeticOverrideSystem.cpp`, and `MethodActing.cpp`.
- [ ] `AfxHookSource2/Filmmaker/Config/FilmmakerSettingsStore.cpp`: replace unconditional per-frame 90+ setting `BuildSnapshot()` serialization with a revision counter / dirty flag.
- [ ] `AfxHookSource2/Filmmaker/Demo/PlayingDemoPath.cpp`: move `__try/__except` out of the per-character loop in `SafeCopyAscii`, cache `PlayingDemoPath()` once per frame, and invalidate on raw-path change instead of 1 s `CreateFileW` TTL.
- [ ] Gate/throttle `CustomizeTarget_RunFrame`, `CamPathShake_RunFrame` (`PublishSnapshot`), `SceneRig_RunMainThreadFrame`, `SceneEditor::PublishOverlay`, `WrpEnsureCommandsRegistered`, and `EasterEggs` (remove per-frame `localtime_s` + 60s main-thread disk save).
- [ ] `AfxHookSource2/Filmmaker/Movie/CameraPath.cpp`, `CamPathShake.cpp`, `DemoProjectStore.cpp`: debounce `project.mvm` writes (500 ms idle, flushed on switch/shutdown) and only rewrite `demos/index.json` when mutated.
- [ ] `AfxHookSource2/Filmmaker/Demo/DemoLibrary.cpp` & `CrashReport.cpp`: cap `ScanWorker` helper pool to `clamp(hw/2, 1, 4)` with `BELOW_NORMAL_PRIORITY_CLASS` + `GOMEMLIMIT` + `JobObject`, and buffer `CrashReport_SessionLogLine` instead of 2 `WriteFile` calls per console line.

---

## Batch 3 — Launcher Performance, Reliability & Quality of Life

**Target branch:** `feat/audit-batch-3-launcher-perf-qol`

- [ ] **Pause GPU/CPU animations while CS2 runs**: pause `linemap.webm` and CSS `wm-sweep` `letter-spacing`/blur keyframes in `SourceMovieMakerLauncher/src/App.tsx` + `styles.css` whenever `started || advanced || !windowFocused`; replace `letter-spacing` keyframes with compositor-only `transform`/`opacity`; unmount `<video>` after intro finishes and allow Space/Esc/click skip.
- [ ] **Async main process & faster startup**: stop `saveSettings()` from invoking `defaultSettings()` -> `reg.exe` on every keystroke (`electron/main.cjs`); make `dlss-tool.cjs` (`adapterNames`, `syncBinaries`, `authenticodeValid`, `compressReShadeLog`) and `cs2-process.cjs` async; deduplicate 3x parallel FFmpeg detection; throttle download progress IPC; do `pathSizes()` in one bottom-up pass; remove `Thread.Sleep(2000)` in `host/Program.cs:227`.
- [ ] **Reliability & security**: add `app.requestSingleInstanceLock()`, gate `SMVM_RENDERER_URL` on `!app.isPackaged`, add CSP in `index.html`; fix `update-service.cjs` dropping `manual: true`/`install()` during in-flight checks and `autoUpdater.on('error')` locking `GO`; add `AbortSignal.timeout` / cancelation to `runtime-payload.cjs` & `ffmpeg-tool.cjs`, timeout to `host/InjectorClient.cs` deserialization, and atomic replace in `ffmpeg-tool.cjs`; fix `setDrawerOpen(false)` DPI height mismatch, unparented dialogs, dangling junction `EEXIST` in `dlss-tool.cjs`, and `host/Program.cs` `Process` handle leak + null-check order.
- [ ] **QoL & Cleanup**: add **When CS2 Starts** behavior (`Minimize` / `Tray` / `Close` / `Stay open`), one-click `LOCATIONS` cache & `ReShade.log` cleanup, quick launch options presets, recent demos + `.dem` drag-and-drop, proactive Steam status badge, copyable error/crash card, `MB` progress + cancel button for downloads, `RE-DETECT` CS2 / `RESET TO AUTO` FFmpeg buttons; remove ~2.3 MB duplicate intro video & 35 unused `public/` assets, extract shared `electron/fs-net-utils.cjs`, split `src/App.tsx` & `src/styles.css`, and pin `"latest"` deps in `package.json`.

---

## Batch 4 — In-Game Panorama UI Speed, QoL & CI Hygiene

**Target branch:** `perf/audit-batch-4-panorama-ui-qol`

- [ ] **Panorama GPU & V8 speed**: gate/disable `WorkspaceShellJs.h`'s 60 FPS Gaussian blur on 5 labels and 4x `linemap.webm` mask panels; keep `ConfigHud` (1.19 MB) and `CameraEditor` (1.09 MB) roots hidden instead of tearing down and recompiling on every toggle; convert periodic/input `RunScript` calls (`Localization.cpp`, `VideoSettingsResolution.cpp`, HUD wheel/char dispatches) to `RunScriptCached` + attributes; split live `mx`/`my` from monolithic state JSON (`ConfigHud`, `CameraEditor`, `GraphEditor`, `CameraTimeline`) and add missing `raw !== lastParsedRaw` guards; stop background `$.Schedule` timers while hidden; pool `GraphEditorJs.h` SVG/DOM nodes; deduplicate 378 KB `CameraEditorCosmeticsCatalog.inc`, lazy-load `EasterEggOverlayHud`, and gzip-compress/externalize 7.98 MiB `LocalizationTables.rc`.
- [ ] **In-game QoL**: wire the 5 missing `PushCurveUndo()` calls in `CameraPath.cpp`; add bounded undo/redo to `Takes` / `SceneEditor` + `#MvmCamToast` undo/redo notifications; add `Ctrl+K` quick-jump across all workspace tabs; add `Shift`/`Ctrl` precision arrow-key stepping in `MovieMode.cpp`; make `G`/`Tab`/workspace toggles rebindable in `KeybindsMath.h` + swap-on-conflict; add `?`/`F1` hotkey cheatsheet; add Essentials/Advanced filter & consistent card reset/modified indicators in `ConfigHud`; surface Onboarding from UI and use dynamic `Keybinds` in `Onboarding.cpp`.
- [ ] **UI & C++ consolidation + CI hygiene**: extract shared `PanoramaPrimitivesJs.h` (`$.MvmUi`) and C++ `HudPanelBase`; fix newline command injection in `DemoOrganizerJs.h` and unquoted skybox filenames in `ConfigHudWeatherSkyTabJs.h`; rename lowercase `automation/tests/relight-native-cluster-tests.cpp` -> `Automation/tests/...`; wire missing `.mjs` and `.py` tests into `.github/workflows/ci.yml`.

---

## Deferred: Unified Low-Spec / Performance Mode Toggle

*(Deferred by user request — implement in a dedicated PR after Batch 4 or when prioritized.)*

1. **Single Toggle Surface**: `mirv_filmmaker perfmode on|off` + Settings / Config toggle.
2. **Rendering & VRAM Overrides**:
   - Force `Relight::Scene::lite = true` (already implemented in `AfxHookSource2/Filmmaker/Relight/RelightProbeMath.h` — swaps 128³ voxel + 64³×6 probe + 512³ `staticVoxels` + `ScreenGi` for the compact 16×16 cube + `LiteGrid`).
   - Parameterize `EnvironmentShadowMapsDetail.h` (`SunResolution`: 2048 vs 4096 = 75% shadow VRAM reduction; `LocalResolution`: 512 vs 1024; `g_actorShadowFacesPerFrame`: 16 vs 48).
   - Expose `EnvironmentRender.cpp` live volumetric divider (`1/4`, `1/6`, `1/8`) and export divider (`1/2`, `1/1`), Half-Res DoF in `LensFx.cpp`, `512`/`1024` cubemap + reduced grid in `WorldWarpRender.cpp`, and optional Half-Res `ContactShadowPass.cpp`.
   - Keep MapData pre-extraction and shader derivation strictly on-demand, and disable `WorkspaceShellJs.h` 60 FPS blur + `linemap.webm` letterbox videos.
3. **Live GPU Pass & VRAM Budget HUD**:
   - Surface already-collected `Filmmaker::GpuProfile` (`Filmmaker/Debug/GpuPassProfiler.h`) and DXGI `RenderGpuMemory_Sample` (`RenderTargetProbe.cpp`) in a compact HUD card showing top passes (ms), hook VRAM vs adapter budget, and a one-click action to release idle VRAM or switch to Performance Mode.
