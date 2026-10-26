#pragma once

// The game side of the movie director outside the workspaces (Ui/DirectorHud.h, Ui/DirectorMath.h,
// Ui/DirectorMouse.h): what its inputs do in CS:S, the cards' readouts, F8's blackout of the game's
// HUD, and its console commands. The UI host (SourceUi.cpp) draws it and routes the input.
//
// CS:S's commands do the work: spec_next / spec_prev step the player, spec_mode the camera,
// sv_specspeed the free cam's speed, cl_drawhud 0 the blackout of the HUD, crosshair and spectator
// bars. Demo playback accepts them even where the demo's server had cheats off. Ctrl+wheel's FOV is
// the view's FOV override (mirv_fov's), because demo_fov_override does not reach SourceTV's camera.
//
// SourceTV's camera resets when a demo starts or a backward seek restarts it. The director puts back
// the mode and the player it followed, and opens a demo whose camera starts at the map origin
// following nobody (old demos, some new ones) in first person on the first player.
//
// Main thread only.

#include "Ui/DirectorHud.h"
#include "Ui/DirectorMath.h"
#include "Ui/DirectorMouse.h"

namespace advancedfx { class ICommandArgs; }

namespace SourceFilmmaker {

// The director's own state: the bar's drag state, whether the cards are on, the blackout.
Ui::DirectorState& Director_State();

// Once a frame: keeps SourceTV's camera across starts and restarts; once no demo plays, undoes the
// director's blackout, FOV and free cam speed.
void Director_Frame();

// True while the director should run: a demo is playing on a loaded map and neither the game's menu
// nor its console is up. Whether a workspace is open is the UI host's to add.
bool Director_Wanted();

// The cards' readouts for this frame.
void Director_BuildCards(Ui::DirectorCardsModel& cards);

// Client-px height of CS:S's spectator bars (Ui/DirectorMath.h kSpectatorBarShare) while the game
// shows them: a spectator's view, HUD on. 0 otherwise.
float Director_SpectatorBarHeight(float clientH);

// HLAE's free camera (mirv_input camera) flies the view: a press steers it at once (Ui/DirectorMouse.h),
// Ctrl and Shift on the wheel are its FOV and speed, Shift held slows it, and stepping to one of the
// game's cameras ends it, as CS2's free cam does.
bool Director_FreeCamOn();
void Director_SetSlow(bool slow);

void Director_Click(Ui::DirectorMouse::Click click);
void Director_Wheel(int notches, bool ctrl, bool shift);
void Director_Key(Ui::DirectorMath::Key key);
// The camera, notches steps around the director's ring (the bar's camera button is one step).
void Director_StepCamera(int notches);

// mirv_filmmaker hud on|off|toggle|state, blackout on|off|toggle|status.
void Director_HudCommand(const char* action);
void Director_BlackoutCommand(const char* action);

// One line for mirv_filmmaker health.
void Director_PrintHealth();

} // namespace SourceFilmmaker
