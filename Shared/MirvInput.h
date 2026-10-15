#pragma once

#include "AfxConsole.h"
#include "AfxMath.h"

#include <string>
#include <map>

#include <windows.h>

class IMirvInputDependencies {
public:
	virtual bool GetSuspendMirvInput() = 0;
	virtual void GetLastCameraData(double & x, double & y, double & z, double & rX, double & rY, double & rZ, double & fov) = 0;
	virtual void GetGameCameraData(double & x, double & y, double & z, double & rX, double & rY, double & rZ, double & fov) = 0;
	virtual double GetInverseScaledFov(double fov) = 0;
};

class MirvInput
{
public:
	enum KeyState
	{
		KS_DOWN,
		KS_UP
	};

	enum OffsetMode
	{
		OffsetMode_Last,
		OffsetMode_OwnLast,
		OffsetMode_Game,
		OffsetMode_Current
	};

	MirvInput(IMirvInputDependencies * dependencies);

	bool Override(float deltaT, float & Tx, float &Ty, float & Tz, float & Rx, float & Ry, float & Rz, float & Fov);
	
	bool Supply_CharEvent(WPARAM wParam, LPARAM lParam);
	bool Supply_KeyEvent(KeyState keyState, WPARAM wParam, LPARAM lParam);
	bool Supply_MouseEvent(DWORD uMsg, WPARAM & wParam, LPARAM & lParam);

	UINT Supply_RawInputData(UINT result, _In_ HRAWINPUT hRawInput, _In_ UINT uiCommand, _Out_writes_bytes_to_opt_(*pcbSize, return) LPVOID pData, _Inout_ PUINT pcbSize, _In_ UINT cbSizeHeader);
	UINT Supply_RawInputBuffer(UINT result, _Out_writes_bytes_opt_(*pcbSize) PRAWINPUT pData, _Inout_ PUINT pcbSize, _In_ UINT cbSizeHeader);

	void Supply_GetCursorPos(LPPOINT lpPoint);
	void Supply_SetCursorPos(int x, int y);
	void Supply_MouseFrameEnd(void);
	// Free-cam mouse diagnostics since the last call (then reset): see m_StatRawFrames.
	void GetMouseSourceStats(int& rawFrames, int& cursorFrames, double& rawYaw, double& cursorYaw, bool& rawRecent) {
		rawFrames = m_StatRawFrames; cursorFrames = m_StatCursorFrames;
		rawYaw = m_StatRawYaw; cursorYaw = m_StatCursorYaw; rawRecent = m_MouseInput.RawRecent;
		m_StatRawFrames = m_StatCursorFrames = 0; m_StatRawYaw = m_StatCursorYaw = 0;
	}
	void Supply_Focus(bool hasFocus);

	//

	bool GetCamResetView(void);
	double GetCamDForward(void);
	double GetCamDLeft(void);
	double GetCamDUp(void);
	double GetCamDPitch(void);
	double GetCamDYaw(void);
	double GetCamDRoll(void);
	double GetCamDFov(void);

	bool GetCameraControlMode(void);
	void SetCameraControlMode(bool enable);
	bool GetThirdPersonOrbitMode(void) const { return m_ThirdPersonOrbitMode; }
	void SetThirdPersonOrbitMode(bool enable);
	// Orbit input is fed from EXACTLY ONE place: the once-per-frame cursor-vs-center
	// delta in main.cpp's FrameStageNotify recenter block (SupplyThirdPersonOrbitPixels).
	// The raw-input / GetCursorPos hooks only BLOCK the game from seeing the mouse in
	// orbit mode -- they must not accumulate, or the same physical movement gets counted
	// once per pipe (raw + every cursor poll) and the orbit spins wildly.
	// Units are raw PIXELS (yaw sign already applied); the consumer applies its own
	// degrees-per-pixel sensitivity.
	void SupplyThirdPersonOrbitPixels(int dX, int dY);
	bool ConsumeThirdPersonOrbitDelta(double& dYawPixels, double& dPitchPixels);

