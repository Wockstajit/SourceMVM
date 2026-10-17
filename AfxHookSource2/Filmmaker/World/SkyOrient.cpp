#include "WorldTransient.h"
#include "SkyOrient.h"

#include "SkyGpuProbe.h"
#include "SunRotationMath.h"

#include "WorldEntities.h"
#include "WorldSchema.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_Linef
#include "../../ClientEntitySystem.h"
#include "../../SceneSystem.h"             // new_ForceUpdateSkybox
#include "../../../Shared/AfxConsole.h"

#include <Windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace Filmmaker {

namespace {

// Both spellings, same reason as everywhere else in this module set: the sky entity reports
// className "env_sky" with clientClassName "C_BaseModelEntity", so matching only the leaf client
// class finds nothing (that bug is what made mirv_sky silently dead before this work).
const char* const kSkyClasses[] = { "env_sky", "C_EnvSky" };
// Cubemap fog has its own scene node on the maps that use it.  It must receive the same
// canonical sky delta or the horizon and fog/smoke volume rotate independently.
const char* const kFogClasses[] = {
	"env_cubemap_fog", "C_EnvCubemapFog",
	"env_gradient_fog", "gradient_fog", "C_GradientFog",
};

float g_yaw = 0.0f;
float g_pitch = 0.0f;
bool  g_mirror = false;
bool  g_rotateSky = true;
bool  g_rotateFog = true;
int   g_skyCount = 0;
int   g_fogCount = 0;
unsigned long long g_lastLogMs = 0;

struct Snapshot {
	float rot[3];       // pitch, yaw, roll as the engine stores QAngle
	float absRot[3];
	float scale;
	bool  taken = false;
	// What we wrote last frame. Read back at the top of the next frame to answer the ONE question
	// that separates the two possible failures: does the engine revert our write (so we are
	// fighting whatever recomputes the transform), or does it persist and the renderer simply
	// never consults it (the post-processing-volume dead end all over again)?
	float wrote[3] = { 0.0f, 0.0f, 0.0f };
	bool  wroteValid = false;
	// The ORIENTATION QUATERNION out of m_nodeToWorld -- the value the renderer actually samples.
	float quat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	bool  quatTaken = false;
};

// MEASURED 2026-08-04 (mvm_debug channel world.skyorient, probe output in Docs/effects/skybox.md):
//
//   m_nodeToWorld[0..3] = position (x, y, z, 1)
//   m_nodeToWorld[4..7] = orientation quaternion (x, y, z, w)
//
// Confirmed against Mirage's second sky entity, whose m_angRotation yaw was 146.2 and whose
// quaternion read (0, 0, 0.9569, 0.2903) -- exactly sin(146.2/2), cos(146.2/2).
constexpr int kN2W_QuatFloatOffset = 4;

// Bounded, opt-in probe. `world skyorient probe <frames>` arms it; it disarms itself. Never on by
// default -- this logs several lines per sky entity per frame and MvmDebugLog flushes per line.
int g_probeFrames = 0;
// Keyed by entity index. Restoring writes back exactly what the MAP had, never a previous
// override -- the reason the snapshot is taken once and never refreshed while active.
std::map<std::pair<CEntityInstance*,int>, Snapshot> g_snapshots;

bool AnyOverride() { return g_yaw != 0.0f || g_pitch != 0.0f || g_mirror; }

SunRotationMath::Quat ReadQuat(const float* values) {
	return values ? SunRotationMath::Normalize({ values[0], values[1], values[2], values[3] })
		: SunRotationMath::IdentityQuat();
}

void WriteQuat(float* values, SunRotationMath::Quat q) {
	if (!values) return;
	q = SunRotationMath::Normalize(q);
	values[0] = q.x; values[1] = q.y; values[2] = q.z; values[3] = q.w;
}

// The fog resource uses the same Source world basis as env_sky on the current build.  Keep this
// as an explicit consumer correction (rather than a second user-facing slider) so a future build
// with the known cubemap sign/axis convention can change one constant without splitting state.
const SunRotationMath::Quat kFogConsumerCorrection = SunRotationMath::IdentityQuat();

SunRotationMath::Quat FogDelta(SunRotationMath::Quat delta) {
	// C * delta * C^-1 is the per-consumer basis conversion.  Identity is the measured Mirage/Nuke
	// value today; the named function makes the one-source-of-truth contract explicit.
	const SunRotationMath::Quat c = kFogConsumerCorrection;
	const SunRotationMath::Quat inverse{ -c.x, -c.y, -c.z, c.w };
	return SunRotationMath::Multiply(SunRotationMath::Multiply(c, delta), inverse);
}

// Walks entity -> m_CBodyComponent -> m_pSceneNode. Returns nullptr if any link is missing, which
// is normal for entities that have no body component at all.
unsigned char* SceneNodeOf(CEntityInstance* ent) {
	const std::ptrdiff_t offBody = WorldSchema_Offset(kWF_Ent_BodyComponent);
	const std::ptrdiff_t offNode = WorldSchema_Offset(kWF_Body_SceneNode);
	if (!ent || 0 == offBody || 0 == offNode) return nullptr;

	unsigned char* body = *(unsigned char**)((unsigned char*)ent + offBody);
	if (!body) return nullptr;
	return *(unsigned char**)(body + offNode);
}

void WriteAngles(unsigned char* node, std::ptrdiff_t off, float pitch, float yaw, float roll) {
	if (0 == off) return;
	float* a = (float*)(node + off);
	a[0] = pitch; a[1] = yaw; a[2] = roll;
}

} // namespace

