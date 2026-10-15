#include "WeaponPivot.h"

#include "WeaponRecoilScope.h" // RecoilWeaponTypeOf: one classifier for how a weapon FIRES
#include "../../../Shared/AfxConsole.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace Filmmaker {
namespace {

double Clamp(double v, double lo, double hi) {
	if (!(v > lo)) return lo;
	return v > hi ? hi : v;
}

bool Contains(const std::string& hay, const char* needle) {
	return hay.find(needle) != std::string::npos;
}

bool NearEq(double a, double b) {
	const double d = a - b;
	return (d < 0 ? -d : d) < 1e-9;
}

} // namespace

bool WeaponPivotClassFromName(const char* name, WeaponPivotClass& out) {
	if (!name || !*name) return false;
	for (int i = 0; i < kWeaponPivotClassCount; ++i) {
		if (0 == _stricmp(name, WeaponPivotClassName((WeaponPivotClass)i))) {
			out = (WeaponPivotClass)i;
			return true;
		}
	}
	return false;
}

WeaponPivotClass WeaponPivotClassOf(const std::string& weaponClassName) {
	if (weaponClassName.empty()) return WeaponPivotClass::Rifle;
	std::string w = weaponClassName;
	for (char& ch : w) ch = (char)std::tolower((unsigned char)ch);

	// Melee first, and by name: RecoilWeaponTypeOf has no knife member because it describes how a
	// weapon FIRES and a knife never does. The knife is the single most important pivot entry, so
	// it cannot depend on a classifier that structurally cannot return it.
	if (Contains(w, "knife") || Contains(w, "bayonet") || Contains(w, "melee"))
		return WeaponPivotClass::Knife;
	// Utility: carried, not aimed. Its own bucket rather than forced into a gun class.
	if (Contains(w, "grenade") || Contains(w, "molotov") || Contains(w, "flashbang")
		|| Contains(w, "decoy") || Contains(w, "smoke") || Contains(w, "c4")
		|| Contains(w, "taser") || Contains(w, "healthshot") || Contains(w, "breachcharge")
		|| Contains(w, "bump"))
		return WeaponPivotClass::Utility;

	switch (RecoilWeaponTypeOf(w)) {
	case RecoilWeaponType::Pistol:  return WeaponPivotClass::Pistol;
	case RecoilWeaponType::Smg:     return WeaponPivotClass::Smg;
	case RecoilWeaponType::Rifle:   return WeaponPivotClass::Rifle;
	case RecoilWeaponType::Shotgun: return WeaponPivotClass::Shotgun;
	case RecoilWeaponType::Sniper:  return WeaponPivotClass::Sniper;
	// An LMG is carried like a rifle, which is the only question this table answers.
	case RecoilWeaponType::Lmg:     return WeaponPivotClass::Rifle;
	// A GUN the table has not seen is still a gun. Defaulting it to the utility hinge gave a new
	// or renamed rifle a grenade's pivot; a rifle is the least-wrong guess for an unknown firearm.
	default:                        return WeaponPivotClass::Rifle;
	}
}

// ================================ the store =================================================

WeaponPivotStore::WeaponPivotStore() { ResetToDefaults(); m_editSerial = 0; }

void WeaponPivotStore::ResetToDefaults() {
	// ALL keeps Weapon Recoil's historical pivot (10 / 4 / -3.2). It is the value every gun used
	// before this store existed, so a setup that clears every override lands exactly where the
	// old build was rather than somewhere new.
	if (!IsDefault()) ++m_editSerial;
	m_all = WeaponPivotPoint{ 10.0, 4.0, -3.2 };
	for (int i = 0; i < kWeaponPivotClassCount; ++i) {
		m_class[i] = WeaponPivotDefaultFor((WeaponPivotClass)i);
		m_hasOverride[i] = true; // the measured per-class points ARE the shipped defaults
	}
}

bool WeaponPivotStore::IsDefault() const {
	const WeaponPivotPoint base{ 10.0, 4.0, -3.2 };
	if (!NearEq(m_all.fwd, base.fwd) || !NearEq(m_all.right, base.right)
		|| !NearEq(m_all.up, base.up)) return false;
	for (int i = 0; i < kWeaponPivotClassCount; ++i) {
		const WeaponPivotPoint d = WeaponPivotDefaultFor((WeaponPivotClass)i);
		if (!m_hasOverride[i]) return false;
		if (!NearEq(m_class[i].fwd, d.fwd) || !NearEq(m_class[i].right, d.right)
			|| !NearEq(m_class[i].up, d.up)) return false;
	}
	return true;
}

