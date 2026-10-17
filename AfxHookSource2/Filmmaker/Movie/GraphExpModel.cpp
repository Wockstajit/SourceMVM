#include "GraphExpModel.h"

#include "CamMarkers.h" // CamMarker (read-only seed source)

#include <algorithm>
#include <cmath>
#include <utility>

namespace Filmmaker {

namespace {
double MarkerChannel(const CamMarker& m, int ch) {
	switch (ch) {
	case 0: return m.x;
	case 1: return m.y;
	case 2: return m.z;
	case 3: return m.pitch;
	case 4: return m.yaw;
	case 5: return m.roll;
	default: return m.fov;
	}
}
void SetMarkerChannel(CamMarker& m, int ch, double v) {
	switch (ch) {
	case 0: m.x = v; break;
	case 1: m.y = v; break;
	case 2: m.z = v; break;
	case 3: m.pitch = v; break;
	case 4: m.yaw = v; break;
	case 5: m.roll = v; break;
	default: m.fov = v; break;
	}
}
GraphCurve::Handle ToHandle(const GraphExpModel::ControlPoint& cp) {
	GraphCurve::Handle h; h.active = cp.active; h.tx = cp.tx; h.dv = cp.dv; return h;
}
GraphExpModel::ControlPoint ToControlPoint(const GraphCurve::Handle& h) {
	GraphExpModel::ControlPoint cp; cp.active = h.active; cp.tx = h.tx; cp.dv = h.dv; return cp;
}
bool SameHandle(const GraphCurve::Handle& a, const GraphCurve::Handle& b) {
	if (a.active != b.active) return false;
	return !a.active || (std::abs(a.tx - b.tx) <= 1e-6 && std::abs(a.dv - b.dv) <= 1e-6);
}
// Marker values for the graph: angle channels unwrapped against the previous marker, so a yaw
// crossing +/-180 reads as one continuous curve -- the way the camera takes it.
std::vector<double> ChannelValues(const std::vector<CamMarker>& markers, int ch) {
	std::vector<double> v(markers.size());
	for (size_t i = 0; i < markers.size(); ++i) {
		v[i] = MarkerChannel(markers[i], ch);
		if (i > 0 && GraphCurve::IsAngleChannel(ch)) v[i] = GraphCurve::Unwrap(v[i - 1], v[i]);
	}
	return v;
}
} // namespace

void GraphExpModel::Clear() {
	for (int c = 0; c < kChannelCount; ++c) {
		m_ch[c].keys.clear();
		m_ch[c].visible = true;
		m_ch[c].solo = false;
	}
	m_selected.clear();
	m_undo.clear();
	m_redo.clear();
	m_nextId = 1;
}

void GraphExpModel::SeedFromMarkers(const std::vector<CamMarker>& markers) {
	Clear();
	for (int c = 0; c < kChannelCount; ++c) {
		const std::vector<double> values = ChannelValues(markers, c);
		for (size_t i = 0; i < markers.size(); ++i) {
			Keyframe k;
			k.id = m_nextId++;
			k.tick = (double)markers[i].tick;
			k.value = values[i];
			// The path's own curve, when it has one: the graph opens on what the camera flies.
			k.cpLeft = ToControlPoint(markers[i].in[c]);
			k.cpRight = ToControlPoint(markers[i].out[c]);
			m_ch[c].keys.push_back(k);
		}
		SortChannel(c);
	}
}

bool GraphExpModel::MarkersHaveCurves(const std::vector<CamMarker>& markers) {
	for (const CamMarker& m : markers) if (m.HasCurve()) return true;
	return false;
}

bool GraphExpModel::ReconcileWithMarkers(const std::vector<CamMarker>& markers, bool smoothNewKeys) {
	bool changed = false;
	std::set<long long> keptSel;
	// A path that carries curves is the authority on handles too; one without keeps the graph's
	// own (display) handles on keys that survive.
	const bool pathCurves = MarkersHaveCurves(markers);
	for (int c = 0; c < kChannelCount; ++c) {
		std::vector<Keyframe>& old = m_ch[c].keys;
		std::vector<Keyframe> next;
		std::set<int> fresh;
		next.reserve(markers.size());
		std::vector<bool> used(old.size(), false);
		const std::vector<double> values = ChannelValues(markers, c);
		for (size_t mi = 0; mi < markers.size(); ++mi) {
			const CamMarker& m = markers[mi];
			const double v = values[mi];
			int match = -1;
			for (size_t i = 0; i < old.size(); ++i) {
				if (!used[i] && std::floor(old[i].tick + 0.5) == (double)m.tick) { match = (int)i; break; }
			}
			Keyframe k;
			if (match >= 0) {
				used[match] = true;
				k = old[match];
				if (k.tick != (double)m.tick || k.value != v) changed = true;
				if (m_selected.count(Pack(c, k.id))) keptSel.insert(Pack(c, k.id));
			} else {
				k.id = m_nextId++;
				fresh.insert(k.id);
				changed = true;
			}
			k.tick = (double)m.tick;
			k.value = v;
			if (pathCurves) {
				const ControlPoint l = ToControlPoint(m.in[c]), r = ToControlPoint(m.out[c]);
				if (!SameHandle(ToHandle(k.cpLeft), m.in[c]) || !SameHandle(ToHandle(k.cpRight), m.out[c])) changed = true;
				k.cpLeft = l; k.cpRight = r;
			}
			next.push_back(k);
		}
		for (bool u : used) if (!u) { changed = true; break; }
		old.swap(next);
		SortChannel(c);
		if (smoothNewKeys && !pathCurves) {
			for (int i = 0; i < (int)old.size(); ++i)
				if (fresh.count(old[i].id)) AutoTangent(c, i);
		}
	}
	m_selected.swap(keptSel);
	if (changed) {
		m_undo.clear();
		m_redo.clear();
	}
	return changed;
}

std::vector<CamMarker> GraphExpModel::BakeMarkers(const std::vector<CamMarker>& old, double intervalPerTick) const {
	// Whole ticks: a marker IS a whole tick, so two keys 0.8 tick apart that both round to tick
	// 100 must make one marker, not two markers sharing tick 100.
	std::vector<int> ticks;
	for (int c = 0; c < kChannelCount; ++c)
		for (const Keyframe& k : m_ch[c].keys)
			ticks.push_back((int)std::floor(k.tick + 0.5));
	std::sort(ticks.begin(), ticks.end());
	ticks.erase(std::unique(ticks.begin(), ticks.end()), ticks.end());

	std::vector<CamMarker> baked;
	baked.reserve(ticks.size());
	for (int tick : ticks) {
		const double t = (double)tick;
		CamMarker m;
		m.tick = tick;
		m.time = (intervalPerTick > 0.0) ? t * intervalPerTick : 0.0;
		for (int c = 0; c < kChannelCount; ++c) {
			double v = 0.0;
			SetMarkerChannel(m, c, SampleChannel(c, t, v) ? v : (c == 6 ? 90.0 : 0.0));
		}
		for (const CamMarker& om : old) {
			if (om.tick == tick) { m.ease = om.ease; m.speedMul = om.speedMul; break; }
		}
		baked.push_back(m);
	}

	// THE SHAPE GOES WITH IT. Each channel's handles become the markers' handles, so the camera
	// flies the curve drawn here (CamPathEval follows marker handles). A marker on a tick where this
	// channel has no key splits the channel's segment there exactly (GraphCurve::SplitAt), so the
	// curve between the keys is the same curve after the bake as before it.
	auto index = [&](double keyTick) {
		const int whole = (int)std::floor(keyTick + 0.5);
		return (int)(std::lower_bound(ticks.begin(), ticks.end(), whole) - ticks.begin());
	};
	for (int c = 0; c < kChannelCount; ++c) {
		const std::vector<Keyframe>& keys = m_ch[c].keys;
		if (keys.empty()) continue;
		// Before the first key and after the last the channel holds flat (SampleChannel), so the
		// outer handles stay inactive: a handle there would bend a segment the graph draws straight.
		SetMarkerChannel(baked[index(keys.front().tick)], c, keys.front().value);
		for (size_t i = 0; i + 1 < keys.size(); ++i) {
			const Keyframe& A = keys[i];
			const Keyframe& B = keys[i + 1];
			const int ja = index(A.tick), jb = index(B.tick);
			if (jb <= ja) continue; // two keys on one whole tick: one marker, already written
			double tL = A.tick, vL = A.value;
			GraphCurve::Handle outL = ToHandle(A.cpRight), inR = ToHandle(B.cpLeft);
			int jPrev = ja;
			for (int j = ja + 1; j < jb; ++j) {
				const GraphCurve::Split s = GraphCurve::SplitAt(tL, vL, outL, B.tick, B.value, inR, (double)ticks[j]);
				baked[jPrev].out[c] = s.outA;
				SetMarkerChannel(baked[j], c, s.value);
				baked[j].in[c] = s.inK;
				tL = (double)ticks[j]; vL = s.value; outL = s.outK; inR = s.inB;
				jPrev = j;
			}
			baked[jPrev].out[c] = outL;
			baked[jb].in[c] = inR;
			SetMarkerChannel(baked[jb], c, B.value);
		}
	}
	return baked;
}

bool GraphExpModel::SameMarkers(const std::vector<CamMarker>& a, const std::vector<CamMarker>& b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i) {
		if (a[i].tick != b[i].tick) return false;
		for (int c = 0; c < kChannelCount; ++c) {
			if (std::abs(MarkerChannel(a[i], c) - MarkerChannel(b[i], c)) > 1e-6) return false;
			if (!SameHandle(a[i].in[c], b[i].in[c]) || !SameHandle(a[i].out[c], b[i].out[c])) return false;
		}
	}
	return true;
}

