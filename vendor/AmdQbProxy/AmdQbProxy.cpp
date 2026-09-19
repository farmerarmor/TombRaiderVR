// AmdQbProxy.cpp
// Fake atidxx32.dll (x86) / atidxx64.dll (x64)
//
// Implements the AMD Quad-Buffer Stereo COM interface so that AMD HD3D
// native games can enable their stereo render path on ANY GPU (AMD, NVIDIA,
// Intel).  Games render left eye into the top half and right eye into the
// bottom half of a doubled render target (top-and-bottom format).
//
// Shadow texture approach: the swap chain back buffer stays at display
// resolution.  A separate "shadow" texture at doubled height is returned
// by our hooked IDXGISwapChain::GetBuffer, so the game renders T&B into
// the shadow.  Our IDXGISwapChain::Present hook composites SBS from the
// shadow into the real (display-sized) BB before calling the original
// Present.  This avoids DXGI scaling issues that caused display bugs on
// NVIDIA (Hitman: Absolution, Tomb Raider 2013, GRID 2/Autosport).
//
// All output modes are half resolution per eye — each eye sees half the
// screen dimensions (SBS: half width, TAB: half height).
//
// D3D11 games: full SBS/TAB/Crosseyed compositor.
// D3D10 games: COM stubs work (game enters stereo mode) but no conversion —
//              TODO: add D3D10 compositor if needed.
//
// Config.xml (same folder as the DLL):
//   <AmdQbProxy>
//     <OutputMode Value="1"/>    <!-- 0=Top-and-Bottom, 1=SBS (default), 2=Crosseyed -->
//   </AmdQbProxy>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define DISPLAYED_VERSION "TombRaiderVR 0.1.0 / wiz3D 981de7b"
#include <new>
#include <stdio.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <unordered_map>
#include <vector>
#include <stdint.h>
#include <intrin.h>

// Local interfaces header — omits the AmdDxExtCreate/AmdDxExtCreate11 function
// declarations from the AMD SDK so our extern "C" exports don't cause C2732.
#include "AmdQbInterfaces.h"

#include <MinHook.h>

// SR-Lib simplified C-style factory API. Static lib with SR runtime
// DLLs delay-loaded — see SR.hpp for the rationale and the vcxproj's
// DelayLoadDLLs entries that pair with it.
#include "NativeBridge.h"

// ============================================================
// Diagnostic log
// ============================================================
static HMODULE g_hSelf = nullptr;

static void WriteLog(const char* msg)
{
    // Write next to the DLL (game directory) so the log is easy to find.
    wchar_t dir[MAX_PATH] = {};
    if (g_hSelf)
    {
        GetModuleFileNameW(g_hSelf, dir, MAX_PATH);
        wchar_t* p = wcsrchr(dir, L'\\');
        if (p) p[1] = L'\0';
    }

    // Draw OS cursor twice for SBS/TAB stereo so the pointer matches both eyes.
    // Cursor drawing is handled in the compositor Present path so we have a
    // valid HWND and can draw after the compositor finishes. The previous
    // implementation attempted to draw from the generic WriteLog function and
    // referenced undefined swap-chain state; move drawing into HookedPresent.
    if (!dir[0] && !GetTempPathW(MAX_PATH, dir)) return;

    wchar_t path[MAX_PATH];
    wcscpy_s(path, dir);
    wcscat_s(path, L"HD3D_atidxx.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(h, msg, static_cast<DWORD>(strlen(msg)), &written, nullptr);
    CloseHandle(h);
}

// ============================================================
// Config
// ============================================================
enum OutputMode {
    OM_OU_HALF          = 0,  // Half Top-and-Bottom     (half res per eye, stacked)
    OM_SBS_HALF         = 1,  // Half Side-by-Side       (half res per eye, side by side, default)
    OM_SBS_FULL         = 2,  // Full Side-by-Side       (set game to half display width)
    OM_OU_FULL          = 3,  // Full Top-and-Bottom     (set game to half display height)
    OM_LINE_INTERLEAVED = 4,  // Line/Row Interleaved    (alternating horizontal rows, passive 3D TVs)
    OM_COL_INTERLEAVED  = 5,  // Column Interleaved      (alternating vertical columns)
    OM_CHECKERBOARD     = 6,  // Checkerboard            (alternating pixels, DLP projectors)
    OM_ANAGLYPH         = 7,  // Anaglyph                (colour-filtered stereo, requires glasses)
    OM_SR_WEAVE         = 8   // Simulated Reality Weave (Leia/Samsung Odyssey Moving Lightfield displays)
};
static OutputMode g_OutputMode = OM_SBS_HALF;
static bool g_bSwapEyes = false;

enum AnaglyphColour {
    AC_RED_CYAN      = 0,  // Red left / Cyan right  (most common)
    AC_GREEN_MAGENTA = 1,  // Green left / Magenta right
    AC_AMBER_BLUE    = 2   // Amber left / Blue right (best colour rendition)
};
static AnaglyphColour g_AnaglyphColour = AC_RED_CYAN;

enum AnaglyphMethod {
    AM_DUBOIS      = 0,  // Best for glasses viewing; minimises ghosting via spectral correction (default)
    AM_COMPROMISE  = 1,  // Best for mixed audiences; looks acceptable with AND without glasses
    AM_COLOR       = 2,  // Full colour, severe ghosting; avoid for glasses use
    AM_HALF_COLOR  = 3,  // Right eye keeps colour, left desaturated; reduced ghosting
    AM_OPTIMISED   = 4,  // Wimmer weighted-channel blend; good ghosting, less colour than Dubois
    AM_GREY        = 5,  // Both eyes greyscale; near-zero ghosting, no colour
    AM_TRUE        = 6   // Classic single-channel luma per eye; maximum ghosting, historical
};
static AnaglyphMethod g_AnaglyphMethod = AM_DUBOIS;

static void LoadConfig()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hSelf, path, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(path, L'\\');
    if (!lastSlash) return;
    size_t remaining = MAX_PATH - static_cast<size_t>(lastSlash - path + 1);
    wcscpy_s(lastSlash + 1, remaining, L"HD3D_Config.xml");

    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    char buf[4096] = {};
    DWORD bytesRead = 0;
    ReadFile(hFile, buf, sizeof(buf) - 1, &bytesRead, nullptr);
    CloseHandle(hFile);

    // Load OutputMode
    const char* p = strstr(buf, "OutputMode");
    if (p)
    {
        const char* v = strstr(p, "Value=\"");
        if (v)
        {
            int mode = atoi(v + 7);
            if (mode >= OM_OU_HALF && mode <= OM_SR_WEAVE)
                g_OutputMode = static_cast<OutputMode>(mode);
        }
    }

    // Some games crash if the SR runtime DLLs are loaded into their process
    // (e.g. Tomb Raider 2013 dies during init when SimulatedRealityCore loads).
    // For those, force the fallback to OM_SBS_HALF here at config time, before
    // any SR code paths run. New entries can be added as more games are tested.
    if (g_OutputMode == OM_SR_WEAVE)
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH))
        {
            for (wchar_t* p = exePath; *p; ++p) *p = towlower(*p);
            const wchar_t* srBlacklist[] = {
                L"tombraider.exe",   // TR2013: SR DllMain side-effects crash the game
            };
            for (size_t i = 0; i < _countof(srBlacklist); ++i)
            {
                if (wcsstr(exePath, srBlacklist[i]))
                {
                    WriteLog("[AmdQbProxy] SR-incompatible exe detected; forcing OM_SBS_HALF\n");
                    g_OutputMode = OM_SBS_HALF;
                    break;
                }
            }
        }
    }
    // For SR-eligible games, SR DLLs are loaded lazily on first stereo present
    // via the delay-load mechanism — so non-SR systems (Linux/Proton, no Leia
    // runtime) won't load SR DLLs at all if the game never reaches a stereo
    // present, and will downgrade gracefully via SR-Lib's LoadLibrary probe
    // (CreateSRInterfaceDX11 returns E_NOINTERFACE) if they do.

    // Load SwapEyes setting
    p = strstr(buf, "SwapEyes");
    if (p)
    {
        const char* v = strstr(p, "Value=\"");
        if (v)
        {
            int swap = atoi(v + 7);
            g_bSwapEyes = (swap != 0);
        }
    }

    // Load AnaglyphColour
    p = strstr(buf, "AnaglyphColour");
    if (p)
    {
        const char* v = strstr(p, "Value=\"");
        if (v)
        {
            int col = atoi(v + 7);
            if (col >= AC_RED_CYAN && col <= AC_AMBER_BLUE)
                g_AnaglyphColour = static_cast<AnaglyphColour>(col);
        }
    }

    // Load AnaglyphMethod
    p = strstr(buf, "AnaglyphMethod");
    if (p)
    {
        const char* v = strstr(p, "Value=\"");
        if (v)
        {
            int meth = atoi(v + 7);
            if (meth >= AM_DUBOIS && meth <= AM_TRUE)
                g_AnaglyphMethod = static_cast<AnaglyphMethod>(meth);
        }
    }
}

