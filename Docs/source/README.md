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
ctest --test-dir build/x64-release -C Release --output-on-failure -R "NetconServerTests|SourceNetvarsTests"
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

Demos recorded on CS:S before the Orange Box engine (network protocol 7, the "v34" era) do not
play on the current game: it stops with "demo network protocol 7 outdated, engine version is 24".

## Console commands

| Command | What it does |
| --- | --- |
| `mirv_filmmaker health` | Hook build, game, engine interfaces, command port counters, `mvm_debug` state, pump counts, the engine adapter, connection, map, demo time, the game's and the rendered view, and a player summary |
| `mirv_filmmaker players` | One line per player: index, name, team, health, alive, dormant, origin, eye angles, class, bot and SourceTV flags. `>` marks the spectated player |
| `mirv_filmmaker engine` | The networked fields the engine adapter resolved, with their offsets |
| `mirv_filmmaker editor on\|off\|toggle\|state` | The Camera Editor workspace. `state` reports the mouse mode, tabs, preview rect and fonts |
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

What differs per game is a `GameProfile` (`SourceGameProfile.h`): team names and where eye angles
are networked. CS:S has its own (`CSS/CssProfile.cpp`); any other Source 1 game gets the generic
one, which reads only fields every Source 1 multiplayer game has.

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
