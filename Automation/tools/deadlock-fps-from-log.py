"""Frame-rate windows from a Deadlock mvm_debug log.

The Deadlock hook writes two per-frame probes into %APPDATA%\\HLAE\\debuglogs\\mvm_debug_*.log:

  [deadlock.view]  every 64 view-setup calls, with a millisecond wall-clock timestamp
  [deadlock.perf]  every ~2 s: `frames=N meanMs=X peakMs=Y` for the main-thread pump

A measurement window is bracketed by echoing markers into the game console, which the log records
under [console.engine]:

  echo FPS_A_BEGIN   ...wait...   echo FPS_A_END

For each window this prints the median and 5th-percentile frame rate (from consecutive
deadlock.view stamps, 64 calls / elapsed seconds) and the mean/peak pump cost. It exits non-zero
when a window has fewer than 3 view samples, so an automated A/B cannot pass on no data.

  py -3 Automation/tools/deadlock-fps-from-log.py <log> [--json] [--calls-per-sample 64]
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
from dataclasses import dataclass, field

STAMP = re.compile(r"^\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\]")
VIEW = re.compile(r"\[deadlock\.view\] .*? n=(\d+)")
PERF = re.compile(r"\[deadlock\.perf\] .*?frames=(\d+) meanMs=([\d.]+) peakMs=([\d.]+)")
# Window names may contain underscores (FPS_P4A_B_BEGIN names the window "P4A_B"): the name is
# everything between "FPS_" and the final "_BEGIN" / "_END".
MARK = re.compile(r"\[console\.engine\].*?\bFPS_([A-Za-z0-9_]+?)_(BEGIN|END)\b")


def stamp_seconds(line: str) -> float | None:
    m = STAMP.match(line)
    if not m:
        return None
    h, mi, s, ms = (int(g) for g in m.groups())
    return h * 3600 + mi * 60 + s + ms / 1000.0


@dataclass
class Window:
    name: str
    views: list[tuple[float, int]] = field(default_factory=list)
    perf: list[tuple[float, float]] = field(default_factory=list)


def percentile(values: list[float], pct: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    k = (len(ordered) - 1) * pct
    lo = int(k)
    hi = min(lo + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (k - lo)


def parse(lines, calls_per_sample: int = 64) -> dict:
    open_windows: dict[str, Window] = {}
    closed: list[Window] = []
    day_offset = 0.0
    last_t = None
    for raw in lines:
        t = stamp_seconds(raw)
        if t is None:
            continue
        if last_t is not None and t + day_offset < last_t - 43200:
            day_offset += 86400.0  # the log crossed midnight
        t += day_offset
        last_t = t

        mark = MARK.search(raw)
        if mark:
            name, edge = mark.group(1), mark.group(2)
            if edge == "BEGIN":
                open_windows[name] = Window(name)
            elif name in open_windows:
                closed.append(open_windows.pop(name))
            continue
        view = VIEW.search(raw)
        if view:
            for w in open_windows.values():
                w.views.append((t, int(view.group(1))))
            continue
        perf = PERF.search(raw)
        if perf:
            for w in open_windows.values():
                w.perf.append((float(perf.group(2)), float(perf.group(3))))

    out = {}
    for w in closed:
        fps = []
        for (t0, n0), (t1, n1) in zip(w.views, w.views[1:]):
            dt = t1 - t0
            calls = n1 - n0 if n1 > n0 else calls_per_sample
            if dt > 0:
                fps.append(calls / dt)
        out[w.name] = {
            "samples": len(fps),
            "medianFps": round(statistics.median(fps), 2) if fps else 0.0,
            "p5Fps": round(percentile(fps, 0.05), 2) if fps else 0.0,
            "pumpMeanMs": round(statistics.mean(m for m, _ in w.perf), 3) if w.perf else None,
            "pumpPeakMs": round(max(p for _, p in w.perf), 3) if w.perf else None,
        }
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--calls-per-sample", type=int, default=64)
    args = ap.parse_args(argv)
    with open(args.log, encoding="utf-8", errors="replace") as f:
        result = parse(f, args.calls_per_sample)
    if not result:
        print("no closed FPS_<name>_BEGIN/END windows found", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for name, r in result.items():
            print(f"{name}: median={r['medianFps']} fps  p5={r['p5Fps']}  samples={r['samples']}  "
                  f"pumpMean={r['pumpMeanMs']} ms  pumpPeak={r['pumpPeakMs']} ms")
    return 1 if any(r["samples"] < 3 for r in result.values()) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
