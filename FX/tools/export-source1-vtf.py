"""Export Source 1 VTF textures for source1import's material converter.

source1import intentionally expects textures to be pre-exported.  This keeps the
original pixels and frame order: single-frame VTFs become ``name.tga`` and
animated VTFs become ``name000.tga``, ``name001.tga``, which source1import then
packs into a Source 2 animation sheet.

Particle sprite SHEETS (the .sht resource embedded in a VTF) are exported the
way ValveResourceFormat reconstructs them for recompilation: per-frame images
sliced from the atlas + a ``name.mks`` sequence script + a ``name.vtex`` whose
input file is the .mks.  ResourceCompiler rebuilds the atlas and embeds the
Source 2 sequence block.  Without this, converted particles draw the WHOLE
atlas per sprite (the "gridded smoke" symptom): source1import's community
particles_import writes plain 2D vtex files and never converts sheet data
(its table literally marks 'sheet' as Discontinued), but it keeps any .vtex
that already exists -- so pre-creating them here wins.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path)
    return parser.parse_args()


def save_tga(image, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, format="TGA")


VTEX_MKS_TEMPLATE = """<!-- dmx encoding keyvalues2_noids 1 format vtex 1 -->
"CDmeVtex"
{
\t"m_inputTextureArray" "element_array"
\t[
\t\t"CDmeInputTexture"
\t\t{
\t\t\t"m_name" "string" "0"
\t\t\t"m_fileName" "string" "<>"
\t\t\t"m_colorSpace" "string" "srgb"
\t\t\t"m_typeString" "string" "2D"
\t\t}
\t]
\t"m_outputTypeString" "string" "2D"
\t"m_outputFormat" "string" "DXT5"
\t"m_textureOutputChannelArray" "element_array"
\t[
\t\t"CDmeTextureOutputChannel"
\t\t{
\t\t\t"m_inputTextureArray" "string_array"
\t\t\t[
\t\t\t\t"0"
\t\t\t]
\t\t\t"m_srcChannels" "string" "rgba"
\t\t\t"m_dstChannels" "string" "rgba"
\t\t\t"m_mipAlgorithm" "CDmeImageProcessor"
\t\t\t{
\t\t\t\t"m_algorithm" "string" ""
\t\t\t\t"m_stringArg" "string" ""
\t\t\t\t"m_vFloat4Arg" "vector4" "0 0 0 0"
\t\t\t}
\t\t\t"m_outputColorSpace" "string" "srgb"
\t\t}
\t]
}"""


def parse_sheet_lenient(source: Path):
    """Parse a VTF's embedded particle-sheet resource, tolerating the duplicate
    sequence ids some mod VTFs carry (srctools raises on those). Returns
    ``{seq_num: (clamp, [(duration, (u0, v0, u1, v1)), ...])}`` or None."""
    data = source.read_bytes()
    if data[:4] != b"VTF\0" or len(data) < 80:
        return None
    ver_minor = struct.unpack_from("<I", data, 8)[0]
    if ver_minor < 3:
        return None  # pre-7.3 VTFs have no resource dictionary
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
            # Version 1 stores 4 sampled rects per frame; they are duplicates in
            # practice (ValveResourceFormat extracts only the first one too).
            offset += 16 if version == 0 else 64
            frames.append((duration, rect))
        sequences.setdefault(seq_num, (bool(clamp), frames))
    return sequences or None


def sheet_is_trivial(sequences, width: int, height: int) -> bool:
    """One sequence of one frame covering the whole texture = a plain 2D image."""
    if len(sequences) != 1:
        return False
    ((_clamp, frames),) = sequences.values()
    if len(frames) != 1:
        return False
    _duration, (u0, v0, u1, v1) = frames[0]
    return (
        round(u0 * width) <= 0 and round(v0 * height) <= 0
        and round(u1 * width) >= width and round(v1 * height) >= height
    )


def export_sheet(atlas, sequences, destination: Path, content_root: Path) -> bool:
    """Write per-frame slices + .mks + a .vtex whose input is the .mks."""
    width, height = atlas.size
    rect_names: dict[tuple[int, int, int, int], str] = {}
    lines = []
    # ResourceCompiler requires sequences numbered 0..N-1 in order. Some mod
    # sheets start at 1 or have gaps; pad the holes with a duplicate of the
    # first real sequence so the ORIGINAL sequence ids the particles reference
    # keep pointing at the right frames.
    first_real = sequences[min(sequences)]
    for seq_num in range(max(sequences) + 1):
        clamp, frames = sequences.get(seq_num, first_real)
        lines.append("")
        lines.append(f"sequence {seq_num}")
        if not clamp:
            lines.append("LOOP")
        for frame_ind, (duration, (u0, v0, u1, v1)) in enumerate(frames):
            box = (
                max(0, min(round(u0 * width), width)),
                max(0, min(round(v0 * height), height)),
                max(0, min(round(u1 * width), width)),
                max(0, min(round(v1 * height), height)),
            )
            if box[2] <= box[0] or box[3] <= box[1]:
                continue
            name = rect_names.get(box)
            if name is None:
                suffix = f"_seq{seq_num}" if len(frames) == 1 else f"_seq{seq_num}_{frame_ind}"
                name = f"{destination.name}{suffix}.tga"
                rect_names[box] = name
                save_tga(atlas.crop(box), destination.with_name(name))
            if clamp and duration == 0:
                duration = 1
            lines.append(f"frame {name} {duration:g}")
    if not rect_names:
        return False
    mks_path = destination.with_suffix(".mks")
    mks_path.write_text(
        "// Reconstructed from the VTF's embedded particle sheet by export-source1-vtf.py\n"
        + "\n".join(lines) + "\n",
        encoding="ascii",
    )
    vtex_path = destination.with_suffix(".vtex")
    mks_resource = mks_path.relative_to(content_root).as_posix()
    vtex_path.write_text(VTEX_MKS_TEMPLATE.replace("<>", mks_resource, 1), encoding="ascii")
    return True


def export_texture(source: Path, destination: Path, content_root: Path) -> tuple[int, str, bool]:
    from srctools.vtf import VTF, VTFFlags

    sheeted = False
    try:
        with source.open("rb") as stream:
            texture = VTF.read(stream)
            if VTFFlags.ENVMAP in texture.flags:
                return 0, "envmap", False
            texture.load()
        atlas = texture.get(frame=0).to_PIL()
        if texture.frame_count == 1:
            save_tga(atlas, destination.with_suffix(".tga"))
        else:
            for frame in range(texture.frame_count):
                frame_path = destination.with_name(f"{destination.name}{frame:03}").with_suffix(".tga")
                save_tga(texture.get(frame=frame).to_PIL(), frame_path)
        frame_count = texture.frame_count
        decoder = "srctools"
    except ValueError as error:
        # Some mod VTFs contain duplicate Source 1 sprite-sheet sequence metadata.
        # vtf2img ignores that metadata and decodes the original atlas pixels.
        if "Duplicate sequence number" not in str(error):
            raise
        from vtf2img import Parser

        atlas = Parser(str(source)).get_image()
        save_tga(atlas, destination.with_suffix(".tga"))
        frame_count = 1
        decoder = "vtf2img"

    # Multi-frame VTFs already export as frame lists that source1import packs
    # itself; the sprite-sheet path handles single-frame ATLAS textures.
    if frame_count == 1:
        sequences = parse_sheet_lenient(source)
        if sequences and not sheet_is_trivial(sequences, *atlas.size):
            sheeted = export_sheet(atlas, sequences, destination, content_root)
    return frame_count, decoder, sheeted


def _export_one(job: tuple[str, str, str, str]) -> dict:
    """Process-pool worker: export one VTF and return a picklable result row."""
    source, destination, content_root, relative = job
    try:
        frames, decoder, sheeted = export_texture(
            Path(source), Path(destination), Path(content_root)
        )
        return {"relative": relative, "frames": frames, "decoder": decoder, "sheeted": sheeted}
    except Exception as error:
        return {"relative": relative, "error": repr(error)}


def main() -> int:
    args = parse_args()
    source_root = args.input.resolve()
    output_root = args.output.resolve()
    if not source_root.is_dir():
        raise SystemExit(f"VTF input directory not found: {source_root}")
    # The .mks path inside a .vtex is resolved against the content root, which
    # is the parent of the exported materials/ folder.
    content_root = output_root.parent

    summary = {
        "sourceRoot": str(source_root),
        "outputRoot": str(output_root),
        "textures": 0,
        "frames": 0,
        "sheets": 0,
        "fallbackTextures": 0,
        "skippedEnvmaps": [],
        "errors": [],
    }

    jobs = [
        (
            str(source),
            str(output_root / source.relative_to(source_root).with_suffix("")),
            str(content_root),
            source.relative_to(source_root).as_posix(),
        )
        for source in sorted(source_root.rglob("*.vtf"))
    ]

    # Each VTF is independent, so decode/re-encode them across all cores.
    from concurrent.futures import ProcessPoolExecutor

    with ProcessPoolExecutor() as pool:
        results = list(pool.map(_export_one, jobs, chunksize=8))

    for result in results:
        if "error" in result:  # report every failed source, then fail the run
            summary["errors"].append({"path": result["relative"], "error": result["error"]})
            continue
        if result["decoder"] == "envmap":
            summary["skippedEnvmaps"].append(result["relative"])
            continue
        summary["textures"] += 1
        summary["frames"] += result["frames"]
        if result["sheeted"]:
            summary["sheets"] += 1
        if result["decoder"] == "vtf2img":
            summary["fallbackTextures"] += 1

    if args.manifest:
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(json.dumps(summary, indent=2), encoding="utf-8")

    print(
        f"Exported {summary['textures']} VTF textures / {summary['frames']} frames, "
        f"{summary['sheets']} sprite sheets as .mks "
        f"({summary['fallbackTextures']} duplicate-sheet fallbacks)."
    )
    if summary["skippedEnvmaps"]:
        print(
            "Skipped unsupported cubemap VTFs (the caller must validate references): "
            + ", ".join(summary["skippedEnvmaps"])
        )
    if summary["errors"]:
        for item in summary["errors"]:
            print(f"ERROR {item['path']}: {item['error']}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
