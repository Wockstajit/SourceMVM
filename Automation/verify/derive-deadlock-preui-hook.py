"""Derive Deadlock's pre-UI render moment, offline, from the shipped DLLs.

WHY
---
Scaling the preview by shrinking the engine's own viewport is the wrong mechanism: it changes what
the engine culls, it never clears the remainder of the render target, and it leaves the view
block's other fields claiming the full screen. CS2 does it properly (AfxHookSource2/ViewportScaler.h)
-- the world renders FULL-SCREEN, and at a callback that runs after the world and before Panorama
composites, a D3D pass copies the backbuffer, CLEARS it, and redraws it scaled into the preview
rect.

CS2 reaches that moment by detouring the two `CSceneSystem::CreateRenderContextPtr` overloads in
scenesystem.dll. Both are varargs and take a format string naming the render pass being set up;
when that string is `#%s/SetupLightsAndViewConstants` with arg0 == "CSGOHud", the HUD's render
context is being created -- world done, UI about to be drawn. CS2 then queues a render-thread
callback on the context's software command list through a vtable slot.

Porting that to Deadlock needs four measured values. This script derives all four and writes them
to reference/deadlock/offsets/preui-hook.json.

HOW EACH ONE IS FOUND, AND WHY THAT WAY
---------------------------------------
1. THE TWO FUNCTIONS -- by STRING ANCHOR, not by a byte pattern. Both are reachable from the
   warning text they emit; a string survives a recompile far better than a prologue, so anchoring
   on it is what makes this re-derivable after a Deadlock update instead of a one-off measurement.
   The byte patterns are then CHECKED for uniqueness, because a pattern that matches twice is
   worse than one that matches never: it detours whichever copy the scan reaches first.

2. THE HUD PASS NAME -- Deadlock's analogue of "CSGOHud". This one CANNOT be derived statically,
   and the attempt to is instructive: "CitadelHud" is a panel class in client.dll and was the
   obvious guess, and it never matched, because what the engine passes here is the render
   CONTEXT's name -- composed at runtime and absent from the binary as a literal. The real value,
   "CitadelHudRoot", was read out of the running game with `mirv_deadlock preui names`, which
   dumps every distinct pass the engine sets up. Guessing it wrong gives a detour that installs
   cleanly and never fires, which is indistinguishable from the feature being off -- so the hook
   reports three separate counters to tell those apart.

3. THE QueueCallback VTABLE SLOT -- NOT assumed to equal CS2's. CS2 hardcodes 142 with a comment
   warning that a wrong value is "silently catastrophic": the callback is never queued AND some
   other virtual is called with a callback pointer as its argument. So the slot is identified by
   finding the SAME FUNCTION in both games' vtables and reading Deadlock's index off, matching on
   code shape. The margin between the best and second-best match is reported, because a narrow
   margin means the answer is a guess wearing a number.

Offline only: it reads DLLs off disk and never attaches to a running game.
"""

from __future__ import annotations

import importlib.util
import json
import re
import struct
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "deadlock_offsets", _HERE / "dump-deadlock-offsets.py")
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)
PE = _mod.PE
find_rtti_vtable = _mod.find_rtti_vtable

ANCHOR = b"WARNING: Trying to create a CRenderContextPtr without a valid context."

# The prologues to detour. Pattern 1 is CS2's with ONE byte wildcarded: the two builds call
# different vtable displacements there (`ff 90 e8 01 00 00` in CS2, `ff 90 e0 01 00 00` in
# Deadlock), which is an ordinary consequence of the interface growing between builds.
PATTERNS = {
    "CreateRenderContextPtr1":
        "40 53 56 57 48 83 ec 20 49 8b 00 49 8b d8 48 8b f9 45 33 c0 48 8b cb 49 8b f1 ff 90 ?? 01 00 00",
    "CreateRenderContextPtr2":
        "40 55 53 57 48 8d 6c 24 ?? 48 81 ec ?? ?? ?? ?? 80 65 ?? ?? 33 c0",
}

# The pass-name format strings the detour compares against.
PASS_STRINGS = [
    b"#%s/SetupLightsAndViewConstants",
    b"#%s/%s/LayerClear",
    b"UpdateBuffers",
]

# Plural "Lists". The singular spelling does not exist and silently finds nothing.
COMMAND_LIST_CLASS = ".?AVCRenderContextDx11_SoftwareCommandLists@@"
CS2_QUEUE_CALLBACK_SLOT = 142


