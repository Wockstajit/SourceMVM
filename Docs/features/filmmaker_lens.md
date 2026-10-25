# Lens: depth of field, bokeh and lens flare

A post pass over the finished world frame. It reads the scene colour and the scene depth the engine
already produced. It runs after the environment renderer and before the Panorama UI and clean movie
capture, so it is in every recorded frame and never on the HUD. It is a module of its own
(`AfxHookSource2/Filmmaker/Lens/`): nothing depends on the environment editor being enabled.

Controls live on the CONFIG panel's **CAMERA** tab as two cards, **DEPTH OF FIELD** and **LENS FLARE**.
Everything is also a console command: `mirv_filmmaker lens help`.

Each card's **Reset** switches that lens effect off and restores its tuning.
`lens reset dof|flare|all` does the same. Look presets keep the on/off choice. The bottom
**RESET CAMERA** also disables both lens effects, along with the rest of the Camera tab.

## The parameter table

Every tunable is one row of `LENS_PARAMS` in [`LensFxMath.h`](../../AfxHookSource2/Filmmaker/Lens/LensFxMath.h)
(key, range, default, integer or not). That one list drives the enum, the `lens set <key> <value>`
command, the clamp on every write, the JSON the panel reads (`st.lens`) and the constant buffer the
shaders see. A new knob is one row. Both effects ship **off**.

```text
mirv_filmmaker lens dof on
mirv_filmmaker lens preset dof cinematic
mirv_filmmaker lens dof.focus 350
mirv_filmmaker lens dof.gamma 8
mirv_filmmaker lens focus auto
mirv_filmmaker lens iris shape polygon
mirv_filmmaker lens iris mask crescent
mirv_filmmaker lens iris file C:\shapes\star.png
mirv_filmmaker lens preset flare anamorphic
mirv_filmmaker lens flare.threshold 0.95
mirv_filmmaker lens flare.tint cool
mirv_filmmaker lens dirt C:\dirt\smudges.png
mirv_filmmaker lens state
```

## Depth of field

**Depth.** The blur follows real distance. CS2 uses several depth ranges. The world sits in the
viewport's depth band (0 to 0.95), the first-person weapon in its own band below 0.1, and the sky at 1.
World depth is decoded with the inverse world-to-clip matrix into the distance along the view axis.
Sky and the 3D skybox count as very far. The weapon is sharp unless *Blur the weapon* is on. In that
case it counts as sitting 30 units from the lens.

**Two ways to say what is sharp.**

- *Lens* (Focus row: Manual or Auto): a thin-lens model of a fixed 50 mm lens. The circle of confusion
  is `c = f² |d − s| / (N d (s − f))` in millimetres (world units are inches, 25.4 mm), turned into
  pixels through a 36 mm sensor width, so a wider aperture (lower f-number) blurs more. There is no
  focal-length setting. Blur strength is the f-stop. **Auto** eases toward whatever sits in the middle
  of the screen (a 1×1 GPU pass, nine depth taps, nearest-cluster average, wall-clock so it also pulls
  while paused). *Focus speed* (`dof.focusSpeed`, 0.25 to 30 per second, default 6) is how fast:
  each frame moves `1 - exp(-speed × dt)` of the way (`AutofocusStep`), so a pull takes the same time at
  any frame rate, and 30 jumps in one frame.
  *Sharp band* (`dof.range`, units, default 0) keeps everything that close to the focus distance
  perfectly sharp. Past the band the blur grows as it would from the focus distance, moved out by the
  band (`SharpBandDistance`), so it starts from zero at the band's edge.
- *Zones*: four absolute distances (fully blurred / sharp from / sharp until / fully blurred),
  smooth-stepped between.

Both cap at *Largest blur* (pixels on a 1080p frame, scaled with the resolution). *Show → Focus*
paints the sharp region magenta, *Blur map* paints near blur orange and far blur blue, and *Zebra*
draws diagonal stripes over whatever blurs less than half a pixel, on the finished picture.

