#pragma once
#include <d3d9.h>
#include <cstdint>
#include <unordered_map>
#include <algorithm>

namespace renderer
{
    constexpr uint64_t FNV1A_64_OFFSET = 0xcbf29ce484222325ull;
    constexpr uint64_t FNV1A_64_PRIME  = 0x100000001b3ull;

    inline uint64_t ComputeFNV1a64(const void* data, size_t size)
    {
        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        uint64_t h = FNV1A_64_OFFSET;
        for (size_t i = 0; i < size; ++i)
        {
            h ^= ptr[i];
            h *= FNV1A_64_PRIME;
        }
        return h;
    }

    class TextureHashLookup
    {
    public:
        static TextureHashLookup& Instance()
        {
            static TextureHashLookup instance;
            return instance;
        }

        void Clear()
        {
            m_ptrToHash.clear();
        }

        uint64_t GetTextureHash(IDirect3DBaseTexture9* baseTex)
        {
            if (!baseTex)
                return 0;
            if (baseTex->GetType() != D3DRTYPE_TEXTURE)
                return 0;

            auto it = m_ptrToHash.find(baseTex);
            if (it != m_ptrToHash.end())
            {
                return it->second;
            }

            uint64_t hash = HashTextureContent(static_cast<IDirect3DTexture9*>(baseTex));
            m_ptrToHash[baseTex] = hash;
            return hash;
        }

    private:
        TextureHashLookup() = default;

        uint64_t HashTextureContent(IDirect3DTexture9* tex)
        {
            if (!tex)
                return 0;

            D3DSURFACE_DESC desc{};
            if (FAILED(tex->GetLevelDesc(0, &desc)))
                return 0;

            // Only compute for terrain-sized textures
            if (desc.Width < 16 || desc.Height < 16)
                return 0;

            D3DLOCKED_RECT lr{};
            if (FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
            {
                return 0;
            }

            size_t dataSize = 0;
            if (desc.Format == D3DFMT_DXT1)
            {
                dataSize = ((desc.Width + 3) / 4) * ((desc.Height + 3) / 4) * 8;
            }
            else if (desc.Format == D3DFMT_DXT3 || desc.Format == D3DFMT_DXT5)
            {
                dataSize = ((desc.Width + 3) / 4) * ((desc.Height + 3) / 4) * 16;
            }
            else if (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8)
            {
                dataSize = static_cast<size_t>(desc.Width) * desc.Height * 4;
            }
            else
            {
                tex->UnlockRect(0);
                return 0;
            }

            uint64_t hash = 0;
            if (desc.Format == D3DFMT_DXT1 || desc.Format == D3DFMT_DXT3 || desc.Format == D3DFMT_DXT5)
            {
                // Contiguous compressed blocks
                hash = ComputeFNV1a64(lr.pBits, dataSize);
            }
            else
            {
                // Row by row for uncompressed
                uint64_t h = FNV1A_64_OFFSET;
                const size_t rowBytes = desc.Width * 4;
                const uint8_t* rowPtr = static_cast<const uint8_t*>(lr.pBits);
                for (UINT y = 0; y < desc.Height; ++y)
                {
                    for (size_t x = 0; x < rowBytes; ++x)
                    {
                        h ^= rowPtr[x];
                        h *= FNV1A_64_PRIME;
                    }
                    rowPtr += lr.Pitch;
                }
                hash = h;
            }

            tex->UnlockRect(0);
            return hash;
        }

        std::unordered_map<const void*, uint64_t> m_ptrToHash;
    };
}
