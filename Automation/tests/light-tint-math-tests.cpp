#include "../../AfxHookSource2/Filmmaker/World/LightTintMath.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void ExpectNear(const char* name, float actual, float expected) {
	if (std::fabs(actual - expected) <= 1e-5f) return;
	std::cerr << name << ": expected " << expected << ", got " << actual << "\n";
	std::exit(1);
}

void ExpectBounded(const char* name, const Filmmaker::LightTintRgb& value, float peak) {
	if (value.r <= peak + 1e-5f && value.g <= peak + 1e-5f && value.b <= peak + 1e-5f)
		return;
	std::cerr << name << ": output (" << value.r << ", " << value.g << ", " << value.b
		<< ") exceeded source peak " << peak << "\n";
	std::exit(1);
}

} // namespace

int main() {
	using Filmmaker::ApplyLightTint;
	using Filmmaker::LightTintMode;
	using Filmmaker::LightTintRgb;

	const LightTintRgb sun{ 175.8f, 99.0f, 58.7f };
	const LightTintRgb blue{ 0.145f, 0.338f, 1.0f };

	const LightTintRgb untouched = ApplyLightTint(sun, blue, 0.0f, LightTintMode::Uniform);
	ExpectNear("zero amount red", untouched.r, sun.r);
	ExpectNear("zero amount green", untouched.g, sun.g);
	ExpectNear("zero amount blue", untouched.b, sun.b);

	const LightTintRgb uniform = ApplyLightTint(sun, blue, 1.0f, LightTintMode::Uniform);
	ExpectBounded("uniform", uniform, sun.r);
	ExpectNear("uniform peak preserved", uniform.b, sun.r);

	const LightTintRgb filtered = ApplyLightTint(sun, blue, 1.0f, LightTintMode::Filtered);
	ExpectNear("filtered red", filtered.r, sun.r * blue.r);
	ExpectNear("filtered green", filtered.g, sun.g * blue.g);
	ExpectNear("filtered blue", filtered.b, sun.b);
	ExpectBounded("filtered", filtered, sun.r);

	const LightTintRgb clamped = ApplyLightTint(sun, blue, 8.0f, LightTintMode::Uniform);
	ExpectNear("amount clamps to one red", clamped.r, uniform.r);
	ExpectNear("amount clamps to one green", clamped.g, uniform.g);
	ExpectNear("amount clamps to one blue", clamped.b, uniform.b);
	return 0;
}
