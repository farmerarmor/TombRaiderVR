#pragma once
#include "SharedPair.h"
#include <cmath>
#include <cstring>
namespace CameraMath {
using Transport::Pose;using Transport::Vector;using Transport::Quaternion;
struct alignas(16) Matrix {float m[16]{};};
inline Quaternion Inverse(Quaternion q){return {-q.x,-q.y,-q.z,q.w};}
inline Quaternion Multiply(Quaternion a,Quaternion b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
            a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
inline Vector Rotate(Quaternion q,Vector v) {
    auto r=Multiply(Multiply(q,{v.x,v.y,v.z,0}),Inverse(q));return {r.x,r.y,r.z};
}
inline Matrix Rotation(Quaternion q) {
    Matrix r;
    Vector axes[3]={{1,0,0},{0,1,0},{0,0,1}};
    for(int i=0;i<3;i++){auto v=Rotate(q,axes[i]);r.m[i*4]=v.x;r.m[i*4+1]=v.y;r.m[i*4+2]=v.z;}
    r.m[15]=1;return r;
}
inline Matrix Multiply(const Matrix& a,const Matrix& b) {
    Matrix r;for(int i=0;i<4;i++)for(int j=0;j<4;j++)for(int k=0;k<4;k++)r.m[i*4+j]+=a.m[i*4+k]*b.m[k*4+j];return r;
}
inline Matrix InverseRigid(const Matrix& a) {
    Matrix r;for(int i=0;i<3;i++)for(int j=0;j<3;j++)r.m[i*4+j]=a.m[j*4+i];
    for(int j=0;j<3;j++)for(int k=0;k<3;k++)r.m[12+j]-=a.m[12+k]*r.m[k*4+j];r.m[15]=1;return r;
}
inline Matrix HeadWorld(const Matrix& game,const Pose& reference,const Pose& head,float scale) {
    auto q=Multiply(Inverse(reference.orientation),head.orientation);
    // PlayerCamera coordinates are right/down/forward; OpenXR is right/up/back.
    q.y=-q.y;q.z=-q.z;
    auto result=Multiply(Rotation(q),game);
    auto delta=Rotate(Inverse(reference.orientation),{head.position.x-reference.position.x,head.position.y-reference.position.y,head.position.z-reference.position.z});
    for(int j=0;j<3;j++)result.m[12+j]+=scale*(delta.x*game.m[j]-delta.y*game.m[4+j]-delta.z*game.m[8+j]);
    return result;
}
// Remove native look pitch from a rendering copy only. The game's Z-up
// PlayerCamera uses right/down/forward axes; rotation about local right
// preserves its yaw, roll and position. OpenXR pitch is applied afterward.
inline Matrix WithoutLookPitch(const Matrix& game) {
    float pitch=std::atan2(game.m[10],-game.m[6]);
    return Multiply(Rotation({-std::sin(pitch*.5f),0,0,std::cos(pitch*.5f)}),game);
}
inline Matrix HorizontalDirection(const Matrix& source,const Matrix& fallback) {
    float x=source.m[8],y=source.m[9];
    float length=std::hypot(x,y);
    // At a vertical aim, the right axis still supplies a usable heading.
    if(length<.001f){x=-source.m[1];y=source.m[0];length=std::hypot(x,y);}
    if(length<.001f){x=fallback.m[8];y=fallback.m[9];length=std::hypot(x,y);}
    if(length<.001f){x=0;y=1;length=1;}
    x/=length;y/=length;
    Matrix result{{y,-x,0,0, 0,0,-1,0, x,y,0,0, fallback.m[12],fallback.m[13],fallback.m[14],1}};
    return result;
}
inline float HeadingDelta(const Matrix& from,const Matrix& to) {
    auto a=HorizontalDirection(from,from),b=HorizontalDirection(to,from);
    return std::atan2(a.m[8]*b.m[9]-a.m[9]*b.m[8],a.m[8]*b.m[8]+a.m[9]*b.m[9]);
}
struct MovementAxes {float strafe,walk;};
inline MovementAxes ReorientMovement(float strafe,float walk,const Matrix& native,const Matrix& target) {
    // Native locomotion actions use the opposite yaw sense to the camera's
    // right/forward basis. Direct world-vector projection mirrored the turn:
    // mouse north + head west sent W east. Rotate in native input coordinates.
    float yaw=HeadingDelta(native,target),c=std::cos(yaw),s=std::sin(yaw);
    return {c*strafe+s*walk,-s*strafe+c*walk};
}
inline Matrix ControllerMuzzle(const Matrix& base,const Pose& reference,const Pose& aim,float scale,float muzzleForward) {
    auto hand=HeadWorld(base,reference,aim,scale);
    Matrix muzzle=hand;
    // Native muzzle firing direction is -Y (callers at 0x75313c/0x6df718).
    // Its top is +Z. PlayerCamera's second row points DOWN, so the first
    // experiment's [right,-forward,down] basis rolled the gun upside down.
    for(int j=0;j<3;j++) {
        muzzle.m[j]=-hand.m[j];
        muzzle.m[4+j]=-hand.m[8+j];muzzle.m[8+j]=-hand.m[4+j];
        muzzle.m[12+j]+=hand.m[8+j]*muzzleForward*scale;
    }
    return muzzle;
}
inline Matrix FiringFromMuzzle(const Matrix& muzzle) {
    auto firing=muzzle;
    // Native 0x750dc0 converts muzzle -Y to the +Z row consumed by the
    // player's spread/raycast calculation (0x763370 -> 0x762e80).
    for(int j=0;j<3;j++) {
        firing.m[4+j]=muzzle.m[8+j];firing.m[8+j]=-muzzle.m[4+j];
    }
    return firing;
}
inline Matrix MoveSkinMatrix(const Matrix& skin,const Matrix& delta) {
    // 0x538160 stores skinning translation with w=0, despite the shader
    // using it as a POSITION. Ordinary 4x4 multiplication loses delta's
    // translation for these bones, leaving animated weapon parts behind.
    auto affine=skin;affine.m[3]=affine.m[7]=affine.m[11]=0;affine.m[15]=1;
    auto moved=Multiply(affine,delta);
    for(int i:{3,7,11,15})moved.m[i]=skin.m[i];
    return moved;
}
inline Matrix EyeWorld(const Transport::Tracking& t,unsigned eye,float scale) {
    const auto& e=t.eyes[eye];
    auto q=Multiply(Inverse(t.head.orientation),e.pose.orientation);
    auto pos=Rotate(Inverse(t.head.orientation),{e.pose.position.x-t.head.position.x,e.pose.position.y-t.head.position.y,e.pose.position.z-t.head.position.z});
    // RenderViewport coordinates are left-handed right/up/forward.
    auto eyeWorld=Rotation({-q.x,-q.y,q.z,q.w});
    eyeWorld.m[12]=pos.x*scale;eyeWorld.m[13]=pos.y*scale;eyeWorld.m[14]=-pos.z*scale;
    return eyeWorld;
}
inline Matrix EyeFrustum(const Matrix& original,const Transport::Tracking& t,unsigned eye) {
    const auto& e=t.eyes[eye];
    float l=std::tan(e.left),r=std::tan(e.right),u=std::tan(e.up),d=std::tan(e.down);
    Matrix p;p.m[0]=2/(r-l);p.m[5]=2/(u-d);p.m[8]=-(r+l)/(r-l);p.m[9]=-(u+d)/(u-d);
    p.m[10]=original.m[10];p.m[11]=1;p.m[14]=original.m[14];
    return p;
}
inline Matrix EyeProjection(const Matrix& original,const Transport::Tracking& t,unsigned eye,float scale) {
    return Multiply(InverseRigid(EyeWorld(t,eye,scale)),EyeFrustum(original,t,eye));
}
// A camera-centred sky dome represents directions, not nearby geometry.
// Keep its rotation and native depth range, but remove head/eye translation.
inline Matrix SkyProjection(Matrix world,Matrix viewportWorld,const Matrix& projection,const Transport::Tracking& t,unsigned eye) {
    for(int i=12;i<15;i++){world.m[i]=0;viewportWorld.m[i]=0;}
    return Multiply(Multiply(world,InverseRigid(viewportWorld)),EyeProjection(projection,t,eye,0));
}
// Reconstruct world positions from (textureU * eyeDepth, textureV *
// eyeDepth, eyeDepth, 1). Lighting must invert the same eye frustum as geometry.
inline Matrix DepthToWorld(const Matrix& viewportWorld,const Transport::Tracking& t,unsigned eye,float scale) {
    const auto& e=t.eyes[eye];
    float l=std::tan(e.left),r=std::tan(e.right),u=std::tan(e.up),d=std::tan(e.down);
    Matrix rays;rays.m[0]=r-l;rays.m[5]=d-u;rays.m[8]=l;rays.m[9]=u;rays.m[10]=rays.m[15]=1;
    return Multiply(Multiply(rays,EyeWorld(t,eye,scale)),viewportWorld);
}
inline Matrix Load(const void* p){Matrix m;std::memcpy(m.m,p,64);return m;}
// Map the UI's original clip rectangle to one head-relative plane, then project
// that SAME plane through each eye. Equal eye pixels are not equal headset rays.
inline Matrix HudClipTransform(const Transport::Tracking& t,unsigned eye,float scale=1.f) {
    Matrix depth;depth.m[10]=1;depth.m[14]=-.01f;
    auto projection=EyeProjection(depth,t,eye,1.f);
    Matrix plane;plane.m[0]=1.6f*scale;plane.m[5]=.9f*scale;plane.m[14]=2.f;plane.m[15]=1;
    auto result=Multiply(plane,projection);
    // This is an overlay, at the near clip depth. Preserve UI stencil ordering.
    for(int i=0;i<4;i++)result.m[i*4+2]=0;
    return result;
}
// Reproject the game's aiming UI through the headset camera. Its normalized
// screen coordinates represent game-camera rays, not head-relative UI points.
inline Matrix AimClipTransform(const Transport::Tracking& t,unsigned eye,const Matrix& gameWorld,const Matrix& headWorld,float fov,float aspect) {
    constexpr float distance=10000.f;
    Matrix plane;plane.m[0]=distance*std::tan(fov*.5f)*aspect;
    plane.m[5]=distance*std::tan(fov*.5f);plane.m[14]=distance;plane.m[15]=1;
    Matrix depth;depth.m[10]=1;depth.m[14]=-.01f;
    auto result=Multiply(Multiply(Multiply(plane,gameWorld),InverseRigid(headWorld)),EyeProjection(depth,t,eye,100.f));
    for(int i=0;i<4;i++)result.m[i*4+2]=0;
    return result;
}
}
