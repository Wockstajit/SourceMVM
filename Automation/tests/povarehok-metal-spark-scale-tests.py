#!/usr/bin/env python3
"""Static checks for shrinking Povarhok metal-impact sparks by 35%."""

from pathlib import Path
import importlib.util


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "FX" / "tools" / "scale-povarehok-metal-sparks.py"
spec = importlib.util.spec_from_file_location("scale_povarehok_metal_sparks", MODULE_PATH)
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)

assert module.SCALE == 0.65
assert module.is_spark_file(Path("impact_spark_spray.vpcf"))
assert module.is_spark_file(Path("ricochet_sparks.vpcf"))
assert not module.is_spark_file(Path("impact_generic_smoke_small.vpcf"))

source = "m_flRadiusMin = 10.0\nm_flRadiusMax = 20.0\n"
scaled = module.scale_spark_text(source)
assert "m_flRadiusMin = 6.5" in scaled
assert "m_flRadiusMax = 13" in scaled
assert module.MARKER in scaled
assert scaled == module.scale_spark_text(scaled)

converter = (ROOT / "FX/tools/convert-fx-source1.ps1").read_text(encoding="utf-8")
assert "scale-povarehok-metal-sparks.py" in converter
assert "Povarhok metal-spark scaler" in converter
assert converter.index("strip-povarehok-3d-debris.py") < converter.index(
    "scale-povarehok-metal-sparks.py"
)

print("OK: Povarhok metal sparks scale to 0.65 and stay out of smoke systems")
