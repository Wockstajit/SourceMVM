#pragma once

// CamMarkers: the authoritative, ordered list of camera markers plus the
// path-wide settings that are persisted alongside them. Plain data (no engine
// types) so this header stays light and (de)serialization is trivial.
//
// Owned by the CameraPath facade; READ by CamPathEval (to rebuild g_CamPath) and
// CamPlayback (to evaluate). This is the "marker data" slice carved out of the
// old CameraPath god-object.

#include <string>
#include <vector>

namespace Filmmaker {

// Per-keyframe easing, applied to the OUTGOING segment's progress only. It warps
// TIMING within the segment (slow-in / slow-out), never the curve GEOMETRY, so it
// composes with the shape/timing split. IWXMVM does not expose this; it is one of
// our extras (see plan Part B).
enum class Ease { None = 0, In = 1, Out = 2, InOut = 3 };

// One camera marker. Doubles for position/angles keep the smoothing math clean.
struct CamMarker {
	double x = 0.0, y = 0.0, z = 0.0;          // world position
	double pitch = 0.0, yaw = 0.0, roll = 0.0; // view angles (degrees)
	double fov = 90.0;
	int tick = 0;                              // demo tick at placement
	double time = 0.0;                         // demo time (seconds) at placement
	// Speed multiplier for this marker's OUTGOING segment (Per-Segment mode only);
	// the last marker has no outgoing segment, so its value is ignored.
	float speedMul = 1.0f;
	Ease ease = Ease::None;                    // easing of this marker's outgoing segment
};

// Path-wide settings persisted in the SAME sidecar as the markers. Kept here so
// the on-disk schema lives in one place; the ints mirror the CameraPath enums
// (SpeedMode / Timing / Interp) to keep this header free of the facade's types.
struct PathSettings {
	int speedMode = 0;   // CameraPath::SpeedMode (0 Manual, 1 Constant, 2 PerSegment)
	int timing = 0;      // CameraPath::Timing    (0 Live, 1 Freeze)
	int interp = 0;      // CameraPath::Interp    (0 Linear, 1 Smooth/cubic)
	float constSpeed = 1.0f;
	bool autoSnap = false;
};

class CamMarkers {
public:
	int Count() const { return (int)m_markers.size(); }
	bool Empty() const { return m_markers.empty(); }
	const std::vector<CamMarker>& All() const { return m_markers; }

	const CamMarker& At(int i) const { return m_markers[i]; }
	CamMarker& At(int i) { return m_markers[i]; }
	const CamMarker& Front() const { return m_markers.front(); }
	const CamMarker& Back() const { return m_markers.back(); }

	int Selected() const { return m_selected; }
	void SetSelected(int i);                                  // clamps into range
	bool ValidIndex(int i) const { return i >= 0 && i < (int)m_markers.size(); }
	bool SelectedIsLast() const { return !m_markers.empty() && m_selected == (int)m_markers.size() - 1; }

	int Add(const CamMarker& m);                              // append, select it, return index
	bool DeleteIndex(int i);                                  // erase + fix selection; false if OOB
	void DeleteAll();
	void Restore(const std::vector<CamMarker>& markers, int selected);

	// JSON payload: markers + settings + selection in one document. The project store
	// carries it as the "campath" section of the demo's project.mvm.
	//   Serialize: writes the JSON text.
	//   Parse: false if the text is malformed (list left untouched); on a valid document
	//         the list is cleared + repopulated.
	std::string Serialize(const PathSettings& settings, int selected) const;
	bool Parse(const std::string& text, PathSettings& settings, int& selected);

private:
	std::vector<CamMarker> m_markers;
	int m_selected = -1;
};

} // namespace Filmmaker
