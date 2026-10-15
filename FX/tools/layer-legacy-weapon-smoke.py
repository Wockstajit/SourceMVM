#!/usr/bin/env python3
"""Layer the old GitHub On-mode Povarhok weapon smoke onto current Insurgency flashes.

The Insurgency On-mode flashes keep their own smoke. This copies only smoke-named children
from the last GitHub Povarhok pack (no flashes, sparks, flames, or shell-eject smoke) and
adds them as extra children so both clouds play together.

Standalone on purpose: do not fold this into merge-legacy-povarehok-impacts.py.
"""

from __future__ import annotations

import argparse
import re
import shutil
from collections import deque
from pathlib import Path


LEGACY_WEAPON_DIR = Path(
    "particles/filmmaker/povarehok/regular/weapons/cs_weapon_fx"
)
RESOURCE_RE = re.compile(r'resource:"([^"]+)"')
SMOKE_NAME_RE = re.compile(r"smoke", re.I)
SKIP_SMOKE_RE = re.compile(r"shell_eject", re.I)
WRAPPER_NAME_RE = re.compile(r"^ac_muzzle_", re.I)
FLASH_SKIP_RE = re.compile(r"(spark|flame|glow|center|trace|flash|init)", re.I)

CHILD_ELEMENT = '\t\t{{\n\t\t\tm_ChildRef = resource:"{res}"\n\t\t}},\n'
FALLBACK_RE = re.compile(r'm_hFallback = resource:"([^"]+)"')
DRIFT_MARKER = "// mvm: on-mode muzzle smoke drift"

# ---- the layer is a LAYER: Insurgency is the base, this sits subtly on top --------------------
# User spec 2026-09-03: "Keep the Insurgency effects as the main/base effect. Layer the Povarhok
# weapon smoke on top. Reduce the Povarhok smoke opacity/intensity to around 20% so it is subtle
# and does not overpower the base effects."
#
# MEASURED over the AKM first-person closure, as sum(emit x mean alpha) per shot:
#     Insurgency's own muzzle smoke   510      (80% of the cloud)
#     this Povarehok layer            125      (20% of the cloud)
# so with Insurgency restored to its authored strength the layer is ALREADY ~20% of the total.
# The layer originally shipped at 0.20. Raised to 0.30 by user request so the Povarhok smoke is
# easier to read while Insurgency remains the full-strength base. ONE constant controls the whole
# layered closure; particle counts, radius, lifetime and drift remain unchanged.
#
# Alpha only. Counts, radius, lifetime, drift and the every-shot emit fix below are untouched,
# so the layer keeps its shape and only loses opacity. Marker-guarded with the factor recorded,
# so re-tuning divides the previous one out instead of compounding.
POVAREHOK_SMOKE_ALPHA_SCALE = 0.30
SMOKE_ALPHA_RE = re.compile(r"(m_nAlpha(?:Min|Max)\s*=\s*)([0-9]+)")
ALPHA_MARKER_PREFIX = "// mvm: layered muzzle smoke opacity scaled by "
ALPHA_MARKER_RE = re.compile(re.escape(ALPHA_MARKER_PREFIX) + r"([0-9.]+)")
SCREEN_FADE_RE = re.compile(r"(?m)^\t+m_fl(?:Start|End)FadeSize = [^\n]+\n")
EMIT_MIN_ZERO_RE = re.compile(
    r'(_class = "C_OP_InstantaneousEmitter"[\s\S]*?m_flRandomMin = )0(\b)'
)
FADE_OUT_ZERO_RE = re.compile(
    r"(_class = \"C_OP_FadeAndKill\"[\s\S]*?m_flStartFadeOutTime = )0(?:\.0)?(\b)"
)
RANDOM_FORCE_BLOCK = (
    "\t\t{\n"
    '\t\t\t_class = "C_OP_RandomForce"\n'
    "\t\t\tm_MinForce = [12.0, 8.0, 6.0]\n"
    "\t\t\tm_MaxForce = [-12.0, -8.0, 18.0]\n"
    "\t\t},\n"
)

