#include "EngineCamera.h"
#include "CameraMath.h"
#include "SceneCache.h"
#include "GameplayCamera.h"
#include "ScriptedOrbit.h"
#include "AutoView.h"
#include "HeadAim.h"
#include <MinHook.h>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <cwchar>
#include <intrin.h>
#include <Xinput.h>
namespace EngineCamera {
namespace {
std::once_flag once;
std::atomic<uint64_t> frame{},created{},drawn{};
std::atomic<int> budget{32};
std::mutex logMutex;
std::mutex cameraMutex;
Transport::Header* channel{};
Transport::TrackingReader reader;
Transport::Tracking nextTracking{};
Transport::Pose reference{};
bool requested=false,referenceValid=false,f6Down=false,f7Down=false,f9Down=false,firstPerson=false;
uintptr_t gameBase{};
using CameraTick=void(__thiscall*)(void*,float,uint32_t);
CameraTick explorationOriginal{},aimOriginal{};
using TargetPosition=float*(__cdecl*)(float*,void*,int);
TargetPosition targetPosition{};
Transport::Vector playerPosition{};
uint64_t playerSampleTime{};
std::atomic<uint64_t> firstPersonScenes{},playerSamples{};
bool firstPersonHooks=false;
bool headAim=false,headAimSupported=false;
using AimGet=float(__thiscall*)(void*);
using AimSet=void(__thiscall*)(void*,float,float,int);
AimGet aimYaw{},aimPitch{};
AimSet setAimYaw{},setAimPitch{};
std::atomic<uint64_t> headAimUpdates{};
float lastHeadYaw{},lastHeadPitch{};
float hudScale=1.f;
bool disableScriptedCamera=false;
bool autoSwitch=true,cinematicReads=false,cutscenesInVR=false;
AutoView autoView;
uint64_t worldSceneTime{};
std::atomic<uint64_t> movieFrameTime{};
using MovieFrame=int(__stdcall*)(void*);
MovieFrame movieFrameOriginal{};
int __stdcall MovieFrameHook(void* movie) {
    movieFrameTime=GetTickCount64();return movieFrameOriginal(movie);
}
// Mirror the native predicates 5137d0/515c70 without calling game code
// from the presentation thread. Only read the main cinematic handler.
bool CinematicActive() {
    if(!cinematicReads)return false;
    __try {
        auto handler=*reinterpret_cast<unsigned char**>(gameBase+0x1ddbf7c);
        return handler && *reinterpret_cast<uintptr_t*>(handler)==gameBase+0x9646a0 &&
            handler[0x10d] && *reinterpret_cast<int*>(handler+0x2c)!=-1;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}

GameplayCamera gameplayView;
using BuildCamera=void(__thiscall*)(void*,void*);
BuildCamera buildCameraOriginal{};
std::atomic<uint64_t> gameplayViews{},scriptedViews{};
uint64_t nativeTurnTime{},aimTickTime{},inputTime{};
float nativeOrientation[4]{};
bool nativeOrientationValid=false;
float fallbackYaw{},fallbackPitch{};
std::atomic<long> mouseX{},mouseY{};
std::atomic<uint64_t> fallbackMoves{},aimUiMatrices{};
using RawInput=UINT(WINAPI*)(HRAWINPUT,UINT,LPVOID,PUINT,UINT);
RawInput rawInputOriginal{};
using PadState=DWORD(WINAPI*)(DWORD,XINPUT_STATE*);
PadState padState{};
bool GameFocused(){DWORD pid{};GetWindowThreadProcessId(GetForegroundWindow(),&pid);return pid==GetCurrentProcessId();}
UINT WINAPI RawInputHook(HRAWINPUT input,UINT command,LPVOID data,PUINT size,UINT header) {
    UINT result=rawInputOriginal(input,command,data,size,header);
    if(command==RID_INPUT && data && result!=UINT(-1) && result>=sizeof(RAWINPUTHEADER)+sizeof(RAWMOUSE) && GameFocused()) {
        auto raw=static_cast<RAWINPUT*>(data);
        if(raw->header.dwType==RIM_TYPEMOUSE && !(raw->data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)) {
            mouseX.fetch_add(raw->data.mouse.lLastX);mouseY.fetch_add(raw->data.mouse.lLastY);
        }
    }
    return result;
}
void PollScriptedOrbit() {
    const auto now=GetTickCount64();float dt=inputTime?float(now-inputTime)*.001f:0.f;inputTime=now;
    dt=std::clamp(dt,0.f,.05f);
    long dx=mouseX.exchange(0),dy=mouseY.exchange(0);
    if(!disableScriptedCamera || !requested || !nextTracking.valid || !gameplayView.valid || !GameFocused()) {fallbackYaw=fallbackPitch=0;return;}
    // Native orbit wins whenever it is updating. Only take over a stalled view.
    if(now-nativeTurnTime<150)return;
    float yaw=float(dx)*.0014f,pitch=float(dy)*.0014f;
    if(padState)for(DWORD i=0;i<4;i++) {
        XINPUT_STATE pad{};if(padState(i,&pad)!=ERROR_SUCCESS)continue;
        auto axis=[](SHORT x){float v=x/32767.f;return std::abs(v)>.24f?std::copysign((std::abs(v)-.24f)/.76f,v):0.f;};
        yaw+=axis(pad.Gamepad.sThumbRX)*1.8f*dt;
        pitch-=axis(pad.Gamepad.sThumbRY)*1.8f*dt;break;
    }
    yaw=std::clamp(yaw,-.35f,.35f);pitch=std::clamp(pitch,-.35f,.35f);
    if(yaw || pitch){fallbackYaw+=yaw;fallbackPitch=std::clamp(fallbackPitch+pitch,-1.3f,1.3f);++fallbackMoves;}
}
uint32_t playerIndex{},playerGeneration{};
bool playerHandleValid=false;
void LoadConfig() {
    wchar_t path[MAX_PATH]{},value[64]{};
    GetFullPathNameW(L"TombRaiderVR.ini",MAX_PATH,path,nullptr);
    GetPrivateProfileStringW(L"VR",L"HUDScale",L"1.0",value,64,path);
    wchar_t* end{};float parsed=wcstof(value,&end);
    hudScale=end!=value && !*end && std::isfinite(parsed)?std::clamp(parsed,.25f,2.f):1.f;
    headAim=GetPrivateProfileIntW(L"VR",L"HeadAim",0,path)!=0;
    cutscenesInVR=GetPrivateProfileIntW(L"VR",L"InGameCutscenesInVR",0,path)!=0;
    autoSwitch=GetPrivateProfileIntW(L"VR",L"AutoSwitchVR",1,path)!=0;
    disableScriptedCamera=GetPrivateProfileIntW(L"VR",L"DisableScriptedCamera",0,path)!=0;
}
// The supported executable resolves controller +1c8/+1cc through the
// 3000-entry instance pool. Position helper 91a040 handles transformed roots.
// Keep optional reads isolated from C++ stack unwinding and fail closed.
bool ReadPlayerHandle(uint32_t index,uint32_t generation,Transport::Vector& position) {
    __try {
        if(!(index|generation) || index>=3000)return false;
        auto instance=reinterpret_cast<unsigned char*>(gameBase+0x1d249e0+index*0xd0);
        if(*reinterpret_cast<uint32_t*>(instance+0xc8)!=index || *reinterpret_cast<uint32_t*>(instance+0xcc)!=generation)return false;
        alignas(16) float root[4]{};
        targetPosition(root,instance,0);
        for(int i=0;i<3;i++)if(!std::isfinite(root[i]) || std::abs(root[i])>1.e7f)return false;
        position={root[0],root[1],root[2]};return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
void SamplePlayer(void* self) {
    Transport::Vector position;
    auto c=static_cast<unsigned char*>(self);
    auto index=*reinterpret_cast<uint32_t*>(c+0x1c8),generation=*reinterpret_cast<uint32_t*>(c+0x1cc);
    if(!ReadPlayerHandle(index,generation,position))return;
    std::lock_guard lock(cameraMutex);
    if(index!=playerIndex || generation!=playerGeneration)gameplayView.valid=false;
    playerIndex=index;playerGeneration=generation;playerHandleValid=true;
    playerPosition=position;playerSampleTime=GetTickCount64();++playerSamples;
}
void __fastcall ExplorationHook(void* self,void*,float dt,uint32_t arg) {
    SamplePlayer(self);explorationOriginal(self,dt,arg);SamplePlayer(self);
}
void __fastcall AimHook(void* self,void*,float dt,uint32_t arg) {
    {std::lock_guard lock(cameraMutex);aimTickTime=GetTickCount64();}
    SamplePlayer(self);
    {
        std::lock_guard lock(cameraMutex);
        if(headAim && headAimSupported && requested && nextTracking.valid && referenceValid &&
           GameFocused() && !CinematicActive() && dt>0 && dt<.25f &&
           playerSampleTime && GetTickCount64()-playerSampleTime<100) {
            float yaw=aimYaw(self),pitch=aimPitch(self);
            auto c=static_cast<unsigned char*>(self);
            float low=*reinterpret_cast<float*>(c+0x1d8),high=*reinterpret_cast<float*>(c+0x1d4);
            if(std::isfinite(yaw) && std::isfinite(pitch) && std::isfinite(low) && std::isfinite(high) &&
               low<=high && low>=-3.2f && high<=3.2f) {
                const auto result=HeadAim::Steer(yaw,pitch,low,high,reference,nextTracking.head);
                setAimYaw(self,result.yaw,0,0);setAimPitch(self,result.pitch,0,0);
                reference=result.reference;
                lastHeadYaw=result.yaw;lastHeadPitch=result.pitch;++headAimUpdates;
            }
        }
    }
    aimOriginal(self,dt,arg);SamplePlayer(self);
}
void __fastcall BuildCameraHook(void* self,void*,void* effects) {
    auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress())-gameBase;
    {
        std::lock_guard lock(cameraMutex);
        if(disableScriptedCamera && requested && nextTracking.valid && playerHandleValid) {
            Transport::Vector root;
            if(ReadPlayerHandle(playerIndex,playerGeneration,root)) {
                // 4f69a0 builds the render matrices from native position +80
                // and quaternion +90. Keep native coordinates here; the engine
                // performs its own basis conversion. +a0/+a4 are FOV/distance.
                auto state=reinterpret_cast<float*>(static_cast<unsigned char*>(self)+0x80);
                if(caller==0x520dad || caller==0x51e858) {
                    // Exploration/aim updates include native mouse/stick orbit.
                    float difference=0,sameSign=0;
                    for(int i=0;i<4;i++){difference+=std::abs(state[4+i]-nativeOrientation[i]);sameSign+=std::abs(state[4+i]+nativeOrientation[i]);}
                    if(!nativeOrientationValid || (difference>.00001f && sameSign>.00001f)) {
                        nativeTurnTime=GetTickCount64();fallbackYaw=fallbackPitch=0;
                        memcpy(nativeOrientation,state+4,sizeof(nativeOrientation));nativeOrientationValid=true;
                    }
                    if(gameplayView.Update(state,root))++gameplayViews;
                } else if((caller==0xf8660 || caller==0xf80e9) && gameplayView.valid) {
                    // Final camera-stack blend. Replace scripted selection with
                    // the latest gameplay view, not a frozen heading. If that
                    // controller stops ticking, follow the validated actor root.
                    gameplayView.Apply(state,root);
                    OrbitScriptedView(state,root,fallbackYaw,fallbackPitch);++scriptedViews;
                }
                // This argument selects the shake/rotation/FOV effects mixer.
                effects=nullptr;
            } else {playerHandleValid=false;gameplayView.valid=false;}
        }
    }
    buildCameraOriginal(self,effects);
}
struct Snapshot {Transport::Tracking tracking{};bool active=false;bool ui=false;bool aimUi=false;CameraMath::Matrix aimWorld{},headWorld{};float aimFov{},aimAspect{};};
Snapshot aimingView;
uint64_t aimingViewTime{};
SceneCache<Snapshot> scenes;
thread_local Snapshot drawing;
thread_local unsigned eyeMask=0;
Transport::RenderInfo pairInfo{};
using Stereo=void(__cdecl*)(float*,bool,float,float);
Stereo stereoOriginal{};
using Matrices=void(__thiscall*)(void*,int);
Matrices matricesOriginal{};
std::atomic<uint64_t> uiMatrices{};
using Create=void*(__thiscall*)(void*,void*,void*,void*,void*,void*,uint32_t);
using Draw=void(__thiscall*)(void*,uint32_t,void*);
Create createOriginal{};
Draw drawOriginal{};
void* __fastcall CreateHook(void* self,void*,void* viewport,void* target,void* depth,void* source,void* sourceDepth,uint32_t flags) {
    Snapshot snap;
    alignas(16) unsigned char adjusted[0x100];
    {
        std::lock_guard lock(cameraMutex);
        if(viewport && !target) {
            auto v=static_cast<float*>(viewport);
            if(v[10]>.1f && v[10]<3.f && v[8]>0 && v[9]>1000)worldSceneTime=GetTickCount64();
        }
        if(viewport && requested && nextTracking.valid) {
            auto v=static_cast<float*>(viewport);
            if(v[10]<=0 && !target) {
                snap={nextTracking,false,true};
                if(GetTickCount64()-aimTickTime<100 && GetTickCount64()-aimingViewTime<100) {
                    snap.aimUi=true;snap.aimWorld=aimingView.aimWorld;snap.headWorld=aimingView.headWorld;
                    snap.aimFov=aimingView.aimFov;snap.aimAspect=aimingView.aimAspect;snap.tracking=aimingView.tracking;
                }
            }
            // Start with perspective world scenes only. Orthographic menu/UI
            // scenes retain their native viewport; F6 returns the whole frame
            // to the confirmed stereo screen while menu classification develops.
            if(std::isfinite(v[10]) && v[10]>.1f && v[10]<3.f && v[8]>0 && v[9]>1000 && !target) {
                memcpy(adjusted,viewport,sizeof(adjusted));auto out=reinterpret_cast<float*>(adjusted);
                auto world=CameraMath::Load(v+16);
                auto gameWorld=world;
                if(disableScriptedCamera && playerHandleValid) {
                    if(ReadPlayerHandle(playerIndex,playerGeneration,playerPosition))playerSampleTime=GetTickCount64();
                    else {playerHandleValid=false;gameplayView.valid=false;}
                }
                if(firstPerson && playerSampleTime && GetTickCount64()-playerSampleTime<100) {
                    float dx=world.m[12]-playerPosition.x,dy=world.m[13]-playerPosition.y,dz=world.m[14]-playerPosition.z;
                    // Reject unrelated distant scenes; stale controller samples
                    // fall back to the game's camera during other camera modes.
                    if(dx*dx+dy*dy+dz*dz<1500.f*1500.f) {
                        float length=std::hypot(world.m[8],world.m[9]);
                        world.m[12]=playerPosition.x+(length>.001f?10.f*world.m[8]/length:0.f);
                        world.m[13]=playerPosition.y+(length>.001f?10.f*world.m[9]/length:0.f);
                        world.m[14]=playerPosition.z+160.f;
                        out[8]=(std::min)(out[8],5.f);++firstPersonScenes;
                    }
                }
                for(int i=4;i<7;i++)world.m[i]=-world.m[i];
                if(!referenceValid){reference=nextTracking.head;referenceValid=true;}
                // Apply headset rotation after selecting the camera origin.
                world=CameraMath::HeadWorld(world,reference,nextTracking.head,0.f);
                for(int i=4;i<7;i++)world.m[i]=-world.m[i];
                memcpy(out+16,world.m,64);
                if(GetTickCount64()-aimTickTime<100) {
                    aimingView.aimWorld=gameWorld;aimingView.headWorld=world;
                    aimingView.aimFov=v[10];aimingView.aimAspect=v[11];aimingView.tracking=nextTracking;aimingViewTime=GetTickCount64();
                }
                float x=0,y=0;
                for(const auto& e:nextTracking.eyes){x=(std::max)(x,(std::max)(std::abs(std::tan(e.left)),std::abs(std::tan(e.right))));y=(std::max)(y,(std::max)(std::abs(std::tan(e.up)),std::abs(std::tan(e.down))));}
                if(x>0 && y>0){out[10]=2*std::atan(y);out[11]=x/y;viewport=adjusted;snap={nextTracking,true};}
            }
        }
    }
    void* scene=createOriginal(self,viewport,target,depth,source,sourceDepth,flags);
    {std::lock_guard lock(cameraMutex);if(scene && (snap.active||snap.ui))scenes.Store(scene,snap,frame.load()+1);else scenes.Erase(scene);}
    ++created;
    if(viewport && budget.load()>0 && budget.fetch_sub(1)>0) {
        std::lock_guard lock(logMutex);
        FILE* f{};
        if(!fopen_s(&f,"TombRaiderVR-camera.log","a")) {
            // Validated at 0x6012d0: 0x100-byte viewport, matrix at +0x40.
            auto* v=static_cast<float*>(viewport);
            fprintf(f,"scene frame=%llu ptr=%p target=%p flags=%08x near=%g far=%g fov=%g aspect=%g matrix=",
                frame.load(),scene,target,flags,v[8],v[9],v[10],v[11]);
            for(int i=16;i<32;i++)fprintf(f,"%g,",v[i]);
            fprintf(f,"\n");fclose(f);
        }
    }
    return scene;
}
void __fastcall DrawHook(void* self,void*,uint32_t pass,void* other) {
    auto saved=drawing;auto savedMask=eyeMask;
    {std::lock_guard lock(cameraMutex);drawing=scenes.Find(static_cast<unsigned char*>(self)-4);}
    eyeMask=0;++drawn;drawOriginal(self,pass,other);
    if(drawing.active && eyeMask) {
        std::lock_guard lock(cameraMutex);
        if(!pairInfo.mode){pairInfo.mode=1;pairInfo.tracking=drawing.tracking;}
        if(pairInfo.tracking.id!=drawing.tracking.id)pairInfo.mode=2;
        pairInfo.eyeMask|=eyeMask;
    }
    drawing=saved;eyeMask=savedMask;
}
void __cdecl StereoHook(float* projection,bool firstEye,float width,float plane) {
    if(drawing.active && std::abs(projection[11]-1.f)<.001f && std::abs(projection[15])<.001f) {
        unsigned eye=firstEye?0:1;
        auto p=CameraMath::EyeProjection(CameraMath::Load(projection),drawing.tracking,eye,100.f);
        memcpy(projection,p.m,64);eyeMask|=1u<<eye;
    } else stereoOriginal(projection,firstEye,width,plane);
}
void __fastcall MatricesHook(void* self,void*,int force) {
    if(!drawing.ui){matricesOriginal(self,force);return;}
    auto state=static_cast<unsigned char*>(self);
    // Verified 0x638a90: projection override +0xa90, fallback +0x990,
    // projection dirty +0xb62, native stereo enable/eye +0xc19/+0xc1a.
    auto& overrideMatrix=*reinterpret_cast<float**>(state+0xa90);
    auto saved=overrideMatrix;auto stereo=state[0xc19];
    auto source=CameraMath::Load(saved?saved:reinterpret_cast<float*>(state+0x990));
    auto eye=state[0xc1a]?0:1;
    auto uiProjection=drawing.aimUi?CameraMath::AimClipTransform(drawing.tracking,eye,drawing.aimWorld,drawing.headWorld,drawing.aimFov,drawing.aimAspect):CameraMath::HudClipTransform(drawing.tracking,eye,hudScale);
    if(drawing.aimUi)++aimUiMatrices;
    auto projected=CameraMath::Multiply(source,uiProjection);
    overrideMatrix=projected.m;state[0xc19]=0;state[0xb62]=1;
    matricesOriginal(self,force);
    overrideMatrix=saved;state[0xc19]=stereo;
    // Force the next world/UI transition to rebuild from the restored source.
    state[0xb62]=1;++uiMatrices;
}
void Install() {
    LoadConfig();
    auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    gameBase=base;
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
    auto create=reinterpret_cast<void*>(base+0x21c8b0);
    auto draw=reinterpret_cast<void*>(base+0x223bc0);
    auto stereo=reinterpret_cast<void*>(base+0x204df0);
    auto matrices=reinterpret_cast<void*>(base+0x238a90);
    const unsigned char stereoPrefix[]={0x55,0x8b,0xec,0x8b,0x45,0x08,0x0f,0x57,0xc0};
    const unsigned char prefix[]={0x53,0x8b,0xdc,0x83,0xec,0x08,0x83,0xe4,0xf0,0x83,0xc4,0x04};
    bool supported=nt->FileHeader.Machine==IMAGE_FILE_MACHINE_I386 &&
        nt->FileHeader.TimeDateStamp==0x632ce0fa && nt->OptionalHeader.SizeOfImage==0x24c2000;
    supported=supported && !memcmp(create,prefix,sizeof(prefix)) && !memcmp(draw,prefix,sizeof(prefix)) && !memcmp(matrices,prefix,sizeof(prefix)) && !memcmp(stereo,stereoPrefix,sizeof(stereoPrefix));
    bool ok=false;
    if(supported) {
        auto init=MH_Initialize();
        if(init==MH_OK || init==MH_ERROR_ALREADY_INITIALIZED) {
            bool madeCreate=MH_CreateHook(create,&CreateHook,reinterpret_cast<void**>(&createOriginal))==MH_OK;
            bool madeDraw=MH_CreateHook(draw,&DrawHook,reinterpret_cast<void**>(&drawOriginal))==MH_OK;
            bool madeStereo=MH_CreateHook(stereo,&StereoHook,reinterpret_cast<void**>(&stereoOriginal))==MH_OK;
            bool madeMatrices=MH_CreateHook(matrices,&MatricesHook,reinterpret_cast<void**>(&matricesOriginal))==MH_OK;
            ok=madeCreate && madeDraw && madeStereo && madeMatrices;
            if(ok)ok=MH_EnableHook(create)==MH_OK && MH_EnableHook(draw)==MH_OK && MH_EnableHook(stereo)==MH_OK && MH_EnableHook(matrices)==MH_OK;
            if(!ok && madeMatrices){MH_DisableHook(matrices);MH_RemoveHook(matrices);}
            if(!ok){if(madeCreate){MH_DisableHook(create);MH_RemoveHook(create);}if(madeDraw){MH_DisableHook(draw);MH_RemoveHook(draw);}if(madeStereo){MH_DisableHook(stereo);MH_RemoveHook(stereo);}}
        }
    }
    if(ok) {
        const unsigned char cinePrefix[]={0x55,0x8b,0xec,0x80,0xb9,0x0d,0x01,0,0,0,0x74,0x18,0x83,0x79,0x2c,0xff};
        cinematicReads=!memcmp(reinterpret_cast<void*>(base+0x115c70),cinePrefix,sizeof(cinePrefix));
        auto bink=GetModuleHandleW(L"binkw32.dll");
        auto movie=bink?GetProcAddress(bink,"_BinkDoFrame@4"):nullptr;
        if(movie && MH_CreateHook(movie,&MovieFrameHook,reinterpret_cast<void**>(&movieFrameOriginal))==MH_OK) {
            if(MH_EnableHook(movie)!=MH_OK){MH_RemoveHook(movie);movieFrameOriginal=nullptr;}
        }
        auto raw=GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetRawInputData");
        if(raw && MH_CreateHook(raw,&RawInputHook,reinterpret_cast<void**>(&rawInputOriginal))==MH_OK)MH_EnableHook(raw);
        auto pad=LoadLibraryExW(L"xinput1_4.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(pad)padState=reinterpret_cast<PadState>(GetProcAddress(pad,"XInputGetState"));
        auto build=reinterpret_cast<void*>(base+0xf69a0);
        const unsigned char buildPrefix[]={0x55,0x8b,0xec,0x56,0x8b,0xf1,0x57,0x8d,0x86,0x90,0,0,0};
        if(!memcmp(build,buildPrefix,sizeof(buildPrefix)) && MH_CreateHook(build,&BuildCameraHook,reinterpret_cast<void**>(&buildCameraOriginal))==MH_OK) {
            if(MH_EnableHook(build)!=MH_OK){MH_RemoveHook(build);disableScriptedCamera=false;}
        } else disableScriptedCamera=false;
        // Exact-build native aim accessors: getters interpolate current aim;
        // setters use radians, zero blend duration, and native pitch limits.
        const unsigned char yawGetPrefix[]={0x81,0xc1,0xf4,0x07,0,0,0xe9};
        const unsigned char pitchGetPrefix[]={0x55,0x8b,0xec,0x51,0x56,0x8b,0xf1,0x8b,0x86,0x14,0x08,0,0};
        const unsigned char yawSetPrefix[]={0x55,0x8b,0xec,0xff,0x75,0x10,0xf3,0x0f,0x10,0x45,0x0c,0x81,0xc1,0xf4,0x07,0,0};
        const unsigned char pitchSetPrefix[]={0x55,0x8b,0xec,0x51,0xf3,0x0f,0x10,0x45,0x08,0x56,0x8b,0xf1};
        headAimSupported=!memcmp(reinterpret_cast<void*>(base+0x5188c0),yawGetPrefix,sizeof(yawGetPrefix)) &&
            !memcmp(reinterpret_cast<void*>(base+0x5187e0),pitchGetPrefix,sizeof(pitchGetPrefix)) &&
            !memcmp(reinterpret_cast<void*>(base+0x523940),yawSetPrefix,sizeof(yawSetPrefix)) &&
            !memcmp(reinterpret_cast<void*>(base+0x523740),pitchSetPrefix,sizeof(pitchSetPrefix));
        if(headAimSupported){aimYaw=reinterpret_cast<AimGet>(base+0x5188c0);aimPitch=reinterpret_cast<AimGet>(base+0x5187e0);
            setAimYaw=reinterpret_cast<AimSet>(base+0x523940);setAimPitch=reinterpret_cast<AimSet>(base+0x523740);}
        auto exploration=reinterpret_cast<void*>(base+0x51f820);
        auto aim=reinterpret_cast<void*>(base+0x51e0f0);
        auto position=reinterpret_cast<void*>(base+0x51a040);
        const unsigned char tickPrefix[]={0x55,0x8b,0xec,0x83,0xe4,0xf0,0x81,0xec,0x28,0x01,0,0};
        if(!memcmp(exploration,tickPrefix,sizeof(tickPrefix)) && !memcmp(aim,prefix,sizeof(prefix)) && !memcmp(position,prefix,sizeof(prefix))) {
            targetPosition=reinterpret_cast<TargetPosition>(position);
            bool a=MH_CreateHook(exploration,&ExplorationHook,reinterpret_cast<void**>(&explorationOriginal))==MH_OK;
            bool b=MH_CreateHook(aim,&AimHook,reinterpret_cast<void**>(&aimOriginal))==MH_OK;
            firstPersonHooks=a && b && MH_EnableHook(exploration)==MH_OK && MH_EnableHook(aim)==MH_OK;
            if(!firstPersonHooks){if(a){MH_DisableHook(exploration);MH_RemoveHook(exploration);}if(b){MH_DisableHook(aim);MH_RemoveHook(aim);}}
        }
    }
    FILE* f{};if(!fopen_s(&f,"TombRaiderVR-camera.log","a")){fprintf(f,"HeadAim=%d supported=%d aimUI=native-controller-state\n",headAim,headAimSupported && firstPersonHooks);fclose(f);}
    if(!fopen_s(&f,"TombRaiderVR-camera.log","a")) {
        fprintf(f,"TombRaiderVR look-around prototype: supported=%d sceneHooks=%d; starts in stereo screen, F6 toggles tracking, F9 recenters\n",supported,ok);fclose(f);
    }
    if(!fopen_s(&f,"TombRaiderVR-camera.log","a")){fprintf(f,"F7 first-person prototype: hooks=%d eyeHeight=160 forwardOffset=10\n",firstPersonHooks);fclose(f);}
    if(!fopen_s(&f,"TombRaiderVR-camera.log","a")){fprintf(f,"Settings HUDScale=%g DisableScriptedCamera=%d (native gameplay orbit retained)\n",hudScale,disableScriptedCamera);fclose(f);}
}
}
void SetChannel(Transport::Header* value) {std::lock_guard lock(cameraMutex);channel=value;if(!value){nextTracking={};referenceValid=false;}}
Transport::RenderInfo OnPresent(uint64_t id, bool capture) {
    std::call_once(once,Install);frame=id;
    Transport::RenderInfo completed;
    {
        std::lock_guard lock(cameraMutex);completed=pairInfo;pairInfo={};scenes.Complete(id);
        const auto now=GetTickCount64();
        const auto movieTime=movieFrameTime.load();
        bool cinematic=CinematicActive();
        bool movie=movieTime && now-movieTime<1000;
        bool gameplay=playerSampleTime && now-playerSampleTime<250 && worldSceneTime && now-worldSceneTime<250;
        if(autoSwitch && cinematicReads && autoView.Update(now,gameplay,cinematic,requested,movie,cutscenesInVR)) {
            referenceValid=false;gameplayView.valid=false;nativeOrientationValid=false;fallbackYaw=fallbackPitch=0;
            FILE* f{};if(!fopen_s(&f,"TombRaiderVR-camera.log","a")) {
                fprintf(f,"automatic view: state=%d immersive=%d cinematic=%d frame=%llu\n",int(autoView.state),requested,cinematic,id);fclose(f);
            }
        }
        if(capture){FILE* f{};if(!fopen_s(&f,"TombRaiderVR-camera.log","a")) {
            fprintf(f,"autoSwitch=%d cinematicReads=%d movieHook=%d state=%d cinematic=%d gameplay=%d movie=%d InGameCutscenesInVR=%d\n",autoSwitch,cinematicReads,movieFrameOriginal!=nullptr,int(autoView.state),cinematic,gameplay,movie,cutscenesInVR);fclose(f);
        }}
        PollScriptedOrbit();
        bool f6=(GetAsyncKeyState(VK_F6)&0x8000)!=0,f7=(GetAsyncKeyState(VK_F7)&0x8000)!=0,f9=(GetAsyncKeyState(VK_F9)&0x8000)!=0;
        if(f6&&!f6Down){requested=!requested;referenceValid=false;gameplayView.valid=false;nativeOrientationValid=false;fallbackYaw=fallbackPitch=0;}
        if(f7&&!f7Down && firstPersonHooks){firstPerson=!firstPerson;requested=true;referenceValid=false;gameplayView.valid=false;nativeOrientationValid=false;fallbackYaw=fallbackPitch=0;}
        if(f9&&!f9Down)referenceValid=false;
        f6Down=f6;f7Down=f7;f9Down=f9;
        if(!reader.Read(channel,nextTracking,GetTickCount64())){nextTracking={};referenceValid=false;}
        if(capture){FILE* f{};if(!fopen_s(&f,"TombRaiderVR-camera.log","a")){fprintf(f,"HeadAim=%d supported=%d updates=%llu yaw=%g pitch=%g aimAge=%llu\n",headAim,headAimSupported,headAimUpdates.load(),lastHeadYaw,lastHeadPitch,aimTickTime?GetTickCount64()-aimTickTime:~0ull);fclose(f);}}
        if(capture){FILE* f{};if(!fopen_s(&f,"TombRaiderVR-camera.log","a")){fprintf(f,"tracking requested=%d valid=%u completedMode=%u eyes=%u pose=%llu\n",requested,nextTracking.valid,completed.mode,completed.eyeMask,completed.tracking.id);fclose(f);}}
        if(capture){FILE* f{};if(!fopen_s(&f,"TombRaiderVR-camera.log","a")){fprintf(f,"firstPerson=%d hooks=%d samples=%llu scenes=%llu sampleAgeMs=%llu root=%g,%g,%g\n",firstPerson,firstPersonHooks,playerSamples.load(),firstPersonScenes.load(),playerSampleTime?GetTickCount64()-playerSampleTime:~0ull,playerPosition.x,playerPosition.y,playerPosition.z);fclose(f);}}
    }
    if(capture){budget=64;FILE* f{};if(!fopen_s(&f,"TombRaiderVR-camera.log","a")){fprintf(f,"capture frame=%llu created=%llu drawn=%llu uiMatrices=%llu gameplayViews=%llu scriptedViews=%llu fallbackMoves=%llu aimUiMatrices=%llu\n",id,created.load(),drawn.load(),uiMatrices.load(),gameplayViews.load(),scriptedViews.load(),fallbackMoves.load(),aimUiMatrices.load());fclose(f);}}
    return completed;
}
}
