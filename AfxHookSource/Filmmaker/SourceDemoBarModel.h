#pragma once

// Turns the demo's markers into what the demo bar shows: kill lines with names, team colours,
// round cells and bomb events. Engine-free, so the game host (SourceDemoBar.cpp) and the browser
// mockup fill the bar the same way. The camera button's names and cycle are Ui/DirectorMath.h's.

#include "SourceDemoMarkers.h"
#include "Ui/DemoBar.h"

#include <functional>
#include <string>

namespace SourceFilmmaker {

// Name and team of a user id, false when the game does not know it.
using PlayerLookup = std::function<bool(int userId, std::string& name, int& team)>;

// Fills the bar's kills, rounds, round starts and bombs from markers.
void DemoBarFillMarkers(Ui::DemoBarModel& model, const DemoMarkers& markers, const PlayerLookup& lookup);

// "Josh killed Xander (usp, headshot)", "Xander died (world)".
std::string DemoBarKillText(const KillMarker& kill, const PlayerLookup& lookup);

Ui::Color DemoBarTeamColor(int team);

} // namespace SourceFilmmaker
