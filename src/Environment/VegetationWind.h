#pragma once
// Device-free policy for wind-driven vegetation sway.
//
// This is the vegetation consumer of the shared wind (WindSystem.h). It owns no
// clock, no random-number state and no weather reader of its own: one
// Update() per frame boundary is fed the same published WindState the clouds,
// waves and precipitation layer read, and everything else here is a pure
// function of that state, the instance's world position and its size.
//
// The client's own skinned M2 vertex shader cannot be replaced - it carries the
// client's lighting, fog and texture-stage logic - so the sway is expressed the
// way the client already consumes animated models: in its BONE MATRICES. The
// layout is the one this project verified live for torch draws
// (src/Lighting/LocalLightManager.cpp): object -> view space model matrices at
// c31 upwards, three registers per bone (three rows of [basis | translation]),
// up to 75 bones. Tilting a model about the point where it meets the ground is
// then a 4x4 pre-multiplication of each validated bone block - a rigid rotation
// about that pivot, which is exactly what a stem or trunk does in a gust and,
// unlike a per-bone taper, cannot pull two batches of one model apart, because
// every bone of an instance receives the SAME transform.
//
// Nothing here touches a device, so tools/VegetationWindRegression.cpp pins the
// lean direction, the pivot, the palette guard, bounds and continuity on Linux.

