#!/usr/bin/env python3
"""Generate the cosmetics item-definition tables from the live CS2 game files.

WHY THIS EXISTS. The def-index tables that classify a live item into a loadout slot
(knife / glove / primary / secondary) used to be hand-typed in four places that nothing kept
in sync (see Docs/agent/cs2-update-recovery.md section 4). Valve adds knives and gloves
regularly, and a missing def is silent: the item is simply never classified. Two real gaps
were found the first time this was checked -- def 41 (weapon_knifegg) and def 4725
(studded_brokenfang_gloves, which lives OUTSIDE the 5027..5040 block the old code assumed).

Emits:

  AfxHookSource2/Filmmaker/Cosmetics/CosmeticDefTables.inc

Consumed by CosmeticCatalog.cpp (native apply loop) and CosmeticUiQueries.cpp (Customize UI),
so both classify identically by construction.

    python Automation/tools/generate_cosmetic_defs.py
    python Automation/tools/generate_cosmetic_defs.py --game "C:/Program Files (x86)/Steam/.../game"
    python Automation/tools/generate_cosmetic_defs.py --check    # exit 1 if the .inc is stale

CLASSIFICATION RULES -- all derived from items_game.txt, each verified against the shipped
build to reproduce the previously hand-typed sets exactly:

  knife     prefab chain contains "melee"   (prefab "melee" or "melee_unusual").
            NOT item_gear_slot == "melee": that also catches def 31, weapon_taser (Zeus).
  glove     prefab starts with "hands"      ("hands", "hands_paintable").
  primary   resolved item_gear_slot == "primary"     -- reproduces the old list exactly.
  secondary resolved item_gear_slot == "secondary"   -- reproduces the old list exactly.

`prefab` may name SEVERAL parents separated by spaces ("weapon_base weapon_supports_stickers"),
so the walk splits and takes the first parent that resolves. Requires the `vdf` package
(pip install vdf); items_game.txt has duplicate keys that a hand-rolled KeyValues reader drops.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import vdf

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_cosmetics_catalog import Vpk  # read-only VPK reader; not modified by this tool

# Where CS2 actually is on this machine. See Automation/lib/steam_paths.py -- these defaults
# used to be a hand-written list of drive letters headed by the author's own Steam library.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from steam_paths import cs2_game_dirs, cs2_install_dirs  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
OUT_PATH = REPO_ROOT / "AfxHookSource2" / "Filmmaker" / "Cosmetics" / "CosmeticDefTables.inc"

# Kept as a tail for the case where Steam itself cannot be located.
_CONVENTIONAL = [
    "C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game",
]
DEFAULT_GAME_DIRS = cs2_game_dirs() + _CONVENTIONAL

# The glove block the old hand-written code assumed. Kept as an ADDITIONAL accept range in the
# generated header so a glove Valve ships inside that block still classifies before anyone
# regenerates. It is not sufficient on its own -- 4725 proved that.
LEGACY_GLOVE_RANGE = (5027, 5040)


def find_game_dir(explicit: str | None) -> Path:
    candidates = [explicit] if explicit else DEFAULT_GAME_DIRS
    for c in candidates:
        if c and (Path(c) / "csgo" / "pak01_dir.vpk").is_file():
            return Path(c)
    raise SystemExit(
        "CS2 game dir not found. Pass --game <...>/Counter-Strike Global Offensive/game"
    )


def read_steam_inf(game_dir: Path) -> dict:
    out = {}
    inf = game_dir / "csgo" / "steam.inf"
    if inf.is_file():
        for line in inf.read_text(encoding="utf-8", errors="replace").splitlines():
            if "=" in line:
                k, _, v = line.partition("=")
                out[k.strip()] = v.strip()
    return out


def flat(entry):
    """items_game.txt has duplicate keys; vdf.VDFDict hands those back as a list."""
    return entry[0] if isinstance(entry, list) else entry


class ItemsGame:
    def __init__(self, game_dir: Path):
        vpk = Vpk(game_dir / "csgo" / "pak01_dir.vpk")
        raw = vpk.read("scripts/items/items_game.txt").decode("utf-8", "replace")
        root = vdf.loads(raw, mapper=vdf.VDFDict)["items_game"]
        self.items = root["items"]
        self.prefabs = root["prefabs"]

    def parents(self, entry) -> list:
        return (entry.get("prefab") or "").split()

    def prefab_chain(self, entry, depth=0):
        """Yield every prefab NAME reachable from entry (multi-parent, depth-limited)."""
        if depth > 8 or not isinstance(entry, dict):
            return
        for name in self.parents(entry):
            yield name
            p = self.prefabs.get(name)
            if p is not None:
                yield from self.prefab_chain(flat(p), depth + 1)

    def gear_slot(self, entry, depth=0):
        if depth > 8 or not isinstance(entry, dict):
            return None
        slot = entry.get("item_gear_slot")
        if slot:
            return slot
        for name in self.parents(entry):
            p = self.prefabs.get(name)
            if p is not None:
                got = self.gear_slot(flat(p), depth + 1)
                if got:
                    return got
        return None

    def classify(self) -> dict:
        knives, gloves, primary, secondary, names = set(), set(), set(), set(), {}
        for key, entry in self.items.items():
            entry = flat(entry)
            if not isinstance(entry, dict):
                continue
            try:
                def_index = int(key)
            except ValueError:
                continue
            names[def_index] = entry.get("name") or ""
            chain = list(self.prefab_chain(entry))
            if any(c == "melee" or c.startswith("melee_") for c in chain) or \
                    (entry.get("prefab") or "").startswith("melee"):
                knives.add(def_index)
                continue
            if any(c.startswith("hands") for c in chain):
                gloves.add(def_index)
                continue
            slot = self.gear_slot(entry)
            if slot == "primary":
                primary.add(def_index)
            elif slot == "secondary":
                secondary.add(def_index)
        return {
            "knife": knives, "glove": gloves,
            "primary": primary, "secondary": secondary, "names": names,
        }


def format_defs(defs: set, names: dict, per_line: int = 12) -> str:
    ordered = sorted(defs)
    lines = []
    for i in range(0, len(ordered), per_line):
        lines.append("\t" + ", ".join(str(d) for d in ordered[i:i + per_line]) + ",")
    body = "\n".join(lines)
    return body


def render(tables: dict, build: dict) -> str:
    names = tables["names"]

    def commented(kind: str) -> str:
        ordered = sorted(tables[kind])
        sample = ", ".join(f"{d}={names.get(d, '?')}" for d in ordered[:4])
        return f"// {len(ordered)} {kind} defs: {sample}{', ...' if len(ordered) > 4 else ''}"

    lo, hi = LEGACY_GLOVE_RANGE
    return f"""// GENERATED FILE -- DO NOT EDIT BY HAND.
