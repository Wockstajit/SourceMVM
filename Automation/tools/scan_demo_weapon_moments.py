#!/usr/bin/env python3
"""Offline demo pre-scan: build probe ticks + fmjson anchors for weapon-moment discovery.

Phase 1 of the weapon-model verification pipeline. Reads <demo>.fmjson (or runs
FilmmakerDemoInfo.exe) and emits probe ticks + pickup/death anchors. Live paint/skin
data is filled in by scan-cosmetics-weapon-moments.ps1 at playback time.

Usage:
  python scan_demo_weapon_moments.py <demo.dem> [--out plan.json] [--helper path]
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Set

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
STEAM64_OFFSET = 76561197960265728
sys.path.insert(0, str(HERE))

from weapon_skin_database import get_database, is_verifiable_gun_def, slot_for_def  # noqa: E402


def find_demo_helper() -> Optional[Path]:
    candidates = [
        ROOT / "build" / "staging-release" / "bin" / "x64" / "FilmmakerDemoInfo" / "FilmmakerDemoInfo.exe",
        ROOT / "build" / "staging-release" / "bin" / "FilmmakerDemoInfo" / "FilmmakerDemoInfo.exe",
        ROOT / "FilmmakerDemoInfoGo" / "FilmmakerDemoInfo.exe",
    ]
    for p in candidates:
        if p.exists():
            return p
    return None


def load_fmjson(demo_path: Path, helper: Optional[Path]) -> Dict[str, Any]:
    cache = Path(str(demo_path) + ".fmjson")
    if cache.exists():
        return json.loads(cache.read_text(encoding="utf-8"))
    if helper and helper.exists():
        out = subprocess.check_output([str(helper), str(demo_path)], text=True, errors="replace")
        cache.write_text(out, encoding="utf-8")
        return json.loads(out)
    raise FileNotFoundError(f"No fmjson cache for {demo_path} and no FilmmakerDemoInfo helper")


def player_lookup(fm: Dict[str, Any]) -> Dict[int, Dict[str, Any]]:
    out: Dict[int, Dict[str, Any]] = {}
    for p in fm.get("players", []):
        aid = int(p.get("accountId", 0))
        if aid:
            out[aid] = p
    return out


def _apply_min_gap(ticks: List[int], min_gap: int) -> List[int]:
    out: List[int] = []
    for t in sorted(ticks):
        if out and (t - out[-1]) < min_gap:
            continue
        out.append(t)
    return out


def build_probe_ticks(fm: Dict[str, Any], max_ticks: int = 0) -> List[int]:
    """Interleave gun-pickup-anchored ticks with generic evenly-spaced ticks.

    fmjson only carries ground pickups -- round-start buys are filtered out
    upstream as freezetime noise -- so pickup-only probing would miss most of a
    match: the majority of weapon-holds are a player's own buy that's never
    dropped or picked up. Generic ticks give the live scanner a shot at
    whatever a player is *actually* holding, regardless of how they got it.
    """
    db = get_database()
    duration = int(fm.get("durationSeconds", 0) or 0)
    est_end = max(32000, duration * 64)
    min_gap = 1800  # ~28s between seeks unless action is far apart

    pickup_ticks: List[int] = []
    for ev in fm.get("events", []):
        if ev.get("type") != "item_pickup":
            continue
        item = (ev.get("item") or "").strip()
        def_idx = db.def_for_item(item)
        if not def_idx or not is_verifiable_gun_def(def_idx):
            continue
        tick = int(ev.get("tick", 0))
        if tick > 0:
            # Stagger a few offsets so inspect/draw/reload windows get a chance.
            for off in (16, 32, 48):
                pickup_ticks.append(tick + off)
    pickup_ticks = _apply_min_gap(pickup_ticks, min_gap)

    generic_step = 1536  # ~24s at 64 tick
    generic_ticks = _apply_min_gap(list(range(4000, max(4001, est_end - 4000), generic_step)), min_gap)

    if not pickup_ticks and not generic_ticks:
        return [4000, 8000, 12000, 16000]

    # Round-robin the two sources (not a plain merge-sort) so capping to
    # max_ticks below keeps both kinds of coverage instead of letting whichever
    # source is denser crowd the other out.
    interleaved: List[int] = []
    seen: List[int] = []
    i = j = 0
    while i < len(pickup_ticks) or j < len(generic_ticks):
        if i < len(pickup_ticks):
            t = pickup_ticks[i]
            i += 1
            if not seen or min(abs(t - s) for s in seen) >= min_gap:
                interleaved.append(t)
                seen.append(t)
        if j < len(generic_ticks):
            t = generic_ticks[j]
            j += 1
            if not seen or min(abs(t - s) for s in seen) >= min_gap:
                interleaved.append(t)
                seen.append(t)

    if max_ticks > 0 and len(interleaved) > max_ticks:
        interleaved = interleaved[:max_ticks]
    return sorted(interleaved)


def account_to_steam64(account_id: int) -> int:
    return int(account_id) + STEAM64_OFFSET


def is_alive_at_tick(account_id: int, tick: int, events: List[Dict[str, Any]]) -> bool:
    """Heuristic only: False when fmjson shows a weapon_drop (death) with no later
    tracked pickup before `tick`. fmjson has no round-boundary ticks, so a player
    who dies once and then respawns next round via the (untracked) freezetime
    auto-equip looks permanently dead here. Callers must NOT hard-skip on this --
    use it only to order/prioritize; the live spectate+diag check is authoritative.
    """
    if account_id <= 0 or tick <= 0:
        return False
    last_death = -1
    respawn_pickup = -1
    for ev in sorted(events, key=lambda e: int(e.get("tick", 0))):
        if int(ev.get("accountId", 0)) != account_id:
            continue
        t = int(ev.get("tick", 0))
        if t > tick:
            break
        et = ev.get("type", "")
        if et == "weapon_drop":
            last_death = t
            respawn_pickup = -1
        elif et == "item_pickup" and last_death >= 0 and t > last_death:
            respawn_pickup = t
    if last_death < 0:
        return True
    if respawn_pickup > last_death:
        return True
    return False


def annotate_pickup_hints(fm: Dict[str, Any], db) -> Dict[Tuple[int, int, str], str]:
    """Map (tick, accountId, weaponClass) -> ownershipHint from fmjson drop/pickup chains."""
    events = sorted(fm.get("events", []), key=lambda e: int(e.get("tick", 0)))
    recent_drops: List[Dict[str, Any]] = []
    hints: Dict[Tuple[int, int, str], str] = {}
    window = 512

    for ev in events:
        et = ev.get("type", "")
        tick = int(ev.get("tick", 0))
        aid = int(ev.get("accountId", 0))
        item = (ev.get("item") or "").strip()
        recent_drops = [d for d in recent_drops if tick - int(d["tick"]) <= window]

        if et == "weapon_drop" and aid > 0:
            recent_drops.append({"tick": tick, "accountId": aid, "item": item})
            continue

        if et != "item_pickup" or aid <= 0 or not item:
            continue
        def_idx = db.def_for_item(item)
        if not def_idx or not is_verifiable_gun_def(def_idx):
            continue

        hint = "owned"
        for d in recent_drops:
            if int(d["accountId"]) == aid:
                continue
            dropped = (d.get("item") or "").strip()
            if dropped and dropped != item:
                ddef = db.def_for_item(dropped)
                if ddef and ddef == def_idx:
                    hint = "pickup"
                    break
            elif not dropped:
                hint = "pickup"
                break

        hints[(tick, aid, item)] = hint
    return hints
def build_scan_targets(
    fm: Dict[str, Any],
    db,
    probe_ticks: List[int],
    max_per_tick: int = 8,
    pickup_hints: Optional[Dict[Tuple[int, int, str], str]] = None,
    max_blind_per_tick: int = 3,
) -> List[Dict[str, Any]]:
    """Known players per probe tick: fmjson gun pickups, plus blind (any-current-weapon)
    probes for known players when a tick has no pickup anchor nearby. Blind probes are
    what let round-start buys that are never dropped/picked up get scanned at all --
    the live scanner reads whatever the player is actually holding at that tick.
    """
    players = player_lookup(fm)
    known_players = [
        {
            "accountId": aid,
            "steamId": account_to_steam64(aid),
            "playerName": p.get("name", ""),
            "team": p.get("team"),
        }
        for aid, p in players.items()
    ]
    pickups: List[Dict[str, Any]] = []
    for ev in fm.get("events", []):
        if ev.get("type") != "item_pickup":
            continue
        tick = int(ev.get("tick", 0))
        aid = int(ev.get("accountId", 0))
        item = (ev.get("item") or "").strip()
        if tick <= 0 or aid <= 0 or not item:
            continue
        def_idx = db.def_for_item(item)
        if not def_idx or not is_verifiable_gun_def(def_idx):
            continue
        pinfo = players.get(aid, {})
        pickups.append({
            "pickupTick": tick,
            "accountId": aid,
            "steamId": account_to_steam64(aid),
            "playerName": pinfo.get("name", ""),
            "team": pinfo.get("team"),
            "weaponClass": item,
            "weaponDefIndex": def_idx,
        })

    events = fm.get("events", [])
    hints = pickup_hints or {}
    targets: List[Dict[str, Any]] = []
    seen: set = set()
    for idx, pt in enumerate(probe_ticks):
        cands = [p for p in pickups if (pt - 384) <= p["pickupTick"] <= (pt + 64)]
        # is_alive_at_tick is a heuristic (see its docstring) -- use it only to try
        # likely-alive candidates first, never to drop a candidate from the list.
        cands.sort(key=lambda p: (not is_alive_at_tick(int(p["accountId"]), pt, events), abs(p["pickupTick"] - pt)))
        added = 0
        for c in cands:
            key = (pt, c["accountId"])
            if key in seen:
                continue
            seen.add(key)
            alive = is_alive_at_tick(int(c["accountId"]), pt, events)
            hint_key = (int(c["pickupTick"]), int(c["accountId"]), c["weaponClass"])
            ownership_hint = hints.get(hint_key, "unknown")
            targets.append({
                "probeTick": pt,
                "seekTick": pt,
                "accountId": c["accountId"],
                "steamId": str(c["steamId"]),
                "playerName": c["playerName"],
                "team": c["team"],
                "weaponClass": c["weaponClass"],
                "expectedDefIndex": c["weaponDefIndex"],
                "aliveAtProbe": alive,
                "ownershipHint": ownership_hint,
                "source": "fmjson_gun_pickup",
            })
            added += 1
            if added >= max_per_tick:
                break

        # Fill remaining per-tick budget with blind probes, rotating the start
        # player per tick so different players get sampled across the demo
        # instead of always the first few in the roster. Blind probes have zero
        # prior info -- most live players are running around with a knife out,
        # not shooting -- so a blind hit rate is inherently low. Cap them tightly
        # per tick (each miss still costs a live spectate+diag round trip) and
        # let the many generic ticks provide breadth over the whole demo instead
        # of burning through the whole roster at one tick.
        blind_budget = min(max_per_tick, added + max_blind_per_tick)
        if added < blind_budget and known_players:
            n = len(known_players)
            start = idx % n
            for k in range(n):
                if added >= blind_budget:
                    break
                pl = known_players[(start + k) % n]
                key = (pt, pl["accountId"])
                if key in seen:
                    continue
                seen.add(key)
                targets.append({
                    "probeTick": pt,
                    "seekTick": pt,
                    "accountId": pl["accountId"],
                    "steamId": str(pl["steamId"]),
                    "playerName": pl["playerName"],
                    "team": pl["team"],
                    "weaponClass": None,
                    "expectedDefIndex": 0,
                    "aliveAtProbe": is_alive_at_tick(int(pl["accountId"]), pt, events),
                    "ownershipHint": "unknown",
                    "source": "blind_probe",
                })
                added += 1
    return targets


def build_anchors(fm: Dict[str, Any], db) -> List[Dict[str, Any]]:
    players = player_lookup(fm)
    anchors: List[Dict[str, Any]] = []
    for ev in fm.get("events", []):
        et = ev.get("type", "")
        tick = int(ev.get("tick", 0))
        aid = int(ev.get("accountId", 0))
        item = (ev.get("item") or "").strip()
        if tick <= 0 or aid <= 0:
            continue
        pinfo = players.get(aid, {})
        def_idx = db.def_for_item(item) if item else None
        anchors.append({
            "tick": tick,
            "eventType": et,
            "accountId": aid,
            "steamId": str(account_to_steam64(aid)),
            "playerName": pinfo.get("name", ""),
            "team": pinfo.get("team"),
            "weaponClass": item or None,
            "weaponDefIndex": def_idx,
            "weaponCategory": slot_for_def(def_idx) if def_idx else None,
            "source": "fmjson_event",
        })
    return anchors


def main() -> int:
    ap = argparse.ArgumentParser(description="Offline demo probe plan for weapon moment scanner")
    ap.add_argument("demo", type=Path, help="Path to .dem file")
    ap.add_argument("--out", type=Path, default=None, help="Output JSON plan path")
    ap.add_argument("--helper", type=Path, default=None, help="FilmmakerDemoInfo.exe")
    ap.add_argument("--max-probe-ticks", type=int, default=0,
                    help="Cap probe ticks (0 = all deduped gun pickups in demo)")
    args = ap.parse_args()

    demo = args.demo.resolve()
    if not demo.exists():
        print(f"Demo not found: {demo}", file=sys.stderr)
        return 1

    helper = args.helper or find_demo_helper()
    db = get_database()
    fm = load_fmjson(demo, helper)
    if not fm.get("ok"):
        print("fmjson parse failed", file=sys.stderr)
        return 1

    demo_name = demo.stem
    rel_demo = f"replays/{demo_name}"
    probe_ticks = build_probe_ticks(fm, args.max_probe_ticks)
    pickup_hints = annotate_pickup_hints(fm, db)
    plan = {
        "schemaVersion": 1,
        "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "demo": rel_demo,
        "demoPath": str(demo),
        "demoName": demo_name,
        "map": fm.get("map"),
        "durationSeconds": fm.get("durationSeconds"),
        "rounds": fm.get("rounds"),
        "players": fm.get("players", []),
        "probeTicks": probe_ticks,
        "scanTargets": build_scan_targets(fm, db, probe_ticks, max_per_tick=8, pickup_hints=pickup_hints),
        "anchors": build_anchors(fm, db),
        "moments": [],
        "note": "moments[] filled by scan-cosmetics-weapon-moments.ps1 live enumeration",
    }

    out = args.out
    if out is None:
        out = ROOT / "Automation" / "output" / "cosmetics_weapon_moments" / demo_name / "plan.json"
    out = out.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(plan, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print(f"Wrote {out}")
    print(f"  probeTicks={len(plan['probeTicks'])} scanTargets={len(plan['scanTargets'])} anchors={len(plan['anchors'])} map={plan['map']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
