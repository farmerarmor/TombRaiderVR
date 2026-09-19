// D3d11VendorProxy.cpp
// d3d11.dll proxy that hooks DXGI CreateDXGIFactory* to spoof AMD vendor ID.
//
// Why d3d11.dll instead of dxgi.dll:
//   Steam's GameOverlayRenderer.dll loads system32\dxgi.dll before the game's
//   import table is resolved, so a local dxgi.dll proxy is never loaded.
//   d3d11.dll is NOT pre-loaded by Steam, so a local proxy wins the DLL
//   search order. We then use MinHook to patch CreateDXGIFactory* bytes in
//   the already-loaded system dxgi.dll, intercepting every call regardless of
//   which DLL triggers it.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define DISPLAYED_VERSION "TombRaiderVR 0.1.0 / wiz3D 981de7b"
#include <dxgi.h>
#include <dxgi1_2.h>
#include "MinHook.h"

static HINSTANCE g_hSelf      = nullptr;
static wchar_t   g_sysDir[MAX_PATH] = {};
static HMODULE   g_hRealD3D11 = nullptr;
static HMODULE   g_hRealDXGI  = nullptr;

// Pre-resolved D3D11 export pointers.  Captured when d3d11.dll is first
// loaded (inside RealD3D11), BEFORE middleware such as Epic's EOS overlay
// can patch the real DLL's Export Address Table (EAT).  If these are
// resolved lazily (on first thunk call), an EAT hook installed between
// d3d11.dll load and the thunk call returns the HOOKED address, causing
// infinite recursion and forcing the game into a DX9 fallback path.
typedef HRESULT(WINAPI*PFN_D3D11CreateDevice)(void*, int, void*, UINT,
    const void*, UINT, UINT, void**, void*, void**);
typedef HRESULT(WINAPI*PFN_D3D11CreateDeviceAndSwapChain)(void*, int, void*, UINT,
    const void*, UINT, UINT, const void*, void**, void**, void*, void**);
static PFN_D3D11CreateDevice             g_pfnD3D11CreateDevice = nullptr;
static PFN_D3D11CreateDeviceAndSwapChain g_pfnD3D11CreateDeviceAndSwapChain = nullptr;

// ---- Diagnostic log (same file as DxgiVendorProxy for unified output) -----

static void WriteLog(const char* msg)
{
    // Write next to the DLL (= game exe directory) so the log is easy to find.
    // Falls back to %TEMP% only when the module path is not yet available.
    wchar_t dir[MAX_PATH] = {};
    if (g_hSelf)
    {
        GetModuleFileNameW(g_hSelf, dir, MAX_PATH);
        wchar_t* p = wcsrchr(dir, L'\\');
        if (p) p[1] = L'\0';
    }
    if (!dir[0] && !GetTempPathW(MAX_PATH, dir)) return;

    wchar_t path[MAX_PATH];
    wcscpy_s(path, dir);
    #ifdef BLOCK_NVAPI_STEREO
        wcscat_s(path, L"HD3D_dxgi.log");
    #else
        wcscat_s(path, L"3DV_dxgi.log");
    #endif
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(h, msg, static_cast<DWORD>(strlen(msg)), &written, nullptr);
    CloseHandle(h);
}

static FARPROC RealD3D11(const char* name)
{
    if (!g_hRealD3D11 && g_sysDir[0])
    {
        wchar_t path[MAX_PATH];
        wcscpy_s(path, g_sysDir);
        wcscat_s(path, L"\\d3d11.dll");
        g_hRealD3D11 = LoadLibraryW(path);
        char buf[128];
        wsprintfA(buf, "[D3d11Proxy] real d3d11.dll %s (hMod=%p self=%p)\n",
                  g_hRealD3D11 ? "loaded OK" : "LOAD FAILED",
                  g_hRealD3D11, g_hSelf);
        WriteLog(buf);

        // Pre-resolve the two critical D3D11 exports RIGHT NOW, before any
        // middleware (Epic EOS overlay, etc.) can EAT-hook the real DLL.
        if (g_hRealD3D11)
        {
            g_pfnD3D11CreateDevice = (PFN_D3D11CreateDevice)
                GetProcAddress(g_hRealD3D11, "D3D11CreateDevice");
            g_pfnD3D11CreateDeviceAndSwapChain = (PFN_D3D11CreateDeviceAndSwapChain)
                GetProcAddress(g_hRealD3D11, "D3D11CreateDeviceAndSwapChain");
        }
    }
    return g_hRealD3D11 ? GetProcAddress(g_hRealD3D11, name) : nullptr;
}

// ---- DXGI vtable hook state ------------------------------------------------
// (Identical logic to DxgiVendorProxy — MinHook patches vtable slots so every
//  adapter instance returns VendorId == 0x1002.)

typedef HRESULT (WINAPI* PFN_GetDesc)(IDXGIAdapter*, DXGI_ADAPTER_DESC*);
typedef HRESULT (WINAPI* PFN_GetDesc1)(IDXGIAdapter1*, DXGI_ADAPTER_DESC1*);
typedef HRESULT (WINAPI* PFN_EnumAdapters)(IDXGIFactory*, UINT, IDXGIAdapter**);
typedef HRESULT (WINAPI* PFN_EnumAdapters1)(IDXGIFactory1*, UINT, IDXGIAdapter1**);

static PFN_GetDesc       g_pfnGetDesc       = nullptr;
static PFN_GetDesc1      g_pfnGetDesc1      = nullptr;
static PFN_EnumAdapters  g_pfnEnumAdapters  = nullptr;
static PFN_EnumAdapters1 g_pfnEnumAdapters1 = nullptr;

static bool g_bGetDescHooked       = false;
static bool g_bGetDesc1Hooked      = false;
static bool g_bEnumAdaptersHooked  = false;
static bool g_bEnumAdapters1Hooked = false;

// IDXGIFactory2 stereo hooks (DXGI 1.2 stereo path diagnostic + fix)
typedef BOOL (WINAPI* PFN_IsWindowedStereoEnabled)(IDXGIFactory2*);
typedef HRESULT (WINAPI* PFN_CreateSwapChainForHwnd)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

static PFN_IsWindowedStereoEnabled g_pfnIsWindowedStereoEnabled = nullptr;
static PFN_CreateSwapChainForHwnd  g_pfnCreateSwapChainForHwnd  = nullptr;

static bool g_bIsWindowedStereoHooked     = false;
static bool g_bCreateSCForHwndHooked      = false;

// MinHook trampolines for CreateDXGIFactory*
typedef HRESULT (WINAPI* PFN_CreateFactory)(REFIID, void**);
typedef HRESULT (WINAPI* PFN_CreateFactory2)(UINT, REFIID, void**);

static PFN_CreateFactory  g_pfnOrigCreateFactory  = nullptr;
static PFN_CreateFactory  g_pfnOrigCreateFactory1 = nullptr;
static PFN_CreateFactory2 g_pfnOrigCreateFactory2 = nullptr;

// ---- Hook implementations -------------------------------------------------

static HRESULT WINAPI HookedGetDesc(IDXGIAdapter* pAdapter, DXGI_ADAPTER_DESC* pDesc)
{
    HRESULT hr = g_pfnGetDesc(pAdapter, pDesc);
    if (SUCCEEDED(hr) && pDesc && pDesc->VendorId != 0x1002)
    {
        char buf[80];
        wsprintfA(buf, "[D3d11Proxy] GetDesc: VendorId 0x%04X->0x1002\n", pDesc->VendorId);
        WriteLog(buf);
        pDesc->VendorId = 0x1002;
        wcscpy_s(pDesc->Description, 128, L"AMD Radeon RX 580 Series");
    }
    return hr;
}

static HRESULT WINAPI HookedGetDesc1(IDXGIAdapter1* pAdapter, DXGI_ADAPTER_DESC1* pDesc)
{
    HRESULT hr = g_pfnGetDesc1(pAdapter, pDesc);
    if (SUCCEEDED(hr) && pDesc && pDesc->VendorId != 0x1002)
    {
        char buf[80];
        wsprintfA(buf, "[D3d11Proxy] GetDesc1: VendorId 0x%04X->0x1002\n", pDesc->VendorId);
        WriteLog(buf);
        pDesc->VendorId = 0x1002;
        wcscpy_s(pDesc->Description, 128, L"AMD Radeon RX 580 Series");
    }
    return hr;
}

static void HookAdapter(IDXGIAdapter* pA);

static HRESULT WINAPI HookedEnumAdapters(IDXGIFactory* pFactory, UINT Adapter, IDXGIAdapter** ppAdapter)
{
    HRESULT hr = g_pfnEnumAdapters(pFactory, Adapter, ppAdapter);
    if (SUCCEEDED(hr) && ppAdapter && *ppAdapter)
        HookAdapter(*ppAdapter);
    return hr;
}

static HRESULT WINAPI HookedEnumAdapters1(IDXGIFactory1* pFactory, UINT Adapter, IDXGIAdapter1** ppAdapter)
{
    HRESULT hr = g_pfnEnumAdapters1(pFactory, Adapter, ppAdapter);
    if (SUCCEEDED(hr) && ppAdapter && *ppAdapter)
        HookAdapter(static_cast<IDXGIAdapter*>(*ppAdapter));
    return hr;
}

// ---- IDXGIFactory2 hook implementations -----------------------------------

static BOOL WINAPI HookedIsWindowedStereoEnabled(IDXGIFactory2* pFactory)
{
    BOOL real = g_pfnIsWindowedStereoEnabled(pFactory);
    char buf[96];
#ifdef BLOCK_NVAPI_STEREO
    wsprintfA(buf, "[D3d11Proxy] IsWindowedStereoEnabled: real=%d -> returning TRUE\n", real);
    WriteLog(buf);
    return TRUE;
#else
    wsprintfA(buf, "[D3d11Proxy] IsWindowedStereoEnabled: real=%d -> pass-through\n", real);
    WriteLog(buf);
    return real;
#endif
}

