"""Packs the compiled FX pack into ONE single-file VPK (version 2), the format CS2's own packs use.

    py -3 FX/tools/pack-fx-vpk.py --input FX/compiled/source_mvm_fx --output <dir>/source_mvm_fx.vpk

Every file under --input goes in except the loose ones CS2 must read off disk (gameinfo.gi, and
the cfg/ and ui_override/ folders a staged pack may carry). The hook mounts the archive as a GAME
search path beside the pack folder (AfxHookSource2/main.cpp), so the particles, materials and
models resolve from it exactly as they did from loose files.

Layout written (as ValvePak reads it):
    header       signature 0x55aa1234, version 2, tree size, data size, archive-MD5 size 0,
                 other-MD5 size 48, signature size 0
    tree         extension -> path -> file name, each list ended by "", each file followed by
                 crc32, preload 0, archive 0x7fff (this file), offset, length, terminator 0xffff
    data         every file's bytes, in tree order
    other MD5    md5(tree), md5(archive-MD5 section = empty), md5(header..through the first 32
                 bytes of this section)
Verify with Source2Viewer-CLI -i <file> --vpk_verify.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path

LOOSE_FILES = {"gameinfo.gi"}
LOOSE_DIRS = {"cfg", "ui_override"}


def collect(root: Path) -> dict[str, dict[str, dict[str, Path]]]:
    """extension -> directory -> name -> file. Paths are lower-case with forward slashes."""
    tree: dict[str, dict[str, dict[str, Path]]] = {}
    for file in sorted(root.rglob("*")):
        if not file.is_file():
            continue
        rel = file.relative_to(root)
        if rel.parts[0].lower() in LOOSE_DIRS or (len(rel.parts) == 1 and rel.name.lower() in LOOSE_FILES):
            continue
        rel_posix = rel.as_posix().lower()
        directory, _, filename = rel_posix.rpartition("/")
        stem, dot, ext = filename.rpartition(".")
        if not dot:
            stem, ext = filename, ""
        tree.setdefault(ext or " ", {}).setdefault(directory or " ", {})[stem] = file
    return tree


def build(root: Path, output: Path) -> tuple[int, int]:
    tree = collect(root)
    tree_bytes = bytearray()
    data = bytearray()
    count = 0
    for ext in sorted(tree):
        tree_bytes += ext.encode("utf-8") + b"\0"
        for directory in sorted(tree[ext]):
            tree_bytes += directory.encode("utf-8") + b"\0"
            for stem in sorted(tree[ext][directory]):
                payload = tree[ext][directory][stem].read_bytes()
                tree_bytes += stem.encode("utf-8") + b"\0"
                tree_bytes += struct.pack("<IHHIIH", zlib.crc32(payload) & 0xFFFFFFFF, 0, 0x7FFF,
                                          len(data), len(payload), 0xFFFF)
                data += payload
                count += 1
            tree_bytes += b"\0"
        tree_bytes += b"\0"
    tree_bytes += b"\0"

    archive_md5 = b""
    header = struct.pack("<7I", 0x55AA1234, 2, len(tree_bytes), len(data), len(archive_md5), 48, 0)
    tree_md5 = hashlib.md5(tree_bytes).digest()
    archive_md5_md5 = hashlib.md5(archive_md5).digest()
    whole = hashlib.md5()
    for part in (header, tree_bytes, data, archive_md5, tree_md5, archive_md5_md5):
        whole.update(part)

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as out:
        for part in (header, tree_bytes, data, archive_md5, tree_md5, archive_md5_md5, whole.digest()):
            out.write(part)
    return count, output.stat().st_size


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--input", type=Path, required=True, help="the compiled pack folder (source_mvm_fx)")
    parser.add_argument("--output", type=Path, required=True, help="the .vpk to write")
    args = parser.parse_args()
    if not (args.input / "particles").is_dir():
        print(f"error: {args.input} has no particles/ folder; not a compiled FX pack", file=sys.stderr)
        return 2
    count, size = build(args.input.resolve(), args.output.resolve())
    print(f"packed {count} files into {args.output} ({size / 1048576:.1f} MB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
