#define NOMINMAX
#include "LensFxState.h"
#include "LensMaskMath.h"
#include "../Environment/EnvironmentMaterialMath.h"
#include "../../../Shared/AfxConsole.h"
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <utility>

namespace Filmmaker {
using namespace Lens;
namespace {
using MaskImage = LensMaskImage;
std::mutex g_mutex;
Values g_values;
uint64_t g_revision = 0;
MaskImage g_iris;
MaskImage g_dirt;
std::string g_status = "Off";
std::atomic<unsigned long long> g_frames{0};

const int kBuiltInMaskSize = 128;

MaskImage BuildBuiltInMask(int id) {
	MaskImage m;
	m.width = m.height = kBuiltInMaskSize;
	m.pixels.resize((size_t)kBuiltInMaskSize * kBuiltInMaskSize);
	for (int y = 0; y < kBuiltInMaskSize; ++y)
		for (int x = 0; x < kBuiltInMaskSize; ++x) {
			const float fx = ((x + 0.5f) / kBuiltInMaskSize) * 2.0f - 1.0f;
			const float fy = 1.0f - ((y + 0.5f) / kBuiltInMaskSize) * 2.0f;
			m.pixels[(size_t)y * kBuiltInMaskSize + x] = IrisMaskCoverage(id, fx, fy) > 0.5f ? 255 : 0;
		}
	m.label = IrisMaskName(id);
	return m;
}

// The shared reducer bounds GPU cost without dropping thin images or partial edge boxes.
MaskImage MaskFromRgba(const Environment::MaterialImage& img, const std::string& label) {
    auto reduced=ReduceMaskRgba(img.width,img.height,img.rgba);
    MaskImage result;result.width=reduced.width;result.height=reduced.height;
    result.pixels=std::move(reduced.pixels);result.label=label;
    return result;
}

struct Once {
	Once() {
		g_iris = BuildBuiltInMask(MaskDiamond);
		g_iris.serial = 1;
	}
} s_once;

const char* const kShapeNames[IrisShapeCount] = { "circle", "polygon", "star", "heart", "ring", "custom" };

std::string JsonSafe(const std::string& s) {
	std::string r;
	for (char c : s) {
		if (c == '\\') r += '/';
		else if (c == '"' || (unsigned char)c < 32) r += ' ';
		else r += c;
	}
	return r;
}

std::wstring Widen(const std::string& s) {
	if (s.empty()) return L"";
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w((size_t)std::max(n, 1), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
	w.resize((size_t)std::max(n - 1, 0));
	return w;
}

int ShapeFromName(const char* s) {
	if (!s) return -1;
	for (int i = 0; i < IrisShapeCount; ++i)
		if (0 == _stricmp(kShapeNames[i], s)) return i;
	char* end = nullptr;
	const long v = std::strtol(s, &end, 10);
	return (end && !*end && v >= 0 && v < IrisShapeCount) ? (int)v : -1;
}

} // namespace

Values LensFx_Get() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_values;
}

bool LensFx_Set(const char* key, double value) {
	const int p = FindParam(key);
	if (p < 0) return false;
	std::lock_guard<std::mutex> lock(g_mutex);
    const float clamped = ClampParam(p,value);
    if (g_values[p] != clamped) { g_values[p] = clamped; ++g_revision; }
	return true;
}

void LensFx_ResetGroup(Group group) {
	std::lock_guard<std::mutex> lock(g_mutex);
    const Values before = g_values;
	ResetGroup(g_values, group);
	g_values[group == Group::Dof ? DofOn : FlareOn] = 0;
    if (g_values.v != before.v) ++g_revision;
}

bool LensFx_Preset(Group group, const char* name) {
	std::lock_guard<std::mutex> lock(g_mutex);
    const Values before = g_values;
    const bool applied = group == Group::Dof ? ApplyDofPreset(g_values,name) : ApplyFlarePreset(g_values,name);
    if (g_values.v != before.v) ++g_revision;
    return applied;
}

bool LensFx_Active() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_values[DofOn] > 0.5f || g_values[FlareOn] > 0.5f;
}

