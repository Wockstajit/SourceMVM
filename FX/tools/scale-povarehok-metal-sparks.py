#!/usr/bin/env python3
"""Shrink Povarhok metal-impact sparks by 35% (radius only).

Standalone on purpose: do not fold this into strip-povarehok-3d-debris.py (that pass only
unhooks C_OP_RenderModels chunks) or Insurgency spark speed tuning (travel distance).
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

MARKER = "// mvm: povarehok metal spark size 0.65"
SCALE = 0.65
IMPACT_DIR = Path("particles/filmmaker/povarehok/less/impacts/impact_fx")
SPARK_RE = re.compile(r"(spark|ricochet)", re.I)
RADIUS_RE = re.compile(r"(m_flRadius(?:Min|Max) = )([0-9.]+)")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--content-root", type=Path, required=True)
    return parser.parse_args()


def is_spark_file(path: Path) -> bool:
    return bool(SPARK_RE.search(path.stem))


def scale_radius_value(value: float) -> float:
    scaled = value * SCALE
    text = f"{scaled:.4f}".rstrip("0").rstrip(".")
    return float(text)


def scale_spark_text(text: str) -> str:
    if MARKER in text or not RADIUS_RE.search(text):
        return text

    def replace(match: re.Match[str]) -> str:
        scaled = scale_radius_value(float(match.group(2)))
        return f"{match.group(1)}{scaled}"

    rewritten = RADIUS_RE.sub(replace, text)
    if rewritten == text:
        return text
    return rewritten.rstrip("\n") + "\n" + MARKER + "\n"


def main() -> int:
    args = parse_args()
    directory = args.content_root.resolve() / IMPACT_DIR
    if not directory.is_dir():
        print(f"Povarhok impact directory not found: {directory}")
        return 1
    changed = 0
    for path in sorted(directory.glob("*.vpcf")):
        if not is_spark_file(path):
            continue
        text = path.read_text(encoding="utf-8")
        scaled = scale_spark_text(text)
        if scaled != text:
            path.write_text(scaled, encoding="utf-8")
            changed += 1
            print(path.as_posix())
    print(f"Scaled {changed} Povarhok metal-spark system(s) to {SCALE}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
