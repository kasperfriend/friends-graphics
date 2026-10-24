#pragma once

#include <cmath>

namespace renderer::skyprojection
{
    // Some clients leave a non-perspective sky/UI matrix in D3DTS_PROJECTION
    // while the full-frame shader camera continues to carry the scene. Never
    // use that matrix's depth terms. Reuse only a perspective already validated
    // from a full-frame camera capture, for the same viewport, and only while
    // that projection cache is recent. Disc evidence is evaluated with this
    // cached perspective and the current rigid VIEW candidate.
    inline bool CanReuseValidatedPerspective(bool cacheValid, bool sameViewport,
                                             float cacheAgeMs, float maxAgeMs) noexcept
    {
        return cacheValid && sameViewport &&
            std::isfinite(cacheAgeMs) && std::isfinite(maxAgeMs) &&
            cacheAgeMs >= 0.f && maxAgeMs >= 0.f && cacheAgeMs <= maxAgeMs;
    }
}
