#include "../Environment/LocationTuning.h"
#include "AmbientOcclusion.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include "../D3D9/ScopedPassState.h"
#include "../D3D9/ShaderBytecodeCache.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace renderer
{
namespace
{
constexpr uint64_t kSetupRetryMs = 2000;

void Log(const std::wstring& path, const std::string& text)
{
    if (path.empty()) return;
    std::ofstream(std::filesystem::path(path), std::ios::app) << text << '\n';
}

// --------------------------------------------------------------- generate ---
// Full-resolution occlusion term, written to a dedicated target and blurred
// onto the scene by kAmbientOcclusionApplySource below. This is a
// line-by-line mirror of renderer::ambientocclusion::ComputeOcclusion in
// AmbientOcclusionCore.h (kernel table, reconstruction, rotation hash use,
// range check, fade); a change in one must land in the other.
const char* kAmbientOcclusionSource = R"HLSL(
sampler2D depthMap:register(s0);
sampler2D rotationNoise:register(s1);
sampler2D waterMask:register(s2);
float4 projection:register(c0); // x=A,y=B,z=P00,w=P11 (frame projUnpack)
float4 screen:register(c1);     // x=1/width, y=1/height, z=viewport MaxZ
float4 tuning:register(c2);     // x=strength, y=radius, z=bias, w=fade distance
float4 noiseScale:register(c3); // xy=resolution/4, the 4x4 rotation block grid
// projection.y and (norm-projection.x) are both negative for every real
// surface - that quotient is what makes the view depth positive - so the
// guard is a floor on the magnitude, not on the signed value.
float ViewZ(float raw) {
    float norm=saturate(raw/max(screen.z,1e-4));
    return projection.y/min(norm-projection.x,-1e-6);
}
float3 ViewPos(float2 uv,float z) {
    return float3((uv.x*2-1)/projection.z,(1-uv.y*2)/projection.w,1)*z;
}
// The 12-direction golden-angle hemisphere from AmbientOcclusionCore.h.
static const float3 kKernel[12] = {
    float3(+0.2742,+0.0000,+0.9617), float3(-0.3433,+0.3145,+0.8850),
    float3(+0.0515,-0.5865,+0.8083), float3(+0.4148,+0.5410,+0.7317),
    float3(-0.7441,-0.1316,+0.6550), float3(+0.6883,-0.4379,+0.5783),
    float3(-0.2246,+0.8354,+0.5017), float3(-0.4172,-0.8033,+0.4250),
    float3(+0.8805,+0.3216,+0.3483), float3(-0.8896,+0.3672,+0.2717),
    float3(+0.4157,-0.8883,+0.1950), float3(+0.2972,+0.9475,+0.1183)
};
float4 main(float2 uv:TEXCOORD0):COLOR0 {
    float raw=tex2D(depthMap,uv).r;
    if(raw>=.9999) return float4(1,1,1,1);
    // Water owns its own shading; its animated depth would only sparkle.
    if(tex2D(waterMask,uv).r>.25) return float4(1,1,1,1);
    float z=ViewZ(raw);
    if(z<=0) return float4(1,1,1,1);
    // Distance fade: reconstruction noise grows with range long before a
    // crease stops being resolvable, so AO retires instead of sparkling.
    float fade=1-smoothstep(tuning.w*.55,tuning.w,z);
    if(fade<=0) return float4(1,1,1,1);
    float3 p=ViewPos(uv,z);
    // Depth-derived normal: on each axis keep the neighbour whose depth is
    // continuous with this pixel, so a silhouette never tilts the normal.
    float zl=ViewZ(tex2D(depthMap,uv-float2(screen.x,0)).r);
    float zr=ViewZ(tex2D(depthMap,uv+float2(screen.x,0)).r);
    float zd=ViewZ(tex2D(depthMap,uv-float2(0,screen.y)).r);
    float zu=ViewZ(tex2D(depthMap,uv+float2(0,screen.y)).r);
    float3 px=abs(zl-z)<abs(zr-z)?ViewPos(uv-float2(screen.x,0),zl):ViewPos(uv+float2(screen.x,0),zr);
    float3 py=abs(zd-z)<abs(zu-z)?ViewPos(uv-float2(0,screen.y),zd):ViewPos(uv+float2(0,screen.y),zu);
    float3 n=normalize(cross(px-p,py-p));
    if(dot(n,p)>0) n=-n; // face the camera at the view origin
    // Tangent frame for the hemisphere.
    float3 helper=abs(n.z)<.9?float3(0,0,1):float3(1,0,0);
    float3 t=normalize(cross(helper,n));
    float3 b=cross(n,t);
    // Static per-pixel kernel rotation, one per 4x4 pixel block.
    float2 rot=tex2D(rotationNoise,uv*noiseScale.xy).xy*2-1;
    // Softness and the locality window are the core's Softness() and
    // LocalityInner/Outer(); see AmbientOcclusionCore.h for why they are
    // this tight and that generous respectively.
    float radius=tuning.y,bias=tuning.z,soft=radius*.25;
    float occlusion=0;
    [unroll] for(int i=0;i<12;++i) {
        float3 k=kKernel[i];
        // Rotate the tangent-space sample about the normal.
        float rx=k.x*rot.x-k.y*rot.y;
        float ry=k.x*rot.y+k.y*rot.x;
        float3 dir=t*rx+b*ry+n*k.z;
        float3 sp=p+dir*radius;
        if(sp.z<=.02) continue;
        // Project the sample back to the screen (inverse of ViewPos).
        float2 suv=float2((sp.x/sp.z*projection.z+1)*.5,(1-sp.y/sp.z*projection.w)*.5);
        suv=saturate(suv); // CLAMP addressing
        float sz=ViewZ(tex2D(depthMap,suv).r);
        // dz>0: the surface at the sample pixel sits in front of the sample
        // point and blocks ambient light on its way to the receiver.
        float dz=sp.z-sz;
        // Locality guard: full credit while the occluder sits within ~2
        // radii of the receiver's depth, nothing past ~2.45. Nothing outside
        // that window can darken a surface - AO stays a crease effect.
        float rangeCheck=1-smoothstep(2.0,2.45,abs(sz-z)/radius);
        occlusion+=smoothstep(bias,bias+soft,dz)*rangeCheck;
    }
    occlusion/=12.0;
    float ao=saturate(1-tuning.x*occlusion);
    ao=lerp(1,ao,fade);
    return float4(ao,ao,ao,1);
})HLSL";

// ----------------------------------------------------------------- apply ---
// Depth-aware 4x4 box blur of the occlusion term, multiplied into the scene
// with a ZERO/SRCCOLOR blend (the caller sets it, and writes RGB only). The
// blur weights mirror ambientocclusion::BlurWeight: gentle terrain slopes
// blur normally, silhouettes get zero weight so occlusion never smears onto
// the background behind them.
const char* kAmbientOcclusionApplySource = R"HLSL(
sampler2D aoMap:register(s0);
sampler2D depthMap:register(s1);
float4 texel:register(c0);     // x=1/width, y=1/height
float4 depthInfo:register(c1); // x=A, y=B, z=viewport MaxZ
float Linear(float raw) {
    float norm=saturate(raw/max(depthInfo.z,1e-4));
    return depthInfo.y/min(norm-depthInfo.x,-1e-6);
}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
    float rawCenter=tex2D(depthMap,uv).r;
    if(rawCenter>=.9999) return float4(1,1,1,1);
    float zc=Linear(rawCenter);
    float threshold=max(zc*.06,.25);
    float sum=0,wsum=0;
    [unroll] for(int j=0;j<4;++j) [unroll] for(int i=0;i<4;++i) {
        float2 suv=uv+float2(float(i)-1.5,float(j)-1.5)*texel.xy;
        float raw=tex2D(depthMap,suv).r;
        float w=raw>=.9999?0:saturate(1-abs(Linear(raw)-zc)/threshold);
        sum+=w*tex2D(aoMap,suv).r;
        wsum+=w;
    }
    float ao=wsum>0?sum/wsum:tex2D(aoMap,uv).r;
    return float4(ao,ao,ao,1);
})HLSL";

