#include "src/Environment/LocationTuning.h"
#pragma once

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "src/D3D9/ScopedPassState.h"
#include "src/Effects/ShadowReceiverBytecode.h"

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
    // 0x449b66c5fb94fede used to sit in this list. A live session reported it
    // ACTIVE with `cascadeStage=-1` - the four native cascades were not bound
    // at its draws, which no receiver of theirs can say - while the shader that
    // WAS drawn with the cascades bound at stage 4 was refused as unknown. It
    // has been removed: a whitelist entry that fires without the cascades is
    // scaling constants that are not a Poisson kernel.
    static constexpr uint64_t kFamilyB[] = { // register layout: c3, count 8
        0x08b17d1abaad8eceull, 0x634793193e26059dull,
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

// c12 turned out to be a `def`-declared shader literal (compiled into the
// instruction stream), not a runtime constant - SetPixelShaderConstantF on
// it is a silent no-op, confirmed live (0% difference across the slider
// range). The only way to actually change it is to patch the compiled
// bytecode's literal and use that as a replacement shader for the exact
// same receiver draws the scope validates - the same "swap the shader for one
// draw, restore after" technique WaterEffect already uses, just building the
// replacement once instead of writing HLSL by hand.
//
// The instruction is a fixed shape and the walk below reads it as one:
//   0x05000051 (D3DSIO_DEF, length=5) [dest register token] [4 raw floats]
// The literal is then identified by ITS OWN VALUES, not by which register the
// shader was said to use: the client's shadow curve is
// `visibility * darkening + floor` with darkening + floor == 1, so the pair is
// the one place in a receiver where two complementary fractions sum to one
// (see renderer::shadowreceiver::ShadowDarkeningPair). That is what lets the
// strength slider reach a receiver whose registers nobody has disassembled -
// the same reason the softness side learned to read the bytecode.
//
// Patching only rewrites those two floats in a private copy of the
// bytecode; instruction count, opcodes and control flow are untouched.
inline bool PatchStrengthLiteral(const std::vector<BYTE>& original,
                                 float darkening, float floor, std::vector<DWORD>& patched,
                                 int* patchedConstant = nullptr)
{
    if (original.size() % 4 != 0 || original.size() < 24) return false;
    patched.assign(original.size() / 4, 0);
    std::memcpy(patched.data(), original.data(), original.size());
    renderer::shadowreceiver::DefLiteral literals[8];
    renderer::shadowreceiver::Scan scan;
    if (!renderer::shadowreceiver::ScanBytecode(
            reinterpret_cast<const std::uint32_t*>(original.data()), original.size() / 4,
            scan, literals, std::size(literals)))
        return false;
    for (const auto& literal : literals)
    {
        const int pair = renderer::shadowreceiver::ShadowDarkeningPair(literal.value);
        if (pair < 0) continue; // not the shadow curve: do not touch it
        std::memcpy(patched.data() + literal.dataWord + size_t(pair), &darkening, 4);
        std::memcpy(patched.data() + literal.dataWord + size_t(pair) + 1, &floor, 4);
        if (patchedConstant) *patchedConstant = int(literal.constant);
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

// Returns a shader identical to `original` except its shadow-curve literal
// scaled by the current strengthScale, rebuilding only when the slider
// actually changes. Returns null (leave the native shader bound) if no `def`
// literal in the stream has the darkening/floor shape, or on any failure -
// this must never be the reason a receiver draw doesn't render.
inline IDirect3DPixelShader9* GetPatchedStrengthShader(IDirect3DDevice9* d, uint64_t hash, IDirect3DPixelShader9* original)
{
    auto& entry = patchedStrengthShaders[hash];
    if (entry.shader && std::abs(entry.builtForScale - strengthScale) < .001f) return entry.shader.Get();

    UINT size = 0;
    if (!original || FAILED(original->GetFunction(nullptr, &size)) || !size || size > 65536) return nullptr;
    std::vector<BYTE> bytecode(size);
    if (FAILED(original->GetFunction(bytecode.data(), &size))) return nullptr;

    const float darkening = std::clamp(.3f * strengthScale, 0.f, 1.f);
    std::vector<DWORD> patched;
    int patchedConstant = -1;
    if (!PatchStrengthLiteral(bytecode, darkening, 1.f - darkening, patched, &patchedConstant))
    {
        static std::unordered_set<uint64_t> loggedNoCurve;
        if (loggedNoCurve.size() < 64 && loggedNoCurve.insert(hash).second)
        {
            std::ostringstream s;
            s << "[STRENGTH] psHash=0x" << std::hex << hash << std::dec
              << " declares no darkening/floor literal, so the strength slider cannot change its shadows\n";
            Append(s.str());
        }
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IDirect3DPixelShader9> created;
    if (FAILED(d->CreatePixelShader(patched.data(), created.GetAddressOf()))) return nullptr;

    entry.shader = created;
    entry.builtForScale = strengthScale;
    std::ostringstream s;
    s << "[STRENGTH] built patched shader for psHash=0x" << std::hex << hash << std::dec
      << " (literal c" << patchedConstant << ") darkening 0.3 -> " << darkening << "\n";
    Append(s.str());
    return entry.shader.Get();
}

// One of the client's four native cascades: a square depth texture of the
// cascade size the client uses (2048 square at the shipped settings, 1024 and
// 4096 have both been seen in neighbouring quality tiers).
//
// This used to demand D24X8 exactly, and the comment that replaced the check
// said why it was loosened - the client exposes the cascades through a
// driver-specific depth FourCC on some drivers, so a valid receiver was
// rejected as "no cascades". Requiring the exact format is what makes a probe
// answer "no" for water that is there; requiring a DEPTH format of square,
// cascade-sized dimensions is the property the question is actually about.
inline bool IsCascadeShaped(const D3DSURFACE_DESC& desc)
{
    if (desc.Width != desc.Height || desc.Width < 1024 || desc.Width > 4096) return false;
    switch (desc.Format)
    {
    case D3DFMT_D16: case D3DFMT_D15S1: case D3DFMT_D24X8: case D3DFMT_D24S8:
    case D3DFMT_D24X4S4: case D3DFMT_D32: case D3DFMT_D32F_LOCKABLE:
        return true;
    default: break;
    }
    // Every driver-private (FourCC) format lives in the upper half of the
    // D3DFORMAT range; the client's cascades have been reported with one.
    return static_cast<UINT>(desc.Format) > 0x7FFFFFFFu;
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
            valid = SUCCEEDED(texture->GetLevelDesc(0, &desc)) && IsCascadeShaped(desc);
        }
        baseTexture->Release();
        if (!valid) return false;
    }
    return true;
}

// The number of sampler stages in the native cascade window a pixel shader's
// own bytecode samples. Four is a receiver; the capture's "cascades bound at
// stage 4" is the runtime half of the same statement.
inline unsigned CascadeStageProbe(IDirect3DDevice9* device)
{
    for (int firstStage = 4; firstStage <= 8; ++firstStage)
        if (HasFourNativeCascades(device, firstStage)) return static_cast<unsigned>(firstStage);
    return 0u;
}

// An enhancement that does nothing has to be able to say where it stopped, or
// the next report is a guess again. Each reason is written once per session.
inline void LogShadowDecline(const char* reason, const std::string& message)
{
    static std::unordered_set<std::string> logged;
    if (logged.size() >= 32 || !logged.insert(reason).second) return;
    Append("[SHADOW] declined: " + message + "\n");
}

// How many receiver draws the session has decided about, and how many it
// actually changed. Written every 3600 of them - a minute at 60 fps with a
// normal amount of terrain in view - so a log from a session that "does
// nothing" says whether the scope runs at all.
inline void NoteShadowDraw(bool softened, bool strengthened)
{
    static uint64_t draws = 0, soft = 0, strong = 0;
    ++draws;
    if (softened) ++soft;
    if (strengthened) ++strong;
    if (draws % 3600 != 0) return;
    std::ostringstream s;
    s << "[SHADOW] " << draws << " shadow-receiver draw(s) so far: " << soft
      << " softened, " << strong << " strengthened, softness "
      << softnessScale << "x strength " << strengthScale << "x\n";
    Append(s.str());
}

inline bool ReadShaderBytecode(IDirect3DPixelShader9* shader, std::vector<BYTE>& bytes)
{
    UINT size = 0;
    if (!shader || FAILED(shader->GetFunction(nullptr, &size)) || !size || size > 65536) return false;
    bytes.resize(size);
    if (FAILED(shader->GetFunction(bytes.data(), &size))) return false;
    bytes.resize(size);
    return true;
}

// A pixel shader this session identified as a receiver by its own bytecode,
// and the constants that bytecode reads as two-component UV offsets. The mask
// is what the softness slider may scale; every draw still has to pass the
// value test (a register that does not look like a tiny offset in THIS draw is
// left alone), so a shader that uses c7.xy for something unrelated in another
// branch of its material code is not touched there.
struct LearnedReceiver
{
    std::uint32_t xyConstants = 0;
    unsigned cascadeStages = 0;
    unsigned cascadeProbe = 0;
    // Learned without readable bytecode: the device had the four cascades
    // bound, so this draw is a receiver, but which of its constants are the
    // kernel's offsets had to be left to the value test over a bounded window.
    bool fallback = false;
};

inline std::unordered_map<uint64_t, LearnedReceiver>& LearnedReceivers()
{
    static std::unordered_map<uint64_t, LearnedReceiver> learned;
    return learned;
}

// Learn a receiver ONCE per hash, on its first draw. Two independent
// statements have to agree before anything is scaled:
//   * the four native cascades are bound to the device at this draw, and
//   * the shader itself samples the cascade window,
// and the shader must name at least one constant it reads as a two-component
// offset. A hash that fails any of these is recorded as answered and never
// probed again, so a frame full of materials costs one probe per material and
// not one per draw.
inline const LearnedReceiver* LearnReceiver(IDirect3DDevice9* device, uint64_t psHash,
                                           IDirect3DPixelShader9* shader)
{
    static std::unordered_set<uint64_t> attempted;
    if (!psHash || !shader || attempted.size() >= 4096 || !attempted.insert(psHash).second)
        return nullptr;

    std::vector<BYTE> bytecode;
    renderer::shadowreceiver::Scan scan;
    const bool scanned = ReadShaderBytecode(shader, bytecode) &&
        renderer::shadowreceiver::ScanBytecode(
            reinterpret_cast<const std::uint32_t*>(bytecode.data()), bytecode.size() / 4, scan);
    if (!scanned)
    {
        // No bytecode to read (a wrapped driver, a shader created by a path
        // that does not keep it). The device still knows whether this draw has
        // the four cascades bound, and that is the same evidence the hand-made
        // whitelist was built from - so a bounded window is allowed to stand in
        // for the bytecode, with every register still passing the value test.
        const unsigned probe = CascadeStageProbe(device);
        if (!probe)
        {
            LogShadowDecline("unreadable",
                "a pixel shader's bytecode could not be read and no cascades were bound, so it is neither a "
                "receiver nor a candidate");
            return nullptr;
        }
        LearnedReceiver fallback;
        fallback.xyConstants = 0xFFFFu; // c0..c15: the offsets live low
        fallback.cascadeProbe = probe;
        fallback.fallback = true;
        auto placeholder = LearnedReceivers().emplace(psHash, fallback);
        std::ostringstream s;
        s << "[SOFTNESS] LEARNED receiver psHash=0x" << std::hex << psHash << std::dec
          << " without readable bytecode (cascades bound at stage " << probe
          << "): scanning c0..c15, each register still has to hold a tiny two-component offset\n";
        Append(s.str());
        return &placeholder.first->second;
    }

    const unsigned cascades = renderer::shadowreceiver::CascadeStageCount(scan.samplerStages);
    // The device is only asked when the shader itself says something that could
    // be a kernel: four GetTexture probes per unknown material, not per draw.
    const unsigned probe = scan.xyConstants ? CascadeStageProbe(device) : 0u;
    if (!scan.xyConstants || (cascades < 3 && !probe))
    {
        std::ostringstream s;
        s << "[SOFTNESS] psHash=0x" << std::hex << psHash << std::dec
          << " is not a receiver this session can prove: samples " << cascades
          << " cascade slots, device had them at stage ";
        if (probe) s << probe; else s << "none";
        s << ", two-component UV offsets " << (scan.xyConstants ? "yes" : "none")
          << " - left alone\n";
        Append(s.str());
        return nullptr;
    }

    LearnedReceiver learned;
    learned.xyConstants = scan.xyConstants;
    learned.cascadeStages = cascades;
    learned.cascadeProbe = probe;
    auto inserted = LearnedReceivers().emplace(psHash, learned);
    std::ostringstream s;
    s << "[SOFTNESS] LEARNED receiver psHash=0x" << std::hex << psHash << std::dec
      << " (samples " << cascades << " cascade slots, device had them at stage ";
    if (probe) s << probe; else s << "s5-or-lower";
    s << ") - the softness slider may scale";
    for (unsigned reg = 0; reg < 32; ++reg)
        if (learned.xyConstants & (1u << reg)) s << " c" << reg;
    s << "\n";
    Append(s.str());
    return &inserted.first->second;
}

// The value test every softness path shares: a Poisson kernel's offsets are a
// few ten-thousandths of a UV, two components, with nothing in .z/.w. A
// register that fails this in the draw at hand is left exactly as the client
// wrote it, whatever the bytecode says about it elsewhere.
inline bool LooksLikeUvOffset(const float value[4]) noexcept
{
    const float x = value[0], y = value[1];
    return std::abs(x) <= .002f && std::abs(y) <= .002f &&
        (std::abs(x) + std::abs(y)) > .00001f &&
        std::abs(value[2]) < .00001f && std::abs(value[3]) < .00001f;
}

class SoftnessScope
{
public:
    SoftnessScope(IDirect3DDevice9* d, uint64_t psHash, IDirect3DPixelShader9* trackedShader) : device(d)
    {
        const bool needSoftness = std::abs(softnessScale - 1.f) > .001f;
        const bool needStrength = std::abs(strengthScale - 1.f) > .001f;
        if (!enhancementEnabled)
            LogShadowDecline("toggle", "the NATIVE SHADOWS toggle is off ([NativeShadows] Enabled=0)");
        if (!needSoftness && !needStrength)
            LogShadowDecline("neutral", "softness is 100% and strength is 100% - both sliders are at their native value");
        if (currentRtTexture)
            LogShadowDecline("offscreen", "this draw renders into a texture, not into the player's view");
        if (!enhancementEnabled || !device || (!needSoftness && !needStrength) || currentRtTexture)
            return;

        // This scope runs on EVERY back-buffer draw, so the cheap gate comes
        // first: the verified families are a dozen in-memory comparisons, and
        // the handful of hashes a session has already learned are one map
        // lookup. Only a pixel shader that is neither pays for a bytecode read,
        // and that happens once per hash for the whole session.
        UINT startRegister = 0, registerCount = 0;
        bool oddRegistersOnly = false;
        const bool windowed = FindReceiverRegisterLayout(psHash, startRegister, registerCount, oddRegistersOnly);
        bool strengthEligible = IsStrengthConfirmedHash(psHash);
        const LearnedReceiver* learned = nullptr;
        bool bound = false;
        if (!windowed && !strengthEligible)
        {
            const auto found = LearnedReceivers().find(psHash);
            if (found != LearnedReceivers().end()) learned = &found->second;
            else
            {
                // The one path that READS the shader object. trackedState holds
                // the client's raw pointer without a reference, so it is only
                // safe to read once the device confirms that same object is
                // what is bound right now - otherwise a recycled identity's
                // bytes are read as this shader's.
                IDirect3DPixelShader9* candidate = nullptr;
                if (FAILED(device->GetPixelShader(&candidate)) || candidate != trackedShader)
                {
                    if (candidate) candidate->Release();
                    LogShadowDecline("bound", "the tracked pixel shader is not the one bound to the device");
                    return;
                }
                if (candidate) candidate->Release();
                bound = true;
                learned = LearnReceiver(device, psHash, trackedShader);
            }
            if (!learned) return;
            strengthEligible = true;
        }
        else if (windowed && !strengthEligible)
        {
            // A verified softness family whose darkening literal nobody has
            // confirmed. The shape test in PatchStrengthLiteral still has to
            // find the curve before a patched copy is built, so this only
            // widens where the strength slider may look, never what it may
            // touch.
            strengthEligible = needStrength;
        }

        if (!bound)
        {
            IDirect3DPixelShader9* actualShader = nullptr;
            if (FAILED(device->GetPixelShader(&actualShader)) || actualShader != trackedShader)
            {
                if (actualShader) actualShader->Release();
                LogShadowDecline("bound", "the tracked pixel shader is not the one bound to the device");
                return;
            }
            if (actualShader) actualShader->Release();
        }

        DWORD colorWrite = 0;
        if (FAILED(device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite)) || colorWrite == 0)
        {
            LogShadowDecline("caster", "colour writes are off - a shadow caster pass, not a receiver");
            return;
        }

        if (needSoftness)
        {
            if (windowed) ApplySoftnessWindow(psHash, startRegister, registerCount, oddRegistersOnly);
            else if (learned) ApplySoftnessMask(psHash, learned->xyConstants);
        }
        if (needStrength && strengthEligible) ApplyStrength(psHash);
        NoteShadowDraw(softnessActive, strengthShaderSwapped);
    }

    ~SoftnessScope()
    {
        if (!device) return;
        if (softnessActive) device->SetPixelShaderConstantF(softnessStart, &softnessOriginal[0][0], softnessCount);
        if (strengthShaderSwapped) device->SetPixelShader(originalShaderForRestore.Get());
    }

    SoftnessScope(const SoftnessScope&) = delete;
    SoftnessScope& operator=(const SoftnessScope&) = delete;

private:
    // The verified families: a window of registers whose odd (or all)
    // entries the client uses as UV offsets.
    void ApplySoftnessWindow(uint64_t psHash, UINT startRegister, UINT registerCount, bool oddRegistersOnly)
    {
        float original[8][4]{}, adjusted[8][4]{};
        if (registerCount == 0 || registerCount > 8) return;
        if (FAILED(device->GetPixelShaderConstantF(startRegister, original[0], registerCount))) return;
        std::copy(&original[0][0], &original[0][0] + registerCount * 4, &adjusted[0][0]);

        bool changed = false;
        for (UINT r = 0; r < registerCount; ++r)
        {
            const UINT absoluteRegister = startRegister + r;
            if (oddRegistersOnly && absoluteRegister % 2 == 0) continue;
            if (!LooksLikeUvOffset(adjusted[r])) continue;
            adjusted[r][0] *= softnessScale;
            adjusted[r][1] *= softnessScale;
            changed = true;
        }
        if (!changed)
        {
            static bool loggedNoOffsetFound = false;
            if (!loggedNoOffsetFound)
            {
                loggedNoOffsetFound = true;
                std::ostringstream s;
                s << "[SOFTNESS] matched receiver psHash=0x" << std::hex << psHash << std::dec
                  << " but none of the " << registerCount << " registers from c" << startRegister
                  << " looked like a UV offset in this draw - nothing scaled\n";
                Append(s.str());
            }
            return;
        }
        Commit(psHash, startRegister, registerCount, original, adjusted, "verified");
    }

    // A receiver learned from its own bytecode: exactly the constants it reads
    // as two-component offsets, each still subject to the value test.
    void ApplySoftnessMask(uint64_t psHash, std::uint32_t mask)
    {
        if (!mask) return;
        UINT first = 0, last = 31;
        while (first < 32 && !(mask & (1u << first))) ++first;
        while (last > first && !(mask & (1u << last))) --last;
        if (first >= 32) return;
        const UINT count = last - first + 1;
        if (count > 32) return;

        float original[32][4]{}, adjusted[32][4]{};
        if (FAILED(device->GetPixelShaderConstantF(first, original[0], count))) return;
        std::copy(&original[0][0], &original[0][0] + count * 4, &adjusted[0][0]);

        bool changed = false;
        for (UINT r = 0; r < count; ++r)
        {
            if (!(mask & (1u << (first + r)))) continue;
            if (!LooksLikeUvOffset(adjusted[r])) continue;
            adjusted[r][0] *= softnessScale;
            adjusted[r][1] *= softnessScale;
            changed = true;
        }
        if (!changed)
        {
            static bool loggedMaskMiss = false;
            if (!loggedMaskMiss)
            {
                loggedMaskMiss = true;
                std::ostringstream s;
                s << "[SOFTNESS] learned receiver psHash=0x" << std::hex << psHash << std::dec
                  << " declared UV-offset constants but none of them held a tiny offset in this draw\n";
                Append(s.str());
            }
            return;
        }
        Commit(psHash, first, count, original, adjusted, "learned");
    }

    void Commit(uint64_t psHash, UINT startRegister, UINT count, const float original[][4],
                const float adjusted[][4], const char* how)
    {
        if (FAILED(device->SetPixelShaderConstantF(startRegister, &adjusted[0][0], count))) return;
        softnessStart = startRegister;
        softnessCount = count;
        std::copy(&original[0][0], &original[0][0] + count * 4, &softnessOriginal[0][0]);
        softnessActive = true;
        if (!loggedActive.exchange(true))
        {
            // Logged once per session, and only here: the cascade probe is four
            // driver round trips, which is affordable for one line and not for
            // every receiver draw of every frame.
            const unsigned probe = CascadeStageProbe(device);
            std::ostringstream s;
            s << "[SOFTNESS] ACTIVE (" << how << "): scaled registers c" << startRegister
              << ".." << (startRegister + count - 1) << " by " << softnessScale
              << "x for psHash=0x" << std::hex << psHash << std::dec
              << " cascadeStage=";
            if (probe) s << probe; else s << -1;
            s << "\n";
            Append(s.str());
        }
    }

    void ApplyStrength(uint64_t psHash)
    {
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
    bool softnessActive = false;
    UINT softnessStart = 0;
    UINT softnessCount = 0;
    float softnessOriginal[32][4]{};
    bool strengthShaderSwapped = false;
    static inline std::atomic<bool> loggedActive{false};
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
    UINT size = 0;
    if (FAILED(shader->GetFunction(nullptr, &size)) || !size || size > 65536) return;
    std::vector<BYTE> bytes(size);
    if (FAILED(shader->GetFunction(bytes.data(), &size))) return;

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
    if (SUCCEEDED(D3DDisassemble(bytes.data(), size, D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING, nullptr, &disassembly)) && disassembly)
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