WeaponPivotPoint WeaponPivotStore::Resolve(WeaponPivotClass c) const {
	const int i = (int)c;
	if (i < 0 || i >= kWeaponPivotClassCount) return m_all;
	return m_hasOverride[i] ? m_class[i] : m_all;
}

bool WeaponPivotStore::HasOverride(WeaponPivotClass c) const {
	const int i = (int)c;
	return i >= 0 && i < kWeaponPivotClassCount && m_hasOverride[i];
}

// Every mutator below bumps m_editSerial ONLY when a stored number actually moves. Two reasons
// for the "only on a real change" part: the panel's sliders re-send the value they already hold
// on any re-sync, and restoring a saved config writes the values that are already there -- and
// neither is the operator reaching for the control, so neither should flash the marker.
void WeaponPivotStore::SetAll(const WeaponPivotPoint& p) {
	const WeaponPivotPoint n{ Clamp(p.fwd, -30.0, 30.0), Clamp(p.right, -30.0, 30.0),
		Clamp(p.up, -30.0, 30.0) };
	if (!NearEq(n.fwd, m_all.fwd) || !NearEq(n.right, m_all.right) || !NearEq(n.up, m_all.up))
		++m_editSerial;
	m_all = n;
}

void WeaponPivotStore::SetAllForward(double fwd) {
	const double n = Clamp(fwd, -30.0, 30.0);
	if (!NearEq(n, m_all.fwd)) ++m_editSerial;
	m_all.fwd = n;
}

void WeaponPivotStore::SetOverride(WeaponPivotClass c, const WeaponPivotPoint& p) {
	const int i = (int)c;
	if (i < 0 || i >= kWeaponPivotClassCount) return;
	const WeaponPivotPoint n{ Clamp(p.fwd, -30.0, 30.0), Clamp(p.right, -30.0, 30.0),
		Clamp(p.up, -30.0, 30.0) };
	if (!m_hasOverride[i] || !NearEq(n.fwd, m_class[i].fwd) || !NearEq(n.right, m_class[i].right)
		|| !NearEq(n.up, m_class[i].up)) ++m_editSerial;
	m_class[i] = n;
	m_hasOverride[i] = true;
}

void WeaponPivotStore::ClearOverride(WeaponPivotClass c) {
	const int i = (int)c;
	if (i < 0 || i >= kWeaponPivotClassCount) return;
	if (m_hasOverride[i]) ++m_editSerial;
	m_hasOverride[i] = false;
}

void WeaponPivotStore::ClearAllOverrides() {
	for (int i = 0; i < kWeaponPivotClassCount; ++i) {
		if (m_hasOverride[i]) ++m_editSerial;
		m_hasOverride[i] = false;
	}
}

void WeaponPivotStore::NoteActive(WeaponPivotClass c, const std::string& weaponClassName) {
	m_activeClass = c;
	if (!weaponClassName.empty()) m_activeWeapon = weaponClassName;
}

void WeaponPivotStore::AppendStateJson(std::ostringstream& o, const char* prefix) const {
	const WeaponPivotPoint p = ResolveActive();
	const std::string k = prefix ? prefix : "pv";
	o << ",\"" << k << "Class\":\"" << WeaponPivotClassName(m_activeClass) << "\""
		<< ",\"" << k << "Weapon\":\"" << m_activeWeapon << "\""
		<< ",\"" << k << "Own\":" << (HasOverride(m_activeClass) ? "true" : "false")
		<< ",\"" << k << "Fwd\":" << (float)p.fwd
		<< ",\"" << k << "Right\":" << (float)p.right
		<< ",\"" << k << "Up\":" << (float)p.up
		<< ",\"" << k << "AllFwd\":" << (float)m_all.fwd
		<< ",\"" << k << "Default\":" << (IsDefault() ? "true" : "false");
	// The whole table, so the CONFIG card's scope selector can show ANY scope's values without
	// round-tripping the selection through C++ -- the panel keeps the selection, this keeps the data.
	o << ",\"" << k << "Table\":{\"all\":{\"own\":true,\"p\":["
		<< (float)m_all.fwd << "," << (float)m_all.right << "," << (float)m_all.up << "]}";
	for (int i = 0; i < kWeaponPivotClassCount; ++i) {
		const WeaponPivotPoint r = Resolve((WeaponPivotClass)i);
		o << ",\"" << WeaponPivotClassName((WeaponPivotClass)i) << "\":{\"own\":"
			<< (m_hasOverride[i] ? "true" : "false")
			<< ",\"p\":[" << (float)r.fwd << "," << (float)r.right << "," << (float)r.up << "]}";
	}
	o << "}";
}

