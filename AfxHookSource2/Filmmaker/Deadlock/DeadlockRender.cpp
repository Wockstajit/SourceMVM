#include "DeadlockRender.h"
#include "DeadlockRenderMath.h"

#include "DeadlockShot.h"
#include "DeadlockPlaybackLoop.h"
#include "DeadlockFollowCamera.h"
#include "../Movie/CamMarkers.h"
#include "DeadlockRenderEncoder.h"
#include "DeadlockWorkspaces.h"
#include "../Render/RenderCaptureMath.h"
#include "../../Deadlock/DeadlockCameraRuntime.h"
#include "../../Deadlock/DeadlockDemo.h"
#include "../../Deadlock/DeadlockIcvarSlots.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/FolderPicker.h"
#include <mutex>
#include <thread>

#include "../Debug/MvmDebugLog.h"
#include "../Movie/CameraBridge.h"
#include "../../Deadlock/DeadlockEngine.h"
#include "../../../Shared/AfxConsole.h"

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <shlobj.h>
#include <wincodec.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>

#pragma comment(lib, "windowscodecs.lib")

namespace Filmmaker {

namespace {

RenderState g_state = RenderState::Idle;
RenderSettings g_settings;
RenderSettings g_job;
std::mutex g_captureMutex;
std::atomic<bool> g_writeFailed{false};
double g_startTime = 0.0;
double g_oldFramerate = 0.0, g_oldScale = 1.0;
bool g_oldHud = false, g_oldFree = false, g_oldFollow = false, g_oldLoop = false;
double g_cameraOrigin[3]{}, g_cameraAngles[3]{}, g_cameraFov = 90;
bool g_restoreClock = false;
bool g_wasPaused = false;
DeadlockWorkspace g_oldWorkspace = DeadlockWorkspace::Off;

bool ReadHostFramerate(double& value) {
    __try {
        for (size_t i = 0; i < 65536; ++i) {
            auto* cv = Deadlock::Icvar_GetCvar(SOURCESDK::CS2::g_pCVar, i);
            if (!cv) break;
            if (cv->m_pszName && 0 == strcmp(cv->m_pszName, "host_framerate")) {
                if (cv->m_eVarType == SOURCESDK::CS2::EConVarType_Float32) value = cv->m_Value.m_flValue;
                else if (cv->m_eVarType == SOURCESDK::CS2::EConVarType_Float64) value = cv->m_Value.m_dbValue;
                else return false;
                return std::isfinite(value);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
void RestoreClock() {
    if (!g_restoreClock) return;
    char command[96];
    std::snprintf(command, sizeof(command), "host_framerate %.9g", g_oldFramerate);
    Deadlock::Engine_ExecuteClientCmd(command);
    Deadlock::Engine_ExecuteClientCmd((g_wasPaused || (g_job.pauseOnDrop && g_writeFailed.load())) ? "demo_pause" : "demo_resume");
    std::snprintf(command, sizeof(command), "demo_timescale %.9g", g_oldScale);
    Deadlock::Engine_ExecuteClientCmd(command);
    PlaybackLoop_SetEnabled(g_oldLoop);
    Workspaces_SetHudHidden(g_oldHud);
    if (g_job.camSrc == "path") Shot_Pause();
    FollowCamera_SetOn(g_oldFollow);
    CameraBridge_SetFreeCamEnabled(g_oldFree);
    Workspaces_Set(g_oldWorkspace);
    g_restoreClock = false;
}
std::atomic<bool> g_folderPending{false};
std::mutex g_folderMutex;
std::wstring g_chosenFolder;
bool g_folderReady = false;

std::atomic<unsigned long long> g_framesWritten{ 0 };
std::atomic<unsigned long long> g_framesDropped{ 0 };

// The capture cursor, in demo ticks. Advanced by exactly one output frame's worth of ticks per
// written frame, never by the observed frame delta -- that is what makes a start deterministic and
// a re-capture of the same range byte-identical in framing.
std::atomic<double> g_nextCaptureTick{ 0.0 };
double g_ticksPerFrame = 0.0;
int g_captureFps = 32;
double g_jobTimescale = 1.0;

int g_takeNumber = 0;
std::wstring g_takeDir;

// The render thread reads these; the main thread writes them. A capture is a coarse state change
// rather than a per-frame handshake, so an atomic flag plus a tick target is enough.
std::atomic<bool> g_captureArmed{ false };
uint64_t g_frameLimit = 0;

std::wstring DefaultOutputRoot() {
	PWSTR videos = nullptr;
	std::wstring root;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos)) && videos) {
		root.assign(videos);
		CoTaskMemFree(videos);
	} else {
		root = L".";
	}
	root += L"\\SourceMVM\\Deadlock";
	return root;
}

// Creates every missing component of a path. SHCreateDirectory would do it, but it rejects
// relative paths, and the fallback root above can be relative.
bool EnsureDirectory(const std::wstring& path) {
	if (path.empty()) return false;
	if (CreateDirectoryW(path.c_str(), nullptr)) return true;
	const DWORD err = GetLastError();
	if (err == ERROR_ALREADY_EXISTS) return true;
	if (err != ERROR_PATH_NOT_FOUND) return false;

	const size_t slash = path.find_last_of(L"\\/");
	if (slash == std::wstring::npos || slash == 0) return false;
	if (!EnsureDirectory(path.substr(0, slash))) return false;
	return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string Utf8(const std::wstring& value) {
	if (value.empty()) return std::string();
	const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(),
	                                      nullptr, 0, nullptr, nullptr);
	if (bytes <= 0) return std::string();
	std::string result((size_t)bytes, '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(),
	                    result.data(), bytes, nullptr, nullptr);
	return result;
}

// Next unused take directory. Numbered rather than timestamped so a sequence of takes sorts the
// way it was shot.
bool AllocateTakeDir() {
	std::wstring root = g_settings.outputDir.empty() ? DefaultOutputRoot() : g_settings.outputDir;
	if (!EnsureDirectory(root)) {
		advancedfx::Warning("AfxHookDeadlock: could not create the output directory.\n");
		return false;
	}
	for (int take = 1; take <= 9999; ++take) {
		wchar_t buf[32];
		std::swprintf(buf, 32, L"\\take%04d", take);
		std::wstring candidate = root + buf;
		if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
			if (!EnsureDirectory(candidate)) return false;
			g_takeDir = candidate;
			g_takeNumber = take;
			return true;
		}
	}
	return false;
}

// Copies the back buffer into a staging texture and writes it. Called on the render thread.
bool CaptureBackBuffer(void* swapChainPtr) {
	IDXGISwapChain* swapChain = (IDXGISwapChain*)swapChainPtr;
	if (!swapChain) return false;

	ID3D11Texture2D* backBuffer = nullptr;
	if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || !backBuffer) return false;

