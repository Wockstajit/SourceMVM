// The console surface: `mirv_filmmaker <subcommand>`; `mirv_deadlock` is a compatibility alias.
//
// Everything the tool does is reachable from here, because this is also how automation drives it
// over netcon. `mirv_filmmaker health` is the fast triage command: it reports whether the
// version-coupled machinery resolved at all, which is the difference between "the feature is
// broken" and "the hook never loaded".

#include "DeadlockAddresses.h"
#include "DeadlockCameraRuntime.h"
#include "DeadlockCameraCommands.h"
#include "DeadlockCommands.h"
#include "../Filmmaker/Platform/TextEncoding.h"
#include "DeadlockDebug.h"
#include "DeadlockEngine.h"
#include "DeadlockDemo.h"
#include "DeadlockInput.h"
#include "DeadlockHotkeys.h"
#include "DeadlockFrame.h"
#include "DeadlockNetcon.h"
#include "DeadlockSchema.h"
#include "DeadlockEntities.h"
#include "DeadlockHudBlackout.h"
#include "DeadlockMouseWheel.h"
#include "../Filmmaker/Deadlock/DeadlockGoToMath.h"
#include "DeadlockCameraRuntime.h"
#include "../Filmmaker/Movie/CameraBridge.h"
#include "DeadlockPanorama.h"
#include "DeadlockCursorClip.h"
#include "DeadlockPreUiHook.h"
#include "DeadlockViewportBlit.h"
#include "DeadlockOverlayDrawer.h"
#include "DeadlockPlaybackSpeed.h"
#include "DeadlockUi.h"
#include "DeadlockUiCommandBridge.h"
#include "DeadlockViewSetup.h"

#include "../WrpConsole.h"
#include "../../Shared/AfxConsole.h"
#include "../../Shared/MirvInput.h"
#include "../Filmmaker/Debug/MvmDebugLog.h"
#include "../Filmmaker/Deadlock/DeadlockGraph.h"
#include "../Filmmaker/Deadlock/DeadlockRender.h"
#include "../Filmmaker/Panorama/UiLiveReload.h"
#include "../Filmmaker/Deadlock/DeadlockDock.h"
#include "../Filmmaker/Deadlock/DeadlockPlaybackLoop.h"
#include "../Filmmaker/Deadlock/DeadlockPathShake.h"
#include "../Filmmaker/Deadlock/DeadlockFollowCamera.h"
#include "../Filmmaker/Deadlock/DeadlockShot.h"
#include "../Filmmaker/Deadlock/DeadlockWorkspaces.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Defined in DeadlockMirvCommands.cpp, which hosts the ported HLAE commands. Declared here rather
// than in a header because it exists for one diagnostic subcommand and nothing else.
namespace Deadlock {
void MirvCommands_ProbeRegistration();
void MirvCommands_DumpCvarVtable(int slots);
void MirvCommands_DumpCmdStructs(int count, int qwords);
}

