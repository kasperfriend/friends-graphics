#pragma once

#include <d3d9.h>
#include <wrl/client.h>
#include <string>
#include "../Core/DeviceHealth.h"
#include "../Core/FrameContext.h"

namespace renderer
{
    using Microsoft::WRL::ComPtr;

    // Cheap screen-space bounce light. Three low-frequency terms, evaluated
    // together at quarter resolution and added to the scene before the
    // atmosphere pass:
    //
    //   * hemispheric sky/ground ambient in the zone's own fog palette,
    //   * one-bounce colour sampled from a blurred copy of the scene itself,
    //   * soft bleed from the same dynamic point/spot selection the direct
    //     lamp pass uses, plus a depth-cavity term that keeps all three out
    //     of creases.
    //
    // The bounce source is the scene AFTER clouds, native shadows and direct
    // lamp pools have been drawn into it, so every one of those is respected
    // implicitly: a cloud shadow or a native cascade darkens the bounced
    // light exactly where it darkened the direct light, with no second
    // shadow lookup and no double-counting. Cloud dimming of the sky term is
    // the only explicit shadow sample, and it reuses the cloud shadow map
    // the atmosphere already reads.
    struct GlobalIlluminationSettings
    {
        bool enabled = true;
        float intensity = 1.0f;
        float skyAmbient = 0.8f;
        float bounce = 0.7f;
        float occlusion = 0.7f;
        float lampBounce = 0.6f;
        float radius = 1.0f;
        bool followCloudShadows = true;
        bool followWeather = true;
        int quality = 1;
        int debugMode = 0;
    };

    class GlobalIllumination
    {
    public:
        static GlobalIllumination& Instance();

        void Configure(const std::wstring& basePath);
        void ReloadTuning();
        void Reset(IDirect3DDevice9* device);
        // Consumes FrameContext::Selected dynamic lights (populated by the
        // direct lamp pass earlier in the same composite) and publishes the
        // lit scene back into FrameContext::sceneColor/sceneSurface, exactly
        // like LocalLightingRenderer, so the atmosphere pass scatters the
        // bounced light without any further plumbing.
        bool Render(IDirect3DDevice9* device, FrameContext& frameContext);
        GlobalIlluminationSettings& Settings() { return m_settings; }
        const GlobalIlluminationSettings& Settings() const { return m_settings; }
        IDirect3DTexture9* GetLitTexture() const { return m_litTexture.Get(); }
        IDirect3DSurface9* GetLitSurface() const { return m_litSurface.Get(); }

    private:
        GlobalIllumination() = default;
        bool EnsureShaders(IDirect3DDevice9* device);
        void NoteFailure(const std::string& reason);
        bool EnsureResources(IDirect3DDevice9* device, uint32_t width, uint32_t height, D3DFORMAT format, int quality);
        void DrawQuad(IDirect3DDevice9* device, uint32_t width, uint32_t height);
        void Log(const std::string& message) const;

        GlobalIlluminationSettings m_settings;
        std::wstring m_iniPath;
        std::wstring m_logPath;
        IDirect3DDevice9* m_owner = nullptr;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        uint32_t m_lowWidth = 0;
        uint32_t m_lowHeight = 0;
        D3DFORMAT m_format = D3DFMT_UNKNOWN;
        int m_quality = -1;
        // Failed passes are retried after a cooldown; a transient failure
        // (device loss, Alt+Tab) must not switch the effect off until reset.
        RetryGate m_retry;
        bool m_shaderErrorLogged = false;
        bool m_resourceErrorLogged = false;

        ComPtr<IDirect3DTexture9> m_radianceTexture;
        ComPtr<IDirect3DSurface9> m_radianceSurface;
        ComPtr<IDirect3DTexture9> m_giTexture;
        ComPtr<IDirect3DSurface9> m_giSurface;
        ComPtr<IDirect3DTexture9> m_litTexture;
        ComPtr<IDirect3DSurface9> m_litSurface;
        ComPtr<IDirect3DPixelShader9> m_integrateShader;
        ComPtr<IDirect3DPixelShader9> m_compositeShader;
    };
}
