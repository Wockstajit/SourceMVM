#define NOMINMAX
#include "WorldWarp.h"

#include "../Movie/CameraBridge.h"
#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Filmmaker {
namespace {

std::mutex g_mutex;
WorldWarp::Settings g_settings;
std::atomic<int> g_activeMode{ 0 };
std::atomic<int> g_cubemapFace{ -1 };
std::atomic<float> g_cubemapBaseYaw{ 0.0f };

} // namespace

WorldWarp::Settings WorldWarp_Get() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_settings;
}

bool WorldWarp_Active() {
	return g_activeMode.load(std::memory_order_relaxed) != (int)WorldWarp::Mode::Off;
}

bool WorldWarp_WantsCubemap() {
	return g_activeMode.load(std::memory_order_relaxed) == (int)WorldWarp::Mode::LittlePlanet;
}

void WorldWarp_SetMode(WorldWarp::Mode m, bool applyPresetDefaults) {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (applyPresetDefaults) {
		const bool keepOrigin = g_settings.useSceneOrigin;
		const float ox = g_settings.origin[0], oy = g_settings.origin[1], oz = g_settings.origin[2];
		g_settings = WorldWarp::PresetFor(m);
		g_settings.useSceneOrigin = keepOrigin;
		g_settings.origin[0] = ox; g_settings.origin[1] = oy; g_settings.origin[2] = oz;
	} else {
		g_settings.mode = m;
	}
	g_activeMode.store((int)g_settings.mode, std::memory_order_release);
	if (g_settings.mode == WorldWarp::Mode::LittlePlanet)
		g_cubemapFace.store(0, std::memory_order_release);
	else
		g_cubemapFace.store(-1, std::memory_order_release);
}

void WorldWarp_Reset() {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_settings = WorldWarp::Settings{};
	g_activeMode.store((int)WorldWarp::Mode::Off, std::memory_order_release);
	g_cubemapFace.store(-1, std::memory_order_release);
}

bool WorldWarp_PlaceOriginAtCamera() {
	double origin[3] = {}, angles[3] = {}, fov = 90.0;
	CameraBridge_GetCurrentCamera(origin, angles, fov);
	std::lock_guard<std::mutex> lock(g_mutex);
	g_settings.useSceneOrigin = true;
	g_settings.origin[0] = (float)origin[0];
	g_settings.origin[1] = (float)origin[1];
	g_settings.origin[2] = (float)origin[2];
	return true;
}

int WorldWarp_PeekCubemapFace(float baseYaw) {
	if (!WorldWarp_WantsCubemap()) return -1;
	int f = g_cubemapFace.load(std::memory_order_acquire);
	if (f < 0 || f >= 6) {
		f = 0;
		g_cubemapFace.store(0, std::memory_order_release);
	}
	g_cubemapBaseYaw.store(baseYaw, std::memory_order_relaxed);
	return f;
}

int WorldWarp_ConsumeCapturedFace() {
	if (!WorldWarp_WantsCubemap()) return -1;
	int f = g_cubemapFace.load(std::memory_order_acquire);
	if (f < 0 || f >= 6) f = 0;
	g_cubemapFace.store((f + 1) % 6, std::memory_order_release);
	return f;
}

void WorldWarp_SetActiveCubemapFace(int faceIndex, float baseYaw) {
	g_cubemapBaseYaw.store(baseYaw, std::memory_order_relaxed);
	g_cubemapFace.store(faceIndex, std::memory_order_release);
}

int WorldWarp_ActiveCubemapFace(float* outBaseYaw) {
	const int face = g_cubemapFace.load(std::memory_order_acquire);
	if (outBaseYaw) *outBaseYaw = g_cubemapBaseYaw.load(std::memory_order_relaxed);
	return face;
}

bool WorldWarp_OverrideCubemapFaceView(int faceIndex, float baseYaw,
	float& pitch, float& yaw, float& roll, float& fov) {
	if (faceIndex < 0 || faceIndex >= 6) return false;
	// 108 deg 4:3 horizontal FOV ensures > 90 deg vertical FOV even on 16:9 displays
	// so every face covers the full 90x90 cubemap quadrant with zero clamping seams.
	fov = 108.0f;
	roll = 0.0f;
	switch (faceIndex) {
	case 0: pitch =  89.95f; yaw = baseYaw;          break; // Down (-Z, center street)
	case 1: pitch =   0.0f;  yaw = baseYaw;          break; // Forward (+X)
	case 2: pitch =   0.0f;  yaw = baseYaw + 90.0f;  break; // Left (+Y)
	case 3: pitch =   0.0f;  yaw = baseYaw + 180.0f; break; // Back (-X)
	case 4: pitch =   0.0f;  yaw = baseYaw - 90.0f;  break; // Right (-Y)
	case 5: pitch = -89.95f; yaw = baseYaw;          break; // Up (+Z, sky rim)
	default: return false;
	}
	return true;
}

