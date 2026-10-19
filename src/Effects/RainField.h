#pragma once
// Layered camera-relative rain particles. WindState.velocity is already
// DOWNWIND in WoW world XY; project it once through the captured view basis.
// Motion accumulates in double precision so long sessions and tuning edits do
// not multiply elapsed time by a new heading or lose subpixel movement.
#include "../Environment/WindSystem.h"
#include "PrecipitationMotion.h" // SplitPhase: exact wrapped lattice phases
#include <algorithm>
#include <cmath>

namespace renderer::rainfield {
inline constexpr float cellWidth[3] = {60.f, 34.f, 22.f}; // pixels at 1080p
inline constexpr float cellHeight[3] = {88.f, 58.f, 38.f};
inline constexpr float layerSpeed[3] = {1.10f, .86f, .64f};
inline constexpr float layerDepth[3] = {4.f, 11.f, 25.f};
inline constexpr float latticePeriod = 4096.f;
// How much of a layer's own screen-space fall a camera translation may take
// away before the parallax stops. A camera that descends (swimming down, a
// mount losing height, a fall) moves world-anchored drops UP the screen, and at
// the nearest layer's 4 yards it does so several times faster than the drops
// fall: the whole field climbed, and the near streaks, whose bright head sits
// on their direction axis, pointed up. The camera may slow the rain down -
// that is the depth cue - but it may never reverse it.
inline constexpr float kMinimumCameraFallShare = .35f;

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

// A camera translation displaces nearby rain more than distant rain. This is
// only an approximation of world anchoring: do not reconstruct an unbounded
// world grid in ps_3_0 or apply session-age * current camera direction.
struct Camera
{
    double position[3]{};
    float right[3]{}, up[3]{}; // captured world-to-view columns
    float projectionX = 0.f, projectionY = 0.f, aspect = 1.f;
    bool valid = false;
};

class Motion {
public:
    void Advance(float dt, float fallSpeed, Projection wind) noexcept
    {
        const double step = WindDelta(dt);
        m_x += step * WindFinite(wind.x, 0.f);
        m_y += step * VerticalSpeed(fallSpeed, wind);
        // Remembered for TrackCamera's clamp: a camera translation has to be
        // measured against the rate these drops are already falling at.
        m_fallSpeed = std::clamp(WindFinite(fallSpeed, 1.f), .1f, 4.f);
        m_wind = {WindFinite(wind.x, 0.f), WindFinite(wind.y, 0.f)};
    }
    void TrackCamera(const Camera& camera, float amount = .7f, float dt = 1.f/60.f) noexcept
    {
        bool valid = camera.valid && std::isfinite(camera.projectionX) &&
            std::isfinite(camera.projectionY) && std::isfinite(camera.aspect) &&
            camera.projectionX > .01f && camera.projectionX < 10.f &&
            camera.projectionY > .01f && camera.projectionY < 10.f &&
            camera.aspect > .3f && camera.aspect < 6.f;
        for (unsigned axis = 0; axis < 3; ++axis)
            valid = valid && std::isfinite(camera.position[axis]) &&
                std::isfinite(camera.right[axis]) && std::isfinite(camera.up[axis]);
        if (!valid) { ForgetCamera(); return; }
        if (!m_haveCamera) { m_previous = camera; m_haveCamera = true; return; }
        double delta[3]{};
        double distanceSquared = 0;
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            delta[axis] = camera.position[axis] - m_previous.position[axis];
            distanceSquared += delta[axis] * delta[axis];
        }
        m_previous = camera;
        // A missing capture or teleport rebases without replaying queued
        // camera movement; wind/gravity keep their independent phases.
        if (distanceSquared > 6.25) { ForgetCamera(); m_previous = camera; m_haveCamera = true; return; }
        const double gain = std::clamp(WindFinite(amount, .7f), 0.f, 1.5f);
        if (gain == 0) { CancelPendingCameraMotion(); return; }
        double projectedRight = 0, projectedUp = 0;
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            projectedRight += delta[axis] * camera.right[axis];
            projectedUp += delta[axis] * camera.up[axis];
        }
        const double x = -540.0 * camera.aspect * camera.projectionX * projectedRight * gain;
        const double y =  540.0 * camera.projectionY * projectedUp * gain;
        const double seconds = double(WindDelta(dt));
        const double positionBlend = -std::expm1(-seconds / .045);
        const double directionBlend = -std::expm1(-seconds / .07);
        // Pixels per second this frame's rain falls by its own gravity, before
        // the camera is allowed to spend any of it on parallax.
        const double fallRate = double(VerticalSpeed(m_fallSpeed, m_wind));
        for (unsigned i = 0; i < 3; ++i)
        {
            // Clamp each newly observed displacement, then ease its target
            // into the rendered phase on EVERY frame. An irregular camera
            // capture no longer moves the whole pattern in discrete jumps.
            // The vertical share is additionally floored: a descending camera
            // contributes y < 0 (screen up), and without the floor the near
            // layer climbed faster than the rain fell - the reported "rain
            // goes up when the camera moves down". The floor is each layer's
            // own fall scaled by kMinimumCameraFallShare, so the same rule
            // holds for all three depths; a camera moving up is never limited.
            const double verticalFall = fallRate * layerSpeed[i];
            const double floorY = std::max(-verticalFall * kMinimumCameraFallShare * seconds, -32.0);
            m_targetX[i] += std::clamp(x / layerDepth[i], -32.0, 32.0);
            m_targetY[i] += std::clamp(y / layerDepth[i], floorY, 32.0);
            const double stepX = (m_targetX[i] - m_cameraX[i]) * positionBlend;
            const double stepY = (m_targetY[i] - m_cameraY[i]) * positionBlend;
            m_cameraX[i] += stepX;
            m_cameraY[i] += stepY;
            const float velocityX = seconds > 0 ? float(stepX / seconds) : 0.f;
            const float velocityY = seconds > 0 ? float(stepY / seconds) : 0.f;
            m_cameraVelocityX[i] += (velocityX - m_cameraVelocityX[i]) * float(directionBlend);
            m_cameraVelocityY[i] += (velocityY - m_cameraVelocityY[i]) * float(directionBlend);
        }
    }
    void ForgetCamera() noexcept
    {
        m_haveCamera = false;
        CancelPendingCameraMotion();
    }
    Projection Direction(unsigned i, Projection wind, float fallSpeed) const noexcept
    {
        i = std::min(i, 2u);
        const float x = wind.x * layerSpeed[i] + m_cameraVelocityX[i];
        const float y = VerticalSpeed(fallSpeed, wind) * layerSpeed[i] + m_cameraVelocityY[i];
        const float length = std::hypot(x,y);
        return length > .001f ? Projection{x/length,y/length} : Projection{0.f,1.f};
    }

    // Shader receives (fraction, wrapped integer) for each axis. No large
    // phase is sent to float32; the integer only offsets the particle hash.
    void Layer(unsigned i, float out[4]) const noexcept
    {
        i = std::min(i, 2u);
        SplitPhase((m_x * layerSpeed[i] + m_cameraX[i]) / cellWidth[i], latticePeriod, out[0], out[1]);
        SplitPhase((m_y * layerSpeed[i] + m_cameraY[i]) / cellHeight[i], latticePeriod, out[2], out[3]);
    }
    double X() const noexcept { return m_x; }
    double Y() const noexcept { return m_y; }
private:
    double m_x = 0, m_y = 0;
    // The fall speed and projected wind last integrated, so the camera clamp
    // measures a translation against the drops' actual screen fall.
    float m_fallSpeed = 1.f;
    Projection m_wind{};
    void CancelPendingCameraMotion() noexcept
    {
        for (unsigned i = 0; i < 3; ++i)
        {
            m_targetX[i] = m_cameraX[i];
            m_targetY[i] = m_cameraY[i];
            m_cameraVelocityX[i] = m_cameraVelocityY[i] = 0;
        }
    }
    double m_cameraX[3]{}, m_cameraY[3]{};
    double m_targetX[3]{}, m_targetY[3]{};
    float m_cameraVelocityX[3]{}, m_cameraVelocityY[3]{};
    Camera m_previous{};
    bool m_haveCamera = false;
};
}
