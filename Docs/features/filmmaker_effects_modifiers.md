# Effects modifiers (Config panel EFFECTS section)

Environment illumination is separate from particle-pack selection. Weapon-fire and
explosion events, held and flying Molotov state, and inferno burning patches drive runtime
lights whichever pack is selected. Empty clicks do not emit muzzle light. Native smoke
can receive these lights, but transparent sprites do not automatically cast opaque shadows.
The [lighting contract](../effects/whole-map-relighting.md) covers moving-slot limits, fire timing,
weapon response, and the visual verification still remaining. Changing a particle asset alone does
not add a light source or establish casing-shadow support.

The EFFECTS section is particle-FX only.

Runtime particle-effect control for demo playback. Each category has **On / Modern / Off**,
and a category only offers the modes it has real assets for. Toggle the modes live from the Config
panel (`mirv_filmmaker config`) or the console (`mirv_filmmaker fx ...`). Settings persist to
`%APPDATA%\HLAE\filmmaker_fx.json`.

## Offline live-match testing (`mvm_test`)

For shooting bots on a local practice map (not demo playback), use the **`mvm_test`**
harness instead of the demo-only CONFIG panel:

```text
mvm_test start [map]     load map + bots (default de_dust2) and arm test mode
mvm_test menu on|off|toggle   FX effects panel (Insert does the same)
mvm_test status          print active flag, map, menu, fx hook state
mvm_test stop            disarm test mode and close the menu
```

One-click launch (hook + FX pack + `mvm_test start`):

```batch
Automation\launch\test.bat
```

Or:

```powershell
powershell.exe -ExecutionPolicy Bypass -File Automation\launch\launch-cs2-test.ps1 -Map de_dust2
```

On a **live map** (not demo playback), the **Insert** key arms test mode and opens the effects-only
FX TEST panel. You do not need `mvm_test start` if you are already on a map. Press Insert again to
close. `mvm_test start` also opens the menu automatically after load.
While the panel is open, **G** switches between **UI mouse** and **game mouse**, the same as the
MOUSE button in the panel header. UI mode lets you click effect toggles. Game mode returns control
for aiming and shooting.

The per-category controls match CONFIG → EFFECTS. Live play does not reseek a demo, so shoot to see swaps.

Fresh runtime defaults are opt-in. The master switch and all six categories start **Off**. With no
saved configuration, startup and demo entry do not arm the particle hook or load anything under
`particles/filmmaker/`. Saved configurations load exactly as stored.

Effects never run at the main menu or on CS2's UI maps. A saved configuration stays stored there,
and the particle hook arms only once a demo plays or an `mvm_test` map is up. At the menu, any
`mirv_filmmaker fx` or `mirv_filmmaker world` command that would switch an effect on is refused
with "Load a demo first". Turning effects off, resets, status and lists still work.
`EffectsPersistence_SceneActive()` is the one predicate for "a scene is up".

Selecting a non-Off category turns the master on and makes only that category's selected pack
eligible for swaps. Loading is **front-loaded into a warm-up burst** at demo open and after settings
switches:

- **Demo open (level change):** the per-level handle cache is wiped, because handles dangle across map transitions. The FULL active target set is queued. Once the new level settles (demo tick advancing on two consecutive pumps, or a 3s wall-clock fallback), the main-thread pump burst-resolves the set under a per-frame time budget: 30ms while the demo is paused or within 10s of open, 20ms otherwise. A measured run resolved 51 targets in 956ms across 26 frames (max single resolve 63ms), concentrated in the demo's first moments. The budget is checked per frame, after each resolve. It bounds the steady rate, not the worst single frame, since a cold blocking load takes 15-60ms on its own.
- **Settings switch mid-demo** (category mode, master on, moneyshot, custom rules): the uncached targets of the new selection are queued and burst on the next pump, with the full budget if paused. Finishing a warm-up while paused triggers the debounced apply-reseek, so the frozen frame recomposes under the resolved swaps.
- **Disk prefetch:** the first warm-up also starts a once-per-process background thread (`ParticleFxPrefetch.cpp`). It plain-reads every file of the mounted pack (`USRLOCALCSGO`) to warm the OS file cache. It uses no engine APIs, so it is thread-safe, and it makes each main-thread blocking resolve cheap.
- **Lazy fallback:** a target the burst missed still fails open once, is queued on first real use, and is resolved by the adaptive trickle. The trickle runs slowly with heavier backoff during playback, faster while paused, and never at the main menu.

`fx state` reports `warmupPhase` (0 idle/trickle, 1 waiting for level settle, 2 bursting). The mvm_debug log gains `fx.resolve` "warmup armed / burst start / done" lines and `fx.prefetch` totals. Switching a category replaces obsolete jobs. Turning the master Off cancels all pending work, but keeps the configured modes and already-resolved handles until the next level change.

Two converted asset packs feed the modes:

- **On**: **Insurgency 2**'s own FX set, from the local `FX/sources/insurgency2/` (donor material is not in the repository: [FX/README.md](../../FX/README.md)). Bullet Impacts use Insurgency's reduced variant of the same system where the game authored one (its `_cheap` rows, else the lighter `_b` sibling).

  Insurgency 2 is one game with one consistent asset library. As a Source game, it uses Valve's `impact_<surface>` / `weapon_tracers*` naming, so it maps onto CS2's system names almost 1:1.
- **Modern**: the GMod **ARC9 / MW2019** packs (class muzzle flashes, per-shot barrel smoke, the mw2019 tracer family, and the MW frag explosion). They were extracted read-only from the local GMod install and converted through the same pipeline. Offered on Bullet Tracers, Muzzle Flash & Shells, and HE Grenade. (`'more'` is accepted as an alias of `on`.)

The runtime does not invent fake sprites or swap grenades to unrelated stock CS2 particles. Every mode points at Source 2 resources converted from the real Source 1 `.pcf` assets with `kristiker/source1import`.

## Asset conversion

The converter's inputs are all repo-local, so a plain run needs no machine-specific setup:

- `Misc/source1import/`: clone of <https://github.com/kristiker/source1import> (gitignored; re-clone if missing).
- `Misc/Source2Converter/`: clone of <https://github.com/REDxEYE/Source2Converter> with its `SourceIO` submodule initialized (gitignored; re-clone if missing).
- The two stock CS:GO models the mod's particles reference (`models/gibs/wood_gib01b`, `models/props_debris/concrete_chunk07a`, each as `.mdl/.vvd/.dx90.vtx/.phy`), plus their VMT/VTF materials. They were extracted from a CS:GO `pak01_dir.vpk`, so conversion does not depend on a CS:GO install.
- Python deps: `pip install Pillow numpy vdf dataclassy==0.10.4 parsimonious==0.10.0
srctools==2.3.4 vtf2img vpk`.
- **Modern pack sources:** the MW2019 Source 1 inputs live in the local `FX/sources/modern-warfare-gmod/`. That is a ~44 MB extracted tree with 3 PCFs and their VMT/VTF closure, and it is not in the repository. A build therefore needs no GMod install. To refresh that tree from an updated GMod install, run the converter with `-RefreshModernFromGmod`. That requires these workshop items to be subscribed: `2910505837` (ARC9 Weapon Base), `3258297368` ([ARC9] Modern Warfare 2019), and `2459720887` (Modern Wokefare Base). `FX/tools/extract-modern-particles-gmod.py` then re-pulls the three PCFs (`arc9_fas_muzzleflashes`, `arc9_fas_explosions`, `mw2019_tracer`) and their material closure from the GMAs and GMod base VPKs. Nothing GMod-side is modified. A handful of sprites the FAS PCFs reference exist in no local source. GMod falls back for them, so their renderers are pruned by the post-process (`modern-missing-materials.txt` lists them).

Convert the referenced Source 1 assets before the modes can use them:

```powershell
powershell -File FX/tools/convert-fx-source1.ps1 -Compile
```

The `-Source1ImportDir`, `-Source2ConverterDir`, and `-LegacyCsgoDir` flags, or the matching env vars, override the repo-local defaults.

The wrapper stages the mod PCFs under a stable namespace, then runs `utils/materials_import.py -b cs2` and `utils/particles_import.py -b cs2`. It exports the mod VTF textures. Sprite-sheet reconstruction turns embedded `.sht` data into per-frame slices, a `.mks`, and a `.vtex` input. Without that, every atlas draws as a frame GRID. The wrapper then post-processes the converted VPCFs (`FX/tools/postprocess_insurgency.py`, which also drives `postprocess_common.py` and `postprocess_modern.py`), converts the bundled MDLs with Source2Converter, compiles everything with CS2's `resourcecompiler.exe`, and validates the full runtime closure (`FX/tools/validate-fx-assets.py`).

The post-process pass exists because CS2 renders these conversions differently from Source 1. It never invents placeholder assets. It only repairs and tones the real ones:

