# Player POV

**Status: the In-game HUD view is built (below). The rest of this page is a plan.** It scopes
[Wockstajit/SourceMVM#116](https://github.com/Wockstajit/SourceMVM/issues/116) into stages the owner
can approve one at a time. When a stage ships, its plan text is replaced by how it works.

## In-game HUD: the player's own screen

The **In-game** HUD option shows the watched player's HUD the way they saw it while playing, so a
first-person render looks like their own gameplay footage. It is the option that already existed,
in the same three places:

- the Camera Editor and Config **GAME UI** picker (`mirv_filmmaker editor hud game`),
- the Render workspace's **GAME HUD → In-game** tile (`mirv_filmmaker render pass hud game`).

There is no separate setting. While the In-game view is on screen (a demo is loaded, the editor or
Config is open, the view is In-game, and F8 has not blacked the screen out), it:

- hides CS2's spectator panels: the name and weapon bar, the centre avatar and the vignette;
- sets `cl_radar_square_when_spectating 0`, so the radar is CS2's round one;
- sets `cl_radar_show_all_players_when_spectating 0`, so only the enemies the team spotted show;
- sets `cl_show_observer_crosshair 2`, so CS2 draws the watched player's own crosshair.

The viewer's own values of those three are read when the view comes up and written back as soon
as it goes: switching to another view, closing the editor or Config, F8, or leaving the demo. They
are written only on those two edges, never every frame. A variable a CS2 update removed is skipped
and logged, not guessed. The `mvm_debug` log records both edges under `hud.gameplay`.

Pick the player with CS2's own first person (the demo bar's player list, or Customize's player
pick) and the render's **First Person (POV)** camera.

Code: [GameplayHudLook.h](../../AfxHookSource2/Filmmaker/Movie/GameplayHudLook.h), called from
`Filmmaker::RunMainThreadFrame` after the HUD panels; the panel hiding is `applyHudView` in
[CameraTimelineJs.h](../../AfxHookSource2/Filmmaker/Panorama/CameraTimelineJs.h).

## The request

During offline demo playback, show one chosen player's view the way they saw it: first person, their
viewmodel and crosshair, the normal CS2 player HUD and the round radar (not the spectator HUD and the
square radar), and a way to switch players. The reporter suggested four stages, and this plan keeps
them:

1. A reliable first-person POV target, with switching.
2. The viewmodel and the player's crosshair.
3. The native player HUD and the round radar.
4. Feedback, audio and death presentation.

Offline demo playback only. Nothing here may run on a live or VAC-protected server.

## Licensing rule: read the feature list, copy nothing

The reporter pointed at a `mirv_pov` implementation in a third-party advancedfx fork. That fork's POV
code is AGPL-3.0-only. SOURCE:MVM is MIT. So:

- Do not copy, paste, port or translate any of its code, and do not open its source while working on
  this feature.
- What we used: its README's list of features and settings, which is a description of behaviour, not
  code. Everything below comes from CS2's own console variables, CS2's Panorama layout and style files
  (the local, git-ignored `reference/cs2 refs/` dump), and hooks SOURCE:MVM already has.
- Any command this needs goes on `mirv_filmmaker`, never `mirv_pov`, so nobody mistakes it for the
  fork's feature or expects its settings to work.

## What SOURCE:MVM already has

