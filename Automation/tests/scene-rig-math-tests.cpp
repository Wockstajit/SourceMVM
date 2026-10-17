// Scene object rigs (Docs/features/scene-object-rigs.md): attaching a placed object to a parent
// frame, resolving chains of attachments, the missing-parent policy, pose channel ids and the
// attach map's file format. Engine-free: SceneRigMath.h only.
#include "../../AfxHookSource2/Filmmaker/Movie/SceneRigMath.h"

#include <cmath>
#include <iostream>
#include <map>
#include <string>

using namespace Filmmaker;
using namespace Filmmaker::Rig;

namespace {
int g_failed = 0;
void Check(bool pass, const char* label) {
	if (pass) return;
	std::cerr << "FAILED " << label << "\n";
	++g_failed;
}
bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
bool NearAngle(double a, double b, double eps = 1e-5) { return std::fabs(WrapDegrees(a - b)) <= eps; }
bool NearPose(const Pose& a, const Pose& b, double eps = 1e-5) {
	return Near(a.pos.x, b.pos.x, eps) && Near(a.pos.y, b.pos.y, eps) && Near(a.pos.z, b.pos.z, eps) &&
		NearAngle(a.ang.pitch, b.ang.pitch, eps) && NearAngle(a.ang.yaw, b.ang.yaw, eps) && NearAngle(a.ang.roll, b.ang.roll, eps);
}
Pose P(double x, double y, double z, double p, double yw, double r) { Pose o; o.pos = {x, y, z}; o.ang = {p, yw, r}; return o; }
}

