#include "ConfigInspector.h"

#include "UiWidgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace SourceFilmmaker {
namespace Ui {

namespace {

#define MIDDOT "\xC2\xB7"

std::string Format(const char* fmt, ...) {
	char buf[256];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	return buf;
}

long Round(double v) { return std::lround(v); }

// The cards' edges: CS2's Config blue is CS:S's orange; Weapon Bob keeps its cyan, the colour of
// its pivot marker.
constexpr Color kMotion = Theme::Accent;
constexpr Color kRecoilAmber = Rgba(0xe8, 0xb3, 0x39);
constexpr Color kBobCyan = Rgba(0x5f, 0xd0, 0xc0);

// Weapon Recoil's scopes (CS2's Applies to and its key cells) and the PIVOT drawer's classes.
const char* const kScopeKinds[3] = { "all", "slot", "type" };
const char* const kScopeKindNames[3] = { "All", "Slot", "Type" };
const char* const kSlotKeys[2] = { "primary", "secondary" };
const char* const kSlotNames[2] = { "Primary", "Secondary" };
const char* const kTypeKeys[6] = { "pistol", "smg", "rifle", "shotgun", "sniper", "lmg" };
const char* const kTypeNames[6] = { "Pistols", "SMGs", "Rifles", "Shotguns", "Snipers", "LMGs" };
const char* const kPivotScopes[7] = { "rifle", "smg", "pistol", "shotgun", "sniper", "knife", "utility" };
const char* const kPivotNames[7] = { "Rifle", "SMG", "Pistol", "Shotgun", "Sniper", "Knife", "Utility" };

// The recorded operator curves Handheld plays per movement state (CS2's HH_CURVES).
struct Curve {
	const char* key;
	const char* caption;
};
const Curve kCurves[] = {
	{ "INVESTIGATION", "Investigation" },
	{ "THE_CLOSEUP", "The Closeup" },
	{ "THE_WEDDING", "The Wedding" },
	{ "WALK_TO_THE_STORE", "Walk to the Store" },
	{ "HANDYCAM_RUN", "HandyCam Run" },
	{ "OUT_CAR_WINDOW", "Out Car Window" },
	{ "BIKE_ON_GRAVEL_2D", "Bike On Gravel" },
	{ "SPACESHIP_SHAKE_2D", "Spaceship Shake" },
	{ "THE_ZEEK_2D", "The Zeek" },
};
constexpr int kCurveCount = (int)(sizeof(kCurves) / sizeof(kCurves[0]));
const char* const kCurveStates[3] = { "idle", "walk", "run" };

const char* CurveCaption(const std::string& key) {
	for (const Curve& c : kCurves)
		if (key == c.key) return c.caption;
	return key.c_str();
}

const std::string& CurveOf(const ConfigModel& m, int state) { return state == 0 ? m.hhIdle : (state == 1 ? m.hhWalk : m.hhRun); }

// ---- the summaries CS2 writes on each card's rail row and header -----------------------------------

std::string SmoothViewSummary(const ConfigModel& m) {
	if (!m.svOn) return "Off";
	const std::string tilt = m.svTiltEnabled ? Format("%ld", Round(m.svTilt)) : std::string("off");
	return "Breathing " MIDDOT " tilt " + tilt + " " MIDDOT " effects " + (m.svFalling || m.svJumping || m.fisheye ? "on" : "off");
}

std::string HandheldSummary(const ConfigModel& m) {
	if (!m.handheldOn) return "Off";
	return Format("Drift %ld%% " MIDDOT " curves %ld%% " MIDDOT " lock %ld%%", Round(m.hhDrift), Round(m.hhCurve), Round(m.hhCenterLock));
}

std::string WeaponRecoilSummary(const ConfigModel& m) {
	if (!m.wrOn) return "Off";
	return Format("%ld%% kick " MIDDOT " %s " MIDDOT " camera %ld%%", Round(m.wrAmount), m.wrStyle == "style2" ? "snappy" : "springy",
		Round(m.wrCamRot));
}

std::string WeaponBobSummary(const ConfigModel& m) {
	if (!m.wbOn) return "Off";
	return Format("%ld%% " MIDDOT " sway %ld%% " MIDDOT " camera share %ld%%", Round(m.wbAmount), Round(m.wbSway), Round(m.wbCam));
}

// ---- rows ------------------------------------------------------------------------------------------

// The pane being drawn: its column, where the edits go, and the row under the pointer for the tooltip.
struct Pane {
	CardColumn c;
	ConfigState* st = nullptr;
	std::vector<std::string>* out = nullptr;
	const char* tip = nullptr;
	Rect tipRect;
};

void Send(Pane& p, const std::string& cmd) { p.out->push_back(cmd); }

void HoverRect(Pane& p, const Rect& r, const char* tip) {
	if (!tip || !ImGui::IsWindowHovered()) return;
	if (ImGui::IsMouseHoveringRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1))) {
		p.tip = tip;
		p.tipRect = r;
	}
}

// A small uppercase caption over a control (the Applies to and Style columns).
void Caption(CardColumn& c, const char* text) {
	ImFont* font = c.Font(FontRole::Caption);
	std::string upper = text;
	for (char& ch : upper)
		if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
	DrawSpacedText(ImGui::GetWindowDrawList(), font, ImVec2(c.x0, c.y), ToU32(Theme::TextDim), upper.c_str(), 1.5f * c.s);
	c.y += font->FontSize + 5 * c.s;
}

// CS2 attaches a row's tooltip to the whole row.
void Hover(Pane& p, float y0, const char* tip) {
	if (!tip || !ImGui::IsWindowHovered()) return;
	const Rect r = MakeRect(p.c.x0, y0, p.c.x1, p.c.y);
	if (ImGui::IsMouseHoveringRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1))) {
		p.tip = tip;
		p.tipRect = r;
	}
}

// A slider (sliderRow): every whole step of the drag sends "<cmd> <value> quiet".
void Slider(Pane& p, const char* id, const char* label, const char* tip, const char* cmd, double value, double min, double max,
	int decimals, const char* unit, bool offAtZero, bool dim = false) {
	const float y0 = p.c.y + Metrics::line.gap * p.c.s;
	ValueRowStyle style;
	style.fine = 0.2;
	style.atMin = offAtZero ? "Off" : nullptr;
	style.dim = dim;
	const ValueEdit e = ValueRow(p.c, id, label, value, min, max, decimals, unit, Theme::Accent, true, style);
	// The module takes the value a frame later, so the drag asks for it once, not every frame until then.
	if (e.changed) {
		const std::string sent = Format("%s %.*f quiet", cmd, decimals, e.value);
		if (sent != p.st->lastSent) Send(p, sent);
		p.st->lastSent = sent;
	}
	if (e.ended) p.st->lastSent.clear();
	Hover(p, y0, tip);
}

