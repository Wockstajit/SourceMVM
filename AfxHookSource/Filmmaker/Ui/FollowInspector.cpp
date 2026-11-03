#include "FollowInspector.h"

#include "UiWidgets.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace SourceFilmmaker {
namespace Ui {

namespace {

// UTF-8 for the non-ASCII characters CS2's copy uses.
#define MIDDOT "\xC2\xB7"
#define DEGREE "\xC2\xB0"
#define EMDASH "\xE2\x80\x94"

// CS2's channel colours (CameraEditorJs.h S.axis), and its follow inspector's own: the live line,
// the LIVE badge, the yellow warning and far rows, Attached's green diagnostics.
const Color kAxis[7] = {
	Rgba(0xff, 0x88, 0x85, 0xcc), Rgba(0x80, 0xd6, 0x80, 0xcc), Rgba(0x69, 0xa7, 0xff, 0xcc), Rgba(0xfd, 0x96, 0xd7, 0xcc),
	Rgba(0xc8, 0x9c, 0xff, 0xcc), Rgba(0x25, 0xc8, 0xff, 0xcc), Rgba(0xff, 0x9a, 0x5a, 0xcc),
};
constexpr Color kLive = Rgba(0x35, 0xcc, 0x59);
constexpr Color kLiveBadge = Rgba(0x4c, 0xd9, 0x6d);
constexpr Color kWarn = Rgba(0xf4, 0xc9, 0x5d);
constexpr Color kDebug = Rgba(0x9e, 0xff, 0xa0);
// The pinned bar's ground and edge (CS2's #11151b and #ffffff14).
constexpr Color kBarBg = Rgba(0x11, 0x15, 0x1b);
constexpr Color kBarBorder = Rgba(255, 255, 255, 20);

const char* const kCameraModes[] = { "Lock-on", "Attached" };
const char* const kDeathModes[] = { "Keep following", "Timed hold" };
const char* const kShotPages[] = { "FRAMING", "TRACKING" };

enum Drop { kDropType, kDropPlayer, kDropSource, kDropTarget, kDropMount, kDropEvent };

std::string Format(const char* fmt, ...) {
	char buf[512];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	return buf;
}

std::string Command(const char* fmt, ...) {
	char buf[256];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	return std::string("mirv_filmmaker follow ") + buf;
}

bool Attached(const FollowInspectorModel& m) { return m.mode == 1; }

bool HaveTarget(const FollowInspectorModel& m) { return m.targetIndex >= 0 || (m.type == 2 && m.weaponPlayerIndex >= 0); }

// The cards' summaries, also their rail rows' (CS2's __note).
std::string TargetSummary(const FollowInspectorModel& m) {
	const bool attach = Attached(m);
	const char* typeWord = attach ? (m.type == 1 ? "Grenade" : (m.type == 2 ? "Weapon attach" : "Player bone"))
	                              : (m.type == 1 ? "Grenade" : (m.type == 2 ? "Weapon / C4" : "Player"));
	std::string out = std::string(attach ? "Attached" : "Lock-on") + " " MIDDOT " ";
	out += HaveTarget(m) ? (m.targetName.empty() ? std::string(typeWord) : m.targetName) : std::string("no target");
	// CS2 reads a distance it is never sent here, so its line never shows it; this one does.
	if (HaveTarget(m) && !attach && m.targetDistance > 0) out += Format(" " MIDDOT " %du", (int)std::lround(m.targetDistance));
	return out;
}

std::string ShotSummary(const FollowInspectorModel& m) {
	return Format("Offset %.0f / %.0f / %.0f " MIDDOT " FOV %.0f " MIDDOT " %s " MIDDOT " %s", m.offset[0], m.offset[1], m.offset[2],
		m.fov, Attached(m) ? "rigid mount" : Format("smoothing %.2fs", m.look).c_str(), m.autoDead ? "timed hold" : "keep on death");
}

std::string CandidateText(const FollowInspectorModel& m, const FollowInspectorCandidate& c) {
	if (m.type == 1) {
		const std::string delta = c.tickDelta >= 0 ? Format("+%d", c.tickDelta) : Format("%d", c.tickDelta);
		return (c.grenadeType.empty() ? c.name : c.grenadeType) + " " MIDDOT " " + (c.ownerName.empty() ? "unknown thrower" : c.ownerName)
			+ (c.team != "-" ? " " MIDDOT " " + c.team : "") + Format(" " MIDDOT " tick %d (", c.throwTick) + delta + ") " MIDDOT " "
			+ (c.status.empty() ? "nearby" : c.status);
	}
	// The engine class trails weapons: several entities share one friendly name, so the class is
	// what says which one a pick follows.
	const std::string cls = c.className.empty() ? "" : " " MIDDOT " " + c.className;
	if (c.isWeapon || c.isBomb)
		return c.name + " " MIDDOT " " + (c.status.empty() ? (c.dropped ? "dropped" : "held") : c.status) + cls;
	return c.name + (!c.team.empty() && c.team != "-" ? " " MIDDOT " " + c.team : "") + (m.type == 0 && !c.alive ? " " MIDDOT " dead" : "")
		+ Format(" " MIDDOT " %du", (int)std::lround(c.distance));
}

std::string EventText(const FollowInspectorEvent& e) {
	std::string label = e.type;
	if (label == "weapon_drop") label = "death drop";
	else if (label == "bomb_dropped") label = "C4 drop";
	else if (label == "bomb_pickup") label = "C4 pickup";
	else if (label == "bomb_planted") label = "C4 plant";
	else if (label == "item_pickup") label = "pickup";
	return Format("tick %d  ", e.tick) + label + (e.item.empty() ? "" : "  " + e.item) + (e.ownerName.empty() ? "" : "  " + e.ownerName);
}

bool CandidateSelected(const FollowInspectorModel& m, const FollowInspectorCandidate& c) {
	return m.targetValid && ((c.index == m.targetIndex && m.targetIndex >= 0) || (c.handle && c.handle == m.targetHandle));
}

// Draws a dropdown row and, while it is the open one, hands its entries to the list
// (DrawFollowDropList). A click on the field opens or closes it.
struct DropEntries {
	std::vector<DropOption> options;
	std::vector<FollowDropPick> picks;
	void Add(const std::string& text, const std::string& command, bool selected, Color color = Theme::Text) {
		DropOption o;
		o.text = text;
		o.color = color;
		o.selected = selected;
		options.push_back(o);
		FollowDropPick p;
		p.command = command;
		picks.push_back(p);
	}
};

bool FollowDrop(FollowInspectorState& st, bool& openSeen, CardColumn& c, int id, const char* label, const std::string& shown,
	Color shownColor, DropEntries& entries, const char* side = nullptr) {
	ImGui::PushID(id);
	const DropClick click = DropRow(c, "drop", label, shown.c_str(), shownColor, side, true);
	ImGui::PopID();
	if (click.field) st.openDrop = st.openDrop == id ? -1 : id;
	if (st.openDrop == id) {
		openSeen = true;
		st.dropField = click.fieldRect;
		st.dropOptions = std::move(entries.options);
		st.dropPicks = std::move(entries.picks);
	}
	return click.side;
}

void Edit(std::vector<std::string>& commands, const ValueEdit& e, const char* verb) {
	if (e.changed) commands.push_back(Command("%s %.3f", verb, e.value));
}

// The pinned bar's target line. Picking a target changes nothing on screen until Live, so the bar's
// second line says so.
std::string StatusLine(const FollowInspectorModel& m) {
	const bool attach = Attached(m);
	if (!HaveTarget(m)) return attach ? "Select a target to ride." : "Place a camera, then select a target.";
	return (m.targetName.empty() ? Format("Entity #%d", m.targetIndex) : m.targetName) + " " EMDASH " "
		+ (m.typeName.empty() ? "Player" : m.typeName) + (attach ? " (attach)" : " (lock-on)");
}

// The bar's pieces: the Live / Stop button at the right, the two text lines left of it. CS2's bar
// is 44 px; a target line too long for one line wraps and the bar grows, rather than cutting it.
struct BarLayout {
	Rect button;
	float textX = 0, textW = 0, lineH = 0, height = 0;
};

BarLayout LayOutBar(const FollowInspectorModel& m, const FontSet& fonts, float s, float x0, float x1) {
	ImFont* status = fonts.Get(FontRole::Label);
	ImFont* live = fonts.Get(FontRole::Caption);
	ImFont* button = fonts.Get(FontRole::Button);
	BarLayout l;
	const float bw = CardButtonWidth(m.enabled ? "Stop" : "Live", button, s) + 10 * s;
	l.textX = x0 + 12 * s;
	l.textW = x1 - 6 * s - bw - 10 * s - l.textX;
	l.lineH = status->CalcTextSizeA(status->FontSize, 1e9f, l.textW, StatusLine(m).c_str()).y;
	const float h = l.lineH + 2 * s + live->FontSize + 12 * s;
	l.height = h > 44 * s ? h : 44 * s;
	const float bh = button->FontSize + 12 * s;
	l.button = MakeRect(x1 - 6 * s - bw, (l.height - bh) * 0.5f, x1 - 6 * s, (l.height + bh) * 0.5f); // y from the bar's top
	return l;
}

void PinnedBar(const FollowInspectorModel& m, const FontSet& fonts, float s, const Rect& bar, const BarLayout& l,
	std::vector<std::string>& commands) {
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(ImVec2(bar.x0, bar.y0), ImVec2(bar.x1, bar.y1), ToU32(kBarBg), 3 * s);
	draw->AddRect(ImVec2(bar.x0, bar.y0), ImVec2(bar.x1, bar.y1), ToU32(kBarBorder), 3 * s);
	ImFont* status = fonts.Get(FontRole::Label);
	ImFont* live = fonts.Get(FontRole::Caption);
	const Rect btn = MakeRect(l.button.x0, bar.y0 + l.button.y0, l.button.x1, bar.y0 + l.button.y1);
	if (CardButton("##followlive", m.enabled ? "Stop" : "Live", btn, ButtonKind::Primary, true, fonts.Get(FontRole::Button)))
		commands.push_back(Command(m.enabled ? "stop" : "preview"));
	const std::string line = StatusLine(m);
	const float y = bar.y0 + (bar.H() - (l.lineH + 2 * s + live->FontSize)) * 0.5f;
	draw->AddText(status, status->FontSize, ImVec2(l.textX, y), ToU32(m.targetValid ? Theme::Text : Theme::TextDisabled), line.c_str(),
		nullptr, l.textW);
	DrawSpacedText(draw, live, ImVec2(l.textX, y + l.lineH + 2 * s), ToU32(m.enabled ? kLive : Theme::Accent),
		m.enabled ? "FOLLOWING LIVE" : "PRESS LIVE TO SEE IT FOLLOW", 1.2f * s);
}

void Rail(FollowInspectorState& st, const FollowInspectorModel& m, const FontSet& fonts, float s, const Rect& rail) {
	ImGui::GetWindowDrawList()->AddLine(ImVec2(rail.x1, rail.y0), ImVec2(rail.x1, rail.y1), ToU32(Rgba(255, 255, 255, 18)), 1.0f);
	const Rect inner = MakeRect(rail.x0, rail.y0, rail.x1 - Metrics::rail.padR * s, rail.y1);
	float width = inner.W();
	CardColumn c;
	c.fonts = &fonts;
	c.s = s;
	c.x0 = inner.x0;
	c.y = BeginCardScroll("##followrail", inner, width);
	c.x1 = inner.x0 + width;
	RailGroup(c, "Setup", true);
	if (RailRow(c, "ftarget", "Camera & Target", TargetSummary(m).c_str(), st.card == 0, Theme::Accent, m.enabled ? "LIVE" : nullptr, kLiveBadge))
		st.card = 0;
	RailGroup(c, "Shot", false);
	if (RailRow(c, "fshot", "Framing & Tracking", ShotSummary(m).c_str(), st.card == 1, Theme::Accent)) st.card = 1;
	EndCardScroll(c.x0, c.y + 8 * s);
}

void TargetCard(FollowInspectorState& st, bool& openSeen, const FollowInspectorModel& m, const FontSet& fonts, float s, const Rect& pane,
	std::vector<std::string>& commands) {
	const bool attach = Attached(m);
	float width = pane.W();
	CardColumn c;
	c.fonts = &fonts;
	c.s = s;
	c.x0 = pane.x0;
	c.y = BeginCardScroll("##followtarget", pane, width);
	c.x1 = pane.x0 + width;

	CardHeader(c, "ftarget", "Camera & Target", TargetSummary(m).c_str(), nullptr, 0);

	// Lock-on (a placed turret) or Attached (ride-along).
	const int mode = ModeRow(c, "cammode", "Camera mode", kCameraModes, 2, attach ? 1 : 0, true);
	if (mode >= 0 && mode != m.mode) commands.push_back(Command(mode == 1 ? "mode attach" : "mode lockon"));
	Note(c, attach ? "Attached: the camera moves and rotates with the selected mount."
	               : "Lock-on: the camera stays where you place it and turns to watch the target.");

	// Place and Clear are Lock-on's; Reposition is both modes' (Attached captures the view as its pose).
	const CardAction thisCamera[] = {
		{ m.hasCamera ? "Replace" : "Place", ButtonKind::Primary, !attach },
		{ "Reposition", ButtonKind::Normal, true },
		{ "Clear", ButtonKind::Danger, !attach },
	};
	switch (ActionRow(c, "thiscamera", "This camera", thisCamera, 3)) {
	case 0: commands.push_back(Command("place")); break;
	case 1: commands.push_back(Command("reposition")); break;
	case 2: commands.push_back(Command("clear")); break;
	default: break;
	}
	const std::string warn = m.repositioning
		? (attach ? "Reposition active: move in freecam, then left-click to save the attach pose."
		          : "Reposition active: move in freecam, then left-click to place.")
		: (!m.message.empty() ? m.message
		: (!attach && m.distanceLevel >= 3 ? "Very far / likely outside the useful shot area."
		: (!attach && m.distanceLevel >= 2 ? "Selected target is far from this camera." : "")));
	Note(c, warn.c_str(), kWarn);

	SubHead(c, "Target");
	{
		// The type's options follow the mode.
		static const char* const lockonNames[] = { "Player", "Grenade", "Weapon/C4" };
		static const char* const attachNames[] = { "Grenade", "Player Bone", "Weapon Attach" };
		static const int lockonTypes[] = { 0, 1, 2 }, attachTypes[] = { 1, 0, 2 };
		static const char* const verbs[] = { "player", "grenade", "weapon" };
		const char* const* names = attach ? attachNames : lockonNames;
		const int* types = attach ? attachTypes : lockonTypes;
		DropEntries e;
		std::string shown;
		for (int i = 0; i < 3; ++i) {
			e.Add(names[i], Command("type %s", verbs[types[i]]), types[i] == m.type);
			if (types[i] == m.type) shown = names[i];
		}
		FollowDrop(st, openSeen, c, kDropType, "Type", shown, Theme::Text, e);
	}
	if (m.type == 2) {
		DropEntries e;
		std::string shown;
		for (const FollowInspectorPlayer& p : m.players) {
			const std::string text = p.name + (!p.team.empty() && p.team != "-" ? " " MIDDOT " " + p.team : "") + (!p.alive ? " " MIDDOT " dead" : "");
			e.Add(text, Command("weaponplayer %d", p.index), p.index == m.weaponFilterPlayer);
			if (p.index == m.weaponFilterPlayer) shown = text;
		}
		if (shown.empty()) shown = m.players.empty() ? "No players available" : "Select a player";
		FollowDrop(st, openSeen, c, kDropPlayer, "Player", shown, m.players.empty() ? Theme::TextDisabled : Theme::Text, e);

		static const char* const sources[] = { "Auto", "Held", "Dropped" };
		static const char* const sourceVerbs[] = { "auto", "held", "dropped" };
		DropEntries src;
		for (int i = 0; i < 3; ++i) src.Add(sources[i], Command("weaponsource %s", sourceVerbs[i]), i == m.weaponSource);
		FollowDrop(st, openSeen, c, kDropSource, "Source", sources[m.weaponSource >= 0 && m.weaponSource < 3 ? m.weaponSource : 0], Theme::Text, src);
	}
	{
		// The far pick asks before it is used: a mis-click on a target across the map otherwise
		// looks like the camera broke.
		DropEntries e;
		std::string shown;
		const size_t n = m.candidates.size() < 40 ? m.candidates.size() : 40;
		for (size_t i = 0; i < n; ++i) {
			const FollowInspectorCandidate& cand = m.candidates[i];
			const std::string text = CandidateText(m, cand);
			const bool selected = CandidateSelected(m, cand);
			e.Add(text, m.type == 1 ? Command("selecthandle %llu", (unsigned long long)cand.handle) : Command("select %d", cand.index), selected,
				cand.distanceLevel >= 2 ? kWarn : Theme::Text);
			if (m.type != 1 && cand.distanceLevel >= 2) {
				FollowDropPick& pick = e.picks.back();
				pick.command.clear();
				pick.farIndex = cand.index;
				pick.farText = Format("%s (%du). Use it anyway?", cand.distanceLevel >= 3 ? "Very far target" : "Far target",
					(int)std::lround(cand.distance));
			}
			if (selected) shown = text;
		}
		const bool none = m.candidates.empty();
		if (shown.empty()) shown = none ? (m.type == 1 ? "No throws nearby" : "No targets " EMDASH " Select Nearest") : Format("Select target (%d)", (int)n);
		if (FollowDrop(st, openSeen, c, kDropTarget, "Target", shown, none ? Theme::TextDisabled : Theme::Text, e, "Go To"))
			commands.push_back(Command("goto"));
	}
	{
		const CardAction nearest[] = { { m.type == 1 ? "Nearest Throw" : "Select Nearest", ButtonKind::Normal, true } };
		if (LabeledActionRow(c, "nearest", "Closest candidate", nearest, 1) == 0) commands.push_back(Command("nearest"));
		const FollowInspectorCandidate* first = m.candidates.empty() ? nullptr : &m.candidates.front();
		const std::string closest = first ? "closest: " + first->name + (!first->team.empty() && first->team != "-" ? "  " MIDDOT "  " + first->team : "")
		                                  : std::string("closest: " EMDASH);
		Note(c, closest.c_str(), first ? Theme::Text : Theme::TextDisabled);
	}
	if (st.pendingIndex >= 0) {
		const CardAction confirm[] = { { "Use anyway", ButtonKind::Normal, true }, { "Cancel", ButtonKind::Normal, true } };
		const int answer = LabeledActionRow(c, "farconfirm", "Target is far away", confirm, 2);
		Note(c, st.pendingText.c_str());
		if (answer == 0) commands.push_back(Command("select %d", st.pendingIndex));
		if (answer >= 0) st.pendingIndex = -1;
	}
	if (attach) {
		// The mounts the target offers; a saved mount this model lacks is listed first, unavailable.
		std::vector<FollowInspectorPoint> points = m.attachPoints;
		const FollowInspectorCandidate* selected = nullptr;
		for (const FollowInspectorCandidate& cand : m.candidates)
			if (cand.index == m.targetIndex || (m.targetHandle && cand.handle == m.targetHandle)) {
				selected = &cand;
				break;
			}
		if (points.empty() && selected) points = selected->attachPoints;
		if (points.empty() && selected)
			for (const std::string& id : selected->attachments) {
				FollowInspectorPoint p;
				p.id = p.label = id;
				p.kind = "attachment";
				p.valid = true;
				points.push_back(p);
			}
		bool hasCurrent = false;
		for (const FollowInspectorPoint& p : points) hasCurrent = hasCurrent || p.id == m.attachment;
		if (!m.attachment.empty() && !hasCurrent) {
			FollowInspectorPoint saved;
			saved.id = m.attachment;
			saved.label = m.attachment + " (saved; unavailable here)";
			points.insert(points.begin(), saved);
		}
		DropEntries e;
		std::string shown;
		for (const FollowInspectorPoint& p : points) {
			const std::string name = !p.id.empty() ? p.id : (!p.label.empty() ? p.label : "entity");
			std::string text = p.label.empty() ? name : p.label;
			if (!p.kind.empty() && p.kind != "attachment") text += " " MIDDOT " " + p.kind;
			if (!p.valid && !p.kind.empty()) text += " " MIDDOT " unavailable";
			e.Add(text, Command("bone %s", name.c_str()), name == m.attachment);
			if (name == m.attachment) shown = text;
		}
		if (points.empty()) shown = "No valid attach points";
		FollowDrop(st, openSeen, c, kDropMount, "Mount", shown, points.empty() ? Theme::TextDisabled : Theme::Text, e);
	}
	if (m.type == 2) {
		DropEntries e;
		std::string shown;
		for (const FollowInspectorEvent& ev : m.events) {
			const std::string text = EventText(ev);
			const bool on = ev.index == m.selectedEvent;
			e.Add(text, Command("eventselect %d", ev.index), on, on ? Theme::Accent : Theme::Text);
			if (on) shown = text;
		}
		if (m.events.empty()) {
			shown = "No drop/pickup events near here.";
			if (m.eventStatus == 1)
				shown = "Scanning demo for drop/pickup events..." + (m.eventScanElapsedMs ? Format(" %ds", (int)std::lround(m.eventScanElapsedMs / 1000.0)) : std::string());
			else if (m.eventStatus == 3)
				shown = m.eventError.empty() ? std::string("Event scan unavailable for this demo.") : "Event scan failed: " + m.eventError;
		} else if (shown.empty()) {
			shown = Format("Select event (%d)", (int)m.events.size());
		}
		FollowDrop(st, openSeen, c, kDropEvent, "Event", shown, m.events.empty() ? Theme::TextDisabled : Theme::Text, e);
	}
	if (m.type == 1 || m.type == 2) {
		const char* go = m.type == 1 ? (m.grenadePending ? "Waiting for Throw..." : "Go To Throw") : "Go To Event";
		const CardAction events[] = { { go, m.grenadePending ? ButtonKind::Primary : ButtonKind::Normal, true } };
		if (LabeledActionRow(c, "events", "Events", events, 1) == 0) commands.push_back(Command("previewtick"));
		Note(c, "Jumps the demo to that moment, so you can mount the camera on an item as it changes hands.");
	}
	if (m.debug && attach && m.attachDebugActive) {
		const std::string text = Format("entity %d  attach %s  %s\ntarget %.2f, %.2f, %.2f\ncamera %.2f, %.2f, %.2f\n"
			"dist %.1f  aim %.1f  jitter %.2f  dC %.2f  dT %.2f", m.attachDebugEntity, m.attachDebugId.empty() ? "-" : m.attachDebugId.c_str(),
			m.attachDebugSource.empty() ? "-" : m.attachDebugSource.c_str(), m.attachDebugTarget[0], m.attachDebugTarget[1], m.attachDebugTarget[2],
			m.attachDebugCamera[0], m.attachDebugCamera[1], m.attachDebugCamera[2], m.attachDebugDistance, m.attachDebugAim, m.attachDebugJitter,
			m.attachDebugCameraDelta, m.attachDebugTargetDelta);
		Note(c, text.c_str(), kDebug);
	}
	EndCardScroll(c.x0, c.y + 10 * s);
}

void ShotCard(FollowInspectorState& st, const FollowInspectorModel& m, const FontSet& fonts, float s, const Rect& pane,
	std::vector<std::string>& commands) {
	const bool attach = Attached(m);
	float width = pane.W();
	CardColumn c;
	c.fonts = &fonts;
	c.s = s;
	c.x0 = pane.x0;
	c.y = BeginCardScroll("##followshot", pane, width);
	c.x1 = pane.x0 + width;

	const CardAction reset[] = { { "Reset", ButtonKind::Normal, true } };
	if (CardHeader(c, "fshot", "Framing & Tracking", ShotSummary(m).c_str(), reset, 1) == 0) {
		commands.push_back(Command("reset framing"));
		commands.push_back(Command("reset tracking"));
	}
	ImGui::SetCursorScreenPos(ImVec2(c.x0, c.y + 4 * s));
	TextTabs("##followshotpages", kShotPages, 2, st.shotPage, fonts.Get(FontRole::Label), Metrics::tab, s, c.W());
	c.y = ImGui::GetCursorScreenPos().y;

	if (st.shotPage == 0) {
		Note(c, attach ? "Attached framing. X = forward, Y = right, Z = up in the mount axes. Reposition captures your current view."
		               : "Lock-on framing. Offsets move the aim point in world axes; rotation trims the aim. The camera stays placed.");
		SubHead(c, "Offset");
		Edit(commands, ValueRow(c, "offx", "Off X", m.offset[0], -256, 256, 1, "u", kAxis[0], true), "offsetx");
		Edit(commands, ValueRow(c, "offy", "Off Y", m.offset[1], -256, 256, 1, "u", kAxis[1], true), "offsety");
		Edit(commands, ValueRow(c, "offz", "Off Z", m.offset[2], -256, 256, 1, "u", kAxis[2], true), "offsetz");
		SubHead(c, "Rotation");
		Edit(commands, ValueRow(c, "pitch", "Pitch", m.rotation[0], -180, 180, 0, DEGREE, kAxis[3], true), "rotpitch");
		Edit(commands, ValueRow(c, "yaw", "Yaw", m.rotation[1], -180, 180, 0, DEGREE, kAxis[4], true), "rotyaw");
		Edit(commands, ValueRow(c, "roll", "Roll", m.rotation[2], -180, 180, 0, DEGREE, kAxis[5], true), "rotroll");
		SubHead(c, "Lens");
		Edit(commands, ValueRow(c, "fov", "FOV", m.fov, 1, 170, 0, "", kAxis[6], true), "fov");
	} else {
		Note(c, attach ? "Attached is a rigid mount: it follows the body directly, so there is no aim smoothing. The On death page still applies."
		               : "Lock-on tracking. These apply only to Lock-on.");
		SubHead(c, "Aim");
		if (!attach) {
			Edit(commands, ValueRow(c, "look", "Aim smoothing", m.look, 0, 1, 2, "s", kAxis[4], true), "look");
			Edit(commands, ValueRow(c, "position", "Target smoothing", m.position, 0, 1, 2, "s", kAxis[0], true), "position");
			Edit(commands, ValueRow(c, "prediction", "Predict", m.prediction, 0, 0.5, 2, "s", kAxis[2], true), "prediction");
			Edit(commands, ValueRow(c, "deadzone", "Deadzone", m.deadzone, 0, 10, 1, DEGREE, kAxis[3], true), "deadzone");
			Edit(commands, ValueRow(c, "maxturn", "Turn/s", m.maxTurn, 0, 1440, 0, DEGREE, kAxis[5], true), "maxturn");
		}
		// On death: a player, or a held weapon's player.
		if (m.type == 0 || (m.type == 2 && m.weaponPlayerIndex >= 0)) {
			SubHead(c, "On death");
			const int death = ModeRow(c, "ondeath", "On death", kDeathModes, 2, m.autoDead ? 1 : 0, true);
			if (death >= 0 && (death == 1) != m.autoDead) commands.push_back(Command("autodead %d", death));
			const bool willRetarget = m.type == 0 && (m.switchWeapon || m.switchBomb);
			if (m.autoDead || willRetarget)
				Edit(commands, ValueRow(c, "deathhold", "Death hold", m.deathHold, 0, 10, 1, "s", kAxis[6], true), "deathhold");
			if (m.type == 0) {
				if (SwitchRow(c, "switchweapon", "Retarget dropped weapon", m.switchWeapon, true))
					commands.push_back(Command("switchweapon %d", m.switchWeapon ? 0 : 1));
				if (SwitchRow(c, "switchbomb", "Retarget dropped C4", m.switchBomb, true))
					commands.push_back(Command("switchbomb %d", m.switchBomb ? 0 : 1));
			}
			Note(c, willRetarget ? "Follow the ragdoll for the hold duration, then switch to the selected dropped item."
			                     : (m.autoDead ? "Follow the moving ragdoll, then stop when the hold ends. Zero stops immediately."
			                                   : "Follow the moving ragdoll for as long as it exists. No death timer is active."));
		}
		SubHead(c, "When target disappears");
		if (SwitchRow(c, "hold", "Hold last camera pose", m.holdLast, true)) commands.push_back(Command("hold %d", m.holdLast ? 0 : 1));
	}
	EndCardScroll(c.x0, c.y + 10 * s);
}

} // namespace

void DrawFollowInspector(FollowInspectorState& state, const FollowInspectorModel& model, const FontSet& fonts, float s,
	const Rect& body, std::vector<std::string>& commands) {
	const float barTop = body.y0 + 14 * s;
	const BarLayout layout = LayOutBar(model, fonts, s, body.x0, body.x1);
	const Rect bar = MakeRect(body.x0, barTop, body.x1, barTop + layout.height);
	PinnedBar(model, fonts, s, bar, layout, commands);
	const float top = bar.y1 + 14 * s;
	const Rect rail = MakeRect(body.x0, top, body.x0 + Metrics::rail.w * s, body.y1);
	const Rect pane = MakeRect(rail.x1 + Metrics::rail.gap * s, top, body.x1, body.y1);
	Rail(state, model, fonts, s, rail);
	bool openSeen = false;
	if (state.card == 0) TargetCard(state, openSeen, model, fonts, s, pane, commands);
	else ShotCard(state, model, fonts, s, pane, commands);
	if (!openSeen) state.openDrop = -1; // its row is gone (another card, mode or type)
}

void DrawFollowDropList(FollowInspectorState& state, const FontSet& fonts, float s, float clientW, float clientH,
	std::vector<std::string>& commands) {
	if (state.openDrop < 0) return;
	bool closed = false;
	const int picked = DropList("##followdrop", state.dropField, state.dropOptions, fonts, s, clientW, clientH, closed);
	if (picked >= 0 && picked < (int)state.dropPicks.size()) {
		const FollowDropPick& pick = state.dropPicks[(size_t)picked];
		if (pick.farIndex >= 0) {
			state.pendingIndex = pick.farIndex;
			state.pendingText = pick.farText;
		} else if (!pick.command.empty()) {
			commands.push_back(pick.command);
		}
	}
	if (closed) state.openDrop = -1;
}

} // namespace Ui
} // namespace SourceFilmmaker
