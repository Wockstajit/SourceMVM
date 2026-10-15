#!/usr/bin/env python3
"""Per-weapon Modern FX barrel alignment audit from FP screenshots.

Uses the muzzle-flash bright centroid as the barrel-tip proxy, then measures pixel
offset/distance to nearby smoke puff and rope-wisp centroids. Intended for screenshots
from verify-fx-weapons-go (modern-{class}-{effect}.png).

Writes alignment-report.json + alignment-summary.txt under the run dir and appends
one NDJSON line per weapon to debug-7803fe.log (session 7803fe).

Usage:
  python Automation/verify/audit-modern-muzzle-alignment.py <run-dir>
  python Automation/verify/audit-modern-muzzle-alignment.py Automation/runs/fx-weapons-go/<timestamp>
"""

from __future__ import annotations

import json
import re
import sys
import time
from pathlib import Path

try:
    from PIL import Image
    import numpy as np
except ImportError:
    print("Requires Pillow and numpy: pip install pillow numpy", file=sys.stderr)
    sys.exit(2)

REPO = Path(__file__).resolve().parents[2]
DEBUG_LOG = REPO / "debug-7803fe.log"
SESSION = "7803fe"

# At 1600x1200 FP, smoke/wisp centroid within this many px of flash reads as barrel-aligned.
PASS_DISTANCE_PX = 55.0

FNAME_RE = re.compile(
    r"^modern-(?P<weapon>[a-z0-9_]+)-(?P<effect>muzzleflash|wisp|barrelsmoke|smoke)\.png$",
    re.I,
)


def _crop_weapon_band(arr: np.ndarray):
    h, w = arr.shape[:2]
    y0, y1 = int(h * 0.45), h
    x0, x1 = int(w * 0.35), int(w * 0.88)  # exclude FxDebugHud strip on far right
    return arr[y0:y1, x0:x1], x0, y0


def _flash_centroid(crop: np.ndarray) -> tuple[float, float] | None:
    r, g, b = crop[..., 0], crop[..., 1], crop[..., 2]
    maxc = np.maximum(np.maximum(r, g), b)
    sat = maxc - np.minimum(np.minimum(r, g), b)
    lum = 0.299 * r + 0.587 * g + 0.114 * b
    ch, cw = crop.shape[:2]
    # Muzzle tip lives in the forward (right) half of the FP weapon band.
    forward = slice(int(cw * 0.38), cw)
    warm_flash = (
        (lum[:, forward] >= 185)
        & (r[:, forward] >= 150)
        & (g[:, forward] >= 80)
        & (r[:, forward] >= b[:, forward] + 15)
        & (sat[:, forward] >= 20)
    )
    white_flash = (lum[:, forward] >= 215) & (maxc[:, forward] >= 210) & (sat[:, forward] >= 12)
    flash_mask = warm_flash | white_flash
    if flash_mask.sum() < 4:
        return None
    coords = np.argwhere(flash_mask)
    sub_lum = lum[:, forward][flash_mask]
    weights = sub_lum / max(sub_lum.max(), 1.0)
    fx = coords[:, 1] + int(cw * 0.38)
    fy = coords[:, 0]
    wx = fx * weights
    wy = fy * weights
    return float(wx.sum() / weights.sum()), float(wy.sum() / weights.sum())


def _barrel_tip_proxy(crop: np.ndarray) -> tuple[float, float] | None:
    """Fallback barrel tip when the muzzle flash frame has already faded (paused capture)."""
    r, g, b = crop[..., 0], crop[..., 1], crop[..., 2]
    lum = 0.299 * r + 0.587 * g + 0.114 * b
    h, w = crop.shape[:2]
    weapon = (lum > 18) & (lum < 232)
    weapon[: int(h * 0.30), :] = False
    weapon[:, : int(w * 0.28)] = False
    coords = np.argwhere(weapon)
    if len(coords) < 80:
        return None
    cx = coords[:, 1]
    right_x = np.percentile(cx, 98.5)
    tip_pts = coords[cx >= right_x - 2.0]
    if len(tip_pts) < 5:
        return None
    return float(tip_pts[:, 1].mean()), float(tip_pts[:, 0].mean())


def _anchor_centroid(crop: np.ndarray) -> tuple[tuple[float, float] | None, str]:
    flash = _flash_centroid(crop)
    if flash is not None:
        return flash, "flash"
    tip = _barrel_tip_proxy(crop)
    if tip is not None:
        return tip, "barrel_tip"
    return None, "none"


