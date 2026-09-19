#include "NativeBridge.h"
#include "PairHistory.h"
#include "PresentationFormat.h"
#include <windows.h>
#include <wrl/client.h>
#include <cstdio>
#include <vector>
#include <cstring>
using Microsoft::WRL::ComPtr;
int main(int argc,char** argv) {
    D3D11_TEXTURE2D_DESC d{}; d.Width=1024;d.Height=1024;d.ArraySize=d.MipLevels=1;
    d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    if(!NativeBridge::ValidatePair(d,512))return 10;
    if(NativeBridge::ValidatePair(d,0)||NativeBridge::ValidatePair(d,1024))return 11;
    d.SampleDesc.Count=2;if(NativeBridge::ValidatePair(d,512))return 12;d.SampleDesc.Count=1;
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL fl{};
    HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx);
    if(FAILED(hr)){printf("D3D11 device failed: %08lx\n",hr);return 1;}
    d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    std::vector<unsigned> pixels(1024*1024);
    for(unsigned y=0;y<1024;y++)for(unsigned x=0;x<1024;x++)pixels[y*1024+x]=y<512?0xff3030d0:0xff30d030;
    D3D11_SUBRESOURCE_DATA data{pixels.data(),1024*4,0};ComPtr<ID3D11Texture2D> pair;
    hr=dev->CreateTexture2D(&d,&data,&pair);if(FAILED(hr))return 2;
    // Exercise actual array-slice copies and read them back. Detect eye swap/box errors.
    D3D11_TEXTURE2D_DESC dest=d;dest.Height=512;dest.ArraySize=2;dest.Format=PresentationFormat(d.Format);
    ComPtr<ID3D11Texture2D> eyes; if(FAILED(dev->CreateTexture2D(&dest,nullptr,&eyes)))return 3;
    for(UINT e=0;e<2;e++){D3D11_BOX b{0,e*512,0,1024,(e+1)*512,1};ctx->CopySubresourceRegion(eyes.Get(),e,0,0,0,pair.Get(),0,&b);}
    dest.Usage=D3D11_USAGE_STAGING;dest.BindFlags=0;dest.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> read;if(FAILED(dev->CreateTexture2D(&dest,nullptr,&read)))return 4;
    ctx->CopyResource(read.Get(),eyes.Get());
    for(UINT e=0;e<2;e++){
        D3D11_MAPPED_SUBRESOURCE m{};if(FAILED(ctx->Map(read.Get(),e,D3D11_MAP_READ,0,&m)))return 5;
        bool valid=true;for(UINT y=0;y<512;y++)for(UINT x=0;x<1024;x++)
            if(reinterpret_cast<const unsigned*>(static_cast<const char*>(m.pData)+y*m.RowPitch)[x]!=(e?0xff30d030:0xff3030d0))valid=false;
        ctx->Unmap(read.Get(),e);if(!valid)return 6;
    }
    printf("PASS: %u-bit D3D11 native pair bounds and UNORM-to-sRGB eye-array copies preserve every pixel byte.\n",unsigned(sizeof(void*)*8));
    if(argc>=2 && !strcmp(argv[1],"--history")) {
        PairHistory history;
        for(UINT i=0;i<PairHistory::Capacity+3;i++)if(!history.Record(dev.Get(),ctx.Get(),pair.Get(),{i,GetTickCount64()}))return 20;
        if(history.Count()!=PairHistory::Capacity || !history.Save(ctx.Get(),"history-probe"))return 21;
        std::ifstream image("history-probe/frame-0000.bmp",std::ios::binary);std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(image)),{});
        size_t offset=sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER);
        auto check=[&](UINT x,UINT y,bool right){size_t p=offset+(y*PairHistory::Width+x)*4;return bytes.size()>p+3 && bytes[p]==0x30 && bytes[p+1]==(right?0xd0:0x30) && bytes[p+2]==(right?0x30:0xd0);};
        if(!check(20,20,false)||!check(PairHistory::Width-20,20,true)||!check(20,PairHistory::Height-20,false))return 22;
        std::ifstream csv("history-probe/frames.csv");std::string line;std::getline(csv,line);std::getline(csv,line);
        if(line.rfind("0,3,",0)!=0)return 23;
        puts("PASS rolling GPU history: eye colours, upright layout, wraparound order and export");return 0;
    }
    if(argc<2||strcmp(argv[1],"--xr"))return 0;
    printf("Testing OpenXR for 20 seconds. Left red, right green.\n");
    ULONGLONG end=GetTickCount64()+20000;
    while(GetTickCount64()<end){NativeBridge::Present(nullptr,pair.Get(),512,false);Sleep(10);}
    auto n=NativeBridge::SubmittedPairs();NativeBridge::Shutdown();
    printf("Submitted pairs: %llu\n",n);return n?0:7;
}
