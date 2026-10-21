// Per-demo cosmetic picks: the "cosmetics" section of the playing demo's project.mvm.
//
// Its own file because a project section is keyed by the ORIGINAL demo (PlayingDemoPath maps a
// keyframed or repaired copy back to it), while the session detection in CosmeticOverrideSystem.cpp
// deliberately follows the raw file the engine has open. Keeping the two apart keeps a section from
// ever being keyed by a copy (Automation/tests/demo-project-keying-tests.mjs).

#include "CosmeticOverrideSystem.h"

#include "../Debug/MvmDebugLog.h"
#include "../Demo/DemoProjectStore.h"
#include "../Filmmaker.h" // PlayingDemoPath()

namespace Filmmaker {

void CosmeticOverrideSystem::FlushProfiles() {
	if (!m_profilesDirty || m_projectDemoPath.empty())
		return;
	m_profilesDirty = false;
	// While a theme dresses the server the store holds the theme's look; the user's own picks are the
	// snapshot it restores at EndTheme, and those are what the demo keeps.
	std::string text;
	if (m_theme) {
		CosmeticProfileStore saved;
		saved.ReplaceAll(m_themeSavedProfiles);
		text = saved.Serialize();
	} else {
		text = m_store.Serialize();
	}
	const bool ok = DemoProjectStore_WriteSection(ProjectGame::Cs2, m_projectDemoPath, ProjectSection::Cosmetics, text);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("cosmetics.save", "ok=%d bytes=%zu profiles=%zu", ok ? 1 : 0, text.size(),
			m_theme ? m_themeSavedProfiles.size() : m_store.All().size());
}

void CosmeticOverrideSystem::LoadDemoProfiles() {
	m_projectDemoPath = PlayingDemoPath();
	m_profilesDirty = false;
	if (m_projectDemoPath.empty())
		return;
	std::string text;
	if (!DemoProjectStore_ReadSection(ProjectGame::Cs2, m_projectDemoPath, ProjectSection::Cosmetics, text))
		return;
	const bool ok = m_store.Deserialize(text);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("cosmetics.load", "ok=%d profiles=%zu", ok ? 1 : 0, m_store.All().size());
	if (m_store.Empty())
		return;
	// No entity carries these yet. They are painted the way a seek's are: the attributes on the next
	// apply pass, the composite on the next paused frame (the user's pause, a seek hold, or a render's
	// pre-roll landing -- which waits for it through SeekRefreshPending).
	m_seekCompositePending = true;
}

} // namespace Filmmaker