#include "WindSystem.h"
#include "../Core/M2ShaderProbe.h"
#include "../Core/MathTypes.h"
#include "../D3D9/CameraContinuity.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace renderer
{
// ---------------------------------------------------------------------------
// Settings (GraphicsEffects.ini [Vegetation], F7 -> WIND)
// ---------------------------------------------------------------------------
struct VegetationSettings
{
    bool enabled = true;         // [Vegetation] Enabled
    float sway = 1.f;            // SwayPercent: overall lean, 100% = reference look
    float gust = .45f;           // GustPercent: how deeply the moving gust field modulates the lean
    float flutter = 1.f;         // FlutterPercent: the small fast shimmer, mostly grass/bushes
    float maxAngle = 20.f;       // MaxAngleDegrees: hard ceiling so nothing can fold over
    float plantGain = 1.f;       // PlantPercent: grass/bush gain (models below ~1.5 yards)
    float treeGain = 1.f;        // TreePercent: tree gain (models above ~6 yards)
    float distance = 120.f;      // DistanceYards: sway fades out to nothing by this range
    bool animatedModels = false; // SwayAnimatedModels: also lean models the client animates
    bool debugView = false;      // DebugView: extra diagnostic HUD line
};

// Per-frame counters for the optional debug line and the log. Diagnostic only:
// none of these change the sway.
struct VegetationStatus
{
    unsigned candidates = 0;      // skinned M2 draw candidates considered this frame
    unsigned swaying = 0;         // instances leaning in this frame's last batch
    unsigned batches = 0;         // draw calls that received a lean this frame
    unsigned skippedPalette = 0;  // c31+ did not look like a bone palette
    unsigned skippedAnimated = 0; // the client's own animation is moving the model
    unsigned skippedShape = 0;    // too long/low to be a plant (fences, walls)
    unsigned skippedSize = 0;     // outside the supported size range
    unsigned skippedRange = 0;    // beyond DistanceYards
    float maxAngle = 0.f;         // largest angle applied this frame
    float lastHeight = 0.f;       // world height of the last leaning instance
};

// ---------------------------------------------------------------------------
// Gust field
// ---------------------------------------------------------------------------
// A world-anchored, seamless noise field: two scalar fields for how hard the air
// presses and how much the gust wanders off the base bearing. The sample point
// is advected with the shared downwind velocity, so a gust line travels downwind
// at the same time the clouds drift, at the same time - and a small constant
// drift keeps the air of a calm zone alive without inventing a second wind.
inline constexpr float kVegetationCellYards = 26.f;
inline constexpr int32_t kVegetationFieldCells = 256; // period = cell * cells, wrapped to stay seamless
inline constexpr float kVegetationAdvect = .3f;       // gusts lag the mean wind
inline constexpr float kVegetationDrift = .55f;       // yards/second of evolution in calm air
inline constexpr float kVegetationMinHeight = .35f;   // a tuft is treated at least this tall
inline constexpr float kVegetationTipBase = .12f;     // yards of tip travel at strength 1
inline constexpr float kVegetationTipPerRoot = .05f;  // extra travel for taller stems
inline constexpr float kVegetationFlutterDegrees = .9f;
inline constexpr float kVegetationFlutterHertz = 2.3f;

inline float VegetationFieldHash(int32_t x, int32_t y, uint32_t salt) noexcept
{
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(y) * 0xd8163841u ^ salt * 0xcb1ab31fu;
    h ^= h >> 15; h *= 0x2c1b3c6du;
    h ^= h >> 12; h *= 0x297a2d39u;
    h ^= h >> 15;
    return float(h & 0xffffffu) / float(0xffffff) - .5f; // [-0.5, 0.5)
}

// Value noise over a lattice that wraps at kVegetationFieldCells, so the field
// repeats exactly and the advected sample point can be wrapped without a seam.
inline float VegetationField(float x, float y, uint32_t salt) noexcept
{
    if (!std::isfinite(x) || !std::isfinite(y)) return 0.f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    const auto cell = [](float value) {
        const int64_t index = int64_t(value);
        return int32_t(index % kVegetationFieldCells);
    };
    const int32_t ix = cell(fx), iy = cell(fy);
    const int32_t ex = cell(fx + 1.f), ey = cell(fy + 1.f);
    const float sx = tx * tx * (3.f - 2.f * tx), sy = ty * ty * (3.f - 2.f * ty);
    const float n00 = VegetationFieldHash(ix, iy, salt), n10 = VegetationFieldHash(ex, iy, salt);
    const float n01 = VegetationFieldHash(ix, ey, salt), n11 = VegetationFieldHash(ex, ey, salt);
    const float top = n00 + (n10 - n00) * sx, bottom = n01 + (n11 - n01) * sx;
    return (top + (bottom - top) * sy) * 2.f; // [-1,1]
}

inline float VegetationSmoothStep(float edge0, float edge1, float value) noexcept
{
    if (!(edge1 > edge0)) return value < edge0 ? 0.f : 1.f;
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// Sway reaches its authored amount at 75% of the range and fades to nothing at
// the range itself, so a tree never pops between leaning and still.
inline float VegetationDistanceFade(float distanceYards, float limitYards) noexcept
{
    if (!std::isfinite(distanceYards) || !(limitYards > 1.f)) return 0.f;
    const float full = .75f * limitYards;
    if (distanceYards <= full) return 1.f;
    if (distanceYards >= limitYards) return 0.f;
    return (limitYards - distanceYards) / (limitYards - full);
}

inline uint64_t VegetationHash64(uint64_t value) noexcept
{
    value ^= value >> 33; value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33; value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

// Stable pseudo-random phase in [0,1) for one instance, so two tufts a yard
// apart do not shimmer in lockstep. Derived from the instance's identity, never
// from a frame counter or the game's random state.
inline float VegetationPhase(uint64_t seed) noexcept
{
    return float(VegetationHash64(seed) & 0xffffffull) / float(0x1000000);
}

struct VegetationSway
{
    float angleDegrees = 0.f;
    float directionX = 0.f, directionY = 0.f; // unit world horizontal, the way the top leans
    float gust = 0.f;                          // field value this instance sampled [-1,1]
    float strength = 0.f;                      // shared wind strength used
    float fade = 0.f;                          // range fade applied
    bool Leaning() const noexcept { return angleDegrees > .01f; }
};

class VegetationWind
{
public:
    void Configure(VegetationSettings settings) noexcept
    {
        settings.sway = std::clamp(WindFinite(settings.sway, 1.f), 0.f, 3.f);
        settings.gust = std::clamp(WindFinite(settings.gust, .45f), 0.f, 1.f);
        settings.flutter = std::clamp(WindFinite(settings.flutter, 1.f), 0.f, 2.f);
        settings.maxAngle = std::clamp(WindFinite(settings.maxAngle, 20.f), 1.f, 30.f);
        settings.plantGain = std::clamp(WindFinite(settings.plantGain, 1.f), 0.f, 2.f);
        settings.treeGain = std::clamp(WindFinite(settings.treeGain, 1.f), 0.f, 2.f);
        settings.distance = std::clamp(WindFinite(settings.distance, 120.f), 20.f, 300.f);
        m_settings = settings;
    }
    const VegetationSettings& Settings() const noexcept { return m_settings; }
    const WindState& Wind() const noexcept { return m_wind; }
    double Time() const noexcept { return m_time; }
    uint64_t Sequence() const noexcept { return m_sequence; }
    VegetationStatus& Status() noexcept { return m_status; }
    const VegetationStatus& Status() const noexcept { return m_status; }

    // One call per frame boundary, immediately after WindSystem::Update. The
    // wind state's own bounded delta is the only clock: a device reset, an
    // Alt+Tab gap or an F7 edit cannot teleport the field.
    void Update(const WindState& wind) noexcept
    {
        const float dt = WindDelta(wind.deltaSeconds);
        m_wind = wind;
        m_time += dt;
        if (wind.enabled && wind.affectVegetation && m_settings.enabled && dt > 0.f)
        {
            const double step = double(dt) * double(kVegetationAdvect);
            const double drift = double(dt) * double(kVegetationDrift) * .5;
            m_fieldX += double(wind.velocity.x) * step + drift;
            m_fieldY += double(wind.velocity.y) * step + drift;
            // Wrap in the field's own period (in double precision): the noise
            // lattice is wrapped the same way, so the wrap is invisible.
            const double period = double(kVegetationCellYards) * double(kVegetationFieldCells);
            m_fieldX -= std::floor(m_fieldX / period) * period;
            m_fieldY -= std::floor(m_fieldY / period) * period;
        }
        ++m_sequence;
    }
    void BeginFrame() noexcept { m_status = {}; }

    // World-space sample point of the field, for the debug line and tests.
    float FieldX() const noexcept { return float(m_fieldX); }
    float FieldY() const noexcept { return float(m_fieldY); }

    VegetationSway Sample(float worldX, float worldY, float heightYards, uint64_t instanceSeed,
        float distanceYards) const noexcept
    {
        VegetationSway out;
        if (!m_settings.enabled || !m_wind.DrivesVegetation()) return out;
        const float strength = std::clamp(m_wind.strength, 0.f, 3.f);
        const float fade = VegetationDistanceFade(distanceYards, m_settings.distance);
        if (strength <= .01f || fade <= 0.f) return out;
        if (!std::isfinite(worldX) || !std::isfinite(worldY)) return out;
        out.strength = strength;
        out.fade = fade;

        const float nx = (worldX - float(m_fieldX)) / kVegetationCellYards;
        const float ny = (worldY - float(m_fieldY)) / kVegetationCellYards;
        const float gustField = VegetationField(nx, ny, 0u) * .7f +
            VegetationField(nx * 2.3f, ny * 2.3f, 0x51ed2701u) * .3f;
        const float wander = VegetationField(nx * .83f + 11.3f, ny * .83f - 7.1f, 0x2545f491u);
        out.gust = gustField;

        // Direction: the shared downwind heading, wandering a little with the
        // gust field. Rotating the downwind vector by a small angle keeps the
        // mean travel identical to what waves and clouds use.
        const float lateral = m_settings.gust * wander * .55f;
        float dx = m_wind.direction.x - m_wind.direction.y * lateral;
        float dy = m_wind.direction.y + m_wind.direction.x * lateral;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (!(length > 1e-4f)) return {};
        out.directionX = dx / length;
        out.directionY = dy / length;

        const float height = std::max(WindFinite(heightYards, 0.f), kVegetationMinHeight);
        const float gain = std::max(.05f, 1.f + m_settings.gust * gustField);
        const float plantWeight = 1.f - VegetationSmoothStep(1.5f, 6.f, height);
        const float classGain = m_settings.plantGain * plantWeight + m_settings.treeGain * (1.f - plantWeight);
        const float tip = (kVegetationTipBase + kVegetationTipPerRoot * std::sqrt(height)) *
            m_settings.sway * strength * gain * classGain * fade;
        const float lean = std::atan(tip / height) * 57.29577951308232f;

        // Small, fast shimmer on top of the lean: strong on grass and bushes,
        // barely there on a trunk. Per-instance phase and a size-dependent
        // frequency keep a field of tufts from pulsing as one body.
        const double frequency = double(kVegetationFlutterHertz) / (1.0 + .28 * double(height));
        const double phase = m_time * frequency + double(VegetationPhase(instanceSeed));
        const double wave = std::sin(6.283185307179586 * (phase - std::floor(phase)));
        const float flutter = kVegetationFlutterDegrees * m_settings.flutter * strength * fade *
            (plantWeight + .25f * (1.f - plantWeight)) * float(wave);
        out.angleDegrees = std::clamp(lean + flutter, 0.f, m_settings.maxAngle);
        return out;
    }

private:
    VegetationSettings m_settings;
    WindState m_wind;
    VegetationStatus m_status;
    double m_time = 0.0;
    double m_fieldX = 0.0, m_fieldY = 0.0;
    uint64_t m_sequence = 0;
};

// ---------------------------------------------------------------------------
// The client's bone palette
// ---------------------------------------------------------------------------
// The palette layout itself lives in src/Core/M2ShaderProbe.h, so the probe,
// this policy and the device-side reader cannot drift apart. Verified live for
// torch draws: c31..c255, three registers per bone, at most 75 bones.
// (kM2BoneBaseRegister, kM2MaxBones, kM2RegistersPerBone, kM2BoneMaxLength)

// A block is accepted only if it can BE an object -> view matrix: three finite
// rows, an orthogonal (scale-free) basis and a plausible translation. This is
// the guard the whole feature rests on - a shader whose c31+ holds something
// else ends the palette at that block instead of being overwritten.
inline bool M2BoneBlockValid(const float r0[4], const float r1[4], const float r2[4]) noexcept
{
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(r0[i]) || !std::isfinite(r1[i]) || !std::isfinite(r2[i])) return false;
    const float l0 = std::sqrt(r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2]);
    const float l1 = std::sqrt(r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]);
    const float l2 = std::sqrt(r2[0] * r2[0] + r2[1] * r2[1] + r2[2] * r2[2]);
    if (!(l0 > 1e-4f) || !(l1 > 1e-4f) || !(l2 > 1e-4f)) return false;
    if (l0 > 1e4f || l1 > 1e4f || l2 > 1e4f) return false;
    const float d01 = (r0[0] * r1[0] + r0[1] * r1[1] + r0[2] * r1[2]) / (l0 * l1);
    const float d02 = (r0[0] * r2[0] + r0[1] * r2[1] + r0[2] * r2[2]) / (l0 * l2);
    const float d12 = (r1[0] * r2[0] + r1[1] * r2[1] + r1[2] * r2[2]) / (l1 * l2);
    if (std::fabs(d01) > .08f || std::fabs(d02) > .08f || std::fabs(d12) > .08f) return false;
    const float tx = r0[3], ty = r1[3], tz = r2[3];
    return tx * tx + ty * ty + tz * tz <= kM2BoneMaxLength * kM2BoneMaxLength;
}

