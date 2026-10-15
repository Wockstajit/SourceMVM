#pragma once

// glTF 2.0 meshes for Scene Objects > Your meshes: .glb (binary) and .gltf (JSON with data URIs
// or files beside it). Engine-free: parsing, the node hierarchy, skins, animation sampling and CPU
// skinning live here and are tested by Automation/tests/environment-gltf-math-tests.cpp. Image
// DECODING (PNG/JPEG bytes -> RGBA) and GPU work stay in the .cpp side (EnvironmentScene.cpp).
//
// The model is kept as a triangle list in its glTF space (Y up) with up to four bone influences
// per vertex. Every vertex is skinned, rigid ones too: a mesh on a plain node is bound to that node
// with weight 1 and an identity inverse bind, so a node animation moves it the same way a skeleton
// moves a skinned mesh. Pose() evaluates the hierarchy for a clip at a time; Skin() applies it,
// converts to the world's Z-up axes and normalizes the result the way OBJ meshes are (centred on
// the rest pose's bounds, longest side 2 units), so a GLB sizes like an OBJ.

#include "EnvironmentMath.h"
#include "../Platform/JsonParser.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Filmmaker::Environment {

// Column-major 4x4, as glTF stores matrices (m[col * 4 + row]).
struct GltfMat4 {
	float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};
inline GltfMat4 GltfMul(const GltfMat4& a, const GltfMat4& b) {
	GltfMat4 r;
	for (int c = 0; c < 4; ++c)
		for (int row = 0; row < 4; ++row) {
			float s = 0;
			for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
			r.m[c * 4 + row] = s;
		}
	return r;
}
inline Vec3 GltfPoint(const GltfMat4& a, Vec3 p) {
	return {a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z + a.m[12],
	        a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z + a.m[13],
	        a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z + a.m[14]};
}
inline Vec3 GltfDirection(const GltfMat4& a, Vec3 p) {
	return {a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z,
	        a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z,
	        a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z};
}
// Translation, rotation (unit quaternion x, y, z, w) and scale to a matrix: T * R * S.
inline GltfMat4 GltfTrs(Vec3 t, const std::array<float, 4>& q, Vec3 s) {
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	GltfMat4 r;
	r.m[0] = (1 - 2 * (y * y + z * z)) * s.x; r.m[1] = (2 * (x * y + z * w)) * s.x;     r.m[2] = (2 * (x * z - y * w)) * s.x;     r.m[3] = 0;
	r.m[4] = (2 * (x * y - z * w)) * s.y;     r.m[5] = (1 - 2 * (x * x + z * z)) * s.y; r.m[6] = (2 * (y * z + x * w)) * s.y;     r.m[7] = 0;
	r.m[8] = (2 * (x * z + y * w)) * s.z;     r.m[9] = (2 * (y * z - x * w)) * s.z;     r.m[10] = (1 - 2 * (x * x + y * y)) * s.z; r.m[11] = 0;
	r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z; r.m[15] = 1;
	return r;
}
inline std::array<float, 4> GltfNlerp(const std::array<float, 4>& a, std::array<float, 4> b, float t) {
	const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	if (dot < 0) for (float& v : b) v = -v; // the short way round
	std::array<float, 4> r{};
	float len = 0;
	for (int i = 0; i < 4; ++i) { r[i] = a[i] + (b[i] - a[i]) * t; len += r[i] * r[i]; }
	len = std::sqrt(len);
	if (len > 1e-8f) for (float& v : r) v /= len;
	else r = {0, 0, 0, 1};
	return r;
}

