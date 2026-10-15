# In-game and native-menu UI mockup

A browser preview of Source MVM's in-game Panorama UI and the CS2 Workshop / Downloaded menu
surfaces, for inspecting and clicking through them without relaunching CS2. The in-game surfaces
run the scripts extracted from `AfxHookSource2/Filmmaker/Panorama/` against the Panorama shim. The
menu surfaces keep the shared extracted scripts. `shim/native-menu.js` rebuilds the engine-owned
menu shell and loads local native XML layouts, and its scoped adapter CSS comes from the native
style references. This boundary is deliberate. It makes menu interaction reviewable without
claiming one-to-one engine parity.

The preview launcher refreshes shared source edits on save. UI fixes that belong in both the browser
and the game go in the shared `*Js.h` headers; adapter CSS changes affect the browser preview only.
See the [mockup-first workflow](../Docs/panorama/mockup-first-workflow.md) for getting a reviewed
change into the game.

## Running it

Double-click `cs2-mockup.bat`, or run:

```bash
node UiMockup/fetch-assets.mjs      # CS2 icons/backgrounds out of pak01 (once)
node Automation/ui/preview.mjs cs2 --open
```

The Node server watches the shared source, extracts it, and refreshes command bindings automatically.
Pending generated-JS drafts are preserved. If using a different static server, run `extract-ui.mjs`
and `derive-bindings.mjs` yourself after source edits.

```bash
node UiMockup/build-check.mjs confighud cameraeditor
```

Runs a surface against a throwaway Panorama stub and renders it, so a ReferenceError shows up in
the terminal instead of as a half-built panel. A script that dies partway through its build still
draws the controls it managed to create. In game, that looks like "the UI is just ugly" rather
than an error. A syntax check cannot catch it, because the file parses fine.

Then open <http://localhost:8823>. It must be served over HTTP, because the surfaces are `fetch`ed.
Opening `index.html` from disk will not work.

## What is in it

The UI opens the way the game does after a demo loads. None of our panels are up yet: CS2's own
demo bar, with MOUSE / CAM EDITOR / CONFIG / LOOP docked onto it, and the director card. From there
you use the UI as in game. CAM EDITOR opens the Camera Editor, its REGULAR TIMELINE / CAMERA /
GRAPH tabs swap the bottom band, RENDER opens the render workspace and its RANGE / CAMERA / HUD /
OUTPUT tabs switch, CONFIG opens Config, ESC EXIT drops back to playback, and F8 blacks the screen
out. The left sidebar is not needed for any of that.

The page is one persistent Panorama context, run the way `Filmmaker::RunMainThreadFrame` runs it.
The always-alive HUDs (MovieHud, MarkerHud, CameraTimeline, DemoBarButtons, AimCrosshair, FxDebug)
are evaluated once, in the engine's pump order, and rendered every frame. The three workspaces
(ConfigHud, CameraEditor, GraphEditor) are built when their flag turns on and torn down when it
turns off: `BuildIfNeeded()` / `Teardown()`, with the same delete-by-id list the C++ runs, read
out of each driver's `Teardown()` by the extractor. A closed workspace is removed. The
dock (`Movie/TimelineDock.cpp`) is claimed, released and applied per frame by whichever workspace
hosts it, so the tab is remembered across closes. Config and the Camera Editor both re-claim every
frame, so the dock is hosted whether Config was opened from playback or from the editor.

Render Workspace, Key Editor and Customize Player are views of the Camera Editor script, which is
where they are built in game:

| Surface | What it covers |
|---|---|
| Config Panel | CAMERA / EFFECTS / RAGDOLLS, all four EFFECTS sub-tabs, every slider, the RESET footer |
| Camera Editor | Workspace chrome, Path + Follow inspectors, gear settings menu |
| Render Workspace | Range & Camera / Passes / Output tabs, START RENDER, running-job state |
| Key Editor | Selected camera key: SELECTED CAMERA / TRANSFORM / LENS / PATH / SHORTCUTS |
| Customize Player | Agent, weapon skin, knife and glove pickers for the selected player |
| Camera Timeline | The editor with its CAMERA tab up: transport, markers, render-range overlay, shake menu, speed scrubber; playback Loop controls stay on the Regular Timeline |
| Graph Editor | The editor with its GRAPH tab up: per-channel curves (x/y/z/pitch/yaw/roll/fov), undo/redo, delete key |
| Marker HUD | Camera-path marker card and prompt banners |
| Movie HUD | Reposition / edit-mode director cards |
| Demo Bar Buttons | The controls Source MVM docks onto CS2's own demo bar |
| Aim Crosshair / FX Debug | The smaller overlays |

