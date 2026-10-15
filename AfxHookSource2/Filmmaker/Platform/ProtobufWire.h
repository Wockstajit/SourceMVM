#pragma once

// Minimal, self-contained protobuf wire-format reader.
// Only what the filmmaker demo metadata needs (varint, fixed32/64, length-delimited).
// No protobuf library dependency.

#include <cstdint>
#include <cstddef>

namespace Filmmaker {

// Protobuf wire types.
enum class PbWire : uint32_t {
	Varint = 0,
	Fixed64 = 1,
	LengthDelimited = 2,
	StartGroup = 3,
	EndGroup = 4,
	Fixed32 = 5,
};

// Reads protobuf fields out of a fixed byte buffer. Bounds-checked; on any
// malformed read the reader latches an error and stops yielding fields.
class ProtobufReader {
public:
	ProtobufReader(const uint8_t* data, size_t size)
		: m_cur(data), m_end(data ? data + size : nullptr) {}

	bool Eof() const { return m_error || m_cur == nullptr || m_cur >= m_end; }
	bool HasError() const { return m_error; }

	// Reads the next field's tag. Returns false at end-of-buffer or on error.
	bool ReadTag(uint32_t& fieldNumber, PbWire& wireType);

	uint64_t ReadVarint();
	uint32_t ReadFixed32();
	uint64_t ReadFixed64();

	// Returns a pointer/length view over a length-delimited field's bytes.
	bool ReadBytes(const uint8_t*& outPtr, size_t& outLen);

	// Returns a sub-reader over a length-delimited field (e.g. a nested message).
	ProtobufReader ReadSubMessage();

	// Skips a field of the given wire type (value already consumed via ReadTag).
	void SkipField(PbWire wireType);

	const uint8_t* Cursor() const { return m_cur; }
	size_t Remaining() const { return m_cur && m_end && m_cur <= m_end ? (size_t)(m_end - m_cur) : 0; }

private:
	const uint8_t* m_cur;
	const uint8_t* m_end;
	bool m_error = false;
};

} // namespace Filmmaker
