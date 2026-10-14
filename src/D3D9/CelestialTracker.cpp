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

            // World-space direction: VIEW is a row-vector world->view
            // transform, so invert its rotation by multiplying with the
            // transpose. For the camera-relative moon billboard, the draw's
            // own D3DTS_VIEW has an identity 3x3 rotation and carries only the
            // view-space translation, so invert against the captured scene
            // camera (FrameContext::viewRaw) whenever available.
            Vec3 worldDir = ViewDirectionToWorld(viewDir, view.m);
            const FrameContext& frameCtx = FrameContext::Current();
            if (cameraRelative && frameCtx.viewRawValid)
            {
                float worldOut[3]{ worldDir.x, worldDir.y, worldDir.z };
                ViewDirectionToWorld(&frameCtx.viewRaw.m[0][0], &viewDir.x, worldOut);
                worldDir = { worldOut[0], worldOut[1], worldOut[2] };
            }

            float p00 = proj.m[0][0], p11 = proj.m[1][1];
            if (!(p00 > 0.0f) || !(p11 > 0.0f))
                return;

            body.visible = true;
            body.viewSpaceDirection = viewDir;
            body.worldSpaceDirection = worldDir;
            body.screenX = 0.5f + 0.5f * (cx / cz) * p00;
            body.screenY = 0.5f - 0.5f * (cy / cz) * p11;
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
        float worldOut[3]{ m_moon.worldSpaceDirection.x, m_moon.worldSpaceDirection.y, m_moon.worldSpaceDirection.z };
        ViewDirectionToWorld(viewRaw, &m_moon.viewSpaceDirection.x, worldOut);
        m_moon.worldSpaceDirection = { worldOut[0], worldOut[1], worldOut[2] };
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