	// Current free-cam speed multiplier (read for the movie director HUD).
	double GetCamSpeed(void) const { return m_CamSpeed; }
	// Same range DoCamSpeedIncrease/Decrease keep (1/256 .. 256).
	void SetCamSpeed(double value) { m_CamSpeed = value < 1.0 / 256 ? 1.0 / 256 : (value > 256 ? 256 : value); }

	// Public speed-step wrappers (one shift+scroll notch) for the movie director.
	void IncreaseCamSpeed(void) { DoCamSpeedIncrease(); }
	void DecreaseCamSpeed(void) { DoCamSpeedDecrease(); }

	// Slow/fine-movement multiplier (movie director Shift-hold). 1.0 = normal.
	double GetSlowFactor(void) const { return m_SlowFactor; }
	void SetSlowFactor(double value) { m_SlowFactor = value; }

	bool IsActive() {
		return m_Focus && !m_Dependencies->GetSuspendMirvInput() && (m_CameraControlMode || m_ThirdPersonOrbitMode);
	}

	double GetKeyboardSensitivty(void);
	void SetKeyboardSensitivity(double value);

	double GetMouseSensitivty(void);
	void SetMouseSensitivity(double value);

	// The game's own mouse aim, in degrees per mouse count (CS2: sensitivity * m_yaw / m_pitch).
	// When set (> 0) free-cam mouse look turns by exactly that per count -- the same feel as
	// aiming in game -- independent of frame rate, scaled only by the mirv_input mouse yaw/pitch
	// speed and sensitivity relative to their defaults. 0 keeps HLAE's legacy per-second scale.
	void SetGameMouseDegreesPerCount(double yaw, double pitch) { m_GameYawPerCount = yaw; m_GamePitchPerCount = pitch; }

	// Free-cam mouse from an external RAW-count source (the host's own raw-input sink): the game
	// aims by raw counts, but the free cam otherwise only sees cursor deltas, which carry Windows
	// pointer speed and acceleration. While the source is on, cursor deltas are ignored (the cursor
	// is still hidden and pinned) and Supply_RawMouseCounts turns the camera instead.
	bool WantsRawMouse() const { return m_Focus && m_CameraControlMode && !m_Dependencies->GetSuspendMirvInput(); }
	void SetExternalRawMouse(bool on) { m_ExternalRawMouse = on; }
	void Supply_RawMouseCounts(long long dX, long long dY);

	double KeyboardForwardSpeed_get(void);
	void KeyboardForwardSpeed_set(double value);
	
	double KeyboardBackwardSpeed_get(void);
	void KeyboardBackwardSpeed_set(double value);
	
	double KeyboardLeftSpeed_get(void);
	void KeyboardLeftSpeed_set(double value);
	
	double KeyboardRightSpeed_get(void);
	void KeyboardRightSpeed_set(double value);
	
	double KeyboardUpSpeed_get(void);
	void KeyboardUpSpeed_set(double value);
	
	double KeyboardDownSpeed_get(void);
	void KeyboardDownSpeed_set(double value);
	
	double KeyboardPitchPositiveSpeed_get(void);
	void KeyboardPitchPositiveSpeed_set(double value);
	
	double KeyboardPitchNegativeSpeed_get(void);
	void KeyboardPitchNegativeSpeed_set(double value);
	
	double KeyboardYawPositiveSpeed_get(void);
	void KeyboardYawPositiveSpeed_set(double value);
	
	double KeyboardYawNegativeSpeed_get(void);
	void KeyboardYawNegativeSpeed_set(double value);
	
	double KeyboardRollPositiveSpeed_get(void);
	void KeyboardRollPositiveSpeed_set(double value);
	
	double KeyboardRollNegativeSpeed_get(void);
	void KeyboardRollNegativeSpeed_set(double value);
	
	double KeyboardFovPositiveSpeed_get(void);
	void KeyboardFovPositiveSpeed_set(double value);
	
	double KeyboardFovNegativeSpeed_get(void);
	void KeyboardFovNegativeSpeed_set(double value);
	
	double MouseYawSpeed_get(void);
	void MouseYawSpeed_set(double value);
	
	double MousePitchSpeed_get(void);
	void MousePitchSpeed_set(double value);

