#include "WeaponRecoil.h"
#include "WeaponRecoilMath.h"
#include "CameraShakeStack.h" // CameraShake_ShakeVisible: can this kick be seen?

#include "CameraShakeHost.h" // ShakeHost_EnsureWeaponChannel: the transform channel
#include "WeaponPivot.h" // the shared hinge -- recoil and bob rotate about ONE point
#include "RecoilPivotMarker.h" // the card's orb: part of whether its Reset may dim
#include "../Debug/MvmDebugLog.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../../../Shared/AfxConsole.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace Filmmaker {

namespace {

double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// One tuning as JSON fields, and back. Presets and the saved live state share these, so the two
// can never disagree about a field's name or range.
void WriteTuningFields(JsonBuilder& b, const RecoilTuning& t) {
	b.DoubleField("amount", t.amount);
	b.DoubleField("punch", t.punch);
	b.DoubleField("rotation", t.rotation);
	b.DoubleField("cameraRot", t.cameraRot);
	b.DoubleField("cameraPos", t.cameraPos);
	b.DoubleField("cameraRoll", t.cameraRoll);
	b.DoubleField("snap", t.snap);
	b.DoubleField("recovery", t.recovery);
	b.DoubleField("pivot", t.pivot);
}

RecoilTuning ReadTuningFields(const JsonValue& value) {
	auto number = [&](const char* key, double fallback, double lo, double hi) {
		const JsonValue* n = value.Find(key);
		return Clamp(n && n->type == JsonValue::Type::Number ? n->number : fallback, lo, hi);
	};
	RecoilTuning t;
	t.amount = number("amount", 100, 0, 150);
	t.punch = number("punch", 100, 0, 150);
	t.rotation = number("rotation", 100, 0, 150);
	t.cameraRot = number("cameraRot", 20, 0, 100);
	t.cameraPos = number("cameraPos", 8, 0, 100);
	t.cameraRoll = number("cameraRoll", 20, 0, 100);
	t.snap = number("snap", 100, 25, 400);
	t.recovery = number("recovery", 100, 25, 400);
	t.pivot = number("pivot", 10, 0, 30);
	return t;
}
double ClampIntensity(double p) { return Clamp(p, 0.0, kShakeMaxIntensity); }

// ---- reference tuning (100% on every slider) ----------------------------------------------
//
// The six channels and their constants now live in WeaponRecoilStyles.h, which holds one profile
// per STYLE (MW2019 default, MW2022 = the original ARC9-derived port). The integrator below is
// style-agnostic: it reads whichever profile is selected and is otherwise unchanged.
//
// The spring model follows ARC9's visual recoil (github.com/HaodongMo/ARC-9,
// lua/weapons/arc9_base/sh_recoil.lua -- the GMod MW-style base):
//
//   accel = -x*|x|*kQuad - sign(x)*kMag - v*damp - v*|v|*kDrag
//
// A QUADRATIC spring (hard yank when far, gentle near rest) plus a small constant-magnitude
// pull that kills the asymptotic tail (crisp settle) plus linear + quadratic drag.
//
// Per shot each channel gets an INSTANT offset add (the punch is visible the same frame --
// the single biggest ingredient of the MW snap; a velocity-only impulse ramps to its peak
// tens of ms later and reads soft) plus a velocity add for carry-through and overshoot.
// Both are scaled by (1 - |offset|/cap): bounded accumulation, mirroring ARC9's
// Clamp(rec + RecoilPerShot, 0, RecoilMax) -- sustained auto fire converges to a rhythmic
// bounce instead of stacking into mush.
//
// The STAGGER across channels is the design: the composite motion reads organic without any
// random screenshake.

// Quadratic velocity drag, shared by all channels (ARC9's 0.5*v*|v| term): brakes fast
// motion hard, leaves the slow settle alone.
// The shared spring step in WeaponRecoilMath.h owns the drag coefficient.

// Fixed integration substep. The quadratic spring at full cap displacement and snap 400%
// produces accelerations ~10^4 units/s^2; 1 ms steps keep semi-implicit Euler comfortably
// stable there. A fixed step -- not "per render frame" -- is also what makes the spring
// trajectory identical between a 60 fps preview and a 300 fps export of the same range.
constexpr double kSubStep = 0.001;

// Springs below these magnitudes with negligible velocity count as settled (used to disarm
// the paused-demo wall clock).
constexpr double kSettleOffset = 0.01;
constexpr double kSettleVel = 0.5;

// The per-weapon multiplier table lives in WeaponRecoilScope.h now: the scope model needs the
// same rows to answer "is this a rifle", and one table cannot drift from itself.
} // namespace

WeaponRecoil& WeaponRecoilRef() {
	static WeaponRecoil s_instance;
	return s_instance;
}

// ============================== settings ===========================================

void WeaponRecoil::SetEnabled(bool on) {
	if (on == m_enabled) return;
	m_enabled = on;
	Reset();
	// The transform rides on the CalcViewModelView input detour; install it up front so the
	// first shot does not race the lazy install. A pattern miss warns inside and the feature
	// degrades to nothing rather than crashing.
	const char* whyNot = "";
	if (on && !ShakeHost_EnsureWeaponChannel(&whyNot))
		advancedfx::Warning(
			"mirv_filmmaker: weapon recoil cannot move the gun: %s. Its camera share still kicks the view.\n", whyNot);
}

