#include "../../AfxHookSource2/Filmmaker/Lens/LensFxState.h"
#include "../../AfxHookSource2/Filmmaker/Lens/FisheyeState.h"
#include "../../Shared/AfxConsole.h"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <initializer_list>
#include <limits>

// The decoder logs its dimensions; logging is outside this isolated state/decoder test.
namespace Filmmaker { void MvmDebugLog_LinefAlways(const char*,const char*,...) {} }
using namespace Filmmaker;
using namespace Filmmaker::Lens;
static int failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n",__LINE__,#x); ++failed; } } while (0)

static void Command(std::initializer_list<const char*> words) {
    advancedfx::CFakeCommandArgs args("mirv_filmmaker");
    args.AddArg("lens");
    for (const char* word : words) args.AddArg(word);
    args.AddArg("quiet");
    LensFx_RunCommand(args.ArgC(),&args,"mirv_filmmaker");
}

int main() {
    CHECK(!Fisheye_Enabled());
    advancedfx::CFakeCommandArgs fish("mirv_filmmaker");fish.AddArg("fisheye");fish.AddArg("on");
    Fisheye_RunCommand(fish.ArgC(),&fish,"mirv_filmmaker");CHECK(Fisheye_Enabled());
    Fisheye_Toggle();CHECK(!Fisheye_Enabled());
    Fisheye_SetEnabled(true);CHECK(Fisheye_Enabled());
    Fisheye_SetEnabled(false);CHECK(!Fisheye_Enabled());
    Command({"reset","all"});
    CHECK(!LensFx_Active());
    Command({"dof","on"});
    Command({"preset","dof","hearts"});
    CHECK(LensFx_DofOn() && LensFx_Get()[DofShape] == IrisHeart);
    Command({"reset","dof"});
    CHECK(!LensFx_DofOn());
    Command({"preset","dof","stars"});
    CHECK(!LensFx_DofOn()); // Looks tune the lens without enabling it.
    Command({"focus","auto"});
    CHECK(LensFx_Get()[DofAuto] == 1);
    Command({"focus","750"});
    CHECK(LensFx_Get()[DofAuto] == 0 && LensFx_Get()[DofFocus] == 750);
    Command({"flare.tint","teal"});
    CHECK(LensFx_Get()[FlareTint] == FlareTintFromName("teal"));
    CHECK(!LensFx_Set("unknown",10));
    LensFx_Set("dof.fstop",-100);
    CHECK(LensFx_Get()[DofFstop] == Specs()[DofFstop].min);
    LensFx_Set("dof.fstop",std::numeric_limits<double>::quiet_NaN());
    CHECK(LensFx_Get()[DofFstop] == Specs()[DofFstop].def);

    const auto first = LensFx_ReadRenderState(0,0);
    CHECK(first.irisChanged && first.iris.pixels.size() == 128*128);
    const auto unchanged = LensFx_ReadRenderState(first.iris.serial,first.dirt.serial);
    CHECK(!unchanged.irisChanged && unchanged.iris.pixels.empty());
    CHECK(!unchanged.dirtChanged && unchanged.dirt.pixels.empty());
    Command({"iris","mask","crescent"});
    const auto changed = LensFx_ReadRenderState(first.iris.serial,0);
    CHECK(changed.irisChanged && changed.iris.label == "crescent");
    CHECK(changed.iris.serial != first.iris.serial && first.iris.label == "diamond");
    CHECK(changed.values[DofShape] == IrisCustom);

    // Exercise the shared WIC path, not a stand-in decoder: a two-by-two white/black BMP.
    wchar_t folder[MAX_PATH] = {}, file[MAX_PATH] = {};
    CHECK(GetTempPathW(MAX_PATH,folder) && GetTempFileNameW(folder,L"mvl",0,file));
    if (*file) {
        const unsigned char bmp[] = {
            'B','M',70,0,0,0,0,0,0,0,54,0,0,0,40,0,0,0,2,0,0,0,2,0,0,0,
            1,0,24,0,0,0,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
            255,255,255,0,0,0,0,0, 0,0,0,255,255,255,0,0
        };
        { std::ofstream out(std::filesystem::path(file),std::ios::binary); out.write((const char*)bmp,sizeof(bmp)); }
        char utf8[MAX_PATH*4] = {};
        WideCharToMultiByte(CP_UTF8,0,file,-1,utf8,sizeof(utf8),nullptr,nullptr);
        std::string error;
        CHECK(LensFx_LoadIrisFile(utf8,&error));
        const auto image = LensFx_ReadRenderState(changed.iris.serial,0);
        CHECK(image.irisChanged && image.iris.width == 2 && image.iris.height == 2);
        CHECK(image.iris.pixels == std::vector<unsigned char>({0,255,255,0}));
        // Valid extreme aspect ratios must never turn into empty GPU masks.
        auto writeWhiteBmp=[&](unsigned width,unsigned height) {
            const unsigned stride=(width*3+3)&~3u;
            BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
            header.bfSize=header.bfOffBits+stride*height;
            BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=(LONG)width;info.biHeight=(LONG)height;
            info.biPlanes=1;info.biBitCount=24;info.biSizeImage=stride*height;
            const std::vector<unsigned char> pixels(stride*height,255);
            std::ofstream out(std::filesystem::path(file),std::ios::binary);
            out.write((const char*)&header,sizeof(header));out.write((const char*)&info,sizeof(info));
            out.write((const char*)pixels.data(),pixels.size());
        };
        writeWhiteBmp(4096,1);
        CHECK(LensFx_LoadIrisFile(utf8,&error));
        const auto wide=LensFx_ReadRenderState(image.iris.serial,0);
        CHECK(wide.iris.width==512 && wide.iris.height==1 && wide.iris.pixels.size()==512);
        CHECK(std::all_of(wide.iris.pixels.begin(),wide.iris.pixels.end(),[](unsigned char v){return v==255;}));
        writeWhiteBmp(1,4096);
        CHECK(LensFx_LoadIrisFile(utf8,&error));
        const auto tall=LensFx_ReadRenderState(wide.iris.serial,0);
        CHECK(tall.iris.width==1 && tall.iris.height==512 && tall.iris.pixels.size()==512);
        Command({"dirt",utf8});
        const auto dirt=LensFx_ReadRenderState(tall.iris.serial,0);
        CHECK(dirt.dirtChanged && dirt.dirt.width==1 && dirt.dirt.height==512 && dirt.dirt.pixels.size()==512);
        Command({"dirt","off"});
        CHECK(LensFx_ReadRenderState(tall.iris.serial,dirt.dirt.serial).dirt.pixels.empty());
        DeleteFileW(file);
        CHECK(!LensFx_LoadIrisFile(utf8,&error) && !error.empty());
        CHECK(!LensFx_ReadRenderState(tall.iris.serial,0).irisChanged);
    }

    const auto beforeDiagnostics=LensFx_PictureRevision();
    LensFx_SetRenderStatus("Rendering");
    LensFx_NoteRenderedFrame();
    CHECK(LensFx_PictureRevision()==beforeDiagnostics);
    LensFx_Set("dof.range",275);
    const auto changedPicture=LensFx_PictureRevision();
    CHECK(changedPicture>beforeDiagnostics);
    LensFx_Set("dof.range",275);
    CHECK(LensFx_PictureRevision()==changedPicture);
    std::ostringstream json;
    LensFx_AppendStateJson(json);
    CHECK(json.str().find("\"status\":\"Rendering\"") != std::string::npos);
    CHECK(json.str().find("\"frames\":1") != std::string::npos);
    Command({"flare","on"});
    Command({"reset","all"});
    CHECK(!LensFx_Active());
    if (failed) return 1;
    std::puts("LensFxStateTests passed");
}
