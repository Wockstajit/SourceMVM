#pragma once

// Engine-free logic for DeadlockEntities.cpp: entity-handle decoding and the hero-roster display
// name. Both are pure functions of already-read values (a raw handle uint32, two short strings),
// so they get their own header-only unit and a standalone test rather than living inline where a
// wrong mask or a wrong string join would only ever be caught by eye in a live probe.

#include <cctype>
#include <cstdint>
#include <string>

namespace Deadlock {

// Source 2's player/controller handles are a 15-bit index in the low bits (measured live: a hero
// pawn's m_hController and the matching controller's m_hPawn both round-trip through this exact
// mask -- see Automation/runs/deadlock-popup-anchor-*/../deadlock-cam-P4a-*/ for the probe that
// confirmed it). The serial number in the high bits is not needed here: Entities_Get() re-checks
// the entity's own class name before anything trusts the result, so a stale handle just fails that
// check rather than reading the wrong slot silently.
constexpr std::uint32_t kEntityHandleIndexMask = 0x7fff;

inline int HandleToEntityIndex(std::uint32_t handle) {
	return static_cast<int>(handle & kEntityHandleIndexMask);
}

// 0 is the measured "no target" sentinel (a hero pawn's own m_hPawn field, which does not apply to
// a pawn, reads 0 and resolves to entity 0, the world entity -- not a crash, just not a player).
// 0xffffffff is the documented "invalid handle" value elsewhere in this codebase's entity handling.
// Both are rejected here so a caller never even asks Entities_Get() for index 0 or index 0x7fff on
// a handle that was never actually set.
inline bool HandleIsValid(std::uint32_t handle) {
	return handle != 0 && handle != 0xffffffffu;
}

namespace detail {
inline std::string LowerAscii(const std::string& s) {
	std::string out(s);
	for (char& c : out) c = (char)std::tolower((unsigned char)c);
	return out;
}
}

// Deadlock's hero codename (read from the pawn's own identity block, e.g. "hero_atlas") into the
// label a human reads in the follow-camera hero picker ("Atlas"). Only the documented "hero_"
// prefix is special-cased -- everything else is a plain underscore-to-space title-case, so a
// codename this function has never seen still comes out readable rather than falling back to a
// raw, un-formatted string.
inline std::string HeroDisplayNameFromCodename(const std::string& codename) {
	if (codename.empty()) return std::string();

	std::string body = codename;
	static const char* const kPrefix = "hero_";
	static const size_t kPrefixLen = 5;
	// >= , not >: "hero_" alone (exactly the prefix, nothing after) must strip to an empty body,
	// not fall through to being title-cased as the word "Hero".
	if (body.size() >= kPrefixLen && detail::LowerAscii(body.substr(0, kPrefixLen)) == kPrefix)
		body = body.substr(kPrefixLen);
	if (body.empty()) return std::string();

	std::string out;
	out.reserve(body.size());
	bool startOfWord = true;
	for (char c : body) {
		if (c == '_' || c == ' ') {
			if (!out.empty() && out.back() != ' ') out += ' ';
			startOfWord = true;
			continue;
		}
		out += startOfWord ? (char)std::toupper((unsigned char)c) : (char)std::tolower((unsigned char)c);
		startOfWord = false;
	}
	// A codename that was only separators ("hero___") leaves a trailing space from the loop above.
	while (!out.empty() && out.back() == ' ') out.pop_back();
	return out;
}

// The one label the follow-camera hero dropdown actually shows, built from whichever of the two
// real names resolved. `fallbackIndex` is the roster position (1-based, matching the pre-P4b
// "Hero N" placeholder this replaces), used only when NEITHER name is known -- which happens on a
// build where the schema fields for both moved, not in normal use.
inline std::string ComposeHeroListName(const std::string& playerName, const std::string& heroDisplay,
	int fallbackIndex) {
	if (!playerName.empty() && !heroDisplay.empty()) return playerName + " (" + heroDisplay + ")";
	if (!playerName.empty()) return playerName;
	if (!heroDisplay.empty()) return heroDisplay;
	return "Hero " + std::to_string(fallbackIndex);
}

} // namespace Deadlock
