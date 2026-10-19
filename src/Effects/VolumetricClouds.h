#pragma once
#include <d3d9.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include "../Core/FrameClock.h"
#include "../Core/DeviceHealth.h"
#include "CloudDynamics.h"
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

    struct VolumetricCloudSettings : CloudShape
    {
        bool enabled = true;
        // 0 = LOW, 1 = MEDIUM, 2 = HIGH, 3 = ULTRA. Drives the march step
        // count, light march length and base step. Buffer resolution is
        // independently owned by ResolutionPercent.
        uint32_t quality = 2;
        uint32_t sampleCount = 96;
        uint32_t lightSampleCount = 6;
        float resolutionScale = .25f;

        // Shape, wind and evolution defaults live in the portable CloudShape.
        // Climate factors layer over the existing location-specific settings.
        bool zoneClouds = true;
        bool weatherClouds = true;
        float transitionSeconds = 12.f;
        CloudWeather weather = CloudWeather::Clear;
        float weatherStrength = 0.f;
        // Retained for the legacy ground-shadow coordinates only.
        float planetRadius = 42000.f;

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
        // layer seen edge-on is 25000 units long, and the width of the fade
        // band that hides the layer's edge at the horizon.
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

        // Cloud shadows and rays share a small map on a fixed world plane.
        // It stores the unitless cloud amount of the column between each
        // point and the top of the deck - the deck's own shape, projected
        // along the sunlight - so the ground under a cloud is dimmed and the
        // ground beside it is not, at any sun elevation.
        // `groundShadowStrength` is how much of the light a solid column
        // removes, and the camera only selects the map's texel-snapped
        // coverage: each receiver is projected along sunlight using its own
        // world height.
        bool groundShadows = true;
        float groundShadowStrength = .55f;
        float groundShadowExtent = 24000.f;
        // Cloud-filtered beams are off by default: the visible sun rays are the
        // object-occluded god rays. The F7 toggle still turns them back on.
        bool sunRays = false;
        float sunRayStrength = .8f;
        // Overall opacity of the cloud material (0..3). Multiplies the
        // base extinction so the deck can be made more opaque without
        // changing its lighting response.
        float opacity = 1.f;

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
        // Fingerprint of the last settings Configure() fully applied. A
        // crossing that resolves this section to the same values skips the
        // re-apply (see Configure). 0 = never applied.
        uint64_t m_settingsFingerprint = 0;
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
        std::wstring m_shaderCacheDir;
        std::wstring m_cachePath;
        RetryGate m_shaderRetry;
        bool m_fieldsReady = false;
        IDirect3DDevice9* m_fieldsDevice = nullptr;
        // Detached generation can outlive a device transition (or DLL object
        // teardown). Shared state and a copied path keep the worker independent
        // of this renderer instance while publishing its result safely.
        struct FieldWorkerState { std::atomic<bool> generating{false}, failed{false}; };
        std::shared_ptr<FieldWorkerState> m_fieldWorker = std::make_shared<FieldWorkerState>();
        uint64_t m_fieldsWaitStart = 0; // first frame that skipped for the worker
        bool m_shadowValid = false;
        Vec4 m_shadowArea;
        Vec4 m_lightProjection;
        uint32_t m_shadowSize = 0;
        float m_shadowOrigin[2] = { 0.f, 0.f };
        uint64_t m_renderFrames = 0;
        uint64_t m_windSequence = 0;
        FrameClock m_clock;
        CloudDynamics m_dynamics;
    };
}