| Piece | What it does today | Where |
| --- | --- | --- |
| Demo cameras | Switches CS2's first person, chase and free cam (`spec_mode 2/3/4`). Forces `spec_show_xray 0` once per demo. | [MovieMode.cpp](../../AfxHookSource2/Filmmaker/Movie/MovieMode.cpp) |
| Spectated player | `AfxGetSpectatedPawnIndex` / `AfxGetSpectatedUserId`: the engine's observer target first, then the player whose eye is within 12 units of the camera, then the local viewer, held for 1 s through dropouts. | [ClientEntitySystem.cpp](../../AfxHookSource2/ClientEntitySystem.cpp) |
| CS2's demo bar state | Reads `HudDemoController.GetDemoControllerState()` and `GetDemoPlayers()` (spectated pawn, observer mode, roster). `spectateNative()` calls `SpectatePlayer(controller)`, the same thing CS2's player list does. | [CameraTimelineJs.h](../../AfxHookSource2/Filmmaker/Panorama/CameraTimelineJs.h) |
| Player pick | Customize's roster and pick: first person on a living player, our free cam on a dead player's eye (CS2 refuses to spectate the dead). | [CustomizeTarget.h](../../AfxHookSource2/Filmmaker/Cosmetics/CustomizeTarget.h), [customize-feature.md](customize-feature.md) |
| Spectate by SteamID | `mirv_filmmaker cosmetics spectate <steamId>` writes the observer target directly, or falls back to one `spec_player`. | `Cosmetics_SpectateSteamId` in `CosmeticDebug.cpp` |
| Game HUD views | `mirv_filmmaker editor hud hidden\|game\|full`. `game` is the In-game view above. The same view drives `render pass hud none\|game\|full`. Outside the editor and Config the HUD is always `full`. | `applyHudView` in [CameraTimelineJs.h](../../AfxHookSource2/Filmmaker/Panorama/CameraTimelineJs.h), [CameraEditorHud.h](../../AfxHookSource2/Filmmaker/Panorama/CameraEditorHud.h), [filmmaker_render_passes.md](filmmaker_render_passes.md) |
| Crosshair off | HUD blackout (F8) and render `hud none` set `crosshair 0` and restore it, because CS2's crosshair is not a Panorama panel. | [HudBlackout.h](../../AfxHookSource2/Filmmaker/Movie/HudBlackout.h) |
| Aim crosshair | Our own reticle at the recorded aim when camera effects move the view off it. Hides CS2's crosshair while on. | [AimCrosshairHud.h](../../AfxHookSource2/Filmmaker/Panorama/AimCrosshairHud.h) |
| Viewmodel | CS2 draws the watched player's viewmodel in first person. We add sway, the cosmetics viewmodel mirror and FX alignment on top. | [ViewModel.cpp](../../AfxHookSource2/ViewModel.cpp), `Cosmetics/`, `Movie/FxAlign.cpp` |
| Kill feed | Inherited `mirv_deathmsg localPlayer` chooses whose kills the death notices highlight. | [DeathMsg.cpp](../../AfxHookSource2/DeathMsg.cpp) |
| Game events | Client-side `FireEvent` hook sees `player_death`, `player_hurt`, `weapon_fire` and the rest. | [GameEvents.cpp](../../AfxHookSource2/GameEvents.cpp) |
| Setting guard | Saves console variables a job changes and restores them on every outcome. | `RenderStateRestoreGuard` in `Render/RenderStateGuard.h` |

So switching players, resolving who is watched and hiding panels already exist. What is missing is
one owner that keeps the view on one player for the whole demo, and a player look for the HUD.

## What CS2 itself offers

Read from the CS2 1.41.9.6 client and its Panorama files on 2026-11-07. The descriptions are CS2's
own help text, reworded. **None of these has been tried in a demo yet**, so each is a lead, not a
result.

| Console variable | CS2's description | Use |
| --- | --- | --- |
| `cl_radar_square_when_spectating` | Square radar while spectating. | `0` should give the round radar. This is the reporter's main complaint. |
| `cl_radar_show_all_players_when_spectating` | Show every player on the radar, spotted or not. | `0` should show only enemies the team has spotted, like a player sees. |
| `cl_show_observer_crosshair` | Show the observed player's crosshair: 0 off, 1 friends and party, 2 everyone. | `2` should draw the watched player's own crosshair. Player controllers carry `m_szCrosshairCodes`. |
| `spec_lock_to_accountid`, `spec_lock_to_current_player` | Lock the spectator target to an account ID, or to the current target. | CS2's own lock. Might do most of stage 1's "stay on this player". |
| `spec_hud_color_to_team` | Spectator HUD colour always follows the team or teammate colour. | Player HUD colour. |
| `spec_show_xray`, `cl_teamid_overhead_mode`, `cl_teamid_overhead_maxdist_spec` | Through-wall outlines and overhead names for spectators. | A player sees no outlines and only teammates' names. |
| `cl_spec_show_bindings`, `cl_spec_stats` | Spectator key hints and stats. | Off. |
| `spec_freeze_time`, `spec_freeze_traveltime`, `spec_freeze_deathanim_time`, `cl_deathcam_audio_mix_*` | Freeze cam and death-cam audio timing. | Stage 4 death presentation. |
| `cl_drawhud_force_radar`, `cl_drawhud_force_deathnotices`, `cl_drawhud_force_teamid_overhead` | Keep these parts when the HUD is hidden. | Possible "radar and kill feed only" looks. |
| `cl_prefer_lefthanded` (and `m_bLeftHanded` on the player) | Left-handed viewmodel. | Stage 2 check. |

