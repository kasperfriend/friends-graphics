#pragma once
// Device-free, render-thread wind simulation. One update at the frame boundary,
// many read-only consumers (clouds/water now; vegetation can use the same state).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace renderer
{
inline constexpr double kWindPi = 3.14159265358979323846;
inline constexpr float kWindReferenceSpeed = 12.f; // world yards/second at strength 1

// WoW world axes: +X north, +Y west, +Z up. Names describe travel TOWARD,
// not the meteorological "coming from" convention. Never use screen axes.
enum class WindDirection : unsigned { North, NorthEast, East, SouthEast, South, SouthWest, West, NorthWest };
struct WindVector { float x = 1.f, y = 0.f; };
inline constexpr std::array<WindVector, 8> kWindDirections{{
    {1.f, 0.f}, {.70710678f, -.70710678f}, {0.f, -1.f}, {-.70710678f, -.70710678f},
    {-1.f, 0.f}, {-.70710678f, .70710678f}, {0.f, 1.f}, {.70710678f, .70710678f}
}};
inline const char* WindDirectionName(WindDirection direction) noexcept
{
    constexpr const char* names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return names[static_cast<unsigned>(direction) % 8];
}
inline float WindFinite(float value, float fallback) noexcept { return std::isfinite(value) ? value : fallback; }
inline float WindDelta(float dt) noexcept { return std::clamp(WindFinite(dt, 0.f), 0.f, .1f); }
inline double WindWrap(double value, double period) noexcept { return value - period * std::floor(value / period); }
inline WindDirection NearestWindDirection(float headingDegrees) noexcept
{
    return static_cast<WindDirection>(unsigned(WindWrap(WindFinite(headingDegrees, 0.f) + 22.5, 360.0) / 45.0));
}

struct WindSettings
{
    bool enabled = true;
    bool dynamic = true;               // time variation; weather response is independent
    WindDirection direction = WindDirection::NorthEast;
    float strength = 1.f;               // 100% = reference breeze, 0 = calm even in a storm
    float gustAmount = .35f;            // bounded fractional variation about the baseline
    float gustSeconds = 18.f;
    float directionSeconds = 120.f;     // interval between eight-way targets
    float transitionSeconds = 30.f;     // strength response to ~95%; also bounds turning speed
    float weatherResponse = 1.f;
    bool affectWaves = true;
    bool affectClouds = true;
    bool debugView = false;
};

struct WindWeather
{
    int kind = 0;                      // native observation: 0 clear, 1 rain, 2 snow, 3 sand
    float strength = 0.f;               // local weather intensity, NOT a server severity reading
};
// Apply the zone multiplier ONCE, before the shared [0,1] clamp. The same
// result feeds wind, cloud coverage and the legacy (unlinked) sea response.
inline WindWeather ResolveWindWeather(int kind, float intensity, float zoneMultiplier) noexcept
{
    if (kind < 1 || kind > 3) return {};
    return {kind, std::clamp(std::clamp(WindFinite(intensity, 0.f), 0.f, 2.f) *
        std::clamp(WindFinite(zoneMultiplier, 1.f), 0.f, 4.f), 0.f, 1.f)};
}

struct WindState
{
    // sequence=0 denotes a frame made outside the live simulation (e.g. a
    // device test). Enabled=false opts consumers back into their authored
    // legacy motion, not a hidden second wind generator.
    uint64_t sequence = 0;
    float deltaSeconds = 0.f;
    bool enabled = false, dynamic = false, affectWaves = false, affectClouds = false, debugView = false;
    WindDirection targetDirection = WindDirection::NorthEast;
    WindVector direction{.70710678f, -.70710678f}; // unit vector, including during turns/calm
    WindVector velocity{0.f, 0.f};               // world yards/second; zero when disabled
    float headingDegrees = 45.f;                // clockwise from north, [0,360)
    float strength = 0.f, targetStrength = 0.f;  // [0,3], 1 = reference breeze
    float secondsToChange = 0.f;
    WindWeather weather;
    bool DrivesWaves() const noexcept { return enabled && affectWaves; }
    bool DrivesClouds() const noexcept { return enabled && affectClouds; }
};

class WindSystem
{
public:
    void Configure(WindSettings settings) noexcept
    {
        settings.direction = static_cast<WindDirection>(std::min(static_cast<unsigned>(settings.direction), 7u));
        settings.strength = std::clamp(WindFinite(settings.strength, 1.f), 0.f, 2.f);
        settings.gustAmount = std::clamp(WindFinite(settings.gustAmount, .35f), 0.f, 1.f);
        settings.gustSeconds = std::clamp(WindFinite(settings.gustSeconds, 18.f), 2.f, 120.f);
        settings.directionSeconds = std::clamp(WindFinite(settings.directionSeconds, 120.f), 15.f, 600.f);
        settings.transitionSeconds = std::clamp(WindFinite(settings.transitionSeconds, 30.f), 1.f, 120.f);
        settings.weatherResponse = std::clamp(WindFinite(settings.weatherResponse, 1.f), 0.f, 2.f);
        // Reloading an unrelated F7 control must NOT reseed wind, restart a
        // gust or undo a currently selected dynamic target.
        if (settings.direction != m_settings.direction || settings.dynamic != m_settings.dynamic)
        {
            m_target = settings.direction;
            m_directionCycle = 0;
        }
        m_settings = settings;
    }
    const WindSettings& Settings() const noexcept { return m_settings; }
    const WindState& State() const noexcept { return m_state; }

    const WindState& Update(WindWeather weather, float deltaSeconds) noexcept
    {
        const float dt = WindDelta(deltaSeconds);
        weather = ResolveWindWeather(weather.kind, weather.strength, 1.f);
        if (!m_primed)
        {
            m_target = m_settings.direction;
            m_heading = double(static_cast<unsigned>(m_target)) * 45.0;
        }
        if (m_settings.enabled && m_settings.dynamic)
        {
            m_directionCycle += double(dt) / m_settings.directionSeconds;
            if (m_directionCycle >= 1.0)
            {
                m_directionCycle -= 1.0;
                // An adjacent or quarter turn, never an instantaneous 180.
                // Deterministic local RNG: independent of draw count/camera,
                // and no interference with the game's random-number state.
                const uint32_t random = NextRandom();
                const int step = (random & 1u ? 1 : -1) * (random & 2u ? 1 : 2);
                m_target = static_cast<WindDirection>((int(m_target) + step + 8) % 8);
            }
            const double rates[] = {1.0, .431, 1.873};
            for (unsigned i = 0; i < m_gustCycle.size(); ++i)
                m_gustCycle[i] = WindWrap(m_gustCycle[i] + dt * rates[i] / m_settings.gustSeconds, 1.0);
        }
        else if (!m_settings.dynamic) m_target = m_settings.direction;

        const float weatherGain = weather.kind == 1 ? .8f : weather.kind == 2 ? .4f : weather.kind == 3 ? 1.1f : 0.f;
        const double gust = m_settings.dynamic ?
            .55 * std::sin(m_gustCycle[0] * 2 * kWindPi) + .3 * std::sin(m_gustCycle[1] * 2 * kWindPi)
                + .15 * std::sin(m_gustCycle[2] * 2 * kWindPi) : 0.0;
        const double targetStrength = std::clamp(double(m_settings.strength) *
            (1.0 + weatherGain * weather.strength * m_settings.weatherResponse) *
            (1.0 + m_settings.gustAmount * gust), 0.0, 3.0);
        if (!m_primed) { m_strength = targetStrength; m_primed = true; }
        if (m_settings.enabled)
        {
            const double response = -std::expm1(-3.0 * dt / m_settings.transitionSeconds);
            const double difference = std::remainder(double(static_cast<unsigned>(m_target)) * 45.0 - m_heading, 360.0);
            const double maxTurn = 45.0 * dt / m_settings.transitionSeconds;
            m_heading = WindWrap(m_heading + std::clamp(difference * response, -maxTurn, maxTurn), 360.0);
            // Clearing deliberately drains more slowly than a rising wind.
            const double strengthResponse = -std::expm1(-3.0 * dt /
                (m_settings.transitionSeconds * (targetStrength < m_strength ? 1.5 : 1.0)));
            m_strength += (targetStrength - m_strength) * strengthResponse;
            if (std::abs(targetStrength - m_strength) < 1e-7) m_strength = targetStrength;
        }
        ++m_state.sequence;
        m_state.deltaSeconds = dt;
        m_state.enabled = m_settings.enabled;
        m_state.dynamic = m_settings.dynamic;
        m_state.affectClouds = m_settings.affectClouds;
        m_state.affectWaves = m_settings.affectWaves;
        m_state.debugView = m_settings.debugView;
        m_state.targetDirection = m_target;
        m_state.headingDegrees = float(m_heading);
        if (m_state.headingDegrees >= 360.f) m_state.headingDegrees = 0.f;
        const double radians = m_heading * kWindPi / 180.0;
        m_state.direction = {float(std::cos(radians)), float(-std::sin(radians))};
        m_state.strength = m_settings.enabled ? float(m_strength) : 0.f;
        m_state.targetStrength = m_settings.enabled ? float(targetStrength) : 0.f;
        m_state.velocity = {m_state.direction.x * m_state.strength * kWindReferenceSpeed,
                            m_state.direction.y * m_state.strength * kWindReferenceSpeed};
        m_state.secondsToChange = m_settings.enabled && m_settings.dynamic ?
            float((1.0 - m_directionCycle) * m_settings.directionSeconds) : 0.f;
        m_state.weather = weather;
        return m_state;
    }

private:
    uint32_t NextRandom() noexcept
    {
        m_random ^= m_random << 13; m_random ^= m_random >> 17; m_random ^= m_random << 5;
        return m_random;
    }
    WindSettings m_settings;
    WindState m_state;
    bool m_primed = false;
    WindDirection m_target = WindDirection::NorthEast;
    uint32_t m_random = 0x6d2b79f5u;
    double m_heading = 45.0, m_strength = 1.0, m_directionCycle = 0;
    std::array<double, 3> m_gustCycle{};
};
}
