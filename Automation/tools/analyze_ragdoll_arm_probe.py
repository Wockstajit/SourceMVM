"""Summarize mvm_debug armprobe controls and impulse results."""
import argparse
import json
import math
import re

parser = argparse.ArgumentParser()
parser.add_argument('log')
args = parser.parse_args()
pose = re.compile(r'hand=\(([^)]+)\) q=\(([^)]+)\)')
trials = []
trial = None
with open(args.log, encoding='utf-8', errors='replace') as stream:
    for line in stream:
        if '[ragdoll.weapon.armwatch]' not in line:
            continue
        match = pose.search(line)
        if not match:
            continue
        p = list(map(float, match[1].split()))
        q = list(map(float, match[2].split()))
        if 'phase=begin' in line:
            trial = {'force': float(re.search(r'force=([\d.]+)', line)[1]),
                     'origin': p, 'rotation': q, 'samples': 0,
                     'maxDisplacement': 0, 'maxDegrees': 0}
            trials.append(trial)
        elif trial:
            trial['samples'] += 1
            distance = math.dist(p, trial['origin'])
            dot = abs(sum(x*y for x,y in zip(q,trial['rotation'])))
            dot /= math.sqrt(sum(x*x for x in q) * sum(x*x for x in trial['rotation']))
            angle = 2 * math.degrees(math.acos(min(1, dot)))
            trial['maxDisplacement'] = max(trial['maxDisplacement'], distance)
            trial['maxDegrees'] = max(trial['maxDegrees'], angle)
print(json.dumps(trials, indent=2))