// ---------------------------------------------------------------------------
// Which models are vegetation
// ---------------------------------------------------------------------------
// Policy, not preference: these are the gates that keep characters, creatures,
// walls and fences out of the wind, and they are device-free so the regression
// suite can pin them. A model is a plant when its skeleton is small, it is
// taller than a tuft, it is not longer than it is tall, and the client is not
// animating it.
inline constexpr unsigned kVegetationMaxBones = 24;     // characters, creatures and mounts carry far more
inline constexpr float kVegetationMinModelHeight = .15f; // yards: below this there is nothing to bend
inline constexpr float kVegetationMaxModelHeight = 90.f; // yards: the sway of a hill-sized tree is invisible
inline constexpr float kVegetationMinFootprint = 3.f;   // yards of absolute horizontal slack
inline constexpr float kVegetationFootprintToHeight = 2.5f; // a fence is long and low; a bush is not
// How far a model's pose signature may move between two readings and still be
// called static. The float noise of two world reconstructions a continent apart
// measures ~1e-3 yards (the test pins the margin), while the client's own
// animations move bones by centimetres, so this is deliberately conservative:
// a model whose motion is ambiguous does not sway.
inline constexpr float kVegetationPoseTolerance = .01f;

inline constexpr unsigned kM2PoseSamples = 16;

