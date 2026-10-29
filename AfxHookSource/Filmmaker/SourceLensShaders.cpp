#include "SourceLensShaders.h"
#include <AfxHookSource2/Filmmaker/Lens/LensFxShaders.h>
#include <regex>
#include <cstring>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace SourceFilmmaker::LensShaders {
namespace {
void Replace(std::string& text,const std::string& old,const std::string& value) {
    const auto at = text.find(old);
    if (at == std::string::npos) throw std::runtime_error("the shared lens shader interface changed: "+old);
    text.replace(at,old.size(),value);
}
struct Texture { const char* name; const char* size; bool scalar; };
const Texture kTextures[] = {
    {"colorTex","dims.xy",false}, {"sceneDepth","dims.zw",true},
    {"focusTex","float2(1,1)",true}, {"focusPrev","float2(1,1)",true},
    {"cocTex","dims.xy",false}, {"tileTex","tileInfo.xy",true},
    {"gatherTex","dims.xy",false}, {"irisMask","float2(128,128)",false},
    {"dofTex","dims.xy",false}, {"brightTex","1/passInfo.xy",false},
    {"ghostTex","lowRes.xy",false}, {"streakTex","max(floor(dims.xy/2),1)",false},
    {"glowTex","lowRes.zw",false}, {"bright8Tex","lowRes.zw",false},
    {"dirtTex","dims.xy",false}, {"ghostSrcTex","lowRes.xy",false},
    {"sourceTex","lowRes.xy",false}, {"source8Tex","lowRes.zw",false},
    {"sourceHTex","float2(lowRes.z,max(floor(dims.y/2),1))",false},
    {"smokeAbsorb","smokeInfo.zw",false}, {"worldMask","dims.xy",true},
    {"sunVisTex","float2(1,1)",true}, {"tex","float2(1,1)",false}
};

// Rewrite balanced method calls; shader arguments themselves contain commas and nested calls.
void Calls(std::string& text,const Texture& texture,const char* method,int count) {
    const std::string token = std::string(texture.name)+"."+method+"(";
    size_t from = 0;
    for (;;) {
        const size_t at = text.find(token,from);
        if (at == std::string::npos) return;
        size_t p = at+token.size(), start = p;
        int depth = 0;
        std::vector<std::string> args;
        for (;p<text.size();++p) {
            if (text[p]=='(') ++depth;
            else if (text[p]==')') { if (!depth) break; --depth; }
            else if (text[p]==',' && !depth) { args.push_back(text.substr(start,p-start)); start=p+1; }
        }
        if (p==text.size()) throw std::runtime_error("unbalanced shared lens shader call");
        args.push_back(text.substr(start,p-start));
        if ((int)args.size()!=count) { from=p+1; continue; } // inactive MSAA branch
        std::string value;
        if (count==1) value = "LensLoad("+std::string(texture.name)+","+args[0]+","+texture.size+")";
        else value = "tex2Dlod("+std::string(texture.name)+",float4("+args[1]+",0,"+args[2]+"))";
        if (texture.scalar) value += ".r";
        text.replace(at,p-at+1,value);
        from=at+value.size();
    }
}
} // namespace

const std::array<const char*,21>& Entries() {
    static const std::array<const char*,21> entries = {"FocusPS","SunPS","CocPS","MipPS","TilePS",
        "DilateHPS","DilateVPS","GatherPS","DenoisePS","BrightPS","Down13PS","SourcePS","SourceHPS",
        "ApertureBlurPS","GhostPS","StreakPS","GlowHPS","GlowVPS","CompositePS","CompositeFactorPS","CompositeAddPS"};
    return entries;
}

