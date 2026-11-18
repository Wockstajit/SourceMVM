# Environment editor

## Current lighting contract

[Whole-map relighting](../effects/whole-map-relighting.md) owns the current native material,
smoke, VFX, stained-glass and shadow behavior. Authored map lights and event-driven fire and muzzle
lights are part of that runtime list. The placed Scene Objects light limits below are a separate
path, and they do not mean that native smoke cannot be relit.

Four moving shadow slots are shared across transient emitters. When several infernos are active,
each receives a representative patch before the same fire gets extra patches. Flashes take a slot
immediately. With shadows enabled, overflow sources are omitted, which keeps wall occlusion.
Transparent particles are not automatically opaque casters. Check the linked verification notes
before claiming casting shadows or that every VFX pack has been visually validated.

Open **CONFIG → ENVIRONMENT** during an offline demo or local practice session. The shared Panorama
implementation and browser mock expose six task-oriented cards:

| Card | Controls and pages |
| --- | --- |
| Looks | Starting presets; Color: exposure, tint, saturation, contrast; Lens: bloom, vignette, grain, chromatic shift |
| Sky | Time of day, sun direction, sun height, sun power, haze, stars, sun color; Moon: brightness, phase, size, color; Clouds: enable, amount, density, height, cloud speed, rain darkening |
| Lighting | Map light power, ambient light, player fill, bounce intensity, 1 to 3 bounce steps per frame, shadows and source sizes; Exposure: the Environment's own auto exposure and its bias |
| Fog | Two pages. **Fog**: height fog density, start distance, height, variation and color, and Light shafts. **Ground fog**: starting looks, density, coverage, depth; shape; light (color, sun, shade, shadows); motion (push, refill time, swirl, drift speed and direction) and the sim view |
| Weather | Type, intensity, fall speed, wind speed/direction; Ground: wetness, puddles, snow, snow depth, CS2 water beads; Lightning: enable, intensity, interval |
| Scene Objects | **+ Add** in the header (point, spot and area lights, a camera object); Outliner: every placed thing in groups, with show/hide, in-renders and lock; Library: CS2 models and your OBJ meshes; the selected object's light or mesh editor and its RIG under the tree; Map lights: Edit lights and Turn all back on for the map's own lamps |
| Round heat map | Its own card: where the round's players walked ([`round-heatmap.md`](round-heatmap.md)) |

Scene presets (save/load) are the menu in the **Looks** header. Looks, Sky, Lighting, Fog and
Weather are each one page with group headings; only Scene Objects keeps sub-tabs.

Bounce steps/frame is an experimental convergence control. 1 keeps the current GPU cost. 2 or 3 run
more indirect-light propagation work and may reduce FPS. The setting changes the number of
iterations, not the total physical bounce depth. Particle field dimensions, camera following,
shelter collision and particle detail use their defaults. Low-level renderer commands remain
available. The engine's original
visual controls stay on **EFFECTS**. Run offline sessions with `-insecure`. Added fog does not
replace smoke.

## Creative controls and unified fog

Sky and lighting are independent: relighting works with the map sky. Changing a surface-lighting
value enables relighting. Clouds can render with either sky. Card switches bypass their effect
and remember its values. Reset clears that memory and restores defaults. Looks has no extra
quality or master switch. Opening a card never enables an effect by itself. Switching off the last
active card also turns Environment off. After 300 inactive rendered frames, it releases its GPU
targets, shadow maps, AO and bounce resources. Other active effects or placed objects keep the
resources they need. Switching back on rebuilds the renderer from prepared shader bytecode.

Fog has two layers and one color. [Ground fog](../effects/ground-fog.md) is a low layer that lies on
the floor, follows slopes and upper floors, and parts around players as they move through it. The
height fog has one density (0 to 6). Start distance leaves the near region clear. Height controls
vertical falloff. Variation is three octaves of drifting, contrast-stretched noise at map scale. It
opens clear pockets and thick banks, and it raises and lowers the fog top. It does not add a fine
ripple that a long ray averages away. The lit march and far continuation integrate disjoint depth
intervals, so the far blend does not apply a second fog layer over the near volume. The shared sun
and first four added lights illuminate the near fog. Depth and per-sample MSAA coverage stop it at
visible geometry. The 3D skybox (distant town, hills) takes the medium out to a fixed 9000 units.
Its reserved depth band has no real distance, so it sinks into fog with the sky behind it. The march
jitter is interleaved gradient noise. In the live view it moves every frame and each frame is blended
into the last ones, found through last frame's view (`VolumeTemporalPS`), so the grain averages out
instead of crawling as the camera moves. A 5 × 5 depth-aware blur at volume resolution then smooths
what is left without crossing silhouettes. A render marches every frame whole with a still jitter.

Fog does not depend on Lighting. It runs none of the dynamic-lighting, bounce or native relighting
passes. Both fogs take their sky light from the map's own authored sky colour, or from the
Environment sky when the Sky card is on, and the fog between a point and the sun dims the sunlight
it gets. The **Light shafts** switch on the Fog page draws the sun's shadow through the height fog, and the
ground fog's own **Shadows** slider does the same for it. The climate sun starts as the map's
`light_environment`, so both follow the map's own sunlight without the Sky or Lighting card. With
both off, fog renders no sun shadow maps. The sun
cascades render only for a pass that samples them: dynamic lighting (including the native relight
path it hands off to), world bounce, clouds, snow and wet coatings, placed props, height fog with light
shafts, ground fog with shadows, and the cascade debug view.

