#pragma once
// The grass the client draws as a blade card, and the wind that bends it.
//
// Every other part of this feature leans a model by turning its bones, which
// works for everything the client skins: bushes, plants, tree canopies, a merged
// patch of grass the client filled with one bone per tuft. A field's ground
// clutter is not one of those. The client bakes it into its own meshes as static
// vertices with no bone indices at all - a draw this renderer's model path
// refuses by its declaration, correctly, because no palette can bend it - and
// animates the blades in the vertex shader if it animates them at all. That is
// the report this file exists to answer: a player standing in a field of grass,
// `0 leaning` on the HUD, and nothing they can see moving.
//
// There is exactly one way to bend a static blade: make its vertex shader move
// it. The reference for the method is WarcraftXL's `wxl-grasswind`
// (github.com/WarcraftXL/wxl-grasswind, GPL-3.0): the client's detail-doodad
// vertex shader is disassembled, a motion block is injected into the middle of
// it and the result is reassembled and swapped in. The block below is this
// project's own, written against what this renderer can measure - the frame
// camera it already tracks, the wind it already simulates, and the draw's own
// declaration and vertices - and its constants ride registers 14..19, the free
// block the engine's per-chunk upload leaves alone (c0..c13 carry the matrices
// the blade shader needs, and nothing of the engine's own lives above them).
//
// Everything here is a pure function over text, numbers and bytes: the shader
// text is a string, the constants are float4 rows, and the disassembly is the
// disassembler's own output with one block inserted into it. That is what lets
// the portable suite compile a garden of shader texts - one with no anchor, one
// that already uses the scratch registers, one whose blade runs the other way up
// - and prove what the injection does to each, without a device.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace renderer
{
    // How the blades move. The numbers are the reference module's own
    // (wxl-grasswind's WindSettings defaults), raised where a player has to SEE
    // the movement from twenty yards away: the primary wave is a tip sway of
    // about five inches at reference strength, which is the same order as the
    // lean the model path applies to a plant of the same size.
    struct GrassBladeSettings
    {
        bool enabled = true;              // [Vegetation] GrassBlades
        // The shipped motion is what a field has to READ as when a player stands
        // in it. A real session's own log - `blade wind: ... yd at the tip` - is
        // the measurement behind these numbers: the 372 byte blade shader was
        // bent on every one of its draws and the tip moved 0.15 to 0.17 yards,
        // over a wave eighteen yards long travelling at three yards a second,
        // which is a whole field drifting a hand's width once every six seconds.
        // That is why the HUD could report a shader bent while the field stood
        // still: the bend was real, and too small and too slow to see. The
        // defaults now move a blade's tip about a foot at reference strength, on
        // a wave five yards long travelling downwind at about two and a half
        // yards a second - a crest every two seconds, close to the reference
        // implementation's own defaults, and readable from a walking pace.
        float tipAmplitudeYards = .35f;   // sway at the tip of a blade, yards, at strength 1
        float wavelengthYards = 5.f;      // the wave the field travels on, yards
        float crossWavelengthYards = 2.4f;// its cross-swell, which is what makes it read as grass
        float crossAmplitudeRatio = .33f; // the cross-swell's share of the primary amplitude
        float crossAngleDegrees = 35.f;   // and its heading, off the wind
        float waveSpeedYardsPerSecond = 2.4f; // how fast the crests travel downwind
        float leanFraction = .35f;        // a constant downwind lean, as a share of the amplitude
        float variance = .6f;             // per-blade amplitude spread, 0 = every blade alike
        float jitterRadians = 1.7f;       // per-blade phase spread, radians
        float anchor = .3f;               // the share of a blade, from its base, that never moves
    };

    // What the blade path is doing, for the optional debug HUD and the log. The
    // per-frame numbers are reset every frame by the device half; the session
    // numbers count shaders, because a shader is bent once and drawn for as long
    // as the client keeps it.
    struct GrassBladeStatus
    {
        unsigned bladeDraws = 0;     // draws this frame that went out with the wind (blades)
        unsigned uploads = 0;        // blocks written this frame (one per frame in practice)
        unsigned heightDraws = 0;    // of those, the cards bent by the vertex's own height
        unsigned foliageDraws = 0;   // unskinned foliage draws offered to this path
        unsigned materialRefused = 0;// of those, the ones whose texture was not a green cut-out
        unsigned noTexcoord = 0;     // refused: neither a texture coordinate nor a position to
                                     // read a height from, so the card has nothing to bend by
        unsigned registerMismatch = 0;// draws whose coordinate is not where the shader reads it
        unsigned neverModel = 0;     // refused: a pass this project knows is not a model (the interface!)
        // The blade path never refuses a shader for binding the world's own
        // projection: an unskinned card is drawn through the world path by
        // construction (its placement IS an object -> world matrix, which is why
        // the client's grass shader reads c4..c7), so a WMO/terrain-shaped
        // constant binding says nothing about a draw that has no bones to begin
        // with. The counter is what says how many bent shaders were that shape.
        unsigned worldShaped = 0;    // bent anyway: its constants bind the world projection
        unsigned cardRefused = 0;    // refused: the draw's own vertices carry no blade (no height, no pair)
        unsigned cardUnreadable = 0; // of those offered, the ones whose vertices could not be read at all
        unsigned examined = 0;       // shaders this build has looked at, this session
        unsigned injected = 0;       // shaders it is bending, this session
        unsigned refused = 0;        // shaders it left alone, this session
        float amplitudeYards = 0.f;  // sway at a blade's tip this frame, yards
    };

    // The block lives in six constant registers: one row each, every component of
    // them accounted for below, written once per frame and re-uploaded whenever
    // the client writes over them again. c14..c19 is where it goes by default, and
    // where it goes for every shader that never reads those registers; a shader
    // that does read them (a real session's 956 and 1048 byte shaders read c17 and
    // c18 as their lighting) gets the first free run above them instead - see
    // GrassBladeFreeConstantRun. `GrassBladeMotionBlock` renumbers its rows to
    // whatever base it is given, so nothing else here has to know.
    inline constexpr unsigned kGrassBladeFirstRegister = 14;
    inline constexpr unsigned kGrassBladeRows = 6;

    // What one frame's constants are built from. Filled by the device half from
    // the frame's own wind and camera; separate so the packing below is a pure
    // function the suite can pin.
    struct GrassBladeFrame
    {
        // The wind, in the world plane. `direction` is a unit travel vector,
        // `strength` the shared wind's 0..3 (1 = reference breeze).
        float directionX = 1.f, directionY = 0.f, strength = 1.f;
        // The world position of the frame's camera. The wave field and the
        // per-blade phase are built from WORLD positions, so a blade keeps its
        // phase while the camera moves - a field whose crests followed the camera
        // would look like a curtain, not like wind.
        float cameraX = 0.f, cameraY = 0.f;
        // The accumulated phase of each wave, in radians, wrapped to [0, 2pi) by
        // the caller: cos(k * dot(p, dir) + phase) with phase falling with time.
        float phasePrimary = 0.f, phaseCross = 0.f;
        // The range the sway fades out over, yards (the vegetation DistanceYards
        // setting), and one blade's own anchor and variance.
        float distanceYards = 120.f;
        GrassBladeSettings settings{};
    };

    // One row of the block, as the device half uploads it.
    struct GrassBladeConstants
    {
        float rows[kGrassBladeRows][4]{};
    };

    // row 0 (c14 at the default base) = { cameraX, cameraY, 1.0, lean }
    // c15 = { primary  dir * k, primary  dir * amplitude }
    // c16 = { crosst    dir * k, cross    dir * amplitude }
    // c17 = { phasePrimary, phaseCross, jitter, -1/distance }
    // c18 = { bendScale, -anchor, varianceCentre, varianceSpread }
    // c19 = { bladeVScale, bladeVOffset, hashX, hashY }
    //
    // The two rows per wave carry the same heading twice: `dir * k` is what the
    // phase is built from (dot(worldPosition, dir * k)), and `dir * amplitude` is
    // what the resulting cosine is scaled into. The amplitude is folded into the
    // direction rather than carried separately so that a wave costs two rows
    // total, which is what keeps the whole block inside the registers the engine
    // actually leaves free.
    //
    // The bend weight is a straight ramp along the blade (0 at the base, 1 at
    // the tip) with a share of the base held rigid, both in one mad: the ramp's
    // own input is built as `v * vScale + vOffset`, which is also how the block
    // copes with a blade whose texture runs the other way up - the caller passes
    // vScale/vOffset from the sign its own vertices measured, and a blade whose
    // v is 1 at the tip is bent by the same code.
    inline GrassBladeConstants PackGrassBladeConstants(const GrassBladeFrame& frame, int bladeVSign) noexcept
    {
        GrassBladeConstants out{};
        const GrassBladeSettings& settings = frame.settings;

        // Amplitude: the wind's own strength, the player's SwayPercent, and the
        // amplitude setting, all in yards at the tip. A calm frame is a still
        // field, which is what every other part of this feature does too.
        const float strength = std::clamp(frame.strength, 0.f, 3.f);
        const float amplitude = std::max(0.f, settings.tipAmplitudeYards) * strength;
        const float primaryK = 6.2831853f / std::max(.5f, settings.wavelengthYards);
        const float crossK = 6.2831853f / std::max(.5f, settings.crossWavelengthYards);
        const float crossAmplitude = amplitude * std::clamp(settings.crossAmplitudeRatio, 0.f, 1.f);

        // The cross-swell's heading: the wind's own, turned by the setting.
        const float crossRadians = std::clamp(settings.crossAngleDegrees, -180.f, 180.f) * 0.0174532925f;
        const float crossCos = std::cos(crossRadians), crossSin = std::sin(crossRadians);
        float crossX = frame.directionX * crossCos - frame.directionY * crossSin;
        float crossY = frame.directionX * crossSin + frame.directionY * crossCos;

        float* c14 = out.rows[0];
        float* c15 = out.rows[1];
        float* c16 = out.rows[2];
        float* c17 = out.rows[3];
        float* c18 = out.rows[4];
        float* c19 = out.rows[5];

        c14[0] = frame.cameraX;
        c14[1] = frame.cameraY;
        c14[2] = 1.f;
        c14[3] = std::clamp(settings.leanFraction, -.9f, .9f);

        c15[0] = frame.directionX * primaryK;
        c15[1] = frame.directionY * primaryK;
        c15[2] = frame.directionX * amplitude;
        c15[3] = frame.directionY * amplitude;

        c16[0] = crossX * crossK;
        c16[1] = crossY * crossK;
        c16[2] = crossX * crossAmplitude;
        c16[3] = crossY * crossAmplitude;

        c17[0] = frame.phasePrimary;
        c17[1] = frame.phaseCross;
        c17[2] = std::max(0.f, settings.jitterRadians);
        // Negative on purpose: the block computes 1 + viewDepth * c17.w in one
        // mad, so the fade arrives as saturate(1 - depth/distance).
        c17[3] = -1.f / std::max(1.f, frame.distanceYards);

        // The ramp, as the one multiply the block can clamp: the input it is
        // given (v * vScale + vOffset, which is 0 at the blade's base and 1 at
        // its tip either way up) has the rigid share subtracted from it, and
        // `mul_sat` by the anchor's own scale turns that into 0 for the first
        // `anchor` share of the blade and 1 at its tip. Both numbers are divided
        // out here so the shader never has to.
        const float anchor = std::clamp(settings.anchor, 0.f, .9f);
        c18[0] = 1.f / (1.f - anchor);
        c18[1] = -anchor;
        const float variance = std::clamp(settings.variance, 0.f, 1.f);
        c18[2] = 1.f - variance * .5f;  // every blade's amplitude is this ...
        c18[3] = variance;              // ... plus this much of its own random

        // Which way the blade's own texture runs. -1/1 is the reference client's
        // own convention (v = 1 at the base, 0 at the tip) and what is used when
        // the draw's vertices say nothing either way.
        c19[0] = bladeVSign > 0 ? 1.f : -1.f;
        c19[1] = bladeVSign > 0 ? 0.f : 1.f;
        // The basis a blade's own random is hashed out of its world position with.
        // Two incommensurate numbers (the golden ratio and its complement) so that
        // blades sharing an X do not share a phase and the field does not read as
        // vertical stripes; they are constants of the block rather than settings,
        // because the hash has to keep working when every setting is at zero.
        c19[2] = .618034f;
        c19[3] = .381966f;
        return out;
    }

    // One wave's phase, advanced by a frame. The crests travel downwind at the
    // wave speed, so the phase falls: the block adds it to k * dot(position,
    // direction), which is what makes the whole field ripple rather than pulse.
    // Wrapped every frame, because a shader's sine is only accurate for the
    // argument it is given and an hour of wind is 7500 radians of it.
    inline float GrassBladePhase(float phase, float yardsPerSecond, float wavelengthYards, float deltaSeconds) noexcept
    {
        const float k = 6.2831853f / std::max(.5f, wavelengthYards);
        const float omega = k * std::max(0.f, yardsPerSecond);
        const float next = phase - omega * std::max(0.f, deltaSeconds);
        const float wrapped = std::fmod(next, 6.2831853f);
        return wrapped < 0.f ? wrapped + 6.2831853f : wrapped;
    }

    // What a draw's own vertices say about the card they carry: which way up its
    // texture runs, how tall the sampled vertices are, and how many pairs of them
    // looked like one blade's base and its tip. The sign is what the block is
    // told; the other two are what keeps a shader from being rewritten for a
    // card that is not a blade at all - a ground decal, a shadow blob, a quad
    // lying flat - where the block's bend weight (the texture's own v, measured
    // along the blade) would translate the whole card instead of bending it.
    struct GrassBladeCard
    {
        int sign = 0;              // -1: v = 1 at the base (the reference client); +1: the other way up
        float extentYards = 0.f;   // how tall the sampled vertices are, in yards
        unsigned pairs = 0;        // sampled pairs that looked like a base and its tip
        // The card's own lowest vertex, in the frame its vertices are stored in.
        // A card that carries a texture coordinate does not need it - its bend
        // weight is that coordinate, and the device half reads it relative to the
        // draw's own floor. A card that carries none is bent by its height, which
        // is the stored value itself, so the block is told where the floor is.
        float baseYards = 0.f;
        // False when the draw's vertices could not be read at all: a zero extent
        // and no pair then mean "no measurement", not "a flat card", and the two
        // must not be refused for the same reason.
        bool measured = false;
    };

    // The smallest card this path will bend, the largest, and the fewest pairs it
    // needs to have voted with. A blade a few inches tall is a blade; a decal
    // lying on the ground has no height to bend and never shows v rising towards
    // its tip. The ceiling is what keeps a TREE out of this path: a real session's
    // log shows it bending a 22.91 yd card with ten pairs in it - a canopy, or a
    // piece of scenery carrying a foliage texture - which is the "parts of the
    // buildings sway" a player reports, and at that size a sway of a blade's
    // amplitude is an invisible bend while the card's whole silhouette moves.
    // Grass tufts measure well under a yard to about two; three yards leaves room
    // for a big tuft and nothing else.
    inline constexpr float kGrassBladeMinExtentYards = .05f;
    inline constexpr float kGrassBladeMaxExtentYards = 3.f;
    inline constexpr unsigned kGrassBladeMinPairs = 1;

    // What the draw's own vertices say about which way a blade's texture runs,
    // as a sign: +1 when v rises with height (v = 0 at the blade's base), -1 when
    // it falls (v = 1 at the base, the reference client's convention), 0 when the
    // vertices do not say. `samples` is an array of {u, v, height above the
    // draw's own lowest vertex}, in the order the draw's vertices appear.
    //
    // Consecutive vertices of a blade card are its base and its tip, so the
    // sign of (v difference * height difference) over most of the pairs is the
    // convention - and a majority vote over a couple of dozen pairs is what
    // makes this a measurement rather than a guess, because one pair can be two
    // vertices of the same edge, or the last card's tip next to the next card's
    // base, where the same sign still holds on level ground and nothing at all
    // holds on a slope.
    inline GrassBladeCard MeasureGrassBladeCard(const float (*samples)[3], unsigned count) noexcept
    {
        GrassBladeCard card;
        if (!samples || count < 2) return card;
        card.measured = true;
        float lowest = samples[0][2], highest = samples[0][2];
        for (unsigned i = 1; i < count; ++i)
        {
            if (!std::isfinite(samples[i][2])) continue;
            lowest = (std::min)(lowest, samples[i][2]);
            highest = (std::max)(highest, samples[i][2]);
        }
        card.extentYards = std::isfinite(lowest) && std::isfinite(highest) ? highest - lowest : 0.f;
        unsigned rising = 0, falling = 0;
        for (unsigned i = 1; i < count; ++i)
        {
            const float deltaV = samples[i][1] - samples[i - 1][1];
            const float deltaHeight = samples[i][2] - samples[i - 1][2];
            // A blade is a few inches to a couple of yards from base to tip; a
            // pair that differs by less than a fifth of the texture's v, or by
            // more than a tree's height, is not one blade's base and tip.
            if (std::fabs(deltaV) < .2f || std::fabs(deltaHeight) < .02f || std::fabs(deltaHeight) > 4.f)
                continue;
            ++card.pairs;
            if (deltaV * deltaHeight < 0.f) ++falling;
            else ++rising;
        }
        if (falling == rising) return card;
        card.sign = falling > rising ? -1 : 1;
        return card;
    }

    // The same card, measured by its own height, for the family a real session
    // offers a million times over: unskinned cards whose declaration carries
    // POSITION and NORMAL and NO texture coordinate at all, whose stage-0 texture
    // measures as a cut-out - foliage drawn with nothing to read a blade's v from.
    // Refusing them left the field still while the log said a shader was bent, so
    // the weight comes from the vertex itself: a blade's height above its own base
    // IS its position in the ramp, and `heights` is that stored value, one per
    // sampled vertex, in the units the draw keeps them in (yards).
    //
    // What a height ramp loses, and why the other measurement stays first: there
    // is no v to vote with, so the up/down question cannot be asked - a height
    // always runs base to tip, which is why `sign` is +1 here and the block is
    // given the base offset instead. The pairs that prove a card is a blade of
    // some kind are the consecutive vertices that step up the blade, which is the
    // same requirement the v measurement makes (a decal lying flat, or a single
    // quad whose vertices all sit at one height, has none).
    inline GrassBladeCard MeasureGrassBladeCardByHeight(const float* heights, unsigned count) noexcept
    {
        GrassBladeCard card;
        if (!heights || count < 4) return card;
        float lowest = 0.f, highest = 0.f;
        unsigned finite = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            if (!std::isfinite(heights[i])) continue;
            if (!finite) lowest = highest = heights[i];
            ++finite;
            lowest = (std::min)(lowest, heights[i]);
            highest = (std::max)(highest, heights[i]);
        }
        if (finite < 4) return card;
        card.measured = true;
        card.baseYards = lowest;
        card.extentYards = highest - lowest;
        card.sign = 1; // a height ramp runs base to tip by definition
        for (unsigned i = 1; i < count; ++i)
        {
            const float step = std::fabs(heights[i] - heights[i - 1]);
            if (step < .02f || step > 4.f) continue;
            ++card.pairs;
        }
        return card;
    }

    // Just the sign, for the callers and the suite that only need that.
    inline int GrassBladeVerticalSign(const float (*samples)[3], unsigned count) noexcept
    {
        return MeasureGrassBladeCard(samples, count).sign;
    }

    // -----------------------------------------------------------------------
    // The injection itself
    // -----------------------------------------------------------------------
    // The motion block, as HLSL assembly. It is inserted directly after the line
    // that lands the vertex in view space (`add r0, r0, c3`: r0 holds the view
    // position once the engine's own rotation has been applied and the view
    // translation added), which is where a blade's position is known and nothing
    // has been projected yet. From there it is arithmetic the reference module
    // established and this project's own geometry can confirm:
    //
    //   * the world position of the vertex comes back from the view position by
    //     the engine's own modelview rows (c0/c1/c2 hold the world axes for a
    //     batch that is drawn unrotated, which is what a baked doodad batch is),
    //     plus the camera position this renderer passes in c14;
    //   * a blade's own random phase is a hash of its world position (`c19.z`/`w`),
    //     so it keeps its identity while the camera moves and never shimmers frame
    //     to frame;
    //   * the bend weight is the blade's texture v, held rigid at the base and
    //     squared towards the tip, faded with the distance from the camera, and
    //     varied from blade to blade;
    //   * the displacement is two travelling sine waves in the world plane - the
    //     wind's own heading and a cross-swell - plus the setting's constant
    //     downwind lean, and it is rotated back into view space by the same two
    //     modelview rows before it is added to r0.
    //
    // r0 is the shader's position and r1 is the engine's own scratch; the block
    // uses r2..r5 and NOTHING else, which is why the injection refuses a shader
    // whose own value still lives in one of them across the insertion point (see
    // GrassBladeScratchLive below). It carries no immediate float operands at all:
    // every scalar it reads is a component of c14..c19, which the caller writes
    // itself, so the text has nothing in it the two versions of the compiler could
    // read differently.
    //
    // `texcoord` is the v register of the blade's texture coordinate, taken from
    // the shader's own declaration line rather than assumed: a client that
    // declares its texcoord in v2 instead of v3 must still have its grass bend.
    // `worldSpace` says which frame r0 is in when the block runs, and the block is
    // built for that frame:
    //
    //   * false - the reference client's own blade shader: r0 is the VIEW position
    //     (`add r0, r0, c3` landed it), and the world position comes back through
    //     the shader's own modelview rows c0/c1 plus the camera in c14. The
    //     displacement is rotated back out through the same two rows.
    //   * true - the other shape a real client draws its ground clutter with: the
    //     vertex is placed by the instance's own rows (`dp4 r0.z, c33, v0`), so r0
    //     already IS the world position. The block reads it directly - no rows to
    //     borrow, no camera to add, and the displacement lands on the world's own
    //     x and y. (The shader's c0/c1 in that shape are `def` literals baked into
    //     the bytecode, and a `def` is only a default: the engine's own upload
    //     writes c0..c13 for every batch and would take it straight back.)
    //
    // Everything else is shared, and every one of the block's own inputs is a
    // component of c14..c19, which this module writes itself.
    //
    // One rule the vs_3_0 assembler enforces and this text has to obey: no
    // instruction may read TWO different constant registers (D3DAssemble answers
    // X5752: "Max. different constant registers readable per instruction is 1").
    // The distance fade originally read c17.w and c14.z in one `mad`, and that
    // single line is what refused every blade shader of the first build that got
    // here - the whole field stayed as the client drew it over one multiply. The
    // fade is two instructions now, and the portable suite scans both variants of
    // this block for the shape: one constant register per instruction, always.
    // `rampComponent` is which lane of that register the bend weight is read
    // from: `y` for a card bent by its texture's v (the reference client's own
    // blade), `z` for one bent by its own height, which is the only ramp a card
    // with no texture coordinate has (see MeasureGrassBladeCardByHeight). Which
    // lane is a per-shader decision made from the shader's own declaration line and
    // the draw's own vertices; the arithmetic after the read is identical, because
    // both are the same 0..1 ramp along the blade.
    inline std::string GrassBladeMotionBlock(const char* rampRegister, bool worldSpace,
        unsigned firstRegister = kGrassBladeFirstRegister, const char* translation = nullptr,
        char rampComponent = 'y')
    {
        // Where the vertex is, as the two coordinates the wave field and the
        // per-blade hash are built from.
        //
        // Three shapes, best first:
        //   * the vertex is already in the world (the instance rows placed it):
        //     the coordinate is r0 itself;
        //   * the shader's own view transform is known from its anchor line
        //     (`add r0, r0, cN`): world = R^T * (r0 - cN), R^T being the shader's
        //     own rows c0/c1 that this block already reads - exact, and with no
        //     term from the frame in it at all;
        //   * neither: the frame's camera corrects a view position that is known
        //     to be camera-relative and unrotated. This is the one shape that can
        //     drift with the camera, and it is now a fallback rather than the
        //     path every view-space shader took.
        char place[640]{};
        if (worldSpace)
            std::snprintf(place, sizeof(place),
                "    mov r2.x, r0.x\n"
                "    mov r2.y, r0.y\n");
        else if (translation && translation[0])
            std::snprintf(place, sizeof(place),
                "    sub r5.x, r0.x, %s.x\n"
                "    sub r5.y, r0.y, %s.y\n"
                "    sub r5.z, r0.z, %s.z\n"
                "    dp3 r2.x, c0.xyz, r5.xyz\n"
                "    dp3 r2.y, c1.xyz, r5.xyz\n",
                translation, translation, translation);
        else
            std::snprintf(place, sizeof(place),
                "    dp3 r2.x, c0.xyz, r0.xyz\n"
                "    add r2.x, r2.x, c14.x\n"
                "    dp3 r2.y, c1.xyz, r0.xyz\n"
                "    add r2.y, r2.y, c14.y\n");
        // And the displacement, once the two waves have been summed: out of the
        // world plane for the view-space shader, straight into it for the other.
        char shear[640]{};
        if (worldSpace)
            std::snprintf(shear, sizeof(shear),
                "    add r0.x, r0.x, r3.z\n"
                "    add r0.y, r0.y, r3.w\n");
        else
            std::snprintf(shear, sizeof(shear),
                "    mul r2.z, r3.z, c0.x\n"
                "    mad r2.z, r3.w, c1.x, r2.z\n"
                "    mul r2.w, r3.z, c0.y\n"
                "    mad r2.w, r3.w, c1.y, r2.w\n"
                "    mul r3.x, r3.z, c0.z\n"
                "    mad r3.x, r3.w, c1.z, r3.x\n"
                "    add r0.x, r0.x, r2.z\n"
                "    add r0.y, r0.y, r2.w\n"
                "    add r0.z, r0.z, r3.x\n");

        char text[4608]{};
        std::snprintf(text, sizeof(text),
            "    ; ---- injected grass wind (this renderer: see src/Environment/GrassShaderWind.h) ----\n"
            "%s"
            "    mul r3.x, r2.x, c19.z\n"
            "    mad r3.x, r2.y, c19.w, r3.x\n"
            "    frc r3.x, r3.x\n"
            "    mad r5.z, %s.%c, c19.x, c19.y\n"
            "    add r5.z, r5.z, c18.y\n"
            "    mul_sat r5.z, r5.z, c18.x\n"
            "    mul r5.z, r5.z, r5.z\n"
            "    mad r3.y, r3.x, c18.w, c18.z\n"
            "    mul r5.w, r0.z, c17.w\n"
            "    add r5.w, r5.w, c14.z\n"
            "    mul_sat r5.z, r5.z, r5.w\n"
            "    mul r5.z, r5.z, r3.y\n"
            "    mul r3.z, r2.x, c15.x\n"
            "    mad r3.z, r2.y, c15.y, r3.z\n"
            "    add r3.z, r3.z, c17.x\n"
            "    mad r3.z, r3.x, c17.z, r3.z\n"
            "    sincos r4.xy, r3.z\n"
            "    add r4.y, r4.y, c14.w\n"
            "    mul r3.y, r2.x, c16.x\n"
            "    mad r3.y, r2.y, c16.y, r3.y\n"
            "    add r3.y, r3.y, c17.y\n"
            "    mad r3.y, r3.x, c17.z, r3.y\n"
            "    sincos r5.xy, r3.y\n"
            "    mul r3.z, r4.y, c15.z\n"
            "    mul r3.w, r4.y, c15.w\n"
            "    mad r3.z, r5.y, c16.z, r3.z\n"
            "    mad r3.w, r5.y, c16.w, r3.w\n"
            "    mul r3.z, r3.z, r5.z\n"
            "    mul r3.w, r3.w, r5.z\n"
            "%s",
            place, rampRegister ? rampRegister : "v3", rampComponent, shear);
        std::string block = text;
        // And the rows move to wherever this shader's own constants are not: the
        // text is written against c14..c19 (the reference module's own free block)
        // and renumbered here, one register at a time, so the block a shader is
        // given never lands on a register that shader reads.
        if (firstRegister != kGrassBladeFirstRegister)
        {
            // Downwards, so a row that has already been moved is never moved
            // again: c19 leaves c19 (to c24) before c14 is written INTO c19.
            for (unsigned i = kGrassBladeRows; i-- > 0; )
            {
                const std::string from = "c" + std::to_string(kGrassBladeFirstRegister + i);
                const std::string to = "c" + std::to_string(firstRegister + i);
                size_t at = 0;
                while ((at = block.find(from, at)) != std::string::npos)
                {
                    const size_t next = at + from.size();
                    const bool boundary = next >= block.size() || block[next] == '.' || block[next] == '\n' ||
                        block[next] == ' ';
                    const bool start = at == 0 || (!std::isalnum(static_cast<unsigned char>(block[at - 1])) &&
                        block[at - 1] != '.');
                    if (boundary && start)
                    {
                        block.replace(at, from.size(), to);
                        at += to.size();
                    }
                    else
                    {
                        at = next;
                    }
                }
            }
        }
        return block;
    }

    // A shader text with the comments and the whitespace taken off one line, so
    // that the parser below cannot be fooled by the disassembler's own notes
    // (`; vs_2_0` and the like) and by a declaration the client's compiler wrote
    // with different spacing than the next one's.
    inline std::string GrassBladeTrimmed(const std::string& line)
    {
        size_t end = line.size();
        for (size_t i = 0; i < line.size(); ++i)
        {
            if (line[i] == ';' || (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/'))
            {
                end = i;
                break;
            }
        }
        size_t begin = 0;
        while (begin < end && (line[begin] == ' ' || line[begin] == '\t' || line[begin] == '\r')) ++begin;
        while (end > begin && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\r')) --end;
        std::string out = line.substr(begin, end - begin);
        // Commas and runs of spaces are one separator to everything below: the
        // disassembler writes `add r0, r0, c3` and an assembler accepts
        // `add r0,r0,c3`, and both are the same instruction.
        std::string compact;
        compact.reserve(out.size());
        for (char c : out)
        {
            if (c == ',') { compact += ' '; continue; }
            compact += c;
        }
        while (compact.find("  ") != std::string::npos) compact.replace(compact.find("  "), 2, " ");
        return compact;
    }

    inline std::vector<std::string> GrassBladeWords(const std::string& line)
    {
        std::vector<std::string> words;
        size_t i = 0;
        while (i < line.size())
        {
            while (i < line.size() && line[i] == ' ') ++i;
            size_t begin = i;
            while (i < line.size() && line[i] != ' ') ++i;
            if (i > begin) words.push_back(line.substr(begin, i - begin));
        }
        return words;
    }

    inline bool GrassBladeRegisterIs(const std::string& word, const char* name) noexcept
    {
        const size_t length = std::strlen(name);
        if (word.size() < length || word.compare(0, length, name) != 0) return false;
        // `r2` and `r2.xyz` are the register; `r20` is a different one.
        return word.size() == length || word[length] == '.';
    }

    // Where any input is declared, from the shader's own `dcl_` line - the texture
    // coordinate the bend weight normally comes from, and the position a card with
    // no coordinate at all is bent by instead. False means the shader does not
    // declare that input at all; `reg` carries its register number, which is 0 for
    // the first input of a declaration and therefore cannot double as the absent
    // answer.
    inline bool GrassBladeDeclaredRegister(const std::string& disassembly, const char* usage,
        size_t usageLength, unsigned& reg) noexcept
    {
        reg = 0;
        size_t at = 0;
        while (at <= disassembly.size())
        {
            const size_t end = disassembly.find('\n', at);
            const std::string line = GrassBladeTrimmed(disassembly.substr(at,
                end == std::string::npos ? std::string::npos : end - at));
            const std::vector<std::string> words = GrassBladeWords(line);
            if (words.size() >= 2 && words[0].compare(0, usageLength, usage) == 0)
                for (size_t i = 1; i < words.size(); ++i)
                    if (words[i].size() >= 2 && words[i][0] == 'v')
                    {
                        unsigned value = 0;
                        bool digits = words[i].size() > 1;
                        for (size_t c = 1; c < words[i].size() && words[i][c] != '.'; ++c)
                        {
                            if (words[i][c] < '0' || words[i][c] > '9') { digits = false; break; }
                            value = value * 10 + unsigned(words[i][c] - '0');
                        }
                        // vs_2_0 and vs_3_0 have sixteen input registers and no
                        // more: anything above that is not a texture coordinate
                        // this block can read, and is left alone.
                        if (digits && value < 16u) { reg = value; return true; }
                    }
            if (end == std::string::npos) break;
            at = end + 1;
        }
        return false;
    }

    // The register a texture coordinate is declared in, and whether the shader
    // declares one at all - the same shape as the position's answer, so a caller
    // that has to compare what the shader reads against what the draw feeds reads
    // both of them the same way.
    inline bool GrassBladeTexcoordRegister(const std::string& disassembly, unsigned& reg) noexcept
    {
        return GrassBladeDeclaredRegister(disassembly, "dcl_texcoord", 12, reg);
    }

    // The register a texture coordinate is declared in. 0 means the shader does not
    // declare one - and every shader this build has ever seen declares its position
    // first, so no coordinate of a real client lands in v0 where the two answers
    // would be the same one.
    inline unsigned GrassBladeTexcoordRegister(const std::string& disassembly) noexcept
    {
        unsigned reg = 0;
        GrassBladeTexcoordRegister(disassembly, reg);
        return reg;
    }

    // Whether the vertex's own position is declared, and in which register. The
    // bool is the answer and not the number: v0 is the ordinary register for a
    // position, and "declared in v0" must never read as "not declared". D3D9 hands
    // the elements of a declaration their input registers in the order they are
    // written, so this reads the same way the texture coordinate does - and it is
    // the value the draw's own vertices carry, which is what a bend weight built
    // from the vertex needs.
    inline bool GrassBladePositionRegister(const std::string& disassembly, unsigned& reg) noexcept
    {
        return GrassBladeDeclaredRegister(disassembly, "dcl_position", 12, reg);
    }

    // A register operand as an instruction writes it: `-r0` and `r0_abs` are r0
    // too, and a source this test missed would be a value the injected block is
    // allowed to destroy.
    inline bool GrassBladeWordIsRegister(const std::string& word, const char* name) noexcept
    {
        size_t begin = 0;
        while (begin < word.size() && (word[begin] == '-' || word[begin] == '!')) ++begin;
        const size_t abs = word.find('_', begin);
        const std::string base = word.substr(begin,
            abs == std::string::npos ? std::string::npos : abs - begin);
        return GrassBladeRegisterIs(base, name);
    }

    // The constant register a word names, or -1: `c31`, `c31.x` and `-c31.y` are
    // all register 31, `c3147` is not a register at all (and neither is a word
    // that only starts with the letter c).
    inline int GrassBladeConstantRegister(const std::string& word) noexcept
    {
        size_t begin = 0;
        while (begin < word.size() && (word[begin] == '-' || word[begin] == '!')) ++begin;
        if (begin >= word.size() || word[begin] != 'c' || begin + 1 >= word.size()) return -1;
        if (word[begin + 1] < '0' || word[begin + 1] > '9') return -1;
        unsigned value = 0;
        size_t at = begin + 1;
        while (at < word.size() && word[at] >= '0' && word[at] <= '9')
        {
            value = value * 10 + unsigned(word[at] - '0');
            ++at;
        }
        if (at < word.size() && word[at] != '.') return -1;
        return value < 256u ? int(value) : -1;
    }

    inline bool GrassBladeReferencesConstant(const std::string& disassembly, unsigned reg) noexcept
    {
        size_t at = 0;
        while (at <= disassembly.size())
        {
            const size_t end = disassembly.find('\n', at);
            const std::string line = GrassBladeTrimmed(disassembly.substr(at,
                end == std::string::npos ? std::string::npos : end - at));
            for (const std::string& word : GrassBladeWords(line))
                if (GrassBladeConstantRegister(word) == int(reg)) return true;
            if (end == std::string::npos) break;
            at = end + 1;
        }
        return false;
    }

    // The line the block is inserted AFTER: the last line that gives r0 the
    // vertex's own position before the shader projects it.
    //
    // The reference client's detail-doodad shader lands the vertex with
    // `add r0, r0, c3` and this build was written against that line; a client
    // that places the vertex through the instance's own rows instead
    // (`dp4 r0.z, c33, v0`) has no such line at all, and refusing those - every
    // one of them, in a real session - is what "0 looked at, 0 bent" on the HUD
    // was. What both forms share is not the instruction but the data: r0 is the
    // vertex's position there, and the projection is about to consume it. So the
    // anchor is found by that: the last write to r0 before a clip-space output
    // reads r0 for the first time.
    inline size_t GrassBladeAnchorLine(const std::string& disassembly) noexcept
    {
        size_t at = 0, lineNumber = 0, lastWrite = ~size_t(0);
        while (at <= disassembly.size())
        {
            const size_t end = disassembly.find('\n', at);
            const std::string line = GrassBladeTrimmed(disassembly.substr(at,
                end == std::string::npos ? std::string::npos : end - at));
            const std::vector<std::string> words = GrassBladeWords(line);
            if (words.size() >= 2)
            {
                // A clip-space output: `oPos` in vs_1_1, `o0` from vs_2_0 on. The
                // disassembler names it, not the shader's author, and a build that
                // only knew the first name refused every shader of the second kind
                // ("the shader writes no clip-space position") that had one.
                const bool position = GrassBladeWordIsRegister(words[1], "oPos") ||
                    (words[1].size() >= 2 && words[1][0] == 'o' && words[1][1] == '0');
                if (position)
                    for (size_t i = 2; i < words.size(); ++i)
                        if (GrassBladeWordIsRegister(words[i], "r0")) return lastWrite;
                if (GrassBladeWordIsRegister(words[1], "r0")) lastWrite = lineNumber;
            }
            if (end == std::string::npos) break;
            at = end + 1;
            ++lineNumber;
        }
        return ~size_t(0);
    }

    // The translation row the shader's own view transform lands the vertex with,
    // read off the anchor line: an `add r0, r0, cN` puts the vertex in view space
    // through cN, and cN is then the one term that has to come OUT of the position
    // again to get the world position back.
    //
    // Why this exists at all: the world position the wave field and the per-blade
    // phase are built from used to come from the frame's camera (`r0 - viewCamera`
    // rotated back), and the frame's camera is not the matrix the shader was drawn
    // with - the client binds its grass matrices from its own camera, and a
    // capture one frame behind (or from another camera entirely) turns into a
    // field-wide phase error that GROWS while the player walks and settles when
    // they stop: exactly the "the grass shakes when I move" a player reports.
    // Taking the term from the shader's own constant makes the world position
    // exact by construction, whatever the frame's camera is doing.
    inline std::string GrassBladeViewTranslation(const std::string& disassembly, size_t anchor) noexcept
    {
        size_t at = 0, lineNumber = 0;
        while (at <= disassembly.size())
        {
            const size_t end = disassembly.find('\n', at);
            const std::string line = GrassBladeTrimmed(disassembly.substr(at,
                end == std::string::npos ? std::string::npos : end - at));
            if (lineNumber == anchor)
            {
                const std::vector<std::string> words = GrassBladeWords(line);
                // `add r0, r0, cN` or `add r0, cN, r0`; a swizzle is allowed as
                // long as it is the .xyz the position is built from.
                if (words.size() == 4 && words[0] == "add" && GrassBladeWordIsRegister(words[1], "r0"))
                {
                    const std::string* constant = GrassBladeWordIsRegister(words[2], "r0") ? &words[3]
                        : GrassBladeWordIsRegister(words[3], "r0") ? &words[2] : nullptr;
                    if (constant)
                    {
                        const std::string word = *constant;
                        const size_t dot = word.find('.');
                        const std::string base = dot == std::string::npos ? word : word.substr(0, dot);
                        const int reg = GrassBladeConstantRegister(base);
                        if (reg >= 0 && (dot == std::string::npos || word.substr(dot) == ".xyz"))
                            return base;
                    }
                }
                return {};
            }
            if (end == std::string::npos) break;
            at = end + 1;
            ++lineNumber;
        }
        return {};
    }

    // The constant registers a shader reads, as a set, for choosing where the block
    // may live. The engine's own upload fills c0..c13 (the matrices) and, for the
    // batches that carry lights, rows like c17/c18 (their colour and parameters)
    // and c21/c22 (their positions): a real session's foliage shaders read c17 and
    // c18, so the block cannot live at c14..c19 for every shader - writing there
    // would have the shader read the block's own wind rows as its lighting. The
    // block's base is therefore chosen per shader, from the registers THAT shader
    // never mentions.
    inline std::vector<bool> GrassBladeConstantUse(const std::string& disassembly)
    {
        std::vector<bool> used(256, false);
        size_t at = 0;
        while (at <= disassembly.size())
        {
            const size_t end = disassembly.find('\n', at);
            const std::string line = GrassBladeTrimmed(disassembly.substr(at,
                end == std::string::npos ? std::string::npos : end - at));
            for (const std::string& word : GrassBladeWords(line))
            {
                const int reg = GrassBladeConstantRegister(word);
                if (reg >= 0) used[static_cast<size_t>(reg)] = true;
            }
            if (end == std::string::npos) break;
            at = end + 1;
        }
        return used;
    }
    // The first run of kGrassBladeRows registers at or above the reference block's
    // own base that the shader never reads. ~0u when the shader has none, which is
    // a refusal: a shader that reads every register the block could use cannot be
    // bent without changing what it sees.
    inline unsigned GrassBladeFreeConstantRun(const std::string& disassembly) noexcept
    {
        const std::vector<bool> used = GrassBladeConstantUse(disassembly);
        // A constant read through an index register (`c12[a0.x]`) can reach any
        // register at or above its base, and the use scan above sees such a word as
        // one register. The search therefore also stops below the lowest base read
        // that way: a shader with an indexed array never has the block land inside
        // it. (The foliage shaders of a real session have none.)
        unsigned ceiling = unsigned(used.size());
        {
            const std::string lowered = disassembly;
            size_t at = 0;
            while ((at = lowered.find('c', at)) != std::string::npos)
            {
                const bool start = at == 0 || (!std::isalnum(static_cast<unsigned char>(lowered[at - 1])) &&
                    lowered[at - 1] != '.');
                size_t digit = at + 1;
                unsigned value = 0;
                bool digits = false;
                while (start && digit < lowered.size() && std::isdigit(static_cast<unsigned char>(lowered[digit])))
                {
                    value = value * 10 + unsigned(lowered[digit] - '0');
                    digits = true;
                    ++digit;
                }
                if (start && digits && digit < lowered.size() && lowered[digit] == '[')
                    ceiling = std::min(ceiling, value);
                at = digit > at + 1 ? digit : at + 1;
            }
        }
        for (unsigned base = kGrassBladeFirstRegister; base + kGrassBladeRows <= ceiling; ++base)
        {
            bool free = true;
            for (unsigned i = 0; i < kGrassBladeRows && free; ++i)
                if (used[base + i]) free = false;
            if (free) return base;
        }
        return ~0u;
    }

    // Does the anchor line place the vertex with the engine's own instance rows?
    // That - not "the shader mentions c31 somewhere" - is what says the vertex is
    // already in the world when the block runs: the client's own blade shader
    // reads c31..c33 for its fog and its second texture coordinates as well, and a
    // test that took any mention of them for a world placement would hand the
    // block the wrong constants (three of the five foliage shaders of one real
    // session read them after their position was projected, in o6/o7/o8).
    inline bool GrassBladeAnchorUsesInstanceRows(const std::string& disassembly, size_t anchorLine) noexcept
    {
        size_t at = 0, lineNumber = 0;
        while (at <= disassembly.size())
        {
            const size_t end = disassembly.find('\n', at);
            const std::string line = GrassBladeTrimmed(disassembly.substr(at,
                end == std::string::npos ? std::string::npos : end - at));
            if (lineNumber == anchorLine)
            {
                const std::vector<std::string> words = GrassBladeWords(line);
                for (size_t i = 2; i < words.size(); ++i)
                {
                    const int reg = GrassBladeConstantRegister(words[i]);
                    if (reg == 31 || reg == 32 || reg == 33) return true;
                }
                return false;
            }
            if (end == std::string::npos) break;
            at = end + 1;
            ++lineNumber;
        }
        return false;
    }

    // Is a register the block writes still needed by the shader once the block
    // has run?
    //
    // The rule is not "the shader never mentions r2..r5". A shader that redefines
    // one of them AFTER the insertion point cannot be harmed by the block - its
    // own instruction lands after ours - and refusing every shader that mentions
    // them is how a client whose blade shader keeps its normals and lighting in
    // r2/r3 read as "nothing sways" (three of the five foliage shaders in one
    // real session). What matters is whether a register holds a value ACROSS the
    // block: written at or before the anchor, and read again before anything
    // writes it after the anchor. Only that register is refused.
    inline bool GrassBladeScratchLive(const std::string& disassembly, size_t anchorLine) noexcept
    {
        static const char* const kScratch[] = { "r2", "r3", "r4", "r5" };
        for (const char* name : kScratch)
        {
            bool writtenBefore = false;
            size_t at = 0, lineNumber = 0;
            // Everything up to and including the anchor: does this register hold a
            // value there at all?
            while (at <= disassembly.size() && lineNumber <= anchorLine)
            {
                const size_t end = disassembly.find('\n', at);
                const std::vector<std::string> words = GrassBladeWords(GrassBladeTrimmed(disassembly.substr(at,
                    end == std::string::npos ? std::string::npos : end - at)));
                if (words.size() >= 2 && GrassBladeWordIsRegister(words[1], name)) writtenBefore = true;
                if (end == std::string::npos) break;
                at = end + 1;
                ++lineNumber;
            }
            if (!writtenBefore) continue;
            // And after it: the first line that touches the register decides. A
            // read (as a source, or as the destination's own operand, as in
            // `add r2.x, r2.x, c5`) means the shader still needs the value the
            // block would destroy; a write means the shader has redefined it
            // before any use.
            while (at <= disassembly.size())
            {
                const size_t end = disassembly.find('\n', at);
                const std::vector<std::string> words = GrassBladeWords(GrassBladeTrimmed(disassembly.substr(at,
                    end == std::string::npos ? std::string::npos : end - at)));
                for (size_t i = 2; i < words.size(); ++i)
                    if (GrassBladeWordIsRegister(words[i], name)) return true;
                if (words.size() >= 2 && GrassBladeWordIsRegister(words[1], name)) break;
                if (end == std::string::npos) break;
                at = end + 1;
            }
        }
        return false;
    }

    struct GrassBladeInjection
    {
        bool ok = false;
        const char* reason = "";          // why not, in one line, for the log
        std::string text;                 // the shader text to assemble
        unsigned texcoordRegister = 0;    // the v register the block reads
        // The ramp this block was built for. False (and `texcoordRegister` set) is
        // the reference client's card, bent along its texture's v. True is a card
        // whose vertices carry no texture coordinate at all, bent along its own
        // height: `positionRegister` is the register that height is read from, and
        // the caller - which is the half that has the draw's vertices - decides
        // which one applies before it calls this, because the two must agree.
        bool rampByHeight = false;
        unsigned positionRegister = 0;    // the v register of the vertex's own height
        size_t anchorLine = 0;
        // Which frame the block's input is in: r0 is the view position, or - when
        // the anchor line places the vertex through the engine's own instance rows
        // (c31..c33) - it is already the world position. The block is built for the
        // frame it will actually run in; the device half complements that for this
        // one by packing the camera correction and the view-distance fade away,
        // neither of which a world position has (see BladeShader::worldSpace).
        bool worldSpace = false;
        // Where the block's own rows live: c14..c19, or - for a shader that reads
        // them itself - the first run of six registers above them that it never
        // mentions (see GrassBladeFreeConstantRun).
        unsigned firstRegister = kGrassBladeFirstRegister;
        // The translation row of the shader's own view transform, when its anchor
        // line names one (`add r0, r0, cN`). The block subtracts it and rotates
        // what is left back with the shader's own rows, so the world position the
        // waves are built from is exact and has no term from the frame in it -
        // which is what keeps the field from shaking as the player walks. Empty
        // for a shader whose anchor does not name one; the block then falls back
        // to the frame's camera correction.
        std::string viewTranslation;
    };

    // The whole injection, as a function of a disassembly and the shader model
    // token the blob itself declares. Nothing here touches a device: the caller
    // disassembles, calls this, and assembles what comes back.
    //
    // Every refusal below is a structure, not a guess, and each one is a shader
    // this build cannot bend without breaking it:
    //   * no texture coordinate, when the caller asked for one: the bend weight IS
    //     that coordinate - unless the caller asked for the height ramp instead,
    //     which is what a card from the family that carries no coordinate at all
    //     is bent by, and then it is the position that has to be there;
    //   * no position: the projection never reads r0, so there is no line the
    //     block can sit after with the vertex in hand;
    //   * a scratch register the shader still needs across the insertion point;
    //   * no free constant register to define the world axes in (the world-space
    //     family only), which no foliage shader in a real session has hit.
    inline GrassBladeInjection BuildGrassBladeInjection(const std::string& disassembly, const char* model,
        bool rampByHeight = false)
    {
        GrassBladeInjection out;
        if (disassembly.empty()) { out.reason = "the shader could not be disassembled"; return out; }
        if (!model || !model[0]) { out.reason = "the shader's model token is unknown"; return out; }

        // Which ramp this shader is bent by, from its own declaration: the caller
        // knows from the draw's vertices whether there is a texture coordinate to
        // read, and the two have to agree or the block would read a register the
        // draw never fed - a lane that holds (0,0,0,1) for every vertex is a
        // constant weight, and a constant weight translates a card instead of
        // bending it.
        unsigned rampRegister = 0;
        char rampComponent = 'y';
        if (rampByHeight)
        {
            if (!GrassBladePositionRegister(disassembly, rampRegister))
            {
                out.reason = "the shader declares no vertex position, so a blade has no height to bend by";
                return out;
            }
            out.rampByHeight = true;
            out.positionRegister = rampRegister;
            rampComponent = 'z';
        }
        else
        {
            rampRegister = GrassBladeTexcoordRegister(disassembly);
            if (!rampRegister)
            {
                out.reason = "the shader does not declare a texture coordinate, so a blade has no height to bend";
                return out;
            }
            out.texcoordRegister = rampRegister;
        }
        const size_t anchor = GrassBladeAnchorLine(disassembly);
        if (anchor == ~size_t(0))
        {
            out.reason = "the shader never lands its vertex in r0 before a clip-space output reads it, so the block "
                "has no position to bend and nowhere to be inserted";
            return out;
        }
        if (GrassBladeScratchLive(disassembly, anchor))
        {
            out.reason = "the shader still needs one of the scratch registers the motion block writes (r2..r5) across "
                "the insertion point";
            return out;
        }
        // The vertex's own frame. The engine's model transform lives in c31..c33
        // for a draw this build can only see as unskinned scenery, and a shader
        // that reads them has placed its vertex in the world already: r0 IS the
        // world position when the block runs, and the world's axes are the
        // coordinates themselves. Those two rows the block reads must then come
        // from registers nothing else touches - the shader's own c0/c1 in that
        // shape are `def` literals the assembler bakes in, and the engine's upload
        // cannot overwrite them - so the injection defines the two axes itself,
        // in the first two constant registers above the matrices that no line of
        // the shader mentions.
        out.worldSpace = GrassBladeAnchorUsesInstanceRows(disassembly, anchor);
        const unsigned base = GrassBladeFreeConstantRun(disassembly);
        if (base == ~0u)
        {
            out.reason = "the shader reads every constant register the motion block's rows could live in";
            return out;
        }
        out.firstRegister = base;

        char rampName[16]{};
        std::snprintf(rampName, sizeof(rampName), "v%u", rampRegister);
        if (!out.worldSpace) out.viewTranslation = GrassBladeViewTranslation(disassembly, anchor);
        const std::string block = GrassBladeMotionBlock(rampName, out.worldSpace, out.firstRegister,
            out.viewTranslation.c_str(), rampComponent);

        // The assembled text: the shader's own lines, with the block after the line
        // that lands the vertex's position. The model token is prepended when the
        // disassembler did not write one, because an assembler needs the model to
        // assemble against.
        std::string text;
        text.reserve(disassembly.size() + block.size() + 96);
        bool hasModel = false;
        size_t at = 0, lineNumber = 0;
        if (disassembly.find(model) != std::string::npos) hasModel = true;
        if (!hasModel) { text += model; text += '\n'; }
        while (at <= disassembly.size())
        {
            const size_t end = disassembly.find('\n', at);
            const size_t length = end == std::string::npos ? std::string::npos : end - at;
            text.append(disassembly, at, length);
            text += '\n';
            if (lineNumber == anchor) text += block;
            if (end == std::string::npos) break;
            at = end + 1;
            ++lineNumber;
        }
        out.ok = true;
        out.text = text;
        out.anchorLine = anchor;
        return out;
    }

    // The shader model a compiled blob declares, as the token an assembler wants
    // - or empty when this build has no business rewriting the blob. A vertex
    // shader's first dword is its version: 0xFFFE0200 is vs_2_0 and 0xFFFE0300 is
    // vs_3_0. Anything else (a pixel shader, a vs_1_1, a shader from a client this
    // build has never seen) is left exactly as the client compiled it.
    inline const char* GrassBladeShaderModel(const void* blob, size_t bytes) noexcept
    {
        if (!blob || bytes < 16) return "";
        uint32_t token = 0;
        std::memcpy(&token, blob, sizeof(token));
        if (token == 0xFFFE0200u) return "vs_2_0";
        if (token == 0xFFFE0300u) return "vs_3_0";
        return "";
    }
}
