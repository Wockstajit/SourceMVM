# Camera Editor Mode

The dedicated in-demo **camera editor workspace**: a Panorama overlay that frames the live game as a preview viewport with a right-side property inspector, a bottom timeline, and workspace tools (Customize, Render, graph editor). Toggle it with `mirv_filmmaker editor toggle` (commonly bound, e.g. `bind "F9" "mirv_filmmaker editor toggle"`).

Related docs: [panorama_ui_guide.md](../panorama/panorama_ui_guide.md) (how the Panorama bridge and JS architecture work), [customize-feature.md](customize-feature.md) (the Customize modal), [follow-camera/](../follow-camera/README.md) (the Follow system that the editor's Follow inspector edits), and [filmmaker_config_panel.md](filmmaker_config_panel.md) (the standalone Config panel, the editor's "little sibling").

## What the workspace is

`CameraEditorHud` is a **presentation shell** over the existing camera back-ends. It adds no new editing logic. While enabled, it:

- frames the live game as a **preview** (top-left) with a dimmed letterbox. The preview is either a crop of the full-screen frame or, with `editor scale on`, a true scaled viewport. The scaled viewport shrinks the whole frame through a D3D blit and a matching HUD CSS transform. The two scaling paths must match 1:1.
- shows a right-side **inspector** with per-mode sections (camera path / Follow camera / playback);
- docks a **bottom panel** whose mode is selectable (`editor curveeditor ...`): the custom camera timeline, the experimental graph editor, or the native CS2 demo timeline;
- suspends mirv free-cam mouse-look and shows the OS cursor while its UI wants the mouse.

Reusable workspace choices persist in `%APPDATA%\HLAE\filmmaker_settings.json`: scaled preview, bottom-panel mode, game-HUD mode, debug overlay, Path/Follow inspector tab, and the Advanced Tracking disclosure. Closing or reopening the editor does not reset them.

Path, Follow and Scene Keys use a grouped rail with one selected inspector pane. Path's selected camera and path controls share a card. Follow's mode and target controls share a card, while its Live/Stop control stays pinned above the rail. Switching panes preserves each control's state. The [shared workspace contract](../panorama/panorama_ui_guide.md#shared-workspace-contract) owns chrome, scroll and value-drag behavior.

Shot-specific state stays out of this global store. Camera transforms, selected path keys, Follow/Attach targets, offsets, rotations, FOV, and tracking values remain owned by their camera systems. One camera's setup is never copied onto another.

## The pieces (and where they live)

All paths under `AfxHookSource2/Filmmaker/`.

| Piece | Files | Notes |
|---|---|---|
| Workspace shell | `Panorama/CameraEditorHud.{h,cpp}` + `Panorama/CameraEditorJs.h` (+ per-section JS fragments `CameraEditorPathInspectorJs.h`, `CameraEditorFollowInspectorJs.h`, `CameraEditorCustomizeJs.h`, `CameraEditorDebugJs.h`) | The C++ bridge pushes state JSON per frame; JS emits `mirv_filmmaker ...` console commands back. |
| Reusable settings | `Config/FilmmakerSettingsStore.{h,cpp}` | Debounced persistence for workspace preferences; excludes camera/shot data. |
| Camera timeline (scrub bar, keys, easing) | `Panorama/CameraTimelineHud.{h,cpp}` + `CameraTimelineJs.h`; back-end `Movie/CamPlayback.cpp`, command surface `CameraTimelineCommand.cpp` (`camtl`) | Space and the play button use the same playback gate. |
| Graph editor (experimental) | `Panorama/GraphEditorExperimentHud.{h,cpp}` + `GraphEditorJs.h`; model `Movie/GraphExpModel.cpp`; command `GraphEditorCommand.cpp` | Graph mutations bake back into CameraPath markers so the graph and timeline share the path. Path-side edits do not reseed a live graph session. |
| Scene keys (animated lighting, sky, fog, weather, look) | Catalog `Movie/SceneKeyframeCatalog.cpp`, track and sampling `Movie/SceneKeyframeTrackMath.h`, runtime and commands `Movie/SceneKeys.cpp` (`scenekeys`); inspector `Panorama/CameraEditorSceneKeysJs.h`; strips in the graph dock's SCENE GRAPH mode (`GraphEditorJs.h`) | See [Scene keys and the Scene Graph](#scene-keys-and-the-scene-graph). |
| Marker / dolly path | `Movie/CameraPath.cpp` (+ `CameraPathPicking.cpp` for the crosshair hover pick), `Movie/CamMarkers.cpp`, `Movie/CamPathEval.cpp`; HUD `Panorama/MarkerHud.{h,cpp}`; command `MarkerCommand.cpp` (`marker`) | BO2-style: K = place, L = delete aimed, F = edit aimed (in free cam). |
| Follow / Lock-On camera | `Movie/FollowCamera.cpp` + `FollowTarget*`/`FollowEventIndex`; command `FollowCommand.cpp` (`follow`) | Full docs in [follow-camera/](../follow-camera/README.md). |
| Customize (loadout) modal | JS in `Panorama/CameraEditorCustomizeJs.h` (modal) + `CameraEditorCustomizePreviewJs.h` (3D preview wiring); queries `Cosmetics/CosmeticUiQueries.{h,cpp}`; backend `Cosmetics/` | Per-player skins, knife, gloves and agent: see [customize-feature.md](customize-feature.md). Includes the experimental 3D player preview ([panorama-player-preview-summary.md](../panorama/panorama-player-preview-summary.md)). |
| Render workspace | `Panorama/CameraEditorRenderJs.h`, `CameraEditorRenderLayoutJs.h`; backend `Render/` | Range, camera, game-HUD policy, output, validation, state restoration, and the recorder-backed beauty capture. See `Render/` and the Render workspace row above for the current contract. |
| Movie/director HUD + input | `Movie/MovieMode.cpp`, `Panorama/MovieHud.{h,cpp}`; input routing in `Filmmaker.cpp` (`MovieInput_*`) | Scroll = cycle cams, LMB/RMB = switch player, Space = pause, F8 = HUD. |

