#define NOMINMAX
#include "RelightLiteGrid.h"
#include "RelightLiteGridMath.h"
#include "../Environment/EnvironmentShadowMath.h"
#include "../Debug/MvmDebugLog.h"
#include <windows.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace Filmmaker::Relight {
using Microsoft::WRL::ComPtr;
namespace {
// The triangles the rays go through: the map's render geometry where it was extracted (with its
// material colours), the collision otherwise.
struct Geometry {
    const std::vector<Vec3>* positions=nullptr;
    const std::vector<uint32_t>* indices=nullptr;
    const std::vector<uint32_t>* albedo=nullptr;
};
Geometry GeometryOf(const Environment::OcclusionMesh& world) {
    const auto* visual=world.visual.get();
    if(visual&&visual->renderGeometry&&visual->rigidIndices.size()>=3&&visual->rigidAlbedo.size()==visual->rigidIndices.size()/3)
        return {&visual->rigidPositions,&visual->rigidIndices,&visual->rigidAlbedo};
    return {&world.positions,&world.indices,nullptr};
}
// The box the probes cover: where people walk (the nav mesh) with room around and above it, inside
// the map's own box. A map's box also holds distant scenery, and probes out there are wasted.
LiteGridLayout LayoutOf(const Environment::OcclusionMesh& world) {
    Vec3 low=world.low,high=world.high;
    if(!world.navPositions.empty()){
        Vec3 a=world.navPositions[0],b=a;
        for(const auto& p:world.navPositions){
            a={(std::min)(a.x,p.x),(std::min)(a.y,p.y),(std::min)(a.z,p.z)};
            b={(std::max)(b.x,p.x),(std::max)(b.y,p.y),(std::max)(b.z,p.z)};
        }
        const Vec3 below{384,384,192},above{384,384,640};
        low={(std::max)(low.x,a.x-below.x),(std::max)(low.y,a.y-below.y),(std::max)(low.z,a.z-below.z)};
        high={(std::min)(high.x,b.x+above.x),(std::min)(high.y,b.y+above.y),(std::min)(high.z,b.z+above.z)};
    }
    return MakeLiteGridLayout(low,high);
}
struct Request {
    std::shared_ptr<const Environment::OcclusionMesh> world;
    LiteGridLayout layout;
    std::vector<LiteLamp> lamps;
    uint64_t lampKey=0;
    Vec3 towardSun{};
    bool sunUp=false;
    LiteGridColours colours;
};
struct Result {
    LiteGridLayout layout;
    std::vector<uint16_t> texels;
};
uint64_t Hash(const void* data,size_t size,uint64_t hash=1469598103934665603ull) {
    const auto* bytes=static_cast<const unsigned char*>(data);
    for(size_t i=0;i<size;++i)hash=(hash^bytes[i])*1099511628211ull;
    return hash;
}
// Half the machine's threads, at most eight, below normal priority: the game keeps its frame rate
// while a bake runs.
unsigned WorkerThreads() {
    const unsigned all=std::thread::hardware_concurrency();
    return std::clamp(all/2,1u,8u);
}
struct Worker {
    std::mutex mutex;
    std::condition_variable wake;
    Request wanted;
    bool hasWanted=false,stop=false;
    std::unique_ptr<Result> ready;
    std::string status="idle";
    // Bumped when the newest request differs in that input: a stage that depends on it gives up.
    std::atomic<uint64_t> worldSerial{0},lampSerial{0},sunSerial{0};
    std::atomic<bool> stopping{false};
    std::thread thread;
    ~Worker() {
        {std::lock_guard<std::mutex> lock(mutex);stop=true;}
        stopping=true;wake.notify_all();
        if(thread.joinable())thread.join();
    }

