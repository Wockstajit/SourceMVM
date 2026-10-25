#define NOMINMAX
#include "AltEnvironment.h"
#include "AltEnvironmentCopies.h"
#include "AltEnvironmentProps.h"
#include "AltEnvironmentSceneProbe.h"

#include "../Filmmaker.h" // PlayingDemoPath, PlayResolvedDemo, SwitchPlayingCopy
#include "../Config/FilmmakerSettingsStore.h" // FilmmakerSettings_UiString: CONFIG's open tab
#include "../Cosmetics/CosmeticEntityAccess.h"
#include "../Debug/MvmDebugLog.h"
#include "../Demo/DemoHeaderReader.h"
#include "../Demo/DemoInfoHelper.h"
#include "../Demo/DemoKeyframes.h"
#include "../Demo/DemoProjectStore.h"
#include "../Demo/DemoRepair.h"
#include "../Demo/DemoWorldSwap.h"
#include "../Environment/EnvironmentOcclusion.h"
#include "../Heatmap/RoundHeatmapMath.h"
#include "../Panorama/ConfigHud.h" // ConfigHud_Enabled: the map view lives in CONFIG
#include "../MapData/MapEntities.h"
#include "../MapData/MapSources.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../Platform/LocalImageServer.h"
#include "../Platform/PngWrite.h"
#include "../Platform/TextEncoding.h"
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "../../../Shared/AfxConsole.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;
extern CMirvTime g_MirvTime;

