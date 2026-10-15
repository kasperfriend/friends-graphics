#pragma once
#include <d3d9.h>
#include <wrl/client.h>
#include <array>
#include "WaterPolicy.h"
#include "../Core/ShaderCache.h"
#include "../D3D9/TrackedRenderState.h"

namespace renderer::water
{
    // One structural reading of the draw, shared by replacement, capture,
    // classification and diagnostics so no path can disagree about what liquid
    // is. `candidate` names the draws that carry the liquid texture signature
    // but are not approved; that is the only way a water family missing from
    // the captured shader lists becomes visible in a player's log.
    struct Observation
    {
        bool liquid = false;
        bool candidate = false;
        bool vertexShaderBound = false;
        bool pixelShaderBound = false;
        unsigned width[2] = {0, 0};
        unsigned height[2] = {0, 0};
        uint64_t vsHash = 0;
        uint64_t psHash = 0;
    };

    // GetTexture is still checked against the live device state (state blocks
    // can bypass SetTexture hooks), but most scene draws reuse the same bound
    // resources. Cache each live texture's level-zero descriptor so a frame no
    // longer asks the driver for both descriptions twice per world draw.
    struct TextureDimensionCache
    {
        Microsoft::WRL::ComPtr<IDirect3DBaseTexture9> texture;
        D3DSURFACE_DESC desc{};
        bool valid = false;
    };
    inline IDirect3DDevice9* g_observationDevice = nullptr;
    inline std::array<TextureDimensionCache, 2> g_textureDimensions{};

    inline void ResetObservationCache(IDirect3DDevice9* device = nullptr)
    {
        for (auto& entry : g_textureDimensions) {
            entry.texture.Reset();
            entry.desc = {};
            entry.valid = false;
        }
        g_observationDevice = device;
    }

    inline Observation Observe(IDirect3DDevice9* device)
    {
        Observation out;
        if (!device) return out;
        if (g_observationDevice != device)
            ResetObservationCache(device);

        D3DSURFACE_DESC desc[2]{};
        for (unsigned slot = 0; slot < 2; ++slot)
        {
            Microsoft::WRL::ComPtr<IDirect3DBaseTexture9> texture;
            auto& cached = g_textureDimensions[slot];
            if (FAILED(device->GetTexture(slot, texture.GetAddressOf())))
            {
                cached.texture.Reset();
                cached.valid = false;
                return out;
            }
            if (!texture || texture->GetType() != D3DRTYPE_TEXTURE)
            {
                cached.texture.Reset();
                cached.valid = false;
                return out;
            }
            if (cached.texture.Get() != texture.Get() || !cached.valid)
            {
                D3DSURFACE_DESC current{};
                if (FAILED(static_cast<IDirect3DTexture9*>(texture.Get())->GetLevelDesc(0, &current)))
                {
                    cached.texture = texture;
                    cached.valid = false;
                    return out;
                }
                cached.texture = texture;
                cached.desc = current;
                cached.valid = true;
            }
            desc[slot] = cached.desc;
            out.width[slot] = desc[slot].Width;
            out.height[slot] = desc[slot].Height;
        }
        out.candidate = IsLiquidCandidate(desc[0].Width, desc[0].Height,
                                          desc[1].Width, desc[1].Height);
        // The canonical layout is the supported liquid signature on its own, so
        // unknown shader variants keep working and nothing is hashed per draw.
        if (IsMaterialLayout(desc[0].Width, desc[0].Height, desc[1].Width, desc[1].Height))
        {
            out.liquid = true;
            return out;
        }
        if (!out.candidate) return out;

        // StateBlock::Apply can bypass SetShader hooks. Only use the mirror's
        // hashes when its identities match the shaders ACTUALLY bound now.
        Microsoft::WRL::ComPtr<IDirect3DVertexShader9> vs;
        Microsoft::WRL::ComPtr<IDirect3DPixelShader9> ps;
        if (FAILED(device->GetVertexShader(vs.GetAddressOf())) || !vs ||
            FAILED(device->GetPixelShader(ps.GetAddressOf())) || !ps)
            return out;
        out.vertexShaderBound = true;
        out.pixelShaderBound = true;
        auto& cache = ShaderCache::Instance();
        out.vsHash = g_trackedState.currentVS == vs.Get() && g_trackedState.vsHash
            ? g_trackedState.vsHash : cache.GetShaderHash(vs.Get());
        out.psHash = g_trackedState.currentPS == ps.Get() && g_trackedState.psHash
            ? g_trackedState.psHash : cache.GetShaderHash(ps.Get());
        out.liquid = IsMaterialLayout(desc[0].Width, desc[0].Height,
                                      desc[1].Width, desc[1].Height,
                                      out.vsHash, out.psHash);
        return out;
    }

    inline bool IsMaterial(IDirect3DDevice9* device) { return Observe(device).liquid; }
}
