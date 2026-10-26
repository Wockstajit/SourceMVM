#pragma once

// The Source 1 UI's look: CS2's geometry, CS:S's paint.
//
// Geometry: every number in `Metrics` is CS2's, from PanoramaUiMetricsJs.h (the `UI` table) and
// CameraEditorJs.h, in 1080-tall stage pixels. Automation/tests/source-ui-metrics-parity-tests.mjs
// reads both files and fails when they disagree, so the two games keep the same layout.
//
// Paint: the colours come from Counter-Strike: Source's own VGUI schemes. The workspace uses the
// spectator / HUD look from cstrike's ClientScheme.res (translucent black, "Orange" 255 176 0,
// "LightOrange" borders, "Red" armed buttons), because the filmmaker lives over a playing demo;
// body text uses the menu scheme's off-white (SourceScheme.res) so values stay readable.
// Type is Verdana, which ClientScheme uses for nearly every font, with Tahoma for dense text.

#include "UiTypes.h"

namespace SourceFilmmaker {
namespace Ui {

namespace Palette {
// cstrike/resource/ClientScheme.res
constexpr Color Orange = Rgba(255, 176, 0);
constexpr Color OrangeDim = Rgba(255, 176, 0, 120);
constexpr Color LightOrange = Rgba(188, 112, 0, 128);
constexpr Color Red = Rgba(192, 28, 0, 140);
constexpr Color TransparentBlack = Rgba(0, 0, 0, 196);
constexpr Color TransparentLightBlack = Rgba(0, 0, 0, 90);
constexpr Color ButtonBg = Rgba(0, 0, 0, 64);
// platform/resource/SourceScheme.res
constexpr Color OffWhite = Rgba(221, 221, 221);
constexpr Color DimText = Rgba(160, 170, 149);
constexpr Color DisabledText = Rgba(117, 128, 111);
constexpr Color Black = Rgba(0, 0, 0);
// cstrike's chat and scoreboard team colours (hud_basechat.cpp g_ColorBlue / g_ColorRed).
constexpr Color TeamCT = Rgba(153, 204, 255);
constexpr Color TeamT = Rgba(255, 63, 63);
} // namespace Palette

// Semantic roles, so widgets ask for "border" rather than for a scheme name.
namespace Theme {
constexpr Color PanelBg = Palette::TransparentBlack;          // inspector column, director cards
constexpr Color DockBg = Rgba(0, 0, 0, 235);                 // the demo bar's panel, docked and on its own
constexpr Color CardBg = Palette::TransparentLightBlack;      // cards and insets inside a panel
constexpr Color Border = Palette::LightOrange;
constexpr Color Accent = Palette::Orange;                     // titles, active tab, preview frame
constexpr Color AccentDim = Palette::OrangeDim;
constexpr Color Text = Palette::OffWhite;                     // values and body text
constexpr Color TextDim = Palette::DimText;                   // captions, hints
constexpr Color TextDisabled = Palette::DisabledText;
constexpr Color ButtonBg = Palette::ButtonBg;
constexpr Color ButtonHoverBg = Rgba(188, 112, 0, 60);
constexpr Color ButtonArmedBg = Palette::Red;                 // pressed, like a VGUI armed button
constexpr Color Letterbox = Palette::Black;                   // around the game preview
constexpr Color Loop = Rgba(124, 224, 142);                   // CS2's LOOP green: the loop region and its lit button
} // namespace Theme

// Fonts the theme asks for. Faces are Windows' Verdana and Tahoma; sizes are stage px.
enum class FontRole {
	Body,      // Verdana 15
	BodyBold,  // Verdana Bold 15
	Label,     // Verdana Bold 13, captions and tab labels
	Small,     // Tahoma 13
	Title,     // Verdana Bold 16, card and panel titles
	Display,   // Verdana Bold 44, the workspace name under the preview
	// The inspector cards' tiers, at CS2's sizes (CameraEditorCardsJs.h).
	Caption,   // Verdana Bold 11, sub-heads and the action rows' headings
	Note,      // Verdana 12, notes and the card header's summary
	Button,    // Verdana Bold 12, card buttons and segmented captions
	RowLabel,  // Verdana 13, a row's name
	Value,     // Verdana Bold 14, a row's value
	RailSum,   // Verdana 14, a rail row's summary
	CardTitle, // Verdana Bold 17, the card's title
	Count
};

struct FontSpec {
	const wchar_t* face;
	float size;
	bool bold;
};

inline FontSpec FontSpecFor(FontRole role) {
	switch (role) {
	case FontRole::Body: return { L"Verdana", 15, false };
	case FontRole::BodyBold: return { L"Verdana", 15, true };
	case FontRole::Label: return { L"Verdana", 13, true };
	case FontRole::Small: return { L"Tahoma", 13, false };
	case FontRole::Title: return { L"Verdana", 16, true };
	case FontRole::Display: return { L"Verdana", 44, true };
	case FontRole::Caption: return { L"Verdana", 11, true };
	case FontRole::Note: return { L"Verdana", 12, false };
	case FontRole::Button: return { L"Verdana", 12, true };
	case FontRole::RowLabel: return { L"Verdana", 13, false };
	case FontRole::Value: return { L"Verdana", 14, true };
	case FontRole::RailSum: return { L"Verdana", 14, false };
	case FontRole::CardTitle: return { L"Verdana", 17, true };
	default: return { L"Verdana", 15, false };
	}
}

// CS2's UI table (PanoramaUiMetricsJs.h `UI`) and Camera Editor constants, stage px.
namespace Metrics {
struct Btn { float padV, padH, font, radius, gap; };
constexpr Btn btn = { 5, 12, 15, 3, 5 };
constexpr Btn btnCompact = { 4, 9, 13, 0, 0 };
struct Action { float h, radius, font, gap, inset; };
constexpr Action action = { 38, 5, 15, 12, 22 };
constexpr float tabBarH = 42;
constexpr float tabGap = 4;
struct Col { float w, pad, gap, rowGap; };
constexpr Col col = { 640, 22, 10, 5 };
struct Card { float padV, padH, radius, gap, edgeW, edgeH, titleFont, subFont, inner, bodyGap, headerGap; };
constexpr Card card = { 10, 16, 5, 8, 3, 32, 16, 13, 442, 6, 10 };
struct Tile { float h, padTop, padBottom, padH, radius, labelFont, labelH, valueFont, barH, perRow; };
constexpr Tile tile = { 64, 5, 5, 13, 4, 14, 17, 20, 3, 2 };
struct Tab { float font, ls, gap, padBottom, underline; };
constexpr Tab tab = { 12, 1.5f, 20, 7, 2 };
constexpr Tab tabTop = { 14, 1.4f, 20, 9, 2 };
struct Rail { float w, padR, gap, rowH, groupGap, rowGap, edge, titleFont, sumFont, groupFont; };
constexpr Rail rail = { 180, 12, 14, 56, 16, 4, 3, 16, 14, 13 };
struct Line { float h, label, value, gap, padH, colGap; };
constexpr Line line = { 32, 130, 66, 5, 10, 12 };
struct Switch { float w, h, knob, travel; };
constexpr Switch sw = { 42, 22, 14, 20 };
constexpr Switch swSm = { 34, 18, 12, 16 };
struct Chip { float font, padV, padH, radius; };
constexpr Chip chip = { 13, 5, 13, 3 };
} // namespace Metrics

} // namespace Ui
} // namespace SourceFilmmaker
