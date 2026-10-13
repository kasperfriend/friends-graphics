#pragma once
#include <d3d9.h>
#include <wrl/client.h>
#include "DepthCapture.h"

namespace renderer
{
    using Microsoft::WRL::ComPtr;

    class ScopedRenderState
    {
    public:
        explicit ScopedRenderState(IDirect3DDevice9* device)
            : m_device(device)
        {
            if (!m_device) return;

            // Target & Depth
            m_device->GetRenderTarget(0, m_rt.GetAddressOf());
            DepthCapture::Instance().RawGetDepth(m_device, m_depth.GetAddressOf());
            m_device->GetViewport(&m_vp);

            // Shaders & Vertex format
            m_device->GetVertexShader(m_vs.GetAddressOf());
            m_device->GetPixelShader(m_ps.GetAddressOf());
            m_device->GetPixelShaderConstantF(0, m_psConstants[0], 64);
            m_device->GetFVF(&m_fvf);
            m_device->GetVertexDeclaration(m_vdecl.GetAddressOf());
            m_device->GetStreamSource(0, m_stream0.GetAddressOf(), &m_streamOffset, &m_streamStride);
            m_device->GetIndices(m_indices.GetAddressOf());

            // Atmosphere uses stage 5 for local fog and stage 6 for cloud light.
            for (DWORD s = 0; s < 7; ++s)
            {
                m_device->GetTexture(s, m_tex[s].GetAddressOf());
                m_device->GetSamplerState(s, D3DSAMP_MINFILTER, &m_samp[s].minFilter);
                m_device->GetSamplerState(s, D3DSAMP_MAGFILTER, &m_samp[s].magFilter);
                m_device->GetSamplerState(s, D3DSAMP_MIPFILTER, &m_samp[s].mipFilter);
                m_device->GetSamplerState(s, D3DSAMP_ADDRESSU, &m_samp[s].addressU);
                m_device->GetSamplerState(s, D3DSAMP_ADDRESSV, &m_samp[s].addressV);
                m_device->GetSamplerState(s, D3DSAMP_SRGBTEXTURE, &m_samp[s].srgb);
            }

            // Render states
            m_device->GetRenderState(D3DRS_ZENABLE, &m_zEnable);
            m_device->GetRenderState(D3DRS_ZWRITEENABLE, &m_zWrite);
            m_device->GetRenderState(D3DRS_ZFUNC, &m_zFunc);
            m_device->GetRenderState(D3DRS_ALPHABLENDENABLE, &m_alphaBlend);
            m_device->GetRenderState(D3DRS_SRCBLEND, &m_srcBlend);
            m_device->GetRenderState(D3DRS_DESTBLEND, &m_destBlend);
            m_device->GetRenderState(D3DRS_BLENDOP, &m_blendOp);
            m_device->GetRenderState(D3DRS_ALPHATESTENABLE, &m_alphaTest);
            m_device->GetRenderState(D3DRS_CULLMODE, &m_cullMode);
            m_device->GetRenderState(D3DRS_COLORWRITEENABLE, &m_colorWrite);
            m_device->GetRenderState(D3DRS_FOGENABLE, &m_fogEnable);
            m_device->GetRenderState(D3DRS_LIGHTING, &m_lighting);
            m_device->GetRenderState(D3DRS_STENCILENABLE, &m_stencilEnable);
            m_device->GetRenderState(D3DRS_SCISSORTESTENABLE, &m_scissorEnable);
            m_device->GetRenderState(D3DRS_SRGBWRITEENABLE, &m_srgbWrite);
            m_device->GetRenderState(D3DRS_FILLMODE, &m_fillMode);
            m_device->GetRenderState(D3DRS_CLIPPLANEENABLE, &m_clipPlane);
            m_device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &m_sepAlphaBlend);
            m_device->GetRenderState(D3DRS_DEPTHBIAS, &m_depthBias);
            m_device->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &m_slopeBias);
        }

        ~ScopedRenderState()
        {
            Restore();
        }

        void Restore()
        {
            if (m_restored || !m_device) return;
            m_restored = true;

            // Restore Render states
            m_device->SetRenderState(D3DRS_ZENABLE, m_zEnable);
            m_device->SetRenderState(D3DRS_ZWRITEENABLE, m_zWrite);
            m_device->SetRenderState(D3DRS_ZFUNC, m_zFunc);
            m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, m_alphaBlend);
            m_device->SetRenderState(D3DRS_SRCBLEND, m_srcBlend);
            m_device->SetRenderState(D3DRS_DESTBLEND, m_destBlend);
            m_device->SetRenderState(D3DRS_BLENDOP, m_blendOp);
            m_device->SetRenderState(D3DRS_ALPHATESTENABLE, m_alphaTest);
            m_device->SetRenderState(D3DRS_CULLMODE, m_cullMode);
            m_device->SetRenderState(D3DRS_COLORWRITEENABLE, m_colorWrite);
            m_device->SetRenderState(D3DRS_FOGENABLE, m_fogEnable);
            m_device->SetRenderState(D3DRS_LIGHTING, m_lighting);
            m_device->SetRenderState(D3DRS_STENCILENABLE, m_stencilEnable);
            m_device->SetRenderState(D3DRS_SCISSORTESTENABLE, m_scissorEnable);
            m_device->SetRenderState(D3DRS_SRGBWRITEENABLE, m_srgbWrite);
            m_device->SetRenderState(D3DRS_FILLMODE, m_fillMode);
            m_device->SetRenderState(D3DRS_CLIPPLANEENABLE, m_clipPlane);
            m_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, m_sepAlphaBlend);
            m_device->SetRenderState(D3DRS_DEPTHBIAS, m_depthBias);
            m_device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, m_slopeBias);

            // Restore Textures & Samplers
            for (DWORD s = 0; s < 7; ++s)
            {
                m_device->SetTexture(s, m_tex[s].Get());
                m_device->SetSamplerState(s, D3DSAMP_MINFILTER, m_samp[s].minFilter);
                m_device->SetSamplerState(s, D3DSAMP_MAGFILTER, m_samp[s].magFilter);
                m_device->SetSamplerState(s, D3DSAMP_MIPFILTER, m_samp[s].mipFilter);
                m_device->SetSamplerState(s, D3DSAMP_ADDRESSU, m_samp[s].addressU);
                m_device->SetSamplerState(s, D3DSAMP_ADDRESSV, m_samp[s].addressV);
                m_device->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, m_samp[s].srgb);
            }

            // Restore Shaders & Vertex format
            m_device->SetVertexShader(m_vs.Get());
            m_device->SetPixelShader(m_ps.Get());
            m_device->SetPixelShaderConstantF(0, m_psConstants[0], 64);
            if (m_vdecl)
                m_device->SetVertexDeclaration(m_vdecl.Get());
            if (m_fvf != 0)
                m_device->SetFVF(m_fvf);

            // Restore Stream source & Indices
            m_device->SetStreamSource(0, m_stream0.Get(), m_streamOffset, m_streamStride);
            m_device->SetIndices(m_indices.Get());

            // SetRenderTarget can reset the viewport (notably MinZ/MaxZ), so
            // restore the viewport only after the framebuffer attachments.
            m_device->SetRenderTarget(0, m_rt.Get());
            DepthCapture::Instance().RawSetDepth(m_device, m_depth.Get());
            m_device->SetViewport(&m_vp);
        }

    private:
        IDirect3DDevice9* m_device = nullptr;
        bool m_restored = false;

        ComPtr<IDirect3DSurface9> m_rt;
        ComPtr<IDirect3DSurface9> m_depth;
        D3DVIEWPORT9 m_vp{};

        ComPtr<IDirect3DVertexShader9> m_vs;
        ComPtr<IDirect3DPixelShader9> m_ps;
        // Local lighting uses c0..c31; atmosphere also uses registers above
        // c15. Restore them before the next native world/UI draw.
        float m_psConstants[64][4]{};
        DWORD m_fvf = 0;
        ComPtr<IDirect3DVertexDeclaration9> m_vdecl;
        ComPtr<IDirect3DVertexBuffer9> m_stream0;
        UINT m_streamOffset = 0;
        UINT m_streamStride = 0;
        ComPtr<IDirect3DIndexBuffer9> m_indices;

        ComPtr<IDirect3DBaseTexture9> m_tex[7];
        struct SamplerStateBackup
        {
            DWORD minFilter = D3DTEXF_POINT;
            DWORD magFilter = D3DTEXF_POINT;
            DWORD mipFilter = D3DTEXF_NONE;
            DWORD addressU = D3DTADDRESS_CLAMP;
            DWORD addressV = D3DTADDRESS_CLAMP;
            DWORD srgb = FALSE;
        } m_samp[7];

        DWORD m_zEnable = FALSE, m_zWrite = FALSE, m_zFunc = D3DCMP_LESSEQUAL;
        DWORD m_alphaBlend = FALSE, m_srcBlend = D3DBLEND_ONE, m_destBlend = D3DBLEND_ZERO, m_blendOp = D3DBLENDOP_ADD;
        DWORD m_alphaTest = FALSE;
        DWORD m_cullMode = D3DCULL_CCW;
        DWORD m_colorWrite = 0xF;
        DWORD m_fogEnable = FALSE;
        DWORD m_lighting = FALSE;
        DWORD m_stencilEnable = FALSE;
        DWORD m_scissorEnable = FALSE;
        DWORD m_srgbWrite = FALSE;
        DWORD m_fillMode = D3DFILL_SOLID;
        DWORD m_clipPlane = 0;
        DWORD m_sepAlphaBlend = FALSE;
        DWORD m_depthBias = 0;
        DWORD m_slopeBias = 0;
    };
}
