#!/usr/bin/env python3
"""Estimate pixel distance from muzzle-flash centroid to barrel-smoke centroid in FP screenshots.

Heuristic only (no engine particle positions): crops the lower-right weapon viewport,
finds the brightest cluster (muzzle flash) and a nearby desaturated gray cluster (smoke),
then reports offset in pixels and approximate screen fraction at 1600x1200.

Usage:
  python Automation/verify/measure-muzzle-smoke-offset.py <image.png> [<image2.png> ...]
  python Automation/verify/measure-muzzle-smoke-offset.py Automation/runs/fx-weapons-go/<run>/*.png
"""

from __future__ import annotations

import sys
from pathlib import Path

try:
    from PIL import Image
    import numpy as np
except ImportError:
    print("Requires Pillow and numpy: pip install pillow numpy", file=sys.stderr)
    sys.exit(2)


def analyze(path: Path) -> dict | None:
    im = Image.open(path).convert("RGB")
    w, h = im.size
    arr = np.asarray(im, dtype=np.float32)

    # FP weapon band: lower 55%, right 55% (muzzle lives here at default HUD scale).
    y0, y1 = int(h * 0.45), h
    x0, x1 = int(w * 0.35), w
    crop = arr[y0:y1, x0:x1]
    ch, cw = crop.shape[:2]

    r, g, b = crop[..., 0], crop[..., 1], crop[..., 2]
    maxc = np.maximum(np.maximum(r, g), b)
    sat = maxc - np.minimum(np.minimum(r, g), b)
    lum = 0.299 * r + 0.587 * g + 0.114 * b

    # Muzzle flash: very bright, warm (R dominant).
    flash_mask = (lum >= 200) & (r >= 180) & (sat >= 40)
    if flash_mask.sum() < 8:
        flash_mask = lum >= np.percentile(lum, 99.5)
    if flash_mask.sum() < 4:
        return None
    fy, fx = np.argwhere(flash_mask).mean(axis=0)

    # Smoke: gray, not as bright, within ~220px of flash in crop space.
    smoke_mask = (sat < 45) & (lum >= 35) & (lum <= 170)
    coords = np.argwhere(smoke_mask)
    if len(coords) == 0:
        return None
    dist = np.sqrt((coords[:, 0] - fy) ** 2 + (coords[:, 1] - fx) ** 2)
    near = coords[dist <= 220]
    if len(near) < 20:
        near = coords[dist <= 320]
    if len(near) < 10:
        return None
    sy, sx = near.mean(axis=0)

    # Map back to full image coords.
    flash_x = x0 + fx
    flash_y = y0 + fy
    smoke_x = x0 + sx
    smoke_y = y0 + sy
    dx = smoke_x - flash_x
    dy = smoke_y - flash_y
    dist_px = float(np.hypot(dx, dy))

    return {
        "file": str(path),
        "size": f"{w}x{h}",
        "flash_xy": (round(flash_x, 1), round(flash_y, 1)),
        "smoke_xy": (round(smoke_x, 1), round(smoke_y, 1)),
        "offset_px": (round(dx, 1), round(dy, 1)),
        "distance_px": round(dist_px, 1),
        "distance_pct_width": round(100.0 * dist_px / w, 2),
    }


def main() -> int:
    paths = [Path(p) for p in sys.argv[1:]]
    if not paths:
        print(__doc__)
        return 1
    rows = []
    for p in paths:
        if p.is_dir():
            paths.extend(sorted(p.glob("*.png")))
            continue
        if not p.is_file():
            continue
        r = analyze(p)
        if r:
            rows.append(r)
    if not rows:
        print("No measurable flash+smoke pairs found.")
        return 1
    print(f"{'file':50} {'dist_px':>8} {'offset(dx,dy)':>18} flash smoke")
    for r in rows:
        name = Path(r["file"]).name
        ox, oy = r["offset_px"]
        fx, fy = r["flash_xy"]
        sx, sy = r["smoke_xy"]
        print(
            f"{name:50} {r['distance_px']:8.1f} ({ox:+6.1f},{oy:+6.1f})"
            f"  flash=({fx:.0f},{fy:.0f}) smoke=({sx:.0f},{sy:.0f})"
        )
    dists = [r["distance_px"] for r in rows]
    print(f"\nSummary: n={len(dists)} mean={sum(dists)/len(dists):.1f}px "
          f"min={min(dists):.1f} max={max(dists):.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
