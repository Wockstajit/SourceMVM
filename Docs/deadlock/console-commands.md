# Deadlock console commands

Use the game's F7 console. SOURCE:MVM registers its commands there, so a separate tilde console is not needed. These commands need the Deadlock hook, `AfxHookDeadlock.dll`, launched in offline/insecure mode. The CS2 DLL does not provide Deadlock commands.

## Shared names

`mirv_filmmaker` is the main command in both builds. Deadlock keeps `mirv_deadlock` as an alias, so existing UI code, binds and automation keep working. Both names call the same Deadlock dispatcher and share settings and camera state.

The root name is shared, but subcommands differ by game. For example, Deadlock's `hud scaled|hidden|toggle` controls its replay HUD. Check Deadlock's help before pasting CS2 `hud`, `play` or `ui` arguments. The [Deadlock command table](README.md#the-console-surface) lists its workspace, render and camera editor operations.

| Purpose | Command | Scope |
| --- | --- | --- |
| Camera FOV | `mirv_fov <degrees>` / `mirv_fov default` | Replay camera override; default releases it |
| Camera controls | `mirv_input` | Shared HLAE parser connected to the Deadlock camera runtime |
| Camera path | `mirv_campath` | Shared HLAE path parser linked to the editor's markers |
| Camera motion IO | `mirv_camio` | HLAE `.cam` import/export with FOV |
| Scheduled commands | `mirv_cmd` | Shared time/tick scheduler driven by the Deadlock main frame |
| Demo navigation | `mirv_skip` | Uses Deadlock's tick/time pair |
| Panorama | `mirv_panorama` | Panel opacity and script evaluation in Deadlock's UI context |
| Filmmaker tools | `mirv_filmmaker` | Main surface; `mirv_deadlock` remains an alias |
| Runtime evidence | `mvm_debug start|stop|status` | Same debug command name in both games |

Enter a command without arguments for help. Commands not listed here are not implied to be ported. Colors were excluded. Entity enumeration and entity-backed follow and attach cameras are live (SEH-guarded reads, about 0 ms per scan; see below). Deadlock has no `mirv_viewmodel` transform command and no `mirv_streams` pass-splitting equivalent.

## Address diagnostics after a game update

`mirv_filmmaker health addresses` (also available through `mirv_deadlock`) prints the hook's cached address registry, including unresolved required entries and moved RVAs. It does not call engine interfaces or read live engine objects. Run it before the full `health` report when vtable compatibility is uncertain. With `mvm_debug` running, each row is also recorded under `deadlock.address`. Resolved addresses alone do not prove that the interfaces or camera are valid.

The startup boot trace reports the actual client and Panorama resolution, plus the view, present and pre-UI installation results. If the main-frame installation fails, the main-thread pump is unavailable. The custom netcon listener may still listen, but it will not process its command queue. Native VConsole is a separate diagnostic route when command registration succeeds. Before sending commands, check that its port belongs to the intended game process.

For read-only view-call tracing during compatibility work, launch with `-mvmDebug -mvmViewProbe`. The probe validates the current CViewRender RTTI slot and prologue, then calls through without camera writes or the main-thread pump. `mirv_filmmaker view` reports its call count. Debug records include camera reads, callback and installation thread IDs, and module-relative stack frames. Logging covers the first four calls and then every 256th call through call 16384. The counter keeps running. This diagnostic replaces normal view-hook installation for that launch. It is off by default.

`mirv_filmmaker engine exec "echo MVM_CHECK"` submits the single quoted argument through the hook's engine command adapter. `submitted` confirms that the adapter call returned. To prove execution, check the command's own output or resulting state. Missing, empty or multiple arguments are rejected. With debug logging active, `deadlock.engine.exec` records the slot, submission result and command. Use this for backend control and adapter verification when the custom queue is down.

## Camera-path gesture cancellation

`scrub <tick>` checks the current path on every update and accepts only finite ticks. Removing markers until fewer than two remain cancels the active scrub or playback immediately. A later `scrubend` then has no stale release tick to seek. Loading another saved shot successfully also cancels its previous motion. Valid scrubs still clamp to the path and seek on a normal release. `deadlock.shot.cancel` records the reason and previous motion flags in the debug log.

## Replay project identity

`mirv_filmmaker health` includes `demofile`, the resolved path of the replay open in Deadlock. The path comes from the engine filename accessor. It covers replays opened through console commands, and replays switched to after a `+playdemo` launch. It is empty outside replay playback, or when the filename cannot be resolved to an existing file. `path save` therefore cannot fall back to a stale launch argument after a disconnect. `deadlock.demo.path` records newly resolved names while debug logging is active.

## Pending shot saves

`health` and `path` report `shot saves: pending=<projects> bytes=<payload bytes>`. Each edit captures its current replay destination and serialized shot together. After a failed write, the newest snapshot for that project is kept. Edits to other replays cannot replace it. Automatic retries wait five seconds per failed project and attempt at most one project per frame. `path save` retries the current project right away, without discarding another project's data. `path load` prefers that project's pending snapshot over its older disk contents.

`deadlock.shot.save` records automatic write results, destination, payload bytes and pending count. The queue lives in memory for the running game process. Pending snapshots are not yet crash or restart recovery files. A zero count means all queued snapshots have been written.

## Read-only panel geometry enumeration

Run `mirv_filmmaker livejs load "<absolute path to Automation/livejs/deadlock-dump-ui-geometry.js>"` after the HUD exists. The script enumerates the workspace, the native replay bar, the tooltip manager and the context-menu manager. `GEOM` TSV rows contain depth, ID, type, accumulated design-pixel position, size, visibility and short label text. Pair the output with a game-window screenshot. A present panel with zero size differs from an absent panel.

Traversal uses an iterative stack. It reads at most 4000 rows and child reads across all roots, to a maximum depth of 64. `GEOMTRUNCATED` reports a panel limit, child limit, depth limit or child-read error. `GEOMEND` gives the number of rows emitted. It does not mean the output is complete. `GEOMMISSING` identifies absent roots, and `GEOMERR` identifies a failed probe. The script changes no panel properties and keeps no scheduled work. With `mvm_debug` active, its console output is also kept in the diagnostic log.

## Camera FOV

```text
mirv_fov 70
mirv_fov default
```

The override writes horizontal FOV to the accepted Deadlock view. Values must be finite and strictly between 1 and 179 degrees. Invalid input leaves the previous override in place. The native `citadel_camera_hero_fov` value and the view value do not need to match. In the measured 1920x1080 replay, hero FOV 90 produced a view FOV near 106.2.

Zoom handling is opt-in, because CS2's unzoomed-FOV threshold is not a Deadlock contract:

```text
mirv_fov handleZoom enabled 1
mirv_fov handleZoom minUnzoomedFov 90
mirv_fov handleZoom enabled 0
```

`minUnzoomedFov current` takes the current camera value. `mirv_filmmaker camera` reports the last accepted view. A matching value proves the write. Before and after game captures are still required to verify the rendered projection.

## Paths and camera IO

Add keys with a replay loaded, after seeking finishes. Console add, selection, editing and XML load update the editor's marker store. Editor edits refresh the console path. Marker edits use the existing `project.mvm` `campath` autosave.

```text
mirv_campath add
mirv_campath print
mirv_campath select all
mirv_campath edit fov 80
mirv_campath edit interp position linear
mirv_campath edit interp rotation sLinear
mirv_campath edit interp fov linear
mirv_campath save "shot.xml"
mirv_campath load "shot.xml"
mirv_campath enabled 1
mirv_campath enabled 0
```

The shared cubic evaluator needs three keys. Linear interpolation needs only two. Enabled console path playback follows replay time and yields to editor path playback or scrubbing. CAM import overrides that result, and `mirv_fov` applies last. `mirv_campath draw enabled 1` uses the editor's path drawing. HLAE axis and frustum gizmos are not implemented.

Use explicit XML save and load to keep the HLAE curve representation. The `campath` section's JSON does not store every detail. Per-channel interpolation, console hold and offset state, and console selection are not all represented. Console overrides are session settings, not persistent startup settings.

```text
mirv_camio export start "shot.cam"
mirv_camio export end
mirv_camio import start "shot.cam"
mirv_camio import end
```

Import aligns the first frame to the current replay time. Export records the final camera once per advancing demo timestamp. Pausing does not append duplicates. A backward seek stops export instead of writing out-of-order rows. Replay unload closes import and export. Failed files are reported and do not stay active.

## Panorama

```text
mirv_panorama panelStyle panelId=<DeadlockPanelId> opacity=0
mirv_panorama panelStyle panelId=<DeadlockPanelId> opacity=1
mirv_panorama panelStyle panelClassName=<DeadlockClass> opacity=0.5
mirv_panorama eval "$.Msg('Deadlock console is working')"
```

Exactly one ID or class selector is required. The command reports the match count. CS2 panel IDs are not aliases for Deadlock panels. `mirv_deadlock ui_eval` and `mirv_filmmaker ui_eval` are also script entry points.

## Entities, follow / attach camera, and Go To

```text
mirv_filmmaker entities [list <filter> <n>|probe <index>]
mirv_filmmaker listentities [n]
mirv_filmmaker follow toggle|live|place|reposition|clear
mirv_filmmaker follow mode lockon|attach
mirv_filmmaker follow hero <index> | nearest | goto | target clear
mirv_filmmaker follow attach origin|chest|head|hand_r|weapon
mirv_filmmaker follow offx|offy|offz|rotpitch|rotyaw|rotroll|fov <v>
mirv_filmmaker follow look|pos|predict|deadzone|maxturn|deathhold <v>
mirv_filmmaker follow releaseondeath|holdlast|retarget 0|1
mirv_filmmaker follow resetframing|resettracking
```

`entities` is the raw diagnostic. `list <classFilter> <maxRows>` walks the client entity table. `probe <index>` dumps one entity's identity block, its resolved fields and both handle interpretations. `listentities` prints the player-only roster that the follow camera builds. It shows index, real hero name, real player name, team, health and alive state. A name that looks wrong in the UI can be checked here, without guessing which of the two paths disagrees. Both work under `mirv_deadlock` too.

Hero pawns are entity class `player`, not the schema class `C_CitadelPlayerPawn`. Their controllers are `citadel_player_controller`. A pawn's hero codename ("hero_atlas") reads straight from its identity block. The player's real name comes from the controller, reached through `m_hController`, which holds `m_iszPlayerName`. A pawn's own copy of that field is unset.

Lock-on keeps the camera's placed position fixed and only turns to aim. `place` and `reposition` capture the free camera's current pose once, and nothing after that moves it. `offx/offy/offz` are a world-axis offset added to the aim point (not the camera). `rotpitch/rotyaw/rotroll` trim the resulting look. Attach rides a point on the hero's model: the same six numbers become a local offset (forward, right and up from the mount) and a rotation composed onto the mount's own orientation. The controls match CS2's own Follow inspector (`Off X/Y/Z` and `Pitch/Yaw/Roll`, one tile set for both modes). The file `AfxHookSource2/Filmmaker/Deadlock/DeadlockFollowSolveMath.h` lists the exact CS2 line numbers that ground each rule. The attach mount names (`origin`/`chest`/`head`/`hand_r`/`weapon`) are real, per-hero bone names resolved via `Automation/tools/deadlock-hero-bones.py`. The mount's origin, however, is still a fixed offset from the pawn's entity origin, not a read of that bone's animated transform. Deadlock has no known way yet to read a live bone world matrix. See [cameras.md](cameras.md).

`follow goto` puts the free camera behind and above the targeted hero (`hero <index>` or `nearest`), looking at their eyes. It does not depend on whether `on` or `placed` is set. A hero can be picked and Go To used before any camera is placed. It fails with a clear message when no hero is targeted, or when the targeted hero is not in the replay.

```text
mirv_filmmaker overlay [on|off]
```

In-world hero boxes over every hero pawn. The follow camera's current target is gold. Off by default. See [cameras.md](cameras.md#in-world-hero-boxes-mirv_filmmaker-overlay-off-by-default) for the full contract: when it draws, what it does not draw yet, and how it was verified. A bare `overlay` reports its ready and enabled state and draw counters without changing anything.

## Director HUD and F8 blackout

```text
mirv_filmmaker blackout [on|off|toggle|status]
```

F8 works with the workspace open or closed, on a loaded replay. It matches CS2: it clears every overlay, both ours and the game's, to a clean screen, then restores it exactly as it was. This is a render-time flag only. Nothing closes, so an in-progress reposition, the open workspace and its selected marker all survive a blackout. It hides the workspace chrome, the native gameplay HUD (health bars, ability icons, minimap, spectator strip) and the native replay bar. It also releases the scaled world preview to full screen. Without that, the world would stay shrunk into the preview rect while the chrome that framed it is gone. Blackout clears itself when the replay it was turned on in ends. Deadlock has no `crosshair` convar to save or restore (CS2's blackout does), so nothing else needs doing there.

