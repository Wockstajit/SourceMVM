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
| `mirv_filmmaker hud on\|off\|toggle\|state` | The [movie director](#movie-director)'s cards |
| `mirv_filmmaker blackout on\|off\|toggle\|status` | F8's blackout: no HUD, no bar, no cards |
| `mirv_filmmaker director state` | The director's mouse, bar, cards and readouts, and where each bar control is |
| `mirv_filmmaker marker ...` | [Camera paths](#camera-paths): `place`, `delete <i>`, `deleteall confirm`, `select <i>\|next\|prev`, `selectdelta <d>`, `next`, `prev`, `open [i]`, `close`, `movekey <i> <tick>`, `setval <i> <channel> <v>`, `fov <v>`, `roll <v>`, `undo`, `redo`, `ease <i> none\|in\|out\|inout`, `speed <i> <0.2..1>`, `reposition`, `placereposition`, `cancelreposition`, `arm`, `play`, `pause`, `stop`, `timing live\|freeze\|toggle`, `interp linear\|smooth\|cycle`, `speedmode manual\|constant\|persegment`, `constspeed <0.2..1>`, `speedmul <v>`, `autosnap`, `list` |
| `mirv_filmmaker capture "<file.bmp>"` | The next frame as the game shows it, the filmmaker's UI included, from the back buffer at Present. Quote the path: the console splits arguments at `:` |
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

In SourceTV playback the view is SourceTV's own camera (client.dll's `C_HLTVCamera`), whose mode and
target the client keeps to itself: the networked observer fields belong to the SourceTV bot and do
not follow the camera. The adapter finds the camera by following `spec_mode`'s callback to
`HLTVCamera()`, a one-instruction accessor, and reads the mode at +0x10 and the target at +0x30 (the
2013 SDK layout, checked against the x64 `client.dll`). `health` shows it on its `spectator:` line,
for example `SourceTV camera at client.dll+0x5b4730: mode 4, target 3`. This SDK numbers the
observer modes 3 fixed, 4 first person, 5 chase, 6 objective and 7 free cam.

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
| Camera | FIRST PERSON, CHASE CAM, FREE CAM in CS2's order (`spec_mode` 4, 5, 7) |
| LOOP | Seeds In and Out at the playhead and 320 ticks on (CS2's seed), then wraps playback at Out |
| CAM EDITOR | Opens the Camera Editor. Inside the editor it is not shown, because CS2 puts CONFIG there and Config is a later step |

The bar sits in the Camera Editor's REGULAR TIMELINE dock and, outside the workspaces, along the
bottom of the screen (the [movie director](#movie-director)). Its rules are engine-free in
`Ui/DemoBarMath.h` (`SourceDemoBarMathTests`), the markers in `SourceDemoMarkers.cpp`
(`SourceDemoMarkersTests`), the drawing in `Ui/DemoBar.cpp`, and the game side in
`SourceDemoBar.cpp`. `mirv_filmmaker editor state` prints each control's rectangle, so a test can
click it by name.

Seeking backwards restarts the demo, waits until it plays again, and then seeks forward. In the
test SourceTV demo the engine's own skip sometimes stopped with "CL_PreserveExistingEntity" or
"CL_CopyExistingEntity: missing client entity 62", in either direction and from the console too;
first-person demos seek cleanly both ways. That is engine playback of that demo, not the bar, and
is checked again with more SourceTV demos.

## Movie director

While a demo plays and no workspace is open, the hook runs CS2's movie director
(`AfxHookSource2/Filmmaker/Movie/MovieMode.cpp`, `Panorama/MovieHudJs.h`) in CS:S paint:

- The [demo bar](#demo-bar) along the bottom edge, the full width. Its panel covers CS:S's own
  bottom spectator bar, whose player name and camera options it replaces.
- The MOVIE DIRECTOR card in the top-right corner, under CS:S's top spectator bar: camera mode,
  playback, free cam speed, and the camera's position, angles, FOV, tick and time. The readouts
  refresh at CS2's 10 Hz.
- The CONTROLS card on the left, a third of the way down, listing the keys below.

| Input | What it does |
| --- | --- |
| Mouse | The cursor stays free and the game gets no mouse. The bar takes its own clicks |
| Click | Left: the next player (`spec_next`); right: the previous one (`spec_prev`) |
| Hold or drag | Held 200 ms or moved more than 4 px, a press looks around (chase, free cam) until it comes up; the cursor then returns where the press was. CS2's rule, shared with the workspaces (`UiClickMath.h`) |
| Wheel | First person, chase, free cam, round again (`spec_mode` 4, 5, 7); up steps forward |
| Ctrl+wheel | FOV, a degree a notch, wheel up zooms in, 1 to 170 (HLAE's view FOV override). It belongs to the camera: a camera change drops it |
| Shift+wheel | In the game's free cam (`spec_mode` 7), its speed on CS2's ladder (0.5x steps, halving below 0.5x); 1x is CS:S's `sv_specspeed` 3. With HLAE's [free cam](#free-cam) on, that camera's speed |
| Space | Pause and play; the game never sees the key, so nothing jumps |
| Left / Right | Five seconds back and on (back restarts and seeks forward, as the bar does) |
| F8 | Blackout: no HUD, crosshair or spectator bars (`cl_drawhud 0`), no bar, no cards. Again for everything back. With the cards switched off, F8 brings them back |

The director stands aside while the game's menu or console is up (`IEngineVGui::IsGameUIVisible`,
`Con_IsVisible`), leaving the game the mouse and the keys. When the demo ends, the blackout, the FOV
and the free cam speed go back to the game's. Demo playback accepts `cl_drawhud` even when the
demo's server had cheats off. Ctrl+wheel's FOV is the hook's view FOV override (`mirv_fov`'s),
because `demo_fov_override` does not reach SourceTV's camera.

SourceTV's camera resets whenever a demo starts or a backward seek restarts it. The director
remembers the camera mode and the player it follows, twice a second, and after a restart puts both
back (`spec_mode`, `spec_player <user id>`), as CS2 keeps the watched player across a seek. A demo
whose camera starts at the map origin following nobody (old demos, and the self-recorded test demo)
opens in first person on the first player instead.

The rules are engine-free in `Ui/DirectorMath.h` and `Ui/DirectorMouse.h`
(`SourceDirectorMathTests`), the drawing in `Ui/DirectorHud.cpp`, and the game side in
`SourceDirector.cpp`. The browser mockup runs the same code with a stand-in spectator camera.

What comes from HLAE's inherited Source 1 hook, and what does not. The director uses HLAE's view
FOV override (`Hook_VClient_RenderView::FovOverride`, `mirv_fov`'s), the game's and the rendered
view and the demo state (`Hook_VClient_RenderView`, `GetDemoInfoEx`), and HLAE's window, cursor and
raw-input hooks, where the filmmaker's handlers run first. HLAE has no x64 CS:S counterpart for the
rest: its SourceTV camera scan (`s_HLTVCamera`) and spectator tools are 32-bit CS:GO code, its
command hook (`Cmd_ExecuteCommand`) is CS:GO and TF2 only, `WrpConVarRef` reads only CS:GO's cvar
interface, and its HUD switches (`mirv_streams` drawHud) are 32-bit only. So the camera is read
through `spec_mode`'s callback, and the HUD, cameras and speed go through console commands. HLAE's
free camera, `mirv_input`, does run on x64 CS:S, and it is the [free cam](#free-cam).

### Free cam

HLAE's own free camera, `mirv_input` (`Shared/MirvInput`), is the filmmaker's free cam in CS:S, as
it is in CS2 (`Movie/CameraBridge.cpp` wraps the same class). `mirv_input camera` turns it on and
`mirv_input end` off, and HLAE's keys fly it: W, A, S, D, numpad 9 and 3 up and down, X and Z roll,
Up and Down pitch, PgUp and PgDn the FOV, Home back to level (the HLAE manual has the rest). HLAE's
view hook applies it (`Hook_VClient_RenderView`, the `Input` override). The director adds what CS2
adds around it:

| Input | With the free cam on |
| --- | --- |
| Press outside the bar | Steers at once, no player step: the camera has the mouse and the keys until the button comes up, then the cursor returns where the press was |
| Cursor free | The free cam waits and its held keys are dropped, so it flies only while a button is held, as in CS2. HLAE's `GetSuspendMirvInput` asks the filmmaker (one marked line in `RenderView.cpp`) |
| Ctrl+wheel | Its FOV, a degree a notch, wheel up zooms in (`MirvInput::NudgeFov`) |
| Shift+wheel | Its speed on CS2's ladder (`FreeCamStepMath.h`) |
| Shift held | A quarter of the speed |
| Wheel, bar camera button | One of the game's cameras takes the view back, and the free cam ends |

It turns with the game's own aim: degrees per mouse count are `sensitivity` times `m_yaw` and
`m_pitch`, read once a second from CS:S's console variables through the CS:S SDK's `ConVar`, because
HLAE's `WrpConVarRef` reads only CS:GO's. The MOVIE DIRECTOR card shows Free cam in orange with its
speed.

Checked in CS:S on 2026-10-26 with a paused demo: the read sensitivity matched the console (1.12,
0.022, 0.022); Ctrl+wheel took the FOV to 87 and Shift+wheel the speed to 2x; held with W for 0.8 s
it flew about 535 units (320 x 2 x 0.8); with the cursor free W did nothing; a plain wheel notch
returned to chase cam. Not checked: turning it with a physical mouse.

### Smooth View

CS2's Smooth View runs in CS:S from the same code: `AfxHookSource2/Filmmaker/Movie/SmoothView.cpp`
is compiled into both hooks, and `SmoothViewHost.h` is what it asks the game it runs in (CS2 answers
in `SmoothViewHostSource2.cpp`, CS:S in `SourceSmoothView.cpp`). It adds breathing, a lean into
strafes, and a pitch while falling or jumping to the game's camera; it never moves the camera or
changes the FOV. It runs from HLAE's view hook after HLAE's overrides (one marked line in
`RenderView.cpp`), only when none of them moved the camera, so the free cam and campaths stay as
they are, as in CS2.

CS:S sends a player's velocity only to that player, so a SourceTV demo has none for the player it
watches; Smooth View measures the camera's own movement instead, which is its fallback in CS2. CS:S
moves its client clock every frame rather than every tick, so that measurement is smooth.

`mirv_filmmaker smoothview` takes CS2's subcommands: `on`, `off`, `toggle`, `state`, `reset`,
`breathX`, `breathY`, `standAmp`, `walkAmp`, `runAmp`, `tiltEnabled`, `tilt`, `tiltSpeed`,
`tiltInv`, `falling`, `jumping`, `healthSway`. Its Config card and the saved settings come with the
Config workspace.

Checked in CS:S on 2026-10-26 in the SourceTV test demo: with it on, the rendered view drew away
from the game's by fractions of a degree, with up to a degree of roll in strafes, and breathing at
the running amplitude; over 5,600 logged frames no frame moved the roll by more than 0.08 degrees
except a camera cut from the free cam, where it starts afresh; with the free cam flying it did
nothing.

In the game's free cam (`spec_mode` 7), X and Z held roll right and left at 90 degrees a second, as
in CS2; the roll belongs to that camera, so a camera change clears it. CS2's X-ray (X in first
person and chase) has no CS:S counterpart. The camera-path keys (K, J, L, F) come with the Camera
Editor's paths.

## Camera paths

CS2's camera paths run in CS:S from CS2's own code. The marker list (`CamMarkers`), the path built
on HLAE's `CamPath` spline (`CamPathEval`), its playback with the predicted demo clock and the
smoothed pose (`CamPlayback`) and aim picking (`CamMarkerPickMath.h`) are compiled from
`AfxHookSource2/Filmmaker/Movie/` into both hooks. CS2's camera bridge (`CameraBridge.h`) is what
they ask the game: CS:S answers in `SourceCameraBridge.cpp` with HLAE's free camera, which flies
the path as it does in CS2, HLAE's view hook and HLAE's demo clock (`GetCurrentDemoTick/Time`).
`SourceCameraPath.cpp` is the facade, as CS2's `CameraPath` and Deadlock's `DeadlockShot` are.
The path builds on its own `CamPath`, so `mirv_campath` keeps HLAE's separate path in the view hook.

| Key | Outside the workspaces and in the Camera Editor |
| --- | --- |
| K | A marker at the view as it is (position, angles, roll, FOV) and the demo's tick; one per tick, so a second press on the same tick re-poses it |
| L | Deletes the marker the view aims at (a cone of about 5 degrees that widens up close); with none aimed at, the key is the game's |
| F | Opens the CAMERA MARKER card on the marker the view aims at; F or Esc closes it. While a camera is being repositioned, F drops it |
| J | Arms the path: the demo goes paused to the first marker and holds its pose. J or Space then plays, Esc cancels, the arrows are held |
| Space, Esc | While playing: Space pauses, J or Esc stops; W, A, S or D stop the path and move the free cam on the same press |

A path needs two markers. Live timing plays it against the demo's ticks, so the camera flies
while the demo plays and the demo stops paused on the last marker, where the game's camera takes the
view back. Freeze plays it on the wall clock with the demo held. With the demo bar's LOOP on, a path
that ends plays again. Speed modes (Manual, Constant, Per-segment), the curve (Linear, Smooth) and
per-marker ease are CS2's. While a path flies the view, presses neither look nor step players and a
camera change stops it.

Each demo keeps its own path, saved where CS2 saves its own: the `campath` section of the demo's
`project.mvm` (`Demo/DemoProjectStore.h`, shared with CS2 and Deadlock), under
`%APPDATA%\HLAE\source_mvm\demos\css\`. The project is keyed on the file the user played, so an old
v34 demo's path belongs to the original, not to its converted copy. An edit is saved at the next
frame (a drag once it ends); playing another demo saves the outgoing path and loads the new demo's,
or starts empty. Checked on 2026-10-26: three markers, an ease and Freeze timing came back after
quitting and relaunching CS:S, and after switching to another demo and back.

Input that CS:S hands the window procedure on its second thread (it does while a demo restarts or
seeks) runs on the main thread: the handlers decide on the spot and post their action
(`SourceMainThread.h`).

### In the Camera Editor

The PATH CAMERA tab is CS2's (`CameraEditorPathInspectorJs.h`), drawn by `Ui/PathInspector.cpp`
with CS2's card grammar (`Ui/InspectorCards.cpp`, from `CameraEditorCardsJs.h`). The rail lists
the cameras, CAMERA 1 to N with each one's tick and curve (and the selected one's FOV), then
+ Add camera. The SELECTED CAMERA card carries Clear all and + Add in its header, then:

| Rows | What they do |
| --- | --- |
| The camera switcher | Previous and next, which camera of how many, and its tick |
| THIS CAMERA | Reposition, Move to playhead, Delete |
| LENS | FOV and roll of the selected camera, dragged from where they are (Shift is fine control) |
| CURVE . WHOLE PATH | Linear or Smooth for the path, and the selected camera's ease |
| SPEED | Manual, Per-Seg or Constant, the amount (0.20, 0.50, 0.80, 1.00), Live or Freeze |

Selecting a camera, on the rail or with the switcher, sends the demo paused to its tick and holds
the free cam on its pose until the seek lands; then the free cam is the user's again. Reposition
sends the demo to the camera's tick and gives the game the mouse to fly the free cam; a left click
or F drops the camera there, Esc cancels. A value drag and a retime are one undo step each
(`mirv_filmmaker marker undo`, `redo`). Clear all asks first, as CS2 does. Rows that do not apply
are disabled in place, and the card's summary keeps two lines, so nothing moves under the pointer.

Under the body the tab pins CS2's key strip. Those keys work in the editor as in the director:
Space, Left and Right drive the demo, X and Z roll the free cam, and over the preview the wheel is
the director's (Ctrl for FOV, Shift for the free cam's speed, a plain notch steps the camera).

Checked in CS:S on 2026-10-26 at 1600x900 by clicking each control with posted window messages:
a rail row and the switcher selected and seeked; a 30-pixel FOV drag went from 90 to 127.4 as one
undo step; Smooth, ease In, Per-Seg with its amount, Freeze and back, Move to playhead and its
undo, Reposition then F, + Add, Delete, and Clear all with No then Yes each did what they say.
The browser mockup draws the same tab from a stand-in list; with the Claude window off screen its
browser pane does not draw, so the review used the game's captures.

### The marker card and banner

F on an aimed marker opens CS2's CAMERA MARKER card (`MarkerHudJs.h`), drawn by
`Ui/MarkerPopup.cpp` in the director cards' language (`Ui/HudCard.h`, which `DirectorHud.cpp` now
shares). It works outside the workspaces and over the Camera Editor:

| Part | What it holds |
| --- | --- |
| Header | Previous and next around CAMERA MARKER #n . N TOTAL; with auto-snap on the view moves to the marker |
| PLAYBACK | Speed mode, path curve, timing (Live gold, Freeze blue) and auto-snap as one-click selectors; the constant speed in Constant and the segment speed in Per-Segment (not on the last marker); a line on what the mode does |
| ACTIONS | Reposition Camera, Remove Marker, Remove All Markers (a second click confirms) |
| CONTROLS, Done | K, J, L and F, then F / ESC Done |

While the card is up every press and wheel notch is its own, so the spectated player cannot change
under it. A banner in CS2's place, centred 11% down the game (the window, or the editor's
preview), shows the path's notices ("Need at least 2 camera markers to play path.") and
REPOSITIONING CAMERA #n while a camera is moved. Outside the editor the free cam makes every press a
look, so while repositioning a left press let go within 200 ms is the placing click.

Checked in CS:S on 2026-10-26 at 1600x900: F opened the card on the aimed marker; the arrows
selected, and with auto-snap on moved the view to the marker; speed mode, timing and auto-snap
switched; Reposition Camera showed the banner and a quick left click placed the camera at the free
cam; Remove All needed its second click; J with no markers showed the notice banner; in the editor
F opened the card over the workspace and Esc closed the card, not the editor.

### In the world

The path is drawn in the world as CS2 draws it (`Ui/PathOverlay.{h,cpp}`, CS2's
`CampathDrawer.cpp` marker style): each marker a small camera with a shaded body, a glass lens out
to its field of view and a triangle for up, its index beside it; the line along the path, sampled
from the evaluator that flies it; and the playhead, a dot where the path puts the camera at the
demo's tick. Live paths are gold and Freeze paths blue, and the marker the view aims at turns white.
It shows outside the workspaces and in the Camera Editor's preview, and hides while a path plays and
during F8's blackout.

CS2 draws these with its own line shader in the world pass. CS:S has no such pass on x64 (HLAE's
Source 1 campath drawer hooks the 32-bit material system), so the overlay projects on the CPU with
Deadlock's projection (`DeadlockOverlayMath.h`) and CS2's marker shading (`SceneMarkerMath.h`) into
a draw list. The view hook builds it from the view the game is about to render, after every
override, and the renderer lays it over the game's frame before the UI, so the Camera Editor's
letterbox scales it into the preview with the frame. Nothing in it has depth: markers show through
walls, as CS2's do.

The projection needs the FOV the engine renders, not the one the view hook sees. CS:S widens its
4:3 FOV for the screen as Alien Swarm does, which HLAE's table for the 64-bit game leaves out:
`health` prints the engine's own matrix (`engine matrix fov 106.30 x 73.74` for FOV 90 at 854x480),
and `View_ScreenFov` in `SourceEngine.h` widens to match. `health` also counts the overlay's frames
and vertices (`path overlay:`); `mvm_debug` logs its inputs once a second (`path.overlay`).

Checked in CS:S on 2026-10-26 in an old dust2 SourceTV demo: K placed markers at three ticks while
it played; J armed at the first, paused, with its pose; Space played the path through the markers
while the demo ran and stopped paused on the last marker, the game's camera back; Freeze timing with
a Smooth constant-speed path flew with the demo held; another demo started with an empty path. In
the self-recorded test demo the resume after arming hit the engine's "missing client entity 62"
again (see [Demo bar](#demo-bar)).

The in-world drawing was checked the same day in the same demo, from back-buffer captures: three
markers and the line and playhead from behind and from the side; a marker placed at a T's feet
landed on the model's feet from a view that put him well off centre (with FOV 90 unwidened it missed
by about 80 pixels); the aimed marker turned white and Freeze turned the path blue; the overlay
scaled into the Camera Editor's preview; `health` counted no vertices during F8's blackout and while
the path played, and the overlay came back after. Fast turns were not measured: with the material
system's queued rendering the overlay can be a frame apart from the image it is laid over.

## Old demos

Most classic CS:S movie demos were recorded before the Orange Box engine update, on the "v34"
engine (demo protocol 3, network protocol 7). The current game refuses them on its own ("demo
network protocol 7 outdated, engine version is 24"). With the hook loaded they play like any other
demo, for example `playdemo replays/old_match`:

- The hook wraps the engine's `playdemo` command (`SourceOldDemos.cpp`). When the file is an old
  demo, it converts it once on a worker thread and plays the converted copy. A 0.8 MB SourceTV demo
  took 46 ms, reading the map included.
- The copy goes to `cstrike/source_mvm/converted/`, under the same path as the original, with a
  `.src` stamp beside it: the original's size and write time and the converter version. Later
  plays reuse the copy until the original or the converter changes.
- The console reports each conversion, for example `converted replays/x.dem (old CS:S v34 demo) in
  46 ms: 2959 frames, map de_dust2, brushes 3 kept, 0 moved, 1 gone`. `health` shows the last one on
  its `old demos:` line.
- A demo recorded on a map that is not installed is not converted. The console names the map to
  put in `cstrike/maps`.
- Old SourceTV demos carry no director camera events, so the SourceTV camera would open at the
  map origin, following nobody. The [movie director](#movie-director) opens them in first person
  on the first player.

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

Open: once on 2026-10-26, playing a converted demo straight after the self-recorded test demo, in
a session that had played the converted demo before, crashed the game two ticks in, inside the
engine's entity decoding (`engine.dll+0x18a298`, a property index with no decoder). Five more
switches in both directions, in the same order, did not crash. It needs another repro before a cause
can be named.

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
| `InspectorCards.cpp` | CS2's inspector card grammar: rail rows, card header, value, mode and action rows, key strip |
| `PathInspector.cpp` | The PATH CAMERA tab and its Clear all dialog |
| `PathOverlay.cpp` | The camera path drawn in the world |
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
- The wheel over the preview is the director's, as CS2's MovieMode keeps it in the editor: Ctrl
  for FOV, Shift for the free cam's speed, a plain notch steps the camera. Over a panel it scrolls
  the panel.
- While a dialog is up (Clear all) the preview takes no clicks.
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
