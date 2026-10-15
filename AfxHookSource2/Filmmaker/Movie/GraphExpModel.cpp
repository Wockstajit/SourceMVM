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
	for (size_t i = 0; i < markers.size(); ++i) {
		const CamMarker& m = markers[i];
		for (int c = 0; c < kChannelCount; ++c) {
			Keyframe k;
			k.id = m_nextId++;
			k.tick = (double)m.tick;
			k.value = MarkerChannel(m, c);
			m_ch[c].keys.push_back(k);
		}
	}
	for (int c = 0; c < kChannelCount; ++c) SortChannel(c);
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
	const double t0 = a.tick, t1 = b.tick;
	const double span = (t1 - t0);
	if (span <= 1e-9) return b.value;

	// Control points: right handle of a, left handle of b. Clamp tx into the segment so
	// X(s) stays monotonic and the bisection below is well-defined (also handles flat
	// value segments without the special-casing the reference needs).
	double txR = a.cpRight.active ? a.cpRight.tx : (1.0 / 3.0);
	double txL = b.cpLeft.active ? b.cpLeft.tx : (1.0 / 3.0);
	if (txR < 0) txR = 0; if (txR > 1) txR = 1;
	if (txL < 0) txL = 0; if (txL > 1) txL = 1;

	const double p0x = t0,                  p0y = a.value;
	const double p1x = t0 + txR * span,     p1y = a.value + (a.cpRight.active ? a.cpRight.dv : 0.0);
	const double p2x = t1 - txL * span,     p2y = b.value + (b.cpLeft.active ? b.cpLeft.dv : 0.0);
	const double p3x = t1,                  p3y = b.value;

	// Solve cubic Bezier X(s) = t for s in [0,1] by bisection (X is monotonic given the
	// clamped control-point times), then evaluate Y(s).
	auto bez = [](double a0, double a1, double a2, double a3, double s) {
		const double u = 1.0 - s;
		return u * u * u * a0 + 3.0 * u * u * s * a1 + 3.0 * u * s * s * a2 + s * s * s * a3;
	};
	double lo = 0.0, hi = 1.0, s = (t - t0) / span;
	for (int i = 0; i < 32; ++i) {
		s = 0.5 * (lo + hi);
		const double x = bez(p0x, p1x, p2x, p3x, s);
		if (x < t) lo = s; else hi = s;
	}
	return bez(p0y, p1y, p2y, p3y, s);
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

void GraphExpModel::SetInterpAll(bool smooth, bool selectedOnly) {
	for (int c = 0; c < kChannelCount; ++c) {
		std::vector<Keyframe>& keys = m_ch[c].keys;
		const int n = (int)keys.size();
		for (int i = 0; i < n; ++i) {
			Keyframe& k = keys[i];
			if (selectedOnly && m_selected.count(Pack(c, k.id)) == 0) continue;
			if (!smooth) { k.cpLeft = ControlPoint(); k.cpRight = ControlPoint(); continue; }
			// Catmull-Rom-style slope from the neighbours (one-sided / flat at the ends), converted
			// to cubic-Bezier handles at the standard 1/3 influence: right handle leans toward the
			// next key, left toward the previous, both on the same tangent line through this key.
			double slope = 0.0;
			if (i > 0 && i < n - 1) {
				double dt = keys[i + 1].tick - keys[i - 1].tick;
				if (dt > 1e-9) slope = (keys[i + 1].value - keys[i - 1].value) / dt;
			} else if (i == 0 && n > 1) {
				double dt = keys[1].tick - keys[0].tick;
				if (dt > 1e-9) slope = (keys[1].value - keys[0].value) / dt;
			} else if (i == n - 1 && n > 1) {
				double dt = keys[n - 1].tick - keys[n - 2].tick;
				if (dt > 1e-9) slope = (keys[n - 1].value - keys[n - 2].value) / dt;
			}
			if (i < n - 1) {
				double spanR = keys[i + 1].tick - keys[i].tick;
				k.cpRight.active = true; k.cpRight.tx = 1.0 / 3.0; k.cpRight.dv = slope * spanR / 3.0;
			} else { k.cpRight = ControlPoint(); }
			if (i > 0) {
				double spanL = keys[i].tick - keys[i - 1].tick;
				k.cpLeft.active = true; k.cpLeft.tx = 1.0 / 3.0; k.cpLeft.dv = -slope * spanL / 3.0;
			} else { k.cpLeft = ControlPoint(); }
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