// A switch row (togglePill): the whole row flips it.
void Toggle(Pane& p, const char* id, const char* label, const char* tip, bool on, const char* cmd, bool enabled = true) {
	const float y0 = p.c.y + Metrics::line.gap * p.c.s;
	if (SwitchRow(p.c, id, label, on, enabled)) Send(p, cmd);
	Hover(p, y0, tip);
}

void Group(Pane& p, const char* head) { SubHead(p.c, head); }

// A card's header: Reset (dim while the card is at its defaults) and the master switch.
void Header(Pane& p, const char* id, const char* title, const std::string& summary, Color edge, bool on, bool atDefault,
	const char* const* reset, int resetCount, const char* toggle) {
	const CardAction buttons[] = { { "Reset", ButtonKind::Normal, !atDefault } };
	CardSwitch master;
	master.show = true;
	master.on = on;
	const int hit = CardHeader(p.c, id, title, summary.c_str(), buttons, 1, edge, master);
	if (hit == 0)
		for (int i = 0; i < resetCount; ++i) Send(p, reset[i]);
	else if (hit == 1)
		Send(p, toggle);
}

// ---- the cards -------------------------------------------------------------------------------------

void SmoothViewCard(Pane& p, const ConfigModel& m) {
	const char* reset[] = { "mirv_filmmaker smoothview reset", "mirv_filmmaker fisheye off" };
	Header(p, "smoothview", "Smooth View", SmoothViewSummary(m), kMotion, m.svOn, m.smoothViewDefault && !m.fisheye, reset, 2,
		"mirv_filmmaker smoothview toggle");
	Group(p, "View");
	Toggle(p, "falling", "Fall pitch", "Pitches the camera during a fall.", m.svFalling, "mirv_filmmaker smoothview falling toggle");
	Toggle(p, "jumping", "Jump kick", "Kicks the camera on take-off and landing.", m.svJumping, "mirv_filmmaker smoothview jumping toggle");
    Toggle(p,"fisheye","Fisheye Lens","Barrel-distortion wide-angle lens pass.",m.fisheye,"mirv_filmmaker fisheye toggle");
	Group(p, "Breathing");
	Toggle(p, "healthsway", "Health Sway", "Breathing gets heavier as health drops.", m.svHealthSway,
		"mirv_filmmaker smoothview healthSway toggle");
	Slider(p, "breathx", "Freq X", "Speed of the side-to-side half of the idle breathing figure-8.", "mirv_filmmaker smoothview breathX",
		m.svBreathX, 0.1, 2, 2, "x", false);
	Slider(p, "breathy", "Freq Y", "Speed of the up-and-down half of the idle breathing figure-8.", "mirv_filmmaker smoothview breathY",
		m.svBreathY, 0.1, 2, 2, "x", false);
	Slider(p, "standamp", "Stand Amp", "Size of the breathing sway while standing still, in degrees.",
		"mirv_filmmaker smoothview standAmp", m.svStandAmp, 0, 2, 2, " deg", false);
	Slider(p, "walkamp", "Walk Amp", "Size of the breathing sway while walking, in degrees.", "mirv_filmmaker smoothview walkAmp",
		m.svWalkAmp, 0, 2, 2, " deg", false);
	Slider(p, "runamp", "Run Amp", "Size of the breathing sway while running, in degrees.", "mirv_filmmaker smoothview runAmp",
		m.svRunAmp, 0, 2, 2, " deg", false);
	Group(p, "Tilt");
	Toggle(p, "tilton", "Strafe Tilt", "Leans the camera into strafing movement.", m.svTiltEnabled,
		"mirv_filmmaker smoothview tiltEnabled toggle");
	Toggle(p, "tiltinv", "Reverse tilt", "Leans away from the movement instead of into it.", m.svTiltInv,
		"mirv_filmmaker smoothview tiltInv toggle");
	Slider(p, "tilt", "Tilt Angle", "How far the view leans into a strafe. 0 keeps the horizon level.", "mirv_filmmaker smoothview tilt",
		m.svTilt, 0, 60, 0, "", false);
	Slider(p, "tiltspeed", "Tilt Speed", "How quickly the lean reaches full angle and returns. Higher is snappier.",
		"mirv_filmmaker smoothview tiltSpeed", m.svTiltSpeed, 0.5, 30, 1, "x", false);
}

void HandheldCard(Pane& p, const ConfigModel& m) {
	const char* reset[] = { "mirv_filmmaker handheld reset quiet" };
	Header(p, "handheld", "Handheld Camera", HandheldSummary(m), kMotion, m.handheldOn, m.handheldDefault, reset, 1,
		"mirv_filmmaker handheld toggle");
	Group(p, "General");
	Toggle(p, "vmdecouple", "Decouple Viewmodel",
		m.viewmodelChannel ? "Keeps the weapon independent from handheld movement."
		                   : "Keeps the weapon independent from handheld movement. It needs a way to move the weapon on its own, "
		                     "which Counter-Strike: Source does not have yet, so the weapon moves with the camera.",
		m.hhVmDecoupled, "mirv_filmmaker handheld viewmodel toggle", m.viewmodelChannel);
	Toggle(p, "centerlock", "Center Lock", "Keeps recorded aim centered while the camera still moves.", m.hhCenterLockOn,
		"mirv_filmmaker handheld centerlock toggle");
	Slider(p, "lockpct", "Lock Strength",
		"How strongly the recorded aim is pulled back to screen centre, so added motion never hides the action.",
		"mirv_filmmaker handheld centerlockpct", m.hhCenterLock, 0, 100, 0, "%", false, !m.hhCenterLockOn);
	Group(p, "Movement");
	Slider(p, "drift", "Drift", "Slow sway, jitter and tremble, as if someone is holding the camera.", "mirv_filmmaker handheld drift",
		m.hhDrift, 0, 150, 0, "%", true);
	Slider(p, "driftdir", "Drift Direction",
		"Which way the slow drift leans. Its cycle is longer than most shots, so a clip usually sees only one lean. This picks which.",
		"mirv_filmmaker handheld driftdir", m.hhDriftDir, 0, 360, 0, "\xC2\xB0", false);
	Slider(p, "tilt", "Ambient Tilt", "A slow lean that wanders while the shot runs.", "mirv_filmmaker handheld tilt", m.hhTilt, 0, 150,
		0, "%", true);
	Slider(p, "variance", "Variance", "Varies each pass of a motion curve so a looping take stops reading as a loop.",
		"mirv_filmmaker handheld variance", m.hhVariance, 0, 100, 0, "%", false);
	Group(p, "Curves");
	Slider(p, "curve", "Motion Curves", "Recorded human camera motion, switching itself between idle, walk and run.",
		"mirv_filmmaker handheld curve", m.hhCurve, 0, 150, 0, "%", true);
	Slider(p, "speed", "Curve Speed", "Playback speed of those recorded curves. Higher makes the operator seem more restless.",
		"mirv_filmmaker handheld speed", m.hhSpeed, 25, 300, 0, "%", false);
	Group(p, "Curve per movement state");
	const char* labels[3] = { "Standing", "Walking", "Running" };
	const char* tips[3] = { "Which recorded operator curve plays while the subject stands still.",
		"Which recorded operator curve plays while the subject walks.", "Which recorded operator curve plays while the subject runs." };
	for (int i = 0; i < 3; ++i) {
		const float y0 = p.c.y + (Metrics::line.gap + 1) * p.c.s;
		ImGui::PushID(i);
		const DropClick d = DropRow(p.c, "curve", labels[i], CurveCaption(CurveOf(m, i)), Theme::Text, nullptr, true);
		ImGui::PopID();
		if (d.field) {
			p.st->openDrop = p.st->openDrop == i ? -1 : i;
			p.st->dropField = d.fieldRect;
		}
		if (p.st->openDrop == i) p.st->dropField = d.fieldRect; // follows the pane's scroll
		Hover(p, y0, tips[i]);
	}
	Group(p, "Detail");
	Slider(p, "regrip", "Re-grip Fumbles", "Occasional jolts, as if the operator adjusts their grip.", "mirv_filmmaker handheld regrip",
		m.hhRegrip, 0, 150, 0, "%", true);
	Slider(p, "period", "Fumble Gap", "Average seconds between those grip adjustments.", "mirv_filmmaker handheld period", m.hhPeriod, 2,
		60, 0, "s", false);
}

