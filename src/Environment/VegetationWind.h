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
    float treeGain = 0.f;        // TreePercent: tree gain (models above ~6 yards); ships 0
    float distance = 120.f;      // DistanceYards: sway fades out to nothing by this range
    // GrassBlades: the grass the client bakes into its own meshes has no bones to
    // turn, so it is bent in the client's own vertex shader instead (one shader
    // rewritten per session: see GrassShaderWind.h). It has a switch of its own
    // because it is the one part of the sway that rewrites something the client
    // compiled - off, every plant with a skeleton still sways.
    // SwayModels: whether the models with a skeleton in them lean at all. This is
    // the tree, bush and plant path - the bone-matrix lean - and it ships OFF: the
    // trees and bushes of a real client are drawn as cards and canopies that do
    // not read as a plant from the outside, so a player sees parts of the scenery
    // move with them. Grass, which is the part that was asked for, is the blade
    // path below and is unaffected by this switch. The height limit under it is
    // the SECOND lock, and it is the one that holds when a player turns this on:
    // a real session leans 3.40, 3.81, 5.66 and 10.77 yd models (the last with
    // thirteen bones) while its grass measures 0.16 to 0.38 yd.
    bool swayModels = false;
    // GrassLimitYards: the tallest model that may lean. Above it the draw is not
    // a tuft of grass: a bush, a canopy, a tree, a fence or a piece of scenery,
    // and a lean on one of those is what a player sees as the world moving. See
    // the size gate in src/Effects/VegetationSway.h.
    float grassLimitYards = 2.f;
    bool grassBlades = true;
    bool animatedModels = false; // SwayAnimatedModels: also lean models the client animates
    bool debugView = false;      // DebugView: extra diagnostic HUD line
};