// Every knob writes the SELECTED scope. EditTuning() materialises its override on this first
// write, seeded with what it was already inheriting, so the change you make is the only change.
void WeaponRecoil::SetAmount(double p) { EditTuning().amount = ClampIntensity(p); AfterEdit(); }
void WeaponRecoil::SetPunch(double p) { EditTuning().punch = ClampIntensity(p); AfterEdit(); }
void WeaponRecoil::SetRotation(double p) { EditTuning().rotation = ClampIntensity(p); AfterEdit(); }
void WeaponRecoil::SetCameraRot(double p) { EditTuning().cameraRot = Clamp(p, 0.0, 100.0); AfterEdit(); }
void WeaponRecoil::SetCameraPos(double p) { EditTuning().cameraPos = Clamp(p, 0.0, 100.0); AfterEdit(); }
void WeaponRecoil::SetCameraRoll(double p) { EditTuning().cameraRoll = Clamp(p, 0.0, 100.0); AfterEdit(); }
void WeaponRecoil::SetSnap(double p) { EditTuning().snap = Clamp(p, 25.0, 400.0); AfterEdit(); }
void WeaponRecoil::SetRecovery(double p) { EditTuning().recovery = Clamp(p, 25.0, 400.0); AfterEdit(); }
void WeaponRecoil::SetPivot(double u) {
	EditTuning().pivot = Clamp(u, 0.0, 30.0);
	// Keep the shared store in step: the slider has always meant "how far out does the
	// gun hinge", and that question now has one owner.
	WeaponPivotRecoilRef().SetAllForward(Clamp(u, 0.0, 30.0));
	AfterEdit();
}

// Style swaps the channel table under the springs. Clear the live springs with it: a spring
// mid-flight under MW2022 constants would keep integrating against MW2019 ones and settle wrong.
void WeaponRecoil::SetStyle(RecoilStyle style) {
	if (style == m_style) return;
	m_style = style;
	Reset();
}

RecoilTuning& WeaponRecoil::EditTuning() {
	if (m_editScope == RecoilScope::All || m_editKey.empty())
		return m_all;
	if (RecoilOverride* existing = RecoilFindOverride(m_overrides, m_editScope, m_editKey))
		return existing->tuning;
	RecoilOverride ov;
	ov.scope = m_editScope;
	ov.key = RecoilLower(m_editKey);
	// Seed from what this scope currently inherits (m_editView), NOT from the factory defaults:
	// creating an override must not silently change every other knob on that weapon.
	ov.tuning = m_editView;
	m_overrides.push_back(ov);
	return m_overrides.back().tuning;
}

void WeaponRecoil::AfterEdit() {
	if (m_editScope == RecoilScope::All || m_editKey.empty()) {
		m_editView = m_all;
	} else if (const RecoilOverride* ov = RecoilFindOverride(m_overrides, m_editScope, m_editKey)) {
		m_editView = ov->tuning;
	} else {
		m_editView = RecoilInheritedTuning(m_all, m_overrides, m_editScope, m_editKey);
	}
	// The springs keep running with the gun that last fired; re-resolve so an edit to the scope
	// covering it takes effect immediately instead of at the next shot.
	m_active = RecoilResolve(m_all, m_overrides, m_lastWeapon);
}

void WeaponRecoil::SetEditScope(RecoilScope scope, const char* key) {
	m_editScope = scope;
	m_editKey = (scope == RecoilScope::All || !key) ? std::string() : RecoilLower(key);
	AfterEdit();
}

bool WeaponRecoil::ClearEditOverride() {
	if (m_editScope == RecoilScope::All || m_editKey.empty()) return false;
	for (size_t i = 0; i < m_overrides.size(); ++i) {
		if (m_overrides[i].scope != m_editScope || m_overrides[i].key != m_editKey) continue;
		m_overrides.erase(m_overrides.begin() + i);
		AfterEdit();
		return true;
	}
	return false;
}

void WeaponRecoil::ClearOverrides() {
	m_overrides.clear();
	AfterEdit();
}

bool WeaponRecoil::SavePreset(const char* name) {
	WeaponRecoilPreset preset;
	preset.name = name ? name : "";
	preset.scope = m_editScope;
	preset.key = m_editKey;
	preset.style = m_style;
	preset.tuning = m_editView;
	return m_presets.Save(preset);
}

bool WeaponRecoil::ApplyPreset(const char* name) {
	const WeaponRecoilPreset* found = m_presets.Find(name);
	if (!found) return false;
	// Copy before editing: EditTuning can grow an override vector, while the preset book remains
	// stable today. Keeping the operation value-based also makes that ownership explicit.
	const WeaponRecoilPreset preset = *found;
	SetEditScope(preset.scope, preset.key.c_str());
	SetStyle(preset.style);
	EditTuning() = preset.tuning;
	AfterEdit();
	return true;
}

bool WeaponRecoil::DeletePreset(const char* name) {
	return m_presets.Erase(name);
}

void WeaponRecoil::WritePresets(JsonBuilder& b) const {
	b.Key("weaponRecoilPresets"); b.BeginArray();
	for (const auto& preset : m_presets.Items()) {
		b.BeginObject();
		b.StringField("name", preset.name);
		b.StringField("scope", RecoilScopeName(preset.scope));
		b.StringField("key", preset.key);
		b.StringField("style", RecoilStyleName(preset.style));
		WriteTuningFields(b, preset.tuning);
		b.EndObject();
	}
	b.EndArray();
}

void WeaponRecoil::WriteState(JsonBuilder& b) const {
	b.Key("weaponRecoil"); b.BeginObject();
	b.BoolField("enabled", m_enabled);
	b.StringField("style", RecoilStyleName(m_style));
	b.Key("all"); b.BeginObject(); WriteTuningFields(b, m_all); b.EndObject();
	b.Key("overrides"); b.BeginArray();
	for (const RecoilOverride& ov : m_overrides) {
		b.BeginObject();
		b.StringField("scope", RecoilScopeName(ov.scope));
		b.StringField("key", ov.key);
		WriteTuningFields(b, ov.tuning);
		b.EndObject();
	}
	b.EndArray();
	b.EndObject();
}

