#define NOMINMAX
#include "SceneKeys.h"
#include "SceneKeyframeCatalog.h"
#include "TimelinePlayhead.h"
#include "../Filmmaker.h"
#include "../Demo/DemoProjectStore.h"
#include "../Demo/PlayingDemoPath.h"
#include "../Environment/EnvironmentScene.h"
#include "../World/WorldTransient.h"
#include "../World/SkyboxLibrary.h"
#include "../World/CloudFx.h"
#include "../World/SunFx.h"
#include "../World/SkyOrient.h"
#include "../World/NightMode.h"
#include "../World/WorldModulation.h"
#include "../Debug/MvmDebugLog.h"
#include "../../SceneSystem.h"
#include "../../../Shared/AfxConsole.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include <Windows.h>
#include <iomanip>

namespace Filmmaker {
namespace {
using namespace SceneKeys;
SceneKeyframeTrack g_track;
std::wstring g_demo;
std::string g_map, g_error;
bool g_loaded = false, g_dirty = false, g_failed = false, g_gesture = false, g_reassert = false;
ULONGLONG g_saveAt = 0;
double g_captureTick = -1;
std::map<std::wstring, TrackData> g_unsaved;
std::map<std::string, std::string> g_preparedSkies;
std::string g_lastValidSky;
bool g_haveValidSky = false;
int g_selectedEvent = -1, g_activeChannel = -1;

std::map<std::string, Value> Baselines() {
    const auto world = Environment::CaptureWorldLook();
    const auto env = Environment::CaptureAnimationValues();
    std::map<std::string, Value> out;
    for (const auto& s : Catalog()) {
        Value v = s.initial;
        if (s.type == ChannelType::SkyId) v.asset = world.sky;
        else {
            const auto& fields = s.family == "effects" ? world.fields : env;
            const auto f = fields.find(s.source);
            if (f != fields.end() && f->second.size() >= size_t(s.offset + s.components))
                v.numbers.assign(f->second.begin() + s.offset, f->second.begin() + s.offset + s.components);
        }
        out[s.id] = v;
    }
    return out;
}
Value Resolved(const ChannelSpec& s, double tick, const std::map<std::string, Value>& base) {
    const auto b = base.find(s.id); const auto& baseline = b == base.end() ? s.initial : b->second;
    if (g_track.data.muted.count(s.id)) return baseline; // disabled: the keys stay, nothing applies
    auto f = g_track.data.fields.find(s.id);
    return f == g_track.data.fields.end() ? baseline : Sample(s, f->second, tick, baseline);
}
Value Editable(const ChannelSpec& s, double tick, const std::map<std::string, Value>& base) {
    auto f = g_track.data.fields.find(s.id);
    if (f != g_track.data.fields.end()) for (const auto& k : f->second) if (k.tick == std::round(tick)) return k.value;
    Value v = Resolved(s, tick, base);
    // A key must be a value the channel can hold. Baselines can carry a mode sentinel (CONFIG
    // exposure "auto" = -1) that is outside the keyable range; key the channel's neutral instead.
    if (s.type != ChannelType::SkyId && !ValidValue(s, v)) v = s.initial;
    return v;
}
void MarkDirty() { g_dirty = true; g_failed = false; g_saveAt = GetTickCount64() + 500; }
bool SaveData(const std::wstring& demo, const TrackData& data) {
    if (demo.empty()) return false;
    try {
        // The store's write is already atomic (temp file + flush + rename inside the
        // project folder), so this module only hands it the encoded section payload.
        const std::string text = Encode(data) + "\n";
        if (!DemoProjectStore_WriteSection(ProjectGame::Cs2, demo, ProjectSection::SceneKeys, text))
            throw std::runtime_error("Cannot write the scenekeys section of project.mvm");
        return true;
    } catch (const std::exception& e) { g_error = e.what(); return false; }
}
void Flush() {
    if (g_dirty && !g_demo.empty()) {
        if (SaveData(g_demo, g_track.data)) { g_dirty = false; g_failed = false; g_unsaved.erase(g_demo); }
        else { g_failed = true; g_unsaved[g_demo] = g_track.data; }
    }
    g_saveAt = GetTickCount64() + 2000;
}
void PrepareAssets() {
    // Only at load/map change/asset edit, never on the capture sample path.
    g_preparedSkies.clear(); g_preparedSkies[""] = "";
    const auto f = g_track.data.fields.find("sky"); if (f == g_track.data.fields.end()) return;
    SkyboxLibrary_Refresh();
    for (const auto& k : f->second) {
        const auto& id = k.value.asset;
        if (g_preparedSkies.count(id)) continue;
        bool found = false;
        for (const auto& entry : SkyboxLibrary_Entries()) if (entry.name == id) {
            if (SceneSystem_ResolveMaterial(entry.material.c_str())) { g_preparedSkies[id] = entry.material; found = true; }
            break;
        }
        if (!found) { g_error = "Missing sky: " + id + ". Keeping the previous valid sky."; MvmDebugLog_Linef("scenekeys.asset", "%s", g_error.c_str()); }
    }
}
void Load() {
    g_track = SceneKeyframeTrack{}; g_dirty = false; g_failed = false; g_error.clear(); g_gesture = false;
    g_selectedEvent = -1; g_activeChannel = -1; g_haveValidSky = false; g_preparedSkies.clear(); g_preparedSkies[""] = "";
    auto cached = g_unsaved.find(g_demo);
    if (cached != g_unsaved.end()) { g_track.data = cached->second; g_dirty = true; g_failed = true; g_saveAt = GetTickCount64() + 500; }
    else if (!g_demo.empty()) try {
        std::string text;
        if (!DemoProjectStore_ReadSection(ProjectGame::Cs2, g_demo, ProjectSection::SceneKeys, text)) return;
        if (text.size() > 16 * 1024 * 1024) throw std::runtime_error("Scene key section exceeds 16 MiB");
        TrackData pending;
        if (!Decode(text, Catalog(), pending, g_error)) { g_failed = true; return; }
        g_track.data = std::move(pending);
    } catch (const std::exception& e) { g_error = e.what(); g_failed = true; }
    PrepareAssets();
}
const ChannelSpec* Channel(int ch) { return ch >= 0 && ch < int(Catalog().size()) ? &Catalog()[ch] : nullptr; }
double Number(const char* text, double fallback = 0) {
    if (!text || !*text) return fallback;
    char* end = nullptr; const double n = std::strtod(text, &end);
    return end && *end == 0 && std::isfinite(n) ? n : std::numeric_limits<double>::quiet_NaN();
}
Value ParseValue(const ChannelSpec& s, int argc, advancedfx::ICommandArgs* args, int start) {
    Value v;
    if (s.type == ChannelType::SkyId) v.asset = start < argc && std::string(args->ArgV(start)) != "default" ? args->ArgV(start) : "";
    else for (int i = start; i < argc; ++i) v.numbers.push_back(Number(args->ArgV(i)));
    return v;
}
void Apply(double tick) {
    auto base = Baselines();
    WorldTransient::State state;
    const auto world = Environment::CaptureWorldLook();
    for (const auto& s : Catalog()) {
        auto f = g_track.data.fields.find(s.id); if (f == g_track.data.fields.end()) continue;
        if (g_track.data.muted.count(s.id)) continue; // disabled from the graph's right-click menu
        bool active = false; auto value = Sample(s, f->second, tick, base[s.id], &active); if (!active) continue;
        if (s.type == ChannelType::SkyId) {
            auto prepared = g_preparedSkies.find(value.asset);
            if (prepared != g_preparedSkies.end()) { g_lastValidSky = prepared->second; g_haveValidSky = true; }
            if (g_haveValidSky) { state.hasSky = true; state.skyMaterial = g_lastValidSky; }
        } else {
            auto& target = state.fields[s.source];
            if (target.empty() && s.family == "effects") { auto w = world.fields.find(s.source); if (w != world.fields.end()) target = w->second; }
            if (target.size() < size_t(s.offset + s.components)) target.resize(s.offset + s.components);
            for (int i = 0; i < s.components; ++i) target[s.offset + i] = value.numbers[i];
        }
    }
    const auto previous = WorldTransient::Read();
    const bool changed = WorldTransient::Publish(state);
    if (!changed && !g_reassert) return;
    auto dirty = [&](const char* id) {
        auto a = previous->fields.find(id); auto b = state.fields.find(id);
        return (g_reassert && (a != previous->fields.end() || b != state.fields.end())) ||
            (a == previous->fields.end()) != (b == state.fields.end()) ||
            (a != previous->fields.end() && b != state.fields.end() && a->second != b->second);
    };
    if (dirty("exposure")) NightMode_RunFrame();
    if (dirty("cloudDensity") || dirty("cloudBrightness") || dirty("cloudSize") || dirty("cloudDrift")) CloudFx_Apply();
    if (dirty("sunPosition") || dirty("sunPositionOverride") || dirty("sunSize") || dirty("sunBrightness") || dirty("sunGlow") || dirty("sunVisible") || dirty("lightColor") || dirty("lightAmount") || dirty("lightUniform") || dirty("lightDirection")) SunFx_Apply();
    if (dirty("skyDirection") || dirty("skyMirror") || dirty("skyTargets")) SkyOrient_RunFrame();
    if (dirty("worldTint") || dirty("worldColors") || dirty("worldStrength")) WorldModulation_RefreshTransient();
    if (dirty("skyTint") || dirty("skyColor") || dirty("cloudTint") || dirty("cloudColor") || previous->hasSky != state.hasSky || previous->skyMaterial != state.skyMaterial || (g_reassert && state.hasSky)) SceneSystem_RefreshTransientSky();
    g_reassert = false;
    if (MvmDebugLog_Active()) { static ULONGLONG last = 0; if (GetTickCount64() - last > 500) { last = GetTickCount64(); MvmDebugLog_Linef("scenekeys.sample", "tick=%.3f fields=%zu rev=%d baselineUnchanged=1", tick, state.fields.size(), g_track.revision); } }
}
void CatalogJson(JsonBuilder& j) {
    j.BeginArray();
    for (const auto& s : Catalog()) {
        j.BeginObject(); j.StringField("id",s.id); j.StringField("label",s.label); j.StringField("family",s.family); j.StringField("group",s.group); j.StringField("groupId",s.groupId);
        j.StringField("type",TypeName(s.type)); j.IntField("components",s.components); j.DoubleField("min",s.min); j.DoubleField("max",s.max);
        if (s.id == "env.weather.type" || s.type == ChannelType::SkyId) {
            j.Key("options"); j.BeginArray();
            if (s.type == ChannelType::SkyId) {
                j.BeginObject(); j.StringField("value","default"); j.StringField("label","Map default"); j.EndObject();
                for (const auto& e : SkyboxLibrary_Entries()) { j.BeginObject(); j.StringField("value",e.name); j.StringField("label",e.label); j.EndObject(); }
            } else {
                const char* names[] = {"Clear","Rain","Snow","Ash","Embers","Motes","Smoke"};
                for (int i=0; i<7; ++i) { j.BeginObject(); j.IntField("value",i); j.StringField("label",names[i]); j.EndObject(); }
            }
            j.EndArray();
        }
        j.EndObject();
    }
    j.EndArray();
}
} // namespace

void SceneKeys_Flush() { g_track.EndEdit(); Flush(); for (auto it = g_unsaved.begin(); it != g_unsaved.end();) { if (SaveData(it->first,it->second)) it=g_unsaved.erase(it); else ++it; } }
void SceneKeys_Reassert() { g_reassert = true; }
void SceneKeys_ApplyCaptureTick(double tick) { g_captureTick = tick; }
void SceneKeys_RunFrame() {
    const auto demo = ResolvePlayingDemoPath();
    const auto map = Environment::CurrentMapName();
    if (!g_loaded || demo != g_demo) { g_track.EndEdit(); Flush(); g_demo = demo; g_loaded = true; Load(); g_reassert = true; }
    if (map != g_map) { g_map = map; PrepareAssets(); g_reassert = true; }
    if (g_dirty && GetTickCount64() >= g_saveAt && !g_gesture) Flush();
    Apply(g_captureTick >= 0 ? g_captureTick : TimelinePlayhead_Tick()); g_captureTick = -1;
}
int SceneKeys_Revision() { return g_track.revision; }
std::string SceneKeys_StateJson() {
    JsonBuilder j; j.BeginObject(); const auto base = Baselines(); const double tick = TimelinePlayhead_Tick();
    j.Key("catalog"); CatalogJson(j);
    j.Key("armed"); j.BeginObject(); for (const auto& id : g_track.data.armed) j.BoolField(id.c_str(),true); j.EndObject();
    j.Key("resolved"); j.BeginObject(); for (const auto& s : Catalog()) { j.Key(s.id.c_str()); WriteValue(j,Resolved(s,tick,base),s.type==ChannelType::SkyId); } j.EndObject();
    j.Key("byField"); j.BeginObject();
    for (const auto& f : g_track.data.fields) { const auto* s = FindSpec(Catalog(),f.first); if (!s) continue;
        j.Key(f.first.c_str()); j.BeginArray(); for (const auto& k : f.second) { j.BeginObject(); j.IntField("id",k.id); j.DoubleField("tick",k.tick); j.Key("value"); WriteValue(j,k.value,s->type==ChannelType::SkyId); j.EndObject(); } j.EndArray();
    } j.EndObject();
    j.Key("events"); j.BeginArray();
    for (const auto& ev : Events(g_track.data)) { j.BeginObject(); j.DoubleField("tick",ev.tick); j.Key("fields"); j.BeginArray();
        for (const auto& id : ev.fields) { j.BeginObject(); j.StringField("id",id); j.EndObject(); } j.EndArray(); j.EndObject();
    } j.EndArray();
    j.IntField("selectedEvent",g_selectedEvent); j.IntField("rev",g_track.revision); j.BoolField("canUndo",g_track.CanUndo()); j.BoolField("canRedo",g_track.CanRedo());
    j.StringField("save",g_failed ? "failed" : g_dirty ? "saving" : "saved"); j.StringField("error",g_error); j.EndObject(); return j.Str();
}
std::string SceneKeys_ChannelsJson() {
    JsonBuilder j; j.BeginArray(); const auto base = Baselines();
    bool solo = false; for (const auto& c : g_track.solo) if (c.second) solo = true;
    const char* colors[] = {"#e4ba69","#8fd29c","#87bafa","#d8a0dd","#a8a3e8","#73cbd0"};
    static const std::vector<SceneKeys::Key> kNoKeys;
    for (int ch=0; ch<int(Catalog().size()); ++ch) { const auto& s = Catalog()[ch];
        auto f = g_track.data.fields.find(s.id);
        const bool keyed = f != g_track.data.fields.end() && !f->second.empty();
        // An ARMED property gets its strip before it has a single key -- that empty strip is where
        // you click to create the first one. Without it, turning a property on in the inspector
        // would show nothing anywhere and there would be no way to key it.
        if (!keyed && !g_track.data.armed.count(s.id)) continue;
        const auto& keys = keyed ? f->second : kNoKeys; bool edit = !solo || g_track.solo[s.id];
        // The strip's value axis is EXACTLY the property's coded range. It used to expand over the
        // keyed span and then pad 8% past that, which pushed Exposure (min 0) to a -1.88 floor and
        // made it look like a lighting value could go negative. The keyed curve and the typeable
        // number are bounded by the same min/max.
        double lo=s.min, hi=s.max;
        if (s.type == ChannelType::Colour || s.type == ChannelType::SkyId) { lo=0; hi=1; }
        j.BeginObject(); j.IntField("ch",ch); j.StringField("fieldId",s.id); j.StringField("name",s.label); j.StringField("family",s.family); j.StringField("group",s.group);
        j.StringField("type",TypeName(s.type)); j.StringField("color",colors[ch%6]); j.BoolField("vis",!g_track.hidden[s.id] && edit); j.BoolField("solo",g_track.solo[s.id]); j.BoolField("edit",edit);
        j.StringField("groupId",(s.family+"."+s.groupId).c_str()); j.BoolField("muted",g_track.data.muted.count(s.id)!=0);
        j.DoubleField("min",lo); j.DoubleField("max",hi); j.Key("value"); WriteValue(j,Editable(s,TimelinePlayhead_Tick(),base),s.type==ChannelType::SkyId);
        j.Key("keys"); j.BeginArray();
        for (size_t i=0; i<keys.size(); ++i) { const auto& k=keys[i]; j.BeginObject(); j.IntField("id",k.id); j.DoubleField("t",k.tick);
            const double scale = 1.0 / std::max(size_t(1),keys.size()-1);
            j.DoubleField("v",s.type==ChannelType::Colour || s.type==ChannelType::SkyId ? i*scale : k.value.numbers[0]);
            j.BoolField("sel",g_track.selected.count(k.id)!=0); j.IntField("direction",int(k.direction));
            auto left=k.left, right=k.right; if(s.type==ChannelType::Colour){left.dv*=scale;right.dv*=scale;}
            WriteHandle(j,"cl",left); WriteHandle(j,"cr",right);
            if(s.type==ChannelType::Colour) { std::ostringstream sw; sw << '#' << std::hex << std::setfill('0'); for(int c=0;c<3;++c)sw<<std::setw(2)<<int(std::round(k.value.numbers[c])); j.StringField("swatch",sw.str()); }
            j.EndObject();
        } j.EndArray(); j.EndObject();
    } j.EndArray(); return j.Str();
}
// A SUGGESTED starting window only -- fit to the keyed events, padded, or a default span around
// the playhead when there are none yet. The JS owns pan/zoom after that (same contract as the
// camera graph's m_viewInit): this value is read once when the dock's scene view has not been
// seeded, and never again, so moving the playhead never recentres, pans or zooms what you are
// looking at. That used to happen here (an anchor that followed the playhead every frame) and it
// made existing keys hard to find the moment you scrubbed away from them.
void SceneKeys_AppendGraphState(std::ostringstream& out) {
    double lo = TimelinePlayhead_Tick() - 1000, hi = lo + 2000; const auto events = Events(g_track.data);
    if (!events.empty()) { lo = events.front().tick; hi = events.back().tick; double pad = std::max(64.0, (hi - lo) * .1); lo -= pad; hi += pad; }
    out << ",\"rev\":" << g_track.revision << ",\"viewT0\":" << std::max(0.0, lo) << ",\"viewT1\":" << hi
        << ",\"selCount\":" << g_track.selected.size() << ",\"selCh\":" << g_activeChannel
        << ",\"canUndo\":" << (g_track.CanUndo()?"true":"false") << ",\"canRedo\":" << (g_track.CanRedo()?"true":"false")
        << ",\"channels\":" << SceneKeys_ChannelsJson();
}

void SceneKeys_RunCommand(int argc, advancedfx::ICommandArgs* args, const char*) {
    if(argc<3)return;
    const std::string op=args->ArgV(2); const auto base=Baselines(); const double tick=std::round(TimelinePlayhead_Tick());
    auto arg=[&](int i)->const char* { return i<argc?args->ArgV(i):""; };
    // Read-only report for automation and triage: every armed or keyed property, its keys, the value
    // resolved at the playhead, and what Apply actually published to the world this frame.
    if(op=="status") {
        auto fmt=[](const Value& v){std::ostringstream s;if(v.numbers.empty())s<<'"'<<v.asset<<'"';else for(size_t i=0;i<v.numbers.size();++i)s<<(i?" ":"")<<v.numbers[i];return s.str();};
        const auto published=WorldTransient::Read();
        advancedfx::Message("scenekeys: tick=%.0f rev=%d save=%s undo=%d redo=%d events=%zu error='%s'\n",tick,g_track.revision,
            g_failed?"failed":g_dirty?"saving":"saved",g_track.CanUndo()?1:0,g_track.CanRedo()?1:0,Events(g_track.data).size(),g_error.c_str());
        for(const auto& s:Catalog()){auto f=g_track.data.fields.find(s.id);const bool armed=g_track.data.armed.count(s.id)!=0;
            if(!armed&&f==g_track.data.fields.end())continue;
            std::ostringstream keys;if(f!=g_track.data.fields.end())for(const auto& k:f->second)keys<<' '<<k.tick<<'='<<fmt(k.value);
            std::ostringstream pub;auto p=published->fields.find(s.source);
            if(s.type!=ChannelType::SkyId&&p!=published->fields.end()&&p->second.size()>=size_t(s.offset+s.components))for(int i=0;i<s.components;++i)pub<<(i?" ":"")<<p->second[s.offset+i];else pub<<'-';
            advancedfx::Message("  %s armed=%d keys=[%s ] resolved=%s published=%s\n",s.id.c_str(),armed?1:0,keys.str().c_str(),fmt(Resolved(s,tick,base)).c_str(),pub.str().c_str());}
        return;
    }
    if(op=="undo"||op=="redo") { if(op=="undo"?g_track.Undo():g_track.Redo()){MarkDirty();PrepareAssets();}g_gesture=false;return; }
    if(op=="editbegin") { g_track.BeginEdit();g_gesture=true;return; }
    if(op=="editend") { if(g_track.EndEdit())MarkDirty();g_gesture=false;return; }
    if(op=="select") { auto ev=Events(g_track.data);int n=atoi(arg(3));g_track.selected.clear();g_selectedEvent=n;
        if(n>=0&&n<int(ev.size()))for(const auto& f:g_track.data.fields)for(const auto& k:f.second)if(k.tick==ev[n].tick)g_track.selected.insert(k.id);
        ++g_track.revision;return;
    }
    if(op=="graph") {
        const std::string action=arg(3); const auto* spec=Channel(atoi(arg(4))); const int kid=atoi(arg(5));
        if(action=="playhead") { if(std::string(arg(4))=="release")TimelinePlayhead_EndScrub();else {double n=Number(arg(4));if(std::isfinite(n))TimelinePlayhead_BeginScrub(std::max(0.0,n));}return; }
        if(action=="undo"||action=="redo") {if(action=="undo"?g_track.Undo():g_track.Redo()){MarkDirty();PrepareAssets();}g_gesture=false;return;}
        if(action=="editbegin"){g_track.BeginEdit();g_gesture=true;return;}
        if(action=="editend"){if(g_track.EndEdit())MarkDirty();g_gesture=false;return;}
        if(action=="chan"&&spec){std::string how=arg(5);if(how=="solo"||how=="unsolo")g_track.solo[spec->id]=how=="solo";else g_track.hidden[spec->id]=how=="hide";g_activeChannel=atoi(arg(4));++g_track.revision;return;}
        if(action=="selclear"||action=="selall"||action=="select"||action=="selset") {
            if(std::string(arg(6))!="add")g_track.selected.clear();
            if(action=="selall")for(const auto& f:g_track.data.fields)for(const auto& k:f.second)g_track.selected.insert(k.id);
            if(action=="select"&&spec&&g_track.Find(spec->id,kid)){g_track.selected.insert(kid);g_activeChannel=atoi(arg(4));}
            if(action=="selset")for(int i=4;i+1<argc;i+=2){auto s=Channel(atoi(arg(i)));if(s&&g_track.Find(s->id,atoi(arg(i+1))))g_track.selected.insert(atoi(arg(i+1)));}
            g_selectedEvent=-1;++g_track.revision;return;
        }
        g_track.BeginEdit();
        if(action=="delsel")g_track.DeleteSelected();
        else if(action=="movesel")g_track.MoveSelected(Catalog(),Number(arg(4)),Number(arg(5)));
        else if(action=="addkey"&&spec&&!spec->Stepped()&&spec->type!=ChannelType::Colour){Value v{{Number(arg(6))},{}};int id=g_track.Put(*spec,Number(arg(5)),v);if(id)g_track.selected={id};}
        else if(action=="handle"&&spec&&!spec->Stepped()) {
            auto k=g_track.Find(spec->id,kid); double tx=Number(arg(7)),dv=Number(arg(8));
            if(k&&std::isfinite(tx)&&std::isfinite(dv)&&std::fabs(dv)<=1e9){if(spec->type==ChannelType::Colour)dv*=std::max(size_t(1),g_track.data.fields[spec->id].size()-1);auto& h=std::string(arg(6))=="left"?k->left:k->right;h={true,Clamp01(tx),dv};++g_track.revision;}
        } else if(action=="smooth"||action=="linear"||action=="ease"||action=="direction") {
            for(auto& f:g_track.data.fields){auto s=FindSpec(Catalog(),f.first);if(!s||s->Stepped())continue;
                for(size_t i=0;i<f.second.size();++i){auto& k=f.second[i];if(!g_track.selected.empty()&&!g_track.selected.count(k.id))continue;
                    if(action=="direction"){int dir=atoi(arg(4));if(dir>=0&&dir<=2)k.direction=AngleDir(dir);continue;}
                    bool active=action!="linear";k.left={active,1.0/3,0};k.right={active,1.0/3,0};
                    if(action=="ease"){std::string how=arg(4);if(how=="in")k.left.active=false;if(how=="out")k.right.active=false;}
                }
            }++g_track.revision;
        } else if(action=="align") {
            double at=tick;if(spec){auto k=g_track.Find(spec->id,kid);if(k)at=k->tick;}
            // Refuse same-property collisions rather than dropping a selected key.
            bool valid=true;for(const auto& f:g_track.data.fields){int atCount=0;for(const auto& k:f.second)if(g_track.selected.count(k.id)||k.tick==at)++atCount;if(atCount>1)valid=false;}
            if(valid){for(auto& f:g_track.data.fields){for(auto& k:f.second)if(g_track.selected.count(k.id))k.tick=at;std::sort(f.second.begin(),f.second.end(),[](auto&a,auto&b){return a.tick<b.tick;});}++g_track.revision;}
        }
        if(!g_gesture&&g_track.EndEdit())MarkDirty();return;
    }
    g_track.BeginEdit();
    if(op=="arm"||op=="strip") {const auto* s=FindSpec(Catalog(),arg(3));if(s){bool on=std::string(arg(4))=="on"||(std::string(arg(4))!="off"&&!g_track.data.armed.count(s->id));if(on)g_track.data.armed.insert(s->id);else g_track.data.armed.erase(s->id);}}
    // mute <family.groupId | fieldId> [on|off]: disable (keys kept, nothing applied) or re-enable a
    // property or a whole group. With no on/off a group flips together: all on unless all are on.
    else if(op=="mute"){std::vector<std::string> ids;bool all=true;for(const auto& s:Catalog())if(s.family+"."+s.groupId==arg(3)||s.id==arg(3)){ids.push_back(s.id);if(!g_track.data.muted.count(s.id))all=false;}
        const std::string how=arg(4);const bool on=how=="on"||(how!="off"&&!all);for(const auto& id:ids){if(on)g_track.data.muted.insert(id);else g_track.data.muted.erase(id);}}
    else if(op=="group"){std::vector<std::string> ids;bool all=true;for(const auto& s:Catalog())if(s.family+"."+s.groupId==arg(3)){ids.push_back(s.id);if(!g_track.data.armed.count(s.id))all=false;}for(const auto& id:ids){if(all)g_track.data.armed.erase(id);else g_track.data.armed.insert(id);}}
    else if(op=="add"){g_track.selected.clear();for(const auto& id:g_track.data.armed){const auto* s=FindSpec(Catalog(),id);if(s){int k=g_track.Put(*s,tick,Editable(*s,tick,base));if(k)g_track.selected.insert(k);}}}
    else if(op=="set"||op=="graphadd"){const auto* s=FindSpec(Catalog(),arg(3));if(s&&(g_track.data.armed.count(s->id)||op=="graphadd")){Value v=op=="set"?ParseValue(*s,argc,args,4):Editable(*s,Number(arg(4)),base);int k=g_track.Put(*s,op=="set"?tick:Number(arg(4)),v);if(k){g_track.selected={k};g_error.clear();if(s->type==ChannelType::SkyId)PrepareAssets();}else g_error="Invalid value for "+s->label;}}
    else if(op=="delsel")g_track.DeleteSelected();
    // clear <all | family.groupId | fieldId>. It DISARMS as well as deleting the keys, because the
    // Scene Keys inspector is an on/off selector: a property left armed with its keys gone would
    // still hold a strip in the graph while its row reads "off".
    else if(op=="clear"){const std::string target=arg(3);
        for(const auto& s:Catalog())if(target=="all"||s.family+"."+s.groupId==target||s.id==target){g_track.data.fields.erase(s.id);g_track.data.armed.erase(s.id);g_track.data.muted.erase(s.id);}
        g_track.selected.clear();++g_track.revision;}
    else if(op=="macro") {
        const std::string name=arg(3);const std::vector<std::string> names={"dawn","day","golden","dusk","night","moonlit"};
        auto n=std::find(names.begin(),names.end(),name);if(n!=names.end()){size_t i=n-names.begin();
            const double exposure[]={.65,1,.8,.45,.12,.25}, elevation[]={8,40,12,2,-20,-8}, yaw[]={70,135,240,270,300,320};
            const double fog[]={.3,.05,.2,.35,.45,.3};const std::vector<double> colors[]={{255,180,140},{255,245,230},{255,185,95},{185,150,220},{90,115,190},{155,190,255}};
            const std::map<std::string,Value> values={{"exposure",{{exposure[i]}, {}}},{"lightColor",{colors[i],{}}},{"lightAmount",{{.8},{}}},
                {"sunPositionOverride",{{1},{}}},{"sunPosition.yaw",{{yaw[i]}, {}}},{"sunPosition.pitch",{{elevation[i]}, {}}},
                {"env.sunAzimuth",{{yaw[i]}, {}}},{"env.sunElevation",{{elevation[i]}, {}}},{"env.fogDensity",{{fog[i]}, {}}},
                {"skyTint",{{1},{}}},{"skyColor",{{colors[i][0],colors[i][1],colors[i][2],255},{}}},{"env.saturation",{{i<3?1.05:.8},{}}}};
            g_track.selected.clear();for(const auto& v:values){auto s=FindSpec(Catalog(),v.first);if(s){g_track.data.armed.insert(s->id);int k=g_track.Put(*s,tick,v.second);if(k)g_track.selected.insert(k);}}
        }
    }
    if(!g_gesture&&g_track.EndEdit())MarkDirty();
}
} // namespace Filmmaker
