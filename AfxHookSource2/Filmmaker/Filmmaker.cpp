#include "Filmmaker.h"
#include "FilmmakerHealth.h" // FilmmakerHealth_Heartbeat
#include "Props/Props.h"
#include "Environment/EnvironmentScene.h"
#include "../WrpConsole.h"

#include "Demo/DemoLibrary.h"
#include "Demo/DemoProjectPath.h"
#include "Demo/DemoProjectStore.h"
#include "Demo/DemoKeyframes.h"
#include "Demo/DemoRepair.h"
#include "Demo/DemoOrganizer.h"
#include "Config/DemoOrganizerStore.h" // NormalizeDemoKey, for re-keying a renamed demo
#include "Demo/PlayingDemoPath.h"
#include "Panorama/FilmmakerMenu.h"
#include "Panorama/VideoSettingsResolution.h"
#include "Panorama/MovieHud.h"
#include "Panorama/MarkerHud.h"
#include "Panorama/FxDebugHud.h"
#include "Panorama/CameraTimelineHud.h"
#include "Panorama/CameraEditorHud.h"
#include "Panorama/WorkspacePreview.h"
#include "Panorama/UiLiveReload.h"
#include "Panorama/AimCrosshairHud.h"
#include "Panorama/EasterEggOverlayHud.h"
#include "EasterEggs/EasterEggs.h"
#include "Panorama/ConfigHud.h"
#include "Movie/HudBlackout.h"
#include "Panorama/GraphEditorExperimentHud.h"
#include "Panorama/DemoBarButtons.h"
#include "Panorama/PanoramaPerfProbe.h" // main-thread UI cost probe (mvm_debug / `mirv_filmmaker perf`)
#include "PlayMenu/PlayMenuHud.h"
#include "Community/CommunityHud.h"
#include "Workshop/WorkshopOrganizer.h"
#include "Movie/CameraBridge.h"
#include "MouseCapture.h" // MouseCapture_ReleaseForUi: the UI cursor takes the mouse back from CS2
#include "Render/RenderCommand.h" // Render_IsJobActive: no keyframe rebuild during a render job
#include "Movie/TimelinePlayhead.h" // TimelinePlayhead_Scrubbing: no copy switch mid-drag
#include "../MirvTime.h"            // g_MirvTime: the tick a copy switch lands back on
#include "Debug/MvmDebugLog.h"      // MvmDebugLog_*: keyframes.switch
#include "../SceneSystem.h"
#include "../UiInputGate.h" // UiInputGate_Active: a foreign UI keeps the wheel
#include "../WindowInputHooks.h" // WindowInputHooks_MainThreadFrame: raw-input wheel notches

#include <cmath>
#include <filesystem> // RenameDemoFile moves the demo and its sidecars
#include "Platform/ShellOpen.h" // ShowInExplorerAsync
#include <system_error>
#include "Movie/MovieMode.h"
#include "Movie/CamPathShake.h"
#include "Movie/CameraPath.h"
#include "Movie/SceneKeys.h"
#include "Movie/FollowCamera.h"
#include "Movie/ViewFx.h"
#include "Movie/ViewFxVm.h"
#include "Movie/SmoothView.h"
#include "Movie/CameraShakeStack.h"
#include "Movie/ParticleFx.h"
#include "Movie/RagdollPhysics.h" // ragdoll gravity/friction/limit (convar-driven)
#include "World/WorldCommand.h" // World_RunFrame: lighting / atmosphere / weather modules
#include "Movie/RagdollWeapon.h"
#include "Movie/DeathAnimations.h"
#include "Movie/NervousSystem.h"
#include "Movie/MethodActing.h"
#include "Panorama/PreviewSceneProbe.h"
#include "Panorama/PreviewAnimClock.h"
#include "Panorama/PreviewAnimTick.h"
#include "Panorama/PreviewRenderProbe.h"
#include "Panorama/PreviewRenderFlag.h"
#include "Panorama/DeathAnimPreview.h"
#include "Props/PropPreview.h"
#include "Movie/DemoEndHold.h"
#include "Movie/PlaybackLoop.h"
#include "Movie/DemoScrub.h"
#include "Movie/WeaponBobProbe.h"
#include "Render/RenderSessionDriver.h"
#include "Render/RenderEngineSize.h"
#include "Render/RenderWorldOnly.h"
#include "Render/RenderWorldPlayersNoGun.h"
#include "Render/RenderPlayersOnlyGreen.h"
#include "Window/WindowResize.h"
#include "Window/UnfocusedRate.h"
#include "../EngineLogQuiet.h"
#include "../FisheyePass.h" // AfxFisheye::SetRequest â€” fisheye lens D3D pass (decoupled from deleted ActionCam)
#include "Cosmetics/CosmeticOverrideSystem.h"
#include "Cosmetics/CosmeticUiQueries.h"
#include "Config/FilmmakerSettingsStore.h"
#include "Config/EffectsPersistence.h" // EFFECTS state carry-over across a demo change
#include "Platform/FolderPicker.h"
#include "Platform/DemoFilePicker.h"
#include "Platform/TextEncoding.h"

#include "../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../../Shared/AfxConsole.h"

#include <Windows.h>
#include <shellapi.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>

// Provided by main.cpp.
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

DemoLibrary g_library;
FilmmakerMenu g_menu;

std::once_flag g_initOnce;
std::atomic<bool> g_rescanRequested{ false };

// Hide the first-person viewmodel + suppress its _fp weapon FX while the free cam is detached.
// OFF by default: hiding them removes the arms/gun you fly the free cam INTO for an
// inside-the-player shot, which is a normal filmmaking move. `mirv_filmmaker fphide on`
// restores the old behaviour (no floating arms / camera-anchored muzzle FX at distance).
bool g_fpHideWhenDetached = false;

// --- Fisheye lens toggle (decoupled from deleted ActionCam) ---
bool s_fisheyeEnabled = false;
bool s_fisheyeRequested = false;

void PublishFisheye() {
	if (s_fisheyeEnabled == s_fisheyeRequested) return;
	s_fisheyeRequested = s_fisheyeEnabled;
	AfxFisheye::SetRequest(s_fisheyeEnabled, 1.0f);
}

// Pump heartbeats. RunFrame() rides the render thread (Before_Present) and is what actually
// STARTS every demo scan; RunMainThreadFrame() rides the main thread. When the demo list stays
// empty and "scan requested" never produces results, the first question is always "is the
// render-thread pump even running?" -- these answer it without a debugger.
std::atomic<uint64_t> g_renderPumpCalls{ 0 };
std::atomic<uint64_t> g_mainPumpCalls{ 0 };
std::atomic<uint64_t> g_scansStarted{ 0 };
std::atomic<bool> g_pickerOpen{ false };
std::atomic<bool> g_shuttingDown{ false }; // set by Shutdown(); picker thread checks before committing

std::mutex g_pendingMutex;
std::vector<std::wstring> g_pendingFolders;
std::vector<std::wstring> g_pendingDemoFiles;

// A Watch() that is waiting on a background demo repair. Polled from the main pump; cleared
// once the repair resolves and playback is issued.
std::mutex g_pendingWatchMutex;
std::wstring g_pendingWatchPath;

// A demo switch waiting for the CURRENT demo to finish tearing down.
//
// WHY THIS EXISTS. CS2's native demo bar (huddemocontroller.js) keeps module-level latches that
// an in-place demo switch does not survive: `nSpectatingPlayerId` is never reset on a demo
// change, so CreateTimelineEvents() -- the kill/death skulls -- is simply never called again,
// and `bRoundsMarked` latches true on the first frame the marker panel has width, so if the new
// demo's RoundIntervals are not populated yet it builds nothing and never retries ("Round: 0",
// no round shading). Valve never hits this because you normally return to the main menu between
// demos, which recreates the panel and resets the latches.
//
// Measured 2026-09-04 with matched screenshots (same demo, same tick, first person throughout):
// straight playdemo -> no skulls, no shading, "Round: 0"; disconnect first -> full skull row,
// shading, "Round: 6". Disconnecting drives GetDemoControllerState() to null, which nulls their
// lastState and forces the clean rebuild.
//
// It has to be DEFERRED. Sending `disconnect` and `playdemo` back-to-back loses the playdemo to
// the host-state transition and lands on the main menu -- verified, it is not a theoretical race.
std::mutex g_pendingSwitchMutex;
std::wstring g_pendingSwitchOriginal;  // what the marker sidecar keys off
std::wstring g_pendingSwitchPlayable;  // what the engine loads
int g_pendingSwitchFrames = 0;         // frames waited, for the give-up fallback