	ID3D11Device* device = nullptr;
	backBuffer->GetDevice(&device);
	if (!device) {
		backBuffer->Release();
		return false;
	}
	ID3D11DeviceContext* context = nullptr;
	device->GetImmediateContext(&context);

	D3D11_TEXTURE2D_DESC desc = {};
	backBuffer->GetDesc(&desc);

	D3D11_TEXTURE2D_DESC staging = desc;
	staging.Usage = D3D11_USAGE_STAGING;
	staging.BindFlags = 0;
	staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	staging.MiscFlags = 0;
	staging.SampleDesc.Count = 1;
	staging.SampleDesc.Quality = 0;

	ID3D11Texture2D* staged = nullptr;
	bool ok = false;
	if (context && SUCCEEDED(device->CreateTexture2D(&staging, nullptr, &staged)) && staged) {
		if (desc.SampleDesc.Count > 1) {
			// A multisampled back buffer cannot be copied straight to staging. Deadlock's
			// gameinfo sets SupportsMSAA 0, so this is a guard rather than a code path in use.
			context->ResolveSubresource(staged, 0, backBuffer, 0, desc.Format);
		} else {
			context->CopyResource(staged, backBuffer);
		}
		D3D11_MAPPED_SUBRESOURCE mapped{};
        const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if ((bgra || rgba) && SUCCEEDED(context->Map(staged, 0, D3D11_MAP_READ, 0, &mapped))) {
            ok = DeadlockEncoder_Write(mapped.pData, mapped.RowPitch, desc.Width, desc.Height, bgra);
            context->Unmap(staged, 0);
        }
	}

	if (staged) staged->Release();
	if (context) context->Release();
	device->Release();
	backBuffer->Release();
	return ok;
}

} // namespace

