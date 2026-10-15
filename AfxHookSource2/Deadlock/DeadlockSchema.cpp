#include "DeadlockSchema.h"

#include "DeadlockAddresses.h"

#include "../../Shared/AfxConsole.h"
#include "../../Shared/binutils.h"
#include "../Filmmaker/Debug/MvmDebugLog.h"
#include "../Globals.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace Deadlock {

namespace {

// ---------------------------------------------------------------------------------------------
// Valve's schema structures.
//
// Field ORDER here is the ABI; the two padded runs are the parts that have shifted before, so they
// are named constants rather than literals buried in a struct and can be overridden at runtime.
// Nothing in this file dereferences any of it without the validation below having passed first.
// ---------------------------------------------------------------------------------------------

size_t g_scopeNamePad = 256;  // CSchemaSystemTypeScope: char m_szName[256] after the vfptr
size_t g_scopeCountPad = 0x368; // ...then this much before the declared-class count

struct SchemaField {
	const char* name;
	void* type;
	uint32_t offset;
	uint32_t metadataSize;
	void* metadata;
};

// CSchemaClassInfo, measured on build 6774 using schema raw on C_BaseEntity and
// CGameSceneNode. The added eight bytes at +0x18 shift size/counts and all arrays;
// class names still worked with the old layout, hiding the loss of every field.
struct SchemaClass {
	void* vfptr;                   // 0x00
	const char* name;              // 0x08
	const char* moduleName;        // 0x10
	uint8_t unknown18[8];          // 0x18
	int32_t size;                  // 0x20: C_BaseEntity 1520, CGameSceneNode 304
	int16_t fieldCount;            // 0x24: C_BaseEntity 81, CGameSceneNode 34
	int16_t staticFieldCount;      // 0x26
	uint8_t unknown28[8];          // 0x28: counts and flags
	SchemaField* fields;           // 0x30
	void* staticFields;            // 0x38
	void* metadata;                // 0x40
};
static_assert(offsetof(SchemaClass, size) == 0x20);
static_assert(offsetof(SchemaClass, fieldCount) == 0x24);
static_assert(offsetof(SchemaClass, fields) == 0x30);

struct SchemaDeclaredClass {
	void* vfptr;
	const char* name;
	const char* moduleName;
	const char* unknownStr;
	SchemaClass* klass;
};

struct SchemaDeclaredClassEntry {
	uint64_t hash[2];
	SchemaDeclaredClass* declared;
};

// CSchemaSystem's own header, up to the scope array. This one has held across both games.
constexpr size_t kSystemScopeArrayPad = 0x190;

// ---------------------------------------------------------------------------------------------

void* g_schemaSystem = nullptr;
bool g_ready = false;
bool g_attempted = false;
std::string g_failure;

// class -> field -> offset, plus class -> size. Kept for the lifetime of the process rather than
// cleared after init the way the CS2 side does: this port resolves fields lazily as features come
// up, and `mirv_filmmaker dump schema` has to be able to answer at any time.
std::map<std::string, std::map<std::string, std::ptrdiff_t>> g_fields;
std::map<std::string, uint32_t> g_sizes;
int g_fieldCount = 0;
std::string g_scopeName;

// Every read of a possibly-bogus pointer goes through here. A wrong layout constant produces a
// pointer that is committed but meaningless just as often as one that faults, so the validation
// below does the real work; this only stops the outright faults.
bool Readable(const void* p, size_t bytes) {
	if (!p) return false;
	MEMORY_BASIC_INFORMATION mbi = {};
	if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
	if (mbi.State != MEM_COMMIT) return false;
	if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
	const uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
	return (uintptr_t)p + bytes <= end;
}

// A C string we did not write. Bounded, and rejected unless it is entirely printable ASCII --
// which is the cheapest reliable signal that a layout constant is still right.
bool SafeName(const char* p, size_t limit, std::string& out) {
	if (!Readable(p, 1)) return false;
	out.clear();
	for (size_t i = 0; i < limit; ++i) {
		if (!Readable(p + i, 1)) return false;
		const char c = p[i];
		if (c == 0) return !out.empty();
		if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) return false;
		out.push_back(c);
	}
	return false;
}

unsigned char* ScopeArray(uint64_t& outCount) {
	unsigned char* sys = (unsigned char*)g_schemaSystem;
	if (!Readable(sys + kSystemScopeArrayPad, 16)) return nullptr;
	outCount = *(uint64_t*)(sys + kSystemScopeArrayPad);
	if (outCount == 0 || outCount > 4096) return nullptr;
	return *(unsigned char**)(sys + kSystemScopeArrayPad + 8);
}