std::mutex g_demoPathMutex;
std::wstring g_currentDemoPath; // last demo launched via Watch (for the marker sidecar)

// SWITCH ONTO A KEYFRAMED COPY THAT FINISHED WHILE ITS DEMO IS OPEN. Without it the copy waited
// for the next open, and the session that built it scrubbed backward from Valve's snapshot a minute
// back (~0.75 s a step, measured 2026-10-16). The switch is a level load, so it waits for a quiet
// moment -- paused, no render job, no scrub, no path playback -- then reopens on the copy and lands
// back on the same tick, paused. Main thread only.
struct CopySwitch {
	enum class State { Idle, Waiting, Loading, Seeking };
	State state = State::Idle;
	std::wstring original, copy;
	int tick = -1;
	int frames = 0;       // since the state began (give-up fallbacks)
	int loadedFrames = 0; // since the copy was seen playing (settle before the seek)
};
CopySwitch g_copySwitch;

void DoInit() {
	g_library.Init();
	CosmeticsRef().Init(); // load persisted offline-demo cosmetic profiles (%APPDATA%\HLAE\)
	ParticleFxRef().LoadSettings(); // effect toggles (%APPDATA%\HLAE\filmmaker_fx.json); hook arms lazily
	FilmmakerSettings_Load(); // reusable Config + Camera Editor preferences
	DemoOrganizer_Init(); // demo-list sort mode, user demo folders, last-played history
	WorkshopOrganizer_Init(); // workshop map sort mode, category filters, last-played history
	g_rescanRequested.store(true); // initial scan happens on the next RunFrame
}

// Builds a "playdemo" command for a demo path. Prefers a path relative to the
// game's csgo/ content root (which the engine resolves reliably); otherwise
// falls back to the absolute path. Strips the .dem extension, forward slashes.
std::string BuildPlaydemoCommand(const std::wstring& path) {
	std::wstring install = g_library.InstallRoot();
	if (!install.empty() && install.back() != L'\\' && install.back() != L'/')
		install += L'\\';
	std::wstring csgo = install + L"csgo\\";

	auto startsWithCI = [](const std::wstring& s, const std::wstring& prefix) {
		if (s.size() < prefix.size())
			return false;
		for (size_t i = 0; i < prefix.size(); ++i)
			if (towlower(s[i]) != towlower(prefix[i]))
				return false;
		return true;
	};

	std::wstring rel;
	if (startsWithCI(path, csgo))
		rel = path.substr(csgo.size());
	else
		rel = path;

	std::string u8 = WideToUtf8(rel);
	std::replace(u8.begin(), u8.end(), '\\', '/');
	if (u8.size() >= 4) {
		std::string tail = u8.substr(u8.size() - 4);
		std::transform(tail.begin(), tail.end(), tail.begin(), [](char c) { return (char)tolower(c); });
		if (tail == ".dem")
			u8.resize(u8.size() - 4);
	}

	return std::string("playdemo \"") + u8 + "\"";
}

} // namespace

void EnsureInitialized() {
	std::call_once(g_initOnce, DoInit);
}

DemoLibrary& Library() {
	EnsureInitialized();
	return g_library;
}

FilmmakerMenu& Menu() {
	EnsureInitialized();
	return g_menu;
}

void RequestRescan() {
	EnsureInitialized();
	g_rescanRequested.store(true);
}

void RequestAddFolder() {
	EnsureInitialized();
	bool expected = false;
	if (!g_pickerOpen.compare_exchange_strong(expected, true))
		return; // a picker is already open

	// The folder dialog is modal and can't be cleanly joined on shutdown, so this stays
	// detached; instead it checks g_shuttingDown before touching the shared queue so a quit
	// while the picker is open can't push into (soon-to-be) torn-down state.
	std::thread([] {
		std::wstring picked;
		const bool ok = PickFolderBlocking(picked, L"Select a demo folder");
		if (ok && !picked.empty() && !g_shuttingDown.load()) {
			std::lock_guard<std::mutex> lock(g_pendingMutex);
			g_pendingFolders.push_back(picked);
		}
		g_pickerOpen.store(false);
	}).detach();
}

void RequestAddDemo() {
	EnsureInitialized();
	bool expected = false;
	if (!g_pickerOpen.compare_exchange_strong(expected, true))
		return; // a picker is already open

	// Detached for the same reason as RequestAddFolder: the file dialog is modal and cannot be
	// cleanly joined on shutdown, so it checks g_shuttingDown before touching shared state.
	std::thread([] {
		std::vector<std::wstring> picked;
		const bool ok = PickDemoFilesBlocking(picked, L"Select demo files");
		if (ok && !picked.empty() && !g_shuttingDown.load()) {
			std::lock_guard<std::mutex> lock(g_pendingMutex);
			g_pendingDemoFiles.insert(g_pendingDemoFiles.end(), picked.begin(), picked.end());
		}
		g_pickerOpen.store(false);
	}).detach();
}

namespace {

// Applies everything the (detached) picker threads have queued. Called from BOTH pumps: the
// render pump is not guaranteed to be up, and applying picks on only one of them is what made
// "Add folder" silently do nothing in that case.
//
// Always rescans, even when the folder/file was already saved -- picking is an explicit "look
// here again" from the user, and skipping the rescan made the button look broken on every use
// after the first.
void DrainPendingPicks() {
	std::vector<std::wstring> folders, files;
	{
		std::lock_guard<std::mutex> lock(g_pendingMutex);
		folders.swap(g_pendingFolders);
		files.swap(g_pendingDemoFiles);
	}
	for (const auto& folder : folders) {
		if (g_library.AddFolder(folder))
			advancedfx::Message("mirv_filmmaker: added demo folder %s\n", WideToUtf8(folder).c_str());
		else
			advancedfx::Message("mirv_filmmaker: demo folder already added, rescanning %s\n", WideToUtf8(folder).c_str());
		g_rescanRequested.store(true);
	}
	for (const auto& file : files) {
		if (g_library.AddDemoFile(file))
			advancedfx::Message("mirv_filmmaker: added demo %s\n", WideToUtf8(file).c_str());
		else
			advancedfx::Message("mirv_filmmaker: demo already added, rescanning %s\n", WideToUtf8(file).c_str());
		g_rescanRequested.store(true);
	}
}

} // namespace

void PlayResolvedDemo(const std::wstring& original, const std::wstring& playable);

void PrepareDemo(std::size_t index) {
	EnsureInitialized();
	std::wstring path;
	if (!g_library.PathByIndex(index, path)) {
		advancedfx::Warning("mirv_filmmaker: no demo at index %zu\n", index);
		return;
	}
	// ResolvePlayable is deliberately non-blocking. Preparation only warms the caches; it must
	// never begin playback. Once the repair verdict is in, the keyframed copy starts building --
	// dense around the camera markers saved with the demo. Build is a no-op when a valid copy
	// already covers them.
	const std::wstring playable = DemoRepair_ResolvePlayable(path);
	if (!playable.empty())
		DemoKeyframes_Build(path, playable, CameraPath_SavedMarkerTicks(path));
}

