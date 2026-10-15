#!/usr/bin/env python3
"""Measure how long a particle system stays on screen, following its whole child closure.

WHY THIS EXISTS
    "Effect X lingers far longer than vanilla" is a claim about a NUMBER, and the number is
    authored in the `.vpcf`. Backlog item B16 (C4 explosion lingers, including after scrubbing)
    explicitly says to measure the swapped-in system against the CS2 original before assuming a
    fix, because the swap hook only ever renames a system -- it applies no lifetime normalisation,
    so a longer-lived replacement lingers exactly as long as its author made it.

WHAT IT MEASURES
    For each system in the closure, the LAST MOMENT A PARTICLE CAN STILL BE ALIVE:

        tail = emission_end + max_particle_lifetime

    where `emission_end` is the emitter's own start + duration (a burst emitter contributes its
    last scheduled burst), clamped by `C_OP_StopAfterCPDuration` when the system carries one, and
    `max_particle_lifetime` comes from the initialisers that set lifetime.

    A parent's child is offset by the child's own delay when one is authored. The reported figure
    for a tree is the maximum tail over every system in it.

    This is an UPPER BOUND on visible duration, not a measurement of when a particle stops being
    noticeable -- a sprite that fades out over its last second is authored as alive for that
    second. Comparing two systems with the same yardstick is the point; treat the absolute number
    as "authored duration", not "seconds of visible smoke".

USAGE
    python FX/tools/measure-particle-lifetime.py <root.vpcf> [<root.vpcf> ...] [--verbose]
    python FX/tools/measure-particle-lifetime.py --compare <a.vpcf> <b.vpcf>
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

# THE TWO SIDES SPELL LIFETIME DIFFERENTLY, AND READING ONLY ONE INFLATES EVERY COMPARISON.
#
# The converted Insurgency tree keeps the Source 1 shape: `C_INIT_RandomLifeTime` with
# `m_fLifetimeMin`/`m_fLifetimeMax`. Modern CS2 particles use neither -- across the whole vanilla
# C4 set there is exactly ONE `m_flConstantLifespan` and no `m_fLifetime*` at all. CS2 instead
# writes a generic `C_INIT_InitFloat` whose `m_nOutputField = 1` (LIFE_DURATION), taking the value
# from an `m_InputValue` block. Measuring vanilla with the Source 1 key alone reports 0 s of
# particle life for every one of its 34 systems, which made the first C4 comparison read 5.30x
# when the honest figure is lower.
LIFETIME_KEYS = ("m_fLifetimeMax", "m_fLifetimeMin", "m_flLifetime", "m_flLifeTime",
                 "m_flConstantLifespan")
# LIFE_DURATION. `m_nOutputField` is omitted when it is 0 (radius), so an absent field is NOT this.
LIFE_DURATION_FIELD = 1
INIT_FLOAT_RE = re.compile(r'_class\s*=\s*"C_INIT_InitFloat"(.*?)(?=_class\s*=\s*")', re.DOTALL)
EMIT_DURATION_KEYS = ("m_flEmissionDuration", "m_flEmitDuration")
EMIT_START_KEYS = ("m_flStartTime", "m_flEmissionStartTime")
STOP_CLASS = "C_OP_StopAfterCPDuration"
CHILD_RE = re.compile(r'm_ChildRef\s*=\s*resource:"([^"]+)"')
DELAY_RE = re.compile(r'm_flDelay\s*=\s*([-0-9.eE]+)')


def _numbers_for(text: str, key: str) -> list[float]:
    """Every literal value assigned to `key`.

    A vpcf scalar is often a whole block (`m_nType`/`m_flLiteralValue`/...) rather than a bare
    number, so accept both `key = 4.0` and `key = { ... m_flLiteralValue = 4.0 ... }`.
    """
    out: list[float] = []
    for m in re.finditer(re.escape(key) + r"\s*=\s*", text):
        rest = text[m.end():m.end() + 400]
        bare = re.match(r"([-0-9][-0-9.eE]*)", rest)
        if bare:
            out.append(float(bare.group(1)))
            continue
        if rest.lstrip().startswith("{"):
            lit = re.search(r"m_flLiteralValue\s*=\s*([-0-9][-0-9.eE]*)", rest)
            if lit:
                out.append(float(lit.group(1)))
    return out


def _max_of(text: str, keys: tuple[str, ...]) -> float:
    best = 0.0
    for key in keys:
        for value in _numbers_for(text, key):
            best = max(best, value)
    return best


def _init_float_lifetime(text: str) -> float:
    """Largest LIFE_DURATION written by a `C_INIT_InitFloat` -- how CS2 authors lifetime."""
    best = 0.0
    for block in INIT_FLOAT_RE.findall(text + '_class = "SENTINEL"'):
        field = re.search(r"m_nOutputField\s*=\s*(\d+)", block)
        if not field or int(field.group(1)) != LIFE_DURATION_FIELD:
            continue
        for key in ("m_flRandomMax", "m_flLiteralValue", "m_flRandomMin"):
            values = _numbers_for(block, key)
            if values:
                best = max(best, max(values))
                break
    return best


def measure_one(path: Path) -> dict:
    text = path.read_text(encoding="utf-8", errors="replace")
    lifetime = max(_max_of(text, LIFETIME_KEYS), _init_float_lifetime(text))
    emit_dur = _max_of(text, EMIT_DURATION_KEYS)
    emit_start = _max_of(text, EMIT_START_KEYS)
    stop = 0.0
    if STOP_CLASS in text:
        after = text[text.index(STOP_CLASS):]
        durations = _numbers_for(after[:2000], "m_flDuration")
        if durations:
            stop = max(durations)
    emission_end = emit_start + emit_dur
    if stop:
        emission_end = min(emission_end, stop) if emission_end else stop
    return {
        "lifetime": lifetime,
        "emit_start": emit_start,
        "emit_duration": emit_dur,
        "stop_after": stop,
        "tail": emission_end + lifetime,
    }


def walk(root: Path, verbose: bool = False) -> tuple[float, list[tuple[str, dict, float]]]:
    """Measure `root` and every system it pulls in. Returns (worst tail, per-system rows)."""
    # A vpcf child is addressed from the content root, so anchor on the ancestor that holds the
    # `particles/` directory this file sits under.
    parts = root.resolve().parts
    if "particles" not in parts:
        content_root = root.parent
    else:
        content_root = Path(*parts[:parts.index("particles")])

    rows: list[tuple[str, dict, float]] = []
    seen: set[Path] = set()
    # (file, time offset inherited from the parent that spawned it)
    queue: list[tuple[Path, float]] = [(root.resolve(), 0.0)]
    while queue:
        path, offset = queue.pop(0)
        if path in seen or not path.is_file():
            continue
        seen.add(path)
        stats = measure_one(path)
        rows.append((str(path.relative_to(content_root)), stats, offset))
        text = path.read_text(encoding="utf-8", errors="replace")
        for ref in CHILD_RE.findall(text):
            child = content_root / ref
            if not child.is_file():
                continue
            tail_of_block = text[text.index(ref):text.index(ref) + 600]
            delay = DELAY_RE.search(tail_of_block)
            queue.append((child, offset + (float(delay.group(1)) if delay else 0.0)))

    worst = max((offset + s["tail"] for _, s, offset in rows), default=0.0)
    return worst, rows


def report(path: Path, verbose: bool) -> float:
    worst, rows = walk(path, verbose)
    print("%s" % path.name)
    print("  systems in closure : %d" % len(rows))
    print("  longest tail       : %.2f s" % worst)
    ranked = sorted(rows, key=lambda r: -(r[2] + r[1]["tail"]))
    for name, s, offset in ranked[: (len(ranked) if verbose else 5)]:
        if offset + s["tail"] <= 0:
            continue
        print("    %-70s tail %6.2f s  (life %.2f, emit %.2f+%.2f, delay %.2f%s)"
              % (name, offset + s["tail"], s["lifetime"], s["emit_start"], s["emit_duration"],
                 offset, ", stop %.2f" % s["stop_after"] if s["stop_after"] else ""))
    return worst


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("files", type=Path, nargs="+")
    p.add_argument("--verbose", action="store_true", help="list every system, not just the top 5")
    p.add_argument("--compare", action="store_true",
                   help="print the ratio between the first two files")
    args = p.parse_args()

    worsts = [report(f, args.verbose) for f in args.files]
    if args.compare and len(worsts) >= 2 and worsts[1]:
        print("\n%s is %.2fx the authored duration of %s (%.2f s vs %.2f s)"
              % (args.files[0].name, worsts[0] / worsts[1], args.files[1].name,
                 worsts[0], worsts[1]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
