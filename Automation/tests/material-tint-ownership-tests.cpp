#include "../../AfxHookSource2/Filmmaker/World/MaterialTintOwnership.h"

#include <cstdlib>
#include <iostream>

namespace {

void Expect(const char* name, bool actual, bool expected) {
	if (actual == expected) return;
	std::cerr << name << ": expected " << expected << ", got " << actual << "\n";
	std::exit(1);
}

} // namespace

int main() {
	Filmmaker::MaterialTintOwnership ownership;
	Expect("empty", ownership.Owns("materials/models/player/glove.vmat"), false);

	ownership.BeginUpdate();
	ownership.Add("materials/models/player/cloth.vmat");
	ownership.Publish();
	Expect("supported material owned", ownership.Owns("materials/models/player/cloth.vmat"), true);
	Expect("unsupported sibling falls back", ownership.Owns("materials/models/player/glove.vmat"), false);

	ownership.BeginUpdate();
	ownership.Add("materials/models/player/glove.vmat");
	ownership.Publish();
	Expect("new publication visible", ownership.Owns("materials/models/player/glove.vmat"), true);
	Expect("stale publication removed", ownership.Owns("materials/models/player/cloth.vmat"), false);

	ownership.BeginUpdate();
	ownership.Publish();
	Expect("reset publishes empty", ownership.Owns("materials/models/player/glove.vmat"), false);
	return 0;
}
