"""Check independent ragdoll.weapon.draw samples from an mvm_debug log.

Run after replaying a death with mvm_debug enabled. Immediate pose/final write
readbacks deliberately do not count as verification. Quaternion signs are
equivalent; compare angular distance after normalization.
"""
import argparse
import json
import math
import re
from pathlib import Path


def analyze(path, max_position=1.0, max_angle=2.0):
    pattern = re.compile(
        r"actual=\(([^)]+)\) q=\(([^)]+)\) expected=\(([^)]+)\) eq=\(([^)]+)\)"
    )
    result = dict(log=str(path), samples=0, invalid=0, failures=0,
                  max_position_error=0.0, max_angle_degrees=0.0, users={}, worst=None)
    for line in path.read_text(errors="replace").splitlines():
        if "[ragdoll.weapon.draw]" not in line:
            continue
        result["samples"] += 1
        match = pattern.search(line)
        try:
            if not match or "ok=1 " not in line:
                raise ValueError("unreadable transform")
            actual, q, expected, eq = [list(map(float, s.split())) for s in match.groups()]
            if list(map(len, (actual, q, expected, eq))) != [3, 4, 3, 4]:
                raise ValueError("wrong transform size")
            if not all(math.isfinite(v) for values in (actual, q, expected, eq) for v in values):
                raise ValueError("nonfinite transform")
            distance = math.dist(actual, expected)
            norm = math.sqrt(sum(v*v for v in q)*sum(v*v for v in eq))
            if norm < 1e-12:
                raise ValueError("zero quaternion")
            dot = abs(sum(a*b for a, b in zip(q, eq))) / norm
            angle = math.degrees(2*math.acos(min(1.0, dot)))
        except (ValueError, TypeError):
            result["invalid"] += 1
            continue
        user = re.search(r"user=(\d+)", line).group(1)
        result["users"][user] = result["users"].get(user, 0) + 1
        if distance > result["max_position_error"] or angle > result["max_angle_degrees"]:
            result["worst"] = line
        result["max_position_error"] = max(result["max_position_error"], distance)
        result["max_angle_degrees"] = max(result["max_angle_degrees"], angle)
        result["failures"] += int(distance > max_position or angle > max_angle)
    result["passed"] = bool(result["samples"] and not result["invalid"] and not result["failures"])
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--max-position", type=float, default=1.0)
    parser.add_argument("--max-angle", type=float, default=2.0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    results = [analyze(path, args.max_position, args.max_angle) for path in args.logs]
    text = json.dumps(results, indent=2)
    if args.output:
        args.output.write_text(text + "\n")
    print(text)
    raise SystemExit(0 if all(result["passed"] for result in results) else 1)
