#include "CelestialTracker.h"
#include "CelestialStability.h"
#include "CameraContinuity.h"
#include "../Core/FrameContext.h"
#include <wrl/client.h>
#include <cmath>
#include <algorithm>

namespace renderer
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        void UpdateBody(CelestialBody& body, float cx, float cy, float cz,
            const D3DMATRIX& view, const D3DMATRIX& proj, uint64_t frameIndex, bool cameraRelative)
        {
            float len = std::sqrt(cx * cx + cy * cy + cz * cz);
            if (!std::isfinite(len) || len < 1e-4f)
                return;

            Vec3 viewDir{ cx / len, cy / len, cz / len };

            // The sun's billboard carries the camera rotation in its own
            // D3DTS_VIEW, so it can be converted directly. The moon is a
            // camera-relative billboard: its world direction must use a pose
            // accepted for THIS frame, or retain the last confirmed world
            // direction. FrameContext::viewRaw remains populated while stale;
            // using it here would pair today's screen position with yesterday's
            // basis and make DiscEvidence reject the real sky camera at some
            // camera angles.
            const FrameContext& frameCtx = FrameContext::Current();
            Vec3 worldDir = ViewDirectionToWorld(viewDir, view.m);
            bool worldDirectionValid = IsUsableCelestialDirection(worldDir);
            if (cameraRelative)
            {
                const float (*viewRaw)[4] = frameCtx.viewRawValid ? frameCtx.viewRaw.m : nullptr;
                worldDirectionValid = ResolveCameraRelativeWorldDirection(
                    viewDir, viewRaw, frameCtx.viewRawValid && !frameCtx.cameraPoseStale,
                    body.worldSpaceDirection, body.worldDirectionValid, worldDir);
            }

            if (!(proj.m[0][0] > 0.0f) || !(proj.m[1][1] > 0.0f))
                return;
            float screenX = -1.f, screenY = -1.f;
            if (!ProjectViewPosition({cx, cy, cz}, proj.m, screenX, screenY))
                return;

            body.visible = true;
            body.viewSpacePosition = { cx, cy, cz };
            body.viewSpaceDirection = viewDir;
            body.worldSpaceDirection = worldDirectionValid ? worldDir : Vec3{};
            body.worldDirectionValid = worldDirectionValid;
            body.screenX = screenX;
            body.screenY = screenY;
            body.lastSeenFrame = frameIndex;
        }
    }

    CelestialTracker& CelestialTracker::Instance()
    {
        static CelestialTracker instance;
        return instance;
    }

    void CelestialTracker::BeginFrame(uint64_t frameIndex)
    {
        m_frameIndex = frameIndex;
        m_sun.visible = false;
        m_moon.visible = false;
    }

    void CelestialTracker::RefreshWorldDirections(const float viewRaw[16])
    {
        if (!viewRaw || !m_moon.visible)
            return;
        float worldOut[3]{};
        ViewDirectionToWorld(viewRaw, &m_moon.viewSpaceDirection.x, worldOut);
        const Vec3 world{ worldOut[0], worldOut[1], worldOut[2] };
        if (!IsUsableCelestialDirection(world))
            return;
        m_moon.worldSpaceDirection = world;
        m_moon.worldDirectionValid = true;
    }

    void CelestialTracker::Observe(IDirect3DDevice9* device, UINT primitiveCount)
    {
        if (!device || primitiveCount == 0 || primitiveCount > 8)
            return;

        // Both known patterns are fixed-function - this rejects the vast
        // majority of draw calls with just two cheap state queries.
        ComPtr<IDirect3DVertexShader9> vs;
        if (FAILED(device->GetVertexShader(vs.GetAddressOf())) || vs)
            return;
        ComPtr<IDirect3DPixelShader9> ps;
        if (FAILED(device->GetPixelShader(ps.GetAddressOf())) || ps)
            return;

        DWORD alphaTest = 0;
        device->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
        if (!alphaTest)
            return;

        ComPtr<IDirect3DBaseTexture9> texture;
        if (FAILED(device->GetTexture(0, texture.GetAddressOf())) || !texture)
            return;

        // Texture size is the most reliable discriminator found: across
        // every in-game capture, the real sun disc is always ~256x256 and
        // the real moon disc is always ~128x128 - while a consistently
        // present, unrelated companion draw (some other sprite, not the
        // disc itself) is always ~64x64 and otherwise matches every other
        // gate here, including at times a WORLD/VIEW shape close enough to
        // be mistaken for the real thing. z-range alone was not reliable
        // (it drifts with viewing angle and can approach the impostor's
        // range near screen edges); size does not drift.
        UINT texW = 0;
        if (texture->GetType() == D3DRTYPE_TEXTURE)
        {
            D3DSURFACE_DESC desc{};
            if (SUCCEEDED(static_cast<IDirect3DTexture9*>(texture.Get())->GetLevelDesc(0, &desc)))
                texW = desc.Width;
        }
        if (texW == 0)
            return;

        D3DMATRIX world{}, view{}, proj{};
        if (FAILED(device->GetTransform(D3DTS_WORLD, &world))) return;
        if (FAILED(device->GetTransform(D3DTS_VIEW, &view))) return;
        if (FAILED(device->GetTransform(D3DTS_PROJECTION, &proj))) return;

        float worldScaleSq = world.m[0][0] * world.m[0][0] + world.m[0][1] * world.m[0][1] + world.m[0][2] * world.m[0][2];
        // The sun's zero-translation/large-scale epsilons stay slightly
        // generous (0.05 / 0.15) - confirmed by in-game testing to track
        // the sun correctly everywhere on screen with no edge issue.
        bool viewTranslationZero = std::fabs(view.m[3][0]) < 0.05f && std::fabs(view.m[3][1]) < 0.05f && std::fabs(view.m[3][2]) < 0.05f;
        bool worldIsLargeScale = worldScaleSq >= 4.0f;
        bool worldIsIdentityScale = std::fabs(worldScaleSq - 1.0f) < 0.15f;
        float viewTransZ = view.m[3][2];

        // Camera-space position of the billboard's local origin: transform
        // (0,0,0,1) by WORLD then VIEW. For a row-vector convention this is
        // simply the translation row of WORLD carried through VIEW's
        // rotation+translation - equivalent to composing the two matrices
        // and reading the translation row, without needing the full 4x4
        // product.
        const Vec3 cameraPosition = TransformPointRowVector(
            {world.m[3][0], world.m[3][1], world.m[3][2]}, view.m);
        const float cx = cameraPosition.x;
        const float cy = cameraPosition.y;
        const float cz = cameraPosition.z;
        if (cz <= 0.05f)
            return;

        if (viewTranslationZero && worldIsLargeScale && texW >= 200)
        {
            UpdateBody(m_sun, cx, cy, cz, view, proj, m_frameIndex, false);
        }
        else if (!viewTranslationZero && worldIsIdentityScale && viewTransZ > 4.0f && viewTransZ < 16.0f
            && texW >= 96 && texW <= 160)
        {
            // z-range is a loose sanity check now, not the discriminator -
            // near screen edges the real moon's z was observed to drop as
            // low as 8.93 (from ~12 near center), so a tight range isn't
            // safe on its own. The texW filter (96-160, i.e. ~128 and
            // explicitly excluding the ~64 impostor) does the real work.
            UpdateBody(m_moon, cx, cy, cz, view, proj, m_frameIndex, true);
        }
    }
}
