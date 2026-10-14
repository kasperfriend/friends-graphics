#pragma once
#include <d3d9.h>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <list>
#include <wrl/client.h>

namespace renderer
{
    struct ShaderInfo
    {
        uint64_t hash = 0;
        uint32_t bytecodeSize = 0;
        bool isKnownTerrain = false;
        bool isKnownWater = false;
        bool isKnownUI = false;
    };

    class ShaderCache
    {
    public:
        static ShaderCache& Instance();
        // Bound retained COM identities, including short-lived diagnostic shaders.
        static constexpr size_t MaxEntries = 1024;

        uint64_t GetShaderHash(IDirect3DVertexShader9* vs);
        uint64_t GetShaderHash(IDirect3DPixelShader9* ps);

        const ShaderInfo* GetShaderInfo(IDirect3DVertexShader9* vs);
        const ShaderInfo* GetShaderInfo(IDirect3DPixelShader9* ps);

        void Clear();

        uint64_t GetHits() const { return m_hits; }
        uint64_t GetMisses() const { return m_misses; }

    private:
        ShaderCache() = default;

        template<typename T>
        const ShaderInfo* FetchOrCreate(T* shader);

        struct Entry
        {
            ShaderInfo info;
            Microsoft::WRL::ComPtr<IUnknown> owner;
            std::list<const void*>::iterator recency;
        };
        std::unordered_map<const void*, Entry> m_cache;
        std::list<const void*> m_recency;
        uint64_t m_hits = 0;
        uint64_t m_misses = 0;
    };
}
