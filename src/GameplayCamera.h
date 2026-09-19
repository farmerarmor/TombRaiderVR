#pragma once
#include "SharedPair.h"
#include <cmath>
#include <cstring>
struct GameplayCamera {
    float state[10]{};
    Transport::Vector root{};
    bool valid=false;
    bool Update(const float* value,Transport::Vector player) {
        for(int i=0;i<10;i++)if(!std::isfinite(value[i]))return false;
        float norm=0;for(int i=4;i<8;i++)norm+=value[i]*value[i];
        if(norm<.9f || norm>1.1f)return false;
        memcpy(state,value,sizeof(state));root=player;valid=true;return true;
    }
    bool Apply(float* output,Transport::Vector player) const {
        if(!valid)return false;
        memcpy(output,state,sizeof(state));
        output[0]+=player.x-root.x;output[1]+=player.y-root.y;output[2]+=player.z-root.z;
        return true;
    }
};
