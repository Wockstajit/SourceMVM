#!/usr/bin/env python3
"""Classify disassembled CS2 forward pixel programs (fxc /dumpbin output).

    python Automation/tools/classify-programs.py <dir-of-.txt> [--detail hash]

For each program it reports the resources the lighting ABI depends on, so an ABI derivation can be
checked against every shipped permutation instead of the handful that were audited by hand.
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

RES = re.compile(r"dcl_resource_(\w+)(?: \((\w+),(\w+),(\w+),(\w+)\))? t(\d+)(?:, (\d+))?")
STRUCT = re.compile(r"dcl_resource_structured t(\d+), (\d+)")
SAMP = re.compile(r"dcl_sampler s(\d+), (\w+)")
CB = re.compile(r"dcl_constantbuffer CB(\d+)\[(\d+)\]")
CBI = re.compile(r"dcl_constantbuffer cb(\d+)\[(\d+)\], (\w+)")


def classify(path: Path) -> dict:
    text = path.read_text(errors="replace")
    info: dict = {"hash": path.stem, "lines": text.count("\n")}
    res = {}
    for m in re.finditer(r"dcl_resource_(\w+) \(([^)]*)\) t(\d+)", text):
        res[int(m.group(3))] = m.group(1)
    for m in re.finditer(r"dcl_resource_structured t(\d+), (\d+)", text):
        res[int(m.group(1))] = f"structured{m.group(2)}"
    for m in re.finditer(r"dcl_resource_raw t(\d+)", text):
        res[int(m.group(1))] = "raw"
    info["res"] = res
    info["samplers"] = {int(m.group(1)): m.group(2) for m in SAMP.finditer(text)}
    info["cb"] = {int(m.group(1)): int(m.group(2)) for m in re.finditer(r"dcl_constantbuffer CB(\d+)\[(\d+)\]", text)}
    info["cb"].update({int(m.group(1)): int(m.group(2)) for m in re.finditer(r"dcl_constantbuffer cb(\d+)\[(\d+)\]", text)})
    info["structured480"] = [s for s, t in res.items() if t == "structured480"]
    info["lm"] = [s for s in (1, 2, 3) if res.get(s) == "texture2darray"]
    info["cube"] = [s for s, t in res.items() if t == "texturecube"]
    info["cubearray"] = [s for s, t in res.items() if t == "texturecubearray"]
    info["tex3d"] = [s for s, t in res.items() if t == "texture3d"]
    info["usesCb3Sun"] = "cb3[1945]" in text or "CB3[1945]" in text
    info["cmp"] = [s for s, m in info["samplers"].items() if m == "mode_comparison"]
    return info


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--detail")
    args = ap.parse_args()
    rows = [classify(p) for p in sorted(Path(args.dir).glob("*.txt"))]
    groups: Counter = Counter()
    example: dict = {}
    for r in rows:
        key = (
            tuple(r["structured480"]),
            tuple(r["lm"]),
            tuple(r["tex3d"]),
            tuple(r["cube"]),
            tuple(r["cubearray"]),
            r["cb"].get(3),
            tuple(r["cmp"]),
            r["usesCb3Sun"],
        )
        groups[key] += 1
        example.setdefault(key, r["hash"])
    print(f"{len(rows)} programs, {len(groups)} distinct interface shapes")
    print("light480 | lmArrays | tex3d | cube | cubeArray | cb3 rows | cmpSampler | cb3 sun | count | example")
    for key, count in groups.most_common():
        print(" | ".join(str(k) for k in key), "|", count, "|", example[key])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
