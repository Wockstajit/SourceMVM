#!/usr/bin/env python3
"""Select mesh-verification cases: per weapon type, prefer owned + pickup holder moments."""
from __future__ import annotations

from collections import defaultdict
from typing import Any, Dict, List, Optional, Set, Tuple

from weapon_skin_database import WeaponSkinDatabase, get_database, is_verifiable_gun_def


def ownership_type(moment: Dict[str, Any]) -> str:
    owner = str(moment.get("weaponOwnerSteamId") or moment.get("ownerSteamId") or "")
    holder = str(moment.get("playerSteamId") or moment.get("holderSteamId") or "")
    if owner and holder and owner not in ("0", "") and holder not in ("0", "") and owner != holder:
        return "pickup"
    hint = moment.get("ownershipHint")
    if hint in ("pickup", "owned"):
        return hint
    return "owned"


def score_moment(m: Dict[str, Any]) -> int:
    s = 0
    if m.get("activeWeapon"):
        s += 50
    if int(m.get("paintIndex", 0) or 0) > 0:
        s += 25
    if m.get("hasNetworkedPaint"):
        s += 20
    anim = str(m.get("animationState", ""))
    if anim == "idle":
        s += 18
    elif anim in ("weapon_draw", "reload", "inspect"):
        s += 22
    elif anim == "weapon_switch":
        s += 10
    if ownership_type(m) == "pickup":
        s += 12
    if m.get("visibilityScore"):
        s += int(m["visibilityScore"])
    # settled=False means the weapon def was still changing a few ticks after this
    # moment was found -- i.e. "just switched, model may not have fully appeared
    # yet". Heavily deprioritize but never hard-exclude: it may be the only
    # candidate seen for this weapon type in the whole demo.
    if m.get("settled") is False:
        s -= 30
    return s


def supports_mesh_test(m: Dict[str, Any], db: WeaponSkinDatabase) -> bool:
    def_i = int(m.get("weaponDefIndex", 0) or 0)
    if def_i <= 0:
        return False
    if not is_verifiable_gun_def(def_i):
        return False
    return db.pick_test_paints(def_i) is not None


def select_mesh_verification_cases(
    moments: List[Dict[str, Any]],
    db: Optional[WeaponSkinDatabase] = None,
    max_per_weapon: int = 2,
    max_total: int = 40,
) -> List[Dict[str, Any]]:
    """Pick up to two cases per weapon def: owned holder + picked-up (if demo has one)."""
    db = db or get_database()
    eligible = [m for m in moments if supports_mesh_test(m, db)]
    by_def: Dict[int, List[Dict[str, Any]]] = defaultdict(list)
    for m in eligible:
        by_def[int(m["weaponDefIndex"])].append(m)

    selected: List[Dict[str, Any]] = []
    seen_keys: Set[Tuple[int, str]] = set()

    for def_idx in sorted(by_def.keys()):
        rows = sorted(by_def[def_idx], key=score_moment, reverse=True)
        picks: List[Dict[str, Any]] = []

        owned = next((r for r in rows if ownership_type(r) == "owned"), None)
        pickup = next((r for r in rows if ownership_type(r) == "pickup"), None)

        if owned:
            picks.append(owned)
        if pickup and pickup is not owned:
            picks.append(pickup)
        if not picks and rows:
            picks.append(rows[0])

        for m in picks[:max_per_weapon]:
            otype = ownership_type(m)
            key = (def_idx, otype)
            if key in seen_keys:
                continue
            seen_keys.add(key)
            out = dict(m)
            out["ownershipType"] = otype
            out["caseKind"] = otype
            selected.append(out)
            if len(selected) >= max_total:
                return selected

    return selected
