"""Stage the Insurgency 2 ("insurgency") Source 1 particle sources from a local install.

Pulls Insurgency 2's own particle libraries and their full material closure (VMTs + the
VTFs they reference) out of the game's VPKs and lays them out as a Source 1 mod tree the
FX converter can stage alongside the Modern (MW2019) tree:

  <output>/particles/filmmaker/insurgency/<pcf name>.pcf
  <output>/materials/<original relative path>.vmt/.vtf

This is the exact shape `extract-modern-particles-gmod.py` produces for the Modern pack,
and for the same reason: the converter then reads a committed, closed, single-donor tree
instead of reaching into a machine-specific game install at build time.

Why Insurgency 2 and not the old CS:GO "Povarehok" mod: that mod's 214-material closure was
assembled from ~12 different donor games (Insurgency 2, Sandstorm, Day of Infamy, GTA V,
Squad, Hell Let Loose, Just Cause 3, Far Cry 4, MW2019, Black Mesa, BrainBread 2, HL2:MMod)
with mutually inconsistent VMT conventions, which is the root cause behind essentially every
texture-failure pattern in Docs/porting/povarehok-csgo-mod-reference.md §10. Insurgency 2
ships a complete, self-consistent FX set of its own -- and, conveniently, inherited Source's
impact/tracer naming, so it maps onto CS2's system names almost 1:1.

This tool only READS the Insurgency install; nothing is modified. Exits non-zero when a
required PCF or any referenced $basetexture cannot be found (a missing sprite would
otherwise convert into an invisible or white-quad particle piece).

Requires: pip install vpk

Usage (from repo root):
    python FX/tools/extract-insurgency-fx.py
    python FX/tools/extract-insurgency-fx.py --insurgency-root "F:\\SteamLibrary\\steamapps\\common\\insurgency2"
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = REPO / "FX" / "sources" / "insurgency2"

# The particle libraries the CS2 swap tables (ParticleFxRules.cpp) draw from. Deliberately
# a subset: Insurgency ships 38 PCFs, but ammo caches, rope/insect/rain/menu ambience and
# the unused fire_01_* experiments are never referenced by any FXRULE, and carrying them
# would triple the committed tree for nothing. Adding a rule that needs a new library means
# adding it here and re-running the extractor.
CORE_PCFS = [
    "impact_fx_ins.pcf",      # every surface bullet impact (impact_concrete/_metal/_wood/...)
    "weapon_fx_ins.pcf",      # muzzle flashes (per weapon, 1p + 3rd), barrel smoke, shell eject, molotov
    "weapon_fx_ins_b.pcf",    # the AK-74 flash family + shared muzzle smoke/spark variants
    "weapon_fx_tracers.pcf",  # the weapon_tracers family
    "explosion_fx_ins.pcf",   # grenade / C4 / flashbang / M203 / RPG detonations
    "blood_fx.pcf",           # the rich blood set (headshot, arterial spray, armor, gibs)
    "blood_impact.pcf",       # the plain blood_impact_red_01 family
    "ins_smokegrenade.pcf",   # smoke grenade cloud
    "fire_01_ins.pcf",        # the smoke_large/_medium/_small lingering smoke set
    # NOTE: fire_01.pcf (the env_fire_* library) was dropped 2026-09-02. It was added for
    # molotov ground fire, but that now plays vanilla CS2 -- Insurgency's env_fire_* are
    # entity campfires and read as a row of fat blobs under CS2's per-flame inferno
    # spawning. Nothing references it, so carrying ~13 MB of it in the tree is dead weight.
    "ins_burning_fx.pcf",     # burning_character (molotov victims)
    "water_impact.pcf",       # water splashes
]

PRINTABLE_RUN_RE = re.compile(rb"[ -~]{6,}")
# VMT texture-ish parameter values worth chasing into VTFs. Matched against every
# quoted/bare parameter value; anything that resolves to materials/<value>.vtf in the
# game's VPKs is pulled in.
VMT_VALUE_RE = re.compile(r'"?\$[a-zA-Z0-9_]+"?\s+"?([^"\r\n]+?)"?\s*$', re.MULTILINE)
# The mod's VMTs carry commented-out parameters (the author trying a flag then disabling
# it). Strip comments before reading any value -- see povarehok-csgo-mod-reference.md §10.
VMT_COMMENT_RE = re.compile(r"//.*")
BASETEXTURE_RE = re.compile(r'"?\$basetexture"?\s+"?([^"\r\n]+?)"?\s*$',
                            re.IGNORECASE | re.MULTILINE)


def resolve_insurgency_root(explicit: str | None) -> Path | None:
    """Locate an Insurgency 2 install: explicit flag, env override, then Steam libraries."""
    if explicit:
        return Path(explicit)
    env = os.environ.get("SOURCEMVM_INSURGENCY2_DIR")
    if env:
        return Path(env)

    candidates: list[Path] = []
    steam_roots = [
        Path(r"C:\Program Files (x86)\Steam"),
        Path(r"C:\Program Files\Steam"),
    ]
    for steam in steam_roots:
        vdf = steam / "steamapps" / "libraryfolders.vdf"
        if not vdf.is_file():
            continue
        text = vdf.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r'"path"\s+"([^"]+)"', text):
            candidates.append(Path(m.group(1).replace("\\\\", "\\")))
        candidates.append(steam)
    for lib in candidates:
        hit = lib / "steamapps" / "common" / "insurgency2"
        if (hit / "insurgency").is_dir():
            return hit
    return None


class VpkIndex:
    """Ordered lookup across the game's VPKs by game-relative path."""

    def __init__(self) -> None:
        self._paks: list[tuple[str, object]] = []

    def add(self, label: str, pak) -> None:
        self._paks.append((label, pak))

    def read(self, rel: str) -> tuple[bytes, str] | None:
        rel = rel.lower().replace("\\", "/")
        for label, pak in self._paks:
            try:
                return pak[rel].read(), label
            except KeyError:
                continue
        return None


