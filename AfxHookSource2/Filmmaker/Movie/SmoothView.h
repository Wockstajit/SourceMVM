#pragma once
#include "SmoothViewAirMotionMath.h"
#include <string>
namespace advancedfx { class ICommandArgs; }
namespace Filmmaker {


// Smooth View: breathing, strafe tilt, and falling/jumping motion only.
// Standalone. No shared state with ViewFx or any other system.
class SmoothView {
public:
	void SetEnabled(bool on);
	bool Enabled() const { return m_enabled; }

	// Breathing
	void SetBreathFreqX(double v);  void SetBreathFreqY(double v);
	double BreathFreqX() const { return m_breathFreqX; }
	double BreathFreqY() const { return m_breathFreqY; }
	void SetStandBreathAmp(double v); void SetWalkBreathAmp(double v); void SetRunBreathAmp(double v);
	double StandBreathAmp() const { return m_standAmp; }
	double WalkBreathAmp() const  { return m_walkAmp; }
	double RunBreathAmp() const   { return m_runAmp; }

	// Tilt
	// Strafe lean, driven by velocity . Right only -- forward/back never rolls the view.
	void SetTiltEnabled(bool on);
	bool TiltEnabled() const { return m_tiltEnabled; }
	// NOTE: nominally degrees, but reproduces the mod's radians-as-degrees arithmetic, so 15
	// yields ~0.52 deg of lean. See the comment in ApplyCamera before "fixing" this.
	void SetTiltFactor(double deg);
	double TiltFactor() const { return m_tiltFactor; }
	void SetTiltSmoothSpeed(double v);
	double TiltSmoothSpeed() const { return m_tiltSmoothSpeed; }
	void SetTiltInvert(bool on);
	bool TiltInvert() const { return m_tiltInvert; }

	// Effects
	void SetFallingEnabled(bool on);  bool FallingEnabled() const { return m_fallingEnabled; }
	void SetFallingIntensity(double v); double FallingIntensity() const { return m_fallingIntensity; }
	void SetJumpingEnabled(bool on);  bool JumpingEnabled() const { return m_jumpingEnabled; }
	void SetJumpingIntensity(double v); double JumpingIntensity() const { return m_jumpingIntensity; }
	void SetHealthSwayEnabled(bool on); bool HealthSwayEnabled() const { return m_healthSwayEnabled; }
	void SetHealthSwayIntensity(double v); double HealthSwayIntensity() const { return m_healthSwayIntensity; }
	void SetPlayerHealth(double hp, double maxHp);
	double PlayerHealth() const { return m_playerHealth; }
	double PlayerMaxHealth() const { return m_playerMaxHealth; }

	void ResetToDefaults();
	bool SettingsAreDefault() const;
	void Reset();
	void Pump(double dt, double speed);

	// The motion signal ApplyCamera needs, derived from the camera pose the GAME produced.
	//
	// This lived in the view-setup trampoline as a block of function-local statics. Those are
	// process-global: they survived demo changes and spectator switches, so a fresh demo could
	// integrate against the previous one's last eye position. As members they are cleared by
	// Reset() along with every other filter.
	//
	// MUST be fed the PRE-SHAKE pose. The handheld layer drifts the camera along its own
	// forward/right axes; sampling velocity from the shaken origin feeds that drift back in as
	// phantom strafe, which is what made plain forward/back walking roll the view.
	struct MotionSample {
		double dt = 0.0;          // 0 on a same-tick render frame -- see SampleMotion
		double speed = 0.0;       // planar, units/sec
		double lateralSpeed = 0.0; // velocity . Right; forward/back is exactly 0 by construction
		double vz = 0.0;          // vertical, units/sec
		bool onGround = true;
	};
	MotionSample SampleMotion(double curTime, double x, double y, double z, double yawDegrees);
	void SetPovContext(bool active, int pawnIndex);
	void ApplyCamera(double& pitch, double& yaw, double& roll, double& fov, double dt, double speed, double lateralSpeed, double vz, bool onGround, bool playbackPaused);

	std::string DebugStateJson() const;

private:
	double Clamp(double v, double lo, double hi);
	static void Figure8(double phase, double& x, double& y);

	bool m_enabled = false;

	bool m_povActive = false;
	int m_povPawnIndex = -1;

	// Breathing
	double m_breathFreqX = 1.0, m_breathFreqY = 1.0;
	double m_standAmp = 0.2, m_walkAmp = 0.4, m_runAmp = 0.8;
	double m_breathAmp = 0.2;
	double m_breathPhase = 0;
	double m_breathSpeed = 5.0;
	double m_breathX = 0, m_breathY = 0;

	// Health. cl_health_sway_enabled 1, cl_health_sway_intensity 0.5.
	bool m_healthSwayEnabled = true;
	double m_healthSwayIntensity = 0.5;
	double m_healthTime = 0;
	double m_playerHealth = 100.0;
	double m_playerMaxHealth = 100.0;

	// Tilt
	bool m_tiltEnabled = true;
	double m_tiltFactor = 40.0;
	double m_tiltSmoothSpeed = 5.0; // mod's cl_tilt_smooth_speed
	bool m_tiltInvert = false;
	double m_tilt = 0, m_tiltTarget = 0;

	// Effects. cl_falling_enabled 1 / cl_jumping_enabled 1, intensities 0.5.
	bool m_fallingEnabled = true, m_jumpingEnabled = true;
	double m_fallingIntensity = 0.5, m_jumpingIntensity = 0.5;
	SmoothViewAirMotionState m_airMotionState;

	// Motion sampler (SampleMotion). Was function-local statics in main.cpp.
	bool m_haveMotion = false;
	double m_motionLastX = 0, m_motionLastY = 0, m_motionLastZ = 0, m_motionLastTime = 0;
	double m_motionSpeed = 0, m_motionLateral = 0, m_motionVz = 0;
	// How many times the teleport guard fired. Reported by `smoothview state`: a count that
	// climbs while a player merely strafes means the guard is misfiring and wiping live tilt.
	unsigned long long m_motionTeleports = 0;
	// Cosmetic-oscillator state. These run on the RENDER clock, not the demo clock (see the
	// oscDt block in ApplyCamera): their targets are still demo-derived, but the phase and the
	// smoothers step per rendered frame so they glide instead of stepping once per tick.
	double m_breathTargetAmp = 0;  // held between ticks; seeded from m_standAmp on Reset()
	int m_oscFrame = -1;           // last frame the oscillators advanced (duplicate-apply guard)

	// DBG
	unsigned long long m_frames = 0;
};

SmoothView& SmoothViewRef();
void SmoothView_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
}
