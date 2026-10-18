#include "PanoramaPerfProbe.h"

#include "../Debug/MvmDebugLog.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace Filmmaker {

namespace {

// One window of frames per report. 120 at 60 fps is two seconds: long enough that a single
// hitch does not dominate the average, short enough to see a toggle take effect while testing.
const int kWindow = 120;
const int kMaxSlots = 64;

struct Slot {
	const char* name = nullptr;
	long long ns = 0;      // accumulated over the window
	long long peakNs = 0;  // worst single frame in the window
	long long frameNs = 0; // accumulated within the CURRENT frame, for the peak
};

bool s_enabled = false;
Slot s_slots[kMaxSlots];
int s_slotCount = 0;
int s_frames = 0;
long long s_windowStart = 0;
long long s_frameStart = 0;
long long s_worstFrameNs = 0;
std::string s_report;

long long QpcFreq() {
	static long long s_freq = [] {
		LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (long long)f.QuadPart;
	}();
	return s_freq;
}

Slot* Find(const char* name) {
	for (int i = 0; i < s_slotCount; ++i)
		if (s_slots[i].name == name || 0 == strcmp(s_slots[i].name, name))
			return &s_slots[i];
	if (s_slotCount >= kMaxSlots) return nullptr;
	Slot* s = &s_slots[s_slotCount++];
	s->name = name; s->ns = 0; s->peakNs = 0; s->frameNs = 0;
	return s;
}

} // namespace

long long PanoramaPerf_NowNs();

long long PanoramaPerf_NowNs() {
	LARGE_INTEGER c; QueryPerformanceCounter(&c);
	return (long long)((double)c.QuadPart * 1e9 / (double)QpcFreq());
}

void PanoramaPerf_SetEnabled(bool enable) {
	if (s_enabled == enable) return;
	s_enabled = enable;
	s_slotCount = 0;
	s_frames = 0;
	s_windowStart = 0;
	s_worstFrameNs = 0;
	s_report.clear();
}

bool PanoramaPerf_Enabled() { return s_enabled; }

bool PanoramaPerf_Active() { return s_enabled; }

void PanoramaPerf_Add(const char* name, long long ns) {
	if (!s_enabled || !name) return;
	Slot* s = Find(name);
	if (!s) return;
	s->ns += ns;
	s->frameNs += ns;
}

void PanoramaPerf_EndFrame() {
	if (!s_enabled) return;
	const long long now = PanoramaPerf_NowNs();

	// Per-frame peaks: fold this frame's per-slot total into its peak, then reset.
	long long thisFrameNs = 0;
	for (int i = 0; i < s_slotCount; ++i) {
		if (s_slots[i].frameNs > s_slots[i].peakNs) s_slots[i].peakNs = s_slots[i].frameNs;
		if (!strchr(s_slots[i].name, '.')) thisFrameNs += s_slots[i].frameNs;
		s_slots[i].frameNs = 0;
	}
	if (thisFrameNs > s_worstFrameNs) s_worstFrameNs = thisFrameNs;

	if (0 == s_windowStart) { s_windowStart = now; s_frameStart = now; s_frames = 0; return; }
	s_frameStart = now;
	if (++s_frames < kWindow) return;

	const double windowMs = (double)(now - s_windowStart) / 1e6;
	const double perFrameMs = windowMs / (double)s_frames;
	char buf[4096];
	int used = _snprintf_s(buf, _TRUNCATE,
		"frames=%d frame=%.2fms (%.0f fps) ourPeak=%.2fms |",
		s_frames, perFrameMs, perFrameMs > 0.0 ? 1000.0 / perFrameMs : 0.0,
		(double)s_worstFrameNs / 1e6);
	if (used < 0) used = 0;
	// Slots NEST: "camtl" is the whole pump and "camtl.state"/"camtl.js" are parts of it, so summing
	// every slot counts the inner ones twice (it read 72% of frame for work that was really 38%).
	// A dot in the name marks a sub-slot; only top-level slots go into the total.
	double ourTotalMs = 0.0;
	for (int i = 0; i < s_slotCount; ++i) {
		const double avgMs = (double)s_slots[i].ns / 1e6 / (double)s_frames;
		if (!strchr(s_slots[i].name, '.')) ourTotalMs += avgMs;
		if (avgMs < 0.005) continue; // below the noise floor of a QPC pair
		const int n = _snprintf_s(buf + used, sizeof(buf) - used, _TRUNCATE,
			" %s=%.2f/%.2f", s_slots[i].name, avgMs, (double)s_slots[i].peakNs / 1e6);
		if (n <= 0) break;
		used += n;
	}
	_snprintf_s(buf + used, sizeof(buf) - used, _TRUNCATE,
		" | ourTotal=%.2fms (%.0f%% of frame)",
		ourTotalMs, perFrameMs > 0.0 ? 100.0 * ourTotalMs / perFrameMs : 0.0);

	s_report = buf;
	// Never deduplicated: the payload is a measurement, and two identical windows are two facts.
	MvmDebugLog_LinefAlways("perf.ui", "%s", buf);

	for (int i = 0; i < s_slotCount; ++i) { s_slots[i].ns = 0; s_slots[i].peakNs = 0; }
	s_frames = 0;
	s_worstFrameNs = 0;
	s_windowStart = now;
}

const char* PanoramaPerf_LastReport() { return s_report.c_str(); }

PanoramaPerf_Scope::PanoramaPerf_Scope(const char* name)
	: m_name(name), m_start(s_enabled ? PanoramaPerf_NowNs() : 0) {}

PanoramaPerf_Scope::~PanoramaPerf_Scope() {
	if (!s_enabled || 0 == m_start) return;
	PanoramaPerf_Add(m_name, PanoramaPerf_NowNs() - m_start);
}

} // namespace Filmmaker