void GraphExpModel::SortChannel(int ch) {
	std::stable_sort(m_ch[ch].keys.begin(), m_ch[ch].keys.end(),
		[](const Keyframe& a, const Keyframe& b) { return a.tick < b.tick; });
}

void GraphExpModel::SetSolo(int ch, bool v) {
	ch = Clamp(ch);
	// Solo is exclusive: turning one on clears the others (AE-like focus).
	if (v) for (int c = 0; c < kChannelCount; ++c) m_ch[c].solo = (c == ch);
	else m_ch[ch].solo = false;
}

bool GraphExpModel::AnySolo() const {
	for (int c = 0; c < kChannelCount; ++c) if (m_ch[c].solo) return true;
	return false;
}

bool GraphExpModel::IsEditable(int ch) const {
	ch = Clamp(ch);
	if (!m_ch[ch].visible) return false;
	if (!AnySolo()) return true;
	return m_ch[ch].solo;
}

// ---- sampling ---------------------------------------------------------------

double GraphExpModel::BezierSegment(const Keyframe& a, const Keyframe& b, double t) const {
	// The camera path evaluates the same function (CamPathEval::EvalCurves), so what the graph
	// draws is what the camera flies.
	return GraphCurve::SegmentValue(a.tick, a.value, ToHandle(a.cpRight), b.tick, b.value, ToHandle(b.cpLeft), t);
}

