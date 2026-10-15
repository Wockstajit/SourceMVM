#!/usr/bin/env python3
"""Static checks for the 1.7x On-mode burst/split sprite emit scale."""

from pathlib import Path
import importlib.util
import sys


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "FX" / "tools" / "postprocess_insurgency.py"
sys.path.insert(0, str(MODULE_PATH.parent))
spec = importlib.util.spec_from_file_location("postprocess_insurgency", MODULE_PATH)
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)

assert module.scaled_burst_emit_count(0) == 0
assert module.scaled_burst_emit_count(1) == 2
assert module.scaled_burst_emit_count(2) == 3
assert module.scaled_burst_emit_count(10) == 17
assert module.BURST_SPRITE_TEX_RE.search("m_hTexture = resource:\"materials/particles/ins_burstspark.vtex\"")
assert module.BURST_SPRITE_TEX_RE.search("m_hTexture = resource:\"materials/particles/ins_muzzleflash_split.vtex\"")
assert not module.BURST_SPRITE_TEX_RE.search("m_hTexture = resource:\"materials/particles/ins_burstspark_trail.vtex\"")
assert "boost_burst_split_sprites(root, insurgency, changed)" in (
    ROOT / "FX/tools/postprocess_insurgency.py"
).read_text(encoding="utf-8")
print("OK: burst/split emit 1.7x, trails excluded")
