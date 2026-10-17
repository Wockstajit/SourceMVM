# Camera shake layers: Handheld, Weapon Recoil and Weapon Bob

SOURCE:MVM has three additive shake modules. All are off by default and integrate through
`CameraShakeStack`.

| Module | Files | Source |
| --- | --- | --- |
| Handheld camera | `AfxHookSource2/Filmmaker/Movie/HandheldCam.{h,cpp}` and generated `HandheldCamCurves.cpp` | Camcorder Effect plus CC0 Camera Shakify curves |
| Weapon recoil | `AfxHookSource2/Filmmaker/Movie/WeaponRecoil.{h,cpp}` | SOURCE:MVM (MW2022-style procedural viewmodel recoil) |
| Weapon bob | `AfxHookSource2/Filmmaker/Movie/WeaponBob.{h,cpp}`, `WeaponBobMath.h` | ARC9 walk bob (style 0), look sway, idle breath |
| Weapon pivot | `AfxHookSource2/Filmmaker/Movie/WeaponPivot.{h,cpp}` | SOURCE:MVM (one per-weapon hinge per effect) |
| Integration | `AfxHookSource2/Filmmaker/Movie/CameraShakeStack.{h,cpp}`, `CameraShakeTypes.h` | SOURCE:MVM |

See [filmmaker_config_panel.md](filmmaker_config_panel.md) for the CAMERA tab.

## Pipeline and camera modes

The view-setup hook applies these layers after the camera ownership stack and before the final
write-back. They are additive over:

- plain first-person spectate;
- Follow/Attach preview;
- CS2's own chase and free cameras (they are not overrides, so they count as plain spectate).

They are disabled for free camera, baked Camera Path playback and other camera owners. The stack is
still called while gated, so it can discard stale integration state and restart cleanly.

Camera Path playback has its own shake system, with authored GTA V shake styles attached to individual
keyframes. It is separate from this stack and is documented at
[filmmaker_camera_path_shake.md](filmmaker_camera_path_shake.md).

All integration uses demo time, not wall time. A paused demo holds the shake pose, and replaying the
same range is deterministic. A shared tracker derives planar and vertical velocity from the accepted
camera pose. It resets on backwards time, long gaps, teleports, demo close and spectator changes.

### Sample continuity, and why the teleport test is on distance

`Movie/CameraShakeTrackerMath.h` decides whether a sample continues the previous one or is a cut
the modules must restart from. `CameraShakeTrackerMathTests` covers it.

The demo clock and the camera position are interpolated separately, so their per-frame deltas
disagree. In 1741 frames of ordinary playback, 4.6% carried a nonzero demo
dt below 4 ms, the shortest 0.4 ms, while the camera moved the distance a running player covers
in a frame. A teleport test that divides one by the other manufactures an impossible speed from
an ordinary stride. For example, 3.9 units over 0.9 ms reads as 4622 u/s, and the frame is classed
as a seek. Each false seek hard-resets the tracker, which drops Weapon Bob's phase and pose.

The test therefore uses the distance moved, measured against what a player could cover in the
sample's own interval, floored at one demo tick. The floor does the work. A sample may claim to span
0.4 ms, but the camera it reports cannot have moved less than one tick's worth of world.

### The oscillator phase clock

`Movie/CameraShakePhaseClockMath.h` drives every oscillator (Weapon Bob's gait, sway and idle
drift). `CameraShakePhaseClockMathTests` covers it. Gait and idle oscillators take
`ShakeInput::phaseDt`. Smoothed movement inputs stay on `dt`. Look sway measures and filters
rendered aim changes using `aimDt`: the render interval multiplied by the selected demo speed
during playback, exact demo intervals during capture, and zero while paused. Oscillator
phase corrections therefore cannot turn a steady view rotation into tiny-denominator sway
spikes or dropped aim samples.

A spectated player's view angle also moves only once per demo tick (64 a second), while frames
render faster, so most frames repeat the last angle. Measured per frame, a steady turn read zero on
those frames and the whole tick's turn on the next, and the sway stuttered with it.
`WeaponBobAimAdvance` holds the last measured turn on a repeated angle (up to 2.5 ticks; past that
the player has stopped) and measures each change over the time since the previous one.
`WeaponBobMathTests` covers a tick-stepped turn at 144 fps.

