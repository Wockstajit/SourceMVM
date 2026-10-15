#include "../../AfxHookSource2/Deadlock/DeadlockViewMath.h"
#include <cstdio>
#include <limits>
int main() {
    int failures = 0;
    auto check = [&](bool value) { if (!value) ++failures; };
    double fov = 0;
    check(Deadlock::ScaleFovForAspect(90, 1, 1, fov) && std::abs(fov - 90) < 1e-9);
    check(Deadlock::ScaleFovForAspect(90, 1, 2, fov) && std::abs(fov - 126.869897645844) < 1e-9);
    check(Deadlock::ScaleFovForAspect(fov, 2, 1, fov) && std::abs(fov - 90) < 1e-9);
    check(!Deadlock::ScaleFovForAspect(90, 0, 1, fov));
    check(!Deadlock::ScaleFovForAspect(180, 1, 1, fov));
    check(!Deadlock::ScaleFovForAspect(90, 1, std::numeric_limits<double>::infinity(), fov));
    check(Deadlock::CameraFrameDelta(0.016) == 0.016);
    check(Deadlock::CameraFrameDelta(2) == 0.1);
    check(Deadlock::CameraFrameDelta(-1) == 0);
    check(Deadlock::CameraFrameDelta(std::numeric_limits<double>::quiet_NaN()) == 0);
    std::printf("Deadlock view math: %d failures\n", failures);
    return failures ? 1 : 0;
}
