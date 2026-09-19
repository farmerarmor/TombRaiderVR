#pragma once
#include "CameraMath.h"
// Native camera state: Z-up position, then a quaternion at float index 4.
inline void OrbitScriptedView(float* state,Transport::Vector root,float yaw,float pitch) {
    using namespace CameraMath;
    Quaternion old{state[4],state[5],state[6],state[7]};
    Quaternion turn{0,0,std::sin(-yaw*.5f),std::cos(yaw*.5f)};
    Quaternion tilt{std::sin(-pitch*.5f),0,0,std::cos(pitch*.5f)};
    auto next=Multiply(Multiply(turn,old),tilt);
    auto offset=Rotate(Multiply(next,Inverse(old)),{state[0]-root.x,state[1]-root.y,state[2]-root.z});
    state[0]=root.x+offset.x;state[1]=root.y+offset.y;state[2]=root.z+offset.z;
    state[4]=next.x;state[5]=next.y;state[6]=next.z;state[7]=next.w;
}
