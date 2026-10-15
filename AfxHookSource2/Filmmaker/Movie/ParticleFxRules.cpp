// ParticleFx rules: name classification into FxCategory, the FXRULE swap tables mapping
// vanilla CS2 systems to the converted Insurgency 2 / MW2019 "Modern" pack assets, per-mode
// target selection, and swap-target pre-queueing so the resolver stays ahead of creations.

#include "ParticleFxInternal.h"

#include <algorithm>
#include <cstring>

namespace Filmmaker {
namespace fx {

// ============================== classification =====================================

void LowerCopy(const char* in, char* out, size_t cap) {
	size_t i = 0;
	for (; in[i] && i + 1 < cap; ++i) {
		char c = in[i];
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		if (c == '\\') c = '/';
		out[i] = c;
	}
	out[i] = 0;
}

namespace {

bool StartsWith(const char* s, const char* prefix) {
	return 0 == std::strncmp(s, prefix, std::strlen(prefix));
}

} // namespace

// n must already be lowercase (LowerCopy). Returns a FxCategory or -1 (untracked).
// Prefixes verified against the build-14166 pak01 particle list (Source2Viewer dump):
// HE grenades detonate as particles/entity/env_explosion/explosion_hegrenade_*, blood
// also appears under impact_fx/ and as explosions_fx/ screen splatter, and current maps
// (mirage etc.) spawn ambience from the LEGACY particles/maps/de_dust/... names plus the
// generic ambient_fx/environment/rain_fx folders.
int ClassifyLower(const char* n) {
	if (StartsWith(n, "particles/ui/")) return -1;         // HUD/MVP effects: never touch
	// DO NOT route the spectator utility trail here. It is the
	// SPECTATOR X-ray trail: it only exists when spectator grenade trails are enabled, and when
	// they are it deliberately draws THROUGH WALLS. Swapping it fired 11/11 and rendered
	// nothing, because the engine creates it dormant unless that spectator feature is drawing --
	// and making it draw would give every grenade a wallhack trail. A real in-world grenade
	// trail has to be SPAWNED on the projectile entity, not swapped onto an existing effect;
	// CS2 plays no in-flight particle for smoke/HE/flash/decoy at all (verified via `fx names`
	// on a live match and Source2Viewer over pak01).
	if (0 == std::strcmp(n, "particles/entity/spectator_utility_trail.vpcf")) return -1;
	// The Zeus is not a bullet weapon and must never be swapped. Its wire effects are named
	// weapon_tracers_taser*, so the "tracer" test below classifies them as bullet tracers; they
	// match none of the weapon-class substrings in kTracerFallbacks, fall through to the DEFAULT
	// fallback, and the electric wire is replaced by a generic bullet streak -- mw2019_tracer in
	// Modern, weapon_tracers in On. That is the "the default Zeus stuff doesn't work" report, and
	// it affects BOTH modes. Neither pack ships a taser asset, so there is nothing to swap to:
	// the only correct behaviour is to leave every taser system completely alone.
	if (std::strstr(n, "taser")) return -1;
	// Blood is never swapped (the Blood category was removed 2026-09-26: CS2's own blood is plenty).
	// This must stay ahead of the folder rules, or blood systems under impact_fx/ would be taken
	// for bullet impacts and swapped to sparks and debris.
	if (std::strstr(n, "blood")) return -1;
	if (StartsWith(n, "particles/impact_fx/") || StartsWith(n, "particles/water_impact/")
		|| StartsWith(n, "particles/breakable_fx/")) return kFxImpacts;
	// The thrown molotov/incendiary FLIGHT TRAIL lives under cs_weapon_fx/, so route it to the
	// Nade Trails category rather than weaponfx.
	if (std::strstr(n, "molotov_thrown") || std::strstr(n, "incend_thrown")) return kFxMolotov;
	if (StartsWith(n, "particles/weapons/cs_weapon_fx/") || StartsWith(n, "particles/unified_weapon_fx/"))
		return std::strstr(n, "tracer") ? kFxTracers : kFxWeaponFx;
	if (StartsWith(n, "particles/explosions_fx/") || StartsWith(n, "particles/entity/env_explosion/")) {
		if (std::strstr(n, "flashbang")) return kFxFlashbang;
		return (std::strstr(n, "c4") || std::strstr(n, "bomb")) ? kFxBombFx : kFxExplosions;
	}
	if (StartsWith(n, "particles/burning_fx/") || StartsWith(n, "particles/inferno_fx")) return kFxMolotov;
	// Map ambience (particles/maps|ambient_fx|environment|rain_fx|critters/) is not a category: the
	// packs ship no ambient effects to swap it to, so its On never did anything (removed 2026-09-26).
	return -1;
}

const char* kCategoryKeys[kFxCategoryCount] = {
	"impacts", "tracers", "weaponfx", "explosions", "bombfx", "molotov", "flashbang"
};
const char* kModeNames[kModeCount] = { "on", "less", "off", "more", "modern" };

// Which modes each category really has (see FxMode's header comment). Modern only where the
// MW2019 pack ships assets (tracers, weapon fx, HE explosion).
//
// Less is no longer exposed by any category. Impact On internally selects reduced assets, then
// randomizes between the old Povarhok Less tree and the generated Insurgency Less tree.
//
// HE grenade: it resolves to Povarehok, whose On and Less are two separately AUTHORED files, and
// the `less/smoke/` folder is not a reduced build at all -- measured across the two closures,
// Less has MORE than On: 21 systems against 18, total max radius 2735 against 2244, total max
// lifetime 22 s against 16 s, and 15 of the 18 shared systems have an identical max radius. On
// screen that read as more smoke, not less (peak smoke 19.4% of frame against 9.4%). The folder
// name was inherited at face value and was wrong.
//
// Bomb (C4): its Less IS a real 0.70x reduction generated by build-less-variants.py, so this is
// not a correctness fix -- it is the user choosing On/Off for the bomb as well, so the two
// explosion rows behave the same way.
//
// NormalizeMode() maps persisted legacy Less values to On, preserving old projects while the
// settings menu only presents On / Off.
bool ModeSupported(int cat, FxMode mode) {
	switch (mode) {
	case FxMode::On:
	case FxMode::Off:
		return true;
	case FxMode::More: // legacy alias, normalized to On before storage
		return true;
	case FxMode::Less:
		return false;
	case FxMode::Modern:
		return cat == kFxTracers || cat == kFxWeaponFx || cat == kFxExplosions;
	default:
		return false;
	}
}

// Storage normalization: More -> On (legacy), unsupported Less/Modern -> On.
FxMode NormalizeMode(int cat, FxMode mode) {
	if (mode == FxMode::More)
		mode = FxMode::On;
	if (!ModeSupported(cat, mode))
		mode = FxMode::On;
	return mode;
}

int CategoryFromKey(const char* key) {
	for (int i = 0; i < kFxCategoryCount; ++i)
		if (0 == _stricmp(key, kCategoryKeys[i]))
			return i;
	return -1;
}

int ModeFromName(const char* name) {
	// "more" was dropped 2026-07-02 (it targeted a byte-identical asset folder, so On and
	// More never looked different); accept it as an alias of On so old persisted configs
	// and muscle-memory console commands keep working.
	if (0 == _stricmp(name, "more"))
		return (int)FxMode::On;
	for (int i = 0; i < kModeCount; ++i)
		if (0 == _stricmp(name, kModeNames[i]))
			return i;
	return -1;
}

// ============================== the FXRULE swap tables =============================

namespace {

// Mode targets are real Insurgency 2 particle systems converted to Source 2 with
// long0900/source1import and mounted under particles/filmmaker/insurgency/<pcf>/<system>.
//
// PACK REBAKE 2026-09-01. On/Less used to come from the CS:GO "Povarehok" mod, whose
// 214-material closure was stitched together from ~12 donor games (Insurgency 2, Sandstorm,
// Day of Infamy, GTA V, Squad, Hell Let Loose, Just Cause 3, Far Cry 4, MW2019, Black Mesa,
// BrainBread 2, HL2:MMod) with mutually inconsistent VMT conventions -- the root cause behind
// essentially every texture-failure pattern in Docs/porting/povarehok-csgo-mod-reference.md
// section 10, and the reason that pack needed ~1,500 lines of post-process damage control
// while Modern needed almost none. On/Less now come from Insurgency 2's own FX set: ONE game,
// one internally consistent asset library, in the local FX\sources\insurgency2\ (not in git).
//
// Insurgency 2 inherited Source's impact_<surface> and weapon_tracers* naming, so much of the
// impact and tracer tables below is a literal 1:1 name match against CS2's own systems.
//
// Mode mapping, which is now a property of the ASSETS rather than of three separate folder
// conversions:
//   On   -> Insurgency's full system.
//   Less -> Insurgency's own reduced variant of that same system where the game authored one
//           (`_cheap` for the surfaces it made cheap versions of, else the lighter `_b`
//           sibling), otherwise the same system as On. This replaces the old per-FILE
//           combination of two "less" mod folders -- there is ONE converted tree now, and
//           Less is a different row in it, not a separately converted build.
//
// If the converted pack is not mounted, resolution fails open and the original CS2 effect
// plays. There are no stock CS2 placeholder substitutions here.
//
// `modern` is the optional MW2019 (ARC9/GMod, converted via the same source1import
// pipeline) target under particles/filmmaker/modern/<pcf>/<system>.vpcf. Ground-truth
// mapping extracted from the pack's own weapon Lua 2026-07-03 (memory:
// modern-pack-mw2019-mapping): rifles muzzleflash_ar, SMGs+snipers muzzleflash_smg,
// marksman muzzleflash_dmr, pistols muzzleflash_pistol(_deagle), shotguns
// muzzleflash_shotgun, silenced muzzleflash_suppressed, per-shot barrel smoke
// barrel_smoke(_plume), tracers the mw2019_tracer family, HE frag explosion_grenade.
struct VariantRule {
	const char* match;
	const char* on;
	const char* less;   // null together with a null `on` = On/Less pass through to vanilla CS2
	const char* modern; // null = category Modern mode passes this system through
	const char* hybrid; // optional second donor used by the Impact On blend
};

#define FXINS(pack, name) "particles/filmmaker/insurgency/" pack "/" name ".vpcf"
// LESS IS A TRANSFORM OF ON, NOT A DIFFERENT ASSET.
//
// User decision 2026-09-02: "Less is the same as On. It's just that everything is thirty
// percent smaller and fades thirty percent faster, because it's less." Before that each
// Less-capable category pointed at a DIFFERENT Insurgency system (HE Less ->
// ins_m203_explosion), which is exactly why Less read as another effect rather than a
// smaller version of the same one.
//
// The _less systems are generated from their On counterparts by
// FX/tools/build-less-variants.py (sizes x0.70, lifetimes x0.70) and compiled beside them.
#define FXINS_LESS(pack, name) "particles/filmmaker/insurgency/" pack "/" name "_less.vpcf"
#define FXRULE(match, pack, name) { match, FXINS(pack, name), FXINS_LESS(pack, name), nullptr, nullptr }
// `lessName` is NO LONGER USED -- Less is derived from `name`. It is kept in the signature,
// and in the 40-odd call sites, purely as the record of which variant used to be hand-picked,
// so going back is a one-line macro change rather than an archaeology exercise. Do not add
// new callers that expect it to be honoured.
#define FXRULE_L(match, pack, name, lessName) { match, FXINS(pack, name), FXINS_LESS(pack, name), nullptr, nullptr }
// Same as FXRULE plus a converted-MW2019 target ("<pcf folder>/<system>" under modern/).
#define FXRULE_MODERN(match, pack, name, modernRel) { match, \
	FXINS(pack, name), FXINS_LESS(pack, name), \
	"particles/filmmaker/modern/" modernRel ".vpcf", nullptr }
#define FXRULE_MODERN_L(match, pack, name, lessName, modernRel) { match, \
	FXINS(pack, name), FXINS_LESS(pack, name), \
	"particles/filmmaker/modern/" modernRel ".vpcf", nullptr }
// Modern-only: On/Less deliberately pass through to vanilla CS2.
#define FXRULE_MODERN_ONLY(match, modernRel) { match, nullptr, nullptr, \
	"particles/filmmaker/modern/" modernRel ".vpcf", nullptr }

