"""Measure camera-frame drift in follow.mount.render debug logs (no game required)."""
import argparse
import json
import math
import re
from collections import defaultdict
from pathlib import Path


def vector(line, name):
    match = re.search(rf"\b{name}=\(([^)]+)\)", line)
    return tuple(map(float, match[1].split())) if match else None


def angle(a, b):
    cosine = sum(x*y for x, y in zip(a, b)) / (math.sqrt(sum(x*x for x in a)*sum(x*x for x in b)))
    return math.degrees(math.acos(max(-1, min(1, cosine))))


def analyze(path):
    groups = defaultdict(list)
    switches = []
    previous = None
    fallbacks = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "[follow.mount.fallback]" in line:
            fallbacks.append(line)
        if "[follow.mount.render]" not in line:
            continue
        fields = dict(re.findall(r"\b(tick|entity|handle|class|attach|oriented)=([^ ]+)", line))
        key = f"{fields['attach']}:{fields['handle']}"
        if key != previous:
            switches.append({k: fields[k] for k in ("tick", "entity", "handle", "class", "attach", "oriented")})
        previous = key
        groups[key].append({**fields, **{v: vector(line, v) for v in ("localPos", "localForward", "localUp", "base")}})
    report = {}
    for key, samples in groups.items():
        first = samples[0]
        report[key] = {
            "samples": len(samples), "ticks": [int(first["tick"]), int(samples[-1]["tick"])],
            "all_oriented": all(s["oriented"] == "1" for s in samples),
            "max_position_drift_units": max(math.dist(s["localPos"], first["localPos"]) for s in samples),
            "max_forward_drift_degrees": max(angle(s["localForward"], first["localForward"]) for s in samples),
            "max_up_drift_degrees": max(angle(s["localUp"], first["localUp"]) for s in samples),
            "mount_angle_ranges": [[min(s["base"][i] for s in samples), max(s["base"][i] for s in samples)] for i in range(3)],
        }
    return {"log": str(path), "groups": report, "switches": switches, "permanent_fallbacks": fallbacks}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--assert-stable", action="store_true")
    args = parser.parse_args()
    report = analyze(args.log)
    encoded = json.dumps(report, indent=2)
    print(encoded)
    if args.output:
        args.output.write_text(encoded + "\n", encoding="utf-8")
    if args.assert_stable:
        assert report["groups"], "No runtime samples"
        assert not report["permanent_fallbacks"], "Requested mount changed"
        for name, group in report["groups"].items():
            assert group["all_oriented"], f"{name}: missing mount orientation"
            assert group["max_position_drift_units"] < .01, f"{name}: position drift"
            assert group["max_forward_drift_degrees"] < .01, f"{name}: facing drift"
            assert group["max_up_drift_degrees"] < .01, f"{name}: roll drift"
