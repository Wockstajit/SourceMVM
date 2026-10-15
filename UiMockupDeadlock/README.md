# Deadlock in-game UI mockup

A browser copy of SOURCE:MVM's Deadlock workspace, for looking at and clicking through it without
launching the game.

This mockup runs the real UI code. The script is extracted straight from the C++ macros in
`AfxHookSource2/Filmmaker/Deadlock/` and runs unmodified against a Panorama emulation layer.
Nothing here is hand-drawn, so it cannot drift from the game the way a static mockup would.

This is the Deadlock sibling of [`../UiMockup`](../UiMockup/README.md), which does the same job for
CS2. Each has its own extraction model, UI shim and state feed. They share file-safety and local
preview-server tooling, but the game UI implementations stay separate. Run both at once on
different ports. See the [mockup-first editing workflow](../Docs/panorama/mockup-first-workflow.md).

## Running it

Double-click **`deadlock-mockup.bat`**, or run:

```bash
node UiMockupDeadlock/fetch-assets.mjs           # Deadlock's fonts and icons (once)
node Automation/ui/preview.mjs deadlock --open
```

Then open <http://localhost:8824>. It must be served over HTTP, because the script is `fetch`ed.
Opening `index.html` from disk will not work.

The Node preview server refreshes shared source edits automatically while it is open. It keeps
pending generated-JS drafts instead of overwriting them on refresh. If you use a different static
server, run `extract-deadlock-ui.mjs` yourself after source edits.

## How Deadlock differs from CS2, and why that shaped this harness

Three differences decided almost everything here.

**There is no root header.** A CS2 surface is a `*Js.h` that `#include`s its fragments, so the
compiler concatenates them and the extraction unit is the file. Deadlock has no such header.
`DeadlockWorkspaces.cpp` pushes twelve named macros through `RunScript`, one after another, into a
single shared JavaScript scope. So the unit here is the `#define`. A file can hold several
(`DeadlockConfigRenderBodyJs.h` holds two), and the order comes from the C++.
`lib/fragments.mjs` reads the `RunScript` calls rather than keeping a list. The order matters. The
style fragment defines helpers that the rest call. The drawing primitives must exist before the
timeline and graph that draw with them. The three docked editors must register before the shell,
whose `build()` asks each of them to build into the dock body.

**There are no engine bindings.** Measured in game on build 6687 by enumerating the global object
from a live `RunScript`: there is no `GameInterfaceAPI` and no `UiToolkitAPI`. Deadlock's own
`CitadelConCommand` is bound per XML context by the panel class that declares it, and it is
undefined here. So `shim/panorama.js` **deletes those globals on purpose**, and a test asserts it. A
shim that provided them would let a panel reach for one, work perfectly in the browser, and be a
dead control in game. That is the most expensive mistake this harness exists to prevent.

A command therefore travels as a **marked console line**. The panel prints `\x02MVMCMD <command>`
through `$.Msg`, and a tier0 logging listener in `DeadlockUiCommandBridge.cpp` recognises the marker
and runs the rest on the main thread. The shim splits the same marker off the same stream, so the
browser's command path is the game's command path.

**State arrives as a call argument.** CS2's panels read a JSON string off a panel attribute.
Deadlock's C++ composes `$.MvmWorkspace.render({...})` and runs it. So there is no attribute feed to
emulate. `app.js` calls `render()` with the state object.

## What is in it

The UI opens the way the game does after a replay loads. None of our panels are open yet. Deadlock's
own replay bar carries our **Cam Editor / Config / Render / Loop** buttons, along with the director
card. From there you use the UI as in game. A bar button opens a workspace. The dock ribbon's
REGULAR TIMELINE / CAMERA / GRAPH tabs swap the bottom band, and the Regular Timeline's own buttons
swap workspaces. Render's × Back and ESC EXIT close them, and F8 blacks the screen out. The
sidebar's highlight follows wherever the UI takes you. You do not need the sidebar to get anywhere.

The game has one surface, the workspace. It has two independent axes. The **Jump to** rows
(collapsed at the bottom of the sidebar) show the combinations worth looking at:

| Axis | Values | Set by |
| --- | --- | --- |
| Which inspector is open (right column) | Camera Editor · Config · Render · closed | `mirv_deadlock workspace …` |
| Which editor is docked (bottom band) | Regular Timeline · Camera · Graph | `mirv_deadlock dock …` |

