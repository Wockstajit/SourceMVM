"""Modern (MW2019/ARC9) pack-specific CS2 VPCF gameplay composites and alignment.

Imports only from postprocess_common -- never from postprocess_insurgency, so the two
pack files stay independent of each other.
"""

from __future__ import annotations

import re
from pathlib import Path

import postprocess_common as common

# ---------------------------------------------------------------------------
# Gameplay composites (2026-07-03): compose REAL pack assets (plus stock CS2
# distortion systems) into the runtime shapes the GMod mod produced with Lua,
# which PCF conversion alone cannot express:
#   - per-shot barrel smoke (ARC9 AfterShotParticle) as children of the modern
#     muzzle flashes;
#   - sniper shots (AWP + autosnipers) get the pack's M82 treatment: shock-dust
#     ring around the shooter + heavy barrel plume + heat distortion;
#   - a grenade flight smoke trail (synthesized from the pack's own
#     barrel_smoke_trail with un-capped emission) swapped over CS2's
#     spectator utility-trail system;
#   - native CS2 refraction restored: converted explosions get the STOCK CS2
#     distort systems as children (the originals' Source 1 refract quads were
#     removed by fix_textureless_renderers), and the pack's muzzle_heatwave
#     files are REWRITTEN as behavior-version-12 systems cloned from the stock
#     explosion_hegrenade_distort renderer. (Lesson 2026-07-03: injecting bare
#     m_flRefractAmount keys into the converted behavior-8 files compiled fine
#     but never engaged the refract shader path -- the warp NORMAL MAP rendered
#     as plain color, the user-reported purple/rainbow blob.)
# This pass is idempotent (safe to re-run on an already-patched tree).
# ---------------------------------------------------------------------------

MODERN_MUZZLE_DIR = "particles/filmmaker/modern/arc9_fas_muzzleflashes"
MODERN_EXPLOSION_DIR = "particles/filmmaker/modern/arc9_fas_explosions"

# Class flash -> barrel smoke pairing (GMod's AfterShotParticle mapping). 2026-07-06
# night: these are NO LONGER direct PCF children of the flashes -- a child spawns with
# EVERY shot, so sustained fire stacked overlapping smoke instances (user: "duplicating
# the smoke every time you shoot; keep one wisp going") and the sniper compositions got
# the plume TWICE (own child + the dmr flash's child = the SCAR-20 double smoke). The
# pairing now produces spray wrappers (mvm_spray_muzzleflash_*, world + _fp), and the
# hook's kSprayPairs + upgrade cooldown (ParticleFxSpray.cpp) keeps ONE smoke instance
# alive per weapon instead of one per shot.
MODERN_FLASH_SMOKE_CHILDREN = {
    "muzzleflash_ar.vpcf": "barrel_smoke.vpcf",
    "muzzleflash_smg.vpcf": "barrel_smoke.vpcf",
    "muzzleflash_shotgun.vpcf": "barrel_smoke.vpcf",
    "muzzleflash_pistol.vpcf": "barrel_smoke.vpcf",
    "muzzleflash_pistol_deagle.vpcf": "barrel_smoke.vpcf",
    "muzzleflash_lmg.vpcf": "barrel_smoke_plume.vpcf",
    "muzzleflash_dmr.vpcf": "barrel_smoke_plume.vpcf",
}

# Synthesized composition systems (children-only files, same structure as the
# pack's own barrel_smoke.vpcf). Sniper comps reproduce the mod's .50-cal look.
# 2026-07-06 night: barrel_smoke_plume REMOVED from the sniper comps -- the dmr flash
# child used to carry its own plume too, so the autosniper got the SAME plume twice per
# shot (the "two smokes on the SCAR-20/autosniper" report), and per-shot comp smoke
# stacked on fast-firing autos anyway. The plume now rides the comps' mvm_spray_*
# wrappers (kSprayPairs + upgrade cooldown = one plume kept going).
MVM_COMPOSITIONS = {
    f"{MODERN_MUZZLE_DIR}/mvm_muzzleflash_sniper_awp.vpcf": (
        f"{MODERN_MUZZLE_DIR}/muzzleflash_smg.vpcf",
        f"{MODERN_MUZZLE_DIR}/m82_shocksmoke.vpcf",
        f"{MODERN_MUZZLE_DIR}/muzzle_heatwave.vpcf",
    ),
    f"{MODERN_MUZZLE_DIR}/mvm_muzzleflash_sniper_auto.vpcf": (
        f"{MODERN_MUZZLE_DIR}/muzzleflash_dmr.vpcf",
        f"{MODERN_MUZZLE_DIR}/m82_shocksmoke.vpcf",
        f"{MODERN_MUZZLE_DIR}/muzzle_heatwave.vpcf",
    ),
    f"{MODERN_MUZZLE_DIR}/mvm_grenade_trail.vpcf": (
        f"{MODERN_MUZZLE_DIR}/mvm_grenade_smoke_trail.vpcf",
        f"{MODERN_MUZZLE_DIR}/mvm_grenade_smoke_trail_b.vpcf",
    ),
}

# First-person twins of the sniper compositions above: identical except the flash CHILD is
# the viewmodel-effect _fp leaf. The _fps sniper rows in kVariantWeaponFx route here.
MVM_COMPOSITIONS_FP = {
    f"{MODERN_MUZZLE_DIR}/mvm_muzzleflash_sniper_awp_fp.vpcf": (
        f"{MODERN_MUZZLE_DIR}/muzzleflash_smg_fp.vpcf",
        f"{MODERN_MUZZLE_DIR}/m82_shocksmoke.vpcf",
        f"{MODERN_MUZZLE_DIR}/muzzle_heatwave.vpcf",
    ),
    f"{MODERN_MUZZLE_DIR}/mvm_muzzleflash_sniper_auto_fp.vpcf": (
        f"{MODERN_MUZZLE_DIR}/muzzleflash_dmr_fp.vpcf",
        f"{MODERN_MUZZLE_DIR}/m82_shocksmoke.vpcf",
        f"{MODERN_MUZZLE_DIR}/muzzle_heatwave.vpcf",
    ),
}

# Grenade-trail systems, authored from scratch (see _trail_system_text). The old
# approach cloned barrel_smoke_trail with un-capped emission, which kept puffing
# forever once the grenade landed or stuck in a wall (user report 2026-07-03);
# these gate the emit rate on control-point SPEED so a resting grenade emits
# nothing while a flying one leaves the MW-style puff trail.
MVM_GRENADE_TRAILS = {
    f"{MODERN_MUZZLE_DIR}/mvm_grenade_smoke_trail.vpcf": dict(
        texture="materials/effects/fas_dust_a.vtex", rate=16.0,
        radius_min=3.0, radius_max=6.0, grey=150, alpha=0.45),
    f"{MODERN_MUZZLE_DIR}/mvm_grenade_smoke_trail_b.vpcf": dict(
        texture="materials/effects/fas_dust_b.vtex", rate=10.0,
        radius_min=4.0, radius_max=8.0, grey=120, alpha=0.35),
}

# Converted explosion roots -> stock CS2 distortion child restoring the native
# heat-shimmer the vanilla systems had.
MODERN_DISTORT_CHILDREN = {
    f"{MODERN_EXPLOSION_DIR}/explosion_grenade.vpcf": common.STOCK_HE_DISTORT,
}

# The pack's per-shot heat distortion, rewritten as full behavior-12 systems
# (size/lifetime per file). REVERTED to mod-authentic scope (2026-07-03 night,
# user report): only the .50-cal / auto-sniper class (AWP + the two autosniper
# compositions) gets this in the mod's own Lua -- it was briefly added as a
# child of every unsuppressed class flash and the user asked for that undone.
# muzzle_heatwave_long is unused by any current rule but kept authored/compiled
# in case a future composition wants the longer variant.
MVM_HEATWAVES = {
    f"{MODERN_MUZZLE_DIR}/muzzle_heatwave.vpcf": dict(
        particles=3, lifetime=0.4, end_scale=10.0, offset_lo=6.0, offset_hi=16.0),
    f"{MODERN_MUZZLE_DIR}/muzzle_heatwave_long.vpcf": dict(
        particles=4, lifetime=0.7, end_scale=16.0, offset_lo=8.0, offset_hi=24.0),
}

# Flash files that ONCE had muzzle_heatwave added as a child (2026-07-03 pm) and
# must have it stripped from an already-patched tree. Kept only for that one-time
# cleanup pass; do not add new entries here -- the sniper compositions
# (mvm_muzzleflash_sniper_awp/auto in kVariantWeaponFx) are the only intended users.
MODERN_HEATWAVE_CHILDREN_TO_REMOVE = (
    "muzzleflash_ar.vpcf",
    "muzzleflash_smg.vpcf",
    "muzzleflash_dmr.vpcf",
    "muzzleflash_lmg.vpcf",
    "muzzleflash_shotgun.vpcf",
    "muzzleflash_pistol.vpcf",
    "muzzleflash_pistol_deagle.vpcf",
)


def _distort_system_text(*, particles: int, lifetime: float, end_scale: float,
                         offset_lo: float, offset_hi: float) -> str:
    """A muzzle-sized clone of CS2's stock explosion_hegrenade_distort.

    Renderer keys (refract amount/blur + warp normal map) match the stock
    system; everything else is a minimal spawn: a few short-lived quads just
    past the muzzle that grow, spin, and fade.

    LESSON (2026-07-03 night, user report "just a circle that gets bigger, not
    actually distorting anything"): the stock file's m_vecColorScale is a warm
    tan tint ([255,188,154]) added on top of the refracted background via
    PARTICLE_OUTPUT_BLEND_MODE_ADD. On a large HE-grenade blast that tint reads
    as part of the fire glow; at muzzle scale against a flat wall (no texture
    detail to visibly bend) it's the ONLY thing visible -- an expanding warm
    blob with no apparent displacement. Color scale here is [0,0,0]: additive
    zero adds nothing, so the quad is invisible except for the refraction
    offset it applies to whatever is drawn behind it -- real heat-shimmer
    (only visible where the background has contrast/detail to warp).
    """
    return f"""{common.VPCF12_HEADER}
{{
	_class = "CParticleSystemDefinition"
	m_nBehaviorVersion = 12
	m_nMaxParticles = 8
	m_Emitters =
	[
		{{
			_class = "C_OP_InstantaneousEmitter"
			m_nParticlesToEmit =
			{{
				m_nType = "PF_TYPE_LITERAL"
				m_flLiteralValue = {float(particles)}
			}}
		}},
	]
	m_Initializers =
	[
		{{
			_class = "C_INIT_CreateWithinSphereTransform"
			m_flRadius =
			{{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = 0.0
				m_flRandomMax = 4.0
			}}
		}},
		{{
			_class = "C_INIT_PositionOffset"
			m_bLocalCoords = true
			m_OffsetMin = [ {offset_lo}, 0.0, 0.0 ]
			m_OffsetMax = [ {offset_hi}, 0.0, 0.0 ]
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_LITERAL"
				m_flLiteralValue = 2.0
			}}
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_LITERAL"
				m_flLiteralValue = {lifetime}
			}}
			m_nOutputField = 1
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = 0.0
				m_flRandomMax = 360.0
			}}
			m_nOutputField = 4
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = 20.0
				m_flRandomMax = 60.0
				m_bHasRandomSignFlip = true
			}}
			m_nOutputField = 5
		}},
	]
	m_Operators =
	[
		{{
			_class = "C_OP_BasicMovement"
			m_fDrag = 0.15
		}},
		{{
			_class = "C_OP_SpinUpdate"
		}},
		{{
			_class = "C_OP_Decay"
		}},
		{{
			_class = "C_OP_FadeOut"
			m_flFadeOutTimeMin = 0.6
			m_flFadeOutTimeMax = 0.6
			m_bEaseInAndOut = false
		}},
		{{
			_class = "C_OP_InterpolateRadius"
			m_flStartScale = 3.0
			m_flEndScale = {end_scale}
			m_flBias = 0.667
		}},
	]
	m_Renderers =
	[
		{{
			_class = "C_OP_RenderSprites"
			m_bDisableZBuffering = true
			m_flRefractAmount = -0.5
			m_nRefractBlurRadius = 3
			m_nRefractBlurType = "BLURFILTER_BOX"
			m_flAnimationRate = 2.0
			m_vecTexturesInput =
			[
				{{
					m_hTexture = resource:"{common.STOCK_WARP_NORMAL}"
					m_nTextureBlendMode = "SPRITECARD_TEXTURE_BLEND_LUMINANCE"
				}},
			]
			m_vecColorScale =
			{{
				m_nType = "PVEC_TYPE_LITERAL_COLOR"
				m_LiteralColor = [ 0, 0, 0 ]
			}}
			m_bUseMixedResolutionRendering = true
			m_nOutputBlendMode = "PARTICLE_OUTPUT_BLEND_MODE_ADD"
		}},
	]
}}
"""