bool LensFx_DofOn() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_values[DofOn] > 0.5f;
}

bool LensFx_FlareOn() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_values[FlareOn] > 0.5f;
}

bool LensFx_SetBuiltInIris(const char* name) {
	const int id = IrisMaskFromName(name);
	if (id < 0) return false;
	std::lock_guard<std::mutex> lock(g_mutex);
	const unsigned serial = g_iris.serial + 1;
	g_iris = BuildBuiltInMask(id);
	g_iris.serial = serial;
	g_values[DofShape] = (float)IrisCustom;
    ++g_revision;
	return true;
}

bool LensFx_LoadIrisFile(const char* path, std::string* error) {
	Environment::MaterialImage img;
	std::string err;
	if (!Environment::LoadMaterialImage(Widen(path ? path : ""), img, err)) {
		if (error) *error = err;
		return false;
	}
	MaskImage m = MaskFromRgba(img, "file");
	std::lock_guard<std::mutex> lock(g_mutex);
	m.serial = g_iris.serial + 1;
	g_iris = std::move(m);
	g_values[DofShape] = (float)IrisCustom;
    ++g_revision;
	return true;
}

static bool LoadDirtFile(const char* path, std::string* error) {
	Environment::MaterialImage img;
	std::string err;
	if (!Environment::LoadMaterialImage(Widen(path ? path : ""), img, err)) {
		if (error) *error = err;
		return false;
	}
	MaskImage m = MaskFromRgba(img, "file");
	std::lock_guard<std::mutex> lock(g_mutex);
	m.serial = g_dirt.serial + 1;
	g_dirt = std::move(m);
    ++g_revision;
	return true;
}
static void ClearDirt() {
	std::lock_guard<std::mutex> lock(g_mutex);
	const unsigned serial = g_dirt.serial + 1;
	g_dirt = MaskImage{};
	g_dirt.serial = serial;
    ++g_revision;
}

void LensFx_AppendStateJson(std::ostringstream& out) {
	std::lock_guard<std::mutex> lock(g_mutex);
	out << ",\"lens\":{";
	for (int i = 0; i < ParamCount; ++i) {
		if (i) out << ",";
		out << "\"" << Specs()[i].key << "\":" << g_values[i];
	}
	// "At defaults" per group, including the off switch, so a card's Reset can dim itself.
	for (Group grp : { Group::Dof, Group::Flare }) {
		bool atDefault = true;
		for (int i = 0; i < ParamCount; ++i)
			if (InGroup(i, grp) && g_values[i] != Specs()[i].def) atDefault = false;
		out << ",\"" << (grp == Group::Dof ? "dofDefault" : "flareDefault") << "\":" << (atDefault ? "true" : "false");
	}
	out << ",\"irisMask\":\"" << JsonSafe(g_iris.label) << "\""
		<< ",\"dirtLoaded\":" << (g_dirt.pixels.empty() ? "false" : "true")
		<< ",\"status\":\"" << JsonSafe(g_status) << "\""
		<< ",\"frames\":" << g_frames.load() << "}";
}

void LensFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	auto arg = [&](int i) -> const char* { return i < argc ? args->ArgV(i) : ""; };
	auto quiet = [&]() { for (int i = 2; i < argc; ++i) if (0 == _stricmp(arg(i), "quiet")) return true; return false; };
	auto state = [&]() {
		std::ostringstream o;
		LensFx_AppendStateJson(o);
		std::string s = o.str();
		advancedfx::Message("[lens][state] {%s}\n", s.size() > 1 ? s.c_str() + 1 : "");
	};
	const char* sub = arg(2);

	if (!*sub || 0 == _stricmp(sub, "help")) {
		advancedfx::Message(
			"%s lens - depth of field, bokeh and lens flare, a post pass over the finished world frame.\n"
			"  lens dof|flare on|off|toggle       switch one effect\n"
			"  lens set <key> <value>             set any parameter (keys below); values are clamped to range\n"
			"  lens <key> <value>                 same, e.g. lens dof.focus 350\n"
			"  lens reset [dof|flare|all]         back to defaults and switches the group off\n"
			"  lens preset dof cinematic|anamorphic|portrait|miniature|hearts|stars\n"
			"  lens preset flare cinematic|anamorphic|classic|star|dreamy\n"
			"  lens iris shape circle|polygon|star|heart|ring|custom\n"
			"  lens iris mask diamond|cross|crescent|clover|smile   built-in custom shape\n"
			"  lens iris file <path>              your own black-and-white image as the iris\n"
			"  lens dirt <path>|off               your own lens-dirt image for the flare\n"
			"  lens focus <units>|auto            fixed focus distance, or focus on the middle of the screen\n"
			"  lens flare.tint <name>|<0..7>      flare colour: warm white gold cool teal rose violet ember\n"
			"  lens flare.streakTint <name>|<0..5>  streak colour: blue cyan white amber red magenta\n"
			"  lens state                         every value as JSON\n"
			"Parameters:", cmd);
		for (int i = 0; i < ParamCount; ++i)
			advancedfx::Message("%s%s [%g..%g]", (i % 4) ? "  " : "\n    ", Specs()[i].key, Specs()[i].min, Specs()[i].max);
		advancedfx::Message("\n");
		return;
	}
	if (0 == _stricmp(sub, "state")) { state(); return; }

	if (0 == _stricmp(sub, "dof") || 0 == _stricmp(sub, "flare")) {
		const bool dof = 0 == _stricmp(sub, "dof");
		const int p = dof ? DofOn : FlareOn;
		const char* a = arg(3);
		const bool cur = LensFx_Get()[p] > 0.5f;
		const bool on = (0 == _stricmp(a, "toggle") || !*a) ? !cur : (0 == _stricmp(a, "on") || 0 == _stricmp(a, "1"));
		LensFx_Set(Specs()[p].key, on ? 1 : 0);
		if (!quiet()) advancedfx::Message("lens: %s %s.\n", sub, on ? "ON" : "off");
		return;
	}
	if (0 == _stricmp(sub, "set") || FindParam(sub) >= 0) {
		const bool viaSet = 0 == _stricmp(sub, "set");
		const char* key = viaSet ? arg(3) : sub;
		const char* val = viaSet ? arg(4) : arg(3);
		if (FindParam(key) < 0) { advancedfx::Message("lens: unknown parameter '%s' (run `lens help`).\n", key); return; }
		if (!*val) { advancedfx::Message("lens: %s = %g\n", key, LensFx_Get()[FindParam(key)]); return; }
		// The two flare colours are palette indices; their names are accepted too.
		int named = -1;
		if (0 == _stricmp(key, "flare.tint")) named = FlareTintFromName(val);
		else if (0 == _stricmp(key, "flare.streakTint")) named = StreakTintFromName(val);
		LensFx_Set(key, named >= 0 ? (double)named : atof(val));
		if (!quiet()) advancedfx::Message("lens: %s = %g\n", key, LensFx_Get()[FindParam(key)]);
		return;
	}
	if (0 == _stricmp(sub, "reset")) {
		const char* w = arg(3);
		if (!*w || 0 == _stricmp(w, "all") || 0 == _stricmp(w, "quiet")) { LensFx_ResetGroup(Group::Dof); LensFx_ResetGroup(Group::Flare); }
		else if (0 == _stricmp(w, "dof")) LensFx_ResetGroup(Group::Dof);
		else if (0 == _stricmp(w, "flare")) LensFx_ResetGroup(Group::Flare);
		if (!quiet()) advancedfx::Message("lens: reset.\n");
		return;
	}
	if (0 == _stricmp(sub, "preset")) {
		const bool dof = 0 == _stricmp(arg(3), "dof");
		if (!dof && 0 != _stricmp(arg(3), "flare")) { advancedfx::Message("lens: preset dof|flare <name>\n"); return; }
		if (!LensFx_Preset(dof ? Group::Dof : Group::Flare, arg(4))) advancedfx::Message("lens: unknown %s preset '%s'.\n", arg(3), arg(4));
		else if (!quiet()) advancedfx::Message("lens: %s preset %s applied.\n", arg(3), arg(4));
		return;
	}
	if (0 == _stricmp(sub, "iris")) {
		const char* what = arg(3);
		if (0 == _stricmp(what, "shape")) {
			const int s = ShapeFromName(arg(4));
			if (s < 0) { advancedfx::Message("lens: iris shape circle|polygon|star|heart|ring|custom\n"); return; }
			LensFx_Set("dof.shape", s);
			if (!quiet()) advancedfx::Message("lens: iris shape %s.\n", kShapeNames[s]);
		} else if (0 == _stricmp(what, "mask")) {
			if (!LensFx_SetBuiltInIris(arg(4))) advancedfx::Message("lens: masks are diamond|cross|crescent|clover|smile\n");
			else if (!quiet()) advancedfx::Message("lens: iris mask %s.\n", arg(4));
		} else if (0 == _stricmp(what, "file")) {
			std::string path;
			for (int i = 4; i < argc; ++i) { if (0 == _stricmp(arg(i), "quiet")) continue; if (!path.empty()) path += ' '; path += arg(i); }
			std::string err;
			if (!LensFx_LoadIrisFile(path.c_str(), &err)) advancedfx::Message("lens: could not load '%s': %s\n", path.c_str(), err.c_str());
			else if (!quiet()) advancedfx::Message("lens: iris image loaded.\n");
		} else {
			advancedfx::Message("lens iris shape <name> | mask <name> | file <path>\n");
		}
		return;
	}
	if (0 == _stricmp(sub, "dirt")) {
		if (0 == _stricmp(arg(3), "off")) { ClearDirt(); if (!quiet()) advancedfx::Message("lens: dirt image cleared.\n"); return; }
		std::string path;
		for (int i = 3; i < argc; ++i) { if (0 == _stricmp(arg(i), "quiet")) continue; if (!path.empty()) path += ' '; path += arg(i); }
		std::string err;
		if (!LoadDirtFile(path.c_str(), &err)) advancedfx::Message("lens: could not load '%s': %s\n", path.c_str(), err.c_str());
		else if (!quiet()) advancedfx::Message("lens: dirt image loaded.\n");
		return;
	}
	if (0 == _stricmp(sub, "focus")) {
		if (0 == _stricmp(arg(3), "auto")) LensFx_Set("dof.auto", 1);
		else if (*arg(3)) { LensFx_Set("dof.auto", 0); LensFx_Set("dof.focus", atof(arg(3))); }
		if (!quiet()) advancedfx::Message("lens: focus %s.\n", LensFx_Get()[DofAuto] > 0.5f ? "auto" : arg(3));
		return;
	}
	advancedfx::Message("%s lens: unknown subcommand '%s' (run `%s lens help`).\n", cmd, sub, cmd);
}

LensRenderState LensFx_ReadRenderState(unsigned irisSerial,unsigned dirtSerial) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LensRenderState out;
    out.values = g_values;
    out.irisChanged = g_iris.serial != irisSerial;
    out.dirtChanged = g_dirt.serial != dirtSerial;
    if (out.irisChanged) out.iris = g_iris;
    if (out.dirtChanged) out.dirt = g_dirt;
    return out;
}

void LensFx_SetRenderStatus(const char* status) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status = status ? status : "";
}
void LensFx_NoteRenderedFrame() { g_frames.fetch_add(1); }
uint64_t LensFx_PictureRevision() { std::lock_guard<std::mutex> lock(g_mutex); return g_revision; }

} // namespace Filmmaker
