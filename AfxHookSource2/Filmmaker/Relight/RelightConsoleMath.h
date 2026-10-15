#pragma once
#include <cmath>
#include <cstdlib>
#include <cctype>
namespace Filmmaker::Relight {
inline bool ParseFiniteNumber(const char* text,float& value) {
    if(!text||!*text)return false;
    char* end=nullptr;const float parsed=std::strtof(text,&end);
    if(end==text||!std::isfinite(parsed))return false;
    while(*end&&std::isspace(static_cast<unsigned char>(*end)))++end;
    if(*end)return false;
    value=parsed;return true;
}
}