- FP muzzle-flash closure: overbright, alpha, and radius tone-down (Source 2 lighting blows out the Source 1 values).
- Molotov roots: incompatible cinematic children are stripped, and fire is toned down.
- Dead references (children and fallbacks the mod never shipped) are stripped, so CS2 stops logging `Failed loading resource` at demo load.
- Sprite renderers without an `m_hTexture` draw SOLID WHITE QUADS, because CS2 ignores `m_hMaterial`. When the VMT has a `$basetexture`, the original texture is injected from it. Otherwise the renderer is stripped (Source 1 heat-distortion and warp quads, and screen overlays).
- `$DUALSEQUENCE` combine keys are stripped. The rebuilt sheets are single-mode, and the dual-sample modes produce background-dependent dark fringes ("black ring" smoke).
- Impact world-trace constraints are forced to per-particle tracing, so debris does not land on a cached infinite plane at stair or ledge height. Physical impact chunks and bits have residual bounce zeroed and spin capped at 0.5 seconds. They tumble in flight but stop rotating shortly after reaching the floor.
- Converted impact simulation is tuned for indoor CS2 scale. On emits 55% of the authored debris, cuts debris launch speed to 45% and dust speed to 35%, uses 0.08 bounce and 0.05 slide, and stops or sticks small pieces near rest. The long flying smoke-ribbon children are handled as a whole-impact probability, because their one-shot emitter count cannot represent a fractional keep rate. `build-impact-smoke-variants.py` produces smoke-trail-free Insurgency closures. The hook keeps `kInsurgencySmokeTrailKeepPerTen` (6 of every 10) mapped Insurgency impacts on the normal closure. Solid chips and metal sparks remain on every impact. Povarhok's short local impact puffs are separate from those flying ribbons and stay intact every time the Povarhok donor is selected. Povarhok metal-spark radii are scaled to 0.65 after merge (`FX/tools/scale-povarehok-metal-sparks.py`), so the hybrid donor's sparks read smaller without changing travel speed.
- Povarhok's **3D model chunks** (the concrete and wood pieces, not smoke or sparks) get a dedicated post-merge physics tune. Each starts at a random orientation, has no continuous spin operator, and raycasts per particle instead of sharing a cached collision plane. It has zero bounce and slide, and fades within 1.25 seconds. This stops the chunks from spinning on the floor or hanging on invisible or previous collision planes. Impact `On` uses the reduced configurations internally, including the mapped wood impact.
- Brick, concrete, plaster, sheetrock, tile, and rock impacts use local smoke and dust puffs with 25% larger sprite radii. `layer-vanilla-impacts.py` forks the donor closures per wall surface family, so carpet, snow, and other surfaces that share those donors keep their original sizes. CS2's native layer, debris, sparks, flying ribbons, and screen-space smoke are unchanged.
- Impact smoke and dust renderers stay alpha-blended and are capped to neutral overbright. Additive or overbright converted smoke would stack repeated bullet hits into red and orange clouds instead of a normal alpha fade. Non-fire bullet impacts and weapon wisps that reference the volumetric `vistasmokev1` sheet are retargeted to Insurgency-exported thin smoke (`materials/particle/insurgency/*`, with in-pack `insandstorm_*` / `smoke1` / `sq_fulldustfront1_2` fallbacks). `patch_non_fire_vistasmoke_replacements` in `FX/tools/postprocess_povarehok.py` does this. Molotov and explosion systems keep vistasmoke. To re-run just these idempotent repairs on an existing tree, use `--runtime-impact-fixes-only` (blending only), or run a full post-process pass after re-converting.
- VMT parameters are matched against **comment-stripped** text. Several mod VMTs carry `// "$additive" "1"`, which the author disabled. A comment-blind match would make translucent smoke additive. Additive ignores the alpha mask, so every soft cloud would render its full square sheet frame (the molotov and C4 gray squares). `repair_wrongly_additive_renderers` also undoes this on an already-converted tree.
- Solid-white-RGB textures used ONLY by additive renderers get their alpha mask premultiplied into RGB. `particle_anamorphic_lens` is the big white square on molotov and C4 explosions. Its lens streak lives in alpha, and CS2's ADD blend ignores alpha.
- Sprite renderers backed by `.mks` sheets get `m_bBlendFramesSeq0 = true`. Source 1 SpriteCard blended sheet frames by default, so stepped frames without that flag read as jittery, pixelated smoke.
- Weapon and bullet-impact smoke sheets get `m_nAnimationType = ANIMATION_TYPE_FIT_LIFETIME` (`fix_looping_smoke_animation` in `postprocess_povarehok.py`). The mod authors these `C_OP_RenderSprites` smoke systems with a fixed `m_flAnimationRate`. CS2 wraps a fixed-rate sheet (`frame = age*rate mod frames`), so one shot's muzzle smoke and one wall hit's impact smoke would visibly **re-play the sprite over and over**. FIT_LIFETIME maps the sheet to `[0..1]` of the particle lifetime, so it plays once with no wrap. The fix is scoped by texture to smoke and dust sheets (`thinsmoke`, `insandstorm`, `wd_gfx_steam`, `smoke1`, `fas_dust`, `water_splash`, ...). Muzzle-flash flames, sparks, fire, and blood keep their intentional fast flicker. The pass is idempotent. It runs in the full pass and in both in-place entries (`--gameplay-composites-only`, `--runtime-impact-fixes-only`).
  **Rate cap:** FIT_LIFETIME alone is not enough. The animation rate still multiplies under FIT (`frame = normalizedAge * rate * sheetTime`, per the engine/VRF frame math). Any smoke renderer with a rate above 1 overshoots the sheet and wraps anyway. Examples: `cursedgovno1`'s wall-impact splash at rate 10 replays 3-6x per hit, and Modern's `muzzleflash_suppressed` dust at rate 5. Two more levers run in the same passes. `fix_looping_smoke_animation` **caps `m_flAnimationRate` at 1.0** on these smoke renderers, and it also walks the **Modern** `arc9_fas_muzzleflashes` tree. `common.clamp_sheet_sequences` strips the `LOOP` flag from the one-shot smoke, dust, steam, and splash `.mks` sheets (`CLAMP_SMOKE_SHEET_RE`). That regex is deliberately narrower than the renderer-texture regex, so sparks, blood, debris, and airburst sheets keep their intentional looping flicker. Any residual overshoot then holds the last frame instead of replaying. Changed sheets need their `.vtex` recompiled. The in-place entries print the list.
- `patch_modern_tracer_glow_brightness` is a **one-shot tone pass** that multiplies alpha and radius by 0.5. It only runs in the full conversion pipeline. `--gameplay-composites-only` passes `tune_tracer_brightness=False`, because an in-place re-run would halve the already-halved tracer glow a second time.
- Modern muzzle-flash sprite bursts that reference `hl2_muzzleflash` are retargeted to the pack's higher-resolution `fas_muzzleflash_test_b` sheet. They receive `C_OP_PositionLock`, so the burst stays attached to the muzzle. Their authored animation rate, alpha behavior, and frame-blend setting stay intact.
  The idempotent in-place patch entry (`--gameplay-composites-only`) runs the three repairs above plus the gameplay composites. Changed files still need a `resourcecompiler` pass.

A pack that compiles particles but not materials, textures, or models renders as white or error quads in game. The validator exists to catch exactly that.

```text
particles/filmmaker/insurgency/<pcf>/...        (On lives here)
particles/filmmaker/money/impact_fxmoney/...    (the moneyshot toggle)
particles/filmmaker/modern/arc9_fas_muzzleflashes/...
particles/filmmaker/modern/arc9_fas_explosions/...
particles/filmmaker/modern/mw2019_tracer/...
```

`ParticleFxRules.cpp` uses these namespaces in its variant tables. The launcher mounts the compiled output automatically via `USRLOCALCSGO`. It prefers the pack staged into the shipped build (`build/staging-release/fx/source_mvm_fx`) and falls back to the converter's working output (`build/fx/povarehok-source1import/source2/game/source_mvm_fx`). If the converted pack is not compiled and mounted into CS2's resource search path, the swap fails open and the original CS2 effect plays. This is intentional. Missing assets must not fall back to random placeholders.

## Categories

