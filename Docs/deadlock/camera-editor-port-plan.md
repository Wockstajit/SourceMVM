# Deadlock: camera editor port plan

The plan for bringing the CS2 filmmaker's **camera editor** to Deadlock as a native part of
Deadlock's UI: the workspace shell with its world scaling and tab strip, the camera editor
(placing cameras, the path, the timeline, the graph, follow cameras), the render workspace with its
camera configuration, and an empty CONFIG shell. Everything Deadlock-specific lives in the two
Deadlock trees and nothing else in the repository changes.

This is a plan, not a status page. Anything measured in game is stated as fact, with its method. Everything else is work.

Related: [README.md](README.md) (build, launch, console surface),
[signatures.md](signatures.md) (addresses), [ui-style.md](ui-style.md) (design tokens).

---

The engine command adapter uses slot **51**, and the bare
map-name getter uses **65** (`GetLevelNameShort`). The replay filename getter is **43**, through
player slot 1 and `CDemoFile` slot 3; engine slot 44 returns the recorder name. These were
re-derived and verified live.

## 1. Ground truth measured in game

Every item below was measured against the installed build (`ClientVersion=6687`) with the game
running, not inferred. They are prerequisites the rest of the plan stands on.

| Item | Measured value | Method |
| --- | --- | --- |
| `ExecuteClientCmd` engine vtable slot | **51** | Re-derived and verified live on build 6774 |
| `GetLevelNameShort` slot (bare map name) | 65 | Re-derived and verified live on build 6774 |
| `IsPlayingDemo` slot | 41 | `demo=playing` during a self-recorded demo, `demo=no` otherwise |
| View struct | embedded in `CViewRender` at **`this+0x10`** | Live dump of the accepted main-view object, cross-checked against `getpos` to the last decimal |
| origin / angles / fov | `this+0x4b0` / `this+0x4c8` / `this+0x4a8` | Same dump: `getpos` gave `11154.06 -7466.16 -245.72` and `11.81 280.19 0.00`, the struct held exactly those |
| viewport | width `this+0x440`, height `this+0x448` (1440x810 at a 1920x1080 window) | Same dump; `+0x444`/`+0x44c` hold the unscaled 1920/1080 |
| aspect, znear, zfar | `this+0x4e8` = 1.7778, `+0x4d4` = 7, `+0x4d8` = 50000 | Same dump |
| Tick rate | **64 ticks/s** | `record` produced 1993 ticks in 31.14 s; the assumed 1/64 is correct |
| Main-view gate chain | `0x2349178 -> 0x16b6700 -> 0x16b6744` | Offline verifier; zero vtable/caller rejections in game |
| Camera write | works: 5,536 writes, 0 rejections, game stable | With the end-of-frame mouse handler off |
| Downloaded replay (`dl_midtown`, `valve_demo_2`) | exits the game at spawn-group load | Reproduced with the hook fully inert and with no injection at all; identical console tail ending in `ResetBreakpadAppId` |
| Self-recorded replay (`record` / `playdemo`) | plays, pauses, survives free camera | All later camera tests ran on it |

Two details these measurements set in the code:

- The view is **embedded at `this+0x10`** in `CViewRender`. The offsets in `DeadlockViewSetup.cpp`
  are relative to `CViewRender` itself.
- `GetDemoFile` is not an engine-interface slot at all. The demo player is an **engine2 global**
  (`demo_player_pointer`, RVA `0x61b618` on this build) whose object's vtable must equal
  `demo_player_vtable` (`0x535730`) before it is trusted. Its slots: `IsPlaying` 11,
  `IsPaused` 12, `IsSeeking` 14, `GetTick` 3, `GetStartTick` 2, `GetPlaybackScale` 17,
  `GetFile` 1. The demo player is read through this global.

---

## 2. Scope

**In.** The workspace shell (chrome, world scaling behind the right column, the Timeline/Graph
tab strip), the camera editor and its cards (path inspector, follow inspector), camera placement
and the path, the timeline docked beside Deadlock's native replay bar, the graph editor, follow
cameras attached to heroes and hero attachment points, the render workspace and its camera
configuration, the CONFIG shell with no tabs populated, and the complete debugging and automation
surface that the CS2 build has.

