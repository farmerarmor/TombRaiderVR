#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include "SharedPair.h"
#include "DisplaySettings.h"
namespace NativeBridge {
// Source must be the current native HD3D back buffer: L at y=0, R at y=eyeHeight.
void Present(IDXGISwapChain* chain, ID3D11Texture2D* nativePair, UINT eyeHeight, bool swapEyes);
void Shutdown(); // Outside DllMain, on render thread only.
uint64_t SubmittedPairs();
void SetSourceFrame(uint64_t frame);
void SetTrackingChannel(Transport::Header* shared);
void SetRenderInfo(const Transport::RenderInfo& info);
bool ValidatePair(const D3D11_TEXTURE2D_DESC& desc, UINT eyeHeight);
bool QueryDisplaySettings(HeadsetDisplay::Settings& settings);
}
