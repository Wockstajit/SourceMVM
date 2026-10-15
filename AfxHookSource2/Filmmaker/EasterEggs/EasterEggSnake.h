#pragma once

// Snake for long renders: once a render's capture is estimated to run long (IsLongRender in
// RenderEtaMath.h), a small prompt asks whether the user wants to play Snake while they wait.
// Y / Enter opens the board; N / Esc dismisses the prompt and nothing more appears for that job.
// The board waits until an arrow key or W/A/S/D is pressed, then it is a full game of Snake whose
// food is a random CS2 equipment icon each time (SnakeMath.h has the rules). Esc closes it; the
// prompt and the board both close themselves when the render ends.
//
// It only appears for renders that capture BEFORE the UI (HUD mode None) -- with a HUD capture the
// UI would be in the take, so every Source MVM panel hides then and Snake stays away too.
// While the prompt is up it takes Y, N, Enter and Esc; while the board is open it takes the arrow
// keys, W/A/S/D and Esc, so they never reach the game or editor.

#include <string>

namespace Filmmaker {

// The board or the prompt is up (the overlay draws either from Snake_BuildJson).
bool Snake_Active();
void Snake_Open();   // test/automation: open the board now (`mirv_filmmaker egg snake open`)
void Snake_Ask();    // test/automation: show the prompt now (`mirv_filmmaker egg snake ask`)
void Snake_Answer(bool play); // the prompt's answer (`mirv_filmmaker egg snake yes|no`)
void Snake_Close();  // closes the board and the prompt
// MAIN thread, every frame: the render watcher and the game clock.
void Snake_RunFrame();
// Window-proc thread. True = swallowed.
bool Snake_OnWindowMessage(unsigned msg, unsigned long long wParam);
// The overlay's per-frame state (attribute "snake").
void Snake_BuildJson(std::string& out);
std::string Snake_Status();

} // namespace Filmmaker
