#include "NativeBridge.h"
#include <windows.h>
#include <wrl/client.h>
#include <vector>
#include <cstdio>
using Microsoft::WRL::ComPtr;
int main(){
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL fl;
    if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx)))return 1;
    D3D11_TEXTURE2D_DESC d{};d.Width=1920;d.Height=2160;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint32_t> pixels(d.Width*d.Height);for(UINT y=0;y<d.Height;y++)for(UINT x=0;x<d.Width;x++)pixels[y*d.Width+x]=y<1080?0xff3030d0:0xff30d030;
    D3D11_SUBRESOURCE_DATA data{pixels.data(),d.Width*4,0};ComPtr<ID3D11Texture2D> pair;
    if(FAILED(dev->CreateTexture2D(&d,&data,&pair)))return 2;
    auto end=GetTickCount64()+20000;while(GetTickCount64()<end){NativeBridge::Present(nullptr,pair.Get(),1080,false);Sleep(10);}
    auto n=NativeBridge::SubmittedPairs();printf("Transferred whole pairs: %llu\n",n);NativeBridge::Shutdown();return n>1?0:3;
}