void WeaponRecoil::LoadState(const JsonValue* object) {
	if (!object || object->type != JsonValue::Type::Object) return;
	if (const JsonValue* style = object->Find("style"); style && style->type == JsonValue::Type::String) {
		RecoilStyle s = m_style;
		if (RecoilStyleParse(style->str.c_str(), s)) SetStyle(s);
	}
	if (const JsonValue* all = object->Find("all"); all && all->type == JsonValue::Type::Object)
		m_all = ReadTuningFields(*all);
	if (const JsonValue* list = object->Find("overrides"); list && list->type == JsonValue::Type::Array) {
		m_overrides.clear();
		for (const JsonValue& value : list->arr) {
			if (value.type != JsonValue::Type::Object) continue;
			const JsonValue* scopeText = value.Find("scope");
			RecoilOverride ov;
			if (!scopeText || scopeText->type != JsonValue::Type::String || !RecoilParseScope(scopeText->str, ov.scope)
				|| ov.scope == RecoilScope::All) continue;
			ov.key = value.Find("key") ? value.Find("key")->AsString() : std::string();
			// The same validity rule presets use: an override for a scope no weapon resolves to is dropped.
			if (RecoilRepresentativeWeapon(ov.scope, ov.key).empty()) continue;
			ov.tuning = ReadTuningFields(value);
			m_overrides.push_back(ov);
		}
	}
	m_editScope = RecoilScope::All;
	m_editKey.clear();
	AfterEdit();
	if (const JsonValue* on = object->Find("enabled"); on && on->type == JsonValue::Type::Bool) SetEnabled(on->boolean);
}

void WeaponRecoil::LoadPresets(const JsonValue* array) {
	m_presets.Clear();
	if (!array || array->type != JsonValue::Type::Array) return;
	for (const JsonValue& value : array->arr) {
		if (value.type != JsonValue::Type::Object) continue;
		const JsonValue* name = value.Find("name");
		const JsonValue* scopeText = value.Find("scope");
		if (!name || name->type != JsonValue::Type::String
			|| !scopeText || scopeText->type != JsonValue::Type::String) continue;
		WeaponRecoilPreset preset;
		preset.name = name->str;
		if (!RecoilParseScope(scopeText->str, preset.scope)) continue;
		preset.key = value.Find("key") ? value.Find("key")->AsString() : std::string();
		if (preset.scope != RecoilScope::All
			&& RecoilRepresentativeWeapon(preset.scope, preset.key).empty()) continue;
		if (const JsonValue* style = value.Find("style"); style && style->type == JsonValue::Type::String)
			RecoilStyleParse(style->str.c_str(), preset.style);
		preset.tuning = ReadTuningFields(value);
		m_presets.Save(preset);
	}
}

std::string WeaponRecoil::LiveWeapon() const {
	std::string live;
	if (CameraShake_SpectatedWeapon(live)) {
		// Reduce the entity classname ("weapon_ak47") to the table needle ("ak47"): it is what
		// the scope tests and the card want, and it is a literal from our own table, so nothing
		// engine-supplied is ever echoed into the state JSON unescaped.
		if (const RecoilWeaponEntry* e = RecoilFindWeapon(live)) {
			m_liveWeapon = e->needle;
			return m_liveWeapon;
		}
	}
	// Nothing readable this frame. The spectated-POV reading is genuinely absent while the CONFIG
	// panel is open -- the one screen this is read from -- so hold the last one that resolved
	// rather than reporting "no gun" every other frame.
	if (!m_liveWeapon.empty()) return m_liveWeapon;
	if (const RecoilWeaponEntry* e = RecoilFindWeapon(m_lastWeapon)) return e->needle;
	return std::string();
}

// Preference order: the gun actually in shot when the selected scope covers it (so a test looks
// like what you are watching), then that scope's representative weapon, then whatever is in
// shot, then a rifle.
//
// Until 2026-09-01 this was a hardcoded "ak47", and that is why Slot and Type appeared not to
// gate: the card said the override existed, the sliders moved, and then Test Shot fired a RIFLE
// -- which a "pistols" override does not cover. Only "All weapons" ever looked like it worked,
// because All covers everything.
std::string WeaponRecoil::TestWeapon() const {
	const std::string live = LiveWeapon();
	if (m_editScope != RecoilScope::All && !m_editKey.empty()) {
		RecoilOverride probe;
		probe.scope = m_editScope;
		probe.key = m_editKey;
		if (!live.empty() && RecoilOverrideMatches(probe, live)) return live;
		const std::string rep = RecoilRepresentativeWeapon(m_editScope, m_editKey);
		if (!rep.empty()) return rep;
	}
	if (!live.empty()) return live;
	return "ak47";
}

double WeaponRecoil::Rand01() {
	if (m_rngState == 0) m_rngState = 0x9E3779B97F4A7C15ull;
	m_rngState ^= m_rngState >> 12;
	m_rngState ^= m_rngState << 25;
	m_rngState ^= m_rngState >> 27;
	const unsigned long long v = m_rngState * 0x2545F4914F6CDD1Dull;
	return (double)(v >> 11) * (1.0 / 9007199254740992.0);
}

void WeaponRecoil::OnFire(const char* weapon) {
	// Producer side of the SPSC ring: fill the slot, then publish it. Only the token is copied;
	const unsigned w = m_pendingWrite.load(std::memory_order_relaxed);
	PendingShot& slot = m_pending[w % kPendingRing];
	slot.weapon[0] = 0;
	if (weapon && *weapon) {
		size_t n = 0;
		while (n + 1 < sizeof(slot.weapon) && weapon[n]) { slot.weapon[n] = weapon[n]; ++n; }
		slot.weapon[n] = 0;
	}
	m_pendingWrite.store(w + 1, std::memory_order_release);
}

void WeaponRecoil::TestFire(const char* weapon) {
	m_testClock = true;
	OnFire(weapon);
}

// ============================== per-frame ==========================================