**Smoke.** CS2 draws smokes without depth, so the depth buffer under a smoke is the wall behind it. A
smoke used to be blurred at the wall's distance, so the background's blur showed through it instead of
stopping at it. Now each pixel also reads CS2's OIT absorbance (`b0`; `exp(-b0)` of the surface still
shows, captured from `mboit_mixed_combine` in `RenderSystemDX11Hooks.cpp`) and the detonated smokes as
spheres (`Environment::CaptureSmokeVolumes`, collected for the lens even with the Environment off). The
smoke's distance is where its light comes from along the ray: the sphere entry plus the mean depth of a
uniform cloud of that absorbance (`SmokeSeenDepth`; the middle of a haze, near the front of a dense
cloud). A ray that is covered but misses every sphere uses the nearest smoke's near edge. The pixel's
blur is the smoke's over the surface's in proportion to coverage, signed by the dominant layer
(`SmokeBlendCoc`), so no sharp ring forms at a smoke's edge. Autofocus counts a dense smoke (over half
coverage) at the focus point as what it is looking at. With no absorbance or no smoke positions the
depth buffer is the whole truth, as before.

**The gather.** Scatter-as-gather bokeh. Per pixel, 48, 96 or 192 spiral taps are read, and their radius
comes from a 16-pixel tile maximum of the blur radius (dilated). An empty area therefore costs nothing,
and a big blur gets enough taps. Each tap asks whether the disc of the pixel it found reaches back to
this one. Far-field taps behind the centre pixel are held to the centre's own radius, so a blurred
background never paints over a sharp foreground. Near-field taps always reach, so a blurred foreground
bleeds over what is behind it. The gather runs in approximately linear light and reads from a 6-level
mip chain of that image.

**Which level a tap reads follows the tap spacing** (`GatherLod`: `log2(spacing × 0.9)`, spacing =
`R·sqrt(pi/taps)`). Every tap then averages the cell it stands in, so a small light is seen by some tap
wherever it sits, and the disc comes out solid. Each pixel rotates the tap spiral by white noise (`PixelNoise`,
`kGatherJitter` = 1). A fixed, unjittered pattern leaves a lattice of dots.
A denoise over similarly blurred neighbours (two rings, 16 taps) then removes the grain. The centre
pixel's own sample is read at the taps' mip level, because a full-resolution read would put a sharp bright dot
in every lifted disc. On a multisampled target, the composite keeps the engine's own per-sample edges
wherever nothing was blurred.

**The sharp weapon is not part of the blurred image.** With *Blur the weapon* off, `CocPS` writes
the first-person weapon out of the gather's image (colour and radius premultiplied by a world mask,
which has its own mip chain), and every tap is weighted by how much of it is world. A mip texel at
the gun's outline used to mix the sharp gun with the blurred world around it; next to a near-blurred
smoke that mix read as a blurred foreground sample, which always spreads, and the gun's colour
smeared out as a dark, ragged halo. The composite keeps the engine's own sharp weapon on top.

The flare is built from the sharp frame, whether or not the depth of field is on. A lifted bokeh disc is
a huge white shape, and a flare cast from it would be a wash. A light flares where it is, in focus or not.

### The bokeh gamma

**Bokeh gamma** (`dof.gamma`, 0 to 12, default 6) is the one control. In `CocPS`, at full
resolution and before the mip chain is built, every pixel's linear colour is multiplied by

```text
boost = 2^( min( 1.75 × gamma × key × ramp × isolation, 15 ) )
key       = peak channel of the display colour ^ 6      (only near-clipped pixels; peak so a red neon counts)
ramp      = saturate((|coc| − 0.5) / 2.5)               (in-focus pixels are never lifted)
isolation = saturate((0.95 − ringMean/peak) / 0.25)     (two rings of 8 taps, at 0.6·|coc| and 1.08·|coc| px; the darker mean decides)
```

