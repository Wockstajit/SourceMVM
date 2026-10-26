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
| 5 | Engine adapter | Health shows live view, demo time and player values during a CS:S demo | |
| 6 | UI foundation | The CS:S-themed UI clicks through in the browser mockup and draws in CS:S at 480p, with CS2's mouse model; captures of open, tab switch and exit | |
| 7 | Demo bar and director card | MOUSE, CAM EDITOR, CONFIG and LOOP; scroll cycles cameras; F8 hides the HUD | |
| 8 | Free cam and smooth view | | |
| 9 | Camera Editor | Paths, markers, inspector, preview viewport, shortcut strip | |
| 10 | Timelines | TIMELINE dock, regular timeline, loop, In/Out flags | |
| 11 | Follow camera | | |
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

Step 17's death animations follow the CS2 pass in
[filmmaker_death_animations.md](../features/filmmaker_death_animations.md): headless Blender on the
CS:S player model, the clip tags ported, the ragdoll bodies mapped, and the match percentages
checked. CS:S players use the ValveBiped skeleton, the same family as the GMod donor pack. That is
checked in Blender at that step, not assumed.

Step 20 adds a core tier that needs only the interfaces every Source 1 game has (engine client,
render view, cvars, material system, D3D9). A game without a profile runs that tier and is labelled
best-effort. GoldSrc is a separate DLL (`AfxHookGoldSrc`) that reuses the engine-free code.

## Known follow-ups

- The command port's socket server is `Shared/NetconServer`, generalized from Deadlock's
  `DeadlockNetcon.cpp`, and the client is `Automation/netcon/hook-netcon.ps1`, generalized from
  `deadlock-netcon.ps1`. Deadlock still has its own copies. Moving Deadlock onto the shared ones is
  a separate change that needs its own Deadlock runtime check.

## Not ported, or asked about first

- Scene objects: not ported.
- CS2 skins, cosmetics and workshop maps have no CS:S counterpart. They are raised with the owner
  when their step comes up.
