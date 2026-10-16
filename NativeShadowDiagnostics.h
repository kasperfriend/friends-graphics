#include "src/Environment/LocationTuning.h"
#pragma once

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "src/D3D9/ScopedPassState.h"

namespace nativeshadowdiag
{
struct TargetRecord
{
    const void* texture = nullptr;
    UINT width = 0;
    UINT height = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN;
    uint64_t lastProducerVs = 0;
    uint64_t lastProducerPs = 0;
    uint64_t lastProducerFrame = 0;
    uint32_t lastProducerDraw = 0;
    bool reuseLogged = false;
    bool depthTarget = false;
};

inline bool enabled = false;
inline bool logConstants = true;
inline uint32_t summaryInterval = 300;
inline uint32_t maxUniqueCandidates = 256;
inline std::wstring logPath;
inline uint64_t frame = 0;
inline uint32_t drawOrdinal = 0;
inline UINT backBufferWidth = 0;
inline UINT backBufferHeight = 0;
inline const void* currentRtTexture = nullptr;
inline D3DSURFACE_DESC currentRtDesc{};
inline bool currentRtValid = false;
inline const void* currentDsTexture = nullptr;
inline D3DSURFACE_DESC currentDsDesc{};
inline bool currentDsValid = false;
inline std::vector<TargetRecord> targets;
inline std::unordered_set<uint64_t> loggedSignatures;
inline uint32_t candidateDrawsThisFrame = 0;
inline uint32_t rtReuseEventsThisFrame = 0;
inline std::array<bool, 16> boundFormerRt{};
inline std::array<IDirect3DTexture9*, 16> previewTextureByStage{};
inline std::unordered_set<uint64_t> dumpedVertexShaders;
inline std::unordered_set<uint64_t> dumpedPixelShaders;
inline std::wstring dumpDirectory;
inline bool previewEnabled = false;
inline bool previewKeyDown = false;
inline IDirect3DDevice9* previewOwner = nullptr;
inline IDirect3DPixelShader9* previewShader = nullptr;
inline bool enhancementEnabled = true;
inline float softnessScale = 1.45f;
// 100% == the native 0.3 max-darkening constant found in the confirmed
// receivers (def c12, 0.3, 0.7, 0, 0 -> result = visibility*0.3+0.7, so
// shadows never go below 70% brightness natively). This scales that 0.3
// directly; 1-scaledStrength is re-derived each time so the fully-lit
// case (visibility=1) still comes out to 1.0, not just the shadowed case
// getting darker.
inline float strengthScale = 1.0f;
inline std::wstring settingsPath;

inline void ReloadEnhancement()
{
    if (settingsPath.empty()) return;
    enhancementEnabled = renderer::locationtuning::ReadInt(L"NativeShadows", L"Enabled", 1, settingsPath.c_str()) != 0;
    const int percent = std::clamp(static_cast<int>(renderer::locationtuning::ReadInt(
        L"NativeShadows", L"SoftnessPercent", 145, settingsPath.c_str())), 50, 220);
    softnessScale = static_cast<float>(percent) * .01f;
    const int strengthPercent = std::clamp(static_cast<int>(renderer::locationtuning::ReadInt(
        L"NativeShadows", L"StrengthPercent", 100, settingsPath.c_str())), 30, 250);
    strengthScale = static_cast<float>(strengthPercent) * .01f;
}

inline uint64_t Mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

inline const char* FormatName(D3DFORMAT f)
{
    switch (f)
    {
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
    case D3DFMT_R16F: return "R16F";
    case D3DFMT_R32F: return "R32F";
    case D3DFMT_D16: return "D16";
    case D3DFMT_D24X8: return "D24X8";
    case D3DFMT_D24S8: return "D24S8";
    case D3DFMT_D24X4S4: return "D24X4S4";
    default: return "OTHER";
    }
}

inline const char* PrimitiveName(D3DPRIMITIVETYPE t)
{
    switch (t)
    {
    case D3DPT_POINTLIST: return "POINTLIST";
    case D3DPT_LINELIST: return "LINELIST";
    case D3DPT_LINESTRIP: return "LINESTRIP";
    case D3DPT_TRIANGLELIST: return "TRIANGLELIST";
    case D3DPT_TRIANGLESTRIP: return "TRIANGLESTRIP";
    case D3DPT_TRIANGLEFAN: return "TRIANGLEFAN";
    default: return "UNKNOWN";
    }
}

inline void Append(const std::string& text)
{
    if (logPath.empty()) return;
    std::ofstream out(logPath, std::ios::app);
    if (out) out << text;
}

inline const void* TextureContainer(IDirect3DSurface9* surface)
{
    if (!surface) return nullptr;
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(surface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&texture))) || !texture)
        return nullptr;
    const void* identity = texture;
    texture->Release();
    return identity;
}

inline TargetRecord* FindTarget(const void* texture)
{
    if (!texture) return nullptr;
    auto it = std::find_if(targets.begin(), targets.end(), [texture](const TargetRecord& r) { return r.texture == texture; });
    return it == targets.end() ? nullptr : &*it;
}