struct GltfVertex {
	Vec3 position, normal; // glTF space, bind pose (a rigid mesh: its node's own space)
	float u = 0, v = 0;
	uint16_t bones[4] = {0, 0, 0, 0};
	float weights[4] = {1, 0, 0, 0};
};
// One primitive: a run of the triangle list drawn with one material.
struct GltfPart {
	uint32_t first = 0, count = 0;
	int material = -1;
};
struct GltfMaterial {
	std::string name;
	float color[4] = {1, 1, 1, 1};
	float roughness = 1, metallic = 1;
	int albedoImage = -1, normalImage = -1, roughnessImage = -1; // images[] index, -1 = none
};
struct GltfImage {
	std::string mime;
	std::vector<uint8_t> bytes; // PNG or JPEG as stored; decoded by the caller
};
struct GltfNode {
	int parent = -1;
	Vec3 t{0, 0, 0}, s{1, 1, 1};
	std::array<float, 4> r{0, 0, 0, 1};
	bool hasMatrix = false; // a node given as a matrix is not animatable
	GltfMat4 matrix;
};
// A bone of the unified skeleton: a node and its inverse bind (identity for rigid meshes).
struct GltfBone {
	int node = 0;
	GltfMat4 inverseBind;
};
struct GltfChannel {
	int node = 0;
	int path = 0;   // 0 translation, 1 rotation, 2 scale
	bool step = false;
	std::vector<float> times, values; // values: 3 or 4 per key
};
struct GltfClip {
	std::string name;
	float duration = 0;
	std::vector<GltfChannel> channels;
};
struct GltfModel {
	std::vector<GltfVertex> vertices; // triangle list
	std::vector<GltfPart> parts;
	std::vector<GltfMaterial> materials;
	std::vector<GltfImage> images;
	std::vector<GltfNode> nodes;
	std::vector<int> order;           // nodes, parents before children
	std::vector<GltfBone> bones;
	std::vector<GltfClip> clips;
	// Rest-pose normalization, applied after the Z-up conversion: (p - center) * scale.
	Vec3 center{0, 0, 0};
	float scale = 1;
};

constexpr size_t kGltfMaxTriangles = 100000;
constexpr size_t kGltfMaxBones = 1024;
constexpr size_t kGltfMaxClips = 64;

// glTF is Y up with +Z toward the viewer; the world is Z up. (x, y, z) -> (x, -z, y) keeps the
// handedness (determinant +1) and stands the model upright.
inline Vec3 GltfToWorld(Vec3 p) { return {p.x, -p.z, p.y}; }

namespace gltf_detail {

inline bool Base64Decode(const std::string& in, std::vector<uint8_t>& out) {
	auto value = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '+' || c == '-') return 62;
		if (c == '/' || c == '_') return 63;
		return -1;
	};
	out.clear();
	uint32_t acc = 0;
	int bits = 0;
	for (char c : in) {
		if (c == '=') break;
		const int v = value(c);
		if (v < 0) { if (c == '\r' || c == '\n' || c == ' ') continue; return false; }
		acc = (acc << 6) | uint32_t(v);
		bits += 6;
		if (bits >= 8) { bits -= 8; out.push_back(uint8_t((acc >> bits) & 0xff)); }
	}
	return true;
}

struct Accessor {
	const uint8_t* base = nullptr;
	size_t count = 0, stride = 0;
	int components = 0, type = 0;
	bool normalized = false;
	float Get(size_t i, int c) const {
		const uint8_t* p = base + i * stride;
		switch (type) {
		case 5126: { float f; std::memcpy(&f, p + c * 4, 4); return f; }
		case 5121: { const uint8_t v = p[c]; return normalized ? v / 255.0f : float(v); }
		case 5120: { const int8_t v = int8_t(p[c]); return normalized ? std::max(v / 127.0f, -1.0f) : float(v); }
		case 5123: { uint16_t v; std::memcpy(&v, p + c * 2, 2); return normalized ? v / 65535.0f : float(v); }
		case 5122: { int16_t v; std::memcpy(&v, p + c * 2, 2); return normalized ? std::max(v / 32767.0f, -1.0f) : float(v); }
		case 5125: { uint32_t v; std::memcpy(&v, p + c * 4, 4); return float(v); }
		}
		return 0;
	}
	uint32_t Index(size_t i) const {
		const uint8_t* p = base + i * stride;
		if (type == 5121) return p[0];
		if (type == 5123) { uint16_t v; std::memcpy(&v, p, 2); return v; }
		uint32_t v; std::memcpy(&v, p, 4); return v;
	}
};

