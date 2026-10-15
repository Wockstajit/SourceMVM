#pragma once

namespace Filmmaker {

struct LightTintRgb {
	float r;
	float g;
	float b;
};

enum class LightTintMode {
	Uniform,
	Filtered
};

// Recolours linear HDR radiance without adding energy. Uniform preserves the source light's peak
// radiance while replacing its hue; Filtered applies the chosen colour channel-by-channel. The
// amount is a 0..1 blend from the engine-authored light to the target.
inline LightTintRgb ApplyLightTint(
	const LightTintRgb& source,
	const LightTintRgb& tint,
	float amount,
	LightTintMode mode) {
	if (amount < 0.0f) amount = 0.0f;
	if (amount > 1.0f) amount = 1.0f;

	LightTintRgb target{};
	if (mode == LightTintMode::Uniform) {
		float sourcePeak = source.r > source.g ? source.r : source.g;
		if (source.b > sourcePeak) sourcePeak = source.b;
		if (sourcePeak < 0.0f) sourcePeak = 0.0f;

		float tintPeak = tint.r > tint.g ? tint.r : tint.g;
		if (tint.b > tintPeak) tintPeak = tint.b;
		if (tintPeak > 0.0f) {
			const float scale = sourcePeak / tintPeak;
			target = { tint.r * scale, tint.g * scale, tint.b * scale };
		}
	} else {
		target = { source.r * tint.r, source.g * tint.g, source.b * tint.b };
	}

	return {
		source.r + (target.r - source.r) * amount,
		source.g + (target.g - source.g) * amount,
		source.b + (target.b - source.b) * amount
	};
}

} // namespace Filmmaker
