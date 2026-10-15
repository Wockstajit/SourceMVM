"""Export every texture referenced when shooting a USP (On/Less pack) to PNG previews.

Walks the built Povarehok particle closure (muzzle flash, tracer, shell, sustained
smoke, concrete impacts), resolves each m_hTexture / m_hNormalTexture to a Source 1
VTF in the reference mod or FX/sources, and writes PNGs under
Automation/output/usp-fx-preview/.

Usage (from repo root):
    python Automation/tools/export-usp-fx-textures.py
"""

from __future__ import annotations

import json
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD_PARTICLES = (
    REPO
    / "build/fx/fx-source1import/source2/content/source_mvm_fx/particles/filmmaker/povarehok/regular"
)
OUTPUT = REPO / "Automation/output/usp-fx-preview"

# Roots searched in order; first match wins.
VTF_SEARCH_ROOTS = [
    REPO
    / "reference/csgo effect mod/p_betterparticlesmod_classic updated_c057b/p_betterparticlesmod_classic/materials",
    REPO / "FX/sources/modern-warfare-gmod/materials",
    REPO / "build/fx/fx-source1import/source1/materials",
]

# Top-level systems swapped for USP shooting (ParticleFxRules.cpp).
USP_ROOT_VPCFS = [
    BUILD_PARTICLES / "weapons/cs_weapon_fx/weapon_muzzle_flash_pistol.vpcf",
    BUILD_PARTICLES / "weapons/cs_weapon_fx/weapon_muzzle_flash_pistol_fp.vpcf",
    BUILD_PARTICLES / "weapons/cs_weapon_fx/weapon_tracers_pistol.vpcf",
    BUILD_PARTICLES / "weapons/cs_weapon_fx/weapon_shell_casing_9mm.vpcf",
    BUILD_PARTICLES / "weapons/cs_weapon_fx/weapon_muzzle_smoke.vpcf",
    BUILD_PARTICLES / "weapons/cs_weapon_fx/weapon_muzzle_smoke_long.vpcf",
    BUILD_PARTICLES / "impact_fx/impact_concrete.vpcf",
]

CHILD_RE = re.compile(r'm_ChildRef\s*=\s*resource:"([^"]+)"')
TEXTURE_RE = re.compile(r'm_h(?:Normal)?Texture\s*=\s*resource:"([^"]+)"')


def parse_sheet_lenient(source: Path):
    data = source.read_bytes()
    if data[:4] != b"VTF\0" or len(data) < 80:
        return None
    ver_minor = struct.unpack_from("<I", data, 8)[0]
    if ver_minor < 3:
        return None
    num_resources = struct.unpack_from("<I", data, 68)[0]
    sheet = None
    for i in range(num_resources):
        tag, _flags, offset = struct.unpack_from("<3sBI", data, 80 + 8 * i)
        if tag == b"\x10\x00\x00":
            size = struct.unpack_from("<I", data, offset)[0]
            sheet = data[offset + 4 : offset + 4 + size]
            break
    if sheet is None:
        return None
    version, sequence_count = struct.unpack_from("<II", sheet)
    if version > 1:
        return None
    offset = 8
    sequences = {}
    for _ in range(sequence_count):
        seq_num, clamp, frame_count, _total = struct.unpack_from("<Ixxx?If", sheet, offset)
        offset += 16
        frames = []
        for _ in range(frame_count):
            (duration,) = struct.unpack_from("<f", sheet, offset)
            offset += 4
            rect = struct.unpack_from("<4f", sheet, offset)
            offset += 16 if version == 0 else 64
            frames.append((duration, rect))
        sequences.setdefault(seq_num, (bool(clamp), frames))
    return sequences or None


def sheet_is_trivial(sequences, width: int, height: int) -> bool:
    if len(sequences) != 1:
        return False
    ((_clamp, frames),) = sequences.values()
    if len(frames) != 1:
        return False
    _duration, (u0, v0, u1, v1) = frames[0]
    return (
        round(u0 * width) <= 0
        and round(v0 * height) <= 0
        and round(u1 * width) >= width
        and round(v1 * height) >= height
    )


