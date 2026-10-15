// Ported from ValveResourceFormat 20.0 (MIT): GltfModelExporter (world nodes, models, draw
// calls, materials), plus the caster rules of the former VisualShadowConverter.
#include "MapCasters.h"
#include "MapCasterLimits.h"
#include "MapGeometry.h"
#include "MapMeshes.h"
#include "MapTextures.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>

namespace Filmmaker::MapData {
namespace {

using Vec4 = std::array<float, 4>;

std::string FileStem(const std::string &path) {
    const size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string FileName(const std::string &path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool ContainsNoCase(const std::string &text, const char *needle) {
    std::string a = text, b = needle;
    for (char &c : a) c = char(tolower(static_cast<unsigned char>(c)));
    for (char &c : b) c = char(tolower(static_cast<unsigned char>(c)));
    return a.find(b) != std::string::npos;
}

float SrgbToLinear(float c) {
    if (c <= 0.04045f) return c / 12.92f;
    return std::exp(2.4f * std::log(std::fma(c, 1.f / 1.055f, 0.055f / 1.055f))) - 0.0000001f;
}

// 3x4 row-major placement: out_i = row_i . (x, y, z, 1).
struct Placement {
    float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    Float3 Apply(const Float3 &p) const {
        const double x = p[0], y = p[1], z = p[2];
        return {float(m[0] * x + m[1] * y + m[2] * z + m[3]), float(m[4] * x + m[5] * y + m[6] * z + m[7]),
                float(m[8] * x + m[9] * y + m[10] * z + m[11])};
    }
    float Determinant() const {
        return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
    }
};

// Scene-object transforms are three float4 rows; fragment transforms are twelve flat floats.
Placement ToPlacement(const Kv3 &v) {
    Placement p;
    if (v.type == Kv3::Type::Array && !v.arr.empty() && v.arr[0].type == Kv3::Type::Array) {
        for (size_t r = 0; r < 3; ++r)
            for (size_t c = 0; c < 4; ++c) p.m[r * 4 + c] = float(v.At(r).At(c).Number());
    } else if (v.type == Kv3::Type::Blob && v.blob.size() >= 48) {
        std::memcpy(p.m, v.blob.data(), 48);
    } else {
        for (size_t k = 0; k < 12; ++k) p.m[k] = float(v.At(k).Number());
    }
    return p;
}

Vec4 ToVec4(const Kv3 &v, Vec4 fallback) {
    if (v.type != Kv3::Type::Array) return fallback;
    Vec4 out{0, 0, 0, 0};
    for (size_t k = 0; k < 4 && k < v.arr.size(); ++k) out[k] = float(v.arr[k].Number());
    return out;
}

// ---------------------------------------------------------------------------------------
// Materials as the exporter writes them (non-adapted textures, fallback shader inputs).

struct Material {
    std::string name, shader;
    std::map<std::string, int64_t> ints;
    std::map<std::string, float> floats;
    std::vector<std::pair<std::string, std::string>> textures; // param -> resource path, material order
    bool blend = false, overlay = false;
    float cutoff = 0.5f;
    std::array<float, 3> factor{1, 1, 1};
    std::string baseTexture, emissiveTexture;

    int64_t Int(const char *k) const {
        const auto it = ints.find(k);
        return it == ints.end() ? 0 : it->second;
    }
};

const std::unordered_map<std::string, int> &GltfChannelOrder() {
    // Valve input name -> first glTF channel that accepts it (BaseColor=0, ..., Emissive=5).
    static const std::unordered_map<std::string, int> order = {
        {"TextureColor", 0}, {"TextureTranslucency", 0}, {"TextureNormal", 1}, {"TextureRoughness", 2},
        {"TextureMetalness", 2}, {"TextureSpecularMask", 3}, {"TextureAmbientOcclusion", 4}, {"TextureSelfIllumMask", 5},
    };
    return order;
}

// The glTF channel the exporter ties a texture parameter to: g_tX -> TextureX, matched exactly
// or as a layered name (TextureXA, TextureX1, TextureX0, TextureLayer1X). -1 when none.
int FirstGltfChannel(const std::string &param) {
    std::string input = param;
    if (input.compare(0, 3, "g_t") == 0) input = "Texture" + input.substr(3);
    int best = -1;
    for (const auto &[name, channel] : GltfChannelOrder()) {
        bool match = input == name;
        const std::string base = name.substr(7);
        for (const char *tpl : {"Texture%sA", "Texture%s1", "Texture%s0", "TextureLayer1%s"}) {
            char buffer[128];
            snprintf(buffer, sizeof buffer, tpl, base.c_str());
            match |= input == buffer;
        }
        if (match && (best < 0 || channel < best)) best = channel;
    }
    return best;
}

std::shared_ptr<Material> LoadMaterial(const PackageSet &resources, const std::string &path, const Vec4 &tint) {
    std::vector<uint8_t> bytes;
    if (path.empty() || !resources.Read(path, bytes)) return nullptr;
    const Kv3 data = Resource(std::move(bytes)).Kv3Block("DATA");
    auto m = std::make_shared<Material>();
    m->name = FileStem(path);
    m->shader = data["m_shaderName"].Str();
    std::map<std::string, Vec4> vectors;
    for (const Kv3 &p : data["m_intParams"].arr) m->ints[p["m_name"].Str()] = p["m_nValue"].Integer();
    for (const Kv3 &p : data["m_floatParams"].arr) m->floats[p["m_name"].Str()] = float(p["m_flValue"].Number());
    for (const Kv3 &p : data["m_vectorParams"].arr) vectors[p["m_name"].Str()] = ToVec4(p["m_value"], {0, 0, 0, 0});
    for (const Kv3 &p : data["m_textureParams"].arr) {
        const std::string key = p["m_name"].Str(), value = p["m_pValue"].Str();
        auto it = std::find_if(m->textures.begin(), m->textures.end(), [&](const auto &t) { return t.first == key; });
        if (it != m->textures.end()) it->second = value;
        else m->textures.emplace_back(key, value);
    }
    const bool translucent = m->Int("F_TRANSLUCENT") > 0 || (m->shader.size() >= 10 && m->shader.compare(m->shader.size() - 10, 10, "_glass.vfx") == 0);
    m->blend = translucent;
    const auto endsWith = [&](const char *suffix) {
        const size_t n = strlen(suffix);
        return m->shader.size() >= n && m->shader.compare(m->shader.size() - n, n, suffix) == 0;
    };
    m->overlay = m->Int("F_OVERLAY") == 1 || m->Int("F_DEPTHBIAS") == 1 || m->Int("F_DEPTH_BIAS") == 1 || endsWith("static_overlay.vfx") ||
                 m->shader == "citadel_overlay.vfx";
    if (!translucent && m->Int("F_ALPHA_TEST") > 0) {
        const auto it = m->floats.find("g_flAlphaTestReference");
        if (it != m->floats.end()) m->cutoff = it->second;
    }
    Vec4 base = tint;
    if (const auto it = m->floats.find("g_flModelTintAmount"); it != m->floats.end())
        for (int k = 0; k < 4; ++k) base[k] = 1.f + (base[k] - 1.f) * it->second;
    if (const auto it = vectors.find("g_vColorTint"); it != vectors.end()) {
        for (int k = 0; k < 4; ++k) base[k] *= it->second[k];
        base[3] = 1;
    }
    for (int k = 0; k < 3; ++k) m->factor[k] = std::clamp(SrgbToLinear(base[k]), 0.f, 1.f);
    for (const auto &[key, value] : m->textures) {
        const int channel = FirstGltfChannel(key);
        if (channel == 0) m->baseTexture = value;
        else if (channel == 5) m->emissiveTexture = value;
    }
    return m;
}

// ---------------------------------------------------------------------------------------
// Texture-derived caster inputs.

struct TextureCache {
    const PackageSet &resources;
    std::vector<std::string> &warnings;
    std::unordered_map<std::string, std::shared_ptr<Image>> images;

    std::shared_ptr<Image> Get(const std::string &path) {
        auto it = images.find(path);
        if (it != images.end()) return it->second;
        auto image = std::make_shared<Image>();
        std::vector<uint8_t> bytes;
        if (!resources.Read(path, bytes)) {
            image->width = image->height = 1;
            image->rgba.assign(4, 0);
        } else {
            try {
                *image = DecodeTexture(bytes);
            } catch (const std::exception &e) {
                warnings.push_back("texture " + path + ": " + e.what());
                image = nullptr;
            }
        }
        images[path] = image;
        return image;
    }
};

std::array<double, 3> AverageColor(const Image &image) {
    double r = 0, g = 0, b = 0, w = 0;
    const int stepX = std::max(1, image.width / 256), stepY = std::max(1, image.height / 256);
    static const auto table = [] {
        std::array<double, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            t[size_t(i)] = c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4);
        }
        return t;
    }();
    for (int y = 0; y < image.height; y += stepY)
        for (int x = 0; x < image.width; x += stepX) {
            const uint8_t *p = image.rgba.data() + (size_t(y) * image.width + x) * 4;
            const double a = p[3] / 255.0;
            r += table[p[0]] * a;
            g += table[p[1]] * a;
            b += table[p[2]] * a;
            w += a;
        }
    return w > 0 ? std::array<double, 3>{r / w, g / w, b / w} : std::array<double, 3>{.5, .5, .5};
}

// Stained-glass transmission image: at most 512 on the long side, resampled with GDI+
// high-quality bicubic exactly as the runtime cache has always been produced.
std::vector<uint8_t> ResizeColor(const Image &in, int &width, int &height) {
    const double scale = std::min(1.0, 512.0 / std::max(in.width, in.height));
    width = std::max(1, int(std::lround(in.width * scale)));
    height = std::max(1, int(std::lround(in.height * scale)));
    static ULONG_PTR token = [] {
        ULONG_PTR t = 0;
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&t, &input, nullptr);
        return t;
    }();
    (void)token;
    std::vector<uint8_t> bgra(in.rgba.size());
    for (size_t p = 0; p < in.rgba.size(); p += 4) {
        bgra[p] = in.rgba[p + 2];
        bgra[p + 1] = in.rgba[p + 1];
        bgra[p + 2] = in.rgba[p];
        bgra[p + 3] = in.rgba[p + 3];
    }
    Gdiplus::Bitmap source(in.width, in.height, in.width * 4, PixelFormat32bppARGB, bgra.data());
    Gdiplus::Bitmap target(width, height, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&target);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(&source, 0, 0, width, height);
    }
    Gdiplus::BitmapData locked{};
    Gdiplus::Rect rect(0, 0, width, height);
    std::vector<uint8_t> out(size_t(width) * height * 4);
    if (target.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &locked) != Gdiplus::Ok)
        throw MapDataError("stained glass resample failed");
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = static_cast<const uint8_t *>(locked.Scan0) + ptrdiff_t(y) * locked.Stride;
        for (int x = 0; x < width; ++x) {
            uint8_t *o = out.data() + (size_t(y) * width + x) * 4;
            o[0] = row[x * 4 + 2];
            o[1] = row[x * 4 + 1];
            o[2] = row[x * 4];
            o[3] = row[x * 4 + 3];
        }
    }
    target.UnlockBits(&locked);
    return out;
}

