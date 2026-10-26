#pragma once

// The Camera Editor's PATH CAMERA tab, CS2's (CameraEditorPathInspectorJs.h) in CS:S paint:
//
//   rail   KEYS: one row per camera (CAMERA n, its tick, the curve, the selected one's FOV), all
//          opening the one card; + Add camera under the list. With none, one row says to press K.
//   card   SELECTED CAMERA, its header carrying Clear all and + Add, then:
//            the camera switcher (previous, which one of how many and its tick, next);
//            THIS CAMERA: Reposition, Move to playhead, Delete;
//            LENS: FOV and roll of the selected camera;
//            CURVE · WHOLE PATH: Linear or Smooth, and the selected camera's ease;
//            SPEED: Manual, Per-segment or Constant, the speed amount, Live or Freeze timing.
//
// The keys strip under the inspector and the Clear all dialog are drawn here too. Engine-free: it
// draws from a model and returns what was asked for; the host applies it (SourceCameraPath.h in the
// game, a stand-in in the browser mockup).

#include "InspectorCards.h"

#include <vector>

namespace SourceFilmmaker {
namespace Ui {

struct PathInspectorKey {
	int tick = 0;
	double fov = 90;
	double roll = 0;
	int ease = 0;          // 0 none, 1 in, 2 out, 3 in/out
	float speedMul = 1.0f; // its outgoing segment (Per-segment)
};

struct PathInspectorModel {
	std::vector<PathInspectorKey> keys;
	int selected = -1;
	int interp = 0;        // 0 Linear, 1 Smooth
	int speedMode = 0;     // 0 Manual, 1 Constant, 2 Per-segment
	int timing = 0;        // 0 Live, 1 Freeze
	float constSpeed = 1.0f;
	int tick = 0;          // the playhead, for Move to playhead
	bool repositioning = false;
};

struct PathInspectorAction {
	enum class Kind {
		Select,          // index
		SelectDelta,     // index is the step
		Add,
		ClearAll,        // confirmed in the dialog
		Reposition,
		MoveToPlayhead,  // index to tick
		Delete,          // index
		EditBegin,
		EditValue,       // index, channel (CS2's: 5 roll, 6 FOV), value
		EditEnd,
		Interp,          // index 0 Linear, 1 Smooth
		Ease,            // index, value as the ease
		SpeedMode,       // index 0 Manual, 1 Constant, 2 Per-segment
		SpeedMul,        // index, value
		ConstSpeed,      // value
		Timing,          // index 0 Live, 1 Freeze
	};
	Kind kind = Kind::Select;
	int index = -1;
	int channel = 0;
	int tick = 0;
	double value = 0;
};

struct PathInspectorState {
	bool confirmClear = false; // the Clear all dialog is up
};

// The tab's rail and card, into rail and pane (client px) of the inspector's body.
void DrawPathInspector(PathInspectorState& state, const PathInspectorModel& model, const FontSet& fonts, float s,
	const Rect& rail, const Rect& pane, std::vector<PathInspectorAction>& actions);

// The keys strip pinned under the body on this tab, and the height it takes at width.
float PathKeyStripHeight(const FontSet& fonts, float s, float width);
void DrawPathKeyStrip(ImDrawList* draw, const FontSet& fonts, float s, const Rect& r);

// The Clear all dialog over the whole window, while state.confirmClear. Draw it last.
void DrawPathClearConfirm(PathInspectorState& state, const FontSet& fonts, float s, float clientW, float clientH,
	std::vector<PathInspectorAction>& actions);

} // namespace Ui
} // namespace SourceFilmmaker
