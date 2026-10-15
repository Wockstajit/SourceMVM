// Console command surface for the filmmaker demo browser: mirv_filmmaker.
// Kept in its own translation unit; registers via the CON_COMMAND macro.

#include "FilmmakerDebugCommand.h" // MvmCommandTrace (the mvm_debug post-command state dumper)
#include "FilmmakerHealth.h"       // "health": did the version-coupled machinery resolve?
#include "Filmmaker.h"
#include "EasterEggs/EasterEggs.h"
#include "EasterEggs/Osu/OsuGame.h"
#include "Movie/TimelineDock.h"
#include "Demo/DemoLibrary.h"
#include "Demo/DemoEntry.h"
#include "Demo/DemoProjectStore.h"
#include "Demo/PlayingDemoPath.h"
#include <filesystem>
#include "../hlaeFolder.h"
#include "Panorama/FilmmakerMenu.h"
#include "Panorama/PanoramaBridge.h"
#include "Panorama/CameraTimelineHud.h"
#include "Panorama/CameraEditorHud.h"
#include "Panorama/ConfigHud.h"
#include "Panorama/GraphEditorExperimentHud.h"
#include "PlayMenu/PlayMenuHud.h"
#include "Community/CommunityHud.h"
#include "Platform/TextEncoding.h"
#include "Movie/CamPathShake.h"
#include "Movie/CameraPath.h"
#include "Movie/SceneKeys.h"
#include "Props/Props.h"
#include "Movie/FollowCamera.h"
#include "Movie/ViewFx.h"
#include "Movie/SmoothView.h"
#include "Movie/DemoAimResampler.h"
#include "Movie/SmoothViewTest.h"
#include "Panorama/AimCrosshairHud.h"
#include "Movie/RecoilPivotMarker.h"
#include "Lens/LensFx.h"
#include "Movie/CameraShakeStack.h"
#include "Movie/HandheldCam.h"
#include "Movie/WeaponRecoil.h"
#include "Movie/WeaponBobProbe.h"
#include "Movie/WeaponBob.h"
#include "Movie/WeaponPivot.h"
#include "Movie/ParticleFx.h"
#include "Movie/RagdollPhysics.h" // "ragdollphys": gravity/friction/limit/collide convar knobs
#include "Movie/RagdollWeapon.h"  // "ragdollweapon": held death weapon + visual discharge
#include "Movie/DeathAnimations.h" // "deathanim": clip-exact death animations, then ragdoll
#include "Movie/NervousSystem.h" // "nervous": post-death reactions on the ground
#include "Movie/MethodActing.h"   // "methodacting": fire squint + idle blinking on living players
#include "Movie/MovieMode.h"
#include "Movie/DemoEndHold.h"
#include "Movie/PlaybackLoop.h"
#include "Movie/DemoTimescale.h"
#include "Movie/DemoScrub.h"
#include "Demo/DemoKeyframes.h"
#include "MouseCapture.h"
#include "Movie/HudBlackout.h"
#include "Cosmetics/CosmeticOverrideSystem.h"
#include "World/WorldCommand.h" // "world": lighting / atmosphere / weather + update-day health
#include "Environment/EnvironmentScene.h"
#include "Config/FilmmakerSettingsStore.h"
#include "Config/EffectsPersistence.h" // "effects": carry EFFECTS settings across a demo change
#include "Render/RenderCommand.h"
#include "Window/WindowResize.h" // "window": free window resizing + size/aspect readout
#include "Panorama/UiLiveReload.h"

#include "../ClientEntitySystem.h"
#include "../WrpConsole.h"
#include "../../Shared/AfxConsole.h"

#include <string>
#include <vector>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace {

std::string FormatDuration(int seconds) {
	if (seconds <= 0)
		return "?";
	char buf[32];
	snprintf(buf, sizeof(buf), "%d:%02d", seconds / 60, seconds % 60);
	return buf;
}

