#include "SkyboxLibrary.h"
#include "../../SceneSystem.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_Linef
#include "../../../Shared/AfxConsole.h"

// SourceSdkShared.h must precede cdll_int.h -- it defines SOURCESDK_abstract_class, without
// which cdll_int.h fails to parse with a cascade of "missing type specifier" errors.
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cstdio>
#include <cstring>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

std::vector<SkyboxEntry> g_entries;
std::string g_current;
bool g_loadedOnce = false;

void ExecCmd(const char* command) {
	if (!g_pEngineToClient || !command) return;
	g_pEngineToClient->ExecuteClientCmd(0, command, true);
}

// --------------------------------------------------------------------------------------------
// Built-in skyboxes.
//
// ENUMERATED, NOT GUESSED (CLAUDE.md: do not guess material paths). The full set of
// materials/skybox/*.vmat_c was listed out of pak01_dir.vpk with Source2Viewer-CLI; this is a
// curated subset of that list -- the map skies plus the generic weather ones -- skipping the
// lighting/fog variants and the Hosek-Wilkie reference grid, which are not usable backdrops.
//
// Their thumbnails are generated OFFLINE by FX/tools/build-builtin-sky-previews.ps1, which mines
// them out of the VPK, rather than decoded here -- a BC6H cubemap decoder in the DLL would be a
// lot of machinery for a picture. The path is derived from the name by convention, and the UI
// falls back to a letter tile if the texture is absent, so a sky whose thumbnail could not be
// built (or a machine where that script was never run) degrades quietly instead of breaking.
// --------------------------------------------------------------------------------------------
struct BuiltinSky { const char* name; const char* label; const char* material; };

const BuiltinSky kBuiltins[] = {
	{ "sky_de_mirage",         "Mirage",        "materials/skybox/sky_de_mirage.vmat" },
	{ "sky_de_dust2",          "Dust II",       "materials/skybox/sky_de_dust2.vmat" },
	{ "sky_de_nuke",           "Nuke",          "materials/skybox/sky_de_nuke.vmat" },
	{ "sky_de_vertigo",        "Vertigo",       "materials/skybox/sky_de_vertigo.vmat" },
	{ "sky_de_overpass_01",    "Overpass",      "materials/skybox/sky_de_overpass_01.vmat" },
	{ "sky_de_annubis",        "Anubis",        "materials/skybox/sky_de_annubis.vmat" },
	{ "sky_de_train03",        "Train",         "materials/skybox/sky_de_train03.vmat" },
	{ "sky_hr_aztec_02",       "Aztec",         "materials/skybox/sky_hr_aztec_02.vmat" },
	{ "sky_ar_baggage_01",     "Baggage",       "materials/skybox/sky_ar_baggage_01.vmat" },
	{ "sky_cs15_daylight01_hdr", "Daylight",    "materials/skybox/sky_cs15_daylight01_hdr.vmat" },
	{ "sky_csgo_cloudy01",     "Cloudy",        "materials/skybox/sky_csgo_cloudy01.vmat" },
	{ "sky_overcast_01",       "Overcast",      "materials/skybox/sky_overcast_01.vmat" },
	{ "sky_rain_night_01",     "Rainy Night",   "materials/skybox/sky_rain_night_01.vmat" },
	{ "sky_black",             "Black",         "materials/skybox/sky_black.vmat" },
};

std::string JsonEscape(const std::string& value) {
	std::string out;
	out.reserve(value.size() + 8);
	for (char c : value) {
		switch (c) {
		case '\"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if ((unsigned char)c < 0x20) { char b[8]; _snprintf_s(b, sizeof(b), _TRUNCATE, "\\u%04x", c); out += b; }
			else out += c;
			break;
		}
	}
	return out;
}

std::string LibraryIndexPath() {
	// %APPDATA%\HLAE\filmmaker\skyboxes\skyboxes.json -- the exact path build-skybox.ps1 writes.
	// SHGetFolderPath rather than getenv("APPDATA") so it still resolves under a launcher that
	// scrubs the environment.
	char appData[MAX_PATH] = { 0 };
	if (S_OK != SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData)) return std::string();
	std::string path(appData);
	path += "\\HLAE\\filmmaker\\skyboxes\\skyboxes.json";
	return path;
}

bool ReadWholeFile(const std::string& path, std::string& out) {
	HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (INVALID_HANDLE_VALUE == h) return false;
	out.clear();
	char buf[4096];
	DWORD got = 0;
	while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
	CloseHandle(h);
	return true;
}