A small DIRECTOR status card (camera owner, playback state, speed, position, angles, FOV, tick and time) shows in the top-left corner while the workspace is closed. This is the one deliberate difference from CS2's always-on corner card. Deadlock's workspace already reports the same numbers, through the `camera` console report and the Follow inspector, whenever it is open. The card would only duplicate them there. It exists for watching a replay with the tool shut, when there is otherwise no way to see what the camera is doing. Blackout and an active render capture hide it, as they hide the hero-box overlay.

## Native alternatives

These names were returned by the installed game's console. Availability does not prove a visible effect in every replay camera mode. Query values before changing them, and restore those values after testing.

| Native name | Console description / intended use | Observed value |
| --- | --- | --- |
| `citadel_camera_hero_fov` | FOV when following a hero | 90 |
| `citadel_camera_height` | Vertical offset of the camera look-at point | 63 |
| `citadel_camera_offset` | Horizontal offset of the camera look-at point | -25 |
| `citadel_camera_height_approach_speed` | Camera height approach speed | 800 |
| `citadel_camera_height_npc` | Look-at height when viewing NPC units | 33 |
| `citadel_hide_replay_hud` | Replay HUD visibility control | false |
| `fov_desired` | Base field of view | 75 |
| `viewmodel_fov` | Native viewmodel FOV variable | 54 |
| `r_drawviewmodel` | Render viewmodel toggle | true |

