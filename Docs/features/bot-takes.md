# Bot takes

**CONFIG → BOTS** records what you do in a private match and has real bots perform it. You run around,
stop the recording, walk wherever you want to be, and press **F7**: a bot does exactly what you did.
Record more takes and they all play together. Shoot them, throw grenades at them, and record the whole
thing to a demo.

It works in a private match or a workshop map. A demo has no server to put bots on, so the tab is
hidden there. Nothing here runs online: it needs the game server inside this process.

Maps with their own game mode can behave differently. Competitive-style workshop maps work.

## Using it

1. Start a private match and go to the spot you want to record from.
2. Press **F6**, or **Record** in the Config panel. The top of the screen says `REC`, and you are not
   moved. Takes you already recorded play along, so you can shoot them or time a throw to them.
3. Do whatever the bot should do: walk, jump, shoot, throw, switch weapons, buy.
4. Press **F6** or **Stop**. The take is kept.
5. Walk to where you want to watch from, then press **F7** or **Play**. A bot appears at the start of
   the take and plays it once. You stay where you are.
6. When the take ends, a bot that is still alive stands at its last position.
7. Press **F7** during a take to stop it, and the bots stay where they are. Press it again to play from
   the start, as often as you like. A bot you killed comes back. Its dropped gun and any grenades still
   in the air or burning are cleared first.
8. Press **Record demo** for one pass as a GOTV demo. It lands with your other demos.

**Loop** is off by default. Turned on, the takes start over a second after the last one ends.

Your takes save themselves per map and come back the next time you load it. **Save** and **Load** keep a
second copy.

Nothing restarts the round, so doors and glass stay as they are between passes. The one exception is the
first use in a match still in warmup: warmup is ended, because a warmup brings the dead back by itself,
and the game restarts the round once when it ends. Freeze time is skipped the same way.

Rounds do not end by themselves once takes are set up, so a dead player stays dead until they press
**F6** or **F7**, and the top of the screen says so. You come back where you fell, on whichever team you
are on now. With no takes recorded, **F7** only brings you back.

## What you can change after recording

The **TAKES** card lists one row per bot. Click a row and that bot's own settings drop down under it.
None of this changes the performance.

| Control | What it does |
| --- | --- |
| Plays | Leaves a take out without deleting it |
| Never fires | Plays everything else, without shooting or throwing |
| Team | Which side the bot plays on |
| Starts after | Holds the take back so it lines up with the others |
| Player model | The character the bot appears as |
| One row per gun in the take | The gun the bot holds in place of that one, from the next time you press Play |

A swapped gun fires at its own speed, because the shots still go through the game's weapon code. Measured
from a take of rapid AK clicks followed by held fire: a Nova fired about once every 0.9 seconds and an AWP
about once every 1.45 seconds. A shotgun does not spam because the take clicked a lot.

### Skins

Skins, knives and gloves are changed in the demo, with Customize, the same as on any player. Customize
keeps those per SteamID and a bot has none, so each performer gets a made-up one from its take's name
(`PerformerSteamId`). It is written to the player and to each gun, so it is in the demo. The same take
keeps the same id, so its Customize choices stay with it.

## Solid objects

In **ENVIRONMENT → Scene objects**, select a placed CS2 asset and turn on **Solid**. You and the bots
now collide with it and can stand on it. **BOTS → Solid objects** can make every placed prop solid, or
make new ones solid as you place them.

The client still draws the prop as it always did. A solid prop adds an invisible `prop_dynamic` with
the same model and pose on the local server, using the model's own physics hull. If a round restart
removes it, it is sent again. Effects have no hull and cannot be solid.

## Gun randomness

**BOTS → Gun randomness** applies to you and the bots.

- **No random spread** is `weapon_accuracy_nospread`. Bullets go exactly where the gun points. Recoil
  is unchanged.
- **Recoil randomness** scales each weapon's `m_flRecoilAngleVariance` and
  `m_flRecoilMagnitudeVariance`, on the server and on the client that predicts it. At 0% each shot
  follows the recoil pattern exactly. The strength of the recoil and every other stat stay as shipped.

## How it stays 1-to-1

A take is not a path the bot is dragged along. It is the commands the server ran for you, tick by tick,
and the bot runs the same commands through the game's own movement, weapon and grenade code. Two takes
of the same input land within 0.02 units over more than 600 ticks, with no correction applied.

Three things keep it exact:

- **Bots do not think.** `CCSBot::Update` is skipped for a performing bot, or it would buy a rifle with
  the free money and walk at the rifle's speed.
- **A move no input explains is recorded.** A `setpos` or a trigger teleport is flagged, and the bot
  makes the same jump on the same tick. It is the only teleport a bot does by default.
- **A swapped gun stays on the path.** A different gun runs at a different speed, so with **Stay on the
  path** on Auto those takes get a per-tick `SetAbsOrigin`, without the teleport flag, so clients keep
  interpolating and it does not show. Always does this for every take. It also walks through anything
  placed in the way after recording, so Never and Auto are better for props.

`mp_solid_teammates 0` is part of the setup, because you finish a recording standing exactly where the
take ends and the bot would walk into you. An enemy still blocks.

## Console

Everything in the panel is `mirv_filmmaker takes ...`. `mirv_filmmaker takes help` lists it all.