uint32_t PackAlbedo(const std::array<double, 3> &c) {
    uint32_t packed = 0;
    for (int i = 0; i < 3; ++i) packed |= uint32_t(std::lround(std::clamp(c[i], 0.0, 1.0) * 255)) << (i * 8);
    return packed;
}

// ---------------------------------------------------------------------------------------
// Batches (VisualShadowConverter).

struct Batch {
    std::vector<float> vertices; // x y z u v w0 w1 w2
    std::vector<uint32_t> indices;
    std::vector<uint8_t> alpha{255};
    int width = 1, height = 1;
    float cutoff = 0;
    int addressU = 0, addressV = 0;
    bool missingWeights = false, stained = false, dropped = false;
    std::string material;
    float animation[8] = {0, 0, 1, 1, 0, 0, 1, 1};
    uint32_t albedo = 0x808080;
    std::vector<uint8_t> color;
    int colorWidth = 0, colorHeight = 0;
};

bool AllowedShader(const std::string &shader, bool &foliage, bool &blockerShader) {
    foliage = shader == "csgo_foliage.vfx";
    blockerShader = shader == "generic.vfx";
    return foliage || shader == "csgo_vertexlitgeneric.vfx" || shader == "csgo_complex.vfx" || shader == "csgo_lightmappedgeneric.vfx" ||
           shader == "csgo_simple.vfx" || shader == "csgo_simple_2way_blend.vfx" || shader == "csgo_environment.vfx" ||
           shader == "csgo_environment_blend.vfx" || shader == "csgo_simple_3layer_parallax.vfx" || shader == "csgo_black_unlit.vfx" ||
           shader == "cables.vfx" || blockerShader;
}