// RENAME THE DEMO FILE ITSELF.
//
// The name the browser shows in the detail title IS the file name, so editing it there has to
// move the file -- and only move it. Nothing here copies, re-encodes or rewrites a .dem; a
// failed rename leaves every byte where it was.
//
// WHAT TRAVELS WITH IT. The original .dem stays in its demo folder. Every sidecar lives in
// the source_mvm project directory, which DemoProjectStore_NoteRenamed rekeys. A leftover
// Valve `<name>.dem.info` that CS2 dropped beside the file since the last scan still moves
// with the rename so the scoreboard is not orphaned before the next claim.
namespace {

// Everything Windows will not accept in a file name, plus the separators -- a rename must never
// be able to walk out of the demo's own folder.
bool DemoNameCharAllowed(wchar_t c) {
	if (c < 32) return false;
	switch (c) {
	case L'<': case L'>': case L':': case L'"': case L'/': case L'\\':
	case L'|': case L'?': case L'*':
		return false;
	default:
		return true;
	}
}

// Returns "" when nothing usable is left. Always ends in .dem: the extension is what makes the
// file a demo to every other part of this tool, and it is not the user's to drop.
std::wstring SanitizeDemoFileName(const std::wstring& raw) {
	std::wstring name;
	name.reserve(raw.size());
	for (wchar_t c : raw)
		if (DemoNameCharAllowed(c))
			name.push_back(c);
	// Leading/trailing spaces and trailing dots are legal to type and illegal (or invisible and
	// confusing) on disk.
	while (!name.empty() && (name.front() == L' ')) name.erase(name.begin());
	while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
	if (name.empty()) return L"";
	// Accept a typed ".dem" instead of appending a second one.
	const bool hasExt = name.size() > 4
		&& 0 == _wcsicmp(name.c_str() + name.size() - 4, L".dem");
	if (!hasExt) name += L".dem";
	return name;
}

} // namespace

void RenameDemoFile(std::size_t index, const std::string& newNameUtf8) {
	EnsureInitialized();
	std::wstring oldPath;
	if (!g_library.PathByIndex(index, oldPath)) {
		advancedfx::Warning("mirv_filmmaker: no demo at index %zu\n", index);
		return;
	}
	const std::wstring wanted = SanitizeDemoFileName(Utf8ToWide(newNameUtf8));
	if (wanted.empty()) {
		advancedfx::Warning("mirv_filmmaker: that demo name has no usable characters.\n");
		return;
	}

	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path oldFs(oldPath);
	const fs::path dir = oldFs.parent_path();
	const std::wstring oldName = oldFs.filename().wstring();
	if (0 == _wcsicmp(oldName.c_str(), wanted.c_str()))
		return; // same name (possibly different case only) -- nothing to do, and no error either
	const fs::path newFs = dir / wanted;
	if (fs::exists(newFs, ec)) {
		advancedfx::Warning("mirv_filmmaker: %s already exists in that folder.\n",
			WideToUtf8(wanted).c_str());
		return;
	}

	// The .dem FIRST. If it cannot move -- the usual reason being that this is the demo currently
	// playing, so the engine holds the handle -- nothing else is touched and the set stays
	// consistent under the old name.
	fs::rename(oldFs, newFs, ec);
	if (ec) {
		advancedfx::Warning("mirv_filmmaker: could not rename the demo (%s). "
			"If it is the demo you are watching, stop playback first.\n", ec.message().c_str());
		return;
	}

	int carried = 0;
	const fs::path leftoverInfo = dir / (oldName + L".info");
	if (fs::is_regular_file(leftoverInfo, ec)) {
		std::error_code moveEc;
		fs::rename(leftoverInfo, dir / (wanted + L".info"), moveEc);
		if (!moveEc) ++carried;
	}

	// Carry the user-side state across to the new path, then rescan so the list shows the new
	// name. Both stores key on the normalised full path (see DemoOrganizerStore.h).
	DemoOrganizer_NoteRenamed(oldPath, newFs.wstring());
	g_library.NoteRenamed(oldPath, newFs.wstring());
	DemoProjectStore_NoteRenamed(ProjectGame::Cs2, oldPath, newFs.wstring());
	g_rescanRequested.store(true);
	advancedfx::Message("mirv_filmmaker: renamed to %s (%d companion file%s moved).\n",
		WideToUtf8(wanted).c_str(), carried, carried == 1 ? "" : "s");
}

// Showing a file in Explorer lives in Platform/ShellOpen (shared with the bug report).

void OpenDemoProjectFolder(std::size_t index) {
	EnsureInitialized();
	std::wstring demoPath;
	if (!g_library.PathByIndex(index, demoPath)) {
		advancedfx::Warning("mirv_filmmaker: no demo at index %zu\n", index);
		return;
	}
	std::wstring showPath = DemoProjectStore_EnsureProjectFile(ProjectGame::Cs2, demoPath);
	if (showPath.empty()) showPath = demoPath;
	const size_t separator = showPath.find_last_of(L"\\/");
	if (separator == std::wstring::npos) {
		advancedfx::Warning("mirv_filmmaker: demo has no containing folder\n");
		return;
	}
	// We own the foreground when the user clicks this, so we are allowed to hand that right to
	// whatever is about to show the window. Without it Windows refuses the activation outright and
	// the window opens behind the game.
	AllowSetForegroundWindow(ASFW_ANY);
	ShowInExplorerAsync(showPath);
	advancedfx::Message("mirv_filmmaker: showing %s in Explorer.\n", WideToUtf8(showPath).c_str());
}

void Watch(std::size_t index) {
	EnsureInitialized();
	std::wstring path;
	if (!g_library.PathByIndex(index, path)) {
		advancedfx::Warning("mirv_filmmaker: no demo at index %zu\n", index);
		return;
	}
	if (!g_pEngineToClient) {
		advancedfx::Warning("mirv_filmmaker: engine not ready, cannot play demo\n");
		return;
	}
	// A keyframed copy (built from the repaired-or-original demo, see DemoKeyframes.h) is already
	// playable: nothing to check or repair.
	const std::wstring keyframed = DemoKeyframes_ReadyPath(path);
	if (!keyframed.empty()) {
		PlayResolvedDemo(path, keyframed);
		return;
	}
	// CS2 14172 broke playback of demos containing legacy entity message 138; resolve to a
	// repaired copy first so this is automatic instead of a manual step. Returns the original
	// straight back for a clean demo, a cached copy if one already exists, or empty while the
	// background check/repair runs -- in which case the pending play below picks it up.
	const std::wstring playable = DemoRepair_ResolvePlayable(path);
	if (playable.empty()) {
		std::lock_guard<std::mutex> lock(g_pendingWatchMutex);
		g_pendingWatchPath = path;
		advancedfx::Message("mirv_filmmaker: %s -- playback starts when it finishes.\n",
			DemoRepair_StatusText().c_str());
		return;
	}
	PlayResolvedDemo(path, playable);
}

// Issues the actual playdemo. `original` is what the camera-marker sidecar keys off (so markers
// follow the demo, not the repaired copy); `playable` is what the engine loads.
// True while the engine has a demo loaded (playing or paused).
bool EngineHasDemoLoaded() {
	if (!g_pEngineToClient) return false;
	if (auto pDemo = g_pEngineToClient->GetDemoFile())
		return pDemo->IsPlayingDemo() || pDemo->IsDemoPaused();
	return false;
}

void IssuePlaydemo(const std::wstring& original, const std::wstring& playable) {
	{
		std::lock_guard<std::mutex> lock(g_demoPathMutex);
		g_currentDemoPath = original;
	}
	DemoOrganizer_NotePlayed(original);
	std::string cmd = BuildPlaydemoCommand(playable);
	advancedfx::Message("mirv_filmmaker: %s\n", cmd.c_str());
	g_pEngineToClient->ExecuteClientCmd(0, cmd.c_str(), true);
}

void PlayResolvedDemo(const std::wstring& original, const std::wstring& playable) {
	if (!g_pEngineToClient) return;
	// The engine reports the COPY's path while it plays; the folder's project.mvm is what maps
	// it back to `original` (ResolveDemoProjectPath), so it has to exist before playback starts.
	if (!WideEqualsI(playable, original))
		DemoProjectStore_EnsureProjectFile(ProjectGame::Cs2, original);
	// One copy: opening this demo removes every other demo's (DemoKeyframes.h).
	DemoKeyframes_KeepOnly(original, ResolvePlayingDemoPath());
	// Not the keyframed copy yet, or one that is not dense where the camera markers now are: play
	// what we have now and build in the background, so the NEXT play of this demo scrubs backward
	// in one frame. Playback is never held for it. The source is never the keyframed copy itself.
	const std::vector<int> markerTicks = CameraPath_SavedMarkerTicks(original);
	if (playable != DemoKeyframes_ReadyPath(original))
		DemoKeyframes_Build(original, playable, markerTicks);
	else
		DemoKeyframes_Build(original, DemoRepair_ResolvePlayable(original), markerTicks);
	// Switching demo-to-demo: tear the current one down FIRST so the native demo bar rebuilds its
	// round shading and kill markers (see g_pendingSwitch above). The playdemo is issued from the
	// main pump once the demo is actually gone; issuing both together loses it.
	if (EngineHasDemoLoaded()) {
		{
			std::lock_guard<std::mutex> lock(g_pendingSwitchMutex);
			g_pendingSwitchOriginal = original;
			g_pendingSwitchPlayable = playable;
			g_pendingSwitchFrames = 0;
		}
		g_pEngineToClient->ExecuteClientCmd(0, "disconnect", true);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(g_demoPathMutex);
		g_currentDemoPath = original;
	}
	// "Last played" for the demo browser's Recently played sort. Recorded against the ORIGINAL
	// demo (the one the list shows), never the repaired copy it may actually load.
	DemoOrganizer_NotePlayed(original);
	std::string cmd = BuildPlaydemoCommand(playable);
	advancedfx::Message("mirv_filmmaker: %s\n", cmd.c_str());
	g_pEngineToClient->ExecuteClientCmd(0, cmd.c_str(), true);
}

