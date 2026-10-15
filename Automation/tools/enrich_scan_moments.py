#!/usr/bin/env python3
"""Enrich raw scan moments (def/paint only) with skin DB metadata in one pass."""
from __future__ import annotations

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from weapon_skin_database import classify_paint, get_database, slot_for_def  # noqa: E402


def enrich(moment: dict, db, demo_name: str) -> dict:
    def_i = int(moment.get("weaponDefIndex", 0))
    paint = int(moment.get("paintIndex", 0))
    model, leg, cs2, default = classify_paint(paint, db)
    out = dict(moment)
    out.setdefault("demoName", demo_name)
    holder = str(out.get("playerSteamId") or "")
    owner = str(out.get("weaponOwnerSteamId") or out.get("ownerSteamId") or out.get("ownerXuid") or "")
    if owner and holder and owner not in ("0", "") and holder not in ("0", "") and owner != holder:
        out["ownershipType"] = "pickup"
    elif out.get("ownershipHint") in ("pickup", "owned"):
        out["ownershipType"] = out["ownershipHint"]
    else:
        out["ownershipType"] = out.get("ownershipType", "owned")
    out["holderSteamId"] = holder
    if owner:
        out["weaponOwnerSteamId"] = owner
    out["weaponName"] = db.weapon_name_for_def(def_i)
    out["weaponCategory"] = slot_for_def(def_i)
    out["skinName"] = db.skin_label(def_i, paint)
    out["expectLegacyModel"] = leg
    out["expectCs2Model"] = cs2
    out["isDefaultSkin"] = default
    out["expectedModelType"] = model
    out.setdefault("activeWeapon", True)
    out.setdefault("firstPersonUsable", True)
    out.setdefault("thirdPersonUsable", True)
    out.setdefault("hudUiUsable", not default)
    return out


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: enrich_scan_moments.py <scan_raw.json>", file=sys.stderr)
        return 1
    path = Path(sys.argv[1])
    doc = json.loads(path.read_text(encoding="utf-8-sig"))
    db = get_database()
    demo_name = doc.get("demoName", "demo")
    doc["moments"] = [enrich(m, db, demo_name) for m in doc.get("moments", [])]
    path.write_text(json.dumps(doc, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print(f"Enriched {len(doc['moments'])} moments in {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
