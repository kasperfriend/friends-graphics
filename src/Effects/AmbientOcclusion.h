#pragma once
#include <d3d9.h>
#include <wrl/client.h>
#include <string>
#include "../Core/DeviceHealth.h"
#include "../Core/FrameContext.h"
#include "AmbientOcclusionCore.h"

namespace renderer
{
    using Microsoft::WRL::ComPtr;

    // Screen-space ambient occlusion, integrated into the atmosphere
    // composite before clouds, local light and fog so only scene geometry is
    // ever darkened. The math lives in AmbientOcclusionCore.h; this class owns
    // the two ps_3_0 passes (generate, blur+apply), the rotation noise and the
    // GraphicsEffects.ini [AmbientOcclusion] tuning.
    class AmbientOcclusion
    {
    public:
        static AmbientOcclusion& Instance();
        void Configure(const std::wstring& basePath);
        void Reset(IDirect3DDevice9* device);

        // True when the pass should run: enabled, visible strength and no
        // debug-independent gate failed. VolumeIntegration consults this for
        // HasActiveEffects so depth capture stays on for AO-only setups.
        bool IsActive() const;

        // Darkens `targetSurface` in place (multiply blend). Returns true when
        // the pass drew this frame, so the caller refreshes the scene copy.
        bool Render(IDirect3DDevice9* device, const FrameContext& frameContext,
                    IDirect3DSurface9* targetSurface, IDirect3DTexture9* depthTexture);

        ambientocclusion::Settings& Settings() { return m_settings; }
        const ambientocclusion::Settings& Settings() const { return m_settings; }

    private:
        AmbientOcclusion() = default;
        bool EnsureShaders(IDirect3DDevice9* device);
        bool EnsureResources(IDirect3DDevice9* device, uint32_t width, uint32_t height);
        void NoteFailure(const std::string& reason);

        ambientocclusion::Settings m_settings;
        // Fingerprint of the last settings Configure() fully applied; a
        // crossing that resolves the same values skips the re-apply and its
        // log write. 0 = never applied.
        uint64_t m_settingsFingerprint = 0;
        IDirect3DDevice9* m_owner = nullptr;
        uint32_t m_width = 0, m_height = 0;
        ComPtr<IDirect3DTexture9> m_aoTexture;
        ComPtr<IDirect3DSurface9> m_aoSurface;
        // Static 4x4 kernel rotations (AmbientOcclusionCore::RotationFor) and
        // a 1x1 transparent stand-in for frames with no water mask.
        ComPtr<IDirect3DTexture9> m_noiseTexture;
        ComPtr<IDirect3DTexture9> m_fallbackMask;
        ComPtr<IDirect3DPixelShader9> m_generateShader;
        ComPtr<IDirect3DPixelShader9> m_applyShader;
        RetryGate m_setupRetry;
        bool m_shaderErrorLogged = false;
        bool m_resourceErrorLogged = false;
        std::wstring m_logPath;
        std::wstring m_shaderCacheDir;
    };
}
