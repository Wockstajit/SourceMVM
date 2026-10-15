#!/usr/bin/env python3
"""Parse mvm_debug skin.live blocks from cosmetics enumeration logs."""
from __future__ import annotations

import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
from weapon_skin_database import (  # noqa: E402
    classify_paint,
    get_database,
    is_verifiable_gun_def,
    skip_reason_for_def,
    slot_for_def,
)

HEADER_RE = re.compile(
    r"skin\.live.*== reason=(\S+) steam=(\d+) name='([^']*)' pawn=(\d+) obs=(\d+) tick=(\d+)"
)
WEAPON_RE = re.compile(
    r"weapon slot=(\S+) idx=(\d+) cls='([^']*)' liveDef=(\d+) active=(\d+) "
    r"paint=(\d+) seed=(\d+) wear=([0-9.]+) stat=(\d+) attrSrc=(\S+) meshLegacy=(-?\d+)"
)
VM_RE = re.compile(
    r"viewmodel worldIdx=(\d+) vmIdx=(\d+) cls='([^']*)' vmPaint=(\d+) vmMesh=(\d+) vmLegacy=(-?\d+)"
)


@dataclass
class WeaponRow:
    slot: str
    entity_idx: int
    weapon_class: str
    def_index: int
    active: bool
    paint: int
    seed: int
    wear: float
    mesh_legacy: int


@dataclass
class SkinSnapshot:
    reason: str
    steam: str
    name: str
    pawn: int
    obs: int
    tick: int
    weapons: List[WeaponRow] = field(default_factory=list)
    active_def: int = -1
    active_paint: int = 0
    vm_paint: int = -1
    vm_legacy: int = -2


def parse_log_text(text: str) -> List[SkinSnapshot]:
    snaps: List[SkinSnapshot] = []
    cur: Optional[SkinSnapshot] = None
    for line in text.splitlines():
        hm = HEADER_RE.search(line)
        if hm:
            cur = SkinSnapshot(
                reason=hm.group(1),
                steam=hm.group(2),
                name=hm.group(3),
                pawn=int(hm.group(4)),
                obs=int(hm.group(5)),
                tick=int(hm.group(6)),
            )
            snaps.append(cur)
            continue
        if not cur:
            continue
        wm = WEAPON_RE.search(line)
        if wm:
            row = WeaponRow(
                slot=wm.group(1),
                entity_idx=int(wm.group(2)),
                weapon_class=wm.group(3),
                def_index=int(wm.group(4)),
                active=wm.group(5) == "1",
                paint=int(wm.group(6)),
                seed=int(wm.group(7)),
                wear=float(wm.group(8)),
                mesh_legacy=int(wm.group(11)),
            )
            cur.weapons.append(row)
            if row.active:
                cur.active_def = row.def_index
                cur.active_paint = row.paint
            continue
        vm = VM_RE.search(line)
        if vm:
            cur.vm_paint = int(vm.group(4))
            cur.vm_legacy = int(vm.group(6))
    return snaps


def snapshot_to_moments(snap: SkinSnapshot, demo_name: str, db, active_only: bool = True) -> List[Dict]:
    moments: List[Dict] = []
    reason_map = {
        "init": "idle",
        "player-switch": "player_switch",
        "seek": "demo_seek",
        "weapon-change": "weapon_switch",
        "loadout-change": "loadout_change",
        "manual": "idle",
    }
    anim = reason_map.get(snap.reason, snap.reason)
    for w in snap.weapons:
        # Only the weapon actually in the player's hands — not loadout/inventory entries.
        if active_only and not w.active:
            continue
        if w.slot in ("gloves", "agent", "none"):
            continue
        skip = skip_reason_for_def(w.def_index)
        if skip:
            continue
        if not is_verifiable_gun_def(w.def_index):
            continue
        model_type, leg, cs2, is_default = classify_paint(w.paint, db)
        if w.mesh_legacy == 1:
            model_type, leg, cs2 = "legacy", True, False
        elif w.mesh_legacy == 0:
            model_type, leg, cs2 = "cs2", False, True
        cat = slot_for_def(w.def_index)
        skin_name = db.skin_label(w.def_index, w.paint)
        fp_ok = w.active and cat in ("primary", "secondary")
        tp_ok = cat in ("primary", "secondary")
        moments.append({
            "demoName": demo_name,
            "tick": snap.tick,
            "playerName": snap.name,
            "playerSteamId": snap.steam,
            "playerEntityId": snap.pawn,
            "team": None,
            "observerMode": snap.obs,
            "weaponName": db.weapon_name_for_def(w.def_index),
            "weaponDefIndex": w.def_index,
            "weaponClass": w.weapon_class,
            "weaponCategory": cat,
            "skinName": skin_name,
            "paintIndex": w.paint,
            "expectLegacyModel": leg,
            "expectCs2Model": cs2,
            "isDefaultSkin": is_default,
            "expectedModelType": model_type,
            "animationState": anim if w.active else "held_in_loadout",
            "activeWeapon": w.active,
            "firstPersonUsable": fp_ok,
            "thirdPersonUsable": tp_ok,
            "hudUiUsable": fp_ok and not is_default,
            "playerSwitchUsable": snap.reason == "player-switch",
            "weaponSwitchUsable": snap.reason == "weapon-change",
            "skinlogReason": snap.reason,
            "reason": _moment_reason(snap, w, anim),
        })
    return moments


def _moment_reason(snap: SkinSnapshot, w: WeaponRow, anim: str) -> str:
    bits = [f"tick {snap.tick}", f"player {snap.name or snap.steam}"]
    if w.active:
        bits.append("active weapon visible")
    bits.append(anim.replace("_", " "))
    if w.paint > 0:
        bits.append(f"paint {w.paint}")
    else:
        bits.append("default skin")
    return "; ".join(bits)


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: parse_skinlog.py <mvm_debug.log> [--demo-name NAME] [--json]", file=sys.stderr)
        return 1
    path = Path(sys.argv[1])
    demo_name = path.stem
    for i, a in enumerate(sys.argv):
        if a == "--demo-name" and i + 1 < len(sys.argv):
            demo_name = sys.argv[i + 1]
    text = path.read_text(encoding="utf-8", errors="replace")
    db = get_database()
    snaps = parse_log_text(text)
    all_m = []
    active_only = "--all-weapons" not in sys.argv
    for s in snaps:
        all_m.extend(snapshot_to_moments(s, demo_name, db, active_only=active_only))
    print(json.dumps({"snapshots": len(snaps), "moments": all_m}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
