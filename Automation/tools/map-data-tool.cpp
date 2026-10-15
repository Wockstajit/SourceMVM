// MapDataTool: runs the hook's native map extractor outside the game, for checking a change
// against an earlier extraction and for preparing maps ahead of time.
//
//   MapDataTool extract <map> [--out <dir>] [--cs2 <root>]   write the lighting cache (default:
//                                                           %APPDATA%\HLAE\source_mvm\environment\occlusion)
//   MapDataTool entities <map> [--cs2 <root>]               print the authored lights document
//   MapDataTool lightmap <map> --out <file> [--cs2 <root>]  write the lightmap layout (MVMLMAP2)
//                                                           for `mirv_relight lightmap <file>`
#include "../../AfxHookSource2/Filmmaker/MapData/MapEntities.h"
#include "../../AfxHookSource2/Filmmaker/MapData/MapExtract.h"
#include "../../AfxHookSource2/Filmmaker/MapData/MapLightmap.h"
#include "../../AfxHookSource2/Filmmaker/MapData/MapSources.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace Filmmaker::MapData;

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: MapDataTool extract <map> [--out <dir>] [--cs2 <root>]\n       MapDataTool entities <map> [--cs2 <root>]\n"
                             "       MapDataTool lightmap <map> --out <file> [--cs2 <root>]\n");
        return 2;
    }
    const std::string command = argv[1], map = argv[2];
    std::filesystem::path cs2, out;
    for (int i = 3; i + 1 < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--cs2") cs2 = std::filesystem::u8path(argv[++i]);
        else if (option == "--out") out = std::filesystem::u8path(argv[++i]);
    }
    try {
        if (cs2.empty()) cs2 = ResolveCs2Root();
        if (cs2.empty()) throw MapDataError("Counter-Strike 2 not found; pass --cs2 <root>");
        const auto start = std::chrono::steady_clock::now();
        if (command == "entities") {
            MapSources sources = OpenMapSources(cs2, map);
            const std::string json = BuildLightsJson(map, ReadMapEntities(*sources.package));
            std::fwrite(json.data(), 1, json.size(), stdout);
        } else if (command == "extract") {
            if (out.empty()) {
                const char *appData = std::getenv("APPDATA");
                if (!appData) throw MapDataError("APPDATA not set; pass --out <dir>");
                out = std::filesystem::u8path(appData) / "HLAE" / "source_mvm" / "environment" / "occlusion";
            }
            ExtractOptions options;
            options.progress = [](const std::string &text) { std::fprintf(stderr, "  %s\n", text.c_str()); };
            const ExtractSummary s = ExtractMap(cs2, map, out, options);
            std::printf("%s: %zu lights, %zu environment entities, %zu collision triangles, %zu render triangles in %zu batches "
                        "(%zu cutout, %zu stained glass), skybox %s\n",
                        map.c_str(), s.lights, s.environment, s.collisionTriangles, s.visual.triangles, s.visual.batches,
                        s.visual.cutoutBatches, s.visual.stainedGlassBatches, s.skybox ? "yes" : "no");
            for (const auto &w : s.warnings) std::printf("  warning: %s\n", w.c_str());
            std::printf("  manifest: %s\n", s.manifest.u8string().c_str());
        } else if (command == "lightmap") {
            if (out.empty()) throw MapDataError("lightmap needs --out <file>");
            MapSources sources = OpenMapSources(cs2, map);
            LightmapStats stats;
            const auto bytes = BuildLightmapLayout(*sources.package, sources.resources, map, stats);
            std::ofstream file(out, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
            if (!file) throw MapDataError("cannot write " + out.u8string());
            std::printf("%s: %zu triangles (%zu vertices) and %zu texel points, atlas %dx%dx%d -> %s\n", map.c_str(), stats.triangles,
                        stats.vertices, stats.points, stats.atlasWidth, stats.atlasHeight, stats.atlasPages, out.u8string().c_str());
        } else {
            throw MapDataError("unknown command: " + command);
        }
        std::fprintf(stderr, "%s: %.1f s\n", map.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