std::string WeaponPivotStore::DebugStateJson() const {
	std::ostringstream o;
	const WeaponPivotPoint act = ResolveActive();
	o << "{\"activeClass\":\"" << WeaponPivotClassName(m_activeClass) << "\""
		<< ",\"activeWeapon\":\"" << m_activeWeapon << "\""
		<< ",\"resolved\":[" << act.fwd << "," << act.right << "," << act.up << "]"
		<< ",\"fromOverride\":" << (HasOverride(m_activeClass) ? "true" : "false")
		<< ",\"all\":[" << m_all.fwd << "," << m_all.right << "," << m_all.up << "]"
		<< ",\"classes\":{";
	for (int i = 0; i < kWeaponPivotClassCount; ++i) {
		if (i) o << ",";
		o << "\"" << WeaponPivotClassName((WeaponPivotClass)i) << "\":{"
			<< "\"own\":" << (m_hasOverride[i] ? "true" : "false")
			<< ",\"p\":[" << m_class[i].fwd << "," << m_class[i].right << "," << m_class[i].up << "]}";
	}
	o << "},\"isDefault\":" << (IsDefault() ? "true" : "false") << "}";
	return o.str();
}

WeaponPivotStore& WeaponPivotRef(WeaponPivotOwner owner) {
	// Two independent stores. Function-local statics so construction order cannot bite, and
	// indexed by owner rather than named separately so adding a third effect is one enum entry.
	static WeaponPivotStore s_stores[kWeaponPivotOwnerCount];
	const int i = (int)owner;
	return s_stores[(i >= 0 && i < kWeaponPivotOwnerCount) ? i : 0];
}

// ================================ console ===================================================

namespace {
bool ParseNum(const char* v, double& out) {
	if (!v || !*v) return false;
	char* end = nullptr;
	const double d = std::strtod(v, &end);
	if (end == v) return false;
	out = d;
	return true;
}
} // namespace

