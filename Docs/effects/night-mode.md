# Night mode and time of day

Exposure control over a demo, plus a preset row that makes exposure read as a time of day.
Exposure alone only makes the same map darker.

- Module: `AfxHookSource2/Filmmaker/World/NightMode.{cpp,h}`
- UI: CONFIG → EFFECTS → Lighting (`Panorama/ConfigHudLightingTabJs.h`)
- Command: `mirv_filmmaker world exposure …`
- Status: SHIPPED, verified in-demo

---

## 1. What it does

CS2 runs auto-exposure: the tonemapper picks an exposure from scene luminance, so the image
brightens and darkens on its own as the camera moves. That is a problem for editing, because two
cuts of the same location can be graded differently by the engine.

This feature pins exposure to a fixed value:

- `0.10`: heavily underexposed. This is "night mode".
- `0.35`: dusk.
- `1.00`: roughly the default look.
- `> 1`: brighter; useful for a dream/flashback grade.

The feature pins the *tonemapper* and draws no dark quad over the frame. The result composites
correctly with everything else: muzzle flashes still bloom, and the sky still reads as sky. It
also survives the render pipeline.

Pinning also removes auto-exposure *drift*, so a locked-off shot stops changing brightness. That
alone is a reason to pin at `1.00`.

## 2. Why it is not a standalone feature

On its own this is one slider. A map at `0.10` exposure is a dark version of a daytime map. The
sun is still overhead and warm, the fog still reads as haze, and the sky is still blue. It does
not look like night.

It reads as night when exposure, sun colour, sun angle, fog colour and sky tint move together.
So Night Mode ships as both the `Night` preset and the raw exposure slider that preset drives.
Both sit in the same Lighting surface as the sun controls they overlap with. A separate tab would
show one lonely slider.

The preset row is deliberately the primary control, and the slider is the manual override beneath
it. As `SunLight`, `FogFx` and `WorldModulation` land, each preset gains those channels, and the
UI keeps the same shape. The row is already where a "look" gets chosen.

There is no separate preset module. The presets are values in the Lighting tab that call
`world exposure`. Exposure therefore has one setter and no second source of truth that can drift.

---

## 3. Mechanism

> This section records measured game behaviour.

### 3.1 Test setup

Mirage, repaired match demo, playback paused at tick 30555. Every capture uses the same camera
and frame, and the screenshots were compared directly.

### 3.2 What does not work

