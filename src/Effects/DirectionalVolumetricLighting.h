#pragma once
#include <d3d9.h>
#include <wrl/client.h>
#include <string>
#include "../Core/DeviceHealth.h"
#include "../Core/FrameClock.h"
#include "../Core/FrameContext.h"

namespace renderer
{
    using Microsoft::WRL::ComPtr;

    enum class VolumetricDebugMode : uint32_t
    {
        None = 0, OpticalDepth, Transmission, TotalDensity, HeightDensity,
        GroundMistDensity, NoiseDensity, AmbientInscatter, CelestialInscatter,
        RawLowResolution, Temporal, Upsampled,
        // Raw boundary depth itself (pre-integration): scene depth
        // everywhere, water surface depth wherever water covers a pixel.
        // Over water this must show the SURFACE distance, not the seabed -
        // if it still shows seabed distance, the water depth MRT write
        // isn't reaching this texture and the fix isn't actually engaging.
        BoundaryDepth,
        // Isolated local-light scattering term (not the direct surface pool).
        LocalLightScattering
    };

    struct VolumetricSettings
    {
        bool enabled = true;
        uint32_t quality = 1;
        uint32_t sampleCount = 16;
        float resolutionScale = 0.25f;
        float densityScale = 1.0f;
        float maxDistance = 520.0f;
        float aerialStart = 35.0f;
        float aerialDensity = 0.002f;
        float heightDensity = 0.005f;
        float heightFalloff = 0.055f;
        float fogBaseOffset = -6.0f;
        float mistDensity = 0.009f;
        float mistFalloff = 0.42f;
        float mistBaseOffset = -3.0f;
        float noiseAmount = 0.22f;
        // Separate local world-space fog volume. It is evaluated inside the
        // same raymarch as the atmosphere, so extinction/scattering, depth
        // occlusion, temporal reprojection and water boundaries remain one
        // coherent medium rather than a fullscreen alpha overlay.
        bool edgeFogEnabled = false;
        float edgeFogDistance = 185.f;
        float edgeFogPower = 2.f;
        bool localFogEnabled = false;
        float localFogDensity = 0.0f;
        float localFogHeightFalloff = 1.f / 3.f;
        float localFogBaseOffset = -1.0f;
        float localFogWakeStrength = 0.85f;
        float localFogWakeRadius = 5.0f;
        float localFogTrailLength = 18.0f;
        float extinction = 1.0f;
        float moonStrength = 0.16f;
        bool temporalEnabled = true;
        float temporalBlend = 0.88f;
        bool edgeAwareBilateral = true;
        // Halo around the confirmed sun/moon disc, inside the haze itself -
        // independent of the screen-space god-ray shaft pass. 0 = no halo.
        float sunGlowStrength = 0.8f;
        // How strongly the atmosphere replaces/dims the scene behind it vs
        // letting it show through. Lower preserves distant detail (e.g.
        // open water) instead of washing it to a flat colour.
        float fogWash = 0.55f;
        // ROUND 5 Phase 16-17: independent multipliers on top of fogWash -
        // extinctionStrength controls how much distant geometry fades out,
        // scatterStrength controls how much light the atmosphere itself
        // adds toward the camera. Both default 1.0 (no change from the
        // single-wash blend); not exposed to the tuning overlay yet, set
        // via ini for now.
        float extinctionStrength = 1.0f;
        float scatterStrength = 1.0f;
        // Derive aerial start/end distance and ambient fog colour from
        // WoW's own authored fog (captured read-only from real shader
        // constants - see EnvironmentFogCapture) instead of one fixed
        // synthetic profile for every zone. The FOG DISTANCE/DENSITY
        // sliders still apply as a scale on top of the captured baseline,
        // not a replacement for it. Off falls back to the old fixed
        // profile, e.g. to A/B compare or if a zone's capture looks wrong.
        bool useEnvironmentBaseline = true;
        VolumetricDebugMode debugMode = VolumetricDebugMode::None;
    };