void WeaponPivot_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// `pivot <recoil|bob> <action> ...`. The owner is REQUIRED and comes first: the two effects
	// have separate hinges, and a command that guessed which one you meant would be the fastest
	// way back to them drifting apart unnoticed.
	WeaponPivotOwner owner;
	if (!WeaponPivotOwnerFromName(argc >= 3 ? args->ArgV(2) : "", owner)) {
		advancedfx::Message(
			"%s pivot recoil|bob all <fwd> [right] [up] - the hinge that effect uses by default.\n"
			"%s pivot recoil|bob class <rifle|smg|pistol|shotgun|sniper|knife|utility> <fwd> [right] [up]\n"
			"%s pivot recoil|bob clear <class|all> | reset | state\n"
			"Weapon Recoil and Weapon Bob keep SEPARATE pivots; the markers are amber (recoil)\n"
			"and cyan (bob). Negative <fwd> puts the hinge BEHIND the eye, right for a knife.\n",
			cmd, cmd, cmd);
		advancedfx::Message("[pivot][recoil] %s\n", WeaponPivotRecoilRef().DebugStateJson().c_str());
		advancedfx::Message("[pivot][bob] %s\n", WeaponPivotBobRef().DebugStateJson().c_str());
		return;
	}
	WeaponPivotStore& st = WeaponPivotRef(owner);
	const char* ownerName = WeaponPivotOwnerName(owner);
	const char* action = (argc >= 4) ? args->ArgV(3) : "";

	// A bare `pivot <owner>` reports that owner's state. The full usage lives in the
	// unknown-owner branch above, which is the only place it can be reached without already
	// knowing the syntax.
	if (0 == _stricmp(action, "state") || !*action) {
		advancedfx::Message("[pivot][%s] %s\n", ownerName, st.DebugStateJson().c_str());
		return;
	}

	if (0 == _stricmp(action, "reset")) {
		st.ResetToDefaults();
		advancedfx::Message("mirv_filmmaker: %s pivots reset to the measured defaults.\n", ownerName);
		return;
	}

	if (0 == _stricmp(action, "all")) {
		WeaponPivotPoint p = st.All();
		double v = 0.0;
		if (argc >= 5 && ParseNum(args->ArgV(4), v)) p.fwd = v;
		if (argc >= 6 && ParseNum(args->ArgV(5), v)) p.right = v;
		if (argc >= 7 && ParseNum(args->ArgV(6), v)) p.up = v;
		st.SetAll(p);
		const WeaponPivotPoint a = st.All();
		advancedfx::Message("mirv_filmmaker: %s pivot All = (%.1f fwd, %.1f right, %.1f up). "
			"Weapons with their own hinge are unaffected -- `pivot clear all` to make this win everywhere.\n",
			ownerName, a.fwd, a.right, a.up);
		return;
	}

	if (0 == _stricmp(action, "class")) {
		WeaponPivotClass c;
		if (!WeaponPivotClassFromName(argc >= 5 ? args->ArgV(4) : "", c)) {
			advancedfx::Message("mirv_filmmaker: unknown weapon class. "
				"Use rifle|smg|pistol|shotgun|sniper|knife|utility.\n");
			return;
		}
		WeaponPivotPoint p = st.Resolve(c);
		double v = 0.0;
		if (argc >= 6 && ParseNum(args->ArgV(5), v)) p.fwd = v;
		if (argc >= 7 && ParseNum(args->ArgV(6), v)) p.right = v;
		if (argc >= 8 && ParseNum(args->ArgV(7), v)) p.up = v;
		st.SetOverride(c, p);
		const WeaponPivotPoint r = st.Resolve(c);
		advancedfx::Message("mirv_filmmaker: %s pivot %s = (%.1f fwd, %.1f right, %.1f up).%s\n",
			ownerName, WeaponPivotClassName(c), r.fwd, r.right, r.up,
			c == st.ActiveClass() ? " (this is the weapon on screen)" : "");
		return;
	}

	// Per-AXIS write for the CONFIG sliders: a slider moves one component, and building the whole
	// point in JS from a possibly-stale table is how two sliders end up fighting. <scope> is "all"
	// or a class; writing to a class creates its override if it had none.
	if (0 == _stricmp(action, "set")) {
		const char* scope = (argc >= 5) ? args->ArgV(4) : "";
		const char* axis = (argc >= 6) ? args->ArgV(5) : "";
		double v = 0.0;
		if (!ParseNum(argc >= 7 ? args->ArgV(6) : "", v)) {
			advancedfx::Message("mirv_filmmaker: `pivot set <all|class> <fwd|right|up> <value>`.\n");
			return;
		}
		const bool isAll = 0 == _stricmp(scope, "all");
		WeaponPivotClass c = WeaponPivotClass::Rifle;
		if (!isAll && !WeaponPivotClassFromName(scope, c)) {
			advancedfx::Message("mirv_filmmaker: unknown pivot scope. Use all|rifle|smg|pistol|shotgun|sniper|knife|utility.\n");
			return;
		}
		WeaponPivotPoint pt = isAll ? st.All() : st.Resolve(c);
		if (0 == _stricmp(axis, "fwd")) pt.fwd = v;
		else if (0 == _stricmp(axis, "right")) pt.right = v;
		else if (0 == _stricmp(axis, "up")) pt.up = v;
		else {
			advancedfx::Message("mirv_filmmaker: pivot axis must be fwd|right|up.\n");
			return;
		}
		if (isAll) st.SetAll(pt); else st.SetOverride(c, pt);
		return; // silent: this is driven per-frame by a slider drag
	}

	if (0 == _stricmp(action, "clear")) {
		const char* what = (argc >= 5) ? args->ArgV(4) : "";
		if (0 == _stricmp(what, "all")) {
			st.ClearAllOverrides();
			advancedfx::Message("mirv_filmmaker: every %s per-weapon pivot cleared; All now applies to all.\n", ownerName);
			return;
		}
		WeaponPivotClass c;
		if (!WeaponPivotClassFromName(what, c)) {
			advancedfx::Message("mirv_filmmaker: `pivot clear <class|all>`.\n");
			return;
		}
		st.ClearOverride(c);
		const WeaponPivotPoint r = st.Resolve(c);
		advancedfx::Message("mirv_filmmaker: %s pivot %s cleared, now inherits All (%.1f fwd).\n",
			ownerName, WeaponPivotClassName(c), r.fwd);
		return;
	}

	advancedfx::Message("%s pivot %s all|class|clear|reset|state (run `%s pivot` for details).\n", cmd, ownerName, cmd);
}

} // namespace Filmmaker
