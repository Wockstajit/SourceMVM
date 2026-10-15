#pragma once

#define _USE_MATH_DEFINES
#include <math.h>

#include "../../../Shared/MirvInput.h"
#include "../Filmmaker.h"
#include "../MouseCapture.h"
#include "../../WrpConsole.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/igameuiservice.h"

extern SOURCESDK::CS2::IGameUIService * g_pGameUIService;

class MirvInputEx : private IMirvInputDependencies
{
public:
	MirvInputEx() {
		LastWidth = 1920;
		LastHeight = 1080;

		LastCameraOrigin[0] = 0.0;
		LastCameraOrigin[1] = 0.0;
		LastCameraOrigin[2] = 0.0;
		LastCameraAngles[0] = 0.0;
		LastCameraAngles[1] = 0.0;
		LastCameraAngles[2] = 0.0;
		LastCameraFov = 90.0;

		GameCameraOrigin[0] = 0.0;
		GameCameraOrigin[1] = 0.0;
		GameCameraOrigin[2] = 0.0;
		GameCameraAngles[0] = 0.0;
		GameCameraAngles[1] = 0.0;
		GameCameraAngles[2] = 0.0;
		GameCameraFov = 90.0;

		LastFrameTime = 0;

		m_MirvInput = new MirvInput(this);
	}

	~MirvInputEx() {
		delete m_MirvInput;
	}

	MirvInput * m_MirvInput;

	double LastCameraOrigin[3];
	double LastCameraAngles[3];
	double LastCameraFov;

	double GameCameraOrigin[3];
	double GameCameraAngles[3];
	double GameCameraFov;

	double LastFrameTime;

	int LastWidth;
	int LastHeight;

private:
	virtual bool GetSuspendMirvInput() override {
		return (g_pGameUIService && g_pGameUIService->Con_IsVisible())
			|| Filmmaker::MouseCapture_IsFree()
			|| Filmmaker::MarkerMenu_WantsCursor()
			|| Filmmaker::CameraTimeline_WantsCursor()
			|| Filmmaker::GraphEditorExperiment_WantsCursor()
			|| Filmmaker::CameraEditor_CustomizeModalOpen()
			// The LIVE Insert panel, and the reason its buttons and tabs were dead while its
			// sliders worked. WM_LBUTTONDOWN records the cursor probe FIRST (that is what drives
			// the sliders, which are read from our own OS-cursor pipe) and only then offers the
			// event to MirvInput -- and an unsuspended MirvInput CONSUMES it, so the click never
			// reaches Panorama and every real button/tab is inert. In a demo the timeline's
			// WantsCursor above covers this; on a live map nothing did, because the live panel
			// owns its own cursor flag and nothing else on a live map asked for it.
			|| Filmmaker::LiveFxMenu_WantsGameMouseReleased();
	}

	virtual void GetLastCameraData(double & x, double & y, double & z, double & rX, double & rY, double & rZ, double & fov) override {
		x = LastCameraOrigin[0];
		y = LastCameraOrigin[1];
		z = LastCameraOrigin[2];
		rX = LastCameraAngles[0];
		rY = LastCameraAngles[1];
		rZ = LastCameraAngles[2];
		fov = LastCameraFov;
	}

	virtual void GetGameCameraData(double & x, double & y, double & z, double & rX, double & rY, double & rZ, double & fov) override {
		x = GameCameraOrigin[0];
		y = GameCameraOrigin[1];
		z = GameCameraOrigin[2];
		rX = GameCameraAngles[0];
		rY = GameCameraAngles[1];
		rZ = GameCameraAngles[2];
		fov = GameCameraFov;
	}

	virtual double GetInverseScaledFov(double fov) override {
		return ScaleFovInverse(LastWidth, LastHeight, fov);
	}

private:

	double ScaleFovInverse(double width, double height, double fov) {
		if (!height) return fov;

		double engineAspectRatio = width / height;
		double defaultAscpectRatio = 4.0 / 3.0;
		double ratio = engineAspectRatio / defaultAscpectRatio;
		double t = tan(0.5 * fov * (2.0 * M_PI / 360.0));
		double halfAngle = atan(t / ratio);
		return 2.0 * halfAngle / (2.0 * M_PI / 360.0);
	}

};

extern MirvInputEx g_MirvInputEx;

float GetLastCameraFov();

CON_COMMAND(mirv_input, "Input mode configuration.")
{
	g_MirvInputEx.m_MirvInput->ConCommand(args);
}