bool Compile(IDirect3DDevice9* device, const char* source, const std::wstring& cacheDir, IDirect3DPixelShader9** out, std::string& error)
{
    // On-disk bytecode cache first (renderer::shaderbytecode): a repeat
    // launch skips fxc entirely, and a cached blob the driver refuses
    // falls back to one recompile, keeping the error text as before.
    Microsoft::WRL::ComPtr<ID3DBlob> shader;
    return renderer::shaderbytecode::GetShader(cacheDir, source, "main", "ps_3_0", shader,
        [device, out](ID3DBlob* b)
        { return SUCCEEDED(device->CreatePixelShader(static_cast<DWORD*>(b->GetBufferPointer()), out)); },
        &error);
}
} // namespace

AmbientOcclusion& AmbientOcclusion::Instance()
{
    static AmbientOcclusion instance;
    return instance;
}

void AmbientOcclusion::Configure(const std::wstring& basePath)
{
    const std::wstring ini = basePath + L"GraphicsEffects.ini";
    m_logPath = basePath + L"AmbientOcclusion.log";
    m_shaderCacheDir = renderer::shaderbytecode::Dir(basePath);
    auto read = [&](const wchar_t* key, int fallback) -> int {
        return static_cast<int>(renderer::locationtuning::ReadInt(L"AmbientOcclusion", key, fallback, ini.c_str()));
    };
    m_settings.enabled = read(L"Enabled", 1) != 0;
    // 60 % of full darkening by default: enough to seat objects on the ground
    // and read every crease, not enough to grey the world.
    m_settings.strength = std::clamp(read(L"StrengthPercent", 60), 0, 150) * .01f;
    // Default and ceiling both come from the measured sweep: 1 keeps every
    // open surface at exactly 1.0, and 6 is where AO has long since stopped
    // being a contact effect. See AmbientOcclusionCore.h.
    m_settings.radius = float(std::clamp(read(L"RadiusUnits", 1), 1, 6));
    m_settings.fadeDistance = float(std::clamp(read(L"DistanceFadeUnits", 250), 20, 600));
    m_settings.debug = read(L"DebugMask", 0) != 0;
    char message[160];
    sprintf_s(message, "tuning enabled=%d strength=%.2f radius=%.1f fade=%.0f debug=%d",
              m_settings.enabled ? 1 : 0, m_settings.strength, m_settings.radius, m_settings.fadeDistance,
              m_settings.debug ? 1 : 0);
    Log(m_logPath, message);
}

