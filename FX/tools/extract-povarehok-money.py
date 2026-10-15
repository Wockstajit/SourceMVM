"""Stage the money-on-headshot Source 1 sources from the local CS:GO Povarehok mod.

Money-on-headshot is its own feature with its own runtime toggle
(`mirv_filmmaker fx moneyshot on|off`, AfxHookSource2/Filmmaker/Movie/ParticleFxMoney.cpp)
and its own single swap target, independent of the On/Less/Modern particle modes. When the
Insurgency 2 rebake replaced the Povarehok mod as the On/Less asset source (2026-09-01) this
one effect had no Insurgency equivalent -- Insurgency has no money-burst -- and the user asked
to keep it, so it gets its OWN committed source tree instead of riding along on a pack that no
longer exists:

  <output>/particles/filmmaker/money/impact_fxmoney.pcf
  <output>/materials/<original relative path>.vmt/.vtf
  <output>/models/usMoney/...

The mod ships the money burst inside `impact_fxmoney.pcf`, a complete ALTERNATE copy of
impact_fx.pcf (216 systems) where the headshot systems throw dollar bills. Only ONE system is
ever swapped to -- `impact_helmet_headshot` (kMoneyBurst in ParticleFxInternal.h). The whole
PCF is staged because it is one binary bundle that cannot be split, but its MATERIALS are
staged from the real closure of that one system: 15 child systems referencing 9 VMTs and the
bill model. Scanning the PCF's whole string table instead would drag in every impact texture
the alternate file also carries (~34 MB of donor-pack atlases the money burst never renders);
walking the child tree keeps this at ~5 MB.

This tool only READS the mod folder; nothing is modified. Requires the git-ignored mod at
reference/csgo effect mod/ -- it is a ONE-TIME extraction whose output is committed, so a
normal build never needs the mod present.

Requires: pip install srctools

Usage (from repo root):
    python FX/tools/extract-povarehok-money.py
"""

from __future__ import annotations

import argparse
import io
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_MOD = (REPO / "reference" / "csgo effect mod"
               / "p_betterparticlesmod_classic updated_c057b" / "p_betterparticlesmod_classic")
DEFAULT_OUTPUT = REPO / "FX" / "sources" / "povarehok-money"

MONEY_PCF = "impact_fxmoney.pcf"
# The single system the DLL ever swaps to. Keep in sync with kMoneyBurst in
# AfxHookSource2/Filmmaker/Movie/ParticleFxInternal.h.
MONEY_ROOT_SYSTEM = "impact_helmet_headshot"
# The bill model's three Source 1 files. The model is named by the burst's `clique6` child;
# .vvd/.vtx are siblings the MDL needs but never names.
MONEY_MODEL_FILES = [
    "models/usMoney/usmoney bills.mdl",
    "models/usMoney/usmoney bills.vvd",
    "models/usMoney/usMoney bills.dx90.vtx",
]

VMT_VALUE_RE = re.compile(r'"?\$[a-zA-Z0-9_]+"?\s+"?([^"\r\n]+?)"?\s*$', re.MULTILINE)
VMT_COMMENT_RE = re.compile(r"//.*")
BASETEXTURE_RE = re.compile(r'"?\$basetexture"?\s+"?([^"\r\n]+?)"?\s*$',
                            re.IGNORECASE | re.MULTILINE)


def money_closure_materials(pcf_bytes: bytes) -> tuple[set[str], list[str]]:
    """Walk the money burst's child tree and collect the .vmt paths it actually renders.

    Returns (material paths relative to materials/, closure system names). Only STRING-typed
    operator attributes are read: a PCF stores numbers and element links in the same option
    map, and srctools raises when asked to coerce those to text.
    """
    from srctools.dmx import ValueType
    from srctools.particles import Particle

    parts = Particle.parse(io.BytesIO(pcf_bytes))
    if isinstance(parts, tuple):
        parts = parts[0]
    if MONEY_ROOT_SYSTEM not in parts:
        raise KeyError(f"{MONEY_ROOT_SYSTEM} is not defined in {MONEY_PCF}")

    seen: set[str] = set()
    order: list[str] = []

    def walk(name: str) -> None:
        if name in seen or name not in parts:
            return
        seen.add(name)
        order.append(name)
        for child in parts[name].children:
            ref = getattr(child, "particle", None) or getattr(child, "name", None)
            if isinstance(ref, str):
                walk(ref)
            elif ref is not None:
                walk(getattr(ref, "name", str(ref)))

    walk(MONEY_ROOT_SYSTEM)

    def strings(attrmap) -> set[str]:
        out: set[str] = set()
        for attr in attrmap.values():
            try:
                if attr.type is ValueType.STRING:
                    out.add(str(attr.val_str))
            except Exception:
                continue
        return out

    refs: set[str] = set()
    for name in order:
        system = parts[name]
        found = strings(system.options)
        for group in (system.renderers, system.operators, system.initializers,
                      system.emitters, system.forces, system.constraints):
            for op in group:
                found |= strings(op.options)
        for value in found:
            low = value.replace("\\", "/").lower()
            if low.endswith(".vmt"):
                refs.add(low.lstrip("/"))
    return refs, order