void PrintHelp(const char* cmd) {
	advancedfx::Message(
		"%s scan - (re)scan the CS2 install + your folders for demos.\n"
		"%s list - list discovered demos with their index.\n"
		"%s folders - show the scanned folders.\n"
		"%s addfolder [path] - add a quoted folder path, or open a folder picker.\n"
		"%s removefolder <index> - remove a saved folder (see 'folders').\n"
		"%s watch <index> - play the demo at <index> (see 'list').\n"
		"%s project [status] - show the source_mvm project folder for the playing demo.\n"
		"%s hide <index> - remove a demo from the list (the .dem file is KEPT on disk).\n"
		"%s hidden - list removed demos.\n"
		"%s unhideall - restore every removed demo.\n"
		, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd
	);
	advancedfx::Message(
		"Panorama UI:\n"
		"%s ui - open the demos page (while a demo/map is loaded).\n"
		"%s ui_status - show Panorama bridge status.\n"
		"%s ui_rebuild - rebuild the Panorama UI next frame.\n"
		"%s ui_eval <panorama js> - run Panorama JS in the UI context (live REPL).\n"
		"%s ui_context <hex> - (advanced) pin a context CUIPanel* by address.\n"
		"%s play [status|rebuild|rescan|ws ...|eval <js>] - main-menu Play page: the\n"
		"   Practice + Workshop Maps strip and the workshop map sort/filter sidebar.\n"
		"%s health [verbose] - did the version-coupled machinery resolve against THIS CS2\n"
		"   build? Run it first when things broke after a game update (see\n"
		"   Docs/agent/cs2-update-recovery.md).\n"
		, cmd, cmd, cmd, cmd, cmd, cmd, cmd
	);
	advancedfx::Message(
		"Movie director (in a demo):\n"
		"%s hud [on|off|toggle] - show/hide the camera help/status panel (also F8).\n"
		"   Scroll = cycle CS2's First person / Chase / Free cam (hold a mouse button to look around);\n"
		"   LMB/RMB = next/prev player; X = X-ray, off by default in demos;\n"
		"   Space = pause/resume; Left/Right = skip -/+5s.\n"
		"%s speedbar [on|off|toggle] - inline demo-bar speed buttons (off = native dropdown).\n"
		"%s mouse free|click|game|toggle|state - cursor modes. Default is CLICK: the desktop cursor\n"
		"                             stays yours until you click into the game. `game` = CS2's normal grab.\n"
		"%s timescale [<v>]         - demo playback speed; keeps all three speed scrubbers in step.\n"
		"%s endhold [on|off|toggle|status|postgame on|off|phase <n>] - stop before CS2's end-of-match\n"
		"                             screens (and before the demo's stop record); play again = restart.\n"
		"%s skip back|forward      - skip the demo -/+5s (also the demo bar's -5s / +5s buttons).\n"
		// SIX %s above (hud / speedbar / mouse / timescale / endhold / skip) -- count them
		// against this list whenever a line is added. A short list here is not a cosmetic bug: the
		// missing argument is read as a char* and dereferenced.
		, cmd, cmd, cmd, cmd, cmd, cmd
	);
	advancedfx::Message(
		"Camera markers / dolly path (in free cam):\n"
		"   K = place, L = delete aimed, F = edit aimed.\n"
		"%s marker [...] - full marker/path control (run for sub-help).\n"
		"%s camtl [...] - camera timeline scrubber (scrub, keys, easing; run for sub-help).\n"
		"%s editor [on|off|toggle] - dedicated CAMERA EDITOR workspace (preview + inspector + graph editor).\n"
		"%s editor scale [on|off|toggle] - TRUE scaled preview viewport (whole frame shrunk, not a crop).\n"
		"%s dock [native|camera|graph|toggle] - shared bottom dock (Regular timeline / Camera / Graph); alias: editor curveeditor.\n"
		"%s editor hud [hidden|game|full|cycle] - game UI behind the editor: hide all, in-game (radar+HP/ammo, no spectator panel), or full.\n"
		"%s editor debug [on|off|toggle] - viewport/HUD debug overlay (window/render-target/viewport numbers; compare vs normal game viewport).\n"
		"%s config [on|off|toggle] - lightweight CONFIG panel (general UI / display settings, no camera tools).\n"
		"%s viewfx sway|deadzone [<0-150>|off] - viewmodel-feel modifiers, as an intensity percent.\n"
		"%s smoothview [...] - Smooth View camera module: aim follow, breathing, tilt, fall/jump (run for sub-help).\n"
		"%s aimresample [on|off|lag|state] - rebuild the demo's recorded aim into a continuous per-frame signal (fixes viewmodel/crosshair jitter).\n"
		"%s smoothtest [start|stop] - one-shot capture for a live-vs-demo Smooth View comparison (run for sub-help).\n"
		"%s fx [...] - particle-effect toggles: impacts/tracers/muzzle/blood/explosions/molotov per-category On|Less|Off (run for sub-help; also in the Config panel).\n"
		"%s effects [keep on|off] [on|off] [reapply] [status] - carry the whole EFFECTS tab into the next demo (default on). `effects off` makes the module inert.\n"
		"%s ragdoll [on|off|toggle|preset|impact|grip|resistance|momentum|collapse|weight|variation|motion|status|reset] - procedural death physics and optional CS2-body motion guidance (also in Config).\n"
		"%s ragdollweapon keep|fire|chance|firems|holdms|state|reset - timed death weapon and visual discharge.\n"
		"%s methodacting [on|off|toggle|blink|squint|hold|attack|release|blinkrate] - players squint while\n"
		"    firing and blink while idle; eyes only (run for sub-help; also in Config).\n"
		"%s fisheye [on|off|toggle|state] - barrel-distortion lens pass (GoPro-style wide-angle look).\n"
		"%s pivotorb recoil|bob [on|off|toggle] [auto on|off] - world-space marker at that effect's\n"
		"    hinge; amber for recoil, cyan for bob.\n"
		"%s follow [...] - place and control a Follow / Lock-On camera.\n"
		"%s handheld [...] - handheld camera: procedural drift + baked motion-capture curves (run for sub-help; also in Config).\n"
		"%s recoil [...] - camera + weapon kick on the spectated player's shots (run for sub-help; also in Config).\n"
		"%s camshake state|reset|off - shared diagnostics for the shake layers above.\n"
		"%s pathshake [...] - GTA V shake styles ON A CAMERA KEYFRAME (right-click a timeline diamond; run for sub-help).\n"
		"%s render [...] - camera-editor render workflow (status, range, camera, start, cancel; run for sub-help).\n"
		"%s window [...] - free window resizing + the live size/aspect-ratio readout (run for sub-help).\n"
		, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd
	);
}

