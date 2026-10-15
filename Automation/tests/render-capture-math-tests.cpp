#include "../../AfxHookSource2/Filmmaker/Render/RenderCaptureMath.h"

#include <iostream>

using Filmmaker::RenderCaptureFps;

namespace {

int g_failed = 0;

void Expect(int actual, int expected, const char* label) {
	if (actual == expected) return;
	std::cerr << "FAILED " << label << ": got " << actual << ", expected " << expected << "\n";
	++g_failed;
}

} // namespace

int main() {
	Expect(RenderCaptureFps(600), 600, "600 fps passthrough");
	Expect(RenderCaptureFps(60), 60, "60 fps passthrough");
	Expect(RenderCaptureFps(1), 1, "floor at 1");
	Expect(RenderCaptureFps(0), 1, "zero clamps to 1");
	Expect(RenderCaptureFps(-5), 1, "negative clamps to 1");
	Expect(RenderCaptureFps(1000000), 1000000, "ceiling allowed");
	Expect(RenderCaptureFps(1000001), 1000000, "above ceiling clamps");

	if (g_failed) {
		std::cerr << g_failed << " check(s) failed\n";
		return 1;
	}
	std::cout << "RenderCaptureMath tests passed.\n";
	return 0;
}