Conflating those two would put the whole timeline down the right-hand side, so the
picker offers both rather than pretending each combination is a separate panel.

**The Regular Timeline is Deadlock's own replay bar, hosted.** Our code finds `#hud_replay`, removes
the classes that hide it, resizes it to the dock's width, and empties the dead space on its left. It
hides the game's speed stepper and puts the CS2-style speed dial in its place. It adds the
**Cam Editor / Config** swap button and **Loop**, and it restores all of that on teardown.
`shim/native-hud.js` stands up the game's bar from its own decompiled files
(`reference/deadlock/panorama/layout/replay/…` and the matching stylesheets). Every id, class and
metric is copied, not invented. Without it, the most-edited surface in the tool would be the one the
mockup could not show.

Loop lives on the Regular Timeline and nowhere else. Looping is a property of playback, so it
belongs on the playback bar. The camera timeline is about the shot, and the graph is about the
curves. Two views that can both *write* the loop can disagree about it. You find out by setting a
loop in one and watching playback ignore it.

## Hotkeys

The hook's own key table is read out of `AfxHookSource2/Deadlock/DeadlockHotkeys.cpp` by the
extractor (`lib/hotkeys.mjs`, into `manifest.hotkeys`). The browser runs its keyboard input through
the same gate that `Hotkeys_RunMainThreadFrame` uses. The page must have focus and a replay must be
loaded. SPACE and the arrows also need the workspace open, because they collide with Deadlock's own
spectator HUD otherwise. The arrows stand down while the free cam has input, and a key cannot fire
twice inside the 250 ms bounce window. Today that means **K** places a marker, **F** / **L** select
and delete the aimed-at marker, **Space** plays and pauses, **Left / Right** skip 10 s, and **F8**
blacks out. Dev › Hotkeys lists the parsed table and every press. Digits, Backspace, Enter and
Escape also reach the value tiles as the `numberKeys` bitmask while the UI owns the mouse, exactly as
the C++ polls them.

## Popups, overlays and reported states

The sidebar is for the things you cannot reach by clicking. **Popups and overlays** go where the
popup lives, by issuing the same commands the UI's own buttons send. Then they reach the real
control. That covers the **Clear all** confirm, a dropdown, the per-marker **shake menu** (a
right-click on a timeline diamond), the graph's **Ease / Align / Delete** menu, the director card
(close the workspace), and F8. In game, C++ opens the graph menu from the cursor probe and publishes
it in the graph body's local space. Here the same `graph.menu` field is fed from the browser's
right-click. The ribbon's gear is deliberately absent. In game, `dock settings` prints
`Dock_Report()` to the console and opens nothing.

**States** patch the model the way the C++ would report it: loop off, no markers, path playing,
render capturing, HUD hidden, mouse in GAME, no replay, and free cam owning input. Each one is a
toggle. **Reset states** undoes all of them.

`?act=A|B` presses sidebar buttons, and `?cmd=a;b` sends commands after the view is up, so
`capture-views.mjs` can photograph a popup.

## Changing the UI here and carrying it into the mod

The generated JS is the game's UI code, with a marker on every C++ literal it came from.

To make a change, edit `generated/workspace.js` in any editor. The page polls it and reloads within
~1.5s of a save, so you see the result against the real state. When it is right:

```bash
node UiMockupDeadlock/apply-deadlock-ui.mjs
```

That writes each edited literal back into the macro it came from and touches nothing else. Every
byte of C++ around the literals is preserved, and so are each header's own line endings (the tree is
mixed CRLF and LF, sometimes inside one file). `--dry-run` reports what would change. `--check`
exits non-zero if `generated/` and the headers disagree. Saved source hashes reject stale drafts
even when the header has the same literal count. All headers are validated before any are written,
and source markers must stay in order.

The applier **refuses** three things, because each would produce C++ that does not compile, or a
header that silently lost code:

- a literal count that does not match (re-extract instead),
- a body containing `)CFJS"`, which would end the raw literal early and spill JS into the C++,
- a literal over **16380 bytes**, MSVC's raw-string cap. Past that limit the compiler **truncates
  without a diagnostic**. A panel that builds fine here but half-builds in game fails this way, and
  it looks like "the UI is just ugly" rather than an error. Split the macro with a
  `)CFJS" R"CFJS(` seam on a single line. A newline there would end the `#define`.

