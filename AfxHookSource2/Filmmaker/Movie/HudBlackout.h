#pragma once

// HUD BLACKOUT: one toggle that clears the screen of EVERY overlay, ours and the game's.
//
// F8 used to hide only the floating movie-director CONTROLS card, which is a small part of what is
// actually on screen while directing: the camera timeline, the editor workspace, the Config
// sidebar, the native CS2 gameplay HUD (crosshair, radar, health/ammo, spectator strip) and the
// native demo bar are all still there. For framing or recording a shot the useful action is "clear
// everything", so that is what F8 does now.
//
// DELIBERATELY A FLAG, NOT A SHUTDOWN. Nothing is closed, disabled or torn down -- each surface
// only RENDERS as hidden while this is true. So a blackout never loses the editor's state, the
// selected camera, the open tab or an in-progress reposition, and turning it off puts the screen
// back exactly as it was. That is also why it does not touch the panels' own Enabled() flags:
// those are the user's choice about what is open, which is a different question from what is
// currently drawn.
//
// It also RELEASES THE WORKSPACE PREVIEW SCALING. The editor and the demo Config panel render the
// game into a scaled rect with their chrome around it; hiding the chrome alone would leave the
// game as a small picture in the corner surrounded by black, which is not "clean screen" by any
// reading. Blackout hands the full frame back to the game and restores the scaled layout on exit.
//
// Demo playback only -- see HudBlackout_RunFrame, which clears the flag when the demo ends so a
// blackout can never persist into the main menu with no way to see the toggle that caused it.

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// True while the screen is blacked out. Read by every HUD's render path (and published into the
// timeline/config state JSON for the Panorama side).
bool HudBlackout_Active();

void HudBlackout_Set(bool on);
void HudBlackout_Toggle();

// Main thread, once per frame: drops the blackout when demo playback ends.
void HudBlackout_RunFrame();

// `mirv_filmmaker blackout [on|off|toggle|status]`.
void HudBlackout_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
