#pragma once

// Runs the FilmmakerDemoInfo helper (Go / demoinfocs-golang) on a .dem to obtain a
// scoreboard WITH player names + map/duration, which the .dem.info matchmaking
// sidecar cannot provide (it has account ids only).
//
// The result is cached in the demo's source_mvm project folder as scoreboard.fmjson;
// a cache newer than the demo is reused, so a library only pays the parse cost once.

#include "DemoEntry.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace Filmmaker {

struct DemoHelperResult {
	bool ok = false;            // helper produced a usable parse
	int schemaVersion = 0;      // "v" field; mismatch -> stale cache, regenerate
	bool namesOnly = false;     // parse stopped early (names only; no duration/scores)
	std::string map;            // map name from the demo (may be empty)
	int durationSeconds = 0;    // demo length in seconds (0 = unknown)
	bool hasScoreboard = false; // at least one player row present
	int teamScore0 = 0;         // CT
	int teamScore1 = 0;         // T
	std::string error;          // helper / parse failure text when ok=false
	std::string helperPath;     // helper exe path used for diagnostics
	std::vector<ScoreboardPlayer> players;
	std::vector<DemoEvent> events; // weapon/C4 drop+pickup ticks (full parse only, v5+)
};

// Parses demoPath via the helper (using/refreshing the scoreboard.fmjson cache).
// Returns ok=false if the helper is missing or the demo could not be parsed.
//
// When wantedAccountIds is non-empty the caller already has the full scoreboard
// (e.g. from the .dem.info matchmaking sidecar) and only needs player NAMES: the
// helper runs in "names-fast" mode and cancels the parse as soon as every wanted
// account id has a name, which is dramatically faster than a full decode.
//
// cancel (optional) lets a caller abort an in-flight helper process promptly (e.g.
// the scan thread when a new scan supersedes it); regardless of cancel, the helper
// is also bounded by an internal hard timeout so a hung/runaway parse can never block
// the caller forever.
DemoHelperResult ReadDemoInfoViaHelper(const std::wstring& demoPath,
	const std::vector<uint32_t>& wantedAccountIds = {},
	const std::atomic<bool>* cancel = nullptr);

} // namespace Filmmaker