The two menu-context surfaces (the main-menu Demos tab and the Video-settings resolution dialog)
are a different Panorama context in game, so the **Context** rows at the top of the sidebar swap
the whole stage to them and back. They dock into native main-menu panels that this harness does
not stand up, so they build very little here.

## Hotkeys

The director keys work in the browser, mirroring `MovieMode::OnKey` (a hand-written table in
`app.js`, each line citing the branch it follows): **F8** blackout, **Esc** closes the marker card
/ the render workspace / the editor / Config in that order, **Space** plays the path when a path
editor is up and pauses the demo otherwise, **Left / Right** skip 15 s (a scrub inside the path
range when the camera timeline is up), **G** toggles the UI cursor, **K** places a marker, **L**
deletes the selected one, **J** arms / plays / stops the path preview, **Ctrl+Z / Ctrl+Shift+Z**
undo / redo on whichever curve surface is open.

## Popups, dialogs and transient states

This is what the sidebar is for: the things you cannot reach by clicking. Two kinds:

**Popups / overlays** first go where the popup lives, by issuing the same commands the UI's own
buttons send (`editor on`, `config on`, `dock camera`), then find the real control by its label
and fire its own handler, so the actual code path runs and no panel is forced visible:

| Surface | Reaches |
|---|---|
| Camera Editor | Customize Player modal · agent picker · skin picker · gear (Editor Settings) menu · **Clear All Camera Paths?** confirm · **Reset to default?** confirm · Render workspace · attachment menu · Path/Follow inspectors |
| Config | Reset footer arm, then **CONFIRM — RESET CAMERA** · **Per-player death animations** picker · **Death clip preview** (picker with the first clip playing) |
| Camera Timeline | Per-keyframe **Camera Shake** menu · Clear path · the floating timeline bar with no editor (`camtl open`) |
| HUD | Director card (`hud toggle`) · **Blackout (F8)** · aim crosshair · FX debug squares |
| Menu context | Demos tab · Custom Resolution dialog |

**States** patch the state feed the way the C++ side would, for overlays that are a reported state
rather than a click. They stack, so pressing two holds both. Each is a toggle: press it again
to undo just that one, or **Reset states** to undo all of them.

| Surface | States |
|---|---|
| Marker HUD | "Need at least 2 camera markers to play path." notice · path-cleared notice · **REPOSITIONING CAMERA #N** banner · the editing card (F / ESC Done) · preview-armed |
| Camera Editor | Render job RUNNING · idle · capture-chrome-hidden · no POV (free roam) · Mouse: GAME |
| Config | Effects still loading · Effects master OFF · live map (not a demo) · Mouse: GAME |
| Camera Timeline | Loop off · no camera keys · path playing live |
| Movie HUD | editing · reposition · hidden |

**Jump to** (collapsed at the bottom) keeps the older per-surface rows. Each one is a place reached
the way a person reaches it. `render` issues `editor on; render open`, `grapheditor` issues
`editor on; dock graph`, and the highlight follows wherever the UI's own buttons take you. The
headless scripts still address these ids through `?surface=`.

## Using it

- Navigation matches the game. The demo bar's CAM EDITOR / CONFIG open the workspaces, the
  editor's REGULAR TIMELINE / CAMERA / GRAPH tabs swap the bottom band, RENDER opens the render
  workspace and its tabs switch, and Config's RENDER / CUSTOMIZE PLAYER hand over to the Camera
  Editor (Config owns neither). ESC EXIT drops back to playback, and the speed dial drags. Every
  one of those is a console command through the same router. The model decides what is built,
  and the sidebar highlight follows.
- Play runs the fake demo clock, so the timeline scrubs and the loop range wraps.
- Engine quirks are reproduced. Config opened from the editor has no dock (its ribbon tabs move
  the highlight and dock nothing). The director card a workspace hid is restored only by the
  workspace that hid it, so editor -> Config -> exit leaves it hidden until `hud toggle` or F8.
- Fit, 100% and 200%: 100% is a true 1:1 scale against a 1920x1080 game. Use it for a
  like-for-like comparison with a screenshot from the game.
- The Dev panel shows `$.Msg` output, every console command the UI issued, and a "Gaps" tab that
  lists anything the shim did not map.
- Controls are live: clicking a mode or dragging a slider issues the same console command the game
  would, the router writes it into the fake state, and the next frame renders the result.

