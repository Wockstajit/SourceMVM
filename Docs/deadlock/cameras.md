# Deadlock cameras

Entity integration is enabled. The `AFX_DEADLOCK_ENTITIES` CMake option (`AfxHookSource2/Deadlock/CMakeLists.txt`) builds `DeadlockEntities.cpp` by default. Passing `-mvmNoEntities` at launch swaps in the stubs from `DeadlockEntitiesDisabled.cpp`, for an A/B frame-rate run or to isolate a crash. With entities on, hero scans measure about 0 ms. Entity reads are SEH-guarded. The follow camera's own A/B test showed no measurable frame-rate cost. For the full command surface, see [console-commands.md](console-commands.md#entities-follow--attach-camera-and-go-to).

The framed preview compositor is disabled. Camera, Key Editor, Config and Render open as Panorama overlays over the full game view. Camera playback and beauty capture do not depend on that preview.

Three systems write the view, and only one may write it in a given frame. This page is the contract between them: what each one needs from the game, and how to prove it still works after a Deadlock update.

## Who owns the view

Resolved once per main-thread frame in `Workspaces_RunMainThreadFrame`, in this order:

1. **The shot**: a camera path that is playing, or a timeline being scrubbed. An explicit, timed camera move. Nothing interrupts it.
2. **The graph**, while its DRIVE toggle is on. Same argument: the user is watching a curve edit.
3. **The follow camera.**
4. Otherwise the game's own camera, or HLAE's free camera when that is enabled.

`mirv_deadlock camera` prints the pose the engine actually used this frame and names the owner. It is the only reliable way to tell a camera feature that did nothing from one that did something small. The owner name is a recorded value (`CameraRuntime_LastAppliedOwner`). It is captured when the view hook resolves and applies the pose. It is not recomputed when the report runs. A console command can run at a point in the frame where `Frame_MainCount()` is one tick off from when a pushed pose was applied. A fresh recomputation there would name "the game" even while a feature's pose reaches the screen. This was measured live.

`Shot_OwnsView()` answers whether something is already writing the camera. It does not answer whether a mode is switched on. Playing or scrubbing a path with fewer than two markers pushes no pose, so it reports false and the follow camera keeps tracking.
## The camera path

Markers snapshot the live camera at the demo tick. The path uses the shared, engine-free `CamMarkers` + `CamPathEval` + `Shared/CamPath` units. These are the same natural cubic position spline and quaternion rotation curve the CS2 build uses, not a reimplementation.

Playback starts from the playhead, not from the first marker. Drag the track to the moment you care about, release, press PLAY SHOT, and the shot runs from there. Playing also resumes the demo, and pausing pauses it. The shot is driven by the demo clock, so a paused demo means the shot advances by zero every frame and looks broken.

Scrubbing previews without seeking. While the playhead is dragged, the world does not move and the camera shows the path's pose at that tick. Release ends the scrub and seeks, so the idle fallback (the playhead equals the demo tick) is correct again immediately.

Paths autosave into the replay's project folder, as the `campath` section of `%APPDATA%\HLAE\source_mvm\demos\deadlock\<demo>\project.mvm`. Marker edits, retimes and curve changes survive closing Deadlock without an explicit Save click.

## Camera motion

Deadlock has no handheld camera shake. If Deadlock needs this kind of continuous procedural camera motion, it needs a real, exposed control first.

The Path Camera's Shake group assigns a RAGE shake style, intensity and speed to each marker. A style continues across adjacent markers with the same style and blends over 0.35 seconds when the style changes. These keys autosave as the `camshake` section of the same `project.mvm` and follow marker retimes.

Global follow framing and tracking, path display and render settings persist in `%APPDATA%\HLAE\deadlock-filmmaker.json`.

## Beauty capture

The Render workspace captures the finished back buffer as a beauty pass. PNG capture was verified live at 1920×1080 with an exact 64-tick demo clock: a three-frame 16 fps range completed with zero dropped frames. Output FPS does not need to divide the demo tick rate. Render sets `host_framerate` to the chosen FPS automatically, so the dial is not required.

## The follow camera

A placed camera with a hero target, in one of two modes. CS2's own Movie/FollowCamera.cpp is the parity spec. `AfxHookSource2/Filmmaker/Deadlock/DeadlockFollowSolveMath.h` cites the exact CS2 line for each rule below.

- Lock-on keeps its placed position fixed for good and only turns to aim. `Place` and `Reposition` capture the free camera's current pose once. Nothing per frame recomputes it. The Off X/Y/Z tiles move the aim point in world axes. Pitch/Yaw/Roll trim the resulting look. Tracking keeps the untrimmed aim, and the rotation trim is applied once to the final view. Repeated paused frames cannot accumulate it, and clearing a trim restores the tracked aim.
- Attach rides the hero: position and rotation both move with it every frame. The same six tiles become a local offset (forward, right and up from the mount) and a rotation composed onto the mount's own orientation.

