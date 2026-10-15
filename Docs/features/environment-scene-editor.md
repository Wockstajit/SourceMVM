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
| Sky | Sun direction/elevation/power, haze, stars; Clouds: enable, amount, height, wind |
| Lighting | Map lights, ambient light, bounce intensity, 1 to 3 bounce steps per frame, shadows and source sizes |
| Fog | Density, Start distance, Height, Variation, Color |
| Weather | Type, intensity, fall speed, wind speed/direction; Ground: wetness, puddles, snow; Lightning: enable, intensity, interval |
| Scene Objects | Outliner: every placed thing in groups, with show/hide, in-renders and lock; Assets: CS2 models and your OBJ meshes; Lights: lit-by, add point/spot/area, the light editor; Presets: save/load |

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

Fog has one density (0 to 6) and color. Start distance leaves the near region clear. Height controls
vertical falloff. Variation is three octaves of drifting, contrast-stretched noise at map scale. It
opens clear pockets and thick banks, and it raises and lowers the fog top. It does not add a fine
ripple that a long ray averages away. The lit march and far continuation integrate disjoint depth
intervals, so the far blend does not apply a second fog layer over the near volume. The shared sun
and first four added lights illuminate the near fog. Depth and per-sample MSAA coverage stop it at
visible geometry. The 3D skybox (distant town, hills) takes the medium out to a fixed 9000 units.
Its reserved depth band has no real distance, so it sinks into fog with the sky behind it. The march
jitter is interleaved gradient noise. A 5 × 5 depth-aware blur at volume resolution removes its grain
from light shafts without crossing silhouettes.

Fog does not depend on Lighting. Fog alone renders no sun shadow maps and runs none of the
dynamic-lighting, bounce or native relighting passes. Its sun term is unshadowed until the Sky or
Lighting card puts a sun up. Then the fog gets shadowed light shafts from the same sun cascades.
The sun cascades render only for a pass that samples them: dynamic lighting (including the native
relight path it hands off to), world bounce, clouds, snow and wet coatings, placed props, shafted
fog under the procedural sky, and the cascade debug view.

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
noise-mask holes on eligible faces. While snow falls on a prepared map, the ground cover uses
the flakes' shelter map, which for snow looks straight down. Open ground and alleys are covered, and
hallways and interiors stay clear. Under an arch, eave or overhang, the cover does not end on a line.
It thins out with the share of open sky around each spot, over a fringe of about 70 % of the cover's
height (56 to 140 units). A tall arch lets snow a long way in, and a low eave lets in only a little.
The edge is uneven and noise-perturbed. The wind setting does not move the cover. Broad drift
patches collect more snow than others, with lumpy relief shading where it lies thick.

Players get no snow and no world wetness. The coating masks are keyed to map positions, which slide
across a moving body. A pixel that belongs to a player, a bot, a held weapon or a world weapon is
therefore left as the engine drew it. The shader tells these apart from the map with an exact
per-frame mask. The native skinned player and world-weapon triangles that the shadow casters already
capture are drawn from the camera into a screen-sized depth map. A point counts as a person when that
map holds a surface at its own position, corpses and held guns included. Until the first capture, a
body-sized capsule around each living pawn serves as the fallback. The capsule ignores the floor and
any wall the player leans on. First-person hands and weapon are skipped by the snow only.

The capture builds a stream-output geometry shader per character or weapon vertex-shader variant from
its reflected output signature, including signatures that pack two semantics into one register. Its
cache holds 4096 entries. A variant without one is missing from the mask and shows snow on bare forearms.

The coating filter blends shallow neighboring facets while rejecting depth discontinuities and hard
corners. Snow keeps the map image's broad shading instead of replacing it with an unshadowed flat
color. Enabled relighting, added lights and world bounce also affect it. Fine grain and sparkle fade
below pixel size. Wetness darkens the visible color and adds a softer filtered sun highlight.

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

An enabled Environment scene owns dynamic material lighting. **Sun brightness** in SKY controls
surface sunlight and sky radiance together. **Map lights** controls authored map fixtures.
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

## World-space precipitation collision

Shelter collision blocks rain, snow and ash using prepared map geometry and added props.
It is enabled by default and loads supplied map data asynchronously. Until geometry is ready,
precipitation still falls but passes through roofs. Visible depth still handles ordinary camera
occlusion. Missing data must be exported in development and supplied with the project.