// Pulls the name and declared-class table out of one scope, applying the current layout. Returns
// false when anything about it fails validation, which is how a shifted ABI is detected rather
// than trusted.
bool ReadScope(unsigned char* scope, std::string& outName, uint16_t& outCount,
               SchemaDeclaredClassEntry*& outEntries) {
	if (!Readable(scope, 8)) return false;
	if (!SafeName((const char*)(scope + 8), g_scopeNamePad, outName)) return false;

	const size_t countAt = 8 + g_scopeNamePad + g_scopeCountPad;
	if (!Readable(scope + countAt, 16)) return false;
	outCount = *(uint16_t*)(scope + countAt);
	outEntries = *(SchemaDeclaredClassEntry**)(scope + countAt + 8);

	if (outCount == 0) return true; // a real, empty scope
	if (outCount > 20000) return false;
	return Readable(outEntries, sizeof(SchemaDeclaredClassEntry) * outCount);
}

bool HarvestScope(unsigned char* scope, const std::string& scopeName,
                  uint16_t classCount, SchemaDeclaredClassEntry* entries) {
	(void)scope;
	int harvested = 0;
	for (uint16_t i = 0; i < classCount; ++i) {
		SchemaDeclaredClass* declared = entries[i].declared;
		if (!Readable(declared, sizeof(SchemaDeclaredClass))) continue;
		SchemaClass* klass = declared->klass;
		if (!Readable(klass, sizeof(SchemaClass))) continue;

		std::string className;
		if (!SafeName(klass->name, 256, className)) continue;

		g_sizes[className] = (uint32_t)(klass->size > 0 ? klass->size : 0);
		++harvested;

		if (klass->fieldCount <= 0
		    || !Readable(klass->fields, sizeof(SchemaField) * (size_t)klass->fieldCount))
			continue;

		auto& table = g_fields[className];
		for (int16_t f = 0; f < klass->fieldCount; ++f) {
			const SchemaField& field = klass->fields[f];
			if (!field.type) continue;
			std::string fieldName;
			if (!SafeName(field.name, 256, fieldName)) continue;
			// A field offset past the declared class size means the layout is wrong, not that
			// Valve shipped an odd class. Drop it rather than hand a caller a bad write target.
			if (klass->size && field.offset > klass->size) continue;
			table[fieldName] = (std::ptrdiff_t)field.offset;
			++g_fieldCount;
		}
	}
	if (harvested == 0) return false;
	g_scopeName = scopeName;
	return true;
}

bool Resolve() {
	g_fields.clear();
	g_sizes.clear();
	g_fieldCount = 0;
	g_scopeName.clear();
	g_failure.clear();

	if (!g_schemaSystem) {
		g_failure = "the schema system global did not resolve";
		return false;
	}

	uint64_t scopeCount = 0;
	unsigned char* array = ScopeArray(scopeCount);
	if (!array || !Readable(array, sizeof(void*) * (size_t)scopeCount)) {
		g_failure = "the scope array is unreadable -- CSchemaSystem's own layout has shifted";
		return false;
	}

	unsigned char** scopes = (unsigned char**)array;
	int scopesSeen = 0, scopesRejected = 0;
	for (uint64_t i = 0; i < scopeCount; ++i) {
		unsigned char* scope = scopes[i];
		if (!scope) continue;

		std::string name;
		uint16_t classCount = 0;
		SchemaDeclaredClassEntry* entries = nullptr;
		if (!ReadScope(scope, name, classCount, entries)) {
			++scopesRejected;
			continue;
		}
		++scopesSeen;

		// The client scope is the only one this port needs: heroes, scene nodes and everything the
		// follow camera touches are declared there.
		if (name != "client.dll") continue;
		if (!HarvestScope(scope, name, classCount, entries)) {
			g_failure = "the client.dll scope was found but yielded no classes";
			return false;
		}
		// A shifted class layout can preserve thousands of names while losing all fields.
		// Do not advertise readiness or expose offsets unless both core entity relationships
		// resolved by name. Their numeric offsets remain build-derived.
		const auto entity = g_fields.find("C_BaseEntity");
		const auto node = g_fields.find("CGameSceneNode");
		if (g_fieldCount == 0 || entity == g_fields.end() || node == g_fields.end()
		    || !entity->second.count("m_pGameSceneNode") || !node->second.count("m_vecAbsOrigin")) {
			g_failure = "core entity fields are missing -- inspect 'schema raw C_BaseEntity' and 'schema raw CGameSceneNode'";
			return false;
		}
		return true;
	}

	if (scopesSeen == 0) {
		g_failure = scopesRejected
			? "every scope failed validation -- the scope layout constants have shifted"
			: "the scope array was empty";
	} else {
		g_failure = "no client.dll scope in the schema system";
	}
	return false;
}

} // namespace

