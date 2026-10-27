#include "CameraFeelSettings.h"

#include "../Movie/HandheldCam.h"
#include "../Movie/SmoothView.h"
#include "../Movie/WeaponBob.h"
#include "../Movie/WeaponRecoil.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"

namespace Filmmaker {

namespace {

const JsonValue* Object(const JsonValue& parent, const char* key) {
	const JsonValue* value = parent.Find(key);
	return value && value->type == JsonValue::Type::Object ? value : nullptr;
}

} // namespace

void CameraFeel_WriteSmoothView(JsonBuilder& b) {
	SmoothView& sv = SmoothViewRef();
	b.Key("smoothView"); b.BeginObject();
	b.BoolField("enabled", sv.Enabled());
	b.DoubleField("breathX", sv.BreathFreqX());
	b.DoubleField("breathY", sv.BreathFreqY());
	b.DoubleField("standBreath", sv.StandBreathAmp());
	b.DoubleField("walkBreath", sv.WalkBreathAmp());
	b.DoubleField("runBreath", sv.RunBreathAmp());
	b.BoolField("tiltEnabled", sv.TiltEnabled());
	b.DoubleField("tilt", sv.TiltFactor());
	b.DoubleField("tiltSpeed", sv.TiltSmoothSpeed());
	b.BoolField("tiltInvert", sv.TiltInvert());
	b.BoolField("fallingEnabled", sv.FallingEnabled());
	b.BoolField("jumpingEnabled", sv.JumpingEnabled());
	b.BoolField("healthSwayEnabled", sv.HealthSwayEnabled());
	b.EndObject();
}

void CameraFeel_WriteHandheld(JsonBuilder& b) {
	HandheldCam& hh = HandheldCamRef();
	b.Key("handheld"); b.BeginObject();
	b.BoolField("enabled", hh.Enabled());
	b.BoolField("viewmodelDecoupled", hh.ViewmodelDecoupled());
	b.BoolField("centerLockEnabled", hh.CenterLockEnabled());
	b.DoubleField("centerLock", hh.CenterLockStrength());
	b.DoubleField("drift", hh.DriftIntensity());
	b.DoubleField("driftDir", hh.DriftDirection());
	b.DoubleField("curve", hh.CurveIntensity());
	b.DoubleField("tilt", hh.TiltIntensity());
	b.DoubleField("regrip", hh.RegripIntensity());
	b.DoubleField("speed", hh.Speed());
	b.DoubleField("variance", hh.Variance());
	b.DoubleField("period", hh.RegripPeriod());
	b.StringField("idleCurve", hh.StateCurveKey(HandheldState::Idle));
	b.StringField("walkCurve", hh.StateCurveKey(HandheldState::Walk));
	b.StringField("runCurve", hh.StateCurveKey(HandheldState::Run));
	b.EndObject();
}

// CAMERA tab, WEAPON BOB card. The per-class multipliers stay console-only and are not saved.
void CameraFeel_WriteWeaponBob(JsonBuilder& b) {
	WeaponBob& bob = WeaponBobRef();
	b.Key("weaponBob"); b.BeginObject();
	b.BoolField("enabled", bob.Enabled());
	b.DoubleField("amount", bob.Amount());
	b.DoubleField("sway", bob.Sway());
	b.DoubleField("tilt", bob.Tilt());
	b.DoubleField("lookSway", bob.LookSway());
	b.DoubleField("inertia", bob.Inertia());
	b.DoubleField("idle", bob.Idle());
	b.DoubleField("cameraShare", bob.CameraShare());
	b.DoubleField("stepSync", bob.StepSync());
	b.DoubleField("rate", bob.Rate());
	b.EndObject();
}

void CameraFeel_LoadWeaponBob(const JsonValue* object) {
	if (!object) return;
	WeaponBob& bob = WeaponBobRef();
	auto number = [&](const char* key, auto setter) {
		if (const JsonValue* value = object->Find(key); value && value->type == JsonValue::Type::Number)
			setter(value->number);
	};
	number("amount", [&](double v) { bob.SetAmount(v); });
	number("sway", [&](double v) { bob.SetSway(v); });
	number("tilt", [&](double v) { bob.SetTilt(v); });
	number("lookSway", [&](double v) { bob.SetLookSway(v); });
	number("inertia", [&](double v) { bob.SetInertia(v); });
	number("idle", [&](double v) { bob.SetIdle(v); });
	number("cameraShare", [&](double v) { bob.SetCameraShare(v); });
	number("stepSync", [&](double v) { bob.SetStepSync(v); });
	number("rate", [&](double v) { bob.SetRate(v); });
	// Last: the master switch reads as off while Amount is 0.
	if (const JsonValue* on = object->Find("enabled"); on && on->type == JsonValue::Type::Bool) bob.SetEnabled(on->boolean);
}

void CameraFeel_LoadSmoothView(const JsonValue* object) {
	if (!object) return;
	SmoothView& sv = SmoothViewRef();
	auto number = [&](const char* key, auto setter) {
		if (const JsonValue* value = object->Find(key); value && value->type == JsonValue::Type::Number)
			setter(value->number);
	};
	auto boolean = [&](const char* key, auto setter) {
		if (const JsonValue* value = object->Find(key); value && value->type == JsonValue::Type::Bool)
			setter(value->boolean);
	};
	number("breathX", [&](double v) { sv.SetBreathFreqX(v); });
	number("breathY", [&](double v) { sv.SetBreathFreqY(v); });
	number("standBreath", [&](double v) { sv.SetStandBreathAmp(v); });
	number("walkBreath", [&](double v) { sv.SetWalkBreathAmp(v); });
	number("runBreath", [&](double v) { sv.SetRunBreathAmp(v); });
	boolean("tiltEnabled", [&](bool v) { sv.SetTiltEnabled(v); });
	number("tilt", [&](double v) { sv.SetTiltFactor(v); });
	number("tiltSpeed", [&](double v) { sv.SetTiltSmoothSpeed(v); });
	boolean("tiltInvert", [&](bool v) { sv.SetTiltInvert(v); });
	boolean("fallingEnabled", [&](bool v) { sv.SetFallingEnabled(v); });
	boolean("jumpingEnabled", [&](bool v) { sv.SetJumpingEnabled(v); });
	boolean("healthSwayEnabled", [&](bool v) { sv.SetHealthSwayEnabled(v); });
	boolean("enabled", [&](bool v) { sv.SetEnabled(v); });
}

void CameraFeel_LoadHandheld(const JsonValue* object) {
	if (!object) return;
	HandheldCam& hh = HandheldCamRef();
	auto number = [&](const char* key, auto setter) {
		if (const JsonValue* value = object->Find(key); value && value->type == JsonValue::Type::Number)
			setter(value->number);
	};
	number("drift", [&](double v) { hh.SetDriftIntensity(v); });
	number("driftDir", [&](double v) { hh.SetDriftDirection(v); });
	number("curve", [&](double v) { hh.SetCurveIntensity(v); });
	number("tilt", [&](double v) { hh.SetTiltIntensity(v); });
	number("regrip", [&](double v) { hh.SetRegripIntensity(v); });
	number("speed", [&](double v) { hh.SetSpeed(v); });
	number("variance", [&](double v) { hh.SetVariance(v); });
	number("period", [&](double v) { hh.SetRegripPeriod(v); });
	number("centerLock", [&](double v) { hh.SetCenterLockStrength(v); });
	struct CurveSetting {
		const char* key;
		HandheldState state;
	};
	const CurveSetting curves[] = {
		{ "idleCurve", HandheldState::Idle },
		{ "walkCurve", HandheldState::Walk },
		{ "runCurve", HandheldState::Run },
	};
	for (const CurveSetting& item : curves) {
		if (const JsonValue* value = object->Find(item.key); value && value->type == JsonValue::Type::String)
			hh.SetStateCurve(item.state, value->str.c_str());
	}
	if (const JsonValue* value = object->Find("viewmodelDecoupled"); value && value->type == JsonValue::Type::Bool)
		hh.SetViewmodelDecoupled(value->boolean);
	if (const JsonValue* value = object->Find("centerLockEnabled"); value && value->type == JsonValue::Type::Bool)
		hh.SetCenterLockEnabled(value->boolean);
	if (const JsonValue* value = object->Find("enabled"); value && value->type == JsonValue::Type::Bool)
		hh.SetEnabled(value->boolean);
}

void CameraFeel_Write(JsonBuilder& b) {
	CameraFeel_WriteSmoothView(b);
	CameraFeel_WriteHandheld(b);
	// Weapon Recoil and Weapon Bob persist like the rest of the CAMERA tab (they used to be
	// session-only, and a restart silently dropped them).
	WeaponRecoilRef().WriteState(b);
	WeaponRecoilRef().WritePresets(b);
	CameraFeel_WriteWeaponBob(b);
}

void CameraFeel_Load(const JsonValue& root) {
	CameraFeel_LoadSmoothView(Object(root, "smoothView"));
	CameraFeel_LoadHandheld(Object(root, "handheld"));
	WeaponRecoilRef().LoadPresets(root.Find("weaponRecoilPresets"));
	WeaponRecoilRef().LoadState(Object(root, "weaponRecoil"));
	CameraFeel_LoadWeaponBob(Object(root, "weaponBob"));
}

} // namespace Filmmaker
