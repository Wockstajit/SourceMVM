#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentGizmoMath.h"
#include <cstdlib>
#include <iostream>
using namespace Filmmaker::Environment;
void Check(bool ok, const char *name) { if(!ok){std::cerr<<name<<"\n";std::exit(1);} }
int main() {
    const Matrix projection={1,0,0,0, 0,1,0,0, 0,0,-1.001f,-.1001f, 0,0,-1,0};
    Vec3 eye{0,0,0}, object{0,0,-10};
    const auto center=GizmoProject(projection,object);
    Check(center.visible&&std::abs(center.x-.5f)<.001f,"center projection");
    Check(!GizmoProject(projection,{0,0,10}).visible,"behind camera rejected");
    Check(GizmoProject(projection,{100,0,-10}).x>1,"offscreen remains identifiable");
    const float size=GizmoWorldSize(projection,object);
    auto tip=GizmoProject(projection,object+Vec3{size,0,0});
    auto farTip=GizmoProject(projection,Vec3{GizmoWorldSize(projection,{0,0,-100}),0,-100});
    Check(std::abs(tip.x-farTip.x)<.00001f,"constant screen size across distance");
    Check(GizmoSegmentDistance({.55f,.5f,true},center,tip,16.f/9)<.001f,"axis hit");
    Check(GizmoSegmentDistance({.55f,.6f,true},center,tip,16.f/9)>.09f,"axis miss");
    GizmoDrag drag;
    Check(drag.Begin(7,0,object,eye,projection,.55f,.5f),"begin");
    Check(drag.Move(.65f,.55f),"move");
    Check(drag.current.x>1.9f&&drag.current.x<2.1f&&drag.current.y==0&&drag.current.z==-10,"constrained X movement without grab jump");
    Check(drag.Commit()&&!drag.Commit(),"exactly one commit per drag");
    Check(drag.Begin(7,1,object,eye,projection,.5f,.5f)&&drag.Move(.7f,.4f),"Y drag");
    Check(drag.current.x==0&&drag.current.y>1.9f,"Y constraint");
    Check(drag.Cancel().y==0&&!drag.active,"cancel restores original");
    Check(!drag.Begin(7,2,object,eye,projection,.5f,.5f),"parallel axis rejected");
    Check(drag.Begin(7,0,object,eye,projection,.5f,.5f)&&!drag.Commit(),"no-op has no undo");
    Check(drag.Begin(7,0,object,eye,projection,.5f,.5f)&&!drag.Move(INFINITY,.5f),"non-finite input rejected");
    const Matrix wide={.5f,0,0,0, 0,1,0,0, 0,0,-1.001f,-.1001f, 0,0,-1,0};
    Check(std::abs(GizmoWorldSize(wide,object)-size)<.0001f,"aspect-independent vertical handle size");
    std::cout<<"Environment gizmo math passed\n";
}
