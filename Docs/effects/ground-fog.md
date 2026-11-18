# Ground fog

Ground fog is a low layer of fog that lies on the floor. Players walking or running through it push
it aside: it parts around their legs, leaves a trail behind them, curls into eddies at the edges of
the trail and slowly fills back in. It is part of the Environment's Fog card, next to the height fog,
and the two share the sky light. The ground fog has its own colour and its own Shadows slider; the
Fog page's Light shafts switch is the height fog's.

Implementation: [`EnvironmentGroundFog.cpp`](../../AfxHookSource2/Filmmaker/Environment/EnvironmentGroundFog.cpp)
(GPU side), [`EnvironmentGroundFogMath.h`](../../AfxHookSource2/Filmmaker/Environment/EnvironmentGroundFogMath.h)
(engine-free pieces), [`EnvironmentGroundFogShaders.h`](../../AfxHookSource2/Filmmaker/Environment/EnvironmentGroundFogShaders.h)
(the fluid) and `GroundFogMarch` in
[`EnvironmentClimateShaders.h`](../../AfxHookSource2/Filmmaker/Environment/EnvironmentClimateShaders.h)
(drawing it).

## Controls

CONFIG > ENVIRONMENT > FOG, under **Ground fog**. Every control is also a climate key
(`mirv_filmmaker environment climate <key> <value>`) and a scene key (`env.<key>`) on the timeline.

| Control | Key | Range | Default | What it does |
| --- | --- | --- | --- | --- |
| Density | `groundFog` | 0 to 4 | 0 (off) | How thick the layer is. Around 1 it hides the floor; 0.2 is a thin veil. 0 switches it off. |
| Coverage | `groundFogCoverage` | 0.05 to 1 | 0.85 | How much of the floor it covers. 1 is an unbroken sheet; lower breaks it into patches with clear ground between them. Applies at once, on a paused frame too. |
| Depth | `groundFogHeight` | 8 to 400 u | 56 | How high it stands over the floor. |
| Softness | `groundFogSoftness` | 0 to 1 | 0.35 | How gradually it fades upward. 0 is a defined sheet with a top surface; 1 thins all the way up from the floor, like mist. |
| Wisps | `groundFogWisps` | 0 to 1 | 0 | Off by default: the streaks read as light rays ruled across the fog. Tears it into thin streaks you can see through. Moving air bends them, so wakes drag them along. |
| Wisp size | `groundFogWispSize` | 8 to 240 u | 36 | How wide those streaks are. |
| Streak | `groundFogStreak` | 0 to 1 | 0.2 | How far the streaks stretch along the drift direction. |
| Billows | `groundFogDetail` | 0 to 1 | 0.6 | Rolling lumps in the top of the layer. |
| Billow size | `groundFogBillowSize` | 60 to 1000 u | 220 | How wide those lumps are. |
| Push | `groundFogFlow` | 0 to 3 | 1 | How hard moving players shove it. 0 leaves it undisturbed. |
| Push size | `groundFogPushSize` | 0.2 to 2 | 0.55 | How wide a player's mark is. It scales the clearing, the shove and the heaped rim together. |
| Refill time | `groundFogRecovery` | 0.5 to 60 s | 8 | How long a cleared trail takes to fill back in. |
| Swirl | `groundFogSwirl` | 0 to 2 | 0.8 | How much the wakes curl into eddies. |
| Drift speed | `groundFogWind` | 0 to 200 u/s | 12 | How fast the fog drifts and carries the trails. |
| Drift direction | `groundFogWindAngle` | 0 to 360° | 30 | Compass direction it drifts toward. |
| Churn | `groundFogChurn` | 0 to 5 | 1 | How fast the billows and wisps roll and change. The fog players pushed is not affected. |
| Ground fog color | `groundFogColorR/G/B` | 0 to 1 | warm white | Its own colour, separate from the height fog's. Not keyable. |
| Sun on fog | `groundFogSun` | 0 to 3 | 1 | How bright the sun makes it. |
| Shade brightness | `groundFogShade` | 0 to 3 | 1 | How bright it is where the sun does not reach. |
| Shade color | `groundFogShadeR/G/B` | 0 to 1 | white | Tints the sky light on it, so fog in shade can go steel blue while fog in the sun stays white. Not keyable. |
| Shadows | `groundFogShadows` | 0 to 1 | 1 | How dark buildings and players make it where they block the sun. 0 lights all of it. Its own: Light shafts do not change it. |
| Show sim view | `environment groundfog-view` | on or off | off | The live fluid in a corner of the screen (see below). A test view, not saved. |

