// Validates every lighting variant the hook derives from CS2's own material programs, outside the
// game: the same header-only patchers DeriveNativeMaterialShaders runs, applied to the programs that
// `mirv_relight dumpprograms <dir>` writes.
//
//   ValidateNativeVariants <dir>                       disassemble, reflect and create every variant on a
//                                                      debug-layer WARP device
//   ValidateNativeVariants <dir> --hardware [--shards N]
//                                                      also draw every variant once on the real GPU, cold:
//                                                      N parallel shards (default 8) from a freshly named
//                                                      copy of this tool, so the driver's per-executable
//                                                      shader cache is empty and it really compiles; a
//                                                      shard the driver kills is restarted after the
//                                                      variant it was drawing, which is reported
//   ValidateNativeVariants <dir> --write <out>         also keep each variant as <hash>-<kind>.dxbc
//   ValidateNativeVariants --cache <shader-cache> [name]
//                                                      cold-compile the newest build of every program in
//                                                      the hook's runtime shader cache, one process each
//
// Why a draw: drivers compile a pixel shader lazily, at its first draw, on their own worker threads,
// so a program the driver cannot compile faults there, not in CreatePixelShader. 2026-11-07: NVIDIA
// 616.56 crashed CS2 as soon as relit draws began; 800 of 11,475 cold-compiled variants crashed this
// tool, every one a dynamic irradiance variant, because the query read .xyzw of inputs declared .xyz.
// A crash is intermittent (a quarter of those variants, each on about one compile in eight), so a clean
// run means nothing until it covers the whole set cold.
#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeMaterialMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeAtmosphereMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeAoMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeLightingMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeClusterMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeCoverageMath.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightIrradianceMath.h"
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Environment;
namespace fs = std::filesystem;

struct Variant { std::string program, kind; std::vector<uint8_t> code; };

// Mirror of DeriveNativeMaterialShaders (EnvironmentNativeMaterial.cpp); the smoke variant needs the
// compiled smoke kernel and is left out.
void Derive(const std::string& name, const std::vector<uint8_t>& code, std::vector<Variant>& out) {
    NativeSunAbiInfo info{};
    const bool sunAbi = HasNativeSunMaterialAbi(code.data(), code.size(), &info);
    if (!sunAbi && !info.lightSlot) return;
    auto push = [&](const std::string& kind, const std::vector<uint8_t>& c) { out.push_back({name, kind, c}); };
    auto shareLighting = [&](const std::string& prefix, const std::vector<uint8_t>& program) {
        std::vector<uint8_t> shared; NativeSharedLightingInfo layout;
        if (NativeSharedLightingProgram(program.data(), program.size(), shared, layout)) push(prefix + ".lighting", shared);
        std::vector<uint8_t> sparse;
        if (NativeSparseClusterProgram(program.data(), program.size(), sparse) &&
            NativeSharedLightingProgram(sparse.data(), sparse.size(), shared, layout))
            push(prefix + ".sparse", shared);
    };
    auto shareAo = [&](const std::string& prefix, const std::vector<uint8_t>& program) {
        std::vector<uint8_t> shared; bool unchanged = false;
        if (!NativeSharedAoProgram(program.data(), program.size(), shared, &unchanged)) { shareLighting(prefix, program); return; }
        push(prefix + ".ao", shared); shareLighting(prefix + ".ao", shared);
    };
    std::vector<uint8_t> atmosphere; bool ownedCascades = false, lampCube = false; uint32_t screenShadowSlot = 0;
    const bool hasAtmosphere = NativeAtmosphereProgram(code.data(), code.size(), atmosphere, true,
        sunAbi ? &ownedCascades : nullptr, &screenShadowSlot, &lampCube);
    if (hasAtmosphere) { push("atmosphere", atmosphere); shareAo("atmosphere", atmosphere); }
    if ((info.lightmapArrayMask & 2) || info.probeSlot) {
        std::vector<uint8_t> dynamic; Filmmaker::Relight::IrradianceProgramInfo query;
        const auto& source = hasAtmosphere ? atmosphere : code;
        if (Filmmaker::Relight::DynamicIrradianceProgram(source.data(), source.size(), dynamic, &query, info.probeSlot)) {
            push("dynamic", dynamic); shareAo("dynamic", dynamic);
        }
    }
    std::vector<uint8_t> coverage;
    if (NativeCoverageProgram(code.data(), code.size(), coverage)) push("coverage", coverage);
    if (NativeCoverageProgram(code.data(), code.size(), coverage, true)) push("coverage-black", coverage);
}

