#pragma once
// Screen-space god rays: the CPU mirror of the sun/moon source mask and the radial
// gather in src/Effects/VolumeEffects.h (volumePixelSource mode 6 and
// solarRadialPixelSource). It has no windows.h dependency, so tools/GodRayPreview.cpp
// and tools/GodRayRegression.cpp run the same arithmetic the ps_3_0 passes run.
//
// Model. A light-transmission mask is built over the WHOLE frame:
//   mask(p) = openness(p) * cloudTransmission(p) * sourceWeight(p)
// openness is 0 wherever the depth buffer holds world geometry (trees, walls,
// characters, ground) and 1 in open sky; cloudTransmission comes from the cloud
// alpha; sourceWeight is the disc plus a sunward glow that falls off with angle
// from the tracked source. A radial gather then averages the mask along the
// screen-space segment from each pixel toward the source. Sky reached through a
// gap reads bright, and a trunk or character crossing that segment casts a dark
// stripe away from the source: beams with visible shadows, not a glow around a disc.
//
// The earlier pass sampled only a small disc-shaped mask, so the gather could only
// show a soft halo with spokes; nothing crossed the scene.

#include <algorithm>
#include <cmath>

namespace renderer::godrays
{
    struct SourceLook
    {
        // Sunward glow falloff in squared screen-height units: exp(-d^2 * spread).
        // 6 keeps beams visible out to about d=0.5 and fades the far sky; 2.2
        // washed the whole frame, 10 kept the rays too close to the disc.
        float skySpread = 6.0f;
        // Minimum glow in open sky. Zero keeps far sky from washing out; the
        // gather's lift below would brighten any residual floor too much.
        float skyFloor = 0.0f;
    };

    struct RadialLook
    {
        int samples = 48;      // taps along the pixel-to-source segment
        float reach = 1.0f;    // fraction of that segment (1 = all the way to the source)
        float decay = 0.9694f; // per-tap weight, pow(.975, ShaftFalloff 1.25)
    };

    inline float Saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }

    inline float SmoothStep(float a, float b, float x)
    {
        const float t = Saturate((x - a) / (b - a));
        return t * t * (3.0f - 2.0f * t);
    }

    // Weight of the source at a screen position. uv are 0..1 with y down;
    // aspect is width/height so distances are measured in screen-height units.
    // An off-screen source contributes nothing (the shaders gate it with sun.x<-.5).
    inline float SourceWeight(float u, float v, float sunU, float sunV, float aspect,
                              bool onScreen, const SourceLook& look)
    {
        if (!onScreen) return 0.0f;
        const float dx = (u - sunU) * aspect;
        const float dy = v - sunV;
        const float d2 = dx * dx + dy * dy;
        const float d = std::sqrt(d2);
        // The disc core: the tight Gaussian the previous pass used, truncated
        // softly so its edge cannot ring around the source.
        const float disc = std::exp(-d2 * 260.0f) * (1.0f - SmoothStep(0.08f, 0.22f, d));
        // Sunward glow reaching across the frame. This is what lets the gather
        // draw beams away from the source; it is multiplied by openness outside.
        const float glow = look.skyFloor + (1.0f - look.skyFloor) * std::exp(-d2 * look.skySpread);
        return std::max(disc, glow);
    }

    // Radial gather. mask(u,v) must return the per-pixel light transmission
    // (openness * cloud transmission * source weight * strength). The taps start
    // at the pixel and advance toward the source, weighted by decay^i, so the
    // result is a normalised average of what the beam crosses on its way to the
    // source; jitter in [0,1) is the fixed per-pixel stratification phase.
    template <class Mask>
    inline float RadialGather(Mask&& mask, float u, float v, float sunU, float sunV,
                              float jitter, const RadialLook& look)
    {
        const float stepU = (sunU - u) * look.reach / float(look.samples);
        const float stepV = (sunV - v) * look.reach / float(look.samples);
        float qu = u - stepU * (1.0f - jitter);
        float qv = v - stepV * (1.0f - jitter);
        float illumination = 1.0f;
        float weightSum = 0.0f;
        float light = 0.0f;
        for (int i = 0; i < look.samples; ++i)
        {
            qu += stepU;
            qv += stepV;
            light += mask(qu, qv) * illumination;
            weightSum += illumination;
            illumination *= look.decay;
        }
        return light / std::max(weightSum, 0.001f);
    }

    // The shaders lift the gathered value with pow(x, 0.72) * 1.2 before the additive
    // composite, so faint streaks read as beams instead of a uniform wash.
    inline float LiftGather(float average)
    {
        return std::pow(Saturate(average), 0.72f) * 1.2f;
    }
}