// A hinge slider in a PIVOT drawer: no fill, a centre tick, the knob in the card's colour. The
// backend's set is silent, so no quiet.
void PivotSlider(Pane& p, const char* id, const char* label, const char* tip, double value, bool enabled, const char* owner,
	const char* scope, const char* axis, Color colour) {
	const float y0 = p.c.y + Metrics::line.gap * p.c.s;
	ValueRowStyle style;
	style.fine = 0.2;
	style.noFill = true;
	style.centreTick = true;
	style.knob = colour;
	const ValueEdit e = ValueRow(p.c, id, label, value, -30, 30, 1, "u", colour, enabled, style);
	if (e.changed && enabled) {
		const std::string sent = Format("mirv_filmmaker pivot %s set %s %s %.1f", owner, scope, axis, e.value);
		if (sent != p.st->lastSent) Send(p, sent);
		p.st->lastSent = sent;
	}
	if (e.ended) p.st->lastSent.clear();
	Hover(p, y0, tip);
}

// CS2's PIVOT drawer (pivotSection) at a weapon card's end: where the effect hinges the weapon, per
// weapon class. Shut by default; its head says the class in shot and its hinge, with the marker's
// switch and a reset. Only the class in shot can be edited. index 0 is Weapon Recoil's, 1 Weapon Bob's.
void PivotDrawer(Pane& p, const ConfigModel& m, const ConfigPivot& pv, int index) {
	const char* owner = index == 0 ? "recoil" : "bob";
	const Color colour = index == 0 ? kRecoilAmber : kBobCyan;
	const float s = p.c.s;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	ImFont* capFont = p.c.Font(FontRole::Caption);
	ImFont* noteFont = p.c.Font(FontRole::Note);
	p.c.y += 9 * s;
	const float top = p.c.y;
	draw->ChannelsSplit(2); // the box goes under what is drawn in it, once its height is known
	draw->ChannelsSetCurrent(1);

	int live = -1;
	for (int i = 0; i < 7; ++i)
		if (pv.cls == kPivotScopes[i]) live = i;
	const double* hinge = live >= 0 ? pv.hinge[live + 1] : nullptr;
	std::string gun = pv.weapon;
	if (gun.compare(0, 7, "weapon_") == 0) gun = gun.substr(7);
	const std::string summary = live >= 0
		? Format("%s%s " MIDDOT " %.1fu / %.1fu / %.1fu", kPivotNames[live], gun.empty() ? "" : (" (" + gun + ")").c_str(), hinge[0],
			hinge[1], hinge[2])
		: std::string("No weapon in shot");

	// The head: dot, PIVOT, the summary (wrapped rather than cut), the marker's eye, reset, chevron.
	const float iconW = 28 * s, iconH = 24 * s, chevW = 24 * s;
	const float capW = SpacedTextWidth(capFont, "PIVOT", 1.5f * s);
	const float textX = p.c.x0 + 10 * s + 8 * s + 8 * s + capW + 8 * s;
	const float sumW = (std::max)(20 * s, p.c.x1 - 6 * s - chevW - 2 * (iconW + 4 * s) - 4 * s - textX);
	const float sumH = noteFont->CalcTextSizeA(noteFont->FontSize, FLT_MAX, sumW, summary.c_str()).y;
	const float headH = (std::max)(34 * s, sumH + 10 * s);
	const Rect head = MakeRect(p.c.x0, top, p.c.x1, top + headH);
	const float iy = top + (headH - iconH) * 0.5f;
	const Rect chev = MakeRect(p.c.x1 - 6 * s - chevW, iy, p.c.x1 - 6 * s, iy + iconH);
	const Rect reset = MakeRect(chev.x0 - 4 * s - iconW, iy, chev.x0 - 4 * s, iy + iconH);
	const Rect eye = MakeRect(reset.x0 - 4 * s - iconW, iy, reset.x0 - 4 * s, iy + iconH);
	bool& open = p.st->pivotOpen[index];
	ImGui::PushID(owner);
	ImGui::SetCursorScreenPos(ImVec2(head.x0, head.y0));
	const bool headClicked = ImGui::InvisibleButton("head", ImVec2(eye.x0 - head.x0, headH));
	const bool headHot = ImGui::IsItemHovered();
	ImGui::SetCursorScreenPos(ImVec2(chev.x0, head.y0));
	const bool chevClicked = ImGui::InvisibleButton("chev", ImVec2(head.x1 - chev.x0, headH));
	const bool chevHot = ImGui::IsItemHovered();
	if (headClicked || chevClicked) open = !open;
	ImGui::SetCursorScreenPos(ImVec2(eye.x0, eye.y0));
	const bool eyeClicked = ImGui::InvisibleButton("eye", ImVec2(eye.W(), eye.H())) && m.pivotMarkers;
	const bool eyeHot = m.pivotMarkers && ImGui::IsItemHovered();
	ImGui::SetCursorScreenPos(ImVec2(reset.x0, reset.y0));
	const bool resetClicked = ImGui::InvisibleButton("reset", ImVec2(reset.W(), reset.H()));
	const bool resetHot = ImGui::IsItemHovered();
	ImGui::PopID();
	if (eyeClicked) {
		Send(p, Format("mirv_filmmaker pivotorb %s toggle quiet", owner));
		if (pv.orb) Send(p, Format("mirv_filmmaker pivotorb %s hide quiet", owner));
	}
	if (resetClicked) {
		Send(p, Format("mirv_filmmaker pivot %s reset", owner));
		Send(p, Format("mirv_filmmaker pivotorb %s off quiet", owner));
	}

	const float midY = top + headH * 0.5f;
	draw->AddRectFilled(ImVec2(p.c.x0 + 10 * s, midY - 4 * s), ImVec2(p.c.x0 + 18 * s, midY + 4 * s), ToU32(colour), 1 * s);
	DrawSpacedText(draw, capFont, ImVec2(p.c.x0 + 26 * s, std::floor(midY - capFont->FontSize * 0.5f)), ToU32(Theme::Text), "PIVOT", 1.5f * s);
	draw->AddText(noteFont, noteFont->FontSize, ImVec2(textX, std::floor(midY - sumH * 0.5f)), ToU32(Theme::TextDim), summary.c_str(),
		nullptr, sumW);
	{
		// The marker's switch: a ring and a dot, in the card's colour while on.
		const bool on = pv.orb && m.pivotMarkers;
		const Color ink = !m.pivotMarkers ? Theme::TextDisabled : (on ? colour : (eyeHot ? Theme::Text : Theme::TextDim));
		if (on) draw->AddRectFilled(ImVec2(eye.x0, eye.y0), ImVec2(eye.x1, eye.y1), ToU32(Faded(colour, 0.18f)), 3 * s);
		else if (eyeHot) draw->AddRectFilled(ImVec2(eye.x0, eye.y0), ImVec2(eye.x1, eye.y1), ToU32(Rgba(255, 255, 255, 20)), 3 * s);
		draw->AddRect(ImVec2(eye.x0, eye.y0), ImVec2(eye.x1, eye.y1), ToU32(on ? Faded(colour, 0.6f) : Rgba(255, 255, 255, 31)), 3 * s);
		const ImVec2 c((eye.x0 + eye.x1) * 0.5f, (eye.y0 + eye.y1) * 0.5f);
		draw->AddCircle(c, 6 * s, ToU32(ink), 20, 2 * s);
		draw->AddCircleFilled(c, 2.5f * s, ToU32(ink), 12);
	}
	{
		// Reset: a turning arrow, dim while every hinge is shipped.
		const Color ink = Faded(resetHot ? Theme::Text : Theme::TextDim, pv.atDefault ? 0.45f : 1.0f);
		if (resetHot) draw->AddRectFilled(ImVec2(reset.x0, reset.y0), ImVec2(reset.x1, reset.y1), ToU32(Rgba(255, 255, 255, 20)), 3 * s);
		const ImVec2 c((reset.x0 + reset.x1) * 0.5f, (reset.y0 + reset.y1) * 0.5f);
		const float r = 5.5f * s, a0 = -0.15f * 3.14159265f, a1 = 1.35f * 3.14159265f;
		draw->PathArcTo(c, r, a0, a1, 18);
		draw->PathStroke(ToU32(ink), 0, 1.6f * s);
		const ImVec2 tip(c.x + r * std::cos(a0), c.y + r * std::sin(a0));
		draw->AddTriangleFilled(ImVec2(tip.x - 3.5f * s, tip.y - 1 * s), ImVec2(tip.x + 2.5f * s, tip.y - 3.5f * s), ImVec2(tip.x + 1.5f * s, tip.y + 2.5f * s),
			ToU32(ink));
	}
	{
		const ImVec2 c((chev.x0 + chev.x1) * 0.5f, midY);
		const ImU32 ink = ToU32(chevHot || headHot ? Theme::Text : Theme::TextDim);
		if (open) draw->AddTriangleFilled(ImVec2(c.x - 4 * s, c.y + 2 * s), ImVec2(c.x + 4 * s, c.y + 2 * s), ImVec2(c.x, c.y - 3 * s), ink);
		else draw->AddTriangleFilled(ImVec2(c.x - 4 * s, c.y - 2 * s), ImVec2(c.x + 4 * s, c.y - 2 * s), ImVec2(c.x, c.y + 3 * s), ink);
	}
	HoverRect(p, eye, m.pivotMarkers
		? (index == 0 ? "Shows an amber marker in the world at the point this effect rotates the weapon around. Switching it off also "
		                "hides the marker that flashes after each edit."
		              : "Shows a cyan marker in the world at the point this effect rotates the weapon around. Switching it off also "
		                "hides the marker that flashes after each edit.")
		: "Shows a marker in the world at the point this effect rotates the weapon around. Counter-Strike: Source does not draw it yet.");
	HoverRect(p, reset, "Reset Pivots: puts every weapon class back on its shipped hinge and switches the marker off.");
	p.c.y = head.y1;

	if (open) {
		const float x0 = p.c.x0, x1 = p.c.x1;
		p.c.x0 += 8 * s;
		p.c.x1 -= 8 * s;
		p.c.y += 2 * s;
		bool cells[7];
		for (int i = 0; i < 7; ++i) cells[i] = i == live;
		Segmented(p.c, "classes", kPivotNames, 7, live, cells);
		const char* scope = live >= 0 ? kPivotScopes[live] : "";
		const double* h = live >= 0 ? hinge : pv.hinge[0];
		PivotSlider(p, "fwd", "Forward",
			"How far in front of the eye the weapon rotates from. Larger swings the muzzle more through a turn. Negative puts the hinge "
			"behind the eye, which suits a weapon held at the shoulder.",
			h[0], live >= 0, owner, scope, "fwd", colour);
		PivotSlider(p, "right", "Right", "Sideways position of the hinge. Positive is toward the right hand.", h[1], live >= 0, owner, scope,
			"right", colour);
		PivotSlider(p, "up", "Up", "Vertical position of the hinge. Negative is below the eye, where a held weapon sits.", h[2], live >= 0,
			owner, scope, "up", colour);
		p.c.y += 8 * s;
		p.c.x0 = x0;
		p.c.x1 = x1;
	}

	draw->ChannelsSetCurrent(0);
	draw->AddRectFilled(ImVec2(p.c.x0, top), ImVec2(p.c.x1, p.c.y), ToU32(Rgba(0, 0, 0, 90)), 3 * s);
	if (headHot || chevHot) draw->AddRectFilled(ImVec2(head.x0, head.y0), ImVec2(head.x1, head.y1), ToU32(Rgba(255, 255, 255, 8)), 3 * s);
	draw->AddRect(ImVec2(p.c.x0, top), ImVec2(p.c.x1, p.c.y), ToU32(Rgba(255, 255, 255, 20)), 3 * s);
	draw->ChannelsMerge();
}

