#!/usr/bin/env python3
"""Regression checks for Povarhok's post-merge 3D impact-debris removal."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "FX" / "tools" / "strip-povarehok-3d-debris.py"
spec = importlib.util.spec_from_file_location("strip_povarehok_3d_debris", MODULE_PATH)
assert spec and spec.loader
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


model = 'particles/filmmaker/povarehok/less/impacts/impact_fx/clique1.vpcf'
source = f'''m_Children = [
    {{ m_ChildRef = resource:"{model}" }},
    {{ m_ChildRef = resource:"particles/filmmaker/povarehok/less/impacts/impact_fx/impact_generic_smoke_small.vpcf" }},
    {{ m_ChildRef = resource:"particles/filmmaker/povarehok/less/impacts/impact_fx/ricochet_sparks.vpcf" }},
]'''

stripped = module.strip_3d_model_children(source, {model})
assert 'resource:"particles/dev/empty.vpcf"' in stripped
assert 'impact_generic_smoke_small.vpcf' in stripped
assert 'ricochet_sparks.vpcf' in stripped
assert module.strip_3d_model_children(stripped, {model}) == stripped

print("OK: only Povarhok 3D model chunks are disabled; puffs and sparks remain")