Both aim at the hero's eye for lock-on's target point, not their origin. A camera aimed at the feet frames the floor, which looks slightly wrong in a way that is hard to name.

The tracking sliders (`look`, `pos`, `predict`, `deadzone`, `maxturn`, `deathhold`) form a filter, not a formula. Where the camera lands this frame depends on where it was last frame. `look` and `pos` are smoothing half-times in seconds, so a smaller number chases harder. The filter state is reset whenever continuity breaks: a different hero, a mode change, or a seek. The lock-on placed pose is not part of that reset. It lives outside the resettable filter state, so switching the target hero or moving between modes cannot move a placed camera. Only `place`, `reposition` and `clear` may move or forget it.

Camera ownership requires a valid replay tick. After a confirmed replay unload, follow turns off and forgets its placed pose and hero target. Freecam and pending placements are released too. Framing and tracking preferences remain saved. Cleanup runs on the main pump, even with UI disabled. The existing unload debounce keeps placement through transient seek state changes.

The clock is the demo tick, never wall time, so the filter settles identically on a second pass over the same shot. A rendered take therefore matches its preview.

Attach mount origins are still approximations, but the mount names are real, measured data. `origin`, `chest`, `head`, `hand_r` and `weapon` each resolve to an actual bone on all 38 heroes, via `Automation/tools/deadlock-hero-bones.py`. The data lives in `Automation/data/deadlock-hero-bones.json` and was extracted from the live game's own hero models with Source2Viewer-CLI. That table is what a live bone-transform read would key on. Deadlock has no known way yet to read a bone's animated world transform. The per-bone matrix array is not a schema field (see the bone path in `Automation/runs/deadlock-cam-P0/FINDINGS.md`'s schema dump). So the mount origin is still a fixed offset from the pawn's entity origin, rotated by the hero's yaw only. Picking Head does raise the mount, but it is not yet a read of the model's actual head bone.

## Entities and real names

Hero pawns are entity class `player`, and their controllers are `citadel_player_controller`. A pawn's own hero codename ("hero_atlas") is read straight from its identity block (`entity+0x10` -> the identity struct -> `+0x18`, right next to the class name at `+0x20`). No schema field or `CitadelHeroData_t` chain is needed. That chain was probed and resolves to garbage on this build. The player's real name takes one more hop. The pawn's `m_hController` handle resolves to its controller, and the controller's own `m_iszPlayerName` (inline, not a pointer) holds the actual name. A pawn's copy of that field is unset. `mirv_filmmaker listentities` prints exactly this resolved roster. See [console-commands.md](console-commands.md#entities-follow--attach-camera-and-go-to).

## In-world hero boxes (`mirv_filmmaker overlay`, off by default)

A box is drawn around every hero pawn while the Camera Editor is open. The currently targeted hero is gold, and everyone else is a cooler translucent white. A user can see who the follow camera would pick before clicking anything. `AfxHookSource2/Deadlock/DeadlockOverlayDrawer.cpp` draws the boxes in the Present hook's pre-UI moment, ahead of the scaled-preview blit. The boxes therefore become part of what the blit scales into the preview rect. CS2's `CampathDrawer` uses the same ordering ahead of its own preview blit. Positions come from `ProjectBox` in `DeadlockOverlayMath.h`, projected from the pose the view hook applied this frame. No engine world-to-screen matrix is needed. A new, self-contained shader pair (`afx_deadlock_overlay_vs_5_0`/`ps_5_0`) reads a per-frame dynamic structured buffer by `SV_VertexID`. This is the same vertex-buffer-free trick the blit's own full-screen triangle uses. Deadlock's software render command list keeps vertex-buffer bindings made by callbacks, and its internal list is 16-bit indexed.

The overlay is hidden whenever something else is driving the view (a playing shot, the graph, or a live follow camera), or a render job is running. It is also hidden while the Camera Editor is closed or no replay is loaded. It is an editing aid, not a permanent HUD element, matching CampathDrawer's own capture-suppression precedent.

v1 scope: hero boxes only. The dolly path polyline and marker diamonds are not drawn yet. CS2's own `afx_line_vs_5_0`/`afx_line_ps_5_0` shaders are real, working and already compiled in this repo. They draw world-space wide lines with GPU-side near-plane clipping. Their vertex shader needs a world-to-screen `VMatrix`, and no live probe has found one on Deadlock. Porting them means deriving that matrix first. Hero boxes need only `ProjectBox`.

Verified live: hero boxes tracked their models exactly across capture and pixel-sample checks. The selected hero's box measured distinctly gold against the others' cooler white (RGB channel order confirmed, not just eyeballed). A ~45 s FPS window with the overlay live and the replay playing showed no regression against the same-session baseline. Render-capture suppression is implemented and code-reviewed. The published snapshot is cleared, not merely hidden, whenever `Render_GetState() != Idle`. It has not been independently confirmed against a completed render's output frames.

## Go To

`follow goto` places the free camera behind and above the targeted hero, looking at their eyes (`AfxHookSource2/Filmmaker/Deadlock/DeadlockGoToMath.h`, CS2's own Follow-inspector "Go To"). It does not depend on `on` or `placed`. Pick a hero (or `nearest`) and go look at them before placing a camera. Behind and above are decided by yaw alone on the XY plane. A hero's pitch and roll never change where the camera goes.

## Director HUD and F8 blackout (`mirv_filmmaker blackout`)

This matches CS2's F8 handling in `MovieMode.cpp` (`AfxHookSource2/Filmmaker/Movie/HudBlackout.h`) and its always-on `MovieHud` status card. It runs on Deadlock's own runtime state, because Deadlock has none of CS2's `MovieMode`, `CameraBridge` or `ThirdPersonCamera`. `AfxHookSource2/Deadlock/DeadlockHudBlackout.h/.cpp` holds the flag. It is a render-time-only toggle that clears the workspace chrome, the native gameplay HUD and the native replay bar, and releases the scaled world preview to full screen. It closes and resets nothing. A second press restores the screen exactly as it was. Deadlock has no `crosshair` convar (a signature sweep for it found zero hits), so there is nothing to save or restore, as CS2's blackout does. The crosshair is a gameplay HUD child like any other, and it disappears with the rest of the native HUD.

`AfxHookSource2/Filmmaker/Deadlock/DeadlockMovieHud.h/.cpp` and `DeadlockMovieHudJs.h` form the director status card. It shows the camera owner (from the same view-owner resolver the `camera` console report uses), playback state, speed, position, angles, FOV, and tick and time. CS2's card owns a separate `PanoramaBridge` and panel. The Deadlock card is one more fragment of the single script that `DeadlockWorkspaces.cpp` already builds and pumps every frame, whether or not the workspace is open. `DeadlockNativeHudJs.h`'s native-HUD driver uses the same pattern. A second bridge instance would recompile its own script against the same HUD context, which is pure overhead. The card builds its panel lazily and re-attaches on a stale reference. It does not depend on a rebuild to recreate the panel, because a rebuild never clears its top-of-file guard.

Shown only while the workspace is closed. This is the one deliberate difference from CS2's always-on card. CS2's editor is a floating panel that leaves most of the screen clear. Deadlock's workspace docks a full-width band and a full-height inspector column around the preview. Both already report the same pose, owner and speed whenever they are open. The card covers the case that CS2's card does not need to cover: watching a replay with the tool shut, when otherwise there is no way to see what the camera is doing. Blackout and an active render capture hide it, as they hide the hero-box overlay.

F8 is bound in `DeadlockHotkeys.cpp`, deliberately outside the workspace-gated binding table that every other shortcut (K/Space/Left/Right) shares. Its purpose is a clean screen for watching the replay, often because the tool is closed. So it cannot require the workspace to be open, as those shortcuts do.

Verified live: with the Camera Editor open on the native (Regular Timeline) tab, toggling blackout hides the workspace chrome, the native gameplay HUD and the native replay bar. `holdPanelShown(id, false)` is deliberately weak against a panel already force-shown by `holdPanelShown(id, true)`, so the bar needs a direct `visible = false` instead. Blackout also releases the world to full screen, and a second toggle restores everything exactly. The director card shows correctly with the workspace closed, and it hides correctly once the workspace opens.

## What the game has to give us

| Need | Where it comes from | Fails as |
| --- | --- | --- |
| the entity table | two byte patterns in `client.dll`, both identical to CS2's | `mirv_deadlock entities` says `list=MISSING` |
| hero positions, health, team | schema lookups **by name** through `DeadlockSchema` | a named field reads `-1` and only its own feature stops |
| which entity is a hero | entity class name `player` | zero heroes with a healthy table |

`mirv_deadlock entities` reports all three in one line. `mirv_deadlock entities list <filter> <n>` walks the live table. It separates three faults that all look like zero heroes: an empty list, a class name that does not read, and a hero class with a different name.

## Verifying

```powershell
powershell -File Automation\verify\verify-deadlock-cameras.ps1
```

Twelve checks against a loaded replay: the entity list resolves and finds heroes, a path plays and moves the camera, scrubbing previews the path and moves the playhead, the follow camera solves and tracks its hero, and attach puts the camera somewhere lock-on does not. Add `-SkipLoad` when a replay is already up.

Two traps the script encodes:

- **Turn the free camera off first.** HLAE's free cam pins the view. With it on, the pose stops following the demo, every marker snapshots the same place, and the resulting path evaluates to one pose from end to end. That looks like "playing the path does nothing".
- **Sample a mode change while the demo is running.** Paused, the engine produces no new view, so the pose read back is the one the last rendered frame used, which is the previous mode's. A working mode change then looks like no change at all.