ShakePose WeaponRecoil::Advance(const ShakeInput& in) {
	ShakePose out;
	if (!m_enabled) {
		m_pendingRead.store(m_pendingWrite.load(std::memory_order_acquire), std::memory_order_relaxed);
		return out;
	}

	if (in.fresh) {
		// Seek / spectator switch / gap: drop the springs, keep the settings. Pending shots
		// from before the jump belong to the old timeline.
		const unsigned long long shots = m_dbgShots;
		const unsigned long long resets = m_dbgResets;
		Reset();
		m_dbgShots = shots;
		m_dbgResets = resets + 1;
		return out;
	}

	// Consume queued shots. Each shot lands as an INSTANT offset (visible this frame -- the
	// MW snap) plus a velocity kick (carry-through/overshoot), both scaled down as the
	// channel approaches its accumulation cap so auto fire converges to a bounded rhythmic
	// bounce (ARC9's Clamp(rec + perShot, 0, RecoilMax)).
	// Each shot is scaled by ITS OWN weapon's tuning, resolved here rather than at fire time so
	// the override vector is only ever touched on this thread. A spray that spans a weapon
	// switch therefore kicks correctly on both guns.
	int fired = 0;
	unsigned readIndex = m_pendingRead.load(std::memory_order_relaxed);
	const unsigned writeIndex = m_pendingWrite.load(std::memory_order_acquire);
	for (; readIndex != writeIndex; ++readIndex) {
		const std::string shotWeapon = m_pending[readIndex % kPendingRing].weapon;
		const RecoilTuning& tuning = RecoilResolve(m_all, m_overrides, shotWeapon);
		// The springs are shared, so the integration constants can only follow the latest shot.
		m_active = tuning;
		m_lastWeapon = shotWeapon;
		const double amount = tuning.amount / 100.0;
		const double punch = tuning.punch / 100.0;
		const double rotation = tuning.rotation / 100.0;
		++fired;
		++m_dbgShots;
		const RecoilStyleProfile& profile = RecoilStyleProfileFor(m_style);
		const double classMult = RecoilWeaponMultiplier(shotWeapon);
		if (Rand01() < profile.flipChance) m_sideSign = -m_sideSign;
		// Magnitude jitter on the SIGNED channels only. The main channels (back, up, pitch) stay
		// exact so the core punch is repeatable shot to shot; MW2019 also runs a much tighter
		// jitter than MW2022, which is part of why a burst reads as one motion.
		const double jLo = 1.0 - profile.jitter, jSpan = 2.0 * profile.jitter;
		const double jSide = jLo + jSpan * Rand01();
		const double jYaw = jLo + jSpan * Rand01();
		const double jRoll = jLo + jSpan * Rand01();
		// Roll sign: MW2019 takes an independent coin flip every shot (ARC9's
		// `VisualRecoilRoll * SharedRandom(-1, 1)`), so a burst reads as the gun torquing in the
		// hands. MW2022 instead cants roll AWAY from the side shift deterministically -- the gun
		// leans off the side it was pushed toward.
		const double rollSign = profile.rollRandomSign
			? (Rand01() < 0.5 ? -1.0 : 1.0)
			: -m_sideSign;
		const double chanScale[6] = {
			punch, punch, punch * m_sideSign * jSide,
			rotation, rotation * m_sideSign * jYaw,
			rotation * rollSign * jRoll,
		};
		for (int c = 0; c < 6; ++c) {
			Spring& s = m_spring[c];
			const double capScaled = profile.channel[c].cap * (amount > 1.0 ? amount : 1.0);
			// Diminishing returns toward the cap; never below 15% so a long spray still ticks.
			double room = 1.0 - std::fabs(s.offset) / capScaled;
			if (room < 0.15) room = 0.15;
			const double k = amount * classMult * chanScale[c] * room;
			s.offset += profile.channel[c].instant * k;
			s.vel += profile.channel[c].kick * k;
		}
	}
	// Consumer side of the ring: release the slots only once they are fully consumed.
	m_pendingRead.store(readIndex, std::memory_order_relaxed);

	// Integration clock: demo time. dt == 0 is the same-tick hold (paused demo or several
	// render frames per tick) and normally freezes the springs -- EXCEPT while the paused-demo
	// test clock is armed, which substitutes wall time so a `test` kick can be tuned on a
	// still frame. Real shots only ever arrive with demo time advancing, so exports are
	// untouched by the wall-clock path.
	double stepDt = in.dt;
	if (stepDt > 0.0) {
		m_testClock = false; // demo clock is live again; it owns the springs from here
	} else if (m_testClock) {
		stepDt = in.wallDt;
		bool settled = true;
		for (const Spring& s : m_spring)
			if (std::fabs(s.offset) > kSettleOffset || std::fabs(s.vel) > kSettleVel) {
				settled = false;
				break;
			}
		if (settled && PendingCount() <= 0) {
			m_testClock = false;
			stepDt = 0.0;
		}
	}

	// Fixed-substep semi-implicit Euler over ARC9's return force:
	//   accel = -x*|x|*kQuad - sign(x)*kMag - v*damp - v*|v|*kDrag
	if (stepDt > 0.0) {
		const RecoilStyleProfile& profile = RecoilStyleProfileFor(m_style);
		const double snap = m_active.snap / 100.0;
		const double recovery = m_active.recovery / 100.0;
		double remaining = stepDt;
		while (remaining > 0.0) {
			const double h = remaining > kSubStep ? kSubStep : remaining;
			remaining -= h;
			for (int c = 0; c < 6; ++c) {
				Spring& s = m_spring[c];
				RecoilSpringStep(s.offset, s.vel, profile.channel[c], snap, recovery, h);
			}
		}
	}

	// Camera share: the same converted engine-convention values the weapon gets, scaled down.
	// Using one conversion for both guarantees camera and gun always kick the same direction.
	// Three independent shares. Position, aim rotation and barrel roll reach the camera on their
	// own dials, so a firm muzzle rise on screen no longer drags the roll along with it.
	const double rotShare = m_active.cameraRot / 100.0;
	const double posShare = m_active.cameraPos / 100.0;
	const double rollShare = m_active.cameraRoll / 100.0;
	out.fwd = -m_spring[0].offset * posShare;
	out.up = m_spring[1].offset * posShare;
	out.right = m_spring[2].offset * posShare;
	out.pitch = -m_spring[3].offset * rotShare; // muzzle up = engine pitch negative
	out.yaw = -m_spring[4].offset * rotShare;   // kick right = engine yaw negative
	out.roll = m_spring[5].offset * rollShare;

	if (MvmDebugLog_Active()) {
		static unsigned s_throttle = 0;
		const bool active = std::fabs(m_spring[0].offset) > 0.001
			|| std::fabs(m_spring[3].offset) > 0.001 || fired > 0;
		// Logged AFTER the shares are resolved, and reporting them, because "the gun kicks but
		// the camera does not" is the report this probe exists to answer -- the spring columns
		// alone cannot distinguish a dead camera share from a small one. camPitch/camRoll are
		// DEGREES actually added to the view this frame; wpn=<gun> is the tuning that resolved,
		// so a test shot and a live shot can be compared without guessing which weapon each used.
		if (active && (fired > 0 || (s_throttle++ % 15) == 0))
			MvmDebugLog_Linef("weaponrecoil.springs",
				"fired=%d wpn=%s back=%.3f up=%.3f side=%.3f mpitch=%.3f yaw=%.3f roll=%.3f "
				"vBack=%.1f vPitch=%.1f dt=%.4f sideSign=%.0f "
				"share=[rot=%.0f pos=%.0f roll=%.0f] camPitch=%.3f camYaw=%.3f camRoll=%.3f "
				"camFwd=%.3f amount=%.0f rot=%.0f",
				fired, m_lastWeapon.empty() ? "-" : m_lastWeapon.c_str(),
				m_spring[0].offset, m_spring[1].offset, m_spring[2].offset,
				m_spring[3].offset, m_spring[4].offset, m_spring[5].offset,
				m_spring[0].vel, m_spring[3].vel, in.dt, m_sideSign,
				m_active.cameraRot, m_active.cameraPos, m_active.cameraRoll,
				out.pitch, out.yaw, out.roll, out.fwd,
				m_active.amount, m_active.rotation);
	}

	return out;
}