## Console surface

Everything routes through `mirv_filmmaker` (dispatcher: `FilmmakerCommand.cpp`; one `*Command.cpp` file per feature):

```
mirv_filmmaker editor [on|off|toggle]      the workspace itself
mirv_filmmaker editor scale [...]          true scaled preview viewport
mirv_filmmaker editor curveeditor [...]    native | graph | timeline | camera bottom panel
mirv_filmmaker editor hud [hidden|game|full|cycle]   game UI behind the editor
mirv_filmmaker editor debug [...]          viewport/HUD debug overlay
mirv_filmmaker camtl ...                   camera timeline (scrub, keys, easing)
mirv_filmmaker marker ...                  marker/dolly path
mirv_filmmaker follow ...                  follow / lock-on camera
mirv_filmmaker grapheditor ...             experimental graph editor
mirv_filmmaker cosmetics ...               customize backend
```

Drive the UI from netcon with short calls into predefined JS helpers (`$.Filmmaker.*`). `ui_eval` truncates at 256 bytes.

## Scene keys and the Scene Graph

The SCENE KEYS inspector tab picks which properties are animated. Turning one on gives it a strip in the graph dock's SCENE GRAPH mode, where its keys and curves are edited. CONFIG stays the baseline for every property that is off, and for every tick outside a keyed span.

**Names.** A property has one name, held in the catalog's name table, and both the inspector row and the graph strip show it. The name is the label of the CONFIG control that edits the same value. Where CONFIG's label only makes sense on its own page, the name adds the page's subject: CONFIG's LIGHTING > Sun "Rotate" is "Sun rotate", and its SKY > Sun & sky "Azimuth" is "Sun azimuth". Values that CONFIG keeps off its panels are named for what they drive, such as "Custom sun azimuth" for the absolute sun position. Groups follow CONFIG's tabs and cards: Effects (Lighting, Weather & Sky, Atmosphere) and Environment (Looks, Sky, Lighting, Fog, Weather, Ground), in CONFIG's order. The graph's group bars read "Effects · Lighting" and "Environment · Lighting", because both tabs have a Lighting card. The inspector lists the properties CONFIG shows by default, and the rest sit under More settings.

**The playhead.** Both graph modes drive the playhead with `grapheditor playhead`, so a SCENE GRAPH ruler drag moves the demo world as well (see [live demo scrubbing](filmmaker_demo_scrubbing.md)). Clicking a scene key moves the playhead to that key. Dragging a key takes the playhead with it, so the world on screen shows the dragged key's own value, and the release lands on the key. After any release, the playhead holds the released tick while the demo travels there, and then while the demo stays where it landed. A seek can land one tick past its target, and without the hold a key you jumped to read as "not here". A value edit at the playhead re-keys the key under it and only adds a key on a tick that has none.

