#include "NativeBridge.h"
#include "SharedPair.h"
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
int wmain(int argc,wchar_t** argv) {
    if(argc>=2 && !wcscmp(argv[1],L"--display-query")) {
        HeadsetDisplay::Settings display{};
        if(!NativeBridge::QueryDisplaySettings(display))return 10;
        if(argc==2){printf("{\"width\":%u,\"height\":%u,\"refreshHz\":%.3f}\n",display.width,display.height,display.refreshHz);return 0;}
        if(argc!=3)return 1;
        DWORD pid=wcstoul(argv[2],nullptr,10);if(!pid)return 1;
        wchar_t name[96];HeadsetDisplay::Name(name,pid);
        HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);if(!mapping)return 11;
        auto* shared=static_cast<HeadsetDisplay::Settings*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(display)));
        if(!shared){CloseHandle(mapping);return 12;}
        *shared=display;UnmapViewOfFile(shared);CloseHandle(mapping);return 0;
    }
    if(argc!=3||wcscmp(argv[1],L"--game-pid"))return 1;
    DWORD pid=wcstoul(argv[2],nullptr,10);if(!pid)return 1;
    HANDLE game=OpenProcess(SYNCHRONIZE,FALSE,pid);if(!game)return 2;
    wchar_t name[96];Transport::Name(name,pid);
    HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);if(!mapping){CloseHandle(game);return 3;}
    auto* header=static_cast<Transport::Header*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Transport::Header)));
    if(!header||header->magic!=Transport::Magic||header->version!=Transport::Version||header->pid!=pid)return 4;
    ComPtr<IDXGIFactory1> factory;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return 5;
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;ComPtr<ID3D11Texture2D> shared,pair;
    ComPtr<IDXGIKeyedMutex> keyed;LONG generation=0;UINT height=0;bool fatal=false;
    NativeBridge::SetTrackingChannel(header);
    while(WaitForSingleObject(game,0)==WAIT_TIMEOUT&&!fatal) {
        LONG next=header->generation;MemoryBarrier();
        if(next && next!=generation) {
            Transport::Header desc{};memcpy(&desc,header,sizeof(desc));MemoryBarrier();
            if(header->generation!=next)continue;
            NativeBridge::Shutdown();keyed.Reset();shared.Reset();pair.Reset();ctx.Reset();dev.Reset();
            ComPtr<IDXGIAdapter1> selected;
            for(UINT i=0;;i++) {
                ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;
                DXGI_ADAPTER_DESC1 ad{};if(a&&SUCCEEDED(a->GetDesc1(&ad))&&!memcmp(&ad.AdapterLuid,&desc.adapter,sizeof(LUID))){selected=a;break;}
            }
            if(!selected)break;
            D3D_FEATURE_LEVEL fl;
            if(FAILED(D3D11CreateDevice(selected.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx)))break;
            if(FAILED(dev->OpenSharedResource(reinterpret_cast<HANDLE>(uintptr_t(desc.sharedHandle)),IID_PPV_ARGS(&shared)))||FAILED(shared.As(&keyed)))break;
            D3D11_TEXTURE2D_DESC td{};shared->GetDesc(&td);
            if(td.Width!=desc.width||td.Height!=desc.height*2||td.Format!=desc.format||!NativeBridge::ValidatePair(td,desc.height))break;
            td.MiscFlags=0;if(FAILED(dev->CreateTexture2D(&td,nullptr,&pair)))break;
            height=desc.height;generation=next;
        }
        if(!keyed){Sleep(2);continue;}
        HRESULT hr=keyed->AcquireSync(1,10);
        if(hr==WAIT_TIMEOUT)continue;
        if(hr!=S_OK)break;
        if(header->generation!=generation){keyed->ReleaseSync(0);continue;}
        uint64_t frame=header->frameId;bool swap=header->swapEyes!=0;
        auto rendered=header->rendered;
        ctx->CopyResource(pair.Get(),shared.Get());ctx->Flush();
        if(FAILED(keyed->ReleaseSync(0)))break;
        // Private snapshot remains stable even while producer writes the next pair.
        NativeBridge::SetSourceFrame(frame);
        NativeBridge::SetRenderInfo(rendered);
        NativeBridge::Present(nullptr,pair.Get(),height,swap);
    }
    NativeBridge::Shutdown();NativeBridge::SetTrackingChannel(nullptr);UnmapViewOfFile(header);CloseHandle(mapping);CloseHandle(game);return 0;
}
