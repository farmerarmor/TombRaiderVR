#pragma once
#include <cstdint>
#include "SharedPair.h"
namespace EngineCamera {
Transport::RenderInfo OnPresent(uint64_t frame, bool capture);
void SetChannel(Transport::Header* header);
}