**Start from** sets the sliders in one go, as a place to start. *Street mist* is thin torn fog with a milky
height-fog haze over the map, white in the sun and blue in shade. *Thick sheet* is one solid layer on the
floor. Neither is a mode: every value it sets is a slider, and players push the fog the same way in both.

The Fog card's switch turns on the height fog, the ground fog and the light shafts together. Either
fog alone keeps the card on. **Light shafts** (`lightShafts`) is a switch on the height fog's page and
does not touch the ground fog.

## How it works

**The floor map.** When a map's collision data is ready, a worker thread rasterises every upward-facing
face into a top-down grid (16-unit texels, at most 1024 per side). Each column keeps up to four floor
heights, so a point under a balcony finds the ground and a point on the balcony finds the balcony.

Fog lies only where people walk, and the map's nav mesh (`maps/<map>.nav` in its package) says where that
is. A floor counts if the nav mesh covers it or passes within 48 units at about the same height, because
the nav mesh stops a player's width short of walls. A place the nav mesh covers has fog even with no
collision floor there. So roofs carry none however they join up with the street, and raised streets
keep theirs. A separate piece smaller than 200 by 200 units, such as a crate someone can jump on, stands
out of the fog instead of carrying it. On a map with no nav mesh, such as many Workshop maps, the choice
falls back on geometry: floors joined by steps of at most 36 units form a piece, and a piece under 12
percent of the largest is dropped. That is only a guess at what people can reach.

Fog thins toward a sheer edge within 96 units, one with nothing to step onto and a floor at least 60 units
below, so it ends softly at a ledge. Stairs and ramps are not edges. The floor under a point never averages
across a wall or ledge, so fog does not float partway up a wall. A small same-floor blur hides the texel
grid. Without map data the floor is the fog's Base height.

**The fluid.** One grid covers the whole map, with square cells of at least 8 units and at most 1024
cells a side. It runs Stable Fluids on the GPU: advection, a pressure solve, vorticity confinement
for the eddies and a faint divergence-free stir so still air does not look frozen. Three things were
added, chosen over the plain solver in a side-by-side on 2026-11-16. Walls: the
fog cannot pass a step of 56 units within two cells, read from the weather cover volume a few rows a
frame, so it piles against walls, flows round their ends and wakes come back off them (until the
cover is baked the fog flows through). Turbulence only where the air is moving, in step with how
fast, so a wake breaks into wisps while still fog lies calm. And a springy pressure: six relaxations
a step on a pressure that carries over, where twenty cancelled every shove at once, so a push sends a
soft swell outward. Because the grid is fixed to the map, turning the camera or
cutting to another player never loses fog, and a render does not depend on its camera path. Each living player is a body. It drags the
air it occupies along with it and pushes the fog in front and beside it outward. Explosions blow the fog away in a growing ring, burning molotov fires clear it around them and CS2
smokes displace it, so flames and clouds stand in clear air instead of behind fog drawn over them.
Moving, a body clears the fog from a footprint a little wider than the body and heaps it into a thicker rim just outside,
like a bow wave. Standing still only thins the fog around the legs. Fog is a quantity the flow carries.
At rest it has a fine, uneven relief, and below full Coverage it also breaks into drifting patches, so
moving air visibly curls and streaks it. It refills toward that resting pattern over the Refill time.
Fog amount advects with a Catmull-Rom lookup, because a bilinear one blurred the streaks away within a
second. Slow eddies at two sizes keep it moving with nobody near. Changing Coverage moves the field to
the new pattern at once and keeps what bodies did to it, even on a paused frame.

