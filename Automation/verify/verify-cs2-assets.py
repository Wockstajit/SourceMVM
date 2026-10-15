#!/usr/bin/env python3
"""Verify every ASSET NAME this project hard-codes against the installed CS2 game files.

Companion to verify-cs2-signatures.py, which covers byte patterns and nothing else. This tool
covers the other half of the update-breakage surface: the Valve-owned NAMES and NUMBERS we
match on. Both fail silently today -- a renamed particle just plays unswapped (the FX hook
fails OPEN by design), and an unknown item-definition index is simply never classified.

    python Automation/verify/verify-cs2-assets.py
    python Automation/verify/verify-cs2-assets.py --game "C:/Program Files (x86)/Steam/.../game"
    python Automation/verify/verify-cs2-assets.py --fx        # only the particle checks
    python Automation/verify/verify-cs2-assets.py --defs      # only the item-definition checks

Exit code 0 when everything resolves, 1 when anything is broken or out of sync.

WHAT IT CHECKS

  [FX/stock]  Every stock CS2 particle path the swap rules match on still exists in the game
              VPK (compiled entries are "<path>_c"). A miss means that rule is dead: the
              vanilla effect plays and nothing anywhere says so.
  [FX/prefix] Every prefix rule (e.g. "particles/impact_fx/") still matches at least one
              shipped particle, so a whole category cannot silently stop classifying.
  [FX/ours]   Every replacement target the rules can REACH exists in the built pack. Missing
              targets are the "white/error quad" class of bug. Skipped when no pack is built.

              Precision limit, stated deliberately: a rule carries both a lessImpacts and a
              lessSmoke target but only ONE is reachable -- ParticleFxRules.cpp picks by
              category (`cat == kFxImpacts ? rule.lessImpacts : rule.lessSmoke`), and the
              built pack only ships less/impacts for impact_fx. Re-deriving that category
              mapping here would create exactly the kind of second copy this pass exists to
              delete, so the check requires AT LEAST ONE of the two to exist. It therefore
              catches "Less mode has no asset at all" but not "the other one of the pair is
              the missing one".
  [DEFS]      The generated CosmeticDefTables.inc still matches items_game.txt, and the other
              hand-maintained copies of the same data agree with it:
                Automation/tools/weapon_skin_database.py   (SECONDARY/PRIMARY/KNIFE/GLOVE)
                Automation/tools/generate_cosmetics_catalog.py (WEAPONS def indices)
                AfxHookSource2/.../CosmeticGloveSkinTable.cpp  (glove defs used by the picker)

Regenerate the tables with: python Automation/tools/generate_cosmetic_defs.py
Background and the full update runbook: Docs/agent/cs2-update-recovery.md
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "Automation" / "tools"
FX_DIR = REPO_ROOT / "AfxHookSource2" / "Filmmaker" / "Movie"
COSMETICS_DIR = REPO_ROOT / "AfxHookSource2" / "Filmmaker" / "Cosmetics"
DEF_TABLES_INC = COSMETICS_DIR / "CosmeticDefTables.inc"

# The pack the FX rules point at, in preference order (built tree, then the staged release).
PACK_DIRS = [
    REPO_ROOT / "build" / "source2-release" / "fx" / "source_mvm_fx",
    REPO_ROOT / "build" / "staging-release" / "fx" / "source_mvm_fx",
    REPO_ROOT / "build" / "fx" / "fx-source1import" / "source2" / "game" / "source_mvm_fx",
    # pre-2026-09-01 converter output dir, kept so an older build tree still resolves
    REPO_ROOT / "build" / "fx" / "povarehok-source1import" / "source2" / "game" / "source_mvm_fx",
]

sys.path.insert(0, str(TOOLS_DIR))


# --------------------------------------------------------------------------- source scanning

def strip_comments(text: str) -> str:
    """Drop // and /* */ so a path parked in a comment cannot fail the run."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def collect_fx_names(verbose: bool):
    """Return (stock_exact, stock_prefixes, our_targets) from the particle-FX sources.

    Our own targets are ASSEMBLED by the FXRULE* macros rather than written as whole literals,
    so the macro invocations are expanded here exactly the way ParticleFxRules.cpp defines
    them. Everything else is picked up as a plain string literal.
    """
    stock_exact, stock_prefix, ours = {}, {}, {}
    less_pairs = []  # (rel, lessImpacts, lessSmoke) -- only one of the two is ever reachable
    runtime_only = set()  # names the engine creates that ship as no standalone VPK entry

    for path in sorted(FX_DIR.glob("ParticleFx*.cpp")) + sorted(FX_DIR.glob("ParticleFx*.h")):
        raw = path.read_text(encoding="utf-8", errors="replace")
        rel = path.relative_to(REPO_ROOT).as_posix()

        # Opt-out marker, read BEFORE comments are stripped. A particle name can be created at
        # runtime without shipping as its own VPK entry (embedded child system, or created by
        # name from code) -- env_explosion/explosion_grenade is a confirmed case, observed in
        # a demo. Such a rule is legitimate, so it must be annotated at the rule line rather
        # than the whole check being softened:
        #     FXRULE_MODERN("particles/...", ...), // fx-verify: runtime-only (why)
        for line in raw.splitlines():
            if "fx-verify: runtime-only" in line:
                for m in re.finditer(r'"(particles/[^"]*\.vpcf)"', line):
                    runtime_only.add(m.group(1))

        text = strip_comments(raw)

        # FXRULE(match, pack, name) / FXRULE_MODERN(match, pack, name, modernRel) / _BP(...)
        for m in re.finditer(
            r'FXRULE(_MODERN_BP|_MODERN)?\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"'
            r'(?:\s*,\s*"([^"]+)")?\s*\)',
            text,
        ):
            kind, match_key, pack, name, modern_rel = m.groups()
            stock_exact.setdefault(match_key, set()).add(rel)
            ours.setdefault(
                f"particles/filmmaker/povarehok/regular/{pack}/{name}.vpcf", set()).add(rel)
            less_pairs.append((
                rel,
                f"particles/filmmaker/povarehok/less/impacts/{pack}/{name}.vpcf",
                f"particles/filmmaker/povarehok/less/smoke/{pack}/{name}.vpcf",
            ))
            if kind == "_MODERN" and modern_rel:
                ours.setdefault(f"particles/filmmaker/modern/{modern_rel}.vpcf", set()).add(rel)

        # Plain literals: exact stock names (strcmp rules), prefixes (StartsWith rules), and
        # any of our own paths written out in full.
        for m in re.finditer(r'"(particles/[^"]*)"', text):
            value = m.group(1)
            if value.endswith("/"):
                # A macro fragment ("particles/filmmaker/povarehok/" in FXVAR) or a category
                # prefix. Ours are not real paths; stock ones are StartsWith rules.
                if not value.startswith("particles/filmmaker/"):
                    stock_prefix.setdefault(value, set()).add(rel)
            elif value.startswith("particles/filmmaker/"):
                ours.setdefault(value, set()).add(rel)
            elif value.endswith(".vpcf"):
                stock_exact.setdefault(value, set()).add(rel)
            else:
                stock_prefix.setdefault(value, set()).add(rel)

    if verbose:
        print(f"  scanned {FX_DIR.relative_to(REPO_ROOT).as_posix()}: {len(stock_exact)} stock, "
              f"{len(stock_prefix)} prefixes, {len(ours)} targets, {len(less_pairs)} less-pairs, "
              f"{len(runtime_only)} runtime-only")
    return stock_exact, stock_prefix, ours, less_pairs, runtime_only