def _trail_system_text(*, texture: str, rate: float, radius_min: float,
                       radius_max: float, grey: int, alpha: float) -> str:
    """A grenade-flight puff trail whose EMISSION is gated on CP0 speed.

    CP0 is the projectile (the spectator_utility_trail anchor), so a grenade in
    flight emits puffs and one at rest (landed / stuck in a wall) emits nothing
    -- the whole point of authoring these instead of cloning barrel_smoke_trail.
    Discrete sprites, not ropes: rope ribbons stretch across the flight path's
    corners and read as smoke floating in mid-air.
    """
    g = grey / 255.0
    return f"""{common.VPCF12_HEADER}
{{
	_class = "CParticleSystemDefinition"
	m_nBehaviorVersion = 12
	m_nMaxParticles = 128
	m_Emitters =
	[
		{{
			_class = "C_OP_ContinuousEmitter"
			m_flEmitRate =
			{{
				m_nType = "PF_TYPE_CONTROL_POINT_SPEED"
				m_nControlPoint = 0
				m_nMapType = "PF_MAP_TYPE_REMAP"
				m_nInputMode = "PF_INPUT_MODE_CLAMPED"
				m_flInput0 = 30.0
				m_flInput1 = 500.0
				m_flOutput0 = 0.0
				m_flOutput1 = {rate}
			}}
		}},
	]
	m_Initializers =
	[
		{{
			_class = "C_INIT_CreateWithinSphereTransform"
			m_flRadius =
			{{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = 0.0
				m_flRandomMax = 3.0
			}}
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = {radius_min}
				m_flRandomMax = {radius_max}
			}}
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = 1.5
				m_flRandomMax = 2.5
			}}
			m_nOutputField = 1
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_LITERAL"
				m_flLiteralValue = {alpha}
			}}
			m_nOutputField = 7
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = 0.0
				m_flRandomMax = 360.0
			}}
			m_nOutputField = 4
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = 10.0
				m_flRandomMax = 30.0
				m_bHasRandomSignFlip = true
			}}
			m_nOutputField = 5
		}},
	]
	m_Operators =
	[
		{{
			_class = "C_OP_BasicMovement"
			m_Gravity = [ 0.0, 0.0, 10.0 ]
			m_fDrag = 0.05
		}},
		{{
			_class = "C_OP_SpinUpdate"
		}},
		{{
			_class = "C_OP_Decay"
		}},
		{{
			_class = "C_OP_FadeOut"
			m_flFadeOutTimeMin = 0.8
			m_flFadeOutTimeMax = 0.8
			m_bEaseInAndOut = false
		}},
		{{
			_class = "C_OP_InterpolateRadius"
			m_flStartScale = 1.0
			m_flEndScale = 2.5
		}},
	]
	m_Renderers =
	[
		{{
			_class = "C_OP_RenderSprites"
			m_bBlendFramesSeq0 = true
			m_vecTexturesInput =
			[
				{{
					m_hTexture = resource:"{texture}"
				}},
			]
			m_vecColorScale =
			{{
				m_nType = "PVEC_TYPE_LITERAL"
				m_vLiteralValue = [ {g:.3f}, {g:.3f}, {g:.3f} ]
			}}
			m_flSelfIllumAmount =
			{{
				m_nType = "PF_TYPE_LITERAL"
				m_flLiteralValue = 1.0
			}}
		}},
	]
}}
"""


# The pack's per-shot barrel smoke ribbons. Converted as C_OP_RenderRopes, the
# engine stretches ONE ribbon across the muzzle's whole sweep between puffs --
# the "smoke floating in mid-air far from the muzzle" arcs (user report
# 2026-07-03). Discrete sprites at a higher rate/size read as the same smoke
# stream without the stretched-ribbon artifact; lifetime comes down from the
# mod's 5s so spent smoke doesn't hang around whole fights. Exact-string
# replacements: idempotent by construction.
# Modern sustained barrel smoke (spray-gated). Regular On/Less uses weapon_muzzle_smoke_long
# instead (see postprocess_povarehok); both share the shared muzzle-smoke alignment recipe.
MODERN_BARREL_TRAIL_FILES = (
    f"{MODERN_MUZZLE_DIR}/barrel_smoke_trail.vpcf",
    f"{MODERN_MUZZLE_DIR}/barrel_smoke_trail_b.vpcf",
)
# The files ACTUALLY swapped to at runtime (kVariantWeaponFx's Modern targets for CS2's
# own weapon_muzzle_smoke/weapon_muzzle_smoke_long, and the smoke half of every
# MODERN_FLASH_SMOKE_CHILDREN composition below). Bug (2026-07-03, "Modern's muzzle attach doesn't
# follow Povarehok's"): only the *_trail files above ever received the CS2 muzzle
# alignment patch, but nothing in the ParticleFx swap tables (ParticleFxRules.cpp) ever swaps to them -- they are dead
# output. These two are the ones that actually spawn on screen; route them through the
# identical alignment recipe Povarehok's own weapon_muzzle_smoke_long already gets.
MODERN_LIVE_BARREL_SMOKE_FILES = (
    f"{MODERN_MUZZLE_DIR}/barrel_smoke.vpcf",
    f"{MODERN_MUZZLE_DIR}/barrel_smoke_plume.vpcf",
)

MODERN_MUZZLEFLASH_FILES = (
    f"{MODERN_MUZZLE_DIR}/muzzleflash_ar.vpcf",
    f"{MODERN_MUZZLE_DIR}/muzzleflash_smg.vpcf",
    f"{MODERN_MUZZLE_DIR}/muzzleflash_shotgun.vpcf",
    f"{MODERN_MUZZLE_DIR}/muzzleflash_pistol.vpcf",
    f"{MODERN_MUZZLE_DIR}/muzzleflash_pistol_deagle.vpcf",
    f"{MODERN_MUZZLE_DIR}/muzzleflash_lmg.vpcf",
    f"{MODERN_MUZZLE_DIR}/muzzleflash_dmr.vpcf",
    f"{MODERN_MUZZLE_DIR}/muzzleflash_suppressed.vpcf",
)

# Muzzle-flash CHILDREN spawn in the WEAPON's frame, not the world's.
#
# Bug (user report, "one of the muzzle flashes is really far from the barrel, most obvious in
# third person"): the leaves under arc9_fas_muzzleflashes carry
#
#     C_INIT_CreateWithinSphere
#         m_bLocalCoords = false
#         m_LocalCoordinateSystemSpeedMin/Max = [400, 0, 0]     (200-400 on the glow/centre leaves)
#
# m_LocalCoordinateSystemSpeed is meant to be read in the weapon's local frame -- +X is forward
# down the barrel -- so 400 u/s is the flash projecting out of the muzzle. With m_bLocalCoords
# false the engine applies it in WORLD space instead, flinging the sprites along world +X no
# matter where the gun points. Over the 0.03-0.05s lifetime that is 6-20 units of displacement on
# blobs whose radius is 10-20 (x2.6 via InterpolateRadius) -- a large, soft, low-opacity puff
# sitting clear of the barrel, which does not track the weapon as the player turns.
#
# WHY THIRD PERSON MAKES IT OBVIOUS: in first person the muzzle sits nearly on the camera axis, so
# a world-space offset is mostly hidden ALONG the view direction. In third person you look across
# the shot line and the same offset is fully visible.
#
# Measured before the fix: 41 of the 70 files in the directory.
#
# patch_cs2_modern_muzzleflash_alignment already does exactly this -- but it only ever ran over
# MODERN_MUZZLEFLASH_FILES, which lists the eight PARENT systems. The speeds live in the children,
# so the repair existed and never reached the files carrying the fault.
#
# The speeds are KEPT, only re-framed. Zeroing them (which the parent pass does, deliberately, so
# the parent emitter cannot drift) would delete the flash's forward projection out of the barrel --
# that motion is the authored look, it was simply being applied in the wrong space.
MODERN_MUZZLE_LOCALCOORDS_MARKER = (
    "// mvm: muzzle-flash children spawn in weapon-local coords (world-space flung them off the "
    "barrel; see postprocess_modern)")


def patch_modern_muzzle_child_local_coords(text: str) -> str:
    """Force m_bLocalCoords = true on this system's spawn emitters. Marker-guarded."""
    if MODERN_MUZZLE_LOCALCOORDS_MARKER in text:
        return text
    edits = []
    for match in re.finditer(r'_class = "C_INIT_CreateWithinSphere"', text):
        start, end = common.block_span(text, match.start())
        block = text[start:end]
        if "m_bLocalCoords" in block:
            fixed = re.sub(r"m_bLocalCoords = false", "m_bLocalCoords = true", block)
        else:
            # An absent key defaults to world space, so it has to be written in explicitly.
            fixed = block.replace(
                '_class = "C_INIT_CreateWithinSphere"',
                '_class = "C_INIT_CreateWithinSphere"\n\t\t\tm_bLocalCoords = true', 1)
        if fixed != block:
            edits.append((start, end, fixed))
    if not edits:
        return text
    for start, end, replacement in reversed(edits):
        text = text[:start] + replacement + text[end:]
    return text.rstrip("\n") + "\n" + MODERN_MUZZLE_LOCALCOORDS_MARKER + "\n"


MODERN_FLASH_BURST_OLD_TEXTURE = "materials/effects/hl2_muzzleflash.vtex"
MODERN_FLASH_BURST_NEW_TEXTURE = "materials/effects/fas_muzzleflash_test_b.vtex"

