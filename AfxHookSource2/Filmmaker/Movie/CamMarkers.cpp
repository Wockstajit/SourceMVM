#include "CamMarkers.h"

#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../../../Shared/AfxConsole.h"

namespace Filmmaker {

void CamMarkers::SetSelected(int i) {
	if (m_markers.empty()) { m_selected = -1; return; }
	if (i < 0) i = 0;
	if (i >= (int)m_markers.size()) i = (int)m_markers.size() - 1;
	m_selected = i;
}

int CamMarkers::Add(const CamMarker& m) {
	// Keep the list ordered by tick. Placing a marker while the playhead sits BETWEEN
	// two existing markers must insert it there (and renumber the later markers), not
	// append it to the end. Insert before the first marker whose tick is strictly
	// greater, so a marker placed between #3 and #4 becomes the new #4 and everything
	// after shifts down. The rest of the path math (MoveKey clamping, segment timing,
	// eval) assumes this tick ordering.
	int idx = (int)m_markers.size();
	for (int i = 0; i < (int)m_markers.size(); ++i) {
		if (m_markers[i].tick > m.tick) { idx = i; break; }
	}
	m_markers.insert(m_markers.begin() + idx, m);
	m_selected = idx;
	return m_selected;
}

bool CamMarkers::DeleteIndex(int i) {
	if (i < 0 || i >= (int)m_markers.size())
		return false;
	m_markers.erase(m_markers.begin() + i);
	if (m_markers.empty())
		m_selected = -1;
	else if (m_selected >= (int)m_markers.size())
		m_selected = (int)m_markers.size() - 1;
	return true;
}

void CamMarkers::DeleteAll() {
	m_markers.clear();
	m_selected = -1;
}

void CamMarkers::Restore(const std::vector<CamMarker>& markers, int selected) {
	m_markers = markers;
	SetSelected(selected);
}

std::string CamMarkers::Serialize(const PathSettings& st, int selected) const {
	JsonBuilder jb;
	jb.BeginObject();
	jb.IntField("version", 3); // 2 added per-marker "ease", 3 the graph's per-channel "curve" handles
	jb.IntField("speedMode", st.speedMode);
	jb.IntField("timing", st.timing);
	jb.IntField("interp", st.interp);
	jb.DoubleField("constSpeed", st.constSpeed);
	jb.IntField("autoSnap", st.autoSnap ? 1 : 0);
	jb.IntField("selected", selected);
	jb.Key("markers");
	jb.BeginArray();
	for (const CamMarker& m : m_markers) {
		jb.BeginObject();
		jb.DoubleField("x", m.x); jb.DoubleField("y", m.y); jb.DoubleField("z", m.z);
		jb.DoubleField("pitch", m.pitch); jb.DoubleField("yaw", m.yaw); jb.DoubleField("roll", m.roll);
		jb.DoubleField("fov", m.fov);
		jb.IntField("tick", m.tick);
		jb.DoubleField("time", m.time);
		jb.DoubleField("speedmul", m.speedMul);
		jb.IntField("ease", (int)m.ease);
		if (m.HasCurve()) {
			// Per channel: [in active, in tx, in dv, out active, out tx, out dv].
			jb.Key("curve");
			jb.BeginArray();
			for (int c = 0; c < 7; ++c) {
				jb.BeginArray();
				jb.ValueInt(m.in[c].active ? 1 : 0); jb.ValueDouble(m.in[c].tx); jb.ValueDouble(m.in[c].dv);
				jb.ValueInt(m.out[c].active ? 1 : 0); jb.ValueDouble(m.out[c].tx); jb.ValueDouble(m.out[c].dv);
				jb.EndArray();
			}
			jb.EndArray();
		}
		jb.EndObject();
	}
	jb.EndArray();
	jb.EndObject();
	return jb.Str();
}

bool CamMarkers::Parse(const std::string& text, PathSettings& st, int& selected) {
	JsonValue root;
	if (!JsonParse(text, root) || root.type != JsonValue::Type::Object) {
		advancedfx::Warning("[campath] bad campath section\n");
		return false;
	}

	if (const JsonValue* v = root.Find("speedMode")) st.speedMode = v->AsInt(0);
	if (const JsonValue* v = root.Find("timing")) st.timing = v->AsInt(0);
	if (const JsonValue* v = root.Find("interp")) st.interp = v->AsInt(0);
	if (const JsonValue* v = root.Find("constSpeed")) st.constSpeed = (float)v->AsNumber(1.0);
	if (const JsonValue* v = root.Find("autoSnap")) st.autoSnap = (v->AsInt(0) != 0);
	int sel = -1; if (const JsonValue* v = root.Find("selected")) sel = v->AsInt(-1);

	m_markers.clear();
	if (const JsonValue* arr = root.Find("markers")) {
		if (arr->type == JsonValue::Type::Array) {
			for (const JsonValue& mo : arr->arr) {
				if (mo.type != JsonValue::Type::Object) continue;
				auto num = [&](const char* k, double d) -> double {
					const JsonValue* x = mo.Find(k); return x ? x->AsNumber(d) : d;
				};
				CamMarker m;
				m.x = num("x", 0); m.y = num("y", 0); m.z = num("z", 0);
				m.pitch = num("pitch", 0); m.yaw = num("yaw", 0); m.roll = num("roll", 0);
				m.fov = num("fov", 90);
				const JsonValue* tk = mo.Find("tick"); m.tick = tk ? tk->AsInt(0) : 0;
				m.time = num("time", 0);
				m.speedMul = (float)num("speedmul", 1.0);
				const JsonValue* ev = mo.Find("ease"); m.ease = (Ease)(ev ? ev->AsInt(0) : 0);
				if (const JsonValue* cv = mo.Find("curve")) {
					if (cv->type == JsonValue::Type::Array) {
						for (int c = 0; c < 7 && c < (int)cv->arr.size(); ++c) {
							const JsonValue& h = cv->arr[c];
							if (h.type != JsonValue::Type::Array || h.arr.size() < 6) continue;
							m.in[c].active = h.arr[0].AsInt(0) != 0;  m.in[c].tx = h.arr[1].AsNumber(1.0 / 3.0);  m.in[c].dv = h.arr[2].AsNumber(0.0);
							m.out[c].active = h.arr[3].AsInt(0) != 0; m.out[c].tx = h.arr[4].AsNumber(1.0 / 3.0); m.out[c].dv = h.arr[5].AsNumber(0.0);
						}
					}
				}
				m_markers.push_back(m);
			}
		}
	}

	if (sel >= (int)m_markers.size()) sel = (int)m_markers.size() - 1;
	m_selected = m_markers.empty() ? -1 : (sel < 0 ? 0 : sel);
	selected = m_selected;
	return true;
}

} // namespace Filmmaker
