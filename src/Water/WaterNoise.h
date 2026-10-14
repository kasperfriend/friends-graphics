#pragma once
#include <cmath>
#include "WaveField.h"

namespace renderer::water
{
    // The wave field's hash, shared by the production pixel shader and the offline
    // previewer in tools/WaterPreview.cpp.
    //
    // Keep this in step with noiseGradient() in WaterEffect.h: the same six lines,
    // factored into a header only so the preview can reproduce the exact waves the
    // player sees. The properties that matter, and that the shader comments explain
    // at length, are that the lattice index is wrapped into [0,kLatticeWrap) BEFORE
    // it is hashed - an unwrapped index at ordinary world distances exhausts the
    // float32 fraction bits, the noise collapses onto its own lattice and hardens
    // into flat axis-aligned squares - and that dx/dy are the exact derivatives of
    // the same interpolated value, so a wave's slope and its lighting cannot
    // disagree with the wave.
    struct NoiseSample
    {
        float value = 0.f;
        float dx = 0.f;
        float dy = 0.f;
    };

    // HLSL frac(): always towards positive infinity, so a negative coordinate still
    // lands on the same corner as the shader's.
    inline float Fraction(float value) { return value - std::floor(value); }

    inline NoiseSample NoiseGradient(float x, float y)
    {
        const float ix = std::floor(x), iy = std::floor(y);
        const float fx = x - ix, fy = y - iy;
        const float wx = ix - std::floor(ix * (1.f / kLatticeWrap)) * kLatticeWrap;
        const float wy = iy - std::floor(iy * (1.f / kLatticeWrap)) * kLatticeWrap;
        // Corner ids in the packed index: (0,0), (1,0), (0,1), (1,1).
        const float base = wx + wy * kLatticeWrap;
        float h[4];
        for (int corner = 0; corner < 4; ++corner)
        {
            const float ids[4] = { 0.f, 1.f, kLatticeWrap, kLatticeWrap + 1.f };
            float value = Fraction((base + ids[corner]) * .1031f);
            value *= value + 33.33f;
            h[corner] = Fraction(value * (value + value));
        }
        const float ux = fx * fx * (3.f - 2.f * fx), uy = fy * fy * (3.f - 2.f * fy);
        const float dax = 6.f * fx * (1.f - fx), day = 6.f * fy * (1.f - fy);
        const float k = h[0] - h[1] - h[2] + h[3];
        return { h[0] + (h[1] - h[0]) * ux + (h[2] - h[0]) * uy + k * ux * uy,
                 dax * ((h[1] - h[0]) + k * uy),
                 day * ((h[2] - h[0]) + k * ux) };
    }

    struct WavePlane
    {
        float x = 0.f;
        float y = 0.f;
    };

    struct WaveWarpSample
    {
        WavePlane plane;
        float noiseValue = .5f; // broad breakup field reused by the crest-foam mask
        float gradientX = 0.f; // derivative of the scalar warp with respect to cell X
        float gradientY = 0.f; // derivative of the scalar warp with respect to cell Y
    };

    // Bend the broad, analytic swell with a smooth pseudorandom scalar field
    // along one oblique direction. The first derivatives come from the same
    // noise sample, so the CPU and pixel shader can apply the exact chain rule
    // to the analytic swell slope. The CPU phase uses this same rotate/scale
    // transform, keeping the field anchored across cameras and liquid tiles.
    inline WaveWarpSample WarpWavePlane(const WaveField& field, float cellX, float cellY)
    {
        const float qx = (cellX * kWaveWarpCos + cellY * kWaveWarpSin) * kWaveWarpFrequency
            + field.warpPhase[0];
        const float qy = (-cellX * kWaveWarpSin + cellY * kWaveWarpCos) * kWaveWarpFrequency
            + field.warpPhase[1];
        const NoiseSample noise = NoiseGradient(qx, qy);
        const float warp = (noise.value - .5f) * kWaveWarpAmplitude;
        const float gradientX = kWaveWarpFrequency * (noise.dx * kWaveWarpCos - noise.dy * kWaveWarpSin);
        const float gradientY = kWaveWarpFrequency * (noise.dx * kWaveWarpSin + noise.dy * kWaveWarpCos);
        return { { cellX + warp * kWaveWarpDirectionX,
                   cellY + warp * kWaveWarpDirectionY }, noise.value, gradientX, gradientY };
    }

    struct GroupSample
    {
        float gain = 1.f;      // the swell's amplitude here: kGroupBase..kGroupMax
        float gradientX = 0.f; // d(gain)/d(lattice cell): the product rule the shading needs
        float gradientY = 0.f;
    };

    // The grouping envelope, at one lattice point. The vertex stage samples this and
    // passes the triple to the pixels, which is why the gradient is pre-scaled by the
    // field's own frequency and depth here: the pixel stage then only has to add
    // `height * gradient` to the swell's slope, and a set's edge tilts the normal
    // exactly as much as the envelope actually moves the water.
    inline GroupSample GroupEnvelope(const WaveField& field, float cellX, float cellY)
    {
        const float qx = (cellX * kGroupCos + cellY * kGroupSin) * kGroupFrequency + field.groupPhase[0];
        const float qy = (-cellX * kGroupSin + cellY * kGroupCos) * kGroupFrequency + field.groupPhase[1];
        const NoiseSample noise = NoiseGradient(qx, qy);
        // Back through the same rotation the sample went through, so both gradients are
        // in lattice-cell axes - which is the space the swell's slope lives in and the
        // only reason the pixel stage can add the two together at all.
        const float scale = kGroupFrequency * kGroupDepth;
        return { kGroupBase + kGroupDepth * noise.value,
                 scale * (noise.dx * kGroupCos - noise.dy * kGroupSin),
                 scale * (noise.dx * kGroupSin + noise.dy * kGroupCos) };
    }
}
