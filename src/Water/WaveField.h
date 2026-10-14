#pragma once
#include <algorithm>
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
    // double on the CPU, from the same draw's own matrix, so the two halves can
    // never disagree. The pixel shader wraps lattice indices modulo kLatticeWrap,
    // and every swell train wraps its own phase modulo its own wavelength, which
    // makes reducing that phase a free, invisible operation: the field stays
    // world-anchored while every coordinate stays small.
    //
    // Three scales share that one lattice:
    //
    //  * The long swell is a sum of ANALYTIC, DIRECTIONAL sine trains - not noise
    //    - because rounded forms with lit backs and shaded faces is what a real
    //    sea looks like, because a sine gives exact slopes for free, and because
    //    the vertex stage can then displace the very same waves the shading
    //    lights, so a crest and its highlight cannot drift apart. Each train
    //    carries its own direction, so the surface is a crossing sea rather than
    //    two perpendicular stripes (which is what a single axis-aligned pair reads
    //    as: wave curves drawn on a flat plane).
    //  * Fine chop on top of them stays procedural value noise, with the same
    //    bounded hash, drifting along the dominant wind.
    //  * The client's own detail map creeps in its uv, untouched by either.
    inline constexpr float kRippleCellYards = 2.5f;   // one ripple lattice cell
    inline constexpr float kLatticeWrap = 64.0f;      // must match noiseGradient()
    inline constexpr float kTwoPi = 6.2831853f;

    // The lattice orientation is FIXED. It used to be `time * kLatticeRotation`,
    // which turns the whole world->lattice matrix about the WORLD ORIGIN, and a
    // rotation about the origin is a translation of radius*omega everywhere else:
    // at ten thousand yards out, 0.035 rad/s dragged the entire wave field
    // sideways at 350 yards per second, and because the radius and its direction
    // both change as the player moves, the sea raced and swam with the camera.
    // The speed slider scaled it, but only down to "still far too fast": at 5% the
    // field still travelled at ~17 yards per second on its own. Direction variety
    // now comes from the trains below, each with its own direction, and the shear
    // knob advects them against one another. Nothing rotates.
    inline constexpr float kLatticeYaw = 0.62f;   // radians, world +X to lattice row 0

    // ---- the swell trains ---------------------------------------------------
    // One table, three readings of it: the vertex stage displaces the two long
    // ones (the client's liquid grid is ~4 yards apart, so a ten yard wave is the
    // shortest it can carry without faceting), the pixel stage shades all three
    // and wraps its phases against them, and this header advances them. Directions
    // are unit vectors in lattice space, wavelengths are in cells, weights are the
    // share of the authored amplitude each train carries. Both shaders spell the
    // same numbers as literals; tools/WaterRegression.cpp pins the spelling.
    inline constexpr int kTrainCount = 3;
    inline constexpr float kTrainDirX[kTrainCount] = {1.f, -.4695f, .4384f};   // 0 deg, 118 deg, -64 deg
    inline constexpr float kTrainDirY[kTrainCount] = {0.f, .8829f, -.8988f};
    inline constexpr float kTrainCells[kTrainCount] = {16.f, 8.f, 4.f};        // 40, 20 and 10 yards
    inline constexpr float kTrainWeight[kTrainCount] = {1.f, .5f, .26f};
    inline constexpr float kTrainRadians[kTrainCount] = {kTwoPi / 16.f, kTwoPi / 8.f, kTwoPi / 4.f};
    // The two trains the mesh is dense enough to carry. Everything above index 2
    // is normal-mapped only, exactly like a real renderer splits simulation from
    // detail: displacing a ten yard wave on a four yard grid is what facets.
    inline constexpr int kGeometryTrainCount = 2;

    // ---- how fast a wave of a given length should travel ---------------------
    // Deep-water dispersion: a gravity wave of wavelength L travels at
    // sqrt(g*L/2pi), so long waves outrun short ones and a sea does not look like
    // one pattern sliding along at a single speed. The previous numbers were the
    // other way round - the forty yard swell crawled at 0.8 yards/s (one crest
    // every fifty seconds, which is why the surface read as flat and dead) while
    // the 2.5 yard chop raced at 2.2 yards/s (one cell every second, which is what
    // read as "too fast"). Both are now the dispersion speed, scaled by one
    // authored factor: a real sea is faster than a third person camera reads as
    // water, and half of it is what the reference shots carry.
    inline constexpr float kGravityYards = 10.73f;    // 9.81 m/s^2 in yards
    inline constexpr float kPhaseSpeedScale = .44f;
    inline float PhaseSpeedYards(float wavelengthYards) noexcept
    {
        const float wavelength = std::isfinite(wavelengthYards) ? std::max(wavelengthYards, 0.f) : 0.f;
        return kPhaseSpeedScale * std::sqrt(kGravityYards * wavelength / kTwoPi);
    }

    // A cross-current, as a fraction of the shortest train's own speed, advecting
    // each train by the projection of its direction onto the current. The current
    // runs across the dominant wind (train 0), so the long swell keeps its clean
    // speed while the two shorter trains shear against it and against each other.
    // That is what stops the surface reading as a still mirror with noise on top,
    // and it is what [Water] FlowRotationPercent and the weather shear both drive.
    inline constexpr float kCrossCurrent = .55f;

    inline constexpr float kUvDriftU = .0019f;        // native detail map creep, uv/s
    inline constexpr float kUvDriftV = -.0011f;

    struct WaveField
    {
        // Rows of the world -> lattice matrix, already scaled by 1/cell. Row 0
        // projects onto the dominant wind direction, row 1 across it. Both shader
        // stages need nothing else: the vertex shader rotates the resulting slope
        // back into view space, so the wave normal follows the real gradient.
        float lattice[4] = {1.f, 0.f, 0.f, 1.f};
        // Lattice-space phase of the chop, in cells, wrapped into the noise
        // period. Both components are wrapped independently because noiseGradient
        // wraps both lattice indices independently.
        float ripplePhase[2] = {0.f, 0.f};
        // Phase of each swell train, in cells ALONG THAT TRAIN'S OWN DIRECTION and
        // wrapped into that train's own wavelength. Wrapping per train rather than
        // per lattice axis is what lets a directional wave share one small number
        // between the stages: a shift of one wavelength is one full turn of the
        // sine, so it cannot be seen, whatever the direction.
        float swellPhase[kTrainCount] = {0.f, 0.f, 0.f};
        // Drift of the client's own water detail texture, in its uv space.
        float uvDrift[2] = {0.f, 0.f};
        // Nothing else is needed: the camera's own coordinate is inside the phases,
        // and because every phase is reduced into a period the shader can share it
        // exactly rather than carrying an unwrapped anchor that would lose float32
        // precision at the far corners of a continent.
    };

    // Lattice indices wrap at kLatticeWrap inside the shader, so shifting a chop
    // phase by any multiple of it cannot change what either stage draws. The same
    // is true of a swell phase shifted by any multiple of its own wavelength.
    inline float WrapLattice(float value) noexcept
    {
        return value - kLatticeWrap * std::floor(value / kLatticeWrap);
    }

    inline double WrapPeriod(double value, double period) noexcept
    {
        return period > 0.0 ? value - period * std::floor(value / period) : 0.0;
    }

    // ---- the anchor: what the draw itself says about the camera ----------------
    // The wave field is world-anchored by adding the camera's own coordinate to the
    // camera-relative one the vertex stage computes. Both halves have to come from the
    // SAME matrix or the field slides: a frame-captured camera is a frame behind (and is
    // the player's camera even while the client renders water into its own reflection
    // target), which is exactly how the sea used to swim when the player moved. So the
    // caller reads the draw's own model->view constants - c5..c8, four rows, row 3 the
    // translation - and this turns them into the two things the field needs.
    struct DrawAnchor
    {
        float camera[2] = {0.f, 0.f};  // camera position in the matrix's own (world) axes
        float up[3] = {0.f, 0.f, 1.f}; // world +Z in view space: this draw's idea of up
        bool valid = false;
    };

    inline DrawAnchor AnchorFromModelView(const float modelView[4][4]) noexcept
    {
        DrawAnchor out;
        // The array is the client's own row-vector matrix: the stage builds
        // p as sum_j modelView[j]*pos[j], so world axis i arrives in view space as row i
        // and the camera is the point that lands on the view origin, -dot(row i,
        // translation). Reading the components ACROSS the rows instead yields the view
        // axes expressed in world coordinates, and dotting those with a view-space
        // position applies the camera's rotation twice over - which is how a field that
        // was meant to be nailed to the terrain turned when the camera turned.
        //
        // Orthonormality is not a sanity check bolted on afterwards: the pairing the
        // caller relies on (a view-space position dotted with row i gives the world
        // offset's component i) is only true of a rigid basis. A scaled or sheared
        // matrix would distort the field even if a camera could be read out of it, so
        // refusing it here is what lets the caller fall back instead.
        for (int axis = 0; axis < 3; ++axis)
        {
            double length = 0.0;
            for (int component = 0; component < 3; ++component)
            {
                const float value = modelView[axis][component];
                if (!std::isfinite(value)) return out;
                length += double(value) * double(value);
            }
            if (!(length > .98 && length < 1.02)) return out;
            for (int other = 0; other < axis; ++other)
            {
                double dot = 0.0;
                for (int component = 0; component < 3; ++component)
                    dot += double(modelView[axis][component]) * double(modelView[other][component]);
                if (!(dot > -.02 && dot < .02)) return out;
            }
        }
        for (int component = 0; component < 3; ++component)
            if (!std::isfinite(modelView[3][component])) return out;
        // In double: a world coordinate and a translation of the same size multiplied in
        // float32 lose the last yards of a continent, and the field is anchored to the yard.
        for (int axis = 0; axis < 2; ++axis)
        {
            const double anchored = -(double(modelView[axis][0]) * double(modelView[3][0])
                + double(modelView[axis][1]) * double(modelView[3][1])
                + double(modelView[axis][2]) * double(modelView[3][2]));
            out.camera[axis] = float(anchored);
            if (!std::isfinite(out.camera[axis])) return out;
        }
        for (int component = 0; component < 3; ++component) out.up[component] = modelView[2][component];
        out.valid = true;
        return out;
    }

    // `scalePercent` is the player-facing wave size knob (100 = shipped default);
    // `counter` is 0..1 and drives the cross-current above; `animated` is false
    // when waves are switched off. `cameraX/cameraY` must be the camera position in
    // the SAME space the vertex shader's own matrix maps from - the caller reads it
    // from this draw's model->view matrix, not from a frame-captured camera, so the
    // two halves of the anchor can never be a frame or a camera apart.
    inline WaveField BuildWaveField(double timeSeconds, float flowSpeed, float counter,
                                    float scalePercent, float cameraX, float cameraY,
                                    bool cameraValid, bool animated) noexcept
    {
        WaveField field;
        const float percent = scalePercent > 1.f ? scalePercent : 100.f;
        const double cell = double(kRippleCellYards) * double(percent) * .01;
        const double time = animated ? timeSeconds * double(std::max(flowSpeed, 0.f)) : 0.0;
        const float ca = std::cos(kLatticeYaw), sa = std::sin(kLatticeYaw);
        const float inv = float(1.0 / cell);
        field.lattice[0] = ca * inv;
        field.lattice[1] = sa * inv;
        field.lattice[2] = -sa * inv;
        field.lattice[3] = ca * inv;

        // The camera's own lattice coordinate, reduced in double. Everything below
        // adds to it, so the shader only ever sees a coordinate relative to the
        // camera plus a wrapped phase: small, exact, and identical for two
        // neighbouring tiles of the same body of water.
        const double cam0 = cameraValid ? double(cameraX) * field.lattice[0] + double(cameraY) * field.lattice[1] : 0.0;
        const double cam1 = cameraValid ? double(cameraX) * field.lattice[2] + double(cameraY) * field.lattice[3] : 0.0;

        // Speeds in cells per second. Dividing the dispersion speed by the cell is
        // what makes a body of water authored twice the size carry waves that
        // travel faster rather than waves that merely look bigger: the wavelength
        // grows with the cell, so its speed grows with the square root of it.
        const auto cellsPerSecond = [cell](float wavelengthCells) {
            return double(PhaseSpeedYards(float(double(wavelengthCells) * cell))) / cell;
        };
        const double shear = std::clamp(counter > 0.f ? counter : 0.f, 0.f, 1.f) * double(kCrossCurrent)
            * cellsPerSecond(kTrainCells[kTrainCount - 1]);
        for (int train = 0; train < kTrainCount; ++train)
        {
            const double along = cam0 * kTrainDirX[train] + cam1 * kTrainDirY[train]
                + time * (cellsPerSecond(kTrainCells[train]) + shear * kTrainDirY[train]);
            field.swellPhase[train] = float(WrapPeriod(along, double(kTrainCells[train])));
        }
        // The chop drifts along the dominant wind and is carried sideways by the
        // same current, so a sheared sea crosses its own ripple field too.
        field.ripplePhase[0] = WrapLattice(float(cam0 + time * cellsPerSecond(1.f)));
        field.ripplePhase[1] = WrapLattice(float(cam1 + time * shear));

        // The client's sampler wraps at 1 in both axes, so an accumulated uv
        // creep can be reduced without moving the image.
        const double creep = time;
        field.uvDrift[0] = animated ? float(creep * kUvDriftU - std::floor(creep * kUvDriftU)) : 0.f;
        field.uvDrift[1] = animated ? float(creep * kUvDriftV - std::floor(creep * kUvDriftV)) : 0.f;
        return field;
    }

    // ---- the swell both stages evaluate --------------------------------------
    struct SwellSample
    {
        float height = 0.f;             // sum of weight*sin, in units of the authored amplitude
        float slope[2] = {0.f, 0.f};    // d(height)/d(lattice cell), in lattice axes
        float pinch = 0.f;              // sum of weight*k*sin: times sigma this is 1 - the Jacobian
    };

    // The largest value `pinch` can reach, which is what makes the fold-safe clamp
    // below a statement about the table rather than a guess.
    inline constexpr float kMaxPinch = kTrainWeight[0] * kTrainRadians[0]
        + kTrainWeight[1] * kTrainRadians[1] + kTrainWeight[2] * kTrainRadians[2];

    // The largest |height| the table can reach is the sum of its weights, so this divisor
    // maps a swell sample into -0.5..0.5 and a crest-reading `swell01` into 0..1 with calm
    // water exactly at 0.5. Both stages multiply by it, so a whitecap threshold means the
    // same thing at any wave size and any sea state.
    inline constexpr float kSwellToUnit = 1.f / (2.f * (kTrainWeight[0] + kTrainWeight[1] + kTrainWeight[2]));

    inline SwellSample EvaluateSwell(float cellX, float cellY, const float phase[kTrainCount]) noexcept
    {
        SwellSample out;
        for (int train = 0; train < kTrainCount; ++train)
        {
            const float theta = kTrainRadians[train]
                * (kTrainDirX[train] * cellX + kTrainDirY[train] * cellY + phase[train]);
            const float sine = std::sin(theta), cosine = std::cos(theta);
            out.height += kTrainWeight[train] * sine;
            const float slope = kTrainWeight[train] * kTrainRadians[train] * cosine;
            out.slope[0] += slope * kTrainDirX[train];
            out.slope[1] += slope * kTrainDirY[train];
            out.pinch += kTrainWeight[train] * kTrainRadians[train] * sine;
        }
        return out;
    }

    // ---- geometry waves ------------------------------------------------------
    // A Gerstner surface: each train lifts the water by weight*amplitude*sin and
    // slides it toward the coming crest by weight*lateral*cos. The slide is what
    // makes crests pinch and troughs flatten, which is the whole difference between
    // a shaded picture of a wave and water that reads as a body moving in three
    // dimensions. `sigma` is the dimensionless steepness both stages share
    // (lateral/cell); the surface folds - crests tear into loops - once
    // sigma*kMaxPinch reaches 1, so it is clamped well below that and the clamp,
    // not the shader, is what guarantees a wave can never turn inside out.
    inline constexpr float kGeometryWaveYards = .75f;  // WAVE HEIGHT 100 at the shipped wave size
    inline constexpr float kGerstnerSigma = .45f;      // the shipped crest shape
    inline constexpr float kGerstnerSigmaMax = .55f;   // fold safety: Jacobian never below ~.34

    struct GeometryWaves
    {
        float amplitude = 0.f;   // yards, peak lift of the unit-weight train
        float lateral = 0.f;     // yards, peak slide toward a crest (Gerstner Q * amplitude)
        float sigma = 0.f;       // lateral / cell: the pinch coefficient the pixel stage divides by
        float cellYards = kRippleCellYards;
    };

    // `displace` is the geometry-wave toggle, `animated` the wave toggle. The pinch is a
    // property of the SEA rather than of the mesh: it sharpens the shading normal and
    // drives the whitecap mask on a body whose geometry waves are switched off too, and it
    // is zero only when the waves themselves are off.
    inline GeometryWaves BuildGeometryWaves(float heightPercent, float scalePercent,
                                            float agitation, bool displace, bool animated) noexcept
    {
        GeometryWaves out;
        const float percent = scalePercent > 1.f ? scalePercent : 100.f;
        out.cellYards = kRippleCellYards * percent * .01f;
        if (!animated) return out;
        out.sigma = std::min(kGerstnerSigma, kGerstnerSigmaMax);
        if (!displace) return out;
        const float asked = std::clamp(std::isfinite(heightPercent) ? heightPercent : 0.f, 0.f, 300.f) * .01f;
        if (asked <= 0.f) return out;
        // Amplitude grows with the square root of the wave size: a body authored twice the
        // size carries taller waves, but not twice as tall, and the crest shape stays the
        // one sigma authors because sigma is scale free. The sea state multiplies it, so a
        // squall lifts the same coast rather than inventing a different one, and the cap
        // keeps an absurd combination of sliders from lifting water over a harbour wall.
        const float size = std::sqrt(std::clamp(percent * .01f, .25f, 4.f));
        const float sea = std::clamp(std::isfinite(agitation) ? agitation : 1.f, .12f, 2.6f);
        out.amplitude = std::min(asked * kGeometryWaveYards * size * sea, 1.2f * out.cellYards);
        out.lateral = out.sigma * out.cellYards;
        return out;
    }
}