int main() {
	// ---- compose / decompose ----
	{
		// A head facing +Y (yaw 90): 20 forward is +Y in the world. Right and up are checked against
		// the follow camera's own basis, so a light and the follow camera agree on what an offset
		// means.
		const Pose head = P(100, 200, 64, 0, 90, 0);
		const Pose world = Compose(head, P(20, 0, 0, 0, 0, 0));
		Check(NearPose(world, P(100, 220, 64, 0, 90, 0)), "forward offset follows the parent's yaw");
		const Pose right = Compose(head, P(0, 10, 0, 0, 0, 0));
		const FollowVec3 r = FollowRotateVector({0, 10, 0}, head.ang);
		Check(Near(right.pos.x, 100 + r.x) && Near(right.pos.y, 200 + r.y), "right offset uses the follow camera's basis");
		const Pose up = Compose(head, P(0, 0, 5, 0, 0, 0));
		Check(NearPose(up, P(100, 200, 69, 0, 90, 0)), "up offset");
		const Pose turned = Compose(head, P(0, 0, 0, 0, 30, 0));
		Check(NearAngle(turned.ang.yaw, 120) && NearAngle(turned.ang.pitch, 0), "local yaw adds to a level parent");
		const Pose tilted = Compose(P(0, 0, 0, 0, 0, 0), P(0, 0, 0, 25, 0, 0));
		Check(NearAngle(tilted.ang.pitch, 25), "identity parent keeps local angles");
	}
	{
		// Round trips over parents with pitch and roll: Decompose(Compose(p, l), p) == l, and
		// Compose(p, Decompose(w, p)) == w.
		const Pose parents[] = {P(0, 0, 0, 0, 0, 0), P(10, -20, 30, 35, 120, 0), P(-400, 80, 12, -60, -170, 45),
			P(5, 5, 5, 10, 270, -80), P(1, 2, 3, 89, 10, 170)};
		const Pose locals[] = {P(20, 0, 0, 0, 0, 0), P(-12, 7, 3, 15, -40, 10), P(0, 0, 64, -80, 179, -179)};
		bool all = true, back = true;
		for (const auto& p : parents) for (const auto& l : locals) {
			const Pose w = Compose(p, l);
			if (!NearPose(Decompose(w, p), l, 1e-4)) all = false;
			if (!NearPose(Compose(p, Decompose(w, p)), w, 1e-4)) back = false;
		}
		Check(all, "decompose inverts compose for pitched and rolled parents");
		Check(back, "compose of a decomposed world pose lands on it");
		// The distance to the parent is the offset's length, whatever the parent's rotation.
		const Pose w = Compose(parents[2], locals[1]);
		const double dx = w.pos.x - parents[2].pos.x, dy = w.pos.y - parents[2].pos.y, dz = w.pos.z - parents[2].pos.z;
		Check(Near(std::sqrt(dx * dx + dy * dy + dz * dz), std::sqrt(12.0 * 12 + 7 * 7 + 3 * 3), 1e-6), "rigid offset keeps its length");
	}
	Check(Near(WrapDegrees(540), 180) && Near(WrapDegrees(-190), 170) && Near(WrapDegrees(180), 180) && Near(WrapDegrees(-180), 180), "wrap to (-180, 180]");

	// ---- chain resolution ----
	{
		std::map<int, Pose> locals = {{1, P(0, 0, 10, 0, 0, 0)}, {2, P(5, 0, 0, 0, 0, 0)}, {3, P(100, 0, 0, 0, 0, 0)}, {4, P(1, 0, 0, 0, 0, 0)}};
		std::map<int, Attachment> attach;
		Attachment onPlayer; onPlayer.parent = ParentKind::Player; onPlayer.steamId = 7; onPlayer.mount = "head";
		attach[1] = onPlayer;
		Attachment onOne; onOne.parent = ParentKind::Object; onOne.object = 1;
		attach[2] = onOne;
		bool playerHere = true, oriented = true;
		const Pose head = P(50, 60, 70, 0, 90, 0);
		LocalFn local = [&](int id, Pose& out) { auto f = locals.find(id); if (f == locals.end()) return false; out = f->second; return true; };
		AttachFn att = [&](int id) -> const Attachment* { auto f = attach.find(id); return f == attach.end() ? nullptr : &f->second; };
		ExternalFn ext = [&](int, const Attachment&) { ExternalPose e; e.valid = playerHere; e.oriented = oriented; e.pose = head; return e; };

		Resolved r3 = ResolveWorld(3, local, att, ext);
		Check(r3.valid && !r3.attached && NearPose(r3.world, locals[3]), "unattached object sits in world space");
		Resolved r1 = ResolveWorld(1, local, att, ext);
		Check(r1.valid && r1.attached && NearPose(r1.world, P(50, 60, 80, 0, 90, 0)) && NearPose(r1.parent, head), "attached to a player");
		Resolved r2 = ResolveWorld(2, local, att, ext);
		Check(r2.valid && NearPose(r2.world, Compose(r1.world, locals[2])) && NearPose(r2.parent, r1.world), "chained through another object");
		Check(r2.depth == 2, "chain depth counts every hop");

		oriented = false;
		Resolved flat = ResolveWorld(1, local, att, ext);
		Check(flat.valid && NearPose(flat.world, P(50, 60, 80, 0, 0, 0)), "a position-only parent gives world-aligned axes");
		oriented = true;

		playerHere = false;
		Check(!ResolveWorld(1, local, att, ext).valid, "missing player");
		Check(!ResolveWorld(2, local, att, ext).valid, "a chain whose root is missing is missing");
		playerHere = true;

		locals.erase(1);
		Check(!ResolveWorld(2, local, att, ext).valid, "a removed parent object is missing");
		locals[1] = P(0, 0, 10, 0, 0, 0);

		// A loop that got in anyway (hand-edited file) terminates and is missing.
		Attachment onTwo; onTwo.parent = ParentKind::Object; onTwo.object = 2;
		attach[1] = onTwo;
		Check(!ResolveWorld(1, local, att, ext).valid && !ResolveWorld(2, local, att, ext).valid, "a loop resolves to missing");
		attach[1] = onPlayer;

		Check(WouldCycle(attach, 1, 2), "re-parenting 1 under its own child is a loop");
		Check(WouldCycle(attach, 4, 4), "an object cannot ride itself");
		Check(!WouldCycle(attach, 4, 2), "riding the end of a chain is fine");
		// Depth limit.
		std::map<int, Attachment> deep;
		for (int i = 2; i <= 20; ++i) { Attachment a; a.parent = ParentKind::Object; a.object = i - 1; deep[i] = a; }
		Check(WouldCycle(deep, 21, 20), "chains past the depth limit are refused");
		Check(!WouldCycle(deep, 21, 3), "short chains are allowed");
	}

	// ---- missing policy ----
	{
		HoldState hold; Pose out;
		Resolved missing;
		Check(!ApplyMissing(missing, Missing::Hold, hold, out), "hold with no pose yet hides");
		Resolved seen; seen.valid = true; seen.world = P(1, 2, 3, 4, 5, 6);
		Check(ApplyMissing(seen, Missing::Hide, hold, out) && NearPose(out, seen.world), "present parent draws");
		Check(!ApplyMissing(missing, Missing::Hide, hold, out), "hide policy hides while missing");
		Check(ApplyMissing(missing, Missing::Hold, hold, out) && NearPose(out, seen.world), "hold keeps the last pose");
	}

	// ---- channel ids ----
	{
		int id = 0, c = 0;
		Check(ChannelId(12, Yaw) == "obj.12.yaw", "channel id format");
		Check(ParseChannelId("obj.12.yaw", id, c) && id == 12 && c == Yaw, "channel id parse");
		Check(!ParseChannelId("obj.0.x", id, c) && !ParseChannelId("obj.012.x", id, c) && !ParseChannelId("obj.3.w", id, c) &&
			!ParseChannelId("obj.3", id, c) && !ParseChannelId("env.fog", id, c) && !ParseChannelId("obj.99999999.x", id, c), "bad channel ids rejected");
		Check(ParseChannelIndex(ChannelIndex(kMaxObjectId, Fov), id, c) && id == kMaxObjectId && c == Fov, "channel index round trip at the id limit");
		Check(!ParseChannelIndex(5, id, c) && !ParseChannelIndex(kObjectChannelBase + kChannelStride + kObjectChannels, id, c), "catalog indices and the spare slot are not object channels");
		double v[kPoseChannels];
		PoseToChannels(P(1, 2, 3, 190, -200, 30), 75, v);
		Check(Near(v[Pitch], -170) && Near(v[Yaw], 160) && Near(v[Fov], 75), "channel values are wrapped");
		Check(NearPose(ChannelsToPose(v), P(1, 2, 3, -170, 160, 30)), "channels back to a pose");
	}

	// ---- file format ----
	{
		std::map<int, Attachment> m;
		Attachment a; a.parent = ParentKind::Player; a.steamId = 76561198000000001ull; a.playerName = "s1mple"; a.mount = "head"; a.offset = P(20, -3, 4.5, 10, 20, -30);
		Attachment b; b.parent = ParentKind::Held; b.steamId = 0; b.playerName = "BOT Kev"; b.mount = "muzzle"; b.missing = Missing::Hold;
		Attachment g; g.parent = ParentKind::Grenade; g.grenadeType = "smoke"; g.throwTick = 18340; g.thrower = "s1mple"; g.mount = "entity";
		Attachment d; d.parent = ParentKind::Dropped; d.entityIndex = 212; d.entityClass = "weapon_ak47"; d.mount = "entity";
		Attachment o; o.parent = ParentKind::Object; o.object = 3; o.mount = "";
		m[3] = a; m[4] = b; m[5] = g; m[6] = d; m[7] = o;
		JsonBuilder j; WriteAttachments(j, m);
		JsonValue v; std::map<int, Attachment> back; std::string error;
		Check(JsonParse(j.Str(), v) && ReadAttachments(v, back, error) && back == m, "attach map round trip (64-bit steam id kept exactly)");
		auto rejects = [&](const std::string& text, const char* label) {
			JsonValue bad; std::map<int, Attachment> keep = m; std::string err;
			Check(JsonParse(text, bad) && !ReadAttachments(bad, keep, err) && keep == m && !err.empty(), label);
		};
		const std::string tail = R"("mount":"head","missing":"hide","offset":[0,0,0,0,0,0]})";
		rejects(R"({"0":{"parent":"player","steamId":"1","name":"a",)" + tail + "}", "object id 0 rejected");
		rejects(R"({"3":{"parent":"camera","steamId":"1","name":"a",)" + tail + "}", "unknown parent rejected");
		rejects(R"({"3":{"parent":"player","steamId":"-1","name":"a",)" + tail + "}", "signed steam id rejected");
		rejects(R"({"3":{"parent":"player","steamId":"0","name":"",)" + tail + "}", "player without identity rejected");
		rejects(R"({"3":{"parent":"object","object":3,)" + tail + "}", "self parent rejected");
		rejects(R"({"3":{"parent":"object","object":4,)" + tail + R"(,"4":{"parent":"object","object":3,)" + tail + "}", "loop rejected");
		rejects(R"({"3":{"parent":"player","steamId":"1","name":"a","mount":"he ad","missing":"hide","offset":[0,0,0,0,0,0]}})", "mount with a space rejected");
		rejects(R"({"3":{"parent":"player","steamId":"1","name":"a","mount":"head","missing":"later","offset":[0,0,0,0,0,0]}})", "unknown missing policy rejected");
		rejects(R"({"3":{"parent":"player","steamId":"1","name":"a","mount":"head","missing":"hide","offset":[0,0,0,0,0]}})", "short offset rejected");
		rejects(R"({"3":{"parent":"player","steamId":"1","name":"a","mount":"head","missing":"hide","offset":[0,0,40000,0,0,0]}})", "offset out of range rejected");
		rejects(R"({"3":{"parent":"grenade","type":"smoke","throwTick":1.5,"thrower":"",)" + tail + "}", "fractional throw tick rejected");
		rejects(R"({"3":{"parent":"dropped","entity":-2,"class":"weapon_ak47",)" + tail + "}", "negative entity rejected");
	}

	if (g_failed) { std::cerr << g_failed << " scene rig check(s) failed.\n"; return 1; }
	std::cout << "Scene rig compose, chains, missing policy, channel ids and file format passed.\n";
	return 0;
}