struct Primitive {
    std::shared_ptr<MeshData> mesh;
    size_t drawCall = 0;
    int material = -1;
};

struct DrawCallStreams {
    const GpuBuffer *position = nullptr, *normal = nullptr, *uv = nullptr, *color = nullptr;
    const VertexAttribute *positionAttr = nullptr, *normalAttr = nullptr, *uvAttr = nullptr, *colorAttr = nullptr;
};

class NodeExporter {
public:
    NodeExporter(const PackageSet &r, TextureCache &t, std::vector<Batch> &b, std::vector<std::string> &w)
        : resources(r), textures(t), batches(b), warnings(w) {}

    void Export(const Kv3 &node) {
        for (const Kv3 &so : node["m_sceneObjects"].arr) {
            const std::string model = so["m_renderableModel"].Str();
            if (model.empty()) continue;
            auto data = Model(model);
            if (!data) continue;
            Vec4 tint = ToVec4(so["m_vTintColor"], {0, 0, 0, 0});
            if (tint == Vec4{0, 0, 0, 0}) tint = {1, 1, 1, 1};
            LoadModelNodes(*data, FileStem(model), ToPlacement(so["m_vTransform"]), tint);
        }
        for (const Kv3 &so : node["m_aggregateSceneObjects"].arr) {
            const std::string model = so["m_renderableModel"].Str();
            if (model.empty()) continue;
            auto data = Model(model);
            if (!data) continue;
            if (!AggregateFragments(*data, so)) LoadModelNodes(*data, FileStem(model), Placement{}, {1, 1, 1, 1});
        }
    }

private:
    const PackageSet &resources;
    TextureCache &textures;
    std::vector<Batch> &batches;
    std::vector<std::string> &warnings;
    std::map<std::string, std::vector<Primitive>> exportedMeshes;
    std::map<std::pair<std::string, Vec4>, int> materialKeys;
    std::vector<std::shared_ptr<Material>> materials;
    std::map<int64_t, size_t> grouped; // materialId*2+missingWeights -> batch index
    std::map<std::string, std::shared_ptr<ModelData>> models;
    std::map<std::string, std::shared_ptr<MeshData>> meshes;

