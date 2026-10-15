#pragma once

#include <atomic>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

struct ThirdPersonCameraState {
	bool active = false;
	bool orbitInput = false;
	double yaw = 0.0;       // orbit offset relative to the player's facing (0 = behind)
	double pitch = 10.0;
	double distance = 150.0;
	double sens = 0.05;     // orbit mouse sensitivity, degrees per cursor pixel
};

// HLAE-owned third-person orbit camera for the demo spectator. The camera is solved
// every frame in the view-setup hook from the spectated pawn's render-time eye pose,
// so it works identically whether the demo is playing or paused, and never fights the
// engine's own chase/thirdperson camera (which we no longer touch at all).
//
// Threading: Enter/Exit/Set*/RunFrame run on the MAIN thread (Filmmaker pump /
// console dispatch). ViewSetupOverride/OwnsView run on the RENDER thread from
// main.cpp's CSetupView trampoline -- same model as FollowCamera's attach override.
class ThirdPersonCamera {
public:
	void Enter();
	void Exit();
	void RunFrame(); // main thread: consume orbit mouse input, track spectated pawn

	// Render-thread pose solve. Writes the orbit pose and returns true while active
	// (leaves fov untouched). Called from the CSetupView trampoline AFTER the
	// FollowCamera attach override; no-ops while the free cam owns the view.
	// `engineCam` / `engineAng` are the ENGINE's camera before any override: the player it
	// watches is the one we orbit, so LMB/RMB (next/prev player) move the orbit with it.
	bool ViewSetupOverride(double& x, double& y, double& z,
	                       double& pitch, double& yaw, double& roll,
	                       const double engineCam[3], const double engineAng[3]);
	bool OwnsView() const { return m_active; }

	void SetEnabled(bool enabled);
	void Toggle();

	void SetAngles(double yaw, double pitch);
	void SetDistance(double distance);
	void SetSensitivity(double degPerPixel);
	bool SetPreset(const char* preset);

	ThirdPersonCameraState State() const;
	std::string BuildStateJson() const;

private:
	bool m_active = false;
	bool m_orbitInput = false;
	double m_yaw = 0.0;      // orbit offset relative to the pawn's eye yaw
	double m_pitch = 10.0;
	double m_distance = 150.0;
	double m_sens = 0.05;    // degrees per cursor pixel (orbit input arrives in pixels)
	// Locked spectated pawn (kept when resolution flickers to -1). Written by RunFrame (main
	// thread, observer services) and ViewSetupOverride (render thread, engine camera).
	std::atomic<int> m_pawnIndex{ -1 };

	// Render-thread smoothing state (world-yaw ease as the tracked player flicks).
	double m_smoothYaw = 0.0;
	bool m_haveSmoothYaw = false;
	long long m_lastQpc = 0;
};

ThirdPersonCamera& ThirdPersonCameraRef();
void ThirdPerson_RunFrame();
void ThirdPerson_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
