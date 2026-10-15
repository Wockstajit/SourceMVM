// SOURCE:MVM debug tooling commands: the mvm_debug console command (start/stop/status of
// the session-wide compressed diagnostic log) and MvmCommandTrace, the RAII post-command
// state dumper mirv_filmmaker's dispatcher wraps every invocation in. Split out of
// FilmmakerCommand.cpp so that file stays a pure command dispatcher.

#include "FilmmakerDebugCommand.h"

#include "Debug/MvmDebugLog.h"
#include "Panorama/PanoramaPerfProbe.h"
#include "Cosmetics/CosmeticOverrideSystem.h"
#include "Movie/CameraPath.h"
#include "Movie/FollowCamera.h"
#include "Movie/ParticleFx.h"
#include "Panorama/CameraTimelineHud.h"
#include "Panorama/CameraEditorHud.h"
#include "Panorama/GraphEditorExperimentHud.h"
#include "Render/RenderCommand.h"

#include "../WrpConsole.h"
#include "../../Shared/AfxConsole.h"

#include <string>
#include <vector>
#include <cstring>

namespace Filmmaker {

MvmCommandTrace::MvmCommandTrace(advancedfx::ICommandArgs* args)
	: m_sub(args && args->ArgC() >= 2 ? args->ArgV(1) : "") {
	Filmmaker::MvmDebugLog_Command(args);
}

MvmCommandTrace::~MvmCommandTrace() {
	if (!Filmmaker::MvmDebugLog_Active())
		return;
	if (0 == _stricmp(m_sub.c_str(), "follow")) {
		const Filmmaker::FollowCamera& follow = Filmmaker::FollowCameraRef();
		const Filmmaker::FollowCameraState& state = follow.State();
		std::string targetName;
		std::string targetStatus;
		for (const Filmmaker::FollowTargetCandidate& candidate : follow.Candidates()) {
			if ((state.targetHandle && candidate.handle == state.targetHandle)
				|| (!state.targetHandle && candidate.entityIndex == state.targetEntityIndex)) {
				targetName = candidate.name;
				targetStatus = candidate.status;
				break;
			}
		}
		Filmmaker::MvmDebugLog_Linef("state.follow",
			"enabled=%d hasCamera=%d repositioning=%d type=%d mode=%d weaponSource=%d "
			"targetIndex=%d targetHandle=%llu targetName='%s' targetStatus='%s' "
			"offset=%.3f,%.3f,%.3f rotation=%.3f,%.3f,%.3f fov=%.3f "
			"look=%.3f position=%.3f prediction=%.3f deadzone=%.3f maxTurn=%.3f "
			"autoDead=%d switchWeapon=%d switchBomb=%d hold=%d attachment='%s' "
			"renderSample=%d message='%s'",
			state.enabled ? 1 : 0, state.hasCamera ? 1 : 0, follow.Repositioning() ? 1 : 0,
			(int)state.targetType, (int)state.mode, (int)state.weaponSource,
			state.targetEntityIndex, (unsigned long long)state.targetHandle,
			targetName.c_str(), targetStatus.c_str(),
			state.offset.x, state.offset.y, state.offset.z,
			state.rotationOffset.pitch, state.rotationOffset.yaw, state.rotationOffset.roll,
			state.fov, state.lookSmoothing, state.positionSmoothing, state.prediction,
			state.deadzone, state.maxTurnSpeed, state.autoDisableOnDeath ? 1 : 0,
			state.switchToDroppedWeaponOnDeath ? 1 : 0,
			state.switchToDroppedBombOnDeath ? 1 : 0,
			state.holdLastKnownPosition ? 1 : 0, state.attachmentName.c_str(),
			follow.RenderTimeSample() ? 1 : 0, follow.LastMessage().c_str());
	} else if (0 == _stricmp(m_sub.c_str(), "marker")
		|| 0 == _stricmp(m_sub.c_str(), "camtl")
		|| 0 == _stricmp(m_sub.c_str(), "editor")) {
		const Filmmaker::CameraPath& path = Filmmaker::CameraPathRef();
		const Filmmaker::CameraEditorHud& editor = Filmmaker::CameraEditorHudRef();
		const std::vector<Filmmaker::CamMarker>& markers = path.Markers();
		const int selected = path.Selected();
		const Filmmaker::CamMarker* marker = selected >= 0 && selected < (int)markers.size()
			? &markers[(size_t)selected] : nullptr;
		Filmmaker::MvmDebugLog_Linef("state.camera",
			"editor=%d scale=%d bottom=%d hud=%d timeline=%d cursor=%d "
			"count=%d selected=%d mode=%s playing=%d pending=%d scrubbing=%d scrubTick=%.3f "
			"speedMode=%s timing=%s curve=%s constSpeed=%.3f autoSnap=%d "
			"keyTick=%d pos=%.3f,%.3f,%.3f angles=%.3f,%.3f,%.3f fov=%.3f "
			"keySpeed=%.3f ease=%d notice='%s'",
			editor.Enabled() ? 1 : 0, editor.ScaleEnabled() ? 1 : 0,
			(int)editor.GetBottomMode(), (int)editor.GetHudView(),
			Filmmaker::CameraTimelineHudRef().Visible() ? 1 : 0,
			Filmmaker::CameraTimelineHudRef().Cursor() ? 1 : 0,
			path.Count(), selected, path.ModeName(), path.IsPlaying() ? 1 : 0,
			path.PlaybackPending() ? 1 : 0, path.IsScrubbing() ? 1 : 0, path.ScrubTick(),
			path.SpeedModeName(), path.TimingName(), path.InterpName(), path.ConstSpeed(),
			path.AutoSnap() ? 1 : 0,
			marker ? marker->tick : -1,
			marker ? marker->x : 0.0, marker ? marker->y : 0.0, marker ? marker->z : 0.0,
			marker ? marker->pitch : 0.0, marker ? marker->yaw : 0.0, marker ? marker->roll : 0.0,
			marker ? marker->fov : 0.0, marker ? marker->speedMul : 0.0f,
			marker ? (int)marker->ease : -1, path.Notice());
	} else if (0 == _stricmp(m_sub.c_str(), "fx")) {
		Filmmaker::MvmDebugLog_Linef("state.fx", "%s", Filmmaker::ParticleFxRef().DebugStateJson().c_str());
	} else if (0 == _stricmp(m_sub.c_str(), "grapheditor")) {
		// Full model + gate dump: the graph now bakes back into the camera path, so three
		// booleans could not answer "did the edit land, and on what". See DebugStateLine().
		Filmmaker::MvmDebugLog_Linef("state.graph", "%s",
			Filmmaker::GraphEditorExperimentHudRef().DebugStateLine().c_str());
	} else if (0 == _stricmp(m_sub.c_str(), "render")) {
		Filmmaker::MvmDebugLog_Linef("state.render", "%s", Filmmaker::Render_DebugStateLine().c_str());
	} else if (0 == _stricmp(m_sub.c_str(), "cosmetics")) {
		const Filmmaker::CosmeticOverrideSystem& cosmetics = Filmmaker::CosmeticsRef();
		const Filmmaker::CosmeticFrameStats& stats = cosmetics.LastFrameStats();
		Filmmaker::MvmDebugLog_Linef("state.cosmetics",
			"enabled=%d armed=%d profiles=%zu inDemo=%d offsets=%d targetSteamId=%llu "
			"matched=%d patched=%d changed=%d knife=%d gloves=%d agents=%d",
			cosmetics.Enabled() ? 1 : 0, cosmetics.Armed() ? 1 : 0,
			cosmetics.Store().All().size(), cosmetics.InDemoContext() ? 1 : 0,
			cosmetics.OffsetsAvailable() ? 1 : 0,
			(unsigned long long)cosmetics.CurrentSpectatedSteamId(),
			stats.entitiesMatched, stats.entitiesPatched, stats.attrValuesChanged,
			stats.knifeModelsApplied, stats.glovesApplied, stats.agentsApplied);
	}
}

} // namespace Filmmaker

