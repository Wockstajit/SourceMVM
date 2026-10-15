#!/usr/bin/env python3
"""Dump every version-coupled Deadlock address this repository needs, offline, to JSON.

    py -3 Automation/verify/dump-deadlock-offsets.py
    py -3 Automation/verify/dump-deadlock-offsets.py --out somewhere/else.json
    py -3 Automation/verify/dump-deadlock-offsets.py --print

The output lands in `reference/deadlock/offsets/deadlock-offsets.json` by default. That tree is
git-ignored on purpose: the file describes ONE machine's installed build and goes stale the moment
Valve ships an update, so it is a local artefact to be regenerated, never a checked-in constant.
`reference/deadlock/offsets/README.md` says where the numbers come from and how to refresh them.

WHY THIS EXISTS, AND WHY IT IS NOT THE VERIFIER
-----------------------------------------------
`verify-deadlock-signatures.py` answers "is the hook's current table still correct?" and exits
non-zero when it is not. This script answers a different question -- "what ARE the numbers on this
build?" -- and always succeeds. They are deliberately separate programs with separate tables, in
line with the repository rule that a new capability gets its own entry point: a dump must keep
working while the hook's table is broken, which is exactly when it is most useful.

WHY A DUMP IS THE SMALL HALF OF THE ANSWER
------------------------------------------
Three classes of number live in a Source 2 game, and only one of them belongs in a file like this.

  1. Byte-pattern RVAs (the view setup, the Panorama RunScript, the globals). These MOVE on every
     build, so the hook resolves them by pattern at load. The dump records where they landed, which
     turns "a game update broke it" into a diff instead of an investigation.
  2. COM/vtable slots fixed by an interface contract (IDXGISwapChain::Present is slot 8). These do
     not move and are not dumped.
  3. Entity field offsets (m_vecAbsOrigin, m_iHealth, ...). These move constantly AND are
     self-describing at runtime through schemasystem.dll. The hook resolves them BY NAME at load
     (AfxHookSource2/Deadlock/DeadlockSchema.cpp), so there is nothing here to go stale. A dumped
     field offset would be a liability: it is a number that looks authoritative and is wrong a week
     later. `mirv_deadlock dump schema` writes the live table when you want to read it.

So this file carries (1), plus the identity anchors that make (3) possible: the RTTI vtables and
the CreateInterface version strings the runtime layer validates itself against.

EXTERNAL CROSS-CHECKS
---------------------
Nothing here depends on a third-party dump, and that is the point -- a community offsets repo is
updated by a stranger on their own schedule and is silently stale in between. See
reference/deadlock/offsets/README.md for the projects worth comparing against when a signature
genuinely cannot be re-derived.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from deadlock_paths import deadlock_game_dirs  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO_ROOT / "reference" / "deadlock" / "offsets" / "deadlock-offsets.json"

MODULE_RELPATHS = {
    "client": Path("citadel") / "bin" / "win64" / "client.dll",
    "host": Path("citadel") / "bin" / "win64" / "host.dll",
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
}

# Byte patterns, and what to do with the match.
#
#   kind "code"   the match RVA itself is the answer (a function prologue)
#   kind "rip"    the match points at a RIP-relative reference; `disp` is the byte offset of the
#                 disp32 within the match and `end` where the referencing instruction ends, so the
#                 absolute target is match + end + disp32. That target is the value the hook wants.
SIGNATURES = [
    {
        "name": "panorama_RunScript",
        "module": "panorama",
        "kind": "code",
        "pattern": "4C 89 4C 24 ?? 4C 89 44 24 ?? 48 89 54 24 ?? 55 53 56 57 41 54 41 56 41 57 "
                   "48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? "
                   "48 C7 44 24 ?? ?? ?? ?? ?? 48 89 44 24 ?? 4D 8B E0",
        "note": "CUIEngine::RunScript(engine, ctx, src, originFile, line) -- the JS injection primitive",
    },
    {
        "name": "client_view_setup",
        "module": "client",
        "kind": "code",
        "pattern": "48 89 5C 24 08 48 89 6C 24 20 56 57 41 56 48 83 EC 60 "
                   "C7 82 FC 04 00 00 01 00 00 00 48 8D 0D ?? ?? ?? ??",
        "note": "the per-frame view setup the camera override is installed inside",
    },
    {
        "name": "client_uiengine_global",
        "module": "client",
        "kind": "rip",
        "pattern": "48 89 78 ?? 48 89 0D ?? ?? ?? ??",
        "disp": 7, "end": 11,
        "note": "CUIEngine** -- the store in the CreatePanelWithCurrentContext function",
    },
    {
        "name": "client_hud_panel_global",
        "module": "client",
        "kind": "rip",
        "pattern": "4C 89 35 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 49 8B 4E 08 45 33 C0",
        "disp": 3, "end": 7,
        "note": "CPanel2D** HUD root -- stored right before the lea of layout/hud.xml",
    },
    {
        "name": "client_global_vars",
        "module": "client",
        "kind": "rip",
        "pattern": "48 8B 0D ?? ?? ?? ?? F3 0F 10 49 30 B9 FF FF FF FF E8 ?? ?? ?? ??",
        "disp": 3, "end": 7,
        "note": "CGlobalVars** -- curtime +0x30, frametime +0x34, interval_per_tick +0x54",
    },
    {
        "name": "demo_player_pointer",
        "module": "engine2",
        "kind": "rip",
        "pattern": "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 58 84 C0 0F 85 ?? ?? ?? ?? 83 EF 03",
        "disp": 3, "end": 7,
        "note": "the engine2 demo-player global; its object's vtable must equal demo_player_vtable",
    },
    {
        "name": "demo_player_vtable",
        "module": "engine2",
        "kind": "rip",
        "pattern": "48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 48 8D 05 ?? ?? ?? ?? 48 89 41 08",
        "disp": 3, "end": 7,
        "note": "the demo player's vtable, used as the identity check on the global above",
    },
    {
        "name": "schemasystem_global",
        "module": "schemasystem",
        "kind": "rip",
        "pattern": "48 89 05 ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 33 C0 48 C7 05",
        "disp": 3, "end": 7,
        "note": "CSchemaSystem* -- the root of every entity field offset, resolved BY NAME at runtime",
    },
]

# RTTI class names the runtime layer uses as identity anchors. A vtable found this way is proof of
# what an object IS, which is what lets DeadlockSchema/DeadlockEntities discover a container offset
# by probing rather than by trusting a hardcoded number.
RTTI_CLASSES = [
    ("client", ".?AVClientModeCitadelNormal@@", "view provider; slot 15 fills the embedded view"),
    ("client", ".?AVCViewRender@@", "view render; the camera override's accept test"),
    ("client", ".?AVCGameEntitySystem@@", "entity system; the identity check for the container probe"),
    ("client", ".?AVCCitadel_Hud_Main@@", "HUD root panel class (CS2's CCSGO_Hud)"),
    ("client", ".?AVCCitadelInput@@", "player input (CS2's CCSGOInput)"),
    ("client", ".?AVCCitadel_Hud_Replay@@", "native replay bar the timeline docks beside"),
    ("client", ".?AVCCitadel_Hud_Replay_Scrubber@@", "native scrub slider"),
    ("client", ".?AVCRenderingPipelineCitadelPostHud@@", "render pipeline (CS2's CRenderingPipelineCsgo)"),
    ("filesystem_stdio", ".?AVCFileSystem_Stdio@@", "AddSearchPath"),
    ("materialsystem2", ".?AVCMaterialSystem2@@", "material system singleton"),
    ("rendersystemdx11", ".?AVCRenderDeviceDx11@@", "render device"),
]

# CreateInterface version strings. Every one is byte-identical to CS2's; recording them makes a
# renamed interface -- the failure mode that otherwise reads as "the hook silently does nothing" --
# visible in a diff.
INTERFACE_VERSIONS = [
    ("client", "Source2Client002"),
    ("client", "GameResourceServiceClientV001"),
    ("engine2", "Source2EngineToClient001"),
    ("engine2", "ResourceSystem013"),
    ("engine2", "VFileSystem017"),
    ("engine2", "SchemaSystem_001"),
    ("engine2", "SceneSystem_002"),
    ("engine2", "PanoramaUIEngine001"),
    ("engine2", "GameUIService_001"),
    ("engine2", "NetworkClientService_001"),
    ("engine2", "EngineServiceMgr001"),
    ("tier0", "VEngineCvar007"),
]

# Demo-player vtable slots, each pinned to the machine code of the accessor it must point at.
# A slot number alone is a guess; a slot number that still lands on code shaped like the getter it
# claims to be is a measurement, and it survives a rebuild that shuffles the vtable.
DEMO_PLAYER_SLOTS = [
    (2, "GetStartTick", "8B 81 04 02 00 00 C3"),
    (3, "GetTick", "40 53 48 83 EC 20 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 C8 01 00 00 "
                   "2B 83 04 02 00 00"),
    (11, "IsPlaying", "0F B6 81 30 12 00 00 C3"),
    (12, "IsPaused", "80 B9 30 12 00 00 00 74 1A 83 B9 28 02 00 00 00 7C 11 83 B9 08 02 00 00 FF"),
    (14, "IsSeeking", "80 B9 30 12 00 00 00 74 0C 83 B9 08 02 00 00 FF 74 03 B0 01 C3"),
    (17, "GetPlaybackScale", "F3 0F 10 81 14 02 00 00 C3"),
]

# Engine-interface slots that were MEASURED in game rather than derived from a binary. They are
# recorded here so the dump is the one place to look, and marked so nobody mistakes them for
# something this script verified.
MEASURED_ENGINE_SLOTS = {
    "ISource2EngineToClient::ExecuteClientCmd": 51,
    "ISource2EngineToClient::GetLevelName": 65,
    "ISource2EngineToClient::IsPlayingDemo": 42,
    "ISource2EngineToClient::GetDemoFilePath": 43,
}

# View-struct field offsets, measured live against `getpos` on 2026-09-10. Relative to the view
# object, which is EMBEDDED in CViewRender at this+0x10 -- the reference offsets were relative to
# the embedded view, not to `this`, which is why they look like CS2's with a constant added.
MEASURED_VIEW_OFFSETS = {
    "_embedded_view_at": 0x10,
    "viewport_width": 0x440,
    "viewport_height": 0x448,
    "fov": 0x4A8,
    "origin": 0x4B0,
    "angles": 0x4C8,
    "znear": 0x4D4,
    "zfar": 0x4D8,
    "aspect": 0x4E8,
}

_PE_MAGIC = b"PE\0\0"


class PE:
    """Enough PE to scan .text, map RVAs and read .rdata. Deliberately self-contained."""

    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        pe_off = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe_off:pe_off + 4] != _PE_MAGIC:
            raise ValueError(f"not a PE: {path}")
        n_sections = struct.unpack_from("<H", self.data, pe_off + 6)[0]
        self.timestamp = struct.unpack_from("<I", self.data, pe_off + 8)[0]
        opt_size = struct.unpack_from("<H", self.data, pe_off + 20)[0]
        opt_off = pe_off + 24
        if struct.unpack_from("<H", self.data, opt_off)[0] != 0x20B:
            raise ValueError(f"expected PE32+ (x64): {path}")
        self.image_base = struct.unpack_from("<Q", self.data, opt_off + 24)[0]
        sec_off = opt_off + opt_size
        self.sections = []
        for i in range(n_sections):
            base = sec_off + i * 40
            name = self.data[base:base + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", self.data, base + 8)
            self.sections.append(
                {"name": name, "vsize": vsize, "vaddr": vaddr, "rsize": rsize, "raddr": raddr})

    def section(self, name: str):
        return next((s for s in self.sections if s["name"] == name), None)

    def off_to_rva(self, off: int):
        for s in self.sections:
            if s["raddr"] <= off < s["raddr"] + s["rsize"]:
                return s["vaddr"] + (off - s["raddr"])
        return None

    def rva_to_off(self, rva: int):
        for s in self.sections:
            span = max(s["vsize"], s["rsize"])
            if s["vaddr"] <= rva < s["vaddr"] + span:
                delta = rva - s["vaddr"]
                if delta < s["rsize"]:
                    return s["raddr"] + delta
        return None

    def read_rva(self, rva: int, size: int) -> bytes:
        off = self.rva_to_off(rva)
        if off is None or off + size > len(self.data):
            return b""
        return self.data[off:off + size]

    def scan_text(self, pattern: str) -> list:
        section = self.section(".text")
        if not section:
            return []
        blob = self.data[section["raddr"]:section["raddr"] + section["rsize"]]
        rx = _pattern_to_regex(pattern)
        return [section["vaddr"] + m.start() for m in rx.finditer(blob)]

    def find_bytes(self, needle: bytes) -> list:
        out, start = [], 0
        while True:
            idx = self.data.find(needle, start)
            if idx < 0:
                return out
            rva = self.off_to_rva(idx)
            if rva is not None:
                out.append(rva)
            start = idx + 1

    def sha256(self) -> str:
        return hashlib.sha256(self.data).hexdigest()


def _pattern_to_regex(pattern: str) -> re.Pattern:
    parts = []
    for token in pattern.split():
        parts.append(b"." if token in ("?", "??") else re.escape(bytes([int(token, 16)])))
    return re.compile(b"".join(parts), re.DOTALL)


def _pattern_at(pe: PE, rva: int, pattern: str) -> bool:
    raw = pe.read_rva(rva, len(pattern.split()))
    return bool(raw and _pattern_to_regex(pattern).fullmatch(raw))


def dump_signatures(modules: dict) -> dict:
    out = {}
    for sig in SIGNATURES:
        pe = modules.get(sig["module"])
        row = {"module": sig["module"], "note": sig["note"]}
        if pe is None:
            row["status"] = "module-missing"
            out[sig["name"]] = row
            return out
        hits = pe.scan_text(sig["pattern"])
        row["matches"] = len(hits)
        row["status"] = "ok" if len(hits) == 1 else ("multi" if hits else "broken")
        if hits:
            row["rva"] = hex(hits[0])
            if sig["kind"] == "rip":
                disp = struct.unpack_from("<i", pe.read_rva(hits[0] + sig["disp"], 4))[0]
                row["target_rva"] = hex(hits[0] + sig["end"] + disp)
        out[sig["name"]] = row
    return out


def find_rtti_vtable(pe: PE, mangled: str):
    """The vtable RVA for a class, via its RTTI type descriptor.

    MSVC lays this out as: type descriptor -> (a complete-object locator that points back at it)
    -> the vtable whose [-1] slot is that locator. Walking it is what makes a vtable findable
    without a byte pattern, and therefore what survives a rebuild that moves the code around.
    """
    for name_rva in pe.find_bytes(mangled.encode("ascii") + b"\0"):
        descriptor_rva = name_rva - 0x10          # TypeDescriptor: vfptr, spare, then the name
        for locator_rva in pe.find_bytes(struct.pack("<I", descriptor_rva)):
            # Field 3 of RTTICompleteObjectLocator is the type-descriptor RVA; a real locator has
            # signature 1 (COL64) and offset 0 for the primary base.
            col = pe.read_rva(locator_rva - 12, 20)
            if len(col) != 20:
                continue
            signature, offset = struct.unpack_from("<II", col, 0)
            if signature != 1 or offset != 0:
                continue
            col_rva = locator_rva - 12
            for ref in pe.find_bytes(struct.pack("<Q", pe.image_base + col_rva)):
                vtable_rva = ref + 8
                slots = 0
                while slots < 512:
                    raw = pe.read_rva(vtable_rva + slots * 8, 8)
                    if len(raw) != 8:
                        break
                    va = struct.unpack_from("<Q", raw)[0]
                    if va == 0 or not (pe.image_base < va < pe.image_base + (1 << 32)):
                        break
                    if pe.rva_to_off(va - pe.image_base) is None:
                        break
                    slots += 1
                if slots:
                    return {"vtable_rva": hex(vtable_rva), "slots": slots}
    return None


def dump_rtti(modules: dict) -> dict:
    out = {}
    for module, mangled, note in RTTI_CLASSES:
        pe = modules.get(module)
        row = {"module": module, "note": note}
        found = find_rtti_vtable(pe, mangled) if pe else None
        if found:
            row.update(found)
            row["status"] = "ok"
        else:
            row["status"] = "broken"
        out[mangled] = row
    return out


def dump_interfaces(modules: dict) -> dict:
    out = {}
    for module, version in INTERFACE_VERSIONS:
        pe = modules.get(module)
        hits = pe.find_bytes(version.encode("ascii") + b"\0") if pe else []
        out[version] = {"module": module, "present": bool(hits),
                        "rva": hex(hits[0]) if hits else None}
    return out


def dump_demo_slots(modules: dict, demo_vtable_rva) -> dict:
    pe = modules.get("engine2")
    out = {"vtable_rva": hex(demo_vtable_rva) if demo_vtable_rva else None, "slots": {}}
    if pe is None or not demo_vtable_rva:
        return out
    for slot, name, pattern in DEMO_PLAYER_SLOTS:
        entry = pe.read_rva(demo_vtable_rva + slot * 8, 8)
        row = {"slot": slot}
        if len(entry) == 8:
            fn_rva = struct.unpack_from("<Q", entry)[0] - pe.image_base
            row["fn_rva"] = hex(fn_rva)
            row["confirmed"] = _pattern_at(pe, fn_rva, pattern)
        else:
            row["confirmed"] = False
        out["slots"][name] = row
    return out


def read_build(game_dir: Path) -> dict:
    """ClientVersion / PatchVersion out of the mod directory's steam.inf."""
    inf = game_dir / "citadel" / "steam.inf"
    if not inf.is_file():
        return {}
    build = {}
    for line in inf.read_text(errors="replace").splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            build[key.strip()] = value.strip()
    return build


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", help="<Deadlock install>/game; auto-detected from Steam otherwise")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help=f"default: {DEFAULT_OUT}")
    ap.add_argument("--print", dest="to_stdout", action="store_true",
                    help="also print a human-readable summary")
    args = ap.parse_args(argv)

    candidates = [args.game] if args.game else deadlock_game_dirs()
    game_dir = next((Path(c) for c in candidates if c and Path(c).is_dir()), None)
    if game_dir is None:
        print("Deadlock not found. Pass --game <install>/game, or set SOURCEMVM_DEADLOCK_DIR.",
              file=sys.stderr)
        return 2

    modules, module_rows = {}, {}
    for name, relpath in MODULE_RELPATHS.items():
        path = game_dir / relpath
        if not path.is_file():
            module_rows[name] = {"status": "missing", "path": str(path)}
            continue
        pe = PE(path)
        modules[name] = pe
        module_rows[name] = {
            "status": "ok",
            "path": str(path),
            "size": len(pe.data),
            "image_base": hex(pe.image_base),
            "link_timestamp": pe.timestamp,
            "sha256": pe.sha256(),
        }

    signatures = dump_signatures(modules)
    demo_vtable = signatures.get("demo_player_vtable", {}).get("target_rva")

    profile = {
        "schema_version": 1,
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "game_dir": str(game_dir),
        "build": read_build(game_dir),
        "modules": module_rows,
        "signatures": signatures,
        "rtti_vtables": dump_rtti(modules),
        "interfaces": dump_interfaces(modules),
        "demo_player": dump_demo_slots(modules, int(demo_vtable, 16) if demo_vtable else None),
        "measured_engine_slots": MEASURED_ENGINE_SLOTS,
        "measured_view_offsets": {k: (v if isinstance(v, str) else hex(v))
                                  for k, v in MEASURED_VIEW_OFFSETS.items()},
        "entity_fields": {
            "_note": "Deliberately absent. Entity field offsets are resolved BY NAME at runtime "
                     "through schemasystem.dll (AfxHookSource2/Deadlock/DeadlockSchema.cpp), so a "
                     "dumped copy would be a number that looks authoritative and is wrong after "
                     "the next patch. Use `mirv_deadlock dump schema` to write the live table.",
        },
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {args.out}")

    broken = [n for n, r in signatures.items() if r.get("status") == "broken"]
    broken += [n for n, r in profile["rtti_vtables"].items() if r.get("status") == "broken"]
    if broken:
        print("  did not resolve: " + ", ".join(broken))
    print(f"  build {profile['build'].get('ClientVersion', '?')}, "
          f"{len(modules)} modules, "
          f"{sum(1 for r in signatures.values() if r.get('status') == 'ok')}/{len(SIGNATURES)} "
          f"signatures unique")

    if args.to_stdout:
        print(json.dumps(profile, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
