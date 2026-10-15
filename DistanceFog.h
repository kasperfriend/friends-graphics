#pragma once
#include "src/Environment/LocationTuning.h"
#include <wrl/client.h>
#include "src/Core/ShaderCache.h"
#include "src/Core/FrameContext.h"
#include "src/Effects/DirectionalVolumetricLighting.h"
#include "src/D3D9/TrackedRenderState.h"
#include "src/Water/WaterPolicy.h"
namespace distancefog {
bool active=true,keyDown=false,showStatus=false,hotkey=true,effectEnabled=true;
float distanceScale=1.f,powerScale=1.f;
unsigned matches=0;
std::wstring logPath;
std::wstring mainIni,tuningIni;
// One F7/GraphicsEffects.ini owner for BOTH the volume composite's edge fog
// and this fallback for clients without a usable atmosphere pass. The old
// startup-file Enabled gate silently disagreed with the live F7 switch.
int ReadTuning(const wchar_t* key,int fallback){return int(renderer::locationtuning::ReadInt(L"DistanceFog",key,fallback,tuningIni.c_str()));}
void ReloadTuning(){effectEnabled=ReadTuning(L"Enabled",1)!=0;distanceScale=std::clamp(ReadTuning(L"DistancePercent",100),10,300)*.01f;powerScale=std::clamp(ReadTuning(L"PowerPercent",100),10,300)*.01f;if(!logPath.empty())std::ofstream(std::filesystem::path(logPath),std::ios::app)<<"tuning enabled="<<effectEnabled<<" distance="<<distanceScale<<" power="<<powerScale<<'\n';}
void Configure(const std::wstring& base) {
    mainIni=base+L"ModernWoWRenderer.ini";tuningIni=base+L"GraphicsEffects.ini";
    showStatus=renderer::locationtuning::ReadInt(L"DistanceFog",L"ShowStatus",0,mainIni.c_str())!=0;
    ReloadTuning();
    logPath=base+L"DistanceFog.log";
}
void Present() {
    bool down=(GetAsyncKeyState(VK_F11)&0x8000)!=0;
    if(hotkey&&down&&!keyDown&&renderer::FrameContext::Current().wowFocused){active=!active;renderer::DirectionalVolumetricLighting::Instance().Settings().edgeFogEnabled=active&&effectEnabled;std::ofstream(std::filesystem::path(logPath),std::ios::app)<<"active="<<active<<'\n';}
    keyDown=down;matches=0;
}
struct Scope {
    IDirect3DDevice9* device=nullptr;UINT reg=0;float original[4]{};
    explicit Scope(IDirect3DDevice9* d,bool skip=false) noexcept {
        // The depth compositor now owns the same edge controls for every world
        // shader, including families absent from the legacy hash table.
        if(renderer::DirectionalVolumetricLighting::Instance().Settings().edgeFogEnabled&&renderer::DirectionalVolumetricLighting::Instance().Settings().enabled)return;
        if(skip||!active||!effectEnabled)return;
        try {
            uint64_t hash = renderer::g_trackedState.vsHash;
            if(!hash)return;
            // Registers verified from captured shader instructions, not guessed globally.
            switch(hash) {
            case 0xb598fbc887a39675ull:case 0x512e5caf066afb4bull:case 0xbd0060eb34ea4a7cull:reg=12;break;
            // Every documented liquid vertex shader feeds the same fog curve to
            // c4. Listing only one of them rescaled the distance fade on some
            // tiles and not on their neighbours, which drew hard squares across
            // one continuous body of water. The shape guard below still rejects a
            // variant whose register holds something else.
            default:reg=renderer::water::FogConstantRegister(hash);if(!reg)return;break;
            case 0xd8f18d8080e6bb3aull:reg=8;break;
            case 0x17a93e12ce3e0fd1ull:case 0x2361bbcb97cae46ull:case 0x2e3dc5694f4a01afull:
            case 0x5f2c6b3af8c6e543ull:case 0xd6dc2873a70922e9ull:case 0xf6a1996937ab5dc7ull:case 0xff32338728131932ull:reg=30;break;
            }
            if(FAILED(d->GetVertexShaderConstantF(reg,original,1)))return;
            for(float f:original)if(!std::isfinite(f))return;
            if(original[0]>0||original[1]<0||original[2]<=0)return;
            float modified[]={original[0]*distanceScale,original[1],original[2]*powerScale*renderer::FrameContext::Current().environment[renderer::DistanceFogPower],original[3]};
            if(FAILED(d->SetVertexShaderConstantF(reg,modified,1)))return;
            device=d;++matches;
            static bool logged=false;
            if(!logged){std::ofstream(std::filesystem::path(logPath),std::ios::app)<<"matched fog register="<<reg<<" original="<<original[0]<<','<<original[1]<<','<<original[2]<<" scale="<<distanceScale<<','<<powerScale<<'\n';logged=true;}
        }catch(...){Restore();}
    }
    void Restore() noexcept {if(device){device->SetVertexShaderConstantF(reg,original,1);device=nullptr;}}
    ~Scope(){Restore();}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
};
}