namespace Filmmaker::AltEnvironment {

namespace {

namespace fs = std::filesystem;

constexpr double kAlignCell = 16;  // world units per cell of the rasters AUTO ALIGN compares
constexpr double kPictureCell = 8; // finest cell of the pictures; big maps get coarser ones
constexpr int kPictureMax = 900;   // longest picture side in pixels
constexpr size_t kPictureFeet = 60000;
constexpr uint64_t kTinyMap = 1u << 20; // a "map" package this small is a placeholder, not a world

double NowMs() {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Lower(std::string s) {
	for (char& c : s) c = char(tolower((unsigned char)c));
	return s;
}

std::string Fixed(double v) {
	char b[32];
	snprintf(b, sizeof(b), "%.3f", std::fabs(v) < 0.0005 ? 0.0 : v);
	return b;
}

// Background workers never compete with the game's frame threads.
struct BackgroundPriority {
	HANDLE thread = GetCurrentThread();
	int previous = GetThreadPriority(thread);
	BackgroundPriority() { SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL); }
	~BackgroundPriority() { SetThreadPriority(thread, previous); }
};

template <class T> bool Ready(const std::future<T>& f) {
	return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// ---- the replacement map ------------------------------------------------------------------------

// What the copy needs to know about a replacement map, read from its own package.
struct MapInfo {
	std::string map, title, addon, skybox;
	bool workshop = false, tiny = false;
};

std::string Pretty(const std::string& map) {
	std::string s = map;
	const size_t underscore = s.find('_');
	if (underscore != std::string::npos && underscore <= 3) s = s.substr(underscore + 1);
	bool up = true;
	for (char& c : s) {
		if (c == '_') { c = ' '; up = true; continue; }
		if (up) c = char(toupper((unsigned char)c));
		up = false;
	}
	return s;
}

std::string PublishTitle(const fs::path& item) {
	std::ifstream in(item / "publish_data.txt", std::ios::binary);
	std::string line;
	while (in && std::getline(in, line)) {
		const size_t key = line.find("\"title\"");
		if (key == std::string::npos) continue;
		const size_t a = line.find('"', key + 7), b = a == std::string::npos ? a : line.rfind('"');
		if (a != std::string::npos && b > a) return line.substr(a + 1, b - a - 1);
	}
	return std::string();
}

// The single-file or _dir package of a Workshop item.
fs::path ItemPackage(const fs::path& item) {
	std::error_code ec;
	const std::string id = item.filename().u8string();
	for (const auto& name : { id + "_dir.vpk", id + ".vpk" })
		if (fs::is_regular_file(item / name, ec)) return item / name;
	for (const auto& e : fs::directory_iterator(item, ec)) {
		const std::string n = Lower(e.path().filename().u8string());
		if (n.size() > 8 && n.compare(n.size() - 8, 8, "_dir.vpk") == 0) return e.path();
	}
	return fs::path();
}

// The installed Workshop item whose package holds maps/<map>.vpk, or "" if none does. A map package
// copied into csgo/maps on its own lacks the materials and models its item ships beside it -- El
// Dorado's csgo/maps/de_eldorado.vpk holds only maps/de_eldorado/, its 128 materials/eldorado/
// files are in item 3408790618 (measured 2026-10-24) -- so the copy has to mount the item to draw it.
// Remembered per map until the next map scan; any thread.
std::mutex g_itemMx;
std::map<std::string, std::string> g_itemFor;

// Whether installed Workshop item `id` holds maps/<map>.vpk.
bool WorkshopItemHas(const std::string& id, const std::string& map) {
	const fs::path root = MapData::ResolveCs2Root();
	if (root.empty() || id.empty() || id.find_first_not_of("0123456789") != std::string::npos) return false;
	const fs::path package = ItemPackage(root.parent_path().parent_path() / "workshop" / "content" / "730" / id);
	if (package.empty()) return false;
	try {
		MapData::Vpk vpk(package);
		const std::string want = "maps/" + Lower(map) + ".vpk";
		for (const std::string& entry : vpk.ListAll())
			if (Lower(entry) == want) return true;
	} catch (const std::exception&) {
	}
	return false;
}

std::string WorkshopItemFor(const std::string& map) {
	const std::string want = "maps/" + Lower(map) + ".vpk";
	{
		std::lock_guard<std::mutex> lock(g_itemMx);
		auto it = g_itemFor.find(want);
		if (it != g_itemFor.end()) return it->second;
	}
	std::string found;
	const fs::path root = MapData::ResolveCs2Root();
	std::error_code ec;
	if (!root.empty())
		for (const auto& item : fs::directory_iterator(root.parent_path().parent_path() / "workshop" / "content" / "730", ec)) {
			if (!item.is_directory(ec)) continue;
			const fs::path package = ItemPackage(item.path());
			if (package.empty()) continue;
			try {
				MapData::Vpk vpk(package);
				for (const std::string& entry : vpk.ListAll())
					if (Lower(entry) == want) { found = item.path().filename().u8string(); break; }
			} catch (const std::exception&) {
			}
			if (!found.empty()) break;
		}
	std::lock_guard<std::mutex> lock(g_itemMx);
	g_itemFor[want] = found;
	return found;
}

// Every map CS2 can load here: the game's own (minus menu and preview scenes) and every map inside
// an installed Workshop item. Placeholder "maps" (a box that tells you to join a server) are kept
// but flagged.
std::vector<MapInfo> ScanMaps() {
	BackgroundPriority background;
	std::vector<MapInfo> official, workshop;
	const fs::path root = MapData::ResolveCs2Root();
	if (root.empty()) return {};
	std::error_code ec;
	for (const auto& e : fs::directory_iterator(root / "game" / "csgo" / "maps", ec)) {
		if (!e.is_regular_file(ec)) continue;
		const std::string file = Lower(e.path().filename().u8string());
		if (file.size() < 5 || file.compare(file.size() - 4, 4, ".vpk") != 0) continue;
		const std::string map = file.substr(0, file.size() - 4);
		auto ends = [&](const char* s) { const size_t n = strlen(s); return map.size() > n && map.compare(map.size() - n, n, s) == 0; };
		if (ends("_vanity") || map.rfind("workshop_preview_", 0) == 0 || map.rfind("lobby_", 0) == 0 ||
			map == "graphics_settings" || !MapData::ValidMapName(map))
			continue;
		official.push_back({ map, Pretty(map), "", "", false, false });
	}
	for (const auto& item : fs::directory_iterator(root.parent_path().parent_path() / "workshop" / "content" / "730", ec)) {
		if (!item.is_directory(ec)) continue;
		const fs::path package = ItemPackage(item.path());
		if (package.empty()) continue;
		try {
			MapData::Vpk vpk(package);
			const std::string id = item.path().filename().u8string(), title = PublishTitle(item.path());
			for (const std::string& entry : vpk.ListAll()) {
				if (entry.rfind("maps/", 0) != 0 || entry.size() < 10 || entry.compare(entry.size() - 4, 4, ".vpk") != 0) continue;
				const std::string map = entry.substr(5, entry.size() - 9);
				if (map.find('/') != std::string::npos || !MapData::ValidMapName(map)) continue;
				workshop.push_back({ map, title.empty() ? Pretty(map) : title, id, "", true, vpk.Size(entry) < kTinyMap });
			}
		} catch (const std::exception& e) {
			MvmDebugLog_LinefAlways("altenv", "workshop item %s unreadable: %s", item.path().u8string().c_str(), e.what());
		}
	}
	auto byTitle = [](const MapInfo& a, const MapInfo& b) { return Lower(a.title) != Lower(b.title) ? Lower(a.title) < Lower(b.title) : a.map < b.map; };
	// A map in a Workshop item is listed from the item, never from a bare copy of its package in
	// csgo/maps: only the item carries the map's own materials (WorkshopItemFor).
	official.erase(std::remove_if(official.begin(), official.end(), [&](const MapInfo& m) {
		return std::any_of(workshop.begin(), workshop.end(), [&](const MapInfo& w) { return w.map == m.map; });
	}), official.end());
	std::sort(official.begin(), official.end(), byTitle);
	std::sort(workshop.begin(), workshop.end(), byTitle);
	official.insert(official.end(), workshop.begin(), workshop.end());
	{
		std::lock_guard<std::mutex> lock(g_itemMx);
		g_itemFor.clear(); // a rescan sees newly installed items
	}
	return official;
}

// The replacement's Workshop addon (the item the engine mounts) and 3D skybox world, from its own
// package and entities -- never guessed from its name.
// info.addon comes in as the item picked in the list, if any: two items can hold a map of the same name
// (Nightly Cache and Rainy Cache are both de_cache_n_r), and the one picked is the one mounted.
bool ResolveMap(MapInfo& info, std::string& error) {
	try {
		const std::string wish = info.addon;
		const bool wished = !wish.empty() && WorkshopItemHas(wish, info.map);
		// The picked item is the one mounted, so its own package is read, not a same-named one.
		MapData::MapSources sources = MapData::OpenMapSources(MapData::ResolveCs2Root(), info.map, wished ? wish : std::string());
		info.addon.clear();
		if (wished) info.addon = wish;
		else if (sources.addon) info.addon = sources.addon->Path().parent_path().filename().u8string();
		// Found as a bare package in csgo/maps: the Workshop item it came from carries its materials.
		else info.addon = WorkshopItemFor(info.map);
		info.workshop = !info.addon.empty();
		info.skybox.clear();
		for (const MapData::MapEntity& e : MapData::ReadMapEntities(*sources.package)) {
			if (e.String("classname") != "skybox_reference") continue;
			std::string target = e.String("targetmapname");
			for (char& c : target) if (c == '\\') c = '/';
			if (target.size() > 5 && target.compare(target.size() - 5, 5, ".vmap") == 0) target.resize(target.size() - 5);
			if (target.rfind("maps/", 0) == 0) info.skybox = target;
			break;
		}
		return true;
	} catch (const std::exception& e) {
		error = e.what();
		return false;
	}
}

// ---- pictures -----------------------------------------------------------------------------------

struct Picture {
	std::string map, error;
	std::string wish;         // the Workshop item it was asked for (ResolveMap)
	std::string resolveError; // the replacement map's package could not be read: it cannot be played
	MapInfo info;             // for the replacement map: addon and skybox
	FloorRaster align, view;  // AUTO ALIGN's raster and the picture's
};

std::shared_ptr<Picture> BuildPicture(const std::string& map, const std::string& wish, bool resolve, const std::atomic<bool>* stop,
		std::function<void(const std::string&)> progress) {
	BackgroundPriority background;
	auto p = std::make_shared<Picture>();
	p->map = map;
	p->wish = wish;
	p->info.map = map;
	p->info.addon = wish;
	const double t0 = NowMs();
	if (resolve && !ResolveMap(p->info, p->resolveError)) {
		p->error = p->resolveError;
		return p;
	}
	Environment::OcclusionMesh mesh;
	// The replacement's floor comes from the item the copy mounts (ResolveMap), so the map view and
	// AUTO ALIGN never measure a same-named map from another item.
	if (!Environment::ReadMapCollision(map, Environment::MapDataFolder(), mesh, p->error, stop, progress,
			resolve ? p->info.addon : std::string()))
		return p;
	static_assert(sizeof(Environment::Vec3) == 12, "positions are read as float triples");
	const float* xyz = reinterpret_cast<const float*>(mesh.positions.data());
	p->align = BuildFloorRaster(xyz, mesh.positions.size(), mesh.indices.data(), mesh.indices.size(), kAlignCell);
	const double extent = std::max(double(mesh.high.x - mesh.low.x), double(mesh.high.y - mesh.low.y));
	p->view = BuildFloorRaster(xyz, mesh.positions.size(), mesh.indices.data(), mesh.indices.size(), CellFor(extent, kPictureMax, kPictureCell));
	if (!p->align.Valid() || !p->view.Valid()) p->error = "the map has no floor to draw";
	MvmDebugLog_LinefAlways("altenv", "picture map=%s triangles=%zu align=%dx%d view=%dx%d cell=%.0f floor=%zu addon=%s skybox=%s ms=%.0f%s%s",
		map.c_str(), mesh.indices.size() / 3, p->align.width, p->align.height, p->view.width, p->view.height, p->view.cell,
		p->align.FloorCount(), p->info.addon.c_str(), p->info.skybox.c_str(), NowMs() - t0, p->error.empty() ? "" : " error=", p->error.c_str());
	return p;
}

// The replacement map as the backdrop: floors from dark (low) to light (high), white walls.
std::string MapPng(const Picture& p) {
	const auto px = ShadeFloor(p.view, { 44, 50, 58, 255 }, { 196, 203, 212, 255 }, { 238, 242, 246, 255 });
	return EncodePngRgba(p.view.width, p.view.height, px.data());
}
// The demo's own map as the layer that moves: only its walls (orange) and where the players walked
// (translucent orange), so the replacement map shows through its floors.
// The map view's demo layer. With a replacement map picked it is coloured by where everything lands
// under the alignment being edited (LandingPicture: green on its floor, yellow floating over it,
// purple under it, red outside it); without one, the demo map's walls and walked paths in orange.
struct DemoLayer {
	std::string png;
	bool landed = false; // coloured against a replacement map
	Landings landings;   // the walked feet, by where they land
	double ms = 0;
};

DemoLayer DemoPng(const Picture& p, const std::vector<Vec3>& feet, const std::shared_ptr<const Picture>& target, const Alignment& a) {
	const double t0 = NowMs();
	DemoLayer layer;
	std::vector<uint8_t> px;
	if (target && target->error.empty() && target->align.Valid()) {
		px = LandingPicture(p.view, target->align, feet, a, &layer.landings);
		layer.landed = true;
	} else {
		px = ShadeFloor(p.view, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 255, 152, 64, 255 });
		StampPoints(px, p.view, feet, { 255, 152, 64, 110 }, 0, true);
	}
	layer.png = EncodePngRgba(p.view.width, p.view.height, px.data());
	layer.ms = NowMs() - t0;
	return layer;
}

// Served images, read by the server thread.
std::mutex g_imageMx;
std::map<std::string, std::string> g_images; // "demo.png" / "map.png" -> PNG bytes
std::atomic<unsigned> g_imageVersion{ 0 };

void PublishImage(const std::string& name, std::string png) {
	{
		std::lock_guard<std::mutex> lock(g_imageMx);
		g_images[name] = std::move(png);
	}
	++g_imageVersion;
	static std::once_flag registered;
	std::call_once(registered, [] {
		LocalImageServer::Register("altenv", [](const std::string& name, std::string& body, std::string& type) {
			std::lock_guard<std::mutex> lock(g_imageMx);
			auto it = g_images.find(name);
			if (it == g_images.end()) return false;
			body = it->second;
			type = "image/png";
			return true;
		});
	});
}

// ---- settings: the project file's "environment" section ---------------------------------------

struct Settings {
	bool enabled = false;
	MapInfo map;
	Alignment align;
	bool props = false; // the demo map's own props shown on the new map (AltEnvironmentProps.h)
};

std::string SettingsJson(const Settings& s) {
	JsonBuilder j;
	j.BeginObject();
	j.IntField("version", 1);
	j.BoolField("enabled", s.enabled);
	j.StringField("map", s.map.map);
	j.StringField("title", s.map.title);
	j.StringField("addon", s.map.addon);
	j.StringField("skybox", s.map.skybox);
	j.DoubleField("x", s.align.x);
	j.DoubleField("y", s.align.y);
	j.DoubleField("z", s.align.z);
	j.DoubleField("yaw", s.align.yaw);
	j.BoolField("props", s.props);
	j.EndObject();
	return j.Str();
}

Settings LoadSettings(const std::wstring& original) {
	Settings s;
	std::string text;
	JsonValue v;
	if (original.empty() || !DemoProjectStore_ReadSection(ProjectGame::Cs2, original, ProjectSection::Environment, text) || !JsonParse(text, v))
		return s;
	auto str = [&](const char* k) { const JsonValue* f = v.Find(k); return f ? f->AsString() : std::string(); };
	auto num = [&](const char* k) { const JsonValue* f = v.Find(k); const double d = f ? f->AsNumber() : 0; return std::isfinite(d) ? d : 0; };
	const JsonValue* on = v.Find("enabled");
	s.enabled = on && on->AsBool();
	s.map.map = str("map");
	s.map.title = str("title");
	s.map.addon = str("addon");
	s.map.skybox = str("skybox");
	s.map.workshop = !s.map.addon.empty();
	s.align = { num("x"), num("y"), num("z"), NormalizeYaw(num("yaw")) };
	// Saved before the map's Workshop item was looked for (WorkshopItemFor): name it now, so the copy
	// mounts the item and the map draws with its own materials.
	if (s.enabled && s.map.addon.empty() && MapData::ValidMapName(s.map.map)) {
		s.map.addon = WorkshopItemFor(s.map.map);
		s.map.workshop = !s.map.addon.empty();
	}
	const JsonValue* props = v.Find("props");
	s.props = props && props->AsBool();
	if (!MapData::ValidMapName(s.map.map)) s.enabled = false;
	return s;
}

void SaveSettings(const std::wstring& original, const Settings& s) {
	if (original.empty()) return;
	DemoProjectStore_EnsureProjectFile(ProjectGame::Cs2, original);
	DemoProjectStore_WriteSection(ProjectGame::Cs2, original, ProjectSection::Environment, s.map.map.empty() ? std::string() : SettingsJson(s));
}

// ---- the copy -----------------------------------------------------------------------------------

// What DemoWorldSwap rewrites: bump it whenever it rewrites more, so copies written before are
// rebuilt. 2: the level name (svc_ClearAllStringTables) and the session config's map.
constexpr int kCopyFormat = 2;

// Everything the copy depends on: change any of it and the copy is rebuilt.
std::string SpecKey(const Settings& s) {
	return "f" + std::to_string(kCopyFormat) + "|" + s.map.map + "|" + s.map.addon + "|" + s.map.skybox + "|" + Fixed(s.align.x) +
		"|" + Fixed(s.align.y) + "|" + Fixed(s.align.z) + "|" + Fixed(NormalizeYaw(s.align.yaw));
}

// ---- state (main thread) ------------------------------------------------------------------------

struct BuildResult {
	bool ok = false;
	std::string error;
	std::wstring copy;
	DemoWorldSwapResult swap;
	double ms = 0;
};
struct Build {
	std::future<BuildResult> job;
	std::wstring original, source;
	Settings settings;
	std::atomic<double> progress{ 0 };
	std::atomic<bool> cancel{ false };
	bool playWhenDone = false; // started by ResolvePlayable: nothing plays this demo yet
};

struct PictureJob {
	std::string map, wish;
	std::future<std::shared_ptr<Picture>> job;
	std::atomic<bool> cancel{ false };
	std::mutex statusMx;
	std::string status;
};

struct State {
	// superseded picture jobs, kept until they finish (a std::async future blocks when destroyed)
	std::vector<std::unique_ptr<PictureJob>> retired;
	bool copyPlaying = false; // the engine plays an environment copy (refreshed a few times a second)
	int copyCheck = 0;
	// the open demo
	std::wstring original;
	int closedFrames = 0; // frames with no demo playing, outside a copy switch
	std::string demoMap;
	Settings settings;      // as saved in its project
	// what the Config card is editing
	MapInfo pick;           // the replacement being aligned ("" = the demo's own map)
	Alignment draft;
	bool view = false;
	// catalog
	std::vector<MapInfo> maps;
	std::future<std::vector<MapInfo>> mapsJob;
	// pictures and the players' paths
	std::shared_ptr<const Picture> demoPic, pickPic;
	std::unique_ptr<PictureJob> demoJob, pickJob;
	std::vector<Vec3> feet;
	std::future<std::vector<Vec3>> feetJob;
	std::atomic<bool> feetCancel{ false };
	std::string feetStatus;
	bool feetTried = false;
	unsigned demoImage = 0, pickImage = 0; // image versions published
	std::future<DemoLayer> demoPngJob;
	// What the demo layer was last drawn from (it follows the alignment while the view is open).
	std::shared_ptr<const Picture> layerDemo;
	size_t layerFeet = 0;
	std::string layerPick;
	Alignment layerDraft;
	double layerAt = 0;
	bool haveLandings = false;
	Landings landings;
	// AUTO ALIGN
	std::future<AutoAlignResult> autoJob;
	std::atomic<double> autoProgress{ 0 };
	double autoStarted = 0;
	bool autoPending = false;
	// Counts demos opened (OpenDemo) and replacement maps picked (Pick). A job keeps the count it
	// started under, and a result from an earlier demo or map is dropped: its alignment belongs to
	// that pair of maps.
	uint64_t selection = 0;
	uint64_t autoJobSelection = 0;
	// The demo last asked to open (ResolvePlayable, OpenDemo). A copy written for a demo opened
	// earlier only plays if it is still this one.
	std::wstring lastOpen;
	Fit fit;
	bool haveFit = false;
	double fitAt = 0;
	bool fitDirty = true;
	std::future<Fit> fitJob;
	const Picture* fitDemo = nullptr; // the pictures the running fit job scores
	const Picture* fitPick = nullptr;
	// copy
	std::unique_ptr<Build> build;
	bool applyPending = false;   // APPLY waits for the replacement map's addon and skybox
	bool restorePending = false; // a demo opened on its own map moves to its saved one
	std::string status, error;
	// live players, demo coordinates
	std::vector<std::array<int, 3>> players;
	int playerFrame = 0;
	// verification probe
	double probeAt = 0;
	std::string awaitLevel; // after a switch: the world it should load (checked once it lands)
	std::string propsLevel; // the level the hidden props belong to (AltEnvironmentProps.h)
};
State& S() {
	static State s;
	return s;
}

bool DemoOpen() {
	if (!g_pEngineToClient) return false;
	auto* demo = g_pEngineToClient->GetDemoFile();
	return demo && (demo->IsPlayingDemo() || demo->IsDemoPaused());
}

std::string LevelName() {
	const char* p = g_pEngineToClient ? g_pEngineToClient->GetLevelNameShort() : nullptr;
	return p ? p : "";
}

void SetStatus(const std::string& status, const std::string& error = std::string()) {
	State& s = S();
	s.status = status;
	s.error = error;
}

// Reopens the open demo from `file` on the same tick; `level` is the world it should then load.
void SwitchTo(const std::wstring& original, const std::wstring& file, const std::string& level, const std::string& status) {
	SwitchPlayingCopy(original, file, false);
	S().awaitLevel = level;
	SetStatus(status);
}

// ---- jobs ---------------------------------------------------------------------------------------

void StartPicture(std::unique_ptr<PictureJob>& slot, const std::string& map, bool resolve, const std::string& wish = std::string()) {
	if (slot && slot->map == map && slot->wish == wish) return;
	if (slot) {
		slot->cancel = true; // it finishes on its own; its result is dropped
		S().retired.push_back(std::move(slot));
	}
	auto job = std::make_unique<PictureJob>();
	job->map = map;
	job->wish = wish;
	PictureJob* raw = job.get();
	job->job = std::async(std::launch::async, [map, wish, resolve, raw] {
		return BuildPicture(map, wish, resolve, &raw->cancel, [raw](const std::string& text) {
			std::lock_guard<std::mutex> lock(raw->statusMx);
			raw->status = text;
		});
	});
	slot = std::move(job);
}

void StartFeet() {
	State& s = S();
	if (s.feetTried || s.original.empty() || s.feetJob.valid()) return;
	s.feetTried = true;
	s.feetStatus = "Reading where the players walked...";
	const std::wstring demo = s.original;
	s.feetCancel = false;
	s.feetJob = std::async(std::launch::async, [demo] {
		BackgroundPriority background;
		std::vector<Vec3> feet;
		std::string error;
		const std::string bytes = ReadRoundPathsViaHelper(demo, error, &S().feetCancel);
		Heatmap::DemoPaths paths;
		if (bytes.empty() || !Heatmap::LooksLikePaths(bytes.data(), bytes.size()) || !Heatmap::ParsePaths(bytes.data(), bytes.size(), paths, error)) {
			MvmDebugLog_LinefAlways("altenv", "paths unavailable demo=%s error=%s", WideToUtf8(demo).c_str(), error.c_str());
			return feet;
		}
		size_t total = 0;
		for (const auto& round : paths.rounds)
			for (const auto& player : round.players)
				for (const auto& sample : player.samples) total += (sample.flags & Heatmap::kOnGround) ? 1 : 0;
		const size_t every = total > kPictureFeet ? (total + kPictureFeet - 1) / kPictureFeet : 1;
		size_t k = 0;
		for (const auto& round : paths.rounds)
			for (const auto& player : round.players)
				for (const auto& sample : player.samples)
					if ((sample.flags & Heatmap::kOnGround) && (k++ % every) == 0) feet.push_back({ sample.x, sample.y, sample.z });
		MvmDebugLog_LinefAlways("altenv", "paths demo=%s rounds=%zu ground=%zu kept=%zu", WideToUtf8(demo).c_str(), paths.rounds.size(), total, feet.size());
		return feet;
	});
}

void StartBuild(const std::wstring& original, const std::wstring& source, const Settings& settings, bool playWhenDone) {
	State& s = S();
	if (s.build && s.build->job.valid()) {
		if (Copies::SameFile(s.build->original, original) && Copies::SameFile(s.build->source, source) && SpecKey(s.build->settings) == SpecKey(settings)) {
			s.build->playWhenDone = s.build->playWhenDone || playWhenDone;
			return;
		}
		s.build->cancel = true;
		s.build->job.wait();
	}
	// Write the slot the engine is not playing.
	const std::wstring copy = Copies::SlotPath(original, Copies::FreeSlot(original), true);
	if (copy.empty()) {
		SetStatus("", "The demo's project folder cannot be written.");
		return;
	}
	auto b = std::make_unique<Build>();
	b->original = original;
	b->source = source;
	b->settings = settings;
	b->playWhenDone = playWhenDone;
	Build* raw = b.get();
	DemoWorldSwap spec;
	spec.sourceMap = ReadDemoHeader(original).map;
	spec.map = settings.map.map;
	spec.addon = settings.map.addon;
	spec.skybox = settings.map.skybox;
	spec.offset = WorldOffsetFor(settings.align);
	MvmDebugLog_LinefAlways("altenv", "build start demo=%s source=%s copy=%s from=%s to=%s addon=%s skybox=%s align=(%.2f %.2f %.2f yaw %.2f) worldoffset=(%.2f %.2f %.2f yaw %.2f)",
		WideToUtf8(original).c_str(), WideToUtf8(source).c_str(), WideToUtf8(copy).c_str(), spec.sourceMap.c_str(), spec.map.c_str(),
		spec.addon.c_str(), spec.skybox.c_str(), settings.align.x, settings.align.y, settings.align.z, settings.align.yaw,
		spec.offset.pos.x, spec.offset.pos.y, spec.offset.pos.z, spec.offset.yaw);
	b->job = std::async(std::launch::async, [raw, spec, copy] {
		BackgroundPriority background;
		BuildResult r;
		r.copy = copy;
		const double t0 = NowMs();
		r.swap = SwapDemoWorldFile(raw->source, copy, spec, &raw->cancel, [raw](double p) { raw->progress = p; });
		r.ms = NowMs() - t0;
		r.ok = r.swap.ok;
		r.error = r.swap.error;
		if (r.ok) Copies::WriteMarker(copy, raw->source, raw->original, SpecKey(raw->settings));
		return r;
	});
	s.build = std::move(b);
	SetStatus("Writing the demo for " + settings.map.title + "...");
}

void FinishBuild() {
	State& s = S();
	if (!s.build || !Ready(s.build->job)) return;
	std::unique_ptr<Build> b = std::move(s.build);
	const BuildResult r = b->job.get();
	MvmDebugLog_LinefAlways("altenv", "build %s demo=%s copy=%s headers=%d serverinfo=%d levelnames=%d worlds=%d skyboxes=%d frames=%llu bytes=%llu->%llu ms=%.0f%s%s",
		r.ok ? "done" : "failed", WideToUtf8(b->original).c_str(), WideToUtf8(r.copy).c_str(), r.swap.headers, r.swap.serverInfos, r.swap.levelNames,
		r.swap.worlds, r.swap.skyboxes, (unsigned long long)r.swap.frames, (unsigned long long)r.swap.bytesIn, (unsigned long long)r.swap.bytesOut,
		r.ms, r.ok ? "" : " error=", r.error.c_str());
	if (b->cancel) return;
	// A copy started by opening a demo plays when it is done, unless another demo was opened since.
	const bool playNow = b->playWhenDone && Copies::SameFile(s.lastOpen, b->original);
	if (b->playWhenDone && !playNow)
		MvmDebugLog_LinefAlways("altenv", "build for %s done after %s was opened: not played", WideToUtf8(b->original).c_str(),
			WideToUtf8(s.lastOpen).c_str());
	if (!r.ok) {
		SetStatus("", "The demo could not be rewritten: " + r.error);
		advancedfx::Warning("mirv_filmmaker altenv: %s\n", r.error.c_str());
		// Never leave a demo the user asked to watch unplayed.
		if (playNow) PlayResolvedDemo(b->original, b->source);
		return;
	}
	advancedfx::Message("mirv_filmmaker altenv: %s plays on %s.\n", WideToUtf8(b->original).c_str(), b->settings.map.map.c_str());
	// The demo is open: reload it from the copy on the same tick. Otherwise this build was started
	// by opening the demo, and that open goes ahead now.
	const std::string loading = "Loading " + b->settings.map.title + "...";
	if (DemoOpen() && Copies::SameFile(PlayingDemoPath(), b->original)) {
		SwitchTo(b->original, r.copy, b->settings.map.map, loading);
	} else if (playNow) {
		PlayResolvedDemo(b->original, r.copy);
		s.awaitLevel = b->settings.map.map;
		SetStatus(loading);
	} else {
		SetStatus("");
	}
}

void StartAutoAlign() {
	State& s = S();
	s.autoPending = false;
	if (s.autoJob.valid()) {
		// Still finishing a job for an earlier demo or map: this one starts once that is dropped.
		if (s.autoJobSelection != s.selection) s.autoPending = true;
		return;
	}
	if (!s.demoPic || !s.pickPic || !s.demoPic->error.empty() || !s.pickPic->error.empty()) {
		s.autoPending = true; // runs when both pictures are in
		return;
	}
	auto demo = s.demoPic, pick = s.pickPic;
	auto feet = s.feet;
	s.autoProgress = 0;
	s.autoStarted = NowMs();
	s.autoJobSelection = s.selection;
	s.autoJob = std::async(std::launch::async, [demo, pick, feet] {
		BackgroundPriority background;
		AutoAlignOptions options;
		// The search spreads over several cores; each worker stays below the game's threads.
		options.workerInit = [] { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); };
		return AutoAlign(demo->align, pick->align, feet, options, [](double p) { S().autoProgress = p; });
	});
	SetStatus("Matching " + s.demoMap + " to " + s.pick.title + "...");
}

// ---- the open demo ------------------------------------------------------------------------------

void OpenDemo(const std::wstring& original) {
	State& s = S();
	if (s.feetJob.valid()) { s.feetCancel = true; s.feetJob.wait(); s.feetJob = {}; }
	s.original = original;
	s.lastOpen = original;
	++s.selection;   // a running AUTO ALIGN is for the demo before
	s.autoPending = false;
	s.demoMap = original.empty() ? std::string() : ReadDemoHeader(original).map;
	s.settings = LoadSettings(original);
	s.pick = s.settings.map;
	s.draft = s.settings.align;
	s.feet.clear();
	s.feetTried = false;
	s.feetStatus.clear();
	s.demoPic.reset();
	s.pickPic.reset();
	s.layerDemo.reset();
	s.haveLandings = false;
	s.haveFit = false;
	s.fitDirty = true;
	s.applyPending = false;
	s.restorePending = s.settings.enabled;
	s.awaitLevel.clear();
	SetStatus("");
	MvmDebugLog_LinefAlways("altenv", "demo=%s map=%s saved=%s enabled=%d align=(%.2f %.2f %.2f yaw %.2f)", WideToUtf8(original).c_str(),
		s.demoMap.c_str(), s.settings.map.map.c_str(), s.settings.enabled ? 1 : 0, s.settings.align.x, s.settings.align.y, s.settings.align.z,
		s.settings.align.yaw);
}

// A map by name and, when given, the Workshop item it is in (two items can hold the same name).
MapInfo FindMap(const std::string& name, const std::string& addon) {
	const std::string want = Lower(name);
	for (const MapInfo& m : S().maps)
		if (Lower(m.map) == want && (addon.empty() || m.addon == addon)) return m;
	for (const MapInfo& m : S().maps)
		if (Lower(m.map) == want || (!m.addon.empty() && m.addon == name)) return m;
	MapInfo m;
	m.map = name;
	m.addon = addon;
	m.title = Pretty(name);
	return m;
}

// The pick's picture is the one for its map and Workshop item.
bool PictureFor(const std::shared_ptr<const Picture>& p, const MapInfo& pick) {
	return p && p->map == pick.map && (p->wish == pick.addon || p->info.addon == pick.addon);
}

// A new selection voids what was asked for the old one: a PLAY waiting for its picture, an AUTO
// ALIGN waiting or running. They belonged to the previous map.
void SelectionChanged() {
	State& s = S();
	++s.selection;
	s.applyPending = false;
	s.autoPending = false;
}

void Pick(const std::string& name, const std::string& addon) {
	State& s = S();
	if (name.empty() || Lower(name) == "none" || Lower(name) == Lower(s.demoMap)) {
		if (!s.pick.map.empty()) SelectionChanged();
		s.pick = MapInfo{};
		s.pickPic.reset();
		s.pickJob.reset();
		s.fitDirty = true;
		return;
	}
	MapInfo m = FindMap(name, addon);
	if (!MapData::ValidMapName(m.map)) {
		SetStatus("", "\"" + name + "\" is not a map name.");
		return;
	}
	if (m.map != s.pick.map || m.addon != s.pick.addon) {
		SelectionChanged();
		s.pick = m;
		s.pickPic.reset();
		s.haveFit = false;
		s.fitDirty = true;
	}
	StartPicture(s.pickJob, s.pick.map, true, s.pick.addon);
	MvmDebugLog_LinefAlways("altenv", "pick map=%s title=%s addon=%s", s.pick.map.c_str(), s.pick.title.c_str(), s.pick.addon.c_str());
}

void Apply() {
	State& s = S();
	if (s.original.empty() || !DemoOpen()) { SetStatus("", "Open a demo first."); return; }
	if (s.pick.map.empty()) { SetStatus("", "Choose a map first."); return; }
	if (s.demoMap.empty()) { SetStatus("", "This demo does not say which map it was recorded on."); return; }
	// The copy needs the replacement's addon and skybox, which its picture job reads.
	if (!PictureFor(s.pickPic, s.pick)) {
		StartPicture(s.pickJob, s.pick.map, true, s.pick.addon);
		s.applyPending = true;
		SetStatus("Reading " + s.pick.title + "...");
		return;
	}
	s.applyPending = false;
	if (!s.pickPic->resolveError.empty()) {
		SetStatus("", s.pick.title + " cannot be read: " + s.pickPic->resolveError);
		return;
	}
	// A demo started from the console has not been through the repair check yet: the copy waits
	// for its verdict (this runs again every frame while applyPending is set).
	const std::wstring base = Copies::BaseFor(s.original);
	if (base.empty()) {
		s.applyPending = true;
		SetStatus("Checking the demo before it plays on " + s.pick.title + "...");
		return;
	}
	Settings next;
	next.enabled = true;
	next.map = s.pick;
	next.map.addon = s.pickPic->info.addon;
	next.map.skybox = s.pickPic->info.skybox;
	next.map.workshop = !next.map.addon.empty();
	next.align = s.draft;
	next.align.yaw = NormalizeYaw(next.align.yaw);
	next.props = s.settings.props;
	s.settings = next;
	SaveSettings(s.original, next);
	const int slot = Copies::CurrentSlot(s.original, base, SpecKey(next));
	MvmDebugLog_LinefAlways("altenv", "apply demo=%s map=%s addon=%s skybox=%s align=(%.2f %.2f %.2f yaw %.2f) base=%s current=%d",
		WideToUtf8(s.original).c_str(), next.map.map.c_str(), next.map.addon.c_str(), next.map.skybox.c_str(), next.align.x, next.align.y,
		next.align.z, next.align.yaw, WideToUtf8(base).c_str(), slot);
	if (slot >= 0) {
		const std::wstring copy = Copies::SlotPath(s.original, slot, false);
		if (!Copies::IsPlaying(copy)) SwitchTo(s.original, copy, next.map.map, "Loading " + next.map.title + "...");
		else SetStatus("");
		return;
	}
	StartBuild(s.original, base, next, false);
}

void Off() {
	State& s = S();
	if (s.build) { s.build->cancel = true; s.build->job.wait(); s.build.reset(); }
	s.settings.enabled = false;
	SaveSettings(s.original, s.settings);
	MvmDebugLog_LinefAlways("altenv", "off demo=%s playingCopy=%d", WideToUtf8(s.original).c_str(), Copies::PlayingCopy() ? 1 : 0);
	if (DemoOpen() && Copies::PlayingCopy()) {
		const std::wstring base = Copies::BaseFor(s.original);
		if (!base.empty()) {
			SwitchTo(s.original, base, s.demoMap, "Back to " + Pretty(s.demoMap) + "...");
			return;
		}
	}
	SetStatus("");
}

// A demo started outside the demo list (playdemo, the automation launchers) opens on its own map.
// With its setting on, it moves to the saved map once the demo has been checked.
void Restore() {
	State& s = S();
	if (!DemoOpen() || !s.settings.enabled || Copies::PlayingCopy() || (s.build && s.build->job.valid())) {
		s.restorePending = false;
		return;
	}
	const std::wstring base = Copies::BaseFor(s.original);
	if (base.empty()) return; // the repair verdict is not in yet
	s.restorePending = false;
	const int slot = Copies::CurrentSlot(s.original, base, SpecKey(s.settings));
	MvmDebugLog_LinefAlways("altenv", "restore demo=%s map=%s base=%s current=%d", WideToUtf8(s.original).c_str(), s.settings.map.map.c_str(),
		WideToUtf8(base).c_str(), slot);
	if (slot >= 0) SwitchTo(s.original, Copies::SlotPath(s.original, slot, false), s.settings.map.map, "Loading " + s.settings.map.title + "...");
	else StartBuild(s.original, base, s.settings, false);
}

// ---- live players -------------------------------------------------------------------------------

// A living player's feet, in demo coordinates. A demo can hand us a half-built entity at any time.
bool ReadPlayer(int controllerIndex, float& x, float& y, int& team) {
	__try {
		CEntityInstance* controller = CosmeticEntFromIndex(controllerIndex);
		if (!controller || !controller->IsPlayerController()) return false;
		const auto handle = controller->GetPlayerPawnHandle();
		if (!handle.IsValid()) return false;
		CEntityInstance* pawn = CosmeticEntFromIndex(handle.GetEntryIndex());
		if (!pawn || !pawn->IsPlayerPawn() || pawn->GetHealth() == 0) return false;
		float z = 0;
		pawn->GetOrigin(x, y, z);
		team = pawn->GetTeam();
		return std::isfinite(x) && std::isfinite(y);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void ReadPlayers(std::vector<std::array<int, 3>>& out) {
	out.clear();
	for (int i = 1; i <= 64; ++i) {
		float x = 0, y = 0;
		int team = 0;
		if (ReadPlayer(i, x, y, team)) out.push_back({ int(std::lround(x)), int(std::lround(y)), team });
	}
}

// ---- verification probe -------------------------------------------------------------------------

// Once a second while a copy plays: which world the engine has, the tick, pause, the alignment.
void Probe() {
	State& s = S();
	if (!MvmDebugLog_Active()) return;
	const double now = NowMs();
	if (now - s.probeAt < 1000) return;
	s.probeAt = now;
	if (!DemoOpen()) return;
	int tick = -1;
	g_MirvTime.GetCurrentDemoTick(tick);
	auto* demo = g_pEngineToClient->GetDemoFile();
	const bool copy = s.copyPlaying;
	const std::string level = LevelName();
	MvmDebugLog_Linef("altenv.probe", "level=%s copy=%d tick=%d paused=%d enabled=%d map=%s align=(%.2f %.2f %.2f yaw %.2f) players=%zu", level.c_str(),
		copy ? 1 : 0, tick, demo && demo->IsDemoPaused() ? 1 : 0, s.settings.enabled ? 1 : 0, s.settings.map.map.c_str(), s.settings.align.x,
		s.settings.align.y, s.settings.align.z, s.settings.align.yaw, s.players.size());
}

// After a switch has landed: the world the engine loaded. Logged once; it ends the "Loading ..."
// status, or says what loaded instead.
void CheckLoaded() {
	State& s = S();
	if (s.awaitLevel.empty() || SwitchingPlayingCopy() || !DemoOpen()) return;
	const std::string level = LevelName();
	if (level.empty()) return;
	const bool ok = Lower(level) == Lower(s.awaitLevel);
	MvmDebugLog_LinefAlways("altenv", "world loaded level=%s expected=%s copy=%d %s", level.c_str(), s.awaitLevel.c_str(),
		Copies::PlayingCopy() ? 1 : 0, ok ? "ok" : "MISMATCH");
	if (ok) SetStatus("");
	else SetStatus("", "The demo loaded " + level + ", not " + s.awaitLevel + ".");
	s.awaitLevel.clear();
}

void Json(std::ostringstream& o, const char* key, const std::string& v) { o << ",\"" << key << "\":\"" << JsonBuilder::Escape(v) << "\""; }

void PictureJson(std::ostringstream& o, const char* key, const std::shared_ptr<const Picture>& p, const char* image, unsigned version) {
	if (!p || !p->error.empty() || !p->view.Valid() || !version) return;
	const std::string url = LocalImageServer::Url("altenv");
	if (url.empty()) return;
	o << ",\"" << key << "\":{\"url\":\"" << JsonBuilder::Escape(url + image + "?v=" + std::to_string(version)) << "\""
	  << ",\"x0\":" << p->view.minX << ",\"y0\":" << p->view.minY << ",\"w\":" << p->view.width * p->view.cell
	  << ",\"h\":" << p->view.height * p->view.cell << ",\"pw\":" << p->view.width << ",\"ph\":" << p->view.height << "}";
}

} // namespace

// ---- public -------------------------------------------------------------------------------------

void RunMainThreadFrame() {
	State& s = S();
	// The demo the engine plays (its original, even while the copy plays).
	const std::wstring original = DemoOpen() ? PlayingDemoPath() : std::wstring();
	if (!original.empty() && !Copies::SameFile(original, s.original)) OpenDemo(original);
	// Out of the demo for a couple of seconds (a map was loaded, or the main menu): opening the same
	// demo again is a new open, so it goes back to its saved map. Copy switches reload the demo and
	// are left alone.
	if (original.empty() && !s.original.empty() && !SwitchingPlayingCopy()) {
		if (++s.closedFrames >= 120) {
			MvmDebugLog_LinefAlways("altenv", "demo closed demo=%s level=%s", WideToUtf8(s.original).c_str(), LevelName().c_str());
			s.original.clear();
			s.closedFrames = 0;
		}
	} else {
		s.closedFrames = 0;
	}
	if (++s.copyCheck >= 15) {
		s.copyCheck = 0;
		s.copyPlaying = !original.empty() && Copies::PlayingCopy();
	}
	// The demo map's own props on the new map: hidden unless shown (AltEnvironmentProps.h). A new
	// level drops the list first; its entities went with the old one.
	{
		const std::string level = DemoOpen() ? LevelName() : std::string();
		if (level != s.propsLevel) { Props::Forget(); s.propsLevel = level; }
		const bool onOtherMap = s.copyPlaying && s.settings.enabled && !level.empty() && Lower(level) == Lower(s.settings.map.map)
			&& !SwitchingPlayingCopy();
		Props::Update(onOtherMap, s.settings.props, s.demoMap);
	}
	for (size_t i = 0; i < s.retired.size();) {
		if (Ready(s.retired[i]->job)) s.retired.erase(s.retired.begin() + long(i));
		else ++i;
	}

	// The map view is part of CONFIG: closing the panel closes it.
	if (s.view && !ConfigHud_Enabled()) s.view = false;
	if (Ready(s.mapsJob)) {
		s.maps = s.mapsJob.get();
		MvmDebugLog_LinefAlways("altenv", "maps scanned count=%zu", s.maps.size());
	}
	// CONFIG open on the ENVIRONMENT tab reads both maps and the players' paths ahead (background
	// priority), so the map view and AUTO ALIGN have them by the time the MAP card is opened.
	const bool ahead = DemoOpen() && ConfigHud_Enabled() && FilmmakerSettings_UiString("config.tab", "") == "environment";
	if (s.view || s.autoPending || s.applyPending || ahead) {
		if (s.maps.empty() && !s.mapsJob.valid()) s.mapsJob = std::async(std::launch::async, ScanMaps);
		if (!s.demoMap.empty() && (!s.demoPic || s.demoPic->map != s.demoMap)) StartPicture(s.demoJob, s.demoMap, false);
		if (!s.pick.map.empty() && !PictureFor(s.pickPic, s.pick)) StartPicture(s.pickJob, s.pick.map, true, s.pick.addon);
		StartFeet();
	}
	// A picture job whose map is the loaded level would extract the same cache as the live loader;
	// ReadMapCollision serialises them, so it only waits.
	if (s.demoJob && Ready(s.demoJob->job)) {
		auto p = s.demoJob->job.get();
		const bool current = !s.demoJob->cancel && p && p->map == s.demoMap;
		s.demoJob.reset();
		if (current) {
			s.demoPic = p;
			s.demoImage = 0;
			s.fitDirty = true;
		}
	}
	if (s.pickJob && Ready(s.pickJob->job)) {
		auto p = s.pickJob->job.get();
		const bool current = !s.pickJob->cancel && PictureFor(p, s.pick);
		s.pickJob.reset();
		if (current) {
			s.pickPic = p;
			s.pick.addon = p->info.addon;
			s.pick.skybox = p->info.skybox;
			s.pick.workshop = !p->info.addon.empty();
			if (p->error.empty()) PublishImage("map.png", MapPng(*p));
			s.pickImage = g_imageVersion.load();
			s.fitDirty = true;
			if (!p->error.empty()) SetStatus("", s.pick.title + ": " + p->error);
		}
	}
	if (Ready(s.feetJob)) {
		s.feet = s.feetJob.get();
		s.feetStatus = s.feet.empty() ? "The players' paths could not be read from this demo." : "";
		s.demoImage = 0;
		s.fitDirty = true;
	}
	// The demo layer: its walls now, the walked paths once they are in, and -- while the map view is
	// open with a replacement map -- coloured by where they land under the alignment being edited,
	// redrawn as it changes: one job at a time (~30 ms), at most every 50 ms. The view moves the last
	// drawing meanwhile, and swaps pictures only once the next one has loaded.
	{
		const bool target = PictureFor(s.pickPic, s.pick) && s.pickPic->error.empty();
		const std::string pickKey = target ? s.pick.map + "|" + s.pick.addon : std::string();
		// Compared with what the last job was given, not with the published image: walked paths that
		// arrive while the walls are being drawn still get drawn after them.
		const bool drawn = s.layerDemo == s.demoPic && s.layerFeet == s.feet.size();
		const bool stale = !drawn || (s.view && (pickKey != s.layerPick || (target && SpecKey({ true, s.pick, s.draft }) != SpecKey({ true, s.pick, s.layerDraft }))));
		const bool due = !drawn || NowMs() - s.layerAt >= 50;
		if (s.demoPic && s.demoPic->error.empty() && stale && due && !s.demoPngJob.valid()) {
			auto pic = s.demoPic;
			auto feet = s.feet;
			std::shared_ptr<const Picture> pick = target ? s.pickPic : nullptr;
			const Alignment draft = s.draft;
			s.layerDemo = pic;
			s.layerFeet = feet.size();
			s.layerPick = pickKey;
			s.layerDraft = draft;
			s.layerAt = NowMs();
			s.demoPngJob = std::async(std::launch::async, [pic, feet, pick, draft] {
				BackgroundPriority background;
				return DemoPng(*pic, feet, pick, draft);
			});
		}
	}
	if (Ready(s.demoPngJob)) {
		DemoLayer layer = s.demoPngJob.get();
		PublishImage("demo.png", std::move(layer.png));
		s.demoImage = g_imageVersion.load();
		s.haveLandings = layer.landed && !s.feet.empty();
		s.landings = layer.landings;
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("altenv.layer", "map=%s draft=(%.1f %.1f %.1f yaw %.1f) feet=%zu on=%zu floating=%zu under=%zu outside=%zu ms=%.0f",
				layer.landed ? s.layerPick.c_str() : "-", s.layerDraft.x, s.layerDraft.y, s.layerDraft.z, s.layerDraft.yaw, layer.landings.Total(),
				layer.landings.on, layer.landings.floating, layer.landings.under, layer.landings.outside, layer.ms);
	}
	if (s.applyPending && PictureFor(s.pickPic, s.pick)) Apply();
	if (s.restorePending) Restore();
	if (s.autoPending && s.demoPic && s.pickPic) StartAutoAlign();
	if (Ready(s.autoJob)) {
		const AutoAlignResult r = s.autoJob.get();
		const bool stale = s.autoJobSelection != s.selection;
		MvmDebugLog_LinefAlways("altenv", "auto align ok=%d identity=%d align=(%.2f %.2f %.2f yaw %.2f) floor=%.3f players=%.3f feet=%zu ms=%.0f%s",
			r.ok ? 1 : 0, r.identity ? 1 : 0, r.alignment.x, r.alignment.y, r.alignment.z, r.alignment.yaw, r.fit.floor, r.fit.players,
			s.feet.size(), NowMs() - s.autoStarted, stale ? " (dropped: the demo or the picked map changed)" : "");
		if (stale) {
			// Nothing of it reaches the demo open now.
		} else if (r.ok) {
			s.draft = r.alignment;
			s.fitDirty = true;
			SetStatus(r.identity ? "The two maps already line up." : "Matched. Check the layout, then press PLAY ON THIS MAP.");
		} else if (r.fit.floorCells) {
			// The search ran, but even its best place fits almost nothing: two different maps.
			SetStatus("", Pretty(s.demoMap) + " and " + s.pick.title + " do not share a layout (at best " + std::to_string(int(std::lround(r.fit.floor * 100)))
				+ "% of the floor lines up). Line them up by hand.");
		} else {
			SetStatus("", "The two maps could not be matched; line them up by hand.");
		}
	}
	// The fit of the alignment being edited, at most ten times a second, on a background thread: on
	// the main thread it cost 2-6 ms each time, a hitch every few frames while dragging.
	if (s.fitDirty && s.demoPic && s.pickPic && s.demoPic->error.empty() && s.pickPic->error.empty() && NowMs() - s.fitAt > 100
		&& !s.fitJob.valid()) {
		s.fitAt = NowMs();
		std::vector<Vec3> few;
		const size_t every = s.feet.size() > 3000 ? s.feet.size() / 3000 : 1;
		for (size_t k = 0; k < s.feet.size(); k += every) few.push_back(s.feet[k]);
		auto demo = s.demoPic, pick = s.pickPic;
		const Alignment draft = s.draft;
		s.fitDemo = demo.get();
		s.fitPick = pick.get();
		s.fitDirty = false;
		s.fitJob = std::async(std::launch::async, [demo, pick, few = std::move(few), draft] {
			BackgroundPriority background;
			return Evaluate(demo->align, pick->align, few, draft, 2);
		});
	}
	if (Ready(s.fitJob)) {
		const Fit fit = s.fitJob.get();
		// A demo or map changed while it ran: its fit is for pictures no longer shown.
		if (s.fitDemo == s.demoPic.get() && s.fitPick == s.pickPic.get()) {
			s.fit = fit;
			s.haveFit = true;
		}
	}
	FinishBuild();
	CheckLoaded();
	if (s.view && ++s.playerFrame >= 6) {
		s.playerFrame = 0;
		ReadPlayers(s.players);
	}
	if (!s.view) s.players.clear();
	Probe();
}

void AppendStateJson(std::ostringstream& o) {
	State& s = S();
	const bool copy = s.copyPlaying;
	o << ",\"altenv\":{\"demo\":" << (!s.original.empty() && DemoOpen() ? "true" : "false");
	Json(o, "demoMap", s.demoMap);
	Json(o, "demoTitle", Pretty(s.demoMap));
	Json(o, "pick", s.pick.map);
	Json(o, "pickTitle", s.pick.title);
	Json(o, "pickAddon", s.pick.addon);
	o << ",\"enabled\":" << (s.settings.enabled ? "true" : "false") << ",\"copy\":" << (copy ? "true" : "false");
	o << ",\"props\":" << (s.settings.props ? "true" : "false") << ",\"hiddenProps\":" << Props::HiddenCount();
	Json(o, "applied", s.settings.enabled ? s.settings.map.map : std::string());
	Json(o, "appliedTitle", s.settings.enabled ? s.settings.map.title : std::string());
	o << ",\"draft\":{\"x\":" << Fixed(s.draft.x) << ",\"y\":" << Fixed(s.draft.y) << ",\"z\":" << Fixed(s.draft.z) << ",\"yaw\":" << Fixed(s.draft.yaw) << "}";
	o << ",\"saved\":{\"x\":" << Fixed(s.settings.align.x) << ",\"y\":" << Fixed(s.settings.align.y) << ",\"z\":" << Fixed(s.settings.align.z)
	  << ",\"yaw\":" << Fixed(s.settings.align.yaw) << "}";
	const bool dirty = s.pick.map != (s.settings.enabled ? s.settings.map.map : std::string()) || SpecKey({ true, s.pick, s.draft }) != SpecKey({ true, s.pick, s.settings.align });
	o << ",\"dirty\":" << (dirty ? "true" : "false");
	o << ",\"view\":" << (s.view ? "true" : "false");
	const bool building = s.build && s.build->job.valid();
	o << ",\"building\":" << (building ? "true" : "false") << ",\"progress\":" << Fixed(building ? s.build->progress.load() : 0);
	o << ",\"auto\":" << (s.autoJob.valid() || s.autoPending ? "true" : "false") << ",\"autoProgress\":" << Fixed(s.autoProgress.load());
	Json(o, "status", s.status);
	Json(o, "error", s.error);
	std::string picStatus;
	auto jobStatus = [](const std::unique_ptr<PictureJob>& j) {
		if (!j) return std::string();
		std::lock_guard<std::mutex> lock(j->statusMx);
		return j->status.empty() ? std::string("Reading the map") : j->status;
	};
	if (s.demoJob) picStatus = Pretty(s.demoMap) + ": " + jobStatus(s.demoJob);
	else if (s.pickJob) picStatus = s.pick.title + ": " + jobStatus(s.pickJob);
	else if (s.demoPic && !s.demoPic->error.empty()) picStatus = Pretty(s.demoMap) + ": " + s.demoPic->error;
	Json(o, "picStatus", picStatus);
	Json(o, "feetStatus", s.feetStatus);
	o << ",\"feet\":" << s.feet.size();
	if (s.haveFit) o << ",\"fit\":{\"floor\":" << Fixed(s.fit.floor) << ",\"players\":" << Fixed(s.fit.players) << ",\"samples\":" << s.fit.playerSamples << "}";
	if (s.haveLandings && !s.pick.map.empty())
		o << ",\"landing\":{\"on\":" << Fixed(s.landings.Share(s.landings.on)) << ",\"floating\":" << Fixed(s.landings.Share(s.landings.floating))
		  << ",\"under\":" << Fixed(s.landings.Share(s.landings.under)) << ",\"outside\":" << Fixed(s.landings.Share(s.landings.outside)) << "}";
	PictureJson(o, "demoPic", s.demoPic, "demo.png", s.demoImage);
	PictureJson(o, "mapPic", s.pickPic, "map.png", s.pickImage);
	if (s.view) {
		o << ",\"players\":[";
		for (size_t i = 0; i < s.players.size(); ++i)
			o << (i ? "," : "") << "[" << s.players[i][0] << "," << s.players[i][1] << "," << s.players[i][2] << "]";
		o << "]";
	}
	o << ",\"maps\":[";
	for (size_t i = 0; i < s.maps.size(); ++i) {
		const MapInfo& m = s.maps[i];
		o << (i ? "," : "") << "{\"m\":\"" << JsonBuilder::Escape(m.map) << "\",\"t\":\"" << JsonBuilder::Escape(m.title) << "\"";
		if (!m.addon.empty()) o << ",\"a\":\"" << JsonBuilder::Escape(m.addon) << "\"";
		if (m.tiny) o << ",\"tiny\":true";
		o << "}";
	}
	o << "],\"mapsBusy\":" << (s.mapsJob.valid() ? "true" : "false") << "}";
}

std::string HealthLine() {
	State& s = S();
	char b[512];
	if (s.build && s.build->job.valid())
		snprintf(b, sizeof(b), "altenv: writing the copy for %s (%.0f%%)", s.build->settings.map.map.c_str(), s.build->progress.load() * 100);
	else if (s.settings.enabled)
		snprintf(b, sizeof(b), "altenv: on, %s (%s), level %s, copy %s, align (%.1f %.1f %.1f yaw %.1f)", s.settings.map.map.c_str(),
			s.settings.map.addon.empty() ? "installed" : s.settings.map.addon.c_str(), LevelName().c_str(), Copies::PlayingCopy() ? "playing" : "not playing",
			s.settings.align.x, s.settings.align.y, s.settings.align.z, s.settings.align.yaw);
	else
		snprintf(b, sizeof(b), "altenv: off%s%s", s.error.empty() ? "" : " -- ", s.error.c_str());
	return b;
}

std::wstring ResolvePlayable(const std::wstring& original, const std::wstring& playable) {
	if (original.empty() || playable.empty()) return playable;
	State& s = S();
	s.lastOpen = original;   // the open the user asked for last, copy or not
	const Settings settings = Copies::SameFile(original, s.original) ? s.settings : LoadSettings(original);
	if (!settings.enabled) return playable;
	const int slot = Copies::CurrentSlot(original, playable, SpecKey(settings));
	MvmDebugLog_LinefAlways("altenv", "resolve demo=%s base=%s map=%s slot=%d", WideToUtf8(original).c_str(), WideToUtf8(playable).c_str(),
		settings.map.map.c_str(), slot);
	if (slot >= 0) return Copies::SlotPath(original, slot, false);
	StartBuild(original, playable, settings, true);
	advancedfx::Message("mirv_filmmaker altenv: preparing %s on %s -- playback starts when it is written.\n",
		WideToUtf8(original).c_str(), settings.map.map.c_str());
	return std::wstring();
}

bool WorldMoved() {
	const State& s = S();
	return s.copyPlaying && s.settings.enabled && !IsIdentity(s.settings.align);
}

void KeepOnly(const std::wstring& original, const std::wstring& playingFile) {
	State& s = S();
	if (s.build && s.build->job.valid()) return; // a build may be writing a slot
	// This demo keeps the copy for its current settings (and the one playing).
	const Settings settings = Copies::SameFile(original, s.original) ? s.settings : LoadSettings(original);
	const std::wstring base = Copies::BaseFor(original);
	const int current = settings.enabled && !base.empty() ? Copies::CurrentSlot(original, base, SpecKey(settings)) : -1;
	Copies::KeepOnly(original, playingFile, current);
}

void RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	State& s = S();
	const char* op = argc > 2 ? args->ArgV(2) : "status";
	const bool quiet = argc > 3 && 0 == _stricmp(args->ArgV(argc - 1), "quiet");
	const int n = quiet ? argc - 1 : argc;
	auto num = [&](int i, double def) { return i < n ? std::atof(args->ArgV(i)) : def; };
	if (0 == _stricmp(op, "status")) {
		// printed below
	} else if (0 == _stricmp(op, "maps")) {
		if (n > 3 && 0 == _stricmp(args->ArgV(3), "rescan")) { s.maps.clear(); if (!s.mapsJob.valid()) s.mapsJob = std::async(std::launch::async, ScanMaps); }
		else if (s.maps.empty() && !s.mapsJob.valid()) s.mapsJob = std::async(std::launch::async, ScanMaps);
		if (s.mapsJob.valid()) { advancedfx::Message("%s altenv maps: scanning...\n", cmd); return; }
		for (const MapInfo& m : s.maps)
			advancedfx::Message("  %-28s %s%s%s%s\n", m.map.c_str(), m.title.c_str(), m.addon.empty() ? "" : "  workshop ", m.addon.c_str(), m.tiny ? "  (placeholder)" : "");
		advancedfx::Message("%zu maps\n", s.maps.size());
		return;
	} else if (0 == _stricmp(op, "pick")) {
		if (n < 4) { advancedfx::Warning("usage: %s altenv pick <map> [workshop item]|none\n", cmd); return; }
		Pick(args->ArgV(3), n > 4 ? args->ArgV(4) : "");
	} else if (0 == _stricmp(op, "align")) {
		if (n < 7) { advancedfx::Warning("usage: %s altenv align <x> <y> <z> <yaw>\n", cmd); return; }
		s.draft = { num(3, 0), num(4, 0), num(5, 0), NormalizeYaw(num(6, 0)) };
		s.fitDirty = true;
	} else if (0 == _stricmp(op, "move")) {
		s.draft.x += num(3, 0);
		s.draft.y += num(4, 0);
		s.draft.z += num(5, 0);
		s.fitDirty = true;
	} else if (0 == _stricmp(op, "turn")) {
		// About the middle of the demo map unless a pivot is given.
		double px = 0, py = 0;
		if (s.demoPic && s.demoPic->view.Valid()) {
			px = s.demoPic->view.minX + s.demoPic->view.width * s.demoPic->view.cell / 2;
			py = s.demoPic->view.minY + s.demoPic->view.height * s.demoPic->view.cell / 2;
		}
		s.draft = RotateAbout(s.draft, num(4, px), num(5, py), num(3, 0));
		s.fitDirty = true;
	} else if (0 == _stricmp(op, "reset")) {
		s.draft = Alignment{};
		s.fitDirty = true;
	} else if (0 == _stricmp(op, "revert")) {
		s.pick = s.settings.enabled ? s.settings.map : MapInfo{};
		s.draft = s.settings.align;
		s.fitDirty = true;
		if (!s.pick.map.empty()) StartPicture(s.pickJob, s.pick.map, true, s.pick.addon);
	} else if (0 == _stricmp(op, "auto")) {
		if (s.pick.map.empty()) { advancedfx::Warning("%s altenv auto: pick a map first\n", cmd); return; }
		StartAutoAlign();
	} else if (0 == _stricmp(op, "apply")) {
		Apply();
	} else if (0 == _stricmp(op, "off")) {
		Off();
	} else if (0 == _stricmp(op, "props")) {
		// The demo map's own props on the new map. Not part of the copy: it applies at once.
		const char* v = n > 3 ? args->ArgV(3) : "toggle";
		// Diagnosis: which entity classes carry models, and which lighting entities exist (AltEnvironmentProps.h).
		if (0 == _stricmp(v, "census")) { Props::Census(); return; }
		if (0 == _stricmp(v, "lights")) { Props::LightCensus(); return; }
		const bool on = 0 == _stricmp(v, "on") || 0 == _stricmp(v, "1") ? true
			: 0 == _stricmp(v, "off") || 0 == _stricmp(v, "0") ? false : !s.settings.props;
		s.settings.props = on;
		if (!s.original.empty() && !s.settings.map.map.empty()) SaveSettings(s.original, s.settings);
		MvmDebugLog_LinefAlways("altenv", "props %s", on ? "shown" : "hidden");
	} else if (0 == _stricmp(op, "view")) {
		const char* v = n > 3 ? args->ArgV(3) : "toggle";
		s.view = 0 == _stricmp(v, "on") ? true : 0 == _stricmp(v, "off") ? false : !s.view;
	} else if (0 == _stricmp(op, "sceneprobe")) {
		// Research (AltEnvironmentSceneProbe.h): dump the scene objects the world draws through, next
		// to the world offset the playing copy applied.
		const WorldOffset w = WorldOffsetFor(s.settings.align);
		char expected[256];
		snprintf(expected, sizeof(expected), "copy=%d level=%s align=(%.2f %.2f %.2f yaw %.2f) worldoffset pos=(%.2f %.2f %.2f) yaw=%.2f cos=%.5f sin=%.5f",
			s.copyPlaying ? 1 : 0, LevelName().c_str(), s.settings.align.x, s.settings.align.y, s.settings.align.z, s.settings.align.yaw,
			w.pos.x, w.pos.y, w.pos.z, w.yaw, std::cos(w.yaw * kPi / 180), std::sin(w.yaw * kPi / 180));
		AltEnvironment_SceneProbeArm(int(num(3, 32)), expected);
		advancedfx::Message("%s altenv sceneprobe: armed (mvm_debug channel altenv.sceneprobe)\n", cmd);
		return;
	} else {
		advancedfx::Message(
			"%s altenv status                    what the open demo plays on\n"
			"%s altenv maps [rescan]             the maps it can play on (installed and Workshop)\n"
			"%s altenv pick <map> [item]|none    choose the replacement map (and its Workshop item)\n"
			"%s altenv align <x> <y> <z> <yaw>   where the demo lands on it (units, degrees)\n"
			"%s altenv move <dx> <dy> [dz]       shift that\n"
			"%s altenv turn <deg> [px py]        turn it about the demo map's middle (or a demo point)\n"
			"%s altenv reset | revert            no offset | back to what is saved\n"
			"%s altenv auto                      AUTO ALIGN: match the two maps\n"
			"%s altenv apply                     play the demo on the picked map\n"
			"%s altenv off                       play it on its own map again\n"
			"%s altenv props on|off|toggle|census|lights  the demo map's own props (census: entity classes and models;\n"
			"                                    lights: the lighting entities, in the mvm_debug log)\n"
			"%s altenv view on|off|toggle        the alignment view in CONFIG\n"
			"%s altenv sceneprobe [n]            research: dump n scene objects the world draws through\n",
			cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
		return;
	}
	// An edited alignment makes the last AUTO ALIGN verdict stale; a copy being written keeps its line.
	const bool edited = !_stricmp(op, "align") || !_stricmp(op, "move") || !_stricmp(op, "turn") || !_stricmp(op, "reset") || !_stricmp(op, "revert");
	if (edited && !(s.build && s.build->job.valid())) SetStatus("");
	if (quiet) return;
	advancedfx::Message("%s\n", HealthLine().c_str());
	advancedfx::Message("  demo map %s, picked %s%s%s, draft (%.2f %.2f %.2f yaw %.2f)%s\n", s.demoMap.c_str(),
		s.pick.map.empty() ? "(its own map)" : s.pick.map.c_str(), s.pick.addon.empty() ? "" : " workshop ", s.pick.addon.c_str(), s.draft.x,
		s.draft.y, s.draft.z, s.draft.yaw, s.haveFit ? "" : "");
	if (s.haveFit) advancedfx::Message("  fit: floor %.0f%%, walked paths on the map %.0f%% of %zu\n", s.fit.floor * 100, s.fit.players * 100, s.fit.playerSamples);
	if (s.haveLandings && !s.pick.map.empty())
		advancedfx::Message("  map view (as last drawn): %.0f%% on the map, %.0f%% floating, %.0f%% under it, %.0f%% outside it\n", s.landings.Share(s.landings.on) * 100,
			s.landings.Share(s.landings.floating) * 100, s.landings.Share(s.landings.under) * 100, s.landings.Share(s.landings.outside) * 100);
	if (!s.status.empty()) advancedfx::Message("  %s\n", s.status.c_str());
	if (!s.error.empty()) advancedfx::Message("  error: %s\n", s.error.c_str());
}

void Shutdown() {
	State& s = S();
	if (s.build) { s.build->cancel = true; if (s.build->job.valid()) s.build->job.wait(); }
	for (auto* job : { &s.demoJob, &s.pickJob })
		if (*job) { (*job)->cancel = true; if ((*job)->job.valid()) (*job)->job.wait(); }
	for (auto& job : s.retired)
		if (job->job.valid()) job->job.wait();
	s.feetCancel = true;
	if (s.feetJob.valid()) s.feetJob.wait();
	if (s.autoJob.valid()) s.autoJob.wait();
	if (s.mapsJob.valid()) s.mapsJob.wait();
	if (s.demoPngJob.valid()) s.demoPngJob.wait();
	if (s.fitJob.valid()) s.fitJob.wait();
}

} // namespace Filmmaker::AltEnvironment
