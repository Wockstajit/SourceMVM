#!/usr/bin/env python3
"""Static checks for scoped nade/impact smoke depth feathering."""

from pathlib import Path
import importlib.util


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "FX" / "tools" / "feather-nade-impact-smoke.py"
spec = importlib.util.spec_from_file_location("feather_nade_impact_smoke", MODULE_PATH)
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)

assert module.smoke_feather_dist(0) == 24.0
assert module.smoke_feather_dist(70) == 24.0
assert module.smoke_feather_dist(100) == 25.0
assert module.smoke_feather_dist(170) == 42.5
assert module.smoke_feather_dist(400) == 48.0

smoke_block = 'm_hMaterial = resource:"materials/particle/ac/insandstorm_t_thinsmoke_07_bc.vmat"'
assert module.is_smoke_renderer("simpleartyfx_mid_shockwave", smoke_block)
assert module.is_smoke_renderer("impact_generic_smoke_large", smoke_block)
assert module.is_smoke_renderer(
    "grenade_smoke_new", 'm_hMaterial = resource:"materials/effects/fas_dust_a.vmat"'
)
assert not module.is_smoke_renderer(
    "ac_grenade_explosion_sparks_glow",
    'm_hMaterial = resource:"materials/particle/bf3test.vmat"',
)
assert not module.is_smoke_renderer(
    "muzzleflash_makarov_1p_sparks",
    'm_hMaterial = resource:"materials/particles/ins_burstspark.vmat"',
)
assert not module.is_smoke_renderer(
    "impact_generic_dust_tracer",
    'm_hMaterial = resource:"materials/particles/flecks_01.vmat"',
)

converter = (ROOT / "FX/tools/convert-fx-source1.ps1").read_text(encoding="utf-8")
assert "feather-nade-impact-smoke.py" in converter
assert "Nade/impact smoke feather" in converter

print("OK: nade/impact smoke feather is scoped, distance scales, sparks stay out")