**Time.** The fluid steps on demo time, not wall time. A paused demo freezes it, slow motion slows
it, and an export at a fixed frame rate steps it the same way each run. A seek of more than half a
second either way starts it again from rest, so wakes build up again over a few seconds. The demo clock
also steps back by a few milliseconds during ordinary playback; such a step simulates nothing and is
not a restart. The HDR and display passes of one frame share a single step. The debug log records
every restart with its reason (`environment.groundfog`); one during plain playback is a bug.

**Drawing.** Inside the Environment fog march, the ground layer is marched first, with fine steps
inside the layer and long steps above it. Inside the layer each step is a fixed share longer than the
last, so the march reaches about 5500 units in preview and 9000 in export. (A slower first version
stopped near 400 units, so the fog vanished in the distance whenever the camera stood in the layer.)
Steps also shorten to the fog's own thickness, so dense fog does not show terraces. In the layer, or
about to enter it, no step crosses more than an eighth of its depth up or down. Before, a ray going down
through a Depth 8 layer crossed all of it in one step, so whether a pixel saw any fog came down to where
its one sample landed, and the fog vanished at some view angles. The fluid decides
both how dense the fog is and how tall it stands. The top follows a soft quarter-resolution copy of
the field and stops at 1.25 times its share, so it stays a gentle dune rather than spiking where fog
is pushed. The fine marbling shows in the density.
**Wisps** tear the density into streaks. Their noise is stretched along the drift and flattened into thin sheets of
height, because a ray near the horizon stays inside one sheet for a long way, and that is what reads as a long
streak. Noise that is uniform in height averages away along such a ray and leaves smooth fog. The billows bend the
sheets so they curve, and the moving air bends them too, so a wake drags the streaks along. **Softness** moves the
start of the fade from partway up the layer down to the floor and lets the fog reach higher, and it also eases the
fog's shading of itself, because thin mist scatters light from all sides. Its density falls off smoothly toward a top that rises and
falls with the billows and sinks where a trail thinned the fog. Each sample is lit by:

- the sky: the map's own authored sky colour, or the Environment sky when the Sky card is on. It is
  kept near neutral and is dimmer near the floor, so fog in shade is a soft grey. Only as much of it
  reaches the fog as reaches that spot: open sky straight above (the top-down sky map, which renders
  whenever ground fog is on) or open air reaching in under cover (the weather cover volume), with a
  little left for light bounced in off walls. Fog in a covered room or inside a stack of crates is dim.