def vmt_texture_refs(vmt_text: str) -> set[str]:
    refs: set[str] = set()
    for match in VMT_VALUE_RE.finditer(VMT_COMMENT_RE.sub("", vmt_text)):
        value = match.group(1).replace("\\", "/").strip().lower().strip('"')
        if value.endswith(".vtf"):
            value = value[: -len(".vtf")]
        if not value or value.startswith("$") or " " in value:
            continue
        if re.fullmatch(r"[a-z0-9_/\-\.]+", value) and "." not in value.rsplit("/", 1)[-1]:
            refs.add(value)
    return refs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mod-root", type=Path, default=DEFAULT_MOD,
                        help="the 'classic updated' Povarehok variant folder")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    mod = args.mod_root
    if not (mod / "particles" / MONEY_PCF).is_file():
        print(f"error: {MONEY_PCF} not found under {mod}. This extractor needs the git-ignored "
              f"CS:GO mod at reference/csgo effect mod/; its output is committed, so a normal "
              f"build does not.", file=sys.stderr)
        return 2

    out_particles = args.output / "particles" / "filmmaker" / "money"
    out_particles.mkdir(parents=True, exist_ok=True)
    data = (mod / "particles" / MONEY_PCF).read_bytes()
    (out_particles / MONEY_PCF).write_bytes(data)
    try:
        material_refs, closure = money_closure_materials(data)
    except ImportError:
        print("error: the 'srctools' python package is required (pip install srctools)",
              file=sys.stderr)
        return 2
    print(f"staged {MONEY_PCF} ({len(data)} bytes); {MONEY_ROOT_SYSTEM} closure = "
          f"{len(closure)} systems, {len(material_refs)} materials")

    missing: list[str] = []
    staged_vmt = staged_vtf = 0
    for ref in sorted(material_refs):
        rel_vmt = ref if ref.startswith("materials/") else f"materials/{ref}"
        src = mod / rel_vmt
        if not src.is_file():
            missing.append(rel_vmt)
            continue
        dst = args.output / Path(rel_vmt)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(src.read_bytes())
        staged_vmt += 1

        for tex in vmt_texture_refs(src.read_text(encoding="utf-8", errors="replace")):
            rel_vtf = f"materials/{tex}.vtf"
            tex_src = mod / rel_vtf
            if not tex_src.is_file():
                continue  # non-texture parameter values land here; only real hits staged
            tex_dst = args.output / Path(rel_vtf)
            if not tex_dst.is_file():
                tex_dst.parent.mkdir(parents=True, exist_ok=True)
                tex_dst.write_bytes(tex_src.read_bytes())
                staged_vtf += 1

    for rel in MONEY_MODEL_FILES:
        src = mod / rel
        if not src.is_file():
            print(f"error: required money model file missing: {rel}", file=sys.stderr)
            return 2
        dst = args.output / Path(rel)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(src.read_bytes())

    if missing:
        for rel in missing:
            print(f"warning: material not found in the mod (renderer will be pruned): {rel}",
                  file=sys.stderr)

    # Same hard guarantee the Insurgency and Modern extractors give: a staged VMT whose
    # $basetexture VTF did not stage would convert into a white quad.
    unresolved: list[str] = []
    for vmt_path in (args.output / "materials").rglob("*.vmt"):
        text = vmt_path.read_text(encoding="utf-8", errors="replace")
        base = BASETEXTURE_RE.search(VMT_COMMENT_RE.sub("", text))
        if not base:
            continue
        tex_rel = "materials/" + base.group(1).replace("\\", "/").lower().strip("/").strip('"')
        if not tex_rel.endswith(".vtf"):
            tex_rel += ".vtf"
        if (args.output / Path(tex_rel)).is_file():
            continue
        src = mod / tex_rel
        if src.is_file():
            dst = args.output / Path(tex_rel)
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(src.read_bytes())
            staged_vtf += 1
        else:
            unresolved.append(f"{vmt_path.relative_to(args.output).as_posix()} -> {tex_rel}")
    if unresolved:
        for line in unresolved:
            print(f"error: $basetexture VTF unresolved: {line}", file=sys.stderr)
        return 2

    (args.output / "money-missing-materials.txt").write_text(
        "\n".join(missing) + ("\n" if missing else ""), encoding="ascii")

    total = sum(f.stat().st_size for f in args.output.rglob("*") if f.is_file())
    print(f"staged money sources: {staged_vmt} VMTs, {staged_vtf} VTFs, "
          f"{len(MONEY_MODEL_FILES)} model files under {args.output} ({total / 1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