    std::shared_ptr<ModelData> Model(const std::string &path) {
        auto it = models.find(path);
        if (it != models.end()) return it->second;
        std::shared_ptr<ModelData> m;
        std::vector<uint8_t> bytes;
        if (resources.Read(path, bytes)) {
            try {
                m = std::make_shared<ModelData>(LoadModel(std::move(bytes)));
            } catch (const std::exception &e) {
                warnings.push_back("model " + path + ": " + e.what());
            }
        }
        models[path] = m;
        return m;
    }

    std::shared_ptr<MeshData> Mesh(const std::string &path) {
        auto it = meshes.find(path);
        if (it != meshes.end()) return it->second;
        std::shared_ptr<MeshData> m;
        std::vector<uint8_t> bytes;
        if (resources.Read(path, bytes)) {
            try {
                m = LoadMesh(std::move(bytes));
            } catch (const std::exception &e) {
                warnings.push_back("mesh " + path + ": " + e.what());
            }
        }
        meshes[path] = m;
        return m;
    }

    // Embedded meshes pair positionally with the LOD mask list (shorter list wins).
    size_t EmbeddedCount(const ModelData &m) const { return std::min(m.embedded.size(), m.lodMasks.size()); }

    static std::vector<const Kv3 *> DrawCalls(const MeshData &mesh) {
        std::vector<const Kv3 *> out;
        for (const Kv3 &so : mesh.data["m_sceneObjects"].arr)
            for (const Kv3 &dc : so["m_drawCalls"].arr) out.push_back(&dc);
        return out;
    }

    int MaterialFor(const Kv3 &drawCall, const Vec4 &parentTint) {
        std::string path = drawCall["m_material"].Str();
        if (path.empty()) path = drawCall["m_pMaterial"].Str();
        Vec4 tint = parentTint;
        if (const Kv3 *dcTint = drawCall.Find("m_vTintColor")) {
            Vec4 t{float(dcTint->At(0).Number()), float(dcTint->At(1).Number()), float(dcTint->At(2).Number()), 1.f};
            if (const Kv3 *alpha = drawCall.Find("m_flAlpha")) t[3] = float(alpha->Number());
            for (int k = 0; k < 4; ++k) tint[k] *= t[k];
        }
        const auto key = std::make_pair(path, tint);
        if (const auto it = materialKeys.find(key); it != materialKeys.end()) return it->second;
        std::shared_ptr<Material> m;
        try {
            m = LoadMaterial(resources, path, tint);
        } catch (const std::exception &e) {
            warnings.push_back("material " + path + ": " + e.what());
        }
        if (!m) {
            warnings.push_back("missing caster material: " + path);
            materialKeys[key] = -1;
            return -1;
        }
        materials.push_back(m);
        const int id = int(materials.size() - 1);
        materialKeys[key] = id;
        return id;
    }

    void LoadModelNodes(const ModelData &model, const std::string &name, const Placement &placement, const Vec4 &tint) {
        const int lod = model.LowestLod();
        std::vector<std::pair<std::shared_ptr<MeshData>, std::string>> list;
        for (size_t k = 0; k < EmbeddedCount(model); ++k)
            if (model.InLod(model.embedded[k].meshIndex, lod)) list.emplace_back(model.embedded[k].mesh, name + "." + model.embedded[k].name);
        for (size_t k = 0; k < model.refMeshes.size(); ++k) {
            if (model.refMeshes[k].empty() || !model.InLod(int(k), lod)) continue;
            auto mesh = Mesh(model.refMeshes[k]);
            if (mesh) list.emplace_back(mesh, FileStem(model.refMeshes[k]));
        }
        for (const auto &[mesh, meshName] : list) {
            const auto drawCalls = DrawCalls(*mesh);
            if (mesh->data["m_sceneObjects"].arr.empty()) continue;
            auto it = exportedMeshes.find(meshName);
            if (it == exportedMeshes.end()) {
                std::vector<Primitive> prims;
                for (size_t d = 0; d < drawCalls.size(); ++d) prims.push_back({mesh, d, MaterialFor(*drawCalls[d], tint)});
                it = exportedMeshes.emplace(meshName, std::move(prims)).first;
            }
            for (const Primitive &p : it->second) Emit(p, placement);
        }
    }

