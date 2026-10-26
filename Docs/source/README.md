# SOURCE:MVM for Source 1 games

The Source 1 port of the filmmaker lives in the inherited Source 1 hook, `AfxHookSource.dll`, which
is injected into the game. The first target is Counter-Strike: Source, the 64-bit Steam build
(`cstrike_win64.exe`). Everything the port adds is under `AfxHookSource/Filmmaker/`. The inherited
HLAE files only call into it at marked `// SOURCE:MVM` lines.

| Document | What it covers |
| --- | --- |
| [port-plan.md](port-plan.md) | The plan: rules, parity with CS2, the ordered steps and their checks |

## Build

The x64 Source 1 hook is built by the legacy `x64-release` preset. Build only the hook target, then
stage it with its runtime and resources into `build/source1-release`:

```batch
cmake --preset x64-release
```

```batch
cmake --build build/x64-release --config Release --target AfxHookSource --parallel
```

```batch
cmake --install build/x64-release --config Release --prefix build/source1-release --component x64-runtime
```

```batch
cmake --install build/x64-release --config Release --prefix build/source1-release --component x64-source1
```

The stage holds `bin/x64/AfxHookSource.dll` with its runtime DLLs, and `bin/resources/` with the
shaders and the hex font the hook loads at runtime.

Code shared with the CS2 and Deadlock hooks is compiled from where it lives in the CS2 tree (for
example `AfxHookSource2/Filmmaker/Debug/MvmDebugLog.cpp`). The target defines `GAME_SOURCE1`, which
those files use to leave out their CS2-only parts, as Deadlock does with `GAME_DEADLOCK`.

## Test

```batch
ctest --test-dir build/x64-release -C Release --output-on-failure -R "NetconServerTests|Source"
```

The Source 1 tests are registered in `CMake/Source1Tests.cmake`, included from
`AfxHookSource/CMakeLists.txt`. The `x64-release` build also holds the CS2 tests, because that preset
builds both hooks; `-R` picks one suite. The injector and the launch host come from the
CS2 stage (`build.bat -NoLaunch` once) and from `SourceMovieMakerLauncher\scripts\build-host.ps1`.
Both are game-agnostic.

## Launch

```powershell
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File Automation\launch\launch-css.ps1
```

Run it as a background task and read the exit code. It starts CS:S offline (`-insecure`), windowed
at 854x480 with D3D9, moves the window to Windows DISPLAY3, and waits until the hook answers
`mirv_filmmaker health` on its command port. A hook startup error appears as a message box titled
"Error - AfxHookSource", which the script reports. `-condebug` mirrors the console into
`cstrike\console.log`.

Close the game with the console `quit` command. Never kill the process.

## Command port

The CS:S 64-bit engine has no `-netconport`, so the hook serves the same protocol itself
(`Shared/NetconServer`) when the game is started with `-mvmnetconport <port>`. Port 29030 is the
default, beside CS2's 29010 and Deadlock's 29020. Nothing listens without the flag.

```powershell
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File Automation\netcon\css-netcon.ps1 -CommandBatch "mirv_filmmaker health"
```

Console output reaches the client through tier0's spew function, which the hook chains in front of
the engine's. Commands run on the main thread: the listener posts a window message, and the hook's
window procedure queues each command into the engine's command buffer, as if it had been typed.

## Test demo

Runtime checks use a self-recorded SourceTV demo, because no CS:S demo ships with the repository
and SourceTV demos carry every player, which is what movie work starts from. Record one with bots
over the command port. Wait for `in-game=1` in `health` after the first line, a few seconds after
the second for the bots to spawn, and about 45 seconds before the last:

```text
sv_lan 1;;maxplayers 14;;tv_enable 1;;tv_delay 0;;tv_autorecord 0;;map de_dust2
bot_join_after_player 0;;bot_quota 10;;mp_freezetime 1;;mp_roundtime 3;;spectate;;mp_restartgame 1
tv_record mvm_css_tv
tv_stoprecord;;disconnect
```

