#pragma once
// WATER QUALITY: one level, 0..10, and the single ladder that decides what the
// water costs.
//
// Every other water number in this tree is a LOOK: how tall, how clear, how much
// foam. None of them says how much work the surface is allowed to take, which left
// a player with two choices - the shipped sea, or turning features off one by one
// until the frame came back. This header is the third thing: one level that trades
// the whole set of expensive terms at once, from a flat sheet that costs about what
// the client's own water costs up to everything this renderer knows how to draw.
//
// It is pure and device-free like the rest of src/Water, and it is the ONLY place
// the ladder is written down. WaterEffect.h resolves a level into a QualityPlan
// once per tuning reload, uploads the plan as constants, and the two shader stages
// compare the level they were given against the midpoints of these same numbers -
// so a level can never sit on a boundary, and the CPU and the GPU cannot disagree
// about what a level means. tools/WaterRegression.cpp holds the ladder, the gates
// and the shader's comparisons to each other.
//
// Two rules the ladder follows:
//
//  * A level never changes the AUTHORED look of the water it keeps. Wave height,
//    clarity, foam amount, glint and every per-zone number mean the same thing at
//    every level; a level decides how many of the terms that build them are
//    evaluated, not how strong they are. Level 6 is the sea this renderer shipped
//    before the slider existed, at the same eight march steps.
//  * A level is monotone. Raising it can only ever add work and add detail, so the
//    slider has no surprises in it and a player looking for frames only moves down.
#include <algorithm>

namespace renderer::water
{
    inline constexpr int kQualityMin = 0;
    inline constexpr int kQualityMax = 10;
    // Ships at 7: the whole of the previously shipped sea plus the mid-wave band, the
    // foam lace and the shore back-flow, at the same eight march steps it always ran.
    // The four levels above it buy displaced mid waves, longer marches and the last of
    // the foam work, and are where a machine with room to spend should be pointed.
    inline constexpr int kQualityDefault = 7;

    // ---- the gates, as levels ------------------------------------------------
    // A feature switches on AT this level, so the shader compares the uploaded level
    // against `GateFor(level) = level - .5`: strictly above the gate below it and
    // strictly below the gate above it, whatever rounding a float constant takes on
    // the way through the driver.
    inline constexpr int kQualityWaves = 1;        // a shaded sea at all (analytic swell)
    inline constexpr int kQualityChop = 2;         // the base chop octave, and the mesh lifts
    inline constexpr int kQualityChopFine = 4;     // the second, finer chop octave
    inline constexpr int kQualityShore = 4;        // beach-vs-rock from the captured bed slope
    inline constexpr int kQualityMidWaves = 7;     // the mid octave, foam lace, shore back-flow
    inline constexpr int kQualityMidGeometry = 8;  // the mesh also lifts train C: 20 yard waves
    inline constexpr int kQualityFoamGlow = 10;    // backlit foam veil, soft refracted contact

    // The midpoint a shader stage compares its level against. Spelled as a function of
    // the level so the two halves of the renderer cannot drift by one: the shader's
    // literals are `GateFor(kQualityX)` and the regression test computes them from here.
    inline constexpr float GateFor(int level) noexcept
    {
        return float(level) - .5f;
    }

    // ---- the ladder ----------------------------------------------------------
    // March steps are the only entries that are not a gate: the two ray loops take a
    // trip count from a constant register instead of a branch, so a level can hand them
    // any count between 0 (skip the loop entirely, which the surrounding `steps > 0`
    // test does) and the 12 the loops are written against. Fewer steps is a coarser
    // reflection and a coarser answer to "is something between this water and the sun",
    // and both degrade quietly rather than switching off.
    inline constexpr int kSsrSteps[11] = {0, 0, 3, 4, 6, 7, 8, 8, 10, 12, 12};
    inline constexpr int kLightSteps[11] = {0, 0, 0, 4, 6, 7, 8, 8, 10, 12, 12};
    // Emitters reflected in the surface. Clamped on the CPU, which is where the lights
    // are selected and transformed, so a low level also skips that per-frame work.
    inline constexpr int kLocalLights[11] = {0, 2, 4, 6, 8, 8, 8, 8, 8, 8, 8};
    // Noise octaves the pixel stage shades: the base chop, the fine chop, and the mid
    // band between the shortest analytic train and the base chop.
    inline constexpr int kChopOctaves[11] = {0, 0, 1, 1, 2, 2, 2, 3, 3, 3, 3};
    // Gerstner trains the MESH lifts: none, the two longest, or the two longest plus the
    // twenty yard train C. The client's liquid grid is about four yards apart, so C - at
    // five facets to a crest - is the shortest wave it can carry without faceting, and it
    // is the ceiling rather than a step on the way to something shorter.
    inline constexpr int kGeometryTrains[11] = {0, 0, 2, 2, 2, 2, 2, 2, 3, 3, 3};

