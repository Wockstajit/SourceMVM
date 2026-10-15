# Method Acting

Method Acting adds eye reactions to supported agents in offline CS2 demos. Its master switch is in **Config > Ragdolls > Method Acting**, separate from Advanced Ragdolls and body physics. The master defaults to off. The panel sets timing automatically. Console tuning is still available.

## Controls

| Panel control | Console command | Enabled default |
| --- | --- | --- |
| Method Acting | `mirv_filmmaker methodacting on\|off\|toggle` | off |
| Blink naturally | `mirv_filmmaker methodacting blink on\|off` | on |
| Combat squint | `mirv_filmmaker methodacting squint 0.5` (off: `0`) | on |
| Look around | `mirv_filmmaker methodacting gaze on\|off` | on |
| Relax eyes on death | `mirv_filmmaker methodacting deatheyes on\|off` | on |

The death option rolls the eyes up and slightly aside, then lowers the partly open lids. It does not force a fully closed pose. Live close-ups of the Phoenix agent showed pinched, hollow-looking lid geometry at full blink weight. The death pose peaks at 25 degrees upward, settles at 22 degrees with a 0.12 lid weight, and stops idle blinking and wandering on dead players.

Combat squint stays on for 0.9 seconds after the last shot, so pistol taps read as one exchange. It then relaxes over 0.45 seconds. An enemy within 260 units of the shooter's forward 900-unit firing path, or a bullet passing within 120 units of the face, holds a slightly lighter defensive squint for 1.1 seconds. A registered hit or a drop in the living player's health triggers a stronger injury squint for 0.35 seconds, followed by the usual release. `player_blind` holds a stronger squint for that victim's recorded flash exposure (up to six seconds), then relaxes. These reactions use the existing Combat squint setting, and setting it to zero disables them. Flash blindness suppression does not block the facial reaction. This also works in demos without bullet-impact events. Utility throws and same-team near misses do not trigger the near-bullet reaction.

Living players ease into left or right glances, hold for 0.65 seconds, return to center, and rest on individual deterministic rhythms. A glance cycle runs every 1.6-2.3 seconds. Ordinary quiet glances turn the head 8-14 degrees. 45% of side glances use a wider 28-45 degree turn. During firing or nearby combat, only 35% of glance cycles run, with head motion reduced to 20% (up to 9 degrees) and eye motion to 22%. Hit and flash reactions briefly steady the head.

Living players can briefly look toward the nearest visible living player within 220 units and a narrow forward cone. Static map collision must be ready and the line between faces clear. Otherwise the ordinary idle glance remains. Glances cannot see through walls. Eye availability and visibility depend on the agent. Masks and opaque goggles can conceal the effect.

Switching Method Acting off restores the saved head and eye poses and clears lid and look weights on both player pawns and client ragdolls. Restoration remains valid while the demo is paused.

## Demo behavior

All timing uses demo ticks. Pausing holds the current pose, and seeking drops integrated state. When seeking directly to an existing corpse, the settled eye pose is used instead of replaying its death. An observed alive-to-dead transition plays the short roll and relaxation. Engine animation preparation may require playback to advance before a newly selected facial setting becomes visible on a paused model. A console write count alone does not prove that the rendered face changed.

Turning off the master or death option releases the facial overrides. `methodacting reset` restores defaults and disables the master. Unsupported rigs are skipped. The feature does nothing outside a demo.

## Implementation

- `Movie/MethodActingMath.h` contains the deterministic blink, squint, gaze and death envelopes. `Automation/tests/method-acting-math-tests.cpp` checks timing, pause behavior, bounded motion, direction axes and the limit that prevents full death-eye closure.
- `Movie/MethodActingFlex.cpp` resolves the engine's controller count, name and bounds-checked weight setter. Controllers are identified by name, never by a universal index. The September 28 Phoenix build uses down/up/blink at indices 0/1/2. Cache entries use the resolved CModel resource, not mutable entity model-name storage. Native calls are guarded.
- `Movie/MethodActingGaze.cpp` requires named eyes and an eye target directly parented to `head_0` on the shipped head rig. It validates pose buffers, writes normalized eye rotations, and restores prior writes only when their entity handle, model, buffers and written values still match. Living head turns are applied only after animation preparation. The local/copy and derived world head subtree are kept in step. Inconsistent copies caused visible face jitter in Ancient close-ups. Body physics is untouched.
- `Movie/ModelPoseAccess.h` shares the guarded model/bone accessors with the death-animation preview.
- The main-thread pass runs before animation preparation and reasserts the final pose after the engine frame handler. It never runs Panorama from the render thread. Envelopes are driven by demo time, so the second pass cannot advance them twice.

Model layout assumptions depend on the game version: CModel names and parents at 0x168/0x180, and local, local-copy and world pose buffers at model-state 0x18/0x48/0x80. Capability checks and debug counters are necessary, but live close-ups must back them up after game updates.

## Tuning and diagnostics

`mirv_filmmaker methodacting status` reports settings, controller names, cache lookups, writes, shots, seeks and faults. `mvm_debug start` records `acting.flex`, `acting.flex.model`, `acting.rig`, `acting.gaze`, `acting.look` and `acting.death`. Stop the log after reproducing a problem. Gaze logs include selected target indices. Bone logs include the target pointer and resolved offsets.

Console-only tuning: `hold <ms>`, `attack <ms>`, `release <ms>`, `blinkrate <seconds>`, `doubleblink <0..1>`, `settle on|off`, and `index <n|auto>` (diagnostic controller override). The panel leaves these fine controls out on purpose.

## Persistence

Settings save and restore through `FilmmakerSettingsStore.cpp`, including `actingGaze` and `actingDeathEyes`. The Ragdolls keep-between-demos group and reset include Method Acting. The shared panel source is `Panorama/ConfigHudRagdollsTabJs.h`.

Validation: build `AfxHookSource2` and `MethodActingMathTests`, run `ctest --test-dir build/x64-source2-release -C Release -R MethodActingMathTests --output-on-failure`, then install both CS2 components and verify the running demo with `mvm_debug` and close-up captures.
