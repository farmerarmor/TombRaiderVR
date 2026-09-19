#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
namespace NativeBounds {
inline bool ExpandWeapon(void* volume,float reach) {
    // DXHRDC 2.0.66.0: union occupies 0x50 bytes, followed by its type.
    // Type 10 is unsupported by SceneCellContainer's volume callbacks.
    uint32_t type;std::memcpy(&type,static_cast<char*>(volume)+0x50,4);
    float v[16];std::memcpy(v,volume,sizeof(v));
    float x{},y{},z{},radius{};
    if(type==0){x=v[0];y=v[1];z=v[2];radius=v[3];}
    else if(type==5) {
        x=v[12];y=v[13];z=v[14];
        for(int row=0;row<3;row++)radius+=std::hypot(v[row*4],v[row*4+1],v[row*4+2]);
    } else return false;
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!std::isfinite(radius)||radius<0 ||
       !std::isfinite(reach)||reach<0||!std::isfinite(radius+reach))return false;
    float sphere[4]{x,y,z,radius+reach};type=0;
    std::memcpy(volume,sphere,sizeof(sphere));std::memcpy(static_cast<char*>(volume)+0x50,&type,4);
    return true;
}
}