**Out.** Everything under `Filmmaker/Environment/`, `Filmmaker/World/`, `Filmmaker/Cosmetics/`,
`Filmmaker/PlayMenu/`, `Filmmaker/Workshop/`, weapon/viewmodel/recoil/skin/FX, ragdolls, method
acting, particle packs, the CS2 demo browser and the CS2 `.dem` protobuf readers. The CONFIG
tabs are empty by design: Deadlock is a different game and each tab is filled later against it.

**Look.** The panels must read as Deadlock's own UI: its warm off-white text, gold accent, green
interactive accent, bevelled buttons, blurred grounds and its fonts, from
[ui-style.md](ui-style.md). Layout and behaviour stay one-to-one with the CS2 editor.

---

## 3. Where things live

Two trees hold the port, plus the root CMake option, the presets and the docs index.

| Tree | Holds |
| --- | --- |
| `AfxHookSource2/Deadlock/` | Engine layer: boot, modules, addresses, engine facade, view hook, present hook, Panorama primitives, input, entities, cvars, netcon, commands, debug |
| `AfxHookSource2/Filmmaker/Deadlock/` | Filmmaker layer: frame pump, workspaces, every `*Js.h` panel, shot, graph, render, follow |
| `Automation/` (`*deadlock*` files only) | Launch, netcon, readiness, verifier, tests |
| `Docs/deadlock/` | Every Deadlock document |

Rules that apply to every file added by this plan:

- A new capability is a new file with its own header; nothing is bolted onto an existing unit
  because it is nearby. `DeadlockDebug.cpp` (mvm_debug and the exit trace) is the model.
- Shared CS2 units are shared by naming them again in the Deadlock `target_sources`, never by
  editing them. A unit that needs a change to be shareable gets a Deadlock re-author instead.
- Every engine number lives in `DeadlockAddresses.cpp` or the slot table in
  `DeadlockEngine.cpp`, is runtime-overridable, and is checked by the offline verifier.
- Any arithmetic goes in a header-only `*Math.h` with a CTest, as the CS2 side does.
- The reference tool this port learned from is not named in Deadlock sources or docs.

---

## 4. Workstreams

### A. Engine foundation

The camera moves today; these make it trustworthy and give the rest of the editor a clock.

**A1. Demo player and clock.** New `DeadlockDemo.cpp/.h`:

- Resolve the engine2 globals `demo_player_pointer` and `demo_player_vtable` by pattern, and
  `network_client_service_pointer` (`0x691388`) with `GetTick` slot 57. Add all three to the
  verifier with the same OK/MULTI/BROKEN classification.
- `Demo_IsPlaying()`, `Demo_IsPaused()`, `Demo_IsSeeking()`, `Demo_Tick()`, `Demo_StartTick()`,
  `Demo_PlaybackScale()`, `Demo_FileName()` (engine slot 42), all guarded, all returning neutral
  when the vtable check fails.
- `CGlobalVars` from the client (`pointer_rva 0x2f09170` on this build): `curtime +0x30`,
  `frametime +0x34`, `interval_per_tick +0x54`. Use `curtime` for fractional time at the render
  hook; do not reconstruct time from the context tick, it rounds. The measured 64 ticks/s is
  the check.
- `DeadlockEngine.h` keeps its surface; `DemoTick()`, `DemoTime()`, `IntervalPerTick()`,
  `IsDemoPaused()` route here. `mirv_deadlock health` shows the resolved globals and whether the
  vtable check passed.
- Demo-clock traps: time steps backwards a few ms routinely (50 ms tolerance is not
  a seek); `IsPlayingDemo` and `IsDemoPaused` blip false mid-seek (30-frame debounce);
  `IsSeeking` should gate the shot outright.

**A2. View write rules.** In `DeadlockViewSetup.cpp`:

