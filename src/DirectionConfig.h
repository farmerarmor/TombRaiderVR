#pragma once
#include <windows.h>
#include <cwchar>
namespace DirectionConfig {
enum class Source {Mouse,Headset,Controller};
inline bool MotionEnabled(const wchar_t* config) {
    return GetPrivateProfileIntW(L"VR",L"MotionControls",1,config)!=0;
}
inline Source Parse(const wchar_t* value,Source fallback) {
    if(!_wcsicmp(value,L"Mouse"))return Source::Mouse;
    if(!_wcsicmp(value,L"Headset"))return Source::Headset;
    if(!_wcsicmp(value,L"Controller"))return Source::Controller;
    return fallback;
}
inline Source Read(const wchar_t* config,const wchar_t* key,Source fallback=Source::Mouse) {
    wchar_t value[32]{};GetPrivateProfileStringW(L"VR",key,L"",value,32,config);
    return Parse(value,fallback);
}
inline const char* Name(Source source) {
    return source==Source::Headset?"Headset":source==Source::Controller?"Controller":"Mouse";
}
}