Glass, flames and leaf cards in CS2's transparency buffer keep the fog of the surface behind them.
Only coverage on a ray that passes through a placed smoke counts as smoke and cuts the fog short
there (details in [ground-fog.md](../effects/ground-fog.md#transparent-surfaces-in-fog)).

Preview rendering is fixed to Balanced (32 march steps, half-resolution volume). Export uses High
(64 steps, full-resolution volume). There are no serialized quality fields or quality commands.
Depth-aware upsampling evaluates unmatched samples directly. Absolute demo time, rather than temporal
history, keeps seeks repeatable.

## Selection and Edit Position

The Outliner lists every placed object, and the Lights page lists the lights. Selection uses stable
object IDs and does not switch page. It draws a world-space selection outline without moving the
camera. Creating or duplicating an object selects it immediately. New props and lights start 160
units ahead of the rendered camera so their handles are reachable.

Edit position shows world X/Y/Z handles in red, green and blue. Their projected size depends on
camera distance. Dragging constrains translation to one world axis. Mouse release commits one undo
entry, and a no-op drag adds none. Escape during a drag restores its starting position. A second
Escape exits position editing. Done moving or closing Config cancels any uncommitted drag.
Selecting another object does not refocus or reposition the camera. Selection stays outlined
outside Edit position. Objects behind the camera have no draggable handle.

The overlay uses Config's existing cursor bridge. It rejects presses outside the world preview or
underneath editor controls. Config JSON exposes `envSelected`, `envEditing`,
`envDragging`, `envEditRevision` and projected `envGizmo` geometry. Console operations:

```text
mirv_filmmaker environment select 1
mirv_filmmaker environment edit start 1
mirv_filmmaker environment edit begin x 0.5 0.5 1.77778
mirv_filmmaker environment edit drag 0.55 0.5
mirv_filmmaker environment edit commit
mirv_filmmaker environment edit cancel
mirv_filmmaker environment edit stop
```

Begin and drag coordinates are normalized to the rendered preview. Begin validates the axis hit against
the native camera projection. Projection, hit testing and axis-plane translation live in the
header-only `EnvironmentGizmoMath.h`, with standalone native tests.

## Surface experiments and relighting without a rebake

**WEATHER → Ground** provides snow coverage, wetness and
puddles. Rain and wetness are one effect. Choosing Rain on untouched ground also sets wetness 0.6
and puddles 0.25. Snow sets snow 0.6. The card switch turns the particles and the ground off and back
on together. While it rains, only exposed ground is wet, and wetness 0 keeps it dry. Ground response
is global by default. Puddle coverage is the environment shader's standing-water film. Native CS2
water meshes are not built for coverage, because regenerating wet-ground geometry stalled every
slider edit. Scene Objects reports scene messages, while Weather summarizes the
enabled ground effects.

Snow uses a world-space coverage mask and filtered upward-facing normals. Full coverage leaves no
noise-mask holes on eligible faces, and anything that is not steep is covered, so a tiled roof is
white across its tiles. Snow never lies indoors, whether or not any is falling: on a map whose
geometry has been read it follows the weather cover described under
[Weather cover](#weather-cover-what-is-indoors). Open ground, alleys and roofs are covered. Under an
arch, an eave or just inside a doorway the cover thins out and ends on an uneven edge. A wide arch
lets it a long way in, a doorway a little, and a wall not at all. The wind setting does not move the
cover. Broad drift patches collect more snow than others, with lumpy relief shading where it lies
thick.

Players get no snow and no world wetness. The coating masks are keyed to map positions, which slide
across a moving body. A pixel that belongs to a player, a bot, a held weapon or a world weapon is
therefore left as the engine drew it. The shader tells these apart from the map with an exact
per-frame mask. The native skinned player and world-weapon triangles that the shadow casters already
capture are drawn from the camera into a screen-sized depth map. A point counts as a person when that
map holds a surface at its own position, corpses and held guns included. Each living player's body as
drawn this frame counts as well (`BonesClaim`): the capsules between its bones, read from the pose it
is drawn with, plus four units for clothes and gear (more far away), except level ground at the
height of its ankles. With the mask alone, bots came out snowed and wet over their shoulders and arms
(2026-11-17). Players keep only CS2's own shading: its frost in snow, its wet in rain. Until the first capture, a
body-sized capsule around each living pawn serves as the fallback. The capsule ignores the floor and
any wall the player leans on. Around the pawn the camera is in, everything inside its hull counts as
the player (the first-person legs are not posed as the body in the mask), except level ground within
a step of the feet: the drawn floor is often a unit or two above the collision floor (two on Mirage's
Palace balcony), and it was taken for the body, a bare 40-unit square round the feet. Level surface
3 to 8 units over the feet is the boots' toe caps, not floor: taken for floor, they came out snowed
(2026-11-18); a step up is higher than a boot. First-person hands and weapon are skipped by the snow only.

The capture builds a stream-output geometry shader per character or weapon vertex-shader variant from
its reflected output signature, including signatures that pack two semantics into one register. Its
cache holds 4096 entries. A variant without one is missing from the mask and shows snow on bare forearms.

The coating filter blends shallow neighboring facets while rejecting depth discontinuities and hard
corners. Snow hides the ground, so it does not take its shade from the ground's own pixel: cobble
joints and tile seams showed through and the cover read as frost. It takes the scene's light from
twelve taps around the pixel (about 2.6 % of the frame's height out), leaving out taps on another
surface. The frame's own sun shadow is taken out of every tap (each tap knows whether the sun
reaches it) and put back once at the pixel with the filtered sun lookup, so a shadow's edge on snow
is as sharp as on the lit world and every tap still counts. Averaged as it stood, the ring smeared
each shadow into a wide grainy blur; with the taps across an edge left out, shade under a palm,
where most taps are across some edge, came out speckled. The ring turns smoothly from place to
place: turned by a hash of each half-unit cell, every cell measured the shadow from different taps
and a shadow's edge came out as a staircase of half-unit blocks. Shade on snow is at least a third
of the sunlit snow (the sky and the snow round it light it); measured from the frame alone, a
corner's own occlusion made the shade under a plank nearly black. That keeps sun and shade, a lamp's pool and a fifth of the light's colour, and loses the
texture. The cover lifts a surface at most four times its own brightness, so ground under snow stays
dark under a night sky. An unbounded lift turned night ground grey (issue 135). Enabled relighting,
added lights and world bounce also affect it. Fine grain and sparkle fade below pixel size.

Wet ground is darker and richer, not paler: a soaked surface loses a quarter to over half of its
brightness, more where it is porous, and gains a little saturation. Its mirror is water's own
Fresnel term and nothing more, next to none looking straight down and strong toward the horizon, and
a rough surface mirrors less. The mirror shows the scene through a short screen-space trace. Where
the trace finds nothing it shows the sky if the sky is open above, and the surface's own colour
otherwise. An earlier constant sky-coloured film made wet ground look frosted and puddles white.
The trace starts at the same point along the ray in every pixel and bisects its hit: started at a
random offset per pixel, neighbouring pixels found the wall or missed it at random, and wet walls and
ground were covered in white grain where the mirrored sky came through. A hit is the ray crossing a
surface between two steps, in front of it at one and behind it at the next. Tested on one step alone
("no more than a step behind"), a ray meeting a wall at a slant usually landed further behind than
that and was refused at every step after, so a mirrored wall came out as stripes of hit and miss
(Dust 2 Long A, 2026-11-18). A ray that passed behind something nearer, such as a pole, was already
far behind it at the step before, so it is still refused.

**What the trace cannot see.** The trace finds only what is on screen, so a puddle seen from above
lost the buildings it should hold. Where it finds nothing, the film shows CS2's own reflection
probes (the map's baked 360° captures) under the map's lighting, and the live capture through the
bounce volume while Environment lighting owns the map (Bounce or Lite). A probe is read where the
reflected ray leaves its box, as the game does, and the most local box that holds the point wins,
feathered into the next box out near its edge. CS2's constants list only the probes its own view
uses (3 at Long A on Dust 2, 18 at CT spawn), so that list is read back twice a second and the
sixteen nearest the camera go to the shader. The readback is mapped on the game's immediate context
when a lit world draw is seen; the composite records into a deferred context, which cannot read
anything back. Both sources hold raw scene light while the film is painted on the finished
frame, so right after CS2's post-process the hook compares its input with its output at a grid of
central pixels, per channel in half-stop bins, and probe light goes through that curve: exposure,
the filmic curve and the colour grade are included, vignette and bloom are not. CS2's post-process
can be an instanced draw, and with MSAA on it writes a 4x multisampled target, which is resolved
before the window is read (2026-11-18: both had kept the curve from ever being measured). The
capture and the measurement run only while a wet film is drawn; with Environment off neither runs.
`mvm_debug` writes `environment.reflection.misses` every 300 frames: which check stopped the capture
or the measurement, and the formats it saw. With the
Environment's sky on, the probe gives the buildings and our sky the open sky. `environment status`
names the source. Code: `EnvironmentReflectionProbes.{h,cpp}` and `EnvironmentReflectionProbeMath.h`
(tests: `reflection-probe-math-tests`).

Puddles require upward faces inside a chosen world-height band. They use procedural ripples and
Fresnel reflection of the analytic sky. Their reflection normal follows a shared horizontal water
surface, which avoids separate reflections on adjacent stone triangles. Ripple detail is filtered
with viewing distance. Puddles reflect the analytic sky by default. **Rough scene reflections** can
also sample the world-space bounce volume described below.**Center surface test here** sets a 600-unit local radius and guesses ground height 64 units below the
camera. Adjust **Puddle height** to the actual floor. Radius zero applies the experiment globally.

These are screen-space appearance tests, not material or geometry edits. They can affect
actors, props and transparent surfaces. They do not add snow thickness, physics, roof-aware
accumulation, splashes, water simulation or new texture detail. The depth-derived normal
cannot recover material normal maps. Strong coverage can obscure existing surface detail.

**Auto exposure** (Lighting > Exposure, climate `autoExposure`, on by default, and `exposureBias`
in stops) is the Environment's own. While Environment lighting is on it meters the relit frame and
drives the tonemapper, so a map lit by the Environment's sun and sky is not left at the exposure
its baked lighting was authored for. It uses the same meter as the in-game Auto exposure on
Effects > Lighting ([night mode](../effects/night-mode.md)) and nothing else of it: neither switch
reads or changes the other, and when Environment lighting goes off the in-game setting is in
charge as it was left. A pinned exposure (a time of day, the Exposure slider) still wins.

It adapts half way, not all the way (`AutoExposureMath::PartialMeter`, after the automatic key of
Krawczyk, Myszkowski and Seidel 2005). Full adaptation brings every scene to the same mid-grey: a
dim room is lifted to daylight brightness, and the sunlit street seen through its doorway is
lifted with it and clips to white. Measured on Mirage, outdoors metered log2 -1.5 and settled at
exposure 0.52, while the Palace interior metered -4.1 and settled at 3.2. Half way from the
daylight reference (-1.5) the Palace settles near 1.3, outdoors is unchanged, and a bright scene
such as snow keeps more exposure instead of being pulled to grey.

An enabled Environment scene owns dynamic material lighting. **Sun brightness** in SKY controls
surface sunlight and sky radiance together. **Map light power** controls authored map fixtures.
**Ambient light** controls sky illumination independently. **Cast shadows** gates the owned sun and
fixture shadows. **Sun size** (climate `sunAngle`, degrees, real sun 0.53) sets how soft sun shadows
get. Shadows are sharp at the contact point and softer the further they fall from the object that
casts them.
The native path suppresses compiled direct lightmaps, baked irradiance, bent-normal AO,
probe lighting, reflection captures and the engine sun/shadows. World materials query an owned
world-space irradiance volume, and lamp lighting is evaluated from authored emitter records.
Map gradient/cubemap fog is bypassed inside recognized forward programs, so it cannot restore
a precomputed scene color after illumination. Environment supplies the live atmosphere.
Engine lighting/shadow switches in EFFECTS are disabled while Environment owns lighting.

The procedural sky shades the map's native sky geometry directly, preserving its depth and
MSAA coverage. Clouds respect background geometry.

### Sky, moon and clouds

The sky is an atmosphere model, not a painted gradient. It is a port of
[glsl-atmosphere](https://github.com/wwwtyro/glsl-atmosphere) (public domain): sunlight scattered by
air (Rayleigh) and haze (Mie) along each view ray, with the repository's constants. A low sun turns
the horizon orange and the sky goes dark after sunset on its own. **Horizon haze** scales the haze
term (0 is a clear deep blue, the default 0.4 is the reference sky). The CPU copy in
`EnvironmentSkyMath.h` integrates the same sky for the light it sends to the relit map, so the
ambient light follows the sky.

**Time of day moves the sun and the moon together** (`ApplyTimeOfDay` in
`EnvironmentClimateMath.h`; climate `timeOfDay` 0-24, `sunPath`, `sunHeight`). The sun goes round
one arc a day: it rises at 6:00 a quarter turn before **Sun direction**, stands highest at noon in
that direction at **Sun height**, sets at 18:00 a quarter turn after it, and at midnight is as far
below the horizon as it was above it at noon. The moon goes round the same arc opposite the sun,
whatever its phase: it rises as the sun sets and is up all night, so a night scene has its moon in
any phase. The sun's and moon's azimuth and elevation are worked out from
these and have no sliders. A sun placed by its angles (the map's own sun on load, a starting look,
`climate sunAzimuth`, a scene saved before the time of day, the sun angles Scene Keys' macros key)
sets the time and path that put it there (`TimeOfDayFromSun`), so the moon still comes up opposite a
sun keyed below the horizon. The defaults put the sun where it used to start: 15:15, 135° and 35° up.

The night follows Jensen et al., *A Physically-Based Night Sky Model* (SIGGRAPH 2001). The moon is a
lit sphere (Lommel-Seeliger shading) with a faint earthshine on its dark side, and the night sky is
the same scattering lit from the moon. The **Moon** group sets its brightness (0 hides it), phase
(the shape: 0 new, 0.25 first quarter, 0.5 full, 0.75 last quarter; it never moves the moon), size (0.52° is the real moon) and color. Once the sun is below the
horizon the moon takes over as the key light: the map is lit and shadowed from the moon's direction,
in its color, at a strength set by brightness and phase (a full moon at 1 is 10 % of a power-1 sun).
The sun fades out over its last degree and the moon fades in over the first six degrees of twilight,
so the light never jumps. The Storm look turns the moon off.

The cloud layer's shape follows Guerrilla's *The Real-time Volumetric Cloudscapes of Horizon Zero
Dawn* (SIGGRAPH 2015): a cumulus height profile, coverage cutting shapes out of the noise, and finer
noise wearing away the edges. Its lighting follows [SebLague/Clouds](https://github.com/SebLague/Clouds)
(MIT): light absorbed on its way through the cloud toward the sun or moon, a floor so the undersides
never go black, and a two-lobe phase that brightens clouds near the sun. The clouds take their sky
light from the Environment sky. **Rain darkening** (default 1) makes rain and lightning storms thicken
the layer, raise its coverage and turn it dark grey. It changes only the clouds. Fog, rain particles
and wet ground are untouched.
Distant 3D-skybox surfaces also pass through the native lighting scopes. Their miniature-world
coordinates and shadow geometry still need validation.

Map resources are verified native packs containing authored inputs and geometry. VRF is an
optional development exporter, with no build or runtime dependency and no map-load extraction.
Preparation status stays inside CONFIG → ENVIRONMENT. See
[whole-map relighting](../effects/whole-map-relighting.md) for current implementation limits,
including owned AO, actor shadow capture, experimental GI and per-map acceptance.

Screen-space indirect lighting and reflections are not part of this renderer, because their
lighting changes when contributing geometry leaves the frame. Nonzero `climate gi` and
`climate ssr` values are rejected. The optional world-space volume below is a separate implementation.

No option here rebuilds full-scene indirect illumination, reflection probes or the map's baked
lightmaps. The controls can produce useful shot-specific transformations without compiling the map.
Complete, physically consistent relighting still requires more scene and material information.

### Internal world-space bounce lighting and rough reflections

The low-level `climate worldGi 1` command enables a fixed 128³ voxel volume. The first enable centers
an unplaced volume at the rendered camera. Either **Place bounce volume here** or the center
coordinates move it explicitly. Camera movement never recenters it. Prepare the map's shadow geometry
first. Added lights use allocated shadow maps or voxel visibility to contribute. The procedural sun
contributes when its environment and sun-shadow switches are enabled.

The renderer voxelizes collision geometry and editor props, injects shadowed direct lighting, and
traces four cones to gather one diffuse bounce. The receiving surface gets colored indirect light
even when its direct lighting is dark. Light edits update radiance. Prop transforms or volume edits
also rebuild the voxel geometry. Camera movement does neither. **Bounce intensity** controls the
contribution. **Volume size** trades coverage for detail. 1536 units gives 12-unit cells, and the
default 2048 gives 16-unit cells. Lighting reset disables and resets this volume.

The low-level `climate sceneReflections` setting enables tracing the same world volume for puddles
and added material props. **Reflection roughness** controls the cone width. This can reflect
offscreen lit geometry. Sky is the fallback where no volume geometry is found. These are coarse
reflections of the added lighting, not textured mirrors or the game's complete rendered appearance.
The scene-reflection switch and volume reset with Lighting. Puddle reflection roughness resets with
Weather. Added material props have their own roughness.

This is a bounded, one-bounce voxel experiment, not Lumen, full-scene GI or hardware ray tracing. Map
reflectance is a uniform adjustable approximation. Editor props use their chosen colors. The receiver
color is estimated from the existing rendered image, not recovered as material albedo. The voxel's
dominant lit face reduces back-face light leakage, but thin geometry, competing lights on opposite
faces, voxel boundaries and mip filtering can still produce artifacts. Native animated actors, native
material textures and emission, and baked lighting are not captured. Added material props can emit
into the volume using their selected color and emission amount. Texture detail is not carried into
the coarse emission cache. The volume adds about 53 MiB of voxel textures plus geometry buffers, and
it costs GPU time for tracing each visible surface. It starts off.

For background on the method, see NVIDIA's [voxel cone tracing overview](https://nvidiagameworks.github.io/rendering.html).
Epic's [Lumen technical details](https://dev.epicgames.com/documentation/unreal-engine/lumen-technical-details-in-unreal-engine)
describe the much broader scene and surface-cache integration that a full Lumen-style system requires.

## Prop materials and light blockers

**SCENE OBJECTS → Outliner → (a mesh) → Material** enables the added prop material path. It provides
metallic/roughness microfacet shading, a separate clear-coat lobe, anisotropic brushed highlights and
emissive color. It does not replace native map materials. Props without it use the default shading. The material sliders stay disabled until enabled.

Place PNG images in the environment asset folder's `textures` subfolder, then **Refresh** assets.
Choose Color, Normal or Roughness, and use an image from the list. Color images are sampled as sRGB.
Normal and roughness data stay linear. Tangent normal maps use the +Y convention, and the shader
handles the OBJ/image V conversion. Normal strength and tiling remain low-level settings. Assigning a
texture enables the material. Clear a slot to return to its scalar/color input.

Files are decoded on the main thread before scene publication. Each file is limited to 16 MB and
4096 × 4096 pixels. The decoded cache is bounded to 256 MB, including live and undo references. GPU
textures use mipmaps and anisotropic sampling. Missing or malformed assets reject the proposed scene
change instead of opening files during rendering. Save/load records asset names, not copies of the
images. Share the corresponding texture and mesh files with a preset.

**Camera visibility → Light blocker only** hides a prop from camera color and depth while keeping its
world-shadow and bounce geometry. The ordinary Hidden switch disables it entirely. A light-colored
card can bounce added lighting, and dark geometry can shape shadows. Emissive props feed the bounded
world-bounce volume even with no direct lights enabled. The voxel cache uses prop color, not its
texture detail. Thin or small emitters remain limited by voxel resolution.

The material reflection path uses the analytic sky or the existing rough scene-reflection volume. It
is not a textured mirror. These controls do not yet provide native material picking, material
painting, glass transmission or full-scene material capture. Existing snow and wetness coatings also
apply to material props, with their documented appearance-test limitations.

## Weather cover: what is indoors

Rain, snow and ash stop at roofs, and snow and wet stop at doorways, from two things worked out from
the map's render triangles and added props. Both load with the map data. Until the geometry is
ready, precipitation still falls but passes through roofs, and coatings stay global.

**The sky map** is one 4096 × 4096 depth map that looks straight down on the playable map (the nav
mesh's box with 512 units around it, about 1.5 to 2 units per texel). It answers "is the sky open
straight above this point" exactly. Surfaces read it through twelve soft taps and count a share
under a third as a seam between two meshes, not an opening. A point is lifted 10 units first (a
kerb, a step or a roof tile's relief is not a roof), and further on a slope. Falling particles and
the distant veil test it once each, so nothing falls under a roof. It looks straight down whatever
the wind: rain sheltered along its slant drew stair-stepped wet on every wall, and the four tilted
"gust" maps that softened it cost 64 MiB.

**The cover volume** (`EnvironmentWeatherCoverMath.h`, `EnvironmentWeatherCover.cpp`) says how far
open air reaches in under cover, which the sky map cannot. It is a grid of 16-unit voxels over the
playable map (at most 320 × 320 × 96; a larger map gets wider voxels), baked once per map on worker
threads:

1. A voxel is open when nothing is above it. Five rays come down each column, and the highest thing
   any of them meets is the roof, so a seam between two roof meshes does not open a column indoors.
2. Two neighbouring voxels are joined when the ray between their centres and most of four rays
   beside it (0.15 of a voxel out) are clear. A crack between a wall and a ceiling, or a seam between
   two meshes, does not join a sealed room to the street; a doorway or window does.
3. Open air spreads from the open voxels through the joins and keeps 79 % per 16 units across a wide
   opening. Walls do not pass it. A wide opening lets more through than a narrow one, and a crack
   lets through next to nothing.

A surface's exposure is the larger of the two. Snow is full where the volume holds 45 % or more and
gone under 10 %; wet reaches further, down to 3 %. Through a wide tall doorway that is snow about
24 to 90 units in, and through an ordinary door less. The volume is stored as eighth roots in one
R8 texture (about 5 MB on Inferno), so a lookup between a street and the sealed room behind a thin
wall leaks about 3 units and not 13. The voxel under a floor copies the air above it, so floors read
the air and not the ground.

The bake runs on eight threads at below-normal priority. Its ray grid covers only the triangles in
and above the volume: over a map's whole box, distant scenery stretched the grid's cells and
Vertigo, whose box runs down to the street, took 43 s. Measured 2026-11-15 with the game running
(seconds; the floor shares come from `coverplan`, which reads the volume alone, so ground beside a
wall counts as reached and not open):

| Map | Voxels | Bake | Floor open | Reached under cover | Sheltered |
| --- | --- | --- | --- | --- | --- |
| de_ancient | 279 x 317 x 53 | 1.3 | 70 % | 17 % | 14 % |
| de_anubis | 285 x 320 x 52 | 0.9 | 75 % | 18 % | 7 % |
| de_dust2 | 298 x 316 x 54 | 1.0 | 66 % | 30 % | 4 % |
| de_inferno | 320 x 316 x 54 | 1.4 | 52 % | 31 % | 17 % |
| de_mirage | 304 x 267 x 53 | 0.7 | 40 % | 49 % | 11 % |
| de_nuke | 320 x 262 x 78 | 1.2 | 50 % | 5 % | 45 % |
| de_overpass | 297 x 320 x 64 | 1.3 | 60 % | 24 % | 16 % |
| de_train | 297 x 272 x 59 | 1.1 | 34 % | 26 % | 41 % |
| de_vertigo | 220 x 218 x 57 | 0.7 | 39 % | 32 % | 29 % |

Each map's plan was read against its layout. Inferno was also walked in game with snow and with rain
(church, wine cellar, garage, arches), and Vertigo's B site, where the plan shows open patches
indoors: they are the roof's skylights, and snow lies only under them. The other seven maps were
checked from their plans only.

**Doorways are walked through.** The volume keeps a second value, traffic: how near a place is to
the way indoors. It spreads out of the rooms (covered places holding under 30 % open air) through
the same joins and keeps 78 % per 16 units, so it is high in a doorway, a quarter of that 24 units
outside it, and gone 120 units down the street. Snow and water use it and one map-fixed pattern of
clumps (`WeatherClumps`):

- Outside a door the snow is trodden: broken into packed, greyer patches with bare ground between,
  up to 42 % bare at the threshold.
- Under cover the snow is scattered: clumps, many at the opening and fewer with every step in. The
  same clumps run across the threshold, so trodden and scattered snow meet without a line.
- Where snow is trodden or scattered it melts: the ground between patches is damp and small pools
  form by the door.
- Rain reaches in under cover as patches of the same pattern, and worn ground by a door holds
  more pools.

**Footprints** (`EnvironmentFootprintMath.h`, `EnvironmentFootprints.cpp`). Players leave marks
where their feet go, and the feet are the player's own: each frame the hook reads the ankle and toe
bones of every live pawn from its world pose (`ReadFeet` in `EnvironmentScene.cpp`) and works out
where each boot is, which way it points and how high its sole is off the ground the pawn stands on.
A foot is set down in the frame it is low and has stopped travelling over the ground
(`FootSetDown`); that is a print, and a splash where there is water. Low alone is not enough: a
running foot drops to the ground while still flying forward at about 380 units a second and comes
to rest a boot's length further on, so an effect fired when the sole first got low played early and
behind the boot. `environment footview` draws what is read over the frame, to check this against
the picture: a cross on each foot (green set down, red not, white in the air) and a yellow ring
where and when each footfall fires; `mvm_debug` logs each footfall with the foot's and the pawn's
speed. Player models differ by a unit or so in where those bones sit, so a
sole is read against that foot's own lowest level (`FootLift`). A pawn in the air leaves nothing. A
pawn whose bones cannot be read falls back to steps worked out from how far it has moved: one every
34 units on alternate sides, and both feet side by side where it sets off and stops
(`AdvanceFootTrack`). The marks live in a 1024 by 1024 image fixed to the world around the camera
(0.75 units a texel, 384 units each way) with the height each was made at, so a mark on another
floor is not drawn. A mark belongs to its own level to within seven units: one made in the street is
not drawn on the kerb beside it (shared across 28 units, a trampled patch ran up over a kerb as a
slab hanging in the air). Thin snow shows a print as a dent: tilted walls, a packed bluish floor, worn
through to the ground where the snow is thin and trodden. While snow falls the marks fill back in
over about 45 seconds. Marks are made as the game plays: a seek or a map change clears them, so an
export does not reproduce the marks of an earlier pass. `rainview` shows prints in magenta.

**Deep snow** (`SnowTrench` in `EnvironmentClimateShaders.h`). The Snow depth control
(`climate snowDepth`, 0 to 1, default 0.5) says how deep the snow lies where it has drifted: up to
14 units, about shin deep, in the drifts and a third of that between them. It does not lie as one
flat sheet: `SnowRelief` shapes it into mounds and hollows fixed to the map (broad swells several
paces across with lumps on them), from about 0.3 to 1.35 of its depth. The mounds are waves, which
repeat, so the ground they are laid on is first pushed about by slow noise and the lumps are made
strong in one place and faint in the next: no two stretches of a yard line up in rows. The method is the one
published for deformable snow in games: the ground the game drew is the bed of the snow, its
surface is above it, and a map of how far the snow has been pushed down is built from where the
players' bodies actually are. Here that map is the image of marks, and the feet press it: a boot on
the ground pushes the snow to its bed in an oval the size of the boot (about 14 by 6 units) with a
ragged wall sloping out beyond it. The leg above the foot goes through the snow too, so wherever a
foot passes the snow is cut down to the height of its sole (`FootPress`): a slow walk, with the
feet hardly lifted, drags a furrow nearly to the bed from one hole to the next, one furrow a leg
with a ridge between them; a run, with the feet lifted high, cuts only the deeper mounds; a foot
lifted over the snow leaves it alone. Holes that overlap are one hole. Pressed
snow is packed a fifth as deep, and the snow pushed out is heaped round the mouth of a hole. The
view ray is traced down through the snow against that surface, read through a filtering, wrapping
sampler. The height at which the ray enters the snow as it lay is found exactly first (the mounds
are a formula: six even heights, then five halvings); if the snow at that point has not been
pressed, that is what the eye meets. Only a ray that enters over pressed snow goes on into the
hole, in samples at most 1.1 units apart along the ground, then three halvings. (Walking every ray
in steps drew the near lip of a furrow as a staircase: a ray that clips the lip's top corner is
inside the snow for less than any step, so it was skipped by an amount that crept and snapped back
along the furrow.) So a mound hides what is behind its crest, the near wall of a hole hides its
floor and the far wall stands behind it, the wall turned to the sun is bright and the floor sits in
the other wall's shade. Players stand in the snow: a pixel of a player lower than the snow between
it and the eye is drawn as snow (`ApplyCoatings`, `legHeight`), so boots and shins sink out of
sight and show again inside the hole they pressed, with a soft snow line on the leg. A foot set
down in snow also kicks up a small puff of powder (the foot splash's sheets, white, where the sky
is open). The ground itself is not moved: the snow does
not stand in front of a wall's foot or a prop, and from a camera lower than about 12 degrees the
depth flattens. Falling snow fills a hole from the bottom at half the pace of a print.

**A body that falls presses the snow too.** A dead player's bones are read the same way as a live
one's feet (`ReadFallen` in `EnvironmentScene.cpp`: pelvis, spine, head, both arms and both legs,
from the pose the body is drawn with, the pawn's own or its client-side ragdoll's). Every part of the
body presses the snow down to its own underside wherever it is (`PressBody`), so nothing is marked
while the body is still upright, the snow is cut as it drops, and where it comes to lie there is a
hollow in the shape of the body. The ground under a body is taken from the body itself (a little
under its lowest bone), because a dead pawn's origin stays where it died. A body is pressed again
only when it has moved, and once a second while it lies there so falling snow does not fill in under
it. The frame the trunk reaches the ground throws up powder in snow, or a splash where there is
standing water, at the hips and the chest. Nothing of ours is drawn on the body itself: players,
alive or dead, keep only the game's own frost shading. Thin snow shows the body's shape as a print.

Only a standing player's own legs are hidden behind deep snow. A pixel counts as a leg when it is at
or above that player's feet (`ActorGround`), so a player overhead, standing on another's head or
jumping over, never claims the body below (that painted a chest as snow), and a player in the air is
skipped, so a jumping player's boots carry no snow line.

**Rain's partial shelter is patchy, and so is the mirror.** Where the sky reaches wet ground only
in part (a roof's edge, the sheltered side of a wall in wind) the share is turned into patches
(`WetOpen`), and the share of the wet film's mirror that is sky uses the same patches with softer
edges; as plain amounts both ended along the roof's outline in a ruled line. Blown mist is lit by
the sky and sun, not by the scene behind it, and is grey: lit like the streaks (the scene read at
the pixel and four points off it) it laid faint copies of the scene over the frame, a blur with
every roof line doubled. Rain's ground spray is a fraction of blown snow's and capped, fades over a
pace or two at a shelter's edge, and goes by how level the ground is.

**Frost is handed back clean.** After the game's frost switch was held on, it is held off for 240
updates before being given back to the map: the game works frost out only while the switch says on
or off, so given straight back, players kept the frost they had (seen after Reset weather). Each
player also keeps a stored amount that melts slowly, and 240 updates is about a second at a high
frame rate, so the frost showed again a moment after the switch was handed back. Once our snow has
frosted people on a level, their frost is drawn at zero strength (the frost hook) whenever snow is
not wanted, until it is wanted again or the level changes.

**Wind gusts** (`climate gusts`, 0 to 1, the Gusts slider under Weather; `EnvironmentWindFluid.cpp`).
Wind is seen by what it carries, so the air is run as a 2D fluid over the whole map, seen from above
(Stable Fluids: curl, forces, advection, a 20-sweep pressure solve, projection; cells of 24 units,
wider on a huge map). The steady wind (Wind speed and direction) blows across it and gust fronts,
bands a few hundred units deep and much longer across the wind, sweep downwind through it, pushing
the air to several times the steady speed and loading it with what is drawn as blown mist in rain
and blown snow in snow. The air cannot pass through houses: their faces are read from the weather
cover volume (`WindObstacles`: a rise of 96 units within three cells, taller than a person, so a
yard wall counts and a stair, a kerb or a slope does not), a few rows a frame after the cover is baked. So gusts squeeze down streets,
spill round corners and curl in the lee of buildings, and players drag the air as they run. The
weather shaders read the fluid (`WindAt` in `EnvironmentClimateShaders.h`): where the air moves faster than
the steady wind, rain leans further over and thickens, snow is carried sideways and drawn out into
streaks, and far off a front is a curtain of thicker weather. The air itself tumbles: turbulence is
added in step with its speed, so a gust wanders instead of running in ruled lines.

**The wind is seen as a pattern carried by the air** (`GustFlow`). All of
it rides downwind together at the speed the fronts travel (a rigid slide, which never tears it),
and where the air at a place moves otherwise it is bent that way by flow mapping (two copies half a
period apart, cross-faded), by no more than a couple of the pattern's own widths. Carried the whole
way by the local velocity, the eddies wound the pattern into marbled stripes across the floor. How
gusty the air is (`GustAmount`) is how far it surges over the steady wind (a front runs at
1.7 × (140 + the wind) over it), shaded by what the fluid carries there; the fronts decide where. How gusty the air
is (`GustAmount`) is how far it surges over the steady wind (a front runs at 1.7 × (140 + the wind)
over it) or what the fluid carries there.
- **In the air** (`GustMist`): waves. A front is a body of blown snow, spray or ash a few hundred
  units across, shaped by billows, thickest over the streets and thinning up over a couple of
  storeys; it sweeps through at the gust's speed, so the air goes white in snow, grey in rain and
  dark in ash, and clears as it passes. Between fronts the air is nearly clear. Thin sheets marched
  along the ray averaged into a flat grey fog over the whole frame and were dropped. The march is
  not dithered: what it reads changes over hundreds of units, and a per-pixel jitter laid a screen of
  grain over the frame.
- **Along the ground**: wisps streaming over the floor where the view ray lands (24 units thick),
  torn at three sizes so they thin out softly (light spray in rain). A cut-out layer read as paint.
- **On wet ground and puddles** (`ApplyCoatings`): a gust raises wavelets, crests across the wind,
  drifting slowly downwind (`GustRipples`), only where the gust runs; they break up what the film
  mirrors (up to 45 % less mirror and rougher reflections), so a gust is a rougher, darker patch
  sweeping over the water, glinting where its wavelets catch the sun.
- **On snow**: loose snow runs over the surface in streamers, a brighter white where they run and
  the crust a shade greyer between.
The Gusts control shows it at full strength from 0.67 up. (Before 2026-11-17 the air showed only
the fluid's density cubed as faint wisps, and nothing on the ground: in rain a gust was invisible.)
Still air behind a house does not stand the rain up straight. The fluid is stepped as the game
plays, like the footprints: a seek restarts it and the fronts fill it again in a second or two.
Without map data there are no gusts. `environment wind-view [on|off|map|<units>]` shows it from above
in a corner of the frame: walls red, fast air blue, what it carries white, the camera green.

What a gust carries is drawn for every weather, in its own amount (`GustMist`): per unit of air in a
front, 1/600 in snow, 1/700 in ash and 1/1000 in rain. Ash is smoke, and what the gust carries through it is a warm dark grey, where the
falling ash stays its own pale flecks.

**CS2's own textures.** The coatings are drawn with Counter-Strike's own art, read from the installed
game at run time and never shipped (`EnvironmentGameTextures.cpp`, reading through
`ReadGameSurfaceTextures` in `MapData/MapTextures.cpp`):

- **The snow** is always CS2's: the snow the Cobblestone winter update laid on its streets,
  `materials/cbble/snow/snow_mix_2`. Its three maps are found through the material, so a texture
  renamed by an update is still found, and uploaded with their own mips as BC7. Everything in them is
  used, as `csgo_environment` reads them (`GameSnowAt`): the colour and, in its alpha, the cavities; the
  normal (hemi-octahedral, CS2's HemiOctAnisoRoughness) and, in the normal map's blue, the roughness;
  the height, which also offsets the lookup toward the eye (parallax, about three units over a repeat).
  They are laid top-down over the map, one repeat every 128 units, each mip picked from how many units
  a pixel covers. The crust's relief is lit as the map lights the snow (its normal against level snow in
  the sun, its tilt in the sky light), so it reads in sun and shade alike; its roughness gives it a GGX
  sheen toward the sun and decides where it sparkles (the smooth specks); a thin patch's edge follows
  the height. The procedural lumps and grain it replaced are removed. Drifts, footprints and trenches
  stay. Until the textures have loaded the snow is plain white.
- **CS2 water beads** (`rainTexture`, on by default): `materials/dev/water_droplets`, the beads of water
  CS2's wet surfaces use, one repeat every 32 units on wet ground that is not under a pool. They bend
  what the wet film mirrors and catch the sun.

Both read the game on a worker thread the first time they are wanted; until then, or if the game has no
such textures, the snow is plain and the wet ground has no beads. `environment status` prints what was found
(`cs2 textures:`). The texture reader is checked by `MapDataTests` on synthetic data and, where the
game is installed, against the real files.

**Snow glisten.** CS2's crust (above) keeps the snow from being a slow white gradient that eight
bits of colour draw as bands, and half a step of per-pixel noise breaks up what gradient is left. It
glistens (`SnowGlisten`): a stationary jittered
grid of sparkles, after Shopf and Wang & Bowles 2016 ("A Robust and Flexible Real-Time Sparkle
Effect"). The ground is divided into cells fixed to the map, each holding one crystal at a random
spot with a random tilt, drawn as a round point about a pixel and a half across (never a whole
cell). A crystal flashes by how nearly its tilt lies halfway between the eye and the sun, plus a
fainter flash straight back at the eye so overcast and lamplit snow still twinkles, so the glints
come and go as the camera or the sun moves. Cells are sized from how much ground a pixel covers, in
doubling steps blended into each other, but never more than 2 units across, and the glints fade out
between 220 and 700 units from the camera. Sized from the pixel alone there were as many on screen
across a far yard as at the feet, and a glint one pixel wide a thousand units away is a crystal the
size of a plate: distant snow was strewn with large bright dots. A glint
is as bright as the light on the snow there, and the snow itself is kept a tenth under full white
so that a glint can be brighter than it. Everything drawn on the snow (lumps, grain, glints, the
shade of its drifts) is looked up at the point of the snow's surface the eye meets, which is nearer
the camera than the ground point of the pixel because the surface stands above the ground. Looked
up at the ground point, the patterns slid over the mounds and holes as the camera moved.

**What snow costs.** Measured on Inferno at 1920 by 1080 with 4x MSAA under the map's own lighting:
the surface pass takes about 0.3 ms a frame with no coating and 5.6 to 6 ms with snow on. Of that
about 2.1 ms is the surface pass every coating pays (normals, reconstruction), 1.6 ms is deciding
where snow lies (the cover lookup and its noise), 0.65 ms is the trace of holes and 1.3 ms the rest
(light taps, grain, glints). The image of marks carries a chain of smaller copies of itself so the
shader can ask in one read whether any foot has been near a point, and skips the trace where none
has; whole snow skips the wet-ground section. The weather cover of a map (about two seconds of
eight threads on Inferno) is kept in `<AppData>\HLAE\source_mvm\environment\cover` under a
fingerprint of the map's triangles and read back in a few hundredths of a second on later launches.

**Player fill** (`climate playerFill`, 1 to 4, default 1.8). Under dynamic lighting players take
their ambient light from sparse probes and stood out near black in shade beside walls lit per
pixel. The composite lifts the darkest shade of player pixels (found with the same player mask the
coatings use) by this factor, fading to no change as a pixel reaches 0.3 luminance, so a player in
the sun is left as lit. It is a fill light, not a fix to the probe lighting. 1 switches it off.

**Splashes** (`FootSplashVS` in `EnvironmentSplashShaders.h`). A foot that comes down in standing
water (the frame the foot is set down, at the boot itself) throws it out low: nine sheets of water that leave outward, rise a hand or two and tear into
shreds within a third of a second (upright, facing the camera, cut from noise fixed to the world),
66 small drops on ballistic arcs that land within a pace, a ragged patch of froth where the boot
went in, and a train of broad ripples through the water's reflection. Thrown water is drawn dim
and lit by the scene, at most 45 % opaque: a grey smudge against what is behind it, never white.
Nothing is painted on the water as a ring. The last 32 strides are kept, each for under two
seconds, and only those that land in a pool splash (`PuddleAt`, the same pools the ground shader
draws), so damp ground and snow do not.

**Frost on players** (`EnvironmentNativeRain.cpp`). Our snow is kept off players, weapons and
corpses. They are frosted by CS2's own frost material instead, which grows on players, weapons and
chickens outdoors, melts under cover, and clears from a hot barrel or a lit molotov. A map asks for
it with `frostenabled` on `info_map_parameters`; while snow falls or the Snow control is above zero
the hook sets `r_csgo_frost_force 1`. The frost is the game's own, at the game's own strength; there
is no amount control.

**One weather, one shading on players.** Rain is the game's own wet shading and nothing else; snow
is the game's own frost and nothing else. While rain falls the frost switch is held at 0, whatever
snow still lies and whatever the map asks for, so nobody is frosted in rain (rain on old snow used
to frost players, and frost left from a moment of snow stayed on them outdoors). The game's wet
shading (`NativeWorldWeatherFor`: the world is marked rain-exposed, with a rain strength, ripples
and wetness) is switched on only while rain falls: the Wetness control wets our ground film at any
time, but the game's own materials and players only in rain. Who is wet or frosted, and that they
dry or thaw under cover, is the game's own sky test in both cases. Where
frost grows is the game's own sky test, not the weather cover above, so the two can disagree at a
doorway. The game's `r_csgo_frost_grow_time`, `r_csgo_frost_melt_time` and
`r_csgo_frost_indoor_distance` set its timing. CS2 builds before November 2026 have no frost;
`environment mapinfo` then reports it as unavailable.

**No frost after the snow stops.** The game works its frost out only while the switch says on or
off. Handed back to the map (`-1`), players keep the frost they carry, so a reset used to turn them
white again a moment later. Once our snow has frosted anyone, the switch therefore stays at 0 until
the level changes, and what players still carry is not drawn while it melts: each frosted thing
keeps four numbers (frost now, the most it has had, meltwater, barrel heat) that one small routine
in `client.dll` packs for the material, and the hook zeroes the first two there. If that routine is
not found after a game update, `environment mapinfo` says so and the leftover frost melts at the
game's own pace instead.

**The wind's angle moves where weather lands.** The weather wind and fall speed give a lean (0
straight down, 0.71 at 45 degrees). It does nothing under a lean of 0.1 and is at full strength by
0.55: the snow preset (wind 18, speed 65) only thins the sheltered side, and a strong wind clears it.

- In the open, a place is missed when the line back toward the weather runs under a roof. Six
  points up that line (30 to 230 units) are tested against the sky map and the share that are clear
  is what reaches it, so the sheltered side of a wall falls off: nearly bare at its foot and more
  with every step out, in a strip that widens with the lean. Snow turns that share into patches
  that thin out, as it does at a doorway. (Two points and the lesser of them drew the strip's edge
  as a ruled line across a yard.) Each point is three taps across and along the line, spread wider
  the further up it is, so the share falls off smoothly: with one tap a point it stepped in sixths,
  each step on the roof edge carried along the wind, and snow patches beside a container or under a
  ledge ended in straight lines (2026-11-18). Lying snow counts only a quarter of this: it came down over hours
  of shifting wind, so its sheltered side is thinned, not cleared. Rain counts all of it.
- Under cover, the volume is read up to 90 units upwind, so an opening that faces the wind takes
  snow and wet further in and one that faces away takes less.
- A falling particle in the open is dropped when the line it came down ran under a roof, and one
  just under cover is kept when open air reaches it and that line is clear.

Set it with `mirv_filmmaker environment weather wind <speed> <direction-degrees>` or the Weather
card's wind controls. `rainview` adds red for traffic.

An earlier version averaged the sky map in rings around a point to find the fringe. That counted
open sky on the far side of a wall, so rooms beside a street got patches of snow, and a per-grain
jitter on the rings drew the fringe as speckle.

`mirv_filmmaker environment rainview on|off` paints the cover: grey under open sky, green where open
air reaches in under cover, dark blue where it does not. `mirv_filmmaker environment coverplan
<file.ppm>` writes the walkable floor seen from above in the same colours and prints the share of
each, for checking a map's interiors at a glance. `mirv_filmmaker environment status` prints the
bake's state and times.

Thin openings, alpha-cutout foliage, moving native actors and anything missing from the render
triangles are not reproduced exactly. Doors that open and close do not change the volume. Embers,
motes and smoke remain local effects and do not use the cover.

Rain streaks are motion blurred. Each streak is one drop smeared over its length, so its opacity is
min(1, 60 × drop width / streak length). Long, fast streaks are faint and soft. Streak length does not
change the rain's total coverage. Drop size spreads below the size setting, which is the largest drop.
Most drops are smaller, down to a quarter of it. Size sets streak width and length. A streak is never
drawn wider than two pixels, so a drop right at the lens is a thin line and not a wedge across the
frame, and drops fade in over 24 to 110 units from the lens. Rain takes the map's light at the drop
times 3, so it reads as water catching light. No drop or splash is drawn more than 40 to 50 % opaque.

## Rain, wetness and storm rendering

Coverage puddles use the standing-water film described in the surface section above. Native CS2
water meshes and the wet-ground ModelDoc job are not built for coverage, because regenerating that
geometry stalled every slider edit.

Native CS2 world weather is responsible for weapon, knife, glove/arm and player base wetness. While
precipitation is Rain, the environment does what de_train's `info_map_parameters` does. It registers
the map world and the first-person model world as rain-exposed in `WorldRendererMgr`, and it pushes
rain, ripple and wetness strengths. CS2's own droplet and wet shading therefore appears on any map.
Player pawns cache their exposure on their first think, so the cache is cleared whenever the weather
changes. Switching rain off restores each world's original state. `environment mapinfo` reports the
worlds.

Valve's `rain_storm` particle is not used. It culls drops against baked precipitation ray-trace data
that only weather maps ship, so most maps show a few streaks at most.

Entering cover does not clear those native wetness values. Active impacts are a separate animated
normal/refraction layer, evaluated only where the weather cover proves exposure.
Prepare map geometry for roof exclusion. With no map geometry, falling rain still renders but
surface impacts fail closed. Impact events use absolute demo time. Pause freezes them, and seeking
reconstructs the same pattern. Their finite lifetime avoids an accumulated simulation cache.

Rain, snow and ash have one density everywhere. Near the camera, they form a single world-coordinate
particle lattice with one cell size (0.028 times the weather radius). A particle's position depends
only on its cell and absolute demo time. The camera chooses which cells are drawn but never moves
one. The particles fill a window about 875 × 425 units around the camera (at the default radius).
Beyond that window, the same density continues as a per-pixel veil that integrates the particles'
expected coverage along each view ray. Particles fade out over the window edge as the veil fades in,
so distance never changes how thick precipitation is.

The veil counts only ray stretches that are open to the sky in the sky map and inside the
precipitation region. Looking out of a doorway therefore shows the same fall as standing outside, and
none indoors. With map geometry, the region is the map's collision bounds, from the floor to half the
weather height above the highest collision. Precipitation therefore covers the whole map and stops at
its edges. Without map geometry, the veil reaches ten weather radii. The veil's clumping noise is
world-anchored and falls with the particles.

Count sets the cell size (every cell holds a particle), so a light fall draws proportionally fewer
particles over the same window. Changing Count re-seeds the lattice. Each particle tests the sky map and
takes its light once, in the vertex shader, rather than per pixel. Rain drop sizes spread below the
size setting (see the motion blur notes below). Ash flakes are thin, translucent torn pieces that flip
as they fall and take the map's lighting. Snow flakes and the far snow veil take it too, with a high
gain that saturates at the weather colour: daylight snow stays white, snow under a dark sky dims with
the scene, and lamps still light it. Looks exposure applies to all three. Unlit snow drew white flakes
and a grey haze over a night scene and brightened the whole frame (issue 135).

Drops use wind-aligned streaks, radial band transitions and conserved subpixel opacity. There is no
camera-centered curtain or special upward-view branch. Scene-color radiance estimates inherit map
brightness and color. Authored lights and storm light add shadowed scattering. Fog attenuates distant
drops. Drops fade in over roughly 14 to 56 units. The per-drop opacity cap keeps near streaks from
becoming solid bars. Rain also wets exposed ground by itself. Wet surfaces mirror the scene through a
short screen-space trace (or the procedural sky when that pass is on), so wetness reads as a water
film. Ground ripples use one world-anchored impact field at every distance. Splash geometry (a small upright
cup of water that lasts an eighth of a second, plus a few fine droplets; no ring is drawn, the rings
are the ripples in the reflection) fires in at most about a third of the impact cells per cycle,
drawn down to about 24 units from the lens, dim and at most about a third opaque. The estimate does not use CS2's reflection probes or material buffers,
so it cannot recover offscreen map lights. The fixed preview and export quality policy applies.

Smoke coverage uses only the current smoke window's `mboit_mixed_combine` zeroth moment. The handoff
is cleared before each window and when the current entity snapshot has no smoke. Projectile lifetime
filters exclude future and expired detonation records after seeks. Ray entry preserves rain in front
of smoke. Optical distance attenuates drops within it, including a camera inside the cloud. The
engine's current absorbance supplies density. A cached sphere does not supply opacity.

**Smoke lies over the snow and the wet.** The coatings are painted after CS2 has drawn its smoke,
and smoke writes no depth. CS2 blends a smoke onto the ground as `frame = ground * T + smoke's own
light`, with `T = exp(-b0)` from the same zeroth moment. The composite takes the smoke's own light
out of the frame (the frame less the bare ground times `T`), coats the ground, and puts the two
back together as `coated ground * T + smoke's own light`: the ordinary over composite with the
game's own transmittance. Only the share of the absorbance that belongs to a placed smoke counts;
the same pass draws floor paint, glass and leaf cards, and paint goes under the snow. Before this
a smoke was laid back by guessing its thickness from how much brighter it made each pixel, and the
bare ground under it showed through as a hole in the snow.

**What the game draws over the world.** CS2 draws its crosshair (`csgo_crosshair.vfx_ps`) and the
engine's debug overlays such as `ent_text` (`ui.vfx_ps`) onto the tonemapped image before the
before-UI boundary. The composite runs just before the first of them, on the same target. Run at
the boundary, it took them for part of the world: snow was painted over them and wet ground
mirrored them.

**The map's own clouds go with its sky.** CS2's clouds are geometry in the 3D skybox, drawn after
the sky, so replacing the sky does not remove them: they stayed in front of the Environment's sky
as dark flat shapes. While the Environment's sky is drawn, every cloud material's opacity is held
at zero (`CloudFx_SetEnvironmentHidden`), as the map's sun disc is suppressed. Every two seconds it
checks whether the level has loaded more materials and, if so, looks for cloud materials again, so
one that registers after the first is hidden too. The Effects tab's
cloud settings are neither read nor changed, and the clouds come back as they were set when the
sky goes off. **Hide map clouds** (Sky card, Clouds group, climate `hideMapClouds`) does the same
under the map's own sky: the map's sky stays and its clouds go, and with Clouds on the Environment's
cloud layer is the only one over it. The switch is always shown; with the added sky on it changes
nothing, because the map's clouds are hidden then anyway.

**Nothing is drawn over CS2's menus.** With the pause menu (or the main menu over a map) open,
Panorama draws it straight onto the back buffer after the tonemap and before the engine's overlay
text, and the BeforeUi boundary does not come (traced 2026-11-17: `post_process`, then
`panorama` / `panorama_fancyquad` for the whole menu, then `ui.vfx_ps`). The composite waited for
the overlay text and drew the rain, the sky and the fog over the menu and the scoreboard. The first
Panorama draw onto the back buffer after the tonemap is now a boundary too
(`AfxBoundaryShaderKind` in `RenderSystemDX11Hooks.cpp`), so the composite lands under the menu.

**The ground under a sunlit tarp is lit in the tarp's colour.** CS2's own lighting shows the shade
under the Mirage B-site tarp bright and green: the cloth passes the sun on as soft coloured light.
Ours treated a tarp as a concrete slab, and the shade under it was a flat dark grey, about a fifth
as bright. The map marks such materials (see [map data](../effects/map-data-extraction.md)), and the
game draws their shaded face lit by the light on the other face. The bounce now holds cloth the same
way: the sheet is solid, and each face also sends on the light that falls on the other, through the
cloth's colour (`VOXEL_CLOTH`, `GiClothPass` 1). The tarp's sun shadow is unchanged and stays solid,
so the result is the same from any distance. A first attempt drew cloth into the sun shadow map
through every other texel; a far cascade filtered that pattern differently from a near one, and the
shade got lighter as the camera came closer. It was removed.

**The first-person gun takes no coating.** CS2's own wet shading already runs on the weapon and
hands. The wet film and the rain impacts are keyed to the ground, and on the gun they drew the
floor's ripples.

**A flash needs a shot.** CS2 raises `weapon_fire` for a click on an empty magazine as well as for
a shot. The muzzle light reads the firing weapon's magazine and gives no light for a click
(`VfxDryFire`: empty now and empty at that weapon's previous event). A magazine that cannot be read
never silences a shot.

Puddle coverage controls the shader film independently of wetness. Zero means no automatic
standing water, even with wetness at one. Increasing coverage grows the same world-anchored noise
basins, and downward faces and draining slopes are rejected. A local radius, elevation and depth confine
the film. This is a screen-space appearance test, not a hydraulic simulation or a mesh rebuild.

World coating adds view-dependent Fresnel reflection and light highlights with modest varying
absorption. Native player and viewmodel wetness is excluded from that additional base film. For generic
world surfaces this is a surface-response approximation. The post pass cannot identify wood, metal
or concrete, nor change their original material roughness. Maps authored with `F_WETNESS` retain
their native per-material behavior. Impact refraction rejects taps crossing depth discontinuities.

Lightning uses a 64-segment midpoint-displaced trunk, three primary branches and optional secondary
forks, with connected parents, tapered widths and seeded brightness and pulse variation. Geometry,
scene light, clouds and fog share the same absolute-time event. Segments rasterize directly instead
of searching every segment at every screen pixel. Each segment draws a hot core at least a pixel wide
and a wide bluish glow, so bolts stay readable against a daytime sky. Dark intervals do not render
lightning geometry or allocate its shadowed light. Existing authored light and shadow slots remain reserved.

Technique references, studied and independently implemented (no external shader source copied):
[Tatarchuk/Isidoro, real-time rain](https://www.realtimerendering.com/advances/s2006/Tatarchuk-Rain.pdf),
[Lagarde, dynamic rain and accumulation](https://seblagarde.wordpress.com/2013/01/03/water-drop-2b-dynamic-rain-and-its-effects/),
[wet-surface optics](https://seblagarde.wordpress.com/2013/03/19/water-drop-3a-physically-based-wet-surfaces/)
and [their real-time approximation](https://seblagarde.wordpress.com/2013/04/14/water-drop-3b-physically-based-wet-surfaces/).
VRF inspection of Train's `hrts2_wood_floor_boards_02_wet` and Ancient's
`hr_ancient_blend_groundcarvings01-grounddirt01-wet` confirmed `F_WETNESS`, separate rain, ripple and
wetness strengths, height inputs and puddle roughness 0.02. The Ancient shallow-water
material has roughness 0.799 to 1, muddy absorption and a jungle cubemap. Those are inappropriate
defaults for clear puddles on other maps. Research dumps are local build artifacts.

Regression coverage includes seeded channel connectivity, smoke ray ordering, dense interior rain,
seek reconstruction, upward rain, unlit rain and coverage independence from wetness. Use
`Automation/verify/verify-weather-session.ps1` for repeatable wide, upward, smoke, coverage and
lightning captures in one running demo. First-person cover, moving actors and native material
appearance still require inspection of real CS2 screenshots. Unit tests cannot certify them.

## Shader initialization

Environment shaders are compiled with the Windows system compiler, referenced by full path. This
avoids accidentally reusing an older compiler that the game already loaded. Compiled bytecode is
cached under the local application data folder in `%APPDATA%\HLAE\source_mvm\shader-cache`. Shader
source, macros, entry point, compiler identity and flags identify each entry. An exact input
comparison and a bytecode checksum reject stale or damaged entries.

The CS2 build prepares all 57 main-renderer variants, including the four atmosphere-pixel variants,
plus nine support programs (66 total). They go in `environment-shaders` beside the hook DLL, and the
`x64-source2` component installs them. At runtime, the renderer checks the writable cache first, then
the packaged bundle, using identical validation. Missing, stale or corrupt entries fall back to
compilation on up to four bytecode-only background workers. CS2 stays open and responsive during
this, and the Environment panel reports the completed shader count. The render thread publishes
completed shaders before rendering environment effects. Native sky and lighting remain active until
the Environment renderer is ready. Device loss restores native lighting. Failed preparation leaves the
game running and reports an error.

Preparation and device creation share the pixel-entry list. An incomplete prepared set reports the
missing shader and stops renderer initialization, instead of compiling it on the render thread.
Placed meshes select separate legacy, PBR-material and water shader entry points from the known object
type. All MSAA variants are available. Depth-based normal fitting shares each tap's world/viewmodel classification and receiver position.

Prepared bytecode also stays in memory for read-only caches. Same-device window resizes retain the
renderer, and only size-dependent surfaces are recreated. No game DLL is replaced.

## Internal studio-lighting capabilities

**SCENE OBJECTS → Objects → Add light → Area** creates a rectangular, one-sided emitter. Width, height
and roll control four fixed quadrature samples across its surface. When dynamic shadows are enabled,
every sample has its own world shadow map. This is a four-sample approximation, not a path-traced area
emitter. Large sources near thin blockers can show stepped penumbrae. The same samples illuminate
surfaces, added fog and the experimental bounce volume.

**Source radius** adds a bounded light-space blocker search and soft-shadow filter. Zero keeps the
existing sharp shadow comparison. Filter radius grows with receiver distance behind a blocker. Its
maximum footprint is 32 shadow-map texels. Thin blockers and cube-face boundaries can still show
filtering artifacts. Area width, area height and source radius are separate controls.

Spot and area lights offer procedural **Window**, **Blinds**, **Foliage** and **Caustics** projectors.
Patterns are anchored to emitter position and orientation, not the camera. Caustics animate at
absolute demo time. **Focused**, **Batwing** and **Strip** profiles shape the angular distribution.
These are art-directed formulas, not imported IES profiles. Point lights reject directional
projectors.

**Pulse**, **Flicker**, **Lightning** and **Beacon** cues modulate intensity using demo time, rate,
amount and phase. Zero amount keeps steady intensity. Seeking reconstructs the same cue. The projector
caustic effect does not simulate refracted water transport. Dynamic cues can make bounce-volume
updates expensive, so use them deliberately when world bounce is enabled.

All studio settings save with the environment, duplicate with their light, and reset with Lighting.
New lights start with steady point or spot behavior. `environment.studio` probes report emitter settings
and evaluated intensity. `environment.shadow` reports shadow-map updates.

## Scrolling and input in this tab

Slider hit tests reject content outside the scroll viewport. A control scrolled under the pinned header
or the pinned footer cannot be grabbed. Starting a new drag requires UI mouse mode.
## Lightning and animated bounced light

Lightning is weather, so it lives on **WEATHER → Lightning**. **Looks → Presets → Storm** sets a dark
cloud layer, wind-driven rain, volumetric mist and wet surfaces. The **Lightning** switch works
whatever the precipitation is doing, and Weather's Reset clears it too. Enabling Lightning anchors the
storm around the current camera automatically. Its fixed center and strike floor remain low-level
settings. Strikes are seeded, branching world-space channels with several pulses and localized
illumination inside the cloud volume. Opaque scene depth hides the channels behind buildings. Their
cloud and fog extinction is an approximation, not a fluid or electromagnetic simulation.

Timing and channel geometry use absolute demo time. Pause and seek reconstruct the same strike. The
low-level `climate stormHold 1` command freezes a repeatable bright strike for stills and lighting
setup. Turn it off for animation. Display, MSAA movie output and EXR use the same storm functions.
Thunder audio is not included.

Lightning uses one temporary point light in the render snapshot, with the existing map and prop
shadow geometry. It scatters into fog and illuminates surfaces. It needs prepared map geometry, fewer
than sixteen enabled authored lights and fewer than four authored shadow lights. If those resources
are unavailable, channels and cloud flashes still render. The renderer status reports why surface
illumination is unavailable. Authored lights are never deleted, and the temporary emitter never enters
saved scenes or Undo. Storm controls persist in version 14 presets. Incompatible versions are
rejected. Weather reset also clears lightning.

The low-level `climate worldGi 1` command enables dynamic single-bounce lighting. Place its volume
around the surfaces being filmed. Lightning, animated point, spot and area lights, and emissive editor
props update the volume when their energy changes, including clearing radiance when a pulse ends.
Changing a light does not revoxelize unchanged geometry. Lights with allocated shadow maps use them.
Other added lights use voxel visibility inside the bounce volume. Blockers beyond that volume are
unknown on the latter path, so use shadow maps for distant emitters. Coarse voxels can miss thin walls.
Native map reflectance remains a uniform approximation. This does not recover native emissive textures,
normal maps or material albedo, or dynamically reconstruct native moving entities.

Console examples:

```text
mirv_filmmaker environment look storm
mirv_filmmaker environment climate stormHold 1
mirv_filmmaker environment gi-center
mirv_filmmaker environment climate worldGi 1
mirv_filmmaker environment climate bounce 1.5
mirv_filmmaker environment climate stormHold 0
```

## Weather

Choose Clear, Rain, Snow or Ash, then adjust intensity, fall speed and wind. Intensity maps 0 to 100%
to the bounded 0 to 6,000 particle budget. Ground response and lightning have their own pages but
share the Weather bypass and reset. Choosing Rain or Snow on untouched ground starts a matching ground
response. Clear stops particles only. The first time Weather is switched on, it starts **clear** with
dry ground and no lightning. Enabling the Environment pass never brings up
clouds. They default off and come on with the Sky card's switch. **Wind speed** pushes the
precipitation and also sets Foliage wind (0 to 500 maps to 0 to 200), so it has a visible effect on
Clear.

Camera following, shelter collision, field size and particle detail are automatic defaults. Shelter
uses prepared geometry. Weather evaluates absolute demo time and a seed. That time is the demo tick
with its interpolated fraction ([the fractional clock](filmmaker_demo_scrubbing.md)), so precipitation
moves every frame at any demo timescale, in step with the players. Whole-tick time held it still
between ticks, and slow motion stepped about five times a second (issue 135). Local practice uses game
time. This is not a physical precipitation or snow-accumulation simulation.

## Assets

**Assets** places real CS2 game models (trees, plants, crates, cars, signs …) as engine entities. CS2
itself draws, lights and shadows them, so they need no Environment pass. Code:
[`Filmmaker/Props`](../../AfxHookSource2/Filmmaker/Props/Props.cpp). The **Meshes** filter lists your
own OBJ files instead (see Your meshes). **Effects** lists CS2's world particle systems (fire, embers,
water streams and drips, steam). Weapon, impact, blood, UI and screen effects are left out
(`IsPlaceableEffect`). Of the rest, only effects that play on their own at one point are listed.
[`audit-particle-effects.py`](../../Automation/tools/audit-particle-effects.py) decompiles CS2's
particles and generates `Props/PropsEffectProfiles.h`. That header hides child-only parts, one-shot
bursts, and effects that need a parent, a model or a beam end. It also gives every control point an
effect reads as a fixed number. All other points sit at the effect. Re-run the script after a CS2
update that changes particles.

An effect is placed through CS2's game particle manager at its position, not as an entity.
An effect has no model, so it is picked by a 32×32×48 handle box at its base. The preview box plays it in CS2's own
`ParticleScenePanel` (`CCSGO_Particle3dPanel`).

- The catalog is read from the game's own VPK index (`game/csgo/pak01_dir.vpk`). It holds about
  12,100 placeable models once player, weapon, UI and gib models are left out. Search matches whole
  words by prefix, so "tree" never finds a "street" sign. **Show** narrows the list to Foliage,
  Props, or models made for the current map (**This map**). A searched word also finds the names CS2
  files that kind of thing under. "wood" finds crates, pallets and planks, and "grass" finds weeds and
  reeds. A map name finds that map's props ("dust2" finds `de_dust` folders)
  ([`PropsMath.h`](../../AfxHookSource2/Filmmaker/Props/PropsMath.h), tested by
  `props-math-tests.cpp`). Each result shows where it comes from: the map it was made for, read from
  its folders ("Cache  ·  foliage"). Shared props such as `models/generic` show only their folder. So
  "barrel nuke" finds Nuke's barrels.
- On a workshop map, the models in its workshop package join the catalog, first and under **This
  map**. The package is found by the map it nests (`maps/<map>.vpk`), because workshop folders are
  named by item ID. These models can only be placed while that map is loaded.
- Clicking a result shows it turning in the preview box above the results, playing or paused
  ([`PropPreview.cpp`](../../AfxHookSource2/Filmmaker/Props/PropPreview.cpp)). Native preview scenes
  only spawn a model while the demo plays. The box is therefore a `MapPlayerPreviewPanel` wearing a
  marker agent, which its pooled preview pawn takes even when paused. The hook swaps that pawn to the
  asset (client-side `SetModel`). It scales the pawn so the asset's bounds fill the frame. The bounds
  are measured on a throwaway `prop_dynamic`, because the pawn's own bounds are the player hull. The
  hook arms the same paused-render gates as the death-animation preview. It turns the model by writing
  its scene-node transform in the late frame. The box keeps one panel and swaps the model on its pawn.- **Place in scene** switches to the free cam and hangs the model under the mouse. The model follows
  the surface under the cursor (the map's collision mesh) over the view until a click drops it. Hold
  the right button to look somewhere else, then let go to carry on placing. **Esc** cancels. Each model is
  precached first, so it never draws as the ERROR placeholder.
- With Scene Objects open, a click on a prop in the view selects it, and a click on empty ground
  deselects it. On the ENVIRONMENT tab the left button is the editor's and never moves the camera:
  hold the right button in the view to look around (`UiClick::RightPressLooks` in
  [`UiClickMath.h`](../../AfxHookSource2/Filmmaker/Movie/UiClickMath.h)). The hold switches to
  the fly cam (HLAE's camera mode) at the current view first, because CS2's own demo cameras
  mostly never started their drag-look from a press under the Config panel. The look works with
  the TIMELINE dock up too: docked in Config, the dock claims the mouse only while the shared
  timeline cursor flag is on, which the hold turns off. A quick right-click instead (let go within
  0.2 s, moved under 4 px) selects what is under the pointer and opens its menu, the Outliner's
  (Duplicate, Edit position, Hide, Lock, Remove; a camera adds Go to camera and Look through). The
  Outliner rows keep their own right-click menu with the dock up as well. After Config closes, a
  click on the fly cam the look left on is a hold-to-look like CS2's own: the mouse comes back when
  the button does, where it used to stay captured until Esc. A selected prop is
  edited in the view. Drag an arrow to move it along that axis, or drag one of three rings to turn it.
  The blue ring turns it about the vertical (yaw). The green ring tilts it about its side (pitch). The
  red ring rolls it about its forward axis. Each ring changes exactly one angle. **Esc** during a drag
  puts it back. Hold **Shift** to snap. An arrow keeps the prop resting on the floor under it (the up
  arrow drops it onto the floor below), and a ring turns in 15° steps. Placing always rests the
  model's lowest point on the surface, not its origin. The cursor ray, ring angles, bounds and picking
  are engine-free ([`PropEditMath.h`](../../AfxHookSource2/Filmmaker/Props/PropEditMath.h), tested by
  `prop-edit-math-tests.cpp`). The movement hint waits for a confirmed backend grab. Clicking empty
  preview space or a locked asset does not report movement. Quick clicks still release queued grabs.
- Placed props belong to the map and are saved per map in
  `%APPDATA%\HLAE\source_mvm\props\<map>.json`. They respawn when that map loads. A prop is a
  client-only entity (index 16384+), which survives demo seeks.
- Selected in the **Outliner**, a prop can also be resized, dropped to the floor, moved to the
  crosshair or stood upright (tilt and roll back to 0). An edit replaces the entity (remove, then
  spawn). This is the one path the engine fully supports for a client entity. A drag does so at most
  every 50 ms.
- **Paint with a brush** scatters the previewed model, or a mix: click one model, Ctrl-click more
  (the first one clicked stays in the mix), then **Paint**. A mix of up to 16 models goes to the
  game in several console lines (`paint start`, then `paint add`), because one line holds only a
  few model paths. Each spray deals the mix like a shuffled deck (`PaintBagNext`), so every model
  appears. Hold the mouse over the view, and copies land inside the circle on the ground, one spray
  every 30 ms. Dots inside the circle show exactly where the next copies land. **Spacing** keeps the
  brush's copies apart. **Density** (5 to 400%) caps how full the circle gets (`PaintBudget`);
  past 100% copies sit closer than the spacing (`PaintSpacing`), so painting over a full patch
  thickens it. Only copies of the brush's own models count toward spacing and density, so a
  different model paints over an earlier patch. **Size**, **Size variation** and **Turn variation**
  randomise each copy. **Erase** (or holding **Shift**) removes this brush's copies under the circle.
  Every copy is filed under one group, "Painted <model>", so the outliner can hide, lock or remove
  the whole patch. A scene holds at most 2,000 props. The scatter maths is engine-free (`Scatter`,
  `PaintScale`, `PaintYaw` in `PropEditMath.h`).
- Placement, the brush and its preview dots find the ground with the shared map-collision ray grid
  ([`MapCollisionGrid.h`](../../AfxHookSource2/Filmmaker/Environment/MapCollisionGrid.h)), the same
  one ragdolls use. It builds on a worker once per map. A ray that walked every map triangle cost
  about 10 ms, which made each spray hitch. The engine's spawn functions are looked up when the map
  loads, not at the first spray.
- **Ctrl+Z** (Config open) takes back the last placement, paste, duplicate, whole paint stroke or
  removal (`props undo`; 64 steps, per map). Removing a large group sends it in batches of 40 ids,
  because one console command holds about 60 arguments.
- Foliage sways (CS2's foliage shader animates leaves and grass) while the demo plays, and holds
  still while it is paused, like the rest of the world. Most maps have no wind, so **Weather →
  Foliage wind** (`props wind <speed>`) adds a client-side `env_wind_clientside` for the whole
  level. 0 removes it. It lasts for the session and resets with the environment.
- Known gaps: foliage leaves and grass cards do not draw in the preview box (its separate scene
  does not run the foliage vertex-animation pass), and a placed tree far from the camera (about
  1,500 units) loses its leaves while its trunk stays. CS2 has no fire models. Fire is a particle
  effect.

The client-side spawn replays `CEntitySpawner<T>::Spawn`
([`ClientEntitySpawn.cpp`](../../AfxHookSource2/Filmmaker/Props/ClientEntitySpawn.cpp)). Every
engine address and offset it needs is found by signature, so a CS2 update fails it closed
(`props status` reports the spawner unavailable) instead of crashing.

```text
mirv_filmmaker props search "palm" foliage
mirv_filmmaker props add models/anubis/foliage/palm_tree_01.vmdl
mirv_filmmaker props set 1 yaw 90
mirv_filmmaker props set 1 scale 1.5
mirv_filmmaker props here 1
mirv_filmmaker props place models/anubis/foliage/palm_tree_01.vmdl
mirv_filmmaker props hover 0.5 0.6
mirv_filmmaker props drop
mirv_filmmaker props grab 1 turn 0.55 0.6
mirv_filmmaker props grab 1 tilt 0.55 0.6
mirv_filmmaker props drag 0.6 0.6
mirv_filmmaker props release
mirv_filmmaker props floor 1
mirv_filmmaker props set 1 group "Palm row"
mirv_filmmaker props set 1 render 0
mirv_filmmaker props wind 60
mirv_filmmaker props paste 1
mirv_filmmaker props paint start models/de_cache_s2/foliage/ch2_grass_detail_large_001.vmdl
mirv_filmmaker props paint set radius 200
mirv_filmmaker props paint stroke 0.5 0.7
mirv_filmmaker props paint stop
mirv_filmmaker props status
```

Placed models come from Assets.

## Outliner

**Outliner** is the first page. It lists every placed thing (CS2 assets, your meshes, lights) in one
tree of groups, Blender-style. Code: [`ConfigHudOutlinerJs.h`](../../AfxHookSource2/Filmmaker/Panorama/ConfigHudOutlinerJs.h).

- Each row has an **eye** (shown in the view), a **camera** (kept in renders: off leaves it in the
  live view but out of what is recorded) and a **lock** (cannot be picked or dragged in the view).
  On a group row they act on every member.
- A group is a name stored on each member (`props set <id> group`, `environment object <id> group`),
  so a group exists while something is filed under it. **New group from selection** files the
  selected object under a new group. The selected object's card picks any existing group or None.
- Selecting a group shows its card: rename it (type, Enter), **Isolate** (hide everything else and
  remember what was shown, so **Show all** puts it back exactly) or **Ungroup** (keeps the objects).
- The card for an item shows its name and where it comes from, then only what is adjusted there:
  for a CS2 asset its size, its turn, tilt and roll, and the moves that are awkward to drag. For a
  mesh, its full editor (position, size, material, textures). For a light, a link to the Lights page.
  Every item also shows its group.
- Right-click a row for its menu: Duplicate, Copy, Paste, the asset moves,
  Hide/Show, Lock/Unlock and Remove. On a group row, Isolate and Ungroup. While Scene Objects is open,
  **Delete** removes the selected item, **Ctrl+D** duplicates it, **Ctrl+C** copies it and
  **Ctrl+V** pastes the copy. A CS2 asset hangs under the mouse like a placement (same size, turn and
  group) and is selected once dropped. A light or mesh pastes as a duplicate. MovieMode hands these
  keys to the panel, which owns the selection, but never while a field is typing. With the TIMELINE
  dock up, Delete is the dock's only while the pointer is over the dock (its keys); anywhere else it
  removes the selected object.
- **Shift-click** selects a range of rows, and **Ctrl-click** adds or drops one. The card then offers
  **New group from these** and **Remove all**, and Delete removes them all. A group's right-click
  menu has **Delete group**, and Delete with a group selected removes all of it. Both ask first.
- **RESET ENVIRONMENT** also removes the scene objects (CS2 assets, lights, meshes) and the foliage
  wind. It asks first whenever there are any.
- The filter narrows the tree to CS2 assets, Lights or Your meshes.

Recording reads the camera flag. Environment objects with it off are left out of the captured scene
while `AfxStreams` records. CS2 assets with it off are removed from the world for the recording
and put back after.

## Your meshes

**Assets → Meshes** lists the OBJ, GLB and glTF files in the asset folder's `meshes` subfolder. When two
files share a name, the OBJ wins. **Open folder** opens the folder, and **Refresh** rescans it. A row
places that mesh 160 units ahead of the camera. Edit a placed mesh in the Outliner: position with the
world-axis gizmo, rotation, size, colour, material, hide, duplicate or remove.

Use a filename containing letters, digits, spaces, hyphens or underscores. Import supports positions,
UVs, authored normals, positive and negative indices, and slash-separated face tokens. Export
triangulated meshes for predictable results. Missing normals are generated from faces. Malformed
referenced UVs or normals are rejected. OBJ material libraries and skeletal animation are not
imported. Assign one material per placed prop through the material inspector. Meshes are centered and
normalized so their longest side is 2 units before object scaling. The limits are 16 MB, 100,000
source vertices and 100,000 triangles per mesh, with 64 placed props. Props are visual set dressing.
They do not alter recorded movement, collision or physics.

**GLB / glTF** ([`EnvironmentGltf.cpp`](../../AfxHookSource2/Filmmaker/Environment/EnvironmentGltf.cpp),
maths in [`EnvironmentGltfMath.h`](../../AfxHookSource2/Filmmaker/Environment/EnvironmentGltfMath.h), tested by
`environment-gltf-math-tests.cpp`. Run that executable with file paths to inspect real exports):

- glTF 2.0 binary `.glb`, or `.gltf` with base64 buffers or files beside it (plain relative names
  only, never outside the meshes folder). Triangle primitives are read. Points, lines, sparse accessors
  and morph targets are not. Up to 128 MB per file, 100,000 triangles, 1,024 bones, 64 clips.
- Each primitive is a **part** drawn with its own material: base colour (factor and texture),
  normal texture, roughness and metal factors. Embedded PNG and JPEG textures load automatically
  (up to 4096 square, 256 MB per mesh). The metal-roughness texture is not used. Its factors are.
  The object's colour tints every part, and a material set on the object replaces them all.
- glTF is Y up. The mesh is turned to the world's Z up ((x, y, z) to (x, -z, y)) and normalized like
  an OBJ from its rest pose: centred, longest side 2 units.
- **Animations**: node and skeletal animation (translation, rotation, scale; linear, step, and the
  key values of cubic splines). The Outliner's **ANIMATION** section picks the clip (or the rest
  pose) and its **Speed** (−4 to 4, negative plays backwards). A clip loops on the demo clock, so a
  render or a scrub poses it the same way. Each animated object is skinned on the CPU per frame.
- Tested on Khronos CesiumMan (skinned walk), DamagedHelmet, CS2 agent exports and a CoD export.

## Attaching, keying and camera objects

**+ Camera** adds a camera object: a viewpoint with a field of view that can be looked through. The
card of every light, mesh and camera has a **RIG** section. Attach the object to a player's bone, a
held or dropped weapon, a thrown grenade or another object, edit its offset there, place it at the
free cam, and turn on **Animate** to key its pose on the TIMELINE dock. Pose keys hold until the
next key by default. Attachments and keys are saved per demo; the object stays in the map's scene.
While an object is rigged, its arrows and turn sliders move it through the rig. The full contract is
[scene-object-rigs.md](scene-object-rigs.md).

## Lights

Lights are added from the card's **+ Add** menu and edited from the Outliner. With Environment
lighting on (the Lighting card), added lights
join the native material lighting, shadow atlas, bounce and smoke-lighting paths. With it off,
they contribute through the environment composite over CS2's lighting. Sky and Lighting are separate
switches; changing the sky does not remove a placed light. **Change in Lighting** opens the Lighting card.

**Point**, **Spot** and **Area** add a light ahead of the rendered camera. Select a light in the list
to adjust its position, direction, color, intensity, range and spot cone. Up to 16 lights are
supported. Their intensity remains independent of **Map lights** brightness. Area lights use four
emitter samples. Native materials use their own normals and reflectance, so brightness is not a
pixel-for-pixel match to the composite. Studio cues work in both paths; native lighting does not yet
apply the composite's pattern and profile shaping.

Each light has an optional **Dynamic shadows** switch. Shadows use world geometry and supported
engine-submitted prop geometry. Opaque placed CS2 assets participate in both lighting paths; moving
or hiding an asset updates its shadow. Many assets share the map's static-material shaders, so live
capture of those shaders keeps only triangles inside a placed asset's box. The extracted map casts the
rest, and with no asset placed nothing from those shaders is captured. Up to 64 assets get their own
box; beyond that, the last box grows to cover the rest.

Native lighting reads the last completed opaque scene because its shadow pass precedes the current
frame's color draws, so opaque movement can lag by one frame. Alpha-tested, cable and GPU-indirect
props are not yet retained that way: in the native path they shadow only when drawn before that pass.
Off-camera geometry that the engine does not submit is still outside this capture's coverage.

Four emitter samples share the moving local-light shadow budget with map and VFX lights. Placed lights
take priority over both, so gunfire flashes cannot take a placed light's shadow; lights outside that
budget remain lit without dynamic shadows. Native lighting shadows every placed sample as a full cube,
so an area light occupies all four slots. The first four enabled lights illuminate the added fog
volume. Imported props can use the material inspector and receive fog, grading and scene lights.

## Presets and reset

**Scene Objects → Scenes** saves the whole environment: sky selection and tint, clouds, sun, exposure,
shadows, world and smoke colors, weather, fog, grading, added lights and placed props. Click the name
field, type a name, then press Enter or **Save**. Load a preset from the list. The status line marks
changes relative to the selected named preset. Named presets remain explicit saves.

Accepted environment edits also autosave per map after 750 ms of quiet. Committed edits flush on
orderly exit. Reloading a map restores its last valid autosave once three things are true: map
geometry is ready, a world camera has rendered, and at least 30 successful presents span a 750 ms
buffer. A render gap longer than 500 ms restarts that buffer. The native scene remains visible while
waiting, including on same-map reloads. Edits made during the wait cancel restoration.

That restore keeps every saved value and leaves sky, lighting, fog and weather switched off, so
reopening a demo does not start them again. Each pass that was on is kept in the scene's `held` list,
and switching its card on puts the held value back. Switching a card off yourself holds its value the
same way (`environment pass-off`), so it also comes back after a restart. Scene objects stay as placed. A preset loaded explicitly in that session takes precedence and applies the lighting
switch it was saved with. Resets are intentional saved edits. Active gizmo drags are excluded until
committed.

Failed writes retain their latest committed snapshots across map switches and retry without mixing
maps. Autosaves live in the sibling `autosaves` directory. A corrupt primary falls back to
`.json.bak`, and subsequent writes preserve the valid recovery copy. The restore crash marker is armed
before assets or materials are applied, and it settles after 600 completed presents. An orderly map
reload clears the current process's marker. A marker left by a crashed process holds its saved scene
aside instead of repeating the restore.

Scene snapshots for autosave, named presets, Undo and preference controls exclude temporary
Disco/Wes lighting. They capture the user's original tint, strength, mode and sun yaw while those
eggs own the live lights. Other edits still save normally. The renderer captures live lighting
separately, so the temporary effects remain visible without becoming part of a saved scene.

Presets live in `%APPDATA%\HLAE\source_mvm\environment\presets`. OBJ files live in the sibling `meshes`
folder. Saves use a temporary file, atomic replacement and a `.bak` of the previous version.
Loading validates all fields and resolves meshes before replacing live state. Missing or invalid
assets leave the current scene intact. Meshes are referenced by name, so include their OBJ files
when sharing a preset. Presets contain data, never console scripts or engine addresses.

Placed objects, fixed weather, local fog, local surface tests and puddles bind the scene to its map.
**Keep between demos** (the ENVIRONMENT footer shares the EFFECTS tab's switch) decides what a new demo
starts with:

- On: a map with its own autosave restores it. A map without one takes the previous demo's look, with
  its passes held off. What belonged to the old map (objects, placed fog and surface volumes, puddles,
  the storm cell, the bounce volume and a fixed weather centre) stays behind (`UnbindSceneFromMap`).
- Off: the demo starts from the defaults and the map's autosave is not applied. The file is left as it
  is until you edit the scene on that map.

Undo history always ends at a map change. Loading a scene on a different map is refused with the required map's name. Weather-only
camera-following presets are portable across maps.
The explicit console operation `environment bind` rebinds a scene to the currently loaded map.

**Reset** in a card header restores that card's own settings and nothing else. The backend resets by
page, and its pages do not line up with these cards. Added fog and the finishing grade are one backend
page, and a page reset of lighting would also delete every added light. So each card restores its own
tuning and switches its passes off. A sky reset also switches clouds off. Lighting resets release AO,
shadows and bounce switches. Resetting the last active pass releases the scene and renderer masters.
Unrelated active passes and placed objects remain in place. A reset never starts a disabled
environment.

**Reset environment** releases every environment override and removes all added props, lights,
and weather. It restores the live map's authored material, tint, brightness, cloud and sun parameters,
and engine-controlled exposure. It never uses your saved settings or a saved preset as the baseline.
Sky snapshots belong to each entity (including its serial), and material snapshots belong to each
material identity and map generation. Different sky entities may have different brightness even
within one map. New maps acquire fresh originals before any override writes.

**Undo** restores up to 32 prior environment commands, including reset, load and remove, within the
current map. Direct edits to the sun and sky controls are included when a reset takes its undo
snapshot. They do not each add an undo entry. Camera, VFX, Ragdolls, camera keys and saved assets
are outside environment reset and presets. Explicit resets update saved lighting preferences too.

**What each reset covers.** A card's **Reset** writes that card's own settings back to their
defaults and nothing else. The tab's **RESET ENVIRONMENT** runs `environment reset environment`:
everything on the Environment tab, with the world look a scene also stores (the skybox, tints,
engine sun and exposure that belong to the Effects tab) left as it is; that tab has its own RESET.
`environment reset all` still resets the world look too, as loading a preset replaces it. A reset
scene is exactly a new scene: every switch that is a pass goes off, and cast shadows, ambient
occlusion, sky occlusion and light shafts keep their default of on.
Presets use **version 14**. Versions 1 to 13 and unknown future versions are rejected with a clear
incompatibility message before replacing live state.

Starting looks (Storm, Winter, Sunset, Night) initialize the added climate and weather.
Named scenes store the result. They never become map-reset defaults.

## Rendering and diagnostics

The pass runs before clean LDR movie capture and before Panorama and scaled-preview composition. While
a sniper scope is up, CS2 tonemaps into a scratch target and its `legacy_sniper_scope` pass draws the
lens mask, vignette, reticle and distortion from it into the back buffer. The pass then composites into
that scratch target just before the scope pass, so the scope covers fog and clouds the same way it
covers the world. Fog never lands on the black scope surround. It handles single-sample and MSAA movie surfaces and depth, with a private prop depth buffer. The captured
world viewport's depth range is normalized before reconstruction. CS2 reserves another part of the
depth buffer for its sky, so assuming a full 0 to 1 world range is incorrect. Fog is evaluated per
color and depth sample before MSAA resolution, which avoids bright silhouette fringes. Where the
colour is already resolved and only the depth keeps its samples, a pixel along a wire or a leaf seen
against the sky is two layers (`EdgeSkyShare`, `EdgeSkyColor`): the share of its samples that
missed the wire is the sky as it was drawn, read from the neighbouring all-sky pixels, and that share
gets the sky's clouds, fog, far rain and, when the Environment sky is laid in the atmosphere pass, the
Environment sky itself. Shaded for its nearest sample alone, it kept a bright outline of the plain
map sky around every thin thing in a dark rain sky. The completed
engine world-to-clip matrix keeps placement aligned with camera overrides, roll, third-person views and
Smooth View. Device resources belong to the hook and are released on device teardown. The immediate
D3D11 state is restored after each submission.

The EXR path applies environment additions to a private resolved HDR copy before writing. The engine
HDR texture is never modified. Export quality has its own ray budget and GPU surfaces. HDR values
above 1.0 are retained. The added layer does not enter the engine's pre-tonemap lighting pipeline. The
game's bloom and tonemapping do not process it, and an EXR needs its own display transform or grading.
Display and EXR are not expected to look identical without grading. The pass does not provide separate
prop, weather or light export layers. Large weather fields, large meshes and many lights increase GPU
cost. The status label reports unavailable resources instead of claiming a draw. The feature does not
establish full launcher and config isolation.
Commands are available through `mirv_filmmaker environment help`. Useful examples:

```text
mirv_filmmaker environment look winter
mirv_filmmaker environment climate snow 0.7
mirv_filmmaker environment climate fogDensity 0.25
mirv_filmmaker environment surface-center
mirv_filmmaker environment weather rain
mirv_filmmaker environment add spot
mirv_filmmaker environment object 2 intensity 3
mirv_filmmaker environment save "Rain at A"
mirv_filmmaker environment reset
mirv_filmmaker environment undo
mirv_filmmaker environment status
```

Use `mvm_debug start` before reproducing a problem. `environment.edit`, `environment.error`,
`environment.shader`, `environment.baked`, `environment.surfaces`, `environment.occlusion`,
`environment.shadow`, `environment.gi`, `environment.gpu` and `environment.frame` report input changes,
asset and compiler failures, surface pointers, formats and sample counts, camera matrices and draw state.
Shadow probes report map triangle counts, updated faces, active shadow lights and camera-independent caching.

Implementation: [`Environment`](../../AfxHookSource2/Filmmaker/Environment/EnvironmentScene.h),
[`renderer`](../../AfxHookSource2/Filmmaker/Environment/EnvironmentRender.cpp),
[`Panorama tab`](../../AfxHookSource2/Filmmaker/Panorama/ConfigHudEnvironmentTabJs.h).
Native `EnvironmentMathTests` covers transforms, time reconstruction, preset validation and OBJ
parsing. `EnvironmentRenderTests` uses the real shaders and WARP to check pixels, occlusion,
lights, fog, mixed MSAA coverage, partitioned viewport depth and D3D state restoration.
It also checks cloud seek determinism and occlusion, snow slope response, puddle height rejection,
neutral reset pixels and HDR radiance preservation. World-shadow probes sample fixed receivers
behind map geometry for point lights, spotlights and sun, verify camera independence, light
movement and disable behavior, and reject stale map shadows. The math suite checks cube-face
selection, full-map sun bounds and malformed cache files. `map-occlusion-tests.py` validates
Source/glTF coordinate conversion and exclusion of invisible clip geometry.
`environment-ui-tests.mjs` exercises the tab's state mirroring, disabled-control command guards,
independent relighting enable, object commands and preset keyboard input.
