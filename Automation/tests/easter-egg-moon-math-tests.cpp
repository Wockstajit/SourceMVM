#include "../../AfxHookSource2/Filmmaker/EasterEggs/EasterEggMoonMath.h"
#include <cstring>
#include <iostream>
#include <limits>

using namespace Filmmaker::EasterEggMoonMath;
namespace {
int failed = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAILED line " << __LINE__ << ": " #x "\n"; ++failed; } } while (0)
void Reject(const std::string& text) {
	float value = 0.7123456f;
	CHECK(!ReadRecovery(text, value));
	CHECK(value == 0.7123456f);
}
struct CommaDecimal : std::numpunct<char> {
	char do_decimal_point() const override { return ','; }
};
}
int main() {
	for (const char* text : { "", " ", "{}", "[]", "[1]", "null", "true", "\"1\"", "nan", "inf",
		"-inf", "1e999", "-2.00000001", "4.00000001", "0 trailing", "0 1", "1,5", "+1" }) Reject(text);
	Reject(std::string("1\0junk", 6));
	Reject("1" + std::string(64, ' '));
	float legacy = 0;
	CHECK(ReadRecovery(" \r\n0.712346\t", legacy) && legacy == 0.712346f);
	CHECK(ReadRecovery("1" + std::string(63, ' '), legacy) && legacy == 1.0f);
	const std::locale original;
	std::locale::global(std::locale(original, new CommaDecimal));
	for (float input : { -2.0f, 4.0f, -0.0f, 0.0f, 1.0f, -0.12f, 0.7123456f,
		std::nextafter(4.0f, 0.0f), std::nextafter(-2.0f, 0.0f), std::numeric_limits<float>::denorm_min() }) {
		std::string text;
		float output = 100;
		CHECK(WriteRecovery(input, text));
		CHECK(text.find(',') == std::string::npos);
		CHECK(ReadRecovery(text, output));
		CHECK(std::memcmp(&input, &output, sizeof(float)) == 0);
	}
	std::locale::global(original);
	for (float input : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
		-std::numeric_limits<float>::infinity(), -2.1f, 4.1f }) {
		std::string output = "untouched";
		CHECK(!WriteRecovery(input, output));
		CHECK(output == "untouched");
	}
	std::cout << "moon recovery: " << failed << " failure(s)\n";
	return failed ? 1 : 0;
}
