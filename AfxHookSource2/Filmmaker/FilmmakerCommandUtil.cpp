#include "FilmmakerCommandUtil.h"

#include "../../Shared/AfxConsole.h"

#include <cstddef>

namespace Filmmaker {

namespace {

std::string Base64Encode(const std::string& input) {
	static const char table[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string output;
	output.reserve(((input.size() + 2) / 3) * 4);
	unsigned int value = 0;
	int bits = -6;
	for (unsigned char c : input) {
		value = (value << 8) | c;
		bits += 8;
		while (bits >= 0) {
			output.push_back(table[(value >> bits) & 0x3f]);
			bits -= 6;
		}
	}
	if (bits > -6)
		output.push_back(table[((value << 8) >> (bits + 8)) & 0x3f]);
	while (output.size() % 4)
		output.push_back('=');
	return output;
}

} // namespace

void PrintEncodedState(const char* marker, const std::string& json) {
	const std::string encoded = Base64Encode(json);
	constexpr size_t chunkSize = 120;
	const size_t chunks = (encoded.size() + chunkSize - 1) / chunkSize;
	for (size_t i = 0; i < chunks; ++i) {
		const std::string chunk = encoded.substr(i * chunkSize, chunkSize);
		advancedfx::Message("%s %zu/%zu %s\n", marker, i + 1, chunks, chunk.c_str());
	}
}

} // namespace Filmmaker
