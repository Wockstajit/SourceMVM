#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentGltfMath.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace Filmmaker::Environment;

namespace {

int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

bool Near(float a, float b, float tolerance = 1e-3f) { return std::abs(a - b) <= tolerance; }
bool NearV(Vec3 a, Vec3 b, float tolerance = 1e-3f) { return Near(a.x, b.x, tolerance) && Near(a.y, b.y, tolerance) && Near(a.z, b.z, tolerance); }

struct Bin {
	std::vector<uint8_t> bytes;
	size_t Floats(const std::vector<float>& f) {
		while (bytes.size() % 4) bytes.push_back(0);
		const size_t at = bytes.size();
		bytes.resize(at + f.size() * 4);
		std::memcpy(bytes.data() + at, f.data(), f.size() * 4);
		return at;
	}
	size_t Bytes(const std::vector<uint8_t>& b) {
		while (bytes.size() % 4) bytes.push_back(0);
		const size_t at = bytes.size();
		bytes.insert(bytes.end(), b.begin(), b.end());
		return at;
	}
};

std::vector<uint8_t> Glb(const std::string& json, const std::vector<uint8_t>& bin) {
	std::string j = json;
	while (j.size() % 4) j.push_back(' ');
	std::vector<uint8_t> b = bin;
	while (b.size() % 4) b.push_back(0);
	std::vector<uint8_t> out(12);
	auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i))); };
	u32(uint32_t(j.size())); u32(0x4E4F534A); out.insert(out.end(), j.begin(), j.end());
	if (!b.empty()) { u32(uint32_t(b.size())); u32(0x004E4942); out.insert(out.end(), b.begin(), b.end()); }
	const uint32_t magic = 0x46546C67, version = 2, length = uint32_t(out.size());
	std::memcpy(out.data(), &magic, 4); std::memcpy(out.data() + 4, &version, 4); std::memcpy(out.data() + 8, &length, 4);
	return out;
}

std::string View(size_t offset, size_t length) {
	return "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) + ",\"byteLength\":" + std::to_string(length) + "}";
}
std::string Acc(int view, int type, size_t count, const char* kind, bool normalized = false) {
	return "{\"bufferView\":" + std::to_string(view) + ",\"componentType\":" + std::to_string(type) + ",\"count\":" +
		std::to_string(count) + ",\"type\":\"" + kind + "\"" + (normalized ? ",\"normalized\":true" : "") + "}";
}

} // namespace

// `environment-gltf-math-tests <file.glb>...`: parse real files and print what came out (manual
// check of exported models; not part of the ctest run).
int Inspect(int argc, char** argv) {
	int bad = 0;
	for (int i = 1; i < argc; ++i) {
		FILE* f = std::fopen(argv[i], "rb");
		if (!f) { std::cout << argv[i] << ": cannot open\n"; ++bad; continue; }
		std::vector<uint8_t> bytes;
		uint8_t chunk[65536];
		size_t n;
		while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) bytes.insert(bytes.end(), chunk, chunk + n);
		std::fclose(f);
		GltfModel model;
		std::string error;
		if (!ParseGltf(bytes.data(), bytes.size(), model, error)) { std::cout << argv[i] << ": FAILED " << error << "\n"; ++bad; continue; }
		size_t textured = 0;
		for (const auto& m : model.materials) if (m.albedoImage >= 0) ++textured;
		std::cout << argv[i] << ": " << model.vertices.size() / 3 << " tris, " << model.parts.size() << " parts, "
		          << model.materials.size() << " materials (" << textured << " textured), " << model.images.size()
		          << " images, " << model.bones.size() << " bones, " << model.clips.size() << " clips";
		for (size_t c = 0; c < model.clips.size() && c < 6; ++c) std::cout << (c ? ", " : " [") << model.clips[c].name << " " << model.clips[c].duration << "s";
		std::cout << (model.clips.empty() ? "" : "]") << "\n";
		std::vector<Vec3> p, nrm;
		GltfSkin(model, model.clips.empty() ? -1 : 0, model.clips.empty() ? 0 : model.clips[0].duration * .5, p, nrm);
		bool finite = true;
		for (const Vec3& v : p) finite = finite && Finite(v) && std::abs(v.x) < 1e4f && std::abs(v.y) < 1e4f && std::abs(v.z) < 1e4f;
		std::cout << "   posed mid-clip: " << (finite ? "finite" : "NOT FINITE/EXPLODED") << "\n";
		if (!model.clips.empty()) {
			std::vector<Vec3> rest, restN, q, qn;
			GltfSkin(model, 0, 0, rest, restN);
			float most = 0;
			for (int step = 1; step <= 8; ++step) {
				GltfSkin(model, 0, model.clips[0].duration * step / 8.0, q, qn);
				for (size_t v = 0; v < q.size(); ++v) { const Vec3 d = q[v] - rest[v]; most = std::max(most, std::sqrt(Dot(d, d))); }
			}
			std::cout << "   largest vertex travel over the clip: " << most << " (normalized units; the mesh is 2 tall)\n";
		}
		if (!finite) ++bad;
	}
	return bad ? 1 : 0;
}

