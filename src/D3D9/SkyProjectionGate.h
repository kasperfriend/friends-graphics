#pragma once

#include <cmath>

namespace renderer::skyprojection
{
    // Some clients leave a non-perspective sky/UI matrix in D3DTS_PROJECTION
    // while the full-frame shader camera continues to carry the scene. Never
    // use that matrix's depth terms. Reuse only a perspective already validated
    // from a full-frame camera capture, for the same full-screen rectangle, and
    // only while that projection cache is recent. The captured viewport's depth
    // range remains paired with its projection for scene-depth reconstruction.
    inline bool CanReuseValidatedPerspective(bool cacheValid, bool sameScreenViewport,
                                             float cacheAgeMs, float maxAgeMs) noexcept
    {
        return cacheValid && sameScreenViewport &&
            std::isfinite(cacheAgeMs) && std::isfinite(maxAgeMs) &&
            cacheAgeMs >= 0.f && maxAgeMs >= 0.f && cacheAgeMs <= maxAgeMs;
    }

    // A full-frame draw whose depth projection is a different shader transform
    // may still carry the live camera VIEW. It can keep a previously validated
    // projection current only when the screen rectangle and both focal scales
    // agree; the projection's depth terms and viewport range continue to come
    // from the validated cache, never from the unrelated draw.
    inline bool MatchesValidatedPerspectiveScales(bool cacheValid, bool sameScreenViewport,
                                                   float cachedP00, float cachedP11,
                                                   float observedP00, float observedP11,
                                                   float relativeTolerance = .005f) noexcept
    {
        if (!cacheValid || !sameScreenViewport ||
            !std::isfinite(cachedP00) || !std::isfinite(cachedP11) ||
            !std::isfinite(observedP00) || !std::isfinite(observedP11) ||
            !std::isfinite(relativeTolerance) || relativeTolerance < 0.f ||
            cachedP00 <= 0.f || cachedP11 <= 0.f ||
            observedP00 <= 0.f || observedP11 <= 0.f)
            return false;
        float toleranceX = std::fabs(cachedP00) * relativeTolerance;
        float toleranceY = std::fabs(cachedP11) * relativeTolerance;
        if (toleranceX < 1e-4f) toleranceX = 1e-4f;
        if (toleranceY < 1e-4f) toleranceY = 1e-4f;
        return std::fabs(observedP00 - cachedP00) <= toleranceX &&
               std::fabs(observedP11 - cachedP11) <= toleranceY;
    }
}