void WeaponRecoil::WeaponPose(double& fwd, double& right, double& up,
	double& pitch, double& yaw, double& roll) const {
	fwd = right = up = pitch = yaw = roll = 0.0;
	if (!m_enabled) return;
	fwd = -m_spring[0].offset;   // back channel positive = toward the camera
	up = m_spring[1].offset;
	right = m_spring[2].offset;
	pitch = -m_spring[3].offset; // muzzle up = engine pitch negative
	yaw = -m_spring[4].offset;   // kick right = engine yaw negative
	roll = m_spring[5].offset;   // engine roll positive = clockwise lean

	// Virtual rotation pivot (ARC9's VisualRecoilCenter). The hook rotates the viewmodel about
	// the CAMERA, which sweeps the whole gun across the screen; translating against the rotation
	// moves the effective hinge onto the weapon, so the muzzle climbs while the grip stays put.
	//
	// The hinge is a 3D POINT, not a forward distance. It was forward-only until 2026-09-01,
	// which pinned it to the view axis -- the centre of the screen, nowhere near the gun. ARC9
	// puts it out at the hand (Vector(2, 4, 2), "where your hand is"); the ratios in
	// WeaponRecoilStyles.h place it right and below by a fraction of Pivot, which is where a CS2
	// viewmodel sits.
	//
	// Exact, not the old small-angle sine: the correction is (d - R*d), the residual translation
	// of the hinge point under this frame's rotation. For a forward-only d and small angles it
	// reduces to the previous d*sin(angle) form, so the existing feel is preserved where it was
	// already right.
	// The hinge is SHARED with Weapon Bob (WeaponPivot.h). Recoil's own Pivot slider still sets
	// the forward distance -- it writes the store's All base -- but a weapon class with its own
	// pivot (a knife hinges BEHIND the eye) wins here exactly as it does for bob. Two effects
	// rotating one gun about two different points is how a shot fired mid-stride stops reading as
	// a single object.
	// Weapon Recoil's OWN hinge; Weapon Bob keeps a separate one (see WeaponPivot.h).
	const WeaponPivotPoint sharedHinge = WeaponPivotRecoilRef().ResolveActive();
	if (m_active.pivot > 0.0) {
		constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
		const double dF = sharedHinge.fwd;
		const double dR = sharedHinge.right;
		const double dU = sharedHinge.up;

		const double cp = std::cos(pitch * kDegToRad), sp = std::sin(pitch * kDegToRad);
		const double cy = std::cos(yaw * kDegToRad), sy = std::sin(yaw * kDegToRad);
		const double cr = std::cos(roll * kDegToRad), sr = std::sin(roll * kDegToRad);

		// Camera-local axes: X = forward, Y = right, Z = up. Engine convention, so +pitch tips
		// the forward axis DOWN, +yaw swings it LEFT (toward -right) and +roll leans the up
		// axis toward the right (clockwise).
		//
		// ROLL BELONGS HERE. It was left out while the other two were compensated, so the
		// barrel roll alone rotated the gun about the EYE rather than about the hinge -- the
		// weapon sits right and below the view axis, so it swung along an arc around the centre
		// of the screen instead of twisting in place. Composed innermost, then pitch, then yaw,
		// which is the engine's own order for the angles this rides on.
		const double xr = dF;
		const double yr = dR * cr + dU * sr;
		const double zr = -dR * sr + dU * cr;
		const double xp = xr * cp + zr * sp;
		const double zp = -xr * sp + zr * cp;
		const double yp = yr;
		const double x2 = xp * cy + yp * sy;
		const double y2 = -xp * sy + yp * cy;
		const double z2 = zp;

		fwd += (dF - x2);
		right += (dR - y2);
		up += (dU - z2);
	}
}

void WeaponRecoil::ResetSettings() {
	// Mirrors the member initialisers in WeaponRecoil.h -- keep the two in step.
	m_enabled = false;
	m_style = RecoilStyle::Mw2019;
	m_all = RecoilTuning();
	// Reset clears the per-weapon work too. Leaving overrides behind would mean "reset to
	// defaults" produced a module that still behaves differently on eight weapons.
	m_overrides.clear();
	m_editScope = RecoilScope::All;
	m_editKey.clear();
	m_lastWeapon.clear();
	m_liveWeapon.clear();
	m_active = RecoilTuning();
	AfterEdit();
	Reset();
}