# ------------------------------------------------------------------------------- table parsing

def parse_inc_tables(path: Path) -> dict:
    if not path.is_file():
        return {}
    text = path.read_text(encoding="utf-8", errors="replace")
    out = {}
    for key, name in (("knife", "kKnifeDefs"), ("glove", "kGloveDefs"),
                      ("primary", "kPrimaryDefs"), ("secondary", "kSecondaryDefs")):
        m = re.search(rf"{name}\[\]\s*=\s*\{{(.*?)\}}", text, flags=re.S)
        out[key] = {int(v) for v in re.findall(r"\d+", m.group(1))} if m else set()
    return out


def parse_python_sets(path: Path) -> dict:
    """Parse hand-typed def sets, or report that the file DERIVES them from the .inc.

    A derived copy is not a copy at all, which is the goal -- but keep parsing so that
    re-hardcoding the literals is caught rather than silently accepted.
    """
    text = path.read_text(encoding="utf-8", errors="replace")
    out = {"derived": "CosmeticDefTables.inc" in text and "_load_def_tables" in text}
    for key, name in (("secondary", "SECONDARY_DEFS"), ("primary", "PRIMARY_DEFS"),
                      ("knife", "KNIFE_DEFS")):
        m = re.search(rf"^{name}\s*=\s*\{{(.*?)\}}", text, flags=re.S | re.M)
        out[key] = {int(v) for v in re.findall(r"\d+", m.group(1))} if m else set()
    m = re.search(r"GLOVE_DEF_MIN,\s*GLOVE_DEF_MAX\s*=\s*(\d+),\s*(\d+)", text)
    out["glove_range"] = (int(m.group(1)), int(m.group(2))) if m else None
    return out