The demo clock is not smooth frame to frame. Two render frames can share a tick and report
dt = 0, and consecutive frames vary by milliseconds for the same wall interval. An oscillator
integrating it directly renders a staircase. The clock is a phase-locked loop instead. Each frame
advances by wall time scaled by the observed playback rate, and the running lead over demo time
is paid back a fraction per frame. The phase tracks demo time over any window longer than a
few frames, while no single frame sees the demo clock's own steps.

Determinism is preserved by locking. While `AfxStreams_IsRcording()` the step is the demo dt
exactly and the loop state is cleared, so a capture never depends on wall time.

## Handheld camera

Handheld combines two independent layers:

- **Drift:** slow angular and positional wander, jitter, tremble, speed-scaled walk motion, and FOV
  motion with a two-stage smoothing chain.
- **Curves:** nine baked 24 fps motion curves, selected for idle, walk or run with debounce and
  crossfade, plus ambient tilt and periodic re-grip motion.

The shipped curve data comes from
[`EatTheFuture/camera_shakify`](https://github.com/EatTheFuture/camera_shakify) and is CC0.
Attribution remains in the generated C++ file.

Regenerate or verify the curve table:

```batch
python FX/tools/convert-shakify-curves.py
python FX/tools/convert-shakify-curves.py --check
```

Console surface:

```text
mirv_filmmaker handheld on|off|toggle
mirv_filmmaker handheld centerlock on|off|toggle
mirv_filmmaker handheld centerlockpct <0-100>
mirv_filmmaker handheld viewmodel on|off|toggle
mirv_filmmaker handheld drift|curve|tilt|regrip <0-150|off>
mirv_filmmaker handheld speed <25-300>
mirv_filmmaker handheld variance <0-100>
mirv_filmmaker handheld period <2-60>
mirv_filmmaker handheld preset <idle|walk|run> <CURVE_KEY>
mirv_filmmaker handheld curves
mirv_filmmaker handheld state
```

Viewmodel decoupling removes this layer's position and rotation from the temporary viewmodel eye pose
without removing normal weapon animation. Centre Lock scales the handheld pitch and yaw contribution.
Translation, roll, FOV motion and recoil remain independent.

## Weapon recoil

Weapon Recoil reads `weapon_fire` events.

Weapon Recoil applies procedural kick to the viewmodel. **Punch** scales the positional channels
(back, up, side) at impulse time, using the ARC9-style spring, with overshoot.

**Rotation** mixes muzzle pitch, yaw and barrel roll about the pivot. Roll and yaw are strong, so the twist
reads as a wrist turn. Pivot compensation includes roll, so the gun
twists about the hinge instead of orbiting the eye. CS2's own weapon animation and other enabled
camera effects still compose with both controls.

The engine-free spring step lives in `Movie/WeaponRecoilMath.h`. `WeaponRecoilMathTests` covers both
styles, sustained bursts, and the full snap and recovery range. Rotation keeps angular overshoot and
seeded randomness. Integration uses fixed substeps on demo time. A paused Test Shot uses wall time.

Application path: the springs advance inside `CameraShake_Apply` on the demo clock (same
`ShakeInput` discipline as the other layers). The resulting transform is published through
`ViewFxVm_SetWeaponKick` to the CalcViewModelView input hook (`ViewFxVmCalcHook.cpp`). The hook adds
the kick to the temporary viewmodel eye origin and angles before CS2 builds the animated arms and
weapon pose, then restores the caller's values. CS2's own firing animation composes on top of the
kick, and the animation system never overwrites it. Position offsets are built in the camera basis
and rotated to world with the pre-shake view angles. The camera receives a configurable share of the
same kick. The rotation share is the `camera` knob. The position share is fixed at 0.4x of that.
The share is added into the stack's summed pose.

Scopes are Type > Slot > All. What differs between an AWP and an MP9 is handled by the multiplier table below.

Per-weapon presets are selected from the `weapon_fire` event's weapon field by substring: AWP 2.2x,
autosnipers/scout 1.6x, shotguns 1.9x, Deagle/R8 1.5x, AK 1.15x, SMGs ~0.6x, pistols ~0.7x, and the
default rifle 1.0x.

#### Which gun a shot belongs to (scope gating)

The scope resolver can only pick a Slot or Type override when it can derive a category from the
shot's weapon token, so getting that token right is the whole feature. Three things guard it:

- **The event field is not guaranteed.** `weapon_fire`'s `weapon` key is optional in the KV3
  payload. An absent key leaves an empty token, which matches no slot and no type, so every override
  falls through to All. `CameraShake_OnGameEvent` therefore falls back to the classname of the weapon
  entity the spectated player is holding (`CameraShake_SpectatedWeapon`) whenever the event token is
  not one the table recognises. A knife or grenade in hand is not in the table either, so the fallback
  can only ever substitute a real gun.
- **Test Shot fires the scope being edited.** `WeaponRecoil::TestWeapon()` picks the gun in shot when the selected scope covers
  it, else that scope's representative weapon, else the gun in shot, else a rifle. The console reply
  names the gun and the scope it used.
- **The card says what the scope reaches.** `wrLive` / `wrLiveType` / `wrLiveSlot` /
  `wrScopeCovers` in the state JSON drive the note under the picker and its live preselect.

### Style (Style 1 / Style 2)

Shown as **Style 1** and **Style 2**. The console tokens are `style1` / `style2`, and `mw2019` / `mw2022` also parse. The MW build years the two tables were derived from stay in
this document and in `WeaponRecoilStyles.h`, where they are provenance for a retune. In the card, they
read as a version picker for something the tool does not ship. Style 1 is springy (heavier, punch-led,
rides up and settles back). Style 2 is snappier (sharper, wider, rollier).

`style` selects the reference channel table the springs run with. It lives in
`Movie/WeaponRecoilStyles.h`, one profile per style. Every per-scope tuning is a multiplier on top of
the selected style, so switching the style rescales all of them.

**Style 1 is the default.** Its profile is derived from published data: ARC9's own visual recoil
(`HaodongMo/ARC-9`, `lua/weapons/arc9_base/{sh_recoil,shared}.lua`) and the MW2019 weapon values in
`CurlySparkle/ARC9-COD2019`. That data settled four things:

1. **One spring constant set for every axis.** ARC9 integrates the position vector and the angle
   vector each with a single `(kQuad 120, kMag 1, damp 6)` plus a shared quadratic drag of 0.5. Its
   return force uses the vector magnitude, not per-axis values. Style 1 uses ARC9's uniform constants, so the
   channels peak and settle together.
2. **Position is punch-dominated, and positional rise is close to nothing.** ARC9 applies
   `pos -= (0, punch, up * VisualRecoilPositionBumpUp) * fake` with `PositionBumpUp = 0.08`, so vertical
   translation is about 8% of vertical tilt. The muzzle rises by rotating about the pivot. It does not
   slide up the screen.
3. **Barrel roll is random-signed every shot, and it is big.** ARC9 computes
   `VisualRecoilRoll * SharedRandom(-1, 1) * 0.1`, an independent coin flip that never keeps a lean.
   The MW2019 pack sets `VisualRecoilRoll = 35` on the Kilo 141, against an ARC9 base default of 0.23.
   So Style 1 is a random-roll style.4. **The MW2019 pack puts no recoil on the camera** (`ViewRecoil = false`, `PhysicalVisualRecoil = true`).
   The weapon moves, and the view is not thrown. The 20% Camera Kick default is already generous for
   this style.

Amplitudes carry the ARC9/MW2019 ratios into our units. ARC9's absolute numbers cannot transfer directly, because
its rotation runs through `VisualRecoilCenter` and a chain of multipliers with no equivalent here.

Style 2 is the ARC9-derived port.

### Pivot marker

Pivot is the one setting whose effect is invisible in the number. Every value produces a plausible
motion, and what changes is where the gun hinges. `Movie/RecoilPivotMarker.{h,cpp}` draws a
wireframe sphere at that point. ARC9 ships a commented-out `debugoverlay.Axis` at the same point for
the same reason.

**There are two markers, one per effect.** Weapon Recoil and Weapon Bob own separate hinges, so each
publishes into its own drawer slot with its own colour: amber (255,190,60) for recoil, cyan
(60,200,255) for bob, from `WeaponPivotOwnerColour`. Pin state, auto-show and the "which effect is running" gate are all per owner, so
an orb is never drawn for an effect that is switched off.

**The marker is a world-space object, not a HUD overlay.** The hinge sits a few units from the eye, inside the weapon, so a flat disc painted over the
finished frame reads as a UI dot stuck to the screen rather than a thing in the gun. The marker is
drawn by `CCampathDrawer` alongside the camera gizmos, which is that class's existing job. That gives
it the scene's depth, the additive glow pass, and capture suppression (it never lands in a recorded
frame). Perspective handles the sizing on its own.

