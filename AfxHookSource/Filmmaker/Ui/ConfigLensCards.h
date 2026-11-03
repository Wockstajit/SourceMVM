// Private Config inspector implementation. Included after its shared rows (Pane, Slider, Header).
namespace Lx = Filmmaker::Lens;
constexpr Color kDof = Rgba(0xc7,0x9b,0xff), kFlare = Rgba(0xff,0xb3,0x6b);
std::string LensSummary(const ConfigModel& m,bool flare) {
    const auto& v=m.lens;
    if(v[flare ? Lx::FlareOn : Lx::DofOn]<.5f) return "Off";
    if(flare) return Format("%.2fx " MIDDOT " %d ghosts",v[Lx::FlareIntensity],(int)v[Lx::FlareGhosts]);
    if(v[Lx::DofMode]>.5f) return "Near / far zones";
    return v[Lx::DofAuto]>.5f ? Format("Auto " MIDDOT " f/%.1f",v[Lx::DofFstop])
        : Format("%.0fu " MIDDOT " f/%.1f",v[Lx::DofFocus],v[Lx::DofFstop]);
}
bool LensDefault(const ConfigModel& m,bool flare) {
    for(int i=0;i<Lx::ParamCount;++i)
        if(Lx::InGroup(i,flare ? Lx::Group::Flare : Lx::Group::Dof) && m.lens[i]!=Lx::Specs()[i].def) return false;
    return true;
}
void LensSet(Pane& p,Lx::Param id,double value) {
    Send(p,Format("mirv_filmmaker lens set %s %.6f quiet",Lx::Specs()[id].key,value));
}
void LensSlider(Pane& p,const ConfigModel& m,Lx::Param id,const char* label,double lo,double hi,int decimals,
    const char* unit="",const char* tip="") {
    const auto* key=Lx::Specs()[id].key;
    const std::string command=std::string("mirv_filmmaker lens set ")+key;
    Slider(p,key,label,tip,command.c_str(),m.lens[id],lo,hi,decimals,unit,false);
}
void LensModes(Pane& p,const ConfigModel& m,Lx::Param id,const char* label,const char* const* names,int n) {
    const int choice=ModeRow(p.c,Lx::Specs()[id].key,label,names,n,(int)m.lens[id],true);
    if(choice>=0) LensSet(p,id,choice);
}
void LensSwitch(Pane& p,const ConfigModel& m,Lx::Param id,const char* label,const char* tip="") {
    const float top=p.c.y;
    if(SwitchRow(p.c,Lx::Specs()[id].key,label,m.lens[id]>.5f,true)) LensSet(p,id,m.lens[id]>.5f ? 0 : 1);
    Hover(p,top,tip);
}
void LensLooks(Pane& p,bool flare) {
    const char* dof[]={"Cinematic","Anamorphic","Portrait","Miniature","Hearts","Stars"};
    const char* df[]={"cinematic","anamorphic","portrait","miniature","hearts","stars"};
    const char* fl[]={"Cinematic","Anamorphic","Classic","Star","Dreamy"};
    const char* fk[]={"cinematic","anamorphic","classic","star","dreamy"};
    SubHead(p.c,"Look");
    const int picked=CellRow(p.c,"lenslook",flare ? fl : dof,flare ? 5 : 6,-1,3);
    if(picked>=0) Send(p,Format("mirv_filmmaker lens preset %s %s quiet",flare ? "flare" : "dof",flare ? fk[picked] : df[picked]));
}
void LensColours(Pane& p,const ConfigModel& m,bool streak) {
    const int count=streak ? Lx::kStreakTintCount : Lx::kFlareTintCount;
    const auto param=streak ? Lx::FlareStreakTint : Lx::FlareTint;
    SubHead(p.c,streak ? "Streak colour" : "Flare colour");
    const float gap=6*p.c.s,step=(std::min)(40*p.c.s,(p.c.W()-gap*(count-1))/count),height=26*p.c.s;
    for(int i=0;i<count;++i) {
        const auto colour=streak ? Lx::StreakTintColor(i) : Lx::FlareTintColor(i);
        const Rect r=MakeRect(p.c.x0+i*(step+gap),p.c.y,p.c.x0+i*(step+gap)+step,p.c.y+height);
        ImGui::PushID(i);ImGui::SetCursorScreenPos(ImVec2(r.x0,r.y0));
        if(ImGui::InvisibleButton("tint",ImVec2(r.W(),r.H()))) LensSet(p,param,i);
        auto* draw=ImGui::GetWindowDrawList();
        draw->AddRectFilled(ImVec2(r.x0+3*p.c.s,r.y0+3*p.c.s),ImVec2(r.x1-3*p.c.s,r.y1-3*p.c.s),
            IM_COL32((int)(255*colour.r),(int)(255*colour.g),(int)(255*colour.b),255));
        draw->AddRect(ImVec2(r.x0,r.y0),ImVec2(r.x1,r.y1),ToU32((int)m.lens[param]==i ? Theme::Accent : Theme::Border));
        HoverRect(p,r,streak ? Lx::StreakTintName(i) : Lx::FlareTintName(i));
        ImGui::PopID();
    }
    p.c.y+=height+8*p.c.s;
}
void LensDofCard(Pane& p,const ConfigModel& m) {
    using namespace Filmmaker::Lens;
    const char* reset[]={"mirv_filmmaker lens reset dof quiet"};
    Header(p,"dof","Depth of field",LensSummary(m,false),kDof,m.lens[DofOn]>.5f,LensDefault(m,false),reset,1,"mirv_filmmaker lens dof toggle quiet");
    const char* pages[]={"Focus","Bokeh"};
    int page=Segmented(p.c,"dofpages",pages,2,p.st->lensPage[0]);if(page>=0) p.st->lensPage[0]=page;
    if(!p.st->lensPage[0]) {
        LensLooks(p,false);
        const char* modes[]={"Manual","Auto","Zones"};
        const int method=m.lens[DofMode]>.5f ? 2 : (m.lens[DofAuto]>.5f ? 1 : 0);
        const int pick=ModeRow(p.c,"focusmethod","Focus",modes,3,method,true);
        if(pick>=0) { LensSet(p,DofMode,pick==2 ? 1 : 0);LensSet(p,DofAuto,pick==1 ? 1 : 0); }
        if(method==2) {
            LensSlider(p,m,DofNearStart,"Near: fully blurred",0,2000,0,"u");
            LensSlider(p,m,DofNearEnd,"Near: sharp from",0,2000,0,"u");
            LensSlider(p,m,DofFarStart,"Far: sharp until",0,6000,0,"u");
            LensSlider(p,m,DofFarEnd,"Far: fully blurred",0,12000,0,"u");
        } else {
            if(method==1) LensSlider(p,m,DofFocusSpeed,"Focus speed",.25,30,2,"","How fast autofocus pulls toward the centre of the picture. 30 jumps instantly.");
            else LensSlider(p,m,DofFocus,"Focus distance",10,3000,0,"u","Distance from the camera that stays sharp, in world units.");
            LensSlider(p,m,DofFstop,"Aperture",.7,16,1,"","Lower f-stops give shallower focus.");
            LensSlider(p,m,DofRange,"Sharp band",0,1000,0,"u","Distance on either side of focus that stays perfectly sharp.");
        }
        LensSlider(p,m,DofMaxBlur,"Largest blur",1,90,0,"px","Largest blur radius at 1080p; scales with output resolution.");
        if(method!=2) LensSwitch(p,m,DofWeapon,"Blur the weapon","Off keeps the first-person weapon sharp.");
        const char* show[]={"Picture","Blur map","Focus","Zebra"};LensModes(p,m,DofDebug,"Show",show,4);
    } else {
        LensSlider(p,m,DofGamma,"Bokeh gamma",0,12,1,"","Lifts small bright lights before blurring. 0 is plain soft blur.");
        const char* shapes[]={"Circle","Polygon","Star","Heart","Ring","Custom"};
        SubHead(p.c,"Shape");const int shape=CellRow(p.c,"irisshape",shapes,6,(int)m.lens[DofShape],3);
        if(shape>=0) LensSet(p,DofShape,shape);
        const int current=(int)m.lens[DofShape];
        if(current==IrisPolygon || current==IrisStar) LensSlider(p,m,DofBlades,"Blades",3,12,0);
        if(current==IrisPolygon) LensSlider(p,m,DofRoundness,"Blade curve",-1,1,2);
        if(current!=IrisCircle && current!=IrisRing) LensSlider(p,m,DofRotation,"Rotation",-180,180,0," deg");
        if(current==IrisCustom) {
            const char* masks[]={"Diamond","Cross","Crescent","Clover","Smiley"};
            SubHead(p.c,"Custom shape");const int mask=CellRow(p.c,"irismask",masks,5,Lx::IrisMaskFromName(m.irisMask.c_str()),5);
            if(mask>=0) Send(p,Format("mirv_filmmaker lens iris mask %s quiet",Lx::IrisMaskName(mask)));
            Hover(p,p.c.y-30*p.c.s,"Your own image: mirv_filmmaker lens iris file <path>. White passes light.");
        }
        LensSlider(p,m,DofSqueeze,"Anamorphic squeeze",1,3,2,"x");
        LensSlider(p,m,DofEdge,"Rim",-1,3,2,"","Negative gathers light toward the centre; positive brightens the edge.");
        const char* quality[]={"Low","Medium","High"};LensModes(p,m,DofQuality,"Quality",quality,3);
    }
}
void LensFlareCard(Pane& p,const ConfigModel& m) {
    using namespace Filmmaker::Lens;
    const char* reset[]={"mirv_filmmaker lens reset flare quiet"};
    Header(p,"flare","Lens flare",LensSummary(m,true),kFlare,m.lens[FlareOn]>.5f,LensDefault(m,true),reset,1,"mirv_filmmaker lens flare toggle quiet");
    const char* pages[]={"Look","Detail"};
    int page=Segmented(p.c,"flarepages",pages,2,p.st->lensPage[1]);if(page>=0) p.st->lensPage[1]=page;
    if(!p.st->lensPage[1]) {
        LensLooks(p,true);
        LensSlider(p,m,FlareIntensity,"Intensity",0,2,2,"x");
        LensSlider(p,m,FlareThreshold,"Source threshold",.75,1,2,"","Only small bright lights flare. 1 turns every source off.");
        LensSlider(p,m,FlareGhosts,"Ghosts",0,8,0);
        LensSlider(p,m,FlareHalo,"Halo",0,2,2,"x");
        LensSlider(p,m,FlareStreak,"Streak",0,3,2,"x");
        LensSlider(p,m,FlareStar,"Spikes",0,3,2,"x");
        LensSlider(p,m,FlareGlow,"Glow",0,3,2,"x");
    } else {
        LensSlider(p,m,FlareStreakLen,"Streak length",.05,1,2);
        LensSlider(p,m,FlareBlades,"Aperture blades",3,12,0);
        LensSlider(p,m,FlareSun,"Sun",0,2,2,"x","Flare from the map's sun, blocked by world depth.");
        LensSlider(p,m,FlareDirt,"Lens dirt",0,1,2,"","Load your own image: mirv_filmmaker lens dirt <path>");
        LensColours(p,m,false);LensColours(p,m,true);
        LensSwitch(p,m,FlareDebug,"Flare only","Shows only the flare on black.");
    }
}