def parse_catalog_weapon_defs(path: Path) -> set:
    """generate_cosmetics_catalog.py's WEAPONS list: ("weapon_x", "Display", <def>, "slot")."""
    text = path.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"^WEAPONS\s*=\s*\[(.*?)^\]", text, flags=re.S | re.M)
    if not m:
        return set()
    return {int(d) for d in re.findall(r'\(\s*"weapon_[^"]*"\s*,\s*"[^"]*"\s*,\s*(\d+)', m.group(1))}


def parse_glove_table_defs(path: Path) -> set:
    text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    return {int(d) for d in re.findall(r"\{\s*(\d+)\s*,\s*\d+\s*,", text)}


# ------------------------------------------------------------------------------------- checks

class Report:
    def __init__(self):
        self.failures = 0
        self.checks = 0

    def ok(self, label: str, detail: str = ""):
        self.checks += 1
        print(f"OK      {label}{('  ' + detail) if detail else ''}")

    def broken(self, label: str, detail: str = ""):
        self.checks += 1
        self.failures += 1
        print(f"BROKEN  {label}{('  ' + detail) if detail else ''}")

    def note(self, label: str, detail: str = ""):
        print(f"note    {label}{('  ' + detail) if detail else ''}")


def check_fx(report: Report, vpk_entries: set, verbose: bool):
    stock_exact, stock_prefix, ours, less_pairs, runtime_only = collect_fx_names(verbose)

    missing = sorted(p for p in stock_exact
                     if f"{p}_c" not in vpk_entries and p not in runtime_only)
    if missing:
        report.broken(f"[FX/stock] {len(missing)}/{len(stock_exact)} matched particles are GONE "
                      f"from the CS2 VPK -- those swap rules are dead:")
        for p in missing:
            print(f"          {p}   ({', '.join(sorted(stock_exact[p]))})")
        print("          If the engine creates one of these WITHOUT shipping it as its own VPK")
        print("          entry, annotate that rule line: // fx-verify: runtime-only (why)")
    else:
        report.ok(f"[FX/stock] all {len(stock_exact)} matched stock particles resolve",
                  f"({len(runtime_only)} annotated runtime-only)" if runtime_only else "")

    # An annotation that stops being necessary is stale documentation; say so.
    stale_marks = sorted(p for p in runtime_only if f"{p}_c" in vpk_entries)
    if stale_marks:
        report.note("[FX/stock] these are marked runtime-only but DO ship in the VPK now; "
                    "the annotation can go:", ", ".join(stale_marks))

    dead_prefixes = []
    for prefix in stock_prefix:
        if not any(e.startswith(prefix) for e in vpk_entries):
            dead_prefixes.append(prefix)
    if dead_prefixes:
        report.broken(f"[FX/prefix] {len(dead_prefixes)} category prefixes match NOTHING shipped:")
        for p in sorted(dead_prefixes):
            print(f"          {p}   ({', '.join(sorted(stock_prefix[p]))})")
    else:
        report.ok(f"[FX/prefix] all {len(stock_prefix)} category prefixes still match shipped particles")

    pack = next((d for d in PACK_DIRS if (d / "particles").is_dir()), None)
    if pack is None:
        report.note("[FX/ours] no built FX pack on disk -- target check skipped "
                    "(build it with FX/tools/convert-fx-source1.ps1 -Compile)")
        return
    have = {p.relative_to(pack).as_posix() for p in pack.rglob("*.vpcf_c")}
    packname = pack.relative_to(REPO_ROOT).as_posix()
    missing_targets = sorted(t for t in ours if f"{t}_c" not in have)
    if missing_targets:
        report.broken(f"[FX/ours] {len(missing_targets)}/{len(ours)} replacement targets are "
                      f"NOT in the built pack ({packname}):")
        for t in missing_targets[:40]:
            print(f"          {t}   ({', '.join(sorted(ours[t]))})")
        if len(missing_targets) > 40:
            print(f"          ... and {len(missing_targets) - 40} more")
    else:
        report.ok(f"[FX/ours] all {len(ours)} replacement targets exist in the built pack")

    # See the precision-limit note in the module docstring: exactly one of the pair is
    # reachable per rule, so require at least one to exist rather than both.
    orphan_less = [(rel, a, b) for rel, a, b in less_pairs
                   if f"{a}_c" not in have and f"{b}_c" not in have]
    if orphan_less:
        report.broken(f"[FX/less] {len(orphan_less)}/{len(less_pairs)} rules have NEITHER Less "
                      f"variant in the built pack (Less mode falls through to vanilla):")
        for rel, a, b in orphan_less[:20]:
            print(f"          {a}\n          {b}   ({rel})")
        if len(orphan_less) > 20:
            print(f"          ... and {len(orphan_less) - 20} more")
    else:
        report.ok(f"[FX/less] all {len(less_pairs)} rules have a reachable Less variant")


