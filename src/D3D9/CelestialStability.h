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
    struct CelestialLightInput
    {
        // The tracker confirmed the disc draw this frame, and which body it was.
        bool discVisible = false;
        bool discIsMoon = false;
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
            m_secondsSinceDisc += dt;

            bool isMoon;
            float target;
            if (input.discVisible)
            {
                m_secondsSinceDisc = 0.0f;
                m_discWasMoon = input.discIsMoon;
                m_hasDisc = true;
                isMoon = input.discIsMoon;
                target = 1.0f;
            }
            else
            {
                // A one-frame (or few-frame) tracker dropout keeps the body
                // it just confirmed: sun and moon can both be drawn on the
                // same evening, and a dropout must not flip the light to
                // the other body and back. Longer term the confirmed body
                // is sticky too - a kind flip would step the downstream
                // moon-strength scaling in full view of the player - and
                // only a clear, dim (i.e. not bright-noon) opposite period
                // takes over from it.
                if (m_secondsSinceDisc < kDiscHoldSeconds)
                {
                    isMoon = m_discWasMoon;
                }
                else if (m_hasDisc)
                {
                    const float current = m_discWasMoon ? input.moonlight : input.daylight;
                    const float opposite = m_discWasMoon ? input.daylight : input.moonlight;
                    isMoon = m_discWasMoon;
                    if (opposite > current + 0.25f && input.directionalLuminance < 0.5f)
                    {
                        isMoon = !m_discWasMoon;
                        m_discWasMoon = isMoon;
                    }
                }
                else
                {
                    isMoon = input.moonlight > input.daylight;
                }
                const float bodyFactor = isMoon ? input.moonlight : input.daylight;
                // Still daytime/night-time for that body: the light stays
                // fully on even with the disc off screen or behind a cloud
                // bank. Only when the body's own period has ended (and the
                // short disc hold has expired) does the light target off.
                target = (bodyFactor > 0.02f || m_secondsSinceDisc < kDiscHoldSeconds ||
                          input.directionalLuminance > 0.18f)
                    ? 1.0f : 0.0f;
            }

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
            m_discWasMoon = false;
            m_hasDisc = false;
            m_secondsSinceDisc = 1e6f;
        }

        bool Primed() const noexcept { return m_primed; }

    private:
        static constexpr float kDiscHoldSeconds = 1.5f;
        float m_intensity = 0.0f;
        bool m_isMoon = false;
        bool m_primed = false;
        bool m_discWasMoon = false;
        bool m_hasDisc = false;
        float m_secondsSinceDisc = 1e6f;
    };

    // Keep separate world-space directions for the sun and moon. Once a
    // billboard confirms a body, camera-derived fallback constants must not
    // replace it merely because the camera turned away from the billboard.
    class CelestialDirectionStabilizer
    {
    public:
        Vec3 Update(Vec3 fallback, const Vec3* confirmed, bool moon, float dt) noexcept
        {
            const unsigned body = moon ? 1u : 0u;
            Vec3 target = confirmed ? *confirmed : fallback;
            const float length = Length(target);
            if (!std::isfinite(length) || length < 1e-4f)
                return m_primed[body] ? m_direction[body] : Vec3{0, 0, 1};
            target = target * (1.f / length);
            if (!m_primed[body])
            {
                m_direction[body] = target;
                m_primed[body] = true;
            }
            else if (confirmed || !m_confirmed[body])
            {
                const float alpha = 1.f - std::exp(-std::clamp(dt, 0.f, .25f) / .35f);
                const Vec3 blended = m_direction[body] * (1.f - alpha) + target * alpha;
                if (Length(blended) > 1e-4f) m_direction[body] = Normalize(blended);
            }
            if (confirmed) m_confirmed[body] = true;
            return m_direction[body];
        }
        void Reset() noexcept { *this = CelestialDirectionStabilizer{}; }
    private:
        Vec3 m_direction[2]{};
        bool m_primed[2]{};
        bool m_confirmed[2]{};
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
