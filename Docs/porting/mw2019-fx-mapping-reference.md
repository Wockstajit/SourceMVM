# MW2019 (GMod ARC9) FX Mapping Reference

This document covers how the GMod addon **ARC9 Modern Warfare 2019** maps muzzle flashes, barrel
smoke, tracers and explosions onto its weapons. The mappings come from reading the addon's own Lua.
They support the variant tables in `AfxHookSource2/Filmmaker/Movie/ParticleFxRules.cpp`. See
[filmmaker_effects_modifiers.md](../features/filmmaker_effects_modifiers.md) (`FxMode::Modern`) for
how the mapping feeds the DLL.

**Sources read** (GMod workshop content, already extracted next to their `.gma` via gmpublisher):

| Layer                    | Workshop ID  | Content |
| ------------------------ | ------------ | ------- |
| ARC9 Weapon Base         | `2910505837` | Base Lua and particle declarations |
| ARC9 Modern Warfare 2019 | `3258297368` | Weapon definitions |
| ARC9 MW2019 Shared Pack  | `3258299652` | Models/sounds/materials only; no particles |

## 1. Two-layer addon architecture

MW2019 does not reinvent the firing and FX pipeline. It is a content pack that sits on the generic
**ARC9 Weapon Base**, which owns the class-based fallback particles and the code that fires effects.

| Layer                               | What it owns                                                                                                                                                                                              |
| ----------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **ARC9 Weapon Base** (`2910505837`) | Generic firing pipeline + class-based fallback particles (`particles/arc9_fas_muzzleflashes.pcf`, `arc9_fas_explosions.pcf`). Defines every field a weapon can set.                                       |
| **ARC9 MW2019** (`3258297368`)      | Adds `mw2019_effects.pcf`, `mw2019_tracer.pcf`, `mw2019_rockettrail.pcf`, `mw2019_explosions_pak.pcf`, and one Lua file per weapon that overrides the base pack's fallback values with COD-specific ones. |

MW2019 weapons set a handful of string fields (`MuzzleParticle`, `AfterShotParticle`,
`TracerEffect`) on top of the base pack's generic effect system. They do not define new effect code.

## 2. The firing pipeline

```
SWEP:PrimaryAttack()
  └─ SWEP:DoEffects()                     [ARC9 Weapon Base: weapons/arc9_base/sh_effects.lua]
       reads self:GetProcessedValue("MuzzleParticle")
       └─ util.Effect("arc9_muzzleeffect", data)
            └─ CreateParticleSystem(muzzleDevice, MuzzleParticle, PATTACH_POINT_FOLLOW, att)
                 [effects/arc9_muzzleeffect.lua — attaches to the barrel/suppressor bone]

  └─ SWEP:DoEject()  → util.Effect(ShellEffect or "ARC9_shelleffect")
       [effects/arc9_shelleffect.lua — physical shell prop + "port_smoke"/"shellsmoke" puff]

  └─ util.Effect("arc9_aftershoteffect")  [effects/arc9_aftershoteffect.lua]
       reads self:GetProcessedValue("AfterShotParticle")   ← this is the barrel smoke
       └─ CreateParticleSystem(muzzleDevice, AfterShotParticle, PATTACH_POINT_FOLLOW, att)

  └─ util.Effect(self.TracerEffect)        e.g. "cod2019_tracer_fast"
       [effects/cod2019_tracer_fast/init.lua]
       └─ CreateParticleSystem(weapon, "mw2019_tracer_fast", PATTACH_ABSORIGIN, att)
            :SetControlPoint(0, muzzlePos)   -- CP0 = start
            :SetControlPoint(1, hitPos)      -- CP1 = end
```

**Key fact for the port:** in GMod the tracer is a real two-control-point beam particle
(CP0 = start, CP1 = end).

> **Tracer port.** GMod tracers are CP0 to CP1 beams. GMod's own parent stack is `Lifetime from Time to Impact` plus
> `Lifespan Decay`. It has no `FadeAndKillForTracers`, which is a Povarehok-only operator in this
> repo. `postprocess_modern.patch_modern_tracer_cs2_discipline` sets CS2 motion (13k u/s
> MoveBetweenPoints, exactly one particle per shot, zero spawn offset) while keeping that GMod
> lifecycle on the four routed MW2019 parents (`mw2019_tracer{,_fast,_slow,_small}`).
> Further passes in the same file: the automatic-weapon parent `mw2019_tracer` drops sniper-only
> children (`mgbase_tracer_glow_large`, `weapon_tracers_4incendiary`). Glow and trail leaves
> halve alpha, radius and self-illumination. The parent `RenderTrails` keeps `m_flLengthFadeInTime ≈ 0.12`
> plus head and tail alpha taper. The AR parent uses `tracer_middle` (not spark). The visible rope
> child (`mgbase_tracer_trail*`) keeps the converted GMod `fas_smoke_beam` with V-scroll, so smoke
> wisps drift along the CP0 to CP1 beam. This matches GMod, and there is no static stretched tile.

