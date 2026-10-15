# Death Animations

The **Advanced Ragdolls** card on the CONFIG panel's RAGDOLLS tab. A dying player plays a baked
death animation with limb-clearance corrections for CS2 bodies, and the native ragdoll takes over
once the clip is over.

Code: `AfxHookSource2/Filmmaker/Movie/DeathAnimations.{h,cpp}` (engine side) and
`DeathAnimationsMath.h` (selection, placement, sampling, limb clearance, release velocity; tests in
`Automation/tests/death-animations-math-tests.cpp`).

## Behaviour

- **The clip drives the pose.** Every frame, each of the victim's fifteen ragdoll bodies is placed on
  the sampled, corrected transform and given its velocity at that moment. Gravity, contacts and joints
  overwrite whatever they did since the last frame. The first 0.2 s blend in from the pose the player
  died in, as Call of Duty blends a death animation in. Several donor deaths start mid-reaction.
  CoD's shotgun deaths lean 28 to 37° at frame 0, so the blend keeps the body from snapping sideways.
- **Limb clearance.** Rounded volumes approximate the chest, vest, pack, thighs and shins. Arms are
  corrected away from the torso and both legs. Each leg is corrected away from the other. Corrections
  rotate the limb segments, preserving their sampled lengths, and turn the hands and feet with them.
  The torso follows the pelvis and shoulder midpoint, with an elliptical cross-section. Extending the
  lower spine missed the chest in hunched poses. Clearance is deliberately tight so it catches only
  real pass-through: a 5 x 5.5 torso (half-width x half-depth), 3.2 thighs, 2.4 shins and 0.5 to 0.8
  units of joint padding. A hand resting on the vest is left alone. Measured over all 449 clips at
  60 fps through the game's own sampler, corrected time is 22.4%, the typical hand move is 0.9 units,
  and clips with a visible pop (over 1.5 units between frames) number 2.
  `bd_death_leftarm_short_01` and `_03` still pop, because the donor drives the hand through the chest.
  Hand and foot probes follow their actual orientations, including bent wrists, and reserve space for
  the glove around the wrist. Rounded ends avoid an abrupt push at flat ends.
  A correction track, sampled at 120 Hz, smooths exit-direction changes near a capsule's centre.
  Additional rotations are limited to 8 radians/s. The track is built once per clip on first use and
  sampled by clip time, so seeking and playback order give the same pose. The corpse and the picker
  preview share this sampler. Authored motion is not temporally filtered.
  The volumes approximate the body. They are not mesh collision. Unusual gear and tightly folded poses
  can still intersect, and smoothing can leave brief penetration while a correction changes direction.
- **The bodies stay dynamic.** Switching them to *keyframed* would stop physics fighting the clip. CS2
  then skips drawing the corpse on most frames (burst captures showed it on roughly one frame in three).
  Writing once per tick or after the client's frame handler does not help.
- **Then the ragdoll.** With a blend out (the default) the clip's hold on the body fades to zero over
  the last seconds, so the ragdoll is already carrying it when the clip ends. With a blend out of 0,
  the ragdoll starts from the final frame and carries the velocity of the last clip frame, capped at
  600 u/s and 1440 °/s. This is the Call of Duty 4 handoff (KisakCOD `src/ragdoll`,
  `Ragdoll_UpdateVelocityCapture`).
- **Placement.** The clip is turned about the vertical axis so its hips face the way the corpse's hips
  face. A weighted fit of every body is the fallback when the hips are degenerate. A whole-body fit is
  pulled sideways by clips whose frame 0 already leans. The clip's pelvis goes over the corpse's pelvis.
  Its frame-0 ankles go on the victim's lowest ankle at death. The map floor under the pelvis replaces
  that height only within 6 units of it, because the map ray can pass through a floor and land on the
  level below. Nothing is scaled or tilted.
- **Timing** follows demo time. Pausing holds the pose, and slow motion slows the clip. Seeking or
  turning the feature off hands every playing body to physics at once.

## Playback

**Exact playback is the default** (Solid body off). The clip is placed every frame, so it plays as
  authored. Every physics-driven variant (below) look wigglier and stiffer than
the clip itself. The world is handled without physics:

- **Ground track** (`BuildGroundTrack`): a vertical offset for the whole pose, computed once per clip
  from the clip and smoothed with a zero-phase filter. It lifts the pose whenever a body would go into
  the floor (a body's origin closer to the floor than its `kGroundRadius`). Once the clip is down
  (pelvis below 18), it also lowers the pose until the lowest body rests on the floor, so there are no
  sinking knees and no floating corpse. It starts at 0, so frame 0 stands where the victim stood. It
  also caps rebounds: the hips never rise more than 4 units above their lowest point so far
  (`kReboundAllowance`). The CoD explosion clips animate a crouch and then the body hurled up (hips 43,
  21, 44, then 11 units in `cod_explosion_death_r1`). On the ground, that read as a falling corpse
  pulled back up to standing. The grenade flight does the actual lifting. Small natural bounces (gun clips,
  1 to 4 units) stay.
- **Grenade flight:** a blast death plays its clip along a flight (`BlastOffset`). The kick is 140 u/s
  out and 150 up at Grenade blast 100 % (scaled by the square root of the slider, so 200 % is ~1.4x).
  Air drag bleeds off the sideways speed (2/s), and gravity brings the body down. The flight lasts about
  0.8 s, reaches 29 units high and covers 55 units out. It is modest because many explosion clips animate
  their own launch. The flight moves the whole blended body, so it leaves from frame 0. Physics takes
  over with the flight's motion when the body comes back to the ground (`why=blast-landed`).
- **Shot limbs** (arm or leg hits) are left to physics for the whole clip. The rest of the clip plays.
- **Fitted to the map before it is picked** (`EnvironmentFit`): each candidate clip is probed at 6
  moments: the floor under its hips, head, hands, calves and feet (stairs, curbs, slopes), and a
  sideways ray along where its hips and head travel (walls). The first candidate that goes no more
  than 3 units into anything plays. Otherwise the best of the first 8 plays. Raw samples and a few dozen
  rays per candidate keep picking from hitching. The `play` line logs `fit=<units>(tried <n>)`.
  Clips chosen by hand (queue, per-player, forced) are not re-picked.
- **Every hand-off crossfades.** The blend out runs before every hand-off, not only at the clip's end.
  A clip that lands ends at the landing, so the last half second of the fall crossfades into the
  ragdoll. A contact or balance loss starts the same crossfade from that moment (`FadeOutWeight`,
  `deathanim fade ...` in the log) instead of cutting.
- **Speed carries into the ragdoll.** Through the blend out, each body's motion is its real physics
  velocity blended with the clip's, so the ragdoll starts at the speed the animation was moving.
  Scaling the clip's velocity toward zero would slow the body, and then gravity would grab it.

How the clip moves the bodies, from the card's sliders (`mirv_filmmaker deathanim ...`):

| Setting | Command | Default | Effect |
| --- | --- | --- | --- |
| Blend in | `blendin <s>` | 0.2 s | time from the death pose into the clip |
| Animation strength | `strength <percent>` | 100 | muscle strength: how fast the clip may change the body's motion (solid); share of the way placed (exact) |
| Grenade blast | `impact <percent>` | 100 | how hard a grenade throws the body, out and up; gun deaths are never pushed (solid only) |
| Blend out | `blendout <s>` | 0.5 s | the clip's hold fades to zero over its last seconds of motion. Playback ends where the clip's motion ends (`MotionEndTime`: the last moment any body moves faster than 20 u/s), before the still tail many clips finish with |
| React to walls | `walls on\|off\|toggle` | on | limbs pushed by something bend, pushed hard they go limp; the torso pushed stops the clip |
| Solid body | `solid on\|off\|toggle` | off | bodies are steered by velocity so floors and walls stop them; off places them exactly, through anything |

**Solid body.** Placing a body on its pose every frame (`SetTransform`) ignores the world. The solver
pushes a knee back out of the floor during its step, and the next frame places it straight back in.
The solid drive never places a body. It gives each one the linear and angular velocity that carries it
from where physics has it onto the clip pose one frame ahead (`DeathAnimationsMath::DriveVelocity`,
the kinematic-target velocity of Jolt's `MoveKinematic` and active-ragdoll velocity matching). The
solver integrates that velocity through its contacts, so a limb aimed into the floor stops on it and
the animation carries on.

- **Aim across the physics tick.** The target is the clip pose `kPhysicsStep` (1/64 s) ahead. The
  velocity covers the gap in one tick. Aiming across a 4 ms render frame would ask for four times
  the velocity. Aiming across the ~7.5 ms at which bodies are seen to move still spins the pelvis about
  twice as fast as commanded, and the ragdoll can blow apart, after which CS2 stops drawing it. The engine
  integrates a set velocity over its whole tick.
- **Angular velocity is world axes, degrees/s.** vphysics2's setter (body slot 68) multiplies by
  pi/180 itself. A probe of commanded against produced pelvis rotation agreed with world axes
  (cosine 0.6 to 0.8), and gave only about 0.2 against local axes.
- **Caps.** 2500 u/s and 3600 °/s. A body far from its pose is never placed back. At low strength that
  is the point, since placing lagging limbs would blow the ragdoll up at 10 %.
- **Hitting something.** Contact is judged on frames right after a physics step, against where that
  step should have left the body. A LIMB pushed over 10 units by the floor or a wall for two steps
  goes limp, and the clip plays on. The TORSO pushed over 6 hands the corpse to CS2's own ragdoll
  physics for the rest (`handoff ... why=contact`, `SolidContactRules`). The CS2 ragdoll cannot reach
  every clip pose exactly, because its bone lengths are not the donor's. Free-space pushes therefore
  reach about 4 on the torso and 6 on limbs.
- **Landing ends the clip.** For a clip that ends lying down, `LandTime` finds the first moment its
  torso is within 3 units of the height it ends at. Playback hands off there (`why=landed`). The clip's
  lying pose often sits a few units above the CS2 floor. Playing on would hold the corpse floating until the
  clip ends, and then drop it.
- **No jitter from the drive.** Three measured causes (reversal counts per second, `deathanim.motion
  jitter=actual/clip`) are handled as follows.
  (1) Asking for "the whole gap in one physics tick" while the muscles limit acceleration makes
  bodies overshoot and oscillate around the pose. The drive uses a braking curve (`TrackVelocity`: the
  clip's velocity plus a correction capped at sqrt(2 * accel * error), and at most a quarter of the error
  per physics tick). The pose it reads is a tick old. Closing all of it per tick rings at ~10 reversals/s
  even at full strength. 0.25 is the largest gain with no oscillation for a one-tick delay.
  (2) Placing a body part-way (strength below 100 %, the blend out) every render frame while physics
  moves it every tick makes a sawtooth. The velocity drive is used whenever the weight is below 100 %,
  and exact placement only at 100 %.
  (3) Re-running the muscles every render frame lets the command ramp 3 to 4 times per tick, because the
  engine hands back our own last write as the body's velocity. The joints then throw it back at each
  step (a hand's z command +3, +15, +21, then -24). The drive writes one command per physics tick and
  leaves it standing until the solver consumes it.
- **Judge jitter on the drawn pose.** In exact playback, the read-back at the start of a frame (what
  `deathanim.motion jitter=` samples) is the pose right after a physics step. Joints and the held velocity
  push it 1-3 units off the clip, and the next hold puts it back. That sawtooth is not drawn. The renderer
  uses the held pose (`deathanim trace 12` logs `deathanim.drawn`, the right-hand attachment just before
  the draw). Measured on `mw_stand_death05_lleg_med_8`: 0-1 frame-to-frame reversals of the
  drawn hand in exact 100 %, exact 50 % and solid 100 %, against 44-61 in the exact read-back.
- **The blend in keeps the death's momentum.** It starts from the death pose, still moving at each body's
  death velocity, and coasts to a stop over 0.08 s (`CarriedOffset`, capped at 400 u/s). A frozen pose at
  t = 0 would make the clip reverse the body out of it. Grenade deaths skip the blend in, because their
  launch replaces it.
- **Strength is muscle strength.** Each body keeps the motion physics gave it (read from the engine:
  linear velocity from body slot 66, angular from the simulation object after it). The clip may only
  change that motion as fast as the muscles can accelerate (`MuscleVelocity`): 6000 u/s² at 100 %
  (15000 °/s² for rotation, kept weak so the joints settle orientation instead of being forced into
  hyperextended, stiff poses). The limit falls with the square of strength (`MuscleLimit`): 540 at 30 %,
  barely above gravity, and 60 at 10 %, where the body is a ragdoll the clip nudges. A share-of-the-gap
  blend cannot do this. It still reaches the pose within a few frames, so 10 % looks like 100 % with
  jitter. The blend out weakens the muscles to zero.
- **Only grenades throw the body.** A blast death replaces CS2's own launch (73 to 299 u/s up and
  180 to 320 sideways measured) with the same modest kick as exact playback: 140 out and 150 up, scaled
  by the square root of **Grenade blast** (`ImpactSpeed`, `BlastScale`). Spin is kept but capped at
  720 °/s. Stacked on the engine's launch, the bodies hang in the sky. Topping up to the engine launch at
  any value above 0 still flies the full launch (at 25 % the bodies travel 105 to 192 units). 0 plays the clip in place.
  The direction is the engine launch's. It points away from the blast. Grenade deaths carry no attacker
  position, so a facing fallback would throw one grenade's victims in unrelated directions.
  The clip drives a thrown body only in the air. Once the hips reach the floor, the crossfade starts
  (`why=landed`), because clips with leg motion walked a landed body around. While it flies, the pelvis is
  pure physics and the clip only poses the other bodies around it. Their targets follow where the pelvis will
  be after the next step, so the clip's own animated jump cannot hold the body up. The flight ends on
  touching the ground (contact), not at the clip's own landing time. Every body's sideways speed decays
  while it flies (`FlightDrag`, 2.5/s). CS2's launch reaches ~330 u/s sideways, and undamped, bodies would land
  like sleds and slide across the map. On the ground (pelvis within 14 units of the floor), the decay is
  12/s, and the clip lets go with a fifth of whatever sideways speed is left. Gun deaths are never pushed,
  because pushed bodies fight the clip and look worse than playing it in place. Solid only.
- **A shot limb goes limp.** An arm or leg hit (hitgroups 4-7, `ShotLimb`) is physics for the whole
  clip; the rest of the animation plays.
- **Losing balance ends the clip.** If physics tips the torso line (pelvis to head) more than 35°
  from the clip's for two steps (`TorsoTilt`), the clip lets go (`why=balance`). Steering the body back
  would make pushed bodies pop back up and fight to stand.
- **Known limit.** The joints and the clip's rotation targets disagree slightly. The angular drive
  often runs at its cap, so the pose tracks the clip less tightly than exact playback.

**Contact.** After the physics step, a body should sit where it was aimed. The distance the world
pushed it from there is its push. Limbs and the head are judged relative to the pelvis, because the whole
body drifts 1 to 2.5 units together from frame timing. The torso is judged absolutely. Limbs give way
in proportion between 1.5 and 6 units and go limp above that. A torso push over 4 for two frames stops
the clip, and physics takes over. Contact is not judged during the blend in or blend out.

`mvm_debug` channels: `deathanim.frame` (every 15 frames: mode, push per group, give, limp, peak
commanded velocities), `deathanim impact` (the push given and the engine's own death velocity),
`deathanim handoff` (why the clip ended, largest push) and `deathanim.settle` (2.5 s after
the hand-off: how far each body dropped and its rest height above the floor).

## Clip choice

The card's **Animations** row picks one source at a time. Every clip lives in one baked library,
`resources/ragdoll/ragdoll_animations.mvmanim` (built by `FX/tools/extract-death-anims.py` as
`FX/death-animations/ragdoll_animations.mvmanim`). The file stores each clip's source as an explicit field:

| Source | Clips |
| --- | --- |
| MW2019 | Modern Warfare 2019 |
| Classic CoD | World at War, Black Ops 1 and 2, MW3 |
| Extra | The GMod death pack |

Every clip carries four **game tags**. The Blender viewer's Death Anims panel shows them for the
current clip under *Game tags* and lets you change them:

| Tag | Values | Default guess (pack builder) |
| --- | --- | --- |
| Body area | head, neck, torso, left/right arm, left/right leg, explosion, fire, melee (knife kills), generic, manual | the donor's own grouping by name; running deaths by where the hit landed (MW2019 `lleg`, `torso`, `explode`), else generic |
| Hit from | any, front, back, left, right | a name that states it (CoD `_front`/`_back`/..., MW2019 numpad `_8` front, `_2` back, `_4` left, `_6` right), else which way the upper body falls |
| Stance | standing, crouched, prone, running | the name (`prone`/`crawling`, then `crouch`, with crouch-runs included, then `run`/`jog`/`sprint`, then `stand`), else frame-0 pelvis below 30 units |
| Weapon | any (universal), shotgun, sniper, heavy | `shotgun` or `flyback` in the name is shotgun, `sniper` is sniper; MW2019 `_lg` (large-calibre hit) is heavy, which answers shotgun and sniper kills; everything else is universal |

A change in Blender rewrites that clip's tag in the baked file immediately and is recorded in
`FX/death-animations/clip-tags.json`. The builder applies that file after its guesses, so a
rebuild keeps every correction.

At death the game works out the same three things:

- **Stance:** crouched when the victim's ducking flag (`m_fFlags` bit 1, sampled every frame while
  alive) is set, with the death pose as a cross-check (pelvis less than 27 units above the lower
  ankle). Otherwise running when the victim moved faster than 130 u/s (CS2's walk speed), else standing.
- **Body area:** fire, explosive and melee first. Then a headshot or the hit location from
  `player_hurt`'s hitgroup. Speed never changes the body area. Speed sets the stance.
- **Hit from:** the attacker's position against the corpse's facing (from the hips).

The pick is strict (`DeathAnimationsMath::SelectPool`):

1. **Stance and source never relax.** A crouched victim only ever gets crouched clips, a running victim
   only running clips, and a standing victim only standing ones. If the chosen source has no clip for the
   stance, the death is left to the normal ragdoll. Manual and prone clips are never picked.
2. **The killing weapon's clips come first.** A shotgun kill (Nova, XM1014, MAG-7, Sawed-Off) tries
   shotgun clips, then heavy, then universal. A sniper kill (AWP, SSG 08, SCAR-20, G3SG1) tries sniper,
   heavy, then universal. Any other kill uses universal clips, and heavy ones only when nothing universal
   fits. A weapon tier with any clip for the stance wins, whatever its body area or side.
3. **Body area outranks hit side.** Within the exact body area, the pick takes a clip for the exact side,
   then an `any`-side clip, then another side. Only then does it fall back to the next body area: neck to
   head, fire to explosion, and then torso and generic for everything.
4. Among equally good clips, one not used in the last eight deaths wins. The pick is seeded by demo tick
   and player, so a seek replays the same death.

The `play` log line reports each part, for example `stance=crouched(clip crouched, duck 1/1, pelvisAboveAnkle 25.4)`,
`weapon=xm1014(shotgun, clip shotgun tier shotgun)` and `match=exact|any-side|other-side|fallback-area`,
along with the size of the pool.

## Per-player animations

The card's **PER-PLAYER ANIMATIONS** button opens a picker over the Config panel (design 8c,
"body map"): the filters on the left, ten clip cards per page in the middle, and on the right a
looping preview of the selected clip above the players to assign it to. T is always on the left, and CT
is always on the right. Players are never interleaved. **Assign to <player>** makes every death of that
player play the clip. **Use automatic pick** hands the player back to the rules above. A player's clip is
always used, and stance and weapon rules do not apply to it. The `next` queue still goes first.

- Players are keyed by steam id (bots by name), so an assignment follows the player across rounds
  and demos. Assignments persist as `deathAnimPlayers` with the other RAGDOLLS settings.
- The hit area is a **body map**: head, neck, torso, both arms and both legs as non-overlapping
  regions (the player faces you, so their right arm is on your left), each showing how many clips it
  would list under the other filters. The causes that are not a place on the body (Explosion, Fire,
  Melee, Generic, Manual) are a row of chips with the same counts. Clicking a region or chip again
  goes back to all areas, and `CLEAR` on the HIT AREA rule resets every filter.
- The other filters (Source, Stance, Weapon) are segmented strips. They show only the tags the library
  carries. No clip is prone and none is sniper-only, so those two are not offered.
- The preview is the selected player's own agent playing the clip on a loop, facing the camera,
  and it plays while the demo is paused. `deathanim view <clip>` starts it; the picker sends it.
  The character is a MapPlayerPreviewPanel (the Customize Player modal's panel);
  `Panorama/DeathAnimPreview.cpp` poses its skeleton every late frame. It drives the fifteen
  ragdoll bones from the clip, and every other bone follows its nearest driven ancestor
  (`DeathAnimationsMath::FollowDriven`). The engine draws from the parent-local pose at
  `CModelState+0x18`. A write to the world bones at `+0x80` never reaches the screen. Bone names
  and parents come from the model (`CModel+0x168`, `+0x180`). The preview does nothing when
  a CS2 update moves those tables, and `deathanim view status` says why.
- The preview character holds no weapon. Its gun is bone-merged onto the `weapon_*` bones, and the
  preview sinks those bones under the floor. Hiding the weapon entity with a mesh-group mask of 0
  crashes CS2 in `UtlVectorMemory`.
- While it is open the picker takes the mouse from the game
  (`UiToolkitAPI.AddDenyInputFlagsToGame(..., 'CaptureMouse')`, as CS2's scoreboard menu does), so a
  click on empty space cannot reach the spectator's player switch. Closing the picker, or Config
  closing under it, releases it.
- The preview panel never reaches the engine's ready call, so while it is open the paused-demo
  draw bypass (`mvm_previewrender`) arms every submitted 3D panel, not only the ready one.
- The picker's layout is fixed: changing a filter never resizes or moves anything else.

Deleting a clip in the Blender viewer takes it out of the baked file and records it in
`FX/death-animations/removed-clips.txt`, which the pack builder honours ([FX/README.md](../../FX/README.md)).

## Commands

```
mirv_filmmaker deathanim on | off | toggle
mirv_filmmaker deathanim source mw2019 | classic | extra
mirv_filmmaker deathanim clip <name> | off     every death plays this clip (testing)
mirv_filmmaker deathanim next <name> [...]     the next deaths play these, in order
mirv_filmmaker deathanim list [source]
mirv_filmmaker deathanim status
mirv_filmmaker deathanim armcheck [top]        rank clips by frames needing limb correction
mirv_filmmaker deathanim reset
mirv_filmmaker deathanim player [<key> <clip|auto>]   per-player clip; no arguments lists them
mirv_filmmaker deathanim catalog                     picker clip list (the Config panel asks for it)
mirv_filmmaker deathanim view <clip> | off | status  the clip on the picker's player-model preview
mirv_filmmaker deathanim trace <body> | -1           per-frame drive trace of one body (mvm_debug)
```

`deathAnimOn` and `deathAnimSource` persist with the other RAGDOLLS settings.

`armcheck` scans raw authored frames against the clearance volumes and lists the clips with the
most affected frames (15 by default). It is a shortlist for visual checks, not a count of remaining
mesh intersections after correction. Despite the command name it checks both arms and legs.

## Engine facts

vphysics2 `CPhysicsBody` vtable (installed build; each slot is verified by its implementation
prefix before use):

| Slot | Call |
| --- | --- |
| 3 / 4 | set / get motion type (1 keyframed, 2 dynamic; the clip keeps 2) |
| 53 | set transform (position, scale, xyzw rotation) |
| 56 | get transform: position floats 0 to 2, scale 3, quaternion 4 to 7 |
| 64 / 66 | set / get linear velocity (inner `+0x108`) |
| 68 / 70 | set / get angular velocity, degrees/s |

The bodies are found through the pawn's model-state `m_pVPhysicsAggregate` (fifteen bodies,
`GetBody` at aggregate slot 24), or its client-side ragdoll child.

## Diagnosis

`mvm_debug start`, then read the `deathanim` lines:
- `death`: the event and its facts;
- `play`: the clip, body area, side and stance (the death's and the clip's), how exact the match was,
  the pool size, yaw, floor and carrier;
- `deathanim.frame`: the pelvis target against its read-back and motion type, every 15 frames;
- `deathanim.clearance`: the first-use correction track's clip, sample count, rate and rotation limit;
- `deathanim.render`: the rendered right-hand attachment against the hand body after the client's
  frame handler, for the first 45 frames of each clip;
- `deathanim.motion`: per finished clip, how far the hips and head moved against the clip, and
  jitter as velocity reversals per second (actual/clip, `perBody=` counts for pelvis, head, hands,
  arms, foot); `deathanim.reversal` logs each reversal with its time and weight;
- `deathanim.trace` (`deathanim trace <body>`, `-1` to stop): that body's read-back, target and
  commanded velocity every frame; `trace 12` also logs `deathanim.drawn`, the drawn right-hand
  attachment against the held pose, until the clip ends;
- `release`: why the clip ended, with the frame count and motion-type re-asserts;
- `skip` / `lost`: why a death was left to physics.