// One reading of a model's skeleton: world pivot (where it meets the ground),
// world height, horizontal footprint, and a compact camera-independent pose
// signature used to tell the client's animated models from its static ones.
struct VegetationBones
{
    bool valid = false;
    unsigned bones = 0;
    float pivot[3]{0.f, 0.f, 0.f};
    float top[3]{0.f, 0.f, 0.f};
    float height = 0.f;
    float footprint = 0.f;
    float root[3]{0.f, 0.f, 0.f};      // bone 0, world
    float rootBasis[3][3]{};           // bone 0 rotation, world (rows)
    float relative[kM2PoseSamples][3]{}; // sampled bones relative to the root, world
    unsigned samples = 0;
};

// The palette is passed as the raw float4 registers the client uploaded, in the
// same order: bone i occupies registers 3i, 3i+1, 3i+2.
inline VegetationBones AnalyzeVegetationBones(const float registers[][4], unsigned capacity,
    const float viewRaw[16], const float camera[3]) noexcept
{
    VegetationBones out;
    if (!registers || !viewRaw || !camera) return out;
    const unsigned limit = std::min(capacity / kM2RegistersPerBone, kM2MaxBones);
    if (!limit) return out;

    float world[kM2MaxBones][3]{};
    const float* base = &viewRaw[0];
    for (unsigned i = 0; i < limit; ++i)
    {
        const float* r0 = registers[i * kM2RegistersPerBone + 0];
        const float* r1 = registers[i * kM2RegistersPerBone + 1];
        const float* r2 = registers[i * kM2RegistersPerBone + 2];
        if (!M2BoneBlockValid(r0, r1, r2)) break;
        const float view[3] = { r0[3], r1[3], r2[3] };
        ViewPositionToWorld(base, camera, view, world[i]);
        ++out.bones;
    }
    if (!out.bones) return out;

    // World basis of the root bone: viewRot (view<-model) composed with the
    // frame's view rotation, the same convention ViewPositionToWorld uses.
    {
        const float* r0 = registers[0];
        const float* r1 = registers[1];
        const float* r2 = registers[2];
        const float viewRot[3][3] = {
            { r0[0], r0[1], r0[2] }, { r1[0], r1[1], r1[2] }, { r2[0], r2[1], r2[2] } };
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
            {
                float value = 0.f;
                for (int k = 0; k < 3; ++k)
                    value += viewRaw[row * 4 + k] * viewRot[k][column];
                out.rootBasis[row][column] = value;
            }
    }

    float lowestZ = world[0][2], highestZ = world[0][2];
    unsigned lowest = 0;
    for (unsigned i = 0; i < out.bones; ++i)
    {
        if (world[i][2] < lowestZ) { lowestZ = world[i][2]; lowest = i; }
        highestZ = std::max(highestZ, world[i][2]);
    }
    // The pivot is the ground contact: the lowest bone's plane, with the XY
    // averaged over every bone inside a short band, so one stray bone cannot
    // drag the pivot away from the trunk.
    float px = 0.f, py = 0.f, pz = 0.f;
    unsigned band = 0;
    float minX = world[0][0], maxX = world[0][0], minY = world[0][1], maxY = world[0][1];
    for (unsigned i = 0; i < out.bones; ++i)
    {
        minX = std::min(minX, world[i][0]); maxX = std::max(maxX, world[i][0]);
        minY = std::min(minY, world[i][1]); maxY = std::max(maxY, world[i][1]);
        if (world[i][2] <= lowestZ + .75f)
        {
            px += world[i][0]; py += world[i][1]; pz += world[i][2];
            ++band;
        }
    }
    if (!band) { px = world[lowest][0]; py = world[lowest][1]; pz = world[lowest][2]; band = 1; }
    out.pivot[0] = px / band; out.pivot[1] = py / band; out.pivot[2] = pz / band;
    for (int i = 0; i < 3; ++i) out.root[i] = world[0][i];
    out.top[0] = out.pivot[0]; out.top[1] = out.pivot[1]; out.top[2] = highestZ;
    out.height = highestZ - out.pivot[2];
    out.footprint = std::max(maxX - minX, maxY - minY);

    // Pose signature: evenly spaced bones, relative to the root. Relative
    // offsets cancel the camera, so a static model stays bit-stable while the
    // player flies across a zone, and an animated one moves immediately.
    out.samples = std::min(out.bones, kM2PoseSamples);
    for (unsigned s = 0; s < out.samples; ++s)
    {
        const unsigned index = out.samples > 1 ? s * (out.bones - 1) / (out.samples - 1) : 0;
        for (int i = 0; i < 3; ++i) out.relative[s][i] = world[index][i] - out.root[i];
    }
    out.valid = std::isfinite(out.height) && std::isfinite(out.footprint) &&
        std::isfinite(out.pivot[0]) && std::isfinite(out.pivot[1]) && std::isfinite(out.pivot[2]);
    return out;
}