- Field of view is written **together with aspect** when the requested FOV differs from the
  engine's. The engine has already scaled the base FOV by the current aspect when the hook runs,
  so a bare FOV write fights it. Formula: `fov' = 2·atan(tan(fov_engine/2) · aspect_wanted /
  aspect_engine)`, written to `+0x4a8` with `aspect_wanted` to `+0x4e8`. Put it in
  `DeadlockViewMath.h` with a CTest.
- Reject a view with bit `0x02` at `+0x565` (alternate projection); the current default reads it
  and the report counts it.
- The end-of-frame mouse handler stays off until A3 lands. Re-enable it there, not before.

**A3. Input layer.** New `DeadlockInput.cpp/.h`. This is the missing half of the free camera:
`WindowInputHooks.cpp` is not in the Deadlock target, and CS2's model (cursor deltas via
`GetCursorPos`/`SetCursorPos`) is the wrong model here because Deadlock reads the mouse through
**raw input**.

- Subclass the game window's `WndProc` (`SetWindowLongPtrW`, restored on detach).
- While the camera owns input: on `WM_INPUT` read the packet through the original
  `GetRawInputData`, feed the deltas to `MirvInput`, and complete the message with
  `DefWindowProc` so the game never sees it. Detour `GetRawInputData` so the game's own reads
  return nothing while owned; detour `SetCursorPos` and `ClipCursor` so the game cannot recentre
  or clip the cursor from under the editor.
- Keys: `WM_KEYDOWN/UP` and `WM_SYSKEY*` routed to `MirvInput::Supply_KeyEvent`, swallowed while
  owned; `Alt+F4` and `Alt+Tab` never swallowed; a held-key reset on `WM_KILLFOCUS`.
- Focus: `WM_SETFOCUS`/`WM_KILLFOCUS`/`WM_ACTIVATEAPP` to `MirvInput::Supply_Focus`.
- Ownership is the existing `GetSuspendMirvInput()` predicate in `DeadlockCameraRuntime.cpp`:
  console visible, mouse free, or any panel wanting the cursor releases input to the game and
  Panorama. Panels need `UiToolkitAPI.AddDenyInputFlagsToGame` with `ShareMouse` on top, or
  sliders work while buttons stay dead.
- Milestone: WASD and mouse-look in a **paused self-recorded replay**, surviving seeks.

**A4. Cvars.** New `DeadlockCvars.cpp/.h` over tier0's `VEngineCvar007` (find slot 11, data slot
43, flags at `+0x30`, scalar at `+0x58`): read a cvar, set a cvar through the engine's own setter,
and clear the development/hidden/defensive flags on a named list when `-dev` alone does not
expose what the tool needs. No external unlocker is needed. The list the
editor needs: `demo_pause`, `demo_resume`, `demo_gototick`, `demo_timescale`, `demo_info`,
`demo_goto`, `demo_step_tick`, `spec_goto`, `spec_pos`, `cl_citadel_forceangles`,
`r_aspectratio`, and the `r_citadel_depthoffield_*` family for a later lens card.

**A5. Replay compatibility.** The downloaded replay exits the game with no hook involved. The
engine carries a recorded-game/current-game mismatch check with `demo_allow_game_mismatch`
defaulting to false. First test, with `-dev` and A4: set it to 1, load the same file, read the
console tail. If it plays, `mirv_deadlock replay doctor` becomes a command that runs
`demo_info`, checks the build, sets the flag and reports; if it does not, the doctor reports the
mismatch plainly instead of letting the game vanish. Until then every in-game gate uses a
self-recorded replay: `record mvmtest` in the hideout, `stop`, `playdemo mvmtest`.

### B. Debugging and automation parity

Everything used on the CS2 side to launch, drive, observe, bisect and test needs a Deadlock
equivalent. Most exists; the table is the checklist.

