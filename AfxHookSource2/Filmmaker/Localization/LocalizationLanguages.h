#pragma once

// The languages the CS2 panels can be shown in: every language CS2 ships.
//
// `id` is CS2's own name for the language (what `cl_language` and Panorama's $.Language() report,
// and the suffix of Valve's csgo_<id>.txt), so "Automatic" needs no mapping. `name` is the
// language's name in that language, which is what the picker shows. `pluralForms` is how many
// forms a plural table entry carries (LocalizationJs.h has the rule per language).
//
// English is the source text written in the scripts and has no table.
// Automation/i18n/strings.mjs carries the same list for the tools; a test compares the two.

#include <cstddef>
#include <cstring>

namespace Filmmaker {

struct LocalizationLanguage {
	const char* id;
	const char* name;
	int pluralForms;
};

inline constexpr LocalizationLanguage kLocalizationLanguages[] = {
	{ "english", "English", 1 },
	{ "brazilian", "Português (Brasil)", 2 },
	{ "bulgarian", "Български", 2 },
	{ "czech", "Čeština", 3 },
	{ "danish", "Dansk", 2 },
	{ "dutch", "Nederlands", 2 },
	{ "finnish", "Suomi", 2 },
	{ "french", "Français", 2 },
	{ "german", "Deutsch", 2 },
	{ "greek", "Ελληνικά", 2 },
	{ "hungarian", "Magyar", 2 },
	{ "indonesian", "Bahasa Indonesia", 1 },
	{ "italian", "Italiano", 2 },
	{ "japanese", "日本語", 1 },
	{ "koreana", "한국어", 1 },
	{ "latam", "Español (Latinoamérica)", 2 },
	{ "norwegian", "Norsk", 2 },
	{ "polish", "Polski", 3 },
	{ "portuguese", "Português", 2 },
	{ "romanian", "Română", 3 },
	{ "russian", "Русский", 3 },
	{ "schinese", "简体中文", 1 },
	{ "spanish", "Español (España)", 2 },
	{ "swedish", "Svenska", 2 },
	{ "tchinese", "繁體中文", 1 },
	{ "thai", "ไทย", 1 },
	{ "turkish", "Türkçe", 2 },
	{ "ukrainian", "Українська", 3 },
	{ "vietnamese", "Tiếng Việt", 1 },
};

inline constexpr size_t kLocalizationLanguageCount = sizeof(kLocalizationLanguages) / sizeof(kLocalizationLanguages[0]);

// The table language for a name the game or the user gave, or nullptr when it is not one of ours.
// Accepts the spellings the engine uses for the same language: `korean` beside `koreana`, and
// Perfect World's `schinese_pw` build.
inline const LocalizationLanguage* LocalizationLanguage_Find(const char* name) {
	if (!name || !*name) return nullptr;
	if (0 == std::strcmp(name, "korean")) name = "koreana";
	else if (0 == std::strcmp(name, "schinese_pw")) name = "schinese";
	for (const LocalizationLanguage& language : kLocalizationLanguages) {
		if (0 == std::strcmp(language.id, name)) return &language;
	}
	return nullptr;
}

} // namespace Filmmaker