void Render_RunMainThreadFrame() {
    {
        std::lock_guard<std::mutex> lock(g_folderMutex);
        if (g_folderReady) {
            g_settings.outputDir.swap(g_chosenFolder);
            g_folderReady = false;
        }
    }
	if (g_state == RenderState::Idle || g_state == RenderState::Failed) return;

	const int tick = Deadlock::Engine_DemoTick();
	if (tick < 0 && g_state != RenderState::Finishing) { Render_Stop(); return; }

	switch (g_state) {
	case RenderState::Arming:
		// Zero residue: wait for the demo to actually reach the start tick rather than beginning
		// wherever the frame landed. Otherwise the same range captured twice is framed differently.
		if (!Deadlock::Demo_IsSeeking() && tick >= g_job.startTick) {
            if (g_job.camSrc == "path") Shot_Play();
            else if (g_job.camSrc == "follow") FollowCamera_SetOn(true);
            else if (g_job.camSrc == "free" || (g_job.camSrc == "current" && g_oldFree)) {
                CameraBridge_SetFreeCamEnabled(true);
                CameraBridge_SetCameraPose(g_cameraOrigin[0],g_cameraOrigin[1],g_cameraOrigin[2],g_cameraAngles[0],g_cameraAngles[1],g_cameraAngles[2],g_cameraFov);
            }
            g_startTime = Deadlock::Engine_DemoTime();
			g_nextCaptureTick.store((double)g_job.startTick);
			g_captureArmed.store(true);
            Deadlock::Engine_ExecuteClientCmd("demo_resume");
			g_state = RenderState::Capturing;
			advancedfx::Message("AfxHookDeadlock: capturing from tick %d at %d fps output (%d capture).\n",
			                    g_job.startTick, g_job.fps, g_captureFps);
		}
		break;

	case RenderState::Capturing: {
		const bool atEnd = g_frameLimit && g_framesWritten.load() >= g_frameLimit;
		if (!Deadlock::Engine_IsPlayingDemo() || g_writeFailed.load() || atEnd) {
			g_captureArmed.store(false);
			g_state = RenderState::Finishing;
		}
		break;
	}

    case RenderState::Finishing: {
        std::lock_guard<std::mutex> captureLock(g_captureMutex);
        g_captureArmed.store(false);
        DeadlockEncoder_Finish();
        const int done = DeadlockEncoder_Poll();
        if (!done) break;
        CameraBridge_SetPathDrawSuppressed(false);
        RestoreClock();
        g_state = done < 0 || g_writeFailed.load() ? RenderState::Failed : RenderState::Idle;
        if (g_job.beep && g_state == RenderState::Idle) MessageBeep(MB_OK);
        advancedfx::Message("AfxHookDeadlock: take finished, %llu frames, %llu dropped. %s\n",
            g_framesWritten.load(), g_framesDropped.load(), DeadlockEncoder_Error());
        break;
    }

	default:
		break;
	}
}

bool Render_RunRenderFrame(void* swapChain) {
	std::lock_guard<std::mutex> captureLock(g_captureMutex);
	if (!g_captureArmed.load()) return false;

	const int tick = Deadlock::Engine_DemoTick();
	if (tick < 0) return false;
    if (g_frameLimit && g_framesWritten.load() >= g_frameLimit) return false;

    // host_framerate == captureFps == job.fps; file muxes at the same rate for 1x real-time watch.
    const double targetTime = g_startTime + (double)g_framesWritten.load() / (double)g_captureFps;
    if (Deadlock::Engine_DemoTime() + 0.0001 < targetTime) return false;

	if (CaptureBackBuffer(swapChain)) {
		g_framesWritten.fetch_add(1);
	} else {
		// Counted, not silently skipped: a capture with holes in it must be visible as such
		// rather than found later in the edit.
		g_framesDropped.fetch_add(1);
        g_writeFailed.store(true);
        g_captureArmed.store(false);
	}

	// Advance by exactly one output frame in ticks, from the target rather than from the observed
	// tick. Advancing from the observed tick would let a late frame push the whole sequence.
	const double next = g_job.startTick + g_framesWritten.load() * g_ticksPerFrame;
	g_nextCaptureTick.store(next);
	return true;
}

RenderState Render_GetState() { return g_state; }

const char* Render_StateName(RenderState state) {
	switch (state) {
	case RenderState::Idle: return "Idle";
	case RenderState::Arming: return "Arming";
	case RenderState::Capturing: return "Capturing";
	case RenderState::Finishing: return "Finishing";
	case RenderState::Failed: return "Failed";
	}
	return "?";
}

RenderSettings Render_GetSettings() { return g_settings; }

void Render_SetFps(int fps) {
	if (fps <= 0 || fps > 1000) return;
	g_settings.fps = fps;
	advancedfx::Message("AfxHookDeadlock: render fps = %d\n", fps);
}

