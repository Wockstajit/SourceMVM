// The Environment sky: the atmosphere port, the moon and the sun-or-moon key light.
// Source under test: EnvironmentSkyMath.h, EnvironmentClimateMath.h and EnvironmentGiMath.h.
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentSkyMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentGiMath.h"
#include <cstdlib>
#include <iostream>

using namespace Filmmaker::Environment;
namespace {
void Check(bool value, const char *message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
float Luminance(Vec3 v) { return .2126f * v.x + .7152f * v.y + .0722f * v.z; }
Vec3 Direction(float azimuth, float elevation) {
    const float a = Radians(azimuth), e = Radians(elevation);
    return {std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e)};
}
Climate Sky(float sunElevation) {
    Climate c;
    c[CL_enabled] = 1; c[CL_sky] = 1;
    c[CL_sunAzimuth] = 0; c[CL_sunElevation] = sunElevation;
    c[CL_moonAzimuth] = 300; c[CL_moonElevation] = 35; // up: the default time of day has it down
    return c;
}
} // namespace

int main() {
    using namespace SkyAtmosphere;
    // The port reproduces glsl-atmosphere's look: a blue zenith at noon, a sky that reddens toward
    // the horizon at sunset, and a dark sky once the sun is well down.
    {
        const Vec3 zenith = Scatter({0, 0, 1}, Direction(0, 60), 1);
        Check(zenith.z > zenith.y && zenith.y > zenith.x, "The midday zenith is blue");
        const Vec3 sunset = Scatter(Direction(20, 3), Direction(0, 2), 1);
        Check(sunset.x > sunset.z, "Looking toward a low sun the horizon is red, not blue");
        const Vec3 night = Scatter({0, 0, 1}, Direction(0, -10), 1);
        Check(Luminance(night) < Luminance(zenith) * .01f, "Ten degrees after sunset the scattered daylight is gone");
        const Vec3 below = Scatter(Direction(0, -20), Direction(0, 40), 1);
        Check(Luminance(below) > 0, "Rays below the horizon take the horizon's colour, never black");
        // Haze is Mie scattering: a bright glow toward the sun, and a whiter (less blue) sky everywhere.
        const Vec3 clearSun = Scatter(Direction(10, 5), Direction(0, 35), MieScale(0));
        const Vec3 hazySun = Scatter(Direction(10, 5), Direction(0, 35), MieScale(2));
        Check(Luminance(hazySun) > Luminance(clearSun), "Haze brightens the horizon under the sun");
        const Vec3 clearAway = Scatter(Direction(180, 5), Direction(0, 35), MieScale(0));
        const Vec3 hazyAway = Scatter(Direction(180, 5), Direction(0, 35), MieScale(2));
        Check(hazyAway.z / hazyAway.x < clearAway.z / clearAway.x, "Haze whitens the horizon away from the sun");
    }
    // The moon takes over as the key light once the sun has set, with no jump at the hand-over.
    {
        Climate day = Sky(30);
        Check(!ClimateMoonKey(day), "By day the sun is the key light");
        Check(Dot(ClimateSunDirection(day), ClimateTrueSunDirection(day)) > .9999f, "The day key light points at the sun");
        Climate night = Sky(-12);
        Check(ClimateMoonKey(night), "At night the moon is the key light");
        Check(Dot(ClimateSunDirection(night), ClimateMoonDirection(night)) > .9999f, "The night key light points at the moon");
        Check(ClimateKeyPower(night) > 0, "A full moon above the horizon lights the map");
        Check(ClimateSamplesSunShadow(night, true, false), "Moonlight casts shadows");
        Climate dusk = Sky(1e-3f), after = Sky(-1e-3f);
        Check(ClimateKeyPower(dusk) < 1e-3f && ClimateKeyPower(after) < 1e-3f,
              "The key light is dark on both sides of the hand-over, so its change of direction never shows");
        const float twilight = ClimateKeyPower(Sky(-3)), full = ClimateKeyPower(Sky(-8));
        Check(twilight > 0 && twilight < full, "Moonlight fades in through civil twilight");
        Climate set = night; set[CL_moonElevation] = -5;
        Check(ClimateKeyPower(set) == 0 && !ClimateSamplesSunShadow(set, true, false), "A set moon gives no light or shadows");
        Climate none = night; none[CL_moon] = 0;
        Check(!ClimateMoonKey(none) && ClimateKeyPower(none) == 0, "With the moon off the night stays dark, as before");
    }
    // Phase: a new moon gives no light, a quarter about half of a full one.
    {
        Check(MoonIllumination(0) < 1e-4f && MoonIllumination(1) < 1e-4f, "A new moon is dark");
        Check(std::abs(MoonIllumination(.5f) - 1) < 1e-4f, "A full moon is fully lit");
        Check(std::abs(MoonIllumination(.25f) - .5f) < 1e-3f, "A quarter moon is half lit");
        Climate full = Sky(-12), quarter = full;
        quarter[CL_moonPhase] = .25f;
        Check(std::abs(ClimateMoonPower(quarter) - ClimateMoonPower(full) * .5f) < 1e-4f, "Moonlight follows the phase");
    }
    // The key light's colour: the sun's ramp by day, the moon's colour by night.
    {
        Climate night = Sky(-12);
        night[CL_moonColorR] = .5f; night[CL_moonColorG] = .6f; night[CL_moonColorB] = 1;
        const Vec3 moon = ClimateSunRadiance(night);
        Check(moon.z > moon.x && std::abs(moon.z - ClimateMoonPower(night)) < 1e-5f, "Moonlight takes the moon's colour");
        const Vec3 sun = ClimateTrueSunRadiance(night);
        Check(Luminance(sun) == 0, "The sun itself gives nothing below the horizon");
        Climate day = Sky(40);
        const Vec3 a = ClimateSunRadiance(day), b = ClimateTrueSunRadiance(day);
        Check(a.x == b.x && a.y == b.y && a.z == b.z, "By day the key light is exactly the sun, as before");
    }
    // The sky light the map takes follows the sky: bright and blue by day, dim by night, and the
    // moonlit sky is brighter than a moonless one.
    {
        const Vec3 day = ClimateSkyIrradiance(Sky(40));
        Check(day.z > day.x && Luminance(day) > .05f, "Daylight sky light is blue and bright");
        Climate dark = Sky(-20); dark[CL_moon] = 0;
        Climate lit = Sky(-20);
        const float moonless = Luminance(ClimateSkyIrradiance(dark)), moonlit = Luminance(ClimateSkyIrradiance(lit));
        Check(moonless < Luminance(day) * .1f, "The night sky is far darker than the day sky");
        Check(moonlit > moonless, "A moonlit sky lights the map more than a moonless one");
        Check(moonlit < Luminance(day) * .2f, "Moonlight stays a night light");
    }
    std::cout << "environment sky math tests passed\n";
    return 0;
}
