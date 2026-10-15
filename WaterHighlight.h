#pragma once
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "src/Core/ShaderCache.h"
#include "src/Core/FrameContext.h"
#include "src/D3D9/ShaderBytecodeCache.h"
#pragma comment(lib,"d3dcompiler.lib")

namespace waterhighlight {
using Microsoft::WRL::ComPtr;
bool enabled=false, visible=true, nextDown=false, toggleDown=false;
unsigned group=0;
uint64_t firstHash=0xebecd65635a1a75eull;
std::vector<DWORD> code;
std::wstring logPath;
void Status() {
    std::ofstream out(std::filesystem::path(logPath),std::ios::app);
    out<<"highlight="<<visible<<" group="<<group+1<<" (1 magenta, 2 cyan, 3 green fixed-function, 4 yellow)\n";
}
void Configure(const std::wstring& base) {
    auto ini=base+L"ModernWoWRenderer.ini";
    enabled=GetPrivateProfileIntW(L"Highlight",L"Enabled",0,ini.c_str())!=0;
    if(!enabled)return;
    logPath=base+L"WaterHighlight.log";
    wchar_t hash[32]{};GetPrivateProfileStringW(L"Highlight",L"Group1PixelShader",L"ebecd65635a1a75e",hash,32,ini.c_str());
    firstHash=wcstoull(hash,nullptr,16);
    const char* source="float4 color:register(c0);float4 main():COLOR{return color;}";
    ComPtr<ID3DBlob> blob;
    // Bytecode-only consumer: the driver creates the shader later at draw
    // time, so "create" just accepts the (validated) cached or compiled blob.
    if(!renderer::shaderbytecode::GetShader(renderer::shaderbytecode::Dir(base),source,"main","ps_2_0",blob,[](const ID3DBlob*){return true;})) { enabled=false;return; }
    code.resize(blob->GetBufferSize()/sizeof(DWORD));memcpy(code.data(),blob->GetBufferPointer(),blob->GetBufferSize());
    Status();
}
void Present() {
    if(!enabled)return;
    bool next=(GetAsyncKeyState(VK_F6)&0x8000)!=0,toggle=(GetAsyncKeyState(VK_F7)&0x8000)!=0;
    if(renderer::FrameContext::Current().wowFocused) {
        if(next&&!nextDown) {group=(group+1)%4;Status();}
        if(toggle&&!toggleDown) {visible=!visible;Status();}
    }
    nextDown=next;toggleDown=toggle;
}
struct Scope {
    IDirect3DDevice9* device=nullptr;
    ComPtr<IDirect3DPixelShader9> original,replacement;
    float oldConstant[4]{};
    bool changed=false;
    explicit Scope(IDirect3DDevice9* d,bool skip=false) noexcept {
        if(skip||!enabled||!visible)return;
        try {
            if(FAILED(d->GetPixelShader(original.GetAddressOf())))return;
            uint64_t hash = original ? renderer::ShaderCache::Instance().GetShaderHash(original.Get()) : 0;
            bool match=(group==0&&hash==firstHash)||(group==1&&hash==0xe56e7f7bd7a96a88ull)||(group==3&&hash==0x17f042a7906ca126ull);
            if(group==2&&!original) {
                ComPtr<IDirect3DVertexShader9> vs;
                match=SUCCEEDED(d->GetVertexShader(vs.GetAddressOf()))&&!vs;
            }
            if(!match)return;
            if(FAILED(d->GetPixelShaderConstantF(0,oldConstant,1)))return;
            if(FAILED(d->CreatePixelShader(code.data(),replacement.GetAddressOf())))return;
            static const float colors[4][4]={{1,0,1,1},{0,1,1,1},{0,1,0,1},{1,1,0,1}};
            if(FAILED(d->SetPixelShader(replacement.Get())))return;
            device=d;changed=true;
            if(FAILED(d->SetPixelShaderConstantF(0,colors[group],1)))Restore();
        }catch(...){Restore();}
    }
    void Restore() noexcept {
        if(changed){device->SetPixelShader(original.Get());device->SetPixelShaderConstantF(0,oldConstant,1);changed=false;}
    }
    ~Scope(){Restore();}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
};
}
