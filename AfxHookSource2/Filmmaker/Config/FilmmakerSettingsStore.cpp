#include "FilmmakerSettingsStore.h"

#include "../Filmmaker.h"
#include "EffectsPersistence.h"
#include "../EasterEggs/EasterEggWorld.h"
#include "../Movie/CameraShakeStack.h"
#include "../Movie/HandheldCam.h"
#include "../Movie/RagdollWeapon.h"
#include "../Movie/DeathAnimations.h"
#include "../Movie/NervousSystem.h"
#include "../Movie/MethodActing.h"
#include "../Movie/SmoothView.h"
#include "../Movie/ViewFx.h"
#include "../Movie/WeaponRecoil.h"
#include "../Movie/WeaponBob.h"
#include "../../ViewModel.h" // CAMERA tab: VIEWMODEL card (per-player vs one universal)
#include "../../MirvCommands.h" // CAMERA tab: shot FOV (`mirv_fov`)
#include "../Panorama/AimCrosshairHud.h"
#include "../Panorama/CameraEditorHud.h"
#include "../World/LightFx.h"
#include "../World/SmokeAppearance.h"
#include "../Environment/EnvironmentBakedLighting.h"
#include "../World/ShadowFx.h"
#include "../World/BakedShadowFx.h"
#include "../World/DynamicShadowFx.h"
#include "../World/CustomShadowFx.h"
#include "../World/NightMode.h"
#include "../World/AutoExposure.h"
#include "../World/SkyOrient.h"
#include "../World/SunFx.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/TextEncoding.h"
#include "../Render/RenderOutput.h" // the render workspace's last output folder
#include "../Platform/JsonParser.h"
#include "../../hlaeFolder.h"
#include "../../../Shared/AfxConsole.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace Filmmaker {
namespace {

constexpr unsigned long long kSaveDebounceMs = 500;

std::map<std::string, std::string> g_ui;
// Every module's constructed state, captured before the saved file is read. See
// FilmmakerSettings_ResetGroup.
std::string g_defaultSnapshot;
std::string g_savedSnapshot;
std::string g_pendingSnapshot;
unsigned long long g_lastChangeMs = 0;
bool g_loaded = false;
bool g_dirty = false;

// ---- quiet-reset window (see FilmmakerSettings_BeginQuietReset) -------------------------------
// 0 = off. While settling we are waiting for the reset's console commands to actually land (they
// go through ExecuteClientCmd, so they are not visible in the very next BuildSnapshot); after that
// g_quietSnapshot is the post-reset state and the window ends the moment the live state differs
// from it -- that difference can only be the operator editing something, which must be saved.
bool g_quiet = false;
int g_quietSettleFrames = 0;
std::string g_quietSnapshot;
constexpr int kQuietSettleFrames = 12;

std::wstring SettingsPath() {
	std::wstring path = GetHlaeRoamingAppDataFolderW();
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
		path += L'\\';
	path += L"filmmaker_settings.json";
	return path;
}

const JsonValue* Object(const JsonValue& parent, const char* key) {
	const JsonValue* value = parent.Find(key);
	return value && value->type == JsonValue::Type::Object ? value : nullptr;
}

void ReadBool(const JsonValue* object, const char* key, void (*set)(bool)) {
	if (!object) return;
	if (const JsonValue* value = object->Find(key); value && value->type == JsonValue::Type::Bool)
		set(value->boolean);
}

void ReadNumber(const JsonValue* object, const char* key, void (*set)(double)) {
	if (!object) return;
	if (const JsonValue* value = object->Find(key); value && value->type == JsonValue::Type::Number)
		set(value->number);
}

bool ValidUiToken(const char* text, bool key) {
	if (!text) return false;
	const size_t maxLength = key ? 64 : 80;
	size_t length = 0;
	for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
		if (++length > maxLength) return false;
		if (key && !(std::isalnum(*p) || *p == '.' || *p == '_' || *p == '-')) return false;
		if (!key && (*p < 0x20 || *p == 0x7f)) return false;
	}
	return length > 0;
}

void WriteViewFx(JsonBuilder& b) {
	ViewFx& fx = ViewFxRef();
	b.Key("viewFx"); b.BeginObject();
	b.DoubleField("weaponSway", fx.WeaponSway().Intensity());
	b.DoubleField("weaponFloatTightness", fx.WeaponFloat().Tightness());
	b.DoubleField("weaponFloatAmount", fx.WeaponFloat().Amount());
	b.DoubleField("deadzoneAim", fx.DeadzoneAim().Intensity());
	b.BoolField("deadzoneCrosshairLock", fx.DeadzoneAim().LockCrosshair());
	b.EndObject();
}