void Render_SetRange(int startTick, int endTick) {
	g_settings.startTick = startTick;
	g_settings.endTick = endTick;
    g_settings.range = "custom";
}

void Render_SetOutputDir(const std::wstring& dir) { g_settings.outputDir = dir; }

void Render_PickFolder() {
    if (g_folderPending.exchange(true)) return;
    std::thread([] {
        std::wstring chosen;
        if (PickFolderBlocking(chosen, L"Select the Deadlock render output folder")) {
            std::lock_guard<std::mutex> lock(g_folderMutex);
            g_chosenFolder = chosen;
            g_folderReady = true;
        }
        g_folderPending.store(false);
    }).detach();
}

bool Render_Start() {
	if (g_state != RenderState::Idle && g_state != RenderState::Failed) {
		advancedfx::Warning("AfxHookDeadlock: a capture is already running.\n");
		return false;
	}

	if (!Deadlock::Engine_IsPlayingDemo()) {
		advancedfx::Warning("AfxHookDeadlock: no demo is playing.\n");
		return false;
	}

	// The two timing preconditions, refused rather than warned about. Producing footage that
	// drifts against the demo clock is worse than refusing to start, because the drift is
	// invisible until it is cut against audio.
	if (!Deadlock::Engine_TickIntervalConfirmed()) {
		advancedfx::Warning(
			"AfxHookDeadlock: Deadlock's live tick interval is unavailable. Measure it and set\n"
			"'mirv_filmmaker engine tickinterval <seconds>' first.\n");
		return false;
	}

	const double tickRate = 1.0 / Deadlock::Engine_IntervalPerTick();

    g_job = g_settings;
    if (g_job.range == "all") { g_job.startTick = 0; g_job.endTick = 0; }
    else if (g_job.range == "shot") { g_job.startTick = Shot_FirstTick(); g_job.endTick = Shot_LastTick(); }
    else if (g_job.range == "segment") {
        const auto* first = Shot_Marker(Shot_Selected()); const auto* last = Shot_Marker(Shot_Selected()+1);
        if (!first || !last) { advancedfx::Warning("Select a camera segment first.\n"); return false; }
        g_job.startTick = first->tick; g_job.endTick = last->tick;
    } else if (g_job.range == "loop" && !PlaybackLoop_Range(g_job.startTick,g_job.endTick)) {
        advancedfx::Warning("Set a playback loop first.\n"); return false;
    }
    if (g_job.range != "all" && (g_job.startTick < 0 || g_job.endTick < g_job.startTick)) {
        advancedfx::Warning("Render Out must be at or after In.\n"); return false;
    }
    if ((g_job.range == "shot" || g_job.camSrc == "path") && Shot_MarkerCount()<2) return false;
    if (g_job.camSrc == "follow" && FollowCamera_Get().hero<0) return false;

	if (!AllocateTakeDir()) {
		g_state = RenderState::Failed;
		return false;
	}

    if (!ReadHostFramerate(g_oldFramerate)) {
        advancedfx::Warning("AfxHookDeadlock: cannot read host_framerate; capture cannot own and restore the clock.\n");
        return false;
    }
    int width = 0, height = 0;
    Deadlock::CameraRuntime_GetViewportSize(width, height);
    if (!DeadlockEncoder_Start(g_job, g_takeDir, width, height)) {
        advancedfx::Warning("AfxHookDeadlock: %s\n", DeadlockEncoder_Error());
        g_state = RenderState::Failed;
        return false;
    }
    g_writeFailed.store(false);
    g_wasPaused = Deadlock::Engine_IsDemoPaused();
    g_oldWorkspace = Workspaces_Current();
    g_oldScale = Deadlock::Demo_PlaybackScale();
    // Recording FPS is mux + sampling. Dial is preview-only; restore it after the job.
    g_jobTimescale = 1.0;
    g_captureFps = RenderCaptureFps(g_job.fps);
    g_ticksPerFrame = tickRate / (double)g_captureFps;
    g_oldHud = Workspaces_HudHidden(); g_oldFree = CameraBridge_GetFreeCamEnabled();
    g_oldFollow = FollowCamera_Get().on; g_oldLoop = PlaybackLoop_Enabled();
    CameraBridge_GetCurrentCamera(g_cameraOrigin,g_cameraAngles,g_cameraFov);
    PlaybackLoop_SetEnabled(false);
    if (g_job.camSrc != "current") { FollowCamera_SetOn(false); CameraBridge_SetFreeCamEnabled(false); }
    Workspaces_SetHudHidden(g_job.hud == "none");
    Deadlock::Engine_ExecuteClientCmd("demo_timescale 1");
    g_restoreClock = true;
    char clockCommand[96];
    std::snprintf(clockCommand, sizeof(clockCommand), "host_framerate %d", g_captureFps);
    Deadlock::Engine_ExecuteClientCmd(clockCommand);
    Workspaces_Set(DeadlockWorkspace::Off);
    Deadlock::Engine_ExecuteClientCmd("demo_pause");
    std::snprintf(clockCommand, sizeof(clockCommand), "demo_gototick %d", g_job.startTick);
    Deadlock::Engine_ExecuteClientCmd(clockCommand);
	g_framesWritten.store(0);
	g_framesDropped.store(0);
	g_frameLimit = g_job.range == "all" ? 0 : DeadlockRenderMath::FrameCount(g_job.startTick,g_job.endTick,Deadlock::Engine_IntervalPerTick(),g_captureFps);

	// Suppress the in-world keyframe gizmos for the lifetime of the job. They draw into the scene
	// target before the UI pass, so without this they land in every captured frame -- and this is
	// a separate flag from the user's own toggle so they return by themselves when the job ends.
	CameraBridge_SetPathDrawSuppressed(true);

	g_state = RenderState::Arming;
	advancedfx::Message(
		"AfxHookDeadlock: take %04d armed -- %d fps (host_framerate %d), %.4f ticks/frame, from tick %d.\n",
		g_takeNumber, g_job.fps, g_captureFps, g_ticksPerFrame, g_job.startTick);

	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("deadlock.render",
			"armed take=%d fps=%d captureFps=%d ticksPerFrame=%.3f start=%d end=%d tickRate=%.4f",
			g_takeNumber, g_job.fps, g_captureFps, g_ticksPerFrame, g_job.startTick,
			g_job.endTick, tickRate);
	return true;
}