inline void Configure(const std::wstring& basePath)
{
    const std::wstring ini = basePath + L"GraphicsEffects.ini";
    settingsPath = ini;
    ReloadEnhancement();
    enabled = renderer::locationtuning::ReadInt(L"ShadowDiagnostics", L"Enabled", 1, ini.c_str()) != 0;
    logConstants = renderer::locationtuning::ReadInt(L"ShadowDiagnostics", L"LogConstants", 1, ini.c_str()) != 0;
    summaryInterval = static_cast<uint32_t>(std::clamp(static_cast<int>(renderer::locationtuning::ReadInt(L"ShadowDiagnostics", L"SummaryIntervalFrames", 300, ini.c_str())), 60, 3600));
    maxUniqueCandidates = static_cast<uint32_t>(std::clamp(static_cast<int>(renderer::locationtuning::ReadInt(L"ShadowDiagnostics", L"MaxUniqueCandidates", 256, ini.c_str())), 32, 1024));
    logPath = basePath + L"NativeShadowDiagnostics.log";
    dumpDirectory = basePath + L"NativeShadowShaders";
    previewEnabled = renderer::locationtuning::ReadInt(L"ShadowDiagnostics", L"PreviewEnabled", 1, ini.c_str()) != 0;
    CreateDirectoryW(dumpDirectory.c_str(), nullptr);
    if (enabled)
    {
        std::ofstream out(logPath, std::ios::trunc);
        if (out)
            out << "Native WoW shadow diagnostics (read-only)\n"
                << "Candidates: offscreen RT, depth-only draw, projected blended draw, or prior RT sampled as texture.\n"
                << "No draw replay and no render output modification are performed.\n\n";
    }
}

inline void Reset()
{
    frame = 0;
    drawOrdinal = 0;
    currentRtTexture = nullptr;
    currentRtValid = false;
    currentDsTexture = nullptr;
    currentDsValid = false;
    targets.clear();
    loggedSignatures.clear();
    candidateDrawsThisFrame = 0;
    rtReuseEventsThisFrame = 0;
    boundFormerRt.fill(false);
    for (auto*& texture : previewTextureByStage)
    {
        if (texture) texture->Release();
        texture = nullptr;
    }
    if (previewShader) previewShader->Release();
    previewShader = nullptr;
    previewOwner = nullptr;
    dumpedVertexShaders.clear();
    dumpedPixelShaders.clear();
}

inline void OnSetRenderTarget(DWORD index, IDirect3DSurface9* surface)
{
    if ((!enabled && !enhancementEnabled) || index != 0 || !surface) return;
    D3DSURFACE_DESC desc{};
    if (FAILED(surface->GetDesc(&desc))) return;
    currentRtDesc = desc;
    currentRtValid = true;
    currentRtTexture = TextureContainer(surface);

    if (!currentRtTexture)
    {
        backBufferWidth = std::max(backBufferWidth, desc.Width);
        backBufferHeight = std::max(backBufferHeight, desc.Height);
        return;
    }

    TargetRecord* record = FindTarget(currentRtTexture);
    if (!record)
    {
        targets.push_back({ currentRtTexture, desc.Width, desc.Height, desc.Format, 0, 0, 0, 0, false, false });
        record = &targets.back();
        if (enabled)
        {
            std::ostringstream s;
            s << "[RT_DISCOVER] frame=" << frame << " texture=" << currentRtTexture
              << " size=" << desc.Width << 'x' << desc.Height
              << " format=" << FormatName(desc.Format) << '(' << unsigned(desc.Format) << ")\n";
            Append(s.str());
        }
    }
}

inline void OnSetDepth(IDirect3DSurface9* surface)
{
    if (!enabled && !enhancementEnabled) return;
    currentDsTexture = nullptr;
    currentDsValid = false;
    if (!surface) return;
    D3DSURFACE_DESC desc{};
    if (FAILED(surface->GetDesc(&desc))) return;
    currentDsDesc = desc;
    currentDsValid = true;
    currentDsTexture = TextureContainer(surface);
    if (!currentDsTexture) return;

    TargetRecord* record = FindTarget(currentDsTexture);
    if (!record)
    {
        targets.push_back({ currentDsTexture, desc.Width, desc.Height, desc.Format, 0, 0, 0, 0, false, true });
        if (enabled)
        {
            std::ostringstream s;
            s << "[DS_DISCOVER] frame=" << frame << " texture=" << currentDsTexture
              << " size=" << desc.Width << 'x' << desc.Height
              << " format=" << FormatName(desc.Format) << '(' << unsigned(desc.Format) << ")\n";
            Append(s.str());
        }
    }
}

inline void OnSetTexture(DWORD stage, IDirect3DBaseTexture9* texture)
{
    if (!enabled && !enhancementEnabled) return;
    if (stage < boundFormerRt.size()) boundFormerRt[stage] = texture && FindTarget(texture);
    if (stage < previewTextureByStage.size())
    {
        IDirect3DTexture9* next = nullptr;
        if (texture && texture->GetType() == D3DRTYPE_TEXTURE)
        {
            TargetRecord* record = FindTarget(texture);
            if (record && record->depthTarget) next = static_cast<IDirect3DTexture9*>(texture);
        }
        if (next) next->AddRef();
        if (previewTextureByStage[stage]) previewTextureByStage[stage]->Release();
        previewTextureByStage[stage] = next;
    }
    if (!texture) return;
    TargetRecord* record = FindTarget(texture);
    if (!enabled || !record || record->lastProducerFrame == 0 || record->reuseLogged) return;
    record->reuseLogged = true;
    ++rtReuseEventsThisFrame;
    std::ostringstream s;
    s << "[RT_REUSED_AS_TEXTURE] frame=" << frame << " draw=" << drawOrdinal
      << " stage=s" << stage << " texture=" << texture
      << " size=" << record->width << 'x' << record->height
      << " format=" << FormatName(record->format)
      << " role=" << (record->depthTarget ? "depth" : "color")
      << " producer_frame=" << record->lastProducerFrame
      << " producer_draw=" << record->lastProducerDraw
      << " producer_vs=0x" << std::hex << record->lastProducerVs
      << " producer_ps=0x" << record->lastProducerPs << std::dec << "\n";
    Append(s.str());
}

