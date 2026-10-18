#include "stdafx.h"

#include "addresses.h"
#include "CampathDrawer.h"
#include "ClientEntitySystem.h"
#include "GameEvents.h"
#include "hlaeFolder.h"
#include "RenderServiceHooks.h"
#include "RenderSystemDX11Hooks.h"
#include "WrpConsole.h"
#include "AfxHookSource2Rs.h"
#include "ReShadeAdvancedfx.h"
#include "CamIO.h"
#include "ViewModel.h"
#include "Globals.h"
#include "DeathMsg.h"
#include "Filmmaker/Filmmaker.h"
#include "Filmmaker/Debug/MvmDebugLog.h"
#include "Filmmaker/Community/BugReport.h"
#include "Filmmaker/Community/CrashReport.h"
#include "Filmmaker/Platform/RawMouse.h"
#include "Filmmaker/Panorama/ConfigHud.h" // ConfigHud_Enabled: is the workspace preview owning the view?
#include "Filmmaker/Render/RenderEngineSize.h"
#include "Filmmaker/Movie/CameraBridge.h"
#include "Filmmaker/Movie/DeathAnimations.h"
#include "Filmmaker/EasterEggs/EasterEggs.h"
#include "Filmmaker/Movie/NervousSystem.h"
#include "Filmmaker/Movie/FollowCamera.h"
#include "Filmmaker/Movie/SceneRig.h"      // scene object rigs: render-time resample, Look through
#include "Filmmaker/Movie/FollowCameraMath.h"
#include "Filmmaker/Movie/MovieMode.h" // MovieMode_NativeCameraRoll: X / Z roll on CS2's free cam
#include "Filmmaker/Movie/ViewFx.h"
#include "Filmmaker/Movie/CameraShakeStack.h" // handheld / recoil shake layers
#include "Filmmaker/Movie/CamPathShake.h"     // per-keyframe camera-path shake (frame shaker)
#include "Filmmaker/Movie/ParticleFx.h"       // level-init handle-cache invalidation
#include "Filmmaker/Movie/RagdollWeaponArmPhysics.h" // physical death-weapon recoil stage bridge
#include "Filmmaker/Config/EffectsPersistence.h" // level-init: keep or drop EFFECTS state
#include "Filmmaker/Movie/SmoothView.h"       // Smooth View 2 camera module
#include "Filmmaker/Movie/ViewFxVm.h"         // true viewmodel angular steering
#include "Filmmaker/Movie/CameraOwnership.h"    // who claimed the pose + shake-mode precedence
#include "Filmmaker/Panorama/AimCrosshairHud.h"
#include "Filmmaker/Movie/CameraRuntimeInternal.h"
#include "Filmmaker/MouseCapture.h"
#include "WindowInputHooks.h"
#include "UiInputGate.h"
#include "ConsoleCapture.h"
#include "ReplaceName.h"
#include "SchemaSystem.h"
#include "SceneSystem.h"
#include "MirvCommands.h"
#include "MirvColors.h"
#include "MirvFix.h"
#include "MirvTime.h"

#include "../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../Deps/release/prop/AfxHookSource/SourceInterfaces.h"
#include "../Deps/release/prop/cs2/Source2Client.h"
#include "../Deps/release/prop/cs2/sdk_src/public/tier1/convar.h"
#include "../Deps/release/prop/cs2/sdk_src/public/filesystem.h"
#include "../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../Deps/release/prop/cs2/sdk_src/public/icvar.h"
#include "../Deps/release/prop/cs2/sdk_src/public/igameuiservice.h"

#include "../Shared/AfxCommandLine.h"
#include "../Shared/AfxConsole.h"
#include "../Shared/AfxDetours.h"
#include "../Shared/ConsolePrinter.h"
#include "../Shared/StringTools.h"
#include "../Shared/binutils.h"
#include "../Shared/CommandSystem.h"
#include "../Shared/GrowingBufferPoolThreadSafe.h"
#include "../Shared/ThreadPool.h"
#include "../Shared/MirvCamIO.h"
#include "../Shared/MirvCampath.h"
#include "../Shared/MirvInput.h"
#include "../Shared/MirvSkip.h"

#include "../Deps/release/Detours/src/detours.h"

#define _USE_MATH_DEFINES
#include <math.h>

#include <stdlib.h>
#include <sstream>
#include <mutex>

HMODULE g_h_engine2Dll = 0;
HMODULE g_H_ClientDll = 0;
HMODULE g_H_SchemaSystem = 0;
HMODULE g_H_ResourceSystemDll = 0;
HMODULE g_H_FileSystem_stdio = 0;

SOURCESDK::CS2::IFileSystem* g_pFileSystem = nullptr;

advancedfx::CCommandLine  * g_CommandLine = nullptr;

typedef void (__fastcall * AddSearchPath_t)(void* This, const char *pPath, const char *pathID, int addType, int priority, int unk );
AddSearchPath_t org_AddSearchPath = nullptr;

void new_AddSearchPath(void* This, const char *pPath, const char *pathID, int addType, int priority, int unk) {
	if (0 == strcmp(pathID, "USRLOCAL")) {
		const wchar_t* USRLOCALCSGO = _wgetenv(L"USRLOCALCSGO");
		if (nullptr != USRLOCALCSGO) {
			std::string USRLOCALCSGO_copy = "";
			WideStringToUTF8String(USRLOCALCSGO, USRLOCALCSGO_copy);
			if (USRLOCALCSGO_copy.size() > 0) {
				// Register the FX pack FIRST (keeps its override priority), then FALL THROUGH so the
				// engine's real USRLOCAL path is registered too. This used to REPLACE the path, which
				// silently broke every USRLOCAL-relative lookup -- most visibly `replays/<demo>.dem.info`,
				// so demos played with no round markers, no kill/death icons, and "Round: 0"
				// (verified 2026-08-28: vanilla and no-FX-mount launches showed all of them, and
				// exposing replays inside the mount restored them with the mount active).
				org_AddSearchPath(This, USRLOCALCSGO_copy.c_str(), pathID, addType, priority, unk);
			}
		}
	}

	return org_AddSearchPath(This, pPath, pathID, addType, priority, unk);
}

FovScaling GetDefaultFovScaling() {
	return FovScaling_AlienSwarm;
}

void PrintInfo() {
	advancedfx::Message(
		"|" "\n"
		"| AfxHookSource2 (" __DATE__ " " __TIME__ ")" "\n"
		"| https://advancedfx.org/" "\n"
		"|" "\n"
	);
}

void * g_pGameResourceService = nullptr;

/*typedef void (*Unknown_ExecuteClientCommandFromNetChan_t)(void * Ecx, void * Edx, void *R8);
Unknown_ExecuteClientCommandFromNetChan_t g_Old_Unknown_ExecuteClientCommandFromNetChan = nullptr;
void New_Unknown_ExecuteClientCommandFromNetChan(void * Ecx, void * Edx, SOURCESDK::CS2::CCommand *r8Command) {
	//for(int i = 0; i < r8Command->ArgC(); i++) {
	//	advancedfx::Message("Command %i: %s\n",i,r8Command->Arg(i));
	//}
	if(0 == stricmp("connect",r8Command->Arg(0))) {
		if(IDYES != MessageBoxA(0,"YOU ARE TRYING TO CONNECT TO A SERVER - THIS WILL GET YOU VAC BANNED.\nARE YOU SURE?", "HLAE WARNING", MB_YESNOCANCEL|MB_ICONHAND|MB_DEFBUTTON2))
			return;
	}
	g_Old_Unknown_ExecuteClientCommandFromNetChan(Ecx, Edx, r8Command);
}*/


void HookEngineDll(HMODULE engineDll) {

	static bool bFirstCall = true;
	if(!bFirstCall) return;
	bFirstCall = false;
	
	// Unknown_ExecuteClientCommandFromNetChan: // Last checked 2023-07-19
	/*
		The function we hook is called in the function referencing the string
		"Client %s(%d) tried to execute command \"%s\" before being fully connected.\n"
		or the other function referencing "SV: Cheat command '%s' ignored.\n"
		as follows:

		loc_1801842F0:
		mov     r8, rdi
		lea     rdx, [rsp+0D68h+var_D38]
		lea     rcx, [rsp+0D68h+arg_18]
		call    sub_180329DD0 <---
		lea     rcx, [rsp+0D68h+var_D30]
		call    sub_180183A60	
	*/
	/*{
		Afx::BinUtils::ImageSectionsReader sections((HMODULE)engineDll);
		Afx::BinUtils::MemRange textRange = sections.GetMemRange();
		Afx::BinUtils::MemRange result = FindPatternString(textRange, "4C 8B D1 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 13 48 8B 01 4D 8B C8 4C 8B C2 49 8B 12 48 FF A0 90 00 00 00 C3");
		if (!result.IsEmpty()) {
			g_Old_Unknown_ExecuteClientCommandFromNetChan = (Unknown_ExecuteClientCommandFromNetChan_t)result.Start;	
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(&(PVOID&)g_Old_Unknown_ExecuteClientCommandFromNetChan, New_Unknown_ExecuteClientCommandFromNetChan);
			if(NO_ERROR != DetourTransactionCommit())
				ErrorBox("Failed to detour Unknown_ExecuteClientCommandFromNetChan.");
		}
		else
			ErrorBox(MkErrStr(__FILE__, __LINE__));
	}*/
}

typedef void (__fastcall * HostStateRequest_Start_t)(void * This);
HostStateRequest_Start_t g_Old_HostStateRequest_Start = nullptr;
void __fastcall New_HostStateRequest_Start(void * This) {
	if(4 == *(int *)This) {
		// "HostStateRequest::Start(HSR_QUIT)\n"
		AfxStreams_ShutDown();
	}
	g_Old_HostStateRequest_Start(This);
}

void Hook_Engine__HostStateRequest_Start() {
	static bool bFirstRun = true;
	if(bFirstRun) {
		bFirstRun = false;
		if(AFXADDR_GET(cs2_engine_HostStateRequest_Start)) {
			g_Old_HostStateRequest_Start = (HostStateRequest_Start_t)AFXADDR_GET(cs2_engine_HostStateRequest_Start);
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(&(PVOID&)g_Old_HostStateRequest_Start,New_HostStateRequest_Start);
			if(NO_ERROR != DetourTransactionCommit()) ErrorBox(MkErrStr(__FILE__, __LINE__));
		}
	}
}


SOURCESDK::CS2::ISource2EngineToClient * g_pEngineToClient = nullptr;

////////////////////////////////////////////////////////////////////////////////

//TODO: Some bellow here might be not accurate yet.

typedef void * Cs2Gloabls_t;
Cs2Gloabls_t g_pGlobals = nullptr;

DWORD g_SleepMs = 0;

CON_COMMAND(__mirv_sleep,"") {
if (2<= args->ArgC()) g_SleepMs = strtoul(args->ArgV(1),nullptr,10);
}

CON_COMMAND(__mirv_info,"") {
	PrintInfo();
}

CON_COMMAND(__mirv_test,"") {
	static int offset = 13;

	if(2 <= args->ArgC()) offset = atoi(args->ArgV(1));

	advancedfx::Message("g_pGlobals[%i]: int: %i , float: %f\n",offset,(g_pGlobals ? *(int *)((unsigned char *)g_pGlobals +offset*4) : 0),(g_pGlobals ? *(float *)((unsigned char *)g_pGlobals +offset*4) : 0));
}

extern const char * GetStringForSymbol(int value);

CON_COMMAND(__mirv_get_string_for_symbol,"") {
	if (2<= args->ArgC()) {
		advancedfx::Message("%i: %s\n",atoi(args->ArgV(1)),GetStringForSymbol(atoi(args->ArgV(1))));
	}
}

CON_COMMAND(__mirv_find_vtable,"") {
	if(args->ArgC()<5) return;

	HMODULE hModule = GetModuleHandleA(args->ArgV(1));
	size_t addr = hModule != 0 ? Afx::BinUtils::FindClassVtable(hModule,args->ArgV(2),atoi(args->ArgV(3)),atoi(args->ArgV(4))) : 0;
	DWORD offset = (DWORD)(addr-(size_t)hModule);
	advancedfx::Message("Result: 0x%016llx (Offset: 0x%08x)\n",addr,offset);
}

/*CON_COMMAND(mirv_exec,"") {
    std::ostringstream oss;

	for(int i=1; i < args->ArgC(); i++) {
		if(1 < i ) oss << " ";
		std::string strArg(args->ArgV(i));

		// Escape quotes:
		for (size_t pos = strArg.find('\"', 0); std::string::npos != pos; pos = strArg.find('\"', pos + 2 ) ) strArg.replace(pos, 1, "\\\"");

		oss << "\"" << strArg << "\"";
	}

    if(g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, oss.str().c_str(), false);	
}*/

CON_COMMAND(mirv_loadlibrary, "Load a DLL.")
{
	int argc = args->ArgC();

	if (2 <= argc)
	{
		char const * cmd1 = args->ArgV(1);

		std::wstring wCmd1;
		if (UTF8StringToWideString(cmd1, wCmd1))
		{

			if (0 != LoadLibraryW(wCmd1.c_str()))
			{
				advancedfx::Message("LoadLibraryA OK.\n");
			}
			else
			{
				advancedfx::Message("LoadLibraryA failed.\n");
			}
		}
		else
		{
			advancedfx::Message("Failed to convert \"%s\" from UFT8 to UTF-16.\n", cmd1);
		}

		return;
	}

	advancedfx::Message(
		"mirv_loadlibrary <sDllFilePath> - Load DLL at given path.\n"
	);
}

////////////////////////////////////////////////////////////////////////////////

SOURCESDK::CS2::IGameUIService * g_pGameUIService = nullptr;
bool AfxConsoleVisible() { return g_pGameUIService && g_pGameUIService->Con_IsVisible(); }

MirvInputEx g_MirvInputEx;

float GetLastCameraFov() {
	return (float)g_MirvInputEx.LastCameraFov;
}


////////////////////////////////////////////////////////////////////////////////

CamPath g_CamPath;

class CMirvCampath_Time : public IMirvCampath_Time
{
public:
	virtual double GetTime() {
		// Can be paused time, we don't support that currently.
		return g_MirvTime.curtime_get();
	}
	virtual double GetCurTime() {
		return g_MirvTime.curtime_get();
	}
	virtual bool GetCurrentDemoTick(int& outTick) {
		return g_MirvTime.GetCurrentDemoTick(outTick);
	}
	virtual bool GetCurrentDemoTime(double& outDemoTime) {
		return g_MirvTime.GetCurrentDemoTime(outDemoTime);
	}
	virtual bool GetDemoTickFromDemoTime(double curTime, double time, int& outTick) {
		outTick = (int)round(time / g_MirvTime.interval_per_tick_get());
		return true;
	}
	virtual bool GetDemoTimeFromClientTime(double curTime, double time, double& outDemoTime) {
		double current_demo_time;
		if(GetCurrentDemoTime(current_demo_time)) {
			outDemoTime = time - (curTime - current_demo_time);
			return true;
		}
		return false;
	}
    virtual bool GetDemoTickFromClientTime(double curTime, double targetTime, int& outTick)
    {
        double demoTime;
        return GetDemoTimeFromClientTime(curTime, targetTime, demoTime) && GetDemoTickFromDemoTime(curTime, demoTime, outTick);
    }
} g_MirvCampath_Time;

class CMirvCampath_Camera : public IMirvCampath_Camera
{
public:
	virtual SMirvCameraValue GetCamera() {
		return SMirvCameraValue(			
			g_MirvInputEx.LastCameraOrigin[0],
			g_MirvInputEx.LastCameraOrigin[1],
			g_MirvInputEx.LastCameraOrigin[2],
			g_MirvInputEx.LastCameraAngles[0],
			g_MirvInputEx.LastCameraAngles[1],
			g_MirvInputEx.LastCameraAngles[2],
			g_MirvInputEx.LastCameraFov
		);
	}
} g_MirvCampath_Camera;

class CMirvCampath_Drawer : public IMirvCampath_Drawer
{
public:
	virtual bool GetEnabled() {
		return g_CampathDrawer.Draw_get();
	}
	virtual void SetEnabled(bool value) {
		g_CampathDrawer.Draw_set(value);
	}
	virtual bool GetDrawKeyframeAxis() {
		return g_CampathDrawer.GetDrawKeyframeAxis();
	}
	virtual void SetDrawKeyframeAxis(bool value) {
		g_CampathDrawer.SetDrawKeyframeAxis(value);
	}
	virtual bool GetDrawKeyframeCam() {
		return g_CampathDrawer.GetDrawKeyframeCam();
	}
	virtual void SetDrawKeyframeCam(bool value) {
		g_CampathDrawer.SetDrawKeyframeCam(value);
	}

	virtual float GetDrawKeyframeIndex() { return g_CampathDrawer.GetDrawKeyframeIndex(); }
	virtual void SetDrawKeyframeIndex(float value) { g_CampathDrawer.SetDrawKeyframeIndex(value); }

} g_MirvCampath_Drawer;

CON_COMMAND(mirv_campath, "camera paths")
{
	if (nullptr == g_pGlobals)
	{
		advancedfx::Warning("Error: Hooks not installed.\n");
		return;
	}

	MirvCampath_ConCommand(args, advancedfx::Message, advancedfx::Warning, &g_CamPath, &g_MirvCampath_Time, &g_MirvCampath_Camera, &g_MirvCampath_Drawer);
}

double MirvCamIO_GetTimeFn(void) {
	return g_MirvTime.curtime_get();
}

CON_COMMAND(mirv_camio, "New camera motion data import / export.") {
	g_S2CamIO.Console_CamIO(args);
}


static bool g_bViewOverriden = false;
static float g_fFovOverride = 90.0f;
static float * g_pFov = nullptr;
int g_iWidth = 1920;
int g_iHeight = 1080;
SOURCESDK::VMatrix g_WorldToScreenMatrix;

extern bool g_b_on_c_view_render_setup_view;

extern bool MirvFovOverride(float &fov);

namespace {

// CViewRender::SetUpView has one optional demo-view branch after the filmmaker trampoline.
// Smooth View must consume the pose at the branch JOIN, not the pose before that branch:
// cl_demoviewoverride can replace origin/angles after our first callback.  Keep only the
// per-call facts that cannot be recovered from CViewSetup at the join.
struct PendingPostDemoView {
	void* setup = nullptr;
	float curTime = 0.0f;
	bool povViewForAim = false;
	bool valid = false;
};

thread_local PendingPostDemoView g_PendingPostDemoView;
// mvm_debug: the pose this frame's setup WROTE (after every shake), to compare with what the engine
// hands the post-demo step -- a difference means a later pass replaced our view.
thread_local struct { bool valid; bool pathShake; float origin[3], angles[3]; } g_WrittenView = {};

}