bool SkyOrient_Available(std::string* outReason) {
	struct Req { WorldField f; const char* name; };
	const Req required[] = {
		{ kWF_Ent_BodyComponent, "C_BaseEntity::m_CBodyComponent" },
		{ kWF_Body_SceneNode,    "CBodyComponent::m_pSceneNode" },
		{ kWF_Node_Rotation,     "CGameSceneNode::m_angRotation" },
	};
	for (const Req& r : required) {
		if (0 == WorldSchema_Offset(r.f)) {
			if (outReason) *outReason = r.name;
			return false;
		}
	}
	return true;
}

void SkyOrient_SetYaw(float degrees) {
	// Wrap rather than clamp: yaw is circular, and a slider that sticks at 360 feels broken.
	// In one step: a float wrap that walks 360 at a time never finishes on a huge value.
	degrees = std::isfinite(degrees) ? std::fmod(degrees, 360.0f) : 0.0f;
	if (degrees < 0.0f) degrees += 360.0f;
	if (degrees >= 360.0f) degrees = 0.0f;
	g_yaw = degrees;
}

void SkyOrient_SetPitch(float degrees) {
	if (degrees < -180.0f) degrees = -180.0f;
	if (degrees > 180.0f) degrees = 180.0f;
	g_pitch = degrees;
}

void SkyOrient_SetMirror(bool mirrored) { g_mirror = mirrored; }
void SkyOrient_SetSkyEnabled(bool enabled) { g_rotateSky = enabled; }
void SkyOrient_SetFogEnabled(bool enabled) { g_rotateFog = enabled; }

float SkyOrient_Yaw()   { return g_yaw; }
float SkyOrient_Pitch() { return g_pitch; }
bool  SkyOrient_Mirror(){ return g_mirror; }
bool  SkyOrient_SkyEnabled() { return g_rotateSky; }
bool  SkyOrient_FogEnabled() { return g_rotateFog; }
int   SkyOrient_SkyCount() { return g_skyCount; }

void SkyOrient_OnLevelInit() { g_snapshots.clear();g_skyCount=g_fogCount=0; }

void SkyOrient_Reset() {
	g_yaw = 0.0f;
	g_pitch = 0.0f;
	g_mirror = false;
	// Reset is a complete restore, including the two independent consumers.  Leaving either
	// target disabled made the next drag look like a broken one-sided sky/fog rotation.
	g_rotateSky = true;
	g_rotateFog = true;
	// One more pass writes the snapshots back; RunFrame handles the restore because it is the
	// thing that knows how to find the entities.
	SkyOrient_RunFrame();
	g_snapshots.clear();
}

