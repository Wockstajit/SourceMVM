"""Behavior checks for smoke lighting without changing sprite coverage or motion."""
import sys
import re
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "FX" / "tools"))
from postprocess_common import light_weapon_smoke


class WeaponSmokeLightingTests(unittest.TestCase):
    def test_modern_smoke_gets_lighting_even_without_existing_renderer_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "particles/filmmaker/modern/arc9_fas_muzzleflashes/muzzle_smoke_pistol.vpcf"
            path.parent.mkdir(parents=True)
            path.write_text('''{
 m_Renderers = 
\t[
 {
  _class = "C_OP_RenderSprites"
  m_hMaterial = resource:"materials/effects/fas_dust_b.vmat"
  m_nAlphaMin = 34
 }
 ]
}''')
            self.assertEqual(light_weapon_smoke(root), [path.relative_to(root).as_posix()])
            result = path.read_text()
            floor = float(re.search(r"m_flSelfIllumAmount = ([0-9.]+)", result).group(1))
            self.assertGreater(floor, 0)
            self.assertLessEqual(floor, 0.25)
            self.assertIn("m_bPerVertexLighting = true", result)
            self.assertIn("m_nAlphaMin = 34", result)
            self.assertEqual(light_weapon_smoke(root), [])

    def test_smoke_is_lit_without_changing_emission_alpha_or_motion(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = root / "particles/filmmaker/povarehok/regular/weapons/cs_weapon_fx"
            base.mkdir(parents=True)
            smoke = base / "weapon_muzzle_flash_smoke_small2.vpcf"
            original = '''{
 m_Renderers = 
	[
 {
  _class = "C_OP_RenderSprites"
  m_hMaterial = resource:"materials/particle/smoke1/smoke1.vmat"
  m_flSelfIllumAmount = 1.0
  m_nAlphaMin = 40
  m_nOutputBlendMode = "PARTICLE_OUTPUT_BLEND_MODE_ALPHA"
 }
 ]
 m_Gravity = [0, 0, 40]
 m_flLifetimeMax = 0.4
}'''
            smoke.write_text(original)
            flame = base / "muzzle_flame.vpcf"
            flame.write_text(original.replace("smoke1", "muzzleflash"))
            glow = base / "muzzle_smoke_glow.vpcf"
            additive = original.replace("PARTICLE_OUTPUT_BLEND_MODE_ALPHA", "PARTICLE_OUTPUT_BLEND_MODE_ADD")
            glow.write_text(additive)
            impact = root / "particles/filmmaker/insurgency/impact_fx_ins/impact_smoke.vpcf"
            impact.parent.mkdir(parents=True)
            impact.write_text(original)
            grenade = base / "molotov_trail_smoke.vpcf"
            grenade.write_text(original)
            mixed_glow = base / "muzzle_smoke_flash.vpcf"
            mixed_glow.write_text(original.replace("smoke1", "muzzleflash"))
            self.assertEqual(light_weapon_smoke(root), [smoke.relative_to(root).as_posix()])
            result = smoke.read_text()
            floor = float(re.search(r"m_flSelfIllumAmount = ([0-9.]+)", result).group(1))
            self.assertGreater(floor, 0, "Low-alpha smoke must remain readable when native scene light is zero")
            self.assertLessEqual(floor, 0.25, "The visibility floor must not restore fullbright smoke")
            self.assertIn("m_bPerVertexLighting = true", result)
            self.assertIn("m_nAlphaMin = 40", result)
            self.assertIn("m_Gravity = [0, 0, 40]", result)
            self.assertIn("m_flLifetimeMax = 0.4", result)
            self.assertIn("PARTICLE_OUTPUT_BLEND_MODE_ALPHA", result)
            self.assertEqual(flame.read_text(), original.replace("smoke1", "muzzleflash"))
            self.assertEqual(glow.read_text(), additive)
            self.assertEqual(impact.read_text(), original)
            self.assertEqual(grenade.read_text(), original)
            self.assertEqual(mixed_glow.read_text(), original.replace("smoke1", "muzzleflash"))
            self.assertEqual(light_weapon_smoke(root), [])


if __name__ == "__main__":
    unittest.main()