void AmbientOcclusion::Reset(IDirect3DDevice9*)
{
    m_aoSurface.Reset();
    m_aoTexture.Reset();
    m_noiseTexture.Reset();
    m_fallbackMask.Reset();
    m_generateShader.Reset();
    m_applyShader.Reset();
    m_owner = nullptr;
    m_width = m_height = 0;
    m_setupRetry.Succeed();
    m_shaderErrorLogged = false;
    m_resourceErrorLogged = false;
}

bool AmbientOcclusion::IsActive() const
{
    return m_settings.enabled && m_settings.strength > 0.f;
}

bool AmbientOcclusion::EnsureShaders(IDirect3DDevice9* device)
{
    if (m_generateShader && m_applyShader) return true;
    std::string error;
    if (!Compile(device, kAmbientOcclusionSource, m_shaderCacheDir, m_generateShader.GetAddressOf(), error) ||
        !Compile(device, kAmbientOcclusionApplySource, m_shaderCacheDir, m_applyShader.GetAddressOf(), error))
    {
        m_generateShader.Reset();
        m_applyShader.Reset();
        if (!m_shaderErrorLogged)
        {
            m_shaderErrorLogged = true;
            Log(m_logPath, "Ambient occlusion shaders unavailable, will retry: " + error);
        }
        return false;
    }
    m_shaderErrorLogged = false;
    return true;
}