The HUD itself is native C++ panels (`CSGOHudRadar`, `CSGOHudHealthAmmoCenter`, `CSGOHudWeaponSelection`,
`CSGOHudTeamCounter` and so on, laid out in `hud.xml`). The native code puts state classes on the HUD
root: `HUD--localplayer--spectator`, `HUD--localplayer--dead`, `HUD--spectating-target`,
`HUD--spec-override` and `HUD--team--ct`/`--terrorist`. The radar layout has both `Radar__Round` and
`Radar__Square`, and the native panel picks one. JS can hide panels and change styles, but the native
code probably rewrites those root classes every update, so changing them from JS may not stick.

What a demo does not carry, as far as we know: the watched player's `viewmodel_fov` and
`viewmodel_offset_*`, `cl_crosshair_recoil`, `cl_hud_color`, and radar zoom and rotation settings.
Those stay the viewer's own settings unless we add per-player overrides.

## Stage 1: POV lock and player switching

**Goal.** `mirv_filmmaker pov <player|next|prev|off|status>` keeps first person on one player for the
whole demo: through pause, seeks, round changes and their death. A player picker in our UI starts
and switches it. Every first-person effect (shake, recoil, Customize, death animations) then agrees
on who that player is.

**What CS2 gives us.** `SpectatePlayer(controller)` on the demo bar, `spec_mode 2`, the roster from
`GetDemoPlayers()`, each controller's SteamID, and possibly `spec_lock_to_accountid`.

**Hook points.** The main-thread frame (next to `MovieMode::FlushActions`), the timeline's spectate
request, level init and demo seeks, and `round_start` / `player_spawn` / `player_death` from the game
event hook. `AfxGetSpectatedUserId` prefers the locked player while a lock is on.

**New code.**

- `Filmmaker/Movie/PovLock.h/.cpp`: the engine side, plus the command.
- `Filmmaker/Movie/PovLockMath.h`: the decisions, engine-free: who to show this frame, when to
  re-assert, next/prev order (T then CT, by slot), and what to do when the player is dead.
- `Automation/tests/pov-lock-math-tests.cpp` and an `add_test` entry in `CMake/Source2Tests.cmake`.
- A player picker, designed mockup first
  ([mockup-first workflow](../panorama/mockup-first-workflow.md)). Customize's player list
  (`CameraEditorCustomizePlayersJs.h`) already lists the T and CT players and is the starting point.

**When the player dies.** Proposed default: stay on their body (our free cam at the body's eye, as
Customize does) until the next round, then go back to first person when they spawn. The owner picks
the default (see the questions at the end).

**Size.** Small to medium: about 400 to 700 lines including tests, about 2 to 3 days with live
testing.

**Risks.**

- POV demos from "Your Matches" record one player. The observer state reads mode 0 and target -1
  there, and other players may not be watchable. The lock must report this, not fail silently.
- Seeking may reset the spectator target. The lock has to re-assert after a seek without fighting
  the timeline's own seek handling.
- The lock must step aside while the camera path, Follow camera or free cam owns the view, and must
  hold during a render job.
- `spec_lock_to_accountid` may behave differently in demo playback than in a live spectator slot.

**Live check.** GOTV demo, `mvm_debug start`. Lock on a player and play three rounds, including their
death and a seek backwards and forwards. `pov status` and throttled `pov.lock` lines in the debug log
should show the target held every frame. Capture alive, dead and next round. Render a short clip with
the lock on. Then load a "Your Matches" POV demo and confirm the command explains what it cannot do.

## Stage 2: viewmodel and crosshair

**Goal.** The watched player's own crosshair, and their viewmodel with its animations.

**What CS2 gives us.** CS2 already draws the watched player's viewmodel in first person, animated
from the demo. `cl_show_observer_crosshair 2` should draw their crosshair from their crosshair code.

**Work.**

1. While a POV lock is on, set `cl_show_observer_crosshair 2` and restore the viewer's value
   afterwards, through the same setting guard. About 100 lines.
2. Make the existing crosshair switches agree: blackout and render `hud none` already force
   `crosshair 0`. Aim crosshair hides CS2's crosshair, so with POV on it should draw the player's
   crosshair style instead of ours. That means decoding the crosshair code into size, gap, colour,
   thickness, dot and outline. It is optional, medium sized, and needs its own math tests.
3. The viewmodel is an audit, not a build. Check reload, inspect, draw, knife, grenades and a
   left-handed player against our sway, cosmetics mirror and FX alignment. If players' viewmodel
   offsets turn out not to be in the demo (expected), offer an optional per-player viewmodel preset
   (FOV and offsets) that applies while locked.

**Hook points.** The POV lock state, the setting guard, `AimCrosshairHud`, `ViewModel.cpp`.

**Size.** Small for item 1. Medium for item 2. Small to medium for the preset in item 3.

