#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <array>
#include <windows.h>
#include <bcrypt.h>
namespace Filmmaker::Environment {
inline bool CacheDigestValid(const std::string &s) {
    return s.size()==64 && s.find_first_not_of("0123456789abcdef")==std::string::npos;
}
// Streaming SHA-256: portable payload identity, independent of allocator, adapter and process.
inline std::string CacheFileDigest(const std::filesystem::path &path, uint64_t limit) {
    std::error_code ec;
    const auto size=std::filesystem::file_size(path,ec);
    if(ec || size>limit) return {};
    std::ifstream in(path,std::ios::binary);
    if(!in) return {};
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    BCRYPT_HASH_HANDLE hash=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return {};
    bool ok=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    std::array<unsigned char,65536> block{};
    uint64_t total=0;
    while(ok && in) {
        in.read(reinterpret_cast<char*>(block.data()),block.size());
        auto count=in.gcount(); total+=count;
        if(total>limit || (count && BCryptHashData(hash,block.data(),ULONG(count),0)<0)) ok=false;
    }
    unsigned char digest[32]{};
    ok=ok && in.eof() && total==size && BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;
    if(hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm,0);
    if(!ok) return {};
    const char hex[]="0123456789abcdef";
    std::string result;
    for(auto b:digest){result+=hex[b>>4];result+=hex[b&15];}
    return result;
}
// Bound size AND nesting before the recursive JSON parser sees cache-owned input.
inline bool ReadCacheManifest(const std::filesystem::path &path,std::string &text) {
    std::error_code ec;auto size=std::filesystem::file_size(path,ec);
    if(ec || size==0 || size>1024*1024) return false;
    std::ifstream in(path,std::ios::binary);
    std::string candidate(size,'\0');in.read(candidate.data(),candidate.size());
    if(!in || in.peek()!=std::char_traits<char>::eof()) return false;
    if(candidate.compare(0,3,"\xEF\xBB\xBF")==0) candidate.erase(0,3);
    unsigned depth=0;bool quoted=false,escaped=false;
    for(char c:candidate){
        if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;}
        else if(c=='"')quoted=true;
        else if(c=='{'||c=='['){if(++depth>32)return false;}
        else if(c=='}'||c==']'){if(!depth)return false;--depth;}
    }
    if(depth || quoted)return false;
    text=std::move(candidate);return true;
}
// Map VPK identity without hashing gigabytes on every launch. The prepare script stores
// sourceLength + an ISO-8601 UTC write time; a Steam update changes at least one of those.
inline bool CacheFileStampMatches(const std::filesystem::path &path, uint64_t storedSize,
                                  const std::string &isoUtc) {
    if (!storedSize || isoUtc.size() < 19) return false;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    ULARGE_INTEGER bytes{};
    bytes.LowPart = data.nFileSizeLow;
    bytes.HighPart = data.nFileSizeHigh;
    if (bytes.QuadPart != storedSize) return false;
    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&data.ftLastWriteTime, &utc)) return false;
    char stamp[32];
    if (sprintf_s(stamp, "%04u-%02u-%02uT%02u:%02u:%02u", utc.wYear, utc.wMonth, utc.wDay,
                  utc.wHour, utc.wMinute, utc.wSecond) != 19)
        return false;
    return isoUtc.compare(0, 19, stamp) == 0;
}
inline bool CacheFileTimeMatches(const std::filesystem::path &path, const std::string &isoUtc) {
    if (isoUtc.size() < 19) return false;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&data.ftLastWriteTime, &utc)) return false;
    char stamp[32];
    if (sprintf_s(stamp, "%04u-%02u-%02uT%02u:%02u:%02u", utc.wYear, utc.wMonth, utc.wDay,
                  utc.wHour, utc.wMinute, utc.wSecond) != 19)
        return false;
    return isoUtc.compare(0, 19, stamp) == 0;
}
inline bool CachePayloadValid(const std::filesystem::path &folder,const std::string &digest,
                              const char *extension,uint64_t limit,std::filesystem::path &path) {
    if(!CacheDigestValid(digest))return false;
    auto candidate=folder/(digest+extension);
    if(CacheFileDigest(candidate,limit)!=digest)return false;
    path=std::move(candidate);return true;
}
}
