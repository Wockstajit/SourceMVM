"""CS2 post-process entry point for the converted FX packs.

Replaces postprocess_povarehok.py (2026-09-01, the Insurgency 2 rebake). It is DELIBERATELY
thin, and that is the point of the rebake:

The old Povarehok pack's ~1,500 lines of post-processing were almost entirely damage control
for a source tree assembled from ~12 donor games with mutually inconsistent VMT conventions --
tone_down passes, vistasmoke retargeting, warm-tint neutralisation, per-weapon flash forks,
$addself restoration, LMG flash realignment. None of that was fixing Source 1 -> Source 2
conversion; it was fixing the mod. Insurgency 2's FX set is one game's internally consistent
output, so what remains is the genuinely shared conversion work in postprocess_common.py --
the seven failure patterns in Docs/porting/povarehok-csgo-mod-reference.md §10 that apply to
ANY Source 1 particle port, not to one donor.

Anything Insurgency-specific belongs in INSURGENCY_TUNING below and should only be added when
an in-game test proves it is needed -- never pre-emptively. If that section starts growing the
way the Povarehok one did, that is the signal the source tree is wrong again, not the fixes.

Imports only from postprocess_common and postprocess_modern; the Modern pack's own
composites are driven from here because they must run over the merged content tree.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

import postprocess_common as common
import postprocess_modern as modern

# A control-point remap with NO control points to remap. Observed in game 2026-09-02: CS2 refused
# ins_grenade_explosion with `Attempting to use invalid particle definition "..."`, so every HE
# grenade swap (On AND Less, since both route there) rendered NOTHING -- the reported "nothing
# happens". The asset, its compiled resource header and all 16 of its children were fine; the only
# structural difference from its working sibling ins_c4_explosion -- the same kind of children-only
# container -- was this one parameterless operator. It is the ONLY file in the 439-file pack that
# carries this class, and it is the only file the engine rejects. A remap with no source or
# destination is a no-op even when accepted, so dropping it costs nothing.
EMPTY_REMAP_OP_RE = re.compile(
    r'\n?[ \t]*\{\s*_class\s*=\s*"C_OP_RemapControlPointToVector"\s*,?\s*\}\s*,?'
)
# An operator list left empty by the above: the working sibling has no m_Operators key at all.
EMPTY_OPERATORS_BLOCK_RE = re.compile(r'\n[ \t]*m_Operators\s*=\s*\n?[ \t]*\[\s*\]\s*')

# Smoke sheets whose .mks sequences get their LOOP flag dropped: a one-shot puff whose
# fixed-rate animation overshoots the frame count should hold the last frame, not replay.
CLAMP_SMOKE_SHEET_RE = re.compile(
    r"(smoke|thinsmoke|thicksmoke|animsmoke|vapor|loopsmoke|dust|puff)", re.IGNORECASE)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--content-root", type=Path, required=True)
    # The staged Source 1 tree (for reading original VMTs). Defaults to the converter's
    # layout: <OutputRoot>/source1_game next to source2/.
    parser.add_argument("--source1-root", type=Path)
    # Run ONLY idempotent patch passes, so an already-converted tree can be patched in
    # place. The full pipeline below is safe to re-run too (every pass it calls is either
    # ceiling-based or marker-guarded), but these keep the fast paths the build scripts use.
    parser.add_argument("--gameplay-composites-only", action="store_true")
    parser.add_argument("--runtime-impact-fixes-only", action="store_true")
    parser.add_argument("--feathering-only", action="store_true")
    parser.add_argument("--texture-leak-only", action="store_true")
    parser.add_argument("--casing-direction-only", action="store_true")
    parser.add_argument("--nade-rope-only", action="store_true")
    # Run apply_insurgency_tuning and nothing else. This file's OWN passes, without the shared
    # Modern/common pipeline the full run also drives -- so a broken pass over there cannot stop
    # an Insurgency asset fix from being applied.
    parser.add_argument("--insurgency-tuning-only", action="store_true")
    return parser.parse_args()


# ---- molotov trail: the glow must not draw through players ---------------------------------
# OBSERVED in game 2026-09-02 (user report): the molotov flight trail's glow was visible
# THROUGH players standing in front of it.
#
# CAUSE: Insurgency authored `molotov_glow` with Source 1's $ignorez, which source1import
# faithfully converts to `m_bDisableZBuffering = 1` -- depth testing literally off, so the
# additive flare draws over everything regardless of what is in front. It is the ONLY system in
# the whole converted pack carrying that flag (checked), so this is a targeted removal, not a
# blanket policy.
#
# The ground fire is deliberately NOT touched here: molotov/incendiary ground fire passes
# through to vanilla CS2 (see kVariantMolotov in ParticleFxRules.cpp) because Insurgency 2 has
# no incendiary-pool effect to donate -- its env_fire_* systems are entity campfires.
DISABLE_Z_LINE_RE = re.compile(r"(?m)^[ \t]*m_bDisableZBuffering = 1[ \t]*\r?\n")
MOLOTOV_TRAIL_ROOTS = [
    "particles/filmmaker/insurgency/weapon_fx_ins/molotov_trail.vpcf",
]

# ---- sparks travel too far ------------------------------------------------------------------
# OBSERVED in game 2026-09-02 (user report): "the sparks from Insurgency, they fly far -- maybe
# decrease it 30 percent, not the size but the distance".
#
# CAUSE: Insurgency's spark systems are authored for its own scale -- impact_metal_sparks
# launches at +/-550 horizontally and up to 1500 units/s upward. CS2's rooms are tighter, so
# the same sparks skate right across them.
#
# FIX: scale ONLY the launch velocity (C_INIT_CreateWithinSphere's local-coordinate speed
# vectors) by 0.7. Travel distance is driven by launch speed, so this is ~30% less distance
# while radius/lifetime/brightness -- the things that read as "size" -- are untouched.
# ---- the native smoke-nade trail needs its OWN smoke ribbon -------------------------------
# CS2 ships no in-flight particle for a smoke/HE/flash/decoy grenade (verified 2026-09-02 with
# Source2Viewer-CLI over pak01). Molotov/incendiary are the exception. The runtime therefore
# calls the same entity-owned grenade particle dispatcher that C_MolotovProjectile uses, but
# supplies this smoke-only resource. It never swaps or reads the spectator utility trail.
#
# The smoke ribbon is forked from Insurgency's molotov_trail_smoke child so its renderer and
# rope behavior stay compatible with the working Molotov trail. The fork gets independent,
# mid-grey/alpha values so brightening the smoke trail cannot change Molotov visuals. Written
# from the source every run, so it stays in sync with any upstream change, and it is idempotent.
#
# OBSERVED in game 2026-09-02 (user screenshots): both the Molotov ribbon and its brighter
# smoke-grenade fork formed overlapping starbursts / stepped triangles along the flight path.
# CAUSE: the converted system connected every emitted particle with C_OP_RenderRopes. That is a
# camera-facing strip, so its joins rotate when the camera orbits the grenade. The combination of
# large random per-node widths and a 5x lifetime expansion turns those joins into the observed
# stepped/starburst triangles. Zeroing the tumbling grenade's local-space launch vector helped,
# but cannot make a connected billboard strip camera-independent. C_OP_RenderCables was tested
# next and rejected: stock CS2 cables require the cables.vfx material path and snapshot/constraint
# particle topology, so this continuously emitted csgo_complex.vfx system compiled but did not
# draw.
#
# FIX: use the pack's proven animated-vapor sprite renderer for a dense trail of independent smoke
# puffs. Individual smoke sprites remain readable from every angle, but no geometry joins adjacent
# particles, so camera motion cannot create mitre spikes or stepping. Both Molotov/incendiary and
# smoke grenades use this same geometry; the smoke-grenade fork changes colour/alpha only. The
# Molotov's separate fire/glow/drip children stay unchanged.
#
# Structure matters: molotov_trail is a pure container parent whose only child is the ribbon.
# The smoke resource keeps that same parent/child shape, with no Molotov drips or glow.
NADE_TRAIL_SRC = "particles/filmmaker/insurgency/weapon_fx_ins/molotov_trail_smoke.vpcf"
NADE_TRAIL_FORK = "particles/filmmaker/insurgency/weapon_fx_ins/mvm_nade_trail_smoke.vpcf"
NADE_TRAIL_WRAPPER = "particles/filmmaker/insurgency/weapon_fx_ins/mvm_nade_trail.vpcf"
# Mirrors molotov_trail.vpcf exactly: a CParticleSystemDefinition that is nothing but a child
# list. Written verbatim rather than cloned, so it can never inherit a molotov-only child.
NADE_TRAIL_WRAPPER_TEXT = (
    '<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} '
    'format:vpcf26:version{26288658-411e-4f14-b698-2e1e5d00dec6} -->\n'
    "{\n"
    "\tm_nBehaviorVersion = 8\n"
    '\t_class = "CParticleSystemDefinition"\n'
    "\tm_Children = \n"
    "\t[\n"
    "\t\t{\n"
    f'\t\t\tm_ChildRef = resource:"{NADE_TRAIL_FORK}"\n'
    "\t\t},\n"
    "\t]\n"
    "\n"
    "\tm_bPreventnamebasedlookup = false\n"
    "}\n"
)
_NADE_FORK_FIELD_SETS = [
    # mid-grey smoke instead of near-black fuel soot
    (re.compile(r"(m_ColorMin = )\[[^\]]*\]"), r"\g<1>[124, 122, 119, 255]"),
    (re.compile(r"(m_ColorMax = )\[[^\]]*\]"), r"\g<1>[98, 97, 94, 255]"),
    # readable against a lit map, still translucent
    (re.compile(r"(m_nAlphaMin = )\d+"), r"\g<1>110"),
    (re.compile(r"(m_nAlphaMax = )\d+"), r"\g<1>150"),
]
_SMOOTH_NADE_TRAIL_FIELD_SETS = [
    (re.compile(r"(m_LocalCoordinateSystemSpeedMin = )\[[^\]]*\]"),
     r"\g<1>[0.0, 0.0, 0.0]"),
    (re.compile(r"(m_LocalCoordinateSystemSpeedMax = )\[[^\]]*\]"),
     r"\g<1>[0.0, 0.0, 0.0]"),
    (re.compile(r"(m_flRadiusMax = )[\d.]+"), r"\g<1>7.0"),
    (re.compile(r"(m_flRadiusMin = )[\d.]+"), r"\g<1>5.0"),
    (re.compile(r"(m_flEmitRate = )[\d.]+"), r"\g<1>60.0"),
    (re.compile(r"(m_flEndScale = )[\d.]+"), r"\g<1>1.8"),
    (re.compile(r"(m_Gravity = )\[[^\]]*\]"), r"\g<1>[0.0, 0.0, 30.0]"),
]
_NADE_CONNECTED_RENDERER_RE = re.compile(
    r'\t\t\{\r?\n\t\t\t_class = "C_OP_Render(?:Ropes|Cables|Sprites)"\r?\n.*?\t\t\},',
    re.DOTALL)
_NADE_SPRITE_RENDERER = (
    '\t\t{\n'
    '\t\t\t_class = "C_OP_RenderSprites"\n'
    '\t\t\tm_nFeatheringMode = "PARTICLE_DEPTH_FEATHERING_ON_OPTIONAL"\n'
    '\t\t\tm_flFeatheringMaxDist = 8.0\n'
    '\t\t\tm_bBlendFramesSeq0 = true\n'
    '\t\t\tm_flSelfIllumAmount = 1.0\n'
    # Keep the known-good staged particle material; the renderer's explicit animated-vapor
    # texture supplies the soft puff image. The converted anim_vapor material itself is not
    # present in the staged runtime closure.
    '\t\t\tm_hMaterial = resource:"materials/particles/ins_smoke_beam.vmat"\n'
    '\t\t\tm_hTexture = resource:"materials/particles/anim_vapor.vtex"\n'
    '\t\t\tm_flStartFadeSize = 0.45\n'
    '\t\t\tm_flEndFadeSize = 1.5\n'
    '\t\t\tm_bPerVertexLighting = 1\n'
    '\t\t\tm_flAnimationRate = 2.0\n'
    '\t\t\tm_flAnimationRate2 = 0.1\n'
    '\t\t},'
)

SPARK_NAME_RE = re.compile(r"(spark|ricochet)", re.IGNORECASE)
SPARK_SPEED_SCALE = 0.7
SPARK_TUNED_MARKER = "// mvm: spark launch velocity scaled for CS2 room scale"

# The spiky explosion sprite (ins_burstspark) and the split-star flash (ins_muzzleflash_split)
# each emit one particle per shot. User request 2026-09-03: play that look 70% more often on
# every gun that already has it. Scale the instantaneous emit count; also attach the same
# children onto the cheap _b fallback so spray fallbacks keep the burst instead of dropping it.
# Trails (ins_burstspark_trail) are a different renderer and are left alone.
BURST_SPRITE_TEX_RE = re.compile(
    r"materials/particles/(?:ins_burstspark|ins_muzzleflash_split)\.vtex"
)
BURST_EMIT_SCALE = 1.7
BURST_EMIT_MARKER = "// mvm: burst/split sprite emit 1.7x"
CHILD_REF_RE = re.compile(r'm_ChildRef = resource:"([^"]+)"')
FALLBACK_RE = re.compile(r'm_hFallback = resource:"([^"]+)"')


def scaled_burst_emit_count(count: int) -> int:
    if count <= 0:
        return count
    bumped = int(round(count * BURST_EMIT_SCALE))
    return max(bumped, count + 1) if bumped <= count else bumped

# ---- Insurgency is the BASE effect; only the layered pack gets dimmed -------------------------
# CORRECTED 2026-09-03 after the first attempt got this exactly backwards. The design the user
# stated is: Insurgency effects are the main/base effect at their authored strength, and the
# Povarehok weapon smoke layered on top by FX/tools/layer-legacy-weapon-smoke.py is what gets
# reduced (to ~20%) so it stays subtle. Dimming Insurgency's own smoke instead removed the base.
#
# This dial therefore stays at 1.0: Insurgency's muzzle smoke is untouched. It is kept (rather
# than deleted) because the marker records the factor each file actually got, so setting it away
# from 1.0 divides the previous factor out first -- which is what restored these files after the
# 0.2 mistake. The Povarehok layer's own dial lives in layer-legacy-weapon-smoke.py.
MUZZLE_SMOKE_ALPHA_SCALE = 1.0
MUZZLE_SMOKE_NAME_RE = re.compile(r"^muzzleflash_.*(?:smoke|dust_kickup)", re.IGNORECASE)
MUZZLE_SMOKE_ALPHA_RE = re.compile(r"(m_nAlpha(?:Min|Max)\s*=\s*)([0-9]+)")
MUZZLE_SMOKE_MARKER_PREFIX = "// mvm: muzzle smoke opacity scaled by "
MUZZLE_SMOKE_MARKER_RE = re.compile(re.escape(MUZZLE_SMOKE_MARKER_PREFIX) + r"([0-9.]+)")

# ---- muzzle smoke popped out of existence when the camera backed away ------------------------
# OBSERVED 2026-09-03 (user): "if you shoot while moving/backing away from the muzzle smoke, the
# smoke can disappear very abruptly once you reach a certain distance", and correctly attributed
# to the depth behaviour added for grenades. Every Insurgency muzzle-smoke sprite is built from
# the SMOKE_RENDERER template above, which stamps Valve's default m_flFeatheringMaxDist = 8.0.
#
# FX/tools/feather-nade-impact-smoke.py already found and documented that 8 units "still reads as
# a hard wall cut" for a cinematic puff, and gave the nade/impact smoke 24..48 scaled by radius.
# The muzzle puffs are 5-35 units, so they get the bottom of that same band. The depth behaviour
# is KEPT (the user asked to keep it) -- only the distance over which it fades is extended.
MUZZLE_SMOKE_FEATHER_DIST = 32.0
MUZZLE_SMOKE_FEATHER_RE = re.compile(r"(m_flFeatheringMaxDist\s*=\s*)([0-9]+(?:\.[0-9]+)?)")

# ---- the weapon muzzle sparks are Insurgency's, exactly as authored ---------------------------
# REVERTED 2026-09-03 at the user's explicit request: "revert the muzzle spark on the weapons
# back to insurgency default that are broken please". This pass is now a pure RESTORE and must
# stay one. Do not re-apply either of the two changes that were tried and pulled:
#
#   * COUNT. Doubling m_nParticlesToEmit was pulled with the rest. (The separate 1.7x burst-sprite
#     bump from boost_burst_split_sprites is a different, earlier, still-wanted change; the emit
#     numbers in the table below already include it, which is why they are 2 and 3 rather than
#     1 and 2. Do not double-count it.)
#   * DIRECTION. The spark_trail family launches along NEGATIVE local X (min -3.5 / max -7.0,
#     min and max also swapped) while every other muzzle leaf launches positive -- cores +550,
#     glow +200, burst spark +35, the shotgun ember +154..378. That reads as sparks travelling
#     back toward the shooter, and turning them around was tried and explicitly reverted. It is
#     Insurgency's authored behaviour and the user wants it kept. LEAVE IT ALONE.
#
# What this pass still earns its place for: the sizes are SET explicitly rather than floored, so
# it returns these leaves to the authored numbers from whatever state the tree is in -- which is
# exactly what a revert needs. ins_burstspark_trail.vtex is 128x256 and sparktrail.vtex is
# 128x64 (four 32x64 frames), so raising the trail lengths here is what makes a spark look
# stretched and pixelated. That is the trap this table exists to hold shut.
MUZZLE_SPARK_MARKER = "// mvm: muzzle sparks restored to Insurgency's authored values"
SPARK_TRAIL_TEX = "materials/particles/ins_burstspark_trail.vtex"
SPARK_BURST_TEX = "materials/particles/ins_burstspark.vtex"
# stem -> (lifetime min/max, trail length min/max, InterpolateRadius end scale, emit count),
# recorded from the tree before any of this session's spark edits. None = the leaf has no such key.
# Authored local-X launch pair per leaf, (min, max). Insurgency launches these sparks
# NEGATIVE -- see the note above; that is deliberate and must not be 'corrected'. Stated
# explicitly because the FAL leaves put the smaller magnitude in max, an order that
# cannot be derived back once a pass has normalised it.
MUZZLE_SPARK_AUTHORED_X = {
    "muzzleflash_spark_trail": (-3.5, -7.0),
    "muzzleflash_spark_trail_3p": (-3.5, -7.0),
    "muzzleflash_spark_trail_b": (-3.5, -7.0),
    "muzzleflash_spark_trail_b_3p": (-3.5, -7.0),
    "muzzleflash_spark_trail_c": (-3.5, -7.0),
    "muzzleflash_m249_1p_spark_trail": (-3.5, -7.0),
    "muzzleflash_m590_1p_sparks_b": (-3.5, -7.0),
    "muzzleflash_m590_3p_sparks": (-3.5, -7.0),
    "muzzleflash_fal_3p_sparks_a": (-3.5, -2.1),
    "muzzleflash_fal_3p_sparks_b": (-3.5, -2.1),
    "muzzleflash_fal_3p_sparks_c": (-3.5, -2.1),
    "muzzleflash_fal_3p_sparks_d": (-3.5, -2.1),
}

MUZZLE_SPARK_AUTHORED = {
    "muzzleflash_spark_trail":          ((0.025, 0.055), (5.0, 10.0), 2.0, 1),
    "muzzleflash_spark_trail_3p":       ((0.025, 0.055), (5.0, 5.0),  2.0, 3),
    "muzzleflash_spark_trail_b":        ((0.025, 0.055), (5.0, 10.0), 2.0, 1),
    "muzzleflash_spark_trail_b_3p":     ((0.025, 0.055), (5.0, 7.0),  2.0, 2),
    "muzzleflash_spark_trail_c":        ((0.025, 0.055), (5.0, 10.0), 2.0, 2),
    "muzzleflash_m249_1p_spark_trail":  ((0.025, 0.055), (5.0, 10.0), 2.0, 2),
    # The M590 pair is Insurgency's own rich authoring and was already correct.
    "muzzleflash_m590_1p_sparks_b":     ((0.075, 0.15),  (10.0, 20.0), 4.0, 6),
    "muzzleflash_m590_3p_sparks":       ((0.075, 0.1),   (10.0, 10.0), 4.0, 6),
    "muzzleflash_fal_3p_sparks_a":      ((None, 0.025),  (0.0, 3.0),  None, 1),
    "muzzleflash_fal_3p_sparks_b":      ((None, 0.025),  (0.0, 3.0),  None, 1),
    "muzzleflash_fal_3p_sparks_c":      ((None, 0.025),  (0.0, 3.0),  None, 1),
    "muzzleflash_fal_3p_sparks_d":      ((None, 0.025),  (0.0, 3.0),  None, 1),
    # Burst sprites (ins_burstspark). Their emit counts already carry the earlier 1.7x pass.
    "muzzleflash_akm_1p_sparks":        ((0.015, 0.025), None, 1.25, 2),
    "muzzleflash_ak74_1p_sparks":       ((0.015, 0.025), None, 1.25, 2),
    "muzzleflash_m16_1p_sparks":        ((0.015, 0.025), None, 1.25, 2),
    "muzzleflash_m249_1p_sparks":       ((0.015, 0.025), None, 1.25, 2),
    "muzzleflash_m9_1p_sparks":         ((0.015, 0.025), None, 1.25, 2),
    "muzzleflash_makarov_1p_sparks":    ((0.015, 0.025), None, 1.25, 2),
    "muzzleflash_mp5_1p_sparks":        ((0.015, 0.025), None, 1.25, 2),
    "muzzleflash_m590_1p_sparks":       ((0.05, 0.075),  None, 1.25, 3),
}
_BLOCK_RE_CACHE: dict[str, re.Pattern] = {}


# The C4 blast's crater smoke emits for FIFTY SECONDS, which is the whole of backlog item B16
# ("C4 explosion effect lingers far longer than vanilla CS2, including after scrubbing").
#
# Measured 2026-09-02 with FX/tools/measure-particle-lifetime.py, comparing authored duration
# across the full child closure of each system:
#     ins_c4_explosion (ours)      57.00 s   <- generic_crater_smoke_big: 50 s emission + 7 s life
#     explosion_c4_500 (vanilla)   30.00 s
# so ours runs 1.90x vanilla, and ONE child accounts for the entire excess: the next longest
# system in our tree tails at 30.10 s, level with vanilla. generic_crater_smoke_big is
# referenced by ins_c4_explosion and nothing else in the pack, so capping it cannot touch
# another effect.
#
# 50 s of crater smoke is right for Insurgency, where a blast site stays smoking for the rest
# of a long round. A CS2 round ENDS on the bomb. Capped to vanilla's own residual-smoke window
# (explosion_c4_ground_residual_particulate emits for 10 s), which lands the tail at 17 s --
# inside vanilla's 20 s main tail, and still a visibly smoking crater.
#
# This is a DURATION cap only: emit rate, size, colour and particle life are untouched, so the
# smoke looks identical, it just stops being produced. Raise the cap here to lengthen it.
C4_CRATER_SMOKE = "particles/filmmaker/insurgency/explosion_fx_ins/generic_crater_smoke_big.vpcf"
C4_CRATER_EMISSION_CAP = 10.0
_EMISSION_DURATION_RE = re.compile(r"(m_flEmissionDuration\s*=\s*)([0-9]+(?:\.[0-9]+)?)")
C4_CRATER_MARKER = "// mvm: crater smoke emission capped for CS2 round length (backlog B16)"
# The Insurgency HE blast constants (HE_BLAST_ROOT, HE_BLAST_SIZE_SCALE, HE_SIZE_FIELDS, the
# scale/length/glow markers, HE_SHARED_CHILDREN and the _he fork suffix) were removed on
# 2026-09-02 together with passes 6/6b/6c. The HE grenade resolves to Povarehok now; see the
# rule table comment in ParticleFxRules.cpp for why, and for what Less means over there.

# ---- bullet impacts are authored for Insurgency's outdoor scale ------------------------------
# OBSERVED in game 2026-09-02 (user, with screenshots): "shooting a concrete floor and the
# particles coming out are dirt" -- and the same on Nuke's breakable glass, which is the concrete
# wall BEHIND the pane (impact_glass itself is never swapped: seen 32 / acted 0).
#
# Every material in the closure is correct: unlit, real alpha falloff, neutral grey (rocks 132,
# loopsmoke 178, thin 84, flecks 99). So this is not a broken asset, it is SCALE. Measured across
# the two concrete closures:
#     vanilla CS2   7 systems, largest sprite radius   7
#     Insurgency   10 systems, largest sprite radius  85  (dust_linger), then 50, 30, 30, 22
# An 85-unit lingering cloud plus six 10-unit rock sprites, on a hit CS2 draws as a 7-unit puff,
# reads as a burst of dirt. The `impact_concrete_thick` in the closure is only the over-budget
# m_hFallback, not a child, so it does not add to a normal hit.
#
# Scaled IN PLACE across the whole impact family, no forking: verified 2026-09-02 that none of the
# 59 impact files is reached from any explosion / weapon / tracer / blood root, so nothing outside
# impacts can shrink as collateral. Size fields only -- lifetimes, velocities, counts untouched.
# The Less variants are regenerated from the scaled On files, so Less stays 0.70x of this.
#
# THE FACTOR IS A DIAL. 0.35 takes the cloud 85 -> 30 and the rocks 10 -> 3.5; matching vanilla's
# 7 literally would erase the effect. The marker records the factor applied, so changing this
# number and re-running divides the old one out first (same mechanism as the HE flash cut had).
IMPACT_SIZE_SCALE = 0.35
IMPACT_SIZE_FIELDS = ("m_flRadiusMin", "m_flRadiusMax", "m_flStartScale", "m_flEndScale",
                      "m_flMinLength", "m_flMaxLength")
# Files whose largest sprite is already at or under this radius are left at 1.0 (see pass 8):
# CS2's biggest concrete sprite is radius 7, so anything under it is chips and flecks, not the
# cloud, and cutting those makes them vanish.
IMPACT_KEEP_BELOW = 7.0
IMPACT_MARKER_PREFIX = "// mvm: impact scaled to CS2 room scale by "
IMPACT_MARKER_RE = re.compile(re.escape(IMPACT_MARKER_PREFIX) + r"([0-9.]+)")

# Insurgency's impact emitters were authored for large outdoor spaces. Shrinking only their
# sprites (pass 8) did not shrink the simulation: one concrete hit still launched 15 root chips
# plus child debris/dust at up to 750 u/s, with bounce values as high as 1.0. In a CS2 room the
# result accumulates into the screen-filling storm shown in the 2026-09-03 tester captures.
#
# Tune the authored On tree itself. Less is regenerated from this tree afterwards, preserving
# its additional 0.70 count/size/lifetime reduction. Sparks keep their existing dedicated tune;
# these constants target solid chips and the pale dust that was crossing whole rooms.
IMPACT_SIM_MARKER = "// mvm: impact counts, launch, bounce, and dust tuned for CS2 rooms"
IMPACT_COUNT_SCALE = 0.55
IMPACT_DEBRIS_SPEED_SCALE = 0.45
IMPACT_DUST_SPEED_SCALE = 0.35
IMPACT_BOUNCE = 0.08
IMPACT_DUST_ALPHA_SCALE = 0.45
IMPACT_DUST_LIFETIME_CAP = 2.0
IMPACT_SETTLE_MARKER = "// mvm: impact collision slide reduced and stop condition added"
IMPACT_SLIDE = 0.05
IMPACT_STOP_SPEED = 40.0
IMPACT_STICK_FIELD = 30
IMPACT_DUST_NAME_RE = re.compile(r"(dust|smoke)", re.IGNORECASE)
_IMPACT_VECTOR_RE = re.compile(
    r"(m_(?:LocalCoordinateSystemSpeed(?:Min|Max)|vecOutput(?:Min|Max))\s*=\s*\[)([^\]]*)(\])")
_IMPACT_COUNT_RE = re.compile(r"(m_nParticlesToEmit\s*=\s*)(\d+)")
_IMPACT_BOUNCE_RE = re.compile(r"(m_flBounceAmount\s*=\s*)(-?[0-9]+(?:\.[0-9]+)?)")
_IMPACT_ALPHA_RE = re.compile(r"(m_nAlpha(?:Min|Max)\s*=\s*)(\d+)")
_IMPACT_LIFE_MAX_RE = re.compile(r"(m_fLifetimeMax\s*=\s*)([0-9]+(?:\.[0-9]+)?)")
_CHILD_REF_CAPTURE_RE = re.compile(
    r"([ \t]*\{\s*m_ChildRef\s*=\s*resource:\"([^\"]+\.vpcf)\"\s*,?\s*\}\s*,?)",
    re.MULTILINE)

MOD2X_MARKER = "// mvm: source was $mod2x"
MATERIAL_REF_RE = re.compile(r'm_hMaterial\s*=\s*resource:"([^"]+)"')
CHILD_ENTRY_TEMPLATE = r'\n[ \t]*\{\s*m_ChildRef\s*=\s*resource:"%s"\s*,?\s*\}\s*,?'
# m_LocalCoordinateSystemSpeedMin/Max = [x, y, z]
_SPEED_VEC_RE = re.compile(
    r"(m_LocalCoordinateSystemSpeed(?:Min|Max) = \[)([^\]]*)(\])")


def _scale_vec(m: "re.Match[str]", factor: float) -> str:
    parts = []
    for piece in m.group(2).split(","):
        raw = piece.strip()
        try:
            parts.append(f"{float(raw) * factor:.1f}")
        except ValueError:
            return m.group(0)  # not a plain numeric vector; leave it alone
    return m.group(1) + ", ".join(parts) + m.group(3)


def tune_impact_simulation(path: Path, text: str) -> str:
    """Return one impact system with restrained indoor counts and movement.

    The marker makes this safe on the in-place generated tree. A full conversion starts from
    the committed Source 1 files, so changing the constants above deliberately requires a fresh
    conversion rather than trying to invert rounded integer counts.
    """
    if path.stem.endswith("_less") or IMPACT_SIM_MARKER in text:
        return text

    is_spark = bool(SPARK_NAME_RE.search(path.stem))
    is_dust = bool(IMPACT_DUST_NAME_RE.search(path.stem))
    tuned = text

    if not is_spark:
        tuned = _IMPACT_COUNT_RE.sub(
            lambda m: m.group(1) + str(max(1, int(round(int(m.group(2)) * IMPACT_COUNT_SCALE)))),
            tuned)
        speed_scale = IMPACT_DUST_SPEED_SCALE if is_dust else IMPACT_DEBRIS_SPEED_SCALE
        tuned = _IMPACT_VECTOR_RE.sub(lambda m: _scale_vec(m, speed_scale), tuned)
        tuned = _IMPACT_BOUNCE_RE.sub(
            lambda m: m.group(1) + ("%.3g" % min(float(m.group(2)), IMPACT_BOUNCE)), tuned)

    if is_dust:
        tuned = _IMPACT_ALPHA_RE.sub(
            lambda m: m.group(1) + str(max(1, int(round(int(m.group(2)) * IMPACT_DUST_ALPHA_SCALE)))),
            tuned)
        tuned = _IMPACT_LIFE_MAX_RE.sub(
            lambda m: m.group(1) + ("%.4g" % min(float(m.group(2)), IMPACT_DUST_LIFETIME_CAP)),
            tuned)

    # Some imported roots (notably impact_plaster) contain the exact same child twice. Source 2
    # instantiates both entries, so remove later duplicates while retaining order and one child.
    seen_children: set[str] = set()

    def _dedupe_child(m: re.Match) -> str:
        child = m.group(2).replace("\\", "/").lower()
        if child in seen_children:
            return ""
        seen_children.add(child)
        return m.group(1)

    tuned = _CHILD_REF_CAPTURE_RE.sub(_dedupe_child, tuned)
    return tuned.rstrip("\n") + "\n" + IMPACT_SIM_MARKER + "\n"


def settle_impact_collisions(path: Path, text: str) -> str:
    """Make solid impact fragments lose lateral motion and actually come to rest."""
    if path.stem.endswith("_less") or SPARK_NAME_RE.search(path.stem) or IMPACT_SETTLE_MARKER in text:
        return text

    cursor = 0
    pieces: list[str] = []
    edited = False
    while True:
        match = common.WORLD_TRACE_CLASS_RE.search(text, cursor)
        if not match:
            pieces.append(text[cursor:])
            break
        start, end = common.block_span(text, match.start())
        pieces.append(text[cursor:start])
        block = text[start:end]
        indent = text[:match.start()].rsplit("\n", 1)[-1]
        fields = (
            ("m_flSlideAmount", "%.3g" % IMPACT_SLIDE),
            ("m_flStopSpeed", "%.1f" % IMPACT_STOP_SPEED),
            ("m_nStickOnCollisionField", str(IMPACT_STICK_FIELD)),
        )
        for field, value in fields:
            pattern = re.compile(r"(" + re.escape(field) + r"\s*=\s*)-?[0-9]+(?:\.[0-9]+)?")
            if pattern.search(block):
                block = pattern.sub(lambda m, v=value: m.group(1) + v, block)
            else:
                at = block.find('"C_OP_WorldTraceConstraint"') + len('"C_OP_WorldTraceConstraint"')
                block = block[:at] + f"\n{indent}{field} = {value}" + block[at:]
            edited = True
        pieces.append(block)
        cursor = end

    if not edited:
        return text
    return "".join(pieces).rstrip("\n") + "\n" + IMPACT_SETTLE_MARKER + "\n"


def smooth_and_fork_nade_rope(root: Path, changed: list[str]) -> None:
    """Replace the shared smoke ribbon with stable puffs, then derive the nade variant."""
    src = common.resource_path(root, NADE_TRAIL_SRC)
    if not src.is_file():
        return

    smoothed = src.read_text(encoding="utf-8")
    for pattern, repl in _SMOOTH_NADE_TRAIL_FIELD_SETS:
        smoothed = pattern.sub(repl, smoothed)
    smoothed, renderer_edits = _NADE_CONNECTED_RENDERER_RE.subn(
        _NADE_SPRITE_RENDERER, smoothed)
    if renderer_edits == 0 and 'C_OP_RenderSprites' not in smoothed:
        raise RuntimeError(f"Could not find the nade smoke renderer in {src}")
    common.write_if_different(root, NADE_TRAIL_SRC, smoothed, changed)

    forked = smoothed
    for pattern, repl in _NADE_FORK_FIELD_SETS:
        forked = pattern.sub(repl, forked)
    common.write_if_different(root, NADE_TRAIL_FORK, forked, changed)
    common.write_if_different(root, NADE_TRAIL_WRAPPER, NADE_TRAIL_WRAPPER_TEXT, changed)


def drop_unrepresentable_mod2x_children(root: Path, changed: list[str]) -> int:
    """Remove particles that exist only to draw a $mod2x sprite CS2 cannot reproduce.

    Source 1's `$mod2x` blend is `2 * src * dst` -- it can darken as well as brighten, and
    csgo_complex.vfx has no equivalent. convert-spritecard-materials.py therefore emits those
    materials as plain alpha blend and stamps them with MOD2X_MARKER, which is what this reads.
    An alpha-blended flare sprite is not a soft glow, it is an OPAQUE WHITE CARD with straight
    sprite edges.

    Measured in game 2026-09-02 on one HE detonation, as the share of the frame washed to
    near-white: 31.8% as authored; 29.6% at 45% size; 73.2% at 11% size (smaller was WORSE);
    93.3% mapped to F_ADDITIVE_BLEND (the material also carries `$overbrightfactor 10`);
    **4.9% with the child dropped**. Nothing about the sprite's SIZE was ever the problem.

    Only systems whose materials are ALL unrepresentable are dropped -- those exist purely to
    draw the bad sprite. A system that uses one such material among several keeps its other
    output and is left alone.

    SECOND CLASS, same remedy: a system with NO RENDERER AND NO CHILDREN.

    Source 1's refract/heat-distortion shockwaves have no csgo_complex equivalent, so
    source1import emits the emitter and its operators but drops the renderer entirely. The result
    still spawns particles; nothing draws them correctly, and what reaches the screen is a large
    default sprite -- the big yellow-white ball at the centre of the HE blast, which survived
    every attempt to shrink the flash sprites because it was never the flash.

    Measured across the pack: 37 of 320 authored systems carry no renderer, but 35 of those are
    legitimate CONTAINERS whose children do the drawing. Exactly 2 are childless, and both are
    shockwaves -- `ins_grenade_explosion_shockwave` (radius 225) and `ins_c4_explosion_shockwave`
    (radius 750). The children test is what separates them, so keep it: dropping every
    rendererless system would delete 35 working effects.
    """
    insurgency = root / "particles" / "filmmaker" / "insurgency"
    if not insurgency.is_dir():
        return 0

    # Which converted materials could not keep their blend mode.
    bad_materials = set()
    materials = root / "materials"
    for vmat in materials.rglob("*.vmat") if materials.is_dir() else []:
        if MOD2X_MARKER in vmat.read_text(encoding="utf-8", errors="replace"):
            bad_materials.add(vmat.relative_to(root).as_posix())

    # Which systems render nothing but those, plus the rendererless-and-childless ones.
    doomed = set()
    for vpcf in sorted(insurgency.rglob("*.vpcf")):
        text = vpcf.read_text(encoding="utf-8", errors="replace")
        refs = set(MATERIAL_REF_RE.findall(text))
        if bad_materials and refs and refs <= bad_materials:
            doomed.add(vpcf.relative_to(root).as_posix())
        elif "C_OP_Render" not in text and "m_ChildRef" not in text:
            doomed.add(vpcf.relative_to(root).as_posix())
    if not doomed:
        return 0

    # Remove the whole `{ m_ChildRef = ... },` entry so the child list stays well formed.
    pattern = re.compile("|".join(CHILD_ENTRY_TEMPLATE % re.escape(d) for d in sorted(doomed)))
    dropped = 0
    for vpcf in sorted(insurgency.rglob("*.vpcf")):
        text = vpcf.read_text(encoding="utf-8", errors="replace")
        pruned = pattern.sub("", text)
        if pruned != text:
            common.write_if_different(
                root, vpcf.relative_to(root).as_posix(), pruned, changed)
            dropped += 1
    return dropped


def boost_burst_split_sprites(root: Path, insurgency: Path, changed: list[str]) -> None:
    """Emit 1.7x as many burst/split SPRITES on muzzle flashes. Marker-guarded.

    Only first-person/world muzzleflash spark and split leaves that draw with
    C_OP_RenderSprites. Trails, flames, explosions, and generated _less copies stay put.
    """
    if not insurgency.is_dir():
        return
    emit_re = re.compile(r"(m_nParticlesToEmit = )([0-9]+)")
    max_re = re.compile(r"(m_nMaxParticles = )([0-9]+)")
    name_re = re.compile(r"muzzleflash_.*(?:sparks|splits)", re.I)
    for folder in sorted(insurgency.glob("weapon_fx*")):
        for path in sorted(folder.glob("*.vpcf")):
            if path.stem.endswith("_less") or not name_re.search(path.stem):
                continue
            text = path.read_text(encoding="utf-8")
            if BURST_EMIT_MARKER in text or not BURST_SPRITE_TEX_RE.search(text):
                continue
            if 'C_OP_RenderSprites' not in text:
                continue
            emits = [int(value) for _, value in emit_re.findall(text)]
            if not emits:
                continue
            out = emit_re.sub(
                lambda m: f"{m.group(1)}{scaled_burst_emit_count(int(m.group(2)))}",
                text,
            )
            new_emit = max(scaled_burst_emit_count(n) for n in emits)
            def _bump_max(match: re.Match) -> str:
                current = int(match.group(2))
                if current <= 0 or current >= new_emit:
                    return match.group(0)
                return f"{match.group(1)}{new_emit}"
            out = max_re.sub(_bump_max, out)
            if out == text:
                continue
            out = out.rstrip("\n") + "\n" + BURST_EMIT_MARKER + "\n"
            common.write_if_different(root, path.relative_to(root).as_posix(), out, changed)


def attach_burst_sprites_to_fallbacks(root: Path, insurgency: Path, changed: list[str]) -> None:
    """Keep the burst/split sprite on the cheap _b fallback so sprays still show it."""
    if not insurgency.is_dir():
        return
    root_name_re = re.compile(r"^muzzleflash_.+_(1p|3rd)$")
    for folder in sorted(insurgency.glob("weapon_fx*")):
        for path in sorted(folder.glob("*.vpcf")):
            if not root_name_re.match(path.stem):
                continue
            text = path.read_text(encoding="utf-8")
            fallback = FALLBACK_RE.search(text)
            if not fallback:
                continue
            burst_children = []
            candidates = list(CHILD_REF_RE.findall(text))
            for child in list(candidates):
                child_path = common.resource_path(root, child.replace("\\", "/"))
                if child_path.is_file():
                    candidates.extend(CHILD_REF_RE.findall(child_path.read_text(encoding="utf-8")))
            seen: set[str] = set()
            for child in candidates:
                resource = child.replace("\\", "/")
                if resource in seen:
                    continue
                seen.add(resource)
                child_path = common.resource_path(root, resource)
                if not child_path.is_file():
                    continue
                child_text = child_path.read_text(encoding="utf-8")
                if "C_OP_RenderSprites" in child_text and BURST_SPRITE_TEX_RE.search(child_text):
                    burst_children.append(resource)
            if not burst_children:
                continue
            fallback_path = common.resource_path(root, fallback.group(1).replace("\\", "/"))
            for child in burst_children:
                if common._add_child_once(fallback_path, child):
                    rel = fallback_path.relative_to(root).as_posix()
                    if rel not in changed:
                        changed.append(rel)


def _block_re(cls: str) -> re.Pattern:
    """Match one `{ _class = "<cls>" ... }` operator block. KV3 blocks here do not nest."""
    if cls not in _BLOCK_RE_CACHE:
        _BLOCK_RE_CACHE[cls] = re.compile(
            r'(\{\s*\n(\s*)_class = "' + re.escape(cls) + r'"\n)([^{}]*?)(\s*\})')
    return _BLOCK_RE_CACHE[cls]


def _set_fields_in_block(text: str, cls: str, fields: dict) -> str:
    """SET numeric fields inside every block of `cls`, inserting any that are absent.

    A value of None means the authored file has NO such key, so any present one is REMOVED.
    Skipping instead of removing is not good enough for a restore: it leaves behind whatever an
    earlier pass injected. That is exactly how the FAL third-person spark leaves ended up with
    an injected m_fLifetimeMin = 0.075 above their authored m_fLifetimeMax = 0.025 -- an
    inverted range on a key Insurgency never wrote.
    """
    def _rewrite(match: re.Match) -> str:
        head, indent, body, tail = match.groups()
        for key, value in fields.items():
            if value is None:
                body = re.sub(r"[ \t]*" + re.escape(key) + r"\s*=\s*[0-9.]+[ \t]*\n", "", body)
                continue
            literal = str(int(value)) if key.startswith("m_n") else ("%g" % value)
            field_re = re.compile(r"(" + re.escape(key) + r"\s*=\s*)([0-9]+(?:\.[0-9]+)?)")
            found = field_re.search(body)
            if found:
                body = body[:found.start()] + found.group(1) + literal + body[found.end():]
            else:
                body = indent + key + " = " + literal + "\n" + body
        return head + body + tail

    return _block_re(cls).sub(_rewrite, text)


def _restore_launch_x(text: str, launch: tuple) -> str:
    """Write the authored local-X launch back, leaving the Y/Z fan untouched."""
    def _fix(match: re.Match) -> str:
        x = launch[0] if match.group(1) == "Min" else launch[1]
        return ("m_LocalCoordinateSystemSpeed%s = [%s, %s, %s]"
                % (match.group(1), ("%g" % x), match.group(3), match.group(4)))

    return re.sub(
        r"m_LocalCoordinateSystemSpeed(Min|Max) = \[(-?[0-9.]+), (-?[0-9.]+), (-?[0-9.]+)\]",
        _fix, text)


def restore_muzzle_sparks(root: Path, insurgency: Path, changed: list[str]) -> None:
    """Put every weapon muzzle spark leaf back to Insurgency's authored values.

    Size, lifetime, emit count and launch direction are all left exactly as the game shipped
    them -- see MUZZLE_SPARK_AUTHORED for why this is a restore and not a tune.
    """
    if not insurgency.is_dir():
        return
    for folder in sorted(insurgency.glob("weapon_fx*")):
        for path in sorted(folder.glob("*.vpcf")):
            authored = MUZZLE_SPARK_AUTHORED.get(path.stem)
            if authored is None or path.stem.endswith("_less"):
                continue
            text = path.read_text(encoding="utf-8")
            if SPARK_TRAIL_TEX not in text and SPARK_BURST_TEX not in text:
                continue
            life, trail, end_scale, emit = authored
            out = _set_fields_in_block(text, "C_INIT_RandomLifeTime", {
                "m_fLifetimeMin": life[0], "m_fLifetimeMax": life[1]})
            if trail is not None:
                out = _set_fields_in_block(out, "C_INIT_RandomTrailLength", {
                    "m_flMinLength": trail[0], "m_flMaxLength": trail[1]})
            if end_scale is not None:
                out = _set_fields_in_block(out, "C_OP_InterpolateRadius",
                                           {"m_flEndScale": end_scale})
            out = _set_fields_in_block(out, "C_OP_InstantaneousEmitter",
                                       {"m_nParticlesToEmit": emit})
            launch = MUZZLE_SPARK_AUTHORED_X.get(path.stem)
            if launch is not None:
                out = _restore_launch_x(out, launch)
            # The authored per-instance ceilings are all >= their emit counts, so nothing to raise.
            out = out.replace("\n" + MUZZLE_SPARK_MARKER + "\n", "\n")
            for stale in ("// mvm: muzzle sparks at authored size, 2x count, launching forward",):
                out = out.replace("\n" + stale + "\n", "\n")
            out = out.rstrip("\n") + "\n" + MUZZLE_SPARK_MARKER + "\n"
            common.write_if_different(root, path.relative_to(root).as_posix(), out, changed)


def extend_muzzle_smoke_feathering(root: Path, insurgency: Path, changed: list[str]) -> None:
    """Widen the depth fade on muzzle smoke so it stops popping out (see the constant)."""
    if not insurgency.is_dir():
        return
    for folder in sorted(insurgency.glob("weapon_fx*")):
        for path in sorted(folder.glob("*.vpcf")):
            if path.stem.endswith("_less") or not MUZZLE_SMOKE_NAME_RE.match(path.stem):
                continue
            text = path.read_text(encoding="utf-8")
            out = MUZZLE_SMOKE_FEATHER_RE.sub(
                lambda m: m.group(1) + ("%g" % MUZZLE_SMOKE_FEATHER_DIST), text)
            common.write_if_different(root, path.relative_to(root).as_posix(), out, changed)


def thin_muzzle_smoke(root: Path, insurgency: Path, changed: list[str]) -> None:
    """Opacity dial for Insurgency's OWN muzzle smoke (see MUZZLE_SMOKE_ALPHA_SCALE).

    At 1.0 this restores/keeps the authored base. The Povarehok LAYER is dimmed separately.
    """
    if not insurgency.is_dir():
        return
    want = MUZZLE_SMOKE_ALPHA_SCALE
    for folder in sorted(insurgency.glob("weapon_fx*")):
        for path in sorted(folder.glob("*.vpcf")):
            if path.stem.endswith("_less") or not MUZZLE_SMOKE_NAME_RE.match(path.stem):
                continue
            text = path.read_text(encoding="utf-8")
            prev = MUZZLE_SMOKE_MARKER_RE.search(text)
            applied = float(prev.group(1)) if prev else 1.0
            if abs(applied - want) < 1e-9:
                continue
            factor = want / applied
            out = MUZZLE_SMOKE_MARKER_RE.sub("", text).rstrip("\n") + "\n"
            out = MUZZLE_SMOKE_ALPHA_RE.sub(
                lambda m: m.group(1) + str(max(1, min(255, round(int(m.group(2)) * factor)))), out)
            if out.rstrip("\n") == text.rstrip("\n"):
                continue
            if want != 1.0:
                out = out.rstrip("\n") + "\n" + MUZZLE_SMOKE_MARKER_PREFIX + repr(want) + "\n"
            common.write_if_different(root, path.relative_to(root).as_posix(), out, changed)


def apply_insurgency_tuning(root: Path) -> list[str]:
    """Insurgency-pack-specific fixes.

    Keep this SMALL -- see the module docstring. Add a pass only after an in-game test shows
    the shared conversion passes leave a specific Insurgency system wrong, and say in a
    comment what was observed. Returns the content-relative resources it changed.
    """
    changed: list[str] = []

    # 1. the molotov trail's glow must respect depth
    roots = [r for r in MOLOTOV_TRAIL_ROOTS if common.resource_path(root, r).is_file()]
    for path in common.collect_closure(root, roots):
        text = path.read_text(encoding="utf-8")
        stripped = DISABLE_Z_LINE_RE.sub("", text)
        if stripped != text:
            common.write_if_different(root, path.relative_to(root).as_posix(), stripped, changed)

    # 2. Remove the tumbling grenade's rotating local launch vector from the shared smoke rope,
    # then fork it so smoke-grenade colour/alpha tuning never touches the Molotov's appearance.
    smooth_and_fork_nade_rope(root, changed)

    # 3. sparks travel ~30% less far (launch velocity only, never radius)
    insurgency = root / "particles" / "filmmaker" / "insurgency"
    boost_burst_split_sprites(root, insurgency, changed)
    attach_burst_sprites_to_fallbacks(root, insurgency, changed)
    for path in sorted(insurgency.rglob("*.vpcf")) if insurgency.is_dir() else []:
        if not SPARK_NAME_RE.search(path.stem):
            continue
        text = path.read_text(encoding="utf-8")
        if SPARK_TUNED_MARKER in text or "m_LocalCoordinateSystemSpeed" not in text:
            continue  # marker-guarded: the scale is multiplicative, never apply twice
        scaled = _SPEED_VEC_RE.sub(lambda m: _scale_vec(m, SPARK_SPEED_SCALE), text)
        if scaled == text:
            continue
        scaled = scaled.rstrip("\n") + "\n" + SPARK_TUNED_MARKER + "\n"
        common.write_if_different(root, path.relative_to(root).as_posix(), scaled, changed)

    # 4. Drop parameterless C_OP_RemapControlPointToVector operators (see EMPTY_REMAP_OP_RE):
    # CS2 rejects the whole definition, so the effect renders nothing at all.
    for path in sorted(insurgency.rglob("*.vpcf")) if insurgency.is_dir() else []:
        text = path.read_text(encoding="utf-8")
        if "C_OP_RemapControlPointToVector" not in text:
            continue
        stripped = EMPTY_REMAP_OP_RE.sub("", text)
        stripped = EMPTY_OPERATORS_BLOCK_RE.sub(chr(10), stripped)
        common.write_if_different(root, path.relative_to(root).as_posix(), stripped, changed)

    # 5. Cap the C4 crater smoke's emission window (see C4_CRATER_SMOKE for the measurement).
    crater = common.resource_path(root, C4_CRATER_SMOKE)
    if crater.is_file():
        text = crater.read_text(encoding="utf-8")
        if C4_CRATER_MARKER not in text:
            def _cap(m: re.Match) -> str:
                return m.group(1) + ("%.1f" % min(float(m.group(2)), C4_CRATER_EMISSION_CAP))
            capped = _EMISSION_DURATION_RE.sub(_cap, text)
            if capped != text:
                capped = capped.rstrip("\n") + "\n" + C4_CRATER_MARKER + "\n"
                common.write_if_different(
                    root, crater.relative_to(root).as_posix(), capped, changed)

    # (Passes 6/6b/6c -- the HE blast size scale, its trail-length follow-up and the flash
    # cut -- are GONE. The HE grenade was pointed back at Povarehok's ac_grenade_explosion on
    # 2026-09-02 at the user's request, so nothing resolves to ins_grenade_explosion any more
    # and those passes had no subject. Pass 7 below is deliberately kept: it is pack-wide and
    # still serves the C4, the M203 and the M590 muzzle flash.)

    # 7. Drop particles that can only ever draw as a white card (see the function's docstring).
    drop_unrepresentable_mod2x_children(root, changed)

    # 8. Shrink every bullet impact into CS2 room scale (see IMPACT_SIZE_SCALE for the numbers).
    # Re-tunable: the marker carries the factor, so a changed constant divides the old one out.
    impacts = insurgency / "impact_fx_ins"
    for path in sorted(impacts.glob("*.vpcf")) if impacts.is_dir() else []:
        if path.stem.endswith("_less"):
            continue  # regenerated from the scaled On file by build-less-variants.py
        text = path.read_text(encoding="utf-8")
        prev = IMPACT_MARKER_RE.search(text)
        applied = float(prev.group(1)) if prev else 1.0
        # Only the OVERSIZED systems get the cut. A file whose largest sprite is already within
        # vanilla's range (CS2's biggest concrete sprite is radius 7) is the chips and flecks --
        # never the problem -- and a uniform 0.35 would shrink a 2-unit fleck to 0.7 and make it
        # vanish. Judged on the UNSCALED radius so a re-run reaches the same decision.
        radii = [float(v) / applied for v in re.findall(r"m_flRadiusMax\s*=\s*([0-9.]+)", text)]
        want = IMPACT_SIZE_SCALE if (radii and max(radii) > IMPACT_KEEP_BELOW) else 1.0
        if abs(applied - want) < 1e-9:
            continue
        factor = want / applied
        scaled = IMPACT_MARKER_RE.sub("", text).rstrip("\n") + "\n"
        for field in IMPACT_SIZE_FIELDS:
            scaled = re.sub(
                r"(" + re.escape(field) + r"\s*=\s*)([0-9]+(?:\.[0-9]+)?)",
                lambda m, f=factor: m.group(1) + ("%.4g" % (float(m.group(2)) * f)),
                scaled)
        if scaled.rstrip("\n") == text.rstrip("\n"):
            continue  # nothing sizeable in this file (a pure container)
        # Stamp the factor THIS file got (1.0 for the small ones), not the global constant, or the
        # next run would divide a never-scaled file by 0.35 and blow it up.
        scaled = scaled.rstrip("\n") + "\n" + IMPACT_MARKER_PREFIX + repr(want) + "\n"
        common.write_if_different(
            root, path.relative_to(root).as_posix(), scaled, changed)

    # 9. Reduce the authored outdoor-scale impact simulation (count, travel, bounce, dust).
    # This deliberately runs after size scaling and before Less generation.
    for path in sorted(impacts.glob("*.vpcf")) if impacts.is_dir() else []:
        if path.stem.endswith("_less"):
            continue
        text = path.read_text(encoding="utf-8")
        tuned = tune_impact_simulation(path, text)
        common.write_if_different(root, path.relative_to(root).as_posix(), tuned, changed)

    # 10. Low bounce alone still permits long lateral slides. Add the same explicit stop/stick
    # condition Valve uses for shell casings, with a much lower slide transfer for tiny chips.
    for path in sorted(impacts.glob("*.vpcf")) if impacts.is_dir() else []:
        if path.stem.endswith("_less"):
            continue
        text = path.read_text(encoding="utf-8")
        settled = settle_impact_collisions(path, text)
        common.write_if_different(root, path.relative_to(root).as_posix(), settled, changed)

    # 11. Muzzle sparks: restored to Insurgency's authored values (user revert, 2026-09-03).
    restore_muzzle_sparks(root, insurgency, changed)

    # 12. Keep the depth fade on muzzle smoke but widen it, so backing away fades the smoke
    # out instead of popping it.
    extend_muzzle_smoke_feathering(root, insurgency, changed)

    # 13. Opacity dial for the Insurgency BASE smoke. 1.0 = authored; the Povarehok layer is
    # dimmed by layer-legacy-weapon-smoke.py instead.
    thin_muzzle_smoke(root, insurgency, changed)

    return changed


def main() -> int:
    args = parse_args()
    root = args.content_root.resolve()
    source1_root = (args.source1_root or root.parents[2] / "source1_game").resolve()

    if args.insurgency_tuning_only:
        tuned = apply_insurgency_tuning(root)
        print(f"Insurgency tuning: {len(tuned)} file(s) written/changed:")
        for res in tuned:
            print(f"  {res}")
        return 0

    if args.nade_rope_only:
        changed: list[str] = []
        smooth_and_fork_nade_rope(root, changed)
        print(f"Nade rope: {len(changed)} file(s) written/changed:")
        for res in changed:
            print(f"  {res}")
        return 0

    if args.gameplay_composites_only:
        wrong_add = common.repair_wrongly_additive_renderers(root, source1_root)
        premul = common.premultiply_white_additive_textures(root)
        noleak = common.zero_additive_transparent_rgb(root)
        blend = common.add_sheet_frame_blending(root)
        rope_changed: list[str] = []
        ropes = common.ensure_rope_ribbon_mapping(root, rope_changed)
        # tune_tracer_brightness=False: that pass MULTIPLIES alpha/radius, so re-running it
        # in place dims tracers again. It belongs to the one-shot fresh-conversion path.
        composites = modern.apply_modern_gameplay_composites(
            root, tune_tracer_brightness=False, source1_root=source1_root)
        composites += apply_insurgency_tuning(root)
        clamped = common.clamp_sheet_sequences(root, CLAMP_SMOKE_SHEET_RE)
        print(
            f"Repairs: {wrong_add} wrongly-additive files fixed, "
            f"{len(premul)} textures premultiplied, {len(noleak)} de-leaked, "
            f"{blend} files frame-blended, {ropes} rope renderers given ribbon mapping, "
            f"{len(clamped)} smoke sheets clamped (loop -> play once)."
        )
        for res in premul + clamped:
            print(f"  recompile texture: {res}")
        print(f"Gameplay composites: {len(composites)} file(s) written/changed:")
        for res in composites:
            print(f"  {res}")
        return 0

    if args.casing_direction_only:
        casing_changed: list[str] = []
        common.fix_casing_eject_direction(root, casing_changed)
        print(f"Casing eject: {len(casing_changed)} file(s) flipped back to +X.")
        for res in casing_changed:
            print(f"  {res}")
        return 0

    if args.texture_leak_only:
        leaked = common.zero_additive_transparent_rgb(root)
        print(f"Texture leak: {len(leaked)} additive texture(s) had transparent RGB zeroed.")
        for res in leaked:
            print(f"  recompile texture: {res}")
        return 0

    if args.feathering_only:
        feather_changed: list[str] = []
        common.add_depth_feathering(root, feather_changed, source1_root)
        print(f"Depth feathering: {len(feather_changed)} file(s) reconciled against "
              f"Source 1 $depthblend.")
        return 0

    if args.runtime_impact_fixes_only:
        collision = common.force_per_particle_collision(root)
        clamped = common.clamp_sheet_sequences(root, CLAMP_SMOKE_SHEET_RE)
        print(
            f"Runtime impact fixes: {collision} files forced to per-particle collision, "
            f"{len(clamped)} smoke sheets clamped (loop -> play once)."
        )
        for res in clamped:
            print(f"  recompile texture: {res}")
        return 0

    # ---- the full fresh-conversion pipeline ----
    # Every pass below is one of the seven generic Source 1 -> CS2 particle failure patterns
    # (povarehok-csgo-mod-reference.md §10). Order matters where noted.
    dead_children = common.strip_dead_child_refs(root)
    repaired, removed = common.fix_textureless_renderers(root, source1_root)
    dual_seq = common.strip_dual_sequence_keys(root)
    collision = common.force_per_particle_collision(root)
    unlit, additive = common.fix_lit_renderers(root, source1_root)
    wrong_add = common.repair_wrongly_additive_renderers(root, source1_root)
    # AFTER fix_lit_renderers so the final ADD/translucent state is known.
    premul = common.premultiply_white_additive_textures(root)
    noleak = common.zero_additive_transparent_rgb(root)
    blend = common.add_sheet_frame_blending(root)
    clamped = common.clamp_sheet_sequences(root, CLAMP_SMOKE_SHEET_RE)
    # Rope ribbons: source1import emits no CS2 V-mapping/tessellation, so a converted rope
    # draws as disconnected per-particle quads until these are supplied.
    rope_changed: list[str] = []
    ropes = common.ensure_rope_ribbon_mapping(root, rope_changed)
    # AFTER fix_textureless_renderers: it rewrites systems whose Source 1 refract renderers
    # that pass just emptied. Also runs the cross-pack casing/feathering normalisation.
    composites = modern.apply_modern_gameplay_composites(root, source1_root=source1_root)
    composites += apply_insurgency_tuning(root)
    print(
        f"Post-processed FX packs: {dead_children} dead child refs stripped, "
        f"{repaired} textureless renderers repaired / {removed} removed, "
        f"{dual_seq} files de-dual-sequenced, "
        f"{collision} files forced to per-particle collision, "
        f"{unlit} renderers made unlit / {additive} made additive "
        f"({wrong_add} wrongly-additive fixed), "
        f"{len(premul)} additive textures premultiplied, {len(noleak)} de-leaked, "
        f"{blend} files frame-blended, {len(clamped)} smoke sheets clamped, "
        f"{ropes} rope renderers given CS2 ribbon mapping, "
        f"{len(composites)} gameplay-composite files."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
