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
}
