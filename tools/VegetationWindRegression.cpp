// Portable checks for wind-driven vegetation sway. Run from the repository
// root, no SDK needed on Linux:
//   mkdir -p build && g++ -O2 -std=c++20 -Wall -Wextra -Werror -pedantic '
//     tools/VegetationWindRegression.cpp -o build/vegetation-test && ./build/vegetation-test
// The same file is built by tools/Test-VegetationWind.cmd on the Windows
// runner, so the settings parity checked here is also checked where the real
// location-scoped INI read/save/reload path lives.
#include "../src/Core/TextureCutout.h"
#include "../src/Environment/GrassShaderWind.h"
#include "../src/Environment/VegetationWind.h"
#include "../src/Environment/WindTuning.h"
#include "../src/Environment/WindDebug.h"
#include "../src/Environment/OverlayLayout.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace renderer;

static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void Pass(const char* message) { std::printf("PASS %s\n", message); }
static bool Near(double a, double b, double epsilon = 1e-5) { return std::abs(a - b) <= epsilon; }
static std::string Read(const char* path)
{
    std::ifstream file(path);
    Check(bool(file), path);
    return {std::istreambuf_iterator<char>(file), {}};
}

// ---------------------------------------------------------------------------
// A synthetic camera, so the sway's conventions are checked end to end: world
// wind -> view direction -> lean -> view position -> world position.
// ---------------------------------------------------------------------------
struct Camera
{
    float position[3]{};
    float view[16]{}; // world -> view: world = position + view (columns = view axes)

    Camera(const float eye[3], float yawDegrees, float pitchDegrees)
    {
        for (int i = 0; i < 3; ++i) position[i] = eye[i];
        const double yaw = yawDegrees * 0.017453292519943295;
        const double pitch = pitchDegrees * 0.017453292519943295;
        // WoW map axes: +X north, +Y west, +Z up. Yaw turns clockwise from north.
        const Vec3 forward = { float(std::cos(yaw) * std::cos(pitch)), float(-std::sin(yaw) * std::cos(pitch)),
                               float(std::sin(pitch)) };
        const Vec3 worldUp = { 0.f, 0.f, 1.f };
        const Vec3 right = Normalize(Cross(worldUp, forward));
        const Vec3 up = Cross(forward, right);
        const Vec3 axes[3] = { right, up, forward };
        for (int column = 0; column < 3; ++column)
            for (int row = 0; row < 3; ++row) view[row * 4 + column] = axes[column].x * (row == 0) +
                axes[column].y * (row == 1) + axes[column].z * (row == 2);
    }

    void ToView(const float world[3], float out[3]) const
    {
        const float offset[3] = { world[0] - position[0], world[1] - position[1], world[2] - position[2] };
        for (int column = 0; column < 3; ++column)
            out[column] = offset[0] * view[0 * 4 + column] + offset[1] * view[1 * 4 + column] +
                offset[2] * view[2 * 4 + column];
    }
    void DirectionToView(const float world[3], float out[3]) const
    {
        for (int column = 0; column < 3; ++column)
            out[column] = world[0] * view[0 * 4 + column] + world[1] * view[1 * 4 + column] +
                world[2] * view[2 * 4 + column];
    }
    void ToWorld(const float viewPosition[3], float out[3]) const
    {
        ViewPositionToWorld(view, position, viewPosition, out);
    }
};

// One bone that places the model's origin at `viewTranslation`, for the
// isolated lean-geometry checks where only the transform is under test.
static void MakePlacementBone(const float viewTranslation[3], float bone[3][4])
{
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column)
            bone[row][column] = (row == column) ? 1.f : 0.f;
    bone[0][3] = viewTranslation[0];
    bone[1][3] = viewTranslation[1];
    bone[2][3] = viewTranslation[2];
}

// One bone exactly as the client uploads it for a model sitting at `world`: the
// three registers hold the model -> view matrix, so the basis is the inverse of
// the camera's world <- view rotation (times the model's own world rotation) and
// the translation is the bone's view-space position. A model that is standing
// still therefore has a DIFFERENT block from every camera, while its
// reconstructed world pose is the same - which is exactly what the static test
// below has to demonstrate.
static const float kNoRotation[3][3] = { { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f } };
static void MakeBone(const Camera& camera, const float world[3], const float worldRotation[3][3],
    float bone[3][4])
{
    float view[3];
    camera.ToView(world, view);
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            // (V transpose * worldRotation), V[r][c] = view[r * 4 + c].
            float value = 0.f;
            for (int k = 0; k < 3; ++k) value += camera.view[k * 4 + row] * worldRotation[k][column];
            bone[row][column] = value;
        }
        bone[row][3] = view[row];
    }
}

static void TestLeanGeometry()
{
    // The lean must tip everything above the pivot toward the wind direction,
    // keep the ground contact fixed, and stay a pure rotation.
    const float pivot[3] = { 2.f, -4.f, 7.f };
    const float up[3] = { 0.f, 0.f, 1.f };
    const float dir[3] = { 1.f, 0.f, 0.f };
    for (float angle : { 0.5f, 5.f, 20.f })
    {
        const auto lean = BuildVegetationLean(pivot, up, dir, angle);
        float bone[3][4], out[3][4];
        MakePlacementBone(pivot, bone);
        ApplyVegetationLean(lean, bone, out);
        Check(Near(out[0][3], pivot[0], 1e-4) && Near(out[1][3], pivot[1], 1e-4) && Near(out[2][3], pivot[2], 1e-4),
            "the ground contact never moves");

        const float top[3] = { pivot[0], pivot[1], pivot[2] + 10.f };
        MakePlacementBone(top, bone);
        ApplyVegetationLean(lean, bone, out);
        const double radians = angle * 0.017453292519943295;
        const float moved[3] = { out[0][3] - pivot[0], out[1][3] - pivot[1], out[2][3] - pivot[2] };
        Check(Near(std::hypot(std::hypot(moved[0], moved[1]), moved[2]), 10.0, 1e-3),
            "a leaning stem keeps its length: this is a bend, not a stretch");
        Check(Near(moved[0], 10.0 * std::sin(radians), 1e-3) && Near(moved[2], 10.0 * std::cos(radians), 1e-3),
            "the tip travels toward the wind, by the requested angle");

        // A point that is not on the pivot stays the same distance from it.
        const float anywhere[3] = { pivot[0] + 3.f, pivot[1] - 2.f, pivot[2] + 5.f };
        MakePlacementBone(anywhere, bone);
        ApplyVegetationLean(lean, bone, out);
        const float before[3] = { 3.f, -2.f, 5.f };
        const float after[3] = { out[0][3] - pivot[0], out[1][3] - pivot[1], out[2][3] - pivot[2] };
        Check(Near(std::hypot(std::hypot(after[0], after[1]), after[2]),
                   std::hypot(std::hypot(before[0], before[1]), before[2]), 1e-3),
            "every bone keeps its distance from the pivot");
        // ... and it does not gain a sideways component.
        const float sideways[3] = { after[0] * 0.f + after[1] * 1.f, 0.f, 0.f };
        Check(Near(sideways[0], before[1], 1e-4), "the lean has no lateral component of its own");

        // The basis stays orthonormal: no scale or shear is introduced.
        for (int row = 0; row < 3; ++row)
        {
            const float length = std::sqrt(out[row][0] * out[row][0] + out[row][1] * out[row][1] +
                                           out[row][2] * out[row][2]);
            Check(Near(length, 1.0, 1e-4), "lean basis rows stay unit length");
        }
        for (int a = 0; a < 3; ++a)
            for (int b = a + 1; b < 3; ++b)
            {
                const float dot = out[a][0] * out[b][0] + out[a][1] * out[b][1] + out[a][2] * out[b][2];
                Check(Near(dot, 0.0, 1e-4), "lean basis rows stay orthogonal");
            }
    }

    // Degenerate input must return an identity, never a NaN matrix.
    const auto identity = [](const VegetationLean& lean) {
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column)
            {
                const float expected = (row == column) ? 1.f : 0.f;
                if (lean.rows[row][column] != expected) return false;
            }
        return true;
    };
    const float nan3[3] = { std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f };
    Check(identity(BuildVegetationLean(pivot, up, dir, 0.f)), "a zero angle is the identity");
    Check(identity(BuildVegetationLean(pivot, up, dir, std::numeric_limits<float>::quiet_NaN())),
        "a NaN angle is the identity");
    Check(identity(BuildVegetationLean(pivot, nan3, dir, 5.f)), "a NaN up vector is the identity");
    Check(identity(BuildVegetationLean(pivot, up, up, 5.f)),
        "a wind direction parallel to up cannot define a lean plane");
    Check(identity(BuildVegetationLean(nullptr, up, dir, 5.f)), "no pivot is the identity");
}

static void TestLeanInWorldSpace()
{
    // End-to-end: the same world wind must tip a model toward that world wind
    // through the client's own view transform, for any camera and any heading.
    const float eye[3] = { 100.f, -50.f, 30.f };
    const float ground[3] = { 96.f, -47.f, 30.f };
    const float heights[3] = { 1.f, 6.f, 22.f };
    for (float yaw : { 0.f, 37.f, 128.f, 250.f, 359.f })
        for (float pitch : { -22.f, 0.f, 41.f })
            for (float heading : { 0.f, 45.f, 200.f, 315.f })
            {
                const Camera camera(eye, yaw, pitch);
                const double radians = heading * 0.017453292519943295;
                const float wind[3] = { float(-std::cos(radians)), float(std::sin(radians)), 0.f };
                float upView[3], dirView[3], pivotView[3];
                const float worldUp[3] = { 0.f, 0.f, 1.f };
                camera.DirectionToView(worldUp, upView);
                camera.DirectionToView(wind, dirView);
                camera.ToView(ground, pivotView);
                const float angle = 12.f;
                const auto lean = BuildVegetationLean(pivotView, upView, dirView, angle);
                for (float height : heights)
                {
                    const float tip[3] = { ground[0], ground[1], ground[2] + height };
                    float viewTip[3], bone[3][4], out[3][4], worldTip[3];
                    camera.ToView(tip, viewTip);
                    MakePlacementBone(viewTip, bone);
                    ApplyVegetationLean(lean, bone, out);
                    // The translation is one float per row: extract it first,
                    // &out[0][3] would walk into the next row's basis.
                    const float leanedTip[3] = { out[0][3], out[1][3], out[2][3] };
                    camera.ToWorld(leanedTip, worldTip);
                    const float moved[3] = { worldTip[0] - ground[0], worldTip[1] - ground[1], worldTip[2] - ground[2] };
                    const double travel = moved[0] * wind[0] + moved[1] * wind[1] + moved[2] * wind[2];
                    Check(Near(travel, double(height) * std::sin(angle * 0.017453292519943295), 2e-3),
                        "in world space the top travels downwind, by height * sin(angle)");
                    Check(Near(std::hypot(std::hypot(moved[0], moved[1]), moved[2]), double(height), 2e-3),
                        "in world space the stem keeps its length");
                }
                // The ground contact is untouched in world space too.
                float viewGround[3], bone[3][4], out[3][4], worldGround[3];
                camera.ToView(ground, viewGround);
                MakePlacementBone(viewGround, bone);
                ApplyVegetationLean(lean, bone, out);
                const float leanedGround[3] = { out[0][3], out[1][3], out[2][3] };
                camera.ToWorld(leanedGround, worldGround);
                Check(Near(worldGround[0], ground[0], 1e-3) && Near(worldGround[1], ground[1], 1e-3) &&
                      Near(worldGround[2], ground[2], 1e-3), "the ground contact is fixed in world space");
            }
    Pass("lean transform: pivot-exact, purely rotational, downwind in world space for any camera or heading");
}

// ---------------------------------------------------------------------------
// Bone palette reading
// ---------------------------------------------------------------------------
static constexpr unsigned kRegisters = kM2MaxBones * kM2RegistersPerBone;
struct Palette
{
    std::vector<std::array<float, 4>> registers{kRegisters};
    // Places bone `index` at a world position with an identity rotation, which is
    // all the analysis needs to reconstruct a skeleton.
    void Place(const Camera& camera, unsigned index, const float world[3],
        const float worldRotation[3][3] = kNoRotation)
    {
        MakeBone(camera, world, worldRotation, reinterpret_cast<float(*)[4]>(registers[index * 3].data()));
    }
};

static void TestBoneAnalysis()
{
    const float eye[3] = { 0.f, 0.f, 0.f };
    const Camera camera(eye, 30.f, -8.f);
    Palette palette;
    const float chain[5][3] = {
        { 500.f, -300.f, 40.f }, { 500.f, -300.f, 43.f }, { 501.f, -299.f, 46.f },
        { 502.f, -298.f, 49.f }, { 503.f, -297.f, 51.f }
    };
    for (unsigned i = 0; i < 5; ++i) palette.Place(camera, i, chain[i]);
    const auto bones = AnalyzeVegetationBones(reinterpret_cast<const float(*)[4]>(palette.registers.data()),
        kRegisters, camera.view, camera.position);
    Check(bones.valid && bones.bones == 5, "a five bone stem is read in full");
    Check(Near(bones.pivot[2], 40.0, 1e-3), "the pivot is the ground contact (the lowest bone)");
    Check(Near(bones.height, 11.0, 1e-3), "height is the ground contact to the highest bone");
    Check(bones.footprint <= 3.1f, "footprint covers the bone set");
    Check(bones.samples > 1, "the pose signature samples several bones");
    Check(Near(bones.relative[0][0], 0.0, 1e-4) && Near(bones.relative[0][1], 0.0, 1e-4) &&
          Near(bones.relative[0][2], 0.0, 1e-4), "the first pose sample is the root itself");

    // A palette that stops looking like object -> view matrices ends there.
    Palette truncated;
    for (unsigned i = 0; i < 5; ++i) truncated.Place(camera, i, chain[i]);
    truncated.registers[3 * 3].data()[0] = std::numeric_limits<float>::quiet_NaN();
    auto shortBones = AnalyzeVegetationBones(reinterpret_cast<const float(*)[4]>(truncated.registers.data()),
        kRegisters, camera.view, camera.position);
    Check(shortBones.valid && shortBones.bones == 3, "a broken block ends the palette instead of being used");
    truncated = {};
    shortBones = AnalyzeVegetationBones(reinterpret_cast<const float(*)[4]>(truncated.registers.data()),
        kRegisters, camera.view, camera.position);
    Check(!shortBones.valid && shortBones.bones == 0, "an all-zero palette is rejected");
    Check(!AnalyzeVegetationBones(nullptr, kRegisters, camera.view, camera.position).valid,
        "no palette is rejected");

    // Non-uniform and mirrored scales are still bones; a sheared basis is not.
    Palette scaled;
    for (unsigned i = 0; i < 3; ++i) scaled.Place(camera, i, chain[i]);
    // A whole row mirrored and scaled 1.5x: still a basis, so still a bone.
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) scaled.registers[3 * 2 + row].data()[column] *= -1.5f;
    Check(AnalyzeVegetationBones(reinterpret_cast<const float(*)[4]>(scaled.registers.data()),
        kRegisters, camera.view, camera.position).bones == 3, "mirrored and scaled bones are accepted");
    Palette sheared;
    for (unsigned i = 0; i < 3; ++i) sheared.Place(camera, i, chain[i]);
    sheared.registers[3 * 1 + 1].data()[0] = 0.6f;
    Check(AnalyzeVegetationBones(reinterpret_cast<const float(*)[4]>(sheared.registers.data()),
        kRegisters, camera.view, camera.position).bones == 1, "a sheared block ends the palette");
    Palette runaway;
    for (unsigned i = 0; i < 3; ++i) runaway.Place(camera, i, chain[i]);
    runaway.registers[3 * 1 + 0].data()[3] = 1e6f;
    Check(AnalyzeVegetationBones(reinterpret_cast<const float(*)[4]>(runaway.registers.data()),
        kRegisters, camera.view, camera.position).bones == 1, "an absurd translation ends the palette");
    Pass("bone palette: accepted only when the blocks can be object -> view matrices");
}

// One placed model's identity, as the device half builds it: where it is placed,
// how its root is oriented, and the vertex buffer it is drawn from. The skeleton
// is deliberately NOT part of it - a batch that reveals fewer bones than another
// batch of the same model is the same plant, and treating it as a second one is
// how a trunk and its canopy come apart.
static uint64_t KeyOf(const VegetationBones& bones, uint64_t model = 0x5eedull)
{
    return VegetationInstanceKey(bones.root, bones.rootBasis, model);
}

static void TestStaticPoseDetection()
{
    // A static model must read as static from a moving camera, even far away -
    // and animated models must be caught.
    const float chain[6][3] = {
        { 6400.f, -5100.f, 122.f }, { 6400.2f, -5100.1f, 125.f }, { 6400.5f, -5099.7f, 129.f },
        { 6401.f, -5099.2f, 133.f }, { 6401.6f, -5098.6f, 137.f }, { 6402.f, -5098.f, 141.f }
    };
    const float eyeA[3] = { 6200.f, -5000.f, 130.f };
    const float eyeB[3] = { 6500.f, -5400.f, 180.f };
    const Camera cameraA(eyeA, 41.f, -6.f);
    const Camera cameraB(eyeB, 212.f, 3.f);
    Palette first, second;
    auto build = [&](Palette& palette, const Camera& camera) {
        for (unsigned i = 0; i < 6; ++i) palette.Place(camera, i, chain[i]);
    };
    build(first, cameraA);
    build(second, cameraB);
    const auto readA = AnalyzeVegetationBones(reinterpret_cast<const float(*)[4]>(first.registers.data()),
        kRegisters, cameraA.view, cameraA.position);
    const auto readB = AnalyzeVegetationBones(reinterpret_cast<const float(*)[4]>(second.registers.data()),
        kRegisters, cameraB.view, cameraB.position);
    Check(Near(readA.pivot[0], readB.pivot[0], 2e-3) && Near(readA.pivot[1], readB.pivot[1], 2e-3),
        "a world reconstruction from a different camera agrees");
    // The tolerance has to clear the noise floor of the reconstruction with a
    // real margin: that margin is why a client animation can never be mistaken
    // for a static model, and why a static one is never called animated.
    Check(VegetationPoseDelta(readA, readB) < kVegetationPoseTolerance * .5f,
        "a static model reads as static from a camera 500 yards away");
    Check(KeyOf(readA) == KeyOf(readB), "one placed model keeps one instance identity as the camera moves");
    Check(Near(readA.rootBasis[0][0], readB.rootBasis[0][0], 1e-4) &&
          Near(readA.rootBasis[1][1], readB.rootBasis[1][1], 1e-4),
        "and the world orientation the identity is built on is camera independent");
    Check(KeyOf(readA, 0x11ull) != KeyOf(readA, 0x22ull),
        "two models drawn from different vertex buffers are different instances");
    // The origin is quantized, so the sub-centimetre noise of two world
    // reconstructions cannot split one model into a new instance every frame.
    VegetationBones nudged = readA;
    nudged.root[0] += .01f; nudged.root[1] -= .01f;
    Check(KeyOf(nudged) == KeyOf(readA), "reconstruction noise does not create a new instance");

    Palette animated;
    build(animated, cameraB);
    const float moved[3] = { chain[4][0] + .25f, chain[4][1], chain[4][2] };
    animated.Place(cameraB, 4, moved);
    const auto readAnimated = AnalyzeVegetationBones(reinterpret_cast<const float(*)[4]>(animated.registers.data()),
        kRegisters, cameraB.view, cameraB.position);
    Check(VegetationPoseDelta(readB, readAnimated) > kVegetationPoseTolerance * 10.f,
        "a bone that moved 25 cm is not static");

    // A model that turned 90 degrees in place: the client uploads a different
    // orientation for the same skeleton, so the world pose is no longer static.
    const float turned[3][3] = { { 0.f, -1.f, 0.f }, { 1.f, 0.f, 0.f }, { 0.f, 0.f, 1.f } };
    Palette rotated;
    for (unsigned i = 0; i < 6; ++i) rotated.Place(cameraB, i, chain[i], turned);
    const auto readRotated = AnalyzeVegetationBones(reinterpret_cast<const float(*)[4]>(rotated.registers.data()),
        kRegisters, cameraB.view, cameraB.position);
    Check(VegetationPoseDelta(readB, readRotated) > .5f, "a model that turned in place is not static");
    Check(KeyOf(readB) != KeyOf(readRotated),
        "and the same skeleton facing another way is a different instance");

    // The bug this identity exists to prevent: the client draws one model in
    // several batches and each batch's own vertices reference a different number
    // of bones. Under the old key (which carried the skeleton) every such batch
    // was a NEW instance, so nothing was ever stable, nothing ever leaned, and
    // the HUD said 0. Two readings of one model that reveal different numbers of
    // bones have to stay ONE instance.
    Palette shorter;
    for (unsigned i = 0; i < 4; ++i) shorter.Place(cameraA, i, chain[i]);
    const auto readShorter = AnalyzeVegetationBones(reinterpret_cast<const float(*)[4]>(shorter.registers.data()),
        kRegisters, cameraA.view, cameraA.position);
    Check(readShorter.bones == 4 && Near(readShorter.pivot[0], readA.pivot[0], 1e-3) &&
          Near(readShorter.pivot[2], readA.pivot[2], 1e-3),
        "a batch that reveals four of six bones is the same placed model");
    Check(KeyOf(readA) == KeyOf(readShorter),
        "batches that reveal different numbers of bones are one instance, not two");
    Pass("static pose: a static model reads static from any camera, animated ones never do");
}