inline int TypeComponents(const std::string& t) {
	if (t == "SCALAR") return 1;
	if (t == "VEC2") return 2;
	if (t == "VEC3") return 3;
	if (t == "VEC4") return 4;
	if (t == "MAT4") return 16;
	return 0;
}
inline int ComponentBytes(int type) {
	switch (type) { case 5120: case 5121: return 1; case 5122: case 5123: return 2; case 5125: case 5126: return 4; }
	return 0;
}
inline const JsonValue* At(const JsonValue* array, int i) {
	if (!array || array->type != JsonValue::Type::Array || i < 0 || i >= int(array->arr.size())) return nullptr;
	return &array->arr[size_t(i)];
}
inline int Int(const JsonValue* o, const char* key, int def = -1) {
	const JsonValue* v = o ? o->Find(key) : nullptr;
	return v && v->type == JsonValue::Type::Number ? int(v->number) : def;
}
inline float Num(const JsonValue* o, const char* key, float def) {
	const JsonValue* v = o ? o->Find(key) : nullptr;
	return v && v->type == JsonValue::Type::Number ? float(v->number) : def;
}

} // namespace gltf_detail

inline void GltfNormalizeToRest(GltfModel& model); // below: needs Pose and Skin

// Reads a file the .gltf names (an image or a .bin beside it). Absent for a .glb.
using GltfFileReader = std::function<bool(const std::string& uri, std::vector<uint8_t>& out)>;

