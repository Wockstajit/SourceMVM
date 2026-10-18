#define NOMINMAX
#include "SceneRig.h"

#include "SceneRigMath.h"
#include "SceneRigTransient.h"
#include "SceneKeys.h"
#include "SceneKeyframeTrackMath.h"
#include "TimelinePlayhead.h"
#include "FollowTargetProviders.h"
#include "CameraBridge.h"
#include "CameraPath.h"   // a playing camera path hides the overlay, as it hides its own markers
#include "HudBlackout.h"  // F8 hides the overlay with the rest of the screen
#include "DemoScrub.h"    // DemoScrub_Release: Go to camera and Look through land on its tick
#include "../../SceneOverlayData.h"
#include "../../SceneMarkerMath.h" // one colour per kind, shared with the drawer
#include "../Environment/EnvironmentScene.h"
#include "../Debug/MvmDebugLog.h"
#include "../Platform/JsonBuilder.h"
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "../../../Shared/AfxConsole.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

// Scene object rigs (Docs/features/scene-object-rigs.md). Each frame, after the scene keys:
//   1. every placed object of the loaded map gets its local pose: its pose keys sampled at the
//      applied tick (hold by default), else its rest pose (the attach offset when it rides
//      something, the map's authored pose when it does not);
//   2. players, weapons and grenades it rides are looked up by identity and read through the
//      Follow Camera's mount resolver (ResolveAttachTransform), chains through other objects
//      recurse (SceneRigMath.h);
//   3. the world pose of every rigged object is published for the Environment (SceneRigTransient).
// At view setup the entity parents found in step 2 are read again, interpolated, and step 3 is
// repeated, so a light on a head does not step at the tick rate while the head moves smoothly.

