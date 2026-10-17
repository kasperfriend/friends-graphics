#pragma once
// Screen-facing, camera-relative rain particles. WindState.velocity is already
// DOWNWIND in WoW world XY; project it once through the captured view basis.
// Motion accumulates in double precision so long sessions and tuning edits do
// not multiply elapsed time by a new heading or lose subpixel movement.
#include "../Environment/WindSystem.h"
#include "PrecipitationMotion.h" // SplitPhase: exact wrapped lattice phases
#include <algorithm>
#include <cmath>

namespace renderer::rainfield {
inline constexpr float cellWidth[3] = {38.f, 28.f, 22.f}; // pixels at 1080p
inline constexpr float cellHeight[3] = {60.f, 48.f, 38.f};
inline constexpr float layerSpeed[3] = {1.15f, .86f, .64f};
inline constexpr float latticePeriod = 4096.f;

struct Projection { float x = 0.f, y = 0.f; }; // screen-right, screen-down, in reference pixels/sec
inline Projection Project(const WindState& wind, WindVector right, WindVector up,
                          bool viewValid, float influence) noexcept
{
    if (!wind.enabled || !viewValid) return {};
    const float scale = 140.f / kWindReferenceSpeed *
        std::clamp(WindFinite(influence, 1.f), 0.f, 2.f);
    const float x = (wind.velocity.x * right.x + wind.velocity.y * right.y) * scale;
    const float y = -(wind.velocity.x * up.x + wind.velocity.y * up.y) * scale;
    return {std::clamp(WindFinite(x, 0.f), -840.f, 840.f),
            std::clamp(WindFinite(y, 0.f), -840.f, 840.f)};
}

inline float VerticalSpeed(float fallSpeed, Projection wind) noexcept
{
    const float gravity = 620.f * std::clamp(WindFinite(fallSpeed, 1.f), .1f, 4.f);
    // A horizontal world wind may project upward in a steep camera view,
    // but cannot reverse gravity's screen-facing fall.
    return std::max(gravity * .25f, gravity + WindFinite(wind.y, 0.f));
}

class Motion {
public:
    void Advance(float dt, float fallSpeed, Projection wind) noexcept
    {
        const double step = WindDelta(dt);
        m_x += step * WindFinite(wind.x, 0.f);
        m_y += step * VerticalSpeed(fallSpeed, wind);
    }
    // Shader receives (fraction, wrapped integer) for each axis. No large
    // phase is sent to float32; the integer only offsets the particle hash.
    void Layer(unsigned i, float out[4]) const noexcept
    {
        i = std::min(i, 2u);
        SplitPhase(m_x * layerSpeed[i] / cellWidth[i], latticePeriod, out[0], out[1]);
        SplitPhase(m_y * layerSpeed[i] / cellHeight[i], latticePeriod, out[2], out[3]);
    }
    double X() const noexcept { return m_x; }
    double Y() const noexcept { return m_y; }
private:
    double m_x = 0, m_y = 0;
};
}