// ---- Povarehok ------------------------------------------------------------------------------
// The pack that preceded Insurgency 2 as the On/Less source. It is still built, compiled and
// (for the systems named below) staged, so a category can be pointed back at it per-effect.
//
// Its tree is split differently from Insurgency's: On lives under `regular/`, and there is a
// SEPARATE AUTHORED TREE under `less/smoke/` rather than a generated `_less` sibling --
// build-less-variants.py does not touch this tree and must not be pointed at it.
//
// THAT `less/smoke` TREE IS NOT A REDUCED BUILD, and nothing points at it any more. Measured
// across the HE grenade's two closures: `less/smoke` has 21 systems against `regular`'s 18,
// total max radius 2735 against 2244 and total max lifetime 22 s against 16 s, with 15 of the 18
// shared systems identical in size -- it is a VARIANT build whose folder name says otherwise. So
// the Less slot below resolves to the SAME asset as On; the category no longer offers Less at
// all (see ModeSupported), and this keeps the slot honest rather than pointing at a heavier
// effect labelled "less".
#define FXPOV(pack, name) "particles/filmmaker/povarehok/regular/" pack "/" name ".vpcf"
#define FXRULE_POV_MODERN(match, pack, name, modernRel) { match, \
	FXPOV(pack, name), FXPOV(pack, name), \
	"particles/filmmaker/modern/" modernRel ".vpcf", nullptr }
#define FXPOV_LESS_IMPACT(name) \
	"particles/filmmaker/povarehok/less/impacts/impact_fx/" name ".vpcf"
#define FXRULE_IMPACT(match, insurgencyName, povarehokName) { match, \
	FXINS("impact_fx_ins", insurgencyName), FXINS_LESS("impact_fx_ins", insurgencyName), \
	nullptr, FXPOV_LESS_IMPACT(povarehokName) }

