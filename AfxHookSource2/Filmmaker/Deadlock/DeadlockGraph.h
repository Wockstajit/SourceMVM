#pragma once

// The graph editor: per-channel keyframe curves over the same shot the timeline shows.
//
// The curve model is the shared, dependency-free GraphExpModel -- seven channels (x, y, z, pitch,
// yaw, roll, fov), stable keyframe ids, Bezier handles, undo snapshots. None of it is CS2-specific,
// so it is reused rather than rewritten.
//
// Two things are Deadlock's own here.
//
// First, the GEOMETRY LIVES IN C++. Panorama gives an in-game HUD context no mouse-move event at
// all, so a drag has to be driven from an OS-side cursor probe -- which means C++ has to know
// where the plot rectangle is on screen. Rather than have JavaScript measure its own layout and
// report it back (a round trip that is stale by definition and was a real source of trouble on the
// CS2 side), C++ owns the rectangle, tells JavaScript where to draw, and converts cursor pixels to
// tick and value itself. One owner, no round trip.
//
// Second, the playhead is NOT this panel's. It belongs to DeadlockShot, because the timeline and
// the graph are two views of one shot: a scrub in either must be the same scrub, or they silently
// disagree the moment one of them is dragged.

#include <string>

namespace Filmmaker {

// Rebuilds the curve model from the shot's markers. Called when the graph workspace opens and
// whenever the marker list changes underneath it.
void Graph_SeedFromShot();

// Switch authored curves/history to this replay; empty means the menu. Stops drive/gestures.
void Graph_SetReplay(const std::wstring& demo);

void Graph_Clear();

// Channel visibility. Index order matches the model: x, y, z, pitch, yaw, roll, fov.
void Graph_ToggleChannel(int channel);

// Show only this channel. Soloing the last visible channel restores every channel instead, so the
// gesture always has a way back.
void Graph_SoloChannel(int channel);
bool Graph_ChannelVisible(int channel);
const char* Graph_ChannelName(int channel);

// Called every MAIN-thread frame while the graph workspace is open. Polls the cursor probe, runs
// any active drag, and drives the camera when the graph owns the view.
void Graph_RunMainThreadFrame();

// True while the graph is claiming the camera, so nothing else writes the pose.
bool Graph_OwnsView();

// Whether the graph drives the camera from its own curves instead of the shot's path. Off by
// default: the graph is an editing surface first, and a panel that silently takes the camera the
// moment it opens is worse than one that needs a button.
bool Graph_Drive();
void Graph_SetDrive(bool drive);

void Graph_Undo();
void Graph_Redo();

// Deletes every currently-selected key. A no-op with nothing selected -- CS2's own "Delete Key"
// button is disabled rather than a no-op click for the same state (GraphEditorJs.h), which the JS
// side mirrors via the same selCount the button's own dim state already reads.
void Graph_DeleteSelected();

// Interpolation across every keyframe, or just the selection.
void Graph_SetSmooth(bool smooth, bool selectedOnly);

// The last mode set across every key. The camera timeline draws Linear | Smooth as a segmented
// pair and lights the active half from this, so the two surfaces cannot disagree.
bool Graph_Smooth();

// The dock's rectangle in window-client pixels, pushed in by the workspace layout each frame. The
// plot is derived from it, and so is every cursor hit test -- which is why it cannot be measured
// from the panel and reported back.
void Graph_SetDockRect(int x, int y, int w, int h);

// Appends the graph's slice of the workspace state JSON: the plot rectangle, the sampled
// polylines, the keyframe dots and the selection. Written into an object the caller has already
// opened, so it emits `"graph":{...}` including the leading comma.
void Graph_AppendStateJson(std::string& out);
// The per-frame half (playhead, per-channel current); see the definition.
void Graph_AppendHotJson(std::string& out);

// Total keyframes across every channel. The dock ribbon shows it as the graph tab's meta line,
// so the count has one source rather than a second tally kept by the panel.
int Graph_KeyCount();

void Graph_Report();

} // namespace Filmmaker