    void Say(const std::string& text) {std::lock_guard<std::mutex> lock(mutex);status=text;}
    template<class Body,class Stale> bool Parallel(size_t count,Body body,Stale stale) {
        std::atomic<size_t> next{0};std::atomic<bool> aborted{false};
        constexpr size_t chunk=64;
        auto run=[&]{
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
            for(;;){
                if(stopping.load(std::memory_order_relaxed)||stale()){aborted=true;return;}
                const size_t first=next.fetch_add(chunk);
                if(first>=count)return;
                body(first,(std::min)(count,first+chunk));
            }
        };
        std::vector<std::thread> pool;
        for(unsigned i=1;i<WorkerThreads();++i)pool.emplace_back(run);
        run();
        for(auto& thread:pool)thread.join();
        return !aborted;
    }
    void Run() {
        std::shared_ptr<const Environment::OcclusionMesh> world;
        Environment::SurfaceRayGrid rays;LiteGridBake bake;
        bool traced=false,lampsDone=false,sunDone=false,sunUp=false;
        uint64_t lampKey=0;Vec3 towardSun{},fallback{};Geometry geometry;
        double traceMs=0,lampMs=0,sunMs=0;
        using Clock=std::chrono::steady_clock;
        auto since=[](Clock::time_point from){return std::chrono::duration<double,std::milli>(Clock::now()-from).count();};
        for(;;){
            Request r;uint64_t worldAt,lampAt,sunAt;
            {
                std::unique_lock<std::mutex> lock(mutex);
                // The ray grid over a map's render triangles is tens of megabytes; an idle worker
                // gives it back and rebuilds it (a second or two) the next time the sun moves.
                while(!wake.wait_for(lock,std::chrono::seconds(30),[&]{return stop||hasWanted;}))
                    if(rays.Valid()){lock.unlock();rays=Environment::SurfaceRayGrid{};lock.lock();}
                if(stop)return;
                r=std::move(wanted);hasWanted=false;
                worldAt=worldSerial.load();lampAt=lampSerial.load();sunAt=sunSerial.load();
            }
            if(!r.world)continue;
            if(r.world!=world){
                world=r.world;traced=lampsDone=sunDone=false;rays=Environment::SurfaceRayGrid{};
                geometry=GeometryOf(*world);bake.Resize(r.layout);
            }
            const bool recolour=fallback.x!=r.colours.albedo.x||fallback.y!=r.colours.albedo.y||fallback.z!=r.colours.albedo.z;
            const bool sunMoved=sunUp!=r.sunUp||towardSun.x!=r.towardSun.x||towardSun.y!=r.towardSun.y||towardSun.z!=r.towardSun.z;
            const bool needLamps=!lampsDone||lampKey!=r.lampKey||recolour,needSun=!sunDone||sunMoved||recolour;
            if((!traced||needLamps||(needSun&&r.sunUp))&&!rays.Valid()){
                Say("bounce grid: reading the map's triangles");
                const auto start=Clock::now();
                if(geometry.indices->size()<3||!Environment::BuildSurfaceRayGrid(*geometry.positions,*geometry.indices,rays)){
                    world.reset();Say("no usable map triangles: map-wide fill");continue;}
                MvmDebugLog_LinefAlways("relight.lite.grid","rayGrid triangles=%zu render=%u cells=%zu entries=%zu buildMs=%.0f",
                    geometry.indices->size()/3,unsigned(geometry.albedo!=nullptr),rays.CellCount(),rays.triangles.size(),since(start));
            }
            const LiteRayMesh mesh{geometry.positions,geometry.indices,&rays,geometry.albedo};
            const size_t cells=bake.layout.Cells();
            auto newWorld=[&]{return worldSerial.load(std::memory_order_relaxed)!=worldAt;};
            if(!traced){
                Say("bounce grid: looking around each probe");
                const auto start=Clock::now();
                if(!Parallel(cells,[&](size_t a,size_t b){LiteGridTrace(mesh,bake,a,b);},newWorld))continue;
                traced=true;traceMs=since(start);
                // Render triangles are wound for back-face culling, so a back is evidence. Collision
                // triangles are only trusted if most of what the probes see is a front.
                size_t fronts=0,backs=0,inside=0;
                for(const auto& hit:bake.hits){fronts+=hit.kind==LiteHitFront;backs+=hit.kind==LiteHitBack;}
                if(!geometry.albedo&&backs*3>fronts+backs)std::fill(bake.inside.begin(),bake.inside.end(),uint8_t(0));
                for(const auto flag:bake.inside)inside+=flag;
                MvmDebugLog_LinefAlways("relight.lite.grid","probes=%ux%ux%u triangles=%zu threads=%u traceMs=%.0f fronts=%zu backs=%zu inside=%zu",
                    bake.layout.nx,bake.layout.ny,bake.layout.nz,geometry.indices->size()/3,WorkerThreads(),traceMs,fronts,backs,inside);
            }
            if(needLamps){
                lampsDone=false;
                Say("bounce grid: lamps");
                const auto start=Clock::now();
                const auto bins=BinLiteLamps(r.lamps,bake.layout.low,bake.layout.high);
                if(!Parallel(cells,[&](size_t a,size_t b){LiteGridLamps(mesh,bake,r.lamps,bins,r.colours.albedo,a,b);},
                    [&]{return newWorld()||lampSerial.load(std::memory_order_relaxed)!=lampAt;}))continue;
                lampsDone=true;lampKey=r.lampKey;lampMs=since(start);
                MvmDebugLog_LinefAlways("relight.lite.grid","lamps=%zu lampMs=%.0f",r.lamps.size(),lampMs);
            }
            if(needSun){
                sunDone=false;
                Say("bounce grid: sun");
                const auto start=Clock::now();
                if(!r.sunUp)std::fill(bake.sun.begin(),bake.sun.end(),Vec3{});
                else if(!Parallel(cells,[&](size_t a,size_t b){LiteGridSun(mesh,bake,r.towardSun,r.colours.albedo,a,b);},
                    [&]{return newWorld()||sunSerial.load(std::memory_order_relaxed)!=sunAt;}))continue;
                sunDone=true;sunUp=r.sunUp;towardSun=r.towardSun;sunMs=since(start);
                MvmDebugLog_LinefAlways("relight.lite.grid","sunUp=%u sunMs=%.0f",unsigned(sunUp),sunMs);
            }
            fallback=r.colours.albedo;
            const auto start=Clock::now();
            auto result=std::make_unique<Result>();
            result->layout=bake.layout;
            result->texels=LiteGridTexels(bake.layout,LiteGridCompose(bake,r.colours));
            char text[200];
            std::snprintf(text,sizeof(text),"ready: %ux%ux%u probes, %u threads; map %.1f s, lamps %.1f s, sun %.1f s, colours %.0f ms",
                bake.layout.nx,bake.layout.ny,bake.layout.nz,WorkerThreads(),traceMs/1000,lampMs/1000,sunMs/1000,since(start));
            std::lock_guard<std::mutex> lock(mutex);
            ready=std::move(result);status=text;
        }
    }
};
struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Texture3D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11Buffer> constants;
    LiteGridLayout layout;
    ProbePacking packing{};
    bool published=false;
};
std::unique_ptr<Worker> g_worker;
Gpu g_gpu;
// What the newest request was made from.
std::shared_ptr<const Environment::OcclusionMesh> g_world;
LiteGridLayout g_layout;
uint64_t g_key=0,g_lampKey=0;
Vec3 g_towardSun{};
bool g_sunUp=false;

