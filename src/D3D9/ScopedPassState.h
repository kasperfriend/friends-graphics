#pragma once
// State-block-based pass scope for the per-frame compositor passes.
//
// Every compositor pass (atmosphere, ambient occlusion, global
// illumination, dynamic lights, the cloud deck, the weather scene copy and
// the post-process composite) used to wrap its work in ScopedRenderState,
// which saves and restores the full device state with ~170 individual
// Get/SetRenderState/Get/SetSamplerState/Get/SetTexture calls per pass -
// about a thousand driver calls per frame across the passes.
//
// A D3D9 state block (D3DSBT_ALL) captures and restores the same state in
// one Capture() and one Apply(). The only state a block cannot hold is the
// framebuffer attachments (RT0, depth, viewport) and the shader constant
// registers, so this class saves exactly those - RT0, the original depth
// surface, the viewport and pixel constants c0..c63, the same span
// ScopedRenderState restored - and leaves everything else to the block.
//
// Safety:
//   * one shared block per device pointer; a new device recreates it, and a
//     block left over from a reset device fails Capture() and is replaced;
//   * if CreateStateBlock is unavailable on a device, the scope falls back
//     to the legacy full ScopedRenderState save, so behavior is identical
//     everywhere and the fast path is pure optimization;
//   * restore order matches ScopedRenderState: state, then framebuffer,
//     viewport last (SetRenderTarget can reset the viewport on some
//     drivers).
#include <d3d9.h>
#include <memory>
#include <wrl/client.h>
#include "DepthCapture.h"
#include "ScopedRenderState.h"

namespace renderer
{
    namespace detail
    {
        // The shared D3DSBT_ALL block. A state block belongs to the device
        // that created it: a different device pointer recreates it, and a
        // capture failure (e.g. a block orphaned by a device reset) is
        // reported so the caller falls back to the legacy save.
        struct PassStateBlock
        {
            // Pins the device that created `block`. A raw pointer cannot
            // tell "the same device" from "a new device allocated at the
            // address of a destroyed one": the allocator reuses freed
            // memory, and a stale block captured through a dangling pointer
            // would then pass Capture() and Apply() to the wrong device.
            // Holding a reference keeps the old device alive until the
            // block is replaced, so a fresh device can never land on the
            // pinned address and pointer identity is trustworthy. The old
            // device is released at most one scope later (every pass opens
            // a scope each frame), which is the standard D3D9 overlay
            // lifetime pattern.
            Microsoft::WRL::ComPtr<IDirect3DDevice9> deviceRef;
            Microsoft::WRL::ComPtr<IDirect3DStateBlock9> block;
        };
        inline PassStateBlock& SharedPassStateBlock()
        {
            static PassStateBlock shared;
            return shared;
        }

        // True when a shared D3DSBT_ALL block captured by `d` is ready.
        // A device other than the pinned one recreates the block; a capture
        // failure (a block orphaned by a device reset) recreates it once so
        // the fast path recovers instead of degrading to the legacy save
        // forever.
        inline bool EnsurePassStateBlock(IDirect3DDevice9* d)
        {
            if (!d) return false;
            auto& shared = SharedPassStateBlock();
            if (shared.deviceRef.Get() != d)
            {
                shared.block.Reset();
                shared.deviceRef.Reset();
                shared.deviceRef = d; // AddRef: the pin owns a reference
                d->CreateStateBlock(D3DSBT_ALL, shared.block.GetAddressOf());
            }
            if (shared.block && SUCCEEDED(shared.block->Capture()))
                return true;
            shared.block.Reset();
            d->CreateStateBlock(D3DSBT_ALL, shared.block.GetAddressOf());
            if (shared.block && SUCCEEDED(shared.block->Capture()))
                return true;
            return false;
        }
    }

    class ScopedPassState
    {
    public:
        explicit ScopedPassState(IDirect3DDevice9* d)
            : m_device(d)
        {
            if (!m_device) return;

            if (detail::EnsurePassStateBlock(m_device))
            {
                m_usedBlock = true;
                m_device->GetRenderTarget(0, m_rt.GetAddressOf());
                DepthCapture::Instance().RawGetDepth(m_device, m_depth.GetAddressOf());
                m_device->GetViewport(&m_vp);
                m_device->GetPixelShaderConstantF(0, m_psConstants[0], 64);
                return;
            }
            // No state block on this device: the legacy full save, byte for
            // byte the pre-existing behavior.
            m_legacy = std::make_unique<ScopedRenderState>(d);
        }

        ~ScopedPassState()
        {
            Restore();
        }

        // S_OK when a state block was captured (or the legacy save ran);
        // E_POINTER when there was no device.
        HRESULT CaptureResult() const noexcept
        {
            return m_device ? S_OK : E_POINTER;
        }

        void Restore() noexcept
        {
            if (m_restored || !m_device) return;
            m_restored = true;
            if (m_usedBlock)
            {
                // Shader constants first, then the block (which restores
                // render states, textures, samplers, shaders, vertex format
                // and streams), then the framebuffer the block cannot hold,
                // viewport last.
                m_device->SetPixelShaderConstantF(0, m_psConstants[0], 64);
                detail::SharedPassStateBlock().block->Apply();
                m_device->SetRenderTarget(0, m_rt.Get());
                DepthCapture::Instance().RawSetDepth(m_device, m_depth.Get());
                m_device->SetViewport(&m_vp);
            }
            else if (m_legacy)
            {
                m_legacy->Restore();
            }
        }

    private:
        IDirect3DDevice9* m_device = nullptr;
        bool m_usedBlock = false;
        bool m_restored = false;
        std::unique_ptr<ScopedRenderState> m_legacy;
        Microsoft::WRL::ComPtr<IDirect3DSurface9> m_rt;
        Microsoft::WRL::ComPtr<IDirect3DSurface9> m_depth;
        D3DVIEWPORT9 m_vp{};
        float m_psConstants[64][4]{};
    };
}
