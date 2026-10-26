#pragma once

// Where a workspace's pieces go, by CS2's rules, so the CS:S Camera Editor is laid out like CS2's.
//
// Everything is in CS2's stage units: the UI is designed on a 1080-tall stage and scaled to the
// window by height, so the stage is as wide as the window's aspect makes it (1920 at 16:9). The
// rules come from CameraEditorJs.h (inspector and preview) and TimelineDockRibbonJs.h
// (mvmDockMeasure); the inspector width and dock heights are shared with CS2 through
// InspectorWidthMath.h and TimelineDock.h.
//
// Engine-free; Automation/tests/source-workspace-layout-math-tests.cpp pins it.

#include "UiTypes.h"

#include "../../../AfxHookSource2/Filmmaker/Movie/TimelineDock.h"
#include "../../../AfxHookSource2/Filmmaker/Panorama/InspectorWidthMath.h"

#include <cmath>

namespace SourceFilmmaker {
namespace Ui {

constexpr float kStageHeight = 1080.0f;
// The bottom editor's ribbon (REGULAR TIMELINE / TIMELINE tabs) and the gap above the editor.
constexpr float kDockRibbon = (float)Filmmaker::kTimelineDockRibbonHeight;
constexpr float kDockGap = 4.0f;
// The TIMELINE dock's own height under the ribbon.
constexpr float kTimelineEditorHeight =
	(float)(Filmmaker::kTimelineDockHeightTimeline - Filmmaker::kTimelineDockRibbonHeight);
// The demo bar's design height. CS2 measures its native bar and falls back to 96; CS:S has no native
// bar, so its own bar is drawn at that height.
constexpr float kDemoBarHeight = 96.0f;

struct Stage {
	float scale = 1;       // client px per stage px
	float width = 0;       // stage px
	float height = kStageHeight;
};

inline Stage StageFor(float clientW, float clientH) {
	Stage s;
	if (!(clientW > 0) || !(clientH > 0)) return s;
	s.scale = clientH / kStageHeight;
	s.width = clientW / s.scale;
	return s;
}

inline Rect ToClient(const Rect& r, const Stage& s) {
	return MakeRect(r.x0 * s.scale, r.y0 * s.scale, r.x1 * s.scale, r.y1 * s.scale);
}

// CS2's mvmDockMeasure: the band the docked editor and its ribbon take at the bottom. editorTop is
// the docked editor's height (the demo bar or the TIMELINE dock).
struct DockMeasure {
	float bottomH = 0;  // ribbon + gap + editor, capped so 160 px of screen always stay above it
	float dockTopY = 0; // top of that band
	float tabY = 0;     // top of the ribbon
};

inline DockMeasure MeasureDock(float stageH, float editorTop, float floor = 0) {
	DockMeasure m;
	const float cap = stageH - 160.0f > 0 ? stageH - 160.0f : 0.0f;
	float wanted = editorTop + kDockGap + kDockRibbon;
	if (wanted > cap) wanted = cap;
	m.bottomH = wanted > kDockRibbon + floor ? wanted : kDockRibbon + floor;
	m.dockTopY = stageH - m.bottomH > 0 ? stageH - m.bottomH : 0;
	const float tab = stageH - editorTop - kDockGap - kDockRibbon;
	m.tabY = tab > m.dockTopY ? tab : m.dockTopY;
	return m;
}

// The largest rect with the game's aspect that fits inside area, centred, on whole stage pixels.
// CameraEditorJs.h sizes the live preview this way, so the letterbox is even on all sides.
inline Rect FitPreview(const Rect& area, float aspect) {
	if (!area.Valid() || !(aspect > 0)) return Rect();
	float pw = area.W(), ph = pw / aspect;
	if (ph > area.H()) { ph = area.H(); pw = ph * aspect; }
	pw = std::floor(pw);
	ph = std::floor(ph);
	if (!(pw > 0) || !(ph > 0)) return Rect();
	float ox = area.x0 + std::floor((area.W() - pw) / 2); if (ox < area.x0) ox = area.x0;
	float oy = area.y0 + std::floor((area.H() - ph) / 2); if (oy < area.y0) oy = area.y0;
	return MakeRect(ox, oy, ox + pw, oy + ph);
}

struct WorkspaceLayout {
	Stage stage;
	float inspectorW = 0;
	Rect inspector; // right column, full height
	Rect ribbon;    // the dock's tab ribbon, left of the inspector
	Rect dock;      // the docked editor, flush at the bottom, left of the inspector
	Rect area;      // left of the inspector, above the ribbon
	Rect preview;   // the game, fitted into area at the window's aspect
};

// The Camera Editor's layout for a window of clientW x clientH, the operator's stored inspector
// width, and the docked editor's height (kDemoBarHeight or kTimelineEditorHeight).
inline WorkspaceLayout CameraEditorLayout(float clientW, float clientH, double storedInspectorW, float editorTop) {
	WorkspaceLayout l;
	l.stage = StageFor(clientW, clientH);
	if (!(l.stage.width > 0)) return l;
	const double stored = Filmmaker::InspectorWidthMath::ClampStored(storedInspectorW);
	l.inspectorW = (float)std::floor(Filmmaker::InspectorWidthMath::Resolve(stored, l.stage.width));
	const float sw = l.stage.width, sh = l.stage.height;
	const float left = sw - l.inspectorW > 0 ? sw - l.inspectorW : 0;
	const DockMeasure dm = MeasureDock(sh, editorTop);
	l.inspector = MakeRect(left, 0, sw, sh);
	l.ribbon = MakeRect(0, dm.tabY, left, dm.tabY + kDockRibbon);
	l.dock = MakeRect(0, sh - editorTop, left, sh);
	l.area = MakeRect(0, 0, left, sh - dm.bottomH);
	l.preview = FitPreview(l.area, clientW / clientH);
	return l;
}

} // namespace Ui
} // namespace SourceFilmmaker
