#!/usr/bin/env python3
"""Measure TEMPORAL alignment between the streams of one render take.

The multi-stream contract is that every selected stream is captured from the SAME source frame
before the demo advances a tick, so `beauty/frame_000007.png` and `players/frame_000007.png` must
be the same instant. If an extra pass is captured one frame late (or early), every plate is
subtly out of sync with beauty and nothing composites.

Comparing a black isolation plate to a lit beauty plate pixel-for-pixel is meaningless, so this
compares MOTION instead: for each stream, the per-frame mean absolute difference from its own
previous frame is a 1-D motion-energy signal. Camera motion drives every stream identically, so
the signals should peak on the same frames. Cross-correlating each stream's signal against
beauty's reveals the lag in frames.

    python check-pass-alignment.py <take-dir>

lag 0 for every stream = aligned. A consistent non-zero lag is a capture-ordering bug.
"""
from __future__ import annotations

import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    print("needs Pillow:  py -m pip install pillow", file=sys.stderr)
    raise SystemExit(2)

THUMB = (160, 90)  # motion energy does not need resolution, and this keeps it fast


def motion_signal(folder: Path) -> list[float]:
    frames = sorted(p for p in folder.iterdir() if p.suffix.lower() in {".png", ".tga"})
    prev = None
    out: list[float] = []
    for f in frames:
        with Image.open(f) as im:
            g = im.convert("L").resize(THUMB)
            px = list(g.getdata())
        if prev is not None:
            out.append(sum(abs(a - b) for a, b in zip(px, prev)) / float(len(px)))
        prev = px
    return out


def normalise(xs: list[float]) -> list[float]:
    if not xs:
        return xs
    m = sum(xs) / len(xs)
    dev = [x - m for x in xs]
    norm = sum(d * d for d in dev) ** 0.5
    return [d / norm for d in dev] if norm > 1e-9 else dev


def best_lag(a: list[float], b: list[float], span: int = 3) -> tuple[int, float]:
    """Lag L maximising correlation of b shifted by L against a."""
    na, nb = normalise(a), normalise(b)
    best, score = 0, -2.0
    for lag in range(-span, span + 1):
        pairs = [
            (na[i], nb[i + lag])
            for i in range(len(na))
            if 0 <= i + lag < len(nb)
        ]
        if len(pairs) < 3:
            continue
        s = sum(x * y for x, y in pairs)
        if s > score:
            best, score = lag, s
    return best, score


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    take = Path(sys.argv[1])
    beauty = take / "beauty"
    if not beauty.is_dir():
        print(f"no beauty/ under {take}", file=sys.stderr)
        return 2

    ref = motion_signal(beauty)
    if len(ref) < 4:
        print("need at least 5 frames to measure alignment", file=sys.stderr)
        return 2

    print(f"{'stream':40s} {'frames':>7s} {'lag':>5s} {'corr':>7s}")
    bad = 0
    n_beauty = len(list(beauty.iterdir()))
    print(f"{'beauty (reference)':40s} {n_beauty:7d} {0:5d} {1.0:7.3f}")
    for d in sorted(p for p in take.iterdir() if p.is_dir() and p.name != "beauty"):
        sig = motion_signal(d)
        n = len(list(d.iterdir()))
        if len(sig) < 4:
            print(f"{d.name:40s} {n:7d}     ?        ? (too few frames)")
            continue
        lag, corr = best_lag(ref, sig)
        flag = "" if lag == 0 else "  <-- MISALIGNED"
        if lag != 0:
            bad += 1
        if n != n_beauty:
            flag += f"  <-- {n} frames vs beauty {n_beauty}"
            bad += 1
        print(f"{d.name:40s} {n:7d} {lag:5d} {corr:7.3f}{flag}")

    if bad:
        print("\nFAIL: streams are not captured from the same source frame.")
        return 1
    print("\nPASS: every stream aligns with beauty frame-for-frame.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
