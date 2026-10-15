#!/usr/bin/env python3
"""Regression checks for the smoke-trail-free Insurgency impact variant."""

from __future__ import annotations

import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "FX" / "tools" / "build-impact-smoke-variants.py"
spec = importlib.util.spec_from_file_location("impact_smoke_variants", MODULE_PATH)
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)

source = '''m_Children = [
    { m_ChildRef = resource:"particles/filmmaker/insurgency/impact_fx_ins/impact_generic_debris_tracer.vpcf" },
    { m_ChildRef = resource:"particles/filmmaker/insurgency/impact_fx_ins/impact_concrete_burst_less.vpcf" },
    { m_ChildRef = resource:"particles/filmmaker/insurgency/impact_fx_ins/impact_gravel_debris_less.vpcf" },
    { m_ChildRef = resource:"particles/filmmaker/povarehok/less/impacts/impact_fx/impact_generic_smoke_small.vpcf" },
]
m_hFallback = resource:"particles/filmmaker/insurgency/impact_fx_ins/impact_concrete_thick_less.vpcf"
'''

result = module.make_no_smoke_trails(source)
assert result.count('resource:"particles/dev/empty.vpcf"') == 2
assert "impact_gravel_debris_less_nosmoketrails.vpcf" in result
assert "impact_concrete_thick_less_nosmoketrails.vpcf" in result
assert "povarehok/less/impacts/impact_fx/impact_generic_smoke_small.vpcf" in result
assert module.make_no_smoke_trails(result) == result
print("OK: Insurgency ribbons removed, solid children and Povarhok puffs retained")
