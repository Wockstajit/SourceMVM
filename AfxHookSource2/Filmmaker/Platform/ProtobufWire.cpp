#include "ProtobufWire.h"

namespace Filmmaker {

uint64_t ProtobufReader::ReadVarint() {
	uint64_t result = 0;
	int shift = 0;
	while (m_cur < m_end) {
		const uint8_t byte = *m_cur++;
		result |= (uint64_t)(byte & 0x7F) << shift;
		if (0 == (byte & 0x80))
			return result;
		shift += 7;
		if (shift >= 64) // malformed varint
			break;
	}
	m_error = true;
	return 0;
}

uint32_t ProtobufReader::ReadFixed32() {
	if (m_cur + 4 > m_end) { m_error = true; return 0; }
	uint32_t v = (uint32_t)m_cur[0] | ((uint32_t)m_cur[1] << 8)
		| ((uint32_t)m_cur[2] << 16) | ((uint32_t)m_cur[3] << 24);
	m_cur += 4;
	return v;
}

uint64_t ProtobufReader::ReadFixed64() {
	if (m_cur + 8 > m_end) { m_error = true; return 0; }
	uint64_t v = 0;
	for (int i = 0; i < 8; ++i)
		v |= (uint64_t)m_cur[i] << (8 * i);
	m_cur += 8;
	return v;
}

bool ProtobufReader::ReadBytes(const uint8_t*& outPtr, size_t& outLen) {
	const uint64_t len = ReadVarint();
	if (m_error || m_cur + len > m_end) { m_error = true; return false; }
	outPtr = m_cur;
	outLen = (size_t)len;
	m_cur += len;
	return true;
}

ProtobufReader ProtobufReader::ReadSubMessage() {
	const uint8_t* ptr = nullptr;
	size_t len = 0;
	if (!ReadBytes(ptr, len))
		return ProtobufReader(nullptr, 0);
	return ProtobufReader(ptr, len);
}

bool ProtobufReader::ReadTag(uint32_t& fieldNumber, PbWire& wireType) {
	if (Eof())
		return false;
	const uint64_t tag = ReadVarint();
	if (m_error)
		return false;
	fieldNumber = (uint32_t)(tag >> 3);
	wireType = (PbWire)(tag & 0x7);
	return fieldNumber != 0;
}

void ProtobufReader::SkipField(PbWire wireType) {
	switch (wireType) {
	case PbWire::Varint: ReadVarint(); break;
	case PbWire::Fixed64: ReadFixed64(); break;
	case PbWire::Fixed32: ReadFixed32(); break;
	case PbWire::LengthDelimited: {
		const uint8_t* p; size_t l;
		ReadBytes(p, l);
		break;
	}
	default:
		// Groups are deprecated and unused by CS2 demo metadata.
		m_error = true;
		break;
	}
}

} // namespace Filmmaker
