# Event FX presets

`CONFIG > EFFECTS > EVENT FX PRESETS` turns demo events into short visual cues. A preset combines
an exposure pulse, camera shake, a field-of-view kick, a chromatic pulse and a light burst, and
fires on the event it is bound to: a kill, a headshot, a flashbang, HE grenade, smoke or molotov
going off, the bomb exploding, or a timeline marker.

A preset can be tested on the spot, and baked: its exposure envelope is written into Scene
Keyframes at the playhead.

## Console

```text
mirv_filmmaker eventfx on|off|toggle|reset
mirv_filmmaker eventfx test <id> | select <id> | enable <id> <0|1> | add | remove <id>
mirv_filmmaker eventfx set <id> <field> <value>
mirv_filmmaker eventfx bake <id>
```

## Code and tests

- [EventFx/](../../AfxHookSource2/Filmmaker/EventFx) (game event and marker listener, the cue stack,
  the pulse renderer)
- `EventFxMathTests`