# Old On-mode flash roots (regular Povarhok) whose smoke children get lifted onto the
# current Insurgency On-mode flash roots. First-person files are preferred; the world
# pistol is included because pistol_fp itself shipped no smoke children.
CLASS_FLASHES: dict[str, tuple[str, ...]] = {
    "pistol": (
        "weapon_muzzle_flash_pistol_fp.vpcf",
        "weapon_muzzle_flash_pistol.vpcf",
    ),
    "deagle": (
        "weapon_muzzle_flash_pistol_fp_deagle.vpcf",
        "weapon_muzzle_flash_pistol_deagle.vpcf",
    ),
    "rifle": (
        "weapon_muzzle_flash_assaultrifle_fp.vpcf",
        "weapon_muzzle_flash_assaultrifle.vpcf",
    ),
    "smg": (
        "weapon_muzzle_flash_smg_fp.vpcf",
        "weapon_muzzle_flash_smg.vpcf",
    ),
    "shotgun": (
        "weapon_muzzle_flash_shotgun_fp.vpcf",
        "weapon_muzzle_flash_shotgun.vpcf",
    ),
    # huntingrifle_fp is the first-person On-mode sniper. weapon_muzzle_flash_awp is the
    # kitchen-sink 3rd-person file and is deliberately not used.
    "sniper": (
        "weapon_muzzle_flash_huntingrifle_fp.vpcf",
    ),
    "lmg": (
        "weapon_muzzle_flash_para_fp.vpcf",
        "weapon_muzzle_flash_para.vpcf",
    ),
    "suppressed": (
        "weapon_muzzle_flash_smg_silenced_fp.vpcf",
        "weapon_muzzle_flash_smg_silenced.vpcf",
    ),
}