def load_vtf(path: Path):
    from srctools.vtf import VTF, VTFFlags

    try:
        with path.open("rb") as stream:
            texture = VTF.read(stream)
            if VTFFlags.ENVMAP in texture.flags:
                return None, 0, "envmap"
            texture.load()
        return texture, texture.frame_count, "srctools"
    except ValueError as error:
        if "Duplicate sequence number" not in str(error):
            raise
        from vtf2img import Parser

        atlas = Parser(str(path)).get_image()
        return atlas, 1, "vtf2img-atlas"


def resolve_vtf(vtex_resource: str) -> Path | None:
    # materials/foo/bar.vtex -> foo/bar.vtf (strip materials/ prefix)
    rel = vtex_resource.replace("\\", "/")
    if rel.startswith("materials/"):
        rel = rel[len("materials/") :]
    if rel.endswith(".vtex"):
        rel = rel[: -len(".vtex")] + ".vtf"
    name = Path(rel).name
    for root in VTF_SEARCH_ROOTS:
        if not root.is_dir():
            continue
        exact = root / rel
        if exact.is_file():
            return exact
        # Fall back: any file with matching basename under this root.
        hits = list(root.rglob(name))
        if hits:
            return hits[0]
    return None


def collect_vpcf_closure() -> tuple[set[Path], dict[str, set[str]]]:
    visited: set[Path] = set()
    texture_users: dict[str, set[str]] = defaultdict(set)

    def walk(vpcf_path: Path, origin: str) -> None:
        if not vpcf_path.is_file():
            return
        key = vpcf_path.resolve()
        if key in visited:
            return
        visited.add(key)
        text = vpcf_path.read_text(encoding="utf-8", errors="replace")
        label = vpcf_path.stem
        for match in TEXTURE_RE.finditer(text):
            texture_users[match.group(1)].add(f"{origin}:{label}")
        for match in CHILD_RE.finditer(text):
            child_res = match.group(1)
            # Child refs are content-relative particle paths.
            child_path = (
                REPO
                / "build/fx/fx-source1import/source2/content/source_mvm_fx"
                / child_res
            )
            walk(child_path, origin)

    for root in USP_ROOT_VPCFS:
        if not root.is_file():
            print(f"WARN: missing root vpcf: {root}", file=sys.stderr)
            continue
        origin = root.stem
        walk(root, origin)

    return visited, texture_users


def safe_name(vtex_resource: str) -> str:
    return vtex_resource.replace("\\", "/").removeprefix("materials/").replace("/", "__")


def export_texture(vtex_resource: str, dest_dir: Path) -> dict:
    vtf_path = resolve_vtf(vtex_resource)
    base = safe_name(vtex_resource)
    result = {
        "vtex": vtex_resource,
        "vtf": str(vtf_path) if vtf_path else None,
        "pngs": [],
        "error": None,
    }
    if vtf_path is None:
        result["error"] = "vtf_not_found"
        return result

    try:
        loaded, frame_count, decoder = load_vtf(vtf_path)
        if loaded is None:
            result["error"] = decoder
            return result

        if decoder == "vtf2img-atlas":
            atlas = loaded
            out = dest_dir / f"{base}.png"
            atlas.save(out, format="PNG")
            result["pngs"].append(str(out.relative_to(REPO)))
            return result

        texture = loaded
        atlas = texture.get(frame=0).to_PIL()
        width, height = atlas.size

        if frame_count > 1:
            sheet_path = dest_dir / f"{base}_sheet.png"
            atlas.save(sheet_path, format="PNG")
            result["pngs"].append(str(sheet_path.relative_to(REPO)))
            for frame in range(frame_count):
                frame_path = dest_dir / f"{base}_frame_{frame:02d}.png"
                texture.get(frame=frame).to_PIL().save(frame_path, format="PNG")
                result["pngs"].append(str(frame_path.relative_to(REPO)))
            return result

        sequences = parse_sheet_lenient(vtf_path)
        if sequences and not sheet_is_trivial(sequences, width, height):
            sheet_path = dest_dir / f"{base}_sheet.png"
            atlas.save(sheet_path, format="PNG")
            result["pngs"].append(str(sheet_path.relative_to(REPO)))
            frame_index = 0
            seen_boxes: set[tuple[int, int, int, int]] = set()
            for _seq_num in sorted(sequences):
                _clamp, frames = sequences[_seq_num]
                for _duration, (u0, v0, u1, v1) in frames:
                    box = (
                        max(0, min(round(u0 * width), width)),
                        max(0, min(round(v0 * height), height)),
                        max(0, min(round(u1 * width), width)),
                        max(0, min(round(v1 * height), height)),
                    )
                    if box[2] <= box[0] or box[3] <= box[1] or box in seen_boxes:
                        continue
                    seen_boxes.add(box)
                    frame_path = dest_dir / f"{base}_frame_{frame_index:02d}.png"
                    atlas.crop(box).save(frame_path, format="PNG")
                    result["pngs"].append(str(frame_path.relative_to(REPO)))
                    frame_index += 1
            return result

        out = dest_dir / f"{base}.png"
        atlas.save(out, format="PNG")
        result["pngs"].append(str(out.relative_to(REPO)))
        return result
    except Exception as error:  # noqa: BLE001 — report per-texture, keep going
        result["error"] = repr(error)
        return result