    // Historical class name retained to avoid unrelated API churn. This is
    // the dedicated production atmosphere pipeline, not the old shadow march.
    class DirectionalVolumetricLighting
    {
    public:
        static DirectionalVolumetricLighting& Instance();
        void Configure(const std::wstring& basePath);
        void Reset(IDirect3DDevice9* device);
        // Drops temporal and ground-tracking history after a frame-stream or
        // playable-world break, so a menu/camera scene cannot seed the next map.
        void InvalidateHistory()
        {
            m_historyValid = false;
            m_previousViewValid = false;
            m_groundHeightIssued[0] = m_groundHeightIssued[1] = false;
        }
        // Every term this pass integrates is an integral over AIR, and the camera's own medium
        // is published in FrameContext::underwaterRamp: a camera inside the liquid is not
        // inside this one, so the densities are scaled by what is left of the air (see the
        // definition) and the pass does not run at all once the ramp reaches 1.
        bool Render(IDirect3DDevice9* device, const FrameContext& frameContext,
                    IDirect3DSurface9* targetSurface, bool globalFogEnabled = true);
        VolumetricSettings& Settings() { return m_settings; }
        const VolumetricSettings& Settings() const { return m_settings; }
        IDirect3DTexture9* GetInteractionState() const { return m_groundHeightTexture[1-m_groundHeightWriteIndex].Get(); }
        IDirect3DTexture9* GetRaymarchTexture() const { return m_integratedTexture.Get(); }
        IDirect3DTexture9* GetResolvedFogTexture() const { return m_upsampledTexture.Get(); }

    private:
        DirectionalVolumetricLighting() = default;
        bool EnsureShaders(IDirect3DDevice9* device);
        bool EnsureResources(IDirect3DDevice9* device, uint32_t fullWidth, uint32_t fullHeight, D3DFORMAT targetFormat);
        void DrawScreenQuad(IDirect3DDevice9* device, uint32_t width, uint32_t height);
        bool ValidateHistory(const FrameContext& frameContext);

        VolumetricSettings m_settings;
        // Fingerprint of the last settings Configure() fully applied; a
        // crossing that resolves the same values skips the re-apply (which
        // also keeps the temporal history valid instead of shimmering a
        // frame). 0 = never applied.
        uint64_t m_settingsFingerprint = 0;
        IDirect3DDevice9* m_owner = nullptr;
        uint32_t m_fullWidth = 0, m_fullHeight = 0, m_lowWidth = 0, m_lowHeight = 0;
        D3DFORMAT m_targetFormat = D3DFMT_UNKNOWN;
        ComPtr<IDirect3DTexture9> m_integratedTexture;
        ComPtr<IDirect3DSurface9> m_integratedSurface;
        ComPtr<IDirect3DTexture9> m_historyTexture[2];
        ComPtr<IDirect3DSurface9> m_historySurface[2];
        ComPtr<IDirect3DTexture9> m_depthTexture[2];
        ComPtr<IDirect3DSurface9> m_depthSurface[2];
        ComPtr<IDirect3DTexture9> m_upsampledTexture;
        ComPtr<IDirect3DSurface9> m_upsampledSurface;
        // Full-res "where does air stop" depth: scene depth everywhere,
        // except water pixels, where it's the water SURFACE depth instead
        // of the seabed depth already sitting in the main depth buffer
        // (water doesn't write that). Computed once per frame and fed into
        // both the low-res downsample and the full-res bilateral upsample,
        // so the atmosphere never integrates air through water to the
        // seabed - see DirectionalVolumetricLighting.cpp.
        ComPtr<IDirect3DTexture9> m_boundaryTexture;
        ComPtr<IDirect3DSurface9> m_boundarySurface;
        uint32_t m_historyReadIndex = 0;
        bool m_historyValid = false;
        RetryGate m_setupRetry;
        ComPtr<IDirect3DPixelShader9> m_depthShader;
        ComPtr<IDirect3DPixelShader9> m_integrateShader;
        ComPtr<IDirect3DPixelShader9> m_temporalShader;
        ComPtr<IDirect3DPixelShader9> m_upsampleShader;
        ComPtr<IDirect3DPixelShader9> m_compositeShader;
        ComPtr<IDirect3DPixelShader9> m_boundaryShader;
        ComPtr<IDirect3DPixelShader9> m_boundaryDebugShader;
        // GPU-only ping-pong state: actor XY, ground Z, wake, planar velocity.
        ComPtr<IDirect3DPixelShader9> m_groundHeightShader;
        ComPtr<IDirect3DPixelShader9> m_localFogFieldShader;
        ComPtr<IDirect3DTexture9> m_localFogFieldTexture;
        ComPtr<IDirect3DSurface9> m_localFogFieldSurface;
        ComPtr<IDirect3DTexture9> m_groundHeightTexture[2];
        ComPtr<IDirect3DSurface9> m_groundHeightSurface[2];
        uint32_t m_groundHeightWriteIndex = 0;
        bool m_groundHeightIssued[2]{};
        Vec3 m_previousCamera{};
        float m_previousProjection[4]{};
        Matrix4 m_previousView{};
        bool m_previousViewValid = false;
        std::wstring m_logPath;
        std::wstring m_shaderCacheDir;
        uint64_t m_renderFrames = 0;
        FrameClock m_fogClock;
        float m_fogElapsed = 0;
    };
}