std::wstring CurrentDemoPath() {
	std::lock_guard<std::mutex> lock(g_demoPathMutex);
	return g_currentDemoPath;
}

std::wstring PlayingDemoPath() {
	// Authoritative source: the demo the engine is actually playing (works for our tab,
	// native Your Matches, and console playdemo alike).
	std::wstring eng = ResolvePlayingDemoPath();
	if (!eng.empty())
		return CanonicalDemoPath(ResolveDemoProjectPath(eng));

	// If no demo is currently playing, do NOT fall back to the last Watch() path. Returning a
	// stale path here keeps CameraPath bound to the previous demo while CS2 is between demos,
	// which lets the old markers bleed into the next session.
	bool playing = false;
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			playing = pDemo->IsPlayingDemo();
	}
	if (!playing)
		return L"";

	// Fallback: a demo is playing but the engine path couldn't be recovered. Use the path our
	// own Watch() recorded (canonicalized so it matches the engine form when it does resolve),
	// so a demo opened from our Downloaded tab is never worse off than before. Cached on the
	// input string to avoid a CreateFile() every frame in this degraded path.
	std::wstring our = CurrentDemoPath();
	if (our.empty())
		return L"";
	static std::mutex s_m;
	std::lock_guard<std::mutex> lk(s_m);
	static std::wstring s_lastIn, s_lastOut;
	if (our != s_lastIn) { s_lastIn = our; s_lastOut = CanonicalDemoPath(our); }
	return s_lastOut;
}

// g_copySwitch, one step per main-thread frame (see the struct).
void PumpCopySwitch() {
	CopySwitch& s = g_copySwitch;
	if (s.state == CopySwitch::State::Idle || !g_pEngineToClient)
		return;
	auto* demoFile = g_pEngineToClient->GetDemoFile();
	const bool playing = demoFile && demoFile->IsPlayingDemo() && !demoFile->IsDemoPaused();
	const bool paused = demoFile && demoFile->IsDemoPaused();
	int tick = -1;
	const bool haveTick = g_MirvTime.GetCurrentDemoTick(tick);
	auto log = [&](const char* what) {
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("keyframes.switch", "%s tick=%d target=%d frames=%d copy=%s",
				what, tick, s.tick, s.frames, WideToUtf8(s.copy).c_str());
	};
	// The engine reports its own spelling of a path (forward slashes, its own case).
	auto sameFile = [](std::wstring a, std::wstring b) {
		for (auto& c : a) if (c == L'/') c = L'\\';
		for (auto& c : b) if (c == L'/') c = L'\\';
		return !a.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
	};

	switch (s.state) {
	case CopySwitch::State::Waiting: {
		// Gone, or a different demo now: nothing to switch.
		if (!sameFile(PlayingDemoPath(), s.original)) { log("dropped: demo changed"); s = CopySwitch(); return; }
		const bool quiet = paused && haveTick && !Render_IsJobActive() && !DemoScrub_Active()
			&& !TimelinePlayhead_Scrubbing() && !CameraPathRef().IsPlaying() && !CameraPathRef().PlaybackPending();
		if (!quiet) return;
		s.tick = tick;
		s.frames = 0;
		s.state = CopySwitch::State::Loading;
		log("switching");
		advancedfx::Message("mirv_filmmaker: switching to the finer scrub keyframes at tick %d...\n", tick);
		PlayResolvedDemo(s.original, s.copy);
		return;
	}
	case CopySwitch::State::Loading:
		// The copy is loaded once the engine plays it and the clock runs. A few frames more let the
		// HUD and our panels settle before the seek.
		if (++s.frames > 64 * 60) { log("gave up: the copy never started"); s = CopySwitch(); return; }
		if (!sameFile(ResolvePlayingDemoPath(), s.copy) || !haveTick || tick < 1 || !(playing || paused)) { s.loadedFrames = 0; return; }
		if (++s.loadedFrames < 30) return;
		// A forward demo_gototick on a PAUSED demo never runs (filmmaker_demo_scrubbing.md), so
		// seek while playing and pause on arrival.
		if (paused) g_pEngineToClient->ExecuteClientCmd(0, "demo_resume", true);
		{
			char cmd[64];
			snprintf(cmd, sizeof(cmd), "demo_gototick %d", s.tick);
			g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
		}
		s.frames = 0;
		s.state = CopySwitch::State::Seeking;
		log("seeking");
		return;
	case CopySwitch::State::Seeking:
		if (++s.frames > 64 * 10) { log("gave up: the seek never landed"); s = CopySwitch(); return; }
		if (!haveTick || std::abs(tick - s.tick) > 1) return;
		if (playing) g_pEngineToClient->ExecuteClientCmd(0, "demo_pause", true); // a toggle: only while playing
		log("done");
		advancedfx::Message("mirv_filmmaker: now scrubbing with the finer keyframes (tick %d).\n", tick);
		s = CopySwitch();
		return;
	default:
		return;
	}
}

void RunFrame() {
	g_renderPumpCalls.fetch_add(1, std::memory_order_relaxed);
	EnsureInitialized();

	// ViewFxVm write-site 3 (render-thread pump; viewmodel-rotation experiment).
	ViewFxVm_RenderPump();

	// Apply any folders / demo files chosen by the picker threads.
	DrainPendingPicks();

	// Start a (re)scan on the render thread so scan lifetime is single-threaded.
	bool wantScan = true;
	if (g_rescanRequested.compare_exchange_strong(wantScan, false)) {
		if (!g_library.IsScanning()) {
			g_scansStarted.fetch_add(1, std::memory_order_relaxed);
			g_library.StartScan();
		}
		else
			g_rescanRequested.store(true); // retry next frame
	}
}

void SetFpHideWhenDetached(bool on) { g_fpHideWhenDetached = on; }
bool FpHideWhenDetached() { return g_fpHideWhenDetached; }

void PumpStatus(uint64_t& outRenderCalls, uint64_t& outMainCalls, uint64_t& outScansStarted) {
	outRenderCalls = g_renderPumpCalls.load(std::memory_order_relaxed);
	outMainCalls = g_mainPumpCalls.load(std::memory_order_relaxed);
	outScansStarted = g_scansStarted.load(std::memory_order_relaxed);
}

// Last main-thread hook point before the draw. See Filmmaker.h for why the mesh-group selection has
// to be re-asserted here rather than in RunMainThreadFrame.
void RunMainThreadLateFrame() {
	CosmeticsRef().ReassertMeshMasksLate();
	// Paused-demo client animation clock. Same reason the mesh-mask reassert lives here: a write in
	// RunMainThreadFrame is re-applied by the client's own FRAME_RENDER_PASS handler afterwards
	// (measured). This is the last write before the draw. See Panorama/PreviewAnimClock.h.
	PreviewAnimClock_RunLateFrame();
	// The client updates the weapon's interpolated pose during FRAME_RENDER_PASS.
	// Apply both its source pose and derived world transform after that update.
	RagdollWeapon_RunLateFrame();
	DeathAnimations_RunLateFrame();
	// Config picker: the death clip on the player-model preview. Local-pose write, so it has to be the
	// last write before the draw like the ones above. Own module: Panorama/DeathAnimPreview.h.
	DeathAnimPreview_RunLateFrame(ConfigHud_Enabled());
	// SCENE OBJECTS > CS2 ASSETS preview: turns the browsed model (same late local-pose write).
	PropPreview_RunLateFrame();
	// Reassert the final facial weights and eye pose after the engine frame handler.
	MethodActing_RunLateFrame(true);
	// Easter eggs: Big Head's head_0 local-pose scale (same last-write-before-draw rule).
	EasterEggs_RunLateFrame();
}

