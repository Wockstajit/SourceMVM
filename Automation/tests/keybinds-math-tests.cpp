#include "../../AfxHookSource2/Filmmaker/Input/KeybindsMath.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

using namespace Filmmaker::KeybindsMath;

int main() {
	int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

	int bound[kActionCount];
	const auto defaults = [&]() { for (size_t i = 0; i < kActionCount; ++i) bound[i] = kActions[i].defaultKey; };
	// What the handler is shown for one press.
	const auto is = [&](int pressed, std::initializer_list<int> want) {
		int out[kMaxDispatch];
		const int n = Dispatch(bound, pressed, out);
		return std::vector<int>(out, out + n) == std::vector<int>(want);
	};

	const int place = FindAction("marker.place"), play = FindAction("path.play"), xray = FindAction("xray");
	const int rollRight = FindAction("roll.right"), rollLeft = FindAction("roll.left");
	CHECK(place >= 0 && play >= 0 && xray >= 0 && rollRight >= 0 && rollLeft >= 0);
	CHECK(FindAction("no.such.action") < 0 && FindAction(nullptr) < 0);

	// The table itself: unique ids and canonical keys, no default on a reserved key (a default the
	// user could never bind back to), and no key with more default actions than a key may carry.
	{
		std::set<std::string> ids;
		std::set<int> canonical;
		defaults();
		for (size_t i = 0; i < kActionCount; ++i) {
			CHECK(ids.insert(kActions[i].id).second);
			CHECK(canonical.insert(kActions[i].canonicalKey).second);
			CHECK(!Reserved(kActions[i].defaultKey));
			CHECK(Conflict(bound, kActions[i].defaultKey, static_cast<int>(i)) < 0);
		}
	}
	// Roll is listed before X-ray: on a shared key the roll is offered to the handler first.
	CHECK(rollRight < xray);

	// With every action on its default, an ordinary key is seen as itself.
	defaults();
	for (int key : std::initializer_list<int>{ 'K', 'J', 'Q', 0x20, 0x77, 0x41, 0x1B }) CHECK(is(key, { key }));

	// X carries roll right and X-ray out of the box: roll first, then X-ray, and X only once.
	CHECK(is('X', { kRollRightKey, 'X' }));
	// Z carries roll left, and is still Z afterwards (Ctrl+Z).
	CHECK(is('Z', { kRollLeftKey, 'Z' }));

	// Place moved from K to P: P arrives as K, and K (now free) arrives as the key no handler tests.
	defaults();
	bound[place] = 'P';
	CHECK(is('P', { 'K' }));
	CHECK(is('K', { kUnboundKey }));
	CHECK(is('J', { 'J' }));

	// Two actions swapped: each key reaches the other action, and neither is lost.
	defaults();
	bound[place] = 'J';
	bound[play] = 'K';
	CHECK(is('J', { 'K' }));
	CHECK(is('K', { 'J' }));

	// Roll right moved to Q: Q rolls, X is X-ray alone.
	defaults();
	bound[rollRight] = 'Q';
	CHECK(is('Q', { kRollRightKey }));
	CHECK(is('X', { 'X' }));
	// X-ray moved away instead: X rolls and is NOT also shown as X.
	defaults();
	bound[xray] = 'B';
	CHECK(is('X', { kRollRightKey }));
	CHECK(is('B', { 'X' }));
	// Both moved away: X does nothing of ours.
	bound[rollRight] = 'Q';
	CHECK(is('X', { kUnboundKey }));
	// Roll left moved off Z: Z is plain Z again.
	defaults();
	bound[rollLeft] = 'Q';
	CHECK(is('Z', { 'Z' }));

	// The shared free-camera input rolls on X and Z only: it is shown X / Z for whatever roll is on.
	defaults();
	CHECK(FreeCamKey(bound, 'X', rollRight, rollLeft) == 'X');
	CHECK(FreeCamKey(bound, 'Z', rollRight, rollLeft) == 'Z');
	CHECK(FreeCamKey(bound, 'W', rollRight, rollLeft) == 'W');
	bound[rollRight] = 'Q';
	CHECK(FreeCamKey(bound, 'Q', rollRight, rollLeft) == 'X');
	CHECK(FreeCamKey(bound, 'X', rollRight, rollLeft) == kUnboundKey);   // X is X-ray only now
	CHECK(FreeCamKey(bound, 'Z', rollRight, rollLeft) == 'Z');

	// A key may carry two actions and no more.
	defaults();
	CHECK(Conflict(bound, 'J', place) < 0);            // J has one action: a second may join it
	CHECK(Conflict(bound, 'X', place) >= 0);           // X already has two
	CHECK(Conflict(bound, 'X', xray) < 0);             // ... but X-ray is one of them
	CHECK(Conflict(bound, 'P', place) < 0);
	bound[place] = 'J';
	CHECK(is('J', { 'K', 'J' }));                       // place first (table order), then play

	// Fixed keys: cancel, modifiers, text editing, free-cam movement, the Ctrl-chord letters, mouse.
	for (int key : std::initializer_list<int>{ 0x1B, 0x10, 0x11, 0x12, 0x08, 0x2E, 0x0D, 0x09, 'W', 'A', 'S', 'D', 'C', 'V', 0x01, 0x02, 0, kUnboundKey })
		CHECK(Reserved(key));
	for (int key : std::initializer_list<int>{ 'P', 'Q', 'Z', '1', 0x70, 0x7B, 0x60, 0x25, 0x20, 0x2D })
		CHECK(!Reserved(key));

	// Cap text.
	char name[24];
	KeyName('K', name, sizeof(name)); CHECK(0 == std::strcmp(name, "K"));
	KeyName('7', name, sizeof(name)); CHECK(0 == std::strcmp(name, "7"));
	KeyName(0x77, name, sizeof(name)); CHECK(0 == std::strcmp(name, "F8"));
	KeyName(0x20, name, sizeof(name)); CHECK(0 == std::strcmp(name, "SPACE"));
	KeyName(0x65, name, sizeof(name)); CHECK(0 == std::strcmp(name, "NUM 5"));
	KeyName(0x2D, name, sizeof(name)); CHECK(0 == std::strcmp(name, "INS"));
	KeyName(0xE7, name, sizeof(name)); CHECK(0 == std::strcmp(name, "KEY 231"));
	// Every name goes into a JSON string unescaped, so none may hold a bare quote or backslash.
	for (int key = 1; key < 0xFF; ++key) {
		KeyName(key, name, sizeof(name));
		CHECK(name[0] != 0 && !std::strchr(name, '"'));
		for (const char* p = name; *p; ++p) if (*p == '\\') { CHECK(p[1] == '\\'); ++p; }
	}

	if (failures) std::printf("%d failure(s)\n", failures);
	else std::printf("keybinds math: all checks passed\n");
	return failures ? 1 : 0;
}
