#pragma once
#include "EnvironmentMath.h"
#include "EnvironmentStudioMath.h"
#include "EnvironmentMaterialMath.h"
#include "EnvironmentShadowMath.h"
#include "EnvironmentWorld.h"
#include "EnvironmentClimateMath.h"
#include "EnvironmentLightingProofMath.h"
#include "../Relight/RelightMath.h"
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
namespace advancedfx {
class ICommandArgs;
}
namespace Filmmaker::Environment {
struct Vertex {
    Vec3 position, normal;
    float u=0,v=0;
};
// A run of a mesh's vertices drawn with its own embedded material (a GLB primitive): textures and
// factors as the file authored them. A material set on the placed object overrides every part.
struct MeshPart {
    unsigned first = 0, count = 0;
    Vec3 color{1, 1, 1};
    float roughness = .5f, metallic = 0;
    std::shared_ptr<const MaterialImage> albedo, normal, roughnessMap;
};
struct GltfModel;
struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<MeshPart> parts;          // empty: one draw with the object's material (OBJ, primitives)
    std::shared_ptr<const GltfModel> rig; // a GLB/glTF: its skeleton and clips (posed per frame)
};
struct Object {
    int id = 0;
    std::string kind = "box", asset;
    bool enabled = true, shadows = false;
    Vec3 position, angles, scale{32, 32, 32}, color{.65f, .65f, .65f};
    float intensity = 1, range = 400, cone = 45;
    float shadowNearClip = 1; // Runtime projection for a finite authored emitter.
    StudioLight studio;
    SurfaceMaterial material;
    bool cameraVisible=true;
    // Scene Objects outliner: the group it is filed under ("" = none), whether it is kept out of
    // renders (still shown in the live view), and whether the outliner locks it against edits.
    std::string group;
    bool render = true, locked = false;
    // A GLB's animation: the clip it plays ("" = the rest pose) and how fast (1 = as authored).
    std::string clip;
    float clipSpeed = 1;
    std::shared_ptr<const Mesh> mesh;
    bool IsLight() const { return kind == "point" || kind == "spot" || kind == "area"; }
};
struct Weather {
    int type = 0, count = 1800, seed = 7;
    float radius = 900, height = 900, speed = 600, size = 1, windX = 40, windY = 0;
    bool follow = true, shelter = true;
    Vec3 center, color{.65f, .75f, .9f};
};
struct Scene {
    bool enabled = false;
    std::string map;
    Weather weather;
    Climate climate;
    WorldLook world = DefaultWorldLook();
    float exposure = 1, ambient = .6f;
    Vec3 fogColor{.45f, .5f, .6f}, tint{1, 1, 1};
    std::vector<Object> objects;
    // Passes switched off by a restore, with the value each had (HoldRestoredPasses). Climate keys
    // plus "weatherType". An explicit edit of a key drops it; switching its card on reads it.
    std::map<std::string, float> held;
};
// A player the world coatings and rain impacts skip. The shader tests pixels against this pawn
// origin and radius (CoatingActor in EnvironmentClimateShaders.h).
struct CoatingActor {
    Vec3 origin;      // pawn origin (feet), world space
    float radius = 0; // horizontal reach; the vertical extent is derived in the shader
};
// A detonated CS2 smoke, as a sphere. Only its position ALONG the view ray is used (in front of or
// behind an addition); how much it hides comes from CS2's own OIT absorbance.
struct SmokeVolume {
    Vec3 center;
    float radius = 0;
};
struct Frame {
    LightingProof proof;
    Relight::Scene relight;
    uint64_t generation = 0;
    Scene scene;
    static const int kMaxSmokes = 8; // must match SMOKE_VOLUMES in the shader
    std::vector<SmokeVolume> smokes;
    // Bounded on purpose: this rides the frame constants, and a match is 10 players.
    static const int kMaxActors = 12;
    std::vector<CoatingActor> actors;
    std::shared_ptr<const OcclusionMesh> occlusion;
    Vec3 camera, angles;
    Matrix projection{};
    Matrix worldToClip{};
    bool hasWorldToClip = false;
    double time = 0;
    bool active = false;
    bool hdr = false, exporting = false;
    bool nativeLightingPass = false, nativeLightingApplied = false;
    bool nativeSkyApplied = false;
    bool nativeCompressedHdr = false;
    // Relight::DaylightEnergyRatio of the Environment lighting over the map's authored
    // lighting; negative when the Environment does not own the lighting this frame.
    float bakedDaylight = -1;
};
// Engine-free validation/serialization, shared with the native tests.
void ResetScenePage(Scene &scene, const std::string &page);
bool ValidName(const std::string &name);
bool ParseScene(const std::string &json, Scene &out, std::string &error);
std::string SerializeScene(const Scene &scene);
bool LoadObj(std::istream &stream, Mesh &mesh, std::string &error);
// A .glb / .gltf: its triangles, one part per primitive with its own textures, and its rig.
bool LoadGltfMesh(const std::filesystem::path &path, Mesh &mesh, std::string &error);
std::shared_ptr<const Mesh> Primitive(const std::string &kind);

void OnLevelInit();
// The short name of the level the engine is running, for features that key off it.
std::string CurrentMapName();
// Immutable validated map physics for independent visual volume growth.
std::shared_ptr<const OcclusionMesh> CollisionGeometrySnapshot();
void FlushCommittedEdits();
// Whether the Environment scene is switched on (CONFIG > ENVIRONMENT, `environment on`). Its
// lights, fog, sky and the scene keys' env.* channels only render while it is.
bool SceneEnabled();
// A practice session (Clippy's tutorial, Config/PracticeSessionFlag.h). Begin writes the user's
// pending edits; during the session no map gets its saved scene and nothing is saved. End drops
// the session's scene and restores the current map's saved one, as on a level load.
void BeginPractice();
void EndPractice();
std::map<std::string, std::vector<double>> CaptureAnimationValues();
void RunMainThreadFrame();
bool PositionEditing();
// The last rendered camera: world-to-clip of the full frame and the eye. False until a frame
// has rendered. Other in-world editors (Props) project their overlays with it.
bool RenderedCamera(Matrix &worldToClip, Vec3 &eye);
void StopPositionEditing();
void RunCommand(advancedfx::ICommandArgs *args);
// The Environment scene's undo history (lights, meshes, sky and the rest of the scene): the
// SceneUndoClock stamp of its newest step (0 when empty), and taking that step back.
uint64_t LastUndoStamp();
bool UndoLastEdit(std::string &error);
void AppendStateJson(std::ostringstream &out);
// Clicking in the view (Filmmaker/Props pick ranks these with the CS2 assets): the box of every
// placed mesh, and a small handle box around every light, in world space. Enabled, unlocked
// objects of the current map only.
struct PickBox {
    int id = 0;
    Vec3 corners[8];
    Vec3 center;
    bool light = false;      // drawn and clicked as a fixed-size dot at `center`
    std::string kind, name;  // "point"/"spot"/"area"/"mesh"...; what the hover label shows
    Vec3 color{1, 1, 1};
    bool selected = false;
};
std::vector<PickBox> ScenePickBoxes();
// A click picked object `id`: select it and arm its move gizmo at once (what Edit position did).
// 0 = the click picked something else or nothing: stop editing.
void SelectInView(int id);
Frame CaptureFrame(const Matrix &projection, const Matrix &worldToClip);
void SetRenderStatus(const char *status, bool drawn = false);
} // namespace Filmmaker::Environment