// Near-literal name matches: Insurgency 2 kept Source's impact_<surface> convention, so most
// of these map onto the game's identically-named system. Less prefers Insurgency's authored
// _cheap row, else the lighter _b sibling. CS2 surfaces Insurgency has no row for
// (sheetrock, wallbang) map onto its nearest equivalent.
const VariantRule kVariantImpacts[] = {
	FXRULE_IMPACT("particles/impact_fx/impact_concrete.vpcf", "impact_concrete", "impact_concrete"),
	FXRULE_IMPACT("particles/impact_fx/impact_plaster.vpcf",  "impact_plaster",  "impact_plaster"),
	FXRULE_IMPACT("particles/impact_fx/impact_brick.vpcf",    "impact_brick",    "impact_brick"),
	FXRULE_IMPACT("particles/impact_fx/impact_tile.vpcf",     "impact_tile",     "impact_tile"),
	// CS2 sheetrock == Insurgency plaster (drywall); it ships no sheetrock row.
	FXRULE_IMPACT("particles/impact_fx/impact_sheetrock.vpcf",   "impact_plaster", "impact_sheetrock"),
	FXRULE_IMPACT("particles/impact_fx/impact_asphalt.vpcf",     "impact_asphalt", "impact_asphalt"),
	FXRULE_IMPACT("particles/impact_fx/impact_rock.vpcf",        "impact_rock",    "impact_rock"),
	FXRULE_IMPACT("particles/impact_fx/impact_dirt.vpcf",        "impact_dirt",    "impact_dirt"),
	FXRULE_IMPACT("particles/impact_fx/impact_plastic.vpcf",     "impact_plastic", "impact_plastic"),
	FXRULE_IMPACT("particles/impact_fx/impact_metal.vpcf",       "impact_metal",   "impact_metal"),
	FXRULE_IMPACT("particles/impact_fx/impact_metal_grate.vpcf", "impact_metal",   "impact_metal"),
	FXRULE_IMPACT("particles/impact_fx/impact_metal_vent.vpcf",  "impact_metal",   "impact_metal"),
	FXRULE_IMPACT("particles/impact_fx/impact_wood.vpcf",        "impact_wood",    "impact_wood"),
	// WALLBANGS ARE DELIBERATELY NOT SWAPPED. impact_wallbang_light / _light_silent / _heavy are
	// CS2's through-penetration puff: they fire beside whatever surface the bullet EXITS into, on
	// every penetrable surface -- including Nuke's breakable glass. Extracted from pak01 2026-09-02,
	// vanilla's is a SINGLE system at radius 2. Every Insurgency impact adds debris that vanilla
	// never draws: the heavy row used to point at impact_concrete_thick (21 rock sprites -- "glass
	// sprays dirt", caught with `fx log on` at tick 3253), and the plaster puff it was moved to
	// still throws 8 opaque `fleks` sprites tinted 84-113 grey, which is the dark chunk that was
	// still there afterwards. There is no chunk-free penetration puff in the pack. With no rule the
	// hook's exact-match lookup misses and the system passes through to vanilla (impacts have no
	// fallback), which is the correct result: the surface's own impact is already swapped.
	FXRULE_IMPACT("particles/impact_fx/impact_ricochet.vpcf", "ricochet_sparks", "impact_ricochet"),

	// ---- the rest of CS2's surface set -------------------------------------------------
	// AUDITED 2026-09-02 against a Source2Viewer dump of pak01: CS2 ships 35 top-level
	// `particles/impact_fx/impact_<surface>` systems and only 12 were mapped, so 23 surfaces --
	// including sand and glass, both named by the user -- played vanilla while everything
	// around them was Insurgency. That is backlog item B18.
	//
	// Insurgency is a military set: it authored asphalt, brick, concrete, dirt, metal, plaster,
	// plastic, rock, tile, wood, sand-dust and gravel, and NOTHING for glass, snow, water, mud,
	// grass, leaves, cloth, carpet, paper, cardboard, rubber, pottery, clay or chainlink
	// (checked by name across the whole converted tree). So the rows below map each remaining
	// surface onto the Insurgency system whose MATERIAL FAMILY actually matches -- loose earth,
	// fired clay, sheet metal, synthetic, soft fibre -- rather than onto a similar-sounding name.
	FXRULE("particles/impact_fx/impact_sand.vpcf",          "impact_fx_ins", "impact_sand_dust"),
	FXRULE("particles/impact_fx/impact_sandbarrel.vpcf",    "impact_fx_ins", "impact_sand_dust"),
	// Loose earth: a dust puff with soft debris, which is what Insurgency's dirt row is.
	FXRULE_IMPACT("particles/impact_fx/impact_mud.vpcf",    "impact_dirt", "impact_dirt"),
	FXRULE_IMPACT("particles/impact_fx/impact_grass.vpcf",  "impact_dirt", "impact_dirt"),
	FXRULE_IMPACT("particles/impact_fx/impact_leaves.vpcf", "impact_dirt", "impact_dirt"),
	// Fired clay IS brick; glazed pottery chips like tile.
	FXRULE_IMPACT("particles/impact_fx/impact_clay.vpcf",    "impact_brick", "impact_brick"),
	FXRULE_IMPACT("particles/impact_fx/impact_pottery.vpcf", "impact_tile",  "impact_tile"),
	// Sheet metal, and the synthetics that spall without sparking.
	FXRULE_IMPACT("particles/impact_fx/impact_chainlink.vpcf",  "impact_metal",   "impact_metal"),
	FXRULE_IMPACT("particles/impact_fx/impact_computer.vpcf",   "impact_plastic", "impact_plastic"),
	FXRULE_IMPACT("particles/impact_fx/impact_electronic.vpcf", "impact_plastic", "impact_plastic"),
	FXRULE_IMPACT("particles/impact_fx/impact_rubber.vpcf",     "impact_plastic", "impact_plastic"),
	// Soft fibrous surfaces: a dust puff with no sparks and no debris trails, which is exactly
	// what the no-trails plaster row does. Nothing in Insurgency is closer.
	FXRULE_IMPACT("particles/impact_fx/impact_cardboard.vpcf",  "impact_plaster_notrails", "impact_sheetrock"),
	FXRULE_IMPACT("particles/impact_fx/impact_paper.vpcf",      "impact_plaster_notrails", "impact_sheetrock"),
	FXRULE_IMPACT("particles/impact_fx/impact_carpet.vpcf",     "impact_plaster_notrails", "impact_sheetrock"),
	FXRULE_IMPACT("particles/impact_fx/impact_cloth.vpcf",      "impact_plaster_notrails", "impact_sheetrock"),
	FXRULE_IMPACT("particles/impact_fx/impact_upholstery.vpcf", "impact_plaster_notrails", "impact_sheetrock"),
	// A bare dust cloud with no impact body of its own.
	FXRULE("particles/impact_fx/impact_particulates.vpcf",  "impact_fx_ins", "impact_concrete_dust"),
	//
	// DELIBERATELY LEFT TO VANILLA CS2, because Insurgency ships no equivalent and substituting
	// a wrong-family asset would look worse than CS2's own:
	//   impact_glass   - glass throws translucent shards; the nearest candidate (tile) throws
	//                    opaque ceramic chips, and CS2's own glass impact is already good.
	//   impact_snow    - no snow asset anywhere in the pack.
	//   impact_water / impact_puddle / impact_wet - liquid splashes; CS2 routes these through
	//                    its own particles/water_impact/ set, which the Impacts category also
	//                    classifies, and Insurgency has no water FX at all.
	//   impact_empty   - a placeholder system, not a surface.

	// ---- CS2's 'simplified' variants -------------------------------------------------------
	// scripts/surfaceproperties_impact_effects.txt binds every surface to TWO systems: `effect`
	// and `effect_simplified` (the `_cheap` sibling), and the engine picks the simplified one at
	// distance / on reduced settings. 17 of those existed and NONE were mapped, so the same
	// concrete hit was Insurgency up close and vanilla CS2 a few metres further away. Each
	// simplified effect resolves to the SAME target as its full version -- the distance switch is
	// CS2's decision to make, not a reason to change material family. (Audited 2026-09-02.)
	FXRULE_IMPACT("particles/impact_fx/impact_carpet_cheap.vpcf",     "impact_plaster_notrails", "impact_sheetrock"),
	FXRULE_IMPACT("particles/impact_fx/impact_computer_cheap.vpcf",   "impact_plastic", "impact_plastic"),
	FXRULE_IMPACT("particles/impact_fx/impact_concrete_cheap.vpcf",   "impact_concrete", "impact_concrete"),
	FXRULE_IMPACT("particles/impact_fx/impact_dirt_cheap.vpcf",       "impact_dirt", "impact_dirt"),
	FXRULE_IMPACT("particles/impact_fx/impact_leaves_cheap.vpcf",     "impact_dirt", "impact_dirt"),
	FXRULE_IMPACT("particles/impact_fx/impact_metal_cheap.vpcf",      "impact_metal", "impact_metal"),
	FXRULE_IMPACT("particles/impact_fx/impact_metal_vent_cheap.vpcf", "impact_metal", "impact_metal"),
	FXRULE_IMPACT("particles/impact_fx/impact_plaster_cheap.vpcf",    "impact_plaster", "impact_plaster"),
	FXRULE_IMPACT("particles/impact_fx/impact_plastic_cheap.vpcf",    "impact_plastic", "impact_plastic"),
	FXRULE_IMPACT("particles/impact_fx/impact_pottery_cheap.vpcf",    "impact_tile", "impact_tile"),
	FXRULE_IMPACT("particles/impact_fx/impact_rubber_cheap.vpcf",     "impact_plastic", "impact_plastic"),
	FXRULE_IMPACT("particles/impact_fx/impact_tile_cheap.vpcf",       "impact_tile", "impact_tile"),
	FXRULE_IMPACT("particles/impact_fx/impact_upholstery_cheap.vpcf", "impact_plaster_notrails", "impact_sheetrock"),
	FXRULE_IMPACT("particles/impact_fx/impact_wood_cheap.vpcf",       "impact_wood", "impact_wood"),
};
// Insurgency's tracer family is named exactly like CS2's (weapon_tracers*) but is per-weapon
// rather than per-class: akm/m16/m14/mosin/mp5/mp40/sks/pistol plus a plain weapon_tracers.
// Mapped by ballistic class. Tracers never use Less (see ModeSupported), so every row's Less
// target simply mirrors On.
const VariantRule kVariantTracers[] = {
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers.vpcf",              "weapon_fx_tracers", "weapon_tracers",        "mw2019_tracer/mw2019_tracer"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_pistol.vpcf",       "weapon_fx_tracers", "weapon_tracers_pistol", "mw2019_tracer/mw2019_tracer_small"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_smg.vpcf",          "weapon_fx_tracers", "weapon_tracers_mp5",    "mw2019_tracer/mw2019_tracer"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_rifle.vpcf",        "weapon_fx_tracers", "weapon_tracers_m14",    "mw2019_tracer/mw2019_tracer_fast"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_rifle_scar.vpcf",   "weapon_fx_tracers", "weapon_tracers_m14",    "mw2019_tracer/mw2019_tracer_fast"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_rifle_ssg.vpcf",    "weapon_fx_tracers", "weapon_tracers_mosin",  "mw2019_tracer/mw2019_tracer_fast"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_assrifle.vpcf",     "weapon_fx_tracers", "weapon_tracers_akm",    "mw2019_tracer/mw2019_tracer"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_assrifle_aug.vpcf", "weapon_fx_tracers", "weapon_tracers_m16",    "mw2019_tracer/mw2019_tracer"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_mach.vpcf",         "weapon_fx_tracers", "weapon_tracers_c",      "mw2019_tracer/mw2019_tracer"),
	// Shotguns create one tracer PER PELLET (~10 a blast). The plain Insurgency weapon_tracers
	// carries a glow child that emits ~50 large glow sprites as it leaves the barrel, so a blast
	// became ~500 orange flecks sprayed around the muzzle instead of pellet streaks (seen in third
	// person, 2026-10-10). weapon_tracers_akm is a single clean streak with no children, at the
	// same 13000 u/s and 500u cap as CS2's own pellet tracer.
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_shot.vpcf",         "weapon_fx_tracers", "weapon_tracers_akm",    "mw2019_tracer/mw2019_tracer_slow"),
};
// Insurgency authors muzzle flashes per real weapon with an explicit first-person (_1p) and
// third-person (_3rd) system, which lines up exactly with CS2's uweapon_muzflsh_X /
// uweapon_muzflsh_X_fps split. Mapped by weapon class:
//   AK-family -> akm, M4/SG -> m16, AUG -> ak74, SMG -> mp5, LMG -> m249,
//   shotgun -> m590, pistol -> makarov, Deagle/R8 -> m9 (the larger frame),
//   marksman/AWP -> m14, auto-sniper -> fal, silenced -> suppressed.
// Insurgency ships ONE suppressed system (first-person only), used for both views.
const VariantRule kVariantWeaponFx[] = {
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_ak47.vpcf",         "weapon_fx_ins",   "muzzleflash_akm_3rd",  "arc9_fas_muzzleflashes/muzzleflash_ar"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_ak47_fps.vpcf",     "weapon_fx_ins",   "muzzleflash_akm_1p",   "arc9_fas_muzzleflashes/muzzleflash_ar_fp"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_riffle.vpcf",       "weapon_fx_ins",   "muzzleflash_m16_3rd",  "arc9_fas_muzzleflashes/muzzleflash_ar"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_riffle_fps.vpcf",   "weapon_fx_ins",   "muzzleflash_m16_1p",   "arc9_fas_muzzleflashes/muzzleflash_ar_fp"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_aug.vpcf",          "weapon_fx_ins_b", "muzzleflash_ak74_3rd", "arc9_fas_muzzleflashes/muzzleflash_ar"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_aug_fps.vpcf",      "weapon_fx_ins_b", "muzzleflash_ak74_1p",  "arc9_fas_muzzleflashes/muzzleflash_ar_fp"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_shot.vpcf",         "weapon_fx_ins",   "muzzleflash_m590_3rd", "arc9_fas_muzzleflashes/muzzleflash_shotgun"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_shot_fps.vpcf",     "weapon_fx_ins",   "muzzleflash_m590_1p",  "arc9_fas_muzzleflashes/muzzleflash_shotgun_fp"),
	// The MW2019 pack's own big-bore snipers (AX-50/HDR/Rytec) use the SMG flash; the mvm
	// sniper composition wraps it with the pack's .50-cal extras the mod adds via Lua --
	// M82 shock-dust ring around the shooter, heavy barrel plume, muzzle heat distortion
	// (synthesized by postprocess_modern.py's apply_modern_gameplay_composites, user request 2026-07-03).
	FXRULE_MODERN("particles/unified_weapon_fx/weapon_muzzleflash_snip.vpcf",      "weapon_fx_ins", "muzzleflash_m14_3rd", "arc9_fas_muzzleflashes/mvm_muzzleflash_sniper_awp"),
	FXRULE_MODERN("particles/unified_weapon_fx/weapon_muzzleflash_snip_fps.vpcf",  "weapon_fx_ins", "muzzleflash_m14_1p",  "arc9_fas_muzzleflashes/mvm_muzzleflash_sniper_awp_fp"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzzleflash_subm.vpcf",     "weapon_fx_ins", "muzzleflash_mp5_3rd", "arc9_fas_muzzleflashes/muzzleflash_smg"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzzleflash_subm_fps.vpcf", "weapon_fx_ins", "muzzleflash_mp5_1p",  "arc9_fas_muzzleflashes/muzzleflash_smg_fp"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzsilenced_subm.vpcf",     "weapon_fx_ins", "muzzleflash_suppressed_1p", "arc9_fas_muzzleflashes/muzzleflash_suppressed"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzsilenced_subm_fps.vpcf", "weapon_fx_ins", "muzzleflash_suppressed_1p", "arc9_fas_muzzleflashes/muzzleflash_suppressed_fp"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzsilenced_rif.vpcf",      "weapon_fx_ins", "muzzleflash_suppressed_1p", "arc9_fas_muzzleflashes/muzzleflash_suppressed"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzsilenced_rif_fps.vpcf",  "weapon_fx_ins", "muzzleflash_suppressed_1p", "arc9_fas_muzzleflashes/muzzleflash_suppressed_fp"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzzleflash_pist.vpcf",     "weapon_fx_ins", "muzzleflash_makarov_3rd", "arc9_fas_muzzleflashes/muzzleflash_pistol"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzzleflash_pist_fps.vpcf", "weapon_fx_ins", "muzzleflash_makarov_1p",  "arc9_fas_muzzleflashes/muzzleflash_pistol_fp"),
	// Deagle/R8, ironsight (scoped) AUG/SG556, and LMG muzzles are separate
	// top-level systems (verified via `fx names` on a live demo, 2026-07-02).
	// The Deagle emits its OWN CS2 name, so unlike the auto-snipers it needs no runtime
	// per-weapon override -- it just needs its own ON asset. Insurgency's m9 is the
	// larger-frame pistol flash, which suits the Deagle's long barrel better than the makarov.
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_deagle.vpcf",            "weapon_fx_ins",   "muzzleflash_m9_3rd",  "arc9_fas_muzzleflashes/muzzleflash_pistol_deagle"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_deagle_fps.vpcf",        "weapon_fx_ins",   "muzzleflash_m9_1p",   "arc9_fas_muzzleflashes/muzzleflash_pistol_deagle_fp"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_aug_fps_ironsight.vpcf", "weapon_fx_ins_b", "muzzleflash_ak74_1p", "arc9_fas_muzzleflashes/muzzleflash_ar_fp"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_sg_fps_ironsight.vpcf",  "weapon_fx_ins",   "muzzleflash_m16_1p",  "arc9_fas_muzzleflashes/muzzleflash_ar_fp"),
	// riffle_lrg = SCAR-20/G3SG1 autosnipers: Insurgency's FAL is its battle-rifle flash.
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_riffle_lrg.vpcf",     "weapon_fx_ins", "muzzleflash_fal_3rd", "arc9_fas_muzzleflashes/muzzleflash_smg"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_riffle_lrg_fps.vpcf", "weapon_fx_ins", "muzzleflash_fal_1p",  "arc9_fas_muzzleflashes/muzzleflash_smg_fp"),
	// weapon_muzzleflash_snip_ar(_fps) is the REAL top-level auto-sniper (SCAR-20/G3SG1)
	// system (verified via `fx names` on the all-weapons test demo 2026-07-03 -- it was
	// unmapped and passing through 100% vanilla, the actual cause of a user-reported
	// "autosniper doesn't match the rest" look).
	FXRULE_MODERN("particles/unified_weapon_fx/weapon_muzzleflash_snip_ar.vpcf",     "weapon_fx_ins", "muzzleflash_fal_3rd", "arc9_fas_muzzleflashes/mvm_muzzleflash_sniper_auto"),
	FXRULE_MODERN("particles/unified_weapon_fx/weapon_muzzleflash_snip_ar_fps.vpcf", "weapon_fx_ins", "muzzleflash_fal_1p",  "arc9_fas_muzzleflashes/mvm_muzzleflash_sniper_auto_fp"),
	// uweapon_muzflsh_mach(_fps) is the LMG (M249/Negev) top-level system -- also unmapped
	// (verified live 2026-07-03), so LMGs got 100% vanilla flash under Modern. Insurgency's
	// own M249 is the exact same gun.
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_mach.vpcf",     "weapon_fx_ins", "muzzleflash_m249_3rd", "arc9_fas_muzzleflashes/muzzleflash_lmg"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_mach_fps.vpcf", "weapon_fx_ins", "muzzleflash_m249_1p",  "arc9_fas_muzzleflashes/muzzleflash_lmg_fp"),
	// R8 primary fire ("fanning") uses its own pist_revolver systems, distinct
	// from the deagle-shared secondary fire.
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzzleflash_pist_revolver.vpcf",      "weapon_fx_ins", "muzzleflash_m9_3rd", "arc9_fas_muzzleflashes/muzzleflash_pistol_deagle"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzzleflash_pist_revolver_fps.vpcf",  "weapon_fx_ins", "muzzleflash_m9_1p",  "arc9_fas_muzzleflashes/muzzleflash_pistol_deagle_fp"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzzleflash_pist_fire_revolver.vpcf", "weapon_fx_ins", "muzzleflash_m9_3rd", "arc9_fas_muzzleflashes/muzzleflash_pistol_deagle"),
	// Brass shell casings: MODERN ONLY. Modern renders its own mw2019 brass (rebuilt from the
	// committed templates in postprocess_modern.patch_modern_shell_port_smoke). Insurgency's
	// shell_eject is a sprite ejection-port PUFF with no model renderer, so swapping CS2's
	// model-based brass for it would DELETE the visible casings -- On/Less therefore pass
	// through to vanilla CS2's own casings, which are already model-based and correct.
	// The cosmetic-only weapon_shell_casing_super_trail stays untouched.
	FXRULE_MODERN_ONLY("particles/weapons/cs_weapon_fx/weapon_shell_casing_9mm.vpcf",          "weapons/cs_weapon_fx/weapon_shell_casing_9mm"),
	FXRULE_MODERN_ONLY("particles/weapons/cs_weapon_fx/weapon_shell_casing_45acp.vpcf",        "weapons/cs_weapon_fx/weapon_shell_casing_9mm"),
	FXRULE_MODERN_ONLY("particles/weapons/cs_weapon_fx/weapon_shell_casing_57.vpcf",           "weapons/cs_weapon_fx/weapon_shell_casing_9mm"),
	FXRULE_MODERN_ONLY("particles/weapons/cs_weapon_fx/weapon_shell_casing_rifle.vpcf",        "weapons/cs_weapon_fx/weapon_shell_casing_rifle"),
	FXRULE_MODERN_ONLY("particles/weapons/cs_weapon_fx/weapon_shell_casing_awp.vpcf",          "weapons/cs_weapon_fx/weapon_shell_casing_50cal"),
	FXRULE_MODERN_ONLY("particles/weapons/cs_weapon_fx/weapon_shell_casing_deagle.vpcf",       "weapons/cs_weapon_fx/weapon_shell_casing_9mm"),
	FXRULE_MODERN_ONLY("particles/weapons/cs_weapon_fx/weapon_shell_casing_shotgun.vpcf",      "weapons/cs_weapon_fx/weapon_shell_casing_shotgun"),
	FXRULE_MODERN_ONLY("particles/weapons/cs_weapon_fx/weapon_shell_casing_shotgun_mag7.vpcf", "weapons/cs_weapon_fx/weapon_shell_casing_shotgun"),
	FXRULE_MODERN_ONLY("particles/weapons/cs_weapon_fx/weapon_shell_casing_shotgun_nova.vpcf", "weapons/cs_weapon_fx/weapon_shell_casing_shotgun"),
	FXRULE_MODERN("particles/unified_weapon_fx/weapon_muzzleflash_basic.vpcf", "weapon_fx_ins", "muzzleflash_akm_3rd", "arc9_fas_muzzleflashes/muzzleflash_ar"),
	// Near-ground muzzle blast dust: CS2 spawns this system by proximity, the MW2019 mod's
	// .50-cal Lua spawns engine ThumperDust at the feet -- the FAS M82 shock dust is that
	// pack's own PCF equivalent. Insurgency authors the same idea as muzzleflash_dust_kickup.
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_ground_smoke.vpcf", "weapon_fx_ins", "muzzleflash_dust_kickup", "arc9_fas_muzzleflashes/m82_shocksmoke"),
};
// HE grenade (+ generic env explosions).
//
// ON IS POVAREHOK'S ac_grenade_explosion. Reverted here 2026-09-02 at the user's request:
// "retune the old povarehok he grenade, only the he grenade -- remove what we have now, I
// don't like it." This row is the ONLY category pointed back at Povarehok; every other
// category still resolves to Insurgency 2.
//
// LESS IS POVAREHOK'S OWN AUTHORED VARIANT, not a generated one. That tree ships a separate
// `less/smoke/explosions_fx/ac_grenade_explosion.vpcf` (23 systems against On's 20, and a
// different file -- checked, not assumed), so the 0.70x transform in build-less-variants.py
// does not apply and must never be pointed at this tree.
//
// The Insurgency HE tuning that used to live here -- HE_BLAST_SIZE_SCALE, the flash cut, the
// trail-length scaling and the forked shared children -- is GONE from postprocess_insurgency.py,
// because nothing resolves to ins_grenade_explosion any more. The pack-wide passes it grew
// alongside (dropping $mod2x-only and renderer-less systems) are kept: those still serve the
// C4, the M203 and the M590 muzzle flash.
//
// Povarehok had the SAME SpriteCard gap as Insurgency -- 594 of its materials had no .vmat at
// all, 8 of them inside this closure. They are converted and compiled now; without that this
// effect would draw white quads exactly like the Insurgency one did.
// Modern = the MW2019 frag's real detonation system (thrownfrag entity ->
// cod2019_grenade_explosion effect -> explosion_grenade from the FAS explosions PCF).
const VariantRule kVariantExplosions[] = {
	FXRULE_POV_MODERN("particles/explosions_fx/explosion_basic.vpcf",              "explosions_fx", "ac_grenade_explosion", "arc9_fas_explosions/explosion_grenade"),
	FXRULE_POV_MODERN("particles/explosions_fx/explosion_hegrenade_brief.vpcf",    "explosions_fx", "ac_grenade_explosion", "arc9_fas_explosions/explosion_grenade"),
	FXRULE_POV_MODERN("particles/entity/env_explosion/explosion_hegrenade_a.vpcf", "explosions_fx", "ac_grenade_explosion", "arc9_fas_explosions/explosion_grenade"),
	FXRULE_POV_MODERN("particles/entity/env_explosion/explosion_hegrenade_b.vpcf", "explosions_fx", "ac_grenade_explosion", "arc9_fas_explosions/explosion_grenade"),
	FXRULE_POV_MODERN("particles/entity/env_explosion/explosion_hegrenade_c.vpcf", "explosions_fx", "ac_grenade_explosion", "arc9_fas_explosions/explosion_grenade"),
	FXRULE_POV_MODERN("particles/entity/env_explosion/explosion_hegrenade_d.vpcf", "explosions_fx", "ac_grenade_explosion", "arc9_fas_explosions/explosion_grenade"),
	FXRULE_POV_MODERN("particles/entity/env_explosion/explosion_hegrenade_e.vpcf", "explosions_fx", "ac_grenade_explosion", "arc9_fas_explosions/explosion_grenade"),
	FXRULE_POV_MODERN("particles/entity/env_explosion/explosion_hegrenade_f.vpcf", "explosions_fx", "ac_grenade_explosion", "arc9_fas_explosions/explosion_grenade"),
	FXRULE_POV_MODERN("particles/entity/env_explosion/explosion_hegrenade_g.vpcf", "explosions_fx", "ac_grenade_explosion", "arc9_fas_explosions/explosion_grenade"),
	FXRULE_POV_MODERN("particles/entity/env_explosion/explosion_hegrenade_h.vpcf", "explosions_fx", "ac_grenade_explosion", "arc9_fas_explosions/explosion_grenade"),
	// The all-weapons test demo (2026-07-03) showed current CS2 HE detonations ALSO create
	// this plain env_explosion/explosion_grenade name (4 creations, none acted) alongside
	// explosion_basic -- an unmapped chunk of every HE blast played vanilla.
	FXRULE_POV_MODERN("particles/entity/env_explosion/explosion_grenade.vpcf",     "explosions_fx", "ac_grenade_explosion", "arc9_fas_explosions/explosion_grenade"), // fx-verify: runtime-only (created by the engine; ships as no standalone VPK entry)
};
// The planted-bomb blast, split from the HE category so the two can differ. Insurgency's
// ins_c4_explosion is a real demolition charge, exactly the right scale for the bomb-site
// blast; the CS2 engine picks the exterior/interior/short variant by site, all mapped to the
// one bomb system, with the frag blast as the Less version.
// Deliberately NO Modern target: the MW2019 pack's C4 is a small breaching charge, not a
// bomb-site blast (user call, 2026-07-03).
const VariantRule kVariantBomb[] = {
	FXRULE_L("particles/explosions_fx/explosion_c4_500.vpcf",          "explosion_fx_ins", "ins_c4_explosion", "ins_grenade_explosion"),
	FXRULE_L("particles/explosions_fx/explosion_c4_500_fallback.vpcf", "explosion_fx_ins", "ins_c4_explosion", "ins_grenade_explosion"),
	FXRULE_L("particles/explosions_fx/explosion_c4_interior.vpcf",     "explosion_fx_ins", "ins_c4_explosion", "ins_grenade_explosion"),
	FXRULE_L("particles/explosions_fx/explosion_c4_short.vpcf",        "explosion_fx_ins", "ins_c4_explosion", "ins_grenade_explosion"),
};
// FLASH GRENADE: the MW2019 flashbang (GMod ARC9 [Modern Warfare 2019],
// arc9_cod2019_thrownflash) detonates as ParticleEffect("Generic_explo_flash") from
// mw2019_explosions_pak.pcf: a flash with sparks, debris, a smoke shockwave, a lingering smoke
// cloud, shrapnel streaks and a glow. On is that system; Off is CS2's own flashbang.
// FLASH GRENADE: On BLOCKS CS2's own flash; the MW2019 flash is created at the detonation point
// instead (ParticleFxFlashbang.cpp says why a swap cannot work for it).
const VariantRule kVariantFlashbang[] = {
	{ "particles/explosions_fx/explosion_flashbang.vpcf", kEmptySystem, kEmptySystem, nullptr, nullptr },
};
// NADE TRAILS: the in-flight smoke trail on thrown grenades. Nothing else -- no ground fire.
//
// User calls, 2026-09-02: "just keep vanilla fire, but I want the trail", then "add the smoke
// trail for the smoke to it too", then "rename the fire trails to nade trails". So this one
// toggle owns the Molotov/incendiary trail plus the smoke-grenade rope trail; its category
// label is "Nade Trails".
//
// GROUND FIRE IS DELIBERATELY ABSENT: every particles/inferno_fx/ system falls through and
// plays vanilla CS2. Insurgency 2 has no incendiary-pool effect to donate -- it has no such
// weapon -- and its env_fire_* systems are ENTITY campfires: authored large, bright and opaque
// for one fire at one spot. CS2's inferno instead spawns one groundfire per flame position
// across the burning area, so every position drew its own fat opaque fireball and the blaze
// read as a row of separate blobs (screenshots 2026-09-01). Vanilla CS2's fire is purpose-built
// for that spawning model and simply looks better, so a molotov that LANDS is 100% stock again.
//
// What is worth keeping is Insurgency's molotov FLIGHT trail: a rope smoke ribbon, burning fuel
// drips and a glow, authored for a thrown bottle.
//   - molotov / incendiary get the full molotov_trail (ribbon + drips + glow).
//   - smoke has no in-flight particle in CS2, so the grenade trail renderer creates the smoke
//     ribbon directly on smokegrenade_projectile. It uses its OWN forked copy
//     (mvm_nade_trail_smoke, synthesized by postprocess_insurgency): the molotov's ribbon is
//     authored near-black for reading against flame, which is invisible on a nade arcing over a
//     lit map -- and brightening the shared system in place wrecked the molotov. Fuel drips and
//     fire glow stay molotov-only too.
const VariantRule kVariantMolotov[] = {
	FXRULE("particles/weapons/cs_weapon_fx/weapon_molotov_thrown.vpcf", "weapon_fx_ins", "molotov_trail"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_incend_thrown.vpcf",  "weapon_fx_ins", "molotov_trail"),
};

#undef FXRULE_MODERN_ONLY
#undef FXRULE_IMPACT
#undef FXPOV_LESS_IMPACT
#undef FXRULE_MODERN_L
#undef FXRULE_MODERN
#undef FXRULE_L
#undef FXRULE
#undef FXINS

// ============================== target selection ===================================

const char* SelectVariantTarget(FxMode mode, const VariantRule& rule) {
	switch (mode) {
	case FxMode::On:
	case FxMode::More: // legacy alias (pre-restructure persisted configs); same target as On
		return rule.on;
	case FxMode::Less:
		return rule.less;
	case FxMode::Modern:
		// null modern target = this system has no MW2019 equivalent; pass through vanilla
		// (do NOT silently mix the Insurgency pack into a Modern category).
		return rule.modern;
	default:
		return nullptr;
	}
}

// Fallback for CS2 tracer systems NOT in kVariantTracers' exact-match table (root-cause
// fix for "tracers don't fire for every weapon", 2026-07-03). The muzzle-flash table
// needed several live-demo-discovered rows beyond what "obvious" pak names suggested
// (weapon_muzzleflash_snip_ar was a hidden top-level name found only via `fx names` on
// a live demo -- see the kVariantWeaponFx comment above); tracers were never given that
// same live audit, and a purely exact-match table is perpetually one CS2
// update/unaudited-weapon-class behind. Rather than guess a literal name with no live
// data to confirm it (the repo rule is "map from live `fx names`, never from pak
// listings"), classify ANY untabled kFxTracers name by weapon-class substring, mirroring
// the groupings the exact table already encodes (e.g. it already folds rifle_scar and
// rifle_ssg into the same "rifle" bucket). A name this table has never seen still gets a
// sensible pack tracer instead of silently staying 100% vanilla in both On and Modern.
struct TracerFallback {
	const char* substr;
	const char* on;     // Insurgency tracer system (weapon_fx_tracers/<on>.vpcf)
	const char* modern; // mw2019_tracer/<modern>.vpcf
};
const TracerFallback kTracerFallbacks[] = {
	{ "pistol",    "weapon_tracers_pistol", "mw2019_tracer_small" },
	{ "revolver",  "weapon_tracers_pistol", "mw2019_tracer_small" },
	{ "deagle",    "weapon_tracers_pistol", "mw2019_tracer_small" },
	{ "shot",      "weapon_tracers_akm",    "mw2019_tracer_slow" }, // per-pellet: no glow child (see the table)
	{ "mach",      "weapon_tracers_c",      "mw2019_tracer" },
	{ "para",      "weapon_tracers_c",      "mw2019_tracer" },
	{ "smg",       "weapon_tracers_mp5",    "mw2019_tracer" },
	{ "aug",       "weapon_tracers_m16",    "mw2019_tracer" },
	{ "assrifle",  "weapon_tracers_akm",    "mw2019_tracer" },
	{ "ssg",       "weapon_tracers_mosin",  "mw2019_tracer_fast" },
	{ "snip",      "weapon_tracers_m14",    "mw2019_tracer_fast" },
	{ "awp",       "weapon_tracers_m14",    "mw2019_tracer_fast" },
	{ "scar",      "weapon_tracers_m14",    "mw2019_tracer_fast" },
	{ "lrg",       "weapon_tracers_m14",    "mw2019_tracer_fast" },
	{ "rifle",     "weapon_tracers_m14",    "mw2019_tracer_fast" },
};
constexpr const char* kTracerFallbackDefaultOn = "weapon_tracers";
constexpr const char* kTracerFallbackDefaultModern = "mw2019_tracer";

// Tracers only support On/Modern (never Less -- see ModeSupported), so no lessImpacts/
// lessSmoke case here. Returns a pointer valid until the next call on this thread
// (thread_local buffer; the caller copies it into a std::string immediately).
const char* TracerFallbackTarget(FxMode mode, const char* n) {
	if (mode != FxMode::On && mode != FxMode::Modern)
		return nullptr;
	const char* onName = kTracerFallbackDefaultOn;
	const char* modernName = kTracerFallbackDefaultModern;
	for (const TracerFallback& f : kTracerFallbacks) {
		if (std::strstr(n, f.substr)) {
			onName = f.on;
			modernName = f.modern;
			break;
		}
	}
	static thread_local std::string s_buf;
	if (mode == FxMode::On)
		s_buf = std::string("particles/filmmaker/insurgency/weapon_fx_tracers/") + onName + ".vpcf";
	else
		s_buf = std::string("particles/filmmaker/modern/mw2019_tracer/") + modernName + ".vpcf";
	return s_buf.c_str();
}

} // namespace