	double MouseFovPositiveSpeed_get(void);
	void MouseFovPositiveSpeed_set(double value);

	double MouseFovNegativeSpeed_get(void);
	void MouseFovNegativeSpeed_set(double value);


	bool GetEnableMouseMove() { return m_MMove; }
	void SetEnableMouseMove(bool value) { m_MMove = value; }

	double GetMouseUpSpeed() { return m_MouseUpSpeed; }
	double GetMouseDownSpeed() { return m_MouseDownSpeed; }
	double GetMouseLeftSpeed() { return m_MouseLeftSpeed;  }
	double GetMouseRightSpeed() { return m_MouseRightSpeed;  }
	double GetMouseForwardSpeed() { return m_MouseForwardSpeed; }
	double GetMouseBackwardSpeed() { return m_MouseBackwardSpeed; }

	void SetMouseUpSpeed(double value) { m_MouseUpSpeed = value; }
	void SetMouseDownSpeed(double value) { m_MouseDownSpeed = value; }
	void SetMouseLeftSpeed(double value) { m_MouseLeftSpeed = value;  }
	void SetMouseRightSpeed(double value) { m_MouseRightSpeed = value; }
	void SetMouseForwardSpeed(double value) { m_MouseForwardSpeed = value; }
	void SetMouseBackwardSpeed(double value) { m_MouseBackwardSpeed = value; }

	OffsetMode GetOffsetMode() { return m_OffsetMode; }
	void SetOffsetMode(OffsetMode value) { m_OffsetMode = value; }

	double GetCamSpeedBasis() { return m_CamSpeedBasis; }
	void SetCamSpeedBasis(double value) {
		if(value < 1) value = 1;
		else if(8 < value) value = 8;
		m_CamSpeedBasis = value;
	}

	void SetTx(float value)
	{
		m_SetTx = true;
		m_SetTxValue = value;
	}

	void SetTy(float value)
	{
		m_SetTy = true;
		m_SetTyValue = value;
	}

	void SetTz(float value)
	{
		m_SetTz = true;
		m_SetTzValue = value;
	}

	void SetRx(float value)
	{
		m_SetRx = true;
		m_SetRxValue = value;
	}

	void SetRy(float value)
	{
		m_SetRy = true;
		m_SetRyValue = value;
	}

	void SetRz(float value)
	{
		m_SetRz = true;
		m_SetRzValue = value;
	}

	void SetFov(float value)
	{
		m_SetFov = true;
		m_SetFovValue = value;
	}

	// Discrete FOV step (mouse-wheel zoom): nudge the free-cam FOV by dFovDegrees and let it
	// persist. Builds on the absolute SetFov override (applied in Override() and fed back into
	// m_InputFov), so each call is one clean step independent of frame time -- unlike the
	// dT-scaled mouse-move FOV accumulator. Accumulates correctly across multiple wheel notches
	// in the same frame by reading the still-pending SetFov value when one is queued.
	void NudgeFov(double dFovDegrees)
	{
		double base = m_SetFov ? (double)m_SetFovValue : m_InputFov;
		SetFov((float)LimitFov(base + dFovDegrees));
	}

	void ConCommand(advancedfx::ICommandArgs * args);

private:
	class Mem
	{
	public:
		void Connect(MirvInput * input) {
			m_Input = input;
		}

		void Console(advancedfx::ICommandArgs * args);

	private:
		struct CData
		{
			double Origin[3];
			double Angles[3];
			double Fov;

			CData()
			{

			}

			CData(double x, double y, double z, double  yPitch, double xRoll, double zYaw, double fov)
			{
				Origin[0] = x;
				Origin[1] = y;
				Origin[2] = z;
				Angles[0] = yPitch;
				Angles[1] = xRoll;
				Angles[2] = zYaw;
				Fov = fov;
			}
		};

		// this is not unicode aware, but whatever:
		struct ci_less
		{
			struct ci_less_char
			{
				bool operator() (const unsigned char& c1, const unsigned char& c2) const {
					return tolower(c1) < tolower(c2);
				}
			};
			bool operator() (const std::string & s1, const std::string & s2) const {
				return std::lexicographical_compare(
					s1.begin(), s1.end(),
					s2.begin(), s2.end(),
					ci_less_char());
			}
		};

