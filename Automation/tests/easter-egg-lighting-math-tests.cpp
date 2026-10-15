#include "../../AfxHookSource2/Filmmaker/EasterEggs/EasterEggLightingMath.h"

#include <iostream>
#include <limits>

using namespace Filmmaker;
namespace {
int failed = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAILED line " << __LINE__ << ": " #x "\n"; ++failed; } } while (0)
const char* valid = R"({"r":37,"g":91,"b":143,"amount":0.37,"uniform":true,"yaw":27.25})";
bool Same(const EasterEggLightingState& a, const EasterEggLightingState& b) {
	return a.r == b.r && a.g == b.g && a.b == b.b && a.amount == b.amount
		&& a.uniform == b.uniform && a.yaw == b.yaw;
}
void Reject(const JsonValue& root) {
	EasterEggLightingState original{ 12, 34, 56, 0.8f, true, 123.5f };
	auto out = original;
	CHECK(!EasterEggLightingMath::ReadRecovery(root, out));
	CHECK(Same(out, original));
}
}
int main() {
	JsonValue root;
	CHECK(JsonParse(valid, root));
	EasterEggLightingState state;
	CHECK(EasterEggLightingMath::ReadRecovery(root, state));
	CHECK(Same(state, { 37, 91, 143, 0.37f, true, 27.25f }));
	for (const char* text : { "{}", "[]", "null", "true", "5",
		R"({"r":0,"r":255,"g":0,"b":0,"amount":1,"uniform":true,"yaw":0})" }) {
		JsonValue bad; CHECK(JsonParse(text, bad)); Reject(bad);
	}
	for (size_t i = 0; i < root.obj.size(); ++i) {
		auto bad = root; bad.obj.erase(bad.obj.begin() + i); Reject(bad);
		bad = root; bad.obj[i].second.type = JsonValue::Type::String; Reject(bad);
		bad = root; bad.obj[i].second.type = JsonValue::Type::Null; Reject(bad);
		bad = root; bad.obj[i].first = "unknown"; Reject(bad);
		bad = root; bad.obj.push_back(bad.obj[i]); Reject(bad);
		if (i == 4) { bad = root; bad.obj[i].second.type = JsonValue::Type::Number; Reject(bad); continue; }
		for (double value : { -1.0, 1e300, std::numeric_limits<double>::infinity(),
			std::numeric_limits<double>::quiet_NaN() }) {
			bad = root; bad.obj[i].second.number = value; Reject(bad);
		}
		if (i < 3) { bad = root; bad.obj[i].second.number = 10.5; Reject(bad); }
	}
	for (const char* text : {
		R"({"r":0,"g":255,"b":0,"amount":0,"uniform":false,"yaw":0})",
		R"({"r":255,"g":0,"b":255,"amount":1,"uniform":true,"yaw":360})" }) {
		JsonValue boundary; CHECK(JsonParse(text, boundary));
		CHECK(EasterEggLightingMath::ReadRecovery(boundary, state));
	}
	std::cout << "lighting recovery validation: " << failed << " failure(s)\n";
	return failed ? 1 : 0;
}
