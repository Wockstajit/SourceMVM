#include "RenderQueue.h"

#include "RenderCommand.h"
#include "RenderOutput.h"
#include "RenderPassPlan.h"
#include "RenderRange.h"
#include "RenderSessionDriver.h"
#include "RenderTypes.h"
#include "../Cosmetics/CosmeticEntityAccess.h"
#include "../Cosmetics/CosmeticOverrideSystem.h" // Cosmetics_SpectateSteamId: the POV entry's player
#include "../Debug/MvmDebugLog.h"
#include "../Movie/CameraBridge.h"
#include "../Movie/CameraPath.h"
#include "../Platform/JsonBuilder.h"
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "../../../Shared/AfxConsole.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

struct Entry {
	RenderRange range;
	RenderCameraSource camera = RenderCameraSource::CurrentViewer;
	CameraPathExtension extension = CameraPathExtension::Fail;
	RenderOutputSettings out;
	RenderPasses passes;
	uint64_t povSteam = 0;     // the watched player (POV, third person, follow, current viewer), 0 = none
	std::string povName;
	std::vector<CamMarker> path;
	bool stillPose = false;    // a Free Camera shot: `path` holds its pose at the shot's start and end
	std::string status = "queued"; // queued | rendering | completed | failed | canceled | skipped
	std::string note;
};

// What the queue changes, put back when it ends.
struct Saved {
	bool valid = false;
	RenderRange range;
	RenderCameraSource camera = RenderCameraSource::CurrentViewer;
	CameraPathExtension extension = CameraPathExtension::Fail;
	RenderOutputSettings out;
	RenderPasses passes;
	std::vector<CamMarker> path;
	bool pathTouched = false;
	int tick = -1;          // the playhead the queue started from
	uint64_t watched = 0;   // the player being watched then, 0 = none
};

enum class Q { Idle, Seek, Spectate, Start, Rendering, Between };

std::vector<Entry> g_entries;
Saved g_saved;
Q g_state = Q::Idle;
// After the queue: watch the saved player again once the seek back to the saved tick has settled.
uint64_t g_rewatch = 0;
int g_rewatchFrames = 0;
int g_current = -1;
int g_frames = 0;
bool g_stopRequested = false;

const char* CameraName(RenderCameraSource s) {
	switch (s) {
	case RenderCameraSource::CurrentViewer: return "current";
	case RenderCameraSource::Pov: return "pov";
	case RenderCameraSource::ThirdPerson: return "thirdperson";
	case RenderCameraSource::FreeCamera: return "free";
	case RenderCameraSource::CameraPath: return "path";
	case RenderCameraSource::FollowCamera: return "follow";
	}
	return "?";
}

void Exec(const char* c) {
	if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, c, true);
}

// The watched player's SteamID and name, from the pawn on screen. SEH-guarded: a stale entity is nobody.
bool SpectatedPlayer(uint64_t& steam, char name[64]) {
	steam = 0; name[0] = 0;
	__try {
		const int pawnIndex = AfxGetSpectatedPawnIndexHeld();
		CEntityInstance* pawn = pawnIndex >= 0 ? CosmeticEntFromIndex(pawnIndex) : nullptr;
		if (!pawn) return false;
		const auto handle = pawn->GetPlayerControllerHandle();
		CEntityInstance* controller = handle.IsValid() ? CosmeticEntFromIndex(handle.GetEntryIndex()) : nullptr;
		if (!controller || !controller->IsPlayerController()) return false;
		steam = controller->GetSteamId();
		const char* n = controller->GetSanitizedPlayerName();
		if (n) strncpy_s(name, 64, n, _TRUNCATE);
		return steam != 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		steam = 0; name[0] = 0;
		return false;
	}
}

std::string Describe(const Entry& e, int index) {
	char buf[256];
	std::string cam = CameraName(e.camera);
	if (!e.povName.empty()) cam += " " + e.povName;
	if (e.camera == RenderCameraSource::CameraPath) cam += " (" + std::to_string(e.path.size()) + " keys)";
	std::snprintf(buf, sizeof(buf), "#%d %s, ticks %d..%d, %d fps %s, %d pass(es) [%s]%s%s", index + 1, cam.c_str(),
		e.range.startTick, e.range.endTickExclusive - 1, e.out.fps, RenderFormatToken(e.out.format),
		(int)BuildPassPlan(e.passes.passes).passes.size(), e.status.c_str(), e.note.empty() ? "" : ": ", e.note.c_str());
	return buf;
}