// ============================================================
// D3D11 state
// ============================================================
static ID3D11Device*        g_pDevice11  = nullptr;
static ID3D11DeviceContext* g_pContext    = nullptr;

// Present hook
// ============================================================
static bool g_bStereoActive       = false;
static bool g_bQbsCreated         = false;
static bool g_bHooked             = false;

// Expose small control APIs so other in-tree proxies (NVAPI proxy) can
// drive our compositor without duplicating heavy internals.
extern "C" {
__declspec(dllexport) void AmdQbProxy_SetStereoActive(bool active)
{
    g_bStereoActive = active;
    char buf[128];
    wsprintfA(buf, "[AmdQbProxy] External SetStereoActive -> %d\n", (int)active);
    WriteLog(buf);
}

__declspec(dllexport) int AmdQbProxy_IsStereoActive()
{
    return g_bStereoActive ? 1 : 0;
}

__declspec(dllexport) void AmdQbProxy_SetOutputMode(int mode)
{
    if (mode >= 0 && mode <= static_cast<int>(OM_SR_WEAVE)) g_OutputMode = static_cast<OutputMode>(mode);
    char buf[128];
    wsprintfA(buf, "[AmdQbProxy] External SetOutputMode -> %d\n", mode);
    WriteLog(buf);
}

__declspec(dllexport) void AmdQbProxy_SetSeparation(float sep)
{
    // Stub: store as basic log; detailed per-handle convergence/separation
    // can be implemented by extending proxy state if needed.
    char buf[128];
    wsprintfA(buf, "[AmdQbProxy] External SetSeparation -> %f\n", sep);
    WriteLog(buf);
}

__declspec(dllexport) void AmdQbProxy_SetConvergence(float conv)
{
    char buf[128];
    wsprintfA(buf, "[AmdQbProxy] External SetConvergence -> %f\n", conv);
    WriteLog(buf);
}
} // extern "C"

typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain*, UINT, UINT);
static PFN_Present g_pfnOrigPresent = nullptr;
using PFN_SetFullscreen=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,BOOL,IDXGIOutput*);
using PFN_GetFullscreen=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,BOOL*,IDXGIOutput**);
static PFN_SetFullscreen originalSetFullscreen{};
static PFN_GetFullscreen originalGetFullscreen{};

typedef HRESULT (STDMETHODCALLTYPE *PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PFN_Present1 g_pfnOrigPresent1 = nullptr;

// Per-swap-chain shadow render target.  The real swap chain back buffer stays
// at display resolution; the shadow texture is doubled in height.  Games see
// the shadow via the hooked IDXGISwapChain::GetBuffer; the compositor draws
// SBS from the shadow into the real (display-sized) BB before Present.
struct ShadowData {
    ID3D11Texture2D*          pTex;     // shadow texture (gameW or W) x (2*origH)
    ID3D11ShaderResourceView* pSRV;     // SRV for compositor input
    UINT                      origH;    // original (non-doubled) height
    UINT                      gameW;    // game's intended width (non-zero in full SBS mode only)
    bool virtualFullscreen{};
    void Release() {
        if (pSRV) { pSRV->Release(); pSRV = nullptr; }
        if (pTex) { pTex->Release(); pTex = nullptr; }
        origH = 0;
        gameW = 0;
    }
};
static std::unordered_map<IDXGISwapChain*, ShadowData> g_scShadow;
static HRESULT STDMETHODCALLTYPE HookSetFullscreen(IDXGISwapChain* sc,BOOL fullscreen,IDXGIOutput* output) {
    auto it=g_scShadow.find(sc);
    if(HeadsetDisplay::Active() && it!=g_scShadow.end()){it->second.virtualFullscreen=fullscreen!=FALSE;return S_OK;}
    return originalSetFullscreen(sc,fullscreen,output);
}
static HRESULT STDMETHODCALLTYPE HookGetFullscreen(IDXGISwapChain* sc,BOOL* fullscreen,IDXGIOutput** output) {
    HRESULT hr=originalGetFullscreen(sc,fullscreen,output);
    auto it=g_scShadow.find(sc);
    if(SUCCEEDED(hr) && fullscreen && HeadsetDisplay::Active() && it!=g_scShadow.end())*fullscreen=it->second.virtualFullscreen;
    return hr;
}

static bool g_bPresentLogOnce = false;
static int  g_nPresentCount  = 0;    // Diagnostic: count Present calls

// --- GetBuffer hook: returns the doubled shadow texture instead of the real BB ---
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetBuffer)(IDXGISwapChain*, UINT, REFIID, void**);
static PFN_GetBuffer g_pfnOrigGetBuffer = nullptr;

