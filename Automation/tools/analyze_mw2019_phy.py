"""Dump Source 1 .phy class shares from the local MW2019 HL2 workshop VPK.

Does not copy or commit the workshop files. Path override:

  py -3 Automation/tools/analyze_mw2019_phy.py --vpk <workshop_dir.vpk>
"""
from __future__ import annotations

import argparse
import collections
import re
import sys

import vpk


CLASS_RULES = (
    ("foot", "foot"),
    ("hand", "hand"),
    ("calf", "calf"),
    ("forearm", "lowerarm"),
    ("thigh", "thigh"),
    ("upperarm", "upperarm"),
    ("head", "head"),
    ("neck", "head"),
    ("spine", "torso"),
    ("pelvis", "pelvis"),
)


def classify(name: str) -> str:
    n = name.lower()
    for token, cls in CLASS_RULES:
        if token in n:
            return cls
    return "unknown"


def parse_solids(text: str) -> list[tuple[str, float]]:
    solids = re.findall(r"solid\s*\{(.*?)(?=solid\s*\{|ragdollconstraint|editparams)", text, re.S)
    out = []
    for block in solids:
        name_m = re.search(r'"name"\s+"([^"]+)"', block)
        mass_m = re.search(r'"mass"\s+"([^"]+)"', block)
        if not name_m or not mass_m:
            continue
        out.append((name_m.group(1), float(mass_m.group(1))))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--vpk", required=True, help="Path to the HL2 workshop workshop_dir.vpk")
    ap.add_argument("--file", default="models/player/corpse01.phy")
    args = ap.parse_args()
    pak = vpk.open(args.vpk)
    data = pak.get_file(args.file).read().decode("latin1", errors="ignore")
    solids = parse_solids(data)
    if not solids:
        print("no solids parsed", file=sys.stderr)
        return 1
    total = sum(m for _, m in solids)
    by_class: dict[str, float] = collections.defaultdict(float)
    print(f"{args.file}: {len(solids)} solids, mass sum {total:.3f}")
    for name, mass in solids:
        cls = classify(name)
        by_class[cls] += mass
        print(f"  {cls:10s} {mass:10.4f}  {name}")
    print("class shares:")
    for cls, mass in sorted(by_class.items(), key=lambda kv: -kv[1]):
        print(f"  {cls:10s} {mass / total:.6f}  ({mass:.3f})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