bool Schema_Init(HMODULE schemaSystemDll) {
	if (g_ready) return true;
	g_attempted = true;

	if (!schemaSystemDll) {
		g_failure = "schemasystem.dll was not loaded";
		return false;
	}

	// The global is stored right before the lea of the "schema_list_bindings <substring>" help
	// text. Same shape in Deadlock as in CS2 -- confirmed unique in this build's schemasystem.dll
	// by Automation/verify/dump-deadlock-offsets.py, which records where it landed.
	const size_t at = getAddress(schemaSystemDll,
		"48 89 05 ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 33 C0 48 C7 05");
	if (!at) {
		g_failure = "the schema system pattern did not match";
		advancedfx::Warning(
			"AfxHookDeadlock: the schema system pattern did not match. Entity features stay off;\n"
			"re-derive with Automation/verify/dump-deadlock-offsets.py.\n");
		return false;
	}

	int32_t disp = 0;
	std::memcpy(&disp, (void*)(at + 3), sizeof(disp));
	g_schemaSystem = (void*)(at + 7 + (ptrdiff_t)disp);

	g_ready = Resolve();
	if (g_ready) {
		advancedfx::Message("AfxHookDeadlock: schema resolved -- %d classes, %d fields.\n",
		                    (int)g_sizes.size(), g_fieldCount);
		if (Filmmaker::MvmDebugLog_Active())
			Filmmaker::MvmDebugLog_LinefAlways("deadlock.schema",
				"ready system=%p classes=%d fields=%d", g_schemaSystem,
				(int)g_sizes.size(), g_fieldCount);
	} else {
		advancedfx::Warning(
			"AfxHookDeadlock: schema resolution failed (%s). Entity features stay off; repair the\n"
			"layout with 'mirv_filmmaker schema layout <scopeNamePad> <scopeCountPad>'.\n",
			g_failure.c_str());
	}
	return g_ready;
}

bool Schema_Reload() {
	if (!g_schemaSystem) return false;
	g_ready = Resolve();
	return g_ready;
}

bool Schema_Ready() { return g_ready; }

std::ptrdiff_t Schema_FieldOffset(const char* klass, const char* field) {
	if (!g_ready || !klass || !field) return -1;
	auto c = g_fields.find(klass);
	if (c == g_fields.end()) return -1;
	auto f = c->second.find(field);
	if (f == c->second.end()) return -1;
	return f->second;
}

std::ptrdiff_t Schema_FieldOffsetAny(const char* const* classes, int classCount,
                                     const char* field) {
	for (int i = 0; i < classCount; ++i) {
		const std::ptrdiff_t off = Schema_FieldOffset(classes[i], field);
		if (off >= 0) return off;
	}
	return -1;
}

uint32_t Schema_ClassSize(const char* klass) {
	if (!g_ready || !klass) return 0;
	auto it = g_sizes.find(klass);
	return it == g_sizes.end() ? 0 : it->second;
}

bool Schema_HasClass(const char* klass) {
	return g_ready && klass && g_fields.count(klass) != 0;
}

int Schema_ClassCount() { return (int)g_sizes.size(); }
int Schema_FieldCount() { return g_fieldCount; }

