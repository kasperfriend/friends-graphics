#pragma once
#include <cmath>

namespace renderer::water
{
    // Everything in this header keeps the water's procedural wave field
    // anchored to the terrain instead of to a liquid tile's material uv, and
    // keeps the numbers the pixel shader evaluates small. Both halves matter:
    //
    //  * A field built from per-tile uv repeats identically in every tile, so
    //    wave detail was framed by hard square edges and restarted at the seams.
    //  * A field built from raw world coordinates multiplies values in the
    //    hundreds or thousands by a hash constant. That exhausts the fraction
    //    bits of float32, the noise collapses onto its own lattice and hardens
    //    into flat axis-aligned squares.
    //
    // So the shader evaluates the lattice from the position RELATIVE TO THE
    // CAMERA (small and exact, recovered in the vertex shader from the draw's own
    // view matrix) and adds the camera's own contribution as a phase computed
    // in double on the CPU. The shader wraps lattice indices modulo
    // kLatticeWrap, which makes reducing that phase into the lattice period a
    // free, invisible operation: the field stays world-anchored while every
    // coordinate stays small.
    inline constexpr float kRippleCellYards = 2.5f;   // one ripple lattice cell
    inline constexpr float kSwellRatio = 0.055f;      // long swell shares the orientation
    inline constexpr float kLatticeWrap = 64.0f;      // must match noiseGradient()
    inline constexpr float kLatticeRotation = 0.035f; // radians per second of flow
    inline constexpr float kRippleDriftYards = 2.2f;  // ripple travel, yards per second
    inline constexpr float kSwellDriftYards = .8f;    // long swell travel
    inline constexpr float kCounterAngle = 1.9f;      // full counter-current spread
    inline constexpr float kGeometryWaveCells = 16.f; // one geometry wave spans 16 cells
    inline constexpr float kUvDriftU = .0019f;        // native detail map creep, uv/s
    inline constexpr float kUvDriftV = -.0011f;

    struct WaveField
    {
        // Rows of the world -> lattice matrix, already scaled by 1/cell. Row 0
        // projects onto the flow direction, row 1 across it. The pixel shader
        // needs nothing else: the vertex shader rotates the resulting slope back
        // into view space, so the wave normal follows the real gradient.
        float lattice[4] = {1.f, 0.f, 0.f, 1.f};
        // Lattice-space phase of each octave, wrapped into the lattice period.
        float phase[4] = {0.f, 0.f, 0.f, 0.f};
        // Drift of the client's own water detail texture, in its uv space.
        float uvDrift[2] = {0.f, 0.f};
        // The camera's own lattice coordinate, in cells, NOT wrapped: it feeds
        // sines whose period is a fraction of the hash period, so reducing it
        // would jump one octave against the other every 64 cells. It is a
        // per-draw constant, so its size costs no per-pixel precision, and the
        // vertex shader adds it back so the displaced crests are anchored to the
        // terrain as well - geometry waves driven from the camera-relative
        // coordinate alone slide across the surface whenever the camera moves.
        float anchor[2] = {0.f, 0.f};
    };

    // Lattice indices wrap at kLatticeWrap inside the shader, so shifting a
    // phase by any multiple of it cannot change the rendered field.
    inline float WrapLattice(float value)
    {
        return value - kLatticeWrap * std::floor(value / kLatticeWrap);
    }

    // `scalePercent` is the player-facing wave size knob (100 = shipped
    // default); `counter` is 0..1 and shears the long swell against the ripple
    // flow, which is what stops the surface reading as a still mirror with noise
    // on top. `animated` is false when waves are switched off.
    inline WaveField BuildWaveField(double timeSeconds, float flowSpeed, float counter,
                                    float scalePercent, float cameraX, float cameraY,
                                    bool cameraValid, bool animated)
    {
        WaveField field;
        const float cell = kRippleCellYards * (scalePercent > 1.f ? scalePercent : 100.f) * .01f;
        const double time = animated ? timeSeconds * flowSpeed : 0.0;
        const float angle = float(time * kLatticeRotation);
        const float ca = std::cos(angle), sa = std::sin(angle);
        const float inv = 1.f / cell;
        field.lattice[0] = ca * inv;
        field.lattice[1] = sa * inv;
        field.lattice[2] = -sa * inv;
        field.lattice[3] = ca * inv;

        // Travel is along the flow for the ripple layer and along a sheared
        // direction for the swell layer, so its contribution through row 1 is
        // the sine of the shear rather than zero.
        const float travel = float(time);
        const float spread = kCounterAngle * (counter > 0.f ? counter : 0.f);
        const float rippleDrift = kRippleDriftYards * travel * inv;
        const float swellDrift = kSwellDriftYards * travel * inv * kSwellRatio;
        const float cam0 = cameraValid ? cameraX * field.lattice[0] + cameraY * field.lattice[1] : 0.f;
        const float cam1 = cameraValid ? cameraX * field.lattice[2] + cameraY * field.lattice[3] : 0.f;
        field.phase[0] = WrapLattice(cam0 + rippleDrift);
        field.phase[1] = WrapLattice(cam1);
        field.phase[2] = WrapLattice(cam0 * kSwellRatio + swellDrift * std::cos(spread));
        field.phase[3] = WrapLattice(cam1 * kSwellRatio - swellDrift * std::sin(spread));
        field.anchor[0] = cameraValid ? cam0 : 0.f;
        field.anchor[1] = cameraValid ? cam1 : 0.f;

        // The client's sampler wraps at 1 in both axes, so an accumulated uv
        // creep can be reduced without moving the image.
        field.uvDrift[0] = animated ? travel * kUvDriftU - std::floor(travel * kUvDriftU) : 0.f;
        field.uvDrift[1] = animated ? travel * kUvDriftV - std::floor(travel * kUvDriftV) : 0.f;
        return field;
    }
}