bool AddCurrent(std::string& error) {
	RenderRange resolved;
	if (!Render_ResolveRangeForDisplay(resolved) || !resolved.Valid()) {
		error = "the render range does not resolve to a usable tick span";
		return false;
	}
	Entry e;
	e.range = resolved;
	e.range.mode = RenderRangeMode::CustomTicks; // the ticks it resolved to now, whatever moves later
	e.camera = Render_GetCameraSource();
	e.extension = Render_GetExtension();
	e.out = RenderOutputSettingsRef();
	e.passes = RenderPassesRef();
	e.passes.passes = NormalizePasses(e.passes.passes);
	if (e.passes.passes.empty()) { error = "no render pass selected"; return false; }
	// Each shot keeps what its camera follows, so a later shot changing the watched player or the
	// view does not change it: the watched player for the player cameras, the pose for Free Camera.
	if (e.camera == RenderCameraSource::FreeCamera) {
		double origin[3] = {}, angles[3] = {}, fov = 90;
		CameraBridge_GetCurrentCamera(origin, angles, fov);
		const double interval = (double)g_MirvTime.interval_per_tick_get();
		for (int tick : { e.range.startTick, (std::max)(e.range.startTick + 1, e.range.endTickExclusive - 1) }) {
			CamMarker m;
			m.x = origin[0]; m.y = origin[1]; m.z = origin[2];
			m.pitch = angles[0]; m.yaw = angles[1]; m.roll = angles[2]; m.fov = fov;
			m.tick = tick;
			m.time = tick * interval;
			e.path.push_back(m);
		}
		e.stillPose = true;
	} else if (e.camera != RenderCameraSource::CameraPath) {
		char name[64];
		if (SpectatedPlayer(e.povSteam, name)) e.povName = name;
		else if (e.camera == RenderCameraSource::Pov) { error = "a POV shot needs a watched player: spectate one first"; return false; }
	}
	if (e.camera == RenderCameraSource::CameraPath) {
		e.path = CameraPathRef().Markers();
		if (e.path.size() < 2) { error = "a Camera Path shot needs 2+ camera keys"; return false; }
	}
	g_entries.push_back(e);
	return true;
}

void Restore() {
	if (!g_saved.valid) return;
	RenderOutputSettingsRef() = g_saved.out;
	RenderPassesRef() = g_saved.passes;
	Render_SetExtension(g_saved.extension);
	Render_SetCameraSource(g_saved.camera);
	Render_SetRange(g_saved.range);
	if (g_saved.pathTouched) CameraPathRef().ReplaceMarkersFromGraph(g_saved.path, false);
	// Each shot switched the live camera, so the view itself goes back to the saved source too.
	Render_ApplyCameraSourceLive(g_saved.camera, "mirv_filmmaker render queue");
	// And back to where the queue started, watching whom it was watching.
	if (g_saved.tick >= 0) {
		char c[64];
		std::snprintf(c, sizeof(c), "demo_gototick %d", g_saved.tick);
		Exec(c);
	}
	g_rewatch = g_saved.watched;
	g_rewatchFrames = 0;
	g_saved = Saved{};
}

void Finish(const char* why) {
	Restore();
	int done = 0, failed = 0;
	for (const Entry& e : g_entries) { if (e.status == "completed") ++done; else if (e.status == "failed") ++failed; }
	advancedfx::Message("mirv_filmmaker render queue: %s (%d completed, %d failed of %d).\n", why, done, failed, (int)g_entries.size());
	MvmDebugLog_LinefAlways("render.queue", "end: %s completed=%d failed=%d total=%d", why, done, failed, (int)g_entries.size());
	g_state = Q::Idle;
	g_current = -1;
	g_stopRequested = false;
}

// Puts entry i's settings into the workspace and lines up its camera.
void Apply(Entry& e) {
	RenderOutputSettingsRef() = e.out;
	RenderPassesRef() = e.passes;
	Render_SetExtension(e.extension);
	// A Free Camera shot renders as a camera held at the pose it was queued with.
	const RenderCameraSource source = e.stillPose ? RenderCameraSource::CameraPath : e.camera;
	if (source == RenderCameraSource::CameraPath) {
		CameraPathRef().ReplaceMarkersFromGraph(e.path, false);
		g_saved.pathTouched = true;
	}
	Render_SetCameraSource(source);
	Render_ApplyCameraSourceLive(source, "mirv_filmmaker render queue");
	Render_SetRange(e.range);
}