// --- ResizeBuffers hook ---
typedef HRESULT (STDMETHODCALLTYPE *PFN_ResizeBuffers)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PFN_ResizeBuffers g_pfnOrigResizeBuffers = nullptr;

// --- CreateSwapChain hook: creates shadow render target for stereo ---
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChain)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
static PFN_CreateSwapChain g_pfnOrigCreateSwapChain = nullptr;

// --- GetDesc hook: reports doubled height to game for stereo swap chains ---
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDesc)(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*);
static PFN_GetDesc g_pfnOrigGetDesc = nullptr;

// --- GetDesc1 hook: DXGI 1.1 version ---
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDesc1)(IDXGISwapChain1*, DXGI_SWAP_CHAIN_DESC1*);
static PFN_GetDesc1 g_pfnOrigGetDesc1 = nullptr;

// --- ResizeTarget hook: prevents doubled height leaking to display mode ---
typedef HRESULT (STDMETHODCALLTYPE *PFN_ResizeTarget)(IDXGISwapChain*, const DXGI_MODE_DESC*);
static PFN_ResizeTarget g_pfnOrigResizeTarget = nullptr;

// Helper: make a format typeless so the shadow supports both UNORM and SRGB RTVs.
static DXGI_FORMAT MakeTypeless(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default: return f;
    }
}

using TextureDescFn=void(STDMETHODCALLTYPE*)(ID3D11Texture2D*,D3D11_TEXTURE2D_DESC*);
static TextureDescFn originalTextureDesc{};
static void STDMETHODCALLTYPE GameTextureDesc(ID3D11Texture2D* texture,D3D11_TEXTURE2D_DESC* desc) {
    originalTextureDesc(texture,desc);
    // Only the executable sees the logical single-eye height. Transport, GPU
    // copies and runtime callers must continue to see the physical two-eye size.
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
    if(!desc || caller<base || caller>=base+nt->OptionalHeader.SizeOfImage)return;
    auto found=g_scShadow.end();
    for(auto it=g_scShadow.begin();it!=g_scShadow.end();++it)if(it->second.pTex==texture){found=it;break;}
    if(found==g_scShadow.end() || !found->second.origH)return;
    desc->Height=found->second.origH;
    static unsigned reports=0;
    if(reports++<8){char line[180];sprintf_s(line,"[StereoSizing] Game texture query callerRVA=%08x logical=%ux%u physicalHeight=%u\n",unsigned(caller-base),desc->Width,desc->Height,desc->Height*2);WriteLog(line);}
}
static bool InstallTextureDesc(ID3D11Texture2D* texture) {
    if(originalTextureDesc)return true;
    auto vt=*reinterpret_cast<void***>(texture);
    auto result=MH_CreateHook(vt[10],&GameTextureDesc,reinterpret_cast<void**>(&originalTextureDesc));
    if(result!=MH_OK)return false;
    if(MH_EnableHook(vt[10])==MH_OK)return true;
    MH_RemoveHook(vt[10]);originalTextureDesc=nullptr;return false;
}

// Helper: create (or recreate) the shadow texture for a stereo swap chain.
// gameW: non-zero only in full SBS mode — shadow uses game width, not real BB width.
static bool CreateShadow(IDXGISwapChain* pSC, ID3D11Device* pDev,
                          UINT W, UINT origH, DXGI_FORMAT fmt, UINT gameW = 0)
{
    auto& sd = g_scShadow[pSC];
    sd.Release();

    UINT shadowW = gameW ? gameW : W;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = shadowW;
    td.Height           = origH * 2;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = fmt;   // Use typed format so games can create RTVs with DXGI_FORMAT_UNKNOWN
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(pDev->CreateTexture2D(&td, nullptr, &sd.pTex)))
    {
        g_scShadow.erase(pSC);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
    svd.Format                    = fmt;   // typed format for shader reads
    svd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels       = 1;
    if (FAILED(pDev->CreateShaderResourceView(sd.pTex, &svd, &sd.pSRV)))
    {
        sd.Release();
        g_scShadow.erase(pSC);
        return false;
    }
    sd.origH = origH;
    sd.gameW = gameW;
    if(!InstallTextureDesc(sd.pTex)) {
        WriteLog("[StereoSizing] Texture descriptor hook failed; shadow creation aborted\n");
        sd.Release();g_scShadow.erase(pSC);return false;
    }

    char buf[200];
    wsprintfA(buf, "[AmdQbProxy] Shadow created: %ux%u (origH=%u fmt=%u gameW=%u)\n",
              shadowW, origH * 2, origH, fmt, gameW);
    WriteLog(buf);
    return true;
}

static HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
    IDXGIFactory* pFactory, IUnknown* pDevice,
    DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSC)
{
    UINT origH = 0;
    UINT gameW = 0;  // non-zero in full SBS fullscreen mode

    bool fullSBSMode = (g_OutputMode == OM_SBS_FULL);

    if (pDesc && g_bStereoActive)
    {
        origH = pDesc->BufferDesc.Height;
        if (fullSBSMode && !pDesc->Windowed)
        {
            gameW = pDesc->BufferDesc.Width;
            char buf[200];
            wsprintfA(buf, "[AmdQbProxy] CreateSwapChain: %ux%u (fullSBS, doubling width -> %u)\n",
                      gameW, origH, gameW * 2);
            WriteLog(buf);
        }
        else
        {
            char buf[160];
            wsprintfA(buf, "[AmdQbProxy] CreateSwapChain: %ux%u (stereo, shadow mode)\n",
                      pDesc->BufferDesc.Width, origH);
            WriteLog(buf);
        }
    }
    else if (pDesc)
    {
        char buf[128];
        wsprintfA(buf, "[AmdQbProxy] CreateSwapChain: %ux%u (no stereo)\n",
                  pDesc->BufferDesc.Width, pDesc->BufferDesc.Height);
        WriteLog(buf);
    }

    // For full SBS: pass a modified desc with doubled width to the driver so the
    // real BB is 2x game width.  We copy instead of modifying pDesc so the
    // caller's struct stays untouched (modifying it breaks games that read it back).
    DXGI_SWAP_CHAIN_DESC descForDriver;
    DXGI_SWAP_CHAIN_DESC* pDescToPass = pDesc;
    if (gameW > 0 && pDesc)
    {
        descForDriver = *pDesc;
        descForDriver.BufferDesc.Width = gameW * 2;
        pDescToPass = &descForDriver;
    }

    if(pDesc && g_bStereoActive && HeadsetDisplay::Active()) {
        descForDriver=*pDescToPass;
        descForDriver.Windowed=TRUE;
        descForDriver.BufferDesc.RefreshRate={0,1};
        descForDriver.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        pDescToPass=&descForDriver;
        WriteLog("[TombRaiderVR] Headset-sized render target, windowed asynchronous desktop mirror\n");
    }
    HRESULT hr = g_pfnOrigCreateSwapChain(pFactory, pDevice, pDescToPass, ppSC);
    if (SUCCEEDED(hr) && ppSC && *ppSC && origH > 0)
    {
        IDXGISwapChain* pSC = *ppSC;
        ID3D11Device* pDev = nullptr;
        pDevice->QueryInterface(__uuidof(ID3D11Device),
                                reinterpret_cast<void**>(&pDev));
        if (pDev)
        {
            DXGI_SWAP_CHAIN_DESC desc = {};
            pSC->GetDesc(&desc);
            if (CreateShadow(pSC, pDev, desc.BufferDesc.Width, origH,
                             desc.BufferDesc.Format, gameW))
            {
                g_scShadow[pSC].virtualFullscreen=pDesc && !pDesc->Windowed;
                // NOTE: Do NOT modify pDesc->BufferDesc.Height here.
                // On real AMD hardware the driver doubles the BB internally
                // but leaves the caller's DXGI_SWAP_CHAIN_DESC struct
                // unchanged.  Games discover the doubled height through
                // GetDesc() (hooked) and GetBuffer() (returns shadow).
                // Modifying pDesc broke Sniper Elite V2 and other Rebellion
                // engine games that compute viewport offsets from the
                // original pDesc value.

                char buf[300];
                wsprintfA(buf, "[AmdQbProxy] CreateSwapChain: shadow %ux%u OK (origH=%u gameW=%u swapEff=%u flags=0x%X windowed=%d)\n",
                          desc.BufferDesc.Width, origH * 2, origH, gameW,
                          desc.SwapEffect, desc.Flags, desc.Windowed);
                WriteLog(buf);
            }
            else
            {
                WriteLog("[AmdQbProxy] CreateSwapChain: shadow creation FAILED\n");
            }
            pDev->Release();
        }
    }
    return hr;
}

// --- ResizeBuffers hook: recreates shadow when game resizes ---
static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(
    IDXGISwapChain* pSC, UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    // Release old shadow before ResizeBuffers (buffer refs are invalidated)
    auto it = g_scShadow.find(pSC);
    bool wasStereo = (it != g_scShadow.end());
    UINT oldOrigH  = wasStereo ? it->second.origH : 0;
    UINT oldGameW  = wasStereo ? it->second.gameW : 0;
    if (wasStereo) it->second.Release();

    // For full SBS: track game's intended width and double it for the real call.
    UINT callWidth = Width;
    UINT newGameW  = 0;
    if (g_bStereoActive && oldGameW > 0)
    {
        // Preserve game width: if Width==0 keep oldGameW, else use what game passed.
        newGameW = (Width > 0) ? Width : oldGameW;
        if (Width > 0)
            callWidth = Width * 2;  // real BB is 2x game width
        // Width==0 means "keep current" — real BB already doubled, so callWidth stays 0.
    }

    if (g_bStereoActive && Height > 0)
    {
        // Guard: if game passes the doubled shadow height, use origH instead
        if (wasStereo && Height == oldOrigH * 2)
        {
            char buf[160];
            wsprintfA(buf, "[AmdQbProxy] ResizeBuffers: %ux%u (shadow height, using origH=%u)\n",
                      Width, Height, oldOrigH);
            WriteLog(buf);
            Height = oldOrigH;
        }
        char buf[200];
        wsprintfA(buf, "[AmdQbProxy] ResizeBuffers: %ux%u -> real %ux%u (stereo%s)\n",
                  Width, Height, callWidth, Height, oldGameW > 0 ? ", fullSBS" : "");
        WriteLog(buf);
    }
    else if (Width > 0 || Height > 0)
    {
        char buf[128];
        wsprintfA(buf, "[AmdQbProxy] ResizeBuffers: %ux%u (no stereo)\n",
                  Width, Height);
        WriteLog(buf);
    }

    HRESULT hr = g_pfnOrigResizeBuffers(pSC, BufferCount, callWidth, Height,
                                         NewFormat, SwapChainFlags);

    // Recreate shadow after successful resize
    if (SUCCEEDED(hr) && g_bStereoActive && Height > 0)
    {
        ID3D11Device* pDev = nullptr;
        pSC->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&pDev));
        if (pDev)
        {
            DXGI_SWAP_CHAIN_DESC desc = {};
            pSC->GetDesc(&desc);
            DXGI_FORMAT fmt = (NewFormat != DXGI_FORMAT_UNKNOWN)
                              ? NewFormat : desc.BufferDesc.Format;
            CreateShadow(pSC, pDev, desc.BufferDesc.Width, Height, fmt, newGameW);
            pDev->Release();
        }
    }
    return hr;
}

// --- GetBuffer hook: return the doubled shadow texture instead of the real BB ---
static HRESULT STDMETHODCALLTYPE HookedGetBuffer(
    IDXGISwapChain* pSC, UINT Buffer, REFIID riid, void** ppSurface)
{
    if (Buffer == 0 && ppSurface)
    {
        auto it = g_scShadow.find(pSC);
        if (it != g_scShadow.end() && it->second.pTex)
            return it->second.pTex->QueryInterface(riid, ppSurface);
    }
    return g_pfnOrigGetBuffer(pSC, Buffer, riid, ppSurface);
}