// Parses .glb bytes (magic "glTF") or .gltf JSON text. False with `error` on anything it cannot
// read faithfully (sparse accessors, non-triangle primitives, out-of-range data, limits).
inline bool ParseGltf(const uint8_t* data, size_t size, GltfModel& out, std::string& error,
                      const GltfFileReader& readFile = nullptr) {
	using namespace gltf_detail;
	out = GltfModel{};
	auto fail = [&](const std::string& message) { error = message; return false; };
	std::string json;
	std::vector<uint8_t> glbBin;
	bool haveGlbBin = false;
	if (size >= 12 && std::memcmp(data, "glTF", 4) == 0) {
		uint32_t version = 0, length = 0;
		std::memcpy(&version, data + 4, 4);
		std::memcpy(&length, data + 8, 4);
		if (version != 2) return fail("Only glTF 2.0 is supported");
		if (length > size) return fail("Truncated GLB");
		size_t at = 12;
		while (at + 8 <= length) {
			uint32_t chunkLength = 0, chunkType = 0;
			std::memcpy(&chunkLength, data + at, 4);
			std::memcpy(&chunkType, data + at + 4, 4);
			at += 8;
			if (at + chunkLength > length) return fail("Truncated GLB chunk");
			if (chunkType == 0x4E4F534A) json.assign(reinterpret_cast<const char*>(data + at), chunkLength);      // JSON
			else if (chunkType == 0x004E4942) { glbBin.assign(data + at, data + at + chunkLength); haveGlbBin = true; } // BIN
			at += (chunkLength + 3) & ~size_t(3);
		}
		if (json.empty()) return fail("GLB has no JSON chunk");
	} else {
		json.assign(reinterpret_cast<const char*>(data), size);
	}
	JsonValue root;
	if (!JsonParse(json, root) || root.type != JsonValue::Type::Object) return fail("Malformed glTF JSON");
	if (const JsonValue* asset = root.Find("asset")) {
		const std::string version = asset->Find("version") ? asset->Find("version")->AsString() : "";
		if (!version.empty() && version[0] != '2') return fail("Only glTF 2.0 is supported");
	}

	// Buffers.
	std::vector<std::vector<uint8_t>> buffers;
	if (const JsonValue* list = root.Find("buffers")) {
		for (size_t i = 0; i < list->arr.size(); ++i) {
			const JsonValue& b = list->arr[i];
			const JsonValue* uri = b.Find("uri");
			std::vector<uint8_t> bytes;
			if (!uri) {
				if (!haveGlbBin) return fail("Buffer without data");
				bytes = glbBin;
			} else {
				const std::string u = uri->AsString();
				const size_t comma = u.find(',');
				if (u.compare(0, 5, "data:") == 0 && comma != std::string::npos) {
					if (!Base64Decode(u.substr(comma + 1), bytes)) return fail("Bad base64 buffer");
				} else if (!readFile || !readFile(u, bytes)) {
					return fail("Missing buffer file: " + u);
				}
			}
			const int declared = Int(&b, "byteLength", 0);
			if (declared < 0 || size_t(declared) > bytes.size()) return fail("Buffer shorter than declared");
			buffers.push_back(std::move(bytes));
		}
	}
	const JsonValue* views = root.Find("bufferViews");
	const JsonValue* accessors = root.Find("accessors");
	auto bufferView = [&](int index, const uint8_t*& base, size_t& length, size_t& stride) {
		const JsonValue* v = At(views, index);
		if (!v) return false;
		const int buffer = Int(v, "buffer", -1), offset = Int(v, "byteOffset", 0), len = Int(v, "byteLength", -1);
		if (buffer < 0 || buffer >= int(buffers.size()) || offset < 0 || len < 0 ||
		    size_t(offset) + size_t(len) > buffers[size_t(buffer)].size()) return false;
		base = buffers[size_t(buffer)].data() + offset;
		length = size_t(len);
		stride = size_t(Int(v, "byteStride", 0));
		return true;
	};
	auto accessor = [&](int index, Accessor& a, int wantComponents) {
		const JsonValue* j = At(accessors, index);
		if (!j || j->Find("sparse")) return false;
		a.type = Int(j, "componentType", 0);
		a.count = size_t(std::max(0, Int(j, "count", 0)));
		a.components = TypeComponents(j->Find("type") ? j->Find("type")->AsString() : "");
		a.normalized = j->Find("normalized") && j->Find("normalized")->AsBool(false);
		if (wantComponents && a.components != wantComponents) return false;
		const int bytes = ComponentBytes(a.type);
		if (!bytes || !a.components) return false;
		const uint8_t* base = nullptr;
		size_t length = 0, stride = 0;
		if (!bufferView(Int(j, "bufferView", -1), base, length, stride)) return false;
		const size_t element = size_t(bytes) * size_t(a.components);
		a.stride = stride ? stride : element;
		const size_t offset = size_t(std::max(0, Int(j, "byteOffset", 0)));
		if (a.count && offset + (a.count - 1) * a.stride + element > length) return false;
		a.base = base + offset;
		return true;
	};

	// Images: bytes only (decoded later).
	if (const JsonValue* list = root.Find("images")) {
		for (const JsonValue& img : list->arr) {
			GltfImage image;
			image.mime = img.Find("mimeType") ? img.Find("mimeType")->AsString() : "";
			if (const JsonValue* uri = img.Find("uri")) {
				const std::string u = uri->AsString();
				const size_t comma = u.find(',');
				if (u.compare(0, 5, "data:") == 0 && comma != std::string::npos) Base64Decode(u.substr(comma + 1), image.bytes);
				else if (readFile) readFile(u, image.bytes);
			} else {
				const uint8_t* base = nullptr;
				size_t length = 0, stride = 0;
				if (bufferView(Int(&img, "bufferView", -1), base, length, stride)) image.bytes.assign(base, base + length);
			}
			out.images.push_back(std::move(image)); // an unreadable one stays empty: the part draws untextured
		}
	}
	const JsonValue* textures = root.Find("textures");
	auto textureImage = [&](const JsonValue* info) {
		const JsonValue* t = At(textures, Int(info, "index", -1));
		const int source = Int(t, "source", -1);
		return source >= 0 && source < int(out.images.size()) ? source : -1;
	};
	if (const JsonValue* list = root.Find("materials")) {
		for (const JsonValue& m : list->arr) {
			GltfMaterial mat;
			mat.name = m.Find("name") ? m.Find("name")->AsString() : "";
			if (const JsonValue* pbr = m.Find("pbrMetallicRoughness")) {
				if (const JsonValue* c = pbr->Find("baseColorFactor"))
					for (int i = 0; i < 4 && i < int(c->arr.size()); ++i) mat.color[i] = float(c->arr[size_t(i)].AsNumber(1));
				mat.roughness = Num(pbr, "roughnessFactor", 1);
				mat.metallic = Num(pbr, "metallicFactor", 1);
				mat.albedoImage = textureImage(pbr->Find("baseColorTexture"));
				mat.roughnessImage = textureImage(pbr->Find("metallicRoughnessTexture"));
			}
			mat.normalImage = textureImage(m.Find("normalTexture"));
			out.materials.push_back(mat);
		}
	}

	// Nodes and the hierarchy.
	const JsonValue* nodes = root.Find("nodes");
	const size_t nodeCount = nodes ? nodes->arr.size() : 0;
	out.nodes.resize(nodeCount);
	for (size_t i = 0; i < nodeCount; ++i) {
		const JsonValue& n = nodes->arr[i];
		GltfNode& node = out.nodes[i];
		if (const JsonValue* m = n.Find("matrix")) {
			if (m->arr.size() != 16) return fail("Bad node matrix");
			node.hasMatrix = true;
			for (int k = 0; k < 16; ++k) node.matrix.m[k] = float(m->arr[size_t(k)].AsNumber(0));
		}
		if (const JsonValue* t = n.Find("translation"); t && t->arr.size() == 3)
			node.t = {float(t->arr[0].AsNumber(0)), float(t->arr[1].AsNumber(0)), float(t->arr[2].AsNumber(0))};
		if (const JsonValue* r = n.Find("rotation"); r && r->arr.size() == 4)
			for (int k = 0; k < 4; ++k) node.r[size_t(k)] = float(r->arr[size_t(k)].AsNumber(k == 3 ? 1 : 0));
		if (const JsonValue* s = n.Find("scale"); s && s->arr.size() == 3)
			node.s = {float(s->arr[0].AsNumber(1)), float(s->arr[1].AsNumber(1)), float(s->arr[2].AsNumber(1))};
		if (const JsonValue* children = n.Find("children"))
			for (const JsonValue& c : children->arr) {
				const int child = c.AsInt(-1);
				if (child < 0 || child >= int(nodeCount) || child == int(i) || out.nodes[size_t(child)].parent >= 0)
					return fail("Bad node hierarchy");
				out.nodes[size_t(child)].parent = int(i);
			}
	}
	// Parents before children (a cycle leaves nodes out and is rejected).
	{
		std::vector<std::vector<int>> kids(nodeCount);
		for (size_t i = 0; i < nodeCount; ++i) if (out.nodes[i].parent >= 0) kids[size_t(out.nodes[i].parent)].push_back(int(i));
		for (size_t i = 0; i < nodeCount; ++i)
			if (out.nodes[i].parent < 0) {
				std::vector<int> stack{int(i)};
				while (!stack.empty()) {
					const int n = stack.back(); stack.pop_back();
					out.order.push_back(n);
					for (int k : kids[size_t(n)]) stack.push_back(k);
				}
			}
		if (out.order.size() != nodeCount) return fail("Bad node hierarchy");
	}

	// Skins: their joints become bones (one bone per skin joint).
	const JsonValue* skins = root.Find("skins");
	std::vector<std::vector<uint16_t>> skinBones; // per skin: joint slot -> bone index
	if (skins) {
		for (const JsonValue& sk : skins->arr) {
			const JsonValue* joints = sk.Find("joints");
			if (!joints) return fail("Skin without joints");
			Accessor ibm;
			const bool haveIbm = sk.Find("inverseBindMatrices") && accessor(Int(&sk, "inverseBindMatrices", -1), ibm, 16);
			if (sk.Find("inverseBindMatrices") && (!haveIbm || ibm.count < joints->arr.size())) return fail("Bad inverse bind matrices");
			std::vector<uint16_t> map;
			for (size_t j = 0; j < joints->arr.size(); ++j) {
				const int node = joints->arr[j].AsInt(-1);
				if (node < 0 || node >= int(nodeCount)) return fail("Bad skin joint");
				if (out.bones.size() >= kGltfMaxBones) return fail("Too many bones");
				GltfBone bone;
				bone.node = node;
				if (haveIbm) for (int k = 0; k < 16; ++k) bone.inverseBind.m[k] = ibm.Get(j, k);
				map.push_back(uint16_t(out.bones.size()));
				out.bones.push_back(bone);
			}
			skinBones.push_back(std::move(map));
		}
	}

	// Meshes, through the nodes that place them.
	const JsonValue* meshes = root.Find("meshes");
	for (int nodeIndex : out.order) {
		const JsonValue& n = nodes->arr[size_t(nodeIndex)];
		const JsonValue* mesh = At(meshes, Int(&n, "mesh", -1));
		if (!mesh) continue;
		const int skin = Int(&n, "skin", -1);
		if (skin >= int(skinBones.size())) return fail("Bad skin index");
		uint16_t rigidBone = 0;
		if (skin < 0) { // a rigid mesh rides its node
			if (out.bones.size() >= kGltfMaxBones) return fail("Too many bones");
			GltfBone bone;
			bone.node = nodeIndex;
			rigidBone = uint16_t(out.bones.size());
			out.bones.push_back(bone);
		}
		const JsonValue* primitives = mesh->Find("primitives");
		if (!primitives) continue;
		for (const JsonValue& prim : primitives->arr) {
			if (Int(&prim, "mode", 4) != 4) continue; // points and lines are not surfaces
			const JsonValue* attributes = prim.Find("attributes");
			Accessor pos, nrm, uv, joints, weights, idx;
			if (!attributes || !accessor(Int(attributes, "POSITION", -1), pos, 3)) return fail("Primitive without positions");
			const bool haveNormals = attributes->Find("NORMAL") && accessor(Int(attributes, "NORMAL", -1), nrm, 3) && nrm.count == pos.count;
			const bool haveUv = attributes->Find("TEXCOORD_0") && accessor(Int(attributes, "TEXCOORD_0", -1), uv, 2) && uv.count == pos.count;
			const bool skinned = skin >= 0 && attributes->Find("JOINTS_0") && attributes->Find("WEIGHTS_0") &&
				accessor(Int(attributes, "JOINTS_0", -1), joints, 4) && accessor(Int(attributes, "WEIGHTS_0", -1), weights, 4) &&
				joints.count == pos.count && weights.count == pos.count;
			const bool indexed = prim.Find("indices") != nullptr;
			if (indexed && !accessor(Int(&prim, "indices", -1), idx, 1)) return fail("Bad primitive indices");
			const size_t corners = indexed ? idx.count : pos.count;
			if (corners % 3) return fail("Primitive is not a triangle list");
			if ((out.vertices.size() + corners) / 3 > kGltfMaxTriangles) return fail("More than 100,000 triangles");
			GltfPart part;
			part.first = uint32_t(out.vertices.size());
			part.count = uint32_t(corners);
			part.material = Int(&prim, "material", -1);
			if (part.material >= int(out.materials.size())) part.material = -1;
			for (size_t c = 0; c < corners; c += 3) {
				GltfVertex tri[3];
				for (int k = 0; k < 3; ++k) {
					const uint32_t i = indexed ? idx.Index(c + size_t(k)) : uint32_t(c + size_t(k));
					if (i >= pos.count) return fail("Index out of range");
					GltfVertex& v = tri[k];
					v.position = {pos.Get(i, 0), pos.Get(i, 1), pos.Get(i, 2)};
					if (!Finite(v.position)) return fail("Non-finite position");
					if (haveNormals) v.normal = {nrm.Get(i, 0), nrm.Get(i, 1), nrm.Get(i, 2)};
					if (haveUv) { v.u = uv.Get(i, 0); v.v = uv.Get(i, 1); } // glTF UV origin is already top-left
					if (skinned) {
						float total = 0;
						for (int b = 0; b < 4; ++b) {
							const int slot = int(joints.Get(i, b));
							v.weights[b] = std::max(0.0f, weights.Get(i, b));
							if (slot < 0 || slot >= int(skinBones[size_t(skin)].size())) { v.weights[b] = 0; v.bones[b] = 0; }
							else v.bones[b] = skinBones[size_t(skin)][size_t(slot)];
							total += v.weights[b];
						}
						if (total > 1e-6f) for (float& w : v.weights) w /= total;
						else { v.weights[0] = 1; v.weights[1] = v.weights[2] = v.weights[3] = 0; }
					} else {
						v.bones[0] = rigidBone; v.weights[0] = 1;
					}
				}
				if (!haveNormals) { // flat normals from the face
					const Vec3 face = Normalize(Cross(tri[1].position - tri[0].position, tri[2].position - tri[0].position));
					for (GltfVertex& v : tri) v.normal = face;
				}
				for (const GltfVertex& v : tri) out.vertices.push_back(v);
			}
			if (part.count) out.parts.push_back(part);
		}
	}
	if (out.vertices.empty()) return fail("No triangles in the glTF scene");

	// Animations: translation / rotation / scale channels on nodes (morph weights are skipped).
	if (const JsonValue* animations = root.Find("animations")) {
		for (size_t a = 0; a < animations->arr.size() && out.clips.size() < kGltfMaxClips; ++a) {
			const JsonValue& anim = animations->arr[a];
			GltfClip clip;
			clip.name = anim.Find("name") ? anim.Find("name")->AsString() : "";
			if (clip.name.empty()) clip.name = "Clip " + std::to_string(a + 1);
			const JsonValue* samplers = anim.Find("samplers");
			if (const JsonValue* channels = anim.Find("channels"))
				for (const JsonValue& ch : channels->arr) {
					const JsonValue* target = ch.Find("target");
					const std::string path = target && target->Find("path") ? target->Find("path")->AsString() : "";
					const int node = Int(target, "node", -1);
					const int kind = path == "translation" ? 0 : path == "rotation" ? 1 : path == "scale" ? 2 : -1;
					const JsonValue* sampler = At(samplers, Int(&ch, "sampler", -1));
					if (kind < 0 || node < 0 || node >= int(nodeCount) || !sampler) continue;
					Accessor input, output;
					const int width = kind == 1 ? 4 : 3;
					if (!accessor(Int(sampler, "input", -1), input, 1) || !accessor(Int(sampler, "output", -1), output, width)) continue;
					const std::string interp = sampler->Find("interpolation") ? sampler->Find("interpolation")->AsString() : "LINEAR";
					const bool cubic = interp == "CUBICSPLINE";
					if (output.count != input.count * (cubic ? 3 : 1) || !input.count) continue;
					GltfChannel channel;
					channel.node = node;
					channel.path = kind;
					channel.step = interp == "STEP";
					for (size_t k = 0; k < input.count; ++k) {
						channel.times.push_back(input.Get(k, 0));
						const size_t row = cubic ? k * 3 + 1 : k; // a cubic key's value sits between its tangents
						for (int c = 0; c < width; ++c) channel.values.push_back(output.Get(row, c));
					}
					clip.duration = std::max(clip.duration, channel.times.back());
					clip.channels.push_back(std::move(channel));
				}
			if (!clip.channels.empty()) out.clips.push_back(std::move(clip));
		}
	}

	// Rest-pose bounds in world axes: the normalization every pose keeps.
	GltfNormalizeToRest(out);
	error.clear();
	return true;
}

