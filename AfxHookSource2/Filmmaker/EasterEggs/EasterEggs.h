#pragma once

// Easter eggs: secret modes, rare floating hints, Clippy, and the Sony Vegas intervention.
//
// Standalone by the repo's coupling rule. Other modules only ever CALL in here at a handful of
// edges (render start, window messages, game events, level init, the main-thread pumps); nothing
// else reads egg state except through EasterEgg_Active(). All on-screen output is one Panorama
// overlay (Panorama/EasterEggOverlayHud), fed by EasterEggs_BuildOverlayJson(); the in-world
// halves live in EasterEggWorld.h.
//
// Persisted in %APPDATA%\HLAE\easter_eggs.json: the hint scheduler's state (EasterEggHintMath.h),
// so the rarity holds across restarts. Effects are temporary and stop on a level transition.

#include <cstdint>
#include <string>

namespace advancedfx { class ICommandArgs; }
namespace SOURCESDK { namespace CS2 { class CGameEvent; } }

namespace Filmmaker {

enum class EasterEgg : int {
	EGirl = 0,     // pastel UI wash, sparkle cursor, pink haze           -- console: uwu
	Disco,         // the sun changes colour on the beat, shadows spin     -- Konami code / inferno_disco
	MoonGravity,   // corpses float away                                   -- one_small_step
	BigHead,       // GoldenEye big heads                                  -- bighead
	ChickenStorm,  // it rains chickens                                    -- bawk
	MichaelBay,    // every death explodes                                 -- michaelbay
	WesAnderson,   // warm pastel light, letterbox, a chapter card a round -- symmetry
	Count
};

const char* EasterEgg_Name(EasterEgg egg);
bool EasterEgg_Active(EasterEgg egg);
// Turns an egg on/off; `announce` floats its name across the screen.
void EasterEgg_Set(EasterEgg egg, bool on, bool announce = true);

// MAIN thread, once per frame (Filmmaker::RunMainThreadFrame): hints, Vegas, Clippy, world eggs.
void EasterEggs_RunMainThreadFrame();
// MAIN thread, Filmmaker::RunMainThreadLateFrame: Big Head's local-pose write.
void EasterEggs_RunLateFrame();
// Game-event thread (GameEvents.cpp): deaths for Michael Bay, round starts for Wes Anderson.
void EasterEggs_OnGameEvent(SOURCESDK::CS2::CGameEvent* event);
// A new level: forget expired entity/model references and queue main-thread cancellation.
void EasterEggs_LevelInit();

// Window-proc thread, OBSERVE ONLY: keys feed the Konami detector, any input resets the idle
// timer Clippy waits on, clicks feed the sparkle bursts.
void EasterEggs_OnWindowMessage(unsigned msg, unsigned long long wParam);

// Render job start (RenderSessionDriver). The Vegas check runs off-thread from here.
void EasterEggs_OnRenderStarted();

// JSON state for the overlay. `out` is rebuilt every call; events carry increasing ids so the JS
// acts once per event. Returns true while something on the overlay needs per-frame animation.
bool EasterEggs_BuildOverlayJson(std::string& out);

// mirv_filmmaker egg ... (automation + testing surface).
void EasterEggs_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// Called from the cheat-code console commands. `code` is the command name.
void EasterEggs_CheatCode(const char* code);
// `osu`: a short pop-up, then osu! opens (typed again while open, it closes).
void EasterEggs_OsuCheat();

} // namespace Filmmaker
