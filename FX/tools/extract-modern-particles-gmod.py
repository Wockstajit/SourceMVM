"""Stage the MW2019 ("modern") Source 1 particle sources from the local GMod install.

Pulls the ARC9/MW2019 PCFs and their full material closure (VMTs + the VTFs they
reference) out of the Steam Workshop GMAs and the GMod base-game VPKs, and lays them
out as a Source 1 mod tree the Povarehok converter can stage alongside the
Povarehok variants:

  <output>/particles/filmmaker/modern/<pcf name>.pcf
  <output>/materials/<original relative path>.vmt/.vtf

Sources (searched in order for every material):
  1. workshop GMA 2910505837  (ARC9 Weapon Base -- FAS muzzle flashes + explosions)
  2. workshop GMA 3258297368  ([ARC9] Modern Warfare 2019 -- tracers)
  3. workshop GMA 2459720887  (Modern Wokefare Base -- a few shared particle sprites)
  4. every *_dir.vpk under the GMod install (stock HL2/GMod sprites the PCFs lean on)

This tool only READS the GMod install; nothing is modified. Exits non-zero when a
required PCF or any referenced material cannot be found (a missing sprite would
otherwise convert into an invisible or white-quad particle piece).

Requires: pip install vpk
"""

from __future__ import annotations

import argparse
import lzma
import re
import sys
from pathlib import Path

# PCFs staged into the modern namespace, keyed by the workshop id that ships them.
# arc9_fas_muzzleflashes: class flashes + barrel_smoke(_plume) + m82_shocksmoke.
# arc9_fas_explosions:   explosion_grenade (the MW2019 frag's real detonation system).
# mw2019_tracer:         the mw2019_tracer family.
PCF_SOURCES = {
    "2910505837": ["arc9_fas_muzzleflashes.pcf", "arc9_fas_explosions.pcf"],
    "3258297368": ["mw2019_tracer.pcf",
                   # Generic_explo_flash: the MW2019 flashbang's detonation (arc9_cod2019_thrownflash)
                   "mw2019_explosions_pak.pcf"],
}
MATERIAL_GMA_IDS = ["2910505837", "3258297368", "2459720887"]

PRINTABLE_RUN_RE = re.compile(rb"[ -~]{6,}")
# VMT texture-ish parameter values worth chasing into VTFs. Matched against every
# quoted/bare parameter value; anything that resolves to materials/<value>.vtf in a
# source is pulled in.
VMT_VALUE_RE = re.compile(r'"?\$[a-zA-Z0-9_]+"?\s+"?([^"\s]+)"?', re.MULTILINE)


def read_cstr(buf: bytes, off: int) -> tuple[str, int]:
    end = buf.index(b"\x00", off)
    return buf[off:end].decode("utf-8", errors="replace"), end + 1


class GmaArchive:
    """Minimal GMA reader (handles the LZMA-wrapped workshop download format)."""

    def __init__(self, path: Path):
        raw = path.read_bytes()
        if raw[:4] != b"GMAD":
            raw = lzma.decompress(raw, lzma.FORMAT_ALONE)
            if raw[:4] != b"GMAD":
                raise ValueError(f"not a GMA archive: {path}")
        self._raw = raw
        off = 4
        version = raw[off]
        off += 1 + 16
        if version > 1:
            while True:
                s, off = read_cstr(raw, off)
                if not s:
                    break
        self.name, off = read_cstr(raw, off)
        _, off = read_cstr(raw, off)
        _, off = read_cstr(raw, off)
        off += 4
        self.entries: dict[str, tuple[int, int]] = {}
        pending: list[tuple[str, int]] = []
        while True:
            idx = int.from_bytes(raw[off : off + 4], "little")
            off += 4
            if idx == 0:
                break
            fname, off = read_cstr(raw, off)
            size = int.from_bytes(raw[off : off + 8], "little")
            off += 12
            pending.append((fname.lower().replace("\\", "/"), size))
        pos = off
        for fname, size in pending:
            self.entries[fname] = (pos, size)
            pos += size

    def read(self, name: str) -> bytes | None:
        hit = self.entries.get(name.lower().replace("\\", "/"))
        if hit is None:
            return None
        pos, size = hit
        return self._raw[pos : pos + size]

    def find_by_basename(self, basename: str) -> str | None:
        basename = basename.lower()
        for name in self.entries:
            if name.rsplit("/", 1)[-1] == basename:
                return name
        return None


class SourceIndex:
    """Ordered lookup across GMAs and VPKs by game-relative path."""

    def __init__(self) -> None:
        self._gmas: list[tuple[str, GmaArchive]] = []
        self._vpks: list[tuple[str, object]] = []

    def add_gma(self, label: str, gma: GmaArchive) -> None:
        self._gmas.append((label, gma))

    def add_vpk(self, label: str, pak) -> None:
        self._vpks.append((label, pak))

    def read(self, rel: str) -> tuple[bytes, str] | None:
        rel = rel.lower().replace("\\", "/")
        for label, gma in self._gmas:
            data = gma.read(rel)
            if data is not None:
                return data, label
        for label, pak in self._vpks:
            try:
                data = pak[rel].read()
                return data, label
            except KeyError:
                continue
        return None


def scan_pcf_materials(data: bytes) -> set[str]:
    refs: set[str] = set()
    for match in PRINTABLE_RUN_RE.finditer(data):
        s = match.group(0).decode("ascii", errors="replace").replace("\\", "/").lower()
        if s.endswith(".vmt"):
            refs.add(s.lstrip("/"))
    return refs


