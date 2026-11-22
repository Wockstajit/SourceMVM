#include "KillFeed.h"

#include "../Cosmetics/CosmeticEntityAccess.h"
#include "../Debug/MvmDebugLog.h"
#include "../Demo/PlayingDemoPath.h"
#include "../Platform/JsonBuilder.h"
#include "../../ClientEntitySystem.h"
#include "../../../Shared/AfxConsole.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Filmmaker {

namespace {

struct Player { std::string name, shown; };

std::vector<Player> g_players; // every player seen in this demo, in the order first seen
bool g_watchedOnly = false;
std::wstring g_demo;
unsigned g_demoGeneration = 0; // bumped for every new demo, so the feed script drops old aliases
unsigned g_scan = 0;

constexpr int kMaxPlayers = 64;
constexpr size_t kMaxName = 63;

struct Names { char name[kMaxPlayers][kMaxName + 1]; int count; };

// The players' names as the kill feed prints them. SEH-guarded: a stale entity is nobody.
void ReadNames(Names& out) {
	out.count = 0;
	__try {
		const int highest = GetHighestEntityIndex();
		for (int i = 1; i <= highest && out.count < kMaxPlayers; ++i) {
			CEntityInstance* ent = CosmeticEntFromIndex(i);
			if (!ent || !ent->IsPlayerController()) continue;
			const char* n = ent->GetPlayerName();
			if (!n || !*n) continue;
			strncpy_s(out.name[out.count], kMaxName + 1, n, _TRUNCATE);
			++out.count;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		out.count = 0;
	}
}

// The watched player's name, from the pawn on screen.
bool WatchedName(char out[kMaxName + 1]) {
	out[0] = 0;
	__try {
		const int pawnIndex = AfxGetSpectatedPawnIndexHeld();
		CEntityInstance* pawn = pawnIndex >= 0 ? CosmeticEntFromIndex(pawnIndex) : nullptr;
		if (!pawn) return false;
		const auto handle = pawn->GetPlayerControllerHandle();
		CEntityInstance* controller = handle.IsValid() ? CosmeticEntFromIndex(handle.GetEntryIndex()) : nullptr;
		if (!controller || !controller->IsPlayerController()) return false;
		const char* n = controller->GetPlayerName();
		if (!n) return false;
		strncpy_s(out, kMaxName + 1, n, _TRUNCATE);
		return out[0] != 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		out[0] = 0;
		return false;
	}
}

void Refresh() {
	const std::wstring demo = CanonicalDemoPath(ResolvePlayingDemoPath());
	if (demo != g_demo) { g_demo = demo; g_players.clear(); ++g_demoGeneration; }
	static Names names; // 4 KB: kept off the stack
	ReadNames(names);
	for (int i = 0; i < names.count; ++i) {
		bool known = false;
		for (const Player& p : g_players) known = known || p.name == names.name[i];
		if (!known && (int)g_players.size() < kMaxPlayers) g_players.push_back({ names.name[i], "" });
	}
}

// A shown name stays one console token's worth of plain text: no quotes or semicolons (they would
// split or end the command that carries it) and no markup (the feed's labels render html).
std::string Clean(const std::string& s) {
	std::string r;
	for (unsigned char c : s) {
		if (c == '"' || c == ';' || c == '<' || c == '>' || c < 0x20) continue;
		r += (char)c;
	}
	while (!r.empty() && r.front() == ' ') r.erase(r.begin());
	while (!r.empty() && r.back() == ' ') r.pop_back();
	if (r.size() > kMaxName) r.resize(kMaxName);
	return r;
}

} // namespace

KillFeedState KillFeed_Snapshot() {
	KillFeedState s;
	s.watchedOnly = g_watchedOnly;
	for (const Player& p : g_players) if (!p.shown.empty()) s.shown.push_back({ p.name, p.shown });
	return s;
}

void KillFeed_Apply(const KillFeedState& state) {
	g_watchedOnly = state.watchedOnly;
	for (Player& p : g_players) p.shown.clear();
	for (const auto& r : state.shown) {
		bool found = false;
		for (Player& p : g_players) if (p.name == r.first) { p.shown = r.second; found = true; }
		if (!found && (int)g_players.size() < kMaxPlayers) g_players.push_back({ r.first, r.second });
	}
}

void KillFeed_Reset() {
	g_players.clear();
	g_demo.clear();
	++g_demoGeneration;
}

void KillFeed_AppendStateJson(std::ostringstream& o) {
	if ((g_scan++ % 30) == 0) Refresh();
	char watched[kMaxName + 1];
	WatchedName(watched);
	if (g_watchedOnly && MvmDebugLog_Active() && (g_scan % 120) == 1)
		MvmDebugLog_LinefAlways("killfeed", "watched-only filter: watching '%s', %zu player(s) known", watched, g_players.size());
	o << ",\"killFeed\":{\"demo\":" << g_demoGeneration << ",\"only\":" << (g_watchedOnly ? 1 : 0)
		<< ",\"watched\":\"" << JsonBuilder::Escape(watched) << "\",\"players\":[";
	for (size_t i = 0; i < g_players.size(); ++i)
		o << (i ? "," : "") << "[\"" << JsonBuilder::Escape(g_players[i].name) << "\",\"" << JsonBuilder::Escape(g_players[i].shown) << "\"]";
	o << "]}";
}

void KillFeed_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	auto arg = [&](int i) -> const char* { return i < argc ? args->ArgV(i) : ""; };
	const char* sub = arg(2);
	auto list = [&]() {
		Refresh();
		for (size_t i = 0; i < g_players.size(); ++i)
			advancedfx::Message("  %d  %s%s%s\n", (int)i + 1, g_players[i].name.c_str(),
				g_players[i].shown.empty() ? "" : "  ->  ", g_players[i].shown.c_str());
		if (g_players.empty()) advancedfx::Message("  (no players yet: play the demo)\n");
	};
	if (!*sub || 0 == _stricmp(sub, "status") || 0 == _stricmp(sub, "list")) {
		char watched[kMaxName + 1];
		WatchedName(watched);
		advancedfx::Message("%s killfeed: %s; watching '%s'.\n", cmd, g_watchedOnly ? "only the watched player's kills" : "every kill", watched);
		list();
		if (!*sub)
			advancedfx::Message(
				"%s killfeed name <n> <new name>  - show player n under another name (from the list above)\n"
				"%s killfeed name <n>             - back to the real name\n"
				"%s killfeed name clear           - every real name back\n"
				"%s killfeed only all|watched     - every kill, or only the ones the watched player is in\n", cmd, cmd, cmd, cmd);
		return;
	}
	if (0 == _stricmp(sub, "name")) {
		if (0 == _stricmp(arg(3), "clear")) {
			for (Player& p : g_players) p.shown.clear();
			advancedfx::Message("%s killfeed: every real name is back.\n", cmd);
			return;
		}
		Refresh();
		const int n = atoi(arg(3));
		if (n < 1 || n > (int)g_players.size()) { advancedfx::Warning("%s killfeed name <1..%d> <new name>\n", cmd, (int)g_players.size()); return; }
		std::string shown;
		for (int i = 4; i < argc; ++i) {
			if (0 == _stricmp(arg(i), "quiet")) continue;
			if (!shown.empty()) shown += ' ';
			shown += arg(i);
		}
		shown = Clean(shown);
		Player& p = g_players[n - 1];
		// The feed tells players apart by the text on a notice, so a name already in use by
		// another player (real or shown) would make two players one.
		if (!shown.empty() && shown != p.name) { // an empty name is "back to the real one"
			for (const Player& o : g_players) {
				if (&o == &p || (o.name != shown && o.shown != shown)) continue;
				advancedfx::Warning("%s killfeed: \"%s\" is already %s; pick a name no other player has.\n", cmd,
					shown.c_str(), o.name == shown ? "another player's name" : ("shown for " + o.name).c_str());
				return;
			}
		}
		p.shown = shown == p.name ? std::string() : shown;
		advancedfx::Message("%s killfeed: %s is shown as %s.\n", cmd, p.name.c_str(), p.shown.empty() ? "their own name" : p.shown.c_str());
		MvmDebugLog_LinefAlways("killfeed", "name #%d '%s' -> '%s'", n, p.name.c_str(), p.shown.c_str());
		return;
	}
	if (0 == _stricmp(sub, "only")) {
		const char* w = arg(3);
		if (0 == _stricmp(w, "watched")) g_watchedOnly = true;
		else if (0 == _stricmp(w, "all")) g_watchedOnly = false;
		else if (0 == _stricmp(w, "toggle") || !*w) g_watchedOnly = !g_watchedOnly;
		else { advancedfx::Warning("%s killfeed only all|watched|toggle\n", cmd); return; }
		advancedfx::Message("%s killfeed: %s.\n", cmd, g_watchedOnly ? "only the watched player's kills" : "every kill");
		return;
	}
	advancedfx::Warning("%s killfeed: unknown '%s' (name|only|list).\n", cmd, sub);
}

} // namespace Filmmaker