bool CS2_Client_CSetupView_Trampoline_IsPlayingDemo(void *ThisCViewSetup) {
	if(!g_pEngineToClient) return false;

	bool originOrAnglesOverriden = false;

	// WHICH system claimed the pose, not just whether one did -- see CameraOwnership.h for why
	// that distinction matters and for the load-bearing precedence between the flags. Each stage
	// below sets its own flag alongside originOrAnglesOverriden.
	Filmmaker::CameraOwnership owner;

	float curTime = g_MirvTime.curtime_get(); //TODO: + m_PausedTime
	float absTime = g_MirvTime.absoluteframetime_get();

	// Wall-clock frame delta for free-cam input. The engine's absoluteframetime
	// (absTime) collapses to ~0 while the demo is PAUSED, which would zero out
	// every WASD/mouse/FOV delta in MirvInput::Override() and freeze the free
	// camera. We instead drive the camera input from a real-time clock so it
	// keeps moving while paused. This is ONLY the camera-input time base; it does
	// not touch demo ticks/playback. Clamped so a hitch/alt-tab/long pause cannot
	// produce a huge camera jump. Only affects free cam (Override no-ops otherwise).
	float camDeltaT;
	{
		static LARGE_INTEGER s_qpcFreq = {};
		static LARGE_INTEGER s_qpcLast = {};
		if (s_qpcFreq.QuadPart == 0) QueryPerformanceFrequency(&s_qpcFreq);
		LARGE_INTEGER qpcNow; QueryPerformanceCounter(&qpcNow);
		if (s_qpcLast.QuadPart == 0) s_qpcLast = qpcNow;
		double dtSec = (double)(qpcNow.QuadPart - s_qpcLast.QuadPart) / (double)s_qpcFreq.QuadPart;
		s_qpcLast = qpcNow;
		if (dtSec < 0.0) dtSec = 0.0; else if (dtSec > 0.1) dtSec = 0.1;
		camDeltaT = (float)dtSec;
	}

	int *pWidth = (int*)((unsigned char *)ThisCViewSetup + 0x434);
	int *pHeight = (int*)((unsigned char *)ThisCViewSetup + 0x43C);

	float *pFov = (float*)((unsigned char *)ThisCViewSetup + 0x498);
	float *pViewOrigin = (float*)((unsigned char *)ThisCViewSetup + 0x4a0);
	float *pViewAngles = (float*)((unsigned char *)ThisCViewSetup + 0x4b8);

	int width = *pWidth;
	int height = *pHeight;
	float Tx = pViewOrigin[0];
	float Ty = pViewOrigin[1];
	float Tz = pViewOrigin[2];
	float Rx = pViewAngles[0];
	float Ry = pViewAngles[1];
	float Rz = pViewAngles[2];
	float Fov = *pFov;

	// The angles the GAME handed us, before any of our cameras or effects touch them. In a plain
	// POV spectate these ARE the spectated player's recorded aim -- the direction the bullets
	// were fired along -- which is what the true-aim crosshair has to project. Captured here
	// because every later stage (override cameras, ViewFx, shake, Smooth View) rewrites Rx/Ry.
	const float rawAimPitch = Rx, rawAimYaw = Ry;

	//advancedfx::Message("Console: %i [%ix%i]\n", (g_pGameUIService->Con_IsVisible()?1:0),width,height);

	//advancedfx::Message("%f: (%f,%f,%f) (%f,%f,%f) [%f]\n",curTime,pViewOrigin[0],pViewOrigin[1],pViewOrigin[2],pViewAngles[0],pViewAngles[1],pViewAngles[2],*pFov);

	g_MirvInputEx.GameCameraOrigin[0] = Tx;
	g_MirvInputEx.GameCameraOrigin[1] = Ty;
	g_MirvInputEx.GameCameraOrigin[2] = Tz;
	g_MirvInputEx.GameCameraAngles[0] = Rx;
	g_MirvInputEx.GameCameraAngles[1] = Ry;
	g_MirvInputEx.GameCameraAngles[2] = Rz;
	g_MirvInputEx.GameCameraFov = Fov;

	if (g_CamPath.Enabled_get() && g_CamPath.CanEval())
	{
		double campathCurTime = curTime - g_CamPath.GetOffset();
		if(g_CamPath.GetHold()) {
			if(campathCurTime > g_CamPath.GetUpperBound()) campathCurTime = g_CamPath.GetUpperBound();
			else if(campathCurTime < g_CamPath.GetLowerBound()) campathCurTime = g_CamPath.GetLowerBound();
		}

		// no extrapolation:
		if (g_CamPath.GetLowerBound() <= campathCurTime && campathCurTime <= g_CamPath.GetUpperBound())
		{
			CamPathValue val = g_CamPath.Eval(campathCurTime);
			QEulerAngles ang = val.R.ToQREulerAngles().ToQEulerAngles();

			//Tier0_Msg("================",curTime);
			//Tier0_Msg("currenTime = %f",curTime);
			//Tier0_Msg("vCp = %f %f %f\n", val.X, val.Y, val.Z);

			originOrAnglesOverriden = true;
			owner.path = true;

			Tx = (float)val.X;
			Ty = (float)val.Y;
			Tz = (float)val.Z;

			Rx = (float)ang.Pitch;
			Ry = (float)ang.Yaw;
			Rz = (float)ang.Roll;

			Fov = (float)val.Fov;
		}
	}

	if (g_S2CamIO.GetCamImport())
	{
		CamIO::CamData camData;

		if (g_S2CamIO.GetCamImport()->GetCamData(curTime, width, height, camData))
		{
			originOrAnglesOverriden = true;
			owner.import = true;

			Tx = (float)camData.XPosition;
			Ty = (float)camData.YPosition;
			Tz = (float)camData.ZPosition;
			Rx = (float)camData.YRotation;
			Ry = (float)camData.ZRotation;
			Rz = (float)camData.XRotation;
			Fov = (float)camData.Fov;
		}
	}	

	if(MirvFovOverride(Fov)) originOrAnglesOverriden = true;

	// Free-cam mouse look from the mouse's RAW counts, like in-game aim (Platform/RawMouse.h): the
	// hidden-cursor deltas it used before carry Windows pointer speed and acceleration, so no scale
	// made them match the game. The cursor path stays as the fallback until the sink is counting.
	{
		MirvInput* mirvInput = g_MirvInputEx.m_MirvInput;
		const bool wantRaw = mirvInput->WantsRawMouse();
		Filmmaker::RawMouse_Claim(Filmmaker::kRawMouseFreeCam, wantRaw);
		const bool rawOn = wantRaw && Filmmaker::RawMouse_State() > 0;
		static bool s_rawWasOn = false;
		static long long s_rawX = 0, s_rawY = 0;
		long long rawX = 0, rawY = 0;
		Filmmaker::RawMouse_Totals(rawX, rawY);
		mirvInput->SetExternalRawMouse(rawOn);
		if (rawOn && s_rawWasOn) mirvInput->Supply_RawMouseCounts(rawX - s_rawX, rawY - s_rawY);
		s_rawWasOn = rawOn;
		s_rawX = rawX; s_rawY = rawY;
	}
	const bool inputOverride = g_MirvInputEx.m_MirvInput->Override(camDeltaT, Tx,Ty,Tz,Rx,Ry,Rz,Fov);
	if(inputOverride) { originOrAnglesOverriden = true; owner.freeCam = true; }

	// Both Follow modes sample interpolated targets here. Attached rebases a rigid mount;
	// Lock-on keeps the placed position and solves its aim/filter once per rendered frame.
	{
		double ax = Tx, ay = Ty, az = Tz, ap = Rx, aya = Ry, ar = Rz, af = Fov;
		const bool attached = Filmmaker::FollowCameraRef().ViewSetupAttachOverride(curTime, ax, ay, az, ap, aya, ar, af);
		const bool lockOn = !attached && Filmmaker::FollowCameraRef().ViewSetupLockOnOverride(curTime, camDeltaT, ax, ay, az, ap, aya, ar, af);
		if (attached || lockOn) {
			Tx = (float)ax; Ty = (float)ay; Tz = (float)az;
			Rx = (float)ap; Ry = (float)aya; Rz = (float)ar; Fov = (float)af;
			originOrAnglesOverriden = true;
			if (attached) owner.attach = true;
			else owner.freeCam = true;
		}
	}

	// Scene object rigs (scene-object-rigs.md): re-read the players, weapons and grenades placed
	// objects ride at their interpolated pose, every frame; and when a camera object is looked
	// through, it is the view -- over the path, the Follow Camera and the free cam.
	{
		double sx = Tx, sy = Ty, sz = Tz, sp = Rx, syaw = Ry, sr = Rz, sf = Fov;
		bool sceneAttached = false;
		if (Filmmaker::SceneRig_ViewSetup(sx, sy, sz, sp, syaw, sr, sf, sceneAttached)) {
			Tx = (float)sx; Ty = (float)sy; Tz = (float)sz;
			Rx = (float)sp; Ry = (float)syaw; Rz = (float)sr; Fov = (float)sf;
			originOrAnglesOverriden = true;
			owner.sceneCamera = true;
			owner.sceneCameraAttached = sceneAttached;
		}
	}

	// CS2's own free cam has no roll; held X / Z adds one on top (MovieMode). Only while none of
	// our cameras owns the pose -- they carry their own roll.
	if (!owner.Any()) {
		const float nativeRoll = Filmmaker::MovieMode_NativeCameraRoll();
		if (nativeRoll != 0.f) {
			Rz += nativeRoll;
			originOrAnglesOverriden = true;
		}
		// Ctrl+wheel zoom in one of CS2's own cameras: it lasts only while that camera does, so
		// first person comes back at the game's FOV (MovieMode::ApplyNativeCameraControls).
		float nativeFov = 0.f;
		if (Filmmaker::MovieMode_NativeCameraFov(nativeFov) && !Filmmaker::MirvFov_IsZoomedFov((float)g_MirvInputEx.GameCameraFov)) {
			Fov = nativeFov;
			originOrAnglesOverriden = true;
		}
	}

	if(g_b_on_c_view_render_setup_view) {
		AfxHookSourceRsView currentView = {Tx,Ty,Tz,Rx,Ry,Rz,Fov};
		AfxHookSourceRsView gameView = {(float)g_MirvInputEx.GameCameraOrigin[0],(float)g_MirvInputEx.GameCameraOrigin[1],(float)g_MirvInputEx.GameCameraOrigin[2],(float)g_MirvInputEx.GameCameraAngles[0],(float)g_MirvInputEx.GameCameraAngles[1],(float)g_MirvInputEx.GameCameraAngles[2],(float)g_MirvInputEx.GameCameraFov};
		AfxHookSourceRsView lastView = {(float)g_MirvInputEx.LastCameraOrigin[0],(float)g_MirvInputEx.LastCameraOrigin[1],(float)g_MirvInputEx.LastCameraOrigin[2],(float)g_MirvInputEx.LastCameraAngles[0],(float)g_MirvInputEx.LastCameraAngles[1],(float)g_MirvInputEx.LastCameraAngles[2],(float)g_MirvInputEx.LastCameraFov};
		if(AfxHookSource2Rs_OnCViewRenderSetupView(
			curTime, absTime, (float)g_MirvInputEx.LastFrameTime,
			currentView, gameView, lastView,
			width,height
		)) {
			Tx = currentView.x;
			Ty = currentView.y;
			Tz = currentView.z;
			Rx = currentView.rx;
			Ry = currentView.ry;
			Rz = currentView.rz;
			Fov = currentView.fov;
			originOrAnglesOverriden = true;
			owner.other = true;
		}
	}

	if (g_S2CamIO.GetCamExport())
	{
		CamIO::CamData camData;

		camData.Time = curTime;
		camData.XPosition = Tx;
		camData.YPosition = Ty;
		camData.ZPosition = Tz;
		camData.YRotation = Rx;
		camData.ZRotation = Ry;
		camData.XRotation = Rz;
		camData.Fov = Fov;

		g_S2CamIO.GetCamExport()->WriteFrame(width, height, camData);
	}

	// ViewFx pose modifiers (deadzone camera lag): only while nothing else
	// already wrote this frame's pose -- i.e. plain default POV/third-person spectate, not
	// free cam, camera path, cam import, or Follow attach. Tx/Ty/Rx/Ry are still the
	// engine's own unmodified pose at this point (this is the last checkpoint before the
	// override write below), so the tracked velocity/aim reflect the spectated player's own
	// movement. Gated the SAME way on the TRACK call too, not just the deltas -- otherwise a
	// free-cam fly-by would poison the shared planar-speed estimate that weapon sway also
	// reads (ViewModel.cpp). Skipping a track is safe: TrackAndPoseDeltas treats the next
	// call after a gap as a fresh sample instead of computing a bogus spike from stale state.
	// Whether the view is still the game's own POV at this point. An override camera (free cam,
	// camera path, attach, third person) replaces the angles wholesale, so the raw angles above
	// stop being anybody's aim and the crosshair has nothing meaningful to point at.
	const bool povViewForAim = !originOrAnglesOverriden;

	if (!originOrAnglesOverriden) {
		const Filmmaker::ViewFxPoseDeltas fx = Filmmaker::ViewFxRef().TrackAndPoseDeltas(
			curTime, Tx, Ty, Rx, Ry);
		if (fx.Any()) {
			Rx = (float)(Rx + fx.pitch);
			Ry = (float)(Ry + fx.yaw);
			originOrAnglesOverriden = true;
		}
	} else {
		Filmmaker::ViewFxRef().NoteGatedByOverride(); // diagnostics: why isn't speed updating?
	}

	// Pose BEFORE the camera-shake layers. g_CurrentGameCamera below must describe the camera
	// the GAME produced, not the one we cosmetically shook: AfxGetSpectatedPawnIndex()
	// identifies the spectated player by matching a pawn's render eye against it within 0.2
	// units / 0.2 degrees, and a handheld drift of a few units is far more than enough to break
	// that match. When it breaks, the spectated player resolves to -1 and everything keyed off
	// it silently stops -- recoil never fires (its weapon_fire filter drops every event), and
	// Follow attach and third-person both require a valid spectated player.
	const float preShakeTx = Tx, preShakeTy = Ty, preShakeTz = Tz;
	const float preShakeRx = Rx, preShakeRy = Ry, preShakeRz = Rz;
	const float preShakeFov = Fov; // the path shake can add FOV too

	// Camera-shake layers (handheld / recoil -- CameraShakeStack.h). Deliberately
	// NOT gated like ViewFx above: these are additive layers meant to ride ON TOP of the
	// Follow attach preview as well as plain POV, so they run after the whole
	// override stack and decide for themselves from the mode which cameras they may touch
	// (POV + attach + third-person yes; free cam / camera path no). Always called, even in a
	// disallowed mode, so the shared pose tracker knows it was gated and treats the next
	// allowed frame as a fresh sample instead of integrating across the gap.
	Filmmaker::ShakePose handheldVmComp;
	if (Filmmaker::CameraShake_Apply(
		curTime, owner.Resolve(), Tx, Ty, Tz, Rx, Ry, Rz, Fov, &handheldVmComp))
		originOrAnglesOverriden = true;

	// Does the shake actually reach the POSE? Measured 2026-08-31: with the CONFIG panel open a
	// test kick moved the screen by 0.43 where the same test with the panel closed moved it by
	// 17.47 -- while camMode stayed Pov, modeAllowed stayed true and applyCalls kept climbing.
	// That narrows it to two possibilities and this line separates them: a non-zero delta here
	// means the stack did its job and the pose is being lost DOWNSTREAM (render/preview path);
	// a zero delta means the modules produced nothing and the cause is inside the stack.
	if (Filmmaker::MvmDebugLog_Active()) {
		const double dPos = std::fabs(Tx - preShakeTx) + std::fabs(Ty - preShakeTy)
			+ std::fabs(Tz - preShakeTz);
		const double dAng = std::fabs(Rx - preShakeRx) + std::fabs(Ry - preShakeRy)
			+ std::fabs(Rz - preShakeRz);
		static ULONGLONG s_lastMs = 0;
		const ULONGLONG nowMs = GetTickCount64();
		// Log every kick, but throttle the idle case so the file is not one line per frame.
		const bool moving = (dPos > 1e-4 || dAng > 1e-4);
		if (moving || nowMs - s_lastMs >= 1000) {
			s_lastMs = nowMs;
			Filmmaker::MvmDebugLog_LinefAlways("camera.shake.pose",
				"dPos=%.4f dAng=%.4f overridden=%d configOpen=%d",
				dPos, dAng, originOrAnglesOverriden ? 1 : 0,
				Filmmaker::ConfigHud_Enabled() ? 1 : 0);
		}
	}

	// Per-keyframe camera shake (CamPathShake.h). It gates ITSELF on whether the filmmaker dolly
	// is driving, and deliberately does NOT take owner.Resolve(): `owner.path` is set only by the
	// LEGACY campath branch above (g_CamPath), while a filmmaker path publishes its pose through
	// MirvInput and therefore resolves to FreeCam -- so passing the mode here disabled the whole
	// feature.
	//
	// This is RAGE's frame-shaker position: the dolly has already published its authored pose, and
	// the shake jogs the FRAME on its way out. Applying it any earlier (inside the path's own pose
	// push) would make the shaken pose the path's published pose, and marker placement, the graph
	// editor's defaults and the spectated-pawn match above would all read it back -- baking a
	// cosmetic wobble permanently into the shot. Here, turning the shake off restores the move.
	// A looked-through camera object (SceneRig, above) takes its own shake track; the path's
	// shake applies only while the path drives the view.
	const bool pathShakeApplied = Filmmaker::CamPathShake_ApplyToView(Tx, Ty, Tz, Rx, Ry, Rz, Fov,
		owner.sceneCamera ? Filmmaker::SceneRig_LookThrough() : 0);
	if (pathShakeApplied)
		originOrAnglesOverriden = true;

	// Build one absolute camera-motion delta for CalcViewModelView. Its hook temporarily removes
	// this delta from the viewmodel's eye inputs, then restores them after the engine has built
	// the complete animated arms/weapon pose. Repeated calls therefore start from the same input.
	double vmCompX = 0.0, vmCompY = 0.0, vmCompZ = 0.0;
	double vmCompPitch = 0.0, vmCompYaw = 0.0, vmCompRoll = 0.0;
	if (handheldVmComp.Any()) {
		const Filmmaker::FollowAngles basis{
			(double)preShakeRx, (double)preShakeRy, (double)preShakeRz };
		const Filmmaker::FollowVec3 world = Filmmaker::FollowRotateVector(
			Filmmaker::FollowVec3{
				handheldVmComp.fwd, handheldVmComp.right, handheldVmComp.up }, basis);
		vmCompX += world.x;
		vmCompY += world.y;
		vmCompZ += world.z;
		vmCompPitch += handheldVmComp.pitch;
		vmCompYaw += handheldVmComp.yaw;
		vmCompRoll += handheldVmComp.roll;
	}
	Filmmaker::ViewFxVm_SetCameraCompensation(
		vmCompX, vmCompY, vmCompZ, vmCompPitch, vmCompYaw, vmCompRoll);

	// Smooth View and the true-aim crosshair are applied at the common branch join later in
	// CViewRender::SetUpView.  This callback is deliberately BEFORE CS2's optional
	// cl_demoviewoverride pass, so applying either here uses an intermediate/stale camera.
	g_PendingPostDemoView.setup = ThisCViewSetup;
	g_PendingPostDemoView.curTime = curTime;
	g_PendingPostDemoView.povViewForAim = povViewForAim;
	g_PendingPostDemoView.valid = true;

	if(originOrAnglesOverriden) {
		pViewOrigin[0] = Tx;
		pViewOrigin[1] = Ty;
		pViewOrigin[2] = Tz;

		pViewAngles[0] = Rx;
		pViewAngles[1] = Ry;
		pViewAngles[2] = Rz;

		*pFov = Fov;

		g_bViewOverriden = true;
		g_fFovOverride = Fov;
		g_pFov = pFov;
	} else {
		g_bViewOverriden = false;
	}
	g_WrittenView.valid = originOrAnglesOverriden;
	g_WrittenView.pathShake = pathShakeApplied;
	g_WrittenView.origin[0] = Tx; g_WrittenView.origin[1] = Ty; g_WrittenView.origin[2] = Tz;
	g_WrittenView.angles[0] = Rx; g_WrittenView.angles[1] = Ry; g_WrittenView.angles[2] = Rz;

	g_iWidth = width;
	g_iHeight = height;

	g_MirvInputEx.LastCameraOrigin[0] = Tx;
	g_MirvInputEx.LastCameraOrigin[1] = Ty;
	g_MirvInputEx.LastCameraOrigin[2] = Tz;
	g_MirvInputEx.LastCameraAngles[0] = Rx;
	g_MirvInputEx.LastCameraAngles[1] = Ry;
	g_MirvInputEx.LastCameraAngles[2] = Rz;
	g_MirvInputEx.LastCameraFov = Fov;

	// mvm_debug: the FOV CS2 handed us vs the one this frame is rendered with, and who owned the
	// view -- logged on change only. A zoomed-in preview whose HUD still fits is a FOV change.
	if (Filmmaker::MvmDebugLog_Active()) {
		static int s_fovSig = 0x7fffffff;
		const int sig = ((int)(g_MirvInputEx.GameCameraFov * 10.f + 0.5f) << 12) ^ (int)(Fov * 10.f + 0.5f)
			^ ((originOrAnglesOverriden ? 1 : 0) << 28) ^ ((owner.path ? 1 : 0) << 29) ^ ((owner.freeCam ? 1 : 0) << 30)
			^ ((width * 7 + height) << 3);
		if (sig != s_fovSig) {
			s_fovSig = sig;
			Filmmaker::MvmDebugLog_LinefAlways("camera.fov",
				"engine=%.2f final=%.2f overridden=%d path=%d freecam=%d attach=%d import=%d other=%d mirvFov=%d size=%dx%d",
				(float)g_MirvInputEx.GameCameraFov, Fov, originOrAnglesOverriden ? 1 : 0, owner.path ? 1 : 0,
				owner.freeCam ? 1 : 0, owner.attach ? 1 : 0, owner.import ? 1 : 0, owner.other ? 1 : 0,
				Filmmaker::MirvFov_Enabled() ? 1 : 0, width, height);
		}
	}

	g_MirvInputEx.LastFrameTime = absTime;

	g_MirvInputEx.LastWidth = width;
	g_MirvInputEx.LastHeight = height;

	// Free-cam mouse look at the game's own aim sensitivity (sensitivity * m_yaw / m_pitch, read
	// once a second): the same hand movement turns the camera as much as it turns your aim.
	{
		static ULONGLONG lastSensRead = 0;
		const ULONGLONG nowSensRead = GetTickCount64();
		if (nowSensRead - lastSensRead >= 1000 && SOURCESDK::CS2::g_pCVar) {
			lastSensRead = nowSensRead;
			auto readFloat = [](const char* name, float fallback) {
				using namespace SOURCESDK::CS2;
				Cvar_s* handle = g_pCVar->GetCvar(g_pCVar->FindConVar(name, false).Get());
				return handle ? handle->m_Value.m_flValue : fallback;
			};
			const double sensitivity = readFloat("sensitivity", 1.25f);
			const double yaw = readFloat("m_yaw", 0.022f), pitch = readFloat("m_pitch", 0.022f);
			if (sensitivity > 0 && yaw > 0 && pitch > 0)
				g_MirvInputEx.m_MirvInput->SetGameMouseDegreesPerCount(sensitivity * yaw, sensitivity * pitch);
		}
	}
	g_MirvInputEx.m_MirvInput->Supply_MouseFrameEnd();
	// Free-cam mouse source, once a second: raw input or cursor deltas (different units; a free
	// cam turned by cursor deltas feels too fast and accelerated).
	if (Filmmaker::MvmDebugLog_Active() && g_MirvInputEx.m_MirvInput->GetCameraControlMode()) {
		static ULONGLONG lastMouseStat = 0;
		const ULONGLONG nowMouseStat = GetTickCount64();
		if (nowMouseStat - lastMouseStat >= 1000) {
			lastMouseStat = nowMouseStat;
			int rawFrames = 0, cursorFrames = 0; double rawYaw = 0, cursorYaw = 0; bool rawRecent = false;
			g_MirvInputEx.m_MirvInput->GetMouseSourceStats(rawFrames, cursorFrames, rawYaw, cursorYaw, rawRecent);
			if (rawFrames || cursorFrames)
				Filmmaker::MvmDebugLog_LinefAlways("freecam.mouse", "raw frames=%d yaw=%.2f  cursor frames=%d yaw=%.2f  rawRecent=%d sens=%.4f",
					rawFrames, rawYaw, cursorFrames, cursorYaw, rawRecent ? 1 : 0, g_MirvInputEx.m_MirvInput->GetMouseSensitivty());
		}
	}

	// Pre-shake pose on purpose -- see preShakeTx above. This is the "which camera is the game
	// on" reference used to resolve the spectated pawn, not a record of what was rendered.
	g_CurrentGameCamera.origin[0] = preShakeTx;
	g_CurrentGameCamera.origin[1] = preShakeTy;
	g_CurrentGameCamera.origin[2] = preShakeTz;
	g_CurrentGameCamera.angles[0] = preShakeRx;
	g_CurrentGameCamera.angles[1] = preShakeRy;
	g_CurrentGameCamera.angles[2] = preShakeRz;
	g_CurrentGameCamera.fov = preShakeFov;
	g_CurrentGameCamera.time = curTime;

	// VIEW OWNERSHIP: this trampoline stands in for CS2's internal IsPlayingDemo() check
	// that gates the demo-view-override (TrueView) pass. Returning true lets that pass
	// re-own the final view AFTER we wrote our pose -- the reason the dolly only "appeared"
	// when the demo paused (the override pass stops re-asserting then). While the filmmaker
	// camera PATH or MirvInput actively drives the view, return false so the engine keeps
	// the pose we just wrote. Range-gated playpath out-of-range remains unaffected because
	// it releases free cam and stops pushing a path pose before this hook runs.
	const bool pathOwnsView = Filmmaker::CameraPathOwnsView() || Filmmaker::GraphEditorExperiment_OwnsView();
	const bool blockDemoViewOverride = inputOverride || pathOwnsView;

	if (Filmmaker::CampathDebug()) {
		static unsigned s_ownerLogThrottle = 0;
		if (pathOwnsView && (s_ownerLogThrottle++ % 15 == 0))
			advancedfx::Message(
				"[setupview][owner] inputOverride=%d pathOwnsView=%d blockDemoViewOverride=%d wroteView=%d pos=(%.1f %.1f %.1f) ang=(%.1f %.1f %.1f) fov=%.1f\n",
				inputOverride ? 1 : 0, pathOwnsView ? 1 : 0,
				blockDemoViewOverride ? 1 : 0,
				originOrAnglesOverriden ? 1 : 0, Tx, Ty, Tz, Rx, Ry, Rz, Fov);
	}

	if (Filmmaker::MvmDebugLog_Active() && pathShakeApplied) {
		static unsigned s_n = 0;
		if ((s_n++ % 30) == 0)
			Filmmaker::MvmDebugLog_Linef("camera.view.owner",
				"pathShake=1 inputOverride=%d pathOwnsView=%d block=%d path=%d freecam=%d attach=%d scene=%d other=%d "
				"written=(%.1f %.1f %.1f | %.2f %.2f %.2f) preShake=(%.2f %.2f %.2f)",
				inputOverride ? 1 : 0, pathOwnsView ? 1 : 0, blockDemoViewOverride ? 1 : 0,
				owner.path ? 1 : 0, owner.freeCam ? 1 : 0, owner.attach ? 1 : 0, owner.sceneCamera ? 1 : 0, owner.other ? 1 : 0,
				Tx, Ty, Tz, Rx, Ry, Rz, preShakeRx, preShakeRy, preShakeRz);
	}
	if (blockDemoViewOverride)
		return false;
	return g_pEngineToClient->IsPlayingDemo();
}

