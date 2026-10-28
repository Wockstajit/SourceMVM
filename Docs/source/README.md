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

### Quitting

The 64-bit game never calls the client's `Shutdown` through HLAE's wrapper, so the hook wraps the
`quit` and `exit` commands instead: the filmmaker saves its settings, stops the demos scan, closes
the size card and stops the command port, and then the engine quits. Before this, every quit of a
game with the hook ended in an abort (`ucrtbase.dll`, 0xc0000409): the port's threads were still
running when the process ran its static destructors, and a `std::thread` destroyed while joinable
calls `std::terminate`. The port and the demos worker also detach a thread still joinable at exit,
in case the game ends some other way. `mvm_debug` logs `filmmaker shutdown`.

Checked on 2026-10-27: three quits during demo playback each logged the shutdown, and Windows logged
no crash. Without the hook, the game exits cleanly.

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

`mvm_css_tv` has no grenade thrown, so the grenade checks use a second demo, `mvm_css_nades`,
recorded the same way with money for grenades. Its second line is
`bot_join_after_player 0;;bot_quota 10;;mp_startmoney 16000;;bot_allow_grenades 1;;mp_freezetime 6;;mp_roundtime 3;;spectate;;mp_restartgame 1`,
and `tv_record mvm_css_nades` follows within three seconds, inside the freeze time, so no grenade
is in the air when the recording starts. Round 1 has the throws the checks use: Yahn's HE at tick
480, Norm's flashbang at 528, Erik's HE at 568 and Will's HE at 741. Without the hook, the game
stops `mvm_css_nades` at its second round start, tick 6396, and a recording that starts with
grenades in the air six seconds in, both on `CL_PreserveExistingEntity: missing client entity`.
The hook keeps them playing ([Entities a demo deletes while still sending
them](#entities-a-demo-deletes-while-still-sending-them)).

Demos recorded on CS:S before the Orange Box engine (network protocol 7, the "v34" era) play too:
the hook converts them first. See [Old demos](#old-demos).

## Console commands

| Command | What it does |
| --- | --- |
| `mirv_filmmaker health` | Hook build, game, engine interfaces, command port counters, `mvm_debug` state, pump counts, the engine adapter, connection, map, demo time, the demo index, who may have the mouse, the window and what the engine renders, the game's and the rendered view, and a player summary |
| `mirv_filmmaker players` | One line per player: index, name, team, health, alive, dormant, origin, eye angles, class, bot and SourceTV flags. `>` marks the spectated player |
| `mirv_filmmaker engine` | The networked fields the engine adapter resolved, with their offsets. `engine classes [filter]` lists the game's network class names |
| `mirv_filmmaker entities [all] [class]` | One line per entity: index, handle, network class, origin, angles, team, and its links (owner, weapon owner, thrower, active weapon, ragdoll, the ragdoll's player). `all` includes dormant ones |
| `mirv_filmmaker entities watch <class>\|eye\|off` | Logs (`mvm_debug`, `entities.watch`) each entity of a matching class the frame it appears and the frame it goes. `eye` logs the spectated player's view height in first person instead (`entities.eye`) |
| `mirv_filmmaker entities releases <index>` | Logs (`mvm_debug`, `entities.release`) every deletion of an entity of that entity's class, with the entity and the three calls that led to it as module and offset: `engine.dll` for a demo packet that deletes it, `client.dll` for the client deleting it on its own. One class per game session |
| `mirv_filmmaker demos on\|off\|toggle\|state` | The [demos browser](#demos-browser). `demos` alone lists its library commands: `list`, `folders`, `addfolder [path]`, `removefolder <i>`, `hidden`, and the page's own |
| `mirv_filmmaker editor on\|off\|toggle\|state` | The Camera Editor workspace. `state` reports the mouse mode, tabs, preview rect and fonts, and where each demo bar control is |
| `mirv_filmmaker loop on\|off\|toggle\|state` | LOOP playback, as the demo bar's LOOP button does |
| `mirv_filmmaker seek <tick>` | Go to a demo tick the way the demo bar does (back, [in place](#seeking-back)) |
| `mirv_filmmaker scrub drag\|release <tick>`, `scrub cache [on\|off\|clear\|budget <MB>]` | The [live scrub](#live-scrub) and its frame cache, as a playhead drag and release do |
| `mirv_filmmaker hud on\|off\|toggle\|state` | The [movie director](#movie-director)'s cards |
| `mirv_filmmaker blackout on\|off\|toggle\|status` | F8's blackout: no HUD, no bar, no cards |
| `mirv_filmmaker director state` | The director's mouse, bar, cards and readouts, and where each bar control is |
| `mirv_filmmaker marker ...` | [Camera paths](#camera-paths): `place`, `delete <i>`, `deleteall confirm`, `select <i>\|next\|prev`, `selectdelta <d>`, `next`, `prev`, `open [i]`, `close`, `movekey <i> <tick>`, `setval <i> <channel> <v>`, `fov <v>`, `roll <v>`, `undo`, `redo`, `ease <i> none\|in\|out\|inout`, `speed <i> <0.2..1>`, `reposition`, `placereposition`, `cancelreposition`, `arm`, `play`, `pause`, `stop`, `timing live\|freeze\|toggle`, `interp linear\|smooth\|cycle`, `speedmode manual\|constant\|persegment`, `constspeed <0.2..1>`, `speedmul <v>`, `autosnap`, `list` |
| `mirv_filmmaker grapheditor ...` | The [TIMELINE dock](#the-timeline-dock)'s keys: `on`, `select`, `movesel`, `addkey`, `delsel`, `handle`, `ease`, `smooth`, `linear`, `align`, `undo`, `redo`, `playhead`, `autopath` and the rest of CS2's verbs |
| `mirv_filmmaker follow ...` | The [follow camera](#follow-camera), CS2's own command: `type player`, `nearest`, `select <i>`, `place`, `reposition`, `preview`, `stop`, `status`, `campose`, `targetprobe`, `look`, `position`, `deadzone`, `maxturn`, `fov`, `autodead`, `deathhold`, `debug`, `state64` and the rest of CS2's verbs |
| `mirv_filmmaker render ...` | The [Render workspace](#render-workspace), CS2's command: `open`, `close`, `tab`, `range`, `in`, `out`, `camera`, `pass`, `output`, `sound`, `start`, `cancel`, `status`, `validate`, `takes`, `import` and the rest of CS2's verbs |
| `mirv_filmmaker capture "<file.bmp>"` | The next frame as the game shows it, the filmmaker's UI included, from the back buffer at Present. Quote the path: the console splits arguments at `:` |
| `mirv_filmmaker window ...` | [Window resize](#window-resize): `status`, `resizable on\|off`, `size <w> <h>`, `aspect <r>`, `ratios`, `snap both\|ratio\|resolution\|off`, `overlay on\|off\|test` |
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
are networked, which game events the demo index keeps, which network classes are players, weapons,
thrown grenades and ragdolls, how a player links to its ragdoll, and the eye heights. CS:S has its
own (`CSS/CssProfile.cpp`); any other Source 1 game gets the generic one, which reads only fields
every Source 1 multiplayer game has.

Since step 11 the adapter reads any entity, not only players (`Entities_Snapshot`), for the follow
camera's targets: its handle (the index in the low 12 bits, a serial above), network class, origin,
angles and team, and the handles that link it to others, read by name like the rest:
`m_hOwnerEntity`, a weapon's `m_hOwner`, a grenade's `m_hThrower`, a player's `m_hActiveWeapon`,
`m_fFlags` and `m_hRagdoll`, and a ragdoll's `m_hPlayer`. A link is read only on the classes that
network it. CS:S's classes, from the game's own list: `CCSPlayer`; weapons are `CWeapon<Name>`
(`CWeaponUSP`, `CWeaponM4A1`, ...) plus the older `CAK47`, `CC4`, `CDEagle`, `CFlashbang`,
`CHEGrenade`, `CKnife` and `CSmokeGrenade`; every thrown grenade is one class,
`CBaseCSGrenadeProjectile`, so its kind is not in the class; the planted bomb is `CPlantedC4`; and
a dead player's body is a separate `CCSRagdoll` entity. A player's eyes are 64 units above its
origin standing and 47 ducking (`m_fFlags` bit 1), the heights CS:S's first-person view sat at.

Checked in the SourceTV test demo on 2026-10-26 (`entities`, `entities watch`): every player listed
its active weapon, every held weapon its owner, and the carried C4 its carrier; after round 1's
deaths five `CCSRagdoll`s each named their player, and the dropped C4 had no owner; the planted
C4 appeared at tick 3284, the tick of the demo's `bomb_planted` event; the eye heights came out
64.00 standing and 47.00 ducking, within about a unit while moving, because an origin read at
frame start is a frame apart from the interpolated view. All 14 fields resolved, with no faults.

A thrown grenade adds two fields, read by name like the rest: its velocity now (`DT_BaseGrenade`'s
`m_vecVelocity`) and the velocity it was thrown with (the profile's launch field, CS:S's
`DT_BaseCSGrenadeProjectile.m_vInitialVelocity`). Every thrown grenade in CS:S is the one class
`CBaseCSGrenadeProjectile`, so its kind comes from its model's name, which `Entity_ModelName`
reads from the studio header (`name` at +12, 64 bytes): `w_eq_fraggrenade_thrown`,
`w_eq_flashbang_thrown` and `w_eq_smokegrenade_thrown` are an HE grenade, a flashbang and a
smoke. `mirv_filmmaker entities Projectile` prints both velocities and the model. With them the
adapter resolves 16 fields.

For Attached mounts the adapter also reads a model's bones and attachments, through the entity's
renderable (`IClientUnknown` slot 5), all measured in the x64 `client.dll` and `engine.dll`
(`SourceSdk2013.h`). `Entity_Bone` finds the bone by name in the studio header
(`VModelInfoClient006`'s `GetStudiomodel`, slot 28; `numbones` at +156, `boneindex` at +160,
216-byte bones), then has the model set up its own bones (`SetupBones`, slot 16) and returns that
bone's matrix. `Entity_Attachment` asks the model (`LookupAttachment`, slot 35, then
`GetAttachment`, slot 37). Both run guarded, so a bad read counts a fault instead of crashing.
CS:S's player models have only the gear attachments (grenades, pistol, primary, C4, feet,
`muzzle_flash`), none on the head or hands, so body mounts read ValveBiped bones; a weapon's world
model has one attachment, `muzzle_flash`, and `w_c4` has none.

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
- Playing the same demo again does not read it again.

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
| Scrubber | Click to seek there; drag and the world follows (the [live scrub](#live-scrub)), landing on the tick at release. A click on a kill goes 0.6 s before it, as CS2's Go To Event does |
| Play / pause, time | `demo_togglepause`; the time, the length, and the tick |
| -5S / +5S | Five seconds at the demo's tick rate (335 ticks at 66.7 tick) |
| < ROUND / ROUND > | The start of this round (or the one before, when already there), and the next round's start |
| SPECTATING | The spectated player |
| Speed dial | CS2's dial: logarithmic 0.05x to 20x, the middle exactly 1x; a click in one of five zones gives 0.1x, 0.25x, 1x, 4x or 10x; a drag gives anything in between (`demo_timescale`) |
| NEXT KILL > | The next kill, 0.6 s early |
| Camera | FIRST PERSON, CHASE CAM, FREE CAM in CS2's order (`spec_mode` 4, 5, 7) |
| LOOP | Seeds In and Out at the playhead and 320 ticks on (CS2's seed), then wraps playback at Out. While it is on, the region has CS2's In and Out flags on the marker band: a line with an I or O tab outside the region, dragged from where it was grabbed (Shift moves it 1/16 as far), with its tick and time shown while held or hovered. Both commit together on release, so dragging In past Out swaps them. `mirv_filmmaker loop range <in> <out>` sets them from the console |
| CAM EDITOR, CONFIG | Open the Camera Editor and the [Config workspace](#config-workspace). Inside a workspace one button swaps to the other: CONFIG in the editor, CAM EDITOR in Config. CONFIG is in CS:S's CT blue, as CS2's is blue |

The bar sits in the Camera Editor's REGULAR TIMELINE dock and, outside the workspaces, along the
bottom of the screen (the [movie director](#movie-director)). Its rules are engine-free in
`Ui/DemoBarMath.h` (`SourceDemoBarMathTests`), the markers in `SourceDemoMarkers.cpp`
(`SourceDemoMarkersTests`), the drawing in `Ui/DemoBar.cpp`, and the game side in
`SourceDemoBar.cpp`. `mirv_filmmaker editor state` prints each control's rectangle, so a test can
click it by name.

### Seeking back

CS:S's own demo player seeks back by starting the demo over. `demo_gototick` disconnects, signs on
and loads the level again, so the loading screen and the game's menu flash by for about half a
second before the demo skips forward to the tick. The hook seeks back in place instead
(`SourceDemoRewind.cpp`), the way the engine goes on after that reload:

- the client frees its entity baselines, as `cl_fullupdate` does;
- the demo file goes back to its first command after the sign-on data, a sync tick at tick 0 in
  every demo measured, and the skip runs from there;
- the demo's first packet is a full snapshot, and on one the engine deletes every entity and builds
  them again, so nothing from later in the demo stays behind.

Every skip, forward ones too, may run for up to 250 ms in the frame it was asked in, where the engine
stops at 100 packets a frame. A seek therefore lands in that frame, and no frame of the skip is shown.
The bar, a typed `demo_gototick`, Go To Event and the [live scrub](#live-scrub) all go through it. The
offsets are checked against the engine's own code first; if they do not match, the engine's reload
stays and `health` says so.

A seek stops on the demo's first packet at or after its tick. A SourceTV demo has one every 4 to 10
ticks (a player's own demo has one every tick), so a seek can land that far past. The rewind keeps the
gaps the skips cross, and the scrub aims short by the largest of nearly all of them.

`health` shows it as `rewind: in place (...); 26 in place, 0 reloads; seeks land up to 7 past (3407
packet gaps); last rewind 1251 -> 400: 84 packets in 8.8 ms over 1 frame(s)`, and `mvm_debug` logs
each seek as `demo.rewind`.

Checked in CS:S on 2026-10-27 in the SourceTV test demo. A seek from 2503 back to 600 replayed 125
packets in 14.5 ms, and the next frame already showed tick 604, with no loading screen or menu in
between. After a seek back from 1500, all 183 `players` and `entities` rows at tick 1206 matched a
fresh load at 1206, handles included; only the first-person viewmodel's sway, which runs on the
clock, differed. Thirty seeks back and forth left no engine error and grew the game's memory by
2 MB, and a seek back while playing kept playing.

A seek back replays the demo from its start, so it costs more the later the tick: about 14 ms at
tick 600 of the test demo, roughly a millisecond per 50 ticks. A long match demo will need snapshots
along the way, as CS2's keyframed copies give it.

### Live scrub

Dragging a playhead moves the demo world with it, as in CS2: the demo bar's scrubber and the TIMELINE
dock's ruler. The rules and the driver are CS2's (`AfxHookSource2/Filmmaker/Movie/DemoScrub.cpp`,
[live demo scrubbing](../features/filmmaker_demo_scrubbing.md)). What they ask the game goes through
`DemoScrubHost.h`, which `SourceDemoScrub.cpp` answers.

Dragged forward, the world plays toward the playhead at a speed that closes the gap in about a tenth
of a second, and stops inside the tick under it. Dragged back, it seeks back a little short of the
playhead and plays the last ticks in, so the frame shown has been played. A release lands on the
whole tick. CS:S has no `demo_step_tick`, so the last ticks play at the step's speed with
`demo_pauseatservertick`. Its server tick is the demo tick plus an offset fixed per demo, which the
scrub learns from the client's clock while the demo is paused on a whole tick.

Cache, on the dock's toolbar, keeps the frames the camera path's take has played
(`SourceScrubFrameCache.cpp`, CS2's rules in `ScrubFrameCacheMath.h`), at the preview's size, in
D3D9 textures, up to 384 MB. A drag back over them shows them from memory while the world stays
where it is, and a release lands the world under the frame. A change to the settings, the path or
its shake starts a new take. `health` prints the cache's line, and `mvm_debug` logs `demo.scrub` and
`scrub.cache`.

Checked in CS:S on 2026-10-27 in the SourceTV test demo, with window messages posted to the dock. A
drag from 615 forward to about 720 glided and seeked forward. Dragged back to 512, the world rewound
in place 12 times, 12 to 15 ms each, each followed by a two-tick play-in, and the release landed on
512 exactly; no frame of the demo's start or the menu showed in between. After the camera path
played its shot (533 to 1421; 353 frames kept, 383 MB), a drag back from 1486 served 492 frames from
the cache while the world stayed at 1422; the release at 887 rewound in place in 15 ms and landed on
887.

### End of demo hold

As in CS2, playback stops just before a demo runs out, so it never reaches the stop record and
drops to the main menu (`AfxHookSource2/Filmmaker/Movie/DemoEndHold.cpp`, shared with CS2). What it
asks the game goes through `DemoEndHoldHost.h`, which `Filmmaker/SourceDemoEndHold.cpp` answers: the
demo's state, tick and length from the engine's demo player, its file from the demo index. Pressing
play at the hold pauses again, and a seek back plays on and re-arms it. CS:S has no end-of-match
line-up and no game phase to read, so CS2's earlier postgame hold never fires.
`mirv_filmmaker endhold on|off|toggle|status` controls it, and `status` prints the margin.

How far before the last tick it stops is the game's choice (`EndHoldHost_EndMarginTicks`). The hold
is checked once a frame, so the margin has to cover what one frame plays, measured over the last 32
frames. CS2 keeps a second (64 ticks), sized for 16x. CS:S queues its `demo_pause` before the next
frame reads the demo, so it stops three frames' worth of ticks before the end: never under 8 ticks
and never over 64. At normal speed that is 8 ticks, about a tenth of a second.

Checked in CS:S on 2026-10-27 in the SourceTV test demo (4410 ticks): at normal speed it held at
4402 (it used to hold at 4346, a second early). At 16x a frame played up to 5 ticks, the margin
grew to 15, it held at 4396, and play from there paused again by 4401. The demo stayed loaded both
times.

### Bodies made during a seek

Every seek, the bar's and a typed `demo_gototick` alike, is the engine skipping through the demo,
and CS:S's own client drops a body that is made during a skip. The `CCSRagdoll` client class
stores the engine's `IsSkippingPlayback` when the body is created, and `C_CSRagdoll::OnDataChanged`
then releases the entity on its first update. The body is a networked entity, so the engine still
has it in its last snapshot, and the next packet that keeps it stops the game with
`Host_Error: CL_PreserveExistingEntity: missing client entity N` (one that changes it:
`CL_CopyExistingEntity`). So any seek across a death could stop the game; in the SourceTV test
demo, entity 62 is the body of the kill at tick 490. Read from the 64-bit `client.dll` on
2026-10-27: the create function (`client.dll+0x1edad0`) is the only caller of `IsSkippingPlayback`
there, and `OnDataChanged` (`client.dll+0x1ec060`) does the release. The game without the hook
stops the same way: a `.vdm` demo action that sent `demo_gototick 600 0 0` at tick 300 ended in
`Host_Error: CL_PreserveExistingEntity: missing client entity 62`.

The hook keeps those bodies (`SourceDemoSkip.cpp`). It wraps the ragdoll class's create function,
and while that runs, the engine client's `IsSkippingPlayback` (slot 128) answers false on that
thread, so the body is made as it is in normal playback. The game profile names the class. `health`
shows it on its `demo skip:` line, for example `CCSRagdoll kept when made during a skip (create
client.dll+0x1edad0, IsSkippingPlayback engine.dll+0x70960), 3 kept`, and `mvm_debug` logs each
body under `demo.skip`. A body made during a seek starts in the player's last pose and falls once
the demo plays on, where in normal playback it would already lie on the ground.

A skip that stops on that error leaves the engine still skipping: `CDemoPlayer` clears its skip
target only when a skip arrives (`engine.dll`, `m_nSkipToTick` at +0x5bc), not when a demo starts.
Without the fix, the next `playdemo` of the demo jumped from its first ticks towards the old target
and stopped on a body that is there from the start, with no seek sent.

Checked in CS:S on 2026-10-27 with the SourceTV test demo, each seek crossing a death and playback
then running on, all without an error: a typed `demo_gototick 600 0 0` from tick 314; the bar's
seek to 600 while paused and then to 900; the follow camera's Go To Event from tick 272 to 4210;
real clicks on +5S (328 to 663 across the kill, and 591 to 926), -5S (2170, restart, then 1835),
a scrubber click from 2336 to 4058 and one from 2776 back to 194; and the engine's own backward
`demo_gototick 300` from 1349, which keeps the five bodies that are there from the start. After the
seek to 600 the body of the tick-490 kill is in the entity list, in view, and on the ground 40
ticks later.

### Entities a demo deletes while still sending them

A delta packet's entity updates end with a list of entities to delete, and the engine deletes each
one (`CClientState::ReadDeletions`, `engine.dll+0xa3620`, then `CL_DeleteDLLEntity`). The SDK only
asserts that a listed entity is not one the packet's new frame still sends; the x64 engine has no
check. CS:S's SourceTV breaks that rule. In the grenade test demo, the packet at the second
`round_start` (tick 6396) lists four `CWeaponGlock`s lying on the ground, and a fifth weapon, for
deletion while the same frame keeps them. The engine deletes them, and the next packet, which keeps
them unchanged, stops the game with `Host_Error: CL_PreserveExistingEntity: missing client entity
71`. The same happens to a grenade in the air when a recording starts (entity 93, six seconds in) and
to bodies and weapons later in a round. So any SourceTV demo longer than a round could stop. The game
without the hook stops the same way: started directly, with a `.vdm` demo action playing the demo at
4×, it ended on `missing client entity 71`. `mirv_filmmaker entities releases 71` showed who deleted
the Glocks: `engine.dll`'s `CL_DeleteDLLEntity` (`+0x880b0`), called from `ReadDeletions`, not the
client. The CS client mode's `round_start` handler releases only the hostage ragdolls it keeps
itself (`g_HostageRagdolls`).

The hook keeps those entities (`SourceDemoDeletions.cpp`). It wraps `ReadDeletions`, the
`CClientState` virtual that passes `"ReadDeletions"` to `CL_DeleteDLLEntity`, found by that string
so a game update that moves the slot is still found (`CClientState` slot 28 today). Before the list
is read, it reads the list ahead from the packet: a set bit and an 11-bit index per entity. For an
entity the new frame still sends (`CClientFrame::transmit_entity`), the client entity list's
`GetClientNetworkable` answers null on that thread while the list is read, and `CL_DeleteDLLEntity`
leaves an entity it cannot find alone. Every other deletion goes through unchanged. `health` shows
it on its `deletions:` line, for example `entities a packet deletes while still sending them are kept
(ReadDeletions: CClientState slot 28, engine.dll+0xa3620), 9 kept`, and `mvm_debug` logs each one
under `demo.deletions`.

Checked in CS:S on 2026-10-27, without an error: `mvm_css_nades` played at 4× from its start to its
end (15036 ticks, both round starts), keeping nine entities; the recording that starts with grenades
in the air played to its end, keeping entity 93 at tick 69 and 24 more; and in `mvm_css_nades`
a `demo_gototick 6700` from the start, a `demo_gototick 3000` back and a `demo_gototick 9000`
forward, each across the round start, all played on.

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
| Left / Right | Five seconds back and on, as the bar's -5S and +5S |
| F8 | Blackout: no HUD, crosshair or spectator bars (`cl_drawhud 0`), no bar, no cards. Again for everything back. With the cards switched off, F8 brings them back |

The director stands aside while the game's menu or console is up (`IEngineVGui::IsGameUIVisible`,
`Con_IsVisible`), leaving the game the mouse and the keys. When the demo ends, the blackout, the FOV
and the free cam speed go back to the game's. Demo playback accepts `cl_drawhud` even when the
demo's server had cheats off. Ctrl+wheel's FOV is the hook's view FOV override (`mirv_fov`'s),
because `demo_fov_override` does not reach SourceTV's camera.

SourceTV's camera resets whenever a demo starts. The director remembers the camera mode and the
player it follows, twice a second, and after a start or a seek back puts both back (`spec_mode`, `spec_player <user id>`), as CS2 keeps the watched player across a seek. A demo
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
`tiltInv`, `falling`, `jumping`, `healthSway`. Its card is in the [Config workspace](#config-workspace).
The settings are [saved between sessions](#saved-settings).

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

Input that CS:S hands the window procedure on its second thread (it does while a demo loads or
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

### The TIMELINE dock

The Camera Editor's TIMELINE dock edits the camera path as keyframes, one row per camera value, as
CS2's does. The keys and their curves are CS2's own model (`GraphExpModel`, shared with CS2), and
`SourceTimelineDock.cpp` keeps them and the path in step, as CS2's `GraphEditorExperimentHud` does:

- Every edit writes the keys back into the path: one marker per whole tick, with each value's curve
  handles, so the camera flies what the dock draws. One drag or one click is one undo step on the
  path.
- A path change the dock did not make (K, the PATH CAMERA tab, an undo, a project load) is read back
  into the keys every frame. Keys that still exist keep their curves and selection. The dock's own
  undo history is dropped then, so an undo can never write an old path back.
- The playhead is CS2's (`SourceTimelinePlayhead.cpp`). While it is dragged it is the dragged tick and
  the world follows it (the [live scrub](#live-scrub)). On release the demo lands on that tick, and
  the playhead holds it until the demo is there and while it sits there.
- AUTO, the CAMERA PATH layer's button: a scrub inside the path's keys shows the path's camera at the
  playhead, and playback through the keys flies the path. Before the first key and after the last the
  view is the game's, and a seek in progress does not count as playback. An edit, and a release until
  the demo lands, show the path's camera with AUTO off too.

`mirv_filmmaker grapheditor` keeps CS2's verbs for automation: `on|off|toggle`, `drive`, `autopath`,
`select <ch> <id> [add]`, `selset <ch> <id> ...`, `selall`, `selclear`, `editbegin`,
`movesel <dTick> <dValue>`, `addkey <ch> <tick> <value>`, `delsel`,
`handle <ch> <id> left|right <tx> <dv> [reflect]`, `ease in|out|inout [all]`, `smooth|linear [sel]`,
`align <ch> <id>|playhead`, `undo`, `redo`, `reseed` and `playhead <tick>|release`. The channels are
0 x, 1 y, 2 z, 3 pitch, 4 yaw, 5 roll and 6 FOV. Each verb prints the dock's `health` line and every
key, with `*` on the selected ones. `mvm_debug` logs `graph.adopt`, `graph.sync`, `graph.drive`,
`graph.playhead`, `playhead.hold` and `campath.auto`.

Checked in CS:S on 2026-10-26 in the test demo, from the console with `mvm_debug`: three markers
placed with `marker place` became three keys on each of the seven values; moving the middle X key by
100 moved marker 1 by 100; Easy Ease on every key and two undos put the path back, and the log showed
no read-back of the dock's own writes; with AUTO on, a scrub to the moved key flew the view there and
halfway between showed the halfway point; a release backwards from 1500 to 1150 held 1150 through the
restart; playback from 900 flew the path from 1000 and gave the view back at 1600.

The dock itself is `Ui/TimelineDock.cpp`, CS2's dock (`GraphEditorJs.h`) in CS:S paint, with its
rules in `Ui/TimelineDockMath.h` (`SourceTimelineDockMathTests`):

| Part | What it does |
| --- | --- |
| Ribbon | REGULAR TIMELINE and TIMELINE, 150 wide; on TIMELINE, how many layers and keys it holds |
| Toolbar | The timecode and tick; KEYFRAMES and GRAPH; the selection; previous and next key (on the selected row, else every key), play, CS2's speed dial; the interpolation strip (Smooth, Linear, Ease In, Ease Out, In/Out: with keys selected it shapes them, with none Smooth and Linear set the whole path); SNAP with its menu (Auto, 1, 10, 100, 1000 ticks, Off; Alt places finely); Undo, Redo and Delete |
| Overview | The whole demo, its keys, the playhead and the window: drag the box to scroll, an end to zoom, empty track to jump |
| Ruler | Labels about 70 px apart. A press scrubs; past either end the window creeps; release seeks |
| Rows | CAMERA PATH with its key count, AUTO and the switch (on: scrubs and edits show the path; off: the free cam stays yours), Transform, Position, Rotation and Field of view with their values at the playhead in each channel's colour, and the previous, add or remove, next buttons |
| Values | Drag a value to scrub it (1 unit, 0.25 degree or 0.1 FOV a pixel; Shift a fifth as fast), click it to type one (Enter or a click elsewhere keys it, Esc leaves it). It is keyed at the snapped playhead; the first key at a tick keys all seven channels at the values they have, as CS2's does, so a camera key is always a whole camera |
| Menus | Right-click a key: its values and place, Go to this key, Delete Keyframe, Align selected keys here, and the Keyframe Assistant (Easy Ease, In, Out, Smooth, Linear). Right-click a row: Separate Dimensions (X, Y and Z, or pitch, yaw and roll, on their own rows) and Join Dimensions, Add or Re-key at playhead, Select all keys, Turn layer off or on |
| Track | A glyph per key time whose halves show how the curve meets it (diamond Linear, disc Smooth, notch eased), the layer's bar. Click a key to select it, Shift adds, drag moves (3 px, snapped), a double-click goes to it, a drag on empty track box-selects, the layer's bar moves every key |
| GRAPH | The curves of the selected row (the whole layer with none), each on its own value scale with a legend: the grid, the first curve's values, the keys, and the selected keys' handles. Drag a key to move it in time and value (Shift for time only), a handle to shape its side of the curve (Shift levels it; it stays 16 px out from the key), a box to select. Right-click a key for its menu. Each scale refits between gestures, never under the pointer. Show in Graph on a row's menu opens it on that row |
| Footer | The visible span, FIT (the selection, else the row, else every key; again for every key), zoom out and in, and the scrollbar |

Ctrl+wheel over the dock zooms about the pointer, Shift+wheel pans, the wheel scrolls the rows. The
dock takes Ctrl+Z, Ctrl+Shift+Z, Ctrl+A, Delete and F9 (Shift+F9 eases in, Ctrl+Shift+F9 out), as
CS2's does. Esc closes a menu, then the SNAP menu, then leaves typing, before it closes the
workspace. Tips show after the pointer rests 2 s. The camera path's shake rows are described under
[Path shake](#path-shake). CS2's
frame cache and render range are steps 13 and 14, so Cache is disabled and the footer says the whole
demo. The toolbar's CAM EDITOR and CONFIG buttons swap workspaces, the open one lit, and are left out
when the dock is too narrow for them beside the tabs. The window, snap and the rows' state are not kept between sessions:
the [settings store](#saved-settings) holds only the camera feel so far. CS2 clamps a typed camera value to the GRAPH's display
range around the channel's keys, so 500 typed into an unkeyed X lands near 32; the CS:S dock keys
what was typed.

Checked on 2026-10-26 in the browser mockup at 1600x900 (headless Chrome over the DevTools
protocol, because the browser pane had no WebGL) and in CS:S at 1600x900 with posted window
messages, from captures: a click selected a Position key's three channels; a 49 px drag moved them
to tick 1348.2 and the path split at 1300 and 1348 as CS2's bake does; F9 gave them the In/Out
glyph; two Ctrl+Z put the path back; SNAP 100 made a drag land on 3400 in the mockup; SNAP's menu
opened over the dock and Esc closed it with the editor still open; Ctrl+wheel zoomed, FIT framed the
selection, Delete and Ctrl+Z round-tripped, the FOV row's add button keyed the live FOV at the
playhead and revealed it; with AUTO on, the playhead held on the ruler at 1210 flew the view to the
path's pose there with the demo still at 1150, and the release seeked to 1210 and held it.

The menus and values were checked the same way: a right-click on a key opened its menu at the
pointer and the Keyframe Assistant's submenu opened beside it, kept inside the dock; Easy Ease from it
eased the key; Esc closed the menu with the editor still open; Separate Dimensions from Position's
row menu split it into X, Y and Z rows; a 40 px drag on X keyed all seven channels at the playhead
(1150 in game, X 40 units on) and put a path marker there; typing 2000 into Y set that marker's Y; two
undos took the path back to its three cameras.

GRAPH was checked the same way: on the layer it drew all seven curves, on Position X, Y and Z; a
selected key showed both handles; dragging the key moved its value as one undo step; dragging its
right handle up bent the curve into an eased bulge and the strip read Ease Out; in CS:S, with AUTO on,
the camera path's X at tick 1350 went from -1913.3 (the straight segment) to -1871.2 after that drag,
so the path flies the curve GRAPH draws. A drag on the layer's bar moved all 21 keys.

The same pass fixed where pop-ups sit: ImGui puts a new panel at the bottom of its stack, so the
CAMERA MARKER card and the clear-all dialog only took clicks where no panel covered them. They now
open as overlays (`BeginOverlay` in `Ui/UiWidgets.h`), as the SNAP menu does.

## Follow camera

CS2's follow camera runs in CS:S from the same code: its behaviour, target selection, state JSON and
console command (`AfxHookSource2/Filmmaker/Movie/Follow*.cpp`, `Filmmaker/FollowCommand.cpp`) compile
into both hooks. What it asks the game goes through `FollowCameraHost.h`, which
`Filmmaker/SourceFollowCamera.cpp` answers from the [engine adapter](#engine-adapter). Step 11 brings it
over in parts; so far, Lock-on and Attached on players, weapons and the C4, from the console and
the FOLLOW CAMERA tab.

- Targets are the players on a team, read with `Entities_Snapshot`. A living player is followed at its
  eyes: its origin raised 64 units, 47 while ducking. A dead one is followed at its `CCSRagdoll`.
  Lock-on aims at those eyes, not at the head bone, so a running player's head leans a few units
  ahead of the aim point.
- Weapons and the C4 are the profile's weapon and bomb classes, named as CS:S names them (AK-47,
  M4A1, MP5 Navy...). A carried weapon or C4 is its owner's (`m_hOwner`); the planted bomb,
  `CPlantedC4`, is nobody's, though it links to its planter. A weapon is followed at its origin: where
  it lies, or at its owner's hips while carried, where CS:S puts a carried weapon (the owner's origin
  raised 31 units). As in CS2, a weapon picked from a player's hands follows whatever that player
  holds, through a switch, and a player's death can hand the camera to the weapon or C4 they dropped.
- The Events list is the [demo index](#demo-index)'s: each death (CS:S records no weapon drop, and a
  death is where the victim's weapons fall), item pickups, and the C4's drops, pickups and plant,
  named by the player's name. Go To Event seeks 40 ticks before it, as the demo bar seeks.
- Lock-on is solved where CS2 solves it, in the view hook once per rendered frame, against the target
  where the game draws it. `SourceFilmmaker::OnViewOverride` gives the follow camera the view before
  Smooth View and the path markers, over HLAE's free camera, which the follow camera turns on.
- Attached rides a mount, with CS2's mount names and CS2's framing. A player's head, chest, pelvis
  and hands are bones of its skeleton; its eyes are the eyes above, looking where the player looks;
  its feet are its origin, turned the way it faces. A dead player's mounts are its ragdoll's bones,
  and its eyes become the ragdoll's head. A weapon's muzzle is its world model's `muzzle_flash`;
  a knife, the C4 and anything else without one ride the weapon itself, as CS2 falls back, and
  `targetprobe` names the source of each mount.
- A bone's own axes run along and across it, and each ValveBiped bone turns them its own way. The
  mount table (`kPlayerMounts`) names which bone axis points where the body faces and which points
  up, so every mount's frame faces the way the body does and an offset behind the head stays behind
  it. Measured on a running CT: head (-y, x), chest (y, x), pelvis (z, y), right hand (x, -z), left
  hand (x, z); every pair lined up with where the player looked within 40 degrees.
- Reposition works as in CS2: the free camera flies to the new spot and a click saves the offset
  against the mount. Esc cancels it. Either way the mouse goes back to the editor.
- While the follow camera is Live, the camera path's playback stops, AUTO and the TIMELINE dock's drive
  stand aside, and the path's markers are not drawn. Closing the Camera Editor stops Live, as in CS2.
- The FOLLOW CAMERA tab is CS2's follow inspector (`Ui/FollowInspector.cpp`): the pinned bar with Live
  and Stop, the CAMERA & TARGET card (mode, Place, Reposition, Clear, type, target list with Go To,
  Select Nearest and the far-target question, mount, events) and the FRAMING & TRACKING card with its
  two pages. It reads the follow camera's state JSON, the payload CS2's inspector reads, and every
  control sends CS2's command, which `mvm_debug` logs as `follow.ui`. Rows that belong to another mode
  or target type are left out, as in CS2. The card's summary shows the target's distance, which
  CS2's reads from a field it is never sent.
- In the world, as in CS2: the placed Lock-on camera is drawn as a camera marker in the follow
  purple while it is not live, and a picked target gets a cyan glass box for five seconds, standing
  on a player's feet or around a weapon. Both are part of the path overlay (`Ui/PathOverlay.cpp`),
  so they show while the Camera Editor is open, and neither shows over a live shot.
- Grenades are CS2's Grenade type. While the Camera Editor is open, every frame of playback looks
  for thrown grenades, and the list holds each one seen this session, in the air now or earlier,
  nearest the playhead first, with its kind, thrower and throw tick. CS:S does not network where a
  grenade was thrown from, so the throw tick comes from its flight: until its first bounce a
  grenade keeps its throw's sideways speed while gravity (0.4 of `sv_gravity`) slows its climb, so
  its launch and current upward speeds give how long it has flown (`FollowGrenadeFlightSeconds` in
  `FollowCameraMath.h`). After a bounce only the tick it was first seen at is left, and a grenade
  keeps the earliest tick it was ever dated with. Go To Throw seeks 40 ticks before the throw,
  plays, and takes the grenade when it is thrown: the same handle, or the same kind thrown within
  four ticks. Lock-on and Attached follow the grenade itself, which has no other mount.
- `health` shows the follow camera's state, and whether its marker and box show, on its `follow:`
  line.

Checked in the SourceTV test demo on 2026-10-26: the placed camera's aim matched the look-at to the
target's eyes to a hundredth of a degree; the eyes CS:S's own first-person view sits at matched the
computed ones; the solve read a new interpolated position every rendered frame; a narrow FOV capture
put the target's feet under the aim point; and when the target died at tick 4310 the camera followed
the ragdoll, and a one-second death hold stopped it at tick 4376, one second of playback later.
The tab, checked in the browser mockup at 1600x900 and 854x480 and then in CS:S at 854x480 with real
window messages: Place, the target list, a pick, Live and Stop each sent its command and the card
followed the state; the camera kept the picked player framed while the demo played; switching to
PATH CAMERA kept it live; Esc closed the editor and stopped Live. Weapons, in the same demo: the
list named every weapon with its owner, the carried C4 and later the planted one; the camera on
Josh's gun followed it from the USP to the M3 and on through the next round's buy; and with Retarget
dropped weapon on, Brad's death at tick 4310 handed the camera to his dropped M4A1 at 4376, after the
one-second hold. The Events list held the demo's three deaths and the plant with their players. The
in-world aids, in the mockup and then in CS:S: the placed camera's marker drew where it was placed,
seen from elsewhere; picking Brad boxed him on his feet and the box went after five seconds; Live
hid both, and Stop brought the marker back.
Attached, checked on 2026-10-27: `targetprobe` resolved every player and weapon mount from its bone
or attachment, with no faults. On Brad's head the camera moved with the head every frame, within a
unit; turned around (`rotyaw 180`) it kept his face in frame while he ran. Through his death the
head mount switched to the ragdoll's bone and followed the head down about 52 units. A Reposition
placed with a real click saved the offset (-47.6, 63.8, 17.5), the live camera came out exactly where
the free camera had been, and the framing held while the demo played; Esc cancelled another and gave
the mouse back. On his USP the muzzle read the model's `muzzle_flash`, about 19 units ahead of the
gun's origin.
Going to an event sends the right seek, and since the hook keeps bodies made during a seek
([Bodies made during a seek](#bodies-made-during-a-seek)) the demo plays on from it.
Grenades, checked on 2026-10-27 in `mvm_css_nades` at 854x480: playing round 1 with the editor
open listed its four throws, each named by kind and thrower and dated two or three ticks before
it was first seen (its flight already 0.030 to 0.045 s old). A seek straight into Yahn's HE, at
tick 503, dated it from its flight alone: its upward speed had fallen from 14.4 to -96.0 units/s,
0.345 s at 320 units/s^2, which is tick 480, the tick forward playback gave, under the same
handle. Go To Throw on it sought to tick 440, played, and took the grenade at tick 483 by its
handle. The placed Lock-on camera turned to keep it in view as it flew and rolled, and held its
last aim after it went off. Attached rode it, turning as the grenade spins. With Will's HE in the
air at tick 760, the Target list held all four throws, Will's as the active projectile. Each row
wraps onto three lines at 480p, and the list grows to show them whole. Before, a list of wrapped
rows ended a line short, because it laid them out a scrollbar narrower than it measured them.

## Camera shakes

CS2's camera shake layers run in CS:S from the same code: Handheld, Weapon Recoil and Weapon Bob,
the weapon pivots and the stack that tracks the view and sums them
(`AfxHookSource2/Filmmaker/Movie/CameraShakeStack.cpp`; [filmmaker_camera_shake.md](../features/filmmaker_camera_shake.md)).
What the stack asks the game goes through `CameraShakeHost.h`, which `Filmmaker/SourceCameraShake.cpp`
answers. They run from the console, with CS2's commands: `mirv_filmmaker handheld`, `weaponrecoil`,
`weaponbob`, `pivot`, `pivotorb` and `camshake`. Handheld, Weapon Recoil and Weapon Bob also have
their cards in the [Config workspace](#config-workspace).

- The shake rides on whatever has the view, after the follow camera and Smooth View, with CS2's
  gate: the game's own view (first person, or SourceTV's camera) and an attached follow camera are
  shaken, and HLAE's free camera, a camera path and a placed Lock-on camera are not.
- The watched player is the one SourceTV's camera follows in first person. Its gun's class name is
  made from the network class, as CS2's recoil table and gun test read it: `CWeaponM4A1` and `CAK47`
  are `weapon_m4a1` and `weapon_ak47`. The recoil table has CS:S's own guns too (scout, SG 550,
  SG 552, M3, TMP, P228), tuned as their CS2 counterparts.
- CS:S's SourceTV demos carry no `weapon_fire` or `player_footstep` event. A 10-bot SourceTV demo
  held only `hltv_status`, `player_death`, `bomb_planted`, `round_end` and `round_start`. So a shot
  comes from the `TEFireBullets` temp entity the game sends every client for each shot, for its
  sounds and tracers. The hook wraps that class's `PostDataUpdate` (`IClientNetworkable` slot 7). The
  shooter is its `m_iPlayer` (+0x20) plus one, and the gun is the shooter's active weapon. The game
  profile names the temp entity. With no footsteps, Weapon Bob's step sync keeps its own clock.
- SourceTV sends no velocity for the watched player, so the body's speed is measured from its origin
  over the demo clock. Its flags give on the ground and ducking, as in CS2.
- The gun moves as in CS2 (`Filmmaker/SourceViewModel.cpp`). CS2 shakes the eye before it builds
  the viewmodel, so the gun rides the shake, and adds Weapon Recoil's and Weapon Bob's weapon motion
  to the viewmodel's eye inputs. CS:S builds the viewmodel inside `CalcView`, before the view reaches
  `SetupEngineView`, where HLAE's overrides and the shake run, so left alone the gun would sit where
  the unshaken eye put it and never kick. The hook detours `C_BaseViewModel::CalcViewModelView`
  (found by RTTI as the one slot that loads 0.1, its `ApplyShake(origin, angles, 0.1)`; slot 212 in
  the 2026 x64 client). While a shake layer is on it holds the call back, and the view override
  runs it once the shake is known: from the eye moved by the shake, less Handheld while its
  viewmodel is decoupled, plus the weapon's kick. That is one call a frame from this frame's
  numbers, as in CS2. A call no view override picked up is run at the next frame's start as the
  game would have. `health` reports it on its `viewmodel:` line.
- The pivot markers (`pivotorb recoil|bob`, or the PIVOT drawers' eye) show where Weapon Recoil and
  Weapon Bob hinge the gun. Each is CS2's wireframe sphere of three world-axis circles, with a faint
  wide pass under a bright thin one, amber for recoil and cyan for bob, at the point and size the
  shake stack publishes after its near-plane escape. CS2 draws it with depth testing off so the
  viewmodel cannot bury it. CS:S draws it in the same world overlay as the camera path (projected
  with the rendered view and laid over the frame), which comes to the same thing. A hinge low on
  the gun can sit below the frame: a pistol's (4 forward, 3.5 down) lands about 41 degrees under
  the view axis, as it does in CS2.
- The magazine is not read, so every shot counts. The settings are
  [saved between sessions](#saved-settings).
- `health` shows the layers and the shot source on its `shakes:` line, for example
  `handheld on, recoil on, bob on; shots from CTEFireBullets (m_iPlayer +0x20), 41 shots`.

Checked in CS:S on 2026-10-27 in the grenade test demo. With all three layers on and SourceTV in
first person, the stack tracked the view as a POV, the rendered view drew off the game's by about
half a unit and a degree of yaw with a degree of roll, and the shot hook saw round 1's 41 shots, each
named with its gun (TMP, M4A1, SG 552, Galil, AWP). It sent the watched player's AWP shot to recoil
and counted the other 38 as other players'. A Test Shot on the paused demo kicked the camera about
1.0° up, 0.7° sideways and 2.4° in roll, settling over about 60 frames. With HLAE's free camera on,
the stack's gate turned the shake off, and with the follow camera attached it shook as Attach.

### Path shake

A camera path's keys carry CS2's path shakes: a RAGE-style shake per key, run until the next key of
the path (the same style ramps to it) or to the end of the shot
(`AfxHookSource2/Filmmaker/Movie/CamPathShake.cpp`, evaluated by `RageShake`;
[filmmaker_camera_path_shake.md](../features/filmmaker_camera_path_shake.md)). What it asks the game goes
through `CamPathShakeHost.h`, which the CS:S camera path answers (`SourceCameraPath.cpp`): its keys,
the tick its playback drives the view at, and the demo's project. The shakes are saved in the demo's
`project.mvm` in their own `camshake` section, next to the path, and follow a retimed key. They
shake the view only while the path plays or scrubs, after the live layers. CS:S has no camera
objects, so the per-object tracks are unused. The console command is CS2's `mirv_filmmaker
pathshake` (`set <key> <style> [intensity] [speed]`, `style`, `intensity`, `speed`, `stop`, `clear`,
`clearall`, `applyall`, `styles`, `state`, `on`/`off`).

In the TIMELINE dock, as in CS2's, the CAMERA PATH layer has a Path group with a Camera shake row,
and once its track has a key, Intensity and Speed rows under it. The Camera shake row shows the style
running under the playhead (or Off), and Intensity and Speed the values there, eased between keys of
the same style as the view plays them. Each row's navigator steps between shake keys, and its diamond
adds or removes the key on the camera key under the playhead: a new key continues the shake running
into it, else takes the menu's first style (Hand) at intensity 5 and speed 50. On the track, a light
blue bar shows how far each shake runs, to the next key or to the last camera key; the keys are
squares on the Camera shake row and CS2's ease marks on Intensity and Speed, a Stop dim. A click on a
key selects it and moves the playhead there, Shift toggles it, a box picks shake keys with the
others, and Delete clears the selected ones.

Intensity and Speed values scrub and type like the camera's. Dragged, one sweeps its whole range
over 200 pixels (Shift a fifth as fast); clicked, it opens for typing. Either sets the shake under
the playhead: the one on its camera key, else the key whose shake runs there. A camera key without a
shake key gets a new one that continues the run. In GRAPH, the Camera shake row and the Path group
draw Intensity in orange and Speed in blue (an Intensity or Speed row only its own), each on its own
0 to 10 or 0 to 100 scale and eased as the view plays it, with a gap after a Stop and past the last
camera key. A click on a shake key there selects it and moves the playhead to it, and dragging it up
or down sets that value (Shift a quarter as fast). With no shake yet, GRAPH says to right-click the
Camera shake row.

A right-click on a shake row opens CS2's camera shake menu for the shake key under the pointer, else
for the camera key under the playhead. Its head names the key's style, strength and speed. Under it
are the five main styles and More shakes with the rest of RAGE's list, each style with Stronger,
Weaker, Faster and Slower, then Stop the shake here, Use on every camera key, and Remove this shake
key. Picking a style or a step keeps the menu open for the next pick. Off a camera key the menu only
says to add one first. A camera key's own menu carries the same menu under Camera Shake. Every edit
is a `pathshake` command. The run and ramp rules are CS2's dock model in `Ui/TimelineDockMath.h`,
tested on the fixture CS2's dock test uses.

Checked in CS:S on 2026-10-27 in the SourceTV test demo: a two-key path from tick 580 to 703 with a
Hand shake (`REPLAY_HAND_SHAKE`, intensity 8, speed 60) on its first key swayed the view by up to 2.5
units and 2 degrees through the shot (`mvm_debug` `pathshake.view`), and the demo's `project.mvm`
held the `camshake` section with `"game":"css"`. The dock's shake menu was checked the same day: a
right-click on the Camera shake row opened it on the first key's Drunk shake (strength 6, speed 40),
picking Hand changed the key to `REPLAY_HAND_SHAKE` with the same values and left the menu open, and
Esc closed the menu without closing the editor. In GRAPH, the Camera shake row drew the Drunk key's
intensity and speed out to the last camera key, a drag on the Intensity value set it from 6 to 9,
and dragging the key down in GRAPH set it to 5, with the playhead moved to the key.

## Config workspace

CS2's Config workspace is the Camera Editor's sibling: it shares the preview, the dock ribbon and the
docked editor, and only the inspector column changes (`Ui/ConfigInspector.cpp`, drawn by
`Ui/CameraEditorShell.cpp`; [filmmaker_config_panel.md](../features/filmmaker_config_panel.md)). The
demo bar shows CAM EDITOR and CONFIG outside the workspaces and one button that swaps to the other
inside them, CONFIG in CS:S's CT blue. The TIMELINE dock's toolbar has the same pair, the open one
lit. `mirv_filmmaker config on|off|toggle|state` opens and closes it from the console, and ESC
closes it.

So far it has CS2's CAMERA tab with the cards whose effects run in CS:S. The rail groups them as CS2
does, each row with the card's summary and its master switch: MOTION holds SMOOTH VIEW and HANDHELD
CAMERA, and WEAPON holds WEAPON RECOIL and WEAPON BOB. The pane shows the selected card with CS2's header: the edge in
the card's colour (dim while it is off), the summary, Reset (dim at the defaults) and the switch.
Every row is CS2's, with CS2's label, tooltip and command. A slider is dragged from where it was
across one bar width, Shift a fifth as fast, and the minimum of an "Off at zero" slider reads Off.
A switch row flips on a click anywhere on it. A tooltip shows after the pointer rests 0.18 s on a
row, left of the column. RESET CAMERA in the pinned footer sends `mirv_filmmaker effects reset
camera`, which resets Smooth View, Handheld, Weapon Recoil, Weapon Bob and both pivot tables.

WEAPON RECOIL has CS2's Test Shot in its header, and T fires one too while Config is open. Above
its sliders are CS2's scope controls. Applies to picks All, a slot or a type, and a slot or type
starts on the gun in shot's own. Style picks Springy or Snappy, and a line says what the scope
reaches: that it inherits until a slider moves, that it owns its settings, or that it does not
cover the gun in shot. A scope that owns settings has Clear Override. Preset lists the saved
tunings, each with an x to delete it. Save turns the field into a name to type, and Enter or Save
again saves it. WEAPON RECOIL and WEAPON BOB end in CS2's PIVOT drawer, shut by default. Its head
says the class in shot, its gun and its hinge. Under it are the seven classes, of which only the
class in shot can be edited, and its Forward, Right and Up from -30 to 30 units.

What the cards show is read from the modules each frame (`SourceConfig.cpp`), through the same
`AppendStateJson` calls CS2's Config panel is built from, so a card says what CS2's says. What
differs from CS2:

- Smooth View has no Fisheye Lens row and RESET CAMERA leaves out the lens, the crosshair, the
  viewmodel and the shot FOV: those come with the [Lens](port-plan.md) step.
- VIEWMODEL, the lens cards, the other tabs, the search box and the GAME UI chip are not there yet,
  and the selected card and the PIVOT drawers' open state are not saved.

`mvm_debug` logs each command the cards send (`config.ui`) and each workspace change (`ui.editor`).

### Saved settings

The camera feel is kept between sessions, as CS2 keeps it: Smooth View, Handheld Camera, Weapon
Recoil with its presets, and Weapon Bob, with Keep between demos. They go to
`%APPDATA%\HLAE\filmmaker_settings_source1.json` (`Filmmaker/SourceSettings.cpp`). It is a file of
its own because CS2's store rewrites `filmmaker_settings.json` from its own modules, and the two would
drop each other's settings. Both games write and read the four groups through the same code
(`AfxHookSource2/Filmmaker/Config/CameraFeelSettings.cpp`), so a group looks the same in either
file.

As in CS2, the modules' defaults are kept before the file is read, a change is written 500 ms after
the last edit, and the file is written once more when the game shuts down. The weapon pivots and
their markers stay for the session, as in CS2. `mirv_filmmaker settings save` writes now, `settings
reset <smoothView|handheld|weaponRecoil|weaponBob|all>` puts a group back to its defaults, and
`health` shows the file and its state on its `settings:` line.

Keep between demos sits at the left of Config's footer while a demo plays
(`mirv_filmmaker effects keep camera on|off|toggle`). On, a new demo keeps the camera feel. Off, each
new demo starts from the defaults, the same reset RESET CAMERA does, but the file never hears of it.
The file holds the saved feel until you change something, so turning keep back on, or restarting,
brings it back.


Checked in CS:S on 2026-10-27 in the SourceTV test demo: `config on` opened the CAMERA tab on SMOOTH
VIEW; its header switch turned Smooth View on; a 20-pixel drag on Freq X set 1.52x, which the card
read back from the module; a tooltip showed on the row; the Handheld row's switch turned Handheld on
with the module's own Drift 35% and Motion Curves 75%; the demo bar's CAM EDITOR swapped to the
editor; and RESET CAMERA turned both layers off.

## Render workspace

CS2's Render workspace renders a range of the demo to a video or an image sequence. In CS:S it opens
from the RENDER button at the foot of the Camera Editor's inspector (RENDER PROGRESS while a job
runs), or with `mirv_filmmaker render open`. BACK and ESC go back to the editor, and opening Config
closes it.

It has CS2's layout (`Ui/RenderWorkspace.cpp`, placed by `RenderLayout` in
`Ui/WorkspaceLayoutMath.h`). The JOB column on the left sums up the job, says what stops it from
starting, lists PREVIOUS RENDERS with Use, and holds START RENDER and CANCEL RENDER. The preview sits
in the middle. The RENDER column on the right has CS2's three tabs, RANGE & CAMERA, PASSES and
OUTPUT, the SOUND chip and BACK, and a footer with the playhead and Set In, Set Out and Clear. While
the workspace is open, the demo bar's In and Out flags show the render range in CS2's teal in place
of LOOP's region. They wait at the bar's ends until a range is set, and dragging one sets a custom
range.

The settings, the console command, validation, the take folder and its manifest, and the previous
renders are CS2's code (`AfxHookSource2/Filmmaker/Render/`). Where the games differ, that code asks
the game through `RenderHost.h`: the demo's bounds and tick rate, the camera path, the camera
sources, and the formats the game cannot write. CS2 answers in `RenderHostSource2.cpp` and CS:S in
`Filmmaker/SourceRenderHost.cpp`. `SourceRenderModel.cpp` fills the panel from the shared state each
frame.

The job is CS:S's own (`SourceRenderDriver.cpp`, behind CS2's `RenderSessionDriver.h`):

1. It saves `host_framerate`, `mat_queue_mode`, `cl_drawhud`, blood, the timescale, the pause, the
   playhead and the free cam. Then it sets `mat_queue_mode 0`, `demo_timescale 1`, and the HUD and
   blood the job asks for.
2. It seeks, paused, to 64 ticks before the range, then plays with `host_framerate` at the output
   frame rate, so one engine frame is one output frame.
3. Close to the range it applies the camera source. Camera Path flies the path with CS2's
   evaluator.
4. From the range's first tick, HLAE's screen recorder (`mirv_streams record screen`, in
   `AfxStreams.cpp`) records every frame. It takes the frame at the material system's SwapBuffers,
   before the filmmaker's UI draws at Present, and hands it to ffmpeg for video or to the classic
   writer for TGA and BMP. The job starts the recorder just before the first frame in range renders
   and ends it just before the frame after the last, so it holds exactly the job's frames.
5. When the writer has finished, it puts back what it saved, HLAE's recorder settings included,
   plays the done sound and lists the new take.

A take holds what a CS2 take holds: `manifest.json`, `beauty\everything.mp4` (or a folder of
frames for a sequence), `frames.csv`, and from HLAE's recorder `cam_main.cam` (when camera data is
on) and `audio.wav`.

The job uses HLAE as it is, with marked lines (`// SOURCE:MVM`) where HLAE needed to know CS:S
x64 or the job's folders:

- `addresses.cpp`: HLAE found the material system's render queue on x64 for TF2 only, so its screen
  recorder refused CS:S. CS:S x64 ships TF2's x64 `materialsystem.dll` (the same `CMaterialSystem`
  vtable, checked 2026-10-28), so it takes TF2's case.
- `AfxStreams.h` / `.cpp`: the recorder can write straight into the job's take folder instead of a
  new `takeNNNN` folder inside it, and put the screen into the Beauty pass's folder. CS2's recorder
  has the same two switches. The job also reads and sets the screen recorder's switch and settings
  through marked accessors, so it can put them back.

What differs from CS2:

- There is one pass, Beauty · Everything, with no HUD or with the game's HUD. HLAE's depth, matte,
  normal and baseFx streams are CS:GO code that the x64 build leaves out (`#ifndef _WIN64` in
  `AfxStreams.cpp` and `MirvCommands.cpp`), so HLAE has no Depth, Normal or Key pass for CS:S x64.
- EXR is greyed out, because the CS:S frame is already tone mapped.
- CS:S runs 66.67 ticks a second, which no output frame rate divides. A job's frame count is the
  range's length in frames rounded up, and validation warns that frames and ticks do not line up.
- The view can give one direction as -1 degrees one frame and 359 the next. The frame map
  (`frames.csv`) moves each row's angles by whole turns to within half a turn of the row before, so
  a camera imported from it does not spin.

`mvm_debug` logs the panel's commands (`render.ui`), the job's phases (`render.job`), the encoder
settings (`render.output`), the done sound (`render.sound`) and the take scan (`render.takes`).
`health` shows the job and whether HLAE's recorder is recording on its `render:` line.

Checked in CS:S on 2026-10-28 in the SourceTV test demo at 1280 x 720. RENDER opened the workspace on
RANGE & CAMERA. Set In and Set Out at the playhead set ticks 1500 to 1702, and the flags appeared on
the demo bar. START RENDER wrote `beauty\everything.mp4`: 183 frames of H.264 from NVENC at 60 fps,
3.05 s, with the manifest marked completed and 183 rows in the frame map. The new take then headed
PREVIOUS RENDERS. Dragging the flags set custom ranges. The same day, on HLAE's recorder: ticks 1500
to 1702 at 60 fps gave 183 frames of H.264 (3.05 s) with no UI or HUD in them, `cam_main.cam` and
3.04 s of `audio.wav`; and a PNG sequence of ticks 600 to 700 at 30 fps, flown on the camera path
with the game's HUD, gave 45 frames. Both manifests were marked completed, and HLAE's record name
was back to its own afterwards.

## Demos browser

CS2's demos page (the Watch page's Downloaded tab, which SOURCE:MVM takes over) is a full page here,
drawn by the hook over the main menu. The page is `Filmmaker/Ui/DemosBrowser.cpp`, with its list
rules in `Ui/DemosBrowserMath.h`; the library behind it is `Filmmaker/SourceDemos.cpp`.

- The main menu's DEMOS button, at the top right, opens it. The button is hidden while the console
  is open and while a map or demo is loaded. `mirv_filmmaker demos on|off|toggle|state` opens and
  closes it from the console, over a playing demo too. The page has the mouse and the keys while it
  is up (Alt combinations stay the game's, so Alt+F4 still quits). ESC closes what is open on the
  page (a question, a menu, a field, the search), then the page.
- It lists every demo under `cstrike`, in all its folders, and in the folders and files the user
  added. A worker thread scans when the game starts and each time the page opens. Each demo is read
  for its 1072-byte header only: map, length, ticks, who recorded it and the server. The date is the
  file's write time, because a CS:S demo has no match time. The converted copies of
  [old demos](#old-demos) (`source_mvm/converted`) are not listed.
- The rail has the sort (Recently played, Newest first, Oldest first, Map name, Demo name), the +
  menu (New folder, Add demo file, Add demo folder, and Show removed demos while some are removed),
  and the demos as tiles under the user's folders. A click on a folder header collapses it, and a
  right click offers Rename folder and Delete folder. A tile dragged onto a header goes into that
  folder. A tile's right-click menu renames the demo, moves it into or out of a folder, and says when
  it was last played. The demo played last has an orange edge.
- Old v34 demos carry a V34 tag: gold until their first play converts them, green once the converted
  copy is ready. A demo of another protocol, or one with no readable header, carries a ? and cannot
  be watched.
- The right side shows the selected demo: its name (click it to rename the file), its folder, Map,
  Length and Date, and what its header says (map file, recorded by, server, ticks and tick rate,
  version, last played, file). WATCH plays it. A loaded map or demo is left first, and for an old
  demo the page stays up with CONVERTING until its copy plays. The folder button shows the file in
  Explorer. The trash button asks, then removes the demo from the list. No file is ever deleted.
- SEARCH jumps to a demo by map, name, recorder or server (12 matches at most, opening a shut
  folder). REFRESH scans again. CLEAR ALL asks, then removes every demo from the list.

Renaming moves the `.dem` first. When that fails (for example the demo is playing), nothing else
changes and the page's header says why. Otherwise the demo's folder, last-played time and project
folder (`source_mvm\demos\css`) follow it to the new name, and the list is scanned again.

The user's side is kept by CS2's stores (`AfxHookSource2/Filmmaker/Config/`), under CS:S's own file
names in `%APPDATA%\HLAE\`, so the two games' lists stay apart:

| File | What it holds |
| --- | --- |
| `filmmaker_demo_folders_source1.txt` | Folders added to the scan |
| `filmmaker_demo_files_source1.txt` | Demos added one at a time |
| `filmmaker_hidden_demos_source1.txt` | Demos removed from the list. Adding their folder again brings them back |
| `filmmaker_demo_organizer_source1.txt` | The sort, the user's folders, and each demo's folder and last-played time |

Last played counts every demo that starts, from the page or from the console, checked once a second
as CS2 does.

`mirv_filmmaker demos` also takes the library's commands. `list` prints each demo's index, map,
length, protocol, whether it plays here, and file. `folders` and `hidden` print the scanned folders
and the removed demos. `addfolder [path]` adds a folder (Windows' folder picker without a path), and
`removefolder <i>` drops one. The page's own commands work too: `refresh`, `clearall`, `unhideall`,
`adddemo`, `watch <i>`, `hide <i>`, `openfolder <i>`, `renamedemo <i> <name>`, and CS2's organiser
lines `org sort|newfolder|rename|delfolder|collapse|assign`. `<i>` is the index `list` prints.
`health` has a `demos:` line, and `mvm_debug` logs the page's commands (`ui.demos`) and the
library's notes (`demos`).

Checked in CS:S on 2026-10-27: the scan found 64 demos in 273 ms. WATCH on `mvm_css_nades` played
it and closed the page. An old v34 demo (`DFS_fMR_dust2_3kAWP_OLD_POV`) converted in 20 ms, played,
and the page closed. Back at the menu, `mvm_css_nades` showed Last played "just now". From the
console, adding a folder listed the demo in it, a rename changed the file on disk, `hide` and
`unhideall` removed the demo and brought it back, `removefolder` dropped the folder, and the
organiser file held the new sort and folder.

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
  plays reuse the copy until the original or the converter changes. A demo outside the game folder
  is copied to `converted/outside-game-dir/`, under its drive letter and path. The
  [demos browser](#demos-browser) tags each old demo V34, green once its copy is ready.
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
| User commands | Rewritten in the current layout. The old engine wrote pitch and yaw as 16-bit angles, roll as an 8-bit angle and the three moves as 16-bit integers; the current one writes floats, and every other field is the same. A demo recorded in first person needs them: playback predicts the player's own gun from them, and without them the gun stays at the first frame of its draw animation, below and behind the camera, so no gun or hands show |
| Voice | Dropped. The 64-bit game has no Speex |

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

Checked on 2026-10-27: all 629,752 user commands of the 59 demos decode to exactly their recorded
length in the old layout, and the converted ones read back exactly in the current layout. In two
first-person demos (an AWP and USP demo on de_dust2 and a knife and AK-47 demo on cs_office) the
gun and hands now draw and animate where none showed before. The converter version went to 2, so
copies made before are converted again on their next play.

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

## Window resize

Drag an edge or a corner of the CS:S window to any size, as in CS2
([Docs/features/filmmaker_window_resize.md](../features/filmmaker_window_resize.md)). While you drag,
a card in the middle of the window shows the size and the closest named aspect ratio. Hold Shift to
snap to a named ratio, and onto a standard resolution of that ratio when one is close. When you let
go, the engine renders at the new size. The ratio table, the snap, the guard against Windows Snap
taking a Shift drag and the card are CS2's own (`AfxHookSource2/Filmmaker/Window/`); `WindowGeometry.h`
there holds the window arithmetic both games share. `Filmmaker/SourceWindow.cpp` is the CS:S side.

CS:S draws its own plain Win32 window with a fixed frame and no SDL, so the CS:S side differs from
CS2's in four places:

- **The border.** While the game is windowed, the hook adds a sizing border and keeps the client
  area exactly where and how big it was. It checks twice a second and puts the border back, because
  the engine sets the window's style again when it changes mode. Fullscreen and `-noborder` windows
  are left alone.
- **The commit.** When a drag ends, the hook asks for the new size with `mat_setvideomode w h 1`.
  The engine only sets a size it has as a mode: one of the display's modes, or in a window the
  custom size that `-w` and `-h` fill at startup. Any other size falls back to 640x480. So the hook
  first writes the size into that custom slot. The engine's video mode object is found by following
  `mat_setvideomode`'s own code to the pointer it loads before calling `SetMode` (engine.dll
  `+0x795358`, the slot at `+0x201C`). The hook only uses it when the mode it holds matches the
  engine's screen size. If it is not found, nothing is asked and the window goes back to the size the
  engine renders, so the picture is never scaled.
- **The place.** The engine centres the window on the main monitor when it sets a mode. For the
  commit's cooldown, the hook answers every move with the place the drag left the window, so it stays
  on the monitor you dragged it on.
- **The HUD.** The mode change lays the game's HUD out again and shows panels that stay hidden until
  used: the freeze cam's, the round's win panel, an achievement. The next tick the demo reads hides
  them, and a paused demo reads none, so a paused demo steps one tick at 0.05x and pauses again.
  SourceTV's spectator bar along the top keeps its old width until the observer mode changes, and
  only a change with frames in between lays it out again. In first person and chase, the hook
  switches to the other mode for four frames and back. Roaming and fixed cameras would lose their
  place, so they keep the short bar until the next camera change.

`mirv_filmmaker window status` and the `window:` line in `health` show the client size, the aspect,
what the engine renders, where the video mode was found, and how the last resize went. `mvm_debug`
logs each step of a drag (`window.resize`), the custom size written (`window.videomode`), and any
Windows Snap the Shift guard refused (`window.snapguard`). The minimum size is 640x360, and sizes are
made even.

Checked in CS:S on 2026-10-27 at 1280x720 with real mouse drags on the paused SourceTV test demo:

- A plain corner drag ended at 1500x681. It was committed as 1500x682, and the engine rendered it,
  with the window still on the left monitor.
- A Shift drag snapped through 2.20:1 to exactly 21:9, 1666x714. Another snapped to 2.76:1, 1656x600.
  The card read `1656 × 600 · 2.76:1 · Ultra Panavision 70 · SNAP` while dragging.
- `window size 1504 846` set 1504x846 the same way.
- Each paused resize stepped the demo exactly one tick, and its 1.00x speed came back. The frames
  afterwards showed no stray HUD panels and a full-width spectator bar.
- Before the hook wrote the custom size, `mat_setvideomode 1504 846 1` alone gave 640x480. Display
  modes (1600x900, 1366x768) worked, and each one moved the window to the main monitor.

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
| `UiStyle.cpp`, `UiWidgets.cpp` | The ImGui style, fonts at the stage scale, CS2's text tabs and chips. Verdana has no arrows or triangles, so each Verdana font borrows them from Tahoma at the same size |
| `CameraEditorShell.cpp` | The workspaces: preview, inspector tabs, dock ribbon, ESC EXIT, with the Camera Editor's or Config's column |
| `DemosBrowser.cpp`, `DemosBrowserMath.h` | The demos browser page and the main menu's DEMOS button; the list's order, folders, sorts, search and the text it derives |
| `RenderWorkspace.cpp` | The Render workspace: the JOB column, the RENDER column's three tabs and the playhead footer |
| `ConfigInspector.cpp` | Config's CAMERA tab: the rail with its switches, the cards, the curve lists, tooltips and the pinned footer |
| `InspectorCards.cpp` | CS2's inspector card grammar: rail rows with badges or switches, card header with an optional master switch, value, mode, action, switch and dropdown rows, key strip |
| `PathInspector.cpp` | The PATH CAMERA tab and its Clear all dialog |
| `FollowInspector.cpp` | The FOLLOW CAMERA tab: pinned bar, rail, CAMERA & TARGET and FRAMING & TRACKING cards, dropdown lists |
| `PathOverlay.cpp` | The camera path drawn in the world, and the follow camera's marker and target box |
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

Outside the UI's own rules the hook follows CS2's click-to-capture (`MouseCapture.h`). The game may
not hide, move or capture the cursor until a click in its window reaches the game, or a look or
GAME mode hands it the mouse. Losing focus frees it again, and so does the UI taking the mouse. So
a launch, a demo opening or Alt+Tab never take the cursor from another window. The game hides its
cursor every frame through `vguimatsurface.dll`, `client.dll` and the engine's `ShowCursor`, which
`main.cpp`'s import hooks leave out; `Ui_InstallCursorHooks` routes those through the same filters
at startup. `mirv_filmmaker health` prints a `mouse:` line: whether the game waits for a click, who
has the mouse, what was kept from the game, and which modules are hooked.

Checked in CS:S on 2026-10-27: before this, a demo starting from the main menu blanked the cursor
over the game for 10 to 30 ms at a time, six times in nine seconds with the game in front and once
with another window in front (the engine hides the cursor as the demo starts, before the director
shows it again). After it, the cursor stayed visible in all 1,146 samples of the same two runs. A
right-drag look, G in the Camera Editor and a click in the main menu each gave the game the mouse,
and putting another window in front freed it again (`input.capture` in the log).

`mvm_debug` logs every button the UI sees (`ui.input`), mode changes (`ui.mouse`), hover changes
(`ui.hover`), the font build (`ui.fonts`), open and close (`ui.editor`), the demos browser
(`ui.demos`), who may have the mouse (`input.capture`), and the cursor calls the filters kept or
passed, at most 8 a second (`ui.cursor`).

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