// Register layout is a property of the SHADER (its own constant layout),
// not of which texture stage the cascades happened to be bound to this
// particular draw - the original stage-keyed lists rejected known-good
// hashes whenever they showed up on the "other" stage (confirmed live:
// 0xac726e53bca0ac1a, originally only accepted at stage 5, was observed
// bound at stage 4 too and wrongly rejected there). Keyed by hash instead.
inline bool FindReceiverRegisterLayout(uint64_t hash, UINT& startRegister, UINT& registerCount, bool& oddRegistersOnly)
{
    // Verified from Round 6 runtime captures and disassembly. These are WoW's
    // native world/WMO/M2 receiver variants, not renderer replacement shaders.
    static constexpr uint64_t kFamilyA[] = { // register layout: c5, count 7 (only odd registers are real UV offsets)
        0x06b49731d6b3d33eull, 0x0a959b9587e8d509ull, 0x2d398d9d47f90d9dull,
        0x42c3a115d145164aull, 0x6ef54f064e5141ddull, 0x827c05c3635b689full,
        0x92a403ad7278dd4full, 0xb52eba6fbc7f34f2ull, 0xc4b68e0f9cd2c852ull,
        0xca8fa34d6b69a7baull, 0xcc75b5e865b2f869ull, 0xdd40e9d5fc1ef426ull,
        0xf71ca77414ce780eull
    };
    static constexpr uint64_t kFamilyB[] = { // register layout: c3, count 8
        0x08b17d1abaad8eceull, 0x449b66c5fb94fedeull, 0x634793193e26059dull,
        0x6e6f053c4b910cf4ull, 0xac726e53bca0ac1aull
    };
    if (std::find(std::begin(kFamilyA), std::end(kFamilyA), hash) != std::end(kFamilyA))
    {
        startRegister = 5u; registerCount = 7u; oddRegistersOnly = true; return true;
    }
    if (std::find(std::begin(kFamilyB), std::end(kFamilyB), hash) != std::end(kFamilyB))
    {
        startRegister = 3u; registerCount = 8u; oddRegistersOnly = false; return true;
    }
    // Newly observed receiver variants (live captures, e.g. tree/building
    // material families not covered by the original two captures) - logged
    // as candidates (see the "not in the receiver whitelist" branch below)
    // but deliberately NOT enhanced yet. Their register layout hasn't been
    // disassembly-verified, and guessing a range risks scaling something
    // that isn't a UV offset in a shader family we haven't inspected.
    return false;
}

// Confirmed by disassembling the dumped .asm for each of these hashes:
// every one defines c12 as (0.3, 0.7, 0, 0) and uses it as exactly
// `result = shadowVisibility * c12.x + c12.y` right before the shadow
// term is applied to the lit colour - i.e. shadows never go below 70%
// brightness natively (c12.x is the only thing controlling how much
// darker a fully-shadowed pixel gets). A fifth family-B hash
// (0x449b66c5fb94fede) uses c12 for something structurally different
// (a different instruction pattern entirely) and is deliberately excluded
// here rather than assumed to match - same "verify before touching"
// approach as the softness registers.
inline bool IsStrengthConfirmedHash(uint64_t hash)
{
    static constexpr uint64_t kConfirmed[] = {
        // Family B: visibility * c12.x + c12.y.
        0x08b17d1abaad8eceull, 0x634793193e26059dull, 0x6e6f053c4b910cf4ull, 0xac726e53bca0ac1aull,
        // Family A: visibility * c8.y + c8.z. Verified from the live
        // 3.3.5 extShadowQuality=5 receiver dumps (terrain/WMO variants).
        0x06b49731d6b3d33eull, 0x42c3a115d145164aull, 0x6ef54f064e5141ddull,
        0x827c05c3635b689full, 0xb52eba6fbc7f34f2ull, 0xca8fa34d6b69a7baull,
        0xdd40e9d5fc1ef426ull, 0xf71ca77414ce780eull
    };
    return std::find(std::begin(kConfirmed), std::end(kConfirmed), hash) != std::end(kConfirmed);
}

inline bool IsFamilyAStrengthHash(uint64_t hash)
{
    static constexpr uint64_t kConfirmed[] = {
        0x06b49731d6b3d33eull, 0x42c3a115d145164aull, 0x6ef54f064e5141ddull,
        0x827c05c3635b689full, 0xb52eba6fbc7f34f2ull, 0xca8fa34d6b69a7baull,
        0xdd40e9d5fc1ef426ull, 0xf71ca77414ce780eull
    };
    return std::find(std::begin(kConfirmed), std::end(kConfirmed), hash) != std::end(kConfirmed);
}

