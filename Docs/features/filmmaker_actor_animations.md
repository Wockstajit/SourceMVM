# Actor Animations

Half-Life: Alyx's Combine soldier animations, played on CS2 player models. The player can preview
them in third person, play one on themselves while moving or recording, or give one to a placed
actor or a recorded bot. The clips are third-party game data: the pack is built from the
maintainer's own copy of Half-Life: Alyx, stays in the git-ignored `FX/actor-animations/`, and is
shipped from there, never committed.

Code: `AfxHookSource2/Filmmaker/Takes/ActorAnimations.{h,cpp}` (engine side) and
`ActorAnimationsMath.h` (pack loader, cue timeline, retarget, leg IK, root motion; tests in
`Automation/tests/actor-animations-math-tests.cpp`). The picker is
`Panorama/ConfigHudActorAnimPickerJs.h`, opened from the BOTS tab.

## Building the pack

`python FX/tools/extract-alyx-anims.py` reads `pak01_dir.vpk` of Half-Life: Alyx and of CS2 through
Source2Viewer-CLI and writes `FX/actor-animations/actor_animations.mvmanim` (`MVMACT02`). Per clip
and frame it stores:

- the rotation of 21 core bones, relative to **CS2's bind pose** (taken from the CS2 agent model, not
  from whatever pose the character happens to be in);
- the direction each arm and leg bone points, so limbs can be steered onto the Alyx pose even though
  the two skeletons rest in different arm and leg poses;
- the pelvis travel, and how far the lowest foot is above the standing floor.

Per clip it stores the weapon the clip was posed for (rifle, grenade, none) and the root speed.
Every clip is turned to start facing the way the model faces at rest. Pass
`--merge-deaths <path that does not exist>` to leave the death-animation library alone.

## How a pose reaches the screen

Everything runs in the late frame, after the engine's own pose (`RunLateFrame`, and again at the end
of the pass, because the engine's pose can land after the first write).

1. The bone pose is built from the bind pose, the clip rotation and the limb direction fix, keeping
   CS2's own bone lengths.
2. The lowest foot is put on the floor. With the map's collision data loaded, each ankle is probed
   against the ground (`MapCollisionRaycast`): a planted foot rides a slope or step, a foot the clip
   lifts keeps its lift, the body drops to the lower foot, and each leg bends with two-bone IK.
   Flat ground reads within an inch of zero and counts as flat; changes are eased in.
3. The parent-local pose at `CModelState+0x18` (and its copy at `+0x48`) is written.
4. The world-space pose at `+0x80` is written too, through the model-to-world transform found from
   the engine's own two buffers. On frames where the engine has already built its bone matrices, the
   draw and every bone-merged weapon read this buffer. Writing it in model space (the first
   attempt) made the character vanish on about a quarter of the frames, and writing nothing let the
   engine's pose flash in on about a quarter of them.

A pose with a NaN, an infinity or a collapsed rotation is skipped for that frame.

## Playing a clip

- **Play on me** plays the clip on the local player. It follows the player's real speed: a walk or
  run clip runs at speed over the clip's root speed and holds while standing, so the feet do not
  slide. Other clips play on the clock.
- **Weapon.** Auto equips what the clip was posed for: an AK-47 for the rifle clips, a grenade, or the
  knife when the hands are free. The picker also offers an M4A1-S, AWP, Nova, or **Keep mine**.
  A weapon is given once, not on every press.
- **Place new actor here** and **Assign to selected bot** give a placed actor the same weapon and walk
  it along the clip's root path (loops add a cycle of travel each time round). A recorded bot keeps
  its recorded movement.
- Recorded performances keep the animation cues beside the take. For a demo, a companion file
  (`demo_companion`) carries the cues, because the demo itself does not.
- **Third-person preview** sets `cam_idealyaw 0`. With a leftover offset the camera sits at the
  player's side, the character looks sideways and W walks toward the camera's left.

## Console

```
mirv_filmmaker takes thirdperson [on|off]
mirv_filmmaker takes anim play <clip> [speed] [loop] [start] [dur] [blendIn] [blendOut]
mirv_filmmaker takes anim stop | status | catalog
mirv_filmmaker takes anim weapon auto|keep|weapon_<name>
mirv_filmmaker takes anim assign <take#> <clip|off> ...
mirv_filmmaker takes actor place [clip] ...
mirv_filmmaker takes anim dbg <mask> [mod] [k]    (test switches: 1 skip world write, 2 skip copy,
                                                   8 keep the engine animation graph on, 16 no foot IK,
                                                   32 fake uneven ground)
```

## Checking it in game

A burst capture (`Automation/capture/capture-game-burst.ps1`) of the posed character, with the
frame-to-frame difference of the body region, shows flicker that a single screenshot cannot. The
engine's own pose sits about 5 apart from ours on that measure; steady playback stays under 3. The
`mvm_debug` log carries `actoranim` lines: the bind-pose check, the model-to-world transform, pose
bounds and ground probes.

## Known gaps

- Fingers are not driven: they keep the engine's pose.
- A few clips still twist the torso (for example the run turns); the pelvis yaw of those clips is
  not normalised past the start.
- Placed actors walk through walls: the path is not collision checked.
- Not checked against real stairs yet: the foot IK was checked on flat ground and with fake uneven
  ground.
