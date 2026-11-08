# Modular layout (CS2)

The CS2 filmmaker UI has two layouts. Both use the same Panorama panels, so a control looks and
works the same in either one.

| Layout | What you see |
| --- | --- |
| **Workspace** (default) | CONFIG or the Camera Editor takes over the screen: the game shrinks into a scaled preview, the column sits on the right and the dock along the bottom. One of them is open at a time. |
| **Modular** | The game stays full screen. A header bar along the top opens each panel. CONFIG and the Camera Editor are windows you can move, scale and close, and both can be open at once. |

Switch with the **Modular windows** row in Editor Settings (the gear), the **WORKSPACE / MODULAR**
switch at the right end of the header bar, or the console:

```text
mirv_filmmaker layout [workspace|modular|toggle]
```

The choice is the `layout` UI key in `%APPDATA%\HLAE\filmmaker_settings.json`, so it survives a
restart. Modular applies to demo playback; the live `mvm_test` CONFIG panel keeps its sidebar.

## The header bar

`Panorama/WorkspaceHeaderHud.{h,cpp}` and `WorkspaceHeaderJs.h`. Built into the HUD context while a
demo plays in the modular layout, and torn down otherwise. "A demo plays" is the camera timeline's
reading (`CameraTimelineHud::DemoActive`), which rides out the few frames a `demo_gototick` seek
reports no demo, so a scrub does not tear the bar and its dock down. Its tabs use the dock ribbon's
box and each lights in its panel's accent while that panel is open:

| Tab | Command |
| --- | --- |
| CONFIG | `mirv_filmmaker config toggle` |
| CAMERA EDITOR | `mirv_filmmaker editor toggle` |
| TIMELINE | `mirv_filmmaker dock toggle` |
| RENDER | `mirv_filmmaker editor render`, or `render close` while it is open |
| CUSTOMIZE | `mirv_filmmaker editor customize` |

The gear opens the shared Editor Settings card (`EditorSettingsPopupJs.h`). It covers the screen (a
click beside the card closes it), so while it is up every click and wheel notch is the UI's
(`WorkspaceHeader_PopupOpen` in `MovieMode`), the free cam included. The bar hides while the
Render workspace is up, during a capture, and under F8.

With no window open, the bar hosts the TIMELINE dock itself (`DockHost::Header` in
`Movie/TimelineDock.h`, ranked below both windows), so TIMELINE works on its own. The dock is then
the standalone timeline: it owns the mouse (`GraphEditorExperiment_WantsCursor`), and a wheel notch
over it goes straight to it (`GraphEditorExperiment_PushWheel`), because no window is there to pass
it on. CONFIG and the Camera Editor drop any notch queued before they opened.

## The windows

`Panorama/ModuleWindowJs.h`, spliced into `ConfigHudJs.h` and `CameraEditorJs.h`.

- **One set of panels.** Each panel builds its column (ground, inspector, fades, scrollbar, RENDER /
  CUSTOMIZE PLAYER) inside `win.host`. In the workspace layout the host is the whole screen, so
  every anchor lands where it always did. In the modular layout it is the window's body.
- **Root stays full screen.** Dropdowns and popups are root children placed in screen coordinates.
  Anything the column places inside the host from a screen position (the scrollbar thumb and the
  top fade) converts through `win.hostTop()`.
- **Move** by dragging the title bar. **Close** with its ✕ (or the column's own ESC EXIT). The ✕
  is read from the cursor pipe, press and release both on it, like the drag: Panorama's
  `onactivate` never fired on it in CS2.
- **Never under a dock.** The TIMELINE dock and CS2's demo bar draw over the windows, so a title bar
  never goes below the top of whatever is docked along the bottom. Opening the dock, or making it
  taller, moves a window under it up.
- **Scale** by dragging any corner; the opposite corner stays where it is. The bottom-right handle
  is always drawn, the other three show an accent bracket under the pointer. The whole window, title
  bar included, gets bigger or smaller with its proportions locked: the layout inside never changes,
  it is only drawn at another size. A top corner stops at the header bar. The scale is shown in the title bar while it is not 100%, and
  runs from 45% to 125% (never taller than the screen under the header bar).
- It is Panorama's `ui-scale` (CS2 uses it, for example `ui-scale: 40%` on its minimised Rush
  panel), not a transform. From script it has to be given all three axes, `70% 70% 100%`: a bare
  number is rejected, and a single percentage is accepted but lays the panel out with no height
  (measured in CS2, 2026-11-07). `ui-scale` lays the panel out smaller, so `actuallayoutwidth` and
  `actualuiscale` report what is drawn and the column's hit tests keep working. If a CS2 update ever
  refuses the property, the window falls back to a `scale3d()` transform and logs
  `[window] ui-scale refused`.
