# Deadlock Audit Backlog (Deferred)

This document records verified bugs, performance bottlenecks, cross-game consolidations, and launcher/QoL parity items discovered in `AfxHookSource2/Deadlock/` and `AfxHookSource2/Filmmaker/Deadlock/`. Per maintainer direction, Deadlock work is deferred until CS2/Launcher batches are complete.

---

## 1. Critical & High-Priority Bugs

1. **`DLL_PROCESS_DETACH` loader-lock deadlock & missing device cleanup**
   - `AfxHookSource2/Deadlock/DeadlockMain.cpp:584-588` & `DeadlockNetcon.cpp:327-346`: `DllMain(DLL_PROCESS_DETACH)` ignores `lpvReserved` and calls `Deadlock::Netcon_Stop()`, joining `g_acceptThread` / `g_writeThread`, calling `UnregisterLogListener()` into `tier0.dll`, and calling `WSACleanup()` under the OS loader lock. Conversely, `ViewportBlit_EndDevice()`, `OverlayDrawer_EndDevice()`, and `UiCommandBridge_Stop()` are never called on detach.
2. **Wrong `Cvar_s` struct layout in `ReadHostFramerate()`**
   - `AfxHookSource2/Filmmaker/Deadlock/DeadlockRender.cpp:55-69`: reads `cv->m_eVarType` and `cv->m_Value` using CS2's `SOURCESDK::CS2::Cvar_s` layout, contradicting `DeadlockMirvCommands.cpp:533-534` (Deadlock has extra callback fields before the value). Can break `Render_Start()` or corrupt `host_framerate` in `RestoreClock()`.
