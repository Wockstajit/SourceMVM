"""Extract the real per-hero skeleton bone-name lists from Deadlock's shipped hero models.

Feeds Automation/data/deadlock-hero-bones.json, which Automation/tools/deadlock-hero-bones.py
(the `resolve_mount` fallback-chain resolver) and its test load at runtime. Re-run this after a
Deadlock update — hero rosters change, models get reworked, and bone names occasionally change
with them.

What it does, end to end:

  1. Decompiles `scripts/heroes.vdata_c` out of the game VPK with Source2Viewer-CLI and parses the
     `hero_<codename> = { ... }` records it contains for `m_bPlayerSelectable`, `m_bDisabled`, and
     `m_strModelName`. Only records with `m_bPlayerSelectable == true` and `m_bDisabled == false`
     are kept -- this is the same flag pair the game itself would use to decide who is actually
     spawnable, and it is far more reliable than guessing from folder names: hero body models are
     split across `models/heroes_staging/` and `models/heroes_wip/` with no naming convention that
     tells the two apart, several heroes share a directory with an unrelated one (three different
     `hero_*` records point at `chrono.vmdl`, only one of which is enabled), and some hero folders
     contain a *pet's* model rather than the hero's own body (Grey Talon's own body model is under
     `models/heroes_staging/archer/`, its internal codename `orion`; the `grey_talon` folder holds
     only the owl companion). The `hero_<codename>` record key IS the short name the running game
     exposes at runtime through `CitadelHeroData_t::m_strHeroSearchName` (a `#hero_<codename>_search`
     loc token) -- see AfxHookSource2/Deadlock/DeadlockEntities.cpp's `heroSearchName` field -- so a
     later live feature can key straight off that string with no separate name-mapping table.

  2. For each surviving hero, decompiles just that hero's `.vmdl_c` DATA block (`-b DATA`, which
     is the resource's own top-level model data and appears once, unlike `-a`/`--all` which repeats
     the skeleton once per LOD/sub-mesh) and reads the `m_modelSkeleton.m_boneName` string array --
     the model's actual compiled skeleton, full stop; no glTF export, no animation decompile needed.

  3. Writes the result as JSON: hero short name -> {model path, full bone name list}.

Source2Viewer-CLI.exe is invoked as an external process (never linked against VRF), so this script
has no build step of its own -- it just needs a built Source2Viewer-CLI.exe and the Deadlock VPK.

Usage:

  py -3 Automation/tools/deadlock-hero-bones-extract.py
  py -3 Automation/tools/deadlock-hero-bones-extract.py --hero orion --hero krill  (subset, for iterating)
  py -3 Automation/tools/deadlock-hero-bones-extract.py --out some/other/path.json --keep-temp

Path resolution (each overridable on the command line):

  --cli       Source2Viewer-CLI.exe. Defaults to $SOURCEMVM_VRF_CLI, else a ValveResourceFormat
              checkout next to this repository (VRF is an external tool this repo does not vendor
              or build).
  --vpk       Deadlock's citadel/pak01_dir.vpk. Defaults to the first Steam-resolved Deadlock
              install (via Automation/lib/deadlock_paths.py, the same resolver the rest of
              Automation/ uses) plus "game/citadel/pak01_dir.vpk", overridable directly or via
              $SOURCEMVM_DEADLOCK_DIR (consumed by that resolver).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "Automation" / "lib"))

# Source2Viewer-CLI (ValveResourceFormat): a checkout next to this repository, which is the
# documented layout; set SOURCEMVM_VRF_CLI to point elsewhere.
DEFAULT_CLI = os.environ.get("SOURCEMVM_VRF_CLI") or str(
    Path(__file__).resolve().parents[2].parent / "ValveResourceFormat" / "CLI" / "bin" / "Release" / "Source2Viewer-CLI.exe")
HEROES_VDATA_PATH = "scripts/heroes.vdata_c"
DEFAULT_OUT = REPO_ROOT / "Automation" / "data" / "deadlock-hero-bones.json"

HERO_RECORD_RE = re.compile(r"^\thero_([A-Za-z0-9_]+) = *$")
FIELD_RE = {
    "selectable": re.compile(r"^\t\tm_bPlayerSelectable = (true|false)$"),
    "disabled": re.compile(r"^\t\tm_bDisabled = (true|false)$"),
    "model": re.compile(r'^\t\tm_strModelName = resource_name:"(.+)"$'),
}


@dataclass
class HeroRecord:
    short_name: str
    selectable: bool | None = None
    disabled: bool | None = None
    model_vmdl: str | None = None

    @property
    def is_playable(self) -> bool:
        return self.selectable is True and self.disabled is False and self.model_vmdl is not None


@dataclass
class ExtractionIssue:
    hero: str
    detail: str


def default_vpk_path() -> Path | None:
    """Best-guess citadel/pak01_dir.vpk from the Steam-resolved Deadlock install, if any."""
    try:
        from deadlock_paths import deadlock_game_dirs
    except ImportError:
        return None
    for game_dir in deadlock_game_dirs():
        candidate = Path(game_dir) / "citadel" / "pak01_dir.vpk"
        if candidate.is_file():
            return candidate
    return None


def run_cli(cli_path: Path, args: list[str]) -> str:
    """Runs Source2Viewer-CLI.exe with the given arguments and returns its stdout as text."""
    proc = subprocess.run(
        [str(cli_path), *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"Source2Viewer-CLI exited {proc.returncode} for args {args}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    return proc.stdout


def parse_hero_records(vdata_text: str) -> list[HeroRecord]:
    """Parses the `hero_<codename> = { ... }` records out of decompiled scripts/heroes.vdata.

    This is a targeted line scanner, not a full KV3 parser: it only reads the handful of
    top-level-within-the-record scalar fields it needs (two tabs of indent, matching the exact
    shape Source2Viewer-CLI's decompiler emits for this file today) and ignores everything else,
    including the much deeper nested stat/upgrade-weight maps each record also contains.
    """
    records: list[HeroRecord] = []
    current: HeroRecord | None = None
    for raw_line in vdata_text.splitlines():
        m = HERO_RECORD_RE.match(raw_line)
        if m:
            if current is not None:
                records.append(current)
            current = HeroRecord(short_name=m.group(1))
            continue
        if current is None:
            continue
        sel = FIELD_RE["selectable"].match(raw_line)
        if sel and current.selectable is None:
            current.selectable = sel.group(1) == "true"
            continue
        dis = FIELD_RE["disabled"].match(raw_line)
        if dis and current.disabled is None:
            current.disabled = dis.group(1) == "true"
            continue
        mod = FIELD_RE["model"].match(raw_line)
        if mod and current.model_vmdl is None:
            current.model_vmdl = mod.group(1)
    if current is not None:
        records.append(current)
    return records


def extract_bone_names(data_block_text: str) -> list[str] | None:
    """Pulls the `m_modelSkeleton.m_boneName` string array out of a `-b DATA` dump.

    Returns None if no skeleton block is present at all (rather than raising), so the caller can
    report a clean per-hero miss instead of the whole run dying on one odd model.
    """
    skel_idx = data_block_text.find("m_modelSkeleton")
    if skel_idx == -1:
        return None
    bone_idx = data_block_text.find("m_boneName", skel_idx)
    if bone_idx == -1:
        return None
    array_start = data_block_text.find("[", bone_idx)
    if array_start == -1:
        return None
    array_end = data_block_text.find("\n\t\t\t]", array_start)  # matches the array's own indent
    if array_end == -1:
        array_end = data_block_text.find("]", array_start)
    array_body = data_block_text[array_start:array_end]
    return re.findall(r'"([^"]*)"', array_body)


def vmdl_to_vmdl_c(vmdl_path: str) -> str:
    if vmdl_path.endswith(".vmdl_c"):
        return vmdl_path
    return vmdl_path + "_c"


def extract(
    cli_path: Path,
    vpk_path: Path,
    hero_filter: set[str] | None,
) -> tuple[dict[str, dict], list[ExtractionIssue]]:
    issues: list[ExtractionIssue] = []

    with tempfile.TemporaryDirectory(prefix="deadlock-hero-bones-") as tmp:
        tmp_dir = Path(tmp)
        run_cli(cli_path, ["-i", str(vpk_path), "-f", HEROES_VDATA_PATH, "-d", "-o", str(tmp_dir)])
        vdata_out = tmp_dir / "scripts" / "heroes.vdata"
        if not vdata_out.is_file():
            raise RuntimeError(f"expected decompiled heroes.vdata at {vdata_out}, it was not written")
        vdata_text = vdata_out.read_text(encoding="utf-8", errors="replace")

    records = parse_hero_records(vdata_text)
    playable = [r for r in records if r.is_playable]
    if not playable:
        raise RuntimeError("parsed 0 playable hero records out of heroes.vdata -- format likely changed")

    if hero_filter is not None:
        unknown = hero_filter - {r.short_name for r in playable}
        for name in sorted(unknown):
            issues.append(ExtractionIssue(name, "not found among playable hero records"))
        playable = [r for r in playable if r.short_name in hero_filter]

    heroes: dict[str, dict] = {}
    for record in sorted(playable, key=lambda r: r.short_name):
        model_vmdl_c = vmdl_to_vmdl_c(record.model_vmdl)
        try:
            data_block = run_cli(cli_path, ["-i", str(vpk_path), "-f", model_vmdl_c, "-b", "DATA"])
        except RuntimeError as exc:
            issues.append(ExtractionIssue(record.short_name, f"CLI failed on {model_vmdl_c}: {exc}"))
            continue
        bones = extract_bone_names(data_block)
        if not bones:
            issues.append(ExtractionIssue(record.short_name, f"no m_modelSkeleton.m_boneName found in {model_vmdl_c}"))
            continue
        heroes[record.short_name] = {
            "modelPath": record.model_vmdl,
            "boneCount": len(bones),
            "bones": bones,
        }

    return heroes, issues


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cli", default=DEFAULT_CLI, help="Path to Source2Viewer-CLI.exe")
    ap.add_argument("--vpk", default=None, help="Path to citadel/pak01_dir.vpk (default: auto-resolve)")
    ap.add_argument("--out", default=str(DEFAULT_OUT), help="Where to write the JSON data file")
    ap.add_argument("--hero", action="append", default=None,
                     help="Restrict extraction to this hero short name (repeatable); default: all playable heroes")
    args = ap.parse_args(argv)

    cli_path = Path(args.cli)
    if not cli_path.is_file():
        print(f"Source2Viewer-CLI.exe not found at {cli_path}\n"
              f"pass --cli or set $SOURCEMVM_VRF_CLI", file=sys.stderr)
        return 2

    vpk_path = Path(args.vpk) if args.vpk else default_vpk_path()
    if vpk_path is None or not vpk_path.is_file():
        print(f"could not resolve citadel/pak01_dir.vpk (tried: {vpk_path}); pass --vpk explicitly",
              file=sys.stderr)
        return 2

    hero_filter = set(args.hero) if args.hero else None

    heroes, issues = extract(cli_path, vpk_path, hero_filter)

    for issue in issues:
        print(f"MISS  {issue.hero}: {issue.detail}", file=sys.stderr)

    all_bones: set[str] = set()
    for h in heroes.values():
        all_bones.update(h["bones"])

    payload = {
        "$schema": "deadlock-hero-bones-v1",
        "generatedBy": "Automation/tools/deadlock-hero-bones-extract.py",
        "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "vpk": str(vpk_path),
        "heroesVdataPath": HEROES_VDATA_PATH,
        "heroCount": len(heroes),
        "uniqueBoneNameCount": len(all_bones),
        "heroes": heroes,
    }

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    print(f"wrote {out_path} : {len(heroes)} heroes, {len(all_bones)} unique bone names, "
          f"{len(issues)} miss(es)")
    return 1 if issues else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
