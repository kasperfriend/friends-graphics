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
    // The raw double integrations. The float Phase() above is what old
    // consumers logged; the wrapped phase computation below needs the doubles.
    double Fall() const noexcept { return m_fall; }
    double WindX() const noexcept { return m_x; }
    double WindY() const noexcept { return m_y; }

private:
    double m_fall = 0, m_x = 0, m_y = 0;
};

// ---------------------------------------------------------------------------
// Per-layer phase packing shared with the precipitation pixel stage
// ---------------------------------------------------------------------------
// The pixel stage draws the continuity layer on three layers of screen-space
// lattice cells, and its grid coordinate is (uv * cells - PHASE) with phases
// that run for the whole rain. That stage is float32: subtracting a phase
// that has grown to a few thousand cells from a uv term below a hundred
// cells is a cancellation, and the result quantizes to the ulp of the
// phase - after an hour or two of rain the drop pattern collapses into
// screen-wide horizontal bands of rain ("the screen overflows with rain
// particles").
//
// The fix is a phase SPLIT, computed here in exact double arithmetic and
// uploaded as two small numbers per layer/axis:
//
//     PHASE = N + f        with f in [0, 1)  and N wrapped by the period
//
// The stage computes uv*cells - f (small minus small: no cancellation, so
// the pixel levels stay exactly as fine as at t=0, forever) and shifts the
// cell INDEX by N, which its cell hash wraps by the same period. The wrap
// is seamless because the hash is periodic in the period, so the visible
// pattern of layer i repeats only after its period has scrolled by (about
// 8-50 min per layer per mode) and the layers, having deliberately
// non-commensurate periods, never realign: the combined pattern
// effectively never repeats.
//
// The phase formulas below are the CPU twin of the stage's grid
// constructions (uv*64*s - 64*v*windX, uv*16*s - 16*v*(1.2*fall+windY),
// etc.); change one and the other must follow. tools/WeatherDeviceRegression.cpp
// drives the stage with phases from this function, so the pairing is
// checked on the GPU in CI.
struct WrappedPhases
{
    // One float4 per layer: (xFrac, xInt, yFrac, yInt) in lattice cells.
    float c6[4] = { 0, 0, 0, 0 };   // layer 0 (nearest)
    float c7[4] = { 0, 0, 0, 0 };   // layer 1
    float c8[4] = { 0, 0, 0, 0 };   // layer 2 (farthest)
    // c9: x = snow wobble sin argument (2pi-wrapped),
    //     yzw = sand gust sin arguments (2pi-wrapped)
    float c9[4] = { 0, 0, 0, 0 };
    // c10: xy = mist drift (decimal parts only; the mist samples with
    //      frac(), so integer travel is irrelevant)
    float c10[4] = { 0, 0, 0, 0 };
};

namespace precipitationlayers
{
    // The stage's own tables (its layerScales/layerSpeeds), kept here so the
    // phase math and the shader cannot drift apart.
    constexpr float scale[3] = { 1.6f, 3.8f, 8.5f };
    constexpr float speed[3] = { 1.25f, 1.0f, 0.75f };
    // Lattice periods, per mode and axis (the stage spells the same values).
    // Chosen so each layer's visible pattern repeats on an independent
    // 8-50 min cycle; the combined pattern effectively never repeats.
    constexpr float rainPeriodX[3] = { 32768.f, 32768.f, 32768.f };
    constexpr float rainPeriodY[3] = { 32768.f, 16384.f, 8192.f };
    constexpr float snowPeriodX[3] = { 32768.f, 32768.f, 32768.f };
    constexpr float snowPeriodY[3] = { 32768.f, 16384.f, 8192.f };
    constexpr float sandPeriodX[3] = { 1024.f, 2048.f, 8192.f };
    constexpr float sandPeriodY[3] = { 1024.f, 2048.f, 8192.f };
    constexpr float mistPeriodX = 1024.f;
    constexpr float mistPeriodY = 2048.f;
}

