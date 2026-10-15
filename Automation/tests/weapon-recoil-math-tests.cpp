#include "../../AfxHookSource2/Filmmaker/Movie/WeaponRecoilMath.h"
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
using namespace Filmmaker;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

int main() { try {
    for (auto style : {RecoilStyle::Mw2019, RecoilStyle::Mw2022}) {
        const auto& channel = RecoilStyleProfileFor(style).channel[0];
        for (double snap : {0.25, 1.0, 4.0}) for (double recovery : {0.25, 0.65, 1.0, 4.0}) {
            double offset = channel.instant * 1.5, velocity = channel.kick * 1.5;
            for (int i = 0; i < 120000; ++i) {
                if (i > 0 && i < 1000 && i % 100 == 0) {
                    const double room = std::fmax(0.15, 1.0 - std::fabs(offset) / (channel.cap * 1.5));
                    offset += channel.instant * room * 1.5;
                    velocity += channel.kick * room * 1.5;
                }
                RecoilSpringStep(offset, velocity, channel, snap, recovery, 0.001);
                Check(std::isfinite(offset) && std::isfinite(velocity),
                    "punch spring must stay finite over a sustained burst");
            }
            if (offset != 0.0 || velocity != 0.0)
                std::cerr << "snap=" << snap << " recovery=" << recovery
                    << " offset=" << offset << " velocity=" << velocity << '\n';
            Check(offset == 0.0 && velocity == 0.0, "punch must settle at rest");
        }

        // Rotation amplitudes kept from the roll/yaw retune: barrel twist must dominate climb.
        const RecoilStyleProfile& profile = RecoilStyleProfileFor(style);
        Check(profile.channel[5].instant > profile.channel[3].instant * 2.0,
            "barrel roll must dominate the muzzle climb");
        Check(profile.channel[4].instant > profile.channel[3].instant * 0.5,
            "the lateral swing must not be a rounding error");

        double angle = 0.01, velocity = -100.0;
        RecoilSpringStep(angle, velocity, channel, 1, 1, 0.001);
        Check(angle < 0, "rotation must retain its existing overshoot");
    }
    std::cout << "Recoil punch trajectories passed\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