## How it fits together

```
AfxHookSource2/Filmmaker/Panorama/*Js.h
        |  extract-ui.mjs        (raw-string bodies + #include splicing, exactly as C++ concatenates them)
        v
generated/<surface>.js  ---- run unmodified ---->  shim/panorama.js   (Panorama -> DOM)
                                                   shim/state.js      (the per-frame state feed)
                                                   shim/commands.js   (console command -> state)
                                                   generated/bindings.json
```

`derive-bindings.mjs` reads the widget declarations out of the extracted code. A `sliderRow` knows
both the command it sends and, at its `.sync()` call, the state field it reads back. The command
and state wiring is therefore generated. A rename in the UI source shows up
as a dropped binding in that script's report instead of a mockup that silently lies.

## Fidelity notes

- Layout is emulated on two rules: a panel with no `flowChildren` stacks its children at its origin
  and sizes to the largest (CSS grid, one cell), and a flowing panel never shrinks its children
  (flex with `flex-shrink: 0`).
- Fonts are the real Stratum2 faces; icons, the dotted panel background and the fade masks are the
  real CS2 assets pulled from `pak01_dir.vpk`.
- The animated panel surfaces play Valve's own `linemap.webm`. CSS cannot play a video as a
  background image, so those panels get a real `<video>` layer underneath their content. The file
  is a loose asset in the game folder rather than a VPK entry, so `fetch-assets.mjs` copies it.
- The native CS2 demo playback bar is rebuilt from the decompiled
  `reference/cs2 refs/layout/hud/huddemocontroller.xml` and its stylesheet, because several Source
  MVM surfaces dock into it by panel id and would otherwise render nothing.
- The game render behind the UI follows the published `previewrect`, the same rect the engine's
  pre-UI blit scales the world into, so the plate rescales when the layout does and everything
  around it is black, as on screen. Drop a `backdrop.jpg` next to `index.html` for a real capture;
  without one it is a gradient.
- The 3D character previews (Customize Player, and the per-player death animation picker under
  RAGDOLLS > Advanced Ragdolls > PER-PLAYER ANIMATIONS) are CS2's native MapPlayerPreviewPanel in
  game. `shim/preview3d.js` stands in for it with three.js: the agent is the game's own model,
  exported from pak01 by the preview server the first time it is shown
  (`assets/models/agents/models/<agent>/<agent>.glb`, `lib/vrf.mjs`), and a death clip is the
  game's own baked clip library (`assets/ragdoll/ragdoll_animations.mvmanim`) posed by a line-for-line port of
  `Panorama/DeathAnimPreview.cpp`: placement against the idle pose, pelvis drift removed, every
  undriven bone following its driven ancestor, a 0.8 s hold before the loop. The camera framing is
  measured off game captures of both pickers. Picking an agent swaps the model; `deathanim view`
  plays the clip on every preview, as in game. Not reproduced: the vanity scene's lighting and
  animgraph idle (the agent stands in the first frame of a standing clip instead), the held weapon,
  and the limb clearance track. three.js loads from jsDelivr (pinned in `index.html`); offline, or
  on a static server that cannot export models, the agent's inventory portrait stands in.
- Customize Player runs the real catalogue. The extractor splices
  `CameraEditorCosmeticsCatalog.inc` the way the compiler does, so every agent, finish, knife and
  glove the game lists is listed here, with its inventory thumbnail. Apply writes the override into
  the reported loadout (`customizeTarget` / `customizePlayers`), so reopening the modal shows it, and
  Reset to default brings the recorded loadout back.
- Per-player death animations load the 449-clip catalogue from the same packs
  (`deathanim catalog`), list the demo's players (`daPlayers`, derived from the reported roster), and
  Assign / Use automatic pick write each player's clip.

## Changing the UI here and carrying it into the mod

The generated JS is the game's UI code, pulled out of the headers with a marker on every C++ literal
it came from. So there are two ways to make a change here that reaches the game, and both have
tooling.

To edit the script, open `generated/<surface>.js` in any editor and change it. The page polls that
file and reloads the surface within ~1.5s of a save, so you see the result against the real state
and the real backdrop. When it is right:

```
node UiMockup/apply-ui.mjs cameraeditor     # or no argument for every surface
```

