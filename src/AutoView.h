#pragma once
#include <cstdint>
// Manual changes last until the next stable gameplay/cinematic transition.
struct AutoView {
    enum class State { Waiting, Gameplay, Cinematic, Movie };
    State state=State::Waiting;
    uint64_t gameplaySince{};
    bool Update(uint64_t now,bool gameplay,bool cinematic,bool& immersive,bool movie=false,bool cutscenesInVR=false) {
        if(cinematic || movie) {
            gameplaySince=0;
            const auto next=movie?State::Movie:State::Cinematic;
            if(state==next)return false;
            state=next;immersive=!movie && cutscenesInVR;return true;
        }
        if(!gameplay){gameplaySince=0;return false;}
        if(state==State::Gameplay)return false;
        if(!gameplaySince){gameplaySince=now;return false;}
        if(now-gameplaySince<350)return false;
        state=State::Gameplay;immersive=true;return true;
    }
};
