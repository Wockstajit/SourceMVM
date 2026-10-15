#pragma once

#include "EasterEggWorld.h"
#include "../Platform/JsonParser.h"

#include <cmath>

namespace Filmmaker::EasterEggLightingMath {

// Recovery must be complete and unambiguous. Never manufacture missing user preferences.
// Keep the output untouched on failure, including before potentially narrowing numbers.
inline bool ReadRecovery(const JsonValue& root, EasterEggLightingState& out) {
	if (root.type != JsonValue::Type::Object || root.obj.size() != 6) return false;
	const JsonValue* r = root.Find("r"), *g = root.Find("g"), *b = root.Find("b");
	const JsonValue* amount = root.Find("amount"), *uniform = root.Find("uniform"), *yaw = root.Find("yaw");
	auto number = [](const JsonValue* v, double low, double high) {
		return v && v->type == JsonValue::Type::Number && std::isfinite(v->number)
			&& v->number >= low && v->number <= high;
	};
	auto channel = [&](const JsonValue* v) {
		return number(v, 0, 255) && std::floor(v->number) == v->number;
	};
	if (!channel(r) || !channel(g) || !channel(b) || !number(amount, 0, 1)
		|| !uniform || uniform->type != JsonValue::Type::Bool || !number(yaw, 0, 360)) return false;
	EasterEggLightingState state;
	state.r = (int)r->number; state.g = (int)g->number; state.b = (int)b->number;
	state.amount = (float)amount->number;
	state.uniform = uniform->boolean;
	state.yaw = (float)yaw->number;
	out = state;
	return true;
}

} // namespace Filmmaker::EasterEggLightingMath