// ---------------------------------------------------------------------------
// Model size: the draw's own vertices, not its bone pivots
// ---------------------------------------------------------------------------
static void TestGeometryMeasurement()
{
    const float eye[3] = { 0.f, 0.f, 0.f };
    const Camera camera(eye, 0.f, 0.f);
    const float ground[3] = { 120.f, -40.f, 31.f };
    Palette palette;
    // A grass tuft: ONE bone. Its pivot spans no height at all, which is exactly
    // why the old size gate (height from the bone pivots) refused every tuft in
    // the game and the HUD said 0 swaying with a one-bone model drawn.
    palette.Place(camera, 0, ground);
    const auto bones = AnalyzeVegetationBones(reinterpret_cast<const float(*)[4]>(palette.registers.data()),
        kRegisters, camera.view, camera.position);
    Check(bones.valid && bones.bones == 1, "a one-bone tuft is a valid palette");
    Check(Near(bones.height, 0.0, 1e-6), "and its bone pivots span no height at all");

    // The model's own vertices, from its base to its tip: 1.6 yards tall and a
    // third of a yard across.
    std::vector<VegetationGeometrySample> samples;
    for (unsigned i = 0; i < 8; ++i)
    {
        VegetationGeometrySample sample;
        sample.position[0] = (i % 2) ? .18f : -.18f;
        sample.position[1] = (i % 3) ? .12f : -.12f;
        sample.position[2] = 1.6f * float(i) / 7.f;
        sample.bone = 0;
        samples.push_back(sample);
    }
    const auto measured = MeasureVegetationGeometry(samples.data(), unsigned(samples.size()),
        reinterpret_cast<const float(*)[4]>(palette.registers.data()), 1, camera.view, camera.position);
    Check(measured.valid && measured.samples == samples.size(), "the draw's own vertices measure the model");
    Check(Near(measured.SpanZ(), 1.6, 1e-3), "the measured height is the model's real height");
    Check(Near(measured.Footprint(), .36, 1e-3), "and the measured footprint is its real footprint");
    Check(Near(measured.lowest[2], ground[2], 1e-3), "measured from the ground the model is placed on");
    Check(measured.SpanZ() > bones.height, "which is information the bone pivots never had");

    // A model-space vertex that references a bone the batch did not reveal is
    // clamped to the last block instead of reading past the bound.
    VegetationGeometrySample beyond = samples.front();
    beyond.bone = 40;
    float clamped[3]{};
    Check(VegetationVertexToWorld(beyond, reinterpret_cast<const float(*)[4]>(palette.registers.data()), 1,
              camera.view, camera.position, clamped),
        "a vertex bound to an unrevealed bone is clamped, not dropped");
    Check(Near(clamped[2], ground[2], 1e-3), "and lands where the last revealed bone puts it");

    // The point a lean rotates about is where the model is PLACED - block 0 -
    // and not the lowest vertex of whichever batch is being drawn: two batches of
    // one tree rotating about two different points is the failure that pulls a
    // trunk away from its canopy.
    float origin[3]{};
    Check(VegetationModelOrigin(reinterpret_cast<const float(*)[4]>(palette.registers.data()), camera.view,
              camera.position, origin),
        "the model has a placement point");
    Check(Near(origin[0], ground[0], 1e-3) && Near(origin[1], ground[1], 1e-3) && Near(origin[2], ground[2], 1e-3),
        "which is block 0's world position");

    // Nothing to measure, nothing claimed.
    Check(!MeasureVegetationGeometry(nullptr, 3, reinterpret_cast<const float(*)[4]>(palette.registers.data()), 1,
              camera.view, camera.position).valid,
        "no samples measure nothing");
    Check(!MeasureVegetationGeometry(samples.data(), 0, reinterpret_cast<const float(*)[4]>(palette.registers.data()),
              1, camera.view, camera.position).valid,
        "an empty batch measures nothing");
    std::vector<VegetationGeometrySample> broken(2, samples.front());
    broken[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    const auto partial = MeasureVegetationGeometry(broken.data(), unsigned(broken.size()),
        reinterpret_cast<const float(*)[4]>(palette.registers.data()), 1, camera.view, camera.position);
    Check(partial.valid && partial.samples == 1, "a broken vertex is dropped, not propagated");
    Pass("model size: measured from the draw's own vertices, and pivoted on its placement point");
}

// ---------------------------------------------------------------------------
// Which draws may be leaned at all
// ---------------------------------------------------------------------------
static void TestShaderAdmission()
{
    // The client's M2 model layout: the projection in c2..c5, exactly as
    // DrawCallClassifier and CameraCapture's sky fallback read it.
    float model[6][4] = { { 1.5f, 0.f, 0.f, 0.f }, { 0.f, 1.5f, 0.f, 0.f }, { 1.5f, 0.f, 0.f, 0.f },
        { 0.f, 1.5f, 0.f, 0.f }, { 0.f, 0.f, 0.f, 0.f }, { 0.f, 0.f, 1.f, 0.f } };
    Check(IsM2ModelConstants(model), "the client's model projection identifies an M2 draw");
    float terrain[6][4] = { { 0.f, 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f, 0.f }, { 1.5f, 0.f, 0.f, 0.f },
        { 0.f, 1.5f, 0.f, 0.f }, { 0.f, 0.f, 0.2f, 0.f }, { 0.f, 0.f, 0.f, 1.f } };
    Check(!IsM2ModelConstants(terrain), "a layout whose projection is not the model row is not a model");
    float transposed[6][4] = { { 0.f, 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f, 0.f }, { 0.f, 1.5f, 0.f, 0.f },
        { 1.5f, 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f, 0.f }, { 0.f, 0.f, 1.f, 0.f } };
    Check(!IsM2ModelConstants(transposed), "and neither is a projection with its rows the other way round");
    float broken[6][4]{};
    for (int i = 0; i < 6; ++i) for (int j = 0; j < 4; ++j) broken[i][j] = model[i][j];
    broken[3][3] = std::numeric_limits<float>::quiet_NaN();
    Check(!IsM2ModelConstants(broken), "NaN constants prove nothing");
    Check(!IsM2ModelConstants(nullptr), "and no constants at all prove nothing");

    // The world's own projection, read over the same nine registers
    // DrawCallClassifier reads it over: c4..c7 column major, c6[3] == 1 and
    // c7[2] < 0. A draw whose constants are the terrain's is never a plant.
    float world[9][4] = { {}, {}, {}, {}, { 1.5f, 0.f, 0.f, 0.f }, { 0.f, 1.5f, 0.f, 0.f },
        { 0.f, 0.f, 0.f, 1.f }, { 0.f, 0.f, -.2f, 0.f }, {} };
    Check(IsWmoOrTerrainConstants(world), "the WMO/terrain projection identifies a piece of the world");
    float modelNine[9][4] = { { 1.f, 0.f, 0.f, 0.f }, { 0.f, 1.f, 0.f, 0.f }, { 1.5f, 0.f, 0.f, 0.f },
        { 0.f, 1.5f, 0.f, 0.f }, { 0.f, 0.f, 0.f, 0.f }, { 0.f, 0.f, 1.f, 0.f }, {}, {}, {} };
    Check(!IsWmoOrTerrainConstants(modelNine),
        "and a model's projection is not mistaken for it, however the rest of the file reads");
    float brokenWorld[9][4]{};
    for (int i = 0; i < 9; ++i) for (int j = 0; j < 4; ++j) brokenWorld[i][j] = world[i][j];
    brokenWorld[7][2] = std::numeric_limits<float>::quiet_NaN();
    Check(!IsWmoOrTerrainConstants(brokenWorld), "a NaN constant proves nothing either");
    Check(!IsWmoOrTerrainConstants(nullptr), "and neither does no constants at all");

    for (uint64_t hash : kRigidM2Shaders)
        Check(IsRigidM2Shader(hash), "a verified rigid M2 shader is accepted");
    for (uint64_t hash : kUnskinnedM2Shaders)
    {
        Check(!IsRigidM2Shader(hash), "an unskinned model variant is not a rigid one");
        Check(IsUnskinnedM2Shader(hash), "but it is an accepted model variant in its own right");
    }
    Check(!IsRigidM2Shader(0) && !IsRigidM2Shader(~0ull), "an unproven shader is not");
    Check(!IsUnskinnedM2Shader(0) && !IsUnskinnedM2Shader(~0ull),
        "and no captured list accepts a hash it does not name");
    for (uint64_t hash : kNonModelShaders)
    {
        Check(IsNonModelShader(hash), "a pass that must never be leaned is refused");
        Check(!IsRigidM2Shader(hash), "and it is not on the rigid list as well");
    }
    Check(!IsNonModelShader(0) && !IsNonModelShader(~0ull), "an unknown shader is not a known non-model");
    Pass("shader admission: the model layout or a captured variant, and never the shadow/UI/water passes");
}

// ---------------------------------------------------------------------------
// Sway sampling
// ---------------------------------------------------------------------------
static WindState Air(WindDirection direction, float strength, float dt = 1.f / 60.f)
{
    WindSystem system;
    WindSettings settings;
    settings.dynamic = false;
    settings.direction = direction;
    settings.strength = strength;
    system.Configure(settings);
    return system.Update({}, dt);
}

static VegetationSway SampleAt(const VegetationWind& vegetation, const WindState& wind,
    float x, float y, float height, float distance = 0.f)
{
    (void)wind; // the state was already consumed by Update(); Sample reads it back
    return vegetation.Sample(x, y, height, 0x1234ull, distance);
}

static void TestFieldAndSampling()
{
    VegetationWind vegetation;
    auto wind = Air(WindDirection::East, 1.f);
    vegetation.Update(wind);
    // Gust response off: the lean follows the shared downwind heading exactly.
    VegetationSettings steady = vegetation.Settings();
    steady.gust = 0.f;
    VegetationWind steadyVegetation;
    steadyVegetation.Configure(steady);
    steadyVegetation.Update(wind);
    const auto sample = SampleAt(steadyVegetation, wind, 120.f, -300.f, 3.f);
    Check(sample.directionX == wind.direction.x && sample.directionY == wind.direction.y,
        "with no gust response the lean follows the shared downwind heading exactly");
    Check(sample.Leaning() && sample.angleDegrees <= steadyVegetation.Settings().maxAngle,
        "a breeze leans a bush, within the ceiling");
    Check(std::fabs(std::hypot(sample.directionX, sample.directionY) - 1.f) < 1e-5f,
        "the lean direction is a unit vector");

    // Gust response on: the heading wanders inside a cone around the shared
    // wind. A gust is never a second wind direction - the mean travel is the
    // same vector the waves and clouds use.
    const auto wandered = SampleAt(vegetation, wind, 120.f, -300.f, 3.f);
    const float alignment = wandered.directionX * wind.direction.x + wandered.directionY * wind.direction.y;
    Check(wandered.Leaning() && alignment > .9f &&
          std::fabs(std::hypot(wandered.directionX, wandered.directionY) - 1.f) < 1e-4f,
        "a gust only tilts the heading, it never replaces the wind direction");

    // Determinism: same state, same answer, no ordering effect.
    const auto repeat = SampleAt(vegetation, wind, 120.f, -300.f, 3.f);
    Check(repeat.angleDegrees == wandered.angleDegrees && repeat.directionX == wandered.directionX,
        "sampling is a pure function of state, position and size");

    // Discontinuity scan across the world, including the field's wrap period.
    const VegetationSettings settings = vegetation.Settings();
    float previous = -1.f, largest = 0.f;
    for (int step = 0; step < 30000; ++step)
    {
        const float x = -7000.f + float(step) * .5f;
        const auto value = SampleAt(vegetation, wind, x, -300.f, 1.f).angleDegrees;
        Check(std::isfinite(value) && value >= 0.f && value <= settings.maxAngle, "the field stays bounded");
        if (previous >= 0.f) largest = std::max(largest, std::fabs(value - previous));
        previous = value;
    }
    Check(largest < .30f * settings.maxAngle,
        "the gust field is continuous across the world, including its wrap period");

    // Time continuity and calm-air life: the field keeps evolving without wind.
    VegetationWind calm;
    VegetationSettings calmSettings = calm.Settings();
    calmSettings.gust = 1.f;
    calm.Configure(calmSettings);
    auto calmWind = Air(WindDirection::North, 0.f);
    calm.Update(calmWind);
    const auto first = SampleAt(calm, calmWind, 10.f, 20.f, 1.f);
    for (int frame = 0; frame < 3600; ++frame)
    {
        calmWind = Air(WindDirection::North, 0.f);
        calm.Update(calmWind);
    }
    const auto later = SampleAt(calm, calmWind, 10.f, 20.f, 1.f);
    Check(first.angleDegrees == 0.f && later.angleDegrees == 0.f,
        "a calm wind never leans anything, however long it runs");
    Check(calm.FieldX() != 0.f || calm.FieldY() != 0.f, "the field still evolves while the air is calm");

    // Gusts travel downwind: with an east wind the sample point drifts west.
    VegetationWind travelling;
    VegetationSettings travellingSettings = travelling.Settings();
    travellingSettings.gust = 1.f;
    travelling.Configure(travellingSettings);
    auto eastWind = Air(WindDirection::East, 2.f);
    travelling.Update(eastWind);
    const float startX = travelling.FieldX(), startY = travelling.FieldY();
    for (int frame = 0; frame < 600; ++frame)
    {
        eastWind = Air(WindDirection::East, 2.f);
        travelling.Update(eastWind);
    }
    // An east wind travels west, which is +Y in the map axes the wind system
    // uses, and the sampled pattern has to move that way with it.
    Check(travelling.FieldY() > startY + 10.f, "the gust pattern travels downwind with the shared wind");
    Check(std::fabs(travelling.FieldX() - startX) < 20.f, "and does not wander across it");

    // Time continuity of the applied angle.
    VegetationWind moving;
    auto movingWind = Air(WindDirection::SouthEast, 1.4f);
    moving.Update(movingWind);
    float previousAngle = -1.f, largestStep = 0.f;
    for (int frame = 0; frame < 3600; ++frame)
    {
        movingWind = Air(WindDirection::SouthEast, 1.4f);
        moving.Update(movingWind);
        const auto value = SampleAt(moving, movingWind, 40.f, 40.f, 1.f).angleDegrees;
        if (previousAngle >= 0.f) largestStep = std::max(largestStep, std::fabs(value - previousAngle));
        previousAngle = value;
    }
    Check(largestStep < .25f * moving.Settings().maxAngle, "the angle never jumps between frames");
    Pass("gust field: continuous in the world and in time, downwind-travelling, calm air stays still");
}

static void TestStrengthSizesAndGates()
{
    VegetationWind vegetation;
    VegetationSettings settings = vegetation.Settings();
    settings.gust = 0.f;      // steady mean lean, so the size classes are comparable
    settings.flutter = 0.f;
    vegetation.Configure(settings);
    auto wind = Air(WindDirection::North, 1.f);
    vegetation.Update(wind);

    const float grass = SampleAt(vegetation, wind, 0.f, 0.f, .4f).angleDegrees;
    const float bush = SampleAt(vegetation, wind, 0.f, 0.f, 2.5f).angleDegrees;
    const float tree = SampleAt(vegetation, wind, 0.f, 0.f, 18.f).angleDegrees;
    Check(grass > bush && bush > tree, "small plants lean further than trees in the same air");
    Check(grass <= settings.maxAngle + 1e-4f, "the ceiling holds for the smallest plant");
    Check(tree > 0.f, "a tree still leans");

    // Taller plants put their tips further downwind.
    const auto move = [&](float height) {
        const auto s = SampleAt(vegetation, wind, 0.f, 0.f, height);
        return double(height) * std::sin(s.angleDegrees * 0.017453292519943295);
    };
    Check(move(18.f) > move(2.5f), "a tree's canopy travels further than a bush top");

    // A storm leans harder than a breeze; a stronger wind eventually saturates
    // at the ceiling for grass but never for a tree.
    auto storm = Air(WindDirection::North, 3.f);
    VegetationWind strong;
    strong.Configure(settings);
    strong.Update(storm);
    const float stormGrass = SampleAt(strong, storm, 0.f, 0.f, .4f).angleDegrees;
    const float stormTree = SampleAt(strong, storm, 0.f, 0.f, 18.f).angleDegrees;
    Check(stormGrass >= grass && stormTree > tree && stormTree <= settings.maxAngle,
        "wind strength raises the lean and the ceiling still holds");

    // The ceiling is honoured for arbitrary settings and absurd inputs.
    VegetationSettings extreme = settings;
    extreme.sway = 3.f; extreme.plantGain = 2.f; extreme.treeGain = 2.f; extreme.maxAngle = 30.f;
    VegetationWind clamped;
    clamped.Configure(extreme);
    clamped.Update(storm);
    for (float height : { .05f, .35f, 1.f, 40.f, 400.f })
        for (float distance : { 0.f, 10.f, 500.f })
        {
            const auto value = SampleAt(clamped, storm, 500.f, -20.f, height, distance);
            Check(std::isfinite(value.angleDegrees) && value.angleDegrees >= 0.f &&
                  value.angleDegrees <= extreme.maxAngle, "no setting or size can exceed the ceiling");
        }
    Check(SampleAt(clamped, storm, std::numeric_limits<float>::quiet_NaN(), 0.f, 2.f).angleDegrees == 0.f,
        "a NaN world position cannot produce a lean");

    // Size class gains do what the sliders say.
    VegetationSettings gains = settings;
    gains.plantGain = 0.f; gains.treeGain = 1.f; gains.maxAngle = 30.f;
    VegetationWind plantsOff;
    plantsOff.Configure(gains);
    plantsOff.Update(wind);
    Check(!SampleAt(plantsOff, wind, 0.f, 0.f, .5f).Leaning(), "PLANT GAIN 0 stops grass and bushes");
    Check(SampleAt(plantsOff, wind, 0.f, 0.f, 20.f).Leaning(), "and leaves trees alone");
    gains.plantGain = 1.f; gains.treeGain = 0.f;
    VegetationWind treesOff;
    treesOff.Configure(gains);
    treesOff.Update(wind);
    Check(!SampleAt(treesOff, wind, 0.f, 0.f, 20.f).Leaning(), "TREE GAIN 0 stops trees");
    Check(SampleAt(treesOff, wind, 0.f, 0.f, .5f).Leaning(), "and leaves the undergrowth alone");

    // Range fade.
    Check(SampleAt(vegetation, wind, 0.f, 0.f, 2.f, settings.distance * .5f).fade == 1.f,
        "sway is at full strength inside 75% of the range");
    Check(SampleAt(vegetation, wind, 0.f, 0.f, 2.f, settings.distance).angleDegrees == 0.f,
        "sway ends at the range limit");
    float lastFade = 2.f;
    for (float distance = 0.f; distance <= settings.distance; distance += 3.f)
    {
        const float fade = SampleAt(vegetation, wind, 0.f, 0.f, 2.f, distance).fade;
        Check(fade <= lastFade + 1e-5f, "the range fade only ever decreases");
        lastFade = fade;
    }
    // Flutter is a shimmer, and it belongs to the small plants.
    VegetationSettings shimmer = settings;
    shimmer.flutter = 2.f; shimmer.maxAngle = 30.f; shimmer.gust = 0.f;
    VegetationWind fluttering;
    fluttering.Configure(shimmer);
    auto flutterWind = Air(WindDirection::West, .6f);
    fluttering.Update(flutterWind);
    float grassLow = 1e9f, grassHigh = -1e9f, treeLow = 1e9f, treeHigh = -1e9f;
    for (int frame = 0; frame < 600; ++frame)
    {
        flutterWind = Air(WindDirection::West, .6f);
        fluttering.Update(flutterWind);
        const float small = SampleAt(fluttering, flutterWind, 0.f, 0.f, .5f).angleDegrees;
        const float tall = SampleAt(fluttering, flutterWind, 0.f, 0.f, 20.f).angleDegrees;
        grassLow = std::min(grassLow, small); grassHigh = std::max(grassHigh, small);
        treeLow = std::min(treeLow, tall); treeHigh = std::max(treeHigh, tall);
    }
    Check(grassHigh - grassLow > treeHigh - treeLow, "grass shimmers more than a trunk does");

    // The link switches: master wind, the vegetation link, the effect itself.
    for (int mode = 0; mode < 4; ++mode)
    {
        WindSettings windSettings;
        windSettings.dynamic = false;
        windSettings.strength = 1.5f;
        windSettings.enabled = mode != 1;
        windSettings.affectVegetation = mode != 2;
        WindSystem system;
        system.Configure(windSettings);
        const auto state = system.Update({}, 1.f / 60.f);
        VegetationSettings own = settings;
        own.enabled = mode != 3;
        VegetationWind linked;
        linked.Configure(own);
        linked.Update(state);
        const auto value = SampleAt(linked, state, 0.f, 0.f, 1.f, 0.f).angleDegrees;
        if (mode == 0) Check(value > 0.f && state.DrivesVegetation(), "everything on leans");
        else Check(value == 0.f, "wind off, link off or effect off restores untouched models");
    }
    Pass("strength, size classes, gains, range fade, flutter and the three switches");
}

static void TestWindDebugLine()
{
    auto wind = Air(WindDirection::East, 1.25f);
    wind.affectVegetation = true;
    const auto lines = WindDebugLines(wind);
    Check(lines[4].find("Plants:") != std::string::npos, "the wind HUD names its vegetation link");
    Check(lines[4].find("Plants: linked") != std::string::npos, "and reports it as linked");
    wind.affectVegetation = false;
    Check(WindDebugLines(wind)[4].find("Plants: legacy") != std::string::npos,
        "and reports the legacy state when unlinked");

    // The vegetation rows. The first in-client report of this feature was "the
    // HUD says 0 swaying and nothing moves", and the gates that refused every
    // draw were the ones that counted nothing - so what the rows say, and how
    // short they are, is pinned here rather than left to a comment.
    VegetationSettings settings;
    VegetationStatus status;
    settings.enabled = false;
    auto rows = VegetationDebugLines(status, settings, wind);
    Check(rows.size() == 1 && rows[0] == "Plants: off (F7)", "an off effect says so in one line");
    settings.enabled = true;
    rows = VegetationDebugLines(status, settings, wind);
    Check(rows.size() == 1 && rows[0].find("legacy") != std::string::npos,
        "an unlinked wind leaves the single row the HUD had before this feature");
    wind.affectVegetation = true;

    // The model path ships OFF, and what a player reads with it off is that the
    // trees and bushes are off - in words. The alternative on this row is a
    // "0 leaning of 40 drew" that reads as a broken feature.
    rows = VegetationDebugLines(status, settings, wind);
    Check(rows.size() == 1 && rows[0].find("Trees & bushes: off (F7)") != std::string::npos &&
          rows[0].find("model draw(s) left alone") != std::string::npos,
        "the model path's own switch says so in words, not with a counter that reads as zero");
    Check(rows[0].size() <= kVegetationHudRowBudget, "and that row fits the box like the others");
    settings.swayModels = true;

    // Every row of the block, joined: the census is allowed to continue on the
    // next row when everything refuses at once (checked below), so what a label
    // says and the order it is said in are checked across the whole block.
    const auto all = [](const std::vector<std::string>& lines) {
        std::string joined;
        for (const auto& line : lines) { if (!joined.empty()) joined += ' '; joined += line; }
        return joined;
    };
    const auto fits = [](const std::vector<std::string>& lines) {
        for (const auto& line : lines) if (line.size() > kVegetationHudRowBudget) return false;
        return true;
    };

    status.swaying = 34; status.batches = 51; status.candidates = 51;
    status.skippedMaterial = 812; status.maxAngle = 12.3f; status.lastMeasured = 4.2f;
    rows = VegetationDebugLines(status, settings, wind);
    Check(rows.size() == 2, "an active session gets the state row and the gate row");
    Check(rows[0].find("34 leaning of 51 drew") != std::string::npos,
        "the state row counts what leans out of the foliage draws that arrived");
    Check(rows[0].find("812 solid") != std::string::npos,
        "and how many solid model draws it passed over, which is what a solid-material bug looks like");
    Check(rows[1].find("none refused") != std::string::npos, "the gate row is quiet while nothing is refused");
    Check(rows[1].find("h 4.2 yd") != std::string::npos, "and always shows the size the gate looked at");
    // The one number on the row that is not a refusal: how many draws arrived
    // as the client's skinned model path at all. "No gate refused anything" and
    // "this client does not draw this field as models" look identical on a row
    // of refusals, and the answer for a player is completely different.
    status.modelDraws = 214;
    rows = VegetationDebugLines(status, settings, wind);
    Check(rows[1].find("h 4.2 yd, 214 models") != std::string::npos,
        "the gate row says how many model draws the frame carried, refused or not");
    Check(fits(rows), "and the head with it still fits the box");
    status.modelDraws = 0;
    // The two counters that used to be one. `flat` is fixed-function T&L - a
    // pass with no vertex shader, which is never the client's model path - and
    // `never` is a pass this project has identified as never a model. Neither is
    // a plant, and neither may be printed as one: a row that says "vs 180" for
    // the UI, the rain and the shadow casters is the row that made an in-client
    // report unreadable.
    status.skippedFixedFunction = 3; status.skippedNeverModel = 41; status.skippedShader = 0;
    rows = VegetationDebugLines(status, settings, wind);
    Check(all(rows).find("flat 3") != std::string::npos,
        "a fixed-function pass is named as one, not as an unproven shader");
    Check(all(rows).find("never 41") != std::string::npos,
        "and a pass this project already refused as never-a-model is named as that");
    status.skippedFixedFunction = 0; status.skippedNeverModel = 0;
    // "Waiting" is neither leaning nor refused: a model that is measured,
    // plant-shaped and standing still but has not repeated itself in a second
    // frame yet. Without it a row can read "0 leaning of 40 drew, 0 solid" and
    // every gate at zero, which is the one report nobody can act on.
    status.pending = 12;
    rows = VegetationDebugLines(status, settings, wind);
    Check(rows[0].find("34 leaning of 51 drew, 12 waiting, 812 solid") != std::string::npos,
        "waiting for the second frame is said out loud, next to what leans");
    Check(rows.size() == 2 && rows[1].find("wait") == std::string::npos,
        "and is never offered as a refusal on the gate row");
    status.pending = 0;
    rows = VegetationDebugLines(status, settings, wind);
    Check(rows[0].find("waiting") == std::string::npos, "a settled scene does not print it at all");

    // A frame that refuses at a couple of gates: the whole census on one row.
    status.skippedShader = 812; status.skippedPalette = 3; status.learnedShaders = 1;
    rows = VegetationDebugLines(status, settings, wind);
    Check(rows.size() == 2, "a few refusals still fit one row");
    Check(all(rows).find("vs 812") != std::string::npos && all(rows).find("pal 3") != std::string::npos &&
          all(rows).find("new 1") != std::string::npos, "and names every gate that refused, with its count");
    Check(all(rows).find("mat") == std::string::npos, "without offering a solid trunk as a refusal");
    // The two refusals a "nothing sways" report turns on and that no other line
    // can tell apart: the world drawn as scenery (correct, and thousands of
    // draws of it) and the depth-only passes, the client's own shadow map among
    // them, whose palette is in the light's space and must never be leaned.
    status.skippedWorld = 400; status.skippedDepthOnly = 900;
    status.skippedCalm = 7; status.skippedWrite = 1;
    rows = VegetationDebugLines(status, settings, wind);
    Check(all(rows).find("world 400") != std::string::npos, "the row separates the world's own draws from the census");
    Check(all(rows).find("depth 900") != std::string::npos, "and the depth-only passes, which are never leaned");
    // The two refusals that are not about the model at all: a plant inside the
    // range that the wind's own gust field leaves with nothing to apply, and a
    // write the device refused. Neither may hide behind `far` or behind the
    // palette.
    Check(all(rows).find("calm 7") != std::string::npos,
        "a plant the wind leaves with no angle is not reported as out of range");
    Check(all(rows).find("write 1") != std::string::npos, "and a refused write is named as one");
    Check(all(rows).find("depth 900") < all(rows).find("vs 812"),
        "in the order the gates run in: a pass that writes no colour is settled before a shader is judged");
    Check(all(rows).find("vs 812") < all(rows).find("world 400"),
        "a shader is judged before the palette that settles a world-shaped binding");
    Check(all(rows).find("world 400") < all(rows).find("pal 3"),
        "and the world is named before the plain palette refusals it is settled with");
    // Everything refusing at once does not fit one row, and a clipped row is
    // exactly as useful as the "0" the player reported: the census continues on
    // the next row, prefixed, in the same order.
    status.skippedLayout = 3200; status.skippedGeometry = 12; status.skippedDepthOnly = 900;
    status.skippedCamera = 3; status.skippedViewport = 1; status.skippedShape = 4; status.skippedSize = 2;
    status.skippedAnimated = 5; status.skippedRange = 6; status.unbounded = 2;
    status.skippedGround = 3;
    status.skippedFixedFunction = 2; status.skippedNeverModel = 44;
    rows = VegetationDebugLines(status, settings, wind);
    Check(rows.size() > 2, "a frame that refuses at every gate continues on the next row");
    Check(rows[2].find(kMoreGatesPrefix) == 0, "and the continuation says that it is one");
    for (const char* label : { "flat", "decl", "world", "vs", "geo", "depth", "never", "cam", "pal", "shape",
                               "size", "ground", "move", "far", "calm", "write", "new" })
        Check(all(rows).find(std::string(label) + " ") != std::string::npos,
            "every gate that refused is on the rows, even when they do not fit one");
    // The row that names the build. An in-client report of "it does not work"
    // arrived with counters that read exactly like the fixed build's, from a
    // client still running the previous d3d9.dll, and nothing in the picture
    // could say so. The stamp is the loaded DLL's own file time, so it needs no
    // version to keep in step with the source.
    buildStamp = "2026-10-15 18:32";
    rows = VegetationDebugLines(status, settings, wind);
    Check(rows.back() == "ModernWoWRenderer 2026-10-15 18:32",
        "the debug rows stamp the build that drew them");
    Check(rows.back().size() <= kVegetationHudRowBudget, "and the stamp fits the box too");
    buildStamp = nullptr;
    rows = VegetationDebugLines(status, settings, wind);
    Check(all(rows).find("ModernWoWRenderer") == std::string::npos,
        "and a session that published no build stamps nothing");
    // The rows have to fit the HUD's own box: 600 logical pixels at a 16 pixel
    // font, drawn 10 pixels in from the left and clipped 18 short of the right.
    // A clipped diagnostic is exactly as useful as the "0" the player reported.
    buildStamp = "2026-10-15 18:32";
    Check(fits(VegetationDebugLines(status, settings, wind)),
        "every vegetation HUD row fits the box it is drawn in");
    buildStamp = nullptr;
    Pass("the vegetation HUD: what leans, what arrived, and the gate that is refusing");
}

// ---------------------------------------------------------------------------
// Configuration, F7 and wiring
// ---------------------------------------------------------------------------
static std::map<std::wstring, int> VegetationValues(const VegetationSettings& s)
{
    return {{L"Enabled",s.enabled},{L"SwayPercent",int(std::lround(s.sway * 100))},
        {L"GustPercent",int(std::lround(s.gust * 100))},{L"FlutterPercent",int(std::lround(s.flutter * 100))},
        {L"MaxAngleDegrees",int(std::lround(s.maxAngle))},{L"PlantPercent",int(std::lround(s.plantGain * 100))},
        {L"TreePercent",int(std::lround(s.treeGain * 100))},{L"DistanceYards",int(std::lround(s.distance))},
        {L"SwayModels",s.swayModels},
        {L"GrassBlades",s.grassBlades},{L"SwayAnimatedModels",s.animatedModels},{L"DebugView",s.debugView}};
}

static void TestConfigurationAndWiring()
{
    const auto ini = Read("GraphicsEffects.ini");
    Check(ini.find("\n[Vegetation]\n") != ini.npos, "the shipped INI documents the vegetation section");
    std::set<std::string> iniKeys;
    std::istringstream iniLines(ini);
    std::string line, group;
    while (std::getline(iniLines, line))
    {
        const auto start = line.find_first_not_of(" \t\r");
        if (start == line.npos || line[start] == ';' || line[start] == '#') continue;
        if (line[start] == '[') { group = line.substr(start, line.find(']', start) - start + 1); continue; }
        const auto equals = line.find('=', start);
        if (equals == line.npos) continue;
        const auto end = line.find_last_not_of(" \t", equals - 1);
        auto normalized = group + "/" + line.substr(start, end - start + 1);
        for (char& ch : normalized) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        Check(iniKeys.emplace(normalized).second,
            "shipped INI must not define a setting twice in one section (case-insensitive)");
    }

    const auto section = ini.substr(ini.find("[Vegetation]"),
        ini.find("[VolumetricClouds]") - ini.find("[Vegetation]"));
    std::map<std::wstring, int> readDefaults;
    const auto defaults = VegetationValues(ReadVegetationSettings(
        [&](const wchar_t* key, int value) { readDefaults[key] = value; return value; }));
    Check(defaults == VegetationValues(VegetationSettings{}),
        "configuration defaults match simulation defaults");
    Check(defaults == readDefaults, "the reader's fallbacks are the shipped defaults");
    Check(VegetationSettings{}.swayModels == false && VegetationSettings{}.grassBlades == true,
        "the shipped defaults are the ones the reports asked for: grass sways, the model path - the trees and the "
        "bushes - does not");
    unsigned count = 0;
    for (const auto& control : tuningoverlay::items)
    {
        if (!control.section || std::wstring(control.section) != L"Vegetation") continue;
        ++count;
        Check(defaults.at(control.key) == control.value, "F7 and runtime have identical defaults");
        const std::wstring key(control.key);
        const std::string entry = std::string(key.begin(), key.end()) + "=" + std::to_string(control.value) + "\n";
        Check(section.find(entry) != std::string::npos, "shipped INI and F7 default match");
        if (control.kind == tuningoverlay::KIND_SLIDER)
            for (const auto& [asked, expected] : { std::pair{control.lo - 1000, control.lo },
                                                   {control.hi + 1000, control.hi} })
            {
                const auto values = VegetationValues(ReadVegetationSettings(
                    [&](const wchar_t* k, int fallback) { return key == k ? asked : fallback; }));
                Check(values.at(key) == expected, "INI and F7 range endpoints match");
            }
    }
    Check(count == defaults.size(), "every supported vegetation option has an F7 control");

    // The link switch itself is part of the wind section, so the wind
    // regression's "every wind option has a control" rule keeps holding.
    const auto windIni = ini.substr(ini.find("[Wind]"), ini.find("[VolumetricClouds]") - ini.find("[Wind]"));
    Check(windIni.find("\nAffectVegetation=1\n") != windIni.npos,
        "the wind section ships the vegetation link, like waves and clouds");
    unsigned linkControls = 0;
    for (const auto& control : tuningoverlay::items)
        if (control.section && std::wstring(control.section) == L"Wind" &&
            std::wstring(control.key) == L"AffectVegetation")
        {
            ++linkControls;
            Check(control.value == 1, "the shipped link default is on");
        }
    Check(linkControls == 1, "the link has exactly one F7 control");

    // Production wiring. The sway must be a per-draw scope inside every draw
    // hook, and one update per frame next to the wind snapshot - never a
    // second wind, clock or weather reader.
    const auto main = Read("ModernWoWRenderer.cpp");
    size_t scopes = 0, position = 0;
    while ((position = main.find("vegetation::Scope vegetationSway", position)) != main.npos) { ++scopes; position += 1; }
    Check(scopes == 4, "all four draw entry points lean vegetation");
    const auto snapshot = main.find("nextFrame.wind=g_windSystem.Update");
    const auto vegetation = main.find("vegetation::Update(nextFrame.wind)");
    Check(snapshot != main.npos && vegetation != main.npos && vegetation > snapshot,
        "vegetation consumes the published snapshot, in the same place the waves and clouds do");
    Check(main.find("vegetation::Reset(d)") != main.npos, "device reset drops the vegetation draw caches");
    Check(main.find("ReloadVegetationTuning()") != main.npos, "F7, F12 and location changes reload it");
    // Each of the four draw entry points describes its own batch, so none of them
    // can pass an argument in the wrong order.
    for (const char* factory : { "vegetation::PlainDraw(", "vegetation::IndexedDraw(",
                                 "vegetation::PlainUpDraw(", "vegetation::IndexedUpDraw(" })
    {
        size_t uses = 0, at = 0;
        while ((at = main.find(factory, at)) != main.npos) { ++uses; at += 1; }
        Check(uses == 1, "each draw entry point describes its batch exactly once");
    }
    Check(main.find("vegetation::DebugLines()") != main.npos,
        "the vegetation HUD rows are drawn by the wind debug text");

    const auto sway = Read("src/Effects/VegetationSway.h");
    const auto wind = Read("src/Environment/VegetationWind.h");
    const auto geometry_ = Read("src/Core/M2DrawGeometry.h");
    Check(sway.find("GetVertexShaderConstantF(renderer::kM2BoneBaseRegister") != sway.npos &&
          sway.find("SetVertexShaderConstantF(renderer::kM2BoneBaseRegister") != sway.npos,
          "the lean is written over the client's own bone palette registers");
    Check(sway.find("m_saved") != sway.npos && sway.find("restore") != sway.npos,
        "and the client's matrices are put back after the draw");
    // The bug this rewrite exists to fix. The palette bound used to be taken from
    // the constant file itself - read c31 upwards while the blocks still look
    // like matrices - which counts the PREVIOUS draw's bones as this model's, so
    // a six-bone bush was analysed as a seventy-five-bone skeleton and its height
    // came from bone pivots that span nothing at all. Both facts now come from
    // the draw's own vertices, and the palette is never read wider than the
    // model's own bones.
    Check(sway.find("detail::BatchGeometry") != sway.npos,
        "the model's bone bound and its own vertices are read from the draw");
    Check(sway.find("MeasureVegetationGeometry") != sway.npos, "the size gate measures those vertices");
    Check(sway.find("VegetationModelOrigin") != sway.npos, "and the lean pivots on the model's placement point");
    Check(sway.find("ReadPalette(device, bound, registers)") != sway.npos,
        "the palette is read bounded, never as the whole constant file");
    // The defect this build removes, and the two things that make the roll-back
    // provable. A palette remembered from an earlier batch of the same instance
    // is not what the device holds at this draw - the client re-uploads bones in
    // the middle of a frame - so the read is per draw, the write is armed before
    // it is attempted so a refused write is rolled back too, and the first leans
    // of a session have their roll-back read back and compared.
    Check(sway.find("g_palette") == sway.npos && sway.find("PaletteCache") == sway.npos &&
          sway.find("registersRead") == sway.npos,
        "no palette is remembered from another batch: the roll-back is this draw's own values");
    Check(sway.find("m_wrote = true;") < sway.find("SetVertexShaderConstantF(renderer::kM2BoneBaseRegister, leaned[0]") &&
          sway.find("SetVertexShaderConstantF(renderer::kM2BoneBaseRegister, leaned[0]") != sway.npos,
        "the roll-back is armed before the write is attempted, not after it succeeded");
    Check(sway.find("GetVertexShaderConstantF(renderer::kM2BoneBaseRegister, back[0], m_count)") != sway.npos &&
          sway.find("if (std::memcmp(back[row], m_saved[row], sizeof(float[4])) != 0) ++differing;") != sway.npos &&
          sway.find("std::memcmp(back[row], m_saved[row]") < sway.find("char text[416]") &&
          sway.find("Status().restoreMismatch = differing;") != sway.npos,
        "and what went back is read back and compared, because a write that did not land is the one failure a "
        "report of a deformed shoulder cannot otherwise be told apart from a gate that let the wrong draw through");
    Check(Read("src/Environment/VegetationWind.h").find("unsigned restoreMismatch = 0;") != std::string::npos &&
          Read("src/Environment/WindDebug.h").find("report(\"restore\", status.restoreMismatch);") != std::string::npos,
        "a mismatch is counted where the player can see it, not only in the log");
    // Inert when off, and every refusal counted. Ordering is the whole guarantee
    // of the first, so it is pinned here rather than left to a comment: the
    // disabled, unlinked, up-front (particle/UI), unverified-shader and
    // non-model-declaration paths all return above the first palette read.
    const auto scopeStart = sway.find("Scope(IDirect3DDevice9* device, bool skip");
    const auto firstPaletteRead = sway.find("GetVertexShaderConstantF(renderer::kM2BoneBaseRegister", scopeStart);
    const auto linkGate = sway.find("if (!frame.wind.DrivesVegetation()) return;", scopeStart);
    const auto upGate = sway.find("if (draw.verticesUP) return;", scopeStart);
    const auto shaderGate = sway.find("detail::ShaderAccepted(device, frame, shader.hash)", scopeStart);
    const auto declarationRead = sway.find("IDirect3DVertexDeclaration9* declaration = detail::BoundDeclaration(device);",
        scopeStart);
    const auto layoutGate = sway.find("detail::Layout(device, declaration, frame.frameIndex, true, false)", scopeStart);
    // And the branch that hands an unskinned foliage draw to the blade path is
    // one of those early paths: it must run before any palette is read, because
    // the draws it serves are exactly the ones that have no palette at all.
    const auto bladeBranch = sway.find("grassblade::ConsiderDraw(device, frame, shader.hash, shader.object, *declared, draw,",
        scopeStart);
    Check(scopeStart != sway.npos && firstPaletteRead != sway.npos &&
          linkGate != sway.npos && linkGate < firstPaletteRead &&
          upGate != sway.npos && upGate < firstPaletteRead &&
          shaderGate != sway.npos && shaderGate < firstPaletteRead &&
          declarationRead != sway.npos && declarationRead < firstPaletteRead &&
          layoutGate != sway.npos && layoutGate < firstPaletteRead &&
          bladeBranch != sway.npos && bladeBranch < firstPaletteRead,
        "the disabled, unlinked and non-model paths return before the first palette read, and the blade path is one of "
        "them");
    for (const char* counter : { "candidates", "modelDraws", "skippedMaterial", "skippedCamera",
                                 "skippedViewport", "skippedFixedFunction", "skippedNeverModel",
                                 "skippedShader", "skippedWorld", "skippedLayout", "skippedGeometry",
                                 "skippedDepthOnly", "skippedPalette", "skippedShape", "skippedSize",
                                 "skippedAnimated", "skippedRange", "skippedCalm", "skippedWrite",
                                 "unbounded", "learnedShaders", "pending",
                                 "swaying", "batches", "maxAngle", "lastHeight", "lastMeasured" })
        Check(sway.find(std::string("Status().") + counter) != sway.npos,
            "every counter the HUD can report is incremented by the device half");
    // The one thing a counter cannot say is WHICH shader it counted, so both
    // verdicts on a variant this build had not seen go to the renderer's log by
    // hash - and the log is wired from the same place the settings are reloaded.
    Check(sway.find("ReportShader") != sway.npos &&
          sway.find("a model draw was refused at the shader gate") != sway.npos &&
          sway.find("Add it to kRigidM2Shaders") != sway.npos,
        "an unproven shader is reported by hash, accepted or refused");
    Check(main.find("vegetation::logLine=Log;") != main.npos,
        "and those reports reach the renderer's own log");
    Check(sway.find("g_reportedRootBlock") != sway.npos &&
          sway.find("did not hold a bone block for a model draw: %.3f %.3f %.3f %.3f") != sway.npos &&
          sway.find("unsigned(renderer::kM2BoneBaseRegister)") != sway.npos,
        "the register a failed palette read looked at is named once, with its numbers");
    // The caps under the caches. A dropped instance is measured again from
    // scratch, so they are pinned as a number rather than left to drift: a
    // client can place a lot of models.
    Check(sway.find("kMaxInstances = 2048") != sway.npos && sway.find("kMaxGeometryProfiles = 2048") != sway.npos,
        "the instance and geometry caches hold a forest's worth of models");
    // What the third in-client report cost: a mount came apart under the player.
    // A creature is animated, and every one of these is a witness that a plant is
    // not: its bones are compact, its placement point travels, its pose changes
    // and its mesh is re-uploaded. The lean must be impossible for it on all four
    // counts - and the batch that proves a model is a plant is not always the
    // batch that gets leaned, so the solid shortcut is checked too.
    Check(sway.find("const bool merged = renderer::VegetationSkeletonIsSpatial(bones);") != sway.npos &&
          wind.find("return bones.spread > kVegetationMergedSpread || bones.height > kVegetationTallSkeleton;") != sway.npos &&
          sway.find("if (bound > renderer::kVegetationMaxBones && !merged)") != sway.npos &&
          sway.find("if (solidModel.bones > renderer::kVegetationMaxBones && !solidSpatial)") != sway.npos,
        "a batch of many plants is not a creature: the cap is applied to compact skeletons only, and to the bound "
        "the draw's own vertices give (the palette reader's count can never exceed its own limit)");
    Check(sway.find("const float footprintLimit = merged ? renderer::kVegetationMaxFootprint") != sway.npos,
        "and a merged patch is allowed to be wide and low, which is what a field's grass draw is");
    Check(sway.find("renderer::kVegetationPlacedEpsilon") != sway.npos &&
          sway.find("instance->movedFrame = frame.frameIndex;") != sway.npos &&
          sway.find("renderer::kVegetationMoveCooldownFrames") != sway.npos,
        "a model whose placement point travels is being carried by the client, and is refused for a while after");
    const auto wind2 = Read("src/Environment/VegetationWind.h");
    Check(sway.find("instance->stable < renderer::kVegetationStableFrames") != sway.npos &&
          wind2.find("kVegetationStableFrames = 6") != sway.npos,
        "the static proof is a run of identical frames, not one: an idle creature breathes");
    Check(sway.find("const float moved = std::sqrt((bones.pivot[0] - known->pivot[0])") != sway.npos &&
          sway.find("const bool poseMoved = known->pose.valid && known->poseBound == bound &&") != sway.npos &&
          sway.find("if (poseMoved && !settings.animatedModels)") != sway.npos,
        "and the solid batch of a leaning model - the saddle, the armour, the held object - is leaned only "
        "while that model is provably standing still");
    Check(sway.find("const float bonePivot[3] = { block[0][3], block[1][3], block[2][3] };") != sway.npos &&
          sway.find("const auto boneLean = bone == 0 ? lean") != sway.npos,
        "every bone turns about its own position, so a merged patch bends plant by plant");
    Check(sway.find("renderer::VegetationCarriesManyPlants(bones, height)") != sway.npos &&
          wind.find("return bones.bones > 1 && bones.spread > kVegetationMergedSpread &&") != sway.npos &&
          sway.find("? bones.bones : 1;") != sway.npos &&
          sway.find("Status().swaying += std::max(instance.plants, 1u);") != sway.npos,
        "a merged draw counts the plants it carries, so a whole field does not read as two leaning");
    // Three one-shot lines that make the next report answerable without a second
    // round: what a declaration this reader cannot measure actually is, that a
    // merged patch was accepted (and how big it was), and the first draw that
    // actually leaned. A player's log has to say WHICH of the two problems it is.
    Check(sway.find("g_reportedMerged") != sway.npos &&
          sway.find("a draw carrying many plants was accepted") != sway.npos,
        "the first merged patch accepted is named in the log, with its size and its plant count");
    Check(sway.find("g_reportedLean") != sway.npos &&
          sway.find("the first draw leaned this session") != sway.npos,
        "and the first draw that leaned is named too - a plant that moves when it should not is a report about that "
        "line");
    Check(sway.find("texture is %s [%u texels examined]") != sway.npos &&
          sway.find("opaque (scenery, not foliage)") != sway.npos,
        "a declaration this reader cannot measure reports whether its texture is foliage-like");
    // The build stamp is published by the Windows half (the overlay rows are
    // portable, so the time itself is read where Windows is available) and it is
    // both stamped on the rows and written to the log at startup.
    Check(main.find("renderer::buildStamp = g_buildStamp.c_str();") != main.npos &&
          main.find("GetFileAttributesExW") != main.npos &&
          main.find("FileTimeToSystemTime") != main.npos,
        "the build stamp is the loaded dll's own file time");
    const auto hud = Read("src/Environment/WindDebug.h");
    // The witnesses a standing character cannot pass by not moving: its items hang
    // from joints and its body references a whole skeleton, and both verdicts are
    // about the MODEL, so they are recorded and never re-opened.
    Check(sway.find("if (renderer::VegetationHangsFromItsOrigin(measured, origin, height))") != sway.npos &&
          sway.find("renderer::VegetationHangsFromItsOrigin(solidMeasured, bones.pivot, known->height)") != sway.npos &&
          sway.find("instance->notPlant = true;") != sway.npos &&
          sway.find("if (instance->notPlant)") != sway.npos &&
          sway.find("if (owner->notPlant)") != sway.npos &&
          wind.find("return origin[2] - measured.lowest[2] > slack;") != sway.npos &&
          hud.find("report(\"ground\", status.skippedGround);") != sway.npos,
        "a model that hangs from its placement point, or whose vertices reference a body's skeleton, is not a plant, "
        "and no batch of it ever leans");
    for (const char* label : { "flat", "decl", "world", "vs", "geo", "depth", "never", "cam", "pal", "shape",
                               "size", "ground", "move", "far", "calm", "write", "new" })
        Check(hud.find(std::string("report(\"") + label + "\"") != hud.npos,
            "every gate the device half counts has a name on the HUD row");
    Check(hud.find("inline const char* buildStamp") != hud.npos &&
          hud.find("\"ModernWoWRenderer %s\", buildStamp") != hud.npos,
        "the debug rows name the build that drew them");
    // The admission rework. A shader list captured from one build of the client
    // cannot gate another build - which is what made a working feature read as
    // "0 swaying" on a machine whose client ships different binaries - so a draw
    // now has to prove itself: bone indices in its own declaration (the client's
    // skinned model path, which terrain, WMOs, particles and the UI never use),
    // or the M2 model projection in its constants, or a captured variant.
    Check(sway.find("renderer::IsUnskinnedM2Shader") != sway.npos,
        "both captured variant lists admit a draw on sight");
    Check(sway.find("renderer::IsWmoOrTerrainConstants") != sway.npos,
        "and the world's own projection makes a draw a suspect rather than scenery");
    Check(layoutGate < shaderGate,
        "the declaration is read first: it is the proof the draw is a model at all");
    Check(geometry_.find("out.valid = out.position && out.skinned") != std::string::npos,
        "a declaration without bone indices is not a model draw, whatever its shader is");
    // Which shader, and which declaration. Both come from the DEVICE, and that
    // is not a detail: the client binds its own passes in cached state blocks
    // that never reach the SetVertexShader/SetVertexDeclaration hooks, so the
    // mirror describes an earlier draw - or no shader at all - while a model is
    // being drawn. Every draw in the frame then reads as "not a model", which is
    // the one refusal a player cannot argue with: it is what the two in-client
    // reports of "2 leaning, nothing moves" looked like from the HUD.
    Check(sway.find("detail::BoundVertexShader(device)") != sway.npos &&
          sway.find("device->GetVertexShader(bound.GetAddressOf())") != sway.npos &&
          sway.find("renderer::ShaderCache::Instance().GetShaderHash(bound.Get())") != sway.npos,
        "the shader a draw is judged by is the one the device is drawing with");
    Check(sway.find("detail::BoundDeclaration(device)") != sway.npos &&
          sway.find("device->GetVertexDeclaration(bound.GetAddressOf())") != sway.npos,
        "and the declaration is the one the device is drawing through");
    Check(sway.find("frame.frameIndex, true, false)") != sway.npos,
        "a declaration that is not the model path is named in the log, mirror or not - and still read, because the "
        "blade path needs what it says about the texture coordinate");
    Check(sway.find("renderer::g_trackedState.vsHash, mirrorMaterial") == sway.npos &&
          sway.find("detail::Layout(renderer::g_trackedState.currentVDecl") == sway.npos,
        "no gate decides a draw's identity from the state mirror");
    // A shader with no identity is not a refusal: it is judged by the constants
    // it binds, on that draw, and never cached - a profile under hash 0 would
    // let the first draw the runtime could not identify decide for every other.
    const auto unidentified = sway.find("if (hash == 0) return UnidentifiedShader(device);");
    Check(unidentified != sway.npos && sway.find("g_shaders.find(hash)") > unidentified &&
          sway.find("IsWmoOrTerrainConstants(constants) ? Admission::World : Admission::Granted") != sway.npos,
        "a shader whose bytecode will not come back is judged per draw, and never cached under 0");
    // The shader gate's refusals are counted whatever the mirror says about the
    // material: the declaration above proved the draw is a model, so a refusal
    // here is about a model and must not be the mirror's to hide. The verdicts
    // are counted apart, because they are three different bugs: a pass this
    // project has always refused as never-a-model, a model draw this build could
    // not prove at all, and the world's own projection - which is carried to the
    // palette gate rather than acted on here.
    Check(sway.find("++Status().skippedNeverModel;") != sway.npos &&
          sway.find("if (admission == detail::Admission::Unknown)") != sway.npos &&
          sway.find("++Status().skippedShader;") != sway.npos,
        "a refusal of a model draw is counted, mirror or not, and counted apart");
    const auto worldSuspect = sway.find("const bool worldSuspect = admission == detail::Admission::World;");
    Check(worldSuspect != sway.npos && sway.find("if (worldSuspect) ++Status().skippedWorld;") > worldSuspect &&
          sway.find("else ++Status().skippedPalette;") > worldSuspect,
        "and the world's own projection is settled by the palette, by name, at the palette gate");
    Check(sway.find("of the world; it is left alone unless its own bone palette says otherwise") != sway.npos,
        "a world-shaped binding with a bone block is a model drawn through the world path, not scenery");
    // The material verdict. A foliage draw that the state mirror believes is
    // solid is never revealed as an instance at all, and the client's own passes
    // arrive through cached state blocks that never reach the SetRenderState
    // hook - so the gate asks the device, exactly like the material cache does.
    Check(sway.find("detail::TransparentMaterial(device)") != sway.npos,
        "the cutout test reads the device, not the mirror");
    Check(sway.find("GetRenderState(D3DRS_ALPHABLENDENABLE") != sway.npos &&
          sway.find("GetRenderState(D3DRS_ALPHATESTENABLE") != sway.npos,
        "both transparent modes are read from that device call");
    Check(sway.find("renderer::g_trackedState.alphaBlend || renderer::g_trackedState.alphaTest") != sway.npos,
        "and the mirror only answers when the device cannot be asked at all");
    const auto materialGate = sway.find("const bool stateCutout = detail::TransparentMaterial(device);");
    const auto cameraGate = sway.find("if (!frame.cameraValid || !frame.viewRawValid)");
    Check(materialGate != sway.npos && materialGate > layoutGate && materialGate > shaderGate &&
          materialGate < cameraGate,
        "and it is asked once per model draw that got past the declaration, the depth test, the shader verdict and "
        "the palette, so the read is paid for the draws that can still become a plant");
    Check(sway.find("const bool mirrorMaterial =") != sway.npos,
        "with the free state mirror left to the early counters");
    // Colour writes. The client's native shadow map skins in the LIGHT's space,

    // so leaning it with the frame camera's wind would rotate a shadow by the
    // wrong air; a depth fill and a prepass write no colour either. The state
    // mirror cannot answer this - the client's own passes arrive through cached
    // state blocks, which never reach the SetRenderState hook - so the device is
    // asked, once per draw that is still a candidate.
    const auto colourGate = sway.find("if (!detail::WritesColor(device))", scopeStart);
    const auto firstLean = sway.find("Lean(frame, *instance", scopeStart);
    Check(sway.find("GetRenderState(D3DRS_COLORWRITEENABLE") != sway.npos,
        "the colour-write proof is read from the device, not from the state mirror");
    Check(sway.find("colorWrite") == sway.npos,
        "and nothing keeps a mirror of it that could go stale");
    Check(colourGate != sway.npos && colourGate < firstLean && colourGate > layoutGate,
        "no draw is leaned before its colour writes are known - and the depth pass is settled before any shader, "
        "palette or texture is read");
    // The refusal that swallowed a whole field. On the client this was reported
    // from, grass arrives with alpha test and alpha blend BOTH off - its alpha
    // test is compiled into the pixel shader - so the state test alone called 238
    // model draws "solid" while the player stood in a field of it. The draw's own
    // stage 0 texture is the witness that cannot be turned off that way, and the
    // two answers are combined per model: state OR texture.
    Check(sway.find("detail::TextureIsCutout(device)") != sway.npos &&
          sway.find("renderer::TextureCutout::Cutout") != sway.npos,
        "a draw the state calls solid is asked about its own texture");
    Check(sway.find("device->GetTexture(0, base.GetAddressOf())") != sway.npos &&
          sway.find("D3DLOCK_READONLY") != sway.npos,
        "read as the device has it bound, read-only, so the client's texture is never disturbed");
    Check(sway.find("ClassifyTextureCutout(") != sway.npos,
        "and decoded by the portable classifier, which is what the suite above tests");
    // Read from the palette ladder's own state test onwards: the blade branch
    // ahead of the ladder is allowed to probe its texture (that is how it
    // decides), but inside the ladder the order the report demanded still holds.
    const auto statePosition = sway.find("const bool stateCutout = detail::TransparentMaterial(device);");
    const auto ladderTexture = sway.find("detail::TextureIsCutout(device)", statePosition);
    Check(statePosition != sway.npos && ladderTexture != sway.npos && ladderTexture > statePosition &&
          sway.find("instanceCutout = stateCutout ?") != sway.npos,
        "the state answers the cut-out half, and the texture is asked the half the state cannot answer: whether it "
        "is green");
    Check(Read("src/Effects/GrassBladeSway.h").find("if (texture.verdict != renderer::TextureCutout::Cutout)") !=
          std::string::npos &&
          Read("src/Effects/GrassBladeSway.h").find("!texture.foliage") == std::string::npos &&
          Read("src/Effects/GrassBladeSway.h").find("greenPercent = texture.greenFraction();") != std::string::npos,
        "a blade card is judged on being cut out at all and never on its colour - an autumn field has to bend too - "
        "while the colour it did have is reported once, for the log");
    Check(sway.find("if (instanceCutout && !texture.foliage && texture.readable())") != sway.npos &&
          sway.find("g_reportedNotFoliage") != sway.npos &&
          Read("src/Core/TextureCutout.h").find("probe.foliage = probe.verdict == TextureCutout::Cutout") != sway.npos,
        "a cut-out that is not green is gear (a cape, a shoulder pad, a saddle, a sword) and is refused, with the "
        "share of green texels named in the log");
    Check(sway.find("instance->swaying = true;") != sway.npos &&
          sway.find("if (!owner || !owner->swaying)") != sway.npos,
        "leaning is the proof a model's other batches look for, and it stops the moment the model moves");
    Check(sway.find("SwayingByRoot") == sway.npos,
        "no gate decides a draw's model from its placement point: a rider sits at their mount's point");
    Check(sway.find("detail::MeasuredInstance(") != sway.npos &&
          sway.find("renderer::VegetationInstanceKey(solidOrigin, solidBasis, solidModel.model)") != sway.npos,
        "a solid batch is leaned only once its own vertex buffer proves it is a batch of that model");
    // The order the gates run in, which is what makes the row readable: a pass
    // that writes no colour is settled before a texture is locked, the root block
    // proves the draw is a model before the material read is paid for, and the
    // texture is only examined where nothing else could answer.
    const auto rootGate = sway.find("GetVertexShaderConstantF(renderer::kM2BoneBaseRegister", scopeStart);
    // The first place in a frame a texture can be locked is the blade branch, and
    // it is behind the same colour-write test - written on the same line as its
    // own other gates - so the shadow map is still never examined either way.
    const auto bladeGuard = sway.find("frame.wind.DrivesVegetation() && detail::WritesColor(device))", scopeStart);
    const auto textureGate = sway.find("detail::TextureIsCutout(device)", scopeStart);
    const auto neverVerdict = sway.find("++Status().skippedNeverModel;");
    Check(colourGate != sway.npos && bladeGuard != sway.npos && bladeGuard < textureGate,
        "the colour-write test runs before any texture is locked: the shadow map is never examined");
    Check(rootGate != sway.npos && rootGate > neverVerdict &&
          sway.find("detail::TextureIsCutout(device)", rootGate) != sway.npos,
        "the root block - the proof the draw is a model at all - runs before the material read");
    Check(sway.find("g_textureCutouts") != sway.npos && sway.find("g_textureCutouts.clear();") != sway.npos,
        "one texture is examined once, and a device reset drops what was learned about it");
    // The other side of the same coin: a model draw that is neither transparent
    // by state nor cut out by texture names its shader once, so `solid` on the
    // HUD is still answerable from a player's log.
    Check(sway.find("g_reportedSolidTexture") != sway.npos &&
          sway.find("HUD counts") != sway.npos &&
          sway.find("these as `solid`") != sway.npos,
        "a solid model draw names its shader once, so `solid` can be diagnosed");
    // And the refusal that is NOT a verdict: a texture that could not be read at
    // all is not the same bug as a texture that was read and had no holes in it,
    // and the log has to say which one the gate row is counting.
    Check(sway.find("g_reportedUnreadableTexture") != sway.npos &&
          sway.find("no texture bound, not a texture, not level 0 ") != sway.npos &&
          sway.find("the HUD counts") == sway.npos,
        "a solid draw whose texture could not be read says that instead of claiming one without holes");
    Check(sway.find("if (texture.verdict == renderer::TextureCutout::Opaque && !g_reportedSolidTexture)") != sway.npos &&
          sway.find("else if (texture.verdict == renderer::TextureCutout::Unknown && !g_reportedUnreadableTexture)") != sway.npos,
        "the two are told apart by the verdict the decoder returned, not by a guess");
    // And the register itself, once, when c31 is not a bone block: `pal` on the
    // HUD is otherwise a dead end from a player's machine - zeros mean the
    // client keeps its matrix elsewhere, a projection means the draw is not a
    // model, and a different stride shows up in the numbers.
    Check(sway.find("kVegetationFootprintToHeight * height") != sway.npos,
        "the shape gate is the plant one: a fence is long and low, a bush is not");
    // The reader itself: the two facts the whole fix rests on are read from the
    // draw's own vertices, which is the only place they exist.
    const auto geometry = geometry_;
    Check(geometry.find("D3DDECLTYPE_UBYTE4N") != std::string::npos,
        "bone indices are read whether the client declares them as bytes or as normalised bytes");
    Check(geometry.find("D3DDECLTYPE_FLOAT4") != std::string::npos &&
          geometry.find("D3DDECLTYPE_FLOAT3") != std::string::npos,
        "and a position is read whether it is three floats or four");
    Check(geometry.find("DescribeVertexDeclaration") != std::string::npos &&
          geometry.find("D3DDECL_END") != std::string::npos,
        "a declaration that cannot be read is named in the renderer's log, in D3D's own terms");
    Check(geometry.find("GetStreamSource") != std::string::npos &&
          geometry.find("kM2GeometryMaxBytes") != std::string::npos,
        "the reader takes the vertex buffer the draw itself is using, within a fixed budget");
    Check(Read("src/Core/M2ShaderProbe.h").find("kM2BoneBaseRegister") != std::string::npos,
        "the shader probe and the palette share one base register");
    Check(Read("README.md").find("[Vegetation]") != std::string::npos,
        "the README lists the vegetation settings section");
    Check(Read("docs/vegetation-wind.md").find("barely") != std::string::npos ||
          Read("docs/vegetation-wind.md").find("never") != std::string::npos,
        "the feature documents what it refuses to touch");
    Pass("INI uniqueness, F7 defaults/ranges, the link switch and the production wiring");
}


// ---------------------------------------------------------------------------
// The cut-out decoder
// ---------------------------------------------------------------------------
// A client whose shader system compiles the alpha test into the pixel shader
// (a `discard` on the sampled alpha) draws its grass with alpha blending and
// alpha testing BOTH off, so the render state cannot tell a leaf from a crate.
// The texture can: a plant is drawn as a card cut out of a transparent image,
// while a prop is drawn with an image that has no transparent texel at all.
// The device half locks the texture once and hands the bytes here; every
// decode below is a pure function, and every one of them is checked - because a
// decoder that invents a transparent texel now puts a solid prop in the wind.
static void WriteDxt1Block(uint8_t* block, bool transparent, uint32_t indices = 0xFFFFFFFFu)
{
    // colour0 <= colour1 is the mode that carries 1-bit alpha; above it a block
    // is opaque by definition whatever the indices say.
    const uint16_t colour0 = transparent ? 0x0000 : 0xFFFF;
    const uint16_t colour1 = transparent ? 0xFFFF : 0x0000;
    block[0] = uint8_t(colour0 & 0xFF); block[1] = uint8_t(colour0 >> 8);
    block[2] = uint8_t(colour1 & 0xFF); block[3] = uint8_t(colour1 >> 8);
    for (int i = 0; i < 4; ++i) block[4 + i] = uint8_t((indices >> (8 * i)) & 0xFF);
}

static std::vector<uint8_t> DxtImage(unsigned width, unsigned height, unsigned blockBytes,
    const std::function<void(uint8_t*, unsigned, unsigned)>& fill)
{
    const unsigned blocksPerRow = (width + 3) / 4, blockRows = (height + 3) / 4;
    const size_t pitch = size_t(blocksPerRow) * blockBytes;
    std::vector<uint8_t> image(pitch * blockRows, 0);
    for (unsigned by = 0; by < blockRows; ++by)
        for (unsigned bx = 0; bx < blocksPerRow; ++bx)
            fill(image.data() + by * pitch + bx * blockBytes, bx, by);
    return image;
}

static void TestTextureCutoutDecode()
{
    const unsigned size = 32; // 64 DXT blocks, 1024 texels
    const size_t dxt1Pitch = size_t(size / 4) * 8;

    // DXT1, the format WoW draws most of its foliage with. Every index three in
    // the alpha mode is a hole; a block in the opaque mode has none.
    {
        auto image = DxtImage(size, size, 8, [](uint8_t* block, unsigned, unsigned) { WriteDxt1Block(block, false); });
        const auto probe = ClassifyTextureCutout(TextureFormat::Dxt1, image.data(), dxt1Pitch, size, size, image.size());
        Check(probe.verdict == TextureCutout::Opaque && probe.transparent == 0 && probe.texels == size * size,
            "an all-opaque DXT1 texture is not a cut-out");
    }
    {
        auto image = DxtImage(size, size, 8, [](uint8_t* block, unsigned, unsigned) { WriteDxt1Block(block, true); });
        const auto probe = ClassifyTextureCutout(TextureFormat::Dxt1, image.data(), dxt1Pitch, size, size, image.size());
        Check(probe.verdict == TextureCutout::Cutout && probe.transparent == size * size && Near(probe.fraction, 1.0),
            "every texel of index three is a hole: the texture is a cut-out");
    }
    // The share matters: one transparent block of sixty-four is an edge
    // artefact, not a leaf.
    {
        auto image = DxtImage(size, size, 8, [](uint8_t* block, unsigned bx, unsigned by)
            { WriteDxt1Block(block, bx == 0 && by == 0); });
        const auto probe = ClassifyTextureCutout(TextureFormat::Dxt1, image.data(), dxt1Pitch, size, size, image.size());
        Check(probe.transparent == 16 && probe.verdict == TextureCutout::Opaque,
            "one transparent block in sixty-four is below the share a cut-out needs");
    }
    {
        // Half the blocks in the alpha mode with every index a hole: half the
        // texels, which is the share a leaf texture has.
        auto image = DxtImage(size, size, 8, [](uint8_t* block, unsigned, unsigned by)
            { WriteDxt1Block(block, by < 4); });
        const auto probe = ClassifyTextureCutout(TextureFormat::Dxt1, image.data(), dxt1Pitch, size, size, image.size());
        Check(probe.verdict == TextureCutout::Cutout && probe.transparent == 512,
            "a texture that is a quarter of the way to half holes is a cut-out");
    }
    // Padding is not texels. A 17x17 image is carried by twenty-five blocks, so
    // most of what a naive decoder would examine does not exist - and if those
    // phantom texels counted as transparent, every odd-sized opaque texture in
    // the world would read as foliage.
    {
        const unsigned odd = 17;
        const size_t oddPitch = size_t((odd + 3) / 4) * 8;
        auto image = DxtImage(odd, odd, 8, [](uint8_t* block, unsigned, unsigned) { WriteDxt1Block(block, false); });
        // Every block's three-in-four padding slots are holes; the real texels
        // are not.
        const unsigned blocksPerRow = (odd + 3) / 4;
        for (unsigned by = 0; by < (odd + 3) / 4; ++by)
            for (unsigned bx = 0; bx < blocksPerRow; ++bx)
            {
                uint8_t* block = image.data() + by * oddPitch + bx * 8;
                block[0] = 0x00; block[1] = 0x00; block[2] = 0xFF; block[3] = 0xFF;
                uint32_t indices = 0;
                for (unsigned j = 0; j < 4; ++j)
                    for (unsigned i = 0; i < 4; ++i)
                        if (bx * 4 + i >= odd || by * 4 + j >= odd) indices |= 3u << (2 * (j * 4 + i));
                for (int i = 0; i < 4; ++i) block[4 + i] = uint8_t((indices >> (8 * i)) & 0xFF);
            }
        const auto probe = ClassifyTextureCutout(TextureFormat::Dxt1, image.data(), oddPitch, odd, odd, image.size());
        Check(probe.texels == odd * odd && probe.transparent == 0 && probe.verdict == TextureCutout::Opaque,
            "texels outside the image are padding, never holes");
    }

    // DXT3: explicit four-bit alpha, two texels per byte. The threshold is the
    // same 128 every other format is compared to.
    {
        const size_t pitch = size_t(size / 4) * 16;
        auto image = DxtImage(size, size, 16, [](uint8_t* block, unsigned, unsigned)
            { for (int i = 0; i < 8; ++i) block[i] = 0xFF; });
        auto probe = ClassifyTextureCutout(TextureFormat::Dxt3, image.data(), pitch, size, size, image.size());
        Check(probe.verdict == TextureCutout::Opaque, "nibble fifteen is opaque DXT3");
        auto clear = DxtImage(size, size, 16, [](uint8_t* block, unsigned, unsigned)
            { for (int i = 0; i < 8; ++i) block[i] = 0x00; });
        probe = ClassifyTextureCutout(TextureFormat::Dxt3, clear.data(), pitch, size, size, clear.size());
        Check(probe.verdict == TextureCutout::Cutout && probe.transparent == size * size,
            "nibble zero is a hole in DXT3");
        auto seven = DxtImage(size, size, 16, [](uint8_t* block, unsigned, unsigned)
            { for (int i = 0; i < 8; ++i) block[i] = 0x77; });
        probe = ClassifyTextureCutout(TextureFormat::Dxt3, seven.data(), pitch, size, size, seven.size());
        Check(probe.verdict == TextureCutout::Cutout, "seven of fifteen is below the halfway mark");
        auto eight = DxtImage(size, size, 16, [](uint8_t* block, unsigned, unsigned)
            { for (int i = 0; i < 8; ++i) block[i] = 0x88; });
        probe = ClassifyTextureCutout(TextureFormat::Dxt3, eight.data(), pitch, size, size, eight.size());
        Check(probe.verdict == TextureCutout::Opaque, "and eight of fifteen is above it");
        // DXT2 carries the same alpha, premultiplied.
        probe = ClassifyTextureCutout(TextureFormat::Dxt2, clear.data(), pitch, size, size, clear.size());
        Check(probe.verdict == TextureCutout::Cutout, "DXT2 alpha is DXT3 alpha");
    }

    // DXT5: two endpoints and sixteen three-bit indices, in both of the
    // format's modes - the mode decides whether index six is a hole or white.
    {
        const size_t pitch = size_t(size / 4) * 16;
        const auto build = [&](uint8_t a0, uint8_t a1, uint8_t index) {
            return DxtImage(size, size, 16, [a0, a1, index](uint8_t* block, unsigned, unsigned)
            {
                block[0] = a0; block[1] = a1;
                uint64_t indices = 0;
                for (int i = 0; i < 16; ++i) indices |= uint64_t(index) << (3 * i);
                for (int i = 0; i < 6; ++i) block[2 + i] = uint8_t((indices >> (8 * i)) & 0xFF);
            });
        };
        auto image = build(255, 0, 0);
        auto probe = ClassifyTextureCutout(TextureFormat::Dxt5, image.data(), pitch, size, size, image.size());
        Check(probe.verdict == TextureCutout::Opaque, "index zero of a white-to-black DXT5 block is white");
        image = build(255, 0, 1);
        probe = ClassifyTextureCutout(TextureFormat::Dxt5, image.data(), pitch, size, size, image.size());
        Check(probe.verdict == TextureCutout::Cutout && probe.transparent == size * size,
            "index one is the other endpoint, and it is black");
        image = build(255, 0, 2);
        probe = ClassifyTextureCutout(TextureFormat::Dxt5, image.data(), pitch, size, size, image.size());
        Check(probe.verdict == TextureCutout::Opaque, "index two interpolates towards white, not black");
        image = build(0, 255, 6);
        probe = ClassifyTextureCutout(TextureFormat::Dxt5, image.data(), pitch, size, size, image.size());
        Check(probe.verdict == TextureCutout::Cutout, "in the other mode index six is a hole");
        image = build(0, 255, 7);
        probe = ClassifyTextureCutout(TextureFormat::Dxt5, image.data(), pitch, size, size, image.size());
        Check(probe.verdict == TextureCutout::Opaque, "and index seven is white");
    }

    // The unpacked formats, and the row pitch that is not the row width: the
    // padding D3D puts at the end of a row must never be read as texels.
    {
        const size_t rowBytes = size_t(size) * 4;
        const size_t pitch = rowBytes + 16; // padded rows, zero filled
        std::vector<uint8_t> image(pitch * size, 0);
        for (unsigned y = 0; y < size; ++y)
            for (unsigned x = 0; x < size; ++x)
                image[y * pitch + x * 4 + 3] = 255;
        auto probe = ClassifyTextureCutout(TextureFormat::A8R8G8B8, image.data(), pitch, size, size, image.size());
        Check(probe.verdict == TextureCutout::Opaque && probe.transparent == 0,
            "a row's padding is not a transparent texel of an A8R8G8B8 texture");
        for (unsigned y = 0; y < size; ++y)
            for (unsigned x = 0; x < size; ++x)
                image[y * pitch + x * 4 + 3] = 0;
        probe = ClassifyTextureCutout(TextureFormat::A8R8G8B8, image.data(), pitch, size, size, image.size());
        Check(probe.verdict == TextureCutout::Cutout && probe.transparent == size * size,
            "and an A8R8G8B8 texture with a zero alpha channel is a cut-out");
        std::fill(image.begin(), image.end(), 0);
        const size_t partial = rowBytes * 4; // a lock that handed over only four rows
        probe = ClassifyTextureCutout(TextureFormat::A8R8G8B8, image.data(), pitch, size, size, partial);
        Check(probe.texels == 3 * size && probe.verdict == TextureCutout::Unknown,
            "a lock that hands over part of an image is read whole rows at a time and never past what it gave - "
            "and too little of it proves nothing");
    }
    {
        const auto unpacked = [&](TextureFormat format, uint16_t value, size_t bytesPerTexel) {
            const size_t pitch = size_t(size) * bytesPerTexel;
            std::vector<uint8_t> image(pitch * size, 0);
            for (size_t i = 0; i + bytesPerTexel <= image.size(); i += bytesPerTexel)
                for (size_t b = 0; b < bytesPerTexel; ++b) image[i + b] = uint8_t((value >> (8 * b)) & 0xFF);
            return ClassifyTextureCutout(format, image.data(), pitch, size, size, image.size());
        };
        Check(unpacked(TextureFormat::X8R8G8B8, 0x0000, 4).verdict == TextureCutout::Opaque,
            "X8R8G8B8 has no alpha channel to be transparent with");
        Check(unpacked(TextureFormat::L8, 0x00, 1).verdict == TextureCutout::Opaque,
            "and neither has L8: black in it is not a hole");
        Check(unpacked(TextureFormat::A8, 0x00, 1).verdict == TextureCutout::Cutout &&
              unpacked(TextureFormat::A8, 0xFF, 1).verdict == TextureCutout::Opaque,
            "A8 is the alpha channel itself");
        Check(unpacked(TextureFormat::A1R5G5B5, 0x8000, 2).verdict == TextureCutout::Opaque &&
              unpacked(TextureFormat::A1R5G5B5, 0x0000, 2).verdict == TextureCutout::Cutout,
            "A1R5G5B5's one bit decides");
        Check(unpacked(TextureFormat::A4R4G4B4, 0xF000, 2).verdict == TextureCutout::Opaque &&
              unpacked(TextureFormat::A4R4G4B4, 0x7000, 2).verdict == TextureCutout::Cutout &&
              unpacked(TextureFormat::A4R4G4B4, 0x8000, 2).verdict == TextureCutout::Opaque,
            "A4R4G4B4's high nibble decides, either side of halfway");
    }

    // What a reader must never do: claim a verdict it did not read. An unknown
    // format, a texture too small to take a share over, and no texture at all
    // are all Unknown - and Unknown is not evidence of anything.
    {
        std::vector<uint8_t> bytes(4096, 0);
        Check(ClassifyTextureCutout(TextureFormat::Unknown, bytes.data(), 128, 32, 32, bytes.size()).verdict ==
            TextureCutout::Unknown, "a format this reader cannot decode proves nothing");
        const auto small = ClassifyTextureCutout(TextureFormat::A8R8G8B8, bytes.data(), 32, 8, 8, bytes.size());
        Check(small.verdict == TextureCutout::Unknown && small.texels == 64 && small.fraction == 1.f,
            "a texture too small to take a share over is classified as nothing at all");
        Check(ClassifyTextureCutout(TextureFormat::Dxt1, nullptr, 64, 32, 32, 0).verdict == TextureCutout::Unknown &&
              ClassifyTextureCutout(TextureFormat::Dxt1, bytes.data(), 0, 32, 32, bytes.size()).verdict == TextureCutout::Unknown &&
              ClassifyTextureCutout(TextureFormat::Dxt1, bytes.data(), 64, 0, 0, bytes.size()).verdict == TextureCutout::Unknown,
            "and neither is a texture that is not there");
    }

    // Bigger than the budget: the image is sampled across its whole height, so a
    // cut-out is still a cut-out, and the hash still names the bytes read.
    {
        const unsigned big = 1024;
        const size_t pitch = size_t(big / 4) * 8;
        auto image = DxtImage(big, big, 8, [](uint8_t* block, unsigned, unsigned) { WriteDxt1Block(block, true); });
        const auto probe = ClassifyTextureCutout(TextureFormat::Dxt1, image.data(), pitch, big, big, image.size());
        Check(probe.verdict == TextureCutout::Cutout && probe.texels < big * big && probe.texels >= 256,
            "an image larger than the scan budget is sampled, not skipped");
        Check(probe.hash != 0 && probe.hash != kTextureAlphaHashOffset && probe.hash != 0xFFFFFFFFFFFFFFFFull,
            "and the bytes that were read are hashed, so a texture can be named in the log");
    }
    // And the half of the material gate that the WXL tree module answers with a
    // model's file name: is this texture FOLIAGE? A plant's cut-out card is
    // green; a cape, a shoulder pad, a saddle and a sword are not green whatever
    // else they are, and that is what keeps gear out of a wind that cannot tell a
    // standing character from a bush.
    {
        const auto foliageBlock = [](uint8_t* block, bool green, bool cut) {
            // Endpoint 0 is black so the pair is always in the 1-bit-alpha order,
            // endpoint 1 is what the texels show, and index 3 is the hole. A "no
            // holes" block inverts the pair, which makes it opaque by DXT1's own
            // definition whatever the indices are.
            const uint16_t visible = cut ? (green ? 0x07E0 : 0x7BEF) : 0xFFFF;
            const uint16_t colour0 = cut ? 0x0000 : 0xFFFF;
            const uint16_t colour1 = cut ? visible : 0x0000;
            block[0] = uint8_t(colour0 & 0xFF); block[1] = uint8_t(colour0 >> 8);
            block[2] = uint8_t(colour1 & 0xFF); block[3] = uint8_t(colour1 >> 8);
            const uint32_t indices = cut ? 0x555555FFu : 0x00000000u; // texels 0-3 of 16 are holes
            for (int i = 0; i < 4; ++i) block[4 + i] = uint8_t((indices >> (8 * i)) & 0xFF);
        };
        const unsigned size = 64;
        const size_t pitch = size_t(size / 4) * 8;
        auto leaves = DxtImage(size, size, 8, [&](uint8_t* block, unsigned, unsigned) { foliageBlock(block, true, true); });
        auto metal = DxtImage(size, size, 8, [&](uint8_t* block, unsigned, unsigned) { foliageBlock(block, false, true); });
        auto solid = DxtImage(size, size, 8, [&](uint8_t* block, unsigned, unsigned) { foliageBlock(block, true, false); });
        const auto leaf = ClassifyTextureCutout(TextureFormat::Dxt1, leaves.data(), pitch, size, size, leaves.size());
        const auto plate = ClassifyTextureCutout(TextureFormat::Dxt1, metal.data(), pitch, size, size, metal.size());
        const auto wall = ClassifyTextureCutout(TextureFormat::Dxt1, solid.data(), pitch, size, size, solid.size());
        Check(leaf.verdict == TextureCutout::Cutout && leaf.foliage,
            "a green cut-out is a leaf: the card is drawn with holes in it and what is left is green");
        Check(plate.verdict == TextureCutout::Cutout && !plate.foliage,
            "a grey cut-out is a cloak or a shoulder pad, not a plant - the hole says card, the colour says what");
        Check(wall.verdict == TextureCutout::Opaque && !wall.foliage,
            "and a texture with no holes is never foliage whichever colour it is");
        Check(leaf.greenFraction() > .5f && plate.greenFraction() < .1f,
            "the share of green texels is what the log names, so a refusal can be read as a number");
        Check(ClassifyTextureCutout(TextureFormat::A8R8G8B8, leaves.data(), pitch, 4, 4, leaves.size()).foliage == false,
            "and a texture too small to judge is never called foliage");
        Check(leaf.greenFraction() > .5f && plate.greenFraction() < .1f,
            "the share of green texels is what the log names, so a refusal can be read as a number");
        Check(ClassifyTextureCutout(TextureFormat::A8R8G8B8, leaves.data(), pitch, 4, 4, leaves.size()).foliage == false,
            "and a texture too small to judge is never called foliage");
    }
    Pass("texture cut-out decode: every format a client uploads, and never a hole it did not have");
}


// ---------------------------------------------------------------------------
// One bone, one pivot: what a lean means inside a model
// ---------------------------------------------------------------------------
// The lean used to turn a whole draw about the model's placement point. That is
// right for a tuft with one bone and wrong for everything else a batch can be: a
// tree's branches and a batch the client merged a whole patch of grass into each
// carry their own bones, and turning them all about one point swings a patch of
// grass like a plank. Every bone now turns about its own position - which is
// exactly the same transform for a one-bone tuft, and a bend per plant, per
// branch and per joint for everything else.
static void TestPerBoneLean()
{
    const float up[3] = { 0.f, 0.f, 1.f };
    const float dir[3] = { 1.f, 0.f, 0.f };
    const float base[3] = { 1.f, 2.f, 3.f };
    const float branch[3] = { 4.f, 6.f, 8.f };
    const float angle = 12.f;

    float bone[2][3][4]{};
    MakePlacementBone(base, bone[0]);
    MakePlacementBone(branch, bone[1]);
    float out[2][3][4]{};
    for (unsigned i = 0; i < 2; ++i)
    {
        // The pivot of a bone is the translation column of its own block - which
        // is what the device half passes for every bone of a draw.
        const float own[3] = { bone[i][0][3], bone[i][1][3], bone[i][2][3] };
        const auto lean = BuildVegetationLean(own, up, dir, angle);
        ApplyVegetationLean(lean, bone[i], out[i]);
    }

    // Each bone stays exactly where it was: the geometry bound to it turns in
    // place instead of being carried to another plant's pivot.
    for (unsigned i = 0; i < 2; ++i)
        Check(Near(out[i][0][3], bone[i][0][3], 1e-4) && Near(out[i][1][3], bone[i][1][3], 1e-4) &&
              Near(out[i][2][3], bone[i][2][3], 1e-4),
            "a bone turns about its own position, so the plant it carries keeps its ground contact");
    Check(!Near(bone[0][0][3], bone[1][0][3], 1e-3),
        "which is a different transform for each of them - a merged patch bends plant by plant");
    // The rotation itself is the one rotation: same angle, same basis for both.
    for (unsigned i = 0; i < 2; ++i)
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                Check(Near(out[i][row][column], out[0][row][column], 1e-4),
                    "and every bone is turned by the same wind");

    // What the client would see for a tuft that is one bone: byte for byte the
    // lean this feature has always applied.
    {
        const auto modelLean = BuildVegetationLean(base, up, dir, angle);
        float modelOut[3][4]{};
        ApplyVegetationLean(modelLean, bone[0], modelOut);
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column)
                Check(Near(out[0][row][column], modelOut[row][column], 1e-5),
                    "a one-bone model is leaned exactly as before: the bone IS the placement point");
    }
    Pass("per-bone lean: every bone turns about its own position, one plant per bone");
}

