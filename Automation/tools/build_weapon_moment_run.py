#!/usr/bin/env python3
"""Post-process scanned weapon moments: dedupe, pair legacy/cs2, write reports + run file."""
from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
from weapon_skin_database import get_database, slot_for_def  # noqa: E402
from select_mesh_verification_cases import select_mesh_verification_cases, ownership_type  # noqa: E402

TRANSITION_ANIMS = {
    "weapon_switch", "player_switch", "demo_seek", "loadout_change",
    "primary_to_secondary", "secondary_to_primary", "knife_draw", "weapon_draw",
    "legacy_to_cs2", "cs2_to_legacy", "default_to_skinned", "skinned_to_default",
}


def moment_key(m: Dict[str, Any]) -> Tuple:
    return (
        m.get("tick"),
        m.get("playerSteamId"),
        m.get("weaponDefIndex"),
        m.get("paintIndex"),
        m.get("animationState"),
        m.get("activeWeapon"),
    )


def score_moment(m: Dict[str, Any]) -> int:
    s = 0
    if m.get("activeWeapon"):
        s += 50
    if m.get("pairedComparison"):
        s += 40
    if m.get("firstPersonUsable"):
        s += 20
    if m.get("weaponSwitchUsable") or m.get("playerSwitchUsable"):
        s += 15
    anim = m.get("animationState", "")
    if anim in ("weapon_switch", "demo_seek", "player_switch"):
        s += 12
    if anim == "idle" and m.get("activeWeapon"):
        s += 8
    if m.get("expectLegacyModel") or (m.get("paintIndex", 0) > 0 and not m.get("isDefaultSkin")):
        s += 5
    return s