`citadel_camera_hero_fov` visibly changed framing in the paused replay. Setting `r_drawviewmodel` to false and `viewmodel_fov` to 20 was accepted, but the third-person hero and gun stayed visible in the screenshot comparison. These controls do not replace CS2's first-person viewmodel positioning system in this replay mode. Both values were restored after the check.

The `citadel_camera_override_x_offset`, `citadel_camera_override_y_offset`, `citadel_camera_override_z_offset`, `citadel_camera_override_fov` and `citadel_camera_use_overrides` were unavailable in this console. Use the confirmed names above, or the mod's camera runtime, rather than building controls around unavailable names.

## Registration and verification

Both games expose `VEngineCvar007`, but Deadlock's registrar is at slot 44, not CS2's 42. It also needs a creation descriptor and MSVC's hidden return buffer. See the [interface contract](signatures.md#native-console-registration).

Start `mvm_debug` before reproducing. `-mvmDebug` starts it during client initialization, before registration. `mirv_filmmaker cvarprobe` reads the engine table. Netcon alone does not test registration, because it can dispatch DLL commands directly. To exercise the native engine callback from netcon, start a single-semicolon batch with an engine command:

```text
echo native_begin; mirv_input; mvm_debug status; echo native_end
```

Live checks:

- All ten expected core names appeared in the native table. Native dispatch, Panorama evaluation and a panel-style match worked. Native `mirv_input camera` and FOV changed the accepted camera.
- FOV 70/default changed the rendered projection in matching paused-game screenshots. NaN input preserved the prior valid override. The native hero-FOV alternative also changed framing.
- A scheduled command executed at tick 7821 for target 7820 (the shared scheduler's one-tick epsilon), with both execution and the engine's result recorded in `mvm_debug`.
- Paused CAM export wrote one data row. Import applied a deliberately shifted position and FOV 60; ending import restored the game camera. Replay unload closed active import.
- Moving export wrote 206 strictly increasing samples over 3.081593 demo seconds across two pause/resume cycles and stayed active while paused. A real backward seek stopped export.
- Campath edits and XML load synchronized editor markers on a copied replay. Native XML load and held-path evaluation reproduced the saved camera. Original replay markers were not edited.
- PrintWindow captures showed fitted previews in Camera Editor, Config and Render. Clicking Exit restored the full game view, playback updated afterward, and replay unload returned to a normally rendered menu. These checks cover preview and exit behavior, not every render output format or the separate Blitz selection transition.

Export has to handle two clock details: resume can move interpolated time backward within a tick, and pause can decrement the reported tick at an unchanged timestamp. The exporter skips those duplicate samples instead of stopping or writing out-of-order rows. The behavior is covered by the camera-command math test and the live debug trace.

Local evidence is under `Automation/runs/deadlock-console-research/` (ignored generated output), including `fov-default.png`, `fov-70.png`, `workspace-camera.png`, `workspace-config.png`, `workspace-render.png`, `exit-resumed.png`, `replay-unloaded.png`, `camio-release.cam` and `release-checks.txt`. Capture sidecars include the method, setup commands and health state.

Upstream's [Source 2 command list](https://github.com/advancedfx/advancedfx/wiki/Source2:Commands) documents CS2 support. No verified upstream drop-in Deadlock implementation was found. This project's adapter uses the installed binaries and the [Deadlock SDK branch](https://github.com/alliedmodders/hl2sdk/tree/deadlock), rather than matching the interface name alone. The native alternatives above come from live console output.
