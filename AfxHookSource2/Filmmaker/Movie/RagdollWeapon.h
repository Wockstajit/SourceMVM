#pragma once

#include <sstream>

namespace advancedfx { class ICommandArgs; }
namespace SOURCESDK { namespace CS2 { class CGameEvent; } }
class CEntityInstance;

namespace Filmmaker {

bool RagdollWeapon_KeepEnabled();
bool RagdollWeapon_FireEnabled();
double RagdollWeapon_FireChance();
double RagdollWeapon_FireDurationMs();
double RagdollWeapon_HoldDurationMs();
void RagdollWeapon_SetFireChance(double value);
void RagdollWeapon_SetFireDurationMs(double value);
void RagdollWeapon_SetHoldDurationMs(double value);
void RagdollWeapon_SetKeepEnabled(bool enabled);
void RagdollWeapon_SetFireEnabled(bool enabled);

void RagdollWeapon_OnGameEvent(SOURCESDK::CS2::CGameEvent* event);
void RagdollWeapon_OnEntityAdded(CEntityInstance* entity, unsigned int handle);
void RagdollWeapon_OnEntityRemoved(CEntityInstance* entity);
void RagdollWeapon_RunFrame();
void RagdollWeapon_RunLateFrame();
void RagdollWeapon_RunPostFrame();
void RagdollWeapon_ProbeDraw(void* batch, int count, const char* material);
void RagdollWeapon_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
void RagdollWeapon_AppendStateJson(std::ostringstream& out);

} // namespace Filmmaker
