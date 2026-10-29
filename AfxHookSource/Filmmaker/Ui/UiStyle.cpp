#include "UiStyle.h"

#include <cstdio>
#include <cwchar>

namespace SourceFilmmaker {
namespace Ui {

namespace {

std::string FontFile(const wchar_t* face, bool bold) {
	if (0 == std::wcscmp(face, L"Tahoma")) return bold ? "tahomabd.ttf" : "tahoma.ttf";
	return bold ? "verdanab.ttf" : "verdana.ttf";
}

bool FileExists(const std::string& path) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	std::fclose(f);
	return true;
}

ImVec4 V(Color c) { return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f); }

} // namespace

FontSet BuildFonts(ImFontAtlas* atlas, const std::string& fontDir, float scale) {
	FontSet set;
	set.scale = scale;
	set.ok = true;
	atlas->Clear();
	// Latin-1 covers every caption the UI writes; the dashes, bullet, ellipsis, angle quote and arrows
	// are for ranges and status lines, lists, cut-off lines, breadcrumbs and chevrons.
	static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x2013, 0x2014, 0x2022, 0x2022, 0x2026, 0x2026, 0x203A, 0x203A,
		0x2190, 0x2193, 0x25B2, 0x25BC, 0 };
	ImFontConfig config;
	config.OversampleH = 2;
	config.OversampleV = 1;
	config.PixelSnapH = true;
	for (int i = 0; i < (int)FontRole::Count; ++i) {
		const FontSpec spec = FontSpecFor((FontRole)i);
		float px = spec.size * scale;
		if (px < 6.0f) px = 6.0f; // stb_truetype output below this is noise
		const std::string path = fontDir + "/" + FontFile(spec.face, spec.bold);
		ImFont* font = nullptr;
		if (FileExists(path)) font = atlas->AddFontFromFileTTF(path.c_str(), px, &config, ranges);
		// Verdana has no arrows or triangles; Tahoma, its VGUI sibling, lends them at the same size.
		const std::string lender = fontDir + "/" + FontFile(L"Tahoma", spec.bold);
		if (font && 0 != std::wcscmp(spec.face, L"Tahoma") && FileExists(lender)) {
			static const ImWchar lent[] = { 0x2190, 0x2193, 0x25B2, 0x25BC, 0 };
			ImFontConfig merge = config;
			merge.MergeMode = true;
			atlas->AddFontFromFileTTF(lender.c_str(), px, &merge, lent);
		}
		if (!font) {
			set.ok = false;
			ImFontConfig fallback;
			fallback.SizePixels = px;
			font = atlas->AddFontDefault(&fallback);
		}
		set.fonts[i] = font;
	}
	atlas->Build();
	return set;
}

void ApplyStyle(ImGuiStyle& style, float scale) {
	style = ImGuiStyle();
	// VGUI frames are square and one pixel wide; CS2's spacing from UiTheme.h's metrics.
	style.WindowRounding = 0;
	style.ChildRounding = 0;
	style.FrameRounding = 0;
	style.PopupRounding = 0;
	style.ScrollbarRounding = 0;
	style.GrabRounding = 0;
	style.TabRounding = 0;
	style.WindowBorderSize = 1;
	style.FrameBorderSize = 1;
	style.PopupBorderSize = 1;
	style.WindowPadding = ImVec2(Metrics::col.pad, Metrics::col.pad);
	style.FramePadding = ImVec2(Metrics::btn.padH, Metrics::btn.padV);
	style.ItemSpacing = ImVec2(Metrics::col.gap, Metrics::col.rowGap);
	style.ItemInnerSpacing = ImVec2(Metrics::line.gap, Metrics::line.gap);
	style.ScrollbarSize = 10;
	style.GrabMinSize = 10;
	style.AntiAliasedLines = true;
	style.AntiAliasedFill = true;
	style.ScaleAllSizes(scale);

	ImVec4* c = style.Colors;
	c[ImGuiCol_Text] = V(Theme::Text);
	c[ImGuiCol_TextDisabled] = V(Theme::TextDisabled);
	c[ImGuiCol_WindowBg] = V(Theme::PanelBg);
	c[ImGuiCol_ChildBg] = V(Rgba(0, 0, 0, 0));
	c[ImGuiCol_PopupBg] = V(Rgba(0, 0, 0, 230));
	c[ImGuiCol_Border] = V(Theme::Border);
	c[ImGuiCol_BorderShadow] = V(Rgba(0, 0, 0, 0));
	c[ImGuiCol_FrameBg] = V(Theme::ButtonBg);
	c[ImGuiCol_FrameBgHovered] = V(Theme::ButtonHoverBg);
	c[ImGuiCol_FrameBgActive] = V(Theme::ButtonArmedBg);
	c[ImGuiCol_TitleBg] = V(Theme::PanelBg);
	c[ImGuiCol_TitleBgActive] = V(Theme::PanelBg);
	c[ImGuiCol_TitleBgCollapsed] = V(Theme::PanelBg);
	c[ImGuiCol_ScrollbarBg] = V(Rgba(0, 0, 0, 60));
	c[ImGuiCol_ScrollbarGrab] = V(Theme::Border);
	c[ImGuiCol_ScrollbarGrabHovered] = V(Theme::AccentDim);
	c[ImGuiCol_ScrollbarGrabActive] = V(Theme::Accent);
	c[ImGuiCol_CheckMark] = V(Theme::Accent);
	c[ImGuiCol_SliderGrab] = V(Theme::Accent);
	c[ImGuiCol_SliderGrabActive] = V(Theme::Accent);
	c[ImGuiCol_Button] = V(Theme::ButtonBg);
	c[ImGuiCol_ButtonHovered] = V(Theme::ButtonHoverBg);
	c[ImGuiCol_ButtonActive] = V(Theme::ButtonArmedBg);
	c[ImGuiCol_Header] = V(Theme::ButtonHoverBg);
	c[ImGuiCol_HeaderHovered] = V(Theme::ButtonHoverBg);
	c[ImGuiCol_HeaderActive] = V(Theme::ButtonArmedBg);
	c[ImGuiCol_Separator] = V(Theme::Border);
	c[ImGuiCol_SeparatorHovered] = V(Theme::Accent);
	c[ImGuiCol_SeparatorActive] = V(Theme::Accent);
	c[ImGuiCol_TextSelectedBg] = V(Theme::Border);
	c[ImGuiCol_NavCursor] = V(Theme::Accent);
}

} // namespace Ui
} // namespace SourceFilmmaker