def scan_pcf_materials(data: bytes) -> set[str]:
    """Every materials/*.vmt path a PCF's string table mentions."""
    refs: set[str] = set()
    for match in PRINTABLE_RUN_RE.finditer(data):
        s = match.group(0).decode("ascii", errors="replace").replace("\\", "/").lower()
        if s.endswith(".vmt"):
            refs.add(s.lstrip("/"))
    return refs


def vmt_texture_refs(vmt_text: str) -> set[str]:
    """Candidate texture paths from every $parameter value in a VMT.

    Values are only candidates -- the caller tries materials/<value>.vtf against the VPKs
    and silently drops non-hits (numbers, shader keywords, env_cubemap, ...).
    """
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
    parser.add_argument("--insurgency-root", default=None,
                        help=r"Insurgency 2 install dir, e.g. F:\SteamLibrary\steamapps\common\insurgency2")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    try:
        import vpk as vpk_mod
    except ImportError:
        print("error: the 'vpk' python package is required (pip install vpk)", file=sys.stderr)
        return 2

    root = resolve_insurgency_root(args.insurgency_root)
    if root is None or not (root / "insurgency").is_dir():
        print("error: could not locate an Insurgency 2 install. Pass --insurgency-root or set "
              "SOURCEMVM_INSURGENCY2_DIR.", file=sys.stderr)
        return 2
    game = root / "insurgency"
    print(f"insurgency 2 install: {root}")

    index = VpkIndex()
    for name in ("insurgency_particles_dir.vpk", "insurgency_materials_dir.vpk",
                 "insurgency_misc_dir.vpk"):
        pak_path = game / name
        if not pak_path.is_file():
            print(f"error: missing {pak_path}", file=sys.stderr)
            return 2
        index.add(f"vpk:{name}", vpk_mod.open(str(pak_path)))

    out_particles = args.output / "particles" / "filmmaker" / "insurgency"
    out_particles.mkdir(parents=True, exist_ok=True)

    material_refs: set[str] = set()
    for pcf_name in CORE_PCFS:
        hit = index.read(f"particles/{pcf_name}")
        if hit is None:
            print(f"error: {pcf_name} not found in the Insurgency VPKs", file=sys.stderr)
            return 2
        data, _label = hit
        (out_particles / pcf_name).write_bytes(data)
        refs = scan_pcf_materials(data)
        material_refs.update(refs)
        print(f"staged {pcf_name} ({len(data)} bytes, {len(refs)} material refs)")

    missing: list[str] = []
    staged_vmt = staged_vtf = 0
    for ref in sorted(material_refs):
        rel_vmt = ref if ref.startswith("materials/") else f"materials/{ref}"
        hit = index.read(rel_vmt)
        if hit is None:
            missing.append(rel_vmt)
            continue
        vmt_data, _label = hit
        dst = args.output / Path(rel_vmt)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(vmt_data)
        staged_vmt += 1

        vmt_text = vmt_data.decode("utf-8", errors="replace")
        for tex in vmt_texture_refs(vmt_text):
            rel_vtf = f"materials/{tex}.vtf"
            tex_hit = index.read(rel_vtf)
            if tex_hit is None:
                continue  # non-texture parameter values land here; only real hits staged
            tex_dst = args.output / Path(rel_vtf)
            if not tex_dst.is_file():
                tex_dst.parent.mkdir(parents=True, exist_ok=True)
                tex_dst.write_bytes(tex_hit[0])
                staged_vtf += 1

    if missing:
        # A wholly missing VMT degrades to an absent renderer (postprocess prunes renderers
        # whose material never arrived), not a white quad -- so warn, don't fail.
        for rel in missing:
            print(f"warning: material not found in any VPK (renderer will be pruned): {rel}",
                  file=sys.stderr)

    # A staged VMT whose $basetexture VTF did not stage WOULD convert into a white quad;
    # treat that (unlike a wholly missing VMT) as an error since the source clearly exists.
    # This hard failure is the guarantee that makes the Modern pack reliable, and is the
    # single most important property this pipeline borrows from it.
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
        hit = index.read(tex_rel)
        if hit is None:
            unresolved.append(f"{vmt_path.relative_to(args.output).as_posix()} -> {tex_rel}")
            continue
        tex_dst = args.output / Path(tex_rel)
        tex_dst.parent.mkdir(parents=True, exist_ok=True)
        tex_dst.write_bytes(hit[0])
        staged_vtf += 1
    if unresolved:
        for line in unresolved:
            print(f"error: $basetexture VTF unresolved: {line}", file=sys.stderr)
        return 2

    (args.output / "insurgency-missing-materials.txt").write_text(
        "\n".join(missing) + ("\n" if missing else ""), encoding="ascii")

    total = sum(f.stat().st_size for f in args.output.rglob("*") if f.is_file())
    print(f"staged insurgency sources: {len(material_refs)} VMT refs ({staged_vmt} VMTs, "
          f"{staged_vtf} VTFs) under {args.output} ({total / 1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