void CS2_Client_CSetupView_PostDemoApply() {
	PendingPostDemoView pending = g_PendingPostDemoView;
	g_PendingPostDemoView.valid = false;
	if (!pending.valid || !pending.setup)
		return;

	unsigned char* setup = (unsigned char*)pending.setup;
	float* pFov = (float*)(setup + 0x498);
	float* pViewOrigin = (float*)(setup + 0x4a0);
	float* pViewAngles = (float*)(setup + 0x4b8);
	if (Filmmaker::MvmDebugLog_Active() && g_WrittenView.valid && g_WrittenView.pathShake) {
		static unsigned s_n = 0;
		if ((s_n++ % 30) == 0)
			Filmmaker::MvmDebugLog_Linef("camera.view.postdemo",
				"written=(%.1f %.1f %.1f | %.2f %.2f %.2f) engine=(%.1f %.1f %.1f | %.2f %.2f %.2f)",
				g_WrittenView.origin[0], g_WrittenView.origin[1], g_WrittenView.origin[2],
				g_WrittenView.angles[0], g_WrittenView.angles[1], g_WrittenView.angles[2],
				pViewOrigin[0], pViewOrigin[1], pViewOrigin[2], pViewAngles[0], pViewAngles[1], pViewAngles[2]);
	}

	const float inputPitch = pViewAngles[0];
	const float inputYaw = pViewAngles[1];
	float finalPitch = inputPitch;
	float finalYaw = inputYaw;
	float finalRoll = pViewAngles[2];
	float finalFov = *pFov;

	Filmmaker::SmoothView& smoothView = Filmmaker::SmoothViewRef();
	Filmmaker::ViewFxVm_SetPovContext(pending.povViewForAim, pending.povViewForAim ? AfxGetSpectatedPawnIndex() : -1);
	smoothView.SetPovContext(
		pending.povViewForAim,
		pending.povViewForAim ? AfxGetSpectatedPawnIndex() : -1);
	bool playbackPaused = false;
	if (g_pEngineToClient) {
		if (auto demo = g_pEngineToClient->GetDemoFile())
			playbackPaused = demo->IsDemoPaused();
	}
	if (smoothView.Enabled() && !g_MirvInputEx.m_MirvInput->GetCameraControlMode()) {
		// This is the actual post-demo camera pose. Sampling it here is the only way the angle
		// filter follows the POV the user sees instead of the stale pre-override camera.
		// FREE CAM EXCLUDED (same rule as CameraShakeStack): the chase filter would smooth the
		// user's own mouse-look and its write-back into LastCameraAngles feeds the filtered pose
		// back into the free-cam controller as next frame's reference -- a feedback loop that
		// reads as the camera flying off / "breaking" (worst during follow-camera reposition).
		const Filmmaker::SmoothView::MotionSample motion =
			smoothView.SampleMotion(
				pending.curTime,
				pViewOrigin[0], pViewOrigin[1], pViewOrigin[2],
				inputYaw);
		double pitch = finalPitch, yaw = finalYaw, roll = finalRoll, fov = finalFov;
		smoothView.ApplyCamera(
			pitch, yaw, roll, fov,
			motion.dt, motion.speed, motion.lateralSpeed, motion.vz, motion.onGround,
			playbackPaused);
		finalPitch = (float)pitch;
		finalYaw = (float)yaw;
		finalRoll = (float)roll;
		finalFov = (float)fov;

		pViewAngles[0] = finalPitch;
		pViewAngles[1] = finalYaw;
		pViewAngles[2] = finalRoll;
		*pFov = finalFov;

		g_bViewOverriden = true;
		g_fFovOverride = finalFov;
		g_pFov = pFov;

		g_MirvInputEx.LastCameraAngles[0] = finalPitch;
		g_MirvInputEx.LastCameraAngles[1] = finalYaw;
		g_MirvInputEx.LastCameraAngles[2] = finalRoll;
		g_MirvInputEx.LastCameraFov = finalFov;

		if (Filmmaker::MvmDebugLog_Active()) {
			static unsigned s_logThrottle = 0;
			if (s_logThrottle++ % 30 == 0) {
					Filmmaker::MvmDebugLog_Linef(
						"camera.smoothview.final",
						"frame=%d pov=%d postDemo=1 input=(%.2f %.2f) output=(%.2f %.2f %.2f) "
						"delta=(%.2f %.2f) zero=%d",
						g_MirvTime.framecount_get(), pending.povViewForAim ? 1 : 0,
						inputPitch, inputYaw,
						finalPitch, finalYaw, finalRoll,
						(double)finalPitch - (double)inputPitch,
						(double)finalYaw - (double)inputYaw,
						(std::fabs((double)finalPitch - inputPitch) < 0.0001
							&& std::fabs((double)finalYaw - inputYaw) < 0.0001) ? 1 : 0);
			}
		}
		}

	// Publish only after the last camera owner has run. The projection now describes the same
	// camera that reaches rendering, so the reticle marks recorded aim rather than screen centre.
	if (Filmmaker::AimCrosshairHudRef().Enabled()) {
		if (pending.povViewForAim) {
			// inputPitch/inputYaw are the recorded first-person POV at the post-demo join:
			// the demo equivalent of the live mouse-look signal.  The earlier trampoline's
			// raw angles can differ when CS2's observer/demo pass owns the POV; mixing that
			// earlier aim with this final camera is what placed the reticle at the wrong spot.
			Filmmaker::AimCrosshair_Publish(
				inputPitch, inputYaw,
				finalPitch, finalYaw, finalRoll, finalFov,
				g_iWidth, g_iHeight);
		} else {
			Filmmaker::AimCrosshair_Invalidate();
		}
	}
}

