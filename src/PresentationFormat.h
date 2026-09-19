#pragma once
#include <dxgiformat.h>
// The final desktop image already contains display-encoded RGB. Copy its bytes
// unchanged and tell OpenXR to decode them before linear-light composition.
inline DXGI_FORMAT PresentationFormat(DXGI_FORMAT source) {
    switch(source) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM:return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    default:return source;
    }
}
