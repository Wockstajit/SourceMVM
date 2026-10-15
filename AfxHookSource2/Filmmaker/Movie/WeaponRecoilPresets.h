#pragma once

#include "WeaponRecoilScope.h"
#include "WeaponRecoilStyles.h"

#include <cctype>
#include <string>
#include <vector>

namespace Filmmaker {

struct WeaponRecoilPreset {
	std::string name;
	RecoilScope scope = RecoilScope::All;
	std::string key;
	RecoilStyle style = RecoilStyle::Mw2019;
	RecoilTuning tuning;
};

// Engine-free named-preset collection. Names are intentionally conservative because they are
// also passed through a Source console command by Panorama.
class WeaponRecoilPresetBook {
public:
	static std::string CleanName(const char* value) {
		std::string out = value ? value : "";
		while (!out.empty() && std::isspace((unsigned char)out.front())) out.erase(out.begin());
		while (!out.empty() && std::isspace((unsigned char)out.back())) out.pop_back();
		if (out.empty() || out.size() > 40) return {};
		for (unsigned char c : out)
			if (!(std::isalnum(c) || c == ' ' || c == '_' || c == '-' || c == '.')) return {};
		return out;
	}

	const WeaponRecoilPreset* Find(const char* name) const {
		const std::string clean = CleanName(name);
		if (clean.empty()) return nullptr;
		const std::string needle = RecoilLower(clean);
		for (const auto& preset : m_items)
			if (RecoilLower(preset.name) == needle) return &preset;
		return nullptr;
	}

	bool Save(WeaponRecoilPreset preset) {
		preset.name = CleanName(preset.name.c_str());
		if (preset.name.empty()) return false;
		preset.key = preset.scope == RecoilScope::All ? std::string() : RecoilLower(preset.key);
		if (preset.scope != RecoilScope::All && preset.key.empty()) return false;
		const std::string needle = RecoilLower(preset.name);
		for (auto& item : m_items) {
			if (RecoilLower(item.name) != needle) continue;
			item = preset;
			return true;
		}
		m_items.push_back(preset);
		return true;
	}

	bool Erase(const char* name) {
		const std::string clean = CleanName(name);
		if (clean.empty()) return false;
		const std::string needle = RecoilLower(clean);
		for (size_t i = 0; i < m_items.size(); ++i) {
			if (RecoilLower(m_items[i].name) != needle) continue;
			m_items.erase(m_items.begin() + i);
			return true;
		}
		return false;
	}

	void Clear() { m_items.clear(); }
	const std::vector<WeaponRecoilPreset>& Items() const { return m_items; }

private:
	std::vector<WeaponRecoilPreset> m_items;
};

} // namespace Filmmaker
