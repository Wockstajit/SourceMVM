#!/usr/bin/env python3
"""Reverse-engineer the CS2 scenesystem LightBinner shadow gates.

Standalone research tool. It shares no build/authoring logic with the other
automation tools; it only reads the shipped CS2 binaries.

Pipeline:

  1. Locate the ConVar name strings for the shadow-related gates.
  2. Follow each name string to its registration xref, and from there to the
     ConVar global object that the constructor initialises.
  3. Find every code site that reads that ConVar global.
  4. Recover the enclosing function of each read site, so the baked / mixed /
     live seam can be read off a function-vs-gate cross table instead of
     guessed from cvar names.

Modes:
  --inventory   dump every shadow/lighting-ish string with its VA
  --xref        full name-string -> object -> read-site -> function pass
  --disasm VA   print a disassembly window around one address

Example:
  python Automation/tools/re_lightbinner_shadows.py --xref --json out.json
"""

from __future__ import annotations

from pathlib import Path
import argparse
import json
import os
import re
import sys
from collections import defaultdict

# Where CS2 actually is on this machine. See Automation/lib/steam_paths.py -- these defaults
# used to be a hand-written list of drive letters headed by the author's own Steam library.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from steam_paths import cs2_game_dirs, cs2_install_dirs  # noqa: E402

try:
    import numpy as np
except ImportError:  # pragma: no cover
    sys.exit("numpy is required: pip install numpy")

try:
    import pefile
except ImportError:  # pragma: no cover
    sys.exit("pefile is required: pip install pefile")

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM, x86
except ImportError:  # pragma: no cover
    sys.exit("capstone is required: pip install capstone")


# Kept as a tail for the case where Steam itself cannot be located.
_CONVENTIONAL = [
    "C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game",
]
DEFAULT_GAME_DIRS = cs2_game_dirs() + _CONVENTIONAL

# Gates relevant to the baked-vs-live shadow seam.
TARGET_CVARS = [
    "lb_enable_baked_shadows",
    "lb_mixed_shadows",
    "lb_enable_fog_mixed_shadows",
    "lb_shadow_map_cull_empty_mixed",
    "lb_shadow_map_culling",
    "lb_precomputed_shadowmap_enable",
    "lb_enable_shadow_casting",
    "lb_enable_stationary_lights",
    "lb_enable_dynamic_lights",
    "lb_enable_sunlight",
    "lb_enable_lights",
    "lb_allow_shadow_rotation",
    "lb_csm_override_staticgeo_cascades",
    "lb_csm_override_staticgeo_cascades_value",
    "lb_csm_override_staticgeo_cascades_animated_verts",
    "lb_sun_csm_size_cull_threshold_texels",
    "sparseshadowtree_enable_rendering",
    "sparseshadowtree_cascade_mask",
    "sparseshadowtree_unpack_direct_to_shadow_atlas",
    "sparseshadowtree_disable_add_layers",
    "csm_max_num_cascades_override",
    "csm_sst_max_visible_dist",
]

INVENTORY_PREFIXES = (
    "lb_",
    "csm_",
    "sparseshadowtree",
    "shadow",
    "baked",
    "mixed",
    "precomputed",
    "lightmap",
    "irradiance",
)

STRING_RE = re.compile(rb"[\x20-\x7e]{4,160}")


