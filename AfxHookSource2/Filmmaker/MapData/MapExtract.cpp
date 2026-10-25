#include "MapExtract.h"
#include "MapCacheCodec.h"
#include "MapEntities.h"
#include "MapPhysics.h"
#include "MapSources.h"
#include "../Platform/JsonParser.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

namespace Filmmaker::MapData {
namespace {

namespace fs = std::filesystem;

bool FileStamp(const fs::path &file, uint64_t &size, uint64_t &writeTime) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) return false;
    size = uint64_t(data.nFileSizeHigh) << 32 | data.nFileSizeLow;
    writeTime = uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32 | data.ftLastWriteTime.dwLowDateTime;
    return true;
}

void WriteAtomic(const fs::path &path, const void *data, size_t size) {
    const fs::path tmp = path.wstring() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char *>(data), std::streamsize(size));
        if (!out) throw MapDataError("cannot write " + tmp.u8string());
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw MapDataError("cannot publish " + path.u8string());
}

std::string Publish(const fs::path &dir, const std::vector<uint8_t> &bytes, const char *extension) {
    const std::string digest = Sha256Bytes(bytes.data(), bytes.size());
    const fs::path path = dir / (digest + extension);
    std::error_code ec;
    if (!fs::exists(path, ec) || fs::file_size(path, ec) != bytes.size()) WriteAtomic(path, bytes.data(), bytes.size());
    return digest;
}

std::string Bounds(const std::array<float, 3> &low, const std::array<float, 3> &high) {
    return "[[" + JsonNumber(low[0]) + "," + JsonNumber(low[1]) + "," + JsonNumber(low[2]) + "],[" + JsonNumber(high[0]) + "," +
           JsonNumber(high[1]) + "," + JsonNumber(high[2]) + "]]";
}

std::string VisualJson(const CasterStats &s) {
    std::ostringstream o;
    o << "{\"batches\":" << s.batches << ",\"cutoutBatches\":" << s.cutoutBatches << ",\"stainedGlassBatches\":" << s.stainedGlassBatches
      << ",\"rigidVertices\":" << s.rigidVertices << ",\"rigidTriangles\":" << s.rigidTriangles << ",\"triangles\":" << s.triangles
      << ",\"bytes\":" << s.bytes << ",\"bounds\":" << Bounds(s.low, s.high) << "}";
    return o.str();
}

bool ReadManifest(const fs::path &path, JsonValue &doc) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size == 0 || size > 4 * 1024 * 1024) return false;
    std::ifstream in(path, std::ios::binary);
    std::string text(size_t(size), '\0');
    in.read(text.data(), std::streamsize(text.size()));
    return in && JsonParse(text, doc) && doc.type == JsonValue::Type::Object;
}

bool CurrentVersion(const JsonValue &doc) {
    const JsonValue *f = doc.Find("format"), *v = doc.Find("extractor");
    return f && v && f->AsInt() == kManifestFormat && v->AsInt() == kExtractorVersion;
}

} // namespace