| CS2 | Deadlock equivalent | State |
| --- | --- | --- |
| `mvm_debug start/stop/status` | `mvm_debug` in `DeadlockDebug.cpp`, real start/stop, console capture, VEH crash lines | Done; unverified beyond one log |
| Crash dumps / event log | **Exit trace**: detours on `ExitProcess`/`TerminateProcess` writing caller module+offset and a 16-frame stack to the boot trace and the debug log, then closing the log | Done; not yet exercised by a real exit |
| Boot trace | `%TEMP%\afxhookdeadlock_boot.log`, raw kernel32, always on | Done |
| `mirv_filmmaker health` | `mirv_deadlock health` | Done; add A1 globals, input state, cvar flag state |
| Per-hook bisect flags | `-mvmNoViewHook -mvmNoViewDetour -mvmNoPresentHook -mvmNoUi -mvmInert`, plus runtime `view enable/write/frameend` | Done; add `-mvmNoInput` with A3 |
| `MvmDebugLog_Linef` probes | `deadlock.view`, `deadlock.view.claimed`, `deadlock.shot`, `deadlock.ui`, `deadlock.panorama`, `deadlock.engineprobe` | Partly; add `deadlock.input`, `deadlock.demo`, `deadlock.follow`, `deadlock.entities` as those land |
| Enumeration | `mirv_deadlock view dump`, `engine slots`, `engine probe` | Done; add `mirv_deadlock enumerate panels` (HUD ids under the root), `enumerate entities` (hero pawns with class, index, team, origin), `enumerate cvars <prefix>` (name, flags, value) |
| Launch + readiness | `deadlock.bat`, `launch-deadlock.ps1` (`-dev -insecure -console`), `wait-for-deadlock-ready.ps1`, `session.json` | Done |
| Netcon | In-DLL channel on 29020, `deadlock-netcon.ps1`, `deadlock-fm-eval.ps1` | Done; fix the write thread's unbounded `WSAEWOULDBLOCK` retry (a gone client can stall every other client's output) |
| Offline signature verifier | `verify-deadlock-signatures.py` | Done; extend with A1 globals, tier0 slots, the entity-system global, and a `--profile` output that writes what it found as JSON for `health` to compare against |
| `mvm_test` (bot practice) | `mirv_deadlock selftest`: record a short demo in the hideout, play it, pause, claim the camera, verify writes>0 and zero rejections, report | New |
| CTest math suite | `deadlock-demo-clock-math-tests.cpp`; add `DeadlockViewMath` (A2), `DeadlockFollowMath` (E) | Extend |
| Panorama build check | `deadlock-panorama-build-tests.mjs` builds and renders every workspace against the stub | Done; every new `*Js.h` registers here |
| Doc integrity | `documentation-integrity-tests.mjs` | Applies unchanged |
| Screenshot capture | `Automation/verify/verify-deadlock-screenshot.ps1`: capture the game window by pid, hide the HUD first, re-send after each seek | New |

Operating rules:

- Every in-game test runs **in a paused replay**. Nothing is measured in a live match.
- Instrument first: a probe that does not exist yet is the first task, then `mvm_debug start`,
  reproduce, read the log.
- A clean exit is a result, not a crash. Read the boot trace and the exit trace before anything
  else.
- Close the game with `quit` over netcon, never a force kill.

### C. Workspace shell and camera editor UI

The CS2 editor is about 31k lines across editor, timeline, graph, config, follow and smooth view.
The Deadlock trees already hold 7.5k lines including a workspace shell, camera body, graph body,
render body and a style token file. The port is file-by-file, and the style is applied at the
token layer so layout code stays identical.

**C1. Style.** `DeadlockStyleJs.h` already carries a token object. Bring it to the full palette
and chrome from [ui-style.md](ui-style.md): `#FFEFD7` text, `#10130D` / `#2e2c27` / `#1a1a1a`
grounds, `#FFED79` accent, `#5FE69E` interactive, `#C6C6C6` muted, `#ee5945` warning; bevelled
buttons (`#373d45` to `#4d5860`, 8 px radius, 36 px min height, uppercase letter-spaced labels
with a hard 2 px shadow); slider thumbs on the green radial; `gaussian(2,2,2)` blur behind
in-game chrome; `Retail Demo` / `Reaver` font stacks; the transport icons from
`s2r://panorama/images/transport_controls/`. Every panel sets every visual property explicitly
from this object, never inheriting game styles, exactly as the CS2 panels do.

