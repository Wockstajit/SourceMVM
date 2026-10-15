#pragma once

// Nervous System: a shot player's body keeps reacting for a few seconds after it goes down, the way
// Euphoria (GTA / Red Dead) and Overgrowth's injured ragdolls do. A hand goes to the wound (a leg hit
// grabs the leg and draws it up, a gut shot curls the body with both hands on the stomach, an arm hit
// clutches the arm with the other hand), the chest heaves, the limbs writhe and twitch, and it all fades
// out until the body is still. A headshot does none of it: the body is limp at once. Blasts and fire
// thrash and pedal instead of holding a wound.
//
// Runs under the Advanced Ragdolls master switch. It waits for the death clip and a settled torso,
// then nudges limb chains once per demo tick. Rolls drive the connected bodies around their support
// side. Connected arm joints follow a continuous target to reach and hold the wound.
// Behaviour lives in NervousSystemMath.h; engine body access lives in RagdollBodies.h.

#include <sstream>

namespace advancedfx { class ICommandArgs; }
namespace SOURCESDK { namespace CS2 { class CGameEvent; } }

namespace Filmmaker {

// Main/UI thread, from Filmmaker::RunMainThreadFrame. No-op while off and idle.
void NervousSystem_RunFrame();
void NervousSystem_OnGameEvent(SOURCESDK::CS2::CGameEvent* event);
// The entity graph is gone: forget tracked bodies without touching them.
void NervousSystem_LevelInit();

bool NervousSystem_Enabled();
void NervousSystem_SetEnabled(bool enabled);
// Muscle strength (10..200 %, default 100): how hard the body can move while it reacts.
double NervousSystem_Strength();
void NervousSystem_SetStrength(double percent);
// Seconds a body reacts for before it goes still (2..20, default 8); each injury and death varies it.
double NervousSystem_Duration();
void NervousSystem_SetDuration(double seconds);
// The four behaviours, each on by default.
bool NervousSystem_Reach();
void NervousSystem_SetReach(bool on);
bool NervousSystem_Breath();
void NervousSystem_SetBreath(bool on);
bool NervousSystem_Writhe();
void NervousSystem_SetWrithe(bool on);
bool NervousSystem_Twitch();
void NervousSystem_SetTwitch(bool on);
// Seconds the reaction eases in once it starts (0..2, default 0.5) and eases out before the body goes
// still (0..4, default 1.5): the hand-off from the death animation and into the final stillness.
double NervousSystem_BlendIn();
void NervousSystem_SetBlendIn(double seconds);
double NervousSystem_BlendOut();
void NervousSystem_SetBlendOut(double seconds);
// A grounded face-down body tries to roll onto its back (default on).
bool NervousSystem_RollOver();
void NervousSystem_SetRollOver(bool on);

void NervousSystem_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
// Config panel state (leading comma included).
void NervousSystem_AppendStateJson(std::ostringstream& o);

} // namespace Filmmaker
