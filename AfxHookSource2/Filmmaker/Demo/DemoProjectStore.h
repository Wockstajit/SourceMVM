#pragma once

#include "DemoProjectFileMath.h"
#include "DemoProjectStoreMath.h"

#include <string>

namespace Filmmaker {

std::wstring DemoProjectStore_Root();
std::wstring DemoProjectStore_ProjectDirectory(ProjectGame game, const std::wstring& demoPath);

// Creates the per-demo folder and its project.mvm container. Empty when AppData cannot be used.
std::wstring DemoProjectStore_EnsureProjectDirectory(ProjectGame game, const std::wstring& demoPath);
std::wstring DemoProjectStore_EnsureProjectFile(ProjectGame game, const std::wstring& demoPath);

// Reads one section of the demo's project.mvm. False when the project file or the section
// is absent or invalid.
bool DemoProjectStore_ReadSection(ProjectGame game, const std::wstring& demoPath,
	ProjectSection section, std::string& payload);

// Read-modify-writes project.mvm with this section replaced; an empty payload removes the
// section. The "project" section is refreshed on every write.
bool DemoProjectStore_WriteSection(ProjectGame game, const std::wstring& demoPath,
	ProjectSection section, const std::string& payload);

std::wstring DemoProjectStore_GeneratedForLoad(ProjectGame game, const std::wstring& logicalDemoPath,
	ProjectGenerated kind);
std::wstring DemoProjectStore_GeneratedForSave(ProjectGame game, const std::wstring& logicalDemoPath,
	ProjectGenerated kind);

// Read CS2's `<demo>.dem.info` in place. If an older release relocated it, restore the saved
// Valve file without overwriting a current sidecar. Returns empty if no sidecar is available.
std::wstring DemoProjectStore_MatchInfoForLoad(ProjectGame game, const std::wstring& demoPath);

void DemoProjectStore_NoteRenamed(ProjectGame game, const std::wstring& oldPath,
	const std::wstring& newPath);

} // namespace Filmmaker
