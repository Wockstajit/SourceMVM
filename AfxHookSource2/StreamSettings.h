#pragma once

#include "../Shared/RecordingSettings.h"
#include "Filmmaker/Render/RenderPassPlan.h"

#include <string>
#include <list>

class CStreamSettings {
public:
    CStreamSettings(advancedfx::CRecordingSettings * settings)
    : Settings(settings){
        settings->AddRef();
    }

    ~CStreamSettings() {
        Settings->Release();
    }

    CStreamSettings(const CStreamSettings & other)
        : Name(other.Name)
        , Folder(other.Folder)
        , Record(other.Record)
        , Capture(other.Capture)
        , CaptureType(other.CaptureType)
        , Depth24(other.Depth24)
        , DepthZip(other.DepthZip)
        , DepthCompositeSmoke(other.DepthCompositeSmoke)
        , DepthVal(other.DepthVal)
        , DepthValMax(other.DepthValMax)
        , DepthChannels(other.DepthChannels)
        , DepthMode(other.DepthMode)
        , ClearOverride(other.ClearOverride)
        , ClearOverrideColor(other.ClearOverrideColor)
        , ClearBeforeUi(other.ClearBeforeUi)
        , ClearBeforeUiColor(other.ClearBeforeUiColor)
        , AutoForceFullResSmoke(other.AutoForceFullResSmoke)
        , PassType(other.PassType)
        , PassElements(other.PassElements)
        , PassDepthNear(other.PassDepthNear)
        , PassDepthFar(other.PassDepthFar)
        , PassKeyOut{other.PassKeyOut[0], other.PassKeyOut[1], other.PassKeyOut[2]}
        , PassReferenceOnly(other.PassReferenceOnly)
        , Settings(other.Settings)
        , BeforeCommands(other.BeforeCommands)
        , AfterCommands(other.AfterCommands)
        , ViewModelAction(other.ViewModelAction)
        , ParticlesAction(other.ParticlesAction)
        , FirstPersonLegsAction(other.FirstPersonLegsAction)
        , ShellsAction(other.ShellsAction)
        , WeaponsAction(other.WeaponsAction)
        , PlayersAction(other.PlayersAction)
        , WorldAction(other.WorldAction)
        , SkyAction(other.SkyAction)
        , SmokeAction(other.SmokeAction)
        , OverlaysAction(other.OverlaysAction)
    {
        Settings->AddRef();
    }

    bool CanCaptureInMainPass() const {
        return true
            && false == ClearOverride
            && false == ClearBeforeUi
            && BeforeCommands.empty()
            && AfterCommands.empty()
            // A Filmmaker pass is taken from the Main render only when its visible elements are
            // Everything; every other visibility group renders in its own extra pass.
            && (!IsPass() || PassRenderKey() == (int)Filmmaker::RenderKey::Main)
            && Action::Draw == ViewModelAction
            && Action::Draw == ParticlesAction
            && Action::Draw == FirstPersonLegsAction
            && Action::Draw == ShellsAction
            && Action::Draw == WeaponsAction
            && Action::Draw == PlayersAction
            && Action::Draw == WorldAction
            && Action::Draw == SkyAction
            && Action::Draw == SmokeAction
            && Action::Draw == OverlaysAction
        ;
    }

    // Filmmaker multipass stream (Filmmaker/Render/RenderPassPlan.h): PassElements >= 0.
    bool IsPass() const { return PassElements >= 0; }
    int PassRenderKey() const {
        return IsPass() ? (int)Filmmaker::RenderKeyFor((Filmmaker::VisibleElements)PassElements) : -1;
    }
    // A pass the pass compositor builds (Key, Depth, Normal, or an output-less reference render)
    // instead of recording the render as drawn. It reads the scene from before the lens post pass
    // (depth of field, flare); Beauty plates and every other stream record the finished frame.
    bool IsComposedPass() const {
        if (!IsPass()) return false;
        if (PassReferenceOnly) return true;
        Filmmaker::PassConfig pass;
        pass.type = (Filmmaker::PassType)PassType;
        pass.elements = (Filmmaker::VisibleElements)PassElements;
        return !Filmmaker::PassIsDirectCapture(pass);
    }

