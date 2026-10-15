#include "DeadlockCameraCommands.h"
#include "DeadlockCameraCommandMath.h"
#include "DeadlockCameraRuntime.h"
#include "DeadlockDemo.h"
#include "DeadlockEngine.h"
#include "../WrpConsole.h"
#include "../Filmmaker/Deadlock/DeadlockShot.h"
#include "../Filmmaker/Deadlock/DeadlockWorkspaces.h"
#include "../Filmmaker/Debug/MvmDebugLog.h"
#include "../Filmmaker/Platform/JsonBuilder.h"
#include "../../Shared/MirvCamIO.h"
#include "../../Shared/StringTools.h"
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

// Deadlock's accepted view stores the horizontal FOV after aspect-ratio scaling.
FovScaling GetDefaultFovScaling() { return FovScaling_None; }

namespace {
bool g_fovEnabled = false;
double g_fov = 90;
// Zoom handling is opt-in: CS2's unzoomed-FOV threshold is not a Deadlock contract.
bool g_handleZoom = false;
double g_unzoomedFov = 90;
CamImport* g_import = nullptr;
CamExport* g_export = nullptr;
double g_exportLast = std::numeric_limits<double>::quiet_NaN();
int g_exportLastTick = -1;
bool g_exportLoggedJitter = false;

bool Number(const char* text, double& out) {
    char* end = nullptr;
    out = std::strtod(text, &end);
    return end != text && *end == '\0' && std::isfinite(out);
}
double DemoTime() { return Deadlock::Demo_Time(); }
void Trace(advancedfx::ICommandArgs* args) {
    if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Command(args);
}
}

CON_COMMAND(mirv_fov, "Override the Deadlock replay camera FOV.") {
    Trace(args);
    double requestedFov = 0;
    if (args->ArgC() == 2 && !_stricmp(args->ArgV(1), "default")) {
        g_fovEnabled = false;
    } else if (args->ArgC() == 2 && Number(args->ArgV(1), requestedFov) && Deadlock::ValidCameraFov(requestedFov)) {
        g_fov = requestedFov;
        g_fovEnabled = true;
    } else if (args->ArgC() >= 3 && !_stricmp(args->ArgV(1), "handleZoom")) {
        if (!_stricmp(args->ArgV(2), "enabled") && args->ArgC() == 4
            && (!strcmp(args->ArgV(3), "0") || !strcmp(args->ArgV(3), "1"))) {
            g_handleZoom = !strcmp(args->ArgV(3), "1");
        } else if (!_stricmp(args->ArgV(2), "minUnzoomedFov") && args->ArgC() == 4) {
            double candidate;
            if (!_stricmp(args->ArgV(3), "current")) {
                double p[3], a[3];
                Deadlock::CameraRuntime_GetLastCamera(p, a, candidate);
            } else if (!Number(args->ArgV(3), candidate)) candidate = 0;
            if (Deadlock::ValidCameraFov(candidate)) g_unzoomedFov = candidate;
            else advancedfx::Warning("mirv_fov: expected a finite FOV between 1 and 179.\n");
        }
    } else {
        advancedfx::Message("mirv_fov <degrees>|default\nmirv_fov handleZoom enabled 0|1\n"
            "mirv_fov handleZoom minUnzoomedFov <degrees>|current\n");
    }
    advancedfx::Message("mirv_fov: %s value=%.3f handleZoom=%d minUnzoomedFov=%.3f (replays only)\n",
        g_fovEnabled ? "enabled" : "default", g_fov, g_handleZoom, g_unzoomedFov);
}

CON_COMMAND(mirv_camio, "Import/export advancedfx camera motion using Deadlock's demo clock.") {
    Trace(args);
    if (args->ArgC() >= 3 && !_stricmp(args->ArgV(2), "start") && Deadlock::Demo_Tick() < 0) {
        advancedfx::Warning("mirv_camio: load a replay before starting camera IO.\n");
        return;
    }
    const bool exportStart = args->ArgC() >= 4 && !_stricmp(args->ArgV(1), "export")
        && !_stricmp(args->ArgV(2), "start");
    MirvCamIO_ConsoleCommand(args, g_import, g_export, &DemoTime);
    if (g_import && g_import->IsBad()) { delete g_import; g_import = nullptr; }
    if (g_export && !g_export->IsOpen()) {
        advancedfx::Warning("mirv_camio: output file could not be opened.\n");
        delete g_export; g_export = nullptr;
    }
    if (exportStart) {
        g_exportLast = std::numeric_limits<double>::quiet_NaN();
        g_exportLastTick = -1;
        g_exportLoggedJitter = false;
    }
    advancedfx::Message("mirv_camio: import=%s export=%s\n", g_import ? "active" : "off", g_export ? "active" : "off");
}