    bool AggregateFragments(const ModelData &model, const Kv3 &so) {
        std::shared_ptr<MeshData> mesh;
        if (EmbeddedCount(model) > 0) {
            if (EmbeddedCount(model) > 1) {
                warnings.push_back("aggregate model with several embedded meshes left out");
                return true;
            }
            mesh = model.embedded[0].mesh;
        } else {
            std::vector<std::string> refs;
            for (size_t k = 0; k < model.refMeshes.size(); ++k)
                if (!model.refMeshes[k].empty() && model.InLod(int(k), model.LowestLod())) refs.push_back(model.refMeshes[k]);
            if (refs.empty()) return true;
            if (refs.size() > 1) {
                warnings.push_back("aggregate model with several referenced meshes left out");
                return true;
            }
            mesh = Mesh(refs[0]);
            if (!mesh) return false;
        }
        const auto &fragments = so["m_aggregateMeshes"].arr;
        if (!fragments.empty() && !fragments[0].Find("m_nDrawCallIndex")) return false;
        const auto drawCalls = DrawCalls(*mesh);
        const auto &transforms = so["m_fragmentTransforms"].arr;
        size_t transformIndex = 0;
        for (const Kv3 &f : fragments) {
            const int64_t dc = f["m_nDrawCallIndex"].Integer(-1);
            Placement placement;
            if (f["m_bHasTransform"].type == Kv3::Type::Bool ? f["m_bHasTransform"].b : f["m_bHasTransform"].Integer() != 0) {
                if (transformIndex >= transforms.size()) throw MapDataError("missing fragment transform");
                placement = ToPlacement(transforms[transformIndex++]);
                if (placement.Determinant() == 0.f) continue;
            }
            if (dc < 0 || size_t(dc) >= drawCalls.size()) throw MapDataError("invalid fragment draw call");
            Vec4 tint{1, 1, 1, 1};
            if (const Kv3 *t = f.Find("m_vTintColor"))
                tint = {float(t->At(0).Number()) / 255.f, float(t->At(1).Number()) / 255.f, float(t->At(2).Number()) / 255.f, 1.f};
            Emit({mesh, size_t(dc), MaterialFor(*drawCalls[size_t(dc)], tint)}, placement);
        }
        return true;
    }

    DrawCallStreams Streams(const MeshData &mesh, const Kv3 &drawCall) {
        DrawCallStreams s;
        for (const Kv3 &vbInfo : drawCall["m_vertexBuffers"].arr) {
            const int64_t index = vbInfo["m_hBuffer"].Integer(-1);
            if (index < 0 || size_t(index) >= mesh.vertexBuffers.size()) continue;
            const GpuBuffer &vb = mesh.vertexBuffers[size_t(index)];
            if (vb.count == 0) continue;
            std::vector<const VertexAttribute *> attrs;
            for (const auto &a : vb.attributes) attrs.push_back(&a);
            std::stable_sort(attrs.begin(), attrs.end(), [](const VertexAttribute *a, const VertexAttribute *b) {
                return a->semanticIndex != b->semanticIndex ? a->semanticIndex < b->semanticIndex : a->offset < b->offset;
            });
            for (const VertexAttribute *a : attrs) {
                const int components = FormatComponents(a->format);
                if (a->semantic == "POSITION") { s.position = &vb; s.positionAttr = a; }
                else if (a->semantic == "NORMAL") { s.normal = &vb; s.normalAttr = a; }
                else if (a->semantic == "TEXCOORD" && components == 2 && !s.uv) { s.uv = &vb; s.uvAttr = a; }
                else if (a->semantic == "COLOR" && a->semanticIndex == 0 && components > 1 && !s.color) { s.color = &vb; s.colorAttr = a; }
            }
        }
        return s;
    }