// Session-wide, compressed SOURCE:MVM diagnostic log.
CON_COMMAND(mvm_debug, "Control the SOURCE:MVM debug log (mvm_debug start|stop|status|perf).") {
	const int argc = args->ArgC();
	const char* mode = (argc >= 2) ? args->ArgV(1) : "";
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Command(args);
	if (0 == _stricmp(mode, "start")) {
		std::string file;
		if (Filmmaker::MvmDebugLog_Start(&file)) {
			const bool consoleCapture = Filmmaker::MvmConsoleCapture_Start();
			Filmmaker::MvmDebugLog_Linef("command", "mvm_debug start");
			Filmmaker::MvmDebugLog_Linef("system", "fullConsoleCapture=%d", consoleCapture ? 1 : 0);
			// The main-thread UI cost probe rides the log: "the editor halves my FPS" is a
			// per-frame measurement and there is nowhere else for it to land.
			Filmmaker::PanoramaPerf_SetEnabled(true);
			advancedfx::Message("mvm_debug: STARTED. file: %s (type 'mvm_debug stop' when done).\n", file.c_str());
		} else
			advancedfx::Warning("mvm_debug: already running (or file could not be opened).\n");
	} else if (0 == _stricmp(mode, "stop")) {
		std::string folder, file;
		Filmmaker::MvmDebugLogStats stats;
		Filmmaker::MvmConsoleCapture_Stop();
		Filmmaker::PanoramaPerf_SetEnabled(false);
		if (Filmmaker::MvmDebugLog_Stop(&folder, &file, &stats)) {
			advancedfx::Message("mvm_debug: STOPPED. captured %llu events; combined %llu repeats.\n",
				(unsigned long long)stats.eventsReceived,
				(unsigned long long)stats.repeatsCombined);
			advancedfx::Message("  file:   %s\n", file.c_str());
			advancedfx::Message("  folder: %s\n", folder.c_str());
		} else {
			advancedfx::Warning("mvm_debug: no debug log is running.\n");
		}
	} else if (0 == _stricmp(mode, "perf")) {
		// Arm the main-thread UI cost probe WITHOUT opening a log, and read the last window back
		// over netcon. A log is the better record -- it keeps every window -- but for a quick A/B,
		// where the only question is "did that toggle move the number", this is the whole loop.
		const char* onoff = (argc >= 3) ? args->ArgV(2) : "";
		if (0 == _stricmp(onoff, "on")) Filmmaker::PanoramaPerf_SetEnabled(true);
		else if (0 == _stricmp(onoff, "off")) Filmmaker::PanoramaPerf_SetEnabled(false);
		else if (onoff[0]) {
			advancedfx::Warning("mvm_debug perf: expected on|off.\n");
			return;
		}
		advancedfx::Message("mvm_debug perf: %s\n  last: %s\n",
			Filmmaker::PanoramaPerf_Enabled() ? "ARMED" : "off",
			Filmmaker::PanoramaPerf_LastReport());
	} else if (0 == _stricmp(mode, "status") || mode[0] == '\0') {
		const Filmmaker::MvmDebugLogStats stats = Filmmaker::MvmDebugLog_GetStats();
		advancedfx::Message("mvm_debug: %s. events=%llu unique=%llu repeats_combined=%llu. "
			"Usage: mvm_debug start|stop|status\n",
			Filmmaker::MvmDebugLog_Active() ? "RUNNING" : "stopped",
			(unsigned long long)stats.eventsReceived,
			(unsigned long long)stats.uniqueEventsWritten,
			(unsigned long long)stats.repeatsCombined);
	} else {
		advancedfx::Warning("mvm_debug: unknown action '%s'. Usage: mvm_debug start|stop|status|perf on|off\n", mode);
	}
}