A. Writing `C_PostProcessingVolume::m_flMinExposure` / `m_flMaxExposure`.
This is what the prior approach did. The writes land. The
`mvm_debug` log shows the map's single volume (entity 82) snapshotted at `min=0.8000
max=1.0000 ctrl=1` and rewritten every frame, but the image does not change.

The reason is visible in `world health`: `camera is inside post-process volume index: -1`.
The spectator camera is inside no volume, so the tonemapper never consults it. Setting
`m_bExposureControl` true (the field the reference threads omit) does not change this either.

Writing *all* volumes instead of only the active one would not rescue this approach. No volume is
being consulted, so the choice of volume makes no difference.

B. `mat_tonemap_force_min` / `mat_tonemap_force_max`.
Confirmed set to `0.1` / `0.1` by reading them back over netcon. No visual change. The CS2
renderer does not read them.

### 3.3 What works

`mat_tonemap_force_scale` is a single cheat convar.

| Value | Result |
|---|---|
| `0` | off; engine auto-exposure resumes |
| `0.10` | night |
| `0.35` | dusk |
| `1.00` | roughly neutral |
| `> 1` | brighter |

Verified by screenshot at the fixed camera and tick above. Pinning the tonemapper, instead of
drawing a dark quad over the frame, is why it composites correctly and survives the render
pipeline.

The convar is cheat-flagged, so the module issues `mirv_cvar_unlock_sv_cheats` + `sv_cheats 1`
once before its first write. That is acceptable because this DLL is offline demo/movie tooling
only, never used near a server.

### 3.4 Consequences for update resilience

This feature depends on no schema fields, only one convar name. That makes it the most
update-proof thing in the World module: only a convar rename can break it.

`WorldSchema` resolves the `C_PostProcessingVolume` fields, and `world health` reports the volume
count and active-volume index. The pair `1 volume, camera inside -1` is the expected report, because
the camera is inside no volume (see 3.2).

### 3.5 Restore and threading

Restore is `mat_tonemap_force_scale 0`.

`NightMode_RunFrame()` runs on the main thread, pumped from `World_RunFrame()`
(`Filmmaker/World/WorldCommand.cpp`), which `Filmmaker::RunMainThreadFrame()` calls each frame
(`Filmmaker.cpp`). It re-asserts the convar every 2 seconds while enabled, because a
demo/map change or a config exec can reset it, and a silently-reverted look mid-edit is worse
than a redundant command. Same cadence and reasoning as the `spec_mode` re-assert in that file.

## 4. Commands

```
mirv_filmmaker world exposure <value>   # 0.02 .. 4.0, pins the tonemapper
mirv_filmmaker world exposure auto      # off; engine auto-exposure resumes
mirv_filmmaker world exposure           # print state + availability + volume diagnostics
mirv_filmmaker world health             # full schema/feature/entity report
```

The setter accepts a trailing `quiet` to suppress console echo, matching the convention the
Config panel's widgets use (the UI always passes it).

The presets are UI-side values that call
`world exposure`, so exposure has one place to be set and no second source of truth to drift.

---

## 5. Presets

Each preset sets exposure only. Sun colour and direction exist as their own
control group in this same Lighting tab (`World/LightFx.cpp`, see [`sun.md`](sun.md)), and fog
is not built yet (`FogFx`, see the master scope). Neither is wired into these preset buttons yet.
The preset row is where a future pass will hook them in.

Auto (restoring engine auto-exposure) is a separate trailing action beneath the slider, outside the preset row.

| Preset | Exposure |
|---|---|
| `Dawn` | 0.55 |
| `Day` | 1.00 |
| `Golden` | 0.85 |
| `Dusk` | 0.35 |
| `Night` | 0.10 |
| `Moonlit` | 0.22 |

A preset highlights only while the live value still matches it; moving the slider by hand drops
the highlight and the card reports the current numeric value instead.

---

## 6. UI

CONFIG → EFFECTS → Lighting, card `TIME OF DAY`:

- Six preset buttons (`Dawn · Day · Golden · Dusk · Night · Moonlit`) in a 3x2 grid. The active preset is highlighted. When nothing matches, the card's note line shows
  the live exposure value instead of a literal `Custom` label.
- An Exposure slider beneath the grid (`sliderRow`, 0.02 to 4.00, 2 decimals). A separate
  `Auto exposure` action button below the slider restores engine auto-exposure. It is the row's
  one trailing action, and the slider has no sentinel value for it.
- The tooltip on the Exposure label explains that it pins the tonemapper and does not dim the
  screen. That distinction explains why it composites correctly in a render.

State round-trips through `ConfigHud::BuildStateJson` as `exposure` alone. The panel matches it
against each preset's value client-side, so a console change shows in the panel on the next frame.

---

## 7. Failure modes

| Symptom | Cause | Check |
|---|---|---|
| Slider moves, nothing changes | `mat_tonemap_force_scale` is not found in the running build | Run it in console. If unknown, find the current tonemap convar with `find tonemap` and update `kScaleConvar` in `NightMode.cpp` |
| Nothing changes, convar exists | Cheats not unlocked | Run `sv_cheats`. It should be `true`. The module issues `mirv_cvar_unlock_sv_cheats` + `sv_cheats 1` on first use |
| Look reverts by itself mid-edit | Something re-executed a config after the convar was set | The module re-asserts every 2s, so this should self-heal. If not, capture `mvm_debug` `world.exposure` lines |
| Feature reports unavailable | Engine-to-client interface not resolved | This is a bigger problem than this feature. Run `mirv_filmmaker health` first |

`mvm_debug` logging: `world.exposure` records each set/restore and the cheat unlock. It is event
driven, not per-frame.

`world health` showing `1 volume, camera inside -1` is normal and expected. See 3.4.

## Related

- [`README.md`](README.md): master scope
- [`update-resilience.md`](update-resilience.md): CS2 update recovery
- [`sun.md`](sun.md): the sun controls this overlaps with
- [`world-modulation.md`](world-modulation.md): whole-scene tinting used by a time-of-day look
- Fog controls: the other half of a convincing time-of-day look (not yet separately documented)
