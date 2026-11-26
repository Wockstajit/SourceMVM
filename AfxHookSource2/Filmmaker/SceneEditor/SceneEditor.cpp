#define NOMINMAX
#include "SceneEditor.h"
#include "SceneEditorMath.h"

#include "../Panorama/ConfigHud.h"
#include "../MvmTest.h"
#include "../Environment/EnvironmentScene.h"
#include "../Environment/EnvironmentGizmoMath.h"
#include "../Environment/MapCollisionGrid.h"
#include "../Movie/CameraBridge.h"
#include "../Movie/HudBlackout.h"
#include "../Panorama/PanoramaBridge.h"
#include "../Platform/JsonBuilder.h"
#include "../Props/PropEditMath.h"
#include "../Props/Props.h"
#include "../Takes/ActorAnimations.h"
#include "../Takes/Takes.h"
#include "../../SceneOverlayData.h"
#include "../../RenderSystemDX11Hooks.h"
#include "../../WrpConsole.h"
#include "../../../Shared/AfxConsole.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

extern SOURCESDK::CS2::ISource2EngineToClient *g_pEngineToClient;

namespace Filmmaker::SceneEditor {

namespace {

enum class SelKind { None, Track, Prop };
enum class GizmoMode { Move, Rotate };
enum class ViewMode { Timeline, Graph };

struct State {
	bool enabled = false;
	bool cursor = true;
	bool flyCam = false;
	bool forcedThirdPerson = false;
	int armFreeCamFrames = 0;
	SelKind selKind = SelKind::None;
	int selTrack = -1;
	int selPropId = 0;
	GizmoMode gizmoMode = GizmoMode::Move;
	ViewMode viewMode = ViewMode::Timeline;
	GraphChannel channel = GraphChannel::HeightZ;
	bool scrubActive = true;
	int scrubTick = 0;

	bool dragging = false;
	bool dragTurn = false;
	int dragTrack = -1;
	bool catalogRequested = false;
	Environment::GizmoDrag gizmoDrag;
	float dragBeforePos[3] = {0.f, 0.f, 0.f};
	float dragBeforeYaw = 0.f;
	float dragTurnStartAngle = 0.f;