// ---------------------------------------------------------------------------------------------
// A deliberately small JSON reader.
//
// It only has to read a file THIS repo generates, whose shape is fixed: an array of flat objects
// with string/number values and no nesting. Pulling in a general JSON library to parse our own
// output would be a new dependency for no gain (CLAUDE.md: avoid new dependencies unless
// necessary). Anything it cannot make sense of yields no entries, never a crash or a partial
// entry -- a malformed index must degrade to "no custom skyboxes", never to a bad material path.
// ---------------------------------------------------------------------------------------------
void SkipWs(const std::string& s, size_t& i) {
	while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
}

// Reads a "..."-quoted string starting at s[i] == '"'. False if unterminated.
bool ReadString(const std::string& s, size_t& i, std::string& out) {
	if (i >= s.size() || s[i] != '\"') return false;
	++i;
	out.clear();
	while (i < s.size()) {
		char c = s[i++];
		if (c == '\"') return true;
		if (c == '\\' && i < s.size()) {
			char e = s[i++];
			switch (e) {
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case 'u': i += 4; break; // no non-ASCII expected in a sanitised name; skip the escape
			default:  out += e; break;
			}
			continue;
		}
		out += c;
	}
	return false;
}

// Consumes one value (string, number, bool, null) without interpreting non-strings -- enough to
// step over "exposure" and "created" while looking for the keys that matter.
void SkipValue(const std::string& s, size_t& i) {
	SkipWs(s, i);
	if (i >= s.size()) return;
	if (s[i] == '\"') { std::string ignored; ReadString(s, i, ignored); return; }
	while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
}

void ParseIndex(const std::string& json, std::vector<SkyboxEntry>& out) {
	size_t i = 0;
	SkipWs(json, i);
	if (i >= json.size() || json[i] != '[') return;
	++i;

	while (i < json.size()) {
		SkipWs(json, i);
		if (i >= json.size() || json[i] == ']') break;
		if (json[i] == ',') { ++i; continue; }
		if (json[i] != '{') break;
		++i;

		SkyboxEntry e;
		while (i < json.size()) {
			SkipWs(json, i);
			if (i >= json.size()) break;
			if (json[i] == '}') { ++i; break; }
			if (json[i] == ',') { ++i; continue; }

			std::string key;
			if (!ReadString(json, i, key)) return; // malformed: abandon the whole file
			SkipWs(json, i);
			if (i < json.size() && json[i] == ':') ++i;
			SkipWs(json, i);

			if (key == "name" || key == "label" || key == "material" || key == "image"
				|| key == "sourcePath") {
				std::string value;
				if (json[i] == '\"') {
					if (!ReadString(json, i, value)) return;
				} else {
					SkipValue(json, i); // null/number where a string was expected -> treat as empty
				}
				if (key == "name") e.name = value;
				else if (key == "label") e.label = value;
				else if (key == "material") e.material = value;
				else if (key == "sourcePath") e.sourcePath = value;
				else e.image = value;
			} else if (key == "rotation") {
				size_t start = i;
				SkipValue(json, i);
				e.rotation = atof(json.substr(start, i - start).c_str());
			} else if (key == "flipped") {
				size_t start = i;
				SkipValue(json, i);
				e.flipped = (json.compare(start, 4, "true") == 0);
			} else {
				SkipValue(json, i);
			}
		}

		// name + material are the two fields without which a card cannot do anything.
		if (!e.name.empty() && !e.material.empty()) {
			if (e.label.empty()) e.label = e.name;
			e.builtin = false;
			out.push_back(e);
		}
	}
}

} // namespace

void SkyboxLibrary_Refresh() {
	g_loadedOnce = true;
	g_entries.clear();

	// Custom entries first: this feature exists for skyboxes you made, so they head the list
	// rather than being buried under fourteen stock ones.
	const std::string indexPath = LibraryIndexPath();
	int customCount = 0;
	if (!indexPath.empty()) {
		std::string json;
		if (ReadWholeFile(indexPath, json)) {
			ParseIndex(json, g_entries);
			customCount = (int)g_entries.size();
		}
	}

	for (const BuiltinSky& b : kBuiltins) {
		SkyboxEntry e;
		e.name = b.name;
		e.label = b.label;
		e.material = b.material;
		e.image = std::string("s2r://panorama/images/filmmaker/skyboxes/builtin_") + b.name + ".vtex";
		e.builtin = true;
		g_entries.push_back(e);
	}

	MvmDebugLog_Linef("world.sky", "refresh: %d custom + %d builtin from \"%s\"",
		customCount, (int)(sizeof(kBuiltins) / sizeof(kBuiltins[0])), indexPath.c_str());
}