> **Modern barrel smoke:** CS2 Modern matches GMod's per-shot `AfterShotParticle`.
> `barrel_smoke(_plume)` is a direct PCF child of each class flash (world and `_fp`), not a
> spray-gated wrapper. Rope wisps (`barrel_smoke_trail*`) stay `C_OP_RenderRopes` in world space
> for third-person visibility. The eject-port puff (`port_smoke` / `shellsmoke`) is attached as a
> child of each `weapon_shell_casing_*`, which is the CS2 swap target because CS2 has no separate
> eject-port create.

`GetProcessedValue()` is ARC9's attachment-aware property resolver. An attachment, such as a
suppressor, can override a weapon's base field at runtime. That resolution plumbing lives in the
base pack's stats file (`weapons/arc9_base/sh_0_stats.lua`). The MW2019 override values live in
`arc9/common/attachments_bulk/mw19_ammo_types.lua`.

## 3. Class → effect defaults (the 90% case)

Set as `SWEP.MuzzleParticle` / `SWEP.AfterShotParticle` / `SWEP.TracerEffect` in each weapon file.
If a weapon doesn't override `TracerEffect`, it falls back to `arc9_cod2019_base.lua`'s
`TracerEffect = "cod2019_tracer"`.

| Weapon class    | Example guns                                                                    | MuzzleParticle              | AfterShotParticle (barrel smoke)                              | TracerEffect                             | Extra                                                            |
| --------------- | ------------------------------------------------------------------------------- | --------------------------- | ------------------------------------------------------------- | ---------------------------------------- | ---------------------------------------------------------------- |
| Assault Rifle   | AK-47, M4, FAL, SCAR, Kilo141, RAM-7, Oden, CR-56, AN-94, FAMAS, Grau 5.56, M13 | `muzzleflash_ar`            | `barrel_smoke`                                                | _(default)_ `cod2019_tracer`             | AS-VAL is the odd AR: `muzzleflash_suppressed`                   |
| SMG             | MP5, MP7, P90, Vector, Uzi, Bizon, CX-9, ISO, Striker 45, AUG                   | `muzzleflash_smg`           | `barrel_smoke`                                                | _(default)_                              | None                                                             |
| LMG             | PKM, MG34, Holger-26, Bruen Mk9, M91, RAAL, SA87, FiNN                          | `muzzleflash_lmg`           | `barrel_smoke`                                                | _(default)_                              | Minigun: `barrel_smoke_plume`                                    |
| Marksman/DMR    | Kar98k, M14, Mk2, SKS, SP-R 208                                                 | `muzzleflash_dmr`           | `barrel_smoke`                                                | `cod2019_tracer_fast`                    | Crossbow: `muzzleflash_suppressed`, no smoke/tracer              |
| **Sniper**      | AX-50, HDR, SVD, Rytec AMR                                                      | **`muzzleflash_smg`** (quirk, see below) | `barrel_smoke`                                           | `cod2019_tracer_fast`                    | `MakeEnvironmentDust(150)` fires on shot: ground dust puff (§6) |
| Shotgun         | Model 680, JAK-12, R9-0, Origin-12, VLK Rogue, .725                             | `muzzleflash_shotgun`       | `barrel_smoke`                                                | `cod2019_tracer_slow`                    | None                                                             |
| Pistol          | M19, M1911, Renetti, Sykov, X16                                                 | `muzzleflash_pistol`        | `barrel_smoke`                                                | `cod2019_tracer_small`                   | Akimbo variants revert to plain `cod2019_tracer`                 |
| Pistol (magnum) | .357, .50 GS                                                                    | `muzzleflash_pistol_deagle` | `barrel_smoke`                                                | `cod2019_tracer_small`                   | None                                                             |
| Launcher        | RPG-7, PILA, JOKR, Strela-P, M32                                                | `muzzleflash_m79`           | `barrel_smoke_plume` (`AfterShotParticleDelay = -1`, instant) | _(default)_, M32 = `cod2019_tracer_slow` | `MakeEnvironmentDust(200)` on fire                               |

**Sniper quirk (verified, not a bug):** every bolt-action and AMR sniper (AX-50, HDR, SVD, Rytec)
uses `muzzleflash_smg`, _not_ `muzzleflash_dmr`. This is consistent across all four sniper weapon
files. It is likely a copy-paste artifact in the original addon, but treat it as intentional and
authoritative when porting. Do not "correct" it.

## 4. Suppressor override