def vmt_texture_refs(vmt_text: str) -> set[str]:
    """Candidate texture paths from every $parameter value in a VMT.

    Values are only candidates -- the caller tries materials/<value>.vtf against the
    sources and silently drops non-hits (numbers, shader keywords, env_cubemap, ...).
    """
    refs: set[str] = set()
    for match in VMT_VALUE_RE.finditer(vmt_text):
        value = match.group(1).replace("\\", "/").strip().lower()
        if value.endswith(".vtf"):
            value = value[: -len(".vtf")]
        if not value or value.startswith("$"):
            continue
        if re.fullmatch(r"[a-z0-9_/\-\. ]+", value) and "." not in value.rsplit("/", 1)[-1]:
            refs.add(value)
    return refs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workshop-root", type=Path, required=True,
                        help=r"Steam workshop content dir for GMod, e.g. C:\Program Files (x86)\Steam\steamapps\workshop\content\4000")
    parser.add_argument("--gmod-root", type=Path, required=True,
                        help=r"GMod install dir, e.g. C:\Program Files (x86)\Steam\steamapps\common\GarrysMod")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        import vpk  # noqa: F401
    except ImportError:
        print("error: the 'vpk' python package is required (pip install vpk)", file=sys.stderr)
        return 2

    index = SourceIndex()
    gmas: dict[str, GmaArchive] = {}
    for wid in dict.fromkeys(list(PCF_SOURCES) + MATERIAL_GMA_IDS):
        folder = args.workshop_root / wid
        candidates = sorted(folder.glob("*.gma")) if folder.is_dir() else []
        if not candidates:
            if wid not in PCF_SOURCES:
                # Only a material source: what it supplied is already in the output tree.
                print(f"warning: workshop item {wid} has no GMA under {folder}; "
                      f"its materials stay as already extracted", file=sys.stderr)
                continue
            print(f"error: workshop item {wid} has no GMA under {folder} "
                  f"(subscribe to it in GMod first)", file=sys.stderr)
            return 2
        gma = GmaArchive(candidates[0])
        gmas[wid] = gma
        index.add_gma(f"gma:{wid}", gma)
        print(f"indexed {candidates[0].name}: {gma.name} ({len(gma.entries)} files)")

    vpk_dirs = [args.gmod_root / "garrysmod", args.gmod_root / "sourceengine"]
    import vpk as vpk_mod
    for vdir in vpk_dirs:
        for pak_path in sorted(vdir.glob("*_dir.vpk")) if vdir.is_dir() else []:
            try:
                index.add_vpk(f"vpk:{pak_path.name}", vpk_mod.open(str(pak_path)))
            except Exception as exc:  # corrupt/unreadable pak: skip, sources may still cover it
                print(f"warning: could not open {pak_path.name}: {exc}", file=sys.stderr)

    out_particles = args.output / "particles" / "filmmaker" / "modern"
    out_particles.mkdir(parents=True, exist_ok=True)

    material_refs: set[str] = set()
    for wid, pcf_names in PCF_SOURCES.items():
        for pcf_name in pcf_names:
            entry = gmas[wid].find_by_basename(pcf_name)
            data = gmas[wid].read(entry) if entry else None
            if data is None:
                print(f"error: {pcf_name} not found inside GMA {wid}", file=sys.stderr)
                return 2
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
        vmt_data, label = hit
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
            tex_data, tex_label = tex_hit
            tex_dst = args.output / Path(rel_vtf)
            if not tex_dst.is_file():
                tex_dst.parent.mkdir(parents=True, exist_ok=True)
                tex_dst.write_bytes(tex_data)
                staged_vtf += 1

    if missing:
        # Not fatal: a handful of sprites referenced by the FAS PCFs exist in NO local
        # source (verified 2026-07-03 across the GMAs, all GMod VPKs, and the user's whole
        # workshop library) -- GMod itself falls back for those pieces, and they belong to
        # systems the modern swap tables do not target (water/mortar/claymore variants).
        # After conversion the post-process removes renderers whose material never arrived,
        # so a missing sprite degrades to an absent piece, not a white quad.
        for rel in missing:
            print(f"warning: material not found in any source (renderer will be pruned): {rel}",
                  file=sys.stderr)

    # A staged VMT whose $basetexture VTF did not stage would convert into a white quad;
    # treat that (unlike a wholly missing VMT) as an error since the source clearly exists.
    unresolved = []
    for vmt_path in (args.output / "materials").rglob("*.vmt"):
        text = vmt_path.read_text(encoding="utf-8", errors="replace")
        base = re.search(r'"?\$basetexture"?\s+"?([^"\s]+)"?', text, re.IGNORECASE)
        if not base:
            continue
        tex_rel = "materials/" + base.group(1).replace("\\", "/").lower().strip("/") + ".vtf"
        if not (args.output / Path(tex_rel)).is_file():
            hit = index.read(tex_rel)
            if hit is not None:
                tex_dst = args.output / Path(tex_rel)
                tex_dst.parent.mkdir(parents=True, exist_ok=True)
                tex_dst.write_bytes(hit[0])
                staged_vtf += 1
            else:
                unresolved.append(f"{vmt_path.relative_to(args.output).as_posix()} -> {tex_rel}")
    if unresolved:
        for line in unresolved:
            print(f"error: $basetexture VTF unresolved: {line}", file=sys.stderr)
        return 2

    (args.output / "modern-missing-materials.txt").write_text(
        "\n".join(missing) + ("\n" if missing else ""), encoding="ascii")

    print(f"staged modern sources: {len(material_refs)} VMT refs ({staged_vmt} VMTs, "
          f"{staged_vtf} VTFs) under {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
