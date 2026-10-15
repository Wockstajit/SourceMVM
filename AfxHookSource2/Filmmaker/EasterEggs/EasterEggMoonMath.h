#pragma once

#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>

namespace Filmmaker::EasterEggMoonMath {

// Same supported gravity range as RagdollPhysics. Preserve the legacy plain-number format.
inline bool ValidGravity(double value) {
	return std::isfinite(value) && value >= -2.0 && value <= 4.0;
}
inline bool ReadRecovery(const std::string& text, float& out) {
	if (text.empty() || text.size() > 64) return false;
	const auto first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return false;
	const char* end = text.data() + text.find_last_not_of(" \t\r\n") + 1;
	double value;
	const auto parsed = std::from_chars(text.data() + first, end, value);
	if (parsed.ec != std::errc{} || parsed.ptr != end || !ValidGravity(value)) return false;
	out = static_cast<float>(value);
	return true;
}
inline bool WriteRecovery(float value, std::string& out) {
	if (!ValidGravity(value)) return false;
	std::ostringstream text;
	text.imbue(std::locale::classic());
	text << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
	out = text.str();
	return true;
}

} // namespace Filmmaker::EasterEggMoonMath
