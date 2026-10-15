#pragma once

// <sstream> in full, not a forward declaration: std::ostringstream is a TYPEDEF, so declaring it
// by hand is ill-formed.
#include <sstream>

namespace Filmmaker {

// THE WIDTH OF THE RIGHT-HAND INSPECTOR COLUMN, in CSS pixels, dragged by the operator.
//
// Three separate Panorama panels need this number and they are not in one script scope:
//
//   * CONFIG draws the column and everything pinned to it (fades, footer, action row);
//   * the CAMERA EDITOR draws its own column the same width, and sizes the scaled preview
//     viewport to what is left of the screen;
//   * the docked TIMELINE (regular / camera / graph) sizes its bar to what is left of the
//     screen, so a column that moved without telling it leaves the bar overlapping or short.
//
// Before this existed each of the three carried its own `520` with a "keep in sync with ..."
// comment, which is a promise the compiler cannot check -- and the timeline's copy is exactly
// the one that produced a bar running under the inspector. One value, read from one place.
//
// It is stored as the UI preference "inspectorW" (FilmmakerSettingsStore), so it persists across
// demos and sessions like every other piece of panel state, and the panels write it with the
// console command that already exists for UI preferences:
//
//     mirv_filmmaker settings ui inspectorW <px>
//
// There is deliberately no setter here. The store owns the value; this is the reader and the
// clamp, so no caller can persist a width the panels would then refuse to honour.

// Hard bounds on the stored value. The lower bound is where the two-per-row value tiles stop
// fitting side by side; the upper bound is a sanity stop for a stored value, not the real limit
// -- see Resolve(), which additionally caps against the screen actually in use.
double InspectorWidth_Min();
double InspectorWidth_Max();
double InspectorWidth_Default();

// The operator's stored width, clamped to [Min, Max]. Independent of the current resolution.
double InspectorWidth_Get();

// The width to actually lay out at on a screen `screenW` CSS px wide: the stored width, capped
// so the column can never take the whole screen and leave no preview. A caller with no width yet
// (the first frames after a build, when the panel still reports 0) passes 0 and gets the stored
// width back unclamped by screen.
double InspectorWidth_Resolve(double screenW);

// Appends `,"inspW":<n>` -- the stored width. Panels apply their own screen cap in JS, because
// only they know the size of the surface they were laid out on.
void InspectorWidth_AppendJson(std::ostringstream& o);

} // namespace Filmmaker
