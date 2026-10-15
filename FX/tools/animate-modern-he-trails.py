#!/usr/bin/env python3
"""Make Modern HE white smoke streaks flow instead of sitting still and wiggling.

Standalone on purpose: do not fold this into postprocess_modern. That file owns MW2019
gameplay composites and tracer ropes. This is the FAS grenade sprite-trail conversion gap.

GMod `explosion_grenade` draws `render_sprite_trail` on `effects/fas_smoke_trail` with
animation rate 0.1. Source 1 sprite trails use that rate to scroll the wisp texture along
the ribbon. source1import mapped it to C_OP_RenderTrails `m_flAnimationRate`, which in CS2
is sheet-frame playback. The reconstructed sheet is three sequences of one frame each, so
the rate is a no-op. What remains is radius 0.3->4.0 plus +/-5 jitter: the frozen noise
pattern that only "wiggles".

This pass:
  * rebuilds `fas_smoke_trail.mks` sequence 0 as a 3-frame flipbook of those looks
  * on the HE root trail renderer, plays that sheet across the particle lifetime and
    adds the same V-scroll the working MW tracer wisps use (`m_flTextureVScrollRate` +
    `m_flTextureVWorldSize`)
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import postprocess_common as common

MARKER = "// mvm: modern he smoke-trail flow"
MKS_MARKER = "// mvm: he smoke-trail flipbook"
HE_ROOT = "particles/filmmaker/modern/arc9_fas_explosions/explosion_grenade.vpcf"
TRAIL_MATERIAL = "fas_smoke_trail"
MKS_REL = "materials/effects/fas_smoke_trail.mks"
SCROLL_RATE = -50.0
WORLD_SIZE = 128.0
ANIM_RATE = 1.0

MKS_FLIPBOOK = """\
// Reconstructed from the VTF's embedded particle sheet by export-source1-vtf.py
{marker}

sequence 0
frame fas_smoke_trail_seq0.tga 1
frame fas_smoke_trail_seq1.tga 1
frame fas_smoke_trail_seq2.tga 1

sequence 1
frame fas_smoke_trail_seq1.tga 1

sequence 2
frame fas_smoke_trail_seq2.tga 1
""".format(marker=MKS_MARKER)

SEQ_MIN_RE = re.compile(r"(m_nSequenceMin = )(\d+)")
SEQ_MAX_RE = re.compile(r"(m_nSequenceMax = )(\d+)")
ANIM_RATE_RE = re.compile(r"(m_flAnimationRate = )([0-9.]+)")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--content-root", type=Path, required=True)
    return parser.parse_args()


def is_he_smoke_trail_renderer(block: str) -> bool:
    return "C_OP_RenderTrails" in block and TRAIL_MATERIAL in block


def insert_after_class(block: str, class_name: str, lines: list[str]) -> str:
    needle = f'_class = "{class_name}"'
    idx = block.find(needle)
    if idx < 0:
        return block
    indent = block[:idx].rsplit("\n", 1)[-1]
    injected = "".join(f"\n{indent}{line}" for line in lines)
    at = idx + len(needle)
    return block[:at] + injected + block[at:]


def patch_trail_renderer(block: str) -> str:
    """Add CS2 trail UV flow + lifetime sheet playback on a fas_smoke_trail renderer."""
    if not is_he_smoke_trail_renderer(block):
        return block
    additions: list[str] = []
    if "m_flTextureVScrollRate" not in block:
        additions.append(f"m_flTextureVScrollRate = {SCROLL_RATE}")
    if "m_flTextureVWorldSize" not in block:
        additions.append(f"m_flTextureVWorldSize = {WORLD_SIZE}")
    if "m_bClampV" not in block:
        additions.append("m_bClampV = false")
    if "m_bBlendFramesSeq0" not in block:
        additions.append("m_bBlendFramesSeq0 = true")
    if "m_nAnimationType" not in block:
        additions.append('m_nAnimationType = "ANIMATION_TYPE_FIT_LIFETIME"')
    if additions:
        block = insert_after_class(block, "C_OP_RenderTrails", additions)
    if ANIM_RATE_RE.search(block):
        block = ANIM_RATE_RE.sub(rf"\g<1>{ANIM_RATE}", block, count=1)
    if 'm_nAnimationType = "ANIMATION_TYPE_FIT_LIFETIME"' not in block and "m_nAnimationType" in block:
        block = re.sub(
            r'm_nAnimationType = "[^"]+"',
            'm_nAnimationType = "ANIMATION_TYPE_FIT_LIFETIME"',
            block,
            count=1,
        )
    return block


def clamp_trail_sequence(text: str) -> str:
    """Pin RandomSequence to the animated flipbook (seq 0)."""
    needle = '_class = "C_INIT_RandomSequence"'
    pos = text.find(needle)
    while pos != -1:
        start, end = common.block_span(text, pos)
        block = text[start:end]
        new_block = SEQ_MIN_RE.sub(r"\g<1>0", block, count=1)
        new_block = SEQ_MAX_RE.sub(r"\g<1>0", new_block, count=1)
        if new_block != block:
            text = text[:start] + new_block + text[end:]
            end = start + len(new_block)
        pos = text.find(needle, end)
    return text


def patch_explosion_grenade(text: str) -> str:
    edits: list[tuple[int, int, str]] = []
    for start, end in common.iter_renderer_blocks(text):
        block = text[start:end]
        new_block = patch_trail_renderer(block)
        if new_block != block:
            edits.append((start, end, new_block))
    for start, end, replacement in reversed(edits):
        text = text[:start] + replacement + text[end:]
    text = clamp_trail_sequence(text)
    if MARKER not in text:
        text = text.rstrip("\n") + "\n" + MARKER + "\n"
    return text


def rewrite_trail_mks(text: str) -> str:
    if MKS_MARKER in text and "frame fas_smoke_trail_seq1.tga 1" in text.split("sequence 0", 1)[-1].split("sequence 1", 1)[0]:
        return text
    return MKS_FLIPBOOK


def main() -> int:
    args = parse_args()
    root = args.content_root.resolve()
    changed = 0

    mks = root.joinpath(*MKS_REL.split("/"))
    if mks.is_file():
        original = mks.read_text(encoding="utf-8")
        new_text = rewrite_trail_mks(original)
        if new_text != original:
            mks.write_text(new_text, encoding="utf-8", newline="\n")
            changed += 1
            print(mks.as_posix())

    vpcf = root.joinpath(*HE_ROOT.split("/"))
    if vpcf.is_file():
        original = vpcf.read_text(encoding="utf-8")
        new_text = patch_explosion_grenade(original)
        if new_text != original:
            vpcf.write_text(new_text, encoding="utf-8")
            changed += 1
            print(vpcf.as_posix())

    print(f"Patched {changed} Modern HE smoke-trail file(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