def check_defs(report: Report, game_dir: Path, verbose: bool):
    from generate_cosmetic_defs import ItemsGame  # same classification rules as the generator

    live = ItemsGame(game_dir).classify()
    inc = parse_inc_tables(DEF_TABLES_INC)
    if not inc:
        report.broken(f"[DEFS] {DEF_TABLES_INC.relative_to(REPO_ROOT).as_posix()} is missing -- "
                      "run: python Automation/tools/generate_cosmetic_defs.py")
        return

    stale = False
    for kind in ("knife", "glove", "primary", "secondary"):
        added = sorted(live[kind] - inc[kind])
        removed = sorted(inc[kind] - live[kind])
        if added or removed:
            stale = True
            detail = []
            if added:
                detail.append("NEW in game: " + ", ".join(
                    f"{d} ({live['names'].get(d, '?')})" for d in added))
            if removed:
                detail.append("gone from game: " + ", ".join(str(d) for d in removed))
            report.broken(f"[DEFS/{kind}] CosmeticDefTables.inc is stale", "; ".join(detail))
    if not stale:
        report.ok("[DEFS] CosmeticDefTables.inc matches items_game.txt", ", ".join(
            f"{k}={len(inc[k])}" for k in ("knife", "glove", "primary", "secondary")))

    # Copy 2: the Python skin database. It now READS the generated header instead of mirroring
    # it, so there is nothing to drift -- verify that it still does, rather than assuming.
    py = parse_python_sets(TOOLS_DIR / "weapon_skin_database.py")
    if py.get("derived"):
        report.ok("[DEFS/copy] weapon_skin_database.py derives its tables from "
                  "CosmeticDefTables.inc (no copy to drift)")
    else:
        mismatched = [k for k in ("knife", "primary", "secondary") if py.get(k) != inc[k]]
        if mismatched:
            for k in mismatched:
                report.broken(f"[DEFS/copy] weapon_skin_database.py {k.upper()}_DEFS disagrees",
                              f"only-in-py={sorted(py.get(k, set()) - inc[k])} "
                              f"only-in-inc={sorted(inc[k] - py.get(k, set()))}")
        else:
            report.ok("[DEFS/copy] weapon_skin_database.py agrees with the generated tables")

        if py.get("glove_range"):
            lo, hi = py["glove_range"]
            outside = sorted(d for d in inc["glove"] if not lo <= d <= hi)
            if outside:
                report.broken("[DEFS/copy] weapon_skin_database.py glove RANGE misses real gloves",
                              f"{outside} fall outside {lo}..{hi}")
            else:
                report.ok("[DEFS/copy] weapon_skin_database.py glove range covers all gloves")

    # Copy 3: the catalog generator's own weapon list (guns only, no knives/gloves).
    catalog_defs = parse_catalog_weapon_defs(TOOLS_DIR / "generate_cosmetics_catalog.py")
    guns = inc["primary"] | inc["secondary"]
    unknown = sorted(catalog_defs - guns - inc["knife"] - inc["glove"])
    missing = sorted(guns - catalog_defs)
    if unknown or missing:
        report.broken("[DEFS/copy] generate_cosmetics_catalog.py WEAPONS disagrees",
                      f"unknown-defs={unknown} missing-guns={missing}")
    else:
        report.ok(f"[DEFS/copy] generate_cosmetics_catalog.py covers all {len(guns)} gun defs")

    # Copy 4: the glove picker table must only name defs we classify as gloves.
    picker = parse_glove_table_defs(COSMETICS_DIR / "CosmeticGloveSkinTable.cpp")
    unclassified = sorted(picker - inc["glove"])
    if unclassified:
        report.broken("[DEFS/copy] CosmeticGloveSkinTable.cpp offers gloves the classifier "
                      "does not recognise", f"{unclassified}")
    else:
        report.ok(f"[DEFS/copy] all {len(picker)} picker glove defs classify as gloves")


