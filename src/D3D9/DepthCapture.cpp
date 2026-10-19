#include "DepthCapture.h"
#include "DepthCapturePolicy.h"
#include "../Diagnostics/RendererDiagnostics.h"
#include <sstream>

namespace renderer
{
    DepthCapture& DepthCapture::Instance()
    {
        static DepthCapture instance;
        return instance;
    }

    void DepthCapture::SetFunctions(SetDepthFn setDepth, GetDepthFn getDepth)
    {
        m_origSetDepth = setDepth;
        m_origGetDepth = getDepth;
    }

    HRESULT DepthCapture::HookSetDepth(IDirect3DDevice9* device, IDirect3DSurface9* surface)
    {
        if (!m_origSetDepth)
            return D3DERR_INVALIDCALL;

        IDirect3DSurface9* target = surface;
        if (m_owner == device && surface && surface == m_originalDepth.Get())
        {
            target = m_depthSurface.Get();
        }

        return m_origSetDepth(device, target);
    }

    HRESULT DepthCapture::HookGetDepth(IDirect3DDevice9* device, IDirect3DSurface9** outSurface)
    {
        if (!m_origGetDepth)
            return D3DERR_INVALIDCALL;

        HRESULT hr = m_origGetDepth(device, outSurface);
        if (SUCCEEDED(hr) && outSurface && m_owner == device && *outSurface == m_depthSurface.Get() && m_originalDepth)
        {
            (*outSurface)->Release();
            *outSurface = m_originalDepth.Get();
            (*outSurface)->AddRef();
        }
        return hr;
    }

    HRESULT DepthCapture::RawSetDepth(IDirect3DDevice9* device, IDirect3DSurface9* surface)
    {
        if (!m_origSetDepth)
            return D3DERR_INVALIDCALL;
        return m_origSetDepth(device, surface);
    }

    HRESULT DepthCapture::RawGetDepth(IDirect3DDevice9* device, IDirect3DSurface9** outSurface)
    {
        if (!m_origGetDepth)
            return D3DERR_INVALIDCALL;
        return m_origGetDepth(device, outSurface);
    }

    bool DepthCapture::BeforeClear(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, float z)
    {
        if (!device || !m_origGetDepth || !m_origSetDepth ||
            !(flags & D3DCLEAR_ZBUFFER) || z != 1.0f || m_owner != nullptr)
            return false;

        ComPtr<IDirect3DSurface9> rt, back, ds;
        D3DSURFACE_DESC desc{}, rd{};
        D3DVIEWPORT9 vp{};

        if (FAILED(device->GetRenderTarget(0, rt.GetAddressOf())) ||
            FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, back.GetAddressOf())) ||
            rt.Get() != back.Get())
            return false;

        if (FAILED(m_origGetDepth(device, ds.GetAddressOf())) || !ds ||
            FAILED(ds->GetDesc(&desc)) || FAILED(rt->GetDesc(&rd)) ||
            FAILED(device->GetViewport(&vp)))
        {
            RendererDiagnostics::Instance().LogOnce("DepthCapture", "depth-query",
                "Could not query the backbuffer depth surface or viewport.");
            return false;
        }

        if (desc.Width != rd.Width || desc.Height != rd.Height ||
            vp.X != 0 || vp.Y != 0 || vp.Width != rd.Width || vp.Height != rd.Height)
            return false;

        if (!ClearsFullViewport(count, rects, vp))
            return false;

        if (desc.MultiSampleType != D3DMULTISAMPLE_NONE || rd.MultiSampleType != D3DMULTISAMPLE_NONE)
        {
            RendererDiagnostics::Instance().LogOnce("DepthCapture", "multisampling",
                "MSAA prevents INTZ depth capture. Set in-game Multisampling to 1x and restart the client.");
            return false;
        }

        if (!CanReplaceDepthFormat(desc.Format))
        {
            RendererDiagnostics::Instance().LogOnce("DepthCapture", "depth-format",
                "Unsupported backbuffer depth format: " + std::to_string(desc.Format));
            return false;
        }

        ComPtr<IDirect3DTexture9> tex;
        ComPtr<IDirect3DSurface9> replacement;
        HRESULT result = device->CreateTexture(desc.Width, desc.Height, 1,
                D3DUSAGE_DEPTHSTENCIL,
                static_cast<D3DFORMAT>(MAKEFOURCC('I','N','T','Z')),
                D3DPOOL_DEFAULT, tex.GetAddressOf(), nullptr);
        if (SUCCEEDED(result)) result = tex->GetSurfaceLevel(0, replacement.GetAddressOf());
        if (SUCCEEDED(result)) result = m_origSetDepth(device, replacement.Get());
        if (FAILED(result))
        {
            std::ostringstream message;
            message << "INTZ depth setup failed: hr=0x" << std::hex << static_cast<unsigned long>(result)
                    << std::dec << " size=" << desc.Width << 'x' << desc.Height
                    << " originalFormat=" << desc.Format;
            RendererDiagnostics::Instance().LogOnce("DepthCapture", "intz-setup", message.str());
            return false;
        }

        m_depthTexture = tex;
        m_depthSurface = replacement;
        m_originalDepth = ds;
        m_target = rt;
        m_owner = device;
        ++m_depthFrames;
        RendererDiagnostics::Instance().RecordDepthCapture();
        RendererDiagnostics::Instance().LogOnce("DepthCapture", "active", "INTZ depth capture active.");
        return true;
    }

    void DepthCapture::OnFrameEnd(IDirect3DDevice9* device)
    {
        if (m_owner && m_owner != device)
            return;

        if (m_owner && m_origGetDepth && m_origSetDepth)
        {
            ComPtr<IDirect3DSurface9> current;
            if (SUCCEEDED(m_origGetDepth(device, current.GetAddressOf())) && current.Get() == m_depthSurface.Get())
            {
                m_origSetDepth(device, m_originalDepth.Get());
            }
        }

        m_depthSurface.Reset();
        m_depthTexture.Reset();
        m_originalDepth.Reset();
        m_target.Reset();
        m_owner = nullptr;
    }

    void DepthCapture::Reset(IDirect3DDevice9* device)
    {
        // OnFrameEnd restores the captured depth surface through the device's
        // original vtable methods. That is safe only for the device that owns
        // the capture; on device replacement release our COM references without
        // making calls through a different device.
        if (m_owner && m_owner != device)
        {
            m_depthSurface.Reset();
            m_depthTexture.Reset();
            m_originalDepth.Reset();
            m_target.Reset();
            m_owner = nullptr;
            return;
        }
        OnFrameEnd(device);
    }
}
