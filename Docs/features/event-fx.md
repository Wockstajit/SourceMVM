# Event FX

`CONFIG > ENVIRONMENT > EVENT FX`, beside Scene Objects, spawns CS2's own particle effects when
something happens in the demo. The presets are tiles, two to a row, each showing its event, how
many effects it plays and how many are placed in the map; the selected one's settings sit in two-
and three-column rows under them, so the card shows whole. A preset says which event plays it,
which effects play, and where:

| Setting | What it does |
| --- | --- |
| Plays on | A kill, a headshot, a flashbang, an HE grenade, a smoke grenade, a molotov, the bomb exploding, or a timeline marker the playhead crosses |
| Effect 1 to 3 | Up to three effects from the catalog below, played together |
| Place | **At the event**: where the grenade went off (the event's own coordinates), the player who died (their feet, or their eye for a headshot), or the planted bomb. **In front of camera**: a little ahead of the view. A timeline marker has no place, so it plays in front of the camera |
| Height | Raises the effects above that point |
| Copies, Spread | Up to six copies scattered on a ring around the point, each starting a moment after the last |
| Delay | Seconds after the event |
| Keep for | Seconds before the effects are removed; 0 uses each effect's own length. Looping fire would otherwise burn forever |
| Light burst | A real CS2 light flashes at the point (warm blast or white flash) |
| Camera shake | Off by default; weaker the farther the camera is from the effect. **Speed** (slow to very fast) and **Length** (short to very long) sit under it; on **Auto** both follow the strength, so a light shake swings slowly and briefly and a hard one quickly and for longer |

**Test** plays the preset in front of the camera on the wall clock, so it plays while the demo is
paused. **Place in world** puts the preset's effects at the crosshair as Scene Objects that play
on the same event (below).

The built-in presets: HE bigger blast, bomb huge blast, molotov fireball, kill blood burst,
headshot blood spray, flash spark burst, smoke debris puff (off) and marker fireworks (off).

## The effect catalog

`kEffects` in [EventFxMath.h](../../AfxHookSource2/Filmmaker/EventFx/EventFxMath.h) lists every
effect a preset can spawn, each a CS2 particle system checked against `pak01_dir.vpk` and seen
playing in CS2 when spawned this way: HE grenade blasts (plain, dirt, snow, indoor), bomb blasts
(plain, indoor, short), heavy charge, small explosion, water blast; molotov and incendiary bursts,
molotov air burst, falling fire, fire, large fire; blood burst, headshot blood, mist, goop;
flashbang burst; smoke puff, debris, lingering smoke; fireworks, big fireworks, spark shower.
Systems that drew nothing when spawned at a point were left out (the gas and sky explosions,
embers, blood chunks, and the spark and electric sprays, which need a direction from a real hit).
`mirv_filmmaker eventfx effects` prints the list.

Effects are created through the game's particle manager (`Props_SpawnEffect`), every control point
at the spawn point. Every system an enabled preset uses is precached once the level has settled,
one a frame, so a blast during playback never stalls on a load. Rewinding the demo removes the
effects of the moment it left.

## Effects placed in the map

A placed effect in **ENVIRONMENT > Scene Objects** has a **Plays** setting: all the time (the
default), on one of the events above, or **At a tick** (when the playhead crosses it; **Set to
playhead** takes the current one). An effect set to an event waits, loaded, and starts from its
first frame each time the event happens, for **Plays for** seconds (0 is six). It is moved, grouped
and hidden like any other scene object, and saved with the map's objects. Every placed effect has
an icon in the view on the ENVIRONMENT tab (an orange burst, tagged with what starts it, such as
ON KILL, or PLAYING): a click on it selects it and shows its move gizmo, even while it waits and
has no particle to click. Placed effects play on
their event whether or not the Event FX presets are on. `Props.cpp` (`PumpTriggers`) runs this
every frame.

## Console

```text
mirv_filmmaker eventfx on|off|toggle|reset
mirv_filmmaker eventfx test <id> | select <id> | enable <id> <0|1> | add | remove <id> | place <id>
mirv_filmmaker eventfx set <id> trigger|effect0|effect1|effect2|where|lift|copies|spread|delay|life|lightBurst|lightFlash|shake|name <value>
mirv_filmmaker eventfx effects
mirv_filmmaker props set <id> play <kill|headshot|flashbang|hegrenade|smoke|molotov|bomb|marker|tick|->
mirv_filmmaker props set <id> playtick <tick|here> | playlife <seconds>
mirv_filmmaker props fire <id>
```

## Code and tests

- [EventFx/](../../AfxHookSource2/Filmmaker/EventFx) (the event listener, where events happened,
  the spawn queue, the shake)
- [Props.cpp](../../AfxHookSource2/Filmmaker/Props/Props.cpp) (placed effects that play on an event)
- `EventFxMathTests`: the catalog, the built-in presets, spawn planning (copies, delay, life, the
  cap) and the shake
- `event-fx-ui-tests.mjs`: the card's catalog matches `kEffects`, and the Scene Objects Play box