// Weapon Recoil's Preset row: a list of the saved tunings and Save, which turns the field into a
// name to type. Enter or Save saves it, Esc or a click away leaves it.
int PresetNameFilter(ImGuiInputTextCallbackData* data) {
	const ImWchar ch = data->EventChar;
	const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == ' ' || ch == '-'
		|| ch == '.' || ch == '_';
	return ok ? 0 : 1;
}

std::string Trimmed(const char* text) {
	std::string out = text;
	while (!out.empty() && out.back() == ' ') out.pop_back();
	size_t i = 0;
	while (i < out.size() && out[i] == ' ') ++i;
	return out.substr(i);
}

void PresetRow(Pane& p, const ConfigModel& m) {
	ConfigState& st = *p.st;
	const float s = p.c.s;
	if (!st.naming) {
		const std::string shown = !st.preset.empty() ? st.preset : (m.weaponRecoilDefault ? "Default tuning" : "Custom tuning");
		const DropClick d = DropRow(p.c, "preset", "Preset", shown.c_str(), Theme::Text, "Save", true);
		if (d.field) {
			st.openDrop = st.openDrop == 3 ? -1 : 3;
			st.dropField = d.fieldRect;
		}
		if (st.openDrop == 3) st.dropField = d.fieldRect;
		if (d.side) {
			st.naming = true;
			st.nameFocus = true;
			st.name[0] = 0;
			st.openDrop = -1;
		}
		return;
	}
	ImFont* labelFont = p.c.Font(FontRole::RowLabel);
	ImFont* valueFont = p.c.Font(FontRole::Value);
	ImFont* buttonFont = p.c.Font(FontRole::Button);
	const float pad = Metrics::line.padH * s, labelW = Metrics::line.label * s, colGap = Metrics::line.colGap * s;
	const float fieldH = 30 * s, saveW = CardButtonWidth("Save", buttonFont, s);
	p.c.y += (Metrics::line.gap + 1) * s;
	const Rect field = MakeRect(p.c.x0 + pad + labelW + colGap, p.c.y, p.c.x1 - saveW - 6 * s, p.c.y + fieldH);
	const Rect save = MakeRect(p.c.x1 - saveW, p.c.y, p.c.x1, p.c.y + fieldH);
	ImGui::GetWindowDrawList()->AddText(labelFont, labelFont->FontSize,
		ImVec2(p.c.x0 + pad, std::floor(p.c.y + (fieldH - labelFont->FontSize) * 0.5f)), ToU32(Theme::Text), "Preset");
	// Save first, so a click on it is not taken for a click away from the name.
	const bool saveClicked = CardButton("presetsave", "Save", save, ButtonKind::Primary, true, buttonFont);
	ImGui::SetCursorScreenPos(ImVec2(field.x0, field.y0));
	ImGui::PushFont(valueFont);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ToU32(Rgba(5, 7, 10)));
	ImGui::PushStyleColor(ImGuiCol_Border, ToU32(Theme::Accent));
	ImGui::PushStyleColor(ImGuiCol_Text, ToU32(Theme::Text));
	ImGui::PushStyleColor(ImGuiCol_TextDisabled, ToU32(Theme::TextDim));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3 * s);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * s, (std::max)(0.0f, (fieldH - valueFont->FontSize) * 0.5f)));
	ImGui::SetNextItemWidth(field.W());
	if (st.nameFocus) {
		ImGui::SetKeyboardFocusHere();
		st.nameFocus = false;
	}
	const bool enter = ImGui::InputTextWithHint("##presetname", "Type a name, press Enter to save", st.name, sizeof(st.name),
		ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter, PresetNameFilter);
	const bool left = ImGui::IsItemDeactivated();
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(4);
	ImGui::PopFont();
	const std::string name = Trimmed(st.name);
	if ((enter || saveClicked) && !name.empty()) {
		Send(p, "mirv_filmmaker weaponrecoil preset save \"" + name + "\"");
		st.preset = name;
		st.naming = false;
	} else if (saveClicked) {
		st.nameFocus = true; // nothing typed yet: keep the name open
	} else if (left && !enter && !ImGui::IsMouseHoveringRect(ImVec2(save.x0, save.y0), ImVec2(save.x1, save.y1))) {
		st.naming = false;
	}
	p.c.y += fieldH;
}