The hinge is a 3D point, not a forward distance. ARC9 puts it out at the hand (`VisualRecoilCenter = Vector(2, 4, 2)`,
"where your hand is"), and the per-class right and up values in the pivot store below put it where a
CS2 viewmodel actually sits. `WeaponPose` compensates with the exact residual `(d - R*d)`. It reduces to the forward-only form when the right and up offsets are zero.

**Getting it on top of the gun and under the UI is an ordering fix, not a depth fix.**
`CCampathDrawer` draws at the first 3D->2D transition. CS2 draws the viewmodel after that, so a marker
drawn there is painted over wherever it overlaps the weapon, and no depth state can help. The marker
therefore retains its draw functor and is drawn a second time at the `BeforeUi` render callback
(`CAfxRenderCallbackBeforeUi` in `RenderSystemDX11Hooks.cpp`). That callback marks the boundary between
"world and viewmodel finished" and "Panorama starts", and it comes before the `BeforeUi2` queue where
the scaled-preview blit lives. So with the Config panel open, the marker is shrunk into the preview
with the rest of the scene.

A re-arm of the draw on the next depth-bound pass is gated on `g_BeforeUiFiredThisPresent`, so nothing can draw into the scene after the boundary.

`CameraShakeStack` publishes both points in one loop over the owners, from the pre-shake basis. During
a kick, the camera takes its own share of the recoil, and the markers hold their place in the world
while the view moves around them. Each point is the resolved per-weapon hinge from that owner's pivot
store (see "Weapon pivot" below), not Weapon Recoil's own number. The two agree only for rifles. The
weapon's summed translation (recoil plus bob) is added to both, because the gun moves bodily by both
effects together, and only the rotation centre differs. F8 blackout hides them.

