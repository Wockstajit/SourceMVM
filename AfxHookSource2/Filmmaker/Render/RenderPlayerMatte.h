#pragma once

// Per-Player Matte controller: picks a specific player in the demo (living or dead) for isolated
// rendering (Selected Player Key / Matte / Depth / Normal passes), live scene highlighting, and
// Effect Nodes player-matte targeting.

#include "RenderPlayerMatteMath.h"

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Main thread, once per frame. Refreshes the roster and the selected player's 3D skeleton + other
// players' capsules whenever a player is picked or the Render workspace / Effect Nodes need it.
void RenderPlayerMatte_RunMainThreadFrame();
void RenderPlayerMatte_OnLevelInit();

// Selection control (main thread).
bool RenderPlayerMatte_PickController(int controller);
bool RenderPlayerMatte_PickSpectated();
void RenderPlayerMatte_Clear();
int RenderPlayerMatte_SelectedController();
std::string RenderPlayerMatte_SelectedName();
bool RenderPlayerMatte_HasTarget();

// Live highlight-in-scene settings (main thread).
void RenderPlayerMatte_SetHighlightEnabled(bool on);
bool RenderPlayerMatte_HighlightEnabled();
void RenderPlayerMatte_SetHighlightDim(float v);
void RenderPlayerMatte_SetHighlightDesat(float v);
void RenderPlayerMatte_SetHighlightOutline(float v);
void RenderPlayerMatte_SetHighlightTint(float r, float g, float b);

// Render thread: thread-safe copy of the current frame's target player bones and other players.
PlayerMatte::RenderSnapshot RenderPlayerMatte_Snapshot();

// Appends `,"playerMatte":{...}` inside st.render (RenderCommand.cpp).
void RenderPlayerMatte_AppendStateJson(std::ostringstream& out);

// Console: `mirv_filmmaker render player ...`
void RenderPlayerMatte_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