# First-person (_fp) muzzle-flash twins. Bug (user report 2026-07-04, "modern first-person
# muzzle flashes don't align -- they float off to the side of the gun; in third person the
# flash sits right"): the pack ships ONE flash asset per weapon and kVariantWeaponFx routed
# BOTH the first-person (_fps) and the world CS2 muzzle systems to it. A world-space flash
# (m_bViewModelEffect = false) anchors correctly on the weapon in third person / free cam,
# but the first-person viewmodel is drawn in a separate pass with viewmodel FOV, so a
# world-space flash placed at the viewmodel muzzle floats out of line. Povarehok solves this
# by shipping a SEPARATE _fp flash with m_bViewModelEffect = true and routing the _fps
# systems to it (see weapon_muzzle_flash_*_fp in kVariantWeaponFx / kSprayPairs). We do the
# same: apply_modern_gameplay_composites writes a viewmodel-effect twin of every world flash
# above, and the _fps rows in kVariantWeaponFx point at the _fp twin. Barrel smoke/wisps
# get the same world + _fp split (see make_modern_smoke_fp below) so FP reload animations
# track the viewmodel muzzle while third person keeps the world-pass twins.

# Modern tracer parents actually routed to at runtime (kVariantTracers + kTracerFallbacks in
# ParticleFxRules.cpp). Converted GMod tracers shipped with broken emitters (random -1..1
# count on mw2019_tracer) and extreme MoveBetweenPoints speeds. patch_modern_tracer_cs2_
# discipline keeps the MW2019 renderer textures/children but fixes CS2 motion: exactly one
# particle per shot, MoveBetweenPoints @ 13k u/s, and GMod's parent lifecycle stack
# (LifespanFromVelocity + Decay — no FadeAndKillForTracers, which GMod never had).
MODERN_TRACER_DIR = "particles/filmmaker/modern/mw2019_tracer"
MODERN_TRACER_PARENTS = (
    f"{MODERN_TRACER_DIR}/mw2019_tracer.vpcf",
    f"{MODERN_TRACER_DIR}/mw2019_tracer_fast.vpcf",
    f"{MODERN_TRACER_DIR}/mw2019_tracer_slow.vpcf",
    f"{MODERN_TRACER_DIR}/mw2019_tracer_small.vpcf",
)
# AR/SMG/LMG/automatic tier -- must not mount sniper-only glow/incendiary children.
MODERN_TRACER_AR_TIER = (f"{MODERN_TRACER_DIR}/mw2019_tracer.vpcf",)
SNIPER_ONLY_TRACER_CHILDREN = frozenset(
    {"mgbase_tracer_glow_large.vpcf", "weapon_tracers_4incendiary.vpcf"}
)
MODERN_TRACER_GLOW_LEAVES = (
    "mgbase_tracer_glow.vpcf",
    "mgbase_tracer_glow_small.vpcf",
    "mgbase_tracer_glow_large.vpcf",
    "mgbase_tracer_trail.vpcf",
    "mgbase_tracer_trail_faint.vpcf",
    "weapon_tracers_4incendiary.vpcf",
)
# Povarehok per-class tracers (weapon_tracers_assrifle etc.) all fly at this speed.
_TRACER_MOVE_SPEED = "13000.0"
# User tuning: softer MW2019 glow stack + soft streak head on RenderTrails.
TRACER_GLOW_BRIGHTNESS_SCALE = 0.5
# IDEMPOTENCY FENCE (2026-09-03). This pass MULTIPLIES, so without a marker every in-place
# re-run halved the MW2019 tracers again. Found with alpha 2/255 and radius 0.19 on the wisp
# (shipped: 55-60 / 6-6.5) = five extra halvings, and every Modern tracer invisible. Keep in
# sync with FX/tools/restore-modern-tracer-brightness.py, which repaired that tree.
TRACER_BRIGHTNESS_MARKER = "// mvm: modern tracer brightness applied once (x0.5)"
_TRACER_TRAIL_LENGTH_FADE_IN = "0.12"
# Tracer rope child (mgbase_tracer_trail*): keep converted GMod fas_smoke_beam + V-scroll.
# RenderTrails head/tail alpha scale (0 = transparent at that end of the streak).
_TRACER_HEAD_ALPHA_SCALE = "0.0"
_TRACER_TAIL_ALPHA_SCALE = "0.0"
_TRACER_RADIUS_HEAD_TAPER = "0.35"
_TRACER_PF_LITERAL_BLOCK_RE = re.compile(
    r"(?m)^(\t\t\t)(m_fl(?:Head|Tail)AlphaScale|m_flRadiusHeadTaper) = \n"
    r"\t\t\t\{[^\n]*\n(?:\t\t\t\t[^\n]*\n)*?\t\t\t\},?\n"
)
# GMod Lua places CP0 at the muzzle -- no forward kick. Longer trail segments (vs the
# converted 20u postage-stamp) pair with m_flLengthFadeInTime for a soft line head.
MODERN_TRACER_TRAIL_LENGTH = {
    "mw2019_tracer.vpcf": 110.0,
    "mw2019_tracer_fast.vpcf": 120.0,
    "mw2019_tracer_slow.vpcf": 110.0,
    "mw2019_tracer_small.vpcf": 90.0,
}
MODERN_TRACER_TRAIL_CHILD_SOFT_HEAD = frozenset(
    {"mgbase_tracer_trail.vpcf", "mgbase_tracer_trail_faint.vpcf"}
)
RENDER_TRAILS_RE = re.compile(r'_class = "C_OP_RenderTrails"')

# Modern rope wisps attach to the barrel and trace gun motion (§6b). The Povarehok
# plume offset (postprocess_common._CS2_MUZZLE_OFFSET_BLOCK) pushes spawn 1-6 units
# forward and 2-4 up -- user report 2026-07-03 night: Modern FP rope reads in front of
# and above every weapon's muzzle.
_CS2_MODERN_ROPE_TRAIL_OFFSET_BLOCK = (
    "\t\t{\n"
    '\t\t\t_class = "C_INIT_PositionOffset"\n'
    "\t\t\tm_bLocalCoords = true\n"
    "\t\t\tm_OffsetMin = [0.0, 0.0, -0.5]\n"
    "\t\t\tm_OffsetMax = [0.5, 0.0, 0.0]\n"
    "\t\t},\n"
)


def _force_cs2_modern_rope_trail_offset_block(block: str) -> str:
    """Barrel-tip spawn for Modern rope wisps (not Povarehok plume forward/up kick)."""
    block = re.sub(r"m_bLocalCoords = (?:true|false)", "m_bLocalCoords = true", block)
    if "m_bLocalCoords" not in block:
        block = block.replace(
            '_class = "C_INIT_PositionOffset"',
            '_class = "C_INIT_PositionOffset"\n\t\t\tm_bLocalCoords = true',
            1,
        )
    block = re.sub(r"m_OffsetMin = \[[^\]]*\]", "m_OffsetMin = [0.0, 0.0, -0.5]", block)
    block = re.sub(r"m_OffsetMax = \[[^\]]*\]", "m_OffsetMax = [0.5, 0.0, 0.0]", block)
    return block


def _ensure_cs2_modern_rope_trail_position_offset(text: str) -> str:
    if "C_INIT_PositionOffset" in text:
        edits = []
        for match in re.finditer(r'_class = "C_INIT_PositionOffset"', text):
            start, end = common.block_span(text, match.start())
            edits.append((start, end, _force_cs2_modern_rope_trail_offset_block(text[start:end])))
        for start, end, replacement in reversed(edits):
            text = text[:start] + replacement + text[end:]
        return text
    anchor = re.search(r'_class = "C_INIT_CreateWithinSphere"', text)
    if not anchor:
        return text
    insert_at = text.find("\n\t\t},", anchor.start())
    if insert_at < 0:
        return text
    return (
        text[: insert_at + len("\n\t\t},")]
        + "\n"
        + _CS2_MODERN_ROPE_TRAIL_OFFSET_BLOCK
        + text[insert_at + len("\n\t\t},") :]
    )


# Povarehok's own barrel plume rises because C_OP_BasicMovement carries a positive-Z
# gravity ([0,0,25] on weapon_muzzle_smoke_long) -- buoyancy that lifts the smoke off the
# barrel after it spawns. The converted Modern rope wisps (barrel_smoke_trail{,_b}) came
# through with m_Gravity = [0,0,0], so they sat glued to the muzzle (user report 2026-07-06,
# "Modern smoke mostly sits at the barrel; make it rise like Povarehok"). This is the SINGLE
# lever that makes it rise: the muzzle-local spawn offset is untouched, so the wisp still
# starts at the barrel tip and only the post-spawn drift is upward, exactly like Povarehok.
_MODERN_SMOKE_RISE_GRAVITY_Z = 22.0


def _force_modern_smoke_rise(text: str, gravity_z: float = _MODERN_SMOKE_RISE_GRAVITY_Z) -> str:
    """Set every C_OP_BasicMovement gravity to a pure upward vector (buoyant rise).

    Zeroing X/Y keeps the rise vertical like Povarehok's plume (whose only gravity component
    is +Z); any inherited forward/side gravity from the conversion is dropped. Idempotent:
    the value is rewritten to the same literal on re-runs.
    """
    edits = []
    for match in re.finditer(r'_class = "C_OP_BasicMovement"', text):
        start, end = common.block_span(text, match.start())
        block = text[start:end]
        if re.search(r"m_Gravity = \[[^\]]*\]", block):
            new_block = re.sub(
                r"m_Gravity = \[[^\]]*\]",
                f"m_Gravity = [0.0, 0.0, {gravity_z}]",
                block,
                count=1,
            )
        else:
            new_block = block.replace(
                '_class = "C_OP_BasicMovement"',
                f'_class = "C_OP_BasicMovement"\n\t\t\tm_Gravity = [0.0, 0.0, {gravity_z}]',
                1,
            )
        if new_block != block:
            edits.append((start, end, new_block))
    for start, end, replacement in reversed(edits):
        text = text[:start] + replacement + text[end:]
    return text


