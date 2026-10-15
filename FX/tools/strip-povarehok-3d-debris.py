#!/usr/bin/env python3
"""Remove only Povarhok impact systems that render 3D models.

The legacy Povarhok impact closure mixes ordinary sprites (puffs, sparks and flecks) with a
small `clique*` set of C_OP_RenderModels chunks.  Those chunks are the only systems removed:
parents replace their child reference with CS2's empty particle system. No smoke, sprite, or
spark definition is altered.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path


IMPACT_DIR = Path("particles/filmmaker/povarehok/less/impacts/impact_fx")
EMPTY = "particles/dev/empty.vpcf"
MARKER = "// mvm: Povarhok 3D model debris disabled"
CHILD_RE = re.compile(r'(m_ChildRef\s*=\s*resource:")([^"]+)(")')


def model_debris_resources(directory: Path) -> set[str]:
    return {
        (IMPACT_DIR / path.name).as_posix().lower()
        for path in directory.glob("*.vpcf")
        if 'C_OP_RenderModels' in path.read_text(encoding="utf-8")
    }


def strip_3d_model_children(text: str, resources: set[str]) -> str:
    if MARKER in text:
        return text

    def replace(match: re.Match[str]) -> str:
        target = match.group(2).replace("\\", "/").lower()
        if target in resources:
            return match.group(1) + EMPTY + match.group(3)
        return match.group(0)

    stripped = CHILD_RE.sub(replace, text)
    if stripped == text:
        return text
    return stripped.rstrip("\n") + "\n" + MARKER + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--content-root", type=Path, required=True)
    args = parser.parse_args()
    directory = args.content_root.resolve() / IMPACT_DIR
    if not directory.is_dir():
        raise SystemExit(f"Povarhok impact directory not found: {directory}")

    resources = model_debris_resources(directory)
    changed = 0
    for path in sorted(directory.glob("*.vpcf")):
        text = path.read_text(encoding="utf-8")
        stripped = strip_3d_model_children(text, resources)
        if stripped != text:
            path.write_text(stripped, encoding="utf-8")
            changed += 1
    print(f"Disabled {len(resources)} Povarhok 3D model-debris systems in {changed} parent particle(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
