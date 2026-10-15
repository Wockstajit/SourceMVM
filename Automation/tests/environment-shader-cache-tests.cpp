#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderPrograms.h"
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
using namespace Filmmaker::Environment;
using Microsoft::WRL::ComPtr;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int wmain(int argc,wchar_t** argv){
    try {
        if(argc==2){
            EnvironmentShaderCachePaths bundlePaths{{},argv[1]};
            const auto& source=EnvironmentShaderSource();const auto started=GetTickCount64();unsigned count=0;
            for(const auto r:EnvironmentShaderRequests()){
                D3D_SHADER_MACRO macros[]={{"DEPTH_MS",r.ms?"1":"0"},{"COLOR_MS",r.cm?"1":"0"},{nullptr,nullptr}};
                ComPtr<ID3DBlob> code,error;bool hit=false,bundled=false;
                require(SUCCEEDED(CompileEnvironmentShader(source.data(),source.size(),"EnvironmentShaders",macros,r.entry,r.profile,
                    EnvironmentShaderFlags,&code,&error,&hit,&bundlePaths,&bundled))&&hit&&bundled,"Production bundle miss");
                ++count;
            }
            require(count==57,"Wrong production request count (including all four per-sample sky variants)");
            std::cout<<"Production bundle: "<<count<<" exact-input hits, no writable cache, "<<GetTickCount64()-started<<" ms\n";
            return 0;
        }
        const auto root=std::filesystem::temp_directory_path()/(
            L"mvm-shader-cache-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        EnvironmentShaderCachePaths paths{root/L"writable",root/L"bundled"};
        std::filesystem::create_directories(paths.bundled);
        const std::string source="float4 Main():SV_Target{return float4(VALUE,0,0,1);}";
        D3D_SHADER_MACRO macros[]={{"VALUE","0.5"},{nullptr,nullptr}};
        const UINT flags=D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3;
        bool hit=false,bundled=false;
        auto compile=[&](const std::string& input){
            ComPtr<ID3DBlob> result,error;
            require(SUCCEEDED(CompileEnvironmentShader(input.data(),input.size(),"test",macros,"Main","ps_5_0",flags,&result,&error,&hit,&paths,&bundled)),"Compile failed");
            return std::string(static_cast<const char*>(result->GetBufferPointer()),result->GetBufferSize());
        };
        const auto original=compile(source);require(!hit&&!bundled,"Cold compile unexpectedly cached");
        require(compile(source)==original&&hit&&!bundled,"Writable cache failed");
        const auto filename=EnvironmentShaderFilename(EnvironmentShaderKey(source.data(),source.size(),"test",macros,"Main","ps_5_0",flags,EnvironmentShaderCompiler()));
        const auto packaged=paths.bundled/filename;
        std::filesystem::rename(paths.writable/filename,packaged);
        // A read-only install must still work when no user cache can be created.
        paths.writable=root/L"not-a-directory";std::ofstream(paths.writable)<<"blocked";
        require(compile(source)==original&&hit&&bundled,"Bundle failed without writable cache");
        auto readFile=[&]{std::ifstream in(packaged,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});};
        auto writeFile=[&](const std::string& bytes){std::ofstream out(packaged,std::ios::binary|std::ios::trunc);out.write(bytes.data(),bytes.size());};
        const auto valid=readFile();
        auto corrupt=valid;corrupt.back()^=1;writeFile(corrupt);
        require(compile(source)==original&&!hit&&!bundled,"Corrupt bytecode was accepted");
        require(readFile()==corrupt,"Runtime overwrote packaged bytecode");
        corrupt=valid;corrupt[40]^=1;writeFile(corrupt);
        require(compile(source)==original&&!hit,"Mismatched exact inputs were accepted");
        writeFile(valid+"trailing");require(compile(source)==original&&!hit,"Trailing data accepted");
        writeFile(valid.substr(0,valid.size()-1));require(compile(source)==original&&!hit,"Truncated data accepted");
        writeFile(valid);
        macros[0].Definition="0.75";require(compile(source)!=original&&!hit,"Changed macros reused stale bytecode");
        macros[0].Definition="0.5";
        require(compile(source+"\n")==original&&!hit,"Changed source reused cache");
        require(compile(source)==original&&hit&&bundled,"Valid bundle did not recover");
        std::filesystem::remove(packaged);
        require(compile(source)==original&&!hit,"Missing bundle fallback failed");
        // Only delete the exact files/directories this test created; no recursive cleanup.
        std::filesystem::remove(paths.writable);
        std::filesystem::remove(root/L"writable");std::filesystem::remove(root/L"bundled");std::filesystem::remove(root);
        std::cout<<"Shader cache: writable, bundled, unavailable user cache, corruption, exact inputs, truncation and fallback passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