const char* VariantTargetLower(int cat, FxMode mode, const char* n) {
	if (mode == FxMode::Off)
		return nullptr;
	if (cat == kFxImpacts && mode == FxMode::On)
		mode = FxMode::Less; // Impact On intentionally uses the reduced internal profile.
	const VariantRule* rules = nullptr;
	size_t count = 0;
	switch (cat) {
	case kFxWeaponFx:   rules = kVariantWeaponFx;   count = sizeof(kVariantWeaponFx) / sizeof(rules[0]); break;
	case kFxTracers:    rules = kVariantTracers;    count = sizeof(kVariantTracers) / sizeof(rules[0]); break;
	case kFxExplosions: rules = kVariantExplosions; count = sizeof(kVariantExplosions) / sizeof(rules[0]); break;
	case kFxBombFx:     rules = kVariantBomb;       count = sizeof(kVariantBomb) / sizeof(rules[0]); break;
	case kFxImpacts:    rules = kVariantImpacts;    count = sizeof(kVariantImpacts) / sizeof(rules[0]); break;
	case kFxMolotov:    rules = kVariantMolotov;    count = sizeof(kVariantMolotov) / sizeof(rules[0]); break;
	case kFxFlashbang:  rules = kVariantFlashbang;  count = sizeof(kVariantFlashbang) / sizeof(rules[0]); break;
	default: return nullptr;
	}
	for (size_t i = 0; i < count; ++i)
		if (0 == std::strcmp(n, rules[i].match))
			return SelectVariantTarget(mode, rules[i]);
	if (cat == kFxTracers)
		return TracerFallbackTarget(mode, n);
	return nullptr;
}