bool WeaponRecoil::SettingsAreDefault() const {
	const RecoilTuning defaults;
	auto eq = [](double a, double b) { return std::fabs(a - b) <= 0.000001; };
	// `m_all.pivot` below is this module's own pivot SCALAR, not the Forward/Right/Up hinge the
	// card also shows -- that lives in WeaponPivot, and the marker orb in RecoilPivotMarker.
	// Both are controls on this card, so both decide whether its Reset may dim.
	if (!WeaponPivotRecoilRef().IsDefault()) return false;
	if (RecoilPivotMarkerRef().Enabled(WeaponPivotOwner::Recoil)) return false;
	return !m_enabled && m_style == RecoilStyle::Mw2019
		&& m_overrides.empty() && m_editScope == RecoilScope::All
		&& m_editKey.empty() && eq(m_all.amount, defaults.amount)
		&& eq(m_all.punch, defaults.punch) && eq(m_all.rotation, defaults.rotation)
		&& eq(m_all.cameraRot, defaults.cameraRot) && eq(m_all.cameraPos, defaults.cameraPos)
		&& eq(m_all.cameraRoll, defaults.cameraRoll) && eq(m_all.snap, defaults.snap)
		&& eq(m_all.recovery, defaults.recovery) && eq(m_all.pivot, defaults.pivot);
}

void WeaponRecoil::Reset() {
	m_pendingRead.store(m_pendingWrite.load(std::memory_order_acquire), std::memory_order_relaxed);
	for (Spring& s : m_spring) s = Spring{};
	m_sideSign = 1.0;
	m_testClock = false;
	m_rngState = 0x9E3779B97F4A7C15ull;
	++m_dbgResets;
}

// ============================== reporting ==========================================

// Scope selection + the override inventory, so the CONFIG card can show which scope is being
// edited, whether it has its own values yet, and what else has been configured.
void WeaponRecoil::AppendScopeJson(std::ostringstream& o) const {
	bool hasOwn = false;
	for (const RecoilOverride& ov : m_overrides)
		if (ov.scope == m_editScope && ov.key == m_editKey) { hasOwn = true; break; }
	o << ",\"wrScope\":\"" << RecoilScopeName(m_editScope) << "\""
		<< ",\"wrScopeKey\":\"" << m_editKey << "\""
		// False means the knobs are showing INHERITED values that no override owns yet, which the
		// card needs in order to label them as such instead of implying this weapon is configured.
		<< ",\"wrScopeOwn\":" << (hasOwn ? "true" : "false")
		<< ",\"wrOverrides\":[";
	for (size_t i = 0; i < m_overrides.size(); ++i) {
		if (i) o << ",";
		o << "{\"scope\":\"" << RecoilScopeName(m_overrides[i].scope) << "\""
			<< ",\"key\":\"" << m_overrides[i].key << "\"}";
	}
	o << "]";

	// WHAT THE SELECTED SCOPE ACTUALLY REACHES.
	//
	// An override that covers nothing currently in shot behaves exactly like a broken one: the
	// sliders move, the card says the scope owns its values, and no shot ever uses them. So the
	// gun in play and whether this scope covers it are reported, and the card says so in words.
	const std::string live = LiveWeapon();
	const RecoilWeaponType liveType = RecoilWeaponTypeOf(live);
	bool covers = true;
	if (m_editScope != RecoilScope::All && !m_editKey.empty()) {
		RecoilOverride probe;
		probe.scope = m_editScope;
		probe.key = m_editKey;
		covers = RecoilOverrideMatches(probe, live);
	}
	o << ",\"wrLive\":\"" << live << "\""
		<< ",\"wrLiveType\":\"" << RecoilWeaponTypeName(liveType) << "\""
		<< ",\"wrLiveSlot\":\"" << RecoilSlotName(RecoilSlotOf(liveType)) << "\""
		<< ",\"wrScopeCovers\":" << (covers ? "true" : "false");
}

void WeaponRecoil::AppendStateJson(std::ostringstream& o) const {
	o << ",\"wrOn\":" << (m_enabled ? "true" : "false")
		<< ",\"wrAmount\":" << (int)std::lround(m_editView.amount)
		<< ",\"wrPunch\":" << (int)std::lround(m_editView.punch)
		<< ",\"wrRot\":" << (int)std::lround(m_editView.rotation)
		<< ",\"wrCamRot\":" << (int)std::lround(m_editView.cameraRot)
		<< ",\"wrCamPos\":" << (int)std::lround(m_editView.cameraPos)
		<< ",\"wrCamRoll\":" << (int)std::lround(m_editView.cameraRoll)
		<< ",\"wrSnap\":" << (int)std::lround(m_editView.snap)
		<< ",\"wrRecovery\":" << (int)std::lround(m_editView.recovery)
		<< ",\"wrPivot\":" << (float)m_editView.pivot
		<< ",\"wrStyle\":\"" << RecoilStyleName(m_style) << "\"";
	AppendScopeJson(o);
	o << ",\"wrPresets\":[";
	bool firstPreset = true;
	for (const auto& preset : m_presets.Items()) {
		if (!firstPreset) o << ',';
		firstPreset = false;
		o << "{\"name\":\"" << JsonBuilder::Escape(preset.name)
			<< "\",\"scope\":\"" << RecoilScopeName(preset.scope)
			<< "\",\"key\":\"" << JsonBuilder::Escape(preset.key) << "\"}";
	}
	o << ']';
}

std::string WeaponRecoil::DebugStateJson() const {
	std::ostringstream o;
	o << "{\"on\":" << (m_enabled ? "true" : "false")
		<< ",\"amount\":" << (float)m_editView.amount
		<< ",\"punch\":" << (float)m_editView.punch
		<< ",\"rotation\":" << (float)m_editView.rotation
		<< ",\"cameraRot\":" << (float)m_editView.cameraRot
		<< ",\"cameraPos\":" << (float)m_editView.cameraPos
		<< ",\"cameraRoll\":" << (float)m_editView.cameraRoll
		<< ",\"snap\":" << (float)m_editView.snap
		<< ",\"recovery\":" << (float)m_editView.recovery
		<< ",\"pivot\":" << (float)m_editView.pivot
		<< ",\"style\":\"" << RecoilStyleName(m_style) << "\""
		<< ",\"scope\":\"" << RecoilScopeName(m_editScope) << "\""
		<< ",\"scopeKey\":\"" << m_editKey << "\""
		<< ",\"overrides\":" << (int)m_overrides.size()
		<< ",\"activeWeapon\":\"" << m_lastWeapon << "\""
		<< ",\"testClock\":" << (m_testClock ? "true" : "false")
		<< ",\"back\":" << (float)m_spring[0].offset
		<< ",\"up\":" << (float)m_spring[1].offset
		<< ",\"side\":" << (float)m_spring[2].offset
		<< ",\"mpitch\":" << (float)m_spring[3].offset
		<< ",\"yaw\":" << (float)m_spring[4].offset
		<< ",\"roll\":" << (float)m_spring[5].offset
		<< ",\"pending\":" << PendingCount()
		<< ",\"shots\":" << m_dbgShots
		<< ",\"resets\":" << m_dbgResets << "}";
	return o.str();
}