void WorldWarp_AppendStateJson(std::ostringstream& out) {
	const WorldWarp::Settings s = WorldWarp_Get();
	out << ",\"worldWarp\":{"
		<< "\"mode\":\"" << WorldWarp::ModeToken(s.mode) << "\""
		<< ",\"on\":" << (s.mode != WorldWarp::Mode::Off ? "true" : "false")
		<< ",\"planetScale\":" << s.planetScale
		<< ",\"planetSpin\":" << s.planetSpin
		<< ",\"planetTilt\":" << s.planetTilt
		<< ",\"planetConformal\":" << s.planetConformal
		<< ",\"planetRimSoftness\":" << s.planetRimSoftness
		<< ",\"horizonBend\":" << s.horizonBend
		<< ",\"horizonFlatRadius\":" << s.horizonFlatRadius
		<< ",\"horizonCurve\":" << s.horizonCurve
		<< ",\"waveHeight\":" << s.waveHeight
		<< ",\"waveLength\":" << s.waveLength
		<< ",\"waveSpeed\":" << s.waveSpeed
		<< ",\"waveDirection\":" << s.waveDirection
		<< ",\"waveRadial\":" << (s.waveRadial ? "true" : "false")
		<< ",\"waveFlatRadius\":" << s.waveFlatRadius
		<< ",\"foldAngle\":" << s.foldAngle
		<< ",\"foldAxisYaw\":" << s.foldAxisYaw
		<< ",\"foldOffset\":" << s.foldOffset
		<< ",\"foldSoftness\":" << s.foldSoftness
		<< ",\"foldFlipSide\":" << (s.foldFlipSide ? "true" : "false")
		<< ",\"useSceneOrigin\":" << (s.useSceneOrigin ? "true" : "false")
		<< ",\"origin\":[" << s.origin[0] << "," << s.origin[1] << "," << s.origin[2] << "]"
		<< "}";
}