Weapons that are inherently suppressed hardcode `SWEP.MuzzleParticle = "muzzleflash_suppressed"`
in their weapon file (AS-VAL and the bolt crossbow). There is also an ammo or attachment-level field,
`ATT.MuzzleParticleSilenced = "AC_muzzle_shotgun_suppressed"`, on shotgun slug ammo types in
`mw19_ammo_types.lua`. The runtime code that reads that field at fire time lives outside the two
packs read here, in the base ARC9 attachment framework. No verified read path exists for dynamic
suppressor-attachment swapping. Only the hardcoded per-weapon case above is confirmed from source.

## 5. Tracer effect name to actual particle system

| `SWEP.TracerEffect` (Lua effect) | Real PCF particle it spawns                                                                                                              |
| -------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| `cod2019_tracer` (default)       | `mw2019_tracer_3`                                                                                                                        |
| `cod2019_tracer_fast`            | `mw2019_tracer_fast`                                                                                                                     |
| `cod2019_tracer_slow`            | `mw2019_tracer_slow_new`                                                                                                                 |
| `cod2019_tracer_small`           | `mw2019_tracer_3` (a commented-out Lua line names `mw2019_tracer_small`)       |
| `cod2019_tracer_2`               | `mw2019_tracer_2`                                                                                                                        |
| `cod2019_tracer_inc`             | `mw2019_tracer_inc`                                                                                                                      |
| `cod2019_tracer_rainbow`         | `mw2019_tracer_rainbow`                                                                                                                  |

All of them spawn via `CreateParticleSystem(weapon, name, PATTACH_ABSORIGIN, attachment)`, then
`SetControlPoint(0, start)` and `SetControlPoint(1, end)`.

## 6. Two non-obvious systems

- **HE and frag explosion**: `entities/arc9_cod2019_thrownfrag` →
  `effects/cod2019_grenade_explosion.lua` → precaches `particles/fas_explosions.pcf` and plays
  **`explosion_grenade`**. This is the _base pack's_ explosion system, not `mw2019_explosions_pak.pcf`.
  MW2019 grenades reuse the FAS pack's detonation particle rather than bringing their own.
- **Sniper and launcher floor dust**: `MakeEnvironmentDust()` in `arc9_cod2019_base.lua` is **not a
  PCF at all**. It is the native GMod/Source engine effect `util.Effect("ThumperDust")`, which falls
  back to `"waterripple"` if the shooter is in water. It fires at the shooter's feet only when
  `IsOnGround()`. CS2 has no equivalent native effect, so the CS2 conversion maps it to its own
  authored `uweapon_muzflsh_ground_smoke` particle.

## 6b. The lingering "smoke follows the gun through the air" trail

After firing for a while and then stopping, a ribbon of smoke can hang in world space and trail
behind wherever the barrel has been when you swing your view or weapon. It is distinct from the
single per-shot muzzle puff described in §3.

- **Asset**: `arc9_fas_muzzleflashes.pcf` (the same base-pack PCF as `MuzzleParticle` and
  `AfterShotParticle`) bundles two extra systems, **`barrel_smoke_trail`** and
  **`barrel_smoke_trail_b`**. They are not in the `PrecacheParticleSystem(...)` list in
  `arc9/shared/sh_effects.lua`, and they have **zero** references in either Lua tree (confirmed by
  grep across both workshop addons). They are real assets in the PCF, but Lua never calls them by name.
- **Why they fire anyway**: Source 1 `.pcf` particle systems can nest **child** systems in the
  particle-editor definition. Children spawn automatically whenever their parent spawns, with no
  scripting. `barrel_smoke_trail(_b)` are almost certainly authored as children of the
  `AfterShotParticle` systems (`barrel_smoke` / `barrel_smoke_heavy`). Every time
  `arc9_aftershoteffect.lua` fires the per-shot puff (§2), it silently drags this trail along. A
  single shot barely shows it. Continuous fire keeps re-triggering the parent every shot, which
  makes it obvious.
- **Why it looks like it follows the gun in the air**: the system's renderer class is
  `C_OP_RenderRopes`, a rope or ribbon renderer, not sprites. The particle lifetime is roughly
  **5 seconds**. A rope renderer draws a continuous ribbon through the emitter's recent path samples.
  The emitter is attached `PATTACH_POINT_FOLLOW` to the moving barrel, so the ribbon traces the
  gun's recent movement through the air for the ~5 seconds the segment stays alive. That is the
  visual described above: smoke hanging in space and "following" the barrel when you look around.
- **Known CS2-port gotcha**: converting `C_OP_RenderRopes` as-is in Source 2 stretches a single
  ribbon across the muzzle's _entire_ sweep between puffs, instead of reading as drifting smoke. It
  needs to become a sprite emitter (with frame blending) and a shorter lifetime (~2 to 3s) to look
  right. A literal 1:1 rope port does not work.

## 6c. First-person vs world muzzle-flash split (`_fp` twins)

