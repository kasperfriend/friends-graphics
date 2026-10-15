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
        float tipAmplitudeYards = .15f;   // sway at the tip of a blade, yards, at strength 1
        float wavelengthYards = 18.f;     // the wave the field travels on, yards
        float crossWavelengthYards = 6.5f;// its cross-swell, which is what makes it read as grass
        float crossAmplitudeRatio = .33f; // the cross-swell's share of the primary amplitude
        float crossAngleDegrees = 35.f;   // and its heading, off the wind
        float waveSpeedYardsPerSecond = 3.f; // how fast the crests travel downwind
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
        unsigned foliageDraws = 0;   // unskinned foliage draws offered to this path
        unsigned materialRefused = 0;// of those, the ones whose texture was not a green cut-out
        unsigned noTexcoord = 0;     // draws with no texture coordinate to bend a blade by
        unsigned registerMismatch = 0;// draws whose coordinate is not where the shader reads it
        unsigned examined = 0;       // shaders this build has looked at, this session
        unsigned injected = 0;       // shaders it is bending, this session
        unsigned refused = 0;        // shaders it left alone, this session
        float amplitudeYards = 0.f;  // sway at a blade's tip this frame, yards
    };

    // The block lives in c14..c19: six rows, one register per row, and every
    // component of them is accounted for below. They are written once per frame
    // and re-uploaded whenever the client writes over them again.
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

    // c14 = { cameraX, cameraY, 1.0, lean }
    // c15 = { primary  dir * k, primary  dir * amplitude }
    // c16 = { crosst    dir * k, cross    dir * amplitude }
    // c17 = { phasePrimary, phaseCross, jitter, -1/distance }
    // c18 = { bendScale, bendShift, varianceCentre, varianceSpread }
    // c19 = { bladeVScale, bladeVOffset, 0, 0 }
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

        // The ramp: saturate(a * bendScale + bendShift) is 0 for the first
        // `anchor` share of the blade and 1 at its tip. Both numbers are the
        // anchor's, divided out here so the shader never has to.
        const float anchor = std::clamp(settings.anchor, 0.f, .9f);
        c18[0] = 1.f / (1.f - anchor);
        c18[1] = -anchor / (1.f - anchor);
        const float variance = std::clamp(settings.variance, 0.f, 1.f);
        c18[2] = 1.f - variance * .5f;  // every blade's amplitude is this ...
        c18[3] = variance;              // ... plus this much of its own random

        // Which way the blade's own texture runs. -1/1 is the reference client's
        // own convention (v = 1 at the base, 0 at the tip) and what is used when
        // the draw's vertices say nothing either way.
        c19[0] = bladeVSign > 0 ? 1.f : -1.f;
        c19[1] = bladeVSign > 0 ? 0.f : 1.f;
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
    inline int GrassBladeVerticalSign(const float (*samples)[3], unsigned count) noexcept
    {
        if (!samples || count < 2) return 0;
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
            if (deltaV * deltaHeight < 0.f) ++falling;
            else ++rising;
        }
        if (falling == rising) return 0;
        return falling > rising ? -1 : 1;
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
    //   * a blade's own random phase is a hash of its world position, so it keeps
    //     its identity while the camera moves and never shimmers frame to frame;
    //   * the bend weight is the blade's texture v, held rigid at the base and
    //     squared towards the tip, faded with the distance from the camera, and
    //     varied from blade to blade;
    //   * the displacement is two travelling sine waves in the world plane - the
    //     wind's own heading and a cross-swell - plus the setting's constant
    //     downwind lean, and it is rotated back into view space by the same two
    //     modelview rows before it is added to r0.
    //
    // r0 is the shader's position and r1 is the engine's own scratch; the block
    // uses r2..r5 and NOTHING else, which is why the caller refuses to inject into
    // any shader that touches them (see GrassBladeTempsUsed below).
    //
    // `texcoord` is the v register of the blade's texture coordinate, taken from
    // the shader's own declaration line rather than assumed: a client that
    // declares its texcoord in v2 instead of v3 must still have its grass bend.
    inline std::string GrassBladeMotionBlock(const char* texcoord)
    {
        char text[4096]{};
        std::snprintf(text, sizeof(text),
            "    ; ---- injected grass wind (this renderer: see src/Environment/GrassShaderWind.h) ----\n"
            "    dp3 r2.x, c0.xyz, r0.xyz\n"
            "    add r2.x, r2.x, c14.x\n"
            "    dp3 r2.y, c1.xyz, r0.xyz\n"
            "    add r2.y, r2.y, c14.y\n"
            "    mul r3.x, r2.x, 0.618034\n"
            "    frc r3.y, r3.x\n"
            "    mad r5.z, %s.y, c19.x, c19.y\n"
            "    mad_sat r5.z, r5.z, c18.x, c18.y\n"
            "    mul r5.z, r5.z, r5.z\n"
            "    mad r5.w, r0.z, c17.w, c14.z\n"
            "    mul_sat r5.w, r5.w, c14.z\n"
            "    mul r5.z, r5.z, r5.w\n"
            "    mad r3.x, r3.y, c18.w, c18.z\n"
            "    mul r5.z, r5.z, r3.x\n"
            "    mul r3.z, r2.x, c15.x\n"
            "    mad r3.z, r2.y, c15.y, r3.z\n"
            "    add r3.z, r3.z, c17.x\n"
            "    mad r3.z, r3.y, c17.z, r3.z\n"
            "    sincos r4.xy, r3.z\n"
            "    add r4.y, r4.y, c14.w\n"
            "    mul r3.w, r2.x, c16.x\n"
            "    mad r3.w, r2.y, c16.y, r3.w\n"
            "    add r3.w, r3.w, c17.y\n"
            "    mad r3.w, r3.y, c17.z, r3.w\n"
            "    sincos r5.xy, r3.w\n"
            "    mul r3.z, r4.y, c15.z\n"
            "    mul r3.w, r4.y, c15.w\n"
            "    mad r3.z, r5.y, c16.z, r3.z\n"
            "    mad r3.w, r5.y, c16.w, r3.w\n"
            "    mul r3.z, r3.z, r5.z\n"
            "    mul r3.w, r3.w, r5.z\n"
            "    mul r3.x, r3.z, c0.x\n"
            "    mad r3.x, r3.w, c1.x, r3.x\n"
            "    mul r2.z, r3.z, c0.y\n"
            "    mad r2.z, r3.w, c1.y, r2.z\n"
            "    mul r2.w, r3.z, c0.z\n"
            "    mad r2.w, r3.w, c1.z, r2.w\n"
            "    add r0.x, r0.x, r3.x\n"
            "    add r0.y, r0.y, r2.z\n"
            "    add r0.z, r0.z, r2.w\n",
            texcoord ? texcoord : "v3");
        return text;
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

    // The register a texture coordinate is declared in, from the shader's own
    // declaration line. 0 means the shader does not declare one at all, which is
    // a shader this build cannot bend: the bend weight IS that coordinate.
    inline unsigned GrassBladeTexcoordRegister(const std::string& disassembly) noexcept
    {
        size_t at = 0;
        while (at <= disassembly.size())
        {
            const size_t end = disassembly.find('\n', at);
            const std::string line = GrassBladeTrimmed(disassembly.substr(at,
                end == std::string::npos ? std::string::npos : end - at));
            const std::vector<std::string> words = GrassBladeWords(line);
            if (words.size() >= 2 && words[0].compare(0, 12, "dcl_texcoord") == 0)
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
                        if (digits && value < 16u) return value;
                    }
            if (end == std::string::npos) break;
            at = end + 1;
        }
        return 0;
    }

    // Does the shader already use a register the injected block needs? Any use at
    // all - read or written, before or after the anchor - refuses the injection,
    // because a scratch value the shader still needs would be destroyed by a
    // block that starts writing there. A shader whose temps are only r0 and r1
    // (the reference client's own detail-doodad shaders are exactly that) can be
    // injected into without touching anything of its own.
    inline bool GrassBladeTempsUsed(const std::string& disassembly) noexcept
    {
        static const char* kNeeded[] = { "r2", "r3", "r4", "r5" };
        size_t at = 0;
        while (at <= disassembly.size())
        {
            const size_t end = disassembly.find('\n', at);
            const std::string line = GrassBladeTrimmed(disassembly.substr(at,
                end == std::string::npos ? std::string::npos : end - at));
            for (const std::string& word : GrassBladeWords(line))
                for (const char* needed : kNeeded)
                    if (GrassBladeRegisterIs(word, needed)) return true;
            if (end == std::string::npos) break;
            at = end + 1;
        }
        return false;
    }

    // The line the block is inserted after: the view transform of the vertex.
    // Found by what it does rather than by its exact text, because two builds of
    // the same shader write the same instruction with different masks.
    inline size_t GrassBladeAnchorLine(const std::string& disassembly) noexcept
    {
        size_t at = 0, lineNumber = 0;
        while (at <= disassembly.size())
        {
            const size_t end = disassembly.find('\n', at);
            const std::string line = GrassBladeTrimmed(disassembly.substr(at,
                end == std::string::npos ? std::string::npos : end - at));
            const std::vector<std::string> words = GrassBladeWords(line);
            if (words.size() >= 3 && (words[0] == "add" || words[0] == "ADD") &&
                GrassBladeRegisterIs(words[1], "r0"))
            {
                bool viewPosition = false, viewTranslation = false;
                for (size_t i = 2; i < words.size(); ++i)
                {
                    if (GrassBladeRegisterIs(words[i], "r0")) viewPosition = true;
                    if (GrassBladeRegisterIs(words[i], "c3")) viewTranslation = true;
                }
                if (viewPosition && viewTranslation) return lineNumber;
            }
            if (end == std::string::npos) break;
            at = end + 1;
            ++lineNumber;
        }
        return ~size_t(0);
    }

    struct GrassBladeInjection
    {
        bool ok = false;
        const char* reason = "";          // why not, in one line, for the log
        std::string text;                 // the shader text to assemble
        unsigned texcoordRegister = 0;    // the v register the block reads
        size_t anchorLine = 0;
    };

    // The whole injection, as a function of a disassembly and the shader model
    // token the blob itself declares. Nothing here touches a device: the caller
    // disassembles, calls this, and assembles what comes back.
    inline GrassBladeInjection BuildGrassBladeInjection(const std::string& disassembly, const char* model)
    {
        GrassBladeInjection out;
        if (disassembly.empty()) { out.reason = "the shader could not be disassembled"; return out; }
        if (!model || !model[0]) { out.reason = "the shader's model token is unknown"; return out; }

        const unsigned texcoord = GrassBladeTexcoordRegister(disassembly);
        if (!texcoord)
        {
            out.reason = "the shader does not declare a texture coordinate, so a blade has no height to bend";
            return out;
        }
        if (GrassBladeTempsUsed(disassembly))
        {
            out.reason = "the shader already uses one of the scratch registers the motion block needs (r2..r5)";
            return out;
        }
        const size_t anchor = GrassBladeAnchorLine(disassembly);
        if (anchor == ~size_t(0))
        {
            out.reason = "the shader has no view-transform line for the block to be inserted after";
            return out;
        }
        // A vertex shader has to write a clip-space position; without one this is
        // not a vertex shader's body at all and nothing should be inserted into
        // it.
        bool writesPosition = false;
        {
            size_t at = 0;
            while (at <= disassembly.size() && !writesPosition)
            {
                const size_t end = disassembly.find('\n', at);
                const std::string line = GrassBladeTrimmed(disassembly.substr(at,
                    end == std::string::npos ? std::string::npos : end - at));
                for (const std::string& word : GrassBladeWords(line))
                    if (word.compare(0, 4, "oPos") == 0) writesPosition = true;
                if (end == std::string::npos) break;
                at = end + 1;
            }
        }
        if (!writesPosition)
        {
            out.reason = "the shader writes no clip-space position";
            return out;
        }

        char texcoordName[16]{};
        std::snprintf(texcoordName, sizeof(texcoordName), "v%u", texcoord);
        const std::string block = GrassBladeMotionBlock(texcoordName);

        // The assembled text: the shader's own lines, with the block after the
        // view transform. The model token is prepended when the disassembler did
        // not write one, because an assembler needs the model to assemble
        // against.
        std::string text;
        text.reserve(disassembly.size() + block.size() + 64);
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
        out.texcoordRegister = texcoord;
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