class Image:
    """A loaded PE with VA/offset translation plus disassembly helpers."""

    def __init__(self, path: str):
        self.path = path
        self.name = os.path.basename(path)
        pe = pefile.PE(path, fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.data = pe.__data__
        self.sections = []
        for section in pe.sections:
            name = section.Name.rstrip(b"\x00").decode("ascii", "replace")
            self.sections.append(
                {
                    "name": name,
                    "va": self.base + section.VirtualAddress,
                    "vsize": max(section.Misc_VirtualSize, section.SizeOfRawData),
                    "raw": section.PointerToRawData,
                    "rawsize": section.SizeOfRawData,
                    "exec": bool(section.Characteristics & 0x20000000),
                    "write": bool(section.Characteristics & 0x80000000),
                }
            )
        self.text = next(s for s in self.sections if s["name"] == ".text")
        self.md = Cs(CS_ARCH_X86, CS_MODE_64)
        self.md.detail = True

    def va_to_off(self, va: int):
        for s in self.sections:
            if s["va"] <= va < s["va"] + s["vsize"]:
                delta = va - s["va"]
                if delta < s["rawsize"]:
                    return s["raw"] + delta
        return None

    def section_of(self, va: int):
        for s in self.sections:
            if s["va"] <= va < s["va"] + s["vsize"]:
                return s["name"]
        return None

    def text_bytes(self):
        s = self.text
        return self.data[s["raw"] : s["raw"] + s["rawsize"]], s["va"]

    def cstring_at(self, va: int, limit: int = 160):
        off = self.va_to_off(va)
        if off is None:
            return None
        end = self.data.find(b"\x00", off, off + limit)
        if end < 0:
            return None
        try:
            return self.data[off:end].decode("ascii")
        except UnicodeDecodeError:
            return None

    def find_string_vas(self, needle: str):
        pattern = needle.encode() + b"\x00"
        out = []
        for s in self.sections:
            if s["exec"]:
                continue
            blob = self.data[s["raw"] : s["raw"] + s["rawsize"]]
            start = 0
            while True:
                i = blob.find(pattern, start)
                if i < 0:
                    break
                if i == 0 or blob[i - 1] == 0:
                    out.append(s["va"] + i)
                start = i + 1
        return out

    def inventory_strings(self, prefixes):
        found = {}
        for s in self.sections:
            if s["exec"]:
                continue
            blob = self.data[s["raw"] : s["raw"] + s["rawsize"]]
            for m in STRING_RE.finditer(blob):
                text = m.group().decode("ascii")
                low = text.lower()
                if any(p in low for p in prefixes):
                    found.setdefault(text, s["va"] + m.start())
        return found


def find_rip_refs(image: Image, targets):
    """Find every instruction in .text with a rip-relative operand in `targets`.

    A linear capstone sweep is not usable here: it desynchronises on the first
    jump table or data blob and silently stops. Instead scan every byte offset
    for a disp32 that would resolve to a wanted VA, then confirm the candidate
    by decoding backwards until an instruction ends exactly on that disp field.
    """
    blob, code_va = image.text_bytes()
    wanted = np.fromiter(sorted(targets), dtype=np.int64)
    if wanted.size == 0:
        return []

    arr = np.frombuffer(blob, dtype=np.uint8)
    n = arr.size
    candidates = []
    for phase in range(4):
        end = n - ((n - phase) % 4)
        if end <= phase:
            continue
        view = np.frombuffer(blob[phase:end], dtype="<i4")
        idx = np.arange(view.size, dtype=np.int64) * 4 + phase
        # A rip operand's disp32 is the last 4 bytes of the instruction unless
        # an immediate follows; handle the common (no-immediate) case here and
        # let the backward decode below catch the rest.
        resolved = code_va + idx + 4 + view.astype(np.int64)
        hit = np.isin(resolved, wanted)
        for off, tgt in zip(idx[hit].tolist(), resolved[hit].tolist()):
            candidates.append((off, tgt))

    results = []
    seen = set()
    for off, tgt in candidates:
        disp_end_va = code_va + off + 4
        for back in range(3, 16):
            start_off = off - back
            if start_off < 0:
                continue
            start_va = code_va + start_off
            chunk = blob[start_off : start_off + 20]
            try:
                insn = next(image.md.disasm(chunk, start_va, count=1))
            except StopIteration:
                continue
            if insn.address + insn.size < disp_end_va:
                continue
            matched = False
            for op in insn.operands:
                if op.type != CS_OP_MEM or op.mem.base != x86.X86_REG_RIP:
                    continue
                if insn.address + insn.size + op.mem.disp != tgt:
                    continue
                matched = True
                key = (insn.address, tgt)
                if key not in seen:
                    seen.add(key)
                    results.append((insn.address, insn.mnemonic, insn.op_str, tgt))
            if matched:
                break
    results.sort()
    return results


def find_function_start(image: Image, va: int, max_back: int = 0x2000):
    """Walk back to the byte after the last int3/padding run before `va`."""
    lo = max(image.text["va"], va - max_back)
    off_lo = image.va_to_off(lo)
    off_hi = image.va_to_off(va)
    if off_lo is None or off_hi is None:
        return None
    blob = image.data[off_lo:off_hi]
    best = None
    i = 0
    while i < len(blob):
        if blob[i] == 0xCC:
            j = i
            while j < len(blob) and blob[j] == 0xCC:
                j += 1
            if j < len(blob):
                best = lo + j
            i = j
        else:
            i += 1
    return best


def disasm_window(image: Image, va: int, before: int, after: int):
    start = va - before
    off = image.va_to_off(start)
    if off is None:
        return []
    blob = image.data[off : off + before + after]
    return [
        (i.address, i.mnemonic, i.op_str)
        for i in image.md.disasm(blob, start)
    ]


def resolve_convar_objects(image: Image, names):
    """name -> registration site -> ConVar global object VA."""
    name_to_vas = {}
    va_to_name = {}
    for name in names:
        vas = image.find_string_vas(name)
        if vas:
            name_to_vas[name] = vas
            for v in vas:
                va_to_name[v] = name

    sites = find_rip_refs(image, set(va_to_name))
    results = {}
    for site_va, mnem, ops, tgt in sites:
        hit_name = va_to_name[tgt]
        # The ConVar object is a lea into writable data inside the same short
        # registration block as the name-string lea.
        obj_candidates = []
        window = disasm_window(image, site_va, before=0, after=80)
        back = disasm_window(image, site_va - 48, before=0, after=48)
        for addr, m, o in list(back) + list(window):
            if m != "lea":
                continue
            mo = re.search(r"rip \+ (0x[0-9a-f]+)\]", o)
            if not mo:
                continue
            # disasm_window gives op_str with the raw disp; recompute via a
            # single-instruction decode for the exact target.
            off = image.va_to_off(addr)
            if off is None:
                continue
            try:
                insn = next(image.md.disasm(image.data[off : off + 20], addr, count=1))
            except StopIteration:
                continue
            for op in insn.operands:
                if op.type != CS_OP_MEM or op.mem.base != x86.X86_REG_RIP:
                    continue
                ref = insn.address + insn.size + op.mem.disp
                sec = image.section_of(ref)
                if sec in (".data", ".bss") and ref not in va_to_name:
                    obj_candidates.append((abs(addr - site_va), addr, ref, sec))
        obj_candidates.sort()
        entry = results.setdefault(
            hit_name,
            {
                "name": hit_name,
                "string_vas": [hex(v) for v in name_to_vas[hit_name]],
                "registrations": [],
            },
        )
        entry["registrations"].append(
            {
                "site": hex(site_va),
                "insn": f"{mnem} {ops}",
                "objects": [
                    {"va": hex(ref), "lea_at": hex(addr), "section": sec}
                    for _d, addr, ref, sec in obj_candidates[:4]
                ],
            }
        )
    for name in names:
        if name not in results:
            results[name] = {
                "name": name,
                "string_vas": [hex(v) for v in name_to_vas.get(name, [])],
                "registrations": [],
                "note": "no code xref to the name string",
            }
    return results


# A ConVar global here is 0x10 bytes: +0x0 is the object (passed as `this` to
# the accessor) and +0x8 is the ConVarData pointer the value is read through.
# Anything at +0x10 belongs to the NEXT convar, not this one.
CONVAR_FIELDS = (0x0, 0x8)


def find_read_sites(image: Image, object_vas):
    """Every site touching a resolved ConVar object at +0x0 or +0x8."""
    targets = {}
    for name, ova in object_vas.items():
        for delta in CONVAR_FIELDS:
            targets.setdefault(ova + delta, []).append((name, delta))
    hits = defaultdict(list)
    for va, mnem, ops, tgt in find_rip_refs(image, set(targets)):
        for name, delta in targets[tgt]:
            hits[name].append((va, mnem, ops, delta))
    return hits


# Console/help routines that touch every registered convar. Not render path.
NOISE_FUNCTIONS = {0x1805676A0, 0x180566120, 0x180568F80}

CVAR_NAME_RE = re.compile(r"^[a-z][a-z0-9_]{4,63}$")


def discover_cvar_names(image: Image, prefixes):
    """Every plausible ConVar name string carrying one of `prefixes`."""
    out = set()
    for text in image.inventory_strings(prefixes):
        if CVAR_NAME_RE.match(text):
            out.add(text)
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=None, help="CS2 .../game directory")
    ap.add_argument("--module", default="bin/win64/scenesystem.dll")
    ap.add_argument("--json", default=None)
    ap.add_argument("--inventory", action="store_true")
    ap.add_argument("--xref", action="store_true")
    ap.add_argument("--disasm", default=None, help="VA to disassemble, e.g. 0x180199590")
    ap.add_argument("--before", type=int, default=32)
    ap.add_argument("--after", type=int, default=320)
    ap.add_argument("--cvars", default=None, help="comma separated override list")
    ap.add_argument("--all", action="store_true", help="resolve every lb_/csm_/sst_/r_/sc_ cvar")
    ap.add_argument("--focus", default=None, help="only print cvars matching this substring")
    ap.add_argument("--annotate", action="store_true", help="label ConVar globals in --disasm")
    ap.add_argument("--refs", default=None, help="comma separated VAs or exact strings to xref")
    args = ap.parse_args()

    game = args.game
    if not game:
        for candidate in DEFAULT_GAME_DIRS:
            if os.path.isdir(candidate):
                game = candidate
                break
    if not game:
        sys.exit("could not locate the CS2 game directory; pass --game")

    path = os.path.join(game, args.module)
    if not os.path.isfile(path):
        sys.exit(f"missing module: {path}")

    image = Image(path)
    print(f"# module {image.name} base=0x{image.base:x} bytes={len(image.data)}")

    if args.inventory:
        found = image.inventory_strings(INVENTORY_PREFIXES)
        for text in sorted(found):
            print(f"0x{found[text]:x}  {text}")
        print(f"# {len(found)} strings")
        return

    if args.refs:
        wanted = {}
        for token in args.refs.split(","):
            token = token.strip()
            if token.startswith("0x"):
                wanted[int(token, 16)] = token
            else:
                for sva in image.find_string_vas(token):
                    wanted[sva] = token
        if not wanted:
            sys.exit("no target resolved for --refs")
        for va, mnem, ops, tgt in find_rip_refs(image, set(wanted)):
            fn = find_function_start(image, va)
            label = image.cstring_at(tgt) or wanted[tgt]
            print(
                f"0x{va:x}  fn=0x{fn:x}  {mnem} {ops}"
                f"   ; 0x{tgt:x} {label!r}" if fn else f"0x{va:x}  {mnem} {ops}"
            )
        return

    if args.disasm:
        va = int(args.disasm, 16)
        start = find_function_start(image, va)
        if start:
            print(f"# enclosing function start ~ 0x{start:x}")

        # Annotate rip targets that are known ConVar globals.
        annotations = {}
        if args.annotate:
            names = discover_cvar_names(image, ("lb_", "csm_", "sparseshadowtree_", "sc_", "r_"))
            reg = resolve_convar_objects(image, names)
            for name, info in reg.items():
                for r in info["registrations"]:
                    if r["objects"]:
                        obj = int(r["objects"][0]["va"], 16)
                        annotations[obj] = f"CVAR {name}"
                        annotations[obj + 8] = f"CVAR {name}.data"
                        break

        off = image.va_to_off(va - args.before)
        if off is None:
            sys.exit("address is not mapped")
        blob = image.data[off : off + args.before + args.after]
        for insn in image.md.disasm(blob, va - args.before):
            note = ""
            for op in insn.operands:
                if op.type != CS_OP_MEM or op.mem.base != x86.X86_REG_RIP:
                    continue
                ref = insn.address + insn.size + op.mem.disp
                label = annotations.get(ref)
                if not label:
                    text = image.cstring_at(ref)
                    if text and text.isprintable() and len(text) >= 4:
                        label = f'"{text}"'
                note = f"   ; 0x{ref:x} {label or image.section_of(ref) or ''}"
            mark = "  <<<" if insn.address == va else ""
            print(f"0x{insn.address:x}  {insn.mnemonic:<8} {insn.op_str}{note}{mark}")
        return

    if not args.xref:
        ap.error("pick one of --inventory / --xref / --disasm")

    if args.cvars:
        names = args.cvars.split(",")
    elif args.all:
        names = discover_cvar_names(image, ("lb_", "csm_", "sparseshadowtree_", "sc_", "r_"))
        print(f"# discovered {len(names)} candidate cvar names", file=sys.stderr)
    else:
        names = TARGET_CVARS
    print("# resolving ConVar objects ...", file=sys.stderr)
    reg = resolve_convar_objects(image, names)

    object_vas = {}
    for name, info in reg.items():
        for r in info["registrations"]:
            if r["objects"]:
                object_vas[name] = int(r["objects"][0]["va"], 16)
                break

    print("# scanning read sites ...", file=sys.stderr)
    reads = find_read_sites(image, object_vas)

    report = {"module": image.name, "cvars": {}}
    func_table = defaultdict(dict)

    for name in names:
        info = reg[name]
        obj = object_vas.get(name)
        entry = {
            "string_vas": info["string_vas"],
            "registrations": info["registrations"],
            "object_va": hex(obj) if obj else None,
            "read_sites": [],
        }
        for va, mnem, ops, delta in reads.get(name, []):
            fn = find_function_start(image, va)
            entry["read_sites"].append(
                {
                    "va": hex(va),
                    "insn": f"{mnem} {ops}",
                    "field_delta": hex(delta),
                    "function": hex(fn) if fn else None,
                }
            )
            if fn:
                func_table[fn].setdefault(name, []).append(hex(va))
        report["cvars"][name] = entry

    report["functions"] = {
        hex(fn): {k: v for k, v in sorted(gates.items())}
        for fn, gates in sorted(func_table.items())
    }

    # Registration thunks and the console dump routine reference every convar;
    # they say nothing about the render path, so keep them out of the tables.
    def is_noise(fn_hex):
        if not fn_hex:
            return True
        fn = int(fn_hex, 16)
        return fn < 0x180020000 or fn in NOISE_FUNCTIONS

    for name in names:
        if args.focus and args.focus not in name:
            continue
        e = report["cvars"][name]
        interesting = [s for s in e["read_sites"] if not is_noise(s["function"])]
        print(f"\n== {name}")
        print(f"   string     {', '.join(e['string_vas']) or 'NOT FOUND'}")
        print(f"   object     {e['object_va'] or 'unresolved'}")
        for s in interesting:
            print(f"   read       {s['va']}  +{s['field_delta']}  fn={s['function']}  {s['insn']}")
        if not interesting:
            print("   read       (registration/dump only - no render-path reader)")

    print("\n== render-path functions by gate set")
    for fn, gates in report["functions"].items():
        if is_noise(fn) or len(gates) < 2:
            continue
        print(f"   {fn}: {', '.join(gates)}")

    if args.json:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(report, fh, indent=2)
        print(f"\n# wrote {args.json}")


if __name__ == "__main__":
    main()