CON_COMMAND(mirv_panorama, "Deadlock Panorama panel styles and script execution.") {
    Trace(args);
    if (args->ArgC() >= 3 && !_stricmp(args->ArgV(1), "eval")) {
        Deadlock::CameraCommands_EvalJs(args, 2, "mirv_panorama");
        return;
    }
    if (args->ArgC() >= 4 && !_stricmp(args->ArgV(1), "panelStyle")) {
        std::string id, klass;
        double opacity = -1;
        for (int i = 2; i < args->ArgC(); ++i) {
            const std::string arg = args->ArgV(i);
            if (arg.compare(0, 8, "panelId=") == 0) id = arg.substr(8);
            else if (arg.compare(0, 15, "panelClassName=") == 0) klass = arg.substr(15);
            else if (arg.compare(0, 8, "opacity=") == 0 && Number(arg.c_str() + 8, opacity)) {}
            else { advancedfx::Warning("mirv_panorama: unknown or invalid option %s.\n", arg.c_str()); return; }
        }
        if ((id.empty() == klass.empty()) || opacity < 0 || opacity > 1) {
            advancedfx::Warning("mirv_panorama: use exactly one panelId/panelClassName and opacity in [0,1].\n");
            return;
        }
        const std::string selector = Filmmaker::JsonBuilder::Escape(id.empty() ? klass : id);
        const std::string js = std::string("(function(){var r=$.GetContextPanel();while(r.GetParent())r=r.GetParent();"
            "var q=[r],n=0;while(q.length){var p=q.pop();if(")
            + (id.empty() ? "p.BHasClass(\"" : "p.id===\"") + selector
            + (id.empty() ? "\")" : "\"") + "){p.style.opacity='" + std::to_string(opacity)
            + "';n++;}for(var i=0;i<p.GetChildCount();i++)q.push(p.GetChild(i));}"
              "$.Msg('mirv_panorama panelStyle matched='+n);})();";
        if (!Filmmaker::Workspaces_Eval(js.c_str())) advancedfx::Warning("mirv_panorama: no ready Panorama context.\n");
        return;
    }
    advancedfx::Message("mirv_panorama panelStyle panelId=<id>|panelClassName=<class> opacity=<0..1>\n"
        "mirv_panorama eval \"<JavaScript>\"\nUse Deadlock panel IDs, not CS2 panel IDs.\n");
}

namespace Deadlock {
bool CameraCommands_EvalJs(advancedfx::ICommandArgs* args, int firstArg, const char* who) {
    std::string js;
    for (int i = firstArg; i < args->ArgC(); ++i) { if (i > firstArg) js += ' '; js += args->ArgV(i); }
    if (Filmmaker::Workspaces_Eval(js.c_str())) return true;
    advancedfx::Warning("%s: no ready Panorama context.\n", who ? who : "eval");
    return false;
}
}

namespace Deadlock {
void CameraCommands_RunFrame(bool demoLoaded) {
    // Use the main pump's debounced state rather than a single transient unload sample.
    if (demoLoaded || (!g_import && !g_export)) return;
    delete g_import; g_import = nullptr;
    delete g_export; g_export = nullptr;
    advancedfx::Message("mirv_camio: import/export stopped after replay unload.\n");
    if (Filmmaker::MvmDebugLog_Active())
        Filmmaker::MvmDebugLog_LinefAlways("deadlock.camio", "closed on replay unload");
}

bool CameraCommands_Active() { return Demo_Tick() >= 0 && (g_fovEnabled || g_import || Filmmaker::Shot_CommandPathActive()); }

bool CameraCommands_Apply(int width, int height, float& x, float& y, float& z,
    float& pitch, float& yaw, float& roll, float& fov) {
    if (Demo_Tick() < 0) return false;
    bool changed = Filmmaker::Shot_ApplyCommandPath(x, y, z, pitch, yaw, roll, fov);
    CamIO::CamData frame;
    if (g_import && g_import->GetCamData(Demo_Time(), width, height, frame)) {
        x = (float)frame.XPosition; y = (float)frame.YPosition; z = (float)frame.ZPosition;
        pitch = (float)frame.YRotation; yaw = (float)frame.ZRotation; roll = (float)frame.XRotation;
        fov = (float)frame.Fov;
        changed = true;
    }
    if (g_fovEnabled && ValidCameraFov(g_fov) && (!g_handleZoom || fov >= g_unzoomedFov)) {
        fov = (float)g_fov;
        changed = true;
    }
    return changed;
}

void CameraCommands_Record(int width, int height, double x, double y, double z,
    double pitch, double yaw, double roll, double fov) {
    if (!g_export) return;
    const int tick = Demo_Tick();
    // The main pump closes IO on unload. Do not export intermediate seek views.
    if (tick < 0 || Demo_IsSeeking()) return;
    const double time = Demo_Time();
    const auto step = CameraExportNext(time, g_exportLast, tick < g_exportLastTick);
    if (step == CameraExportStep::Rewind) {
        if (Filmmaker::MvmDebugLog_Active())
            Filmmaker::MvmDebugLog_LinefAlways("deadlock.camio", "export rewind time=%.6f last=%.6f tick=%d lastTick=%d", time, g_exportLast, tick, g_exportLastTick);
        advancedfx::Warning("mirv_camio: export stopped after a backward seek.\n");
        delete g_export; g_export = nullptr;
        return;
    }
    if (!g_exportLoggedJitter && step == CameraExportStep::Duplicate && time < g_exportLast) {
        g_exportLoggedJitter = true;
        if (Filmmaker::MvmDebugLog_Active())
            Filmmaker::MvmDebugLog_LinefAlways("deadlock.camio", "skipped interpolation jitter time=%.6f last=%.6f tick=%d lastTick=%d", time, g_exportLast, tick, g_exportLastTick);
    }
    if (step != CameraExportStep::Write || !ValidCameraFov(fov)) return;
    CamIO::CamData frame;
    frame.Time = time;
    frame.XPosition = x; frame.YPosition = y; frame.ZPosition = z;
    frame.XRotation = roll; frame.YRotation = pitch; frame.ZRotation = yaw; frame.Fov = fov;
    g_export->WriteFrame(width, height, frame);
    if (!g_export->IsOpen()) {
        advancedfx::Warning("mirv_camio: camera export stopped after an output write error.\n");
        delete g_export; g_export = nullptr;
        return;
    }
    if (Filmmaker::MvmDebugLog_Active() && !std::isfinite(g_exportLast))
        Filmmaker::MvmDebugLog_LinefAlways("deadlock.camio", "first export time=%.6f size=%dx%d fov=%.3f", frame.Time, width, height, fov);
    g_exportLast = frame.Time;
    g_exportLastTick = tick;
}
}