const char* ImpactHybridTargetLower(const char* n) {
	for (const VariantRule& rule : kVariantImpacts)
		if (0 == std::strcmp(n, rule.match))
			return rule.hybrid;
	return nullptr;
}

std::string ImpactNoSmokeTrailTarget(const char* target) {
	if (!target || !target[0])
		return {};
	std::string result(target);
	const size_t extension = result.rfind(".vpcf");
	if (extension == std::string::npos)
		return {};
	result.insert(extension, "_nosmoketrails");
	return result;
}

// THE PER-SURFACE VANILLA LAYER (2026-09-03). Impact On no longer replaces CS2's impact: the
// game's own effect for the surface that was hit plays underneath ours. Full account in
// FX/tools/layer-vanilla-impacts.py; the short version is that 45 CS2 impact surfaces share 24
// of our systems, so a static child inside a shared system can only ever name ONE vanilla
// effect (the first cut gave grass CS2's dirt). The fix is one thin wrapper per (target,
// surface) pair, generated by that tool, and this function is how the hook names it.
//
// THE NAME IS A CONTRACT with the generator: swap the target's directory (impact_fx_ins or
// impact_fx) for `impact_layered/` and append `__<surface>` to the stem, where <surface> is the
// vanilla name's stem without its `impact_` prefix:
//     impact_fx_ins/impact_dirt_less.vpcf  x  particles/impact_fx/impact_grass.vpcf
//         -> particles/filmmaker/insurgency/impact_layered/impact_dirt_less__grass.vpcf
// Returns empty for anything that is not an impact-tree path so callers keep the plain target.
std::string ImpactSurfaceLayeredTarget(const char* target, const char* vanillaName) {
	if (!target || !target[0] || !vanillaName || !vanillaName[0])
		return {};
	const std::string t(target);
	const size_t ext = t.rfind(".vpcf");
	const size_t slash = t.rfind('/');
	if (ext == std::string::npos || slash == std::string::npos || slash == 0)
		return {};
	const size_t dirSlash = t.rfind('/', slash - 1);
	if (dirSlash == std::string::npos)
		return {};
	const std::string dir = t.substr(dirSlash + 1, slash - dirSlash - 1);
	if (dir != "impact_fx_ins" && dir != "impact_fx")
		return {};
	std::string surface(vanillaName);
	const size_t vslash = surface.rfind('/');
	if (vslash != std::string::npos)
		surface.erase(0, vslash + 1);
	if (surface.size() > 5 && surface.compare(surface.size() - 5, 5, ".vpcf") == 0)
		surface.erase(surface.size() - 5);
	if (surface.rfind("impact_", 0) == 0)
		surface.erase(0, 7);
	if (surface.empty())
		return {};
	return t.substr(0, dirSlash + 1) + "impact_layered/"
		+ t.substr(slash + 1, ext - slash - 1) + "__" + surface + ".vpcf";
}

