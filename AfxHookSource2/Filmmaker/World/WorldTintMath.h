#pragma once

#include <cmath>

namespace Filmmaker {

// Valve's g_vColorTint uniforms are declared SrgbRead(true). Keep the transfer-function
// boundary in one small, testable header so material writes cannot accidentally feed linear HDR
// values back into an sRGB material parameter.
inline float WorldSrgbToLinear(float value) {
	const float srgb = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
	return srgb <= 0.04045f
		? srgb / 12.92f
		: std::pow((srgb + 0.055f) / 1.055f, 2.4f);
}

inline float WorldSrgbByteToLinear(unsigned char value) {
	return WorldSrgbToLinear((float)value / 255.0f);
}

inline float WorldLinearToSrgb(float value) {
	const float linear = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
	return linear <= 0.0031308f
		? linear * 12.92f
		: 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

inline float ComposeWorldSrgbTint(float authoredSrgb, float tintSrgb) {
	return WorldLinearToSrgb(WorldSrgbToLinear(authoredSrgb) * WorldSrgbToLinear(tintSrgb));
}

} // namespace Filmmaker
