#pragma once
// Camera-relative continuity-layer motion. Native precipitation is the game's
// own world-space draw; this only gives the small pre-UI gap-filling layer the
// SAME wind as clouds and water. The shader samples UV - phase, so increasing
// phase moves visible drops in the projected downwind direction.
#include "../Environment/WindSystem.h"
#include <algorithm>
#include <cmath>

namespace renderer
{
struct PrecipitationPhase
{
    float fall = 0.f, windX = 0.f, windY = 0.f;
};

class PrecipitationMotion
{
public:
    // right/up are the horizontal world components of the camera's view
    // basis (columns 0 and 1 of the world->view row-vector matrix). Screen V
    // increases DOWN, hence the minus on the view-up projection.
    void Advance(float dt, float fallSpeed, const WindState& wind,
                 WindVector right, WindVector up, bool viewValid) noexcept
    {
        const double step = double(WindDelta(dt)) *
            std::clamp(WindFinite(fallSpeed, 1.2f), 0.f, 4.f);
        m_fall += step;
        if (!wind.enabled || !viewValid) return;
        const double x = (double(wind.velocity.x) * right.x + double(wind.velocity.y) * right.y) /
            kWindReferenceSpeed;
        const double y = -(double(wind.velocity.x) * up.x + double(wind.velocity.y) * up.y) /
            kWindReferenceSpeed;
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        // The old .35 tilt is now a fixed *visual* scale at reference wind,
        // not another independent direction/strength setting. Integrate the
        // projected displacement, not elapsed session age * current wind:
        // F7/F12 edits and camera turns cannot teleport the drop pattern.
        m_x += step * std::clamp(x, -3.0, 3.0) * .35;
        m_y += step * std::clamp(y, -3.0, 3.0) * .07;
    }
    PrecipitationPhase Phase() const noexcept
    {
        return {float(m_fall), float(m_x), float(m_y)};
    }

private:
    double m_fall = 0, m_x = 0, m_y = 0;
};
}
