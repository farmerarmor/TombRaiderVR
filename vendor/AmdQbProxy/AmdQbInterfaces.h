// AmdQbInterfaces.h
// Minimal AMD QBS interface declarations needed to IMPLEMENT the fake atidxx32/64.dll.
// We intentionally omit the AmdDxExtCreate / AmdDxExtCreate11 function declarations
// that appear in AmdDxExtApi.h so that our extern "C" exports in AmdQbProxy.cpp do not
// cause a C2732 linkage-specification conflict.

#ifndef AMDQBINTERFACES_H
#define AMDQBINTERFACES_H

#include <dxgi.h>

// ---------------------------------------------------------------------------
// From AmdDxExt.h
// ---------------------------------------------------------------------------
enum AmdDxExtPrimitiveTopology
{
    AmdDxExtPrimitiveTopology_Undefined         = 0,
    AmdDxExtPrimitiveTopology_PointList         = 1,
    AmdDxExtPrimitiveTopology_LineList          = 2,
    AmdDxExtPrimitiveTopology_LineStrip         = 3,
    AmdDxExtPrimitiveTopology_TriangleList      = 4,
    AmdDxExtPrimitiveTopology_TriangleStrip     = 5,
    AmdDxExtPrimitiveTopology_ExtQuadList       = 7, // forgot to add this earlier, if there's errors investigate
    AmdDxExtPrimitiveTopology_RectList          = 8, // AmdDxExtPrimitiveTopology_ExtPatch
    AmdDxExtPrimitiveTopology_LineListAdj       = 10,
    AmdDxExtPrimitiveTopology_LineStripAdj      = 11,
    AmdDxExtPrimitiveTopology_TriangleListAdj   = 12,
    AmdDxExtPrimitiveTopology_TriangleStripAdj  = 13,
    AmdDxExtPrimitiveTopology_Max               = 14
};

// ---------------------------------------------------------------------------
// From AmdDxExtIface.h — base interface (AddRef / Release)
// ---------------------------------------------------------------------------
class IAmdDxExtInterface
{
public:
    virtual unsigned int AddRef(void)  = 0;
    virtual unsigned int Release(void) = 0;

protected:
    IAmdDxExtInterface() {}
    virtual ~IAmdDxExtInterface() = 0 {} // Might drop the "= 0" to satisfy modern C++ standards
};

// ---------------------------------------------------------------------------
// From AmdDxExtApi.h — version struct + main extension interface
// ---------------------------------------------------------------------------
struct AmdDxExtVersion
{
    unsigned int majorVersion;
    unsigned int minorVersion;
};

// Forward declarations matching the AMD SDK (we only use pointers)
interface ID3D10Device;
interface ID3D11Device;
interface ID3D10Resource;
interface ID3D11Resource;

class IAmdDxExt : public IAmdDxExtInterface
{
public:
    virtual HRESULT             GetVersion(AmdDxExtVersion* pExtVer) = 0;
    virtual IAmdDxExtInterface* GetExtInterface(unsigned int iface) = 0;

    virtual HRESULT IaSetPrimitiveTopology(unsigned int topology) = 0;
    virtual HRESULT IaGetPrimitiveTopology(AmdDxExtPrimitiveTopology* pExtTopology) = 0;
    virtual HRESULT SetSingleSampleRead(ID3D10Resource* pResource, BOOL singleSample) = 0;
    virtual HRESULT SetSingleSampleRead11(ID3D11Resource* pResource, BOOL singleSample) = 0;

protected:
    IAmdDxExt() {}
    virtual ~IAmdDxExt() = 0 {} // Again might drop the "= 0" to meet modern C++
};

// ---------------------------------------------------------------------------
// From AmdDxExtQbStereoApi.h — quad-buffer stereo interface
// ---------------------------------------------------------------------------
const unsigned int AmdDxExtQuadBufferStereoID = 2;

class IAmdDxExtQuadBufferStereo : public IAmdDxExtInterface
{
public:
    virtual HRESULT EnableQuadBufferStereo(BOOL enable) = 0;

    virtual HRESULT GetDisplayModeList(DXGI_FORMAT EnumFormat,
                                       UINT Flags,
                                       UINT* pNumModes,
                                       DXGI_MODE_DESC* pDesc) = 0;

    virtual UINT GetLineOffset(IDXGISwapChain* pSwapChain) = 0;
};

#endif // AMDQBINTERFACES_H