// Game-facing descriptors describe one eye. The physical back buffer is doubled;
// GetLineOffset supplies the right-eye destination independently.
static HRESULT STDMETHODCALLTYPE HookedGetDesc(
    IDXGISwapChain* pSC, DXGI_SWAP_CHAIN_DESC* pDesc)
{
    HRESULT hr = g_pfnOrigGetDesc(pSC, pDesc);
    if (SUCCEEDED(hr) && pDesc)
    {
        auto it = g_scShadow.find(pSC);
        if (it != g_scShadow.end() && it->second.origH > 0)
        {
            pDesc->BufferDesc.Height = it->second.origH;
            if(HeadsetDisplay::Active())pDesc->Windowed=!it->second.virtualFullscreen;
            if (it->second.gameW > 0)
                pDesc->BufferDesc.Width = it->second.gameW;
            static bool s_bLogOnce = false;
            if (!s_bLogOnce)
            {
                s_bLogOnce = true;
                char buf[200];
                wsprintfA(buf, "[AmdQbProxy] GetDesc: reporting %ux%u (origH=%u gameW=%u)\n",
                          pDesc->BufferDesc.Width, pDesc->BufferDesc.Height,
                          it->second.origH, it->second.gameW);
                WriteLog(buf);
            }
        }
    }
    return hr;
}

// --- GetDesc1 hook: DXGI 1.1 version ---
static HRESULT STDMETHODCALLTYPE HookedGetDesc1(
    IDXGISwapChain1* pSC1, DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    HRESULT hr = g_pfnOrigGetDesc1(pSC1, pDesc);
    if (SUCCEEDED(hr) && pDesc)
    {
        auto it = g_scShadow.find(static_cast<IDXGISwapChain*>(pSC1));
        if (it != g_scShadow.end() && it->second.origH > 0)
        {
            pDesc->Height = it->second.origH;
            if (it->second.gameW > 0)
                pDesc->Width = it->second.gameW;
        }
    }
    return hr;
}

// --- ResizeTarget hook: halve doubled height to prevent bad display mode changes ---
// Games that read our doubled GetDesc height may pass it to ResizeTarget, which
// would attempt to change the display mode to an impossibly tall resolution.
static HRESULT STDMETHODCALLTYPE HookedResizeTarget(
    IDXGISwapChain* pSC, const DXGI_MODE_DESC* pNewTargetParameters)
{
    if(HeadsetDisplay::Active() && g_scShadow.count(pSC))return S_OK;
    if (pNewTargetParameters)
    {
        auto it = g_scShadow.find(pSC);
        if (it != g_scShadow.end())
        {
            bool needModify = false;
            DXGI_MODE_DESC modified = *pNewTargetParameters;

            // Halve doubled height the game may have read back from our hooked GetDesc.
            if (it->second.origH > 0 && modified.Height == it->second.origH * 2)
            {
                modified.Height = it->second.origH;
                needModify = true;
            }

            // Double game width in full SBS mode so the display mode matches the real BB.
            if (it->second.gameW > 0 && modified.Width == it->second.gameW)
            {
                modified.Width = it->second.gameW * 2;
                needModify = true;
            }

            if (needModify)
            {
                char buf[200];
                wsprintfA(buf, "[AmdQbProxy] ResizeTarget: %ux%u -> %ux%u\n",
                          pNewTargetParameters->Width, pNewTargetParameters->Height,
                          modified.Width, modified.Height);
                WriteLog(buf);
                return g_pfnOrigResizeTarget(pSC, &modified);
            }
        }
    }
    return g_pfnOrigResizeTarget(pSC, pNewTargetParameters);
}