		MirvInput * m_Input;

		std::map<std::string, CData, ci_less> m_Map;

		bool Save(wchar_t const * fileName);

		bool Load(wchar_t const * fileName);

	};

	static const double m_CamSpeedFacMove;
	static const double m_CamSpeedFacRotate;
	static const double m_CamSpeedFacZoom;

	IMirvInputDependencies * m_Dependencies;
	
	bool m_FirstGetCursorPos;
	double m_MouseSens;
	double m_KeyboardSens;

	double m_KeyboardForwardSpeed;
	double m_KeyboardBackwardSpeed;
	double m_KeyboardLeftSpeed;
	double m_KeyboardRightSpeed;
	double m_KeyboardUpSpeed;
	double m_KeyboardDownSpeed;
	double m_KeyboardPitchPositiveSpeed;
	double m_KeyboardPitchNegativeSpeed;
	double m_KeyboardYawPositiveSpeed;
	double m_KeyboardYawNegativeSpeed;
	double m_KeyboardRollPositiveSpeed;
	double m_KeyboardRollNegativeSpeed;
	double m_KeyboardFovPositiveSpeed;
	double m_KeyboardFovNegativeSpeed;
	double m_MouseYawSpeed;
	double m_MousePitchSpeed;
	double m_MouseUpSpeed;
	double m_MouseDownSpeed;
	double m_MouseForwardSpeed;
	double m_MouseBackwardSpeed;
	double m_MouseLeftSpeed;
	double m_MouseRightSpeed;
	double m_MouseFovPositiveSpeed;
	double m_MouseFovNegativeSpeed;

	bool m_CamResetView;
	double m_CamForward;
	double m_CamForwardI;
	double m_CamLeft;
	double m_CamLeftI;
	double m_CamUp;
	double m_CamUpI;
	double m_CamFov;
	double m_CamFovI;
	double m_CamPitch;
	double m_CamPitchI;
	double m_CamYaw;
	double m_CamYawI;
	double m_CamRoll;
	double m_CamRollI;
	double m_CamSpeed;
	double m_CamSpeedBasis = 2.0f;
	// Transient multiplier for "slow/fine" movement (held while Shift is down in
	// free cam). Independent of m_CamSpeed so it doesn't fight Shift+Scroll speed
	// changes. Applied in GetCamD* to movement and rotation alike.
	double m_SlowFactor = 1.0;

	struct MouseInput {
		double Forward = 0;
		double Left = 0;
		double Up = 0;
		double Pitch = 0;
		double Yaw = 0;
		double Fov = 0;
		// Game-sensitivity mouse look, in degrees this frame (never frame-time scaled).
		double LookYaw = 0, LookPitch = 0;
		bool LeftButtonDown = false;
		bool RightButtonDown = false;

		bool HasInput() {
			return Forward != 0 || Left != 0 || Up != 0 || Pitch != 0 || Yaw != 0 || Fov != 0 || LookYaw != 0 || LookPitch != 0;
		}

		void Clear() {
			Forward = 0;
			Left = 0;
			Up = 0;
			Pitch = 0;
			Yaw = 0;
			Fov = 0;
			LookYaw = 0;
			LookPitch = 0;
		}
	};

	struct MultiMouseInput {
		MouseInput Normal;
		MouseInput Raw;
		// Raw input arrived within the last RawStickyFrames frames. While it does, the cursor
		// delta (Normal) is ignored even on a frame with no raw packet: the two are in different
		// units (raw counts vs Windows cursor pixels, which carry pointer speed and acceleration),
		// so falling back per frame made the free cam's sensitivity jump around.
		bool RawRecent = false;