// How far the client's own animation moved this model between two readings.
// Static doodads (trees, bushes, ground clutter, rocks) read ~0 - at most the
// float noise of two world reconstructions, which the caller's tolerance covers.
inline float VegetationPoseDelta(const VegetationBones& previous, const VegetationBones& current) noexcept
{
    if (!previous.valid || !current.valid) return 0.f;
    float delta = 0.f;
    const unsigned samples = std::min(previous.samples, current.samples);
    for (unsigned s = 0; s < samples; ++s)
        for (int i = 0; i < 3; ++i)
            delta = std::max(delta, std::fabs(previous.relative[s][i] - current.relative[s][i]));
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            delta = std::max(delta, std::fabs(previous.rootBasis[row][column] - current.rootBasis[row][column]));
    return delta;
}

// Identity of one placed model: the ground contact, the skeleton size and the
// root orientation. Batches of one instance share all three; two different
// models that share a spot differ in the skeleton or the orientation.
inline uint64_t VegetationInstanceKey(const VegetationBones& bones) noexcept
{
    const auto quantize = [](float value, float scale) {
        return uint64_t(int64_t(std::llround(double(WindFinite(value, 0.f)) * double(scale))));
    };
    uint64_t key = 0x9e3779b97f4a7c15ull;
    key ^= VegetationHash64(quantize(bones.pivot[0], 4.f) * 0x100000001b3ull);
    key ^= VegetationHash64(quantize(bones.pivot[1], 4.f) * 0x100000001b3ull) << 1;
    key ^= VegetationHash64(quantize(bones.pivot[2], 4.f)) << 7;
    key ^= VegetationHash64(bones.bones * 0x51ull + quantize(bones.rootBasis[0][0], 32.f)) << 13;
    key ^= VegetationHash64(quantize(bones.rootBasis[0][1], 32.f)) << 19;
    key ^= VegetationHash64(quantize(bones.rootBasis[1][1], 32.f)) << 29;
    return VegetationHash64(key);
}

