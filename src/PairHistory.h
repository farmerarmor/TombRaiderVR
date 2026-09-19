#pragma once
#include "SharedPair.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <vector>

// Diagnostic only: records both eyes from one native pair at each submission.
// Small GPU copies run while armed; mapping and disk IO happen only on F8.
class PairHistory {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
public:
    static constexpr UINT Width=384,Height=192,Capacity=360;
    struct Metadata {
        uint64_t frame{},tick{},predictedTime{};
        uint32_t layers{},shouldRender{},swapEyes{};
        Transport::RenderInfo render{};
    };
private:
    struct Slot {Ptr<ID3D11Texture2D> image;Metadata meta;};
    std::array<Slot,Capacity> slots{};
    Ptr<ID3D11Device> device;
    Ptr<ID3D11Texture2D> target,source;
    Ptr<ID3D11RenderTargetView> rtv;
    Ptr<ID3D11ShaderResourceView> srv;
    Ptr<ID3D11VertexShader> vs;
    Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11SamplerState> sampler;
    Ptr<ID3D11RasterizerState> raster;
    UINT next{},count{};
    bool Init(ID3D11Device* dev) {
        static constexpr char shader[]=R"(
Texture2D pairImage:register(t0); SamplerState imageSampler:register(s0);
struct V {float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID){V o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}
float4 PS(V i):SV_TARGET {float eye=step(0.5,i.uv.x);return float4(pairImage.SampleLevel(imageSampler,float2(frac(i.uv.x*2),i.uv.y*0.5+eye*0.5),0).rgb,1);}
)";
        Ptr<ID3DBlob> v,p,error;
        if(FAILED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"VS","vs_4_0",0,0,&v,&error)) ||
           FAILED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"PS","ps_4_0",0,0,&p,&error)))return false;
        if(FAILED(dev->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs)) ||
           FAILED(dev->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps)))return false;
        D3D11_TEXTURE2D_DESC d{};d.Width=Width;d.Height=Height;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET;
        if(FAILED(dev->CreateTexture2D(&d,nullptr,&target)) || FAILED(dev->CreateRenderTargetView(target.Get(),nullptr,&rtv)))return false;
        d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        for(auto& s:slots)if(FAILED(dev->CreateTexture2D(&d,nullptr,&s.image)))return false;
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        if(FAILED(dev->CreateSamplerState(&sd,&sampler)))return false;
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
        if(FAILED(dev->CreateRasterizerState(&rd,&raster)))return false;
        device=dev;return true;
    }
public:
    bool Record(ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11Texture2D* pair,const Metadata& meta) {
        if(device.Get()!=dev){Reset();if(!Init(dev)){Reset();return false;}}
        if(source.Get()!=pair){srv.Reset();if(FAILED(dev->CreateShaderResourceView(pair,nullptr,&srv)))return false;source=pair;}
        auto* output=rtv.Get();auto* input=srv.Get();auto* filter=sampler.Get();
        ctx->OMSetRenderTargets(1,&output,nullptr);ctx->OMSetBlendState(nullptr,nullptr,0xffffffff);ctx->OMSetDepthStencilState(nullptr,0);
        D3D11_VIEWPORT viewport{0,0,float(Width),float(Height),0,1};ctx->RSSetViewports(1,&viewport);ctx->RSSetState(raster.Get());
        ctx->IASetInputLayout(nullptr);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(),nullptr,0);ctx->GSSetShader(nullptr,nullptr,0);ctx->HSSetShader(nullptr,nullptr,0);ctx->DSSetShader(nullptr,nullptr,0);
        ctx->PSSetShader(ps.Get(),nullptr,0);ctx->PSSetShaderResources(0,1,&input);ctx->PSSetSamplers(0,1,&filter);ctx->Draw(3,0);
        input=nullptr;ctx->PSSetShaderResources(0,1,&input);ctx->OMSetRenderTargets(0,nullptr,nullptr);
        auto& s=slots[next];ctx->CopyResource(s.image.Get(),target.Get());s.meta=meta;
        next=(next+1)%Capacity;if(count<Capacity)++count;return true;
    }
    void Reset() {
        for(auto& s:slots)s={};device.Reset();target.Reset();source.Reset();rtv.Reset();srv.Reset();vs.Reset();ps.Reset();sampler.Reset();raster.Reset();next=count=0;
    }
    UINT Count()const{return count;}
    bool Save(ID3D11DeviceContext* ctx,const std::filesystem::path& folder) {
        if(!count)return false;
        std::error_code ec;std::filesystem::create_directories(folder,ec);if(ec)return false;
        std::ofstream csv(folder/"frames.csv");
        csv<<"index,frame,tick,predictedTime,layers,shouldRender,swapEyes,mode,eyeMask,poseId,poseTick,headX,headY,headZ,headQx,headQy,headQz,headQw\n";
        bool ok=true;
        for(UINT i=0;i<count;i++){
            auto& s=slots[(next+Capacity-count+i)%Capacity];D3D11_MAPPED_SUBRESOURCE map{};
            if(FAILED(ctx->Map(s.image.Get(),0,D3D11_MAP_READ,0,&map))){ok=false;continue;}
            char name[32];sprintf_s(name,"frame-%04u.bmp",i);std::ofstream out(folder/name,std::ios::binary);
            BITMAPFILEHEADER fh{};BITMAPINFOHEADER ih{};fh.bfType=0x4d42;fh.bfOffBits=sizeof(fh)+sizeof(ih);fh.bfSize=fh.bfOffBits+Width*Height*4;
            ih.biSize=sizeof(ih);ih.biWidth=Width;ih.biHeight=-LONG(Height);ih.biPlanes=1;ih.biBitCount=32;
            out.write(reinterpret_cast<char*>(&fh),sizeof(fh));out.write(reinterpret_cast<char*>(&ih),sizeof(ih));
            std::array<unsigned char,Width*4> row;
            for(UINT y=0;y<Height;y++){
                memcpy(row.data(),static_cast<unsigned char*>(map.pData)+y*map.RowPitch,row.size());
                for(UINT x=0;x<Width;x++)std::swap(row[x*4],row[x*4+2]);out.write(reinterpret_cast<char*>(row.data()),row.size());
            }
            ctx->Unmap(s.image.Get(),0);ok=ok&&out.good();
            auto& m=s.meta;auto& t=m.render.tracking;auto& h=t.head;
            csv<<i<<','<<m.frame<<','<<m.tick<<','<<m.predictedTime<<','<<m.layers<<','<<m.shouldRender<<','<<m.swapEyes<<','<<m.render.mode<<','<<m.render.eyeMask<<','<<t.id<<','<<t.tick
               <<','<<h.position.x<<','<<h.position.y<<','<<h.position.z<<','<<h.orientation.x<<','<<h.orientation.y<<','<<h.orientation.z<<','<<h.orientation.w<<'\n';
        }
        return ok&&csv.good();
    }
};
