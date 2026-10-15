#include "../../AfxHookSource2/Filmmaker/Movie/CameraCenterLockMath.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void ExpectNear(const char* name, double actual, double expected) {
	if (std::abs(actual - expected) <= 1e-9) return;
	std::cerr << name << ": expected " << expected << ", got " << actual << "\n";
	std::exit(1);
}

void ExpectAngles(const char* name, bool enabled, double strength, double expectedPitch,
	double expectedYaw) {
	double pitch = 4.0;
	double yaw = -6.0;
	Filmmaker::CameraCenterLockApply(enabled, strength, pitch, yaw);
	ExpectNear(name, pitch, expectedPitch);
	ExpectNear(name, yaw, expectedYaw);
}

} // namespace

int main() {
	ExpectAngles("disabled", false, 100.0, 4.0, -6.0);
	ExpectAngles("zero", true, 0.0, 4.0, -6.0);
	ExpectAngles("half", true, 50.0, 2.0, -3.0);
	ExpectAngles("full", true, 100.0, 0.0, 0.0);
	ExpectAngles("clamp low", true, -25.0, 4.0, -6.0);
	ExpectAngles("clamp high", true, 125.0, 0.0, 0.0);
	return 0;
}
