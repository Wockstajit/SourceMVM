"""Final, idempotent lighting pass for converted weapon-smoke sprites."""
import argparse
from pathlib import Path
from postprocess_common import light_weapon_smoke


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--content-root", type=Path, required=True)
    args = parser.parse_args()
    if not (args.content_root / "particles" / "filmmaker").is_dir():
        parser.error("content-root must contain particles/filmmaker")
    changed = light_weapon_smoke(args.content_root.resolve())
    for resource in changed:
        print(resource)
    print(f"Scene lighting enabled on {len(changed)} weapon-smoke systems.")
