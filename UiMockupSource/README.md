# Counter-Strike: Source UI mockup

A browser copy of SOURCE:MVM's Counter-Strike: Source workspaces, for looking at and clicking
through them without launching the game.

It runs the real UI code. The Source 1 hook draws its workspaces natively with Dear ImGui
(`AfxHookSource/Filmmaker/Ui/`), and this mockup compiles those same C++ files to WebAssembly with
Emscripten. Only the two ends differ: WebGL draws the frame instead of D3D9, and the page forwards
DOM mouse and key events the way the hook forwards window messages. Layout, paint, which panel takes
a click, and when a right press becomes a look all come from the game's code, so the mockup cannot
drift from the game.

This is the CS:S sibling of [`../UiMockup`](../UiMockup/README.md) (CS2) and
[`../UiMockupDeadlock`](../UiMockupDeadlock/README.md). Those two run Panorama script extracted
from the C++ headers. CS:S has no Panorama, so here the C++ itself is what runs.

## Running it

Double-click **`css-mockup.bat`**, or run:

```bash
node Automation/ui/preview.mjs css --open
```

Then open <http://localhost:8825>. The first start builds the WebAssembly (a few seconds). Saving a
UI source file rebuilds it, and the page reloads itself onto the new build.

It needs Node.js and [Emscripten](https://emscripten.org/docs/getting_started/downloads.html). The
build looks for emsdk in `SOURCEMVM_EMSDK`, then `EMSDK`, then `%USERPROFILE%\emsdk`. To build
without the preview server, run `node UiMockupSource/build-mockup.mjs`. The output goes to
`build/ui-mockup-source/`.

## What is real and what stands in

| Part | In the mockup |
| --- | --- |
| Workspace layout, paint, fonts, widgets, the demo bar | The game's code (`CameraEditorShell.cpp`, `DemoBar.cpp`, `UiStyle.cpp`, `UiWidgets.cpp`, `SourceDemoBarModel.cpp`) |
| Who gets the mouse, look, G, Esc | The game's code (`WorkspaceMouse.h`, `UiKeys.h`, CS2's `UiClickMath.h`) |
| Verdana and Tahoma | Read from this machine's Windows Fonts folder at build time and packed into the build output |
| The game | A still frame, letterboxed into the preview where the game renderer puts the real frame |
| The console | The `mirv_filmmaker` box runs the mockup's `editor on`, `off`, `toggle` and `state` |
| The demo | A playback clock over the test SourceTV demo's real length, tick rate and events, with stand-in names, so the demo bar responds. `demo on`, `demo off`, `demo pause` and `demo seek <tick>` drive it |

The game frame is `assets/backdrop.png` when it exists: a capture of CS:S, which stays local
because game pixels are not ours to commit. Take one with the editor closed:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Automation\capture\capture-game-window.ps1 -Process cstrike_win64 -Out UiMockupSource\assets\backdrop.png
```

Without it, a drawn horizon stands in. While the game has the mouse, moving it turns that frame, as
the game's mouse look would.

The canvas is shown at one canvas pixel per screen pixel, so an 854 x 480 window has the same text
sharpness as the game window. On a scaled display it looks smaller than 854 CSS pixels. That is
intended.

## The mouse in a browser

The page asks for pointer lock while the game has the mouse (after a left click in the preview, or
during a right-button look) and releases it when the UI takes the mouse back. A browser keeps Esc
for itself while the pointer is locked: the first Esc releases the lock, which hands the mouse back
like G. A second Esc closes the editor, as Esc does in the game.

## Checking it

`window.mockup.state()` returns the host's state (open, mouse mode, tabs, preview rect, fonts, the
simulated demo, and where each demo bar control is), and `window.mockup.command('editor on')` runs a
command. A browser tab in the background gets about one animation frame a second, so a test script
calls `window.mockup.step(n)` to run frames itself between the events it sends. The browser
console carries the same `ui.input`, `ui.mouse` and `ui.editor` lines the game writes to its
`mvm_debug` log, so a click sequence can be compared line by line. With `preserveDrawingBuffer` on, the canvas can be read
back and compared against a capture of the game at the same window size.

`Automation/tests/source-ui-mockup-tests.mjs` checks that the mockup compiles every UI source the
hook compiles and takes its mouse and key rules from the shared headers.