Each unit of `gamma` is 1.75 stops of lift at pure white (`kLiftStopsPerGamma`). So 2 is ×11, 4 is ×128,
6 is ×1450 and 8 is ×16000. The lift stops at 15 stops (`kMaxLiftStops`), so a lifted pixel stays inside
a half float.
The isolation term keeps the control honest. It lifts a lamp on a darker surround and leaves a lit wall
or the sky alone, because their rings are as bright as they are. Raising the gamma therefore reveals
shapes instead of whiting out the frame. A tonemapped lamp is a clipped core inside its own glow.
Measured on CS2 wall lamps, the ring at the core's edge is 0.8 of the peak. So the test looks at
two radii and takes the darker one.

The functions are mirrored in `LensFxMath.h` (`HighlightKey`, `HighlightFocusRamp`, `IsolationRadius`,
`HighlightIsolation`, `HighlightBoost`, `GatherLod`), with behaviour tests. The HLSL constants are
checked by a static test.

A reference simulation of the pass (a numpy port of the gather on a synthetic frame: dark background,
3 to 12 px lamps, a lit wall and a sky band, uniform blur radius 26 to 60 px) shows:

- Lifting in `CocPS` makes every lamp size a shape and leaves a lit wall untouched.
- Disc speckle (coefficient of variation inside the disc) at footprint 0.6 is 3 to 5%. At 0.9 to 1.0 it
  is 1 to 2% for 64 and 128 taps, and 2% at 32. Wider than 1.0 only softens the edge.
- Edge width (10 to 90% of the linear plateau) at radius 26 is about 10 px (0.4 R) at 64 taps, 7 px at
  128, and 15 px at 32 (Low is a soft-edged shape). In display terms it reads crisper. Above about 4 stops
  the disc interior is far above 1.0, so the visible edge is where it falls to 1.0, close to the true iris
  boundary.
- Range needed: a 5 px white lamp at radius 26 fills 1/27 of a disc. Gamma 3 gives a faint disc, the
  default 6 a clear one, and 8 and up a bold, saturated one. 3 px lamps want 8 to 12. Presets that show a
  shape (Hearts, Stars) use 8.
- Two plain-blur checks: gamma 0 is an ordinary photographic blur, and a lit wall or the sky stays
  unlifted at any gamma.

