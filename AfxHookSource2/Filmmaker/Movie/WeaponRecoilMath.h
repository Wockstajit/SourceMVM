#pragma once
#include "WeaponRecoilStyles.h"
#include <cmath>

namespace Filmmaker {

// Shared spring step used by Weapon Recoil. Punch itself is still the old impulse-time scale
// on the back/up/side channels in WeaponRecoil::Advance -- not a post-integration distance
// multiplier -- so this only owns the ARC9 return force every channel already used.
inline void RecoilSpringStep(double& offset, double& velocity, const RecoilChannelRef& channel,
    double snap, double recovery, double dt) {
    const double sign = offset > 0.0 ? 1.0 : offset < 0.0 ? -1.0 : 0.0;
    const double acceleration = -offset * std::fabs(offset) * channel.kQuad * snap
        - sign * channel.kMag * snap - velocity * channel.damp * recovery
        - velocity * std::fabs(velocity) * 0.5 * recovery;
    velocity += acceleration * dt;
    double next = offset + velocity * dt;
    // The constant-magnitude pull would chatter across zero forever on its own; once it is the
    // only force left, snap to rest (ARC9 kills the tail the same way with a hard reset
    // threshold). A fast crossing is left alone so the spring can overshoot.
    if (sign != 0.0 && next * sign < 0.0 && std::fabs(velocity) < channel.kMag * 2.0) {
        next = 0.0;
        velocity = 0.0;
    }
    offset = next;
}

}
