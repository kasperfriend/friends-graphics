#pragma once

#include <algorithm>
#include <cmath>

namespace renderer::skyprojection
{
    // A current D3DTS_PROJECTION can be an orthographic/UI matrix even while
    // D3DTS_VIEW still carries the scene sky's current rotation. Reuse a known
    // perspective projection for disc corroboration only when both lateral
    // scales match the last camera projection; a different FOV/aspect must not
    // lend its screen-space evidence to this transform.
    inline bool MatchesPerspectiveScale(float currentX, float currentY,
                                        float cameraX, float cameraY) noexcept
    {
        if (!std::isfinite(currentX) || !std::isfinite(currentY) ||
            !std::isfinite(cameraX) || !std::isfinite(cameraY) ||
            currentX <= 0.f || currentY <= 0.f || cameraX <= 0.f || cameraY <= 0.f)
            return false;
        const float toleranceX = std::max(.001f, std::fabs(cameraX) * .005f);
        const float toleranceY = std::max(.001f, std::fabs(cameraY) * .005f);
        return std::fabs(currentX - cameraX) <= toleranceX &&
               std::fabs(currentY - cameraY) <= toleranceY;
    }
}