// The 14182 join starts with a seven-byte RIP-relative load, then prepares RSI/RBP.
// Branches from both sides of cl_demoviewoverride land here. A nearby thunk lets us
// run the post-demo camera pass before the load without losing those engine instructions.
bool CS2_Client_InstallPostDemoJoin(unsigned char *join) {
	const unsigned char expected[] = {
		0x48, 0x8B, 0x05, 0, 0, 0, 0,
		0x48, 0x8D, 0xB3, 0xA0, 0x04, 0, 0,
		0x48, 0x8D, 0xAB, 0xB8, 0x04, 0, 0
	};
	if (!join || memcmp(join, expected, 3) || memcmp(join + 7, expected + 7, 14))
		return false;

	int32_t globalDisp;
	memcpy(&globalDisp, join + 3, sizeof(globalDisp));
	unsigned char *global = join + 7 + globalDisp;

	SYSTEM_INFO info{};
	GetSystemInfo(&info);
	const uintptr_t granularity = info.dwAllocationGranularity;
	const uintptr_t center = reinterpret_cast<uintptr_t>(join) & ~(granularity - 1);
	unsigned char *thunk = nullptr;
	for (uintptr_t distance = granularity; distance <= 0x70000000 && !thunk; distance += granularity) {
		for (int direction : {1, -1}) {
			uintptr_t candidate = direction > 0 ? center + distance :
				(center > distance ? center - distance : 0);
			if (!candidate) continue;
			unsigned char *allocation = (unsigned char *)VirtualAlloc((void *)candidate,
				info.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
			if (!allocation) continue;
			int64_t delta = (int64_t)reinterpret_cast<uintptr_t>(allocation)
				- (int64_t)reinterpret_cast<uintptr_t>(join + 5);
			if (delta >= INT32_MIN && delta <= INT32_MAX) {
				thunk = allocation;
				break;
			}
			VirtualFree(allocation, 0, MEM_RELEASE);
		}
	}
	if (!thunk) return false;

	// mov rax, PostDemoApply; call rax; mov rax, global; mov rax, [rax]; jmp join+7.
	// The overwritten load supplied RAX for the next cmp. The thunk restores that
	// exact value, while the engine's RSI/RBP setup remains at join+7.
	unsigned char code[] = {
		0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xD0,
		0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x8B, 0x00,
		0xE9, 0, 0, 0, 0
	};
	void *callback = (void *)CS2_Client_CSetupView_PostDemoApply;
	memcpy(code + 2, &callback, sizeof(callback));
	memcpy(code + 14, &global, sizeof(global));
	int64_t returnDelta = (int64_t)reinterpret_cast<uintptr_t>(join + 7)
		- (int64_t)reinterpret_cast<uintptr_t>(thunk + sizeof(code));
	if (returnDelta < INT32_MIN || returnDelta > INT32_MAX) {
		VirtualFree(thunk, 0, MEM_RELEASE);
		return false;
	}
	int32_t returnRel = (int32_t)returnDelta;
	memcpy(code + 26, &returnRel, sizeof(returnRel));
	memcpy(thunk, code, sizeof(code));
	DWORD oldProtect;
	if (!VirtualProtect(thunk, info.dwPageSize, PAGE_EXECUTE_READ, &oldProtect)) {
		VirtualFree(thunk, 0, MEM_RELEASE);
		return false;
	}
	FlushInstructionCache(GetCurrentProcess(), thunk, sizeof(code));

	unsigned char patch[] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
	int32_t jumpRel = (int32_t)((int64_t)reinterpret_cast<uintptr_t>(thunk)
		- (int64_t)reinterpret_cast<uintptr_t>(join + 5));
	memcpy(patch + 1, &jumpRel, sizeof(jumpRel));
	MdtMemBlockInfos mbis;
	MdtMemAccessBegin(join, sizeof(patch), &mbis);
	memcpy(join, patch, sizeof(patch));
	MdtMemAccessEnd(&mbis);
	FlushInstructionCache(GetCurrentProcess(), join, sizeof(patch));
	return true;
}

// mvm_debug: the FOV the frame is finally rendered with, logged whenever it (or what decides it)
// changes -- our override on/off, the FOV we wrote, the engine's own value, the size the scaling
// used. A zoomed-in preview whose HUD still fits is a FOV change, and this is where it lands.
static void FovProbe_Log(bool overridden, float ourFov, float engineFov, float finalFov) {
	if (!Filmmaker::MvmDebugLog_Active()) return;
	static int s_sig = 0x7fffffff;
	const int sig = (overridden ? 1 : 0) | ((int)(finalFov * 10.f + 0.5f) << 1)
		^ ((g_MirvInputEx.LastWidth * 31 + g_MirvInputEx.LastHeight) << 14);
	if (sig == s_sig) return;
	s_sig = sig;
	Filmmaker::MvmDebugLog_LinefAlways("camera.fov",
		"final=%.2f overridden=%d ours=%.2f engine=%.2f size=%dx%d freecam=%d path=%d",
		finalFov, overridden ? 1 : 0, ourFov, engineFov, g_MirvInputEx.LastWidth, g_MirvInputEx.LastHeight,
		g_MirvInputEx.m_MirvInput->GetCameraControlMode() ? 1 : 0, Filmmaker::CameraPathOwnsView() ? 1 : 0);
}

typedef void (__fastcall * Unk_Override_Fov_t)(void *param_1,int param_2);
Unk_Override_Fov_t g_Old_Unk_Override_Fov = nullptr;
void __fastcall New_Unk_Override_Fov(void *param_1,int param_2) {

	if(g_bViewOverriden) {
		float * pWeaponFov = g_pFov + 1;
		float oldFov = *g_pFov;
		float oldWeaponFov = *pWeaponFov;

		g_Old_Unk_Override_Fov(param_1,param_2);
		const float engineFov = *g_pFov;

		*g_pFov = Apply_FovScaling(g_MirvInputEx.LastWidth, g_MirvInputEx.LastHeight, oldFov, FovScaling_AlienSwarm);
		*pWeaponFov = Apply_FovScaling(g_MirvInputEx.LastWidth, g_MirvInputEx.LastHeight, oldWeaponFov, FovScaling_AlienSwarm);
		FovProbe_Log(true, oldFov, engineFov, *g_pFov);
	} else {
		g_Old_Unk_Override_Fov(param_1,param_2);
		FovProbe_Log(false, 0.f, 0.f, g_MirvInputEx.LastCameraFov);
	}
}

/*size_t ofsProj = 0;

DirectX::XMMATRIX g_Mul = {
	{1,0,0,0},
	{0,1,0,0},
	{0,0,1,0},
	{0,0,0,1}
};

bool transpose = false;

CON_COMMAND(__mirv_t,"") {
	if(args->ArgC()>=2) transpose = 0 != atoi(args->ArgV(1));
}

CON_COMMAND(__mirv_o,"") {
	advancedfx::Message("=> %i\n",ofsProj);

	float b0 = ((ofsProj>>0) & 1) ? -1 : 1;
	float b1 = ((ofsProj>>1) & 1) ? -1 : 1;
	float b2 = ((ofsProj>>2) & 1) ? -1 : 1;

	switch((ofsProj>>3)%6) {
	default:
	case 0:
		// 0 1 2
		g_Mul = DirectX::XMMATRIX(
			b0, 0, 0, 0,
			0, b1, 0, 0,
			0, 0, b2, 0,
			0, 0, 0, 1
		);
		break;
	case 1:
		// 0 2 1
		g_Mul = DirectX::XMMATRIX(
			b0, 0, 0, 0,
			0, 0, b2, 0,
			0, b1, 0, 0,
			0, 0, 0, 1
		);
		break;
	case 2:
		// 1 0 2
		g_Mul = DirectX::XMMATRIX(
			0, b1, 0, 0,
			b0, 0, 0, 0,
			0, 0, b2, 0,
			0, 0, 0, 1
		);
		break;
	case 3:
		// 1 2 0
		// 0 1 2
		g_Mul = DirectX::XMMATRIX(
			0, b1, 0, 0,
			0, 0, b2, 0,
			b0, 0, 0, 0,
			0, 0, 0, 1
		);
		break;
	case 4:
		// 2 0 1
		g_Mul = DirectX::XMMATRIX(
			0, 0, b2, 0,
			b0, 0, 0, 0,
			0, b1, 0, 0,
			0, 0, 0, 1
		);		
		break;
	case 5:
		// 2 1 0
		g_Mul = DirectX::XMMATRIX(
			0, 0, b2, 0,
			0, b1, 0, 0,
			b0, 0, 0, 0,
			0, 0, 0, 1
		);		
		break;
	}


	ofsProj = (ofsProj + 1)%48;
}*/

// CCSGOInput::MouseInputEnabled -- client.dll's primary CCSGOInput vtable slot 23 (confirmed
// by static RE of the current build: RTTI ".?AVCCSGOInput@@", the 33-slot primary vtable; the
// slot is a multi-condition bool that gates whether the game consumes the mouse for player
// aim). Returning false frees the mouse and shows the cursor -- the nerv/Andromeda technique
// used by both the live-match menu and the explicit global free-mouse mode.
typedef bool (__fastcall * CCSGOInput_MouseInputEnabled_t)(void* This);
CCSGOInput_MouseInputEnabled_t g_Old_CCSGOInput_MouseInputEnabled = nullptr;
bool __fastcall New_CCSGOInput_MouseInputEnabled(void* This) {
	// NOTE: the original's return was once sampled here as the input gate's menu signal --
	// measured WRONG: it is false for ALL demo spectating, menu or not. The gate's menu level
	// comes from CS2's Panorama show/hide events instead (see UiInputGate.h).
	const bool engineEnabled = g_Old_CCSGOInput_MouseInputEnabled(This);

	// UiInputGate_Active: the game must not consume the mouse for aim while CS2's pause menu or
	// the ReShade overlay owns the screen -- the raw-input aim path bypasses both the WndProc
	// gate and ReShade's own input blocking, which is what kept the camera spinning under the
	// overlay ("two mouses").
	const bool releaseMouse = Filmmaker::LiveFxMenu_WantsGameMouseReleased()
		|| Filmmaker::MouseCapture_IsFree()
		|| Filmmaker::UiInputGate_Active();
	if (Filmmaker::MvmDebugLog_Active()) {
		static int s_lastReleaseMouse = -1;
		const int current = releaseMouse ? 1 : 0;
		if (current != s_lastReleaseMouse) {
			s_lastReleaseMouse = current;
			Filmmaker::MvmDebugLog_Linef("input.capture",
				"game_mouse_enabled release=%d engine=%d this=%p",
				current, engineEnabled ? 1 : 0, This);
		}
	}
	if (releaseMouse)
		return false;
	return engineEnabled;
}

// NOTE: an earlier attempt here detoured "CInputSystem::EnableCursor" at inputsystem.dll
// vtable slot 76 to leave SDL relative-mouse mode for the live Config sidebar. That slot
// index was never verified by RE, and a wrong slot silently detours an unrelated function on
// every frame -- exactly the class of breakage [[cs2-update-signature-repair]] warns about.
// It has been removed. Releasing the cursor for the sidebar is instead done with two
// verified, slot-independent levers:
//   * CCSGOInput::MouseInputEnabled -> false (stops player-aim consumption), and
//   * swallowing the engine's SetCursorPos warps in new_SetCursorPos (WindowInputHooks.cpp),
//     which is what actually stops the cursor being dragged back to screen centre.

typedef void (__fastcall * CViewRender_UnkMakeMatrix_t)(void* This);
CViewRender_UnkMakeMatrix_t g_Old_CViewRender_UnkMakeMatrix = nullptr;
void __fastcall New_CViewRender_UnkMakeMatrix(void* This) {

	g_Old_CViewRender_UnkMakeMatrix(This);
	//memcpy(g_WorldToScreenMatrix.m,(unsigned char*)This + 0x1b8,sizeof(g_WorldToScreenMatrix.m));


	/*DirectX::XMMATRIX * proj = (DirectX::XMMATRIX *)((unsigned char*)This + 0x298);
	DirectX::XMMATRIX result = g_Mul * *proj;
	if(transpose) result = DirectX::XMMatrixTranspose(result);

	g_WorldToScreenMatrix.m[0][0] = result(0,0);
	g_WorldToScreenMatrix.m[0][1] = result(0,1);
	g_WorldToScreenMatrix.m[0][2] = result(0,2);
	g_WorldToScreenMatrix.m[0][3] = result(0,3);
	g_WorldToScreenMatrix.m[1][0] = result(1,0);
	g_WorldToScreenMatrix.m[1][1] = result(1,1);
	g_WorldToScreenMatrix.m[1][2] = result(1,2);
	g_WorldToScreenMatrix.m[1][3] = result(1,3);
	g_WorldToScreenMatrix.m[2][0] = result(2,0);
	g_WorldToScreenMatrix.m[2][1] = result(2,1);
	g_WorldToScreenMatrix.m[2][2] = result(2,2);
	g_WorldToScreenMatrix.m[2][3] = result(2,3);
	g_WorldToScreenMatrix.m[3][0] = result(3,0);
	g_WorldToScreenMatrix.m[3][1] = result(3,1);
	g_WorldToScreenMatrix.m[3][2] = result(3,2);
	g_WorldToScreenMatrix.m[3][3] = result(3,3);*/

	float* proj = (float*)((unsigned char*)This + 0x218);
	SOURCESDK::VMatrix projectionMatrix;
	projectionMatrix.m[0][0] = proj[4 * 0 + 0];
	projectionMatrix.m[0][1] = proj[4 * 0 + 1];
	projectionMatrix.m[0][2] = proj[4 * 0 + 2];
	projectionMatrix.m[0][3] = proj[4 * 0 + 3];
	projectionMatrix.m[1][0] = proj[4 * 1 + 0];
	projectionMatrix.m[1][1] = proj[4 * 1 + 1];
	projectionMatrix.m[1][2] = proj[4 * 1 + 2];
	projectionMatrix.m[1][3] = proj[4 * 1 + 3];
	projectionMatrix.m[2][0] = proj[4 * 2 + 0];
	projectionMatrix.m[2][1] = proj[4 * 2 + 1];
	projectionMatrix.m[2][2] = proj[4 * 2 + 2];
	projectionMatrix.m[2][3] = proj[4 * 2 + 3];
	projectionMatrix.m[3][0] = proj[4 * 3 + 0];
	projectionMatrix.m[3][1] = proj[4 * 3 + 1];
	projectionMatrix.m[3][2] = proj[4 * 3 + 2];
	projectionMatrix.m[3][3] = proj[4 * 3 + 3];
	RenderSystemDX11_SupplyProjectionMatrix(projectionMatrix);

	proj = (float *)((unsigned char*)This + 0x298);
	g_WorldToScreenMatrix.m[0][0] = proj[4*0+0];
	g_WorldToScreenMatrix.m[0][1] = proj[4*0+1];
	g_WorldToScreenMatrix.m[0][2] = proj[4*0+2];
	g_WorldToScreenMatrix.m[0][3] = proj[4*0+3];
	g_WorldToScreenMatrix.m[1][0] = proj[4*1+0];
	g_WorldToScreenMatrix.m[1][1] = proj[4*1+1];
	g_WorldToScreenMatrix.m[1][2] = proj[4*1+2];
	g_WorldToScreenMatrix.m[1][3] = proj[4*1+3];
	g_WorldToScreenMatrix.m[2][0] = proj[4*2+0];
	g_WorldToScreenMatrix.m[2][1] = proj[4*2+1];
	g_WorldToScreenMatrix.m[2][2] = proj[4*2+2];
	g_WorldToScreenMatrix.m[2][3] = proj[4*2+3];
	g_WorldToScreenMatrix.m[3][0] = proj[4*3+0];
	g_WorldToScreenMatrix.m[3][1] = proj[4*3+1];
	g_WorldToScreenMatrix.m[3][2] = proj[4*3+2];
	g_WorldToScreenMatrix.m[3][3] = proj[4*3+3];

	g_CampathDrawer.OnEngineThread_SetupViewDone();
}

/*
class CCSGOVScriptGameSystem;
CCSGOVScriptGameSystem * g_pCCSGOVScriptGameSystem = nullptr;
typedef void (__fastcall * CCSGOVScriptGameSystem_UnkAddon_t)(CCSGOVScriptGameSystem *This); //:000
typedef unsigned long long int (__fastcall * CCSGOVScriptGameSystem_UnkLoadScriptFile_t)(CCSGOVScriptGameSystem *This, const char * pszFileName, bool bDebugPrint); //:008
CCSGOVScriptGameSystem_UnkAddon_t g_Old_CCSGOVScriptGameSystem_UnkAddon = nullptr;
CCSGOVScriptGameSystem_UnkLoadScriptFile_t g_Old_CCSGOVScriptGameSystem_UnkLoadScriptFile = nullptr;

void __fastcall New_CSGOVScriptGameSystem_UnkAddon(CCSGOVScriptGameSystem *This) {
	g_pCCSGOVScriptGameSystem = This;
	//advancedfx::Message("GOT IT\n");
	g_Old_CCSGOVScriptGameSystem_UnkAddon(This);
}

unsigned long long int __fastcall New_CCSGOVScriptGameSystem_UnkLoadScriptFile(CCSGOVScriptGameSystem *This, const char * pszFileName, bool bDebugPrint) {
	g_pCCSGOVScriptGameSystem = This;
	advancedfx::Message("LoadScriptFile: %s\n",pszFileName);
	return g_Old_CCSGOVScriptGameSystem_UnkLoadScriptFile(This, pszFileName, bDebugPrint);
}

CON_COMMAND(mirv_vscript_exec,"") {
	int argC = args->ArgC();
	const char * arg0 = args->ArgV(0);

	if(2 <= argC) {
		if(g_pCCSGOVScriptGameSystem) {
			g_Old_CCSGOVScriptGameSystem_UnkLoadScriptFile(g_pCCSGOVScriptGameSystem,args->ArgV(1),3 <= argC ? (0 != atoi(args->ArgV(2))) : true);
		} else advancedfx::Warning("Missing hooks.\n");
		return;
	}
	advancedfx::Message("%s <script_file_name> [<debug_print=0|1>]\n",arg0);
}*/

/*
typedef void (__fastcall * CViewRender_RenderView_t)(void* This, void * pViewSetup, void * pHudViewSetup, void * nClearFlags, void * whatToDraw);
CViewRender_RenderView_t g_Old_CViewRender_RenderView = nullptr;

enum ClearFlags_t
{
	VIEW_CLEAR_COLOR = 0x1,
	VIEW_CLEAR_DEPTH = 0x2,
	VIEW_CLEAR_FULL_TARGET = 0x4,
	VIEW_NO_DRAW = 0x8,
	VIEW_CLEAR_OBEY_STENCIL = 0x10, // Draws a quad allowing stencil test to clear through portals
	VIEW_CLEAR_STENCIL = 0x20,
};
void __fastcall New_CViewRender_RenderView(void* This, void * pViewSetup, void * pHudViewSetup, void * nClearFlags, void * whatToDraw) {
	return;
	g_Old_CViewRender_RenderView(This, pViewSetup, pHudViewSetup, nClearFlags, whatToDraw);
	//if((nClearFlags & VIEW_CLEAR_COLOR)&&(nClearFlags && VIEW_CLEAR_DEPTH)) {
		DrawCampath();
	//}
}*/

void HookClientDll(HMODULE clientDll) {
	static bool bFirstCall = true;
	if(!bFirstCall) return;
	bFirstCall = false;

	Afx::BinUtils::MemRange textRange = Afx::BinUtils::MemRange::FromEmpty();
	Afx::BinUtils::MemRange dataRange = Afx::BinUtils::MemRange::FromEmpty();
	{
		Afx::BinUtils::ImageSectionsReader sections((HMODULE)clientDll);
		if(!sections.Eof()) {
			textRange = sections.GetMemRange();
			sections.Next();
			if(!sections.Eof()){
				dataRange = sections.GetMemRange();
			}
		}
	}

	/*
		This is where it checks for engine->IsPlayingDemo() (and afterwards for cl_demoviewoverride (float))
		before under these conditions it is calling CalcDemoViewOverride, so this is in CViewRender::SetUpView:

       180898120 48 8b 0d        MOV        RCX,qword ptr [DAT_181e2d7a8]
                 81 56 59 01
       180898127 48 8b 01        MOV        RAX,qword ptr [RCX]
       18089812a ff 90 40        CALL       qword ptr [RAX + 0x148]
                 01 00 00
       180898130 0f 57 f6        XORPS      XMM6,XMM6
       180898133 84 c0           TEST       AL,AL
       180898135 74 63           JZ         LAB_18089819a
       180898137 ba ff ff        MOV        EDX,0xffffffff
                 ff ff

	*/
	{
		Afx::BinUtils::MemRange result = FindPatternString(textRange, "48 8b 0d ?? ?? ?? ?? 48 8b 01 ff 90 50 01 00 00 0f 57 ff 84 c0 74 ?? 48 8d 0d");
																	  
		if (!result.IsEmpty()) {
			/*
				These are the top 16 bytes we change to:

180882cd6	4C89f1               mov     rcx, r14
			48b8???????????????? mov     rax, ???????????????? <-- here we load our hook's address
			ff10                 call    qword ptr [rax]
			90                   nop

			The CViewSetup pointer is in RBX in CS2 build 14182. The surrounding
			code is the tell: right before this site the game does LEA R8,[RBX+0x4b8] and
			LEA RDX,[RBX+0x4a0] -- exactly the view-angles / view-origin offsets the
			trampoline below reads. Passing the wrong register here hands the trampoline a
			garbage CViewSetup and it then WRITES the view through it, so this crashes on the
			first rendered frame rather than failing safely.
			*/
			unsigned char asmCode[16]={
				0x48, 0x89, 0xd9, // mov rcx, rbx: CViewSetup moved from r14 to rbx
				0x48, 0xb8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
				0xff, 0x10,
				0x90
			};
			static LPVOID ptr = CS2_Client_CSetupView_Trampoline_IsPlayingDemo;
			LPVOID ptrPtr = &ptr;
			memcpy(&asmCode[5], &ptrPtr, sizeof(LPVOID));

			MdtMemBlockInfos mbis;
			MdtMemAccessBegin((LPVOID)result.Start, 16, &mbis);
			memcpy((LPVOID)result.Start, asmCode, 16);
			MdtMemAccessEnd(&mbis);

			// The test/jz at offsets 19..22 lands at the shared join after the optional
			// demo override. The branch body changed size in 14182, so derive the join
			// from its rel8 displacement. At this join the cvar read was inlined away.
			unsigned char* postJoin = nullptr;
			if ((unsigned char)((unsigned char*)result.Start)[19] == 0x84
				&& (unsigned char)((unsigned char*)result.Start)[20] == 0xC0
				&& (unsigned char)((unsigned char*)result.Start)[21] == 0x74) {
				const int rel8 = (int)(signed char)((unsigned char*)result.Start)[22];
				postJoin = (unsigned char*)result.Start + 23 + rel8;
			}
			if (!CS2_Client_InstallPostDemoJoin(postJoin)) {
				// Name the casualties. Without this the only symptom is "Smooth View does
				// nothing and the true-aim crosshair never appears", with no hint that a
				// signature moved -- and the repo's AOB verifier cannot catch this site,
				// because the derivation and the byte check are code, not a pattern string.
				advancedfx::Warning(
					"AfxHookSource2: CViewRender::SetUpView post-demo join did not match"
					" (%s). Smooth View and the true-aim crosshair are DISABLED"
					" this session; re-check the join signature against client.dll.\n",
					postJoin ? "validated but bytes differ" : "could not be derived");
				ErrorBox(MkErrStr(__FILE__, __LINE__));
			}
		}
		else
			ErrorBox(MkErrStr(__FILE__, __LINE__));
	}

	/*
		The FOV is overridden / computed a second time in the function called in
		CViewRender::SetUpView (see hook above on how to find it):
		
       180898360 49 8b cf        MOV        RCX,R15
       180898363 8b 10           MOV        EDX,dword ptr [RAX]
       180898365 e8 b6 fd        CALL       FUN_180888120 <-- we detour this function.                                   undefined FUN_180888120()
                 fe ff
       18089836a 4c 8b c7        MOV        R8,RDI
       18089836d 41 c6 87        MOV        byte ptr [R15 + 0x1330],0x0
                 30 13 00 
                 00 00

		void FUN_180888120(longlong *param_1,int param_2) { ... }
	*/
	// Commenting out this for now since it's now in the same function as above
	// {
	// 	Afx::BinUtils::MemRange result = FindPatternString(textRange, "48 8B C4 53 55 56 57 41 56 41 57");
	// 	if (!result.IsEmpty()) {
	// 		g_Old_Unk_Override_Fov = (Unk_Override_Fov_t)result.Start;
	// 		DetourTransactionBegin();
	// 		DetourUpdateThread(GetCurrentThread());
	// 		DetourAttach((PVOID*)&g_Old_Unk_Override_Fov, New_Unk_Override_Fov);
	// 		if(NO_ERROR != DetourTransactionCommit()) ErrorBox(MkErrStr(__FILE__, __LINE__));            
	// 	} else ErrorBox(MkErrStr(__FILE__, __LINE__));
	// }
/*
	if(void ** vtable = (void**)Afx::BinUtils::FindClassVtable(clientDll,".?AVCRenderingPipelineCsgo@@", 0, 0x0)) {
		g_Old_CViewRender_RenderView = (CViewRender_RenderView_t)vtable[0] ;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_Old_CViewRender_RenderView,New_CViewRender_RenderView);
        // doesn't work without error // DetourAttach(&(PVOID&)g_Old_CCSGOVScriptGameSystem_UnkLoadScriptFile, New_CCSGOVScriptGameSystem_UnkLoadScriptFile);
        if(NO_ERROR != DetourTransactionCommit()) ErrorBox(MkErrStr(__FILE__, __LINE__));
	} else ErrorBox(MkErrStr(__FILE__, __LINE__));*/

	if(!Hook_CGameEventManager((void*)clientDll)) ErrorBox(MkErrStr(__FILE__, __LINE__));
/*
	if(void ** vtable = (void**)Afx::BinUtils::FindClassVtable(clientDll,".?AVCCSGOVScriptGameSystem@@", 0, 0x10)) {
		g_Old_CCSGOVScriptGameSystem_UnkAddon = (CCSGOVScriptGameSystem_UnkAddon_t)vtable[0] ;
		g_Old_CCSGOVScriptGameSystem_UnkLoadScriptFile = (CCSGOVScriptGameSystem_UnkLoadScriptFile_t)vtable[5] ;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_Old_CCSGOVScriptGameSystem_UnkAddon,New_CSGOVScriptGameSystem_UnkAddon);
        // doesn't work without error // DetourAttach(&(PVOID&)g_Old_CCSGOVScriptGameSystem_UnkLoadScriptFile, New_CCSGOVScriptGameSystem_UnkLoadScriptFile);
        if(NO_ERROR != DetourTransactionCommit()) ErrorBox(MkErrStr(__FILE__, __LINE__));
		else AfxDetourPtr((PVOID *)&(vtable[7]), New_CCSGOVScriptGameSystem_UnkLoadScriptFile, (PVOID*)&g_Old_CCSGOVScriptGameSystem_UnkLoadScriptFile);
	} else ErrorBox(MkErrStr(__FILE__, __LINE__));*/

	if(void ** vtable = (void**)Afx::BinUtils::FindClassVtable(clientDll,".?AVCViewRender@@", 0, 0x0)) {
		g_Old_CViewRender_UnkMakeMatrix = (CViewRender_UnkMakeMatrix_t)vtable[4] ;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_Old_CViewRender_UnkMakeMatrix,New_CViewRender_UnkMakeMatrix);
        if(NO_ERROR != DetourTransactionCommit()) ErrorBox(MkErrStr(__FILE__, __LINE__));
	} else ErrorBox(MkErrStr(__FILE__, __LINE__));

	// CCSGOInput::MouseInputEnabled = primary CCSGOInput vtable slot 23 (see the hook above).
	// Frees the mouse to the offline mvm_test FX menu on a live map.
	if(void ** vtable = (void**)Afx::BinUtils::FindClassVtable(clientDll,".?AVCCSGOInput@@", 0, 0x0)) {
		g_Old_CCSGOInput_MouseInputEnabled = (CCSGOInput_MouseInputEnabled_t)vtable[23] ;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_Old_CCSGOInput_MouseInputEnabled,New_CCSGOInput_MouseInputEnabled);
        if(NO_ERROR != DetourTransactionCommit()) ErrorBox(MkErrStr(__FILE__, __LINE__));
	} else ErrorBox(MkErrStr(__FILE__, __LINE__));

	// client entity system related
	{
		// "Entities/Client Entity Count"
		auto unkFn = Afx::BinUtils::FindPatternString(textRange, "40 55 53 48 8d ac 24 ?? ?? ?? ?? 48 81 ec ?? ?? ?? ?? 48 8b 0d ?? ?? ?? ?? 33 d2 e8 ?? ?? ?? ??");
		if (!unkFn.IsEmpty()) {
			void * pEntityList = (void *)(unkFn.Start+18+7+*(int*)(unkFn.Start+18+3));
			void * pFnGetHighestEntityIterator = (void *)(unkFn.Start+27+5+*(int*)(unkFn.Start+27+1));

			// see near "no such entity %d\n" called with pEntityList and uint
            // or near "Format: ent_find_index <index>\n" called only with uint and there's pEntityList inside with uint
			auto fnGetEntityFromIndexMem = Afx::BinUtils::FindPatternString(textRange, "4c 8d 49 10 81 fa fe 7f 00 00");
			if (!fnGetEntityFromIndexMem.IsEmpty()) {
				auto pFnGetEntityFromIndex = (void*)(fnGetEntityFromIndexMem.Start);
				if(! Hook_ClientEntitySystem( pEntityList, pFnGetHighestEntityIterator, pFnGetEntityFromIndex )) ErrorBox(MkErrStr(__FILE__, __LINE__));

			} else ErrorBox(MkErrStr(__FILE__, __LINE__));

		} else ErrorBox(MkErrStr(__FILE__, __LINE__));

	}
	/*
	   GetSplitScreenPlayer(int): 
	   This function is called in GetLocalPlayerController script function in clientDll, 
	   go inside of function there and it's called with 0.

       1808541f0 40 53           PUSH       RBX
       1808541f2 48 83 ec 20     SUB        RSP,0x20
       1808541f6 8b 91 9c        MOV        EDX,dword ptr [RCX + 0x9c]
                 00 00 00
       1808541fc 48 8b d9        MOV        RBX,RCX
       1808541ff 83 fa ff        CMP        EDX,-0x1
       180854202 0f 84 27        JZ         LAB_18085432f
                 01 00 00
       180854208 4c 8b 0d        MOV        R9,qword ptr [DAT_1818b7d68]
                 59 3b 06 01
       18085420f 4d 85 c9        TEST       R9,R9

	*/
	{
		Afx::BinUtils::MemRange range_get_split_screen_player = Afx::BinUtils::FindPatternString(textRange, "48 83 EC ?? 83 F9 ?? 75 ?? 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 ?? 48 8B 01 FF 90 ?? ?? ?? ?? 8B 08 48 63 C1 48 8D 0D ?? ?? ?? ?? 48 8B 04 C1 48 83 C4 ?? C3");
		if(!range_get_split_screen_player.IsEmpty()) {
			Hook_GetSplitScreenPlayer((void*)range_get_split_screen_player.Start);
		} else ErrorBox(MkErrStr(__FILE__, __LINE__));
	}
}