An orb flashes for 2.5 s whenever its own Pivot is edited (`pivotorb <owner> auto`, default on). It can
also be pinned from its card or the console. Each draws only while its own effect is enabled, so no
stale orb hangs in the world.

**"Edited" means the store's edit counter moved, not the resolved value.** The auto-show does not watch the resolved pivot, which also changes whenever the weapon in shot changes. `WeaponPivotStore::EditSerial()` is bumped only when a stored number actually moves. A weapon switch, a spectator switch, and a settings
restore that writes the values already present all arm nothing.

**The marker is drawn further from the eye than the hinge really is** (`RecoilPivotMarkerMath.h`).
The hinge lives inside the weapon, closer than the near plane. Weapon Bob's shipped hinge is 4.1
forward against a plane at about 6.5, so drawn at its true position the orb would be sliced in half at the
default and would be gone entirely for any smaller Forward. The marker is slid out along the eye ray until its
nearest point clears 16 units, and its radius is scaled by the same factor. A perspective projection
divides by depth, so a common scale about the eye leaves screen position and apparent size unchanged.
The marker already draws with depth testing off, so there is no occlusion to get wrong. Dragging
Forward down sweeps the orb out to the edge of frame and off it.

A hinge at or behind the eye has no screen position, and it is not drawn at all. That includes the
knife's measured -7.5 forward anchor, so the knife orb is never visible. That is geometry, not a fault.
The marker can also be legitimately off screen. The default pistol hinge is 3.5 units below the eye at
4 forward, about 41 degrees down, and a 90-degree horizontal FOV at 4:3 only reaches 36.9 degrees.

`mvm_debug` channels: `recoil.pivot.marker` (per frame while showing: pivot, world point, drawn
radius, edit serial), `recoil.pivot.project`.

Console surface:

```text
mirv_filmmaker weaponrecoil on|off|toggle
mirv_filmmaker weaponrecoil style style1|style2
mirv_filmmaker weaponrecoil amount|punch|rotation <0-150|off>
mirv_filmmaker weaponrecoil camerarot|camerapos|cameraroll <0-100|off>
mirv_filmmaker weaponrecoil snap|recovery <25-400>
mirv_filmmaker weaponrecoil pivot <0-30>
mirv_filmmaker weaponrecoil scope [all|slot|type] [<key>]
mirv_filmmaker weaponrecoil scopeclear [all]
mirv_filmmaker weaponrecoil test [<weapon>]
mirv_filmmaker weaponrecoil state
mirv_filmmaker pivotorb recoil|bob on|off|toggle
mirv_filmmaker pivotorb recoil|bob auto on|off|toggle
mirv_filmmaker pivotorb recoil|bob state
```

`snap` scales the spring return forces (higher means a harder snap back). `recovery` scales damping
(lower means looser, with more overshoot and wobble). `pivot` sets the rotation pivot distance in units
(0 means rotate about the camera). `weaponrecoil test` works on a paused demo. The test arms a
wall-clock preview that runs the springs until they settle, then hands the clock back to demo time.
Real playback shots never touch the wall clock, so exports stay deterministic.