| Key          | UI label              | Modes              | Matches (path prefix on the vpcf resource name)                                                                   |
| ------------ | --------------------- | ------------------ | ----------------------------------------------------------------------------------------------------------------- |
| `impacts`    | Bullet Impacts        | On/Off             | `particles/impact_fx/`, `particles/water_impact/`, `particles/breakable_fx/`                                      |
| `tracers`    | Bullet Tracers        | On/Modern/Off      | weapon-fx paths whose name contains `tracer`                                                                      |
| `weaponfx`   | Muzzle Flash & Shells | On/Modern/Off      | `particles/weapons/cs_weapon_fx/`, `particles/unified_weapon_fx/`                                                 |
| `explosions` | HE Grenade            | On/Modern/Off      | explosion folders, minus `c4`/`bomb` names                                                                        |
| `bombfx`     | Bomb (C4)             | On/Off             | explosion folders, `c4`/`bomb` in the name                                                                        |
| `flashbang`  | Flash grenade         | On/Off             | explosion folders, `flashbang` in the name. On = the MW2019 flashbang (`Generic_explo_flash` from ARC9 [Modern Warfare 2019]'s `mw2019_explosions_pak.pcf`: flash, sparks, debris, smoke shockwave and cloud). It is not a swap. CS2 stops its flashbang particle as soon as it creates it, which kills every child system. So On blocks the native flash, and `ParticleFxFlashbang.cpp` creates the MW2019 system at each `flashbang_detonate` position. Events replayed by a seek are skipped |
| `molotov`    | Nade Trails           | On/Off             | `weapon_molotov_thrown`, `weapon_incend_thrown`, plus a smoke-only rope trail spawned on `smokegrenade_projectile` (thrown-grenade flight trails only; molotov ground fire plays vanilla) |

**Dynamic Lights** is a three-cell row in Particle Packs: **Molly**, **Flashbang**, and **Grenade**. Each cell is an independent toggle. Highlighted means On. Clicking again switches it Off. Any combination is allowed, including all On or all Off. All cells default to Off. Molly supplies the held or thrown Molotov and ground-fire lighting described below. `mirv_filmmaker fx mollylights on|off` saves the setting. Enabling while paused uses the existing apply refresh to replay particle state. CS2 submits native particle lights on the next simulation frame, so a newly enabled paused light may need a brief resume. Pausing freezes the fire flicker. Switching off requests immediate emitter destruction. The light stays alive until its source ends, with irregular amber fire variation. A brief attachment grace period avoids black frames during pose updates. Every teardown writes zero native light energy before destroying the particle system, including extinction, seeks, and toggle or master Off, so stopped lights leave no wall glow. On also enables the Particle Packs master. Master Off and the card's Reset remove the additional lights. Molly does not change Nade Trails or swap the ground-fire visuals.

The light-only `particles/filmmaker/molly_light.vpcf` and `molly_area.vpcf` use CS2's native `C_OP_RenderOmni2Light`. They are driven by the shared Molotov source tracking. The sources are the lit held bottle's attachment, the flying Molotov attachment, and up to four real burning patches per inferno, each twenty-four units above the ground. Each patch has opposing rectangular area emitters for the floor and the surrounding wall. The emitters have a 64-unit face with 48-unit feathering, overlapping soft falloff, and a local 260-unit range. Held and flying bottles use smaller spherical emitters. Brightness follows the same irregular fire surge, with smaller per-patch variations, so overlap keeps visible flicker. The light works with native map lighting and does not require Environment.

Environment's relighting has its own VFX lights. This toggle does not enable that renderer or override the user's engine-light controls. Molotov sources use up to 32 native light systems. Detonation bursts share an overall 48-system cap. Sources fade in and out, follow demo time, and are cleaned up on seeks, master Off, demo exit, and level changes. The setting persists as `mollyLights` in `filmmaker_fx.json`. `fx state` reports `mollyLights` and `mollyLightsReady`. Diagnostics use `fx.molly.*`.

**Grenade** adds a warm native CS2 light at each `hegrenade_detonate` position. It fades over 0.30 seconds. **Flashbang** adds a pale light at each `flashbang_detonate` position, fading over 0.12 seconds. Both use soft spherical emitters with a bounded local range: 650 units for HE and 500 for flash. These lights are independent of the particle replacement modes, Environment, and Flash blindness. Pausing freezes the burst, and demo speed changes stretch its fade. Duplicate detonation callbacks and stale events replayed during seeks are rejected. Expiration, toggle Off, master Off, and seek cleanup all write zero light energy before releasing the native system.

`mirv_filmmaker fx helights on|off` and `fx flashlights on|off` control the HE and flash light cells. Settings persist as `heLights` and `flashLights` in `filmmaker_fx.json`, with matching fields in `fx state`. Card Reset switches all three cells Off. `fx.blast.*` diagnostics record accepted and skipped events, positions, brightness, and burst expiration. Flashbang uses the authored `particles/filmmaker/flash_light.vpcf`. HE reuses the warm `molly_light.vpcf` with its own detonation energy and lifetime.

Rebuild these authored assets with `powershell -File FX/tools/build-molly-light.ps1`. This needs CS2 Workshop Tools. The full FX converter includes `FX/native` before its runtime closure check and pruning, so full builds also keep the authored light.

`particles/ui/` (HUD and MVP effects) is always excluded. The gameplay cloud from CS2 smoke grenades is deliberately untouched, because it is a volumetric system and not a normal particle swap. When Nade Trails is On, the smoke projectile receives only the separate smoke-rope flight particle.

Modes (unsupported ones snap to On on the C++ side; the panel only offers the real ones):

- **On**: converted Insurgency 2 assets, except Bullet Impacts. Impact On alternates per hit between the generated 70% Insurgency variant and the exact reduced Povarhok `less/impacts/impact_fx` asset. Surfaces present in both packs receive both donors. Insurgency-only surfaces such as sand keep their only valid donor. Each ready per-surface wrapper plays CS2's original surface impact plus the selected donor. If the wrapper is not ready, the original CS2 impact plays while the wrapper is queued. The hook never substitutes a donor-only fallback. `fx.impact.wait` records these readiness misses. `fx.impact` identifies layered roots and their smoke policy. The 50/50 donor choice picks the custom look. It does not decide whether a ready mapped impact gets its custom layer. A persisted `less` setting normalizes to On.
  **Muzzle Flash & Shells On** keeps the Insurgency flash and its own smoke. It then layers the Povarhok *weapon smoke only* (`ac_muzzle_*_smoke*`, `weapon_muzzle_flash_smoke_*`) on top, with no flashes or sparks. That smoke is also attached to each flash's cheap `_b` fallback (and nested fallbacks). Sprays therefore still emit it every shot, instead of dropping to a pistol LOD with no Povarhok cloud. The layered smoke leaves emit at least one particle per shot. They drop CS2 screen-size fades, which cut the puff short when you walk backward into it. They hold full alpha for the first part of their life. They also get light gravity and `C_OP_RandomForce` drift, so they circulate instead of hanging in still air. Opacity is not lowered. The Insurgency `ins_burstspark` and `ins_muzzleflash_split` sprites emit 1.7× as many particles. The same sprites are attached to each flash's cheap `_b` fallback, so sprays still show them.
- **Modern**: the converted MW2019 pack. Class flashes follow the pack's own weapon Lua. Rifles use `muzzleflash_ar`, SMGs `muzzleflash_smg`, LMGs `muzzleflash_lmg`, and autosnipers the `mvm_muzzleflash_sniper_auto` composition around `muzzleflash_dmr`. The AWP and bolt snipers use the `mvm_muzzleflash_sniper_awp` composition around `muzzleflash_smg`. Pistols use `muzzleflash_pistol(_deagle)`, shotguns `muzzleflash_shotgun`, and silenced weapons `muzzleflash_suppressed`. Central sprite leaves use `fas_muzzleflash_test_b`. Their authored timing, alpha behavior, and frame-blend settings stay intact, and the retained `C_OP_PositionLock` keeps the center burst attached to the muzzle.
  Tracers map to the `mw2019_tracer` family. Rifles use `mw2019_tracer`, snipers `mw2019_tracer_fast`, pistols and SMGs `mw2019_tracer_small`, and shotguns `mw2019_tracer_slow`. HE maps to `explosion_grenade`, the actual detonation system of the pack's frag grenade. Systems with no modern equivalent pass through vanilla, so packs are never silently mixed.
  **Brass shell casings are MODERN-ONLY.** Modern renders its own casings under `particles/filmmaker/modern/weapons/cs_weapon_fx/weapon_shell_casing_*`. They point at the **`models/shells/mw2019/*`** meshes and have no `port_smoke` child. `patch_modern_shell_port_smoke` rebuilds them from the casing VPCF templates in the local `FX/sources/modern-warfare-gmod/templates/`. **On passes shell casings through to vanilla CS2.** Vanilla brass is already model-based and correct. Insurgency's `shell_eject` is a sprite ejection-port puff with no model renderer, so swapping to it would delete the visible casings rather than improve them. CS2 calibers without a per-pack counterpart use the nearest one (45acp and 5.7 map to 9mm, MAG-7 and Nova to the shotgun shell, AWP to the .50 cal). GMod's separate `DoEject` Lua effect has no CS2 top-level create to hook, so the eject puff rides as a PCF child of each Modern casing system.

  The post-process pass `apply_modern_gameplay_composites` (postprocess_modern.py) recreates the behaviors the GMod mod produced with **Lua** as composed PCFs. The pass is idempotent. To run it standalone on an already converted tree, use `--gameplay-composites-only` plus a per-file resourcecompiler pass:
  - **Barrel smoke timing = GMod AfterShotParticle emulation**: BOTH
    packs run every class's barrel smoke through spray-gated `mvm_spray_*` wrappers
    (emitted by `FX/tools/postprocess_modern.py` and gated in `ParticleFxHook.cpp` against the
    heat clock: Povarehok flashes incl. the AWP/hunting-rifle snipers, Modern class flashes +
    `_fp` twins, and the Modern `mvm_muzzleflash_sniper_*`
    compositions). **The GMod reference** (ARC9 base, read from the workshop Lua): barrel smoke is not per-shot. Every shot `StopEmission()`s the active
    smoke, and one new smoke spawns ~one RPM-interval AFTER firing stops
    (`sh_think.lua` AfterShot flag; attach = engine `PATTACH_POINT_FOLLOW` on the muzzle).
    **Our emulation** (the hook can only act at creation time; no StopEmission verb is
    RE'd for CS2. `CNewParticleEffect::StopEmission` exists in client.dll RTTI if ever
    needed): the hook upgrades only the shot that STARTS a burst (`SprayHeat.count == 1`,
    plus the `kSprayUpgradeCooldownTicks` ~2s re-arm guard bounding rapid-tap overlap),
    and the wrapper's smoke child carries `m_flDelay = AFTERSHOT_SMOKE_DELAY` (0.45s in
    postprocess_common.py), so taps and short bursts bloom their single wisp right
    after firing stops, exactly like GMod; long sprays get one wisp (blooming mid-spray
    rather than at the very end, which is the known deviation). Per-shot smoke children would stack plumes and double-smoke the snipers, so the sniper comps
    are smoke-less (flash + shock dust + heatwave) with the plume on their wrappers.
    (CS2's own `weapon_muzzle_smoke*` systems stay mapped too, but current CS2 is not
    observed creating them in demo playback.)
    Modern `barrel_smoke_trail(_b)` keep GMod's **`C_OP_RenderRopes`** (not sprites) with
    CS2 muzzle-tip offset and world-space drift tuning. Povarehok Less wrappers reference
    the 30%-reduced `less/smoke` plume while preserving the same flash. **Buoyant rise:** Modern rope wisps carry an upward gravity, and `_force_modern_smoke_rise` forces every barrel-smoke
    `C_OP_BasicMovement` gravity to a clean upward `[0,0,22]` (barrel_smoke_trail{,_b} and
    barrel_smoke_plume), matching Povarehok's `weapon_muzzle_smoke_long` rising plume
    (`[0,0,25]`). The muzzle-local spawn offset is untouched, so the wisp still starts at
    the barrel tip and only the post-spawn drift lifts upward.
    **Povarehok barrel smoke uses Modern's recipe:**
    `patch_cs2_muzzle_smoke_alignment` and `patch_cs2_muzzle_rope_trail_alignment`
    mirror Modern's barrel alignment instead of a viewmodel-space forward jet: world-space (`m_bViewModelEffect = false`, so third person / free cam see it),
    spawn at the shared barrel-tip offset (`MUZZLE_OFFSET_BARREL_TIP` in
    postprocess_common, same `[0,0,-0.5]..[0.5,0,0]` Modern uses; keep in sync with
    FxAlign.cpp's `kModernCfgOffset`), zeroed emission jets (the plume rises via +Z
    gravity instead of streaming 40-120 u/s forward), and a fast 0.1-0.15s fade-in. **Tight barrel tracking:** both packs' barrel
    smoke and rope wisps carry a FULL-lifetime `C_OP_PositionLock`
    (`ensure_full_position_lock`, start/end times 1e6). The engine default lock window
    (~1s) let already-spawned smoke stop following and drift behind the gun during
    reloads/weapon movement. The lock adds the muzzle control point's translation on
    top of the particles' own world-space rise/noise, so it does NOT bring back the
    local-space "glued noise" look that `_tune_modern_rope_trail_particles` handles.
    **De-clumped wisps:** `weapon_muzzle_smoke_long` has five overlapping ribbon/trace
    children (a broad `ac_muzzle*trail*` name match also attached
    `ac_muzzle_smg_trail{,5}` and `ac_muzzle_shotgun_trail`, which are short per-shot traces,
    not barrel wisps); the over-matched three are stripped
    (`WRONG_SMOKE_LONG_CHILDREN`), and the real wisp pair's spawn spread is tamed
    (sideways velocity noise is kept small so the rope's points do not scatter into a
    "crumpled clump").
  - **Smoke-motion recipe: stock CS2's own pattern.** Behaviour: smoke sits at the barrel while emitted but LAGS in the air like
    real smoke when the gun moves/reloads ("draws like a sheet with the motion of the
    gun"). That is exactly what Valve's `weapon_muzzle_smoke_long` does: world-pass
    rendering, newborn particles locked to the muzzle for 0.1s then free, and the
    engine continuously driving the instance's muzzle CP so emission tracks the gun
    (and a thrown weapon). World twins of all barrel smoke (wisps, plume, sustained
    smoke) use `ensure_brief_position_lock` and `m_bViewModelEffect = false`. **First-person reload follow:** muzzle flashes already
    ship world + `_fp` twins (`muzzleflash_*` / `muzzleflash_*_fp`); barrel smoke and
    rope wisps mirror that split (`barrel_smoke_fp`, `barrel_smoke_trail*_fp`,
    `weapon_muzzle_smoke_long_fp`, wisp `_fp` copies). `_fps` CS2 systems and
    `mvm_spray_*_fp` wrappers route to the viewmodel-pass smoke so wisps track the
    visible barrel during reload; world / non-`_fp` wrappers keep the world-pass assets
    for third person and chase cam (`g_fpFxSuppress`).
    **Rifle barrel wisp:** `_tune_modern_rope_trail_particles` sets `barrel_smoke_trail{,_b}` to hold, then fade over a ~3s life (`0.4`->`1.0`, lifetime `3.0`) at `m_nAlphaMax = 100`. The LMG/DMR `barrel_smoke_plume` fades over its full life at alpha 220.
    - **Emission and reload follow:** new smoke comes off the moving barrel as the gun sways or reloads, and the emitted rope drifts freely in the air rather than sitting as a rigid line on the muzzle. `make_modern_smoke_fp` provides this with a full-lifetime `C_OP_PositionLock`, world-space motion, and no control-point follow config.
      The full lock adds the engine-driven muzzle CP's translation on top of the particle's own rise and noise (it does not freeze it; see `common.ensure_full_position_lock`), so the wisp rides the barrel without going rigid. Both packs' `_fp` barrel smoke uses `make_modern_smoke_fp`.
      Muzzle-flash `_fp` twins (`make_modern_muzzleflash_fp`) are separate, since flashes are instantaneous. A wrapper upgrade per burst emits ~1.5s (trail) / 2.2s (plume), so a reload or inspect that starts after emission ends sees only the drifting tail. Bump the trail's `m_flEmissionDuration` if continuous muzzle smoke through a whole reload is wanted.
  - **MW2019 tracer wisp animation:** the `mgbase_tracer_trail{,_faint}`
    rope children animate in GMod by V-scrolling `fas_smoke_beam` along the ribbon.
    The converted files carry `m_flTextureVScrollRate = -50` (the field exists in
    the engine schema, confirmed via Misc/cs2-offsets), but with no explicit
    `m_flTextureVWorldSize` the scroll is imperceptible.
    `patch_modern_tracer_trail_scroll` sets `m_flTextureVWorldSize = 128` (one repeat
    per 128 units -> ~0.4 repeats/s of visible flow); idempotent, and runs in-place too.
  - **Povarehok plume uses the stock CS2 recipe:**
    `weapon_muzzle_smoke_long` is Valve's asset. Its 46-83 dark-gray rope renders **additively** (`PARTICLE_OUTPUT_BLEND_MODE_ADD`) with spread `[-10,-10,10]..[10,10,15]`. The alignment patch sets ADD and `m_flSelfIllumAmount = 1.0` only on `beam_smoke_01` renderers, because blanket ADD would square-frame translucent sheet smoke. The FAS wisp trails additionally run through Modern's own
    `_tune_modern_rope_trail_particles` so both packs' wisps are byte-equivalent in
    look and physics.
  - **Sniper shots** (AWP + autosnipers): the `mvm_muzzleflash_sniper_*` compositions add
    the pack's M82 treatment: `m82_shocksmoke` dust ring around the shooter,
    `barrel_smoke_plume`, and `muzzle_heatwave` heat distortion. **This is the ONLY place
    heat distortion appears** (mod-authentic scope).
  - **MW2019 tracer CS2 discipline:** routed parents get GMod lifecycle
    (`LifespanFromVelocity` + `Decay`, not `FadeAndKillForTracers`) plus CS2 motion fixes
    (`MoveBetweenPoints` @ 13k u/s, single emit, zero muzzle offset) while keeping MW2019
    textures. Automatic-weapon parent `mw2019_tracer` drops sniper-only children; glow/trail
    leaves halve alpha/radius/self-illum; parent `RenderTrails` use `m_flLengthFadeInTime`
    and head/tail alpha taper; rope trail children (`mgbase_tracer_trail*`) keep converted
    GMod `fas_smoke_beam` with V-scroll (scrolling smoke wisps along the beam).
  - **Grenade flight smoke trail**: CS2 has no in-flight particle for a smoke grenade. Nade
    Trails creates a dedicated smoke-only rope particle on `smokegrenade_projectile`; it does
    not reuse `particles/entity/spectator_utility_trail.vpcf`, which is an X-ray-only effect.
    The wrapper contains the lighter `mvm_nade_trail_smoke` fork of the Molotov rope; the
    Molotov keeps its darker flame-readable ribbon and its fuel/glow children. The deployed
    smoke cloud remains CS2's vanilla volumetric system.
  - **Heat distortion**: converted explosions do not carry vanilla's refraction children, and the pack's Source 1 refract quads are unrenderable and are stripped. The
    pass adds the STOCK CS2 distort systems (`explosion_hegrenade_distort`,
    `explosion_c4_distort01d_1k`) as children of the converted HE/C4 systems (both packs),
    and rewrites `muzzle_heatwave(_long)` as full behavior-12 systems cloned from the
    stock HE distort's renderer (refract keys + `warp_ripple3_normal` sampled in
    LUMINANCE mode + additive output). Bare
    `m_flRefractAmount` keys on the converted behavior-8 files compile but never
    take the refract shader path, so the warp normal map renders as an opaque
    purple/rainbow swirl. Color scale is `[0, 0, 0]`, not a warm tint: with `PARTICLE_OUTPUT_BLEND_MODE_ADD`, additive zero adds
    nothing, so only the refraction offset is visible (real heat-shimmer, invisible except
    where the background has detail to bend).    `muzzle_heatwave` is scoped to the sniper compositions only (see above). It is
    mod-authentic (no MW2019 Lua references it on any other class).

