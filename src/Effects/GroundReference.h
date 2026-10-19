#pragma once

// The floor a fog layer sits on.
//
// LocalFog, the height slab and the ground mist are all integrated against a
// WORLD height taken from the terrain height atlas, or - where no terrain was
// captured, which is most of Orgrimmar from street level - from the depth
// buffer. That fallback used to read three fixed rows near the bottom of the
// screen and take their median. Instrumented reports from a city show what
// that means: as soon as the camera pitches up, those three taps land on a
// wall, a balcony or a roof instead of the street, and the whole fog bank
// rides up with the view. Standing in the same spot and looking down puts it
// back on the ground, which is why the layer looked camera-attached.
//
// The rule here is the floor: a probe is only a candidate if it actually hit a
// surface, and a surface far above the eye is never the floor - it is a roof
// or a ledge the view swept over. The base is the LOWEST such surface, so a
// crate or a stair beside the player cannot lift it either, and when a frame
// offers no candidate at all the previous base is held rather than replaced by
// a guess.
//
// Kept device-free so tools/CameraAnchoringRegression.cpp can pin it; the ps_3_0
// probe in DirectionalVolumetricLighting's kGroundHeightSource mirrors it and
// the same test checks that the two still agree.

#include <cmath>
#include <cstddef>

namespace renderer::groundref
{
    struct Sample
    {
        float height = 0.f;
        bool hit = false;   // false: the tap saw clear sky, not a surface
    };

    // A surface more than this far above the eye cannot be the floor a fog
    // layer sits on. Wide enough for a step or a kerb under the camera, far
    // short of a wall or a roof.
    inline constexpr float kAboveEyeTolerance = 0.75f;

    // Yard-per-second bounds on how fast the base may travel. A probe that
    // flicks between the street and a crate for one frame must not drag the
    // whole layer with it.
    inline constexpr float kRiseYardsPerSecond = 3.f;
    inline constexpr float kFallYardsPerSecond = 8.f;

    // Which sample is the floor, among those that could be one. Returns false
    // when nothing in the frame qualifies (the caller then holds `previous`).
    inline bool PickFloor(const Sample* samples, std::size_t count, float eyeHeight,
                          float& out) noexcept
    {
        bool found = false;
        float lowest = 0.f;
        for (std::size_t i = 0; i < count; ++i)
        {
            const Sample& sample = samples[i];
            if (!sample.hit || !std::isfinite(sample.height))
                continue;
            // Above the eye: a wall or a roof, never the floor.
            if (sample.height > eyeHeight + kAboveEyeTolerance)
                continue;
            if (!found || sample.height < lowest)
            {
                lowest = sample.height;
                found = true;
            }
        }
        if (!found)
            return false;
        // The layer never sits above the viewer: standing under an arch or on
        // a ledge must put the bank at or below the eye, not over the head.
        out = std::fmin(lowest, eyeHeight);
        return true;
    }

    // Resolves this frame's layer base. `previous` is the last published
    // base; a non-finite value means "no history" and takes the new floor
    // immediately. Returns the value to publish.
    inline float ResolveLayerBase(float previous, float eyeHeight, const Sample* samples,
                                  std::size_t count, float dtSeconds, bool& observed) noexcept
    {
        float target = 0.f;
        observed = PickFloor(samples, count, eyeHeight, target);
        if (!observed)
            return std::isfinite(previous) ? previous : eyeHeight;
        if (!std::isfinite(previous))
            return target;
        const float dt = std::isfinite(dtSeconds) ? (dtSeconds < 0.f ? 0.f : dtSeconds) : 0.f;
        const float delta = target - previous;
        const float limit = delta > 0.f ? kRiseYardsPerSecond * dt : kFallYardsPerSecond * dt;
        const float next = previous + (delta > limit ? limit : (delta < -limit ? -limit : delta));
        return std::fmin(next, eyeHeight);
    }
}