SOURCESDK::CreateInterfaceFn g_AppSystemFactory = nullptr;
SOURCESDK::CS2::IMemAlloc *SOURCESDK::CS2::g_pMemAlloc = nullptr;
SOURCESDK::CS2::ICvar * SOURCESDK::CS2::cvar = nullptr;
SOURCESDK::CS2::ICvar * SOURCESDK::CS2::g_pCVar = nullptr;
void * g_pSceneSystem = nullptr;

typedef bool (__fastcall * CSceneSystem_WaitForRenderingToComplete_t)(void * This);
CSceneSystem_WaitForRenderingToComplete_t g_Old_CSceneSystem_WaitForRenderingToComplete = nullptr;

bool __fastcall New_CSceneSystem_WaitForRenderingToComplete(void * This) {
	bool result = g_Old_CSceneSystem_WaitForRenderingToComplete(This);
	//DrawCampath();
	return result;
}

typedef int(* CCS2_Client_Connect_t)(void* This, SOURCESDK::CreateInterfaceFn appSystemFactory);
CCS2_Client_Connect_t old_CCS2_Client_Connect;
int new_CCS2_Client_Connect(void* This, SOURCESDK::CreateInterfaceFn appSystemFactory) {
	static bool bFirstCall = true;

	if (bFirstCall) {
		bFirstCall = false;

		void * iface = NULL;

		if (SOURCESDK::CS2::g_pCVar = SOURCESDK::CS2::cvar = (SOURCESDK::CS2::ICvar*)appSystemFactory(SOURCESDK_CS2_CVAR_INTERFACE_VERSION, NULL)) {
		}
		else ErrorBox(MkErrStr(__FILE__, __LINE__));

		if (g_pEngineToClient = (SOURCESDK::CS2::ISource2EngineToClient*)appSystemFactory(SOURCESDK_CS2_ENGINE_TO_CLIENT_INTERFACE_VERSION, NULL)) {
		}
		else ErrorBox(MkErrStr(__FILE__, __LINE__));

		if (g_pGameUIService = (SOURCESDK::CS2::IGameUIService*)appSystemFactory(SOURCESDK_CS2_GAMEUISERVICE_INTERFACE_VERSION, NULL)) {
		}
		else ErrorBox(MkErrStr(__FILE__, __LINE__));

		if (g_pSceneSystem = (SOURCESDK::CS2::IGameUIService*)appSystemFactory("SceneSystem_002", NULL)) {
			Hook_SceneSystem_WaitForRenderingToComplete(g_pSceneSystem);
		}
		else ErrorBox(MkErrStr(__FILE__, __LINE__));
	}

	return old_CCS2_Client_Connect(This, appSystemFactory);
}

CON_COMMAND(mirv_cvar_unhide_all, "Unlocks cmds and cvars.") {
	int total = 0;
	int nUnhidden = 0;
	for(size_t i = 0; i < 65536; i++ )
	{
		SOURCESDK::CS2::CCmd * cmd = SOURCESDK::CS2::g_pCVar->GetCmd(i);
		if(nullptr == cmd) break;
		int nFlags = cmd->GetFlags();
		if(nFlags == 0x400) break;
		total++;
		if(nFlags & (SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY | SOURCESDK_CS2_FCVAR_HIDDEN)) {
//			fprintf(f1,"[+] %lli: 0x%08x: %s : %s\n", i, cmd->m_nFlags, cmd->m_pszName, cmd->m_pszHelpString);
			cmd->SetFlags(nFlags &= ~(SOURCESDK::int64)(SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY | SOURCESDK_CS2_FCVAR_HIDDEN));
			nUnhidden++;
		} else {
//			fprintf(f1,"[ ] %lli: 0x%08x: %s : %s\n", i, cmd->m_nFlags, cmd->m_pszName, cmd->m_pszHelpString);
		}
	}
	advancedfx::Message("==== Cmds total: %i (Cmds unhidden: %i) ====\n",total,nUnhidden);

	total = 0;
	nUnhidden = 0;
	for(size_t i = 0; i < 65536; i++ )
	{
		SOURCESDK::CS2::Cvar_s * cvar = SOURCESDK::CS2::g_pCVar->GetCvar(i);
		if(nullptr == cvar) break;
		total++;
		if(cvar->m_nFlags & (SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY | SOURCESDK_CS2_FCVAR_HIDDEN)) {
//			fprintf(f1,"[+] %lli: 0x%08x: %s : %s\n", i, cvar->m_nFlags, cvar->m_pszName, cvar->m_pszHelpString);
			cvar->m_nFlags &= ~(SOURCESDK::int64)(SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY | SOURCESDK_CS2_FCVAR_HIDDEN);
			nUnhidden++;
		} else {
//			fprintf(f1,"[ ] %lli: 0x%08x: %s : %s\n", i, cvar->m_nFlags, cvar->m_pszName, cvar->m_pszHelpString);
		}
	}
	
	advancedfx::Message("==== Cvars total: %i (Cvars unhidden: %i) ====\n",total,nUnhidden);
}

CON_COMMAND(mirv_cvar_unlock_sv_cheats, "Unlocks sv_cheats on client (as much as possible).") {
	int total = 0;
	int nUnhidden = 0;
	for(size_t i = 0; i < 65536; i++ )
	{
		SOURCESDK::CS2::CCmd * cmd = SOURCESDK::CS2::g_pCVar->GetCmd(i);
		if(nullptr == cmd) break;
		int nFlags = cmd->GetFlags();
		if(nFlags == 0x400) break;
		total++;
		if(nFlags & (SOURCESDK_CS2_FCVAR_CHEAT)) {
//			fprintf(f1,"[+] %lli: 0x%08x: %s : %s\n", i, cmd->m_nFlags, cmd->m_pszName, cmd->m_pszHelpString);
			cmd->SetFlags(nFlags &= ~(SOURCESDK::int64)(SOURCESDK_CS2_FCVAR_CHEAT));
			nUnhidden++;
		} else {
//			fprintf(f1,"[ ] %lli: 0x%08x: %s : %s\n", i, cmd->m_nFlags, cmd->m_pszName, cmd->m_pszHelpString);
		}
	}
	advancedfx::Message("==== Cmds total: %i (Cmds unlocked: %i) ====\n",total,nUnhidden);

	total = 0;
	nUnhidden = 0;
	for(size_t i = 0; i < 65536; i++ )
	{
		SOURCESDK::CS2::Cvar_s * cvar = SOURCESDK::CS2::g_pCVar->GetCvar(i);
		if(nullptr == cvar) break;
		total++;
		if(cvar->m_nFlags & (SOURCESDK_CS2_FCVAR_CHEAT)) {
//			fprintf(f1,"[+] %lli: 0x%08x: %s : %s\n", i, cvar->m_nFlags, cvar->m_pszName, cvar->m_pszHelpString);
			cvar->m_nFlags &= ~(SOURCESDK::int64)(SOURCESDK_CS2_FCVAR_CHEAT);
			nUnhidden++;
		} else {
//			fprintf(f1,"[ ] %lli: 0x%08x: %s : %s\n", i, cvar->m_nFlags, cvar->m_pszName, cvar->m_pszHelpString);
		}
		if(0 == strcmp("sv_cheats",cvar->m_pszName)) {
			cvar->m_nFlags &= ~(SOURCESDK::int64)(SOURCESDK_CS2_FCVAR_REPLICATED| SOURCESDK_CS2_FCVAR_NOTIFY);
			cvar->m_nFlags |= SOURCESDK_CS2_FCVAR_CLIENTDLL;
			cvar->m_Value.m_bValue = true;			
		}
	}
	
	advancedfx::Message("==== Cvars total: %i (Cvars unlocked: %i) ====\n",total,nUnhidden);
}

typedef int(* CCS2_Client_Init_t)(void* This);
CCS2_Client_Init_t old_CCS2_Client_Init;
int new_CCS2_Client_Init(void* This) {
	// Arm the opt-in render-size gateway before the original client Init builds its target pool.
	// At this point client.dll is loaded but its render-device global is not populated yet; the
	// gateway installed here completes the vtable hook at the start of the target builder itself.
	Filmmaker::RenderEngineSize_EnsureInstalled();
	int result = old_CCS2_Client_Init(This);

	if(!Hook_ClientEntitySystem2()) ErrorBox(MkErrStr(__FILE__, __LINE__));	

	// Connect to reshade addon if present:
	g_ReShadeAdvancedfx.Connect();

	WrpRegisterCommands();

	AfxHookSource2Rs_Engine_Init();

	PrintInfo();

	HookSchemaSystem(g_H_SchemaSystem);

	if (g_pFileSystem) {
		std::string path(GetHlaeFolder());
		path.append("resources\\AfxHookSource2\\cs2");
		g_pFileSystem->AddSearchPath(path.c_str(), "GAME");

		const wchar_t* USRLOCALCSGO = _wgetenv(L"USRLOCALCSGO");
		if (nullptr != USRLOCALCSGO) {
			std::string USRLOCALCSGO_copy = "";
			WideStringToUTF8String(USRLOCALCSGO, USRLOCALCSGO_copy);
			if (USRLOCALCSGO_copy.size() > 0) {
				// Content mount (FX packs, ragdolls). Added at the TAIL like everything else:
				// it only ADDS new asset paths, so it must never shadow a stock file.
				g_pFileSystem->AddSearchPath(USRLOCALCSGO_copy.c_str(), "GAME");
				// The packed form: the whole pack as ONE single-file VPK beside gameinfo.gi
				// (FX/tools/pack-fx-vpk.py). A .vpk search path mounts the archive itself.
				std::string packedFx = USRLOCALCSGO_copy;
				packedFx.append("\\source_mvm_fx.vpk");
				if (GetFileAttributesA(packedFx.c_str()) != INVALID_FILE_ATTRIBUTES) {
					g_pFileSystem->AddSearchPath(packedFx.c_str(), "GAME");
					Filmmaker::MvmDebugLog_LinefAlways("fx.mount", "mounted packed FX pack %s", packedFx.c_str());
				}

				// UI overrides, added at the HEAD so they OUTRANK csgo/. This is the only way to
				// replace a stock Panorama asset that the game loads by its own fixed name --
				// e.g. the launch intro, which panorama/scripts/intromovie.vts loads as
				// "file://{resources}/videos/intro.webm". At the tail (the default) csgo's copy
				// always wins, which is why an override placed in the content mount above is
				// silently ignored no matter what the mount's gameinfo.gi search order says.
				//
				// Deliberately a SEPARATE, NARROW subtree rather than promoting the whole
				// content mount: that mount also carries gameinfo.gi, cfg/, materials/ and
				// models/, and letting those outrank the game's own copies is a far larger
				// blast radius than this needs.
				std::string uiOverride = USRLOCALCSGO_copy;
				uiOverride.append("\\ui_override");
				g_pFileSystem->AddSearchPath(uiOverride.c_str(), "GAME",
					SOURCESDK::CS2::PATH_ADD_TO_HEAD);
			}
		}
	}

	return result;
}

CON_COMMAND(__mirv_print_search_paths, "")
{
	g_pFileSystem->PrintSearchPaths();
}

#include "Filmmaker/Environment/EnvironmentScene.h"
#include "Filmmaker/Props/Props.h" // Props_OnShutdown: a clean exit clears the crash-loop guard

typedef void(* CCS2_Client_Shutdown_t)(void* This);
CCS2_Client_Shutdown_t old_CCS2_Client_Shutdown;
void new_CCS2_Client_Shutdown(void* This) {
	// Each step leaves a session-log line. A quit that hung until Windows ended it (player report
	// 2026-10-15, last line "ShutdownSource2Logging") then shows whether it stopped in our steps.
	Filmmaker::BugReport_Record("shutdown", "client shutdown: saving environment edits");
	Filmmaker::Environment::FlushCommittedEdits();
	Filmmaker::Props_OnShutdown();
	Filmmaker::BugReport_Record("shutdown", "client shutdown: our steps done; CS2 continues its shutdown");
	// A quit reaches here, but CS2 then ends the process without DLL detach, so the lock removed
	// only at detach made every normal quit look like a crash ("CS2 closed without a clean exit"
	// with Source2Shutdown as the last log line). The session is over here.
	Filmmaker::CrashReport_CleanExit();
	AfxHookSource2Rs_Engine_Shutdown();

	old_CCS2_Client_Shutdown(This);
	// The engine is gone, but the window still gets messages (WM_ACTIVATE as it is destroyed), and a
	// debug-log line there read the demo tick through this interface: engine2 null read on quit
	// (reproduced 2026-10-13). Every user null-checks it.
	g_pEngineToClient = nullptr;
}


