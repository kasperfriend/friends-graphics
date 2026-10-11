#pragma once
#include <d3d9.h>
#include <wrl/client.h>
#include <string>
#include "../Core/FrameContext.h"

namespace renderer
{
    using Microsoft::WRL::ComPtr;

    enum class CloudDebugMode : uint32_t
    {
        None = 0,
        // Raw raymarch output before the sky mask and the upsample, the
        // accumulated cloud coverage (1 - transmittance), and the direct
        // (sun/moon) lighting term on its own.
        RawLowResolution,
        Coverage,
        DirectLighting
    };

    struct VolumetricCloudSettings
    {
        bool enabled = true;
        // 0 = LOW (quarter resolution, 14 steps), 1 = HIGH (quarter, 20),
        // 2 = ULTRA (half, 28). The light march is always 4 fixed steps.
        uint32_t quality = 1;
        uint32_t sampleCount = 20;
        float resolutionScale = .25f;

        // How much of the sky the deck covers (0 = clear sky) and how
        // optically thick the cloud material itself is.
        float coverage = .42f;
        float density = 1.f;

        // The deck is an absolute world-space slab (WoW world Z is up), so
        // flying up to it stays consistent instead of the layer running
        // away from the camera. It is still clamped to at least
        // `minClearance` above the camera so a high mountain or a flying
        // mount can never end up inside or above the band, where a
        // sky-only pass would look broken.
        float altitude = 480.f;
        float thickness = 190.f;
        float minClearance = 140.f;

        // Horizontal size of one noise cell in world units, how strongly
        // the high-frequency octave erodes the base shape, and the wind
        // that animates the whole field.
        float featureSize = 2600.f;
        float detail = .34f;
        float windSpeed = 11.f;
        float windAngle = 25.f;

        // Lighting response: self-shadowing strength along the light
        // march, direct and ambient gain, the moon's share of the direct
        // term and the forward-scattering (Henyey-Greenstein) lobe.
        float absorption = 1.25f;
        float sunIntensity = 1.3f;
        float ambientIntensity = .62f;
        float moonStrength = .3f;
        float phase = .55f;

        // Clouds are only integrated above the horizon; this is the width
        // of the fade band (in |ray.z|) that hides the slab's edge, plus
        // the distance at which the march stops following the slab out to
        // infinity near the horizon.
        float horizonFade = .085f;
        float maxDistance = 9000.f;

        // Pushed in per frame by the host when the client is actually
        // rendering rain or snow: a storm sky is a covered sky.
        float weatherCoverageBoost = 0.f;

        CloudDebugMode debugMode = CloudDebugMode::None;
    };

    // Raymarched cloud deck for the sky. It runs before the atmosphere
    // compositor and writes into the same scene colour the atmosphere then
    // reads, so aerial perspective, the sun halo and the god-ray mask all
    // see the clouds instead of fighting a separate overlay drawn on top.
    class VolumetricClouds
    {
    public:
        static VolumetricClouds& Instance();
        void Configure(const std::wstring& basePath);
        void Reset(IDirect3DDevice9* device);
        bool Render(IDirect3DDevice9* device, const FrameContext& frameContext, IDirect3DSurface9* targetSurface);
        VolumetricCloudSettings& Settings() { return m_settings; }
        const VolumetricCloudSettings& Settings() const { return m_settings; }
        IDirect3DTexture9* GetCloudTexture() const { return m_cloudTexture.Get(); }

    private:
        VolumetricClouds() = default;
        bool EnsureShaders(IDirect3DDevice9* device);
        bool EnsureResources(IDirect3DDevice9* device, uint32_t fullWidth, uint32_t fullHeight);
        void DrawScreenQuad(IDirect3DDevice9* device, uint32_t width, uint32_t height);

        VolumetricCloudSettings m_settings;
        IDirect3DDevice9* m_owner = nullptr;
        uint32_t m_fullWidth = 0, m_fullHeight = 0, m_lowWidth = 0, m_lowHeight = 0;
        ComPtr<IDirect3DTexture9> m_cloudTexture;
        ComPtr<IDirect3DSurface9> m_cloudSurface;
        ComPtr<IDirect3DPixelShader9> m_marchShader;
        ComPtr<IDirect3DPixelShader9> m_compositeShader;
        std::wstring m_logPath;
        uint64_t m_renderFrames = 0;
        ULONGLONG m_lastTick = 0;
        float m_elapsed = 0.f;
    };
}