// c12 turned out to be a `def`-declared shader literal (compiled into the
// instruction stream), not a runtime constant - SetPixelShaderConstantF on
// it is a silent no-op, confirmed live (0% difference across the slider
// range). The only way to actually change it is to patch the compiled
// bytecode's literal and use that as a replacement shader for the exact
// same draws SoftnessScope already validates (same hash whitelist, same
// cascade/colourWrite guards) - the same "swap the shader for one draw,
// restore after" technique WaterEffect already uses, just building the
// replacement once instead of writing HLSL by hand.
//
// D3D9 SM3 bytecode: a `def cN, x, y, z, w` instruction is the fixed byte
// sequence [opcode+length][dest register token][4 raw floats]. Verified
// byte-for-byte in the dumped .bin for all 4 confirmed hashes:
//   0x05000051 (D3DSIO_DEF, length=5) 0xA00F000C (dest = c12, full mask)
//   followed immediately by the four x/y/z/w float DWORDs.
// Patching only rewrites those two floats in a private copy of the
// bytecode; instruction count, opcodes and control flow are untouched.
inline bool PatchStrengthLiteral(const std::vector<BYTE>& original, uint64_t hash,
                                 float darkening, float floor, std::vector<DWORD>& patched)
{
    if (original.size() % 4 != 0 || original.size() < 24) return false;
    patched.assign(original.size() / 4, 0);
    std::memcpy(patched.data(), original.data(), original.size());
    const bool familyA = IsFamilyAStrengthHash(hash);
    const DWORD expectedDest = familyA ? 0xA00F0008u : 0xA00F000Cu; // c8 or c12
    for (size_t i = 0; i + 24 <= original.size(); i += 4)
    {
        DWORD opcode, dest;
        std::memcpy(&opcode, original.data() + i, 4);
        std::memcpy(&dest, original.data() + i + 4, 4);
        if (opcode != 0x05000051u || dest != expectedDest) continue;
        float floats[4];
        std::memcpy(floats, original.data() + i + 8, 16);
        const bool shapeMatches = familyA
            ? (std::abs(floats[0] - 1.2f) <= 1e-4f &&
               std::abs(floats[1] - .3f) <= 1e-4f &&
               std::abs(floats[2] - .7f) <= 1e-4f)
            : (std::abs(floats[0] - .3f) <= 1e-4f &&
               std::abs(floats[1] - .7f) <= 1e-4f &&
               floats[2] == 0.f && floats[3] == 0.f);
        if (!shapeMatches) continue; // do not touch an unverified literal shape
        const size_t darkeningOffset = i + (familyA ? 12 : 8);
        const size_t floorOffset = i + (familyA ? 16 : 12);
        std::memcpy(reinterpret_cast<BYTE*>(patched.data()) + darkeningOffset, &darkening, 4);
        std::memcpy(reinterpret_cast<BYTE*>(patched.data()) + floorOffset, &floor, 4);
        return true;
    }
    return false;
}

struct PatchedStrengthShader
{
    Microsoft::WRL::ComPtr<IDirect3DPixelShader9> shader;
    float builtForScale = -1.f;
};
inline std::unordered_map<uint64_t, PatchedStrengthShader> patchedStrengthShaders;

// Returns a shader identical to `original` except c12's literal scaled by
// the current strengthScale, rebuilding only when the slider actually
// changes. Returns null (leave the native shader bound) if the def
// instruction wasn't found in the expected shape, or on any failure -
// this must never be the reason a receiver draw doesn't render.
inline IDirect3DPixelShader9* GetPatchedStrengthShader(IDirect3DDevice9* d, uint64_t hash, IDirect3DPixelShader9* original)
{
    auto& entry = patchedStrengthShaders[hash];
    if (entry.shader && std::abs(entry.builtForScale - strengthScale) < .001f) return entry.shader.Get();

    ID3DBlob* blob = nullptr;
    if (!original || FAILED(original->GetFunction(&blob)) || !blob) return nullptr;
    const UINT size = static_cast<UINT>(blob->GetBufferSize());
    if (!size || size > 65536) { blob->Release(); return nullptr; }
    std::vector<BYTE> bytecode(size);
    memcpy(bytecode.data(), blob->GetBufferPointer(), size);
    blob->Release();

    const float darkening = std::clamp(.3f * strengthScale, 0.f, 1.f);
    std::vector<DWORD> patched;
    if (!PatchStrengthLiteral(bytecode, hash, darkening, 1.f - darkening, patched)) return nullptr;

    Microsoft::WRL::ComPtr<IDirect3DPixelShader9> created;
    if (FAILED(d->CreatePixelShader(patched.data(), created.GetAddressOf()))) return nullptr;

    entry.shader = created;
    entry.builtForScale = strengthScale;
    std::ostringstream s;
    s << "[STRENGTH] built patched shader for psHash=0x" << std::hex << hash << std::dec
      << " darkening 0.3 -> " << darkening << "\n";
    Append(s.str());
    return entry.shader.Get();
}

inline bool HasFourNativeCascades(IDirect3DDevice9* device, int firstStage)
{
    if (!device) return false;
    for (int i = 0; i < 4; ++i)
    {
        IDirect3DBaseTexture9* baseTexture = nullptr;
        if (FAILED(device->GetTexture(firstStage + i, &baseTexture)) || !baseTexture)
            return false;
        bool valid = false;
        if (baseTexture->GetType() == D3DRTYPE_TEXTURE)
        {
            IDirect3DTexture9* texture = static_cast<IDirect3DTexture9*>(baseTexture);
            D3DSURFACE_DESC desc{};
            valid = SUCCEEDED(texture->GetLevelDesc(0, &desc)) &&
                desc.Width == 2048 && desc.Height == 2048 && desc.Format == D3DFMT_D24X8;
        }
        baseTexture->Release();
        if (!valid) return false;
    }
    return true;
}