| Command | What it does |
| --- | --- |
| `record [name]`, `stop`, `play` | The three buttons |
| `loop on\|off` | Play again after the last take ends. Off by default |
| `status` | The match state, and each bot's health, position, gun, magazine, drift and deaths |
| `trace on` | One line per bot command in the `mvm_debug` log, against the take |
| `demo [name]` | One pass as a GOTV demo |
| `object add <model.vmdl> [nosolid] [scale]`, `move`, `remove`, `list`, `clear` | A visible, solid server prop that also shows in a demo |
| `drift <units>` | Snaps a bot back past this distance. Off by default, because a snap is visible |

## Implementation

| File | Job |
| --- | --- |
| `Filmmaker/Takes/TakesMath.h` | Engine-free: the command rewrite, the scene clock, the file format. `Automation/tests/takes-math-tests.cpp` |
| `Filmmaker/Takes/TakesServer.cpp` | The only door into `server.dll`: signatures, detours, entities |
| `Filmmaker/Takes/Takes.cpp` | Recording, the pass state machine, objects, settings, commands, panel state |
| `Filmmaker/Takes/TakesHud.cpp` | The status line at the top of the screen |
| `Filmmaker/Panorama/ConfigHudBotsTabJs.h` | The BOTS tab |

Commands are stored as `CSGOUserCmdPB` wire bytes, not as C++ structs. The wire format survives game
updates, and `RewriteCmdForBot` changes what must change (tick, pawn, weapon, shoot position) without
knowing the layout of the generated message. Scenes save to
`%APPDATA%\HLAE\source_mvm\takes\<map>\<name>.mvmtakes`.

A dead bot comes back through the path the game's own join code uses: the controller's observer pawn gives
way to the player pawn, and the pawn's spawn routine runs. That routine refuses a pawn the game's rules
would not let in, so `FPlayerCanRespawn` is asked first and a refused player keeps spectating. `m_iNumSpawns`
is cleared, as the join code does. The round start time reads as now for that one call, because with players
on both teams the check refuses anyone later than `mp_join_grace_time` into a round, and that cvar stops at 30.

The three detours go in on first use, so a session that only watches demos never touches `server.dll`:

| Function | Why |
| --- | --- |
| The `bot_mimic` check inside the bot's command builder | Replaces the bot's command for the tick |
| `CCSPlayer_MovementServices` slot after a command runs | Records the player's command and where it left them |
| `CCSBot::Update` | Skips the AI for performers |

### Traps worth knowing

- `CCSGOUserCmd` keeps its buttons twice: in the message and in three plain fields next to it. On the way
  to the movement code the plain fields overwrite the message, so a command filled through the message
  alone arrives with no buttons at all. `ParseCmd` runs the copy the other way, as the game's own
  `bot_mimic` copy does.
- `weaponselect` is an entity index, not a handle.
- A player model must come from `agents/models/`. The same character under `characters/models/` is not
  in the map's manifest, so the client draws a tiny ERROR model and no player.
- `ent_create` loads nothing by itself. `PrecacheModel` runs first, and a spawn that still lands before
  the load finishes is checked a moment later and sent again.
- `ent_remove` finds its target when the deferred kill runs, so a new copy under the old name dies with
  the old one. Every spawned copy gets its own name.
- `bot_kick` also kicks SourceTV, even when given a bot's name, and **Record demo** then refuses until the map
  reloads. Bots are kicked with `kickid <slot>`.
- `bot_quota` left over from the match adds an extra AI bot next to a performer, and on the other team it
  shoots the performer at spawn. The setup holds the quota at the number of performers.
- A client `record` on a listen server drops most of its snapshots (`Cannot process snapshot ... delta`),
  which plays back as a bot that stutters. Demos use `tv_record`. GOTV starts with a map, so `tv_enable 1`
  is sent on the first frame of the session. If a match started before that, reload the map once.
- `mp_warmup_end` schedules its restart about 15 seconds out. `HurryMatch` moves that time to now, and
  moves the round start time the same way to end freeze time, so the game's own code and events still run.
  Writing the flags themselves would skip them.
- A pass waits a few ticks after the last respawn before it equips anyone, so the game's own spawn loadout is
  in place first.
- A bot starts at the speed its take starts at. From a standstill, a take started at a run stays a whole
  acceleration behind its recording for its entire length, about 47 units.
- On a slope a bot's boots stay flat, so the uphill foot can dip into the ground and the downhill one hover.
  Standing on the same spot, a plain AI bot with no takes involved looks the same to within 0.3 of 255 per
  colour channel, and the server reports the same ground values for both. A moving stock bot has not been
  compared.
- The team menu's switch only happens at the next round, and rounds do not end here. Once takes are set up,
  a pending `m_iPendingTeamNum` is applied at once with the game's own `JoinTeam`.
- The bottom HUD's spectator panel is collapsed by the game's stylesheet and opened by a class. Setting
  `visible = true` on it pins it on, and it then shows the last player you spectated during live play. Our HUD
  code gives it back with `style.visibility = null`.
- `m_steamID` cannot tell who is a bot, because a performer has one. `ControllerIsBot` reads `FL_FAKECLIENT`.
- A dropped gun is only cleared if a performer held it this pass and nobody owns it now. A gun lying on the
  map, or one you picked up, stays.
- `drop` is a client command, not part of the recorded input, so a bot does not repeat a gun you dropped by
  hand. It keeps the gun and switches to the next one on the same tick.

Recordings hold each command the server ran, so a client frame hitch that delivers two commands in one
tick plays back one tick apart. It stays within a tick of the original and does not build up.