bool GraphExpModel::SampleChannel(int ch, double tick, double& out) const {
	ch = Clamp(ch);
	const std::vector<Keyframe>& k = m_ch[ch].keys;
	if (k.empty()) return false;
	if (tick <= k.front().tick) { out = k.front().value; return true; } // flat before first
	if (tick >= k.back().tick)  { out = k.back().value;  return true; } // flat after last
	for (size_t i = 0; i + 1 < k.size(); ++i) {
		if (tick >= k[i].tick && tick <= k[i + 1].tick) {
			const Keyframe& a = k[i];
			const Keyframe& b = k[i + 1];
			if (a.cpRight.active || b.cpLeft.active) { out = BezierSegment(a, b, tick); return true; }
			const double span = b.tick - a.tick;
			const double f = (span > 1e-9) ? (tick - a.tick) / span : 0.0;
			out = a.value + f * (b.value - a.value); // linear
			return true;
		}
	}
	out = k.back().value;
	return true;
}

void GraphExpModel::SamplePose(double tick, const double defaults[7], double out[7]) const {
	for (int c = 0; c < kChannelCount; ++c) {
		double v;
		out[c] = SampleChannel(c, tick, v) ? v : defaults[c];
	}
}

double GraphExpModel::MinTick() const {
	double mn = 0.0; bool any = false;
	for (int c = 0; c < kChannelCount; ++c)
		if (!m_ch[c].keys.empty()) { double t = m_ch[c].keys.front().tick; if (!any || t < mn) { mn = t; any = true; } }
	return mn;
}
double GraphExpModel::MaxTick() const {
	double mx = 0.0; bool any = false;
	for (int c = 0; c < kChannelCount; ++c)
		if (!m_ch[c].keys.empty()) { double t = m_ch[c].keys.back().tick; if (!any || t > mx) { mx = t; any = true; } }
	return mx;
}
int GraphExpModel::TotalKeys() const {
	int n = 0; for (int c = 0; c < kChannelCount; ++c) n += (int)m_ch[c].keys.size(); return n;
}

// ---- editing ----------------------------------------------------------------

