#include "ToolVersion.h"
#include "ChangelogMath.h"

#include "../Config/FilmmakerSettingsStore.h" // whatsnew.seen
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../Platform/ShellOpen.h"
#include "../../hlaeFolder.h" // GetHlaeFolderW: the runtime folder, with a trailing backslash

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace Filmmaker {

namespace {

constexpr const char* kSeenKey = "whatsnew.seen";
// Every section past the ones the window opens with is a line under EARLIER VERSIONS; a few
// releases back is plenty there, GitHub has the rest.
constexpr size_t kMaxSections = 8;

std::string ReadText(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return {};
	std::ostringstream s;
	s << in.rdbuf();
	return s.str();
}

// <payload>/runtime, the folder above x64/AfxHookSource2.dll (GetHlaeFolderW ends in a backslash).
fs::path RuntimeFolder() { return fs::path(GetHlaeFolderW()).parent_path(); }

} // namespace

const std::string& ToolVersion_Runtime() {
	static const std::string version = [] {
		const std::string text = ReadText(RuntimeFolder().parent_path() / "installed.json");
		JsonValue root;
		if (text.empty() || !JsonParse(text, root)) return std::string();
		const JsonValue* runtime = root.Find("runtime");
		const JsonValue* v = runtime ? runtime->Find("version") : nullptr;
		return (v && v->type == JsonValue::Type::String) ? v->str : std::string();
	}();
	return version;
}

std::string ToolVersion_Label() {
	const ChangelogVersion v = ParseChangelogVersion(ToolVersion_Runtime());
	return v.Valid() ? ChangelogVersionLabel(v) : std::string("dev build");
}

std::string ToolVersion_WhatsNewJson() {
	const std::vector<ChangelogSection> sections = ParseChangelog(ReadText(RuntimeFolder() / "resources" / "CHANGELOG.md"));
	const std::string& runtime = ToolVersion_Runtime();
	const ChangelogVersion current = ParseChangelogVersion(runtime);
	const std::string seenText = FilmmakerSettings_UiString(kSeenKey, "");
	const ChangelogVersion seen = ParseChangelogVersion(seenText);

	std::vector<size_t> top = ChangelogWhatsNew(sections, current, seen);
	// Already seen: opened by hand, it leads with the release you are on.
	const bool fresh = current.Valid() && seenText != runtime && !top.empty();
	if (top.empty()) top = ChangelogWhatsNew(sections, current, ChangelogVersion{});

	JsonBuilder b;
	b.BeginObject();
	b.StringField("label", ToolVersion_Label());
	b.BoolField("dev", !current.Valid());
	b.BoolField("open", fresh);
	b.Key("sections"); b.BeginArray();
	size_t written = 0;
	for (size_t i = 0; i < sections.size() && written < kMaxSections; ++i) {
		const ChangelogSection& s = sections[i];
		bool isTop = false;
		for (size_t t : top) if (t == i) isTop = true;
		// A dev build's newer-than-installed history is what Unreleased already says.
		if (!s.unreleased && current.Valid() && CompareChangelogVersion(s.version, current) > 0) continue;
		if (s.unreleased && !isTop) continue;
		b.BeginObject();
		b.StringField("title", s.title);
		b.StringField("date", s.date);
		b.BoolField("top", isTop);
		b.Key("groups"); b.BeginArray();
		for (const ChangelogGroup& g : s.groups) {
			b.BeginObject();
			b.StringField("name", g.name);
			b.Key("items"); b.BeginArray();
			for (const std::string& item : g.items) b.ValueString(item);
			b.EndArray();
			b.EndObject();
		}
		b.EndArray();
		b.EndObject();
		++written;
	}
	b.EndArray();
	b.EndObject();
	return b.Str();
}

void ToolVersion_MarkWhatsNewSeen() {
	const std::string& runtime = ToolVersion_Runtime();
	if (!runtime.empty()) FilmmakerSettings_SetUi(kSeenKey, runtime.c_str());
}

void ToolVersion_ForgetWhatsNewSeen() {
	FilmmakerSettings_SetUi(kSeenKey, "none");
}

void ToolVersion_OpenChangelogOnline() {
	OpenUrlAsync(L"https://github.com/Wockstajit/SourceMVM/blob/main/CHANGELOG.md");
}

} // namespace Filmmaker