Shelter is graded, not a cut. Rain does not arrive along one exact line, so four more 2048 × 2048
maps look along gust directions tilted about the mean. Two run along the wind (20°, up to 38° as the
wind slants the rain), and two run across it (13° up to 22°). A surface's exposure is the weighted
share of directions with open sky (mean 36 %, each gust 16 %). A doorway or arch facing the rain is
therefore partly exposed for a short way in, and deep interiors stay dry. Wetness, splashes and,
while it rains, puddles all follow that exposure. Puddles form on open ground and the wetted fringe of
openings, not across interior floors. Each rain particle is roof-tested along one of those five
directions (in the same proportions), so a few drops reach just inside an opening. Every drop still
streaks along the shared fall direction. `mirv_filmmaker environment rainview on|off` paints the
exposure (grey open, dark blue sheltered). The gust maps are static, like the main map, and are
redrawn only when geometry or wind changes.

Snow and ash are sheltered straight overhead instead, with no gust maps. What lies on the ground is
hours of fall from every direction, so it depends on open sky, not on the current wind. The flakes themselves still drift and slant
with the wind.

Rain streaks are motion blurred. Each streak is one drop smeared over its length, so its opacity is
min(1, 40 × drop width / streak length). Long, fast streaks are faint and soft. Streak length does not
change the rain's total coverage. Drop size spreads below the size setting, which is the largest drop.
Most drops are smaller, down to a quarter of it. Size sets streak width and length. Rain takes the
map's light at the drop times 2, so it reads as water catching light. No drop or splash is drawn more
than 40 to 50 % opaque, so streaks near the lens never become solid white lines.

A 2048 × 2048 depth map faces the incoming precipitation direction, including wind.
It fits fixed world bounds and updates when geometry or wind direction changes. Turning the camera,
moving a following particle field, or changing the sun does not invalidate it. Camera-hidden props
can form rain shelters. This uses the existing world-depth infrastructure and does not require
procedural sky, sunlight, GI or enabled dynamic light shadows.

The collision mask costs 16 MiB and shares uploaded map and prop geometry with world shadows. It is a
finite-resolution approximation to prepared collision surfaces, not per-particle physics. Thin
openings, alpha-cutout foliage, moving native actors and visual details missing from that mesh are
not reproduced exactly. Embers, motes and smoke remain local effects and do not use the rain mask.

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
normal/refraction layer, evaluated only where the precipitation shelter map proves exposure.
Prepare map geometry for roof exclusion. With no shelter geometry, falling rain still renders but
surface impacts fail closed. Impact events use absolute demo time. Pause freezes them, and seeking
reconstructs the same pattern. Their finite lifetime avoids an accumulated simulation cache.

Rain, snow and ash have one density everywhere. Near the camera, they form a single world-coordinate
particle lattice with one cell size (0.028 times the weather radius). A particle's position depends
only on its cell and absolute demo time. The camera chooses which cells are drawn but never moves
one. The particles fill a window about 875 × 425 units around the camera (at the default radius).
Beyond that window, the same density continues as a per-pixel veil that integrates the particles'
expected coverage along each view ray. Particles fade out over the window edge as the veil fades in,
so distance never changes how thick precipitation is.

The veil counts only ray stretches that are open to the sky in the shelter map and inside the
precipitation region. Looking out of a doorway therefore shows the same fall as standing outside, and
none indoors. With map geometry, the region is the map's collision bounds, from the floor to half the
weather height above the highest collision. Precipitation therefore covers the whole map and stops at
its edges. Without map geometry, the veil reaches ten weather radii. The veil's clumping noise is
world-anchored and falls with the particles.

Count sets the cell size (every cell holds a particle), so a light fall draws proportionally fewer
particles over the same window. Changing Count re-seeds the lattice. Each particle tests the roof and
takes its light once, in the vertex shader, rather than per pixel. Rain drop sizes spread below the
size setting (see the motion blur notes below). Ash flakes are thin, translucent torn pieces that flip
as they fall and take the map's lighting.