// ---------------------------------------------------------------------------
// Many bones, one patch
// ---------------------------------------------------------------------------
// A client that merges a field's grass into a single draw hands us one bone per
// tuft: thousands of vertices, more bones than any creature, and a footprint far
// wider than it is tall. The rule that keeps a fence out of the wind would keep
// that field out with it, and the bone cap written for creatures reads it as a
// character. What tells them apart is where the bones are: a patch's bones are
// spread over the patch, a creature's all sit inside one body.
static void TestMergedPatchSkeleton()
{
    const float eye[3] = { -20.f, -30.f, 9.f };
    const Camera camera(eye, 35.f, -8.f);
    const float* view = camera.view; // world -> view, exactly as the device reads it
    float registers[kM2MaxBones * kM2RegistersPerBone][4]{};

    // Twenty tufts three yards apart, all of them half a yard tall: a patch, not
    // a body, and not a single plant either.
    for (unsigned tuft = 0; tuft < 20; ++tuft)
    {
        const float world[3] = { float(tuft % 5) * 3.f, float(tuft / 5) * 3.f, 0.f };
        MakeBone(camera, world, kNoRotation,
            reinterpret_cast<float(*)[4]>(registers[tuft * kM2RegistersPerBone]));
    }
    const auto patch = AnalyzeVegetationBones(registers, 20 * kM2RegistersPerBone, view, camera.position);
    Check(patch.valid && patch.bones == 20, "a merged patch validates every bone it carries");
    Check(patch.spread > 12.f, "and its bones are spread over the patch, not stacked in one body");
    Check(patch.spread > kVegetationMergedSpread, "which is what marks it as many plants rather than a creature");
    Check(patch.height < 1.f && patch.footprint > 12.f,
        "it is wide and low, exactly like the fence the shape rule was written for");

    // The same number of bones inside one body: a creature, and the spread says
    // so even though the count alone cannot.
    float compact[kM2MaxBones * kM2RegistersPerBone][4]{};
    for (unsigned bone = 0; bone < 20; ++bone)
    {
        const float world[3] = { float(bone % 4) * .2f, float(bone / 4) * .2f, float(bone % 3) * .3f };
        MakeBone(camera, world, kNoRotation,
            reinterpret_cast<float(*)[4]>(compact[bone * kM2RegistersPerBone]));
    }
    const auto creature = AnalyzeVegetationBones(compact, 20 * kM2RegistersPerBone, view, camera.position);
    Check(creature.valid && creature.bones == 20 && creature.spread < kVegetationMergedSpread,
        "twenty bones inside one body are a creature: compact, and refused by the bone cap");
    Check(creature.height < 1.f, "and no size rule would have caught it: only the count and the spread do");
    // The two things that carry more bones than a body are told apart by shape,
    // not by a bone count: a patch of grass is low and wide, a tree is tall. The
    // HUD counts the first as the plants it carries and the second as one plant.
    const auto skeleton = [](unsigned bones, float spread, float height) {
        renderer::VegetationBones skeleton{};
        skeleton.valid = true;
        skeleton.bones = bones;
        skeleton.spread = spread;
        skeleton.height = height;
        return skeleton;
    };
    Check(!renderer::VegetationSkeletonIsSpatial(skeleton(7, 1.f, 2.f)),
        "a bush is one body's worth of bones in one body's worth of space: compact and short");
    Check(renderer::VegetationSkeletonIsSpatial(skeleton(60, 40.f, 1.f)) &&
          renderer::VegetationCarriesManyPlants(skeleton(60, 40.f, 1.f), 1.f),
        "a field of grass merged into one draw is low and wide, so it is a patch of many plants");
    Check(renderer::VegetationSkeletonIsSpatial(skeleton(40, 3.f, 12.f)) &&
          !renderer::VegetationCarriesManyPlants(skeleton(40, 3.f, 12.f), 12.f),
        "a tree is tall: admitted with all its branches, and still one plant");
    Check(!renderer::VegetationCarriesManyPlants(skeleton(1, 40.f, 1.f), 1.f),
        "and one bone is one plant, however wide the patch it belongs to measures");
    // The two positive witnesses, which are what a standing character cannot pass
    // by not moving: a plant's skeleton is not a body's, and a plant's geometry
    // stands on its own origin instead of hanging from it.
    const auto measurement = [](float lowestZ, float highestZ) {
        renderer::VegetationMeasurement measurement{};
        measurement.valid = true;
        measurement.samples = 8;
        measurement.lowest[2] = lowestZ;
        measurement.highest[2] = highestZ;
        return measurement;
    };
    const float ground[3] = { 100.f, 200.f, 3.f };
    Check(!renderer::VegetationHangsFromItsOrigin(measurement(3.02f, 3.6f), ground),
        "a tuft whose vertices all sit at or above the point it is placed at is a plant growing out of the ground");
    Check(renderer::VegetationHangsFromItsOrigin(measurement(1.4f, 1.9f), ground),
        "a cape, a shoulder pad, a sword and a saddle reach BELOW the joint they are placed at, and that is what "
        "they are");
    Check(!renderer::VegetationHangsFromItsOrigin(measurement(2.8f, 3.6f), ground),
        "the slack is a plant's own body: a doodad whose origin sits a little inside its base still passes");
    Check(!renderer::VegetationHangsFromItsOrigin(renderer::VegetationMeasurement{}, ground),
        "and a measurement that could not be taken refuses nothing - a draw nobody could measure is left alone, "
        "never banned on a guess");
    Pass("merged patch skeleton: a field of grass in one draw is not a creature, and a creature is not a patch");
}

