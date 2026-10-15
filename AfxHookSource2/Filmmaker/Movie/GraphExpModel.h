#pragma once

// GraphExpModel: the ISOLATED data model for the EXPERIMENTAL After-Effects-style camera
// graph editor. It deliberately does NOT touch the stable camera path (CameraPath /
// CamPathEval / CamMarkers); it owns its own per-channel keyframe + Bezier-handle model so
// the experiment can be deleted wholesale without affecting the regular editor.
//
// Model shape (mirrors alexharri/animation-editor's timeline model):
//   * 7 independent channels: x, y, z, pitch, yaw, roll, fov (indices match
//     CameraPath::SetChannelValue's channel order, but the data is separate).
//   * Each channel is a list of keyframes sorted by tick. Each keyframe has a stable id,
//     a tick (time) and value, and two optional Bezier control points (left/right).
//   * A control point is stored relative to its keyframe: tx in [0,1] is the fraction of
//     the segment toward the neighbour; dv is the value offset from the keyframe value.
//   * Selection is a set of keyframe ids (stable across re-sorts), so dragging that
//     reorders keyframes never loses the selection.
//
// Sampling: SampleChannel does flat extrapolation before the first / after the last
// keyframe (AE behaviour), linear between keyframes with no active handles, and cubic
// Bezier (solved X(s)=t by bisection, robust for flat segments) when a handle is active.
//
// Threading: all access is from the main/UI thread (the console dispatch + the HUD
// RunFrame), same as CameraPath, so no locking is needed.

#include <vector>
#include <set>

namespace Filmmaker {

struct CamMarker; // fwd; SeedFromMarkers reads the stable marker list read-only

class GraphExpModel {
public:
	static const int kChannelCount = 7; // x,y,z,pitch,yaw,roll,fov

	struct ControlPoint {
		bool active = false; // false => this side is linear (no handle)
		double tx = 0.33;    // 0..1 fraction of the segment toward the neighbour
		double dv = 0.0;     // value offset from the owning keyframe's value
	};
	struct Keyframe {
		int id = 0;
		double tick = 0.0;
		double value = 0.0;
		ControlPoint cpLeft;  // toward the previous keyframe
		ControlPoint cpRight; // toward the next keyframe
	};
	struct Channel {
		std::vector<Keyframe> keys; // always kept sorted by tick
		bool visible = true;
		bool solo = false;
	};

	// Replace the model with keyframes sampled (read-only) from the stable marker list:
	// every marker contributes one keyframe to every channel (value = that channel at the
	// marker). This is how the experiment starts from the user's real camera path.
	void SeedFromMarkers(const std::vector<CamMarker>& markers);
	void Clear();

	int ChannelCount() const { return kChannelCount; }
	const Channel& GetChannel(int ch) const { return m_ch[Clamp(ch)]; }
	Channel& GetChannelMut(int ch) { return m_ch[Clamp(ch)]; }

	// --- visibility ---
	void SetVisible(int ch, bool v) { m_ch[Clamp(ch)].visible = v; }
	void SetSolo(int ch, bool v);     // solo hides editing focus to one channel
	bool AnySolo() const;
	bool IsEditable(int ch) const;    // visible AND (no solo OR this is the solo)

	// --- value sampling (used by the live-drive pose push) ---
	// Returns true and writes the sampled value if the channel has >=1 keyframe; false
	// otherwise (caller keeps the current camera value for that axis).
	bool SampleChannel(int ch, double tick, double& out) const;
	// Fill out[0..6]; channels with no keyframes keep defaults[ch].
	void SamplePose(double tick, const double defaults[7], double out[7]) const;

	double MinTick() const; // across all channels (0 if empty)
	double MaxTick() const;
	int TotalKeys() const;

	// --- editing (all by stable keyframe id) ---
	int  AddKey(int ch, double tick, double value); // returns id (replaces if same tick)
	bool DeleteKey(int ch, int id);
	bool FindKey(int ch, int id, int& outIndex) const;
	Keyframe* FindKeyMut(int ch, int id);
	const Keyframe* FindKeyConst(int ch, int id) const;

	// Move one keyframe to an absolute tick/value (re-sorts; id preserved).
	void MoveKeyAbs(int ch, int id, double tick, double value);
	// Shift every selected keyframe by (dTick, dValue) (used for drag of a selection).
	void MoveSelectedBy(double dTick, double dValue);
	// Snap every selected keyframe to the SAME tick (vertical alignment across channels):
	// each key keeps its own value and Bezier handles, only its time changes. Used by the
	// right-click Align action ("make these keys happen at the same moment"). Returns the
	// number of keyframes moved.
	int AlignSelectionToTick(double tick);
	// Set a keyframe's value only (used by the number field / typing).
	void SetKeyValue(int ch, int id, double value);

	// Bezier handle edit. side: -1 = left, +1 = right. tx/dv are absolute handle params.
	void SetHandle(int ch, int id, int side, double tx, double dv, bool reflect);
	void ClearHandles(int ch, int id);

	// Apply an easing preset by flattening keyframe handle(s) to a flat (zero-velocity) tangent.
	// mode: 0 = ease in (incoming/left), 1 = ease out (outgoing/right), 2 = ease in+out (both).
	// selectedOnly=true restricts it to the current selection (right-click menu); false applies it
	// to every keyframe in every channel (the inspector's path-wide Ease button).
	void SetEase(int mode, bool selectedOnly);

	// Set the interpolation of EVERY keyframe in EVERY channel at once (the graph's own Smooth /
	// Linear toggle, independent of the stable camera path). smooth=true builds Catmull-Rom-style
	// auto tangents so the curves render smooth; smooth=false clears all handles back to straight
	// lines. selectedOnly restricts it to the current selection.
	void SetInterpAll(bool smooth, bool selectedOnly);

	// --- selection (ids) ---
	void ClearSelection() { m_selected.clear(); }
	void Select(int ch, int id, bool additive);
	void SelectAdd(int ch, int id);
	void SelectAll(); // every keyframe in every channel (Ctrl+A)
	bool IsSelected(int ch, int id) const;
	int  SelectionCount() const { return (int)m_selected.size(); }
	// First selected (ch,id) or false. Used by the inspector / number fields.
	bool FirstSelected(int& outCh, int& outId) const;

	// --- undo / redo (whole-model snapshots; one per drag gesture) ---
	void BeginEdit();   // push a snapshot (call once at gesture start); clears the redo stack
	void EndEdit() {}    // symmetry / future use
	void Undo();         // restore the last snapshot (and push current onto the redo stack)
	void Redo();         // re-apply the last undone snapshot
	bool CanUndo() const { return !m_undo.empty(); }
	bool CanRedo() const { return !m_redo.empty(); }

private:
	static int Clamp(int ch) { return (ch < 0) ? 0 : (ch >= kChannelCount ? kChannelCount - 1 : ch); }
	void SortChannel(int ch);
	double BezierSegment(const Keyframe& a, const Keyframe& b, double t) const;

	// Selection key packs (channel, id) so the same id in different channels is distinct.
	static long long Pack(int ch, int id) { return ((long long)ch << 32) ^ (unsigned)id; }

	Channel m_ch[kChannelCount];
	std::set<long long> m_selected;
	int m_nextId = 1;

	struct Snapshot { Channel ch[kChannelCount]; std::set<long long> sel; int nextId; };
	Snapshot CaptureSnapshot() const;
	void RestoreSnapshot(const Snapshot& s);
	std::vector<Snapshot> m_undo;
	std::vector<Snapshot> m_redo;
};

} // namespace Filmmaker