def steam_libraries() -> list:
    """Every Steam library root, read from Steam's own index rather than hardcoded."""
    roots = []
    vdf = Path(r"C:/Program Files (x86)/Steam/steamapps/libraryfolders.vdf")
    if vdf.exists():
        for m in re.finditer(r'"path"\s*"([^"]+)"', vdf.read_text(encoding="utf-8", errors="replace")):
            roots.append(Path(m.group(1).replace("\\\\", "\\")))
    return roots


def find_game(folder_name: str):
    for root in steam_libraries():
        cand = root / "steamapps" / "common" / folder_name
        if cand.exists():
            return cand
    return None


def rip_refs(pe: PE, target_rva: int) -> list:
    """Every RIP-relative lea in .text whose destination is `target_rva`."""
    sec = pe.section(".text")
    blob = pe.data[sec["raddr"]:sec["raddr"] + sec["rsize"]]
    out = []
    for m in re.finditer(rb"[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", blob):
        off = m.start()
        if off + 7 > len(blob):
            continue
        disp = struct.unpack_from("<i", blob, off + 3)[0]
        insn = sec["vaddr"] + off
        if insn + 7 + disp == target_rva:
            out.append(insn)
    return out


def function_start(pe: PE, rva: int, max_back: int = 0x2000):
    """Walk back to the enclosing function start through MSVC's `cc` alignment padding."""
    sec = pe.section(".text")
    off = pe.rva_to_off(rva)
    if off is None:
        return None
    limit = max(sec["raddr"], off - max_back)
    i = off
    while i > limit:
        if pe.data[i - 1] == 0xCC and pe.data[i - 2] == 0xCC:
            return pe.off_to_rva(i)
        i -= 1
    return None


def vtable_slots(pe: PE, vtable_rva: int, count: int) -> list:
    out = []
    for i in range(count):
        raw = pe.read_rva(vtable_rva + i * 8, 8)
        if len(raw) != 8:
            break
        out.append(struct.unpack("<Q", raw)[0] - pe.image_base)
    return out


def prefix_agreement(a: bytes, b: bytes) -> int:
    n = 0
    for x, y in zip(a, b):
        if x != y:
            break
        n += 1
    return n