def _cluster_near(
    crop: np.ndarray,
    anchor_x: float,
    anchor_y: float,
    *,
    max_radius: float,
    mask: np.ndarray,
    min_pixels: int,
) -> tuple[float, float] | None:
    coords = np.argwhere(mask)
    if len(coords) < min_pixels:
        return None
    dist = np.sqrt((coords[:, 0] - anchor_y) ** 2 + (coords[:, 1] - anchor_x) ** 2)
    near = coords[dist <= max_radius]
    if len(near) < min_pixels:
        return None
    cy, cx = near.mean(axis=0)
    return float(cx), float(cy)


def _not_debug_red(r, g, b):
    return ~((r >= 140) & (g <= 90) & (b <= 90))


def _smoke_centroid(crop: np.ndarray, ax: float, ay: float) -> tuple[float, float] | None:
    r, g, b = crop[..., 0], crop[..., 1], crop[..., 2]
    maxc = np.maximum(np.maximum(r, g), b)
    sat = maxc - np.minimum(np.minimum(r, g), b)
    lum = 0.299 * r + 0.587 * g + 0.114 * b
    smoke_mask = _not_debug_red(r, g, b) & (
        ((sat < 55) & (lum >= 30) & (lum <= 175))
        | ((lum >= 175) & (sat < 45))
    )
    return _cluster_near(crop, ax, ay, max_radius=140, mask=smoke_mask, min_pixels=12)


def _wisp_centroid(crop: np.ndarray, ax: float, ay: float) -> tuple[float, float] | None:
    r, g, b = crop[..., 0], crop[..., 1], crop[..., 2]
    maxc = np.maximum(np.maximum(r, g), b)
    sat = maxc - np.minimum(np.minimum(r, g), b)
    lum = 0.299 * r + 0.587 * g + 0.114 * b
    # Modern rope wisp: desaturated gray with slight cyan bias (never mvm_debug red).
    wisp_mask = _not_debug_red(r, g, b) & (
        (sat < 55) & (lum >= 22) & (lum <= 150) & (b >= r * 0.85) & (g >= r * 0.85)
    )
    return _cluster_near(crop, ax, ay, max_radius=180, mask=wisp_mask, min_pixels=20)


def _offset(full_x0: int, full_y0: int, ax: float, ay: float, bx: float, by: float) -> dict:
    fx = full_x0 + ax
    fy = full_y0 + ay
    tx = full_x0 + bx
    ty = full_y0 + by
    dx = tx - fx
    dy = ty - fy
    dist = float(np.hypot(dx, dy))
    return {
        "from_xy": [round(fx, 1), round(fy, 1)],
        "to_xy": [round(tx, 1), round(ty, 1)],
        "offset_px": [round(dx, 1), round(dy, 1)],
        "distance_px": round(dist, 1),
        "pass": dist <= PASS_DISTANCE_PX,
    }


def analyze_image(path: Path, shared_anchor: dict | None = None) -> dict | None:
    m = FNAME_RE.match(path.name)
    if not m:
        return None
    weapon = m.group("weapon").lower()
    effect = m.group("effect").lower()
    if effect == "smoke":
        effect = "barrelsmoke"

    im = Image.open(path).convert("RGB")
    arr = np.asarray(im, dtype=np.float32)
    crop, x0, y0 = _crop_weapon_band(arr)
    if shared_anchor is not None:
        ax, ay = shared_anchor["ax"], shared_anchor["ay"]
        anchor_kind = shared_anchor.get("kind", "muzzleflash_ref")
    else:
        anchor, anchor_kind = _anchor_centroid(crop)
        if anchor is None:
            return {
                "weapon": weapon,
                "effect": effect,
                "file": path.name,
                "status": "NO_ANCHOR",
                "pass": False,
            }
        ax, ay = anchor
    out: dict = {
        "weapon": weapon,
        "effect": effect,
        "file": path.name,
        "status": "OK",
        "anchor": anchor_kind,
        "anchor_xy": [round(x0 + ax, 1), round(y0 + ay, 1)],
        "anchor_crop": [round(ax, 1), round(ay, 1)],
        "measurements": {},
    }

    smoke = _smoke_centroid(crop, ax, ay)
    if smoke:
        out["measurements"]["smoke_from_flash"] = _offset(x0, y0, ax, ay, *smoke)

    wisp = _wisp_centroid(crop, ax, ay)
    if wisp:
        out["measurements"]["wisp_from_flash"] = _offset(x0, y0, ax, ay, *wisp)

    if effect == "wisp" and "wisp_from_flash" in out["measurements"]:
        out["pass"] = out["measurements"]["wisp_from_flash"]["pass"]
    elif effect in ("muzzleflash", "barrelsmoke") and "smoke_from_flash" in out["measurements"]:
        out["pass"] = out["measurements"]["smoke_from_flash"]["pass"]
    elif effect == "muzzleflash" and "wisp_from_flash" in out["measurements"]:
        out["pass"] = out["measurements"]["wisp_from_flash"]["pass"]
    else:
        out["pass"] = False
        out["status"] = "NO_TARGET_CLUSTER"

    return out


