#include "ShaderCache.h"
#include "../Water/WaterPolicy.h"

namespace renderer
{
    ShaderCache& ShaderCache::Instance()
    {
        // COM references must be released by Clear during reset, not by CRT
        // teardown under the loader lock. Process exit owns final lifetime.
        static auto* instance = new ShaderCache;
        return *instance;
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
            m_recency.splice(m_recency.begin(), m_recency, it->second.recency);
            return &it->second.info;
        }

        UINT size = 0;
        if (FAILED(shader->GetFunction(nullptr, &size)) || size == 0 || size > 65536)
        {
            // A transient GetFunction failure must not poison this identity.
            ++m_misses;
            return nullptr;
        }

        std::vector<BYTE> bytes(size);
        if (FAILED(shader->GetFunction(bytes.data(), &size)))
        {
            // A transient GetFunction failure must not poison this identity.
            ++m_misses;
            return nullptr;
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
        // Raw interface addresses can be recycled as soon as the game releases
        // a shader. Hold the identity while cached, or a new shader at the same
        // address can inherit an old water/UI hash without reading its bytecode.
        m_recency.push_front(key);
        Entry entry{info, shader, m_recency.begin()};
        decltype(m_cache)::iterator iter;
        try { iter = m_cache.emplace(key, std::move(entry)).first; }
        catch (...) { m_recency.pop_front(); throw; }
        if (m_cache.size() > MaxEntries)
        {
            m_cache.erase(m_recency.back());
            m_recency.pop_back();
        }
        return &iter->second.info;
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
        m_recency.clear();
    }
}
