"""Packs CC0 Poly Haven PBR sand materials into the maps the Environment's sand coating reads.

The sand coating draws through the same path as CS2 snow (GameSnowAt in
AfxHookSource2/Filmmaker/Environment/EnvironmentClimateShaders.h). Each style ships three maps:

  <prefix>color.png   RGBA  colour (sRGB) and, in alpha, the cavities (ambient occlusion)
  <prefix>normal.png  RGB   the normal, hemi-octahedral in r and g (DirectX: +y runs down),
                            and the roughness in b
  <prefix>height.png  L     the height (0 low, 255 high)

Sources (all CC0 1.0 from Poly Haven, https://polyhaven.com):
  - sand_01        (Fine dry uneven sand)
  - coast_sand_02  (Coarse gritty sand with fine pebbles)
  - park_sand      (Organic weathered/trampled sand relief)

All three are color-harmonized in linear light to one cohesive warm sunlit desert-sand palette
while preserving each scan's real mineral grain contrast, then combined seamlessly (tileable
height-weighted + vector normal blend) into the default style (`sand_*.png`, "Dusty Mix" in the UI) alongside
the three individual styles (`sand_fine_*.png`, `sand_coarse_*.png`, `sand_rough_*.png`).

Usage:
    python FX/tools/build-sand-texture.py --source <folder containing sand_01, coast_sand_02, park_sand>
"""

import argparse
import os
import sys

import numpy as np
from PIL import Image

SIZE = 1024

# Target warm sunlit desert sand (linear RGB): bright pale sandstone-tan like CS2 Dust2 / Mirage /
# Anubis, avoiding muddy orange-brown in shade while keeping natural warm mineral colour.
TARGET_LINEAR_RGB = np.array([0.56, 0.45, 0.30], dtype=np.float64)


def find_map(folder, stem):
    for ext in (".png", ".jpg", ".jpeg"):
        p = os.path.join(folder, stem + ext)
        if os.path.isfile(p):
            return p
    sys.exit(f"missing {stem}.png/.jpg in {folder}")


def resize_arr(arr, size=SIZE):
    if arr.ndim == 2:
        return np.asarray(Image.fromarray(arr.astype(np.float32)).resize((size, size), Image.Resampling.LANCZOS), dtype=np.float64)
    return np.stack([
        np.asarray(Image.fromarray(arr[..., k].astype(np.float32)).resize((size, size), Image.Resampling.LANCZOS), dtype=np.float64)
        for k in range(arr.shape[-1])
    ], axis=-1)


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)


def hemi_oct(n):
    p = n[..., :2] / (np.abs(n[..., 0]) + np.abs(n[..., 1]) + n[..., 2])[..., None]
    r = (p[..., 0] + p[..., 1] + 1.0) * 0.5
    g = (p[..., 0] - p[..., 1] + 1.0) * 0.5
    return r, g