**C2. Shell.** `DeadlockWorkspaceJs.h` / `DeadlockWorkspaces.cpp` become the port of
`WorkspaceShellJs.h` + `CameraEditorHud.cpp`: the right column, the top tab strip (CAMERA /
GRAPH / CONFIG / RENDER), the F8 clear-everything, and the HUD hide list read from
`layout/hud.xml` into `DeadlockHudIds.h`.

**C3. World scaling.** The editor scales the rendered world into the region left of the right
column. On CS2 this is `WorkspacePreview` (the single owner of the scaled rect) over
`ViewportScaler` (a render-thread blit driven from the present hook). Port as
`DeadlockWorkspacePreview.cpp` and `DeadlockViewportScaler.cpp` on the DX11 present hook that
already exists; the rect is published by the panel's `render()` each frame and forwarded on the
main thread. Recorded failure to watch for: a full-screen world behind the panel means
`render()` threw.

**C4. Camera editor cards.** Port `CameraEditorJs.h` (1,425), `CameraEditorCardsJs.h` (636),
`CameraEditorWidgetsJs.h` (1,036), `CameraEditorPathInspectorJs.h` (315) and
`CameraEditorFollowInspectorJs.h` (514) into `Deadlock*Js.h` fragments, restyled through C1.
`CameraEditorCustomizeJs.h` and its preview are CS2 cosmetics and do not come. Three Panorama
facts to respect from the first line: all fragments share one JS scope, so no plain `var`
collisions; `style = ''` throws, clear with `rgba(0,0,0,0)`; pinned chrome beside a clipped
scrolling column needs its own `zIndex`.

**C5. Timeline.** `CameraTimelineJs.h` (1,627) and `CameraTimelineRangeJs.h` port; the native
demo-bar hijack (`CameraTimelineJs.h:248-450`), `DemoBarButtons*`, `SpeedScrubberJs.h` and the
shake menu do not. The Deadlock timeline docks beside `citadel_hud_replay.xml` and delegates
speed, skip and marker stepping to `CitadelReplayIncrementSpeedStep`, `CitadelReplaySkipByTime`
and `CitadelReplayNextMarker`. `TimelinePlayhead` stays the one shared cursor with the graph.

**C6. Graph.** Exists (`DeadlockGraph*`). Restyle through C1 and register in the build test.

**C7. Render workspace.** `CameraEditorRenderJs.h` (1,088) + `CameraEditorRenderLayoutJs.h`
port over the existing `DeadlockRender.cpp`. The camera configuration block (resolution, fps,
range, output folder) is the "render camera config" the user asked for.

**C8. CONFIG shell.** Port `ConfigHudJs.h` chrome, tab strip and dispatcher plus
`ConfigHudWidgetsJs.h`; no tab bodies. `Filmmaker/Config/` stores are shared unchanged.

### D. Camera placement and the path

`DeadlockShot.cpp` already holds markers, playback and the playhead. This workstream completes
the editing loop:

- Capture the current free-camera pose as a marker (`marker add`, bound to a key through A3).
- Keyframe gizmos in world through `CampathDrawer.cpp` (shared), drawn from the present hook.
- Path playback locked to `Demo_Tick()` from A1, with the seek and pause traps applied.
- Space/Play gating: timeline open, Space plays the shot; closed, Space is demo pause/resume.
- Save/load through `Filmmaker/Config/` stores.

### E. Follow cameras

The highest-risk item, because it is the only one that needs the entity system.

**E1. Entity facade.** New `DeadlockEntities.cpp/.h`, the only place that touches entities:

- Resolve the client entity system global and the get-entity-by-index function (pattern; add to
  the verifier), and class schema through `schemasystem.dll` with the same technique CS2's
  `SchemaSystem.cpp` uses (find the client type scope, declared class, field offset by name).