3. **Unescaped & 256-byte truncated JSON in `Entities_AppendHeroesJson`**
   - `AfxHookSource2/Deadlock/DeadlockEntities.cpp:530-541`: uses `char buf[256]` + `std::snprintf` without JSON escaping (`JsonBuilder::Escape`). Steam names containing `"`, `\`, or control chars break Panorama `RunScript` every frame, and long names truncate mid-object.
4. **3D overlays never render when the workspace is closed**
   - `AfxHookSource2/Deadlock/DeadlockPreUiHook.cpp:198,219` gates `QueuePreUiCallback(param_1)` solely on `ViewportBlit_Active()`, which is `false` when the workspace is closed — preventing path, markers, digits, and follow boxes from rendering when pressing `K` with the editor closed (`DeadlockWorkspaces.cpp:600-662`).
5. **`Ui_ConsoleVisible()` hardcoded `false`**
   - `AfxHookSource2/Deadlock/DeadlockUi.cpp:94-99`: typing in Deadlock's F7 console still drives WASD freecam (`DeadlockInput.cpp:52-55`) and global hotkeys (`DeadlockHotkeys.cpp:106`).
6. **Cross-thread data races**:
   - `DeadlockViewportBlit.cpp:42-49,169-294,330-341`: non-atomic `g_active`, `g_suppressed`, `g_rect[4]`, `g_blits`, `g_skips`, `g_lastVp` shared between main and render threads.
   - `DeadlockCameraRuntime.cpp:62-64,190-211` + `DeadlockOverlayDrawer.cpp:462`: `lastOrigin`/`lastAngles`/`lastFov` written on main thread, read unsynchronized on render thread.
   - `DeadlockOverlayDrawer.cpp:86,94-98,419-443,604`: unsynchronized `g_enabled` / `Ready()` / `TexReady()`, plus 4 separate mutex locks allowing torn overlay snapshots.
   - `DeadlockPreUiHook.cpp:22-24,102-130,328-339`: `SeenNames()` (`std::set<std::string>`) mutated on render thread and read/cleared on main thread without a mutex; `IsHudPass` (`157-162`) assumes first `va_arg` is `const char*` whenever `"%s"` appears anywhere in `fmt`.
   - `DeadlockUiCommandBridge.cpp:38,59,135-141`: non-atomic `g_draining` shared across main and tier0 logging threads.
   - `DeadlockCursorClip.cpp:18-25,35-56`: unsynchronized cursor clip state.
   - `DeadlockMain.cpp:326-327`: unchecked dereference of `GetProcAddress(module, "g_pMemAlloc")`.
7. **D3D11 MSAA capture API violation**
   - `AfxHookSource2/Filmmaker/Deadlock/DeadlockRender.cpp:191-205`: `CaptureBackBuffer` creates `staged` as `D3D11_USAGE_STAGING` and passes it to `ResolveSubresource` when `SampleDesc.Count > 1` (D3D11 requires `D3D11_USAGE_DEFAULT` as resolve destination).
8. **Input, hotkey, blackout, netcon & UI reload bugs**:
   - `DeadlockMouseWheel.cpp:29-51`: installs a global `WH_MOUSE_LL` hook on a detached thread with no `WM_QUIT` teardown path.
   - `DeadlockInput.cpp:116-124,204-207,26,169`: `CursorClip_RunFrame()` runs 1 frame behind `cursorShowing`, double `Supply_SetCursorPos(0, 0)` every frame, unused `wasMirvSuspended`.
   - `DeadlockNetcon.cpp:117,181-209,235-246`: drops client on `WSAEWOULDBLOCK`, 0–100 ms `select()` latency on reads, `partials` static map not cleared on stop.
   - `DeadlockHotkeys.cpp:31-33,77`: `VK_SPACE` bypasses `mirv_filmmaker play`.
   - `DeadlockHudBlackout.cpp:19-21`: lacks the 30-frame seek/Blitz debounce in `DeadlockFrame.cpp:66-89`.
   - `DeadlockCommands.cpp:82,337-365,1072-1074,1180-1183`: unimplemented `ui cursor toggle` help row, stale `0x438`/`0x43c` fallback offsets, and lossy `(wchar_t)(unsigned char)` non-ASCII widening.
   - `DeadlockWorkspaces.cpp:683-686`: `ui rebuild` omits 6 JS globals (`$.MvmDraw`, `$.MvmControls`, `$.MvmSpeedDial`, `$.MvmInspectorGrip`, `$.MvmRegularTimeline`, `$.MvmNativeHud`).
   - `DeadlockPathShake.cpp:153-189`: missing CS2's `CamPathShakeRampMath.h` update, so unshaken camera keys still cut off active shakes.

---

## 2. Performance Bottlenecks

1. **Startup syscall storm**: `DeadlockSchema.cpp:106-117` calls `VirtualQuery()` per character on every class/field name (~1M+ syscalls), and `DeadlockFrame.cpp:55-64` retries `Schema_Init()` pattern scans up to 600 frames.
2. **Closed-workspace per-frame work**: `DeadlockWorkspaces.cpp:626-752`, `DeadlockGraph.cpp:926-1070` (672 Bezier samples + 1,344 `snprintf`s/frame), `DeadlockRender.cpp:579` (`SHGetKnownFolderPath(FOLDERID_Videos)` every frame), and `DeadlockPathShake.cpp:207-216` run unconditionally even when the workspace is closed.
3. **290+ Win32 `GetAsyncKeyState` syscalls/frame**: `DeadlockInput.cpp:137-157` loops `0..255` every frame plus calls `GetCommandLineW()` every frame.
4. **Render-thread heap churn & redundant projection**: `DeadlockOverlayDrawer.cpp:445-589` allocates 6+ unreserved `std::vector`s/frame and re-projects gizmos 5× and hero boxes 3× for glow passes.
5. **Render capture stalls**: `DeadlockRender.cpp:172-223` allocates/frees a staging texture per frame, performs scalar CPU RGBA->BGRA swizzle + 1,080 `WriteFile` calls on the render thread, and adds FFmpeg `lanczos` scale/pad even at native resolution.
6. **Netcon & boot log overhead**: `DeadlockNetcon.cpp:215-229` wakes `WriteThread` at 200 Hz (`Sleep(5)`) with no `condition_variable`, and `DeadlockBoot.h:33-55` opens/writes/closes `%TEMP%\afxhookdeadlock_boot.log` on every netcon command.

---

## 3. Cross-Game Consolidation & Build Cleanup

- **Math duplication**: make `FollowCameraMath.h` header-only to eliminate the verbatim copy in `DeadlockFollowSolveMath.h:48-248`, and deduplicate `AngleVectors` / gizmo geometry (`DeadlockOverlayMath.h`) and `Shared/FovScaling.cpp` (`DeadlockCameraRuntime.cpp:99-107`).
- **Module consolidation**: extract shared engine-callback interfaces for `PlaybackLoop`, `CamPathShake`, `WorkspacePreview`, `HudBlackout`, `ViewportBlit` (using Deadlock's cleaner `SV_VertexID` fullscreen triangle), and replace ad-hoc JSON/UTF-8 helpers with `JsonBuilder` and `TextEncoding`.
- **Dead code & CMake**: remove `DeadlockEntitiesDisabled.cpp`, dead `wincodec.h` in `DeadlockRender.cpp`, and empty stubs in `DeadlockCameraRuntime.cpp:440-445`; introduce a shared static/object library instead of re-listing 47 shared `.cpp` files in `AfxHookSource2/Deadlock/CMakeLists.txt:171-232`; stop `x64-deadlock-*` presets from configuring Rust/OpenEXR/ShaderBuilder/Go/`dxc.exe`; narrow `/EHa` to SEH TUs; standardize PDB install path (`pdb/x64` vs `bin/x64`); and split `DeadlockWorkspaces.cpp`'s 15 `*Js.h` includes.

---

## 4. Launcher & Feature Parity

- Wire Deadlock (`--steam-appid 1422450`, already supported in `SourceMovieMakerLauncher/host/Program.cs`) into the Electron launcher + add `build-deadlock.bat` / staged release layout (`launch-deadlock.ps1` currently hardcodes `build/x64-deadlock-release/...` and borrows CS2's `injector.exe`).
- Reuse `Filmmaker/Render/RenderEncoderDetect.cpp` so Deadlock's `auto` render encoder uses GPU encoders instead of hardcoding CPU (`DeadlockRenderEncoder.cpp:82-83`).
- Port customizable keybinds, `DemoKeyframes` backward-seek helper, live bone transforms for Follow Attach, and clean up stale sections in [README.md](README.md) and [console-commands.md](console-commands.md).
