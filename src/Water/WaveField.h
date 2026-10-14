#pragma once
#include <cmath>

namespace renderer::water
{
    // Everything in this header keeps the water's procedural wave field
    // anchored to the terrain instead of to a liquid tile's material uv, and
    // keeps the numbers the shader evaluates small. Both halves matter:
    //
    //  * A field built from per-tile uv repeats identically in every tile, so
    //    wave detail was framed by hard square edges and restarted at the seams.
    //  * A field built from raw world coordinates multiplies values in the
    //    hundreds or thousands by a hash constant. That exhausts the fraction
    //    bits of float32, the noise collapses onto its own lattice and hardens
    //    into flat axis-aligned squares.
    //
    // So the shaders evaluate the lattice from the position RELATIVE TO THE
    // CAMERA (small and exact, recovered in the vertex shader from the draw's own
    // view matrix) and add the camera's own contribution as a phase computed in
    // double on the CPU. The pixel shader wraps lattice indices modulo
    // kLatticeWrap, which makes reducing that phase into the lattice period a
    // free, invisible operation: the field stays world-anchored while every
    // coordinate stays small.
    //
    // Two scales share that one lattice. The long swell is a pair of ANALYTIC
    // sine trains - not noise - because rounded forms with lit backs and shaded
    // faces is what a real sea looks like, because a sine gives exact slopes for
    // free, and because the vertex stage can then displace the very same waves
    // the shading lights, so a crest and its highlight cannot drift apart. The
    // trains' periods (kSwellCells and half of it) divide kLatticeWrap exactly,
    // which is what makes wrapping the swell phase harmless. Fine chop on top of
    // them stays procedural noise, with the same bounded hash.
    inline constexpr float kRippleCellYards = 2.5f;   // one ripple lattice cell
    inline constexpr float kLatticeWrap = 64.0f;      // must match noiseGradient()
    inline constexpr float kSwellCells = 16.0f;        // long train: 16 cells
    inline constexpr float kSwellShortCells = 8.0f;    // secondary train: half of it
    inline constexpr float kSwellRadians = 6.2831853f / kSwellCells;
    inline constexpr float kLatticeRotation = 0.035f; // radians per second of flow
    inline constexpr float kRippleDriftYards = 2.2f;  // ripple travel, yards per second
    inline constexpr float kSwellDriftYards = .8f;    // long swell travel
    inline constexpr float kCounterAngle = 1.9f;      // full counter-current spread
    inline constexpr float kUvDriftU = .0019f;        // native detail map creep, uv/s
    inline constexpr float kUvDriftV = -.0011f;

    struct WaveField
    {
        // Rows of the world -> lattice matrix, already scaled by 1/cell. Row 0
        // projects onto the flow direction, row 1 across it. Both shader stages
        // need nothing else: the vertex shader rotates the resulting slope back
        // into view space, so the wave normal follows the real gradient.
        float lattice[4] = {1.f, 0.f, 0.f, 1.f};
        // Lattice-space phase of each octave, in cells and wrapped into the
        // lattice period: xy=ripple, zw=long swell. The swell phase stays in
        // RIPPLE cells - each stage scales it by its own wave frequency - so one
        // number feeds the shading, the displacement and the foam masks.
        float phase[4] = {0.f, 0.f, 0.f, 0.f};
        // Drift of the client's own water detail texture, in its uv space.
        float uvDrift[2] = {0.f, 0.f};
        // Nothing else is needed: the camera's own coordinate is inside `phase`, and
        // because every train's period divides kLatticeWrap, the vertex stage can share
        // the wrapped phase exactly rather than carrying an unwrapped anchor that would
        // lose float32 precision at the far corners of a continent.
    };

    // Lattice indices wrap at kLatticeWrap inside the shader, and both swell
    // trains divide that wrap, so shifting a phase by any multiple of it cannot
    // change what either stage draws.
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
        const float swellDrift = kSwellDriftYards * travel * inv;
        const float cam0 = cameraValid ? cameraX * field.lattice[0] + cameraY * field.lattice[1] : 0.f;
        const float cam1 = cameraValid ? cameraX * field.lattice[2] + cameraY * field.lattice[3] : 0.f;
        field.phase[0] = WrapLattice(cam0 + rippleDrift);
        field.phase[1] = WrapLattice(cam1);
        field.phase[2] = WrapLattice(cam0 + swellDrift * std::cos(spread));
        field.phase[3] = WrapLattice(cam1 - swellDrift * std::sin(spread));

        // The client's sampler wraps at 1 in both axes, so an accumulated uv
        // creep can be reduced without moving the image.
        field.uvDrift[0] = animated ? travel * kUvDriftU - std::floor(travel * kUvDriftU) : 0.f;
        field.uvDrift[1] = animated ? travel * kUvDriftV - std::floor(travel * kUvDriftV) : 0.f;
        return field;
    }
}
