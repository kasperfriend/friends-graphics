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
#include <cstdint>
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

    inline bool IsUsableCelestialDirection(const Vec3& direction) noexcept
    {
        const float length = Length(direction);
        return std::isfinite(direction.x) && std::isfinite(direction.y) && std::isfinite(direction.z) &&
            std::isfinite(length) && length > 0.25f;
    }

    // The moon is a camera-relative billboard. Its world direction may only be
    // recomputed with a camera pose accepted for THIS frame; FrameContext keeps
    // the last raw view matrix around while the current pose is stale. In that
    // case the last independently confirmed celestial direction is better
    // evidence than multiplying the billboard by yesterday's camera rotation.
    // With no confirmed direction yet, fail closed so sky-pose selection falls
    // back to camera continuity instead of comparing a pose with stale data.
    inline bool ResolveCameraRelativeWorldDirection(
        const Vec3& viewDirection, const float view[4][4], bool cameraPoseFresh,
        const Vec3& retainedWorldDirection, bool retainedDirectionValid,
        Vec3& worldDirection) noexcept
    {
        if (cameraPoseFresh && view)
        {
            const Vec3 current = ViewDirectionToWorld(viewDirection, view);
            if (IsUsableCelestialDirection(current))
            {
                worldDirection = current;
                return true;
            }
        }
        if (retainedDirectionValid && IsUsableCelestialDirection(retainedWorldDirection))
        {
            worldDirection = Normalize(retainedWorldDirection);
            return true;
        }
        worldDirection = {};
        return false;
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

    enum class VisibleCelestialSource : std::uint8_t { None, Sun, Moon };

    // Pick the screen-space glare origin using the already-stabilized scene
    // light, not the tint-only day/night estimate. A cool, bright noon can
    // make moonlightFactor exceed daylightFactor even while the sun is plainly
    // the dominant source; following that weaker estimate pins the halo to the
    // moon and creates a second pale "sun" elsewhere in the sky.
    inline VisibleCelestialSource ChooseVisibleCelestialSource(
        bool sunVisible, bool moonVisible, bool moonDominant) noexcept
    {
        if (moonVisible && (moonDominant || !sunVisible))
            return VisibleCelestialSource::Moon;
        if (sunVisible)
            return VisibleCelestialSource::Sun;
        return VisibleCelestialSource::None;
    }

    // Project a view-space point with the actual row-vector D3D projection
    // matrix, including off-centre terms. Most WoW captures are symmetric, for
    // which this reduces to the familiar P00/P11 formula; using the complete
    // matrix keeps a tracked disc on the client-painted pixel if the viewport
    // projection is shifted or otherwise not perfectly symmetric.
    inline bool ProjectViewPosition(const Vec3& point, const float projection[4][4],
                                   float& screenX, float& screenY) noexcept
    {
        const float clipX = point.x * projection[0][0] + point.y * projection[1][0] +
                            point.z * projection[2][0] + projection[3][0];
        const float clipY = point.x * projection[0][1] + point.y * projection[1][1] +
                            point.z * projection[2][1] + projection[3][1];
        const float clipW = point.x * projection[0][3] + point.y * projection[1][3] +
                            point.z * projection[2][3] + projection[3][3];
        if (!std::isfinite(clipX) || !std::isfinite(clipY) ||
            !std::isfinite(clipW) || clipW <= 1e-4f)
            return false;
        screenX = .5f + .5f * clipX / clipW;
        screenY = .5f - .5f * clipY / clipW;
        return std::isfinite(screenX) && std::isfinite(screenY);
    }

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
    // the first real track eases into that approximation instead of making
    // the world light jump on the exact frame its billboard enters the view.
    // Once confirmed, retain that body's direction while its billboard is culled.
    class CelestialDirectionStabilizer
    {
    public:
        Vec3 Update(const Vec3* confirmed, bool moon, float dt) noexcept
        {
            const unsigned body = moon ? 1u : 0u;
            if (!m_initialized[body])
            {
                m_direction[body] = {0, 0, 1};
                m_initialized[body] = true;
            }
            if (!confirmed)
                return m_direction[body];
            const float length = Length(*confirmed);
            if (!std::isfinite(length) || length < 1e-4f)
                return m_direction[body];

            const Vec3 target = *confirmed * (1.f / length);
            // A newly observed billboard is real direction evidence, but its
            // first frame is also when the old overhead approximation was
            // replaced abruptly. Use the same bounded, view-independent
            // response for acquisition and subsequent server-time motion.
            const float step = std::clamp(dt, 0.f, .25f);
            const float alpha = 1.f - std::exp(-step / .35f);
            const Vec3 blended = m_direction[body] * (1.f - alpha) + target * alpha;
            if (Length(blended) > 1e-4f) m_direction[body] = Normalize(blended);
            m_confirmed[body] = true;
            return m_direction[body];
        }

        bool HasConfirmed(bool moon) const noexcept { return m_confirmed[moon ? 1u : 0u]; }
        void Reset() noexcept { *this = CelestialDirectionStabilizer{}; }

    private:
        Vec3 m_direction[2]{{0, 0, 1}, {0, 0, 1}};
        bool m_initialized[2]{};
        bool m_confirmed[2]{};
    };

    // ---- which body's direction a consumer inside the draw stream takes ----------
    //
    // `CelestialBody::visible` answers "did the client paint that billboard THIS
    // frame", which is a frustum event and not a lighting fact: pitch the camera up
    // past the horizon and a low sun leaves the bottom of the frame, its draw stops,
    // and the flag drops while the sun itself has neither moved nor set. Water's
    // per-draw light direction was gated on exactly that flag, so one frame of camera
    // pitch handed every term that reads the light over to whatever the client's own
    // constant carries that frame - the sun road, the reflected disc, the sky gradient
    // and the in-scatter's elevation gate all went at once, which is the report "the
    // water loses a lot of brightness when I look up above the horizon".
    //
    // A direction is a WORLD fact, so the choice below is made from the scene's own
    // day/night state - view independent, the same dominance the compositor's glare
    // origin uses - and a body that was confirmed once keeps supplying its direction
    // while its billboard is culled, which is precisely the contract
    // CelestialDirectionStabilizer gives the clouds, the god rays and the shadows.
    struct CelestialTrack
    {
        bool drawn = false;  // the billboard was painted this frame
        bool known = false;  // a disc has confirmed this body's world direction at some point
        Vec3 world{ 0.0f, 0.0f, 1.0f };
    };

    // Returns 0 when nothing has ever been confirmed - the caller must then keep its
    // own fallback source rather than invent a light - 1 for the sun, 2 for the moon.
    inline int ChooseCelestialTrack(const CelestialTrack& sun, const CelestialTrack& moon,
                                    float daylight, float moonlight) noexcept
    {
        const auto usable = [](const CelestialTrack& body) noexcept {
            if (!body.known || !std::isfinite(body.world.x) || !std::isfinite(body.world.y) ||
                !std::isfinite(body.world.z))
                return false;
            const float length = Length(body.world);
            return std::isfinite(length) && length > 1e-4f;
        };
        // The body the scene's own lighting says is up, never the body the camera
        // happens to see: a daytime moon inside the frustum must not take the light
        // from the sun, which is the mistake the compositor's own glare choice made
        // before it was given this dominance test.
        const bool night = moonlight > daylight;
        if (usable(night ? moon : sun))
            return night ? 2 : 1;
        // The dominant body has never been confirmed. A disc painted THIS frame is
        // live evidence of a real direction and beats a client constant; a body that
        // was never seen at all supplies nothing, and the caller keeps its own source.
        const CelestialTrack& other = night ? sun : moon;
        if (other.drawn && usable(other))
            return night ? 1 : 2;
        return 0;
    }

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