void WriteSmoothView(JsonBuilder& b) {
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

void WriteHandheld(JsonBuilder& b) {
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
void WriteWeaponBob(JsonBuilder& b) {
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

void LoadWeaponBob(const JsonValue* object) {
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

// CAMERA tab, VIEWMODEL card. Per-field ownership is saved alongside the values: a released
// axis and an axis that merely happens to sit at zero are different states, and only the
// ownership flag distinguishes them. Without it, reopening would silently take over every axis.
void WriteViewmodel(JsonBuilder& b) {
	const ViewmodelState vm = Viewmodel_State();
	b.Key("viewmodel"); b.BeginObject();
	b.BoolField("universal", vm.universal);
	b.DoubleField("offsetX", vm.offsetX);
	b.DoubleField("offsetY", vm.offsetY);
	b.DoubleField("offsetZ", vm.offsetZ);
	b.DoubleField("fov", vm.fov);
	b.BoolField("leftHanded", vm.leftHanded);
	b.BoolField("ownX", vm.ownX);
	b.BoolField("ownY", vm.ownY);
	b.BoolField("ownZ", vm.ownZ);
	b.BoolField("ownFov", vm.ownFov);
	b.BoolField("ownLeftHanded", vm.ownLeftHanded);
	b.EndObject();
}

void LoadViewmodel(const JsonValue* object) {
	if (!object) return;
	auto number = [&](const char* key, double fallback) {
		const JsonValue* value = object->Find(key);
		return value && value->type == JsonValue::Type::Number ? value->number : fallback;
	};
	auto boolean = [&](const char* key) {
		const JsonValue* value = object->Find(key);
		return value && value->type == JsonValue::Type::Bool && value->boolean;
	};
	// Assign first, then release what was not owned: Assign is what carries the value, so a
	// field that is saved un-owned still restores its retained number for the slider to show.
	Viewmodel_Assign(ViewmodelField::OffsetX, (float)number("offsetX", 0));
	Viewmodel_Assign(ViewmodelField::OffsetY, (float)number("offsetY", 0));
	Viewmodel_Assign(ViewmodelField::OffsetZ, (float)number("offsetZ", 0));
	Viewmodel_Assign(ViewmodelField::Fov, (float)number("fov", 68));
	Viewmodel_Assign(ViewmodelField::LeftHanded, boolean("leftHanded") ? 1.0f : 0.0f);
	if (!boolean("ownX")) Viewmodel_Release(ViewmodelField::OffsetX);
	if (!boolean("ownY")) Viewmodel_Release(ViewmodelField::OffsetY);
	if (!boolean("ownZ")) Viewmodel_Release(ViewmodelField::OffsetZ);
	if (!boolean("ownFov")) Viewmodel_Release(ViewmodelField::Fov);
	if (!boolean("ownLeftHanded")) Viewmodel_Release(ViewmodelField::LeftHanded);
	Viewmodel_SetUniversal(boolean("universal"));
}

void WriteCamFov(JsonBuilder& b) {
	b.BoolField("camFovOn", MirvFov_Enabled());
	b.IntField("camFov", (int64_t)std::lround(MirvFov_Value()));
}

void LoadCamFov(const JsonValue& root) {
	const JsonValue* onVal = root.Find("camFovOn");
	const JsonValue* fovVal = root.Find("camFov");
	if (!onVal && !fovVal) return;
	const bool on = onVal && onVal->type == JsonValue::Type::Bool && onVal->boolean;
	float fov = 90.f;
	if (fovVal && fovVal->type == JsonValue::Type::Number)
		fov = (float)fovVal->number;
	MirvFov_Set(on, fov);
}

void WriteLighting(JsonBuilder& b) {
	bool engineLights, bakedShadows, dynamicShadows;
	Environment::CaptureNativeLighting(engineLights, bakedShadows, dynamicShadows);
	const EasterEggLightingState light = EasterEggLighting_UserState();
	b.Key("lighting"); b.BeginObject();
	b.IntField("lightR", light.r);
	b.IntField("lightG", light.g);
	b.IntField("lightB", light.b);
	b.DoubleField("lightAmount", light.amount);
	b.BoolField("lightUniform", light.uniform);
	b.BoolField("engineLights", engineLights);
	b.DoubleField("sunYaw", light.yaw);
	b.DoubleField("sunPitch", LightFx_SunPitch());
	b.DoubleField("sunGlowSize", SunFx_GlowSize());
	b.DoubleField("skyYaw", SkyOrient_Yaw());
	b.DoubleField("skyPitch", SkyOrient_Pitch());
	b.BoolField("skyMirror", SkyOrient_Mirror());
	b.BoolField("skyMoveSky", SkyOrient_SkyEnabled());
	b.BoolField("skyMoveFog", SkyOrient_FogEnabled());
	b.BoolField("shadows", bakedShadows && dynamicShadows);
	b.BoolField("shaderShadows", CustomShadowFx_Enabled());
	b.DoubleField("shaderShadowStrength", CustomShadowFx_Strength());
	b.DoubleField("shaderShadowStepPixels", CustomShadowFx_StepPixels());
	b.DoubleField("shaderShadowThickness", CustomShadowFx_Thickness());
	b.IntField("shaderShadowSteps", CustomShadowFx_Steps());
	// Keep the two shadow choices independently; the master is only an all-on/off shortcut.
	b.BoolField("bakedShadows", bakedShadows);
	b.BoolField("dynamicShadows", dynamicShadows);
	b.DoubleField("exposure", NightMode_Exposure());
	b.BoolField("exposureAdapt", AutoExposure_Enabled());
	b.DoubleField("exposureSpeed", AutoExposure_Speed());
	b.DoubleField("exposureCompensation", AutoExposure_Compensation());
	b.EndObject();
}

void WriteSmokeAppearance(JsonBuilder& b) {
    const auto state = SmokeAppearance::Snapshot();
    b.Key("smokeAppearance"); b.BeginObject(); b.BoolField("enabled", state.enabled);
    for (const auto& c : SmokeAppearance::Controls) b.DoubleField(c.name, state.parameters.*(c.member));
    b.EndObject();
}
void LoadSmokeAppearance(const JsonValue* object) {
    if (!object) return;
    SmokeAppearance::Parameters p;
    for (const auto& c : SmokeAppearance::Controls) {
        const auto* v = object->Find(c.name);
        if (v && v->type == JsonValue::Type::Number) SmokeAppearance::Set(p, c, static_cast<float>(v->number));
    }
    const auto* on = object->Find("enabled");
    SmokeAppearance::Configure(on && on->type == JsonValue::Type::Bool && on->boolean, p);
}

std::string BuildSnapshot() {
	JsonBuilder b;
	b.BeginObject();
	b.IntField("version", 1);
	WriteViewFx(b);
	WriteSmoothView(b);
	WriteHandheld(b);
	// Weapon Recoil and Weapon Bob persist like the rest of the CAMERA tab (they used to be
	// session-only, and a restart silently dropped them).
	WeaponRecoilRef().WriteState(b);
	WeaponRecoilRef().WritePresets(b);
	WriteWeaponBob(b);
	WriteViewmodel(b);
	WriteCamFov(b);
	WriteLighting(b);
	WriteSmokeAppearance(b);
	// The render workspace opens on the last output folder used instead of asking every session.
	b.StringField("renderOutputFolder", WideToUtf8(RenderOutputSettingsRef().folder));
	b.BoolField("fisheye", Fisheye_Enabled());
	b.BoolField("trueAimCrosshair", AimCrosshairHudRef().Enabled());
	b.BoolField("deathAnimOn", DeathAnimations_Enabled());
	b.DoubleField("deathAnimSource", DeathAnimations_Source());
	b.DoubleField("deathAnimBlendIn", DeathAnimations_BlendIn());
	b.DoubleField("deathAnimStrength", DeathAnimations_Strength());
	b.DoubleField("deathAnimImpact", DeathAnimations_Impact());
	b.DoubleField("deathAnimBlendOut", DeathAnimations_BlendOut());
	b.BoolField("deathAnimWalls", DeathAnimations_Walls());
	b.BoolField("deathAnimSolid", DeathAnimations_Solid());
	b.Key("deathAnimPlayers"); b.BeginObject();
	for (const auto& kv : DeathAnimations_PlayerClips())
		b.StringField(kv.first.c_str(), kv.second);
	b.EndObject();
	b.BoolField("nervousOn", NervousSystem_Enabled());
	b.DoubleField("nervousStrength", NervousSystem_Strength());
	b.DoubleField("nervousDuration", NervousSystem_Duration());
	b.BoolField("nervousReach", NervousSystem_Reach());
	b.BoolField("nervousBreath", NervousSystem_Breath());
	b.BoolField("nervousWrithe", NervousSystem_Writhe());
	b.BoolField("nervousTwitch", NervousSystem_Twitch());
	b.DoubleField("nervousBlendIn", NervousSystem_BlendIn());
	b.DoubleField("nervousBlendOut", NervousSystem_BlendOut());
	b.BoolField("nervousRollOver", NervousSystem_RollOver());
	b.BoolField("ragdollKeepWeapon", RagdollWeapon_KeepEnabled());
	b.BoolField("ragdollDeathFire", RagdollWeapon_FireEnabled());
	b.DoubleField("ragdollFireChance", RagdollWeapon_FireChance());
	b.DoubleField("ragdollFireDurationMs", RagdollWeapon_FireDurationMs());
	b.DoubleField("ragdollHoldDurationMs", RagdollWeapon_HoldDurationMs());
	b.BoolField("actingOn", MethodActing_Enabled());
	b.BoolField("actingBlink", MethodActing_BlinkEnabled());
	b.BoolField("actingGaze", MethodActing_GazeEnabled());
	b.BoolField("actingDeathEyes", MethodActing_DeathEyesEnabled());
	b.DoubleField("actingSquintLevel", MethodActing_SquintLevel());
	b.DoubleField("actingHoldMs", MethodActing_HoldMs());
	b.DoubleField("actingAttackMs", MethodActing_AttackMs());
	b.DoubleField("actingReleaseMs", MethodActing_ReleaseMs());
	b.DoubleField("actingBlinkSec", MethodActing_BlinkIntervalSec());
	b.DoubleField("actingDoubleBlink", MethodActing_DoubleBlinkChance());
	b.BoolField("actingSettleEyes", MethodActing_SettleEyes());
	b.BoolField("keepCameraAcrossDemos", EffectsPersistence_Keep("camera"));
	b.BoolField("keepEffectsAcrossDemos", EffectsPersistence_Keep("effects"));
	b.BoolField("keepRagdollsAcrossDemos", EffectsPersistence_Keep("ragdolls"));
	CameraEditorHud& editor = CameraEditorHudRef();
	b.Key("editor"); b.BeginObject();
	b.BoolField("scale", editor.ScaleEnabled());
	b.StringField("bottomMode", editor.GetBottomMode() == CameraEditorHud::BottomMode::Graph
		? "graph" : editor.GetBottomMode() == CameraEditorHud::BottomMode::CameraTimeline
		? "camera" : "native");
	b.StringField("hudView", editor.UserHudView() == CameraEditorHud::HudView::HideAll
		? "hidden" : editor.UserHudView() == CameraEditorHud::HudView::InGame ? "game" : "full");
	b.BoolField("debugOverlay", editor.DebugOverlay());
	b.EndObject();
	b.Key("ui"); b.BeginObject();
	for (const auto& entry : g_ui)
		b.StringField(entry.first.c_str(), entry.second);
	b.EndObject();
	b.EndObject();
	return b.Str();
}

bool SaveSnapshot(const std::string& snapshot) {
	std::ofstream file{ std::filesystem::path(SettingsPath()), std::ios::trunc };
	if (!file.is_open()) return false;
	file << snapshot;
	return file.good();
}

void LoadViewFx(const JsonValue* object) {
	if (!object) return;
	ViewFx& fx = ViewFxRef();
	auto number = [&](const char* key, auto setter) {
		if (const JsonValue* value = object->Find(key); value && value->type == JsonValue::Type::Number)
			setter(value->number);
	};
	auto boolean = [&](const char* key, auto setter) {
		if (const JsonValue* value = object->Find(key); value && value->type == JsonValue::Type::Bool)
			setter(value->boolean);
	};
	number("weaponSway", [&](double v) { fx.WeaponSway().SetIntensity(v); });
	number("weaponFloatTightness", [&](double v) { fx.WeaponFloat().SetTightness(v); });
	number("weaponFloatAmount", [&](double v) { fx.WeaponFloat().SetAmount(v); });
	number("deadzoneAim", [&](double v) { fx.DeadzoneAim().SetIntensity(v); });
	boolean("deadzoneCrosshairLock", [&](bool v) { fx.DeadzoneAim().SetLockCrosshair(v); });
}

void LoadSmoothView(const JsonValue* object) {
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

void LoadHandheld(const JsonValue* object) {
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


void LoadLighting(const JsonValue* object) {
	if (!object) return;
	int r = 255, g = 255, b = 255;
	bool hasR = false, hasG = false, hasB = false;
	auto number = [&](const char* key, auto setter) {
		if (const JsonValue* value = object->Find(key); value && value->type == JsonValue::Type::Number)
			setter(value->number);
	};
	if (const JsonValue* value = object->Find("lightR"); value && value->type == JsonValue::Type::Number) {
		r = (int)value->number; hasR = true;
	}
	if (const JsonValue* value = object->Find("lightG"); value && value->type == JsonValue::Type::Number) {
		g = (int)value->number; hasG = true;
	}
	if (const JsonValue* value = object->Find("lightB"); value && value->type == JsonValue::Type::Number) {
		b = (int)value->number; hasB = true;
	}
	if (hasR || hasG || hasB) LightFx_SetTint(r, g, b);
	number("lightAmount", [](double v) { LightFx_SetAmount((float)v); });
	if (const JsonValue* value = object->Find("lightUniform"); value && value->type == JsonValue::Type::Bool)
		LightFx_SetUniform(value->boolean);
    if(const JsonValue* value=object->Find("engineLights");value&&value->type==JsonValue::Type::Bool)
        LightFx_SetSceneLightsEnabled(value->boolean);
	number("sunYaw", [](double v) { LightFx_SetSunYaw((float)v); });
	number("sunPitch", [](double v) { LightFx_SetSunPitch((float)v); });
	number("sunGlowSize", [](double v) { SunFx_SetGlowSize((float)v); });
	number("skyYaw", [](double v) { SkyOrient_SetYaw((float)v); });
	number("skyPitch", [](double v) { SkyOrient_SetPitch((float)v); });
	if (const JsonValue* value = object->Find("skyMirror"); value && value->type == JsonValue::Type::Bool)
		SkyOrient_SetMirror(value->boolean);
	if (const JsonValue* value = object->Find("skyMoveSky"); value && value->type == JsonValue::Type::Bool)
		SkyOrient_SetSkyEnabled(value->boolean);
	if (const JsonValue* value = object->Find("skyMoveFog"); value && value->type == JsonValue::Type::Bool)
		SkyOrient_SetFogEnabled(value->boolean);
	if (const JsonValue* value = object->Find("shadows"); value && value->type == JsonValue::Type::Bool) {
		ShadowFx_SetEnabled(value->boolean);
	}
	{
		// Split values take precedence over the compatibility master value.
		if (const JsonValue* value = object->Find("bakedShadows"); value && value->type == JsonValue::Type::Bool)
			BakedShadowFx_SetEnabled(value->boolean);
		if (const JsonValue* value = object->Find("dynamicShadows"); value && value->type == JsonValue::Type::Bool)
			DynamicShadowFx_SetEnabled(value->boolean);
	}
	if (const JsonValue* value = object->Find("shaderShadows"); value && value->type == JsonValue::Type::Bool)
		CustomShadowFx_SetEnabled(value->boolean);
	number("shaderShadowStrength", [](double v) { CustomShadowFx_SetStrength((float)v); });
	number("shaderShadowStepPixels", [](double v) { CustomShadowFx_SetStepPixels((float)v); });
	number("shaderShadowThickness", [](double v) { CustomShadowFx_SetThickness((float)v); });
	number("shaderShadowSteps", [](double v) { CustomShadowFx_SetSteps((int)v); });
	number("exposure", [](double v) { NightMode_SetExposure((float)v); });
	if (const JsonValue* value = object->Find("exposureAdapt"); value && value->type == JsonValue::Type::Bool)
		AutoExposure_SetEnabled(value->boolean);
	number("exposureSpeed", [](double v) { AutoExposure_SetSpeed((float)v); });
	number("exposureCompensation", [](double v) { AutoExposure_SetCompensation((float)v); });
}

} // namespace

void FilmmakerSettings_Load() {
	if (g_loaded) return;
	g_loaded = true;
	// FIRST, before anything can overwrite them: the modules are still at their construction
	// defaults on this line, and this is the only moment that is true. Everything "reset to
	// defaults" ever means is replayed from here.
	g_defaultSnapshot = BuildSnapshot();
	std::ifstream file{ std::filesystem::path(SettingsPath()) };
	if (!file.is_open()) {
		g_savedSnapshot = BuildSnapshot();
		g_pendingSnapshot = g_savedSnapshot;
		return;
	}
	std::stringstream buffer;
	buffer << file.rdbuf();
	JsonValue root;
	if (!JsonParse(buffer.str(), root) || root.type != JsonValue::Type::Object) {
		g_savedSnapshot = BuildSnapshot();
		g_pendingSnapshot = g_savedSnapshot;
		return;
	}

	LoadViewFx(Object(root, "viewFx"));
	LoadSmoothView(Object(root, "smoothView"));
	const JsonValue* handheld = Object(root, "handheld");
	LoadHandheld(handheld);
	WeaponRecoilRef().LoadPresets(root.Find("weaponRecoilPresets"));
	WeaponRecoilRef().LoadState(Object(root, "weaponRecoil"));
	LoadWeaponBob(Object(root, "weaponBob"));
	LoadViewmodel(Object(root, "viewmodel"));
	LoadCamFov(root);
	LoadLighting(Object(root, "lighting"));
	LoadSmokeAppearance(Object(root, "smokeAppearance"));
	// Empty never overwrites: a settings reset must not forget where renders go.
	if (const JsonValue* folder = root.Find("renderOutputFolder");
		folder && folder->type == JsonValue::Type::String && !folder->str.empty())
		RenderOutputSettingsRef().folder = Utf8ToWide(folder->str);
	// Migrate the old stack-wide "shake survives" value into Handheld only. The new control
	// uses the inverse meaning: 100 is fully centred. Legacy 100 meant no lock, so leave the
	// new opt-in default alone in that case.
	if (!handheld || !handheld->Find("centerLockEnabled")) {
		if (const JsonValue* legacy = root.Find("shakeMovesAim");
			legacy && legacy->type == JsonValue::Type::Number && legacy->number < 100.0) {
			HandheldCamRef().SetCenterLockEnabled(true);
			HandheldCamRef().SetCenterLockStrength(100.0 - legacy->number);
		}
	}
	ReadBool(&root, "fisheye", Fisheye_SetEnabled);
	ReadBool(&root, "trueAimCrosshair", [](bool v) { AimCrosshairHudRef().SetEnabled(v); });

	ReadBool(&root, "deathAnimOn", DeathAnimations_SetEnabled);
	ReadNumber(&root, "deathAnimSource", [](double v) { DeathAnimations_SetSource((int)v); });
	ReadNumber(&root, "deathAnimBlendIn", DeathAnimations_SetBlendIn);
	ReadNumber(&root, "deathAnimStrength", DeathAnimations_SetStrength);
	ReadNumber(&root, "deathAnimImpact", DeathAnimations_SetImpact);
	ReadNumber(&root, "deathAnimBlendOut", DeathAnimations_SetBlendOut);
	ReadBool(&root, "deathAnimWalls", DeathAnimations_SetWalls);
	ReadBool(&root, "deathAnimSolid", DeathAnimations_SetSolid);
	if (const JsonValue* players = Object(root, "deathAnimPlayers"))
		for (const auto& kv : players->obj)
			if (kv.second.type == JsonValue::Type::String) DeathAnimations_SetPlayerClip(kv.first, kv.second.str);
	ReadBool(&root, "nervousOn", NervousSystem_SetEnabled);
	ReadNumber(&root, "nervousStrength", NervousSystem_SetStrength);
	ReadNumber(&root, "nervousDuration", NervousSystem_SetDuration);
	ReadBool(&root, "nervousReach", NervousSystem_SetReach);
	ReadBool(&root, "nervousBreath", NervousSystem_SetBreath);
	ReadBool(&root, "nervousWrithe", NervousSystem_SetWrithe);
	ReadBool(&root, "nervousTwitch", NervousSystem_SetTwitch);
	ReadNumber(&root, "nervousBlendIn", NervousSystem_SetBlendIn);
	ReadNumber(&root, "nervousBlendOut", NervousSystem_SetBlendOut);
	ReadBool(&root, "nervousRollOver", NervousSystem_SetRollOver);
	ReadBool(&root, "ragdollKeepWeapon", RagdollWeapon_SetKeepEnabled);
	ReadBool(&root, "ragdollDeathFire", RagdollWeapon_SetFireEnabled);
	ReadNumber(&root, "ragdollHoldDurationMs", RagdollWeapon_SetHoldDurationMs);
	ReadNumber(&root, "ragdollFireDurationMs", RagdollWeapon_SetFireDurationMs);
	ReadNumber(&root, "ragdollFireChance", RagdollWeapon_SetFireChance);
	ReadBool(&root, "actingOn", MethodActing_SetEnabled);
	ReadBool(&root, "actingBlink", MethodActing_SetBlinkEnabled);
	ReadBool(&root, "actingGaze", MethodActing_SetGazeEnabled);
	ReadBool(&root, "actingDeathEyes", MethodActing_SetDeathEyesEnabled);
	ReadNumber(&root, "actingSquintLevel", MethodActing_SetSquintLevel);
	ReadNumber(&root, "actingHoldMs", [](double v) { MethodActing_SetHoldMs(v == 250.0 ? 900.0 : v); });
	ReadNumber(&root, "actingAttackMs", MethodActing_SetAttackMs);
	ReadNumber(&root, "actingReleaseMs", [](double v) { MethodActing_SetReleaseMs(v == 350.0 ? 450.0 : v); });
	ReadNumber(&root, "actingBlinkSec", MethodActing_SetBlinkIntervalSec);
	ReadNumber(&root, "actingDoubleBlink", MethodActing_SetDoubleBlinkChance);
	ReadBool(&root, "actingSettleEyes", MethodActing_SetSettleEyes);
	// `effectsKeepAcrossDemos` was the single flag this shipped with; read it first so an existing
	// settings file keeps its answer, then let the per-tab keys override.
	ReadBool(&root, "effectsKeepAcrossDemos", [](bool v) { EffectsPersistence_SetKeep("effects", v); });
	ReadBool(&root, "keepCameraAcrossDemos", [](bool v) { EffectsPersistence_SetKeep("camera", v); });
	ReadBool(&root, "keepEffectsAcrossDemos", [](bool v) { EffectsPersistence_SetKeep("effects", v); });
	ReadBool(&root, "keepRagdollsAcrossDemos", [](bool v) { EffectsPersistence_SetKeep("ragdolls", v); });

	if (const JsonValue* editor = Object(root, "editor")) {
		CameraEditorHud& hud = CameraEditorHudRef();
		if (const JsonValue* value = editor->Find("scale"); value && value->type == JsonValue::Type::Bool)
			hud.SetScale(value->boolean);
		if (const JsonValue* value = editor->Find("bottomMode"); value && value->type == JsonValue::Type::String)
			hud.SetBottomMode(value->str == "graph" ? CameraEditorHud::BottomMode::Graph
				: value->str == "camera" ? CameraEditorHud::BottomMode::CameraTimeline
				: CameraEditorHud::BottomMode::Native);
		if (const JsonValue* value = editor->Find("hudView"); value && value->type == JsonValue::Type::String)
			hud.SetHudView(value->str == "hidden" ? CameraEditorHud::HudView::HideAll
				: value->str == "game" ? CameraEditorHud::HudView::InGame
				: CameraEditorHud::HudView::ShowAll);
		if (const JsonValue* value = editor->Find("debugOverlay"); value && value->type == JsonValue::Type::Bool)
			hud.SetDebugOverlay(value->boolean);
	}
	if (const JsonValue* ui = Object(root, "ui")) {
		for (const auto& entry : ui->obj) {
			if (ValidUiToken(entry.first.c_str(), true) && entry.second.type == JsonValue::Type::String
				&& ValidUiToken(entry.second.str.c_str(), false))
				g_ui[entry.first] = entry.second.str;
		}
	}
    if(g_ui.find("config.environment.tab")==g_ui.end() && g_ui["config.tab"]=="effects") {
        const std::string legacy=g_ui["config.fx.tab"];
        if(legacy=="lighting" || legacy=="atmos" || legacy=="weather") {
            g_ui["config.tab"]="environment";
            g_ui["config.environment.tab"]=legacy=="atmos"?"atmosphere":legacy=="weather"?"sky":"lighting";
        }
    }
	g_savedSnapshot = BuildSnapshot();
	g_pendingSnapshot = g_savedSnapshot;
}

void FilmmakerSettings_RunFrame() {
	if (!g_loaded) return;
	const std::string snapshot = BuildSnapshot();
	const unsigned long long now = GetTickCount64();
	if (g_quiet) {
		if (g_quietSettleFrames > 0) {
			// The reset commands are still draining out of the engine's command queue. Nothing is
			// saved and nothing is compared yet -- a snapshot taken mid-drain would be adopted as
			// "the post-reset state" and the window would end on the reset's own remaining writes.
			if (--g_quietSettleFrames == 0) g_quietSnapshot = snapshot;
			g_pendingSnapshot = snapshot;
			return;
		}
		if (snapshot == g_quietSnapshot) {
			// Still exactly what the reset produced: hold the file as it was.
			g_pendingSnapshot = snapshot;
			return;
		}
		// The operator has moved something. Resume normal autosave from here.
		g_quiet = false;
		g_quietSnapshot.clear();
	}
	if (snapshot != g_pendingSnapshot) {
		g_pendingSnapshot = snapshot;
		g_lastChangeMs = now;
		g_dirty = true;
	}
	if (g_dirty && now - g_lastChangeMs >= kSaveDebounceMs && SaveSnapshot(g_pendingSnapshot)) {
		g_savedSnapshot = g_pendingSnapshot;
		g_dirty = false;
	}
}

void FilmmakerSettings_BeginQuietReset() {
	if (!g_loaded) return;
	g_quiet = true;
	g_quietSettleFrames = kQuietSettleFrames;
	g_quietSnapshot.clear();
	g_dirty = false;                 // drop anything the reset itself already marked
	g_pendingSnapshot = g_savedSnapshot;
}

void FilmmakerSettings_AcceptCurrent() {
    g_quiet = false; g_quietSnapshot.clear();
    FilmmakerSettings_Flush();
}

void FilmmakerSettings_Flush() {
	if (!g_loaded) return;
	// Shutdown runs through here. Flushing inside a quiet window would write the level-local reset
	// to disk on exit -- the exact loss the window exists to prevent. `settings save` clears the
	// window first, so an explicit save still saves whatever is on screen.
	if (g_quiet) { g_dirty = false; return; }
	g_pendingSnapshot = BuildSnapshot();
	if (g_pendingSnapshot != g_savedSnapshot && SaveSnapshot(g_pendingSnapshot))
		g_savedSnapshot = g_pendingSnapshot;
	g_dirty = false;
}

bool FilmmakerSettings_UiBool(const char* key, bool fallback) {
	auto it = g_ui.find(key ? key : "");
	if (it == g_ui.end()) return fallback;
	return it->second == "1" || it->second == "true" || it->second == "on";
}

std::string FilmmakerSettings_UiString(const char* key, const char* fallback) {
	auto it = g_ui.find(key ? key : "");
	return it == g_ui.end() ? std::string(fallback ? fallback : "") : it->second;
}

bool FilmmakerSettings_ResetGroup(const char* group) {
	if (!group || !*group || g_defaultSnapshot.empty()) return false;

	JsonValue root;
	if (!JsonParse(g_defaultSnapshot, root) || root.type != JsonValue::Type::Object)
		return false;

	const bool all = (0 == _stricmp(group, "all"));
	bool matched = all;

	// Deliberately the SAME Load* functions the saved file goes through. A reset that walked its
	// own list of setters would be a second description of every setting, free to fall behind the
	// first one; this cannot.
	if (all || 0 == _stricmp(group, "viewFx")) { LoadViewFx(Object(root, "viewFx")); matched = true; }
	if (all || 0 == _stricmp(group, "smoothView")) { LoadSmoothView(Object(root, "smoothView")); matched = true; }
	if (all || 0 == _stricmp(group, "handheld")) { LoadHandheld(Object(root, "handheld")); matched = true; }
	if (all || 0 == _stricmp(group, "viewmodel")) { LoadViewmodel(Object(root, "viewmodel")); matched = true; }
	if (all || 0 == _stricmp(group, "weaponBob")) { LoadWeaponBob(Object(root, "weaponBob")); matched = true; }
	if (all || 0 == _stricmp(group, "weaponRecoil")) { WeaponRecoilRef().LoadState(Object(root, "weaponRecoil")); matched = true; }
	if (all || 0 == _stricmp(group, "camFov")) { LoadCamFov(root); matched = true; }
	if (all || 0 == _stricmp(group, "smokeAppearance")) { LoadSmokeAppearance(Object(root, "smokeAppearance")); matched = true; }
	if (all || 0 == _stricmp(group, "lighting")) { LoadLighting(Object(root, "lighting")); matched = true; }
	if (all || 0 == _stricmp(group, "flags")) {
		ReadBool(&root, "fisheye", Fisheye_SetEnabled);
		ReadBool(&root, "trueAimCrosshair", [](bool v) { AimCrosshairHudRef().SetEnabled(v); });

		ReadBool(&root, "deathAnimOn", DeathAnimations_SetEnabled);
		ReadNumber(&root, "deathAnimSource", [](double v) { DeathAnimations_SetSource((int)v); });
		ReadNumber(&root, "deathAnimBlendIn", DeathAnimations_SetBlendIn);
		ReadNumber(&root, "deathAnimStrength", DeathAnimations_SetStrength);
		ReadNumber(&root, "deathAnimImpact", DeathAnimations_SetImpact);
		ReadNumber(&root, "deathAnimBlendOut", DeathAnimations_SetBlendOut);
		ReadBool(&root, "deathAnimWalls", DeathAnimations_SetWalls);
		ReadBool(&root, "deathAnimSolid", DeathAnimations_SetSolid);
		if (const JsonValue* players = Object(root, "deathAnimPlayers"))
			for (const auto& kv : players->obj)
				if (kv.second.type == JsonValue::Type::String) DeathAnimations_SetPlayerClip(kv.first, kv.second.str);
		ReadBool(&root, "nervousOn", NervousSystem_SetEnabled);
		ReadNumber(&root, "nervousStrength", NervousSystem_SetStrength);
		ReadNumber(&root, "nervousDuration", NervousSystem_SetDuration);
		ReadBool(&root, "nervousReach", NervousSystem_SetReach);
		ReadBool(&root, "nervousBreath", NervousSystem_SetBreath);
		ReadBool(&root, "nervousWrithe", NervousSystem_SetWrithe);
		ReadBool(&root, "nervousTwitch", NervousSystem_SetTwitch);
		ReadNumber(&root, "nervousBlendIn", NervousSystem_SetBlendIn);
		ReadNumber(&root, "nervousBlendOut", NervousSystem_SetBlendOut);
		ReadBool(&root, "nervousRollOver", NervousSystem_SetRollOver);
		ReadBool(&root, "ragdollKeepWeapon", RagdollWeapon_SetKeepEnabled);
		ReadBool(&root, "ragdollDeathFire", RagdollWeapon_SetFireEnabled);
		ReadNumber(&root, "ragdollHoldDurationMs", RagdollWeapon_SetHoldDurationMs);
		ReadNumber(&root, "ragdollFireDurationMs", RagdollWeapon_SetFireDurationMs);
		ReadNumber(&root, "ragdollFireChance", RagdollWeapon_SetFireChance);
	ReadBool(&root, "actingOn", MethodActing_SetEnabled);
	ReadBool(&root, "actingBlink", MethodActing_SetBlinkEnabled);
	ReadBool(&root, "actingGaze", MethodActing_SetGazeEnabled);
	ReadBool(&root, "actingDeathEyes", MethodActing_SetDeathEyesEnabled);
	ReadNumber(&root, "actingSquintLevel", MethodActing_SetSquintLevel);
	ReadNumber(&root, "actingHoldMs", [](double v) { MethodActing_SetHoldMs(v == 250.0 ? 900.0 : v); });
	ReadNumber(&root, "actingAttackMs", MethodActing_SetAttackMs);
	ReadNumber(&root, "actingReleaseMs", [](double v) { MethodActing_SetReleaseMs(v == 350.0 ? 450.0 : v); });
	ReadNumber(&root, "actingBlinkSec", MethodActing_SetBlinkIntervalSec);
	ReadNumber(&root, "actingDoubleBlink", MethodActing_SetDoubleBlinkChance);
	ReadBool(&root, "actingSettleEyes", MethodActing_SetSettleEyes);
		matched = true;
	}
	return matched;
}

void FilmmakerSettings_AppendUiJson(std::ostringstream& o) {
	o << ",\"ui\":{";
	bool first = true;
	for (const auto& entry : g_ui) {
		if (!first) o << ",";
		first = false;
		o << "\"" << entry.first << "\":\"";
		// Keys are already restricted to [alnum . _ -] by ValidUiToken, but VALUES only exclude
		// control characters -- a quote or a backslash would otherwise break the whole snapshot,
		// and the panel's JSON.parse failing takes the entire HUD script down with it.
		for (char c : entry.second) {
			if (c == '"' || c == '\\') o << '\\';
			o << c;
		}
		o << "\"";
	}
	o << "}";
}

void FilmmakerSettings_SetUi(const char* key, const char* value) {
	if (!ValidUiToken(key, true) || !ValidUiToken(value, false)) return;
	if (g_ui[key] == value) return;
	g_ui[key] = value;
	g_dirty = true;
	g_lastChangeMs = GetTickCount64();
}

void FilmmakerSettings_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* action = argc >= 3 ? args->ArgV(2) : "";
	if (0 == _stricmp(action, "ui") && argc >= 5) {
		FilmmakerSettings_SetUi(args->ArgV(3), args->ArgV(4));
		return;
	}
	if (0 == _stricmp(action, "save")) {
		g_quiet = false; g_quietSnapshot.clear(); // an explicit save is the operator overriding the hold
		FilmmakerSettings_Flush();
		advancedfx::Message("mirv_filmmaker: settings saved.\n");
		return;
	}
	if (0 == _stricmp(action, "reset")) {
		const char* group = argc >= 4 ? args->ArgV(3) : "";
		if (FilmmakerSettings_ResetGroup(group))
			advancedfx::Message("mirv_filmmaker: %s reset to defaults.\n", group);
		else
			advancedfx::Warning("mirv_filmmaker: unknown settings group \"%s\". Use one of: "
				"viewFx, smoothView, handheld, viewmodel, camFov, lighting, flags, all.\n", group);
		return;
	}
	advancedfx::Message(
		"%s settings ui <key> <value>\n"
		"%s settings save\n"
		"%s settings reset viewFx|smoothView|handheld|viewmodel|camFov|lighting|flags|all\n"
		"    - back to the values the modules were constructed with (what a fresh install has).\n",
		cmd, cmd, cmd);
}

} // namespace Filmmaker