bool GraphExpModel::FindKey(int ch, int id, int& outIndex) const {
	ch = Clamp(ch);
	for (size_t i = 0; i < m_ch[ch].keys.size(); ++i)
		if (m_ch[ch].keys[i].id == id) { outIndex = (int)i; return true; }
	return false;
}
GraphExpModel::Keyframe* GraphExpModel::FindKeyMut(int ch, int id) {
	int i; if (FindKey(ch, id, i)) return &m_ch[Clamp(ch)].keys[i]; return nullptr;
}
const GraphExpModel::Keyframe* GraphExpModel::FindKeyConst(int ch, int id) const {
	int i; if (FindKey(ch, id, i)) return &m_ch[Clamp(ch)].keys[i]; return nullptr;
}

int GraphExpModel::AddKey(int ch, double tick, double value) {
	ch = Clamp(ch);
	// Replace a keyframe already at this tick (within half a tick) instead of stacking.
	for (auto& k : m_ch[ch].keys)
		if (std::abs(k.tick - tick) < 0.5) { k.value = value; return k.id; }
	Keyframe k; k.id = m_nextId++; k.tick = tick; k.value = value;
	m_ch[ch].keys.push_back(k);
	SortChannel(ch);
	return k.id;
}

bool GraphExpModel::DeleteKey(int ch, int id) {
	ch = Clamp(ch);
	auto& v = m_ch[ch].keys;
	for (size_t i = 0; i < v.size(); ++i)
		if (v[i].id == id) { v.erase(v.begin() + i); m_selected.erase(Pack(ch, id)); return true; }
	return false;
}

void GraphExpModel::MoveKeyAbs(int ch, int id, double tick, double value) {
	ch = Clamp(ch);
	if (Keyframe* k = FindKeyMut(ch, id)) { k->tick = tick; k->value = value; SortChannel(ch); }
}

void GraphExpModel::MoveSelectedBy(double dTick, double dValue) {
	bool touched[kChannelCount] = { false };
	for (long long packed : m_selected) {
		const int ch = (int)(packed >> 32);
		const int id = (int)(unsigned)(packed & 0xffffffffLL);
		if (ch < 0 || ch >= kChannelCount) continue;
		if (Keyframe* k = FindKeyMut(ch, id)) { k->tick += dTick; k->value += dValue; touched[ch] = true; }
	}
	for (int c = 0; c < kChannelCount; ++c) if (touched[c]) SortChannel(c);
}

void GraphExpModel::SetKeyValue(int ch, int id, double value) {
	if (Keyframe* k = FindKeyMut(ch, id)) k->value = value;
}

int GraphExpModel::AlignSelectionToTick(double tick) {
	bool touched[kChannelCount] = { false };
	int moved = 0;
	for (long long packed : m_selected) {
		const int ch = (int)(packed >> 32);
		const int id = (int)(unsigned)(packed & 0xffffffffLL);
		if (ch < 0 || ch >= kChannelCount) continue;
		if (Keyframe* k = FindKeyMut(ch, id)) {
			if (k->tick != tick) { k->tick = tick; ++moved; }
			touched[ch] = true;
		}
	}
	for (int c = 0; c < kChannelCount; ++c) if (touched[c]) SortChannel(c);
	return moved;
}

void GraphExpModel::SetHandle(int ch, int id, int side, double tx, double dv, bool reflect) {
	Keyframe* k = FindKeyMut(ch, id);
	if (!k) return;
	if (tx < 0) tx = 0; if (tx > 1) tx = 1;
	ControlPoint& cpNear = (side < 0) ? k->cpLeft : k->cpRight;
	cpNear.active = true; cpNear.tx = tx; cpNear.dv = dv;
	if (reflect) {
		ControlPoint& cpFar = (side < 0) ? k->cpRight : k->cpLeft;
		cpFar.active = true; cpFar.tx = tx; cpFar.dv = -dv; // mirror the value slope, keep length
	}
}

void GraphExpModel::ClearHandles(int ch, int id) {
	if (Keyframe* k = FindKeyMut(ch, id)) { k->cpLeft = ControlPoint(); k->cpRight = ControlPoint(); }
}

