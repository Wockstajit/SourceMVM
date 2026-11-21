#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderPrograms.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentAmbientOcclusionShaders.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentCasterBoundsShaders.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentIndirectCasterShaders.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentParticleShadowShaders.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentGroundFogShaders.h"
#include <wrl/client.h>
#include <future>
#include <atomic>
#include <thread>
#include <functional>
#include <process.h>
#include <cstdlib>
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
        // MVM_SHADER_DUMP=<file> writes the whole program text there and stops: for timing one
        // entry with fxc by hand when the bundle gets slow.
        if(const char* dump=std::getenv("MVM_SHADER_DUMP")){
            std::ofstream out(dump,std::ios::binary|std::ios::trunc);out.write(source.data(),std::streamsize(source.size()));
            return out?0:1;
        }
        std::vector<std::string> filenames(requests.size());
        std::atomic<size_t> next{0};std::atomic<unsigned> failures{0},reused{0};std::mutex output;
        const auto started=GetTickCount64();
        // One compile is one thread, and the bundle takes as long as its slowest program if every
        // program has a thread to itself from the start. So: a worker per logical core (two left
        // for the rest of the build), and the slow programs handed out first. With four workers in
        // list order the four big surface programs started minutes apart and the bundle took twice
        // as long as any one of them.
        // The process keeps normal priority. At high priority the workers outranked every program on
        // the PC: Windows runs a higher class first, so the desktop, the game and every other window
        // got a core only when the scheduler's starvation boost came round, and the PC froze for the
        // minutes the bundle took (2026-11-18). Any class above normal does the same. At normal
        // priority Windows shares the cores and still favours the window in use, so the bundle runs
        // at full speed on an idle PC and only slows while something else is busy. (A quarter of the
        // cores at idle priority starved it instead: 18 minutes beside CS2.) The big programs hold
        // about four gigabytes between them. MVM_SHADER_WORKERS sets another count, and
        // MVM_SHADER_PRIORITY=below or idle gives way further.
        DWORD priority=NORMAL_PRIORITY_CLASS;
        if(const char* asked=std::getenv("MVM_SHADER_PRIORITY")){
            const std::string level=asked;
            priority=level=="idle"?IDLE_PRIORITY_CLASS:level=="below"?BELOW_NORMAL_PRIORITY_CLASS:level=="high"?HIGH_PRIORITY_CLASS:NORMAL_PRIORITY_CLASS;
        }
        SetPriorityClass(GetCurrentProcess(),priority);
        const unsigned cores=std::thread::hardware_concurrency();
        unsigned wanted=(std::max)(2u,cores>2?cores-2:cores);
        if(const char* asked=std::getenv("MVM_SHADER_WORKERS"))wanted=(std::max)(1u,unsigned(std::atoi(asked)));
        const unsigned workers=(std::max)(1u,(std::min)(unsigned(requests.size()),wanted));
        std::vector<size_t> order(requests.size());
        for(size_t i=0;i<order.size();++i)order[i]=i;
        const auto weight=[&](size_t i){
            const std::string entry=requests[i].entry;
            return entry=="AtmospherePS"||entry=="AtmospherePixelPS"?5:entry=="CompositePS"?4:entry=="MaterialMeshPS"||entry=="MeshPS"||entry=="CompositePixelPS"?3:entry=="SkyVolumePS"||entry=="VolumePS"||entry=="ExactWalkPS"?2:0;
        };
        std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){return weight(a)>weight(b);});
        // The compiler recurses deeply on the largest programs. On a default one-megabyte thread
        // stack it overflowed inside D3DCompile and the worker stopped for good, with no error and
        // no CPU use, so the build hung. The workers get 256 MB of stack each.
        const std::function<void()> work=[&]{
            for(;;){
                const auto turn=next.fetch_add(1);if(turn>=requests.size())break;const auto i=order[turn];const auto r=requests[i];
                D3D_SHADER_MACRO macros[]={{"DEPTH_MS",r.ms?"1":"0"},{"COLOR_MS",r.cm?"1":"0"},{nullptr,nullptr}};
                ComPtr<ID3DBlob> code,error;bool cached=false;const auto began=GetTickCount64();
                auto hr=CompileEnvironmentShader(source.data(),source.size(),"EnvironmentShaders",macros,r.entry,r.profile,
                    EnvironmentShaderFlags,&code,&error,&cached,&paths);
                if(FAILED(hr)){
                    ++failures;std::lock_guard<std::mutex> lock(output);std::cerr<<r.entry<<": "<<(error?static_cast<const char*>(error->GetBufferPointer()):"compile failed")<<'\n';continue;
                }
                if(cached)++reused;
                // MVM_SHADER_TIMES=1 lists what each program cost: one slow entry otherwise looks like a hang.
                if(!cached&&std::getenv("MVM_SHADER_TIMES")){std::lock_guard<std::mutex> lock(output);std::cout<<r.entry<<" ms="<<r.ms<<" cm="<<r.cm<<": "<<GetTickCount64()-began<<" ms"<<std::endl;}
                filenames[i]=EnvironmentShaderFilename(EnvironmentShaderKey(source.data(),source.size(),"EnvironmentShaders",macros,r.entry,r.profile,EnvironmentShaderFlags,EnvironmentShaderCompiler()));
                // Compilation may succeed with an unwritable cache at runtime. A build must
                // fail instead of installing an incomplete bundle.
                const auto expected=32+EnvironmentShaderKey(source.data(),source.size(),"EnvironmentShaders",macros,r.entry,r.profile,EnvironmentShaderFlags,EnvironmentShaderCompiler()).size()+code->GetBufferSize();
                if(!std::filesystem::is_regular_file(folder/filenames[i])||std::filesystem::file_size(folder/filenames[i])!=expected)++failures;
            }
        };
        std::vector<HANDLE> jobs;
        for(unsigned worker=0;worker<workers;++worker)
            if(const auto thread=_beginthreadex(nullptr,256u<<20,[](void* run)->unsigned{(*static_cast<const std::function<void()>*>(run))();return 0;},
                const_cast<std::function<void()>*>(&work),STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr))jobs.push_back(reinterpret_cast<HANDLE>(thread));
        if(jobs.empty())work();
        for(const auto job:jobs){WaitForSingleObject(job,INFINITE);CloseHandle(job);}
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
        // Ground fog fluid (EnvironmentGroundFog.cpp): same name, entries and flags as the runtime.
        std::vector<std::pair<const char*,const char*>> groundFogPrograms{{"OverlayVS","vs_5_0"},{"OverlayPS","ps_5_0"}};
        for(const auto* entry:GroundFogShaderEntries)groundFogPrograms.push_back({entry,"cs_5_0"});
        for(const auto& program:groundFogPrograms){
            const char* entry=program.first;const char* profile=program.second;
            constexpr UINT flags=D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3;
            ComPtr<ID3DBlob> code,error;bool cached=false;
            const auto hr=CompileEnvironmentShader(GroundFogShader,sizeof(GroundFogShader)-1,"EnvironmentGroundFog",nullptr,entry,profile,
                flags,&code,&error,&cached,&paths);
            if(FAILED(hr)){++failures;std::cerr<<"Ground fog "<<entry<<": "<<(error?static_cast<const char*>(error->GetBufferPointer()):"compile failed")<<'\n';continue;}
            if(cached)++reused;
            const auto key=EnvironmentShaderKey(GroundFogShader,sizeof(GroundFogShader)-1,"EnvironmentGroundFog",nullptr,entry,profile,flags,EnvironmentShaderCompiler());
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