def _tune_modern_rope_trail_particles(text: str) -> str:
    """Keep C_OP_RenderRopes; reduce world-up velocity bias and soften the ribbon."""
    new = text
    # World-space noise with m_vecOutput Z=7..10 lifts the ribbon above the barrel.
    #
    # BUG (found 2026-07-04, user report "wisp sits around, looks like you move your
    # gun"): this used to force false->true, i.e. LOCAL space, the opposite of what the
    # comment above says and wants. In local space the noise/movement operator's output
    # is recomputed relative to the (moving, viewmodel-attached) control point every
    # frame, so an already-spawned puff keeps re-anchoring to the current muzzle
    # position/orientation instead of drifting freely -- it reads as glued to the barrel
    # and dragged around as the gun moves. Force WORLD space instead: the initial
    # C_INIT_PositionOffset spawn point stays local (muzzle-anchored, unchanged), but the
    # wisp's post-spawn rise is independent world-space motion, like real smoke.
    new = re.sub(r"m_bLocalSpace = (?:true|false)", "m_bLocalSpace = false", new, count=1)
    new = re.sub(
        r"m_vecOutputMin = \[[^\]]*\]",
        "m_vecOutputMin = [-1.0, -1.0, -0.5]",
        new,
        count=1,
    )
    new = re.sub(
        r"m_vecOutputMax = \[[^\]]*\]",
        "m_vecOutputMax = [1.0, 1.0, 0.5]",
        new,
        count=1,
    )
    new = new.replace("m_flTessScale = 10", "m_flTessScale = 6")
    new = new.replace("m_flConstantRadius = 5.0", "m_flConstantRadius = 3.5")
    new = re.sub(r"m_flEndScale = 3\.0", "m_flEndScale = 2.0", new)
    # Alpha was reduced to 38/255 (~15%) which made the rifle wisp nearly invisible once it
    # was past its peak -- combined with the old fade-from-birth it read as "doesn't show all
    # the way" and left nothing visible to follow during a reload/inspect (2026-07-06). Keep
    # it a wisp, but visible: ~100/255.
    new = re.sub(r"m_nAlphaMax = 50", "m_nAlphaMax = 100", new)
    new = re.sub(r"m_flSelfIllumAmount = 1\.0", "m_flSelfIllumAmount = 0.35", new, count=1)
    # Buoyant upward drift like Povarehok's rising plume (was m_Gravity = [0,0,0] -> sat at
    # the barrel). Spawn stays muzzle-anchored; only the post-spawn rise changes.
    new = _force_modern_smoke_rise(new)
    # Rifle barrel wisp lifetime/fade (user report + demo capture 2026-07-06): the converted
    # wisp died at 20% of its 5s lifespan (C_OP_FadeAndKill m_flEndFadeOutTime = 0.2) AND
    # faded from birth (m_flStartFadeOutTime = 0.0), so the AK/M4 muzzle smoke "disappeared
    # really quickly, didn't show all the way" and was already gone ~1s later when the player
    # reloaded/inspected -- leaving the follow PositionLock (barrel_smoke_trail_fp) nothing to
    # track. (The LMG/DMR barrel_smoke_plume already faded over its full life, which is why
    # only rifles/SMG showed it.) Hold full, then fade over the back half of a punchier ~3s
    # life so the wisp shows fully AND survives long enough to be followed through a reload/
    # inspect. m_flEndFadeOutTime = 1.0 keeps the kill at end-of-life (no wrap).
    new = re.sub(r"\bm_flStartFadeOutTime = [0-9.]+", "m_flStartFadeOutTime = 0.4", new, count=1)
    new = re.sub(r"\bm_flEndFadeOutTime = [0-9.]+", "m_flEndFadeOutTime = 1.0", new, count=1)
    new = re.sub(r"m_fLifetimeMin = [0-9.]+", "m_fLifetimeMin = 3.0", new)
    new = re.sub(r"m_fLifetimeMax = [0-9.]+", "m_fLifetimeMax = 3.0", new)
    return new


def patch_cs2_modern_rope_trail_alignment(text: str) -> str:
    """Modern barrel rope wisps: same FINAL recipe as Povarehok's (see
    common.patch_cs2_muzzle_rope_trail_alignment) -- WORLD-pass + stock brief 0->0.1s
    lock, so moving/reloading draws the lagging smoke sheet in the air while emission
    tracks the engine-driven muzzle CP. Viewmodel-pass + ride-the-gun locks (both tried
    2026-07-06) read as rigid camera-glued smoke.
    """
    new = text.replace("m_bLocalCoords = false", "m_bLocalCoords = true")
    new = new.replace("m_bViewModelEffect = true", "m_bViewModelEffect = false")
    new = new.replace(
        '_class = "C_OP_RenderSprites"\n\t\t\tm_bBlendFramesSeq0 = true',
        '_class = "C_OP_RenderRopes"',
    )
    new = new.replace('_class = "C_OP_RenderSprites"', '_class = "C_OP_RenderRopes"', 1)
    new = _ensure_cs2_modern_rope_trail_position_offset(new)
    new = _tune_modern_rope_trail_particles(new)
    new = common.ensure_brief_position_lock(new)
    new = common.remove_muzzle_follow_config(new)
    return new


def patch_cs2_modern_barrel_smoke_alignment(text: str) -> str:
    """Modern barrel_smoke(_plume): WORLD-pass + brief lock (2026-07-06 final; see
    patch_cs2_modern_rope_trail_alignment)."""
    new = text.replace("m_bLocalCoords = false", "m_bLocalCoords = true")
    new = new.replace("m_bViewModelEffect = true", "m_bViewModelEffect = false")
    new = _ensure_cs2_modern_rope_trail_position_offset(new)
    # Same bug as _tune_modern_rope_trail_particles (found 2026-07-04): forcing LOCAL
    # space re-anchors the plume's noise/movement to the moving muzzle every frame, so
    # the LMG/DMR black smoke plume sits glued to the barrel instead of drifting up into
    # world space. Force world space so the spawn point stays muzzle-anchored but the
    # post-spawn rise is independent.
    new = re.sub(r"m_bLocalSpace = (?:true|false)", "m_bLocalSpace = false", new)
    new = re.sub(
        r"m_vecOutputMin = \[[^\]]*\]",
        "m_vecOutputMin = [-1.0, -1.0, -0.5]",
        new,
        count=1,
    )
    new = re.sub(
        r"m_vecOutputMax = \[[^\]]*\]",
        "m_vecOutputMax = [1.0, 1.0, 0.5]",
        new,
        count=1,
    )
    # Match Povarehok: buoyant vertical rise. The converted plume came through with a mixed
    # forward+up gravity ([15,0,15]); force a clean upward vector so the LMG/DMR/sniper plume
    # lifts off the barrel the same way the rope wisps now do.
    new = _force_modern_smoke_rise(new)
    # Stock-style brief anchor (rewrites any experimental lock left on patched trees);
    # the reverted CP-config injection stays stripped.
    new = common.ensure_brief_position_lock(new)
    new = common.remove_muzzle_follow_config(new)
    return new


# Shell eject port puff (GMod arc9_shelleffect.lua spawns port_smoke/shellsmoke separately).
MODERN_SHELL_PORT_SMOKE_CANDIDATES = (
    "port_smoke.vpcf",
    "shellsmoke.vpcf",
    "port_smoke_small.vpcf",
)
# SEPARATE Modern casings (user directive 2026-07-07 "modern and pov have different casings;
# nothing shared"): Modern gets its OWN casing systems under modern/weapons/cs_weapon_fx/ that
# render the mw2019/ shell models + the Modern port_smoke eject puff. Povarehok keeps its own
# generic-mesh casings, unmodified. Previously Modern reused Povarehok's casing SYSTEMS and the
# Modern port_smoke was bolted onto them (two kinds of cross-pack sharing) -- both removed.
MODERN_CASING_DIR = "particles/filmmaker/modern/weapons/cs_weapon_fx"
# Casing TEMPLATES, committed under FX/sources/modern-warfare-gmod/templates/ and staged by the
# converter into <source1_root>/templates/. They used to be cloned live out of the converted
# Povarehok tree; the Insurgency 2 rebake (2026-09-01) retired that pack, and Insurgency's own
# shell_eject is a sprite puff with no model renderer, so there was nothing left to clone from.
# Freezing them as Modern's own assets is also what the "never couple" rule wants: Modern's
# casings are Modern's, and no longer reach into another pack's build output to exist.
CASING_TEMPLATE_SUBDIR = "templates/weapons/cs_weapon_fx"
# Child refs that pointed back into the retired Povarehok tree and must not survive the clone.
DEAD_PACK_CHILD_RE = re.compile(
    r'\t*\{\n[^{}]*resource:"particles/filmmaker/povarehok/[^"]*"[^{}]*\},\n')
# caliber -> mw2019 shell model the Modern casing renders (generic geometry -> mw2019 geometry)
MODERN_CASING_MODEL = {
    "weapon_shell_casing_9mm": "models/shells/mw2019/shell_pistol.vmdl",
    "weapon_shell_casing_rifle": "models/shells/mw2019/shell_rifle.vmdl",
    "weapon_shell_casing_deagle": "models/shells/mw2019/shell_pistol.vmdl",
    "weapon_shell_casing_shotgun": "models/shells/mw2019/shell_12gauge.vmdl",
    "weapon_shell_casing_50cal": "models/shells/mw2019/shell_50cal.vmdl",
}


def _resolve_modern_shell_port_smoke(root: Path) -> str | None:
    for name in MODERN_SHELL_PORT_SMOKE_CANDIDATES:
        res = f"{MODERN_MUZZLE_DIR}/{name}"
        if common.resource_path(root, res).is_file():
            return res
    return None


def patch_modern_shell_port_smoke(root: Path, changed: list[str],
                                  source1_root: Path | None = None) -> None:
    """Build Modern's OWN casing systems (mw2019 mesh) from the committed templates.

    Idempotent. See MODERN_CASING_* above and ParticleFxRules.cpp's separated
    FXRULE_MODERN shell-casing rows."""
    port_smoke = _resolve_modern_shell_port_smoke(root)
    if source1_root is None:
        return
    template_dir = Path(source1_root) / CASING_TEMPLATE_SUBDIR
    # 1. create/refresh each Modern casing from its committed template.
    for name, mw_model in MODERN_CASING_MODEL.items():
        src = template_dir / f"{name}.vpcf"
        if not src.is_file():
            continue
        t = src.read_text(encoding="utf-8")
        # The templates were captured from a build that still had the Povarehok tree, so
        # they carry child/fallback refs into it. Those systems no longer exist.
        t = DEAD_PACK_CHILD_RE.sub("", t)
        t = re.sub(r'm_model = resource:"models/shells/[^"]*"',
                   f'm_model = resource:"{mw_model}"', t)
        # drop the Povarehok fallback cross-ref (self-contained; casing count is tiny in a movie)
        t = re.sub(r'\tm_hFallback = resource:"[^"]*"\n', "", t)
        t = re.sub(r'\tm_nFallbackMaxCount = \d+\n', "", t)
        # NO eject-port smoke child. It was added as an MW2019 flourish and is MISPLACED in game
        # (user report 2026-09-01, screenshots on MP5/Glock/CZ75): port_smoke sets
        # m_nControlPointNumber = 0, so as a CHILD of the casing it inherits the parent's CP0 --
        # the weapon ORIGIN, not the casing's ejection position. On a viewmodel CP0 sits at the
        # grip/receiver, so the puff renders at the BACK of the gun on every weapon instead of at
        # the ejection port. The muzzle flashes carry their own muzzle_smoke_* children, which are
        # anchored to the muzzle and are unaffected, so removing this loses nothing that was
        # correctly placed. Strip it from any tree a previous build already patched.
        common.write_if_different(root, f"{MODERN_CASING_DIR}/{name}.vpcf", t, changed)
        dst = common.resource_path(root, f"{MODERN_CASING_DIR}/{name}.vpcf")
        if common.remove_child_refs(dst, {"port_smoke.vpcf", "shellsmoke.vpcf", "port_smoke_small.vpcf"}):
            changed.append(f"{MODERN_CASING_DIR}/{name}.vpcf")
    # (The old step 2 de-contaminated the Povarehok casings that Modern used to be cloned
    # from -- stripping Modern's port smoke back out of them and restoring their generic
    # shotgun mesh. With that pack retired there is no second set of casings to clean, and
    # On/Less now pass shell casings through to vanilla CS2's own model-based brass.)


