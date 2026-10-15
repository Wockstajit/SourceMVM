#include "WorldTransient.h"
#include "CloudFx.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_Linef
#include "../../SceneSystem.h"
#include "../../../Shared/AfxConsole.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace Filmmaker {

namespace {

// Parameters this module drives, and how many floats each occupies.
//
// The g_v* ones are vectors. Only the first two components are written: these are UV scales and
// pan speeds, so components 2 and 3 are padding, and multiplying padding is how you turn a
// working material into a corrupt one.
struct CloudParam {
	const char* name;
	int floats;        // how many components to scale
};

const CloudParam kDensityParams[]    = { { "g_flOpacityScale", 1 } };
const CloudParam kBrightnessParams[] = { { "g_flColorBoost",   1 } };
const CloudParam kSizeParams[]       = {
	{ "g_vMask1Scale", 2 }, { "g_vMask2Scale", 2 }, { "g_vMask3Scale", 2 }
};
const CloudParam kDriftParams[]      = {
	{ "g_vMask1PanSpeed", 2 }, { "g_vMask2PanSpeed", 2 }, { "g_vMask3PanSpeed", 2 }
};

float g_density    = kCloudDefault;
float g_brightness = kCloudDefault;
float g_size       = kCloudDefault;
float g_drift      = kCloudDefault;
int   g_materialCount = 0;

// Authored values, keyed by material name then parameter name. Captured the first time a
// parameter is touched, so a restore always writes back what the MAP shipped -- not whatever a
// previous override left behind.
std::map<std::pair<CMaterial2*, std::string>, std::map<std::string, std::vector<float>>> g_snapshots;

bool AnyOverride() {
	return g_density != kCloudDefault || g_brightness != kCloudDefault
		|| g_size != kCloudDefault || g_drift != kCloudDefault;
}

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Writes one parameter group on one material. `restore` ignores the multiplier and puts the
// snapshot back.
void ApplyGroup(CMaterial2* mat, const std::string& matName,
                const CloudParam* params, int count, float mul, bool restore) {
	for (int i = 0; i < count; ++i) {
		float* p = SceneSystem_FindMaterialParam(mat, params[i].name);
		if (!p) continue; // not every cloud material carries every parameter

		const auto key=std::make_pair(mat,matName);
        if (restore && g_snapshots.find(key)==g_snapshots.end()) continue;
        std::map<std::string, std::vector<float>>& perMat = g_snapshots[key];
		std::map<std::string, std::vector<float>>::iterator it = perMat.find(params[i].name);
		if (it == perMat.end()) {
            if (restore) continue;
			std::vector<float> snap;
			for (int c = 0; c < params[i].floats; ++c) snap.push_back(p[c]);
			it = perMat.insert(std::make_pair(std::string(params[i].name), snap)).first;
		}

		for (int c = 0; c < params[i].floats; ++c) {
			p[c] = restore ? it->second[c] : it->second[c] * mul;
		}
	}
}

void ForEachCloudMaterial(bool restore) {
	GetMaterialsArrayResult materials = { 0 };
	if (!g_pCResourceSystem) return;
	g_pCResourceSystem->GetMaterials(&materials);

	int touched = 0;
	for (uint64_t i = 0; i < materials.count; ++i) {
		// Freed/pending slots carry a -1 sentinel while the resource system grows this array
		// during a level load; dereferencing one crashed (crash.veh 2026-08-07, read of
		// 0xFFFFFFFFFFFFFFFF in materialsystem2 during the demo-start effects reset).
		CMaterial2** slot = materials.pArrMaterials[i];
		if (!slot || (uintptr_t)slot == (uintptr_t)-1) continue;
		CMaterial2* mat = *slot;
		if (!mat || (uintptr_t)mat == (uintptr_t)-1) continue;
		char nameBuf[512];
		if (!SceneSystem_MaterialName(mat, nameBuf, sizeof(nameBuf)) || !nameBuf[0]) continue;
		std::string matName(nameBuf);

		// Same match the existing `mirv_sky clouds color` backend uses (SceneSystem.cpp): any
		// loaded material whose name contains "clouds". That is what makes this map-adaptive with
		// no per-map table -- whatever cloud materials this map loaded are the ones written.
		if (matName.find("clouds") == std::string::npos) continue;

		ApplyGroup(mat, matName, kDensityParams,    1, float(WorldTransient::Number("cloudDensity", g_density)),    restore);
		ApplyGroup(mat, matName, kBrightnessParams, 1, float(WorldTransient::Number("cloudBrightness", g_brightness)), restore);
		ApplyGroup(mat, matName, kSizeParams,       3, float(WorldTransient::Number("cloudSize", g_size)),       restore);
		ApplyGroup(mat, matName, kDriftParams,      3, float(WorldTransient::Number("cloudDrift", g_drift)),      restore);

		SceneSystem_UpdateMaterial(mat);
		++touched;
	}
	g_materialCount = touched;

	MvmDebugLog_Linef("world.clouds", "%s %d material(s) density=%.2f bright=%.2f size=%.2f drift=%.2f",
		restore ? "restored" : "applied", touched, g_density, g_brightness, g_size, g_drift);
}

} // namespace