Then run the fast hook-only rebuild and relaunch. Applying a draft updates the source, not the DLL
already loaded in the game. `livejs` can run temporary scripts, and `ui rebuild` recreates the
compiled workspace. Neither installs source edits automatically.

## Checking it against the real game

To check every view headless:

```bash
node UiMockupDeadlock/capture-views.mjs
```

It screenshots each view at a true 1920x1080 into `Automation/output/deadlock-ui/mock/`, writes a
geometry dump beside each one, and **exits non-zero if any view threw**. It runs in a real browser,
which is what it adds over the Node-stub check in `Automation/tests/deadlock-panorama-build-tests.mjs`.
The stub answers "did the script finish". Only a real layout engine answers "and where did everything
end up".

It also reports **style writes per second in a steady frame, which should be zero**. That is not
housekeeping. In game, a style write re-does the panel's geometry. A 3D transform allocates a
composited layer with its own D3D11 vertex buffer, which Panorama indexes with an *unsigned short*.
So a panel restyled every frame runs through 65535 entries, and the game dies with
`CVertexBufferDx11 … EnsureCapacity allocation count overflow`, after first showing parts of the
preview frozen on an old still.

Against the running game:

```
mirv_deadlock livejs load <abs>/Automation/livejs/deadlock-dump-ui-geometry.js
```

Then save the console output and run:

```bash
node UiMockupDeadlock/compare-geometry.mjs <that file> --view camera-native
```

Both sides run the same panel script, so the trees have to agree panel for panel. A **structural
divergence** (a panel present in one and not the other) is reported first and stops the rect
comparison, because everything after it is misaligned anyway. `--at <n>` prints both trees side by
side around one index. The probe dumps the game's replay bar as well as our own panels, because
almost everything the Regular Timeline does is a change to a panel we do not own.

Offline:

```bash
node --test Automation/tests/deadlock-mockup-tests.mjs          # the harness's own edges
node --test Automation/tests/deadlock-panorama-build-tests.mjs  # the panels build and render
```

The first one keeps the mockup honest where it could drift. It covers extraction, the byte-exact
round trip, and the two places where the harness re-implements something C++ owns: the layout solver
and the dock metrics. Both are checked against the constants read back out of the C++ headers.

## Fidelity notes

- Layout is emulated on two rules: a panel with no `flowChildren` stacks its children at its origin
  and sizes to the largest (CSS grid, one cell), and a flowing panel never shrinks its children (flex
  with `flex-shrink: 0`).
- Fonts are Deadlock's real faces (Retail Demo, Retail Text Demo, Reaver, VALVEPulp), copied out of
  the install. They are loose files, so no VPK is needed. Metrics are tuned against them. A label's
  width decides whether a tab ribbon fits, and a fallback face would make the mockup disagree with
  the game in the direction that hides overflow.
- Icons are the game's own, decompiled from its VPK.
- The layout solver is a JS port of `DeadlockWorkspaceLayoutMath.h`. In game, C++ solves the layout,
  and the panel draws nothing without an answer. The port is held to the same test vectors the C++
  unit test uses, and its constants are read back out of the header.

### Not reproducible here

- The scaled world. In game the world is blitted into the preview rect. Here the backdrop sits at
  native size behind the panels, and the preview *frame* shows where it would go. Drop a real
  capture in as `assets/backdrop.jpg`.
- The replay's own marker list. The game feeds its bar from that list, and it never reaches our
  state feed. The mockup draws a plausible spread.
- Mouse-driven hit testing against the C++ cursor. The graph and timeline hit-test against a cursor
  position published from the OS side, because this Panorama context has no mouse-move event at all.
  The browser has real mouse events, so drags there follow the shim's path, not the game's.

## Regenerating

`generated/`, `assets/` and `fonts/` are derived and git-ignored. `fetch-assets.mjs` needs a Deadlock
install (found automatically, or set `SOURCEMVM_DEADLOCK_DIR`). The native bar's icons also need the
Source2Viewer CLI (`SOURCEMVM_VRF_CLI`) and `DEADLOCK_PAK`. Without them the mockup still runs. The
bar's round buttons draw as plain discs, which is obvious rather than subtly wrong.
