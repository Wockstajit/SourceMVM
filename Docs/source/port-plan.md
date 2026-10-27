# Counter-Strike: Source port plan

The plan for bringing the whole CS2 filmmaker to Counter-Strike: Source (the 64-bit Steam build),
with a best-effort mode for other Source 1 games afterwards. This is a plan and a status page.
Anything marked done was checked in the game, and the evidence is in the step's commit.

Related: [README.md](README.md) (build, launch, console surface for the Source 1 hook).

## Goal

Everything the CS2 filmmaker does, in CS:S: the camera editor, timelines, smooth view, follow
camera, shakes, demo tools, the render workspace, lens, effects and the animation features. Two
exceptions: scene objects are not ported, and lighting comes last.

## Rules

**Parity with CS2.** CS:S moves and behaves like the CS2 filmmaker, not like the inherited HLAE
Source 1 commands. Where they differ, CS2 wins. That covers the mouse model (the cursor stays free
until you click into the game, holding the right button for 200 ms or moving it 4 px starts the
look, a left click picks and a left drag moves after 3 px), the keyboard shortcuts, the workflows
and the console command surface (`mirv_filmmaker <subcommand>`).

**One copy of the behaviour.** Engine-free CS2 code is compiled into the Source 1 hook from where it
lives, the way the Deadlock hook already compiles CS2 files. When a shared file reaches into CS2's
engine, it is split into the behaviour and a small engine adapter, and CS2 is retested. A fix in
CS2 then reaches CS:S without a second edit, and one test covers both games.

**Source-only code lives in the Source folders.**

| What | Where |
| --- | --- |
| Source 1 filmmaker code: engine adapter, input, UI, features | `AfxHookSource/Filmmaker/` |
| CS:S-only parts (offsets, player and weapon access, HUD) | `AfxHookSource/Filmmaker/CSS/` |
| Browser mockup of the Source 1 UI | `UiMockupSource/` |
| Launch and command scripts | `Automation/launch/launch-css.ps1`, `Automation/netcon/css-netcon.ps1` |
| Docs | `Docs/source/` |

Inherited HLAE files such as `AfxHookSource/main.cpp` only get small, marked calls into the
filmmaker, plus the fixes the 64-bit CS:S build needs.

**The UI is native C++ with the CS:S look.** The layouts follow CS2's workspaces. Only the colour
scheme and theme change. The browser mockup runs the same C++ UI compiled to WebAssembly, so it
cannot drift from the game, just as [UiMockup](../../UiMockup/README.md) runs the real CS2 scripts.
The [mockup-first workflow](../panorama/mockup-first-workflow.md) applies: review in the mockup,
then build into the game and compare real captures.

**In order, one step at a time.** Each step ends with a check that has to pass in the game before
the next step starts. A feature is not pulled in early because it is convenient.

**Testing.** The game runs offline, windowed at 854x480 on the left monitor (Windows DISPLAY3),
with D3D9. CS:S ships a Vulkan path (`dxvk_d3d9.dll`), which the hook does not support. Close the
game with the console `quit` command, never by killing the process.

## What the 64-bit CS:S build changes

Measured on the installed game on 2026-10-26:

- The game starts from `cstrike_win64.exe` with its binaries in `bin\x64`. The inherited x64
  Source 1 hook detected CS:S only by `cstrike.exe`; it now also accepts `cstrike_win64.exe`. Most of
  its CS:S-only code is still compiled for 32-bit only (`#ifndef _WIN64`) and is enabled as later
  steps need it.
- With the hook loaded, the engine hands out `VClient017`, `VEngineClient013` and `VEngineCvar004`,
  the same interfaces the hook's x64 TF2 path uses. The window title reads "Direct3D 9 - 64 Bit".
