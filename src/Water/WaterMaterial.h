#pragma once
#include <d3d9.h>
#include <wrl/client.h>
#include "WaterPolicy.h"
#include "../Core/ShaderCache.h"
#include "../D3D9/TrackedRenderState.h"

namespace renderer::water
{
    inline bool IsMaterial(IDirect3DDevice9* device)
    {
        if (!device) return false;
        D3DSURFACE_DESC desc[2]{};
        for (unsigned slot = 0; slot < 2; ++slot)
        {
            Microsoft::WRL::ComPtr<IDirect3DBaseTexture9> texture;
            if (FAILED(device->GetTexture(slot, texture.GetAddressOf())) || !texture ||
                texture->GetType() != D3DRTYPE_TEXTURE ||
                FAILED(static_cast<IDirect3DTexture9*>(texture.Get())->GetLevelDesc(0, &desc[slot])))
                return false;
        }
        if (IsMaterialLayout(desc[0].Width, desc[0].Height, desc[1].Width, desc[1].Height))
            return true;
        if (!IsRippleLookup(desc[0].Width, desc[0].Height) ||
            !desc[1].Width || desc[1].Width != desc[1].Height)
            return false;

        // StateBlock::Apply can bypass SetShader hooks. Only use the mirror's
        // hashes when its identities match the shaders ACTUALLY bound now.
        Microsoft::WRL::ComPtr<IDirect3DVertexShader9> vs;
        Microsoft::WRL::ComPtr<IDirect3DPixelShader9> ps;
        if (FAILED(device->GetVertexShader(vs.GetAddressOf())) || !vs ||
            FAILED(device->GetPixelShader(ps.GetAddressOf())) || !ps)
            return false;
        const auto& tracked = g_trackedState;
        auto& cache = ShaderCache::Instance();
        const auto vsHash = tracked.currentVS == vs.Get() && tracked.vsHash
            ? tracked.vsHash : cache.GetShaderHash(vs.Get());
        const auto psHash = tracked.currentPS == ps.Get() && tracked.psHash
            ? tracked.psHash : cache.GetShaderHash(ps.Get());
        return IsMaterialLayout(desc[0].Width, desc[0].Height,
                                desc[1].Width, desc[1].Height, vsHash, psHash);
    }
}