// Per-frame counters for the optional debug line and the log. Diagnostic only:
// none of these change the sway.
//
// Every gate a draw can fail is counted here. The feature's first in-client
// report was "nothing sways and the counter says zero", and the reason it said
// zero was that the gates which rejected everything - the shader, the
// declaration, the material, the camera - were the ones that incremented
// nothing. A counter that cannot be zero while something is still wrong is the
// whole point of the line.
struct VegetationStatus
{
    unsigned candidates = 0;      // skinned M2 draw candidates considered this frame
    unsigned swaying = 0;         // instances leaning in this frame's last batch
    // Model draws the SwayModels switch turned away before any other gate ran.
    unsigned modelsOff = 0;       // trees/bushes switch off: nothing to report but that
    unsigned batches = 0;         // draw calls that received a lean this frame
    unsigned pending = 0;         // measured and plant-shaped, waiting for the second frame
    // Every draw that carried the client's skinned model declaration, whatever
    // happened to it afterwards. The gates below say why a draw did not lean, but
    // none of them says whether the field the player is standing in arrived as
    // model draws at all: a client that draws its grass as terrain detail, or
    // batches it into one static mesh, shows a small number here and no number of
    // gate refusals can explain it. This is that number.
    unsigned modelDraws = 0;
    unsigned skippedMaterial = 0; // solid material: not foliage, so not a candidate
    unsigned skippedCamera = 0;   // no accepted camera pose for the frame yet
    unsigned skippedViewport = 0; // drawn into a viewport that is not the frame camera's
    unsigned skippedFixedFunction = 0; // fixed-function T&L: no vertex shader, never the model path
    unsigned skippedNeverModel = 0;   // a pass this project identified as never a model (UI, rain, water)
    unsigned skippedShader = 0;   // a model draw this build could not prove (expected to stay zero)
    unsigned skippedWorld = 0;    // the WMO/terrain projection: a piece of the world, never a plant
    unsigned skippedLayout = 0;   // vertex declaration is not POSITION + BLENDINDICES
    unsigned skippedGeometry = 0; // couldn't read the draw's own vertices (the palette bound)
    unsigned skippedDepthOnly = 0; // writes no colour: a shadow map, a depth fill, a prepass
    unsigned skippedPalette = 0;  // c31 did not look like a bone palette
    unsigned skippedAnimated = 0; // the client's own animation is moving the model
    unsigned skippedShape = 0;    // a body's worth of bones, or longer than it is tall
    unsigned skippedSize = 0;     // outside the supported size range
    unsigned skippedTall = 0;     // taller than a grass tuft: a bush, a canopy, a tree
    unsigned skippedGround = 0;   // hangs from its origin instead of standing on it
    unsigned skippedRange = 0;    // beyond DistanceYards
    unsigned skippedCalm = 0;     // inside the range, but the wind leaves a zero angle here
    unsigned skippedWrite = 0;    // the leaned registers could not be written back
    unsigned learnedShaders = 0;  // new M2 layout variants accepted by proof this frame
    unsigned unbounded = 0;       // candidates whose palette could not be bounded
    // The roll-back that was read back and did not match: the client's own
    // matrices were not on the device any more after this renderer leaned a draw.
    // Zero on a healthy client, and the number that separates "the lean deformed
    // it" from "the gates let the wrong draw through" on the HUD.
    unsigned restoreMismatch = 0;
    float maxAngle = 0.f;         // largest angle applied this frame
    float lastHeight = 0.f;       // world height of the last leaning instance
    // The height the size gate last measured, whether it accepted the model or
    // not: "size 300" in the HUD is only answerable if the number the gate looked
    // at is on screen next to it.
    float lastMeasured = 0.f;
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
        settings.treeGain = std::clamp(WindFinite(settings.treeGain, 0.f), 0.f, 2.f);
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
// The model's own vertices
// ---------------------------------------------------------------------------
// The palette says which bones a model has and the mesh says how big it is, and
// neither answer can be taken from the constant file alone: c31 is followed by
// whatever the previous draw left there (so "how many blocks look like matrices"
// counts another model's skeleton), and a model's bone pivots span no height at
// all when it has one bone. src/Core/M2DrawGeometry.h reads both from the draw's
// own vertices; everything here is the device-free half of that read, so the
// bound and the measurement can be pinned without a device.
inline constexpr unsigned kVegetationGeometrySamples = 32;

struct VegetationGeometrySample
{
    float position[3]{0.f, 0.f, 0.f}; // model space
    unsigned bone = 0;                // the bone block this vertex is skinned by
};

// The world-space box the model's own vertices occupy under one draw's palette.
struct VegetationMeasurement
{
    bool valid = false;
    unsigned samples = 0;
    float lowest[3]{}, highest[3]{};
    float SpanX() const noexcept { return highest[0] - lowest[0]; }
    float SpanY() const noexcept { return highest[1] - lowest[1]; }
    float SpanZ() const noexcept { return highest[2] - lowest[2]; }
    float Footprint() const noexcept { return std::max(SpanX(), SpanY()); }
};

// One model-space vertex through the bone block it is skinned by, and then
// through the accepted camera pose - the same two steps the flame capture runs
// (view position = block * position, then ViewPositionToWorld). Model space ->
// view space is a rigid transform per bone, so a lean applied to the bone blocks
// carries every measured vertex with it and the measurement stays truthful for
// the frame it was taken in.
inline bool VegetationVertexToWorld(const VegetationGeometrySample& vertex, const float registers[][4],
    unsigned boneBlocks, const float viewRaw[16], const float camera[3], float out[3]) noexcept
{
    if (!registers || !viewRaw || !camera || !boneBlocks) return false;
    const unsigned bone = std::min(vertex.bone, boneBlocks - 1);
    const float* r0 = registers[bone * kM2RegistersPerBone + 0];
    const float* r1 = registers[bone * kM2RegistersPerBone + 1];
    const float* r2 = registers[bone * kM2RegistersPerBone + 2];
    const float view[3] = {
        r0[0] * vertex.position[0] + r0[1] * vertex.position[1] + r0[2] * vertex.position[2] + r0[3],
        r1[0] * vertex.position[0] + r1[1] * vertex.position[1] + r1[2] * vertex.position[2] + r1[3],
        r2[0] * vertex.position[0] + r2[1] * vertex.position[1] + r2[2] * vertex.position[2] + r2[3] };
    if (!std::isfinite(view[0]) || !std::isfinite(view[1]) || !std::isfinite(view[2])) return false;
    ViewPositionToWorld(viewRaw, camera, view, out);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

inline VegetationMeasurement MeasureVegetationGeometry(const VegetationGeometrySample* samples, unsigned count,
    const float registers[][4], unsigned boneBlocks, const float viewRaw[16], const float camera[3]) noexcept
{
    VegetationMeasurement out;
    if (!samples || !count) return out;
    bool any = false;
    for (unsigned i = 0; i < count; ++i)
    {
        float world[3]{};
        if (!VegetationVertexToWorld(samples[i], registers, boneBlocks, viewRaw, camera, world)) continue;
        if (!any) { for (int axis = 0; axis < 3; ++axis) out.lowest[axis] = out.highest[axis] = world[axis]; any = true; }
        else for (int axis = 0; axis < 3; ++axis)
        {
            out.lowest[axis] = std::min(out.lowest[axis], world[axis]);
            out.highest[axis] = std::max(out.highest[axis], world[axis]);
        }
        ++out.samples;
    }
    out.valid = any && out.samples > 0;
    return out;
}

// Where the model is placed: the world position of block 0, the root bone every
// batch of one model uploads first. This is the point a lean rotates about, and
// it is deliberately NOT the lowest bone of the blocks this batch happened to
// reveal - two batches of one tree rotating about two different points is
// exactly the "trunk and canopy came apart" failure that the instance identity
// exists to prevent. A WoW doodad is placed with its origin at the ground, so
// this is where the model meets it.
inline bool VegetationModelOrigin(const float registers[][4], const float viewRaw[16], const float camera[3],
    float out[3]) noexcept
{
    if (!registers || !viewRaw || !camera) return false;
    const float view[3] = { registers[0][3], registers[1][3], registers[2][3] };
    if (!std::isfinite(view[0]) || !std::isfinite(view[1]) || !std::isfinite(view[2])) return false;
    ViewPositionToWorld(viewRaw, camera, view, out);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
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
// A batch that carries MANY separate plants. This client merges a whole patch of
// grass into one draw with one bone per tuft, so a grass draw can carry far more
// bones than any plant - and refusing those for carrying a "full skeleton" is
// how a field of grass reads as "nothing sways". What separates such a batch
// from a creature is not the number of bones but where they ARE: a creature's
// skeleton sits inside one body a couple of yards across, while a patch's bones
// are spread over the patch. `spread` is the horizontal diagonal of the bone
// translations, `kVegetationMergedSpread` the distance beyond which the bones
// cannot belong to one body.
inline constexpr float kVegetationMergedSpread = 6.f;
// A skeleton this tall is not a body either: mounts and characters are a couple
// of yards from hoof to head, while a tree IS its skeleton, head to root. It is
// the second half of the same idea - what makes a creature a creature is being
// one body, and a thing whose bones span six yards is not one.
inline constexpr float kVegetationTallSkeleton = 6.f;

inline constexpr float kVegetationMaxFootprint = 80.f;   // yards: a larger single draw is not one plant patch
// The animation proof, in frames. A static doodad repeats its pose and its
// vertices exactly; a creature breathing, a tail swishing or a walk cycle only
// looks still for a frame or two. Six consecutive identical frames is what keeps
// mounts and characters out of the wind with the animated-models switch off.
inline constexpr unsigned kVegetationStableFrames = 6;
// And for at least this many frames, which is what makes the proof about a
// stretch of time rather than about two neighbouring frames: a creature's idle
// animation is slow, and "it did not move since the last frame" is a test a
// breathing shoulder can pass for a while.
inline constexpr uint64_t kVegetationStanceFrames = 20;
// And once a model HAS been seen moving, it is refused for this many frames
// afterwards, so a walk cycle cannot flicker back into leaning between steps.
inline constexpr uint64_t kVegetationMoveCooldownFrames = 90;
// How far a model may be moved between two frames and still be a plant: a
// placed doodad does not travel, and a mount, a creature or an object carried by
// the player does.
inline constexpr float kVegetationPlacedEpsilon = .05f;
// How far a model's own geometry may sit BELOW the point it is placed at and
// still be a plant. A plant grows out of its origin: a WoW doodad is placed with
// its base on the ground, so its vertices are above the point it is placed at -
// a tuft, a bush and a tree alike, which is why this holds for all three. What
// hangs from an origin instead is everything the client attaches to a joint: a
// cape, a shoulder pad, a sword, a saddle, a banner on a wall. Those are the
// models that broke when a standing character was measured as a plant, and this
// is the witness that refuses them without asking how they move - because a
// standing character's shoulder does not move.
inline constexpr float kVegetationBelowPivot = .25f;
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
    // Horizontal diagonal of the bone translations: near zero for one plant, the
    // size of the patch for a batch the client merged a field's grass into.
    float spread = 0.f;
    float root[3]{0.f, 0.f, 0.f};      // bone 0, world
    float rootBasis[3][3]{};           // bone 0 rotation, world (rows)
    float relative[kM2PoseSamples][3]{}; // sampled bones relative to the root, world
    unsigned samples = 0;
};

// Two different things wear more bones than a body, and they must not be told
// apart by the same test, because they are told apart by the player:
//
//   * a MERGED PATCH - one draw the client filled with a whole field's grass,
//     one bone per tuft. Its bones are spread over the patch, but none of them
//     is above the grass, so its skeleton is LOW AND WIDE: wider than it is
//     tall, and only as tall as the grass.
//   * a TREE - a single plant whose skeleton is the whole thing, root to
//     crown. It is as tall as it is wide, and it is one plant however many
//     branches it carries.
//
// Both are refused by the bone cap that keeps creatures out (a creature has one
// body's worth of bones in one body's worth of space), and only the patch is
// counted on the HUD as the plants it carries - a tree counted as forty plants
// would be as wrong as a field counted as one.
inline bool VegetationSkeletonIsSpatial(const VegetationBones& bones) noexcept
{
    return bones.spread > kVegetationMergedSpread || bones.height > kVegetationTallSkeleton;
}

// The height passed here is the MEASURED height of the draw (its own vertices),
// not the span of its bone translations: a big tree's bones all sit in its trunk
// while its canopy reaches twenty yards up, and counting that tree as twenty
// plants would be as wrong as counting a field as one.
inline bool VegetationCarriesManyPlants(const VegetationBones& bones, float measuredHeight) noexcept
{
    return bones.bones > 1 && bones.spread > kVegetationMergedSpread &&
        measuredHeight <= kVegetationTallSkeleton;
}

// The other positive witness: a plant stands ON its origin, a joint attachment
// hangs FROM it. Measured in world space, from the draw's own vertices through
// their own bone blocks, so it costs no extra read.
// A plant's own base is its origin, so the slack is a share of the model's own
// height - not a fixed half yard, which a shoulder pad's origin sits inside with
// room to spare. A TUFT of grass is a few tenths of a yard tall and still has to
// pass, which is what the absolute floor is for.
inline constexpr float kVegetationBelowPivotShare = .1f;
inline bool VegetationHangsFromItsOrigin(const VegetationMeasurement& measured, const float origin[3],
    float heightYards = 0.f) noexcept
{
    if (!measured.valid || !origin) return false;
    const float slack = std::max(kVegetationBelowPivot, kVegetationBelowPivotShare * heightYards);
    return origin[2] - measured.lowest[2] > slack;
}

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
    for (unsigned i = 0; i < out.bones; ++i)
    {
        lowestZ = std::min(lowestZ, world[i][2]);
        highestZ = std::max(highestZ, world[i][2]);
    }
    // The pivot is where the model is PLACED: block 0's world position, the root
    // bone every batch of one model uploads first. It is not the lowest bone of
    // the blocks this batch happened to reveal - that moves when a batch reveals
    // more of the skeleton, and two batches of one tree leaning about two
    // different points is the "trunk and canopy came apart" failure the instance
    // identity exists to prevent. A WoW doodad is placed with its origin at the
    // ground, so this is also where it meets it.
    float minX = world[0][0], maxX = world[0][0], minY = world[0][1], maxY = world[0][1];
    for (unsigned i = 0; i < out.bones; ++i)
    {
        minX = std::min(minX, world[i][0]); maxX = std::max(maxX, world[i][0]);
        minY = std::min(minY, world[i][1]); maxY = std::max(maxY, world[i][1]);
    }
    for (int i = 0; i < 3; ++i) { out.pivot[i] = world[0][i]; out.root[i] = world[0][i]; }
    out.top[0] = out.pivot[0]; out.top[1] = out.pivot[1]; out.top[2] = highestZ;
    // The bone span is a LOWER bound on the model's height: pivots sit inside the
    // model, so a stem is never taller than the bones that carry it. The draw's
    // own vertices raise it to the real size (VegetationMeasurement), which is
    // what a one-bone tuft needs - its bones span nothing at all.
    out.height = std::max(highestZ - lowestZ, 0.f);
    out.footprint = std::max(maxX - minX, maxY - minY);
    out.spread = std::sqrt((maxX - minX) * (maxX - minX) + (maxY - minY) * (maxY - minY));

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

// Identity of one placed model - what a lean belongs to, rather than what a draw
// call belongs to. Three things every batch of one model shares and no other
// model does: where it is placed (block 0's world position), the root bone's
// world orientation, and the vertex buffer it is drawn from. All three are
// camera-independent, so a model keeps one identity while the player turns, and
// none of them depends on how many bones a particular batch revealed - which is
// what makes the trunk and the canopy of one tree meet in the same instance.
inline uint64_t VegetationInstanceKey(const float origin[3], const float rootBasis[3][3], uint64_t model) noexcept
{
    const auto quantize = [](float value, float scale) {
        return uint64_t(int64_t(std::llround(double(WindFinite(value, 0.f)) * double(scale))));
    };
    uint64_t key = 0x9e3779b97f4a7c15ull;
    key ^= VegetationHash64(quantize(origin[0], 4.f) * 0x100000001b3ull);
    key ^= VegetationHash64(quantize(origin[1], 4.f) * 0x100000001b3ull) << 1;
    key ^= VegetationHash64(quantize(origin[2], 4.f)) << 7;
    key ^= VegetationHash64(quantize(rootBasis[0][0], 32.f)) << 13;
    key ^= VegetationHash64(quantize(rootBasis[0][1], 32.f)) << 19;
    key ^= VegetationHash64(quantize(rootBasis[1][1], 32.f)) << 29;
    key ^= VegetationHash64(model * 0x2545f4914f6cdd1dull) << 37;
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