void DoUiStatus() {
	auto& b = Filmmaker::Menu().Bridge();
	advancedfx::Message(
		"filmmaker Panorama UI:\n"
		"  engine resolved   : %s\n"
		"  RunScript resolved: %s\n"
		"  context panel     : %p %s\n"
		"  built             : %s\n",
		b.HasEngine() ? "yes" : "no",
		b.HasRunScript() ? "yes" : "no",
		b.ContextPanel(),
		b.ContextPanel() ? "" : "(none yet - load a demo/map so the HUD panel exists)",
		Filmmaker::Menu().IsBuilt() ? "yes" : "no");
}

void DoList() {
	auto entries = Filmmaker::Library().Snapshot();
	if (Filmmaker::Library().IsScanning())
		advancedfx::Message("(scan in progress, list may be incomplete)\n");
	if (entries.empty()) {
		advancedfx::Message("No demos found yet. Try 'mirv_filmmaker scan'.\n");
		return;
	}
	advancedfx::Message("idx | map | length | scoreboard | file\n");
	for (size_t i = 0; i < entries.size(); ++i) {
		const Filmmaker::DemoEntry& e = entries[i];
		advancedfx::Message("%zu | %s | %s | %s | %s\n",
			i,
			e.map.empty() ? "?" : e.map.c_str(),
			FormatDuration(e.durationSeconds).c_str(),
			e.hasScoreboard ? "yes" : "no",
			e.fileName.c_str());
	}
	advancedfx::Message("%zu demo(s).\n", entries.size());
}

void DoFolders() {
	advancedfx::Message("install root (always scanned): %s\n",
		Filmmaker::WideToUtf8(Filmmaker::Library().InstallRoot()).c_str());
	auto folders = Filmmaker::Library().Folders();
	if (folders.empty()) {
		advancedfx::Message("no extra folders added.\n");
		return;
	}
	for (size_t i = 0; i < folders.size(); ++i)
		advancedfx::Message("%zu | %s\n", i, Filmmaker::WideToUtf8(folders[i]).c_str());
}

