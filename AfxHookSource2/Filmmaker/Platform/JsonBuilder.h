#pragma once

// Tiny JSON string builder used to hand the demo list to the Panorama UI as a
// single attribute string. Intentionally minimal — only objects, arrays,
// strings and integers, which is all the demo payload needs.

#include <string>
#include <cstdint>

namespace Filmmaker {

class JsonBuilder {
public:
	void BeginObject();
	void EndObject();
	void BeginArray();
	void EndArray();

	// Key for the next value inside an object.
	void Key(const char* name);

	void ValueString(const std::string& s);
	void ValueInt(int64_t v);
	void ValueDouble(double v);
	void ValueBool(bool v);

	// Convenience: key + value in one call.
	void StringField(const char* name, const std::string& s);
	void IntField(const char* name, int64_t v);
	void DoubleField(const char* name, double v);
	void BoolField(const char* name, bool v);

	const std::string& Str() const { return m_out; }

	static std::string Escape(const std::string& s);

private:
	void Separator();

	std::string m_out;
	bool m_needComma = false;
};

} // namespace Filmmaker
