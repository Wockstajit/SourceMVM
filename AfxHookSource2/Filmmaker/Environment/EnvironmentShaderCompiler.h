#pragma once
#include <windows.h>
#include <string>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstdint>
#include <cstring>
namespace Filmmaker::Environment {
// A basename load can reuse CS2's already loaded, older compiler even with
// LOAD_LIBRARY_SEARCH_SYSTEM32. Resolve the full path so tests and the hook
// use the same compiler without replacing any game DLL.
inline HMODULE EnvironmentShaderCompiler() {
    static HMODULE module = []() -> HMODULE {
        wchar_t directory[MAX_PATH];
        UINT length = GetSystemDirectoryW(directory, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return nullptr;
        std::wstring path(directory, length);
        path += L"\\d3dcompiler_47.dll";
        return LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }();
    return module;
}
// Cache only compiler inputs and bytecode. Read during device/feature initialization,
// never during ordinary frame submission. Exact input comparison protects hash collisions.
inline uint64_t EnvironmentShaderHash(const void *data, size_t size) {
    uint64_t hash = 14695981039346656037ull;
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}
inline std::string EnvironmentShaderKey(const void *source,size_t size,const char *name,
                                       const D3D_SHADER_MACRO *macros,const char *entry,
                                       const char *profile,UINT flags,HMODULE module) {
    std::string key;
    auto append = [&](const void *data, size_t length) {
        key.append(reinterpret_cast<const char *>(&length), sizeof(length));
        key.append(static_cast<const char *>(data), length);
    };
    append(source, size);
    append(name, strlen(name));
    append(entry, strlen(entry));
    append(profile, strlen(profile));
    append(&flags, sizeof(flags));
    auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
    auto *pe = reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const unsigned char *>(module) +
                                                          dos->e_lfanew);
    append(&pe->FileHeader.TimeDateStamp, sizeof(DWORD));
    append(&pe->OptionalHeader.SizeOfImage, sizeof(DWORD));
    if (macros)
        for (auto m = macros; m->Name; ++m) {
            append(m->Name, strlen(m->Name));
            const char *value = m->Definition ? m->Definition : "";
            append(value, strlen(value));
        }
    return key;
}
inline std::string EnvironmentShaderFilename(const std::string& key) {
    std::ostringstream filename;
    filename << std::hex << EnvironmentShaderHash(key.data(),key.size()) << ".cso";
    return filename.str();
}
struct EnvironmentShaderCachePaths {
    std::filesystem::path writable,bundled;
};
inline EnvironmentShaderCachePaths DefaultEnvironmentShaderCachePaths() {
    EnvironmentShaderCachePaths paths;
    wchar_t location[32768];
    DWORD length=GetEnvironmentVariableW(L"APPDATA",location,32768);
    if(length&&length<32768)
        paths.writable=std::filesystem::path(location)/L"HLAE"/L"source_mvm"/L"shader-cache";
    HMODULE owner=nullptr;
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&DefaultEnvironmentShaderCachePaths),&owner)){
        length=GetModuleFileNameW(owner,location,32768);
        if(length&&length<32768)paths.bundled=std::filesystem::path(location).parent_path()/L"environment-shaders";
    }
    return paths;
}
inline HRESULT CompileEnvironmentShader(const void *source, size_t size, const char *name,
                                        const D3D_SHADER_MACRO *macros, const char *entry,
                                        const char *profile, UINT flags, ID3DBlob **code, ID3DBlob **errors,
                                        bool *cacheHit = nullptr,
                                        const EnvironmentShaderCachePaths *cachePaths = nullptr,
                                        bool *bundledHit = nullptr) {
    if(cacheHit)*cacheHit=false;
    if(bundledHit)*bundledHit=false;
    if(code)*code=nullptr;
    if(errors)*errors=nullptr;
    if(!code||!source||!name||!entry||!profile||size>1024*1024)return E_INVALIDARG;
    HMODULE module=EnvironmentShaderCompiler();
    auto compile=module?reinterpret_cast<pD3DCompile>(GetProcAddress(module,"D3DCompile")):nullptr;
    using CreateBlob=HRESULT(WINAPI*)(SIZE_T,ID3DBlob**);
    auto create=module?reinterpret_cast<CreateBlob>(GetProcAddress(module,"D3DCreateBlob")):nullptr;
    if(!compile||!create)return E_NOINTERFACE;
    const auto key=EnvironmentShaderKey(source,size,name,macros,entry,profile,flags,module);
    const auto filename=EnvironmentShaderFilename(key);
    EnvironmentShaderCachePaths paths;
    try {paths=cachePaths?*cachePaths:DefaultEnvironmentShaderCachePaths();}catch(...){}
    struct Header {
        uint64_t magic, inputSize, codeSize, checksum;
    };
    constexpr uint64_t magic = 0x324f53434d564dull;
    auto read=[&](const std::filesystem::path& folder,bool bundled){
        if(folder.empty())return false;
        try {
            std::ifstream in(folder/filename, std::ios::binary);
            Header h{};
            in.read(reinterpret_cast<char *>(&h), sizeof(h));
            if (in && h.magic == magic && h.inputSize == key.size() && h.codeSize > 0 &&
                h.codeSize <= 16 * 1024 * 1024) {
                std::string stored(key.size(), '\0');
                in.read(stored.data(), stored.size());
                if (in && stored == key) {
                    std::vector<char> bytes(static_cast<size_t>(h.codeSize));
                    in.read(bytes.data(), bytes.size());
                    if (in && in.peek() == std::char_traits<char>::eof() &&
                        EnvironmentShaderHash(bytes.data(), bytes.size()) == h.checksum &&
                        SUCCEEDED(create(bytes.size(), code))) {
                        memcpy((*code)->GetBufferPointer(), bytes.data(), bytes.size());
                        if (cacheHit)
                            *cacheHit = true;
                        if(bundledHit)*bundledHit=bundled;
                        return true;
                    }
                }
            }
        }catch(...){}
        return false;
    };
    // Packaged bytecode uses the same exact-input and checksum validation as the
    // writable cache. Missing, stale or corrupt bundles still compile normally.
    if(read(paths.writable,false)||read(paths.bundled,true))return S_OK;
    HRESULT hr = compile(source, size, name, macros, nullptr, entry, profile, flags, 0, code, errors);
    if (SUCCEEDED(hr) && *code && !paths.writable.empty())
        try {
            std::filesystem::create_directories(paths.writable);
            const auto path=paths.writable/filename;
            Header h{magic, key.size(), (*code)->GetBufferSize(),
                     EnvironmentShaderHash((*code)->GetBufferPointer(), (*code)->GetBufferSize())};
            auto temporary = path;
            temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char *>(&h), sizeof(h));
                out.write(key.data(), key.size());
                out.write(static_cast<const char *>((*code)->GetBufferPointer()), h.codeSize);
                out.close();
                if (!out)
                    throw std::runtime_error("Shader cache write failed");
            }
            MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        } catch (...) {
        }
    return hr;
}
} // namespace Filmmaker::Environment
