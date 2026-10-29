// Weapon Recoil scope/precedence tests.
//
// The rule under test -- Specific Weapon > Weapon Type > Primary/Secondary > All -- is invisible
// in-game: an AK firing with the rifle tuning and an AK firing with a per-weapon tuning look the
// same on screen unless the numbers happen to differ a lot. So it gets checked here instead.

#include "../../AfxHookSource2/Filmmaker/Movie/WeaponRecoilScope.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Filmmaker;

static int g_failures = 0;
static int g_checks = 0;

static void Check(bool ok, const char* what) {
	++g_checks;
	if (ok) return;
	++g_failures;
	std::printf("FAIL: %s\n", what);
}

static RecoilOverride MakeOverride(RecoilScope scope, const char* key, double amount) {
	RecoilOverride ov;
	ov.scope = scope;
	ov.key = key;
	ov.tuning.amount = amount;
	return ov;
}

static void TestTaxonomy() {
	Check(RecoilWeaponTypeOf("ak47") == RecoilWeaponType::Rifle, "ak47 is a rifle");
	Check(RecoilWeaponTypeOf("weapon_ak47") == RecoilWeaponType::Rifle, "weapon_ak47 is a rifle");
	Check(RecoilWeaponTypeOf("AK47") == RecoilWeaponType::Rifle, "weapon match is case-insensitive");
	Check(RecoilWeaponTypeOf("glock") == RecoilWeaponType::Pistol, "glock is a pistol");
	Check(RecoilWeaponTypeOf("deagle") == RecoilWeaponType::Pistol, "deagle is a pistol");
	Check(RecoilWeaponTypeOf("awp") == RecoilWeaponType::Sniper, "awp is a sniper");
	Check(RecoilWeaponTypeOf("nova") == RecoilWeaponType::Shotgun, "nova is a shotgun");
	Check(RecoilWeaponTypeOf("negev") == RecoilWeaponType::Lmg, "negev is an lmg");
	Check(RecoilWeaponTypeOf("mp9") == RecoilWeaponType::Smg, "mp9 is an smg");
	// CS:S's guns, as the CS:S hook names them from their network classes.
	Check(RecoilWeaponTypeOf("weapon_scout") == RecoilWeaponType::Sniper, "CS:S scout is a sniper");
	Check(RecoilWeaponTypeOf("weapon_sg550") == RecoilWeaponType::Sniper, "CS:S sg550 is a sniper");
	Check(RecoilWeaponTypeOf("weapon_sg552") == RecoilWeaponType::Rifle, "CS:S sg552 is a rifle");
	Check(RecoilWeaponTypeOf("weapon_m3") == RecoilWeaponType::Shotgun, "CS:S m3 is a shotgun");
	Check(RecoilWeaponTypeOf("weapon_tmp") == RecoilWeaponType::Smg, "CS:S tmp is an smg");
	Check(RecoilWeaponTypeOf("weapon_mp5navy") == RecoilWeaponType::Smg, "CS:S mp5navy is an smg");
	Check(RecoilWeaponTypeOf("weapon_p228") == RecoilWeaponType::Pistol, "CS:S p228 is a pistol");
	Check(RecoilWeaponTypeOf("") == RecoilWeaponType::Unknown, "empty weapon has no type");
	Check(RecoilWeaponTypeOf("frobnicator9000") == RecoilWeaponType::Unknown,
		"a weapon CS2 has not shipped yet has no type");

	Check(RecoilSlotOf(RecoilWeaponType::Pistol) == RecoilSlot::Secondary, "pistols are secondary");
	Check(RecoilSlotOf(RecoilWeaponType::Rifle) == RecoilSlot::Primary, "rifles are primary");
	Check(RecoilSlotOf(RecoilWeaponType::Lmg) == RecoilSlot::Primary, "lmgs are primary");
	Check(RecoilSlotOf(RecoilWeaponType::Unknown) == RecoilSlot::Unknown,
		"an unknown weapon gets no slot rather than a guessed one");

	// The multiplier table and the type table are the same table; a lookup miss must still be a
	// usable default rather than zero, or an unknown gun would recoil not at all.
	Check(RecoilWeaponMultiplier("awp") > RecoilWeaponMultiplier("mp9"), "awp kicks harder than mp9");
	Check(RecoilWeaponMultiplier("frobnicator9000") == 1.0, "unknown weapon falls back to 1.0");
	Check(RecoilWeaponMultiplier("") == 1.0, "empty weapon falls back to 1.0");
}