void GraphExpModel::SetEase(int mode, bool selectedOnly) {
	// Flatten the chosen handle(s) to a zero-velocity (dv=0) tangent at a default 1/3 influence,
	// which is exactly an AE "Easy Ease": the curve decelerates into / accelerates out of the key.
	for (int c = 0; c < kChannelCount; ++c) {
		for (Keyframe& k : m_ch[c].keys) {
			if (selectedOnly && m_selected.count(Pack(c, k.id)) == 0) continue;
			if (mode == 0 || mode == 2) { k.cpLeft.active = true;  k.cpLeft.tx = 1.0 / 3.0;  k.cpLeft.dv = 0.0; }
			if (mode == 1 || mode == 2) { k.cpRight.active = true; k.cpRight.tx = 1.0 / 3.0; k.cpRight.dv = 0.0; }
		}
	}
}

void GraphExpModel::AutoTangent(int ch, int i) {
	std::vector<Keyframe>& keys = m_ch[Clamp(ch)].keys;
	const int n = (int)keys.size();
	if (i < 0 || i >= n) return;
	// Catmull-Rom slope from the neighbours as 1/3-influence Bezier handles. The camera path's
	// own Smooth (CameraPath::SetInterp) uses the same function, so both make the same curve.
	std::vector<double> ticks(n), values(n);
	for (int j = 0; j < n; ++j) { ticks[j] = keys[j].tick; values[j] = keys[j].value; }
	GraphCurve::Handle in, out;
	GraphCurve::AutoTangent(ticks.data(), values.data(), n, i, in, out);
	keys[i].cpLeft = ToControlPoint(in);
	keys[i].cpRight = ToControlPoint(out);
}

void GraphExpModel::SetInterpAll(bool smooth, bool selectedOnly) {
	for (int c = 0; c < kChannelCount; ++c) {
		std::vector<Keyframe>& keys = m_ch[c].keys;
		const int n = (int)keys.size();
		for (int i = 0; i < n; ++i) {
			Keyframe& k = keys[i];
			if (selectedOnly && m_selected.count(Pack(c, k.id)) == 0) continue;
			if (!smooth) { k.cpLeft = ControlPoint(); k.cpRight = ControlPoint(); continue; }
			AutoTangent(c, i);
		}
	}
}

// ---- selection --------------------------------------------------------------

void GraphExpModel::Select(int ch, int id, bool additive) {
	ch = Clamp(ch);
	if (!additive) m_selected.clear();
	m_selected.insert(Pack(ch, id));
}
void GraphExpModel::SelectAdd(int ch, int id) { m_selected.insert(Pack(Clamp(ch), id)); }
void GraphExpModel::SelectAll() {
	m_selected.clear();
	for (int c = 0; c < kChannelCount; ++c)
		for (const Keyframe& k : m_ch[c].keys) m_selected.insert(Pack(c, k.id));
}
bool GraphExpModel::IsSelected(int ch, int id) const { return m_selected.count(Pack(Clamp(ch), id)) != 0; }

bool GraphExpModel::FirstSelected(int& outCh, int& outId) const {
	if (m_selected.empty()) return false;
	long long packed = *m_selected.begin();
	outCh = (int)(packed >> 32);
	outId = (int)(unsigned)(packed & 0xffffffffLL);
	return true;
}

// ---- undo / redo ------------------------------------------------------------

GraphExpModel::Snapshot GraphExpModel::CaptureSnapshot() const {
	Snapshot s;
	for (int c = 0; c < kChannelCount; ++c) s.ch[c] = m_ch[c];
	s.sel = m_selected;
	s.nextId = m_nextId;
	return s;
}

void GraphExpModel::RestoreSnapshot(const Snapshot& s) {
	for (int c = 0; c < kChannelCount; ++c) m_ch[c] = s.ch[c];
	m_selected = s.sel;
	m_nextId = s.nextId;
}

void GraphExpModel::BeginEdit() {
	m_undo.push_back(CaptureSnapshot());
	if (m_undo.size() > 64) m_undo.erase(m_undo.begin());
	m_redo.clear(); // a fresh edit invalidates anything that was previously undone
}

void GraphExpModel::Undo() {
	if (m_undo.empty()) return;
	m_redo.push_back(CaptureSnapshot());           // remember current state so Redo can return to it
	if (m_redo.size() > 64) m_redo.erase(m_redo.begin());
	RestoreSnapshot(m_undo.back());
	m_undo.pop_back();
}

void GraphExpModel::Redo() {
	if (m_redo.empty()) return;
	m_undo.push_back(CaptureSnapshot());
	if (m_undo.size() > 64) m_undo.erase(m_undo.begin());
	RestoreSnapshot(m_redo.back());
	m_redo.pop_back();
}

} // namespace Filmmaker
