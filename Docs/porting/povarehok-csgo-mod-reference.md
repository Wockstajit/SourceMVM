# Povarehok (CS:GO FX mod) Reference

> **Current use.** Most On and Less categories use **Insurgency 2's own FX set**, in the local
> `FX/sources/insurgency2/`. The HE grenade explosion, the Less fallback impact blend and
> money-on-headshot still use assets from this mod. See [FX/README.md](../../FX/README.md) and
> [`Docs/features/filmmaker_effects_modifiers.md`](../features/filmmaker_effects_modifiers.md) for
> how it is wired in. Two parts apply to the current build:
> - **The seven failure patterns in section 10** describe generic Source 1 to CS2 particle
>   conversion bugs. They are implemented in `FX/tools/postprocess_common.py`, which the packs share.
> - **Money-on-headshot** (sections 2 and 5) comes from this mod. It is the one effect with no
>   Insurgency equivalent, and it is extracted on its own into the local `FX/sources/povarehok-money/`.

This document covers what the CS:GO particle and material mod (a local donor tree, not in the repository) is,
and why it is hard to port to CS2. It covers the mod's features and loading mechanism, the texture
failure patterns, and a debugging checklist. It is written for an LLM
or a human exploring the mod or debugging one broken texture. See also
[Docs/porting/mw2019-fx-mapping-reference.md](mw2019-fx-mapping-reference.md) (the other source pack)
and `Docs/features/filmmaker_effects_modifiers.md` for how the converted result is wired into the DLL
(`FxMode::On` and `Less`).

## 1. What it is

A from-scratch particle overhaul for CS:GO by an author going by **Povarehok**. The author spent
roughly 4 months on it (per the author's own release post), and made it for moviemaking and cinematic
editing, not competitive play. It replaces almost every particle system a player sees. That includes
HE grenade explosions, molotov and incendiary fire, C4 detonation, every weapon's muzzle flash, tracer,
shell casing and barrel smoke, player-hit impacts (blood, headshots), and environment impacts
(wood, metal, concrete, wallbang). It also adds a few novelty extras: an optional "money sprays out on
headshot" swap, a taser shock-rope that keeps glowing after the shot, warp and heat distortion on
muzzle flashes and explosions, and a small lingering fog radius after C4 detonates ("yaderka mode").

## 2. How CS:GO loads it, and why that makes the CS2 port hard

This is **not** a Workshop addon, and it is not a scripted mod like the GMod ARC9 and MW2019 pack.
There is no Lua, no VScript and no per-weapon config. It is a plain **Source 1 content-replacement
pack**: a folder tree of `.pcf` (particle definitions), `.vmt` (materials), `.vtf` (textures) and
`.mdl` (models). It is distributed as a `.rar` and installed through **MIGI**, a third-party CS:GO tool
that mounts extra addon folders as additional search paths without touching the base game files. The
author's install instructions, quoted from the mod's release notes, are:

> 1. If u want update impacts/explosive particles, just go to `Counter-Strike Global
>    Offensive\migi\csgo\addons\PARTICLESADDONFOLDER\particles` and replace
>    `explosives_fx.pcf` and `impact_fx.pcf`
> 2. If u want update weapon particles, go to the same `particles` folder, rename the
>    file to `PARTICLESADDONFOLDER.pcf`, and drop it in

That mechanism works because of how Source 1 resolves particle systems. **Every particle system is
looked up purely by its own name** (for example `impact_concrete`, `weapon_muzzle_flash_assaultrifle`,
`blood_impact_medium` or `explosion_c4_500`), never by the file it lives in. Whichever mounted `.pcf`
defines that name first in the active search paths wins. So the mod does not need to hook or patch
anything. It ships `.pcf` files under the same relative paths CS:GO already expects
(`particles/impact_fx.pcf`, and so on), and those files redefine the same system names with new
particle content. In native CS:GO the game has no idea a mod is active. The mod silently shadows the
stock files.

**This is the single biggest reason the CS2 port is hard.** CS2 has no "first loose file with this
name wins" precache resolution, and it does not understand Source 1 `.pcf`, `.vmt` or `.vtf` at all.
There is no shortcut. Every asset has to be converted to Source 2 `.vpcf`, `.vmat` and `.vtex` (via
`source1import`), and the DLL has to swap to the converted file's own distinct path at runtime (see the
`FXVAR` and `FXRULE` macros in `ParticleFxRules.cpp`). Copying the folder in does not work. Every texture
problem in section 10 traces back to an assumption in the conversion pipeline that held for stock CS:GO
content but not for this mod's assets.