    void Emit(const Primitive &p, const Placement &placement) {
        if (p.material < 0) return;
        const Material &m = *materials[size_t(p.material)];
        const auto drawCalls = DrawCalls(*p.mesh);
        const Kv3 &dc = *drawCalls[p.drawCall];
        const Kv3 &prim = dc["m_nPrimitiveType"];
        if (!(prim.type == Kv3::Type::String ? prim.s == "RENDER_PRIM_TRIANGLES" : prim.Integer() == 5)) {
            warnings.push_back("non-triangle draw call left out: " + m.name);
            return;
        }
        bool foliage = false, blockerShader = false;
        if (!AllowedShader(m.shader, foliage, blockerShader)) return;
        const bool blocker = blockerShader && ContainsNoCase(m.name, "blocklight");
        if (blockerShader && !blocker) return;
        const bool cutout = m.Int("F_ALPHA_TEST") == 1;
        const bool stained = !cutout && ContainsNoCase(m.name, "stained") && ContainsNoCase(m.name, "glass");
        if (m.Int("F_DO_NOT_CAST_SHADOWS") != 0) return;
        if (m.Int("F_OVERLAY") != 0 || m.Int("F_TRANSLUCENT") != 0 || m.Int("F_ADDITIVE_BLEND") != 0 || m.blend) return;

        const DrawCallStreams s = Streams(*p.mesh, dc);
        const bool missingWeights = m.Int("F_VERTEX_ANIMATION") != 0 && !s.color;
        const int64_t key = int64_t(p.material) * 2 + (missingWeights ? 1 : 0);
        auto found = grouped.find(key);
        if (found == grouped.end()) {
            batches.push_back(CreateBatch(m, foliage, blocker, cutout, stained, missingWeights));
            found = grouped.emplace(key, batches.size() - 1).first;
        }
        Batch &b = batches[found->second];
        if (b.dropped) return;

        if (!s.position) { warnings.push_back("draw call without positions: " + m.name); return; }
        std::vector<Float3> pos;
        if (!ReadFloat3(*s.position, *s.positionAttr, pos)) { warnings.push_back("unsupported position format: " + m.name); return; }
        // Overlay (decal) surfaces sit pushed out along their normals, as the exported scene has them.
        std::vector<Float3> normals;
        if (m.overlay && s.normal && ReadNormals(*s.normal, *s.normalAttr, normals) && normals.size() == pos.size()) {
            constexpr double offset = double(0.01f / 0.0254f) / 0.0254;
            for (size_t v = 0; v < pos.size(); ++v)
                for (int c = 0; c < 3; ++c) pos[v][size_t(c)] = float(pos[v][size_t(c)] + normals[v][size_t(c)] * offset);
        }
        const bool textured = b.cutoff > 0 || b.stained || cutout;
        std::vector<std::array<float, 2>> uv;
        if (textured) {
            if (!s.uv || !ReadFloat2(*s.uv, *s.uvAttr, uv) || uv.size() != pos.size()) {
                warnings.push_back("textured caster without usable UVs left out: " + m.name);
                return;
            }
        }
        std::vector<std::array<float, 4>> colors;
        if (s.color && !ReadFloat4(*s.color, *s.colorAttr, colors)) colors.clear();
        if (!colors.empty() && colors.size() != pos.size()) colors.clear();
        if ((b.animation[0] > 0 || b.animation[4] > 0) && colors.empty()) {
            warnings.push_back("animated foliage without weights kept static: " + m.name);
            b.animation[0] = b.animation[4] = 0;
        }

        const int64_t ibIndex = dc["m_indexBuffer"]["m_hBuffer"].Integer(-1);
        if (ibIndex < 0 || size_t(ibIndex) >= p.mesh->indexBuffers.size()) throw MapDataError("invalid index buffer");
        const GpuBuffer &ib = p.mesh->indexBuffers[size_t(ibIndex)];
        const int64_t baseVertex = dc["m_nBaseVertex"].Integer(), start = dc["m_nStartIndex"].Integer(), count = dc["m_nIndexCount"].Integer();
        if (start < 0 || count < 0 || (ib.stride != 2 && ib.stride != 4) || size_t(start + count) * ib.stride > ib.data.size())
            throw MapDataError("invalid index range");
        if (count % 3) throw MapDataError("invalid index count");
        std::unordered_map<int64_t, uint32_t> remap;
        for (int64_t i = 0; i < count; ++i) {
            int64_t vertex;
            if (ib.stride == 4) {
                int32_t v;
                std::memcpy(&v, ib.data.data() + size_t(start + i) * 4, 4);
                vertex = int64_t(int32_t(uint32_t(v) + uint32_t(baseVertex)));
            } else {
                uint16_t v;
                std::memcpy(&v, ib.data.data() + size_t(start + i) * 2, 2);
                vertex = baseVertex + v;
            }
            if (vertex < 0 || size_t(vertex) >= pos.size()) throw MapDataError("invalid visual index");
            auto r = remap.find(vertex);
            if (r == remap.end()) {
                r = remap.emplace(vertex, uint32_t(b.vertices.size() / 8)).first;
                const Float3 w = placement.Apply(pos[size_t(vertex)]);
                b.vertices.insert(b.vertices.end(), {w[0], w[1], w[2]});
                b.vertices.push_back(textured ? uv[size_t(vertex)][0] : 0.f);
                b.vertices.push_back(textured ? uv[size_t(vertex)][1] : 0.f);
                for (int c = 0; c < 3; ++c) {
                    const float weight = colors.empty() ? 0.f : colors[size_t(vertex)][size_t(c)];
                    b.vertices.push_back(std::isnan(weight) ? 0.f : std::clamp(weight, 0.f, 1.f));
                }
            }
            b.indices.push_back(r->second);
        }
    }

