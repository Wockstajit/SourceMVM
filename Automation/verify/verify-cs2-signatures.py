#!/usr/bin/env python3
"""Verify every AOB signature in AfxHookSource2 against the installed CS2 binaries.

Run this FIRST after any CS2 update. A game update relocates code and breaks byte-pattern
scans; each broken one shows up at runtime as an "Error - AfxHookSource2 / Problem in <file>:<line>"
message box, one per failed scan, which is slow and painful to triage by hand. This finds them
all in one pass, offline, without launching the game.

    python Automation/verify/verify-cs2-signatures.py
    python Automation/verify/verify-cs2-signatures.py --game "C:/Program Files (x86)/Steam/.../game"
    python Automation/verify/verify-cs2-signatures.py --pattern "48 89 5C 24 ?? 4C 89"

Exit code is 0 when every signature matches, 1 when any is broken.

Output classes:
  OK      exactly one match in exactly one module -- what you want
  MULTI   matches, but more than once; getAddress takes the FIRST hit, so this is only a
          problem if the intended target is not the first one. Worth an eyeball, not an error.
  BROKEN  no match in any module -- this one will pop an error box at runtime

Comments are stripped before scanning, so patterns parked in /* ... */ blocks are ignored.

### Re-deriving a broken signature

There is no shortcut, but the reliable recipe is:

1. Find an anchor in the target module -- usually a nearby string. Profiling scope names
   ("CUIEngine::RunScript (compile+run)"), assert text and RTTI names all work well.
2. Find the code that RIP-relative references that string.
3. Resolve the function CONTAINING that reference via the .pdata RUNTIME_FUNCTION table
   (exact function boundaries, no guessing).
4. Take the prologue bytes, wildcard the stack displacements, and confirm the pattern matches
   exactly once and resolves to the address you expect.

`PE` below gives you sections, VA<->offset mapping and .pdata; that is everything step 1-4 needs.
This is how the CUIEngine::RunScript pattern was re-derived for build 14172 -- see the comment
at that call site in DeathMsg.cpp.

Also check upstream first: https://github.com/advancedfx/advancedfx often has the fix already
for signatures inherited from HLAE (`git fetch upstream && git log upstream/main`). Only
fork-local signatures (Cosmetics/*, our own hooks) have to be re-derived by hand.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys
from pathlib import Path

# Where CS2 actually is on this machine. See Automation/lib/steam_paths.py -- these defaults
# used to be a hand-written list of drive letters headed by the author's own Steam library.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from steam_paths import cs2_game_dirs, cs2_install_dirs  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOT = REPO_ROOT / "AfxHookSource2"

# Steam libraries are per-machine; override with --game.
# Kept as a tail for the case where Steam itself cannot be located.
_CONVENTIONAL = [
    "C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game",
]
DEFAULT_GAME_DIRS = cs2_game_dirs() + _CONVENTIONAL

# Every module AfxHookSource2 scans, plus the neighbours a pattern might land in.
MODULE_RELPATHS = {
    "client": "csgo/bin/win64/client.dll",
    "server": "csgo/bin/win64/server.dll",
    "engine2": "bin/win64/engine2.dll",
    "panorama": "bin/win64/panorama.dll",
    "rendersystemdx11": "bin/win64/rendersystemdx11.dll",
    "scenesystem": "bin/win64/scenesystem.dll",
    "particles": "bin/win64/particles.dll",
    "schemasystem": "bin/win64/schemasystem.dll",
    "tier0": "bin/win64/tier0.dll",
    "animationsystem": "bin/win64/animationsystem.dll",
    "inputsystem": "bin/win64/inputsystem.dll",
    "materialsystem2": "bin/win64/materialsystem2.dll",
    "worldrenderer": "bin/win64/worldrenderer.dll",
    "soundsystem": "bin/win64/soundsystem.dll",
    "resourcesystem": "bin/win64/resourcesystem.dll",
    "filesystem_stdio": "bin/win64/filesystem_stdio.dll",
    "networksystem": "bin/win64/networksystem.dll",
}


class PE:
    """Just enough PE32+ to read sections, map VA<->file offset, and walk .pdata."""

    def __init__(self, path):
        self.path = str(path)
        self.data = open(path, "rb").read()
        d = self.data
        e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
        if d[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise ValueError(f"not a PE: {path}")
        coff = e_lfanew + 4
        nsec, = struct.unpack_from("<H", d, coff + 2)
        opt_size, = struct.unpack_from("<H", d, coff + 16)
        opt = coff + 20
        magic, = struct.unpack_from("<H", d, opt)
        if magic != 0x20B:
            raise ValueError(f"not PE32+: {path}")
        self.image_base, = struct.unpack_from("<Q", d, opt + 24)
        sec = opt + opt_size
        self.sections = []
        for i in range(nsec):
            off = sec + i * 40
            name = d[off:off + 8].rstrip(b"\0").decode("latin1")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, off + 8)
            self.sections.append((name, vaddr, vsize, rawptr, rawsize))

    def raw(self, name):
        for n, va, vs, rp, rs in self.sections:
            if n == name:
                return self.data[rp:rp + rs], va, rp
        return b"", 0, 0

    def off_to_va(self, off):
        for n, sva, vs, rp, rs in self.sections:
            if rp <= off < rp + rs:
                return self.image_base + sva + (off - rp)
        return None

    def va_to_off(self, va):
        rva = va - self.image_base
        for n, sva, vs, rp, rs in self.sections:
            if sva <= rva < sva + vs:
                o = rp + (rva - sva)
                return o if o < len(self.data) else None
        return None

    def functions(self):
        """(start_va, end_va) for every RUNTIME_FUNCTION in .pdata, sorted."""
        pd, _, _ = self.raw(".pdata")
        out = []
        for i in range(0, len(pd) - 11, 12):
            b, e, u = struct.unpack_from("<III", pd, i)
            if b or e:
                out.append((self.image_base + b, self.image_base + e))
        out.sort()
        return out

    def function_containing(self, va):
        funcs = self.functions()
        lo, hi = 0, len(funcs) - 1
        while lo <= hi:
            m = (lo + hi) // 2
            b, e = funcs[m]
            if va < b:
                hi = m - 1
            elif va >= e:
                lo = m + 1
            else:
                return b, e
        return None


def pattern_to_regex(pattern):
    out = b""
    for tok in pattern.split():
        if tok in ("??", "?"):
            out += b"."
        else:
            out += re.escape(bytes([int(tok, 16)]))
    return re.compile(out, re.S)


def scan(buf, pattern):
    return [m.start() for m in pattern_to_regex(pattern).finditer(buf)]


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group().count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


AOB_RE = re.compile(r'"((?:(?:[0-9A-Fa-f]{2}|\?\?|\?)[ ]){5,}(?:[0-9A-Fa-f]{2}|\?\?|\?))"')


# Maps the module variable a getAddress()-style call is given to a MODULE_RELPATHS key.
# Without this the verifier only asks "does this pattern match SOMEWHERE?", which passed a
# genuinely broken signature on 2026-08-27: the CS2 2000897 update moved
# CUIEngine::RunScript in panorama.dll, the old pattern stopped matching there, and it
# happened to still match one address in client.dll -- so the tool printed a green
# "OK ... client @ 0x181280970" over a break that popped an error box at launch.
MODULE_VAR_TO_KEY = {
    "clientdll": "client",
    "serverdll": "server",
    "engine2dll": "engine2",
    "panoramadll": "panorama",
    "rendersystemdx11dll": "rendersystemdx11",
    "scenesystemdll": "scenesystem",
    "particlesdll": "particles",
    "schemasystemdll": "schemasystem",
    "tier0dll": "tier0",
    "animationsystemdll": "animationsystem",
    "inputsystemdll": "inputsystem",
    "materialsystemdll": "materialsystem2",
    "materialsystem2dll": "materialsystem2",
    "worldrendererdll": "worldrenderer",
}

MODULE_VAR_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*Dll)\b")


def module_hint(lines, index):
    """Which module is this pattern actually scanned against, if the source says so.

    The call is often split over two lines -- getAddress(panoramaDll,\n "48 89 ...") -- so the
    variable usually sits on a PRECEDING line. Looks back a few lines and takes the nearest.
    Returns None when the pattern does not come from a module-scoped scan, in which case the
    old "matched in exactly one module" rule still applies.
    """
    for back in range(0, 4):
        i = index - back
        if i < 0:
            break
        m = None
        for m in MODULE_VAR_RE.finditer(lines[i]):
            pass  # take the LAST occurrence on the line: closest to the pattern
        if m:
            return MODULE_VAR_TO_KEY.get(m.group(1).lower())
    return None


def collect_patterns(source_root):
    found = []
    for dirpath, _dirs, files in os.walk(source_root):
        # AfxHookSource2 also contains a separate Deadlock DLL. Its patterns are
        # checked by verify-deadlock-signatures.py against that game's binaries.
        if Path(dirpath).relative_to(source_root).parts[:1] == ("Deadlock",):
            continue
        for fn in files:
            if not fn.endswith((".cpp", ".h")):
                continue
            fp = Path(dirpath) / fn
            try:
                text = strip_comments(fp.read_text(encoding="utf-8", errors="replace"))
            except OSError:
                continue
            rel = fp.relative_to(source_root).as_posix()
            lines = text.splitlines()
            for i, line in enumerate(lines, 1):
                for m in AOB_RE.finditer(line):
                    if len(m.group(1).split()) >= 6:
                        found.append((rel, i, m.group(1), module_hint(lines, i - 1)))
    return found


def resolve_game_dir(explicit):
    if explicit:
        p = Path(explicit)
        if not p.is_dir():
            sys.exit(f"error: --game path not found: {p}")
        return p
    for cand in DEFAULT_GAME_DIRS:
        if Path(cand).is_dir():
            return Path(cand)
    sys.exit(
        "error: could not find the CS2 'game' directory.\n"
        "       pass it explicitly, e.g.\n"
        '       --game "C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game"'
    )


STAMP_PATH = Path(__file__).resolve().parent / "cs2-build-stamp.txt"


def read_steam_inf(game_dir):
    out = {}
    inf = game_dir / "csgo" / "steam.inf"
    if inf.is_file():
        for line in inf.read_text(errors="replace").splitlines():
            if "=" in line:
                k, _, v = line.partition("=")
                out[k.strip()] = v.strip()
    return out


def report_build_stamp(game_dir, broken, total, write):
    """Record (and compare against) the CS2 build these signatures last verified clean on.

    Without this, "did the game move?" needs a launch and a crash to answer. The stamp is
    checked in, so a mismatch is visible in `git status` and before anything is started.
    """
    live = read_steam_inf(game_dir)
    live_version = live.get("ClientVersion", "?")
    previous = {}
    if STAMP_PATH.is_file():
        for line in STAMP_PATH.read_text(encoding="utf-8").splitlines():
            if "=" in line and not line.startswith("#"):
                k, _, v = line.partition("=")
                previous[k.strip()] = v.strip()

    prev_version = previous.get("ClientVersion")
    if prev_version and prev_version != live_version:
        print(f"\nCS2 BUILD CHANGED: signatures last verified clean on {prev_version} "
              f"({previous.get('VersionDate', '?')}), game is now {live_version} "
              f"({live.get('VersionDate', '?')}).")
        print("    Also re-run: python Automation/verify/verify-cs2-assets.py")
        print("    Runbook:     Docs/agent/cs2-update-recovery.md")
    elif prev_version:
        print(f"\nCS2 build unchanged since the last clean run ({prev_version}).")

    if broken or not write:
        return
    STAMP_PATH.write_text(
        "# Written by Automation/verify/verify-cs2-signatures.py on a fully-clean run.\n"
        "# The CS2 build every AOB signature in AfxHookSource2 last verified against.\n"
        "# A mismatch with the installed game means the patterns are unverified for it.\n"
        f"ClientVersion={live_version}\n"
        f"PatchVersion={live.get('PatchVersion', '?')}\n"
        f"VersionDate={live.get('VersionDate', '?')}\n"
        f"SignaturesVerified={total}\n",
        encoding="utf-8",
    )
    if prev_version != live_version:
        print(f"    Stamped {STAMP_PATH.name}: ClientVersion={live_version}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", help="path to the CS2 'game' directory")
    ap.add_argument("--source", default=str(SOURCE_ROOT), help="source tree to scan")
    ap.add_argument("--pattern", help="test a single ad-hoc pattern instead of scanning the tree")
    ap.add_argument("--quiet", action="store_true", help="only report problems")
    ap.add_argument("--no-stamp", action="store_true",
                    help="do not update cs2-build-stamp.txt on a fully-clean run")
    args = ap.parse_args(argv)

    game = resolve_game_dir(args.game)
    print(f"CS2 game dir: {game}")

    inf = game / "csgo" / "steam.inf"
    if inf.is_file():
        for line in inf.read_text(errors="replace").splitlines():
            if line.startswith(("ClientVersion", "PatchVersion")):
                print(f"  {line.strip()}")

    modules = {}
    for name, rel in MODULE_RELPATHS.items():
        p = game / rel
        if p.is_file():
            try:
                pe = PE(p)
                text, tva, trp = pe.raw(".text")
                modules[name] = (pe, text, trp)
            except Exception as exc:
                print(f"  skip {name}: {exc}")
    print(f"  modules loaded: {len(modules)}\n")
    if not modules:
        sys.exit("error: no modules could be loaded")

    def probe(pattern):
        hits = []
        for name, (pe, text, trp) in modules.items():
            offs = scan(text, pattern)
            if offs:
                hits.append((name, [pe.off_to_va(trp + o) for o in offs]))
        return hits

    if args.pattern:
        hits = probe(args.pattern)
        if not hits:
            print("BROKEN  (no match in any module)")
            return 1
        for name, vas in hits:
            print(f"{name}: {len(vas)} hit(s) -> {', '.join(hex(v) for v in vas[:8])}")
        return 0

    patterns = collect_patterns(Path(args.source))
    print(f"live AOB signatures found: {len(patterns)}\n")

    broken, multi, wrongmod = [], [], []
    for rel, line, pattern, want in patterns:
        hits = probe(pattern)
        names = [n for n, _v in hits]
        if not hits:
            broken.append((rel, line, pattern))
        elif want and want not in names:
            # Matched, but NOT in the module this code actually scans -- i.e. broken at runtime,
            # and the incidental match elsewhere is what used to disguise it.
            wrongmod.append((rel, line, pattern, want, hits))
        elif all(len(vas) > 1 for _n, vas in hits):
            multi.append((rel, line, pattern, hits))
        elif not args.quiet:
            name, vas = next((n, v) for n, v in hits if len(v) == 1)
            print(f"OK      {rel}:{line}  {name} @ {hex(vas[0])}")

    if multi:
        print()
        for rel, line, pattern, hits in multi:
            counts = ", ".join(f"{n}x{len(v)}" for n, v in hits)
            print(f"MULTI   {rel}:{line}  ({counts})")
            print(f"          {pattern[:110]}")

    if wrongmod:
        print()
        for rel, line, pattern, want, hits in wrongmod:
            where = ", ".join(f"{n}x{len(v)}" for n, v in hits)
            print(f"WRONGMOD {rel}:{line}  scanned against {want}, but only matches: {where}")
            print("          -> BROKEN at runtime. The match elsewhere is a coincidence, not a pass.")
            print(f"          {pattern[:110]}")

    if broken:
        print()
        for rel, line, pattern in broken:
            print(f"BROKEN  {rel}:{line}")
            print(f"          {pattern[:110]}")

    failed = len(broken) + len(wrongmod)
    print(f"\n=== {len(patterns) - failed}/{len(patterns)} match, "
          f"{len(multi)} multi-match, {len(broken)} BROKEN, {len(wrongmod)} WRONG-MODULE ===")
    # A wrong-module hit is a runtime break, so it must gate the stamp exactly like BROKEN --
    # otherwise a clean-looking stamp would record a build the patterns do NOT actually work on.
    report_build_stamp(game, broken=failed, total=len(patterns), write=not args.no_stamp)
    if failed:
        print("\nSee this file's docstring for the re-derivation recipe, and check\n"
              "`git fetch upstream && git log upstream/main` first -- inherited HLAE\n"
              "signatures are often already fixed there.")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