int main(int argc, char** argv) {
	if (argc > 1) return Inspect(argc, argv);
	// A skinned triangle on two joints: root at the origin, joint 1 at y=1. Vertex 2 hangs on
	// joint 1 alone; a 90-degree turn of joint 1 about Z at t=1 must swing it.
	{
		Bin bin;
		const size_t pos = bin.Floats({0, 0, 0, 1, 0, 0, 0, 2, 0});
		const size_t nrm = bin.Floats({0, 0, 1, 0, 0, 1, 0, 0, 1});
		const size_t uv = bin.Floats({0, 0, 1, 0, .5f, 1});
		const size_t joints = bin.Bytes({0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0});
		const size_t weights = bin.Floats({1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0});
		// inverse binds: joint0 identity, joint1 translate(0,-1,0)
		const size_t ibm = bin.Floats({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
		                              1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1});
		const size_t times = bin.Floats({0, 1});
		const float s = std::sqrt(.5f);
		const size_t rots = bin.Floats({0, 0, 0, 1, 0, 0, s, s});
		const size_t png = bin.Bytes({0x89, 'P', 'N', 'G', 1, 2, 3});
		const std::string json = std::string("{\"asset\":{\"version\":\"2.0\"},") +
			"\"buffers\":[{\"byteLength\":" + std::to_string(bin.bytes.size()) + "}]," +
			"\"bufferViews\":[" + View(pos, 36) + "," + View(nrm, 36) + "," + View(uv, 24) + "," + View(joints, 12) + "," +
			View(weights, 48) + "," + View(ibm, 128) + "," + View(times, 8) + "," + View(rots, 32) + "," + View(png, 7) + "]," +
			"\"accessors\":[" + Acc(0, 5126, 3, "VEC3") + "," + Acc(1, 5126, 3, "VEC3") + "," + Acc(2, 5126, 3, "VEC2") + "," +
			Acc(3, 5121, 3, "VEC4") + "," + Acc(4, 5126, 3, "VEC4") + "," + Acc(5, 5126, 2, "MAT4") + "," +
			Acc(6, 5126, 2, "SCALAR") + "," + Acc(7, 5126, 2, "VEC4") + "]," +
			"\"images\":[{\"bufferView\":8,\"mimeType\":\"image/png\"}],\"textures\":[{\"source\":0}]," +
			"\"materials\":[{\"name\":\"skin\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,0.5,0.25,1]," +
			"\"baseColorTexture\":{\"index\":0},\"roughnessFactor\":0.4,\"metallicFactor\":0}}]," +
			"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2," +
			"\"JOINTS_0\":3,\"WEIGHTS_0\":4},\"material\":0}]}]," +
			"\"skins\":[{\"joints\":[1,2],\"inverseBindMatrices\":5}]," +
			"\"nodes\":[{\"mesh\":0,\"skin\":0},{\"children\":[2]},{\"translation\":[0,1,0]}]," +
			"\"scenes\":[{\"nodes\":[0,1]}]," +
			"\"animations\":[{\"name\":\"Swing\",\"samplers\":[{\"input\":6,\"output\":7}]," +
			"\"channels\":[{\"sampler\":0,\"target\":{\"node\":2,\"path\":\"rotation\"}}]}]}";
		const auto glb = Glb(json, bin.bytes);
		GltfModel model;
		std::string error;
		const bool parsed = ParseGltf(glb.data(), glb.size(), model, error);
		CHECK(parsed);
		if (!parsed) { std::cerr << error << "\n"; return 1; }
		CHECK(error.empty());
		CHECK(model.vertices.size() == 3 && model.parts.size() == 1 && model.parts[0].count == 3);
		CHECK(model.bones.size() == 2 && model.clips.size() == 1 && model.clips[0].name == "Swing");
		CHECK(Near(model.clips[0].duration, 1));
		CHECK(model.materials.size() == 1 && model.materials[0].albedoImage == 0 && Near(model.materials[0].color[1], .5f));
		CHECK(Near(model.materials[0].roughness, .4f) && Near(model.materials[0].metallic, 0));
		CHECK(model.images.size() == 1 && model.images[0].mime == "image/png" && model.images[0].bytes.size() == 7);
		CHECK(Near(model.vertices[2].u, .5f) && Near(model.vertices[2].v, 1)); // top-left origin kept
		CHECK(GltfClipIndex(model, "Swing") == 0 && GltfClipIndex(model, "nope") == -1);

		// Rest pose, raw world axes: glTF (x, y, z) -> world (x, -z, y), so glTF up is world +Z.
		std::vector<GltfMat4> world;
		std::vector<Vec3> p, n;
		GltfPose(model, -1, 0, world);
		GltfSkinRaw(model, world, p, n);
		CHECK(NearV(p[0], {0, 0, 0}) && NearV(p[1], {1, 0, 0}) && NearV(p[2], {0, 0, 2}));
		CHECK(NearV(n[0], {0, -1, 0})); // glTF +Z normal -> world -Y

		// t=1: joint 1 turned 90 degrees about glTF Z: vertex 2 (one unit above the joint) swings
		// to glTF (-1, 1, 0) = world (-1, 0, 1). Halfway: 45 degrees.
		GltfPose(model, 0, 1.0 - 1e-4, world);
		GltfSkinRaw(model, world, p, n);
		CHECK(NearV(p[2], {-1, 0, 1}, 2e-3f));
		CHECK(NearV(p[0], {0, 0, 0})); // on the root, unmoved
		GltfPose(model, 0, .5, world);
		GltfSkinRaw(model, world, p, n);
		CHECK(NearV(p[2], {-std::sqrt(.5f), 0, 1 + std::sqrt(.5f)}, 2e-3f));
		// The clip loops: t = 2.5 is t = 0.5.
		std::vector<Vec3> q, m;
		GltfPose(model, 0, 2.5, world);
		GltfSkinRaw(model, world, q, m);
		CHECK(NearV(p[2], q[2], 2e-3f));
		// Played backwards (negative time) it still loops: t = -0.5 is t = 0.5 of a 1 s clip.
		GltfPose(model, 0, -.5, world);
		GltfSkinRaw(model, world, q, m);
		CHECK(NearV(p[2], q[2], 2e-3f));

		// Normalized like an OBJ: rest bounds centred, longest side 2.
		GltfSkin(model, -1, 0, p, n);
		Vec3 lo = p[0], hi = p[0];
		for (const Vec3& v : p) { lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)}; hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)}; }
		CHECK(Near(std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}), 2));
		CHECK(NearV((lo + hi) * .5f, {0, 0, 0}));
	}

	// A rigid mesh on an animated node, given as .gltf JSON with a base64 buffer and indices: a
	// translation channel moves the whole mesh; no normals -> flat face normals.
	{
		Bin bin;
		const size_t pos = bin.Floats({0, 0, 0, 1, 0, 0, 0, 1, 0});
		const size_t idx = bin.Bytes({0, 0, 1, 0, 2, 0, 0, 0}); // three u16 + padding
		const size_t times = bin.Floats({0, 2});
		const size_t moves = bin.Floats({0, 0, 0, 4, 0, 0});
		static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string b64;
		for (size_t i = 0; i < bin.bytes.size(); i += 3) {
			uint32_t v = uint32_t(bin.bytes[i]) << 16;
			if (i + 1 < bin.bytes.size()) v |= uint32_t(bin.bytes[i + 1]) << 8;
			if (i + 2 < bin.bytes.size()) v |= bin.bytes[i + 2];
			b64 += table[(v >> 18) & 63]; b64 += table[(v >> 12) & 63];
			b64 += i + 1 < bin.bytes.size() ? table[(v >> 6) & 63] : '=';
			b64 += i + 2 < bin.bytes.size() ? table[v & 63] : '=';
		}
		const std::string json = std::string("{\"asset\":{\"version\":\"2.0\"},") +
			"\"buffers\":[{\"byteLength\":" + std::to_string(bin.bytes.size()) + ",\"uri\":\"data:application/octet-stream;base64," + b64 + "\"}]," +
			"\"bufferViews\":[" + View(pos, 36) + "," + View(idx, 6) + "," + View(times, 8) + "," + View(moves, 24) + "]," +
			"\"accessors\":[" + Acc(0, 5126, 3, "VEC3") + "," + Acc(1, 5123, 3, "SCALAR") + "," + Acc(2, 5126, 2, "SCALAR") + "," + Acc(3, 5126, 2, "VEC3") + "]," +
			"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]," +
			"\"nodes\":[{\"mesh\":0}]," +
			"\"animations\":[{\"samplers\":[{\"input\":2,\"output\":3}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}]}]}";
		GltfModel model;
		std::string error;
		CHECK(ParseGltf(reinterpret_cast<const uint8_t*>(json.data()), json.size(), model, error));
		CHECK(model.vertices.size() == 3 && model.bones.size() == 1 && model.clips.size() == 1);
		CHECK(model.clips[0].name == "Clip 1");
		CHECK(NearV(model.vertices[0].normal, {0, 0, 1})); // flat face normal (glTF space)
		std::vector<GltfMat4> world;
		std::vector<Vec3> p, n;
		GltfPose(model, 0, 1, world); // halfway: moved 2 along glTF X = world X
		GltfSkinRaw(model, world, p, n);
		CHECK(NearV(p[1], {3, 0, 0}));
	}

	// Rejected cleanly: garbage, a GLB without JSON, a non-triangle-list primitive.
	{
		GltfModel model;
		std::string error;
		const std::string junk = "not json";
		CHECK(!ParseGltf(reinterpret_cast<const uint8_t*>(junk.data()), junk.size(), model, error) && !error.empty());
		const auto empty = Glb("", {});
		CHECK(!ParseGltf(empty.data(), empty.size(), model, error));
		Bin bin;
		const size_t pos = bin.Floats({0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0});
		const std::string json = std::string("{\"buffers\":[{\"byteLength\":48}],\"bufferViews\":[") + View(pos, 48) +
			"],\"accessors\":[" + Acc(0, 5126, 4, "VEC3") + "],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],\"nodes\":[{\"mesh\":0}]}";
		const auto glb = Glb(json, bin.bytes);
		CHECK(!ParseGltf(glb.data(), glb.size(), model, error) && error.find("triangle") != std::string::npos);
	}

	if (g_failed) { std::cerr << g_failed << " check(s) failed\n"; return 1; }
	std::cout << "environment gltf math tests passed\n";
	return 0;
}
