#pragma once

#include <d3d9.h>
#include <wrl/client.h>
#include <string>
#include "../Core/DeviceHealth.h"
#include "../Core/FrameContext.h"

namespace renderer
{
    using Microsoft::WRL::ComPtr;

    class LocalLightingRenderer
    {
    public:
        static LocalLightingRenderer& Instance();

        void Configure(const std::wstring& basePath);
        void Reset(IDirect3DDevice9* device);
        bool Render(IDirect3DDevice9* device, FrameContext& frameContext);
        IDirect3DTexture9* GetLitTexture() const { return m_litTexture.Get(); }
        IDirect3DSurface9* GetLitSurface() const { return m_litSurface.Get(); }

    private:
        LocalLightingRenderer() = default;
        bool EnsureShaders(IDirect3DDevice9* device);
        void NoteFailure(const std::string& reason);
        bool EnsureResources(IDirect3DDevice9* device, uint32_t width, uint32_t height, D3DFORMAT format, int quality);
        void DrawQuad(IDirect3DDevice9* device, uint32_t width, uint32_t height);
        void Log(const std::string& message) const;

        std::wstring m_logPath;
        std::wstring m_shaderCacheDir;
        IDirect3DDevice9* m_owner = nullptr;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        uint32_t m_lightWidth = 0;
        uint32_t m_lightHeight = 0;
        D3DFORMAT m_format = D3DFMT_UNKNOWN;
        int m_quality = -1;
        // Failed passes are retried after a cooldown; a transient failure (device
        // loss, Alt+Tab) must not switch the effect off until the next reset.
        RetryGate m_retry;
        bool m_shaderErrorLogged = false;
        bool m_resourceErrorLogged = false;
        // One refusal line a second, at most: a stuck gate is a log this size, not
        // a flood of them.
        ULONGLONG m_stateLogTick = 0;

        ComPtr<IDirect3DTexture9> m_lightTexture;
        ComPtr<IDirect3DSurface9> m_lightSurface;
        ComPtr<IDirect3DTexture9> m_litTexture;
        ComPtr<IDirect3DSurface9> m_litSurface;
        ComPtr<IDirect3DPixelShader9> m_surfaceShader;
        ComPtr<IDirect3DPixelShader9> m_compositeShader;
    };
}
