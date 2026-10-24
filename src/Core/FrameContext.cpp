#include "FrameContext.h"

namespace renderer
{
    FrameContext& FrameContext::Current()
    {
        static FrameContext s_current;
        return s_current;
    }

    void FrameContext::ResetForNewFrame(uint64_t newFrameIndex, IDirect3DDevice9* dev)
    {
        frameIndex = newFrameIndex;
        device = dev;
        ResetWorldSceneFrame();

        // Keep the last accepted camera pose/projection as a fallback when a
        // frame contains only a skybox and no terrain draw can recapture it.
        // `cameraValid` is the freshness gate: consumers must not use that pose
        // until this frame's camera capture or sky fallback explicitly accepts it.
        sunScreenX = -1.0f;
        sunScreenY = -1.0f;
        sunStrength = 0.0f;
        moonDiscScreenX = -1.0f;
        moonDiscScreenY = -1.0f;
        moonDiscViewDepth = 0.0f;
        moonDiscValid = false;
        sunDiscScreenX = -1.0f;
        sunDiscScreenY = -1.0f;
        sunDiscViewDepth = 0.0f;
        sunDiscValid = false;
        sunDiscExpectedRawDepth = -1.0f;
        sunDiscRawDepthTolerance = 0.0f;
        celestialIntensity = 0.0f;
        celestialIsMoon = false;
        directLightIntensity = 0.0f;
        directLightIsMoon = false;
        directLightValid = false;
        directLightDirectionView = { 0.0f, 0.0f, 1.0f };
        directLightDirectionWorld = { 0.0f, 0.0f, 1.0f };

        shadowMatrix.SetIdentity();
        viewToShadowMatrix.SetIdentity();
        shadowTexture = nullptr;
        shadowMapValid = false;
        shadowMapSize = 2048;
        shadowMapDistance = 180.0f;

        waterMaskTexture = waterDepthTexture = nullptr;
        nativeWaterMaskCaptured = false;
        // Published by the compositor from the water stage's own tracker before any pass
        // reads it, so a frame that never reaches the composite must not inherit the medium
        // the last one left behind.
        underwaterRamp = 0.f;
        depthTexture = nullptr;
        depthSurface = nullptr;
        sceneColor = nullptr;
        sceneSurface = nullptr;
        atmosphereNoise = nullptr;
        cloudTexture = cloudShadowTexture = nullptr;
        cloudShadowArea = {};
        cloudLightProjection = {};
        cloudRayStrength = 0.f;

        // A pose published by the previous frame says nothing about this one:
        // the capture (or the corroborated sky fallback) re-publishes all of
        // these as it publishes the pose itself. Until then a consumer must
        // treat the pose as stale, which the boolean's own default does.
        cameraPoseStale = true;
        // No draw has carried an eye into THIS frame yet: a consumer that asks
        // how old the eye is must not read "just now".
        cameraEyeAgeMs = 1.0e6f;
        cameraEyeObservedThisFrame = false;
        cameraPoseSource = PoseNone;
        skyPoseCandidateCount = 0;
        skyPoseDiscEvidence = false;
        skyEyeCandidateObserved = false;
        skyEyeCandidateDistanceYards = -1.f;
        skyObservationResult = SkyObservationResult::NotAttempted;
        skyProjection[0] = skyProjection[1] = skyProjection[2] =
            skyProjection[3] = skyProjection[4] = skyProjection[5] = 0.0f;
        skyProjectionAvailable = false;
        skyProjectionCacheUsable = false;
        skyProjectionCacheAgeMs = -1.f;
        skyProjectionCacheScaleX = skyProjectionCacheScaleY = 0.f;
        skyProjectionFallbackUsed = false;

        depthAvailable = false;
        cameraValid = false;
        composed = false;
    }
}