// Split a double phase into (f in [0,1), N wrapped into [0,period)):
// both exact (integer arithmetic below 2^53, result below 2^24). The float
// conversion of the fraction can round UP to exactly 1.0 - either from a
// fraction above 1 - 2^-25, or from a phase a hair negative (floor -1,
// fraction a hair below 1), which is what the wind projections sitting on
// the camera axis produce. In that case the ONE moves into N instead: f
// and N stay exact, and the stage's identity floor(uv*S - f) - N =
// floor(uv*S - phase) is unchanged, because N only shifts by one integer.
static inline float FractionOf(double x) noexcept
{
    // The naive (float)(x - floor(x)) can round UP to exactly 1.0 - either
    // from a fraction above 1 - 2^-25, or from x a hair negative (floor -1,
    // fraction a hair below 1), which is what the wind projections sitting
    // on the camera axis produce. 1.0 is not in [0,1): the ONE moves into
    // the integer part by the caller instead.
    const float f = (float)(x - std::floor(x));
    return f >= 1.0f ? 0.f : f;
}

static inline void SplitPhase(double phase, double period, float& f, float& n)
{
    if (!std::isfinite(phase)) { f = 0.f; n = 0.f; return; }
    double integer = std::floor(phase);
    f = FractionOf(phase);
    if (f == 0.f && (phase - integer) > 0.5 * 2.0 / 16777216.0)
    {
        // The fraction was so close to 1 that it rounded up: the ONE belongs
        // to N, not to f. (0.5 * 2/2^24 is the float midpoint of [0,1).)
        integer += 1.0;
    }
    n = (float)WindWrap(integer, period);
}

inline WrappedPhases ComputeWrappedPhases(double fall, double windX, double windY, int mode) noexcept
{
    WrappedPhases out;
    const auto* periodX = mode == 2 ? precipitationlayers::snowPeriodX
                       : mode == 3 ? precipitationlayers::sandPeriodX
                       :              precipitationlayers::rainPeriodX;
    const auto* periodY = mode == 2 ? precipitationlayers::snowPeriodY
                       : mode == 3 ? precipitationlayers::sandPeriodY
                       :              precipitationlayers::rainPeriodY;
    for (int i = 0; i < 3; ++i)
    {
        const float v = precipitationlayers::speed[i];
        double px, py;
        if (mode == 2)
        {
            // Snow: the stage samples the grid *12; its x offset is
            // 12*0.5*v*windX and its y offset 12*0.6*v*fall.
            px = 6.0 * v * windX;
            py = 7.2 * v * fall;
        }
        else if (mode == 3)
        {
            // Sand: the stage samples *0.5; the fall term is not
            // layer-speed scaled in the shader, only the wind offset is.
            px = 1.5 * v * windX + fall;
            py = 0.5 * v * windY;
        }
        else
        {
            // Rain: the grid's x offset is 64*v*windX and its y offset
            // 16*v*(1.2*fall + windY), both in lattice cells.
            px = 64.0 * v * windX;
            py = 16.0 * v * (1.2 * fall + windY);
        }
        float* dst = i == 0 ? out.c6 : i == 1 ? out.c7 : out.c8;
        SplitPhase(px, periodX[i], dst[0], dst[1]);
        SplitPhase(py, periodY[i], dst[2], dst[3]);
    }
    // The wobbles are sines: 2pi-wrapping their arguments (exact in double)
    // is enough to keep them bounded for a whole session.
    // Wobble/mist lanes use the same bounded-fraction discipline: the float
    // of a double just below a period boundary can round ONTO the boundary,
    // and sin(2pi) is not the authored 0.
    const double twoPi = 6.283185307179586476925286766559005;
    const float twoPiFloat = 6.2831855f; // the float32 of 2pi
    if (mode == 2)
    {
        out.c9[0] = (float)std::fmod(1.6 * fall, twoPi);
        if (out.c9[0] >= twoPiFloat) out.c9[0] = 0.f;
    }
    else if (mode == 3)
        for (int i = 0; i < 3; ++i)
        {
            out.c9[1 + i] = (float)std::fmod(6.0 * precipitationlayers::speed[i] * windX + 4.0 * fall, twoPi);
            if (out.c9[1 + i] >= twoPiFloat) out.c9[1 + i] = 0.f;
        }
    if (mode != 3)
    {
        out.c10[0] = FractionOf(0.2 * windX);
        out.c10[1] = FractionOf(0.15 * fall);
    }
    return out;
}
}
