// AmdAdlProxy.cpp
// Fake AMD Display Library (ADL) proxy DLL.
// Builds as atiadlxy.dll (x86) / atiadlxx.dll (x64).
//
// Some HD3D games (Codemasters EGO engine: DiRT Showdown, GRID 2, F1 series)
// load atiadlxy.dll / atiadlxx.dll to check for AMD hardware and stereo
// capabilities BEFORE loading atidxx32.dll for quad-buffer stereo.  On NVIDIA
// systems the ADL DLLs don't exist, so the games give up on HD3D.
//
// This proxy returns fake AMD adapter info (VendorId 0x1002) so the games
// proceed to load atidxx32.dll / atidxx64.dll (our AmdQbProxy).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define DISPLAYED_VERSION "TombRaiderVR 0.1.0 / wiz3D 981de7b"
#include <cstring>

// ---- ADL constants and types ------------------------------------------------

#define ADL_MAX_PATH            256
#define ADL_OK                  0
#define ADL_ERR                 -1
#define ADL_ERR_NOT_INIT        -2
#define ADL_ERR_INVALID_PARAM   -3
#define ADL_ERR_NOT_SUPPORTED   -8

typedef void* (__stdcall* ADL_MAIN_MALLOC_CALLBACK)(int);

struct AdapterInfo
{
    int  iSize;
    int  iAdapterIndex;
    char strUDID[ADL_MAX_PATH];
    int  iBusNumber;
    int  iDeviceNumber;
    int  iFunctionNumber;
    int  iVendorID;
    char strAdapterName[ADL_MAX_PATH];
    char strDisplayName[ADL_MAX_PATH];
    int  iPresent;
    int  iExist;
    char strDriverPath[ADL_MAX_PATH];
    char strDriverPathExt[ADL_MAX_PATH];
    char strPNPString[ADL_MAX_PATH];
    int  iOSDisplayIndex;
};

struct ADLVersionsInfo
{
    char strDriverVer[ADL_MAX_PATH];
    char strCatalystVersion[ADL_MAX_PATH];
    char strCatalystWebLink[ADL_MAX_PATH];
};

struct ADLDisplayID
{
    int iDisplayLogicalIndex;
    int iDisplayPhysicalIndex;
    int iDisplayLogicalAdapterIndex;
    int iDisplayPhysicalAdapterIndex;
};

// ADL_DISPLAY_DISPLAYINFO_* flags
#define ADL_DISPLAY_DISPLAYINFO_DISPLAYCONNECTED      0x00000001
#define ADL_DISPLAY_DISPLAYINFO_DISPLAYMAPPED         0x00000002
#define ADL_DISPLAY_DISPLAYINFO_MANNER_SINGLE         0x00000010

struct ADLDisplayInfo
{
    ADLDisplayID displayID;
    int iDisplayControllerIndex;
    char strDisplayName[ADL_MAX_PATH];
    char strDisplayManufacturerName[ADL_MAX_PATH];
    int iDisplayType;          // ADL_DT_*
    int iDisplayOutputType;    // ADL_DOT_*
    int iDisplayConnector;     // ADL_DC_*
    int iDisplayInfoMask;
    int iDisplayInfoValue;
};

struct ADLMode
{
    int iAdapterIndex;
    ADLDisplayID displayID;
    int iXPos;
    int iYPos;
    int iXRes;
    int iYRes;
    int iColourDepth;
    float fRefreshRate;
    int iOrientation;
    int iModeFlag;
    int iModeMask;
    int iModeValue;
};

struct ADLDisplayMap
{
    int iDisplayMapIndex;
    ADLMode displayMode;
    int iNumDisplayTarget;
    int iFirstDisplayTargetArrayIndex;
    int iDisplayMapMask;
    int iDisplayMapValue;
};

struct ADLDisplayTarget
{
    ADLDisplayID displayID;
    int iDisplayMapIndex;
    int iDisplayTargetMask;
    int iDisplayTargetValue;
};

// ---- Globals

static HINSTANCE g_hSelf = nullptr;
static ADL_MAIN_MALLOC_CALLBACK g_mallocCb = nullptr;
static bool g_bInitialized = false;

// ---- Diagnostic log ---------------------------------------------------------

