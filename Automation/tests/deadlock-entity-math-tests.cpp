// Tests for AfxHookSource2/Deadlock/DeadlockEntityMath.h: entity-handle decoding and the hero
// roster's display name, both engine-free.
//
//   ctest --test-dir build/x64-deadlock-release -C Release -R DeadlockEntityMathTests
#include "../../AfxHookSource2/Deadlock/DeadlockEntityMath.h"

#include <cstdio>

int g_failures = 0;

void Check(bool value, const char* name) {
	if (value) return;
	++g_failures;
	std::printf("  FAIL %s\n", name);
}

int main() {
	using namespace Deadlock;

	// --- HandleToEntityIndex / HandleIsValid -----------------------------------------------------
	// 0x00bb8004 is the exact raw handle read live off a hero pawn's m_hController, which decoded
	// to controller index 4 -- confirmed by probing that controller's own m_hPawn, which decoded
	// back to the pawn's index 69 via the same mask.
	Check(HandleToEntityIndex(0x00bb8004u) == 4, "measured m_hController handle -> index 4");
	Check(HandleToEntityIndex(0x01cd0045u) == 69, "measured m_hPawn handle -> index 69");
	Check(HandleToEntityIndex(0x7fffu) == 0x7fff, "the mask keeps a full 15-bit index");
	Check(HandleToEntityIndex(0xffff8000u) == 0, "bits above the mask never leak into the index");

	Check(!HandleIsValid(0), "a zero handle (the 'no target' sentinel) is invalid");
	Check(!HandleIsValid(0xffffffffu), "the documented invalid-handle value is invalid");
	Check(HandleIsValid(0x00bb8004u), "a real measured handle is valid");
	Check(HandleIsValid(1), "index 1 alone (no serial bits) is still a valid handle");

	// --- HeroDisplayNameFromCodename --------------------------------------------------------------
	Check(HeroDisplayNameFromCodename("hero_atlas") == "Atlas", "measured codename: hero_atlas -> Atlas");
	Check(HeroDisplayNameFromCodename("HERO_ATLAS") == "Atlas", "the hero_ prefix strips case-insensitively");
	Check(HeroDisplayNameFromCodename("hero_ghost_hound") == "Ghost Hound",
		"a multi-word codename becomes a multi-word title-cased label");
	Check(HeroDisplayNameFromCodename("atlas") == "Atlas",
		"a codename with no hero_ prefix is still title-cased, not passed through raw");
	Check(HeroDisplayNameFromCodename("") == "", "an empty codename stays empty");
	Check(HeroDisplayNameFromCodename("hero_") == "", "a prefix with nothing after it is empty, not 'Hero'");
	Check(HeroDisplayNameFromCodename("hero") == "Hero",
		"'hero' alone is too short to match the hero_ prefix and is title-cased as-is");
	Check(HeroDisplayNameFromCodename("HeRo_AtLaS") == "Atlas", "mixed-case input normalizes to one case");

	// --- ComposeHeroListName ------------------------------------------------------------------------
	Check(ComposeHeroListName("aitch", "Atlas", 1) == "aitch (Atlas)",
		"both names known: player name first, hero name in parens");
	Check(ComposeHeroListName("aitch", "", 1) == "aitch", "only the player name known");
	Check(ComposeHeroListName("", "Atlas", 1) == "Atlas", "only the hero name known");
	Check(ComposeHeroListName("", "", 3) == "Hero 3",
		"neither name known falls back to the pre-P4b placeholder, by roster position");

	if (g_failures) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("Deadlock entity math: all checks passed\n");
	return 0;
}