def patch_cs2_modern_muzzleflash_alignment(text: str) -> str:
    """Modern per-shot muzzle flash: WORLD-space, muzzle-attachment anchored.

    Bug (user report 2026-07-04, "modern muzzle flash unaligns when you move the
    camera; there's no third-person flash at all"): this used to force
    m_bViewModelEffect = true. A viewmodel-effect system renders ONLY in the
    first-person viewmodel pass -- it is drawn at the viewmodel's position with
    viewmodel FOV, which is fine while the camera sits on the spectated eye but
    floats out into mid-air the instant the view detaches (third-person orbit /
    free cam), and never produces a world-anchored flash on the actual weapon.
    Because the SAME asset is the swap target for both the first-person (_fps) and
    the third-person (world) CS2 muzzle systems (kVariantWeaponFx), forcing the
    viewmodel flag broke BOTH camera-detached cases at once.

    Povarehok's own world muzzle flashes (weapon_muzzle_flash_*.vpcf) never set
    this flag -- they render in WORLD space and anchor to the weapon's muzzle
    control point, so they look right from every camera (first person, third
    person, free cam) with no extra machinery. Match that here: leave the flash
    world-space (viewmodel effect OFF) and keep only the muzzle-local spawn offset
    so it still sits at the barrel tip. First-person in-eye spectating still shows
    it correctly (the control point is at the viewmodel muzzle then, exactly as it
    is for Povarehok); detached cameras now get a real world flash via the existing
    _fps-suppress + spec_mode-3 chase path (see Filmmaker.cpp RunMainThreadFrame).
    """
    new = text.replace("m_bLocalCoords = false", "m_bLocalCoords = true")
    # World-space, NOT a viewmodel effect (see the docstring). Clear any inherited
    # true and never re-add the flag; an absent key defaults to false = world-space,
    # which is exactly what Povarehok's flashes carry.
    new = new.replace("m_bViewModelEffect = true", "m_bViewModelEffect = false")
    new = _ensure_cs2_modern_rope_trail_position_offset(new)
    new = re.sub(
        r"m_LocalCoordinateSystemSpeedMin = \[[^\]]*\]",
        "m_LocalCoordinateSystemSpeedMin = [0.0, 0.0, 0.0]",
        new,
    )
    new = re.sub(
        r"m_LocalCoordinateSystemSpeedMax = \[[^\]]*\]",
        "m_LocalCoordinateSystemSpeedMax = [0.0, 0.0, 0.0]",
        new,
    )
    return new


def _fp_variant_res(res: str) -> str:
    """particles/.../muzzleflash_ar.vpcf -> particles/.../muzzleflash_ar_fp.vpcf."""
    assert res.endswith(".vpcf"), res
    return res[:-len(".vpcf")] + "_fp.vpcf"


def make_modern_muzzleflash_fp(world_text: str) -> str:
    """First-person viewmodel twin of a world muzzle flash.

    Identical to the world flash except drawn in the viewmodel pass
    (m_bViewModelEffect = true) so it lines up with the foreshortened first-person weapon,
    exactly how CS2 (and Povarehok) ship a separate _fp flash. The world twin keeps
    viewmodel OFF so it anchors on the weapon for third person / free cam. The _fps CS2
    source systems route here; the world systems route to the world twin.
    """
    new = world_text.replace("m_bViewModelEffect = false", "m_bViewModelEffect = true")
    return common._ensure_viewmodel_effect_flag(new)


def _rewrite_modern_smoke_fp_child_refs(text: str) -> str:
    """Point wrapper children at _fp trail twins (barrel_smoke -> barrel_smoke_trail_b_fp)."""
    for base in ("barrel_smoke_trail_b", "barrel_smoke_trail", "barrel_smoke_plume", "barrel_smoke"):
        text = text.replace(f"/{base}.vpcf", f"/{base}_fp.vpcf")
    return text


# The "game" control-point config CS2's own first-person weapon FX use (verified by
# decompiling uweapon_muzflsh_ak47_fps.vpcf): it makes the ENGINE drive control point 0 to
# continuously follow the weapon's "muzzle_flash" attachment. Without it, a swapped viewmodel
# effect's CP0 is set once (at the muzzle-flash create event) and never updated, so the smoke
# stays where it was fired. WITH it, the emission SOURCE tracks the moving muzzle -- new smoke
# comes off the barrel as the gun moves -- while already-emitted particles still fly free in
# world space (see make_modern_smoke_fp). This is exactly how stock weapon_muzzle_smoke works.
_FP_MUZZLE_FOLLOW_CONFIG = (
    '\tm_controlPointConfigurations = \n'
    "\t[\n"
    "\t\t{\n"
    '\t\t\tm_name = "game"\n'
    "\t\t\tm_drivers = \n"
    "\t\t\t[\n"
    "\t\t\t\t{\n"
    '\t\t\t\t\tm_iAttachType = "PATTACH_POINT_FOLLOW"\n'
    '\t\t\t\t\tm_attachmentName = "muzzle_flash"\n'
    '\t\t\t\t\tm_entityName = "self"\n'
    "\t\t\t\t},\n"
    "\t\t\t]\n"
    "\t\t},\n"
    "\t]\n"
)


def _add_fp_muzzle_follow_config(text: str) -> str:
    if "m_controlPointConfigurations" in text:
        return text
    anchor = '\t_class = "CParticleSystemDefinition"\n'
    if anchor not in text:
        return text
    return text.replace(anchor, anchor + _FP_MUZZLE_FOLLOW_CONFIG, 1)


def make_modern_smoke_fp(world_text: str) -> str:
    """First-person viewmodel twin of world barrel smoke / rope wisp assets.

    RECIPE (2026-07-07, user directive "Povarehok looks better -- whatever Povarehok is
    doing, do it for Modern too"): the `_fp` twin rides the barrel through reload/inspect via
    a FULL-LIFETIME C_OP_PositionLock (0 -> 1e6) in the viewmodel pass, with NO
    `m_controlPointConfigurations` follow driver. This is the recipe the shipped Povarehok
    fp smoke (weapon_muzzle_smoke_long_fp) already used and that the user judged as following
    the gun better than Modern's previous brief-lock + follow-config approach. The full lock
    ADDS the muzzle CP's translation on top of the particle's own buoyant rise/noise (it does
    NOT freeze them -- see common.ensure_full_position_lock), so the wisp still lifts, it just
    lifts relative to the moving barrel = follows during reload/inspect.

    HISTORY: an earlier iteration used the brief 0->0.1s lock + a PATTACH_POINT_FOLLOW "game"
    driver so already-emitted particles flew free in world space. The user's final call
    (2026-07-07) is that Povarehok's full-lock look follows better, so both packs now use it
    (Povarehok's fp smoke is built through this same function -- postprocess_povarehok.py
    lines ~916/927 -- so this keeps the two packs consistent). `remove_muzzle_follow_config`
    strips the reverted follow driver from any already-patched tree."""
    new = world_text.replace("m_bViewModelEffect = false", "m_bViewModelEffect = true")
    new = common._ensure_viewmodel_effect_flag(new)
    new = common.remove_muzzle_follow_config(new)   # strip the reverted follow driver
    new = common.ensure_full_position_lock(new)     # ride the barrel through reload (Povarehok recipe)
    return _rewrite_modern_smoke_fp_child_refs(new)


def _ensure_modern_flash_position_lock(text: str) -> str:
    if 'C_OP_PositionLock' in text:
        return text
    element = (
        "\t\t{\n"
        '\t\t\t_class = "C_OP_PositionLock"\n'
        "\t\t},\n"
    )
    match = re.search(r"m_Operators = \n(\t*)\[\n", text)
    if match:
        return text[:match.end()] + element + text[match.end():]
    anchor = re.search(r'_class = "CParticleSystemDefinition"\n', text)
    if not anchor:
        return text
    block = "\tm_Operators = \n\t[\n" + element + "\t]\n\n"
    return text[:anchor.end()] + block + text[anchor.end():]


def patch_modern_flash_burst(text: str) -> str:
    """Retarget old HL2 sprite bursts and keep them locked to the muzzle."""
    edits = []
    for start, end in common.iter_renderer_blocks(text):
        block = text[start:end]
        if '_class = "C_OP_RenderSprites"' not in block:
            continue
        if MODERN_FLASH_BURST_OLD_TEXTURE not in block:
            continue
        replacement = block.replace(
            MODERN_FLASH_BURST_OLD_TEXTURE,
            MODERN_FLASH_BURST_NEW_TEXTURE,
        )
        edits.append((start, end, replacement))
    for start, end, replacement in reversed(edits):
        text = text[:start] + replacement + text[end:]
    return _ensure_modern_flash_position_lock(text) if edits else text


# Emit counts converted out of Source 1 can carry a NEGATIVE random minimum. The worst are the
# muzzle-flash roots: muzzleflash_ar.vpcf ships
#
#     m_nParticlesToEmit = { PF_TYPE_RANDOM_UNIFORM, m_flRandomMin = -1, m_flRandomMax = 1 }
#
# and a draw of <= 0 emits nothing, so roughly HALF of all rifle shots had no muzzle flash at
# all. It was reported as a low-timescale bug because slow motion gives every surviving flash
# ~10x more captured frames, which makes the missing ones obvious -- but the dropout is there at
# any speed, and it is weapon-dependent (max=1 systems lose ~50%, max=15 ones ~6%).
#
# Only the MINIMUM is clamped, deliberately. patch_modern_tracer_cs2_discipline force-writes
# m_nParticlesToEmit = 1, which is right for a single tracer head but would turn a 65-particle
# shrapnel burst into one particle. Raising just the floor keeps every authored range and its
# variance and removes only the impossible negative part.
_EMIT_COUNT_BLOCK_RE = re.compile(
    r"m_nParticlesToEmit\s*=\s*\n\s*\{(.*?)\}",
    re.DOTALL,
)

# MW2019 flash grenade. The hook creates each piece as its own system at the detonation
# (AfxHookSource2/Filmmaker/Movie/ParticleFxFlashbang.cpp), because children created through
# the game particle manager land at the world origin. generic_explo_flash_core.vpcf is the
# parent's own glow without its m_Children; the children are used as they are.
MW2019_FLASH_DIR = "particles/filmmaker/modern/mw2019_explosions_pak"
# Two debris sprites name the plain .vtex, which nothing compiles; their vmats compile the same
# image under a hashed name, so the particle points there (else: magenta missing-texture squares).
MW2019_FLASH_TEXTURES = {
    f"{MW2019_FLASH_DIR}/generic_explo_high_debris.vpcf": (
        "materials/decals/debris_concrete001b.vtex", "materials/decals/debris_concrete001b_tga_c8049771.vtex"),
    f"{MW2019_FLASH_DIR}/generic_explo_high_debris_sheet.vpcf": (
        "materials/effects/fas_debris.vtex", "materials/effects/fas_debris_tga_5003a8c2.vtex"),
}