**Editing.** A key drag sends its total offset since the press, so a step refused at another key or clamped at a range bound is not lost and the key stays under the pointer. Bezier handles stay inside the property's range, because the sampled value is clamped to it anyway. Each scene curve is fitted to what it draws: its keys, the curve between them and the handles of selected keys. The fit is padded and never wider than the property's range. It is frozen while a drag is open and refits on release. Angles are drawn the way they are sampled, the short way round (or in the key's set direction) and wrapped. The band starts below the ruler, so a key at the top of its range is never hidden under it. **FIT** in the bottom bar frames every visible key. A key created off screen and a jump to a key off screen both page the window to it. A colour strip's chip opens the shared CONFIG colour picker in a tray beside the strip list. A press outside the tray closes it.

**Diagnosis.** `mvm_debug` logs `scenekeys.set` (every new key, sampled re-keys, with the playhead it used), `scenekeys.move` (sampled drags) and `playhead.hold` (hold, landed, drop and why). `mirv_filmmaker scenekeys status` prints each armed or keyed property with its keys and the value resolved at the playhead.

Tests: `SceneKeyframeTrackTests` (catalog names and groups, sampling, drag offsets, serialization), `Automation/tests/scene-graph-tests.mjs`, `scene-keys-ui-tests.mjs` and `graph-timeline-nav-tests.mjs`.

## Known contracts / gotchas

- **All Panorama work runs on the game's MAIN thread** (`RunMainThreadFrame`). Calling `RunScript` from the render thread crashes V8. See CLAUDE.md's threading section.
- **Per-frame close paths must no-op when already closed.** Closing Customize must not unconditionally tear down unrelated dropdowns.
- **Release scaled HUD styles on exit.** Config/Editor temporarily write inline Panorama transforms to match the scaled preview. Teardown uses a two-phase release. First, snap `transform` to `none`, so Panorama refreshes compositor-backed panels such as `HudBottomGradient`. Then, on the next UI tick, assign `null` to the inline `transform`, `transformOrigin`, and `transition`. The second step returns ownership to CS2's native demo-controller rule, which keeps the lower gameplay HUD above its timeline. Permanently leaving `transform: none` masks that rule.
- Round-marker rescaling, native-bar `EndPlayback`/gear integration, and the settings popup are owned by the Camera Timeline/Editor Panorama fragments.
- **The docked transport row is scaled to fit, which takes two writes, not one.** Docking narrows CS2's demo bar to sit left of the inspector. Since the 2026-10-14 CS2 build its `ControlRow` reads: play, time/tick, the 5s skips, round nav, `MvmSpeedSpacer`, the speed scrubber, `ControlRowSpacer`, player stepper, next event, camera mode, and our `MvmBarButtons` group (LOOP icon, MOUSE, CAM EDITOR, CONFIG). `arrangeRow` (CameraTimelineJs) moves the scrubber between the two flexible spacers so it sits mid-bar. Only the spacers flex; every other cluster is fixed px, so a narrow dock leaves the spacers at 0 and Panorama's default `overflow: squish` crushes the clusters during layout, before any transform applies. `fitControlRow` therefore sizes the row to its fixed content plus a 44px buffer (without this the row clips its own children at the dock width), then scales it from its left edge by `dock / needed`, floored at 0.62. CS2's own `UpdateControlRowFit` collapses its labels while `ControlRowSpacer` reads 0. While docked, `dockPopups` lifts the native controller to z58 and scales CS2's player list and round menu by the same factor, so they open above the editor at matching size. Read the fit back with `mirv_filmmaker camtl eval $.Msg($.CamTimeline.dbgBar()+String.fromCharCode(10));`. `need > avail` with `k == 1` means the fit is not running.
  - The scale factor is published as `$.MirvBarFitK`, because the speed scrubber in that row is cursor-driven. `GetPositionWithinWindow()` is transform-aware and `actuallayoutwidth` is not. Without `$.MirvBarFitK`, every drag would be off by `1/k`.
- **A one-frame UI flip needs a log, not a screenshot.** `capture-game-burst.ps1` costs ~23ms a frame at 1600x1200, so it samples roughly every third frame. A single bad frame usually falls between two captures. `mirv_filmmaker camtl eval $.CamTimeline.trace(1);` emits one `[camtl.trace]` line per CHANGE of the bar's decided state (host, tab, dock width and scale, HUD view, preview rect). `mvm_debug start` captures those lines with a frame stamp. A state that exists for exactly one frame prints twice instead of hiding.
- Playback Loop controls (toggle, range highlight, draggable In/Out flags) belong only to the Regular Timeline, including its standalone and Config-docked forms. Camera and Graph timelines do not duplicate them. Switching views preserves the shared loop state and saved range.
- Dragging UI must be built from `Slider` panels. HUD JS has no mouse-move event.
