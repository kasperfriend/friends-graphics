#pragma once
// Portable cloud climate/animation policy. No device, camera or random-number
// state: the march, light samples and shadow map consume one world-space field.
#include "../Environment/EnvironmentProfile.h"
#include "../Environment/WindSystem.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace renderer
{
struct CloudShape
{
    float coverage = .50f, density = 1.f, cloudType = 0.f;
    float baseAltitude = 850.f, thickness = 2150.f;
    float featureSize = 3000.f, shapeAspect = 1.3f;
    float detailSize = 240.f, weatherSize = 34000.f, detailStrength = .88f;
    float windSpeed = 26.f, windAngle = 25.f, windShear = .22f;
    float evolutionSpeed = 1.f;
};

// The stale-eye sky fallback has no trustworthy world XY at which to sample
// the world weather map. Keep the zone/native-weather-resolved mean instead
// of inventing a weather-front projection over screen directions.
inline float SkyFallbackCoverage(const CloudShape& shape)
{
    return std::isfinite(shape.coverage) ? std::clamp(shape.coverage, 0.f, 1.f) : 0.f;
}

inline float SkyFallbackCloudType(const CloudShape& shape)
{
    if (!std::isfinite(shape.cloudType)) return .5f;
    return std::clamp((std::clamp(shape.cloudType, -1.f, 1.f) + 1.f) * .5f, 0.f, 1.f);
}

// These are detected native precipitation types, NOT an invented server
// weather forecast. The client currently exposes no verified severity signal.
enum class CloudWeather { Clear = 0, Rain = 1, Snow = 2, Sand = 3 };

inline CloudShape ResolveCloudClimate(CloudShape s, const ResolvedEnvironmentState& env, bool zones)
{
    if (zones)
    {
        s.coverage *= env[CloudCoverage];
        s.cloudType += env[CloudType];
        s.featureSize *= env[CloudFeatureSize];
        s.thickness *= env[CloudThickness];
        s.detailStrength *= env[CloudDetail];
        s.evolutionSpeed *= env[CloudEvolution];
    }
    s.coverage = std::clamp(s.coverage, 0.f, 1.f);
    s.cloudType = std::clamp(s.cloudType, -1.f, 1.f);
    s.featureSize = std::clamp(s.featureSize, 600.f, 40000.f);
    s.thickness = std::clamp(s.thickness, 100.f, 12000.f);
    s.detailStrength = std::clamp(s.detailStrength, 0.f, .9f);
    s.evolutionSpeed = std::clamp(s.evolutionSpeed, 0.f, 4.f);
    return s;
}

struct CloudMotion
{
    // Normalised, wrapped texture offsets. Kept in double precision on CPU;
    // converted only when uploading shader constants. Changing wind or tile
    // size changes future velocity, never re-multiplies the session's age.
    std::array<double, 2> shape{}, detail{}, weather{};
    double shapeZ = 0, detailZ = 0, cycle = 0;
    float variation = 0;

    static double Wrap(double v) { return v - std::floor(v); }
    void Advance(const CloudShape& s, float dt)
    {
        const double angle = s.windAngle * .017453292519943;
        const double evolution = s.evolutionSpeed * dt;
        const double velocity[2] = {std::cos(angle) * s.windSpeed, std::sin(angle) * s.windSpeed};
        for (int i = 0; i < 2; ++i)
        {
            shape[i] = Wrap(shape[i] + velocity[i] * dt / std::max(s.featureSize, 1.f));
            detail[i] = Wrap(detail[i] + velocity[i] * dt * 1.7 / std::max(s.detailSize, 1.f));
            weather[i] = Wrap(weather[i] + velocity[i] * dt * .35 / std::max(s.weatherSize, 1.f)
                              + evolution * (i ? -1.0 / 9900.0 : 1.0 / 7200.0));
        }
        // Different noise slices reshape the bodies/edges even with zero
        // wind. The analytic world-height profile itself never moves.
        shapeZ = Wrap(shapeZ + evolution * .00045);
        detailZ = Wrap(detailZ - evolution * .0013);
        cycle = Wrap(cycle + evolution / 900.0);
        // Use the existing weather-map B channel to gently open/close local
        // patches, not a global random coverage jump. Full overcast stays shut.
        variation = float(std::sin(cycle * 6.283185307179586)) * .12f * (1.f - s.coverage);
    }
};

// Both shader density paths sample p + offset. Their sampling velocity and
// height shear must be NEGATIVE the wind's travel vector, or clouds/shadows
// would head upwind while water heads downwind. WindAngle is legacy sampling
// orientation only; the shared system owns direction whenever it is linked.
inline CloudShape ApplyCloudWind(CloudShape shape, const WindState& wind)
{
    if (!wind.DrivesClouds()) return shape;
    const float strength = std::clamp(WindFinite(wind.strength, 0.f), 0.f, 3.f);
    shape.windSpeed *= strength;
    shape.windShear *= strength;
    const double length = std::hypot(double(wind.direction.x), double(wind.direction.y));
    if (std::isfinite(length) && length > .01)
        shape.windAngle = float(std::atan2(-double(wind.direction.y), -double(wind.direction.x)) * 180.0 / kWindPi);
    return shape;
}

class CloudDynamics
{
public:
    CloudShape Update(const CloudShape& authored, const ResolvedEnvironmentState& env,
                      bool zones, CloudWeather weather, float strength,
                      float transitionSeconds, float dt, const WindState& wind = {})
    {
        dt = std::isfinite(dt) ? std::clamp(dt, 0.f, .1f) : 0.f;
        const CloudShape target = ResolveCloudClimate(authored, env, zones);
        const float alpha = transitionSeconds <= 0.f ? 1.f : Response(dt, transitionSeconds);
        if (!m_primed) m_current = target;
        // Filter only the physical field, never quality/debug/enabled flags.
        // LocationGraphics edits and rapid border reversals start from what
        // is visible now. No reset on INI reload or Direct3D device recovery.
        for (auto member : {&CloudShape::coverage, &CloudShape::density, &CloudShape::cloudType,
             &CloudShape::baseAltitude, &CloudShape::thickness, &CloudShape::featureSize,
             &CloudShape::shapeAspect, &CloudShape::detailSize, &CloudShape::weatherSize,
             &CloudShape::detailStrength, &CloudShape::windSpeed, &CloudShape::windShear,
             &CloudShape::evolutionSpeed})
        {
            m_current.*member += (target.*member - m_current.*member) * alpha;
            if (std::abs(target.*member - m_current.*member) < 1e-5f * std::max(1.f, std::abs(target.*member)))
                m_current.*member = target.*member;
        }
        m_current.windAngle = std::remainder(m_current.windAngle +
            std::remainder(target.windAngle - m_current.windAngle, 360.f) * alpha, 360.f);

        strength = std::isfinite(strength) ? std::clamp(strength, 0.f, 1.f) : 0.f;
        for (int i = 0; i < 3; ++i)
        {
            const float goal = int(weather) == i + 1 ? strength : 0.f;
            if (!m_primed) m_wet[i] = goal;
            // Use one response for the whole vector so rain->snow cannot
            // temporarily exceed unit weight. Clearing is deliberately slower.
            const float seconds = weather == CloudWeather::Clear || strength == 0.f ? 35.f : 18.f;
            m_wet[i] += (goal - m_wet[i]) * Response(dt, seconds);
            if (std::abs(goal - m_wet[i]) < 1e-5f) m_wet[i] = goal;
        }
        m_primed = true;
        CloudShape result = m_current;
        const float wet = std::clamp(m_wet[0] + m_wet[1] + m_wet[2], 0.f, 1.f);
        result.coverage += (1.f - result.coverage) * wet;
        // Rain builds deep billows; snow a broad, low-contrast stratus deck;
        // sand a flatter dusty lid. Weather beats a zone's fair-sky baseline.
        result.cloudType = result.cloudType * (1.f - wet) +
                           .8f * m_wet[0] - .8f * m_wet[1] - .65f * m_wet[2];
        result.density *= 1.f + .4f * m_wet[0] + .15f * m_wet[1];
        result.thickness = std::clamp(result.thickness * (1.f + .4f * m_wet[0]), 100.f, 12000.f);
        result.featureSize = std::clamp(result.featureSize * (1.f + .35f * wet), 600.f, 40000.f);
        // Keep a solid core even in highly eroded/small-cloud desert profiles.
        result.detailStrength *= 1.f - .35f * wet;
        result = ApplyCloudWind(result, wind);
        m_motion.Advance(result, dt);
        return result;
    }

    // A confirmed zone change is a weather discontinuity, not a weather trend: the
    // precipitation the client was emitting came from a place the player has left, and
    // the wet vector it fed forced the deck's coverage to 1. Carrying it across the
    // hold (30 s of "no new evidence" grace) and then the slow clearing ramp left a
    // full overcast hanging over a clear zone after a fast teleport - the reported
    // "stuck covering the whole sky, no transition between locations". The new zone's
    // own particles set it again within a frame or two, and the climate filter below
    // still eases the field itself, so the sky changes but does not pop.
    void ForgetWeather() noexcept { m_wet = {0.f, 0.f, 0.f}; }

    const CloudMotion& Motion() const { return m_motion; }
    float WeatherAmount() const { return m_wet[0] + m_wet[1] + m_wet[2]; }

private:
    // Seconds to approximately 95% of a new target, independent of framerate.
    static float Response(float dt, float seconds) { return -std::expm1(-3.f * dt / seconds); }
    bool m_primed = false;
    CloudShape m_current;
    std::array<float, 3> m_wet{};
    CloudMotion m_motion;
};
}