namespace {

// ONE TABLE FOR THE HELP, CHECKED AGAINST THE DISPATCHER.
//
// The help used to be a hand-written string that nothing compared with the `_stricmp(sub, ...)`
// chain below, so it went stale: it left out follow, seek, skip, blit, preui and most marker and
// path verbs, and it described `play` as a plain pause toggle. Automation/tests/
// deadlock-command-table-tests.mjs fails when a dispatched verb has no row here, when a row names a
// verb the dispatcher does not handle, or when the UI sends a verb that does not exist -- which is
// how the timeline's +/-10 s buttons shipped sending a `seekrel` nobody handled.
//
// Help always prints under the canonical root. `mirv_deadlock` stays a silent alias.
struct HelpRow { const char* verb; const char* usage; const char* help; };
const HelpRow kHelpRows[] = {
	{ "health", "[addresses]", "health or cached address enumeration without engine calls" },
	{ "cvarprobe", "[cmds|vtable]", "is every command present in the game console" },
	{ "workspace", "off|camera|config|render|toggle", "open, switch or close the editor workspace" },
	{ "ui", "rebuild|screen <w> <h>|inspectorw <px>|toolkit|cursor toggle", "Panorama panel plumbing" },
	{ "ui_eval", "\"<js>\"", "run Panorama JS in the workspace (same as mirv_panorama eval)" },
	{ "livejs", "load|watch|unwatch|status <path>", "hot-reload Panorama JS from disk" },
	{ "panorama", "", "Panorama state transport status" },
	{ "view", "[report]|phase before|after|both|enable 0|1|write 0|1|offsets ...|dump [a] [b]", "view hook: calls, writes, offsets" },
	{ "engine", "slots|slot <name> <n>|tickinterval <s>|exec \"command\"", "engine diagnostics and explicit command submission" },
	{ "camera", "", "the pose the engine used this frame, and which feature owns it" },
	{ "freecam", "on|off|toggle", "free camera (same as mirv_input camera / end)" },
	{ "mouse", "game|ui|toggle", "editor mouse mode (J mirrors this)" },
	{ "pathshake", "...", "per-keyframe path shake (run for sub-help)" },
	{ "marker", "add|delete|clear|goto <i>|gotosel|update|reposition|sel prev|next|<i>|fov <v>|roll <v>|ease <mode> [i]", "camera path markers" },
	{ "path", "play|pause|save|load|interp linear|cubic|smooth|speedmode manual|constant|perseg|speed <0.2..1.0>|cycle|draw 0|1|drawmarkers 0|1|freeze 0|1", "camera path playback and settings" },
	{ "play", "", "editor open: play/pause the camera path; closed: demo pause/resume" },
	{ "scrub", "<tick>", "drag the camera-path playhead (no seek)" },
	{ "scrubend", "", "release the drag and seek the demo there" },
	{ "scrubtick", "<n>", "step the playhead by n ticks" },
	{ "seek", "<tick>", "seek the demo to an absolute tick" },
	{ "seekrel", "<ticks>", "seek the demo relative to the current tick (negative goes back)" },
	{ "skip", "<seconds>", "seek the demo relative to now, in seconds" },
	{ "speed", "up|down|<multiplier>", "playback speed" },
	{ "loop", "on|off|toggle|in|out|range <a> <b>|clear|status|torender", "playback loop" },
	{ "graph", "seed|clear|smooth|linear|undo|redo|delsel|channel <i>|solo <i>|drive on|off|toggle", "graph editor" },
	{ "follow", "toggle|live|place|reposition|clear|mode lockon|attach|hero <i>|attach <point>|nearest|goto|target clear|resetframing|resettracking|<setting> <v>", "follow and attached camera" },
	{ "render", "fps <1-1000>|range <a> <b>|folder|dir <path>|format <f>|quality <0-51>|size <w> <h>|bitrate <kbps>|encoder <auto|cpu|gpu>|speed <quality|balanced|fast>|start|stop", "render capture" },
	{ "dock", "native|camera|graph|rect <x> <y> <w> <h>|nativeheight <px>|settings", "which editor is docked below the preview" },
	{ "hud", "scaled|hidden|toggle", "the game HUD inside the preview" },
	{ "blackout", "[on|off|toggle|status]", "F8: clear every overlay, ours and the game's" },
	{ "preview", "[on|off|offsets <x> <y>]", "the scaled world preview" },
	{ "blit", "on|off", "scaled preview blit" },
	{ "overlay", "on|off|status", "in-world camera path, camera gizmos and the follow-target box -- on by default (see Docs/deadlock/cameras.md)" },
	{ "preui", "names [on|off]", "pre-UI render hook diagnostics" },
	{ "entities", "[list <filter> <n>|probe <index>]", "the replay's entity table" },
	{ "listentities", "[n]", "player-only roster: index, hero, player, team, hp" },
	{ "schema", "[layout <a> <b>|reload|raw <class>|field <class> <field>]", "entity offsets, resolved by name" },
	{ "dump", "schema [filter] [path]", "write the live schema to JSON" },
	{ "netcon", "[start|stop|status] [port]", "netcon listener" },
};

void PrintHelp() {
	for (const HelpRow& r : kHelpRows) {
		if (r.usage[0])
			advancedfx::Message("mirv_filmmaker %s %s\n    %s\n", r.verb, r.usage, r.help);
		else
			advancedfx::Message("mirv_filmmaker %s\n    %s\n", r.verb, r.help);
	}
}

// Uses only the hook's cached registry; safe to request before engine slots are re-derived.
void PrintAddresses() {
	Deadlock::AddressStatus rows[32];
	const int n = Deadlock::Addresses_Status(rows, 32);
	advancedfx::Message("  addresses:\n");
	for (int i = 0; i < n; ++i) {
		const Deadlock::AddressStatus& r = rows[i];
		if (Filmmaker::MvmDebugLog_Active())
			Filmmaker::MvmDebugLog_Linef("deadlock.address", "name=%s module=%s required=%d resolved=%d rva=0x%zx expected=0x%zx",
			                 r.name, r.module, r.required, r.resolved, r.rva, r.expectedRva);
		if (!r.resolved) {
			advancedfx::Warning("    %-26s %-10s BROKEN%s\n",
			                    r.name, r.module, r.required ? " (required)" : "");
			continue;
		}
		if (r.expectedRva && r.rva != r.expectedRva)
			advancedfx::Message("    %-26s %-10s rva 0x%zx  (moved from 0x%zx)\n",
			                    r.name, r.module, r.rva, r.expectedRva);
		else
			advancedfx::Message("    %-26s %-10s rva 0x%zx\n", r.name, r.module, r.rva);
	}
	advancedfx::Message("  required addresses: %s\n",
	                    Deadlock::Addresses_AllRequiredResolved() ? "resolved" : "BROKEN");
}

void PrintHealth() {
	advancedfx::Message("==== AfxHookDeadlock health ====\n");

	advancedfx::Message(
		"  pumps:     render=%llu  main=%llu\n",
		Deadlock::Frame_RenderCount(), Deadlock::Frame_MainCount());
	if (Deadlock::Frame_RenderCount() == 0)
		advancedfx::Warning(
			"  render pump is 0: the present / scenesystem hooks never installed. Check that\n"
"  first -- nothing else can work without it.\n");
	if (Deadlock::Frame_MainCount() == 0)
		advancedfx::Warning(
			"  main pump is 0: check the frame-stage signature, hook installation and diagnostic switches.\n");

	Deadlock::Demo_Report();
	Deadlock::Input_Report();
	Deadlock::CursorClip_Report();
	Deadlock::Schema_Report();
	PrintAddresses();

	advancedfx::Message(
		"  engine:    engineToClient=%s  demo=%s  paused=%s  tick=%d  level='%s'\n",
		Deadlock::Engine_HasEngineToClient() ? "yes" : "NO",
		Deadlock::Engine_IsPlayingDemo() ? "playing" : "no",
		Deadlock::Engine_IsDemoPaused() ? "yes" : "no",
		Deadlock::Engine_DemoTick(),
		Deadlock::Engine_LevelName().c_str());
	advancedfx::Message("  demofile:  %s\n", Deadlock::Engine_DemoFilePath().c_str());
	advancedfx::Message(
		"  tickrate:  %.6f s/tick (%s)\n",
		Deadlock::Engine_IntervalPerTick(),
		Deadlock::Engine_TickIntervalConfirmed() ? "measured from Deadlock" : "fallback, not measured");
	if (!Deadlock::Engine_TickIntervalConfirmed())
		advancedfx::Warning(
			"  Deadlock's live tick interval is unavailable. It paces the render output, so\n"
"  measure it and set 'engine tickinterval' before\n"
"  trusting a capture's frame timing.\n");
	if (Deadlock::Engine_HadFault())
		advancedfx::Warning(
			"  a guarded engine call has faulted: a verified vtable entry no longer matches this\n"
"  Deadlock build. Run the offline signature verifier before relaunching.\n");

	Deadlock::PreUiHook_Report();
	Deadlock::ViewportBlit_Report();
	Deadlock::ViewSetup_Report();
	Deadlock::ViewSetup_PreviewReport();
	Filmmaker::Workspaces_Report();
	Filmmaker::Shot_Report();
	Filmmaker::Graph_Report();
	Filmmaker::Render_Report();
	Deadlock::PlaybackSpeed_Report();
	Deadlock::Ui_Report();
	Deadlock::UiCommandBridge_Report();
	Deadlock::Hotkeys_Report();
	Deadlock::MouseWheel_Report();
	advancedfx::Message("  blackout:  %s (F8)\n",
	                    Deadlock::HudBlackout_Active() ? "BLACKED OUT" : "off");

	const Deadlock::NetconStats ns = Deadlock::Netcon_GetStats();
	advancedfx::Message(
		"  netcon:    %s port=%u clients=%d commands=%llu lines=%llu dropped=%llu\n",
		ns.active ? "listening" : "off", (unsigned)ns.port, ns.clients,
		ns.commandsExecuted, ns.linesSent, ns.linesDropped);

	const bool ok = Deadlock::Addresses_AllRequiredResolved()
		&& Deadlock::Frame_MainCount() > 0
		&& Deadlock::Engine_HasEngineToClient();
	advancedfx::Message("==== %s ====\n", ok ? "healthy" : "NOT healthy" );
}

long ParseNumber(const char* s, long fallback) {
	if (!s || !*s) return fallback;
	char* end = nullptr;
	// Base 0 so both 1200 and 0x4a0 work; offsets are natural in hex and slots in decimal.
	long v = std::strtol(s, &end, 0);
	if (end == s) return fallback;
	return v;
}

// ONE PLACE THAT SEEKS THE DEMO. `seek`, `seekrel` and `skip` all land here, so an absolute or a
// relative seek clamps the same way and leaves the same `deadlock.seek` log line -- which is what a
// timeline button that "did nothing" gets diagnosed from.
void SeekToTick(long target, const char* why) {
	if (target < 0) target = 0;
	const int from = Deadlock::Engine_DemoTick();
	char cmd[64];
	std::snprintf(cmd, sizeof(cmd), "demo_gototick %ld", target);
	Deadlock::Engine_ExecuteClientCmd(cmd);
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("deadlock.seek", "from=%d to=%ld why=%s", from, target, why ? why : "?");
}

// One console line, split into arguments the way the engine would split it. Double quotes group,
// so `ui_eval "$.Msg('hi')"` arrives as two arguments rather than as however many spaces the
// JavaScript happened to contain.
class LineArgs : public advancedfx::ICommandArgs {
public:
	explicit LineArgs(const char* line) {
		if (!line) return;
		const char* p = line;
		while (*p) {
			while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
			if (!*p) break;
			std::string token;
			if (*p == '"') {
				++p;
				while (*p && *p != '"') token.push_back(*p++);
				if (*p == '"') ++p;
			} else {
				while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') token.push_back(*p++);
			}
			m_Args.emplace_back(std::move(token));
		}
	}

	virtual int ArgC() override { return (int)m_Args.size(); }