// Lazily create a shadow-fixup texture matching the shadow's dimensions.
// shadowH is the full shadow height (origH * 2). Returned texture has the
// same size and format as the shadow itself.
// TombRaiderVR: consume the uncompressed native eye pair before desktop Present.
// Neither eye is retained from an earlier Present. DXGI_PRESENT_TEST is inert.
static int traceBudget=32;
static bool traceKey=false;
using DrawFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
using DrawIndexedFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
using CopyRegionFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,UINT,UINT,UINT,ID3D11Resource*,UINT,const D3D11_BOX*);
static DrawFn originalDraw{};
static DrawIndexedFn originalDrawIndexed{};
static CopyRegionFn originalCopyRegion{};
static bool IsShadow(ID3D11Resource* r) {
    for(auto& entry:g_scShadow)if(entry.second.pTex==r)return true;
    return false;
}
static void TraceDraw(ID3D11DeviceContext* ctx,const char* kind) {
    if(traceBudget<=0)return;
    ID3D11RenderTargetView* rt{};ctx->OMGetRenderTargets(1,&rt,nullptr);if(!rt)return;
    ID3D11Resource* resource{};rt->GetResource(&resource);rt->Release();
    bool match=IsShadow(resource);resource->Release();if(!match)return;
    --traceBudget;
    D3D11_VIEWPORT vp[16]{};UINT count=16;ctx->RSGetViewports(&count,vp);
    char line[512];sprintf_s(line,"[StereoTrace] %s viewportCount=%u viewport=(%.1f,%.1f %.1fx%.1f)\n",kind,count,vp[0].TopLeftX,vp[0].TopLeftY,vp[0].Width,vp[0].Height);WriteLog(line);
    ID3D11ShaderResourceView* views[8]{};ctx->PSGetShaderResources(0,8,views);
    for(UINT i=0;i<8;i++)if(views[i]) {
        ID3D11Resource* r{};ID3D11Texture2D* tex{};views[i]->GetResource(&r);
        if(SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&tex)))) {
            D3D11_TEXTURE2D_DESC d{};tex->GetDesc(&d);sprintf_s(line,"[StereoTrace] PS slot=%u source=%ux%u array=%u format=%u\n",i,d.Width,d.Height,d.ArraySize,d.Format);WriteLog(line);tex->Release();
        }
        r->Release();views[i]->Release();
    }
}
static void STDMETHODCALLTYPE TraceDrawHook(ID3D11DeviceContext* c,UINT a,UINT b){TraceDraw(c,"Draw");originalDraw(c,a,b);}
static void STDMETHODCALLTYPE TraceDrawIndexedHook(ID3D11DeviceContext* c,UINT a,UINT b,INT d){TraceDraw(c,"DrawIndexed");originalDrawIndexed(c,a,b,d);}
static void STDMETHODCALLTYPE TraceCopyHook(ID3D11DeviceContext* c,ID3D11Resource* dst,UINT sub,UINT x,UINT y,UINT z,ID3D11Resource* src,UINT srcSub,const D3D11_BOX* box) {
    if(traceBudget>0 && IsShadow(dst)) {
        --traceBudget;ID3D11Texture2D* tex{};
        if(SUCCEEDED(src->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&tex)))) {
            D3D11_TEXTURE2D_DESC d{};tex->GetDesc(&d);tex->Release();
            D3D11_BOX b=box?*box:D3D11_BOX{0,0,0,d.Width,d.Height,1};char line[512];
            sprintf_s(line,"[StereoTrace] Copy source=%ux%u box=(%u,%u)-(%u,%u) destination=(%u,%u)\n",d.Width,d.Height,b.left,b.top,b.right,b.bottom,x,y);WriteLog(line);
        }
    }
    originalCopyRegion(c,dst,sub,x,y,z,src,srcSub,box);
}
static void InstallStereoTrace() {
    if(originalDraw || !g_pDevice11)return;
    ID3D11DeviceContext* ctx{};g_pDevice11->GetImmediateContext(&ctx);if(!ctx)return;
    auto vt=*reinterpret_cast<void***>(ctx);
    if(MH_CreateHook(vt[13],&TraceDrawHook,reinterpret_cast<void**>(&originalDraw))==MH_OK)MH_EnableHook(vt[13]);
    if(MH_CreateHook(vt[12],&TraceDrawIndexedHook,reinterpret_cast<void**>(&originalDrawIndexed))==MH_OK)MH_EnableHook(vt[12]);
    if(MH_CreateHook(vt[46],&TraceCopyHook,reinterpret_cast<void**>(&originalCopyRegion))==MH_OK)MH_EnableHook(vt[46]);
    ctx->Release();WriteLog("[StereoTrace] Final stereo draw/copy tracing installed\n");
}
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    bool pressed=(GetAsyncKeyState(VK_F8)&0x8000)!=0;
    if(pressed&&!traceKey)traceBudget=32;
    traceKey=pressed;
    if (!(flags & DXGI_PRESENT_TEST) && g_bStereoActive) {
        auto it = g_scShadow.find(sc);
        if (it != g_scShadow.end() && it->second.pTex && it->second.origH) {
            NativeBridge::Present(sc, it->second.pTex, it->second.origH, g_bSwapEyes);
            // Flat monitor mirror: current left eye, full size; no shader state changes.
            ID3D11Texture2D* bb = nullptr;
            if (g_pfnOrigGetBuffer && SUCCEEDED(g_pfnOrigGetBuffer(sc, 0,
                    __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) {
                D3D11_TEXTURE2D_DESC d{}, src{};
                bb->GetDesc(&d); it->second.pTex->GetDesc(&src);
                ID3D11Device* mirrorDevice=nullptr;
                ID3D11DeviceContext* mirrorContext=nullptr;
                it->second.pTex->GetDevice(&mirrorDevice);
                mirrorDevice->GetImmediateContext(&mirrorContext);
                if (mirrorContext && d.Width == src.Width && d.Height == it->second.origH &&
                    d.Format == src.Format && d.SampleDesc.Count == 1) {
                    D3D11_BOX box{0,0,0,d.Width,d.Height,1};
                    mirrorContext->CopySubresourceRegion(bb,0,0,0,0,it->second.pTex,0,&box);
                }
                mirrorContext->Release(); mirrorDevice->Release();
                bb->Release();
            }
        }
    }
    if(!(flags & DXGI_PRESENT_TEST) && g_bStereoActive && g_scShadow.count(sc)) {
        HRESULT hr=g_pfnOrigPresent(sc,0,flags|DXGI_PRESENT_DO_NOT_WAIT);
        return hr==DXGI_ERROR_WAS_STILL_DRAWING?S_OK:hr;
    }
    return g_pfnOrigPresent(sc, sync, flags);
}

// Present1 hook (IDXGISwapChain1::Present1, vtable slot 22) — same as Present but DXGI 1.1 API.
// Some games (Hitman: Absolution) use Present for loading then switch to Present1 for rendering.
static HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* pSC1, UINT SyncInterval, UINT PresentFlags,
                                                 const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    // Run the full compositor through HookedPresent (which calls g_pfnOrigPresent at the end).
    // Present and Present1 are functionally equivalent for our purposes — the extra
    // DXGI_PRESENT_PARAMETERS only affect dirty-rect optimisations.
    return HookedPresent(pSC1, SyncInterval, PresentFlags);
}

