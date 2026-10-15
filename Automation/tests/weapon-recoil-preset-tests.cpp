#include "../../AfxHookSource2/Filmmaker/Movie/WeaponRecoilPresets.h"

#include <cassert>
#include <iostream>

using namespace Filmmaker;

int main() {
	WeaponRecoilPresetBook book;
	WeaponRecoilPreset rifles;
	rifles.name = "  Heavy Rifle Recoil  ";
	rifles.scope = RecoilScope::Type;
	rifles.key = "RIFLE";
	rifles.style = RecoilStyle::Mw2022;
	rifles.tuning.amount = 137.0;
	assert(book.Save(rifles));
	assert(book.Items().size() == 1);
	const WeaponRecoilPreset* saved = book.Find("heavy rifle recoil");
	assert(saved);
	assert(saved->name == "Heavy Rifle Recoil");
	assert(saved->scope == RecoilScope::Type);
	assert(saved->key == "rifle");
	assert(saved->style == RecoilStyle::Mw2022);
	assert(saved->tuning.amount == 137.0);

	// Saving a case-variant updates the named slot instead of creating an ambiguous duplicate.
	rifles.name = "HEAVY RIFLE RECOIL";
	rifles.scope = RecoilScope::Slot;
	rifles.key = "PRIMARY";
	rifles.tuning.amount = 64.0;
	assert(book.Save(rifles));
	assert(book.Items().size() == 1);
	saved = book.Find("Heavy Rifle Recoil");
	assert(saved && saved->scope == RecoilScope::Slot && saved->key == "primary");
	assert(saved->tuning.amount == 64.0);

	assert(!book.Save(WeaponRecoilPreset{ "bad;command" }));
	assert(!book.Save(WeaponRecoilPreset{ "" }));
	assert(book.Erase("heavy rifle recoil"));
	assert(book.Items().empty());
	assert(!book.Erase("missing"));

	std::cout << "weapon recoil preset tests passed\n";
	return 0;
}