// A vertex shader whose outputs are exactly the pixel shader's inputs, so a draw links.
std::string VertexSourceFor(ID3D11ShaderReflection* reflection) {
    D3D11_SHADER_DESC desc{}; reflection->GetDesc(&desc);
    std::ostringstream fields, body;
    bool position = false;
    for (UINT i = 0; i < desc.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p{}; reflection->GetInputParameterDesc(i, &p);
        if (p.SystemValueType == D3D_NAME_POSITION) { position = true; continue; }
        if (p.SystemValueType != D3D_NAME_UNDEFINED && p.SystemValueType != D3D_NAME_CLIP_DISTANCE &&
            p.SystemValueType != D3D_NAME_CULL_DISTANCE) continue; // generated by the rasterizer
        unsigned components = 0; for (unsigned m = p.Mask; m; m >>= 1) components = components + 1;
        const char* type = p.ComponentType == D3D_REGISTER_COMPONENT_UINT32 ? "uint" :
                           p.ComponentType == D3D_REGISTER_COMPONENT_SINT32 ? "int" : "float";
        const std::string field = "f" + std::to_string(i);
        fields << "  nointerpolation " << type << (components > 1 ? std::to_string(components) : "") << " " << field << " : "
               << p.SemanticName << p.SemanticIndex << ";\n";
        body << "  o." << field << " = 0;\n";
    }
    std::ostringstream s;
    s << "struct O {\n  float4 position : SV_Position;\n" << fields.str() << "};\n"
      << "O main(uint id : SV_VertexID) {\n  O o;\n  float2 uv = float2((id << 1) & 2, id & 2);\n"
      << "  o.position = float4(uv * float2(2, -2) + float2(-1, 1), .5, 1);\n" << body.str() << "  return o;\n}\n";
    (void)position;
    return s.str();
}

struct Result { bool disassembled = false, reflected = false, created = false; unsigned debugErrors = 0; std::string firstError; };

Result Check(const std::vector<uint8_t>& code, ID3D11Device* debugDevice) {
    Result r;
    ComPtr<ID3DBlob> text;
    r.disassembled = SUCCEEDED(D3DDisassemble(code.data(), code.size(), 0, nullptr, &text));
    ComPtr<ID3D11ShaderReflection> reflection;
    r.reflected = SUCCEEDED(D3DReflect(code.data(), code.size(), IID_PPV_ARGS(&reflection)));
    ComPtr<ID3D11InfoQueue> queue; debugDevice->QueryInterface(IID_PPV_ARGS(&queue));
    if (queue) queue->ClearStoredMessages();
    ComPtr<ID3D11PixelShader> ps;
    r.created = SUCCEEDED(debugDevice->CreatePixelShader(code.data(), code.size(), nullptr, &ps));
    if (queue) {
        for (UINT64 i = 0, n = queue->GetNumStoredMessages(); i < n; ++i) {
            SIZE_T bytes = 0; queue->GetMessage(i, nullptr, &bytes);
            std::vector<char> storage(bytes); auto* m = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            if (SUCCEEDED(queue->GetMessage(i, m, &bytes)) && m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
                if (!r.debugErrors) r.firstError.assign(m->pDescription, m->DescriptionByteLength ? m->DescriptionByteLength - 1 : 0);
                ++r.debugErrors;
            }
        }
    }
    return r;
}

