#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentCacheMath.h"
#include <iostream>
#include <stdexcept>
using namespace Filmmaker::Environment;
void Check(bool value,const char *message){if(!value)throw std::runtime_error(message);}
int main(){
    auto folder=std::filesystem::temp_directory_path()/("mvm-cache-test-"+std::to_string(GetCurrentProcessId()));
    try {
        std::filesystem::create_directories(folder);
        auto write=[&](const std::filesystem::path& path,const std::string& text){std::ofstream(path,std::ios::binary)<<text;};
        auto source=folder/"source";write(source,"abc");
        const std::string known="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        Check(CacheFileDigest(source,3)==known,"SHA-256 known answer");
        Check(CacheFileDigest(source,2).empty(),"oversize rejected");
        std::filesystem::path selected;
        write(folder/(known+".data"),"abc");
        Check(CachePayloadValid(folder,known,".data",3,selected),"valid portable payload");
        write(selected,"abd");
        Check(!CachePayloadValid(folder,known,".data",3,selected),"corruption rejected");
        Check(!CachePayloadValid(folder,"../source","",3,selected),"path traversal rejected");
        Check(!CachePayloadValid(folder,std::string(64,'f'),".data",3,selected),"interrupted absent payload rejected");
        auto manifest=folder/"manifest";std::string text="unchanged";
        write(manifest,"{\"quoted\":\"[\\\"]\",\"list\":[{}]}");
        Check(ReadCacheManifest(manifest,text),"quoted delimiters accepted");
        write(manifest,std::string(33,'[')+std::string(33,']'));
        Check(!ReadCacheManifest(manifest,text),"excessive nesting rejected");
        write(manifest,std::string(1024*1024+1,' '));
        Check(!ReadCacheManifest(manifest,text),"oversized manifest rejected before allocation");
        Check(!ReadCacheManifest(manifest,text),"interrupted manifest rejected");
        write(source,"stamp-body");
        WIN32_FILE_ATTRIBUTE_DATA data{};
        Check(GetFileAttributesExW(source.c_str(),GetFileExInfoStandard,&data),"stamp source readable");
        SYSTEMTIME utc{};
        Check(FileTimeToSystemTime(&data.ftLastWriteTime,&utc),"stamp converts to UTC");
        char iso[32];
        sprintf_s(iso,"%04u-%02u-%02uT%02u:%02u:%02u.0000000Z",utc.wYear,utc.wMonth,utc.wDay,
                  utc.wHour,utc.wMinute,utc.wSecond);
        ULARGE_INTEGER bytes{};bytes.LowPart=data.nFileSizeLow;bytes.HighPart=data.nFileSizeHigh;
        Check(CacheFileStampMatches(source,bytes.QuadPart,iso),"matching size and write time accepted");
        Check(!CacheFileStampMatches(source,bytes.QuadPart+1,iso),"size mismatch rejected");
        Check(!CacheFileStampMatches(source,bytes.QuadPart,"1999-01-01T00:00:00Z"),"write time mismatch rejected");
        Check(CacheFileTimeMatches(source,iso),"time-only match accepted");
        std::filesystem::remove_all(folder);
        std::cout<<"Environment cache validation passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