static void InstallPresentHook()
{
    HeadsetDisplay::Install();
    InstallStereoTrace();
    if (g_bHooked || !g_pDevice11) return;

    // Create a throwaway window to host the dummy swap chain
    HWND hWnd = CreateWindowW(L"STATIC", nullptr, WS_POPUP, 0, 0, 8, 8,
                              nullptr, nullptr, nullptr, nullptr);
    if (!hWnd) return;

    IDXGIDevice* pDXGIDevice = nullptr;
    if (FAILED(g_pDevice11->QueryInterface(__uuidof(IDXGIDevice),
                                           reinterpret_cast<void**>(&pDXGIDevice))))
    { DestroyWindow(hWnd); return; }

    IDXGIAdapter* pAdapter = nullptr;
    pDXGIDevice->GetAdapter(&pAdapter);
    pDXGIDevice->Release();
    if (!pAdapter) { DestroyWindow(hWnd); return; }

    IDXGIFactory* pFactory = nullptr;
    pAdapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&pFactory));
    pAdapter->Release();
    if (!pFactory) { DestroyWindow(hWnd); return; }

    DXGI_SWAP_CHAIN_DESC scd    = {};
    scd.BufferCount             = 1;
    scd.BufferDesc.Width        = 8;
    scd.BufferDesc.Height       = 8;
    scd.BufferDesc.Format       = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage             = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow            = hWnd;
    scd.SampleDesc.Count        = 1;
    scd.Windowed                = TRUE;

    IDXGISwapChain* pDummy = nullptr;
    if (SUCCEEDED(pFactory->CreateSwapChain(g_pDevice11, &scd, &pDummy)))
    {
        // IDXGISwapChain::Present is vtable slot 8 (0-indexed)
        void** vtable    = *reinterpret_cast<void***>(pDummy);
        if(MH_CreateHook(vtable[10],&HookSetFullscreen,reinterpret_cast<void**>(&originalSetFullscreen))==MH_OK)MH_EnableHook(vtable[10]);
        if(MH_CreateHook(vtable[11],&HookGetFullscreen,reinterpret_cast<void**>(&originalGetFullscreen))==MH_OK)MH_EnableHook(vtable[11]);
        void*  presentFn = vtable[8];

        if (MH_CreateHook(presentFn,
                          reinterpret_cast<LPVOID>(&HookedPresent),
                          reinterpret_cast<LPVOID*>(&g_pfnOrigPresent)) == MH_OK)
            MH_EnableHook(presentFn);

        // IDXGISwapChain1::Present1 is vtable slot 22 (DXGI 1.1)
        // Some games switch from Present to Present1 after initial setup.
        {
            IDXGISwapChain1* pDummy1 = nullptr;
            if (SUCCEEDED(pDummy->QueryInterface(__uuidof(IDXGISwapChain1),
                                                  reinterpret_cast<void**>(&pDummy1))))
            {
                void** vtable1     = *reinterpret_cast<void***>(pDummy1);
                void*  present1Fn  = vtable1[22];
                if (present1Fn != presentFn)
                {
                    if (MH_CreateHook(present1Fn,
                                      reinterpret_cast<LPVOID>(&HookedPresent1),
                                      reinterpret_cast<LPVOID*>(&g_pfnOrigPresent1)) == MH_OK)
                    {
                        MH_EnableHook(present1Fn);
                        WriteLog("[AmdQbProxy] IDXGISwapChain1::Present1 hook installed\n");
                    }
                }
                // IDXGISwapChain1::GetDesc1 is vtable slot 18
                void* getDesc1Fn = vtable1[18];
                if (MH_CreateHook(getDesc1Fn,
                                  reinterpret_cast<LPVOID>(&HookedGetDesc1),
                                  reinterpret_cast<LPVOID*>(&g_pfnOrigGetDesc1)) == MH_OK)
                {
                    MH_EnableHook(getDesc1Fn);
                    WriteLog("[AmdQbProxy] IDXGISwapChain1::GetDesc1 hook installed\n");
                }

                pDummy1->Release();
            }
        }

        // IDXGISwapChain::ResizeBuffers is vtable slot 13
        void* resizeFn = vtable[13];
        if (MH_CreateHook(resizeFn,
                          reinterpret_cast<LPVOID>(&HookedResizeBuffers),
                          reinterpret_cast<LPVOID*>(&g_pfnOrigResizeBuffers)) == MH_OK)
        {
            MH_EnableHook(resizeFn);
            WriteLog("[AmdQbProxy] IDXGISwapChain::ResizeBuffers hook installed\n");
        }

        // IDXGISwapChain::GetBuffer is vtable slot 9
        void* getBufferFn = vtable[9];
        if (MH_CreateHook(getBufferFn,
                          reinterpret_cast<LPVOID>(&HookedGetBuffer),
                          reinterpret_cast<LPVOID*>(&g_pfnOrigGetBuffer)) == MH_OK)
        {
            MH_EnableHook(getBufferFn);
            WriteLog("[AmdQbProxy] IDXGISwapChain::GetBuffer hook installed\n");
        }

        // IDXGISwapChain::GetDesc is vtable slot 12
        void* getDescFn = vtable[12];
        if (MH_CreateHook(getDescFn,
                          reinterpret_cast<LPVOID>(&HookedGetDesc),
                          reinterpret_cast<LPVOID*>(&g_pfnOrigGetDesc)) == MH_OK)
        {
            MH_EnableHook(getDescFn);
            WriteLog("[AmdQbProxy] IDXGISwapChain::GetDesc hook installed\n");
        }

        // IDXGISwapChain::ResizeTarget is vtable slot 14
        void* resizeTargetFn = vtable[14];
        if (MH_CreateHook(resizeTargetFn,
                          reinterpret_cast<LPVOID>(&HookedResizeTarget),
                          reinterpret_cast<LPVOID*>(&g_pfnOrigResizeTarget)) == MH_OK)
        {
            MH_EnableHook(resizeTargetFn);
            WriteLog("[AmdQbProxy] IDXGISwapChain::ResizeTarget hook installed\n");
        }

        g_bHooked = true;
        WriteLog("[AmdQbProxy] IDXGISwapChain::Present hook installed\n");
        pDummy->Release();

        // Hook IDXGIFactory::CreateSwapChain (vtable[10]) to create shadow for stereo
        {
            void** factoryVtbl = *reinterpret_cast<void***>(pFactory);
            void*  createSCFn  = factoryVtbl[10];
            if (MH_CreateHook(createSCFn,
                              reinterpret_cast<LPVOID>(&HookedCreateSwapChain),
                              reinterpret_cast<LPVOID*>(&g_pfnOrigCreateSwapChain)) == MH_OK)
            {
                MH_EnableHook(createSCFn);
                WriteLog("[AmdQbProxy] IDXGIFactory::CreateSwapChain hook installed\n");
            }
        }
    }

    pFactory->Release();
    DestroyWindow(hWnd);
}

// ============================================================
// Fake AMD COM interfaces
// ============================================================

// IAmdDxExtQuadBufferStereo implementation
class FakeAmdDxExtQbStereo : public IAmdDxExtQuadBufferStereo
{
    ULONG m_ref;
public:
    FakeAmdDxExtQbStereo() : m_ref(1) {}

    unsigned int AddRef()  override { return ++m_ref; }
    unsigned int Release() override
    {
        ULONG r = --m_ref;
        if (r == 0)
        {
            WriteLog("[AmdQbProxy] FakeAmdDxExtQbStereo destroyed\n");
            delete this;
        }
        return r;
    }

    HRESULT EnableQuadBufferStereo(BOOL enable) override
    {
        g_bStereoActive = (enable != FALSE);
        if (g_bStereoActive)
        {
            WriteLog("[AmdQbProxy] EnableQuadBufferStereo(TRUE) - installing Present hook\n");
            InstallPresentHook();
        }
        else
        {
            WriteLog("[AmdQbProxy] EnableQuadBufferStereo(FALSE) - stereo disabled\n");
        }
        return S_OK;
    }

    // Game uses this to determine where the right eye starts in the back buffer.
    // The shadow texture is 2*origH tall; we report origH as the split point.
    UINT GetLineOffset(IDXGISwapChain* pSC) override
    {
        UINT offset = 0;
        if (pSC)
        {
            auto it = g_scShadow.find(pSC);
            if (it != g_scShadow.end())
                offset = it->second.origH;
        }
        if (!offset && pSC)
        {
            DXGI_SWAP_CHAIN_DESC desc = {};
            pSC->GetDesc(&desc);
            offset = desc.BufferDesc.Height;
        }
        static int s_nLogCount = 0;
        if (++s_nLogCount <= 10 || (s_nLogCount % 5000) == 0)
        {
            char buf[128];
            wsprintfA(buf, "[AmdQbProxy] GetLineOffset(SC=%p) -> %u (call #%d)\n", pSC, offset, s_nLogCount);
            WriteLog(buf);
        }
        return offset;
    }

