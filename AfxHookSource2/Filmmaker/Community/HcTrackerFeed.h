#pragma once

// Live HCs for the main-menu HC Tracker badge: which HCs are open now and which are coming up,
// read from hctracker.com's /api/explore (the data its own homepage shows). Fetched on a worker
// thread while the main menu is up, at most every few minutes; a failure keeps the last good list
// and the badge simply shows less. Parsing: HcTrackerFeedMath.h.

namespace Filmmaker {

class JsonBuilder;

// Call every main-thread frame the main menu (or pause menu) exists. Starts a fetch when one is
// due; never blocks.
void HcTrackerFeed_Tick();

// Bumped whenever a fetch brings a new answer, so the UI pushes only on change.
unsigned HcTrackerFeed_Version();

// Writes "hc": {"ok", "live": n, "open": [...], "upcoming": n, "scheduled": [...]} into an object.
void HcTrackerFeed_AppendJson(JsonBuilder& j);

} // namespace Filmmaker
