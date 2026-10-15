#!/usr/bin/env python3
"""Resolve and verify AfxHookDeadlock byte signatures against the installed Deadlock binaries.

The Deadlock sibling of verify-cs2-signatures.py. Same job, separate script, separate table: a
Deadlock update and a CS2 update are unrelated events, and neither verifier should be able to
report the other game's breakage.

    py -3 Automation/verify/verify-deadlock-signatures.py
    py -3 Automation/verify/verify-deadlock-signatures.py --game "E:/SteamLibrary/.../Deadlock/game"
    py -3 Automation/verify/verify-deadlock-signatures.py --pattern "48 8B 01 4C 8B C3"

Exit code is 0 when every required signature resolves, 1 when any does not.

Output classes:
  OK      exactly one match, in the module it was looked for in
  MULTI   more than one match; a getAddress()-style scan takes the FIRST hit, so this is only
          wrong if the intended target is not first. Worth an eyeball, not a failure.
  BROKEN  no match. Required entries fail the run; optional ones only warn.

## Discovery mode

Porting means deriving these addresses in the first place, so the same tool does the finding:

    --find-string "layout/hud.xml"        which module holds a string, and at what RVA
    --xref-string "layout/hud.xml"        every instruction that RIP-relative references it,
                                          plus the .pdata function each reference sits inside
    --dump-rtti Citadel                   RTTI class names containing a substring
    --prologue client 0x16bcfb0           the first bytes of a function, to build a pattern from

The recipe for a new signature is unchanged from the CS2 side: anchor on a string, find the code
that references it, resolve the containing function through .pdata rather than guessing where it
starts, then wildcard the stack displacements in the prologue and confirm a unique match.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from deadlock_paths import deadlock_game_dirs  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]

# Deadlock puts the engine modules in game/bin/win64 and the client under the mod directory,
# which is the one structural difference from CS2 that matters to a binary scan.
MODULE_RELPATHS = {
    "client": Path("citadel") / "bin" / "win64" / "client.dll",
    "engine2": Path("bin") / "win64" / "engine2.dll",
    "panorama": Path("bin") / "win64" / "panorama.dll",
    "panoramauiclient": Path("bin") / "win64" / "panoramauiclient.dll",
    "schemasystem": Path("bin") / "win64" / "schemasystem.dll",
    "scenesystem": Path("bin") / "win64" / "scenesystem.dll",
    "materialsystem2": Path("bin") / "win64" / "materialsystem2.dll",
    "rendersystemdx11": Path("bin") / "win64" / "rendersystemdx11.dll",
    "filesystem_stdio": Path("bin") / "win64" / "filesystem_stdio.dll",
    "inputsystem": Path("bin") / "win64" / "inputsystem.dll",
    "tier0": Path("bin") / "win64" / "tier0.dll",
    "host": Path("citadel") / "bin" / "win64" / "host.dll",
}

# The signature table. `required` entries fail the run when broken; the rest only warn, because
# losing one of those costs a single feature rather than the whole hook.
#
# `source` records where the value came from, so a re-derivation starts from evidence:
#   cs2       lifted unchanged from the CS2 hook, expected to hit because the code is shared
#   derived   derived here, against this Deadlock build
SIGNATURES = [
    {'name': 'client_frame_stage', 'module': 'client', 'required': True, 'source': 'derived',
     'pattern': '48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 40 48 8B F1 33 FF 48 8B 89 30 02 00 00 8B DA 48 85 C9',
     'expect_rva': 0x1886570,
     'note': 'CSource2Client slot 36; stage 12 view caller traced on client initialization thread'},
    {'name': 'demo_player_pointer', 'module': 'engine2', 'pattern': '48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 58 84 C0 0F 85 ?? ?? ?? ?? 83 EF 03', 'source': 'derived', 'required': True, 'note': 'RIP-relative global; checked against measured profile', 'target_rva': 6403608},
    {'name': 'demo_player_vtable', 'module': 'engine2', 'pattern': '48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 48 8D 05 ?? ?? ?? ?? 48 89 41 08', 'source': 'derived', 'required': True, 'note': 'RIP-relative global; checked against measured profile', 'target_rva': 5461808},
    {'name': 'client_global_vars', 'module': 'client', 'pattern': '48 8B 0D ?? ?? ?? ?? F3 0F 10 49 30 B9 FF FF FF FF E8 ?? ?? ?? ??', 'source': 'derived', 'required': True, 'note': 'RIP-relative global; checked against measured profile', 'target_rva': 0x3310df0},

    {
        "name": "panorama_RunScript",
        "module": "panorama",
        "pattern": "4C 89 4C 24 ?? 4C 89 44 24 ?? 48 89 54 24 ?? 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 C7 44 24 ?? ?? ?? ?? ?? 48 89 44 24 ?? 4D 8B E0",
        "source": "derived",
        "required": True,
        "note": "CUIEngine::RunScript(engine, ctx, src, originFile, line) -- the JS injection primitive",
        "expect_rva": 0xB4450,
        "derivation": (
            "Build 6774: compile+run profiling reference at 0xb44ba; .pdata entry 0xb4450. The chained unwind fragments continue past the first range. Source r8 becomes r12, panel rdx becomes rbx, engine rcx becomes r15, and origin r9 becomes rdi."
        ),
    },
    {
        "name": "client_hud_panel_global",
        "module": "client",
        "pattern": "4C 89 35 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 49 8B 4E 08 45 33 C0",
        "source": "derived",
        "required": True,
        "note": "HUD root panel global (CPanel2D**); the RIP displacement is at +3",
        "expect_rva": 0x1B195B6,
        "derivation": (
            "Build 6774: hud.xml lea at 0x1b195bd inside 0x1b19310. The preceding r14 store at 0x1b195b6 resolves the CPanel2D global 0x3d00158; the following load reads its CUIPanel at +8."
        ),
    },
    {
        "name": "client_uiengine_global",
        "module": "client",
        "pattern": "48 89 78 ?? 48 89 0D ?? ?? ?? ??",
        "source": "cs2",
        "required": True,
        "note": "CUIEngine global; anchor is the CreatePanelWithCurrentContext function",
        "expect_rva": 0x1EE8B30,
    },
    {
        "name": "client_view_setup_prologue",
        "module": "client",
        "pattern": "48 89 5C 24 08 48 89 6C 24 20 56 57 41 56 48 83 EC 60 C7 82 FC 04 00 00 01 00 00 00 48 8D 0D ?? ?? ?? ??",
        "source": "derived",
        "required": True,
        "note": "the per-frame view setup; the camera override is installed inside this function",
        "expect_rva": 0x5D7120,
    },
]

# RTTI class names the hook resolves vtables through. Deadlock keeps CS2's engine-generic names
# and swaps only the game-specific ones, so this table is mostly a regression guard.
RTTI_CLASSES = [
    ("client", ".?AVClientModeCitadelNormal@@", True, "view provider; slot 15 fills the embedded view"),
    ("client", ".?AVCViewRender@@", True, "view render; world-to-screen matrix and RenderView"),
    ("client", ".?AVCCitadel_Hud_Main@@", True, "the HUD root panel class, CS2's CCSGO_Hud"),
    ("client", ".?AVCCitadelInput@@", False, "player input; CS2's CCSGOInput, for releasing aim"),
    ("client", ".?AVCCitadel_Hud_Replay@@", False, "native replay bar"),
    ("client", ".?AVCCitadel_Hud_Replay_Scrubber@@", False, "native scrub slider"),
    ("client", ".?AVCRenderingPipelineCitadelPostHud@@", False, "CS2's CRenderingPipelineCsgo"),
    ("filesystem_stdio", ".?AVCFileSystem_Stdio@@", True, "AddSearchPath detour"),
    ("materialsystem2", ".?AVCMaterialSystem2@@", True, "material system singleton"),
    ("rendersystemdx11", ".?AVCRenderDeviceDx11@@", True, "render device; the present hook"),
]

# CreateInterface version strings. Every one of these is byte-identical to CS2's, so the whole
# interface-acquisition layer ports without change.
INTERFACE_VERSIONS = [
    ("client", "Source2Client002", True),
    ("engine2", "Source2EngineToClient001", True),
    ("engine2", "ResourceSystem013", True),
    ("engine2", "VFileSystem017", True),
    ("engine2", "SchemaSystem_001", True),
    ("engine2", "SceneSystem_002", True),
    ("engine2", "PanoramaUIEngine001", True),
    ("engine2", "GameUIService_001", True),
    ("engine2", "NetworkClientService_001", False),
]

# Strings worth keeping in one place: these are the anchors the RE work keeps coming back to.
ANCHOR_STRINGS = [
    ("client", "layout/hud.xml", "creates the HUD root panel; CitadelHud is its panel class"),
    ("client", "CreatePanelWithCurrentContext", "sits in the function holding the CUIEngine global"),
    ("client", "CitadelHudReplay", "native replay bar panel class"),
    ("client", "CitadelHUDReplayScrubber", "native scrub slider panel class"),
    ("engine2", "Unable to open netconsole on port", "the netconsole server exists in this build"),
    ("panorama", "CUIEngine::RunScript", "profiling scope name beside RunScript itself"),
]

_PE_MAGIC = bytes([0x50, 0x45, 0x00, 0x00])
_NUL = bytes([0x00])
_DOT = bytes([0x2E])


class PE:
    """Just enough PE for pattern scanning, RVA mapping and .pdata function bounds."""

    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        pe_off = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe_off:pe_off + 4] != _PE_MAGIC:
            raise ValueError("not a PE: " + str(path))
        n_sections = struct.unpack_from("<H", self.data, pe_off + 6)[0]
        opt_size = struct.unpack_from("<H", self.data, pe_off + 20)[0]
        opt_off = pe_off + 24
        magic = struct.unpack_from("<H", self.data, opt_off)[0]
        if magic != 0x20B:
            raise ValueError("expected PE32+ (x64): " + str(path))
        self.image_base = struct.unpack_from("<Q", self.data, opt_off + 24)[0]
        sec_off = opt_off + opt_size
        self.sections = []
        for i in range(n_sections):
            base = sec_off + i * 40
            name = self.data[base:base + 8].rstrip(_NUL).decode("ascii", "replace")
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", self.data, base + 8)
            self.sections.append({
                "name": name, "vsize": vsize, "vaddr": vaddr, "rsize": rsize, "raddr": raddr,
            })
        self._functions = None

    def section(self, name: str):
        for s in self.sections:
            if s["name"] == name:
                return s
        return None

    def raw(self, name: str) -> bytes:
        s = self.section(name)
        if not s:
            return b""
        return self.data[s["raddr"]:s["raddr"] + s["rsize"]]

    def off_to_rva(self, off: int):
        for s in self.sections:
            if s["raddr"] <= off < s["raddr"] + s["rsize"]:
                return s["vaddr"] + (off - s["raddr"])
        return None

    def rva_to_off(self, rva: int):
        for s in self.sections:
            span = s["vsize"] if s["vsize"] > s["rsize"] else s["rsize"]
            if s["vaddr"] <= rva < s["vaddr"] + span:
                delta = rva - s["vaddr"]
                if delta < s["rsize"]:
                    return s["raddr"] + delta
        return None

    def functions(self):
        """(begin_rva, end_rva) for every RUNTIME_FUNCTION in .pdata, sorted."""
        if self._functions is None:
            blob = self.raw(".pdata")
            out = []
            for i in range(0, len(blob) - 11, 12):
                begin, end, _unwind = struct.unpack_from("<III", blob, i)
                if begin == 0 and end == 0:
                    continue
                out.append((begin, end))
            out.sort()
            self._functions = out
        return self._functions

    def function_containing(self, rva: int):
        for begin, end in self.functions():
            if begin <= rva < end:
                return (begin, end)
        return None


def pattern_to_regex(pattern: str) -> re.Pattern:
    parts = []
    for token in pattern.split():
        if token in ("?", "??"):
            parts.append(_DOT)
            continue
        parts.append(re.escape(bytes([int(token, 16)])))
    return re.compile(b"".join(parts), re.DOTALL)


def scan_text(pe: PE, pattern: str) -> list:
    """Every RVA in .text matching the pattern."""
    rx = pattern_to_regex(pattern)
    section = pe.section(".text")
    if not section:
        return []
    blob = pe.data[section["raddr"]:section["raddr"] + section["rsize"]]
    return [section["vaddr"] + m.start() for m in rx.finditer(blob)]


def read_rva(pe: PE, rva: int, size: int) -> bytes:
    off = pe.rva_to_off(rva)
    if off is None or off + size > len(pe.data):
        return b""
    return pe.data[off:off + size]


def pattern_at(pe: PE, rva: int, pattern: str) -> bool:
    raw = read_rva(pe, rva, len(pattern.split()))
    return bool(raw and pattern_to_regex(pattern).fullmatch(raw))


def check_demo_filename_chain(pe: PE, demo_vtable_rva: int):
    """CEngineClient slot43 -> CDemoPlayer slot1 -> CDemoFile slot3 (not recorder slot4)."""
    engine = primary_vtable(pe, ".?AVCEngineClient@@")
    file = primary_vtable(pe, ".?AVCDemoFile@@")
    if not engine or not file:
        return None
    def slot(vt, index):
        raw = read_rva(pe, vt + index * 8, 8)
        return struct.unpack("<Q", raw)[0] - pe.image_base if len(raw) == 8 else 0
    getter, embedded, filename = slot(engine, 43), slot(demo_vtable_rva, 1), slot(file, 3)
    if not pattern_at(pe, getter,
        "48 83 EC 28 48 83 3D ?? ?? ?? ?? 00 74 2C 48 8B 0D ?? ?? ?? ?? "
        "48 8B 01 FF 50 58 84 C0 74 1B 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 08 "
        "48 8B C8 48 8B 10 48 83 C4 28 48 FF 62 18 33 C0 48 83 C4 28 C3"):
        return None
    # The playing test and file fetch must refer to the same player global.
    code = read_rva(pe, getter, 65)
    if getter + 21 + struct.unpack_from("<i", code, 17)[0] != getter + 38 + struct.unpack_from("<i", code, 34)[0]:
        return None
    if not pattern_at(pe, embedded, "48 8D 41 20 C3") or not pattern_at(pe, filename,
        "48 8B 51 08 48 8D 05 ?? ?? ?? ?? 48 85 D2 48 0F 45 C2 C3"):
        return None
    return getter, embedded, filename


def check_demo_vtable(pe: PE, vtable_rva: int):
    """Validate every demo method the hook calls against its accessor machine code.

    This makes the slot numbers derived properties of the installed Deadlock binary. A copied
    CS2 index or a slot shifted by a game update fails offline before the DLL calls it.
    """
    checks = [
        (2, "start tick", "8B 81 04 02 00 00 C3"),
        (3, "current tick", "40 53 48 83 EC 20 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 C8 01 00 00 2B 83 04 02 00 00"),
        (11, "playing", "0F B6 81 30 12 00 00 C3"),
        (12, "paused", "80 B9 30 12 00 00 00 74 1A 83 B9 28 02 00 00 00 7C 11 83 B9 08 02 00 00 FF"),
        (14, "seeking", "80 B9 30 12 00 00 00 74 0C 83 B9 08 02 00 00 FF 74 03 B0 01 C3"),
        (17, "playback scale", "F3 0F 10 81 14 02 00 00 C3"),
    ]
    rows = []
    for slot, name, pattern in checks:
        entry = read_rva(pe, vtable_rva + slot * 8, 8)
        if len(entry) != 8:
            rows.append((slot, name, None, False))
            continue
        va = struct.unpack_from("<Q", entry)[0]
        fn_rva = va - pe.image_base
        rows.append((slot, name, fn_rva, pattern_at(pe, fn_rva, pattern)))
    return rows


def find_string(pe: PE, needle: str) -> list:
    """Every RVA where `needle` appears as ASCII bytes anywhere in the image."""
    raw = needle.encode("ascii")
    out = []
    start = 0
    while True:
        idx = pe.data.find(raw, start)
        if idx < 0:
            break
        rva = pe.off_to_rva(idx)
        if rva is not None:
            out.append(rva)
        start = idx + 1
    return out


# lea reg, [rip+disp32] and mov reg, [rip+disp32] cover essentially every string reference the
# compiler emits here. Match the two-byte REX+opcode forms with a RIP-relative modrm.
_RIP_FORMS = (
    bytes([0x48, 0x8D]), bytes([0x4C, 0x8D]),
    bytes([0x48, 0x8B]), bytes([0x4C, 0x8B]),
)


def xref_string(pe: PE, needle: str) -> list:
    """Instructions that RIP-relative reference `needle`, with the function each sits in."""
    targets = set(find_string(pe, needle))
    if not targets:
        return []
    section = pe.section(".text")
    if not section:
        return []
    blob = pe.data[section["raddr"]:section["raddr"] + section["rsize"]]
    base_rva = section["vaddr"]
    hits = []
    for i in range(len(blob) - 7):
        if blob[i:i + 2] not in _RIP_FORMS:
            continue
        modrm = blob[i + 2]
        if (modrm & 0xC7) != 0x05:  # mod=00, rm=101 -> RIP-relative
            continue
        disp = struct.unpack_from("<i", blob, i + 3)[0]
        insn_rva = base_rva + i
        target = insn_rva + 7 + disp
        if target in targets:
            hits.append({
                "insn_rva": insn_rva,
                "target_rva": target,
                "function": pe.function_containing(insn_rva),
            })
    return hits


def dump_rtti(pe: PE, substring: str) -> list:
    """MSVC mangled RTTI class names containing `substring`."""
    rx = re.compile(rb"[.][?]AV[A-Za-z0-9_@?$]{1,200}@@")
    found = set()
    for m in rx.finditer(pe.data):
        name = m.group(0).decode("ascii", "replace")
        if substring.lower() in name.lower():
            found.add(name)
    return sorted(found)


def primary_vtable(pe, name):
    """Resolve one primary x64 RTTI table, validating its complete object locator."""
    candidates = set()
    rdata = pe.section('.rdata')
    text = pe.section('.text')
    if not rdata or not text:
        return None
    for name_rva in find_string(pe, name + '\0'):
        descriptor = name_rva - 16
        needle = struct.pack('<I', descriptor)
        for match in re.finditer(re.escape(needle), pe.data):
            col = pe.off_to_rva(match.start() - 12)
            raw = read_rva(pe, col, 24) if col is not None else b''
            if len(raw) != 24:
                continue
            signature, offset, cd_offset, td, hierarchy, self_rva = struct.unpack('<6I', raw)
            if (signature, offset, cd_offset, td, self_rva) != (1, 0, 0, descriptor, col):
                continue
            for ref in re.finditer(re.escape(struct.pack('<Q', pe.image_base + col)), pe.raw('.rdata')):
                vt = rdata['vaddr'] + ref.start() + 8
                entry = read_rva(pe, vt, 8)
                if len(entry) == 8:
                    fn = struct.unpack('<Q', entry)[0] - pe.image_base
                    if text['vaddr'] <= fn < text['vaddr'] + text['vsize']:
                        candidates.add(vt)
    return next(iter(candidates)) if len(candidates) == 1 else None


def check_view_chain(pe, view_setup_rva):
    """CViewRender slot4 -> slot17 -> ClientModeCitadelNormal slot15(mode, embedded view)."""
    view = primary_vtable(pe, '.?AVCViewRender@@')
    mode = primary_vtable(pe, '.?AVClientModeCitadelNormal@@')
    if view is None or mode is None:
        return None
    def slot(table, index):
        raw = read_rva(pe, table + index * 8, 8)
        return struct.unpack('<Q', raw)[0] - pe.image_base if len(raw) == 8 else -1
    if slot(mode, 15) != view_setup_rva:
        return None
    if not pattern_at(pe, slot(view, 4),
            '48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 '
            '48 81 EC B0 00 00 00 48 8B 01 4C 8B F1 FF 90 88 00 00 00'):
        return None
    setup_caller = slot(view, 17)
    call = bytes.fromhex('48 8B C8 48 8B 10 4C 8B 42 78 48 8D 56 10 41 FF D0')
    hits = [m.start() for m in re.finditer(re.escape(call), read_rva(pe, setup_caller, 0x80))]
    if len(hits) != 1:
        return None
    return view, setup_caller, setup_caller + hits[0] + len(call)


def resolve_game_dir(explicit):
    if explicit:
        path = Path(explicit)
        if not path.is_dir():
            raise SystemExit("not a directory: " + str(path))
        return path
    for candidate in deadlock_game_dirs():
        return Path(candidate)
    raise SystemExit("Deadlock not found. Set SOURCEMVM_DEADLOCK_DIR or pass --game <install>/game.")


def load_modules(game_dir: Path) -> dict:
    modules = {}
    for name, rel in MODULE_RELPATHS.items():
        path = game_dir / rel
        if not path.is_file():
            continue
        try:
            modules[name] = PE(path)
        except Exception as exc:  # a module we cannot parse is not a signature failure
            print("  ! could not parse " + name + ": " + str(exc))
    return modules


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", help="<Deadlock install>/game")
    ap.add_argument("--pattern", help="scan one ad-hoc pattern across every module")
    ap.add_argument("--find-string", dest="find_string")
    ap.add_argument("--xref-string", dest="xref_string")
    ap.add_argument("--dump-rtti", dest="dump_rtti")
    ap.add_argument("--prologue", nargs=2, metavar=("MODULE", "RVA"))
    ap.add_argument("--module", help="restrict a discovery query to one module")
    args = ap.parse_args(argv)

    game_dir = resolve_game_dir(args.game)
    print("Deadlock: " + str(game_dir))
    modules = load_modules(game_dir)
    if not modules:
        raise SystemExit("no Deadlock modules could be read under " + str(game_dir))
    if args.module:
        modules = {k: v for k, v in modules.items() if k == args.module}
        if not modules:
            raise SystemExit("no such module: " + args.module)
    print("modules:  " + ", ".join(sorted(modules)))
    print("")

    if args.find_string:
        for name in sorted(modules):
            for rva in find_string(modules[name], args.find_string)[:12]:
                print("  " + name + "  rva 0x" + format(rva, "x"))
        return 0

    if args.xref_string:
        for name in sorted(modules):
            for hit in xref_string(modules[name], args.xref_string)[:40]:
                fn = hit["function"]
                where = ("fn 0x" + format(fn[0], "x") + "-0x" + format(fn[1], "x")) if fn else "fn ?"
                print("  " + name + "  insn 0x" + format(hit["insn_rva"], "x") + "  " + where)
        return 0

    if args.dump_rtti:
        for name in sorted(modules):
            names = dump_rtti(modules[name], args.dump_rtti)
            if names:
                print("  " + name + ":")
                for n in names[:120]:
                    print("    " + n)
        return 0

    if args.prologue:
        name, rva_text = args.prologue
        pe = modules.get(name)
        if pe is None:
            raise SystemExit("no such module: " + name)
        rva = int(rva_text, 0)
        off = pe.rva_to_off(rva)
        if off is None:
            raise SystemExit("rva not mapped")
        raw = pe.data[off:off + 48]
        print("  " + " ".join(format(b, "02X") for b in raw))
        fn = pe.function_containing(rva)
        if fn:
            print("  function 0x" + format(fn[0], "x") + "-0x" + format(fn[1], "x"))
        return 0

    if args.pattern:
        for name in sorted(modules):
            hits = scan_text(modules[name], args.pattern)
            if hits:
                print("  " + name + ": " + str(len(hits)) + " hit(s) -> " +
                      ", ".join("0x" + format(h, "x") for h in hits[:8]))
        return 0

    broken_required = 0
    for entry in SIGNATURES:
        module = entry["module"]
        pe = modules.get(module)
        if pe is None:
            print("SKIP    " + entry["name"] + "  (" + module + " not present)")
            if entry["required"]: broken_required += 1
            continue
        hits = scan_text(pe, entry["pattern"])
        tag = "OK     " if len(hits) == 1 else ("MULTI  " if hits else "BROKEN ")
        detail = ", ".join("0x" + format(h, "x") for h in hits[:4]) if hits else "-"
        if "target_rva" in entry and hits:
            resolved = []
            for hit in hits:
                off = pe.rva_to_off(hit)
                resolved.append(hit + 7 + struct.unpack_from("<i", pe.data, off + 3)[0])
            detail += " -> " + ", ".join(hex(target) for target in resolved)
            if len(hits) != 1:
                broken_required += 1
            for target in resolved:
                if not any(section["vaddr"] <= target < section["vaddr"] + section["vsize"] for section in pe.sections):
                    broken_required += 1
        expected = entry.get("expect_rva")
        drift = ""
        if expected is not None and hits and hits[0] != expected:
            drift = "   (moved from 0x" + format(expected, "x") + ")"
        print(tag + " " + entry["name"] + "  [" + module + ", " + entry["source"] + "]  " +
              detail + drift)
        if not hits:
            if entry["required"]:
                broken_required += 1
            print("         " + entry["note"])
            if entry.get("derivation"):
                print("         how it was derived: " + entry["derivation"])

    print("")
    print("Demo-player vtable methods")
    eng = modules.get("engine2")
    if eng is not None:
        vt_entry = next(e for e in SIGNATURES if e["name"] == "demo_player_vtable")
        vt_hits = scan_text(eng, vt_entry["pattern"])
        if len(vt_hits) == 1:
            hit_off = eng.rva_to_off(vt_hits[0])
            vtable_rva = vt_hits[0] + 7 + struct.unpack_from("<i", eng.data, hit_off + 3)[0]
            filename_chain = check_demo_filename_chain(eng, vtable_rva)
            if filename_chain:
                print("  OK      filename chain: engine slot43 0x%x -> player slot1 0x%x -> file slot3 0x%x" % filename_chain)
            else:
                broken_required += 1
                print("  BROKEN  replay filename accessor chain")
            for slot, name, fn_rva, ok in check_demo_vtable(eng, vtable_rva):
                if not ok:
                    broken_required += 1
                where = "-" if fn_rva is None else "0x%x" % fn_rva
                print(("  OK     " if ok else "  BROKEN ") +
                      "slot %d %-15s -> %s" % (slot, name, where))
        else:
            broken_required += 1
            print("  BROKEN  demo-player vtable did not resolve uniquely")

    print("")
    print("Main-view gate chain")
    cli = modules.get("client")
    if cli is not None:
        setup_hits = scan_text(cli, next(
            e["pattern"] for e in SIGNATURES if e["name"] == "client_view_setup_prologue"))
        if not setup_hits:
            print("  SKIP    the view setup did not resolve, so the chain cannot be checked")
        else:
            chain = check_view_chain(cli, setup_hits[0])
            if chain:
                print("  OK      CViewRender 0x%x -> slot17 0x%x -> mode slot15 returns 0x%x"
                      % chain)
            else:
                broken_required += 1
                print("  BROKEN  RTTI view/mode tables or the measured slot17 call contract do not match.")
                print("          The hook gates every camera write on this chain; without it the")
                print("          write lands in whatever object the view function is handed, which")
                print("          does not fault and kills the process a few frames later.")

    print("")
    print("RTTI classes")
    for module, mangled, required, why in RTTI_CLASSES:
        pe = modules.get(module)
        if pe is None:
            continue
        present = bool(find_string(pe, mangled))
        if not present and required:
            broken_required += 1
        print(("  OK     " if present else "  MISSING") + " " + mangled +
              "  [" + module + "]   (" + why + ")")

    print("")
    print("CreateInterface versions")
    for module, version, required in INTERFACE_VERSIONS:
        pe = modules.get(module)
        if pe is None:
            continue
        present = bool(find_string(pe, version))
        if not present and required:
            broken_required += 1
        print(("  OK     " if present else "  MISSING") + " " + version + "  [" + module + "]")

    print("")
    print("Anchor strings")
    for module, needle, why in ANCHOR_STRINGS:
        pe = modules.get(module)
        if pe is None:
            continue
        hits = find_string(pe, needle)
        state = ("found " + str(len(hits))) if hits else "MISSING"
        print("  " + module + "  " + repr(needle) + "  " + state + "   (" + why + ")")

    print("")
    if broken_required:
        print(str(broken_required) + " required signature(s) broken.")
        return 1
    print("All required signatures resolved.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
