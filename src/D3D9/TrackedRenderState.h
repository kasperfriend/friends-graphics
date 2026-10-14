#pragma once
#include <cstdint>

// Only identities are tracked; no COM calls are made by the software mirror.
struct IDirect3DVertexShader9;
struct IDirect3DPixelShader9;
struct IDirect3DVertexDeclaration9;

namespace renderer
{
    struct TrackedRenderState
    {
        IDirect3DVertexShader9* currentVS = nullptr;
        IDirect3DPixelShader9* currentPS = nullptr;
        IDirect3DVertexDeclaration9* currentVDecl = nullptr;
        uint32_t currentFVF = 0;
        uint64_t vsHash = 0;
        uint64_t psHash = 0;
        bool alphaBlend = false;
        bool alphaTest = false;
        bool zWrite = true;
        bool zEnable = true;
    };

    inline TrackedRenderState g_trackedState;

    // StateBlock::Apply restores the device internally, bypassing our Set*
    // hooks. Every state-block-based internal pass must restore this mirror
    // too, AFTER applying the GPU block. Otherwise the next world draw can
    // inherit UI flags/hashes even though the GPU is back in world state.
    class ScopedTrackedRenderState
    {
    public:
        ScopedTrackedRenderState() noexcept : m_saved(g_trackedState) {}
        ~ScopedTrackedRenderState() { Restore(); }
        void Restore() noexcept
        {
            if (m_restored) return;
            g_trackedState = m_saved;
            m_restored = true;
        }
        ScopedTrackedRenderState(const ScopedTrackedRenderState&) = delete;
        ScopedTrackedRenderState& operator=(const ScopedTrackedRenderState&) = delete;
    private:
        TrackedRenderState m_saved;
        bool m_restored = false;
    };
}