The optional **"money on headshot"** feature uses the same file-swap trick. The author documented it in
a text file bundled inside the mod (`Read if u wanna money FX.txt`):

> IF YOU WANT ADD MONEY FX FOR HEADSHOT:
> 1. Go to `Counter Strike: Global Offensive/migi/csgo/addons/p_betterparticlesmod/particles`
> 2. Change name for `impact_fx.pcf` to anything else
> 3. Change name for `impact_fxmoney.pcf` to `impact_fx.pcf`
> 4. Update your MIGI build.
> 5. Now u have some "CHOPPA" by static fx in game lol

So the toggle works at the file level. It is a full swap of the entire impact-particle file.
`impact_fxmoney.pcf` is a complete alternate version of `impact_fx.pcf`, where the headshot-specific
system names render a dollar-bill and confetti burst instead of blood. You install one file or the
other, never both.

## 3. The three variants on disk

```
csgo-effect-mod/            (local donor tree)
├── p_betterparticlesmod_classic updated_c057b/p_betterparticlesmod_classic/   -> On/regular
├── p_betterparticlesmod_lessimpacts/p_betterparticlesmod_lessimpacts/         -> Less (impacts half)
├── p_betterparticlesmod_lesssmoke_22ac2/p_betterparticlesmod_lesssmoke/       -> Less (smoke/explosions half)
└── legacy_csgo_deps/                                                          -> 2 bundled stock CS:GO models the particles reference
```

These are the mod author's own internal folder names. Note the `p_betterparticlesmod_` prefix, not
"Povarehok". They stay exactly as they are on disk. Only SOURCE:MVM's converted-output namespace is
named `povarehok`. See [filmmaker_effects_modifiers.md](../features/filmmaker_effects_modifiers.md) for
the runtime namespace.

Each of the three is a full, independent copy of the same folder skeleton (`particles/`, `materials/`,
`models/`). None is a small diff patch.

**Per-file comparison:** `less_impacts` and `less_smoke` are each nearly identical to
`classic updated`. The only files that differ are `impact_fx.pcf` (different in both, each in its own
way) and `explosions_fx.pcf` (an identical "toned down" version in both). Everything else (blood,
tracers, muzzle flashes, molotov, money, snow impacts, shells) is byte-for-byte the same in all three
folders. That is why our CS2 "Less" mode is built as a **per-file combination**. Bullet impacts come
from `less_impacts`, and everything else that differs comes from `less_smoke`. We do not pick one
folder wholesale.

## 4. File-type primer (Source 1 formats)

| Extension | What it is |
|---|---|
| `.pcf` | Binary "Particle Compiler File": one file bundles many independently named particle system definitions (operators, emitters, renderers, and **child** system references that spawn automatically with no scripting). CS:GO resolves systems by name across every mounted `.pcf`. |
| `.vmt` | Plain-text "Valve Material Type": a shader name (`SpriteCard`, `UnlitGeneric`, `Sprite`, ...) plus `$key "value"` parameters. This is what a particle renderer or model actually points at. |
| `.vtf` | Binary "Valve Texture Format": the bitmap data a `.vmt`'s `$basetexture` names. It can be a single image or, for animated sprites, embed a **sheet** resource (multiple frame rects plus sequence timing) so one texture plays like a flipbook. |
| `.mdl`(+`.vvd`/`.dx90.vtx`/`.phy`) | Compiled Source 1 model. Only 2 stock CS:GO models are needed here (`legacy_csgo_deps/`: a concrete debris chunk and a wood gib). They come from a CS:GO `pak01`, so the converter does not need a full CS:GO install. |

