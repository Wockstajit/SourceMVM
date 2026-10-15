#include "../World/WorldTransient.h"
#define NOMINMAX
#include "EnvironmentScene.h"
#include <unordered_map>
#include "EnvironmentGltfMath.h"
#include "EnvironmentVfxLighting.h"
#include "EnvironmentParticleShadows.h"
#include "../../SceneSystem.h"
#include "../Relight/RelightStainedGlass.h"
#include "EnvironmentPersistenceMath.h"
#include "EnvironmentCoatingMath.h"
#include "EnvironmentRender.h"
#include "EnvironmentNativeWater.h"
#include "EnvironmentNativeRain.h"
#include "EnvironmentRayProbe.h"
#include "EnvironmentGizmoMath.h"
#include "../Panorama/ConfigHud.h"
#include "EnvironmentBakedLighting.h"
#include "EnvironmentOcclusion.h"
#include "EnvironmentSurfaceMath.h"
#include "../Config/FilmmakerSettingsStore.h"
#include "../Config/EffectsPersistence.h"
#include "../Debug/MvmDebugLog.h"
#include "../Movie/CameraBridge.h"
#include "../Cosmetics/CosmeticEntityAccess.h" // CosmeticEntFromIndex: pawn origins for the coating actors
#include "../../ClientEntitySystem.h"      // GetHighestEntityIndex, CEntityInstance
#include "../../SchemaSystem.h"          // g_clientDllOffsets: smoke detonation positions
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../Relight/Relight.h"
#include "../Relight/RelightEnvironmentMath.h"
#include "../Relight/RelightLightEditor.h"
#include "../../hlaeFolder.h"
#include "../../MirvTime.h"
#include "../../RenderSystemDX11Hooks.h"
#include "../Props/SceneUndoClock.h"
bool AfxStreams_IsRcording(); // RenderSystemDX11Hooks: a recording is running
#include "../../../Shared/AfxConsole.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include <cstdio>
#include <cstring>
#include <Windows.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <future>
#include <chrono>
#include <memory>
#include <cstdlib>

extern SOURCESDK::CS2::ISource2EngineToClient *g_pEngineToClient;
namespace Filmmaker::Relight {
// Only the verified stained-glass family loses baked emission. Lamps and screens retain it.
namespace {
struct GlassMaterialSnapshot {float brightness;};
std::map<std::pair<CMaterial2*,std::string>,GlassMaterialSnapshot> glassMaterials;
int WriteGlassBrightness(float* parameter,float desired,float* original) {
    __try {if(original)*original=*parameter; if(*parameter==desired)return 0; *parameter=desired;return 1;}
    __except(EXCEPTION_EXECUTE_HANDLER){return -1;}
}
}
void UpdateStainedGlassMaterials(bool owned) {
    if(!owned&&glassMaterials.empty())return;
    // Discover late-loaded materials without updating GPU state when the value is unchanged.
    static ULONGLONG last=0;const auto now=GetTickCount64();
    if(owned&&now-last<500)return;last=now;
    GetMaterialsArrayResult materials{};if(!g_pCResourceSystem)return;
    g_pCResourceSystem->GetMaterials(&materials);
    for(uint64_t i=0;i<materials.count;++i){
        auto slot=materials.pArrMaterials[i];if(!slot||uintptr_t(slot)==~uintptr_t(0))continue;
        auto material=*slot;if(!material||uintptr_t(material)==~uintptr_t(0))continue;
        char name[512]{};if(!SceneSystem_MaterialName(material,name,sizeof(name)))continue;
        if(std::string(name).find("glass_stained")==std::string::npos)continue;
        // Brightness is an exposure value: zero still emits. Scale zero removes emission.
        bool materialChanged=false;
        for(const char* property:{"g_flSelfIllumBrightness","g_flSelfIllumScale"}){
            float* parameter=SceneSystem_FindMaterialParam(material,property);if(!parameter)continue;
            const auto key=std::make_pair(material,std::string(name)+":"+property);auto found=glassMaterials.find(key);
            float original=0;if(!owned&&found==glassMaterials.end())continue;
            const float desired=owned?0:found->second.brightness;
            const int changed=WriteGlassBrightness(parameter,desired,found==glassMaterials.end()?&original:nullptr);
            if(changed>=0&&owned&&found==glassMaterials.end())glassMaterials.emplace(key,GlassMaterialSnapshot{original});
            if(changed>0){materialChanged=true;
                if(MvmDebugLog_Active())MvmDebugLog_LinefAlways("relight.glass.material","material=%s property=%s pointer=%p parameter=%p value=%g owned=%u",name,property,material,parameter,desired,unsigned(owned));}
        }
        if(materialChanged)SceneSystem_UpdateMaterial(material);
    }
    if(!owned)glassMaterials.clear();
}
void ResetStainedGlassMaterials(){UpdateStainedGlassMaterials(false);glassMaterials.clear();}
}
namespace Filmmaker::Environment {
namespace {
std::mutex g_mutex, g_statusMutex;
Scene g_scene;
WorldLook g_liveWorld=DefaultWorldLook();
LightingProof g_proof;
std::vector<Scene> g_undo;
std::vector<uint64_t> g_undoStamps; // SceneUndoClock stamp of each g_undo entry (same order)
ULONGLONG g_lastRemember = 0;
void PushUndoScene(Scene before) {
    g_undo.push_back(std::move(before));
    g_undoStamps.push_back(NextSceneUndoStamp());
    if (g_undo.size() > 32) { g_undo.erase(g_undo.begin()); g_undoStamps.erase(g_undoStamps.begin()); }
}
std::map<std::string, std::shared_ptr<const Mesh>> g_meshes;
std::vector<std::string> g_presets, g_assets, g_textures;
std::map<std::string,std::weak_ptr<const MaterialImage>> g_images;
std::string g_message = "Choose weather, or add a prop or light.", g_savedName, g_savedJson,
            g_status = "Waiting for scene";
int g_selected = 0, g_editing = 0;
GizmoDrag g_drag;
Scene g_dragBefore;
Matrix g_gizmoProjection{};
unsigned g_editRevision = 0;
SceneSaveQueue g_autosave;
std::map<std::string,SceneSaveQueue> g_unsavedMaps;
bool g_restoreAutosave = true;
SceneRestoreGate g_restoreGate;
bool g_restoreSettling = false;
uint64_t g_restoreFirstPresent = 0;
std::string g_restoreMap;
ULONGLONG g_nextAutosavePoll = 0;
uint64_t g_sceneGeneration = 1;
unsigned long long g_draws = 0;
bool g_scanned = false;
Vec3 g_camera, g_forward{1, 0, 0}, g_angles;
bool g_cameraValid = false;
std::filesystem::path Root() {
    return std::filesystem::path(GetHlaeRoamingAppDataFolderW()) / L"source_mvm" / L"environment";
}
std::filesystem::path RestoringMarker(const std::string& map) {
    return Root()/"autosaves"/(map+".restoring");
}
std::string CurrentMap() {
    const char *p = g_pEngineToClient ? g_pEngineToClient->GetLevelNameShort() : nullptr;
    return p ? p : "";
}
void Scan() {
    g_presets.clear();
    g_assets.clear();
    g_textures.clear();
    for (auto folder : {"presets", "meshes", "textures"}) {
        auto dir = Root() / folder;
        std::filesystem::create_directories(dir);
        for (auto &item : std::filesystem::directory_iterator(dir))
            if (item.is_regular_file()) {
                auto name = item.path().stem().string();
                auto ext = item.path().extension().string();
                if (ValidName(name)) {
                    if (ext == ".json" && std::string(folder) == "presets")
                        g_presets.push_back(name);
                    if (ext == ".png" && std::string(folder) == "textures") g_textures.push_back(name);
                    // Your meshes: OBJ, and glTF (.glb or .gltf) with textures and animations.
                    std::string lower = ext;
                    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
                    if ((lower == ".obj" || lower == ".glb" || lower == ".gltf") && std::string(folder) == "meshes" &&
                        std::find(g_assets.begin(), g_assets.end(), name) == g_assets.end())
                        g_assets.push_back(name);
                }
            }
    }
    std::sort(g_presets.begin(), g_presets.end());
    std::sort(g_assets.begin(), g_assets.end());
    std::sort(g_textures.begin(),g_textures.end());
    g_scanned = true;
}
bool ResolveImage(const std::string &name,std::shared_ptr<const MaterialImage> &image,std::string &error){
    image.reset();if(name.empty())return true;
    if(auto cached=g_images[name].lock()){image=cached;return true;}
    auto path=Root()/"textures"/(name+".png");
    if(!std::filesystem::exists(path)||std::filesystem::file_size(path)>16*1024*1024){error="Missing or oversized texture: "+name;return false;}
    auto loaded=std::make_shared<MaterialImage>();if(!LoadMaterialImage(path.wstring(),*loaded,error)){error=name+": "+error;return false;}
    size_t resident=loaded->rgba.size();for(auto it=g_images.begin();it!=g_images.end();)if(auto active=it->second.lock()){resident+=active->rgba.size();++it;}else it=g_images.erase(it);
    if(resident>256*1024*1024){error="Texture cache exceeds 256 MB including undo references";return false;}
    image=loaded;g_images[name]=loaded;return true;
}
bool Resolve(Scene &s, std::string &error) {
    for (auto &o : s.objects)
        if (!o.IsLight()) {
            if(!ResolveImage(o.material.albedoTexture,o.material.albedoImage,error)||
               !ResolveImage(o.material.normalTexture,o.material.normalImage,error)||
               !ResolveImage(o.material.roughnessTexture,o.material.roughnessImage,error))return false;
            std::string key = o.kind == "obj" ? "obj:" + o.asset : o.kind;
            auto found = g_meshes.find(key);
            if (found != g_meshes.end()) {
                o.mesh = found->second;
                continue;
            }
            std::shared_ptr<const Mesh> mesh;
            if (o.kind == "obj") {
                // A user mesh is named without its extension: an OBJ first, then a GLB / glTF.
                auto meshes = Root() / "meshes";
                auto loaded = std::make_shared<Mesh>();
                if (std::filesystem::exists(meshes / (o.asset + ".obj"))) {
                    auto path = meshes / (o.asset + ".obj");
                    if (std::filesystem::file_size(path) > 16 * 1024 * 1024) {
                        error = "Missing or oversized mesh: " + o.asset;
                        return false;
                    }
                    std::ifstream file(path);
                    if (!file || !LoadObj(file, *loaded, error)) {
                        error = o.asset + ": " + error;
                        return false;
                    }
                } else {
                    auto path = meshes / (o.asset + ".glb");
                    if (!std::filesystem::exists(path)) path = meshes / (o.asset + ".gltf");
                    if (!std::filesystem::exists(path)) {
                        error = "Missing mesh: " + o.asset;
                        return false;
                    }
                    if (!LoadGltfMesh(path, *loaded, error)) {
                        error = o.asset + ": " + error;
                        return false;
                    }
                }
                mesh = loaded;
            } else
                mesh = Primitive(o.kind);
            if (!mesh || mesh->vertices.empty()) {
                error = "Empty mesh: " + key;
                return false;
            }
            g_meshes[key] = mesh;
            o.mesh = mesh;
        }
    return true;
}
bool AtomicReplace(const std::filesystem::path& path, const std::string& bytes, std::string& error) {
    auto temporary = path; temporary += L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error="Could not open scene save: "+std::to_string(GetLastError()); return false; }
    DWORD written=0;
    bool ok=bytes.size()<=MAXDWORD && WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr) &&
            written==bytes.size() && FlushFileBuffers(file);
    DWORD code=ok?ERROR_SUCCESS:GetLastError();
    CloseHandle(file);
    if (ok) { ok=MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE; code=GetLastError(); }
    if (!ok) error="Could not commit scene save: "+std::to_string(code);
    return ok;
}
bool AtomicSave(const std::filesystem::path &path, const std::string &bytes, std::string &error) {
    std::filesystem::create_directories(path.parent_path());
    if (std::filesystem::exists(path) && std::filesystem::file_size(path)<=1024*1024) {
        std::ifstream old(path,std::ios::binary);
        std::string previous((std::istreambuf_iterator<char>(old)),{});
        Scene validated; std::string validationError;
        // A corrupt primary must never overwrite the last valid recovery copy.
        if (!old.bad() && ParseScene(previous,validated,validationError)) {
            auto backup=path; backup+=L".bak";
            if (!AtomicReplace(backup,previous,error)) return false;
        }
    }
    return AtomicReplace(path,bytes,error);
}
Scene LiveScene() {
    Scene scene = g_scene;
    // Scene snapshots drive autosave, presets and undo. Temporary eggs are only render state.
    scene.world = CaptureWorldLook(WorldLookCapture::User);
    return scene;
}
bool IsSceneMap(const std::string& map) {
    return ValidName(map) && map.rfind("ui_", 0) != 0 && map.rfind("ui/", 0) != 0;
}
void SaveQueuedScene(SceneSaveQueue &queue,uint64_t now,bool force) {
    if(!queue.Due(now,force))return;
    std::string error;bool saved=false;
    try{saved=AtomicSave(Root()/"autosaves"/(queue.map+".json"),queue.pending,error);}
    catch(const std::exception& e){error=e.what();}
    queue.Complete(saved,now);
    MvmDebugLog_LinefAlways("environment.autosave","map=%s saved=%d generation=%llu error=%s",
        queue.map.c_str(),int(saved),g_sceneGeneration,error.c_str());
    if(!saved)g_message="Lighting autosave failed: "+error;
}
void FlushAutosave(bool force) {
    const auto now=GetTickCount64();
    for(auto it=g_unsavedMaps.begin();it!=g_unsavedMaps.end();){
        SaveQueuedScene(it->second,now,force);
        if(!it->second.dirty)it=g_unsavedMaps.erase(it);else ++it;
    }
    SaveQueuedScene(g_autosave,now,force);
}
void BindAutosave(const std::string& map) {
    // A failed disk write must survive leaving the map, including repeated switches.
    BindSceneSaveQueue(g_autosave,g_unsavedMaps,map);
}