void SkyOrient_RunFrame() {
 const auto effective_g_yaw = WorldTransient::Number("skyDirection", g_yaw, 0);
 const auto effective_g_pitch = WorldTransient::Number("skyDirection", g_pitch, 1);
 const auto effective_g_mirror = WorldTransient::Number("skyMirror", g_mirror, 0);
 const auto effective_g_rotateSky = WorldTransient::Number("skyTargets", g_rotateSky, 0);
 const auto effective_g_rotateFog = WorldTransient::Number("skyTargets", g_rotateFog, 1);
	const bool active = effective_g_yaw != 0 || effective_g_pitch != 0 || effective_g_mirror != 0;
	if (!SkyOrient_Available(nullptr)) return;
	if (!active && g_snapshots.empty()) {
		// Nothing overridden and nothing to restore: no writes. The sky count is still kept
		// current (once a second), because the panel reads it to know a sky exists. Counting only
		// while a rotation was applied left it at 0 after a reset or a new map, and the Sky
		// Orientation page said "Waiting for a sky entity" on a loaded map.
		static unsigned long long s_lastCount = 0;
		const unsigned long long now = GetTickCount64();
		if (now - s_lastCount < 1000) return;
		s_lastCount = now;
		int skies = 0, fogs = 0;
		const int top = GetHighestEntityIndex();
		for (int i = 0; i <= top; ++i) {
			CEntityInstance* ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i);
			if (!ent || !SceneNodeOf(ent)) continue;
			if (WorldEntity_Matches(ent, kSkyClasses, sizeof(kSkyClasses) / sizeof(kSkyClasses[0]))) ++skies;
			else if (WorldEntity_Matches(ent, kFogClasses, sizeof(kFogClasses) / sizeof(kFogClasses[0]))) ++fogs;
		}
		g_skyCount = skies;
		g_fogCount = fogs;
		return;
	}

	const std::ptrdiff_t offRot = WorldSchema_Offset(kWF_Node_Rotation);
	const std::ptrdiff_t offAbs = WorldSchema_Offset(kWF_Node_AbsRotation);
	const std::ptrdiff_t offScale = WorldSchema_Offset(kWF_Node_Scale);
	const std::ptrdiff_t offN2W = WorldSchema_Offset(kWF_Node_NodeToWorld);

	int found = 0;
	int foundFog = 0;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i);
		if (!ent) continue;
		const bool isSky = WorldEntity_Matches(ent, kSkyClasses, sizeof(kSkyClasses) / sizeof(kSkyClasses[0]));
		const bool isFog = WorldEntity_Matches(ent, kFogClasses, sizeof(kFogClasses) / sizeof(kFogClasses[0]));
		if (!isSky && !isFog) continue;

		unsigned char* node = SceneNodeOf(ent);
		if (!node) continue;
		if (isSky) ++found;
		if (isFog) ++foundFog;

		Snapshot& snap = g_snapshots[std::make_pair(ent,ent->GetHandle().ToInt())];

		// ---- PROBE: read BEFORE writing, so this frame's values are last frame's aftermath ----
		if (g_probeFrames > 0) {
			const float* r = (const float*)(node + offRot);
			const float* a = offAbs ? (const float*)(node + offAbs) : nullptr;
			const char* dbg = ent->GetDebugName();
			const char* cls = ent->GetClassName();
			MvmDebugLog_Linef("world.skyorient",
				"probe ent=%d kind=%s name=%s class=%s node=%p rot=(%.1f %.1f %.1f) abs=(%.1f %.1f %.1f)%s",
				i, isSky ? "sky" : "fog", dbg ? dbg : "?", cls ? cls : "?", (void*)node,
				r[0], r[1], r[2],
				a ? a[0] : 0.0f, a ? a[1] : 0.0f, a ? a[2] : 0.0f,
				// THE decisive line: if this says REVERTED, the engine is recomputing the
				// transform after us and the fix is to write what it computes FROM, not the
				// angles. If it says persisted and the sky still has not moved, the renderer is
				// reading a different transform entirely.
				(snap.wroteValid
					? ((r[0] == snap.wrote[0] && r[1] == snap.wrote[1] && r[2] == snap.wrote[2])
						? "  [persisted]" : "  [REVERTED]")
					: ""));

			// m_nodeToWorld is the matrix the renderer actually samples. Dumped raw so its layout
			// can be identified (a unit-norm 4-tuple is the orientation quaternion).
			if (offN2W) {
				const float* n = (const float*)(node + offN2W);
				MvmDebugLog_Linef("world.skyorient",
					"probe ent=%d nodeToWorld[0..7]=%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
					i, n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]);
			}
		}

		if (!snap.taken) {
			const float* r = (const float*)(node + offRot);
			snap.rot[0] = r[0]; snap.rot[1] = r[1]; snap.rot[2] = r[2];
			if (offAbs) {
				const float* a = (const float*)(node + offAbs);
				snap.absRot[0] = a[0]; snap.absRot[1] = a[1]; snap.absRot[2] = a[2];
			}
			snap.scale = offScale ? *(const float*)(node + offScale) : 1.0f;
			snap.taken = true;
		}

		// THE ORIENTATION QUATERNION IN m_nodeToWorld IS THE ONE THAT MATTERS.
		//
		// Writing m_angRotation alone does NOTHING VISIBLE. Measured: the angle write persists
		// perfectly (nothing reverts it), but the engine never recomputes m_nodeToWorld from it,
		// and m_nodeToWorld is what the renderer samples -- so the sky keeps drawing at its
		// original angle. That is why the first version of this module looked correct in memory
		// and did nothing on screen.
		//
		// The angles are still written, so anything that reads them (and a later engine-side
		// recompute) stays consistent with what is on screen.
		float* n2w = offN2W ? (float*)(node + offN2W) : nullptr;
		if (n2w && !snap.quatTaken) {
			for (int c = 0; c < 4; ++c) snap.quat[c] = n2w[kN2W_QuatFloatOffset + c];
			snap.quatTaken = true;
		}

		const bool targetEnabled = (isSky && effective_g_rotateSky) || (isFog && effective_g_rotateFog);
		if (active && targetEnabled) {
			// The delta is the single source of truth.  Compose it with the map-authored quaternion,
			// then project only for legacy angle fields.  No Euler addition occurs at the poles.
			const SunRotationMath::Quat authored = ReadQuat(snap.quat);
			const SunRotationMath::Quat delta = SunRotationMath::FromYawPitch(effective_g_yaw, effective_g_pitch);
			const SunRotationMath::Quat consumerDelta = isFog ? FogDelta(delta) : delta;
			const SunRotationMath::Quat output = SunRotationMath::Multiply(consumerDelta, authored);
			float projected[3] = {};
			SunRotationMath::ToSourceAngles(output, projected);

			WriteAngles(node, offRot, projected[0], projected[1], projected[2]);
			if (offAbs)
				WriteAngles(node, offAbs, projected[0], projected[1], projected[2]);

			if (n2w) WriteQuat(n2w + kN2W_QuatFloatOffset, output);

			// Negative scale mirrors the sky cubemap sampling -- no second texture needed.  Fog
			// keeps its authored scale; only its orientation delta is shared.
			if (offScale && isSky)
				*(float*)(node + offScale) = effective_g_mirror ? -snap.scale : snap.scale;

			snap.wrote[0] = projected[0]; snap.wrote[1] = projected[1]; snap.wrote[2] = projected[2];
			snap.wroteValid = true;
		} else {
			WriteAngles(node, offRot, snap.rot[0], snap.rot[1], snap.rot[2]);
			if (offAbs) WriteAngles(node, offAbs, snap.absRot[0], snap.absRot[1], snap.absRot[2]);
			if (n2w && snap.quatTaken)
				for (int c = 0; c < 4; ++c) n2w[kN2W_QuatFloatOffset + c] = snap.quat[c];
			if (offScale) *(float*)(node + offScale) = snap.scale;
		}

		// The sky caches a resolved transform; without kicking it the write lands in memory and
		// nothing on screen changes. This is the same refresh the existing material swap uses.
		if (isSky) new_ForceUpdateSkybox(ent);
	}
	g_skyCount = found;
	g_fogCount = foundFog;
	if (g_probeFrames > 0) --g_probeFrames;

	// Throttled: this runs every frame, and MvmDebugLog flushes per line.
	const unsigned long long now = GetTickCount64();
	if (now - g_lastLogMs > 2000) {
		g_lastLogMs = now;
		MvmDebugLog_Linef("world.skyorient", "skies=%d fog=%d yaw=%.1f pitch=%.1f mirror=%d active=%d",
			found, foundFog, effective_g_yaw, effective_g_pitch, effective_g_mirror ? 1 : 0, active ? 1 : 0);
	}

	if (!active) g_snapshots.clear();
}