void Render_Stop() {
    if (g_state == RenderState::Idle || g_state == RenderState::Failed) return;
    g_captureArmed.store(false);
    g_state = RenderState::Finishing;
}

unsigned long long Render_FramesWritten() { return g_framesWritten.load(); }
unsigned long long Render_FramesDropped() { return g_framesDropped.load(); }

// One option at a time. Each name is checked against the values the Render column actually offers,
// so the console cannot put the job into a shape the UI has no way to show.
bool Render_SetOption(const char* which, const char* value) {
	if (!which || !value || !*value) return false;
	auto oneOf = [&](std::initializer_list<const char*> allowed) {
		for (const char* a : allowed) if (0 == _stricmp(a, value)) return true;
		return false;
	};
	if (0 == _stricmp(which, "range")) {
		if (!oneOf({ "all", "shot", "segment", "loop", "custom" })) return false;
		g_settings.range = value;
		return true;
	}
	if (0 == _stricmp(which, "camsrc")) {
		if (!oneOf({ "current", "free", "path", "follow" })) return false;
		g_settings.camSrc = value;
		return true;
	}
	if (0 == _stricmp(which, "hud")) {
		if (!oneOf({ "none", "game", "full" })) return false;
		g_settings.hud = value;
		return true;
	}
    if (0 == _stricmp(which, "encoder")) {
        if (!oneOf({"auto", "cpu", "gpu"})) return false;
        g_settings.encoder = value; return true;
    }
    if (0 == _stricmp(which, "speed")) {
        if (!oneOf({"quality", "balanced", "fast"})) return false;
        g_settings.speed = value; return true;
    }
	if (0 == _stricmp(which, "format")) {
		if (!oneOf({ "png", "tga", "jpg", "mp4", "avi", "gif", "tiff", "bmp", "exr" })) return false;
		g_settings.format = value;
		return true;
	}
	return false;
}

bool Render_SetFlag(const char* which, bool value) {
	if (!which) return false;
	if (0 == _stricmp(which, "names")) { g_settings.names = value; return true; }
	if (0 == _stricmp(which, "damage")) { g_settings.damage = value; return true; }
	if (0 == _stricmp(which, "pauseondrop")) { g_settings.pauseOnDrop = value; return true; }
	if (0 == _stricmp(which, "beep")) { g_settings.beep = value; return true; }
	return false;
}

void Render_SetQuality(int quality) {
	if (quality < 0) quality = 0; else if (quality > 51) quality = 51;
	g_settings.quality = quality;
}