bool ReadBindings(const void* bytecode,size_t size,std::vector<Binding>& out) {
    out.clear();
    if (!bytecode || size<8 || size%4) return false;
    const auto* bytes = static_cast<const unsigned char*>(bytecode);
    auto word = [&](size_t p) { uint32_t value=0; std::memcpy(&value,bytes+p*4,4); return value; };
    if (word(0)!=0xffff0300u && word(0)!=0xfffe0300u) return false;
    const size_t words=size/4;
    for (size_t p=1;p<words;) {
        const uint32_t token=word(p),op=token&0xffff;
        if (op==0xffff) return false;
        const size_t count = op==0xfffe ? (token>>16)&0x7fff : (token>>24)&15;
        if (count>words-p-1) return false;
        if (op==0xfffe && count>=8 && word(p+1)==0x42415443u) {
            const auto* table=bytes+(p+2)*4;
            const size_t length=(count-1)*4;
            auto u32 = [&](size_t at) { uint32_t v=0; std::memcpy(&v,table+at,4); return v; };
            auto u16 = [&](size_t at) { uint16_t v=0; std::memcpy(&v,table+at,2); return v; };
            const size_t entries=u32(12),info=u32(16);
            if (u32(0)!=28 || info>length || entries>(length-info)/20) return false;
            std::vector<Binding> pending;
            for (size_t i=0;i<entries;++i) {
                const size_t row=info+i*20,name=u32(row);
                if (name>=length) return false;
                const char* label=reinterpret_cast<const char*>(table+name);
                const auto* end=static_cast<const char*>(std::memchr(label,0,length-name));
                if (!end || end==label) return false;
                Binding binding{std::string(label,end),u16(row+4),u16(row+6),u16(row+8)};
                if (binding.set>3 || !binding.count || (binding.set==3 && binding.first+binding.count>16)
                    || (binding.set==2 && binding.first+binding.count>224)) return false;
                pending.push_back(std::move(binding));
            }
            out=std::move(pending);
            return true;
        }
        p += count+1;
    }
    return false;
}