const std::vector<SkyboxEntry>& SkyboxLibrary_Entries() {
	if (!g_loadedOnce) SkyboxLibrary_Refresh();
	return g_entries;
}

bool SkyboxLibrary_Apply(const char* name) {
	if (!name || !*name) return false;
	if (!g_loadedOnce) SkyboxLibrary_Refresh();

	for (const SkyboxEntry& e : g_entries) {
		if (0 != _stricmp(e.name.c_str(), name)) continue;

		SceneSystem_SetSkyMaterial(e.material.c_str());
		g_current = e.name;
		MvmDebugLog_Linef("world.sky", "apply \"%s\" -> %s", e.name.c_str(), e.material.c_str());
		return true;
	}
	return false;
}

void SkyboxLibrary_Default() {
	SceneSystem_SetSkyMaterial("");
	g_current.clear();
	MvmDebugLog_Linef("world.sky", "apply default (map's own sky)");
}

const std::string& SkyboxLibrary_Current() { return g_current; }

namespace {

// Rewrites skyboxes.json without `name`. Rebuilt from the entries currently in memory rather
// than by editing text, so the file cannot end up half-valid.
bool WriteIndexWithout(const std::string& dropName, std::string* outReason) {
	const std::string path = LibraryIndexPath();
	if (path.empty()) {
		if (outReason) *outReason = "Could not locate %APPDATA%\\HLAE\\filmmaker\\skyboxes.";
		return false;
	}

	std::string json = "[\n";
	bool first = true;
	for (const SkyboxEntry& e : g_entries) {
		if (e.builtin) continue;                       // stock skies are not in the file
		if (0 == _stricmp(e.name.c_str(), dropName.c_str())) continue;
		if (!first) json += ",\n";
		first = false;
		json += "  {\"name\":\"" + JsonEscape(e.name) + "\"";
		json += ",\"label\":\"" + JsonEscape(e.label) + "\"";
		json += ",\"material\":\"" + JsonEscape(e.material) + "\"";
		json += ",\"image\":\"" + JsonEscape(e.image) + "\"";
		json += ",\"sourcePath\":\"" + JsonEscape(e.sourcePath) + "\"";
		char num[64];
		_snprintf_s(num, sizeof(num), _TRUNCATE, "%.2f", e.rotation);
		json += ",\"rotation\":";
		json += num;
		json += ",\"flipped\":";
		json += (e.flipped ? "true" : "false");
		json += "}";
	}
	json += "\n]\n";

	HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (INVALID_HANDLE_VALUE == h) {
		if (outReason) *outReason = "Could not write " + path;
		return false;
	}
	DWORD written = 0;
	const BOOL ok = WriteFile(h, json.data(), (DWORD)json.size(), &written, nullptr);
	CloseHandle(h);
	if (!ok) {
		if (outReason) *outReason = "Failed writing " + path;
		return false;
	}
	return true;
}

// Best-effort. A leftover compiled file is harmless -- the entry is gone from the index, so it
// no longer appears in the picker -- which is why a failed delete here is not an error.
void DeleteQuietly(const std::string& path) {
	DeleteFileA(path.c_str());
}

} // namespace

bool SkyboxLibrary_Delete(const char* name, std::string* outReason) {
	if (!name || !*name) { if (outReason) *outReason = "No skybox name given."; return false; }
	if (!g_loadedOnce) SkyboxLibrary_Refresh();

	const SkyboxEntry* found = nullptr;
	for (const SkyboxEntry& e : g_entries) {
		if (0 == _stricmp(e.name.c_str(), name)) { found = &e; break; }
	}
	if (!found) { if (outReason) *outReason = std::string("No skybox named \"") + name + "\"."; return false; }
	if (found->builtin) {
		if (outReason) *outReason = "That is one of CS2's own skies - it is not yours to delete.";
		return false;
	}

	const std::string doomed = found->name;

	// If it is on screen, get off it first: leaving the view on a material that is about to be
	// deleted is how you get an error texture instead of a sky.
	if (0 == _stricmp(g_current.c_str(), doomed.c_str())) SkyboxLibrary_Default();

	if (!WriteIndexWithout(doomed, outReason)) return false;

	// The on-disk preview beside the index. The compiled game-side files are left alone: they sit
	// under the CS2 install, this module has no reliable path to them, and an orphaned .vmat_c
	// costs a few KB and is overwritten if the same name is imported again.
	char appData[MAX_PATH] = { 0 };
	if (S_OK == SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData)) {
		std::string dir(appData);
		dir += "\\HLAE\\filmmaker\\skyboxes\\";
		DeleteQuietly(dir + doomed + ".png");
	}

	SkyboxLibrary_Refresh();
	MvmDebugLog_Linef("world.sky", "deleted \"%s\"", doomed.c_str());
	return true;
}

