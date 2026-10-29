#pragma once

// Bit-level reading and writing in the order Valve's bf_read / bf_write use (least significant bit
// first), for the Source 1 demo reader and the old-demo converter. Engine-free, header-only.
//
// Reads past the end set Overrun() and return zeros, so callers check once per message rather than
// after every field.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace SourceFilmmaker {
namespace DemoConvert {

class BitReader {
public:
	BitReader() = default;
	BitReader(const unsigned char* data, size_t bytes) : m_data(data), m_end(bytes * 8) {}
	// A reader over bits [startBit, endBit) of data.
	BitReader(const unsigned char* data, size_t startBit, size_t endBit) : m_data(data), m_pos(startBit), m_end(endBit) {}

	size_t Position() const { return m_pos; }
	size_t End() const { return m_end; }
	size_t Left() const { return m_pos < m_end ? m_end - m_pos : 0; }
	bool Overrun() const { return m_overrun; }
	const unsigned char* Data() const { return m_data; }

	uint32_t Bits(int count) {
		if (count <= 0) return 0;
		if (count > 32 || m_pos + (size_t)count > m_end) return Fail();
		uint32_t value = 0;
		size_t pos = m_pos;
		int done = 0;
		while (done < count) {
			const int bit = (int)(pos & 7);
			const int take = 8 - bit < count - done ? 8 - bit : count - done;
			const uint32_t chunk = (m_data[pos >> 3] >> bit) & ((1u << take) - 1);
			value |= chunk << done;
			done += take;
			pos += take;
		}
		m_pos = pos;
		return value;
	}
	int32_t Signed(int count) {
		const uint32_t raw = Bits(count);
		if (count > 0 && count < 32 && (raw & (1u << (count - 1)))) return (int32_t)(raw | (~0u << count));
		return (int32_t)raw;
	}
	bool Bit() { return Bits(1) != 0; }
	float Float() {
		const uint32_t raw = Bits(32);
		float value;
		std::memcpy(&value, &raw, sizeof(value));
		return value;
	}
	std::string String(size_t maxLength = 4096) {
		std::string out;
		while (!m_overrun) {
			const char c = (char)Bits(8);
			if (!c) break;
			if (out.size() >= maxLength) { Fail(); break; }
			out.push_back(c);
		}
		return out;
	}
	// Protobuf-style varint, as Source 2013 writes lengths (bf_read::ReadVarInt32).
	uint32_t VarInt() {
		uint32_t result = 0;
		for (int i = 0; i < 5; ++i) {
			const uint32_t b = Bits(8);
			result |= (b & 0x7F) << (7 * i);
			if (!(b & 0x80)) break;
		}
		return result;
	}
	// Episode One's bf_read::ReadUBitVar (network protocol 7): n zero bits up to a one, then n bits;
	// the value is 2^n - 1 plus those bits.
	uint32_t UBitVarEp1() {
		int n = 0;
		while (!Bit()) {
			if (++n > 31 || m_overrun) return Fail();
		}
		return ((1u << n) - 1) + Bits(n);
	}
	// Source 2013's bf_read::ReadUBitVar (network protocol 24): a 2-bit selector, then 4, 8, 12 or
	// 32 bits.
	uint32_t UBitVar2013() {
		static const int kWidths[4] = { 4, 8, 12, 32 };
		return Bits(kWidths[Bits(2)]);
	}
	void Skip(size_t count) {
		if (m_pos + count > m_end) { Fail(); return; }
		m_pos += count;
	}
	// One component of bf_read::ReadBitCoord: integer and fraction flags, sign, 14 + 5 bits.
	void SkipCoord() {
		const bool integer = Bit(), fraction = Bit();
		if (!integer && !fraction) return;
		Bit();
		if (integer) Bits(14);
		if (fraction) Bits(5);
	}
	// A reader over the next count bits; this reader moves past them.
	BitReader Sub(size_t count) {
		if (m_pos + count > m_end) {
			Fail();
			return BitReader(m_data, m_pos, m_pos);
		}
		BitReader sub(m_data, m_pos, m_pos + count);
		m_pos += count;
		return sub;
	}

private:
	uint32_t Fail() {
		m_overrun = true;
		m_pos = m_end;
		return 0;
	}

	const unsigned char* m_data = nullptr;
	size_t m_pos = 0;
	size_t m_end = 0;
	bool m_overrun = false;
};

class BitWriter {
public:
	size_t Size() const { return m_bits; }
	size_t Bytes() const { return (m_bits + 7) / 8; }
	const std::vector<unsigned char>& Data() const { return m_data; }

	void Bits(uint32_t value, int count) {
		if (count <= 0) return;
		if (count < 32) value &= (1u << count) - 1;
		while (count > 0) {
			const size_t byte = m_bits >> 3;
			const int bit = (int)(m_bits & 7);
			if (byte >= m_data.size()) m_data.push_back(0);
			const int take = 8 - bit < count ? 8 - bit : count;
			m_data[byte] |= (unsigned char)((value & ((1u << take) - 1)) << bit);
			value = take < 32 ? value >> take : 0;
			count -= take;
			m_bits += take;
		}
	}
	void Signed(int32_t value, int count) { Bits((uint32_t)value, count); }
	void Bit(bool value) { Bits(value ? 1 : 0, 1); }
	void Float(float value) {
		uint32_t raw;
		std::memcpy(&raw, &value, sizeof(raw));
		Bits(raw, 32);
	}
	void String(const std::string& text) {
		for (char c : text) Bits((unsigned char)c, 8);
		Bits(0, 8);
	}
	void VarInt(uint32_t value) {
		do {
			uint32_t b = value & 0x7F;
			value >>= 7;
			if (value) b |= 0x80;
			Bits(b, 8);
		} while (value);
	}
	// Source 2013's bf_write::WriteUBitVar: the smallest of 4, 8, 12 or 32 bits that holds value.
	void UBitVar2013(uint32_t value) {
		if (value < (1u << 4)) { Bits(0, 2); Bits(value, 4); }
		else if (value < (1u << 8)) { Bits(1, 2); Bits(value, 8); }
		else if (value < (1u << 12)) { Bits(2, 2); Bits(value, 12); }
		else { Bits(3, 2); Bits(value, 32); }
	}
	// Copies bits [from, to) of reader's data verbatim.
	void Copy(const BitReader& reader, size_t from, size_t to) {
		if (to <= from) return;
		if (!(from & 7) && !(m_bits & 7)) {
			// Both on a byte boundary: whole bytes at once.
			const size_t bytes = (to - from) / 8;
			m_data.resize(m_bits / 8);
			m_data.insert(m_data.end(), reader.Data() + from / 8, reader.Data() + from / 8 + bytes);
			m_bits += bytes * 8;
			from += bytes * 8;
		}
		BitReader r(reader.Data(), from, to);
		while (r.Left() > 0) {
			const int take = r.Left() >= 24 ? 24 : (int)r.Left();
			Bits(r.Bits(take), take);
		}
	}
	void Append(const BitWriter& other) {
		BitReader r(other.m_data.data(), 0, other.m_bits);
		Copy(r, 0, other.m_bits);
	}
	// Pads to a whole byte with zeros.
	void PadToByte() {
		while (m_bits & 7) Bit(false);
	}

private:
	std::vector<unsigned char> m_data;
	size_t m_bits = 0;
};

inline int Log2(unsigned value) {
	int result = 0;
	while (value >>= 1) ++result;
	return result;
}

} // namespace DemoConvert
} // namespace SourceFilmmaker
