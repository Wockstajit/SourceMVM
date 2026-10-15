#include "ExrWriter.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <vector>

namespace Filmmaker {

namespace {

// Buffered appender so the writer makes a handful of WriteFile calls, not one per row.
struct FileBuf {
	HANDLE h = INVALID_HANDLE_VALUE;
	std::vector<uint8_t> buf;
	bool ok = true;

	explicit FileBuf(const wchar_t* path) {
		h = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		ok = (h != INVALID_HANDLE_VALUE);
		buf.reserve(1 << 20);
	}
	~FileBuf() {
		Flush();
		if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
	}
	void Flush() {
		if (!ok || buf.empty()) { buf.clear(); return; }
		DWORD written = 0;
		if (!::WriteFile(h, buf.data(), (DWORD)buf.size(), &written, nullptr)
			|| written != buf.size())
			ok = false;
		buf.clear();
	}
	void Bytes(const void* p, size_t n) {
		if (!ok) return;
		const uint8_t* b = (const uint8_t*)p;
		buf.insert(buf.end(), b, b + n);
		if (buf.size() >= (1 << 20)) Flush();
	}
	void U8(uint8_t v) { Bytes(&v, 1); }
	void I32(int32_t v) { Bytes(&v, 4); }
	void U64(uint64_t v) { Bytes(&v, 8); }
	void F32(float v) { Bytes(&v, 4); }
	void Str(const char* s) { Bytes(s, strlen(s) + 1); } // includes NUL
};

// One EXR attribute: name, type, int32 payload size, payload (written by caller).
void AttrHead(FileBuf& f, const char* name, const char* type, int32_t size) {
	f.Str(name);
	f.Str(type);
	f.I32(size);
}

} // namespace

bool ExrWriteHalfRgba(const wchar_t* path, const uint16_t* rgbaHalf, int width, int height,
	int rowPitchBytes) {
	if (!path || !rgbaHalf || width <= 0 || height <= 0 || rowPitchBytes < width * 8)
		return false;

	FileBuf f(path);
	if (!f.ok) return false;

	// -- magic + version ------------------------------------------------------------------------
	f.I32(0x01312F76); // 'v'/'2f31' magic, little-endian
	f.I32(2);          // version 2, no flags (single-part scanline)

	// -- header ---------------------------------------------------------------------------------
	// channels: chlist entries sorted alphabetically (B, G, R), HALF, sampling 1:1.
	{
		// Each entry: name\0 + int32 pixelType + uchar pLinear + 3 reserved + int32 x/ySampling.
		const char* names[3] = { "B", "G", "R" };
		const int32_t entryBytes = 2 + 4 + 4 + 4 + 4; // "X\0" + type + linear/reserved + samplings
		AttrHead(f, "channels", "chlist", 3 * entryBytes + 1);
		for (const char* n : names) {
			f.Str(n);
			f.I32(1);  // HALF
			f.I32(0);  // pLinear + 3 reserved bytes
			f.I32(1);  // xSampling
			f.I32(1);  // ySampling
		}
		f.U8(0); // chlist terminator
	}
	AttrHead(f, "compression", "compression", 1);
	f.U8(0); // NO_COMPRESSION
	AttrHead(f, "dataWindow", "box2i", 16);
	f.I32(0); f.I32(0); f.I32(width - 1); f.I32(height - 1);
	AttrHead(f, "displayWindow", "box2i", 16);
	f.I32(0); f.I32(0); f.I32(width - 1); f.I32(height - 1);
	AttrHead(f, "lineOrder", "lineOrder", 1);
	f.U8(0); // INCREASING_Y
	AttrHead(f, "pixelAspectRatio", "float", 4);
	f.F32(1.0f);
	AttrHead(f, "screenWindowCenter", "v2f", 8);
	f.F32(0.0f); f.F32(0.0f);
	AttrHead(f, "screenWindowWidth", "float", 4);
	f.F32(1.0f);
	f.U8(0); // end of header

	// -- scanline offset table ------------------------------------------------------------------
	// Every uncompressed block is the same size, so offsets are arithmetic. The table position
	// itself depends only on what was written above -- flush and ask the file.
	f.Flush();
	LARGE_INTEGER pos{}, zero{};
	if (!::SetFilePointerEx(f.h, zero, &pos, FILE_CURRENT)) return false;
	const uint64_t tableBytes = (uint64_t)height * 8;
	const uint64_t rowBlockBytes = 8ull + (uint64_t)width * 2 * 3; // y + size + 3 half channels
	uint64_t firstBlock = (uint64_t)pos.QuadPart + tableBytes;
	for (int y = 0; y < height; ++y)
		f.U64(firstBlock + (uint64_t)y * rowBlockBytes);

	// -- scanline blocks: y, byte count, then planar B, G, R half rows ---------------------------
	std::vector<uint16_t> plane((size_t)width * 3);
	for (int y = 0; y < height; ++y) {
		const uint16_t* row = (const uint16_t*)((const uint8_t*)rgbaHalf + (size_t)y * rowPitchBytes);
		uint16_t* b = plane.data();
		uint16_t* g = b + width;
		uint16_t* r = g + width;
		for (int x = 0; x < width; ++x) {
			r[x] = row[x * 4 + 0];
			g[x] = row[x * 4 + 1];
			b[x] = row[x * 4 + 2];
		}
		f.I32(y);
		f.I32((int32_t)(width * 2 * 3));
		f.Bytes(plane.data(), plane.size() * 2);
	}

	f.Flush();
	return f.ok;
}

} // namespace Filmmaker