That writes each edited literal back into the header it came from: `CameraEditorJs.h` and any
fragment it `#include`s. It changes nothing else. Every byte of C++ around the literals is
preserved, and so are each header's own line endings (the tree mixes CRLF and LF, sometimes inside
one file). `--dry-run` reports what would change. `--check` exits non-zero if generated/ and the
headers disagree. Saved source hashes also reject stale drafts when a header changed without
changing its literal count. All headers are validated before writing, and changed markers, invalid
literal endings and oversized literals are rejected. `Automation/tests/ui-mockup-roundtrip-tests.mjs`
checks that extract and apply round-trip byte for byte for every surface.

Then run the fast hook-only rebuild and install from CLAUDE.md, and relaunch CS2. Applying a draft
changes the source, not the DLL already loaded in the game. `livejs` can run temporary scripts but
does not install source edits automatically.

To edit in devtools, tweak a panel in the Elements panel: a width, a colour, a label's text, or
hide it. Then open **Dev > Edits**. It lists every panel you touched, the Panorama line that
produces the same result (`.style.width = '460px'`, `.text = '...'`, `.visible = false`), and a
`rg` that finds the panel in `AfxHookSource2/Filmmaker/Panorama`. It knows the difference between
your edit and what the script wrote because the shim snapshots the style after every write the
script makes; only hand changes show. `window.MvmCompare.styleDiff()` returns the same thing as
data. Placement properties (flex/grid) are the shim's own layout and are flagged rather than
translated. In Panorama those are `horizontalAlign` / `verticalAlign` / `flowChildren`.

## Checking it against the real game

The mockup is checked two ways: against a running CS2, and against itself. Everything here is
scripted, so a change can be re-verified rather than re-eyeballed.

Screenshots come from `node UiMockup/capture-mock.mjs`, which renders every view at a true 1920x1080
through headless Chrome into `Automation/output/ui-compare/mock/`. Game-side captures go beside it in
`.../game/` via `Automation/capture/capture-game-window.ps1`. That script uses PrintWindow, so the
grab is correct even with another window on top.

Screenshots show that something is off, but not what. So the game reports its own numbers:

```
mirv_filmmaker livejs context hud
mirv_filmmaker livejs load <abs>/Automation/livejs/dump-ui-geometry.js
```

That walks every SOURCE:MVM panel and prints `depth id type x y w h visible text` in design px.

`node UiMockup/verify-flow.mjs` drives the mockup headless the way a person would: tab clicks, the
hand-overs between panels, a drag on the speed dial, stacked states, the closed boot, the ribbon's
`dock` verb, `render open` / `render tab`, `editor close`, both halves of the dock quirk, F8, and
whether a closed workspace is really torn down. It asserts what the game shows for each step. It
gates any change to the shim, the router or the workspace pump.

Three more headless scripts ask questions a screenshot cannot. Each one runs over a list of states
rather than a list of panels:

| Script | Asks | Fault it exists to catch |
|---|---|---|
| `verify-clicks.mjs` | can a control be reached by the mouse, and does it fire only its own handler? | a panel stacked on top of a control, or a click that bubbles into a second handler |
| `verify-layers.mjs` | who actually PAINTS here? | a panel that is perfectly clickable and completely covered: `pointer-events: none` is invisible to hit testing yet still paints |
| `verify-bounds.mjs` | does every root stay on the screen? | a root enlarged by its children past 1920, which then feeds its own width back into the layout |

Both the click and the layer audits cover popups as well as the resting surfaces, because every
destructive button in this tool sits behind one: **Clear All Camera Paths?**, **CONFIRM — RESET
CAMERA**, **Reset to default**.

A modal takes all input, so "covered by a modal" is the modal working, not a fault. That is why
`customize` could not be audited for clicks until the audit learned to tell the two apart. It ranks
roots by the z-layer map in `AfxHookSource2/Filmmaker/Panorama/PanoramaBridge.h`, so a modal covering
the editor passes. A real fault still fails: something painting over the modal's own controls, which
is exactly what the graph editor did to the Customize modal.

Each popup case also names the root it opens and the labels it expects, because a popup that
silently stopped opening would otherwise pass by having nothing left to check. `MVM_LABELS=1`
prints every reachable control per case, which is how to write the `expect` list for a new one.

`node UiMockup/compare-geometry.mjs <captured console output>` renders the same surfaces headless,
walks them the same way, and diffs the two trees position by position. Both sides run the SAME panel
script, so the trees have to agree panel for panel. A **structural divergence** (a panel present in
one and not the other) is reported first and stops the rect comparison, because everything after it
is misaligned anyway. `--surface <id>`, `--tol <px>` and `--at <index>` (prints both trees side by
side around one index) narrow it down.

