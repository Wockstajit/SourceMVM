#!/usr/bin/env python3
"""Static checks for Modern HE white smoke-trail flow."""

from pathlib import Path
import importlib.util


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "FX" / "tools" / "animate-modern-he-trails.py"
spec = importlib.util.spec_from_file_location("animate_modern_he_trails", MODULE_PATH)
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)

renderer = """
		{
			_class = "C_OP_RenderTrails"
			m_hMaterial = resource:"materials/effects/fas_smoke_trail.vmat"
			m_hTexture = resource:"materials/effects/fas_smoke_trail.vtex"
			m_flAnimationRate = 0.1
			m_flMaxLength = 2000.0
			m_flMinLength = 0.0
		},
"""
assert module.is_he_smoke_trail_renderer(renderer)
assert not module.is_he_smoke_trail_renderer(
    '_class = "C_OP_RenderTrails"\n\t\t\tm_hMaterial = resource:"materials/effects/sparkwires.vmat"'
)

patched = module.patch_trail_renderer(renderer)
assert "m_flTextureVScrollRate = -50.0" in patched
assert "m_flTextureVWorldSize = 128.0" in patched
assert "m_bClampV = false" in patched
assert "m_bBlendFramesSeq0 = true" in patched
assert 'm_nAnimationType = "ANIMATION_TYPE_FIT_LIFETIME"' in patched
assert "m_flAnimationRate = 1.0" in patched
assert patched == module.patch_trail_renderer(patched)

spark = '_class = "C_OP_RenderTrails"\n\t\t\tm_hMaterial = resource:"materials/effects/sparkwires.vmat"\n'
assert module.patch_trail_renderer(spark) == spark

system = """
	m_Renderers =
	[
		{
			_class = "C_OP_RenderTrails"
			m_hMaterial = resource:"materials/effects/fas_smoke_trail.vmat"
			m_hTexture = resource:"materials/effects/fas_smoke_trail.vtex"
			m_flAnimationRate = 0.1
		},
	]
	m_Initializers =
	[
		{
			_class = "C_INIT_RandomSequence"
			m_nSequenceMin = 0
			m_nSequenceMax = 3
		},
	]
}
"""
out = module.patch_explosion_grenade(system)
assert module.MARKER in out
assert "m_nSequenceMin = 0" in out
assert "m_nSequenceMax = 0" in out
assert "m_nSequenceMax = 3" not in out

original_mks = """\
// Reconstructed from the VTF's embedded particle sheet by export-source1-vtf.py

sequence 0
frame fas_smoke_trail_seq0.tga 1

sequence 1
frame fas_smoke_trail_seq1.tga 1

sequence 2
frame fas_smoke_trail_seq2.tga 1
"""
flip = module.rewrite_trail_mks(original_mks)
assert module.MKS_MARKER in flip
seq0 = flip.split("sequence 0", 1)[1].split("sequence 1", 1)[0]
assert "fas_smoke_trail_seq0.tga" in seq0
assert "fas_smoke_trail_seq1.tga" in seq0
assert "fas_smoke_trail_seq2.tga" in seq0
assert flip == module.rewrite_trail_mks(flip)

converter = (ROOT / "FX/tools/convert-fx-source1.ps1").read_text(encoding="utf-8")
assert "animate-modern-he-trails.py" in converter
assert "Modern HE smoke-trail flow" in converter

print("OK: Modern HE trails get V-scroll, lifetime flipbook, seq 0, sparks stay out")
