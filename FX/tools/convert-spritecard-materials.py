#!/usr/bin/env python3
"""Convert Source 1 `SpriteCard` materials to CS2 `.vmat`, which source1import does not do.

WHY THIS EXISTS
    Measured 2026-09-02 over the Insurgency pack's 193 staged `.vmt`:

        source shader      count  converted  rate
        SpriteCard           107          1     1%      <-- every particle SPRITE
        UnlitGeneric          72         42    58%
        Refract                7          0     0%
        VertexLitGeneric       5          5   100%

    `SpriteCard` is the Source 1 shader for essentially every particle sprite -- smoke, fire,
    sparks, flares -- and source1import converts 1 of 107. Everything downstream follows:
    128 of 319 particle systems (40% of the pack) reference a material that does not exist, and a
    particle with no material renders a WHITE ERROR QUAD. That is what the user saw as "the grenade
    seems huge, the whole screen is damn near covered in smoke" -- a 400-unit white quad, not smoke.

    The build never caught it because the converter's closure step classifies an unconverted
    material as an `externalResource` ("the base game provides this"). CS2 does not ship
    `ins_smoke_v2.vmat`.

WHY IT IS SAFE AND SMALL
    The textures already convert: 2135 `.tga` exist in the content tree, including every one of the
    missing materials'. Only the `.vmat` linking texture to shader is absent. So this writes the
    missing link, and nothing else.

MAPPING (derived from what source1import ITSELF emits for this pack, not invented -- see
`ar2_altfire1b.vmat`, which compiles here today)

    SpriteCard                    -> shader csgo_complex.vfx + F_UNLIT 1
    $basetexture <p>              -> TextureColor  materials/<p>.tga
    $additive 1                   -> F_ADDITIVE_BLEND 1
    $mod2x 1                      -> NOTHING; CS2 cannot express it. See build_vmat.
    $translucent 1 (or $additive) -> F_TRANSLUCENT 1
    $vertexcolor / $vertexalpha   -> F_VERTEX_COLOR 1
    $nocull 1                     -> F_RENDER_BACKFACES 1
    (always)                      -> TextureRoughness materials/default/default_rough_s1import.tga
    TextureTranslucency            = <base>_a_trans.tga when that companion exists, else a literal
                                     white [1 1 1 1], exactly as source1import does.

    NOT mapped, deliberately: $depthblend/$depthblendscale (soft particles), $startfadesize /
    $endfadesize / $maxsize, $blendframes, $overbrightfactor, $addself, $sequence_blend_mode.
    Those are RENDERER-side behaviour in Source 2, not material flags -- they belong on the .vpcf's
    renderer, and inventing material keys for them would just be dropped by the compiler. They are
    listed in the report so the gap to a true 1:1 port is visible rather than silent.

USAGE
    python FX/tools/convert-spritecard-materials.py \\
        --source1-root build/fx/fx-source1import/source1_game \\
        --content-root build/fx/fx-source1import/source2/content/source_mvm_fx [--dry-run]

Existing .vmat files are never overwritten: source1import's own output wins.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

SHADER = "csgo_complex.vfx"
# Stamped into a material whose Source 1 blend mode CS2 cannot reproduce (see build_vmat).
MOD2X_MARKER = "// mvm: source was $mod2x -- CS2 has no modulate blend, this is alpha-blended"
DEFAULT_ROUGHNESS = "materials/default/default_rough_s1import.tga"

# Parameters that are renderer behaviour in Source 2, not material flags. Counted for the report so
# the distance to a true 1:1 port stays visible.
RENDERER_SIDE = ("$depthblend", "$depthblendscale", "$startfadesize", "$endfadesize", "$maxsize",
                 "$blendframes", "$overbrightfactor", "$addself", "$sequence_blend_mode",
                 "$maxlumframeblend1", "$maxlumframeblend2", "$vertexfogamount")


def parse_vmt(text: str) -> tuple[str, dict[str, str]]:
    shader = ""
    m = re.match(r'\s*"?([A-Za-z_]+)"?', text)
    if m:
        shader = m.group(1).lower()
    params: dict[str, str] = {}
    for key, value in re.findall(r'"?(\$[A-Za-z0-9_]+)"?\s+"?([^"\r\n]*)"?', text):
        params[key.lower()] = value.strip().strip('"')
    return shader, params


def texture_path(raw: str) -> str:
    """`particles/loopsmoke.vtf` or `particles/loopsmoke` -> materials/particles/loopsmoke.tga"""
    rel = raw.strip().replace("\\", "/").strip('"')
    rel = re.sub(r"\.vtf$", "", rel, flags=re.IGNORECASE)
    return "materials/" + rel + ".tga"


def extract_alpha(color_tga: Path, out_tga: Path) -> bool:
    """Write the base texture's ALPHA channel out as an 8-bit greyscale TGA.

    `TextureTranslucency` wants a SINGLE-CHANNEL map. Pointing it at the 32-bit colour texture makes
    the sprite's COLOUR drive its translucency -- a red blood sprite then renders as solid red
    blocks, which is exactly what appeared in game after the first attempt. source1import solves
    this by writing a companion `_a_trans.tga` (verified on its own output: TGA image type 3,
    8 bpp), so this does the same for the SpriteCard materials it never converted.
    """
    try:
        from PIL import Image
    except ImportError:
        return False
    try:
        with Image.open(color_tga) as img:
            if "A" not in img.getbands():
                return False  # no alpha to extract; caller falls back to no translucency map
            alpha = img.convert("RGBA").getchannel("A")
            out_tga.parent.mkdir(parents=True, exist_ok=True)
            alpha.save(out_tga)
        return True
    except Exception:
        return False


def build_vmat(params: dict[str, str], content_root: Path) -> str | None:
    base = params.get("$basetexture")
    if not base:
        return None
    color = texture_path(base)
    if not (content_root / color).is_file():
        return None  # no texture -> a .vmat would just be a different flavour of broken

    additive = params.get("$additive", "0").strip() not in ("", "0")
    # $mod2x IS A BLEND MODE, AND IGNORING IT PAINTS AN OPAQUE WHITE CARD.
    #
    # Source 1's SpriteCard $mod2x means "modulate 2x": result = 2 * src * dst, so a mid-grey
    # texel leaves the background alone and only brighter texels glow. It is not $additive, so
    # reading only $additive left these materials as PLAIN ALPHA BLEND -- and an alpha-blended
    # white flare is a solid white quad, not a glow.
    #
    # Seen in game 2026-09-02: the HE grenade's near-camera flare (generic_nearglow ->
    # particle_flare_004b_mod_ob, `$mod2x 1`) drew as a hard-edged white card covering most of the
    # frame, with visible straight sprite edges -- and SHRINKING it made coverage worse (31.8% ->
    # 73.2% of the frame), which is what finally showed the size was never the problem.
    #
    # MAPPING IT TO F_ADDITIVE_BLEND WAS TRIED AND IS WRONG. Measured on the same frame: alpha
    # blend whited out 31.8% of it, additive whited out 93.3%. mod2x can DARKEN as well as
    # brighten (a texel below mid-grey attenuates the background), and this material also carries
    # `$overbrightfactor 10` -- rendered additively that multiplies straight into a full white-out.
    # Additive shares only half of mod2x's behaviour, and it is the wrong half here.
    #
    # csgo_complex.vfx has no mod2x mode, and there is no flag combination that reproduces one, so
    # these four materials stay alpha-blended: wrong, but the least wrong of the options measured.
    # Their systems are handled at the PARTICLE level instead -- see HE_DROPPED_CHILDREN in
    # postprocess_insurgency.py, which drops the unrepresentable near-camera flare from the HE
    # blast rather than shipping a white card. Do not "fix" this by reaching for additive again.
    mod2x = params.get("$mod2x", "0").strip() not in ("", "0")
    # A SPRITE CARD IS IMPLICITLY TRANSLUCENT AND VERTEX-COLOURED. `$translucent` and
    # `$vertexcolor` are UnlitGeneric conventions; SpriteCard sources mostly omit them (only 37
    # and 42 of the 107 write them) because the shader always alpha-blends and always takes the
    # particle's per-particle colour/alpha. Reading them literally produced materials with NO
    # translucency, so the sprites rendered as OPAQUE QUADS -- solid blocks in game where smoke
    # belongs. Forced on; only ADDITIVE is still read from the source.
    translucent = True
    vertex_color = True
    nocull = params.get("$nocull", "0").strip() not in ("", "0")

    # Alpha: prefer the separate _a_trans.tga that source1import extracts when one exists,
    # otherwise point at the base texture -- these .tga are 32-bit and carry their own alpha
    # (verified on loopsmoke.tga: bpp=32). A literal white would make every sprite fully
    # OPAQUE, which is the bug that rendered solid blocks where smoke belongs.
    trans_companion = color[:-4] + "_a_trans.tga"
    if not (content_root / trans_companion).is_file():
        if not extract_alpha(content_root / color, content_root / trans_companion):
            trans_companion = None
    translucency = trans_companion

    lines = ['"Layer0"', "{", '\tshader\t"%s"' % SHADER, '\tF_UNLIT\t"1"',
             '\tTextureColor\t"%s"' % color]
    if additive:
        lines.append('\tF_ADDITIVE_BLEND\t"1"')
    if translucent:
        lines.append('\tF_TRANSLUCENT\t"1"')
        if translucency:
            lines.append('\tTextureTranslucency\t"%s"' % translucency)
    if vertex_color:
        lines.append('\tF_VERTEX_COLOR\t"1"')
    if nocull:
        lines.append('\tF_RENDER_BACKFACES\t"1"')
    lines.append('\tTextureRoughness\t"%s"' % DEFAULT_ROUGHNESS)
    lines.append("}")
    body = "\n".join(lines) + "\n"
    if mod2x:
        # Leave a machine-readable note that this material's blend mode could NOT be reproduced.
        # postprocess_insurgency.py reads this marker to drop the particles that exist only to
        # draw such a sprite, rather than letting them ship as white cards. Keeping the signal in
        # the asset means a rebake re-derives it instead of relying on a hand-kept name list.
        body += MOD2X_MARKER + "\n"
    return body


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--source1-root", type=Path, required=True)
    p.add_argument("--content-root", type=Path, required=True)
    p.add_argument("--dry-run", action="store_true")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    src = args.source1_root.resolve()
    content = args.content_root.resolve()
    mats = src / "materials"
    if not mats.is_dir():
        print("no materials tree at %s" % mats)
        return 1

    seen = written = skipped_existing = skipped_no_texture = 0
    renderer_gaps: dict[str, int] = {}

    for vmt in sorted(mats.rglob("*.vmt")):
        shader, params = parse_vmt(vmt.read_text(encoding="utf-8", errors="replace"))
        if shader != "spritecard":
            continue
        seen += 1
        rel = vmt.relative_to(mats).with_suffix(".vmat")
        out = content / "materials" / rel
        if out.is_file():
            skipped_existing += 1
            continue
        body = build_vmat(params, content)
        if body is None:
            skipped_no_texture += 1
            continue
        for key in RENDERER_SIDE:
            if key in params:
                renderer_gaps[key] = renderer_gaps.get(key, 0) + 1
        if not args.dry_run:
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(body, encoding="utf-8")
        written += 1

    print("SpriteCard sources: %d" % seen)
    print("  written            : %d%s" % (written, " (dry run)" if args.dry_run else ""))
    print("  already converted  : %d" % skipped_existing)
    print("  no matching texture: %d" % skipped_no_texture)
    if renderer_gaps:
        print("\nRenderer-side parameters seen but NOT expressible in the material "
              "(the remaining gap to a true 1:1 port):")
        for key, count in sorted(renderer_gaps.items(), key=lambda kv: -kv[1]):
            print("    %-24s %d" % (key, count))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
