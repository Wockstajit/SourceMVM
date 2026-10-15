#!/usr/bin/env python3
"""List textures shared between Povarehok molotov and C4 effect closures; export PNG previews."""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "FX" / "tools"))
import postprocess_common as common  # noqa: E402

ROOT = Path(__file__).resolve().parents[2] / (
    "build/fx/fx-source1import/source2/content/source_mvm_fx"
)
OUT = Path(__file__).resolve().parents[1] / "output" / "molotov-c4-shared-textures"
PREFIX = "particles/filmmaker/povarehok/regular/"

MOLOTOV_ROOTS = [
    f"{PREFIX}inferno_fx/molotov_groundfire_00high.vpcf",
    f"{PREFIX}inferno_fx/molotov_fire01.vpcf",
    f"{PREFIX}inferno_fx/molotov_explosion.vpcf",
    f"{PREFIX}inferno_fx/molotov_groundfire_fallback.vpcf",
]
C4_ROOTS = [f"{PREFIX}explosions_fx/explosion_c4_500.vpcf"]

TEX_RE = re.compile(r'm_hTexture = resource:"([^"]+)"')
MAT_RE = re.compile(r'm_hMaterial = resource:"([^"]+)"')


def textures_for(paths: set[Path]) -> tuple[set[str], set[str]]:
    tex: set[str] = set()
    mat: set[str] = set()
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="ignore")
        tex.update(TEX_RE.findall(text))
        mat.update(MAT_RE.findall(text))
    return tex, mat


def resolve_tga(root: Path, vtex_res: str) -> list[Path]:
    return common._vtex_frame_tgas(root, vtex_res)


def export_texture(root: Path, vtex_res: str, out_dir: Path) -> list[Path]:
    from PIL import Image

    frames = resolve_tga(root, vtex_res)
    if not frames:
        return []
    exported: list[Path] = []
    stem = Path(vtex_res).stem
    if len(frames) == 1:
        dst = out_dir / f"{stem}.png"
        Image.open(frames[0]).convert("RGBA").save(dst)
        exported.append(dst)
        return exported
    # Animated sheet: export each frame + a contact sheet if small enough.
    for i, frame in enumerate(frames):
        dst = out_dir / f"{stem}_frame{i:02d}.png"
        Image.open(frame).convert("RGBA").save(dst)
        exported.append(dst)
    if len(frames) <= 16:
        imgs = [Image.open(f).convert("RGBA") for f in frames]
        w, h = imgs[0].size
        sheet = Image.new("RGBA", (w * len(imgs), h))
        for i, im in enumerate(imgs):
            sheet.paste(im, (i * w, 0))
        sheet_path = out_dir / f"{stem}_sheet.png"
        sheet.save(sheet_path)
        exported.append(sheet_path)
    return exported


def main() -> int:
    if not ROOT.is_dir():
        print(f"content root missing: {ROOT}")
        return 1

    mol_paths = common.collect_closure(ROOT, MOLOTOV_ROOTS)
    c4_paths = common.collect_closure(ROOT, C4_ROOTS)
    mol_tex, mol_mat = textures_for(mol_paths)
    c4_tex, c4_mat = textures_for(c4_paths)
    shared_tex = sorted(mol_tex & c4_tex)
    shared_mat = sorted(mol_mat & c4_mat)

    OUT.mkdir(parents=True, exist_ok=True)
    manifest = OUT / "manifest.txt"
    lines = [
        f"molotov particle systems: {len(mol_paths)}",
        f"c4 particle systems: {len(c4_paths)}",
        f"molotov unique textures: {len(mol_tex)}",
        f"c4 unique textures: {len(c4_tex)}",
        f"SHARED TEXTURES ({len(shared_tex)}):",
    ]
    lines.extend(f"  {t}" for t in shared_tex)
    lines.append(f"SHARED MATERIALS ({len(shared_mat)}):")
    lines.extend(f"  {m}" for m in shared_mat)
    manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(manifest.read_text(encoding="utf-8"))

    for vtex_res in shared_tex:
        exported = export_texture(ROOT, vtex_res, OUT)
        print(f"exported {vtex_res}: {[p.name for p in exported]}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