// ---------------------------------------------------------------------------
// The grass the client draws as blades: a shader text, the block inserted into
// it, and the constants that block reads. All of it is text and numbers, which
// is the point - the injection can be checked in a garden of shader texts, on a
// machine with no device and no d3dcompiler, before it is ever reassembled.
// ---------------------------------------------------------------------------
static void TestGrassBlades()
{
    // The model token, from the blob's own first dword. A pixel shader's token or
    // a vs_1_1 is a shader this build must not rewrite.
    {
        const auto blob = [](uint32_t token) {
            uint8_t bytes[64]{};
            std::memcpy(bytes, &token, sizeof(token));
            return std::vector<uint8_t>(bytes, bytes + sizeof(bytes));
        };
        const std::vector<uint8_t> vs20 = blob(0xFFFE0200u), vs30 = blob(0xFFFE0300u);
        const std::vector<uint8_t> ps30 = blob(0xFFFF0300u), vs11 = blob(0xFFFE0101u);
        uint8_t shortBlob[8]{};
        Check(std::strcmp(GrassBladeShaderModel(vs20.data(), vs20.size()), "vs_2_0") == 0,
            "a 0xFFFE0200 blob is a vs_2_0 shader");
        Check(std::strcmp(GrassBladeShaderModel(vs30.data(), vs30.size()), "vs_3_0") == 0,
            "a 0xFFFE0300 blob is a vs_3_0 shader");
        Check(GrassBladeShaderModel(ps30.data(), ps30.size())[0] == 0 &&
              GrassBladeShaderModel(vs11.data(), vs11.size())[0] == 0 &&
              GrassBladeShaderModel(shortBlob, sizeof(shortBlob))[0] == 0 &&
              GrassBladeShaderModel(nullptr, 0)[0] == 0,
            "and a pixel shader, an older vertex shader model or a truncated blob is not rewritten at all");
    }

    // A disassembly in the shape a client's detail-doodad shader has: the view
    // transform of the vertex, a texture coordinate, a clip-space output, and
    // nothing but r0/r1 for temporaries.
    const std::string grass =
        "// Generated by Microsoft (R) HLSL Shader Compiler\n"
        "    vs_2_0\n"
        "    def c13, 1, 1, 1, 1\n"
        "    dcl_position v0\n"
        "    dcl_normal v1\n"
        "    dcl_color0 v2\n"
        "    dcl_texcoord0 v3\n"
        "    dp3 r0.x, c0, v0\n"
        "    dp3 r0.y, c1, v0\n"
        "    dp3 r0.z, c2, v0\n"
        "    add r0, r0, c3\n"
        "    mov oPos, r0\n"
        "    mov oT0, v3\n";
    const GrassBladeInjection injected = BuildGrassBladeInjection(grass, "vs_2_0");
    Check(injected.ok, "a detail-doodad shader takes the motion block");
    Check(injected.texcoordRegister == 3, "the block reads the register the shader itself declares its texcoord in");
    Check(injected.text.find("injected grass wind") != std::string::npos &&
          injected.text.find("v3.y") != std::string::npos,
        "and the text that comes back carries the block, reading that register");
    // After the view transform and before the projection: the block has to see a
    // view-space position, and it must land before anything reads r0 again.
    const auto anchorAt = injected.text.find("add r0, r0, c3");
    const auto blockAt = injected.text.find("injected grass wind");
    const auto projectedAt = injected.text.find("mov oPos, r0");
    Check(anchorAt != std::string::npos && blockAt != std::string::npos && projectedAt != std::string::npos &&
          anchorAt < blockAt && blockAt < projectedAt,
        "the block lands between the view transform it needs and the projection that consumes it");
    Check(anchorAt < injected.text.find("mov oT0, v3"),
        "and the shader's own instructions after the anchor are all still there");

    // A shader that declares its coordinate somewhere else is bent the same way:
    // the block is written against the register the shader itself reads.
    {
        std::string other = grass;
        other.replace(other.find("dcl_texcoord0 v3"), 16, "dcl_texcoord0 v2");
        const GrassBladeInjection moved = BuildGrassBladeInjection(other, "vs_3_0");
        Check(moved.ok && moved.texcoordRegister == 2 && moved.text.find("v2.y") != std::string::npos,
            "a blade whose texture coordinate is declared in another register is bent by that register");
        Check(GrassBladeTexcoordRegister(other) == 2, "and the register is read out of the declaration");
    }

    // The model token is prepended when the disassembler did not write one: an
    // assembler needs a model to assemble against.
    {
        std::string bare = grass;
        bare.replace(bare.find("    vs_2_0\n"), 11, "");
        const GrassBladeInjection model = BuildGrassBladeInjection(bare, "vs_2_0");
        Check(model.ok && model.text.compare(0, 7, "vs_2_0\n") == 0,
            "a text without a model token gets one, at the top, where an assembler wants it");
    }

    // Comments and spacing are the disassembler's business, not the parser's: a
    // shader whose anchor is written with a different mask, or with a note after
    // it, is the same shader.
    {
        std::string loose = grass;
        loose.replace(loose.find("    add r0, r0, c3\n"), 20, "    add r0.xyz,r0.xyz,c3.xyz ; view\n");
        const GrassBladeInjection spaced = BuildGrassBladeInjection(loose, "vs_2_0");
        Check(spaced.ok && spaced.anchorLine == 10,
            "an anchor written with a mask, no spaces and a trailing comment is found where it is");
    }

    // Every refusal, in the words the log would carry, and all of them leave the
    // client's shader alone.
    {
        std::string noTexcoord = grass;
        noTexcoord.replace(noTexcoord.find("    dcl_texcoord0 v3\n"), 21, "");
        const GrassBladeInjection none = BuildGrassBladeInjection(noTexcoord, "vs_2_0");
        Check(!none.ok && none.text.empty() && std::strlen(none.reason) > 0,
            "a shader with no texture coordinate cannot be bent (a blade has no height to bend by)");

        // A scratch register the shader still NEEDS across the insertion point is
        // refused: r2 is written before the block would run and read after it.
        std::string live = grass;
        live.replace(live.find("    add r0, r0, c3\n"), 20, "    mul r2, v3, c13\n    add r0, r0, c3\n");
        live.replace(live.find("    mov oT0, v3\n"), 16, "    mov oT0, r2\n");
        const GrassBladeInjection used = BuildGrassBladeInjection(live, "vs_2_0");
        Check(!used.ok && std::string(used.reason).find("scratch") != std::string::npos,
            "a shader whose own value still lives in one of the block's scratch registers is refused, never clobbered");

        // And the same register redefined after the insertion point is fine: the
        // shader's own instruction lands after the block's, so nothing of its is
        // destroyed. This is the shape a real client's blade shader has (three of
        // the five foliage shaders of one session keep their normals and their
        // lighting in r2/r3), and refusing it is what "nothing sways" was.
        std::string reused = grass;
        reused.replace(reused.find("    mov oT0, v3\n"), 16, "    nrm r2.xyz, v3\n    mov oT0, r2\n");
        const GrassBladeInjection redeclared = BuildGrassBladeInjection(reused, "vs_2_0");
        Check(redeclared.ok && redeclared.text.find("    nrm r2.xyz, v3\n") != std::string::npos,
            "a shader that redefines a scratch register after the block is bent, with its own instruction intact");

        std::string noAnchor = grass;
        noAnchor.replace(noAnchor.find("    dp3 r0.x, c0, v0\n"), 20, "");
        noAnchor.replace(noAnchor.find("    dp3 r0.y, c1, v0\n"), 20, "");
        noAnchor.replace(noAnchor.find("    dp3 r0.z, c2, v0\n"), 20, "");
        noAnchor.replace(noAnchor.find("    add r0, r0, c3\n"), 20, "");
        Check(!BuildGrassBladeInjection(noAnchor, "vs_2_0").ok,
            "a shader that never lands its vertex in r0 has nowhere for the block to go");

        std::string noPosition = grass;
        noPosition.replace(noPosition.find("    mov oPos, r0\n"), 17, "");
        Check(!BuildGrassBladeInjection(noPosition, "vs_2_0").ok,
            "and a body that writes no clip-space position is not a vertex shader's body at all");

        Check(!BuildGrassBladeInjection("", "vs_2_0").ok && !BuildGrassBladeInjection(grass, "").ok,
            "an empty disassembly and an unknown model token are both refusals");
        Check(GrassBladeAnchorLine(grass) != ~size_t(0) && GrassBladeAnchorLine(noAnchor) == ~size_t(0),
            "the anchor is found by what the instruction does, and reported as absent when it is");
    }

    // The other shape a client draws its ground clutter with, and the one that made
    // a real field read "0 looked at, 0 bent": the vertex is placed by the
    // instance's own rows (c31..c33) and clipped by the projection, so there is no
    // `add r0, r0, c3` anywhere and the clip-space output is spelled `o0`, not
    // `oPos`. Both were refusals once; both have to be bent now, with the block
    // built for a position that is already in the world.
    {
        const std::string field =
            "// Generated by Microsoft (R) HLSL Shader Compiler\n"
            "    vs_3_0\n"
            "    def c0, 1, 0, 0, 0\n"
            "    dcl_position v0\n"
            "    dcl_color v1\n"
            "    dcl_texcoord v2\n"
            "    dcl_position o0\n"
            "    dp4 r0.x, c31, v0\n"
            "    dp4 r0.y, c32, v0\n"
            "    mov r0.w, c0.x\n"
            "    dp4 r0.z, c33, v0\n"
            "    dp4 o0.x, c2, r0\n"
            "    dp4 o0.y, c3, r0\n"
            "    dp4 o0.z, c4, r0\n"
            "    dp4 o0.w, c5, r0\n"
            "    mad o2.x, v2.x, c0.x, c0.y\n";
        const GrassBladeInjection bent = BuildGrassBladeInjection(field, "vs_3_0");
        Check(bent.ok, "a shader that places its vertex with the instance's rows and writes o0 is bent");
        Check(bent.worldSpace, "and the block is built for a position that is already the world's");
        // The last write to r0 before the projection is the instance row's own
        // dp4; the block belongs there and nowhere else.
        Check(bent.anchorLine == 10 && bent.text.find("    dp4 r0.z, c33, v0\n") != std::string::npos,
            "the block lands after the last line that gives r0 its position");
        const size_t anchorAt = bent.text.find("    dp4 r0.z, c33, v0\n");
        const size_t blockAt = bent.text.find("injected grass wind");
        const size_t projectedAt = bent.text.find("    dp4 o0.x, c2, r0\n");
        Check(anchorAt != std::string::npos && blockAt != std::string::npos && projectedAt != std::string::npos &&
              anchorAt < blockAt && blockAt < projectedAt,
            "with the shader's own clip-space output untouched and still reading the position the block bent");
        Check(bent.text.find("dcl_position o0") != std::string::npos &&
              bent.text.find("    mov r2.x, r0.x\n") != std::string::npos &&
              bent.text.find("    dp3 r2.x, c0.xyz, r0.xyz\n") == std::string::npos,
            "and the block reads the world position directly rather than borrowing the shader's own rows");

        // The same shader with r2...r5 written after the anchor before any read -
        // the field's own 956 and 1048 byte shaders, which keep their normals and
        // their light vectors there - is bent too.
        std::string loaded = field;
        loaded.replace(loaded.find("    mad o2.x, v2.x, c0.x, c0.y\n"), 29,
            "    nrm r2.xyz, v1\n    mul r3.xyz, r2, c6\n    add r4.xyz, r3, c7\n    add r5.xyz, r4, c8\n"
            "    mad o2.x, v2.x, r2.x, r5.y\n");
        Check(BuildGrassBladeInjection(loaded, "vs_3_0").ok,
            "a blade shader that keeps its normals and its lighting in r2..r5 after the block is bent as well");
    }

    // Where the block's own rows live. c14..c19 is the reference module's free
    // block, and a real session's field disproved that it is free for every
    // client: that session's 956 and 1048 byte foliage shaders read c17 and c18
    // themselves, so a block written over them would have had the shader read the
    // wind as its own lighting. The base is chosen per shader, from the registers
    // THAT shader never mentions.
    {
        const std::string lit =
            "// Generated by Microsoft (R) HLSL Shader Compiler\n"
            "    vs_3_0\n"
            "    def c0, 1, 0, 0, 0\n"
            "    dcl_position v0\n"
            "    dcl_color v1\n"
            "    dcl_texcoord v2\n"
            "    dcl_position o0\n"
            "    dp4 r0.x, c31, v0\n"
            "    dp4 r0.y, c32, v0\n"
            "    mov r0.w, c0.x\n"
            "    dp4 r0.z, c33, v0\n"
            "    dp3 r1.x, c17.xyz, v1\n"
            "    dp3 r1.y, c18.xyz, v1\n"
            "    dp4 o0.x, c2, r0\n"
            "    dp4 o0.y, c3, r0\n"
            "    dp4 o0.z, c4, r0\n"
            "    dp4 o0.w, c5, r0\n"
            "    mad o2.x, v2.x, r1.x, r1.y\n";
        const GrassBladeInjection moved = BuildGrassBladeInjection(lit, "vs_3_0");
        Check(moved.ok, "a foliage shader that reads the lighting rows c17 and c18 itself is bent");
        Check(moved.firstRegister == 19 && moved.text.find("    dp3 r1.x, c17.xyz, v1\n") != std::string::npos &&
              moved.text.find("    dp3 r1.y, c18.xyz, v1\n") != std::string::npos,
            "the block moves off c14..c19 to the first run of six registers above it that this shader never reads");
        // And the rows it moved to are the rows the block reads - once each, at
        // the new base, with the shader's own lighting lines untouched.
        const std::string renumbered = GrassBladeMotionBlock("v2", moved.worldSpace, moved.firstRegister);
        for (unsigned row = 0; row < kGrassBladeRows; ++row)
        {
            const std::string tag = "c" + std::to_string(moved.firstRegister + row);
            Check(renumbered.find(tag) != std::string::npos,
                "every one of the six rows is renamed into the run that was chosen");
        }
        Check(GrassBladeFreeConstantRun(lit) == moved.firstRegister,
            "and the run it chose is the first one that is free rather than the reference block's own base");
        // A shader that reads every register the rows could live in is refused
        // rather than written over: there is nowhere left to put them.
        std::string crowded =
            "// Generated by Microsoft (R) HLSL Shader Compiler\n"
            "    vs_3_0\n"
            "    dcl_position v0\n"
            "    dcl_texcoord v2\n"
            "    dcl_position o0\n"
            "    dp4 r0.x, c31, v0\n"
            "    dp4 r0.y, c32, v0\n"
            "    dp4 r0.z, c33, v0\n";
        for (unsigned reg = 14; reg < 256; ++reg)
            crowded += "    dp3 r1.x, c" + std::to_string(reg) + ".xyz, v0\n";
        crowded += "    dp4 o0.x, c2, r0\n    dp4 o0.y, c3, r0\n    dp4 o0.z, c4, r0\n"
            "    dp4 o0.w, c5, r0\n    mad o2.x, v2.x, c0.x, c0.y\n";
        const GrassBladeInjection none = BuildGrassBladeInjection(crowded, "vs_3_0");
        Check(!none.ok && std::string(none.reason).find("constant register") != std::string::npos,
            "and a shader that reads every register the rows could live in is refused, with the reason said");
    }

    // The block is arithmetic over the engine's own modelview rows and the free
    // registers, and the check that matters without a device: it never writes a
    // register outside the ones the caller proved free (r2..r5) and the
    // position's own r0.
    {
        const std::string block = GrassBladeMotionBlock("v3", false);
        const std::string worldBlock = GrassBladeMotionBlock("v3", true);
        Check(worldBlock.find("    mov r2.x, r0.x\n    mov r2.y, r0.y\n") != std::string::npos &&
              worldBlock.find("    add r0.x, r0.x, r3.z\n    add r0.y, r0.y, r3.w\n") != std::string::npos &&
              worldBlock.find("c0.xyz") == std::string::npos &&
              worldBlock.find("    mul r5.w, r0.z, c17.w\n    add r5.w, r5.w, c14.z\n") != std::string::npos &&
              worldBlock.find("mul r3.x, r2.x, c19.z") != std::string::npos,
            "the world-space block reads the position it is given and lands its displacement on the world's own "
            "axes, with the same waves, hash and bend weight as the view-space one - and no row of the shader's own");
        for (const char* forbidden : { "r6", "r7", "r1.", "r1,", " c4", " c5", " c14.x, c14.y, c30" })
            Check(block.find(forbidden) == std::string::npos,
                "the block stays inside r0 and r2..r5 and the registers this module writes itself");
        Check(block.find("add r0.x, r0.x") != std::string::npos && block.find("sincos r4.xy") != std::string::npos &&
              block.find("sincos r5.xy") != std::string::npos,
            "and it adds its displacement to the view position the engine built, from two travelling waves");
        Check(block.find("c19.x, c19.y") != std::string::npos,
            "with the blade's own direction (which way its texture runs) read from a constant");
        Check(block.find("mul_sat r5.z, r5.z, c18.x") != std::string::npos &&
              block.find("mul_sat r5.z, r5.z, r5.w") != std::string::npos &&
              block.find("mov_sat") == std::string::npos,
            "and its two clamps are the one saturate modifier the reference module proved on this hardware");
        // The one rule the vs_3_0 assembler enforces that a hand-written block can
        // break without noticing: an instruction may read at most ONE constant
        // register (D3DAssemble answers X5752, "Max. different constant registers
        // readable per instruction is 1"). The distance fade read c17.w and c14.z
        // in a single `mad`, and that one line refused every blade shader of the
        // first build that reached this point - the field stayed exactly as the
        // client drew it. Both variants are scanned here, instruction by
        // instruction, because the rule is the assembler's, not the reference
        // module's, and nothing else in this suite would catch it.
        for (const std::string* variant : { &block, &worldBlock })
        {
            size_t at = 0;
            bool offender = false;
            std::string line;
            while (at <= variant->size())
            {
                const size_t end = variant->find('\n', at);
                line = variant->substr(at, end == std::string::npos ? std::string::npos : end - at);
                if (line.compare(0, 4, "    ") == 0)
                {
                    int constants = 0, previous = -1;
                    for (size_t i = 0; i < line.size(); ++i)
                    {
                        if (line[i] != 'c' || (i && (std::isalnum(static_cast<unsigned char>(line[i - 1])) || line[i - 1] == '.'))) continue;
                        int value = 0;
                        size_t j = i + 1;
                        bool digits = false;
                        while (j < line.size() && line[j] >= '0' && line[j] <= '9')
                        {
                            value = value * 10 + (line[j] - '0');
                            ++j;
                            digits = true;
                        }
                        if (!digits || (j < line.size() && line[j] != '.')) continue;
                        if (value != previous) ++constants;
                        previous = value;
                    }
                    if (constants > 1) offender = true;
                }
                if (end == std::string::npos) break;
                at = end + 1;
            }
            Check(!offender, "no instruction of the injected block reads two different constant registers: the vs_3_0 "
                "assembler refuses that outright (X5752), and one such line is a whole still field");
        }
        Check(block.find("frc r3.x, r3.x") != std::string::npos && block.find("frc r3.y") == std::string::npos,
            "the per-blade random is taken with the write mask the fractional instruction requires");
        Check(block.find("mul r5.w, r0.z, c17.w") != std::string::npos &&
              block.find("add r5.w, r5.w, c14.z") != std::string::npos &&
              block.find("mul r3.x, r2.x, c19.z") != std::string::npos &&
              block.find("mad r3.x, r2.y, c19.w, r3.x") != std::string::npos,
            "and its own random takes both world axes, so a field does not stripe");
        // The one piece of assembly syntax the compiler versions do not have to
        // agree on, and the reason the block reads scalars from its own constants
        // instead: no immediate float operand anywhere in the injected text.
        bool immediate = false;
        for (size_t i = 0; i + 2 < block.size(); ++i)
            if (std::isdigit(static_cast<unsigned char>(block[i])) && block[i + 1] == '.' &&
                std::isdigit(static_cast<unsigned char>(block[i + 2])))
                immediate = true;
        Check(!immediate, "the injected block carries no immediate float operand: every scalar it reads is in c14..c19");
    }

    // The constants: one block, six rows, every component accounted for.
    {
        GrassBladeFrame frame;
        frame.directionX = 0.f;
        frame.directionY = 1.f; // wind travelling towards +Y (west, in WoW's axes)
        frame.strength = 1.f;
        frame.cameraX = 10.f;
        frame.cameraY = -4.f;
        frame.distanceYards = 100.f;
        frame.phasePrimary = .5f;
        frame.phaseCross = 1.5f;
        frame.settings.tipAmplitudeYards = .2f;
        frame.settings.wavelengthYards = 20.f;
        frame.settings.crossWavelengthYards = 5.f;
        frame.settings.crossAmplitudeRatio = .5f;
        frame.settings.crossAngleDegrees = 90.f;
        frame.settings.anchor = .25f;
        frame.settings.variance = .6f;
        frame.settings.leanFraction = .4f;
        const GrassBladeConstants packed = PackGrassBladeConstants(frame, 0);

        Check(Near(packed.rows[0][0], 10.f) && Near(packed.rows[0][1], -4.f) && Near(packed.rows[0][2], 1.f),
            "the block carries the camera the wave field is anchored to, and a 1 for the blade's ramp and fade");
        Check(Near(packed.rows[0][3], .4f), "the lean");
        const float k = float(2.0 * 3.14159265358979323846 / 20.0);
        Check(Near(packed.rows[1][0], 0.f) && Near(packed.rows[1][1], k) && Near(packed.rows[1][2], 0.f) &&
              Near(packed.rows[1][3], .2f),
            "the primary wave's heading is carried twice: as the phase basis (dir * k) and as the displacement (dir * amplitude)");
        Check(Near(packed.rows[2][2], -.1f),
            "and the cross-swell's amplitude is its own share of the primary's, along its own heading");
        Check(Near(packed.rows[2][1], 0.f) && std::fabs(packed.rows[2][0]) > .1f,
            "turned off the wind by the cross angle, as the setting says");
        Check(Near(packed.rows[3][0], .5f) && Near(packed.rows[3][1], 1.5f) && Near(packed.rows[3][3], -.01f),
            "the two phases travel with the frame, and the range fade is written as a fall-off per yard");
        Check(Near(packed.rows[4][0], 1.f / .75f) && Near(packed.rows[4][1], -.25f) &&
              Near(packed.rows[4][2], .7f) && Near(packed.rows[4][3], .6f),
            "the ramp holds the bottom share of the blade rigid, and every blade gets its own amplitude within the variance");
        Check(Near(packed.rows[5][0], -1.f) && Near(packed.rows[5][1], 1.f),
            "and the default blade runs the reference client's way up: 1 at the base, 0 at the tip");
        Check(Near(packed.rows[5][2], .618034f) && Near(packed.rows[5][3], .381966f),
            "and its own random is hashed out of both world axes with the block's own basis");
        const GrassBladeConstants flipped = PackGrassBladeConstants(frame, 1);
        Check(Near(flipped.rows[5][0], 1.f) && Near(flipped.rows[5][1], 0.f),
            "a blade whose own vertices measured the other way up is bent by the same block with that sign flipped");
        // A calm frame moves nothing, like every other part of this feature.
        frame.strength = 0.f;
        const GrassBladeConstants calm = PackGrassBladeConstants(frame, 0);
        Check(Near(calm.rows[1][2], 0.f) && Near(calm.rows[1][3], 0.f) && Near(calm.rows[2][2], 0.f),
            "and a frame with no wind carries no displacement at all");
    }

    // The phases walk forward and are wrapped, so a shader's sine never sees an
    // hour's worth of radians.
    {
        float phase = 0.f;
        phase = GrassBladePhase(phase, 3.f, 18.f, .5f);
        const float k = float(2.0 * 3.14159265358979323846 / 18.0);
        Check(Near(phase, 6.2831853f - k * 3.f * .5f, 1e-3),
            "the crests travel downwind: the phase falls by the wave speed times the frame, wrapped");
        for (int i = 0; i < 5000; ++i) phase = GrassBladePhase(phase, 3.f, 18.f, .05f);
        Check(phase >= 0.f && phase < 6.283186f, "and it is wrapped every frame, however long the session runs");
        Check(Near(GrassBladePhase(2.f, 0.f, 18.f, .5f), 2.f), "a still frame does not advance it");
        Check(Near(GrassBladePhase(.5f, 3.f, 18.f, 0.f), .5f), "and neither does a frame of no length");
    }

    // Which way up a blade's texture runs, from the vertices themselves: the
    // sign of v against height, by majority over the card's own pairs.
    {
        const float falling[6][3] = { { 0.f, 1.f, 0.f }, { 0.f, 0.f, .5f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, .4f },
                                      { 0.f, 1.f, .1f }, { 0.f, 0.f, .6f } };
        const float rising[6][3] = { { 0.f, 0.f, 0.f }, { 0.f, 1.f, .5f }, { 0.f, 0.f, 0.f }, { 0.f, 1.f, .4f },
                                     { 0.f, 0.f, .1f }, { 0.f, 1.f, .6f } };
        Check(GrassBladeVerticalSign(falling, 6) == -1,
            "a blade whose texture is 1 at its base measures as the reference client has it");
        Check(GrassBladeVerticalSign(rising, 6) == 1, "and one that runs the other way measures as the other way");
        const float noise[4][3] = { { 0.f, .5f, 0.f }, { 0.f, .52f, 1.f }, { 0.f, .5f, 0.f }, { 0.f, .48f, 1.f } };
        Check(GrassBladeVerticalSign(noise, 4) == 0,
            "vertices that carry no v that follows their height measure as nothing, and nothing means the default");
        Check(GrassBladeVerticalSign(nullptr, 4) == 0 && GrassBladeVerticalSign(falling, 1) == 0,
            "and so does a draw with too few vertices to vote");
        // The same vertices, measured for what they say about the CARD: how tall
        // it is and how many pairs of it looked like a base and its tip. This is
        // what refuses a decal or a shadow blob on the ground, where a bend
        // weight running along the blade would translate the whole card.
        const GrassBladeCard tall = MeasureGrassBladeCard(falling, 6);
        Check(tall.sign == -1 && Near(tall.extentYards, .6f) && tall.pairs >= 2 && tall.measured,
            "a blade card carries its own height and the pairs that voted, next to the sign");
        Check(!MeasureGrassBladeCard(nullptr, 0).measured,
            "and a card that could not be read at all says so, so a failed read is never refused as a flat card");
        const float flat[6][3] = { { 0.f, 0.f, 0.f }, { 0.f, 1.f, .001f }, { 0.f, 0.f, 0.f }, { 0.f, 1.f, .002f },
                                   { 0.f, 0.f, .001f }, { 0.f, 1.f, .003f } };
        const GrassBladeCard decal = MeasureGrassBladeCard(flat, 6);
        Check(decal.pairs == 0 && decal.extentYards < kGrassBladeMinExtentYards,
            "a card lying on the ground has no height to bend along and no pair that looks like a base and a tip");
        const float uniform[4][3] = { { 0.f, .5f, 0.f }, { 0.f, .5f, .5f }, { 0.f, .5f, 0.f }, { 0.f, .5f, .5f } };
        const GrassBladeCard noV = MeasureGrassBladeCard(uniform, 4);
        Check(noV.pairs == 0 && noV.extentYards >= .4f,
            "a tall card whose vertices carry no v along their height has nothing for the bend weight to follow either");
        Check(MeasureGrassBladeCard(nullptr, 4).pairs == 0 && MeasureGrassBladeCard(nullptr, 4).extentYards == 0.f,
            "and a draw with no vertices at all is neither");
    }

    // The HUD rows the blade path adds, in the wind debug box.
    {
        GrassBladeSettings blades;
        GrassBladeStatus status;
        status.bladeDraws = 37;
        status.uploads = 2;
        status.amplitudeYards = .15f;
        status.examined = 4;
        status.injected = 1;
        const std::vector<std::string> lines = GrassBladeDebugLines(status, blades);
        Check(lines.size() == 2 && lines[0].find("37") != std::string::npos &&
              lines[0].find("0.15") != std::string::npos,
            "the blade row says how many draws went out with the wind and how far a tip moves");
        Check(lines[1].find("4 looked at, 1 bent") != std::string::npos &&
              lines[1].find("none refused") != std::string::npos,
            "and the gate row says how many of the client's shaders this build has looked at and bent");
        status.neverModel = 3;
        status.cardRefused = 2;
        status.cardUnreadable = 1;
        status.worldShaped = 5;
        const std::vector<std::string> gates = GrassBladeDebugLines(status, blades);
        Check(gates.size() > 1 && gates[1].find("never 3") != std::string::npos &&
              gates[1].find("card 2") != std::string::npos &&
              gates[1].find("read 1") != std::string::npos,
            "the row names the interface when it is refused, a card that carries no blade, and one that could not be "
            "read at all");
        // The world verdict is a shape, not a refusal, on this path, and the row
        // has to say so: it is what a real session's `world 376` among the
        // refusals was hiding, and reading it as a refusal is what left a field
        // still.
        Check(gates[1].find("world") == std::string::npos &&
              gates.back().find("Blades world-path: 5 bent") != std::string::npos,
            "the world's projection is recorded as the shape an unskinned card is drawn with, not as a refusal");
        status.neverModel = 0;
        status.worldShaped = 0;
        status.cardRefused = 0;
        status.cardUnreadable = 0;
        status.materialRefused = 6;
        status.noTexcoord = 2;
        status.refused = 1;
        const std::vector<std::string> refused = GrassBladeDebugLines(status, blades);
        Check(!refused.empty() && refused.back().find("mat 6") != std::string::npos &&
              refused.back().find("decl 2") != std::string::npos &&
              refused.back().find("asm 1") != std::string::npos,
            "a refusal is named on the row, so a field of static grass is answerable on screen");
        blades.enabled = false;
        Check(GrassBladeDebugLines(status, blades).empty(), "and a switched-off blade wind adds no rows at all");
    }

    // The wiring: the blade path is reached from the one place an unskinned
    // foliage draw can be, the swap happens before state tracking, the block is
    // re-armed by the constant-write hook, and the whole path is behind the same
    // switches as the sway it belongs to.
    {
        const std::string sway = Read("src/Effects/VegetationSway.h");
        const std::string blades = Read("src/Effects/GrassBladeSway.h");
        const std::string main = Read("ModernWoWRenderer.cpp");
        const std::string tuning = Read("src/Environment/WindTuning.h");
        Check(sway.find("grassblade::ConsiderDraw(device, frame, shader.hash, shader.object, *declared, draw,") !=
              sway.npos &&
              sway.find("if (!declared || !declared->valid)") != sway.npos,
            "an unskinned foliage draw is handed to the blade path from the branch that refuses the model path");
        Check(sway.find("detail::TextureIsCutout(device); });") != sway.npos,
            "and the texture probe it needs is asked for lazily, because most draws on that branch are not grass");
        // The interface is an unskinned cut-out card with a texture coordinate -
        // the exact shape this path looks for - so the shader verdict the model
        // path uses has to run here too, before anything is offered to the
        // injection, or a whole interface would wave.
        const auto bladeCall = sway.find("grassblade::ConsiderDraw(device, frame, shader.hash, shader.object,");
        Check(sway.find("const detail::Admission blades = detail::ShaderAccepted(device, frame, shader.hash);") !=
              sway.npos &&
              sway.find("const detail::Admission blades = detail::ShaderAccepted(device, frame, shader.hash);") <
              bladeCall &&
              sway.find("grassblade::RefuseShader();") != sway.npos &&
              bladeCall != sway.npos,
            "the interface, the precipitation and the liquid variants are refused before the blade path sees a draw");
        // ...and the world is NOT, which is the one thing that differs from the
        // model path's use of the same verdict.
        Check(sway.find("grassblade::RefuseShader(blades == detail::Admission::Never);") == sway.npos &&
              sway.find("blades == detail::Admission::World,") != sway.npos &&
              sway.find("if (blades == detail::Admission::Never)") != sway.npos,
            "a world-shaped binding is recorded and bent on the blade path, because an unskinned card has no palette "
            "to settle it with");
        Check(blades.find("grassblade::ConsiderDraw") == blades.npos &&
              blades.find("entry->structureRefused = true;") < blades.find("MeasureBladeCard(device, layout, draw)"),
            "the blade half keeps its refusals on the shader entry, and measures the card before it bends it");
        // The three measurements that a real field was lost to: a card whose
        // texture coordinate ends exactly on the stride's last byte fits (a `<=`
        // refused every one of them), a card that could not be read is not a flat
        // card, and a measured card that IS flat is still refused.
        Check(blades.find("if (stride < layout.texcoordOffset + 8u || stride < layout.positionOffset + 12u)") !=
              blades.npos &&
              blades.find("const bool bladeShaped = card.measured && card.pairs >= renderer::kGrassBladeMinPairs &&") !=
              blades.npos &&
              blades.find("card.extentYards >= renderer::kGrassBladeMinExtentYards &&") != blades.npos &&
              blades.find("card.extentYards <= renderer::kGrassBladeMaxExtentYards") != blades.npos &&
              blades.find("if (!bladeShaped)") != blades.npos &&
              blades.find("card.measured = true;") == blades.npos,
            "a draw's own vertices are the whole admission - a readable card of a blade's size with a base and a tip "
            "in it, and the fit test is a strict inequality");
        Check(main.find("grassblade::Substitute(&vs);") != main.npos &&
              main.find("grassblade::Substitute(&vs);") < main.find("renderer::g_trackedState.currentVS != vs"),
            "the bent shader is swapped in at the SetVertexShader hook, before this renderer tracks what is bound");
        Check(main.find("grassblade::NoteConstantWrite(start, count);") != main.npos &&
              main.find("originalSetVertexShaderConstantF=reinterpret_cast<SetVertexShaderConstantFFn>(table[94]);") !=
              main.npos,
            "and the constant-write hook (device vtable 94) is what re-arms the block when the client writes over it");
        Check(main.find("grassblade::logLine=Log;") != main.npos && main.find("grassblade::active=grassblade::active=graphicsActive;") == main.npos &&
              main.find("vegetation::active=grassblade::active=graphicsActive;") != main.npos &&
              main.find("grassblade::Update(nextFrame,vegetation::settings.sway);") != main.npos,
            "it shares the sway's log, its F11 switch, its wind snapshot and its SwayPercent");
        Check(tuning.find("read(L\"GrassBlades\", 1) != 0") != tuning.npos &&
              Read("src/Environment/OverlayItems.h").find("L\"GrassBlades\",\"GRASS BLADE WIND\"") != sway.npos,
            "and it has its own switch in the INI and in the F7 menu, because it rewrites a shader the client compiled");
        Check(blades.find("D3DDisassemble") != blades.npos && blades.find("D3DAssemble") != blades.npos &&
              blades.find("d3dcompiler_47.dll") != blades.npos,
            "the compiler it needs is resolved by name, and a machine without it loses only the blade wind");
        Check(blades.find("entry.structureRefused = true;") != blades.npos &&
              blades.find("g_status.refused;") != blades.npos,
            "and every refusal is recorded on the shader that failed, so nothing is examined twice");
        Check(blades.find("BladeShader* entry = FindBladeShader(shader);") != blades.npos &&
              blades.find("for (auto& entry : g_blades)") != blades.npos &&
              blades.find("if (entry.second.injected.Get() == shader) return &entry.second;") != blades.npos,
            "a bent shader that is what the device is drawing with maps back to its own entry - the swap means every "
            "later read of \"what is bound\" returns the bent object, and it must not be examined twice");
        Check(blades.find("if (shader == entry->original.Get()) device->SetVertexShader(entry->injected.Get());") !=
              blades.npos,
            "and a client that binds its shaders through state blocks - which never reach the Set hook - still draws "
            "the bent one, because the draw that proved it is a blade card binds it");
    }
    // Where the wave field's world position comes from. A real session's shaders
    // land the vertex with `add r0, r0, c3`, so c3 is the one term that has to
    // come back OUT of r0 before the position is a world position again - and
    // taking it from the shader's own constant, rather than from the frame's
    // camera, is what keeps a walking player's grass from shaking: the camera this
    // renderer captured is not the matrix the shader was drawn with, and the
    // difference grows with the player's own movement.
    Check(injected.viewTranslation == "c3" &&
          injected.text.find("    sub r5.x, r0.x, c3.x\n    sub r5.y, r0.y, c3.y\n    sub r5.z, r0.z, c3.z\n"
              "    dp3 r2.x, c0.xyz, r5.xyz\n    dp3 r2.y, c1.xyz, r5.xyz\n") != std::string::npos,
        "the block subtracts the translation row of the shader's OWN view transform and rotates the rest back into "
        "the world with the shader's own rows");
    Check(injected.text.find("add r2.x, r2.x, c14.x") == std::string::npos &&
          injected.text.find("add r2.y, r2.y, c14.y") == std::string::npos,
        "so no term of that block comes from the frame's camera, which is what made the grass shake as the camera "
        "moved");
    Check(GrassBladeMotionBlock("v3", false).find("    dp3 r2.x, c0.xyz, r0.xyz\n    add r2.x, r2.x, c14.x\n") !=
              std::string::npos &&
          GrassBladeMotionBlock("v3", false, kGrassBladeFirstRegister, "c7").find("    sub r5.x, r0.x, c7.x\n") !=
              std::string::npos &&
          GrassBladeMotionBlock("v3", false, kGrassBladeFirstRegister, "c7").find("c14.x") == std::string::npos,
        "a shader whose anchor names no translation row keeps the camera fallback, and the translated form has no "
        "camera in it at all");
    // The model path - trees, bushes and the plants the client draws as models -
    // is switched off in the shipped INI, and the HUD says so in words rather than
    // as a "0 leaning" row that reads like a broken feature.
    {
        const std::string sway = Read("src/Effects/VegetationSway.h");
        const std::string wind = Read("src/Environment/WindDebug.h");
        Check(sway.find("if (!settings.swayModels)") != sway.npos && sway.find("++Status().modelsOff;") != sway.npos,
            "the model path ends at its own switch, before the first counter");
        Check(wind.find("Trees & bushes: off (F7)") != wind.npos &&
              Read("GraphicsEffects.ini").find("\nSwayModels=0\n") != wind.npos,
            "and a player who turned it off reads that the trees are off rather than that nothing is leaning");
    }
    // Every part of the block a device would upload lives where the report says it
    // does: the base is the shader's own, and the rows are one per register.
    Check(Read("src/Effects/GrassBladeSway.h").find(
              "device->SetVertexShaderConstantF(entry.firstRegister, entry.rows[0], renderer::kGrassBladeRows);") !=
              std::string::npos &&
          Read("src/Effects/GrassBladeSway.h").find(
              "if (start < entry.firstRegister + renderer::kGrassBladeRows && end > entry.firstRegister)") !=
              std::string::npos,
        "the rows go to the base the shader was given, and the re-arm watches that same range, not c14..c19");
    Pass("grass blade wind: a shader text, the block injected into it, the constants it reads, and the wiring");
}

int main() try
{
    TestLeanGeometry();
    TestLeanInWorldSpace();
    TestBoneAnalysis();
    TestStaticPoseDetection();
    TestFieldAndSampling();
    TestStrengthSizesAndGates();
    TestWindDebugLine();
    TestGeometryMeasurement();
    TestShaderAdmission();
    TestTextureCutoutDecode();
    TestPerBoneLean();
    TestMergedPatchSkeleton();
    TestGrassBlades();
    TestConfigurationAndWiring();
    std::puts("Vegetation wind regressions passed");
    return 0;
}
catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