	virtual char const* ArgV(int i) override {
		if (i < 0 || i >= (int)m_Args.size()) return "";
		return m_Args[i].c_str();
	}

private:
	std::vector<std::string> m_Args;
};

void MirvDeadlock_Run(advancedfx::ICommandArgs* args) {
	const int argc = args->ArgC();
	if (argc < 2) {
		PrintHelp();
		return;
	}
	const char* sub = args->ArgV(1);

	// Reachable without the engine on purpose: it exists to diagnose the case where the engine
	// cannot see our commands at all. See MirvCommands_ProbeRegistration.
	if (0 == _stricmp(sub, "cvarprobe")) {
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "cmds")) {
			Deadlock::MirvCommands_DumpCmdStructs(
				argc >= 4 ? (int)ParseNumber(args->ArgV(3), 4) : 4,
				argc >= 5 ? (int)ParseNumber(args->ArgV(4), 12) : 12);
			return;
		}
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "vtable")) {
			Deadlock::MirvCommands_DumpCvarVtable(argc >= 4 ? (int)ParseNumber(args->ArgV(3), 64) : 64);
			return;
		}
		Deadlock::MirvCommands_ProbeRegistration();
		return;
	}

	if (0 == _stricmp(sub, "health")) {
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "addresses")) {
			PrintAddresses();
			return;
		}
		PrintHealth();
		return;
	}

	if (0 == _stricmp(sub, "workspace")) {
		if (argc < 3) {
			advancedfx::Message("workspace = %s\n",
			                    Filmmaker::Workspaces_Name(Filmmaker::Workspaces_Current()));
			return;
		}
		if (0 == _stricmp(args->ArgV(2), "toggle")) {
			// What the native replay bar's CAM EDITOR button sends. A toggle rather than a switch,
			// because that button is the way back out as well as the way in.
			Filmmaker::Workspaces_Set(Filmmaker::Workspaces_Open()
				? Filmmaker::DeadlockWorkspace::Off
				: Filmmaker::DeadlockWorkspace::CameraEditor);
			return;
		}
		Filmmaker::DeadlockWorkspace ws;
		if (!Filmmaker::Workspaces_FromName(args->ArgV(2), &ws)) {
			advancedfx::Warning("unknown workspace '%s'.\n", args->ArgV(2));
			return;
		}
		Filmmaker::Workspaces_Set(ws);
		return;
	}

	if (0 == _stricmp(sub, "ui")) {
		if (argc >= 5 && 0 == _stricmp(args->ArgV(2), "screen")) {
			// Issued by the workspace panel whenever its measured screen changes. Panorama is the
			// only side that can measure it, and C++ owns everything derived from it, so this one
			// command is the whole channel between the two.
			Filmmaker::Workspaces_SetScreenSize((int)ParseNumber(args->ArgV(3), 0),
			                                    (int)ParseNumber(args->ArgV(4), 0));
			return;
		}
		if (argc >= 4 && 0 == _stricmp(args->ArgV(2), "inspectorw")) {
			// The column's drag edge, coalesced to a few commands per second while dragging and one
			// more on release. C++ owns the width because C++ solves the layout the blit follows.
			Filmmaker::Workspaces_SetInspectorWidth((int)ParseNumber(args->ArgV(3), 0));
			return;
		}
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "rebuild")) {
			Filmmaker::Workspaces_RequestRebuild();
			advancedfx::Message("panels will rebuild on the next frame.\n");
			return;
		}
		if (argc >= 4 && 0 == _stricmp(args->ArgV(2), "toolkit")) {
			// The probe's answer, coming back from inside the Panorama context. RunScript has no
			// return value, so a question asked of that context can only be answered this way.
			Deadlock::Ui_SetToolkitAvailable(0 != ParseNumber(args->ArgV(3), 0));
			return;
		}
		Filmmaker::Workspaces_Report();
		return;
	}

	if (0 == _stricmp(sub, "ui_eval")) {
		if (argc < 3) {
			advancedfx::Message("ui_eval \"<javascript>\"\n");
			return;
		}
		// The tokenizer splits an argument on quotes and commas, so a JS snippet arrives as
		// several arguments that have to be rejoined with the spaces it removed. Rejoining with a
		// single space is safe for JavaScript and is what the CS2 side does.
		Deadlock::CameraCommands_EvalJs(args, 2, "ui_eval");
		return;
	}

	if (0 == _stricmp(sub, "panorama")) {
		Deadlock::Panorama_ReportCandidates();
		return;
	}

	if (0 == _stricmp(sub, "view")) {
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "phase")) {
			if (argc < 4) {
				advancedfx::Message("phase = %s\n",
				                    Deadlock::ViewSetup_PhaseName(Deadlock::ViewSetup_GetPhase()));
				return;
			}
			Deadlock::ViewPhase phase;
			if (!Deadlock::ViewSetup_PhaseFromName(args->ArgV(3), &phase)) {
				advancedfx::Warning("phase must be before, after or both.\n");
				return;
			}
			Deadlock::ViewSetup_SetPhase(phase);
			advancedfx::Message("phase = %s\n", Deadlock::ViewSetup_PhaseName(phase));
			return;
		}
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "enable")) {
			const bool enable = (argc < 4) || (ParseNumber(args->ArgV(3), 1) != 0);
			Deadlock::ViewSetup_SetEnabled(enable);
			return;
		}
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "write")) {
			Deadlock::ViewSetup_SetWriteEnabled((argc < 4) || (ParseNumber(args->ArgV(3), 1) != 0));
			return;
		}
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "dump")) {
			const size_t first = (size_t)ParseNumber(argc >= 4 ? args->ArgV(3) : "", 0x400);
			const size_t last = (size_t)ParseNumber(argc >= 5 ? args->ArgV(4) : "", (long)(first + 0x200));
			Deadlock::ViewSetup_Dump(first, last);
			return;
		}
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "offsets")) {
			if (argc < 8) {
				const Deadlock::ViewOffsets o = Deadlock::ViewSetup_GetOffsets();
				advancedfx::Message(
					"offsets: origin=0x%zx angles=0x%zx fov=0x%zx width=0x%zx height=0x%zx\n",
					(size_t)o.origin, (size_t)o.angles, (size_t)o.fov,
					(size_t)o.width, (size_t)o.height);
				return;
			}
			// Seeded from the CURRENT offsets, not default-constructed. A fresh struct leaves
			// `flags` uninitialised, and since this command does not take a flags argument that
			// gave it a stack address -- every read then faulted before the sanity check could
			// run, so the report went on saying "viewport reads as 0x0" with correct offsets in
			// place and nothing to suggest the command itself was the problem.
			Deadlock::ViewOffsets o = Deadlock::ViewSetup_GetOffsets();
			o.origin = ParseNumber(args->ArgV(3), (long)o.origin);
			o.angles = ParseNumber(args->ArgV(4), (long)o.angles);
			o.fov = ParseNumber(args->ArgV(5), (long)o.fov);
			o.width = ParseNumber(args->ArgV(6), (long)o.width);
			o.height = ParseNumber(args->ArgV(7), (long)o.height);
			Deadlock::ViewSetup_SetOffsets(o);
			advancedfx::Message("view offsets updated.\n");
			return;
		}
		Deadlock::ViewSetup_Report();
		return;
	}

	if (0 == _stricmp(sub, "engine")) {
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "exec")) {
			// Require one quoted argument so nested command syntax is preserved exactly.
			if (argc != 4 || !args->ArgV(3)[0]) {
				advancedfx::Warning("engine exec requires one nonempty quoted command.\n");
				return;
			}
			const bool submitted = Deadlock::Engine_ExecuteClientCmd(args->ArgV(3));
			if (Filmmaker::MvmDebugLog_Active())
				Filmmaker::MvmDebugLog_Linef("deadlock.engine.exec", "slot=%d submitted=%d command=%s",
					Deadlock::Engine_GetSlot(Deadlock::EngineSlot::ExecuteClientCmd), submitted, args->ArgV(3));
			advancedfx::Message("engine command %s\n", submitted ? "submitted" : "FAILED");
			return;
		}

		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "slots")) {
			advancedfx::Message("engine slots:\n");
			for (int i = 0; i < (int)Deadlock::EngineSlot::Count; ++i) {
				Deadlock::EngineSlot s = (Deadlock::EngineSlot)i;
				advancedfx::Message("  %-18s %d\n",
				                    Deadlock::Engine_SlotName(s), Deadlock::Engine_GetSlot(s));
			}
			return;
		}
		if (argc >= 5 && 0 == _stricmp(args->ArgV(2), "slot")) {
			Deadlock::EngineSlot s;
			if (!Deadlock::Engine_SlotFromName(args->ArgV(3), &s)) {
				advancedfx::Warning("unknown slot '%s' (see 'engine slots').\n", args->ArgV(3));
				return;
			}
			Deadlock::Engine_SetSlot(s, (int)ParseNumber(args->ArgV(4), -1));
			return;
		}
		if (argc >= 4 && 0 == _stricmp(args->ArgV(2), "tickinterval")) {
			const double seconds = std::atof(args->ArgV(3));
			Deadlock::Engine_SetIntervalPerTick(seconds);
			advancedfx::Message("tick interval = %.6f (%.2f tick/s)\n",
			                    Deadlock::Engine_IntervalPerTick(),
			                    seconds > 0.0 ? 1.0 / seconds : 0.0);
			return;
		}
		advancedfx::Message("engine: slots | slot <name> <n> |"
" tickinterval <s> | exec \"command\"\n");
		return;
	}

	if (0 == _stricmp(sub, "freecam")) {
		// A bare `freecam` REPORTS. It used to toggle, so a status query switched the free camera on
		// and pinned the view without anyone asking for it.
		if (argc < 3) {
			advancedfx::Message("free cam %s\n", Deadlock::CameraRuntime_FreeCam() ? "on" : "off");
			return;
		}
		const char* op = args->ArgV(2);
		bool enable = false;
		if (0 == _stricmp(op, "on")) enable = true;
		else if (0 == _stricmp(op, "off")) enable = false;
		else if (0 == _stricmp(op, "toggle")) enable = !Deadlock::CameraRuntime_FreeCam();
		else {
			advancedfx::Message("freecam [on|off|toggle]\n");
			return;
		}
		if (!Deadlock::CameraRuntime_SetFreeCam(enable, "freecam")) {
			advancedfx::Warning("the camera runtime is not up.\n");
			return;
		}
		advancedfx::Message("free cam %s\n", enable ? "on" : "off");
		return;
	}

	if (0 == _stricmp(sub, "mouse")) {
		bool enable = !Deadlock::Input_UiMouse();
		if (argc >= 3) {
			if (0 == _stricmp(args->ArgV(2), "ui") || 0 == _stricmp(args->ArgV(2), "on")) enable = true;
			else if (0 == _stricmp(args->ArgV(2), "game") || 0 == _stricmp(args->ArgV(2), "off")) enable = false;
		}
		Deadlock::Input_SetUiMouse(enable);
		advancedfx::Message("editor mouse = %s (native Deadlock binding: J)\n", enable ? "UI" : "GAME");
		return;
	}

	if (0 == _stricmp(sub, "pathshake")) {
		Filmmaker::DeadlockPathShake_RunCommand(argc, args, "mirv_filmmaker");
		return;
	}

	if (0 == _stricmp(sub, "marker")) {
		if (argc < 3) {
			Filmmaker::Shot_Report();
			return;
		}
		const char* op = args->ArgV(2);
		if (0 == _stricmp(op, "add")) { Filmmaker::Shot_AddMarkerHere(); return; }
		if (0 == _stricmp(op, "delete")) { Filmmaker::Shot_DeleteSelected(); return; }
		if (0 == _stricmp(op, "clear")) { Filmmaker::Shot_Clear(); return; }
		// Select AND go there (free cam on, paused, seeked, posed) -- CS2's SelectForEditor.
		if (0 == _stricmp(op, "goto") && argc >= 4) {
			if (!Filmmaker::Shot_SelectForEditor((int)ParseNumber(args->ArgV(3), -1)))
				advancedfx::Warning("marker goto: no such marker.\n");
			return;
		}
		if (0 == _stricmp(op, "gotosel")) {
			Filmmaker::Shot_Select(Filmmaker::Shot_Selected());
			return;
		}
		if (0 == _stricmp(op, "update")) { Filmmaker::Shot_UpdateSelectedFromCamera(); return; }
		// CS2's "Tick -> Here": retime the selected marker to the current tick (pose untouched).
		if (0 == _stricmp(op, "tickhere")) {
			if (!Filmmaker::Shot_RetimeSelectedToCurrentTick())
				advancedfx::Warning("marker tickhere: nothing selected, or no demo tick.\n");
			return;
		}
		// The marker under the crosshair -- CS2's F (select + go to it) and L (delete). Both no-op
		// with nothing aimed at, which is what lets them live on plain keys with no panel open.
		if (0 == _stricmp(op, "hovergoto")) {
			const int h = Filmmaker::Shot_HoveredMarker();
			if (h >= 0) Filmmaker::Shot_SelectForEditor(h);
			return;
		}
		if (0 == _stricmp(op, "hoverdelete")) {
			const int h = Filmmaker::Shot_HoveredMarker();
			if (h >= 0) { Filmmaker::Shot_Select(h); Filmmaker::Shot_DeleteSelected(); }
			return;
		}
		// Step the selection, wrapping. The PATH card stepper and the timeline both come here, so
		// neither works out the next index for itself and disagrees at the ends.
		if (0 == _stricmp(op, "sel") && argc >= 4) {
			const char* dir = args->ArgV(3);
			if (0 == _stricmp(dir, "prev")) { Filmmaker::Shot_SelectStep(-1); return; }
			if (0 == _stricmp(dir, "next")) { Filmmaker::Shot_SelectStep(1); return; }
			Filmmaker::Shot_Select((int)ParseNumber(dir, -1));
			return;
		}
		// Per-marker values. Each says so when nothing is selected rather than writing marker zero.
		if (0 == _stricmp(op, "fov") && argc >= 4) {
			if (!Filmmaker::Shot_SetSelectedFov(std::atof(args->ArgV(3))))
				advancedfx::Warning("marker fov: nothing selected.\n");
			return;
		}
		if (0 == _stricmp(op, "roll") && argc >= 4) {
			if (!Filmmaker::Shot_SetSelectedRoll(std::atof(args->ArgV(3))))
				advancedfx::Warning("marker roll: nothing selected.\n");
			return;
		}
		// The selected marker's OUTGOING segment speed (Per-Segment mode only). Mirrors CS2's
		// "marker speedmul" -- see DeadlockCameraBodyJs.h's Amount stepper for the caller.
		if (0 == _stricmp(op, "speedmul") && argc >= 4) {
			if (!Filmmaker::Shot_SetSelectedSpeedMul(std::atof(args->ArgV(3))))
				advancedfx::Warning("marker speedmul: nothing selected, or selection is the last marker.\n");
			return;
		}
		if (0 == _stricmp(op, "ease") && argc >= 4) {
			const char* name = args->ArgV(3);
			int ease = -1;
			if (0 == _stricmp(name, "none")) ease = 0;
			else if (0 == _stricmp(name, "in")) ease = 1;
			else if (0 == _stricmp(name, "out")) ease = 2;
			else if (0 == _stricmp(name, "inout")) ease = 3;
			if (ease < 0) { advancedfx::Message("marker ease: none | in | out | inout [marker index]\n"); return; }
			const int index = argc >= 5 ? std::atoi(args->ArgV(4)) : Filmmaker::Shot_Selected();
			if (!Filmmaker::Shot_SetMarkerEase(index, ease))
				advancedfx::Warning("marker ease: no marker %d.\n", index);
			return;
		}
		if (0 == _stricmp(op, "reposition")) {
			// The camera has to be flown first, so this is `update` by another name until a
			// click-to-place gesture exists. Saying what it did beats a button with no visible effect.
			if (!Filmmaker::Shot_UpdateSelectedFromCamera())
				advancedfx::Warning("marker reposition: nothing selected.\n");
			else
				advancedfx::Message("marker: moved the selected camera to the current view.\n");
			return;
		}
		advancedfx::Message(
			"marker: add | delete | clear | goto <index> | gotosel | update | tickhere | sel prev|next|<i>\n"
			"        | fov <v> | roll <v> | ease none|in|out|inout | speedmul <0.2..1.0> | reposition\n"
			"        | hovergoto | hoverdelete  (the marker under the crosshair; F and L)\n");
		return;
	}

	if (0 == _stricmp(sub, "path")) {
		if (argc < 3) { Filmmaker::Shot_Report(); return; }
		const char* op = args->ArgV(2);
		if (0 == _stricmp(op, "play")) { Filmmaker::Shot_Play(); return; }
		if (0 == _stricmp(op, "pause")) { Filmmaker::Shot_Pause(); return; }
		if (0 == _stricmp(op, "save")) { Filmmaker::Shot_Save(); return; }
		if (0 == _stricmp(op, "load")) { Filmmaker::Shot_Load(); return; }
		// The curve is the whole path, and it is the SAME setting the graph Linear / Smooth pair
		// writes, so it goes to that owner rather than to a second copy that could disagree.
		if (0 == _stricmp(op, "interp") && argc >= 4) {
			const char* how = args->ArgV(3);
			if (0 == _stricmp(how, "cubic") || 0 == _stricmp(how, "smooth")) {
				Filmmaker::Graph_SetSmooth(true, false);
				return;
			}
			if (0 == _stricmp(how, "linear")) { Filmmaker::Graph_SetSmooth(false, false); return; }
			advancedfx::Message("path interp: linear | cubic\n");
			return;
		}
		if (0 == _stricmp(op, "speedmode") && argc >= 4) {
			const char* m = args->ArgV(3);
			if (0 == _stricmp(m, "manual")) { Filmmaker::Shot_SetSpeedMode(0); return; }
			if (0 == _stricmp(m, "constant")) { Filmmaker::Shot_SetSpeedMode(1); return; }
			if (0 == _stricmp(m, "perseg")) { Filmmaker::Shot_SetSpeedMode(2); return; }
			advancedfx::Message("path speedmode: manual | constant | perseg\n");
			return;
		}
		// Global Constant-mode speed multiplier -- CS2's "marker constspeed", moved under `path`
		// alongside `speedmode` since it is path-wide settings, not a per-marker value.
		if (0 == _stricmp(op, "speed") && argc >= 4) {
			const char* v = args->ArgV(3);
			if (0 == _stricmp(v, "cycle")) { Filmmaker::Shot_CycleConstSpeed(); return; }
			Filmmaker::Shot_SetConstSpeed(std::atof(v));
			return;
		}
		if (0 == _stricmp(op, "draw") && argc >= 4) {
			Filmmaker::Shot_SetDrawPath(0 != (int)ParseNumber(args->ArgV(3), 0));
			return;
		}
		if (0 == _stricmp(op, "drawmarkers") && argc >= 4) {
			Filmmaker::Shot_SetDrawMarkers(0 != (int)ParseNumber(args->ArgV(3), 0));
			return;
		}
		if (0 == _stricmp(op, "freeze") && argc >= 4) {
			const bool on = 0 != (int)ParseNumber(args->ArgV(3), 0);
			Filmmaker::Shot_SetFreeze(on);
			// Holding the tick means the action stops while a camera is placed, so it pauses the
			// demo as well as recording the flag. A flag that did not pause would be a setting with
			// no effect.
			if (on && !Deadlock::Engine_IsDemoPaused())
				Deadlock::Engine_ExecuteClientCmd("demo_pause");
			return;
		}
		advancedfx::Message(
			"path: play | pause | save | load | interp linear|cubic\n"
			"      | speedmode manual|constant|perseg | speed <0.2..1.0>|cycle\n"
			"      | draw 0|1 | drawmarkers 0|1 | freeze 0|1\n");
		return;
	}

	// --- the follow camera ---------------------------------------------------------------------
	//
	// A PLACED camera that locks on to a hero or rides a point on its model. Every subcommand
	// writes one field: the column writes one field at a time, and a compound setter would make
	// "which control changed this" unanswerable from the log.
	if (0 == _stricmp(sub, "follow")) {
		if (argc < 3) { Filmmaker::FollowCamera_Report(); return; }
		const char* op = args->ArgV(2);
		const char* arg = (argc >= 4) ? args->ArgV(3) : "";

		if (0 == _stricmp(op, "toggle")) { Filmmaker::FollowCamera_Toggle(); return; }
		if (0 == _stricmp(op, "live")) { Filmmaker::FollowCamera_SetOn(true); return; }
		if (0 == _stricmp(op, "place")) { Filmmaker::FollowCamera_Place(); return; }
		if (0 == _stricmp(op, "reposition")) { Filmmaker::FollowCamera_Reposition(); return; }
		if (0 == _stricmp(op, "clear")) { Filmmaker::FollowCamera_Clear(); return; }
		if (0 == _stricmp(op, "mode") && argc >= 4) {
			Filmmaker::FollowCamera_SetMode(0 == _stricmp(arg, "attach")
				? Filmmaker::FollowMode::Attach : Filmmaker::FollowMode::LockOn);
			return;
		}
		if (0 == _stricmp(op, "hero") && argc >= 4) {
			Filmmaker::FollowCamera_SetHero((int)ParseNumber(arg, -1));
			return;
		}
		if (0 == _stricmp(op, "attach") && argc >= 4) { Filmmaker::FollowCamera_SetAttach(arg); return; }
		if (0 == _stricmp(op, "resetframing")) { Filmmaker::FollowCamera_ResetFraming(); return; }
		if (0 == _stricmp(op, "resettracking")) { Filmmaker::FollowCamera_ResetTracking(); return; }
		if (0 == _stricmp(op, "nearest")) {
			// The hero nearest the camera you are looking through right now, which is what the
			// column's NEAREST button means by "nearest".
			double origin[3], angles[3], fov;
			Filmmaker::CameraBridge_GetCurrentCamera(origin, angles, fov);
			const int hero = Deadlock::Entities_NearestHero(origin);
			if (hero < 0) {
				advancedfx::Warning("follow nearest: no hero pawns in this replay right now.\n");
				return;
			}
			Filmmaker::FollowCamera_SetHero(hero);
			advancedfx::Message("follow: hero -> %d\n", hero);
			return;
		}
		if (0 == _stricmp(op, "goto")) {
			// Behind and above the TARGETED hero, looking at their eyes -- same "Go To" action as
			// CS2's Follow inspector (DeadlockGoToMath.h). Independent of whether the follow camera
			// is even on or placed: picking a hero (or Nearest) and clicking Go To is how you look at
			// them BEFORE deciding where to place a camera, not only after.
			const int hero = Filmmaker::FollowCamera_Get().hero;
			if (hero < 0) {
				advancedfx::Warning("follow goto: no hero picked -- Nearest, or pick one in the dropdown, first.\n");
				return;
			}
			void* pawn = Deadlock::Entities_Get(hero);
			if (!pawn || Deadlock::Entities_ClassName(pawn) != "player") {
				advancedfx::Warning("follow goto: hero %d is not in the replay right now.\n", hero);
				return;
			}
			double eye[3], angles[3];
			if (!Deadlock::Entities_EyeOrigin(pawn, eye) || !Deadlock::Entities_Angles(pawn, angles)) {
				advancedfx::Warning("follow goto: could not read hero %d's position.\n", hero);
				return;
			}
			const Deadlock::GoToPose pose = Deadlock::SolveGoToPose(eye[0], eye[1], eye[2], angles[1]);
			if (!pose.ok) {
				advancedfx::Warning("follow goto: solve failed (non-finite hero position/angle).\n");
				return;
			}
			// The free camera has to be ON for its one-shot placement to be applied; pushing the pose
			// with the free camera off leaves it pending, to jump in later.
			Deadlock::CameraRuntime_SetFreeCam(true, "follow goto");
			Filmmaker::CameraBridge_SetCameraPose(pose.position.x, pose.position.y, pose.position.z,
				pose.pitch, pose.yaw, pose.roll, Filmmaker::FollowCamera_Get().fov);
			advancedfx::Message("follow: go to hero %d -> %.1f %.1f %.1f\n",
				hero, pose.position.x, pose.position.y, pose.position.z);
			return;
		}
		if (0 == _stricmp(op, "target")) {
			Filmmaker::FollowCamera_SetHero(-1);
			advancedfx::Message("follow: target cleared.\n");
			return;
		}
		if (argc >= 4 && Filmmaker::FollowCamera_SetFlag(op, 0 != (int)ParseNumber(arg, 0))) return;
		if (argc >= 4 && Filmmaker::FollowCamera_SetValue(op, std::atof(arg))) return;

		advancedfx::Message(
			"follow: toggle | live | place | reposition | clear | mode lockon|attach\n"
			"        | hero <index> | attach <point> | nearest | goto | target clear\n"
			"        | offx|offy|offz|rotpitch|rotyaw|rotroll|fov|look|pos|predict|deadzone|maxturn|deathhold <v>\n"
			"        | releaseondeath|holdlast|retarget 0|1 | resetframing | resettracking\n");
		return;
	}

	if (0 == _stricmp(sub, "seek")) {
		// SEEK THE DEMO to an absolute tick. Deliberately not `scrub`, which drags the camera
		// SHOT's playhead: those are two different playheads, and a marker click that reached for
		// the wrong one landed the demo 44000 ticks away from the kill it was aimed at.
		if (argc < 3) return;
		SeekToTick((long)ParseNumber(args->ArgV(2), 0), "seek");
		return;
	}

	if (0 == _stricmp(sub, "seekrel")) {
		// Relative seek in TICKS: the Regular Timeline's +/-10 s and +/-60 s buttons send 640 and 3840.
		// They sent this verb for a long time with nothing handling it, so the buttons fell through to
		// the help text whenever the native button they prefer was unavailable.
		if (argc < 3) {
			advancedfx::Message("seekrel <ticks>  - relative seek, negative goes back\n");
			return;
		}
		const int tick = Deadlock::Engine_DemoTick();
		if (tick < 0) {
			advancedfx::Warning("seekrel: no replay tick available.\n");
			return;
		}
		SeekToTick((long)tick + ParseNumber(args->ArgV(2), 0), "seekrel");
		return;
	}

	if (0 == _stricmp(sub, "scrub")) {
		if (argc < 3) return;
		Filmmaker::Shot_BeginScrub(std::atof(args->ArgV(2)));
		return;
	}

	if (0 == _stricmp(sub, "scrubend")) {
		Filmmaker::Shot_EndScrub();
		return;
	}

	if (0 == _stricmp(sub, "play")) {
		// Space with the timeline OPEN plays the camera path; Space with it closed is a plain demo
		// pause or resume. Keeping those two verbs distinct is the whole gating contract.
		if (Filmmaker::Workspaces_Open() && Filmmaker::Shot_MarkerCount() >= 2) {
			if (Filmmaker::Shot_Playing()) Filmmaker::Shot_Pause();
			else Filmmaker::Shot_Play();
			return;
		}
		Deadlock::Engine_ExecuteClientCmd(
			Deadlock::Engine_IsDemoPaused() ? "demo_resume" : "demo_pause");
		return;
	}

	if (0 == _stricmp(sub, "graph")) {
		if (argc < 3) { Filmmaker::Graph_Report(); return; }
		const char* op = args->ArgV(2);
		if (0 == _stricmp(op, "seed")) { Filmmaker::Graph_SeedFromShot(); return; }
		if (0 == _stricmp(op, "clear")) { Filmmaker::Graph_Clear(); return; }
		if (0 == _stricmp(op, "smooth")) { Filmmaker::Graph_SetSmooth(true, false); return; }
		if (0 == _stricmp(op, "linear")) { Filmmaker::Graph_SetSmooth(false, false); return; }
		if (0 == _stricmp(op, "undo")) { Filmmaker::Graph_Undo(); return; }
		if (0 == _stricmp(op, "redo")) { Filmmaker::Graph_Redo(); return; }
		if (0 == _stricmp(op, "delsel")) { Filmmaker::Graph_DeleteSelected(); return; }
		if (0 == _stricmp(op, "channel") && argc >= 4) {
			Filmmaker::Graph_ToggleChannel((int)ParseNumber(args->ArgV(3), -1));
			return;
		}
		if (0 == _stricmp(op, "solo") && argc >= 4) {
			Filmmaker::Graph_SoloChannel((int)ParseNumber(args->ArgV(3), -1));
			return;
		}
		if (0 == _stricmp(op, "drive")) {
			bool enable = !Filmmaker::Graph_Drive();
			if (argc >= 4) {
				if (0 == _stricmp(args->ArgV(3), "on")) enable = true;
				else if (0 == _stricmp(args->ArgV(3), "off")) enable = false;
			}
			Filmmaker::Graph_SetDrive(enable);
			return;
		}
		Filmmaker::Graph_Report();
		return;
	}

	if (0 == _stricmp(sub, "render")) {
		if (argc < 3) { Filmmaker::Render_Report(); return; }
		const char* op = args->ArgV(2);
		if (0 == _stricmp(op, "size") && argc >= 5) {
            Filmmaker::Render_SetSize((int)ParseNumber(args->ArgV(3), 0), (int)ParseNumber(args->ArgV(4), 0)); return;
        }
        if (0 == _stricmp(op, "bitrate") && argc >= 4) { Filmmaker::Render_SetBitrate((int)ParseNumber(args->ArgV(3), 0)); return; }
        if (0 == _stricmp(op, "folder")) { Filmmaker::Render_PickFolder(); return; }
		if (0 == _stricmp(op, "fps") && argc >= 4) {
			Filmmaker::Render_SetFps((int)ParseNumber(args->ArgV(3), 60));
			return;
		}
		if (0 == _stricmp(op, "range") && argc >= 5) {
			Filmmaker::Render_SetRange((int)ParseNumber(args->ArgV(3), 0),
			                           (int)ParseNumber(args->ArgV(4), 0));
			return;
		}
		if (0 == _stricmp(op, "dir") && argc >= 4) {
            Filmmaker::Render_SetOutputDir(Filmmaker::Utf8ToWide(args->ArgV(3)));
			return;
		}
		if (0 == _stricmp(op, "rangefromshot")) {
			// The shot IS the range you meant, nine times in ten. Explicit rather than automatic, for
			// the same reason the loop and the render range are separate objects: a range you spent
			// time getting right must not be redefined by an unrelated edit.
			if (Filmmaker::Shot_MarkerCount() < 2) {
				advancedfx::Warning("render: the shot needs at least two markers to give a range.\n");
				return;
			}
			Filmmaker::Render_SetRange(Filmmaker::Shot_FirstTick(), Filmmaker::Shot_LastTick());
			return;
		}
		if (0 == _stricmp(op, "start")) { Filmmaker::Render_Start(); return; }
		if (0 == _stricmp(op, "stop")) { Filmmaker::Render_Stop(); return; }
		if (0 == _stricmp(op, "quality") && argc >= 4) {
			Filmmaker::Render_SetQuality((int)ParseNumber(args->ArgV(3), 18));
			return;
		}
		// The option rows. Each name is checked against the values the column offers, so a
		// mistyped console command is an error rather than a job silently left in its old shape.
		if (argc >= 4) {
			if (Filmmaker::Render_SetOption(op, args->ArgV(3))) return;
			if (Filmmaker::Render_SetFlag(op, 0 != (int)ParseNumber(args->ArgV(3), 0))) return;
		}
		if (0 == _stricmp(op, "camsrc") || 0 == _stricmp(op, "hud")
		    || 0 == _stricmp(op, "format")) {
			advancedfx::Message(
				"render camsrc: current | free | path | follow\n"
				"render hud:    none | game | full\n"
				"render format: mp4 | avi | gif | png | tiff | tga | bmp | jpg\n");
			return;
		}
		Filmmaker::Render_Report();
		return;
	}

	if (0 == _stricmp(sub, "speed")) {
		// Deadlock's own replay bar has a speed stepper, but its Panorama entry points are bound
		// per-XML-context and are undefined in the context this hook runs scripts in -- measured in
		// game on build 6687. So the panels' delegation falls through to here, and here drives the
		// engine convar instead.
		//
		// TWO KINDS OF CALLER, and they need different behaviour. The stepper buttons walk a list
		// of eight presets. The timeline's and the graph's sliders are CONTINUOUS, and snapping
		// their value to the nearest preset makes the handle crawl away from where it was dropped:
		// the panel asks for 0.37x, the dial answers 0.25x, and the slider re-seeds from the
		// answer on the next frame. An explicit value is taken as given.
		if (argc >= 3) {
			const char* op = args->ArgV(2);
			if (0 == _stricmp(op, "up")) Deadlock::PlaybackSpeed_Step(1);
			else if (0 == _stricmp(op, "down")) Deadlock::PlaybackSpeed_Step(-1);
			else Deadlock::PlaybackSpeed_Set(std::atof(op));
		}
		advancedfx::Message("playback speed = %.2fx\n", Deadlock::PlaybackSpeed());
		return;
	}

	if (0 == _stricmp(sub, "scrubtick")) {
		// Whole-tick stepping for the timeline's joined step pair. It goes through the scrub verbs
		// rather than seeking directly, so one frame-step lands exactly where a dragged scrub of
		// the same distance would -- the two must not disagree by a tick.
		if (argc < 3) {
			advancedfx::Message("scrubtick <n>  - step the playhead by n ticks\n");
			return;
		}
		const double delta = std::atof(args->ArgV(2));
		Filmmaker::Shot_BeginScrub(Filmmaker::Shot_PlayheadTick() + delta);
		Filmmaker::Shot_EndScrub();
		return;
	}

	if (0 == _stricmp(sub, "skip")) {
		if (argc < 3) {
			advancedfx::Message("skip <seconds>  - relative seek, negative goes back\n");
			return;
		}
		const double seconds = std::atof(args->ArgV(2));
		const int tick = Deadlock::Engine_DemoTick();
		if (tick < 0) {
			advancedfx::Warning("skip: no replay tick available.\n");
			return;
		}
		double interval = Deadlock::Engine_IntervalPerTick();
		if (!(interval > 0.00001)) interval = 1.0 / 64.0;
		SeekToTick((long)tick + (long)(seconds / interval), "skip");
		return;
	}
	if (0 == _stricmp(sub, "livejs")) {
		// Shared with the CS2 hook unchanged. The console cannot carry real JavaScript -- it splits
		// on quotes and commas and truncates around 250 bytes -- so the command carries a PATH and
		// the script is read off disk as bytes. Anything built this way is ordinary Panorama JS and
		// pastes into a *Js.h fragment unchanged once it is right.
		Filmmaker::UiLive_RunCommand(argc, args, "mirv_filmmaker");
		return;
	}

	if (0 == _stricmp(sub, "hud")) {
		// SCALED is the default and the interesting one: the game's own HUD is mapped into the same
		// rect the world renders into, so the spectator bar, the portraits and the map view sit
		// inside the picture instead of floating over it at full screen size.
		bool hidden = Filmmaker::Workspaces_HudHidden();
		if (argc >= 3) {
			const char* op = args->ArgV(2);
			if (0 == _stricmp(op, "hidden") || 0 == _stricmp(op, "hide")) hidden = true;
			else if (0 == _stricmp(op, "scaled") || 0 == _stricmp(op, "show")) hidden = false;
			else if (0 == _stricmp(op, "toggle")) hidden = !hidden;
		}
		Filmmaker::Workspaces_SetHudHidden(hidden);
		advancedfx::Message("game hud = %s\n", hidden ? "hidden" : "scaled into the preview");
		return;
	}

	if (0 == _stricmp(sub, "blackout")) {
		Deadlock::HudBlackout_RunCommand(argc, args, "mirv_filmmaker");
		return;
	}

	if (0 == _stricmp(sub, "dock")) {
		// The panel reports Deadlock's replay bar's real height here, because only Panorama can
		// measure it. Clamped on the far side before it is believed.
		if (argc >= 4 && 0 == _stricmp(args->ArgV(2), "nativeheight")) {
			Filmmaker::Dock_SetMeasuredNativeHeight((int)ParseNumber(args->ArgV(3), 0));
			return;
		}
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "settings")) {
			Filmmaker::Dock_Report();
			return;
		}
		Filmmaker::Dock_RunCommand(argc, args);
		return;
	}

	if (0 == _stricmp(sub, "loop")) {
		Filmmaker::PlaybackLoop_RunCommand(argc, args);
		return;
	}

	if (0 == _stricmp(sub, "preui")) {
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "names")) {
			if (argc >= 4) {
				const bool on = 0 != _stricmp(args->ArgV(3), "off");
				Deadlock::PreUiHook_SetCollectNames(on);
				advancedfx::Message("pre-ui name collection %s\n", on ? "on" : "off");
				return;
			}
			Deadlock::PreUiHook_DumpNames();
			return;
		}
		Deadlock::PreUiHook_Report();
		return;
	}

	// The blit's own stand-down, so "is the scaled preview what is eating this?" can be answered in
	// one command without closing the workspace and changing everything else with it.
	if (0 == _stricmp(sub, "blit")) {
		if (argc >= 3) {
			const bool off = (0 == _stricmp(args->ArgV(2), "off"))
				|| (0 != _stricmp(args->ArgV(2), "on") && ParseNumber(args->ArgV(2), 1) == 0);
			Deadlock::ViewportBlit_SetSuppressed(off);
			advancedfx::Message("blit: %s\n", off ? "suppressed (frame untouched)" : "enabled");
		}
		Deadlock::ViewportBlit_Report();
		return;
	}

	// Off by default (P7 v1: hero boxes only, no path/marker drawing yet -- see
	// DeadlockOverlayDrawer.h's file header for the exact scope). A kill switch that starts OFF
	// rather than needing one to turn off is the right default for a first-pass D3D feature that
	// has not had real soak time yet.
	if (0 == _stricmp(sub, "overlay")) {
		// `status` (or any word that is not on/off/number) only reports. It used to be read as
		// "not on" and silently switch the overlay OFF, so asking how it was doing broke it.
		if (argc >= 3 && 0 != _stricmp(args->ArgV(2), "status")) {
			const char* v = args->ArgV(2);
			const bool isOn = 0 == _stricmp(v, "on"), isOff = 0 == _stricmp(v, "off");
			const bool numeric = (v[0] >= '0' && v[0] <= '9');
			if (isOn || isOff || numeric) {
				const bool on = isOn || (numeric && ParseNumber(v, 0) != 0);
				Deadlock::OverlayDrawer_SetEnabled(on);
				advancedfx::Message("overlay: %s\n", on ? "enabled" : "disabled");
			} else {
				advancedfx::Message("overlay: on | off | status\n");
			}
		}
		Deadlock::OverlayDrawer_Report();
		return;
	}

	if (0 == _stricmp(sub, "preview")) {
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "offsets")) {
			if (argc < 5) {
				ptrdiff_t x = 0, y = 0;
				Deadlock::ViewSetup_GetViewportOriginOffsets(x, y);
				advancedfx::Message("preview viewport origin: x=+0x%zx y=+0x%zx\n",
				                    (size_t)x, (size_t)y);
				return;
			}
			Deadlock::ViewSetup_SetViewportOriginOffsets(
				(ptrdiff_t)ParseNumber(args->ArgV(3), 0x438),
				(ptrdiff_t)ParseNumber(args->ArgV(4), 0x43c));
			advancedfx::Message("preview viewport origin updated; the stand-down is retracted.\n");
			return;
		}
		if (argc >= 3) {
			const bool on = (0 == _stricmp(args->ArgV(2), "on"))
				|| (0 != _stricmp(args->ArgV(2), "off") && ParseNumber(args->ArgV(2), 1) != 0);
			Deadlock::ViewSetup_SetPreviewViewportEnabled(on);
		}
		Deadlock::ViewSetup_PreviewReport();
		return;
	}

	if (0 == _stricmp(sub, "camera")) {
		// The pose the engine actually used this frame, after every override. The one readable
		// number that says whether a camera feature did anything.
		double origin[3], angles[3], fov = 0.0;
		Deadlock::CameraRuntime_GetLastCamera(origin, angles, fov);
		advancedfx::Message(
			"  camera:    pos %.3f %.3f %.3f  ang %.3f %.3f %.3f  fov %.2f\n",
			origin[0], origin[1], origin[2], angles[0], angles[1], angles[2], fov);
		// The same resolver the view hook claims with. Console overrides are a layer applied on top
		// of whichever owner produced the pose, so they get their own line instead of hiding it.
		advancedfx::Message("             owner: %s\n", Deadlock::CameraRuntime_OwnerName());
		if (Deadlock::CameraCommands_Active())
			advancedfx::Message("             layer: console camera override (mirv_fov/camio/campath)\n");
		return;
	}

	if (0 == _stricmp(sub, "listentities")) {
		const int n = (argc >= 3) ? (int)ParseNumber(args->ArgV(2), 0) : 0;
		Deadlock::Entities_ListHeroes(n);
		return;
	}
	if (0 == _stricmp(sub, "entities")) {
		if (argc >= 4 && 0 == _stricmp(args->ArgV(2), "probe")) {
			Deadlock::Entities_Probe((int)ParseNumber(args->ArgV(3), -1));
			return;
		}
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "list")) {
			Deadlock::Entities_List(argc >= 4 ? args->ArgV(3) : "",
			                        argc >= 5 ? (int)ParseNumber(args->ArgV(4), 60) : 60);
			return;
		}
		// The replay's own entity list, as this port reads it. The first thing to run when the
		// follow camera says it is not solving: a hero count of zero and a resolved field table
		// are a different fault from an unresolved list.
		Deadlock::Entities_Report();
		return;
	}

	if (0 == _stricmp(sub, "schema")) {
		if (argc >= 5 && 0 == _stricmp(args->ArgV(2), "layout")) {
			// The repair hatch for a shifted internal ABI. Valve's schema structures have moved
			// before on the CS2 side, and when they do the symptom is a table of plausible garbage
			// rather than an error -- which is why the reader validates, and why this exists.
			Deadlock::Schema_SetLayout((size_t)ParseNumber(args->ArgV(3), 0),
			                           (size_t)ParseNumber(args->ArgV(4), 0));
			advancedfx::Message("schema layout set; reloading.\n");
			Deadlock::Schema_Reload();
			Deadlock::Schema_Report();
			return;
		}
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "reload")) {
			Deadlock::Schema_Reload();
			Deadlock::Schema_Report();
			return;
		}
		if (argc >= 4 && 0 == _stricmp(args->ArgV(2), "raw")) {
			// The bytes behind one class, for when the TABLE itself is what came out wrong. A drifted
			// schema ABI does not error -- it produces a full table of plausible garbage -- so the
			// repair starts from bytes rather than from a guess.
			std::string bytes;
			if (!Deadlock::Schema_DumpClassBytes(args->ArgV(3), bytes)) {
				advancedfx::Warning("schema: no class '%s' in the client scope.\n", args->ArgV(3));
				return;
			}
			advancedfx::Message("schema raw %s:\n%s", args->ArgV(3), bytes.c_str());
			return;
		}
		if (argc >= 4 && 0 == _stricmp(args->ArgV(2), "field")) {
			const char* klass = args->ArgV(3);
			const char* field = (argc >= 5) ? args->ArgV(4) : "";
			const ptrdiff_t off = Deadlock::Schema_FieldOffset(klass, field);
			if (off < 0)
				advancedfx::Warning("schema: %s::%s is not declared on this build.\n", klass, field);
			else
				advancedfx::Message("schema: %s::%s = +0x%zx\n", klass, field, (size_t)off);
			return;
		}
		Deadlock::Schema_Report();
		advancedfx::Message(
			"schema: reload | layout <scopeNamePad> <scopeCountPad> | field <class> <field>\n"
			"        raw <class>  - one CSchemaClassInfo's bytes, to repair a drifted layout\n");
		return;
	}

	if (0 == _stricmp(sub, "dump")) {
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "schema")) {
			if (!Deadlock::Schema_Ready()) {
				advancedfx::Warning("dump: the schema table did not resolve; nothing to write.\n");
				Deadlock::Schema_Report();
				return;
			}
			const char* filter = (argc >= 4) ? args->ArgV(3) : "";
			std::wstring path;
			if (argc >= 5) {
				const char* narrow = args->ArgV(4);
				while (*narrow) path.push_back((wchar_t)(unsigned char)*narrow++);
			} else {
				path = Deadlock::Schema_DefaultDumpPath();
			}
			if (Deadlock::Schema_DumpJson(path.c_str(), filter))
				advancedfx::Message("dump: wrote %d classes to %ls\n",
				                    Deadlock::Schema_ClassCount(), path.c_str());
			else
				advancedfx::Warning("dump: could not write %ls\n", path.c_str());
			return;
		}
		advancedfx::Message("dump: schema [classNameFilter] [path]\n");
		return;
	}
	if (0 == _stricmp(sub, "netcon")) {
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "stop")) {
			Deadlock::Netcon_Stop();
			return;
		}
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "start")) {
			Deadlock::Netcon_Start(argc >= 4 ? (unsigned short)ParseNumber(args->ArgV(3), 0) : 0);
			return;
		}
		const Deadlock::NetconStats ns = Deadlock::Netcon_GetStats();
		advancedfx::Message(
			"netcon: %s port=%u clients=%d commands=%llu lines=%llu dropped=%llu\n",
			ns.active ? "listening" : "off", (unsigned)ns.port, ns.clients,
			ns.commandsExecuted, ns.linesSent, ns.linesDropped);
		return;
	}

	PrintHelp();
}

} // namespace

