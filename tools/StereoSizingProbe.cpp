#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include "../vendor/AmdQbProxy/AmdQbInterfaces.h"
using Microsoft::WRL::ComPtr;
int main() {
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;
    if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,nullptr,&ctx)))return 1;
    auto module=LoadLibraryW(L"atidxx32.dll");if(!module)return 2;
    using Create=HRESULT(__cdecl*)(ID3D11Device*,IAmdDxExt**);
    auto create=reinterpret_cast<Create>(GetProcAddress(module,"AmdDxExtCreate11"));
    IAmdDxExt* ext{};if(!create || FAILED(create(dev.Get(),&ext)))return 3;
    auto stereo=static_cast<IAmdDxExtQuadBufferStereo*>(ext->GetExtInterface(AmdDxExtQuadBufferStereoID));
    if(!stereo || FAILED(stereo->EnableQuadBufferStereo(TRUE)))return 4;
    ComPtr<IDXGIDevice> dx;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory> factory;
    dev.As(&dx);dx->GetAdapter(&adapter);adapter->GetParent(IID_PPV_ARGS(&factory));
    auto window=CreateWindowW(L"STATIC",L"Stereo sizing probe",WS_POPUP,0,0,64,32,nullptr,nullptr,nullptr,nullptr);
    DXGI_SWAP_CHAIN_DESC d{};d.BufferDesc.Width=64;d.BufferDesc.Height=32;d.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count=1;d.BufferCount=1;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.Windowed=TRUE;d.OutputWindow=window;
    ComPtr<IDXGISwapChain> chain;if(FAILED(factory->CreateSwapChain(dev.Get(),&d,&chain)))return 5;
    ComPtr<ID3D11Texture2D> pair;if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&pair))))return 6;
    D3D11_TEXTURE2D_DESC logical{};pair->GetDesc(&logical);
    if(logical.Width!=64 || logical.Height!=32 || stereo->GetLineOffset(chain.Get())!=32)return 7;
    DXGI_SWAP_CHAIN_DESC reported{};chain->GetDesc(&reported);if(reported.BufferDesc.Height!=32)return 8;
    // Ordinary textures retain their real dimensions, including tall textures.
    D3D11_TEXTURE2D_DESC td=logical;td.Height=64;
    ComPtr<ID3D11Texture2D> ordinary;if(FAILED(dev->CreateTexture2D(&td,nullptr,&ordinary)))return 9;
    ordinary->GetDesc(&td);if(td.Height!=64)return 10;
    // Despite its game-facing height, the shadow physically holds BOTH eyes.
    ComPtr<ID3D11RenderTargetView> rt;if(FAILED(dev->CreateRenderTargetView(pair.Get(),nullptr,&rt)))return 11;
    const float color[4]={1,0,0,1};ctx->ClearRenderTargetView(rt.Get(),color);
    td=logical;td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;td.MiscFlags=0;
    ComPtr<ID3D11Texture2D> read;if(FAILED(dev->CreateTexture2D(&td,nullptr,&read)))return 12;
    for(UINT eye=0;eye<2;eye++) {
        D3D11_BOX box{0,eye*32,0,64,(eye+1)*32,1};ctx->CopySubresourceRegion(read.Get(),0,0,0,0,pair.Get(),0,&box);
        D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped)))return 13;
        bool valid=true;for(UINT y=0;y<32;y++)for(UINT x=0;x<64;x++)if(reinterpret_cast<unsigned*>(static_cast<char*>(mapped.pData)+y*mapped.RowPitch)[x]!=0xff0000ff)valid=false;
        ctx->Unmap(read.Get(),0);if(!valid)return 14;
    }
    printf("PASS: game descriptors 64x32; ordinary textures unchanged; both physical stereo halves readable.\n");
    stereo->Release();ext->Release();DestroyWindow(window);return 0;
}