def _without_children(text: str) -> str:
    start = text.find("m_Children")
    if start < 0:
        return text
    depth = 0
    i = text.index("[", start)
    while True:
        if text[i] == "[":
            depth += 1
        elif text[i] == "]":
            depth -= 1
            if depth == 0:
                break
        i += 1
    line_start = text.rfind("\n", 0, start) + 1
    return text[:line_start] + text[text.index("\n", i) + 1:]


# Timing. Every piece is created at the same instant, and the GMod source gives every child a
# zero delay too, but as authored the pop was staged and slow (user report 2026-10-09): the core
# flash lived 0.1 s and GREW from size 0, so it peaked as it vanished; the shockwave smoke faded in
# over 0.25 s; the debris sheet started 0.125 s late. Make it burst at once: both glows start at
# full size, the core lives 0.15 s, the shockwave fades in over 0.08 s, the sheet leaves with the
# sparks. (pattern, replacement) pairs, applied to the generated sources.
MW2019_FLASH_TIMING = {
    f"{MW2019_FLASH_DIR}/generic_explo_flash_core.vpcf": [
        (r"(m_flStartScale = )0\.0(\r?\n)", r"\g<1>1.0\g<2>"),
        (r"(m_fLifetimeM(?:in|ax) = )0\.1(\r?\n)", r"\g<1>0.15\g<2>"),
    ],
    f"{MW2019_FLASH_DIR}/generic_explo_flash_glow2.vpcf": [
        (r"(m_flStartScale = )0\.0(\r?\n)", r"\g<1>1.0\g<2>"),
    ],
    f"{MW2019_FLASH_DIR}/generic_explo_high_smoke_shock.vpcf": [
        (r"(m_flFadeInTimeM(?:in|ax) = )0\.25(\r?\n)", r"\g<1>0.08\g<2>"),
    ],
    f"{MW2019_FLASH_DIR}/generic_explo_high_debris_sheet.vpcf": [
        (r"(m_flStartTime = )0\.125(\r?\n)", r"\g<1>0.012\g<2>"),
    ],
}


def patch_mw2019_flashbang_pieces(root: Path, changed: list[str]) -> None:
    for res, (old, new) in MW2019_FLASH_TEXTURES.items():
        path = common.resource_path(root, res)
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        new_text = text.replace(f'"{old}"', f'"{new}"')
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
            changed.append(res)
    parent = common.resource_path(root, f"{MW2019_FLASH_DIR}/generic_explo_flash.vpcf")
    if parent.is_file():
        common.write_if_different(root, f"{MW2019_FLASH_DIR}/generic_explo_flash_core.vpcf",
                                  _without_children(parent.read_text(encoding="utf-8")), changed)
    for res, edits in MW2019_FLASH_TIMING.items():
        path = common.resource_path(root, res)
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        new_text = text
        for pattern, replacement in edits:
            new_text = re.sub(pattern, replacement, new_text)
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
            if res not in changed:
                changed.append(res)


_EMIT_COUNT_MIN_RE = re.compile(r"m_flRandomMin\s*=\s*(-?\d+(?:\.\d+)?)")
_EMIT_COUNT_MAX_RE = re.compile(r"m_flRandomMax\s*=\s*(-?\d+(?:\.\d+)?)")


def clamp_emit_count_minimums(text: str) -> str:
    """Raise any m_nParticlesToEmit random minimum below 1 up to 1.

    Idempotent: an already-clamped block is skipped, so this is safe in the in-place
    --gameplay-composites-only entry as well as in a fresh conversion.
    """
    pieces: list[str] = []
    last = 0
    for match in _EMIT_COUNT_BLOCK_RE.finditer(text):
        body = match.group(1)
        lo = _EMIT_COUNT_MIN_RE.search(body)
        hi = _EMIT_COUNT_MAX_RE.search(body)
        if not lo or not hi:
            continue
        # Already safe, or a range whose max is below 1 -- never invert min > max.
        if float(lo.group(1)) >= 1.0 or float(hi.group(1)) < 1.0:
            continue
        start = match.start(1) + lo.start(1)
        end = match.start(1) + lo.end(1)
        pieces.append(text[last:start])
        pieces.append("1")
        last = end
    if not pieces:
        return text
    pieces.append(text[last:])
    return "".join(pieces)


def _remove_init_blocks(text: str, class_name: str) -> str:
    """Delete every initializer/operator array element of the given _class, comma and all."""
    needle = f'_class = "{class_name}"'
    while True:
        idx = text.find(needle)
        if idx < 0:
            return text
        start, end = common.block_span(text, idx)
        line_start = text.rfind("\n", 0, start) + 1
        j = end
        if j < len(text) and text[j] == ",":
            j += 1
        if j < len(text) and text[j] == "\n":
            j += 1
        text = text[:line_start] + text[j:]


def _insert_after_class_block(text: str, class_name: str, new_block: str) -> str:
    m = re.search(rf'_class = "{re.escape(class_name)}"', text)
    if not m:
        return text
    _, end = common.block_span(text, m.start())
    j = end
    if j < len(text) and text[j] == ",":
        j += 1
    if j < len(text) and text[j] == "\n":
        j += 1
    return text[:j] + new_block + text[j:]


def _force_tracer_spawn_at_muzzle(text: str) -> str:
    """GMod sets CP0 at the muzzle via Lua -- trust engine CP0, zero forward kick."""
    edits = []
    for match in re.finditer(r'_class = "C_INIT_PositionOffset"', text):
        start, end = common.block_span(text, match.start())
        block = text[start:end]
        block = re.sub(r"m_bLocalCoords = (?:true|false)", "m_bLocalCoords = true", block)
        block = re.sub(r"m_OffsetMin = \[[^\]]*\]", "m_OffsetMin = [0.0, 0.0, 0.0]", block)
        block = re.sub(r"m_OffsetMax = \[[^\]]*\]", "m_OffsetMax = [0.0, 0.0, 0.0]", block)
        edits.append((start, end, block))
    for start, end, replacement in reversed(edits):
        text = text[:start] + replacement + text[end:]
    return text


def _tracer_pf_literal_field(field: str, value: str) -> str:
    return (
        f"\t\t\t{field} = \n"
        "\t\t\t{\n"
        '\t\t\t\tm_nType = "PF_TYPE_LITERAL"\n'
        f"\t\t\t\tm_flLiteralValue = {value}\n"
        "\t\t\t}\n"
    )


def _upsert_render_trails_endpoint_fade(block: str) -> str:
    """Along-trail alpha taper at head and tail (fixes hard rectangular streak ends)."""
    block = _TRACER_PF_LITERAL_BLOCK_RE.sub("", block)
    block = re.sub(
        r"(?m)^\t\t\tm_fl(?:Head|Tail)AlphaScale = \S+\n",
        "",
        block,
    )
    block = re.sub(
        r"(?m)^\t\t\tm_flRadiusHeadTaper = \S+\n",
        "",
        block,
    )
    insert = (
        _tracer_pf_literal_field("m_flHeadAlphaScale", _TRACER_HEAD_ALPHA_SCALE)
        + _tracer_pf_literal_field("m_flTailAlphaScale", _TRACER_TAIL_ALPHA_SCALE)
        + _tracer_pf_literal_field("m_flRadiusHeadTaper", _TRACER_RADIUS_HEAD_TAPER)
    )
    anchor = "m_flLengthFadeInTime"
    if anchor in block:
        line_end = block.find("\n", block.find(anchor))
        return block[: line_end + 1] + insert + block[line_end + 1 :]
    anchor = "m_flMaxLength"
    if anchor in block:
        line_end = block.find("\n", block.find(anchor))
        return block[: line_end + 1] + insert + block[line_end + 1 :]
    return block.replace(
        '_class = "C_OP_RenderTrails"',
        '_class = "C_OP_RenderTrails"\n' + insert,
        1,
    )


