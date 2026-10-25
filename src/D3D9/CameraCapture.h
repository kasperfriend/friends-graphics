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

        // Called at the start of a depth-captured frame.
        void BeginFrame()
        {
            m_skyPose.BeginFrame();
            m_fallbackSawSceneDraw = false;
            m_rejectedCaptureValid = false;
            m_rejectedCaptureProjectionScaleMatched = false;
            m_skyObservationResult = FrameContext::SkyObservationResult::NotAttempted;
            m_skyProjectionAvailable = false;
            m_skyProjectionCacheUsable = false;
            m_skyProjectionCacheMinZ = m_skyProjectionCacheMaxZ = -1.f;
            m_skyProjectionObservedMinZ = m_skyProjectionObservedMaxZ = -1.f;
            m_skyProjectionFallbackUsed = false;
            m_skyEyeCandidateValid = false;
            m_skyEyeCandidateObserved = false;
            m_skyEyeCandidateDistanceYards = 1e30f;
        }

        // Observes D3DTS_VIEW / D3DTS_PROJECTION during the scene pass so that
        // looking straight up at the sky (when all terrain/WMO chunks are
        // frustum-culled below the camera) still tracks the camera orientation
        // and keeps volumetric clouds visible overhead.
        void ObserveSkyFallback(IDirect3DDevice9* device);

        // Commits the sky/retained camera fallback if the scene pass ended
        // (UI draw reached or Present) without a terrain/WMO draw latching.
        bool ApplySkyFallback(
            FrameContext& frameContext,
            const CameraCaptureConfig& config,
            float outConstants[13][4],
            float outCapturedViewTranslation[3],
            bool& outCapturedViewValid);

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
            m_skyPose.Reset();
            m_fallbackProjectionValid = false;
            m_fallbackProjectionSeconds = -1.0;
            m_skyProjectionFallbackUsed = false;
            m_skyEyeCandidateValid = false;
            // No draw has carried an eye on the new device yet, so nothing may
            // claim a fresh one until one does.
            m_lastEyeSeconds = -1.0;
            BeginFrame();
        }

    private:
        CameraCapture() = default;
        void RecordSkyObservation(FrameContext::SkyObservationResult result) noexcept
        {
            if (static_cast<uint8_t>(result) > static_cast<uint8_t>(m_skyObservationResult))
                m_skyObservationResult = result;
        }
        // Which camera the captured light belongs to. Frames that merely look
        // like scene draws (a target portrait's close-up camera) must not
        // re-light the world; see CameraContinuityGate.
        CameraContinuityGate m_cameraGate;
        bool m_valid = false;
        uint32_t m_capturedFrames = 0;
        uint64_t m_cameraCaptureShaderHash = 0;
        float m_smoothedSunX = -1.0f;
        float m_smoothedSunY = -1.0f;
        // The frame's sky candidates, and the rule that decides whether one of
        // them may become the frame's camera (see SkyPoseSelector). Candidates
        // are collected all frame and resolved once, at the UI draw, when the
        // frame's own celestial disc has already been drawn.
        SkyPoseSelector m_skyPose;
        // A transform is only collected as a candidate when it could BE this
        // camera: either it carries no translation at all (the sun's skybox
        // trick, where the rows are the camera's own rotation) or the eye it
        // implies lands within kSkyEyeLeashYards of the accepted one. A
        // camera-relative billboard (the moon: identity rotation, a 4-16 yard
        // translation that swings with the mouse) and another pass's camera
        // (portrait, reflection) fail both and are dropped, because taking
        // their rows or their translation as the camera is exactly the
        // reported "clouds and lights dragged along with the camera".
        bool m_fallbackSawSceneDraw = false;
        // A bounded full-frame shader eye can arrive with a non-camera depth
        // projection, or be refused only by orientation continuity. In either
        // case retain it only for same-frame sky-selector corroboration. The
        // scale-match flag permits renewing the validated projection's age only
        // for the projection-rejected case with matching focal scales.
        bool m_rejectedCaptureValid = false;
        bool m_rejectedCaptureProjectionScaleMatched = false;
        FrameContext::SkyObservationResult m_skyObservationResult =
            FrameContext::SkyObservationResult::NotAttempted;
        float m_skyProjection[6]{}; // P00, P11, P22, P23, P32, P33 from the last finite observation
        bool m_skyProjectionAvailable = false;
        bool m_skyProjectionCacheUsable = false;
        float m_skyProjectionCacheMinZ = -1.f;
        float m_skyProjectionCacheMaxZ = -1.f;
        float m_skyProjectionObservedMinZ = -1.f;
        float m_skyProjectionObservedMaxZ = -1.f;
        bool m_skyProjectionFallbackUsed = false;
        bool m_fallbackProjectionValid = false;
        // Performance-clock seconds of the last validated full-frame projection.
        double m_fallbackProjectionSeconds = -1.0;
        // A current non-zero VIEW translation may carry the actual camera eye.
        // Keep it only when it is within five yards of the accepted anchor, and
        // refresh the anchor only if this exact basis wins sky-pose selection.
        bool m_skyEyeCandidateValid = false;
        bool m_skyEyeCandidateObserved = false;
        float m_skyEyeCandidateDistanceYards = 1e30f;
        float m_skyEyeTranslation[4]{};
        float m_skyEyeBasis[3][4]{};
        float m_rejectedCaptureEye[3]{};
        float m_rejectedCaptureBasis[3][4]{};
        D3DVIEWPORT9 m_rejectedCaptureVp{};
        D3DMATRIX m_fallbackProj{};
        D3DVIEWPORT9 m_fallbackVp{};
        // Performance-clock seconds when a draw last carried the camera's eye.
        // Published as FrameContext::cameraEyeAgeMs; passes that place things
        // within a few yards of the camera refuse an older one.
        double m_lastEyeSeconds = -1.0;
        static constexpr float kSkyEyeLeashYards = 40.0f;
        // Rejected shader captures carry a trustworthy eye, so keep this path
        // much tighter than the looser D3DTS_VIEW sky-transform leash.
        static constexpr float kRejectedCaptureEyeLeashYards = 5.0f;
    };
}