    Batch CreateBatch(const Material &m, bool foliage, bool blocker, bool cutout, bool stained, bool missingWeights) {
        Batch b;
        b.material = m.name;
        b.addressU = int(m.Int("g_nTextureAddressModeU"));
        b.addressV = int(m.Int("g_nTextureAddressModeV"));
        if (b.addressU < 0 || b.addressU > 4 || b.addressV < 0 || b.addressV > 4) {
            warnings.push_back("invalid texture addressing, using wrap: " + m.name);
            b.addressU = b.addressV = 0;
        }
        std::shared_ptr<Image> base = m.baseTexture.empty() ? nullptr : textures.Get(m.baseTexture);
        if (blocker) {
            b.albedo = 0x404040;
        } else {
            std::array<double, 3> c{m.factor[0], m.factor[1], m.factor[2]};
            if (base) {
                const auto avg = AverageColor(*base);
                for (int i = 0; i < 3; ++i) c[size_t(i)] *= avg[size_t(i)];
            } else {
                for (double &v : c) v *= .5;
            }
            b.albedo = PackAlbedo(c);
        }
        const int64_t variant = m.Int("F_VERTEX_ANIMATION");
        if (variant == 1 && !foliage) {
            warnings.push_back("vertex animation on non-foliage shader kept static: " + m.name);
        } else if (variant == 1) {
            static const char *names[8] = {"g_flSwayAmount", "g_flSwaySpeed", "g_flSwayNoiseScale", "g_flSwayFalloff",
                                           "g_flFlutterAmount", "g_flFlutterSpeed", "g_flFlutterNoiseScale", "g_flFlutterFalloff"};
            bool valid = true;
            float values[8];
            for (int i = 0; i < 8; ++i) {
                const auto it = m.floats.find(names[i]);
                valid &= it != m.floats.end() && !std::isnan(it->second) && it->second >= 0 && it->second <= 100;
                values[i] = valid ? it->second : 0;
            }
            const auto noise = std::find_if(m.textures.begin(), m.textures.end(), [](const auto &t) { return t.first == "g_tNoiseMap"; });
            valid &= noise != m.textures.end() && FileName(noise->second) == "perlin_a_z000_tga_8c53a467.vtex";
            if (!valid) warnings.push_back("foliage sway parameters unsupported, kept static: " + m.name);
            else std::copy(values, values + 8, b.animation);
            b.missingWeights = missingWeights;
            if (missingWeights) b.animation[0] = b.animation[4] = 0;
        }
        // Other animation variants (cloth, flags) stay static casters, as they always have.
        if (cutout) {
            if (!base) {
                warnings.push_back("cutout without colour texture treated as opaque: " + m.name);
            } else if (base->width > 8192 || base->height > 8192) {
                warnings.push_back("oversized cutout texture treated as opaque: " + m.name);
            } else {
                b.cutoff = m.cutoff;
                b.width = base->width;
                b.height = base->height;
                b.alpha.resize(size_t(b.width) * b.height);
                bool solid = true;
                for (size_t i = 0; i < b.alpha.size(); ++i) {
                    b.alpha[i] = base->rgba[i * 4 + 3];
                    solid &= b.alpha[i] == 255;
                }
                if (solid) {
                    b.cutoff = 0;
                    b.width = b.height = 1;
                    b.alpha.assign(1, 255);
                }
            }
        }
        if (stained) {
            const std::string &path = !m.emissiveTexture.empty() && m.Int("F_SELF_ILLUM") == 1 ? m.emissiveTexture : m.baseTexture;
            std::shared_ptr<Image> image = path.empty() ? nullptr : textures.Get(path);
            if (!image) {
                warnings.push_back("stained glass without a colour texture left opaque: " + m.name);
            } else {
                b.stained = true;
                b.color = ResizeColor(*image, b.colorWidth, b.colorHeight);
            }
        }
        return b;
    }
};

} // namespace