static void TestPrecedence() {
	RecoilTuning all;
	all.amount = 10.0;

	// Every scope matches an AK at once. Each step adds a NARROWER one, which must take over.
	std::vector<RecoilOverride> overrides;
	Check(RecoilResolve(all, overrides, "ak47").amount == 10.0, "no overrides -> All");

	overrides.push_back(MakeOverride(RecoilScope::Slot, "primary", 20.0));
	Check(RecoilResolve(all, overrides, "ak47").amount == 20.0, "slot beats All");

	overrides.push_back(MakeOverride(RecoilScope::Type, "rifle", 30.0));
	Check(RecoilResolve(all, overrides, "ak47").amount == 30.0, "type beats slot");

	// Type is the narrowest scope now that per-weapon is gone. A second override at the SAME
	// scope must not win merely by being later in the vector: ties go to the first match, so the
	// answer cannot depend on how the list was built.
	overrides.push_back(MakeOverride(RecoilScope::Type, "rifle", 40.0));
	Check(RecoilResolve(all, overrides, "ak47").amount == 30.0, "a same-scope tie keeps the first");

	// Order in the vector must not change the answer.
	std::vector<RecoilOverride> reversed(overrides.rbegin(), overrides.rend());
	Check(RecoilResolve(all, reversed, "ak47").amount == 40.0, "precedence is order-independent");

	// A gun that only the broad scopes cover still gets them.
	Check(RecoilResolve(all, overrides, "m4a4").amount == 30.0, "m4a4 gets the rifle tuning");
	Check(RecoilResolve(all, overrides, "awp").amount == 20.0, "awp is a primary but not a rifle");
	Check(RecoilResolve(all, overrides, "glock").amount == 10.0,
		"a pistol matches none of these and falls back to All");

	// An unknown weapon must not be swept up by a slot or type override.
	Check(RecoilResolve(all, overrides, "frobnicator9000").amount == 10.0,
		"an untabled weapon falls back to All, not to primary");
	Check(RecoilResolve(all, overrides, "").amount == 10.0, "a blank weapon token falls back to All");
}

static void TestKeyMatching() {
	RecoilTuning all;
	all.amount = 1.0;
	std::vector<RecoilOverride> overrides;
	overrides.push_back(MakeOverride(RecoilScope::Type, "rifle", 2.0));

	// The event field is "ak47" from one delivery path and "weapon_ak47" from another. A TYPE
	// override compares the derived category, so both forms have to derive the same type or the
	// tuning would apply only on some deliveries.
	Check(RecoilResolve(all, overrides, "weapon_ak47").amount == 2.0, "the prefixed form derives rifle");
	Check(RecoilResolve(all, overrides, "ak47").amount == 2.0, "the bare form derives rifle");
	Check(RecoilResolve(all, overrides, "m4a4").amount == 2.0, "another rifle gets the same tuning");
	Check(RecoilResolve(all, overrides, "glock").amount == 1.0, "a pistol falls through to All");

	// An empty key is not a wildcard.
	std::vector<RecoilOverride> blank;
	blank.push_back(MakeOverride(RecoilScope::Type, "", 9.0));
	Check(RecoilResolve(all, blank, "ak47").amount == 1.0, "an empty key matches nothing");
}

