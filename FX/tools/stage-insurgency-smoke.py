"""Stage Insurgency-exported smoke sources into the CS2 FX content tree.

Reads ``FX/sources/insurgency-sandstorm/picks.json`` (from extract-insurgency-smoke.py)
and writes ``materials/particle/insurgency/*`` with TGAs, VTEX, and VMATs ready for
resourcecompiler.

Usage:
    python FX/tools/stage-insurgency-smoke.py --content-root build/fx/.../source2/content/source_mvm_fx
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOLS = Path(__file__).resolve().parent
DEFAULT_SOURCE = REPO / "FX/sources/insurgency-sandstorm"
DEFAULT_PICKS = DEFAULT_SOURCE / "picks.json"

VMAT_TEMPLATE = '''"Layer0"
{{
\tshader\t"csgo_complex.vfx"
\tF_UNLIT\t"1"
\tTextureColor\t"{color_tga}"
\tF_VERTEX_COLOR\t"1"
\tF_TRANSLUCENT\t"1"
\tTextureRoughness\t"materials/default/default_rough_s1import.tga"
}}
'''


def load_vtex_template():
    spec = importlib.util.spec_from_file_location(
        "export_source1_vtf", TOOLS / "export-source1-vtf.py"
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("Cannot load export-source1-vtf.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.VTEX_MKS_TEMPLATE


def stage_pick(source_root: Path, content_root: Path, bucket: str, pick: dict) -> str | None:
    local_stem = pick.get("localStem")
    if not local_stem:
        return None
    src_base = source_root / local_stem
    if not any(src_base.parent.glob(src_base.name + "*")) and not src_base.with_suffix(".tga").is_file():
        return None

    name = Path(local_stem).name
    dest_dir = content_root / "materials/particle/insurgency"
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest_stem = dest_dir / name

    # Copy TGAs / MKS from the exported tree.
    src_dir = source_root / Path(local_stem).parent
    copied = False
    for src in sorted(src_dir.glob(f"{name}*")):
        if src.suffix.lower() in {".tga", ".mks"}:
            shutil.copy2(src, dest_dir / src.name)
            copied = True

    mks = dest_dir / f"{name}.mks"
    tga = dest_dir / f"{name}.tga"

    vtex_path = dest_stem.with_suffix(".vtex")
    vtex_template = load_vtex_template()
    if mks.is_file():
        mks_resource = mks.relative_to(content_root).as_posix()
        vtex_path.write_text(vtex_template.replace("<>", mks_resource, 1), encoding="ascii")
    elif tga.is_file():
        tga_resource = tga.relative_to(content_root).as_posix()
        vtex_path.write_text(vtex_template.replace("<>", tga_resource, 1), encoding="ascii")
    else:
        return None

    color_tga = tga.relative_to(content_root).as_posix() if tga.is_file() else ""
    if not color_tga:
        # Multi-frame exports use name000.tga etc.; reference the first frame.
        frames = sorted(dest_dir.glob(f"{name}*.tga"))
        if frames:
            color_tga = frames[0].relative_to(content_root).as_posix()
    if not color_tga:
        return None

    vmat_path = dest_stem.with_suffix(".vmat")
    vmat_path.write_text(VMAT_TEMPLATE.format(color_tga=color_tga), encoding="utf-8")

    resource = f"materials/particle/insurgency/{name}"
    print(f"  staged {bucket}: {resource}")
    return resource


def load_picks(path: Path) -> dict:
    if not path.is_file():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--content-root", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--picks", type=Path, default=DEFAULT_PICKS)
    args = parser.parse_args()

    content_root = args.content_root.resolve()
    if not content_root.is_dir():
        print(f"Content root not found: {content_root}", file=sys.stderr)
        return 1

    picks = load_picks(args.picks)
    if not picks:
        print(f"No picks at {args.picks}; run extract-insurgency-smoke.py first.", file=sys.stderr)
        return 1

    if not args.source_root.is_dir():
        print(f"Source tree not found: {args.source_root}", file=sys.stderr)
        return 1

    staged: dict[str, str] = {}
    for bucket, pick in picks.items():
        resource = stage_pick(args.source_root, content_root, bucket, pick)
        if resource:
            staged[bucket] = resource

    report = content_root / "materials/particle/insurgency/staged.json"
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(staged, indent=2), encoding="utf-8")
    print(f"Staged {len(staged)} Insurgency material(s).")
    return 0 if staged else 1


if __name__ == "__main__":
    raise SystemExit(main())