SourceTV has to be enabled before the map loads. `bot_join_after_player 0` matters: by default CS:S
bots wait for a human to join a team, and the recorder is spectating. The demo lands in
`cstrike\mvm_css_tv.dem` and plays with `playdemo mvm_css_tv`. In playback the SourceTV camera
starts unattached (at the map origin) until it is pointed at someone, for example with
`spec_mode 4;;spec_next`.

Demos recorded on CS:S before the Orange Box engine (network protocol 7, the "v34" era) play too:
the hook converts them first. See [Old demos](#old-demos).

## Console commands

| Command | What it does |
| --- | --- |
| `mirv_filmmaker health` | Hook build, game, engine interfaces, command port counters, `mvm_debug` state, pump counts, the engine adapter, connection, map, demo time, the demo index, the game's and the rendered view, and a player summary |
| `mirv_filmmaker players` | One line per player: index, name, team, health, alive, dormant, origin, eye angles, class, bot and SourceTV flags. `>` marks the spectated player |
| `mirv_filmmaker engine` | The networked fields the engine adapter resolved, with their offsets |
| `mirv_filmmaker editor on\|off\|toggle\|state` | The Camera Editor workspace. `state` reports the mouse mode, tabs, preview rect and fonts, and where each demo bar control is |
| `mirv_filmmaker loop on\|off\|toggle\|state` | LOOP playback, as the demo bar's LOOP button does |
| `mirv_filmmaker seek <tick>` | Go to a demo tick the way the demo bar does (back = restart, then forward) |
| `mvm_debug start\|stop\|status` | The shared runtime debug log, as in CS2: `%APPDATA%\HLAE\debuglogs\mvm_debug_*.log`, every console line and every command received over the port, stamped with demo tick, frame and thread |

`health` is the first check when something stops working. `console capture REPLACED` means the
engine installed its own spew function after ours, so command output no longer reaches the port
or the log. A `frame render starts` count that stays at 0 means the client frame hook is not
running.

Console capture is one spew function chained in front of the engine's (`SourceConsole.cpp`). It
stays installed once the port or the log has asked for it, and feeds whichever of the two is on.
While SourceTV records on a listen server, `health` reads `REPLACED` and port output can arrive out
of order; both recover when the recording stops.

## Engine adapter

`SourceEngine.{h,cpp}` is what later steps read the game through: the game's and the rendered view,
demo state, the map, and players. It talks to the engine only through the 2013 SDK layouts and
virtual slots in `SourceSdk2013.h`, each call guarded so a wrong slot or a stale entity costs a
counted fault (`health` shows the count) instead of a crash. Networked fields such as `m_iHealth`
are looked up by name in the client's network tables at startup (`SourceNetvars.h`), so a game
update that moves them does not need new offsets. `mvm_debug start` writes the resolved offsets to
the log.

What differs per game is a `GameProfile` (`SourceGameProfile.h`): team names, where eye angles
are networked, and which game events the demo index keeps. CS:S has its own (`CSS/CssProfile.cpp`);
any other Source 1 game gets the generic one, which reads only fields every Source 1 multiplayer
game has.

## Demo index

CS:S writes no event sidecar next to a demo, the way CS2 writes `.dem.info`, so the hook reads the
events out of the demo file itself (`SourceDemoFile.{h,cpp}`). That gives the demo bar every kill,
death, round and bomb event from the first frame, not only the ones playback has reached.

- The engine prints `Playing demo from <file>.` whenever a demo starts, however it was started.
  The console capture passes that line to `SourceDemoIndex.cpp`, which finds the file under the
  game directory and reads it on a worker thread. A 3.4 MB SourceTV demo takes about 4 ms.
- The reader handles the Orange Box format (demo protocol 3, network protocol 24) that current
  CS:S, TF2, DoD:S and HL2:DM write. It reads or skips every network message by its layout, so an
  unknown message stops the scan with an error rather than misreading the rest. Events read before
  the stop are kept.
- Which events are kept comes from the game profile. CS:S keeps `player_death`, `player_team`,
  `round_start`, `round_freeze_end`, `round_end` and the bomb events.
- Ticks are demo ticks, the same numbers `demo_gototick` and `health` use. Checked in game: the
  reader put the first kill of the test demo at tick 490, and the game printed it at tick 490.
- Restarting the same demo (a backward seek replays it from the start) does not read it again.

`health` shows the index (`demo index: mvm_css_tv.dem, 6 events (3 deaths, 1 round starts) in
3.7 ms`), and `mvm_debug` logs each event under `demo.index`. `SourceDemoFileTests` writes small
demos bit by bit to test the reader; set `SOURCEMVM_SOURCE1_TEST_DEMO` to a real demo to also scan
that one and print its events.

## Demo bar

CS:S has no demo bar of its own, so the hook draws one: CS2's layout in CS:S paint. CS2's bar is
mostly CS2's own Panorama with SOURCE:MVM's speed dial, LOOP, CAM EDITOR and CONFIG added, and its
kill and round markers come from Valve's `.dem.info` sidecar. Here the whole bar is ours, and the
markers come from the [demo index](#demo-index).

| Part | What it does |
| --- | --- |
| Marker band | Round cells (R1, R2, ...) with a rule in the winner's colour at each round's end; a dot per kill in the victim's team colour (CS:S's chat blue and red), ringed for a headshot; an orange square per bomb event. Hover for "Josh killed Xander (usp, headshot)" and the time |
| Scrubber | Click or drag and release to seek there. A click on a kill goes 0.6 s before it, as CS2's Go To Event does |
| Play / pause, time | `demo_togglepause`; the time, the length, and the tick |
| -5S / +5S | Five seconds at the demo's tick rate (335 ticks at 66.7 tick) |
| < ROUND / ROUND > | The start of this round (or the one before, when already there), and the next round's start |
| SPECTATING | The spectated player |
| Speed dial | CS2's dial: logarithmic 0.05x to 20x, the middle exactly 1x; a click in one of five zones gives 0.1x, 0.25x, 1x, 4x or 10x; a drag gives anything in between (`demo_timescale`) |
| NEXT KILL > | The next kill, 0.6 s early |
| Camera | FIRST PERSON, CHASE CAM, FREE CAM in CS2's order (`spec_mode`) |
| LOOP | Seeds In and Out at the playhead and 320 ticks on (CS2's seed), then wraps playback at Out |
| CAM EDITOR | Opens the Camera Editor. Inside the editor it is not shown, because CS2 puts CONFIG there and Config is a later step |

The bar sits in the Camera Editor's REGULAR TIMELINE dock. Its rules are engine-free in
`Ui/DemoBarMath.h` (`SourceDemoBarMathTests`), the markers in `SourceDemoMarkers.cpp`
(`SourceDemoMarkersTests`), the drawing in `Ui/DemoBar.cpp`, and the game side in
`SourceDemoBar.cpp`. `mirv_filmmaker editor state` prints each control's rectangle, so a test can
click it by name.

Seeking backwards restarts the demo, waits until it plays again, and then seeks forward. In the
test SourceTV demo the engine's own skip sometimes stopped with "CL_PreserveExistingEntity" or
"CL_CopyExistingEntity: missing client entity 62", in either direction and from the console too;
first-person demos seek cleanly both ways. That is engine playback of that demo, not the bar, and
is checked again with more SourceTV demos.

## Old demos

Most classic CS:S movie demos were recorded before the Orange Box engine update, on the "v34"
engine (demo protocol 3, network protocol 7). The current game refuses them on its own ("demo
network protocol 7 outdated, engine version is 24"). With the hook loaded they play like any other
demo, for example `playdemo replays/old_match`:

- The hook wraps the engine's `playdemo` command (`SourceOldDemos.cpp`). When the file is an old
  demo, it converts it once on a worker thread and plays the converted copy. A 3 MB SourceTV demo
  takes about 50 ms.
- The copy goes to `cstrike/source_mvm/converted/`, under the same path as the original, with a
  `.src` stamp beside it: the original's size and write time and the converter version. Later
  plays reuse the copy until the original or the converter changes.
- The console reports each conversion, for example `converted replays/x.dem (old CS:S v34 demo) in
  46 ms: 2959 frames, map de_dust2, brushes 3 kept, 0 moved, 1 gone`. `health` shows the last one on
  its `old demos:` line.
- A demo recorded on a map that is not installed is not converted. The console names the map to
  put in `cstrike/maps`.
- Old SourceTV demos carry no director camera events, so the SourceTV camera would open at the
  map origin, following nobody. Once a player is alive the hook puts it in first person on the
  first player (`spec_mode 4; spec_next`), again after a backward seek restarts the demo. A
  player's own (first-person) recording is left alone.

The converter (`AfxHookSource/Filmmaker/DemoConvert/`, engine-free) keeps the demo's own network
tables and writes them in the new layout, so the game decodes every value the way the old one did,
and the values are copied bit for bit. It rewrites what changed between the two engines:

| Part | What changes |
| --- | --- |
| Messages | Message ids grow from 5 to 6 bits. `net_Tick` gains the frame time. `svc_ServerInfo` swaps the map CRC for an MD5 (left empty; playback does not check it) and gains a replay flag. Length and index fields in temp entities, string table updates, decals and prefetches widen |
| Entities | Entity and property index steps use the new encoding. `SPROP_NORMAL` is cleared on integers, because the new engine reads it there as a varint |
| Network tables | The new layout (wider property count, flags and bit count). DT_Beam's two arrays become tables, as the current client declares them. DT_Team's player array grows from 64 to 65 entries. Five properties Valve renamed get their new names: the rope material, the model scale, the ragdoll count, and the M3 and XM1014 reload state. `CWeaponCubemap`, which the current client lacks, is dropped |
| String tables | Entry data lengths widen. Three tables the current client expects are added empty |
| Sounds | Re-encoded field by field: flags, sound index, sequence, volume and origin |
| User messages | Renumbered to the current client's order. VoiceMask is dropped, and HintText loses the count byte the old one led with |
| Voice and user commands | Dropped. The 64-bit game has no Speex, and playback does not use user commands |

Valve rebuilt the CS:S maps after v34, so a demo's brush models ("*N": doors, glass, breakables,
trigger volumes) can have other numbers in the installed map, or be gone. Loading one the map does
not have stops the game with "bad inline model number". The converter reads the installed BSP's
models and entities (`MapModels.cpp`) and matches each brush entity, by where the demo puts it
(origin and collision bounds), to an installed model, in this order:

1. The same number, in the same place.
2. The same place under another number.
3. The nearest brush of about the same size (a door Valve moved a little).
4. For brushes built around their own origin, such as doors and moving parts, the same shape
   anywhere (a door first seen open).

A brush the map no longer has is left out, together with the entities that use it. On the maps
checked: cs_office, de_cbble and de_train keep every brush, de_inferno moves 18, de_nuke swaps 4
identical parts, and de_dust2 loses one volume that blocks smoke.

`SourceDemoConvertTool`, built with the Source 1 tests, converts outside the game.
`--dir <folder> <out folder>` converts a folder, and `SOURCEMVM_CONVERT_VERBOSE=1` lists which
brushes moved:

```batch
build\x64-release\AfxHookSource\Release\SourceDemoConvertTool.exe --maps "<cstrike folder>\maps" old.dem converted.dem
```

`SourceDemoConvertTests` covers the bit encodings, the table layouts, brush matching, and a whole
conversion of a small v34 demo written bit by bit, read back with the [demo index](#demo-index)
reader.

Checked on 2026-10-26: all 59 v34 demos in a test folder convert. Independent decoders read every
entity, string table and sound of the converted demos with the same values as the originals. Demos
on de_inferno, de_dust2, de_nuke, cs_office, de_cbble and de_train play in first person and chase
with no errors, de_train's cables draw, and the demo bar shows their kills and rounds.

Limits:

- Only network protocol 7 is converted. Demos of other protocols (one test demo used 17) are not.
- A demo that ends in a cut or broken frame is converted up to that frame, and the console says
  the recording ends early.
- Values the current client has no property for are lost: the old per-player fog settings (the
  current client reads fog from a fog controller), the spread in bullet effects, the Dual Elites'
  firing side, the size and lifetime of entity flames, projected-texture colour, and the colour
  correction weight.
- A backward seek restarts the demo, which resets the SourceTV camera, so it opens on the first
  player again rather than on the one being watched.

## Workspace UI

The workspaces are native C++ drawn with [Dear ImGui](https://github.com/ocornut/imgui)
(`Deps/release/imgui`, a submodule pinned to v1.91.9), laid out like CS2's and painted like CS:S.
The engine-free part lives in `AfxHookSource/Filmmaker/Ui/`:

| File | What it holds |
| --- | --- |
| `WorkspaceLayoutMath.h` | CS2's stage scaling, inspector column, dock measure and centred preview fit |
| `WorkspaceMouse.h` | Who has the mouse (UI, game, look), CS2's rules as one state machine both hosts use |
| `UiKeys.h` | ESC closes, G trades the mouse, and the key mapping into ImGui, for both hosts |
| `UiTheme.h` | CS:S colours (ClientScheme / SourceScheme), Verdana and Tahoma, and CS2's UI metrics |
| `UiStyle.cpp`, `UiWidgets.cpp` | The ImGui style, fonts at the stage scale, CS2's text tabs and chips |
| `CameraEditorShell.cpp` | The Camera Editor: preview, inspector tabs, dock ribbon, ESC EXIT |
| `UiRenderDx9.cpp` | Draws a finished frame on the render thread, and letterboxes the game into the preview |

`SourceUi.cpp` is the game host. The UI is built on the main thread at `FRAME_RENDER_START`, copied,
and drawn by the thread that presents, just before `Present`, so no D3D call leaves that thread.
While a workspace is open the finished game frame is copied out of the back buffer and drawn into
the preview rect, black around it, as CS2's preview viewport does.

The mouse follows CS2's workspace rules (`UiClickMath.h`, shared with CS2):

- While a workspace is open the cursor is free and the game sees no mouse movement or buttons:
  the hook swallows the game's cursor recentring and zeroes raw mouse input.
- A left click in the preview hands the mouse to the game. G hands it back.
- A right press in the preview that is held for 200 ms or dragged 4 px looks around until it is
  released, then the cursor returns where it was. A quicker right click stays a click.
- The wheel over the preview is the game's; over a panel it scrolls the panel.
- ESC closes the workspace.

`mvm_debug` logs every button the UI sees (`ui.input`), mode changes (`ui.mouse`), hover changes
(`ui.hover`), the font build (`ui.fonts`) and open and close (`ui.editor`).

`Automation\capture\send-click.ps1 -Process cstrike_win64 -Button Left|Right -HoldMs <ms>` drives
the UI with real window messages. Post from a per-monitor DPI-aware thread, as that script does:
otherwise posted coordinates arrive scaled by the display's scale factor.

### Browser mockup

[UiMockupSource](../../UiMockupSource/README.md) compiles the same UI files to WebAssembly and runs
them in a browser, with a still capture of the game in the preview:

```batch
node Automation/ui/preview.mjs css --open
```

It needs [Emscripten](https://emscripten.org/docs/getting_started/downloads.html) (emsdk in
`%USERPROFILE%\emsdk`, or `SOURCEMVM_EMSDK`). Saving a UI source rebuilds it and reloads the page.
Review a UI change there first, then build it into the game and compare captures at the same window
size, as the [mockup-first workflow](../panorama/mockup-first-workflow.md) describes for CS2.

During playback, `local` and `observing` describe the demo's recorder. In a SourceTV demo that is
the SourceTV bot, so `observing` is the director's pick at record time. The playback camera itself
is read through the view.