The pack ships one flash asset per weapon. A world-space flash (`m_bViewModelEffect = false`)
anchors on the weapon muzzle in third person and free cam. The first-person viewmodel is drawn in a
separate pass with viewmodel FOV, so a world-space particle placed at the viewmodel muzzle does not
line up. The barrel smoke aligns in both views because `patch_cs2_modern_barrel_smoke_alignment`
gives it its own `m_bViewModelEffect = true` viewmodel attach.

CS2 and Povarehok ship a **separate `_fp` flash** with `m_bViewModelEffect = true`, and route the
`_fps` systems to it (see `weapon_muzzle_flash_*_fp` in `kVariantWeaponFx` and `kSprayPairs`).
Modern does the same:

- `postprocess_modern.make_modern_muzzleflash_fp` writes a `<flash>_fp.vpcf` viewmodel-effect twin of
  every world flash in `MODERN_MUZZLEFLASH_FILES`. The twin is identical apart from the flag. It also
  writes `_fp` variants of the two sniper compositions (`MVM_COMPOSITIONS_FP`, whose flash child is the
  `_fp` leaf). The shock-dust, plume and heat children stay world-space.
- The `*_fps` rows in `kVariantWeaponFx` point at the `_fp` target. The world rows keep the world target.
- Detached-camera handling is unchanged. `g_fpFxSuppress` and `IsFirstPersonWeaponFxPath` still key on
  the **source** `_fps` or `_fp` system name, not the swap target. So a third-person or free-cam view
  still suppresses the viewmodel flash and shows the world one through the spec_mode-3 chase path.
  Spray upgrades key on the resolved **target**. `_fp` targets are not `kSprayPairs` bases, so a
  first-person flash is never re-upgraded to a world-space spray wrapper.

## 6d. Flash sheet and thrown-grenade trail port

Modern central muzzle-flash sprite leaves that reference `hl2_muzzleflash` are retargeted to the
pack's higher-resolution `fas_muzzleflash_test_b` sheet. They also receive `C_OP_PositionLock`, so the
center burst stays attached to the muzzle. Authored animation rates, alpha behavior and frame-blend
settings are left intact. The `_fp` twins described above inherit the texture and lock. They differ
from their world flashes only in the required viewmodel flag.

Thrown-grenade smoke trails are a synthesized CS2-side composition, not a converted
`mw2019_effects.pcf` system. The runtime swaps CS2's demo-only
`particles/entity/spectator_utility_trail.vpcf` to `mvm_grenade_trail` in Modern mode only. Its two
children use the `materials/effects/fas_dust_a.vtex` and `fas_dust_b.vtex` textures with their authored
alpha, radius, lifetime, fade and growth values. A `PF_TYPE_CONTROL_POINT_SPEED`
emit-rate gate stops the trail emitting when the grenade lands. The deployed smoke grenade cloud is
still CS2's native volumetric system. This particle swap does not change it.

## 6e. Runtime resolve and precache pacing

The Modern pack has enough top-level swap targets that cold resolution can hitch if demo entry forces
the whole table through synchronous particle-resource loads. The active target set is therefore kept
separate from the resolve queue. Mode rebuilds make Modern targets eligible, but they do not enqueue
the entire pack. The create hook queues a target only after its first real use, and creation fails
open. `ParticleFx.cpp` warms the demand-only queue one target at a time. During active playback it
uses a slow, wall-clock-spaced trickle with heavier backoff after cold loads. Paused demos warm
faster, and the main menu does not resolve queued FX targets. This only controls **when** target
handles are resolved.
Level and demo changes still clear cached handles, because CS2 purges particle resources across map
transitions.

## 7. Particle source files (what to extract and convert)

| File                                                                                                                                                  | Contents                                                                                                                                           |
| ----------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| `arc9_fas_muzzleflashes.pcf`                                                                                                                          | All `muzzleflash_*` class systems, `barrel_smoke*` (incl. the un-precached rope trail `barrel_smoke_trail`/`_b`, §6b), `shellsmoke`, `port_smoke*` |
| `arc9_fas_explosions.pcf` (loaded as `fas_explosions.pcf` in the grenade effect; the two references do not match; verify the actual filename on disk) | `explosion_grenade` and friends                                                                                                                    |
| `mw2019_tracer.pcf`                                                                                                                                   | `mw2019_tracer`, `_2`, `_3`, `_fast`, `_slow_new`, `_small`, `_inc`, `_rainbow`                                                                    |
| `mw2019_effects.pcf`                                                                                                                                  | Misc COD-specific effects (grenade trails, molotov, etc.)                                                                                          |
| `mw2019_rockettrail.pcf`                                                                                                                              | Launcher rocket trails                                                                                                                             |
| `mw2019_explosions_pak.pcf`                                                                                                                           | Non-grenade explosions (rockets, C4, etc.; grenades do not use this one)                                                                           |
