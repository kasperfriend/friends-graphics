#include "ShaderCache.h"
#include "../Water/WaterPolicy.h"

namespace renderer
{
    ShaderCache& ShaderCache::Instance()
    {
        static ShaderCache instance;
        return instance;
    }

    template<typename T>
    const ShaderInfo* ShaderCache::FetchOrCreate(T* shader)
    {
        if (!shader)
            return nullptr;

        const void* key = static_cast<const void*>(shader);

        auto it = m_cache.find(key);
        if (it != m_cache.end())
        {
            ++m_hits;
            return &it->second;
        }

        UINT size = 0;
        if (FAILED(shader->GetFunction(nullptr, &size)) || size == 0 || size > 65536)
        {
            auto [iter, inserted] = m_cache.emplace(key, ShaderInfo{});
            ++m_misses;
            return &iter->second;
        }

        std::vector<BYTE> bytes(size);
        if (FAILED(shader->GetFunction(bytes.data(), &size)))
        {
            auto [iter, inserted] = m_cache.emplace(key, ShaderInfo{});
            ++m_misses;
            return &iter->second;
        }

        uint64_t hash = 14695981039346656037ull;
        for (BYTE b : bytes)
        {
            hash ^= b;
            hash *= 1099511628211ull;
        }

        ShaderInfo info;
        info.hash = hash;
        info.bytecodeSize = size;
        info.isKnownUI = (hash == 0xd9e7756460af6296ull || hash == 0xc29c7060b723c0c6ull);
        // 0x48a82796bd612aeb: a second water vertex shader, paired with the
        // same known 0x7d4f078fa1876a09 pixel shader, that the client swaps
        // to for water tiles very close to the camera (found via WaterDiag
        // capture - it was missing from this list, so those close tiles
        // fell through unclassified and rendered with none of our effects).
        info.isKnownWater = water::IsKnownVertexShader(hash) || water::IsKnownPixelShader(hash);

        ++m_misses;
        auto [iter, inserted] = m_cache.emplace(key, info);
        return &iter->second;
    }

    uint64_t ShaderCache::GetShaderHash(IDirect3DVertexShader9* vs)
    {
        const ShaderInfo* info = FetchOrCreate(vs);
        return info ? info->hash : 0;
    }

    uint64_t ShaderCache::GetShaderHash(IDirect3DPixelShader9* ps)
    {
        const ShaderInfo* info = FetchOrCreate(ps);
        return info ? info->hash : 0;
    }

    const ShaderInfo* ShaderCache::GetShaderInfo(IDirect3DVertexShader9* vs)
    {
        return FetchOrCreate(vs);
    }

    const ShaderInfo* ShaderCache::GetShaderInfo(IDirect3DPixelShader9* ps)
    {
        return FetchOrCreate(ps);
    }

    void ShaderCache::Clear()
    {
        m_cache.clear();
    }
}
