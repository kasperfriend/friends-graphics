#pragma once

// Is that bright pixel really the sun/moon sprite, or the ground in front of it?
//
// The client draws the moon as a small camera-facing billboard at a fixed
// 4-16 yards along the light - the sprite's depth is a constant, and it is
// NEARER than most of the world. The cloud pass has to treat those pixels as
// clear sky (otherwise the deck is cut by a dark disc), so it identifies them
// by "geometry whose depth matches the tracked sprite". That test alone is
// ambiguous: any wall, floor or roof that happens to be 4-16 yards away inside
// the same screen circle matches too, and the pass then marches clouds through
// it and lifts the sun-shadow mask there. Seen from below in a city - a floor
// or a ceiling filling the circle at the sprite's own distance - the result is
// exactly the report: the moon's disc appearing through the ground.
//
// The discriminator is geometry: a camera-facing billboard has ONE view depth
// across its whole face, so its raw depth is constant from the centre to the
// rim. A wall, floor or roof at the same centre distance is a plane at an
// angle, and its depth varies across the same circle. Requiring the centre AND
// a ring inside the rim to agree in raw depth rejects the impostor without
// touching a real sprite (which is constant by construction).
//
// Kept device-free so tools/CameraAnchoringRegression.cpp can pin it; the ps_3_0
// helpers in VolumetricClouds mirror the same two expressions.

#include <cmath>
#include <cstddef>

namespace renderer::discgate
{
    // raw = A + B/z for the capture's projection unpack {A=P22, B=P32}. This is
    // the inverse of the z = B/(raw - A) every injected depth pass uses.
    inline float RawDepth(float p22, float p32, float viewDepth) noexcept
    {
        return p22 + p32 / (viewDepth > 1e-4f ? viewDepth : 1e-4f);
    }

    // Raw-space distance the billboard's own depth tolerance spans. Sampling
    // the two ends rather than differentiating keeps it exact at close range,
    // where the perspective divide is at its steepest.
    inline float RawTolerance(float p22, float p32, float viewDepth, float viewTolerance) noexcept
    {
        const float near = viewDepth - viewTolerance > 1e-4f ? viewDepth - viewTolerance : 1e-4f;
        const float far = viewDepth + viewTolerance;
        return std::fabs(RawDepth(p22, p32, near) - RawDepth(p22, p32, far));
    }

    // centreRaw: raw depth at the tracked disc centre.
    // rimRaw: raw depth a little inside the disc's rim - both a horizontal and
    //         a vertical tap, because that is what separates an angled
    //         floor/wall from a camera-facing face.
    // skyRaw: the raw depth at or above which the buffer is clear sky; a centre
    //         that reads as sky means the sprite was not painted there at all.
    inline bool IsFlatSprite(float centreRaw, const float* rimRaw, std::size_t rimCount,
                             float rawTolerance, float skyRaw) noexcept
    {
        if (!std::isfinite(centreRaw) || !std::isfinite(rawTolerance) || rawTolerance <= 0.f ||
            rimRaw == nullptr || rimCount == 0)
            return false;
        // Clear sky at the centre: nothing was drawn there to protect.
        if (centreRaw >= skyRaw)
            return false;
        for (std::size_t i = 0; i < rimCount; ++i)
        {
            if (!std::isfinite(rimRaw[i]))
                return false;
            // A rim tap that runs off the sprite onto the sky is not agreement,
            // it is the edge of a billboard that does not cover the circle.
            if (rimRaw[i] >= skyRaw)
                return false;
            if (std::fabs(rimRaw[i] - centreRaw) > rawTolerance)
                return false;
        }
        return true;
    }
}