def dbg_log(weapon: str, data: dict) -> None:
    entry = {
        "sessionId": SESSION,
        "hypothesisId": "H-align-audit",
        "location": "audit-modern-muzzle-alignment.py",
        "message": f"modern {weapon} alignment",
        "data": data,
        "timestamp": int(time.time() * 1000),
        "runId": "align-audit",
    }
    with DEBUG_LOG.open("a", encoding="utf-8") as f:
        f.write(json.dumps(entry) + "\n")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    run_dir = Path(sys.argv[1])
    if not run_dir.is_dir():
        print(f"Not a directory: {run_dir}", file=sys.stderr)
        return 1

    pngs = sorted(run_dir.glob("modern-*.png"))
    muzzle_anchors: dict[str, dict] = {}
    for png in pngs:
        m = FNAME_RE.match(png.name)
        if not m or m.group("effect").lower() != "muzzleflash":
            continue
        row = analyze_image(png)
        if row and row.get("anchor") == "flash" and "anchor_crop" in row:
            ax, ay = row["anchor_crop"]
            muzzle_anchors[row["weapon"]] = {"ax": ax, "ay": ay, "kind": "muzzleflash_ref"}

    rows: list[dict] = []
    for png in pngs:
        m = FNAME_RE.match(png.name)
        shared = None
        if m and m.group("effect").lower() != "muzzleflash":
            shared = muzzle_anchors.get(m.group("weapon").lower())
        row = analyze_image(png, shared)
        if row:
            rows.append(row)
            dbg_log(row["weapon"], row)

    by_weapon: dict[str, dict] = {}
    for row in rows:
        w = row["weapon"]
        by_weapon.setdefault(w, {})[row["effect"]] = row

    weapons = sorted(by_weapon.keys())
    summary_lines = [
        "Modern per-weapon barrel alignment audit",
        f"run: {run_dir}",
        f"pass threshold: {PASS_DISTANCE_PX}px from muzzle-flash proxy",
        "",
        f"{'weapon':18} {'effect':12} {'dist':>7} {'offset(dx,dy)':>18} {'status':>14} pass",
    ]
    any_fail = False
    for w in weapons:
        for effect in ("muzzleflash", "barrelsmoke", "wisp"):
            row = by_weapon[w].get(effect)
            if not row:
                summary_lines.append(f"{w:18} {effect:12} {'—':>7} {'—':>18} {'MISSING':>14} NO")
                any_fail = True
                continue
            meas = row.get("measurements", {})
            key = "wisp_from_flash" if effect == "wisp" else "smoke_from_flash"
            if key not in meas and effect == "wisp":
                key = "wisp_from_flash"
            if key not in meas:
                summary_lines.append(
                    f"{w:18} {effect:12} {'—':>7} {'—':>18} {row.get('status', '?'):>14} FAIL"
                )
                any_fail = True
                continue
            m = meas[key]
            ox, oy = m["offset_px"]
            ok = m["pass"]
            if not ok:
                any_fail = True
            summary_lines.append(
                f"{w:18} {effect:12} {m['distance_px']:7.1f} ({ox:+6.1f},{oy:+6.1f})"
                f" {row.get('status', ''):>14} {'OK' if ok else 'FAIL'}"
            )

    report = {
        "run_dir": str(run_dir),
        "pass_distance_px": PASS_DISTANCE_PX,
        "weapons": by_weapon,
        "rows": rows,
        "any_fail": any_fail,
    }
    report_path = run_dir / "alignment-report.json"
    summary_path = run_dir / "alignment-summary.txt"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    summary_path.write_text("\n".join(summary_lines) + "\n", encoding="utf-8")

    print("\n".join(summary_lines))
    print(f"\nWrote {report_path.name} and {summary_path.name}")
    return 1 if any_fail else 0


if __name__ == "__main__":
    sys.exit(main())