class SoftnessScope
{
public:
    SoftnessScope(IDirect3DDevice9* d, uint64_t psHash, IDirect3DPixelShader9* trackedShader) : device(d)
    {
        const bool needSoftness = std::abs(softnessScale - 1.f) > .001f;
        const bool needStrength = std::abs(strengthScale - 1.f) > .001f;
        if (!enhancementEnabled || !device || (!needSoftness && !needStrength) || currentRtTexture)
            return;

        // The exact receiver hashes were verified from dumped shader bytecode,
        // and this scope runs on EVERY back-buffer draw - so the whitelist is
        // checked in memory first. A non-receiver pixel shader then costs a
        // dozen comparisons and returns, instead of crossing the D3D9 driver
        // for the GetPixelShader check, the cascade texture probe (several
        // GetTexture/GetLevelDesc/Release calls) and a render-state query on
        // every single draw. The whitelist is the authoritative safety gate;
        // the cascade probe's result is diagnostic only.
        UINT startRegister = 0, registerCount = 0;
        bool oddRegistersOnly = false;
        const bool isSoftnessReceiver = needSoftness &&
            FindReceiverRegisterLayout(psHash, startRegister, registerCount, oddRegistersOnly);
        const bool isStrengthReceiver = needStrength && IsStrengthConfirmedHash(psHash);
        if (!isSoftnessReceiver && !isStrengthReceiver)
        {
            // New-receiver discovery survives the pre-gate: a hash the game
            // draws with the native cascades bound is exactly the variant the
            // whitelist wants to grow for (OnDraw's candidate log does not
            // see back-buffer receiver draws). Each unknown pixel shader is
            // probed ONCE per session - on its first draw - then cached;
            // distinct pixel shaders are bounded, so discovery costs a few
            // dozen probes when a new zone's materials first appear, never
            // per draw. A first draw seen without cascades bound is assumed
            // not to be a receiver (the game binds the four cascade textures
            // on every draw of a shadow-sampling material).
            if (needSoftness)
            {
                static std::unordered_set<uint64_t> probedUnknown;
                if (probedUnknown.size() < 1024 && probedUnknown.insert(psHash).second)
                {
                    const int stage = HasFourNativeCascades(device, 4) ? 4 :
                        (HasFourNativeCascades(device, 5) ? 5 : -1);
                    if (stage >= 0)
                    {
                        static std::unordered_set<uint64_t> loggedVariants;
                        if (loggedVariants.insert(psHash).second)
                        {
                            std::ostringstream s;
                            s << "[SOFTNESS] cascades bound at stage " << stage
                              << " but psHash=0x" << std::hex << psHash << std::dec
                              << " is not in the receiver whitelist\n";
                            Append(s.str());
                        }
                    }
                }
            }
            return;
        }

        IDirect3DPixelShader9* actualShader = nullptr;
        if (FAILED(device->GetPixelShader(&actualShader)) || actualShader != trackedShader)
        {
            if (actualShader) actualShader->Release();
            return;
        }
        if (actualShader) actualShader->Release();

        // Requiring GetLevelDesc to report four D24X8 textures was not
        // portable across D3D9 wrappers: this client exposes its native
        // cascade resources with a driver-specific depth FourCC, so valid
        // receivers were rejected before either slider could run. Hence the
        // probe stays diagnostic only.
        firstStage = HasFourNativeCascades(device, 4) ? 4 : (HasFourNativeCascades(device, 5) ? 5 : -1);

        DWORD colorWrite = 0;
        if (FAILED(device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite)) || colorWrite == 0) return;

        if (isSoftnessReceiver) ApplySoftness(psHash);
        if (isStrengthReceiver) ApplyStrength(psHash);
    }

    ~SoftnessScope()
    {
        if (!device) return;
        if (softnessActive) device->SetPixelShaderConstantF(softnessStart, softnessOriginal[0], softnessCount);
        if (strengthShaderSwapped) device->SetPixelShader(originalShaderForRestore.Get());
    }

    SoftnessScope(const SoftnessScope&) = delete;
    SoftnessScope& operator=(const SoftnessScope&) = delete;

