#!/usr/bin/env python3
"""Stitch verification preview screenshots into a contact sheet PNG (and GIF if Pillow supports it)."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("Pillow required: pip install Pillow", file=sys.stderr)
    raise


def make_contact_sheet(images: list[Path], out: Path, cols: int = 4, thumb_w: int = 400) -> None:
    if not images:
        return
    thumbs = []
    for p in images:
        im = Image.open(p).convert("RGB")
        ratio = thumb_w / im.width
        thumbs.append(im.resize((thumb_w, int(im.height * ratio)), Image.Resampling.LANCZOS))
    rows = (len(thumbs) + cols - 1) // cols
    cell_h = max(t.height for t in thumbs)
    sheet = Image.new("RGB", (cols * thumb_w, rows * (cell_h + 24)), (24, 24, 28))
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("arial.ttf", 14)
    except OSError:
        font = ImageFont.load_default()
    for i, t in enumerate(thumbs):
        c = i % cols
        r = i // cols
        x = c * thumb_w
        y = r * (cell_h + 24)
        sheet.paste(t, (x, y))
        draw.text((x + 4, y + cell_h + 4), images[i].stem[:48], fill=(200, 200, 210), font=font)
    sheet.save(out)


def make_gif(images: list[Path], out: Path, duration_ms: int = 600) -> None:
    if len(images) < 2:
        return
    frames = [Image.open(p).convert("RGB").resize((800, 600), Image.Resampling.LANCZOS) for p in images]
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=duration_ms, loop=0)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("preview_dir", type=Path)
    ap.add_argument("--pattern", default="*_baseline.png")
    ap.add_argument("--out-sheet", type=Path, default=None)
    ap.add_argument("--out-gif", type=Path, default=None)
    args = ap.parse_args()
    imgs = sorted(args.preview_dir.glob(args.pattern))
    if not imgs:
        imgs = sorted(args.preview_dir.glob("*.png"))[:16]
    out_dir = args.preview_dir
    sheet = args.out_sheet or out_dir / "preview_contact.png"
    gif = args.out_gif or out_dir / "preview_moments.gif"
    make_contact_sheet(imgs[:16], sheet)
    make_gif(imgs[:12], gif)
    print(f"Wrote {sheet} ({len(imgs)} sources)")
    if gif.exists():
        print(f"Wrote {gif}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