INSURGENCY_TARGETS: dict[str, tuple[str, ...]] = {
    "pistol": (
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_makarov_1p.vpcf",
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_makarov_3rd.vpcf",
    ),
    "deagle": (
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_m9_1p.vpcf",
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_m9_3rd.vpcf",
    ),
    "rifle": (
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_akm_1p.vpcf",
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_akm_3rd.vpcf",
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_m16_1p.vpcf",
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_m16_3rd.vpcf",
        "particles/filmmaker/insurgency/weapon_fx_ins_b/muzzleflash_ak74_1p.vpcf",
        "particles/filmmaker/insurgency/weapon_fx_ins_b/muzzleflash_ak74_3rd.vpcf",
    ),
    "smg": (
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_mp5_1p.vpcf",
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_mp5_3rd.vpcf",
    ),
    "shotgun": (
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_m590_1p.vpcf",
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_m590_3rd.vpcf",
    ),
    "sniper": (
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_m14_1p.vpcf",
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_m14_3rd.vpcf",
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_fal_1p.vpcf",
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_fal_3rd.vpcf",
    ),
    "lmg": (
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_m249_1p.vpcf",
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_m249_3rd.vpcf",
    ),
    "suppressed": (
        "particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_suppressed_1p.vpcf",
    ),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--legacy-content-root", type=Path)
    parser.add_argument("--legacy-game-root", type=Path)
    parser.add_argument("--content-root", type=Path, required=True)
    parser.add_argument("--game-root", type=Path)
    # Re-apply ONLY the opacity dial over the layer already present in --content-root. Needs no
    # legacy trees, so a taste change costs one run instead of a re-layer.
    parser.add_argument("--dim-only", action="store_true")
    args = parser.parse_args()
    if not args.dim_only and not (args.legacy_content_root and args.legacy_game_root
                                  and args.game_root):
        parser.error("--legacy-content-root, --legacy-game-root and --game-root are required "
                     "unless --dim-only is given")
    return args


def is_smoke_leaf(resource: str) -> bool:
    stem = Path(resource.replace("\\", "/")).stem
    return bool(SMOKE_NAME_RE.search(stem)) and not SKIP_SMOKE_RE.search(stem)


def is_smoke_wrapper(resource: str) -> bool:
    stem = Path(resource.replace("\\", "/")).stem
    if is_smoke_leaf(resource):
        return False
    if not WRAPPER_NAME_RE.search(stem):
        return False
    return not FLASH_SKIP_RE.search(stem)


def child_resources(text: str) -> list[str]:
    return [value.replace("\\", "/") for value in RESOURCE_RE.findall(text)
            if value.replace("\\", "/").endswith(".vpcf")]


def collect_smoke_resources(legacy_content: Path, flash_names: tuple[str, ...]) -> list[str]:
    found: list[str] = []
    seen: set[str] = set()
    pending: deque[str] = deque()
    for name in flash_names:
        pending.append((LEGACY_WEAPON_DIR / name).as_posix())
    while pending:
        resource = pending.popleft()
        if resource in seen:
            continue
        seen.add(resource)
        source = legacy_content.joinpath(*resource.split("/"))
        if not source.is_file():
            continue
        text = source.read_text(encoding="utf-8", errors="replace")
        for child in child_resources(text):
            if is_smoke_leaf(child):
                if child not in found:
                    found.append(child)
            elif is_smoke_wrapper(child):
                pending.append(child)
    return found


def add_child_once(path: Path, resource: str) -> bool:
    if not path.is_file():
        return False
    text = path.read_text(encoding="utf-8")
    if resource in text:
        return False
    element = CHILD_ELEMENT.format(res=resource)
    match = re.search(r"m_Children = \n(\t*)\[\n", text)
    if match:
        text = text[: match.end()] + element + text[match.end():]
    else:
        anchor = re.search(r'_class = "CParticleSystemDefinition"\n', text)
        if not anchor:
            return False
        block = "\tm_Children = \n\t[\n" + element + "\t]\n"
        text = text[: anchor.end()] + block + text[anchor.end():]
    path.write_text(text, encoding="utf-8")
    return True


def attach_smoke_to_fallback(content_root: Path, flash_path: Path, smokes: list[str]) -> int:
    """Keep Povarhok smoke on cheap _b fallbacks so sprays still emit it every shot."""
    added = 0
    seen: set[str] = set()
    current = flash_path
    while current.is_file():
        key = str(current.resolve())
        if key in seen:
            break
        seen.add(key)
        fallback = FALLBACK_RE.search(current.read_text(encoding="utf-8"))
        if not fallback:
            break
        fallback_path = content_root.joinpath(*fallback.group(1).replace("\\", "/").split("/"))
        if not fallback_path.is_file():
            break
        for smoke in smokes:
            if add_child_once(fallback_path, smoke):
                added += 1
        current = fallback_path
    return added


def ensure_random_force(text: str) -> str:
    if "C_OP_RandomForce" in text or "C_OP_RenderSprites" not in text:
        return text
    match = re.search(r"m_ForceGenerators = \n\t\[\n", text)
    if match:
        return text[: match.end()] + RANDOM_FORCE_BLOCK + text[match.end():]
    insert = "\tm_ForceGenerators = \n\t[\n" + RANDOM_FORCE_BLOCK + "\t]\n"
    anchor = re.search(r"\n\tm_bPreventnamebasedlookup", text)
    if anchor:
        return text[: anchor.start()] + "\n" + insert + text[anchor.start():]
    stripped = text.rstrip()
    if stripped.endswith("}"):
        return stripped[:-1] + insert + "}\n"
    return text + insert


def ensure_drift_gravity(text: str) -> str:
    if "C_OP_BasicMovement" not in text:
        return text
    if re.search(r"m_Gravity = \[0\.0, 0\.0, 0\.0\]", text):
        return re.sub(
            r"m_Gravity = \[0\.0, 0\.0, 0\.0\]",
            "m_Gravity = [6.0, 4.0, 12.0]",
            text,
            count=1,
        )
    if "m_Gravity =" in text:
        return text
    return re.sub(
        r'(_class = "C_OP_BasicMovement"\n)',
        r"\1\t\t\tm_Gravity = [6.0, 4.0, 12.0]\n",
        text,
        count=1,
    )


def dim_layered_smoke(text: str) -> str:
    """Scale this layer's alpha to POVAREHOK_SMOKE_ALPHA_SCALE.

    Re-tunable: the marker records the factor the file actually got, so changing the constant
    divides the previous one out instead of compounding.
    """
    previous = ALPHA_MARKER_RE.search(text)
    applied = float(previous.group(1)) if previous else 1.0
    if abs(applied - POVAREHOK_SMOKE_ALPHA_SCALE) < 1e-9 or not SMOKE_ALPHA_RE.search(text):
        return text
    factor = POVAREHOK_SMOKE_ALPHA_SCALE / applied
    out = ALPHA_MARKER_RE.sub("", text).rstrip("\n") + "\n"
    # Never round to zero: an alpha of 0 is a system that draws nothing at all, which is a
    # different outcome from "subtle" and is how the layer would read as broken.
    out = SMOKE_ALPHA_RE.sub(
        lambda m: m.group(1) + str(max(1, min(255, round(int(m.group(2)) * factor)))), out)
    return out.rstrip("\n") + "\n" + ALPHA_MARKER_PREFIX + repr(POVAREHOK_SMOKE_ALPHA_SCALE) + "\n"


def tune_smoke_text(text: str) -> str:
    """Every-shot emit, world linger, and air drift without lowering alpha."""
    if DRIFT_MARKER in text:
        return text
    original = text
    text = SCREEN_FADE_RE.sub("", text)
    text = EMIT_MIN_ZERO_RE.sub(r"\g<1>1\2", text, count=1)
    text = FADE_OUT_ZERO_RE.sub(r"\g<1>0.45\2", text, count=1)
    text = re.sub(r"(m_fDrag = )0\.1\b", r"\g<1>0.05", text, count=1)
    text = ensure_drift_gravity(text)
    text = ensure_random_force(text)
    if text != original:
        text = text.rstrip("\n") + "\n" + DRIFT_MARKER + "\n"
    return text


def tune_layered_smoke(content_root: Path, smokes: list[str]) -> int:
    tuned = 0
    seen: set[str] = set()
    pending = deque(smokes)
    while pending:
        resource = pending.popleft()
        if resource in seen:
            continue
        seen.add(resource)
        path = content_root.joinpath(*resource.split("/"))
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        for child in child_resources(text):
            if child.startswith("particles/filmmaker/povarehok/") and child not in seen:
                pending.append(child)
        if not is_smoke_leaf(resource):
            continue
        new_text = dim_layered_smoke(tune_smoke_text(text))
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
            tuned += 1
    return tuned


def copy_compiled_closure(
    legacy_content: Path, legacy_game: Path, content_root: Path, game_root: Path,
    roots: list[str],
) -> int:
    resources: set[str] = set()
    pending = deque(roots)
    while pending:
        resource = pending.popleft()
        if resource in resources:
            continue
        resources.add(resource)
        source = legacy_content.joinpath(*resource.split("/"))
        if not source.is_file() or source.suffix.lower() not in {".vpcf", ".vmat", ".vmdl"}:
            continue
        for child in RESOURCE_RE.findall(source.read_text(encoding="utf-8", errors="replace")):
            normalized = child.replace("\\", "/")
            if normalized not in resources:
                pending.append(normalized)

    copied = 0
    for resource in sorted(resources):
        content_source = legacy_content.joinpath(*resource.split("/"))
        content_destination = content_root.joinpath(*resource.split("/"))
        if content_source.is_file() and not content_destination.is_file():
            content_destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(content_source, content_destination)
        # A .vtex is recompiled from its inputs, which sit beside it under its own stem: the
        # image (.tga, its _a_trans alpha), and for a sprite sheet the .mks and every
        # _seq<N>_<M>.tga frame. Without them the texture compile fails ("Missing file ...mks").
        if content_source.is_file() and content_destination.is_file():
            if content_source.suffix.lower() == ".vtex":
                stem = content_source.stem
                for sibling in content_source.parent.iterdir():
                    name = sibling.name
                    if not sibling.is_file() or not name.startswith(stem):
                        continue
                    rest = name[len(stem):]
                    if not (rest in (".tga", ".mks", "_a_trans.tga") or rest.startswith("_seq")):
                        continue
                    target = content_destination.parent / name
                    if not target.is_file():
                        shutil.copy2(sibling, target)
        compiled_source = legacy_game.joinpath(*(resource + "_c").split("/"))
        compiled_destination = game_root.joinpath(*(resource + "_c").split("/"))
        if not compiled_source.is_file() or compiled_destination.is_file():
            continue
        compiled_destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(compiled_source, compiled_destination)
        copied += 1
    return copied


def dim_only(content_root: Path) -> int:
    """Re-apply the opacity dial to every layered smoke leaf already in the tree."""
    directory = content_root / LEGACY_WEAPON_DIR
    if not directory.is_dir():
        raise SystemExit("layered weapon smoke not found: %s" % directory)
    dimmed = 0
    for path in sorted(directory.glob("*.vpcf")):
        if not is_smoke_leaf(path.as_posix()):
            continue
        text = path.read_text(encoding="utf-8")
        out = dim_layered_smoke(text)
        if out == text:
            continue
        path.write_text(out, encoding="utf-8")
        dimmed += 1
        print("  %s" % path.relative_to(content_root).as_posix())
    print("Layered muzzle smoke: %d system(s) set to %s opacity."
          % (dimmed, POVAREHOK_SMOKE_ALPHA_SCALE))
    return 0


def main() -> int:
    args = parse_args()
    if args.dim_only:
        return dim_only(args.content_root.resolve())
    legacy_content = args.legacy_content_root.resolve()
    legacy_game = args.legacy_game_root.resolve()
    content_root = args.content_root.resolve()
    game_root = args.game_root.resolve()
    source_dir = legacy_content / LEGACY_WEAPON_DIR
    if not source_dir.is_dir():
        raise SystemExit(f"legacy Povarhok weapon FX source not found: {source_dir}")

    all_smoke: list[str] = []
    attached = 0
    for class_name, flashes in CLASS_FLASHES.items():
        smokes = collect_smoke_resources(legacy_content, flashes)
        for smoke in smokes:
            if smoke not in all_smoke:
                all_smoke.append(smoke)
        for target in INSURGENCY_TARGETS[class_name]:
            path = content_root.joinpath(*target.split("/"))
            if not path.is_file():
                continue
            for smoke in smokes:
                if add_child_once(path, smoke):
                    attached += 1
            attached += attach_smoke_to_fallback(content_root, path, smokes)

    copied = copy_compiled_closure(
        legacy_content, legacy_game, content_root, game_root, all_smoke)
    drifted = tune_layered_smoke(content_root, all_smoke)

    print(
        f"Layered {len(all_smoke)} old On-mode weapon-smoke systems onto Insurgency flashes "
        f"({attached} child refs added, {copied} legacy-only compiled dependencies, "
        f"{drifted} smoke systems given air drift)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