std::vector<uint8_t> BuildVisualCasters(const Vpk &package, const PackageSet &resources, CasterStats &stats,
                                        std::vector<std::string> &warnings, const CasterProgress &progress) {
    std::vector<Batch> batches;
    TextureCache textures{resources, warnings, {}};
    const auto nodes = package.List("vwnod_c");
    if (nodes.empty()) throw MapDataError("no world nodes in map package");
    for (size_t n = 0; n < nodes.size(); ++n) {
        if (progress.cancel && progress.cancel->load()) throw MapDataError("cancelled");
        if (progress.report) progress.report("Reading world nodes " + std::to_string(n + 1) + "/" + std::to_string(nodes.size()));
        std::vector<uint8_t> bytes;
        package.Read(nodes[n], bytes);
        const Kv3 node = Resource(std::move(bytes)).Kv3Block("DATA");
        NodeExporter(resources, textures, batches, warnings).Export(node);
    }

    // Validation (former converter bounds); offending batches are left out, not the map.
    for (Batch &b : batches) {
        const size_t vc = b.vertices.size() / 8;
        const bool separate = b.cutoff > 0 || b.animation[0] > 0 || b.animation[4] > 0 || b.stained;
        // Opaque materials merge into one rigid mesh, so use its global limits here.
        // Applying the smaller cutout-batch limit drops entire walls on community maps.
        bool bad = vc < 3 || vc > (separate ? 2000000 : kVisualCasterMaxVertices) ||
                   b.indices.size() > (separate ? 6000000 : kVisualCasterMaxIndices) ||
                   std::isnan(b.cutoff) || b.cutoff < 0 || b.cutoff > 1;
        for (float v : b.vertices) bad |= !std::isfinite(v) || std::fabs(v) > 100000;
        if (bad && vc > 0) warnings.push_back("caster batch outside runtime bounds left out: " + b.material);
        b.dropped = bad;
    }
    batches.erase(std::remove_if(batches.begin(), batches.end(), [](const Batch &b) { return b.dropped; }), batches.end());
    if (batches.size() > 4096) throw MapDataError("too many visual materials");

    OccluderBuilder rigid;
    std::vector<uint32_t> rigidAlbedo;
    std::vector<const Batch *> cutouts, stained;
    uint64_t bytes = 0;
    for (const Batch &b : batches) {
        stats.triangles += b.indices.size() / 3;
        if (b.cutoff > 0 || b.animation[0] > 0 || b.animation[4] > 0) {
            cutouts.push_back(&b);
            bytes += b.vertices.size() * 4 + b.indices.size() * 4 + b.alpha.size();
            continue;
        }
        std::vector<uint32_t> remap(b.vertices.size() / 8);
        for (size_t v = 0; v < remap.size(); ++v) {
            const Float3 p{b.vertices[v * 8], b.vertices[v * 8 + 1], b.vertices[v * 8 + 2]};
            auto it = rigid.unique.find(p);
            if (it == rigid.unique.end()) {
                it = rigid.unique.emplace(p, uint32_t(rigid.vertices.size())).first;
                rigid.vertices.push_back(p);
            }
            remap[v] = it->second;
        }
        for (uint32_t i : b.indices) rigid.indices.push_back(remap[i]);
        rigidAlbedo.insert(rigidAlbedo.end(), b.indices.size() / 3, b.albedo);
        if (b.stained) {
            stained.push_back(&b);
            bytes += b.vertices.size() / 8 * 20 + b.indices.size() * 4 + b.color.size();
        }
    }
    bytes += rigid.vertices.size() * 12 + rigid.indices.size() * 4 + rigidAlbedo.size() * 4;
    if (rigid.vertices.size() < 3 || rigid.vertices.size() > kVisualCasterMaxVertices || rigid.indices.size() > kVisualCasterMaxIndices || bytes > kVisualCasterMemoryLimit)
        throw MapDataError("render geometry exceeds runtime bounds: " + std::to_string(bytes) + " bytes, " +
                           std::to_string(rigid.vertices.size()) + " vertices, " + std::to_string(rigid.indices.size()) + " indices");

    ByteWriter w;
    w.Bytes("MVMCAST6", 8);
    w.Put(int32_t(cutouts.size()));
    w.Put(int32_t(1));
    w.Put(int32_t(rigid.vertices.size()));
    w.Put(int32_t(rigid.indices.size()));
    for (const Float3 &p : rigid.vertices) w.Bytes(p.data(), 12);
    w.Bytes(rigid.indices.data(), rigid.indices.size() * 4);
    w.Bytes(rigidAlbedo.data(), rigidAlbedo.size() * 4);
    for (const Batch *b : cutouts) {
        w.Put(int32_t(b->vertices.size() / 8));
        w.Put(int32_t(b->indices.size()));
        w.Put(int32_t(b->width));
        w.Put(int32_t(b->height));
        w.Put(b->cutoff);
        w.Bytes(b->animation, sizeof b->animation);
        w.Put(int32_t(b->addressU));
        w.Put(int32_t(b->addressV));
        w.Put(b->albedo);
        w.Bytes(b->vertices.data(), b->vertices.size() * 4);
        w.Bytes(b->indices.data(), b->indices.size() * 4);
        w.Bytes(b->alpha.data(), b->alpha.size());
    }
    w.Put(int32_t(stained.size()));
    for (const Batch *b : stained) {
        w.Put(int32_t(b->vertices.size() / 8));
        w.Put(int32_t(b->indices.size()));
        w.Put(int32_t(b->colorWidth));
        w.Put(int32_t(b->colorHeight));
        for (size_t v = 0; v < b->vertices.size() / 8; ++v) w.Bytes(&b->vertices[v * 8], 20);
        w.Bytes(b->indices.data(), b->indices.size() * 4);
        w.Bytes(b->color.data(), b->color.size());
    }

    stats.batches = batches.size();
    stats.cutoutBatches = cutouts.size();
    stats.stainedGlassBatches = stained.size();
    stats.rigidVertices = rigid.vertices.size();
    stats.rigidTriangles = rigid.indices.size() / 3;
    stats.bytes = bytes;
    BoundsOf(rigid.vertices, stats.low, stats.high);
    return std::move(w.out);
}

} // namespace Filmmaker::MapData