- The column's width edge (`InspectorResizeJs.h`) belongs to the workspace layout and is hidden in a
  window, so the corner scale is the window's only resize. Both windows use the stored column width.
- Position and scale are saved per window as `win.config` and `win.editor`: `x_y_scale`, layout px
  and whole percent (underscores, because CS2's console tokenizer splits on commas). The save is a
  console command, which CS2 runs a frame or more later, so until the state hands the new value back
  the window ignores the stored one. Applying the stale value made a window snap back to where the
  drag began and then forward again on release. The mockup reproduces the delay with `?cmdlag=N`.
- Moving or scaling a window brings it to the front at once; a click elsewhere in it does so on
  release.
- A window opens at the left (CONFIG) or right (Camera Editor) edge under the header bar, tall
  enough to stay above CS2's demo bar at 100%.
- The window you pressed last draws on top (`ConfigHudRoot` and `CamEditorRoot` move to z54;
  otherwise CONFIG sits at z52 under the Camera Editor's z53). Where the windows overlap, only the
  one drawn on top takes a press: its title bar, corners, close button and the raise on release
  (`mvmWinTopAt` in `ModuleWindowJs.h`). The one underneath ignores it.

The Render workspace keeps the docked layout: it is a full-screen job with its own preview of the
final frame. CONFIG's window steps aside while it is up.

## Input

`Panorama/UiLayout.{h,cpp}` holds the layout flag and where each window and the header bar are, in
client px. The scripts publish `winrect` / `winfront`; the HUD pumps hand them over on the main
thread, and `MovieMode` reads them on the input thread.

- There is no scaled preview, so the game is the whole window. `UiClick::Input::onPanel`
  (`Movie/UiClickMath.h`) marks a point on a window, the header bar, CS2's demo bar or the TIMELINE
  dock. A click on one of those takes the UI mouse; a click on the game around them gives the mouse
  to the camera, as a click in the preview does in the workspace layout.
- A wheel notch over a window scrolls that window (`PushWorkspaceWheel`), over the dock it goes to
  the dock, and over the game it steps CS2's camera.
- Opening one window does not close the other, and neither hides the director card (there is no
  preview for it to sit in). The card follows the layout while a window is open: switching to
  WORKSPACE hides it, switching back restores it (`ReconcileMovieHud` in `ConfigHud.cpp` and
  `CameraEditorHud.cpp`). The live sidebar and the Render workspace still hide it. Each panel
  holds its own bit in `MovieHud::HideFor`, so the card comes back as it was once neither hides
  it, whatever order they close in. Closing the Camera Editor while CONFIG stays open still hands
  the camera back and stops a Follow preview.
- CONFIG's CUSTOMIZE PLAYER opens the Camera Editor's own modal while that window is open.
- CONFIG's ENVIRONMENT tab edits the game view as it does when docked: a left click on the game picks
  and drags scene objects and a held right click looks around, with the Camera Editor's window
  open too (in the docked layout the editor covers CONFIG, so there the editor being open still
  ends it). The gizmos treat the whole screen as the view and stop catching clicks where
  `onPanel` (CONFIG's state, `MovieMode::PointOnPanel`) says the cursor is on a window, the header
  bar or a dock.

## Docks

The TIMELINE dock and CS2's demo bar span the whole width in the modular layout (`st.modular` in
`GraphEditorJs.h` and `CameraTimelineJs.h`); nothing is docked beside them. The windows sit under
the dock (z52-54 against z65). There is no ribbon above the TIMELINE dock here, so the click router
measures only its body (`DockBandHeight` in `Movie/MovieMode.cpp`): the strip of game just above the
dock stays the game's.

**The TIMELINE dock's height** is the operator's, in both layouts. Drag the grab strip along its top
edge down to make it shorter (more of the game shows above it) and back up to its full 440, which is
also the most it can be; 220 is the least. Only the rows give up height: the toolbar, ruler and
footer keep theirs. The height is the `timeline.dockH` UI key; `Movie/TimelineDock.h` mirrors it
once a frame for every reader (the click router, `TimelineDock_Height()` in the panels' `dockH`),
and `$.MvmDockBodyH` carries the live height during a drag so the workspace preview, the ribbon and
the windows follow the edge before the stored value catches up. From the console:
`mirv_filmmaker config eval $.GraphExp.dockHeight(300)`.

## Checks

- Browser: `node UiMockup/capture-mock.mjs modular modular_scaled modular_timeline modular_short_dock modular_bar_only modular_settings`.
  The mockup emulates `ui-scale` (`shim/panorama.js`), the header surface and the modular
  workspace rules (`app.js`, `shim/commands.js`).
- Native: `ctest --test-dir build/x64-source2-release -C Release -R UiClickMathTests`.
- In game: switch layouts, open both windows, move and scale them, click and scroll inside each and
  on the game between them, toggle TIMELINE with no window open, open Render and Customize.
