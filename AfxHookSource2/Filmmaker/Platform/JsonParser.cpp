#include "JsonParser.h"

#include <charconv>
#include <cmath>

namespace Filmmaker {

const JsonValue* JsonValue::Find(const char* key) const {
	if (type != Type::Object)
		return nullptr;
	for (const auto& kv : obj)
		if (kv.first == key)
			return &kv.second;
	return nullptr;
}

namespace {

struct Parser {
	const char* p;
	const char* end;

	void skipWs() {
		while (p < end) {
			char c = *p;
			if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p;
			else break;
		}
	}

	bool parseValue(JsonValue& out);

	void appendUtf8(std::string& s, unsigned cp) {
		if (cp <= 0x7F) {
			s.push_back((char)cp);
		} else if (cp <= 0x7FF) {
			s.push_back((char)(0xC0 | (cp >> 6)));
			s.push_back((char)(0x80 | (cp & 0x3F)));
		} else if (cp <= 0xFFFF) {
			s.push_back((char)(0xE0 | (cp >> 12)));
			s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
			s.push_back((char)(0x80 | (cp & 0x3F)));
		} else {
			s.push_back((char)(0xF0 | (cp >> 18)));
			s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
			s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
			s.push_back((char)(0x80 | (cp & 0x3F)));
		}
	}

	bool hex4(unsigned& out) {
		if (end - p < 4) return false;
		unsigned v = 0;
		for (int i = 0; i < 4; ++i) {
			char c = *p++;
			v <<= 4;
			if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
			else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
			else return false;
		}
		out = v;
		return true;
	}

	bool parseString(std::string& out) {
		if (p >= end || *p != '"') return false;
		++p;
		while (p < end) {
			char c = *p++;
			if (c == '"') return true;
			if (c == '\\') {
				if (p >= end) return false;
				char e = *p++;
				switch (e) {
					case '"': out.push_back('"'); break;
					case '\\': out.push_back('\\'); break;
					case '/': out.push_back('/'); break;
					case 'b': out.push_back('\b'); break;
					case 'f': out.push_back('\f'); break;
					case 'n': out.push_back('\n'); break;
					case 'r': out.push_back('\r'); break;
					case 't': out.push_back('\t'); break;
					case 'u': {
						unsigned cp;
						if (!hex4(cp)) return false;
						if (cp >= 0xD800 && cp <= 0xDBFF) { // high surrogate
							if (end - p >= 2 && p[0] == '\\' && p[1] == 'u') {
								p += 2;
								unsigned lo;
								if (!hex4(lo)) return false;
								if (lo >= 0xDC00 && lo <= 0xDFFF)
									cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
							}
						}
						appendUtf8(out, cp);
						break;
					}
					default: return false;
				}
			} else {
				out.push_back(c);
			}
		}
		return false;
	}

	bool literal(const char* lit) {
		size_t n = 0;
		while (lit[n]) ++n;
		if ((size_t)(end - p) < n) return false;
		for (size_t i = 0; i < n; ++i) if (p[i] != lit[i]) return false;
		p += n;
		return true;
	}
};

bool Parser::parseValue(JsonValue& out) {
	skipWs();
	if (p >= end) return false;
	char c = *p;
	if (c == '"') {
		out.type = JsonValue::Type::String;
		return parseString(out.str);
	}
	if (c == '{') {
		++p; out.type = JsonValue::Type::Object;
		skipWs();
		if (p < end && *p == '}') { ++p; return true; }
		while (p < end) {
			skipWs();
			std::string key;
			if (!parseString(key)) return false;
			skipWs();
			if (p >= end || *p != ':') return false;
			++p;
			JsonValue v;
			if (!parseValue(v)) return false;
			out.obj.emplace_back(std::move(key), std::move(v));
			skipWs();
			if (p >= end) return false;
			if (*p == ',') { ++p; continue; }
			if (*p == '}') { ++p; return true; }
			return false;
		}
		return false;
	}
	if (c == '[') {
		++p; out.type = JsonValue::Type::Array;
		skipWs();
		if (p < end && *p == ']') { ++p; return true; }
		while (p < end) {
			JsonValue v;
			if (!parseValue(v)) return false;
			out.arr.push_back(std::move(v));
			skipWs();
			if (p >= end) return false;
			if (*p == ',') { ++p; continue; }
			if (*p == ']') { ++p; return true; }
			return false;
		}
		return false;
	}
	if (c == 't') { if (!literal("true")) return false; out.type = JsonValue::Type::Bool; out.boolean = true; return true; }
	if (c == 'f') { if (!literal("false")) return false; out.type = JsonValue::Type::Bool; out.boolean = false; return true; }
	if (c == 'n') { if (!literal("null")) return false; out.type = JsonValue::Type::Null; return true; }
	// JSON number grammar; conversion must consume the entire token and ignore the OS locale.
	const char* start = p;
	if (*p == '-') ++p;
	if (p == end) return false;
	if (*p == '0') ++p;
	else {
		if (*p < '1' || *p > '9') return false;
		do { ++p; } while (p < end && *p >= '0' && *p <= '9');
	}
	if (p < end && *p == '.') {
		++p;
		const char* digits = p;
		while (p < end && *p >= '0' && *p <= '9') ++p;
		if (p == digits) return false;
	}
	if (p < end && (*p == 'e' || *p == 'E')) {
		++p;
		if (p < end && (*p == '+' || *p == '-')) ++p;
		const char* digits = p;
		while (p < end && *p >= '0' && *p <= '9') ++p;
		if (p == digits) return false;
	}
	double number;
	const auto converted = std::from_chars(start, p, number);
	if (converted.ec != std::errc{} || converted.ptr != p || !std::isfinite(number)) return false;
	out.type = JsonValue::Type::Number;
	out.number = number;
	return true;
}

} // namespace

bool JsonParse(const std::string& text, JsonValue& out) {
	Parser parser{ text.data(), text.data() + text.size() };
	JsonValue parsed;
	if (!parser.parseValue(parsed)) return false;
	parser.skipWs();
	if (parser.p != parser.end) return false;
	out = std::move(parsed);
	return true;
}

} // namespace Filmmaker
