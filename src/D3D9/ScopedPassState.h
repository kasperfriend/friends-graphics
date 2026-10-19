#pragma once
// State-block-based pass scope for the per-frame compositor passes.
//
// Every compositor pass (atmosphere, ambient occlusion, dynamic lights, the
// cloud deck, the weather scene copy and the post-process composite) used to
// wrap its work in ScopedRenderState,
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
#include "TrackedRenderState.h"

namespace renderer
{
    namespace detail
    {
        using ScopedTrackedRenderState = renderer::ScopedTrackedRenderState;

        // The shared D3DSBT_ALL blocks, one per nesting level. A state
        // block belongs to the device that created it: a different device
        // pointer recreates the set, and a capture failure (e.g. a block
        // orphaned by a device reset) is replaced so the caller falls back
        // to the legacy save for that one scope.
        //
        // Scopes are synchronous C++ RAII on the render thread, so they
        // nest strictly LIFO. A single shared slot would let a nested scope
        // overwrite the outer scope's snapshot (the outer Apply would then
        // restore the inner scope's "original" state - mid-pass bindings
        // would leak into the game's next draw), so each level owns its own
        // block object. The levels are created lazily and reused for the
        // lifetime of the device, so steady state costs one Capture per
        // scope entry and one Apply per exit, at every nesting depth.
        struct PassStateBlock
        {
            static constexpr int kMaxDepth = 4;

            // The hook-maintained state mirror at each level. Apply()
            // restores the GPU state without running the Set* hooks, so
            // without this the next world draw would classify against the
            // pass's shaders and blend/depth flags instead of the world's
            // (the same invariant ScopedStateBlock documents for UI
            // drawing). Saved on Capture, restored on Apply.
            TrackedRenderState tracked[kMaxDepth];

            // Pins the device that created the blocks. A raw pointer cannot
            // tell "the same device" from "a new device allocated at the
            // address of a destroyed one": the allocator reuses freed
            // memory, and a stale block captured through a dangling pointer
            // would then pass Capture() and Apply() to the wrong device.
            // Holding a reference keeps the old device alive until the
            // blocks are replaced, so a fresh device can never land on the
            // pinned address and pointer identity is trustworthy. The old
            // device is released at most one scope later (every pass opens
            // a scope each frame), which is the standard D3D9 overlay
            // lifetime pattern.
            Microsoft::WRL::ComPtr<IDirect3DDevice9> deviceRef;
            Microsoft::WRL::ComPtr<IDirect3DStateBlock9> block[kMaxDepth];
            int depth = 0;
        };
        inline PassStateBlock& SharedPassStateBlock()
        {
            static PassStateBlock shared;
            return shared;
        }

        // State blocks capture textures and other bound resources as well as
        // scalar state. A cached block can therefore keep D3DPOOL_DEFAULT
        // resources alive across IDirect3DDevice9::Reset, which makes a game
        // display-mode switch fail repeatedly. Explicitly drop every cached
        // block and the device pin before the original Reset is called.
        inline void ResetPassStateBlocks() noexcept
        {
            auto& shared = SharedPassStateBlock();
            for (auto& block : shared.block) block.Reset();
            shared.deviceRef.Reset();
            shared.depth = 0;
        }

        // Acquires the block at the current nesting level, captures the
        // current state, and returns true when the caller now holds it -
        // the matching release is EndPassState() (Apply + level release).
        // A device other than the pinned one recreates the set; a capture
        // failure (a block orphaned by a device reset) recreates the level
        // once so the fast path recovers instead of degrading to the
        // legacy save forever. Beyond kMaxDepth nesting the caller gets
        // false and runs its legacy save, which is self-contained.
        inline bool BeginPassState(IDirect3DDevice9* d)
        {
            if (!d) return false;
            auto& shared = SharedPassStateBlock();
            if (shared.depth >= PassStateBlock::kMaxDepth) return false;
            if (shared.deviceRef.Get() != d)
            {
                for (auto& b : shared.block) b.Reset();
                shared.deviceRef.Reset();
                shared.deviceRef = d; // AddRef: the pin owns a reference
                shared.depth = 0;
            }
            auto& b = shared.block[shared.depth];
            if (!b) (void)d->CreateStateBlock(D3DSBT_ALL, b.GetAddressOf());
            if (b && SUCCEEDED(b->Capture()))
            {
                shared.tracked[shared.depth] = g_trackedState;
                ++shared.depth;
                return true;
            }
            b.Reset();
            (void)d->CreateStateBlock(D3DSBT_ALL, b.GetAddressOf());
            if (b && SUCCEEDED(b->Capture()))
            {
                shared.tracked[shared.depth] = g_trackedState;
                ++shared.depth;
                return true;
            }
            return false;
        }

        // Applies the block held by the most recent unmatched BeginPassState
        // and releases its level. Every scope exits in the reverse order it
        // entered (RAII on one thread), so the top of the stack is always
        // the caller's own block. The mirror is restored AFTER the Apply,
        // the same transaction ScopedStateBlock uses for UI drawing.
        inline void EndPassState()
        {
            auto& shared = SharedPassStateBlock();
            if (shared.depth <= 0) return;
            if (auto& b = shared.block[shared.depth - 1]; b) (void)b->Apply();
            g_trackedState = shared.tracked[shared.depth - 1];
            --shared.depth;
        }
    }

    class ScopedPassState
    {
    public:
        explicit ScopedPassState(IDirect3DDevice9* d)
            : m_device(d)
        {
            if (!m_device) return;

            m_tracked.Save();
            if (detail::BeginPassState(m_device))
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
                detail::EndPassState();
                m_tracked.Restore();
                m_device->SetRenderTarget(0, m_rt.Get());
                DepthCapture::Instance().RawSetDepth(m_device, m_depth.Get());
                m_device->SetViewport(&m_vp);
            }
            else if (m_legacy)
            {
                // The legacy save restores through the Set* hooks, which
                // re-sync the mirror - except the conditional vdecl/FVF
                // writes it skips when the original pass used neither, so
                // the snapshot is restored explicitly to be exact.
                m_legacy->Restore();
                m_tracked.Restore();
            }
        }

    private:
        IDirect3DDevice9* m_device = nullptr;
        bool m_usedBlock = false;
        bool m_restored = false;
        detail::ScopedTrackedRenderState m_tracked;
        std::unique_ptr<ScopedRenderState> m_legacy;
        Microsoft::WRL::ComPtr<IDirect3DSurface9> m_rt;
        Microsoft::WRL::ComPtr<IDirect3DSurface9> m_depth;
        D3DVIEWPORT9 m_vp{};
        float m_psConstants[64][4]{};
    };
}