// ---------------------------------------------------------------------------
// The lean itself
// ---------------------------------------------------------------------------
// Row-vector 4x4: rows 0..2 are (basis row, translation), exactly how the client
// stores a 3x4 model -> view matrix in three vertex shader registers, with row 3
// (0,0,0,1) so that "apply M, then A" is the matrix product A * M.
struct VegetationLean
{
    float rows[4][4]{
        {1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f},
        {0.f, 0.f, 1.f, 0.f}, {0.f, 0.f, 0.f, 1.f} };
    float angleDegrees = 0.f;
    bool Identity() const noexcept { return angleDegrees == 0.f; }
};

// Rotation about `pivot` that tips everything above it toward `dir`, by an
// orthonormal-frame construction rather than a cross-product handedness: the
// up axis maps to cos*up + sin*dir, so the lean direction is guaranteed no
// matter which handedness the client's view space uses. Distance from the pivot
// is preserved, which is what makes a stem bend instead of stretch.
inline VegetationLean BuildVegetationLean(const float pivotView[3], const float upView[3],
    const float dirView[3], float angleDegrees) noexcept
{
    VegetationLean lean;
    if (!pivotView || !upView || !dirView) return lean;
    if (!(angleDegrees > .001f) || !std::isfinite(angleDegrees)) return lean;
    const Vec3 rawUp = { upView[0], upView[1], upView[2] };
    const float upLength = Length(rawUp);
    if (!(upLength > .5f)) return lean;
    const Vec3 up = rawUp * (1.f / upLength);
    const Vec3 raw = { dirView[0], dirView[1], dirView[2] };
    const Vec3 flat = raw - up * Dot(raw, up);
    const float flatLength = Length(flat);
    if (!(flatLength > 1e-3f)) return lean;
    const Vec3 dir = flat * (1.f / flatLength);
    const Vec3 axis = Cross(up, dir);
    const float radians = angleDegrees * .017453292519943295f;
    const float c = std::cos(radians), s = std::sin(radians);
    const Vec3 tiltedUp = up * c + dir * s;
    const Vec3 tiltedDir = dir * c - up * s;
    // The client's registers are three ROWS of the model -> view matrix, i.e.
    // ApplyVegetationLean computes out = A * M with A's row `r` multiplying the
    // bone rows. In that product A is indexed as [row][column] = the image of
    // basis vector `column`, which is the column of the rotation - not its row,
    // and the difference is the transpose, i.e. a lean into the wind instead of
    // downwind. The matrix is therefore assembled column by column.
    for (int column = 0; column < 3; ++column)
    {
        const Vec3 e = { column == 0 ? 1.f : 0.f, column == 1 ? 1.f : 0.f, column == 2 ? 1.f : 0.f };
        const Vec3 image = tiltedUp * Dot(e, up) + tiltedDir * Dot(e, dir) + axis * Dot(e, axis);
        lean.rows[0][column] = image.x;
        lean.rows[1][column] = image.y;
        lean.rows[2][column] = image.z;
    }
    for (int row = 0; row < 3; ++row)
        // Keep the ground contact exactly where it was: A * pivot + t == pivot.
        lean.rows[row][3] = pivotView[row] - (lean.rows[row][0] * pivotView[0] +
            lean.rows[row][1] * pivotView[1] + lean.rows[row][2] * pivotView[2]);
    // A degenerate pivot (a model drawn before the camera has a position) still
    // produces a valid rotation, but never a translation the caller did not ask
    // for, so the identity is reported instead.
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column)
            if (!std::isfinite(lean.rows[row][column])) return VegetationLean{};
    lean.angleDegrees = angleDegrees;
    return lean;
}

// out = A * M, where both are the 3x4 slices of 4x4 matrices whose last row is
// the implicit (0,0,0,1): the lean's translation column reaches the bone's
// translation column only, never its basis. Written out, out[row][column] =
// sum_k A[row][k] * M[k][column] + (column == 3 ? A[row][3] : 0).
inline void ApplyVegetationLean(const VegetationLean& lean, const float bone[3][4], float out[3][4]) noexcept
{
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column)
        {
            float value = column == 3 ? lean.rows[row][3] : 0.f;
            for (int k = 0; k < 3; ++k) value += lean.rows[row][k] * bone[k][column];
            out[row][column] = value;
        }
}

}
