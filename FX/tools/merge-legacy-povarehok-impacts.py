#!/usr/bin/env python3
"""Merge the last GitHub build's reduced Povarhok impact closure into the current FX tree."""

from __future__ import annotations

import argparse
import re
import shutil
from collections import deque
from pathlib import Path


POVARHOK_IMPACT_DIR = Path(
    "particles/filmmaker/povarehok/less/impacts/impact_fx"
)
RESOURCE_RE = re.compile(r'resource:"([^"]+)"')


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--legacy-content-root", type=Path, required=True)
    parser.add_argument("--legacy-game-root", type=Path, required=True)
    parser.add_argument("--content-root", type=Path, required=True)
    parser.add_argument("--game-root", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    legacy_content = args.legacy_content_root.resolve()
    legacy_game = args.legacy_game_root.resolve()
    source_dir = legacy_content / POVARHOK_IMPACT_DIR
    if not source_dir.is_dir():
        raise SystemExit(f"legacy Povarhok impact source not found: {source_dir}")

    destination_dir = args.content_root.resolve() / POVARHOK_IMPACT_DIR
    destination_dir.mkdir(parents=True, exist_ok=True)
    sources = sorted(source_dir.glob("*.vpcf"))
    for source in sources:
        shutil.copy2(source, destination_dir / source.name)

    # Follow particle, model and material references in the legacy source tree. Compiled
    # dependencies are copied without replacing current-pack files, so shared CS2 resources
    # keep the current build's version while legacy-only textures/models are restored.
    resources: set[str] = set()
    pending = deque(
        path.relative_to(legacy_content).as_posix() for path in sources
    )
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
    destination_game = args.game_root.resolve()
    for resource in sorted(resources):
        compiled_source = legacy_game.joinpath(*(resource + "_c").split("/"))
        compiled_destination = destination_game.joinpath(*(resource + "_c").split("/"))
        if not compiled_source.is_file() or compiled_destination.is_file():
            continue
        compiled_destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(compiled_source, compiled_destination)
        copied += 1

    print(f"Merged {len(sources)} reduced Povarhok impact VPCFs and {copied} legacy-only compiled dependencies.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
