#define NOMINMAX
#include "EnvironmentFootprints.h"
#include "EnvironmentFootprintMath.h"
#include "../Debug/MvmDebugLog.h"
#include <wrl/client.h>
#include <map>

namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
namespace {
struct Feet {
    FootState foot[2];
    Vec3 origin{}; // where the pawn was at the last look, for how fast it moves
    bool seen = false;
    FootLanding landing; // off the ground and back: how hard it landed (FootImpact)
};
// A dead player's body from frame to frame: pressed again only when it has moved (or once a second,
// against falling snow filling in under it), and one splash where it comes down.
struct Fallen {
    FallenBones bones;
    double pressed = -1e9;
    bool landed = false;
};
struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    FootImage image;
    std::map<int, FootTrack> tracks; // pawns whose bones cannot be read
    std::map<int, Feet> feet;
    std::map<int, Fallen> fallen;
    const OcclusionMesh *world = nullptr;
    double time = 0, filled = 0;
    int fillRow = 0; // where the falling snow has filled the marks up to (a band of rows a frame)
    float steps[kFootSteps][4]{};
    float impact[kFootSteps]{}; // FootImpact of each step
    float live[kFootLive][4]{};
    int liveCount = 0;
    int stepCount = 0, stepNext = 0;
    bool dirty = true;
};
State *g_state = nullptr;
} // namespace
void EndFootprints() {
    delete g_state;
    g_state = nullptr;
}
FootprintBinding PrepareFootprints(ID3D11Device *device, ID3D11DeviceContext *ctx, const Frame &frame, float clock) {
    if (!device || !ctx) return {};
    if (g_state && g_state->device.Get() != device) EndFootprints();
    if (!g_state) {
        g_state = new State;
        g_state->device = device;
        g_state->time = g_state->filled = frame.time;
    }
    auto &s = *g_state;
    if (!s.texture) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = kFootSize;
        // With its chain of smaller copies (made again after every change): the shader reads one
        // texel of a coarse copy to learn whether anything is marked near a point, and skips the
        // whole trace of holes where nothing is, which is nearly all the snow on screen.
        d.ArraySize = d.SampleDesc.Count = 1;
        d.MipLevels = 0;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &s.texture)) ||
            FAILED(device->CreateShaderResourceView(s.texture.Get(), nullptr, &s.view))) {
            s.texture.Reset();
            s.view.Reset();
            return {};
        }
        s.dirty = true;
    }
    // Another map, or the demo's clock going back or leaping (a seek): the marks belong to a
    // moment that is gone.
    if (s.world != frame.occlusion.get() || frame.time < s.time - .05 || frame.time > s.time + 5) {
        s.world = frame.occlusion.get();
        s.image.Clear();
        s.tracks.clear();
        s.feet.clear();
        s.fallen.clear();
        s.stepCount = s.stepNext = 0;
        s.filled = frame.time;
        s.dirty = true;
    }
    const float dt = float(frame.time - s.time);
    s.time = frame.time;
    if (s.image.Centre(frame.camera)) s.dirty = true;
    std::map<int, FootTrack> seen;
    std::map<int, Feet> seenFeet;
    s.liveCount = 0;
    auto splash = [&](Vec3 at, float impact) {
        float *slot = s.steps[s.stepNext];
        slot[0] = at.x;
        slot[1] = at.y;
        slot[2] = at.z;
        slot[3] = clock;
        s.impact[s.stepNext] = impact;
        s.stepNext = (s.stepNext + 1) % kFootSteps;
        s.stepCount = (std::min)(s.stepCount + 1, kFootSteps);
    };
    for (const auto &actor : frame.actors) {
        if (actor.hasFeet) {
            // The pawn's own feet: each presses the snow to its own height wherever it goes, and
            // the frame a sole reaches the ground is a print, and a splash where there is water.
            Feet feet = s.feet.count(actor.entity) ? s.feet[actor.entity] : Feet{};
            const float ox = actor.origin.x - feet.origin.x, oy = actor.origin.y - feet.origin.y;
            const float pawnSpeed = feet.seen && dt > 0 ? (std::min)(std::sqrt(ox * ox + oy * oy) / dt, 600.f) : 0;
            if (dt > 0) {
                feet.origin = actor.origin;
                feet.seen = true;
            }
            // A jump kicks water up from both feet as they leave the ground, and the steps it lands
            // on throw the biggest splashes (FootImpact).
            const bool jumped = AdvanceFootLanding(feet.landing, actor.origin.z, actor.grounded, dt);
            const float landingSpeed = FootLandingSpeed(feet.landing);
            if (jumped) {
                for (int k = 0; k < 2; ++k) splash(feet.foot[k].valid ? feet.foot[k].last : actor.foot[k], .95f);
                if (MvmDebugLog_Active())
                    MvmDebugLog_Linef("environment.feet", "jump entity=%d time=%.4f z=%.1f", actor.entity, frame.time, actor.origin.z);
            }
            for (int k = 0; k < 2 && dt > 0; ++k) {
                FootState &foot = feet.foot[k];
                const Vec3 at = actor.foot[k];
                const float lift = actor.grounded ? FootLift(foot, FootSole(actor.ankleZ[k], actor.toeZ[k], actor.origin.z), dt) : 1e9f;
                const float press = FootPress(lift);
                const float dx = at.x - foot.last.x, dy = at.y - foot.last.y;
                // The same foot a frame on, not a respawn or a teleport.
                const bool follows = foot.valid && dx * dx + dy * dy < 48 * 48 && std::abs(at.z - foot.last.z) < 24;
                if (follows) s.dirty |= s.image.Sweep(foot.last, at, actor.footYaw[k], FootPress(foot.lift), press);
                else if (press > .02f) s.dirty |= s.image.Press(at, actor.footYaw[k], press, false);
                const float footSpeed = follows ? std::sqrt(dx * dx + dy * dy) / dt : 0;
                if (FootSetDown(foot, lift, footSpeed, pawnSpeed) && follows) {
                    s.dirty |= s.image.Press(at, actor.footYaw[k], 1, true);
                    const float impact = FootImpact(pawnSpeed, landingSpeed);
                    splash(at, impact);
                    if (MvmDebugLog_Active())
                        MvmDebugLog_Linef("environment.feet", "footfall entity=%d foot=%d time=%.4f at=(%.1f %.1f %.1f) lift=%.2f footSpeed=%.0f pawnSpeed=%.0f landing=%.0f impact=%.2f",
                                          actor.entity, k, frame.time, at.x, at.y, at.z, lift, footSpeed, pawnSpeed, landingSpeed, impact);
                }
                if (s.liveCount < kFootLive) {
                    float *live = s.live[s.liveCount++];
                    live[0] = at.x;
                    live[1] = at.y;
                    live[2] = actor.origin.z + (std::max)(FootSole(actor.ankleZ[k], actor.toeZ[k], actor.origin.z), 0.f);
                    live[3] = lift > 1e8f ? lift : foot.planted ? 0 : kFootDown + 1; // the view: green planted, red not, white in the air
                }
                foot.last = at;
                foot.lift = lift;
                foot.valid = true;
            }
            seenFeet[actor.entity] = feet;
            continue;
        }
        auto track = s.tracks.count(actor.entity) ? s.tracks[actor.entity] : FootTrack{};
        FootStep steps[2];
        bool grounded = false;
        const int landed = AdvanceFootTrack(track, actor.origin, dt, steps, grounded);
        for (int i = 0; i < landed; ++i) s.dirty |= s.image.Press(steps[i].position, steps[i].heading, 1, true);
        // Only a stride throws water: feet set down together where a pawn stands do not.
        if (landed == 1) splash(steps[0].position, .7f); // speed unknown here: between a walk and a run
        seen[actor.entity] = track;
    }
    s.tracks.swap(seen);
    s.feet.swap(seenFeet);
    // FIRE AND BLAST. A fire melts the snow round it, a little a frame, out to its edge in about two
    // seconds; an explosion throws it clear at once and leaves a shallow crater in what is left.
    for (const auto &heat : frame.snowHeat) {
        if (heat.age < 0) {
            if (dt > 0) s.image.Melt(heat.position, heat.radius, (std::min)(dt * 1.6f, .3f), true);
        } else if (heat.age < .6f) {
            s.image.Melt(heat.position, heat.radius, 1, false);
            s.image.PressRound({heat.position.x, heat.position.y, heat.position.z}, heat.radius * .45f, .7f);
        }
    }
    // The dead: each body presses the snow under it as it drops and where it lies.
    std::map<int, Fallen> seenFallen;
    for (const auto &body : frame.fallen) {
        Fallen fallen = s.fallen.count(body.entity) ? s.fallen[body.entity] : Fallen{};
        float ground = 0;
        if (!BodyGround(body.bones, ground)) continue;
        float moved = 1e9f;
        if (fallen.pressed > -1e8) {
            moved = 0;
            for (int k = 0; k < kBodyBones; ++k) {
                const Vec3 d = body.bones.bone[k] - fallen.bones.bone[k];
                if (body.bones.has[k]) moved = (std::max)(moved, std::abs(d.x) + std::abs(d.y) + std::abs(d.z));
            }
        }
        if (moved > .25f || frame.time - fallen.pressed > 1) {
            s.dirty |= s.image.PressBody(body.bones, ground);
            fallen.bones = body.bones;
            fallen.pressed = frame.time;
        }
        // The trunk reaching the ground is the thump: powder in snow, a splash in standing water.
        const Vec3 pelvis = body.bones.bone[BB_pelvis];
        if (!fallen.landed && pelvis.z - ground < 10 && moved < 1e8f) {
            fallen.landed = true;
            const Vec3 chest = body.bones.has[BB_chest] ? body.bones.bone[BB_chest] : pelvis;
            splash({pelvis.x, pelvis.y, ground}, 1.6f);
            splash({chest.x, chest.y, ground}, 1.4f);
            if (MvmDebugLog_Active())
                MvmDebugLog_Linef("environment.feet", "body down entity=%d time=%.4f pelvis=(%.1f %.1f %.1f) ground=%.1f", body.entity, frame.time, pelvis.x, pelvis.y, pelvis.z, ground);
        }
        seenFallen[body.entity] = fallen;
    }
    s.fallen.swap(seenFallen);
    // Falling snow fills the marks back in: about 45 seconds for a full print.
    // The whole image is gone over once every 0.35 seconds, a band of rows a frame.
    if (frame.time > s.filled) {
        const int rows = (std::min)(kFootSize, int(double(kFootSize) * (frame.time - s.filled) / .35));
        if (rows > 0) {
            s.filled = frame.time;
            // Snow cleared by a fire or a blast drifts back in about forty seconds, snowing or not.
            const bool snowing = frame.scene.weather.type == 2 && frame.scene.weather.count > 0;
            s.image.FillRows(snowing ? 2 : 0, s.fillRow, rows, snowing ? 3 : 2);
            s.fillRow = FootImage::Wrap(s.fillRow + rows);
        }
    }
    // Only what changed is sent (FootImage::Touch); the smaller copies are made again from it.
    if (s.dirty) s.image.all = true;
    if (s.image.all || !s.image.changed.empty()) {
        if (s.image.all)
            ctx->UpdateSubresource(s.texture.Get(), 0, nullptr, s.image.texels.data(), kFootSize * 4, 0);
        else
            for (const auto &b : s.image.changed) {
                const D3D11_BOX box{UINT(b.x0), UINT(b.y0), 0, UINT(b.x1 + 1), UINT(b.y1 + 1), 1};
                ctx->UpdateSubresource(s.texture.Get(), 0, &box, s.image.At(b.x0, b.y0), kFootSize * 4, 0);
            }
        ctx->GenerateMips(s.view.Get());
        s.image.Sent();
        s.dirty = false;
    }
    if (MvmDebugLog_Active()) {
        static double last = 0;
        if (std::abs(frame.time - last) > 2) {
            last = frame.time;
            size_t marked = 0;
            for (size_t i = 0; i < s.image.texels.size(); i += 4) marked += s.image.texels[i] || s.image.texels[i + 3];
            const CoatingActor first = frame.actors.empty() ? CoatingActor{} : frame.actors[0];
            MvmDebugLog_Linef("environment.footprints",
                              "actors=%zu fallen=%zu withFeet=%zu withoutFeet=%zu steps=%d markedTexels=%zu camera=(%.0f %.0f %.0f) first=(%.0f %.0f %.0f) grounded=%d "
                              "left=(%.1f %.1f) ankle+%.1f toe+%.1f right=(%.1f %.1f) ankle+%.1f toe+%.1f",
                              frame.actors.size(), frame.fallen.size(), s.feet.size(), s.tracks.size(), s.stepCount, marked, frame.camera.x, frame.camera.y, frame.camera.z,
                              first.origin.x, first.origin.y, first.origin.z, int(first.grounded), first.foot[0].x, first.foot[0].y,
                              first.ankleZ[0] - first.origin.z, first.toeZ[0] - first.origin.z, first.foot[1].x, first.foot[1].y,
                              first.ankleZ[1] - first.origin.z, first.toeZ[1] - first.origin.z);
        }
    }
    FootprintBinding binding;
    binding.view = s.view.Get();
    binding.stepCount = s.stepCount;
    binding.feetCount = s.liveCount;
    for (int i = 0; i < s.liveCount; ++i)
        for (int k = 0; k < 4; ++k) binding.feet[i][k] = s.live[i][k];
    for (int i = 0; i < kFootSteps; ++i) {
        for (int k = 0; k < 4; ++k) binding.steps[i][k] = s.steps[i][k];
        binding.impact[i] = s.impact[i];
    }
    return binding;
}
} // namespace Filmmaker::Environment