static void WriteLog(const char* msg)
{
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
    wcscat_s(path, L"HD3D_adl.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(h, msg, static_cast<DWORD>(strlen(msg)), &written, nullptr);
    CloseHandle(h);
}

// ---- ADL function implementations ------------------------------------------

extern "C" int __cdecl ADL_Main_Control_Create(ADL_MAIN_MALLOC_CALLBACK callback, int iEnumConnectedAdapters)
{
    WriteLog("[AmdAdlProxy] ADL_Main_Control_Create\n");
    g_mallocCb = callback;
    g_bInitialized = true;
    return ADL_OK;
}

extern "C" int __cdecl ADL_Main_Control_Destroy()
{
    WriteLog("[AmdAdlProxy] ADL_Main_Control_Destroy\n");
    g_bInitialized = false;
    g_mallocCb = nullptr;
    return ADL_OK;
}

extern "C" int __cdecl ADL_Main_Control_Refresh()
{
    WriteLog("[AmdAdlProxy] ADL_Main_Control_Refresh\n");
    return ADL_OK;
}

extern "C" int __cdecl ADL_Main_Control_IsFunctionValid(HMODULE hModule, const char* lpProcName)
{
    char buf[300];
    wsprintfA(buf, "[AmdAdlProxy] ADL_Main_Control_IsFunctionValid(%s)\n",
              lpProcName ? lpProcName : "(null)");
    WriteLog(buf);
    if (lpProcName && GetProcAddress(g_hSelf, lpProcName))
        return 1;
    return 0;
}

extern "C" int __cdecl ADL_Adapter_NumberOfAdapters_Get(int* lpNumAdapters)
{
    WriteLog("[AmdAdlProxy] ADL_Adapter_NumberOfAdapters_Get -> 1\n");
    if (!lpNumAdapters) return ADL_ERR_INVALID_PARAM;
    *lpNumAdapters = 1;
    return ADL_OK;
}

extern "C" int __cdecl ADL_Adapter_AdapterInfo_Get(AdapterInfo* lpInfo, int iInputSize)
{
    char buf[80];
    wsprintfA(buf, "[AmdAdlProxy] ADL_Adapter_AdapterInfo_Get(size=%d)\n", iInputSize);
    WriteLog(buf);
    if (!lpInfo || iInputSize < static_cast<int>(sizeof(AdapterInfo)))
        return ADL_ERR_INVALID_PARAM;

    memset(lpInfo, 0, sizeof(AdapterInfo));
    lpInfo->iSize            = sizeof(AdapterInfo);
    lpInfo->iAdapterIndex    = 0;
    strcpy_s(lpInfo->strUDID, "PCI_VEN_1002&DEV_67DF&SUBSYS_0000&REV_00");
    lpInfo->iBusNumber       = 1;
    lpInfo->iDeviceNumber    = 0;
    lpInfo->iFunctionNumber  = 0;
    lpInfo->iVendorID        = 0x1002;
    strcpy_s(lpInfo->strAdapterName,  "AMD Radeon RX 580 Series");
    strcpy_s(lpInfo->strDisplayName,  "\\\\.\\DISPLAY1");
    lpInfo->iPresent         = 1;
    lpInfo->iExist           = 1;
    strcpy_s(lpInfo->strDriverPath,    "\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Video\\{FAKE-AMD}\\0000");
    strcpy_s(lpInfo->strDriverPathExt, "\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Video\\{FAKE-AMD}\\0000");
    strcpy_s(lpInfo->strPNPString,     "PCI\\VEN_1002&DEV_67DF&SUBSYS_0000&REV_00\\0000");
    lpInfo->iOSDisplayIndex  = 0;

    return ADL_OK;
}

extern "C" int __cdecl ADL_Adapter_Active_Get(int iAdapterIndex, int* lpStatus)
{
    char buf[80];
    wsprintfA(buf, "[AmdAdlProxy] ADL_Adapter_Active_Get(adapter=%d)\n", iAdapterIndex);
    WriteLog(buf);
    if (!lpStatus) return ADL_ERR_INVALID_PARAM;
    *lpStatus = 1; // ADL_TRUE
    return ADL_OK;
}

extern "C" int __cdecl ADL_Adapter_ID_Get(int iAdapterIndex, int* lpAdapterID)
{
    char buf[80];
    wsprintfA(buf, "[AmdAdlProxy] ADL_Adapter_ID_Get(adapter=%d)\n", iAdapterIndex);
    WriteLog(buf);
    if (!lpAdapterID) return ADL_ERR_INVALID_PARAM;
    *lpAdapterID = 0x67DF; // RX 580 device ID
    return ADL_OK;
}

extern "C" int __cdecl ADL_Adapter_Aspects_Get(int iAdapterIndex, char* lpAspects, int iSize)
{
    WriteLog("[AmdAdlProxy] ADL_Adapter_Aspects_Get\n");
    if (lpAspects && iSize > 0)
        strcpy_s(lpAspects, iSize, "Power;3D;Stereo;Display;Core");
    return ADL_OK;
}

extern "C" int __cdecl ADL_Adapter_Primary_Get(int* lpPrimaryAdapterIndex)
{
    WriteLog("[AmdAdlProxy] ADL_Adapter_Primary_Get\n");
    if (!lpPrimaryAdapterIndex) return ADL_ERR_INVALID_PARAM;
    *lpPrimaryAdapterIndex = 0;
    return ADL_OK;
}

extern "C" int __cdecl ADL_Display_NumberOfDisplays_Get(int iAdapterIndex, int* lpNumDisplays)
{
    char buf[80];
    wsprintfA(buf, "[AmdAdlProxy] ADL_Display_NumberOfDisplays_Get(adapter=%d)\n", iAdapterIndex);
    WriteLog(buf);
    if (!lpNumDisplays) return ADL_ERR_INVALID_PARAM;
    *lpNumDisplays = 1;
    return ADL_OK;
}

extern "C" int __cdecl ADL_Display_DisplayInfo_Get(int iAdapterIndex, int* lpNumDisplays, void** lppInfo, int iForceDetect)
{
    char buf[80];
    wsprintfA(buf, "[AmdAdlProxy] ADL_Display_DisplayInfo_Get(adapter=%d)\n", iAdapterIndex);
    WriteLog(buf);
    if (!lpNumDisplays || !lppInfo) return ADL_ERR_INVALID_PARAM;

    if (!g_mallocCb) { *lpNumDisplays = 0; *lppInfo = nullptr; return ADL_OK; }

    *lpNumDisplays = 1;
    ADLDisplayInfo* pInfo = static_cast<ADLDisplayInfo*>(g_mallocCb(sizeof(ADLDisplayInfo)));
    if (!pInfo) { *lpNumDisplays = 0; *lppInfo = nullptr; return ADL_ERR; }
    memset(pInfo, 0, sizeof(ADLDisplayInfo));

    pInfo->displayID.iDisplayLogicalIndex          = 0;
    pInfo->displayID.iDisplayPhysicalIndex         = 0;
    pInfo->displayID.iDisplayLogicalAdapterIndex    = 0;
    pInfo->displayID.iDisplayPhysicalAdapterIndex   = 0;
    pInfo->iDisplayControllerIndex = 0;
    strcpy_s(pInfo->strDisplayName, "\\\\.\\DISPLAY1");
    strcpy_s(pInfo->strDisplayManufacturerName, "Generic Monitor");
    pInfo->iDisplayType       = 3;   // ADL_DT_DIGITAL_FLAT_PANEL
    pInfo->iDisplayOutputType = 4;   // ADL_DOT_HDMI (common for stereo)
    pInfo->iDisplayConnector  = 12;  // ADL_DC_HDMI
    pInfo->iDisplayInfoMask   = ADL_DISPLAY_DISPLAYINFO_DISPLAYCONNECTED |
                                ADL_DISPLAY_DISPLAYINFO_DISPLAYMAPPED |
                                ADL_DISPLAY_DISPLAYINFO_MANNER_SINGLE;
    pInfo->iDisplayInfoValue  = ADL_DISPLAY_DISPLAYINFO_DISPLAYCONNECTED |
                                ADL_DISPLAY_DISPLAYINFO_DISPLAYMAPPED |
                                ADL_DISPLAY_DISPLAYINFO_MANNER_SINGLE;

    *lppInfo = pInfo;
    return ADL_OK;
}

extern "C" int __cdecl ADL_Display_SLSMapIndex_Get(int iAdapterIndex, int iNumDisplayTarget,
    void* lpDisplayTarget, int* lpSLSMapIndex)
{
    WriteLog("[AmdAdlProxy] ADL_Display_SLSMapIndex_Get\n");
    if (lpSLSMapIndex) *lpSLSMapIndex = -1;
    return ADL_OK;
}

extern "C" int __cdecl ADL_Display_SLSMapConfig_Get(int iAdapterIndex, int iSLSMapIndex,
    void* lpSLSMap, int* lpNumSLSTarget, void** lppSLSTarget,
    int* lpNumNativeMode, void** lppNativeMode,
    int* lpNumBezelMode, void** lppBezelMode,
    int* lpNumTransientMode, void** lppTransientMode,
    int* lpNumSLSOffset, void** lppSLSOffset, int iOption)
{
    WriteLog("[AmdAdlProxy] ADL_Display_SLSMapConfig_Get\n");
    if (lpNumSLSTarget) *lpNumSLSTarget = 0;
    if (lppSLSTarget) *lppSLSTarget = nullptr;
    if (lpNumNativeMode) *lpNumNativeMode = 0;
    if (lppNativeMode) *lppNativeMode = nullptr;
    if (lpNumBezelMode) *lpNumBezelMode = 0;
    if (lppBezelMode) *lppBezelMode = nullptr;
    if (lpNumTransientMode) *lpNumTransientMode = 0;
    if (lppTransientMode) *lppTransientMode = nullptr;
    if (lpNumSLSOffset) *lpNumSLSOffset = 0;
    if (lppSLSOffset) *lppSLSOffset = nullptr;
    return ADL_OK;
}

extern "C" int __cdecl ADL_Display_Modes_Get(int iAdapterIndex, int iDisplayIndex,
    int* lpNumModes, void** lppModes)
{
    WriteLog("[AmdAdlProxy] ADL_Display_Modes_Get\n");
    if (lpNumModes) *lpNumModes = 0;
    if (lppModes) *lppModes = nullptr;
    return ADL_OK;
}

extern "C" int __cdecl ADL_Display_DisplayMapConfig_Get(int iAdapterIndex,
    int* lpNumDisplayMap, void** lppDisplayMap,
    int* lpNumDisplayTarget, void** lppDisplayTarget, int iOption)
{
    WriteLog("[AmdAdlProxy] ADL_Display_DisplayMapConfig_Get\n");
    if (!lpNumDisplayMap || !lppDisplayMap || !lpNumDisplayTarget || !lppDisplayTarget)
        return ADL_ERR_INVALID_PARAM;

    if (!g_mallocCb)
    {
        *lpNumDisplayMap = 0; *lppDisplayMap = nullptr;
        *lpNumDisplayTarget = 0; *lppDisplayTarget = nullptr;
        return ADL_OK;
    }

    // Query current primary display resolution for realistic mode data
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    if (screenW <= 0) screenW = 1920;
    if (screenH <= 0) screenH = 1080;

    // Allocate one display map
    ADLDisplayMap* pMap = static_cast<ADLDisplayMap*>(g_mallocCb(sizeof(ADLDisplayMap)));
    if (!pMap)
    {
        *lpNumDisplayMap = 0; *lppDisplayMap = nullptr;
        *lpNumDisplayTarget = 0; *lppDisplayTarget = nullptr;
        return ADL_ERR;
    }
    memset(pMap, 0, sizeof(ADLDisplayMap));
    pMap->iDisplayMapIndex                            = 0;
    pMap->displayMode.iAdapterIndex                   = 0;
    pMap->displayMode.displayID.iDisplayLogicalIndex   = 0;
    pMap->displayMode.displayID.iDisplayPhysicalIndex  = 0;
    pMap->displayMode.displayID.iDisplayLogicalAdapterIndex  = 0;
    pMap->displayMode.displayID.iDisplayPhysicalAdapterIndex = 0;
    pMap->displayMode.iXPos        = 0;
    pMap->displayMode.iYPos        = 0;
    pMap->displayMode.iXRes        = screenW;
    pMap->displayMode.iYRes        = screenH;
    pMap->displayMode.iColourDepth = 32;
    pMap->displayMode.fRefreshRate = 60.0f;
    pMap->displayMode.iOrientation = 0;
    pMap->iNumDisplayTarget                    = 1;
    pMap->iFirstDisplayTargetArrayIndex        = 0;
    pMap->iDisplayMapMask                      = 0x0003; // connected + mapped
    pMap->iDisplayMapValue                     = 0x0003;

    // Allocate one display target
    ADLDisplayTarget* pTgt = static_cast<ADLDisplayTarget*>(g_mallocCb(sizeof(ADLDisplayTarget)));
    if (!pTgt)
    {
        *lpNumDisplayMap = 0; *lppDisplayMap = nullptr;
        *lpNumDisplayTarget = 0; *lppDisplayTarget = nullptr;
        return ADL_ERR;
    }
    memset(pTgt, 0, sizeof(ADLDisplayTarget));
    pTgt->displayID.iDisplayLogicalIndex          = 0;
    pTgt->displayID.iDisplayPhysicalIndex         = 0;
    pTgt->displayID.iDisplayLogicalAdapterIndex    = 0;
    pTgt->displayID.iDisplayPhysicalAdapterIndex   = 0;
    pTgt->iDisplayMapIndex     = 0;
    pTgt->iDisplayTargetMask   = 0x0001; // connected
    pTgt->iDisplayTargetValue  = 0x0001;

    *lpNumDisplayMap    = 1;
    *lppDisplayMap      = pMap;
    *lpNumDisplayTarget = 1;
    *lppDisplayTarget   = pTgt;

    char logbuf[160];
    wsprintfA(logbuf, "[AmdAdlProxy] ADL_Display_DisplayMapConfig_Get -> 1 map (%dx%d), 1 target\n",
              screenW, screenH);
    WriteLog(logbuf);
    return ADL_OK;
}

extern "C" int __cdecl ADL_Graphics_Versions_Get(ADLVersionsInfo* lpVersionsInfo)
{
    WriteLog("[AmdAdlProxy] ADL_Graphics_Versions_Get\n");
    if (!lpVersionsInfo) return ADL_ERR_INVALID_PARAM;
    memset(lpVersionsInfo, 0, sizeof(ADLVersionsInfo));
    strcpy_s(lpVersionsInfo->strDriverVer, "23.20.16041.5");
    strcpy_s(lpVersionsInfo->strCatalystVersion, "15.201.1151");
    strcpy_s(lpVersionsInfo->strCatalystWebLink, "http://support.amd.com");
    return ADL_OK;
}

extern "C" int __cdecl ADL_Display_EdidData_Get(int iAdapterIndex, int iDisplayIndex, void* lpEDIDData)
{
    WriteLog("[AmdAdlProxy] ADL_Display_EdidData_Get\n");
    return ADL_ERR_NOT_SUPPORTED;
}

// ---- ADL2 context-based API (used by newer games) ---------------------------

extern "C" int __cdecl ADL2_Main_Control_Create(ADL_MAIN_MALLOC_CALLBACK callback, int iEnumConnectedAdapters, void** context)
{
    WriteLog("[AmdAdlProxy] ADL2_Main_Control_Create\n");
    g_mallocCb = callback;
    g_bInitialized = true;
    if (context) *context = reinterpret_cast<void*>(1); // fake context
    return ADL_OK;
}

extern "C" int __cdecl ADL2_Main_Control_Destroy(void* context)
{
    WriteLog("[AmdAdlProxy] ADL2_Main_Control_Destroy\n");
    g_bInitialized = false;
    return ADL_OK;
}

extern "C" int __cdecl ADL2_Adapter_NumberOfAdapters_Get(void* context, int* lpNumAdapters)
{
    WriteLog("[AmdAdlProxy] ADL2_Adapter_NumberOfAdapters_Get\n");
    if (!lpNumAdapters) return ADL_ERR_INVALID_PARAM;
    *lpNumAdapters = 1;
    return ADL_OK;
}

extern "C" int __cdecl ADL2_Adapter_AdapterInfo_Get(void* context, AdapterInfo* lpInfo, int iInputSize)
{
    WriteLog("[AmdAdlProxy] ADL2_Adapter_AdapterInfo_Get\n");
    return ADL_Adapter_AdapterInfo_Get(lpInfo, iInputSize);
}

extern "C" int __cdecl ADL2_Adapter_Active_Get(void* context, int iAdapterIndex, int* lpStatus)
{
    WriteLog("[AmdAdlProxy] ADL2_Adapter_Active_Get\n");
    return ADL_Adapter_Active_Get(iAdapterIndex, lpStatus);
}

// ---- DllMain ----------------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hSelf = hInst;
        DisableThreadLibraryCalls(hInst);
        WriteLog("[AmdAdlProxy] DLL_PROCESS_ATTACH (wiz3D " DISPLAYED_VERSION ")\n");
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        WriteLog("[AmdAdlProxy] DLL_PROCESS_DETACH\n");
    }
    return TRUE;
}
