#include "EasterEggWorld.h"

#include "EasterEggChickenMath.h"

#include "../Props/ClientEntitySpawn.h"
#include "../Environment/EnvironmentScene.h"
#include "../Environment/EnvironmentSurfaceMath.h"
#include "../Movie/CameraBridge.h"
#include "../Debug/MvmDebugLog.h"
#include "../../ClientEntitySystem.h"
#include "../../SchemaSystem.h"

#include <Windows.h>

#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

// CHICKEN STORM. Real CS2 chickens (client-side prop_dynamic, the same spawner the Props workspace
// uses -- it works in demos) rain from above the camera, tumble, bounce, sit a moment looking
// confused, and are removed. Each one is moved by writing its scene node's pose every frame: the
// interpolation sources (m_vecOrigin / m_angRotation), the absolute pose, AND the cached
// m_nodeToWorld, so CS2's lazy transform rebuild cannot snap it back (RagdollWeapon learned this
// with a hardware watchpoint). Where each bird lands is found ONCE, at spawn, by a ray straight
// down the map's collision -- no per-frame traces.
//
// PRECACHE: every model is loaded when the storm starts, before the first bird spawns, so the
// first drop does not hitch (repo rule).

namespace Filmmaker {
namespace {

using Environment::Vec3;
namespace EC = EasterEggChicken;

// Verified present in pak01_dir.vpk (models/chicken/*.vmdl_c). The roasted one is rare on purpose.
const char* const kModels[] = {
	"models/chicken/chicken.vmdl", "models/chicken/chicken.vmdl", "models/chicken/chicken.vmdl",
	"models/chicken/chicken_polish.vmdl", "models/chicken/chicken_silkie.vmdl",
	"models/chicken/chick.vmdl", "models/chicken/chicken_roasted.vmdl",
};
constexpr int kModelCount = (int)(sizeof(kModels) / sizeof(kModels[0]));
constexpr int kMaxBirds = 36;
constexpr double kSpawnEvery = 0.14;      // seconds between drops
constexpr double kRadius = 900.0;         // around the camera
constexpr double kDropHeightMin = 420.0, kDropHeightMax = 900.0; // above the camera

struct Chicken {
	unsigned char* entity = nullptr;
	EC::Bird bird;
	double x = 0.0, y = 0.0;
	double age = 0.0;
	bool physics = false; // the engine simulates it: no pose writes, removed by age
};

// Which entity class a chicken is spawned as. A client physics prop falls and lands on its own;
// prop_dynamic needs our pose writes. Switchable from the console while testing.
std::string g_class = "prop_physics";
constexpr double kPhysicsLife = 7.0;
bool IsPhysicsClass(const std::string& c) { return c.rfind("prop_physics", 0) == 0; }

bool g_on = false;
bool g_precached = false;
std::vector<Chicken> g_birds;
std::mt19937 g_rng{ 0xC41C4E4u };
unsigned long long g_lastMs = 0;
double g_spawnAccum = 0.0;
unsigned long long g_spawned = 0, g_failed = 0;

double Rand(double a, double b) { return std::uniform_real_distribution<double>(a, b)(g_rng); }

bool Precache() {
	bool ok = true;
	for (const char* m : kModels) ok = Props::PrecacheModel(m) && ok;
	MvmDebugLog_LinefAlways("egg.chickens", "precache %s", ok ? "ok" : "PARTIAL");
	return ok;
}

bool WritePose(unsigned char* entity, double x, double y, double z, double pitch, double yaw, double roll) {
	const auto& base = g_clientDllOffsets.C_BaseEntity;
	const auto& o = g_clientDllOffsets.CGameSceneNode;
	if (!base.m_pGameSceneNode || !o.m_vecOrigin || !o.m_angRotation || !o.m_nodeToWorld) return false;
	const float pos[3] = { (float)x, (float)y, (float)z };
	const float ang[3] = { (float)pitch, (float)yaw, (float)roll };
	float q[4];
	EC::AngleQuaternion(pitch, yaw, roll, q);
	__try {
		unsigned char* node = *(unsigned char**)(entity + base.m_pGameSceneNode);
		if ((uintptr_t)node < 0x10000) return false;
		std::memcpy(node + o.m_vecOrigin, pos, sizeof(pos));
		std::memcpy(node + o.m_angRotation, ang, sizeof(ang));
		if (o.m_vecAbsOrigin) std::memcpy(node + o.m_vecAbsOrigin, pos, sizeof(pos));
		if (o.m_angAbsRotation) std::memcpy(node + o.m_angAbsRotation, ang, sizeof(ang));
		// The lazy world-transform rebuild reads these, not m_vecOrigin: leaving them at the spawn
		// pose snaps the bird back to where it was created. A spawned prop has no parent, so local
		// IS world here.
		if (o.m_vecWrappedLocalOrigin) std::memcpy(node + o.m_vecWrappedLocalOrigin, pos, sizeof(pos));
		if (o.m_angWrappedLocalRotation) std::memcpy(node + o.m_angWrappedLocalRotation, ang, sizeof(ang));
		float* x4 = (float*)(node + o.m_nodeToWorld);
		x4[0] = pos[0]; x4[1] = pos[1]; x4[2] = pos[2];
		x4[4] = q[0]; x4[5] = q[1]; x4[6] = q[2]; x4[7] = q[3];
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Interpolation would blend every frame's pose toward the last networked one (there is none).
void DisableInterpolation(unsigned char* entity) {
	const ptrdiff_t off = g_clientDllOffsets.C_BaseModelEntity.m_bNoInterpolate;
	if (!off) return;
	__try { *(bool*)(entity + off) = true; }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Remove(Chicken& c) {
	if (c.entity && Props::ClientEntityIndex(c.entity) >= 0) Props::RemoveClientEntity(c.entity);
	c.entity = nullptr;
}

void SpawnOne() {
	double origin[3] = {}, angles[3] = {}, fov = 90.0;
	CameraBridge_GetCurrentCamera(origin, angles, fov);
	// Mostly in front of the camera, where the storm can actually be seen.
	const double yaw = angles[1] * 3.14159265358979 / 180.0 + Rand(-1.1, 1.1);
	const double dist = Rand(120.0, kRadius);
	Chicken c;
	c.x = origin[0] + std::cos(yaw) * dist;
	c.y = origin[1] + std::sin(yaw) * dist;
	c.bird.z = origin[2] + Rand(kDropHeightMin, kDropHeightMax);
	c.bird.vz = Rand(-80.0, 0.0);
	c.bird.groundZ = origin[2] - 64.0; // no collision cache yet: roughly where a player's feet are
	if (auto world = Environment::CollisionGeometrySnapshot()) {
		Environment::SurfaceHit hit;
		const Vec3 from{ (float)c.x, (float)c.y, (float)c.bird.z };
		if (Environment::SurfaceRaycast(*world, from, Vec3{ 0, 0, -1 }, 8192.0f, hit)) c.bird.groundZ = hit.position.z;
	}
	c.bird.yaw = Rand(-180.0, 180.0);
	c.bird.spinPitch = Rand(-420.0, 420.0);
	c.bird.spinRoll = Rand(-300.0, 300.0);
	c.bird.spinYaw = Rand(-200.0, 200.0);
	const char* model = kModels[std::uniform_int_distribution<int>(0, kModelCount - 1)(g_rng)];
	c.physics = IsPhysicsClass(g_class);
	c.entity = Props::SpawnClientEntity(g_class.c_str(), "mvm_egg_chicken", model,
		Vec3{ (float)c.x, (float)c.y, (float)c.bird.z }, Vec3{ 0.0f, (float)c.bird.yaw, 0.0f }, 1.0f);
	if (!c.entity) { ++g_failed; return; }
	++g_spawned;
	DisableInterpolation(c.entity);
	g_birds.push_back(c);
}

} // namespace

void EasterEggChickens_Set(bool on) {
	if (on == g_on) return;
	g_on = on;
	if (on) {
		if (!Props::ClientEntitySpawnAvailable()) {
			MvmDebugLog_LinefAlways("egg.chickens", "client entity spawn unavailable");
			return;
		}
		if (!g_precached) g_precached = Precache();
		g_spawnAccum = 0.0;
		g_lastMs = GetTickCount64();
	} else {
		// Birds already in the air finish their fall; only new drops stop.
	}
	MvmDebugLog_LinefAlways("egg.chickens", "%s live=%zu spawned=%llu failed=%llu", on ? "on" : "off",
		g_birds.size(), g_spawned, g_failed);
}

void EasterEggChickens_RunFrame() {
	if (!g_on && g_birds.empty()) return;
	const unsigned long long now = GetTickCount64();
	const double dt = g_lastMs ? (now - g_lastMs) / 1000.0 : 0.0;
	g_lastMs = now;

	if (g_on && Props::ClientEntitySpawnAvailable()) {
		if (!g_precached) g_precached = Precache();
		g_spawnAccum += dt;
		while (g_spawnAccum >= kSpawnEvery) {
			g_spawnAccum -= kSpawnEvery;
			if ((int)g_birds.size() < kMaxBirds) SpawnOne();
		}
	}

	for (size_t i = 0; i < g_birds.size();) {
		Chicken& c = g_birds[i];
		c.age += dt;
		const bool alive = c.physics ? c.age < kPhysicsLife : EC::Step(c.bird, dt);
		if (!alive || !c.entity || Props::ClientEntityIndex(c.entity) < 0) {
			Remove(c);
			g_birds.erase(g_birds.begin() + i);
			continue;
		}
		++i;
	}
	if (MvmDebugLog_Active()) {
		static unsigned long long s_last = 0;
		if (now - s_last > 2000) {
			s_last = now;
			MvmDebugLog_Linef("egg.chickens", "live=%zu spawned=%llu failed=%llu", g_birds.size(), g_spawned, g_failed);
		}
	}
}

void EasterEggChickens_RunLateFrame() {
	// After the client's FRAME_RENDER_PASS transform work: the last write before the draw.
	for (Chicken& c : g_birds)
		if (c.entity && !c.physics)
			WritePose(c.entity, c.x, c.y, c.bird.z, EC::WrapDegrees(c.bird.pitch), EC::WrapDegrees(c.bird.yaw),
				EC::WrapDegrees(c.bird.roll));
}

void EasterEggChickens_LevelInit() {
	// The level that owned these entities is gone and so are they. Forget, never touch. The main
	// pump consumes the queued egg cancellation before another bird can spawn.
	g_birds.clear();
	g_precached = false;
}

int EasterEggChickens_Count() { return (int)g_birds.size(); }

void EasterEggChickens_SetClass(const char* cls) {
	if (cls && *cls) g_class = cls;
	MvmDebugLog_LinefAlways("egg.chickens", "spawn class=%s", g_class.c_str());
}
const char* EasterEggChickens_Class() { return g_class.c_str(); }

} // namespace Filmmaker
