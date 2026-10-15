#!/usr/bin/env python3
"""Restore the MW2019 (Modern) tracer glow/trail brightness and size to their shipped values.

Standalone on purpose: this is a REPAIR of a tree that a converter pass damaged, not a
converter pass. Do not fold it into postprocess_modern.py; that module's fix is the marker
fence added to patch_modern_tracer_glow_brightness, which prevents the damage from recurring.

WHAT THE USER SAW (2026-09-03): "modern tracers aren't working, I don't see the wisp or trails".

WHAT WAS ACTUALLY HAPPENING (measured on the live game, then in the tree):
  * The swap DOES fire: `fx names tracer` showed weapon_tracers_assrifle 540 seen / 362 acted,
    and `fx recent` showed every creation retargeted to mw2019_tracer_slow / _small / etc.
    So the Modern asset was being created -- and drawn invisibly.
  * Every brightness/size field on the ten mw2019_tracer leaves was a clean power-of-two
    fraction of its shipped value, with NO marker on any file:
        mgbase_tracer_trail (the wisp)   alpha 55-60 -> 2-2     radius 6-6.5 -> 0.1875-0.203
        mgbase_tracer_glow               alpha 75    -> 2       radius 2     -> 0.0625
        mw2019_tracer* (the trail root)                         radius 1.5-2 -> 0.047-0.0625
    6.5 / 32 = 0.203125. That is postprocess_modern.patch_modern_tracer_glow_brightness
    (x0.5 on alpha, radius, constant radius, start/end scale) applied FIVE times on top of
    the one intentional application, because it carried no idempotency marker. Alpha 2 of 255
    on a 0.2-unit rope is invisible by arithmetic.

SOURCE OF TRUTH for the restore is the previous build's copy of the same files
(build/fx/povarehok-source1import/...), which holds the values after the ONE intended halving
(glow 75 / 2, wisp 55-60 / 6-6.5, self-illum 0.5). Only the fields that pass touches are
copied across, field by field; everything else in the current files (the later trail-scroll
patch that the older copy lacks on weapon_tracers_4incendiary, child lists, materials) is
left exactly as it is. A blind file copy would have quietly reverted that scroll patch.

Stamps TRACER_BRIGHTNESS_MARKER (the same marker the fenced pass now checks) so the
in-place tuning entry points will not halve these files again. A fresh full conversion
starts from unmarked source, halves once as intended, and stamps.

USAGE
    python FX/tools/restore-modern-tracer-brightness.py \
        --content-root <.../fx-source1import/.../content/source_mvm_fx> \
        --reference-root <.../povarehok-source1import/.../content/source_mvm_fx> [--dry-run]
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

TRACER_DIR = Path("particles/filmmaker/modern/mw2019_tracer")
# Exactly the fields patch_modern_tracer_glow_brightness multiplies. Self-illum is deliberately
# NOT here: its regex only ever matched the literal 1.0, so it stopped at 0.5 (correct) and
# never compounded.
FIELDS = ("m_nAlphaMin", "m_nAlphaMax", "m_flRadiusMin", "m_flRadiusMax",
          "m_flConstantRadius", "m_flStartScale", "m_flEndScale")
# Keep in sync with postprocess_modern.TRACER_BRIGHTNESS_MARKER.
MARKER = "// mvm: modern tracer brightness applied once (x0.5)"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--content-root", type=Path, required=True)
    parser.add_argument("--reference-root", type=Path, required=True)
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def field_values(text: str, field: str) -> list:
    """Every occurrence of `field = <number>` in document order (a file can hold several)."""
    return re.findall(r"(?m)^([ \t]*" + re.escape(field) + r"\s*=\s*)(-?[0-9]+(?:\.[0-9]+)?)", text)


def restore(current: str, reference: str) -> tuple:
    """Write the reference's values for FIELDS into `current`, occurrence by occurrence."""
    out = current
    changed = 0
    for field in FIELDS:
        ref = [v for _, v in field_values(reference, field)]
        if not ref:
            continue
        seen = {"i": 0}
        pattern = re.compile(r"(?m)^([ \t]*" + re.escape(field) + r"\s*=\s*)(-?[0-9]+(?:\.[0-9]+)?)")

        def _sub(match: re.Match) -> str:
            i = seen["i"]
            seen["i"] = i + 1
            if i >= len(ref):
                return match.group(0)   # more occurrences than the reference has: leave it
            return match.group(1) + ref[i]

        new = pattern.sub(_sub, out)
        if new != out:
            changed += 1
            out = new
    return out, changed


def main() -> int:
    args = parse_args()
    root = args.content_root.resolve()
    ref_root = args.reference_root.resolve()
    directory = root / TRACER_DIR
    ref_dir = ref_root / TRACER_DIR
    if not directory.is_dir():
        raise SystemExit("tracer directory not found: %s" % directory)
    if not ref_dir.is_dir():
        raise SystemExit("reference tracer directory not found: %s" % ref_dir)

    restored = missing = 0
    for path in sorted(directory.glob("*.vpcf")):
        if "_less" in path.stem:
            continue
        ref = ref_dir / path.name
        if not ref.is_file():
            missing += 1
            print("  no reference for %s -- left alone" % path.name)
            continue
        text = path.read_text(encoding="utf-8")
        out, fields_changed = restore(text, ref.read_text(encoding="utf-8", errors="replace"))
        if MARKER not in out:
            out = out.rstrip("\n") + "\n" + MARKER + "\n"
        if out == text:
            continue
        if not args.dry_run:
            path.write_text(out, encoding="utf-8")
        restored += 1
        print("  %-32s %d field(s) restored" % (path.stem, fields_changed))

    print("Modern tracer brightness: %d file(s) restored to shipped values, %d without a reference."
          % (restored, missing))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
