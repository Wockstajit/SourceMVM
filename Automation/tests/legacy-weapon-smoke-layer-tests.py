#!/usr/bin/env python3
"""Static checks for layering old GitHub On-mode weapon smoke onto Insurgency flashes."""

from pathlib import Path
import importlib.util


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "FX" / "tools" / "layer-legacy-weapon-smoke.py"
spec = importlib.util.spec_from_file_location("legacy_weapon_smoke", MODULE_PATH)
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)

assert module.POVAREHOK_SMOKE_ALPHA_SCALE == 0.30
assert module.is_smoke_leaf("particles/filmmaker/povarehok/regular/weapons/cs_weapon_fx/ac_muzzle_pistol_smoke_a.vpcf")
assert module.is_smoke_leaf("weapon_muzzle_flash_smoke_small.vpcf")
assert not module.is_smoke_leaf("weapon_muzzle_flash_sparks5.vpcf")
assert not module.is_smoke_leaf("weapon_shell_eject_smoke_pistol3.vpcf")
assert not module.is_smoke_leaf("ac_muzzle_ar2_flame2.vpcf")
assert module.is_smoke_wrapper("ac_muzzle_pistol.vpcf")
assert module.is_smoke_wrapper("ac_muzzle_357.vpcf")
assert not module.is_smoke_wrapper("ac_muzzle_pistol_smoke_a.vpcf")
assert not module.is_smoke_wrapper("ac_muzzle_357_sparks.vpcf")

text = '''m_Children =
	[
		{
			m_ChildRef = resource:"particles/filmmaker/povarehok/regular/weapons/cs_weapon_fx/weapon_muzzle_flash_sparks5.vpcf"
		},
		{
			m_ChildRef = resource:"particles/filmmaker/povarehok/regular/weapons/cs_weapon_fx/ac_muzzle_pistol.vpcf"
		},
	]
'''
assert "ac_muzzle_pistol.vpcf" in "".join(module.child_resources(text))

converter = (ROOT / "FX/tools/convert-fx-source1.ps1").read_text(encoding="utf-8")
assert "layer-legacy-weapon-smoke.py" in converter
assert "Legacy weapon-smoke layer" in converter
assert "scale-povarehok-metal-sparks.py" in converter
assert "Povarehok weapon smoke" in converter
assert "m_hFallback" in MODULE_PATH.read_text(encoding="utf-8")

smoke = """
		{
			_class = "C_OP_InstantaneousEmitter"
			m_nParticlesToEmit =
			{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = 0
				m_flRandomMax = 1
			}
		},
		{
			_class = "C_OP_FadeAndKill"
			m_flStartFadeOutTime = 0.0
		},
		{
			_class = "C_OP_BasicMovement"
			m_fDrag = 0.1
		},
		{
			_class = "C_OP_RenderSprites"
			m_flStartFadeSize = 0.35
			m_flEndFadeSize = 0.4
		},
"""
tuned = module.tune_smoke_text(smoke)
assert "m_flRandomMin = 1" in tuned
assert "m_flStartFadeOutTime = 0.45" in tuned
assert "m_flStartFadeSize" not in tuned
assert "m_flEndFadeSize" not in tuned
assert "m_Gravity = [6.0, 4.0, 12.0]" in tuned
assert "C_OP_RandomForce" in tuned
assert module.DRIFT_MARKER in tuned
assert tuned == module.tune_smoke_text(tuned)

flash = 'm_hFallback = resource:"particles/filmmaker/insurgency/weapon_fx_ins/muzzleflash_m9_1p_b.vpcf"\n'
assert module.FALLBACK_RE.search(flash).group(1).endswith("muzzleflash_m9_1p_b.vpcf")

print(
    f"OK: smoke filter rejects flashes/sparks/shell-eject, "
    f"{len(module.CLASS_FLASHES)} class maps, {len(module.INSURGENCY_TARGETS)} Insurgency targets, "
    f"fallback attach + every-shot drift"
)

