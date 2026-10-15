#!/usr/bin/env python3
"""Give additive particle materials a translucency map that actually has a SHAPE.

THE FAULT
    A Source 1 additive sprite is shaped by its RGB: the glow sits in the middle and the texture
    fades to black at the edges, so adding it to the frame lights only the middle. Plenty of these
    textures therefore ship an alpha channel that is FULLY OPAQUE -- alpha was never what shaped
    them, so nobody authored it.

    Converting one of those to `csgo_complex.vfx` gives it `F_TRANSLUCENT` plus a translucency map,
    and both routes produce a map with no shape in it:

      * source1import writes a literal `TextureTranslucency "[1 1 1 1]"` -- opaque everywhere.
      * convert-spritecard-materials.py extracts the alpha channel, which is opaque everywhere.

    CS2 then composites the WHOLE QUAD instead of the glow, and the sprite draws as a hard-edged
    rectangle of its own colour. Seen in game 2026-09-02 on the Povarehok HE grenade:

      * `fas_glow_debris` (alpha 255 everywhere) at radius 870 -> a bright rectangle across the
        room at detonation, with a straight vertical edge cutting over the far wall.
      * `particle_glow_07_15ob` (alpha 255 everywhere), used by the four shrapnel/spark systems
        whose colour inits are orange-red -> small opaque red-orange QUADS instead of embers.
        That is the "red debris" in the screenshots; the concrete debris model was innocent.

    Materials whose alpha DOES have falloff (`bf3test`, `Particle_Glow_05` -- edge alpha 1 and 0)
    render correctly and are left alone. That is the test this tool applies.

THE REPAIR
    Write `<base>_a_lum.tga`, an 8-bit greyscale map of the colour texture's LUMINANCE, and point
    `TextureTranslucency` at it. For an additive glow luminance IS the coverage: the bright core
    contributes, the black surround does not, which is exactly what the RGB was already doing.
    Same 8-bit greyscale TGA shape source1import emits for its own `_a_trans` companions.

    Deliberately NOT touched: materials without `F_ADDITIVE_BLEND` (an alpha-blended sprite's
    opaque alpha may be intentional -- a decal, a debris chunk), and anything whose translucency
    map already has falloff.

USAGE
    python FX/tools/repair-additive-alpha.py --content-root <content/source_mvm_fx> [--dry-run]
Marker-guarded, so it is safe to re-run.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# COVERAGE_GAMMA -- why the map is not raw luminance.
#
# An additive sprite was ALREADY shaped by its RGB in Source 1: the framebuffer got `+= rgb`,
# and the black surround added nothing. Giving it alpha = luminance makes CS2 draw
# `+= rgb * luminance`, which applies that same falloff a SECOND time and squares it.
#
# Measured on ins_muzzleflash (the On-mode muzzle flash base, 512x512):
#     peak alpha   1.00   -- the core is unaffected, so it does not look obviously broken
#     median lit   0.13
#     mean lit     0.25   -- the flash body loses ~75% of its contribution
#
# That is the "muzzle flash looks dim" report, and it also breaks the ON-mode rule that the
# Insurgency base stays at its authored strength (only the layers on top are dimmed).
#
# Raising the map to a gamma keeps what the repair was FOR -- zero stays zero, so the opaque
# alpha that drew a hard-edged rectangle cannot come back -- while undoing most of the
# squaring. At 0.5 the flash body's mean coverage goes 0.25 -> 0.50, and the core stays 1.0.
COVERAGE_GAMMA = 0.5

MARKER_PREFIX = "// mvm: translucency rebuilt from luminance"
MARKER = (MARKER_PREFIX + "^%g (opaque alpha carried no shape; the gamma keeps an additive "
          "sprite from being shaped twice)" % COVERAGE_GAMMA)
TRANS_RE = re.compile(r'(\bTextureTranslucency\s+")([^"]+)(")')
COLOR_RE = re.compile(r'\bTextureColor\s+"([^"]+\.tga)"')
# Above this mean edge alpha the channel carries no usable falloff.
OPAQUE_EDGE_THRESHOLD = 200


def edge_mean(img) -> float:
    """Mean alpha along the texture's border -- the part a glow must fade out in."""
    a = img.resize((32, 32))
    px = list(a.getdata())
    border = ([px[i] for i in range(32)] + [px[31 * 32 + i] for i in range(32)]
              + [px[i * 32] for i in range(32)] + [px[i * 32 + 31] for i in range(32)])
    return sum(border) / len(border)