typedef void * (* CS2_Client_SetGlobals_t)(void* This, void * pGlobals);
CS2_Client_SetGlobals_t old_CS2_Client_SetGlobals;
void *  new_CS2_Client_SetGlobals(void* This, void * pGlobals) {

	g_pGlobals = (Cs2Gloabls_t)pGlobals;

	return old_CS2_Client_SetGlobals(This, pGlobals);
}

class CExecuteClientCmdForCommandSystem : public IExecuteClientCmdForCommandSystem {
public:
	virtual void ExecuteClientCmd(const char * value) {
		if(g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0,value,true);
	}
} g_ExecuteClientCmdForCommandSystem;

class CGetTickForCommandSystem : public IGetTickForCommandSystem {
public:
	virtual float GetTick() {
		float tick = 0;
		if(g_pEngineToClient) {
			if(SOURCESDK::CS2::IDemoFile * pDemoFile = g_pEngineToClient->GetDemoFile()) {
				tick = (float)pDemoFile->GetDemoTick() + g_MirvTime.interpolation_amount_get();
			}
		}
		return tick;
	}
} g_GetTickForCommandSystem;

class CGetTimeForCommandSystem : public IGetTimeForCommandSystem {
public:
	virtual float GetTime() {
		return g_MirvTime.curtime_get();
	}
} g_GetTimeForCommandSystem;

class CommandSystem g_CommandSystem(&g_ExecuteClientCmdForCommandSystem, &g_GetTickForCommandSystem, &g_GetTimeForCommandSystem);

CON_COMMAND(mirv_cmd, "Command system (for scheduling commands).")
{
	g_CommandSystem.Console_Command(args);
}

class CMirvSkip_GotoDemoTick : public IMirvSkip_GotoDemoTick {
	virtual void GotoDemoTick(int tick) {
        std::ostringstream oss;
        oss << "demo_gototick " << tick;
        if(g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0,oss.str().c_str(),true);		
	}
} g_MirvSkip_GotoDemoTick;

CON_COMMAND(mirv_skip, "for skipping through demos (uses demo_gototick)")
{
    MirvSkip_ConsoleCommand(args, &g_MirvCampath_Time, &g_MirvSkip_GotoDemoTick);
}

#include "Filmmaker/Environment/EnvironmentWorld.h"
extern void resetDefaultCloudColors();
extern void resetCachedMaterials();
namespace Filmmaker { void RenderPlayersOnlyGreen_OnLevelInit(); }

static void *g_CS2_ClientIface = nullptr;
static int g_ForceCommandReregisterFrames = 0;
void CS2_HookClientDllInterface(void * iface);

typedef void * (* CS2_Client_LevelInitPreEntity_t)(void* This, void * pUnk1, void * pUnk2);
CS2_Client_LevelInitPreEntity_t old_CS2_Client_LevelInitPreEntity;
void * new_CS2_Client_LevelInitPreEntity(void* This, void * pUnk1, void * pUnk2) {
	Filmmaker::Environment::WorldLook_OnLevelInit();
	resetDefaultCloudColors();
	resetCachedMaterials();
	Filmmaker::RenderPlayersOnlyGreen_OnLevelInit();
	// Before the new level exists: drop cached particle-swap handles, which point into
	// resources this teardown freed (see ParticleFx_OnLevelInit).
	Filmmaker::ParticleFx_OnLevelInit();
	// Arm the EFFECTS-tab carry-over (or the deliberate reset). Only arms here -- the actual work
	// runs from the frame pump once the new level's materials exist.
	Filmmaker::EffectsPersistence_OnLevelInit();
	// The previous entity graph is gone: forget corpses mid-clip without touching them.
	Filmmaker::DeathAnimations_LevelInit();
	Filmmaker::EasterEggs_LevelInit(); // forget old entities/models and queue egg cleanup
	Filmmaker::NervousSystem_LevelInit();
	void * result = old_CS2_Client_LevelInitPreEntity(This, pUnk1, pUnk2);
	g_CommandSystem.OnLevelInitPreEntity();
	// Do not touch ICvar during LevelInit — unregister/re-register here has crashed map
	// loads. Defer ensure for a few seconds so the engine finishes rebuilding console tables.
	WrpDeferCommandEnsure(3000);
	g_ForceCommandReregisterFrames = 300;
	return result;
}

typedef void (* CS2_Client_FrameStageNotify_t)(void* This, SOURCESDK::CS2::ClientFrameStage_t curStage);

CS2_Client_FrameStageNotify_t old_CS2_Client_FrameStageNotify;

// mirv_fix forceClInterpRatio (upstream advancedfx): cl_interp_ratio 0 during demo playback makes
// player animations stutter at low fps and in recordings.
bool g_bForceClInterpRatio = true;

void  new_CS2_Client_FrameStageNotify(void* This, SOURCESDK::CS2::ClientFrameStage_t curStage) {
	
	AfxHookSource2Rs_Engine_RunJobQueue();

	/*
	// React to demo being paused / unpaused to work around Valve's new bandaid client time "fix":
	bool bIsDemoPaused = false;
	if(g_pEngineToClient) {
		if(SOURCESDK::CS2::IDemoFile * pDemoPlayer = g_pEngineToClient->GetDemoFile()) {
			if(pDemoPlayer->IsPlayingDemo())
				bIsDemoPaused = pDemoPlayer->IsDemoPaused();
		}
	}
	if(bIsDemoPaused != g_DemoPausedData.IsPaused) {
		if(bIsDemoPaused) {
			g_DemoPausedData.FirstPausedCurtime = g_MirvTime.curtime_get();
			g_DemoPausedData.FirstPausedInterpolationAmount = g_MirvTime.interpolation_amount_get();
			g_DemoPausedData.IsPaused = true;
		} else {
			g_DemoPausedData.IsPaused = false;
		}
	}*/

	// Work around demoui cursor sheningans:
	// - Always manually fetch cursor pos.
	// - Make room to move the cursor.
	// - Hide cursor (can do this way because SDL3 never hides it, uses invisible one instead).
	static bool bCursorHidden = false;
	static bool bUiCursorForcedVisible = false;
	const bool bLiveUiCursor = Filmmaker::LiveFxMenu_WantsGameMouseReleased();
	const bool bGlobalFreeMouse = Filmmaker::MouseCapture_IsFree();
	const bool bCursorReleased = bLiveUiCursor || bGlobalFreeMouse;
	if (Filmmaker::MvmDebugLog_Active()) {
		static int s_lastCursorReleased = -1;
		const int current = bCursorReleased ? 1 : 0;
		if (current != s_lastCursorReleased) {
			s_lastCursorReleased = current;
			Filmmaker::MvmDebugLog_Linef("input.capture",
				"cursor release=%d globalFree=%d liveUi=%d mirvActive=%d",
				current, bGlobalFreeMouse ? 1 : 0, bLiveUiCursor ? 1 : 0,
				g_MirvInputEx.m_MirvInput->IsActive() ? 1 : 0);
		}
	}
	if(bCursorReleased) {
		// Returning false from CCSGOInput::MouseInputEnabled stops player aim capture, but
		// the old demoui workaround below could still hide/park the Win32 cursor every frame.
		if (bCursorHidden) {
			bCursorHidden = false;
			ShowCursor(TRUE);
		}
		// EVERY FRAME, not once on entry: CS2's gameplay mouse mode re-asserts capture and
		// re-clips the cursor continuously. Releasing only on the opening transition let it
		// re-confine the pointer immediately afterwards, which is why the sidebar cursor could
		// not be moved at all (opening the scoreboard "fixed" it precisely because that makes
		// CS2 drop its own gameplay mouse grab). Both calls are cheap and idempotent.
		ReleaseCapture();
		// ClipCursor is DESKTOP-GLOBAL: there is one clip rectangle per desktop, and
		// ClipCursor(nullptr) clears whatever the FOREGROUND application installed -- not just
		// ours. This block runs EVERY FRAME and it also runs while CS2 is unfocused: alt-tabbing
		// away SETS free-mouse (MouseCapture_OnActivate's "deactivate rearm"), and UnfocusedRate
		// deliberately keeps the frame pump at full rate when we do not own focus. So a
		// backgrounded CS2 was wiping the cursor confinement of whatever game the user had
		// alt-tabbed INTO, tens of times a second, and their pointer kept escaping that window.
		// Only release the clip while it is plausibly OURS to release.
		if (nullptr != g_UiCursorProbe.hwnd && GetForegroundWindow() == g_UiCursorProbe.hwnd)
			ClipCursor(nullptr);
		if (!bUiCursorForcedVisible) {
			// ShowCursor is a REFERENCE COUNT, so it must stay one-shot -- calling it per frame
			// would run the counter away and break the hide on exit.
			while (ShowCursor(TRUE) < 0) {}
			// CS2 can leave SDL's invisible cursor shape installed after relative-input mode.
			// Give the Panorama sidebar a normal Win32 arrow on the same transition.
			SetCursor(LoadCursor(nullptr, IDC_ARROW));
			bUiCursorForcedVisible = true;
		}
	}
	else {
		bUiCursorForcedVisible = false;
	if(g_MirvInputEx.m_MirvInput->IsActive()) {
		HWND hWnd = GetActiveWindow();
		// input.park probe: this block PARKS the OS cursor at the window centre every frame, so
		// it is the prime suspect whenever the pointer will not stay where the user put it.
		// GetActiveWindow() is thread-local and is NOT guaranteed to be the game window -- if it
		// ever differs from the WndProc-captured HWND, the centre is computed against the wrong
		// window and the cursor is warped somewhere else entirely. Log the identity comparison
		// and the decision inputs; change-gated so this costs nothing per frame.
		if (Filmmaker::MvmDebugLog_Active()) {
			static int s_sig = -1;
			const HWND probeHwnd = g_UiCursorProbe.hwnd;
			const int sig = (hWnd == probeHwnd ? 1 : 0)
				| (NULL != hWnd ? 2 : 0)
				| (g_MirvInputEx.m_MirvInput->GetCameraControlMode() ? 4 : 0)
				| (Filmmaker::CameraTimeline_WantsCursor() ? 16 : 0)
				| (Filmmaker::CameraEditor_Active() ? 32 : 0)
				| (bCursorHidden ? 64 : 0);
			if (sig != s_sig) {
				s_sig = sig;
				RECT probeRect = {}; RECT activeRect = {};
				if (probeHwnd) GetClientRect(probeHwnd, &probeRect);
				if (hWnd) GetClientRect(hWnd, &activeRect);
				Filmmaker::MvmDebugLog_LinefAlways("input.park",
					"ACTIVE active=%p probe=%p same=%d camCtl=%d tlCursor=%d editor=%d"
					" activeClient=%ldx%ld probeClient=%ldx%ld hidden=%d",
					(void*)hWnd, (void*)probeHwnd, hWnd == probeHwnd ? 1 : 0,
					g_MirvInputEx.m_MirvInput->GetCameraControlMode() ? 1 : 0,
					Filmmaker::CameraTimeline_WantsCursor() ? 1 : 0,
					Filmmaker::CameraEditor_Active() ? 1 : 0,
					activeRect.right - activeRect.left, activeRect.bottom - activeRect.top,
					probeRect.right - probeRect.left, probeRect.bottom - probeRect.top,
					bCursorHidden ? 1 : 0);
			}
		}
		if(NULL != hWnd) {
			POINT point;
			const bool bHavePoint = 0 != GetCursorPos(&point);
			// new_GetCursorPos FAKES the position it is handed while a capture mode is on
			// (Supply_GetCursorPos), so keep the REAL cursor position for the orbit delta.
			const POINT realPoint = point;
			if(!bCursorHidden) ShowCursor(FALSE);
			else if(bHavePoint) {
				new_GetCursorPos(&point);
			}
			RECT rect;
			if(GetClientRect(hWnd, &rect)) {
				POINT pt {(rect.left+rect.right)/2,(rect.top+rect.bottom)/2};
				if(ClientToScreen(hWnd,&pt)) {
					// Where the cursor actually is vs where we are about to slam it. Throttled
					// (this runs every frame while parking); a large persistent delta is the
					// signature of the pointer being dragged away from the user's hand.
					if (Filmmaker::MvmDebugLog_Active()) {
						static int s_throttle = 0;
						if (0 == (s_throttle++ % 120))
							Filmmaker::MvmDebugLog_LinefAlways("input.park",
								"warp real=(%ld,%ld) -> centre=(%ld,%ld) delta=(%ld,%ld) client=%ldx%ld",
								realPoint.x, realPoint.y, pt.x, pt.y,
								realPoint.x - pt.x, realPoint.y - pt.y,
								rect.right - rect.left, rect.bottom - rect.top);
					}
					new_SetCursorPos(pt.x, pt.y);
				}
			}
		}
		bCursorHidden = true;
	}
	else if(bCursorHidden) {
		bCursorHidden = false;
		ShowCursor(TRUE);
		// The other edge: parking stopped. Pairs with the ACTIVE line above so the log shows
		// exactly which frames owned the cursor.
		if (Filmmaker::MvmDebugLog_Active())
			Filmmaker::MvmDebugLog_LinefAlways("input.park",
				"RELEASED (MirvInput inactive) tlCursor=%d editor=%d",
				Filmmaker::CameraTimeline_WantsCursor() ? 1 : 0,
				Filmmaker::CameraEditor_Active() ? 1 : 0);
	}
	}

	switch(curStage) {
	case 0:
		if(g_bForceClInterpRatio && g_pEngineToClient && g_pEngineToClient->IsPlayingDemo()){
			static SOURCESDK::CS2::Cvar_s * handle_cl_interp_ratio = SOURCESDK::CS2::g_pCVar->GetCvar(SOURCESDK::CS2::g_pCVar->FindConVar("cl_interp_ratio", false).Get());
			if(handle_cl_interp_ratio){
				if(0 == handle_cl_interp_ratio->m_Value.m_flValue) {
					handle_cl_interp_ratio->m_Value.m_flValue = 1;
				}
			}
		}
		break;
	case SOURCESDK::CS2::FRAME_RENDER_PASS:
		g_CommandSystem.OnExecuteCommands();
		if(g_SleepMs) Sleep(g_SleepMs);
		Filmmaker::RunMainThreadFrame(); // Panorama UI work on the main thread
		if(g_ForceCommandReregisterFrames > 0) {
			--g_ForceCommandReregisterFrames;
			// Probe only; ForceReregister is expensive and unsafe if the name hash is healthy.
			WrpEnsureCommandsRegistered();
		}
		break;
	}

	Filmmaker::RagdollWeaponArmPhysics_BeforeClientStage(static_cast<int>(curStage));
	AfxHookSource2Rs_Engine_OnClientFrameStageNotify(curStage, true);

	old_CS2_Client_FrameStageNotify(This, curStage);

	// The engine's handler above restores each weapon's recorded mesh-group mask from the demo
	// snapshot, silently undoing the legacy-vs-CS2 selection made in RunMainThreadFrame. Re-assert
	// it here so ours is the last write before the draw (mesh mask only -- no Panorama work).
	switch (curStage) {
	case SOURCESDK::CS2::FRAME_RENDER_PASS:
		Filmmaker::RunMainThreadLateFrame();
		break;
	}

	AfxHookSource2Rs_Engine_OnClientFrameStageNotify(curStage, false);

	AfxHookSource2Rs_Engine_RunJobQueue();
	if (curStage == SOURCESDK::CS2::FRAME_RENDER_PASS)
		Filmmaker::RunMainThreadPostFrame();
}


void CS2_HookClientDllInterface(void * iface)
{
	g_CS2_ClientIface = iface;
	void ** vtable = *(void***)iface;

	AfxDetourPtr((PVOID *)&(vtable[0]), new_CCS2_Client_Connect, (PVOID*)&old_CCS2_Client_Connect);
	AfxDetourPtr((PVOID *)&(vtable[3]), new_CCS2_Client_Init, (PVOID*)&old_CCS2_Client_Init);
	AfxDetourPtr((PVOID *)&(vtable[4]), new_CCS2_Client_Shutdown, (PVOID*)&old_CCS2_Client_Shutdown);
	AfxDetourPtr((PVOID *)&(vtable[11]), new_CS2_Client_SetGlobals, (PVOID*)&old_CS2_Client_SetGlobals);
	AfxDetourPtr((PVOID *)&(vtable[35]), new_CS2_Client_LevelInitPreEntity, (PVOID*)&old_CS2_Client_LevelInitPreEntity);
	AfxDetourPtr((PVOID *)&(vtable[36]), new_CS2_Client_FrameStageNotify, (PVOID*)&old_CS2_Client_FrameStageNotify);
}

SOURCESDK::CreateInterfaceFn old_Client_CreateInterface = 0;

void* new_Client_CreateInterface(const char *pName, int *pReturnCode)
{
	static bool bFirstCall = true;

	void * pRet = old_Client_CreateInterface(pName, pReturnCode);

	if(bFirstCall)
	{
		bFirstCall = false;

		void * iface = NULL;
		
		if (iface = old_Client_CreateInterface(SOURCESDK_CS2_Source2Client_VERSION, NULL)) {
			CS2_HookClientDllInterface(iface);
		}
		else
		{
			ErrorBox("Could not get a supported VClient interface.");
		}
	}

	return pRet;
}

SOURCESDK::CreateInterfaceFn old_ResourceSystem_CreateInterface = 0;

void* new_ResourceSystem_CreateInterface(const char *pName, int *pReturnCode)
{
	static bool bFirstCall = true;
	void * pRet = old_ResourceSystem_CreateInterface (pName, pReturnCode);

	if(bFirstCall)
	{
		bFirstCall = false;
		if (!(g_pCResourceSystem = (CResourceSystem*)old_ResourceSystem_CreateInterface("ResourceSystem013", NULL))) ErrorBox("Could not get ResourceSystem013 interface.");
	}

	return pRet;
}

SOURCESDK::CreateInterfaceFn old_FileSystem_CreateInterface = 0;

void* new_FileSystem_CreateInterface(const char *pName, int *pReturnCode)
{
	static bool bFirstCall = true;
	void * pRet = old_FileSystem_CreateInterface (pName, pReturnCode);

	if(bFirstCall)
	{
		bFirstCall = false;
		if (!(g_pFileSystem = (SOURCESDK::CS2::IFileSystem*)old_FileSystem_CreateInterface("VFileSystem017", NULL))) ErrorBox("Could not get VFileSystem017 interface.");
	}

	return pRet;
}

FARPROC Tier0GetProcAddressFilter(HMODULE hModule, LPCSTR lpProcName, FARPROC nResult);

FARPROC WINAPI new_tier0_GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
	return Tier0GetProcAddressFilter(hModule, lpProcName, GetProcAddress(hModule, lpProcName));
}