void WeaponRecoilCard(Pane& p, const ConfigModel& m) {
	const float s = p.c.s;
	{
		const CardAction buttons[] = { { "Test Shot", ButtonKind::Solid, true }, { "Reset", ButtonKind::Normal, !m.weaponRecoilDefault } };
		CardSwitch master;
		master.show = true;
		master.on = m.wrOn;
		switch (CardHeader(p.c, "weaponrecoil", "Weapon Recoil", WeaponRecoilSummary(m).c_str(), buttons, 2, kRecoilAmber, master)) {
		case 0: Send(p, "mirv_filmmaker weaponrecoil test"); break;
		case 1:
			Send(p, "mirv_filmmaker weaponrecoil reset quiet");
			Send(p, "mirv_filmmaker pivot recoil reset");
			Send(p, "mirv_filmmaker pivotorb recoil off quiet");
			break;
		case 2: Send(p, "mirv_filmmaker weaponrecoil toggle"); break;
		default: break;
		}
	}
	if (!m.viewmodelChannel)
		Note(p.c, "Counter-Strike: Source cannot move the weapon on its own yet, so only the CAMERA SHAKE part of the kick shows. "
			"Test Shot (or T) fires one kick.");

	// Applies to and Style, side by side.
	int kind = 0;
	for (int i = 0; i < 3; ++i)
		if (m.wrScope == kScopeKinds[i]) kind = i;
	{
		p.c.y += 8 * s;
		const float half = (p.c.W() - 10 * s) * 0.5f;
		CardColumn left = p.c, right = p.c;
		left.x1 = p.c.x0 + half;
		right.x0 = left.x1 + 10 * s;
		const float y0 = p.c.y;
		Caption(left, "Applies to");
		const int pickKind = Segmented(left, "scopekind", kScopeKindNames, 3, kind);
		Caption(right, "Style");
		const char* styles[2] = { "Springy", "Snappy" };
		const int pickStyle = Segmented(right, "style", styles, 2, m.wrStyle == "style2" ? 1 : 0);
		HoverRect(p, MakeRect(right.x0, y0, right.x1, right.y),
			"Springy: a heavier punch-led kick that rides up and settles back. Snappy: a sharper kick that throws the gun wider and "
			"rolls it more.");
		p.c.y = (std::max)(left.y, right.y);
		if (pickKind == 0) {
			Send(p, "mirv_filmmaker weaponrecoil scope all quiet");
		} else if (pickKind > 0) {
			// The live gun's slot or type when the list has it, else the list's first.
			const char* const* keys = pickKind == 1 ? kSlotKeys : kTypeKeys;
			const int n = pickKind == 1 ? 2 : 6;
			const std::string& liveKey = pickKind == 1 ? m.wrLiveSlot : m.wrLiveType;
			const char* key = keys[0];
			for (int i = 0; i < n; ++i)
				if (liveKey == keys[i]) key = keys[i];
			Send(p, Format("mirv_filmmaker weaponrecoil scope %s %s quiet", kScopeKinds[pickKind], key));
		}
		if (pickStyle >= 0) Send(p, Format("mirv_filmmaker weaponrecoil style %s quiet", pickStyle == 1 ? "style2" : "style1"));
	}
	if (kind != 0) {
		const char* const* keys = kind == 1 ? kSlotKeys : kTypeKeys;
		const char* const* names = kind == 1 ? kSlotNames : kTypeNames;
		const int n = kind == 1 ? 2 : 6;
		int sel = -1;
		for (int i = 0; i < n; ++i)
			if (m.wrScopeKey == keys[i]) sel = i;
		p.c.y += 6 * s;
		const int pick = CellRow(p.c, "scopekey", names, n, sel);
		if (pick >= 0) Send(p, Format("mirv_filmmaker weaponrecoil scope %s %s quiet", kScopeKinds[kind], keys[pick]));
	}
	{
		// What the selected scope reaches, in words.
		std::string live = m.wrLive;
		for (char& ch : live)
			if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
		const std::string inUse = live.empty() ? std::string() : " Gun in shot: " + live + ".";
		if (kind != 0 && !live.empty() && !m.wrScopeCovers) {
			Note(p.c, ("This scope does not cover the gun in shot (" + live + "). Changes here will not show until a matching weapon fires.").c_str(),
				Theme::Accent);
		} else if (kind == 0) {
			const std::string overrides = m.wrOverrides > 0 ? Format(" %d override%s set.", m.wrOverrides, m.wrOverrides == 1 ? "" : "s") : "";
			Note(p.c, ("All weapons." + overrides + inUse).c_str());
		} else {
			Note(p.c, ((m.wrScopeOwn ? "Editing settings that belong to this scope." : "Inheriting. Move a slider to give this scope its own settings.")
				+ inUse).c_str());
		}
	}
	if (kind != 0 && m.wrScopeOwn) {
		ImFont* buttonFont = p.c.Font(FontRole::Button);
		p.c.y += 4 * s;
		if (CardButton("clearoverride", "Clear Override", MakeRect(p.c.x0, p.c.y, p.c.x1, p.c.y + buttonFont->FontSize + 14 * s), ButtonKind::Normal,
			true, buttonFont))
			Send(p, "mirv_filmmaker weaponrecoil scopeclear");
		p.c.y += buttonFont->FontSize + 14 * s;
	}
	PresetRow(p, m);

	Group(p, "Weapon");
	Slider(p, "amount", "Amount", "Master strength of the kick on every shot.", "mirv_filmmaker weaponrecoil amount", m.wrAmount, 0, 150, 0, "%",
		true);
	Slider(p, "punch", "Punch",
		"How far the gun kicks straight backward per shot. Higher values pull it farther back; it returns to rest without bouncing forward.",
		"mirv_filmmaker weaponrecoil punch", m.wrPunch, 0, 150, 0, "%", true);
	Slider(p, "rotation", "Rotation", "Muzzle climb, sideways yaw, and roll around the barrel per shot.", "mirv_filmmaker weaponrecoil rotation",
		m.wrRot, 0, 150, 0, "%", true);
	Slider(p, "snap", "Snappiness", "How hard the spring yanks the gun back to rest. Higher is a sharper, faster kick.",
		"mirv_filmmaker weaponrecoil snap", m.wrSnap, 25, 400, 0, "%", false);
	Slider(p, "recovery", "Recovery", "How firmly the gun springs back. Lower overshoots and wobbles more before settling.",
		"mirv_filmmaker weaponrecoil recovery", m.wrRecovery, 25, 400, 0, "%", false);
	Group(p, "Camera shake");
	Note(p.c, "Shares of the kick, not extra kick: higher moves the view and steadies the gun.");
	Slider(p, "camerarot", "Rotation", "How much of the muzzle rise and sideways yaw the camera takes. MW sits around 15-30.",
		"mirv_filmmaker weaponrecoil camerarot", m.wrCamRot, 0, 100, 0, "%", false);
	Slider(p, "camerapos", "Punch",
		"How much of the backward kick moves the camera with it. Higher means the view takes the punch and the weapon moves less on screen, "
		"not that the punch gets bigger.",
		"mirv_filmmaker weaponrecoil camerapos", m.wrCamPos, 0, 100, 0, "%", false);
	Slider(p, "cameraroll", "Barrel Roll",
		"How much of the roll around the barrel axis reaches the camera. The first one to lower if the screen feels like it is tipping.",
		"mirv_filmmaker weaponrecoil cameraroll", m.wrCamRoll, 0, 100, 0, "%", false);
	PivotDrawer(p, m, m.pvr, 0);
}

