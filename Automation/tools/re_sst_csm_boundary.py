#!/usr/bin/env python3
"""Locate the CS2 Sparse Shadow Tree -> CSM copy boundary.

Standalone research tool. Shares no build/authoring logic with other automation
scripts; it only reads the shipped CS2 binaries.

Anchors:
  CopySSTDepthToCSM_PS
  CopySSTDepthToCSM_CopyTex
  sparseshadowtree_copy_to_shadow_atlas_ps

Verified on scenesystem.dll ClientVersion 2000880:
  builder entry VA 0x180157510 (vtable slot 0x1805eb6b8)
  PS path when sparseshadowtree_copy_to_shadow_atlas_ps != 0
  CopyTex path otherwise

Example:
  python Automation/tools/re_sst_csm_boundary.py
  python Automation/tools/re_sst_csm_boundary.py --json out.json
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from re_lightbinner_shadows import (  # noqa: E402
    DEFAULT_GAME_DIRS,
    Image,
    disasm_window,
    find_function_start,
    find_rip_refs,
    resolve_convar_objects,
)


ANCHORS = [
    "CopySSTDepthToCSM_PS",
    "CopySSTDepthToCSM_CopyTex",
    "sparseshadowtree_copy_to_shadow_atlas_ps",
    "sparseshadowtree_unpack_direct_to_shadow_atlas",
    "sparseshadowtree_enable_rendering",
    "sparseshadowtree_disable_add_layers",
]

# Unique prologue of the CopySSTDepthToCSM builder (0x180157510 on 2000880).
BUILDER_PROLOGUE = bytes.fromhex(
    "48 89 5c 24 08 4c 89 4c 24 20 55 56 57 41 54 41 55 41 56 41 57 "
    "48 8d 6c 24 80 48 81 ec 80 01 00 00"
)


def find_builder_by_prologue(image: Image):
    blob, code_va = image.text_bytes()
    hits = []
    start = 0
    while True:
        i = blob.find(BUILDER_PROLOGUE, start)
        if i < 0:
            break
        hits.append(code_va + i)
        start = i + 1
    return hits


def find_absolute_ptrs(image: Image, target_va: int):
    pat = struct.pack("<Q", target_va)
    out = []
    for s in image.sections:
        blob = image.data[s["raw"] : s["raw"] + s["rawsize"]]
        start = 0
        while True:
            i = blob.find(pat, start)
            if i < 0:
                break
            out.append({"va": hex(s["va"] + i), "section": s["name"]})
            start = i + 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=None)
    ap.add_argument("--module", default="bin/win64/scenesystem.dll")
    ap.add_argument("--json", default=None)
    ap.add_argument("--disasm", action="store_true")
    args = ap.parse_args()

    game = args.game
    if not game:
        for candidate in DEFAULT_GAME_DIRS:
            if os.path.isdir(candidate):
                game = candidate
                break
    if not game:
        sys.exit("could not locate CS2 game directory; pass --game")

    path = os.path.join(game, args.module)
    if not os.path.isfile(path):
        sys.exit(f"missing module: {path}")

    image = Image(path)
    print(f"# module {image.name} base=0x{image.base:x}")

    report = {"module": image.name, "anchors": {}, "copy_builder": None}

    for name in ANCHORS:
        vas = image.find_string_vas(name)
        entry = {"string_vas": [hex(v) for v in vas], "xrefs": []}
        if vas:
            for va, mnem, ops, tgt in find_rip_refs(image, set(vas)):
                fn = find_function_start(image, va)
                entry["xrefs"].append(
                    {
                        "site": hex(va),
                        "insn": f"{mnem} {ops}",
                        "function_guess": hex(fn) if fn else None,
                        "target": hex(tgt),
                    }
                )
        report["anchors"][name] = entry
        print(f"\n== {name}")
        print(f"   string  {', '.join(entry['string_vas']) or 'NOT FOUND'}")
        for x in entry["xrefs"]:
            print(f"   xref    {x['site']}  guess={x['function_guess']}  {x['insn']}")

    builders = find_builder_by_prologue(image)
    if not builders:
        sys.exit("CopySSTDepthToCSM builder prologue not found — CS2 updated?")
    if len(builders) != 1:
        print(f"# WARNING: prologue matched {len(builders)} times: {[hex(b) for b in builders]}")
    builder = builders[0]
    ptrs = find_absolute_ptrs(image, builder)

    gate = resolve_convar_objects(image, ["sparseshadowtree_copy_to_shadow_atlas_ps"]).get(
        "sparseshadowtree_copy_to_shadow_atlas_ps", {}
    )
    gate_obj = None
    for r in gate.get("registrations") or []:
        if r.get("objects"):
            gate_obj = r["objects"][0]["va"]
            break

    report["copy_builder"] = {
        "function": hex(builder),
        "rva": hex(builder - image.base),
        "prologue_matches": len(builders),
        "vtable_or_ptr_slots": ptrs,
        "gate_cvar": "sparseshadowtree_copy_to_shadow_atlas_ps",
        "gate_object": gate_obj,
        "ps_branch": report["anchors"]["CopySSTDepthToCSM_PS"]["xrefs"],
        "copytex_branch": report["anchors"]["CopySSTDepthToCSM_CopyTex"]["xrefs"],
        "notes": [
            "Virtual method (no direct E8 callers); installed via Detour on the prologue.",
            "PS path when sparseshadowtree_copy_to_shadow_atlas_ps != 0; else CopyTex.",
            "Suppressing this builder is the candidate native SST->CSM seam.",
            "Hook pattern lives in AfxHookSource2/Filmmaker/World/ShadowStageProbe.cpp.",
        ],
    }

    print(f"\n== copy builder")
    print(f"   function {hex(builder)}  RVA {hex(builder - image.base)}")
    print(f"   ptr slots {ptrs}")
    print(f"   gate obj  {gate_obj}")

    if args.disasm:
        ps = report["anchors"]["CopySSTDepthToCSM_PS"]["xrefs"]
        site = int(ps[0]["site"], 16) if ps else builder
        print(f"\n== disasm around {hex(site)}")
        for addr, mnem, ops in disasm_window(image, site, 64, 160):
            mark = "  <<<" if addr == site else ""
            print(f"0x{addr:x}  {mnem:<8} {ops}{mark}")

    if args.json:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)) or ".", exist_ok=True)
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(report, fh, indent=2)
        print(f"\n# wrote {args.json}")


if __name__ == "__main__":
    main()
