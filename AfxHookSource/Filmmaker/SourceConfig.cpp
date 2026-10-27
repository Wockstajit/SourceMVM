#include "stdafx.h"

#include "SourceConfig.h"
#include "SourceEngine.h"
#include "SourceSettings.h"
#include "SourceViewModel.h"

#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Movie/HandheldCam.h>
#include <AfxHookSource2/Filmmaker/Movie/RecoilPivotMarker.h>
#include <AfxHookSource2/Filmmaker/Movie/SmoothView.h>
#include <AfxHookSource2/Filmmaker/Movie/WeaponBob.h>
#include <AfxHookSource2/Filmmaker/Movie/WeaponPivot.h>
#include <AfxHookSource2/Filmmaker/Movie/WeaponRecoil.h>
#include <AfxHookSource2/Filmmaker/Platform/JsonParser.h>
#include <Shared/AfxConsole.h>

#include <sstream>

extern WrpVEngineClient* g_VEngineClient;

namespace SourceFilmmaker {

namespace {

// CS2's kCameraReset (EffectsPersistence.cpp) for what CS:S has: no fisheye, true-aim crosshair,
// viewmodel card, shot FOV or lens yet.
const char* const kCameraReset[] = {
	"mirv_filmmaker smoothview reset",
	"mirv_filmmaker handheld reset quiet",
	"mirv_filmmaker weaponrecoil reset quiet",
	"mirv_filmmaker weaponbob reset",
	"mirv_filmmaker pivot recoil reset",
	"mirv_filmmaker pivot bob reset",
	"mirv_filmmaker pivotorb recoil off quiet",
	"mirv_filmmaker pivotorb bob off quiet",
};

void Run(const char* command) {
	if (g_VEngineClient) g_VEngineClient->ClientCmd_Unrestricted(command);
}

bool Bool(const Filmmaker::JsonValue& root, const char* key, bool fallback) {
	const Filmmaker::JsonValue* v = root.Find(key);
	return v ? v->AsBool(fallback) : fallback;
}

double Number(const Filmmaker::JsonValue& root, const char* key, double fallback) {
	const Filmmaker::JsonValue* v = root.Find(key);
	return v ? v->AsNumber(fallback) : fallback;
}

std::string String(const Filmmaker::JsonValue& root, const char* key, const std::string& fallback) {
	const Filmmaker::JsonValue* v = root.Find(key);
	return v ? v->AsString(fallback.c_str()) : fallback;
}

// A PIVOT drawer from a pivot store's fields under its prefix ("pvr", "pvb"): the class in shot, its
// gun, and the hinge per scope from its table.
void ReadPivot(const Filmmaker::JsonValue& root, const std::string& prefix, Ui::ConfigPivot& pv) {
	static const char* const kScopes[8] = { "all", "rifle", "smg", "pistol", "shotgun", "sniper", "knife", "utility" };
	pv.cls = String(root, (prefix + "Class").c_str(), "");
	pv.weapon = String(root, (prefix + "Weapon").c_str(), "");
	pv.atDefault = Bool(root, (prefix + "Default").c_str(), true);
	const Filmmaker::JsonValue* table = root.Find((prefix + "Table").c_str());
	if (!table) return;
	for (int i = 0; i < 8; ++i) {
		const Filmmaker::JsonValue* row = table->Find(kScopes[i]);
		const Filmmaker::JsonValue* hinge = row ? row->Find("p") : nullptr;
		if (!hinge || hinge->type != Filmmaker::JsonValue::Type::Array || hinge->arr.size() < 3) continue;
		for (int a = 0; a < 3; ++a) pv.hinge[i][a] = hinge->arr[(size_t)a].AsNumber(pv.hinge[i][a]);
	}
}

} // namespace

void Config_BuildModel(Ui::ConfigModel& m) {
	const DemoState demo = Demo_State();
	m.demo = demo.playing || demo.paused;
	m.keepCamera = Settings_KeepCamera();

	// SMOOTH VIEW: CS2's ConfigHud writes these fields itself.
	const Filmmaker::SmoothView& sv = Filmmaker::SmoothViewRef();
	m.svOn = sv.Enabled();
	m.svBreathX = sv.BreathFreqX();
	m.svBreathY = sv.BreathFreqY();
	m.svStandAmp = sv.StandBreathAmp();
	m.svWalkAmp = sv.WalkBreathAmp();
	m.svRunAmp = sv.RunBreathAmp();
	m.svTiltEnabled = sv.TiltEnabled();
	m.svTilt = sv.TiltFactor();
	m.svTiltSpeed = sv.TiltSmoothSpeed();
	m.svTiltInv = sv.TiltInvert();
	m.svFalling = sv.FallingEnabled();
	m.svJumping = sv.JumpingEnabled();
	m.svHealthSway = sv.HealthSwayEnabled();
	m.smoothViewDefault = sv.SettingsAreDefault();

	// The shake layers write their own fields; reading them back keeps one copy of what each says.
	std::ostringstream o;
	o << "{\"_\":0";
	Filmmaker::HandheldCamRef().AppendStateJson(o);
	Filmmaker::WeaponRecoilRef().AppendStateJson(o);
	Filmmaker::WeaponBobRef().AppendStateJson(o);
	Filmmaker::WeaponPivotRecoilRef().AppendStateJson(o, "pvr");
	Filmmaker::WeaponPivotBobRef().AppendStateJson(o, "pvb");
	o << "}";
	Filmmaker::JsonValue root;
	if (!Filmmaker::JsonParse(o.str(), root)) {
		if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("config.model", "state json did not parse");
		return;
	}
	m.handheldOn = Bool(root, "handheldOn", m.handheldOn);
	m.hhVmDecoupled = Bool(root, "hhVmDecoupled", m.hhVmDecoupled);
	m.hhCenterLockOn = Bool(root, "hhCenterLockOn", m.hhCenterLockOn);
	m.hhCenterLock = Number(root, "hhCenterLock", m.hhCenterLock);
	m.hhDrift = Number(root, "hhDrift", m.hhDrift);
	m.hhDriftDir = Number(root, "hhDriftDir", m.hhDriftDir);
	m.hhCurve = Number(root, "hhCurve", m.hhCurve);
	m.hhTilt = Number(root, "hhTilt", m.hhTilt);
	m.hhRegrip = Number(root, "hhRegrip", m.hhRegrip);
	m.hhSpeed = Number(root, "hhSpeed", m.hhSpeed);
	m.hhVariance = Number(root, "hhVariance", m.hhVariance);
	m.hhPeriod = Number(root, "hhPeriod", m.hhPeriod);
	m.hhIdle = String(root, "hhIdle", m.hhIdle);
	m.hhWalk = String(root, "hhWalk", m.hhWalk);
	m.hhRun = String(root, "hhRun", m.hhRun);
	m.handheldDefault = Filmmaker::HandheldCamRef().SettingsAreDefault();
	m.viewmodelChannel = ViewModel_Installed(); // the gun moves through SourceViewModel.cpp
	m.pivotMarkers = false;     // nor a drawer for the pivot markers (ShakeHost_DrawPivotOrb)

	m.wrOn = Bool(root, "wrOn", m.wrOn);
	m.wrAmount = Number(root, "wrAmount", m.wrAmount);
	m.wrPunch = Number(root, "wrPunch", m.wrPunch);
	m.wrRot = Number(root, "wrRot", m.wrRot);
	m.wrCamRot = Number(root, "wrCamRot", m.wrCamRot);
	m.wrCamPos = Number(root, "wrCamPos", m.wrCamPos);
	m.wrCamRoll = Number(root, "wrCamRoll", m.wrCamRoll);
	m.wrSnap = Number(root, "wrSnap", m.wrSnap);
	m.wrRecovery = Number(root, "wrRecovery", m.wrRecovery);
	m.wrStyle = String(root, "wrStyle", m.wrStyle);
	m.wrScope = String(root, "wrScope", m.wrScope);
	m.wrScopeKey = String(root, "wrScopeKey", m.wrScopeKey);
	m.wrScopeOwn = Bool(root, "wrScopeOwn", m.wrScopeOwn);
	m.wrScopeCovers = Bool(root, "wrScopeCovers", m.wrScopeCovers);
	if (const Filmmaker::JsonValue* overrides = root.Find("wrOverrides")) m.wrOverrides = (int)overrides->arr.size();
	m.wrLive = String(root, "wrLive", m.wrLive);
	m.wrLiveType = String(root, "wrLiveType", m.wrLiveType);
	m.wrLiveSlot = String(root, "wrLiveSlot", m.wrLiveSlot);
	m.wrPresets.clear();
	if (const Filmmaker::JsonValue* presets = root.Find("wrPresets"))
		for (const Filmmaker::JsonValue& preset : presets->arr)
			m.wrPresets.push_back({ String(preset, "name", ""), String(preset, "scope", "all"), String(preset, "key", "") });
	m.weaponRecoilDefault = Filmmaker::WeaponRecoilRef().SettingsAreDefault();
	ReadPivot(root, "pvr", m.pvr);
	m.pvr.orb = Filmmaker::RecoilPivotMarkerRef().Enabled(Filmmaker::WeaponPivotOwner::Recoil);
	ReadPivot(root, "pvb", m.pvb);
	m.pvb.orb = Filmmaker::RecoilPivotMarkerRef().Enabled(Filmmaker::WeaponPivotOwner::Bob);

	m.wbOn = Bool(root, "wbOn", m.wbOn);
	m.wbAmount = Number(root, "wbAmount", m.wbAmount);
	m.wbSway = Number(root, "wbSway", m.wbSway);
	m.wbTilt = Number(root, "wbTilt", m.wbTilt);
	m.wbLook = Number(root, "wbLook", m.wbLook);
	m.wbInertia = Number(root, "wbInertia", m.wbInertia);
	m.wbIdle = Number(root, "wbIdle", m.wbIdle);
	m.wbCam = Number(root, "wbCam", m.wbCam);
	m.wbStep = Number(root, "wbStep", m.wbStep);
	m.wbRate = Number(root, "wbRate", m.wbRate);
	m.weaponBobDefault = Filmmaker::WeaponBobRef().SettingsAreDefault();
}

void Config_RunCommands(const std::vector<std::string>& commands) {
	for (const std::string& command : commands) {
		if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("config.ui", "%s", command.c_str());
		Run(command.c_str());
	}
}

void Config_EffectsCommand(int argc, advancedfx::ICommandArgs* args) {
	const char* action = argc >= 3 ? args->ArgV(2) : "";
	const char* group = argc >= 4 ? args->ArgV(3) : "";
	if (0 == _stricmp(action, "reset") && 0 == _stricmp(group, "camera")) {
		for (const char* command : kCameraReset) Run(command);
		advancedfx::Message("effects: camera reset.\n");
		return;
	}
	if (0 == _stricmp(action, "keep") && 0 == _stricmp(group, "camera")) {
		const char* how = argc >= 5 ? args->ArgV(4) : "toggle";
		const bool keep = 0 == _stricmp(how, "on") || 0 == _stricmp(how, "1")
			|| (0 != _stricmp(how, "off") && 0 != _stricmp(how, "0") && !Settings_KeepCamera());
		Settings_SetKeepCamera(keep);
		advancedfx::Message("effects: keep camera between demos %s.\n", keep ? "on" : "off");
		return;
	}
	advancedfx::Message("mirv_filmmaker effects reset camera | keep camera on|off|toggle\n");
}

} // namespace SourceFilmmaker