	std::string statusMsg;
} g;

void Exec(const char *cmd) {
	if (g_pEngineToClient && cmd && *cmd) g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
}

void Execf(const char *fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Exec(buf);
}

void ClearDragState(bool restore) {
	if (g.dragging && restore && g.dragTrack >= 0) {
		Takes::SetTrackOffset(g.dragTrack, g.dragBeforePos, g.dragBeforeYaw, true);
	}
	g.dragging = false;
	g.dragTurn = false;
	g.dragTrack = -1;
	g.gizmoDrag.Cancel();
}

void PublishOverlay(const Takes::Scene &scene, int effectiveTick) {
	if (!g.enabled || HudBlackout_Active()) {
		CameraBridge_SetSceneOverlay(nullptr);
		return;
	}
	if (scene.tracks.empty()) {
		CameraBridge_SetSceneOverlay(nullptr);
		return;
	}

	auto overlay = std::make_shared<SceneOverlay>();
	const ActorAnim::Library &lib = ActorAnim::GetLibrary();
	const float rate = scene.tickRate > 0.f ? scene.tickRate : 64.f;

	for (size_t i = 0; i < scene.tracks.size(); ++i) {
		const Takes::Track &t = scene.tracks[i];
		if (!t.enabled || t.ticks.empty()) continue;

		const TrackVisualPath vp = BuildTrackVisualPath(t, rate, &lib, effectiveTick, 160);
		if (!vp.valid) continue;

		const bool isSel = (g.selKind == SelKind::Track && g.selTrack == int(i));
		SceneOverlayColour col{};
		if (t.placedActor) col = {110, 235, 175, 255};
		else if (t.team == 2) col = {255, 176, 68, 255};
		else col = {96, 200, 255, 255};
		if (isSel) col.a = 248;
		else col.a = 175;

		SceneOverlay::Path mainPath;
		mainPath.colour = col;
		mainPath.selected = isSel;

		// Measure horizontal span so a stationary clip still renders a clean floor marker ring + heading.
		const float spanXY = std::hypot(vp.endPos[0] - vp.startPos[0], vp.endPos[1] - vp.startPos[1]);
		if (spanXY < 6.0f && !vp.hasAirborneArc) {
			constexpr int kRing = 16;
			constexpr float kRad = 18.0f;
			for (int k = 0; k <= kRing; ++k) {
				const float a = float(k) * (6.2831853f / float(kRing));
				mainPath.points.push_back({vp.startPos[0] + std::cos(a) * kRad,
				                           vp.startPos[1] + std::sin(a) * kRad,
				                           vp.startPos[2] + 2.5f});
			}
			const float yr = vp.startYaw * 0.017453292519943295f;
			mainPath.points.push_back({vp.startPos[0] + std::cos(yr) * (kRad + 14.f),
			                           vp.startPos[1] + std::sin(yr) * (kRad + 14.f),
			                           vp.startPos[2] + 2.5f});
		} else {
			mainPath.points.reserve(vp.points.size());
			for (const auto &pt : vp.points)
				mainPath.points.push_back({pt.pos[0], pt.pos[1], pt.pos[2] + 2.5f});
		}

		mainPath.keys.push_back({vp.startPos[0], vp.startPos[1], vp.startPos[2] + 2.5f});
		for (const auto &cm : vp.cueMarkers)
			mainPath.keys.push_back({cm[0], cm[1], cm[2] + 2.5f});
		if (spanXY >= 6.0f || vp.hasAirborneArc)
			mainPath.keys.push_back({vp.endPos[0], vp.endPos[1], vp.endPos[2] + 2.5f});
		mainPath.keys.push_back({vp.scrubPos[0], vp.scrubPos[1], vp.scrubPos[2] + 2.5f});
		overlay->paths.push_back(std::move(mainPath));

		// Floor projection under jump/ledge-drop arcs so landing alignment on props is unambiguous.
		if (vp.hasAirborneArc && vp.groundPoints.size() >= 2) {
			SceneOverlay::Path groundPath;
			groundPath.colour = {uint8_t(col.r / 2), uint8_t(col.g / 2), uint8_t(col.b / 2), 145};
			groundPath.selected = false;
			groundPath.points.reserve(vp.groundPoints.size());
			for (const auto &gp : vp.groundPoints)
				groundPath.points.push_back({gp.pos[0], gp.pos[1], gp.pos[2] + 1.8f});
			groundPath.keys.push_back({vp.groundPoints.front().pos[0], vp.groundPoints.front().pos[1], vp.groundPoints.front().pos[2] + 1.8f});
			groundPath.keys.push_back({vp.groundPoints.back().pos[0], vp.groundPoints.back().pos[1], vp.groundPoints.back().pos[2] + 1.8f});
			overlay->paths.push_back(std::move(groundPath));
		}
	}

	CameraBridge_SetSceneOverlay(overlay->Empty() ? nullptr : std::move(overlay));
}

bool SnapTrackToFloor(int idx) {
	Takes::Scene sc;
	int tick = 0; bool playing = false, rec = false, loop = false; std::string phase;
	if (!Takes::SnapshotScene(sc, tick, playing, rec, loop, phase)) return false;
	if (idx < 0 || idx >= int(sc.tracks.size())) return false;
	const Takes::Track &t = sc.tracks[idx];
	if (t.ticks.empty()) return false;
	float pos[3], vel[3], yaw = 0.f;
	Takes::TransformTrackSample(t, t.ticks[0], pos, vel, yaw);
	Environment::MapCollisionGrid_Pump();
	Environment::SurfaceHit hit;
	if (!Environment::MapCollisionRaycast({pos[0], pos[1], pos[2] + 40.f}, {0.f, 0.f, -1.f}, 1024.f, hit))
		return false;
	float nextOff[3] = {t.offsetPos[0], t.offsetPos[1], t.offsetPos[2] + (hit.position.z + 1.0f - pos[2])};
	return Takes::SetTrackOffset(idx, nextOff, t.offsetYaw, true);
}

const char *ChannelName(GraphChannel ch) {
	switch (ch) {
	case GraphChannel::HeightZ: return "z";
	case GraphChannel::Speed: return "speed";
	case GraphChannel::Yaw: return "yaw";
	case GraphChannel::AnimWeight: return "anim";
	}
	return "z";
}

} // namespace

bool Enabled() { return g.enabled; }

void SetEnabled(bool enabled) {
	if (enabled == g.enabled) return;
	if (enabled) {
		if (!Takes::Available()) {
			g.statusMsg = "Scene Editor works only inside a private match";
			advancedfx::Warning("sceneeditor: start a private match first\n");
			return;
		}
		if (ConfigHud_Enabled()) ConfigHudRef().SetEnabled(false);
		MvmTest_EnsureArmed();
		g.enabled = true;
		g.cursor = true;
		g.statusMsg.clear();
		// Put local player in centered third-person and arm FreeCam 2 frames later so you see
		// yourself immediately in the preview window and can RMB+WASD fly around right away.
		Exec("mirv_cvar_unlock_sv_cheats");
		Exec("sv_cheats 1");
		Exec("thirdperson");
		Exec("cam_idealyaw 0");
		Exec("c_thirdpersonshoulder 0");
		g.forcedThirdPerson = true;
		g.armFreeCamFrames = 2;
		Takes::EnsureMapSceneAndBotsReady();
		Takes::Scene sc;
		int tick = 0; bool playing = false, rec = false, loop = false; std::string phase;
		if (Takes::SnapshotScene(sc, tick, playing, rec, loop, phase)) {
			if (g.selKind == SelKind::None && !sc.tracks.empty()) {
				g.selKind = SelKind::Track;
				g.selTrack = 0;
			}
			if (!playing && g.scrubActive) Takes::SetScrubPreview(true, g.scrubTick);
		}
		advancedfx::Message("sceneeditor: opened\n");
	} else {
		ClearDragState(false);
		Props_StopEditing();
		Takes::SetScrubPreview(false, 0);
		CameraBridge_SetSceneOverlay(nullptr);
		g.armFreeCamFrames = 0;
		if (g.flyCam || CameraBridge_GetFreeCamEnabled()) {
			CameraBridge_SetFreeCamEnabled(false);
			g.flyCam = false;
		}
		if (g.forcedThirdPerson) {
			Exec("firstperson");
			g.forcedThirdPerson = false;
		}
		g.enabled = false;
		g.cursor = false;
		advancedfx::Message("sceneeditor: closed\n");
	}
}

void Toggle() { SetEnabled(!g.enabled); }

bool WantsCursor() { return g.enabled && g.cursor; }

void SetCursor(bool cursor) {
	if (!g.enabled) return;
	g.cursor = cursor;
}

void ToggleCursor() {
	if (!g.enabled) return;
	g.cursor = !g.cursor;
}

bool FlyCam() { return g.enabled && (g.flyCam || CameraBridge_GetFreeCamEnabled()); }

void SetFlyCam(bool fly) {
	if (!g.enabled) return;
	g.armFreeCamFrames = 0;
	if (fly) {
		if (!g.forcedThirdPerson) {
			Exec("mirv_cvar_unlock_sv_cheats");
			Exec("sv_cheats 1");
			Exec("thirdperson");
			Exec("cam_idealyaw 0");
			Exec("c_thirdpersonshoulder 0");
			g.forcedThirdPerson = true;
		}
		CameraBridge_SetFreeCamEnabled(true);
		g.flyCam = true;
	} else {
		CameraBridge_SetFreeCamEnabled(false);
		if (g.forcedThirdPerson) {
			Exec("firstperson");
			g.forcedThirdPerson = false;
		}
		g.flyCam = false;
	}
}

void OnRecordingFinished(int newTrackIdx) {
	if (!Takes::Available()) return;
	SetEnabled(true);
	if (newTrackIdx >= 0) {
		g.selKind = SelKind::Track;
		g.selTrack = newTrackIdx;
		g.scrubActive = true;
		g.scrubTick = 0;
		Takes::SetScrubPreview(true, 0);
		g.statusMsg = "Take recorded and loaded in Scene Editor";
	}
}

void CancelActiveEdit() {
	if (Props_EditorBusy()) Props_StopEditing();
	if (g.dragging) ClearDragState(true);
}

bool BusyEditing() { return g.dragging || Props_EditorBusy(); }

bool TakeCatalogRequested() {
	if (!g.catalogRequested) return false;
	g.catalogRequested = false;
	return true;
}

void RunMainThreadFrame() {
	if (!g.enabled) return;
	if (!Takes::Available()) {
		SetEnabled(false);
		return;
	}
	if (g.armFreeCamFrames > 0 && --g.armFreeCamFrames == 0) {
		CameraBridge_SetFreeCamEnabled(true);
	}
	g.flyCam = CameraBridge_GetFreeCamEnabled();
	Takes::Scene sc;
	int liveTick = 0; bool playing = false, rec = false, loop = false; std::string phase;
	Takes::SnapshotScene(sc, liveTick, playing, rec, loop, phase);
	if (g.selKind == SelKind::Track && (g.selTrack < 0 || g.selTrack >= int(sc.tracks.size()))) {
		g.selTrack = sc.tracks.empty() ? -1 : 0;
		if (g.selTrack < 0) g.selKind = SelKind::None;
	}
	const int effTick = playing ? liveTick : g.scrubTick;
	if (!playing && g.scrubActive) Takes::SetScrubPreview(true, effTick);
	PublishOverlay(sc, effTick);
}

void Shutdown() {
	if (g.enabled) SetEnabled(false);
}

std::string BuildStateJson() {
	Takes::Scene sc;
	int liveTick = 0; bool playing = false, rec = false, loop = false; std::string phase;
	Takes::SnapshotScene(sc, liveTick, playing, rec, loop, phase);
	const float rate = sc.tickRate > 0.f ? sc.tickRate : 64.f;
	const int lengthTicks = (std::max)(1, Takes::SceneLengthTicks(sc));
	const int effTick = playing ? liveTick : g.scrubTick;

	Environment::Matrix vp{};
	Environment::Vec3 eye{};
	const bool haveCam = Environment::RenderedCamera(vp, eye);
	const ActorAnim::Library &lib = ActorAnim::GetLibrary();

	int cmx = 0, cmy = 0; bool clmb = false, crmb = false, cshift = false; unsigned cseq = 0;
	CameraBridge_GetUiCursor(cmx, cmy, clmb, crmb, cshift, cseq);

	JsonBuilder j;
	j.BeginObject();
	j.BoolField("enabled", g.enabled);
	j.BoolField("streamRecording", AfxStreams_RecordingCapturesAfterUi());
	j.BoolField("cursor", g.cursor);
	j.BoolField("flyCam", g.flyCam);
	j.BoolField("playing", playing);
	j.BoolField("recording", rec);
	j.BoolField("loop", loop);
	j.StringField("phase", phase);
	j.StringField("status", g.statusMsg);
	j.DoubleField("tickRate", rate);
	j.IntField("playheadTick", effTick);
	j.IntField("lengthTicks", lengthTicks);
	j.BoolField("scrubActive", g.scrubActive);
	j.StringField("gizmoMode", g.gizmoMode == GizmoMode::Rotate ? "rotate" : "move");
	j.StringField("viewMode", g.viewMode == ViewMode::Graph ? "graph" : "timeline");
	j.StringField("graphChannel", ChannelName(g.channel));
	j.StringField("selKind", g.selKind == SelKind::Track ? "track" : (g.selKind == SelKind::Prop ? "prop" : "none"));
	j.IntField("selTrack", g.selTrack);
	j.IntField("selProp", g.selPropId);
	j.BoolField("dragging", g.dragging);
	j.StringField("dragKind", g.dragTurn ? "turn" : (g.dragging ? "axis" : ""));
	j.IntField("mx", cmx);
	j.IntField("my", cmy);
	j.BoolField("lmb", clmb);
	j.BoolField("rmb", crmb);
	j.BoolField("shift", cshift);

	j.Key("tracks"); j.BeginArray();
	for (size_t i = 0; i < sc.tracks.size(); ++i) {
		const Takes::Track &t = sc.tracks[i];
		const TrackVisualPath vpPath = BuildTrackVisualPath(t, rate, &lib, effTick, 48);
		j.BeginObject();
		j.IntField("index", int(i));
		j.StringField("name", t.name);
		j.IntField("team", int(t.team));
		j.BoolField("enabled", t.enabled);
		j.BoolField("holdFire", t.holdFire);
		j.BoolField("placedActor", t.placedActor);
		j.IntField("delayTicks", t.delayTicks);
		j.IntField("durationTicks", int(t.ticks.size()));
		j.Key("offsetPos"); j.BeginArray();
		j.ValueDouble(t.offsetPos[0]); j.ValueDouble(t.offsetPos[1]); j.ValueDouble(t.offsetPos[2]);
		j.EndArray();
		j.DoubleField("offsetYaw", t.offsetYaw);
		j.DoubleField("startYaw", vpPath.startYaw);
		j.BoolField("hasAirborneArc", vpPath.hasAirborneArc);
		if (haveCam && vpPath.valid) {
			const auto pBadge = Environment::GizmoProject(vp, {vpPath.scrubPos[0], vpPath.scrubPos[1], vpPath.scrubPos[2] + 74.f});
			const auto pScrub = Environment::GizmoProject(vp, {vpPath.scrubPos[0], vpPath.scrubPos[1], vpPath.scrubPos[2] + 34.f});
			const auto pEnd = Environment::GizmoProject(vp, {vpPath.endPos[0], vpPath.endPos[1], vpPath.endPos[2] + 32.f});
			j.Key("startScreen"); j.BeginArray(); j.ValueDouble(pBadge.x); j.ValueDouble(pBadge.y); j.ValueBool(pBadge.visible); j.EndArray();
			j.Key("scrubScreen"); j.BeginArray(); j.ValueDouble(pScrub.x); j.ValueDouble(pScrub.y); j.ValueBool(pScrub.visible); j.EndArray();
			j.Key("endScreen"); j.BeginArray(); j.ValueDouble(pEnd.x); j.ValueDouble(pEnd.y); j.ValueBool(pEnd.visible); j.EndArray();
		}
		j.Key("cues"); j.BeginArray();
		for (size_t ci = 0; ci < t.animCues.size(); ++ci) {
			const auto &c = t.animCues[ci];
			const ActorAnim::Clip *clip = lib.Find(c.clip);
			const float durSec = clip ? ActorAnim::CueEffectiveDuration(c, *clip) : (c.duration > 0.f ? c.duration : 2.f);
			j.BeginObject();
			j.IntField("index", int(ci));
			j.IntField("startTick", c.startTick);
			j.IntField("durationTicks", (std::max)(8, int(std::round(durSec * rate))));
			j.StringField("clip", c.clip);
			j.StringField("display", clip ? clip->displayName : c.clip);
			j.DoubleField("speed", c.speed);
			j.BoolField("loop", c.loop);
			j.BoolField("upper", c.upperBodyOnly);
			j.DoubleField("blendIn", c.blendIn);
			j.DoubleField("blendOut", c.blendOut);
			j.DoubleField("duration", c.duration);
			j.EndObject();
		}
		j.EndArray();
		j.EndObject();
	}
	j.EndArray();

	if (g.selKind == SelKind::Track && g.selTrack >= 0 && g.selTrack < int(sc.tracks.size())) {
		const Takes::Track &selT = sc.tracks[g.selTrack];
		const TrackVisualPath vpSel = BuildTrackVisualPath(selT, rate, &lib, effTick, 48);
		if (haveCam && vpSel.valid) {
			const Environment::Vec3 org{vpSel.scrubPos[0], vpSel.scrubPos[1], vpSel.scrubPos[2] + 24.f};
			const float gsz = Environment::GizmoWorldSize(vp, org);
			const auto qOrg = Environment::GizmoProject(vp, org);
			j.Key("trackGizmo"); j.BeginObject();
			j.Key("origin"); j.BeginArray(); j.ValueDouble(qOrg.x); j.ValueDouble(qOrg.y); j.ValueBool(qOrg.visible); j.EndArray();
			j.Key("axes"); j.BeginArray();
			for (int a = 0; a < 3; ++a) {
				const auto qt = Environment::GizmoProject(vp, org + Environment::GizmoAxis(a) * gsz);
				j.BeginArray(); j.ValueDouble(qt.x); j.ValueDouble(qt.y); j.ValueBool(qt.visible); j.EndArray();
			}
			j.EndArray();
			j.Key("yawRing"); j.BeginArray();
			constexpr int kRingPts = 32;
			for (int k = 0; k < kRingPts; ++k) {
				const float a = float(k) * (6.2831853f / float(kRingPts));
				const Environment::Vec3 rp{org.x + std::cos(a) * gsz * 0.85f, org.y + std::sin(a) * gsz * 0.85f, org.z};
				const auto qr = Environment::GizmoProject(vp, rp);
				j.BeginArray(); j.ValueDouble(qr.x); j.ValueDouble(qr.y); j.ValueBool(qr.visible); j.EndArray();
			}
			j.EndArray();
			j.EndObject();
		}
		const GraphCurve curve = BuildTrackGraphCurve(selT, rate, &lib, g.channel, 128);
		j.Key("graph"); j.BeginObject();
		j.StringField("channel", ChannelName(g.channel));
		j.IntField("startTick", curve.startTick);
		j.IntField("endTick", curve.endTick);
		j.DoubleField("minVal", curve.minVal);
		j.DoubleField("maxVal", curve.maxVal);
		j.Key("samples"); j.BeginArray();
		for (const auto &pt : curve.samples) {
			j.BeginArray(); j.ValueInt(pt.sceneTick); j.ValueDouble(pt.value); j.EndArray();
		}
		j.EndArray();
		j.EndObject();
	}

	j.EndObject();
	std::string out = j.Str();
	std::ostringstream propsPart;
	Props_AppendStateJson(propsPart);
	std::string pStr = propsPart.str();
	if (!pStr.empty() && pStr.front() == ',') {
		out.insert(out.size() - 1, pStr);
	}
	return out;
}

void RunCommand(int argc, advancedfx::ICommandArgs *args, const char *cmd) {
	const char *sub = argc >= 3 ? args->ArgV(2) : "toggle";
	auto arg = [&](int i) { return i < argc ? args->ArgV(i) : ""; };

	if (0 == _stricmp(sub, "on") || 0 == _stricmp(sub, "1")) { SetEnabled(true); return; }
	if (0 == _stricmp(sub, "off") || 0 == _stricmp(sub, "0")) { SetEnabled(false); return; }
	if (0 == _stricmp(sub, "toggle")) { Toggle(); return; }
	if (0 == _stricmp(sub, "cursor")) {
		const char *v = arg(3);
		if (0 == _stricmp(v, "on") || 0 == _stricmp(v, "1")) SetCursor(true);
		else if (0 == _stricmp(v, "off") || 0 == _stricmp(v, "0")) SetCursor(false);
		else ToggleCursor();
		return;
	}
	if (0 == _stricmp(sub, "flycam")) {
		const char *v = arg(3);
		SetFlyCam(0 == _stricmp(v, "toggle") ? !g.flyCam : (0 != _stricmp(v, "off") && 0 != _stricmp(v, "0")));
		return;
	}
	if (0 == _stricmp(sub, "mode")) {
		g.gizmoMode = (0 == _stricmp(arg(3), "rotate")) ? GizmoMode::Rotate : GizmoMode::Move;
		return;
	}
	if (0 == _stricmp(sub, "view")) {
		g.viewMode = (0 == _stricmp(arg(3), "graph")) ? ViewMode::Graph : ViewMode::Timeline;
		return;
	}
	if (0 == _stricmp(sub, "channel")) {
		const char *ch = arg(3);
		if (0 == _stricmp(ch, "speed")) g.channel = GraphChannel::Speed;
		else if (0 == _stricmp(ch, "yaw")) g.channel = GraphChannel::Yaw;
		else if (0 == _stricmp(ch, "anim")) g.channel = GraphChannel::AnimWeight;
		else g.channel = GraphChannel::HeightZ;
		return;
	}
	if (0 == _stricmp(sub, "select")) {
		const char *what = arg(3);
		if (0 == _stricmp(what, "track")) {
			g.selKind = SelKind::Track;
			g.selTrack = atoi(arg(4));
			g.selPropId = 0;
		} else if (0 == _stricmp(what, "prop")) {
			g.selKind = SelKind::Prop;
			g.selPropId = atoi(arg(4));
		} else {
			g.selKind = SelKind::None;
		}
		return;
	}
	if (0 == _stricmp(sub, "pick")) {
		const float u = float(atof(arg(3))), v = float(atof(arg(4)));
		Takes::Scene sc;
		int tick = 0; bool playing = false, rec = false, loop = false; std::string phase;
		Environment::Matrix vp{}; Environment::Vec3 eye{};
		if (Takes::SnapshotScene(sc, tick, playing, rec, loop, phase) && Environment::RenderedCamera(vp, eye)) {
			const ActorAnim::Library &lib = ActorAnim::GetLibrary();
			const float rate = sc.tickRate > 0.f ? sc.tickRate : 64.f;
			const int effTick = playing ? tick : g.scrubTick;
			std::vector<std::array<float, 3>> pts;
			std::vector<int> idxMap;
			for (size_t i = 0; i < sc.tracks.size(); ++i) {
				if (!sc.tracks[i].enabled) continue;
				const TrackVisualPath path = BuildTrackVisualPath(sc.tracks[i], rate, &lib, effTick, 24);
				if (!path.valid) continue;
				auto pushWorldPt = [&](float wx, float wy, float wz) {
					const auto q = Environment::GizmoProject(vp, {wx, wy, wz});
					pts.push_back({q.x, q.y, q.visible ? 1.f : 0.f});
					idxMap.push_back(int(i));
				};
				// Test actor body heights (feet, torso, head) at start & scrub pose so clicking
				// directly on any actor model in the preview picks them reliably.
				for (float hz : {8.f, 36.f, 62.f}) {
					pushWorldPt(path.scrubPos[0], path.scrubPos[1], path.scrubPos[2] + hz);
					pushWorldPt(path.startPos[0], path.startPos[1], path.startPos[2] + hz);
				}
				const size_t stride = (std::max)(size_t(1), path.points.size() / 8);
				for (size_t k = 0; k < path.points.size(); k += stride) {
					const auto &pp = path.points[k];
					pushWorldPt(pp.pos[0], pp.pos[1], pp.pos[2] + 6.f);
				}
			}
			const int hit = pts.empty() ? -1 : PickNearestProjected(u, v, reinterpret_cast<const float(*)[3]>(pts.data()), int(pts.size()), 0.065f);
			if (hit >= 0) {
				g.selKind = SelKind::Track;
				g.selTrack = idxMap[hit];
				g.selPropId = 0;
				return;
			}
		}
		// Nothing in tracks matched -> try props pick
		Execf("mirv_filmmaker props pick %.4f %.4f", u, v);
		return;
	}
	if (0 == _stricmp(sub, "grab")) {
		const char *which = arg(3);
		const float u = float(atof(arg(4))), v = float(atof(arg(5)));
		Takes::Scene sc; int tick = 0; bool playing = false, rec = false, loop = false; std::string phase;
		Environment::Matrix vp{}; Environment::Vec3 eye{};
		if (g.selKind != SelKind::Track || !Takes::SnapshotScene(sc, tick, playing, rec, loop, phase) ||
		    g.selTrack < 0 || g.selTrack >= int(sc.tracks.size()) || !Environment::RenderedCamera(vp, eye)) return;
		const Takes::Track &t = sc.tracks[g.selTrack];
		const TrackVisualPath path = BuildTrackVisualPath(t, sc.tickRate, &ActorAnim::GetLibrary(), playing ? tick : g.scrubTick, 16);
		if (!path.valid) return;
		const Environment::Vec3 org{path.scrubPos[0], path.scrubPos[1], path.scrubPos[2] + 24.f};
		for (int k = 0; k < 3; ++k) g.dragBeforePos[k] = t.offsetPos[k];
		g.dragBeforeYaw = t.offsetYaw;
		g.dragTrack = g.selTrack;
		if (0 == _stricmp(which, "turn")) {
			const auto qo = Environment::GizmoProject(vp, org);
			if (!qo.visible) return;
			g.dragging = true;
			g.dragTurn = true;
			g.dragTurnStartAngle = std::atan2(v - qo.y, u - qo.x);
		} else {
			const int axis = (0 == _stricmp(which, "x")) ? 0 : (0 == _stricmp(which, "y")) ? 1 : 2;
			if (g.gizmoDrag.Begin(g.selTrack + 1, axis, org, eye, vp, u, v)) {
				g.dragging = true;
				g.dragTurn = false;
			}
		}
		return;
	}
	if (0 == _stricmp(sub, "drag")) {
		if (!g.dragging || g.dragTrack < 0) return;
		const float u = float(atof(arg(3))), v = float(atof(arg(4)));
		const bool snap = argc > 5 && atoi(arg(5)) != 0;
		if (g.dragTurn) {
			Takes::Scene sc; int tick = 0; bool playing = false, rec = false, loop = false; std::string phase;
			Environment::Matrix vp{}; Environment::Vec3 eye{};
			if (!Takes::SnapshotScene(sc, tick, playing, rec, loop, phase) ||
			    g.dragTrack >= int(sc.tracks.size()) || !Environment::RenderedCamera(vp, eye)) return;
			const TrackVisualPath path = BuildTrackVisualPath(sc.tracks[g.dragTrack], sc.tickRate, &ActorAnim::GetLibrary(), g.scrubTick, 8);
			const auto qo = Environment::GizmoProject(vp, {path.scrubPos[0], path.scrubPos[1], path.scrubPos[2] + 24.f});
			if (!qo.visible) return;
			float dRad = std::atan2(v - qo.y, u - qo.x) - g.dragTurnStartAngle;
			while (dRad > 3.14159265f) dRad -= 6.2831853f;
			while (dRad < -3.14159265f) dRad += 6.2831853f;
			float dDeg = -dRad * 57.2957795f;
			if (snap) dDeg = std::round(dDeg / 15.f) * 15.f;
			Takes::SetTrackOffset(g.dragTrack, g.dragBeforePos, g.dragBeforeYaw + dDeg, false);
		} else if (g.gizmoDrag.Move(u, v)) {
			float d[3] = {
				g.gizmoDrag.current.x - g.gizmoDrag.origin.x,
				g.gizmoDrag.current.y - g.gizmoDrag.origin.y,
				g.gizmoDrag.current.z - g.gizmoDrag.origin.z,
			};
			if (snap) {
				for (int k = 0; k < 3; ++k) d[k] = std::round(d[k] / 8.f) * 8.f;
			}
			float next[3] = {g.dragBeforePos[0] + d[0], g.dragBeforePos[1] + d[1], g.dragBeforePos[2] + d[2]};
			Takes::SetTrackOffset(g.dragTrack, next, g.dragBeforeYaw, false);
		}
		return;
	}
	if (0 == _stricmp(sub, "release")) {
		if (g.dragging && g.dragTrack >= 0) {
			Takes::Scene sc; int tick = 0; bool playing = false, rec = false, loop = false; std::string phase;
			if (Takes::SnapshotScene(sc, tick, playing, rec, loop, phase) && g.dragTrack < int(sc.tracks.size())) {
				const auto &t = sc.tracks[g.dragTrack];
				Takes::SetTrackOffset(g.dragTrack, t.offsetPos, t.offsetYaw, true);
			}
		}
		ClearDragState(false);
		return;
	}
	if (0 == _stricmp(sub, "cancel")) {
		CancelActiveEdit();
		return;
	}
	if (0 == _stricmp(sub, "offset")) {
		const int idx = atoi(arg(3));
		float pos[3] = {float(atof(arg(4))), float(atof(arg(5))), float(atof(arg(6)))};
		const float yaw = argc > 7 ? float(atof(arg(7))) : 0.f;
		Takes::SetTrackOffset(idx, pos, yaw, true);
		return;
	}
	if (0 == _stricmp(sub, "resetoffset")) {
		Takes::ResetTrackOffset(atoi(arg(3)));
		return;
	}
	if (0 == _stricmp(sub, "snapfloor")) {
		SnapTrackToFloor(atoi(arg(3)));
		return;
	}
	if (0 == _stricmp(sub, "delay")) {
		Takes::SetTrackDelayTicks(atoi(arg(3)), atoi(arg(4)));
		return;
	}
	if (0 == _stricmp(sub, "enable")) {
		Takes::SetTrackEnabled(atoi(arg(3)), 0 != _stricmp(arg(4), "off") && 0 != _stricmp(arg(4), "0"));
		return;
	}
	if (0 == _stricmp(sub, "team")) {
		Takes::SetTrackTeam(atoi(arg(3)), 0 == _stricmp(arg(4), "t") ? 2 : 3);
		return;
	}
	if (0 == _stricmp(sub, "holdfire")) {
		Takes::SetTrackHoldFire(atoi(arg(3)), 0 != _stricmp(arg(4), "off") && 0 != _stricmp(arg(4), "0"));
		return;
	}
	if (0 == _stricmp(sub, "delete")) {
		Takes::DeleteTrack(atoi(arg(3)));
		return;
	}
	if (0 == _stricmp(sub, "cue")) {
		const char *what = arg(3);
		const int idx = atoi(arg(4));
		Takes::Scene sc; int tick = 0; bool playing = false, rec = false, loop = false; std::string phase;
		if (!Takes::SnapshotScene(sc, tick, playing, rec, loop, phase) || idx < 0 || idx >= int(sc.tracks.size())) return;
		if (0 == _stricmp(what, "add")) {
			ActorAnim::Cue c;
			c.clip = arg(5);
			c.startTick = argc > 6 ? atoi(arg(6)) : (std::max)(0, g.scrubTick - sc.tracks[idx].delayTicks);
			if (const auto *cl = ActorAnim::GetLibrary().Find(c.clip)) c.loop = cl->loopable;
			Takes::AddOrUpdateTrackCue(idx, -1, c);
		} else if (0 == _stricmp(what, "move")) {
			const int ci = atoi(arg(5));
			if (ci >= 0 && ci < int(sc.tracks[idx].animCues.size())) {
				ActorAnim::Cue c = sc.tracks[idx].animCues[ci];
				c.startTick = (std::max)(0, atoi(arg(6)));
				Takes::AddOrUpdateTrackCue(idx, ci, c);
			}
		} else if (0 == _stricmp(what, "edit")) {
			const int ci = atoi(arg(5));
			if (ci >= 0 && ci < int(sc.tracks[idx].animCues.size())) {
				ActorAnim::Cue c = sc.tracks[idx].animCues[ci];
				c.speed = std::clamp(float(atof(arg(6))), 0.1f, 3.0f);
				c.loop = atoi(arg(7)) != 0;
				c.upperBodyOnly = atoi(arg(8)) != 0;
				c.blendIn = std::clamp(float(atof(arg(9))), 0.f, 2.f);
				c.blendOut = std::clamp(float(atof(arg(10))), 0.f, 2.f);
				if (argc > 11) c.duration = std::max(0.f, float(atof(arg(11))));
				Takes::AddOrUpdateTrackCue(idx, ci, c);
			}
		} else if (0 == _stricmp(what, "remove")) {
			Takes::RemoveTrackCue(idx, atoi(arg(5)));
		}
		return;
	}
	if (0 == _stricmp(sub, "scrub")) {
		g.scrubActive = true;
		g.scrubTick = (std::max)(0, atoi(arg(3)));
		Takes::SetScrubPreview(true, g.scrubTick);
		return;
	}
	if (0 == _stricmp(sub, "play")) { Takes::Play(); return; }
	if (0 == _stricmp(sub, "stop")) { Takes::Stop(); return; }
	if (0 == _stricmp(sub, "loop")) { Takes::SetLoop(0 != _stricmp(arg(3), "off") && 0 != _stricmp(arg(3), "0")); return; }
	if (0 == _stricmp(sub, "record")) {
		SetEnabled(false);
		Takes::RecordForSceneEditor("");
		return;
	}
	if (0 == _stricmp(sub, "placeactor")) {
		const char *clip = arg(3);
		if (Takes::PlaceActorForSceneEditor(clip)) {
			Takes::Scene sc; int tick = 0; bool playing = false, rec = false, loop = false; std::string phase;
			if (Takes::SnapshotScene(sc, tick, playing, rec, loop, phase) && !sc.tracks.empty()) {
				g.selKind = SelKind::Track;
				g.selTrack = int(sc.tracks.size()) - 1;
				g.scrubActive = true;
				Takes::SetScrubPreview(true, g.scrubTick);
			}
		}
		return;
	}
	if (0 == _stricmp(sub, "catalog")) {
		g.catalogRequested = true;
		return;
	}
	if (0 == _stricmp(sub, "status")) {
		advancedfx::Message("%s sceneeditor: enabled=%s cursor=%s selKind=%d selTrack=%d scrubTick=%d\n",
			cmd, g.enabled ? "on" : "off", g.cursor ? "on" : "off", int(g.selKind), g.selTrack, g.scrubTick);
		return;
	}
	advancedfx::Warning("usage: %s sceneeditor on|off|toggle|cursor|flycam|mode|view|channel|select|pick|grab|drag|release|cancel|offset|resetoffset|snapfloor|delay|enable|team|holdfire|delete|cue|scrub|play|stop|loop|record|placeactor\n", cmd);
}

} // namespace Filmmaker::SceneEditor