void Render_SetSize(int width, int height) {
    if ((width == 0 && height == 0) || (width >= 0 && height >= 0 && width != 1 && height != 1 && width <= 16384 && height <= 16384)) {
        g_settings.width = width; g_settings.height = height;
    }
}
void Render_SetBitrate(int kbps) { if (kbps >= 0 && kbps <= 400000) g_settings.bitrateKbps = kbps; }

// THE PER-FRAME HALF of the render state. Merged by the panel back onto st.render.
void Render_AppendHotJson(std::string& out) {
	char buf[256];
	std::snprintf(buf, sizeof(buf),
		",\"renderHot\":{\"state\":\"%s\",\"running\":%s,\"frames\":%llu"
		",\"frame\":%llu,\"dropped\":%llu,\"take\":%d}",
		Render_StateName(g_state),
		(g_state == RenderState::Capturing || g_state == RenderState::Arming
		 || g_state == RenderState::Finishing) ? "true" : "false",
		g_framesWritten.load(), g_framesWritten.load(), g_framesDropped.load(), g_takeNumber);
	out += buf;
    if (g_state == RenderState::Failed) {
        JsonBuilder error; error.BeginObject(); error.StringField("renderError", DeadlockEncoder_Error()); error.EndObject();
        const auto json = error.Str(); out += "," + json.substr(1,json.size()-2);
    }
}

void Render_AppendStateJson(std::string& out) {
    const auto& settings = (g_state == RenderState::Arming || g_state == RenderState::Capturing || g_state == RenderState::Finishing) ? g_job : g_settings;
	const int previewCapture = (g_state == RenderState::Arming || g_state == RenderState::Capturing || g_state == RenderState::Finishing)
		? g_captureFps : RenderCaptureFps(settings.fps);
	char buf[512];
	// The job's settings only. The live counters (state, running, frames, dropped, take) are in
	// Render_AppendHotJson: they tick every captured frame, and this block rides in the chunk the
	// workspace ships only when it changes. timescale is always 1 for render math (dial ignored).
	std::snprintf(buf, sizeof(buf),
		",\"render\":{\"fps\":%d,\"captureFps\":%d,\"timescale\":1,\"startTick\":%d,\"endTick\":%d",
		settings.fps, previewCapture, settings.startTick, settings.endTick);
	out += buf;

	// The job's shape. Written as the same strings the option rows use, so the panel compares
	// against its own vocabulary rather than mapping an enum back into words.
	std::snprintf(buf, sizeof(buf),
		",\"range\":\"%s\",\"camSrc\":\"%s\",\"hud\":\"%s\",\"format\":\"%s\""
		",\"quality\":%d,\"names\":%s,\"damage\":%s,\"pauseOnDrop\":%s,\"beep\":%s",
		settings.range.c_str(), settings.camSrc.c_str(),
		settings.hud.c_str(), settings.format.c_str(), settings.quality,
		settings.names ? "true" : "false",
		settings.damage ? "true" : "false",
		settings.pauseOnDrop ? "true" : "false",
		settings.beep ? "true" : "false");
	out += buf;

	// The output folder, shown in the render workspace so a capture is never started against a
	// path nobody has looked at. Escaped the same way the workspace escapes a level name: this
	// feeds one attribute read by one parser, and a Windows path is mostly backslashes.
	out += ",\"dir\":\"";
	{
		const std::string dir = Utf8(settings.outputDir.empty() ? DefaultOutputRoot() : settings.outputDir);
		for (char c : dir) {
			if (c == '\\') out += "\\\\";
			else if (c == '"') out += "\\\"";
			else if ((unsigned char)c >= 0x20) out += c;
		}
	}
	out += "\",\"encoder\":\"" + settings.encoder + "\",\"speed\":\"" + settings.speed + "\"";
    out += ",\"width\":" + std::to_string(settings.width) + ",\"height\":" + std::to_string(settings.height);
    out += ",\"bitrateKbps\":" + std::to_string(settings.bitrateKbps);
    out += ",\"folderPending\":";
	out += g_folderPending.load() ? "true}" : "false}";
}

void Render_Report() {
	advancedfx::Message(
		"  render:    %s  fps=%d  host_framerate=%d  ticksPerFrame=%.2f  frames=%llu  dropped=%llu\n",
		Render_StateName(g_state), g_settings.fps, g_captureFps, g_ticksPerFrame,
		g_framesWritten.load(), g_framesDropped.load());
	if (g_takeNumber > 0) {
		const std::string dir = Utf8(g_takeDir);
		advancedfx::Message("  take:      %04d  %s\n", g_takeNumber, dir.c_str());
	}
}

} // namespace Filmmaker