def decode_hemi_oct(r, g):
    tx, ty = r + g - 1.0, r - g
    n = np.stack([tx, ty, np.maximum(1.0 - np.abs(tx) - np.abs(ty), 0.05)], axis=-1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


def harmonize_linear_color(linear, target=TARGET_LINEAR_RGB, chroma_keep=0.72, contrast_boost=1.08):
    """Bring a scanned sand diffuse map into the shared desert-sand palette without flattening grain."""
    mean = np.maximum(linear.mean(axis=(0, 1)), 1e-4)
    # Match overall white balance / mean albedo to target
    balanced = linear * (target / mean)
    # Keep luminance micro-variation crisp while gently tightening extreme hue shifts between scans
    lum_w = np.array([0.2126, 0.7152, 0.0722])
    lum = np.sum(balanced * lum_w, axis=-1, keepdims=True)
    target_lum = float(np.sum(target * lum_w))
    neutral = (target / target_lum) * lum
    harmonized = neutral + (balanced - neutral) * chroma_keep
    # Subtle micro-contrast around mean so individual quartz grains read cleanly
    h_mean = harmonized.mean(axis=(0, 1), keepdims=True)
    harmonized = h_mean + (harmonized - h_mean) * contrast_boost
    return np.clip(harmonized, 0.02, 0.98)


def load_scan(folder, normal_strength=1.0, relief_scale=1.0):
    diff_img = Image.open(find_map(folder, "Diffuse")).convert("RGB")
    original = np.clip(resize_arr(srgb_to_linear(np.asarray(diff_img, dtype=np.float64) / 255.0)), 0.01, 0.99)
    harmonized = harmonize_linear_color(original)

    ao_img = Image.open(find_map(folder, "AO")).convert("L")
    ao = np.clip(resize_arr(np.asarray(ao_img, dtype=np.float64) / 255.0), 0.0, 1.0)

    rough_img = Image.open(find_map(folder, "Rough")).convert("L")
    rough = np.clip(resize_arr(np.asarray(rough_img, dtype=np.float64) / 255.0), 0.25, 0.98)

    disp_img = Image.open(find_map(folder, "Displacement"))
    disp = np.asarray(disp_img, dtype=np.float64)
    if disp.ndim == 3:
        disp = disp[..., 0]
    disp = (disp - disp.min()) / max(disp.max() - disp.min(), 1e-6)
    height = np.clip(resize_arr(disp), 0.0, 1.0)
    if relief_scale != 1.0:
        height = np.clip(0.5 + (height - 0.5) * relief_scale, 0.0, 1.0)

    nor_img = Image.open(find_map(folder, "nor_dx")).convert("RGB")
    nor = np.asarray(nor_img, dtype=np.float64) / 255.0 * 2.0 - 1.0
    nor /= np.maximum(np.linalg.norm(nor, axis=-1, keepdims=True), 1e-6)
    full = nor.shape[0]
    if full >= SIZE and full % SIZE == 0:
        step = full // SIZE
        nor = nor.reshape(SIZE, step, SIZE, step, 3).mean(axis=(1, 3))
    else:
        nor = resize_arr(nor)
    if normal_strength != 1.0:
        nor[..., :2] *= normal_strength
    nor[..., 2] = np.maximum(nor[..., 2], 0.08)
    nor /= np.maximum(np.linalg.norm(nor, axis=-1, keepdims=True), 1e-6)

    # Check DirectX normal convention against displacement gradient (+x right, +y down).
    gy, gx = np.gradient(height)
    mag = np.hypot(gx, gy)
    mask = mag > np.percentile(mag, 75)
    lean = nor[..., 0] * -gx + nor[..., 1] * -gy
    agree = float(np.mean(np.sign(lean[mask])))
    print(f"  loaded {os.path.basename(folder):14s}: normal/height agreement={agree:+.2f}, orig={original.mean(axis=(0,1)).round(3)} harm={harmonized.mean(axis=(0,1)).round(3)}")
    if agree < -0.2:
        print(f"  flipping normal Y for {os.path.basename(folder)} to match DirectX +y down")
        nor[..., 1] = -nor[..., 1]
    return {"color": original, "harmonized": harmonized, "ao": ao, "rough": rough, "height": height, "normal": nor}


def periodic_noise(size, freqs, seed):
    """Smooth tileable 0..1 noise constructed from integer-frequency harmonics."""
    rng = np.random.default_rng(seed)
    t = np.linspace(0.0, 2.0 * np.pi, size, endpoint=False)
    x, y = np.meshgrid(t, t)
    acc = np.zeros((size, size), dtype=np.float64)
    total_w = 0.0
    for fx, fy, amp in freqs:
        ph = rng.uniform(0.0, 2.0 * np.pi)
        acc += amp * np.sin(fx * x + fy * y + ph)
        total_w += amp
    acc = acc / max(total_w, 1e-6)
    return np.clip((acc - acc.min()) / max(acc.max() - acc.min(), 1e-6), 0.0, 1.0)


def blend_scans(fine, coarse, rough):
    """Height-aware tileable blend of sand_01 (fine), coast_sand_02 (coarse grit), and park_sand (organic relief)."""
    # Periodic macro patches so coarse pebbles, fine wind-swept sand, and organic depressions interweave naturally.
    m_coarse = periodic_noise(SIZE, [(1, 0, 1.0), (0, 1, 1.0), (1, 1, 0.7), (2, -1, 0.55), (-1, 2, 0.45), (3, 1, 0.3)], seed=101)
    m_rough = periodic_noise(SIZE, [(1, -1, 1.0), (0, 2, 0.8), (2, 1, 0.65), (-2, 1, 0.45), (1, 3, 0.3)], seed=202)

    # Height-modulated softmax weights: coarse pebbles sit naturally on slightly raised patches,
    # fine sand fills smooth areas, and park_sand weaves organic wind-pitted relief across ~22%.
    s_fine = 0.44 + 0.30 * (1.0 - m_coarse) + 0.28 * fine["height"]
    s_coarse = 0.40 + 0.38 * m_coarse + 0.36 * coarse["height"]
    s_rough = 0.34 + 0.36 * m_rough + 0.28 * rough["height"]

    sharpness = 5.5
    mx = np.maximum(np.maximum(s_fine, s_coarse), s_rough)
    e_fine = np.exp((s_fine - mx) * sharpness)
    e_coarse = np.exp((s_coarse - mx) * sharpness)
    e_rough = np.exp((s_rough - mx) * sharpness)
    denom = e_fine + e_coarse + e_rough
    wf = e_fine / denom
    wc = e_coarse / denom
    wr = e_rough / denom

    # Blended style harmonizes the three source palettes before combining so there are no colour seams.
    color = wf[..., None] * fine["harmonized"] + wc[..., None] * coarse["harmonized"] + wr[..., None] * rough["harmonized"]
    ao = wf * fine["ao"] + wc * coarse["ao"] + wr * rough["ao"]
    roughness = wf * fine["rough"] + wc * coarse["rough"] + wr * rough["rough"]
    height = wf * fine["height"] + wc * coarse["height"] + wr * rough["height"]
    height = (height - height.min()) / max(height.max() - height.min(), 1e-6)

    # Blend normals in slope space (nx/nz, ny/nz) so fine grit from coarse_sand stays sharp
    def slope(n):
        return n[..., :2] / np.maximum(n[..., 2:3], 0.15)

    # Plus a gentle cross-carry of micro-grain slope from coarse_sand so even fine patches feel authentically sandy
    s_blend = wf[..., None] * slope(fine["normal"]) + wc[..., None] * slope(coarse["normal"]) + wr[..., None] * slope(rough["normal"])
    # Add 18% high-pass grit normal from coast_sand_02 everywhere for crisp micro-granularity
    coarse_blur = resize_arr(resize_arr(slope(coarse["normal"]), SIZE // 16), SIZE)
    coarse_hi = slope(coarse["normal"]) - coarse_blur
    s_blend = s_blend + 0.22 * (1.0 - wc[..., None]) * coarse_hi

    normal = np.concatenate([s_blend, np.ones((SIZE, SIZE, 1), dtype=np.float64)], axis=-1)
    normal /= np.maximum(np.linalg.norm(normal, axis=-1, keepdims=True), 1e-6)
    print(f"  blended weights: fine={wf.mean():.2f}, coarse={wc.mean():.2f}, rough={wr.mean():.2f}")
    return {"color": color, "ao": ao, "rough": roughness, "height": height, "normal": normal}


def write_pack(out_dir, prefix, mat):
    srgb = linear_to_srgb(mat["color"])
    rgba = np.concatenate([srgb, np.clip(mat["ao"], 0.0, 1.0)[..., None]], axis=-1)
    Image.fromarray(np.round(np.clip(rgba, 0.0, 1.0) * 255.0).astype(np.uint8)).save(
        os.path.join(out_dir, f"{prefix}color.png"), optimize=True
    )
    Image.fromarray(np.round(np.clip(mat["height"], 0.0, 1.0) * 255.0).astype(np.uint8)).save(
        os.path.join(out_dir, f"{prefix}height.png"), optimize=True
    )
    r, g = hemi_oct(mat["normal"])
    back = decode_hemi_oct(r, g)
    worst = float(np.degrees(np.arccos(np.clip(np.sum(back * mat["normal"], axis=-1), -1.0, 1.0))).max())
    packed = np.stack([r, g, np.clip(mat["rough"], 0.0, 1.0)], axis=-1)
    Image.fromarray(np.round(np.clip(packed, 0.0, 1.0) * 255.0).astype(np.uint8)).save(
        os.path.join(out_dir, f"{prefix}normal.png"), optimize=True
    )
    mean = np.mean(mat["color"], axis=(0, 1))
    print(f"  wrote {prefix}{{color,normal,height}}.png (mean linear {mean[0]:.3f} {mean[1]:.3f} {mean[2]:.3f}, hemi-oct err {worst:.3f} deg)")


def render_preview(styles, preview_path):
    """Render a 2x2 contact sheet of lit 2x2-tiled patches for visual review."""
    from PIL import ImageDraw
    cell = 512
    sheet = Image.new("RGB", (cell * 2, cell * 2), (20, 20, 20))
    draw = ImageDraw.Draw(sheet)
    sun = np.array([0.55, -0.45, 0.70], dtype=np.float64)
    sun /= np.linalg.norm(sun)
    for idx, (label, mat) in enumerate(styles):
        # Simple directional + sky hemisphere shading in linear light
        n = mat["normal"]
        ndotl = np.clip(n[..., 0] * sun[0] + n[..., 1] * sun[1] + n[..., 2] * sun[2], 0.0, 1.0)
        sky = 0.32 + 0.16 * n[..., 2]
        lit = mat["color"] * (sky[..., None] + 0.95 * ndotl[..., None] * np.array([1.05, 0.98, 0.88])) * (0.78 + 0.22 * mat["ao"][..., None])
        img = Image.fromarray(np.round(linear_to_srgb(lit) * 255.0).astype(np.uint8))
        tile = img.resize((cell // 2, cell // 2), Image.Resampling.LANCZOS)
        r, c = divmod(idx, 2)
        for ty in range(2):
            for tx in range(2):
                sheet.paste(tile, (c * cell + tx * (cell // 2), r * cell + ty * (cell // 2)))
        draw.rectangle([c * cell, r * cell, c * cell + 280, r * cell + 28], fill=(0, 0, 0))
        draw.text((c * cell + 8, r * cell + 6), label, fill=(255, 255, 255))
    sheet.save(preview_path)
    print(f"wrote preview {preview_path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", required=True, help="folder holding sand_01/, coast_sand_02/, park_sand/")
    parser.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "..", "Assets", "environment", "sand"))
    parser.add_argument("--preview", default="", help="optional path for a 2x2 lit contact sheet")
    args = parser.parse_args()

    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)

    fine = load_scan(os.path.join(args.source, "sand_01"), normal_strength=0.85, relief_scale=0.85)
    coarse = load_scan(os.path.join(args.source, "coast_sand_02"), normal_strength=0.95, relief_scale=1.0)
    # Soften park_sand slightly so its organic relief blends smoothly across streets without harsh prints
    rough = load_scan(os.path.join(args.source, "park_sand"), normal_strength=0.72, relief_scale=0.75)
    blended = blend_scans(fine, coarse, rough)

    write_pack(out, "sand_", blended)
    write_pack(out, "sand_fine_", fine)
    write_pack(out, "sand_coarse_", coarse)
    write_pack(out, "sand_rough_", rough)

    if args.preview:
        render_preview([
            ("0: Blended (sand_01 + coast_02 + park)", blended),
            ("1: Fine (Poly Haven sand_01)", fine),
            ("2: Coarse (Poly Haven coast_sand_02)", coarse),
            ("3: Rough (Poly Haven park_sand)", rough),
        ], args.preview)


if __name__ == "__main__":
    main()