// World matrices of every node for a clip at `time` seconds (looping); clip < 0 = rest pose.
inline void GltfPose(const GltfModel& model, int clip, double time, std::vector<GltfMat4>& world) {
	std::vector<Vec3> t(model.nodes.size()), s(model.nodes.size());
	std::vector<std::array<float, 4>> r(model.nodes.size());
	for (size_t i = 0; i < model.nodes.size(); ++i) { t[i] = model.nodes[i].t; r[i] = model.nodes[i].r; s[i] = model.nodes[i].s; }
	if (clip >= 0 && clip < int(model.clips.size())) {
		const GltfClip& c = model.clips[size_t(clip)];
		double wrapped = c.duration > 0 ? std::fmod(time, double(c.duration)) : 0.0;
		if (wrapped < 0) wrapped += c.duration; // a negative speed plays it backwards, still looping
		const float at = float(wrapped);
		for (const GltfChannel& ch : c.channels) {
			const size_t keys = ch.times.size();
			const int width = ch.path == 1 ? 4 : 3;
			size_t k1 = 0;
			while (k1 < keys && ch.times[k1] < at) ++k1;
			size_t k0 = k1 ? k1 - 1 : 0;
			if (k1 >= keys) k1 = k0 = keys - 1;
			float f = 0;
			if (k1 != k0 && !ch.step) {
				const float span = ch.times[k1] - ch.times[k0];
				f = span > 1e-6f ? (at - ch.times[k0]) / span : 0;
			}
			if (ch.step) k1 = k0;
			const float* a = &ch.values[k0 * size_t(width)];
			const float* b = &ch.values[k1 * size_t(width)];
			const size_t n = size_t(ch.node);
			if (ch.path == 1) {
				r[n] = GltfNlerp({a[0], a[1], a[2], a[3]}, {b[0], b[1], b[2], b[3]}, f);
			} else {
				const Vec3 v{a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f};
				(ch.path == 0 ? t : s)[n] = v;
			}
		}
	}
	world.assign(model.nodes.size(), GltfMat4{});
	for (int n : model.order) {
		const GltfNode& node = model.nodes[size_t(n)];
		const GltfMat4 local = node.hasMatrix ? node.matrix : GltfTrs(t[size_t(n)], r[size_t(n)], s[size_t(n)]);
		world[size_t(n)] = node.parent >= 0 ? GltfMul(world[size_t(node.parent)], local) : local;
	}
}

