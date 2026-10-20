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

        // This frame's pose comes from a draw that carried BOTH the rotation and
        // the eye, so nothing about it can be stale: publish that, and let the
        // sky fallback judge its candidates against this camera from now on.
        float capturedBasis[3][4]{};
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
                capturedBasis[r][c] = v[r][c];
            capturedBasis[r][3] = 0.f;
        }
        m_skyPose.SetReference(capturedBasis);
        m_lastEyeSeconds = PerformanceSeconds();
        frameContext.cameraPoseStale = false;
        frameContext.cameraEyeAgeMs = 0.f;
        frameContext.cameraPoseSource = FrameContext::PoseCaptured;

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

        if (!m_cameraGate.MatchesViewport(vp.Width, vp.Height))
            return;

        float basis[3][4]{};
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
                basis[r][c] = view.m[r][c];
            basis[r][3] = 0.f;
            const float lengthSquared = basis[r][0] * basis[r][0] + basis[r][1] * basis[r][1] +
                basis[r][2] * basis[r][2];
            // A rigid camera basis is unit length; rows of any other length mean
            // the transform carries a scale (a scaled billboard's, above all) and
            // its rows are not this camera's axes.
            if (fabsf(lengthSquared - 1.0f) > 0.01f)
                return;
            for (int other = r + 1; other < 3; ++other)
                if (fabsf(basis[r][0] * basis[other][0] + basis[r][1] * basis[other][1] +
                          basis[r][2] * basis[other][2]) > 0.02f)
                    return;
        }

        m_fallbackSawSceneDraw = true;
        m_fallbackProj = proj;
        m_fallbackVp = vp;

        // Does this transform carry the camera's EYE, or a camera-relative
        // offset? The sun is drawn with a zero view translation (the classic
        // skybox trick; its rows therefore ARE the camera's rotation) and the
        // moon as a billboard at a fixed render distance along the light (its
        // translation swings with the mouse and its rotation is not the
        // camera's at all). Only a zero translation, or one that lands at the
        // eye we already accepted, can belong to this frame's camera. Anything
        // else is dropped here rather than merely remembered as unusable: its
        // rows are equally not the camera, and collecting them let a stray
        // identity transform win the "nothing continuous" path.
        const bool zeroTranslation =
            fabsf(view.m[3][0]) < 0.05f && fabsf(view.m[3][1]) < 0.05f && fabsf(view.m[3][2]) < 0.05f;
        bool eyeCompatible = zeroTranslation;
        if (!zeroTranslation)
        {
            const float px = -(view.m[3][0] * basis[0][0] + view.m[3][1] * basis[0][1] + view.m[3][2] * basis[0][2]);
            const float py = -(view.m[3][0] * basis[1][0] + view.m[3][1] * basis[1][1] + view.m[3][2] * basis[1][2]);
            const float pz = -(view.m[3][0] * basis[2][0] + view.m[3][1] * basis[2][1] + view.m[3][2] * basis[2][2]);
            const float* anchor = m_cameraGate.Position();
            const float dx = px - anchor[0], dy = py - anchor[1], dz = pz - anchor[2];
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            eyeCompatible = std::isfinite(distance) && distance <= kSkyEyeLeashYards;
        }
        if (!eyeCompatible)
            return;

        // The frame's candidates are decided at the UI draw, when the frame's
        // own sun/moon disc has already been drawn and can testify about the
        // camera. Collect; do not publish.
        m_skyPose.Observe(basis);
    }

    // What the frame's own celestial draw says about the camera: the stabilized
    // world direction towards the body, and where the client painted it. Both
    // are independent of the pose being tested, which is what makes agreement
    // evidence rather than a pose compared with itself.
    static DiscObservation DiscEvidence(const FrameContext& frameContext, const D3DMATRIX& proj)
    {
        DiscObservation disc;
        const auto& tracker = CelestialTracker::Instance();
        const auto& sun = tracker.Sun();
        const auto& moon = tracker.Moon();
        const bool lightIsMoon = frameContext.directLightValid && frameContext.directLightIsMoon;
        const CelestialBody* body = lightIsMoon ? &moon : &sun;
        if (!body->visible)
            body = sun.visible ? &sun : (moon.visible ? &moon : nullptr);
        if (!body)
            return disc;
        // A disc at the very edge of the frame is half outside it, and its
        // painted position is then the least reliable thing in the frame.
        if (!(body->screenX > .03f && body->screenX < .97f && body->screenY > .03f && body->screenY < .97f))
            return disc;
        // The direction the tracker derived for the SAME billboard is the one
        // paired with screenX/Y; only fall back to the captured light when the
        // tracker's own direction is unusable. (The light direction is a
        // stabilized, possibly held value - good for shading, less direct as
        // evidence about this draw.)
        Vec3 world = body->worldSpaceDirection;
        float lengthSquared = world.x * world.x + world.y * world.y + world.z * world.z;
        if (!(lengthSquared > .25f) || !std::isfinite(lengthSquared))
        {
            world = frameContext.directLightValid ? frameContext.directLightDirectionWorld
                                                  : frameContext.sunDirectionWorld;
            lengthSquared = world.x * world.x + world.y * world.y + world.z * world.z;
            if (!(lengthSquared > .25f) || !std::isfinite(lengthSquared))
                return disc;
        }
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        disc.world[0] = world.x * inverseLength;
        disc.world[1] = world.y * inverseLength;
        disc.world[2] = world.z * inverseLength;
        disc.projectionX = proj.m[0][0];
        disc.projectionY = proj.m[1][1];
        disc.screenX = body->screenX;
        disc.screenY = body->screenY;
        disc.available = true;
        return disc;
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

        // The eye is whatever a draw last carried. A rotation-only sky
        // transform says nothing about where the camera is, so it can age.
        const double now = PerformanceSeconds();
        const float eyeAge = m_lastEyeSeconds >= 0.0
            ? float(std::clamp(now - m_lastEyeSeconds, 0.0, 1e6)) : 1e6f;
        frameContext.cameraEyeAgeMs = eyeAge * 1000.f;
        frameContext.cameraPoseSource = FrameContext::PoseHeld;
        frameContext.cameraPoseStale = true; // until something corroborates it below

        float basis[3][4]{};
        bool corroborated = false;
        const bool havePose = m_skyPose.HasCandidate() &&
            m_skyPose.Resolve(DiscEvidence(frameContext, m_fallbackProj), basis, corroborated);

        if (!havePose)
        {
            // Nothing this frame could be corroborated as the camera. Keep the
            // previous pose published - the atmosphere and the screen-space
            // passes still need a direction - and say plainly that it is stale,
            // so nothing world-anchored is drawn from a camera the player has
            // already left. This is the guard that turns "the clouds and the
            // torches follow me around a city" into "they wait a frame".
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
                v[r][c] = basis[r][c];
            v[r][3] = 0.0f;
        }

        // The corroborated rotation is what the gate should expect a real
        // capture to continue from. Teaching it the basis (never the position -
        // the position is only ever accepted from a draw that carried it) means
        // that when terrain draws appear again on higher ground, the first one
        // is accepted immediately instead of taking three frames to escape the
        // teleport guard.
        m_cameraGate.UpdateBasis(v, m_fallbackVp.Width, m_fallbackVp.Height);

        // The eye stays the one a draw carried. Deliberately NOT the sky
        // transform's translation: for a camera-relative billboard that
        // translation is an offset from the eye that swings with the mouse,
        // and anchoring the world to it is exactly the reported "clouds and
        // lights dragged along with the camera". A transform that does carry
        // the eye only *confirms* that the anchor is plausible; it never
        // moves it.
        const float* anchor = m_cameraGate.Position();
        const float camPos[3] = { anchor[0], anchor[1], anchor[2] };
        for (int c = 0; c < 3; ++c)
            v[3][c] = -(camPos[0] * v[0][c] + camPos[1] * v[1][c] + camPos[2] * v[2][c]);
        v[3][3] = 1.0f;

        frameContext.cameraPoseStale = !corroborated;
        frameContext.cameraPoseSource = FrameContext::PoseSky;

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

        ++m_capturedFrames;
        return true;
    }
}