- the sun: the climate sun, which starts as the map's own `light_environment`. Dense low fog scatters
  light many times, so it is lit from every side as well as having a forward glow toward the sun. It
  is shadowed by the map, by the Shadows slider (averaged along each march step, so an edge does not
  shift with each pixel's jitter, and over a few units across), by the players, and by the fog
  itself through a short march toward the sun: sun-facing slopes are bright and the far side of a heap
  is darker.
- the first four added lights, shadowed by the same Shadows slider.

A player's shadow on the fog has the player's shape: the capsules between its bones (trunk, head,
arms, legs), read from the pose it is drawn with each frame, so the shadow walks and crouches with it. A
player whose bones cannot be read falls back to a slim upright capsule. One upright capsule for every
player drew a pill laid along the sun. The sun's shadow map holds only the players CS2 drew to the
screen this frame and never the first-person player, so the bones are needed.

The height fog uses the same sky light and a closed-form sun self-shadow, and its far continuation is lit the
same way as the near part, so the two meet without a brightness step.

## Effects in the fog

Particles write no depth, so fog measured to the surface behind one is too much when the effect is
close. Two things decide how much fog goes over an effect, for both fogs:

- **What it did to the frame.** The hook copies CS2's HDR scene target just before its transparent
  passes and compares it with the finished frame. That gives each pixel the share of light an effect
  added (flames, tracers, sparks, muzzle flashes) and the share of the background an effect hid
  (explosion smoke, glass). These are shares, not cut-offs, so an effect's edges stay soft. A black
  cloud over a dark background changes almost nothing, so on dark pixels what the effect hides close by
  counts as well. CS2's own absorbance buffer cannot help here: it stays empty for an explosion cloud.
- **Where it is.** Explosions, fires, burning bottles, muzzles and smokes are known positions. Only the
  fog between the camera and that position goes over the effect: a fire far down a fog bank sinks into
  it, and a close explosion cloud has almost none over it. A shot counts for 0.35 seconds, longer than the
  light it gives, because its flash and smoke last longer. A first-person flash is placed just ahead of the
  lens. Placed at the muzzle, any radius would take in every effect on screen, including far fires.
  A cloud hides the fog behind it as far as it
  hides anything else, and what shows through a thin cloud keeps its full fog, so a cloud never opens a
  clear window onto the far scene. Explosion smoke counts for 6 seconds, then fades out over 3.
  Transparents with no known position take the fog of the surface behind them, except bright light such
  as a tracer's far end, which counts as most of the way to the surface.

The player's own gun and arms are kept almost clear of fog however thick it is around the camera.

This needs CS2's multisampled scene target: with MSAA off, every effect takes the fog of the surface
behind it. `environment groundfog-dump` saves the mask next to the field (red is added light, green is
what was hidden).

## Seeing the sim

**Show sim view** on the Fog card, or `mirv_filmmaker environment groundfog-view [on|off|map|<units across>]`
(no argument toggles), draws
the live fluid in the right of the frame, from above with north up. It shows 1600 units around the
camera; a number sets how wide, and `map` fits the whole map. Mid grey is fog at rest, dark is cleared
fog, bright is fog heaped up, and blue tints moving air. Cells with no floor are dimmed so the map's
outline reads. Orange dots are players, red dots are blasts, fires and smokes, and the green dot and tick are the camera and where it looks. It
never appears in an EXR. `mirv_filmmaker environment groundfog-dump` saves the same field as a BMP in the debug log
folder and logs its numbers (lowest and highest fog, cells cleared and heaped, restarts).

## Light shafts

Light shafts are the sun's shadow drawn through the height fog. The ground fog's shadows are its own
Shadows slider. Either renders the world sun shadow maps: the height fog with its switch on, the ground
fog with Shadows above 0. Neither needs the Sky or Lighting card. The shadows fall where the map's
sunlight does, because the climate sun starts from the map's `light_environment`.

## Transparent surfaces in fog

CS2 draws glass, flames, smoke and some foliage cards with moment-based OIT. Its absorbance buffer
holds all of them, not only smoke. The composite used to treat any covered pixel as smoke whenever a
smoke grenade existed anywhere on the map, and cut that pixel's fog short at the nearest smoke. Glass,
fire and leaves then stood out of the fog, crisp and bright. Now only coverage on a ray that passes
through a placed smoke counts as smoke. Each smoke's sphere is widened by 15 to 60 percent with a soft
fade, because a CS2 cloud billows past the sphere placed for it. Other transparents keep the fog of
the surface behind them. The same rule covers rain, snow and placed props hidden behind smoke, and
relit smoke. Reference and tests: `SmokeRayShare` in
[`EnvironmentClimateMath.h`](../../AfxHookSource2/Filmmaker/Environment/EnvironmentClimateMath.h).

## Limits

- The fluid is 2D. Players on an upper floor also push the fog on the floor below at the same spot.
- Walls do not stop the flow. Fog flows through a wall in the fluid, but nobody can see that.
- Living players, explosions, fires and smokes move the fog. Ragdolls and other particles do not.
- A particle with no known position cannot be placed: it takes the fog of the surface behind it, or most
  of that if it is bright. The far end of a tracer is the usual case.
- Fires and explosions do not light the fog unless Dynamic lighting is on, so one hidden in a thick bank
  gives no glow.
- Wakes are history: after a seek they start from rest and build up again during playback.
- The drift is added up as the demo plays (`GroundFogDriftTick`), so a new Drift speed only changes
  where the pattern goes next. A seek starts it again from time x speed, which is what a steady wind
  adds up to, so a render that seeks to its start begins at the same offset each run. A new Drift
  direction still re-lays the wisps at once: they are stretched along the drift, so their pattern is
  looked up in a frame turned to that direction.
- The march runs at the Environment's volume resolution (a quarter of each side in preview, full in
  export). In the live view each frame's march is jittered anew and blended into the last frames'
  (`VolumeTemporalPS`), so its grain averages out instead of crawling as the camera moves. A render
  marches each frame whole, with a still jitter.