def main() -> int:
    if not BUILD_PARTICLES.is_dir():
        print(f"Build particles not found: {BUILD_PARTICLES}", file=sys.stderr)
        print("Run build.bat or convert the FX pack first.", file=sys.stderr)
        return 1

    visited, texture_users = collect_vpcf_closure()
    OUTPUT.mkdir(parents=True, exist_ok=True)

    manifest = {
        "outputDir": str(OUTPUT.relative_to(REPO)),
        "vpcfCount": len(visited),
        "textureCount": len(texture_users),
        "textures": [],
        "missing": [],
    }

    for vtex_resource in sorted(texture_users):
        entry = export_texture(vtex_resource, OUTPUT)
        entry["usedBy"] = sorted(texture_users[vtex_resource])
        manifest["textures"].append(entry)
        if entry.get("error"):
            manifest["missing"].append(vtex_resource)

    manifest_path = OUTPUT / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    smoke_keys = (
        "smoke",
        "vistasmoke",
        "steam",
        "fulldust",
        "insandstorm",
        "fas_smoke",
        "beam_smoke",
    )
    lines = [
        "USP FX texture preview export",
        f"  {manifest['vpcfCount']} particle systems, {manifest['textureCount']} textures",
        "",
        "SMOKE (start here for red-smoke investigation)",
    ]
    for entry in manifest["textures"]:
        key = entry["vtex"].lower()
        if not any(s in key for s in smoke_keys):
            continue
        preview = entry["pngs"][0] if entry["pngs"] else f"MISSING ({entry.get('error')})"
        users = ", ".join(entry["usedBy"])
        lines.append(f"  {entry['vtex']}")
        lines.append(f"    -> {preview}")
        lines.append(f"    used by: {users}")
    lines.append("")
    lines.append("FLASH / SPARKS / GLOW / TRACER / DEBRIS / SHELL")
    for entry in manifest["textures"]:
        key = entry["vtex"].lower()
        if any(s in key for s in smoke_keys):
            continue
        preview = entry["pngs"][0] if entry["pngs"] else f"MISSING ({entry.get('error')})"
        lines.append(f"  {entry['vtex']} -> {preview}")
    (OUTPUT / "INDEX.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    exported = sum(1 for t in manifest["textures"] if t.get("pngs"))
    png_count = sum(len(t.get("pngs", [])) for t in manifest["textures"])
    print(f"Walked {manifest['vpcfCount']} vpcf files, {manifest['textureCount']} unique textures.")
    print(f"Exported {exported} textures ({png_count} PNG files) to {OUTPUT.relative_to(REPO)}")
    if manifest["missing"]:
        print(
            f"Note: {len(manifest['missing'])} texture(s) not in mod sources "
            f"(likely vanilla CS2 assets) — see manifest.json",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
