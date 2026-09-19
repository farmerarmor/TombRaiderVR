#include "DisplaySettings.h"
#include <dxgi.h>
#include <mutex>
#include <string>
#include <cstdio>
#include <atomic>
#include "MinHook.h"
namespace HeadsetDisplay {
namespace {
Settings settings{};
uintptr_t base{};
std::once_flag queryOnce,installOnce;
using FillMode=DXGI_MODE_DESC*(__thiscall*)(void*,DXGI_MODE_DESC*);
using Resize=void(__thiscall*)(void*,uint32_t,uint32_t,bool);
FillMode originalFill{};Resize originalResize{};
void Log(const char* message) {FILE* f{};if(!fopen_s(&f,"TombRaiderVR-display.log","a")){fprintf(f,"[%llu] %s\n",GetTickCount64(),message);fclose(f);}}
void Query() {
    wchar_t name[96];Name(name,GetCurrentProcessId());
    HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Settings),name);
    if(!mapping)return;
    if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(mapping);return;}
    auto* shared=static_cast<Settings*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Settings)));
    if(!shared){CloseHandle(mapping);return;}
    wchar_t module[MAX_PATH]{};HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&Query),&self);
    GetModuleFileNameW(self,module,MAX_PATH);
    auto slash=wcsrchr(module,L'\\');
    if(slash) {
        slash[1]=0;std::wstring dir=module,exe=dir+L"TombRaiderVR\\TombRaiderVRHost.exe";
        wchar_t cmd[1024];swprintf_s(cmd,L"\"%s\" --display-query %lu",exe.c_str(),GetCurrentProcessId());
        STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
        if(CreateProcessW(exe.c_str(),cmd,nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,dir.c_str(),&si,&pi)) {
            DWORD wait=WaitForSingleObject(pi.hProcess,10000),code=1;
            if(wait==WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess,&code) && code==0 && Valid(*shared))settings=*shared;
            // This is our bounded, disposable settings-query child, never the VR host.
            if(wait==WAIT_TIMEOUT)TerminateProcess(pi.hProcess,1);
            CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
        }
    }
    UnmapViewOfFile(shared);CloseHandle(mapping);
    char line[200];sprintf_s(line,"Headset settings: valid=%d eye=%ux%u refresh=%.3f Hz (0=runtime frame timing)",Valid(settings),settings.width,settings.height,settings.refreshHz);Log(line);
}
DXGI_MODE_DESC* __fastcall FillHook(void* self,void*,DXGI_MODE_DESC* mode) {
    std::call_once(queryOnce,Query);
    auto result=originalFill(self,mode);
    if(!Valid(settings))return result;
    // 61b190 also allocates render targets from DisplayConfig's uint16
    // fullscreen dimensions (+8/+a), even for non-exclusive fullscreen.
    // Keep that source consistent with the swapchain and context dimensions.
    auto manager=*reinterpret_cast<void**>(base+0x1712270);
    using GetConfig=unsigned char*(__thiscall*)(void*);
    auto getConfig=reinterpret_cast<GetConfig>((*reinterpret_cast<void***>(manager))[2]);
    auto config=getConfig(manager);
    *reinterpret_cast<uint16_t*>(config+8)=uint16_t(settings.width);
    *reinterpret_cast<uint16_t*>(config+10)=uint16_t(settings.height);
    // FillModeDesc is used before internalCreate allocates depth and colour targets.
    // Update the context too, including when the game is configured windowed.
    auto* bytes=static_cast<unsigned char*>(self);
    *reinterpret_cast<uint32_t*>(bytes+0x18)=settings.width;
    *reinterpret_cast<uint32_t*>(bytes+0x1c)=settings.height;
    mode->Width=settings.width;mode->Height=settings.height;
    if(settings.refreshHz>0)mode->RefreshRate={uint32_t(settings.refreshHz*1000.f+0.5f),1000};
    static std::atomic<unsigned> logged{};
    if(logged.fetch_add(1)<4){char line[160];sprintf_s(line,"Applied headset eye resolution %ux%u to context, mode and render-target config",settings.width,settings.height);Log(line);}
    return result;
}
void __fastcall ResizeHook(void* self,void*,uint32_t width,uint32_t height,bool force) {
    if(Valid(settings)){width=settings.width;height=settings.height;}
    originalResize(self,width,height,force);
}
void InstallOnce() {
    base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
    if(nt->FileHeader.TimeDateStamp!=0x632ce0fa || nt->OptionalHeader.SizeOfImage!=0x24c2000)return;
    auto fill=reinterpret_cast<void*>(base+0x62a230-0x400000);
    auto resize=reinterpret_cast<void*>(base+0x630f60-0x400000);
    // The absolute manager address in FillModeDesc is relocated by the loader.
    const unsigned char fillPrefix[]={0x55,0x8b,0xec,0x53,0x56,0x8b,0x35};
    const unsigned char resizePrefix[]={0x55,0x8b,0xec,0x57,0x8b,0xf9,0x83,0x7f,0x40,0x00};
    if(memcmp(fill,fillPrefix,sizeof(fillPrefix)) || *reinterpret_cast<uint32_t*>(static_cast<unsigned char*>(fill)+7)!=base+0x1b12270-0x400000 ||
       memcmp(resize,resizePrefix,sizeof(resizePrefix)))return;
    auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return;
    bool ok=MH_CreateHook(fill,&FillHook,reinterpret_cast<void**>(&originalFill))==MH_OK;
    ok=ok && MH_CreateHook(resize,&ResizeHook,reinterpret_cast<void**>(&originalResize))==MH_OK;
    if(ok){MH_QueueEnableHook(fill);MH_QueueEnableHook(resize);ok=MH_ApplyQueued()==MH_OK;}
    if(!ok){MH_DisableHook(fill);MH_RemoveHook(fill);MH_DisableHook(resize);MH_RemoveHook(resize);}
    Log(ok?"Engine display hooks installed":"Engine display hooks failed");
}
}
void Install(){std::call_once(installOnce,InstallOnce);}
bool Active(){return Valid(settings);}
}
