#!/usr/bin/env python3
"""Static regression checks for the two-state hybrid impact configuration."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
rules = (ROOT / "AfxHookSource2/Filmmaker/Movie/ParticleFxRules.cpp").read_text(encoding="utf-8")
hook = (ROOT / "AfxHookSource2/Filmmaker/Movie/ParticleFxHook.cpp").read_text(encoding="utf-8")
ui = (ROOT / "AfxHookSource2/Filmmaker/Panorama/ConfigHudEffectsTabJs.h").read_text(encoding="utf-8")

assert "return false;" in rules[rules.index("case FxMode::Less:"):rules.index("case FxMode::Modern:")]
assert "'impacts', ['on', 'off']" in ui
assert "mode = FxMode::Less; // Impact On intentionally uses the reduced internal profile." in rules
assert "ImpactHybridTargetLower(low)" in hook
assert 'target.find("/impact_fx_ins/") != std::string::npos' in hook
assert "(sequence % 10u) >= kInsurgencySmokeTrailKeepPerTen" in hook
assert '"flying-trails-30pct"' in hook
assert '"flying-trails-10pct"' not in hook
internal = (ROOT / "AfxHookSource2/Filmmaker/Movie/ParticleFxInternal.h").read_text(encoding="utf-8")
assert "kInsurgencySmokeTrailKeepPerTen = 3u" in internal
assert '"impact-puff-preserved"' in hook

hybrid_rows = re.findall(r'FXRULE_IMPACT\("([^"]+)",\s*"([^"]+)",\s*"([^"]+)"\)', rules)
assert len(hybrid_rows) >= 40, len(hybrid_rows)
assert any(match.endswith("impact_wood.vpcf") and ins == pov == "impact_wood"
           for match, ins, pov in hybrid_rows)
assert 'FXRULE("particles/impact_fx/impact_sand.vpcf",' in rules
assert not any(match.endswith("impact_sand.vpcf") for match, _, _ in hybrid_rows)
print(f"OK: On/Off menu, reduced internal profile, and {len(hybrid_rows)} hybrid impact mappings")