**Risks.** `cl_show_observer_crosshair` may be ignored in demo playback, or need the code to be in
the demo. Crosshair recoil and HUD colour are the viewer's own settings. Any crosshair-code decoding
must be written from CS2's behaviour, not from another project's code.

**Live check.** Lock on two players with clearly different crosshairs. Compare each capture with
the code CS2's scoreboard shows for that player. Capture reload, inspect, knife and grenade. Log the
crosshair code read in `mvm_debug`.

## Stage 3: native player HUD and round radar

**Goal.** The HUD a player saw: round radar centred and turned with them, only spotted enemies,
teammate colours, their health, armour, ammo, money and weapons, no spectator panels, no enemy health
in the top bar. It applies in plain playback and in renders, not only inside the editor.

Cheapest layer first. Stop as soon as the captures match.

- **3a. Console variables. Built** as the In-game view (top of this page) with the two radar
  variables and the crosshair. Still to compare against a real player capture:
  `cl_spec_show_bindings`, `spec_hud_color_to_team` and the overhead-name settings.
- **3b. Panorama (medium, about 200 to 400 lines of JS plus the mockup).** Extend the `game` view to
  hide whatever spectator-only parts the captures still show, re-asserted every frame like
  `applyHudView` does today. Likely candidates are enemy health in the top bar and spectator name
  plates. Make the view apply outside the editor while POV is on.
- **3c. Native (large, unknown).** Only if 3a and 3b leave a visible gap: find where the client
  decides the viewer is a spectator and make the HUD treat the locked player as local. This is tied
  to the game build and could break demo playback. It needs its own approval.

**Hook points.** `applyHudView` in `CameraTimelineJs.h`, `CameraEditorHud::HudView`, the render
session's HUD setting, the settings store, and the setting guard.

**Risks.** The native HUD rewrites its own classes and visibility, and it rebuilds while the editor
is open (the restore code already copes with rebuilds). The round radar in spectator mode might
centre on the map, not the player (`cl_radar_always_centered`). Spotted-only enemies rely on spotted
flags being in the demo. Buy-zone and bomb-carrier indicators may stay spectator-only.

**Live check.** First get ground truth: play an offline bot match with `mvm_test` as a player and
capture the real player HUD at 854x480. Then capture the POV HUD on the same map at the same size and
compare element by element: radar shape, centre, rotation, enemy icons and teammate colours; health,
armour, ammo, money, weapon list; top bar; kill feed highlight; crosshair. Capture the open, switch
and exit transitions, as CLAUDE.md asks, and render a clip with `render pass hud game`.

## Stage 4: feedback, audio and death

Take each item as its own issue, after stages 1 to 3. For each, first check what CS2 already shows in
first-person spectating. Prefer the native effect. An overlay we draw ourselves is the "simulated"
look the reporter wants to avoid, so if there is no native path, say so instead.

| Item | First thing to check | Likely size |
| --- | --- | --- |
| Kill feed highlight | Point `mirv_deathmsg localPlayer` at the locked player. | Small |
| Flash whiteout | Whether CS2 already draws it for the watched player. | Small if native |
| Damage direction | Whether `CSGOHudDamageIndicator` fires for a watched player. | Unknown |
| HE and flash ringing | Whether the sound effect is local-player only. | Unknown |
| Death cam and death panel | `HudDeathPanel` and the `spec_freeze_*` settings. Fits with our death animations. | Medium |
| Voice and radio | Demo voice playback and who a spectator hears. | Medium |
| Radar sound rings, pickup hints, kill reward | Whether the native panels can be fed a non-local player. | Unknown |
| Buy menu and scoreboard replay | Out of scope unless asked for separately. | Large |

**Live check.** Per item: the matching game events in `mvm_debug`, captures across several frames
(one screenshot cannot show motion), and audio in a rendered clip.

## Next steps

The owner chose the small route first: no new settings, the existing In-game view does the work.
What is left, in order:

1. Compare a render with **HUD game** against a real player capture from an `mvm_test` bot match
   and list what still differs (stage 3b).
2. Stage 1's player lock, only if CS2's own first person turns out to drop the player too often
   during a render (deaths, seeks).

## Questions for the owner

1. If a player lock is built: stay on their body until they respawn, follow their killer, or do
   what CS2 does by default?
2. Scope: GOTV demos only, with "Your Matches" POV demos reporting what they cannot do?

## Not in scope

- Live matches or online servers of any kind.
- Any code from the fork that inspired the request.
- Buy menu and scoreboard replay, unless requested as separate work.