void WeaponBobCard(Pane& p, const ConfigModel& m) {
	const char* reset[] = { "mirv_filmmaker weaponbob reset", "mirv_filmmaker pivot bob reset", "mirv_filmmaker pivotorb bob off quiet" };
	Header(p, "weaponbob", "Weapon Bob", WeaponBobSummary(m), kBobCyan, m.wbOn, m.weaponBobDefault, reset, 3,
		"mirv_filmmaker weaponbob toggle");
	if (!m.viewmodelChannel)
		Note(p.c, "Counter-Strike: Source cannot move the weapon on its own yet, so only the Camera Share part of this motion shows.");
	Group(p, "Walk");
	Slider(p, "amount", "Amount", "Master intensity for everything in this card. The game adds a little movement of its own underneath.",
		"mirv_filmmaker weaponbob amount", m.wbAmount, 0, 150, 0, "%", true);
	Slider(p, "sway", "Sway", "How far the gun travels: the side-to-side, up-down and in-out motion of the walk cycle.",
		"mirv_filmmaker weaponbob sway", m.wbSway, 0, 150, 0, "%", true);
	Slider(p, "tilt", "Tilt",
		"How much the gun rocks and rolls as it moves. This is most of the character; raise it for a heavier, looser weapon.",
		"mirv_filmmaker weaponbob tilt", m.wbTilt, 0, 150, 0, "%", true);
	Slider(p, "rate", "Cadence", "Speed of the walk cycle without changing how far the gun moves. Lower reads as a heavier weapon.",
		"mirv_filmmaker weaponbob rate", m.wbRate, 25, 400, 0, "%", false);
	Slider(p, "stepsync", "Step Sync",
		"Pulls the cycle onto real footsteps so the gun dips as each foot lands, instead of drifting against them.",
		"mirv_filmmaker weaponbob stepsync", m.wbStep, 0, 100, 0, "%", true);
	Group(p, "Look");
	Slider(p, "looksway", "Look Sway",
		"The gun lags and rotates behind the aim as the player looks around, hinged on the weapon rather than the camera.",
		"mirv_filmmaker weaponbob looksway", m.wbLook, 0, 150, 0, "%", true);
	Slider(p, "inertia", "Inertia",
		"A slower, heavier overshoot layered on the look sway. Raise it for a weapon that keeps swinging after a turn stops.",
		"mirv_filmmaker weaponbob inertia", m.wbInertia, 0, 150, 0, "%", true);
	Slider(p, "idle", "Idle Breath", "Slow drift while standing still so the gun is never pinned in place.", "mirv_filmmaker weaponbob idle",
		m.wbIdle, 0, 150, 0, "%", true);
	Slider(p, "camera", "Camera Share",
		"How much of the weapon motion the camera also picks up. Keep this low: high values make the whole view move with the gun.",
		"mirv_filmmaker weaponbob camera", m.wbCam, 0, 100, 0, "%", true);
	PivotDrawer(p, m, m.pvb, 1);
}