ExtractSummary ExtractMap(const fs::path &cs2Root, const std::string &map, const fs::path &outDir, const ExtractOptions &options) {
    const auto progress = [&](const std::string &text) { if (options.progress) options.progress(text); };
    const auto checkCancel = [&] { if (options.cancel && options.cancel->load()) throw MapDataError("cancelled"); };
    ExtractSummary summary;
    progress("Locating map packages");
    MapSources sources = OpenMapSources(cs2Root, map, options.addon);
    fs::create_directories(outDir);

    progress("Reading authored lights and environment");
    const auto entities = ReadMapEntities(*sources.package);
    const std::string lights = BuildLightsJson(map, entities);
    JsonValue lightsDoc;
    if (JsonParse(lights, lightsDoc)) {
        if (const auto *l = lightsDoc.Find("lights")) summary.lights = l->arr.size();
        if (const auto *e = lightsDoc.Find("environment")) summary.environment = e->arr.size();
    }
    checkCancel();

    progress("Reading world collision");
    const OcclusionMesh collision = BuildOcclusionMesh(*sources.package, map);
    summary.collisionTriangles = collision.indices.size() / 3;
    checkCancel();

    progress("Reading world nodes");
    CasterProgress casterProgress{options.progress, options.cancel};
    const auto casters = BuildVisualCasters(*sources.package, sources.resources, summary.visual, summary.warnings, casterProgress);

    // The 3D skybox lives in its own package; follow the authored reference, never a guess.
    std::string skyboxJson;
    std::vector<uint8_t> skyCasters;
    CasterStats skyStats;
    std::vector<const MapEntity *> references;
    for (const MapEntity &e : entities)
        if (e.String("classname") == "skybox_reference") references.push_back(&e);
    if (references.size() > 1) summary.warnings.push_back("several skybox world groups; 3D skybox left out");
    std::string skyPackage, skyCameraJson;
    double skyScale = 0;
    if (references.size() == 1) {
        std::string target = references[0]->String("targetmapname");
        for (char &c : target) if (c == '\\') c = '/';
        const bool valid = target.size() > 10 && target.compare(0, 5, "maps/") == 0 && target.compare(target.size() - 5, 5, ".vmap") == 0 &&
                           target.find("..") == std::string::npos &&
                           target.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-/.") == std::string::npos;
        std::shared_ptr<Vpk> sky;
        if (valid) {
            skyPackage = target.substr(0, target.size() - 5) + ".vpk";
            sky = OpenReferencedPackage(sources, skyPackage);
        }
        if (!sky) {
            summary.warnings.push_back("3D skybox package not shipped (" + target + "); skybox casters skipped");
        } else {
            std::vector<const MapEntity *> cameras;
            const auto skyEntities = ReadMapEntities(*sky);
            for (const MapEntity &e : skyEntities)
                if (e.String("classname") == "sky_camera") cameras.push_back(&e);
            skyScale = cameras.size() == 1 ? cameras[0]->Float("scale", 0) : 0;
            if (cameras.size() != 1 || !(skyScale >= 1 && skyScale <= 1024)) {
                summary.warnings.push_back("skybox needs exactly one sky_camera with scale 1-1024; 3D skybox left out");
            } else {
                const auto origin = cameras[0]->Vector("origin");
                skyCameraJson = "[" + JsonNumber(origin[0]) + "," + JsonNumber(origin[1]) + "," + JsonNumber(origin[2]) + "]";
                PackageSet skyResources;
                skyResources.Add(sky);
                if (sources.addon) skyResources.Add(sources.addon);
                for (size_t k = sources.addon ? 2 : 1; k < sources.resources.Packages().size(); ++k) skyResources.Add(sources.resources.Packages()[k]);
                progress("Reading 3D skybox");
                try {
                    skyCasters = BuildVisualCasters(*sky, skyResources, skyStats, summary.warnings, casterProgress);
                } catch (const MapDataError &e) {
                    if (options.cancel && options.cancel->load()) throw;
                    summary.warnings.push_back(std::string("3D skybox left out: ") + e.what());
                }
            }
        }
    }
    checkCancel();

    // Sources must not have changed while extracting.
    std::ostringstream sourceJson;
    sourceJson << "[";
    for (size_t k = 0; k < sources.files.size(); ++k) {
        uint64_t size = 0, time = 0;
        if (!FileStamp(sources.files[k], size, time)) throw MapDataError("source file vanished: " + sources.files[k].u8string());
        sourceJson << (k ? "," : "") << "{\"path\":\"" << JsonEscape(sources.files[k].u8string()) << "\",\"size\":" << size
                   << ",\"writeTime\":\"" << time << "\"}";
    }
    sourceJson << "]";

    progress("Writing map data cache");
    const std::string lightsDigest = Publish(outDir, std::vector<uint8_t>(lights.begin(), lights.end()), ".lights.json");
    const std::string physicsDigest = Publish(outDir, CompressCachePayload(SerializeOcclusion(collision)), ".mvmocclusion");
    const std::string visualDigest = Publish(outDir, CompressCachePayload(casters), ".mvmcasters");
    if (!skyCasters.empty()) {
        summary.skybox = true;
        skyboxJson = "{\"package\":\"" + JsonEscape(skyPackage) + "\",\"visualDigest\":\"" + Publish(outDir, CompressCachePayload(skyCasters), ".mvmcasters") +
                     "\",\"cameraScale\":" + JsonNumber(skyScale) + ",\"cameraOrigin\":" + skyCameraJson + ",\"visual\":" + VisualJson(skyStats) + "}";
    }
    std::ostringstream excluded, warnings;
    for (size_t k = 0; k < collision.excluded.size(); ++k) excluded << (k ? "," : "") << "\"" << JsonEscape(collision.excluded[k]) << "\"";
    for (size_t k = 0; k < summary.warnings.size(); ++k) warnings << (k ? "," : "") << "\"" << JsonEscape(summary.warnings[k]) << "\"";
    std::ostringstream manifest;
    manifest << "{\"format\":" << kManifestFormat << ",\"extractor\":" << kExtractorVersion << ",\"map\":\"" << JsonEscape(map)
             << "\",\"mapDigest\":\"" << sources.mapDigest << "\",\"sources\":" << sourceJson.str() << ",\"lightsDigest\":\"" << lightsDigest
             << "\",\"physicsDigest\":\"" << physicsDigest << "\",\"visualDigest\":\"" << visualDigest << "\",\"geometry\":{\"vertices\":"
             << collision.vertices.size() << ",\"triangles\":" << collision.indices.size() / 3 << ",\"bounds\":" << Bounds(collision.low, collision.high)
             << ",\"excluded\":[" << excluded.str() << "]},\"visual\":" << VisualJson(summary.visual) << ",\"skyboxes\":["
             << skyboxJson << "],\"warnings\":[" << warnings.str() << "]}";
    const std::string text = manifest.str();
    summary.manifest = outDir / (map + ".json");
    WriteAtomic(summary.manifest, text.data(), text.size());
    return summary;
}

