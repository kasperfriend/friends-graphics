#pragma once
#include <d3d9.h>
#include <cstdint>
#include <functional>
#include "MaterialType.h"

namespace renderer
{
    struct DrawCallContext
    {
        IDirect3DDevice9* device = nullptr;
        IDirect3DVertexShader9* vertexShader = nullptr;
        IDirect3DPixelShader9* pixelShader = nullptr;
        IDirect3DVertexDeclaration9* vertexDecl = nullptr;

        uint64_t vsHash = 0;
        uint64_t psHash = 0;

        D3DPRIMITIVETYPE primitiveType = D3DPT_TRIANGLELIST;
        uint32_t primitiveCount = 0;

        bool alphaBlend = false;
        bool alphaTest = false;
        bool zWrite = true;
        bool zEnable = true;

        D3DCULL cullMode = D3DCULL_CCW;

        uint32_t renderTargetWidth = 0;
        uint32_t renderTargetHeight = 0;
    };

    struct PipelineKey
    {
        const void* vs = nullptr;
        const void* ps = nullptr;
        const void* vertexDecl = nullptr;
        uint32_t renderStateBits = 0;
        // Addresses may be reused after eviction from the owning shader cache.
        uint64_t vsHash = 0, psHash = 0;

        bool operator==(const PipelineKey& o) const
        {
            return vs == o.vs && ps == o.ps && vertexDecl == o.vertexDecl && renderStateBits == o.renderStateBits &&
                   vsHash == o.vsHash && psHash == o.psHash;
        }
    };
}

namespace std
{
    template<>
    struct hash<renderer::PipelineKey>
    {
        size_t operator()(const renderer::PipelineKey& k) const noexcept
        {
            size_t h = reinterpret_cast<size_t>(k.vs);
            h ^= reinterpret_cast<size_t>(k.ps) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= reinterpret_cast<size_t>(k.vertexDecl) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(k.renderStateBits) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint64_t>{}(k.vsHash) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint64_t>{}(k.psHash) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
}
