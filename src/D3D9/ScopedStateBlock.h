#pragma once
#include <d3d9.h>
#include <wrl/client.h>
#include "TrackedRenderState.h"

namespace renderer
{
    // Full D3D9 state and the hook-maintained mirror are one transaction.
    // Used by UI drawing, whose fixed-function setters must never leak into
    // world classification, camera capture, weather or attached-light tracking.
    class ScopedStateBlock
    {
    public:
        explicit ScopedStateBlock(IDirect3DDevice9* device)
        {
            if (!device) return;
            m_tracked.Save();
            m_captureResult = device->CreateStateBlock(D3DSBT_ALL, m_block.GetAddressOf());
            if (SUCCEEDED(m_captureResult)) m_captureResult = m_block->Capture();
        }
        ~ScopedStateBlock() { Restore(); }
        HRESULT CaptureResult() const noexcept { return m_captureResult; }
        HRESULT Restore() noexcept
        {
            if (!m_restored)
            {
                m_restored = true;
                if (SUCCEEDED(m_captureResult)) m_restoreResult = m_block->Apply();
                // Apply may itself use setters on some runtimes. Restore the
                // mirror last, and do not let an early return skip restoration.
                m_tracked.Restore();
            }
            return m_restoreResult;
        }
        ScopedStateBlock(const ScopedStateBlock&) = delete;
        ScopedStateBlock& operator=(const ScopedStateBlock&) = delete;
    private:
        ScopedTrackedRenderState m_tracked;
        Microsoft::WRL::ComPtr<IDirect3DStateBlock9> m_block;
        HRESULT m_captureResult = E_POINTER, m_restoreResult = S_OK;
        bool m_restored = false;
    };
}