static HRESULT WINAPI HookedCreateSwapChainForHwnd(
    IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    char buf[256];
    if (pDesc)
    {
        wsprintfA(buf, "[D3d11Proxy] CreateSwapChainForHwnd: %ux%u fmt=%u stereo=%d bufCnt=%u hwnd=%p\n",
                  pDesc->Width, pDesc->Height, pDesc->Format, pDesc->Stereo,
                  pDesc->BufferCount, hWnd);
    }
    else
    {
        wsprintfA(buf, "[D3d11Proxy] CreateSwapChainForHwnd: pDesc=NULL hwnd=%p\n", hWnd);
    }
    WriteLog(buf);

    HRESULT hr = g_pfnCreateSwapChainForHwnd(pFactory, pDevice, hWnd, pDesc,
                                              pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    wsprintfA(buf, "[D3d11Proxy] CreateSwapChainForHwnd -> 0x%08X\n", hr);
    WriteLog(buf);
    return hr;
}

// vtable layout:
//   IDXGIAdapter:  ...GetDesc[8]...
//   IDXGIAdapter1: ...GetDesc1[10]...
//   IDXGIFactory:  ...EnumAdapters[7]...
//   IDXGIFactory1: ...EnumAdapters1[12]...
//   IDXGIFactory2: ...IsWindowedStereoEnabled[14]...CreateSwapChainForHwnd[15]...

static void HookAdapter(IDXGIAdapter* pA)
{
    void** vt = *reinterpret_cast<void***>(pA);

    if (!g_bGetDescHooked)
    {
        if (MH_CreateHook(vt[8], &HookedGetDesc,
                reinterpret_cast<void**>(&g_pfnGetDesc)) == MH_OK)
        {
            MH_EnableHook(vt[8]);
            g_bGetDescHooked = true;
            WriteLog("[D3d11Proxy] IDXGIAdapter::GetDesc hooked\n");
        }
    }

    IDXGIAdapter1* pA1 = nullptr;
    if (!g_bGetDesc1Hooked &&
        SUCCEEDED(pA->QueryInterface(__uuidof(IDXGIAdapter1),
                reinterpret_cast<void**>(&pA1))))
    {
        void** vt1 = *reinterpret_cast<void***>(pA1);
        if (MH_CreateHook(vt1[10], &HookedGetDesc1,
                reinterpret_cast<void**>(&g_pfnGetDesc1)) == MH_OK)
        {
            MH_EnableHook(vt1[10]);
            g_bGetDesc1Hooked = true;
            WriteLog("[D3d11Proxy] IDXGIAdapter1::GetDesc1 hooked\n");
        }
        pA1->Release();
    }
}

static void HookFactory(void* pRaw)
{
    if (!pRaw) return;

    IDXGIFactory* pF = nullptr;
    if (FAILED(static_cast<IUnknown*>(pRaw)->QueryInterface(
            __uuidof(IDXGIFactory), reinterpret_cast<void**>(&pF))))
    {
        WriteLog("[D3d11Proxy] HookFactory: QI(IDXGIFactory) failed\n");
        return;
    }
    WriteLog("[D3d11Proxy] HookFactory: installing hooks\n");

    IDXGIAdapter* pA0 = nullptr;
    if (SUCCEEDED(pF->EnumAdapters(0, &pA0)))
    {
        HookAdapter(pA0);
        pA0->Release();
    }

    void** vt = *reinterpret_cast<void***>(pF);
    if (!g_bEnumAdaptersHooked)
    {
        if (MH_CreateHook(vt[7], &HookedEnumAdapters,
                reinterpret_cast<void**>(&g_pfnEnumAdapters)) == MH_OK)
        {
            MH_EnableHook(vt[7]);
            g_bEnumAdaptersHooked = true;
            WriteLog("[D3d11Proxy] IDXGIFactory::EnumAdapters hooked\n");
        }
    }

    IDXGIFactory1* pF1 = nullptr;
    if (!g_bEnumAdapters1Hooked &&
        SUCCEEDED(pF->QueryInterface(__uuidof(IDXGIFactory1),
                reinterpret_cast<void**>(&pF1))))
    {
        void** vt1 = *reinterpret_cast<void***>(pF1);
        if (MH_CreateHook(vt1[12], &HookedEnumAdapters1,
                reinterpret_cast<void**>(&g_pfnEnumAdapters1)) == MH_OK)
        {
            MH_EnableHook(vt1[12]);
            g_bEnumAdapters1Hooked = true;
            WriteLog("[D3d11Proxy] IDXGIFactory1::EnumAdapters1 hooked\n");
        }
        pF1->Release();
    }

    // IDXGIFactory2 hooks — DXGI 1.2 stereo path (Tomb Raider 2013 etc.)
    IDXGIFactory2* pF2 = nullptr;
    if (SUCCEEDED(pF->QueryInterface(__uuidof(IDXGIFactory2),
            reinterpret_cast<void**>(&pF2))))
    {
        void** vt2 = *reinterpret_cast<void***>(pF2);
        if (!g_bIsWindowedStereoHooked)
        {
            if (MH_CreateHook(vt2[14], &HookedIsWindowedStereoEnabled,
                    reinterpret_cast<void**>(&g_pfnIsWindowedStereoEnabled)) == MH_OK)
            {
                MH_EnableHook(vt2[14]);
                g_bIsWindowedStereoHooked = true;
                WriteLog("[D3d11Proxy] IDXGIFactory2::IsWindowedStereoEnabled hooked\n");
            }
        }
        if (!g_bCreateSCForHwndHooked)
        {
            if (MH_CreateHook(vt2[15], &HookedCreateSwapChainForHwnd,
                    reinterpret_cast<void**>(&g_pfnCreateSwapChainForHwnd)) == MH_OK)
            {
                MH_EnableHook(vt2[15]);
                g_bCreateSCForHwndHooked = true;
                WriteLog("[D3d11Proxy] IDXGIFactory2::CreateSwapChainForHwnd hooked\n");
            }
        }
        pF2->Release();
    }

    pF->Release();
}

// ---- Hooked CreateDXGIFactory* (called via MinHook when game hits them) ---

static HRESULT WINAPI HookedCreateDXGIFactory(REFIID riid, void** ppFactory)
{
    // DXHR asks for the original IDXGIFactory interface. Create it through the
    // DXGI 1.1 entry point so its devices support keyed GPU sharing; preserve IID.
    WriteLog("[D3d11Proxy] CreateDXGIFactory -> DXGI 1.1 factory for native pair sharing\n");
    HRESULT hr = g_pfnOrigCreateFactory1 ? g_pfnOrigCreateFactory1(riid, ppFactory)
                                       : g_pfnOrigCreateFactory(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        HookFactory(*ppFactory);
    return hr;
}

static HRESULT WINAPI HookedCreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    WriteLog("[D3d11Proxy] CreateDXGIFactory1\n");
    HRESULT hr = g_pfnOrigCreateFactory1(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        HookFactory(*ppFactory);
    return hr;
}

static HRESULT WINAPI HookedCreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory)
{
    WriteLog("[D3d11Proxy] CreateDXGIFactory2\n");
    HRESULT hr = g_pfnOrigCreateFactory2(Flags, riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        HookFactory(*ppFactory);
    return hr;
}

// ---- Patch CreateDXGIFactory* bytes in already-loaded system dxgi.dll -----

static void InstallDxgiHooks()
{
    if (!g_hRealDXGI) return;

    void* pf1  = GetProcAddress(g_hRealDXGI, "CreateDXGIFactory");
    void* pf1a = GetProcAddress(g_hRealDXGI, "CreateDXGIFactory1");
    void* pf2  = GetProcAddress(g_hRealDXGI, "CreateDXGIFactory2");

    if (pf1)  MH_CreateHook(pf1,  &HookedCreateDXGIFactory,
                    reinterpret_cast<void**>(&g_pfnOrigCreateFactory));
    if (pf1a) MH_CreateHook(pf1a, &HookedCreateDXGIFactory1,
                    reinterpret_cast<void**>(&g_pfnOrigCreateFactory1));
    if (pf2)  MH_CreateHook(pf2,  &HookedCreateDXGIFactory2,
                    reinterpret_cast<void**>(&g_pfnOrigCreateFactory2));

    char buf[128];
    wsprintfA(buf, "[D3d11Proxy] hook ptrs: f=%p f1=%p f2=%p\n", pf1, pf1a, pf2);
    WriteLog(buf);

    MH_STATUS s = MH_EnableHook(MH_ALL_HOOKS);
    WriteLog(s == MH_OK
        ? "[D3d11Proxy] CreateDXGIFactory hooks installed OK\n"
        : "[D3d11Proxy] CreateDXGIFactory hooks FAILED\n");
}

// GetProcAddress trampolines — declared here (before ProxiedAmdDxExtCreate11) so
// the proxy can call through the trampoline instead of the hooked GetProcAddress,
// avoiding hook re-entry when resolving AmdQbProxy exports.
typedef FARPROC (WINAPI* PFN_GetProcAddr)(HMODULE, LPCSTR);
static PFN_GetProcAddr g_pfnGetProcAddr   = nullptr;  // kernel32 trampoline
static PFN_GetProcAddr g_pfnGetProcAddrKB = nullptr;  // KernelBase trampoline

// Tracked real atidxx module handle — set by GetModuleHandleA hook for diagnostics
static HMODULE g_hAtidxx = nullptr;

// ---- AMD extension IAT intercept -----------------------------------------
// TR2013 and similar games have atidxx32.dll as a static import.  After the
// VendorId spoof they call AmdDxExtCreate11 from system32\atidxx32.dll (the
// real AMD UMD), which crashes when handed a NVIDIA D3D11 device.
// We walk the game exe's IAT at startup and replace the AmdDxExtCreate11
// function pointer with our proxy.  The proxy lazy-loads our game-dir
// atidxx32/64.dll (AmdQbProxy) outside of DllMain so there is no loader-lock
// risk, and delegates to our working implementation.

typedef HRESULT (__cdecl* PFN_AmdDxExtCreate11)(void*, void**);
static HMODULE              g_hAmdProxy           = nullptr;
static PFN_AmdDxExtCreate11 g_pfnProxyExtCreate11  = nullptr;

// Forward-declared LoadLibraryExW trampoline — set by InstallLoadLibraryHooks().
// Used in LoadAmdProxy() to call the real LoadLibraryExW and avoid our hook
// re-entering itself (the local atidxx path contains "atidxx").
typedef HMODULE (WINAPI* PFN_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD);
static PFN_LoadLibraryExW g_pfnLoadLibraryExW = nullptr;

static HMODULE LoadAmdProxy()
{
    if (g_hAmdProxy) return g_hAmdProxy;
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hSelf, path, MAX_PATH);
    wchar_t* sep = wcsrchr(path, L'\\');
    if (!sep) return nullptr;
    size_t rem = MAX_PATH - static_cast<size_t>(sep - path) - 1;
#ifdef _WIN64
    wcscpy_s(sep + 1, rem, L"atidxx64.dll");
#else
    wcscpy_s(sep + 1, rem, L"atidxx32.dll");
#endif
    // Full-path load treats our game-dir DLL as a separate module from
    // system32\atidxx32.dll even when the system one is already mapped.
    // Use the real (unhooked) LoadLibraryExW to avoid re-entering our
    // LoadLibrary hooks (the path contains "atidxx" which would trigger
    // an infinite redirect loop).
    if (g_pfnLoadLibraryExW)
        g_hAmdProxy = g_pfnLoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    else
        g_hAmdProxy = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (g_hAmdProxy)
    {
        // Pin the module so the game's FreeLibrary() never unloads it.
        // Without this, g_hAmdProxy becomes a stale handle and subsequent
        // GetProcAddress calls return NULL.
        HMODULE hPin = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(g_hAmdProxy), &hPin);
    }
    WriteLog(g_hAmdProxy
        ? "[D3d11Proxy] AmdQbProxy loaded OK\n"
        : "[D3d11Proxy] AmdQbProxy load FAILED\n");
    return g_hAmdProxy;
}

// ---- AMD ADL proxy loader --------------------------------------------------
// Codemasters EGO engine games (DiRT Showdown, GRID 2, F1 series) load
// atiadlxy.dll (x86) / atiadlxx.dll (x64) to check AMD stereo capabilities.
// On NVIDIA systems those DLLs don't exist, so the games give up on HD3D.
// Redirect those loads to our local AmdAdlProxy.

static HMODULE g_hAmdAdl = nullptr;

static HMODULE LoadAmdAdlProxy()
{
    if (g_hAmdAdl) return g_hAmdAdl;
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hSelf, path, MAX_PATH);
    wchar_t* sep = wcsrchr(path, L'\\');
    if (!sep) return nullptr;
    size_t rem = MAX_PATH - static_cast<size_t>(sep - path) - 1;
#ifdef _WIN64
    wcscpy_s(sep + 1, rem, L"atiadlxx.dll");
#else
    wcscpy_s(sep + 1, rem, L"atiadlxy.dll");
