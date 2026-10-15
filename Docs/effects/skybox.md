# Skybox & Clouds

Import any image or HDRI as a CS2 skybox, pick between yours and CS2's own, orient it live, and
drive the cloud layer.

- UI: CONFIG → EFFECTS → **Weather & Sky** (`Panorama/ConfigHudWeatherSkyTabJs.h`)
- Library: `World/SkyboxLibrary.cpp`, the pickable list, with apply and delete
- Import: `World/SkyboxBuilder.cpp` → `FX/tools/build-skybox.ps1`
- Orientation: `World/SkyOrient.cpp` (state and commands) drives `World/SkyGpuRotation.cpp` (the
  draw-time rotation itself), with live yaw/tilt/mirror
- Clouds: `World/CloudFx.cpp` (shape) and existing `mirv_sky clouds color` (colour)
- Stock thumbnails: `FX/tools/build-builtin-sky-previews.ps1`
- Status: **SHIPPED**, verified in-game

---

## 1. Five non-obvious details

These are the parts most likely to break on a CS2
update, or to be broken again by a well-meaning simplification.

### 1.1 Sky entity class matching

Sky entities match on both `env_sky` and `C_EnvSky` (`SkyOrient.cpp`). The sky entity reports:

```
debugName "mirage_sky" / className "env_sky" / clientClassName "C_BaseModelEntity"
```

Matching only the client class name finds **zero** entities and silently does nothing. This is
the same entity-class-vs-client-class trap recorded in `World/WorldEntities.h`. It is the most
likely silent failure after an update.

### 1.2 The sky texture must be a cubemap

`sky.vfx` samples `g_tSkyTexture` as a cube. Feeding it a 2D equirect does not reduce quality. It
does not render at all:

```
Bad texture dimension type 1 for texture "..." wanted 4 - returning error texture
```

CS2 then draws the magenta error texture, which can pass for "a custom sky loaded" in a
before/after screenshot.
Setting **both** `m_typeString` and `m_outputTypeString` to `"Cube"` makes resourcecompiler
reproject the equirect onto six faces by itself. There is no lat-long image processor to invoke.
Verified against Valve's `sky_de_mirage_exr_*.vtex_c`: 512×512 BC6H carrying
`VTEX_FLAG_CUBE_TEXTURE`, built from a single `.exr`.

### 1.3 Panorama will not render a compressed sRGB texture correctly

A DXT1 thumbnail renders **washed out to near-white with a pink cast**, even though the exact
same `.vtex_c` decodes perfectly outside the game. Valve's own `panorama/images` textures are all
uncompressed `RGBA8888` with `VTEX_FLAG_NO_LOD`. Match that, or the picker looks broken.

### 1.4 An sRGB output colour space is not a tone map

Compiling an HDR source straight to 8-bit with `m_outputColorSpace "srgb"` applies a transfer
curve. HDRI sky luminance routinely exceeds 1.0, so the values clamp and every thumbnail comes out
white. The pipeline therefore runs a float intermediate, then Source2Viewer `--texture_decode_flags
ForceLDR` (which does tone-map), then compiles the resulting PNG.

Corollary for the stock-sky previews: that decode must run **without** `ForceLDR` on the cubemap
itself. Otherwise the faces decode to `Bgra8888` first, and cube assembly fails with
`Cubemap faces must be RgbaF32 format`. Decode the cube to `.exr` first, then tone-map the 2D.

### 1.5 `.hdr` is not readable by resourcecompiler

`HelperLoadFbmByFileExtension: Unknown file type`. `.exr`, `.png`, `.jpg`, `.tga`, `.psd` and
`.pfm` all load. Since `.hdr` is what most HDRI sites hand you by default, the tool decodes
Radiance RGBE to `.pfm` itself (inline C#, both formats are linear 32-bit float, so nothing is
lost). `.hdr` support is therefore a *conversion*, which the compiler does not do itself.

---

## 2. Live sky rotation: draw-time renderer path

CS2's sky draw does not consume the `env_sky` scene-node transform. The feature therefore uses
the renderer hook in `World/SkyGpuRotation.cpp` instead of writing entity angles:

1. `sky_pc_50_vs` declares `g_matSkyWsToOs` in the vertex shader's `$Globals` constant buffer
   (`VS.cb0`, found by extracting and reflecting the shipped VCS).
2. The hook caches the engine's dynamic upload, clones the 128-byte matrix, composes yaw/pitch/
   mirror, and binds the clone only for the sky draw. The original buffer is restored immediately.
   The matrix is already Z-up: the engine's own value is a pure rotation about its third axis.
   The yaw composes about that axis, so **Rotate** spins the sky with a level horizon.3. The map sun is a separate `csgo_effects.vfx` instanced draw. SceneSystem marks materials in the
   `sun_disc_glow` and `sun_glow` families before the draw hook runs, so both the bright disc halo
   and the lower-boost halo use the same billboard transform. The high pixel `g_flColorBoost`
   test remains a fallback for material names that are not exposed. Their vertex buffers are
   cloned and rotated around the current camera. Bloom size scales that geometry around its
   authored centre. This avoids rotating unrelated effect sprites and keeps the halo on the disc.
