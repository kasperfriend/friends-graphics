#pragma once
#include <d3d9.h>
#include <cstdint>
#include "../Core/FrameContext.h"
#include "CameraContinuity.h"

namespace renderer
{
    struct CameraCaptureConfig
    {
        float sunOffsetX = 0.0f;
        float sunOffsetY = 0.0f;
        float sunVerticalScale = 1.0f;
        float strength = 1.0f;
        float moonStrength = 0.25f;
        float baseHeight = 0.0f;
        float falloff = 0.005f;
        float density = 1.0f;
        float fogWash = 0.5f;
        bool shaftDebug = false;
        float variation = 0.5f;
        float lowLayer = 0.2f;
        float raySoftness = 0.1f;
        float rayFalloff = 0.95f;
    };

    class CameraCapture
    {
    public:
        static CameraCapture& Instance();

        bool Capture(
            IDirect3DDevice9* device,
            FrameContext& frameContext,
            const CameraCaptureConfig& config,
            float outConstants[13][4],
            float outCapturedViewTranslation[3],
            bool& outCapturedViewValid,
            uint64_t shaderHash);

        bool IsValid() const { return m_valid; }
        uint32_t GetCapturedFrames() const { return m_capturedFrames; }
        uint64_t GetCameraCaptureShaderHash() const { return m_cameraCaptureShaderHash; }
        void Reset()
        {
            m_valid = false;
            m_cameraCaptureShaderHash = 0;
            m_smoothedSunX = -1.0f;
            m_smoothedSunY = -1.0f;
            m_cameraGate.Reset();
        }

    private:
        CameraCapture() = default;
        // Which camera the captured light belongs to. Frames that merely look
        // like scene draws (a target portrait's close-up camera) must not
        // re-light the world; see CameraContinuityGate.
        CameraContinuityGate m_cameraGate;
        bool m_valid = false;
        uint32_t m_capturedFrames = 0;
        uint64_t m_cameraCaptureShaderHash = 0;
        float m_smoothedSunX = -1.0f;
        float m_smoothedSunY = -1.0f;
    };
}
