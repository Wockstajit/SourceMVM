# Nervous System

The **NERVOUS SYSTEM** sub-tab sits inside **ADVANCED RAGDOLLS**, beside **DEATH ANIMATIONS**, on
the CONFIG panel's RAGDOLLS tab. The Advanced Ragdolls master switch gates both features. A shot
player keeps reacting for a few seconds before the body goes still: a hand goes to the wound, the
chest heaves, and the limbs writhe and twitch. A headshot does none of this. The body goes limp at
once.

Code: `AfxHookSource2/Filmmaker/Movie/NervousSystem.{h,cpp}` (engine side),
`NervousSystemMath.h` (injury profiles, consciousness, IK, layers, twitches, muscles; tests in
`Automation/tests/nervous-system-math-tests.cpp`). Corpse body access is shared with Death Animations
through `Movie/RagdollBodies.{h,cpp}`.

## What the body does

The hit location from `player_hurt` (and `player_death`'s headshot flag and weapon) picks the reaction:

| Hit | Reaction |
| --- | --- |
| Head (headshot) | nothing: limp at once |
| Neck | both hands to the throat, gasping breath, a short life |
| Chest | both hands on the upper chest; sometimes one knee drawn up |
| Stomach, knife | both hands on the stomach, both legs drawn up (the body curls) |
| Left / right arm | that arm hangs limp; the other hand clutches it |
| Left / right leg | both hands reach for the injured thigh, and that leg is drawn up to meet it |
| Grenade, fire | no wound to hold: the legs pedal, the body thrashes and twitches harder |

Not every layer plays on every death. Each death picks its layers from the enabled ones
(`PickLayers`, seeded per death). Reaching for the wound always plays when enabled. Rolling over also always comes with
it, because the roll exposes the wound, even when the Roll over setting is off. Of breathing,
writhing and twitches, at most two play, and the pair changes from death to death. With every layer
running, the body looks like it is thrashing.

On top of the reaction, a living body can also have:

- **Breathing**: the chest and head lift along the chest's facing. It is quick at first (1.1 Hz),
  slows as consciousness goes (0.3 Hz), and ends with one last heave 0.6 s before the body goes still.
- **Writhing**: smooth noise on the hands, forearms, knees, feet, spine and head, with a period of
  about 1 s. Arms are softer than legs and back, following Euphoria's `bodyWrithe` defaults.
  The head rolls slowly side to side about the spine.
- **Twitches**: occasional kicks of a hand, foot or the head, more often as the body fades (agonal
  spasms). A limb that is reaching, curled or shot is never twitched.

**Consciousness** runs each reaction. It stays at 0 for 0.2 s (Euphoria's `timeBeforeReachForWound`),
ramps in over 0.4 s, and stays full for the first 30 % of the body's life. It then fades linearly to
0, as Overgrowth's injured ragdoll fades toward its time of death. A body still moving fast has
little say. Its strength is multiplied by `clamp(2 - speed/130, 0.2, 1)` (Overgrowth's speed gate,
converted to units/s), so a falling or tumbling body is not steered.

Life = **Duration** x the injury's share x a random 0.75-1.25 per death. Neck 0.7, blast 0.8,
fire 0.9, chest 1.0, stomach 1.2, arm/leg 1.3. Everything is seeded by demo tick and player, so a given
death reacts the same way each time it plays.

## How the body is moved

Limbs are moved by **nudges**. Once per physics tick, starting from wherever physics left the body,
the limb is placed a little further along its motion (`SetTransform`, body slot 53). Its own native
linear and angular velocities are preserved. The floor, walls and other bodies push the limb during
the step, and the next nudge starts from where they left it. Solver correction opposing a nudge
reduces its weight over 0.08 s; unblocked drive recovers over 1.5 s. A small integration allowance
prevents ordinary settling from being treated as a blocked limb. This reduces repeated pushing at
contacts and joint limits. Pelvis velocity is not broadcast to the limbs, which could amplify a small
contact impulse into whole-body drift.

- **Chains.** An arm or leg moves as a chain. The upper segment turns about the shoulder or hip toward
  its elbow/knee goal. The lower segment turns about the elbow or knee toward its wrist/ankle goal,
  and the hand or foot turns with it (`SwingVelocity`, `LowerSwing`, `NudgeChain`). The lower
  segment's world turn subtracts the upper segment's contribution, so it is not applied twice.
  Turning about the joint pivots
  keeps the ragdoll's joints connected. The chest and head are nudged along their offsets.
- **Goals.**
  - *Reach.* The wound point lies on the hit body's line, pushed out to the skin toward the chest's
    front and a little toward the reaching shoulder. Limb targets use the surface facing the hand;
    if the nominal point is too far away, the target moves along the same injured limb to a closer
    contact point. An injured leg curls farther toward the hand. Lying face
    down, the chest is against the floor, so torso reaches wait until it rolls over. Limb wounds
    are approached on their exposed side.
  - *Hands.* Two hands sit 2.5 units apart. The wrist stays 5 units back from the skin, and the hand
    pulses 0.8 units against the wound. Wrist clearance is measured outward from the skin.
    A hand behind the torso approaches via a flank waypoint. After clearing it, the hand stays on
    the wound route unless it actually goes behind the torso again; the pressing pulse cannot
    toggle it back and forth across the waypoint threshold. Forearm nudges that reduce torso
    clearance below 6.5 units are rejected. This is a torso capsule approximation, not a mesh test.
  - *Two-bone IK.* IK from the current shoulder or hip keeps each goal between 50 % and 97 % of the
    limb's length. A reach is never asked to stretch, which would drag the corpse, or to fold shut
    past a ragdoll joint. Arm IK chooses a nearby elbow bend with clearance from the torso and floor.
    A blocked reach reserves its arm so writhing cannot take over and reverse the motion.
  - *Never force a hand* (`ArbitrateReach`). Re-placing a hand the solver keeps
    rejecting pushes it back about 1.8 units every tick. That push goes through the shoulder into the
    torso. Because the goal follows the torso, the body chases its own hand and slides 10-23 units along
    the floor in each reaction. An A/B test on one death measured 11.1 units of slide with all layers,
    6-7 with writhing off, and 0.9 with reach off.
    - A hand that **arrives** within 10 units of its goal **rests**. Nothing is written, and physics
      settles it against the chest. It drives again only if the wound moves more than 14 units away.
      Released within 5 units, the wrist springs back to 9-11 units within 3 ticks, because the joints
      and the hand-chest contact reject anything closer. Holding it there fights the solver. A single
      radius would flip the hand between rest and reach every few ticks, and each re-placement would shove
      the body again. Arrival is judged against the wound point itself, never the flank waypoint.
    - **No clearance, no reach** (`ArmPinned`). An arm the body lies on, or one behind the back, is
      never reached with and never relaxed: pulling it out levers the whole body over it, which spins the
      corpse. The forearm or hand counts as pinned when it is more than 3 units behind the back plane
      and within 8 units of the spine sideways, or below the torso's centre line within 7 units of the
      spine horizontally. An arm on the floor beside a body lying on its back is free. The check runs
      every tick before a hand drives. A pinned arm gives up for the rest of the reaction
      (`why=pinned`), physics keeps it, and the other hand reaches on alone.
    - A reaching hand is placed at most 0.6 units per physics tick (`CapPlaceWeight`). Re-aiming the
      whole arm after the solver shoves it back would re-place the hand several units in one tick,
      which shows as a pop.
    - A given-up arm is lowered to the body's side by the relax drive (below) for 1.6 s, instead of
      staying frozen wherever joint friction catches it.
    - A hand **gives up** for the rest of the reaction when it stays blocked away from its goal for
      0.6 s, or its goal stays out of arm's reach for 1.2 s. It also gives up at once, while still
      driving, if the pelvis has slid more than 6 units since steering began (a roll re-bases this).
      That arm goes limp and the other hand keeps reaching (`nervous giveup ... why=blocked|out-of-reach|slide`).
  - *Contact.* The wrist target advances smoothly in the torso's frame, so joint friction cannot
    reset the reach to its starting point every tick. The shoulder and elbow turn the connected
    ragdoll bodies toward that target, matching the elbow bend plane as well as its direction.
    Segment lengths stay fixed; the approach is capped at 80 units/s and fades with muscle strength.
    This never rewrites the renderer's local bone translations or scales. Chest, stomach and leg
    wounds use both hands; an injured arm stays limp while the other hand reaches for it.
  - *Leg curl.* The curl blends from the leg's own direction at the start of the reaction, stored in
    the body frame, to the thigh raised ~70° (up to ~112° for the injured leg) with the shin hanging back. Lying face down, the curl is
    suppressed for wound curls until the torso rolls over.
  - *Writhe, breathing, twitches and the last breath.* These are offsets from a base position that
    follows the body (1.2 s time constant, stored relative to the pelvis in the body frame). A free
    limb's hand or foot goal moves by the offset, and IK bends the chain to follow.
- **Muscles.** Swing rates use the braking curve of Death Animations' `BrakingGain`: a quarter of the
  error per tick at most. They are capped at 120°/s and ramp from the previous rate by at most
  `accel * dt`. Acceleration is 1200°/s² (3000 u/s² for the chest and head) x Strength² x
  consciousness x speed gate. Below the level of gravity, the drive stops writing and physics has the
  body. That is how a hand slides off the wound as the body fades. Twitches and the last breath are
  involuntary and get 60 % of full strength whatever the consciousness. Nothing is steered while the
  speed gate is below 0.5 (a tumbling body).
- **Why nudges, not velocity** (measured with `nervous kick`):
  - *The corpse is awake.* A velocity written to all fifteen bodies of a landed corpse throws it
    13 units up. Re-setting the motion type or re-placing the body first makes no difference.
  - *Relative motion is erased.* Velocity that moves a limb relative to the torso is erased every
    tick. A hand commanded at 160 u/s moves 0-8 u/s, and a whole arm chain at 360°/s barely moves.
    The ragdoll's joints carry friction (`m_flFriction` in Valve's PHYS), and it cancels the light
    limb bodies' relative velocity.
  - *Nudges work.* Placement is not a velocity. With nudges the reaching wrist closes from 11-20
    units to 1.5-4 units from its goal in under half a second.
- **Relax.** Joint friction holds a limb wherever the last nudge left it, so a body that simply
  stops being driven would keep its hands frozen on the wound. For 1.6 s after its life (`RelaxWeight`),
  the arms are lowered to the torso's sides and toward the floor (`RelaxArmGoal`). The legs return to
  the direction they had when the reaction began. This drive is the same at any Strength, is
  contact weighted, and stops at the floor and the body. No reach, layer or twitch runs during it.
  Then physics has the whole body (`why=still`).
- **Once per demo tick.** A repeated demo tick cannot advance a nudge, even when our own transform
  write changed the pose. Paused playback does not move the body. Reaction clocks still run when a
  ragdoll sleeps, so an upcoming twitch can start without another effect keeping the body awake.
- **Inactive layers do not write.** No offset means no pose or velocity write for that body.
  Disabling twitches or breathing also stops an already scheduled twitch or final breath. With all
  five reaction toggles off the Nervous System leaves the entire ragdoll to physics.
- **Ground support.** The torso must be lying down with all three core bodies below 12 units/s
  vertical speed for 0.25 s before steering. A fall or jump apex cannot start a reaction.
- **Roll over.** A prone body continues its existing lean toward face up. A fully flat body uses a
  seeded side. The lower arm braces while the free arm sweeps across the torso, and all connected
  bodies receive a coherent angular/linear turn about the support side. The initial ground gate
  does not cancel this turn when it lifts the torso. The drive ends once face up, pauses for a fast
  ledge fall, and stops after four active seconds if obstructed. It does not run for headshots.
- **No floor slide.** There is no whole-body translation or slide setting. Displacement comes from
  native physics and contact with moving limbs. A settled, actively driven reaction adds horizontal
  velocity damping to absorb low-speed creep from those contacts. It fades out between 10 and 20
  units/s, does not change vertical velocity, and does not run during a roll or with all layers off.
- **The body frame.**
  - *Axes.* up = pelvis toward the shoulders; right = the left-to-right shoulder and hip line;
    front = up x right.
  - *Smoothing.* The frame is low-passed with a 0.4 s time constant, so offsets built on it do not
    jump when the body rolls.

## With Death Animations

With Advanced Ragdolls on, a dying player first plays its death clip. The Nervous System waits until
`DeathAnimations_Owns(pawn)` is false, meaning the clip has handed off or been released, up to 20 s.
It then reacts from wherever the clip left the body after it settles. Turning Advanced Ragdolls off
drops pending and active reactions, even if the Nervous System preference remains on. Deaths while
the master is off do not queue reactions. Airborne non-explosion deaths use native falling physics
instead of grounding a death clip at the victim's starting foot height.

## Settings

| Setting | Command | Default | Effect |
| --- | --- | --- | --- |
| On/off | `mirv_filmmaker nervous on\|off\|toggle` | off | the whole feature |
| Strength | `strength <10-200>` | 100 % | muscle strength (squared into the acceleration) |
| Duration | `duration <2-20>` | 8 s | roughly how long a body reacts |
| Reach for the wound | `reach on\|off\|toggle` | on | hands to the wound, and the leg/stomach curl |
| Breathing | `breath on\|off\|toggle` | on | chest heave and the last breath |
| Writhing | `writhe on\|off\|toggle` | on | writhe layers, head sway, blast/fire pedalling |
| Twitches | `twitch on\|off\|toggle` | on | spasm kicks |
| Roll over | `rollover on\|off\|toggle` | on | try to turn a prone body face up |
| Blend in | `blendin <0-2>` | 0.5 s | ease into the reaction |
| Blend out | `blendout <0-4>` | 1.5 s | ease out before stillness |

`kick <0|1|2>` (diagnostics) throws the next reacting body 150 u/s up once, and logs 24 physics ticks
of `nervous.kick`. The modes set the velocity only (0), re-set the motion type first (1), or re-place
the body on its own pose first (2).

`force <injury>|off` makes every death react as one injury (`head`, `neck`, `chest`, `stomach`,
`leftarm`, `rightarm`, `leftleg`, `rightleg`, `explosion`, `fire`, `melee`), for testing. `reset`
restores the defaults (off). `status` lists the settings, counts and the bodies reacting now. The
settings persist as `nervousOn`, `nervousStrength`, `nervousDuration`, `nervousReach`,
`nervousBreath`, `nervousWrithe`, `nervousTwitch`, `nervousRollOver`, `nervousBlendIn` and
`nervousBlendOut` with the other RAGDOLLS settings.

A seek, level change or turning the feature off lets go of every body without writing to it; the
engine may have just rebuilt them.

## Diagnosis

`mvm_debug start`, then read:

- `nervous death`: the event's hitgroup, headshot flag and the injury picked;
- `nervous wait`: waiting for a death clip; `react`: the profile, life and this death's `layers`; `limp`: a headshot;
- `nervous.frame` (every 32 physics ticks): time, consciousness, speed gate, muscle acceleration,
  whether the muscles hold, bodies written, how far each reaching wrist is from its goal, which way the chest faces
  (`front.z` < -0.3 is face down) and the breathing rate;
- `nervous.drift` (every 4 physics ticks): `slide` (pelvis planar distance since steering began),
  planar speed, `yaw` spin (deg/s), total placement this tick, and per body `layer/contact/height/speed`.
  The layer is reach, rest, giveup, curl, pedal, writhe, torso, spasm, relax or `-` (not written); height is
  above the lowest body. This is the probe for slide and spin: look for the layer whose bodies sit at low
  contact weight while `slide` climbs;
- `nervous.twitch`: each kick (limb 0/1 hands, 2/3 feet, 4 head);
- `nervous.reach` (every 8 physics ticks per reaching hand): shoulder, wrist, wound goal, IK goal, the
  error, and the upper/lower swing rates;
- `nervous.pose`: all fifteen body positions and the body frame when a reaction starts;
- `nervous giveup` (a hand that could not reach), `nervous lastgasp`, `nervous relax`, then `nervous end ... why=still|respawned|lost|seek|disabled`. `lost` is
  preceded by a `lost` line saying which check failed and whether the bodies could be found again.
  A practice or deathmatch demo that respawns the player takes the corpse bodies with it at the
  respawn (`respawned`, ~3.1 s in `replays/ragdolls.dem`); a match demo keeps the corpse until the
  round ends.
- `nervous skip`: no ragdoll bodies were found (the routes searched).

## References

- Overgrowth (Wolfire Games, open source): `Data/Scripts/aschar.as`, `SetActiveRagdollInjuredPose`
  and `SetRagdollType(_RGDL_INJURED)`. The injured ragdoll follows a writhe animation with a
  speed-gated strength that fades to its time of death, then goes limp.
- GTA V NaturalMotion Euphoria behaviours, as exposed by ScriptHookVDotNet's `EuphoriaHelpers.cs`:
  `Shot` (reachForWound, timeBeforeReachForWound, grabHoldTime, conscious-pain ramps),
  `InjuredOnGround`, `BodyWrithe`, `ShotInGuts`, `ConfigureShotInjuredArm/Leg` and `PedalLegs`.
  The parameter names and defaults informed the timings here; the implementation is our own.