4. The visible disc uses Sun yaw/pitch only. Skybox yaw/pitch never enters the disc vertex
   transform, so moving the cubemap cannot move the sun decal. This keeps the physical sun, its
   live shadows, and the on-screen disc on one control path, while the sky/cubemap remains
   independently editable.

Controls:

```
mirv_filmmaker world skyorient yaw <deg>|pitch <deg>|mirror 0|1|target sky 0|1|target fog 0|1|reset
mirv_filmmaker world light yaw <deg>|pitch <deg>|reset
```

Sky yaw, pitch and mirror, plus the independent Rotate skybox and Rotate fog & smoke targets, are
in CONFIG → EFFECTS → Weather & Sky → Skybox → Settings. Scene-light direction stays in
Lighting → Sun. The two control groups are intentionally independent. All values persist in
`%APPDATA%\HLAE\filmmaker_settings.json`.

### 2.1 Verification

The Nuke repair demo was held at a fixed camera and tick. With the Sun direction reset,
the sun disc appeared at the baseline position. Changing Sun yaw by −15° moved the disc
horizontally, and changing pitch by −20° moved it downward. Changing Skybox yaw while holding
Sun fixed left the disc at that same screen position; only the cubemap moved. The capture
stayed clean because only named/high-glow sun materials were selected. A 180° Sun sweep moved
the live fence shadow across the spawn ground in the same run.

`build-skybox.ps1` keeps `-Rotation` / `-Flip` for authoring a pre-oriented sky. Live controls
use the draw-time path above and do not build a second cubemap per angle.

### 2.2 Known boundary

Distant mountains and skylines are 3D skybox geometry, not the cubemap texture. The live rotation
moves only the sampled sky. The separate sun draw follows Sun, and unrelated world geometry is
not rotated.

---

## 3. Clouds are a shader, not a texture

`materials/effects/clouds_001.vmat_c` and friends use `csgo_effects.vfx`, which exposes
`g_flOpacityScale`, `g_flColorBoost`, and three scrolling mask layers (`g_vMask1..3Scale` /
`g_vMask1..3PanSpeed`). Density, size and drift are therefore real controls.

Two rules the module holds to:

- **Every value is a multiplier on the map's own**, never an absolute. Each map authors its
  clouds differently, so `1.00` means "untouched" everywhere, and restore is exact.
- **Only the first two components of each vector are scaled.** Those are UV scales and pan
  speeds. Components 2 and 3 are padding, and multiplying padding corrupts the material.

Colour is handled by the `mirv_sky clouds color` backend. A second writer for
`g_vColorTint` would let two features fight over a frame.

Both match materials whose name contains `"clouds"`, so the cloud materials the current map loaded are the ones written:
No per-map table is needed.

---

## 4. Commands

```
mirv_filmmaker world sky list|apply <name>|default|delete <name>|refresh
mirv_filmmaker world skyorient yaw <deg>|pitch <deg>|mirror 0|1|target sky 0|1|target fog 0|1|reset
mirv_filmmaker world skybuild pick|import <img> [name] [rot] [flip]|rebuild <name>|status
mirv_filmmaker world clouds density|brightness|size|drift <x>|apply|reset
mirv_sky clouds color <r> <g> <b>|default
mirv_sky color <r> <g> <b> <brightness>|default
```

Authoring a skybox outside the game:

```
pwsh FX\tools\build-skybox.ps1 -Image <file> -Name <name> [-Rotation <deg>] [-Flip] [-FaceSize 1024]
pwsh FX\tools\build-builtin-sky-previews.ps1     # thumbnails for CS2's own skies, run once
```

---

## 5. Verification

1. Imported a Poly Haven `.hdr` and `.exr` end to end. Both compiled to BC6H cubemaps.
2. Swapped between three skies at a fixed camera and tick: three distinct skies, no error texture.
3. Rotation and mirror confirmed by screenshot at a fixed camera and tick (see 2.1).
4. Delete confirmed by the library showing 14 entries (built-ins only).
5. Cloud tint confirmed by screenshot (pink cloud wisps over Mirage).

Not verified: behaviour inside a render job; a map whose sky is not `env_sky`; the two stock skies
that do not decode (`sky_csgo_cloudy01`, `sky_black`) keep letter tiles.

---

## 6. Known gaps

- Cloud density/size/drift apply **on command**, not continuously. After a map change, move a
  slider (or `world clouds apply`) to re-assert. Sky orientation *does* re-assert per frame.
- Deleting a skybox leaves the compiled `.vmat_c`/`.vtex_c` under the CS2 install. This is
  harmless. The index entry is gone, so the sky leaves the picker, and re-importing the same name
  overwrites the files.
- `-Rotation`/`-Flip` on the build script cannot be applied to `.exr` or `.tga` sources, because
  they are not decodable in-script. The script refuses the request rather than silently ignoring it.
- No in-game rename.

## Related

- [`README.md`](README.md): master scope
- [`update-resilience.md`](update-resilience.md): CS2 update recovery
- `AfxHookSource2/Filmmaker/World/WorldEntities.h`: the entity-class-name trap in 1.1