## Tests

- `EnvironmentGroundFogMathTests` ([source](../../Automation/tests/environment-ground-fog-math-tests.cpp)):
  floor map (balconies, ledges, crates, ceilings, ramps), reading a nav file and using it to decide where fog lies, the whole-map fluid grid, the demo clock (including its small backward steps), body
  velocities, which coverage is smoke, the fog's sky light, how fog thins at ledges but not on stairs,
  and how much of a ray lies in front of an effect.
- `EnvironmentRenderTests`: transparents keep the surface's fog; a close flame or cloud has no fog over
  it, a far one sinks in, and one nobody placed keeps most or all of it; ground fog in the layer.
- `EnvironmentMathTests`: light shafts and the ground fog's Shadows each request sun shadow maps on
  their own, and Light shafts off leaves the ground fog's shadows on; ground fog is held
  across a restart like the height fog; scenes saved before these controls still load.
- `environment-ui-tests.mjs`: the Fog card's ground fog controls, reset and switch.

## Changes of 2026-11-16

- **Light that is not a flame does not cut the fog.** The mask of what transparent effects did to the
  frame counted any added light alike, so a pale puff of muzzle smoke or a spent case cleared the fog
  over itself and the ground showed through unfogged. Light now counts by how far it swamps what was
  behind it (`FrontMask`), and squared in the composite: flames and tracers still stand out of the
  fog, puffs and cases do not.
- **No squares along a near outline.** The fog is marched at a lower resolution and fitted back to
  the frame by depth. Where the coarse texels that match a pixel's depth weigh under a fifth (the
  outline of a barrel a pace away), their value is one far corner's, and the fog was drawn in the
  coarse squares. Those pixels are marched exactly, as pixels with no match already were.
- **Shade on the fog is soft and the same fog.** The sun's shadow is read at four points turned a
  different way at every pixel, out to the layer's own depth, and shaded fog keeps a seventh of the
  sun (it is lit by the sunlit fog beside it). Before, a roof's shade lay on the fog as a flat darker
  shape with stepped edges.
- **The height fog lies in front of it.** The ground layer is marched first; each piece of it is now
  seen through the height fog between it and the eye and takes that fog's colour for the share it
  hides, so the two read as one air and not white billows cut out of a blue haze.


## Changes of 2026-11-17

- **Thin layers no longer vanish at some angles.** No march step crosses more than an eighth of the
  layer's depth up or down, so a Depth 8 layer is sampled several times on the way through.
- **No crawling grain while the camera moves.** The live view jitters its march anew every frame and
  blends each frame into the last ones, found through last frame's view. Last frame counts only where it
  saw the same surface and only within the range of this frame's march, so nothing leaves a trail.
- **Shadows of its own.** The Shadows slider replaces Light shafts for the ground fog. The map's shadow is
  spread over a quarter of the layer's depth (4 to 16 units) instead of the whole depth, which had washed
  buildings' shadows out of the fog, and shaded fog keeps a fifth of the sun.
- **No checkerboard through cracks.** A pixel the coarse fog has no match for (a crack between crates,
  a near outline) is marched on its own. It used the coarse texel's sample offset, the same for a 4 x 4
  block and different for the next, so a thin layer seen through a crack came out as a row of fogged
  and clear squares. Those pixels now sample each step at its middle.
- **Fog under cover is not lit by the whole sky.** Fog inside covered rooms and crate stacks glowed
  white in the dark; its sky light now follows the sky map and the cover volume.
- **Players' shadows have their shape.** See the drawing section: the bones replace one capsule a player.