#include "ConfigLensCards.h"

// ---- the rail --------------------------------------------------------------------------------------

void Rail(ConfigState& st, const ConfigModel& m, const FontSet& fonts, float s, const Rect& rail, std::vector<std::string>& out) {
	ImGui::GetWindowDrawList()->AddLine(ImVec2(rail.x1, rail.y0), ImVec2(rail.x1, rail.y1), ToU32(Rgba(255, 255, 255, 18)), 1.0f);
	const Rect inner = MakeRect(rail.x0, rail.y0, rail.x1 - Metrics::rail.padR * s, rail.y1);
	float width = inner.W();
	CardColumn c;
	c.fonts = &fonts;
	c.s = s;
	c.x0 = inner.x0;
	c.y = BeginCardScroll("##cfgrail", inner, width);
	c.x1 = inner.x0 + width;
	struct Entry {
		ConfigCard card;
		const char* group;
		const char* title;
		Color edge;
		bool on;
		std::string summary;
		const char* toggle;
	};
	const Entry entries[] = {
		{ ConfigCard::SmoothView, "Motion", "Smooth View", kMotion, m.svOn, SmoothViewSummary(m), "mirv_filmmaker smoothview toggle" },
		{ ConfigCard::Handheld, "Motion", "Handheld Camera", kMotion, m.handheldOn, HandheldSummary(m), "mirv_filmmaker handheld toggle" },
		{ ConfigCard::WeaponRecoil, "Weapon", "Weapon Recoil", kRecoilAmber, m.wrOn, WeaponRecoilSummary(m), "mirv_filmmaker weaponrecoil toggle" },
		{ ConfigCard::WeaponBob, "Weapon", "Weapon Bob", kBobCyan, m.wbOn, WeaponBobSummary(m), "mirv_filmmaker weaponbob toggle" },
        { ConfigCard::DepthOfField, "Lens", "Depth of field", kDof, m.lens[Lx::DofOn]>.5f, LensSummary(m,false), "mirv_filmmaker lens dof toggle quiet" },
        { ConfigCard::LensFlare, "Lens", "Lens flare", kFlare, m.lens[Lx::FlareOn]>.5f, LensSummary(m,true), "mirv_filmmaker lens flare toggle quiet" },
	};
	const char* group = nullptr;
	for (const Entry& e : entries) {
		if (!group || 0 != std::strcmp(group, e.group)) {
			RailGroup(c, e.group, group == nullptr);
			group = e.group;
		}
		CardSwitch master;
		master.show = true;
		master.on = e.on;
		ImGui::PushID((int)e.card);
		const RailHit hit = RailRow(c, "card", e.title, e.summary.c_str(), st.card == e.card, e.edge, master);
		ImGui::PopID();
		if (hit == RailHit::Row) {
			st.card = e.card;
			st.openDrop = -1;
			st.naming = false;
		} else if (hit == RailHit::Switch) {
			out.push_back(e.toggle);
		}
	}
	EndCardScroll(c.x0, c.y + 8 * s);
}

} // namespace

void DrawConfigInspector(ConfigState& state, const ConfigModel& model, const FontSet& fonts, float s, const Rect& body,
	std::vector<std::string>& commands) {
	// The pinned footer: Keep between demos at the left while a demo plays, RESET CAMERA at the right.
	ImFont* button = fonts.Get(FontRole::Button);
	const float footH = button->FontSize + 14 * s;
	const Rect foot = MakeRect(body.x0, body.y1 - 2 * s - footH, body.x1, body.y1 - 2 * s);
	const char* footTip = nullptr;
	Rect footTipRect;
	{
		const char* caption = "RESET CAMERA";
		const float w = CardButtonWidth(caption, button, s) + 2 * s;
		if (CardButton("##cfgresetcamera", caption, MakeRect(foot.x1 - w, foot.y0, foot.x1, foot.y1), ButtonKind::Danger, true, button))
			commands.push_back("mirv_filmmaker effects reset camera");
		if (model.demo) {
			// The whole row is the switch (CS2's keep toggle): a small switch and its caption.
			ImFont* label = fonts.Get(FontRole::RowLabel);
			const char* text = "Keep between demos";
			const float sw = Metrics::swSm.w * s, sh = Metrics::swSm.h * s;
			const float textW = label->CalcTextSizeA(label->FontSize, FLT_MAX, 0, text).x;
			const Rect row = MakeRect(foot.x0, foot.y0, (std::min)(foot.x0 + sw + 10 * s + textW, foot.x1 - w - 10 * s), foot.y1);
			ImGui::SetCursorScreenPos(ImVec2(row.x0, row.y0));
			const bool clicked = ImGui::InvisibleButton("##cfgkeepcamera", ImVec2((std::max)(1.0f, row.W()), row.H()));
			const bool hot = ImGui::IsItemHovered();
			ImDrawList* draw = ImGui::GetWindowDrawList();
			const float cy = (row.y0 + row.y1) * 0.5f;
			const Rect track = MakeRect(row.x0, cy - sh * 0.5f, row.x0 + sw, cy + sh * 0.5f);
			const bool on = model.keepCamera;
			draw->AddRectFilled(ImVec2(track.x0, track.y0), ImVec2(track.x1, track.y1), ToU32(on ? Rgba(255, 176, 0, 46) : Rgba(0, 0, 0, 0)));
			draw->AddRect(ImVec2(track.x0, track.y0), ImVec2(track.x1, track.y1), ToU32(on ? Theme::Accent : (hot ? Theme::Text : Theme::Border)));
			const float knob = Metrics::swSm.knob * s, inset = (sh - knob) * 0.5f, kx = on ? track.x1 - inset - knob : track.x0 + inset;
			draw->AddRectFilled(ImVec2(kx, track.y0 + inset), ImVec2(kx + knob, track.y0 + inset + knob), ToU32(on ? Theme::Accent : Theme::TextDim));
			draw->AddText(label, label->FontSize, ImVec2(track.x1 + 10 * s, std::floor(cy - label->FontSize * 0.5f)), ToU32(Theme::Text), text);
			if (clicked) commands.push_back(on ? "mirv_filmmaker effects keep camera off" : "mirv_filmmaker effects keep camera on");
			if (hot) {
				footTip = "Carries camera motion and lens effects into the next demo you load. Off, each new demo starts "
				          "from the defaults.";
				footTipRect = row;
			}
		}
	}

	// The rail and the selected card above it.
	const float top = body.y0 + 14 * s, bottom = foot.y0 - 22 * s;
	const Rect rail = MakeRect(body.x0, top, body.x0 + Metrics::rail.w * s, bottom);
	const Rect paneRect = MakeRect(rail.x1 + Metrics::rail.gap * s, top, body.x1, bottom);
	Rail(state, model, fonts, s, rail, commands);

	float width = paneRect.W();
	Pane p;
	p.c.fonts = &fonts;
	p.c.s = s;
	p.c.x0 = paneRect.x0;
	p.c.y = BeginCardScroll("##cfgpane", paneRect, width);
	p.c.x1 = paneRect.x0 + width;
	p.st = &state;
	p.out = &commands;
	ImGui::PushID((int)state.card);
	switch (state.card) {
	case ConfigCard::Handheld: HandheldCard(p, model); break;
	case ConfigCard::WeaponRecoil: WeaponRecoilCard(p, model); break;
	case ConfigCard::WeaponBob: WeaponBobCard(p, model); break;
    case ConfigCard::DepthOfField: LensDofCard(p,model); break;
    case ConfigCard::LensFlare: LensFlareCard(p,model); break;
	default: SmoothViewCard(p, model); break;
	}
	ImGui::PopID();
	EndCardScroll(p.c.x0, p.c.y + 10 * s);

	// The tooltip waits on one row; a new row starts the wait again.
	if (footTip) {
		p.tip = footTip;
		p.tipRect = footTipRect;
	}
	const std::string tip = p.tip ? p.tip : "";
	if (tip != state.tipText) {
		state.tipText = tip;
		state.tipSince = tip.empty() ? -1 : ImGui::GetTime();
	}
	state.tipAnchor = p.tipRect;
}