    int CompareRenderPass(const CStreamSettings &  o) const {
        int cmp;
        if(cmp = CompareBool(ClearOverride, o.ClearOverride)) return cmp;
        if(ClearOverride) {
           if(cmp = CompareFloat(ClearOverrideColor.R, o.ClearOverrideColor.R)) return cmp;
           if(cmp = CompareFloat(ClearOverrideColor.G, o.ClearOverrideColor.G)) return cmp;
           if(cmp = CompareFloat(ClearOverrideColor.B, o.ClearOverrideColor.B)) return cmp;
           if(cmp = CompareFloat(ClearOverrideColor.A, o.ClearOverrideColor.A)) return cmp;
        }
        if(cmp = CompareBool(ClearBeforeUi, o.ClearBeforeUi)) return cmp;
        if(ClearBeforeUi) {
           if(cmp = CompareFloat(ClearBeforeUiColor.R, o.ClearBeforeUiColor.R)) return cmp;
           if(cmp = CompareFloat(ClearBeforeUiColor.G, o.ClearBeforeUiColor.G)) return cmp;
           if(cmp = CompareFloat(ClearBeforeUiColor.B, o.ClearBeforeUiColor.B)) return cmp;
           if(cmp = CompareFloat(ClearBeforeUiColor.A, o.ClearBeforeUiColor.A)) return cmp;
        }
        if(cmp = CompareCommands(BeforeCommands, o.BeforeCommands)) return cmp;
        if(cmp = CompareCommands(AfterCommands, o.AfterCommands)) return cmp;
        // Passes sharing a render key share one physical render (e.g. Beauty and Depth of Players
        // Only; World + Players and Gun Only both come from the NoGun render). The key order is the
        // render order within a logical frame.
        if(PassRenderKey() != o.PassRenderKey()) return PassRenderKey() < o.PassRenderKey() ? -1 : 1;
        if(cmp = CompareAction(ViewModelAction, o.ViewModelAction)) return cmp;
        if(cmp = CompareAction(ParticlesAction, o.ParticlesAction)) return cmp;
        if(cmp = CompareAction(FirstPersonLegsAction, o.FirstPersonLegsAction)) return cmp;
        if(cmp = CompareAction(ShellsAction, o.ShellsAction)) return cmp;
        if(cmp = CompareAction(WeaponsAction, o.WeaponsAction)) return cmp;
        if(cmp = CompareAction(PlayersAction, o.PlayersAction)) return cmp;
        if(cmp = CompareAction(WorldAction, o.WorldAction)) return cmp;
        if(cmp = CompareAction(SkyAction, o.SkyAction)) return cmp;
        if(cmp = CompareAction(SmokeAction, o.SmokeAction)) return cmp;
        if(cmp = CompareAction(OverlaysAction, o.OverlaysAction)) return cmp;

        return 0;
    }    

    std::wstring Name;

    // Folder under the take this stream writes into; empty = its Name (mirv_streams behaviour).
    // Render jobs point several streams at one folder (beauty\) and name their files instead.
    std::wstring Folder;

    bool Record = true;

    enum class Capture_e {
        BeforePresent,
        BeforeUi
    } Capture = Capture_e::BeforePresent;

    enum class CaptureType_e {
        Rgb,
        Rgba,
        DepthRgb,
        DepthF
    } CaptureType = CaptureType_e::Rgb;

    bool Depth24 = false;
    bool DepthZip = false;

    bool DepthCompositeSmoke = true;

    float DepthVal = 7;
    float DepthValMax = 2100;

    enum class DepthChannels_e {
        Gray,
        SplitRgb,
        Dithered
    } DepthChannels = DepthChannels_e::Gray;

