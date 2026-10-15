"""CS2-specific cleanup for converted Povarehok VPCFs. Also the pipeline's CLI
entry point: drives the shared repairs (postprocess_common) and Modern's own
gameplay composites (postprocess_modern) alongside Povarehok's own.

This does not create placeholder effects. It adjusts the converted Source 1
assets where Source 2 control-point / viewmodel behavior makes the original
values unusable in CS2.
"""

from __future__ import annotations

import argparse
import math
import re
from pathlib import Path

import postprocess_common as common
import postprocess_modern as modern


VARIANTS = ("regular", "less/impacts", "less/smoke")


def _fp_variant_res(res: str) -> str:
    assert res.endswith(".vpcf"), res
    return res[:-len(".vpcf")] + "_fp.vpcf"


def iter_povarehok_impact_vpcfs(root: Path):
    pvrh_root = root.joinpath("particles", "filmmaker", "povarehok")
    impact_dirs = {"impact_fx", "impact_fxmoney", "impact_fxsnow", "impact_fx_smoke"}
    for path in pvrh_root.rglob("*.vpcf"):
        if any(part.lower() in impact_dirs for part in path.parts):
            yield path


MUZZLE_ROOTS = (
    "weapon_muzzle_flash_assaultrifle_fp.vpcf",
    "weapon_muzzle_flash_shotgun_fp.vpcf",
    "weapon_muzzle_flash_huntingrifle_fp.vpcf",
    "weapon_muzzle_flash_smg_fp.vpcf",
    "weapon_muzzle_flash_smg_silenced_fp.vpcf",
    "weapon_muzzle_flash_pistol_fp.vpcf",
)
MOLOTOV_GROUNDFIRE_ROOTS = (
    "molotov_groundfire_00high.vpcf",
    "molotov_fire01.vpcf",
)
MOLOTOV_EXPLOSION_ROOTS = (
    "molotov_explosion.vpcf",
)

