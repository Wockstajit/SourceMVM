# Per-keyframe camera shake (GTA V / Rockstar Editor style)

Right-click a diamond on the camera timeline to pick a shake for that keyframe. Each keyframe gets one style, one intensity (1 to 10), and one speed (10 to 100). This matches the shape the Rockstar Editor gives a camera marker, plus Apply to All, Remove, and Remove All.

This is a **separate additive layer over the camera path**. It is not part of the camera graph and cannot enter it.

| Piece | File |
|---|---|
| RAGE shake evaluator (oscillator + envelope + frame components) | `AfxHookSource2/Filmmaker/Movie/RageShake.{h,cpp}` |
| The 86-style table | `AfxHookSource2/Filmmaker/Movie/RageShakeStyles.cpp` |
| Per-keyframe store, project.mvm section, blending, apply | `AfxHookSource2/Filmmaker/Movie/CamPathShake.{h,cpp}` |
| Console surface | `AfxHookSource2/Filmmaker/CamPathShakeCommand.cpp` |
| Timeline right-click menu | `AfxHookSource2/Filmmaker/Panorama/CameraTimelineShakeMenuJs.h` |
| Source material, provenance, `cameras.ymt` extraction recipe | Not in the repository |

## Not to be confused with `camshake`

There are two unrelated shake systems, gated apart on purpose:

| | `mirv_filmmaker camshake` (Handheld / Recoil / Weapon Recoil) | `mirv_filmmaker pathshake` (this) |
|---|---|---|
| What it models | a live operator and weapon kick | an authored shot property |
| Where it applies | POV / Attach / ThirdPerson spectate | camera-path playback only |
| During camera-path playback | **disabled** (`ShakeCamMode::CameraPath`) | this is its only home |
| Stored | global settings | per keyframe, per demo |

See [filmmaker_camera_shake.md](filmmaker_camera_shake.md) for the other stack.

## It shakes the view, not the path

The shake is a **frame shaker**: it jogs the camera frame that the director already computed from the marker path. The path itself is never touched. The call runs from the view-setup trampoline in `main.cpp`, in the slot `CameraShake_Apply` uses. It runs after the whole camera-ownership stack and just before the write-back. `CamPathShake_ApplyToView` gates on `ShakeCamMode::CameraPath`.

The shake does not run inside the camera-path pose push (`CamPlayback`). Adding the offset there would make the shaken pose the path's *published* pose, and anything that reads the camera back would see it:

- placing or repositioning a marker snapshots the current camera, so the shake gets **baked into the keyframe permanently**;
- the graph editor's per-channel defaults come from `CameraBridge_GetCurrentCamera`, so they drift;
- `main.cpp`'s spectated-pawn match compares a pawn's render eye to the camera within 0.2 units / 0.2°, which a shake breaks outright.

Shaking the view keeps the authored path clean. `CamPlayback` publishes the authored pose and exports only `LastEvalTick()`, so the view layer knows where it is on the path.

## Why the graph can never pick this up

Shake must never enter the camera graph, which holds only authored positioning and movement. The structure enforces this:

- Settings live in `CamPathShake`'s own map and in its own `camshake` section of the demo's `project.mvm`. They never sit on `CamMarker`, in `PathSettings`, or in the `campath` section. A value on a marker would be a graph channel. `CameraPath::ReplaceMarkersFromGraph` rewrites the whole marker vector on every curve drag, so any marker field counts as graph data.
- The shake is applied at the view stage, not to the path's published pose (see the section above). The dolly's own pose and its smoother state are never shaken. Turning the layer off therefore restores a bit-identical move, and nothing that reads the camera back can bake the wobble in.

Keyframe identity is the keyframe's **tick**, not its index. An index would silently re-point to the wrong keyframe as soon as a key was inserted ahead of it, because `CamMarkers::Add` inserts in tick order and renumbers. `CamPathShake_RunFrame` watches the live tick list. An equal count with moved ticks means a retime, and the settings follow. A changed count means pruning anything whose keyframe is gone.

## Determinism

Everything is a pure function of the **path tick**:

- The clock is `tick * interval_per_tick`. It is never wall time, and never the markers' recorded `time` fields, which can go stale after retimes or graph rebakes. This is the same reason `CamPlayback` evaluates Live playback by tick.
- Oscillators are evaluated in closed form rather than integrated per frame, so frame cadence cannot change the result.
- Each oscillator uses a fixed per-component phase offset, not a random one.

Scrubbing to a tick shows the frame that will render, and re-rendering a range gives identical frames. Freeze timing maps its position back onto the tick axis first, so the same point on the path shakes the same way in both timing modes.

## Playback

- The keyframe's shake starts at its own tick and runs for its segment.
- **Looping** styles (handheld, vibration, drunk, skydive, …) hold indefinitely. **One-shot** styles (explosions, recoil, impacts, jolt) fire once and release.
- On a style change, the outgoing keyframe's shake keeps decaying for 0.35 s past the new keyframe's tick, while the incoming style's own attack ramps in. Without this, swapping styles cuts one loop dead on a single frame, which reads as a hitch.
- Scrub preview applies the shake too.
- **Apply to All** makes every keyframe carry the same style. That is one continuous run, so a looping style holds across the whole shot instead of re-attacking at each keyframe.

## UI

Right-click any diamond on the camera timeline:

