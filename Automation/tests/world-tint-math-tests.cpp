#include "../../AfxHookSource2/Filmmaker/World/WorldTintMath.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void ExpectNear(const char* name, float actual, float expected, float epsilon = 1e-5f) {
	if (std::fabs(actual - expected) <= epsilon) return;
	std::cerr << name << ": expected " << expected << ", got " << actual << "\n";
	std::exit(1);
}

} // namespace

int main() {
	using namespace Filmmaker;

	ExpectNear("black", WorldSrgbToLinear(0.0f), 0.0f);
	ExpectNear("white", WorldSrgbToLinear(1.0f), 1.0f);
	ExpectNear("mid sRGB", WorldSrgbToLinear(0.5f), 0.214041f, 1e-4f);
	ExpectNear("round trip", WorldLinearToSrgb(WorldSrgbToLinear(0.5f)), 0.5f, 1e-5f);
	ExpectNear("white tint preserves authored", ComposeWorldSrgbTint(0.42f, 1.0f), 0.42f, 1e-5f);
	ExpectNear("black tint removes authored", ComposeWorldSrgbTint(0.42f, 0.0f), 0.0f, 1e-5f);

	// The old bug wrote 20/255 as a linear value. The shader then decoded it again. The composed
	// value must stay in the material's sRGB domain instead.
	const float composed = ComposeWorldSrgbTint(1.0f, 20.0f / 255.0f);
	ExpectNear("sRGB material write", composed, 20.0f / 255.0f, 1e-5f);
	return 0;
}