// ============================== swap-target pre-queueing ===========================

bool HasActiveFxLocked() {
	if (!g_enabled)
		return false;
	if (g_mollyLights.load() || g_heLights.load() || g_flashLights.load() || g_moneyHeadshot || !g_customRules.empty())
		return true;
	for (int i = 0; i < kFxCategoryCount; ++i)
		if (g_modes[i] != FxMode::Off)
			return true;
	return false;
}

void RebuildActiveSwapTargetsLocked(bool invalidateObsoleteHandles) {
	std::vector<std::string> desired;
	std::vector<std::string> eager;
	auto addActive = [&desired](const char* n) {
		if (!n || !n[0])
			return;
		if (std::find(desired.begin(), desired.end(), n) == desired.end())
			desired.push_back(n);
	};
	auto addEager = [&addActive, &eager](const char* n) {
		addActive(n);
		if (n && n[0] && std::find(eager.begin(), eager.end(), n) == eager.end())
			eager.push_back(n);
	};
	auto selected = [&desired](const char* n) {
		return n && std::find(desired.begin(), desired.end(), n) != desired.end();
	};
	bool needsEmptySystem = false;
	for (const CustomRule& r : g_customRules) {
		if (r.target.empty())
			needsEmptySystem = true;
		else
			addEager(r.target.c_str());
	}
	if (needsEmptySystem)
		addEager(kEmptySystem);
	// FP-FX and blood suppression block to dev/empty regardless of the category modes.
	if (g_fpFxSuppress.load(std::memory_order_relaxed) || g_bloodSuppress.load(std::memory_order_relaxed))
		addEager(kEmptySystem);
	if (g_moneyHeadshot)
		addActive(kMoneyBurst);
	// Not a swap target (the rule above blocks to dev/empty), but created by name on every
	// flashbang_detonate: load it with the rest so the first flash does not hitch.
	if (g_modes[kFxFlashbang] != FxMode::Off)
		for (const char* part : kMw2019FlashbangParts)
			addEager(part);
	const VariantRule* tables[] = { kVariantWeaponFx, kVariantTracers,
		kVariantExplosions, kVariantBomb, kVariantImpacts, kVariantMolotov, kVariantFlashbang };
	const size_t counts[] = {
		sizeof(kVariantWeaponFx) / sizeof(VariantRule), sizeof(kVariantTracers) / sizeof(VariantRule),
		sizeof(kVariantExplosions) / sizeof(VariantRule), sizeof(kVariantBomb) / sizeof(VariantRule),
		sizeof(kVariantImpacts) / sizeof(VariantRule), sizeof(kVariantMolotov) / sizeof(VariantRule),
		sizeof(kVariantFlashbang) / sizeof(VariantRule)
	};
	const FxCategory cats[] = { kFxWeaponFx, kFxTracers, kFxExplosions, kFxBombFx, kFxImpacts, kFxMolotov, kFxFlashbang };
	for (int t = 0; t < 7; ++t) {
		const FxMode mode = g_modes[cats[t]];
		if (mode == FxMode::Off)
			continue;
		for (size_t i = 0; i < counts[t]; ++i) {
			const FxMode targetMode = cats[t] == kFxImpacts && mode == FxMode::On
				? FxMode::Less : mode;
			if (const char* target = SelectVariantTarget(targetMode, tables[t][i])) {
				addActive(target);
				if (cats[t] == kFxImpacts) {
					// Only Insurgency gets a no-trails closure. Povarhok's hybrid
					// target remains intact so its authored local puff stays visible.
					const std::string noSmokeTrails = ImpactNoSmokeTrailTarget(target);
					addActive(noSmokeTrails.c_str());
					addActive(tables[t][i].hybrid);
					// The per-surface vanilla wrappers are what the hook actually names (see
					// ImpactSurfaceLayeredTarget), so they warm here too. The plain targets
                    // above stay warm as the wrapper's child roots. Until a wrapper
                    // is ready, the hook leaves CS2's native impact intact.
					const char* vanilla = tables[t][i].match;
					const std::string layered[] = {
						ImpactSurfaceLayeredTarget(target, vanilla),
						ImpactSurfaceLayeredTarget(noSmokeTrails.c_str(), vanilla),
						ImpactSurfaceLayeredTarget(tables[t][i].hybrid, vanilla),
					};
					for (const std::string& l : layered)
						if (!l.empty())
							addActive(l.c_str());
				}
			}
		}
	}
	// Smoke grenades have no stock in-flight particle to swap. Their projectile hook creates
	// this smoke-only rope wrapper directly, so warm it with the rest of Nade Trails.
	if (g_modes[kFxMolotov] != FxMode::Off)
		addEager("particles/filmmaker/insurgency/weapon_fx_ins/mvm_nade_trail.vpcf");
	// Keep every TracerFallbackTarget bucket eligible too (not just the exact-table
	// targets above), but do not force-load them during demo entry. Untabled tracer names
	// may fail open once, then queue for paused warm-up.
	if (const FxMode tracerMode = g_modes[kFxTracers]; tracerMode == FxMode::On || tracerMode == FxMode::Modern) {
		addActive(TracerFallbackTarget(tracerMode, ""));
		for (const TracerFallback& f : kTracerFallbacks)
			addActive(TracerFallbackTarget(tracerMode, f.substr));
	}

	if (invalidateObsoleteHandles) {
		for (auto it = g_handleCache.begin(); it != g_handleCache.end();) {
			const bool selected = std::find(desired.begin(), desired.end(), it->first) != desired.end();
			// Explicit settings changes retry failed selected targets immediately,
			// while ready handles shared with the new selection stay hot.
			if (!selected || !it->second.handle)
				it = g_handleCache.erase(it);
			else
				++it;
		}
	}

	g_activeTargets.clear();
	g_resolveQueue.clear();
	if (!g_enabled) {
		// The FP-FX suppression gate must stay functional with the EFFECTS master off:
		// keep dev/empty resolvable so the hook's block target can swap.
		if (g_fpFxSuppress.load(std::memory_order_relaxed) || g_bloodSuppress.load(std::memory_order_relaxed)) {
			g_activeTargets.push_back(kEmptySystem);
			if (g_handleCache.find(kEmptySystem) == g_handleCache.end())
				g_resolveQueue.push_back(kEmptySystem);
		}
		return;
	}

	g_activeTargets = desired;
	for (const std::string& name : eager)
		if (g_handleCache.find(name) == g_handleCache.end())
			g_resolveQueue.push_back(name);
}

void RebuildActiveSwapTargets(bool invalidateObsoleteHandles) {
	std::lock_guard<std::mutex> lock(g_mx);
	RebuildActiveSwapTargetsLocked(invalidateObsoleteHandles);
}

void QueueAllUncachedActiveTargetsLocked() {
	for (const std::string& name : g_activeTargets) {
		if (g_handleCache.find(name) != g_handleCache.end())
			continue;
		if (std::find(g_resolveQueue.begin(), g_resolveQueue.end(), name) != g_resolveQueue.end())
			continue;
		g_resolveQueue.push_back(name);
	}
}

} // namespace fx
} // namespace Filmmaker
