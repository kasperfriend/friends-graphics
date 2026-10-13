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

        width = 0;
        height = 0;
        viewport = {};

        view.SetIdentity();
        projection.SetIdentity();
        viewProjection.SetIdentity();
        inverseView.SetIdentity();

        cameraPosition = { 0.0f, 0.0f, 0.0f };
        sunDirectionView = { 0.0f, 0.0f, 1.0f };
        sunDirectionWorld = { 0.0f, 0.0f, 1.0f };
        directionalLightColor = { 1.0f, 1.0f, 1.0f };

        daylightFactor = 0.0f;
        moonlightFactor = 0.0f;
        shadowLightFactor = 0.0f;

        sunScreenX = -1.0f;
        sunScreenY = -1.0f;
        sunStrength = 0.0f;
        celestialIntensity = 0.0f;
        celestialIsMoon = false;
        directLightIntensity = 0.0f;
        directLightIsMoon = false;
        directLightValid = false;
        directLightDirectionView = { 0.0f, 0.0f, 1.0f };
        directLightDirectionWorld = { 0.0f, 0.0f, 1.0f };

        projUnpack[0] = projUnpack[1] = projUnpack[2] = projUnpack[3] = 0.0f;
        depthMaxZ = 1.0f;

        shadowMatrix.SetIdentity();
        viewToShadowMatrix.SetIdentity();
        shadowTexture = nullptr;
        shadowMapValid = false;
        shadowMapSize = 2048;
        shadowMapDistance = 180.0f;

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