// Shared by the tier0 import-table hook and the inline GetProcAddress hook (see
// InstallInlineKernel32Hooks): swaps in our CreateInterface wrappers for the modules we track.
FARPROC Tier0GetProcAddressFilter(HMODULE hModule, LPCSTR lpProcName, FARPROC nResult)
{
	if (!nResult)
		return nResult;

	if (HIWORD(lpProcName))
	{
		if (!lstrcmp(lpProcName, "GetProcAddress"))
			return (FARPROC) &new_tier0_GetProcAddress;

		if (
			hModule == g_H_ClientDll
			&& !lstrcmp(lpProcName, "CreateInterface")
		) {
			old_Client_CreateInterface = (SOURCESDK::CreateInterfaceFn)nResult;
			return (FARPROC) &new_Client_CreateInterface;
		}

		if (
			hModule == g_H_ResourceSystemDll
			&& !lstrcmp(lpProcName, "CreateInterface")
		) {
			old_ResourceSystem_CreateInterface = (SOURCESDK::CreateInterfaceFn)nResult;
			return (FARPROC) &new_ResourceSystem_CreateInterface;
		}

		if (
			hModule == g_H_FileSystem_stdio
			&& !lstrcmp(lpProcName, "CreateInterface")
		) {
			old_FileSystem_CreateInterface = (SOURCESDK::CreateInterfaceFn)nResult;
			return (FARPROC) &new_FileSystem_CreateInterface;
		}
	}

	return nResult;
}

HMODULE WINAPI new_LoadLibraryA(LPCSTR lpLibFileName);
HMODULE WINAPI new_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
HMODULE WINAPI new_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);

HANDLE
WINAPI
new_CreateFileW(
	_In_ LPCWSTR lpFileName,
	_In_ DWORD dwDesiredAccess,
	_In_ DWORD dwShareMode,
	_In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
	_In_ DWORD dwCreationDisposition,
	_In_ DWORD dwFlagsAndAttributes,
	_In_opt_ HANDLE hTemplateFile
);

BOOL
WINAPI
new_CreateDirectoryW(
    _In_ LPCWSTR lpPathName,
    _In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes
    );

BOOL
WINAPI
new_GetFileAttributesExW(
    _In_ LPCWSTR lpFileName,
    _In_ GET_FILEEX_INFO_LEVELS fInfoLevelId,
    _Out_writes_bytes_(sizeof(WIN32_FILE_ATTRIBUTE_DATA)) LPVOID lpFileInformation
    );

CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD)> g_Import_tier0_KERNEL32_LoadLibraryExA("LoadLibraryExA", &new_LoadLibraryExA);
CAfxImportFuncHook<HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD)> g_Import_tier0_KERNEL32_LoadLibraryExW("LoadLibraryExW", &new_LoadLibraryExW);
CAfxImportFuncHook<FARPROC(WINAPI*)(HMODULE, LPCSTR)> g_Import_tier0_KERNEL32_GetProcAddress("GetProcAddress", &new_tier0_GetProcAddress);
CAfxImportFuncHook<HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE)> g_Import_tier0_KERNEL32_CreateFileW("CreateFileW", &new_CreateFileW);
CAfxImportFuncHook<BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES)> g_Import_tier0_KERNEL32_CreateDirectoryW("CreateDirectoryW", &new_CreateDirectoryW);
CAfxImportFuncHook<BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID)> g_Import_tier0_KERNEL32_GetFileAttributesExW("GetFileAttributesExW", &new_GetFileAttributesExW);

HANDLE WINAPI new_CreateFileW(
	_In_ LPCWSTR lpFileName,
	_In_ DWORD dwDesiredAccess,
	_In_ DWORD dwShareMode,
	_In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
	_In_ DWORD dwCreationDisposition,
	_In_ DWORD dwFlagsAndAttributes,
	_In_opt_ HANDLE hTemplateFile
)
{
	static bool bWasRecording = false; // allow startmovie wav-fixup by engine to get through one more time.
	if (AfxStreams_IsRcording() || bWasRecording) {
		std::wstring strFileName(lpFileName);
		for (auto& c : strFileName) c = std::tolower(c);
		if (StringEndsWithW(strFileName.c_str(), L"" ADVANCEDFX_STARTMOVIE_WAV_KEY ".wav")) {
			// Detours our wav to our folder.			
			bWasRecording = AfxStreams_IsRcording();
			std::wstring newPath(AfxStreams_GetTakeDir());
			newPath.append(L"\\audio.wav");
			return g_Import_tier0_KERNEL32_CreateFileW.TrueFunc(newPath.c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
		}
	}
	return g_Import_tier0_KERNEL32_CreateFileW.TrueFunc(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

BOOL
WINAPI
new_CreateDirectoryW(
    _In_ LPCWSTR lpPathName,
    _In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes
    ) {

	if (AfxStreams_IsRcording()) {
		// Do not create dummy movie folders while recording startmovie wav.
		std::wstring strMovieFolder(L"\\\\?\\");
		strMovieFolder.append(GetProcessFolderW());
		strMovieFolder.append(L"csgo\\movie\\");
		if(StringBeginsWithW(lpPathName,strMovieFolder.c_str())) return TRUE;
	}

	return g_Import_tier0_KERNEL32_CreateDirectoryW.TrueFunc(lpPathName,lpSecurityAttributes);
}

BOOL
WINAPI
new_GetFileAttributesExW(
    _In_ LPCWSTR lpFileName,
    _In_ GET_FILEEX_INFO_LEVELS fInfoLevelId,
    _Out_writes_bytes_(sizeof(WIN32_FILE_ATTRIBUTE_DATA)) LPVOID lpFileInformation
    ) {

	if (AfxStreams_IsRcording()) {
		std::wstring strFileName(lpFileName);
		for (auto& c : strFileName) c = std::tolower(c);
		if (StringEndsWithW(strFileName.c_str(), L"" ADVANCEDFX_STARTMOVIE_WAV_KEY ".wav")) {
			// Detours our wav to our folder.			
			std::wstring newPath(AfxStreams_GetTakeDir());
			newPath.append(L"\\audio.wav");
			return g_Import_tier0_KERNEL32_GetFileAttributesExW.TrueFunc(newPath.c_str(),fInfoLevelId,lpFileInformation);
		}
	}

	return g_Import_tier0_KERNEL32_GetFileAttributesExW.TrueFunc(lpFileName,fInfoLevelId,lpFileInformation);
}


CAfxImportDllHook g_Import_tier0_KERNEL32("KERNEL32.dll", CAfxImportDllHooks({
	&g_Import_tier0_KERNEL32_LoadLibraryExA
	, &g_Import_tier0_KERNEL32_LoadLibraryExW
	, &g_Import_tier0_KERNEL32_GetProcAddress
	, &g_Import_tier0_KERNEL32_CreateFileW
	, &g_Import_tier0_KERNEL32_CreateDirectoryW
	, &g_Import_tier0_KERNEL32_GetFileAttributesExW}));

CAfxImportsHook g_Import_tier0(CAfxImportsHooks({
	&g_Import_tier0_KERNEL32 }));

void CommonHooks()
{
	static bool bFirstRun = true;
	static bool bFirstTier0 = true;

	// do not use messageboxes here, there is some friggin hooking going on in between by the
	// Source engine.

	if (bFirstRun)
	{
		bFirstRun = false;
	}
}

CAfxImportFuncHook<HMODULE (WINAPI *)(LPCSTR)> g_Import_launcher_KERNEL32_LoadLibraryA("LoadLibraryA", &new_LoadLibraryA);
CAfxImportFuncHook<HMODULE (WINAPI *)(LPCSTR, HANDLE, DWORD)> g_Import_launcher_KERNEL32_LoadLibraryExA("LoadLibraryExA", &new_LoadLibraryExA);

CAfxImportDllHook g_Import_launcher_KERNEL32("KERNEL32.dll", CAfxImportDllHooks({
	&g_Import_launcher_KERNEL32_LoadLibraryA
	, &g_Import_launcher_KERNEL32_LoadLibraryExA }));

CAfxImportsHook g_Import_launcher(CAfxImportsHooks({
	&g_Import_launcher_KERNEL32 }));

CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR)> g_Import_filesystem_steam_KERNEL32_LoadLibraryA("LoadLibraryA", &new_LoadLibraryA);
CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD)> g_Import_filesystem_steam_KERNEL32_LoadLibraryExA("LoadLibraryExA", &new_LoadLibraryExA);

CAfxImportDllHook g_Import_filesystem_steam_KERNEL32("KERNEL32.dll", CAfxImportDllHooks({
	&g_Import_filesystem_steam_KERNEL32_LoadLibraryA
	, &g_Import_filesystem_steam_KERNEL32_LoadLibraryExA }));

CAfxImportsHook g_Import_filesystem_steam(CAfxImportsHooks({
	&g_Import_filesystem_steam_KERNEL32 }));


//CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR)> g_Import_engine2_KERNEL32_LoadLibraryA("LoadLibraryA", &new_LoadLibraryA);
//CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD)> g_Import_engine2_KERNEL32_LoadLibraryExA("LoadLibraryExA", &new_LoadLibraryExA);

//CAfxImportDllHook g_Import_engine2_KERNEL32("KERNEL32.dll", CAfxImportDllHooks({
//	&g_Import_engine2_KERNEL32_LoadLibraryA
//	, &g_Import_engine2_KERNEL32_LoadLibraryExA }));

void * New_SteamInternal_FindOrCreateUserInterface(void*pUser, const char * pIntervaceName);

CAfxImportFuncHook<void*(*)(void *, const char *)> g_Import_engine2_steam_api64_SteamInternal_FindOrCreateUserInterface("SteamInternal_FindOrCreateUserInterface", &New_SteamInternal_FindOrCreateUserInterface);

void * New_SteamInternal_FindOrCreateUserInterface(void*pUser, const char * pInterfaceName) {
	if(0 == strcmp(pInterfaceName,"STEAMREMOTESTORAGE_INTERFACE_VERSION016")) {
		if (int idx = g_CommandLine->FindParam(L"-afxDisableSteamStorage")) {
			return nullptr;
		}
	}
	
	return g_Import_engine2_steam_api64_SteamInternal_FindOrCreateUserInterface.GetTrueFuncValue()(pUser,pInterfaceName);
}

CAfxImportDllHook g_Import_engine2_steam_api64("steam_api64.dll", CAfxImportDllHooks({
	&g_Import_engine2_steam_api64_SteamInternal_FindOrCreateUserInterface }));

CAfxImportsHook g_Import_engine2(CAfxImportsHooks({
	//&g_Import_engine2_KERNEL32,
	&g_Import_engine2_steam_api64 }));

CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR)> g_Import_materialsystem2_KERNEL32_LoadLibraryA("LoadLibraryA", &new_LoadLibraryA);
CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD)> g_Import_materialsystem2_KERNEL32_LoadLibraryExA("LoadLibraryExA", &new_LoadLibraryExA);

CAfxImportDllHook g_Import_materialsystem2_KERNEL32("KERNEL32.dll", CAfxImportDllHooks({
	&g_Import_materialsystem2_KERNEL32_LoadLibraryA
	, &g_Import_materialsystem2_KERNEL32_LoadLibraryExA }));

CAfxImportsHook g_Import_materialsystem2(CAfxImportsHooks({
	&g_Import_materialsystem2_KERNEL32 }));


CAfxImportFuncHook<LONG_PTR(WINAPI*)(HWND, int)> g_Import_SDL3_USER32_GetWindowLongW("GetWindowLongPtrW", &new_GetWindowLongPtrW);
CAfxImportFuncHook<LONG_PTR(WINAPI*)(HWND, int, LONG_PTR)> g_Import_SDL3_USER32_SetWindowLongW("SetWindowLongPtrW", &new_SetWindowLongPtrW);
CAfxImportFuncHook<HCURSOR(WINAPI*)(HCURSOR)> g_Import_SDL3_USER32_SetCursor("SetCursor", &new_SetCursor);
CAfxImportFuncHook<int(WINAPI*)(BOOL)> g_Import_SDL3_USER32_ShowCursor("ShowCursor", &new_ShowCursor);
CAfxImportFuncHook<HWND(WINAPI*)(HWND)> g_Import_SDL3_USER32_SetCapture("SetCapture", &new_SetCapture);
CAfxImportFuncHook<BOOL(WINAPI*)()> g_Import_SDL3_USER32_ReleaseCapture("ReleaseCapture", &new_ReleaseCapture);
CAfxImportFuncHook<BOOL(WINAPI*)(const RECT*)> g_Import_SDL3_USER32_ClipCursor("ClipCursor", &new_ClipCursor);
CAfxImportFuncHook<BOOL(WINAPI*)(LPPOINT)> g_Import_SDL3_USER32_GetCursorPos("GetCursorPos", &new_GetCursorPos);
CAfxImportFuncHook<BOOL(WINAPI*)(int, int)> g_Import_SDL3_USER32_SetCursorPos("SetCursorPos", &new_SetCursorPos);
// Legacy render-resolution experiment: route the optional client-rect override through SDL3.
// Runtime testing proved this does not resize the captured scene target consistently; the hooks
// remain only for the settled-or-labeled-fallback path. Gated inside new_GetClientRect.
CAfxImportFuncHook<BOOL(WINAPI*)(HWND, LPRECT)> g_Import_SDL3_USER32_GetClientRect("GetClientRect", &new_GetClientRect);


UINT
WINAPI
New_GetRawInputBuffer(
    _Out_writes_bytes_opt_(*pcbSize) PRAWINPUT pData,
    _Inout_ PUINT pcbSize,
    _In_ UINT cbSizeHeader);

CAfxImportFuncHook<UINT(WINAPI*)(_Out_writes_bytes_opt_(*pcbSize) PRAWINPUT, _Inout_ PUINT, _In_ UINT cbSizeHeader)> g_Import_SDL3_USER32_GetRawInputBuffer("GetRawInputBuffer", &New_GetRawInputBuffer);

UINT
WINAPI
New_GetRawInputBuffer(
    _Out_writes_bytes_opt_(*pcbSize) PRAWINPUT pData,
    _Inout_ PUINT pcbSize,
    _In_ UINT cbSizeHeader) {
	UINT result = g_Import_SDL3_USER32_GetRawInputBuffer.GetTrueFuncValue()(pData,pcbSize,cbSizeHeader);

	result = g_MirvInputEx.m_MirvInput->Supply_RawInputBuffer(result, pData,pcbSize,cbSizeHeader);

	if (result != (UINT)-1 && 0 < result && pData && pcbSize && sizeof(RAWINPUTHEADER) == cbSizeHeader) {
		PRAWINPUT block = pData;
		for (UINT i = 0; i < result; i++) {
			WindowInputHooks_FilterRawInput(block);
			// NEXTRAWINPUTBLOCK, spelled out (its QWORD typedef is not declared here).
			block = (PRAWINPUT)((BYTE*)block + ((block->header.dwSize + sizeof(ULONGLONG) - 1) & ~(sizeof(ULONGLONG) - 1)));
		}
	}

	return result;

}


UINT WINAPI New_GetRawInputData(
    _In_ HRAWINPUT hRawInput,
    _In_ UINT uiCommand,
    _Out_writes_bytes_to_opt_(*pcbSize, return) LPVOID pData,
    _Inout_ PUINT pcbSize,
    _In_ UINT cbSizeHeader);

CAfxImportFuncHook<UINT(WINAPI*)(_In_ HRAWINPUT, _In_ UINT, _Out_writes_bytes_to_opt_(*pcbSize, return) LPVOID pData,_Inout_ PUINT,_In_ UINT)> g_Import_SDL3_USER32_GetRawInputData("GetRawInputData", &New_GetRawInputData);

UINT WINAPI New_GetRawInputData(
    _In_ HRAWINPUT hRawInput,
    _In_ UINT uiCommand,
    _Out_writes_bytes_to_opt_(*pcbSize, return) LPVOID pData,
    _Inout_ PUINT pcbSize,
    _In_ UINT cbSizeHeader) {

	UINT result = g_Import_SDL3_USER32_GetRawInputData.GetTrueFuncValue()(hRawInput,uiCommand,pData,pcbSize,cbSizeHeader);

	result = g_MirvInputEx.m_MirvInput->Supply_RawInputData(result, hRawInput, uiCommand,pData,pcbSize,cbSizeHeader);

	if (result != (UINT)-1 && uiCommand == RID_INPUT && pData && pcbSize && sizeof(RAWINPUT) <= *pcbSize)
		WindowInputHooks_FilterRawInput((RAWINPUT*)pData);

	return result;
}


CAfxImportDllHook g_Import_SDL3_USER32("USER32.dll", CAfxImportDllHooks({
	&g_Import_SDL3_USER32_GetWindowLongW,
	&g_Import_SDL3_USER32_SetWindowLongW,
	&g_Import_SDL3_USER32_SetCursor,
	&g_Import_SDL3_USER32_ShowCursor,
	&g_Import_SDL3_USER32_SetCapture,
	&g_Import_SDL3_USER32_ReleaseCapture,
	&g_Import_SDL3_USER32_ClipCursor,
	&g_Import_SDL3_USER32_GetCursorPos,
	&g_Import_SDL3_USER32_SetCursorPos,
	&g_Import_SDL3_USER32_GetRawInputData,
	&g_Import_SDL3_USER32_GetRawInputBuffer,
	&g_Import_SDL3_USER32_GetClientRect }));

CAfxImportsHook g_Import_SDL3(CAfxImportsHooks({
	&g_Import_SDL3_USER32 }));

// Route the same legacy experiment through rendersystemdx11. A larger swapchain alone only
// upscales the unchanged scene target, so this is not the working startup scene-scale seam.
// (Named *_ClientRect: RenderSystemDX11Hooks.cpp already owns a g_Import_rendersystemdx11.)
CAfxImportFuncHook<BOOL(WINAPI*)(HWND, LPRECT)> g_Import_rsdx11_USER32_GetClientRect("GetClientRect", &new_GetClientRect);

CAfxImportDllHook g_Import_rsdx11_USER32_ClientRect("USER32.dll", CAfxImportDllHooks({
	&g_Import_rsdx11_USER32_GetClientRect }));

CAfxImportsHook g_Import_rsdx11_ClientRect(CAfxImportsHooks({
	&g_Import_rsdx11_USER32_ClientRect }));

// engine2 owns additional video-mode/client-rect consumers. Including it did not make the legacy
// override work; it remains here only so all three experimental imports share one inert gate.
CAfxImportFuncHook<BOOL(WINAPI*)(HWND, LPRECT)> g_Import_engine2_USER32_GetClientRect("GetClientRect", &new_GetClientRect);

CAfxImportDllHook g_Import_engine2_USER32_ClientRect("USER32.dll", CAfxImportDllHooks({
	&g_Import_engine2_USER32_GetClientRect }));

CAfxImportsHook g_Import_engine2_ClientRect(CAfxImportsHooks({
	&g_Import_engine2_USER32_ClientRect }));

