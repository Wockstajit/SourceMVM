#include "ThirdPersonCamera.h"

#include "CameraBridge.h"
#include "MovieMode.h"
#include "ThirdPersonCameraMath.h"
#include "FollowTargetProviders.h" // EntityAt

#include "../../ClientEntitySystem.h" // CEntityInstance, AfxGetSpectatedPawnIndex
#include "../../../Shared/AfxConsole.h"

#include <Windows.h> // QueryPerformanceCounter (render-thread wall-clock dt)

#include <cstdlib>
#include <cstring>
#include <sstream>

namespace Filmmaker {

namespace {

// Yaw ease time constant: how quickly the camera swings around when the tracked
// player flicks their view (and how damped the mouse orbit feels). Wall clock, so
// the ease behaves the same while the demo is paused.
constexpr double kYawSmoothTau = 0.12;

double r1(double v) {
	return floor(v * 10.0 + 0.5) / 10.0;
}

} // namespace

ThirdPersonCamera& ThirdPersonCameraRef() {
	static ThirdPersonCamera s_instance;
	return s_instance;
}

void ThirdPersonCamera::Enter() {
	CameraBridge_SetFreeCamEnabled(false);
	m_pawnIndex = AfxGetSpectatedPawnIndex();
	m_haveSmoothYaw = false;
	m_lastQpc = 0;
	m_active = true;
	m_orbitInput = true;
	CameraBridge_SetThirdPersonOrbitEnabled(true);
	if (m_pawnIndex < 0)
		advancedfx::Message("mirv_filmmaker: thirdperson waiting for a spectated player (switch to a POV).\n");
}

void ThirdPersonCamera::Exit() {
	CameraBridge_SetThirdPersonOrbitEnabled(false);
	m_orbitInput = false;
	m_active = false;
	m_haveSmoothYaw = false;
}

void ThirdPersonCamera::RunFrame() {
	if (!m_active)
		return;
	// Track the spectated pawn. While we own the view the render-eye fallback inside
	// AfxGetSpectatedPawnIndex can't match (the camera is away from any eye), so keep
	// the last locked index whenever resolution reads -1; observer-services still
	// reports switches (LMB/RMB player change) in matchmaking demos.
	const int idx = AfxGetSpectatedPawnIndex();
	if (idx >= 0)
		m_pawnIndex = idx;

	// Orbit input arrives as raw cursor PIXELS (one delta per frame, single-sourced in
	// main.cpp's recenter block); scale by the degrees-per-pixel sensitivity here.
	double dYawPix = 0.0, dPitchPix = 0.0;
	if (CameraBridge_ConsumeThirdPersonOrbit(dYawPix, dPitchPix))
		SetAngles(m_yaw + m_sens * dYawPix, m_pitch + m_sens * dPitchPix);
}

bool ThirdPersonCamera::ViewSetupOverride(double& x, double& y, double& z,
                                          double& pitch, double& yaw, double& roll,
                                          const double engineCam[3], const double engineAng[3]) {
	if (!m_active)
		return false;
	// The free cam (including FollowCamera previews, which ride on it) outranks us.
	if (CameraBridge_GetFreeCamEnabled())
		return false;

	// Follow the player the ENGINE's spectator camera is watching. In GOTV demos observer
	// services read mode 0 / target -1 (measured 2026-09-27, ragdolls.dem), and the main-thread
	// eye match cannot work once this override owns the view -- so without this the orbit stayed
	// on the first player while LMB/RMB switched the spectator (HUD) to the next one.
	{
		int best = -1;
		double bestScore = 1e30;
		const int highest = GetHighestEntityIndex();
		for (int i = 0; i <= highest; ++i) {
			CEntityInstance* ent = EntityAt(i);
			if (!ent || !ent->IsPlayerPawn())
				continue;
			float e[3] = {};
			ent->GetRenderEyeOrigin(e);
			const double eye[3] = { e[0], e[1], e[2] };
			const double score = ThirdPersonWatchScore(engineCam, engineAng[0], engineAng[1], eye);
			if (score >= 0.0 && score < bestScore) { bestScore = score; best = i; }
		}
		if (best >= 0)
			m_pawnIndex.store(best, std::memory_order_relaxed);
	}

	const int pawnIndex = m_pawnIndex.load(std::memory_order_relaxed);
	if (pawnIndex < 0)
		return false;
	CEntityInstance* pawn = EntityAt(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn())
		return false;

	float eyeF[3] = {};
	pawn->GetRenderEyeOrigin(eyeF);
	float angF[3] = {};
	pawn->GetRenderEyeAngles(angF);

	// Wall-clock dt so the yaw ease keeps working while the demo is paused (the
	// engine's frame time collapses to ~0 then).
	double dt = 0.0;
	{
		static LARGE_INTEGER s_freq = {};
		if (s_freq.QuadPart == 0) QueryPerformanceFrequency(&s_freq);
		LARGE_INTEGER now; QueryPerformanceCounter(&now);
		if (m_lastQpc != 0)
			dt = (double)(now.QuadPart - m_lastQpc) / (double)s_freq.QuadPart;
		m_lastQpc = now.QuadPart;
		if (dt < 0.0) dt = 0.0; else if (dt > 0.1) dt = 0.1;
	}

	const double targetYaw = ThirdPersonWrapYaw((double)angF[1] + m_yaw);
	if (!m_haveSmoothYaw) {
		m_smoothYaw = targetYaw;
		m_haveSmoothYaw = true;
	} else {
		m_smoothYaw = ThirdPersonApproachYaw(m_smoothYaw, targetYaw, dt, kYawSmoothTau);
	}

	const double eye[3] = { (double)eyeF[0], (double)eyeF[1], (double)eyeF[2] };
	const ThirdPersonPose pose = ThirdPersonSolvePose(eye, m_smoothYaw, m_pitch, m_distance);
	x = pose.x;
	y = pose.y;
	z = pose.z;
	pitch = pose.pitch;
	yaw = pose.yaw;
	roll = 0.0;
	return true;
}

void ThirdPersonCamera::SetEnabled(bool enabled) {
	if (enabled)
		Enter();
	else
		Exit();
}

void ThirdPersonCamera::Toggle() {
	SetEnabled(!m_active);
}

void ThirdPersonCamera::SetAngles(double yaw, double pitch) {
	m_yaw = ThirdPersonWrapYaw(yaw);
	m_pitch = ThirdPersonClampPitch(pitch);
}

void ThirdPersonCamera::SetDistance(double distance) {
	m_distance = ThirdPersonClampDistance(distance);
}

void ThirdPersonCamera::SetSensitivity(double degPerPixel) {
	if (degPerPixel < 0.005) degPerPixel = 0.005;
	if (degPerPixel > 0.5) degPerPixel = 0.5;
	m_sens = degPerPixel;
}

bool ThirdPersonCamera::SetPreset(const char* preset) {
	double yaw = 0.0;
	if (!ThirdPersonPresetYaw(preset, yaw))
		return false;
	SetAngles(yaw, m_pitch);
	return true;
}

ThirdPersonCameraState ThirdPersonCamera::State() const {
	ThirdPersonCameraState s;
	s.active = m_active;
	s.orbitInput = m_orbitInput;
	s.yaw = m_yaw;
	s.pitch = m_pitch;
	s.distance = m_distance;
	s.sens = m_sens;
	return s;
}

std::string ThirdPersonCamera::BuildStateJson() const {
	std::ostringstream o;
	o << "{";
	o << "\"active\":" << (m_active ? "true" : "false");
	o << ",\"orbitInput\":" << (m_orbitInput ? "true" : "false");
	o << ",\"yaw\":" << r1(m_yaw);
	o << ",\"pitch\":" << r1(m_pitch);
	o << ",\"distance\":" << r1(m_distance);
	o << ",\"sens\":" << m_sens;
	o << ",\"pawnIndex\":" << m_pawnIndex.load(std::memory_order_relaxed);
	o << "}";
	return o.str();
}

void ThirdPerson_RunFrame() {
	ThirdPersonCameraRef().RunFrame();
}

void ThirdPerson_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	ThirdPersonCamera& tp = ThirdPersonCameraRef();
	const char* action = argc >= 3 ? args->ArgV(2) : "toggle";