bool Schema_DumpClassBytes(const char* klass, std::string& out) {
	out.clear();
	if (!klass || !*klass) return false;

	// The declared-class table is walked again rather than cached, because the whole point of this
	// command is to be usable when the cached table came out wrong.
	uint64_t scopeCount = 0;
	unsigned char* array = ScopeArray(scopeCount);
	if (!array || !Readable(array, sizeof(void*) * (size_t)scopeCount)) return false;
	unsigned char** scopes = (unsigned char**)array;

	for (uint64_t i = 0; i < scopeCount; ++i) {
		unsigned char* scope = scopes[i];
		if (!scope) continue;
		std::string name;
		uint16_t classCount = 0;
		SchemaDeclaredClassEntry* entries = nullptr;
		if (!ReadScope(scope, name, classCount, entries)) continue;
		if (name != "client.dll") continue;

		for (uint16_t c = 0; c < classCount; ++c) {
			SchemaDeclaredClass* declared = entries[c].declared;
			if (!Readable(declared, sizeof(SchemaDeclaredClass))) continue;
			SchemaClass* k = declared->klass;
			if (!Readable(k, 0x40)) continue;
			std::string className;
			if (!SafeName(k->name, 256, className)) continue;
			if (className != klass) continue;

			const unsigned char* p = (const unsigned char*)k;
			char line[256];
			for (int row = 0; row < 4; ++row) {
				int n = std::snprintf(line, sizeof(line), "  +0x%02x ", row * 16);
				for (int b = 0; b < 16; ++b)
					n += std::snprintf(line + n, sizeof(line) - n, "%02x ", p[row * 16 + b]);
				out += line;
				out += "\n";
			}
			std::snprintf(line, sizeof(line),
			              "  read as: size=%d fields=%d staticFields=%d fieldsPtr=%p\n",
			              (int)k->size, (int)k->fieldCount, (int)k->staticFieldCount,
			              (void*)k->fields);
			out += line;

			// WHICH POINTER IS THE FIELD ARRAY. Three pointers sit in a row at +0x28, +0x30 and
			// +0x38, and only one of them is the fields. Reading the wrong one does not error --
			// it yields a short list of plausible names attached to meaningless offsets, which is
			// exactly what the first corrected layout produced. So the probe reads all three and
			// prints what each would give; the right one is the array whose names are m_-prefixed
			// and whose offsets climb through the class size.
			//
			// Build 6774: +0x30 is the field array; +0x28 contains counts/flags.
			for (size_t cand = 0x28; cand <= 0x38; cand += 8) {
				SchemaField* arr = *(SchemaField**)(p + cand);
				std::snprintf(line, sizeof(line), "  +0x%02zx -> %p", cand, (void*)arr);
				out += line;
				if (arr && Readable(arr, sizeof(SchemaField) * 4)) {
					for (int f = 0; f < 4; ++f) {
						std::string fn;
						if (!SafeName(arr[f].name, 128, fn)) { out += "  <bad>"; break; }
						std::snprintf(line, sizeof(line), "  %s@%u", fn.c_str(),
						              (unsigned)arr[f].offset);
						out += line;
					}
				} else {
					out += "  <unreadable>";
				}
				out += "\n";
			}
			return true;
		}
		return false;
	}
	return false;
}

void Schema_SetLayout(size_t scopeNamePad, size_t scopeCountPad) {
	if (scopeNamePad) g_scopeNamePad = scopeNamePad;
	if (scopeCountPad) g_scopeCountPad = scopeCountPad;
}

void Schema_GetLayout(size_t& scopeNamePad, size_t& scopeCountPad) {
	scopeNamePad = g_scopeNamePad;
	scopeCountPad = g_scopeCountPad;
}

std::wstring Schema_DefaultDumpPath() {
	// The hook is staged into the game directory, so the repository is not reachable from here by
	// construction. Honour an explicit override, else fall back to %TEMP% and say where it went.
	wchar_t buf[MAX_PATH] = {};
	if (GetEnvironmentVariableW(L"SOURCEMVM_DEADLOCK_OFFSETS_DIR", buf, MAX_PATH) && buf[0]) {
		std::wstring dir(buf);
		if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
		return dir + L"deadlock-schema.json";
	}
	wchar_t tmp[MAX_PATH] = {};
	if (GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH) && tmp[0])
		return std::wstring(tmp) + L"\\deadlock-schema.json";
	return L"deadlock-schema.json";
}

bool Schema_DumpJson(const wchar_t* path, const char* filter) {
	if (!g_ready || !path) return false;

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) return false;

	const std::string want = filter ? filter : "";

	out << "{\n  \"scope\": \"" << g_scopeName << "\",\n";
	out << "  \"classes\": {\n";
	bool firstClass = true;
	for (const auto& sizeEntry : g_sizes) {
		const std::string& className = sizeEntry.first;
		if (!want.empty() && className.find(want) == std::string::npos) continue;
		if (!firstClass) out << ",\n";
		firstClass = false;
		out << "    \"" << className << "\": { \"size\": " << sizeEntry.second << ", \"fields\": {";
		auto it = g_fields.find(className);
		if (it != g_fields.end()) {
			bool firstField = true;
			for (const auto& field : it->second) {
				if (!firstField) out << ",";
				firstField = false;
				out << "\n      \"" << field.first << "\": " << field.second;
			}
			if (!firstField) out << "\n    ";
		}
		out << "} }";
	}
	out << "\n  }\n}\n";
	return out.good();
}

void Schema_Report() {
	if (!g_attempted) {
		advancedfx::Message("  schema:    not attempted (schemasystem.dll has not loaded)\n");
		return;
	}
	if (!g_ready) {
		advancedfx::Warning("  schema:    BROKEN -- %s\n", g_failure.c_str());
		advancedfx::Message("             layout scopeNamePad=0x%zx scopeCountPad=0x%zx\n",
		                    g_scopeNamePad, g_scopeCountPad);
		return;
	}
	advancedfx::Message(
		"  schema:    %s  classes=%d  fields=%d  (offsets resolved by name, never hardcoded)\n",
		g_scopeName.c_str(), (int)g_sizes.size(), g_fieldCount);
}

} // namespace Deadlock