Drops use wind-aligned streaks, radial band transitions and conserved subpixel opacity. There is no
camera-centered curtain or special upward-view branch. Scene-color radiance estimates inherit map
brightness and color. Authored lights and storm light add shadowed scattering. Fog attenuates distant
drops. Drops fade in over roughly 14 to 56 units. The per-drop opacity cap keeps near streaks from
becoming solid bars. Rain also wets exposed ground by itself. Wet surfaces mirror the scene through a
short screen-space trace (or the procedural sky when that pass is on), so wetness reads as a water
film. Ground ripples use one world-anchored impact field at every distance. Splash geometry (a ring
plus a crown of droplets) fires in at most about a third of the impact cells per cycle, drawn down to
about 24 units from the lens. The estimate does not use CS2's reflection probes or material buffers,
so it cannot recover offscreen map lights. The fixed preview and export quality policy applies.

Smoke coverage uses only the current smoke window's `mboit_mixed_combine` zeroth moment. The handoff
is cleared before each window and when the current entity snapshot has no smoke. Projectile lifetime
filters exclude future and expired detonation records after seeks. Ray entry preserves rain in front
of smoke. Optical distance attenuates drops within it, including a camera inside the cloud. The
engine's current absorbance supplies density. A cached sphere does not supply opacity.

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
uses prepared geometry. Weather evaluates absolute demo time and a seed. Local practice uses game
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
  the surface under the cursor (the map's collision mesh) over the view until a click drops it. Fly
  CS2's free cam to look somewhere else and back. **Esc** cancels. Each model is
  precached first, so it never draws as the ERROR placeholder.
- With Scene Objects open, a click on a prop in the view selects it, and a click on empty ground
  deselects it. A right-click selects the prop and opens its menu (the outliner's). A selected prop is
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
- **Paint with a brush** scatters the previewed model. Hold the mouse over the view, and copies land
  inside the circle on the ground. Dots inside the circle show exactly where the next copies land.
  **Spacing** keeps one stroke's copies apart. **Density** (5 to 100%) caps how full one stroke makes
  the circle (`PaintBudget`). A new stroke over a painted patch adds another layer on top. **Size**,
  **Size variation** and **Turn variation** randomise each copy. **Erase** (or holding **Shift**)
  removes this brush's copies under the circle. Every copy is filed under one group, "Painted
  <model>", so the outliner can hide, lock or remove the whole patch. A scene holds at most 2,000
  props. The scatter maths is engine-free (`Scatter`, `PaintScale`, `PaintYaw` in
  `PropEditMath.h`).
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
- Right-click a row (or a prop in the view) for its menu: Duplicate, Copy, Paste, the asset moves,
  Hide/Show, Lock/Unlock and Remove. On a group row, Isolate and Ungroup. While Scene Objects is open,
  **Delete** removes the selected item, **Ctrl+D** duplicates it, **Ctrl+C** copies it and
  **Ctrl+V** pastes the copy. A CS2 asset hangs under the mouse like a placement (same size, turn and
  group) and is selected once dropped. A light or mesh pastes as a duplicate. MovieMode hands these
  keys to the panel, which owns the selection, but never while a field is typing.
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

## Lights

**Lights** opens with **Lit by**. With Environment lighting on (the Lighting card), added lights
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
or hiding an asset updates its shadow. Native lighting reads the last completed opaque scene because
its shadow pass precedes the current frame's color draws, so opaque movement can lag by one frame.
Off-camera geometry that the engine does not submit is still outside this capture's coverage.

Four emitter samples share the moving local-light shadow budget with map and VFX lights. Placed lights
take priority over map fixtures; lights outside that budget remain lit without dynamic shadows.
An area light can occupy all four slots. The first four enabled lights illuminate the added fog
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

That restore keeps the saved lighting values (map lights, ambient, shadows, bounce) and leaves dynamic
lighting switched off, so reopening a demo does not start it again. Turning Lighting on uses those
saved values. A preset loaded explicitly in that session takes precedence and applies the lighting
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
Changing maps hides the scene when **Keep between demos** is on, and returning to that map restores
it. With Keep off, changing maps clears added weather and objects. Undo history always ends at a map
change. Loading a scene on a different map is refused with the required map's name. Weather-only
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
color and depth sample before MSAA resolution, which avoids bright silhouette fringes. The completed
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
