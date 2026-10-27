#pragma once

// CS2's card grammar for the Camera Editor's inspector (CameraEditorCardsJs.h), drawn on ImGui in
// CS:S paint. Each inspector tab is a RAIL, one row per card (or per camera) under small group
// headings, beside a PANE that shows the selected card: a header (accent edge, title, summary,
// header buttons) and its rows. A value is one row, label | bar | number, dragged relative to
// where it was (Shift is fine control); a pick-one is a mode row, label then a segmented strip;
// actions are a row of equal buttons under a small heading. A row that does not apply is
// disabled in place, never hidden, so nothing moves under the pointer, and no caption is cut:
// a strip that does not fit goes under its label, buttons that do not fit stack. Rows that belong
// to another mode or target type are left out, as CS2 leaves them out: the switch that changes
// them sits above them.
//
// Sizes are CS2's stage px (Metrics in UiTheme.h) times the caller's scale. Engine-free: the game
// host and the browser mockup both draw with it, inside an ImGui window.

#include "UiStyle.h"

#include <string>
#include <vector>

namespace SourceFilmmaker {
namespace Ui {

// Rows go down a column from y, between x0 and x1 (client px). Each call draws one row and moves y
// below it.
struct CardColumn {
	const FontSet* fonts = nullptr;
	float s = 1;
	float x0 = 0, x1 = 0;
	float y = 0;
	float W() const { return x1 - x0; }
	ImFont* Font(FontRole role) const { return fonts->Get(role); }
};

// A row's ground on the inspector's black (CS2's tileBg, lifted for CS:S's darker panel).
constexpr Color kCardTileBg = Rgba(255, 255, 255, 10);

enum class ButtonKind {
	Normal,  // the label tier on a faint ground
	Primary, // outlined: accent caption on a faint accent ground
	Danger,  // red caption and outline
	Solid,   // the header's one solid accent button
};

struct CardAction {
	const char* caption;
	ButtonKind kind;
	bool enabled;
};

// A card button filling r, its caption centred; true when clicked.
bool CardButton(const char* id, const char* caption, const Rect& r, ButtonKind kind, bool enabled, ImFont* font);
// The width a card button needs for its caption (CS2's 11 px side padding).
float CardButtonWidth(const char* caption, ImFont* font, float s);

// The card's header: a 3 x 30 accent edge, the title, the summary under it (wrapped, two lines kept
// for it), and the header buttons right-aligned in the given order. Returns the clicked button's
// index, -1 for none.
int CardHeader(CardColumn& c, const char* id, const char* title, const char* summary, const CardAction* buttons, int count);

// A child window over r that scrolls when what is drawn into it outgrows it. Returns the content's
// top (scrolled) and sets width to what is left beside a showing scrollbar. End it with the
// content's bottom, for the scroll range.
float BeginCardScroll(const char* id, const Rect& r, float& width);
void EndCardScroll(float x0, float contentBottom);

// A small uppercase caption grouping the rows under it (edSubHead).
void SubHead(CardColumn& c, const char* text);
// A wrapped hint line (edNote), in the hint colour or the given one (CS2's yellow warning line).
void Note(CardColumn& c, const char* text);
void Note(CardColumn& c, const char* text, Color color);

// A value row (edValueTile). began on the press, changed with the new value while dragging, ended
// on the release; the caller turns them into one undoable edit.
struct ValueEdit {
	bool began = false;
	bool changed = false;
	bool ended = false;
	double value = 0;
};
ValueEdit ValueRow(CardColumn& c, const char* id, const char* label, double value, double min, double max, int decimals,
	const char* unit, Color fill, bool enabled);

// A mode row (edModeRow): the label, then one cell per caption, the selected one tinted. Returns the
// clicked cell, -1 for none. selected -1 lights none.
int ModeRow(CardColumn& c, const char* id, const char* label, const char* const* captions, int count, int selected,
	bool enabled);

// Equal buttons under a small heading (edEvenActions). Returns the clicked button, -1 for none.
int ActionRow(CardColumn& c, const char* id, const char* heading, const CardAction* buttons, int count);
// Equal buttons in the value column beside a label (edActionGroup). Returns the clicked button.
int LabeledActionRow(CardColumn& c, const char* id, const char* label, const CardAction* buttons, int count);

// A switch row (edSwitchTile): the label, and a small switch at the right. True when clicked.
bool SwitchRow(CardColumn& c, const char* id, const char* label, bool on, bool enabled);

// A dropdown row (CS2's folDropRow): the label, a field showing the current choice with a caret,
// and an optional side button (side null for none). field holds the field's rect for its list.
struct DropClick {
	bool field = false;
	bool side = false;
	Rect fieldRect;
};
DropClick DropRow(CardColumn& c, const char* id, const char* label, const char* shown, Color shownColor, const char* side,
	bool enabled);

// One entry of a dropdown's list: its text, colour, and whether it is the current choice.
struct DropOption {
	std::string text;
	Color color = Theme::Text;
	bool selected = false;
};
// A dropdown's open list, over everything, under the field (above it when there is no room below),
// scrolling past CS2's 340 px. Draw it after the panels. Returns the picked entry, -1 for none;
// closed turns true when a pick or a click outside closed it.
int DropList(const char* id, const Rect& field, const std::vector<DropOption>& options, const FontSet& fonts, float s,
	float clientW, float clientH, bool& closed);

// The rail: a group heading, a row per card or camera (title and wrapped summary, the accent on its
// left edge, an optional badge such as CS2's green LIVE at the right of the title), and the add
// button closing a list.
void RailGroup(CardColumn& c, const char* name, bool first);
bool RailRow(CardColumn& c, const char* id, const char* title, const char* summary, bool selected, Color accent,
	const char* badge = nullptr, Color badgeColor = Theme::Accent);
bool RailAddButton(CardColumn& c, const char* id, const char* caption);

// The key strip under the inspector's body: key caps and what they do, wrapping.
struct KeyHint {
	const char* keys;
	const char* does;
};
float KeyStripHeight(const KeyHint* hints, int count, const FontSet& fonts, float s, float width);
void DrawKeyStrip(ImDrawList* draw, const KeyHint* hints, int count, const FontSet& fonts, float s, const Rect& r);

} // namespace Ui
} // namespace SourceFilmmaker
