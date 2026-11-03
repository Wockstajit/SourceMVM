#pragma once

// The weapon channel in CS:S (CameraShakeHost.h ShakeHost_EnsureWeaponChannel and
// ShakeHost_SetWeaponKick): what lets Weapon Recoil and Weapon Bob move the gun, and the gun ride the
// camera shake, as CS2's CalcViewModelView hook does (ViewFxVmCalcHook.cpp).
//
// CS2 shakes the eye before it builds the viewmodel, so the gun rides the shake, and adds the
// weapon kick to the viewmodel's eye inputs. CS:S builds the viewmodel inside CalcView, before the
// view reaches SetupEngineView, where HLAE's overrides and the shake run: left alone, the gun sits
// where the unshaken eye put it while the view shakes around it, and never kicks. So this hook holds
// C_BaseViewModel::CalcViewModelView back while a shake layer is on, and the view override runs it
// once the shake is known, from the eye moved by the shake (less Handheld while its viewmodel is
// decoupled) plus the weapon kick. One call a frame, from this frame's numbers, as CS2's.
//
// Main thread.

namespace SourceFilmmaker {

// Finds and detours CalcViewModelView; retried each frame until it settles.
void ViewModel_Init();
// The detour is in.
bool ViewModel_Installed();
// Why not, while it is not.
const char* ViewModel_Status();

// The weapon kick for this frame: a world-space position and engine angles (degrees), added to the
// viewmodel's eye inputs. Replaced every frame.
void ViewModel_SetWeaponKick(double x, double y, double z, double pitch, double yaw, double roll);
// The camera's motion the gun rides this frame: the shake's change to the view, less a decoupled
// Handheld's part. World position, degrees. Replaced every frame.
void ViewModel_SetCameraDelta(double x, double y, double z, double pitch, double yaw, double roll);

// From the view override, after the shake: builds the held viewmodels from the eye plus this
// frame's camera delta and kick.
void ViewModel_OnView();
// At the start of a frame: a viewmodel still held (no view override came) is built as the game
// would have.
void ViewModel_Frame();

void ViewModel_PrintHealth();

} // namespace SourceFilmmaker
