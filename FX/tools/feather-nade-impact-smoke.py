#!/usr/bin/env python3
"""Force depth-feather nade-explosion and impact wall-smoke sprites.

Standalone on purpose: do not fold this into add_depth_feathering. That pass follows Source 1
$depthblend and STRIPS feathering when the VMT is silent -- which is most of the Povarehok HE
and hybrid impact smoke. Blanket-feathering every sprite also painted black boxes on muzzle
flashes; this walk is only HE On/Modern closures plus impact-fx smoke.

Source 2 fades a billboard against the depth buffer via
m_nFeatheringMode = PARTICLE_DEPTH_FEATHERING_ON_OPTIONAL (Valve's default on CS2 explosions)
and m_flFeatheringMaxDist. Valve authors 8.0; these cinematic puffs are 70-170 units, so 8
units still reads as a hard wall cut. Distance scales with radius, 24..48.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import deque
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import postprocess_common as common

MARKER = "// mvm: nade/impact smoke depth feather"
FEATHERING_MODE = common.FEATHERING_MODE
MIN_DIST = 24.0
MAX_DIST = 48.0
RADIUS_SCALE = 0.25

HE_ROOTS = (
    "particles/filmmaker/povarehok/regular/explosions_fx/ac_grenade_explosion.vpcf",
    "particles/filmmaker/modern/arc9_fas_explosions/explosion_grenade.vpcf",
)
IMPACT_DIRS = (
    "particles/filmmaker/insurgency/impact_fx_ins",
    "particles/filmmaker/povarehok/less/impacts/impact_fx",
)

SMOKE_RE = re.compile(
    r"smoke|dust|puff|steam|vapor|thinsmoke|thicksmoke|loopsmoke|vistasmoke|airburst",
    re.I,
)
SKIP_RE = re.compile(
    r"spark|flash|flame|glow|flare|shrapnel|shaprel|debris|fleck|lensflare|core0",
    re.I,
)
RESOURCE_RE = re.compile(r'resource:"([^"]+)"')
MAT_RE = re.compile(r'm_hMaterial = resource:"([^"]+)"')
TEX_RE = re.compile(r'm_hTexture = resource:"([^"]+)"')
RADIUS_RE = re.compile(r"m_flRadiusMax = ([0-9.]+)")
DIST_RE = re.compile(r"(m_flFeatheringMaxDist = )([0-9.]+)")
DISABLE_Z_RE = re.compile(
    r"(?m)^[ \t]*m_bDisableZBuffering = (?:1|true)[ \t]*\r?\n"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--content-root", type=Path, required=True)
    parser.add_argument(
        "--legacy-content-root",
        type=Path,
        help="Old Povarehok content tree (HE On lives here, not in the Insurgency convert).",
    )
    return parser.parse_args()


def smoke_feather_dist(radius: float) -> float:
    if radius <= 0:
        return MIN_DIST
    return max(MIN_DIST, min(MAX_DIST, round(radius * RADIUS_SCALE, 1)))


def is_smoke_renderer(stem: str, block: str) -> bool:
    haystack = " ".join([stem, *MAT_RE.findall(block), *TEX_RE.findall(block)])
    if SKIP_RE.search(haystack):
        return False
    return bool(SMOKE_RE.search(haystack))


def collect_closure(roots_on: list[Path], resource: str) -> list[Path]:
    pending = deque([resource.replace("\\", "/")])
    seen: set[str] = set()
    found: list[Path] = []
    while pending:
        current = pending.popleft()
        if current in seen:
            continue
        seen.add(current)
        path = None
        for base in roots_on:
            candidate = base.joinpath(*current.split("/"))
            if candidate.is_file():
                path = candidate
                break
        if path is None:
            continue
        found.append(path)
        text = path.read_text(encoding="utf-8", errors="replace")
        for child in RESOURCE_RE.findall(text):
            child = child.replace("\\", "/")
            if child.endswith(".vpcf") and child not in seen:
                pending.append(child)
    return found


def target_files(content_roots: list[Path]) -> list[Path]:
    files: list[Path] = []
    seen: set[Path] = set()
    for resource in HE_ROOTS:
        for path in collect_closure(content_roots, resource):
            if path not in seen:
                seen.add(path)
                files.append(path)
    for folder in IMPACT_DIRS:
        for base in content_roots:
            directory = base.joinpath(*folder.split("/"))
            if not directory.is_dir():
                continue
            for path in sorted(directory.rglob("*.vpcf")):
                if path not in seen:
                    seen.add(path)
                    files.append(path)
    return files


def feather_file(path: Path) -> bool:
    text = path.read_text(encoding="utf-8")
    original = text
    radius = 0.0
    found = RADIUS_RE.findall(text)
    if found:
        radius = max(float(value) for value in found)
    dist = smoke_feather_dist(radius)
    edited = False
    for cls in common.FEATHERED_RENDERERS:
        needle = f'_class = "{cls}"'
        pos = text.find(needle)
        while pos != -1:
            start, end = common.block_span(text, pos)
            block = text[start:end]
            if not is_smoke_renderer(path.stem, block):
                pos = text.find(needle, end)
                continue
            stripped = DISABLE_Z_RE.sub("", block)
            if stripped != block:
                block = stripped
                text = text[:start] + block + text[end:]
                end = start + len(block)
                edited = True
            if "m_nFeatheringMode" not in block:
                indent = text[:pos].rsplit("\n", 1)[-1]
                ins = (
                    f'\n{indent}m_nFeatheringMode = "{FEATHERING_MODE}"'
                    f"\n{indent}m_flFeatheringMaxDist = {dist}"
                )
                rel = pos - start + len(needle)
                block = block[:rel] + ins + block[rel:]
                text = text[:start] + block + text[end:]
                end = start + len(block)
                edited = True
            else:
                bumped = DIST_RE.sub(lambda m: f"{m.group(1)}{dist}", block)
                if bumped != block:
                    text = text[:start] + bumped + text[end:]
                    end = start + len(bumped)
                    edited = True
            pos = text.find(needle, end)
    if edited and MARKER not in text:
        text = text.rstrip("\n") + "\n" + MARKER + "\n"
    if text != original:
        path.write_text(text, encoding="utf-8")
        return True
    return False


def main() -> int:
    args = parse_args()
    roots = [args.content_root.resolve()]
    if args.legacy_content_root:
        roots.append(args.legacy_content_root.resolve())
    roots = [path for path in roots if path.is_dir()]
    if not roots:
        raise SystemExit("no content root exists")

    changed = 0
    for path in target_files(roots):
        if feather_file(path):
            changed += 1
            print(path.as_posix())
    print(f"Feathered {changed} nade/impact smoke system(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
