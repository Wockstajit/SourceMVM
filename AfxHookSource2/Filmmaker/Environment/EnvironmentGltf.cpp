// Scene Objects > Your meshes: a .glb / .gltf as a placed Environment mesh, with its embedded
// textures and its animations. The parsing, skeleton and skinning maths are engine-free in
// EnvironmentGltfMath.h; this file reads the file, decodes the images (WIC) and builds the Mesh the
// renderer draws: one part per glTF primitive with that primitive's material, plus the rig, which
// CaptureFrame (EnvironmentScene.cpp) poses each frame for the object's chosen clip.
#include "EnvironmentScene.h"
#include "EnvironmentGltfMath.h"
#include "../Debug/MvmDebugLog.h"

#include <filesystem>
#include <fstream>
#include <map>

namespace Filmmaker::Environment {

namespace {

bool ReadAll(const std::filesystem::path& path, std::vector<uint8_t>& out, size_t limit) {
	std::error_code ec;
	const auto size = std::filesystem::file_size(path, ec);
	if (ec || size > limit) return false;
	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	out.resize(size_t(size));
	in.read(reinterpret_cast<char*>(out.data()), std::streamsize(size));
	return size_t(in.gcount()) == size_t(size);
}

// A file a .gltf names beside itself. Only plain relative names inside its own folder: no
// parent-directory or absolute paths, so a scene file cannot read anything else on the disk.
bool ReadSide(const std::filesystem::path& folder, const std::string& uri, std::vector<uint8_t>& out) {
	if (uri.empty() || uri.find("..") != std::string::npos || uri.find(':') != std::string::npos ||
	    uri[0] == '/' || uri[0] == '\\')
		return false;
	std::string decoded; // %20 and friends
	for (size_t i = 0; i < uri.size(); ++i) {
		if (uri[i] == '%' && i + 2 < uri.size()) {
			decoded += char(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
			i += 2;
		} else decoded += uri[i];
	}
	return ReadAll(folder / std::filesystem::u8path(decoded), out, 64u * 1024 * 1024);
}

} // namespace

bool LoadGltfMesh(const std::filesystem::path& path, Mesh& mesh, std::string& error) {
	std::vector<uint8_t> bytes;
	if (!ReadAll(path, bytes, 128u * 1024 * 1024)) { error = "Missing or larger than 128 MB"; return false; }
	auto model = std::make_shared<GltfModel>();
	const auto folder = path.parent_path();
	if (!ParseGltf(bytes.data(), bytes.size(), *model, error,
	               [&](const std::string& uri, std::vector<uint8_t>& out) { return ReadSide(folder, uri, out); }))
		return false;

	// Each image once, shared by every part that uses it. One that will not decode leaves those
	// parts untextured rather than failing the mesh.
	std::map<int, std::shared_ptr<const MaterialImage>> decoded;
	size_t residentBytes = 0;
	auto image = [&](int index) -> std::shared_ptr<const MaterialImage> {
		if (index < 0 || index >= int(model->images.size())) return nullptr;
		auto found = decoded.find(index);
		if (found != decoded.end()) return found->second;
		std::shared_ptr<const MaterialImage> result;
		const auto& source = model->images[size_t(index)].bytes;
		auto loaded = std::make_shared<MaterialImage>();
		std::string imageError;
		if (!source.empty() && LoadMaterialImageFromMemory(source.data(), source.size(), *loaded, imageError) &&
		    residentBytes + loaded->rgba.size() <= 256u * 1024 * 1024) {
			residentBytes += loaded->rgba.size();
			result = loaded;
		} else {
			MvmDebugLog_LinefAlways("environment.gltf", "%s: image %d not used (%s)", path.filename().string().c_str(),
			                        index, imageError.empty() ? "empty or over the 256 MB budget" : imageError.c_str());
		}
		decoded[index] = result;
		return result;
	};

	std::vector<Vec3> positions, normals;
	GltfSkin(*model, -1, 0, positions, normals); // the rest pose
	mesh.vertices.resize(model->vertices.size());
	for (size_t i = 0; i < model->vertices.size(); ++i) {
		Vertex& v = mesh.vertices[i];
		v.position = positions[i];
		v.normal = normals[i];
		v.u = model->vertices[i].u;
		v.v = model->vertices[i].v;
	}
	mesh.parts.clear();
	for (const GltfPart& p : model->parts) {
		MeshPart part;
		part.first = p.first;
		part.count = p.count;
		if (p.material >= 0) {
			const GltfMaterial& m = model->materials[size_t(p.material)];
			part.color = {m.color[0], m.color[1], m.color[2]};
			part.roughness = std::clamp(m.roughness, .05f, 1.f);
			part.metallic = std::clamp(m.metallic, 0.f, 1.f);
			part.albedo = image(m.albedoImage);
			part.normal = image(m.normalImage);
			// glTF's metallicRoughness map packs roughness in G and metal in B; the material
			// shader reads a single roughness channel, so the factor is used instead.
		}
		mesh.parts.push_back(std::move(part));
	}
	// The rig rides along only when there is something to play; the bind data can be large.
	if (!model->clips.empty()) mesh.rig = model;
	MvmDebugLog_LinefAlways("environment.gltf", "%s: %zu triangles, %zu parts, %zu images, %zu bones, %zu clips",
	                        path.filename().string().c_str(), model->vertices.size() / 3, mesh.parts.size(),
	                        decoded.size(), model->bones.size(), model->clips.size());
	error.clear();
	return true;
}

} // namespace Filmmaker::Environment
