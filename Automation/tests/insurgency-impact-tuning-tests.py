#!/usr/bin/env python3
"""Regression checks for the Insurgency indoor bullet-impact tuning pass."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "FX" / "tools" / "postprocess_insurgency.py"
sys.path.insert(0, str(MODULE_PATH.parent))
spec = importlib.util.spec_from_file_location("postprocess_insurgency", MODULE_PATH)
assert spec and spec.loader
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def check(needle: str, text: str) -> None:
    assert needle in text, f"missing {needle!r} in:\n{text}"


debris = '''
m_LocalCoordinateSystemSpeedMin = [-100.0, -100.0, 50.0]
m_LocalCoordinateSystemSpeedMax = [100.0, 100.0, 500.0]
m_nParticlesToEmit = 15
m_Constraints =
[
    {
        _class = "C_OP_WorldTraceConstraint"
        m_flBounceAmount = 0.75
    },
]
m_Children =
[
    { m_ChildRef = resource:"particles/x.vpcf" },
    { m_ChildRef = resource:"particles/x.vpcf" },
]
'''
tuned = module.tune_impact_simulation(Path("impact_concrete.vpcf"), debris)
check("m_nParticlesToEmit = 8", tuned)
check("m_LocalCoordinateSystemSpeedMax = [45.0, 45.0, 225.0]", tuned)
check("m_flBounceAmount = 0.08", tuned)
assert tuned.count('resource:"particles/x.vpcf"') == 1
assert module.tune_impact_simulation(Path("impact_concrete.vpcf"), tuned) == tuned
settled = module.settle_impact_collisions(Path("impact_concrete.vpcf"), tuned)
check("m_flSlideAmount = 0.05", settled)
check("m_flStopSpeed = 40.0", settled)
check("m_nStickOnCollisionField = 30", settled)
assert module.settle_impact_collisions(Path("impact_concrete.vpcf"), settled) == settled

dust = '''
m_LocalCoordinateSystemSpeedMax = [150.0, 150.0, 650.0]
m_fLifetimeMax = 15.0
m_nAlphaMin = 75
m_nAlphaMax = 150
m_nParticlesToEmit = 3
'''
tuned_dust = module.tune_impact_simulation(Path("impact_concrete_dust_linger.vpcf"), dust)
check("m_LocalCoordinateSystemSpeedMax = [52.5, 52.5, 227.5]", tuned_dust)
check("m_fLifetimeMax = 2", tuned_dust)
check("m_nAlphaMin = 34", tuned_dust)
check("m_nAlphaMax = 68", tuned_dust)
check("m_nParticlesToEmit = 2", tuned_dust)

spark = "m_LocalCoordinateSystemSpeedMax = [350.0, 350.0, 350.0]\nm_nParticlesToEmit = 4\n"
tuned_spark = module.tune_impact_simulation(Path("ricochet_sparks.vpcf"), spark)
check("[350.0, 350.0, 350.0]", tuned_spark)
check("m_nParticlesToEmit = 4", tuned_spark)

print("OK: impact count, speed, bounce, dust visibility, de-duplication, and idempotence")