CAfxImportFuncHook<LONG_PTR(WINAPI*)(HWND, int)> g_Import_inputsystem_USER32_GetWindowLongW("GetWindowLongPtrW", &new_GetWindowLongPtrW);
CAfxImportFuncHook<LONG_PTR(WINAPI*)(HWND, int, LONG_PTR)> g_Import_inputsystem_USER32_SetWindowLongW("SetWindowLongPtrW", &new_SetWindowLongPtrW);
CAfxImportFuncHook<HCURSOR(WINAPI*)(HCURSOR)> g_Import_inputsystem_USER32_SetCursor("SetCursor", &new_SetCursor);
CAfxImportFuncHook<int(WINAPI*)(BOOL)> g_Import_inputsystem_USER32_ShowCursor("ShowCursor", &new_ShowCursor);
CAfxImportFuncHook<HWND(WINAPI*)(HWND)> g_Import_inputsystem_USER32_SetCapture("SetCapture", &new_SetCapture);
CAfxImportFuncHook<BOOL(WINAPI*)()> g_Import_inputsystem_USER32_ReleaseCapture("ReleaseCapture", &new_ReleaseCapture);
CAfxImportFuncHook<BOOL(WINAPI*)(const RECT*)> g_Import_inputsystem_USER32_ClipCursor("ClipCursor", &new_ClipCursor);
CAfxImportFuncHook<BOOL(WINAPI*)(LPPOINT)> g_Import_inputsystem_USER32_GetCursorPos("GetCursorPos", &new_GetCursorPos);
CAfxImportFuncHook<BOOL(WINAPI*)(int, int)> g_Import_inputsystem_USER32_SetCursorPos("SetCursorPos", &new_SetCursorPos);

CAfxImportDllHook g_Import_inputsystem_USER32("USER32.dll", CAfxImportDllHooks({
	&g_Import_inputsystem_USER32_GetWindowLongW,
	&g_Import_inputsystem_USER32_SetWindowLongW,
	&g_Import_inputsystem_USER32_SetCursor,
	&g_Import_inputsystem_USER32_ShowCursor,
	&g_Import_inputsystem_USER32_SetCapture,
	&g_Import_inputsystem_USER32_ReleaseCapture,
	&g_Import_inputsystem_USER32_ClipCursor,
	&g_Import_inputsystem_USER32_GetCursorPos,
	&g_Import_inputsystem_USER32_SetCursorPos }));

CAfxImportsHook g_Import_inputsystem(CAfxImportsHooks({
	&g_Import_inputsystem_USER32 }));

//CAfxImportDllHook g_Import_client_steam_api64("steam_api64.dll", CAfxImportDllHooks({
//	&g_Import_client_steam_api64_SteamInternal_FindOrCreateUserInterface }));
//
//CAfxImportsHook g_Import_client(CAfxImportsHooks({
//&g_Import_client_steam_api64 }));

void LibraryHooksA(HMODULE hModule, LPCSTR lpLibFileName)
{
	CommonHooks();

	if(!hModule || !lpLibFileName)
		return;

#if 0
	static FILE *f1=NULL;

	if( !f1 ) f1=fopen("hlae_log_LibraryHooksA.txt","wb");
	fprintf(f1,"%s\n", lpLibFileName);
	fflush(f1);
#endif
}

extern void GetClientDllSceneObjectVtable(HMODULE clientDll);
extern void GetParticlesDllSceneObjectVtable(HMODULE particlesDll);

void LibraryHooksW(HMODULE hModule, LPCWSTR lpLibFileName)
{
	static bool bFirstTier0 = true;
	static bool bFirstClient = true;
	static bool bFirstEngine2 = true;
	static bool bFirstfilesystem_stdio = true;
	static bool bFirstMaterialsystem2 = true;
	static bool bFirstInputsystem = true;
	static bool bFirstSDL3 = true;
	static bool bFirstRenderSystemDX11 = true;
	static bool bFirstPanorama = true;
	static bool bFirstSchemaSystem = true;
	static bool bFirstSceneSystem = true;
	static bool bFirstResourceSystem = true;
	static bool bFirstParticles = true;
	
	CommonHooks();

	if (!hModule || !lpLibFileName)
		return;

#if 0
	static FILE *f1 = NULL;

	if (!f1) f1 = fopen("hlae_log_LibraryHooksW.txt", "wb");
	fwprintf(f1, L"%s\n", lpLibFileName);
	fflush(f1);
#endif


	if(bFirstTier0 && StringEndsWithW( lpLibFileName, L"tier0.dll"))
	{
		bFirstTier0 = false;
		
		g_Import_tier0.Apply(hModule);

		SOURCESDK::CS2::g_pMemAlloc = *(SOURCESDK::CS2::IMemAlloc **)GetProcAddress(hModule, "g_pMemAlloc");

		if(Tier0_Message = (Tier0MsgFn)GetProcAddress(hModule, "Msg"))
			advancedfx::Message = My_Console_Message;
		else
			ErrorBox(MkErrStr(__FILE__, __LINE__));
		if(Tier0_Warning = (Tier0MsgFn)GetProcAddress(hModule, "Warning"))
			advancedfx::Warning = My_Console_Warning;
		else
			ErrorBox(MkErrStr(__FILE__, __LINE__));
		if(Tier0_DevMessage = (Tier0DevMsgFn)GetProcAddress(hModule, "DevMsg"))
			advancedfx::DevMessage = My_Console_DevMessage;
		else
			ErrorBox(MkErrStr(__FILE__, __LINE__));
		if(Tier0_DevWarning = (Tier0DevMsgFn)GetProcAddress(hModule, "DevWarning"))
			advancedfx::DevWarning = My_Console_DevWarning;
		else
			ErrorBox(MkErrStr(__FILE__, __LINE__));
	}
	else if(bFirstfilesystem_stdio && StringEndsWithW( lpLibFileName, L"filesystem_stdio.dll"))
	{
		bFirstfilesystem_stdio = false;

		g_H_FileSystem_stdio = hModule;

		org_AddSearchPath = (AddSearchPath_t)getVTableFn(hModule, 31, ".?AVCFileSystem_Stdio@@");
		if (0 == org_AddSearchPath) ErrorBox(MkErrStr(__FILE__, __LINE__));

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		
		DetourAttach(&(PVOID&)org_AddSearchPath, new_AddSearchPath);
		
		if(NO_ERROR != DetourTransactionCommit()) ErrorBox("Failed to detour filesystem_stdio functions.");
		
		// g_Import_filesystem_stdio.Apply(hModule);
	}
	else if(bFirstInputsystem && StringEndsWithW(lpLibFileName, L"inputsystem.dll"))
	{
		bFirstInputsystem = false;

		// The USER32 import hooks below are the live-UI cursor lever: new_SetCursorPos
		// swallows the engine's re-centring warps while the Config sidebar owns the mouse.
		// No inputsystem vtable slot is detoured here on purpose -- see the note next to
		// New_CCSGOInput_MouseInputEnabled.
		g_Import_inputsystem.Apply(hModule);
	}	
	else if(bFirstSDL3 && StringEndsWithW(lpLibFileName, L"SDL3.dll"))
	{
		bFirstSDL3 = false;

		g_Import_SDL3.Apply(hModule);
	}
	else if(bFirstEngine2 && StringEndsWithW( lpLibFileName, L"engine2.dll"))
	{
		bFirstEngine2 = false;

		g_h_engine2Dll = hModule;

		Addresses_InitEngine2Dll((AfxAddr)hModule);

		HookEngineDll(hModule);

		g_Import_engine2.Apply(hModule);
		g_Import_engine2_ClientRect.Apply(hModule);

		Hook_Engine_RenderService();

		Hook_Engine__HostStateRequest_Start();
	}
	else if(bFirstSceneSystem && StringEndsWithW( lpLibFileName, L"scenesystem.dll"))
	{
		bFirstSceneSystem = false;

		Addresses_InitSceneSystemDll((AfxAddr)hModule);

		g_Import_SceneSystem.Apply(hModule);
		Hook_SceneSystem(hModule);
		HookSceneSystem(hModule);
	}
	else if(bFirstMaterialsystem2 && StringEndsWithW( lpLibFileName, L"materialsystem2.dll"))
	{
		bFirstMaterialsystem2 = false;

		HookMaterialSystem(hModule);

		// g_Import_materialsystem2.Apply(hModule);
	}
	else if(bFirstRenderSystemDX11 && StringEndsWithW( lpLibFileName, L"rendersystemdx11.dll"))
	{
		bFirstRenderSystemDX11 = false;

		g_Import_rsdx11_ClientRect.Apply(hModule);

		Hook_RenderSystemDX11((void*)hModule);
	}
	else if(bFirstClient && StringEndsWithW(lpLibFileName, L"csgo\\bin\\win64\\client.dll"))
	{
		bFirstClient = false;

		g_H_ClientDll = hModule;

		Addresses_InitClientDll((AfxAddr)hModule);

		//if(!g_Import_client.Apply(hModule)) ErrorBox("client.dll steam_api64 hooks failed.");

		HookMirvColors(hModule);

		HookMirvCommands(hModule);

		HookViewmodel(hModule);

		HookDeathMsg(hModule);

		HookReplaceName(hModule);

		HookClientDll(hModule);

		Hook_ClientEntitySystem3(hModule);

		GetClientDllSceneObjectVtable(hModule);
	} 
	else if(bFirstPanorama && StringEndsWithW(lpLibFileName, L"panorama.dll"))
	{
		bFirstPanorama = false;
		g_Import_panorama.Apply(hModule);
		HookPanorama(hModule);
	}
	else if(bFirstSchemaSystem && StringEndsWithW(lpLibFileName, L"schemasystem.dll"))
	{
		bFirstSchemaSystem = false;
		g_H_SchemaSystem = hModule;
	}
	else if(bFirstResourceSystem && StringEndsWithW(lpLibFileName, L"resourcesystem.dll"))
	{
		bFirstResourceSystem = false;
		g_H_ResourceSystemDll = hModule;
	}
	else if(bFirstParticles && StringEndsWithW(lpLibFileName, L"particles.dll"))
	{
		bFirstParticles = false;
		GetParticlesDllSceneObjectVtable(hModule);
	}
}

HMODULE WINAPI new_LoadLibraryA( LPCSTR lpLibFileName ) {
	HMODULE hRet = LoadLibraryA(lpLibFileName);

	LibraryHooksA(hRet, lpLibFileName);

	return hRet;
}

HMODULE WINAPI new_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
	HMODULE hRet = LoadLibraryExA(lpLibFileName, hFile, dwFlags);

	LibraryHooksA(hRet, lpLibFileName);

	return hRet;
}

// RenderDoc coexistence. The import-table patches in this file are not enough once RenderDoc is in
// the process: it interposes its own wrappers on the same kernel32 import slots, and rewrites them
// again later. The hook then never sees tier0/engine2/client load, mirv_* is never registered and
// g_pCVar stays null, so the first call into a client hook crashes the game. The two kernel32
// entry points below are therefore also hooked inline: every caller, RenderDoc's wrappers included,
// ends up in the real function body.
#include <intrin.h>

void RenderSystemDX11_ReassertImports(); // RenderSystemDX11Hooks.cpp

static HMODULE(WINAPI *g_Org_LoadLibraryExW_Inline)(LPCWSTR, HANDLE, DWORD) = nullptr;

static HMODULE WINAPI New_LoadLibraryExW_Inline(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
	HMODULE hRet = g_Org_LoadLibraryExW_Inline(lpLibFileName, hFile, dwFlags);
	LibraryHooksW(hRet, lpLibFileName);
	RenderSystemDX11_ReassertImports();
	return hRet;
}

// tier0's GetProcAddress import is the one that installs our CreateInterface wrappers. Only calls
// made by tier0 are filtered, as with the import hook. RenderDoc puts its own GetProcAddress
// between tier0 and kernel32, so the caller seen here is then renderdoc.dll; its CreateInterface
// lookups are tier0's, forwarded.
static FARPROC(WINAPI *g_Org_GetProcAddress_Inline)(HMODULE, LPCSTR) = nullptr;

static FARPROC WINAPI New_GetProcAddress_Inline(HMODULE hModule, LPCSTR lpProcName) {
	FARPROC result = g_Org_GetProcAddress_Inline(hModule, lpProcName);
	if (result && HIWORD(lpProcName)) {
		HMODULE caller = nullptr;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCWSTR)_ReturnAddress(), &caller) && caller) {
			const bool fromTier0 = caller == GetModuleHandleW(L"tier0.dll");
			const bool fromRenderDoc = caller == GetModuleHandleW(L"renderdoc.dll");
			if (fromTier0 || (fromRenderDoc && 0 == lstrcmpA(lpProcName, "CreateInterface")))
				return Tier0GetProcAddressFilter(hModule, lpProcName, result);
		}
	}
	return result;
}

static void InstallInlineKernel32Hooks() {
	HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
	if (!kernel32) return;
	g_Org_LoadLibraryExW_Inline = (decltype(g_Org_LoadLibraryExW_Inline))GetProcAddress(kernel32, "LoadLibraryExW");
	g_Org_GetProcAddress_Inline = (decltype(g_Org_GetProcAddress_Inline))GetProcAddress(kernel32, "GetProcAddress");
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	if (g_Org_LoadLibraryExW_Inline) DetourAttach(&(PVOID &)g_Org_LoadLibraryExW_Inline, New_LoadLibraryExW_Inline);
	if (g_Org_GetProcAddress_Inline) DetourAttach(&(PVOID &)g_Org_GetProcAddress_Inline, New_GetProcAddress_Inline);
	DetourTransactionCommit();
}

HMODULE WINAPI new_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
	HMODULE hRet = LoadLibraryExW(lpLibFileName, hFile, dwFlags);

	LibraryHooksW(hRet, lpLibFileName);

	return hRet;
}

CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR)> g_Import_PROCESS_KERNEL32_LoadLibraryA("LoadLibraryA", &new_LoadLibraryA);
CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD)> g_Import_PROCESS_KERNEL32_LoadLibraryExA("LoadLibraryExA", &new_LoadLibraryExA);
CAfxImportFuncHook<HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD)> g_Import_PROCESS_KERNEL32_LoadLibraryExW("LoadLibraryExW", &new_LoadLibraryExW);

CAfxImportDllHook g_Import_PROCESS_KERNEL32("KERNEL32.dll", CAfxImportDllHooks({
	&g_Import_PROCESS_KERNEL32_LoadLibraryA
	, &g_Import_PROCESS_KERNEL32_LoadLibraryExA
	, &g_Import_PROCESS_KERNEL32_LoadLibraryExW }));

CAfxImportsHook g_Import_PROCESS(CAfxImportsHooks({
	&g_Import_PROCESS_KERNEL32 }));


advancedfx::CThreadPool * g_pThreadPool = nullptr;
advancedfx::CGrowingBufferPoolThreadSafe * g_pImageBufferPoolThreadSafe = nullptr;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved)
{
	switch (fdwReason) 
	{ 
		case DLL_PROCESS_ATTACH:
		{
			g_CommandLine = new advancedfx::CCommandLine();
			// CLICK-TO-CAPTURE IS THE DEFAULT, not a launch option.
			//
			// This used to be `FindParam(L"-afxFreeMouse") != 0`, so whether CS2 stole the desktop
			// cursor on startup depended entirely on how the game happened to be launched -- one
			// shortcut or automation script passing the flag and another not, for the same build.
			// That is not a preference, it is the tool behaving differently for no visible reason;
			// an offline movie-making session should never take the mouse until you actually click
			// into the game. So it is native now: free until the first real game click, re-armed
			// whenever the window loses focus, and unaffected by first / third / free cam.
			//
			// `-afxFreeMouse` is still accepted and does nothing (existing shortcuts stay valid).
			// `-afxGrabMouse` is the explicit opt-OUT for anyone who wants CS2's normal
			// grab-on-focus behaviour back; `mirv_filmmaker mouse game` does the same at runtime.
			Filmmaker::MouseCapture_Initialize(
				g_CommandLine->FindParam(L"-afxGrabMouse") == 0);

			if(!g_CommandLine->FindParam(L"-insecure"))
			{
				ErrorBox("Please add -insecure to launch options, AfxHookSource2 will refuse to work without it!");

				HANDLE hproc = OpenProcess(PROCESS_TERMINATE, true, GetCurrentProcessId());
				TerminateProcess(hproc, 0);
				CloseHandle(hproc);
				
				do MessageBoxA(NULL, "Please terminate the game manually in the taskmanager!", "Cannot terminate, please help:", MB_OK | MB_ICONERROR);
				while (true);
			}

#if _DEBUG
			MessageBox(0,"DLL_PROCESS_ATTACH","MDT_DEBUG",MB_OK);
#endif
			g_Import_PROCESS.Apply(GetModuleHandle(NULL));
			InstallInlineKernel32Hooks();

			if (!(g_Import_PROCESS_KERNEL32_LoadLibraryA.TrueFunc || g_Import_PROCESS_KERNEL32_LoadLibraryExA.TrueFunc || g_Import_PROCESS_KERNEL32_LoadLibraryExW.TrueFunc))
				ErrorBox();

			// Automatic crash reports: handlers only here (kernel32 work); the session starts on the
			// first main-thread frame. Filmmaker/Community/CrashReport.h.
			Filmmaker::CrashReport_InstallHandlers();


			//
			// Remember we are not on the main program thread here,
			// instead we are on our own thread, so don't run
			// things here that would have problems with that.
			//

			size_t thread_pool_thread_count = advancedfx::CThreadPool::GetDefaultThreadCount();
			if (int idx = g_CommandLine->FindParam(L"-afxThreadPoolSize")) {
				if (idx + 1 < g_CommandLine->GetArgC()) {
					thread_pool_thread_count = (size_t)wcstoul( g_CommandLine->GetArgV(idx + 1), nullptr, 10);
				}
			}
			g_pThreadPool = new advancedfx::CThreadPool(thread_pool_thread_count);

			g_pImageBufferPoolThreadSafe = new advancedfx::CGrowingBufferPoolThreadSafe();

			g_ConsolePrinter = new CConsolePrinter();

			g_CampathDrawer.Begin();

			if (int idx = g_CommandLine->FindParam(L"-afxFixNetCon")) {
				// https://github.com/ValveSoftware/csgo-osx-linux/issues/3603#issuecomment-2163695087

				WORD wVersionRequested;
    			WSADATA wsaData;
    			int err;

				wVersionRequested = MAKEWORD(2, 0);

				err = WSAStartup(wVersionRequested, &wsaData);
    			if (err != 0) {
					ErrorBox("WSAStartup failed");
			    }
			
			    if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 0) {
			        ErrorBox("Could not find a usable version of Winsock.dll");
        			WSACleanup();
				}		
			}

			break;
		}
		case DLL_PROCESS_DETACH:
		{
			// actually this gets called now.
			// A clean exit (quit): not something to report next launch.
			Filmmaker::CrashReport_CleanExit();
			Filmmaker::MvmConsoleCapture_Stop();
			// Process exit (lpReserved set): CS2 has already shut down and saved, ExitProcess has ended
			// every other thread, and the clean exit is recorded above. Nothing our static destructors
			// would do is still safe: a finished std::thread is still joinable and its destructor calls
			// std::terminate -> abort() (crash reports 2026-10-12/13, FollowCamera and demo workers),
			// and our D3D objects hold the device past the renderer's shutdown, so their final Release
			// runs the driver's teardown on killed threads (ATIDXX64 crash on quit, 2026-10-13) or
			// RenderDoc's already freed wrappers. End the process here.
			if (lpReserved)
				TerminateProcess(GetCurrentProcess(), 0);

			g_CampathDrawer.End();

			g_S2CamIO.ShutDown();

			delete g_ConsolePrinter;

			delete g_pImageBufferPoolThreadSafe;

			delete g_pThreadPool;

#ifdef _DEBUG
			_CrtDumpMemoryLeaks();
#endif

			break;
		}
		case DLL_THREAD_ATTACH:
		{
			break;
		}
		case DLL_THREAD_DETACH:
		{
			break;
		}
	}
	return TRUE;
}