- **Off**: default CS2 pass-through for that category.

**HE Grenade and Bomb (C4) are separate rows** so the packs can mix, e.g. Povarehok's
`explosion_c4_500` on the bomb with the MW2019 frag explosion on HE. The
MW2019 pack's own C4 is a small breaching charge, so the bomb deliberately has no Modern
mode.

**Blood and Map ambience are not categories.** Blood: CS2's own blood is
plenty, so every name containing `blood` is left alone. It is classified before the folder rules,
or blood systems under `impact_fx/` would be taken for bullet impacts. Map ambience: the converted
packs ship no ambient effects, so the category has no swap table.

**HE On needs the stage step.** HE On resolves to Povarehok's `ac_grenade_explosion`,
which is compiled into the Povarehok output
(`build/fx/povarehok-source1import/source2/game/source_mvm_fx`) while the mounted pack is built from
`build/fx/fx-source1import/.../game`. `FX/tools/stage-legacy-he.py` (run by
`convert-fx-source1.ps1` after compiling) copies the HE closure, and any texture a
staged material names, into the pack. Without it the target is missing, the swap fails open, and
HE On plays vanilla; `FX/tools/verify-fx-pack.py` reports that as `ROOT NOT STAGED`.

**Money on Headshot** (`mirv_filmmaker fx moneyshot on|off`) swaps the headshot-only
particles (`blood_impact_headshot`, `impact_helmet_headshot`, ...) to the converted money
burst. There is no event gating; those systems are only ever created on real headshot hits (particles spawn before the
`player_hurt`/`player_death` events within the tick).