Config panel: CAMERA tab, "WEAPON RECOIL" card.
`mvm_debug` channels: `weaponrecoil.springs` (per-shot impulses and throttled offsets) and the
`viewfx.vm.caller` line's `kickPos`/`kickAng` fields.

## Weapon pivot

Where the viewmodel hinges when an effect rotates it. The hook can only rotate the viewmodel about
the camera, which sweeps the whole gun across the screen. Weapon Recoil and Weapon Bob therefore
rotate about a point out on the weapon, and translate back to compensate with the exact residual
`(d - R*d)`.

`Movie/WeaponPivot.{h,cpp}` holds one store per effect. Weapon Recoil and Weapon Bob each get their own
ALL base, their own per-class overrides and their own marker colour, indexed by `WeaponPivotOwner`.
They are tuned for different motions. A kick reads best hinged near the shoulder so the muzzle climbs,
while a walk cycle often wants the hinge further out. Each has its own numbers, so a recoil adjustment
never retunes the bob.

Each store is scoped like the recoil tunings: an ALL base plus optional per-class overrides, and a
class override wins. Both start from the same measured defaults below.

Two entries are measured from the ARC9 Modern Warfare 2019 pack's per-weapon `CustomizeRotateAnchor`
(GMod (fwd, left, up) frame, with X scaled by 0.75 as ARC9 does). The rest are interpolated by how far
from the eye the weapon is held:

| Class | fwd | right | up | Note |
| --- | --- | --- | --- | --- |
| rifle | 10.5 | 3.0 | -4.0 | measured (M4) |
| shotgun / sniper | 11 / 13 | 3.0 | -4.0 | |
| smg | 7.0 | 2.6 | -4.0 | |
| pistol | 4.0 | 2.25 | -3.5 | |
| knife | **-7.5** | 2.25 | -4.0 | measured; behind the eye |
| utility (grenades, C4, Zeus, healthshot) | 6.0 | 2.5 | -4.0 | carried close, not aimed |

The knife's sign flip is the important one. Rotating a weapon held at the shoulder about a point ten
units in front of the eye throws it across the screen. The class comes from the spectated player's
held weapon each frame (`WeaponPivotClassOf`). Melee and utility are matched by name first, because
Weapon Recoil's classifier has no knife member (it describes how a weapon fires). The guns then go
through that classifier, so the two features can never disagree about what an MP9 is. A gun that
classifier does not recognise defaults to the rifle hinge. An unknown firearm is still a firearm.

Both the WEAPON RECOIL and WEAPON BOB cards carry a PIVOT section, built from one JS factory
(`pivotSection` in `ConfigHudCameraTabJs.h`), so the two can never drift apart. It has a class strip with
only the weapon in shot selectable, Forward / Right / Up sliders for it, a note naming that weapon, Reset
Pivots, and that effect's marker toggle labelled with its colour. State reaches the panel as two
prefixed blocks, `pvr*` for recoil and `pvb*` for bob.

The panel does not expose the ALL base for editing (see the "NO All CELL" note in `pivotSection`), and every class ships with an override. Reset Pivots restores the defaults in the panel. `mirv_filmmaker pivot <owner> clear <class>` clears a class override from the console.

`mirv_filmmaker weaponrecoil pivot <n>` writes the recoil store's ALL forward distance.

The owner is the first argument and is required. There are two hinges, and a command that guessed which
one you meant would be the fastest route back to them drifting apart unnoticed:

```text
mirv_filmmaker pivot recoil|bob all <fwd> [right] [up]
mirv_filmmaker pivot recoil|bob class <rifle|smg|pistol|shotgun|sniper|knife|utility> <fwd> [right] [up]
mirv_filmmaker pivot recoil|bob set <all|class> <fwd|right|up> <value>   (what the sliders send)
mirv_filmmaker pivot recoil|bob clear <class|all>
mirv_filmmaker pivot recoil|bob reset | state
```

## Weapon bob

The carried-weapon feel: what moving and looking around do to the gun. It moves the weapon only,
through the same `CalcViewModelView` input hook as Weapon Recoil, so CS2's own animation composes on
top. It is not view bob.
Three terms, all from ARC9 (`lua/weapons/arc9_base/cl_sway.lua`, `cl_vmposition.lua`), the base the
"ARC9 Modern Warfare 2019" pack runs on:

