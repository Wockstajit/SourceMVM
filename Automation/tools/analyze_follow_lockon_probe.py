"""Summarize Lock-on render/early-sample disagreement and smoothing clock probes."""
import argparse
import json
import math
import re
from pathlib import Path


def vec(line, name):
    m = re.search(rf"\b{name}=\(([^)]+)\)", line)
    return tuple(map(float, m[1].split())) if m else None


def analyze(path):
    rows, clocks = [], []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "[follow.lockon.render]" in line:
            fields = dict(re.findall(r"\b(tick|time|dt|look|position)=([\d.e+-]+)", line))
            rows.append({**fields, **{k: vec(line, k) for k in ("fresh", "stepped", "camera", "aim", "output", "error")}})
        if "[follow.lockon.frame]" in line:
            fields = dict(re.findall(r"\b(dt|advancing)=([\d.e+-]+)", line))
            clocks.append({k: float(v) for k, v in fields.items()})
    if not rows:
        raise ValueError("No Lock-on render probes")
    errors = [math.hypot(*r["error"]) for r in rows]
    stale = [math.dist(r["fresh"], r["stepped"]) for r in rows]
    report = {
        "log": str(path), "samples": len(rows),
        "ticks": [min(int(r["tick"]) for r in rows), max(int(r["tick"]) for r in rows)],
        "max_early_render_target_difference_units": max(stale),
        "max_aim_error_degrees": max(errors),
        "rms_aim_error_degrees": math.sqrt(sum(x*x for x in errors)/len(errors)),
        "camera_position_movement_units": max(math.dist(r["camera"], rows[0]["camera"]) for r in rows),
        "main_frame_samples": len(clocks),
        "main_dt_at_tick_boundary": sorted(set(c["dt"] for c in clocks if c.get("advancing") == 1))[:20],
    }
    render_dt = [float(r["dt"]) for r in rows if "dt" in r]
    if render_dt:
        report["render_dt_range"] = [min(render_dt), max(render_dt)]
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--assert-exact", action="store_true", help="Use only with smoothing, prediction and aim trims zero")
    args = parser.parse_args()
    report = analyze(args.log)
    encoded = json.dumps(report, indent=2)
    print(encoded)
    if args.output:
        args.output.write_text(encoded + "\n", encoding="utf-8")
    if args.assert_exact:
        assert report["samples"] >= 30, "Not enough render samples"
        assert report["max_aim_error_degrees"] < .01, "Camera is aiming at an earlier target pose"
        assert report["camera_position_movement_units"] < .01, "Lock-on camera position moved"
