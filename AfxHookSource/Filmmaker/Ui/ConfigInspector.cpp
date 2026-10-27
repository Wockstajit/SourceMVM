#include "ConfigInspector.h"

#include "UiWidgets.h"

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
constexpr Color kBobCyan = Rgba(0x5f, 0xd0, 0xc0);

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
	return "Breathing " MIDDOT " tilt " + tilt + " " MIDDOT " effects " + (m.svFalling || m.svJumping ? "on" : "off");
}

std::string HandheldSummary(const ConfigModel& m) {
	if (!m.handheldOn) return "Off";
	return Format("Drift %ld%% " MIDDOT " curves %ld%% " MIDDOT " lock %ld%%", Round(m.hhDrift), Round(m.hhCurve), Round(m.hhCenterLock));
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
	const char* reset[] = { "mirv_filmmaker smoothview reset" };
	Header(p, "smoothview", "Smooth View", SmoothViewSummary(m), kMotion, m.svOn, m.smoothViewDefault, reset, 1,
		"mirv_filmmaker smoothview toggle");
	Group(p, "View");
	Toggle(p, "falling", "Fall pitch", "Pitches the camera during a fall.", m.svFalling, "mirv_filmmaker smoothview falling toggle");
	Toggle(p, "jumping", "Jump kick", "Kicks the camera on take-off and landing.", m.svJumping, "mirv_filmmaker smoothview jumping toggle");
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
}

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
		{ ConfigCard::WeaponBob, "Weapon", "Weapon Bob", kBobCyan, m.wbOn, WeaponBobSummary(m), "mirv_filmmaker weaponbob toggle" },
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
		} else if (hit == RailHit::Switch) {
			out.push_back(e.toggle);
		}
	}
	EndCardScroll(c.x0, c.y + 8 * s);
}

} // namespace

void DrawConfigInspector(ConfigState& state, const ConfigModel& model, const FontSet& fonts, float s, const Rect& body,
	std::vector<std::string>& commands) {
	// The pinned footer: RESET CAMERA at the right.
	ImFont* button = fonts.Get(FontRole::Button);
	const float footH = button->FontSize + 14 * s;
	const Rect foot = MakeRect(body.x0, body.y1 - 2 * s - footH, body.x1, body.y1 - 2 * s);
	{
		const char* caption = "RESET CAMERA";
		const float w = CardButtonWidth(caption, button, s) + 2 * s;
		if (CardButton("##cfgresetcamera", caption, MakeRect(foot.x1 - w, foot.y0, foot.x1, foot.y1), ButtonKind::Danger, true, button))
			commands.push_back("mirv_filmmaker effects reset camera");
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
	case ConfigCard::WeaponBob: WeaponBobCard(p, model); break;
	default: SmoothViewCard(p, model); break;
	}
	ImGui::PopID();
	EndCardScroll(p.c.x0, p.c.y + 10 * s);

	// The tooltip waits on one row; a new row starts the wait again.
	const std::string tip = p.tip ? p.tip : "";
	if (tip != state.tipText) {
		state.tipText = tip;
		state.tipSince = tip.empty() ? -1 : ImGui::GetTime();
	}
	state.tipAnchor = p.tipRect;
}

void DrawConfigOverlays(ConfigState& state, const ConfigModel& model, const FontSet& fonts, float s, float clientW, float clientH,
	std::vector<std::string>& commands) {
	if (state.openDrop >= 0 && state.card == ConfigCard::Handheld) {
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
	if (state.openDrop < 0) return false;
	state.openDrop = -1;
	return true;
}

} // namespace Ui
} // namespace SourceFilmmaker
