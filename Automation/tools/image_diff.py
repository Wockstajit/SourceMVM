#!/usr/bin/env python3
"""Compare two screenshots and print simple JSON diff metrics."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image, ImageChops, ImageStat


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--min-mean", type=float, default=0.0)
    ap.add_argument("--crop", default=None,
                    help="left,top,right,bottom region to restrict the diff to "
                         "(e.g. a weapon-viewmodel box); diff is computed on the crop only.")
    args = ap.parse_args()

    before = Image.open(Path(args.before)).convert("RGB")
    after = Image.open(Path(args.after)).convert("RGB")
    if before.size != after.size:
        raise SystemExit(f"image sizes differ: {before.size} vs {after.size}")

    if args.crop:
        box = tuple(int(v) for v in args.crop.split(","))
        before = before.crop(box)
        after = after.crop(box)

    diff = ImageChops.difference(before, after)
    stat = ImageStat.Stat(diff)
    mean = sum(stat.mean) / len(stat.mean)
    extrema = diff.getextrema()
    max_channel = max(high for _low, high in extrema)
    bbox = diff.getbbox()
    changed_pixels = 0
    if bbox:
        # Count non-black diff pixels. Screenshots are small enough for verifier use.
        changed_pixels = sum(1 for px in diff.getdata() if px != (0, 0, 0))

    metrics = {
        "width": before.size[0],
        "height": before.size[1],
        "mean": mean,
        "max": max_channel,
        "bbox": bbox,
        "changedPixels": changed_pixels,
    }
    print(json.dumps(metrics, sort_keys=True))
    return 0 if mean >= args.min_mean else 2


if __name__ == "__main__":
    raise SystemExit(main())
