#pragma once
#include "MethodActingMath.h"
class CEntityInstance;
namespace Filmmaker { namespace MethodActingGaze {
// Read the rendered skeleton; eye rotations and a subtle living head follow are restored each frame.
struct FaceView { float position[3] = {}, headRotation[4] = {}; };
bool ReadFace(CEntityInstance* entity, FaceView& view);
bool Apply(CEntityInstance* entity, MethodActingMath::Gaze gaze, double headYaw = 0.0, bool moveEyeTarget = false);
// Explicit disable restores the saved pre-effect pose even if CS2 normalized/copied it.
// Per-frame release only restores values still matching our writes.
void Release(CEntityInstance* entity,bool force = false);
void ReleaseAll(bool force = false);
unsigned long long Writes();
unsigned long long Faults();
} }