void RunMainThreadPostFrame() {
	// Publish the final hand target for independent read-only scene-draw diagnostics.
	RagdollWeapon_RunPostFrame();
}

void RunMainThreadFrame() {
	g_mainPumpCalls.fetch_add(1, std::memory_order_relaxed);
	EnsureInitialized();

	// RenderDoc F12 capture can drop our ConCommands from ICvar while the DLL stays mapped.
	WrpEnsureCommandsRegistered();

	// Alt-tab must not throttle the game (one-shot; inert once applied).
	UnfocusedRate_MainThreadTick();

	// Silence the per-frame RenderSystem/SceneSystem console spam (one-shot; inert once applied).
	EngineLogQuiet_RunMainThreadFrame();

	// Frame times, VRAM and GPU into the always-on recorder every few seconds (crash reports).
	FilmmakerHealth_Heartbeat();

	// Effects > Flash blindness off: zero every pawn's flash so flashbangs never blind anyone
	// (the demo re-networks it each detonation, so it is cleared every frame).
	if (!ParticleFxRef().FlashBlind()) CustomizeSuppressFlashTick();

	// Safety net: the demo scan is normally started by RunFrame() on the render thread, but
	// that pump depends on the Present/scenesystem hooks being installed. If those ever fail
	// (a CS2 update moving their signatures is the usual cause) the render pump goes silent and
	// the demo list stays permanently empty with no other symptom. Starting the scan from the
	// main-thread pump too costs nothing when the render pump is healthy (the flag is already
	// consumed by then) and keeps the Demos tab working when it is not.
	{
		bool wantScan = true;
		if (g_rescanRequested.compare_exchange_strong(wantScan, false)) {
			if (!g_library.IsScanning()) {
				g_scansStarted.fetch_add(1, std::memory_order_relaxed);
				g_library.StartScan();
			} else {
				g_rescanRequested.store(true);
			}
		}
	}

	// Same reasoning for picks made in the native dialogs: applying them only on the render
	// pump means "Add folder" / "Add demo" silently do nothing whenever that pump is down.
	DrainPendingPicks();
	// Demos opened from CS2's own Your Matches page or from a console `playdemo` never go
	// through our Watch(), so the Recently played sort also watches what the engine is actually
	// playing. Sampled once a second, not per frame: PlayingDemoPath() canonicalises a path and
	// a "last played" stamp has no use for sub-second precision.
	{
		static ULONGLONG s_lastPlayingPollMs = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - s_lastPlayingPollMs >= 1000) {
			s_lastPlayingPollMs = now;
			DemoOrganizer_PollPlaying(PlayingDemoPath());
			// Scrub keyframes follow the camera markers: a rebuild with denser windows once they
			// have settled outside the copy's (Docs/features/filmmaker_demo_scrubbing.md), and a
			// switch onto a copy that finished while its demo is open.
			{
				std::vector<int> markerTicks;
				for (const CamMarker& m : CameraPathRef().Markers()) markerTicks.push_back(m.tick);
				const std::wstring demo = PlayingDemoPath();
				if (DemoKeyframes_RunFrame(demo, ResolvePlayingDemoPath(), markerTicks, Render_IsJobActive())) {
					g_copySwitch = CopySwitch();
					g_copySwitch.original = demo;
					g_copySwitch.copy = DemoKeyframes_ReadyPath(demo);
					g_copySwitch.state = g_copySwitch.copy.empty() ? CopySwitch::State::Idle : CopySwitch::State::Waiting;
					if (g_copySwitch.state == CopySwitch::State::Waiting)
						CameraPathRef().ShowNotice("Finer scrub keyframes are ready -- switching to them when the demo is paused.");
				}
			}
			// Same idea for workshop maps: the Play page's "Recently played" sort needs to know
			// which map is loaded, and a map started from CS2's own UI never goes through us.
			// Rides the same one-second sample -- a "last played" stamp has no use for sub-second
			// precision, and the level name only changes on a map change anyway.
			if (g_pEngineToClient) {
				const char* lvl = g_pEngineToClient->GetLevelNameShort();
				WorkshopOrganizer_PollLevel(lvl ? lvl : "");
			}
		}
	}

	// Panorama work (RunScript + panel IO) must run on the main/UI thread.
	g_menu.RunFrame(g_library);

	// Play-page takeover (main menu only): strips the page to Practice + Workshop Maps and
	// drives the workshop map sidebar. Cheap no-op while a demo/map is loaded.
	PlayMenu_RunMainThreadFrame();
	// HC Tracker badge + Report Bugs on the main menu (and so on the in-game pause menu).
	Community_RunMainThreadFrame();

	// Extend CS2's own Video > Resolution dropdown with filmmaker custom sizes. This stays in
	// the native pause/settings flow and is independent from the Filmmaker Config sidebar.
	VideoSettingsResolution_RunMainThreadFrame();

	// Client animation clock for the paused-demo player preview. Runs FIRST so the injected
	// curtime/frametime are in place before anything else this frame reads them. No-op unless
	// enabled AND the demo is paused; never touches the demo tick. See Panorama/PreviewAnimClock.h.
	PreviewAnimClock_RunFrame();

	// End-of-demo hold: pause on the last tick instead of letting the engine disconnect to the
	// main menu; a play press while held restarts the demo from the beginning. Runs early so
	// the pause lands the same frame the tick crosses the hold point.
	DemoEndHold_RunFrame();

	// Playback loop: seeks back to Loop In when playback reaches Loop Out. Runs right after
	// the end-of-demo hold so the hold gets first refusal -- a loop parked at the very end of
	// a demo must not fight the pause that stops the engine disconnecting. Inert unless a loop
	// is armed AND its region is usable.
	PlaybackLoop_RunFrame();

	// Live scrubbing: the world follows a timeline drag (plays forward, seeks back). Inert unless a
	// scrub surface is being dragged.
	DemoScrub_RunFrame();

	// World Only: refresh the planted-bomb bit BEFORE the render driver steps, so a job's first
	// captured frame already has the right verdict for C4 draws. Inert (and does not touch the
	// entity list at all) unless World Only is armed.
	RenderWorldOnly_RunMainThreadFrame();

	// Render-job driver: walks an active render session through seek â†’ warm-up â†’ capture â†’
	// restore, one state step per frame. Cheap no-op while no job is running. Runs right after
	// DemoEndHold so an end-of-demo pause is already applied when the driver reads the tick.
	RenderSessionDriver_RunFrame();

	// Free window resizing: applies the resizable window style once SDL and the window exist (and
	// removes it again for fullscreen), then runs the "did the engine follow the new size?" check
	// after a resize. Cheap: two SDL flag reads per frame while idle. See Window/WindowResize.h.
	WindowResize_PumpMainThread();

	// Engine render-size hook: installs itself as soon as client.dll's render-device interface
	// exists, so it is already in place when a demo/map build its render targets. The install is
	// a one-shot; after it succeeds this is a single atomic read. Inert until a factor is set.
	RenderEngineSize_EnsureInstalled();

	// Phase 0 weapon-bob / reload-impact research probe: samples the first-person weapon's
	// muzzle attachment and the spectated pawn's movement state into the mvm_debug log.
	// Read-only, and returns immediately unless BOTH `bobprobe on` and a log are active.
	WeaponBobProbe_RunMainThreadFrame();

	// Wheel notches that came in as raw input (CS2's look-around drag holds the mouse, so no wheel
	// message arrives): hand them to the director before it applies this frame's actions.
	WindowInputHooks_MainThreadFrame(MovieModeRef().OwnsWheel() && !UiInputGate_Active());

	// Movie director: apply queued input actions (engine commands / free-cam
	// toggle) on this thread, then refresh the in-game help/status HUD panel.
	MovieModeRef().FlushActions();

	// Camera detached from the spectated eye (FREE CAM ONLY): hide the first-person viewmodel
	// and suppress its _fp weapon FX. Both are anchored to the CAMERA, so once the view leaves
	// the eye they render as giant floating arms and mid-air muzzle flashes/smoke. Restored the
	// frame the camera re-attaches.
	//
	// Third-person orbit was REMOVED from this on purpose (undoing part of d5eebb2d): treating
	// it as detached forced spec_mode 3 + r_drawviewmodel false, so pushing the orbit camera
	// INSIDE the player showed nothing at all. Left in-eye, the engine renders the FP legs when
	// the camera enters the pawn and the viewmodel arms/gun stay drawn -- the "camera latched
	// inside the player" shot. The cost, accepted deliberately: at orbit DISTANCE the FP arms
	// float at the camera again and the spectated player's own muzzle FX anchor to the camera
	// (the exact artifacts this block was added to fix).
	//
	// FREE CAM was removed from it too, for the same reason: flying the free cam INTO a player
	// is the standard way to get the "latched inside them" shot, and forcing r_drawviewmodel
	// false + spec_mode 3 the moment free cam engages means there are never any first-person
	// hands to fly into. Hiding is now opt-in via `mirv_filmmaker fphide on`, default off, so
	// the arms/gun behave the way they do without the mod. Turn it on if floating arms at
	// distance bother you more than losing the inside-the-player view.
	{
		static bool s_fpViewHidden = false;
		static ULONGLONG s_lastSpecAssertMs = 0;
		const bool camDetached = g_fpHideWhenDetached && CameraBridge_GetFreeCamEnabled();
		if (camDetached != s_fpViewHidden) {
			s_fpViewHidden = camDetached;
			s_lastSpecAssertMs = 0;
			ParticleFx_SetFirstPersonFxSuppressed(camDetached);
			if (g_pEngineToClient) {
				g_pEngineToClient->ExecuteClientCmd(0,
					camDetached ? "r_drawviewmodel false" : "r_drawviewmodel true", true);
				// CHASE while detached: in-eye spectating spawns ONLY the _fp viewmodel
				// weapon FX for the spectated player (suppressed above -> no flashes at
				// all on their gun). Chase makes the engine spawn the THIRD-PERSON
				// world-model FX -- which the ParticleFx swap tables (Modern/Insurgency)
				// then apply to, same as for every other player. The rendered view stays
				// ours either way (the view-setup override outranks the chase camera).
				g_pEngineToClient->ExecuteClientCmd(0,
					camDetached ? "spec_mode 3" : "spec_mode 2", true);
			}
		}
		// Re-assert chase while detached: spec_mode does NOT apply while the demo is
		// paused (verified live), and the engine resets the observer mode on round
		// changes / player switches -- a one-shot exec at the transition silently
		// reverts to in-eye and the spectated player's weapon FX vanish again.
		if (camDetached && g_pEngineToClient) {
			const ULONGLONG now = GetTickCount64();
			if (now - s_lastSpecAssertMs >= 2000) {
				s_lastSpecAssertMs = now;
				g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 3", true);
			}
		}
	}

	// World + Players / No Gun is a render-subject policy, not a camera-detached policy. Apply it
	// after the detached-camera helper so that helper cannot re-enable the viewmodel while a job or
	// live Render workspace has explicitly selected the no-gun subject set.
	RenderWorldPlayersNoGun_RunMainThreadFrame();
	// Players Only / Green prepares its already-precached chroma material on the main thread and
	// reconciles live-preview state. The render job itself arms the policy through reliable pass
	// commands, so this never paints the beauty pass while a capture is running.
	RenderPlayersOnlyGreen_RunMainThreadFrame();
	{ PanoramaPerf_Scope _p("moviehud"); MovieHudRef().RunFrame(); }

	// Camera-marker / dolly path: hover picking, playback driver, auto-save, then
	// the BO2-style marker-edit menu. Both run on this (main/UI) thread.
	CameraPathRef().RunFrame();
	// Per-keyframe GTA V camera shake: follows the active demo's sidecar and keyframe retimes.
	// Must run AFTER CameraPathRef().RunFrame() -- it syncs against the keyframe list that call
	// has just loaded/rebuilt, so running first would sync against the previous demo's keys.
	CamPathShake_RunFrame();
	// Follow camera treats EITHER workspace as "the editor is up": opening Config closes the
	// Camera Editor (they share the right column), but that swap must not kill a running
	// Follow Live preview -- Config's scaled workspace shows the same follow-driven camera.
	FollowCameraRef().RunFrame(CameraEditorHudRef().Enabled() || ConfigHud_Enabled());
	// Fisheye lens D3D pass â€” publish the current toggle state every main-thread frame.
	PublishFisheye();
	// ViewFxVm write-site 2 (main-thread pump; viewmodel-rotation experiment).
	ViewFxVm_MainPump();
	// Camera-shake layers: housekeeping only (drops integrated springs when the demo closes).
	// The actual per-frame solve rides the view-setup trampoline in main.cpp.
	CameraShake_RunFrame();
	// Smooth View: advance breathing phase, FOV recovery. Camera/weapon apply runs in main.cpp.
	SmoothViewRef().Pump(0.016, 0.0);

	// A Watch() that was waiting on the background demo repair: start playback the moment the
	// repaired copy (or the "no repair needed" verdict) lands.
	{
		std::wstring pending;
		{
			std::lock_guard<std::mutex> lock(g_pendingWatchMutex);
			pending = g_pendingWatchPath;
		}
		if (!pending.empty()) {
			const std::wstring playable = DemoRepair_ResolvePlayable(pending);
			if (!playable.empty()) {
				{
					std::lock_guard<std::mutex> lock(g_pendingWatchMutex);
					g_pendingWatchPath.clear();
				}
				PlayResolvedDemo(pending, playable);
			}
		}
	}
	// Deferred demo switch: the previous demo has been told to disconnect, so issue the queued
	// playdemo as soon as the engine reports no demo loaded. The frame counter is a give-up
	// fallback -- if the teardown never reports done, play it anyway rather than swallow the
	// user's click (worst case is the old behaviour, a demo with no round markers).
	{
		std::wstring original, playable;
		bool fire = false;
		{
			std::lock_guard<std::mutex> lock(g_pendingSwitchMutex);
			if (!g_pendingSwitchPlayable.empty()) {
				++g_pendingSwitchFrames;
				// ~4 s at 64 fps. A cold disconnect takes well under a second in practice.
				if (!EngineHasDemoLoaded() || g_pendingSwitchFrames > 256) {
					original = g_pendingSwitchOriginal;
					playable = g_pendingSwitchPlayable;
					g_pendingSwitchOriginal.clear();
					g_pendingSwitchPlayable.clear();
					g_pendingSwitchFrames = 0;
					fire = true;
				}
			}
		}
		if (fire) IssuePlaydemo(original, playable);
	}
	PumpCopySwitch();
	{ PanoramaPerf_Scope _p("markerhud"); MarkerHudRef().RunFrame(); }

	// Offline demo skin-changer: re-assert any per-player weapon-skin overrides on this frame's
	// entities. Cheap no-op when nothing is overridden. Entity-touching, so it rides the main
	// thread alongside the camera/follow passes above.
	Cosmetics_RunFrame();
	// Cache each living player's active weapon before the engine processes this frame. The actual
	// corpse-hand transform is written by RunMainThreadLateFrame after dropped-weapon physics.
	RagdollWeapon_RunFrame();
	// Supply eyelid weights before animation/render preparation, then reassert the final eye bones late.
	MethodActing_RunLateFrame(false);
	// Ragdoll PHYSICS (gravity/friction/limit/collide). Convar-driven, own module.
	// No-op unless something is overridden.
	RagdollPhysics_RunFrame();
	// Death Animations: a dying player plays its clip exactly (keyframed ragdoll bodies), then
	// the native ragdoll takes over. Own module; no-op while off and idle.
	DeathAnimations_RunFrame();
	// Nervous System: a shot body keeps reacting on the ground for a few seconds (reach for the wound,
	// breathe, writhe, twitch). After Death Animations, which it waits for. Own module; no-op while off.
	NervousSystem_RunFrame();
	// World/Effects: lighting, atmosphere, weather. Entity-touching like the passes above, so it
	// rides this thread. Fans out to its own modules; every one is a no-op while disabled.
	World_RunFrame();
	// Player-preview scene probe (read-only instrumentation, own module). No-op unless armed with
	// `mvm_previewprobe watch`; exists to settle which clock the preview animgraph follows.
	PreviewSceneProbe_RunFrame();
	// Customize player preview: run the client's own animgraph tick for the preview pawns while the
	// demo is paused, so the character renders and keeps idling WITHOUT the old resume/re-pause of
	// playback. Armed purely by the modal being open. Own module -- see Panorama/PreviewAnimTick.h.
	PreviewAnimTick_RunFrame(CameraEditor_CustomizeModalOpen());
	// Read-only evidence for the remaining black composition-layer bug. This does not invoke any
	// render path or manipulate playback; it only observes engine-owned per-frame calls.
	// The CS2 ASSETS preview (Props/PropPreview.h) is the same kind of panel and needs the same arming.
	const bool assetPreview = ConfigHud_Enabled() && PropPreview_Active();
	PreviewRenderProbe_RunFrame(CameraEditor_CustomizeModalOpen(), DeathAnimPreview_Active() || assetPreview);
	// ...and the entity-side half of the same symptom: a preview pawn SPAWNED while the demo is
	// paused never interpolates, so the renderer skips its body and only the held weapon shows.
	// Sets the engine's own m_bRenderEvenWhenNotSuccessfullyInterpolated override on preview pawns.
	// Own module -- see Panorama/PreviewRenderFlag.h.
	PreviewRenderFlag_RunFrame(CameraEditor_CustomizeModalOpen() || DeathAnimPreview_Active() || assetPreview);

	// Camera timeline / curve editor (native-styled scrubber + IWXMVM-style lanes).
	{ PanoramaPerf_Scope _p("camtl"); CameraTimelineHudRef().RunFrame(); }

	// Native demo-bar inline speed buttons (replaces the timescale dropdown).
	{ PanoramaPerf_Scope _p("speedbar"); DemoBarButtonsRef().RunFrame(); }

	// Lightweight CONFIG panel (general UI/display settings; no camera tools). Runs before the
	// Camera Editor so the editor's host orchestration still wins the frame if both got toggled
	// in the same tick (they are mutually exclusive -- each closes the other on open).
	{ PanoramaPerf_Scope _p("aimxhair"); AimCrosshairHudRef().RunFrame(); }

	{ PanoramaPerf_Scope _p("confighud"); ConfigHudRef().RunFrame(); }

	// Drops the F8 blackout when demo playback ends -- the toggle that clears it is itself one
	// of the things it hides, so a stuck blackout would read as broken UI.
	HudBlackout_RunFrame();

	// Camera Editor Mode workspace shell. Runs LAST so its host orchestration (timeline
	// hosting + gameplay-HUD hide) is applied on top of every other panel this frame.
	{ PanoramaPerf_Scope _p("cameditor"); CameraEditorHudRef().RunFrame(); }

	// Easter eggs: rare hints, secret modes, Clippy, the Sony Vegas intervention. The overlay is
	// the topmost root (z900) in whichever context is up, in-game HUD or main menu.
	EasterEggs_RunMainThreadFrame();
	{ PanoramaPerf_Scope _p("eggs"); EasterEggOverlayHudRef().RunFrame(); }

	// Experimental After-Effects-style graph editor (opt-in overlay; default OFF). Runs after
	// everything else so, while enabled, its full-screen overlay sits on top; when disabled it
	// is a cheap no-op and the regular editor is untouched.
	// Every workspace has now submitted whether it wants the viewport scaled; decide and issue
	// the ONE render-layer request for this frame. Must stay after CameraEditorHud (the highest
	// precedence owner) and before nothing in particular -- see Panorama/WorkspacePreview.h.
	// Development hot-reload: re-runs a watched Panorama JS file when it changes on disk.
	// No-op unless "mirv_filmmaker livejs watch <path>" is active.
	UiLive_PumpMainThread();

	{ PanoramaPerf_Scope _p("previewcommit"); WorkspacePreviewRef().Commit(); }
	{
		// Where the game preview is this frame: clicks and the wheel split on it (MovieMode, input thread).
		float px0 = 0, py0 = 0, px1 = 0, py1 = 0;
		const bool preview = WorkspacePreviewRef().Active(px0, py0, px1, py1);
		MovieModeRef().SetUiClickRegions(preview, px0, py0, px1, py1);
	}

	{ PanoramaPerf_Scope _p("graphexp"); GraphEditorExperimentHudRef().RunFrame(); }

	// Particle-effect modifiers: lazy hook-arm and one-resource-per-frame resolver. Fresh
	// defaults are fully Off, so startup/demo entry performs no custom particle loading.
	ParticleFxRef().PumpMainThread();
	// Debug squares proving the barrel-smoke wisp's parent swap actually fires, distinctly for
	// Modern vs On/Insurgency (opt-in; 'mirv_filmmaker fx debughud on|off'). No-op when off.
	{ PanoramaPerf_Scope _p("fxdebughud"); FxDebugHudRef().RunFrame(); }
	// A demo change tears down the level-owned half of the EFFECTS settings (sky precache, cloud
	// and sun material parameters). Re-assert them onto the new level, or -- when the operator
	// turned the carry-over off -- reset every EFFECTS group to the map default. No-op unless a
	// level init armed it.
	EffectsPersistence_RunFrame();
	SceneKeys_RunFrame();
	Environment::RunMainThreadFrame();
	Props_RunMainThreadFrame();
	// Debounced persistence for reusable Config / Camera Editor preferences. Shot-specific
	// camera, path and Follow/Attach state stays in its owning subsystem.
	FilmmakerSettings_RunFrame();

	// Close the measurement window for this main-thread frame (no-op unless the probe is armed).
	PanoramaPerf_EndFrame();
}

