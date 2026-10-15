#!/usr/bin/env python3
"""Stage the compiled Povarehok HE grenade explosion into the FX pack.

HE On and Less swap to Povarehok's `ac_grenade_explosion` (ParticleFxRules.cpp, kVariantExplosions).
That system is converted and compiled from the legacy Povarehok tree
(build/fx/povarehok-source1import/source2/{content,game}/source_mvm_fx), but the pack the game
mounts is built from the MAIN output (build/fx/fx-source1import/source2/game/source_mvm_fx). Nothing
copied the compiled HE across, so its root was never in the mount, the swap failed open, and HE On
played the vanilla CS2 explosion (found 2026-09-26 with verify-fx-pack.py: "ROOT NOT STAGED").

This copies exactly the HE closure -- the root systems, every child system the compiled files
reference, their materials and those materials' textures -- and nothing else (the legacy material
folder is ~218 MB; the closure is a small fraction of it).

USAGE
    python FX/tools/stage-legacy-he.py --legacy-game-root <...>/game/source_mvm_fx \
        --legacy-content-root <...>/content/source_mvm_fx --game-root <pack game dir> [--game-root <another>]
"""
from __future__ import annotations

import argparse
import importlib.util
import re
import shutil
import sys
from pathlib import Path

ROOTS = [
    "particles/filmmaker/povarehok/regular/explosions_fx/ac_grenade_explosion.vpcf",
    "particles/filmmaker/povarehok/less/smoke/explosions_fx/ac_grenade_explosion.vpcf",
]
VTEX_RE = re.compile(rb'(materials/[A-Za-z0-9_/\-.]+?\.(?:vtex|tga))')  # as verify-fx-pack.py reads them


def load_verifier():
    path = Path(__file__).with_name("verify-fx-pack.py")
    spec = importlib.util.spec_from_file_location("verify_fx_pack", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--legacy-game-root", required=True, type=Path)
    ap.add_argument("--legacy-content-root", required=True, type=Path)
    ap.add_argument("--game-root", required=True, type=Path, action="append")
    args = ap.parse_args()
    verifier = load_verifier()

    files: set[str] = set()
    for root in ROOTS:
        systems, materials = verifier.closure(args.legacy_game_root, root, [args.legacy_content_root])
        if root not in systems:
            print(f"legacy HE root not compiled: {root}", file=sys.stderr)
            return 1
        files.update(s + "_c" for s in systems)
        for mat in materials:
            files.add(mat + "_c")
            for blob_path in (args.legacy_game_root / (mat + "_c"), args.legacy_content_root / mat):
                if blob_path.is_file():
                    for ref in VTEX_RE.findall(blob_path.read_bytes()):
                        files.add(re.sub(r"\.(vtex|tga)$", ".vtex", ref.decode()) + "_c")

    missing = [f for f in sorted(files) if not (args.legacy_game_root / f).is_file()]
    for dest_root in args.game_root:
        copied = 0
        for rel in sorted(files):
            src = args.legacy_game_root / rel
            if not src.is_file():
                continue
            dst = dest_root / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            copied += 1
        print(f"staged legacy Povarehok HE into {dest_root}: {copied} files")
    # Backfill: any texture a staged material references that only the legacy build has (the
    # merged legacy impact / weapon-smoke materials name a few of these too).
    for dest_root in args.game_root:
        filled = 0
        for vmat in dest_root.rglob("*.vmat_c"):
            for ref in VTEX_RE.findall(vmat.read_bytes()):
                rel = re.sub(r"\.(vtex|tga)$", ".vtex", ref.decode()) + "_c"
                src, dst = args.legacy_game_root / rel, dest_root / rel
                if src.is_file() and not dst.is_file():
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(src, dst)
                    filled += 1
        print(f"backfilled {filled} legacy-only textures into {dest_root}")
    if missing:
        print(f"{len(missing)} closure files absent from the legacy build (left to the base game):")
        for m in missing[:20]:
            print("   " + m)
    return 0


if __name__ == "__main__":
    sys.exit(main())
