"""Export Insurgency Sandstorm smoke/dust/muzzle/shell textures for the CS2 FX pack.

Scans the game's Source 1 VPKs, buckets particle-relevant paths, exports VTFs to
``FX/sources/insurgency-sandstorm/``, and writes a browsable catalog under
``Automation/output/insurgency-smoke-catalog/``.

Usage (from repo root):
    python FX/tools/extract-insurgency-smoke.py
    python FX/tools/extract-insurgency-smoke.py --insurgency-root "D:\\...\\insurgency2"
"""

from __future__ import annotations

import os
import argparse
import importlib.util
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOLS = Path(__file__).resolve().parent
# Insurgency 2 (the asset donor), wherever Steam put it on this machine. Only this one tool
# needs it, so it is a plain env override rather than a library-scanning resolver.
DEFAULT_INSURGENCY = os.environ.get("SOURCEMVM_INSURGENCY2_DIR") or (
    r"C:\Program Files (x86)\Steam\steamapps\common\insurgency2"
)
# Source2Viewer-CLI (ValveResourceFormat). Not a build dependency. Resolved as a checkout
# sitting NEXT TO this repository; set SOURCEMVM_VRF_CLI to point elsewhere.
DEFAULT_VRF = os.environ.get("SOURCEMVM_VRF_CLI") or str(
    REPO.parent / "ValveResourceFormat" / "CLI" / "bin" / "Release" / "Source2Viewer-CLI.exe"
)
SOURCE_OUT = REPO / "FX/sources/insurgency-sandstorm"
CATALOG_OUT = REPO / "Automation/output/insurgency-smoke-catalog"

BUCKETS: dict[str, list[str]] = {
    "impact_dust": ["impact", "dust", "debris", "concrete", "dirt", "puff"],
    "muzzle": ["muzzle", "barrel", "flash_smoke", "gunsmoke"],
    "shell_eject": ["shell", "casing", "eject", "brass"],
    "thin_smoke": ["thinsmoke", "smoke", "steam", "vapor", "wisp"],
    "heavy_smoke": ["thick", "grenade", "stack", "burst"],
}

# Prefer these game paths when auto-picking one asset per bucket.
PICK_PRIORITY: dict[str, list[str]] = {
    "impact_dust": [
        "materials/particles/dust_puff.vtf",
        "materials/particles/dust_01.vtf",
        "materials/particles/dust1.vtf",
        "materials/particles/dust2.vtf",
        "materials/particle/particle_debris_02.vtf",
    ],
    "muzzle": [
        "materials/particles/ins_muzzle_smoke.vtf",
        "materials/particles/ins_animsmokethin_01.vtf",
        "materials/particle/muzzleflashcloud.vtf",
        "materials/particle/smoke1/smoke1.vtf",
    ],
    "shell_eject": [
        "materials/particle/shells/particle_shells.vtf",
        "materials/particles/ins_bullet_shell_rifle.vtf",
        "materials/particles/ins_bullet_shell_pistol.vtf",
        "materials/particles/ins_bullet_shell_shotty.vtf",
    ],
    "thin_smoke": [
        "materials/particles/ins_animsmokethin_01.vtf",
        "materials/particles/ins_thin_smoke.vtf",
        "materials/particles/ins_animsmokethin_02.vtf",
        "materials/particles/loopsmoke_thin.vtf",
    ],
}

PARTICLE_PATH_HINT = re.compile(r"materials/(?:particle|particles|effects)/", re.IGNORECASE)
SKIP_PATH = re.compile(
    r"(overlay/|maps/|models/weapons/shells/w_|editor/|laser|scroll|lightray|window)",
    re.IGNORECASE,
)


def load_vpk_reader():
    gencat_path = REPO / "Automation/tools/generate_cosmetics_catalog.py"
    spec = importlib.util.spec_from_file_location("gencat", gencat_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot load VPK reader from {gencat_path}")
    gencat = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gencat)
    return gencat.Vpk


def discover_vpk_dirs(insurgency_root: Path) -> list[Path]:
    candidates: list[Path] = []
    for sub in ("insurgency", "game/insurgency", "game"):
        base = insurgency_root / sub
        if not base.is_dir():
            continue
        for path in sorted(base.glob("*_dir.vpk")):
            candidates.append(path)
    if not candidates:
        for path in sorted(insurgency_root.rglob("*_dir.vpk")):
            candidates.append(path)
    # De-dupe while preserving order.
    seen: set[str] = set()
    out: list[Path] = []
    for path in candidates:
        key = str(path.resolve()).lower()
        if key not in seen:
            seen.add(key)
            out.append(path)
    return out


