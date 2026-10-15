// Portable checks for wind-driven vegetation sway. Run from the repository
// root, no SDK needed on Linux:
//   mkdir -p build && g++ -O2 -std=c++20 -Wall -Wextra -Werror -pedantic '
//     tools/VegetationWindRegression.cpp -o build/vegetation-test && ./build/vegetation-test
// The same file is built by tools/Test-VegetationWind.cmd on the Windows
// runner, so the settings parity checked here is also checked where the real
// location-scoped INI read/save/reload path lives.
#include "../src/Core/TextureCutout.h"
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
    status.skippedFixedFunction = 2; status.skippedNeverModel = 44;
    rows = VegetationDebugLines(status, settings, wind);
    Check(rows.size() > 2, "a frame that refuses at every gate continues on the next row");
    Check(rows[2].find(kMoreGatesPrefix) == 0, "and the continuation says that it is one");
    for (const char* label : { "flat", "decl", "world", "vs", "geo", "depth", "never", "cam", "pal", "shape",
                               "size", "move", "far", "calm", "write", "new" })
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
        {L"SwayAnimatedModels",s.animatedModels},{L"DebugView",s.debugView}};
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
    Check(sway.find("ReadPalette(device, rootKey, frame.frameIndex, bound") != sway.npos,
        "the palette is read bounded, never as the whole constant file");
    // Inert when off, and every refusal counted. Ordering is the whole guarantee
    // of the first, so it is pinned here rather than left to a comment: the
    // disabled, unlinked, up-front (particle/UI), unverified-shader and
    // non-model-declaration paths all return above the first palette read.
    const auto scopeStart = sway.find("Scope(IDirect3DDevice9* device, bool skip");
    const auto firstPaletteRead = sway.find("GetVertexShaderConstantF(renderer::kM2BoneBaseRegister", scopeStart);
    const auto linkGate = sway.find("if (!frame.wind.DrivesVegetation()) return;", scopeStart);
    const auto upGate = sway.find("if (draw.verticesUP) return;", scopeStart);
    const auto shaderGate = sway.find("detail::ShaderAccepted(device, frame, shader.hash)", scopeStart);
    const auto layoutGate = sway.find("detail::Layout(device, detail::BoundDeclaration(device)", scopeStart);
    const auto layoutReport = sway.find("frame.frameIndex, true);", layoutGate);
    Check(scopeStart != sway.npos && firstPaletteRead != sway.npos &&
          linkGate != sway.npos && linkGate < firstPaletteRead &&
          upGate != sway.npos && upGate < firstPaletteRead &&
          shaderGate != sway.npos && shaderGate < firstPaletteRead &&
          layoutGate != sway.npos && layoutGate < firstPaletteRead,
        "the disabled, unlinked and non-model paths return before the first palette read");
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
          sway.find("if (bones.bones > renderer::kVegetationMaxBones && !merged)") != sway.npos,
        "a batch of many plants is not a creature: the bone cap is applied to compact skeletons only");
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
    Check(sway.find("const bool manyPlants = renderer::VegetationCarriesManyPlants(bones);") != sway.npos &&
          wind.find("return bones.bones > 1 && bones.spread > kVegetationMergedSpread &&") != sway.npos &&
          sway.find("instance->plants = manyPlants ? bones.bones : 1;") != sway.npos &&
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
    for (const char* label : { "flat", "decl", "world", "vs", "geo", "depth", "never", "cam", "pal", "shape",
                               "size", "move", "far", "calm", "write", "new" })
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
    Check(layoutReport != sway.npos,
        "a declaration that is not the model path is named in the log, mirror or not");
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
    const auto materialGate = sway.find("const bool foliageMaterial = detail::TransparentMaterial(device);");
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
    Check(sway.find("const bool foliageMaterial = detail::TransparentMaterial(device);") <
          sway.find("detail::TextureIsCutout(device)") &&
          sway.find("if (foliageMaterial)") != sway.npos,
        "the state is asked first: a cutout material is the common case and costs no lock");
    Check(sway.find("bool cutout = false;") != sway.npos && sway.find("instance->cutout = true;") != sway.npos &&
          sway.find("instance.cutout = false;") != sway.npos,
        "the proof belongs to the instance, so a tree's other batches need no texture read - and a model that "
        "stops leaning loses it rather than lending it to whatever is placed there next");
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
    const auto textureGate = sway.find("detail::TextureIsCutout(device)");
    const auto neverVerdict = sway.find("++Status().skippedNeverModel;");
    Check(colourGate != sway.npos && colourGate < textureGate,
        "the colour-write test runs before any texture is locked: the shadow map is never examined");
    Check(rootGate != sway.npos && rootGate < textureGate && rootGate > neverVerdict,
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
          renderer::VegetationCarriesManyPlants(skeleton(60, 40.f, 1.f)),
        "a field of grass merged into one draw is low and wide, so it is a patch of many plants");
    Check(renderer::VegetationSkeletonIsSpatial(skeleton(40, 3.f, 12.f)) &&
          !renderer::VegetationCarriesManyPlants(skeleton(40, 3.f, 12.f)),
        "a tree is tall: admitted with all its branches, and still one plant");
    Check(!renderer::VegetationCarriesManyPlants(skeleton(1, 40.f, 1.f)),
        "and one bone is one plant, however wide the patch it belongs to measures");
    Pass("merged patch skeleton: a field of grass in one draw is not a creature, and a creature is not a patch");
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
    TestConfigurationAndWiring();
    std::puts("Vegetation wind regressions passed");
    return 0;
}
catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