**The iris.** This is what a point of light becomes: Circle, Polygon (3 to 12 blades, blade curve), Star,
Heart, Ring (mirror lens with a hole), or Custom (five built-in shapes, or your own black-and-white image).
Anamorphic squeeze stretches every disc into a vertical oval at constant area. *Blade curve* runs from -1
to 1: 1 is a circle, 0 hard blades, and below 0 the blades bow inward into a curved star with the same
number of points (every vertex stays on the circle, a side's middle comes in to `2 cos(pi/n) - 1` at -1).
*Rim* runs from -1 to 3: above 0 it brightens the disc edge (spherical aberration), below 0 the light
gathers in the middle and fades toward the edge (`1 + rim × u²`), a soft creamy disc.

**Orientation:** a gather draws the kernel reflected through its centre, so the kernel is built as a half
turn of the shape you see. Shapes are designed on screen. A polygon or star has a point at the top, the
heart's tip points down, and a positive rotation turns clockwise. The heart's polar curve is drawn about its
notch, so the gather moves the
heart's area centroid onto the tap centre (`kHeartCentroid`, turning with the rotation) and scales it
(`kHeartScale` 1.65, about 1.2 radii from the middle, 0.68 of the unit disc). So a heart sits on its light
and is as big as a hexagon.

*Largest blur* (`dof.maxBlur`) defaults to 40 px at 1080p. The shapes are read from the disc's outline,
so they only look like hearts and hexagons once a disc is a few dozen pixels across. Set the focus close,
for example `dof.focus 25`, `dof.fstop 1`, `dof.maxBlur 90`, to get discs 150 px wide.

**Settings.** `dof.on`, `dof.mode` (0 lens, 1 zones), `dof.auto`, `dof.focusSpeed`, `dof.focus`,
`dof.fstop`, `dof.range`, `dof.maxBlur`, `dof.nearStart`, `dof.nearEnd`, `dof.farStart`, `dof.farEnd`,
`dof.shape` (0 circle, 1 polygon, 2 star, 3 heart, 4 ring, 5 custom), `dof.blades`, `dof.rotation`,
`dof.roundness`, `dof.squeeze`, `dof.edge`, `dof.gamma`, `dof.quality` (48 / 96 / 192 taps),
`dof.weapon`, `dof.debug` (1 blur map, 2 focus, 3 zebra). That is twenty-two settings. These are fixed
in `LensFxMath.h` (and mirrored in the HLSL): the focal length (50 mm), the autofocus point (screen
centre), the weapon distance (30 units), the rim width and the Ring hole. Settings are session-local.

**Panel.** DEPTH OF FIELD has two pages, with at most eight controls on either. *Focus* holds the look,
the Manual / Auto / Zones row, focus distance (Manual) or focus speed (Auto), aperture and sharp band
(or the four zone distances), largest blur, blur the weapon (hidden in Zones to keep the page at eight),
and *Show*. *Bokeh* starts with the gamma,
then the shape and only the adjustments that mean something for it: blades for polygon and star, blade
curve for polygon, rotation for anything with an orientation, and the custom-shape picker for Custom.
Squeeze, rim and quality follow.

Looks: Cinematic, Anamorphic, Portrait, Miniature (zones), Hearts, Stars. A look resets the group first,
so applying two in a row never leaves values from the first.

## Lens flare

Screen-space, built from the hot spots of the (possibly already blurred) frame: ghosts shaped like the
aperture, a ring, an anamorphic streak, diffraction spikes, a soft glow and lens dirt, in a warm tint.
Fifteen settings, five looks.

**Green screen and the other passes.** The lens is a Beauty effect. A render job builds its Key,
Depth and Normal passes from the scene before the lens, so depth of field and the flare reach only
the Beauty plates ([Render passes](filmmaker_render_passes.md#lens-effects)). The live view of a
Key, Depth or Normal pass is built the same way. The console green screen (`render greenscreen on`)
repaints the scene itself, so there the flare is held back instead: it adds light over the whole
frame, and the chroma background would take on a halo and the subject's edges a tint that no
compositor could key out. Depth of field keeps running. `lens state` says `Flare held back on the
green screen pass` while this applies, and `mvm_debug` logs it under `lens.flare`. Add the lens to
the composited picture instead.

**The sun.** The rules below keep only small lights brighter than their surroundings and turn the
flare down on a frame full of near-white, so a sun on a bright sky never flared. The sun is found
from the sky instead: its direction is `SolarPosition` on the sky material (where sky.vfx draws its
disc, read live by `SunFx_RefreshVisibleSun` while the flare is on), else the map's
`light_environment`. It is projected onto the screen (`SunScreenUv`); a 1×1 pass (`SunPS`) checks
how much of its disc is open sky (geometry and smoke in front block it); the sun is added to the
source map past the isolation test (`SunSourceBlob`), and the budget is lifted while it shows.
*Sun* (`flare.sun`, 0 to 2, default 1) is its strength.

The flare follows these rules:

1. **A source is a luminance.** The gate is a ramp from *Source threshold* up to just above white
   (`kFlareCeil` 1.05, `FlareGate`). At about 0.95, only near-white pixels pass, and even those count
   partly. At 1.0 nothing does, and a saturated sky or a red neon is never a source. The layers saturate
   (`1 - exp(-gain x)`), which hides how much light a source gives. So on a frame whose lights are all
   near white, the gate alone acts like a switch. The threshold therefore also scales the whole flare
   (`FlareThresholdFade`: full strength up to 0.8, falling smoothly to nothing at 1.0), which makes the
   sweep from 0.75 to 1.0 a gradual fall. The gate runs on every tap of the 13-tap downsample before it
   is averaged, so a lamp a few pixels wide is not diluted below the threshold.
2. **A source must be small.** Sixteen taps of the frame's mean luminance sit on a ring 4.5% of the frame
   height out. A tap counts when it is nearly as bright as the source's own peak (`FlareRingWeight`). A
   source whose ring is mostly such taps is part of a big bright area, and it is dropped (`FlareIsolation`;
   a straight edge reads 0.5 and a corner 0.25, both dropped). A lamp on a darker surround, the sun's disc
   or a specular hot spot qualifies. A wall, the sky or a lit doorway does not. So the flare's energy cannot
   follow the area of a bright region. This test looks at the frame only, never at the threshold. That is
   what makes the threshold monotone: raising it can only remove light. Sparkles a pixel or two wide are
   dropped by a size floor (`FlareSourceFloor`).

   **A wide ring and a budget.** A second ring 13% of the frame height out drops a source that sits inside a
   big bright area, even where its immediate surround is darker (`FlareWideIsolation`: taps at 0.75 to 0.95
   of the source's peak count). So a sun in a much dimmer blue sky is kept, and a blown-out opening full of
   sky is dropped. The flare is also a *budget* (`FlareBudget`). The composite measures how much of the frame
   is near-white (a soft step from 0.75 to 0.95 on the plain luminance, 16 × 16 taps of the eighth-resolution
   bright pass). It knows nothing of the threshold. The flare's strength is `(0.01 / area) ^ 0.6` beyond 1%
   of the frame. A few lamps or a sun keep full strength. A frame with an opening full of sky (7% near-white)
   gets about 0.3, so it is a veil, not a wash. `flare.debug 2` shows the measure (red = area × 10, green =
   the budget).
3. **No copy of the frame has an edge.** The source map fades out over the last 5% of the frame. A ghost is
   windowed by the position it is sampled from, so nothing ends in a straight border.
4. **Everything is bounded.** Each layer saturates on its own (`1 - exp(-gain x)`). The layers are summed,
   scaled by *Intensity*, passed through the same soft limiter, and screen-blended onto the picture.
   Intensity 1 cannot white out a frame, and raising it adds light smoothly.

**The layers.**

- **Ghosts.** Each source becomes the shape of the aperture: the mean of the source map over an n-blade
  polygon, then saturated so a small lamp and a big one both make a clear blob. Its 256 taps read
  footprint-matched source mips to fill the aperture without speckled holes. Ghost *k* is that blob scaled
  about the frame centre by a fixed factor (`GhostScale`: alternately mirrored through the centre, or
  between the source and the centre). So every ghost lies on the straight line through its source and the
  centre. Each ghost has a slight colour variety and a red/blue split, weaker for sources far from the axis
  ([Chapman, pseudo lens flare](https://john-chapman-graphics.blogspot.com/2013/02/pseudo-lens-flare.html);
  [Hullin et al., physically based lens flare](https://resources.mpi-inf.mpg.de/lensflareRendering/pdf/flare.pdf)).
- **Halo.** The mean of the blobs on a circle round the frame centre, scaled by how many blobs fit round it
  (`HaloRingBlobs`). This is a ring centred on the screen centre, passing through each source's own distance
  from it, and as bright as one blob whatever its radius.
- **Streak.** A horizontal line integral centred on each source. It samples every pixel centre of a
  filtered source map, so camera movement changes the smooth kernel weights without skipping narrow lights
  or leaving repeated bars. A nine-tap Gaussian over `kStreakSoft` (six full-resolution pixels) smooths the
  width. The map uses eighth width and half height. Streaks and spikes
  render at half resolution to keep their edges smooth. Both line integrals weight each tap by
  `LineTailWeight`: exponential falloff, with a smooth taper to zero over the last half of the reach.
- **Spikes.** One symmetric line per blade pair, each through the source. A hexagonal aperture makes six
  spikes. An odd blade count gives twice as many spikes as it has blades. The first spike is vertical
  (`SpikeLines`, `SpikeAngle`).
- **Glow** (a separable blur of the source map) and **lens dirt** (procedural, or your own image with
  `lens dirt <path>`, read as brightness). Both only light up where flare lands.

**Settings.** `flare.on`, `flare.intensity` (0 to 2), `flare.threshold` (0.75 to 1, default 0.85),
`flare.ghosts` (0 to 8), `flare.halo`, `flare.streak`, `flare.streakLen`, `flare.star` (spikes),
`flare.blades` (3 to 12: ghost shape and spike count), `flare.glow`, `flare.dirt`, `flare.tint`,
`flare.streakTint`, `flare.debug` (1 flare only, on black; 2 the budget measure). The two tints are
indices into palettes in `LensFxMath.h`. `FlareTintColor` holds warm, white, gold, cool, teal, rose,
violet and ember. `StreakTintColor` holds blue, cyan, white, amber, red and magenta. The console also takes
the name (`lens flare.tint cool`). Everything else is a fixed constant in
`LensFxMath.h` (mirrored in the HLSL): range, boost, spread, gains, ghost size and shape, hue, saturation,
variety, seed, blade roundness, halo width and compression, streak angle, spike length, rotation and
rainbow, glow size, dirt scale, edge fade and warp. Settings are session-local.

**Panel.** LENS FLARE has two pages, with at most eight controls on either. *Look* holds the look,
intensity, source threshold, ghosts, halo, streak, spikes and glow. *Detail* holds streak length, aperture
blades, lens dirt, the flare colour and streak colour swatches, and *Flare only*.

Looks: Cinematic (the defaults), Anamorphic (a long cool streak), Classic (eight ghosts and a ring), Star
(spikes only), and Dreamy (glow only, with a lower threshold). A look resets the group first.

## Pass list

Per frame, on a deferred context replayed with state restore: snapshot (resolve + whole-copy for
MSAA, depth copy) → autofocus → circle of confusion with the bokeh-gamma lift (+5 mips) → tile max →
dilate ×2 → gather → denoise → (the flare chain, from the SHARP frame) bright pass (half, gated per tap) → 13-tap
down (quarter, eighth) → source map (quarter: isolation, size floor, border fade) → 13-tap down (eighth) and
source mips plus a Gaussian streak map (eighth width, half height) → aperture blobs
(quarter, from footprint-filtered source mips) →
ghosts + halo → streak + spikes (half) → glow (H, V) → composite (soft limiter, screen blend; per-sample on MSAA).
Shaders are compiled in a background job at device start through the environment shader cache.

Surface reuse checks colour and depth dimensions independently, along with their formats and
sample count and quality. A depth-only resize rebuilds the depth copy and resets autofocus history.
An incomplete allocation is never reused as a finished surface set. `lens.targets` records each
successful rebuild, and `lens.frame` includes the copied depth dimensions.

## Verification

`Automation/tests/lens-fx-math-tests.cpp` pins the parameter table, circle
of confusion (both lens types), iris boundaries, orientation and kernel, the bokeh-gamma lift and the
gather LOD, masks, presets and the flare math. The flare math covers the luminance gate and its
monotonicity in the threshold, the isolation and border fade, the limiter and the bounded blend, ghost
placement on the source-to-centre line, the halo and spike geometry, the palettes and the panel's
swatches, and the five looks. The same file statically checks that the HLSL still carries the same
constants and shapes, and that the constant buffer in the shader and the `Constants` struct in
`LensFx.cpp` list the same members in the same order.

`python Automation/verify/compile-lens-shaders.py` compiles every shader entry with fxc exactly as the
hook does (it parses `ShaderSource()` and `ShaderRequests()` from `LensFx.cpp`), one PASS/FAIL line each.
A shader typo or constant-buffer slip shows up without launching the game. `mvm_debug` logs `lens.shader`
(compile), `lens.frame` (target, depth, sample counts, formats, settings) and `lens.focus` (where autofocus
is sitting). Live captures are under `Automation/runs/lens/`.

`LensFlareGpuTests` renders the real shaders against a one-pixel lamp and checks streak continuity, a single
smooth peak across its width, and a filled ghost interior. These GPU checks supplement the engine-free math
suite.

## Limits

The frame is already tonemapped, so a "sun" is only white. The bokeh gamma and the flare's per-layer gains
approximate the missing range, and they are not physical radiance. A source bigger than about 4% of the
frame height, or nearly as bright as its surroundings, never flares, whatever the threshold. That is
deliberate. It keeps the sky and lit walls from washing the frame. The cost is that a huge sun disc in a
nearly-as-bright sky throws no flare. The EXR (pre-tonemap HDR) export path does not run this pass.