# --------------------------------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", help="CS2 .../Counter-Strike Global Offensive/game directory")
    ap.add_argument("--fx", action="store_true", help="only the particle checks")
    ap.add_argument("--defs", action="store_true", help="only the item-definition checks")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    from generate_cosmetic_defs import find_game_dir, read_steam_inf
    from generate_cosmetics_catalog import Vpk

    game_dir = find_game_dir(args.game)
    build = read_steam_inf(game_dir)
    print(f"CS2 {game_dir}")
    print(f"    ClientVersion={build.get('ClientVersion', '?')} "
          f"PatchVersion={build.get('PatchVersion', '?')} ({build.get('VersionDate', '?')})\n")

    report = Report()
    run_fx = args.fx or not args.defs
    run_defs = args.defs or not args.fx

    if run_fx:
        vpk = Vpk(game_dir / "csgo" / "pak01_dir.vpk")
        check_fx(report, set(vpk.entries.keys()), args.verbose)
    if run_defs:
        check_defs(report, game_dir, args.verbose)

    print()
    if report.failures:
        print(f"=== {report.failures}/{report.checks} checks BROKEN ===")
        print("    Runbook: Docs/agent/cs2-update-recovery.md")
        return 1
    print(f"=== {report.checks}/{report.checks} checks OK ===")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