## 5. Feature to file map

This table cross-references the author's changelog text against the actual folder contents:

| Feature (author's words) | Real file(s) |
|---|---|
| HE grenade explosion | `particles/explosions_fx.pcf` (+ `explosions_fx2.pcf`) |
| Molotov / incendiary burn & explosion | `particles/inferno_fx.pcf` (+ `inferno_fx3.pcf`) |
| C4 explosion | also `explosions_fx.pcf`; grenade and C4 detonations are defined in the same file |
| Player hit impacts (headshots, blood) | `particles/blood_impact.pcf` |
| Environment impacts (wood/metal/concrete/wallbang) | `particles/impact_fx.pcf`, `impact_fx_smoke.pcf` |
| Snow-surface impacts | `particles/impact_fxsnow.pcf` |
| Money-on-headshot (optional) | `particles/impact_fxmoney.pcf` (full swap over `impact_fx.pcf`, see §2) |
| All weapon muzzleflashes + tracers + barrel smoke + shell casings | `particles/weapons/cs_weapon_fx.pcf` and `particles/polished_weapons_fx.pcf` (see §7 for why there are two) |
| Shell casing **models** (not just particles) | `models/shells/*.mdl`: one model per caliber (9mm, 5.56, .308/762nato, 12 gauge, .50 cal, .338 mag, 5.7, plus a couple of oddly named ones like `aalpha12`/`vlk`) |
| Money-burst prop | `models/usMoney/` (a physical dollar-bill model) + `materials/usMoney/` (1/2/10/20/100-dollar bill textures) |
| Per-map ambient dust/fog | `particles/maps20/*.pcf`; **not mentioned in the changelog at all**, see §6 |

The author's "small features" list, mapped to how they are built:

- **Barrel smoke after shots**: a smoke puff or trail attached to the weapon's muzzle attachment point,
  defined inside `cs_weapon_fx.pcf` or `polished_weapons_fx.pcf`, alongside the muzzle flash for the
  same weapon class.
- **3D model particles for concrete, wood, brick and stone impacts**: some impact systems use a
  `RenderModels`-style operator that spawns real physical debris chunk props (see the bundled stock
  CS:GO gib dependencies under `legacy_csgo_deps/models/`). Heavy impacts throw actual tumbling
  geometry instead of a flat decal and puff.
- **Taser shock-rope that keeps glowing after the shot**: a rope-renderer particle tied to the taser
  wire, with a longer fade-out than stock, so the arc visibly lingers.
- **Warp on muzzle flashes, explosions, molotov and C4**: a mild heat-distortion or refraction child on
  the bigger effects, dialed subtly ("a little bit actually" per the author).
- **C4's small lingering fog radius** ("yaderka mode"): an extra low, ground-hugging fog puff child
  added to the C4 detonation system on top of the normal explosion.
- **Headshot sparks**: an extra spark child layered onto the headshot blood system in `blood_impact.pcf`.

## 6. Full file inventory (`classic updated` variant; identical layout in all 3)

```
particles/
  blood_impact.pcf              impact_fx.pcf             impact_fxmoney.pcf
  impact_fxsnow.pcf             impact_fx_smoke.pcf       explosions_fx.pcf
  explosions_fx2.pcf            inferno_fx.pcf            inferno_fx3.pcf
  polished_weapons_fx.pcf       weapons/cs_weapon_fx.pcf
```

These **11 files are the only ones our conversion pipeline touches**, through the `$effectPcfs` list in
`convert-povarehok-source1.ps1`. `polished_weapons_fx.pcf` exists only in `classic updated`, not in
either `less_*` folder.

The mod tree also ships extra content that the pipeline ignores entirely. Knowing this helps when
something "worked in CS:GO but is just missing in CS2". It is not broken. It is not converted at all:

- `particles/maps20/`: **per-map `.pcf` files** for 14 maps (`de_dust.pcf`, `de_nuke.pcf`,
  `de_inferno.pcf`, `de_train.pcf`, `de_mill.pcf`, `de_overpass.pcf`, `de_bank.pcf`, `de_aztec.pcf`,
  `de_house.pcf`, `de_shacks.pcf`, `cs_office.pcf`, `cs_italy.pcf`, `ar_monastery.pcf`,
  `gg_baggage.pcf`). CS:GO auto-loads a map-named particle file to override that level's ambient
  dust, fog and particle props. None of this is wired into the CS2 hook, so the mod's map-specific FX
  polish is not ported. If "some textures aren't working" turns out to be a map ambience issue, look
  in this folder.
- Bonus and kitchen-sink content that appears to be folded in from other community packs. The pipeline
  never references it, and no weapon uses it: `baconnparticles.pcf`, `betterparticlesmod_classic.pcf`,
  `block.pcf`, `kkk.pcf`, `13391392139030810398.pcf`, `4442342.pcf`, `impact_fx_insurgency.pcf`. The
  first six are within a few hundred bytes of `polished_weapons_fx.pcf`. They are near-duplicates of the weapon effects file. None of them is referenced by the mod's install instructions. They match the
  `materials/particles/tfa_ins2` and `materials/particle/ac/*` folders (reused Insurgency-mod textures)
  and a `materials/particles/minecraft/` folder. None of this is in the effect-PCF list, so none of it
  reaches CS2.
- `particles/blood_impact.pcf90` and `.pcf900`: **not real `.pcf` files that CS:GO would load**. The
  wrong extension makes them inert even in native CS:GO. They are almost certainly stray backup copies
  the author left in the folder. Ignore them.
- `models/shells/mw2019/`: a subfolder that duplicates four shell models (`shell_12gauge`,
  `shell_50cal`, `shell_pistol`, `shell_rifle`). The top level of `models/shells/` has the same
  models. The duplicates appear identically in all three variants, so they are original mod content.
  Povarehok's CS:GO mod already borrowed COD MW2019-styled brass models for those calibers. This is
  separate from the GMod ARC9 and MW2019 pack this project also uses for "Modern" mode. The duplication
  under an `mw2019/` path suggests the model's internal name baked into the `.mdl` does not match the
  path the particle system expects. The author placed a second physical copy instead of relying on one
  resolution.

`materials/` mirrors the particle content. `effects/` holds muzzle flash cards, explosion sprites, tiled
fire and lens flares. `particle/` and `particles/` hold impact dust, blood, smoke, sparks and debris,
which is the bulk of the texture count. `models/shells/` holds per-caliber brass textures, and `usMoney/`
holds the money-burst bills and confetti. In `classic updated` there are **1,264 `.vmt` files and 954
`.vtf` files** across those folders.

## 7. Why there are two weapon-particle files

`particles/weapons/cs_weapon_fx.pcf` is the correctly pathed, "real" weapon file that CS:GO expects.
`particles/polished_weapons_fx.pcf` is a second file with a different name. Both ship side by side, and
both are a meaningful size (about 305 KB each). Neither is a stray leftover. The name matches the author's
"More Polished / Clean UPDATE" changelog entry, which describes cleaner gun shots and fewer
"overspammed" particles. So `polished_weapons_fx.pcf` is a toned-down version of the
weapon effects. It ships as an addition next to the original, not as a replacement.

## 8. Why some textures look wrong

This mod's textures were assembled from a **large number of different donor packs and games**. They were
not authored from scratch as one consistent set. The distinct source packs show up as top-level
subfolders under `materials/`:

- `ac/`: a large recurring donor set (muzzleflash, blood and explosion sprites). It is also the only
  place in this mod where an MW2019-styled texture atlas appears:
  `particle/ac/experimental/mw2019_vfx_fluid_pool_4_atlas_2x2`.
- `bb2/`, `bmclassic/`, `bmpep/`, `hl2mmod/`: Source-engine mod texture sets (muzzleflash sprites, glow effects).
- `tfa_ins2/`: Insurgency-styled smoke and tracer sprites, from a GMod TFA weapon base. This is the same
  lineage as the ARC9 packs used elsewhere in this project.
- `baconn/`, `minecraft/`, `insandstorm/`, and a handful of one-off single-texture folders.

Because they were authored across different games, engines and years, these textures do not share one
material convention. Some use the modern `SpriteCard` shader. Some use plain `UnlitGeneric` with
`$additive`. A few use the old `Sprite` shader with `$spriterendermode` and `$spriteorientation` keys.
Some VMTs carry a **commented-out** `$additive` line. Any tool that reads VMT text must strip comments
before it checks flags. There are also duplicate and stray files. For example, `materials/effects/blueblackflash.txt`
sits next to the real `blueblackflash.vmt`. It is a draft saved with the wrong extension, and it
is harmless clutter. This inconsistency is the root cause behind most of the failure patterns in section
10. The pipeline has to handle several mutually inconsistent authoring conventions, not one clean asset set.

## 9. VMT keys that matter for the port

A typical particle-sprite VMT looks like this:

```
"SpriteCard"
{
    "$basetexture"    "effects/mp5_muzzle_card/mp5_muzzle_card"
    "$additive"       "1"          <- ADD blend: ignores alpha entirely, colors just sum
    "$addself"        "3"          <- self-glow multiplier (only meaningful w/ additive)
    "$overbrightfactor" "1.5"      <- brightness multiplier past 1.0
    "$translucent"    "1"          <- alpha-blend instead (respects the alpha channel)
    "$vertexcolor"/"$vertexalpha"  <- lets per-particle tint/alpha drive the sprite
    "$DEPTHBLEND"     "1"          <- soft-particle fade against nearby geometry
}
```

Source 1's `SpriteCard` shader is **always unlit**. None of these particles received scene lighting. Any
darkening was baked in by the artist at spawn time, through color and alpha, not by the renderer. CS2's
equivalent renderer (`C_OP_RenderSprites`) defaults to **scene-lit** unless `m_flSelfIllumAmount = 1.0`
is set explicitly. This single default change is the number one cause of the "looks right in daylight,
goes dark and boxy in shade" reports.

## 10. Known texture failure patterns (check these first)

`FX/tools/postprocess_common.py` (shared by both packs)
patches every one. If a specific texture still looks wrong, check whether it matches one of these
patterns. Then check whether the fix's detection heuristic misses it. Do not assume a new bug class
first.

| Symptom | Root cause | Fix |
|---|---|---|
| Solid white or error quad | The converted sprite renderer has a `m_hMaterial` reference but **no `m_hTexture`**. CS2's sprite renderer only reads `m_hTexture`, and the `m_hMaterial` reference is silently ignored. Source 1 warp, refract and heat-distortion quads and screen overlays have no CS2 equivalent. | Inject `m_hTexture` from the VMT's `$basetexture` if the converted `.vtex` exists. Otherwise delete the renderer entirely, because an invisible emitter beats a white square. |
| Big glowing white square (molotov, C4, lens flares) | Some sprites export as **solid-white RGB with the shape stored only in alpha** (`particle_anamorphic_lens` and similar). CS2's `PARTICLE_OUTPUT_BLEND_MODE_ADD` **ignores alpha entirely**, so the whole square renders at full white. | Premultiply: bake `RGB *= alpha` into the texture for any texture used only by additive renderers. |
| Translucent smoke suddenly renders as its full square sheet-frame | Several VMTs carry `// "$additive" "1"`, which is commented out. A naive text search for `$additive` matches the commented line too, and wrongly flips the renderer to additive. | Always match VMT parameters against **comment-stripped** text, never raw text. |
| Smoke or atlas texture draws as a visible grid of frames instead of one cell | The mod's `.vtf` files store animated sprites as an embedded **sheet resource**, a frame table equivalent to `.sht`. Generic `source1import` conversion **drops sheet data entirely**. It exports a flat texture, so every particle samples the whole grid. | Use a custom `export-source1-vtf.py` step. It parses the VTF's embedded sheet info (or a lenient manual fallback), slices the real frames, and emits a `.mks` sequence file, so `resourcecompiler` builds a proper `vtex_c` sheet. |
| Dark fringe or "black ring" around soft smoke edges | The original VMT uses `$DUALSEQUENCE` and blend-mode keys (`ALPHA_FROM0_RGB_FROM1`-style dual-frame sampling). The rebuilt `.mks` sheets are single-mode, so the dual-sample keys read the wrong frame at the edges. | Strip the dual-sequence combine keys from converted VPCFs entirely. This reverts to plain single-frame sampling. |
| Flash or glow dims and gets a dark box around it, specifically in shaded areas | See section 9. The Source 1 `SpriteCard` is unlit. Converted renderers omit `m_flSelfIllumAmount`, so CS2's default scene lighting multiplies the sprite and its alpha halo toward black outside direct light. | Set `m_flSelfIllumAmount = 1.0` on every converted sprite, trail and rope renderer. Set `PARTICLE_OUTPUT_BLEND_MODE_ADD` wherever the original VMT said `$additive 1`. |
| Debris and chunks float in mid-air at ledges and stairs | Converted `C_OP_WorldTraceConstraint` collision blocks default to a cached single-trace "infinite plane" mode. Near an edge, that plane extends past the real geometry. | Force `COLLISION_MODE_PER_PARTICLE_TRACE` (a real per-particle raycast, which is fine for an offline movie tool). |
| A lingering smoke *ribbon* seems to hang in mid-air, disconnected from the muzzle | `C_OP_RenderRopes` (the rope renderer) draws one continuous ribbon across the emitter's **entire path** between two puffs. Ported as-is, it does not read as drifting smoke. | Convert rope renderers to sprite emitters (with frame blending), and shorten the particle lifetime. |

## 11. Checklist for a texture that still looks wrong in CS2

1. Find the resource in the mod tree: `materials/<...>.vmt`. Search by the name shown in
   `mirv_filmmaker fx names`, or grep for the sprite or model name.
2. Read the **raw** VMT. Note whether `$additive` is present, and whether it is commented out. Check
   whether there is a `$basetexture2`, `$DUALSEQUENCE` or sequence-blend key.
3. Check whether the referenced `.vtf` is a multi-frame sprite. Look for repeated tiles in the raw
   texture, or check whether the effect is meant to animate or flip.
4. Look at the corresponding **converted** `.vpcf` under
   `build/fx/povarehok-source1import/source2/content/.../particles/filmmaker/povarehok/<variant>/...vpcf`
   (after a `-Compile` run). Check whether it has `m_hTexture` set, whether `m_flSelfIllumAmount` and
   `m_nOutputBlendMode` were applied, and whether it matches one of the seven patterns in section 10.
5. If it matches a known pattern but still renders wrong, the postprocess pass's **detection heuristic**
   (a regex or name-hint list, not a general rule) probably misses this file. Extend the relevant
   function in `FX/tools/postprocess_common.py` instead of hand-patching the one `.vpcf`.
6. If it matches **none** of the seven patterns, it is a new failure mode. Capture a screenshot and the
   exact system name with `mirv_filmmaker fx log on` and `fx names` before you guess at a fix.