#endif
    if (g_pfnLoadLibraryExW)
        g_hAmdAdl = g_pfnLoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    else
        g_hAmdAdl = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (g_hAmdAdl)
    {
        // Pin the module so the game's FreeLibrary() never unloads it.
        // EGO engine games (DiRT Showdown, GRID 2) call FreeLibrary after
        // the first ADL init cycle, which unmaps the module while g_hAmdAdl
        // still caches the handle.  The second LoadLibrary then returns the
        // stale handle and every GetProcAddress returns NULL.
        HMODULE hPin = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(g_hAmdAdl), &hPin);
    }
    WriteLog(g_hAmdAdl
        ? "[D3d11Proxy] AmdAdlProxy loaded OK\n"
        : "[D3d11Proxy] AmdAdlProxy load FAILED\n");
    return g_hAmdAdl;
}

static HRESULT __cdecl ProxiedAmdDxExtCreate11(void* pDevice, void** ppExt)
{
    WriteLog("[D3d11Proxy] AmdDxExtCreate11 -> proxy\n");
    if (!g_pfnProxyExtCreate11)
    {
        HMODULE h = LoadAmdProxy();
        if (h)
        {
            PFN_GetProcAddr resolveProc = g_pfnGetProcAddrKB ? g_pfnGetProcAddrKB : g_pfnGetProcAddr;
            g_pfnProxyExtCreate11 =
                reinterpret_cast<PFN_AmdDxExtCreate11>(resolveProc(h, "AmdDxExtCreate11"));
        }
    }
    if (!g_pfnProxyExtCreate11)
    {
        WriteLog("[D3d11Proxy] AmdDxExtCreate11: proxy unavailable -> E_NOTIMPL\n");
        if (ppExt) *ppExt = nullptr;
        return E_NOTIMPL;
    }
    return g_pfnProxyExtCreate11(pDevice, ppExt);
}

static void PatchAmdIAT()
{
    HMODULE hExe = GetModuleHandleW(nullptr);
    if (!hExe) return;

    BYTE* base = reinterpret_cast<BYTE*>(hExe);
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt  = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) { WriteLog("[D3d11Proxy] PatchAmdIAT: no import dir\n"); return; }

    auto* imp = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + dir.VirtualAddress);
    bool patched = false;

    for (; imp->Name; ++imp)
    {
        const char* dllName = reinterpret_cast<const char*>(base + imp->Name);
        char lc[MAX_PATH] = {};
        for (int i = 0; dllName[i] && i < MAX_PATH - 1; ++i)
            lc[i] = (dllName[i] >= 'A' && dllName[i] <= 'Z') ? dllName[i] + 32 : dllName[i];
        if (!strstr(lc, "atidxx")) continue;

        char mbuf[128];
        wsprintfA(mbuf, "[D3d11Proxy] PatchAmdIAT: found %s\n", dllName);
        WriteLog(mbuf);

        if (!imp->OriginalFirstThunk) continue;
        auto* orig  = reinterpret_cast<PIMAGE_THUNK_DATA>(base + imp->OriginalFirstThunk);
        auto* thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + imp->FirstThunk);

        for (; orig->u1.Function; ++orig, ++thunk)
        {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto* ibn = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
                base + orig->u1.AddressOfData);
            if (_stricmp(reinterpret_cast<const char*>(ibn->Name), "AmdDxExtCreate11") != 0)
                continue;

            WriteLog("[D3d11Proxy] PatchAmdIAT: patching AmdDxExtCreate11\n");
            DWORD old;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            thunk->u1.Function = reinterpret_cast<ULONG_PTR>(&ProxiedAmdDxExtCreate11);
            VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
            patched = true;
        }
    }

    if (!patched)
        WriteLog("[D3d11Proxy] PatchAmdIAT: AmdDxExtCreate11 not in static imports\n");
}

// ---- GetProcAddress hook --------------------------------------------------
// All tested games use GetModuleHandle("atidxx32.dll") + GetProcAddress at
// runtime to locate AmdDxExtCreate11.  Hook kernel32!GetProcAddress and
// redirect any lookup for that name to our proxy implementation.

static FARPROC WINAPI HookedGetProcAddr(HMODULE hMod, LPCSTR name)
{
    if (name && !IS_INTRESOURCE(name))
    {
        if (_stricmp(name, "AmdDxExtCreate11") == 0)
        {
            WriteLog("[D3d11Proxy] GetProcAddress(AmdDxExtCreate11) intercepted\n");
            return reinterpret_cast<FARPROC>(&ProxiedAmdDxExtCreate11);
        }
        if (_strnicmp(name, "Amd", 3) == 0)
        {
            char logbuf[128];
            wsprintfA(logbuf, "[D3d11Proxy] GetProcAddress(%s) - unhandled AMD name\n", name);
            WriteLog(logbuf);
        }
        if (_strnicmp(name, "ADL_", 4) == 0 || _strnicmp(name, "ADL2_", 5) == 0)
        {
            PFN_GetProcAddr pfnR = g_pfnGetProcAddrKB ? g_pfnGetProcAddrKB : g_pfnGetProcAddr;
            FARPROC result = pfnR(hMod, name);
            char logbuf[256];
            wsprintfA(logbuf, "[D3d11Proxy] GetProcAddress(%s) hMod=%p result=%p (adl=%p)\n",
                      name, hMod, result, g_hAmdAdl);
            WriteLog(logbuf);
            return result;
        }
        // Log all lookups on the real atidxx32/64 module (TR2013 diagnosis)
        if (g_hAtidxx && hMod == g_hAtidxx)
        {
            char logbuf[128];
            wsprintfA(logbuf, "[D3d11Proxy] GetProcAddress(atidxx, %s)\n", name);
            WriteLog(logbuf);
        }
    }
    else if (IS_INTRESOURCE(name) && g_hAtidxx && hMod == g_hAtidxx)
    {
        char logbuf[64];
        wsprintfA(logbuf, "[D3d11Proxy] GetProcAddress(atidxx, ordinal#%u)\n",
                   static_cast<UINT>(reinterpret_cast<ULONG_PTR>(name)));
        WriteLog(logbuf);
    }
    PFN_GetProcAddr pfnReal = g_pfnGetProcAddrKB ? g_pfnGetProcAddrKB : g_pfnGetProcAddr;
    return pfnReal(hMod, name);
}

// GetModuleHandleA hook — tracks when games obtain atidxx32/64 module handle
typedef HMODULE (WINAPI* PFN_GetModuleHandleA)(LPCSTR);
static PFN_GetModuleHandleA g_pfnGetModuleHandleA = nullptr;

static HMODULE WINAPI HookedGetModuleHandleA(LPCSTR name)
{
    HMODULE h = g_pfnGetModuleHandleA(name);
    if (name)
    {
        char lc[MAX_PATH] = {};
        for (int i = 0; name[i] && i < MAX_PATH - 1; ++i)
            lc[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i];
        if (strstr(lc, "atidxx"))
        {
            if (h)
            {
                g_hAtidxx = h;
                char buf[128];
                wsprintfA(buf, "[D3d11Proxy] GetModuleHandleA(%s) = %p\n", name, h);
                WriteLog(buf);
            }
            else
            {
                // Module not loaded — load our AmdQbProxy so games that use
                // GetModuleHandle + GetProcAddress (e.g. Sleeping Dogs) find
                // AmdDxExtCreate11 without an explicit LoadLibrary call.
                char buf[256];
                wsprintfA(buf, "[D3d11Proxy] GetModuleHandleA(%s) = NULL -> loading AmdQbProxy\n", name);
                WriteLog(buf);
                h = LoadAmdProxy();
                if (h) g_hAtidxx = h;
            }
        }
    }
    return h;
}

// ---- LoadLibrary hooks ---------------------------------------------------
// Some HD3D games (DiRT Showdown, Sleeping Dogs) resolve the AMD UMD via
// LoadLibrary with a full path read from the AMD driver registry keys.
// On NVIDIA systems those registry keys don't exist, so the LoadLibrary
// call either fails or loads the wrong DLL.  We intercept LoadLibrary*
// and redirect any request whose filename contains "atidxx" to our local
// atidxx32/64.dll (AmdQbProxy).

typedef HMODULE (WINAPI* PFN_LoadLibraryA)(LPCSTR);
typedef HMODULE (WINAPI* PFN_LoadLibraryW)(LPCWSTR);
typedef HMODULE (WINAPI* PFN_LoadLibraryExA)(LPCSTR, HANDLE, DWORD);
// PFN_LoadLibraryExW typedef + g_pfnLoadLibraryExW declared above LoadAmdProxy()

static PFN_LoadLibraryA   g_pfnLoadLibraryA   = nullptr;
static PFN_LoadLibraryW   g_pfnLoadLibraryW   = nullptr;
static PFN_LoadLibraryExA g_pfnLoadLibraryExA = nullptr;

// Helper: check if an ASCII string contains "atidxx" (case-insensitive)
static bool ContainsAtidxxA(LPCSTR s)
{
    if (!s) return false;
    for (const char* p = s; *p; ++p)
    {
        if ((p[0] | 0x20) == 'a' && (p[1] | 0x20) == 't' &&
            (p[2] | 0x20) == 'i' && (p[3] | 0x20) == 'd' &&
            (p[4] | 0x20) == 'x' && (p[5] | 0x20) == 'x')
            return true;
    }
    return false;
}

// Helper: check if a wide string contains "atidxx" (case-insensitive)
static bool ContainsAtidxxW(LPCWSTR s)
{
    if (!s) return false;
    for (const wchar_t* p = s; *p; ++p)
    {
        if ((p[0] | 0x20) == L'a' && (p[1] | 0x20) == L't' &&
            (p[2] | 0x20) == L'i' && (p[3] | 0x20) == L'd' &&
            (p[4] | 0x20) == L'x' && (p[5] | 0x20) == L'x')
            return true;
    }
    return false;
}

// Helper: check if an ASCII string contains "atiadl" (case-insensitive)
static bool ContainsAtiadlA(LPCSTR s)
{
    if (!s) return false;
    for (const char* p = s; *p; ++p)
    {
        if ((p[0] | 0x20) == 'a' && (p[1] | 0x20) == 't' &&
            (p[2] | 0x20) == 'i' && (p[3] | 0x20) == 'a' &&
            (p[4] | 0x20) == 'd' && (p[5] | 0x20) == 'l')
            return true;
    }
    return false;
}

// Helper: check if a wide string contains "atiadl" (case-insensitive)
static bool ContainsAtiadlW(LPCWSTR s)
{
    if (!s) return false;
    for (const wchar_t* p = s; *p; ++p)
    {
        if ((p[0] | 0x20) == L'a' && (p[1] | 0x20) == L't' &&
            (p[2] | 0x20) == L'i' && (p[3] | 0x20) == L'a' &&
            (p[4] | 0x20) == L'd' && (p[5] | 0x20) == L'l')
            return true;
    }
    return false;
}

#ifdef BLOCK_NVAPI_STEREO
// Helper: check if a narrow string contains "Stereo3D" (case-insensitive)
// Used to block NvAPI stereo registry access so games fall through to AMD HD3D.
// Without this, games like Sleeping Dogs see a working NvAPI stereo and never
// attempt AMD HD3D.  Tomb Raider crashes when NvAPI stereo initialises after
// the VendorId has been spoofed to AMD.
static bool ContainsNvStereo3DA(LPCSTR s)
{
    if (!s) return false;
    for (const char* p = s; *p; ++p)
    {
        if ((p[0] | 0x20) == 's' && (p[1] | 0x20) == 't' &&
            (p[2] | 0x20) == 'e' && (p[3] | 0x20) == 'r' &&
            (p[4] | 0x20) == 'e' && (p[5] | 0x20) == 'o' &&
            p[6] == '3' && (p[7] | 0x20) == 'd')
            return true;
    }
    return false;
}

