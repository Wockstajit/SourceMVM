#!/usr/bin/env python3
"""Static probes for CS2 cosmetics/composite-material reverse engineering.

This is intentionally narrow: it scans a local client.dll for strings and
RIP-relative xrefs, then can disassemble around specific VAs/RVAs. It does not
modify game files.
"""

from __future__ import annotations

import sys
import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import capstone
import pefile

# Where CS2 actually is on this machine. See Automation/lib/steam_paths.py -- these defaults
# used to be a hand-written list of drive letters headed by the author's own Steam library.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from steam_paths import cs2_game_dirs, cs2_install_dirs  # noqa: E402


_INSTALLS = cs2_install_dirs()
DEFAULT_CLIENT_DLL = (
    str(_INSTALLS[0] / "game" / "csgo" / "bin" / "win64" / "client.dll")
    if _INSTALLS else ""
)


@dataclass(frozen=True)
class Section:
    name: str
    va: int
    rva: int
    file_offset: int
    raw_size: int
    virtual_size: int


class PeImage:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        self.pe = pefile.PE(str(path), fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.sections = [
            Section(
                sec.Name.rstrip(b"\0").decode("ascii", errors="replace"),
                self.base + sec.VirtualAddress,
                sec.VirtualAddress,
                sec.PointerToRawData,
                sec.SizeOfRawData,
                sec.Misc_VirtualSize,
            )
            for sec in self.pe.sections
        ]

    def section(self, name: str) -> Section:
        for sec in self.sections:
            if sec.name == name:
                return sec
        raise KeyError(name)

    def rva_from_file_offset(self, file_offset: int) -> int:
        return self.pe.get_rva_from_offset(file_offset)

    def file_offset_from_va(self, va: int) -> int:
        return self.pe.get_offset_from_rva(va - self.base)

    def bytes_at_va(self, va: int, size: int) -> bytes:
        off = self.file_offset_from_va(va)
        return self.data[off : off + size]

    def find_ascii(self, needle: str) -> list[int]:
        raw = needle.encode("ascii")
        hits: list[int] = []
        start = 0
        while True:
            idx = self.data.find(raw, start)
            if idx < 0:
                return hits
            hits.append(self.base + self.rva_from_file_offset(idx))
            start = idx + 1

    def find_qword_refs(self, target_va: int) -> list[int]:
        raw = int(target_va).to_bytes(8, "little")
        refs: list[int] = []
        start = 0
        while True:
            idx = self.data.find(raw, start)
            if idx < 0:
                return refs
            refs.append(self.base + self.rva_from_file_offset(idx))
            start = idx + 1

    def text_bytes(self) -> tuple[int, bytes]:
        text = self.section(".text")
        return text.va, self.data[text.file_offset : text.file_offset + text.raw_size]


def make_disassembler() -> capstone.Cs:
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    return md


def rip_xrefs_to(image: PeImage, targets: Iterable[int]) -> dict[int, list[capstone.CsInsn]]:
    target_set = set(targets)
    out = {target: [] for target in target_set}
    text_va, text_data = image.text_bytes()
    md = make_disassembler()
    for ins in md.disasm(text_data, text_va):
        for op in ins.operands:
            if op.type != capstone.x86.X86_OP_MEM:
                continue
            if op.mem.base != capstone.x86.X86_REG_RIP:
                continue
            dest = ins.address + ins.size + op.mem.disp
            if dest in target_set:
                out[dest].append(ins)
    return out


def print_disasm(image: PeImage, va: int, before: int, size: int) -> None:
    start = va - before
    data = image.bytes_at_va(start, size)
    md = make_disassembler()
    for ins in md.disasm(data, start):
        marker = "=>" if ins.address == va else "  "
        print(f"{marker} {ins.address:016x}: {ins.mnemonic:8} {ins.op_str}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--client-dll", default=DEFAULT_CLIENT_DLL)
    ap.add_argument("--string", action="append", default=[])
    ap.add_argument("--xref-string", action="append", default=[])
    ap.add_argument("--ptr-xref-string", action="append", default=[])
    ap.add_argument("--disasm-va", action="append", default=[])
    ap.add_argument("--before", type=lambda x: int(x, 0), default=0x80)
    ap.add_argument("--size", type=lambda x: int(x, 0), default=0x200)
    args = ap.parse_args()

    image = PeImage(Path(args.client_dll))
    print(f"image={image.path}")
    print(f"base=0x{image.base:x}")

    for text in args.string:
        hits = image.find_ascii(text)
        print(f"string {text!r}: {len(hits)} hit(s)")
        for va in hits:
            print(f"  va=0x{va:x} rva=0x{va - image.base:x}")

    if args.xref_string:
        targets: list[tuple[str, int]] = []
        for text in args.xref_string:
            for va in image.find_ascii(text):
                targets.append((text, va))
        xrefs = rip_xrefs_to(image, [va for _, va in targets])
        for text, va in targets:
            refs = xrefs.get(va, [])
            print(f"xrefs to {text!r} @ 0x{va:x}: {len(refs)}")
            for ins in refs:
                print(f"  {ins.address:016x}: {ins.mnemonic:8} {ins.op_str}")

    if args.ptr_xref_string:
        for text in args.ptr_xref_string:
            for va in image.find_ascii(text):
                refs = image.find_qword_refs(va)
                print(f"qword refs to {text!r} @ 0x{va:x}: {len(refs)}")
                for ref in refs[:64]:
                    print(f"  va=0x{ref:x} rva=0x{ref - image.base:x}")
                xrefs = rip_xrefs_to(image, refs)
                for ref in refs:
                    code_refs = xrefs.get(ref, [])
                    if not code_refs:
                        continue
                    print(f"  code xrefs to qword ref @ 0x{ref:x}: {len(code_refs)}")
                    for ins in code_refs:
                        print(f"    {ins.address:016x}: {ins.mnemonic:8} {ins.op_str}")

    for raw_va in args.disasm_va:
        va = int(raw_va, 0)
        if va < image.base:
            va += image.base
        print(f"disasm around 0x{va:x}:")
        print_disasm(image, va, args.before, args.size)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