namespace Filmmaker {
namespace {
using namespace SceneKeys;
using Rig::Pose;

// An entity parent as found this frame: what to read again at view setup.
struct ParentRead {
	bool found = false;
	int entityIndex = -1;
	uint64_t handle = 0;
	FollowTargetType type = FollowTargetType::Player;
	std::string mount, source, who;
	bool isWeapon = false, isBomb = false;
	Rig::ExternalPose pose;
};

struct Live {
	Environment::ObjectSummary info;
	bool attached = false, keyed = false, armed = false;
	int keys = 0;
	Rig::Attachment att;
	double local[Rig::kPoseChannels] = {};
	double rest[Rig::kPoseChannels] = {};
	ParentRead parent;
	FollowPlayerPose playerPose;
	Rig::Resolved resolved;
	Rig::HoldState hold;
	bool visible = true;
	Pose world;
	// A light's settings (Rig::LightChannel order): their rest values, this frame's values, and
	// which of them are keyed (bit per channel).
	double lightRest[Rig::kLightChannels] = {};
	double light[Rig::kLightChannels] = {};
	unsigned lightKeyed = 0;
	bool Camera() const { return info.kind == "camera"; }
	bool Animated() const { return keyed || armed; }
	bool Rigged() const { return attached || keyed; }
	int Channels() const { return Camera() ? Rig::kPoseChannels : Rig::Fov; }
};

std::map<int, Live> g_live;
int g_lookThrough = 0;
// A slider drag sends a stream of `rig set`: the edits that follow each other closely are one undo
// step, closed 600 ms after the last (the Environment's Remember() rule for its own sliders).
bool g_sliderGesture = false;
ULONGLONG g_sliderEditAt = 0;
void SliderEdit() {
	if (!g_sliderGesture) { SceneKeys_BeginGesture(); g_sliderGesture = true; }
	g_sliderEditAt = GetTickCount64();
}
std::string g_message;
unsigned g_frame = 0;

struct PlayerRow { int pawn = -1; uint64_t steamId = 0; std::string name; int team = 0; bool alive = false; };
struct GrenadeRow { int index = -1; uint64_t handle = 0; std::string type, thrower; int throwTick = -1; };
struct DroppedRow { int index = -1; std::string cls, name; };
std::vector<PlayerRow> g_players;
std::vector<GrenadeRow> g_grenades;
std::vector<DroppedRow> g_dropped;
unsigned g_listsFrame = 0, g_grenadeScanFrame = 0;

bool EntitiesReady() { return g_pEntityList && *g_pEntityList && g_GetEntityFromIndex; }

std::string ClassOf(CEntityInstance* e) {
	const std::string designer = DesignerClass(e);
	return designer.empty() ? EntityClass(e) : designer;
}

std::string ControllerName(CEntityInstance* pawnOrController) {
	if (!pawnOrController) return "";
	CEntityInstance* controller = pawnOrController;
	if (pawnOrController->IsPlayerPawn()) {
		const auto h = pawnOrController->GetPlayerControllerHandle();
		controller = h.IsValid() ? EntityAt(h.GetEntryIndex()) : nullptr;
	}
	if (!controller || !controller->IsPlayerController()) return "";
	const char* name = controller->GetPlayerName();
	return name ? name : "";
}


void ScanPlayers() {
	g_players.clear();
	if (!EntitiesReady()) return;
	for (int i = 0, n = GetHighestEntityIndex(); i <= n; ++i) {
		CEntityInstance* controller = EntityAt(i);
		if (!controller || !controller->IsPlayerController()) continue;
		const auto pawnHandle = controller->GetPlayerPawnHandle();
		if (!pawnHandle.IsValid()) continue;
		CEntityInstance* pawn = EntityAt(pawnHandle.GetEntryIndex());
		if (!pawn || !pawn->IsPlayerPawn()) continue;
		PlayerRow row;
		row.pawn = pawnHandle.GetEntryIndex();
		row.steamId = controller->GetSteamId();
		const char* name = controller->GetPlayerName();
		row.name = name ? name : "";
		row.team = pawn->GetTeam();
		row.alive = pawn->GetHealth() > 0;
		g_players.push_back(std::move(row));
	}
}

// Thrown grenades (the shared index the Follow Camera reads too, FollowTargetProviders.h: the
// ones in the air now with their entity, and the ones seen earlier this session with index -1),
// and dropped weapons alive this tick.
void ScanWorld(bool dropped) {
	g_grenades.clear();
	if (dropped) g_dropped.clear();
	if (!EntitiesReady()) return;
	int currentTick = 0;
	g_MirvTime.GetCurrentDemoTick(currentTick);
	for (const GrenadeSighting& s : GrenadeIndex_Update()) {
		if (!s.active && std::abs(s.throwTick - currentTick) > kGrenadeBrowseTicks) continue;
		GrenadeRow row;
		row.index = s.active ? s.entityIndex : -1;
		row.handle = s.handle;
		row.type = s.type;
		row.thrower = s.thrower;
		row.throwTick = s.throwTick;
		g_grenades.push_back(std::move(row));
	}
	std::sort(g_grenades.begin(), g_grenades.end(), [&](const GrenadeRow& a, const GrenadeRow& b) {
		if ((a.index >= 0) != (b.index >= 0)) return a.index >= 0; // in the air now first
		return std::abs(a.throwTick - currentTick) < std::abs(b.throwTick - currentTick);
	});
	if (!dropped) return;
	for (int i = 0, n = GetHighestEntityIndex(); i <= n; ++i) {
		CEntityInstance* e = EntityAt(i);
		if (!e) continue;
		const std::string weapon = ClassOf(e);
		if (IsWeaponOrBomb(weapon) && !e->GetOwnerEntityHandle().IsValid()) {
			DroppedRow row;
			row.index = i;
			row.cls = weapon;
			row.name = IsBombClass(weapon) ? "C4" : PrettyWeaponName(weapon);
			g_dropped.push_back(std::move(row));
		}
	}
}

const PlayerRow* FindPlayer(const Rig::Attachment& a) {
	if (a.steamId) for (const auto& p : g_players) if (p.steamId == a.steamId) return &p;
	for (const auto& p : g_players) if (!a.playerName.empty() && p.name == a.playerName) return &p;
	return nullptr;
}

const GrenadeRow* FindGrenade(const Rig::Attachment& a) {
	const GrenadeRow* best = nullptr;
	for (const auto& g : g_grenades)
		if (a.grenadeHandle && g.index >= 0 && g.handle == a.grenadeHandle) return &g;
	for (const auto& g : g_grenades) {
		if (g.index < 0) continue; // seen earlier, not in the world now
		if (g.type != a.grenadeType || std::abs(g.throwTick - a.throwTick) > Rig::kGrenadeTickSlack) continue;
		if (!best || (g.thrower == a.thrower && best->thrower != a.thrower)) best = &g;
	}
	return best;
}

bool NeedsPlayers() {
	for (const auto& l : g_live)
		if (l.second.attached && (l.second.att.parent == Rig::ParentKind::Player || l.second.att.parent == Rig::ParentKind::Held)) return true;
	return false;
}

// Look the entity parent up by identity (main frame), then read its mount.
void FindParent(Live& l) {
	ParentRead previous = l.parent;
	ParentRead& p = l.parent;
	p = ParentRead{};
	const Rig::Attachment& a = l.att;
	p.mount = a.mount.empty() ? Rig::DefaultMount(a.parent) : a.mount;
	if (!EntitiesReady()) return;
	switch (a.parent) {
	case Rig::ParentKind::Player:
	case Rig::ParentKind::Held: {
		const PlayerRow* player = FindPlayer(a);
		if (!player) return;
		if (a.parent == Rig::ParentKind::Player) {
			p.entityIndex = player->pawn;
			p.type = FollowTargetType::Player;
			p.who = player->name;
		} else {
			CEntityInstance* pawn = EntityAt(player->pawn);
			if (!pawn) return;
			const auto weapon = pawn->GetActiveWeaponHandle();
			if (!weapon.IsValid()) return;
			p.entityIndex = weapon.GetEntryIndex();
			p.type = FollowTargetType::Weapon;
			const std::string cls = ClassOf(EntityAt(p.entityIndex));
			p.isWeapon = IsWeaponClass(cls); p.isBomb = IsBombClass(cls);
			p.who = player->name + "'s " + (p.isBomb ? std::string("C4") : PrettyWeaponName(cls));
		}
		break;
	}
	case Rig::ParentKind::Dropped: {
		CEntityInstance* e = EntityAt(a.entityIndex);
		if (!e || ClassOf(e) != a.entityClass || e->GetOwnerEntityHandle().IsValid()) return;
		p.entityIndex = a.entityIndex;
		p.type = FollowTargetType::Weapon;
		p.isWeapon = IsWeaponClass(a.entityClass); p.isBomb = IsBombClass(a.entityClass);
		p.who = p.isBomb ? "C4" : PrettyWeaponName(a.entityClass);
		break;
	}
	case Rig::ParentKind::Grenade: {
		// Keep the projectile found earlier while its handle is still at that index; otherwise
		// look again, a few times a second, until the throw happens.
		CEntityInstance* cached = previous.found ? EntityAt(previous.entityIndex) : nullptr;
		if (cached && EntityHandleValue(cached) == previous.handle && !ThrownGrenadeClass(cached).empty()) {
			p.entityIndex = previous.entityIndex;
		} else {
			if (g_frame - g_grenadeScanFrame >= 8 || g_frame < g_grenadeScanFrame) { ScanWorld(false); g_grenadeScanFrame = g_frame; }
			const GrenadeRow* g = FindGrenade(a);
			if (!g) return;
			p.entityIndex = g->index;
		}
		p.type = FollowTargetType::Grenade;
		p.mount = "entity";
		p.who = a.grenadeType + (a.thrower.empty() ? "" : " by " + a.thrower);
		break;
	}
	default:
		return;
	}
	CEntityInstance* e = EntityAt(p.entityIndex);
	if (!e) return;
	p.handle = EntityHandleValue(e);
	p.found = true;
}

// Read (or read again) the mount of a parent found this frame.
void ReadParent(Live& l) {
	ParentRead& p = l.parent;
	p.pose = Rig::ExternalPose{};
	if (!p.found) return;
	CEntityInstance* e = EntityAt(p.entityIndex);
	if (!e || EntityHandleValue(e) != p.handle) { p.found = false; p.source = "gone"; return; }
	const AttachTransformResult r = ResolveAttachTransform(e, p.entityIndex, p.type, p.mount,
		p.type == FollowTargetType::Player ? &l.playerPose : nullptr, true);
	p.source = r.source;
	p.pose.valid = r.valid;
	p.pose.oriented = r.oriented;
	p.pose.pose.pos = r.pos;
	p.pose.pose.ang = r.ang;
	// mvm_debug: a held weapon's mount next to its owner's interpolated origin, every read -- a mount
	// that moves in steps while the origin moves smoothly is read from stale bones (player jitter).
	if (MvmDebugLog_Active() && p.type == FollowTargetType::Weapon) {
		FollowVec3 owner{}, self{};
		const auto oh = e->GetOwnerEntityHandle();
		CEntityInstance* pawn = oh.IsValid() ? EntityAt(oh.GetEntryIndex()) : nullptr;
		const bool haveOwner = pawn && ReadOrigin(pawn, owner);
		ReadOrigin(e, self);
		MvmDebugLog_Linef("rig.parent.sample", "frame=%d time=%.5f mount=(%.3f %.3f %.3f) owner=(%.3f %.3f %.3f) ownerOk=%d weapon=(%.3f %.3f %.3f) src=%s ang=(%.3f %.3f %.3f)",
			g_MirvTime.framecount_get(), g_MirvTime.curtime_get(), r.pos.x, r.pos.y, r.pos.z,
			owner.x, owner.y, owner.z, haveOwner ? 1 : 0, self.x, self.y, self.z, r.source.c_str(), r.ang.pitch, r.ang.yaw, r.ang.roll);
	}
}

void ResolveAll() {
	Rig::LocalFn local = [](int id, Pose& out) {
		const auto f = g_live.find(id);
		if (f == g_live.end() || !f->second.info.enabled) return false;
		out = Rig::ChannelsToPose(f->second.local);
		return true;
	};
	Rig::AttachFn attach = [](int id) -> const Rig::Attachment* {
		const auto f = g_live.find(id);
		return f != g_live.end() && f->second.attached ? &f->second.att : nullptr;
	};
	Rig::ExternalFn external = [](int id, const Rig::Attachment&) {
		const auto f = g_live.find(id);
		return f == g_live.end() ? Rig::ExternalPose{} : f->second.parent.pose;
	};
	for (auto& e : g_live) {
		Live& l = e.second;
		l.resolved = Rig::ResolveWorld(e.first, local, attach, external);
		l.visible = Rig::ApplyMissing(l.resolved, l.attached ? l.att.missing : Rig::Missing::Hide, l.hold, l.world);
	}
}

void Publish() {
	RigTransient::State state;
	for (const auto& e : g_live) {
		const Live& l = e.second;
		if (!l.Rigged() && !l.lightKeyed) continue;
		RigTransient::Entry entry;
		entry.pose = l.Rigged();
		std::copy(l.light, l.light + Rig::kLightChannels, entry.light);
		entry.lightMask = l.lightKeyed;
		entry.pos[0] = l.world.pos.x; entry.pos[1] = l.world.pos.y; entry.pos[2] = l.world.pos.z;
		entry.ang[0] = l.world.ang.pitch; entry.ang[1] = l.world.ang.yaw; entry.ang[2] = l.world.ang.roll;
		entry.fov = l.local[Rig::Fov];
		entry.visible = l.visible;
		state.objects[e.first] = entry;
	}
	RigTransient::Publish(std::move(state));
}

// ---- the in-world overlay -----------------------------------------------------------------
// The placed cameras and lights drawn in the world next to the camera path, with the path each
// keyed one takes (SceneOverlayData.h): also while Config is closed and while the Follow Camera
// owns the view. Hidden with F8 and while a camera path plays (as the path's own markers are);
// the drawer keeps it out of every captured frame. `rig overlay on|off`.
bool g_overlayOn = true;
bool g_overlayShown = false;
// A keyed object's path is re-sampled only when its keys or its rest pose change.
struct PathCache { int revision = -1; double rest[3] = {}; std::vector<std::array<double, 3>> points, keys; };
std::map<int, PathCache> g_pathCache;

const PathCache& PathOf(int id, const Live& l, const TrackData& data, int revision) {
	PathCache& c = g_pathCache[id];
	if (c.revision != revision || !std::equal(c.rest, c.rest + 3, l.rest)) {
		c.revision = revision;
		std::copy(l.rest, l.rest + 3, c.rest);
		c.points = SamplePositionPath(data, id, l.rest, 12, 4096, &c.keys);
	}
	return c;
}

void HideOverlay() {
	if (!g_overlayShown) return;
	CameraBridge_SetSceneOverlay(nullptr);
	g_overlayShown = false;
}

// Once a second while logging: what the overlay published, or why it is hidden.
void LogOverlay(const char* what, const SceneOverlay* overlay) {
	if (!MvmDebugLog_Active()) return;
	static ULONGLONG last = 0;
	const ULONGLONG now = GetTickCount64();
	if (now - last < 1000) return;
	last = now;
	if (!overlay) { MvmDebugLog_Linef("rig.overlay", "hidden: %s", what); return; }
	const auto* g = overlay->glyphs.empty() ? nullptr : &overlay->glyphs.front();
	MvmDebugLog_Linef("rig.overlay", "%s paths=%d glyphs=%d firstGlyph=%.1f %.1f %.1f firstPathPoints=%d",
		what, (int)overlay->paths.size(), (int)overlay->glyphs.size(),
		g ? g->pos[0] : 0.f, g ? g->pos[1] : 0.f, g ? g->pos[2] : 0.f,
		overlay->paths.empty() ? 0 : (int)overlay->paths.front().points.size());
}

void PublishOverlay() {
	const char* hidden = !g_overlayOn ? "rig overlay off" : HudBlackout_Active() ? "F8 blackout"
		: g_live.empty() ? "no scene objects"
		: CameraPathRef().GetMode() == CameraPath::Mode::PreviewPlaying ? "a camera path is playing" : nullptr;
	if (hidden) {
		HideOverlay();
		LogOverlay(hidden, nullptr);
		return;
	}
	auto overlay = std::make_shared<SceneOverlay>();
	const TrackData& data = SceneKeys_Data();
	const int revision = SceneKeys_Revision();
	const int selected = Environment::SelectedObject();
	for (const auto& e : g_live) {
		const Live& l = e.second;
		const bool camera = l.Camera();
		const bool light = l.info.kind == "point" || l.info.kind == "spot" || l.info.kind == "area";
		if ((!camera && !light) || !l.info.enabled) continue;
		// One colour per kind (SceneMarkerMath.h); the selected one is lit white by the drawer.
		const SceneOverlay::GlyphKind kind = camera ? SceneOverlay::GlyphKind::Camera
			: l.info.kind == "spot" ? SceneOverlay::GlyphKind::SpotLight
			: l.info.kind == "area" ? SceneOverlay::GlyphKind::AreaLight : SceneOverlay::GlyphKind::PointLight;
		const Marker::Rgb rgb = kind == SceneOverlay::GlyphKind::Camera ? Marker::kSceneCamera
			: kind == SceneOverlay::GlyphKind::SpotLight ? Marker::kSpotLight
			: kind == SceneOverlay::GlyphKind::AreaLight ? Marker::kAreaLight : Marker::kPointLight;
		const SceneOverlayColour colour{rgb.r, rgb.g, rgb.b, 255};
		const bool isSelected = e.first == selected;
		// Only a path in the world frame can be drawn: an attached object's keys are an offset from
		// a parent whose past and future poses are not known here.
		if (l.keyed && !l.attached) {
			const PathCache& sampled = PathOf(e.first, l, data, revision);
			if (sampled.points.size() >= 2) {
				SceneOverlay::Path path;
				path.colour = colour;
				path.colour.a = isSelected ? 235 : 190;
				path.selected = isSelected;
				path.points.reserve(sampled.points.size());
				for (const auto& p : sampled.points) path.points.push_back({float(p[0]), float(p[1]), float(p[2])});
				for (const auto& k : sampled.keys) path.keys.push_back({float(k[0]), float(k[1]), float(k[2])});
				overlay->paths.push_back(std::move(path));
			}
		}
		// Not where it is hidden (a parent that is gone), nor the camera being looked through.
		if (!l.visible || e.first == g_lookThrough) continue;
		SceneOverlay::Glyph glyph;
		glyph.kind = kind;
		glyph.selected = isSelected;
		auto channel = [](float v) { return (unsigned char)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
		glyph.emit = SceneOverlayColour{channel(l.info.color.x), channel(l.info.color.y), channel(l.info.color.z), 255};
		glyph.cone = l.info.cone;
		glyph.width = l.info.width;
		glyph.height = l.info.height;
		glyph.pos[0] = float(l.world.pos.x); glyph.pos[1] = float(l.world.pos.y); glyph.pos[2] = float(l.world.pos.z);
		glyph.ang[0] = float(l.world.ang.pitch); glyph.ang[1] = float(l.world.ang.yaw); glyph.ang[2] = float(l.world.ang.roll);
		glyph.fov = float(l.local[Rig::Fov]);
		glyph.colour = colour;
		overlay->glyphs.push_back(glyph);
	}
	for (auto it = g_pathCache.begin(); it != g_pathCache.end();) it = g_live.count(it->first) ? std::next(it) : g_pathCache.erase(it);
	if (overlay->Empty()) { HideOverlay(); LogOverlay("nothing to draw (no enabled light or camera)", nullptr); return; }
	LogOverlay("published", overlay.get());
	CameraBridge_SetSceneOverlay(std::move(overlay));
	g_overlayShown = true;
}

// ---- Go to camera / Look through: its tick -------------------------------------------------
// The tick a camera object belongs to: its first pose key when the playhead is outside its keys,
// or the throw when it rides a grenade. -1 when it has none, or the playhead is already in its keys.
double SeekTickFor(int id, const Live& l) {
	const TrackData& data = SceneKeys_Data();
	double first = -1, last = -1;
	for (int c = 0; c < Rig::kPoseChannels; ++c) {
		const auto f = data.fields.find(ObjectSpec(id, c)->id);
		if (f == data.fields.end() || f->second.empty()) continue;
		if (first < 0 || f->second.front().tick < first) first = f->second.front().tick;
		if (last < 0 || f->second.back().tick > last) last = f->second.back().tick;
	}
	const double now = SceneKeys_AppliedTick();
	if (first >= 0) return (now >= first - 0.5 && now <= last + 0.5) ? -1 : first;
	if (l.attached && l.att.parent == Rig::ParentKind::Grenade && l.att.throwTick >= 0) {
		const double thrown = double(l.att.throwTick) + 2; // a couple of ticks in: the projectile exists
		return std::fabs(now - thrown) <= 1 ? -1 : thrown;
	}
	return -1;
}

// Go to camera: land on its tick, then put the free cam where it looks from.
struct GoToPending { int id = 0; double tick = -1; int frames = 0; int landedFrames = 0; };
GoToPending g_goto;

void SeekForCamera(int id, const Live& l, const char* why) {
	const double tick = SeekTickFor(id, l);
	if (tick < 0) return;
	DemoScrub_Release(tick, /*resume*/ false);
	MvmDebugLog_LinefAlways("rig.view", "%s camera=%d -> tick %.0f", why, id, tick);
}

void RunGoTo() {
	if (!g_goto.id) return;
	const auto found = g_live.find(g_goto.id);
	if (found == g_live.end()) { g_goto = GoToPending{}; return; }
	const Live* l = &found->second;
	const bool landed = g_goto.tick < 0
		|| (!DemoScrub_Active() && std::fabs(SceneKeys_AppliedTick() - g_goto.tick) <= 1.0);
	if (!landed && ++g_goto.frames < 240) return;
	// Its parent resolves against the landed world a frame or two later. A camera whose parent is
	// gone at this tick (a dead player without a ragdoll mount) has no pose to fly to: going there
	// used to put the free cam at the world origin, under the map (2026-10-17).
	if (!l->visible) {
		if (++g_goto.landedFrames < 30) return;
		g_message = l->info.name + " " + std::to_string(g_goto.id) + " is not in the demo at this tick: what it rides is gone.";
		MvmDebugLog_LinefAlways("rig.view", "go to camera=%d at tick %.0f: hidden (its parent is gone), free cam left where it is",
			g_goto.id, SceneKeys_AppliedTick());
		g_goto = GoToPending{};
		return;
	}
	if (g_lookThrough == g_goto.id) g_lookThrough = 0; // going to it is flying from it
	CameraBridge_SetFreeCamEnabled(true);
	CameraBridge_SetCameraPose(l->world.pos.x, l->world.pos.y, l->world.pos.z,
		l->world.ang.pitch, l->world.ang.yaw, l->world.ang.roll, l->local[Rig::Fov]);
	MvmDebugLog_LinefAlways("rig.view", "go to camera=%d at tick %.0f (waited %d frames)", g_goto.id, SceneKeys_AppliedTick(), g_goto.frames);
	g_goto = GoToPending{};
}

void LogResolve(const char* stage) {
	if (!MvmDebugLog_Active()) return;
	static ULONGLONG last = 0;
	const ULONGLONG now = GetTickCount64();
	if (now - last < 1000) return;
	last = now;
	for (const auto& e : g_live) {
		const Live& l = e.second;
		if (!l.Rigged()) continue;
		MvmDebugLog_Linef("rig.resolve",
			"%s tick=%.0f id=%d kind=%s parent=%s who='%s' mount=%s entity=%d source=%s found=%d valid=%d oriented=%d "
			"keyed=%d keys=%d local=%.1f %.1f %.1f / %.1f %.1f %.1f world=%.1f %.1f %.1f / %.1f %.1f %.1f visible=%d",
			stage, SceneKeys_AppliedTick(), e.first, l.info.kind.c_str(), l.attached ? Rig::ParentKindName(l.att.parent) : "none",
			l.parent.who.c_str(), l.parent.mount.c_str(), l.parent.entityIndex, l.parent.source.c_str(), int(l.parent.found),
			int(l.parent.pose.valid), int(l.parent.pose.oriented), int(l.keyed), l.keys,
			l.local[0], l.local[1], l.local[2], l.local[3], l.local[4], l.local[5],
			l.world.pos.x, l.world.pos.y, l.world.pos.z, l.world.ang.pitch, l.world.ang.yaw, l.world.ang.roll, int(l.visible));
	}
}

// ---- edits -------------------------------------------------------------------------------

double Playhead() { return std::round(TimelinePlayhead_Tick()); }

void ClampChannels(double v[Rig::kPoseChannels]) {
	for (int c = Rig::X; c <= Rig::Z; ++c) v[c] = std::clamp(std::isfinite(v[c]) ? v[c] : 0.0, -32768.0, 32768.0);
	for (int c = Rig::Pitch; c <= Rig::Roll; ++c) v[c] = Rig::WrapDegrees(v[c]);
	v[Rig::Fov] = std::clamp(std::isfinite(v[Rig::Fov]) ? v[Rig::Fov] : 90.0, 1.0, 179.0);
}

// Key every pose channel at the playhead (a "pose key": the channels move together).
void KeyPose(SceneKeyframeTrack& track, int id, const Live& l, const double v[Rig::kPoseChannels], double tick) {
	for (int c = 0; c < l.Channels(); ++c) {
		const ChannelSpec* s = ObjectSpec(id, c);
		track.data.armed.insert(s->id);
		track.Put(*s, tick, Value{{v[c]}, {}});
	}
}

// Store a local pose for a rigged object: keyed when it is animated, its attach offset otherwise.
bool StorePose(int id, Live& l, double v[Rig::kPoseChannels]) {
	ClampChannels(v);
	if (l.Animated()) {
		const double tick = Playhead();
		SceneKeys_Edit([&](SceneKeyframeTrack& track) { KeyPose(track, id, l, v, tick); });
	} else if (l.attached) {
		SceneKeys_Edit([&](SceneKeyframeTrack& track) {
			auto a = track.data.attach.find(id);
			if (a != track.data.attach.end()) a->second.offset = Rig::ChannelsToPose(v);
		});
	} else return false;
	std::copy(v, v + Rig::kPoseChannels, l.local);
	return true;
}

Live* Find(int id) {
	const auto f = g_live.find(id);
	return f == g_live.end() ? nullptr : &f->second;
}

// Where the object is now, in the world: its rigged pose, else the last held one, else its rest.
Pose CurrentWorld(const Live& l) {
	if (l.visible) return l.world;
	if (l.hold.have) return l.hold.last;
	Pose p;
	p.pos = {l.info.position.x, l.info.position.y, l.info.position.z};
	p.ang = {l.info.angles.x, l.info.angles.y, l.info.angles.z};
	return p;
}

bool SetRestInScene(int id, const Pose& world, double fov, std::string& error) {
	return Environment::SetObjectRestPose(id,
		{float(world.pos.x), float(world.pos.y), float(world.pos.z)},
		{float(world.ang.pitch), float(world.ang.yaw), float(world.ang.roll)}, float(fov), error);
}

double Number(const char* text, bool& ok) {
	char* end = nullptr;
	const double n = text ? std::strtod(text, &end) : 0;
	ok = text && *text && end && !*end && std::isfinite(n);
	return n;
}

int IntArg(const char* text, bool& ok) {
	const double n = Number(text, ok);
	ok = ok && n == std::floor(n) && std::fabs(n) < 2147483647.0;
	return ok ? int(n) : 0;
}

// The pose a new parent has right now, to keep the object where it is when it is attached.
bool ParentPoseNow(int id, const Rig::Attachment& a, Pose& out, std::string& who, std::string& error) {
	if (a.parent == Rig::ParentKind::Object) {
		const Live* p = Find(a.object);
		if (!p) { error = "That scene object no longer exists"; return false; }
		if (!p->visible) { error = "That scene object is hidden at this tick"; return false; }
		out = p->world;
		who = p->info.name + " " + std::to_string(a.object);
		return true;
	}
	Live probe;
	probe.att = a;
	if (const Live* l = Find(id)) probe.playerPose = l->playerPose;
	FindParent(probe);
	ReadParent(probe);
	if (!probe.parent.pose.valid) { error = "That parent is not in the demo at this tick (scrub to where it is)"; return false; }
	out = probe.parent.pose.pose;
	if (!probe.parent.pose.oriented) out.ang = {};
	who = probe.parent.who;
	return true;
}

void Attach(int id, Live& l, const Rig::Attachment* next) {
	const Pose world = CurrentWorld(l);
	std::string error;
	if (!next) {
		// Detach: the object stays where it is, in world space.
		const bool animated = l.Animated();
		const double tick = Playhead();
		double v[Rig::kPoseChannels];
		Rig::PoseToChannels(world, l.local[Rig::Fov], v);
		ClampChannels(v);
		SceneKeys_Edit([&](SceneKeyframeTrack& track) {
			track.data.attach.erase(id);
			if (animated) KeyPose(track, id, l, v, tick);
		});
		if (!animated && !SetRestInScene(id, world, l.local[Rig::Fov], error)) g_message = error;
		else g_message = "Detached. It stays where it is.";
		MvmDebugLog_LinefAlways("rig.attach", "id=%d detach world=%.1f %.1f %.1f animated=%d", id, world.pos.x, world.pos.y, world.pos.z, int(animated));
		return;
	}
	Pose parent;
	std::string who;
	if (!ParentPoseNow(id, *next, parent, who, error)) { g_message = error; advancedfx::Warning("rig: %s\n", error.c_str()); return; }
	const Pose local = Rig::Decompose(world, parent);
	Rig::Attachment a = *next;
	a.offset = local;
	const bool animated = l.Animated();
	const double tick = Playhead();
	double v[Rig::kPoseChannels];
	Rig::PoseToChannels(local, l.local[Rig::Fov], v);
	ClampChannels(v);
	a.offset = Rig::ChannelsToPose(v);
	SceneKeys_Edit([&](SceneKeyframeTrack& track) {
		track.data.attach[id] = a;
		if (animated) KeyPose(track, id, l, v, tick);
	});
	g_message = "Attached to " + who + ". It stays where it is.";
	MvmDebugLog_LinefAlways("rig.attach", "id=%d parent=%s who='%s' mount=%s parentPose=%.1f %.1f %.1f / %.1f %.1f %.1f offset=%.1f %.1f %.1f / %.1f %.1f %.1f animated=%d tick=%.0f",
		id, Rig::ParentKindName(a.parent), who.c_str(), a.mount.c_str(), parent.pos.x, parent.pos.y, parent.pos.z,
		parent.ang.pitch, parent.ang.yaw, parent.ang.roll, v[0], v[1], v[2], v[3], v[4], v[5], int(animated), tick);
}

void Animate(int id, Live& l, bool on) {
	const double tick = Playhead();
	double v[Rig::kPoseChannels];
	std::copy(l.local, l.local + Rig::kPoseChannels, v);
	ClampChannels(v);
	if (on) {
		const bool hadKeys = l.keyed;
		SceneKeys_Edit([&](SceneKeyframeTrack& track) {
			for (int c = 0; c < l.Channels(); ++c) track.data.armed.insert(ObjectSpec(id, c)->id);
			// A light's settings are armed with it: a key on any of them is one click on its row.
			for (int c : Rig::ChannelsFor(l.info.kind)) if (c >= Rig::kPoseChannels) track.data.armed.insert(ObjectSpec(id, c)->id);
			// The first key holds the pose it has now, so switching Animate on never moves it.
			if (!hadKeys) KeyPose(track, id, l, v, tick);
		});
		g_message = "Animate is on: Place at camera and pose edits key it at the playhead.";
	} else {
		// Like After Effects' stopwatch: the keys go, and the pose at the playhead stays.
		const bool attached = l.attached;
		SceneKeys_Edit([&](SceneKeyframeTrack& track) {
			for (int c = 0; c < Rig::kObjectChannels; ++c) {
				const std::string sid = Rig::ChannelId(id, c);
				track.data.fields.erase(sid); track.data.armed.erase(sid); track.data.muted.erase(sid);
			}
			track.selected.clear();
			if (attached) { auto a = track.data.attach.find(id); if (a != track.data.attach.end()) a->second.offset = Rig::ChannelsToPose(v); }
		});
		std::string error;
		if (!attached && !SetRestInScene(id, Rig::ChannelsToPose(v), v[Rig::Fov], error)) g_message = error;
		else g_message = "Animate is off. Its keys are gone; it keeps this pose.";
	}
	MvmDebugLog_LinefAlways("rig.animate", "id=%d on=%d tick=%.0f", id, int(on), tick);
}

void Place(int id, Live& l) {
	double eye[3] = {}, view[3] = {}, fov = 90;
	CameraBridge_GetCurrentCamera(eye, view, fov);
	if (!std::isfinite(eye[0]) || !std::isfinite(eye[1]) || !std::isfinite(eye[2])) { g_message = "No camera to place at yet"; return; }
	Pose world;
	world.pos = {eye[0], eye[1], eye[2]};
	// A light points and a camera looks where the view does; a mesh keeps its own turn.
	world.ang = l.info.kind == "point" || l.info.kind == "spot" || l.info.kind == "area" || l.Camera()
		? FollowAngles{view[0], view[1], view[2]} : CurrentWorld(l).ang;
	const double viewFov = l.Camera() ? fov : l.local[Rig::Fov];
	std::string error;
	if (!l.Rigged() && !l.Animated()) {
		if (!SetRestInScene(id, world, viewFov, error)) { g_message = error; return; }
		g_message = "Placed at the camera.";
	} else {
		Pose local = world;
		if (l.attached) {
			if (!l.resolved.valid) { g_message = "Its parent is not in the demo at this tick"; return; }
			local = Rig::Decompose(world, l.resolved.parent);
		}
		double v[Rig::kPoseChannels];
		Rig::PoseToChannels(local, viewFov, v);
		StorePose(id, l, v);
		g_message = l.Animated() ? "Placed at the camera: keyed at the playhead." : "Placed at the camera.";
	}
	MvmDebugLog_LinefAlways("rig.place", "id=%d tick=%.0f world=%.1f %.1f %.1f / %.1f %.1f %.1f fov=%.1f animated=%d attached=%d",
		id, Playhead(), world.pos.x, world.pos.y, world.pos.z, world.ang.pitch, world.ang.yaw, world.ang.roll, viewFov,
		int(l.Animated()), int(l.attached));
}

void PrintStatus(int only) {
	advancedfx::Message("rig: tick=%.0f objects=%zu lookthrough=%d message='%s'\n", SceneKeys_AppliedTick(), g_live.size(), g_lookThrough, g_message.c_str());
	if (!only) {
		// What `rig parent` can take at this tick, by entity index (the RIG card's pickers).
		ScanPlayers();
		ScanWorld(true);
		advancedfx::Message("  players (pawn):");
		for (const auto& p : g_players) advancedfx::Message(" %d=%s%s", p.pawn, p.name.c_str(), p.alive ? "" : "(dead)");
		advancedfx::Message("\n  grenades (entity):");
		for (const auto& g : g_grenades) advancedfx::Message(" %d=%s/%s@%d", g.index, g.type.c_str(), g.thrower.c_str(), g.throwTick);
		advancedfx::Message("\n  dropped (entity):");
		for (const auto& d : g_dropped) advancedfx::Message(" %d=%s", d.index, d.name.c_str());
		advancedfx::Message("\n");
	}
	for (const auto& e : g_live) {
		const Live& l = e.second;
		if (only && e.first != only) continue;
		if (!only && !l.Rigged() && !l.Animated()) continue;
		advancedfx::Message("  %d %s '%s' parent=%s who='%s' mount=%s entity=%d source=%s valid=%d visible=%d keyed=%d armed=%d keys=%d\n"
			"     local=%.2f %.2f %.2f / %.2f %.2f %.2f fov=%.1f world=%.2f %.2f %.2f / %.2f %.2f %.2f\n",
			e.first, l.info.kind.c_str(), l.info.name.c_str(), l.attached ? Rig::ParentKindName(l.att.parent) : "none",
			l.parent.who.c_str(), l.attached ? (l.att.mount.empty() ? Rig::DefaultMount(l.att.parent) : l.att.mount.c_str()) : "-",
			l.parent.entityIndex, l.parent.source.c_str(), int(l.resolved.valid), int(l.visible), int(l.keyed), int(l.armed), l.keys,
			l.local[0], l.local[1], l.local[2], l.local[3], l.local[4], l.local[5], l.local[6],
			l.world.pos.x, l.world.pos.y, l.world.pos.z, l.world.ang.pitch, l.world.ang.yaw, l.world.ang.roll);
	}
}

void MountsJson(JsonBuilder& j, const std::vector<FollowAttachPoint>& points) {
	j.BeginArray();
	for (const auto& p : points) {
		if (!p.valid) continue;
		j.BeginObject(); j.StringField("id", p.id); j.StringField("label", p.label); j.EndObject();
	}
	j.EndArray();
}

void StaticMounts(JsonBuilder& j, bool weapon) {
	static const char* player[][2] = {{"head", "Head"}, {"eyes", "Eyes"}, {"chest", "Chest"}, {"pelvis", "Pelvis"},
		{"feet", "Feet"}, {"weapon_hand_R", "Right hand"}, {"weapon_hand_L", "Left hand"}, {"entity", "Entity"}};
	static const char* gun[][2] = {{"muzzle", "Muzzle"}, {"shell_eject", "Shell eject"}, {"magazine", "Magazine"},
		{"bolt", "Bolt"}, {"slide", "Slide"}, {"entity", "Entity"}};
	j.BeginArray();
	auto put = [&](const char* id, const char* label) { j.BeginObject(); j.StringField("id", id); j.StringField("label", label); j.EndObject(); };
	if (weapon) for (const auto& m : gun) put(m[0], m[1]);
	else for (const auto& m : player) put(m[0], m[1]);
	j.EndArray();
}

} // namespace

// ---- frame ---------------------------------------------------------------------------------

void SceneRig_RunMainThreadFrame() {
	++g_frame;
	if (g_sliderGesture && GetTickCount64() - g_sliderEditAt > 600) { g_sliderGesture = false; SceneKeys_EndGesture(true); }
	const auto objects = Environment::ObjectSummaries();
	const TrackData& data = SceneKeys_Data();
	const double tick = SceneKeys_AppliedTick();
	std::map<int, Live> next;
	for (const auto& info : objects) {
		Live l;
		if (const Live* old = Find(info.id)) { l.playerPose = old->playerPose; l.hold = old->hold; l.parent = old->parent; }
		l.info = info;
		const auto a = data.attach.find(info.id);
		l.attached = a != data.attach.end();
		if (l.attached) l.att = a->second;
		if (l.attached) Rig::PoseToChannels(l.att.offset, info.fov, l.rest);
		else {
			Pose base;
			base.pos = {info.position.x, info.position.y, info.position.z};
			base.ang = {info.angles.x, info.angles.y, info.angles.z};
			Rig::PoseToChannels(base, info.fov, l.rest);
		}
		for (int c = 0; c < Rig::kPoseChannels; ++c) {
			const ChannelSpec* spec = ObjectSpec(info.id, c);
			l.local[c] = l.rest[c];
			if (data.armed.count(spec->id)) l.armed = true;
			const auto f = data.fields.find(spec->id);
			if (f == data.fields.end() || f->second.empty()) continue;
			l.keys = (std::max)(l.keys, int(f->second.size()));
			if (data.muted.count(spec->id)) continue;
			l.local[c] = Sample(*spec, f->second, tick, Value{{l.rest[c]}, {}}).numbers[0];
			l.keyed = true;
		}
		const double lightRest[Rig::kLightChannels] = {info.intensity, info.range, info.cone, info.radius, info.width, info.height};
		for (int i = 0; i < Rig::kLightChannels; ++i) {
			l.lightRest[i] = l.light[i] = lightRest[i];
			if (!Rig::IsLightKind(info.kind)) continue;
			const ChannelSpec* spec = ObjectSpec(info.id, Rig::kPoseChannels + i);
			if (data.armed.count(spec->id)) l.armed = true;
			const auto f = data.fields.find(spec->id);
			if (f == data.fields.end() || f->second.empty() || data.muted.count(spec->id)) continue;
			l.light[i] = Sample(*spec, f->second, tick, Value{{lightRest[i]}, {}}).numbers[0];
			l.lightKeyed |= 1u << i;
		}
		next[info.id] = std::move(l);
	}
	g_live.swap(next);
	if (NeedsPlayers()) ScanPlayers();
	for (auto& e : g_live) {
		Live& l = e.second;
		if (!l.attached || l.att.parent == Rig::ParentKind::Object) { l.parent = ParentRead{}; continue; }
		FindParent(l);
		ReadParent(l);
	}
	ResolveAll();
	Publish();
	PublishOverlay();
	RunGoTo();
	LogResolve("frame");
}

bool SceneRig_ViewSetup(double& x, double& y, double& z, double& pitch, double& yaw, double& roll, double& fov, bool& attached) {
	if (g_live.empty()) return false;
	bool entities = false;
	for (auto& e : g_live) if (e.second.parent.found) { ReadParent(e.second); entities = true; }
	if (entities) { ResolveAll(); Publish(); LogResolve("view"); }
	if (!g_lookThrough) return false;
	const Live* l = Find(g_lookThrough);
	if (!l || !l->Camera() || !l->visible || !l->info.enabled) return false;
	x = l->world.pos.x; y = l->world.pos.y; z = l->world.pos.z;
	pitch = l->world.ang.pitch; yaw = l->world.ang.yaw; roll = l->world.ang.roll;
	fov = l->Rigged() ? l->local[Rig::Fov] : l->info.fov;
	attached = l->attached;
	if (MvmDebugLog_Active()) {
		static ULONGLONG last = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - last >= 1000) {
			last = now;
			MvmDebugLog_Linef("rig.view", "camera=%d tick=%.0f pos=%.1f %.1f %.1f ang=%.1f %.1f %.1f fov=%.1f attached=%d",
				g_lookThrough, SceneKeys_AppliedTick(), x, y, z, pitch, yaw, roll, fov, int(attached));
		}
	}
	return true;
}

void SceneRig_Reset() {
	GrenadeIndex_Reset(); // another demo: its grenades are not this one's
	g_live.clear();
	g_lookThrough = 0;
	RigTransient::Publish(RigTransient::State{});
	g_pathCache.clear();
	HideOverlay();
	g_goto = GoToPending{};
}

int SceneRig_LookThrough() { return g_lookThrough; }

// ---- Environment hooks ---------------------------------------------------------------------

bool SceneRig_Rigged(int id) {
	const Live* l = Find(id);
	return l && (l->Rigged() || l->Animated());
}

bool SceneRig_SetLocal(int id, int channel, double value) {
	Live* l = Find(id);
	if (!l || channel < 0 || channel >= Rig::kPoseChannels || !std::isfinite(value)) return false;
	if (channel == Rig::Fov && (!l->Camera() || !l->Animated())) return false; // the scene's own fov
	if (!(l->Rigged() || l->Animated())) return false;
	double v[Rig::kPoseChannels];
	std::copy(l->local, l->local + Rig::kPoseChannels, v);
	v[channel] = value;
	return StorePose(id, *l, v);
}

bool SceneRig_SetWorld(int id, const double position[3], const double* angles) {
	Live* l = Find(id);
	if (!l || !(l->Rigged() || l->Animated())) return false;
	if (l->attached && !l->resolved.valid) return false; // no parent frame to put it in
	Pose world = CurrentWorld(*l);
	world.pos = {position[0], position[1], position[2]};
	if (angles) world.ang = {angles[0], angles[1], angles[2]};
	Pose local = l->attached ? Rig::Decompose(world, l->resolved.parent) : world;
	if (!angles) local.ang = Rig::ChannelsToPose(l->local).ang;
	double v[Rig::kPoseChannels];
	Rig::PoseToChannels(local, l->local[Rig::Fov], v);
	return StorePose(id, *l, v);
}

void SceneRig_BeginGesture() { SceneKeys_BeginGesture(); }
void SceneRig_EndGesture(bool keep) { SceneKeys_EndGesture(keep); }

bool SceneRig_RestChannel(int id, int channel, double& out) {
	const Live* l = Find(id);
	if (!l || channel < 0 || channel >= Rig::kObjectChannels) return false;
	out = channel < Rig::kPoseChannels ? l->rest[channel] : l->lightRest[channel - Rig::kPoseChannels];
	return true;
}

std::vector<int> SceneRig_TimelineObjects() {
	std::vector<int> out;
	for (const auto& e : g_live)
		if (e.second.Camera() || Rig::IsLightKind(e.second.info.kind)) out.push_back(e.first);
	return out;
}

std::vector<int> SceneRig_DockChannels(int id) {
	const Live* l = Find(id);
	return Rig::ChannelsFor(l ? l->info.kind : std::string());
}

std::string SceneRig_ObjectName(int id) {
	const Live* l = Find(id);
	return l ? l->info.name + " " + std::to_string(id) : "Object " + std::to_string(id) + " (removed)";
}

bool SceneRig_ObjectExists(int id) { return Find(id) != nullptr; }

// ---- console ---------------------------------------------------------------------------------

void SceneRig_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	auto arg = [&](int i) -> const char* { return i < argc ? args->ArgV(i) : ""; };
	const std::string op = arg(2);
	if (op.empty() || op == "help") {
		advancedfx::Message(
			"%s rig status [<id>]\n"
			"%s rig parent <id> none | player <pawn> [<mount>] | held <pawn> [<mount>] | dropped <entity> | grenade <entity> | object <id>\n"
			"%s rig mount <id> <mount>        e.g. head eyes chest pelvis feet weapon_hand_R muzzle shell_eject entity\n"
			"%s rig missing <id> hide|hold\n"
			"%s rig animate <id> on|off       on: edits key the pose at the playhead (linear keys)\n"
			"%s rig place <id>                put it at the camera (keys it when animated)\n"
			"%s rig set <id> x|y|z|pitch|yaw|roll|fov <value>   local pose (the offset when attached)\n"
			"%s rig offset <id> <x> <y> <z> [<pitch> <yaw> <roll>]\n"
			"%s rig lookthrough <id> on|off   render from a camera object\n"
			"%s rig overlay on|off            draw the scene's cameras and lights, and their paths, in the world\n"
			"%s rig goto <id>                 a camera object: go to its tick and put the free cam at its view\n",
			cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
		return;
	}
	if (op == "overlay") {
		const std::string how = arg(3);
		g_overlayOn = how == "on" || (how != "off" && !g_overlayOn);
		advancedfx::Message("rig: the scene's cameras and lights are %s in the world.\n", g_overlayOn ? "drawn" : "not drawn");
		return;
	}
	if (op == "status") {
		bool ok = false;
		const int id = argc > 3 ? IntArg(arg(3), ok) : 0;
		PrintStatus(ok ? id : 0);
		return;
	}
	if (op == "lookthrough" && argc == 4 && std::string(arg(3)) == "off") {
		g_lookThrough = 0; g_message = "Back to the free cam.";
		MvmDebugLog_LinefAlways("rig.view", "look through off");
		return;
	}
	bool ok = false;
	const int id = IntArg(arg(3), ok);
	Live* l = ok ? Find(id) : nullptr;
	if (!l) { advancedfx::Warning("rig: no placed object %s on this map (environment status lists them).\n", arg(3)); return; }
	if (op == "parent") {
		const std::string kind = arg(4);
		Rig::Attachment a;
		if (l->attached) { a.missing = l->att.missing; }
		if (kind == "none") { Attach(id, *l, nullptr); return; }
		if (!Rig::ParseParentKind(kind, a.parent) || a.parent == Rig::ParentKind::None) { advancedfx::Warning("rig parent: none|player|held|dropped|grenade|object\n"); return; }
		// A grenade seen earlier but not in the air now is named by its handle: h<handle>.
		const char* targetArg = arg(5);
		const bool byHandle = a.parent == Rig::ParentKind::Grenade && (targetArg[0] == 'h' || targetArg[0] == 'H');
		const int target = byHandle ? -1 : IntArg(targetArg, ok);
		if (byHandle) ok = true;
		if (!ok) { advancedfx::Warning("rig parent %s: give the entity index (or object id)\n", kind.c_str()); return; }
		a.mount = argc > 6 ? arg(6) : Rig::DefaultMount(a.parent);
		if (!Rig::MountName(a.mount)) { advancedfx::Warning("rig: invalid mount name\n"); return; }
		if (a.parent == Rig::ParentKind::Player || a.parent == Rig::ParentKind::Held) {
			ScanPlayers();
			const PlayerRow* row = nullptr;
			for (const auto& p : g_players) if (p.pawn == target) row = &p;
			if (!row) { advancedfx::Warning("rig: %d is not a player pawn at this tick\n", target); return; }
			a.steamId = row->steamId; a.playerName = row->name;
		} else if (a.parent == Rig::ParentKind::Dropped) {
			CEntityInstance* e = EntityAt(target);
			const std::string cls = e ? ClassOf(e) : "";
			if (!e || !IsWeaponOrBomb(cls) || e->GetOwnerEntityHandle().IsValid()) { advancedfx::Warning("rig: %d is not a dropped weapon at this tick\n", target); return; }
			a.entityIndex = target; a.entityClass = cls; a.mount = argc > 6 ? arg(6) : "entity";
		} else if (a.parent == Rig::ParentKind::Grenade) {
			const GrenadeSighting* found = nullptr;
			const uint64_t handle = byHandle ? std::strtoull(targetArg + 1, nullptr, 10) : 0;
			for (const GrenadeSighting& s : GrenadeIndex_Update())
				if (byHandle ? s.handle == handle : (s.active && s.entityIndex == target)) { found = &s; break; }
			if (!found) { advancedfx::Warning("rig: %s is not a grenade this session has seen\n", targetArg); return; }
			a.grenadeHandle = found->handle; a.grenadeType = found->type; a.throwTick = found->throwTick; a.thrower = found->thrower; a.mount = "entity";
		} else {
			if (target == id || !Find(target)) { advancedfx::Warning("rig: choose another placed object\n"); return; }
			if (Rig::WouldCycle(SceneKeys_Data().attach, id, target)) { advancedfx::Warning("rig: that would attach it to itself\n"); return; }
			a.object = target; a.mount = "";
		}
		Attach(id, *l, &a);
		return;
	}
	if (op == "mount" || op == "missing") {
		if (!l->attached) { advancedfx::Warning("rig: attach it first\n"); return; }
		Rig::Attachment a = l->att;
		const std::string value = arg(4);
		if (op == "mount") {
			if (!Rig::MountName(value) || value.empty()) { advancedfx::Warning("rig: invalid mount name\n"); return; }
			a.mount = value;
			Attach(id, *l, &a); // keeps it where it is: the offset is re-solved on the new mount
		} else {
			if (value != "hide" && value != "hold") { advancedfx::Warning("rig missing: hide|hold\n"); return; }
			a.missing = value == "hold" ? Rig::Missing::Hold : Rig::Missing::Hide;
			SceneKeys_Edit([&](SceneKeyframeTrack& track) { track.data.attach[id].missing = a.missing; });
		}
		return;
	}
	if (op == "animate") {
		const std::string how = arg(4);
		Animate(id, *l, how == "on" || (how != "off" && !l->Animated()));
		return;
	}
	if (op == "place") { Place(id, *l); return; }
	if (op == "set" && argc == 6) {
		const int channel = Rig::ChannelOfKey(arg(4));
		const double value = Number(arg(5), ok);
		if (channel < 0 || !ok) { advancedfx::Warning("rig set: x|y|z|pitch|yaw|roll|fov <number>\n"); return; }
		if (channel == Rig::Fov && !l->Camera()) { advancedfx::Warning("rig: only cameras have a field of view\n"); return; }
		if (l->Rigged() || l->Animated()) SliderEdit();
		if (!SceneRig_SetLocal(id, channel, value)) {
			// Not rigged (the scene's own world pose), or the field of view of a camera that rides
			// something without keys: both are the map's authored values.
			Pose p;
			p.pos = {l->info.position.x, l->info.position.y, l->info.position.z};
			p.ang = {l->info.angles.x, l->info.angles.y, l->info.angles.z};
			double v[Rig::kPoseChannels];
			Rig::PoseToChannels(p, l->info.fov, v);
			v[channel] = value;
			ClampChannels(v);
			std::string error;
			if (!SetRestInScene(id, Rig::ChannelsToPose(v), v[Rig::Fov], error)) advancedfx::Warning("rig: %s\n", error.c_str());
		}
		return;
	}
	if (op == "offset" && (argc == 7 || argc == 10)) {
		double v[Rig::kPoseChannels];
		std::copy(l->local, l->local + Rig::kPoseChannels, v);
		for (int i = 0; i < argc - 4; ++i) { v[i] = Number(arg(4 + i), ok); if (!ok) { advancedfx::Warning("rig offset: numbers only\n"); return; } }
		if (!l->Rigged() && !l->Animated()) { advancedfx::Warning("rig offset: attach or animate it first\n"); return; }
		StorePose(id, *l, v);
		return;
	}
	if (op == "goto") {
		if (!l->Camera()) { advancedfx::Warning("rig: Go to camera is for camera objects\n"); return; }
		const double tick = SeekTickFor(id, *l);
		SeekForCamera(id, *l, "go to");
		g_goto = GoToPending{id, tick, 0};
		g_message = "Going to " + l->info.name + " " + std::to_string(id) + ".";
		return;
	}
	if (op == "lookthrough") {
		const std::string how = arg(4);
		const bool on = how == "on" || (how != "off" && g_lookThrough != id);
		if (on && !l->Camera()) { advancedfx::Warning("rig: only camera objects can be looked through\n"); return; }
		if (on) SeekForCamera(id, *l, "look through");
		g_lookThrough = on ? id : (g_lookThrough == id ? 0 : g_lookThrough);
		g_message = on ? "Looking through " + l->info.name + " " + std::to_string(id) + "." : "Back to the free cam.";
		MvmDebugLog_LinefAlways("rig.view", "look through camera=%d on=%d", id, int(on));
		return;
	}
	advancedfx::Warning("rig: unknown command (%s rig help)\n", cmd);
}

void SceneRig_AppendStateJson(std::ostringstream& out) {
	// The pickers change slowly; listing them every frame would rebuild the card for nothing.
	if (g_frame - g_listsFrame >= 15 || g_frame < g_listsFrame) {
		g_listsFrame = g_frame;
		ScanPlayers();
		ScanWorld(true);
	}
	JsonBuilder j;
	j.BeginObject();
	j.IntField("lookThrough", g_lookThrough);
	j.BoolField("overlay", g_overlayOn);
	j.StringField("message", g_message);
	j.Key("objects"); j.BeginObject();
	for (const auto& e : g_live) {
		const Live& l = e.second;
		j.Key(std::to_string(e.first).c_str()); j.BeginObject();
		j.BoolField("camera", l.Camera());
		j.BoolField("attached", l.attached);
		j.StringField("parent", l.attached ? Rig::ParentKindName(l.att.parent) : "none");
		j.StringField("who", l.parent.who.empty() && l.attached && l.att.parent == Rig::ParentKind::Object ? SceneRig_ObjectName(l.att.object) : l.parent.who);
		j.StringField("player", l.att.playerName);
		j.IntField("object", l.att.object);
		j.StringField("mount", l.attached ? (l.att.mount.empty() ? Rig::DefaultMount(l.att.parent) : l.att.mount) : "");
		j.StringField("missing", l.att.missing == Rig::Missing::Hold ? "hold" : "hide");
		j.BoolField("valid", l.resolved.valid);
		j.BoolField("visible", l.visible);
		j.BoolField("animated", l.Animated());
		j.IntField("keys", l.keys);
		j.Key("local"); j.BeginArray(); for (double v : l.local) j.ValueDouble(std::round(v * 100) / 100); j.EndArray();
		if (l.attached && l.parent.found) {
			CEntityInstance* parent = EntityAt(l.parent.entityIndex);
			j.Key("mounts"); MountsJson(j, AvailableAttachPoints(parent, l.parent.type, l.parent.isWeapon, l.parent.isBomb));
		}
		j.EndObject();
	}
	j.EndObject();
	j.Key("players"); j.BeginArray();
	for (const auto& p : g_players) {
		j.BeginObject(); j.IntField("pawn", p.pawn); j.StringField("name", p.name); j.IntField("team", p.team); j.BoolField("alive", p.alive); j.EndObject();
	}
	j.EndArray();
	j.Key("grenades"); j.BeginArray();
	for (const auto& g : g_grenades) {
		j.BeginObject(); j.IntField("index", g.index); j.StringField("handle", std::to_string(g.handle)); j.StringField("type", g.type);
		j.StringField("thrower", g.thrower); j.IntField("throwTick", g.throwTick); j.BoolField("active", g.index >= 0); j.EndObject();
	}
	j.EndArray();
	j.Key("dropped"); j.BeginArray();
	for (const auto& d : g_dropped) {
		j.BeginObject(); j.IntField("index", d.index); j.StringField("name", d.name); j.EndObject();
	}
	j.EndArray();
	j.Key("playerMounts"); StaticMounts(j, false);
	j.Key("weaponMounts"); StaticMounts(j, true);
	j.EndObject();
	out << ",\"rig\":" << j.Str();
}

} // namespace Filmmaker