bool NextQueued() {
	for (int i = g_current + 1; i < (int)g_entries.size(); ++i)
		if (g_entries[i].status == "queued") { g_current = i; return true; }
	return false;
}

void Begin() {
	g_saved.valid = true;
	g_saved.range = Render_GetRange();
	g_saved.camera = Render_GetCameraSource();
	g_saved.extension = Render_GetExtension();
	g_saved.out = RenderOutputSettingsRef();
	g_saved.passes = RenderPassesRef();
	g_saved.path = CameraPathRef().Markers();
	g_saved.pathTouched = false;
	{
		int tick = -1;
		g_saved.tick = g_MirvTime.GetCurrentDemoTick(tick) ? tick : -1;
		char name[64];
		if (!SpectatedPlayer(g_saved.watched, name)) g_saved.watched = 0;
	}
	for (Entry& e : g_entries) if (e.status != "completed") { e.status = "queued"; e.note.clear(); }
	g_current = -1;
	g_stopRequested = false;
	g_state = Q::Between;
	g_frames = 0;
	MvmDebugLog_LinefAlways("render.queue", "start: %d entries", (int)g_entries.size());
}

} // namespace

bool RenderQueue_Running() { return g_state != Q::Idle; }

void RenderQueue_RunFrame() {
	if (g_state == Q::Idle) {
		if (g_rewatch && ++g_rewatchFrames >= 30) {
			Cosmetics_SpectateSteamId("mirv_filmmaker render queue", g_rewatch);
			g_rewatch = 0;
		}
		return;
	}
	++g_frames;
	switch (g_state) {
	case Q::Between:
		// A breath between jobs: the last one's restore has run and the demo is back.
		if (g_frames < 15) return;
		if (g_stopRequested) { Finish("stopped"); return; }
		if (!Render_IsModalOpen()) { Finish("stopped: the Render workspace was closed"); return; }
		if (!NextQueued()) { Finish("done"); return; }
		Apply(g_entries[g_current]);
		g_entries[g_current].status = "rendering";
		// A shot that follows a player first goes to its start, where the player is alive, then
		// watches them.
		if (g_entries[g_current].povSteam != 0) {
			char c[64];
			std::snprintf(c, sizeof(c), "demo_gototick %d", g_entries[g_current].range.startTick);
			Exec(c);
			g_state = Q::Seek;
		} else {
			g_state = Q::Start;
		}
		g_frames = 0;
		MvmDebugLog_LinefAlways("render.queue", "apply %s", Describe(g_entries[g_current], g_current).c_str());
		return;
	case Q::Seek:
		if (g_frames < 30) return;
		{
			Entry& e = g_entries[g_current];
			if (!Cosmetics_SpectateSteamId("mirv_filmmaker render queue", e.povSteam)) {
				e.status = "skipped";
				e.note = "could not watch " + e.povName + " at the shot's start";
				g_state = Q::Between; g_frames = 0;
				return;
			}
		}
		g_state = Q::Spectate; g_frames = 0;
		return;
	case Q::Spectate:
		if (g_frames < 10) return;
		g_state = Q::Start; g_frames = 0;
		return;
	case Q::Start: {
		Entry& e = g_entries[g_current];
		std::string err;
		if (!RenderSessionDriver_Start(err)) {
			e.status = "failed";
			e.note = err;
			MvmDebugLog_LinefAlways("render.queue", "start failed #%d: %s", g_current + 1, err.c_str());
			g_state = Q::Between; g_frames = 0;
			return;
		}
		g_state = Q::Rendering; g_frames = 0;
		return;
	}
	case Q::Rendering: {
		if (RenderSessionDriver_IsActive()) return;
		Entry& e = g_entries[g_current];
		const RenderProgress& p = RenderSessionDriver_Progress();
		if (p.phase == "completed") e.status = "completed";
		else if (p.phase == "canceled") { e.status = "canceled"; g_stopRequested = true; }
		else { e.status = "failed"; e.note = p.lastError; }
		MvmDebugLog_LinefAlways("render.queue", "ended #%d: %s %s take=%s", g_current + 1, e.status.c_str(), e.note.c_str(), p.takeDirUtf8.c_str());
		g_state = Q::Between; g_frames = 0;
		return;
	}
	case Q::Idle:
		return;
	}
}

