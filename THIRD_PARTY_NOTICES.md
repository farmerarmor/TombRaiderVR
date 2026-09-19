# Third-party notices

The AMD stereo and D3D11 adapter proxy sources in `vendor` derive from [effcol/wiz3D](https://github.com/effcol/wiz3D), revision `981de7bf5414fad307754c6db03fd04970a25afb`. The upstream LGPL 2.1 license is retained as `LICENSE`, and the iZ3D notice is retained in `licenses/LICENSE-iZ3D.txt`. Original source notices remain in the files. This combined source project is distributed under LGPL 2.1; retain the corresponding modified source when distributing its binaries.

Local modifications replace the upstream stereo compositor with the TombRaiderVR transport, preserve original per-eye swapchain dimensions while providing a double-height native stereo buffer, avoid retaining discarded adapter-probe devices, and create DXGI 1.1 factories to permit keyed-mutex resources. The companion and transport sources are new code.

OpenXR SDK Source: KhronosGroup/OpenXR-SDK-Source revision `c07ad64839653712190e05dbd8cf460e1d239513`, Apache 2.0, notice in `licenses/OpenXR.txt`.

MinHook: TsudaKageyu/minhook revision `c3fcafdc10146beb5919319d0683e44e3c30d537`, BSD-style license, notice in `licenses/MinHook.txt`.

[rrika/cdcEngineDXHR](https://github.com/rrika/cdcEngineDXHR) was consulted as a reverse-engineering reference for engine class names and layouts. Camera addresses and layouts must be checked against the supported executable; the reference is not a drop-in SDK for this version.