void QueueCommittedScene() {
    if (g_drag.active) return;
    Scene committed = LiveScene();
    if (committed.map.empty()) committed.map = CurrentMap();
    if (!IsSceneMap(committed.map)) return;
    if (g_autosave.map != committed.map) { FlushAutosave(true); BindAutosave(committed.map); }
    g_autosave.Observe(SerializeScene(committed), GetTickCount64());
}
void UpdateAutosave(const std::string& map) {
    if (map != g_autosave.map) {
        FlushAutosave(true);
        BindAutosave(IsSceneMap(map) ? map : "");
        g_restoreAutosave = true;
        g_restoreGate.Reset();
    }
    if (!IsSceneMap(map)) {FlushAutosave(false);return;}
    // Restore-loop guard. A saved scene that freezes the GPU (bounce steps 3 on a large map: three
    // minutes frozen, user report 2026-10-13) froze it again on every load, so the demo could not
    // be opened. <map>.restoring is written before a restore and removed once the game has drawn
    // kRestoreSettleFrames frames with it; frames, not seconds, because a frozen game still ages.
    constexpr int kRestoreSettleFrames = 600;
    const auto presents = RenderCompletedPresents();
    if (g_restoreSettling && (g_restoreMap != map ||
        (presents >= g_restoreFirstPresent && presents - g_restoreFirstPresent >= kRestoreSettleFrames))) {
        std::error_code ec;
        std::filesystem::remove(RestoringMarker(g_restoreMap), ec);
        g_restoreSettling = false;
    }
    if (g_restoreAutosave) {
        // Surface objects need map geometry before they can be resolved. Never
        // overwrite edits made while waiting: RunCommand cancels this restore.
        if (!g_restoreGate.Ready(g_cameraValid && !MapOcclusionPreparing(), presents, GetTickCount64())) return;
        MvmDebugLog_LinefAlways("environment.autosave", "restore ready map=%s presents=%llu bufferMs=%llu",
            map.c_str(), presents - g_restoreGate.firstPresent, GetTickCount64() - g_restoreGate.startedAt);
        g_restoreAutosave = false;
        std::string error;
        bool restored = false;
        {
            std::error_code ec;
            if (std::filesystem::exists(RestoringMarker(map), ec)) {
                // The last restore never settled: set its scene aside instead of applying it again.
                const auto saved = Root()/"autosaves"/(map+".json");
                std::filesystem::rename(saved, Root()/"autosaves"/(map+".json.held"), ec);
                std::filesystem::remove(Root()/"autosaves"/(map+".json.bak"), ec);
                std::filesystem::remove(RestoringMarker(map), ec);
                g_autosave.dirty = false;
                g_message = "Saved lighting for " + map + " was not applied: the game stopped responding after it "
                    "was last applied. It was kept as " + map + ".json.held in the lighting folder.";
                MvmDebugLog_LinefAlways("environment.autosave", "restore held map=%s (previous restore never settled)", map.c_str());
                Scene initial = LiveScene(); initial.map = map;
                g_autosave.Bind(map, SerializeScene(initial));
                return;
            }
        }
        // Arm the crash guard before resolving assets or applying engine materials.
        // A crash inside those operations must leave evidence for the next launch.
        auto armRestore = [&]() {
            std::error_code ec;
            std::filesystem::create_directories(Root()/"autosaves", ec);
            std::ofstream(RestoringMarker(map), std::ios::binary | std::ios::trunc) << "restoring\n";
        };
        if(g_autosave.dirty){
            Scene pending;
            if(ParseScene(g_autosave.pending,pending,error)&&pending.map==map){
                armRestore();
                HoldRestoredPasses(pending.climate, pending.weather.type, pending.held);
                if(Resolve(pending,error)&&PrepareWorldLook(pending.world,error)){
                    ApplyWorldLook(pending.world);FilmmakerSettings_AcceptCurrent();
                    g_scene=std::move(pending);++g_sceneGeneration;restored=true;
                    g_message="Recovered environment for "+map+". Effects stay off until you switch them on; their settings are kept.";
                }
            }
        }
        for (const char* suffix : {".json", ".json.bak"}) {
            if(restored)break;
            const auto path = Root()/"autosaves"/(map+suffix);
            try {
                if (!std::filesystem::exists(path)) continue;
                if (std::filesystem::file_size(path) > 1024*1024) { error="Autosave exceeds 1 MB"; continue; }
                std::ifstream file(path, std::ios::binary);
                std::string bytes((std::istreambuf_iterator<char>(file)), {});
                Scene scene;
                if (!ParseScene(bytes, scene, error) || scene.map != map) continue;
                armRestore();
                HoldRestoredPasses(scene.climate, scene.weather.type, scene.held);
                if (!Resolve(scene, error) || !PrepareWorldLook(scene.world, error)) continue;
                ApplyWorldLook(scene.world);
                FilmmakerSettings_AcceptCurrent();
                g_scene = std::move(scene);
                ++g_sceneGeneration;
                restored = true;
                g_message = "Restored environment for " + map + ". Effects stay off until you switch them on; their settings are kept.";
                break;
            } catch (const std::exception& e) { error=e.what(); }
        }
        if (restored) {
            g_restoreSettling = true;
            g_restoreFirstPresent = RenderCompletedPresents();
            g_restoreMap = map;
        } else {
            std::error_code ec;
            std::filesystem::remove(RestoringMarker(map), ec);
        }
        // A snapshot from a different map must not be rebound to this map.
        if (!restored && !g_scene.map.empty() && g_scene.map != map) g_scene = Scene{};
        Scene initial = LiveScene(); initial.map = map;
        if(!g_autosave.dirty)g_autosave.Bind(map, SerializeScene(initial));
        MvmDebugLog_LinefAlways("environment.autosave", "restore map=%s success=%d error=%s", map.c_str(), int(restored), error.c_str());
    }
    const auto now = GetTickCount64();
    if (now >= g_nextAutosavePoll && !g_drag.active) {
        g_nextAutosavePoll = now + 250;
        Scene committed = LiveScene();
        if (committed.map.empty() || committed.map == map) {
            committed.map = map;
            g_autosave.Observe(SerializeScene(committed), now);
        }
    }
    FlushAutosave(false);
}
// A slider drag sends a stream of edits: the ones that follow each other closely are one undo step
// (the scene from before the first of them).
void Remember() {
    const ULONGLONG now = GetTickCount64();
    const bool continuing = !g_undo.empty() && now - g_lastRemember < 600;
    g_lastRemember = now;
    if (continuing) { g_undoStamps.back() = NextSceneUndoStamp(); return; }
    PushUndoScene(LiveScene());
}
// A new object starts `distance` ahead of the view you are looking through -- the camera bridge's,
// the one Props places from. g_camera is whatever camera the Environment last saw render, and a
// preview scene (the Assets turntable) or another pass could leave it anywhere: a light once
// landed across the map. A surface nearer than `distance` stops it just short of the surface.
void Place(Object &o, float distance) {
    double origin[3] = {}, view[3] = {}, fov = 90;
    CameraBridge_GetCurrentCamera(origin, view, fov);
    Vec3 eye{float(origin[0]), float(origin[1]), float(origin[2])};
    Vec3 forward;
    {
        const double pitch = view[0] * 3.14159265358979 / 180.0, yaw = view[1] * 3.14159265358979 / 180.0;
        forward = {float(std::cos(pitch) * std::cos(yaw)), float(std::cos(pitch) * std::sin(yaw)), float(-std::sin(pitch))};
    }
    if (!Finite(eye) || !Finite(forward) || Dot(forward, forward) < .5f) {
        if (!g_cameraValid) throw std::runtime_error("Waiting for a rendered camera");
        eye = g_camera; forward = g_forward;
    }
    float reach = distance;
    if (auto world = MapOcclusion()) {
        SurfaceHit hit;
        if (SurfaceRaycast(*world, eye, forward, distance, hit)) reach = (std::max)(8.f, hit.distance - 16.f);
    }
    o.position = eye + forward * reach;
    if (o.IsLight())
        o.angles = {float(view[0]), float(view[1]), 0.f};
}
float Number(const char *value) {
    char *end = nullptr;
    float n = strtof(value, &end);
    if (end == value || *end || !std::isfinite(n))
        throw std::runtime_error("Enter a finite number");
    return n;
}
int Id(const char *value) {
    float n = Number(value);
    if (n < 1 || n > 1000000 || std::floor(n) != n)
        throw std::runtime_error("Invalid object ID");
    return int(n);
}
void SetVector(Vec3 &v, const std::string &key, float n) {
    if (key == "x" || key == "r")
        v.x = n;
    else if (key == "y" || key == "g")
        v.y = n;
    else if (key == "z" || key == "b")
        v.z = n;
    else
        throw std::runtime_error("Unknown vector field");
}
int NextId(const Scene &s) {
    int id = 1;
    for (auto &o : s.objects)
        id = std::max(id, o.id + 1);
    return id;
}
} // namespace
// Overlay a copy: CONFIG, undo snapshots and autosaves retain authored values.
static Scene AnimatedScene(Scene scene) {
    const auto state = WorldTransient::Read();
    auto number = [&](const std::string& id, double base, size_t component = 0) {
        auto f = state->fields.find(id);
        return f != state->fields.end() && component < f->second.size() ? f->second[component] : base;
    };
    for (int i=0;i<ClimateCount;++i) scene.climate.values[i]=float(number(std::string("env.")+ClimateSpecs()[i].key,scene.climate.values[i]));
    scene.enabled=number("env.enabled",scene.enabled)!=0;
    scene.exposure=float(number("env.exposure",scene.exposure));
    auto color=[&](const char* id,Vec3 c) {return Vec3{float(number(id,c.x*255,0)/255),float(number(id,c.y*255,1)/255),float(number(id,c.z*255,2)/255)};};
    scene.fogColor=color("env.fogColor",scene.fogColor);scene.tint=color("env.tint",scene.tint);
    scene.weather.color=color("env.weather.color",scene.weather.color);
    scene.weather.type=int(number("env.weather.type",scene.weather.type));
    scene.weather.speed=float(number("env.weather.speed",scene.weather.speed));
    scene.weather.size=float(number("env.weather.size",scene.weather.size));
    scene.weather.windX=float(number("env.weather.windX",scene.weather.windX));
    scene.weather.windY=float(number("env.weather.windY",scene.weather.windY));
    return scene;
}
std::map<std::string,std::vector<double>> CaptureAnimationValues() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::map<std::string,std::vector<double>> out;
    for(int i=0;i<ClimateCount;++i)out[std::string("env.")+ClimateSpecs()[i].key]={g_scene.climate.values[i]};
    out["env.enabled"]={double(g_scene.enabled)};out["env.exposure"]={g_scene.exposure};
    auto color=[&](const char* id,Vec3 c){out[id]={c.x*255.,c.y*255.,c.z*255.};};
    color("env.fogColor",g_scene.fogColor);color("env.tint",g_scene.tint);color("env.weather.color",g_scene.weather.color);
    out["env.weather.type"]={double(g_scene.weather.type)};out["env.weather.speed"]={g_scene.weather.speed};
    out["env.weather.size"]={g_scene.weather.size};out["env.weather.windX"]={g_scene.weather.windX};out["env.weather.windY"]={g_scene.weather.windY};
    return out;
}
void RunMainThreadFrame() {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto map = CurrentMap();
    UpdateMapOcclusion(map, Root() / "occlusion");
    UpdateAutosave(map);
    if (g_restoreAutosave) return;
    const auto geometry=MapOcclusion();
    if(geometry&&Relight::UpdateMapLighting(map,geometry->lightMetadataPath)&&g_savedName.empty()){
        const auto lights=Relight::CaptureScene(map);
        if(lights.hasSun){g_scene.climate[CL_sunElevation]=lights.sunAngles.x;g_scene.climate[CL_sunAzimuth]=lights.sunAngles.y;}
    }
    g_liveWorld=CaptureWorldLook();
    const auto animated = AnimatedScene(g_scene);
    bool active = animated.enabled && !map.empty() && map.rfind("ui/", 0) != 0 && map.rfind("ui_", 0) != 0 &&
                  (animated.map.empty() || animated.map == map);
    auto climate = EffectiveClimate(animated.climate);
    // Keep the native image intact until its replacement can actually render. This also
    // restores native lighting on device loss, rather than showing an unlit preparation frame.
    const bool rendering=active && g_cameraValid && RendererReady();
    // Our sky owns the sun while it is up, disc and halo included. Scoped to the sky rather than
    // to the lighting pass, because the leftover halo is a SKY artefact: it shows up whenever the
    // backdrop is replaced, whether or not the surfaces are being relit.
    SetEngineSunDiscSuppressed(rendering && climate[CL_sky] > 0);
    // The owned render pipeline supplies its own lighting inputs. Environment's
    // engine-side overrides must not alter its baseline or unsupported skybox draws.
    // Baked lighting is not an engine switch: CL_bakedSuppressed substitutes the lightmap inputs
    // inside the native-material replay (EnvironmentNativeMaterial.cpp), so nothing is asked of
    // the engine here for it.
    const auto authoredLighting=Relight::CaptureScene(map);
    const bool lightingReady=Relight::MapLightingReady(authoredLighting,map,bool(geometry));
    // The map's lighting is only taken over when Lighting is switched on (CL_relighting) or the
    // light editor has map lights to own. Weather, fog, sky or grading alone leave CS2's own
    // lighting in place: switching on rain must not relight the map.
    const bool relightingOn=active && climate[CL_relighting]>.5f;
    const bool ownedLighting=(lightingReady && (relightingOn || authoredLighting.enabled)) ||
                             (geometry && g_proof.enabled && g_proof.map==map);
    SetEngineLightingTakeover(g_cameraValid && RendererReady() && ownedLighting);
    Relight::UpdateStainedGlassMaterials((active || Relight::CaptureScene(map).enabled) && RendererReady());
    UpdateNativeWater(animated,map);
    UpdateNativeRain(animated, map, active, g_camera, g_cameraValid);
}
bool PositionEditing() { std::lock_guard<std::mutex> lock(g_mutex); return g_editing!=0; }
bool RenderedCamera(Matrix &worldToClip, Vec3 &eye) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_cameraValid) return false;
    worldToClip = g_gizmoProjection; eye = g_camera;
    return true;
}
void StopPositionEditing() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if(g_drag.active) {g_scene=g_dragBefore;g_drag.Cancel();}
    g_editing=0;++g_editRevision;
}
void FlushCommittedEdits() {
    std::lock_guard<std::mutex> lock(g_mutex);
    // The queue contains accepted snapshots, never an in-progress gizmo drag.
    FlushAutosave(true);
}
std::string CurrentMapName() { return CurrentMap(); }
std::shared_ptr<const OcclusionMesh> CollisionGeometrySnapshot() {
    std::lock_guard<std::mutex> lock(g_mutex); return MapOcclusion();
}
void OnLevelInit() {
    Relight::ResetStainedGlassMaterials();
    ResetEngineLightingTakeover();
    ResetEngineSunDiscSuppression();
    std::lock_guard<std::mutex> lock(g_mutex);
    FlushAutosave(true);
    // An orderly reload, including the same map, is not a crashed process.
    // Keep an orphaned marker from a previous process for UpdateAutosave to hold.
    if (g_restoreSettling) {
        std::error_code ec;
        std::filesystem::remove(RestoringMarker(g_restoreMap), ec);
        g_restoreSettling = false;
    }
    g_proof.enabled=false;
    Relight::OnLevelChanged();
    g_restoreAutosave = true;
    g_restoreGate.Reset();
    ++g_sceneGeneration;
    ResetNativeWater();
    ResetNativeRain();
    g_cameraValid = false;

    ReloadMapOcclusion();
    if (g_drag.active) g_scene = g_dragBefore;
    g_selected = 0; g_editing = 0; g_drag = {}; ++g_editRevision;
    g_undo.clear();
    g_undoStamps.clear();
    if (!EffectsPersistence_Keep("effects"))
        g_scene = Scene{};
    // Keep-on leaves the previous scene in memory until the autosave replaces it.
    // Either way dynamic lighting stays off across the demo change; the restore
    // path disarms the saved switch again after it copies the saved values in.
    HoldRestoredPasses(g_scene.climate, g_scene.weather.type, g_scene.held);
    MvmDebugLog_LinefAlways("environment.level", "cleared=%d mapBinding=%s relighting=0",
                            !EffectsPersistence_Keep("effects"), g_scene.map.c_str());
}
void SetRenderStatus(const char *status, bool drawn) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    if (g_status != status) {
        g_status = status;
        MvmDebugLog_LinefAlways("environment.render", "status=%s", status);
    }
    if (drawn)
        ++g_draws;
}
// WHERE THE PLAYERS ARE, so the world coatings and rain impacts can leave them alone.
//
// The coating masks are a function of world position, which is right for the map and wrong for
// anything that moves: a pattern painted over a running player slides across them. Players get no
// world coating, and the shader finds them from these pawn origins.
//
// SEH-guarded and index-scanned for the same reason the cosmetics code is: a stale entity in the
// list must read as "no actor", not take the render thread down. Failing to find anyone is not an
// error -- nothing is excluded.
void CaptureCoatingActors(Frame &f) {
    f.actors.clear();
    const auto &c = f.scene.climate;
    if (!(c[CL_snow] > 0 || c[CL_wet] > 0 || c[CL_puddles] > 0 || (f.scene.weather.type == 1 && f.scene.weather.count > 0)))
        return;
    __try {
        const int highest = GetHighestEntityIndex();
        for (int i = 1; i <= highest && (int)f.actors.size() < Frame::kMaxActors; ++i) {
            CEntityInstance *ent = CosmeticEntFromIndex(i);
            if (!ent || !ent->IsPlayerPawn() || ent->GetHealth() == 0)
                continue;
            float x = 0, y = 0, z = 0;
            ent->GetOrigin(x, y, z);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                continue;
            CoatingActor a;
            a.origin = {x, y, z};
            // Body-sized: the shader's CoatingActor also rejects floor and walls, but a wider
            // column still let the ground around the feet read as body.
            a.radius = kCoatingBodyRadius;
            f.actors.push_back(a);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        f.actors.clear();
    }
}
// WHERE THE SMOKES ARE. CS2's OIT absorbance says how much a smoke hides at each pixel but not how
// far away it is; fog, rain and props need that to stay visible in FRONT of a smoke and vanish
// BEHIND it. A detonated smoke is placed as a sphere around its detonation point -- an
// approximation of the voxel cloud that only has to get the ordering right.
static constexpr float kSmokeRadius = 200.f; // a bloomed CS2 cloud's typical half-width
static bool SmokeVolumeOf(CEntityInstance *ent, SmokeVolume &out) {
    const auto &o = g_clientDllOffsets.C_SmokeGrenadeProjectile;
    __try {
        if (!*(bool *)((unsigned char *)ent + o.m_bDidSmokeEffect))
            return false;
        if (o.m_nSmokeEffectTickBegin) {
            const int start = *(int *)((unsigned char *)ent + o.m_nSmokeEffectTickBegin);
            const double age = g_MirvTime.curtime_get() - double(start) / 64.0;
            // OIT still supplies actual density/fade. This only removes future or dead
            // projectiles after a seek; leave the entire rendered tail available.
            if (start <= 0 || age < 0 || age > 24) return false;
        }
        const float *p = (const float *)((unsigned char *)ent + o.m_vSmokeDetonationPos);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
            return false;
        out.center = {p[0], p[1], p[2]};
        out.radius = kSmokeRadius;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void CaptureSmokeVolumes(Frame &f) {
    f.smokes.clear();
    const auto &o = g_clientDllOffsets.C_SmokeGrenadeProjectile;
    if (!o.m_vSmokeDetonationPos || !o.m_bDidSmokeEffect || !f.active)
        return;
    const int highest = GetHighestEntityIndex();
    for (int i = 1; i <= highest && (int)f.smokes.size() < Frame::kMaxSmokes; ++i) {
        CEntityInstance *ent = CosmeticEntFromIndex(i);
        if (!ent)
            continue;
        const char *cls = ent->GetClassName();
        if (!cls || std::strcmp(cls, "smokegrenade_projectile") != 0)
            continue;
        SmokeVolume v;
        if (SmokeVolumeOf(ent, v))
            f.smokes.push_back(v);
    }
}
Frame CaptureFrame(const Matrix &projection, const Matrix &worldToClip) {
    Frame f;
    std::lock_guard<std::mutex> lock(g_mutex);
    f.scene = AnimatedScene(g_scene);
    // Same-map reloads retain authored state, but must render the native world
    // during the buffer instead of immediately reusing the previous scene.
    if (g_restoreAutosave) f.scene = Scene{};
    f.scene.world=g_liveWorld;
    // Outliner "in renders" off: shown while you work, left out of what is recorded.
    if (AfxStreams_IsRcording())
        f.scene.objects.erase(std::remove_if(f.scene.objects.begin(), f.scene.objects.end(),
            [](const Object &o) { return !o.render; }), f.scene.objects.end());
    f.generation = g_sceneGeneration;
    f.occlusion = MapOcclusion();
    auto map = CurrentMap();
    f.proof=g_proof;f.proof.enabled=g_proof.enabled&&g_proof.map==map;
    f.relight=Relight::CaptureScene(map);
    if (g_restoreAutosave) { f.relight.enabled = false; f.proof.enabled = false; }
    const bool sceneActive=f.scene.enabled&&(f.scene.map.empty()||f.scene.map==map);
    const bool lightingReady=Relight::MapLightingReady(f.relight,map,bool(f.occlusion));
    if(!lightingReady)f.relight.enabled=false;
    // Only the Lighting switch hands the map's lighting to the Environment. Weather, fog, sky or
    // grading alone keep CS2's own lighting: applying the Environment's sun, sky light and bounce
    // whenever the pass was on is what made switching on Weather "turn on the sky lighting and GI"
    // (user report, 2026-10-09). Same rule as the engine-side takeover in RunMainThreadFrame.
    const bool relightingOn=EffectiveClimate(f.scene.climate)[CL_relighting]>.5f;
    if(sceneActive){
        if(lightingReady&&relightingOn){
            const auto authored=f.relight;
            Relight::ApplyEnvironmentLighting(f.relight,f.scene.climate,f.scene.world);
            f.bakedDaylight=Relight::DaylightEnergyRatio(authored,f.relight);
        }
        else if(f.relight.enabled)Relight::ApplyGlobalLighting(f.relight,f.scene.world);
        // The native material path now owns all illumination. The differential
        // baked-colour replay must not add its former fill on top of that result.
        f.scene.climate[CL_relighting]=0;f.scene.climate[CL_relight]=0;
        f.scene.climate[CL_bakedSuppressed]=0;f.scene.climate[CL_bakedFill]=0;
        // Bounce is the native material's probe volume (Relight probe GI). The composite's own
        // world bounce would add a second, screen-space copy of it on top.
        f.scene.climate[CL_worldGi]=0;
    }
    else if(f.relight.enabled)Relight::ApplyGlobalLighting(f.relight,f.scene.world);
    Relight::ApplyLightChoices(f.relight);
    f.active = (sceneActive||f.proof.enabled||f.relight.enabled) && !map.empty() &&
               map.rfind("ui/", 0) != 0 && map.rfind("ui_", 0) != 0;
    // Relight is independent of saved Environment enablement. An inactive or other-map
    // Environment scene must not contribute objects/weather just because Relight runs.
    if(!sceneActive&&(f.proof.enabled||f.relight.enabled))f.scene=Scene{};
    f.projection = projection;
    f.worldToClip = worldToClip;
    g_gizmoProjection = worldToClip;
    f.hasWorldToClip = true;
    Matrix inverse;
    g_cameraValid = Inverse(worldToClip, inverse);
    if (g_cameraValid) {
        Matrix inverseView = Multiply(inverse, projection);
        g_camera = Transform(inverseView, {0, 0, 0});
        g_forward = Normalize(Transform(inverseView, {0, 0, -1}) - g_camera);
        g_angles = {std::asin(std::clamp(-g_forward.z, -1.f, 1.f)) / Radians(1),
                    std::atan2(g_forward.y, g_forward.x) / Radians(1), 0};
        g_cameraValid = Finite(g_camera) && Finite(g_forward);
        f.camera = g_camera;
        f.angles = g_angles;
    }
    if (!g_cameraValid)
        f.active = false;
    if (!g_MirvTime.GetCurrentDemoTime(f.time))
        f.time = g_MirvTime.curtime_get();
    if (!std::isfinite(f.time))
        f.active = false;
    // GLB animation: each object playing a clip draws its own copy of the mesh, posed at demo
    // time (so a render and a scrub pose it the same way), at the object's speed.
    for (auto &o : f.scene.objects) {
        if (!o.mesh || !o.mesh->rig || o.clip.empty() || !std::isfinite(f.time)) continue;
        const int clip = GltfClipIndex(*o.mesh->rig, o.clip);
        if (clip < 0) continue;
        auto posed = std::make_shared<Mesh>(*o.mesh);
        std::vector<Vec3> positions, normals;
        GltfSkin(*o.mesh->rig, clip, f.time * double(o.clipSpeed), positions, normals);
        if (positions.size() != posed->vertices.size()) continue;
        for (size_t i = 0; i < positions.size(); ++i) {
            posed->vertices[i].position = positions[i];
            posed->vertices[i].normal = normals[i];
        }
        o.mesh = posed;
    }
    const auto sceneLights=f.active?Relight::ApplySceneObjectLights(f.relight,f.scene.objects,f.time):0u;
    if(MvmDebugLog_Active()) {
        static ULONGLONG last=0;const auto now=GetTickCount64();
        if(now-last>=3000){last=now;MvmDebugLog_LinefAlways("environment.scene.lights",
            "active=%d native=%d emitters=%u total=%zu time=%.3f",int(f.active),int(f.relight.enabled),sceneLights,f.relight.lights.size(),f.time);}
    }
    CaptureVfxLights(f.relight,f.time,map,f.active);
    CaptureCoatingActors(f);
    CaptureSmokeVolumes(f);
    return f;
}
std::vector<PickBox> ScenePickBoxes() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<PickBox> out;
    if (!g_scene.map.empty() && g_scene.map != CurrentMap()) return out;
    for (const auto &o : g_scene.objects) {
        if (!o.enabled || o.locked) continue;
        PickBox box;
        box.id = o.id;
        box.center = o.position;
        box.kind = o.IsLight() ? o.kind : "mesh";
        box.color = o.color;
        box.selected = o.id == g_selected;
        if (o.IsLight()) {
            box.name = o.kind == "spot" ? "Spot light" : o.kind == "area" ? "Area light" : "Point light";
        } else if (o.kind == "obj") {
            box.name = std::filesystem::path(o.asset).stem().string();
        } else {
            box.name = o.kind;
            if (!box.name.empty()) box.name[0] = char(toupper(static_cast<unsigned char>(box.name[0])));
        }
        if (o.IsLight()) {
            // The light's dot: drawn at a fixed screen size over the view (Props marks it), and
            // a 24-unit box for the hover outline.
            box.light = true;
            const Vec3 h{12, 12, 12};
            for (int i = 0; i < 8; ++i)
                box.corners[i] = o.position + Vec3{(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z};
        } else {
            // The mesh's own bounds, measured once per mesh: this runs every frame for the view's
            // markers and hover, and walking a large GLB's vertices each time cost frames.
            Vec3 lo{-1, -1, -1}, hi{1, 1, 1};
            if (o.mesh && !o.mesh->vertices.empty()) {
                struct Bounds { size_t count; Vec3 lo, hi; };
                static std::unordered_map<const void *, Bounds> s_bounds;
                auto found = s_bounds.find(o.mesh.get());
                if (found == s_bounds.end() || found->second.count != o.mesh->vertices.size()) {
                    if (s_bounds.size() > 512) s_bounds.clear();
                    Bounds b{o.mesh->vertices.size(), o.mesh->vertices[0].position, o.mesh->vertices[0].position};
                    for (const auto &v : o.mesh->vertices) {
                        b.lo = {std::min(b.lo.x, v.position.x), std::min(b.lo.y, v.position.y), std::min(b.lo.z, v.position.z)};
                        b.hi = {std::max(b.hi.x, v.position.x), std::max(b.hi.y, v.position.y), std::max(b.hi.z, v.position.z)};
                    }
                    found = s_bounds.insert_or_assign(o.mesh.get(), b).first;
                }
                lo = found->second.lo; hi = found->second.hi;
            }
            const Matrix model = ModelMatrix(o.position, o.angles, o.scale);
            for (int i = 0; i < 8; ++i)
                box.corners[i] = Transform(model, {(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z});
        }
        out.push_back(box);
    }
    return out;
}
void SelectInView(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_drag.active) { g_scene = g_dragBefore; g_drag.Cancel(); }
    g_drag = {};
    if (id > 0 && std::any_of(g_scene.objects.begin(), g_scene.objects.end(), [&](const Object &o) { return o.id == id; })) {
        g_selected = g_editing = id;
        g_message = "Drag an arrow to move it. Click elsewhere to deselect.";
    } else {
        g_editing = 0;
    }
    ++g_editRevision;
}
void AppendStateJson(std::ostringstream &out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_scanned) {
        try {
            Scan();
        } catch (const std::exception &e) {
            g_message = e.what();
        }
    }
    out << ",\"environment\":" << SerializeScene(LiveScene()) << ",\"envSelected\":" << g_selected
        << ",\"envMessage\":\"" << JsonBuilder::Escape(g_message) << "\",\"envSaved\":\""
        << JsonBuilder::Escape(g_savedName)
        << "\",\"envDirty\":" << (SerializeScene(LiveScene()) != g_savedJson ? "true" : "false")
        << ",\"envUndo\":" << (!g_undo.empty() ? "true" : "false") << ",\"envMap\":\""
        << JsonBuilder::Escape(CurrentMap()) << "\"";
    auto list = [&](const char *name, const std::vector<std::string> &items) {
        out << ",\"" << name << "\":[";
        bool first = true;
        for (auto &i : items) {
            if (!first)
                out << ',';
            first = false;
            out << '\"' << JsonBuilder::Escape(i) << '\"';
        }
        out << ']';
    };
    out << ",\"lightEditor\":" << (Relight::LightEditorActive() ? "true" : "false")
        << ",\"lightsOff\":" << Relight::LightsSwitchedOff();
    out << ",\"envEditing\":" << g_editing << ",\"envDragging\":" << (g_drag.active ? "true" : "false")
        << ",\"envEditRevision\":" << g_editRevision;
    out << ",\"envGizmo\":";
    auto selected = std::find_if(g_scene.objects.begin(), g_scene.objects.end(), [](const Object &o){return o.id==g_selected;});
    if (!g_cameraValid || selected==g_scene.objects.end() || (!g_scene.map.empty() && g_scene.map!=CurrentMap())) out << "null";
    else {
        const auto &o=*selected;
        auto point=[&](Vec3 p) {
            const auto q=GizmoProject(g_gizmoProjection,p);
            out << '[' << q.x << ',' << q.y << ',' << (q.visible?"true":"false") << ']';
        };
        out << "{\"origin\":"; point(o.position); out << ",\"axes\":[";
        float size=GizmoWorldSize(g_gizmoProjection,o.position);
        for(int i=0;i<3;++i){if(i)out<<',';point(o.position+GizmoAxis(i)*size);}
        out << "],\"bounds\":[";
        const Matrix model=ModelMatrix(o.position,o.angles,o.IsLight()?Vec3{8,8,8}:o.scale);
        for(int i=0;i<8;++i){if(i)out<<',';point(Transform(model,{i&1?1.f:-1.f,i&2?1.f:-1.f,i&4?1.f:-1.f}));}
        out << "]}";
    }
    // The Effects > Lighting switch mirrors the climate rather than an engine cvar: baked
    // lighting is off exactly when an enabled scene asks for the lightmap substitution.
    {
        const auto& live = LiveScene();
        const bool suppressed = live.enabled && EffectiveClimate(live.climate)[CL_bakedSuppressed] > .5f;
        out << ",\"bakedLighting\":" << (suppressed ? "false" : "true") << ",\"bakedLightingOk\":true";
    }
    std::string occlusionStatus = MapOcclusionStatus();
    bool occlusionBusy = MapOcclusionPreparing();
    std::string buildStatus;
    bool buildBusy = occlusionBusy;
    if (occlusionBusy)
        buildStatus = occlusionStatus;
    out << ",\"envOcclusionReady\":" << (MapOcclusion() ? "true" : "false") << ",\"envOcclusionStatus\":\""
        << JsonBuilder::Escape(occlusionStatus) << "\"";
    // Map data is extracted and loaded asynchronously; report that work in the panel.
    std::lock_guard<std::mutex> statusLock(g_statusMutex);
    if (g_status.find("Preparing") != std::string::npos || g_status.find("Waiting") == 0) {
        buildBusy = true;
        if (buildStatus.empty())
            buildStatus = g_status;
    }
    if (buildStatus.empty())
        buildStatus = occlusionBusy ? occlusionStatus : (g_status.empty() ? occlusionStatus : g_status);
    out << ",\"envOcclusionPreparing\":" << (buildBusy ? "true" : "false");
    out << ",\"envBuildBusy\":" << (buildBusy ? "true" : "false") << ",\"envBuildStatus\":\""
        << JsonBuilder::Escape(buildStatus) << "\"";
    list("envPresets", g_presets);
    list("envAssets", g_assets);
    list("envTextures",g_textures);
    // The animations each placed GLB carries, by object id: the inspector's clip picker.
    out << ",\"envClips\":{";
    bool firstClip = true;
    for (const auto &o : g_scene.objects) {
        if (!o.mesh || !o.mesh->rig || o.mesh->rig->clips.empty()) continue;
        out << (firstClip ? "" : ",") << '\"' << o.id << "\":[";
        firstClip = false;
        for (size_t i = 0; i < o.mesh->rig->clips.size(); ++i)
            out << (i ? "," : "") << '\"' << JsonBuilder::Escape(o.mesh->rig->clips[i].name) << '\"';
        out << ']';
    }
    out << '}';
    out << ",\"envRender\":\"" << JsonBuilder::Escape(g_status) << "\",\"envDraws\":" << g_draws;
}
namespace {
bool UndoLastEditLocked(std::string &error) {
    if (g_undo.empty()) { error = "Nothing to undo"; return false; }
    if (!PrepareWorldLook(g_undo.back().world, error)) return false;
    ApplyWorldLook(g_undo.back().world);
    FilmmakerSettings_AcceptCurrent();
    g_scene = g_undo.back();
    g_restoreAutosave = false;
    ++g_sceneGeneration;
    g_undo.pop_back();
    g_undoStamps.pop_back();
    g_lastRemember = 0;
    g_selected = 0;
    QueueCommittedScene();
    return true;
}
} // namespace
uint64_t LastUndoStamp() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_undoStamps.empty() ? 0 : g_undoStamps.back();
}
bool UndoLastEdit(std::string &error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!UndoLastEditLocked(error)) return false;
    g_message = "Undid last scene edit.";
    return true;
}
void RunCommand(advancedfx::ICommandArgs *args) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const int argc = args->ArgC();
    auto arg = [&](int i) -> std::string { return i < argc ? args->ArgV(i) : ""; };
    auto required = [&](int n) {
        if (argc != n)
            throw std::runtime_error("Wrong argument count; use environment help");
    };
    try {
        if (!g_scanned)
            Scan();
        auto op = arg(2);
        if(op=="particle-shadows") {
            if(argc==4){const auto value=arg(3);if(value!="0"&&value!="1")throw std::runtime_error("particle-shadows expects 0 or 1");SetParticleShadowsEnabled(value=="1");}
            else required(3);
            advancedfx::Message("environment particle-shadows: %d (temporary A/B control; native effects and illumination retained)\n",int(ParticleShadowsEnabled()));return;
        }
        if(op=="proof") {
            const auto action=arg(3);
            if(action=="load") {
                required(6);std::ifstream file(std::filesystem::u8path(arg(4)),std::ios::binary|std::ios::ate);
                if(!file||file.tellg()<0||file.tellg()>4*1024*1024)throw std::runtime_error("Invalid proof metadata size");
                auto bytes=static_cast<size_t>(file.tellg());file.seekg(0);std::string text(bytes,'\0');file.read(text.data(),bytes);
                JsonValue root;if(!file||!JsonParse(text,root))throw std::runtime_error("Invalid proof metadata JSON");
                const auto* map=root.Find("map");const auto* lights=root.Find("lights");const auto* format=root.Find("format");
                if(!map||map->AsString()!=CurrentMap()||!lights||lights->type!=JsonValue::Type::Array||!format||format->AsInt()!=1)
                    throw std::runtime_error("Proof metadata map/format mismatch");
                LightingProof next;next.map=map->AsString();unsigned matches=0;
                for(const auto& light:lights->arr){
                    auto id=light.Find("identity");if(!id||id->AsString()!=arg(5))continue;++matches;
                    auto kind=light.Find("kind");if(!kind||kind->AsString()!="light_omni2")throw std::runtime_error("Proof requires authored omni2");
                    auto number=[&](const char* key){auto v=light.Find(key);if(!v||v->type!=JsonValue::Type::Number||!std::isfinite(v->number))throw std::runtime_error("Invalid fixture number");return float(v->number);};
                    auto vector=[&](const char* key){auto v=light.Find(key);if(!v||v->type!=JsonValue::Type::Array||v->arr.size()!=3)throw std::runtime_error("Invalid fixture vector");Vec3 out;float* values=&out.x;for(int i=0;i<3;++i){if(v->arr[i].type!=JsonValue::Type::Number||!std::isfinite(v->arr[i].number))throw std::runtime_error("Invalid fixture component");values[i]=float(v->arr[i].number);}return out;};
                    next.identity=id->AsString();next.position=vector("origin");next.color=vector("color")*(1.f/255);
                    next.lumens=number("brightnessLumens");next.authoredScale=number("brightnessScale");next.range=number("range");
                    if(next.lumens<0||next.lumens>1e7||next.authoredScale<0||next.authoredScale>100||next.range<1||next.range>10000||!Finite(next.position)||!Finite(next.color))throw std::runtime_error("Fixture outside proof bounds");
                }
                if(matches!=1)throw std::runtime_error("Fixture identity missing or ambiguous");
                next.generation=g_proof.generation+1;g_proof=next;
            } else if(action=="on"||action=="off") {
                required(4);if(action=="on"&&(g_proof.identity.empty()||g_proof.map!=CurrentMap()))throw std::runtime_error("Load an authored fixture for this map first");
                g_proof.enabled=action=="on";++g_proof.generation;
            } else if(action=="fixture"||action=="intensity"||action=="range"||action=="indirect"||action=="shadows") {
                required(5);float n=Number(args->ArgV(4));
                if(n<0||n>(action=="range"?10000.f:action=="intensity"?100.f:1.f)||(action=="range"&&n<1))throw std::runtime_error("Proof control out of range");
                if(action=="shadows")g_proof.shadows=n!=0;else if(action=="fixture")g_proof.fixture=n!=0;else if(action=="intensity")g_proof.intensity=n;else if(action=="range")g_proof.range=n;else g_proof.indirect=n;
                ++g_proof.generation;
            } else if(action=="color") {
                required(7);Vec3 color{Number(args->ArgV(4)),Number(args->ArgV(5)),Number(args->ArgV(6))};
                if(color.x<0||color.y<0||color.z<0||color.x>1||color.y>1||color.z>1)throw std::runtime_error("Proof color must be sRGB 0..1");g_proof.color=color;++g_proof.generation;
            } else if(action!="status")throw std::runtime_error("proof load <metadata.json> <identity> | on|off|status | fixture|intensity|range|indirect|shadows <value> | color <r> <g> <b>");
            if(action!="status")++g_sceneGeneration;
            advancedfx::Message("proof: enabled=%d generation=%llu fixture=%d lumens=%g multiplier=%g range=%g indirect=%g identity='%s'; experimental native direct lighting; material coverage incomplete\n",g_proof.enabled,g_proof.generation,g_proof.fixture,g_proof.lumens,g_proof.intensity,g_proof.range,g_proof.indirect,g_proof.identity.c_str());return;
        }
        if (op.empty() || op == "status") {
            std::lock_guard<std::mutex> statusLock(g_statusMutex);
            advancedfx::Message("environment: enabled=%d map='%s' current='%s' objects=%zu weather=%d "
                                "render='%s' draws=%llu selected=%d\n%s\n",
                                g_scene.enabled, g_scene.map.c_str(), CurrentMap().c_str(),
                                g_scene.objects.size(), g_scene.weather.type, g_status.c_str(), g_draws,
                                g_selected, SerializeScene(LiveScene()).c_str());
            advancedfx::Message("  mapdata: ready=%d status='%s'\n",bool(MapOcclusion()),MapOcclusionStatus().c_str());
            return;
        }
        if (op == "climate" && argc == 3) {
            for (int i = 0; i < ClimateCount; ++i) {
                const auto &spec = ClimateSpecs()[i];
                advancedfx::Message("%s: %g (range %g..%g, default %g, page %s)\n", spec.key,
                                    g_scene.climate.values[i], spec.lo, spec.hi, spec.initial, spec.page);
            }
            return;
        }
        if (op == "help") {
            advancedfx::Message(
                "Environment UI: Looks, Sky, Lighting, Fog, Weather, Scene Objects.\n"
                "Fog: climate fogDensity|fogStart|fogHeight|fogNoise; set fogR|fogG|fogB.\n"
                "Rendering: Balanced preview / High export (fixed). Presets: version 14 only.\n"
                "mirv_filmmaker environment on|off|status|undo|rescan|folder\n"
                " environment weather wind <speed> <direction-degrees>\n"
                " environment reset [all|lighting|sky|atmosphere|weather|props]\n"
                " environment weather clear|rain|snow|ash|storm|embers|motes|smoke\n"
                " environment look clear|storm|winter|sunset|night; climate <field> <value>\n"
                " environment surface-center|fog-center|gi-center|reload-geometry\n"
                " environment weather set "
                "type|count|seed|radius|height|speed|size|windX|windY|follow|shelter|x|y|z|r|g|b <number>\n"
                " environment set exposure|ambient|fogR|fogG|fogB|tintR|tintG|tintB <number>\n"
                " environment add point|spot|area|obj [asset-name]\n"
                " environment select|remove|duplicate <id>; environment place <id> <distance>\n"
                " environment object <id> "
                "x|y|z|pitch|yaw|roll|sx|sy|sz|r|g|b|intensity|range|cone|enabled|shadows|width|height|sourceRadius|pattern|profile|cue|patternScale|rate|amount|phase "
                "<number>\n"
                " environment object <id> materialEnabled|cameraVisible|roughness|metallic|coat|coatRoughness|anisotropy|emission|normalStrength|uvScale <number>\n"
                " environment edit start <id>|stop|begin <axis> <u> <v> <aspect>|drag <u> <v>|commit|cancel\n"
                " environment smokeview [on|off] - show the CS2 smoke coverage that hides additions\n"
                " environment mapinfo [rain|ripple|ripdir|wet|drying|trace <value>] - read/write the map's native weather\n"
                " environment trace-probe - test hardware geometry tracing (debug log)\n"
                " environment particle-shadows [0|1] - temporary smoke shadow A/B control\n"
                " environment texture <id> albedo|normal|roughness <asset-name or ->\n"
                " environment save|load <preset-name>; environment bind (keep scene, bind to current map)\n");
            return;
        }
        if (op == "edit") {
            const auto action=arg(3);
            if (action=="start") {
                required(5);
                const int id=Id(args->ArgV(4));
                auto it=std::find_if(g_scene.objects.begin(),g_scene.objects.end(),[&](const Object &o){return o.id==id;});
                if(it==g_scene.objects.end())throw std::runtime_error("Object no longer exists");
                if(!ConfigHud_Enabled() || !g_cameraValid)throw std::runtime_error("Open Config and wait for a rendered camera");
                if(!g_scene.map.empty() && g_scene.map!=CurrentMap())throw std::runtime_error("Object belongs to another map");
                if(g_drag.active) g_scene=g_dragBefore;
                g_drag={};g_selected=g_editing=id;++g_editRevision;
                g_message="Drag an axis to move. Release to keep; Escape cancels.";
            } else if(action=="begin") {
                required(8); // axis, normalized viewport x/y, viewport aspect
                if(!g_editing || g_drag.active)throw std::runtime_error("Choose Edit position first");
                const int axis=arg(4)=="x"?0:arg(4)=="y"?1:arg(4)=="z"?2:-1;
                auto it=std::find_if(g_scene.objects.begin(),g_scene.objects.end(),[&](const Object &o){return o.id==g_editing;});
                if(it==g_scene.objects.end() || axis<0)throw std::runtime_error("Invalid object or axis");
                float u=Number(args->ArgV(5)),v=Number(args->ArgV(6)),aspect=Number(args->ArgV(7));
                auto origin=GizmoProject(g_gizmoProjection,it->position);
                auto tip=GizmoProject(g_gizmoProjection,it->position+GizmoAxis(axis)*GizmoWorldSize(g_gizmoProjection,it->position));
                if(u<0||u>1||v<0||v>1||aspect<.1f||aspect>10 ||
                   GizmoSegmentDistance({u,v,true},origin,tip,aspect)>.025f ||
                   !g_drag.Begin(g_editing,axis,it->position,g_camera,g_gizmoProjection,u,v))
                    throw std::runtime_error("This axis cannot be dragged from the current view");
                g_dragBefore=LiveScene();++g_editRevision;
            } else if(action=="drag") {
                required(6);
                if(!g_drag.active) return;
                if(!g_drag.Move(Number(args->ArgV(4)),Number(args->ArgV(5)))) return;
                Scene preview=g_scene;
                auto it=std::find_if(preview.objects.begin(),preview.objects.end(),[&](const Object &o){return o.id==g_drag.objectId;});
                if(it==preview.objects.end())throw std::runtime_error("Object no longer exists");
                it->position=g_drag.current;
                std::string error;
                if(!Resolve(preview,error)) { g_message=error;return; }
                g_scene=std::move(preview);
            } else if(action=="commit") {
                required(4);
                if(g_drag.active) {
                    auto moved=std::find_if(g_scene.objects.begin(),g_scene.objects.end(),[&](const Object &o){return o.id==g_drag.objectId;});
                    const Vec3 delta=moved==g_scene.objects.end()?Vec3{}:moved->position-g_drag.origin;
                    if(Dot(delta,delta)>.000001f) {
                        PushUndoScene(g_dragBefore);g_lastRemember=0;
                        MvmDebugLog_LinefAlways("environment.edit","position commit id=%d axis=%d",g_drag.objectId,g_drag.axisIndex);
                    }
                    g_drag.Commit();++g_editRevision;g_restoreAutosave=false;++g_sceneGeneration;QueueCommittedScene();
                }
            } else if(action=="cancel" || action=="stop") {
                required(4);
                if(g_drag.active) {g_scene=g_dragBefore;g_drag.Cancel();}
                else if(action=="cancel")g_editing=0;
                if(action=="stop")g_editing=0;
                ++g_editRevision;
            } else throw std::runtime_error("Use edit start <id>|stop|begin <axis> <u> <v> <aspect>|drag <u> <v>|commit|cancel");
            return;
        }
        // An unrelated edit ends the transient gesture first, so it cannot invalidate its object.
        if(g_drag.active && op!="status") {g_scene=g_dragBefore;g_drag.Cancel();++g_editRevision;}
        if(op=="select"||op=="remove"||op=="add"||op=="load"||op=="reset"||op=="undo")g_editing=0;
        if (op == "mapinfo") {
            // Research surface for CS2's native weather globals on info_map_parameters.
            const auto &o = g_clientDllOffsets.CMapInfo;
            {
                char report[512];
                NativeWorldWeatherReport(report, sizeof(report));
                advancedfx::Message("%s%c", report, 10);
            }
            if (!o.m_flEnvRainStrength) throw std::runtime_error("CMapInfo schema fields are unresolved");
            CEntityInstance *info = nullptr;
            for (int i = 1, n = GetHighestEntityIndex(); i <= n && !info; ++i) {
                CEntityInstance *e = CosmeticEntFromIndex(i);
                const char *c = e ? e->GetClassName() : nullptr;
                if (c && std::strcmp(c, "info_map_parameters") == 0) info = e;
            }
            if (!info) throw std::runtime_error("this map has no info_map_parameters entity");
            unsigned char *base = (unsigned char *)info;
            if (argc > 4) {
                const std::string field = arg(3);
                const float v = std::stof(arg(4));
                ptrdiff_t at = field == "rain" ? o.m_flEnvRainStrength : field == "ripple" ? o.m_flEnvPuddleRippleStrength
                             : field == "ripdir" ? o.m_flEnvPuddleRippleDirection : field == "wet" ? o.m_flEnvWetnessCoverage
                             : field == "drying" ? o.m_flEnvWetnessDryingAmount : 0;
                if (field == "trace" && o.m_bRainTraceToSkyEnabled) *(bool *)(base + o.m_bRainTraceToSkyEnabled) = v != 0;
                else if (at) *(float *)(base + at) = v;
                else throw std::runtime_error("unknown mapinfo field");
            }
            char text[256];
            std::snprintf(text, sizeof(text), "mapinfo trace=%d rain=%.3f ripple=%.3f ripdir=%.3f wet=%.3f drying=%.3f",
                          o.m_bRainTraceToSkyEnabled ? (int)*(bool *)(base + o.m_bRainTraceToSkyEnabled) : -1,
                          *(float *)(base + o.m_flEnvRainStrength), *(float *)(base + o.m_flEnvPuddleRippleStrength),
                          *(float *)(base + o.m_flEnvPuddleRippleDirection), *(float *)(base + o.m_flEnvWetnessCoverage),
                          *(float *)(base + o.m_flEnvWetnessDryingAmount));
            advancedfx::Message("%s%c", text, 10);
            g_message = text;
            return;
        }
        if (op == "shadowview") {
            // Debug: which sun cascade each pixel's shadow lookup resolved to. on|off|toggle.
            // red close, green detail, blue middle, yellow outer, MAGENTA none -- magenta marks
            // surfaces no cascade projects, so no caster geometry can ever shadow them.
            const std::string v = argc > 3 ? arg(3) : "toggle";
            SetShadowCascadeView(v == "on" || v == "1" || (v == "toggle" && !ShadowCascadeView()));
            g_message = ShadowCascadeView()
                            ? "Sun cascade view on (red close, green detail, blue middle, yellow outer, magenta none)"
                            : "Sun cascade view off";
            return;
        }
        if (op == "rainview") {
            // Debug: how much rain reaches each surface (PrecipitationExposure). Grey = open sky,
            // dark blue = sheltered; doorways and arches facing the wind show the graded fringe.
            const std::string v = argc > 3 ? arg(3) : "toggle";
            SetRainExposureView(v == "on" || v == "1" || (v == "toggle" && !RainExposureView()));
            g_message = RainExposureView() ? "Rain exposure view on (grey open, dark blue sheltered)" : "Rain exposure view off";
            return;
        }
        if (op == "smokeview") {
            // Debug: draw CS2's smoke coverage (what hides our additions) as grey. on|off|toggle.
            const std::string v = argc > 3 ? arg(3) : "toggle";
            SetSmokeDebugView(v == "on" || v == "1" || (v == "toggle" && !SmokeDebugView()));
            g_message = SmokeDebugView() ? "Smoke coverage view on" : "Smoke coverage view off";
            return;
        }
        if (op == "trace-probe") { required(3); RequestHardwareRayProbe(CurrentMap()); g_message="Hardware ray probe queued; results in debug log"; return; }
        if (op == "reload-geometry") {
            required(3);
            ReloadMapOcclusion();
            g_message = "Reloading map data";
            return;
        }
        if (op == "folder") {
            required(3);
            auto path = Root();
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        if (op == "rescan") {
            required(3);
            Scan();
            g_message = "Preset and OBJ lists refreshed.";
            return;
        }
        if (op == "save") {
            required(4);
            auto name = arg(3);
            if (!ValidName(name))
                throw std::runtime_error("Use 1-48 letters, numbers, spaces, - or _ for a preset name");
            std::string error, bytes = SerializeScene(LiveScene());
            if (!AtomicSave(Root() / "presets" / (name + ".json"), bytes, error))
                throw std::runtime_error(error);
            g_savedName = name;
            g_savedJson = bytes;
            Scan();
            g_message = "Saved " + name;
            return;
        }
        if (op == "select") {
            required(4);
            int id = Id(args->ArgV(3));
            for (auto &o : g_scene.objects)
                if (o.id == id) {
                    g_selected = id;
                    return;
                }
            throw std::runtime_error("Object no longer exists");
        }
        if (op == "undo") {
            required(3);
            std::string error;
            if (!UndoLastEditLocked(error)) throw std::runtime_error(error);
            g_message = "Undid last scene edit.";
            return;
        }
        Scene next = LiveScene();
        std::string resetPage = "all";
        int nextSelected = g_selected;
        if (op == "on" || op == "off") {
            required(3);
            next.enabled = op == "on";
        } else if (op == "reset") {
            if (argc != 3 && argc != 4)
                throw std::runtime_error("Use reset all|lighting|sky|atmosphere|weather|props");
            if (argc == 4)
                resetPage = arg(3);
            if (resetPage != "all" && resetPage != "lighting" && resetPage != "sky" &&
                resetPage != "atmosphere" && resetPage != "weather" && resetPage != "props")
                throw std::runtime_error("Unknown environment page");
            ResetScenePage(next, resetPage);
            // A reset page's held values are part of what it resets.
            if (resetPage == "all") next.held.clear();
            else for (auto it = next.held.begin(); it != next.held.end();) {
                bool onPage = it->first == HeldWeatherTypeKey() ? resetPage == "weather" : false;
                for (int i = 0; i < ClimateCount; ++i)
                    if (it->first == ClimateSpecs()[i].key) onPage = resetPage == ClimateSpecs()[i].page;
                it = onPage ? next.held.erase(it) : std::next(it);
            }
            nextSelected = 0;
        } else if (op == "bind") {
            required(3);
            if (CurrentMap().empty())
                throw std::runtime_error("Load a map first");
            next.map = CurrentMap();
        } else if (op == "load") {
            required(4);
            if (!ValidName(arg(3)))
                throw std::runtime_error("Invalid preset name");
            auto path = Root() / "presets" / (arg(3) + ".json");
            if (!std::filesystem::exists(path) || std::filesystem::file_size(path) > 1024 * 1024)
                throw std::runtime_error("Missing or oversized preset");
            std::ifstream in(path);
            std::string bytes((std::istreambuf_iterator<char>(in)), {}), error;
            if (!ParseScene(bytes, next, error))
                throw std::runtime_error(error);
            if (!next.map.empty() && next.map != CurrentMap())
                throw std::runtime_error("This scene belongs to " + next.map +
                                         ". Load that map before loading this preset.");
        } else if (op == "weather-center") {
            required(3);
            if (!g_cameraValid)
                throw std::runtime_error("Waiting for a rendered camera");
            next.weather.center = g_camera;
            next.weather.follow = false;
            next.map = CurrentMap();
        } else if (op == "weather") {
            auto mode = arg(3);
            auto &w = next.weather;
            if (mode == "wind") {
                required(6);
                float speed=Number(args->ArgV(4)),direction=Number(args->ArgV(5));
                if(speed<0||speed>500||direction<0||direction>360)throw std::runtime_error("Wind speed is 0..500 and direction 0..360");
                w.windX=speed*std::cos(Radians(direction));w.windY=speed*std::sin(Radians(direction));
            } else if (mode == "set") {
                required(6);
                std::string key = arg(4);
                float n = Number(args->ArgV(5));
                if (key == "type" || key == "count" || key == "seed") {
                    if (std::floor(n) != n || n < 0 || n > 65535)
                        throw std::runtime_error("Expected a bounded whole number");
                    if (key == "type") {
                        w.type = int(n);
                        next.held.erase(HeldWeatherTypeKey());
                    }
                    else if (key == "count")
                        w.count = int(n);
                    else
                        w.seed = int(n);
                } else if(key=="shelter"&&(n==0||n==1))
                    w.shelter=n!=0;
                else if (key == "radius")
                    w.radius = n;
                else if (key == "height")
                    w.height = n;
                else if (key == "speed")
                    w.speed = n;
                else if (key == "size")
                    w.size = n;
                else if (key == "windX")
                    w.windX = n;
                else if (key == "windY")
                    w.windY = n;
                else if (key == "follow" && (n == 0 || n == 1)) {
                    if (n == 0 && w.follow) {
                        if (CurrentMap().empty() || !g_cameraValid)
                            throw std::runtime_error("Load a map and wait for a rendered camera first");
                        if (!next.map.empty() && next.map != CurrentMap())
                            throw std::runtime_error("Scene belongs to another map. Reset or bind it first.");
                        w.center = g_camera;
                        next.map = CurrentMap();
                    }
                    w.follow = n != 0;
                } else if (key == "x" || key == "y" || key == "z")
                    SetVector(w.center, key, n);
                else if (key == "r" || key == "g" || key == "b")
                    SetVector(w.color, key, n);
                else
                    throw std::runtime_error("Unknown weather field");
            } else {
                required(4);
                next.held.erase(HeldWeatherTypeKey());
                if (mode == "clear") {
                    w.type = 0;
                } else if (mode == "rain" || mode == "storm") {
                    w.type = 1;
                    w.speed = mode == "storm" ? 1050.f : 700.f;
                    w.count = mode == "storm" ? 4500 : 2400;
                    w.size = .8f;
                    w.windX = mode == "storm" ? 180.f : 40.f;
                    w.color = {.6f, .7f, .85f};
                } else if (mode == "snow") {
                    w.type = 2;
                    w.speed = 65;
                    w.count = 4200;
                    w.size = 1.9f;
                    w.windX = 18;
                    w.color = {.9f, .95f, 1};
                } else if (mode == "ash") {
                    w.type = 3;
                    w.speed = 24;
                    w.count = 4200;
                    w.size = 1.35f;
                    w.windX = 28;
                    w.color = {.62f, .61f, .6f};
                } else if (mode == "embers" || mode == "motes" || mode == "smoke") {
                    w.count = mode == "smoke" ? 450 : 1200;
                    w.type = mode == "embers" ? 4 : mode == "motes" ? 5 : 6;
                    w.speed = mode == "embers" ? 35 : 8;
                    w.size = mode == "smoke" ? 12 : 2;
                    w.color = mode == "embers" ? Vec3{1, .25f, .04f} : Vec3{.8f, .85f, .9f};
                } else
                    throw std::runtime_error("Unknown weather preset");
                next.enabled = true;
            }
        } else if (op == "climate") {
            required(5);
            std::string key = arg(3);
            double n = Number(args->ArgV(4));
            int field = -1;
            for (int i = 0; i < ClimateCount; ++i)
                if (key == ClimateSpecs()[i].key)
                    field = i;
            if (!ClimateValueValid(field, n))
                throw std::runtime_error("Unknown or out-of-range climate value");
            next.held.erase(key);
            if ((field == CL_surfaceRadius || field == CL_localFogRadius || field == CL_puddles ||
                 field == CL_worldGi || field == CL_storm) &&
                n > 0) {
                if (!next.map.empty() && next.map != CurrentMap())
                    throw std::runtime_error("Scene belongs to another map. Reset or bind it first.");
                next.map = CurrentMap();
            }
            if ((field == CL_gi || field == CL_ssr) && n != 0)
                throw std::runtime_error("Screen-space GI and reflections are not available. Use world-space "
                                         "shadows in Lighting");
            if (field == CL_worldGi && n == 1 && g_cameraValid && next.climate[CL_bounceX] == 0 &&
                next.climate[CL_bounceY] == 0 && next.climate[CL_bounceZ] == 0) {
                next.climate[CL_bounceX] = g_camera.x;
                next.climate[CL_bounceY] = g_camera.y;
                next.climate[CL_bounceZ] = g_camera.z;
            }
            if(field==CL_storm && n>0 && next.climate[CL_storm]==0 && g_cameraValid) {
                next.climate[CL_stormX]=g_camera.x;next.climate[CL_stormY]=g_camera.y;next.climate[CL_stormZ]=g_camera.z-64;
            }
            next.climate.values[field] = float(n);
            // Sun placement is one creative operation. It always drives the sky, foreground map,
            // props and preserved 3D skybox together instead of requiring three hidden toggles.
            CoupleSunToLighting(next.climate, ClimateField(field));
            next.enabled = true;
        } else if (op == "surface-center" || op == "fog-center" || op == "gi-center" || op == "storm-center") {
            // DROP THIS VOLUME WHERE THE CAMERA IS, once. It then stays there: a placed volume that
            // followed the camera would defeat the reason for placing it.
            //
            // Placing is now a CHOICE, not a prerequisite. Radius 0 means global, so every coating
            // and the fog already do something before you come here; this narrows them to a spot.
            required(3);
            if (!g_cameraValid)
                throw std::runtime_error("Waiting for a rendered camera");
            if (!next.map.empty() && next.map != CurrentMap())
                throw std::runtime_error("Scene belongs to another map. Reset or bind it first.");
            auto &c = next.climate;
            if (op == "storm-center") {
                c[CL_stormX] = g_camera.x; c[CL_stormY] = g_camera.y; c[CL_stormZ] = g_camera.z - 64;
            } else if (op == "gi-center") {
                c[CL_bounceX] = g_camera.x;
                c[CL_bounceY] = g_camera.y;
                c[CL_bounceZ] = g_camera.z;
            } else if (op == "surface-center") {
                c[CL_surfaceX] = g_camera.x;
                c[CL_surfaceY] = g_camera.y;
                c[CL_surfaceRadius] = 600;
                c[CL_puddleHeight] = g_camera.z - 64;
            } else {
                c[CL_fogX] = g_camera.x;
                c[CL_fogY] = g_camera.y;
                c[CL_fogZ] = g_camera.z;
                c[CL_localFogRadius] = 600;
                c[CL_fogBase] = g_camera.z;
            }
            next.map = CurrentMap();
            next.enabled = true;
        } else if (op == "look") {
            required(4);
            auto name = arg(3);
            auto &c = next.climate;
            next.held.clear(); // a look is a whole new set of passes
            if (name == "clear")
                c = Climate{};
            else if (name == "storm" || name == "winter" || name == "sunset" || name == "night") {
                c = Climate{};
                next.weather = Weather{};
                c[CL_enabled] = 1;
                c[CL_relighting] = 1;
                c[CL_relight] = name == "night" ? .9f : name == "storm" ? .4f : .55f;
                c[CL_sky] = 1;
                c[CL_clouds] = 1;
                c[CL_sunRelight] = .45f;
                c[CL_sunElevation] = name == "night" ? -12 : name == "sunset" ? 8 : 25;
                c[CL_coverage] = name == "storm" ? .8f : name == "winter" ? .7f : .45f;
                c[CL_fogDensity] = name == "storm" ? .25f : .12f;
                c[CL_bloom] = .18f;
                if (name == "winter") {
                    c[CL_snow] = .8f;
                    next.weather.type = 2;
                    next.weather.speed = 65;
                    next.weather.size = 2;
                    next.weather.color = {.9f, .95f, 1};
                }
                if (name == "storm") {
                    c[CL_storm] = 1;
                    c[CL_density] = 1.3f; c[CL_cloudDepth] = 2200; c[CL_windSpeed] = 90;
                    c[CL_sunElevation] = -5; c[CL_sunPower] = .2f; c[CL_ambient] = .12f;
                    c[CL_cloudBase] = g_cameraValid ? g_camera.z + 1100 : 1500;
                    if (g_cameraValid) {
                        c[CL_stormX] = g_camera.x; c[CL_stormY] = g_camera.y; c[CL_stormZ] = g_camera.z - 64;
                        next.map = CurrentMap();
                    }
                    next.weather.count = 4500; next.weather.windX = 180; next.weather.windY = 60;
                    next.weather.shelter = true;
                    c[CL_wet] = .8f;
                    next.weather.type = 1;
                    next.weather.speed = 900;
                    next.weather.size = .8f;
                }
                if (name == "night") {
                    c[CL_ambient] = .08f;
                    c[CL_sunRelight] = .08f;
                }
            } else
                throw std::runtime_error("Use look clear|storm|winter|sunset|night");
            next.enabled = true;
        } else if (op == "set") {
            required(5);
            next.enabled = true;
            auto key = arg(3);
            float n = Number(args->ArgV(4));
            if (key == "exposure")
                next.exposure = n;
            else if (key == "ambient")
                next.ambient = n;
            else if (key == "fogR")
                next.fogColor.x = n;
            else if (key == "fogG")
                next.fogColor.y = n;
            else if (key == "fogB")
                next.fogColor.z = n;
            else if (key == "tintR")
                next.tint.x = n;
            else if (key == "tintG")
                next.tint.y = n;
            else if (key == "tintB")
                next.tint.z = n;
            else
                throw std::runtime_error("Unknown environment field");
        } else if (op == "add") {
            required(arg(3) == "obj" ? 5 : 4);
            if (CurrentMap().empty())
                throw std::runtime_error("Load a map first");
            if (!next.map.empty() && next.map != CurrentMap())
                throw std::runtime_error("Scene belongs to another map. Reset or explicitly bind it first.");
            Object o;
            o.id = NextId(next);
            o.kind = arg(3);
            // Puddles and snow are weather (Weather > Ground), not placed objects.
            if (o.kind=="snowdrift"||o.kind=="puddle"||o.kind=="flood")
                throw std::runtime_error("Puddles and snow are weather: use Weather > Ground");
            // The basic test shapes are no longer something to place: real models come from CS2
            // assets (Filmmaker/Props). The meshes stay for imported OBJ and the render tests.
            if (o.kind=="box"||o.kind=="beveledbox"||o.kind=="sphere"||o.kind=="cylinder")
                throw std::runtime_error("Basic shapes are not available. Place a model from Scene Objects > CS2 assets");
            o.asset = arg(4);
            if (o.IsLight()) {
                o.color = {1, .8f, .6f};
                o.scale = {1, 1, 1};
                if (o.kind == "area") o.cone = 150;
            }
            Place(o, 160.f);
            next.objects.push_back(o);
            next.map = CurrentMap();
            // Placed lights and meshes are drawn by the Environment pass, so adding one switches the
            // pass on. That is NOT a lighting takeover: a light adds its fill on top of whichever
            // lighting is running (CS2's own unless Lighting is switched on). Leaving the pass off
            // for lights is why a new light drew nothing until some other card happened to be on.
            next.enabled = true;
            nextSelected = o.id;
        } else if (op == "remove" || op == "duplicate" || op == "place" || op == "object" || op == "texture") {
            required(op == "object" || op == "texture" ? 6 : op == "place" ? 5 : 4);
            int id = Id(args->ArgV(3));
            auto it =
                std::find_if(next.objects.begin(), next.objects.end(), [&](auto &o) { return o.id == id; });
            if (it == next.objects.end())
                throw std::runtime_error("Object no longer exists");
            if (op == "remove") {
                next.objects.erase(it);
                nextSelected = 0;
            } else if (op == "duplicate") {
                Object copy = *it;
                copy.id = NextId(next);
                copy.position.x += 24;
                next.objects.push_back(copy);
                nextSelected = copy.id;
            } else if (op == "place")
                Place(*it, Number(args->ArgV(4)));
            else if(op=="texture"){
                if(it->IsLight())throw std::runtime_error("Textures belong to solid props");
                std::string name=arg(5)=="-"?"":arg(5);auto channel=arg(4);
                if(!name.empty()&&!ValidName(name))throw std::runtime_error("Invalid texture asset name");
                if(channel=="albedo")it->material.albedoTexture=name;
                else if(channel=="normal")it->material.normalTexture=name;
                else if(channel=="roughness")it->material.roughnessTexture=name;
                else throw std::runtime_error("Use texture ID albedo|normal|roughness name (or - to clear)");
                it->material.enabled=true;
            }
            else if (arg(4) == "group") { // object <id> group <name>|-  (outliner)
                std::string name = arg(5) == "-" ? "" : arg(5);
                if (name.size() > 40) throw std::runtime_error("Group names are at most 40 characters");
                it->group = name;
            }
            else if (arg(4) == "clip") { // object <id> clip <name>|-  (a GLB's animation; - = rest pose)
                std::string name;
                for (int i = 5; i < argc; ++i) name += (i > 5 ? " " : "") + arg(i); // clip names may have spaces
                if (name == "-") name.clear();
                if (name.size() > 128) throw std::runtime_error("Clip names are at most 128 characters");
                it->clip = name;
            }
            else {
                auto key = arg(4);
                float n = Number(args->ArgV(5));
                if (key == "x" || key == "y" || key == "z")
                    SetVector(it->position, key, n);
                else if (key == "r" || key == "g" || key == "b")
                    SetVector(it->color, key, n);
                else if (key == "pitch")
                    it->angles.x = n;
                else if (key == "yaw")
                    it->angles.y = n;
                else if (key == "roll")
                    it->angles.z = n;
                else if (key == "sx")
                    it->scale.x = n;
                else if (key == "sy")
                    it->scale.y = n;
                else if (key == "sz")
                    it->scale.z = n;
                else if (key == "range")
                    it->range = n;
                else if (key == "intensity")
                    it->intensity = n;
                else if (key == "cone")
                    it->cone = n;
                else if(!it->IsLight()&&key=="materialEnabled"&&(n==0||n==1))it->material.enabled=n!=0;
                else if(!it->IsLight()&&key=="cameraVisible"&&(n==0||n==1))it->cameraVisible=n!=0;
                else if(key=="render"&&(n==0||n==1))it->render=n!=0;
                else if(key=="locked"&&(n==0||n==1))it->locked=n!=0;
                else if(!it->IsLight()&&key=="clipSpeed"&&n>=-4&&n<=4)it->clipSpeed=n;
                else if(!it->IsLight()&&key=="roughness")it->material.roughness=n;
                else if(!it->IsLight()&&key=="metallic")it->material.metallic=n;
                else if(!it->IsLight()&&key=="coat")it->material.coat=n;
                else if(!it->IsLight()&&key=="coatRoughness")it->material.coatRoughness=n;
                else if(!it->IsLight()&&key=="anisotropy")it->material.anisotropy=n;
                else if(!it->IsLight()&&key=="emission")it->material.emission=n;
                else if(!it->IsLight()&&key=="normalStrength")it->material.normalStrength=n;
                else if(!it->IsLight()&&key=="uvScale")it->material.uvScale=n;
                else if (it->IsLight() && key == "width") it->studio.width=n;
                else if (it->IsLight() && key == "height") it->studio.height=n;
                else if (it->IsLight() && key == "sourceRadius") it->studio.sourceRadius=n;
                else if (it->IsLight() && key == "patternScale") it->studio.patternScale=n;
                else if (it->IsLight() && key == "rate") it->studio.rate=n;
                else if (it->IsLight() && key == "amount") it->studio.amount=n;
                else if (it->IsLight() && key == "phase") it->studio.phase=n;
                else if (it->IsLight() && key == "pattern" && n>=0 && n<=4 && n==std::floor(n)) it->studio.pattern=int(n);
                else if (it->IsLight() && key == "profile" && n>=0 && n<=3 && n==std::floor(n)) it->studio.profile=int(n);
                else if (it->IsLight() && key == "cue" && n>=0 && n<=4 && n==std::floor(n)) it->studio.cue=int(n);
                else if (key == "shadows" && it->IsLight() && (n == 0 || n == 1))
                    it->shadows = n != 0;
                else if (key == "enabled" && (n == 0 || n == 1))
                    it->enabled = n != 0;
                else
                    throw std::runtime_error("Unknown object field");
            }
        } else
            throw std::runtime_error("Unknown operation; use environment help");
        std::string error;
        Scene validated;
        if (!ParseScene(SerializeScene(next), validated, error) || !Resolve(validated, error))
            throw std::runtime_error(error);
        if ((op == "load" || op == "reset") && !PrepareWorldLook(validated.world, error))
            throw std::runtime_error(error);
        Remember();
        if (op == "load" || op == "reset") {
            ApplyWorldLook(validated.world, op == "reset" ? resetPage : "all");
            FilmmakerSettings_AcceptCurrent();
        }
        g_scene = std::move(validated);
        g_restoreAutosave = false;
        ++g_sceneGeneration;
        if (IsSceneMap(CurrentMap())) {
            Scene committed = LiveScene(); committed.map = CurrentMap();
            if (g_autosave.map != committed.map) { FlushAutosave(true); BindAutosave(committed.map); }
            g_autosave.Observe(SerializeScene(committed), GetTickCount64());
        }
        g_selected = nextSelected;
        if (op == "load") {
            g_savedName = arg(3);
            g_savedJson = SerializeScene(LiveScene());
            g_selected = 0;
        }
        g_message = op == "reset"  ? "Restored map defaults for " + resetPage + ". Undo is available."
                    : op == "load" ? "Loaded " + arg(3)
                                   : "Scene updated.";
        MvmDebugLog_LinefAlways("environment.edit", "op=%s objects=%zu weather=%d enabled=%d map=%s",
                                op.c_str(), g_scene.objects.size(), g_scene.weather.type, g_scene.enabled,
                                g_scene.map.c_str());
    } catch (const std::exception &e) {
        g_message = e.what();
        advancedfx::Warning("environment: %s\n", e.what());
        MvmDebugLog_LinefAlways("environment.error", "%s", e.what());
    }
}
} // namespace Filmmaker::Environment
