#pragma once
// Zone and shared-wind (or legacy native-weather) driven sea state.
//
// The wave numbers are authored globally ([Water] RIPPLE, SWELL, WIND), but a Stranglethorn coast
// and an Elwynn pond are not the same water, and neither of them stays flat when a squall comes
// through. This header turns those two facts into one small state: a per-zone agitation read from
// the shared location-profile mechanism (`WaterAgitationMultiplier`, authored per map/zone/area)
// and the live weather observation the renderer already takes for clouds and precipitation, eased
// over seconds so weather arrives instead of switching.
//
// It is pure and device-free, like the rest of src/Water, and deliberately so: WaterEffect.h and
// tools/WaterPreview.cpp both derive their wave numbers from here, so the offline frames the look
// was tuned against carry the same sea state the game will use. A change that is visible in one is
// visible in the other, which is the whole point of having the previewer.
//
// In the legacy/unlinked mode every multiplier is 1 (and the shear 0) for an untuned
// zone under a clear sky. Linked wind has the same identity at strength 1. With
// [Water] WeatherResponsePercent=0 and no authored agitation the produced numbers are exactly the
// ones from before this existed. That property is asserted in tools/WaterRegression.cpp.
#include <algorithm>
#include <cmath>

namespace renderer::water
{
    // The weather the renderer observed this frame. The kinds are the existing native weather
    // modes - 0 clear, 1 rain, 2 snow, 3 sandstorm - so water reacts to the same observation the
    // cloud deck does and neither module has to guess at the client's state.
    struct SeaWeather
    {
        int kind = 0;
        float strength = 0.f;   // 0..1, already scaled by the zone's weather intensity multiplier
    };

    struct SeaTuning
    {
        float zoneAgitation = 1.f;   // [Environment] WaterAgitationMultiplier for the area
        float response = 1.f;        // [Water] WeatherResponsePercent, how hard weather lands here
        float authoredWind = 1.f;    // [Water] WindStrengthPercent, the authored foam/wind bias
        bool sharedWind = false;    // use the one weather-driven wind instead of a second storm boost
        float windStrength = 1.f;   // shared wind [0,3], before this body's response/exposure
    };

    // Multipliers on the authored numbers. See TargetSeaState for what each one is allowed to do.
    inline float Finite(float value, float fallback) noexcept
    {
        return std::isfinite(value) ? value : fallback;
    }

    struct SeaState
    {
        float agitation = 1.f;   // wave amplitude: swell, chop, and the foam energy that goes with it
        float motion = 1.f;      // how fast the whole field travels
        float shear = 0.f;       // extra counter-current spread between the two layers, 0..1
        float wind = 1.f;        // the wind term the shader's foam gates already read
    };

    // The shape of a sea state, in one place, so the game, the previewer and the tests agree:
    //
    //  * agitation is the zone's baseline with weather added on top of it - a storm multiplies a
    //    coast's energy and barely ripples a forest pond, because a real coast is exposed and a
    //    pond is sheltered by trees. It is clamped well above 1 so an authored rough zone can be
    //    genuinely rough, and well below 1 so a calm one is genuinely flat but never a mirror.
    //  * motion follows agitation only halfway, plus weather: big slow swell still travels slowly,
    //    and rain on still water makes it busy without making it big.
    //  * shear is a weather-only effect: a blow puts a cross-chop on top of the swell, which is
    //    what stops a storm reading as the same waves turned up.
    //  * wind scales the authored WIND number rather than replacing it, so an authored calm river
    //    (WIND 0) still shows no white line at all, in any weather.
    inline SeaState TargetSeaState(const SeaTuning& tuning, const SeaWeather& weather)
    {
        SeaState state;
        const bool precipitating = weather.kind > 0 && weather.kind < 4;
        const float amount = precipitating ? std::clamp(Finite(weather.strength, 0.f), 0.f, 1.f) : 0.f;
        const float push = std::clamp(Finite(tuning.response, 1.f), 0.f, 4.f) * amount;
        // Every input is sanitised because these multipliers end up in a shader constant: a NaN
        // that survives to a wave amplitude is a black frame, not a calmer sea. `clamp` alone
        // cannot do this - it passes NaN straight through - hence the explicit finite test.
        // Linked water consumes wind's ALREADY weather-modulated strength.
        // Never multiply another rain boost into it. Response=0 keeps the
        // authored sea energy (direction can still follow wind); at strength
        // zero a little residual swell survives, but wind-driven foam vanishes.
        const float air = tuning.sharedWind ? std::clamp(1.f +
            (std::clamp(Finite(tuning.windStrength, 0.f), 0.f, 3.f) - 1.f) *
            std::clamp(Finite(tuning.response, 1.f), 0.f, 4.f), 0.f, 5.f) : 1.f;
        const float forcing = tuning.sharedWind ? std::max(air - 1.f, 0.f) : push;
        const float energy = tuning.sharedWind ? .2f + .8f * air : 1.f + .95f * push;
        state.agitation = std::clamp(Finite(tuning.zoneAgitation, 1.f) * energy, .12f, 2.6f);
        state.motion = std::clamp(1.f + (state.agitation - 1.f) * .55f + .8f * forcing, .08f, 3.f);
        state.shear = std::clamp(.25f * forcing + (state.agitation - 1.f) * .12f, 0.f, 1.f);
        state.wind = std::clamp(Finite(tuning.authoredWind, 1.f) * air * (.45f + .55f * state.agitation), 0.f, 2.5f);
        return state;
    }

    // Eases the authored target into what is on screen. Same response rule as the cloud deck: one
    // frame-rate independent time constant to 95%, a clamped delta so a loading screen cannot
    // teleport the state, and a slower drain than a build, because open water that snaps flat the
    // instant the rain stops looks broken rather than calm.
    class SeaStateFilter
    {
    public:
        static float Response(float dt, float seconds)
        {
            return -std::expm1(-3.f * std::clamp(Finite(dt, 0.f), 0.f, .1f) /
                std::max(Finite(seconds, 14.f), .01f));
        }

        SeaState Observe(const SeaTuning& tuning, const SeaWeather& weather, float dt, float buildSeconds)
        {
            const SeaState target = TargetSeaState(tuning, weather);
            if (!m_primed) { m_current = target; m_primed = true; return m_current; }
            const bool easingDown = target.agitation < m_current.agitation;
            const float seconds = std::max(Finite(buildSeconds, 14.f), .01f);
            const float alpha = Response(dt, easingDown ? seconds * 2.f : seconds);
            for (auto member : {&SeaState::agitation, &SeaState::motion, &SeaState::shear, &SeaState::wind})
            {
                m_current.*member += (target.*member - m_current.*member) * alpha;
                if (std::abs(target.*member - m_current.*member) < 1e-5f * std::max(1.f, std::abs(target.*member)))
                    m_current.*member = target.*member;
            }
            return m_current;
        }

        // Device loss replays nothing: the next frame continues from what was visible.
        void Forget() { m_primed = false; }

    private:
        bool m_primed = false;
        SeaState m_current;
    };

    // Legacy scalar clock retained for fixed-wind callers/tests. Runtime uses
    // WaveMotion, which integrates each train's speed AND shear independently.
    // Multiplying session age by a changing parameter would teleport crests.
    class SeaMotionClock
    {
    public:
        void Observe(float dt, float speed)
        {
            if (!std::isfinite(dt) || !std::isfinite(speed)) return;
            m_time += std::clamp(dt, 0.f, .1f) * std::max(speed, 0.f);
        }
        double Time() const { return m_time; }
        void Reset() { m_time = 0.0; }

    private:
        double m_time = 0.0;
    };
}