# These children are cinematic/screen-space Source 1 pieces that do not match
# CS2's sustained inferno control points. They produce the giant rectangle and
# particles visibly attracting back to the molotov control point.
MOLOTOV_REMOVE_CHILDREN = {
    "molotov_smoke_screen.vpcf",
    "extinguish_fire.vpcf",
    "molotov_groundfire_main_center.vpcf",
    "molotov_groundfire_main_fancy.vpcf",
    "ac_rpg_explosion_air_smoke_a_copy.vpcf",
    "ac_grenade_explosion_smoketrail_a.vpcf",
    "realistic_campfire_glow.vpcf",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--content-root", type=Path, required=True)
    # The staged Source 1 tree (for reading original VMTs). Defaults to the
    # converter's layout: <OutputRoot>/source1_game next to source2/.
    parser.add_argument("--source1-root", type=Path)
    # Run ONLY idempotent patch passes. The full pipeline is NOT rerunnable on
    # an already-processed tree (tone_down would compound), so these flags let
    # an existing converted tree be patched in place.
    parser.add_argument("--gameplay-composites-only", action="store_true")
    parser.add_argument("--runtime-impact-fixes-only", action="store_true")
    parser.add_argument("--muzzle-smoke-only", action="store_true")
    parser.add_argument("--lmg-flash-only", action="store_true")
    parser.add_argument("--flash-addself-only", action="store_true")
    parser.add_argument("--feathering-only", action="store_true")
    parser.add_argument("--texture-leak-only", action="store_true")
    parser.add_argument("--casing-direction-only", action="store_true")
    parser.add_argument("--per-weapon-forks-only", action="store_true")
    return parser.parse_args()


SMOKE_CHILD_HINT_RE = re.compile(r"(smoke|wisp|puff|plume)", re.IGNORECASE)


def tone_down_muzzle_closure(root: Path, muzzle_roots: list[str], *, alpha: float,
                              radius: float, overbright: float) -> int:
    """Tone down the (overbright) muzzle-flash roots' child-ref closure, but skip smoke
    children instead of blindly applying the flash's own harsh factors to them.

    Bug (2026-07-03, "On-mode barrel smoke is too dark"): collect_closure() walks the
    WHOLE child-ref graph from each flash root, which includes that flash's own per-shot
    smoke wisp child (weapon_muzzle_flash_smoke_small2 and friends -- see the
    patch_cs2_muzzle_smoke_alignment docstring). Applying alpha=0.45/overbright=0.35
    (tuned to tame the blown-out FLASH sprite) to that smoke child crushed it toward
    black. Smoke's own CS2 darkening (erroneous scene-lit shading) is already corrected
    separately by fix_lit_renderers' unlit/self-illum pass; it does not need -- and must
    not get -- the flash's darkening on top.
    """
    changed = 0
    for path in common.collect_closure(root, muzzle_roots):
        if SMOKE_CHILD_HINT_RE.search(path.name):
            continue
        if tone_down(path, alpha=alpha, radius=radius, overbright=overbright):
            changed += 1
    return changed


def tone_down(path: Path, *, alpha: float, radius: float, overbright: float) -> bool:
    text = path.read_text(encoding="utf-8")
    out: list[str] = []
    changed = False
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        new_line = line
        if stripped.startswith("m_flOverbrightFactor"):
            new_line = common.scale_number(line, overbright, cap=1.15)
        # m_flAddSelfAmount is deliberately NOT scaled here. See restore_muzzle_addself below:
        # $addself is a BLEND control, not a brightness dial, and scaling it down is what made
        # the muzzle flash render a dark box. Overbright and alpha below still tone the flash.
        elif stripped.startswith("m_flAlphaScale"):
            new_line = common.scale_number(line, alpha, cap=0.65)
        elif stripped.startswith("m_nAlphaMin") or stripped.startswith("m_nAlphaMax"):
            new_line = common.scale_number(line, alpha, integer=True)
        elif (
            stripped.startswith("m_flRadiusMin")
            or stripped.startswith("m_flRadiusMax")
            or stripped.startswith("m_flConstantRadius")
        ):
            new_line = common.scale_number(line, radius)
        if new_line != line:
            changed = True
        out.append(new_line)
    if changed:
        path.write_text("".join(out), encoding="utf-8")
    return changed


# ---- weapon muzzle smoke tuning ------------------------------------------------------------
#
# History, because this was tuned twice and the second pass overshot:
#
# 1. tone_down_muzzle_closure deliberately SKIPS any child matching (smoke|wisp|puff|plume) so
#    the flash's alpha=0.45/overbright=0.35 never crushes smoke toward black (the 2026-07-03
#    "On-mode barrel smoke is too dark" fix). Correct, but TOTAL: smoke got no ceiling at all.
# 2. That left the ac_muzzle_* clouds at radius 8-10 for up to 2.5s. At rifle cadence a spray
#    stacks ~20 of them before the first expires -- the AK-47 white-out (2026-09-01).
# 3. The first fix applied hard ceilings (radius 5.0 / life 1.0s / growth 1.75x / overbright 1.0)
#    on top of soften_weapon_smoke's earlier alpha x0.6. Compounded, that took rifle smoke to
#    alpha 10-19 of 255 and the user reported the opposite problem: "the fade is way too fast and
#    they're barely visible", "the shotgun needs to be better -- if you changed it, undo it".
#
# So this is now an explicit per-family TARGET table rather than a blanket ceiling. Ceilings
# cannot express "shotgun keeps its big slow cloud but the AK does not", and they only ever push
# values DOWN, so they could not walk the over-correction back.
#
# Targets are deliberately BETWEEN the over-corrected state and the Source 1 defaults -- the user
# asked to "increase them up a lil but not back to default" -- except shotgun, which is restored
# because the user asked for that change to be undone.
#
# NOTE ON RESTORATION: build/ is gitignored and the Povarehok Source 1 input is not in
# FX/sources/, so the pre-tuning values could not be diffed from a pristine tree. The shotgun
# numbers below are a reconstruction from measurements taken before the caps were applied
# (radius 8-10, lifetime 1.0-2.5s, growth 3.0x), not a byte-exact restore. If shotgun smoke still
# reads wrong, restoring the Source 1 assets and re-running convert-povarehok-source1.ps1 is the
# only way to recover the true originals.
#
# Every value is an ABSOLUTE target, applied by scaling a block's min/max pair so the MAX lands
# on the target. That preserves the authored min/max variance, moves values UP as well as down,
# and is idempotent (once max == target the factor is 1.0).

# radius / lifetime / growth / overbright / alpha, matched against the file stem in order.
MUZZLE_SMOKE_TUNING = (
    # Shotgun: user asked for this one to be put back. Reconstructed Source 1 values.
    (re.compile(r"^ac_muzzle_shotgun.*smoke", re.I),
     dict(radius=10.0, lifetime=2.5, growth=3.0, overbright=2.2, alpha=65)),
    (re.compile(r"^weapon_muzzle_flash_smoke_medium$", re.I),   # shotgun + AWP flash puff
     dict(radius=10.0, growth=3.0, alpha=150)),
    # Auto snipers / AWP: the shotgun's cloud with the shotgun's density, but a 0.7s dwell.
    # Forked by fork_sniper_muzzle_smoke so this cannot shorten the shotgun's own smoke.
    (re.compile(r"^ac_muzzle_sniper.*smoke", re.I),
     dict(radius=10.0, lifetime=0.7, growth=3.0, overbright=2.2, alpha=65)),
    # Rifles (ac_muzzle_357_* is the AK/AUG/M4 cloud). Lifetime went 2.5 (default) -> 1.0
    # (over-corrected, "barely visible") -> 1.9 -> 0.7 on request. The 1.0 that read as "way too
    # fast" was never really a lifetime problem: alpha had been crushed to 10-19 of 255 at the
    # same time. With alpha and size restored the cloud is visible again, and at 1.9s it lingered,
    # so the dwell comes back down while the density stays.
    (re.compile(r"^ac_muzzle_357.*smoke", re.I),
     dict(radius=6.5, lifetime=0.7, growth=2.4, overbright=1.7, alpha=32)),
    # SMG/UMP: "it's there, but the fade is a little bit too fast" -- a smaller nudge than rifles.
    (re.compile(r"^ac_muzzle_smg.*smoke", re.I),
     dict(radius=6.0, lifetime=1.7, growth=2.3, overbright=1.6, alpha=45)),
    (re.compile(r"^ac_muzzle_pistol.*smoke", re.I),
     dict(radius=6.0, lifetime=1.7, growth=2.3, overbright=1.6, alpha=50)),
    # The brief flash puffs (0.1-0.2s). Lifetime intentionally left alone -- these are meant to
    # be instantaneous; only their size/brightness was over-trimmed.
    (re.compile(r"^weapon_muzzle_flash_smoke_", re.I),
     dict(radius=6.0, growth=2.4, overbright=1.7, alpha=48)),
)

_CREATION_NOISE_RE = re.compile(r'_class = "C_INIT_CreationNoise"')
_RANDOM_RADIUS_RE = re.compile(r'_class = "C_INIT_RandomRadius"')
_RANDOM_LIFETIME_RE = re.compile(r'_class = "C_INIT_RandomLifeTime"')
_RANDOM_ALPHA_RE = re.compile(r'_class = "C_INIT_RandomAlpha"')
_INTERPOLATE_RADIUS_RE = re.compile(r'_class = "C_OP_InterpolateRadius"')
_OUTPUT_MINMAX_RE = re.compile(r"(m_flOutputM(?:in|ax) = )([0-9.]+)")
_RADIUS_MINMAX_RE = re.compile(r"(m_flRadiusM(?:in|ax) = )([0-9.]+)")
_LIFETIME_MINMAX_RE = re.compile(r"(m_fLifetimeM(?:in|ax) = )([0-9.]+)")
_ALPHA_MINMAX_RE = re.compile(r"(m_nAlphaM(?:in|ax) = )([0-9]+)")
_END_SCALE_RE = re.compile(r"(m_flEndScale = )([0-9.]+)")
_OVERBRIGHT_RE = re.compile(r"(m_flOverbrightFactor = )([0-9.]+)")


def _scale_block_to_max(text: str, class_re, value_re, target: float, *,
                        integer: bool = False, skip_fielded: bool = False) -> str:
    """Scale each `class_re` block's min/max pair so its max lands exactly on `target`.

    Proportional, so the authored spread survives; bidirectional, so an earlier over-correction
    can be walked back up; idempotent, because a block already at the target scales by 1.0.
    `skip_fielded` protects C_INIT_CreationNoise blocks that name an explicit m_nFieldOutput --
    those drive some field other than radius and must not be touched.
    """
    for match in reversed(list(class_re.finditer(text))):
        start, end = common.block_span(text, match.start())
        block = text[start:end]
        if skip_fielded and "m_nFieldOutput" in block:
            continue
        values = [float(value) for _, value in value_re.findall(block)]
        peak = max(values) if values else 0.0
        if not peak or abs(peak - target) < 1e-6:
            continue
        factor = target / peak

        def rescale(m):
            scaled = float(m.group(2)) * factor
            return f"{m.group(1)}{max(1, int(round(scaled))) if integer else round(scaled, 2)}"

        text = text[:start] + value_re.sub(rescale, block) + text[end:]
    return text


def _set_in_blocks(text: str, class_re, value_re, target: float) -> str:
    """Set a scalar to an absolute value, scoped to its owning operator block."""
    for match in reversed(list(class_re.finditer(text))):
        start, end = common.block_span(text, match.start())
        block = text[start:end]
        new_block = value_re.sub(lambda m: f"{m.group(1)}{target}", block)
        if new_block != block:
            text = text[:start] + new_block + text[end:]
    return text


def tame_muzzle_smoke_bloom(root: Path) -> int:
    """Apply MUZZLE_SMOKE_TUNING to weapon muzzle smoke, first person and third.

    Scoped by NAME, not by walking a flash's child closure: MUZZLE_ROOTS is first-person only
    (every entry is _fp), so a closure walk silently misses world-only systems --
    weapon_muzzle_flash_smoke_small4 is reached only from weapon_muzzle_flash_smg.vpcf and
    friends, and third person is exactly where the UMP problem was reported.
    """
    changed = 0
    for path in sorted(root.glob("particles/filmmaker/povarehok/**/weapons/cs_weapon_fx/*.vpcf")):
        tuning = next((t for pattern, t in MUZZLE_SMOKE_TUNING if pattern.match(path.stem)), None)
        if tuning is None:
            continue
        text = path.read_text(encoding="utf-8")
        out = text
        if "radius" in tuning:
            # A CreationNoise block with no m_nFieldOutput defaults to the RADIUS field; that is
            # where the flash puffs get their real radius, not the file-scope m_flConstantRadius
            # (which the noise overwrites). The ac_muzzle_* clouds use C_INIT_RandomRadius.
            out = _scale_block_to_max(out, _RANDOM_RADIUS_RE, _RADIUS_MINMAX_RE, tuning["radius"])
            out = _scale_block_to_max(out, _CREATION_NOISE_RE, _OUTPUT_MINMAX_RE,
                                      tuning["radius"], skip_fielded=True)
        if "lifetime" in tuning:
            out = _scale_block_to_max(out, _RANDOM_LIFETIME_RE, _LIFETIME_MINMAX_RE,
                                      tuning["lifetime"])
        if "alpha" in tuning:
            out = _scale_block_to_max(out, _RANDOM_ALPHA_RE, _ALPHA_MINMAX_RE,
                                      tuning["alpha"], integer=True)
        if "growth" in tuning:
            out = _set_in_blocks(out, _INTERPOLATE_RADIUS_RE, _END_SCALE_RE, tuning["growth"])
        if "overbright" in tuning:
            out = _OVERBRIGHT_RE.sub(lambda m: f"{m.group(1)}{tuning['overbright']}", out)
        if out != text:
            path.write_text(out, encoding="utf-8")
            changed += 1
    return changed


# ---- the LMG muzzle flash spawns off the barrel tip -----------------------------------------
#
# "The muzzle flashes don't align on the LMGs" (user report 2026-09-01, ON mode).
#
# Povarehok's LMG flash keeps Source 1's offsets, and unlike every other weapon in the pack they
# are a wide SPREAD along the barrel axis rather than a fixed point:
#
#   weapon_muzzle_flash_para_flash   m_OffsetMin = [-1.0, 0, 0]  m_OffsetMax = [7.0, 0, 0]
#   weapon_muzzle_flash_para_b       m_OffsetMin = [-2.0, 0, 0]  m_OffsetMax = [-2.0, 0, 0]
#
# So flash sprites spawn anywhere from 2 units behind the muzzle to 7 in front, which on the
# M249/Negev's long barrel reads as a flash floating off the end of the gun. Every other
# Povarehok flash uses a tight fixed offset -- weapon_muzzle_flash_assualtrifle_flame is
# [1.0, 0, 0] on both ends -- and those align correctly.
#
# Note this is an ON-mode-only problem. The Modern pack's muzzleflash_lmg(_fp) already carries the
# CS2-calibrated MUZZLE_OFFSET_BARREL_TIP values ([0,0,-0.5] to [0.5,0,0]), byte-identical to its
# own muzzleflash_ar, so Modern's LMG needs no change and gets none.
#
# The fix clamps only the OUTLIERS into the range the rest of the pack uses, rather than forcing
# Modern's CS2 calibration onto Povarehok (which would shift every LMG flash sprite and change
# the pack's look, not just its alignment). _main and _glow already sit at [1.0, 0, 0] and are
# left untouched.
LMG_FLASH_OFFSET_MIN = 0.0
LMG_FLASH_OFFSET_MAX = 2.0

_OFFSET_VEC_RE = re.compile(r"(m_OffsetM(?:in|ax) = \[)(-?[0-9.]+)(, -?[0-9.]+, -?[0-9.]+\])")


def align_lmg_muzzle_flash(root: Path) -> int:
    """Clamp the barrel-axis (X) spawn offset of Povarehok's LMG flash into the range the rest of
    the pack uses, so the flash sits at the barrel tip instead of scattered around it."""
    changed = 0
    targets = [
        "weapon_muzzle_flash_para*.vpcf",   # LMG (M249/Negev)
        # The sniper flash is NOT clamped here. weapon_muzzle_flash_sparks7 spawns across
        # [1.0 .. 15.0] units, which is the "SCAR-20 flash sits in front of the gun" report --
        # but that flash is shared by the AWP, SCAR-20 and G3SG1, and the G3 looks right as
        # authored. Clamping it here fixed one weapon by moving three. fork_per_weapon_flash
        # gives the SCAR-20 its own copy instead; see PerWeaponFx.h for the runtime side.
    ]
    paths = sorted({p for pattern in targets for p in root.glob(
        f"particles/filmmaker/povarehok/**/weapons/cs_weapon_fx/{pattern}")})
    for path in paths:
        text = path.read_text(encoding="utf-8")

        def clamp(match):
            value = float(match.group(2))
            clamped = min(max(value, LMG_FLASH_OFFSET_MIN), LMG_FLASH_OFFSET_MAX)
            if abs(clamped - value) < 1e-6:
                return match.group(0)
            return f"{match.group(1)}{clamped}{match.group(3)}"

        out = _OFFSET_VEC_RE.sub(clamp, text)
        if out != text:
            path.write_text(out, encoding="utf-8")
            changed += 1
    return changed


# ---- the muzzle flash renders a dark box around itself --------------------------------------
#
# "Pistols have this black box around them when they fire, same with the AUG -- it doesn't happen
# on Modern, so why does it happen on Povarehok?" (user report 2026-09-01, ON mode, first person,
# R8 / Five-SeveN / M4A4 screenshots.)
#
# The answer is Source 1's $addself. Povarehok's flash cores are NOT additive materials -- they
# are alpha-blended sprites that lean on a strong self-add to lift their own dark background:
#
#     materials/particle/fire_particle_4/fire_particle_4.vmt
#         SpriteCard { $addself .75   $overbrightfactor 3.5   $DEPTHBLEND 1 }
#
# tone_down_muzzle_closure used to scale m_flAddSelfAmount by its `overbright` factor (0.35),
# taking 0.75 -> 0.2625. That is a category error: $addself is a BLEND control, not a brightness
# dial. At 0.26 the sprite renders as a plain alpha-blended fire texture, so the dark, low-alpha
# region of the sheet is composited over the scene as a visible dark quad -- the "black box".
#
# Why Modern never shows it: the ARC9 flash cores are authored $additive (muzzleflash_ar and
# friends resolve to hl2_muzzleflash_add.vmat and carry PARTICLE_OUTPUT_BLEND_MODE_ADD), and
# under ADD a dark background is mathematically invisible. Modern's only non-additive muzzle
# systems are smoke/dust. Povarehok mixes the two, so only Povarehok can show the artifact.
#
# Why it surfaced now rather than in July: the oversized muzzle smoke cloud used to sit directly
# on top of the flash and hide it. Trimming that smoke (MUZZLE_SMOKE_TUNING) uncovered a quad
# that had been there since the 2026-07-03 tone-down.
#
# The value is read back from the ORIGINAL Source 1 VMT rather than guessed. Note those VMTs are
# staged at <OutputRoot>/source1_game -- NOT under FX/sources/, which holds other packs' inputs.
_ADDSELF_VMT_RE = re.compile(r"^\s*\$addself\s+\"?([0-9.]+)\"?", re.IGNORECASE | re.MULTILINE)
_MATERIAL_RES_RE = re.compile(r'm_hMaterial = resource:"([^"]+)\.vmat"')
_ADDSELF_LINE_RE = re.compile(r"(\n\t\t\t)m_flAddSelfAmount = [0-9.]+")


def _source1_addself(source1_root: Path, material_res: str) -> float | None:
    """Read $addself out of the Source 1 VMT backing a converted vmat, or None if it has none."""
    vmt = source1_root / (material_res + ".vmt")
    if not vmt.is_file():
        return None
    try:
        text = vmt.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return None
    # Strip // comments first: this pack has $additive/$addself sitting inside commented-out
    # lines, and matching those raw has bitten the FX pipeline before.
    text = "\n".join(line.split("//")[0] for line in text.splitlines())
    match = _ADDSELF_VMT_RE.search(text)
    return float(match.group(1)) if match else None


def restore_muzzle_addself(root: Path, source1_root: Path) -> int:
    """Restore each muzzle renderer's m_flAddSelfAmount to the value its Source 1 VMT authored.

    Idempotent: once a renderer already carries the VMT value there is nothing to write.
    """
    changed = 0
    for path in sorted(root.glob("particles/filmmaker/povarehok/**/weapons/cs_weapon_fx/*.vpcf")):
        text = path.read_text(encoding="utf-8")
        out = text
        for start, end in reversed(list(common.iter_renderer_blocks(out))):
            block = out[start:end]
            material = _MATERIAL_RES_RE.search(block)
            if not material:
                continue
            authored = _source1_addself(source1_root, material.group(1))
            if authored is None:
                continue
            if _ADDSELF_LINE_RE.search(block):
                new_block = _ADDSELF_LINE_RE.sub(
                    lambda m: f"{m.group(1)}m_flAddSelfAmount = {authored}", block)
            else:
                # The converter dropped it entirely; put it back next to the material it belongs to.
                new_block = block.replace(
                    material.group(0),
                    f"{material.group(0)}\n\t\t\tm_flAddSelfAmount = {authored}", 1)
            if new_block != block:
                out = out[:start] + new_block + out[end:]
        if out != text:
            path.write_text(out, encoding="utf-8")
            changed += 1
    return changed


# ---- auto-sniper smoke has to clear without shortening the shotgun's ------------------------
#
# "Also make the auto snipers' smoke disappear at .7 secs too" (user request 2026-09-01, after the
# same change on assault rifles).
#
# The catch: auto snipers (SCAR-20/G3SG1) have no dedicated asset in the mod. ParticleFxRules maps
# weapon_muzzleflash_snip_ar(_fps) onto weapon_muzzle_flash_awp, and that flash's lingering cloud
# is ac_muzzle_shotgun_smoke / _smoke_b -- the SHOTGUN systems, which the user explicitly asked to
# have restored to their 2.5s Source 1 dwell one round earlier. Retuning them in place would
# silently undo that.
#
# So the sniper class gets its own copies instead. ac_muzzle_sniper_smoke(_b) are byte copies of
# the shotgun systems, and only weapon_muzzle_flash_awp is repointed at them; ac_muzzle_shotgun
# (the shotgun's own flash) keeps the originals untouched. MUZZLE_SMOKE_TUNING then tunes the two
# families independently -- identical values except lifetime, since only the dwell was asked about.
#
# Scope note: this covers the AWP as well as the auto snipers, because ON mode resolves BOTH
# weapon_muzzleflash_snip (AWP) and weapon_muzzleflash_snip_ar (auto snipers) to the same
# weapon_muzzle_flash_awp system. Separating them would need a new mapping target in
# ParticleFxRules.cpp and a DLL rebuild, not a particle change. The Scout/SSG08
# (weapon_muzzle_flash_huntingrifle_*) also shares the shotgun smoke and is deliberately NOT
# touched -- it was not part of the request.
SNIPER_SMOKE_FORKS = {
    "ac_muzzle_shotgun_smoke.vpcf": "ac_muzzle_sniper_smoke.vpcf",
    "ac_muzzle_shotgun_smoke_b.vpcf": "ac_muzzle_sniper_smoke_b.vpcf",
}
SNIPER_FORK_CONSUMER = "weapon_muzzle_flash_awp.vpcf"


def fork_sniper_muzzle_smoke(root: Path) -> int:
    """Give the sniper flash its own copy of the shotgun smoke systems so the two can be tuned
    apart. Idempotent: the copy is only written when it differs from the source."""
    changed = 0
    for consumer in sorted(root.glob(
            f"particles/filmmaker/povarehok/**/weapons/cs_weapon_fx/{SNIPER_FORK_CONSUMER}")):
        weapon_dir = consumer.parent
        text = consumer.read_text(encoding="utf-8")
        out = text
        for source_name, fork_name in SNIPER_SMOKE_FORKS.items():
            source = weapon_dir / source_name
            if not source.is_file():
                continue
            fork = weapon_dir / fork_name
            # Seed the fork from the shotgun system ONCE. After that the two diverge (different
            # lifetimes), so re-copying every run would clobber the sniper tuning.
            if not fork.is_file():
                fork.write_text(source.read_text(encoding="utf-8"), encoding="utf-8")
                changed += 1
            out = out.replace(f"/{source_name}", f"/{fork_name}")
        if out != text:
            consumer.write_text(out, encoding="utf-8")
            changed += 1
    return changed


# ---- per-weapon flash forks ------------------------------------------------------------------
#
# "If something is not working for one weapon but works for all of them, separate it. Make it so
# you can do a per weapon if you have to." (user directive 2026-09-01.)
#
# Several weapons share one flash system, so tuning it for one moves the rest. Two situations,
# fixed in two different places:
#
#   1. The weapons emit DIFFERENT CS2 particle names -- ParticleFxRules.cpp separates them on its
#      own. The Deagle is this case (uweapon_muzflsh_deagle is its own name); it was simply
#      pointed at the generic weapon_muzzle_flash_pistol.
#   2. The weapons emit the SAME name -- nothing in the particle can tell them apart, so the
#      runtime must look at the firing weapon. SCAR-20 vs G3SG1, M249 vs Negev. See PerWeaponFx.h.
#
# Either way the ASSET has to exist, which is this pass.
#
# It must fork DEEPLY. A flash root like weapon_muzzle_flash_pistol is a pure composition -- the
# offsets that actually need changing live several levels down, and those children are shared with
# every other pistol. Copying just the root produces a fork that is byte-identical in behaviour
# (the first attempt at this did exactly that and changed nothing). So every system on the path
# from the root down to a tweaked leaf is forked, and each fork's child references are rewritten
# to point at the forked copies. Systems off that path stay shared, which is the point: only what
# has to differ does.
#
# Naming is always "<stem><suffix>.vpcf", matching what PerWeaponFx_Override builds at runtime.
DEEP_FORK_SUFFIX_RE = re.compile(r"\.vpcf$")
DEAGLE_FLASH_FORWARD_OFFSET = 3.5
SCAR20_SPARK_MAX_OFFSET = 2.0

_OFFSET_X_RE = re.compile(r"(m_OffsetM(?:in|ax) = \[)(-?[0-9.]+)(, -?[0-9.]+, -?[0-9.]+\])")


def _tweak_clamp_forward(text: str, limit: float) -> str:
    return _OFFSET_X_RE.sub(
        lambda m: f"{m.group(1)}{min(float(m.group(2)), limit)}{m.group(3)}", text)


def _tweak_push_forward(text: str, distance: float) -> str:
    # Only positive (muzzle-side) offsets move; a negative ejection-side offset is left alone.
    return _OFFSET_X_RE.sub(
        lambda m: f"{m.group(1)}{max(float(m.group(2)), distance)}{m.group(3)}"
        if float(m.group(2)) >= 0 else m.group(0), text)


# (root system, fork suffix, {leaf system: tweak})
PER_WEAPON_FLASH_FORKS = (
    # SCAR-20: the shared sniper flash throws sparks up to 15 units down the barrel axis, which
    # reads as "in front of the gun" on the SCAR-20 but is fine on the G3SG1. Only the fork's own
    # spark copy is clamped, so AWP and G3SG1 keep the authored spread.
    ("weapon_muzzle_flash_awp", "_scar20",
     {"weapon_muzzle_flash_sparks7": lambda t: _tweak_clamp_forward(t, SCAR20_SPARK_MAX_OFFSET)}),
    # Deagle: the generic pistol flash offsets suit a short 9mm slide, so the flash landed short
    # of the Deagle's much longer barrel.
    ("weapon_muzzle_flash_pistol", "_deagle",
     {"ac_muzzle_pistol_center_b": lambda t: _tweak_push_forward(t, DEAGLE_FLASH_FORWARD_OFFSET)}),
    ("weapon_muzzle_flash_pistol_fp", "_deagle",
     {"ac_muzzle_pistol_center_b": lambda t: _tweak_push_forward(t, DEAGLE_FLASH_FORWARD_OFFSET),
      "weapon_muzzle_flash_pistol_core2": lambda t: _tweak_push_forward(
          t, DEAGLE_FLASH_FORWARD_OFFSET)}),
)


def _child_names(text: str) -> list[str]:
    return [ref.split("/")[-1][:-5]
            for ref in re.findall(r'm_ChildRef = resource:"([^"]+)"', text)]


def fork_per_weapon_flash(root: Path) -> int:
    """Deep-fork the per-weapon flash variants. Idempotent: a fork that already exists has
    diverged and is never re-seeded."""
    created = 0
    for root_name, suffix, leaf_tweaks in PER_WEAPON_FLASH_FORKS:
        for base in sorted(root.glob(
                f"particles/filmmaker/povarehok/**/weapons/cs_weapon_fx/{root_name}.vpcf")):
            weapon_dir = base.parent

            def read(name: str) -> str | None:
                path = weapon_dir / f"{name}.vpcf"
                return path.read_text(encoding="utf-8") if path.is_file() else None

            # Walk the closure and record each system's parents, so the path from the root down
            # to every tweaked leaf can be reconstructed.
            parents: dict[str, set[str]] = {}
            seen: set[str] = set()
            stack = [root_name]
            while stack:
                name = stack.pop()
                if name in seen:
                    continue
                seen.add(name)
                text = read(name)
                if text is None:
                    continue
                for child in _child_names(text):
                    parents.setdefault(child, set()).add(name)
                    stack.append(child)

            # Everything that must be forked: the tweaked leaves plus every ancestor of them.
            need: set[str] = set()
            pending = [leaf for leaf in leaf_tweaks if leaf in seen]
            while pending:
                name = pending.pop()
                if name in need:
                    continue
                need.add(name)
                pending.extend(parents.get(name, ()))
            need.add(root_name)
            if not need:
                continue

            for name in sorted(need):
                text = read(name)
                if text is None:
                    continue
                fork_path = weapon_dir / f"{name}{suffix}.vpcf"
                if fork_path.is_file():
                    continue  # already seeded and possibly hand-tuned since
                out = text
                for child in _child_names(text):
                    if child in need:
                        out = out.replace(f"/{child}.vpcf", f"/{child}{suffix}.vpcf")
                tweak = leaf_tweaks.get(name)
                if tweak is not None:
                    out = tweak(out)
                fork_path.write_text(out, encoding="utf-8")
                created += 1
    return created


def variant_resource(variant: str, folder: str, name: str) -> str:
    return f"particles/filmmaker/povarehok/{variant}/{folder}/{name}"


IMPACT_SMOKE_HINT_RE = re.compile(
    r"(smoke|dust|puff|steam|vistasmoke|wd_gfx_steam|copyka228smoke|cursedgovno|burn)",
    re.IGNORECASE,
)
IMPACT_SMOKE_SKIP_RE = re.compile(r"(spark|glow|flare|star|tracer|ricochet)", re.IGNORECASE)


def _is_impact_smoke_renderer(path: Path, block: str) -> bool:
    resources = "\n".join(common.RESOURCE_RE.findall(block))
    haystack = f"{path.name}\n{resources}"
    if IMPACT_SMOKE_SKIP_RE.search(haystack):
        return False
    return bool(IMPACT_SMOKE_HINT_RE.search(haystack))


def fix_impact_smoke_blending(root: Path) -> tuple[int, int]:
    """Keep impact smoke/dust alpha-blended and neutral under heavy overlap.

    Source 1 impact dust relied on SpriteCard's alpha blending. Some converted
    impact smoke children come through additive and overbright, so stacked bullet
    impacts tint toward hot red/orange instead of just fading out.
    """
    changed_files = changed_renderers = 0
    for path in iter_povarehok_impact_vpcfs(root):
        text = path.read_text(encoding="utf-8")
        edits = []  # (start, end, replacement)
        for start, end in common.iter_renderer_blocks(text):
            block = text[start:end]
            if not _is_impact_smoke_renderer(path, block):
                continue
            new_block = common.ADD_BLEND_OUTPUT_LINE_RE.sub("", block)
            new_block = common.OVERBRIGHT_OUTPUT_LINE_RE.sub(
                r"\1m_flOverbrightFactor = 1.0", new_block
            )
            new_block = common.ADD_SELF_OUTPUT_LINE_RE.sub(r"\1m_flAddSelfAmount = 0.0", new_block)
            if new_block != block:
                edits.append((start, end, new_block))
                changed_renderers += 1
        if edits:
            for start, end, replacement in reversed(edits):
                text = text[:start] + replacement + text[end:]
            path.write_text(text, encoding="utf-8")
            changed_files += 1
    return changed_files, changed_renderers


VISTASMOKES_RE = re.compile(r"vistasmoke", re.IGNORECASE)
TEXTURE_LINE_RE = re.compile(r'm_hTexture\s*=\s*resource:"[^"]+"')
MATERIAL_LINE_RE = re.compile(r'm_hMaterial\s*=\s*resource:"[^"]+"')

# 21 unique systems (regular + less/impacts|smoke) — fire/explosion vistasmoke is untouched.
NON_FIRE_VISTASMOKESYSTEMS: dict[str, str] = {
    "impact_concrete_child_smoke": "impact_dust",
    "impact_dirt_child_smoke_puff": "impact_thin",
    "impact_generic_smoke_small": "impact_thin",
    "impact_generic_smoke_medium": "impact_thin",
    "impact_generic_smoke_large": "impact_thin",
    "impact_screen_smoke_small": "impact_thin",
    "impact_screen_smoke_medium": "impact_thin",
    "impact_screen_smoke_large": "impact_thin",
    "weapon_muzzle_flash_smoke_small": "muzzle",
    "weapon_muzzle_flash_smoke_small2": "muzzle",
    "weapon_muzzle_flash_smoke_small4": "muzzle",
    "weapon_shell_eject_smoke_assrifle2": "shell",
    "weapon_shell_eject_smoke_assrifle3": "shell",
    "weapon_shell_eject_smoke_awp3": "shell",
    "weapon_shell_eject_smoke_para3": "shell",
    "weapon_shell_eject_smoke_pistol3": "shell",
    "weapon_shell_eject_smoke_shotgun1": "shell",
    "weapon_shell_eject_smoke_shotgun2": "shell",
    "weapon_shell_eject_smoke_shotgun3": "shell",
    "weapon_shell_eject_smoke_smg1": "shell",
    "weapon_shell_eject_smoke_smg3": "shell",
}

INSURGENCY_MATERIAL_NAMES = {
    "impact_dust": "dust_puff",
    "impact_thin": "ins_animsmokethin_01",
    "muzzle": "ins_muzzle_smoke",
    "shell": "particle_shells",
}

FALLBACK_MATERIALS = {
    "impact_dust": "materials/particle/ac/insandstorm_t_thinsmoke_05_bc",
    "impact_thin": "materials/particle/ac/insandstorm_t_thinsmoke_01_bc",
    "muzzle": "materials/particle/smoke1/smoke1",
    "shell": "materials/particle/ac/sq_fulldustfront1_2",
}


def _material_exists(root: Path, resource: str) -> bool:
    rel = resource.replace("\\", "/")
    return (root / f"{rel}.vmat").is_file() or (root / f"{rel}.vtex").is_file()


def _resolve_smoke_material(root: Path, role: str) -> str:
    insurgency = f"materials/particle/insurgency/{INSURGENCY_MATERIAL_NAMES[role]}"
    if _material_exists(root, insurgency):
        return insurgency
    return FALLBACK_MATERIALS[role]


def _set_renderer_field(block: str, key: str, value: str) -> str:
    match = re.search(rf"(?m)^(\s*){re.escape(key)} = ", block)
    if match:
        indent = match.group(1)
        return re.sub(
            rf"(?m)^{re.escape(indent)}{re.escape(key)} = [^\n]+",
            f"{indent}{key} = {value}",
            block,
            count=1,
        )
    return re.sub(
        r'(_class = "C_OP_RenderSprites")',
        rf'\1\n\t\t\t{key} = {value}',
        block,
        count=1,
    )


def _remove_renderer_field(block: str, key: str) -> str:
    return re.sub(rf"(?m)^\s*{re.escape(key)} = [^\n]+\n?", "", block)


def _normalize_impact_smoke_renderer(block: str) -> str:
    block = _set_renderer_field(block, "m_bBlendFramesSeq0", "true")
    block = _set_renderer_field(block, "m_flOverbrightFactor", "1.0")
    block = _set_renderer_field(block, "m_flSelfIllumAmount", "0.0")
    block = _set_renderer_field(block, "m_nAnimationType", '"ANIMATION_TYPE_FIT_LIFETIME"')
    block = _set_renderer_field(block, "m_flAnimationRate", "0.6")
    block = _remove_renderer_field(block, "m_flAnimationRate2")
    return block


def _normalize_muzzle_smoke_renderer(block: str) -> str:
    block = _set_renderer_field(block, "m_bBlendFramesSeq0", "true")
    block = _remove_renderer_field(block, "m_flAnimationRate2")
    if "m_flAnimationRate" not in block:
        block = _set_renderer_field(block, "m_flAnimationRate", "8.0")
    return block


def _normalize_shell_smoke_renderer(block: str) -> str:
    block = _set_renderer_field(block, "m_bBlendFramesSeq0", "true")
    block = _set_renderer_field(block, "m_flOverbrightFactor", "2.0")
    block = _set_renderer_field(block, "m_nAnimationType", '"ANIMATION_TYPE_FIT_LIFETIME"')
    block = _set_renderer_field(block, "m_flAnimationRate", "0.5")
    block = _remove_renderer_field(block, "m_flAnimationRate2")
    return block


def _normalize_swapped_renderer(block: str, role: str) -> str:
    if role in ("impact_dust", "impact_thin"):
        return _normalize_impact_smoke_renderer(block)
    if role == "muzzle":
        return _normalize_muzzle_smoke_renderer(block)
    return _normalize_shell_smoke_renderer(block)


def _swap_renderer_textures(block: str, material_base: str) -> str:
    tex = f'resource:"{material_base}.vtex"'
    mat = f'resource:"{material_base}.vmat"'
    block = TEXTURE_LINE_RE.sub(f"m_hTexture = {tex}", block)
    block = MATERIAL_LINE_RE.sub(f"m_hMaterial = {mat}", block)
    return block


def _neutralize_impact_colors(text: str) -> str:
    text = re.sub(
        r"m_ColorMin = \[\d+, \d+, \d+, 255\]",
        "m_ColorMin = [120, 120, 120, 255]",
        text,
    )
    text = re.sub(
        r"m_ColorMax = \[\d+, \d+, \d+, 255\]",
        "m_ColorMax = [170, 170, 170, 255]",
        text,
    )
    return text


def iter_non_fire_vistasmoke_vpcfs(root: Path):
    pvrh_root = root / "particles" / "filmmaker" / "povarehok"
    if not pvrh_root.is_dir():
        return
    for path in pvrh_root.rglob("*.vpcf"):
        if any(part in ("inferno_fx", "explosions_fx", "explosions_fx2") for part in path.parts):
            continue
        role = NON_FIRE_VISTASMOKESYSTEMS.get(path.stem)
        if role is None:
            continue
        rel = path.relative_to(pvrh_root).as_posix()
        if "impact_fx/" not in rel and "weapons/cs_weapon_fx/" not in rel:
            continue
        yield path, role


def patch_non_fire_vistasmoke_replacements(root: Path) -> tuple[int, int]:
    """Retarget bullet-impact and weapon wisps off the volumetric vistasmoke sheet."""
    changed_files = changed_renderers = 0
    for path, role in iter_non_fire_vistasmoke_vpcfs(root):
        text = path.read_text(encoding="utf-8")
        if not VISTASMOKES_RE.search(text):
            continue
        material_base = _resolve_smoke_material(root, role)
        edits = []
        for start, end in common.iter_renderer_blocks(text):
            block = text[start:end]
            if not VISTASMOKES_RE.search(block):
                continue
            new_block = _swap_renderer_textures(block, material_base)
            new_block = _normalize_swapped_renderer(new_block, role)
            if new_block != block:
                edits.append((start, end, new_block))
                changed_renderers += 1
        if not edits:
            continue
        for start, end, replacement in reversed(edits):
            text = text[:start] + replacement + text[end:]
        if role.startswith("impact"):
            text = _neutralize_impact_colors(text)
        text = common.DUAL_SEQUENCE_LINE_RE.sub("", text)
        path.write_text(text, encoding="utf-8")
        changed_files += 1
    return changed_files, changed_renderers


# Animated smoke/dust SHEETS whose animation must play ONCE across the particle lifetime.
# Source 1 authored these with a fixed C_OP_RenderSprites frame rate (m_flAnimationRate) that
# lined up with the particle's lifetime; CS2 + the reconstructed .mks sheets instead LOOP the
# sheet (frame = age*rate mod frames), so a single shot's muzzle smoke -- and a single wall
# hit's impact smoke, which shares these same sheets on some weapons -- visibly re-plays the
# sprite over and over (user report 2026-07-06 "the smoke replays / plays the sprite over and
# over"). ANIMATION_TYPE_FIT_LIFETIME maps the sheet to [0..1] of the lifetime = plays exactly
# once, no wrap (this is what the already-correct impact_generic_smoke_* systems use). Scoped
# by TEXTURE to smoke/dust sheets only: muzzle-flash flames (snipermuzzle/pistolmuzzle/...),
# sparks, fire, and blood in the same folders keep their intentional fast fixed-rate flicker.
LOOPING_SMOKE_SHEET_RE = re.compile(
    r"(thinsmoke|insandstorm|wd_gfx_steam|ins_muzzle_smoke|ins_animsmoke|"
    r"beam_smoke|copyka228smoke|dust_puff|fas_dust|/smoke1/|sq_fulldustfront|"
    r"water_splash)",
    re.IGNORECASE,
)

# Sheets whose .mks sequences get their LOOP flag dropped (clamp_sheet_sequences):
# one-shot smoke/dust/steam/splash ONLY. Deliberately narrower than
# LOOPING_SMOKE_SHEET_RE's renderer-texture match: a bare "insandstorm" would also
# de-loop the pack's spark/blood/debris/airburst sheets, whose fast looping flicker is
# intentional (caught 2026-07-06 -- the first clamp run swept those in and had to be
# restored).
CLAMP_SMOKE_SHEET_RE = re.compile(
    r"(thinsmoke|animsmoke|ins_muzzle_smoke|wd_gfx_steam|copyka228smoke|dust_puff|"
    r"fas_dust|/smoke1/|smoke1\.mks|sq_fulldustfront|water_splash)",
    re.IGNORECASE,
)


def _iter_weapon_and_impact_vpcfs(root: Path):
    pvrh_root = root.joinpath("particles", "filmmaker", "povarehok")
    if pvrh_root.is_dir():
        for path in pvrh_root.rglob("*.vpcf"):
            rel = path.relative_to(pvrh_root).as_posix()
            # explosions_fx added 2026-09-03: the ON-mode HE grenade's
            # simpleartyfx_mid_smoke_base carries rate 0.35 with no FIT_LIFETIME and a 3.5 s
            # life, so its sheet wraps at ~1.2 playthroughs and replays a few frames just as
            # the smoke is dying -- the user's "it loops at the end, but only on the smoke".
            # Same bug class, same fix; this walk was simply not reaching the file.
            if ("weapons/cs_weapon_fx/" in rel or "impact_fx" in rel
                    or "explosions_fx" in rel):
                yield path
    # The Modern pack's muzzle smoke (fas_dust_* sheets on muzzle_smoke_c,
    # muzzle_supressed_smoke_big, the suppressed flash, ...) replayed exactly the same
    # way but was missed by the original Povarehok-only walk (found 2026-07-06 while the
    # user still saw double-playing smoke after the first fix).
    modern_root = root.joinpath("particles", "filmmaker", "modern", "arc9_fas_muzzleflashes")
    if modern_root.is_dir():
        yield from modern_root.rglob("*.vpcf")


def fix_looping_smoke_animation(root: Path) -> tuple[int, int]:
    """Make weapon + bullet-impact smoke sprite sheets play ONCE (loop -> FIT_LIFETIME).

    The animation rate still MULTIPLIES under FIT_LIFETIME (frame = normalizedAge *
    rate * sheet time), so any rate > 1.0 overshoots the sheet and -- on a LOOP-flagged
    sequence -- wraps back to frame 0 and replays. Cap the rate at 1.0 on these smoke
    renderers (exactly one pass over the particle lifetime); rates <= 1.0 are authored
    pacing and stay. The sheets themselves are additionally clamped by
    common.clamp_sheet_sequences so fixed-rate overshoots hold the last frame too.

    Idempotent: FIT_LIFETIME and the rate cap are fixed literals. C_OP_RenderRopes wisps
    (weapon_muzzle_smoke_long's beam) are untouched -- only sprite renderers loop a sheet.
    """
    rate_re = re.compile(r"(?m)^(\s*)m_flAnimationRate = (\d+(?:\.\d+)?)(\s*)$")
    changed_files = changed_renderers = 0
    for path in _iter_weapon_and_impact_vpcfs(root):
        text = path.read_text(encoding="utf-8")
        edits = []
        for start, end in common.iter_renderer_blocks(text):
            block = text[start:end]
            if '_class = "C_OP_RenderSprites"' not in block:
                continue
            tex = re.search(r'm_hTexture = resource:"([^"]+)"', block)
            if not tex or not LOOPING_SMOKE_SHEET_RE.search(tex.group(1)):
                continue
            new_block = block
            if "ANIMATION_TYPE_FIT_LIFETIME" not in new_block:
                new_block = _set_renderer_field(
                    new_block, "m_nAnimationType", '"ANIMATION_TYPE_FIT_LIFETIME"'
                )
            new_block = rate_re.sub(
                lambda m: m.group(0) if float(m.group(2)) <= 1.0
                else f"{m.group(1)}m_flAnimationRate = 1.0{m.group(3)}",
                new_block,
            )
            if new_block != block:
                edits.append((start, end, new_block))
                changed_renderers += 1
        if edits:
            for start, end, replacement in reversed(edits):
                text = text[:start] + replacement + text[end:]
            path.write_text(text, encoding="utf-8")
            changed_files += 1
    return changed_files, changed_renderers


IMPACT_SETTLE_HINT_RE = re.compile(
    r"(clique|clump|chunk|chunks|debris|bits|gib|child_bounce|concrete_b|tile_b)",
    re.IGNORECASE,
)
IMPACT_SETTLE_SKIP_RE = re.compile(
    r"(spark|glow|trail|smoke|dust|blood|spurt|splash|water)",
    re.IGNORECASE,
)
BOUNCE_AMOUNT_LINE_RE = re.compile(r"(?m)^(\s*)m_flBounceAmount = [^\n]+")
IMPACT_SPIN_CLASS_RE = re.compile(r'_class = "C_OP_Spin(?:Yaw)?"')
SPIN_STOP_LINE_RE = re.compile(r"(?m)^(\s*)m_fSpinRateStopTime = (-?\d+(?:\.\d+)?)")
MAX_IMPACT_SPIN_SECONDS = 0.5


def _is_impact_debris_that_should_settle(path: Path, text: str) -> bool:
    name = path.name
    if IMPACT_SETTLE_SKIP_RE.search(name):
        return False
    collision_driven = "C_OP_WorldTraceConstraint" in text and (
        "C_OP_RenderModels" in text or "C_OP_Spin" in text
    )
    return collision_driven or bool(IMPACT_SETTLE_HINT_RE.search(name))


def _cap_spin_stop_time(block: str) -> str:
    match = SPIN_STOP_LINE_RE.search(block)
    if match:
        current = float(match.group(2))
        if current <= MAX_IMPACT_SPIN_SECONDS:
            return block
        return SPIN_STOP_LINE_RE.sub(
            rf"\1m_fSpinRateStopTime = {MAX_IMPACT_SPIN_SECONDS}", block, count=1
        )
    return re.sub(
        r'(_class = "C_OP_Spin(?:Yaw)?")',
        rf"\1\n\t\t\tm_fSpinRateStopTime = {MAX_IMPACT_SPIN_SECONDS}",
        block,
        count=1,
    )


def settle_impact_debris(root: Path) -> tuple[int, int, int]:
    """Stop physical bullet-impact chunks from bouncing or spinning indefinitely."""
    changed_files = changed_constraints = changed_spin_ops = 0
    for path in iter_povarehok_impact_vpcfs(root):
        text = path.read_text(encoding="utf-8")
        if not _is_impact_debris_that_should_settle(path, text):
            continue
        edits = []  # (start, end, replacement)
        for match in common.WORLD_TRACE_CLASS_RE.finditer(text):
            start, end = common.block_span(text, match.start())
            block = text[start:end]
            new_block = BOUNCE_AMOUNT_LINE_RE.sub(r"\1m_flBounceAmount = 0.0", block)
            if new_block != block:
                edits.append((start, end, new_block))
                changed_constraints += 1
        for match in IMPACT_SPIN_CLASS_RE.finditer(text):
            start, end = common.block_span(text, match.start())
            block = text[start:end]
            new_block = _cap_spin_stop_time(block)
            if new_block != block:
                edits.append((start, end, new_block))
                changed_spin_ops += 1
        if edits:
            for start, end, replacement in sorted(edits, reverse=True):
                text = text[:start] + replacement + text[end:]
            path.write_text(text, encoding="utf-8")
            changed_files += 1
    return changed_files, changed_constraints, changed_spin_ops


DIRECT_EMIT_COUNT_RE = re.compile(r"(?m)^(\s*m_nParticlesToEmit = )(\d+)(\s*)$")
RANDOM_EMIT_COUNT_RE = re.compile(r"(?m)^(\s*m_flRandom(?:Min|Max) = )(-?\d+(?:\.\d+)?)(\s*)$")


def _format_scaled_number(source: str, value: float) -> str:
    if "." not in source and value.is_integer():
        return str(int(value))
    return f"{value:.6f}".rstrip("0").rstrip(".")


def _halve_debris_emitter(block: str) -> str:
    def direct_repl(match: re.Match[str]) -> str:
        value = max(1, math.ceil(int(match.group(2)) * 0.5))
        return f"{match.group(1)}{value}{match.group(3)}"

    new = DIRECT_EMIT_COUNT_RE.sub(direct_repl, block)
    if re.search(r"m_nParticlesToEmit =\s*\n\s*\{", block):
        def random_repl(match: re.Match[str]) -> str:
            value = float(match.group(2)) * 0.5
            return f"{match.group(1)}{_format_scaled_number(match.group(2), value)}{match.group(3)}"

        new = RANDOM_EMIT_COUNT_RE.sub(random_repl, new)
    return new


def halve_less_impact_debris(root: Path) -> tuple[int, int]:
    """Halve physical debris emission in the Less impacts variant only.

    This runs only in the clean conversion pipeline, which is already documented as
    non-rerunnable because its other tone passes are multiplicative too.
    """
    changed_files = changed_emitters = 0
    for path in iter_povarehok_impact_vpcfs(root):
        if "less/impacts" not in path.as_posix():
            continue
        text = path.read_text(encoding="utf-8")
        if not _is_impact_debris_that_should_settle(path, text):
            continue
        edits = []
        for match in re.finditer(r'_class = "C_OP_InstantaneousEmitter"', text):
            start, end = common.block_span(text, match.start())
            block = text[start:end]
            new_block = _halve_debris_emitter(block)
            if new_block != block:
                edits.append((start, end, new_block))
                changed_emitters += 1
        if edits:
            for start, end, replacement in sorted(edits, reverse=True):
                text = text[:start] + replacement + text[end:]
            path.write_text(text, encoding="utf-8")
            changed_files += 1
    return changed_files, changed_emitters


IMPACT_WARM_SMOKE_FILES = (
    "impact_fx/copyka228smoke2.vpcf",
    "impact_fx/cursedgovno1.vpcf",
)


def fix_warm_impact_smoke_tints(root: Path) -> int:
    """Neutralize brown/orange impact-smoke tints that read as red when stacked."""
    changed = 0
    for rel in IMPACT_WARM_SMOKE_FILES:
        for variant in VARIANTS:
            path = common.resource_path(root, f"particles/filmmaker/povarehok/{variant}/{rel}")
            if not path.is_file():
                continue
            text = new = path.read_text(encoding="utf-8")
            new = re.sub(
                r"m_ColorMin = \[\d+, \d+, \d+, 255\]",
                "m_ColorMin = [120, 120, 120, 255]",
                new,
            )
            new = re.sub(
                r"m_ColorMax = \[\d+, \d+, \d+, 255\]",
                "m_ColorMax = [170, 170, 170, 255]",
                new,
            )
            new = re.sub(
                r"m_ConstantColor = \[\d+, \d+, \d+, 255\]",
                "m_ConstantColor = [140, 140, 140, 255]",
                new,
            )
            new = re.sub(r"m_flSelfIllumAmount = 1\.0", "m_flSelfIllumAmount = 0.0", new)
            new = common.ADD_BLEND_OUTPUT_LINE_RE.sub("", new)
            if new != text:
                path.write_text(new, encoding="utf-8")
                changed += 1
    return changed


PVRH_WEAPON_VARIANTS = ("regular", "less/smoke")


def _pvrh_weapon_dir(variant: str) -> str:
    return f"particles/filmmaker/povarehok/{variant}/weapons/cs_weapon_fx"


LESS_MUZZLE_SMOKE_SCALE = 0.9
MUZZLE_SMOKE_ALPHA_FIELDS = ("m_nAlphaMin", "m_nAlphaMax")
MUZZLE_SMOKE_RADIUS_FIELDS = (
    "m_flRadiusMin", "m_flRadiusMax", "m_flStartScale", "m_flEndScale"
)


def _scale_fields(block: str, fields: tuple[str, ...], factor: float, *, integer: bool) -> str:
    field_group = "|".join(re.escape(field) for field in fields)
    pattern = re.compile(rf"(?m)^(\s*(?:{field_group}) = )(-?\d+(?:\.\d+)?)(\s*)$")

    def repl(match: re.Match[str]) -> str:
        scaled = float(match.group(2)) * factor
        value = str(max(0, round(scaled))) if integer else _format_scaled_number(match.group(2), scaled)
        return f"{match.group(1)}{value}{match.group(3)}"

    return pattern.sub(repl, block)


def _reduce_muzzle_smoke_text(text: str) -> str:
    edits = []
    class_fields = {
        "C_INIT_RandomAlpha": (MUZZLE_SMOKE_ALPHA_FIELDS, True),
        "C_INIT_RandomRadius": (MUZZLE_SMOKE_RADIUS_FIELDS, False),
        "C_OP_InterpolateRadius": (MUZZLE_SMOKE_RADIUS_FIELDS, False),
    }
    for class_name, (fields, integer) in class_fields.items():
        for match in re.finditer(rf'_class = "{class_name}"', text):
            start, end = common.block_span(text, match.start())
            block = text[start:end]
            new_block = _scale_fields(block, fields, LESS_MUZZLE_SMOKE_SCALE, integer=integer)
            if new_block != block:
                edits.append((start, end, new_block))
    for start, end, replacement in sorted(edits, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text


def _is_muzzle_smoke_leaf(path: Path) -> bool:
    name = path.name.lower()
    return "smoke" in name or ("ac_muzzle" in name and "trail" in name)


def write_less_muzzle_smoke_variant(root: Path, changed: list[str]) -> None:
    """Derive Less smoke leaves from repaired Regular leaves, then scale by 10%."""
    regular_dir = common.resource_path(root, _pvrh_weapon_dir("regular"))
    less_dir = common.resource_path(root, _pvrh_weapon_dir("less/smoke"))
    if not regular_dir.is_dir() or not less_dir.is_dir():
        return
    for regular_path in regular_dir.glob("*.vpcf"):
        if not _is_muzzle_smoke_leaf(regular_path):
            continue
        less_path = less_dir / regular_path.name
        if not less_path.is_file():
            continue
        text = regular_path.read_text(encoding="utf-8").replace(
            "/povarehok/regular/", "/povarehok/less/smoke/"
        )
        new_text = _reduce_muzzle_smoke_text(text)
        old_text = less_path.read_text(encoding="utf-8")
        if new_text != old_text:
            less_path.write_text(new_text, encoding="utf-8")
            changed.append(less_path.relative_to(root).as_posix())
# Former per-shot direct-child smoke flashes; kept only so the apply loop can strip the
# obsolete smoke_long child ref from already-patched trees (now spray-pair bases).
PVRH_PER_SHOT_SMOKE_FLASHES = (
    "weapon_muzzle_flash_awp.vpcf",
    "weapon_muzzle_flash_huntingrifle_fp.vpcf",
)

WEAPON_MUZZLE_SMOKE_FILES = tuple(
    f"particles/filmmaker/povarehok/{variant}/weapons/cs_weapon_fx/{name}"
    for variant in VARIANTS
    for name in (
        "weapon_muzzle_smoke_long.vpcf",
        "weapon_muzzle_smoke_long_b.vpcf",
        "weapon_muzzle_smoke.vpcf",
        "weapon_muzzle_smoke_b.vpcf",
        "weapon_muzzle_smoke_b_version_#2.vpcf",
    )
)

# Converted explosion roots -> stock CS2 distortion child restoring the native
# heat-shimmer the vanilla systems had.
POVAREHOK_DISTORT_CHILDREN = {
    "particles/filmmaker/povarehok/regular/explosions_fx/explosion_basic.vpcf": common.STOCK_HE_DISTORT,
    "particles/filmmaker/povarehok/regular/explosions_fx/explosion_hegrenade_interior.vpcf": common.STOCK_HE_DISTORT,
    "particles/filmmaker/povarehok/less/smoke/explosions_fx/explosion_basic.vpcf": common.STOCK_HE_DISTORT,
    "particles/filmmaker/povarehok/less/smoke/explosions_fx/explosion_hegrenade_interior.vpcf": common.STOCK_HE_DISTORT,
    "particles/filmmaker/povarehok/regular/explosions_fx/explosion_c4_500.vpcf": common.STOCK_C4_DISTORT,
    "particles/filmmaker/povarehok/less/smoke/explosions_fx/explosion_c4_500.vpcf": common.STOCK_C4_DISTORT,
}


# The FAS barrel wisp pair only (ac_muzzle_shotgun_alt_barrel_smoke_trail{,_b}).
# GOTCHA (found 2026-07-06, the "clumped/warped muzzle smoke" screenshot): an earlier
# "ac_muzzle*trail*" name match also swept up ac_muzzle_smg_trail{,5} and
# ac_muzzle_shotgun_trail -- short per-shot smoke TRACES, not barrel wisps -- and
# attached all five to weapon_muzzle_smoke_long, so every spray shot stacked five
# overlapping ribbons at the muzzle. Only the real barrel wisps belong here; the traces
# stay untouched as native children of their own flash systems.
def iter_muzzle_trail_wisp_vpcfs(root: Path):
    pvrh_root = root.joinpath("particles", "filmmaker", "povarehok")
    for path in pvrh_root.rglob("*.vpcf"):
        name = path.name.lower()
        if "ac_muzzle" in name and "barrel_smoke_trail" in name and "weapons" in path.parts:
            yield path


# Wrongly-attached smoke_long children to strip from already-patched trees (see above).
WRONG_SMOKE_LONG_CHILDREN = {
    "ac_muzzle_smg_trail.vpcf",
    "ac_muzzle_smg_trail5.vpcf",
    "ac_muzzle_shotgun_trail.vpcf",
}


# Stock CS2 weapon_muzzle_smoke_long's own brief lock (decompiled from pak01 2026-07-06):
# newborn smoke anchors to the muzzle for the first 0.1s, then rises freely. The plume's
# base keeps tracking the gun via CONTINUOUS EMISSION at the engine-driven muzzle CP,
# exactly like Valve's own asset -- do NOT full-lock the whole rising column to the gun
# (rigid ride) and do NOT inject CP configs (froze the engine's own driving).
_CS2_STOCK_BRIEF_POSITION_LOCK = (
    "\t\t{\n"
    '\t\t\t_class = "C_OP_PositionLock"\n'
    "\t\t\tm_flStartTime_min = 0.0\n"
    "\t\t\tm_flStartTime_max = 0.0\n"
    "\t\t\tm_flEndTime_min = 0.1\n"
    "\t\t\tm_flEndTime_max = 0.1\n"
    "\t\t},\n"
)


def patch_cs2_muzzle_smoke_alignment(text: str, *, sustained: bool = False) -> str:
    """CS2 muzzle alignment for the sustained Povarehok barrel plume.

    2026-07-06 night rework, calibrated against the DECOMPILED stock CS2
    weapon_muzzle_smoke_long (Valve's own barrel smoke, which the mod's version is a
    copy of): world-space, barrel-tip spawn, stock initial-velocity spread
    ([-10,-10,10]..[10,10,15] -- an earlier +/-1.5 "de-clump" over-tightened the rope
    into the dense dark column of the user's screenshot), stock brief 0.1s PositionLock
    (base tracking comes from emission at the CP-followed muzzle, not from locking the
    risen column), and the stock ADDITIVE rope blend on beam_smoke_01 (the earlier
    blanket ADD-strip in this function alpha-blended the dark-gray rope into a BLACK
    smoke column; stock renders those 46-83 grays additively = a soft translucent wisp).
    """
    new = re.sub(
        r'\{\s*_class = "C_OP_PositionLock"\s*(?:m_bLockRot = true\s*)?(?:m_flStartTime_min = [^\}]*)?\},?\s*',
        "",
        text,
        flags=re.MULTILINE | re.DOTALL,
    )
    if sustained and "C_OP_PositionLock" not in new:
        anchor = re.search(r"m_Operators = \n\t\[", new)
        if anchor:
            new = new[: anchor.end()] + "\n" + _CS2_STOCK_BRIEF_POSITION_LOCK + new[anchor.end():]

    new = new.replace("m_bLocalCoords = false", "m_bLocalCoords = true")
    # FINAL (2026-07-06 third round): WORLD-pass like the stock asset -- viewmodel-pass
    # smoke rides the camera rigidly and never lags in the air (the "doesn't move like
    # it would in real life" complaint). The engine drives the muzzle CP itself; no
    # config injection (that froze it -- see remove_muzzle_follow_config).
    new = new.replace("m_bViewModelEffect = true", "m_bViewModelEffect = false")
    new = common.remove_muzzle_follow_config(new)

    # Zero any converted/previously-patched emission kick; the plume must not be shot
    # forward out of the barrel (stock CreateWithinSphereTransform has no speed at all).
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
    # Stock spread: mostly-upward drift with enough lateral variance that the rope
    # doesn't collapse into a single dense column.
    new = re.sub(r"(?m)^(\s*)m_vecOutputMin = \[[^\]]*\]", r"\1m_vecOutputMin = [-10.0, -10.0, 10.0]", new)
    new = re.sub(r"(?m)^(\s*)m_vecOutputMax = \[[^\]]*\]", r"\1m_vecOutputMax = [10.0, 10.0, 15.0]", new)

    new = new.replace('_class = "C_OP_RenderTrails"', '_class = "C_OP_RenderSprites"\n\t\t\tm_bBlendFramesSeq0 = true')
    new = re.sub(r"\tm_bAdditive = 1\n", "", new)
    new = common.OVERBRIGHT_OUTPUT_LINE_RE.sub(r"\1m_flOverbrightFactor = 1.0", new)

    # Stock additive blend on the beam_smoke_01 rope (see docstring). Targeted by
    # texture so translucent sheet smoke in the same family never goes additive
    # (additive ignores the alpha mask -> square frames).
    edits = []
    for start, end in common.iter_renderer_blocks(new):
        block = new[start:end]
        if "beam_smoke_01" not in block:
            continue
        patched = block
        if "m_nOutputBlendMode" not in patched:
            patched = patched.replace(
                '_class = "C_OP_RenderRopes"',
                '_class = "C_OP_RenderRopes"\n\t\t\t' + common.ADDITIVE_BLEND_LINE,
                1,
            )
        # Unlit like every other additive renderer (fix_lit_renderers' rule); undoes
        # the 0.0 an earlier version of this function baked in.
        patched = patched.replace("m_flSelfIllumAmount = 0.0", "m_flSelfIllumAmount = 1.0")
        if patched != block:
            edits.append((start, end, patched))
    for start, end, replacement in reversed(edits):
        new = new[:start] + replacement + new[end:]

    new = new.replace("m_flMaxLength = 11.0", "m_flMaxLength = 24.0")
    new = new.replace("m_flMinLength = 30.0", "m_flMinLength = 12.0")

    new = common._ensure_cs2_muzzle_position_offset(new, common.MUZZLE_OFFSET_BARREL_TIP)

    # Close the puff-to-plume visibility gap: the plume must be visible while the
    # per-shot muzzle puff (0.2s life) still is, or one shot reads as two smokes.
    new = re.sub(r"m_flFadeInTimeMax = 0\.6", "m_flFadeInTimeMax = 0.15", new)
    new = re.sub(r"m_flFadeInTimeMin = 0\.5", "m_flFadeInTimeMin = 0.1", new)

    new = new.replace(
        '{\n\t\t\t_class = "C_OP_BasicMovement"\n\t\t},',
        '{\n\t\t\t_class = "C_OP_BasicMovement"\n\t\t\tm_fDrag = 0.05\n\t\t\tm_Gravity = [ 0.0, 0.0, 25.0 ]\n\t\t},',
    )
    # The RenderTrails->RenderSprites rewrite above inserts m_bBlendFramesSeq0
    # blindly; drop the insert again wherever the converted block already
    # carried the key, or resourcecompiler fails on the duplicate.
    new = common.dedup_blend_frames_key(new)
    return new


def apply_povarehok_gameplay_composites(root: Path) -> list[str]:
    """Returns the content-relative resources that were written/changed."""
    changed: list[str] = []

    for variant in PVRH_WEAPON_VARIANTS:
        weapon_dir = _pvrh_weapon_dir(variant)
        barrel_smoke = f"{weapon_dir}/weapon_muzzle_smoke_long.vpcf"
        if not common.resource_path(root, barrel_smoke).is_file():
            continue
        removed_smoke_children = set(WRONG_SMOKE_LONG_CHILDREN)
        for wisp_path in iter_muzzle_trail_wisp_vpcfs(root):
            try:
                wisp_path.relative_to(common.resource_path(root, weapon_dir))
            except ValueError:
                continue
            removed_smoke_children.add(wisp_path.name)
            removed_smoke_children.add(_fp_variant_res(wisp_path.name))
        if common.remove_child_refs(common.resource_path(root, barrel_smoke), removed_smoke_children):
            changed.append(barrel_smoke)

    for res, distort in POVAREHOK_DISTORT_CHILDREN.items():
        if common._add_child_once(common.resource_path(root, res), distort):
            changed.append(res)

    return changed


def main() -> int:
    args = parse_args()
    root = args.content_root.resolve()
    source1_root = (args.source1_root or root.parents[2] / "source1_game").resolve()

    if args.gameplay_composites_only:
        # In-place patch entry: also run the idempotent material repairs so an
        # already-converted tree picks them up without a full (non-rerunnable)
        # pipeline pass. Changed textures/particles need a resourcecompiler run.
        wrong_add = common.repair_wrongly_additive_renderers(root, source1_root)
        premul = common.premultiply_white_additive_textures(root)
        # Transparent-pixel colour bleed becomes a visible quad under additive/addself
        # blending once CS2 auto-exposure lifts it above threshold.
        noleak = common.zero_additive_transparent_rgb(root)
        blend = common.add_sheet_frame_blending(root)
        # tune_tracer_brightness=False: the brightness pass is a one-shot tone pass
        # (multiplicative, like tone_down); re-running it in place dims tracers again.
        composites = apply_povarehok_gameplay_composites(root) + modern.apply_modern_gameplay_composites(
            root, tune_tracer_brightness=False, source1_root=source1_root)
        warm = fix_warm_impact_smoke_tints(root)
        loop_files, loop_renderers = fix_looping_smoke_animation(root)
        clamped = common.clamp_sheet_sequences(root, CLAMP_SMOKE_SHEET_RE)
        print(
            f"Repairs: {wrong_add} wrongly-additive files fixed, "
            f"{len(premul)} textures premultiplied, {len(noleak)} de-leaked, "
            f"{blend} files frame-blended, "
            f"{warm} warm impact-smoke files neutralized, "
            f"{loop_files} looping-smoke files / {loop_renderers} renderers set to play once, "
            f"{len(clamped)} smoke sheets clamped (loop -> play once)."
        )
        for res in premul + clamped:
            print(f"  recompile texture: {res}")
        print(f"Gameplay composites: {len(composites)} file(s) written/changed:")
        for res in composites:
            print(f"  {res}")
        return 0

    if args.per_weapon_forks_only:
        # Seeds each fork once, so it is safely rerunnable and never clobbers fork tuning.
        forks = fork_per_weapon_flash(root)
        print(f"Per-weapon forks: {forks} file(s) created.")
        return 0

    if args.casing_direction_only:
        # Only flips ranges that are entirely negative, so it is safely rerunnable.
        casing_changed: list[str] = []
        common.fix_casing_eject_direction(root, casing_changed)
        print(f"Casing eject: {len(casing_changed)} file(s) flipped back to +X.")
        for res in casing_changed:
            print(f"  {res}")
        return 0

    if args.texture_leak_only:
        # Marker-guarded, so it is safely rerunnable; changed textures need a resourcecompiler run.
        leaked = common.zero_additive_transparent_rgb(root)
        print(f"Texture leak: {len(leaked)} additive texture(s) had transparent RGB zeroed.")
        for res in leaked:
            print(f"  recompile texture: {res}")
        return 0

    if args.feathering_only:
        # Follows the Source 1 $depthblend value, so it is safely rerunnable.
        feather_changed: list[str] = []
        common.add_depth_feathering(root, feather_changed, source1_root)
        print(f"Depth feathering: {len(feather_changed)} file(s) reconciled against "
              f"Source 1 $depthblend.")
        return 0

    if args.flash_addself_only:
        # Reads the authored value from the Source 1 VMTs, so it is safely rerunnable.
        restored = restore_muzzle_addself(root, source1_root)
        print(f"Flash blend: {restored} file(s) had m_flAddSelfAmount restored from Source 1.")
        return 0

    if args.lmg_flash_only:
        # Clamping, so safely rerunnable on an already-converted tree.
        aligned = align_lmg_muzzle_flash(root)
        print(f"LMG flash: {aligned} file(s) clamped to "
              f"[{LMG_FLASH_OFFSET_MIN}, {LMG_FLASH_OFFSET_MAX}] along the barrel axis.")
        return 0

    if args.muzzle_smoke_only:
        # Ceiling-based and therefore safely rerunnable on an already-converted tree,
        # unlike the multiplicative tone_down passes in the full pipeline.
        forks = fork_sniper_muzzle_smoke(root)
        smoke = tame_muzzle_smoke_bloom(root)
        print(f"Muzzle smoke: {smoke} file(s) retuned, {forks} sniper fork edit(s) "
              f"({len(MUZZLE_SMOKE_TUNING)} family targets).")
        return 0

    if args.runtime_impact_fixes_only:
        collision = common.force_per_particle_collision(root)
        debris_files, debris_constraints, debris_spin = settle_impact_debris(root)
        smoke_files, smoke_renderers = fix_impact_smoke_blending(root)
        loop_files, loop_renderers = fix_looping_smoke_animation(root)
        clamped = common.clamp_sheet_sequences(root, CLAMP_SMOKE_SHEET_RE)
        print(
            f"Runtime impact fixes: {collision} files forced to per-particle collision, "
            f"{debris_files} debris files / {debris_constraints} constraints / "
            f"{debris_spin} spin operators settled, "
            f"{smoke_files} smoke files / {smoke_renderers} renderers neutralized, "
            f"{loop_files} looping-smoke files / {loop_renderers} renderers set to play once, "
            f"{len(clamped)} smoke sheets clamped (loop -> play once)."
        )
        for res in clamped:
            print(f"  recompile texture: {res}")
        return 0

    changed_muzzle = 0
    changed_molotov = 0

    for variant in VARIANTS:
        muzzle_roots = [
            variant_resource(variant, "weapons/cs_weapon_fx", name)
            for name in MUZZLE_ROOTS
        ]
        changed_muzzle += tone_down_muzzle_closure(
            root, muzzle_roots, alpha=0.45, radius=0.72, overbright=0.35
        )
        # The smoke children that pass deliberately skips still need a size and
        # brightness ceiling, or the per-shot puff grows into a screen-filling wall
        # during a spray (AK/AUG/UMP all share the same oversized smoke systems).

        molotov_roots = [
            variant_resource(variant, "inferno_fx", name)
            for name in MOLOTOV_GROUNDFIRE_ROOTS + MOLOTOV_EXPLOSION_ROOTS
        ]
        for resource in molotov_roots:
            path = common.resource_path(root, resource)
            if path.is_file() and common.remove_child_refs(path, MOLOTOV_REMOVE_CHILDREN):
                changed_molotov += 1
        for path in common.collect_closure(root, molotov_roots):
            # Molotov fire still comes from the real mod, but Source 2's lighting
            # makes the Source 1 overbright values dominate the frame.
            if tone_down(path, alpha=0.75, radius=0.82, overbright=0.55):
                changed_molotov += 1

    # Weapon muzzle smoke needs size/brightness/lifetime ceilings that tone_down_muzzle_closure
    # cannot give it (that pass deliberately skips smoke children so it never crushes them to
    # black). Scans by name across every variant, so it runs once, outside the variant loop.
    # Must run BEFORE tame_muzzle_smoke_bloom so the forked sniper systems exist to tune.
    sniper_forks = fork_sniper_muzzle_smoke(root)
    # Per-weapon flash forks (SCAR-20 sparks, Deagle offsets).
    weapon_forks = fork_per_weapon_flash(root)
    muzzle_smoke = tame_muzzle_smoke_bloom(root)
    # Povarehok LMG flash keeps a Source 1 spread that puts it off the barrel tip.
    lmg_flash = align_lmg_muzzle_flash(root)
    # Must run AFTER tone_down_muzzle_closure so it restores the authored blend the
    # tone-down would otherwise leave crushed (the "black box" around the flash).
    addself = restore_muzzle_addself(root, source1_root)

    dead_children = common.strip_dead_child_refs(root)
    repaired, removed = common.fix_textureless_renderers(root, source1_root)
    dual_seq = common.strip_dual_sequence_keys(root)
    collision = common.force_per_particle_collision(root)
    unlit, additive = common.fix_lit_renderers(root, source1_root)
    wrong_add = common.repair_wrongly_additive_renderers(root, source1_root)
    # Must run AFTER fix_lit_renderers so the final ADD/translucent state is known.
    premul = common.premultiply_white_additive_textures(root)
    # Transparent-pixel colour bleed becomes a visible quad under additive/addself blending
    # once CS2 auto-exposure lifts it above threshold (see zero_additive_transparent_rgb).
    noleak_main = common.zero_additive_transparent_rgb(root)
    blend = common.add_sheet_frame_blending(root)
    debris_files, debris_constraints, debris_spin = settle_impact_debris(root)
    less_debris_files, less_debris_emitters = halve_less_impact_debris(root)
    smoke_files, smoke_renderers = fix_impact_smoke_blending(root)
    vistasmoke_files, vistasmoke_renderers = patch_non_fire_vistasmoke_replacements(root)
    warm_smoke = fix_warm_impact_smoke_tints(root)
    # Must run AFTER patch_non_fire_vistasmoke_replacements (which already sets FIT_LIFETIME on
    # the systems it retargets) so this only touches the remaining looping smoke sheets.
    loop_files, loop_renderers = fix_looping_smoke_animation(root)
    # One-shot smoke sheets: loop -> clamp so frame overshoot holds instead of replaying
    # (the sheets compile after this in the full pipeline, so no separate recompile step).
    clamped = common.clamp_sheet_sequences(root, CLAMP_SMOKE_SHEET_RE)
    # Must run AFTER fix_textureless_renderers: it fully rewrites the heatwave
    # systems whose (Source 1 refract) renderers that pass just emptied.
    composites = apply_povarehok_gameplay_composites(root) + modern.apply_modern_gameplay_composites(root)
    print(
        f"Post-processed Povarehok: {changed_muzzle} muzzle files "
        f"({muzzle_smoke} muzzle-smoke files retuned, {sniper_forks} sniper-smoke forks, "
        f"{weapon_forks} per-weapon flash forks, "
        f"{lmg_flash} LMG flash files aligned, "
        f"{addself} flash files with $addself restored), "
        f"{changed_molotov} molotov files, {dead_children} dead child refs stripped, "
        f"{repaired} textureless renderers repaired / {removed} removed, "
        f"{dual_seq} files de-dual-sequenced, "
        f"{collision} files forced to per-particle collision, "
        f"{unlit} renderers made unlit / {additive} made additive "
        f"({wrong_add} wrongly-additive fixed), "
        f"{len(premul)} additive textures premultiplied, {blend} files frame-blended, "
        f"{debris_files} impact-debris files settled ({debris_constraints} constraints, "
        f"{debris_spin} spin operators), "
        f"{less_debris_files} Less debris files halved ({less_debris_emitters} emitters), "
        f"{smoke_files} impact-smoke files neutralized ({smoke_renderers} renderers), "
        f"{vistasmoke_files} non-fire vistasmoke files retargeted ({vistasmoke_renderers} renderers), "
        f"{warm_smoke} warm impact-smoke tints fixed, "
        f"{loop_files} looping-smoke files set to play once ({loop_renderers} renderers), "
        f"{len(clamped)} smoke sheets clamped, "
        f"{len(composites)} gameplay-composite files."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