    // Return real DXGI output modes so the game considers HD3D available.
    // Uses CreateDXGIFactory instead of g_pDevice11 to avoid dangling-pointer
    // crashes when the game destroys/recreates the D3D11 device mid-init.
    HRESULT GetDisplayModeList(DXGI_FORMAT format, UINT flags,
                               UINT* pNum, DXGI_MODE_DESC* pModes) override
    {
        if (!pNum) return E_INVALIDARG;

        IDXGIFactory* pFactory = nullptr;
        if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory),
                                     reinterpret_cast<void**>(&pFactory))))
        { *pNum = 0; return S_OK; }

        IDXGIAdapter* pAdapter = nullptr;
        HRESULT hr = pFactory->EnumAdapters(0, &pAdapter);
        pFactory->Release();
        if (FAILED(hr)) { *pNum = 0; return S_OK; }

        IDXGIOutput* pOutput = nullptr;
        hr = pAdapter->EnumOutputs(0, &pOutput);
        pAdapter->Release();
        if (FAILED(hr)) { *pNum = 0; return S_OK; }

        DXGI_FORMAT fmt = (format != DXGI_FORMAT_UNKNOWN) ? format
                                                          : DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = pOutput->GetDisplayModeList(fmt, flags, pNum, pModes);
        pOutput->Release();

        char buf[256];
        wsprintfA(buf, "[AmdQbProxy] GetDisplayModeList(fmt=%u flags=0x%X): %u modes\n",
                  (UINT)fmt, flags, *pNum);
        WriteLog(buf);

        // Diagnostic dump: when the game requests the actual mode list (pModes
        // non-null), log each entry once per unique (format, flags, count) tuple.
        // Helps diagnose game-side stereo validation that rejects our forwarded
        // DXGI mode list (e.g. Thief looking for a 120Hz stereo refresh rate
        // that real AMD HD3D would advertise but we don't synthesize).
        if (pModes && *pNum > 0)
        {
            static UINT s_lastFmt   = 0xFFFFFFFFu;
            static UINT s_lastFlags = 0xFFFFFFFFu;
            static UINT s_lastNum   = 0xFFFFFFFFu;
            if ((UINT)fmt != s_lastFmt || flags != s_lastFlags || *pNum != s_lastNum)
            {
                s_lastFmt = (UINT)fmt; s_lastFlags = flags; s_lastNum = *pNum;
                for (UINT i = 0; i < *pNum; ++i)
                {
                    const DXGI_MODE_DESC& m = pModes[i];
                    UINT hz_x100 = m.RefreshRate.Denominator
                        ? (m.RefreshRate.Numerator * 100u) / m.RefreshRate.Denominator
                        : 0u;
                    wsprintfA(buf, "[AmdQbProxy]   mode[%u]: %ux%u @ %u.%02uHz fmt=%u scan=%u scale=%u\n",
                              i, m.Width, m.Height, hz_x100 / 100u, hz_x100 % 100u,
                              (UINT)m.Format, (UINT)m.ScanlineOrdering, (UINT)m.Scaling);
                    WriteLog(buf);
                }
            }
        }

        return hr;
    }
};

// IAmdDxExt implementation
class FakeAmdDxExt : public IAmdDxExt
{
    ULONG m_ref;
public:
    FakeAmdDxExt() : m_ref(1) {}

    unsigned int AddRef()  override { return ++m_ref; }
    unsigned int Release() override
    {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }

    HRESULT GetVersion(AmdDxExtVersion* pVer) override
    {
        if (pVer) { pVer->majorVersion = 1; pVer->minorVersion = 1; }
        return S_OK;
    }

    IAmdDxExtInterface* GetExtInterface(unsigned int iface) override
    {
        if (iface == AmdDxExtQuadBufferStereoID)
        {
            g_bQbsCreated = true;
            InstallPresentHook();  // Install hooks early for games that skip EnableQBS
            WriteLog("[AmdQbProxy] GetExtInterface(QuadBufferStereo) -> OK\n");
            return new (std::nothrow) FakeAmdDxExtQbStereo();
        }
        char buf[80];
        wsprintfA(buf, "[AmdQbProxy] GetExtInterface(%u) -> NULL\n", iface);
        WriteLog(buf);
        return nullptr;
    }

    // Remaining IAmdDxExt methods — not needed for stereo
    HRESULT IaSetPrimitiveTopology(unsigned int) override                { return E_NOTIMPL; }
    HRESULT IaGetPrimitiveTopology(AmdDxExtPrimitiveTopology*) override  { return E_NOTIMPL; }
    HRESULT SetSingleSampleRead(ID3D10Resource*, BOOL) override          { return E_NOTIMPL; }
    HRESULT SetSingleSampleRead11(ID3D11Resource*, BOOL) override        { return E_NOTIMPL; }
};

// ============================================================
// Exports
// Using extern "C" is safe here because our local AmdQbInterfaces.h does NOT
// forward-declare AmdDxExtCreate / AmdDxExtCreate11, so there is no C2732
// linkage-specification conflict.
// ============================================================
extern "C"
{
    // D3D10 path: return COM stubs.  No compositor (D3D10 TODO).
    HRESULT __cdecl AmdDxExtCreate(ID3D10Device* /*pDevice*/, IAmdDxExt** ppExt)
    {
        if (!ppExt) return E_INVALIDARG;
        *ppExt = new (std::nothrow) FakeAmdDxExt();
        return *ppExt ? S_OK : E_OUTOFMEMORY;
    }

    // D3D11 path: store device for compositor + return COM stubs.
    HRESULT __cdecl AmdDxExtCreate11(ID3D11Device* pDevice, IAmdDxExt** ppExt)
    {
        if (!ppExt) return E_INVALIDARG;

        char buf[128];
        wsprintfA(buf, "[AmdQbProxy] AmdDxExtCreate11 called (pDevice=%p)\n", pDevice);
        WriteLog(buf);

        if (!pDevice)
        {
            WriteLog("[AmdQbProxy]   pDevice is NULL, skipping device setup\n");
        }
        else
        {
            // DXHR adapter enumeration destroys probe devices before the next
            // extension request. Never retain/release a probe context across it.
            // This borrowed pointer is used only by immediate hook installation.
            g_pDevice11 = pDevice;
            WriteLog("[AmdQbProxy]   borrowed device for hook installation\n");
        }

        *ppExt = new (std::nothrow) FakeAmdDxExt();
        WriteLog("[AmdQbProxy]   returning FakeAmdDxExt\n");
        return *ppExt ? S_OK : E_OUTOFMEMORY;
    }
}

// ============================================================
// DllMain
// ============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_hSelf = hModule;
        DisableThreadLibraryCalls(hModule);
        LoadConfig();
        MH_Initialize();   // still needed for the IDXGIFactory::CreateSwapChain hook
        WriteLog("[AmdQbProxy] DLL_PROCESS_ATTACH: atidxx loaded OK (wiz3D " DISPLAYED_VERSION ")\n");
        break;

    case DLL_PROCESS_DETACH:
        // Process lifetime: never invoke graphics/runtime teardown under loader lock.
        break;
    }
    return TRUE;
}