void DrawConfigOverlays(ConfigState& state, const ConfigModel& model, const FontSet& fonts, float s, float clientW, float clientH,
	std::vector<std::string>& commands) {
	if (state.openDrop == 3 && state.card == ConfigCard::WeaponRecoil) {
		std::vector<DropOption> options;
		for (const ConfigPreset& preset : model.wrPresets) {
			DropOption o;
			o.text = preset.name + "  " MIDDOT "  "
				+ (preset.scope == "slot" ? "Slot: " + preset.key : (preset.scope == "type" ? "Type: " + preset.key : std::string("All")));
			o.selected = preset.name == state.preset;
			options.push_back(o);
		}
		bool closed = false;
		if (options.empty()) {
			DropOption o;
			o.text = "No saved presets yet. Press Save to name the current tuning.";
			o.color = Theme::TextDim;
			options.push_back(o);
			DropList("##cfgpresets", state.dropField, options, fonts, s, clientW, clientH, closed);
		} else {
			int deleted = -1;
			const int picked = DropList("##cfgpresets", state.dropField, options, fonts, s, clientW, clientH, closed, deleted);
			if (deleted >= 0) commands.push_back("mirv_filmmaker weaponrecoil preset delete \"" + model.wrPresets[(size_t)deleted].name + "\"");
			else if (picked >= 0) {
				commands.push_back("mirv_filmmaker weaponrecoil preset load \"" + model.wrPresets[(size_t)picked].name + "\"");
				state.preset = model.wrPresets[(size_t)picked].name;
			}
		}
		if (closed) state.openDrop = -1;
	} else if (state.openDrop >= 0 && state.openDrop < 3 && state.card == ConfigCard::Handheld) {
		const std::string& current = CurveOf(model, state.openDrop);
		std::vector<DropOption> options;
		for (const Curve& c : kCurves) {
			DropOption o;
			o.text = c.caption;
			o.selected = current == c.key;
			options.push_back(o);
		}
		bool closed = false;
		const int picked = DropList("##cfgcurves", state.dropField, options, fonts, s, clientW, clientH, closed);
		if (picked >= 0 && picked < kCurveCount)
			commands.push_back(Format("mirv_filmmaker handheld preset %s %s quiet", kCurveStates[state.openDrop], kCurves[picked].key));
		if (closed) state.openDrop = -1;
	} else {
		state.openDrop = -1;
	}

	// CS2's tooltip: 300 wide, left of the row (right of it when there is no room), after 0.18 s,
	// never while a button is down.
	if (state.tipText.empty() || state.tipSince < 0 || state.openDrop >= 0) return;
	if (ImGui::GetTime() - state.tipSince < 0.18 || ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
	ImFont* font = fonts.Get(FontRole::Note);
	const float w = 300 * s, padX = 12 * s, padTop = 9 * s, padBottom = 10 * s;
	const float textH = font->CalcTextSizeA(font->FontSize, FLT_MAX, w - 2 * padX, state.tipText.c_str()).y;
	const float h = padTop + textH + padBottom;
	const Rect& a = state.tipAnchor;
	float x = a.x0 - w - 16 * s;
	if (x < 2) x = a.x1 + 12 * s;
	float y = a.y0;
	if (y + h > clientH - 2) y = clientH - 2 - h;
	if (x + w > clientW - 2) x = clientW - 2 - w;
	ImDrawList* draw = ImGui::GetForegroundDrawList();
	draw->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), ToU32(Theme::DockBg), 5 * s);
	draw->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), ToU32(Theme::Border), 5 * s);
	draw->AddText(font, font->FontSize, ImVec2(x + padX, y + padTop), ToU32(Theme::Text), state.tipText.c_str(), nullptr, w - 2 * padX);
}

bool ConfigModal(const ConfigState& state) { return state.openDrop >= 0; }

bool ConfigEscape(ConfigState& state) {
	if (state.naming) {
		state.naming = false;
		return true;
	}
	if (state.openDrop < 0) return false;
	state.openDrop = -1;
	return true;
}

const char* ConfigCardName(ConfigCard card) {
	switch (card) {
	case ConfigCard::SmoothView: return "smoothview";
	case ConfigCard::Handheld: return "handheld";
	case ConfigCard::WeaponRecoil: return "weaponrecoil";
	case ConfigCard::WeaponBob: return "weaponbob";
    case ConfigCard::DepthOfField: return "dof";
    case ConfigCard::LensFlare: return "flare";
	default: return "?";
	}
}

} // namespace Ui
} // namespace SourceFilmmaker
