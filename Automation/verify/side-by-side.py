#!/usr/bin/env python3
"""Join lighting captures into one labelled contact sheet.

    python Automation/verify/side-by-side.py out.png a.png b.png [c.png ...] [--width 800]

Images are scaled to --width each and laid out left to right, with the file stem as caption.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("images", nargs="+")
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--crop", default=None, help="left,top,right,bottom crop applied to each input first")
    args = ap.parse_args()

    tiles = []
    for name in args.images:
        image = Image.open(name).convert("RGB")
        if args.crop:
            image = image.crop(tuple(int(v) for v in args.crop.split(",")))
        scale = args.width / image.width
        tiles.append((Path(name).stem, image.resize((args.width, max(1, round(image.height * scale))), Image.LANCZOS)))

    caption = 22
    height = max(t.height for _, t in tiles) + caption
    sheet = Image.new("RGB", (args.width * len(tiles), height), (16, 16, 16))
    draw = ImageDraw.Draw(sheet)
    for i, (stem, tile) in enumerate(tiles):
        sheet.paste(tile, (i * args.width, caption))
        draw.text((i * args.width + 6, 4), stem, fill=(255, 255, 255))
    sheet.save(args.out)
    print(args.out, sheet.size)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
