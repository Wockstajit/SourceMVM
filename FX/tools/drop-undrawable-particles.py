#!/usr/bin/env python3
"""Remove child particles that CS2 can only ever draw wrong, from any converted pack.

A system with NO renderer (`C_OP_Render*`) AND NO children still emits particles; nothing draws
them correctly, and what reaches the screen is a large engine-default sprite. Source 1's
refract / heat-distortion shockwaves convert into exactly this shape, because csgo_complex.vfx
has no refract equivalent and source1import drops the renderer while keeping the emitter.

Seen in game 2026-09-02 on both packs:
  * `ins_grenade_explosion_shockwave` (radius 225) and `ins_c4_explosion_shockwave` (radius 750).
  * `ac_grenade_explosion_air_fireball_warp` in the Povarehok HE, On and Less.

THE CHILDREN TEST MATTERS. Across the Insurgency pack 37 of 320 systems carry no renderer, but 35
of them are legitimate CONTAINERS whose children do the drawing. Dropping every renderer-less
system would delete 35 working effects; only the childless ones are undrawable.

This removes the whole `{ m_ChildRef = resource:"..." },` entry from every parent in the subtree,
so the child list stays well formed, and leaves the orphaned file itself in place (unreferenced).

USAGE
    python FX/tools/drop-undrawable-particles.py --content-root <content/source_mvm_fx> \
        --subtree particles/filmmaker/povarehok [--dry-run]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

CHILD_ENTRY = r'\n[ \t]*\{\s*m_ChildRef\s*=\s*resource:"%s"\s*,?\s*\}\s*,?'


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--content-root", type=Path, required=True)
    ap.add_argument("--subtree", default="particles/filmmaker")
    # Systems PROVEN undrawable by an in-game test rather than by the structural rule above.
    # `cliquebomba` is the case this exists for: it renders debris with C_OP_RenderModels, so it
    # has a renderer and passes the heuristic, but in CS2 it draws bright red plastic-looking
    # chunks. Its model, its material and all three of that material's textures are present and
    # staged, and the material's own texture is grey-brown (110,107,99) with a grey particle
    # colour init -- so nothing in the asset explains the colour. Disabling just this child took
    # the red from 2.71% of the frame to 0.67%, which is what identified it.
    ap.add_argument("--also-drop", action="append", default=[], metavar="RELPATH",
                    help="content-relative .vpcf to drop as well (repeatable)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    root = args.content_root.resolve()
    tree = root / args.subtree
    if not tree.is_dir():
        print("no such subtree: %s" % tree)
        return 1

    undrawable, containers = [], 0
    for vpcf in sorted(tree.rglob("*.vpcf")):
        text = vpcf.read_text(encoding="utf-8", errors="replace")
        if "C_OP_Render" in text:
            continue
        if "m_ChildRef" in text:
            containers += 1
            continue
        undrawable.append(vpcf.relative_to(root).as_posix())

    for extra in args.also_drop:
        extra = extra.replace("\\", "/")
        if extra not in undrawable:
            undrawable.append(extra)

    print("renderer-less containers kept : %d" % containers)
    print("undrawable (no renderer, no children) + explicit : %d" % len(undrawable))
    for u in undrawable:
        print("   " + u)
    if not undrawable:
        print("\nnothing to drop.")
        return 0

    pattern = re.compile("|".join(CHILD_ENTRY % re.escape(u) for u in undrawable))
    changed = 0
    for vpcf in sorted(tree.rglob("*.vpcf")):
        text = vpcf.read_text(encoding="utf-8", errors="replace")
        pruned = pattern.sub("", text)
        if pruned != text:
            if not args.dry_run:
                vpcf.write_text(pruned, encoding="utf-8")
            changed += 1
            print("   pruned from %s" % vpcf.relative_to(root).as_posix())

    print("\nparents updated%s: %d" % (" (dry run)" if args.dry_run else "", changed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
