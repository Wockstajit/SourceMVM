#include "../../AfxHookSource2/Filmmaker/Panorama/HudReadoutCadenceMath.h"
#include <iostream>
#include <stdexcept>

void Check(bool value, const char* label) { if (!value) throw std::runtime_error(label); }
int main() { try {
    Filmmaker::HudReadoutCadence clock;
    int updates = 0;
    for (unsigned ms = 0; ms < 1000; ms += 5) updates += clock.Ready(ms, 10, false);
    Check(updates == 10, "200 FPS playback publishes 10 passive samples per second");
    Check(clock.Ready(995, 10, true), "visibility and input bypass the passive deadline");
    Check(clock.Ready(996, 10, true), "successive wheel steps are never throttled");
    Check(!clock.Ready(997, 10, false), "passive samples resume waiting after input");
    Check(clock.Ready(1096, 10, false), "readout resumes at the deadline");
    Check(clock.Ready(10, 10, false), "backwards clock cannot freeze the HUD");
    Check(clock.Ready(11, 0, false) && clock.Ready(11, 0, false), "zero preserves per-frame comparison mode");
    Check(clock.Ready(12, 10, true), "rebuild/rate change forces a first publication");
    std::cout << "HUD cadence, input bypass and clock recovery passed.\n"; return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