		bool UseRaw() { return RawRecent || Raw.HasInput(); }
		double GetForward() {
			return UseRaw() ? Raw.Forward : Normal.Forward;
		}
		double GetLeft() {
			return UseRaw() ? Raw.Left : Normal.Left;
		}
		double GetUp() {
			return UseRaw() ? Raw.Up : Normal.Up;
		}
		double GetPitch() {
			return UseRaw() ? Raw.Pitch : Normal.Pitch;
		}
		double GetYaw() {
			return UseRaw() ? Raw.Yaw : Normal.Yaw;
		}
		double GetFov() {
			return UseRaw() ? Raw.Fov : Normal.Fov;
		}
		double GetLookYaw() { return UseRaw() ? Raw.LookYaw : Normal.LookYaw; }
		double GetLookPitch() { return UseRaw() ? Raw.LookPitch : Normal.LookPitch; }

		bool HasInput() {
			return Normal.HasInput() || Raw.HasInput();
		}

		void Clear() {
			Normal.Clear();
			Raw.Clear();
		}
	} m_MouseInput;
	static constexpr int RawStickyFrames = 240;
	double m_GameYawPerCount = 0, m_GamePitchPerCount = 0;
	bool GameMouseLook() const { return m_GameYawPerCount > 0 && m_GamePitchPerCount > 0; }
	// Adds one mouse look delta (counts or pixels) through whichever scale is in use.
	void AddMouseLook(MouseInput& into, double dX, double dY);
	// One mouse move: look, or (mirv_input mmove with a button held) strafe / dolly / crane.
	void AddMouseMove(MouseInput& into, double dX, double dY, bool leftDown, bool rightDown);
	bool m_ExternalRawMouse = false;
	// Where the free cam pins the hidden cursor each frame: the centre of the game window.
	void PinCursor();
	int m_RawRecentFrames = 0;
	// Free-cam mouse diagnostics, since the last GetMouseSourceStats: frames turned by raw input,
	// frames turned by cursor deltas, and the yaw each source supplied (degrees).
	int m_StatRawFrames = 0, m_StatCursorFrames = 0;
	double m_StatRawYaw = 0, m_StatCursorYaw = 0;
	bool m_MNormalLeftButtonWasDown = false;
	bool m_MNormalRightButtonWasDown = false;

	bool m_MMove = false;
	bool m_CameraControlMode;
	bool m_ThirdPersonOrbitMode = false;
	double m_ThirdPersonOrbitYaw = 0.0;
	double m_ThirdPersonOrbitPitch = 0.0;
	bool m_Focus;
	bool m_IgnoreKeyUp;
	bool m_IgnoreNextKey;
	LONG m_LastCursorX;
	LONG m_LastCursorY;

	bool m_SetTx = false;
	bool m_SetTy = false;
	bool m_SetTz = false;
	bool m_SetRx = false;
	bool m_SetRy = false;
	bool m_SetRz = false;
	bool m_SetFov = false;

	float m_SetTxValue;
	float m_SetTyValue;
	float m_SetTzValue;
	float m_SetRxValue;
	float m_SetRyValue;
	float m_SetRzValue;
	float m_SetFovValue;

	OffsetMode m_OffsetMode = OffsetMode_Last;

	bool m_InputOn = false;
	double m_InputX = 0;
	double m_InputY = 0;
	double m_InputZ = 0;
	double m_InputRx = 0;
	double m_InputRy = 0;
	double m_InputRz = 0;
	double m_InputFov = 90.0;

	Mem m_Mem;

	// BEGIN Smooth feature related

	bool m_SmoothEnabled = false;
	bool m_SmoothWasEnabled = false;

	double m_LastX = 0;
	double m_LastY = 0;
	double m_LastZ = 0;

	Afx::Math::Quaternion m_LastOutQuat;

	double m_LastFov = 90.0;

	double m_HalfTimeVec = 0.5;
	double m_HalfTimeAng = 0.5;
	double m_HalfTimeFov = 0.5;

	bool m_RotShortestPath = true;

	// END Smooth feature related

	bool m_RotLocalSpace = false;

	void DoCamSpeedDecrease(void);
	void DoCamSpeedIncrease(void);

	double CalcExpSmooth(double deltaT, double oldVal, double newVal);
	double CalcDeltaExpSmooth(double deltaT, double deltaVal);

	void ProcessRawInputData(PRAWINPUT pData);

	double LimitFov(double fov);
};
