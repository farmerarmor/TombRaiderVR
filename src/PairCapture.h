#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <fstream>
#include <filesystem>
#include <algorithm>
inline bool CaptureNativePair(ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11Texture2D* pair,UINT h,uint64_t id) {
    D3D11_TEXTURE2D_DESC d{};pair->GetDesc(&d);
    bool rgba=d.Format==DXGI_FORMAT_R8G8B8A8_UNORM||d.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if(!rgba&&d.Format!=DXGI_FORMAT_B8G8R8A8_UNORM&&d.Format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)return false;
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> read;
    if(FAILED(dev->CreateTexture2D(&d,nullptr,&read)))return false;
    ctx->CopyResource(read.Get(),pair);D3D11_MAPPED_SUBRESOURCE m{};
    if(FAILED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&m)))return false;
    std::error_code ec;std::filesystem::create_directory("TombRaiderVR-captures",ec);
    bool ok=!ec;
    for(UINT eye=0;eye<2;eye++) {
        char path[180];sprintf_s(path,"TombRaiderVR-captures/native-%llu-%s.bmp",id,eye?"right":"left");
        std::ofstream out(path,std::ios::binary);BITMAPFILEHEADER fh{};BITMAPINFOHEADER ih{};
        fh.bfType=0x4d42;fh.bfOffBits=sizeof(fh)+sizeof(ih);fh.bfSize=fh.bfOffBits+d.Width*h*4;
        ih.biSize=sizeof(ih);ih.biWidth=d.Width;ih.biHeight=-LONG(h);ih.biPlanes=1;ih.biBitCount=32;
        out.write(reinterpret_cast<char*>(&fh),sizeof(fh));out.write(reinterpret_cast<char*>(&ih),sizeof(ih));
        std::vector<uint8_t> row(d.Width*4);
        for(UINT y=0;y<h;y++) {
            memcpy(row.data(),static_cast<uint8_t*>(m.pData)+(eye*h+y)*m.RowPitch,row.size());
            if(rgba)for(UINT x=0;x<d.Width;x++)std::swap(row[x*4],row[x*4+2]);
            out.write(reinterpret_cast<char*>(row.data()),row.size());
        }
        ok=ok&&out.good();
    }
    ctx->Unmap(read.Get(),0);return ok;
}