def needs_repair(content: Path, vmat_text: str) -> bool:
    m = TRANS_RE.search(vmat_text)
    if not m:
        return False
    value = m.group(2)
    if value.startswith("["):
        return True  # a literal constant can never have a shape
    from PIL import Image
    p = content / value
    if not p.is_file():
        return False
    try:
        with Image.open(p) as img:
            return edge_mean(img.convert("L")) > OPAQUE_EDGE_THRESHOLD
    except Exception:
        return False


def write_luminance(content: Path, color_rel: str, out_rel: str) -> bool:
    from PIL import Image
    src = content / color_rel
    if not src.is_file():
        return False
    try:
        with Image.open(src) as img:
            lum = img.convert("RGB").convert("L")
            if COVERAGE_GAMMA != 1.0:
                # 255 * (v/255)^gamma via a 256-entry LUT. Exact at both ends, so the
                # transparent surround stays transparent (no rectangle) and the core stays
                # fully covered; only the mid-tones the flash body is made of come back up.
                lut = [min(255, int(round(255.0 * ((i / 255.0) ** COVERAGE_GAMMA))))
                       for i in range(256)]
                lum = lum.point(lut)
            out = content / out_rel
            out.parent.mkdir(parents=True, exist_ok=True)
            lum.save(out)
        return True
    except Exception:
        return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--content-root", type=Path, required=True)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    content = args.content_root.resolve()
    materials = content / "materials"
    if not materials.is_dir():
        print("no materials tree at %s" % materials)
        return 1

    scanned = additive = repaired = skipped_shaped = 0
    for vmat in sorted(materials.rglob("*.vmat")):
        text = vmat.read_text(encoding="utf-8", errors="replace")
        scanned += 1
        if 'F_ADDITIVE_BLEND' not in text:
            continue
        additive += 1
        if MARKER in text:
            continue
        # Already repaired, but at a DIFFERENT gamma: redo it. needs_repair() cannot decide this
        # on its own -- TextureTranslucency now points at the generated map, whose edge is dark,
        # so it would answer "already shaped" and the tool could never re-tune itself.
        retune = MARKER_PREFIX in text
        if not retune and not needs_repair(content, text):
            skipped_shaped += 1
            continue
        color = COLOR_RE.search(text)
        if not color:
            continue
        lum_rel = color.group(1)[:-4] + "_a_lum.tga"
        if not args.dry_run and not write_luminance(content, color.group(1), lum_rel):
            continue
        new_text = TRANS_RE.sub(lambda m: m.group(1) + lum_rel + m.group(3), text, count=1)
        # Drop any previous gamma's marker so they cannot stack up across re-runs.
        new_text = "\n".join(l for l in new_text.splitlines() if MARKER_PREFIX not in l)
        new_text = new_text.rstrip("\n") + "\n" + MARKER + "\n"
        if not args.dry_run:
            vmat.write_text(new_text, encoding="utf-8")
        repaired += 1
        print("  repaired %s -> %s" % (vmat.relative_to(content).as_posix(), lum_rel))

    print("\n.vmat scanned            : %d" % scanned)
    print("additive materials       : %d" % additive)
    print("already had real falloff : %d" % skipped_shaped)
    print("coverage gamma           : %g" % COVERAGE_GAMMA)
    print("repaired%s : %d" % (" (dry run)" if args.dry_run else "          ", repaired))
    return 0


if __name__ == "__main__":
    sys.exit(main())