private:
    void ApplySoftness(uint64_t psHash)
    {
        bool oddRegistersOnly = false;
        UINT startRegister = 0, registerCount = 0;
        if (!FindReceiverRegisterLayout(psHash, startRegister, registerCount, oddRegistersOnly))
        {
            static std::unordered_set<uint64_t> loggedUnmatched;
            if (loggedUnmatched.insert(psHash).second)
            {
                std::ostringstream s;
                s << "[SOFTNESS] cascades bound at stage " << firstStage << " but psHash=0x" << std::hex << psHash << std::dec << " is not in the receiver whitelist\n";
                Append(s.str());
            }
            return;
        }

        float original[8][4]{}, adjusted[8][4]{};
        if (FAILED(device->GetPixelShaderConstantF(startRegister, original[0], registerCount))) return;
        std::copy(&original[0][0], &original[0][0] + registerCount * 4, &adjusted[0][0]);

        bool changed = false;
        for (UINT r = 0; r < registerCount; ++r)
        {
            const UINT absoluteRegister = startRegister + r;
            const bool usedOffset = oddRegistersOnly ? (absoluteRegister % 2 == 1) : true;
            const float x = adjusted[r][0], y = adjusted[r][1];
            const bool looksLikeUvOffset = usedOffset && std::abs(x) <= .002f && std::abs(y) <= .002f &&
                (std::abs(x) + std::abs(y)) > .00001f && std::abs(adjusted[r][2]) < .00001f &&
                std::abs(adjusted[r][3]) < .00001f;
            if (!looksLikeUvOffset) continue;
            adjusted[r][0] *= softnessScale;
            adjusted[r][1] *= softnessScale;
            changed = true;
        }
        static bool loggedNoOffsetFound = false;
        if (!changed)
        {
            if (!loggedNoOffsetFound)
            {
                loggedNoOffsetFound = true;
                std::ostringstream s;
                s << "[SOFTNESS] matched receiver psHash=0x" << std::hex << psHash << std::dec
                  << " at stage " << firstStage << " but none of the " << registerCount
                  << " registers from c" << startRegister << " looked like a UV offset - nothing scaled\n";
                Append(s.str());
            }
            return;
        }
        if (SUCCEEDED(device->SetPixelShaderConstantF(startRegister, adjusted[0], registerCount)))
        {
            softnessStart = startRegister; softnessCount = registerCount;
            std::copy(&original[0][0], &original[0][0] + registerCount * 4, &softnessOriginal[0][0]);
            softnessActive = true;
            static bool loggedActive = false;
            if (!loggedActive)
            {
                loggedActive = true;
                std::ostringstream s;
                s << "[SOFTNESS] ACTIVE: scaled registers c" << startRegister << ".." << (startRegister + registerCount - 1)
                  << " by " << softnessScale << "x for psHash=0x" << std::hex << psHash << std::dec
                  << " cascadeStage=" << firstStage << "\n";
                Append(s.str());
            }
        }
    }

    void ApplyStrength(uint64_t psHash)
    {
        if (!IsStrengthConfirmedHash(psHash)) return;

        IDirect3DPixelShader9* current = nullptr;
        if (FAILED(device->GetPixelShader(&current)) || !current) return;

        IDirect3DPixelShader9* patched = GetPatchedStrengthShader(device, psHash, current);
        if (!patched) { current->Release(); return; }

        if (SUCCEEDED(device->SetPixelShader(patched)))
        {
            originalShaderForRestore.Attach(current);
            strengthShaderSwapped = true;
            static bool loggedActive = false;
            if (!loggedActive)
            {
                loggedActive = true;
                std::ostringstream s;
                s << "[STRENGTH] ACTIVE: swapped patched shader for psHash=0x" << std::hex << psHash << std::dec << "\n";
                Append(s.str());
            }
        }
        else
        {
            current->Release();
        }
    }

    IDirect3DDevice9* device = nullptr;
    int firstStage = -1;
    bool softnessActive = false;
    UINT softnessStart = 0;
    UINT softnessCount = 0;
    float softnessOriginal[8][4]{};
    bool strengthShaderSwapped = false;
    Microsoft::WRL::ComPtr<IDirect3DPixelShader9> originalShaderForRestore;
};

inline void LogConstantsBlock(IDirect3DDevice9* d, std::ostringstream& s)
{
    if (!logConstants) return;
    float constants[16][4]{};
    if (SUCCEEDED(d->GetVertexShaderConstantF(0, constants[0], 16)))
    {
        s << "  VS c0-c15:\n";
        for (int i = 0; i < 16; ++i)
            s << "    c" << i << "={" << constants[i][0] << ',' << constants[i][1] << ',' << constants[i][2] << ',' << constants[i][3] << "}\n";
    }
    if (SUCCEEDED(d->GetPixelShaderConstantF(0, constants[0], 16)))
    {
        s << "  PS c0-c15:\n";
        for (int i = 0; i < 16; ++i)
            s << "    c" << i << "={" << constants[i][0] << ',' << constants[i][1] << ',' << constants[i][2] << ',' << constants[i][3] << "}\n";
    }
}

template<typename Shader>
inline void DumpShader(Shader* shader, uint64_t hash, const wchar_t* stage, std::unordered_set<uint64_t>& dumped)
{
    if (!shader || !hash || !dumped.insert(hash).second) return;
    ID3DBlob* blob = nullptr;
    if (FAILED(shader->GetFunction(&blob)) || !blob) return;
    const UINT size = static_cast<UINT>(blob->GetBufferSize());
    if (!size || size > 65536) { blob->Release(); return; }
    std::vector<BYTE> bytes(size);
    memcpy(bytes.data(), blob->GetBufferPointer(), size);
    blob->Release();

    wchar_t name[96]{};
    swprintf_s(name, L"\\%s_%016llx.bin", stage, static_cast<unsigned long long>(hash));
    HANDLE file = CreateFileW((dumpDirectory + name).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(file, bytes.data(), size, &written, nullptr);
        CloseHandle(file);
    }

    ID3DBlob* disassembly = nullptr;
    if (SUCCEEDED(D3DDisassemble(bytes.data(), size, D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING, nullptr, &disassembly, nullptr)) && disassembly)
    {
        swprintf_s(name, L"\\%s_%016llx.asm", stage, static_cast<unsigned long long>(hash));
        file = CreateFileW((dumpDirectory + name).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(file, disassembly->GetBufferPointer(), static_cast<DWORD>(disassembly->GetBufferSize()), &written, nullptr);
            CloseHandle(file);
        }
        disassembly->Release();
    }
}

inline bool EnsurePreviewShader(IDirect3DDevice9* d)
{
    if (previewOwner == d && previewShader) return true;
    if (previewShader) previewShader->Release();
    previewShader = nullptr;
    previewOwner = d;
    static const char source[] =
        "sampler2D shadowMap:register(s0); float4 tuning:register(c0);"
        "float4 main(float2 uv:TEXCOORD0):COLOR0{"
        "float depth=tex2D(shadowMap,uv).r;"
        "float visible=pow(saturate((1-depth)*tuning.x),.35);"
        "return float4(visible.xxx,1);}";
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main", "ps_3_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) errors->Release();
    if (FAILED(hr) || !code) return false;
    const HRESULT createHr = d->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &previewShader);
    code->Release();
    return SUCCEEDED(createHr);
}

