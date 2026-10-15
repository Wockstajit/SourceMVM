#include "CamPathShake.h"

#include "CameraPath.h"
#include "../Render/RenderCommand.h"
#include "RageShake.h"
#include "../Filmmaker.h"            // PlayingDemoPath()
#include "../Demo/DemoProjectStore.h"
#include "../../MirvTime.h"          // interval_per_tick_get: the tick->seconds rate
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../Platform/TextEncoding.h"
#include "../Debug/MvmDebugLog.h" // MvmDebugLog_*

#include "../../../Shared/AfxConsole.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <atomic>
#include <mutex>
#include <sstream>

namespace Filmmaker {

namespace {

// An outgoing keyframe's shake keeps running for this long past the next keyframe's tick, fading
// out while the incoming style's own attack envelope fades in. Without it, swapping styles mid
// path cuts one loop dead on a single frame, which reads as a hitch rather than a transition.
constexpr double kHandoverSec = 0.35;

// UI scales -> RageShake_Eval arguments.
//
// INTENSITY is QUADRATIC, not linear: shakeAmplitude = a*I*I + b*I. The coefficients are ours,
// chosen so 1 is a hint, 5 is the authored strength, 10 is roughly triple it. A linear ramp -- which this used
// before -- makes the bottom half of the dial feel dead and the top half feel like nothing
// changes, which is exactly the "intensity isn't working" symptom.
double AmplitudeFor(double intensity) {
	const double I = intensity;
	return 0.024 * I * I + 0.076 * I; // I=1 -> 0.10, I=5 -> 0.98, I=10 -> 3.16
}

// SPEED is a TIME SCALE FACTOR, not a frequency multiplier: it scales the shaker's whole
// timestep so envelopes stretch with the oscillation. 50 = authored speed, 10 = fifth, 100 = double.
double TimeScaleFor(double speed) { return speed / 50.0; }

double Lerp(double a, double b, double u) { return a + (b - a) * u; }
double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Store {
	// keyframe tick -> setting. Tick, not index: see the header.
	std::map<int, CamPathShakeSetting> byTick;
	std::wstring demoPath;
	std::vector<int> lastTicks; // previous frame's keyframe ticks, for retime detection
	bool dirty = false;
	bool enabled = true;
	bool loaded = false;
};

Store& S() {
	static Store s;
	return s;
}

void Save() {
	Store& s = S();
	if (s.demoPath.empty()) return;

	JsonBuilder jb;
	jb.BeginObject();
	jb.IntField("version", 1);
	jb.BoolField("enabled", s.enabled);
	jb.Key("keys");
	jb.BeginArray();
	for (const auto& kv : s.byTick) {
		if (!kv.second.Active()) continue;
		jb.BeginObject();
		jb.IntField("tick", kv.first);
		// The style is written by NAME, not index. An index would silently re-point at a
		// different shake if the style table ever gains or reorders an entry.
		jb.StringField("style", RageShake_StyleName(kv.second.style));
		jb.IntField("intensity", kv.second.intensity);
		jb.IntField("speed", kv.second.speed);
		jb.EndObject();
	}
	jb.EndArray();
	jb.EndObject();

	// Its OWN section, deliberately not folded into the campath payload: the camera
	// path data is the graph's, and this data must never travel inside it.
	if (!DemoProjectStore_WriteSection(ProjectGame::Cs2, s.demoPath,
		ProjectSection::Camshake, jb.Str()))
		advancedfx::Warning("[pathshake] could not write project.mvm for %s\n",
			WideToUtf8(s.demoPath).c_str());
}

void Load() {
	Store& s = S();
	s.byTick.clear();
	if (s.demoPath.empty()) return;

	std::string text;
	if (!DemoProjectStore_ReadSection(ProjectGame::Cs2, s.demoPath,
		ProjectSection::Camshake, text))
		return; // no camshake section yet

	JsonValue root;
	if (!JsonParse(text, root) || root.type != JsonValue::Type::Object) {
		advancedfx::Warning("[pathshake] bad camshake section for %s\n", WideToUtf8(s.demoPath).c_str());
		return;
	}
	if (const JsonValue* v = root.Find("enabled")) s.enabled = v->AsBool(true);

	int skipped = 0;
	if (const JsonValue* arr = root.Find("keys")) {
		if (arr->type == JsonValue::Type::Array) {
			for (const JsonValue& ko : arr->arr) {
				if (ko.type != JsonValue::Type::Object) continue;
				const JsonValue* tk = ko.Find("tick");
				const JsonValue* st = ko.Find("style");
				if (!tk || !st) continue;
				const int styleIdx = RageShake_FindStyle(st->AsString("").c_str());
				if (styleIdx < 0) { ++skipped; continue; } // unknown style name: drop, don't guess
				CamPathShakeSetting cfg;
				cfg.style = styleIdx;
				const JsonValue* iv = ko.Find("intensity");
				const JsonValue* pv = ko.Find("speed");
				cfg.intensity = ClampInt(iv ? iv->AsInt(5) : 5, 1, 10);
				cfg.speed = ClampInt(pv ? pv->AsInt(50) : 50, 10, 100);
				s.byTick[tk->AsInt(0)] = cfg;
			}
		}
	}
	if (!s.byTick.empty() || skipped)
		advancedfx::Message("[pathshake] loaded %d keyframe shakes%s.\n",
			(int)s.byTick.size(), skipped ? " (some styles unknown, dropped)" : "");
}

// Marker index -> tick, and back. Both go through the live marker list so they can never
// disagree with what the timeline is drawing.
int TickForIndex(int markerIndex) {
	const std::vector<CamMarker>& mk = CameraPathRef().Markers();
	if (markerIndex < 0 || markerIndex >= (int)mk.size()) return INT32_MIN;
	return mk[(size_t)markerIndex].tick;
}

// The shake clock's tick -> seconds rate. DELIBERATELY NOT the markers' recorded `time` fields:
// those go stale (CamPlayback evaluates Live playback by TICK for exactly that reason -- see its
// "marker.time can go STALE after retiming" note, and graph rebakes replace the whole marker
// vector), and a stale time would silently run a shake at the wrong speed for one segment. Ticks
// are the one axis every editor surface agrees on, so seconds are derived from them directly --
// the same `tick * interval_per_tick` mapping CameraPath::MoveKey itself uses.
double SecondsPerTick() {
	const float ipt = g_MirvTime.interval_per_tick_get();
	// No demo / engine not resolved: fall back to CS2's 64-tick rate rather than a zero rate
	// (which would freeze every shake at its t=0 pose and look exactly like "not working").
	return (ipt > 0.0f) ? (double)ipt : (1.0 / 64.0);
}

// Follow retimes and prune orphans. Runs against the live keyframe tick list, so it is correct
// whoever moved the key -- the timeline's "Tick->Here", a graph drag, or an undo.
void SyncToMarkers() {
	Store& s = S();
	const std::vector<CamMarker>& mk = CameraPathRef().Markers();

	std::vector<int> ticks;
	ticks.reserve(mk.size());
	for (const CamMarker& m : mk) ticks.push_back(m.tick);

	if (ticks == s.lastTicks)
		return;

	// An EMPTY keyframe list means "there is nothing to sync against", never "delete everything".
	// Without this the prune below would wipe a freshly loaded sidecar and save the empty result
	// on any frame the path happens to be empty -- a demo opening before its campath sidecar
	// lands, a Clear that is later undone, a graph rebake passing through zero. Data loss with no
	// symptom until the user reopens the demo.
	if (ticks.empty()) {
		s.lastTicks.clear();
		return;
	}

	// Same keyframe COUNT with ticks that moved = a retime. Migrate index-wise: the list stays
	// tick-ordered, so position i is still the same keyframe.
	if (!s.byTick.empty() && ticks.size() == s.lastTicks.size()) {
		std::map<int, CamPathShakeSetting> moved;
		for (size_t i = 0; i < ticks.size(); ++i) {
			auto it = s.byTick.find(s.lastTicks[i]);
			if (it != s.byTick.end())
				moved[ticks[i]] = it->second;
		}
		if (moved != s.byTick) {
			s.byTick.swap(moved);
			s.dirty = true;
		}
	} else if (!s.byTick.empty()) {
		// Count changed (insert / delete / graph rebake): keep only settings whose keyframe is
		// still there. A stray entry would otherwise fire a shake with no diamond to turn it off.
		std::map<int, CamPathShakeSetting> kept;
		for (int t : ticks) {
			auto it = s.byTick.find(t);
			if (it != s.byTick.end()) kept[t] = it->second;
		}
		if (kept.size() != s.byTick.size()) {
			s.byTick.swap(kept);
			s.dirty = true;
		}
	}

	s.lastTicks.swap(ticks);
}

// ---------------------------------------------------------------------------------------------
// THE PUBLISHED SNAPSHOT -- and why it has to exist.
//
// The shake is applied from the view-setup trampoline, and that trampoline is NOT guaranteed to
// be the main thread (main.cpp keeps its per-call state in a `thread_local` precisely because it
// can be entered from more than one). Everything this module owns on the main thread is a
// container: `Store::byTick` is a std::map, and the keyframes it is keyed against live in
// CameraPath's std::vector.
//
// Reading those from the view thread crashed the game on demo load, and the sequence is exact:
// CameraPath::RunFrame sees the new demo, calls m_data.DeleteAll() and Load(), which REALLOCATES
// the marker vector -- while the view thread is part-way through walking it. Stale sidecar data
// was never involved; the same crash happens with an empty one.
//
// So the main thread publishes a flat, self-contained snapshot once per frame and the view thread
// reads ONLY that. It carries every marker tick (not just the shaken ones) because the run-start
// walk and the segment search need the full ordered list.
//
// The lock is held for a copy and nothing else -- never across the evaluation, never across file
// IO -- so the view thread's worst case is one memcpy of a few hundred bytes.
struct SnapKey {
	int tick = 0;
	int style = -1;   // < 0 = this keyframe carries no shake
	int intensity = 5;
	int speed = 50;
	bool Active() const { return style >= 0 && intensity > 0; }
};

std::mutex g_snapMutex;
std::vector<SnapKey> g_snapKeys;            // guarded by g_snapMutex; ordered by tick
double g_snapSecondsPerTick = 1.0 / 64.0;   // guarded
bool g_snapEnabled = false;                 // guarded
bool g_snapAnyShake = false;                // guarded: at least one key is Active()

// Main thread, once per frame from CamPathShake_RunFrame.
void PublishSnapshot() {
	Store& s = S();
	const std::vector<CamMarker>& mk = CameraPathRef().Markers();

	std::vector<SnapKey> next;
	next.reserve(mk.size());
	bool any = false;
	for (const CamMarker& m : mk) {
		SnapKey k;
		k.tick = m.tick;
		auto it = s.byTick.find(m.tick);
		if (it != s.byTick.end() && it->second.Active()) {
			k.style = it->second.style;
			k.intensity = it->second.intensity;
			k.speed = it->second.speed;
			any = true;
		}
		next.push_back(k);
	}

	std::lock_guard<std::mutex> lock(g_snapMutex);
	g_snapKeys.swap(next);
	g_snapSecondsPerTick = SecondsPerTick();
	g_snapEnabled = s.enabled;
	g_snapAnyShake = any;
}

// Any thread. Copies into `out` (reusing its capacity, so the steady state does not allocate).
// Returns false when there is nothing to apply, without touching `out`.
bool ReadSnapshot(std::vector<SnapKey>& out, double& outSecondsPerTick) {
	std::lock_guard<std::mutex> lock(g_snapMutex);
	if (!g_snapEnabled || !g_snapAnyShake || g_snapKeys.empty())
		return false;
	out.assign(g_snapKeys.begin(), g_snapKeys.end());
	outSecondsPerTick = g_snapSecondsPerTick;
	return true;
}

// Evaluate a style `tSec` into its run, at an already-blended intensity/speed.
RageShakePose EvalStyle(int style, double intensity, double speed, double tSec) {
	if (style < 0 || intensity <= 0.0 || tSec < 0.0) return RageShakePose();
	const RageShakeDef& def = RageShake_Style(style);
	return RageShake_Eval(def, tSec, AmplitudeFor(intensity), TimeScaleFor(speed));
}

} // namespace

bool CamPathShake_Enabled() { return S().enabled; }

void CamPathShake_SetEnabled(bool on) {
	Store& s = S();
	if (s.enabled == on) return;
	s.enabled = on;
	s.dirty = true;
}

void CamPathShake_RunFrame() {
	Store& s = S();

	// Follow the demo the ENGINE is playing, matching CameraPath::RunFrame, so the shakes and the
	// keyframes they belong to are always loaded from the same .dem.
	std::wstring cur = PlayingDemoPath();
	if (cur != s.demoPath || !s.loaded) {
		if (s.dirty && !s.demoPath.empty()) { Save(); s.dirty = false; }
		s.demoPath = cur;
		s.lastTicks.clear();
		Load();
		s.loaded = true;
	}

	SyncToMarkers();

	// Hand the view thread an immutable copy of what it needs. Must come AFTER the sync so the
	// snapshot matches the keyframes as they are this frame.
	PublishSnapshot();

	if (s.dirty) { Save(); s.dirty = false; }
}

CamPathShakeSetting CamPathShake_Get(int markerIndex) {
	const int tick = TickForIndex(markerIndex);
	if (tick == INT32_MIN) return CamPathShakeSetting();
	Store& s = S();
	auto it = s.byTick.find(tick);
	if (it == s.byTick.end()) return CamPathShakeSetting();
	return it->second;
}

void CamPathShake_Set(int markerIndex, const CamPathShakeSetting& in) {
	const int tick = TickForIndex(markerIndex);
	if (tick == INT32_MIN) return;
	CamPathShakeSetting cfg = in;
	cfg.style = (cfg.style >= 0 && cfg.style < RageShake_StyleCount()) ? cfg.style : -1;
	cfg.intensity = ClampInt(cfg.intensity, 1, 10);
	cfg.speed = ClampInt(cfg.speed, 10, 100);
	Store& s = S();
	if (cfg.style < 0) s.byTick.erase(tick);
	else s.byTick[tick] = cfg;
	s.dirty = true;
}

void CamPathShake_Clear(int markerIndex) {
	const int tick = TickForIndex(markerIndex);
	if (tick == INT32_MIN) return;
	Store& s = S();
	if (s.byTick.erase(tick) > 0) s.dirty = true;
}

void CamPathShake_ClearAll() {
	Store& s = S();
	if (s.byTick.empty()) return;
	s.byTick.clear();
	s.dirty = true;
}

int CamPathShake_Count() { return (int)S().byTick.size(); }

int CamPathShake_ApplyToAll(int fromMarkerIndex) {
	const CamPathShakeSetting src = CamPathShake_Get(fromMarkerIndex);
	if (!src.Active())
		return 0;
	Store& s = S();
	const std::vector<CamMarker>& mk = CameraPathRef().Markers();
	int written = 0;
	for (const CamMarker& m : mk) {
		auto it = s.byTick.find(m.tick);
		if (it != s.byTick.end() && it->second == src) continue; // already identical
		s.byTick[m.tick] = src;
		++written;
	}
	// Every keyframe now carries the same style, so the whole path is ONE continuous run: the
	// run-start walk in Apply() will not restart the oscillator at each key, and looping styles
	// hold across the entire shot instead of re-attacking on every diamond.
	if (written > 0) s.dirty = true;
	return written;
}

// The evaluator. Works ONLY on a snapshot, so it is safe on any thread and cannot be affected by
// the main thread editing keyframes underneath it.
bool CamPathShake_EvalSnapshot(const std::vector<SnapKey>& keys, double secondsPerTick,
	double lookupTick,
	double& x, double& y, double& z,
	double& pitch, double& yaw, double& roll, double& fov) {
	const int n = (int)keys.size();
	if (n == 0) return false;
	if (lookupTick < (double)keys[0].tick)
		return false; // before the path starts: nothing has been triggered yet

	// Which keyframe owns this point on the path? The one at or before it.
	int seg = 0;
	for (int i = 0; i < n; ++i) {
		if ((double)keys[(size_t)i].tick <= lookupTick) seg = i;
		else break;
	}

	// Seconds derived from ticks, so the shake is a pure function of the path tick: scrub,
	// preview and render all agree, and re-renders are frame-identical.
	const double spt = secondsPerTick;
	const double now = lookupTick * spt;

	auto findAt = [&](int i) -> const SnapKey* {
		if (i < 0 || i >= n) return nullptr;
		const SnapKey& k = keys[(size_t)i];
		return k.Active() ? &k : nullptr;
	};
	// Where did the CURRENT RUN of this style begin? Walk back over consecutive keyframes that
	// carry the same style. This is what makes a looping shake CONTINUOUS across a run of keyframes instead of
	// re-attacking at each one.
	auto runStartOf = [&](int i) {
		const SnapKey* c = findAt(i);
		if (!c) return i;
		// One-shot exception: a one-shot is never contiguous with a previous keyframe of the
		// same style -- each burst fires on its own keyframe.
		if (RageShake_Finished(RageShake_Style(c->style), 1.0e6, 1.0))
			return i;
		while (i > 0) {
			const SnapKey* prev = findAt(i - 1);
			if (!prev || prev->style != c->style) break;
			--i;
		}
		return i;
	};

	RageShakePose sum;

	const SnapKey* cur = findAt(seg);
	if (cur) {
		double intensity = (double)cur->intensity;
		double speed = (double)cur->speed;
		// Within a run, intensity and speed glide toward the next keyframe's values.
		// This is what lets a shake ramp up over a shot instead of stepping.
		const SnapKey* nxt = findAt(seg + 1);
		if (nxt && nxt->style == cur->style) {
			const double span = (double)(keys[(size_t)seg + 1].tick - keys[(size_t)seg].tick);
			const double u = (span > 0.0)
				? Clamp01((lookupTick - (double)keys[(size_t)seg].tick) / span) : 0.0;
			intensity = Lerp(intensity, (double)nxt->intensity, u);
			speed = Lerp(speed, (double)nxt->speed, u);
		}
		const int runStart = runStartOf(seg);
		sum.AddScaled(EvalStyle(cur->style, intensity, speed,
			now - (double)keys[(size_t)runStart].tick * spt), 1.0);
	}

	// A DIFFERENT style ending at this keyframe keeps decaying through the handover window, so a
	// style change is a transition rather than a cut. Same-style runs never reach this -- they
	// are one continuous shake above.
	if (seg > 0) {
		const double since = now - (double)keys[(size_t)seg].tick * spt;
		if (since >= 0.0 && since < kHandoverSec) {
			const SnapKey* prev = findAt(seg - 1);
			if (prev && (!cur || prev->style != cur->style)) {
				const int prevRun = runStartOf(seg - 1);
				const double w = 1.0 - (since / kHandoverSec);
				sum.AddScaled(EvalStyle(prev->style, (double)prev->intensity, (double)prev->speed,
					now - (double)keys[(size_t)prevRun].tick * spt), w);
			}
		}
	}

	if (!sum.Any())
		return false;

	// Camera-basis offsets -> world, using the pose's OWN yaw/pitch (the authored orientation),
	// so "up" means up relative to the shot, not relative to the world.
	if (sum.right != 0.0 || sum.fwd != 0.0 || sum.up != 0.0) {
		const double kDeg2Rad = 0.017453292519943295;
		const double cy = std::cos(yaw * kDeg2Rad), sy = std::sin(yaw * kDeg2Rad);
		const double cp = std::cos(pitch * kDeg2Rad), sp = std::sin(pitch * kDeg2Rad);
		// Source AngleVectors with roll = 0:
		//   forward = ( cp*cy,  cp*sy, -sp)
		//   right   = (    sy,    -cy,   0)
		//   up      = ( sp*cy,  sp*sy,  cp)
		const double fx = cp * cy, fy = cp * sy, fz = -sp;
		const double rx = sy, ry = -cy, rz = 0.0;
		const double ux = sp * cy, uy = sp * sy, uz = cp;
		x += sum.fwd * fx + sum.right * rx + sum.up * ux;
		y += sum.fwd * fy + sum.right * ry + sum.up * uy;
		z += sum.fwd * fz + sum.right * rz + sum.up * uz;
	}
	x += sum.worldX; y += sum.worldY; z += sum.worldZ;
	pitch += sum.pitch;
	yaw += sum.yaw;
	roll += sum.roll;
	fov += sum.fov;
	return true;
}

bool CamPathShake_Apply(double lookupTick,
	double& x, double& y, double& z,
	double& pitch, double& yaw, double& roll, double& fov) {
	std::vector<SnapKey> keys;
	double spt = 1.0 / 64.0;
	if (!ReadSnapshot(keys, spt))
		return false;
	return CamPathShake_EvalSnapshot(keys, spt, lookupTick, x, y, z, pitch, yaw, roll, fov);
}

bool CamPathShake_ApplyToView(
	float& x, float& y, float& z,
	float& pitch, float& yaw, float& roll, float& fov) {
	// GATE ON OUR OWN CAMERA PATH -- deliberately NOT on ShakeCamMode.
	//
	// This looks like it should ask `owner.Resolve() == ShakeCamMode::CameraPath`, and doing that
	// silently disabled the entire feature. `owner.path` is set only by the LEGACY HLAE campath
	// (`g_CamPath.Enabled_get()` in main.cpp). The filmmaker path never enables g_CamPath -- see
	// CamPathEval::Rebuild's `g_CamPath.Enabled_set(false)` -- it publishes its pose through
	// CameraBridge_SetCameraPose, which reaches the view as a MirvInput override and sets
	// `owner.freeCam`. So a filmmaker dolly shot resolves to ShakeCamMode::FreeCam, the check
	// never matched, and the shake never ran once.
	//
	// The authoritative question is "is the dolly driving the view right now?", and CameraPath
	// answers it directly: CurrentPathTick() succeeds only while playing or scrubbing, which is
	// exactly when the pose in x/y/z/pitch/yaw/roll came from the path.
	double lookupTick = 0.0;
	// During a Camera Path render the job drives the path itself and the editor is not playing,
	// so CurrentPathTick() is false there: take the tick the render is evaluating instead.
	if (!CameraPathRef().CurrentPathTick(lookupTick) && !Render_PathCameraTick(lookupTick))
		return false;

	// EVERYTHING below runs on the view thread, so it touches only the published snapshot and
	// plain scalars -- never Store::byTick and never CameraPath's marker vector. Reading those
	// here is what crashed the game on demo load (see PublishSnapshot).
	//
	// Reused across calls so the steady state performs no allocation. thread_local because the
	// view trampoline can be entered from more than one thread (main.cpp keeps its own per-call
	// state thread_local for the same reason).
	static thread_local std::vector<SnapKey> s_keys;
	double spt = 1.0 / 64.0;
	if (!ReadSnapshot(s_keys, spt))
		return false;

	double dx = x, dy = y, dz = z;
	double dPitch = pitch, dYaw = yaw, dRoll = roll, dFov = fov;
	if (!CamPathShake_EvalSnapshot(s_keys, spt, lookupTick, dx, dy, dz, dPitch, dYaw, dRoll, dFov))
		return false;

	if (MvmDebugLog_Active()) {
		static std::atomic<unsigned> s_n{ 0 };
		if ((s_n.fetch_add(1, std::memory_order_relaxed) % 30) == 0)
			MvmDebugLog_Linef("pathshake.view",
				"tick=%.1f keys=%d dPos=(%.2f %.2f %.2f) dAng=(%.3f %.3f %.3f) dFov=%.3f",
				lookupTick, (int)s_keys.size(),
				dx - (double)x, dy - (double)y, dz - (double)z,
				dPitch - (double)pitch, dYaw - (double)yaw, dRoll - (double)roll,
				dFov - (double)fov);
	}

	x = (float)dx; y = (float)dy; z = (float)dz;
	pitch = (float)dPitch; yaw = (float)dYaw; roll = (float)dRoll; fov = (float)dFov;
	return true;
}

std::string CamPathShake_StateJsonArray() {
	Store& s = S();
	const std::vector<CamMarker>& mk = CameraPathRef().Markers();
	std::ostringstream o;
	o << "[";
	bool first = true;
	for (int i = 0; i < (int)mk.size(); ++i) {
		auto it = s.byTick.find(mk[(size_t)i].tick);
		if (it == s.byTick.end() || !it->second.Active()) continue;
		if (!first) o << ",";
		first = false;
		o << "{\"i\":" << i
			<< ",\"s\":" << it->second.style
			<< ",\"n\":" << it->second.intensity
			<< ",\"p\":" << it->second.speed << "}";
	}
	o << "]";
	return o.str();
}

} // namespace Filmmaker
