#pragma once
#include <d3d9.h>
#include <wrl/client.h>
#include "WaterPolicy.h"

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
        return IsMaterialLayout(desc[0].Width, desc[0].Height, desc[1].Width, desc[1].Height);
    }
}
