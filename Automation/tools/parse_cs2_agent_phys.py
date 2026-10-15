"""Parse a Source2Viewer-CLI PHYS dump of a CS2 agent .vmdl_c.

Prints GetBody-index bone names, compiled masses, and constraint friction.
Does not call VRF itself; dump first:

  Source2Viewer-CLI.exe -i pak01_dir.vpk -f agents/models/tm_phoenix/tm_phoenix.vmdl_c -o tm_phoenix.vmdl_c
  Source2Viewer-CLI.exe -i tm_phoenix.vmdl_c -b PHYS > tm_phoenix.phys.kv3
"""
from __future__ import annotations

import argparse
import re
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("kv3", help="PHYS kv3 text from Source2Viewer-CLI -b PHYS")
    args = ap.parse_args()
    text = open(args.kv3, encoding="utf-8", errors="replace").read()
    names_m = re.search(r"m_boneNames\s*=\s*\[(.*?)\]", text, re.S)
    names = re.findall(r'"([^"]+)"', names_m.group(1)) if names_m else []
    masses = [float(x) for x in re.findall(r"m_flMass = ([0-9.]+)", text)]
    cons = text.split("m_constraints")[1] if "m_constraints" in text else ""
    joints = re.findall(
        r"m_nBody1 = (\d+)\s+m_nBody2 = (\d+).*?m_flFriction = ([0-9.]+)",
        cons,
        re.S,
    )
    print(f"bones {len(names)} masses {len(masses)} sum {sum(masses):.1f}")
    n = max(len(names), len(masses))
    for i in range(n):
        name = names[i] if i < len(names) else "?"
        mass = masses[i] if i < len(masses) else 0.0
        print(f"  {i:2d} {name:16s} {mass:7.1f}")
    print(f"constraints {len(joints)}")
    for b1, b2, fr in joints:
        n1 = names[int(b1)] if int(b1) < len(names) else "?"
        n2 = names[int(b2)] if int(b2) < len(names) else "?"
        print(f"  {b1}->{b2} {n1}->{n2} friction={fr}")
    return 0 if names and masses else 1


if __name__ == "__main__":
    raise SystemExit(main())