    enum class DepthMode_e {
        Inverse,
        Linear,
        LogE,
        PyramidalLinear,
        PyramidalLogE
    } DepthMode = DepthMode_e::Inverse;

    bool ClearOverride = false;

    struct {
        float R = 0.0f;
        float G = 0.0f;
        float B = 0.0f;
        float A = 0.0f;
    } ClearOverrideColor;

    bool ClearBeforeUi = false;

    struct {
        float R = 0.0f;
        float G = 0.0f;
        float B = 0.0f;
        float A = 0.0f;
    } ClearBeforeUiColor;

    bool AutoForceFullResSmoke = true;

    // Filmmaker multipass stream (Filmmaker/Render/RenderPassPlan.h). PassElements < 0 marks a
    // classic mirv_streams stream. PassType / PassElements hold Filmmaker::PassType /
    // Filmmaker::VisibleElements; the render key and the compositor mode follow from them.
    int PassType = 0;
    int PassElements = -1;
    float PassDepthNear = 8.0f;
    float PassDepthFar = 4096.0f;
    float PassKeyOut[3] = {0.0f, 1.0f, 0.0f};
    // A render with no output of its own: the World Only render a Players Only job keeps the
    // depth of, so the players mask is "closer than the world" (RenderPassCompositor).
    bool PassReferenceOnly = false;

    std::list<std::list<std::string>> BeforeCommands;
    std::list<std::list<std::string>> AfterCommands;

    advancedfx::CRecordingSettings* Settings;

    bool WantsSmokeComposite() {
        return (
            CaptureType == CaptureType_e::DepthF
            || CaptureType == CaptureType_e::DepthRgb
        ) && (
            DepthCompositeSmoke
        );
    }

    bool WantsFullResSmoke() {
        return WantsSmokeComposite()
            && AutoForceFullResSmoke
        ;
    }

    enum class Action : int {
        Draw = 0,
        NoDraw,
        ZOnly
    };

    Action ViewModelAction = Action::Draw;
    Action ParticlesAction = Action::Draw;
    Action FirstPersonLegsAction = Action::Draw;
    Action ShellsAction = Action::Draw;
    Action WeaponsAction = Action::Draw;
    Action PlayersAction = Action::Draw;
    Action WorldAction = Action::Draw;
    Action SkyAction = Action::Draw;
    Action SmokeAction = Action::Draw;
    Action OverlaysAction = Action::Draw;

private:
    static int CompareBool(bool lhs, bool rhs) {
        if(lhs != rhs) return lhs ? 1 : -1;
        return 0;
    }

    static int CompareFloat(float lhs, float rhs) {
        if(lhs != rhs) return lhs > rhs ? 1 : -1;
        return 0;
    }

    static int CompareCommand(const std::list<std::string> & lhs, const std::list<std::string> & rhs) {
        auto itLhs = lhs.begin();
        auto itRhs = rhs.begin();
        while(itLhs != lhs.end() && itRhs != rhs.end()) {
            if(int cmp = itLhs->compare(*itRhs)) return cmp;
            itLhs++;
            itRhs++;
        }
        if(itLhs != lhs.end()) return 1;
        if(itRhs != rhs.end()) return -1;
        return 0;        
    }

    static int CompareCommands(const std::list<std::list<std::string>> & lhs, const std::list<std::list<std::string>> & rhs) {        
        auto itLhs = lhs.begin();
        auto itRhs = rhs.begin();
        while(itLhs != lhs.end() && itRhs != rhs.end()) {
            if(int cmp = CompareCommand(*itLhs, *itRhs)) return cmp;
            itLhs++;
            itRhs++;
        }
        if(itLhs != lhs.end()) return 1;
        if(itRhs != rhs.end()) return -1;
        return 0;
    }

    static int CompareAction(Action lhs, Action rhs) {
        int cmp = (int)lhs - (int)rhs;
        if(cmp) return 0 < cmp ? 1 : -1;
        return 0;
    }

};