- **Walk bob** -- ARC9 bob style 0 ("Bread & Darsu", the convar default). Amplitude-modulated
  sines: each channel's carrier is multiplied by a slow second sine, so the figure drifts instead of
  repeating. Walk and sprint branches are blended by speed. A CS2 run maps onto the sprint figure
  (the larger shape), but it is not scaled down the way MW2019's `BobSprintMult = 0.1` scales its
  tactical sprint. CS2 has no such state.
- **Look sway** -- `GetViewModelSway` + `GetViewModelInertia`: smoothed aim velocity, clamped
  (pitch +-10, yaw +-4, roll +-2), rotated about the hinge by the negative of the turn, so the gun
  lags behind the aim, plus a slower "inertia" overshoot. ARC9's own inertia term cannot leave
  zero (its approach rate is proportional to its own value, which starts at 0), so the intent is
  implemented with a long half-life instead.
- **Idle breath** -- the at-rest drift from `GetViewModelPosition`.

Per-class walk and sprint multipliers (`WeaponBobProfileFor`) calm the weapons held with the hands close
together: pistol 0.55, SMG 0.62, knife 0.38, rifle 1.0. The hinge is Weapon Bob's own pivot store
above, independent of Weapon Recoil's.

**Airborne and crouch come from the pawn, not from the camera.** The gait fades and the walk clock
slows in the air. Crouching multiplies the vertical channel, and the jump term lifts and dips the gun
on takeoff and landing. The camera's own vertical motion cannot tell a staircase, a kerb or a weapon lying on the floor from a jump or a duck. The stack reads
`FL_ONGROUND`, `FL_DUCKING` and the vertical velocity from `m_fFlags` / `m_vecVelocity`
(`SampleBody` in `CameraShakeStack.cpp`; `ShakeInput.haveBody`). Ground contact must be lost for
60 ms before it counts as airborne (`WeaponBobAirTimeAdvance`), because walking down stairs drops the
flag for a tick or two. On the ground, the jump term reads zero. The pawn is held across frames
(`ResolveBodyPawn`), because the spectated-pawn lookup (the pawn eye nearest the camera, within
12 units) can drop a frame during a spectator switch or a seek. A pawn whose flags
are all zero is dead or unspawned and counts as no pawn. With no pawn, the position-derived estimate is
the fallback, with the climb a slope explains taken out (`WeaponBobFreeVz`).

Determinism: everything integrates on the demo clock. Two framerate hacks in the ARC9 source,
`sightmult = 0.5 + clamp(1/ft/100, 0, 5)` and the per-frame aim deltas, use a fixed 100 fps reference, so a render is identical at any rate. The walk carrier is its own accumulator
(`WeaponBobAdvanceStepArg`). With a smoothed crouch, the stride
frequency slides from 9.25 to 6, and advancing the argument with that frequency would sweep through about 300 radians. That
is the crouch jitter the accumulator avoids. Strafe roll is smoothed at 0.2 s so counter-strafe taps average out.

`player_footstep` arrives during demo playback (176 in five minutes), so Step Sync pulls
the carrier onto real footfalls. The pull is partial, never a snap.

The maths is in `WeaponBobMath.h`, unit tested by `WeaponBobMathTests` (CTest). The CONFIG card is
"WEAPON BOB", under WEAPON RECOIL.

```text
mirv_filmmaker weaponbob on|off|toggle
mirv_filmmaker weaponbob amount|sway|tilt <0-150>
mirv_filmmaker weaponbob looksway|inertia|idle <0-150>
mirv_filmmaker weaponbob camera <0-100> | stepsync <0-100> | rate <25-400>
mirv_filmmaker weaponbob class <class> <walk> [sprint] | classmult <walk> <sprint>
mirv_filmmaker weaponbob state | reset
```

## Shared diagnostics

```text
mirv_filmmaker camshake state
mirv_filmmaker camshake reset
mirv_filmmaker camshake off
```

`state` prints the shared motion tracker plus Handheld and Weapon Recoil diagnostics.
`off` disables both modules.

## Known limitation

Smooth View's breathing and health oscillators still advance on accepted demo ticks, while their
offset is reused on every render frame. At render rates above the demo tick rate, they can look
stepped. The clock that fixes this already exists: `CameraShakePhaseClockMath.h`, continuous for
interactive preview and locked to demo time during export. Smooth View does not use it.
Weapon Bob and the rest of this stack already use it.
