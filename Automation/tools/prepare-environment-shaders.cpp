#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderPrograms.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentAmbientOcclusionShaders.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentCasterBoundsShaders.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentIndirectCasterShaders.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentParticleShadowShaders.h"
#include <wrl/client.h>
#include <future>
#include <atomic>
#include <thread>
#include <mutex>
#include <set>
#include <iostream>
#include <algorithm>

using namespace Filmmaker::Environment;
using Microsoft::WRL::ComPtr;
int wmain(int argc,wchar_t** argv){
    if(argc!=2){std::cerr<<"Usage: EnvironmentShaderBuilder <output-directory>\n";return 1;}
    try {
        const auto folder=std::filesystem::absolute(argv[1]).lexically_normal();
        std::filesystem::create_directories(folder);
        EnvironmentShaderCachePaths paths{folder,{}};
        const auto& source=EnvironmentShaderSource();const auto requests=EnvironmentShaderRequests();
        std::vector<std::string> filenames(requests.size());
        std::atomic<size_t> next{0};std::atomic<unsigned> failures{0},reused{0};std::mutex output;
        const auto started=GetTickCount64();
        const unsigned workers=(std::min)(4u,(std::max)(1u,std::thread::hardware_concurrency()));
        std::vector<std::future<void>> jobs;
        for(unsigned worker=0;worker<workers;++worker)jobs.push_back(std::async(std::launch::async,[&]{
            for(;;){
                const auto i=next.fetch_add(1);if(i>=requests.size())break;const auto r=requests[i];
                D3D_SHADER_MACRO macros[]={{"DEPTH_MS",r.ms?"1":"0"},{"COLOR_MS",r.cm?"1":"0"},{nullptr,nullptr}};
                ComPtr<ID3DBlob> code,error;bool cached=false;
                auto hr=CompileEnvironmentShader(source.data(),source.size(),"EnvironmentShaders",macros,r.entry,r.profile,
                    EnvironmentShaderFlags,&code,&error,&cached,&paths);
                if(FAILED(hr)){
                    ++failures;std::lock_guard<std::mutex> lock(output);std::cerr<<r.entry<<": "<<(error?static_cast<const char*>(error->GetBufferPointer()):"compile failed")<<'\n';continue;
                }
                if(cached)++reused;
                filenames[i]=EnvironmentShaderFilename(EnvironmentShaderKey(source.data(),source.size(),"EnvironmentShaders",macros,r.entry,r.profile,EnvironmentShaderFlags,EnvironmentShaderCompiler()));
                // Compilation may succeed with an unwritable cache at runtime. A build must
                // fail instead of installing an incomplete bundle.
                const auto expected=32+EnvironmentShaderKey(source.data(),source.size(),"EnvironmentShaders",macros,r.entry,r.profile,EnvironmentShaderFlags,EnvironmentShaderCompiler()).size()+code->GetBufferSize();
                if(!std::filesystem::is_regular_file(folder/filenames[i])||std::filesystem::file_size(folder/filenames[i])!=expected)++failures;
            }
        }));
        for(auto& job:jobs)job.get();
        // AO has its own source and sample-count macro. Match the runtime cache
        // key exactly instead of compiling these entries from the sky source.
        for(unsigned i=0;i<5;++i){
            const auto entry=i<4?"Horizon":"Filter";
            const std::string samples=std::to_string(i<4?1u<<i:1u);
            D3D_SHADER_MACRO macros[]{{"MSAA_SAMPLES",samples.c_str()},{nullptr,nullptr}};
            ComPtr<ID3DBlob> code,error;bool cached=false;
            const auto hr=CompileEnvironmentShader(AmbientOcclusionShader,sizeof(AmbientOcclusionShader)-1,
                "EnvironmentAmbientOcclusion",macros,entry,"cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,
                &code,&error,&cached,&paths);
            if(FAILED(hr)){
                ++failures;std::cerr<<entry<<": "<<(error?static_cast<const char*>(error->GetBufferPointer()):"compile failed")<<'\n';continue;
            }
            if(cached)++reused;
            const auto key=EnvironmentShaderKey(AmbientOcclusionShader,sizeof(AmbientOcclusionShader)-1,
                "EnvironmentAmbientOcclusion",macros,entry,"cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,EnvironmentShaderCompiler());
            filenames.push_back(EnvironmentShaderFilename(key));
            if(!std::filesystem::is_regular_file(folder/filenames.back())||
               std::filesystem::file_size(folder/filenames.back())!=32+key.size()+code->GetBufferSize())++failures;
        }
        {
            ComPtr<ID3DBlob> code,error;bool cached=false;
            const auto hr=CompileEnvironmentShader(CasterBoundsShader,sizeof(CasterBoundsShader)-1,"EnvironmentCasterBounds",
                nullptr,"Main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&code,&error,&cached,&paths);
            if(FAILED(hr)){++failures;std::cerr<<"Caster bounds: "<<(error?static_cast<const char*>(error->GetBufferPointer()):"compile failed")<<'\n';}
            else {
                if(cached)++reused;
                const auto key=EnvironmentShaderKey(CasterBoundsShader,sizeof(CasterBoundsShader)-1,"EnvironmentCasterBounds",
                    nullptr,"Main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,EnvironmentShaderCompiler());
                filenames.push_back(EnvironmentShaderFilename(key));
                if(!std::filesystem::is_regular_file(folder/filenames.back())||
                   std::filesystem::file_size(folder/filenames.back())!=32+key.size()+code->GetBufferSize())++failures;
            }
        }
        {
            ComPtr<ID3DBlob> code,error;bool cached=false;
            const auto hr=CompileEnvironmentShader(IndirectCasterArgumentsShader,sizeof(IndirectCasterArgumentsShader)-1,"EnvironmentIndirectCasterArguments",
                nullptr,"Main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&code,&error,&cached,&paths);
            if(FAILED(hr)){++failures;std::cerr<<"Indirect caster arguments: "<<(error?static_cast<const char*>(error->GetBufferPointer()):"compile failed")<<'\n';}
            else {if(cached)++reused;
                const auto key=EnvironmentShaderKey(IndirectCasterArgumentsShader,sizeof(IndirectCasterArgumentsShader)-1,"EnvironmentIndirectCasterArguments",
                    nullptr,"Main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,EnvironmentShaderCompiler());
                filenames.push_back(EnvironmentShaderFilename(key));
                if(!std::filesystem::is_regular_file(folder/filenames.back())||
                   std::filesystem::file_size(folder/filenames.back())!=32+key.size()+code->GetBufferSize())++failures;
            }
        }
        for(const auto* entry:{"VS","PS"}){
            const char* profile=entry[0]=='V'?"vs_5_0":"ps_5_0";
            ComPtr<ID3DBlob> code,error;bool cached=false;
            const auto length=strlen(ParticleShadowShader);
            const auto hr=CompileEnvironmentShader(ParticleShadowShader,length,"ParticleShadow",nullptr,entry,profile,
                D3DCOMPILE_ENABLE_STRICTNESS,&code,&error,&cached,&paths);
            if(FAILED(hr)){++failures;std::cerr<<"Particle shadow "<<entry<<": "<<(error?static_cast<const char*>(error->GetBufferPointer()):"compile failed")<<'\n';continue;}
            if(cached)++reused;
            const auto key=EnvironmentShaderKey(ParticleShadowShader,length,"ParticleShadow",nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,EnvironmentShaderCompiler());
            filenames.push_back(EnvironmentShaderFilename(key));
            if(!std::filesystem::is_regular_file(folder/filenames.back())||std::filesystem::file_size(folder/filenames.back())!=32+key.size()+code->GetBufferSize())++failures;
        }
        if(failures){std::cerr<<"Shader bundle preparation failed: "<<failures<<" entries\n";return 1;}
        // Remove only obsolete cache filenames previously recorded by this builder.
        // No recursive cleanup and no paths supplied by a manifest are trusted.
        const std::set<std::string> current(filenames.begin(),filenames.end());
        std::ifstream previous(folder/"manifest.txt");std::string name;
        while(std::getline(previous,name)){
            if(name.size()<=4||name.size()>20||name.substr(name.size()-4)!=".cso")continue;
            if(name.substr(0,name.size()-4).find_first_not_of("0123456789abcdef")!=std::string::npos)continue;
            if(!current.count(name))std::filesystem::remove(folder/name);
        }
        previous.close();
        std::ofstream manifest(folder/"manifest.txt",std::ios::trunc);
        for(const auto& file:filenames)manifest<<file<<'\n';manifest.close();
        if(!manifest)throw std::runtime_error("Could not write shader manifest");
        std::ofstream stamp(folder/"ready.stamp",std::ios::trunc);stamp<<filenames.size()<<'\n';stamp.close();
        if(!stamp)throw std::runtime_error("Could not write shader completion stamp");
        std::cout<<"Environment shader bundle: "<<filenames.size()<<" programs, "<<reused<<" reused, "<<GetTickCount64()-started<<" ms\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
