#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentFootprintMath.h"
#include <cstdlib>
#include <iostream>
#include <memory>
using namespace Filmmaker::Environment;
void Check(bool ok, const char *why) {
    if (!ok) {
        std::cerr << why << '\n';
        std::exit(1);
    }
}
int main() {
    {
        // Walking 340 units in a straight line puts a foot down every 34, on alternate sides, and
        // the trail starts and ends with both feet side by side.
        FootTrack track;
        FootStep step[2];
        bool grounded = false;
        int steps = 0, lefts = 0;
        float lastSide = 0;
        const float dt = 1 / 64.f; // one unit a tick is 64 units a second: a walk
        Check(!AdvanceFootTrack(track, {0, 0, 0}, dt, step, grounded) && !grounded, "The first sight of a pawn is no step");
        Check(AdvanceFootTrack(track, {1, 0, 0}, dt, step, grounded) == 2 && grounded, "Setting off marks both feet");
        Check(std::abs(step[0].position.x) < 1e-3f && std::abs(step[1].position.x) < 1e-3f && std::abs(step[0].position.y - kFootSide) < 1e-3f &&
                  std::abs(step[1].position.y + kFootSide) < 1e-3f,
              "They stand side by side where the pawn stood, across the way it goes");
        for (int i = 2; i <= 341; ++i) {
            const int landed = AdvanceFootTrack(track, {float(i), 0, 0}, dt, step, grounded);
            Check(landed <= 1, "Walking puts one foot down at a time");
            if (landed) {
                ++steps;
                Check(std::abs(std::abs(step[0].position.y) - kFootSide) < 1e-3f, "A foot lands beside the path, not on it");
                Check(step[0].position.y * lastSide <= 0, "Feet alternate sides");
                lastSide = step[0].position.y;
                lefts += step[0].position.y > 0;
                Check(std::abs(step[0].heading) < 1e-3f && grounded, "A step faces the way the pawn moves");
            }
        }
        Check(steps == 10 && lefts == 5, "One step every 34 units");
        Check(AdvanceFootTrack(track, {341, 0, 0}, 0, step, grounded) == 0, "A paused demo makes no mark");
        Check(AdvanceFootTrack(track, {341, 0, 0}, dt, step, grounded) == 2 && !grounded, "Coming to a stop brings the feet together");
        Check(std::abs(step[0].position.x - 341) < 1e-3f && std::abs(step[0].position.y - step[1].position.y) > 2 * kFootSide - 1e-3f, "Side by side where it stopped");
        Check(!AdvanceFootTrack(track, {341, 0, 0}, dt, step, grounded) && !grounded, "Standing still makes no more marks");
        Check(AdvanceFootTrack(track, {342, 0, 0}, dt, step, grounded) == 0 && grounded, "Setting off again from marked ground adds no pair");
        Check(!AdvanceFootTrack(track, {347, 0, 60}, dt, step, grounded) && !grounded, "A jump leaves no mark");
        Check(!AdvanceFootTrack(track, {2000, 0, 60}, dt, step, grounded) && !grounded, "A teleport leaves no mark");
    }
    {
        // A pawn's own feet: how high a sole is, how far it presses the snow, and when it comes down.
        Check(std::abs(FootSole(100 + kFootAnkle, 100 + kFootToe, 100)) < 1e-4f, "A foot flat on the ground has its sole on it");
        Check(std::abs(FootSole(100 + kFootAnkle + 9, 100 + kFootToe + 5, 100) - 5) < 1e-3f, "The lower of heel and toe decides how high a foot is");
        Check(FootSole(100 + kFootAnkle + 6, 100 + kFootToe - 3, 100) == 0, "A toe bone dipping under the ground as the foot pushes off is a foot on the ground");
        {
            // A model whose bones sit a unit higher: its foot is still down when it stands.
            FootState other;
            Check(FootLift(other, 1, 1 / 64.f) == 0, "A foot is read against its own lowest level");
            Check(std::abs(FootLift(other, 7, 1 / 64.f) - 6) < .05f, "Lifted six units it is six units up");
            Check(FootLift(other, 1, 1 / 64.f) < .05f, "And down again when it comes back");
            FootState high;
            Check(std::abs(FootLift(high, 9, 1 / 64.f) - (9 - kFootBaseMost)) < 1e-3f, "A foot first seen in the air is not taken to be standing");
            FootState dipped;
            FootLift(dipped, -5, 1 / 64.f);
            Check(FootLift(dipped, .5f, 1 / 64.f) < kFootDown, "A sole that dipped far down in a landing does not leave the foot reading as lifted");
        }
        Check(FootPress(0) == 1 && FootPress(kFootClear) == 0 && FootPress(100) == 0, "A foot on the ground presses snow to its bed, a foot lifted clear presses nothing");
        Check(std::abs(FootPress(kFootClear * .5f) - .5f) < 1e-3f && FootPress(2) > .85f, "The leg cuts the snow down to the height of the sole: a foot hardly lifted drags a furrow nearly to the bed");
        {
            // A foot is set down when it is low and has stopped travelling, not when it first gets low.
            FootState foot;
            Check(!FootSetDown(foot, 0, 0, 0) && foot.planted, "The first sight of a standing foot is not a step");
            foot.valid = true;
            Check(!FootSetDown(foot, 0, 0, 0), "A foot that stays down was set down once");
            Check(!FootSetDown(foot, 9, 480, 250) && !foot.planted, "Lifted and swinging, it is up");
            Check(!FootSetDown(foot, 1.2f, 380, 250), "Low but still flying forward is not down yet");
            Check(FootSetDown(foot, .4f, 30, 250), "It is set down in the frame it comes to rest");
            Check(!FootSetDown(foot, .3f, 60, 250) && foot.planted, "A planted foot that creeps a little stays planted");
            Check(!FootSetDown(foot, .5f, 300, 250) && !foot.planted, "A foot dragged along the ground is not planted");
            // A slow walk: the pawn does 80, a swinging foot 160.
            FootState slow;
            slow.valid = true;
            Check(!FootSetDown(slow, 3.5f, 160, 80) && FootSetDown(slow, .5f, 20, 80), "A slow walk sets its feet down too");
        }
    }
    {
        auto image = std::make_unique<FootImage>();
        image->Centre({100, 100, 0});
        const Vec3 at{120, 90, 64};
        Check(image->Press(at, 0, 1, true), "Pressing fresh snow changes the image");
        Check(!image->Press(at, 0, 1, true), "A foot standing still changes nothing more");
        const int x = int(std::floor(120 / kFootTexel)), y = int(std::floor(90 / kFootTexel));
        Check(image->At(x, y)[0] > 200 && image->At(x, y)[2] == FootLayer(64), "A print is deep at its middle and keeps its height layer");
        Check(image->At(x + int(12 / kFootTexel), y)[0] == 0 && image->At(x, y + int(6 / kFootTexel))[0] == 0, "A print is a small oval: 12 units along, 5 across");
        Check(image->At(x, y)[3] == 255 && image->At(x + int(5 / kFootTexel), y)[3] == 255 && image->At(x, y + int(2 / kFootTexel))[3] == 255,
              "Under the boot the snow is pressed to its bed");
        Check(image->At(x, y + int(3.8f / kFootTexel))[3] > 0 && image->At(x, y + int(3.8f / kFootTexel))[3] < 255, "The wall of the hole slopes out beyond the boot");
        Check(image->At(x, y + int(9 / kFootTexel))[3] == 0 && image->At(x + int(13.5f / kFootTexel), y)[3] == 0 && image->At(x - int(13.5f / kFootTexel), y)[3] == 0,
              "The hole is the size of a boot: nothing is pressed a boot's width away");
        {
            float low = 1, high = -1;
            for (int i = 0; i < 400; ++i) {
                const float r = FootRough(i * 3, i * 7 - 900);
                low = (std::min)(low, r);
                high = (std::max)(high, r);
            }
            Check(low >= -.11f && high <= .11f && high - low > .1f, "The hole's edge is ragged by up to 0.11 of its width, and not all one way");
            float jump = 0;
            for (int i = 0; i < 400; ++i) jump = (std::max)(jump, std::abs(FootRough(i, 37) - FootRough(i + 1, 37)));
            Check(jump < .05f, "The raggedness is smooth from one texel to the next: no jagged stair along an edge");
        }
        {
            // A foot swinging forward low through the snow: a shallow groove all the way, no gaps.
            auto swing = std::make_unique<FootImage>();
            swing->Centre({0, 0, 0});
            Check(swing->Sweep({0, 0, 0}, {30, 0, 0}, 0, .4f, .4f), "A sweep marks the snow");
            const int row = int(std::floor(0 / kFootTexel));
            int least = 255, most = 0;
            for (float along = 0; along <= 30; along += kFootTexel) {
                const int v = swing->At(int(std::floor(along / kFootTexel)), row)[3];
                least = (std::min)(least, v);
                most = (std::max)(most, v);
            }
            Check(least > 90 && most <= 103, "The groove is as deep as the foot was low, with no gap along it");
            Check(swing->At(int(std::floor(15 / kFootTexel)), row)[0] == 0, "A swinging foot leaves no print");
            Check(!swing->Sweep({0, 0, 0}, {30, 0, 0}, 0, 0, 0), "A foot lifted clear marks nothing");
        }
        {
            // Two feet close together: one hole, deeper between them than either pressed alone.
            auto pair = std::make_unique<FootImage>();
            pair->Centre({0, 0, 0});
            pair->Press({0, 5, 0}, 0, 1, false);
            const int my = int(std::floor(0 / kFootTexel));
            const int one = pair->At(0, my)[3];
            pair->Press({0, -1.5f, 0}, 0, 1, false);
            Check(one < 255 && pair->At(0, my)[3] == 255, "Holes that overlap are one hole");
        }
        {
            // A body lying on the ground presses a hollow its own shape; the same body upright presses
            // only where its feet are.
            FallenBones lying;
            auto set = [&](FallenBones &b, int bone, Vec3 at) {
                b.bone[bone] = at;
                b.has[bone] = true;
            };
            set(lying, BB_pelvis, {0, 0, 105});
            set(lying, BB_spine, {8, 0, 105});
            set(lying, BB_chest, {18, 0, 106});
            set(lying, BB_head, {30, 0, 104});
            set(lying, BB_legL, {-3, 4, 104});
            set(lying, BB_kneeL, {-20, 5, 103});
            set(lying, BB_ankleL, {-38, 6, 103});
            float ground = 0;
            Check(BodyGround(lying, ground) && std::abs(ground - 100) < 1e-3f, "A body's ground is a little under its lowest bone");
            auto bed = std::make_unique<FootImage>();
            bed->Centre({0, 0, 0});
            Check(bed->PressBody(lying, ground), "A body lying in snow marks it");
            auto at = [&](FootImage &image, float px, float py) { return int(image.At(int(std::floor(px / kFootTexel)), int(std::floor(py / kFootTexel)))[3]); };
            Check(at(*bed, 0, 0) == 255 && at(*bed, 18, 0) == 255 && at(*bed, 30, 0) == 255 && at(*bed, -30, 5.5f) > 240, "It is pressed to the bed under the trunk, the head and the leg");
            Check(at(*bed, 10, 14) == 0 && at(*bed, -30, -8) == 0 && at(*bed, 45, 0) == 0, "The snow beside the body is untouched: the hollow is the body's shape");
            Check(at(*bed, 9, 0) == 255, "There is no gap between one part of the body and the next");
            Check(!bed->PressBody(lying, ground), "A body lying still changes nothing more");
            FallenBones upright;
            set(upright, BB_pelvis, {0, 0, 138});
            set(upright, BB_chest, {0, 0, 154});
            set(upright, BB_head, {0, 0, 166});
            set(upright, BB_ankleL, {0, 5, 103});
            set(upright, BB_kneeL, {0, 5, 120});
            auto stood = std::make_unique<FootImage>();
            stood->Centre({0, 0, 0});
            stood->PressBody(upright, 100);
            Check(at(*stood, 0, 5) == 255 && at(*stood, 0, -6) == 0, "A body still upright presses only where its foot is");
        }
        image->Fill(100);
        Check(image->At(x, y)[0] < 160 && image->At(x, y)[0] > 0, "Falling snow fills a print back in");
        Check(image->At(x, y)[3] == 205, "Pressed snow fills at half the pace of a print");
        Check(image->At(x, y + int(3.8f / kFootTexel))[3] < 205, "A hole fills from the bottom: its wall stays lower than its middle");
        // Marks outside the window are not written; the window follows the camera and wipes what scrolls in.
        Check(!image->Press({100 + 1200, 100, 0}, 0, 1, true), "A foot beyond the window is not kept");
        Check(image->At(int(std::floor(1300 / kFootTexel)), int(std::floor(100 / kFootTexel)))[0] == 0, "Nothing was written for it");
        Check(image->Centre({100 + 600, 100, 0}), "Moving the camera wipes the edge that scrolls in");
        Check(image->At(x, y)[0] > 0, "Marks still inside the window stay where they are in the world");
        {
            // Only what changed is noted for sending: a boot's rectangle, not the image.
            auto small = std::make_unique<FootImage>();
            small->Centre({0, 0, 0});
            small->Sent();
            small->Press({40, 40, 0}, 0, 1, true);
            Check(!small->all && small->changed.size() == 1 && small->changed[0].x1 - small->changed[0].x0 < 40, "A footprint notes one small rectangle to send");
            small->Sent();
            small->Press({-2, 30, 0}, 0, 1, true);
            Check(!small->all && small->changed.size() == 2, "A mark across the image's seam notes a piece each side");
            small->Sent();
            small->FillRows(2, kFootSize - 4, 8);
            Check(!small->all && small->changed.size() == 2 && small->changed[0].x0 == 0 && small->changed[0].x1 == kFootSize - 1, "A band of rows filled across the seam is two full-width pieces");
        }
        image->Centre({100 + 5000, 100, 0});
        Check(image->At(x, y)[0] == 0, "A camera cut far away starts clean");
    }
    {
        // How hard a foot comes down: a walk throws less than a run, a landing more than either, and
        // a drop more than a jump in place.
        const float walk = FootImpact(130, 0), run = FootImpact(250, 0), jump = FootImpact(250, 300), drop = FootImpact(250, 750);
        Check(walk < run && run < jump && jump < drop, "Walk, run, jump and drop splash in that order");
        Check(walk > .45f && walk < .65f && run > .75f && run < .95f, "A walk and a run are about .55 and .85");
        Check(jump >= 1.5f && drop > 2.5f && drop <= 2.6f, "A landing is 1.5 and up, 2.6 at most");
        Check(FootImpact(0, 100) == FootImpact(0, 0), "A step down a kerb is not a landing");
    }
    {
        // A jump in place at 60 frames a second: up at 300 units a second, down again under gravity.
        FootLanding l;
        const float dt = 1.f / 60, g = 800;
        float z = 0, vz = 0;
        bool jumped = false;
        for (int f = 0; f < 10; ++f) jumped |= AdvanceFootLanding(l, z, true, dt);
        Check(!jumped && FootLandingSpeed(l) == 0, "Standing still is neither a jump nor a landing");
        vz = 301;
        int frames = 0;
        do {
            z += vz * dt;
            vz -= g * dt;
            jumped |= AdvanceFootLanding(l, (std::max)(z, 0.f), false, dt);
            ++frames;
        } while (z > 0 && frames < 200);
        Check(jumped, "Leaving the ground going up is a jump");
        AdvanceFootLanding(l, 0, true, dt);
        Check(FootLandingSpeed(l) > 250 && FootLandingSpeed(l) < 340, "It lands at about the speed it left");
        for (int f = 0; f < 30; ++f) AdvanceFootLanding(l, 0, true, dt);
        Check(FootLandingSpeed(l) == 0, "The landing is over after a third of a second");
        // Walking off a ledge: no jump, a landing.
        FootLanding ledge;
        AdvanceFootLanding(ledge, 128, true, dt);
        bool ledgeJump = false;
        z = 128;
        vz = 0;
        while (z > 0) {
            z += vz * dt;
            vz -= g * dt;
            ledgeJump |= AdvanceFootLanding(ledge, (std::max)(z, 0.f), false, dt);
        }
        AdvanceFootLanding(ledge, 0, true, dt);
        Check(!ledgeJump && FootLandingSpeed(ledge) > 400, "A drop off a ledge lands hard without a jump");
    }
    std::cout << "Footprint math passed\n";
}