def detect_transitions(moments: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """Infer transition moments from ordered snapshots per player."""
    by_player: Dict[str, List[Dict[str, Any]]] = defaultdict(list)
    for m in moments:
        by_player[str(m.get("playerSteamId", ""))].append(m)
    extra: List[Dict[str, Any]] = []
    for steam, rows in by_player.items():
        rows.sort(key=lambda r: (r.get("tick", 0), r.get("weaponDefIndex", 0)))
        prev = None
        for m in rows:
            if not m.get("activeWeapon"):
                prev = m
                continue
            if prev and prev.get("activeWeapon"):
                prev_slot = prev.get("weaponCategory")
                cur_slot = m.get("weaponCategory")
                prev_paint = prev.get("paintIndex", 0)
                cur_paint = m.get("paintIndex", 0)
                prev_model = prev.get("expectedModelType")
                cur_model = m.get("expectedModelType")
                trans = None
                if prev_slot == "primary" and cur_slot == "secondary":
                    trans = "primary_to_secondary"
                elif prev_slot == "secondary" and cur_slot == "primary":
                    trans = "secondary_to_primary"
                elif prev_slot != "knife" and cur_slot == "knife":
                    trans = "knife_draw"
                elif prev_slot == "knife" and cur_slot in ("primary", "secondary"):
                    trans = "weapon_draw"
                elif prev.get("weaponDefIndex") != m.get("weaponDefIndex"):
                    trans = "weapon_switch"
                elif prev_paint <= 0 and cur_paint > 0:
                    trans = "default_to_skinned"
                elif prev_paint > 0 and cur_paint <= 0:
                    trans = "skinned_to_default"
                elif prev_model == "legacy" and cur_model == "cs2":
                    trans = "legacy_to_cs2"
                elif prev_model == "cs2" and cur_model == "legacy":
                    trans = "cs2_to_legacy"
                if trans:
                    nm = dict(m)
                    nm["animationState"] = trans
                    nm["transitionType"] = trans
                    nm["weaponSwitchUsable"] = True
                    nm["reason"] = f"transition {trans} at tick {m.get('tick')}"
                    extra.append(nm)
            prev = m
    return extra


def apply_paired_comparisons(moments: List[Dict[str, Any]], db) -> None:
    by_weapon: Dict[int, Dict[str, Set[str]]] = defaultdict(lambda: {"legacy": set(), "cs2": set(), "default": set()})
    for m in moments:
        if not m.get("activeWeapon"):
            continue
        d = int(m.get("weaponDefIndex", 0))
        if d <= 0:
            continue
        mt = m.get("expectedModelType")
        if m.get("isDefaultSkin"):
            by_weapon[d]["default"].add(str(m.get("playerSteamId")))
        elif mt == "legacy":
            by_weapon[d]["legacy"].add(str(m.get("playerSteamId")))
        elif mt == "cs2":
            by_weapon[d]["cs2"].add(str(m.get("playerSteamId")))

    for m in moments:
        d = int(m.get("weaponDefIndex", 0))
        if d <= 0:
            continue
        g = by_weapon[d]
        has_l = bool(g["legacy"])
        has_c = bool(g["cs2"]) or bool(g["default"])
        paired = has_l and has_c and db.has_both_model_types(db.weapon_name_for_def(d))
        m["pairedComparisonKey"] = f"def_{d}"
        m["pairedComparison"] = {
            "hasLegacySkin": has_l,
            "hasCs2OrDefaultSkin": has_c,
            "isPairedCase": paired,
            "legacyPlayers": sorted(g["legacy"]),
            "cs2Players": sorted(g["cs2"] | g["default"]),
        }


def select_moments(all_moments: List[Dict[str, Any]], max_per_weapon: int = 2, max_total: int = 40) -> List[Dict[str, Any]]:
    db = get_database()
    return select_mesh_verification_cases(
        all_moments, db, max_per_weapon=max_per_weapon, max_total=max_total,
    )


def coverage_summary(moments: List[Dict[str, Any]], db) -> Dict[str, Any]:
    weapons: Dict[str, Dict[str, Any]] = {}
    for m in moments:
        if not m.get("activeWeapon"):
            continue
        d = int(m.get("weaponDefIndex", 0))
        name = m.get("weaponName", db.weapon_name_for_def(d))
        w = weapons.setdefault(name, {
            "defIndex": d,
            "category": m.get("weaponCategory"),
            "legacySeen": False,
            "cs2Seen": False,
            "defaultSeen": False,
            "pairedInDemo": False,
            "animationStates": set(),
        })
        if m.get("isDefaultSkin"):
            w["defaultSeen"] = True
        elif m.get("expectLegacyModel"):
            w["legacySeen"] = True
        else:
            w["cs2Seen"] = True
        if m.get("pairedComparison", {}).get("isPairedCase"):
            w["pairedInDemo"] = True
        w["animationStates"].add(m.get("animationState"))

    covered = []
    missing_paired = []
    default_only = []
    for name, w in sorted(weapons.items()):
        w["animationStates"] = sorted(w["animationStates"])
        covered.append({**w, "weaponName": name})
        if db.has_both_model_types(name) and not w["pairedInDemo"]:
            if w["legacySeen"] ^ w["cs2Seen"]:
                missing_paired.append(name)
        if w["defaultSeen"] and not w["legacySeen"] and not w["cs2Seen"]:
            default_only.append(name)

    return {
        "weaponsSeen": covered,
        "missingPairedCoverage": missing_paired,
        "defaultOnlyWeapons": default_only,
    }


def human_report(moments: List[Dict[str, Any]]) -> str:
    lines = ["=== Weapon model verification moments ===", ""]
    by_weapon: Dict[str, List[Dict[str, Any]]] = defaultdict(list)
    for m in moments:
        by_weapon[m.get("weaponName", "?")].append(m)
    for weapon, rows in sorted(by_weapon.items()):
        lines.append(f"Weapon: {weapon}")
        lines.append("")
        for m in sorted(rows, key=lambda r: r.get("tick", 0)):
            lines.extend([
                f"Player: {m.get('playerName') or m.get('playerSteamId')}",
                f"Holder steam: {m.get('playerSteamId')}",
                f"Owner steam: {m.get('weaponOwnerSteamId') or m.get('ownerSteamId') or '(same)'}",
                f"Ownership: {m.get('ownershipType', ownership_type(m))}",
                f"Weapon entity: {m.get('weaponEntityId')}",
                f"Tick: {m.get('tick')}",
                f"Skin: {m.get('skinName')}",
                f"Paint index: {m.get('paintIndex')}",
                f"Expected model: {m.get('expectedModelType')}",
                f"Animation: {m.get('animationState')}",
                f"First-person usable: {m.get('firstPersonUsable')}",
                f"Third-person usable: {m.get('thirdPersonUsable')}",
                f"HUD/UI usable: {m.get('hudUiUsable')}",
                f"Why this is a good test case: {m.get('reason')}",
                "",
            ])
        lines.append("---")
        lines.append("")
    return "\n".join(lines)


def build_verification_run(moments: List[Dict[str, Any]], scan_path: Path, db) -> Dict[str, Any]:
    steps: List[Dict[str, Any]] = []
    for i, m in enumerate(moments):
        mid = f"m{i:04d}"
        m["id"] = mid
        def_idx = int(m.get("weaponDefIndex", 0))
        paints = db.pick_test_paints(def_idx) if def_idx > 0 else None
        steps.append({"action": "seek", "tick": m.get("tick"), "momentId": mid})
        steps.append({"action": "spectate_steam", "steam": m.get("playerSteamId"), "momentId": mid})
        steps.append({"action": "setup_fp", "momentId": mid})
        steps.append({"action": "screenshot_fp_baseline", "tag": f"{mid}_baseline", "momentId": mid})
        steps.append({"action": "screenshot_tp_baseline", "tag": f"{mid}_tp_baseline", "momentId": mid})
        if paints:
            leg = paints["legacyPaint"]["id"]
            mod = paints["modernPaint"]["id"]
            steps.append({"action": "apply_paint", "defIndex": def_idx, "paint": leg, "momentId": mid, "expectModel": "legacy"})
            steps.append({"action": "screenshot_fp", "tag": f"{mid}_legacy_apply", "momentId": mid})
            steps.append({"action": "verify_paint_readback", "want": leg, "momentId": mid})
            steps.append({"action": "apply_paint", "defIndex": def_idx, "paint": mod, "momentId": mid, "expectModel": "cs2"})
            steps.append({"action": "screenshot_fp", "tag": f"{mid}_modern_apply", "momentId": mid})
            steps.append({"action": "verify_paint_readback", "want": mod, "momentId": mid})
            steps.append({"action": "mesh_switch_test", "defIndex": def_idx,
                          "legacyPaint": leg, "modernPaint": mod, "momentId": mid,
                          "ownershipType": m.get("ownershipType", "owned")})
        steps.append({"action": "flicker_smoke", "tick": m.get("tick"), "momentId": mid})
        if m.get("playerSwitchUsable"):
            steps.append({"action": "spec_next", "momentId": mid})
        if m.get("weaponSwitchUsable"):
            steps.append({"action": "note_weapon_switch", "momentId": mid})
    return {
        "runSchemaVersion": 1,
        "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "scanFile": str(scan_path),
        "momentCount": len(moments),
        "steps": steps,
        "moments": moments,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("scan_json", type=Path, help="Scan output JSON with raw moments")
    ap.add_argument("--max-per-weapon", type=int, default=2)
    ap.add_argument("--max-total", type=int, default=40)
    ap.add_argument("--out-dir", type=Path, default=None)
    args = ap.parse_args()

    doc = json.loads(args.scan_json.read_text(encoding="utf-8-sig"))
    db = get_database()
    raw = list(doc.get("moments", []))
    raw.extend(detect_transitions(raw))
    apply_paired_comparisons(raw, db)
    selected = select_moments(raw, max_per_weapon=args.max_per_weapon, max_total=args.max_total)
    # Selection groups by weapon def index, which would make VERIFY's demo seeks
    # bounce across the whole timeline between almost every case (tens of
    # thousands of ticks each time -- seeking is the slow part of this pipeline).
    # Sort by tick so consecutive cases are nearby and seeks stay short.
    selected.sort(key=lambda m: int(m.get("tick", 0) or 0))
    for i, m in enumerate(selected):
        m["id"] = f"m{i:04d}"

    out_dir = args.out_dir or args.scan_json.parent
    out_dir.mkdir(parents=True, exist_ok=True)
    demo = doc.get("demoName") or doc.get("demo", "demo")

    final = {
        "schemaVersion": 1,
        "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "demo": doc.get("demo"),
        "demoName": demo,
        "map": doc.get("map"),
        "moments": selected,
        "coverage": coverage_summary(selected, db),
        "rawMomentCount": len(raw),
        "selectedMomentCount": len(selected),
    }
    moments_path = out_dir / "moments.json"
    moments_path.write_text(json.dumps(final, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")

    report_path = out_dir / "verification_report.txt"
    report_path.write_text(human_report(selected), encoding="utf-8")

    run = build_verification_run(selected, moments_path, db)
    run_path = out_dir / "verification_run.json"
    run_path.write_text(json.dumps(run, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")

    print(f"Wrote {moments_path} ({len(selected)} moments from {len(raw)} raw)")
    print(f"Wrote {report_path}")
    print(f"Wrote {run_path}")
    cov = final["coverage"]
    print(f"  weapons={len(cov['weaponsSeen'])} missingPaired={len(cov['missingPairedCoverage'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