void RenderQueue_RunCommand(int argc, const char* const* argv, const char* cmd) {
	const char* sub = argc > 0 ? argv[0] : "";
	auto list = [&]() {
		if (g_entries.empty()) { advancedfx::Message("%s render queue: empty.\n", cmd); return; }
		for (size_t i = 0; i < g_entries.size(); ++i) advancedfx::Message("  %s\n", Describe(g_entries[i], (int)i).c_str());
	};
	if (!*sub || 0 == _stricmp(sub, "list")) {
		advancedfx::Message("%s render queue: %d shot(s)%s.\n", cmd, (int)g_entries.size(), RenderQueue_Running() ? ", rendering" : "");
		list();
		if (!*sub)
			advancedfx::Message(
				"%s render queue add            - add the current shot (range, camera, output, passes)\n"
				"%s render queue remove <n> | clear\n"
				"%s render queue start | stop   - render every queued shot in turn (stop ends after the current one)\n", cmd, cmd, cmd);
		return;
	}
	if (0 == _stricmp(sub, "add")) {
		if (RenderQueue_Running()) { advancedfx::Warning("%s render queue: wait for the queue to end.\n", cmd); return; }
		std::string err;
		if (!AddCurrent(err)) { advancedfx::Warning("%s render queue: cannot add: %s.\n", cmd, err.c_str()); return; }
		advancedfx::Message("%s render queue: added %s.\n", cmd, Describe(g_entries.back(), (int)g_entries.size() - 1).c_str());
		return;
	}
	if (0 == _stricmp(sub, "remove")) {
		if (RenderQueue_Running()) { advancedfx::Warning("%s render queue: wait for the queue to end.\n", cmd); return; }
		const int n = argc > 1 ? atoi(argv[1]) : 0;
		if (n < 1 || n > (int)g_entries.size()) { advancedfx::Warning("%s render queue remove <1..%d>\n", cmd, (int)g_entries.size()); return; }
		g_entries.erase(g_entries.begin() + (n - 1));
		list();
		return;
	}
	if (0 == _stricmp(sub, "clear")) {
		if (RenderQueue_Running()) { advancedfx::Warning("%s render queue: stop it first.\n", cmd); return; }
		g_entries.clear();
		advancedfx::Message("%s render queue: cleared.\n", cmd);
		return;
	}
	if (0 == _stricmp(sub, "start")) {
		if (RenderQueue_Running()) { advancedfx::Warning("%s render queue: already rendering.\n", cmd); return; }
		if (Render_IsJobActive() || RenderSessionDriver_IsActive()) { advancedfx::Warning("%s render queue: a render job is running.\n", cmd); return; }
		if (!Render_IsModalOpen()) { advancedfx::Warning("%s render queue: open the Render workspace first.\n", cmd); return; }
		bool any = false;
		for (const Entry& e : g_entries) any = any || e.status != "completed";
		if (!any) { advancedfx::Warning("%s render queue: nothing to render (add shots with \"render queue add\").\n", cmd); return; }
		Begin();
		advancedfx::Message("%s render queue: rendering %d shot(s).\n", cmd, (int)g_entries.size());
		return;
	}
	if (0 == _stricmp(sub, "stop")) {
		if (!RenderQueue_Running()) { advancedfx::Message("%s render queue: not running.\n", cmd); return; }
		g_stopRequested = true;
		advancedfx::Message("%s render queue: stops after the current shot.\n", cmd);
		return;
	}
	advancedfx::Warning("%s render queue: unknown '%s' (add|remove|clear|start|stop|list).\n", cmd, sub);
}

void RenderQueue_AppendStateJson(std::ostringstream& o) {
	o << ",\"queue\":{\"running\":" << (RenderQueue_Running() ? "true" : "false")
		<< ",\"current\":" << g_current << ",\"items\":[";
	for (size_t i = 0; i < g_entries.size(); ++i) {
		const Entry& e = g_entries[i];
		o << (i ? "," : "") << "{\"camera\":\"" << CameraName(e.camera) << "\""
			<< ",\"player\":\"" << JsonBuilder::Escape(e.povName) << "\""
			<< ",\"keys\":" << e.path.size()
			<< ",\"start\":" << e.range.startTick << ",\"end\":" << e.range.endTickExclusive
			<< ",\"fps\":" << e.out.fps << ",\"format\":\"" << RenderFormatToken(e.out.format) << "\""
			<< ",\"passes\":" << BuildPassPlan(e.passes.passes).passes.size()
			<< ",\"status\":\"" << e.status << "\",\"note\":\"" << JsonBuilder::Escape(e.note) << "\"}";
	}
	o << "]}";
}

} // namespace Filmmaker
