#include "JsonBuilder.h"

#include <cstdio>

namespace Filmmaker {

std::string JsonBuilder::Escape(const std::string& s) {
	std::string out;
	out.reserve(s.size() + 8);
	for (unsigned char c : s) {
		switch (c) {
		case '\"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", (int)c);
				out += buf;
			} else {
				out += (char)c;
			}
		}
	}
	return out;
}

void JsonBuilder::Separator() {
	if (m_needComma)
		m_out += ',';
	m_needComma = false;
}

void JsonBuilder::BeginObject() { Separator(); m_out += '{'; m_needComma = false; }
void JsonBuilder::EndObject()   { m_out += '}'; m_needComma = true; }
void JsonBuilder::BeginArray()  { Separator(); m_out += '['; m_needComma = false; }
void JsonBuilder::EndArray()    { m_out += ']'; m_needComma = true; }

void JsonBuilder::Key(const char* name) {
	Separator();
	m_out += '\"';
	m_out += Escape(name);
	m_out += "\":";
	m_needComma = false;
}

void JsonBuilder::ValueString(const std::string& s) {
	Separator();
	m_out += '\"';
	m_out += Escape(s);
	m_out += '\"';
	m_needComma = true;
}

void JsonBuilder::ValueInt(int64_t v) {
	Separator();
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
	m_out += buf;
	m_needComma = true;
}

void JsonBuilder::ValueDouble(double v) {
	Separator();
	char buf[64];
	// %.6g keeps coordinates/angles compact but lossless enough for camera paths,
	// and never emits "nan"/"inf" as bare tokens that would break the JSON.
	if (!(v == v) || v > 1e308 || v < -1e308)
		m_out += '0';
	else {
		std::snprintf(buf, sizeof(buf), "%.6g", v);
		m_out += buf;
	}
	m_needComma = true;
}

void JsonBuilder::ValueBool(bool v) {
	Separator();
	m_out += v ? "true" : "false";
	m_needComma = true;
}

void JsonBuilder::StringField(const char* name, const std::string& s) { Key(name); ValueString(s); }
void JsonBuilder::IntField(const char* name, int64_t v) { Key(name); ValueInt(v); }
void JsonBuilder::DoubleField(const char* name, double v) { Key(name); ValueDouble(v); }
void JsonBuilder::BoolField(const char* name, bool v) { Key(name); ValueBool(v); }

} // namespace Filmmaker