// The triangle list posed by `world`, in world axes, UNnormalized (positions and unit normals).
inline void GltfSkinRaw(const GltfModel& model, const std::vector<GltfMat4>& world,
                        std::vector<Vec3>& positions, std::vector<Vec3>& normals) {
	std::vector<GltfMat4> skin(model.bones.size());
	for (size_t b = 0; b < model.bones.size(); ++b)
		skin[b] = GltfMul(world[size_t(model.bones[b].node)], model.bones[b].inverseBind);
	positions.resize(model.vertices.size());
	normals.resize(model.vertices.size());
	for (size_t i = 0; i < model.vertices.size(); ++i) {
		const GltfVertex& v = model.vertices[i];
		Vec3 p{0, 0, 0}, n{0, 0, 0};
		for (int k = 0; k < 4; ++k) {
			const float w = v.weights[k];
			if (w <= 0) continue;
			const GltfMat4& m = skin[v.bones[k]];
			p = p + GltfPoint(m, v.position) * w;
			n = n + GltfDirection(m, v.normal) * w;
		}
		positions[i] = GltfToWorld(p);
		const Vec3 wn = GltfToWorld(n);
		normals[i] = Dot(wn, wn) > 1e-12f ? Normalize(wn) : Vec3{0, 0, 1};
	}
}