void Shutdown() {
    g_shuttingDown.store(true);
    DemoRepair_Reset(); // join before DLL-owned repair code/state can be torn down
    DemoKeyframes_Reset();
    WindowResize_Shutdown(); // destroys the resize readout window before the DLL goes away
	SceneKeys_Flush();
	Environment::FlushCommittedEdits();
	FilmmakerSettings_Flush();
	FollowCameraRef().Shutdown(); // cancel + join the background loadout-event loader
	g_library.Shutdown();
}

// --- Movie director input taps (forward to the MovieMode singleton) ---
bool MovieInput_OnKey(int vkey, bool down) { return MovieModeRef().OnKey(vkey, down); }
bool MovieInput_OnMouseButton(int button, bool down, int x, int y) { return MovieModeRef().OnMouseButton(button, down, x, y); }
bool MovieInput_OnUiClick(int x, int y) { return MovieModeRef().OnUiClick(x, y); }
bool MovieInput_OnMouseWheel(int delta, bool shiftDown, bool ctrlDown, int x, int y) { return MovieModeRef().OnMouseWheel(delta, shiftDown, ctrlDown, x, y); }

// --- HUD panel show/hide ---
void MovieHud_Set(bool visible) { MovieHudRef().SetVisible(visible); }
void MovieHud_Toggle() { MovieHudRef().Toggle(); }
bool MovieHud_Visible() { return MovieHudRef().Visible(); }
void MovieHud_Eval(const std::string& js) { MovieHudRef().RequestEval(js); }