// Offline demo-only cosmetics override. All parsing + application now lives in the Cosmetics
// module (Filmmaker/Cosmetics/*). The override store is keyed by SteamID so cosmetics follow the
// player across pawn recreation / round resets / death / observer switches / demo seeks. See
// Filmmaker::Cosmetics_RunCommand for the full grammar
// (enabled|status|debug|clear|clearPlayer|target|player <steamId> weapon|knife|gloves|agent ...).
void DoCosmetics(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	Filmmaker::Cosmetics_RunCommand(argc, args, cmd);
}

void DoRemoveFolder(const char* arg) {
	int index = atoi(arg);
	auto folders = Filmmaker::Library().Folders();
	if (index < 0 || index >= (int)folders.size()) {
		advancedfx::Warning("mirv_filmmaker: no folder at index %d (see 'folders')\n", index);
		return;
	}
	if (Filmmaker::Library().RemoveFolder(folders[index])) {
		advancedfx::Message("removed folder %s\n", Filmmaker::WideToUtf8(folders[index]).c_str());
		Filmmaker::RequestRescan();
	}
}

} // namespace

CON_COMMAND(mirv_filmmaker, "Browse and play CS2 demos (filmmaker tool).") {
	const char* cmd = args->ArgV(0);
	const int argc = args->ArgC();
	Filmmaker::MvmCommandTrace trace(args);

	if (argc < 2) {
		PrintHelp(cmd);
		return;
	}

	const char* sub = args->ArgV(1);

	if (0 == _stricmp(sub, "scan") || 0 == _stricmp(sub, "rescan")) {
		Filmmaker::RequestRescan();
		advancedfx::Message("mirv_filmmaker: scan requested.\n");
	} else if (0 == _stricmp(sub, "list")) {
		DoList();
	} else if (0 == _stricmp(sub, "folders")) {
		DoFolders();
	} else if (0 == _stricmp(sub, "addfolder")) {
		if (argc >= 3) {
			const auto folder = Filmmaker::Utf8ToWide(args->ArgV(2));
			std::error_code ec;
			if (!std::filesystem::is_directory(std::filesystem::path(folder), ec)) {
				advancedfx::Warning("mirv_filmmaker: folder does not exist or cannot be read.\n");
				return;
			}
			Filmmaker::Library().AddFolder(folder);
			Filmmaker::RequestRescan();
			advancedfx::Message("mirv_filmmaker: demo folder added; scan requested.\n");
		} else {
			Filmmaker::RequestAddFolder();
			advancedfx::Message("mirv_filmmaker: opening folder picker...\n");
		}
	} else if (0 == _stricmp(sub, "removefolder")) {
		if (argc < 3) { advancedfx::Warning("usage: %s removefolder <index>\n", cmd); return; }
		DoRemoveFolder(args->ArgV(2));
	} else if (0 == _stricmp(sub, "watch")) {
		if (argc < 3) { advancedfx::Warning("usage: %s watch <index>\n", cmd); return; }
		Filmmaker::Watch((size_t)atoi(args->ArgV(2)));
	} else if (0 == _stricmp(sub, "project")) {
		const std::wstring root = Filmmaker::DemoProjectStore_Root();
		const std::wstring playing = Filmmaker::PlayingDemoPath();
		std::wstring dir;
		if (!playing.empty())
			dir = Filmmaker::DemoProjectStore_ProjectDirectory(Filmmaker::ProjectGame::Cs2, playing);
		advancedfx::Message("mirv_filmmaker: project root %s\n", Filmmaker::WideToUtf8(root).c_str());
		if (!playing.empty())
			advancedfx::Message("  playing %s\n  folder  %s\n",
				Filmmaker::WideToUtf8(playing).c_str(), Filmmaker::WideToUtf8(dir).c_str());
		else
			advancedfx::Message("  no demo playing\n");
	} else if (0 == _stricmp(sub, "hide")) {
		if (argc < 3) { advancedfx::Warning("usage: %s hide <index>\n", cmd); return; }
		const size_t idx = (size_t)atoi(args->ArgV(2));
		if (Filmmaker::Library().HideDemo(idx))
			advancedfx::Message("mirv_filmmaker: removed demo %zu from the list (file kept on disk).\n", idx);
		else
			advancedfx::Warning("mirv_filmmaker: no demo at index %zu (or already removed).\n", idx);
	} else if (0 == _stricmp(sub, "fphide")) {
		if (argc < 3) {
			advancedfx::Message("mirv_filmmaker: fphide = %s (hide first-person arms/gun while free cam is detached).\n",
				Filmmaker::FpHideWhenDetached() ? "on" : "off");
			return;
		}
		const char* v = args->ArgV(2);
		const bool on = (0 == _stricmp(v, "toggle")) ? !Filmmaker::FpHideWhenDetached() : (0 == _stricmp(v, "on"));
		Filmmaker::SetFpHideWhenDetached(on);
		advancedfx::Message("mirv_filmmaker: fphide = %s.\n", on ? "on" : "off");
	} else if (0 == _stricmp(sub, "world")) {
		Filmmaker::World_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "environment")) {
		Filmmaker::Environment::RunCommand(args);
	} else if (0 == _stricmp(sub, "ragdollphys")) {
		Filmmaker::RagdollPhysics_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "deathanim")) {
		Filmmaker::DeathAnimations_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "nervous")) {
		Filmmaker::NervousSystem_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "ragdollweapon")) {
		Filmmaker::RagdollWeapon_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "methodacting")) {
		Filmmaker::MethodActing_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "health")) {
		Filmmaker::Filmmaker_RunHealthCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "pumpstatus")) {
		uint64_t renderCalls = 0, mainCalls = 0, scans = 0;
		Filmmaker::PumpStatus(renderCalls, mainCalls, scans);
		auto entries = Filmmaker::Library().Snapshot();
		advancedfx::Message("filmmaker pumps:\n");
		advancedfx::Message("  render pump (Before_Present): %llu\n", (unsigned long long)renderCalls);
		advancedfx::Message("  main pump                   : %llu\n", (unsigned long long)mainCalls);
		advancedfx::Message("  scans started               : %llu\n", (unsigned long long)scans);
		advancedfx::Message("  scanning now                : %s\n", Filmmaker::Library().IsScanning() ? "yes" : "no");
		advancedfx::Message("  demos in list               : %zu\n", entries.size());
		if (renderCalls == 0)
			advancedfx::Warning("  RENDER PUMP IS DEAD - the Present/scenesystem hooks did not install.\n");
	} else if (0 == _stricmp(sub, "hidden")) {
		auto hidden = Filmmaker::Library().HiddenDemos();
		if (hidden.empty()) {
			advancedfx::Message("mirv_filmmaker: no removed demos.\n");
		} else {
			advancedfx::Message("mirv_filmmaker: %zu removed demo(s) (re-add the folder to restore):\n", hidden.size());
			for (const auto& p : hidden)
				advancedfx::Message("  %s\n", Filmmaker::WideToUtf8(p).c_str());
		}
	} else if (0 == _stricmp(sub, "unhideall")) {
		const size_t n = Filmmaker::Library().UnhideAll();
		advancedfx::Message("mirv_filmmaker: restored %zu removed demo(s).\n", n);
		Filmmaker::RequestRescan();
	} else if (0 == _stricmp(sub, "ui")) {
		Filmmaker::Menu().RequestShow();
		advancedfx::Message("mirv_filmmaker: opening demos page (must be in a demo/map).\n");
	} else if (0 == _stricmp(sub, "demoprobe")) {
		// Diagnostic: dump the demo-path candidates the engine-scan finds for the playing demo,
		// plus the final resolved path. Used to verify / tune ResolvePlayingDemoPath().
		Filmmaker::DebugProbePlayingDemoPath();
	} else if (0 == _stricmp(sub, "ui_status")) {
		DoUiStatus();
	} else if (0 == _stricmp(sub, "ui_context")) {
		if (argc < 3) { advancedfx::Warning("usage: %s ui_context <hex>\n", cmd); return; }
		Filmmaker::Menu().Bridge().SetContextPanel((void*)(uintptr_t)strtoull(args->ArgV(2), nullptr, 16));
		Filmmaker::Menu().RequestRebuild();
		DoUiStatus();
	} else if (0 == _stricmp(sub, "ui_rebuild")) {
		Filmmaker::Menu().RequestRebuild();
		advancedfx::Message("mirv_filmmaker: UI rebuild requested.\n");
	} else if (0 == _stricmp(sub, "ui_eval")) {
		if (argc < 3) { advancedfx::Warning("usage: %s ui_eval <panorama js>\n", cmd); return; }
		std::string js;
		for (int i = 2; i < argc; ++i) { if (i > 2) js += ' '; js += args->ArgV(i); }
		Filmmaker::Menu().RequestEval(js);
		advancedfx::Message("mirv_filmmaker: queued ui_eval (%zu chars).\n", js.size());
	} else if (0 == _stricmp(sub, "hud")) {
		// Show/hide the in-game movie-director help/status panel.
		// Bind it to a key, e.g.  bind "h" "mirv_filmmaker hud toggle"
		const char* arg = (argc >= 3) ? args->ArgV(2) : "toggle";
		if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "1")) Filmmaker::MovieHud_Set(true);
		else if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "0")) Filmmaker::MovieHud_Set(false);
		else Filmmaker::MovieHud_Toggle();
		advancedfx::Message("mirv_filmmaker: movie HUD %s.\n", Filmmaker::MovieHud_Visible() ? "shown" : "hidden");
	} else if (0 == _stricmp(sub, "hud_eval")) {
		if (argc < 3) { advancedfx::Warning("usage: %s hud_eval <panorama js>\n", cmd); return; }
		std::string js;
		for (int i = 2; i < argc; ++i) { if (i > 2) js += ' '; js += args->ArgV(i); }
		Filmmaker::MovieHud_Eval(js);
		advancedfx::Message("mirv_filmmaker: queued hud_eval (%zu chars).\n", js.size());
	} else if (0 == _stricmp(sub, "speedbar")) {
		// Inline demo-bar speed buttons; off restores the native timescale dropdown.
		const char* arg = (argc >= 3) ? args->ArgV(2) : "toggle";
		if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "1")) Filmmaker::DemoSpeedBar_Set(true);
		else if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "0")) Filmmaker::DemoSpeedBar_Set(false);
		else Filmmaker::DemoSpeedBar_Toggle();
		advancedfx::Message("mirv_filmmaker: demo speed buttons %s.\n", Filmmaker::DemoSpeedBar_Enabled() ? "on" : "off");
	} else if (0 == _stricmp(sub, "play")) {
		// Main-menu Play page: the Practice + Workshop Maps strip and the workshop map sidebar.
		Filmmaker::PlayMenu_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "community")) {
		// Main-menu HC Tracker badge and Report Bugs (writes a bug report to send on Discord).
		Filmmaker::Community_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "blackout")) {
		// F8's console twin: clear every overlay (ours, the gameplay HUD and the demo bar).
		// NOT named "hud" -- that subcommand already exists above and toggles only the
		// movie-director CONTROLS card, so a second "hud" branch here was simply unreachable.
		Filmmaker::HudBlackout_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "mouse")) {
		Filmmaker::MouseCapture_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "window")) {
		// Free resizing of the CS2 window + the size/aspect readout shown while you drag.
		Filmmaker::WindowResize_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "keyframes")) {
		// Keyframed demo copies for one-frame backward seeks -- see Demo/DemoKeyframes.h.
		Filmmaker::DemoKeyframes_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "scrub")) {
		// Live timeline scrubbing: the world follows a drag -- see Movie/DemoScrub.h.
		Filmmaker::DemoScrub_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "timescale")) {
		// Demo speed. Goes through here (not straight to `demo_timescale`) so all three speed
		// scrubbers read the same value -- see Movie/DemoTimescale.h.
		Filmmaker::DemoTimescale_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "endhold")) {
		// Stops playback before the match-over screens (and before the demo's stop record);
		// pressing play while held restarts the demo from the beginning. Default: on.
		Filmmaker::DemoEndHold_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "skip")) {
		// The native bar's -/+ buttons and the arrow keys share one step and one path.
		const char* dir = (argc >= 3) ? args->ArgV(2) : "";
		if (0 == _stricmp(dir, "back")) Filmmaker::MovieModeRef().SkipSeconds(-1);
		else if (0 == _stricmp(dir, "forward")) Filmmaker::MovieModeRef().SkipSeconds(+1);
		else advancedfx::Message("%s skip back|forward\n", cmd);
	} else if (0 == _stricmp(sub, "loop")) {
		// Playback loop region. Deliberately NOT the render or export range -- see PlaybackLoop.h.
		Filmmaker::PlaybackLoop_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "marker")) {
		Filmmaker::Marker_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "camtl")) {
		Filmmaker::CameraTimeline_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "follow")) {
		Filmmaker::Follow_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "cosmetics")) {
		DoCosmetics(argc, args, cmd);
	} else if (0 == _stricmp(sub, "grapheditor")) {
		Filmmaker::GraphEditor_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "scenekeys")) {
		Filmmaker::SceneKeys_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "props")) {
		Filmmaker::Props_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "dock")) {
		// The shared bottom dock (REGULAR TIMELINE / CAMERA / GRAPH). One verb, because one dock
		// is shown by the Camera Editor, the CONFIG panel and the Render workspace.
		Filmmaker::TimelineDock_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "editor")) {
		// Dedicated camera-editor workspace. Bind it to a key, e.g.
		//   bind "F9" "mirv_filmmaker editor toggle"
		Filmmaker::CameraEditor_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "config")) {
		// Lightweight CONFIG panel: general UI / game-display settings only (no camera tools).
		Filmmaker::ConfigHud_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "settings")) {
		Filmmaker::FilmmakerSettings_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "viewfx")) {
		// Camera "feel" modifiers: Quake/Doom-style strafe roll + weapon sway/bob.
		Filmmaker::ViewFx_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "fx")) {
		// Particle-effect modifiers: category-specific On/Off/Modern modes over impacts, tracers, muzzle
		// fx, blood, explosions, molotov, map ambience (runtime particle-create hook).
		Filmmaker::ParticleFx_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "effects")) {
		// Whether the whole EFFECTS tab (particles, lighting, atmosphere, weather & sky) carries
		// into the next demo or resets to the map default. NOT the particle switches themselves --
		// those stay under "fx".
		Filmmaker::EffectsPersistence_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "fisheye")) {
		Filmmaker::Fisheye_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "lens")) {
		// Depth of field + bokeh iris + cinematic lens flare (a post pass over the world frame).
		Filmmaker::LensFx_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "pivotorb")) {
		// Weapon Recoil pivot orb: shows where the gun hinges while you set Pivot.
		Filmmaker::RecoilPivotMarker_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "osu")) {
		// The osu! easter egg: open/close, beatmap library, offset, volume.
		Filmmaker::Osu_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "egg")) {
		// Easter eggs: status, hint preview, Vegas test, Clippy (automation surface).
		Filmmaker::EasterEggs_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "xhair")) {
		// True-aim crosshair: drawn where the shots actually go, not at screen centre.
		Filmmaker::AimCrosshair_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "smoothview")) {
		Filmmaker::SmoothView_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "aimresample")) {
		// Rebuilds the demo's recorded aim into a continuous per-frame signal. Standalone: it
		// feeds Smooth View but owns no Smooth View state.
		Filmmaker::DemoAimResampler_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "smoothtest")) {
		// One-shot live-vs-demo capture: arms SmoothView's + DemoAimResampler's own traces
		// together under one settings-snapshotted mvm_debug log. Owns no state of its own.
		Filmmaker::SmoothViewTest_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "handheld")) {
		// Handheld camera: procedural drift + baked motion-capture curves (CameraShakeStack).
		Filmmaker::HandheldCam_RunCommand(argc, args, cmd);
		} else if (0 == _stricmp(sub, "weaponrecoil")) {
		// Procedural viewmodel recoil (MW-style spring kick on the gun itself).
		Filmmaker::WeaponRecoil_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "pivot")) {
		// Where the weapon hinges. Shared by Weapon Recoil and Weapon Bob, and what the
		// pivot orb draws -- All base plus per-weapon-class overrides.
		Filmmaker::WeaponPivot_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "weaponbob")) {
		// The gun's walk cycle. Separate module from weaponrecoil; they share only the transport.
		Filmmaker::WeaponBob_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "bobprobe")) {
		// Phase 0 weapon-bob / reload-impact RESEARCH probe. Read-only, writes nothing.
		Filmmaker::WeaponBobProbe_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "camshake")) {
		// Stack-level diagnostics for the layers above.
		Filmmaker::CameraShake_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "pathshake")) {
		// AUTHORED, per-camera-keyframe GTA V shake. Unrelated to "camshake" above: that stack
		// is a live spectator effect and is gated off during camera-path playback; this one IS
		// the shot. Rides on top of the path pose and never touches the camera graph.
		Filmmaker::CamPathShake_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "render")) {
		Filmmaker::Render_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "livejs")) {
		Filmmaker::UiLive_RunCommand(argc, args, cmd);
	} else {
		PrintHelp(cmd);
	}
}