void SkyboxLibrary_AppendStateJson(std::ostringstream& o) {
	const std::vector<SkyboxEntry>& entries = SkyboxLibrary_Entries();
	o << ",\"skyCurrent\":\"" << JsonEscape(g_current) << "\"";
	o << ",\"skyList\":[";
	for (size_t i = 0; i < entries.size(); ++i) {
		const SkyboxEntry& e = entries[i];
		if (i) o << ",";
		o << "{\"name\":\"" << JsonEscape(e.name) << "\""
		  << ",\"label\":\"" << JsonEscape(e.label) << "\""
		  << ",\"image\":\"" << JsonEscape(e.image) << "\""
		  << ",\"rotation\":" << e.rotation
		  << ",\"flipped\":" << (e.flipped ? "true" : "false")
		  // Whether the Settings tab can rebuild this one at all: a stock sky has no source image
		  // to re-roll, so the UI greys out rotate/flip instead of failing on click.
		  << ",\"editable\":" << (!e.sourcePath.empty() ? "true" : "false")
		  << ",\"builtin\":" << (e.builtin ? "true" : "false") << "}";
	}
	o << "]";
}

void SkyboxLibrary_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// argv: mirv_filmmaker world sky <op> ...
	const char* op = (argc >= 4) ? args->ArgV(3) : "";

	if (0 == _stricmp(op, "refresh")) {
		SkyboxLibrary_Refresh();
		advancedfx::Message("Skybox library reloaded: %d entries.\n", (int)g_entries.size());
		return;
	}

	if (0 == _stricmp(op, "list")) {
		const std::vector<SkyboxEntry>& entries = SkyboxLibrary_Entries();
		advancedfx::Message("Skyboxes (%d):\n", (int)entries.size());
		for (const SkyboxEntry& e : entries) {
			advancedfx::Message("  %-28s %-10s %s%s\n",
				e.name.c_str(),
				e.builtin ? "[builtin]" : "[custom]",
				e.material.c_str(),
				(e.name == g_current) ? "   <-- active" : "");
		}
		advancedfx::Message("Build one from your own image: FX/tools/build-skybox.ps1\n");
		return;
	}

	if (0 == _stricmp(op, "delete")) {
		if (argc < 5) { advancedfx::Message("Usage: %s world sky delete <name>\n", cmd); return; }
		std::string reason;
		if (SkyboxLibrary_Delete(args->ArgV(4), &reason)) {
			advancedfx::Message("Deleted skybox \"%s\".\n", args->ArgV(4));
		} else {
			advancedfx::Message("%s\n", reason.c_str());
		}
		return;
	}

	if (0 == _stricmp(op, "default")) {
		SkyboxLibrary_Default();
		advancedfx::Message("Skybox restored to the map's own.\n");
		return;
	}

	if (0 == _stricmp(op, "apply")) {
		if (argc < 5) { advancedfx::Message("Usage: %s world sky apply <name>\n", cmd); return; }
		const char* name = args->ArgV(4);
		if (SkyboxLibrary_Apply(name)) {
			advancedfx::Message("Skybox applied: %s\n", name);
		} else {
			// Naming the failure beats a silent no-op -- the usual cause is a skybox built after
			// the list was last read.
			advancedfx::Message(
				"No skybox named \"%s\". Run `%s world sky list`, "
				"or `%s world sky refresh` if you just built it.\n", name, cmd, cmd);
		}
		return;
	}

	advancedfx::Message(
		"mirv_filmmaker world sky - skybox library.\n"
		"Usage:\n"
		"  mirv_filmmaker world sky list           - every selectable skybox\n"
		"  mirv_filmmaker world sky apply <name>   - swap to it (instant, no map reload)\n"
		"  mirv_filmmaker world sky default        - back to the map's own sky\n"
		"  mirv_filmmaker world sky delete <name>  - remove one of YOUR skyboxes\n"
		"  mirv_filmmaker world sky refresh        - re-read the library index\n"
		"Import your own image/HDRI: FX/tools/build-skybox.ps1\n"
		"Docs: Docs/effects/skybox.md\n");
}

} // namespace Filmmaker