void WorldWarp_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 3) {
		advancedfx::Message(
			"%s worldwarp mode <off|littleplanet|inception|horizon|wave|fold> [preset]\n"
			"%s worldwarp set <key> <value>\n"
			"%s worldwarp origin camera|clear|<x> <y> <z>\n"
			"%s worldwarp reset | status\n", cmd, cmd, cmd, cmd);
		return;
	}
	const char* sub = args->ArgV(2);
	if (0 == _stricmp(sub, "off")) {
		WorldWarp_SetMode(WorldWarp::Mode::Off, false);
		return;
	}
	if (0 == _stricmp(sub, "reset")) {
		WorldWarp_Reset();
		return;
	}
	if (0 == _stricmp(sub, "mode") || 0 == _stricmp(sub, "preset")) {
		if (argc < 4) return;
		WorldWarp::Mode m = WorldWarp::Mode::Off;
		if (!WorldWarp::ModeFromToken(args->ArgV(3), m)) {
			advancedfx::Warning("%s worldwarp: unknown mode '%s'.\n", cmd, args->ArgV(3));
			return;
		}
		const bool preset = (0 == _stricmp(sub, "preset")) || (argc >= 5 && 0 == _stricmp(args->ArgV(4), "preset"));
		WorldWarp_SetMode(m, preset);
		return;
	}
	if (0 == _stricmp(sub, "origin")) {
		if (argc < 4) return;
		const char* a = args->ArgV(3);
		if (0 == _stricmp(a, "camera") || 0 == _stricmp(a, "here")) {
			WorldWarp_PlaceOriginAtCamera();
			return;
		}
		if (0 == _stricmp(a, "clear") || 0 == _stricmp(a, "off")) {
			std::lock_guard<std::mutex> lock(g_mutex);
			g_settings.useSceneOrigin = false;
			return;
		}
		if (argc >= 6) {
			std::lock_guard<std::mutex> lock(g_mutex);
			g_settings.useSceneOrigin = true;
			g_settings.origin[0] = (float)std::atof(args->ArgV(3));
			g_settings.origin[1] = (float)std::atof(args->ArgV(4));
			g_settings.origin[2] = (float)std::atof(args->ArgV(5));
			return;
		}
		return;
	}
	if (0 == _stricmp(sub, "set") && argc >= 5) {
		const char* k = args->ArgV(3);
		const float v = (float)std::atof(args->ArgV(4));
		using WorldWarp::Clamp;
		std::lock_guard<std::mutex> lock(g_mutex);
		if (0 == _stricmp(k, "planetScale")) g_settings.planetScale = Clamp(v, 0.25f, 3.0f);
		else if (0 == _stricmp(k, "planetSpin")) g_settings.planetSpin = Clamp(v, -180.0f, 180.0f);
		else if (0 == _stricmp(k, "planetTilt")) g_settings.planetTilt = Clamp(v, -60.0f, 60.0f);
		else if (0 == _stricmp(k, "planetConformal")) g_settings.planetConformal = Clamp(v, 0.0f, 1.0f);
		else if (0 == _stricmp(k, "planetRimSoftness")) g_settings.planetRimSoftness = Clamp(v, 0.0f, 1.0f);
		else if (0 == _stricmp(k, "horizonBend")) g_settings.horizonBend = Clamp(v, -2.5f, 2.5f);
		else if (0 == _stricmp(k, "horizonFlatRadius")) g_settings.horizonFlatRadius = Clamp(v, 0.0f, 2500.0f);
		else if (0 == _stricmp(k, "horizonCurve")) g_settings.horizonCurve = Clamp(v, 1.1f, 3.5f);
		else if (0 == _stricmp(k, "waveHeight")) g_settings.waveHeight = Clamp(v, 0.0f, 600.0f);
		else if (0 == _stricmp(k, "waveLength")) g_settings.waveLength = Clamp(v, 64.0f, 4000.0f);
		else if (0 == _stricmp(k, "waveSpeed")) g_settings.waveSpeed = Clamp(v, -20.0f, 20.0f);
		else if (0 == _stricmp(k, "waveDirection")) g_settings.waveDirection = Clamp(v, 0.0f, 360.0f);
		else if (0 == _stricmp(k, "waveRadial")) g_settings.waveRadial = v > 0.5f;
		else if (0 == _stricmp(k, "waveFlatRadius")) g_settings.waveFlatRadius = Clamp(v, 0.0f, 1500.0f);
		else if (0 == _stricmp(k, "foldAngle")) g_settings.foldAngle = Clamp(v, -180.0f, 180.0f);
		else if (0 == _stricmp(k, "foldAxisYaw")) g_settings.foldAxisYaw = Clamp(v, 0.0f, 360.0f);
		else if (0 == _stricmp(k, "foldOffset")) g_settings.foldOffset = Clamp(v, -2500.0f, 2500.0f);
		else if (0 == _stricmp(k, "foldSoftness")) g_settings.foldSoftness = Clamp(v, 0.0f, 1000.0f);
		else if (0 == _stricmp(k, "foldFlipSide")) g_settings.foldFlipSide = v > 0.5f;
		else if (0 == _stricmp(k, "useSceneOrigin")) g_settings.useSceneOrigin = v > 0.5f;
		else advancedfx::Warning("%s worldwarp: unknown key '%s'.\n", cmd, k);
		return;
	}
	if (0 == _stricmp(sub, "status")) {
		const WorldWarp::Settings s = WorldWarp_Get();
		advancedfx::Message("worldwarp: mode=%s scale=%.2f bend=%.2f waveH=%.1f fold=%.1fdeg origin=%d (%.0f %.0f %.0f)\n",
			WorldWarp::ModeToken(s.mode), s.planetScale, s.horizonBend, s.waveHeight, s.foldAngle,
			s.useSceneOrigin ? 1 : 0, s.origin[0], s.origin[1], s.origin[2]);
		return;
	}
	// Shorthand: `mirv_filmmaker worldwarp littleplanet`
	WorldWarp::Mode m = WorldWarp::Mode::Off;
	if (WorldWarp::ModeFromToken(sub, m)) {
		WorldWarp_SetMode(m, true);
		return;
	}
	advancedfx::Warning("%s worldwarp: unknown subcommand '%s'.\n", cmd, sub);
}

} // namespace Filmmaker