bool Upload(ID3D11Device* device,const Result& result) {
    auto& g=g_gpu;
    const auto& l=result.layout;
    if(!g.texture||!(g.layout==l)){
        g.texture.Reset();g.view.Reset();g.constants.Reset();g.published=false;
        D3D11_TEXTURE3D_DESC d{};d.Width=l.nx;d.Height=l.ny;d.Depth=l.nz*6;d.MipLevels=1;
        d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        g.packing=PackProbe(l.low,l.high,l.nx,l.ny,l.nz);
        std::vector<ProbeRow> rows(3600);std::copy(g.packing.volume.begin(),g.packing.volume.end(),rows.begin());
        D3D11_BUFFER_DESC b{};b.ByteWidth=UINT(rows.size()*sizeof(ProbeRow));b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;b.Usage=D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=rows.data();
        if(FAILED(device->CreateTexture3D(&d,nullptr,&g.texture))||
           FAILED(device->CreateShaderResourceView(g.texture.Get(),nullptr,&g.view))||
           FAILED(device->CreateBuffer(&b,&initial,&g.constants))){
            g.texture.Reset();g.view.Reset();g.constants.Reset();return false;
        }
        g.layout=l;
    }
    ComPtr<ID3D11DeviceContext> ctx;device->GetImmediateContext(&ctx);
    ctx->UpdateSubresource(g.texture.Get(),0,nullptr,result.texels.data(),l.nx*8,l.nx*l.ny*8);
    g.published=true;
    return true;
}
}
void EndLiteGrid() {
    g_worker.reset();
    g_gpu=Gpu{};g_world.reset();g_layout=LiteGridLayout{};g_key=g_lampKey=0;g_towardSun={};g_sunUp=false;
}
std::string LiteGridStatus() {
    if(!g_worker)return "off";
    std::lock_guard<std::mutex> lock(g_worker->mutex);
    return g_worker->status;
}
ProbeBinding PrepareLiteGrid(ID3D11Device* device,const std::shared_ptr<const Environment::OcclusionMesh>& world,
    const Scene& scene) {
    if(!device||!world)return {};
    if(g_gpu.device.Get()!=device){g_gpu=Gpu{};g_gpu.device=device;}
    // Everything the grid is made from. Effect lights (fire, muzzle flashes) are left out, so they
    // never restart a bake.
    const Vec3 sky=SkyAmbient(scene,scene.indirect),sun=SunRadiance(scene);
    uint64_t key=Hash(&sky,sizeof(sky));key=Hash(&sun,sizeof(sun),key);
    key=Hash(&scene.sunAngles,sizeof(scene.sunAngles),key);key=Hash(&scene.albedo,sizeof(scene.albedo),key);
    key=Hash(&scene.liteBounce,sizeof(scene.liteBounce),key);key=Hash(&scene.intensity,sizeof(scene.intensity),key);
    key=Hash(&scene.localLights,sizeof(scene.localLights),key);
    for(const auto& light:scene.lights){
        if(light.transient)continue;
        for(const auto* field:{&light.position,&light.angles,&light.color})key=Hash(field,sizeof(Vec3),key);
        for(const float value:{light.lumens,light.brightnessScale,light.range,light.sourceRadius,light.sizeX,light.sizeY,
            light.innerAngle,light.outerAngle,light.skirtNear,light.skirtFar,light.bounceScale,float(light.enabled),float(light.sceneObject)})key=Hash(&value,sizeof(value),key);
    }
    if(!g_worker){
        g_worker=std::make_unique<Worker>();
        g_worker->thread=std::thread([worker=g_worker.get()]{
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);worker->Run();});
        g_world.reset();g_key=0;
    }
    if(world!=g_world||key!=g_key){
        if(world!=g_world)g_layout=LayoutOf(*world);
        Request r;r.world=world;r.layout=g_layout;
        unsigned off=0,effects=0,dark=0;
        if(scene.localLights)for(const auto& light:scene.lights){
            LiteLamp lamp;
            if(MakeLiteLamp(light,scene.intensity,lamp))r.lamps.push_back(lamp);
            else if(!light.enabled)++off;else if(light.transient)++effects;else ++dark;
        }
        if(MvmDebugLog_Active())MvmDebugLog_LinefAlways("relight.lite.grid","request lights=%zu lamps=%zu off=%u effects=%u noLight=%u",
            scene.lights.size(),r.lamps.size(),off,effects,dark);
        r.lampKey=Hash(r.lamps.data(),r.lamps.size()*sizeof(LiteLamp),r.lamps.size()+1);
        r.towardSun=SunDirection(scene.sunAngles);
        r.sunUp=scene.hasSun&&r.towardSun.z>0&&(sun.x>0||sun.y>0||sun.z>0);
        r.colours=MakeLiteGridColours(sky,sun,scene.albedo,scene.liteBounce);
        const bool sunMoved=r.sunUp!=g_sunUp||r.towardSun.x!=g_towardSun.x||r.towardSun.y!=g_towardSun.y||r.towardSun.z!=g_towardSun.z;
        const bool newWorld=world!=g_world,newLamps=r.lampKey!=g_lampKey;
        g_world=world;g_key=key;g_lampKey=r.lampKey;g_towardSun=r.towardSun;g_sunUp=r.sunUp;
        {
            // With the request, so the worker never pairs an old request with the new serials.
            std::lock_guard<std::mutex> lock(g_worker->mutex);
            if(newWorld)++g_worker->worldSerial;
            if(newLamps)++g_worker->lampSerial;
            if(sunMoved)++g_worker->sunSerial;
            g_worker->wanted=std::move(r);g_worker->hasWanted=true;
        }
        g_worker->wake.notify_one();
    }
    std::unique_ptr<Result> ready;
    {std::lock_guard<std::mutex> lock(g_worker->mutex);ready=std::move(g_worker->ready);}
    if(ready)Upload(device,*ready);
    // A grid made for another map must not light this one.
    if(!g_gpu.published||!(g_gpu.layout==g_layout))return {};
    auto binding=NeutralProbe(device,world->low,world->high);
    if(!binding.constants)return {};
    binding.constants=g_gpu.constants.Get();binding.irradiance=g_gpu.view.Get();binding.packing=g_gpu.packing;
    const auto cell=g_gpu.layout.Cell();
    binding.normalBias=LiteGridNormalBias*(std::min)({cell.x,cell.y,cell.z});
    return binding;
}
}
