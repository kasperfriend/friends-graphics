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

        depthAvailable = false;
        cameraValid = false;
        composed = false;
    }
}
