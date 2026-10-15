#pragma once

// Minimal read-only JSON parser (objects, arrays, strings with full escape +
// \uXXXX/surrogate handling, numbers, bool, null). Just enough to read the
// FilmmakerDemoInfo helper's output; values are returned as UTF-8 std::string.

#include <string>
#include <vector>
#include <utility>

namespace Filmmaker {

struct JsonValue {
	enum class Type { Null, Bool, Number, String, Array, Object };
	Type type = Type::Null;
	bool boolean = false;
	double number = 0.0;
	std::string str;
	std::vector<JsonValue> arr;
	std::vector<std::pair<std::string, JsonValue>> obj;

	// Object member lookup (nullptr if not an object or key absent).
	const JsonValue* Find(const char* key) const;

	bool AsBool(bool def = false) const { return type == Type::Bool ? boolean : def; }
	double AsNumber(double def = 0.0) const { return type == Type::Number ? number : def; }
	int AsInt(int def = 0) const { return type == Type::Number ? (int)number : def; }
	std::string AsString(const char* def = "") const { return type == Type::String ? str : std::string(def); }
};

// Parses the whole text, replacing out only on success. Numbers must fit a finite double.
// Returns false on malformed input or out-of-range numeric conversion; ignores the OS locale.
bool JsonParse(const std::string& text, JsonValue& out);

} // namespace Filmmaker