- Fields: `C_BaseEntity.m_pGameSceneNode`, `CGameSceneNode.m_vecAbsOrigin`,
  `CGameSceneNode.m_angAbsRotation`, the hero pawn class (`C_CitadelPlayerPawn`) with health,
  team and hero id, and its skeleton instance for attachments.
- Surface: enumerate hero pawns; origin, eye angles, alive, team, display name; attachment
  lookup by name; world transform of an attachment. Every write of a resolved offset and target
  pointer goes to `deadlock.entities` in the debug log.

**E2. Providers and math.** `FollowCameraMath.h` is engine-free and is shared with its CTest.
`FollowCamera.cpp` (1,056) is re-authored as `DeadlockFollowCamera.cpp` over E1 with only the
target types that exist here: hero, hero head, hero chest, hero weapon hand. No bombs, no
dropped weapons, no grenades.

**E3. Inspector.** The follow inspector panel from C4 drives E2; the cards list heroes from E1
by name and team, with attachment points as a dropdown.

### F. Milestones and gates

Each gate is one netcon-checkable statement plus a screenshot, always in a paused self-recorded
replay. No gate is passed on a green build.

| # | Gate | Proof |
| --- | --- | --- |
| 1 | Demo clock | `health` shows the demo player resolved with vtable OK; `Demo_Tick()` climbs while playing and holds while paused; `IntervalPerTick` = 0.015625 |
| 2 | Free camera | WASD and mouse-look move the view; `view` shows writes climbing, 0 rejections; survives `demo_gototick` |
| 3 | Shell | Workspace opens over the paused replay; world scaled beside the column; tab strip switches; screenshot reads as Deadlock |
| 4 | Placement | Three markers, path plays between them, gizmos visible, timeline scrubs, graph edits a curve |
| 5 | Follow | Follow a hero and a hero head through a seek |
| 6 | Render | A job writes a numbered PNG sequence at 32 fps with zero drops and zero residue |
| 7 | CONFIG | Opens empty over the scaled world |
| 8 | Automation | `selftest` passes from a cold launch with no human input; verifier exit 0; all tests green |

Order: A1 -> A3 -> A2/A4 -> gate 1, 2 -> C1, C2, C3 -> gate 3 -> C4, C5, C6, D -> gate 4 ->
E -> gate 5 -> C7 -> gate 6 -> C8 -> gate 7 -> B completion -> gate 8. A5 runs whenever a
downloaded replay is at hand and never blocks the line.

---

## 5. Verification

Offline, per commit:

```batch
py -3 Automation/verify/verify-deadlock-signatures.py
```

```batch
ctest --test-dir build/x64-deadlock-release -C Release --output-on-failure
```

```batch
node --test Automation/tests/deadlock-panorama-build-tests.mjs
```

```batch
node --test Automation/tests/documentation-integrity-tests.mjs
```

In game, per gate: launch as a background task and read the exit code; `mirv_deadlock health`
first; `record` and `playdemo` a replay; `demo_pause`; `mvm_debug start`; then the gate. Read the
boot trace and the debug log yourself; a console reply shows one instant.

---

## 6. Risks

| Risk | Consequence | Mitigation |
| --- | --- | --- |
| Entity system and schema resist resolution | No follow cameras | E1 is isolated; gates 1-4 and 6-8 do not depend on it |
| Raw-input interception fights the game | No mouse-look, or the game keeps looking | A3 detours are per-call bisectable; `-mvmNoInput`; release-on-suspend is the default state |
| A game update moves everything | Same treadmill as CS2 | One address file, offline verifier, `signatures.md` records derivations |
| Downloaded replays never play | Only self-recorded replays usable for testing | A5; the tool's own workflow does not depend on it |
| Shared CS2 units drift | Deadlock breaks from an unrelated commit | Only engine-free files are shared; CTest covers them |
| Clean exits with no evidence | Hours lost to guessing | Exit trace, boot trace, bisect flags, always-in-a-demo rule |