bool AmbientOcclusion::EnsureResources(IDirect3DDevice9* device, uint32_t width, uint32_t height)
{
    if (m_owner == device && m_width == width && m_height == height && m_aoTexture && m_noiseTexture && m_fallbackMask)
        return true;

    m_aoSurface.Reset(); m_aoTexture.Reset(); m_noiseTexture.Reset(); m_fallbackMask.Reset();
    m_owner = device; m_width = width; m_height = height;

    if (FAILED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                     D3DPOOL_DEFAULT, m_aoTexture.GetAddressOf(), nullptr)) ||
        FAILED(m_aoTexture->GetSurfaceLevel(0, m_aoSurface.GetAddressOf())))
        return false;

    // Static 4x4 rotation texture from the core's own hash, so the CPU tools
    // turn their kernels exactly like the GPU does.
    if (FAILED(device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                     m_noiseTexture.GetAddressOf(), nullptr)))
        return false;
    {
        D3DLOCKED_RECT locked{};
        if (FAILED(m_noiseTexture->LockRect(0, &locked, nullptr, 0)))
        {
            m_noiseTexture.Reset();
            return false;
        }
        for (int y = 0; y < 4; ++y)
        {
            for (int x = 0; x < 4; ++x)
            {
                float rot[2];
                ambientocclusion::RotationFor(x, y, rot);
                const BYTE r = BYTE(std::clamp(rot[0] * .5f + .5f, 0.f, 1.f) * 255.f + .5f);
                const BYTE g = BYTE(std::clamp(rot[1] * .5f + .5f, 0.f, 1.f) * 255.f + .5f);
                *reinterpret_cast<DWORD*>(static_cast<BYTE*>(locked.pBits) + size_t(y) * locked.Pitch + size_t(x) * 4) =
                    D3DCOLOR_ARGB(255, r, g, 0);
            }
        }
        m_noiseTexture->UnlockRect(0);
    }

    // 1x1 transparent stand-in for frames without a water mask texture.
    if (FAILED(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                     m_fallbackMask.GetAddressOf(), nullptr)))
        return false;
    {
        D3DLOCKED_RECT locked{};
        if (FAILED(m_fallbackMask->LockRect(0, &locked, nullptr, 0)))
        {
            m_fallbackMask.Reset();
            return false;
        }
        *reinterpret_cast<DWORD*>(locked.pBits) = D3DCOLOR_ARGB(0, 0, 0, 0);
        m_fallbackMask->UnlockRect(0);
    }
    return true;
}

void AmbientOcclusion::NoteFailure(const std::string& reason)
{
    if (m_setupRetry.Fail(GetTickCount64(), kSetupRetryMs) && !m_resourceErrorLogged)
    {
        m_resourceErrorLogged = true;
        Log(m_logPath, reason + "; ambient occlusion skipped and retried without affecting the scene");
    }
}