// --- Native demo-bar speed buttons on/off ---
void DemoSpeedBar_Set(bool enabled) { DemoBarButtonsRef().SetEnabled(enabled); }
void DemoSpeedBar_Toggle() { DemoBarButtonsRef().Toggle(); }
bool DemoSpeedBar_Enabled() { return DemoBarButtonsRef().Enabled(); }

// --- Camera-marker menu: cursor/suspend hook for main.cpp ---
bool MarkerMenu_WantsCursor() { return CameraPathRef().WantsCursor(); }

// --- Camera timeline panel: cursor/suspend hook for main.cpp ---
bool CameraTimeline_WantsCursor() {
	auto& tl = CameraTimelineHudRef();
	// The camera timeline / curve editor is always a UI surface, so it forces cursor
	// ownership while open. When closed, the regular native-bar/G cursor toggle owns it.
	return tl.Cursor();
}
bool CameraTimeline_Visible() { return CameraTimelineHudRef().Visible(); }

// --- Live-match FX test menu: release the GAME mouse so the cursor drives the panel ---
// main.cpp detours CCSGOInput::MouseInputEnabled and returns false while this is true, so
// the engine stops consuming the mouse for player aim and shows the cursor on a LIVE map
// (GetSuspendMirvInput only suspends HLAE's own free-cam look, which does nothing about the
// live player's aim capture). Gated on:
//   - MvmTest_CanUseMenu(): an offline live match with a map loaded and NOT demo playback --
//     the FX menu is a private-match-only harness and must never touch demo playback (the
//     camera timeline reuses CameraTimelineHudRef().Cursor() during demos, so this explicit
//     demo exclusion keeps the mouse hook from ever firing there); and
//   - the live Insert panel actually open in UI-cursor mode -- pressing G (MOUSE: GAME) drops
//     ITS OWN cursor flag so aiming/shooting returns.
//
// This used to read CameraTimelineHudRef().Cursor(), i.e. the DEMO camera timeline's flag. That
// was the bug behind "I have a cursor but clicking shoots": G routed to `camtl cursor`, which on
// a live map fails the timeline's third-person/free-cam gate and answers by forcing the cursor
// OFF -- so G could only ever take the mouse away, never hand it over. The live panel now owns
// its own flag and the demo timeline is left alone.
bool LiveFxMenu_WantsGameMouseReleased() {
	return MvmTest_CanUseMenu() && ConfigHud_IsLive() && ConfigHud_LiveCursor();
}