- `bin\x64\engine.dll` has no `-netconport` option. The hook runs its own command port on 29030
  ([README.md](README.md#command-port)). tier0 still exports `SpewOutputFunc` and
  `GetSpewOutputFunc`, so console output is captured by chaining the spew function.
- The client's `FRAME_RENDER_START` runs on the main menu as well as in game, and the input
  system subclasses the game window through `SetWindowLongPtrW`, which the hook already intercepts.
- Closing the window (WM_CLOSE) opens the game's own "Quit game?" dialog instead of exiting.

## Steps

| # | Step | Done when | Status |
| --- | --- | --- | --- |
| 1 | Build the x64 Source 1 hook as it is | `AfxHookSource` builds in `x64-release` | Done 2026-10-26 |
| 2 | Run the hook in CS:S 64-bit | A demo plays with the hook loaded and the inherited `mirv_` commands answer | Done 2026-10-26 |
| 3 | Automation | A script launches CS:S, gets `mirv_filmmaker health` back over the command port, and quits it | Done 2026-10-26 |
| 4 | Shared code and tests | Shared CS2 code builds into the Source 1 hook; native tests pass for both; CS2 still works; `mvm_debug` writes a log from CS:S | Done 2026-10-26 |
| 5 | Engine adapter | Health shows live view, demo time and player values during a CS:S demo | Done 2026-10-26 |
| 6 | UI foundation | The CS:S-themed UI clicks through in the browser mockup and draws in CS:S at 480p, with CS2's mouse model; captures of open, tab switch and exit | Done 2026-10-26 |
| 7 | Demo bar and director card | Scrubber with kill, death and round markers read from the demo file; speed dial, -5s/+5s, LOOP, CAM EDITOR and CONFIG; scroll cycles cameras; the director cards; F8 blackout | Done 2026-10-26, CONFIG with the Config workspace |
| 8 | Free cam and smooth view | HLAE's `mirv_input` flies as CS2's free cam does; Smooth View runs from CS2's code | Done 2026-10-26, Smooth View's Config card with the Config workspace |
| 9 | Camera Editor | Paths, markers, inspector, preview viewport, shortcut strip | Done 2026-10-26 |
| 10 | Timelines | TIMELINE dock, regular timeline, loop, In/Out flags | Done 2026-10-26; the TIMELINE dock's camera shake rows with step 12, its Cache with step 13 and its render range with step 14 |
| 11 | Follow camera | Lock-on and Attached; players, weapons, the C4 and grenades; death and lost-target policy; the FOLLOW CAMERA tab; the Events list; the in-world marker and target box | In parts. Done 2026-10-26: CS2's follow camera split into shared code and a game host, and Lock-on on players, weapons and the C4, with the Events list and the in-world marker and target box, from the console and the FOLLOW CAMERA tab ([README.md](README.md#follow-camera)). Done 2026-10-27: Attached on players' bones (living or ragdoll) and weapons' muzzles, with Reposition. Left: grenades, which need a demo with a throw |
| 12 | Camera shakes | Handheld, path shake, recoil, bob, rage | |
| 13 | Demo tools | Demos browser, scrubbing, timescale, end hold | |
| 14 | Render workspace | Range, camera, HUD and output tabs; passes; FPS; done sound; previous renders | |
| 15 | Lens | Depth of field, bokeh, flare, fisheye | |
| 16 | Effects | Config effects, view and weapon effects, particles converted for CS:S and precached | |
| 17 | Animation | Method acting, nervous system, ragdolls, death animations | |
| 18 | Smaller pieces | Onboarding, What's new, heat map | |
| 19 | Lighting | | |
| 20 | Other Source 1 games, then GoldSrc | | |

Steps 2 and 3 were checked together, because step 2 needed a way to send commands. There was no
CS:S demo on the machine, so the check recorded one with bots on de_dust2 over the command port
(`record mvm_css_test`, 40.8 s), played it back, and ran `mirv_campath add` and `print` against the
live view. `mvm_debug` moved into step 4: its logger is CS2 code that has to become shared first.

Step 4 set up the sharing, not a bulk copy. The Source 1 hook compiles CS2 files from the CS2 tree
under `GAME_SOURCE1`, beside Deadlock's `GAME_DEADLOCK`. The first ones are the `mvm_debug` logger
and its text helper. The logger's three engine reads (log folder, demo tick, frame) moved behind
`MvmDebugLogHost.h`; CS2 and Deadlock answer them in `MvmDebugLogHostSource2.cpp` with the same
calls as before, and CS:S in `AfxHookSource/Filmmaker/SourceDebugLog.cpp`. Later steps bring in
the CS2 math and models they use, when they use them. Checked on 2026-10-26: `NetconServerTests`
passes; the CS2 suite passes (123 tests, one hardware test skipped); CS2 and Deadlock build; CS2
and CS:S both write `mvm_debug` logs with console capture, and the CS:S log carries advancing
demo ticks during playback.

Step 5 added the engine adapter ([README.md](README.md#engine-adapter)). Checked on 2026-10-26 in a
live bot game and in the SourceTV test demo: 200 client classes and all 7 networked fields resolved
with no faults; 12 players with names, teams, health, origins and eye angles; with playback paused
at tick 1000 and the camera locked on one bot, the view sat at that bot's origin plus 64 units of
eye height, with its eye angles, and `getpos` agreed. The first live run caught two wrong virtual
slots: `GetAbsOrigin` and `GetAbsAngles` are 9 and 10, not 10 and 11, because `IClientEntity::Release`
overrides its secondary bases and takes no slot of its own. `SourceNetvarsTests` covers the
network-table walker, and the 32-bit Source 1 hook still builds.

Step 6, in game (2026-10-26): the Camera Editor shell draws in CS:S at 854x480 with the CS:S
theme, the game letterboxed into the preview. Captured and logged: open, the inspector tab and the
dock tab switching (TIMELINE re-centres the preview above the 440 px dock), a quick right click
staying a click, a 450 ms right hold becoming a look and returning on release, a left click in the
preview handing the mouse to the game, G taking it back, and ESC closing with no leftovers.

Step 6, browser mockup (2026-10-26): [UiMockupSource](../../UiMockupSource/README.md) compiles the
same UI files to WebAssembly. The same nine-stage walk passed in the browser, and its console log
matched the game's `mvm_debug` lines in order. Read back at 854x480 and compared with a capture of
the game in the same state, the inspector differed by 0.21 on average (of 255) and the ribbon and
dock by under 3. That comparison allowed a one-pixel shift: the game's client area measures
855x481 although the launcher asks for 854x480. The mouse rules and the key rules
(`WorkspaceMouse.h`, `UiKeys.h`) are shared by both hosts.

Step 7 is done in parts. CS2's demo bar is mostly CS2's own Panorama, with SOURCE:MVM's speed dial,
LOOP, CAM EDITOR and CONFIG added; its kill, death and round markers are CS2's, drawn from the
`.dem.info` sidecar Valve writes. CS:S has neither, so the port draws the whole bar and reads the
markers out of the demo file. First part (2026-10-26): the demo index
([README.md](README.md#demo-index)) reads a demo's events when playback starts, in 4 ms for the
test demo, with ticks that match the game's own kill lines. Second part (2026-10-26): the demo bar
([README.md](README.md#demo-bar)) in the REGULAR TIMELINE dock. Every control was clicked in the
browser mockup and in CS:S at 854x480 with real window messages, at the positions the bar reports:
play and pause, +5S (+335 ticks), -5S through the restart, NEXT KILL, < ROUND, the dial at 4x and
1x, the camera button, a scrub click, a kill marker, and LOOP wrapping from 763 back to 476. Third
part (2026-10-26): the [movie director](README.md#movie-director) outside the workspaces: the bar
along the bottom, CS2's two cards, its mouse (a click steps the player, a held press looks), the
wheel's cameras with Ctrl for FOV and Shift for speed, Space, Left, Right and F8. Checked in the
browser mockup and in CS:S at 854x480 with real window messages, against health and the SourceTV
camera's own mode and target, with captures: each input did what CS2's does; the camera opened in
first person on a demo that starts at the map origin and came back to the same player and mode after
a backward seek; CAM EDITOR, ESC and the game menu handed over and back. The CONFIG button comes
with the Config workspace. Not checked: looking around with a physical mouse.

Old demos, added during step 7 at the owner's request (2026-10-26): demos from the v34 engine
(network protocol 7) play on the 64-bit game. `playdemo` converts them once into the current format,
with their brush models matched to the installed maps, and plays the copy
([README.md](README.md#old-demos)). All 59 v34 demos in the test folder convert, and demos on
de_inferno, de_dust2, de_nuke, cs_office, de_cbble and de_train play in game in first person and
chase, with the demo bar's markers.

Seeks across a death, fixed during step 11 (2026-10-27): seeking the SourceTV test demo stopped the
game with `Host_Error: CL_PreserveExistingEntity: missing client entity 62`, from the demo bar, the
follow camera's Go To Event and a typed `demo_gototick` alike. CS:S's own client releases a body
that is made while the demo skips, and the engine then misses that networked entity
([README.md](README.md#bodies-made-during-a-seek)). The hook now keeps those bodies. Without the
hook, the game stopped the same way. With it, the bar's +5S, -5S, scrubber seeks forward and back,
Go To Event and typed `demo_gototick` seeks across the tick-490 kill all played on without an
error in CS:S at 854x480.

Step 17's death animations follow the CS2 pass in
[filmmaker_death_animations.md](../features/filmmaker_death_animations.md): headless Blender on the
CS:S player model, the clip tags ported, the ragdoll bodies mapped, and the match percentages
checked. CS:S players use the ValveBiped skeleton, the same family as the GMod donor pack. That is
checked in Blender at that step, not assumed.

Step 20 adds a core tier that needs only the interfaces every Source 1 game has (engine client,
render view, cvars, material system, D3D9). A game without a profile runs that tier and is labelled
best-effort. GoldSrc is a separate DLL (`AfxHookGoldSrc`) that reuses the engine-free code.

## Known follow-ups

- Old demos: demos of network protocols other than 7 and 24 (one test demo used 17) are not
  converted. [README.md](README.md#old-demos) lists the values the converter cannot carry over.

- The demo bar seeks back by restarting the demo itself and waiting for it to play before it seeks
  forward (`SourceDemoBar.cpp`), because the engine's own backward seek used to stop on dropped
  bodies. Since the hook keeps them, the engine's own `demo_gototick` back works too (checked
  2026-10-27). Moving the bar onto it removes the restart wait, and needs the bar, LOOP, camera
  path and TIMELINE seeks checked again.

- The command port's socket server is `Shared/NetconServer`, generalized from Deadlock's
  `DeadlockNetcon.cpp`, and the client is `Automation/netcon/hook-netcon.ps1`, generalized from
  `deadlock-netcon.ps1`. Deadlock still has its own copies. Moving Deadlock onto the shared ones is
  a separate change that needs its own Deadlock runtime check.

## Not ported, or asked about first

- Scene objects: not ported.
- CS2 skins, cosmetics and workshop maps have no CS:S counterpart. They are raised with the owner
  when their step comes up.
