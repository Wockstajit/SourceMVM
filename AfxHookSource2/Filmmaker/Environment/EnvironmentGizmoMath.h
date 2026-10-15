#pragma once

#include "EnvironmentMath.h"

namespace Filmmaker::Environment {
// Normalized viewport coordinates, with Y increasing downward. Projection is shared by
// the real UI and the edit solver; the mock supplies representative projected coordinates.
struct GizmoPoint { float x = 0, y = 0; bool visible = false; };
inline GizmoPoint GizmoProject(const Matrix &vp, Vec3 p) {
    const float w = vp[12]*p.x + vp[13]*p.y + vp[14]*p.z + vp[15];
    if (!Finite(p) || !std::isfinite(w) || w <= .001f) return {};
    const Vec3 n = Transform(vp, p);
    if (!Finite(n)) return {};
    return {(n.x + 1)*.5f, (1 - n.y)*.5f, n.z >= 0 && n.z <= 1};
}
inline Vec3 GizmoAxis(int axis) {
    return axis == 0 ? Vec3{1,0,0} : axis == 1 ? Vec3{0,1,0} : Vec3{0,0,1};
}
inline float GizmoWorldSize(const Matrix &vp, Vec3 p) {
    const float w = vp[12]*p.x + vp[13]*p.y + vp[14]*p.z + vp[15];
    const float vertical = std::sqrt(vp[4]*vp[4] + vp[5]*vp[5] + vp[6]*vp[6]);
    return std::max(.01f, .16f * w / std::max(.0001f, vertical));
}
inline float GizmoSegmentDistance(GizmoPoint p, GizmoPoint a, GizmoPoint b, float aspect) {
    const float dx=(b.x-a.x)*aspect, dy=b.y-a.y;
    const float length=dx*dx+dy*dy;
    if (!a.visible || !b.visible || length < .000025f) return 1000;
    const float t=std::clamp(((p.x-a.x)*aspect*dx+(p.y-a.y)*dy)/length,0.f,1.f);
    const float x=(p.x-a.x)*aspect-t*dx, y=p.y-a.y-t*dy;
    return std::sqrt(x*x+y*y);
}

struct GizmoDrag {
    bool active = false;
    int objectId = 0, axisIndex = 0;
    Vec3 origin{}, current{}, camera{}, axis{}, planeNormal{};
    Matrix inverse{};
    float anchor = 0;

    bool Parameter(float u, float v, float &value) const {
        if (!std::isfinite(u) || !std::isfinite(v)) return false;
        const Vec3 point=Transform(inverse,{u*2-1,1-v*2,.5f});
        if (!Finite(point)) return false;
        const Vec3 direction=Normalize(point-camera);
        const float denominator=Dot(direction,planeNormal);
        if (std::abs(denominator)<.0001f) return false;
        const float t=Dot(origin-camera,planeNormal)/denominator;
        if (!std::isfinite(t) || t<=0) return false;
        value=Dot(camera+direction*t-origin,axis);
        return std::isfinite(value);
    }
    bool Begin(int id, int selectedAxis, Vec3 position, Vec3 eye, const Matrix &vp, float u, float v) {
        active=false;
        if (id<=0 || selectedAxis<0 || selectedAxis>2 || !Finite(position) || !Finite(eye)) return false;
        origin=current=position; camera=eye; axis=GizmoAxis(selectedAxis);
        const Vec3 toEye=eye-position;
        const Vec3 normal=toEye-axis*Dot(toEye,axis);
        // An axis looking straight into the camera cannot be dragged meaningfully.
        if (Dot(normal,normal)<.0001f || !Inverse(vp,inverse)) return false;
        planeNormal=Normalize(normal);
        if (!Parameter(u,v,anchor)) return false;
        objectId=id; axisIndex=selectedAxis; active=true;
        return true;
    }
    bool Move(float u, float v) {
        float value;
        if (!active || !Parameter(u,v,value)) return false;
        const Vec3 next=origin+axis*(value-anchor);
        if (!Finite(next) || std::abs(next.x)>32768 || std::abs(next.y)>32768 || std::abs(next.z)>32768) return false;
        current=next;
        return true;
    }
    bool Changed() const { const Vec3 d=current-origin; return Dot(d,d)>.000001f; }
    Vec3 Cancel() { active=false; current=origin; return origin; }
    bool Commit() { const bool changed=active&&Changed(); active=false; return changed; }
};
} // namespace Filmmaker::Environment