def main() -> int:
    ok = True
    report = {}

    dl = find_game("Deadlock")
    if not dl:
        print("Deadlock install not found via Steam's library index.")
        return 2
    dl_bin = dl / "game" / "bin" / "win64"
    scene = PE(dl_bin / "scenesystem.dll")
    print(f"scenesystem.dll  sha256={scene.sha256()[:16]}")
    report["scenesystem_sha256"] = scene.sha256()

    # ---- 1. the two CreateRenderContextPtr overloads ---------------------------------------------
    hits = scene.find_bytes(ANCHOR)
    print(f"\nanchor: {ANCHOR[:46].decode()!r}...")
    if not hits:
        print("  NOT FOUND -- the warning text changed. Re-anchor before trusting anything below.")
        return 1
    print("  " + ", ".join(f"0x{h:x}" for h in hits))

    refs = sorted(r for h in hits for r in rip_refs(scene, h))
    funcs = []
    for r in refs:
        s = function_start(scene, r)
        if s and s not in funcs:
            funcs.append(s)
    print(f"  {len(refs)} reference(s) -> {len(funcs)} function(s): "
          + ", ".join(f"0x{f:x}" for f in funcs))
    report["anchor_functions"] = [f"0x{f:x}" for f in funcs]

    print("\nprologue patterns (must be UNIQUE -- a double match detours the wrong copy):")
    report["patterns"] = {}
    for name, pat in PATTERNS.items():
        found = scene.scan_text(pat)
        verdict = ("UNIQUE" if len(found) == 1 else
                   "NOT FOUND" if not found else f"AMBIGUOUS ({len(found)})")
        anchored = "" if not found else (
            "  (anchor agrees)" if found[0] in funcs else "  (WARNING: not an anchored function)")
        print(f"  {name:26s} {verdict:16s} "
              + ", ".join(f"0x{h:x}" for h in found) + anchored)
        report["patterns"][name] = {"pattern": pat,
                                    "hits": [f"0x{h:x}" for h in found],
                                    "verdict": verdict}
        if verdict != "UNIQUE" or found[0] not in funcs:
            ok = False

    print("\npass-name format strings:")
    report["pass_strings"] = {}
    for s in PASS_STRINGS:
        found = scene.find_bytes(s + b"\0")
        print(f"  {s.decode():34s} " + (", ".join(f"0x{x:x}" for x in found) or "NOT FOUND"))
        report["pass_strings"][s.decode()] = [f"0x{x:x}" for x in found]
        if not found:
            ok = False

    # ---- 2. the HUD pass name --------------------------------------------------------------------
    client = dl / "game" / "citadel" / "bin" / "win64" / "client.dll"
    print(f"\nHUD pass name (Deadlock's analogue of CS2's \"CSGOHud\"), in client.dll:")
    report["hud_pass_name"] = "CitadelHudRoot"   # confirmed in game, not inferred
    report["hud_names"] = []
    if client.exists():
        cpe = PE(client)
        seen = set()
        for sec_name in (".rdata", ".data"):
            sec = cpe.section(sec_name)
            if not sec:
                continue
            blob = cpe.data[sec["raddr"]:sec["raddr"] + sec["rsize"]]
            for m in re.finditer(rb"(?<![A-Za-z0-9_])[A-Za-z][A-Za-z0-9_]{2,28}Hud\0", blob):
                text = m.group()[:-1].decode("ascii", "replace")
                if text in seen:
                    continue
                seen.add(text)
                report["hud_names"].append(
                    {"rva": f"0x{sec['vaddr'] + m.start():x}", "text": text})
        for e in report["hud_names"]:
            mark = "  <-- CONFIRMED render-context name" if e["text"] == "CitadelHudRoot" else ""
            print(f"  {e['rva']:<12s} {e['text']}{mark}")
        # Informational only. "CitadelHudRoot" is NOT a plain string in client.dll -- the render
        # context's name is composed at runtime -- so its absence here proves nothing, and gating
        # on it would fail a derivation that is correct. The authority is the live dump
        # (`mirv_deadlock preui names`), which confirmed it, and the in-game counters, which show
        # the pass being recognised. What this listing is good for is spotting the day the naming
        # scheme changes entirely.
        print("  (the render-context name is built at runtime and is not expected in this list;")
        print("   confirm it with 'mirv_deadlock preui names' in game)")
    else:
        print(f"  {client} not found")

    # ---- 3. the QueueCallback vtable slot ---------------------------------------------------------
    print(f"\n{COMMAND_LIST_CLASS} vtable:")
    report["queue_callback"] = {}
    dl_rs = PE(dl_bin / "rendersystemdx11.dll")
    dl_vt = find_rtti_vtable(dl_rs, COMMAND_LIST_CLASS)
    print(f"  Deadlock: {dl_vt}")
    report["queue_callback"]["deadlock_vtable"] = dl_vt

    cs2 = find_game("Counter-Strike Global Offensive")
    cs2_rs_path = cs2 / "game" / "bin" / "win64" / "rendersystemdx11.dll" if cs2 else None
    if dl_vt and cs2_rs_path and cs2_rs_path.exists():
        cs_rs = PE(cs2_rs_path)
        cs_vt = find_rtti_vtable(cs_rs, COMMAND_LIST_CLASS)
        print(f"  CS2:      {cs_vt}")
        report["queue_callback"]["cs2_vtable"] = cs_vt
        if cs_vt:
            dl_slots = vtable_slots(dl_rs, int(dl_vt["vtable_rva"], 16), dl_vt["slots"])
            cs_slots = vtable_slots(cs_rs, int(cs_vt["vtable_rva"], 16), cs_vt["slots"])
            want = cs_rs.read_rva(cs_slots[CS2_QUEUE_CALLBACK_SLOT], 512)
            scored = sorted(
                ((prefix_agreement(want, dl_rs.read_rva(r, 512)), i, r)
                 for i, r in enumerate(dl_slots)), reverse=True)
            best, runner = scored[0], scored[1]
            print(f"  CS2 QueueCallback is slot {CS2_QUEUE_CALLBACK_SLOT} "
                  f"(0x{cs_slots[CS2_QUEUE_CALLBACK_SLOT]:x})")
            print("  best Deadlock matches (bytes agreeing, slot, rva):")
            for agree, i, r in scored[:4]:
                print(f"    {agree:4d}  slot {i:3d}  0x{r:x}")
            margin = best[0] - runner[0]
            print(f"  -> slot {best[1]}, {best[0]} bytes identical, "
                  f"margin {margin} over the next best")
            report["queue_callback"].update({
                "cs2_slot": CS2_QUEUE_CALLBACK_SLOT,
                "deadlock_slot": best[1],
                "deadlock_fn_rva": f"0x{best[2]:x}",
                "prefix_bytes": best[0],
                "margin": margin,
            })
            # A narrow margin means the match is a coincidence of prologue shape, not the function.
            if best[0] < 32 or margin < 16:
                print("  MARGIN TOO NARROW -- do not trust this slot.")
                ok = False
    else:
        print("  CS2 not found; cannot cross-match the slot.")
        ok = False

    out = Path("reference/deadlock/offsets/preui-hook.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    report["ok"] = ok
    out.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"\nwrote {out}")
    print("\n" + ("all four values derived" if ok else "SOMETHING DID NOT RESOLVE -- see above"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