// One variant drawn on the hardware device: create, bind with a matching vertex shader, draw into a
// small target, then wait for the GPU, which forces the driver's own compile of the program.
int DrawOnHardware(const fs::path& file) {
    std::ifstream f(file, std::ios::binary); std::vector<uint8_t> code((std::istreambuf_iterator<char>(f)), {});
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx))) return 3;
    ComPtr<ID3D11PixelShader> ps;
    if (FAILED(device->CreatePixelShader(code.data(), code.size(), nullptr, &ps))) return 4;
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(code.data(), code.size(), IID_PPV_ARGS(&reflection)))) return 5;
    const auto source = VertexSourceFor(reflection.Get());
    ComPtr<ID3DBlob> vsCode, errors;
    if (FAILED(D3DCompile(source.data(), source.size(), "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsCode, &errors))) {
        std::cerr << (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "vs compile failed") << "\n" << source;
        return 6;
    }
    ComPtr<ID3D11VertexShader> vs; device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs);
    D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = 64; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target; device->CreateTexture2D(&td, nullptr, &target);
    ComPtr<ID3D11RenderTargetView> rtv; device->CreateRenderTargetView(target.Get(), nullptr, &rtv);
    D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
    ctx->RSSetViewports(1, &vp); auto rt = rtv.Get(); ctx->OMSetRenderTargets(1, &rt, nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs.Get(), nullptr, 0); ctx->PSSetShader(ps.Get(), nullptr, 0);
    ctx->Draw(3, 0);
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0}; ComPtr<ID3D11Query> done; device->CreateQuery(&qd, &done);
    ctx->End(done.Get()); ctx->Flush();
    BOOL finished = FALSE; for (int i = 0; i < 20000 && ctx->GetData(done.Get(), &finished, sizeof(finished), 0) != S_OK; ++i) Sleep(1);
    return finished ? 0 : 7;
}

// The hook's runtime shader cache (%APPDATA%/HLAE/source_mvm/shader-cache): a 32-byte header
// {magic, keySize, codeSize, checksum}, the key (source, name, entry, profile, ...), then DXBC.
struct CachedProgram { fs::path file; std::string name, entry, profile; std::vector<uint8_t> code; fs::file_time_type time; };
bool ReadCached(const fs::path& file, CachedProgram& out) {
    std::ifstream in(file, std::ios::binary); uint64_t h[4]{};
    if (!in.read(reinterpret_cast<char*>(h), sizeof(h)) || h[0] != 0x324f53434d564dull || h[2] == 0 || h[2] > 16u << 20 || h[1] > 4u << 20) return false;
    std::string key(size_t(h[1]), '\0'); in.read(key.data(), key.size());
    out.code.resize(size_t(h[2])); if (!in.read(reinterpret_cast<char*>(out.code.data()), out.code.size())) return false;
    out.file = file; out.time = fs::last_write_time(file);
    // The key's printable fields: take the profile (xs_5_0) and the two words before it.
    std::vector<std::string> words; std::string word;
    for (char c : key) { if (c >= 32 && c < 127) word += c; else { if (!word.empty()) words.push_back(word); word.clear(); } }
    if (!word.empty()) words.push_back(word);
    for (size_t i = 0; i < words.size(); ++i)
        if (words[i].size() == 6 && words[i].substr(2) == "_5_0") { out.profile = words[i]; out.entry = i ? words[i - 1] : ""; out.name = i > 1 ? words[i - 2] : ""; }
    return true;
}
// Program type from the SHEX/SHDR version token: 0 pixel, 1 vertex, 2 geometry, 3 hull, 4 domain, 5 compute.
int ProgramType(const std::vector<uint8_t>& code) {
    if (code.size() < 32) return -1;
    uint32_t chunks = 0; std::memcpy(&chunks, code.data() + 28, 4);
    for (uint32_t i = 0; i < chunks && 32 + 4 * i + 4 <= code.size(); ++i) {
        uint32_t offset = 0; std::memcpy(&offset, code.data() + 32 + 4 * i, 4);
        if (offset + 12 > code.size()) return -1;
        if (!std::memcmp(code.data() + offset, "SHEX", 4) || !std::memcmp(code.data() + offset, "SHDR", 4)) {
            uint32_t version = 0; std::memcpy(&version, code.data() + offset + 8, 4); return int(version >> 16);
        }
    }
    return -1;
}
// One cached program forced through the driver's compile on the real GPU.
int ForceCachedOnHardware(const fs::path& file) {
    CachedProgram p; if (!ReadCached(file, p)) return 8;
    const int type = ProgramType(p.code);
    if (type == 0) {
        const auto copy = fs::temp_directory_path() / (file.stem().u8string() + ".ps.dxbc");
        std::ofstream(copy, std::ios::binary).write(reinterpret_cast<const char*>(p.code.data()), std::streamsize(p.code.size()));
        const int r = DrawOnHardware(copy); fs::remove(copy); return r;
    }
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx))) return 3;
    HRESULT hr = E_FAIL;
    if (type == 5) {
        ComPtr<ID3D11ComputeShader> cs; hr = device->CreateComputeShader(p.code.data(), p.code.size(), nullptr, &cs);
        if (SUCCEEDED(hr)) { ctx->CSSetShader(cs.Get(), nullptr, 0); ctx->Dispatch(1, 1, 1); }
    } else if (type == 1) { ComPtr<ID3D11VertexShader> s; hr = device->CreateVertexShader(p.code.data(), p.code.size(), nullptr, &s); }
    else if (type == 2) { ComPtr<ID3D11GeometryShader> s; hr = device->CreateGeometryShader(p.code.data(), p.code.size(), nullptr, &s); }
    else return 9;
    if (FAILED(hr)) return 4;
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0}; ComPtr<ID3D11Query> done; device->CreateQuery(&qd, &done);
    ctx->End(done.Get()); ctx->Flush();
    BOOL finished = FALSE; for (int i = 0; i < 20000 && ctx->GetData(done.Get(), &finished, sizeof(finished), 0) != S_OK; ++i) Sleep(1);
    return finished ? 0 : 7;
}
int RunCache(const char* self, const fs::path& folder, const std::string& filter) {
    // The newest build of each (name, entry, profile): older ones are stale sources.
    std::map<std::string, CachedProgram> newest; unsigned unreadable = 0;
    for (const auto& entry : fs::directory_iterator(folder)) {
        if (entry.path().extension() != ".cso") continue;
        CachedProgram p; if (!ReadCached(entry.path(), p)) { ++unreadable; continue; }
        if (!filter.empty() && p.name.find(filter) == std::string::npos) continue;
        const auto key = p.name + "|" + p.entry + "|" + p.profile;
        auto found = newest.find(key);
        if (found == newest.end() || found->second.time < p.time) newest[key] = std::move(p);
    }
    std::cout << newest.size() << " distinct cached programs (" << unreadable << " unreadable)\n";
    unsigned failures = 0;
    for (const auto& [key, p] : newest) {
        std::cout << "  " << key << " ... " << std::flush;
        std::string command = "\"\"" + std::string(self) + "\" --force-one \"" + p.file.u8string() + "\"\"";
        const int code = std::system(command.c_str());
        std::cout << (code == 0 ? "ok" : "FAILED exit=" + std::to_string(code)) << "  " << p.file.filename().u8string() << std::endl;
        if (code) ++failures;
    }
    std::cout << failures << " failures\n";
    return failures ? 1 : 0;
}