	if (0 == _stricmp(action, "on") || 0 == _stricmp(action, "1")) {
		tp.SetEnabled(true);
		MovieModeRef().SetMode(MovieMode::Mode::ThirdPerson);
	} else if (0 == _stricmp(action, "off") || 0 == _stricmp(action, "0")) {
		tp.SetEnabled(false);
		MovieModeRef().SetMode(MovieMode::Mode::Default);
	} else if (0 == _stricmp(action, "toggle")) {
		tp.Toggle();
		MovieModeRef().SetMode(tp.State().active ? MovieMode::Mode::ThirdPerson : MovieMode::Mode::Default);
	} else if (0 == _stricmp(action, "state")) {
		advancedfx::Message("[thirdperson][state] %s\n", tp.BuildStateJson().c_str());
		return;
	} else if (0 == _stricmp(action, "preset")) {
		if (argc < 4) {
			advancedfx::Warning("usage: %s thirdperson preset front|back|left|right\n", cmd);
			return;
		}
		if (!tp.SetPreset(args->ArgV(3))) {
			advancedfx::Warning("mirv_filmmaker: unknown thirdperson preset '%s'.\n", args->ArgV(3));
			return;
		}
		tp.SetEnabled(true);
		MovieModeRef().SetMode(MovieMode::Mode::ThirdPerson);
	} else if (0 == _stricmp(action, "angles")) {
		if (argc < 5) {
			advancedfx::Warning("usage: %s thirdperson angles <yaw> <pitch>\n", cmd);
			return;
		}
		tp.SetAngles(atof(args->ArgV(3)), atof(args->ArgV(4)));
		tp.SetEnabled(true);
		MovieModeRef().SetMode(MovieMode::Mode::ThirdPerson);
	} else if (0 == _stricmp(action, "distance")) {
		if (argc < 4) {
			advancedfx::Warning("usage: %s thirdperson distance <30-200>\n", cmd);
			return;
		}
		tp.SetDistance(atof(args->ArgV(3)));
		tp.SetEnabled(true);
		MovieModeRef().SetMode(MovieMode::Mode::ThirdPerson);
	} else if (0 == _stricmp(action, "sens")) {
		if (argc < 4) {
			advancedfx::Warning("usage: %s thirdperson sens <deg per pixel, 0.005-0.5; default 0.05>\n", cmd);
			return;
		}
		tp.SetSensitivity(atof(args->ArgV(3)));
	} else {
		advancedfx::Message(
			"Usage:\n"
			"%s thirdperson on|off|toggle|state\n"
			"%s thirdperson preset front|back|left|right\n"
			"%s thirdperson angles <yaw> <pitch>\n"
			"%s thirdperson distance <30-200>\n"
			"%s thirdperson sens <deg per pixel, 0.005-0.5>\n",
			cmd, cmd, cmd, cmd, cmd);
		return;
	}

	ThirdPersonCameraState s = tp.State();
	advancedfx::Message("mirv_filmmaker: thirdperson %s yaw=%.1f pitch=%.1f dist=%.1f sens=%.3f.\n",
		s.active ? "ON" : "off", s.yaw, s.pitch, s.distance, s.sens);
}

} // namespace Filmmaker