// Sets the normalization from the rest pose: centred on its bounds, longest side 2 units.
inline void GltfNormalizeToRest(GltfModel& model) {
	std::vector<GltfMat4> world;
	std::vector<Vec3> p, n;
	GltfPose(model, -1, 0, world);
	GltfSkinRaw(model, world, p, n);
	Vec3 lo = p.empty() ? Vec3{0, 0, 0} : p[0], hi = lo;
	for (const Vec3& q : p) {
		lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
		hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
	}
	model.center = (lo + hi) * .5f;
	const float extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
	model.scale = extent > 1e-6f ? 2 / extent : 1;
}

// The posed, normalized triangle list: what a placed mesh draws for a clip at a time.
inline void GltfSkin(const GltfModel& model, int clip, double time, std::vector<Vec3>& positions, std::vector<Vec3>& normals) {
	std::vector<GltfMat4> world;
	GltfPose(model, clip, time, world);
	GltfSkinRaw(model, world, positions, normals);
	for (Vec3& p : positions) p = (p - model.center) * model.scale;
}

inline int GltfClipIndex(const GltfModel& model, const std::string& name) {
	for (size_t i = 0; i < model.clips.size(); ++i) if (model.clips[i].name == name) return int(i);
	return -1;
}

} // namespace Filmmaker::Environment