bool CacheCurrent(const fs::path &manifest, const std::string &map, const std::string &addon) {
    JsonValue doc;
    if (!ReadManifest(manifest, doc) || !CurrentVersion(doc)) return false;
    const JsonValue *name = doc.Find("map"), *files = doc.Find("sources");
    if (!name || name->AsString() != map || !files || files->type != JsonValue::Type::Array || files->arr.empty()) return false;
    bool fromAddon = addon.empty();
    for (const JsonValue &f : files->arr) {
        const JsonValue *path = f.Find("path"), *size = f.Find("size"), *time = f.Find("writeTime");
        if (!path || !size || !time) return false;
        const fs::path source = fs::u8path(path->AsString());
        uint64_t actualSize = 0, actualTime = 0;
        if (!FileStamp(source, actualSize, actualTime)) return false;
        if (double(actualSize) != size->AsNumber(-1) || std::to_string(actualTime) != time->AsString()) return false;
        // A Workshop item's package sits in workshop/content/730/<item>/.
        if (!fromAddon)
            fromAddon = source.parent_path().filename() == fs::u8path(addon) && source.parent_path().parent_path().filename() == L"730";
    }
    return fromAddon;
}

void PruneCache(const fs::path &outDir) {
    std::error_code ec;
    if (!fs::is_directory(outDir, ec)) return;
    std::set<std::string> referenced;
    std::vector<fs::path> payloads;
    for (const auto &item : fs::directory_iterator(outDir, ec)) {
        if (!item.is_regular_file()) continue;
        const std::string name = item.path().filename().u8string();
        const size_t dot = name.find('.');
        if (dot == 64 && name.find_first_not_of("0123456789abcdef") == 64) {
            payloads.push_back(item.path());
            continue;
        }
        if (item.path().extension() == L".tmp" || name == "prepare.log") {
            fs::remove(item.path(), ec);
            continue;
        }
        if (item.path().extension() != L".json") continue;
        JsonValue doc;
        if (!ReadManifest(item.path(), doc) || !CurrentVersion(doc)) {
            fs::remove(item.path(), ec);
            continue;
        }
        for (const char *key : {"lightsDigest", "physicsDigest", "visualDigest"})
            if (const JsonValue *v = doc.Find(key)) referenced.insert(v->AsString());
        if (const JsonValue *sky = doc.Find("skyboxes"))
            for (const JsonValue &s : sky->arr)
                if (const JsonValue *v = s.Find("visualDigest")) referenced.insert(v->AsString());
    }
    for (const fs::path &p : payloads)
        if (!referenced.count(p.filename().u8string().substr(0, 64))) fs::remove(p, ec);
}

} // namespace Filmmaker::MapData