std::string Source() {
    using namespace Filmmaker::Lens;
    std::string text = std::string(kShaderCommon)+kShaderDof+kShaderFlare+kShaderComposite;
    // Shader Model 3 uses individual constant registers in the same float4 order as the cbuffer.
    const size_t cb = text.find("cbuffer LensConstants"), begin = text.find('{',cb), end = text.find("};",begin);
    if (cb==std::string::npos || begin==std::string::npos || end==std::string::npos)
        throw std::runtime_error("the shared lens constant buffer changed");
    std::istringstream lines(text.substr(begin+1,end-begin-1));
    std::ostringstream constants;
    std::string line;
    int reg = 0;
    while (std::getline(lines,line)) {
        const auto semicolon = line.find(';');
        if (semicolon==std::string::npos) continue;
        constants << line.substr(0,semicolon) << " : register(c" << reg << ");\n";
        reg += line.find("float4x4")!=std::string::npos ? 4 : (line.find("[8]")!=std::string::npos ? 8 : 1);
    }
    text.replace(cb,end+2-cb,constants.str());
    text = std::regex_replace(text,std::regex(R"(Texture2D<(float4|float)> (\w+) : register\(t\d+\);)"),"sampler2D $2;");
    Replace(text,"SamplerState linearClamp : register(s0);","");
    const size_t vs = text.find("VOut VS("), vsEnd = text.find("float Luma(",vs);
    if (vs==std::string::npos || vsEnd==std::string::npos) throw std::runtime_error("the shared lens vertex interface changed");
    text.erase(vs,vsEnd-vs); // D3D9 uses HLAE's textured quad vertex layout.
    Replace(text,"float4 pos : SV_Position","float4 pos : VPOS");
    // D3D9 VPOS names integer pixel centres; SV_Position names them at +0.5. UVs already
    // receive HLAE's half-pixel vertex adjustment, so only explicit pixel coordinates change.
    text = std::regex_replace(text,std::regex("i\\.pos\\.xy"),"(i.pos.xy+.5)");
    text = std::regex_replace(text,std::regex("SV_Target"),"COLOR");
    Replace(text,"LineSum(Texture2D<float4> tex, float2 uv,","LineSum(sampler2D tex, float2 texSize, float2 uv,");
    Replace(text,"uint tw, th; tex.GetDimensions(tw, th);","int tw = (int)texSize.x, th = (int)texSize.y;");
    Replace(text,"for (uint sx = 0; sx < tw; ++sx)","for (int sx = 0; sx < tw; ++sx)");
    Replace(text,"p >> 4","int2(floor(float2(p)/16))");
    // All supported gather qualities have fewer than 256 taps. Arithmetic reversal of these
    // eight integer bits is exact in SM3 floats and preserves the custom iris sample positions.
    const size_t bits = text.find("uint b = idx;"), bitsEnd = text.find("q.y = float(b) * 2.3283064365386963e-10;",bits);
    if (bits==std::string::npos || bitsEnd==std::string::npos) throw std::runtime_error("the iris sequence changed");
    text.replace(bits,bitsEnd+std::string("q.y = float(b) * 2.3283064365386963e-10;").size()-bits,R"HLSL(
        float4 lo = floor(float(idx)/float4(1,2,4,8));
        float4 hi = floor(float(idx)/float4(16,32,64,128));
        q.y = dot(lo-2*floor(lo*.5),float4(.5,.25,.125,.0625))
            + dot(hi-2*floor(hi*.5),float4(.03125,.015625,.0078125,.00390625));
)HLSL");
    std::string scales;
    for (int k=0;k<7;++k) scales += "k=="+std::to_string(k)+" ? kGhostScales["+std::to_string(k)+"] : ";
    scales += "kGhostScales[7]";
    Replace(text,"kGhostScales[k]", "("+scales+")");
    // SM3 colour outputs have four components. The effect's scalar value remains in .r.
    std::string scalarOutputs;
    for (const char* entry : {"FocusPS","SunPS","TilePS","DilateHPS","DilateVPS"}) {
        Replace(text,std::string("float ")+entry+"(VOut i) : COLOR",std::string("float ")+entry+"Value(VOut i)");
        scalarOutputs += std::string("float4 ")+entry+"(VOut i) : COLOR0 { float v = "+entry+"Value(i); return v.xxxx; }\n";
    }
    Replace(text,"float world : COLOR1","float4 world : COLOR1");
    text += scalarOutputs;
    // A D3D9 loop can run at most 255 iterations. Chunk long loops without dropping a tap.
    auto chunk = [&](const char* loop,const char* index,const char* bound) {
        const size_t start = text.find(loop), open = text.find('{',start);
        if (start==std::string::npos || open==std::string::npos) throw std::runtime_error("the lens loop changed");
        size_t close = open+1; int level = 1;
        for (;close<text.size() && level;++close) { if (text[close]=='{') ++level; else if (text[close]=='}') --level; }
        if (level) throw std::runtime_error("unbalanced lens loop");
        text.insert(close,"}");
        const std::string idx=index,n=bound;
        text.replace(start,open-start+1,"[loop] for(int block=0;block<("+n+"+127)/128;++block) { [loop] for(int part=0;part<128;++part) { int "+idx+"=block*128+part; if("+idx+">="+n+") break;");
    };
    chunk("[loop] for (int k = 0; k < 256; ++k)","k","256");
    chunk("[loop] for (int sx = 0; sx < tw; ++sx)","sx","tw");
    chunk("[loop] for (int k = 0; k < taps; ++k)","k","taps");
    Replace(text,"LineSum(sourceHTex, i.uv,","LineSum(sourceHTex, float2(lowRes.z,max(floor(dims.y/2),1)), i.uv,");
    Replace(text,"LineSum(sourceTex, i.uv,","LineSum(sourceTex, lowRes.xy, i.uv,");
    for (const auto& texture : kTextures) { Calls(text,texture,"Load",1); Calls(text,texture,"SampleLevel",3); }
    const char* load = R"HLSL(
float4 LensLoad(sampler2D image,float3 p,float2 size) {
    float2 levelSize = max(floor(size*exp2(-p.z)),1);
    if (any(p.xy<0) || any(p.xy>=levelSize)) return 0;
    return tex2Dlod(image,float4((p.xy+.5)/levelSize,0,p.z));
}
)HLSL";
    return std::string("#define DEPTH_MS 0\n#define COLOR_MS 0\n")+load+text;
}
} // namespace SourceFilmmaker::LensShaders
