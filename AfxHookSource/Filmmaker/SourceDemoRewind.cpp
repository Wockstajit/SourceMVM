#include "stdafx.h"

#include "SourceDemoRewind.h"
#include "SourceConCommand.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <Shared/AfxConsole.h>
#include <Shared/AfxDetours.h>

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace SourceFilmmaker {

namespace {

// The demo player's slots and fields (SourceDemoRewind.h says where they were measured).
constexpr int kSlotGetDemoFile = 1;     // lea rax, [rcx + 8]: the CDemoFile inside the player
constexpr int kSlotGetPlaybackTick = 3; // host_tickcount - the playback's start tick
constexpr int kSlotPausePlayback = 13;  // PausePlayback(float seconds), -1: until resumed
constexpr int kSlotSkipToTick = 14;     // SkipToTick(int tick, bool relative, bool pause)
constexpr int kSlotReadPacket = 19;     // ReadPacket(): the next packet that is due, or nullptr
constexpr int kDemoFile = 8;
constexpr int kSkipToTick = 0x5bc;      // the tick a skip runs to, or -1
constexpr int kSkipCount = 0x5c8;       // packets skipped in a row; ReadPacket ends the frame's at 100
// SkipToTick marks a skip that waits for the demo's sync tick (after its reload) with these bits.
constexpr unsigned kWaitForSyncTick = 0x88000000u;
// CDemoFile: the demo's demoheader_t, then the CUtlBuffer the file is read through.
constexpr int kFileHeader = 0x104;
constexpr int kFileBuffer = 0x538;
constexpr int kHeaderBytes = 1072; // sizeof(demoheader_t)
constexpr int kHeaderPlaybackTicks = 1060;
constexpr int kHeaderSignonLength = 1068;
const char kHeaderStamp[8] = { 'H', 'L', '2', 'D', 'E', 'M', 'O', 0 };
constexpr unsigned char kSyncTick = 3; // dem_synctick
// How long a skip may run in one frame before the engine's own 100 packets a frame apply again.
constexpr double kSkipBudgetMs = 250.0;

// tier1/utlbuffer.h's CUtlBuffer, as far as SeekGet reads it.
struct UtlBuffer {
	unsigned char* memory; // CUtlMemory<unsigned char>
	int allocationCount;
	int growSize;
	int get;
	int put;
	unsigned char error;
	unsigned char flags;
	unsigned char reserved;
	unsigned char pad;
	int tab;
	int maxPut;
	int offset; // the file position of memory[0]
	bool (*getOverflow)(UtlBuffer* self, int size); // brings the file around get into memory
};
static_assert(offsetof(UtlBuffer, get) == 0x10 && offsetof(UtlBuffer, error) == 0x18, "CUtlBuffer layout");
static_assert(offsetof(UtlBuffer, maxPut) == 0x20 && offsetof(UtlBuffer, getOverflow) == 0x28, "CUtlBuffer layout");
constexpr unsigned char kGetOverflow = 2;

typedef int (*TickFn)(void* player);
typedef void (*PauseFn)(void* player, float seconds);
typedef void (*SkipToTickFn)(void* player, int tick, bool relative, bool pause);
typedef void* (*ReadPacketFn)(void* player);

void* g_player = nullptr;
SkipToTickFn g_skipToTick = nullptr;
ReadPacketFn g_readPacket = nullptr;
CommandCallback g_fullUpdate = nullptr;
bool g_done = false;
int g_tries = 0;
std::string g_status = "not installed yet";

// The skip under way, for the log and health.
struct Skip {
	bool active = false;
	bool rewound = false;
	int from = 0, to = 0;
	int packets = 0, frames = 0;
	double startMs = 0, ms = 0;
};
Skip g_skip;
Skip g_last;
unsigned long long g_rewinds = 0, g_reloads = 0;
bool g_batchEnded = true; // the last ReadPacket returned nothing: the next call starts a frame's batch
double g_batchStartMs = 0;

// The gaps between the packets the skips of this demo crossed, in ticks (DemoRewind_SeekLandsPast).
constexpr int kMaxGap = 64;
unsigned g_gaps[kMaxGap + 1] = {};
unsigned g_gapCount = 0;
int g_lastPacketTick = -1;
long long g_demoKey = -1; // the demo the gaps are of: its header's sign-on length and ticks

double NowMs() {
	LARGE_INTEGER now, frequency;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&frequency);
	return 1000.0 * (double)now.QuadPart / (double)frequency.QuadPart;
}

void* Slot(void* object, int slot) { return (*(void***)object)[slot]; }

int& Field(void* player, int offset) { return *(int*)((unsigned char*)player + offset); }

// CUtlBuffer::SeekGet(SEEK_HEAD, position), as the engine's (engine.dll+0x28deb0) does it.
bool SeekGet(UtlBuffer* buffer, int position) {
	buffer->get = position;
	if (position > buffer->maxPut) {
		buffer->error |= kGetOverflow;
		return false;
	}
	buffer->error &= ~kGetOverflow;
	if (position >= buffer->offset && position < buffer->offset + buffer->allocationCount) return true;
	return buffer->getOverflow && buffer->getOverflow(buffer, -1);
}

// Puts the demo file back on its first command after the sign-on data, when that reads as the sync
// tick at tick 0 every demo measured starts with. False, the file where it was, otherwise.
bool RewindFile(void* player, int& position) {
	__try {
		unsigned char* file = (unsigned char*)player + kDemoFile;
		if (0 != std::memcmp(file + kFileHeader, kHeaderStamp, sizeof(kHeaderStamp))) return false;
		int signon = 0;
		std::memcpy(&signon, file + kFileHeader + kHeaderSignonLength, sizeof(signon));
		UtlBuffer* buffer = *(UtlBuffer**)(file + kFileBuffer);
		if (!buffer || signon <= 0) return false;
		position = kHeaderBytes + signon;
		const int before = buffer->get;
		bool ok = SeekGet(buffer, position) && position + 5 <= buffer->offset + buffer->allocationCount;
		if (ok) {
			const unsigned char* at = buffer->memory + (position - buffer->offset);
			int tick = -1;
			std::memcpy(&tick, at + 1, sizeof(tick));
			ok = at[0] == kSyncTick && tick == 0;
		}
		if (!ok) SeekGet(buffer, before);
		return ok;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void FinishSkip(void* player) {
	const double ms = NowMs() - g_skip.startMs;
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("demo.rewind", "%s %d -> %d done at %d: %d packets in %.1f ms over %d frame(s)",
			g_skip.rewound ? "rewind" : "skip", g_skip.from, g_skip.to, ((TickFn)Slot(player, kSlotGetPlaybackTick))(player),
			g_skip.packets, ms, g_skip.frames);
	g_skip.active = false;
	g_skip.ms = ms;
	g_last = g_skip;
}

// A new demo starts its gaps over.
void NoteDemo(void* player) {
	__try {
		const unsigned char* header = (const unsigned char*)player + kDemoFile + kFileHeader;
		int ticks = 0, signon = 0;
		std::memcpy(&ticks, header + kHeaderPlaybackTicks, sizeof(ticks));
		std::memcpy(&signon, header + kHeaderSignonLength, sizeof(signon));
		const long long key = ((long long)signon << 32) | (unsigned)ticks;
		if (key == g_demoKey) return;
		g_demoKey = key;
		std::memset(g_gaps, 0, sizeof(g_gaps));
		g_gapCount = 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void HookSkipToTick(void* player, int tick, bool relative, bool pause) {
	if (player != g_player) {
		g_skipToTick(player, tick, relative, pause);
		return;
	}
	NoteDemo(player);
	g_lastPacketTick = -1;
	const int now = ((TickFn)Slot(player, kSlotGetPlaybackTick))(player);
	const int target = relative ? now + tick : tick;
	g_skip = Skip();
	g_skip.from = now;
	g_skip.to = target;
	g_skip.startMs = NowMs();
	int position = 0;
	if (target >= 0 && target < now && RewindFile(player, position)) {
		// The baselines go first (cl_fullupdate's ForceFullUpdate), then the skip runs from the sync
		// tick: CL_ProcessPacketEntities rebuilds every entity on the first, full, snapshot.
		CommandArgs args = {};
		args.argc = 1;
		std::snprintf(args.argvBuffer, sizeof(args.argvBuffer), "cl_fullupdate");
		args.argv0Size = (int)std::strlen(args.argvBuffer);
		args.argv[0] = args.argvBuffer;
		g_fullUpdate(args);
		Field(player, kSkipToTick) = (int)((unsigned)target | kWaitForSyncTick);
		if (pause) ((PauseFn)Slot(player, kSlotPausePlayback))(player, -1.0f);
		g_skip.active = true;
		g_skip.rewound = true;
		++g_rewinds;
		if (Filmmaker::MvmDebugLog_Active())
			Filmmaker::MvmDebugLog_Linef("demo.rewind", "rewind %d -> %d in place: file back to its sync tick at byte %d%s", now, target,
				position, pause ? ", paused" : "");
		return;
	}
	if (target >= 0 && target < now) {
		++g_reloads;
		if (Filmmaker::MvmDebugLog_Active())
			Filmmaker::MvmDebugLog_Linef("demo.rewind", "rewind %d -> %d: the file did not read as expected, the engine reloads the demo", now, target);
	}
	g_skipToTick(player, tick, relative, pause);
	g_skip.active = Field(player, kSkipToTick) != -1;
}

void* HookReadPacket(void* player) {
	if (player != g_player) return g_readPacket(player);
	const bool skipping = Field(player, kSkipToTick) != -1;
	if (g_batchEnded) {
		g_batchEnded = false;
		g_batchStartMs = NowMs();
		if (g_skip.active && skipping) ++g_skip.frames;
	}
	// The engine stops a skip's frame at 100 packets; within the budget the count starts over, so the
	// skip goes on in this frame.
	if (skipping && NowMs() - g_batchStartMs < kSkipBudgetMs) Field(player, kSkipCount) = 0;
	void* packet = g_readPacket(player);
	if (!packet) g_batchEnded = true;
	if (packet && skipping) {
		// Skipping, the playback tick is the packet's own (ReadPacket sets the start tick by it).
		const int tick = ((TickFn)Slot(player, kSlotGetPlaybackTick))(player);
		const int gap = tick - g_lastPacketTick;
		if (g_lastPacketTick >= 0 && gap > 0 && gap <= kMaxGap) {
			++g_gaps[gap];
			++g_gapCount;
		}
		g_lastPacketTick = tick;
	}
	if (g_skip.active) {
		if (packet) ++g_skip.packets;
		if (Field(player, kSkipToTick) == -1) FinishSkip(player);
	}
	return packet;
}

bool InModule(const void* p, uintptr_t lo, uintptr_t hi) { return (uintptr_t)p >= lo && (uintptr_t)p < hi; }

// Whether needle occurs in code[0, scan).
bool CodeHas(const void* code, size_t scan, const unsigned char* needle, size_t size) {
	__try {
		const unsigned char* c = (const unsigned char*)code;
		for (size_t i = 0; i + size <= scan; ++i)
			if (0 == std::memcmp(c + i, needle, size)) return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return false;
}

// demo_gototick's callback ends `mov rcx, [rip + demoplayer]` ... `jmp [rax + 0x70]` (SkipToTick):
// the variable, when it lies in [lo, hi).
void* const* FindDemoPlayerVariable(const unsigned char* code, size_t scan, uintptr_t lo, uintptr_t hi) {
	static const unsigned char kJumpSkipToTick[3] = { 0xFF, 0x60, (unsigned char)(kSlotSkipToTick * 8) };
	__try {
		for (size_t i = 0; i + 7 <= scan; ++i) {
			if (code[i] != 0x48 || code[i + 1] != 0x8B || code[i + 2] != 0x0D) continue;
			int32_t disp;
			std::memcpy(&disp, code + i + 3, 4);
			const unsigned char* variable = code + i + 7 + disp;
			if ((uintptr_t)variable < lo || (uintptr_t)variable + 8 > hi) continue;
			for (size_t j = i + 7; j <= i + 7 + 48 && j + sizeof(kJumpSkipToTick) <= scan; ++j)
				if (0 == std::memcmp(code + j, kJumpSkipToTick, sizeof(kJumpSkipToTick))) return (void* const*)variable;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return nullptr;
}

void* ReadPointer(void* const* variable) {
	__try {
		return *variable;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

void Finish(const std::string& status) {
	g_done = true;
	g_status = status;
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("demo.rewind", "%s", status.c_str());
}

} // namespace

void DemoRewind_Init() {
	if (g_done) return;
	HMODULE engine = GetModuleHandleW(L"engine.dll");
	const CommandCallback gototick = ConCommand_Callback("demo_gototick");
	const CommandCallback fullUpdate = ConCommand_Callback("cl_fullupdate");
	if (!engine || !gototick || !fullUpdate) {
		if (++g_tries > 600) Finish("NOT installed: demo_gototick or cl_fullupdate not found");
		return;
	}
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)engine;
	const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const unsigned char*)engine + dos->e_lfanew);
	const uintptr_t lo = (uintptr_t)engine, hi = lo + nt->OptionalHeader.SizeOfImage;
	if (!InModule((const void*)gototick, lo, hi) || !InModule((const void*)fullUpdate, lo, hi)) {
		Finish("NOT installed: demo_gototick or cl_fullupdate is not engine.dll's");
		return;
	}
	void* const* variable = FindDemoPlayerVariable((const unsigned char*)gototick, 0xC0, lo, hi);
	void* player = variable ? ReadPointer(variable) : nullptr;
	if (!player) {
		if (++g_tries > 600) Finish("NOT installed: no demo player in demo_gototick's code");
		return;
	}
	void** vtable = *(void***)player;
	static const unsigned char kGetDemoFile[5] = { 0x48, 0x8D, 0x41, (unsigned char)kDemoFile, 0xC3 };
	static const unsigned char kSkipField[4] = { 0xBC, 0x05, 0x00, 0x00 };
	static const unsigned char kCountField[4] = { 0xC8, 0x05, 0x00, 0x00 };
	static const unsigned char kFullUpdate[3] = { 0x48, 0x8D, 0x0D }; // lea rcx, [rip + cl]; jmp ForceFullUpdate
	const bool slotsInEngine = InModule(vtable, lo, hi) && InModule(vtable[kSlotGetDemoFile], lo, hi)
		&& InModule(vtable[kSlotGetPlaybackTick], lo, hi) && InModule(vtable[kSlotPausePlayback], lo, hi)
		&& InModule(vtable[kSlotSkipToTick], lo, hi) && InModule(vtable[kSlotReadPacket], lo, hi);
	if (!slotsInEngine || !CodeHas(vtable[kSlotGetDemoFile], sizeof(kGetDemoFile), kGetDemoFile, sizeof(kGetDemoFile))
		|| !CodeHas(vtable[kSlotSkipToTick], 0xC0, kSkipField, sizeof(kSkipField))
		|| !CodeHas(vtable[kSlotReadPacket], 0x100, kSkipField, sizeof(kSkipField))
		|| !CodeHas(vtable[kSlotReadPacket], 0x100, kCountField, sizeof(kCountField))
		|| !CodeHas((const void*)fullUpdate, sizeof(kFullUpdate), kFullUpdate, sizeof(kFullUpdate))) {
		Finish("NOT installed: the demo player's code is not what was measured (the engine reloads on a seek back)");
		return;
	}
	g_player = player;
	g_fullUpdate = fullUpdate;
	// ReadPacket first: g_readPacket is set before a skip can start through the wrapped SkipToTick.
	if (!AfxDetourPtr(&vtable[kSlotReadPacket], (PVOID)&HookReadPacket, (PVOID*)&g_readPacket)
		|| !AfxDetourPtr(&vtable[kSlotSkipToTick], (PVOID)&HookSkipToTick, (PVOID*)&g_skipToTick)) {
		Finish("NOT installed: the demo player's table could not be written");
		return;
	}
	char status[200];
	std::snprintf(status, sizeof(status), "in place (demo player engine.dll+0x%llx, SkipToTick engine.dll+0x%llx)",
		(unsigned long long)((uintptr_t)variable - lo), (unsigned long long)((uintptr_t)g_skipToTick - lo));
	Finish(status);
}

bool DemoRewind_Installed() { return g_player != nullptr; }

int DemoRewind_SeekLandsPast() {
	if (g_gapCount < 20) return 8;
	unsigned seen = 0;
	for (int gap = 1; gap <= kMaxGap; ++gap) {
		seen += g_gaps[gap];
		if (seen * 100ull >= g_gapCount * 99ull) return gap > 1 ? gap - 1 : 1;
	}
	return kMaxGap - 1;
}

void DemoRewind_PrintHealth() {
	if (!g_player) {
		advancedfx::Message("  rewind:     %s\n", g_status.c_str());
		return;
	}
	advancedfx::Message("  rewind:     %s; %llu in place, %llu reloads; seeks land up to %d past (%u packet gaps)", g_status.c_str(),
		g_rewinds, g_reloads, DemoRewind_SeekLandsPast(), g_gapCount);
	if (g_last.to || g_last.from)
		advancedfx::Message("; last %s %d -> %d: %d packets in %.1f ms over %d frame(s)", g_last.rewound ? "rewind" : "skip",
			g_last.from, g_last.to, g_last.packets, g_last.ms, g_last.frames);
	advancedfx::Message("\n");
}

} // namespace SourceFilmmaker