inline void DrawPreview(IDirect3DDevice9* d)
{
    if (!enabled || !previewEnabled || !d || !EnsurePreviewShader(d)) return;
    int firstStage = -1;
    for (int start : { 5, 4 })
    {
        bool complete = true;
        for (int i = 0; i < 4; ++i) complete = complete && previewTextureByStage[start + i];
        if (complete) { firstStage = start; break; }
    }
    if (firstStage < 0) return;

    renderer::ScopedPassState state(d);
    D3DVIEWPORT9 viewport{};
    if (FAILED(d->GetViewport(&viewport))) return;
    struct Vertex { float x, y, z, rhw, u, v; };
    const float tile = std::clamp(float(viewport.Height) * .16f, 128.f, 230.f);
    const float gap = 5.f;
    const float left = float(viewport.X + viewport.Width) - (tile + gap) * 4.f - gap;
    const float top = float(viewport.Y) + gap;

    d->SetVertexShader(nullptr);
    d->SetPixelShader(previewShader);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    const float tuning[4] = { 64.f, 0, 0, 0 };
    d->SetPixelShaderConstantF(0, tuning, 1);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    for (int i = 0; i < 4; ++i)
    {
        const float x0 = left + (tile + gap) * i;
        const float x1 = x0 + tile;
        const float y0 = top;
        const float y1 = y0 + tile;
        Vertex quad[] = {
            { x0 - .5f, y0 - .5f, 0, 1, 0, 0 }, { x1 - .5f, y0 - .5f, 0, 1, 1, 0 },
            { x0 - .5f, y1 - .5f, 0, 1, 0, 1 }, { x1 - .5f, y1 - .5f, 0, 1, 1, 1 }
        };
        d->SetTexture(0, previewTextureByStage[firstStage + i]);
        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
    }
    d->SetTexture(0, nullptr);
}