// --- camera-path view ownership + debug gate (read by main.cpp's view-setup hook) ---
bool CameraPathOwnsView() { return CameraPathRef().OwnsView() || FollowCameraRef().OwnsView(); }
bool CampathDebug() { return CameraPathRef().Debug(); }

// --- Camera Editor Mode (dedicated editor workspace) ---
// The two workspaces are mutually exclusive: they draw the SAME right-hand column area. Config
// already closed the editor on open (ConfigHud.cpp), but the reverse was missing, so pressing
// "CAM EDITOR" from Config left both enabled -- two opaque columns stacked in one place, which is
// what produced the blue bleed-through and the flicker while switching. Closing the other one
// here makes the swap atomic within a single frame, so no frame ever shows both.
//
// No recursion: only the ENABLE path closes the sibling, and the sibling's disable path does not
// call back.
void CameraEditor_Set(bool enabled) {
	if (enabled && ConfigHud_Enabled())
		ConfigHudRef().SetEnabled(false);
	CameraEditorHudRef().SetEnabled(enabled);
}
bool CameraEditor_Enabled() { return CameraEditorHudRef().Enabled(); }
void CameraEditor_FlashInspector() { CameraEditorHudRef().FlashInspector(); }
void CameraEditor_Toggle() { CameraEditor_Set(!CameraEditorHudRef().Enabled()); }
bool CameraEditor_Active() { return CameraEditorHudRef().Enabled(); }
void CameraEditor_SetCursorMode(bool uiCursor) {
	CameraTimelineHudRef().SetCursor(uiCursor);
	// ONE-DIRECTIONAL on purpose. "Does the user hold the UI cursor?" was being used to mean
	// "may the graph fly the camera?" as well, so EVERY path that handed the cursor back to
	// Panorama -- placing or cancelling a reposition in either camera system, closing Render,
	// restoring after a render job, opening the editor with the Graph panel -- silently RE-ARMED
	// a per-frame absolute pose push. That is the mechanism behind four of the six "camera paths
	// keep taking control of the viewport" triggers in the tester report.
	//
	// Taking free-look still disarms it, which is what the user means by grabbing the camera.
	// Arming stays explicit: `mirv_filmmaker graph drive on`, or a follow/attach preview.
	if (!uiCursor) {
		GraphEditorExperimentHudRef().SetDrive(false);
		CameraPathRef().StopScrub();
	} else {
		// The cursor is the panel's now, so the mouse is too. Without this, one click into the game
		// earlier in the session left CS2's mouse input live under the UI, and the next right-click
		// over the preview started CS2's own look-around and took the pointer away.
		MouseCapture_ReleaseForUi("ui cursor on");
	}
}

// Scaled-preview viewport (render-layer): scales the whole frame into the preview rect
// instead of showing a crop. Only renders while the editor is open and not recording.
void CameraEditor_SetScale(bool enabled) { CameraEditorHudRef().SetScale(enabled); }
void CameraEditor_ToggleScale() { CameraEditorHudRef().ToggleScale(); }
bool CameraEditor_ScaleActive() { return CameraEditorHudRef().ScaleEnabled(); }
void CameraEditor_SetUseTimeline(bool useTimeline) { CameraEditorHudRef().SetUseTimeline(useTimeline); }
void CameraEditor_ToggleUseTimeline() { CameraEditorHudRef().ToggleUseTimeline(); }
void CameraEditor_SetNativeTimeline() {
	CameraEditorHudRef().SetBottomMode(CameraEditorHud::BottomMode::Native);
}
const char* CameraEditor_HudViewName() {
	switch (CameraEditorHudRef().GetHudView()) {
	case CameraEditorHud::HudView::ShowAll: return "full";
	case CameraEditorHud::HudView::InGame: return "game";
	default: return "hidden";
	}
}
bool CameraEditor_ScaledHud(float& x0, float& y0, float& x1, float& y1) {
	// Both this and ConfigHud_ScaledHud now answer from the SAME committed state, so the bridges
	// that scale the native game HUD can never follow a different workspace than the world blit
	// did -- which they could when each asked its own panel in a fixed order.
	return WorkspacePreviewRef().Active(x0, y0, x1, y1);
}
bool CameraEditor_CustomizeModalOpen() { return CameraEditorHudRef().CustomizeModalOpen(); }
bool CameraEditor_PopupOpen() { return CameraEditorHudRef().PopupOpen(); }
bool CameraTimeline_MenuOpen() { return CameraTimelineHudRef().MenuOpen(); }
int CameraTimeline_NativeCameraMode() { return CameraTimelineHudRef().NativeCameraMode(); }
void CameraTimeline_PushWheel(int notches) { CameraTimelineHudRef().PushWheel(notches); }
void CameraEditor_CustomizePushWheel(int notches, int x, int y) { CameraEditorHudRef().PushCustomizeWheel(notches, x, y); }
void CameraEditor_CustomizePushChar(unsigned charCode) { CameraEditorHudRef().PushCustomizeChar(charCode); }
bool CameraEditor_TextCaptureActive() { return CameraEditorHudRef().TextCaptureActive(); }

// --- Camera-path preview: HUD masked (Tab) -> MovieHud hides itself this frame ---
bool CameraPath_PreviewHudHidden() { return CameraPathRef().PreviewHudHidden(); }

// --- Experimental graph editor (isolated opt-in overlay) ---
void GraphEditorExperiment_Set(bool enabled) { GraphEditorExperimentHudRef().SetEnabled(enabled); }
void GraphEditorExperiment_Toggle() { GraphEditorExperimentHudRef().Toggle(); }
bool GraphEditorExperiment_Enabled() { return GraphEditorExperimentHudRef().Enabled(); }
bool GraphEditorExperiment_OwnsView() { return GraphEditorExperimentHudRef().OwnsView(); }
bool GraphEditorExperiment_WantsCursor() {
	if (!GraphEditorExperimentHudRef().Enabled())
		return false;
	// Standalone graph-editor mode owns the cursor. Inside Camera Editor Mode,
	// G controls cursor ownership through the hosted timeline flag.
	return !CameraEditorHudRef().Enabled() || CameraTimelineHudRef().Cursor();
}

// --- Fisheye lens (barrel-distortion D3D pass, decoupled from deleted ActionCam) ---
bool Fisheye_Enabled() { return s_fisheyeEnabled; }
void Fisheye_SetEnabled(bool on) { s_fisheyeEnabled = on; PublishFisheye(); }
void Fisheye_Toggle() { Fisheye_SetEnabled(!s_fisheyeEnabled); }
void Fisheye_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = (argc >= 3) ? args->ArgV(2) : "toggle";
	if (0 == _stricmp(arg, "state")) {
		advancedfx::Message("[fisheye][state] %s\n", s_fisheyeEnabled ? "on" : "off");
		return;
	}
	if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "1")) Fisheye_SetEnabled(true);
	else if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "0")) Fisheye_SetEnabled(false);
	else Fisheye_Toggle();
	advancedfx::Message("mirv_filmmaker: fisheye lens %s.\n", s_fisheyeEnabled ? "ON" : "off");
}

} // namespace Filmmaker