void CloudFx_OnLevelInit() {
    if (!g_snapshots.empty()) ForEachCloudMaterial(true);
    g_snapshots.clear();g_materialCount=0;
    MvmDebugLog_Linef("environment.baseline","cloud materials restored and generation released");
}
void CloudFx_Apply() { ForEachCloudMaterial(false); }

bool CloudFx_HasOverride() {
	return g_density != kCloudDefault || g_brightness != kCloudDefault
		|| g_size != kCloudDefault || g_drift != kCloudDefault;
}

void CloudFx_ResetAll() {
	if (!g_snapshots.empty()) {
		// Restore with the CURRENT multipliers still in place -- ForEachCloudMaterial ignores them
		// in restore mode -- then clear, so a later Apply() re-snapshots from clean values.
		ForEachCloudMaterial(true);
		g_snapshots.clear();
	}
	g_density = g_brightness = g_size = g_drift = kCloudDefault;
}

void CloudFx_SetDensity(float mul)    { g_density    = Clamp(mul, 0.0f, 3.0f); CloudFx_Apply(); }
void CloudFx_SetBrightness(float mul) { g_brightness = Clamp(mul, 0.0f, 3.0f); CloudFx_Apply(); }
void CloudFx_SetSize(float mul)       { g_size       = Clamp(mul, 0.1f, 4.0f); CloudFx_Apply(); }
void CloudFx_SetDrift(float mul)      { g_drift      = Clamp(mul, 0.0f, 8.0f); CloudFx_Apply(); }

float CloudFx_Density()    { return g_density; }
float CloudFx_Brightness() { return g_brightness; }
float CloudFx_Size()       { return g_size; }
float CloudFx_Drift()      { return g_drift; }
int   CloudFx_MaterialCount() { return g_materialCount; }

void CloudFx_AppendStateJson(std::ostringstream& o) {
	o << ",\"cloudDensity\":" << g_density;
	o << ",\"cloudBright\":" << g_brightness;
	o << ",\"cloudSize\":" << g_size;
	o << ",\"cloudDrift\":" << g_drift;
	o << ",\"cloudMats\":" << g_materialCount;
}

void CloudFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// argv: mirv_filmmaker world clouds <op> <value>
	const char* op = (argc >= 4) ? args->ArgV(3) : "";

	if (0 == _stricmp(op, "reset")) {
		CloudFx_ResetAll();
		advancedfx::Message("Clouds restored to the map's own values.\n");
		return;
	}
	if (0 == _stricmp(op, "apply")) {
		CloudFx_Apply();
		advancedfx::Message("Clouds re-applied to %d material(s).\n", g_materialCount);
		return;
	}

	struct Knob { const char* name; void (*set)(float); float (*get)(); };
	const Knob knobs[] = {
		{ "density",    &CloudFx_SetDensity,    &CloudFx_Density },
		{ "brightness", &CloudFx_SetBrightness, &CloudFx_Brightness },
		{ "size",       &CloudFx_SetSize,       &CloudFx_Size },
		{ "drift",      &CloudFx_SetDrift,      &CloudFx_Drift },
	};

	for (const Knob& k : knobs) {
		if (0 != _stricmp(op, k.name)) continue;
		if (argc >= 5) {
			k.set((float)atof(args->ArgV(4)));
			// `quiet` keeps a slider drag from flooding the console, same convention as the
			// other slider-backed commands.
			const bool quiet = (argc >= 6) && (0 == _stricmp(args->ArgV(5), "quiet"));
			if (!quiet) {
				advancedfx::Message("Cloud %s x%.2f on %d material(s).\n",
					k.name, k.get(), g_materialCount);
			}
			return;
		}
		advancedfx::Message("Cloud %s is x%.2f (1.00 = the map's own value).\n", k.name, k.get());
		return;
	}

	advancedfx::Message(
		"mirv_filmmaker world clouds - cloud look. Every value MULTIPLIES the map's own, so 1.0\n"
		"is untouched and the same number means the same thing on every map.\n"
		"Usage:\n"
		"  %s world clouds density <x>     - 0 = clear sky, 1 = as authored, >1 = thicker\n"
		"  %s world clouds brightness <x>  - dim or lift the clouds\n"
		"  %s world clouds size <x>        - >1 = smaller and busier, <1 = bigger and sparser\n"
		"  %s world clouds drift <x>       - how fast they scroll; 0 freezes them\n"
		"  %s world clouds apply           - re-assert after a map change\n"
		"  %s world clouds reset           - back to the map's own values\n"
		"Colour is a separate backend: mirv_sky clouds color <r> <g> <b>\n"
		"Currently %d cloud material(s) on this map.\n",
		cmd, cmd, cmd, cmd, cmd, cmd, g_materialCount);
}

} // namespace Filmmaker