**Taser/Zeus is untouched by the variant tables**: its `weapon_tracers_taser*` wire
systems are not mapped, so they play vanilla.

**Variant tables must map top-level systems only.** Child systems are created internally by
the engine and bypass the hook. Verify what a demo actually creates with
`mirv_filmmaker fx names <filter>` and then add mappings only for those top-level names.

## How it works

### Code layout (`AfxHookSource2/Filmmaker/Movie/`)

The runtime is split into focused translation units behind one public header
(`ParticleFx.h`; shared internal surface in `ParticleFxInternal.h`):

| File                        | Responsibility                                                                                                                                            |
| --------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ParticleFx.cpp`            | Core: engine binding + demo state, the apply-now reseek, the public class methods, the main-thread pump, and the `fx` command dispatch.                   |
| `ParticleFxRules.cpp`       | Name classification into categories, the FXRULE variant/swap tables (vanilla → pack assets), per-mode target selection, swap-target pre-queueing.         |
| `ParticleFxHook.cpp`        | Vtable resolution + the create-collection detour + JIT-manifest target loading, and the spray-gated barrel-smoke decision (it reads the heat clock below). The crash notes live in its comments. |
| `ParticleFxPrefetch.cpp`    | Warm-up burst precache: queue-all, frame-budgeted drain, background disk prefetch.                                                                        |
| `ParticleFxMoney.cpp`       | Money-on-headshot candidates + game-event plumbing (`ParticleFx_OnGameEvent`).                                                                            |
| `ParticleFxSettings.cpp`    | JSON persistence to `%APPDATA%\HLAE\filmmaker_fx.json`.                                                                                                   |
| `ParticleFxDiagnostics.cpp` | FxDebugHud state, the `fx recent`/`fx names` telemetry ring, agent-log writers.                                                                           |

The spray-heat clock is produced once per frame in
`ParticleFx.cpp` (`PumpMainThread` stores the demo tick, or a synthetic one under `mvm_test`, into
the `g_demoTickNow` atomic declared in `ParticleFxInternal.h`) and read by the create detour in
`ParticleFxHook.cpp`. The producer must stay on the main thread: the hook runs off it and can only
touch the atomic, never the engine.

### The hook

One Detours hook on `particles.dll`'s `CParticleSystemMgr` create-collection body catches
particle instantiation:

1. `CreateInterface("ParticleSystemMgr003")` resolves the particle manager.
2. Slot 15 (`FindParticleSystem`) stays callable for resolving swap target names.
3. Slot 17's shared create body is detoured, including the internal direct-call path.
4. The hook reads the resource name off the handle, classifies it, and either passes it
   through or swaps it to a resolved target. Per-category Off is pass-through; explicit
   custom `fx block` rules still swap to `particles/dev/empty.vpcf`.

Swap targets are resolved on the main thread only. Resolving inside the create hook can
re-enter the resource system during particle creation, so the hook is cache-hit-only: an
unresolved target fails open once and gets queued for later resolution.

The pending queue is rebuilt from one authoritative desired-target set after settings or
level changes. A disabled master always publishes an empty target set and queue, including
when diagnostic logging explicitly installs the hook. Level changes invalidate all cached
handles and rebuild only currently enabled targets; level changes while Off leave the
queue empty.

"Level change" means the engine's RESOURCE CONTEXT changed, not that the user opened a
different demo. A demo can reload its own level mid-playback. A round-boundary
jump-back can reset the engine frame counter from
1164073 to 0, which frees every loaded particle definition while the demo path stays
identical. Cached handles are therefore keyed on a **resource-context epoch**, bumped from
the `LevelInitPreEntity` detour (primary; fires before the new level creates anything) and
from an engine-framecount-reset check in the pump (backstop). Every cache entry carries the
epoch it was resolved in plus the resource name read back off the handle, and both are
re-checked at use time. A mismatch fails open and requeues rather than handing the engine
a freed definition. Rejections are logged as `fx.stale`, epoch bumps as `fx.level`. Without
this check, an in-demo reload crashes the engine on the first post-reload swap (see the crash notes in
`ParticleFxHook.cpp`).

Non-precached targets are loaded through the engine's just-in-time manifest path instead
of the plain single-resource blocking load. That keeps dependency handles fixed up and
avoids the crash path when an unseen target is loaded mid-create.

Toggles apply to the current paused moment automatically. Settings changes request a
debounced one-tick-backward `demo_gototick`, which destroys live particles and replays the
recent event stream under the new rules. While the demo is playing, the automatic reseek is
skipped to avoid hitching; `mirv_filmmaker fx apply` forces it manually.

Limits: surface decals are not particles and never change; effects older than the last
demo full-packet are not replayed.

## Console reference

```text
mirv_filmmaker fx set <category> <on|less|modern|off> per-category control ('more' is an alias of on)
mirv_filmmaker fx on|off                             master switch
mirv_filmmaker fx state                              status + counters
mirv_filmmaker fx log on|off                         capture every creation
mirv_filmmaker fx recent [n]                         print last n captured/acted creations
mirv_filmmaker fx names [filter]                     aggregated per-name creation counts
mirv_filmmaker fx block <substr>                     custom rule: block names containing substr
mirv_filmmaker fx swap <substr> <target.vpcf>        custom rule: swap matching names
mirv_filmmaker fx unblock|unswap <substr>            remove custom rule(s)
mirv_filmmaker fx rules                              list custom rules
mirv_filmmaker fx test <name>                        dry-run the decision for one name
mirv_filmmaker fx apply                              re-create the current moment's live effects now
mirv_filmmaker fx moneyshot on|off                   event-gated money effect on headshot hits
mirv_filmmaker fx align on|off|report|clear          Modern muzzle-FX alignment probe (Source units)
mirv_filmmaker fx align gate on|off                  spray-wisp heat gate (off = every shot wisps, TESTING ONLY)
mirv_filmmaker fx align threshold <units>            per-sample pass distance (default 2.5)
```

Tuning workflow: `fx log on`, play the moment, inspect `fx names`, then adjust the variant
tables in `ParticleFxRules.cpp`. Runtime events also mirror into the `mvm_debug` log
(`fx.create`, `fx.install`, `fx.event`, `state.fx`).

## Automated coverage check

`Automation/verify/verify-fx-allweapons.ps1` replays an every-weapon test demo (default:
`all weapon test .dem` in game/csgo) once per mode profile (`-Profiles modern,less` by
default), with `fx log on`. Before playback it also verifies lazy-precache state behavior:
all-Off produces a zero queue, selecting one category/pack auto-enables the master without
changing other modes, repeated/switch selections do not grow stale work, master Off
cancels the queue, and Money/custom rules opt in automatically. From the hook's own
`fx names` counters it then asserts each
expected group: muzzle flashes (incl. silenced), tracers, sustained-fire muzzle smoke,
impacts, HE, bomb, and molotov was both created by the demo (seen) and swapped (acted), and
writes `unmapped-<profile>.txt` listing weapon-path systems nothing acted on (the feed for
new variant-table entries, e.g. thrown-grenade trails). Periodic screenshots land in the
run folder for visual review (white quads, trail-follows-grenade, muzzle alignment).
Requires CS2 up via `Automation/launch/launch-cs2-netcon.ps1` with the pack mounted.

## Muzzle alignment check (Source units, not pixels)

`Automation/verify/verify-modern-muzzle-alignment.ps1` verifies that the Modern pack's
muzzle flash / first-shot barrel smoke / sustained wisp spawn AT the weapon muzzle, using
the in-game probe `mirv_filmmaker fx align` (`Movie/FxAlign.cpp`) rather than a
screenshot pixel audit. The probe covers BOTH packs: `EffectKindFor`
classifies `/modern/` (muzzleflash_*, barrel_smoke*, mvm_spray_*) AND `/povarehok/`
(weapon_muzzle_flash_*, weapon_muzzle_smoke*). Per swapped creation the hook queues the
vanilla+target names and the engine's returned collection; the main-thread pump resolves
the muzzle attachment and the spawn point.

**First-person viewmodel muzzle resolution.** The reference is the
FIRST-PERSON viewmodel weapon's `muzzle_flash` attachment (what a `m_bViewModelEffect`
particle actually anchors to), resolved via `ResolveViewmodelWeaponEntityIndex`
(CosmeticModelSwap). The first-person viewmodel weapon is class
`C_CS2HudModelWeapon` (NOT the world weapon class), a CLIENT-ONLY entity at index >= 0x4000,
so the resolver does not match on class. It returns the entity POINTER, and the
attachment read is SEH-guarded (`SehReadAttachment`) because the client viewmodel entity can
be mid-reconstruction during live playback. Use
`mirv_filmmaker fx align vmprobe` to dump, for the spectated player, the world vs viewmodel
muzzle side by side (confirms which reference resolved and the delta).

**The `cp-scan` absolute distance is not the effect's true offset. Do not tune against it.**
cp-scan takes the nearest world float3 in the collection header to the muzzle. It never finds CP0: Modern reports a near-constant ~18u fingerprint per weapon (same
relative vector regardless of weapon = a fixed structural point ≈ the weapon origin, NOT the
emit point; even instantaneous silenced flashes read 8-13u where CP0 would be ~0), and
Povarehok reports 100-330u with std up to 150u (it grabs the WORLD-DRIFTED smoke rope points,
which lag into the air by design). Per-axis std dev is the only relative signal that stands
out: Modern **shotgun** wisp has both the highest median (30.8u) and the highest variance
(std 13/23/18u vs ~5u for every other weapon).

**GROUND TRUTH = the authored `C_INIT_PositionOffset` (static, in the `.vpcf` files).** The
effect spawns at CP0 (engine-driven to the viewmodel muzzle, proven by the 100% viewmodel
reference resolution + the `_fp` `PATTACH_POINT_FOLLOW` "muzzle_flash" config) PLUS this
authored local offset. Extracted for every FP file, both packs:
- **Modern**: every FP muzzle flash = local `[0,0,-0.5]..[0.5,0,0]` (≈0.25u forward/down of
  the muzzle CP); barrel smoke `_fp` = the same or no offset at all (spawns exactly at CP0).
  All roots carry `m_bViewModelEffect=true`; smoke carries the follow config.
- **Povarehok**: FP flashes mostly have NO PositionOffset (spawn exactly at CP0); FP muzzle
  smoke = `[0,0,-0.5]..[0.5,0,0]` (≈0.25u). Roots carry the VM flag; `weapon_muzzle_smoke_long_fp`
  is the follow-tracked one.
So BOTH packs author every first-person muzzle flash and barrel smoke to spawn **≤0.5u from the
viewmodel muzzle**, meaning at the barrel, by construction. Any perceived "a little in front" is
the `muzzle_flash` attachment's own model position (barrel tip, forward of the receiver) or the
smoke sprite's visual RADIUS (a 3-8u puff extends forward of its centroid) or Povarehok's
intended world-space drift, not a spawn-offset error.

**Child systems inherit the viewmodel pass from their parent root:**
Modern's confirmed-correct `muzzleflash_ar_fp` has 10 children that ALL lack
`m_bViewModelEffect`, only the root has it, and it aligns correctly, so a child without the
flag is normal/correct in BOTH packs (Povarehok's flash children lacking the flag is not a bug).

To read the AUTHORED offsets, parse `C_INIT_PositionOffset` `m_OffsetMin/Max` from the text
`.vpcf` under `build/fx/povarehok-source1import/source2/content/source_mvm_fx/particles/filmmaker/`.
The runtime probe remains useful only for the REFERENCE check (does the viewmodel muzzle
resolve) and relative std-dev outliers, not for absolute placement.

Samples land as NDJSON in `%APPDATA%\HLAE\fx_align.jsonl`; the harness aggregates per
weapon-class/effect and fails on mean distance > threshold (default 2.5 units), pass-rate
< 80%, or missing required coverage. During a run the spray-heat gate is bypassed
(`fx align gate off`) so single shots still produce wisp samples for every class. The
harness always restores it (`gate on`) in teardown. NOTE: the `all weapon test .dem` is
~6:18 long; a capture that only runs ~60s covers just the first weapons. Play the full
demo per pack for complete coverage.

## Where weapon FX rules come from (read before adding one)

**Never map a rule from `scripts/items/items_game.txt`.** Its `muzzle_flash_effect_*` values name
systems under `particles/weapons/cs_weapon_fx/` that CS2 does not spawn. A rule
written against them looks correct in review and does nothing in game.

What spawns is decided by the **weapon model**, and it is always a `particles/unified_weapon_fx/`
system. The .vmdl files show that the Dual Berettas use the ordinary
`uweapon_muzzleflash_pist`, the MP5-SD uses `uweapon_muzsilenced_subm`, the SSG08 shares the AWP's
`weapon_muzzleflash_snip`, and the XM1014 uses `uweapon_muzflsh_shot`. There is no per-weapon
"elite"/"autoshotgun"/"ssg08" system to map; `weapon_muzzleflash_ssg08.vpcf` and
`uweapon_muzsilenced_p90subm.vpcf` ship in the VPK but are unused.

Ground truth comes from `Automation/verify/fx-weapons-go` against
`replays/repaired/all weapon test .dem`: `fx-names-<profile>.txt` is what CS2 really spawned and
how often we acted on it, and `unmapped-<profile>.txt` is the real candidate list. Caveat: that
demo does not fire every weapon (no SSG08, dualies or silenced-P90 variant), so absence from it is
not evidence of a gap.

### The Zeus is never swapped

Its wire effects are named `weapon_tracers_taser*`. `ClassifyLower` matches the substring "tracer",
so without an exclusion these wires would be classified as bullet tracers and replaced by a generic
bullet streak in **both** On and Modern. Neither pack ships a taser asset, so every `taser` system is excluded outright in `ClassifyLower`.

### Shell casing scale

CS2's own casings all use `m_flConstantRadius 0.75` and bounce `0.5`: one radius for every
caliber, with the MESH carrying the size difference. **That does not transfer here**: the
converted packs render their own meshes, which are not authored at consistent relative scale,
so each caliber needs its own radius. Each caliber therefore keeps its own radius. The 9mm radius is `2.00`: below the .50 AE deagle at `2.61` (both use the same `shell_pistol.vmdl` mesh), and above `0.95`. A 9mm case is physically smaller than a .50 AE, so the 9mm value stays below the deagle's.

Bounce is uniform across calibers, because the mesh-independent part of the casing behavior transfers. `postprocess_common.normalize_shell_casings` owns both. It lives in
`postprocess_common.py` rather than the Modern postprocess because both packs inherited the
same numbers.

### Soft particles (depth feathering)

A billboard is a flat quad, so where it intersects world geometry it ends in a hard straight line
across the wall or floor. It is worst exactly where it matters, on big explosion and smoke sprites in a
cinematic shot. Source 2 solves this with depth feathering: the renderer compares each pixel
against the depth buffer and fades the sprite out near the surface.

Measured across CS2's own `particles/explosions_fx` (319 decompiled systems), Valve sets
`m_nFeatheringMode = "PARTICLE_DEPTH_FEATHERING_ON_OPTIONAL"` on 175 renderers,
`ON_REQUIRED` on 19, and the explicit distance is always `m_flFeatheringMaxDist = 8.0`. The
unpatched converted packs have **no feathering key at all** across their 575 sprite and 136 trail renderers,
so every one hard-clips. `postprocess_common.add_depth_feathering` adds Valve's ON_OPTIONAL + 8.0
("optional" feathers when a depth buffer exists and renders normally when it does not, which is
both the safe choice and Valve's own default).

**Do not apply a blanket feather.** Feathering *every* sprite/trail renderer causes a worse artifact: the sparks and the flash show black boxes at certain lighting.

Feathering makes the renderer fade against the depth buffer. On a system never authored for it, the
fade lands as a hard cut that **tracks scene geometry**, so the flash and sparks show angular
translucent panels whose edges follow the walls behind the shot, appearing and vanishing with
what is behind the player.

Source 1 states the intent per material: `$depthblend 1` = soft particles, `$depthblend 0` = feathering
**off**. A blanket pass would override 50 muzzle renderers that say `0` or nothing,
including `particle_muzzleflash4` / `_x` / `_2` (the flash cores, all explicitly `0`) and
`particle_spark` / `sparkwires` / `sparktrail` / `bb2_randomsparks_3` (the sparks).

The pass takes `source1_root` and follows the authored value: feather where `$depthblend 1`,
**strip** where the material says `0` or is silent, leave alone when no VMT can be found. In the
Povarehok weapon closure that is 23 kept and 50 stripped. Exposed as `--feathering-only`.

Trade-off, stated plainly: materials that never authored `$depthblend` lose the anti-clipping
mitigation this pass addresses. That is the right default. They hard-clip in Source 1
too, and the big smoke/explosion sprites that have the clipping problem (`smoke1`,
`fire_particle_4`) authored `$depthblend 1` and keep it. If wall-clipping returns, fix those
systems specifically rather than applying a blanket feather.

**Nade + impact wall smoke.** HE On is Povarehok `ac_grenade_explosion`, which
lives outside the Insurgency/Modern namespace walk, so `add_depth_feathering` never sees it.
Hybrid impact smoke is copied in as precompiled `_c` from the same pack, also unfeathered.
Those big puffs (radius 70 to 170) still hard-cut walls and floors at Valve's 8-unit fade.
`FX/tools/feather-nade-impact-smoke.py` force-feathers only smoke/dust sprite and trail
renderers in the HE On/Modern closures and the two impact-fx folders. Distance scales with
radius (24 to 48). Sparks, flashes, and muzzle systems stay on the $depthblend pass. Runs after
impact-smoke variant generation so the generated copies get it too.

**Modern HE white smoke streaks.** GMod `explosion_grenade` uses
`render_sprite_trail` on `fas_smoke_trail`. Source 1 scrolls that wisp texture along the
ribbon; source1import stored the rate as CS2 sheet playback on a 3-sequence, 1-frame-each
sheet, so the streaks only thickened and jittered. `FX/tools/animate-modern-he-trails.py`
rebuilds sequence 0 as a 3-frame flipbook, plays it across the particle lifetime, and adds
the same V-scroll the MW tracer wisps use. Sparks and shotgun trails that are not this
renderer stay untouched.

### Tracers must die where the bullet stopped

`C_INIT_LifespanFromVelocity` is what makes a tracer stop at geometry: it traces ahead
(`m_flMaxTraceLength`, 8192 here) and derives a lifespan expiring where the trace hits. CS2's own
tracers use the same idea plus `C_OP_FadeAndKillForTracers`. Notably, they do not use world collision, so
adding a `WorldTraceConstraint` would have been the wrong fix.

Some converted Modern tracers also carry `C_INIT_RandomLifeTime` at a flat 1.0s, **later in the
initializer list**. Initializers run in order, so the traced lifespan is computed and then
overwritten by the constant. At tracer speed (Valve uses 30000 u/s) one second is up to 30000
units of travel, through the wall and across the map, which lets bullet trails go through
walls. `postprocess_common.fix_traced_lifespan_clobber` drops the later constant so the
trace stands. Deliberately scoped: only systems with BOTH, and only where the constant comes
second (4 systems, all `modern/mw2019_tracer/`). A flat lifetime alone is legitimate; it is only
wrong when it silently defeats a trace.

### No eject-port smoke on Modern casings

Modern casings have no `port_smoke` child. The reason is control-point inheritance. `port_smoke` sets `m_nControlPointNumber = 0`, and as a
CHILD of the casing system it inherits the PARENT's CP0 (the weapon origin), not the casing's
ejection position. On a viewmodel CP0 sits at the grip/receiver, so the puff would draw at the
grip rather than where the shell leaves the gun.

The muzzle flashes carry their own `muzzle_smoke_*` children, which are anchored to the muzzle.

### Casings must come to REST (not just bounce less)

Lowering `m_flBounceAmount` to CS2's 0.5 does not make casings settle, because restitution is not the whole story.
Against CS2's own `weapon_shell_casing_9mm` the casings already match on gravity (-600) and
drag (0.05). What they lacked entirely, on the same `C_OP_WorldTraceConstraint`, is the STOP
condition: `m_flStopSpeed = 24.0` and `m_nStickOnCollisionField = 30`. Without them, 0.5
restitution halves the speed at every impact but never reaches zero, so a casing hops
indefinitely.
`postprocess_common.stop_casing_bouncing` adds both to every casing in every pack.

### Weapon muzzle smoke: per-family target table

**The white-out.** `tone_down_muzzle_closure` deliberately **skips** any child matching
`(smoke|wisp|puff|plume)` so the flash's `alpha=0.45 / overbright=0.35` never crushes smoke toward
black. Smoke therefore has no ceiling of any kind. The `ac_muzzle_*` clouds reach radius 8-10 and last up to **2.5 seconds**.
At rifle cadence a spray stacks ~20 of them before the first expires, which causes the AK-47 white-out.

The brief flash puff (`weapon_muzzle_flash_smoke_*`, 0.1-0.2s life) is *not* the culprit and
capping it alone does nothing. Alpha does not help either: `soften_weapon_smoke` already takes it from 60 to 36, and the wall remains. **Screen coverage x persistence** is what blinds you.

Blanket ceilings cannot express per-family differences, such as a shotgun keeping a big slow cloud while the AK does not, and they only push values **down**. `MUZZLE_SMOKE_TUNING` is an explicit per-family table of **absolute
targets**, applied by scaling a block's min/max pair so the max lands on the target, which
preserves the authored spread, moves values up as well as down, and stays idempotent.

| family | radius | lifetime | growth | overbright | alpha |
|---|---|---|---|---|---|
| `ac_muzzle_shotgun*smoke` | 10.0 | 2.5s | 3.0x | 2.2 | 65 |
| `ac_muzzle_sniper*smoke` (AWP/auto) | 10.0 | 0.7s | 3.0x | 2.2 | 65 |
| `ac_muzzle_357*smoke` (AK/AUG/M4) | 6.5 | 0.7s | 2.4x | 1.7 | 32 |
| `ac_muzzle_smg*smoke` (UMP) | 6.0 | 1.7s | 2.3x | 1.6 | 45 |
| `ac_muzzle_pistol*smoke` | 6.0 | 1.7s | 2.3x | 1.6 | 50 |
| `weapon_muzzle_flash_smoke_medium` | 10.0 | (unchanged) | 3.0x | - | 150 |
| `weapon_muzzle_flash_smoke_*` | 6.0 | (unchanged) | 2.4x | 1.7 | 48 |

Rifle persistence (radius x growth x lifetime) is **10.9** with these targets, against 52.5 for the Source 1 default.

Shotgun clouds keep their larger, slower values. Flash-puff lifetimes are left unchanged because they are meant to be instantaneous.

The Povarehok Source 1 tree is staged at `build/fx/povarehok-source1import/source1_game` (35 `.pcf`, 1273 `.vmt`), and re-running `convert-fx-source1.ps1` regenerates the source values.

**Scoped by name, not by closure.** `MUZZLE_ROOTS` is first-person only (every entry is `_fp`), so
a closure walk silently misses world-only systems: `weapon_muzzle_flash_smoke_small4` is reached
only from `weapon_muzzle_flash_smg.vpcf` and friends, and third person is exactly where the UMP
problem shows up. Exposed
in-place as `--muzzle-smoke-only`.

A `C_INIT_CreationNoise` block with no `m_nFieldOutput` defaults to the **radius** field. That is
where the flash puffs get their real radius, not the file-scope `m_flConstantRadius`, which the
noise overwrites. Blocks naming an explicit field drive something else and are left alone.

### Auto-sniper smoke shares the shotgun's systems: fork, do not retune

"Make the auto snipers' smoke disappear at .7 secs too" looks like a one-line tuning change and is
not, because **auto snipers have no dedicated asset in the mod**. `ParticleFxRules` maps
`weapon_muzzleflash_snip_ar(_fps)` onto `weapon_muzzle_flash_awp`, and that flash's lingering cloud
is `ac_muzzle_shotgun_smoke` / `_smoke_b`, the shotgun systems kept at their 2.5s Source 1
dwell. Retuning them in place would silently change the shotgun.

`fork_sniper_muzzle_smoke` seeds `ac_muzzle_sniper_smoke(_b)` from the shotgun systems and repoints
**only** `weapon_muzzle_flash_awp` at them; `ac_muzzle_shotgun` keeps the originals. The two then
tune independently through `MUZZLE_SMOKE_TUNING`. They have identical values except lifetime.
The seed copy happens once (guarded on the fork not existing), because
re-copying every run would clobber the sniper tuning with the shotgun's.

**Check the sharing graph before retuning any `ac_muzzle_*` system.** Several are shared across
weapon classes in non-obvious ways: `weapon_muzzle_flash_smoke_small` alone is used by the pistol,
para/LMG, AWP, hunting rifle and silenced-rifle flashes. A quick reverse-reference grep costs
nothing and prevents a fix for one weapon from regressing another.

**Scope:** this covers the AWP as well as the auto snipers, because ON mode resolves both
`weapon_muzzleflash_snip` (AWP) and `weapon_muzzleflash_snip_ar` (auto snipers) to the same system.
Separating them needs a new mapping target in `ParticleFxRules.cpp` and a DLL rebuild, not a
particle change. The Scout/SSG08 (`weapon_muzzle_flash_huntingrifle_*`) also shares the shotgun
smoke and is not changed by this fork.

### The LMG muzzle flash spawns off the barrel tip

"The muzzle flashes don't align on the LMGs" (ON mode).

Povarehok's LMG flash keeps Source 1's offsets, and unlike every other weapon in the pack they are
a wide **spread** along the barrel axis rather than a fixed point:

    weapon_muzzle_flash_para_flash   m_OffsetMin = [-1.0, 0, 0]   m_OffsetMax = [7.0, 0, 0]
    weapon_muzzle_flash_para_b       m_OffsetMin = [-2.0, 0, 0]   m_OffsetMax = [-2.0, 0, 0]

Sprites spawn anywhere from 2 units behind the muzzle to 7 in front, which on the M249/Negev's long
barrel reads as a flash floating off the end of the gun. Every other Povarehok flash uses a tight
fixed offset (`weapon_muzzle_flash_assualtrifle_flame` is `[1.0, 0, 0]` on both ends) and aligns.

**ON mode only.** Modern's `muzzleflash_lmg(_fp)` already carries the CS2-calibrated
`MUZZLE_OFFSET_BARREL_TIP` (`[0,0,-0.5]` to `[0.5,0,0]`), byte-identical to its own
`muzzleflash_ar`, so Modern needs no change and gets none.

`align_lmg_muzzle_flash` clamps only the **outliers** into `[0.0, 2.0]`, the range the rest of the
pack uses, rather than forcing Modern's CS2 calibration onto Povarehok, which would shift every
LMG flash sprite and change the pack's look rather than just its alignment. `_main` and `_glow`
already sit at `[1.0, 0, 0]` and are untouched. Exposed in-place as `--lmg-flash-only`.

`patch_cs2_muzzle_smoke_alignment` in `postprocess_povarehok.py` is
defined but never called from anywhere. It is dead code.

### The muzzle flash renders a dark box around itself

Pistols and the AUG can show a black box around the flash in ON mode (first person). Modern does not.

Source 1's `$addself`. Povarehok's flash cores are **not** additive materials; they are
alpha-blended sprites that lean on a strong self-add to lift their own dark background:

    materials/particle/fire_particle_4/fire_particle_4.vmt
        SpriteCard { $addself .75   $overbrightfactor 3.5   $DEPTHBLEND 1 }

Scaling `m_flAddSelfAmount` by the `overbright` factor (0.35) would take
0.75 -> 0.2625. That is a category error: **`$addself` is a blend control, not a brightness dial.**
At 0.26 the sprite composites as a plain alpha-blended fire texture, so the dark, low-alpha region
of the sheet is drawn over the scene as a visible dark quad.

**Why Modern never shows it:** the ARC9 flash cores are authored `$additive`
(`muzzleflash_ar` and friends resolve to `hl2_muzzleflash_add.vmat` and carry
`PARTICLE_OUTPUT_BLEND_MODE_ADD`), and under ADD a dark background is mathematically invisible.
Modern's only non-additive muzzle systems are smoke/dust. Povarehok mixes the two, so only
Povarehok can show the artifact. Diagnostic worth reusing: **a dark box can only come from a
non-additive renderer**. ADD blending cannot darken anything.

`restore_muzzle_addself` reads `$addself` back out of the original Source 1 VMT (comments stripped
first, since this pack has `$additive`/`$addself` inside commented-out lines and matching those raw would
mis-read them) and writes it to the renderer, inserting the key when the converted output lacks it. `tone_down` does not touch the self-add; it tones overbright and alpha, which
are brightness controls. The 9 flash cores are set to `0.75`. Exposed as `--flash-addself-only`, and
it needs `--source1-root` since it reads the VMTs.

### Auto-exposure reveals the sprite quad (transparent-pixel colour bleed)

CS2 auto-exposure also applies to effects, and then the bounding quad of each PNG sprite is visible.
Source 1 sprite sheets are authored with **colour bleed**: fully transparent texels still carry RGB,
usually the edge colour smeared outward, so bilinear filtering never pulls black into the visible
edge. That is correct authoring, it is invisible under ordinary alpha blending (alpha is 0, so it
contributes nothing), and **58 of the 109 textures these packs reference have it**. Leave it alone
in general.

It stops being invisible the moment a renderer ADDS the texture regardless of alpha:
`PARTICLE_OUTPUT_BLEND_MODE_ADD`, or a SpriteCard `$addself`. Every transparent texel then adds its
leftover colour, the whole quad lifts by a constant, and CS2's auto-exposure decides whether that
constant is above the visible threshold. So the rectangle appears indoors and vanishes outside.

Measured in the muzzle path: `muzzleflashcloud` and `particle_glow_04` at RGB **255** in fully
transparent pixels, `fire_particle_4` at 99.

`zero_additive_transparent_rgb` zeroes RGB in `alpha == 0` texels, for the 21 textures an
additive/addself renderer consumes. Partially transparent texels keep their colour, so the bleed
still works where it matters and no black fringe is introduced. Marker-guarded (`.noleak`, mtime
compared) like the neighbouring premultiply pass; changed textures need a resourcecompiler run, so
**the build must compile `materials\*.vtex` as well as particles**. Exposed as `--texture-leak-only`.

**This interacts with `restore_muzzle_addself`.** `$addself` is the multiplier on the leaked colour, so the
authored 0.75 makes the leak more visible. Both fixes are needed.

Note the resolver detail: `_vtex_frame_tgas` returns the **sheet frame** TGAs
(`fire_particle_4_seq0_*.tga`), not the base image of the same name. Verify the frames, not the
base.
### Deagle casing eject direction

Every converted casing throws brass along +X in the ejection port's local frame. The Deagle is the exception:

    weapon_shell_casing_9mm      Min [ 120.0, -15.0, 50.0]   Max [ 150.0, 5.0, 75.0]
    weapon_shell_casing_deagle   Min [-120.0, -15.0, 50.0]   Max [-150.0, 5.0, 75.0]

The Deagle values are the 9mm values with X negated, and the range is inverted as well (min -120 is
greater than max -150). Both are the signature of a conversion sign error rather than authored
intent. A real Deagle ejects right like every other pistol, and nothing else in the pack throws
brass backwards. `fix_casing_eject_direction` flips the axis and restores min <= max, but only for
casings whose X range is **entirely** negative, so a genuinely mixed range is never guessed at.
The fix applies in all three packs. Exposed as `--casing-direction-only`.

### SCAR-20 flash sits in front of the gun

`weapon_muzzle_flash_sparks7` spawns across `[1.0 .. 15.0]` units along the barrel axis where the
pack norm is 1-2. It is the same class of bug as the LMG flash above, so it is handled by the same
clamp to `[0.0, 2.0]`. It is used *only* by `weapon_muzzle_flash_awp`, so the shotgun cannot be
disturbed.

**What cannot be fixed here:** the AWP, SCAR-20 and G3SG1 all resolve to that one flash system, and
both auto-snipers emit the same CS2 particle name (`weapon_muzzleflash_snip_ar`), so they are
indistinguishable even at the mapping layer in `ParticleFxRules.cpp`. Moving the sparks to the
barrel tip necessarily moves them on all three. This is a deliberate trade.

`weapon_muzzle_flash_smoke_medium` is the other outlier on that flash (8.0 forward) and is
deliberately **not** clamped. It is shared with `weapon_muzzle_flash_shotgun`, which is kept at its authored values.

### Per-weapon FX: separating weapons that share one flash system

Separate a weapon when a fix is needed for that weapon alone.

Several weapons share one muzzle-flash system, so tuning it for one moves the others. Tuning
the SCAR-20's spark offset alone would also move the AWP and G3SG1. There are **two** situations and they are fixed in different places; check which one
applies before writing any code.

**1. The weapons emit different CS2 particle names.** `ParticleFxRules.cpp` can already separate
them; the rule points at a shared asset. The **Deagle** is this case:
`uweapon_muzflsh_deagle` is its own name, and Modern routes it to a Deagle asset, while ON
resolves to the generic `weapon_muzzle_flash_pistol` (its offsets suit a 9mm slide, not a longer barrel). This is a one-line rule change with no runtime work.

**2. The weapons emit the same name.** Nothing in the particle can tell them apart:

    SCAR-20 / G3SG1  -> weapon_muzzleflash_snip_ar
    M249 / Negev     -> uweapon_muzflsh_mach

`PerWeaponFx` (`Movie/PerWeaponFx.{h,cpp}`) resolves the **firing weapon** at swap time and
redirects to a fork. Adding a weapon is one table entry:

    { "weapon_muzzle_flash_para.vpcf", "weapon_m249", "_m249" },

Two things that will bite anyone extending this:

- **Match on `DesignerClass`, never `EntityClass`.** `DesignerClass` is `GetClassName()` and gives
  `weapon_scar20`. `EntityClass` is `GetClientClassName()`, and nearly every CS2 gun reports the
  same `C_CSWeaponBaseGun`. A rule keyed on it compiles fine and then silently never matches.
- **Sample on the main thread.** The particle hook resolves swaps on whatever thread creates
  particles, where entity access is not safe, so `PerWeaponFx_SampleMainThread` caches the weapon
  from `ParticleFx::PumpMainThread` and the swap only reads the cache.

**The asset fork has to be DEEP.** `fork_per_weapon_flash` in `postprocess_povarehok.py` handles
this. A flash root like `weapon_muzzle_flash_pistol` is a pure composition. The offsets that need
changing live several levels down, in children shared with every other pistol. Copying only the
root yields a fork that is byte-identical in behaviour. The pass forks every system on the path from root to a tweaked leaf and rewires
each fork's child references, leaving everything off that path shared.

Fork naming is always `<stem><suffix>.vpcf`, matching what `PerWeaponFx_Override` builds at
runtime, so the rule and the asset cannot drift apart.

| | shared (unchanged) | fork |
|---|---|---|
| sniper sparks `weapon_muzzle_flash_sparks7` | 15.0 (AWP, G3SG1) | **2.0** (SCAR-20) |
| pistol flash `ac_muzzle_pistol_center_b` | 2.0 (all 9mm) | **3.5** (Deagle) |

The Deagle's 3.5 is a reasoned starting value from barrel length, not a measured one. It is a
single number on an asset nothing else uses, so it can be adjusted freely.

Exposed as `--per-weapon-forks-only`. Note `less/impacts` carries **no** weapon FX (0 files), so
forks legitimately exist only under `regular` and `less/smoke`.
