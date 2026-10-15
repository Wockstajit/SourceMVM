#pragma once

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

struct DeadlockPathShakeSetting {
	int style = -1;
	int intensity = 5;
	int speed = 50;
	bool Active() const { return style >= 0 && intensity > 0; }
	bool operator==(const DeadlockPathShakeSetting& o) const {
		return style == o.style && intensity == o.intensity && speed == o.speed;
	}
	bool operator!=(const DeadlockPathShakeSetting& o) const { return !(*this == o); }
};

void DeadlockPathShake_RunFrame();
bool DeadlockPathShake_ApplyToView(float& x, float& y, float& z,
	float& pitch, float& yaw, float& roll, float& fov);
void DeadlockPathShake_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
void DeadlockPathShake_AppendStateJson(std::string& out);

} // namespace Filmmaker