//
// Regenerate:  python Automation/tools/generate_cosmetic_defs.py
// Source:      <CS2>/game/csgo/pak01_dir.vpk -> scripts/items/items_game.txt
// CS2 build:   ClientVersion={build.get('ClientVersion', '?')} \
PatchVersion={build.get('PatchVersion', '?')} ({build.get('VersionDate', '?')})
//
// These are Valve-owned item-definition indices. They used to be hand-typed in four places
// that nothing kept in sync; a missing def is SILENT (the item is never classified). See
// Docs/agent/cs2-update-recovery.md and run Automation/verify/verify-cs2-assets.py after a
// CS2 update to find out whether this file has gone stale.
//
// Rules (see the generator docstring): knife = prefab chain contains "melee"; glove = prefab
// chain starts with "hands"; primary/secondary = resolved item_gear_slot.

namespace Filmmaker {{
namespace CosmeticDefTables {{

{commented('knife')}
constexpr int kKnifeDefs[] = {{
{format_defs(tables['knife'], names)}
}};

{commented('glove')}
constexpr int kGloveDefs[] = {{
{format_defs(tables['glove'], names)}
}};

{commented('primary')}
constexpr int kPrimaryDefs[] = {{
{format_defs(tables['primary'], names)}
}};

{commented('secondary')}
constexpr int kSecondaryDefs[] = {{
{format_defs(tables['secondary'], names)}
}};

// Gloves Valve shipped inside the historical contiguous block are ALSO accepted, so a glove
// added there still classifies before anyone regenerates this file. Not sufficient alone:
// def 4725 (studded_brokenfang_gloves) lives outside it, which is why the explicit list exists.
constexpr int kLegacyGloveDefMin = {lo};
constexpr int kLegacyGloveDefMax = {hi};

// Allocation-free membership test (these tables are tens of entries; a linear scan over a
// constexpr array costs less than the cache miss a jump table would take).
template <int N>
constexpr bool Contains(const int (&table)[N], int defIndex) {{
	for (int i = 0; i < N; ++i)
		if (table[i] == defIndex)
			return true;
	return false;
}}

}} // namespace CosmeticDefTables
}} // namespace Filmmaker
"""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", help="CS2 .../Counter-Strike Global Offensive/game directory")
    ap.add_argument("--check", action="store_true",
                    help="do not write; exit 1 if the checked-in .inc differs from the game files")
    args = ap.parse_args()

    game_dir = find_game_dir(args.game)
    build = read_steam_inf(game_dir)
    tables = ItemsGame(game_dir).classify()
    text = render(tables, build)

    for kind in ("knife", "glove", "primary", "secondary"):
        print(f"{kind:10} {len(tables[kind]):3} defs")
    print(f"CS2 build  ClientVersion={build.get('ClientVersion', '?')} "
          f"({build.get('VersionDate', '?')})")

    existing = OUT_PATH.read_text(encoding="utf-8") if OUT_PATH.is_file() else None
    if args.check:
        if existing == text:
            print(f"OK  {OUT_PATH.relative_to(REPO_ROOT)} is up to date.")
            return 0
        print(f"STALE  {OUT_PATH.relative_to(REPO_ROOT)} differs from the installed game files.")
        print("       Run: python Automation/tools/generate_cosmetic_defs.py")
        return 1

    if existing == text:
        print(f"unchanged: {OUT_PATH.relative_to(REPO_ROOT)}")
        return 0
    OUT_PATH.write_text(text, encoding="utf-8")
    print(f"wrote: {OUT_PATH.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
