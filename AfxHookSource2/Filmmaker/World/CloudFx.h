#pragma once

// Cloud look controls -- density, brightness, size and drift.
//
// CS2's clouds are NOT a flat sky texture. They are geometry drawn with `csgo_effects.vfx`, whose
// parameters include g_flOpacityScale, g_flColorBoost, and three scrolling mask layers
// (g_vMask1..3Scale / g_vMask1..3PanSpeed). That is why they can be thinned, enlarged and set
// moving rather than only tinted. Confirmed by decompiling materials/effects/clouds_001.vmat_c
// and de_vertigo/vertigo_clouds_001.vmat_c out of pak01_dir.vpk.
//
// EVERY CONTROL IS A MULTIPLIER ON THE MAP'S OWN VALUE, never an absolute. Each map authors its
// clouds differently -- Vertigo's are not Ancient's -- so "density 0.5" means "half of whatever
// this map chose", which is the only definition that behaves the same everywhere. It also makes
// restore exact: the snapshot is the map's authored value.
//
// Colour is deliberately NOT here. `mirv_sky clouds color` already owns g_vColorTint, and a
// second writer for one parameter is how two features end up fighting over a frame.
//
// See Docs/effects/skybox.md.

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {
void CloudFx_OnLevelInit();

// 1.0 = the map's own authored value, for every one of these.
constexpr float kCloudDefault = 1.0f;

void  CloudFx_SetDensity(float mul);   // g_flOpacityScale  -- thinner / thicker cloud cover
void  CloudFx_SetBrightness(float mul); // g_flColorBoost   -- dimmer / brighter clouds
void  CloudFx_SetSize(float mul);      // g_vMask*Scale     -- bigger, sparser / smaller, busier
void  CloudFx_SetDrift(float mul);     // g_vMask*PanSpeed  -- how fast they scroll

float CloudFx_Density();
float CloudFx_Brightness();
float CloudFx_Size();
float CloudFx_Drift();

// Re-writes every cloud material from the current multipliers. Call after a map change, since
// materials are reloaded and come back with their authored values.
void CloudFx_Apply();

// Is any cloud multiplier away from 1.00? Same purpose as SunFx_HasOverride: Apply() snapshots
// every matching material on first sight, so a caller with nothing to re-assert should not call it.
bool CloudFx_HasOverride();

// Restores every touched material and forgets the snapshots.
void CloudFx_ResetAll();

// How many cloud materials the last Apply() actually wrote. 0 after a map change with no clouds
// is the honest answer for a map that has none -- it is not a failure.
int CloudFx_MaterialCount();

void CloudFx_AppendStateJson(std::ostringstream& o);

void CloudFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