inline void OnDraw(IDirect3DDevice9* d, const char* api, D3DPRIMITIVETYPE primitiveType, UINT primitiveCount,
                   uint64_t vsHash, uint64_t psHash, DWORD fvf, const void* vertexDecl,
                   bool alphaBlend, bool alphaTest, bool zEnable, bool zWrite)
{
    if (!enabled || !d) return;
    ++drawOrdinal;

    // Only the state that gates candidacy or enters the dedupe signature is
    // read here. The rest (cull/stencil/viewport/stage textures) feeds the
    // first-sighting log only and is collected where that log is built, so a
    // repeated candidate draw costs three render-state reads instead of the
    // ~37 driver calls the full dump would take.
    DWORD colorWrite = 0xf;
    d->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
    const bool offscreen = currentRtValid && currentRtTexture &&
        (currentRtDesc.Width < std::max(1u, backBufferWidth) || currentRtDesc.Height < std::max(1u, backBufferHeight));
    const bool depthOnly = colorWrite == 0 && zEnable && zWrite;
    const bool projectedBlend = !currentRtTexture && alphaBlend && zEnable && !zWrite;
    const bool samplesPriorRt = std::any_of(boundFormerRt.begin(), boundFormerRt.end(), [](bool value) { return value; });
    const bool candidate = offscreen || depthOnly || projectedBlend || samplesPriorRt;
    if (!candidate) return;

    DWORD srcBlend = 0, dstBlend = 0;
    d->GetRenderState(D3DRS_SRCBLEND, &srcBlend);
    d->GetRenderState(D3DRS_DESTBLEND, &dstBlend);

    ++candidateDrawsThisFrame;
    uint64_t signature = 0xcbf29ce484222325ull;
    signature = Mix(signature, vsHash); signature = Mix(signature, psHash);
    signature = Mix(signature, currentRtDesc.Width); signature = Mix(signature, currentRtDesc.Height);
    signature = Mix(signature, uint32_t(currentRtDesc.Format)); signature = Mix(signature, colorWrite);
    signature = Mix(signature, reinterpret_cast<uintptr_t>(currentDsTexture));
    const uint64_t stateBits = uint64_t(alphaBlend) | (uint64_t(alphaTest) << 1) |
        (uint64_t(zEnable) << 2) | (uint64_t(zWrite) << 3);
    signature = Mix(signature, stateBits);
    signature = Mix(signature, srcBlend | (uint64_t(dstBlend) << 16));

    if (currentRtTexture)
    {
        if (TargetRecord* record = FindTarget(currentRtTexture))
        {
            record->lastProducerVs = vsHash;
            record->lastProducerPs = psHash;
            record->lastProducerFrame = frame;
            record->lastProducerDraw = drawOrdinal;
        }
    }
    if (currentDsTexture)
    {
        if (TargetRecord* record = FindTarget(currentDsTexture))
        {
            record->lastProducerVs = vsHash;
            record->lastProducerPs = psHash;
            record->lastProducerFrame = frame;
            record->lastProducerDraw = drawOrdinal;
        }
    }

    const bool first = loggedSignatures.size() < maxUniqueCandidates && loggedSignatures.insert(signature).second;
    if (first)
    {
        // First sighting of this signature: the full state dump. Everything
        // here - stage textures, sampler states, constants, shader dumps -
        // runs once per unique candidate, never per draw.
        DWORD cull = 0, stencil = 0;
        d->GetRenderState(D3DRS_CULLMODE, &cull);
        d->GetRenderState(D3DRS_STENCILENABLE, &stencil);
        D3DVIEWPORT9 viewport{};
        d->GetViewport(&viewport);

        std::array<IDirect3DBaseTexture9*, 16> rawTextures{};
        for (DWORD stage = 0; stage < rawTextures.size(); ++stage)
            d->GetTexture(stage, &rawTextures[stage]);

        IDirect3DSurface9* depth = nullptr;
        D3DSURFACE_DESC depthDesc{};
        if (SUCCEEDED(d->GetDepthStencilSurface(&depth)) && depth)
        {
            depth->GetDesc(&depthDesc);
            depth->Release();
        }

        std::ostringstream s;
        s << std::setprecision(6)
          << "[CANDIDATE] frame=" << frame << " draw=" << drawOrdinal << " api=" << api
          << " primitive=" << PrimitiveName(primitiveType) << " count=" << primitiveCount << " reasons="
          << (offscreen ? "offscreen_rt," : "") << (depthOnly ? "depth_only," : "")
          << (projectedBlend ? "projected_blend," : "") << (samplesPriorRt ? "samples_prior_rt," : "") << "\n"
          << "  VS=0x" << std::hex << vsHash << " PS=0x" << psHash << std::dec
          << " vdecl=" << vertexDecl << " fvf=0x" << std::hex << fvf << std::dec << "\n"
          << "  RT=" << currentRtDesc.Width << 'x' << currentRtDesc.Height << ' ' << FormatName(currentRtDesc.Format)
          << '(' << unsigned(currentRtDesc.Format) << ") texture=" << currentRtTexture << "\n"
          << "  DS=" << depthDesc.Width << 'x' << depthDesc.Height << ' ' << FormatName(depthDesc.Format)
          << '(' << unsigned(depthDesc.Format) << ") texture=" << currentDsTexture
          << " viewport={" << viewport.X << ',' << viewport.Y << ','
          << viewport.Width << ',' << viewport.Height << ',' << viewport.MinZ << ',' << viewport.MaxZ << "}\n"
          << "  state colorWrite=0x" << std::hex << colorWrite << std::dec
          << " alphaBlend=" << alphaBlend << " src=" << srcBlend << " dst=" << dstBlend
          << " alphaTest=" << alphaTest << " zEnable=" << zEnable << " zWrite=" << zWrite
          << " cull=" << cull << " stencil=" << stencil << "\n";

        for (DWORD stage = 0; stage < rawTextures.size(); ++stage)
        {
            IDirect3DBaseTexture9* texture = rawTextures[stage];
            if (!texture) continue;
            D3DRESOURCETYPE type = texture->GetType();
            if (type == D3DRTYPE_TEXTURE)
            {
                D3DSURFACE_DESC td{};
                static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &td);
                s << "  s" << stage << '=' << texture << ' ' << td.Width << 'x' << td.Height << ' '
                  << FormatName(td.Format) << '(' << unsigned(td.Format) << ')'
                  << (FindTarget(texture) ? " [FORMER_RT]" : "") << "\n";
            }
            else s << "  s" << stage << '=' << texture << " type=" << unsigned(type) << "\n";
        }
        LogConstantsBlock(d, s);
        for (DWORD stage = 4; stage <= 8; ++stage)
        {
            if (!rawTextures[stage] || !FindTarget(rawTextures[stage]) || !FindTarget(rawTextures[stage])->depthTarget) continue;
            DWORD minFilter = 0, magFilter = 0, mipFilter = 0, addressU = 0, addressV = 0, border = 0;
            d->GetSamplerState(stage, D3DSAMP_MINFILTER, &minFilter);
            d->GetSamplerState(stage, D3DSAMP_MAGFILTER, &magFilter);
            d->GetSamplerState(stage, D3DSAMP_MIPFILTER, &mipFilter);
            d->GetSamplerState(stage, D3DSAMP_ADDRESSU, &addressU);
            d->GetSamplerState(stage, D3DSAMP_ADDRESSV, &addressV);
            d->GetSamplerState(stage, D3DSAMP_BORDERCOLOR, &border);
            s << "  sampler s" << stage << " min=" << minFilter << " mag=" << magFilter << " mip=" << mipFilter
              << " addressU=" << addressU << " addressV=" << addressV << " border=0x" << std::hex << border << std::dec << "\n";
        }
        if (samplesPriorRt)
        {
            IDirect3DVertexShader9* vs = nullptr;
            IDirect3DPixelShader9* ps = nullptr;
            if (SUCCEEDED(d->GetVertexShader(&vs)) && vs)
            {
                DumpShader(vs, vsHash, L"vs", dumpedVertexShaders);
                vs->Release();
            }
            if (SUCCEEDED(d->GetPixelShader(&ps)) && ps)
            {
                DumpShader(ps, psHash, L"ps", dumpedPixelShaders);
                ps->Release();
            }
        }
        s << '\n';
        Append(s.str());

        for (auto* texture : rawTextures) if (texture) texture->Release();
    }
}

inline void OnPresent(IDirect3DDevice9* d)
{
    if (!enabled) return;
    ++frame;
    if (d && (!backBufferWidth || !backBufferHeight))
    {
        IDirect3DSurface9* rt = nullptr;
        if (SUCCEEDED(d->GetRenderTarget(0, &rt)) && rt)
        {
            D3DSURFACE_DESC desc{};
            if (SUCCEEDED(rt->GetDesc(&desc))) { backBufferWidth = desc.Width; backBufferHeight = desc.Height; }
            rt->Release();
        }
    }
    if (frame % summaryInterval == 0)
    {
        std::ostringstream s;
        s << "[SUMMARY] frame=" << frame << " unique_candidates=" << loggedSignatures.size()
          << " tracked_texture_rts=" << targets.size() << " last_frame_candidate_draws=" << candidateDrawsThisFrame
          << " last_frame_rt_reuse_events=" << rtReuseEventsThisFrame << "\n";
        Append(s.str());
    }
    drawOrdinal = 0;
    candidateDrawsThisFrame = 0;
    rtReuseEventsThisFrame = 0;
    for (auto& target : targets) target.reuseLogged = false;
}
}
