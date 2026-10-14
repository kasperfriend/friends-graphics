#pragma once
// Stable direct-light signal for clouds and the atmosphere.
//
// This header deliberately has no Windows or Direct3D includes (the same
// arrangement as src/Core/DeviceHealth.h) so the rule can be exercised on
// any host by tools/DeviceHealthRegression.cpp.
//
// Why it exists: the sun/moon *disc draw* is a screen-space event. WoW only
// draws the billboard while the disc is inside the frustum, and the tracker
// that detects it can miss individual frames at the screen edge. Feeding
// that binary "disc was drawn this frame" signal straight into cloud and
// fog lighting made the whole sky blink: pan the camera and the clouds lit
// and unlit, and the ground under them shadowed and unshadowed, even though
// the sun itself had not moved or set. A cloud in an off-screen direction
// from the disc is still sunlit, so the *lighting* signal must come from
// the scene's daylight/moonlight state - which is view-independent - and
// only the god-ray/glare *origin* (a screen position) may stay gated on
// the disc actually being on screen.

#include <algorithm>
#include <cmath>
#include "../Core/MathTypes.h"

namespace renderer
{
    // Convert a direction from view space back to world space using the
    // inverse rotation of a row-vector world-to-view matrix. For a rigid
    // D3D view transform, that inverse is the transpose. Keeping this helper
    // beside the host-independent celestial rules makes the matrix convention
    // testable without a D3D device.
    inline Vec3 ViewDirectionToWorld(const Vec3& viewDirection, const float view[4][4]) noexcept
    {
        return Normalize({
            viewDirection.x * view[0][0] + viewDirection.y * view[0][1] + viewDirection.z * view[0][2],
            viewDirection.x * view[1][0] + viewDirection.y * view[1][1] + viewDirection.z * view[1][2],
            viewDirection.x * view[2][0] + viewDirection.y * view[2][1] + viewDirection.z * view[2][2]
        });
    }

    struct CelestialLightInput
    {
        // Deliberately no disc visibility/kind: those depend on the camera,
        // and must never select the world light or turn it on/off.
        // View-independent day/night state captured from the game's own
        // directional light (see CameraCapture). These are gates, not
        // magnitudes: a cool-tinted noon can score a low daylight factor
        // while plainly being day, so they must never scale the light
        // directly - they only decide which body is up and whether any is.
        float daylight = 0.0f;
        float moonlight = 0.0f;
        // Luminance of the game's directional colour. A second, tint-free
        // opinion about "some body is up": the tint-based factors above
        // can read near zero at a cool-tinted noon, and a bright colour
        // must not let those factors reclassify noon as moonlit.
        float directionalLuminance = 0.0f;
        float deltaSeconds = 1.0f / 60.0f;
    };

    struct CelestialLightResult
    {
        // Smoothed 0..1 direct-light strength and the body it belongs to.
        // 1 means "the dominant body is up", exactly what the old binary
        // celestialIntensity meant on a frame where the disc happened to
        // be drawn, so downstream moon-strength scaling is unchanged.
        float intensity = 0.0f;
        bool isMoon = false;
        float targetIntensity = 0.0f;
    };

    class CelestialLightStabilizer
    {
    public:
        CelestialLightResult Update(const CelestialLightInput& input) noexcept
        {
            const float dt = std::clamp(input.deltaSeconds, 0.0f, 0.25f);
            // Pick the dominant body from scene lighting only. The visible
            // disc used to override this: turn from the sun to a daytime
            // moon and the shadow projection jumped to the other body.
            // A small hysteresis band keeps twilight/tint noise from swapping
            // bodies, without depending on which disc the camera has seen.
            bool isMoon = m_isMoon;
            if (!m_primed)
                isMoon = input.moonlight > input.daylight && input.directionalLuminance < 0.5f;
            else if (input.directionalLuminance >= 0.5f || input.daylight > input.moonlight + 0.25f)
                isMoon = false;
            else if (input.moonlight > input.daylight + 0.25f)
                isMoon = true;

            // Magnitudes are still gates, not strength multipliers. Taking
            // either period avoids extinguishing the light inside the body
            // selection's twilight hysteresis band. Bright cool-tinted zones
            // also remain lit even when both tint factors score near zero.
            const float target = (std::max(input.daylight, input.moonlight) > 0.02f ||
                                  input.directionalLuminance > 0.18f) ? 1.0f : 0.0f;

            if (!m_primed)
            {
                m_intensity = target;
                m_primed = true;
            }
            else
            {
                // Exponential approach with a ~0.15 s time constant: a real
                // transition (sunset, walking into a cave) settles in well
                // under a second, but no single frame can step the sky.
                const float alpha = 1.0f - std::exp(-std::max(dt, 0.001f) / 0.15f);
                m_intensity += (target - m_intensity) * alpha;
                if (std::fabs(target - m_intensity) < 0.002f)
                    m_intensity = target;
            }
            m_isMoon = isMoon;
            return { m_intensity, m_isMoon, target };
        }

        void Reset() noexcept
        {
            m_intensity = 0.0f;
            m_isMoon = false;
            m_primed = false;
        }

        bool Primed() const noexcept { return m_primed; }

    private:
        float m_intensity = 0.0f;
        bool m_isMoon = false;
        bool m_primed = false;
    };

    // Only a tracked disc supplies a world-space direction. The old v[24]
    // fallback is camera-dependent; limiting its angular speed merely made
    // a camera turn become a slow sunset (at 2.5 degrees/s it could remove
    // every ground shadow in under a minute). Never integrate that source.
    // Until a body is observed, use a deterministic overhead approximation;
    // afterwards retain its own direction while its billboard is culled.
    class CelestialDirectionStabilizer
    {
    public:
        Vec3 Update(const Vec3* confirmed, bool moon, float dt) noexcept
        {
            const unsigned body = moon ? 1u : 0u;
            if (!confirmed)
                return m_primed[body] ? m_direction[body] : Vec3{0, 0, 1};
            const float length = Length(*confirmed);
            if (!std::isfinite(length) || length < 1e-4f)
                return m_primed[body] ? m_direction[body] : Vec3{0, 0, 1};
            const Vec3 target = *confirmed * (1.f / length);
            if (!m_primed[body])
            {
                m_direction[body] = target;
                m_primed[body] = true;
            }
            else
            {
                // Follow actual celestial movement, including server time
                // changes. A culled disc never supplies a replacement target.
                const float step = std::clamp(dt, 0.f, .25f);
                const float alpha = 1.f - std::exp(-step / .35f);
                const Vec3 blended = m_direction[body] * (1.f - alpha) + target * alpha;
                if (Length(blended) > 1e-4f) m_direction[body] = Normalize(blended);
            }
            return m_direction[body];
        }
        void Reset() noexcept { *this = CelestialDirectionStabilizer{}; }
    private:
        Vec3 m_direction[2]{};
        bool m_primed[2]{};
    };

    // Cloud shadows used to switch fully on/off at a fixed sun elevation,
    // so a disc hovering near the threshold (or a direction source that
    // changed by a degree) blinked the shadow of the whole deck across the
    // world. Fade the strength in over the elevation band instead.
    constexpr float CloudShadowElevationFade(float lightZ) noexcept
    {
        const float t = std::clamp((lightZ - 0.03f) / 0.12f, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
}
