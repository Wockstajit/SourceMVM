#!/usr/bin/env python3
"""Make the ON-mode impact sparks half-size, un-stretched, forward-facing and more frequent.

Standalone on purpose: do NOT fold this into scale-povarehok-metal-sparks.py (radius only, a
different factor, already stamped into the tree) or into postprocess_insurgency.py's impact
passes (those own the whole impact family's room-scale fit, not spark readability). This tool
owns ONE thing: how the impact spark BURST reads on camera.

WHAT THE USER SAW (2026-09-03, with screenshots of an AK firefight)
    "The spark effect from the old Povarhok setup is way too large ... they are currently
     stretched enough that the texture becomes visibly pixelated ... the sparks also appear
     to be facing/moving backwards ... increase the frequency slightly."

WHAT THE TREE SAYS -- three separate faults, all measured, none guessed:

1. SIZE. The Povarhok spray family is the biggest spark in either impact tree:
       impact_spark_spray          Trails  radius 3.9-7.8   trail length 64
       impact_spark_spray_forward  Trails  radius 7.8-3.9   trail length 64
       impact_spark_spray_small    Trails  radius 3.9-7.8   trail length 32
   against Insurgency's own metal sparks at radius 1.05-3.5. (Those 3.9-7.8 numbers are
   already after scale-povarehok-metal-sparks.py's 0.65; this composes on top of it.)

2. STRETCH / PIXELATION. Their renderer draws materials/particles/sparktrail.vtex, which is
   128x64 holding FOUR 32x64 frames. A 32-pixel-wide frame magnified across a 64-unit trail is
   exactly the "you can see them pixelated" report. Halving the size halves that magnification;
   SPARK_LENGTH_SCALE takes a further bite out of the long axis specifically.

3. DIRECTION. For an impact the outward axis is Z (the surface normal), and every correctly
   authored spark leaf in either tree launches Z POSITIVE, away from the wall:
       impact_metal_sparks         Z  385 .. 1050
       impact_metal_sparks_c       Z   35 .. 1050
       ricochet_sparks (ins)       Z    0 ..  350
   The Povarhok spray family is the exception and fires INTO the surface:
       impact_spark_spray          Z -1600 .. -400
       impact_spark_spray_small    Z -1600 .. -400
       impact_spark_spray_forward  X  -100 .. -650, Y/Z +600 .. -600   (also min/max swapped)
   plus Insurgency's own small spray at Z -10.5 .. -7.0. That is the "moving backwards". The
   fix mirrors the normal component to positive and re-orders every min/max pair that the
   conversion emitted the wrong way round; the symmetric X/Y spread is left alone because that
   is the fan, not the direction.

SCOPE. The size/frequency change is POVAREHOK-ONLY -- that is the pack the user named, and
Insurgency's impact sparks already carry postprocess_insurgency's 0.35 room-scale cut, so
halving those again would erase them. The DIRECTION fix runs on both trees: a spark firing into
the wall is a bug wherever it lives.

Every pass is marker-guarded and records the factor it applied, so changing a constant here and
re-running divides the old one out first instead of compounding (the multiplicative-pass
landmine documented in postprocess_insurgency.py).

USAGE
    python FX/tools/tune-impact-sparks.py --content-root <.../content/source_mvm_fx> [--dry-run]
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

# The two ON-mode impact trees. Insurgency is the base; the hybrid randomiser in
# ParticleFxHook sends a share of every impact to the Povarehok tree instead.
INSURGENCY_IMPACTS = Path("particles/filmmaker/insurgency/impact_fx_ins")
POVAREHOK_IMPACTS = Path("particles/filmmaker/povarehok/less/impacts/impact_fx")

SPARK_NAME_RE = re.compile(r"(spark|ricochet)", re.IGNORECASE)

# --- dials ------------------------------------------------------------------------------------
# "Shrink the sparks to approximately half their current size."
SPARK_SIZE_SCALE = 0.5
# ...and take an extra bite out of the long axis only, which is what the 32-pixel sheet frame is
# being smeared across. 0.4 against 0.5 means the trail also gets ~20% less elongated.
SPARK_LENGTH_SCALE = 0.4
# "Increase the frequency slightly so sparks play more often, without making each spark larger."
SPARK_EMIT_SCALE = 1.35

SIZE_FIELDS = ("m_flRadiusMin", "m_flRadiusMax")
LENGTH_FIELDS = ("m_flMinLength", "m_flMaxLength")

SIZE_MARKER_PREFIX = "// mvm: impact spark size scaled by "
SIZE_MARKER_RE = re.compile(re.escape(SIZE_MARKER_PREFIX) + r"([0-9.]+)")
EMIT_MARKER_PREFIX = "// mvm: impact spark emit scaled by "
EMIT_MARKER_RE = re.compile(re.escape(EMIT_MARKER_PREFIX) + r"([0-9.]+)")
FORWARD_MARKER = "// mvm: impact sparks launch out of the surface, not into it"

NUM = r"([-0-9.]+)"
SPEED_RE = re.compile(
    r"m_LocalCoordinateSystemSpeed(Min|Max) = \[" + NUM + r", " + NUM + r", " + NUM + r"\]")
EMIT_RE = re.compile(r"(m_nParticlesToEmit\s*=\s*)([0-9]+)")
MAXP_RE = re.compile(r"(m_nMaxParticles\s*=\s*)([0-9]+)")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--content-root", type=Path, required=True)
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def _fmt(value: float) -> str:
    return ("%.4f" % value).rstrip("0").rstrip(".") or "0"


def _scale_fields(text: str, fields: tuple, factor: float) -> str:
    for field in fields:
        text = re.sub(
            r"(" + re.escape(field) + r"\s*=\s*)([0-9]+(?:\.[0-9]+)?)",
            lambda m: m.group(1) + _fmt(float(m.group(2)) * factor),
            text)
    return text


def scale_size(text: str) -> str:
    """Half-size radius plus the extra trail-length trim. Re-tunable via the stamped factor."""
    previous = SIZE_MARKER_RE.search(text)
    applied = float(previous.group(1)) if previous else 1.0
    if abs(applied - SPARK_SIZE_SCALE) < 1e-9:
        return text
    out = SIZE_MARKER_RE.sub("", text).rstrip("\n") + "\n"
    out = _scale_fields(out, SIZE_FIELDS, SPARK_SIZE_SCALE / applied)
    # The length dial is expressed relative to the size dial, so one stamped factor drives both.
    out = _scale_fields(out, LENGTH_FIELDS,
                        (SPARK_LENGTH_SCALE / SPARK_SIZE_SCALE) * (SPARK_SIZE_SCALE / applied))
    if out.rstrip("\n") == text.rstrip("\n"):
        return text  # a pure container with no size of its own
    return out.rstrip("\n") + "\n" + SIZE_MARKER_PREFIX + repr(SPARK_SIZE_SCALE) + "\n"


def scale_emit(text: str) -> str:
    previous = EMIT_MARKER_RE.search(text)
    applied = float(previous.group(1)) if previous else 1.0
    if abs(applied - SPARK_EMIT_SCALE) < 1e-9 or not EMIT_RE.search(text):
        return text
    factor = SPARK_EMIT_SCALE / applied
    out = EMIT_MARKER_RE.sub("", text).rstrip("\n") + "\n"
    counts = []

    def _bump(match: re.Match) -> str:
        base = int(match.group(2))
        want = max(base + 1, int(round(base * factor))) if base > 0 else base
        counts.append(want)
        return match.group(1) + str(want)

    out = EMIT_RE.sub(_bump, out)
    if counts:
        ceiling = max(counts)
        out = MAXP_RE.sub(
            lambda m: m.group(1) + str(max(int(m.group(2)), ceiling))
            if int(m.group(2)) > 0 else m.group(0), out)
    return out.rstrip("\n") + "\n" + EMIT_MARKER_PREFIX + repr(SPARK_EMIT_SCALE) + "\n"


def launch_outward(text: str) -> str:
    """Mirror the surface-normal component positive and put every min/max pair back in order.

    Z is the impact normal. X/Y are the symmetric fan and keep their signs; only their ORDER is
    corrected, because a min above its max collapses the range the engine samples.
    """
    if FORWARD_MARKER in text:
        return text
    pairs = {m.group(1): m for m in SPEED_RE.finditer(text)}
    if "Min" not in pairs or "Max" not in pairs:
        return text
    lo = [float(pairs["Min"].group(i)) for i in (2, 3, 4)]
    hi = [float(pairs["Max"].group(i)) for i in (2, 3, 4)]

    # Only two shapes are FAULTS. Everything else is authoring and is left alone.
    #  (a) the whole normal range is negative -- the burst fires entirely into the surface;
    #  (b) a min above its max -- the conversion emitted the pair swapped, which collapses the
    #      range the engine samples.
    # A SYMMETRIC range (impact_metal_sparks_tracer_b at Z -105..105) is a deliberate spherical
    # burst, not a fault: forcing it outward would turn an authored sphere into a hemisphere.
    fires_inward = lo[2] <= 0 and hi[2] <= 0
    swapped = any(a > b for a, b in zip(lo, hi))
    if not (fires_inward or swapped):
        return text

    new_lo = [min(a, b) for a, b in zip(lo, hi)]
    new_hi = [max(a, b) for a, b in zip(lo, hi)]
    if fires_inward:
        new_lo[2], new_hi[2] = sorted((abs(lo[2]), abs(hi[2])))
    if new_lo == lo and new_hi == hi:
        return text

    def _rewrite(match: re.Match) -> str:
        vec = new_lo if match.group(1) == "Min" else new_hi
        return ("m_LocalCoordinateSystemSpeed%s = [%s, %s, %s]"
                % (match.group(1), _fmt(vec[0]), _fmt(vec[1]), _fmt(vec[2])))

    return SPEED_RE.sub(_rewrite, text).rstrip("\n") + "\n" + FORWARD_MARKER + "\n"


def is_spark(path: Path) -> bool:
    # "_less" anywhere in the stem, not just at the end: the generated Less siblings are named
    # <name>_less_nosmoketrails as well, and they are regenerated from the On files.
    return bool(SPARK_NAME_RE.search(path.stem)) and "_less" not in path.stem


def main() -> int:
    args = parse_args()
    root = args.content_root.resolve()
    sized = turned = 0

    for tree, do_size in ((POVAREHOK_IMPACTS, True), (INSURGENCY_IMPACTS, False)):
        directory = root / tree
        if not directory.is_dir():
            print("skipped (not present): %s" % tree.as_posix())
            continue
        for path in sorted(directory.glob("*.vpcf")):
            if not is_spark(path):
                continue
            text = path.read_text(encoding="utf-8")
            out = text
            if do_size:
                out = scale_size(out)
                out = scale_emit(out)
            out = launch_outward(out)
            if out == text:
                continue
            if not args.dry_run:
                path.write_text(out, encoding="utf-8")
            rel = path.relative_to(root).as_posix()
            if do_size and SIZE_MARKER_PREFIX in out and SIZE_MARKER_PREFIX not in text:
                sized += 1
            if FORWARD_MARKER in out and FORWARD_MARKER not in text:
                turned += 1
            print("  %s" % rel)

    print("Impact sparks: %d resized/re-emitted, %d turned to fire out of the surface "
          "(size x%s, length x%s, emit x%s)."
          % (sized, turned, SPARK_SIZE_SCALE, SPARK_LENGTH_SCALE, SPARK_EMIT_SCALE))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