static bool ContainsNvStereo3DW(LPCWSTR s)
{
    if (!s) return false;
    for (const wchar_t* p = s; *p; ++p)
    {
        if ((p[0] | 0x20) == L's' && (p[1] | 0x20) == L't' &&
            (p[2] | 0x20) == L'e' && (p[3] | 0x20) == L'r' &&
            (p[4] | 0x20) == L'e' && (p[5] | 0x20) == L'o' &&
            p[6] == L'3' && (p[7] | 0x20) == L'd')
            return true;
    }
    return false;
}
#endif // BLOCK_NVAPI_STEREO

#ifdef BLOCK_NVAPI_STEREO
// Helper: check if a narrow path's basename is "nvapi.dll" or "nvapi64.dll"
// Block NvAPI entirely so the spoofed AMD VendorId doesn't conflict with
// NVIDIA driver internals (fixes Tomb Raider crash).
static bool IsNvapiDllA(LPCSTR s)
{
    if (!s) return false;
    const char* base = s;
    for (const char* p = s; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    if ((base[0]|0x20)=='n' && (base[1]|0x20)=='v' && (base[2]|0x20)=='a' &&
        (base[3]|0x20)=='p' && (base[4]|0x20)=='i')
    {
        if ((base[5]|0x20)=='.' || (base[5]=='6' && base[6]=='4' && (base[7]|0x20)=='.'))
            return true;
    }
    return false;
}

static bool IsNvapiDllW(LPCWSTR s)
{
    if (!s) return false;
    const wchar_t* base = s;
    for (const wchar_t* p = s; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    if ((base[0]|0x20)==L'n' && (base[1]|0x20)==L'v' && (base[2]|0x20)==L'a' &&
        (base[3]|0x20)==L'p' && (base[4]|0x20)==L'i')
    {
        if ((base[5]|0x20)==L'.' || (base[5]==L'6' && base[6]==L'4' && (base[7]|0x20)==L'.'))
            return true;
    }
    return false;
}

// On a real AMD system, nvapi.dll does not exist — LoadLibrary returns NULL
// and SetLastError(ERROR_MOD_NOT_FOUND).  Games handle this by skipping
// NvAPI entirely and proceeding to the AMD HD3D path.  We mimic this
// behaviour exactly.  The nvapi_QueryInterface export and NvApiStub below
// are kept as a safety net but should never be reached.

// Universal NvAPI stub function.
// All NvAPI functions are __cdecl and return NvAPI_Status (int).
// Returning NVAPI_ERROR (-1) makes every caller treat NvAPI as unavailable.
// Because __cdecl is caller-cleanup, a zero-parameter stub is safe regardless
// of how many arguments the real signature expects — the caller pops its own
// stack frame after we return.
static int __cdecl NvApiStub()
{
    WriteLog("[D3d11Proxy] NvApiStub called -> NVAPI_ERROR\n");
    return -1; // NVAPI_ERROR
}

// Exported nvapi_QueryInterface stub — safety net only.
// LoadLibrary("nvapi.dll") now returns NULL to mimic a real AMD system.
// This export is kept as a fallback in case any game obtains our HMODULE
// through a path we don't intercept (e.g. GetModuleHandle after injection).
extern "C" __declspec(dllexport) void* __cdecl nvapi_QueryInterface(unsigned int id)
{
    char buf[128];
    wsprintfA(buf, "[D3d11Proxy] nvapi_QueryInterface(0x%08X) -> stub fn\n", id);
    WriteLog(buf);
    return reinterpret_cast<void*>(&NvApiStub);
}
#endif // BLOCK_NVAPI_STEREO
// Only checks the basename (after last \ or /) to avoid false positives from
// directory names like "application", "Corporation", "compatibility", etc.
static bool IsAmdRelatedA(LPCSTR s)
{
    if (!s) return false;
    // Find basename
    const char* base = s;
    for (const char* p = s; *p; ++p)
    {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    // Check if basename starts with "ati" or "amd"
    if ((base[0] | 0x20) == 'a')
    {
        if (((base[1] | 0x20) == 't' && (base[2] | 0x20) == 'i') ||
            ((base[1] | 0x20) == 'm' && (base[2] | 0x20) == 'd'))
            return true;
    }
    return false;
}

static bool IsAmdRelatedW(LPCWSTR s)
{
    if (!s) return false;
    // Find basename
    const wchar_t* base = s;
    for (const wchar_t* p = s; *p; ++p)
    {
        if (*p == L'\\' || *p == L'/') base = p + 1;
    }
    // Check if basename starts with "ati" or "amd"
    if ((base[0] | 0x20) == L'a')
    {
        if (((base[1] | 0x20) == L't' && (base[2] | 0x20) == L'i') ||
            ((base[1] | 0x20) == L'm' && (base[2] | 0x20) == L'd'))
            return true;
    }
    return false;
}

static HMODULE WINAPI HookedLoadLibraryA(LPCSTR lpFileName)
{
#ifdef BLOCK_NVAPI_STEREO
    if (IsNvapiDllA(lpFileName))
    {
        char buf[300];
        wsprintfA(buf, "[D3d11Proxy] LoadLibraryA(%s) -> NULL (nvapi blocked)\n", lpFileName);
        WriteLog(buf);
        SetLastError(ERROR_MOD_NOT_FOUND);
        return NULL;
    }
#endif
    if (ContainsAtidxxA(lpFileName))
    {
        char buf[300];
        wsprintfA(buf, "[D3d11Proxy] LoadLibraryA(%s) -> redirecting to local proxy\n", lpFileName);
        WriteLog(buf);
        HMODULE h = LoadAmdProxy();
        if (h) return h;
        WriteLog("[D3d11Proxy] LoadLibraryA redirect failed, falling through\n");
    }
    else if (ContainsAtiadlA(lpFileName))
    {
        char buf[300];
        wsprintfA(buf, "[D3d11Proxy] LoadLibraryA(%s) -> redirecting to ADL proxy\n", lpFileName);
        WriteLog(buf);
        HMODULE h = LoadAmdAdlProxy();
        if (h) return h;
        WriteLog("[D3d11Proxy] LoadLibraryA ADL redirect failed, falling through\n");
    }
    else if (IsAmdRelatedA(lpFileName))
    {
        char buf[400];
        wsprintfA(buf, "[D3d11Proxy] LoadLibraryA(%s) [AMD-related, not redirected]\n", lpFileName);
        WriteLog(buf);
    }
    return g_pfnLoadLibraryA(lpFileName);
}

static HMODULE WINAPI HookedLoadLibraryW(LPCWSTR lpFileName)
{
#ifdef BLOCK_NVAPI_STEREO
    if (IsNvapiDllW(lpFileName))
    {
        char narrow[300] = {};
        WideCharToMultiByte(CP_ACP, 0, lpFileName, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        char buf[400];
        wsprintfA(buf, "[D3d11Proxy] LoadLibraryW(%s) -> NULL (nvapi blocked)\n", narrow);
        WriteLog(buf);
        SetLastError(ERROR_MOD_NOT_FOUND);
        return NULL;
    }
#endif
    if (ContainsAtidxxW(lpFileName))
    {
        char narrow[300] = {};
        WideCharToMultiByte(CP_ACP, 0, lpFileName, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        char buf[400];
        wsprintfA(buf, "[D3d11Proxy] LoadLibraryW(%s) -> redirecting to local proxy\n", narrow);
        WriteLog(buf);
        HMODULE h = LoadAmdProxy();
        if (h) return h;
        WriteLog("[D3d11Proxy] LoadLibraryW redirect failed, falling through\n");
    }
    else if (ContainsAtiadlW(lpFileName))
    {
        char narrow[300] = {};
        WideCharToMultiByte(CP_ACP, 0, lpFileName, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        char buf[400];
        wsprintfA(buf, "[D3d11Proxy] LoadLibraryW(%s) -> redirecting to ADL proxy\n", narrow);
        WriteLog(buf);
        HMODULE h = LoadAmdAdlProxy();
        if (h) return h;
        WriteLog("[D3d11Proxy] LoadLibraryW ADL redirect failed, falling through\n");
    }
    else if (IsAmdRelatedW(lpFileName))
    {
        char narrow[300] = {};
        WideCharToMultiByte(CP_ACP, 0, lpFileName, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        char buf[400];
        wsprintfA(buf, "[D3d11Proxy] LoadLibraryW(%s) [AMD-related, not redirected]\n", narrow);
        WriteLog(buf);
    }
    return g_pfnLoadLibraryW(lpFileName);
}

static HMODULE WINAPI HookedLoadLibraryExA(LPCSTR lpFileName, HANDLE hFile, DWORD dwFlags)
{
#ifdef BLOCK_NVAPI_STEREO
    if (IsNvapiDllA(lpFileName))
    {
        char buf[300];
        wsprintfA(buf, "[D3d11Proxy] LoadLibraryExA(%s) -> NULL (nvapi blocked)\n", lpFileName);
        WriteLog(buf);
        SetLastError(ERROR_MOD_NOT_FOUND);
        return NULL;
    }
#endif
    if (ContainsAtidxxA(lpFileName))
    {
        char buf[300];
        wsprintfA(buf, "[D3d11Proxy] LoadLibraryExA(%s) -> redirecting to local proxy\n", lpFileName);
        WriteLog(buf);
        HMODULE h = LoadAmdProxy();
        if (h) return h;
        WriteLog("[D3d11Proxy] LoadLibraryExA redirect failed, falling through\n");
    }
    else if (ContainsAtiadlA(lpFileName))
    {
        char buf[300];
        wsprintfA(buf, "[D3d11Proxy] LoadLibraryExA(%s) -> redirecting to ADL proxy\n", lpFileName);
        WriteLog(buf);
        HMODULE h = LoadAmdAdlProxy();
        if (h) return h;
        WriteLog("[D3d11Proxy] LoadLibraryExA ADL redirect failed, falling through\n");
    }
    else if (IsAmdRelatedA(lpFileName))
    {
        char buf[400];
        wsprintfA(buf, "[D3d11Proxy] LoadLibraryExA(%s) [AMD-related, not redirected]\n", lpFileName);
        WriteLog(buf);
    }
    return g_pfnLoadLibraryExA(lpFileName, hFile, dwFlags);
}

static HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR lpFileName, HANDLE hFile, DWORD dwFlags)
{
#ifdef BLOCK_NVAPI_STEREO
    if (IsNvapiDllW(lpFileName))
    {
        char narrow[300] = {};
        WideCharToMultiByte(CP_ACP, 0, lpFileName, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        char buf[400];
        wsprintfA(buf, "[D3d11Proxy] LoadLibraryExW(%s) -> NULL (nvapi blocked)\n", narrow);
        WriteLog(buf);
        SetLastError(ERROR_MOD_NOT_FOUND);
        return NULL;
    }
#endif
    if (ContainsAtidxxW(lpFileName))
    {
        char narrow[300] = {};
        WideCharToMultiByte(CP_ACP, 0, lpFileName, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        char buf[400];
        wsprintfA(buf, "[D3d11Proxy] LoadLibraryExW(%s) -> redirecting to local proxy\n", narrow);
        WriteLog(buf);
        HMODULE h = LoadAmdProxy();
        if (h) return h;
        WriteLog("[D3d11Proxy] LoadLibraryExW redirect failed, falling through\n");
    }
    else if (ContainsAtiadlW(lpFileName))
    {
        char narrow[300] = {};
        WideCharToMultiByte(CP_ACP, 0, lpFileName, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        char buf[400];
        wsprintfA(buf, "[D3d11Proxy] LoadLibraryExW(%s) -> redirecting to ADL proxy\n", narrow);
        WriteLog(buf);
        HMODULE h = LoadAmdAdlProxy();
        if (h) return h;
        WriteLog("[D3d11Proxy] LoadLibraryExW ADL redirect failed, falling through\n");
    }
    else if (IsAmdRelatedW(lpFileName))
    {
        char narrow[300] = {};
        WideCharToMultiByte(CP_ACP, 0, lpFileName, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        char buf[400];
        wsprintfA(buf, "[D3d11Proxy] LoadLibraryExW(%s) [AMD-related, not redirected]\n", narrow);
        WriteLog(buf);
    }
    return g_pfnLoadLibraryExW(lpFileName, hFile, dwFlags);
}

// ---- Registry monitoring hooks -----------------------------------------------
// EGO engine games (DiRT Showdown, GRID 2, F1) may check AMD driver registry
// keys before calling any ADL functions.  On NVIDIA systems those keys don't
// exist, causing the games to skip HD3D.  These hooks log AMD-related registry
// access so we can identify which keys need spoofing.

typedef LSTATUS (WINAPI* PFN_RegOpenKeyExA)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LSTATUS (WINAPI* PFN_RegOpenKeyExW)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
typedef LSTATUS (WINAPI* PFN_RegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LSTATUS (WINAPI* PFN_RegQueryValueExW)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);

static PFN_RegOpenKeyExA    g_pfnRegOpenKeyExA    = nullptr;
static PFN_RegOpenKeyExW    g_pfnRegOpenKeyExW    = nullptr;
static PFN_RegQueryValueExA g_pfnRegQueryValueExA = nullptr;
static PFN_RegQueryValueExW g_pfnRegQueryValueExW = nullptr;

typedef LSTATUS (WINAPI* PFN_RegGetValueA)(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD);
typedef LSTATUS (WINAPI* PFN_RegGetValueW)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);

static PFN_RegGetValueA g_pfnRegGetValueA = nullptr;
static PFN_RegGetValueW g_pfnRegGetValueW = nullptr;

// Track HKEYs opened from AMD/display-class paths so we can log their queries.
static HKEY  g_trackedKeys[64] = {};
static int   g_trackedKeyCount = 0;

static void TrackKey(HKEY k)
{
    if (g_trackedKeyCount < 64)
        g_trackedKeys[g_trackedKeyCount++] = k;
}

static bool IsTrackedKey(HKEY k)
{
    for (int i = 0; i < g_trackedKeyCount; ++i)
        if (g_trackedKeys[i] == k) return true;
    return false;
}

// Check if a narrow string contains AMD-related substrings
// Matches: "ati", "amd" (at word boundaries only), "radeon", display adapter class GUID "4d36e968"
// Word-boundary check avoids false positives from "Corporation", "Migration",
// "Elevation", "Compatibility", "Application", etc.
static bool IsAmdRegistryPathA(LPCSTR s)
{
    if (!s) return false;
    for (const char* p = s; *p; ++p)
    {
        char c0 = p[0] | 0x20;
        if (c0 == 'a')
        {
            // Only match at word boundary: start-of-string or preceded by non-alnum
            bool boundary = (p == s) ||
                !((p[-1] >= 'A' && p[-1] <= 'Z') ||
                  (p[-1] >= 'a' && p[-1] <= 'z') ||
                  (p[-1] >= '0' && p[-1] <= '9'));
            if (boundary)
            {
                char c1 = p[1] | 0x20;
                if ((c1 == 't' && (p[2] | 0x20) == 'i') ||
                    (c1 == 'm' && (p[2] | 0x20) == 'd'))
                    return true;
            }
        }
        if (c0 == 'r' && (p[1]|0x20)=='a' && (p[2]|0x20)=='d' &&
            (p[3]|0x20)=='e' && (p[4]|0x20)=='o' && (p[5]|0x20)=='n')
            return true;
        // Display adapter class GUID {4d36e968-e325-11ce-bfc1-08002be10318}
        if (p[0]=='4' && (p[1]|0x20)=='d' && p[2]=='3' && p[3]=='6' &&
            (p[4]|0x20)=='e' && p[5]=='9')
            return true;
    }
    return false;
}

static bool IsAmdRegistryPathW(LPCWSTR s)
{
    if (!s) return false;
    for (const wchar_t* p = s; *p; ++p)
    {
        wchar_t c0 = p[0] | 0x20;
        if (c0 == L'a')
        {
            bool boundary = (p == s) ||
                !((p[-1] >= L'A' && p[-1] <= L'Z') ||
                  (p[-1] >= L'a' && p[-1] <= L'z') ||
                  (p[-1] >= L'0' && p[-1] <= L'9'));
            if (boundary)
            {
                wchar_t c1 = p[1] | 0x20;
                if ((c1 == L't' && (p[2] | 0x20) == L'i') ||
                    (c1 == L'm' && (p[2] | 0x20) == L'd'))
                    return true;
            }
        }
        if (c0 == L'r' && (p[1]|0x20)==L'a' && (p[2]|0x20)==L'd' &&
            (p[3]|0x20)==L'e' && (p[4]|0x20)==L'o' && (p[5]|0x20)==L'n')
            return true;
        if (p[0]==L'4' && (p[1]|0x20)==L'd' && p[2]==L'3' && p[3]==L'6' &&
            (p[4]|0x20)==L'e' && p[5]==L'9')
            return true;
    }
    return false;
}

static LSTATUS WINAPI HookedRegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey,
    DWORD ulOptions, REGSAM samDesired, PHKEY phkResult)
{
    #ifdef BLOCK_NVAPI_STEREO
    // Block NvAPI Stereo3D registry access so games fall through to AMD HD3D.
    if (lpSubKey && ContainsNvStereo3DA(lpSubKey))
    {
        char buf[512];
        wsprintfA(buf, "[D3d11Proxy] RegOpenKeyExA(%s) -> BLOCKED (NvStereo3D)\n", lpSubKey);
        WriteLog(buf);
        return ERROR_FILE_NOT_FOUND;
    }
#endif

    LSTATUS r = g_pfnRegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult);
    bool tracked = IsTrackedKey(hKey);
    if (lpSubKey && (IsAmdRegistryPathA(lpSubKey) || tracked))
    {
        char buf[512];
        wsprintfA(buf, "[D3d11Proxy] RegOpenKeyExA(%s) -> %ld%s\n",
                  lpSubKey, r, tracked ? " [child of tracked]" : "");
        WriteLog(buf);
        if (r == ERROR_SUCCESS && phkResult)
            TrackKey(*phkResult);
    }
    return r;
}

static LSTATUS WINAPI HookedRegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey,
    DWORD ulOptions, REGSAM samDesired, PHKEY phkResult)
{
    #ifdef BLOCK_NVAPI_STEREO
    // Block NvAPI Stereo3D registry access so games fall through to AMD HD3D.
    if (lpSubKey && ContainsNvStereo3DW(lpSubKey))
    {
        char narrow[400] = {};
        WideCharToMultiByte(CP_ACP, 0, lpSubKey, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        char buf[512];
        wsprintfA(buf, "[D3d11Proxy] RegOpenKeyExW(%s) -> BLOCKED (NvStereo3D)\n", narrow);
        WriteLog(buf);
        return ERROR_FILE_NOT_FOUND;
    }
#endif

    LSTATUS r = g_pfnRegOpenKeyExW(hKey, lpSubKey, ulOptions, samDesired, phkResult);
    bool tracked = IsTrackedKey(hKey);
    if (lpSubKey && (IsAmdRegistryPathW(lpSubKey) || tracked))
    {
        char narrow[400] = {};
        WideCharToMultiByte(CP_ACP, 0, lpSubKey, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        char buf[512];
        wsprintfA(buf, "[D3d11Proxy] RegOpenKeyExW(%s) -> %ld%s\n",
                  narrow, r, tracked ? " [child of tracked]" : "");
        WriteLog(buf);
        if (r == ERROR_SUCCESS && phkResult)
            TrackKey(*phkResult);
    }
    return r;
}

static LSTATUS WINAPI HookedRegQueryValueExA(HKEY hKey, LPCSTR lpValueName,
    LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    LSTATUS r = g_pfnRegQueryValueExA(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
    if (IsTrackedKey(hKey) && lpValueName)
    {
        char buf[512];
        if (r == ERROR_SUCCESS && lpData && lpcbData && *lpcbData > 0 &&
            lpType && *lpType == REG_SZ)
        {
            wsprintfA(buf, "[D3d11Proxy] RegQueryValueExA(%s) = \"%s\"\n",
                      lpValueName, reinterpret_cast<const char*>(lpData));
            WriteLog(buf);
#ifdef BLOCK_NVAPI_STEREO
            // Spoof display adapter registry values that reveal NVIDIA identity.
            // Without this, the VendorId=AMD spoof on DXGI conflicts with
            // NVIDIA driver info in the display adapter class registry.
            char* str = reinterpret_cast<char*>(lpData);
            bool hasNv = false;
            for (const char* p = str; *p; ++p)
            {
                if ((p[0]|0x20)=='n' && (p[1]|0x20)=='v' && (p[2]|0x20)=='i' &&
                    (p[3]|0x20)=='d' && (p[4]|0x20)=='i' && (p[5]|0x20)=='a')
                { hasNv = true; break; }
            }
            if (hasNv)
            {
                if (lstrcmpiA(lpValueName, "ProviderName") == 0)
                {
                    memcpy(lpData, "AMD", 4);
                    *lpcbData = 4;
                    WriteLog("[D3d11Proxy]   -> SPOOFED ProviderName='AMD'\n");
                }
                else if (lstrcmpiA(lpValueName, "DriverDesc") == 0)
                {
                    memcpy(lpData, "AMD Radeon", 11);
                    *lpcbData = 11;
                    WriteLog("[D3d11Proxy]   -> SPOOFED DriverDesc='AMD Radeon'\n");
                }
                else
                {
                    // Generic: replace "NVIDIA" (6 chars) with "AMD   " (pad spaces)
                    for (char* p = str; *p; ++p)
                    {
                        if ((p[0]|0x20)=='n' && (p[1]|0x20)=='v' && (p[2]|0x20)=='i' &&
                            (p[3]|0x20)=='d' && (p[4]|0x20)=='i' && (p[5]|0x20)=='a')
                        { p[0]='A'; p[1]='M'; p[2]='D'; p[3]=' '; p[4]=' '; p[5]=' '; }
                    }
                    wsprintfA(buf, "[D3d11Proxy]   -> SPOOFED %s (NVIDIA->AMD)\n", lpValueName);
                    WriteLog(buf);
                }
            }
            // Spoof NVIDIA PCI vendor ID in MatchingDeviceId
            if (lstrcmpiA(lpValueName, "MatchingDeviceId") == 0)
            {
                bool spoofed = false;
                for (char* p = str; *p; ++p)
                {
                    if ((p[0]|0x20)=='v' && (p[1]|0x20)=='e' && (p[2]|0x20)=='n' &&
                        p[3]=='_' && p[4]=='1' && p[5]=='0' &&
                        (p[6]|0x20)=='d' && (p[7]|0x20)=='e')
                    { p[6]='0'; p[7]='2'; spoofed = true; break; }
                }
                if (spoofed)
                    WriteLog("[D3d11Proxy]   -> SPOOFED MatchingDeviceId ven_1002\n");
            }
#endif
        }
        else
        {
            wsprintfA(buf, "[D3d11Proxy] RegQueryValueExA(%s) -> status=%ld\n",
                      lpValueName, r);
            WriteLog(buf);
        }
    }
    return r;
}

static LSTATUS WINAPI HookedRegQueryValueExW(HKEY hKey, LPCWSTR lpValueName,
    LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    LSTATUS r = g_pfnRegQueryValueExW(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
    if (IsTrackedKey(hKey) && lpValueName)
    {
        char narrowName[256] = {};
        WideCharToMultiByte(CP_ACP, 0, lpValueName, -1, narrowName, sizeof(narrowName) - 1, nullptr, nullptr);
        char buf[512];
        if (r == ERROR_SUCCESS && lpData && lpcbData && *lpcbData > 0 &&
            lpType && *lpType == REG_SZ)
        {
            char narrowVal[256] = {};
            WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<LPCWSTR>(lpData), -1,
                                narrowVal, sizeof(narrowVal) - 1, nullptr, nullptr);
            wsprintfA(buf, "[D3d11Proxy] RegQueryValueExW(%s) = \"%s\"\n",
                      narrowName, narrowVal);
            WriteLog(buf);
#ifdef BLOCK_NVAPI_STEREO
            // Spoof display adapter registry values that reveal NVIDIA identity.
            wchar_t* wstr = reinterpret_cast<wchar_t*>(lpData);
            bool hasNv = false;
            for (const wchar_t* p = wstr; *p; ++p)
            {
                if ((p[0]|0x20)==L'n' && (p[1]|0x20)==L'v' && (p[2]|0x20)==L'i' &&
                    (p[3]|0x20)==L'd' && (p[4]|0x20)==L'i' && (p[5]|0x20)==L'a')
                { hasNv = true; break; }
            }
            if (hasNv)
            {
                if (lstrcmpiW(lpValueName, L"ProviderName") == 0)
                {
                    memcpy(lpData, L"AMD", 4 * sizeof(wchar_t));
                    *lpcbData = 4 * sizeof(wchar_t);
                    WriteLog("[D3d11Proxy]   -> SPOOFED ProviderName='AMD'\n");
                }
                else if (lstrcmpiW(lpValueName, L"DriverDesc") == 0)
                {
                    memcpy(lpData, L"AMD Radeon", 11 * sizeof(wchar_t));
                    *lpcbData = 11 * sizeof(wchar_t);
                    WriteLog("[D3d11Proxy]   -> SPOOFED DriverDesc='AMD Radeon'\n");
                }
                else
                {
                    for (wchar_t* p = wstr; *p; ++p)
                    {
                        if ((p[0]|0x20)==L'n' && (p[1]|0x20)==L'v' && (p[2]|0x20)==L'i' &&
                            (p[3]|0x20)==L'd' && (p[4]|0x20)==L'i' && (p[5]|0x20)==L'a')
                        { p[0]=L'A'; p[1]=L'M'; p[2]=L'D'; p[3]=L' '; p[4]=L' '; p[5]=L' '; }
                    }
                    wsprintfA(buf, "[D3d11Proxy]   -> SPOOFED %s (NVIDIA->AMD)\n", narrowName);
                    WriteLog(buf);
                }
            }
            if (lstrcmpiW(lpValueName, L"MatchingDeviceId") == 0)
            {
                bool spoofed = false;
                for (wchar_t* p = wstr; *p; ++p)
                {
                    if ((p[0]|0x20)==L'v' && (p[1]|0x20)==L'e' && (p[2]|0x20)==L'n' &&
                        p[3]==L'_' && p[4]==L'1' && p[5]==L'0' &&
                        (p[6]|0x20)==L'd' && (p[7]|0x20)==L'e')
                    { p[6]=L'0'; p[7]=L'2'; spoofed = true; break; }
                }
                if (spoofed)
                    WriteLog("[D3d11Proxy]   -> SPOOFED MatchingDeviceId ven_1002\n");
            }
#endif
        }
        else
        {
            wsprintfA(buf, "[D3d11Proxy] RegQueryValueExW(%s) -> status=%ld\n",
                      narrowName, r);
            WriteLog(buf);
        }
    }
    return r;
}

static LSTATUS WINAPI HookedRegGetValueA(HKEY hKey, LPCSTR lpSubKey,
    LPCSTR lpValue, DWORD dwFlags, LPDWORD pdwType, PVOID pvData, LPDWORD pcbData)
{
    LSTATUS r = g_pfnRegGetValueA(hKey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);

    bool tracked = IsTrackedKey(hKey);
    if (!tracked && lpSubKey && IsAmdRegistryPathA(lpSubKey))
        tracked = true;
    if (!tracked || !lpValue) return r;

    char buf[512];
    if (r == ERROR_SUCCESS && pvData && pcbData && *pcbData > 0 &&
        pdwType && *pdwType == REG_SZ)
    {
        wsprintfA(buf, "[D3d11Proxy] RegGetValueA(%s) = \"%s\"\n",
                  lpValue, reinterpret_cast<const char*>(pvData));
        WriteLog(buf);
#ifdef BLOCK_NVAPI_STEREO
        char* str = reinterpret_cast<char*>(pvData);
        bool hasNv = false;
        for (const char* p = str; *p; ++p)
        {
            if ((p[0]|0x20)=='n' && (p[1]|0x20)=='v' && (p[2]|0x20)=='i' &&
                (p[3]|0x20)=='d' && (p[4]|0x20)=='i' && (p[5]|0x20)=='a')
            { hasNv = true; break; }
        }
        if (hasNv)
        {
            if (lstrcmpiA(lpValue, "ProviderName") == 0)
            {
                memcpy(pvData, "AMD", 4);
                *pcbData = 4;
                WriteLog("[D3d11Proxy]   -> SPOOFED ProviderName='AMD'\n");
            }
            else if (lstrcmpiA(lpValue, "DriverDesc") == 0)
            {
                memcpy(pvData, "AMD Radeon", 11);
                *pcbData = 11;
                WriteLog("[D3d11Proxy]   -> SPOOFED DriverDesc='AMD Radeon'\n");
            }
            else
            {
                for (char* p = str; *p; ++p)
                {
                    if ((p[0]|0x20)=='n' && (p[1]|0x20)=='v' && (p[2]|0x20)=='i' &&
                        (p[3]|0x20)=='d' && (p[4]|0x20)=='i' && (p[5]|0x20)=='a')
                    { p[0]='A'; p[1]='M'; p[2]='D'; p[3]=' '; p[4]=' '; p[5]=' '; }
                }
                wsprintfA(buf, "[D3d11Proxy]   -> SPOOFED %s (NVIDIA->AMD)\n", lpValue);
                WriteLog(buf);
            }
        }
        if (lstrcmpiA(lpValue, "MatchingDeviceId") == 0)
        {
            bool spoofed = false;
            for (char* p = str; *p; ++p)
            {
                if ((p[0]|0x20)=='v' && (p[1]|0x20)=='e' && (p[2]|0x20)=='n' &&
                    p[3]=='_' && p[4]=='1' && p[5]=='0' &&
                    (p[6]|0x20)=='d' && (p[7]|0x20)=='e')
                { p[6]='0'; p[7]='2'; spoofed = true; break; }
            }
            if (spoofed)
                WriteLog("[D3d11Proxy]   -> SPOOFED MatchingDeviceId ven_1002\n");
        }
#endif
    }
    else
    {
        wsprintfA(buf, "[D3d11Proxy] RegGetValueA(%s) -> status=%ld type=%lu\n",
                  lpValue, r, pdwType ? *pdwType : 0);
        WriteLog(buf);
    }
    return r;
}

static LSTATUS WINAPI HookedRegGetValueW(HKEY hKey, LPCWSTR lpSubKey,
    LPCWSTR lpValue, DWORD dwFlags, LPDWORD pdwType, PVOID pvData, LPDWORD pcbData)
{
    LSTATUS r = g_pfnRegGetValueW(hKey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);

    bool tracked = IsTrackedKey(hKey);
    if (!tracked && lpSubKey && IsAmdRegistryPathW(lpSubKey))
        tracked = true;
    if (!tracked || !lpValue) return r;

    char narrowName[256] = {};
    WideCharToMultiByte(CP_ACP, 0, lpValue, -1, narrowName, sizeof(narrowName) - 1, nullptr, nullptr);
    char buf[512];
    if (r == ERROR_SUCCESS && pvData && pcbData && *pcbData > 0 &&
        pdwType && *pdwType == REG_SZ)
    {
        char narrowVal[256] = {};
        WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<LPCWSTR>(pvData), -1,
                            narrowVal, sizeof(narrowVal) - 1, nullptr, nullptr);
        wsprintfA(buf, "[D3d11Proxy] RegGetValueW(%s) = \"%s\"\n",
                  narrowName, narrowVal);
        WriteLog(buf);
#ifdef BLOCK_NVAPI_STEREO
        wchar_t* wstr = reinterpret_cast<wchar_t*>(pvData);
        bool hasNv = false;
        for (const wchar_t* p = wstr; *p; ++p)
        {
            if ((p[0]|0x20)==L'n' && (p[1]|0x20)==L'v' && (p[2]|0x20)==L'i' &&
                (p[3]|0x20)==L'd' && (p[4]|0x20)==L'i' && (p[5]|0x20)==L'a')
            { hasNv = true; break; }
        }
        if (hasNv)
        {
            if (lstrcmpiW(lpValue, L"ProviderName") == 0)
            {
                memcpy(pvData, L"AMD", 4 * sizeof(wchar_t));
                *pcbData = 4 * sizeof(wchar_t);
                WriteLog("[D3d11Proxy]   -> SPOOFED ProviderName='AMD'\n");
            }
            else if (lstrcmpiW(lpValue, L"DriverDesc") == 0)
            {
                memcpy(pvData, L"AMD Radeon", 11 * sizeof(wchar_t));
                *pcbData = 11 * sizeof(wchar_t);
                WriteLog("[D3d11Proxy]   -> SPOOFED DriverDesc='AMD Radeon'\n");
            }
            else
            {
                for (wchar_t* p = wstr; *p; ++p)
                {
                    if ((p[0]|0x20)==L'n' && (p[1]|0x20)==L'v' && (p[2]|0x20)==L'i' &&
                        (p[3]|0x20)==L'd' && (p[4]|0x20)==L'i' && (p[5]|0x20)==L'a')
                    { p[0]=L'A'; p[1]=L'M'; p[2]=L'D'; p[3]=L' '; p[4]=L' '; p[5]=L' '; }
                }
                wsprintfA(buf, "[D3d11Proxy]   -> SPOOFED %s (NVIDIA->AMD)\n", narrowName);
                WriteLog(buf);
            }
        }
        if (lstrcmpiW(lpValue, L"MatchingDeviceId") == 0)
        {
            bool spoofed = false;
            for (wchar_t* p = wstr; *p; ++p)
            {
                if ((p[0]|0x20)==L'v' && (p[1]|0x20)==L'e' && (p[2]|0x20)==L'n' &&
                    p[3]==L'_' && p[4]==L'1' && p[5]==L'0' &&
                    (p[6]|0x20)==L'd' && (p[7]|0x20)==L'e')
                { p[6]=L'0'; p[7]=L'2'; spoofed = true; break; }
            }
            if (spoofed)
                WriteLog("[D3d11Proxy]   -> SPOOFED MatchingDeviceId ven_1002\n");
        }
#endif
    }
    else
    {
        wsprintfA(buf, "[D3d11Proxy] RegGetValueW(%s) -> status=%ld type=%lu\n",
                  narrowName, r, pdwType ? *pdwType : 0);
        WriteLog(buf);
    }
    return r;
}

static void InstallRegistryHooks()
{
    HMODULE hKB = GetModuleHandleW(L"KernelBase.dll");
    if (!hKB) hKB = GetModuleHandleW(L"advapi32.dll");
    if (!hKB) return;

    PFN_GetProcAddr resolveProc = g_pfnGetProcAddrKB ? g_pfnGetProcAddrKB : g_pfnGetProcAddr;
    if (!resolveProc) resolveProc = reinterpret_cast<PFN_GetProcAddr>(&GetProcAddress);

    void* pfnROA  = resolveProc(hKB, "RegOpenKeyExA");
    void* pfnROW  = resolveProc(hKB, "RegOpenKeyExW");
    void* pfnRQA  = resolveProc(hKB, "RegQueryValueExA");
    void* pfnRQW  = resolveProc(hKB, "RegQueryValueExW");

    if (pfnROA && MH_CreateHook(pfnROA, &HookedRegOpenKeyExA,
            reinterpret_cast<void**>(&g_pfnRegOpenKeyExA)) == MH_OK)
    {
        MH_EnableHook(pfnROA);
        WriteLog("[D3d11Proxy] RegOpenKeyExA hook installed\n");
    }
    if (pfnROW && MH_CreateHook(pfnROW, &HookedRegOpenKeyExW,
            reinterpret_cast<void**>(&g_pfnRegOpenKeyExW)) == MH_OK)
    {
        MH_EnableHook(pfnROW);
        WriteLog("[D3d11Proxy] RegOpenKeyExW hook installed\n");
    }
    if (pfnRQA && MH_CreateHook(pfnRQA, &HookedRegQueryValueExA,
            reinterpret_cast<void**>(&g_pfnRegQueryValueExA)) == MH_OK)
    {
        MH_EnableHook(pfnRQA);
        WriteLog("[D3d11Proxy] RegQueryValueExA hook installed\n");
    }
    if (pfnRQW && MH_CreateHook(pfnRQW, &HookedRegQueryValueExW,
            reinterpret_cast<void**>(&g_pfnRegQueryValueExW)) == MH_OK)
    {
        MH_EnableHook(pfnRQW);
        WriteLog("[D3d11Proxy] RegQueryValueExW hook installed\n");
    }

    void* pfnRGA = resolveProc(hKB, "RegGetValueA");
    void* pfnRGW = resolveProc(hKB, "RegGetValueW");

    if (pfnRGA && MH_CreateHook(pfnRGA, &HookedRegGetValueA,
            reinterpret_cast<void**>(&g_pfnRegGetValueA)) == MH_OK)
    {
        MH_EnableHook(pfnRGA);
        WriteLog("[D3d11Proxy] RegGetValueA hook installed\n");
    }
    if (pfnRGW && MH_CreateHook(pfnRGW, &HookedRegGetValueW,
            reinterpret_cast<void**>(&g_pfnRegGetValueW)) == MH_OK)
    {
        MH_EnableHook(pfnRGW);
        WriteLog("[D3d11Proxy] RegGetValueW hook installed\n");
    }
}

static void InstallLoadLibraryHooks(HMODULE hK)
{
    // Use the real (unhooked) GetProcAddress to find LoadLibrary*
    PFN_GetProcAddr resolveProc = g_pfnGetProcAddrKB ? g_pfnGetProcAddrKB : g_pfnGetProcAddr;
    if (!resolveProc) resolveProc = reinterpret_cast<PFN_GetProcAddr>(&GetProcAddress);

    HMODULE hKB = GetModuleHandleW(L"KernelBase.dll");
    HMODULE hTarget = hKB ? hKB : hK;

    void* pfnLLA  = resolveProc(hTarget, "LoadLibraryA");
    void* pfnLLW  = resolveProc(hTarget, "LoadLibraryW");
    void* pfnLLEA = resolveProc(hTarget, "LoadLibraryExA");
    void* pfnLLEW = resolveProc(hTarget, "LoadLibraryExW");

    if (pfnLLA && MH_CreateHook(pfnLLA, &HookedLoadLibraryA,
            reinterpret_cast<void**>(&g_pfnLoadLibraryA)) == MH_OK)
    {
        MH_EnableHook(pfnLLA);
        WriteLog("[D3d11Proxy] LoadLibraryA hook installed\n");
    }
    if (pfnLLW && MH_CreateHook(pfnLLW, &HookedLoadLibraryW,
            reinterpret_cast<void**>(&g_pfnLoadLibraryW)) == MH_OK)
    {
        MH_EnableHook(pfnLLW);
        WriteLog("[D3d11Proxy] LoadLibraryW hook installed\n");
    }
    if (pfnLLEA && MH_CreateHook(pfnLLEA, &HookedLoadLibraryExA,
            reinterpret_cast<void**>(&g_pfnLoadLibraryExA)) == MH_OK)
    {
        MH_EnableHook(pfnLLEA);
        WriteLog("[D3d11Proxy] LoadLibraryExA hook installed\n");
    }
    if (pfnLLEW && MH_CreateHook(pfnLLEW, &HookedLoadLibraryExW,
            reinterpret_cast<void**>(&g_pfnLoadLibraryExW)) == MH_OK)
    {
        MH_EnableHook(pfnLLEW);
        WriteLog("[D3d11Proxy] LoadLibraryExW hook installed\n");
    }
}

static void InstallGetProcHook()
{
    HMODULE hK = GetModuleHandleW(L"kernel32.dll");
    if (!hK) return;
    void* pfn = GetProcAddress(hK, "GetProcAddress");
    if (!pfn) return;
    if (MH_CreateHook(pfn, &HookedGetProcAddr,
            reinterpret_cast<void**>(&g_pfnGetProcAddr)) == MH_OK)
    {
        MH_EnableHook(pfn);
        WriteLog("[D3d11Proxy] GetProcAddress hook installed\n");
    }
    else
    {
        WriteLog("[D3d11Proxy] GetProcAddress hook FAILED\n");
        return;
    }

    // Also hook KernelBase!GetProcAddress if it is a separate implementation
    HMODULE hKB = GetModuleHandleW(L"KernelBase.dll");
    if (hKB)
    {
        void* pfnKB = g_pfnGetProcAddr(hKB, "GetProcAddress");
        if (pfnKB && pfnKB != pfn)
        {
            if (MH_CreateHook(pfnKB, &HookedGetProcAddr,
                    reinterpret_cast<void**>(&g_pfnGetProcAddrKB)) == MH_OK)
            {
                MH_EnableHook(pfnKB);
                WriteLog("[D3d11Proxy] KernelBase GetProcAddress hook installed\n");
            }
        }
    }

    // Hook GetModuleHandleA to track when games obtain atidxx32/64 handle
    void* pfnGMH = g_pfnGetProcAddr(hK, "GetModuleHandleA");
    if (pfnGMH)
    {
        if (MH_CreateHook(pfnGMH, &HookedGetModuleHandleA,
                reinterpret_cast<void**>(&g_pfnGetModuleHandleA)) == MH_OK)
        {
            MH_EnableHook(pfnGMH);
            WriteLog("[D3d11Proxy] GetModuleHandleA hook installed\n");
        }
    }

    // Hook LoadLibrary* to redirect atidxx loading from any path to our local proxy
    InstallLoadLibraryHooks(hK);
}

// ---- DllMain --------------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        // Store module handle first so WriteLog can resolve the game directory.
        g_hSelf = hInst;
        DisableThreadLibraryCalls(hInst);
        WriteLog("[D3d11Proxy] DLL_PROCESS_ATTACH (wiz3D " DISPLAYED_VERSION ")\n");

        // Store system directory for RealD3D11() lazy-loader.
        // Real d3d11.dll is NOT loaded here — LoadLibraryW from DllMain risks a
        // loader-lock deadlock when Steam's overlay threads are initialising.
        // The first D3D11 thunk call triggers lazy loading safely outside DllMain.
        GetSystemDirectoryW(g_sysDir, MAX_PATH);
        WriteLog("[D3d11Proxy] GetSystemDirectoryW OK\n");

        // dxgi.dll: Steam pre-loads it, so GetModuleHandleExW finds it without LoadLibraryW.
        // For non-Steam games it won't be loaded yet, so fall back to LoadLibraryW.
        if (!GetModuleHandleExW(0, L"dxgi.dll", &g_hRealDXGI))
        {
            wchar_t p[MAX_PATH];
            wcscpy_s(p, g_sysDir);
            wcscat_s(p, L"\\dxgi.dll");
            g_hRealDXGI = LoadLibraryW(p);
        }
        WriteLog(g_hRealDXGI
            ? "[D3d11Proxy] dxgi.dll handle acquired\n"
            : "[D3d11Proxy] WARNING: dxgi.dll not available\n");

        MH_Initialize();
        WriteLog("[D3d11Proxy] MH_Initialize OK\n");
        InstallDxgiHooks();
        PatchAmdIAT();
        InstallGetProcHook();
        InstallRegistryHooks();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        WriteLog("[D3d11Proxy] DLL_PROCESS_DETACH\n");
        MH_Uninitialize();
        if (g_hAmdAdl)    { FreeLibrary(g_hAmdAdl);    g_hAmdAdl    = nullptr; }
        if (g_hAmdProxy)  { FreeLibrary(g_hAmdProxy);  g_hAmdProxy  = nullptr; }
        // Do NOT FreeLibrary g_hRealDXGI / g_hRealD3D11.
        // Games like Thief (2014) unload and reload d3d11.dll mid-process
        // (launcher -> game).  AmdQbProxy (pinned, stays loaded) holds COM
        // pointers to objects created by real d3d11.dll.  Freeing the real
        // DLL unmaps the vtables, so the next AmdDxExtCreate11 call crashes
        // when it tries to Release the old device/context.  System DLLs are
        // cleaned up automatically at process exit.
        g_hRealDXGI  = nullptr;
        g_hRealD3D11 = nullptr;
    }
    return TRUE;
}

// ---- d3d11.dll export thunks ----------------------------------------------
// All 51 exports of system d3d11.dll forwarded to the real module.
// Using void* for D3D11/DXGI object pointers avoids pulling in d3d11.h
// (which would add unwanted dxgi.lib link dependencies) while still
// producing the correct x86 stdcall stack frames.

extern "C" HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(void* a0, void** a1)
{
    typedef HRESULT(WINAPI*PFN)(void*, void**);
    static PFN fn = (PFN)RealD3D11("CreateDirect3D11DeviceFromDXGIDevice");
    return fn ? fn(a0, a1) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI CreateDirect3D11SurfaceFromDXGISurface(void* a0, void** a1)
{
    typedef HRESULT(WINAPI*PFN)(void*, void**);
    static PFN fn = (PFN)RealD3D11("CreateDirect3D11SurfaceFromDXGISurface");
    return fn ? fn(a0, a1) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D11CoreCreateDevice(
    void* pFactory, void* pAdapter, UINT Flags,
    const void* pFeatureLevels, UINT FeatureLevels, void** ppDevice)
{
    typedef HRESULT(WINAPI*PFN)(void*, void*, UINT, const void*, UINT, void**);
    static PFN fn = (PFN)RealD3D11("D3D11CoreCreateDevice");
    return fn ? fn(pFactory, pAdapter, Flags, pFeatureLevels, FeatureLevels, ppDevice) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D11CoreCreateLayeredDevice(
    const void* pInput, DWORD InputSize, const void* pLayerInput,
    const void* riid, void** ppvObj)
{
    typedef HRESULT(WINAPI*PFN)(const void*, DWORD, const void*, const void*, void**);
    static PFN fn = (PFN)RealD3D11("D3D11CoreCreateLayeredDevice");
    return fn ? fn(pInput, InputSize, pLayerInput, riid, ppvObj) : E_NOTIMPL;
}

extern "C" SIZE_T WINAPI D3D11CoreGetLayeredDeviceSize(const void* pInput, DWORD InputSize)
{
    typedef SIZE_T(WINAPI*PFN)(const void*, DWORD);
    static PFN fn = (PFN)RealD3D11("D3D11CoreGetLayeredDeviceSize");
    return fn ? fn(pInput, InputSize) : 0;
}

extern "C" HRESULT WINAPI D3D11CoreRegisterLayers(const void* pLayerDesc, DWORD NumLayers)
{
    typedef HRESULT(WINAPI*PFN)(const void*, DWORD);
    static PFN fn = (PFN)RealD3D11("D3D11CoreRegisterLayers");
    return fn ? fn(pLayerDesc, NumLayers) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D11CreateDevice(
    void* pAdapter, int DriverType, void* Software, UINT Flags,
    const void* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    void** ppDevice, void* pFeatureLevel, void** ppContext)
{
    typedef HRESULT(WINAPI*PFN)(void*, int, void*, UINT, const void*, UINT, UINT, void**, void*, void**);
    // Prefer the pre-resolved pointer (captured before EAT hooks).
    // Fall back to late resolution only if d3d11.dll was not yet loaded.
    static PFN fn = nullptr;
    if (!fn)
    {
        fn = (PFN)g_pfnD3D11CreateDevice;
        if (!fn)
            fn = (PFN)RealD3D11("D3D11CreateDevice");
        char fbuf[128];
        wsprintfA(fbuf, "[D3d11Proxy] D3D11CreateDevice fn=%p self=%p\n",
                  (void*)fn, (void*)&D3D11CreateDevice);
        WriteLog(fbuf);
    }
    // Recursion guard — middleware (e.g. Epic overlay) may hook the real
    // d3d11.dll and call back through our proxy.  Allow exactly ONE
    // re-entry so the hook chain can resolve through its trampoline.
    static __declspec(thread) int t_depth = 0;
    if (t_depth > 0)
    {
        if (t_depth > 1)
        {
            WriteLog("[D3d11Proxy] D3D11CreateDevice DEEP RECURSION -> E_FAIL\n");
            return E_FAIL;
        }
        ++t_depth;
        HRESULT hr2 = fn ? fn(pAdapter, DriverType, Software, Flags,
                       pFeatureLevels, FeatureLevels, SDKVersion,
                       ppDevice, pFeatureLevel, ppContext) : E_NOTIMPL;
        --t_depth;
        return hr2;
    }
    ++t_depth;
    char buf[128];
    wchar_t debugFlag[2] = {};
    if (GetEnvironmentVariableW(L"DEUSEXHRVR_D3D_DEBUG", debugFlag, 2) && debugFlag[0] == L'1')
        Flags |= 0x2; // D3D11_CREATE_DEVICE_DEBUG; process-local diagnostic only.
    wsprintfA(buf, "[D3d11Proxy] D3D11CreateDevice(adapter=%p driverType=%d flags=0x%X)\n",
              pAdapter, DriverType, Flags);
    WriteLog(buf);
    HRESULT hr = fn ? fn(pAdapter, DriverType, Software, Flags,
                   pFeatureLevels, FeatureLevels, SDKVersion,
                   ppDevice, pFeatureLevel, ppContext) : E_NOTIMPL;
    wsprintfA(buf, "[D3d11Proxy] D3D11CreateDevice -> 0x%08X\n", hr);
    WriteLog(buf);
    --t_depth;
    return hr;
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    void* pAdapter, int DriverType, void* Software, UINT Flags,
    const void* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    const void* pSwapChainDesc, void** ppSwapChain,
    void** ppDevice, void* pFeatureLevel, void** ppContext)
{
    typedef HRESULT(WINAPI*PFN)(void*, int, void*, UINT, const void*, UINT, UINT,
                                 const void*, void**, void**, void*, void**);
    // Prefer the pre-resolved pointer (captured before EAT hooks).
    static PFN fn = nullptr;
    if (!fn)
    {
        fn = (PFN)g_pfnD3D11CreateDeviceAndSwapChain;
        if (!fn)
            fn = (PFN)RealD3D11("D3D11CreateDeviceAndSwapChain");
        char fbuf[128];
        wsprintfA(fbuf, "[D3d11Proxy] D3D11CreateDeviceAndSwapChain fn=%p self=%p\n",
                  (void*)fn, (void*)&D3D11CreateDeviceAndSwapChain);
        WriteLog(fbuf);
    }
    // Recursion guard (same logic as D3D11CreateDevice)
    static __declspec(thread) int t_depth = 0;
    if (t_depth > 0)
    {
        if (t_depth > 1)
        {
            WriteLog("[D3d11Proxy] D3D11CreateDeviceAndSwapChain DEEP RECURSION -> E_FAIL\n");
            return E_FAIL;
        }
        ++t_depth;
        HRESULT hr2 = fn ? fn(pAdapter, DriverType, Software, Flags,
                       pFeatureLevels, FeatureLevels, SDKVersion,
                       pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppContext) : E_NOTIMPL;
        --t_depth;
        return hr2;
    }
    ++t_depth;
    char buf[128];
    wsprintfA(buf, "[D3d11Proxy] D3D11CreateDeviceAndSwapChain(adapter=%p driverType=%d flags=0x%X)\n",
              pAdapter, DriverType, Flags);
    WriteLog(buf);
    HRESULT hr = fn ? fn(pAdapter, DriverType, Software, Flags,
                   pFeatureLevels, FeatureLevels, SDKVersion,
                   pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppContext) : E_NOTIMPL;
    wsprintfA(buf, "[D3d11Proxy] D3D11CreateDeviceAndSwapChain -> 0x%08X\n", hr);
    WriteLog(buf);
    --t_depth;
    return hr;
}

extern "C" HRESULT WINAPI D3D11CreateDeviceForD3D12(
    void* pDevice, UINT Flags, const void* pFeatureLevels,
    UINT FeatureLevels, UINT SDKVersion, void** ppDevice, void** ppContext)
{
    typedef HRESULT(WINAPI*PFN)(void*, UINT, const void*, UINT, UINT, void**, void**);
    static PFN fn = (PFN)RealD3D11("D3D11CreateDeviceForD3D12");
    return fn ? fn(pDevice, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
                   ppDevice, ppContext) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D11On12CreateDevice(
    void* pDevice, UINT Flags, const void* pFeatureLevels, UINT FeatureLevels,
    void* ppCommandQueues, UINT NumQueues, UINT NodeMask,
    void** ppDevice, void** ppContext, void* pChosenFeatureLevel)
{
    typedef HRESULT(WINAPI*PFN)(void*, UINT, const void*, UINT,
                                 void*, UINT, UINT, void**, void**, void*);
    static PFN fn = (PFN)RealD3D11("D3D11On12CreateDevice");
    return fn ? fn(pDevice, Flags, pFeatureLevels, FeatureLevels,
                   ppCommandQueues, NumQueues, NodeMask,
                   ppDevice, ppContext, pChosenFeatureLevel) : E_NOTIMPL;
}

// D3DKMT kernel-mode thunks — all take one struct pointer, return NTSTATUS
#define DKMT1(name) \
    extern "C" LONG WINAPI name(void* p) { \
        typedef LONG(WINAPI*PFN)(void*); \
        static PFN fn = (PFN)RealD3D11(#name); \
        return fn ? fn(p) : 0xC00000BB; /* STATUS_NOT_SUPPORTED */ \
    }

DKMT1(D3DKMTCloseAdapter)
DKMT1(D3DKMTCreateAllocation)
DKMT1(D3DKMTCreateContext)
DKMT1(D3DKMTCreateDevice)
DKMT1(D3DKMTCreateSynchronizationObject)
DKMT1(D3DKMTDestroyAllocation)
DKMT1(D3DKMTDestroyContext)
DKMT1(D3DKMTDestroyDevice)
DKMT1(D3DKMTDestroySynchronizationObject)
DKMT1(D3DKMTEscape)
DKMT1(D3DKMTGetContextSchedulingPriority)
DKMT1(D3DKMTGetDeviceState)
DKMT1(D3DKMTGetDisplayModeList)
DKMT1(D3DKMTGetMultisampleMethodList)
DKMT1(D3DKMTGetRuntimeData)
DKMT1(D3DKMTGetSharedPrimaryHandle)
DKMT1(D3DKMTLock)
DKMT1(D3DKMTOpenAdapterFromHdc)
DKMT1(D3DKMTOpenResource)
DKMT1(D3DKMTPresent)
DKMT1(D3DKMTQueryAdapterInfo)
DKMT1(D3DKMTQueryAllocationResidency)
DKMT1(D3DKMTQueryResourceInfo)
DKMT1(D3DKMTRender)
DKMT1(D3DKMTSetAllocationPriority)
DKMT1(D3DKMTSetContextSchedulingPriority)
DKMT1(D3DKMTSetDisplayMode)
DKMT1(D3DKMTSetDisplayPrivateDriverFormat)
DKMT1(D3DKMTSetGammaRamp)
DKMT1(D3DKMTSetVidPnSourceOwner)
DKMT1(D3DKMTSignalSynchronizationObject)
DKMT1(D3DKMTUnlock)
DKMT1(D3DKMTWaitForSynchronizationObject)
DKMT1(D3DKMTWaitForVerticalBlankEvent)

#undef DKMT1

extern "C" int WINAPI D3DPerformance_BeginEvent(DWORD col, const void* name)
{
    typedef int(WINAPI*PFN)(DWORD, const void*);
    static PFN fn = (PFN)RealD3D11("D3DPerformance_BeginEvent");
    return fn ? fn(col, name) : -1;
}

extern "C" int WINAPI D3DPerformance_EndEvent()
{
    typedef int(WINAPI*PFN)();
    static PFN fn = (PFN)RealD3D11("D3DPerformance_EndEvent");
    return fn ? fn() : -1;
}

extern "C" DWORD WINAPI D3DPerformance_GetStatus()
{
    typedef DWORD(WINAPI*PFN)();
    static PFN fn = (PFN)RealD3D11("D3DPerformance_GetStatus");
    return fn ? fn() : 0;
}

extern "C" void WINAPI D3DPerformance_SetMarker(DWORD col, const void* name)
{
    typedef void(WINAPI*PFN)(DWORD, const void*);
    static PFN fn = (PFN)RealD3D11("D3DPerformance_SetMarker");
    if (fn) fn(col, name);
}

extern "C" void WINAPI EnableFeatureLevelUpgrade(void* pData)
{
    typedef void(WINAPI*PFN)(void*);
    static PFN fn = (PFN)RealD3D11("EnableFeatureLevelUpgrade");
    if (fn) fn(pData);
}

extern "C" LONG WINAPI OpenAdapter10(void* pOpenData)
{
    typedef LONG(WINAPI*PFN)(void*);
    static PFN fn = (PFN)RealD3D11("OpenAdapter10");
    return fn ? fn(pOpenData) : 0xC00000BB;
}

extern "C" LONG WINAPI OpenAdapter10_2(void* pOpenData)
{
    typedef LONG(WINAPI*PFN)(void*);
    static PFN fn = (PFN)RealD3D11("OpenAdapter10_2");
    return fn ? fn(pOpenData) : 0xC00000BB;
}
