#pragma once
#include <cmath>
namespace Deadlock {
inline double CameraFrameDelta(double seconds) {
    return !std::isfinite(seconds) || seconds < 0 ? 0 : (seconds > 0.1 ? 0.1 : seconds);
}
inline bool ScaleFovForAspect(double fov, double engineAspect, double wantedAspect, double& result) {
    if (!std::isfinite(fov) || fov <= 1 || fov >= 179 ||
        !std::isfinite(engineAspect) || engineAspect <= 0 ||
        !std::isfinite(wantedAspect) || wantedAspect <= 0) return false;
    constexpr double pi = 3.14159265358979323846;
    result = 360 / pi * std::atan(std::tan(fov * pi / 360) * wantedAspect / engineAspect);
    return std::isfinite(result) && result > 1 && result < 179;
}
}