def patch_modern_tracer_trail_endpoint_fade(text: str) -> str:
    """Soft fade at both ends of the streak (RenderTrails head/tail alpha scale)."""
    edits = []
    for start, end in common.iter_renderer_blocks(text):
        block = text[start:end]
        if "C_OP_RenderTrails" not in block:
            continue
        new_block = _upsert_render_trails_endpoint_fade(block)
        if new_block != block:
            edits.append((start, end, new_block))
    for start, end, replacement in sorted(edits, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text


def patch_modern_tracer_mw_streak_texture(text: str) -> str:
    """Use the pack's tracer_middle streak on the AR parent (spark reads as a hard bar)."""
    return (
        text.replace(
            'm_hMaterial = resource:"materials/effects/spark.vmat"',
            'm_hMaterial = resource:"materials/effects/tracer_middle.vmat"',
        ).replace(
            'm_hTexture = resource:"materials/effects/spark.vtex"',
            'm_hTexture = resource:"materials/effects/tracer_middle.vtex"',
        )
    )


def patch_modern_tracer_soft_head(text: str, trail_length: float) -> str:
    """Longer RenderTrails segments so LengthFadeInTime reads as a soft streak head."""
    length = f"{trail_length:.1f}".rstrip("0").rstrip(".")
    edits = []
    for start, end in common.iter_renderer_blocks(text):
        block = text[start:end]
        if "C_OP_RenderTrails" not in block:
            continue
        new_block = block
        if re.search(r"m_flMaxLength = ", new_block):
            new_block = re.sub(r"m_flMaxLength = \S+", f"m_flMaxLength = {length}", new_block)
        if re.search(r"m_flMinLength = ", new_block):
            new_block = re.sub(r"m_flMinLength = \S+", f"m_flMinLength = {length}", new_block)
        if new_block != block:
            edits.append((start, end, new_block))
    for start, end, replacement in sorted(edits, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text


# The MW2019 tracer wisp ropes (mgbase_tracer_trail{,_faint}) animate in GMod by
# V-SCROLLING the fas_smoke_beam texture along the ribbon. The converted files carry
# m_flTextureVScrollRate = -50 (the current engine schema still has the field on
# C_OP_RenderRopes, offset 0x2F90 build in reference/cs2-offsets), but with NO explicit
# m_flTextureVWorldSize the V mapping leaves the scroll imperceptible -- the wisps read
# as static (user report 2026-07-06: "the wisps from the bullet trails are supposed to
# be animated and they're not"). One texture repeat per 128 units at -50 u/s ~= 0.4
# repeats/s of visible flow along the trail.
_TRACER_TRAIL_V_WORLD_SIZE = "128.0"


def patch_modern_tracer_trail_scroll(text: str) -> str:
    """Explicit V-world-size on scrolling rope renderers so the beam scroll shows."""
    edits = []
    for start, end in common.iter_renderer_blocks(text):
        block = text[start:end]
        if "C_OP_RenderRopes" not in block or "m_flTextureVScrollRate" not in block:
            continue
        if re.search(r"m_flTextureVWorldSize = ", block):
            new_block = re.sub(
                r"m_flTextureVWorldSize = \S+",
                f"m_flTextureVWorldSize = {_TRACER_TRAIL_V_WORLD_SIZE}",
                block,
            )
        else:
            new_block = re.sub(
                r"(?m)^(\s*)(m_flTextureVScrollRate = [^\n]+)$",
                rf"\1\2\n\1m_flTextureVWorldSize = {_TRACER_TRAIL_V_WORLD_SIZE}",
                block,
                count=1,
            )
        if new_block != block:
            edits.append((start, end, new_block))
    for start, end, replacement in sorted(edits, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text


def patch_modern_tracer_trail_child_soft_head(text: str) -> str:
    """MW2019 rope child: ease the beam color in at the head (GMod-style soft leading edge)."""
    edits = []
    for match in re.finditer(r'_class = "C_OP_ColorInterpolate"', text):
        start, end = common.block_span(text, match.start())
        block = text[start:end]
        new_block = re.sub(r"m_bEaseInOut = \S+", "m_bEaseInOut = true", block)
        new_block = re.sub(r"m_flFadeStartTime = \S+", "m_flFadeStartTime = 0.0", new_block)
        new_block = re.sub(r"m_flFadeEndTime = \S+", "m_flFadeEndTime = 0.25", new_block)
        if new_block != block:
            edits.append((start, end, new_block))
    for start, end, replacement in sorted(edits, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text


def _ensure_gmod_tracer_parent_lifecycle(text: str) -> str:
    """GMod mw2019_tracer_3 parent: LifespanFromVelocity + Decay, not FadeAndKillForTracers."""
    text = _remove_init_blocks(text, "C_OP_FadeAndKillForTracers")

    if "C_OP_Decay" not in text:
        decay_block = (
            "\t\t{\n"
            '\t\t\t_class = "C_OP_Decay"\n'
            "\t\t},\n"
        )
        text = _insert_after_class_block(text, "C_OP_BasicMovement", decay_block)

    if "C_INIT_LifespanFromVelocity" not in text:
        lifespan_block = (
            "\t\t{\n"
            '\t\t\t_class = "C_INIT_LifespanFromVelocity"\n'
            "\t\t\tm_flMaxTraceLength = 8192.0\n"
            "\t\t},\n"
        )
        anchor = "C_INIT_MoveBetweenPoints" if "C_INIT_MoveBetweenPoints" in text else "C_INIT_PositionOffset"
        text = _insert_after_class_block(text, anchor, lifespan_block)

    return text


def patch_modern_tracer_cs2_discipline(text: str) -> str:
    """MW2019 tracer look with CS2 motion fixes and GMod parent lifecycle (MODERN_TRACER_PARENTS).

    Keeps MW2019 renderer textures and child glow/trail systems. Fixes broken emitters and
    MoveBetweenPoints speed while restoring GMod's LifespanFromVelocity + Decay stack (GMod
    never shipped FadeAndKillForTracers — that was a Povarehok-only CS2 addition).
    """
    text = _remove_init_blocks(text, "C_INIT_MoveBetweenPoints")

    m = re.search(r'_class = "C_INIT_CreateWithinSphere"', text)
    if m:
        start, end = common.block_span(text, m.start())
        block = text[start:end]
        block = re.sub(
            r"m_LocalCoordinateSystemSpeedMin = \[[^\]]*\]",
            "m_LocalCoordinateSystemSpeedMin = [0.0, 0.0, 0.0]",
            block,
        )
        block = re.sub(
            r"m_LocalCoordinateSystemSpeedMax = \[[^\]]*\]",
            "m_LocalCoordinateSystemSpeedMax = [0.0, 0.0, 0.0]",
            block,
        )
        block = re.sub(r"m_fSpeedRandExp = \S+", "m_fSpeedRandExp = 0.0", block)
        block = re.sub(r"m_vecDistanceBias = \[[^\]]*\]", "m_vecDistanceBias = [0.0, 0.0, 0.0]", block)
        block = re.sub(r"m_fSpeedMin = \S+", "m_fSpeedMin = 0.0", block)
        block = re.sub(r"m_fSpeedMax = \S+", "m_fSpeedMax = 0.0", block)
        text = text[:start] + block + text[end:]

    move_block = (
        "\t\t{\n"
        '\t\t\t_class = "C_INIT_MoveBetweenPoints"\n'
        f"\t\t\tm_flSpeedMin = {_TRACER_MOVE_SPEED}\n"
        f"\t\t\tm_flSpeedMax = {_TRACER_MOVE_SPEED}\n"
        "\t\t},\n"
    )
    if "C_INIT_MoveBetweenPoints" not in text:
        anchor = "C_INIT_PositionOffset" if "C_INIT_PositionOffset" in text else "C_INIT_CreateWithinSphere"
        text = _insert_after_class_block(text, anchor, move_block)

    if "C_OP_RenderTrails" in text and "C_INIT_RandomTrailLength" not in text:
        trail_block = (
            "\t\t{\n"
            '\t\t\t_class = "C_INIT_RandomTrailLength"\n'
            "\t\t\tm_flLengthRandExponent = 2.0\n"
            "\t\t\tm_flMaxLength = 0.093\n"
            "\t\t\tm_flMinLength = 0.084\n"
            "\t\t},\n"
        )
        text = _insert_after_class_block(text, "C_INIT_MoveBetweenPoints", trail_block)

    text = _ensure_gmod_tracer_parent_lifecycle(text)
    text = patch_modern_tracer_trail_length_fade(text)
    text = _force_tracer_spawn_at_muzzle(text)

    text = re.sub(
        r"\t\t\tm_nParticlesToEmit = \n\t\t\t\{[^}]+\}\n",
        "\t\t\tm_nParticlesToEmit = 1\n",
        text,
        flags=re.DOTALL,
    )
    text = re.sub(r"m_nParticlesToEmit = -?\d+", "m_nParticlesToEmit = 1", text)
    text = re.sub(r"m_nMaxEmittedPerFrame = -?\d+", "m_nMaxEmittedPerFrame = 1", text)
    text = re.sub(
        r"\t\t\tm_flStartTime = \n\t\t\t\{[^}]+\}\n",
        "",
        text,
        flags=re.DOTALL,
    )
    text = re.sub(r"m_nMaxParticles = 2", "m_nMaxParticles = 1", text)
    return text


# Legacy name used by docs / earlier commits.
patch_modern_tracer_forward = patch_modern_tracer_cs2_discipline


def _format_tracer_scaled_number(source: str, value: float) -> str:
    if "." not in source and value.is_integer():
        return str(int(value))
    return f"{value:.6f}".rstrip("0").rstrip(".")


def _scale_tracer_numeric_fields(
    block: str, fields: tuple[str, ...], scale: float, *, integer: bool
) -> str:
    field_group = "|".join(re.escape(field) for field in fields)
    pattern = re.compile(rf"(?m)^(\s*(?:{field_group}) = )(-?\d+(?:\.\d+)?)(\s*)$")

    def repl(match: re.Match[str]) -> str:
        scaled = float(match.group(2)) * scale
        if integer:
            value = str(max(0, round(scaled)))
        else:
            value = _format_tracer_scaled_number(match.group(2), scaled)
        return f"{match.group(1)}{value}{match.group(3)}"

    return pattern.sub(repl, block)


def patch_modern_tracer_glow_brightness(text: str) -> str:
    """Halve MW2019 tracer glow/trail brightness (alpha, radius, self-illum). ONCE per file:
    marker-fenced, because it multiplies (see TRACER_BRIGHTNESS_MARKER)."""
    if TRACER_BRIGHTNESS_MARKER in text:
        return text
    _orig = text  # the edit loop below rewrites `text` in place
    scale = TRACER_GLOW_BRIGHTNESS_SCALE
    edits = []
    for start, end in common.iter_renderer_blocks(text):
        block = text[start:end]
        if "C_OP_RenderTrails" not in block and "C_OP_RenderSprites" not in block and "C_OP_RenderRopes" not in block:
            continue
        new_block = _scale_tracer_numeric_fields(
            block,
            ("m_nAlphaMin", "m_nAlphaMax"),
            scale,
            integer=True,
        )
        new_block = _scale_tracer_numeric_fields(
            new_block,
            ("m_flRadiusMin", "m_flRadiusMax", "m_flConstantRadius", "m_flStartScale", "m_flEndScale"),
            scale,
            integer=False,
        )
        new_block = re.sub(
            r"(?m)^(\s*m_flSelfIllumAmount = )1\.0(\s*)$",
            rf"\g<1>{_format_tracer_scaled_number('1.0', scale)}\2",
            new_block,
        )
        if new_block != block:
            edits.append((start, end, new_block))
    for class_name in ("C_INIT_RandomAlpha", "C_INIT_RandomRadius", "C_OP_InterpolateRadius"):
        for match in re.finditer(rf'_class = "{class_name}"', text):
            start, end = common.block_span(text, match.start())
            block = text[start:end]
            integer = class_name == "C_INIT_RandomAlpha"
            fields = (
                ("m_nAlphaMin", "m_nAlphaMax")
                if integer
                else ("m_flRadiusMin", "m_flRadiusMax", "m_flStartScale", "m_flEndScale")
            )
            new_block = _scale_tracer_numeric_fields(block, fields, scale, integer=integer)
            if new_block != block:
                edits.append((start, end, new_block))
    if "m_flConstantRadius = " in text:
        for match in re.finditer(r"(?m)^(\s*m_flConstantRadius = )(-?\d+(?:\.\d+)?)(\s*)$", text):
            scaled = float(match.group(2)) * scale
            replacement = (
                f"{match.group(1)}{_format_tracer_scaled_number(match.group(2), scaled)}{match.group(3)}"
            )
            edits.append((match.start(), match.end(), replacement))
    for start, end, replacement in sorted(edits, reverse=True):
        text = text[:start] + replacement + text[end:]
    _out = text
    if _out != _orig:
        _out = _out.rstrip(chr(10)) + chr(10) + TRACER_BRIGHTNESS_MARKER + chr(10)
    return _out


def patch_modern_tracer_trail_length_fade(text: str) -> str:
    """RenderTrails soft streak head via m_flLengthFadeInTime (GMod render_sprite_trail)."""
    edits = []
    for start, end in common.iter_renderer_blocks(text):
        block = text[start:end]
        if "C_OP_RenderTrails" not in block:
            continue
        if re.search(r"m_flLengthFadeInTime = ", block):
            new_block = re.sub(
                r"m_flLengthFadeInTime = \S+",
                f"m_flLengthFadeInTime = {_TRACER_TRAIL_LENGTH_FADE_IN}",
                block,
            )
        else:
            indent = re.search(r"(?m)^(\s*)m_flMaxLength", block)
            prefix = indent.group(1) if indent else "\t\t\t"
            new_block = block.replace(
                '_class = "C_OP_RenderTrails"',
                '_class = "C_OP_RenderTrails"\n'
                f"{prefix}m_flLengthFadeInTime = {_TRACER_TRAIL_LENGTH_FADE_IN}",
                1,
            )
        if new_block != block:
            edits.append((start, end, new_block))
    for start, end, replacement in sorted(edits, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text


# Legacy alias.
patch_modern_tracer_muzzle_fade = patch_modern_tracer_trail_length_fade


def patch_modern_tracer_class_tiers(root: Path, changed: list[str]) -> None:
    """Keep sniper-only MW2019 tracer children off the automatic-weapon parent."""
    for res in MODERN_TRACER_AR_TIER:
        path = common.resource_path(root, res)
        if path.is_file() and common.remove_child_refs(path, set(SNIPER_ONLY_TRACER_CHILDREN)):
            changed.append(res)


def patch_modern_tracer_glow_leaves(root: Path, changed: list[str]) -> None:
    """Apply brightness + muzzle fade to routed MW2019 tracer child glow/trail leaves."""
    tracer_dir = common.resource_path(root, MODERN_TRACER_DIR)
    if not tracer_dir.is_dir():
        return
    names = set(MODERN_TRACER_GLOW_LEAVES)
    for path in tracer_dir.glob("*.vpcf"):
        if path.name not in names:
            continue
        text = path.read_text(encoding="utf-8")
        new_text = patch_modern_tracer_glow_brightness(text)
        if path.name in MODERN_TRACER_TRAIL_CHILD_SOFT_HEAD:
            new_text = patch_modern_tracer_trail_child_soft_head(new_text)
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
            changed.append(path.relative_to(root).as_posix())


def apply_modern_gameplay_composites(root: Path, *, tune_tracer_brightness: bool = True,
                                     source1_root: Path | None = None) -> list[str]:
    """Returns the content-relative resources that were written/changed.

    tune_tracer_brightness: patch_modern_tracer_glow_brightness MULTIPLIES alpha/radius
    by TRACER_GLOW_BRIGHTNESS_SCALE, so it is NOT idempotent -- it must run exactly once,
    in the full (fresh-conversion) pipeline, like the tone_down passes. In-place patch
    entries (--gameplay-composites-only) must pass False or every re-run visibly dims
    the tracers again (caught 2026-07-06: an in-place run halved the already-halved
    values a second time).
    """
    changed: list[str] = []

    for res, params in MVM_HEATWAVES.items():
        common.write_if_different(root, res, _distort_system_text(**params), changed)

    for res, params in MVM_GRENADE_TRAILS.items():
        common.write_if_different(root, res, _trail_system_text(**params), changed)

    muzzle_root = common.resource_path(root, MODERN_MUZZLE_DIR)
    if muzzle_root.is_dir():
        for path in muzzle_root.glob("*.vpcf"):
            if path.name.lower().startswith("testd"):
                continue
            text = path.read_text(encoding="utf-8")
            new_text = patch_modern_flash_burst(text)
            if new_text != text:
                path.write_text(new_text, encoding="utf-8")
                changed.append(path.relative_to(root).as_posix())

    # Emit counts that can draw <= 0 (see clamp_emit_count_minimums). Its own pass over its own
    # file set rather than a line inside one of the loops above: this is not a muzzle-flash fix,
    # an explosion fix or a tracer fix -- it is one conversion artifact that hits all three.
    for emit_dir in (MODERN_MUZZLE_DIR, MODERN_EXPLOSION_DIR, MODERN_TRACER_DIR):
        emit_root = common.resource_path(root, emit_dir)
        if not emit_root.is_dir():
            continue
        for path in sorted(emit_root.glob("*.vpcf")):
            text = path.read_text(encoding="utf-8")
            new_text = clamp_emit_count_minimums(text)
            if new_text == text:
                continue
            path.write_text(new_text, encoding="utf-8")
            rel = path.relative_to(root).as_posix()
            if rel not in changed:
                changed.append(rel)

    for res in MODERN_BARREL_TRAIL_FILES:
        path = common.resource_path(root, res)
        if not path.is_file():
            continue
        text = new_text = path.read_text(encoding="utf-8")
        new_text = patch_cs2_modern_rope_trail_alignment(new_text)
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
            changed.append(res)

    for res in MODERN_LIVE_BARREL_SMOKE_FILES:
        path = common.resource_path(root, res)
        if not path.is_file():
            continue
        text = new_text = path.read_text(encoding="utf-8")
        new_text = patch_cs2_modern_barrel_smoke_alignment(new_text)
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
            changed.append(res)

    # Every leaf in the muzzle directory, not just the eight parents: the world-space spawn
    # speeds that threw the flash off the barrel live in the CHILDREN. Runs first so the parent
    # pass below still gets the last word on the parents (it zeroes their speeds outright).
    muzzle_dir = root / MODERN_MUZZLE_DIR
    for path in sorted(muzzle_dir.glob("*.vpcf")) if muzzle_dir.is_dir() else []:
        text = path.read_text(encoding="utf-8")
        fixed = patch_modern_muzzle_child_local_coords(text)
        if fixed != text:
            common.write_if_different(
                root, path.relative_to(root).as_posix(), fixed, changed)

    for res in MODERN_MUZZLEFLASH_FILES:
        path = common.resource_path(root, res)
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        world = patch_cs2_modern_muzzleflash_alignment(text)
        # Strip the reverted CP-config experiment (2026-07-06 night; it froze the
        # engine's own, already-correct control-point driving).
        world = common.remove_muzzle_follow_config(world)
        if world != text:
            path.write_text(world, encoding="utf-8")
            changed.append(res)
        # First-person viewmodel twin, written from the freshly-patched world flash so the
        # two only ever differ by the viewmodel-effect flag (see make_modern_muzzleflash_fp
        # and the _fps routing in kVariantWeaponFx). Must precede the _fp sniper
        # compositions below, which reference the _fp leaves as children.
        common.write_if_different(
            root, _fp_variant_res(res), make_modern_muzzleflash_fp(world), changed)

    for res in MODERN_TRACER_PARENTS:
        path = common.resource_path(root, res)
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        trail_length = MODERN_TRACER_TRAIL_LENGTH.get(path.name, 110.0)
        new_text = patch_modern_tracer_cs2_discipline(text)
        new_text = patch_modern_tracer_mw_streak_texture(new_text)
        new_text = patch_modern_tracer_soft_head(new_text, trail_length)
        new_text = patch_modern_tracer_trail_endpoint_fade(new_text)
        if tune_tracer_brightness:
            new_text = patch_modern_tracer_glow_brightness(new_text)
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
            changed.append(res)

    patch_modern_tracer_class_tiers(root, changed)
    if tune_tracer_brightness:
        patch_modern_tracer_glow_leaves(root, changed)

    # Wisp-scroll animation (idempotent, runs in-place too -- unlike the brightness
    # pass above): explicit V mapping so the beam texture's scroll is visible.
    tracer_dir_path = common.resource_path(root, MODERN_TRACER_DIR)
    if tracer_dir_path.is_dir():
        for path in tracer_dir_path.glob("*.vpcf"):
            if path.name not in MODERN_TRACER_TRAIL_CHILD_SOFT_HEAD:
                continue
            text = path.read_text(encoding="utf-8")
            new_text = patch_modern_tracer_trail_scroll(text)
            if new_text != text:
                path.write_text(new_text, encoding="utf-8")
                changed.append(path.relative_to(root).as_posix())

    # Wire the "smoke follows the gun through the air" rope wisp back in (user
    # report 2026-07-03, see Docs/porting/mw2019-fx-mapping-reference.md §6b). ARC9's OWN
    # barrel_smoke.vpcf is already a thin wrapper whose ONLY content is a child
    # ref to barrel_smoke_trail_b.vpcf (confirmed post-conversion, 2026-07-03 --
    # barrel_smoke.vpcf has no renderers/operators of its own at all, so every
    # class in MODERN_FLASH_SMOKE_CHILDREN that resolves to it already gets the rope wisp
    # natively). barrel_smoke_plume.vpcf (the lmg/dmr/sniper-composition smoke)
    # has NO such child, so its classes never got a wisp at all. Give it the
    # OTHER trail file (barrel_smoke_trail.vpcf, previously unused by anything)
    # instead of duplicating barrel_smoke's own trail_b onto it.
    removed_smoke_children = {
        "barrel_smoke.vpcf",
        "barrel_smoke_fp.vpcf",
        "barrel_smoke_plume.vpcf",
        "barrel_smoke_plume_fp.vpcf",
        "barrel_smoke_trail.vpcf",
        "barrel_smoke_trail_fp.vpcf",
        "barrel_smoke_trail_b.vpcf",
        "barrel_smoke_trail_b_fp.vpcf",
    }
    for res in MODERN_LIVE_BARREL_SMOKE_FILES:
        path = common.resource_path(root, res)
        if path.is_file() and common.remove_child_refs(path, removed_smoke_children):
            changed.append(res)

    for res, children in {**MVM_COMPOSITIONS, **MVM_COMPOSITIONS_FP}.items():
        if not all(common.resource_path(root, c).is_file() for c in children):
            continue
        common.write_if_different(root, res, common._composition_text(children), changed)
    # Strip old smoke children from already-patched trees. Do not generate mvm_spray_*
    # wrappers or first-person smoke twins; the runtime no longer swaps to them.
    for name, smoke in MODERN_FLASH_SMOKE_CHILDREN.items():
        smoke_res = f"{MODERN_MUZZLE_DIR}/{smoke}"
        smoke_fp_res = _fp_variant_res(smoke_res)
        for flash_res in (f"{MODERN_MUZZLE_DIR}/{name}", _fp_variant_res(f"{MODERN_MUZZLE_DIR}/{name}")):
            flash_path = common.resource_path(root, flash_res)
            if not flash_path.is_file():
                continue
            if common.remove_child_refs(flash_path, {smoke_res, smoke_fp_res}):
                changed.append(flash_res)

    patch_mw2019_flashbang_pieces(root, changed)
    patch_modern_shell_port_smoke(root, changed, source1_root)
    # AFTER the clone, so the normalisation cannot be undone by it.
    common.normalize_shell_casings(root, changed)
    # Restitution alone never stopped them: casings also need CS2's stop-speed and
    # stick-on-collision, or they hop forever from an ever-smaller bounce.
    common.stop_casing_bouncing(root, changed)

    # Soft particles: a billboard that intersects a wall otherwise terminates in a hard line
    # across it. Feathering fades it into the surface instead. Runs over BOTH packs (every
    # namespace vpcf), which is why it lives here with the other cross-pack passes.
    common.add_depth_feathering(root, changed, source1_root)

    # Tracers must expire where the bullet stopped: a later flat RandomLifeTime was silently
    # overwriting the traced lifespan, sending trails through walls and across the map.
    common.fix_traced_lifespan_clobber(root, changed)

    # Muzzle smoke carried GMod-scale radii (8-16) against CS2's own 2-3, so the puff
    # spilled back over the weapon and read as "smoke at the back of the gun".
    common.scale_modern_muzzle_smoke(root, changed)


    # Weapon smoke was dense enough to obscure the shot in both views; drop the alpha
    # and fade it sooner rather than speeding the sprite up (which would change the look).
    common.soften_weapon_smoke(root, changed)



    for name in MODERN_HEATWAVE_CHILDREN_TO_REMOVE:
        res = f"{MODERN_MUZZLE_DIR}/{name}"
        path = common.resource_path(root, res)
        if path.is_file() and common.remove_child_refs(path, {"muzzle_heatwave.vpcf"}):
            changed.append(res)

    for res, distort in MODERN_DISTORT_CHILDREN.items():
        if common._add_child_once(common.resource_path(root, res), distort):
            changed.append(res)

    return changed
