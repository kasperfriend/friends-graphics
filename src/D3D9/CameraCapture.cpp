#include "CameraCapture.h"
#include "CelestialTracker.h"
#include "../Core/FrameClock.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace renderer
{
    CameraCapture& CameraCapture::Instance()
    {
        static CameraCapture instance;
        return instance;
    }

    bool CameraCapture::Capture(
        IDirect3DDevice9* device,
        FrameContext& frameContext,
        const CameraCaptureConfig& config,
        float outConstants[13][4],
        float outCapturedViewTranslation[3],
        bool& outCapturedViewValid,
        uint64_t shaderHash)
    {
        if (!device)
            return false;

        m_cameraCaptureShaderHash = shaderHash;

        float v[27][4]{};
        D3DVIEWPORT9 vp{};

        if (FAILED(device->GetVertexShaderConstantF(0, v[0], 27)) ||
            FAILED(device->GetViewport(&vp)) ||
            vp.MinZ != 0.0f || vp.MaxZ <= 0.0f)
            return false;

        for (auto& row : v)
            for (float f : row)
                if (!std::isfinite(f))
                    return false;

        // Only a draw covering the whole render target can be the scene camera
        // whose light this frame's sky and fog are built from. A target/portrait
        // draw qualifies on every check above - rigid view, standard projection -
        // but renders into a small viewport with a close-up camera of its own, and
        // latching it is the reported "click an NPC and the world brightens":
        // the frame inherited the portrait's light direction and daylight factors.
        IDirect3DSurface9* renderTarget = nullptr;
        D3DSURFACE_DESC targetDesc{};
        const bool fullFrame = SUCCEEDED(device->GetRenderTarget(0, &renderTarget)) && renderTarget &&
            SUCCEEDED(renderTarget->GetDesc(&targetDesc)) &&
            vp.X == 0 && vp.Y == 0 && vp.Width == targetDesc.Width && vp.Height == targetDesc.Height;
        if (renderTarget) renderTarget->Release();
        if (!fullFrame)
            return false;

        // Verify standard WoW perspective projection and rigid world-view
        if (v[4][0] <= 0.0f || v[5][1] <= 0.0f || v[6][2] <= 1.0f || v[7][2] >= 0.0f ||
            fabsf(v[6][3] - 1.0f) > 0.001f || fabsf(v[7][3]) > 0.001f)
            return false;

        // Verify orthogonality of rotation sub-matrix (v[0..2])
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                float dot = 0.0f;
                for (int k = 0; k < 3; ++k)
                    dot += v[i][k] * v[j][k];
                if (fabsf(dot - (i == j ? 1.0f : 0.0f)) > 0.002f)
                    return false;
            }
        }

        // Even at full frame, the first qualifying draw is not automatically
        // this frame's camera - the gate holds the last accepted one and asks
        // whether this candidate can be it. A rejection is not an overwrite:
        // nothing below runs, the previous camera's factors stay in place, and
        // the real scene draw that follows in the same frame still latches.
        if (!m_cameraGate.Accept(v[3], v, vp.Width, vp.Height))
            return false;

        // Swap BEFORE overwriting: whatever viewRaw held (last successful
        // capture's forward view) becomes previousViewRaw for this frame's
        // temporal reprojection use; then this frame's raw rows are stored
        // for the same swap to happen next frame.
        frameContext.previousViewRaw = frameContext.viewRaw;
        frameContext.previousViewValid = frameContext.viewRawValid;
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                frameContext.viewRaw.m[row][col] = v[row][col];
        frameContext.viewRawValid = true;
        CelestialTracker::Instance().RefreshWorldDirections(&frameContext.viewRaw.m[0][0]);

        memset(outConstants, 0, sizeof(float) * 13 * 4);

        // c0: Projection unpacking factors [P22, P32, P00, P11]
        outConstants[0][0] = v[6][2];
        outConstants[0][1] = v[7][2];
        outConstants[0][2] = v[4][0];
        outConstants[0][3] = v[5][1];

        // c1: Fog parameters
        outConstants[1][0] = config.baseHeight;
        outConstants[1][1] = config.falloff;
        outConstants[1][2] = config.density;
        outConstants[1][3] = 350.0f;

        float directLuminance = v[26][0] * 0.2126f + v[26][1] * 0.7152f + v[26][2] * 0.0722f;

        float daylightBrightness = std::clamp((directLuminance - 0.18f) * 4.0f, 0.0f, 1.0f);
        float daylightTint = std::clamp((v[26][0] - v[26][2]) * 4.0f + 0.35f, 0.0f, 1.0f);
        float daylight = daylightBrightness * daylightTint;

        float moonBrightness = std::clamp((directLuminance - 0.12f) * 4.0f, 0.0f, 1.0f);
        float moonTint = std::clamp((v[26][2] - v[26][0]) * 6.0f, 0.0f, 1.0f);
        float moonlight = moonBrightness * moonTint * (1.0f - daylight);

        float celestialShadowLight = std::clamp((directLuminance - 0.08f) * 3.8f, 0.18f, 1.0f);

        outConstants[2][0] = -1.0f;
        outConstants[2][1] = -1.0f;
        outConstants[2][2] = 0.0f;
        outConstants[2][3] = vp.MaxZ;

        float lightLen = std::sqrt(v[24][0] * v[24][0] + v[24][1] * v[24][1] + v[24][2] * v[24][2]);
        if (lightLen > 1e-4f)
        {
            // View-space light vector points towards celestial body:
            // In WoW, v[24] is light propagation (towards camera/down).
            // Facing the sun/moon is in direction (-v[24][0], -v[24][1], +v[24][2]).
            float lx = -v[24][0] / lightLen;
            float ly = -v[24][1] / lightLen;
            float lz =  v[24][2] / lightLen;
            outConstants[10][0] = lx;
            outConstants[10][1] = ly;
            outConstants[10][2] = lz;
            outConstants[10][3] = 0.0f;

            if (lz > 0.02f)
            {
                // The celestial disc and world geometry use this same perspective
                // projection. Keeping the shaft origin in the identical space is
                // essential: a dome/yaw-pitch approximation drifts vertically as
                // the player tilts the camera and only occasionally meets the sun.
                const float inverseZ = 1.0f / lz;
                const float targetX = std::clamp(
                    0.5f + 0.5f * lx * inverseZ * v[4][0] + config.sunOffsetX,
                    -1.5f, 2.5f);
                const float targetY = std::clamp(
                    0.5f - 0.5f * ly * inverseZ * v[5][1] *
                        std::max(config.sunVerticalScale, 0.05f) + config.sunOffsetY,
                    -1.5f, 2.5f);

                // Do not smooth screen coordinates: even a short temporal lag
                // visibly detaches the rays from the disc during camera motion.
                m_smoothedSunX = targetX;
                m_smoothedSunY = targetY;

                outConstants[2][0] = m_smoothedSunX;
                outConstants[2][1] = m_smoothedSunY;

                float facing = std::clamp((lz + 0.05f) * 2.5f, 0.0f, 1.0f);
                if (targetX < -0.35f || targetX > 1.35f || targetY < -0.35f || targetY > 1.35f)
                    facing = 0.0f;

                outConstants[2][2] = config.strength * (daylight + config.moonStrength * moonlight) * facing;

                frameContext.sunScreenX = m_smoothedSunX;
                frameContext.sunScreenY = m_smoothedSunY;
                frameContext.sunStrength = outConstants[2][2];
            }
            else
            {
                m_smoothedSunX = -1.0f;
                m_smoothedSunY = -1.0f;
                frameContext.sunScreenX = -1.0f;
                frameContext.sunScreenY = -1.0f;
                frameContext.sunStrength = 0.0f;
            }

            frameContext.sunDirectionView = { lx, ly, lz };
        }
        else
        {
            m_smoothedSunX = -1.0f;
            m_smoothedSunY = -1.0f;
            outConstants[10][0] = 0.0f;
            outConstants[10][1] = 0.0f;
            outConstants[10][2] = 1.0f;
            outConstants[10][3] = 0.0f;
            frameContext.sunScreenX = -1.0f;
            frameContext.sunScreenY = -1.0f;
            frameContext.sunStrength = 0.0f;
        }

        // Ambient / horizon color
        for (int i = 0; i < 3; ++i)
        {
            float horizon = v[25][i] * 1.5f + v[26][i] * 0.30f;
            outConstants[3][i] = std::clamp(horizon, 0.12f, 0.95f);
        }

        // c4..c6: Inverse view rotation
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                outConstants[4 + i][j] = v[j][i];

        // c7: Camera world position
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                outConstants[7][i] -= v[3][j] * v[i][j];
        outConstants[7][3] = 1.0f;

        // c8: Fog wash and debug
        outConstants[8][0] = 3.0f;
        outConstants[8][1] = config.fogWash;
        outConstants[8][2] = config.shaftDebug ? 1.0f : 0.0f;

        // View translation
        outCapturedViewTranslation[0] = v[3][0];
        outCapturedViewTranslation[1] = v[3][1];
        outCapturedViewTranslation[2] = v[3][2];
        outCapturedViewValid = true;

        // c9: Time and variation
        // Performance-counter seconds: GetTickCount64 advanced this in
        // ~15.6 ms steps, so the legacy fog detail visibly ticked.
        outConstants[9][0] = float(std::fmod(PerformanceSeconds(), 1200.0));
        outConstants[9][1] = 0.018f;
        outConstants[9][2] = config.variation;
        outConstants[9][3] = config.lowLayer;

        // c11: Direct light color
        for (int i = 0; i < 3; ++i)
            outConstants[11][i] = v[26][i];

        // c12: Viewport and ray params
        outConstants[12][0] = 1.0f / static_cast<float>(vp.Width);
        outConstants[12][1] = 1.0f / static_cast<float>(vp.Height);
        outConstants[12][2] = config.raySoftness;
        outConstants[12][3] = config.rayFalloff;

        // Populate FrameContext
        frameContext.cameraPosition = { outConstants[7][0], outConstants[7][1], outConstants[7][2] };
        frameContext.daylightFactor = daylight;
        frameContext.moonlightFactor = moonlight;
        frameContext.shadowLightFactor = celestialShadowLight;
        frameContext.directionalLightColor = { v[26][0], v[26][1], v[26][2] };
        frameContext.viewport = vp;
        frameContext.width = vp.Width;
        frameContext.height = vp.Height;
        frameContext.cameraValid = true;

        frameContext.projUnpack[0] = outConstants[0][0]; // P22
        frameContext.projUnpack[1] = outConstants[0][1]; // P32
        frameContext.projUnpack[2] = outConstants[0][2]; // P00
        frameContext.projUnpack[3] = outConstants[0][3]; // P11
        frameContext.depthMaxZ = vp.MaxZ;

        // Inverse view rotation and translation
        frameContext.inverseView.m[0][0] = outConstants[4][0]; frameContext.inverseView.m[0][1] = outConstants[4][1]; frameContext.inverseView.m[0][2] = outConstants[4][2]; frameContext.inverseView.m[0][3] = 0.0f;
        frameContext.inverseView.m[1][0] = outConstants[5][0]; frameContext.inverseView.m[1][1] = outConstants[5][1]; frameContext.inverseView.m[1][2] = outConstants[5][2]; frameContext.inverseView.m[1][3] = 0.0f;
        frameContext.inverseView.m[2][0] = outConstants[6][0]; frameContext.inverseView.m[2][1] = outConstants[6][1]; frameContext.inverseView.m[2][2] = outConstants[6][2]; frameContext.inverseView.m[2][3] = 0.0f;
        frameContext.inverseView.m[3][0] = outConstants[7][0]; frameContext.inverseView.m[3][1] = outConstants[7][1]; frameContext.inverseView.m[3][2] = outConstants[7][2]; frameContext.inverseView.m[3][3] = 1.0f;

        // World-space sun direction: transform lightView by inverse view rotation
        Vec3 invX{ outConstants[4][0], outConstants[4][1], outConstants[4][2] };
        Vec3 invY{ outConstants[5][0], outConstants[5][1], outConstants[5][2] };
        Vec3 invZ{ outConstants[6][0], outConstants[6][1], outConstants[6][2] };
        Vec3 lightView{ outConstants[10][0], outConstants[10][1], outConstants[10][2] };
        Vec3 sunWorld = {
            invX.x * lightView.x + invY.x * lightView.y + invZ.x * lightView.z,
            invX.y * lightView.x + invY.y * lightView.y + invZ.y * lightView.z,
            invX.z * lightView.x + invY.z * lightView.y + invZ.z * lightView.z
        };
        float swLen = sqrtf(sunWorld.x * sunWorld.x + sunWorld.y * sunWorld.y + sunWorld.z * sunWorld.z);
        if (swLen > 1e-4f) {
            sunWorld.x /= swLen; sunWorld.y /= swLen; sunWorld.z /= swLen;
            // Only a direction with the light above the horizon is accepted.
            // The old code clamped z up to 0.04 and renormalised instead,
            // which did not repair a low or ambiguous result: it MANUFACTURED
            // a sun 2.3 degrees above the horizon out of whatever x and y the
            // frame happened to carry, and the cloud deck was then lit from
            // that direction and its ground shadow map projected along it at
            // seventeen times the receiver's height. Because the survivors of
            // that clamp keep the camera's own rotation in x and y, panning
            // the camera rotated the deck's light - the residual "light on
            // the clouds follows me" report. Holding the last usable
            // direction cannot rotate anything, and the end of the day is
            // already expressed by the daylight/moonlight factors every
            // consumer multiplies in.
            if (sunWorld.z >= 0.04f)
                frameContext.sunDirectionWorld = sunWorld;
        }

        m_valid = true;
        ++m_capturedFrames;
        return true;
    }

    void CameraCapture::ObserveSkyFallback(IDirect3DDevice9* device)
    {
        if (!device || !m_valid || !m_cameraGate.Valid())
            return;

        D3DVIEWPORT9 vp{};
        if (FAILED(device->GetViewport(&vp)) || vp.MaxZ <= 0.0f)
            return;

        IDirect3DSurface9* renderTarget = nullptr;
        D3DSURFACE_DESC targetDesc{};
        const bool fullFrame = SUCCEEDED(device->GetRenderTarget(0, &renderTarget)) && renderTarget &&
            SUCCEEDED(renderTarget->GetDesc(&targetDesc)) &&
            vp.X == 0 && vp.Y == 0 && vp.Width == targetDesc.Width && vp.Height == targetDesc.Height;
        if (renderTarget) renderTarget->Release();
        if (!fullFrame)
            return;

        // Record that a full-frame scene/sky draw occurred during this depth-captured frame.
        float m2Consts[6][4]{};
        if (SUCCEEDED(device->GetVertexShaderConstantF(0, m2Consts[0], 6)))
        {
            const bool isM2Proj = (fabsf(m2Consts[5][2] - 1.0f) < 0.02f &&
                                   fabsf(m2Consts[5][0]) < 0.01f &&
                                   fabsf(m2Consts[5][1]) < 0.01f &&
                                   fabsf(m2Consts[5][3]) < 0.01f &&
                                   m2Consts[2][0] > 0.1f &&
                                   m2Consts[3][1] > 0.1f);
            if (isM2Proj)
                m_fallbackSawSceneDraw = true;
        }

        D3DMATRIX view{}, proj{};
        if (FAILED(device->GetTransform(D3DTS_VIEW, &view)) ||
            FAILED(device->GetTransform(D3DTS_PROJECTION, &proj)))
            return;

        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                if (!std::isfinite(view.m[r][c]) || !std::isfinite(proj.m[r][c]))
                    return;

        if (proj.m[0][0] <= 0.0f || proj.m[1][1] <= 0.0f || proj.m[2][2] <= 1.0f || proj.m[3][2] >= 0.0f ||
            fabsf(proj.m[2][3] - 1.0f) > 0.001f || fabsf(proj.m[3][3]) > 0.001f)
            return;

        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                float dot = 0.0f;
                for (int k = 0; k < 3; ++k)
                    dot += view.m[i][k] * view.m[j][k];
                if (fabsf(dot - (i == j ? 1.0f : 0.0f)) > 0.002f)
                    return;
            }
        }

        // Reject the moon billboard's camera-relative identity D3DTS_VIEW.
        const bool isIdentityRotation =
            fabsf(view.m[0][0] - 1.0f) < 1e-3f &&
            fabsf(view.m[1][1] - 1.0f) < 1e-3f &&
            fabsf(view.m[2][2] - 1.0f) < 1e-3f;
        if (isIdentityRotation && view.m[3][2] > 4.0f && view.m[3][2] < 16.0f)
            return;

        m_fallbackSawSceneDraw = true;
        // Pitch/yaw may change by far more than the normal scene-capture gate's
        // one-frame threshold when the player snaps the camera toward zenith.
        // The full-frame viewport and the position check below distinguish that
        // turn from an unrelated portrait/reflection camera; basis similarity
        // must not freeze the sky fallback on yesterday's view.
        if (!m_cameraGate.MatchesViewport(vp.Width, vp.Height))
            return;

        const bool zeroTrans =
            fabsf(view.m[3][0]) < 0.05f &&
            fabsf(view.m[3][1]) < 0.05f &&
            fabsf(view.m[3][2]) < 0.05f;
        bool hasWorldTranslation = false;
        if (!zeroTrans)
        {
            const float px = -(view.m[3][0] * view.m[0][0] + view.m[3][1] * view.m[0][1] + view.m[3][2] * view.m[0][2]);
            const float py = -(view.m[3][0] * view.m[1][0] + view.m[3][1] * view.m[1][1] + view.m[3][2] * view.m[1][2]);
            const float pz = -(view.m[3][0] * view.m[2][0] + view.m[3][1] * view.m[2][1] + view.m[3][2] * view.m[2][2]);
            const float* cam = m_cameraGate.Position();
            const float dx = px - cam[0], dy = py - cam[1], dz = pz - cam[2];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) <= 40.0f)
                hasWorldTranslation = true;
        }

        // Prefer a candidate carrying world translation if one arrives, otherwise keep the skybox rotation.
        if (!m_fallbackCandidateValid || hasWorldTranslation || !m_fallbackHasTranslation)
        {
            m_fallbackView = view;
            m_fallbackProj = proj;
            m_fallbackVp = vp;
            m_fallbackHasTranslation = hasWorldTranslation;
            m_fallbackCandidateValid = true;
        }
    }

    bool CameraCapture::ApplySkyFallback(
        FrameContext& frameContext,
        const CameraCaptureConfig& config,
        float outConstants[13][4],
        float outCapturedViewTranslation[3],
        bool& outCapturedViewValid)
    {
        if (!m_valid || !m_cameraGate.Valid() || !frameContext.viewRawValid)
            return false;

        if (!m_fallbackCandidateValid)
        {
            if (!m_fallbackSawSceneDraw)
                return false;
            outConstants[9][0] = float(std::fmod(PerformanceSeconds(), 1200.0));
            frameContext.cameraValid = true;
            CelestialTracker::Instance().RefreshWorldDirections(&frameContext.viewRaw.m[0][0]);
            ++m_capturedFrames;
            return true;
        }

        float v[4][4]{};
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
                v[r][c] = m_fallbackView.m[r][c];
            v[r][3] = 0.0f;
        }

        float camPos[3]{ m_cameraGate.Position()[0], m_cameraGate.Position()[1], m_cameraGate.Position()[2] };
        if (m_fallbackHasTranslation)
        {
            for (int c = 0; c < 3; ++c)
                v[3][c] = m_fallbackView.m[3][c];
            v[3][3] = 1.0f;
            if (!m_cameraGate.AcceptFallback(v[3], v, m_fallbackVp.Width, m_fallbackVp.Height))
                return false;
            camPos[0] = m_cameraGate.Position()[0];
            camPos[1] = m_cameraGate.Position()[1];
            camPos[2] = m_cameraGate.Position()[2];
        }
        else
        {
            m_cameraGate.UpdateBasis(v, m_fallbackVp.Width, m_fallbackVp.Height);
            for (int c = 0; c < 3; ++c)
                v[3][c] = -(camPos[0] * v[0][c] + camPos[1] * v[1][c] + camPos[2] * v[2][c]);
            v[3][3] = 1.0f;
        }

        frameContext.previousViewRaw = frameContext.viewRaw;
        frameContext.previousViewValid = frameContext.viewRawValid;
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                frameContext.viewRaw.m[row][col] = v[row][col];
        frameContext.viewRawValid = true;
        CelestialTracker::Instance().RefreshWorldDirections(&frameContext.viewRaw.m[0][0]);

        // c0: Projection unpacking factors [P22, P32, P00, P11]
        outConstants[0][0] = m_fallbackProj.m[2][2];
        outConstants[0][1] = m_fallbackProj.m[3][2];
        outConstants[0][2] = m_fallbackProj.m[0][0];
        outConstants[0][3] = m_fallbackProj.m[1][1];

        // c1: Fog parameters
        outConstants[1][0] = config.baseHeight;
        outConstants[1][1] = config.falloff;
        outConstants[1][2] = config.density;
        outConstants[1][3] = 350.0f;

        // c4..c6: Inverse view rotation
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                outConstants[4 + i][j] = v[j][i];

        // c7: Camera world position
        outConstants[7][0] = camPos[0];
        outConstants[7][1] = camPos[1];
        outConstants[7][2] = camPos[2];
        outConstants[7][3] = 1.0f;

        // c8: Fog wash and debug
        outConstants[8][0] = 3.0f;
        outConstants[8][1] = config.fogWash;
        outConstants[8][2] = config.shaftDebug ? 1.0f : 0.0f;

        outCapturedViewTranslation[0] = v[3][0];
        outCapturedViewTranslation[1] = v[3][1];
        outCapturedViewTranslation[2] = v[3][2];
        outCapturedViewValid = true;

        // c9: Time and variation
        outConstants[9][0] = float(std::fmod(PerformanceSeconds(), 1200.0));
        outConstants[9][1] = 0.018f;
        outConstants[9][2] = config.variation;
        outConstants[9][3] = config.lowLayer;

        // Re-project the world sun direction into the updated view space
        const Vec3& sunW = frameContext.sunDirectionWorld;
        const float lx = sunW.x * v[0][0] + sunW.y * v[1][0] + sunW.z * v[2][0];
        const float ly = sunW.x * v[0][1] + sunW.y * v[1][1] + sunW.z * v[2][1];
        const float lz = sunW.x * v[0][2] + sunW.y * v[1][2] + sunW.z * v[2][2];
        outConstants[10][0] = lx;
        outConstants[10][1] = ly;
        outConstants[10][2] = lz;
        outConstants[10][3] = 0.0f;
        frameContext.sunDirectionView = { lx, ly, lz };

        // c12: Viewport and ray params
        outConstants[12][0] = 1.0f / static_cast<float>(m_fallbackVp.Width);
        outConstants[12][1] = 1.0f / static_cast<float>(m_fallbackVp.Height);
        outConstants[12][2] = config.raySoftness;
        outConstants[12][3] = config.rayFalloff;

        frameContext.cameraPosition = { camPos[0], camPos[1], camPos[2] };
        frameContext.width = m_fallbackVp.Width;
        frameContext.height = m_fallbackVp.Height;
        if (m_fallbackVp.MinZ == 0.0f)
        {
            frameContext.viewport = m_fallbackVp;
            frameContext.depthMaxZ = m_fallbackVp.MaxZ;
            outConstants[2][3] = m_fallbackVp.MaxZ;
        }
        frameContext.cameraValid = true;

        frameContext.projUnpack[0] = outConstants[0][0];
        frameContext.projUnpack[1] = outConstants[0][1];
        frameContext.projUnpack[2] = outConstants[0][2];
        frameContext.projUnpack[3] = outConstants[0][3];

        frameContext.inverseView.m[0][0] = outConstants[4][0]; frameContext.inverseView.m[0][1] = outConstants[4][1]; frameContext.inverseView.m[0][2] = outConstants[4][2]; frameContext.inverseView.m[0][3] = 0.0f;
        frameContext.inverseView.m[1][0] = outConstants[5][0]; frameContext.inverseView.m[1][1] = outConstants[5][1]; frameContext.inverseView.m[1][2] = outConstants[5][2]; frameContext.inverseView.m[1][3] = 0.0f;
        frameContext.inverseView.m[2][0] = outConstants[6][0]; frameContext.inverseView.m[2][1] = outConstants[6][1]; frameContext.inverseView.m[2][2] = outConstants[6][2]; frameContext.inverseView.m[2][3] = 0.0f;
        frameContext.inverseView.m[3][0] = outConstants[7][0]; frameContext.inverseView.m[3][1] = outConstants[7][1]; frameContext.inverseView.m[3][2] = outConstants[7][2]; frameContext.inverseView.m[3][3] = 1.0f;

        m_fallbackCandidateValid = false;
        ++m_capturedFrames;
        return true;
    }
}
