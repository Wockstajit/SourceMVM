#!/usr/bin/env python3
"""Check that every effect the rules can swap to is complete in the MOUNTED pack.

Scaling correctness is verify-fx-scaling.py's job. This is the other half: does each target, and
everything it pulls in, actually exist where the game will look for it -- and is it free of the
two artefact classes that have shipped from this pipeline before?

WHAT IT CHECKS, per rule target (both packs; the macro decides the namespace)

  1. COMPLETENESS. The root .vpcf_c, every system in its child closure, every material those
     systems name, and every texture those materials reference, all present in the staged pack.
     A missing material is the exact failure that made 40% of the Insurgency pack draw white
     error quads while the asset validator reported success, because it classified unconverted
     materials as "the base game provides this". CS2 does not.

  2. RENDERER-LESS ORPHANS. A system with no C_OP_Render* AND no children draws with an engine
     default -- a large pale sprite. Source 1 refract/heat-distortion shockwaves convert into
     exactly this shape. A renderer-less system WITH children is a legitimate container and is
     not reported.

  3. UNSHAPED ADDITIVE TRANSLUCENCY. An additive sprite whose translucency map is a literal
     constant, or an alpha channel that is opaque to the edges, has no shape for the shader to
     cut the quad with, so it composites as a hard-edged rectangle of its own colour. This is
     what drew the Povarehok HE's rectangular flash and its red shrapnel quads.
     FX/tools/repair-additive-alpha.py fixes them; this reports any that slipped through.

USAGE
    python FX/tools/verify-fx-pack.py --stage build/source2-release/fx/source_mvm_fx
Exit 1 on any problem, so a build script can gate on it.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import deque
from pathlib import Path

RES = re.compile(r'resource:"([^"]+)"')
TEX_IN_VMAT = re.compile(r'"(materials/[^"]+?\.tga)"')
TRANS_RE = re.compile(r'\bTextureTranslucency\s+"([^"]+)"')
OPAQUE_EDGE_THRESHOLD = 200

# Which macro builds which resource paths. Keeping this explicit (rather than guessing from the
# arguments) is what lets one rules file drive two packs with different tree shapes.
MACROS = {
    "FXRULE":            lambda p, n: ["particles/filmmaker/insurgency/%s/%s.vpcf" % (p, n),
                                       "particles/filmmaker/insurgency/%s/%s_less.vpcf" % (p, n)],
    "FXRULE_L":          lambda p, n: ["particles/filmmaker/insurgency/%s/%s.vpcf" % (p, n),
                                       "particles/filmmaker/insurgency/%s/%s_less.vpcf" % (p, n)],
    "FXRULE_MODERN":     lambda p, n: ["particles/filmmaker/insurgency/%s/%s.vpcf" % (p, n),
                                       "particles/filmmaker/insurgency/%s/%s_less.vpcf" % (p, n)],
    "FXRULE_MODERN_L":   lambda p, n: ["particles/filmmaker/insurgency/%s/%s.vpcf" % (p, n),
                                       "particles/filmmaker/insurgency/%s/%s_less.vpcf" % (p, n)],
    "FXRULE_POV_MODERN": lambda p, n: ["particles/filmmaker/povarehok/regular/%s/%s.vpcf" % (p, n),
                                       "particles/filmmaker/povarehok/less/smoke/%s/%s.vpcf" % (p, n)],
}
ARGS_RE = re.compile(r'"[^"]*\.vpcf"\s*,\s*"([a-z0-9_]+)"\s*,\s*"([a-z0-9_]+)"')


def rule_targets(rules_cpp: Path) -> set[str]:
    out = set()
    for line in rules_cpp.read_text(encoding="utf-8", errors="replace").splitlines():
        s = line.strip()
        if s.startswith("//") or s.startswith("#define"):
            continue
        for macro, build in MACROS.items():
            if macro + "(" not in s:
                continue
            m = ARGS_RE.search(s)
            if m:
                out.update(build(m.group(1), m.group(2)))
            break
    return out


def closure(stage: Path, root: str, contents: list[Path]) -> tuple[set[str], set[str]]:
    """Systems and materials reachable from `root`.

    Child systems come from the COMPILED file in the mount, because that is what the game will
    actually follow. Material names come from the AUTHORED .vpcf: a compiled vpcf stores its
    material as a resource handle rather than a readable path, so scanning the binary for
    `materials/....vmat` finds nothing and every completeness check silently passes.
    """
    systems, materials = set(), set()
    q, seen = deque([root]), set()
    while q:
        r = q.popleft().replace("\\", "/")
        if r in seen:
            continue
        seen.add(r)
        c = stage / (r + "_c")
        if not c.is_file():
            continue
        systems.add(r)
        for ref in re.findall(rb'(particles/[A-Za-z0-9_/\-.]+?\.vpcf)', c.read_bytes()):
            q.append(ref.decode())
        for root_dir in contents:
            src = root_dir / r
            if src.is_file():
                text = src.read_text(encoding="utf-8", errors="replace")
                for ref in RES.findall(text):
                    ref = ref.replace("\\", "/")
                    if ref.endswith(".vmat"):
                        materials.add(ref)
                    elif ref.endswith(".vpcf"):
                        q.append(ref)
                break
    return systems, materials


def unshaped_additive(stage: Path, content_roots: list[Path], mat_rel: str) -> str | None:
    """Report a reason string when an additive material's translucency carries no shape."""
    src = None
    for c in content_roots:
        p = c / mat_rel
        if p.is_file():
            src = p
            break
    if src is None:
        return None  # no authored source to inspect (base-game material)
    text = src.read_text(encoding="utf-8", errors="replace")
    if "F_ADDITIVE_BLEND" not in text:
        return None
    m = TRANS_RE.search(text)
    if not m:
        return None
    value = m.group(1)
    if value.startswith("["):
        return "literal translucency %s" % value
    from PIL import Image
    tp = None
    for c in content_roots:
        if (c / value).is_file():
            tp = c / value
            break
    if tp is None:
        return None
    try:
        with Image.open(tp) as img:
            a = img.convert("L").resize((32, 32))
            px = list(a.getdata())
            border = ([px[i] for i in range(32)] + [px[31 * 32 + i] for i in range(32)]
                      + [px[i * 32] for i in range(32)] + [px[i * 32 + 31] for i in range(32)])
            edge = sum(border) / len(border)
            if edge > OPAQUE_EDGE_THRESHOLD:
                return "translucency map opaque to the edge (mean %.0f)" % edge
    except Exception:
        return None
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stage", type=Path,
                    default=Path("build/source2-release/fx/source_mvm_fx"))
    ap.add_argument("--rules", type=Path,
                    default=Path("AfxHookSource2/Filmmaker/Movie/ParticleFxRules.cpp"))
    ap.add_argument("--content", type=Path, nargs="*", default=[
        Path("build/fx/fx-source1import/source2/content/source_mvm_fx"),
        Path("build/fx/povarehok-source1import/source2/content/source_mvm_fx"),
    ])
    args = ap.parse_args()
    stage = args.stage.resolve()
    contents = [c.resolve() for c in args.content if c.exists()]

    targets = sorted(rule_targets(args.rules))
    missing_root, missing_mat, missing_tex, orphans, unshaped = [], [], [], [], []
    all_systems, all_mats = set(), set()

    for t in targets:
        if not (stage / (t + "_c")).is_file():
            missing_root.append(t)
            continue
        systems, mats = closure(stage, t, contents)
        all_systems |= systems
        all_mats |= mats
        for m in mats:
            if not (stage / (m + "_c")).is_file():
                missing_mat.append("%s  (needed by %s)" % (m, t.split("/")[-1]))

    for s in sorted(all_systems):
        for c in contents:
            p = c / s
            if p.is_file():
                txt = p.read_text(encoding="utf-8", errors="replace")
                if "C_OP_Render" not in txt and "m_ChildRef" not in txt:
                    orphans.append(s)
                break

    for m in sorted(all_mats):
        why = unshaped_additive(stage, contents, m)
        if why:
            unshaped.append("%s  -- %s" % (m, why))
        c = stage / (m + "_c")
        if c.is_file():
            for tex in re.findall(rb'(materials/[A-Za-z0-9_/\-.]+?\.(?:vtex|tga))', c.read_bytes()):
                rel = re.sub(r"\.(vtex|tga)$", ".vtex", tex.decode())
                if not (stage / (rel + "_c")).is_file():
                    missing_tex.append("%s  (from %s)" % (rel, m.split("/")[-1]))

    print("rule targets            : %d" % len(targets))
    print("systems reached         : %d" % len(all_systems))
    print("materials reached       : %d" % len(all_mats))

    problems = 0
    for label, items in (("ROOT NOT STAGED", missing_root),
                         ("MATERIAL NOT STAGED", sorted(set(missing_mat))),
                         ("TEXTURE NOT STAGED", sorted(set(missing_tex))),
                         ("RENDERER-LESS ORPHAN (draws an engine default)", sorted(set(orphans))),
                         ("UNSHAPED ADDITIVE (draws a hard rectangle)", sorted(set(unshaped)))):
        if items:
            problems += len(items)
            print("\n%s: %d" % (label, len(items)))
            for x in items[:20]:
                print("   " + x)
            if len(items) > 20:
                print("   ... and %d more" % (len(items) - 20))

    if problems:
        print("\n%d PROBLEM(S)" % problems)
        return 1
    print("\nOK: every rule target is fully staged, no renderer-less orphans, "
          "no unshaped additive materials.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