// A shard: draw every variant listed in <list> from line <start>, writing the line about to be drawn
// to <progress> first. The driver compiles on its own threads, so a crash kills this process while
// the progress file still names the variant.
int DrawShard(const fs::path& list, size_t start, const fs::path& progress) {
    std::ifstream in(list); std::vector<std::string> files; std::string line;
    while (std::getline(in, line)) if (!line.empty()) files.push_back(line);
    for (size_t i = start; i < files.size(); ++i) {
        { std::ofstream p(progress, std::ios::trunc); p << i << "\n"; }
        const int r = DrawOnHardware(fs::u8path(files[i]));
        if (r) { std::ofstream(progress.u8string() + ".soft", std::ios::app) << i << " exit=" << r << " " << files[i] << "\n"; }
    }
    { std::ofstream p(progress, std::ios::trunc); p << files.size() << "\n"; }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--draw-one") return DrawOnHardware(fs::u8path(argv[2]));
    if (argc >= 5 && std::string(argv[1]) == "--draw-shard") return DrawShard(fs::u8path(argv[2]), std::stoul(argv[3]), fs::u8path(argv[4]));
    if (argc >= 3 && std::string(argv[1]) == "--force-one") return ForceCachedOnHardware(fs::u8path(argv[2]));
    if (argc >= 3 && std::string(argv[1]) == "--cache") return RunCache(argv[0], fs::u8path(argv[2]), argc >= 4 ? argv[3] : "");
    if (argc < 2) {
        std::cerr << "usage: ValidateNativeVariants <dumped-programs-dir> [--hardware [--shards N]] [--write <dir>]\n"
                     "       ValidateNativeVariants --cache <shader-cache-dir> [name-filter]\n";
        return 2;
    }
    const fs::path input = fs::u8path(argv[1]);
    bool hardware = false; fs::path write; unsigned shards = 8;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--hardware") hardware = true;
        else if (std::string(argv[i]) == "--write" && i + 1 < argc) write = fs::u8path(argv[++i]);
        else if (std::string(argv[i]) == "--shards" && i + 1 < argc) shards = (std::max)(1u, unsigned(std::stoul(argv[++i])));
    }
    std::vector<Variant> variants; unsigned programs = 0;
    for (const auto& entry : fs::directory_iterator(input)) {
        if (entry.path().extension() != ".dxbc") continue;
        std::ifstream f(entry.path(), std::ios::binary); std::vector<uint8_t> code((std::istreambuf_iterator<char>(f)), {});
        ++programs; Derive(entry.path().stem().u8string(), code, variants);
    }
    std::cout << programs << " programs, " << variants.size() << " variants\n";
    ComPtr<ID3D11Device> debugDevice; ComPtr<ID3D11DeviceContext> debugCtx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &debugDevice, nullptr, &debugCtx))) {
        std::cerr << "no WARP debug device (Graphics Tools optional feature?)\n"; return 2;
    }
    std::map<std::string, unsigned> bad, total, crashed;
    fs::path scratch = write.empty() ? fs::temp_directory_path() / ("native-variants-" + std::to_string(GetCurrentProcessId())) : write;
    if (hardware || !write.empty()) fs::create_directories(scratch);
    unsigned failures = 0;
    std::vector<fs::path> written;
    for (size_t i = 0; i < variants.size(); ++i) {
        const auto& v = variants[i]; ++total[v.kind];
        const auto r = Check(v.code, debugDevice.Get());
        const bool ok = r.disassembled && r.reflected && r.created && !r.debugErrors;
        if (!ok) {
            ++failures; ++bad[v.kind];
            if (bad[v.kind] <= 5)
                std::cout << "INVALID " << v.program << " " << v.kind << " disasm=" << r.disassembled << " reflect=" << r.reflected
                          << " create=" << r.created << " debugErrors=" << r.debugErrors << " " << r.firstError << "\n";
        }
        if (hardware || !write.empty()) {
            written.push_back(scratch / (v.program + "-" + v.kind + ".dxbc"));
            std::ofstream(written.back(), std::ios::binary).write(reinterpret_cast<const char*>(v.code.data()), std::streamsize(v.code.size()));
        }
        if ((i + 1) % 2000 == 0) std::cout << "  checked " << (i + 1) << "/" << variants.size() << std::endl;
    }
    unsigned crashes = 0;
    if (hardware) {
        // A cold compile of every variant: the driver keeps a shader cache per executable, so the
        // shards run from a freshly named copy of this tool, and each variant is new to it.
        const auto self = fs::temp_directory_path() / ("vnv-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + ".exe");
        fs::copy_file(fs::u8path(argv[0]), self, fs::copy_options::overwrite_existing);
        std::vector<std::thread> workers; std::mutex lock;
        for (unsigned s = 0; s < shards; ++s) workers.emplace_back([&, s] {
            const auto list = scratch / ("shard" + std::to_string(s) + ".txt"), progress = scratch / ("shard" + std::to_string(s) + ".progress");
            std::vector<fs::path> mine; for (size_t i = s; i < written.size(); i += shards) mine.push_back(written[i]);
            { std::ofstream out(list); for (const auto& f : mine) out << f.u8string() << "\n"; }
            size_t start = 0;
            while (start < mine.size()) {
                const std::string command = "\"\"" + self.u8string() + "\" --draw-shard \"" + list.u8string() + "\" " + std::to_string(start) + " \"" + progress.u8string() + "\"\"";
                const int code = std::system(command.c_str());
                size_t at = mine.size(); { std::ifstream p(progress); p >> at; }
                if (code == 0 && at >= mine.size()) break;
                if (at >= mine.size()) break;
                std::lock_guard<std::mutex> guard(lock);
                ++crashes; const auto name = mine[at].stem().u8string();
                ++crashed[name.substr(name.find('-') + 1)];
                std::cout << "CRASH " << name << " exit=0x" << std::hex << unsigned(code) << std::dec << std::endl;
                start = at + 1;
            }
        });
        for (auto& w : workers) w.join();
        std::error_code ignored; fs::remove(self, ignored);
    }
    for (const auto& [kind, n] : total)
        std::cout << kind << ": " << n << " variants, " << bad[kind] << " invalid" << (hardware ? ", " + std::to_string(crashed[kind]) + " crashed" : "") << "\n";
    std::cout << failures << " invalid, " << crashes << " hardware crashes\n";
    return failures || crashes ? 1 : 0;
}
