#pragma once

#include <atomic>
#include <cstddef>

namespace Filmmaker::ViewFxVmInternal {

// Shared control state for the three focused implementation modules. Runtime callers continue
// to see only ViewFxVm.h; this internal seam keeps engine-hook, scene-transform, and command
// details out of the public interface.
extern std::atomic<int> g_site;
extern std::atomic<bool> g_siteExplicit;
extern std::atomic<int> g_field;
extern std::atomic<int> g_mode;
extern double g_spinDegPerApply;
extern double g_freeAimPitch;
extern double g_freeAimYaw;
extern std::atomic<double> g_cameraCompX;
extern std::atomic<double> g_cameraCompY;
extern std::atomic<double> g_cameraCompZ;
extern std::atomic<double> g_cameraCompPitch;
extern std::atomic<double> g_cameraCompYaw;
extern std::atomic<double> g_cameraCompRoll;
// Weapon Recoil kick: ADDED to the CalcViewModelView inputs (compensation is subtracted).
// Position is a world-space delta, angles are engine-convention degrees.
extern std::atomic<double> g_weaponKickX;
extern std::atomic<double> g_weaponKickY;
extern std::atomic<double> g_weaponKickZ;
extern std::atomic<double> g_weaponKickPitch;
extern std::atomic<double> g_weaponKickYaw;
extern std::atomic<double> g_weaponKickRoll;
// Last engine frame on which the CalcViewModelView caller hook actually ran (set on every entry,
// including the no-adjustment early-out). -1 until the first call. The camera-side per-frame path
// compares against this to make "the viewmodel did NOT run this frame" visible in the log -- the
// exact instrument the camera-vs-viewmodel delivery-rate question needs.
extern std::atomic<int> g_lastVmCallerFrame;
extern std::atomic<bool> g_povActive;
extern std::atomic<int> g_povPawnIndex;
extern bool g_previousAimNonZero;

// Production CalcViewModelView input hook.
void CalcHook_NoteViewmodelHelper(std::size_t helperAddr);
bool CalcHook_EnsureInstalled();
bool CalcHook_Installed();

// Legacy scene-node write sites and their tightly-coupled diagnostics.
void SceneTransform_Apply(const char* siteTag);
void SceneTransform_UpdatePovState(bool active, int pawnIndex);
unsigned SceneTransform_ObserveCallerFrame();
void SceneTransform_DumpInfo();
unsigned long long SceneTransform_ApplyCount();
int SceneTransform_LastViewmodelIndex();
int SceneTransform_QuatOffset();

} // namespace Filmmaker::ViewFxVmInternal
