#pragma once
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include "../Core/FrameContext.h"

namespace renderer
{
    using Microsoft::WRL::ComPtr;

    enum class CloudDebugMode : uint32_t
    {
        None = 0,
        // Raw raymarch output before the sky mask and the upsample, the
        // accumulated cloud coverage (1 - transmittance), the direct
        // (sun/moon) lighting term on its own, and the ground shadow map.
        RawLowResolution,
        Coverage,
        DirectLighting,
        ShadowMap
    };

    struct VolumetricCloudSettings
    {
        bool enabled = true;
        // 0 = LOW, 1 = MEDIUM, 2 = HIGH, 3 = ULTRA. Drives the march step
        // count, the light march length, the base step and the resolution
        // the deck is integrated at.
        uint32_t quality = 2;
        uint32_t sampleCount = 96;
        uint32_t lightSampleCount = 6;
        float resolutionScale = .25f;

        // How much of the sky the deck covers (0 = clear sky), how optically
        // thick the material is, and which way the vertical profile leans:
        // -1 flattens everything to stratus, +1 pushes towering cumulus.
        float coverage = .50f;
        float density = 1.f;
        float cloudType = 0.f;

        // The deck is a spherical shell around a planet of `planetRadius`
        // centred under the camera, so it meets the horizon with a curve and
        // can be flown into and above instead of being a flat slab that has
        // to be pushed out of the camera's way.
        float planetRadius = 42000.f;
        float baseAltitude = 850.f;
        float thickness = 2150.f;

        // World size of one tile of each noise field, and how hard the
        // high-frequency volume erodes the base shape. `shapeAspect` is how
        // many times the base shape repeats vertically per horizontal tile:
        // anything much above 1 squashes the billows into lozenges, which is
        // what makes a cloud deck read as painted-on rather than solid.
        float featureSize = 3000.f;
        float shapeAspect = 1.3f;
        float detailSize = 240.f;
        float weatherSize = 34000.f;
        float detailStrength = .88f;

        // Wind advects the whole field; the shear offsets the tops downwind
        // with height, which is what gives a cumulus its leaning anvil.
        float windSpeed = 26.f;
        float windAngle = 25.f;
        float windShear = .22f;

        // Lighting response. The light march is a Beer integral toward the
        // tracked disc; `multiScatter*` are the three octaves of Wrenninge's
        // approximation that keep thick clouds from going black.
        float lightAbsorption = .38f;
        float sunIntensity = 1.30f;
        float ambientIntensity = 1.05f;
        float moonStrength = .3f;
        float forwardScatter = .78f;
        float backwardScatter = .32f;
        float silverLining = .55f;
        float powder = .70f;
        float multiScatterAttenuation = .60f;
        float multiScatterContribution = .92f;
        float multiScatterEccentricity = .55f;

        // March shape: the step grows with distance because a 1400-unit-thick
        // shell seen edge-on is 25000 units long, and the width of the fade
        // band that hides the shell's edge at the horizon.
        float baseStep = 70.f;
        float stepGrowth = .010f;
        float coarseScale = 3.f;
        float jitter = .25f;
        float horizonFade = .018f;
        float maxDistance = 26000.f;

        // Aerial perspective: how much of the sky's own haze colour a cloud
        // picks up over the draw distance. Distance has to wash a cloud out,
        // or near and far clouds read as the same contrast and the deck
        // looks like a backdrop painting.
        float aerialPerspective = .60f;

        // Cloud shadows and rays share a small transmittance map on a fixed
        // world plane. The camera only selects its texel-snapped coverage;
        // each receiver is projected along sunlight using its own height.
        bool groundShadows = true;
        float groundShadowStrength = .55f;
        float groundShadowExtent = 14000.f;
        bool sunRays = true;
        float sunRayStrength = .8f;

        // Pushed in per frame by the host when the client is actually
        // rendering rain or snow: a storm sky is a covered sky.
        float weatherCoverageBoost = 0.f;

        CloudDebugMode debugMode = CloudDebugMode::None;
    };

    // Raymarched volumetric cloud deck. It runs before the atmosphere
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
        IDirect3DTexture9* GetShadowTexture() const { return m_shadowValid ? m_shadowTexture.Get() : nullptr; }
        Vec4 ShadowArea() const { return m_shadowArea; }
        Vec4 LightProjection() const { return m_lightProjection; }
        // True once the noise fields exist on the GPU. Generation happens on
        // the first frame that renders clouds (or loads from the cache file).
        bool FieldsReady() const { return m_fieldsReady; }
        void ApplyQualityPreset(uint32_t quality);

    private:
        VolumetricClouds() = default;
        bool EnsureShaders(IDirect3DDevice9* device);
        bool EnsureResources(IDirect3DDevice9* device, uint32_t fullWidth, uint32_t fullHeight);
        bool EnsureFields(IDirect3DDevice9* device);
        void DrawScreenQuad(IDirect3DDevice9* device, uint32_t width, uint32_t height);

        VolumetricCloudSettings m_settings;
        IDirect3DDevice9* m_owner = nullptr;
        uint32_t m_fullWidth = 0, m_fullHeight = 0, m_lowWidth = 0, m_lowHeight = 0;
        ComPtr<IDirect3DTexture9> m_cloudTexture;
        ComPtr<IDirect3DSurface9> m_cloudSurface;
        ComPtr<IDirect3DTexture9> m_shadowTexture;
        ComPtr<IDirect3DSurface9> m_shadowSurface;
        ComPtr<IDirect3DVolumeTexture9> m_shapeTexture;
        ComPtr<IDirect3DVolumeTexture9> m_detailTexture;
        ComPtr<IDirect3DTexture9> m_weatherTexture;
        ComPtr<IDirect3DPixelShader9> m_marchShader;
        ComPtr<IDirect3DPixelShader9> m_compositeShader;
        ComPtr<IDirect3DPixelShader9> m_shadowShader;
        std::wstring m_logPath;
        std::wstring m_cachePath;
        bool m_shadersFailed = false;
        bool m_fieldsReady = false;
        bool m_fieldsFailed = false;
        bool m_shadowValid = false;
        Vec4 m_shadowArea;
        Vec4 m_lightProjection;
        uint32_t m_shadowSize = 0;
        float m_shadowOrigin[2] = { 0.f, 0.f };
        uint64_t m_renderFrames = 0;
        ULONGLONG m_lastTick = 0;
        float m_elapsed = 0.f;
    };
}