def classify_bucket(game_path: str) -> str | None:
    lower = game_path.lower()
    if not PARTICLE_PATH_HINT.search(lower):
        return None
    if SKIP_PATH.search(lower):
        return None
    if not lower.endswith(".vtf"):
        return None
    scores: dict[str, int] = {}
    stem = Path(lower).stem
    for bucket, keywords in BUCKETS.items():
        score = sum(1 for kw in keywords if kw in lower or kw in stem)
        if score:
            scores[bucket] = score
    if not scores:
        return None
    return max(scores, key=scores.get)


def load_export_helpers():
    spec = importlib.util.spec_from_file_location(
        "export_source1_vtf", TOOLS / "export-source1-vtf.py"
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("Cannot load export-source1-vtf.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def vtf_dimensions(vtf_bytes: bytes) -> tuple[int, int, int]:
    if vtf_bytes[:4] != b"VTF\0" or len(vtf_bytes) < 20:
        return 0, 0, 0
    width, height = struct.unpack_from("<HH", vtf_bytes, 16)
    frames = struct.unpack_from("<I", vtf_bytes, 24)[0] if len(vtf_bytes) >= 28 else 1
    return width, height, max(1, frames)


def write_catalog_png(vtf_path: Path, dest_png: Path) -> bool:
    try:
        from srctools.vtf import VTF, VTFFlags

        with vtf_path.open("rb") as stream:
            texture = VTF.read(stream)
            if VTFFlags.ENVMAP in texture.flags:
                return False
            texture.load()
        atlas = texture.get(frame=0).to_PIL()
        dest_png.parent.mkdir(parents=True, exist_ok=True)
        atlas.save(dest_png, format="PNG")
        return True
    except Exception:
        try:
            from vtf2img import Parser

            atlas = Parser(str(vtf_path)).get_image()
            dest_png.parent.mkdir(parents=True, exist_ok=True)
            atlas.save(dest_png, format="PNG")
            return True
        except Exception:
            return False


def try_vrf_decompile(vrf_cli: Path, compiled_path: Path, out_dir: Path) -> Path | None:
    if not vrf_cli.is_file():
        return None
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [str(vrf_cli), "-i", str(compiled_path), "-o", str(out_dir), "-d"]
    try:
        subprocess.run(cmd, check=True, capture_output=True, text=True)
    except (subprocess.CalledProcessError, OSError):
        return None
    for ext in (".png", ".tga"):
        for hit in out_dir.rglob(f"*{ext}"):
            return hit
    return None


def pick_for_bucket(bucket: str, entries: list[dict]) -> dict | None:
    by_path = {e["gamePath"].lower(): e for e in entries}
    for preferred in PICK_PRIORITY.get(bucket, []):
        hit = by_path.get(preferred.lower())
        if hit and not hit.get("error"):
            return hit
    for entry in entries:
        if not entry.get("error"):
            return entry
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--insurgency-root", default=DEFAULT_INSURGENCY)
    parser.add_argument("--vrf-cli", default=DEFAULT_VRF)
    parser.add_argument("--source-out", type=Path, default=SOURCE_OUT)
    parser.add_argument("--catalog-out", type=Path, default=CATALOG_OUT)
    parser.add_argument("--max-per-bucket", type=int, default=24)
    args = parser.parse_args()

    insurgency_root = Path(args.insurgency_root)
    if not insurgency_root.is_dir():
        print(f"Insurgency root not found: {insurgency_root}", file=sys.stderr)
        return 1

    vpk_dirs = discover_vpk_dirs(insurgency_root)
    if not vpk_dirs:
        print(f"No *_dir.vpk found under {insurgency_root}", file=sys.stderr)
        return 1

    print("Discovered VPK dir files:")
    for path in vpk_dirs:
        print(f"  {path}")

    Vpk = load_vpk_reader()
    export_mod = load_export_helpers()

    indexed: dict[str, tuple[Path, str]] = {}
    for dir_vpk in vpk_dirs:
        try:
            vpk = Vpk(dir_vpk)
        except Exception as error:
            print(f"  skip {dir_vpk.name}: {error}", file=sys.stderr)
            continue
        for game_path in vpk.entries:
            bucket = classify_bucket(game_path)
            if bucket is None:
                continue
            indexed.setdefault(game_path.lower(), (dir_vpk, game_path))

    if not indexed:
        print("No particle smoke paths matched bucket keywords.", file=sys.stderr)
        return 1

    args.source_out.mkdir(parents=True, exist_ok=True)
    args.catalog_out.mkdir(parents=True, exist_ok=True)
    for bucket in BUCKETS:
        (args.catalog_out / bucket).mkdir(parents=True, exist_ok=True)

    manifest: dict = {
        "generatedAt": datetime.now(timezone.utc).isoformat(),
        "insurgencyRoot": str(insurgency_root),
        "vpkDirs": [str(p) for p in vpk_dirs],
        "buckets": {bucket: [] for bucket in BUCKETS},
        "picks": {},
    }

    open_vpks: dict[Path, object] = {}
    bucket_counts: dict[str, int] = {bucket: 0 for bucket in BUCKETS}

    with tempfile.TemporaryDirectory(prefix="ins-smoke-") as tmp:
        tmp_root = Path(tmp)
        for _key, (dir_vpk, game_path) in sorted(indexed.items()):
            bucket = classify_bucket(game_path)
            if bucket is None:
                continue
            if bucket_counts[bucket] >= args.max_per_bucket:
                continue

            if dir_vpk not in open_vpks:
                open_vpks[dir_vpk] = Vpk(dir_vpk)
            vpk = open_vpks[dir_vpk]

            rel_stem = Path(game_path).with_suffix("").as_posix()
            local_materials = args.source_out / "materials" / Path(game_path).relative_to("materials").parent
            local_materials.mkdir(parents=True, exist_ok=True)
            vtf_out = args.source_out / rel_stem

            entry: dict = {
                "bucket": bucket,
                "gamePath": game_path,
                "localStem": rel_stem,
                "vpk": str(dir_vpk),
            }

            try:
                raw = vpk.read(game_path)
                width, height, frames = vtf_dimensions(raw)
                entry["width"] = width
                entry["height"] = height
                entry["frames"] = frames

                extract_vtf = tmp_root / f"{bucket_counts[bucket]:03d}.vtf"
                extract_vtf.write_bytes(raw)
                frames_exported, decoder, sheeted = export_mod.export_texture(
                    extract_vtf, vtf_out, args.source_out
                )
                entry["decoder"] = decoder
                entry["exportedFrames"] = frames_exported
                entry["sheeted"] = sheeted

                vmt_path = game_path[:-4] + ".vmt"
                if vmt_path.lower() in vpk.entries:
                    vmt_bytes = vpk.read(vmt_path)
                    (args.source_out / Path(vmt_path)).parent.mkdir(parents=True, exist_ok=True)
                    (args.source_out / Path(vmt_path)).write_bytes(vmt_bytes)
                    entry["vmt"] = vmt_path

                png_name = Path(game_path).stem + ".png"
                catalog_png = args.catalog_out / bucket / png_name
                if write_catalog_png(extract_vtf, catalog_png):
                    entry["catalogPng"] = str(catalog_png.relative_to(REPO))

            except Exception as error:
                entry["error"] = repr(error)

            manifest["buckets"][bucket].append(entry)
            bucket_counts[bucket] += 1

    for bucket, entries in manifest["buckets"].items():
        pick = pick_for_bucket(bucket, entries)
        if pick:
            stem = pick["localStem"]
            manifest["picks"][bucket] = {
                "gamePath": pick["gamePath"],
                "contentResource": f"materials/particle/insurgency/{Path(stem).name}",
                "localStem": stem,
            }

    picks_path = args.source_out / "picks.json"
    picks_path.write_text(json.dumps(manifest["picks"], indent=2), encoding="utf-8")
    manifest_path = args.catalog_out / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    index_lines = [
        "Insurgency smoke catalog",
        f"Source tree: {args.source_out.relative_to(REPO)}",
        "",
    ]
    for bucket in BUCKETS:
        pick = manifest["picks"].get(bucket)
        index_lines.append(f"## {bucket} ({len(manifest['buckets'][bucket])} hits)")
        if pick:
            index_lines.append(f"  pick: {pick['gamePath']} -> {pick['contentResource']}")
        index_lines.append("")
    (args.catalog_out / "INDEX.txt").write_text("\n".join(index_lines), encoding="utf-8")

    total = sum(len(v) for v in manifest["buckets"].values())
    print(f"Exported {total} textures to {args.source_out.relative_to(REPO)}")
    print(f"Catalog: {args.catalog_out.relative_to(REPO)}")
    print(f"Picks: {picks_path.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