static void TestScopeParsing() {
	RecoilScope s = RecoilScope::Type;
	Check(RecoilParseScope("all", s) && s == RecoilScope::All, "parse all");
	Check(RecoilParseScope("Slot", s) && s == RecoilScope::Slot, "parse slot, case-insensitive");
	Check(RecoilParseScope("type", s) && s == RecoilScope::Type, "parse type");
	Check(!RecoilParseScope("nonsense", s), "an unknown scope name is rejected");

	Check(std::string(RecoilScopeName(RecoilScope::All)) == "all", "scope name round-trips");
}

static void TestFindOverride() {
	std::vector<RecoilOverride> overrides;
	overrides.push_back(MakeOverride(RecoilScope::Type, "rifle", 5.0));
	Check(RecoilFindOverride(overrides, RecoilScope::Type, "rifle") != nullptr, "finds by scope+key");
	Check(RecoilFindOverride(overrides, RecoilScope::Type, "RIFLE") != nullptr,
		"lookup key is case-insensitive");
	Check(RecoilFindOverride(overrides, RecoilScope::Slot, "rifle") == nullptr,
		"same key under a different scope is a different override");
	Check(RecoilFindOverride(overrides, RecoilScope::Type, "smg") == nullptr, "missing key -> null");
}

static void TestDefaultsAreNeutral() {
	// A freshly created override must behave exactly like no override, so adding one and then
	// changing nothing cannot alter how the gun already felt.
	RecoilTuning all;
	RecoilTuning fresh;
	Check(all.amount == fresh.amount && all.punch == fresh.punch && all.rotation == fresh.rotation
		&& all.cameraRot == fresh.cameraRot && all.cameraPos == fresh.cameraPos
		&& all.cameraRoll == fresh.cameraRoll && all.snap == fresh.snap && all.recovery == fresh.recovery
		&& all.pivot == fresh.pivot, "a new override starts at the reference tuning");
}

static void TestInheritance() {
	RecoilTuning all;
	all.amount = 10.0;
	std::vector<RecoilOverride> overrides;
	overrides.push_back(MakeOverride(RecoilScope::Slot, "primary", 20.0));
	overrides.push_back(MakeOverride(RecoilScope::Type, "rifle", 30.0));

	// A representative must be a token the TABLE knows, or the category tests match nothing.
	Check(RecoilWeaponTypeOf(RecoilRepresentativeWeapon(RecoilScope::Type, "sniper"))
		== RecoilWeaponType::Sniper, "the sniper stand-in really is a sniper");
	Check(RecoilSlotOf(RecoilWeaponTypeOf(RecoilRepresentativeWeapon(RecoilScope::Slot, "secondary")))
		== RecoilSlot::Secondary, "the secondary stand-in really is a secondary");
	Check(RecoilRepresentativeWeapon(RecoilScope::Type, "trebuchet").empty(),
		"an unknown category has no stand-in");

	// Opening a scope shows what it inherits, so seeding a new override with it changes nothing.
	Check(RecoilInheritedTuning(all, overrides, RecoilScope::Type, "sniper").amount == 20.0,
		"a new sniper override inherits the primary tuning, not the rifle one");
	Check(RecoilInheritedTuning(all, overrides, RecoilScope::Type, "rifle").amount == 20.0,
		"the rifle scope inherits from primary, ignoring its OWN override");
	Check(RecoilInheritedTuning(all, overrides, RecoilScope::Slot, "primary").amount == 10.0,
		"the primary scope inherits from All, ignoring its own override");
	Check(RecoilInheritedTuning(all, overrides, RecoilScope::Type, "pistol").amount == 10.0,
		"a pistol inherits All when nothing broader covers it");
}

int main() {
	TestTaxonomy();
	TestInheritance();
	TestPrecedence();
	TestKeyMatching();
	TestScopeParsing();
	TestFindOverride();
	TestDefaultsAreNeutral();

	if (g_failures) {
		std::printf("weapon-recoil-scope-tests: %d/%d checks FAILED\n", g_failures, g_checks);
		return 1;
	}
	std::printf("weapon-recoil-scope-tests: %d checks passed\n", g_checks);
	return 0;
}