    struct QualityPlan
    {
        int level = kQualityDefault;
        int ssrSteps = 8;        // screen-space reflection march steps, 0..12
        int lightSteps = 8;      // celestial occlusion march steps, 0..12
        int localLights = 8;     // emitters reflected, 0..8
        int chopOctaves = 2;     // noise octaves shaded, 0..3
        int geometryTrains = 2;  // Gerstner trains the mesh lifts, 0..3
        // The gates, already resolved: cheaper to read than to re-derive, and a caller
        // that asks the plan cannot ask a different question than the shader answers.
        bool waves = true;       // a shaded sea at all; false leaves the client's own surface
        bool shore = true;       // bed-slope shore understanding: a beach is not a rock wall
        bool midWaves = true;    // the mid octave, the foam lace and the shore back-flow
        bool midGeometry = false;// train C displaced as well as shaded
        bool foamGlow = false;   // backlit foam veil and the soft refracted contact line
    };

    inline constexpr QualityPlan QualityPlanFor(int level) noexcept
    {
        const int resolved = level < kQualityMin ? kQualityMin : (level > kQualityMax ? kQualityMax : level);
        QualityPlan plan;
        plan.level = resolved;
        plan.ssrSteps = kSsrSteps[resolved];
        plan.lightSteps = kLightSteps[resolved];
        plan.localLights = kLocalLights[resolved];
        plan.chopOctaves = kChopOctaves[resolved];
        plan.geometryTrains = kGeometryTrains[resolved];
        plan.waves = resolved >= kQualityWaves;
        plan.shore = resolved >= kQualityShore;
        plan.midWaves = resolved >= kQualityMidWaves;
        plan.midGeometry = resolved >= kQualityMidGeometry;
        plan.foamGlow = resolved >= kQualityFoamGlow;
        return plan;
    }

    // A flat plan is the one level that removes the procedural sea entirely: no wave
    // gate, no chop, no marches, no foam. Depth absorption, refraction and the sky
    // reflection survive, because those are what the captured scene copy buys and they
    // are the cheap part of it.
    inline constexpr bool IsFlat(const QualityPlan& plan) noexcept
    {
        return !plan.waves;
    }

    // Names for logs and diagnostics. A player reads these in WaterEffect.log next to
    // the resolved numbers, which is how a report says what a machine was actually
    // asked to draw rather than what its ini claimed.
    inline const char* QualityName(int level) noexcept
    {
        const int resolved = level < kQualityMin ? kQualityMin : (level > kQualityMax ? kQualityMax : level);
        switch (resolved)
        {
        case 0: return "FLAT";
        case 1: return "MINIMAL";
        case 2: return "LOW";
        case 3: return "FAIR";
        case 4: return "MEDIUM";
        case 5: return "GOOD";
        case 6: return "HIGH";
        case 7: return "ENHANCED";
        case 8: return "VERY HIGH";
        case 9: return "ULTRA";
        default: return "MAX";
        }
    }

    // How far the waterline travels up and back down a beach, in yards, at this level.
    // It is the same amplitude the geometry waves lift by - a run-up is an arriving crest
    // spending its height on the slope - so it inherits WAVE HEIGHT, WAVE SIZE, the zone's
    // own sea state and the WAVES switch without a second authoring path, and it is zero
    // on water that was authored flat, where nothing arrives to run up. `amplitude` is
    // GeometryWaves::amplitude; the cap keeps an absurd WAVE HEIGHT from draining a whole
    // harbour up the beach.
    inline float SwashYards(const QualityPlan& plan, float amplitude) noexcept
    {
        if (!plan.midWaves) return 0.f;
        const float yards = std::isfinite(amplitude) ? amplitude : 0.f;
        return std::clamp(yards * 1.35f, 0.f, 2.5f);
    }
}
