#!/usr/bin/env python3
"""Cross-pass leak detector for tinted render takes.

Run a render job with `mirv_filmmaker render difftint on`. Every classified draw in every pass --
beauty included -- is then painted a flat colour by subject domain:

    world  = blue      players = red      equipment = purple
    props  = green     weapons = white

That turns the acceptance rule into arithmetic. A pixel that is RED in an isolation pass but NOT
red in beauty means a player was drawn where beauty shows something else -- i.e. through a wall,
or through smoke. No perceptual judgement, no thresholds tuned to a particular shot.

    python check-tint-leak.py <take-dir> [--subject red] [--tolerance 0.002]

Exit code 0 = every pass matched beauty's subject coverage; 1 = at least one pass leaked.

Standalone by design (CLAUDE.md): it reads a finished take off disk and knows nothing about the
netcon driver or the other verify scripts.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    print("needs Pillow:  py -m pip install pillow", file=sys.stderr)
    raise SystemExit(2)

# Domain colours as rendered by the dev materials. Matching is done by dominance, not equality,
# because the flat materials still take a little shading and MSAA resolve.
SUBJECTS = {
    "red": lambda r, g, b: r > 90 and r > g * 2 and r > b * 2,
    "blue": lambda r, g, b: b > 90 and b > r * 2 and b > g * 2,
    "green": lambda r, g, b: g > 90 and g > r * 2 and g > b * 2,
    "purple": lambda r, g, b: r > 70 and b > 70 and g * 2 < r and g * 2 < b,
}


def mask(path: Path, test) -> tuple[set[int], int]:
    """Indices of pixels satisfying `test`, plus total pixel count."""
    with Image.open(path) as im:
        im = im.convert("RGB")
        px = im.load()
        w, h = im.size
        hits = set()
        for y in range(h):
            for x in range(w):
                r, g, b = px[x, y]
                if test(r, g, b):
                    hits.add(y * w + x)
        return hits, w * h


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("take", type=Path)
    ap.add_argument("--subject", default="red", choices=sorted(SUBJECTS),
                    help="domain colour to police (default red = players)")
    ap.add_argument("--tolerance", type=float, default=0.002,
                    help="fraction of frame allowed to leak (resolve/AA slack)")
    ap.add_argument("--frame", default=None, help="single frame filename to check")
    args = ap.parse_args()

    beauty_dir = args.take / "beauty"
    if not beauty_dir.is_dir():
        print(f"no beauty/ in {args.take}", file=sys.stderr)
        return 2
    test = SUBJECTS[args.subject]

    frames = sorted(p.name for p in beauty_dir.iterdir() if p.suffix.lower() in {".png", ".tga"})
    if args.frame:
        frames = [args.frame]
    if not frames:
        print("no frames", file=sys.stderr)
        return 2

    worst: dict[str, tuple[float, str]] = {}
    comparisons = 0
    for name in frames:
        beauty_hits, total = mask(beauty_dir / name, test)
        for d in sorted(p for p in args.take.iterdir() if p.is_dir() and p.name != "beauty"):
            f = d / name
            if not f.exists():
                continue
            comparisons += 1
            hits, _ = mask(f, test)
            # Pixels where this pass paints the subject but beauty does not = leak.
            leak = len(hits - beauty_hits) / float(total)
            # Record exact-zero comparisons too. Previously an entirely clean take printed an
            # empty table and PASS, which was indistinguishable from measuring no pass files.
            prev = worst.setdefault(d.name, (0.0, name))
            if leak > prev[0]:
                worst[d.name] = (leak, name)

    if comparisons == 0:
        print("no matching beauty/pass frames were compared", file=sys.stderr)
        return 2

    bad = 0
    print(f"{'pass':44s} {'leak%':>8s}  worst frame")
    for name in sorted(worst):
        frac, frame = worst[name]
        flag = "LEAK" if frac > args.tolerance else "ok"
        if frac > args.tolerance:
            bad += 1
        print(f"{name:44s} {frac*100:7.3f}%  {frame}  {flag}")

    if bad:
        print(f"\nFAIL: {bad} pass(es) draw '{args.subject}' subjects beauty does not show.")
        return 1
    print(f"\nPASS: no pass shows a '{args.subject}' subject that beauty hides.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