bool AmbientOcclusion::Render(IDirect3DDevice9* device, const FrameContext& frameContext,
                              IDirect3DSurface9* targetSurface, IDirect3DTexture9* depthTexture)
{
    if (!IsActive() || !device || !targetSurface || !depthTexture) return false;
    if (!m_setupRetry.Open(GetTickCount64())) return false;

    // Camera/depth reconstruction must be valid this frame; without it the
    // pass cannot tell a crease from the far plane.
    const float* unpack = frameContext.projUnpack;
    if (!std::isfinite(unpack[0]) || !std::isfinite(unpack[1]) || unpack[1] == 0.f ||
        unpack[2] <= 0.f || unpack[3] <= 0.f || frameContext.depthMaxZ <= 0.f)
        return false;

    if (!EnsureShaders(device))
    {
        NoteFailure("Ambient occlusion shaders failed");
        return false;
    }
    D3DSURFACE_DESC desc{};
    if (FAILED(targetSurface->GetDesc(&desc)) || desc.Width == 0 || desc.Height == 0)
        return false;
    if (!EnsureResources(device, desc.Width, desc.Height))
    {
        NoteFailure("Ambient occlusion resources failed");
        return false;
    }

    // Captures and restores every state this pass touches, including the
    // render target, viewport, pixel-shader constants and sampler states.
    renderer::ScopedPassState saved(device);
    bool ok = true;
    auto check = [&](HRESULT hr) { if (FAILED(hr)) ok = false; };

    check(device->SetVertexShader(nullptr));
    check(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
    for (D3DRENDERSTATETYPE state : { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE,
                                      D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE,
                                      D3DRS_LIGHTING, D3DRS_SRGBWRITEENABLE })
        check(device->SetRenderState(state, FALSE));
    check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    check(device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID));
    check(device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0));

    struct V { float x, y, z, w, u, v; };
    const float qw = float(desc.Width) - .5f, qh = float(desc.Height) - .5f;
    const V quad[] = { { -.5f, -.5f, 0, 1, 0, 0 }, { qw, -.5f, 0, 1, 1, 0 },
                       { -.5f, qh, 0, 1, 0, 1 }, { qw, qh, 0, 1, 1, 1 } };
    auto drawQuad = [&]() { check(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V))); };

    D3DVIEWPORT9 viewport{ 0, 0, desc.Width, desc.Height, 0, 1 };

    // ---- pass 1: full-resolution occlusion term --------------------------
    check(device->SetRenderTarget(0, m_aoSurface.Get()));
    check(device->SetViewport(&viewport));
    check(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
    check(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF));
    check(device->SetPixelShader(m_generateShader.Get()));

    check(device->SetTexture(0, depthTexture));
    check(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    check(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    check(device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
    check(device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE));
    check(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
    check(device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));

    check(device->SetTexture(1, m_noiseTexture.Get()));
    check(device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    check(device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    check(device->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
    check(device->SetSamplerState(1, D3DSAMP_SRGBTEXTURE, FALSE));
    check(device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP));
    check(device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP));

    IDirect3DTexture9* waterMask = frameContext.waterMaskTexture ? frameContext.waterMaskTexture : m_fallbackMask.Get();
    check(device->SetTexture(2, waterMask));
    check(device->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR));
    check(device->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
    check(device->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
    check(device->SetSamplerState(2, D3DSAMP_SRGBTEXTURE, FALSE));
    check(device->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
    check(device->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));

    const float projection[4] = { unpack[0], unpack[1], unpack[2], unpack[3] };
    const float screen[4] = { 1.f / float(desc.Width), 1.f / float(desc.Height), frameContext.depthMaxZ, 0.f };
    const float tuning[4] = { m_settings.strength, m_settings.radius,
                              ambientocclusion::Bias(m_settings.radius), m_settings.fadeDistance };
    const float noiseScale[4] = { float(desc.Width) * .25f, float(desc.Height) * .25f, 0.f, 0.f };
    check(device->SetPixelShaderConstantF(0, projection, 1));
    check(device->SetPixelShaderConstantF(1, screen, 1));
    check(device->SetPixelShaderConstantF(2, tuning, 1));
    check(device->SetPixelShaderConstantF(3, noiseScale, 1));
    if (ok) drawQuad();

    // ---- pass 2: depth-aware blur, multiplied into the scene -------------
    check(device->SetRenderTarget(0, targetSurface));
    check(device->SetViewport(&viewport));
    check(device->SetPixelShader(m_applyShader.Get()));
    check(device->SetTexture(0, m_aoTexture.Get()));
    check(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    check(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    check(device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
    check(device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE));
    check(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
    check(device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
    check(device->SetTexture(1, depthTexture));
    check(device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    check(device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    check(device->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
    check(device->SetSamplerState(1, D3DSAMP_SRGBTEXTURE, FALSE));
    check(device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
    check(device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));

    const float texel[4] = { 1.f / float(desc.Width), 1.f / float(desc.Height), 0.f, 0.f };
    const float depthInfo[4] = { unpack[0], unpack[1], frameContext.depthMaxZ, 0.f };
    check(device->SetPixelShaderConstantF(0, texel, 1));
    check(device->SetPixelShaderConstantF(1, depthInfo, 1));
    if (m_settings.debug)
    {
        // Opaque overlay of the blurred occlusion term for tuning.
        check(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
        check(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF));
    }
    else
    {
        check(device->SetRenderState(D3DRS_COLORWRITEENABLE, 7));
        check(device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD));
        check(device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
        check(device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO));
        check(device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR));
    }
    if (ok) drawQuad();

    if (!ok)
    {
        NoteFailure("Ambient occlusion draw failed");
        return false;
    }
    m_setupRetry.Succeed();
    m_resourceErrorLogged = false;
    return true;
}
} // namespace renderer