```
CAMERA SHAKE  (KEY 3)                           ✕
  Style       ◀      Hand       ▶   <- click the name for the full list
  Intensity   ◀        5        ▶
                 ▓▓▓▓▓░░░░░
  Speed       ◀       50        ▶
                 ▓▓▓▓▓░░░░░
  ─────────────────────────────────────────────
  [ Apply to All ] [ Remove ]        [ Remove All (4) ]
```

Intensity and speed are segmented meters, not `5 / 10` text. You set them by feel while watching the shot. A filled bar reads at a glance, where a number has to be read and compared. Both values are discrete (1 to 10 for intensity, steps of 10 for speed), so the meter uses segments instead of a continuous fill. You can tell 6 from 7 without reading the number.

**Apply to All** copies this keyframe's shake onto every keyframe. That is the normal way to shake a whole shot. It also makes the path one continuous run, so a looping style holds across the shot instead of re-attacking at each diamond. **Remove All** is the only irreversible action here. It sits apart, carries the danger colour, and confirms in place.

- The arrows are the fast path. Clicking the style name opens the scrollable list.
- Style arrows wrap, so holding one keeps moving instead of stopping at the end.
- Intensity steps by 1 (1 to 10). Speed steps by 10 (10 to 100).
- The menu stays up until you click somewhere else. A full-screen catcher underneath dismisses it, which is the only way to get click-outside in this Panorama context.
- A keyframe carrying a shake shows a blue dot inside its diamond, so a shot's shake points are readable off the bar.

The timeline forces UI-mouse mode so the right-click reaches the menu. Without that, `MovieMode::OnMouseButton` swallows the click for CS2's spectator switch. `oncontextmenu` is the only right-button event this in-game HUD context delivers.

## The style list

The Rockstar Editor's camera marker offers **five shakes plus None**. They correspond to the five `REPLAY_`-prefixed names in the native list:

| Editor shake | native shake | our label |
|---|---|---|
| None | - | (no shake) |
| Hand | `REPLAY_HAND_SHAKE` | Hand |
| Drunk | `REPLAY_DRUNK_SHAKE` | Drunk |
| Explosion | `REPLAY_EXPLOSION_SHAKE` | Explosion |
| Air Turbulence | `REPLAY_SKY_DIVING_SHAKE` | Air Turbulence |
| Ground Vibration | `REPLAY_HIGH_SPEED_VEHICLE_SHAKE` | Ground Vibration |

Five shakes head the dropdown under a **ROCKSTAR EDITOR** header. Below a **MORE CAMERA SHAKES** header sit fourteen of our own extras: other camera-behaviour shakes from the wider native list (Hand Soft, Running, Turbulence, Road Vibration, Water Bob, Wobbly, Jolt, …). The rest of the 86 are weapon recoil, reloads, magazine drops and hit reactions. They stay off the menu, but you can set them by name from the console. `pathshake styles all` lists them.

## Console

```text
mirv_filmmaker pathshake on|off|toggle          - master switch (A/B a shot)
mirv_filmmaker pathshake set <key> <style|index> [intensity 1-10] [speed 10-100]
mirv_filmmaker pathshake style     <key> next|prev|<name>|<index>
mirv_filmmaker pathshake intensity <key> next|prev|<1-10>
mirv_filmmaker pathshake speed     <key> next|prev|<10-100>
mirv_filmmaker pathshake applyall <key>         - copy this keyframe's shake onto every keyframe
mirv_filmmaker pathshake clear <key>  |  pathshake clearall
mirv_filmmaker pathshake styles [all]           - the 19 menu styles (or all 86 native names)
mirv_filmmaker pathshake state                  - what is set where
```

`<key>` is the 0-based keyframe index. The timeline labels keyframes from #1.

The `mvm_debug` channel is `pathshake.apply` (throttled). It logs the path tick, segment, style, intensity, speed, elapsed time, and the resulting position, angle, and FOV delta.

## Scale mapping (as in the Rockstar Editor)

Both mappings follow how the Rockstar Editor's dials behave:

| UI | Engine | Why |
|---|---|---|
| intensity 1 to 10 | amplitude via a **quadratic** curve: `0.024·I² + 0.076·I` → 0.10 / 0.98 / 3.16 at 1 / 5 / 10 | A quadratic curve; a linear ramp makes the bottom of the dial feel dead. |
| speed 10 to 100 | **time scale** `speed / 50` (0.2 to 2.0) | Shake speed warps the shaker's whole clock. Envelopes stretch with the oscillation, so a slowed one-shot also decays more slowly. |

Two more behaviours match the editor:

- **Runs are continuous.** Consecutive keyframes with the same style are ONE shake, clocked from the run's first keyframe. It does not re-attack at every key. Intensity and speed **lerp toward the next keyframe's values** across the segment, so a shake can ramp over a shot.
- **One-shots are per-key.** An explosion keyframe following another explosion keyframe fires again. This is the one-shot exception.

## Where the numbers come from

The **86 style names** match `SHAKE_GAMEPLAY_CAM`'s public documentation entry for entry. The **model** is an oscillator-plus-envelope shaker.

The **per-style numbers are ours**. Rockstar's authored values live in `common.rpf/data/cameras.ymt`, which is game data with no public dump. Each name is therefore mapped onto one of 17 families whose oscillator and envelope shape matches what that shake is for, with per-name amplitude and frequency scales. `shake-styles.json` mirrors the table.
