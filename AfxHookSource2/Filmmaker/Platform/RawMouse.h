#pragma once

// The mouse's RAW counts (no Windows pointer speed or acceleration), for features that must turn
// exactly like the game aims. A process has ONE raw-mouse registration, so while any owner claims
// the mouse a message-only window of ours holds it (RIDEV_INPUTSINK) and sums the counts; CS2's
// own (SDL's) registration is saved first and put back when the last owner lets go. Legacy mouse
// messages keep flowing, so the OS cursor and the game UI are unaffected.
//
// Owners poll RawMouse_Totals each frame and use the difference from their previous read.

namespace Filmmaker {

enum RawMouseOwner : unsigned { kRawMouseOsu = 1u << 0, kRawMouseFreeCam = 1u << 1 };

// Claim (want = true) or release the mouse for one owner. Cheap to call every frame.
void RawMouse_Claim(RawMouseOwner owner, bool want);
// 1 registered and counting, 0 not (yet) registered, -1 the registration failed.
int RawMouse_State();
// Running totals of raw counts since the process started (x right, y down).
void RawMouse_Totals(long long& x, long long& y);

} // namespace Filmmaker