void SkyOrient_AppendStateJson(std::ostringstream& o) {
	o << ",\"skyYaw\":" << g_yaw;
	o << ",\"skyPitch\":" << g_pitch;
	o << ",\"skyMirror\":" << (g_mirror ? "true" : "false");
	o << ",\"skyMoveSky\":" << (g_rotateSky ? "true" : "false");
	o << ",\"skyMoveFog\":" << (g_rotateFog ? "true" : "false");
	o << ",\"skyEnts\":" << g_skyCount;
	o << ",\"skyFogEnts\":" << g_fogCount;
	o << ",\"skyOrientOk\":" << (SkyOrient_Available(nullptr) ? "true" : "false");
}

void SkyOrient_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// argv: mirv_filmmaker world skyorient <op> <value>
	const char* op = (argc >= 4) ? args->ArgV(3) : "";
	if (0 == _stricmp(op, "gputrace")) {
		const int n = argc >= 5 ? atoi(args->ArgV(4)) : 8;
		if (argc >= 6 && 0 == _stricmp(args->ArgV(5), "effects"))
			SkyGpuProbe_ArmEffects(n);
		else
			SkyGpuProbe_Arm(n);
		return;
	}

	std::string reason;
	if (!SkyOrient_Available(&reason) && 0 != _stricmp(op, "")) {
		advancedfx::Message(
			"Sky orientation unavailable on this CS2 build: %s did not resolve.\n"
			"Run `%s world health` after an update.\n", reason.c_str(), cmd);
		return;
	}

	const bool quiet = (argc >= 6) && (0 == _stricmp(args->ArgV(5), "quiet"));

	if (0 == _stricmp(op, "yaw") || 0 == _stricmp(op, "pitch")) {
		const bool isYaw = (0 == _stricmp(op, "yaw"));
		if (argc >= 5) {
			if (isYaw) SkyOrient_SetYaw((float)atof(args->ArgV(4)));
			else       SkyOrient_SetPitch((float)atof(args->ArgV(4)));
			if (!quiet)
				advancedfx::Message("Sky %s %.1f deg (%d sky entit%s).\n", op,
					isYaw ? g_yaw : g_pitch, g_skyCount, g_skyCount == 1 ? "y" : "ies");
			return;
		}
		advancedfx::Message("Sky %s is %.1f deg.\n", op, isYaw ? g_yaw : g_pitch);
		return;
	}

	if (0 == _stricmp(op, "mirror")) {
		if (argc >= 5) {
			SkyOrient_SetMirror(0 != atoi(args->ArgV(4)));
			if (!quiet) advancedfx::Message("Sky mirror %s.\n", g_mirror ? "on" : "off");
			return;
		}
		advancedfx::Message("Sky mirror is %s.\n", g_mirror ? "on" : "off");
		return;
	}

	if (0 == _stricmp(op, "target")) {
		const char* target = argc >= 5 ? args->ArgV(4) : "";
		if (argc >= 6) {
			const bool enabled = 0 != atoi(args->ArgV(5));
			if (0 == _stricmp(target, "sky")) SkyOrient_SetSkyEnabled(enabled);
			else if (0 == _stricmp(target, "fog")) SkyOrient_SetFogEnabled(enabled);
			else {
				advancedfx::Message("Sky orientation target must be sky or fog.\n");
				return;
			}
			const bool targetQuiet = argc >= 7 && 0 == _stricmp(args->ArgV(6), "quiet");
			if (!targetQuiet) advancedfx::Message("Sky orientation target %s %s.\n",
				target, enabled ? "enabled" : "disabled");
			return;
		}
		advancedfx::Message("Sky targets: sky=%s fog=%s.\n",
			g_rotateSky ? "on" : "off", g_rotateFog ? "on" : "off");
		return;
	}

	if (0 == _stricmp(op, "reset")) {
		SkyOrient_Reset();
		advancedfx::Message("Sky orientation restored to the map's own.\n");
		return;
	}

	if (0 == _stricmp(op, "probe")) {
		g_probeFrames = (argc >= 5) ? atoi(args->ArgV(4)) : 8;
		if (g_probeFrames < 1) g_probeFrames = 1;
		if (g_probeFrames > 240) g_probeFrames = 240;   // this logs per entity per frame
		advancedfx::Message(
			"Probing the next %d frame(s) into mvm_debug channel world.skyorient.\n"
			"Start mvm_debug FIRST, then set a yaw, then run this.\n", g_probeFrames);
		return;
	}

	advancedfx::Message(
		"mirv_filmmaker world skyorient - LIVE skybox orientation. No rebuild, no second copy.\n"
		"Usage:\n"
		"  %s world skyorient yaw <deg>     - live sky/cubemap yaw offset (sun decal stays put)\n"
		"  %s world skyorient pitch <deg>   - tilt it\n"
		"  %s world skyorient mirror 0|1    - flip left/right\n"
		"  %s world skyorient target sky 0|1 - include/exclude the actual sky draw\n"
		"  %s world skyorient target fog 0|1 - include/exclude fog and smoke alignment\n"
		"  %s world skyorient reset\n"
		"  %s world skyorient gputrace [n] [effects] - diagnose sky/effect draws (mvm_debug)\n"
		"Applies to the map's own sky and to any imported one alike through the live GPU draw path;\n"
		"the env_sky transform is also maintained for map entities and fog compatibility.\n"
		"Current: yaw %.1f, pitch %.1f, mirror %s, %d sky entit%s.\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, g_yaw, g_pitch, g_mirror ? "on" : "off",
		g_skyCount, g_skyCount == 1 ? "y" : "ies");
}

} // namespace Filmmaker
