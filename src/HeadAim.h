#pragma once
#include "CameraMath.h"
#include <algorithm>
namespace HeadAim {
inline CameraMath::Matrix Basis(float yaw,float pitch) {
    // Native 915850 builds Qz(yaw + pi) * Qx(-pitch); forward is +Y.
    // Express that view as right/down/forward for CameraMath::HeadWorld.
    yaw+=3.14159265359f;pitch=-pitch;
    const float s=std::sin(yaw),c=std::cos(yaw),sp=std::sin(pitch),cp=std::cos(pitch);
    CameraMath::Matrix b;
    b.m[0]=c;b.m[1]=s;
    b.m[4]=-s*sp;b.m[5]=c*sp;b.m[6]=-cp;
    b.m[8]=-s*cp;b.m[9]=c*cp;b.m[10]=sp;b.m[15]=1;
    return b;
}
struct Result {float yaw,pitch;Transport::Pose reference;};
inline Result Steer(float yaw,float pitch,float low,float high,const Transport::Pose& reference,const Transport::Pose& head) {
    const auto target=CameraMath::HeadWorld(Basis(yaw,pitch),reference,head,0);
    const float horizontal=std::hypot(target.m[8],target.m[9]);
    float nextYaw=horizontal>.001f?std::remainder(std::atan2(-target.m[8],target.m[9])-3.14159265359f,6.28318530718f):yaw;
    float nextPitch=std::clamp(-std::atan2(target.m[10],horizontal),low,high);
    // Consume yaw/pitch in the native controller. Leave roll in the rendering
    // reference so the same head rotation is never applied twice.
    // Rotation from old native basis to new basis, expressed in camera axes.
    auto oldBasis=Basis(yaw,pitch),newBasis=Basis(nextYaw,nextPitch);
    auto r=CameraMath::Multiply(newBasis,CameraMath::InverseRigid(oldBasis));
    Transport::Quaternion q{};
    float trace=r.m[0]+r.m[5]+r.m[10];
    if(trace>0){float s=2*std::sqrt(trace+1);q={ (r.m[6]-r.m[9])/s,(r.m[8]-r.m[2])/s,(r.m[1]-r.m[4])/s,s*.25f};}
    else {
        int i=r.m[5]>r.m[0]?1:0;if(r.m[10]>r.m[i*4+i])i=2;
        int j=(i+1)%3,k=(j+1)%3;float s=2*std::sqrt(1+r.m[i*4+i]-r.m[j*4+j]-r.m[k*4+k]);
        float v[3]{};v[i]=s*.25f;v[j]=(r.m[i*4+j]+r.m[j*4+i])/s;v[k]=(r.m[i*4+k]+r.m[k*4+i])/s;
        q={v[0],v[1],v[2],(r.m[j*4+k]-r.m[k*4+j])/s};
    }
    q.y=-q.y;q.z=-q.z;
    auto consumed=reference;
    consumed.orientation=CameraMath::Multiply(reference.orientation,q);
    return {nextYaw,nextPitch,consumed};
}
}