CON_COMMAND(mirv_deadlock, "SOURCE:MVM filmmaker for Deadlock.") {
	MirvDeadlock_Run(args);
}

CON_COMMAND(mirv_filmmaker, "SOURCE:MVM filmmaker for Deadlock (mirv_deadlock is a compatibility alias).") {
	MirvDeadlock_Run(args);
}

CON_COMMAND(mvm_debug, "Runtime debug log: start | stop | status.") {
	Deadlock::Debug_Command(args);
}

bool Deadlock::Commands_DispatchLine(const char* line) {
	LineArgs args(line);
	if (args.ArgC() < 1) return false;
	const char* name = args.ArgV(0);

	if (0 == _stricmp(name, "mirv_filmmaker") || 0 == _stricmp(name, "mirv_deadlock")) {
		MirvDeadlock_Run(&args);
		return true;
	}
	if (0 == _stricmp(name, "mvm_debug")) {
		Deadlock::Debug_Command(&args);
		return true;
	}

	// Use the same callbacks for native F7, netcon and Panorama buttons. Native registration
	// is handled by the Deadlock ABI adapter; this local route also works during console triage.
	if (advancedfx::CommandCallback_t callback = WrpFindCommandCallback(name)) {
		callback(&args);
		return true;
	}
	return false;
}
