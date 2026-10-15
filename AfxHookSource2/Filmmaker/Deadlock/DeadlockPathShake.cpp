#include "DeadlockPathShake.h"

#include "DeadlockShot.h"
#include "../Movie/CamMarkers.h"
#include "../Movie/RageShake.h"
#include "../Demo/DemoProjectStore.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../Platform/TextEncoding.h"
#include "../Debug/MvmDebugLog.h"
#include "../../Deadlock/DeadlockEngine.h"
#include "../../../Shared/AfxCommandLine.h"
#include "../../../Shared/AfxConsole.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

namespace Filmmaker {
namespace {

constexpr double kHandoverSec = 0.35;

int ClampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
double Clamp01(double v) { return std::max(0.0, std::min(1.0, v)); }
double AmplitudeFor(double intensity) { return 0.024 * intensity * intensity + 0.076 * intensity; }
double TimeScaleFor(double speed) { return speed / 50.0; }

struct Store {
	std::map<int, DeadlockPathShakeSetting> byTick;
	std::vector<int> lastTicks;
	std::wstring demoPath;
	bool enabled = true;
	bool loaded = false;
	bool dirty = false;
} g_store;

struct SnapKey {
	int tick = 0, style = -1, intensity = 5, speed = 50;
	bool Active() const { return style >= 0 && intensity > 0; }
};

std::mutex g_snapMutex;
std::vector<SnapKey> g_snap;
double g_secondsPerTick = 1.0 / 64.0;
bool g_snapEnabled = true;

std::wstring DemoPath() {
	return Utf8ToWide(Deadlock::Engine_DemoFilePath());
}

void Save() {
	if (g_store.demoPath.empty()) return;
	JsonBuilder b;
	b.BeginObject(); b.IntField("version", 1); b.BoolField("enabled", g_store.enabled);
	b.Key("keys"); b.BeginArray();
	for (const auto& entry : g_store.byTick) {
		if (!entry.second.Active()) continue;
		b.BeginObject(); b.IntField("tick", entry.first);
		b.StringField("style", RageShake_StyleName(entry.second.style));
		b.IntField("intensity", entry.second.intensity); b.IntField("speed", entry.second.speed);
		b.EndObject();
	}
	b.EndArray(); b.EndObject();
	if (!DemoProjectStore_WriteSection(ProjectGame::Deadlock, g_store.demoPath,
		ProjectSection::Camshake, b.Str())) {
		advancedfx::Warning("[deadlock pathshake] could not write project.mvm for %s\n",
			WideToUtf8(g_store.demoPath).c_str());
		return;
	}
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("deadlock.pathshake.save", "demo=%ls keys=%d bytes=%d",
			g_store.demoPath.c_str(), (int)g_store.byTick.size(), (int)b.Str().size());
}

void Load() {
	g_store.byTick.clear();
	if (g_store.demoPath.empty()) return;
	std::string text;
	if (!DemoProjectStore_ReadSection(ProjectGame::Deadlock, g_store.demoPath,
		ProjectSection::Camshake, text))
		return;
	JsonValue root;
	if (!JsonParse(text, root) || root.type != JsonValue::Type::Object) return;
	if (const JsonValue* v = root.Find("enabled")) g_store.enabled = v->AsBool(true);
	if (const JsonValue* keys = root.Find("keys"); keys && keys->type == JsonValue::Type::Array) {
		for (const JsonValue& item : keys->arr) {
			if (item.type != JsonValue::Type::Object) continue;
			const JsonValue* tick = item.Find("tick"); const JsonValue* style = item.Find("style");
			if (!tick || !style) continue;
			DeadlockPathShakeSetting s;
			s.style = RageShake_FindStyle(style->AsString("").c_str());
			if (s.style < 0) continue;
			s.intensity = ClampInt(item.Find("intensity") ? item.Find("intensity")->AsInt(5) : 5, 1, 10);
			s.speed = ClampInt(item.Find("speed") ? item.Find("speed")->AsInt(50) : 50, 10, 100);
			g_store.byTick[tick->AsInt(0)] = s;
		}
	}
}

void SyncMarkers() {
	std::vector<int> ticks;
	for (int i = 0; i < Shot_MarkerCount(); ++i)
		if (const CamMarker* m = Shot_Marker(i)) ticks.push_back(m->tick);
	if (ticks == g_store.lastTicks) return;
	if (ticks.empty()) { g_store.lastTicks.clear(); return; }
	if (!g_store.byTick.empty() && ticks.size() == g_store.lastTicks.size()) {
		std::map<int, DeadlockPathShakeSetting> moved;
		for (size_t i = 0; i < ticks.size(); ++i) {
			auto it = g_store.byTick.find(g_store.lastTicks[i]);
			if (it != g_store.byTick.end()) moved[ticks[i]] = it->second;
		}
		if (moved != g_store.byTick) { g_store.byTick.swap(moved); g_store.dirty = true; }
	} else if (!g_store.byTick.empty()) {
		std::map<int, DeadlockPathShakeSetting> kept;
		for (int tick : ticks) {
			auto it = g_store.byTick.find(tick);
			if (it != g_store.byTick.end()) kept[tick] = it->second;
		}
		if (kept != g_store.byTick) { g_store.byTick.swap(kept); g_store.dirty = true; }
	}
	g_store.lastTicks.swap(ticks);
}

void Publish() {
	std::vector<SnapKey> next;
	for (int i = 0; i < Shot_MarkerCount(); ++i) {
		const CamMarker* marker = Shot_Marker(i); if (!marker) continue;
		SnapKey k; k.tick = marker->tick;
		auto it = g_store.byTick.find(k.tick);
		if (it != g_store.byTick.end()) {
			k.style = it->second.style; k.intensity = it->second.intensity; k.speed = it->second.speed;
		}
		next.push_back(k);
	}
	std::lock_guard<std::mutex> lock(g_snapMutex);
	g_snap.swap(next);
	g_secondsPerTick = Deadlock::Engine_IntervalPerTick();
	if (!(g_secondsPerTick > 0.0)) g_secondsPerTick = 1.0 / 64.0;
	g_snapEnabled = g_store.enabled;
}

RageShakePose Eval(int style, double intensity, double speed, double elapsed) {
	return RageShake_Eval(RageShake_Style(style), elapsed,
		AmplitudeFor(intensity), TimeScaleFor(speed));
}

bool Apply(const std::vector<SnapKey>& keys, double spt, double tick,
	double& x, double& y, double& z, double& pitch, double& yaw, double& roll, double& fov) {
	if (keys.empty() || tick < keys.front().tick) return false;
	int seg = 0;
	for (int i = 1; i < (int)keys.size() && keys[i].tick <= tick; ++i) seg = i;
	auto active = [&](int i) -> const SnapKey* {
		return i >= 0 && i < (int)keys.size() && keys[i].Active() ? &keys[i] : nullptr;
	};
	auto runStart = [&](int i) {
		const SnapKey* cur = active(i); if (!cur) return i;
		if (RageShake_Finished(RageShake_Style(cur->style), 1.0e6, 1.0)) return i;
		while (i > 0) { const SnapKey* p = active(i - 1); if (!p || p->style != cur->style) break; --i; }
		return i;
	};
	const double now = tick * spt;
	RageShakePose sum;
	const SnapKey* cur = active(seg);
	if (cur) {
		double intensity = cur->intensity, speed = cur->speed;
		const SnapKey* next = active(seg + 1);
		if (next && next->style == cur->style) {
			const double span = keys[seg + 1].tick - keys[seg].tick;
			const double u = span > 0.0 ? Clamp01((tick - keys[seg].tick) / span) : 0.0;
			intensity += (next->intensity - intensity) * u;
			speed += (next->speed - speed) * u;
			sum.AddScaled(Eval(cur->style, intensity, speed,
				now - keys[runStart(seg)].tick * spt), 1.0);
		} else sum.AddScaled(Eval(cur->style, cur->intensity, cur->speed,
			now - keys[runStart(seg)].tick * spt), 1.0);
	}
	if (seg > 0) {
		const double since = now - keys[seg].tick * spt;
		const SnapKey* prev = active(seg - 1);
		if (since >= 0.0 && since < kHandoverSec && prev && (!cur || prev->style != cur->style))
			sum.AddScaled(Eval(prev->style, prev->intensity, prev->speed,
				now - keys[runStart(seg - 1)].tick * spt), 1.0 - since / kHandoverSec);
	}
	if (!sum.Any()) return false;
	const double rad = 0.017453292519943295;
	const double cy = std::cos(yaw * rad), sy = std::sin(yaw * rad), cp = std::cos(pitch * rad), sp = std::sin(pitch * rad);
	x += sum.fwd * cp * cy + sum.right * sy + sum.up * sp * cy + sum.worldX;
	y += sum.fwd * cp * sy - sum.right * cy + sum.up * sp * sy + sum.worldY;
	z += -sum.fwd * sp + sum.up * cp + sum.worldZ;
	pitch += sum.pitch; yaw += sum.yaw; roll += sum.roll; fov += sum.fov;
	return true;
}

int TickForIndex(int index) {
	const CamMarker* marker = Shot_Marker(index);
	return marker ? marker->tick : INT32_MIN;
}

} // namespace

void DeadlockPathShake_RunFrame() {
	const std::wstring demo = DemoPath();
	if (demo != g_store.demoPath || !g_store.loaded) {
		if (g_store.dirty) Save();
		g_store.demoPath = demo; g_store.lastTicks.clear(); g_store.dirty = false; g_store.loaded = true;
		Load();
	}
	SyncMarkers(); Publish();
	if (g_store.dirty) { Save(); g_store.dirty = false; }
}

bool DeadlockPathShake_ApplyToView(float& x, float& y, float& z,
	float& pitch, float& yaw, float& roll, float& fov) {
	double tick = 0.0; if (!Shot_CurrentPathTick(tick)) return false;
	static thread_local std::vector<SnapKey> keys;
	double spt = 1.0 / 64.0;
	{
		std::lock_guard<std::mutex> lock(g_snapMutex);
		if (!g_snapEnabled || g_snap.empty()) return false;
		keys = g_snap; spt = g_secondsPerTick;
	}
	double dx = x, dy = y, dz = z, dp = pitch, dyaw = yaw, dr = roll, df = fov;
	if (!Apply(keys, spt, tick, dx, dy, dz, dp, dyaw, dr, df)) return false;
	if (MvmDebugLog_Active()) {
		static unsigned throttle = 0;
		if ((throttle++ % 30) == 0)
			MvmDebugLog_Linef("deadlock.pathshake.view", "tick=%.2f dPos=(%.3f %.3f %.3f) dAng=(%.3f %.3f %.3f) dFov=%.3f",
				tick, dx - x, dy - y, dz - z, dp - pitch, dyaw - yaw, dr - roll, df - fov);
	}
	x = (float)dx; y = (float)dy; z = (float)dz; pitch = (float)dp; yaw = (float)dyaw; roll = (float)dr; fov = (float)df;
	return true;
}

void DeadlockPathShake_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* op = argc >= 3 ? args->ArgV(2) : "";
	if (0 == _stricmp(op, "on") || 0 == _stricmp(op, "off") || 0 == _stricmp(op, "toggle")) {
		g_store.enabled = 0 == _stricmp(op, "toggle") ? !g_store.enabled : 0 == _stricmp(op, "on");
		g_store.dirty = true; return;
	}
	if (0 == _stricmp(op, "styles")) {
		for (int i = 0; i < RageShake_MenuCount(); ++i) {
			const int style = RageShake_MenuStyle(i);
			advancedfx::Message("  %d: %s (%s)\n", style, RageShake_StyleLabel(style), RageShake_StyleName(style));
		}
		return;
	}
	if (0 == _stricmp(op, "clearall")) { if (!g_store.byTick.empty()) { g_store.byTick.clear(); g_store.dirty = true; } return; }
	if ((0 == _stricmp(op, "clear") || 0 == _stricmp(op, "applyall") || 0 == _stricmp(op, "set")) && argc >= 4) {
		const int index = 0 == _stricmp(args->ArgV(3), "selected") ? Shot_Selected() : std::atoi(args->ArgV(3));
		const int tick = TickForIndex(index);
		if (tick == INT32_MIN) { advancedfx::Warning("%s pathshake: no marker %d\n", cmd, index); return; }
		if (0 == _stricmp(op, "clear")) { if (g_store.byTick.erase(tick)) g_store.dirty = true; return; }
		if (0 == _stricmp(op, "applyall")) {
			auto found = g_store.byTick.find(tick); if (found == g_store.byTick.end()) return;
			for (int i = 0; i < Shot_MarkerCount(); ++i) g_store.byTick[TickForIndex(i)] = found->second;
			g_store.dirty = true; return;
		}
		if (argc < 5) return;
		DeadlockPathShakeSetting s;
		s.style = RageShake_FindStyle(args->ArgV(4));
		if (s.style < 0) s.style = std::atoi(args->ArgV(4));
		if (s.style < 0 || s.style >= RageShake_StyleCount()) { advancedfx::Warning("%s pathshake: unknown style\n", cmd); return; }
		s.intensity = ClampInt(argc >= 6 ? std::atoi(args->ArgV(5)) : 5, 1, 10);
		s.speed = ClampInt(argc >= 7 ? std::atoi(args->ArgV(6)) : 50, 10, 100);
		g_store.byTick[tick] = s; g_store.dirty = true; return;
	}
	advancedfx::Message("%s pathshake on|off|toggle | set <marker> <style> [intensity 1-10] [speed 10-100] | clear <marker> | clearall | applyall <marker> | styles\n", cmd);
}

void DeadlockPathShake_AppendStateJson(std::string& outString) {
	std::ostringstream out;
	out << ",\"pathShakeOn\":" << (g_store.enabled ? "true" : "false") << ",\"shakes\":[";
	bool first = true;
	for (int i = 0; i < Shot_MarkerCount(); ++i) {
		auto found = g_store.byTick.find(TickForIndex(i)); if (found == g_store.byTick.end()) continue;
		if (!first) out << ","; first = false;
		out << "{\"i\":" << i << ",\"s\":" << found->second.style
			<< ",\"n\":" << found->second.intensity << ",\"p\":" << found->second.speed << "}";
	}
	out << "],\"shakeEssentialsCount\":" << RageShake_RockstarEditorCount() << ",\"shakeStyles\":[";
	for (int i = 0; i < RageShake_MenuCount(); ++i) {
		if (i) out << ",";
		const int style = RageShake_MenuStyle(i);
		out << "[" << style << ",\"" << RageShake_StyleLabel(style) << "\",\""
			<< RageShake_StyleName(style) << "\"]";
	}
	out << "]";
	outString += out.str();
}

} // namespace Filmmaker