// ============================== console ============================================

namespace {

bool QuietArg(int argc, advancedfx::ICommandArgs* args, int index) {
	return argc >= index + 1 && 0 == _stricmp(args->ArgV(index), "quiet");
}

// `scope` takes an optional key, so "quiet" lands at a different index depending on which form
// was used. Scanning is the only way that does not silently print on one of them.
bool QuietAnywhere(int argc, advancedfx::ICommandArgs* args) {
	for (int i = 3; i < argc; ++i)
		if (0 == _stricmp(args->ArgV(i), "quiet")) return true;
	return false;
}

} // namespace

void WeaponRecoil_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	WeaponRecoil& wr = WeaponRecoilRef();
	const char* action = (argc >= 3) ? args->ArgV(2) : "";
	const char* value = (argc >= 4) ? args->ArgV(3) : "";

	if (0 == _stricmp(action, "on") || 0 == _stricmp(action, "off") || 0 == _stricmp(action, "toggle")) {
		const bool on = (0 == _stricmp(action, "toggle")) ? !wr.Enabled() : (0 == _stricmp(action, "on"));
		wr.SetEnabled(on);
		if (!QuietArg(argc, args, 3))
			advancedfx::Message("mirv_filmmaker: weapon recoil = %s.\n", on ? "on" : "off");
		return;
	}

	if (0 == _stricmp(action, "state")) {
		advancedfx::Message("[weaponrecoil][state] %s\n", wr.DebugStateJson().c_str());
		return;
	}

	if (0 == _stricmp(action, "reset")) {
		wr.ResetSettings();
		if (!QuietArg(argc, args, 3))
			advancedfx::Message("mirv_filmmaker: weapon recoil reset to defaults.\n");
		return;
	}

	if (0 == _stricmp(action, "test")) {
		// One kick on demand, with the gun the SELECTED SCOPE covers unless one is named --
		// otherwise a test can never exercise the override being edited. Works on a PAUSED demo
		// too: the test arms a wall-clock preview that runs the springs until they settle.
		const std::string testWeapon = (argc >= 4 && *value) ? std::string(value) : wr.TestWeapon();
		wr.TestFire(testWeapon.c_str());
		// Say whether it can actually be SEEN. The spring runs regardless, but in free cam or on a
		// baked path CameraShake_Apply bails out and publishes zero, so the old unconditional
		// "test shot fired" was true and useless -- it is the reason tuning these sliders from the
		// camera editor looks like the feature is broken.
		// The master toggle is the likeliest reason a test does nothing, and it is invisible from
		// the sliders: Advance() returns immediately while disabled, so the spring never runs.
		// Weapon Recoil also ships OFF and is deliberately NOT persisted by the settings store,
		// so it is off again every session however it was left.
		if (!wr.Enabled()) {
			advancedfx::Warning("mirv_filmmaker: weapon recoil is OFF -- the test did nothing."
				" Turn it on (WEAPON RECOIL toggle, or `%s weaponrecoil on`); the sliders do not"
				" enable it.\n", cmd);
			return;
		}
		const char* why = "";
		if (!CameraShake_ShakeVisible(&why))
			advancedfx::Warning("mirv_filmmaker: weapon recoil test fired, but it will NOT be"
				" visible: %s.\n", why);
		else
			// Name the gun and the scope its tuning came from. "The slider does nothing" is
			// almost always a test that fired something the edited scope does not cover.
			advancedfx::Message("mirv_filmmaker: weapon recoil test shot fired (%s, %s tuning).\n",
				testWeapon.c_str(), RecoilScopeName(wr.EditScope()));
		return;
	}

	// ---- scope ----------------------------------------------------------------------------
	// Repoints every knob above at a narrower set of weapons. The knobs themselves do not change;
	// this is the only new control the feature needs.
	if (0 == _stricmp(action, "scope")) {
		RecoilScope scope = RecoilScope::All;
		if (*value && !RecoilParseScope(value, scope)) {
			advancedfx::Warning("mirv_filmmaker: unknown recoil scope '%s' (all|slot|type).\n",
				value);
			return;
		}
		if (*value) {
			const char* key = (argc >= 5) ? args->ArgV(4) : "";
			if (scope != RecoilScope::All && !*key) {
				advancedfx::Warning(
					"mirv_filmmaker: scope %s needs a key (slot: primary|secondary, "
					"type: pistol|smg|rifle|shotgun|sniper|lmg).\n",
					RecoilScopeName(scope));
				return;
			}
			wr.SetEditScope(scope, key);
		}
		if (QuietAnywhere(argc, args)) return;
		if (wr.EditScope() == RecoilScope::All)
			advancedfx::Message("mirv_filmmaker: weapon recoil scope = all weapons.\n");
		else
			advancedfx::Message("mirv_filmmaker: weapon recoil scope = %s '%s'.\n",
				RecoilScopeName(wr.EditScope()), wr.EditKey().c_str());
		return;
	}

	if (0 == _stricmp(action, "scopeclear")) {
		// Clearing an override makes that scope INHERIT again -- it does not zero its values.
		if (0 == _stricmp(value, "all")) {
			const int had = wr.OverrideCount();
			wr.ClearOverrides();
			advancedfx::Message("mirv_filmmaker: cleared %d weapon recoil override(s).\n", had);
			return;
		}
		if (wr.ClearEditOverride())
			advancedfx::Message(
				"mirv_filmmaker: weapon recoil override cleared; %s '%s' inherits again.\n",
				RecoilScopeName(wr.EditScope()), wr.EditKey().c_str());
		else
			advancedfx::Message("mirv_filmmaker: nothing to clear for the selected scope.\n");
		return;
	}

	if (0 == _stricmp(action, "preset")) {
		const char* operation = value;
		const char* name = argc >= 5 ? args->ArgV(4) : "";
		bool ok = false;
		if (0 == _stricmp(operation, "save")) ok = wr.SavePreset(name);
		else if (0 == _stricmp(operation, "load")) ok = wr.ApplyPreset(name);
		else if (0 == _stricmp(operation, "delete")) ok = wr.DeletePreset(name);
		else {
			advancedfx::Warning("mirv_filmmaker: use weaponrecoil preset save|load|delete <name>.\n");
			return;
		}
		if (!ok) {
			advancedfx::Warning("mirv_filmmaker: could not %s recoil preset '%s'. Names use 1-40 "
				"letters, numbers, spaces, '.', '-' or '_'.\n", operation, name);
			return;
		}
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("weaponrecoil.preset",
				"operation=%s name='%s' scope=%s key='%s' amount=%.1f style=%s count=%d",
				operation, name, RecoilScopeName(wr.EditScope()), wr.EditKey().c_str(),
				wr.Amount(), RecoilStyleName(wr.Style()), (int)wr.Presets().size());
		advancedfx::Message("mirv_filmmaker: recoil preset '%s' %s%s.\n", name,
			0 == _stricmp(operation, "save") ? "saved with its " :
			0 == _stricmp(operation, "load") ? "loaded with its " : "deleted",
			0 == _stricmp(operation, "delete") ? "" : RecoilScopeName(wr.EditScope()));
		return;
	}

	if (0 == _stricmp(action, "style")) {
		RecoilStyle style = wr.Style();
		if (*value && !RecoilStyleParse(value, style)) {
			advancedfx::Warning("mirv_filmmaker: unknown recoil style \"%s\". Use style1 or style2.\n",
				value);
			return;
		}
		wr.SetStyle(style);
		if (!QuietArg(argc, args, 4))
			advancedfx::Message("mirv_filmmaker: weapon recoil style = %s.\n",
				RecoilStyleName(wr.Style()));
		return;
	}

	if (0 == _stricmp(action, "pivot")) {
		double v = wr.Pivot();
		if (*value) v = std::atof(value);
		wr.SetPivot(v);
		if (!QuietArg(argc, args, 4))
			advancedfx::Message("mirv_filmmaker: weapon recoil pivot = %.1f units.\n", wr.Pivot());
		return;
	}

	struct Knob {
		const char* action;
		const char* label;
		void (WeaponRecoil::*set)(double);
		double (WeaponRecoil::*get)() const;
	};
	static const Knob kKnobs[] = {
		{ "amount",   "weapon recoil amount",         &WeaponRecoil::SetAmount,   &WeaponRecoil::Amount },
		{ "punch",    "weapon recoil punch",          &WeaponRecoil::SetPunch,    &WeaponRecoil::Punch },
		{ "rotation", "weapon recoil rotation",       &WeaponRecoil::SetRotation, &WeaponRecoil::Rotation },
		{ "camerarot",  "weapon recoil camera rotation share", &WeaponRecoil::SetCameraRot,  &WeaponRecoil::CameraRot },
		{ "camerapos",  "weapon recoil camera position share", &WeaponRecoil::SetCameraPos,  &WeaponRecoil::CameraPos },
		{ "cameraroll", "weapon recoil camera roll share",     &WeaponRecoil::SetCameraRoll, &WeaponRecoil::CameraRoll },
		{ "snap",     "weapon recoil snappiness",     &WeaponRecoil::SetSnap,     &WeaponRecoil::Snap },
		{ "recovery", "weapon recoil recovery",       &WeaponRecoil::SetRecovery, &WeaponRecoil::Recovery },
	};

	for (const Knob& k : kKnobs) {
		if (0 != _stricmp(action, k.action)) continue;
		double v = (wr.*(k.get))();
		if (*value) v = (0 == _stricmp(value, "off")) ? 0.0 : std::atof(value);
		(wr.*(k.set))(v);
		if (QuietArg(argc, args, 4)) return;
		const double applied = (wr.*(k.get))();
		if (applied <= 0.0) advancedfx::Message("mirv_filmmaker: %s = off.\n", k.label);
		else advancedfx::Message("mirv_filmmaker: %s = %d%%.\n", k.label, (int)std::lround(applied));
		return;
	}

	advancedfx::Message(
		"%s weaponrecoil on|off|toggle - procedural viewmodel recoil on the spectated player's shots.\n"
		"%s weaponrecoil amount [<0-150>|off] - master impulse strength.\n"
		"%s weaponrecoil punch [<0-150>|off] - positional punch (back/up/side).\n"
		"%s weaponrecoil rotation [<0-150>|off] - rotational kick (muzzle rise/yaw/barrel roll).\n"
		"%s weaponrecoil camerarot|camerapos|cameraroll [<0-100>|off] - share of the kick the camera\n"
		"    also receives, split into aim rotation, positional punch and barrel roll.\n"
		"%s weaponrecoil snap [<25-400>] - spring return strength (higher = harder snap back).\n"
		"%s weaponrecoil recovery [<25-400>] - damping; lower = looser, more overshoot.\n"
		"%s weaponrecoil style [style1|style2] - reference feel. style1 (default) is springy:\n"
		"    vertically dominant, heavier, punch-led. style2 is snappier and more chaotic.\n"
		"%s weaponrecoil pivot [<0-30>] - rotation pivot distance in units (0 = camera, ~10 = grip).\n"
		"%s weaponrecoil scope [all|slot|type] [<key>] - point the knobs above at a\n"
		"    narrower set of weapons (slot: primary|secondary, type: pistol|smg|rifle|shotgun|\n"
		"    sniper|lmg). Narrower wins: type > slot > all.\n"
		"%s weaponrecoil scopeclear [all] - drop the selected override (or every one) so it\n"
		"    inherits again.\n"
		"%s weaponrecoil preset save|load|delete <name> - manage named tuning snapshots; save\n"
		"    records the selected scope and load restores it before applying the tuning.\n"
		"%s weaponrecoil test [<weapon>] - fire one kick; works while the demo is paused.\n"
		"%s weaponrecoil state - current settings + live spring offsets.\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker
