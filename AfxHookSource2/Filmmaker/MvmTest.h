#pragma once

// Offline live-match FX testing harness (mvm_test console command; the live Config panel is its UI).
// Arms ParticleFx precache outside demo playback and exposes an Insert-key effects menu.

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

bool MvmTest_Active();
void MvmTest_SetActive(bool on);
bool MvmTest_LevelLoaded();
bool MvmTest_CanUseMenu();
void MvmTest_EnsureArmed();
bool MvmTest_HandleInsertKey();

void MvmTest_RunCommand(int argc, advancedfx::ICommandArgs* args);

} // namespace Filmmaker