A comparison against a hand-written state feed mostly measures the stand-in, not the UI. So the real
feed is captured too:

```
mirv_filmmaker livejs load <abs>/Automation/livejs/dump-ui-state.js
node UiMockup/import-state.mjs <captured console output>
```

That writes `generated/state-snapshot.json`, which the mockup loads automatically. The browser then
shows a real moment out of a real demo, with the real camera keys, slider values and player loadout.
`?live=0` in the URL falls back to the built-in stand-in. Every root is captured and merged, because
they do not share a feed (the Camera Timeline's is the only one carrying `markers`).

### Current results

Measured against a paused demo at 1920x1080, editor open, Regular Timeline mode:

| Root | Panels (game / mockup) | Over 3px |
|---|---|---|
| CamEditorRoot | 659 / 659 | 22 |
| CamEditorRenderRoot | 282 / 282 | 7 |
| CamEditorConfirmRoot | 9 / 9 | 0 |
| CamEditorDebugRoot | 2 / 2 | 0 |
| CamTimelineRoot | 117 / 117 | 42 |
| CamTimelineRangeRoot | 9 / 9 | 7 |
| MarkerHudRoot | 90 / 90 | 0 |
| MovieHudRoot | 96 / 96 | 74 |

There are zero structural divergences. Every root builds the same tree, with the same panel count
and the same label text as the game.

The remaining differences, in order of how often they turn up:

- Text metrics: Chrome measures a Stratum2 string a few px wider than Panorama does (~0.4px per
  character, consistent with Panorama flooring each glyph advance). Label heights are matched.
  `line-height: 1.19` is measured off the game's own reported heights.
- Reported state missing from the feed: MovieHudRoot's card and CamTimelineRoot's speed-scrubber
  ticks are laid out in the game and collapsed here (or the reverse). Their visibility comes from
  engine state that the capture happened to take at a different moment.
- One deliberate divergence: Panorama's `Slider` builds its own SliderTrack/Thumb children, while the
  shim uses one styled range input. `compare-geometry.mjs` collapses a Slider to a leaf on both
  sides so it does not derail the walk.

### Not reproducible here

- The engine-rendered parts of the 3D character previews: scene lighting, the animgraph idle, the
  held weapon and its paint (see the 3D preview note under Fidelity notes for what is reproduced).
- The world's content. `assets/backdrop.jpg` is one still, scaled into the preview rect the way the
  blit scales the live render, so the framing tracks the layout but the picture never moves or
  re-frames with the camera. Re-capture the plate any time with a clean frame (no SOURCE:MVM UI)
  cropped to 1920x990. The bottom 90px is left off because the mockup draws the demo bar itself.
- The native bar's round number. CS2 feeds its own bar from the demo's interval list, which never
  reaches the Panorama state feed. The mockup derives a plausible number from the playhead instead.

## Regenerating

`generated/` and `assets/` are derived and git-ignored. Re-run the three scripts after changing any
Panorama source. `fetch-assets.mjs` needs the Source2Viewer CLI (a `ValveResourceFormat` checkout
next to this repository) and a CS2 install found through Steam; override the paths with the
`SOURCEMVM_VRF_CLI` and `CS2_PAK` environment variables. Besides the UI artwork it extracts
every inventory thumbnail the cosmetics catalogue references and copies the death animation packs;
agent models are not pre-extracted (about 20 MB each with textures). The preview server exports
each one on first use with the same CLI and caches it under `assets/models/`.

## Native CS2 menu surfaces

The menu review covers Workshop Maps (`surface=workshop`) and Downloaded demos
(`surface=filmmakergui`). The shell is rebuilt in `shim/native-menu.js`. `prepare-menu.mjs` prepares
the native XML bundle from `reference/cs2 refs/layout`, and the adapter uses manually scoped CSS
from `reference/cs2 refs/styles`. `WorkshopFilterJs.h`, `WorkshopKeywordEditorJs.h`, and
`FilmmakerGuiJs.h` remain extracted and executed unmodified. The browser therefore runs the same
filtering, keyword editing, demo selection, player selection, and organizer code paths as the
game-facing scripts.
The Workshop manifest uses `sourceDir: ../PlayMenu` and combines the three PlayMenu headers;
`node UiMockup/apply-ui.mjs workshop` round-trips edits back to those headers with their original
line endings.

Set up or refresh the local assets and extracted bundle with:

```powershell
node UiMockup/fetch-assets.mjs
node UiMockup/extract-ui.mjs
node Automation/ui/preview.mjs cs2 --port 8833 --open
```

This review uses the temporary server override `8833`; the documented default remains `8823`
(`node Automation/ui/preview.mjs cs2 --port 8823 --open`). For a browser comparison at a native
menu viewport, use `bare=1`, disable source watching with `watch=0`, and choose `1440`, `1728`, or
`1920` design width. `zoom=fit` shows the complete stage; use matching capture dimensions for
one-to-one screenshots:

```text
http://127.0.0.1:8833/index.html?bare=1&watch=0&surface=workshop&aspect=1440&zoom=fit
http://127.0.0.1:8833/index.html?bare=1&watch=0&surface=filmmakergui&aspect=1920&zoom=fit
```

For automated captures, keep the browser and game dimensions identical, for example
`node UiMockup/capture-mock.mjs --width 1440 --height 1080 workshop_menu downloaded_menu`.

The Workshop surface supports sorting, the hide-training option, editable keywords, map selection,
and the native mode popup from GO. Downloaded supports demo and player selection, folder grouping,
rename, watch activity, and the hide/clear confirmation dialogs. The backend command shim simulates
sorting, `ws hide`, keyword edits, demo/player selection, folder creation/collapse/rename/delete and
assignment, rename, watch, hide/clear confirmations, and mode-popup choices. Rescan/refresh,
playback, and external Steam/browser links report their status only; no disk, Steam, or playback
operation is performed. `Reset menu fixtures` in the sidebar (or `window.MvmNativeMenu.reset()`)
restores the loaded fixture.

### Fixtures and native captures

Keep CS2 open with the native main-menu context and UI on screen. From a netcon client, load
`Automation/livejs/dump-menu-fixture.js` into that menu context; the script only reads panel
attributes and emits chunked JSON. Save the netcon response, then import it:

```powershell
powershell.exe -File Automation\netcon\cs2-netcon.ps1 `
  -CommandBatch 'context menu;;mirv_filmmaker livejs load "<repo>\Automation\livejs\dump-menu-fixture.js"' `
  -LogPath 'C:\temp\menu-capture.log'
node UiMockup/import-menu-fixture.mjs 'C:\temp\menu-capture.log'  # add --no-thumbnails when offline
```

The importer strips install and workshop paths and writes the local generated fixture. Private
generated captures are ignored; `fixtures/menu-default.json` is the committed anonymized sample.
Only the selected Anubis demo carries a complete `perRound` capture for all 10 players. Other
round data is intentionally omitted. Optional cached Steam thumbnails can be materialized with
`UiMockup/cache-menu-thumbnails.ps1`.

For captured menu artwork, pass a JSON crop spec to `UiMockup/import-menu-artwork.py`. It writes
PNG metadata identifying the local source and crop in `assets/menu/`; the spec must use local files
and explicit rectangles. The captured CS2 background and portraits are frozen in the local fixture.

### Comparison status

Exact `Stratum2Condensed` Regular/Medium/Bold (including the 700 binding), `Stratum2`
Regular/Medium/Bold, and Regular/Bold Monodigit faces, plus the shipped CS2 SVG symbols, are
validated against the installed game assets (matching hashes). The reviewed pass includes centered
Downloaded `MatchMap` / `MatchInfo` cards and the native round chart's 2px baseline with 18px glyphs.
The reviewer disposition is **ship**: the listed fixes are resolved at 1280×960. Browser font
antialiasing, the animated 3D scene, and the frozen background naturally differ; the preview is an
interaction and comparison aid. It does not claim zero drift or full pixel-perfect engine parity.

Current checks are reproducible with `node UiMockup/verify-flow.mjs "menu:" --default-menu`
(menu flow, **7 passed**), `node --test Automation/tests/ui-mockup-roundtrip-tests.mjs`
(round-trip, **4 passed**), and `node --test Automation/tests/ui-mockup-menu-fixture-tests.mjs`
(importer/parser, **5 passed**).
The existing three browser-flow cases remain failed with the adapter disabled: SmoothView Tilt and
Effects clicks are blocked, and the Render HUD case cannot find its expected label. Separately, five
clock unit tests fail because their standalone harness does not load the pre-existing `MvmSceneKeys`
dependency. Do not report either baseline as a full pass.

The hook-only CMake build keeps CS2 open while testing the shared header path. `build.bat` closes the
game and should be avoided for this iteration. Menu backend commands are simulated against the
fixture, so use the live game only for read-only capture and geometry comparison.
