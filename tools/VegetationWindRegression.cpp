// Portable checks for wind-driven vegetation sway. Run from the repository
// root, no SDK needed on Linux:
//   mkdir -p build && g++ -O2 -std=c++20 -Wall -Wextra -Werror -pedantic '
//     tools/VegetationWindRegression.cpp -o build/vegetation-test && ./build/vegetation-test
// The same file is built by tools/Test-VegetationWind.cmd on the Windows
// runner, so the settings parity checked here is also checked where the real
// location-scoped INI read/save/reload path lives.
#include "../src/Environment/VegetationWind.h"
#include "../src/Environment/WindTuning.h"
#include "../src/Environment/WindDebug.h"
#include "../src/Environment/OverlayLayout.h"
#include <cmath>
#include <cstdio>
#include <fstream>
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
    status.skippedShader = 812; status.skippedPalette = 3; status.learnedShaders = 1;
    rows = VegetationDebugLines(status, settings, wind);
    Check(rows[1].find("vs 812") != std::string::npos && rows[1].find("pal 3") != std::string::npos &&
          rows[1].find("new 1") != std::string::npos, "and names every gate that refused, with its count");
    // The two refusals a "nothing sways" report turns on and that no other line
    // can tell apart: the world drawn as scenery (correct, and thousands of
    // draws of it) and the depth-only passes, the client's own shadow map among
    // them, whose palette is in the light's space and must never be leaned.
    status.skippedWorld = 400; status.skippedDepthOnly = 900;
    rows = VegetationDebugLines(status, settings, wind);
    Check(rows[1].find("world 400") != std::string::npos, "the row separates the world's own draws from the census");
    Check(rows[1].find("depth 900") != std::string::npos, "and the depth-only passes, which are never leaned");
    Check(rows[1].find("world 400") < rows[1].find("vs 812"), "in the order the gates run in");
    Check(rows[1].find("vs 812") < rows[1].find("pal 3"), "in the order the gates run in");
    Check(rows[1].find("mat") == std::string::npos, "without offering a solid trunk as a refusal");
    // The row that names the build. Two in-client reports of "it does not work"
    // arrived with counters that read exactly like the fixed build's, from a
    // client still running the previous d3d9.dll, and nothing in the picture
    // could say so. The stamp is the loaded DLL's own file time, so it needs no
    // version to keep in step with the source.
    buildStamp = "2026-10-15 18:32";
    rows = VegetationDebugLines(status, settings, wind);
    Check(rows.size() == 3 && rows[2] == "ModernWoWRenderer 2026-10-15 18:32",
        "the debug rows stamp the build that drew them");
    buildStamp = nullptr;
    rows = VegetationDebugLines(status, settings, wind);
    Check(rows.size() == 2, "and a session that published no build stamps nothing");
    // The rows have to fit the HUD's own box: 600 logical pixels at a 16 pixel
    // font, drawn 10 pixels in from the left and clipped 18 short of the right.
    // A clipped diagnostic is exactly as useful as the "0" the player reported.
    buildStamp = "2026-10-15 18:32";
    for (const auto& row : VegetationDebugLines(status, settings, wind))
        Check(row.size() <= 72, "a vegetation HUD row fits in the box it is drawn in");
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
    const auto layoutGate = sway.find("detail::Layout(detail::BoundDeclaration(device)", scopeStart);
    const auto layoutReport = sway.find("frame.frameIndex, true);", layoutGate);
    Check(scopeStart != sway.npos && firstPaletteRead != sway.npos &&
          linkGate != sway.npos && linkGate < firstPaletteRead &&
          upGate != sway.npos && upGate < firstPaletteRead &&
          shaderGate != sway.npos && shaderGate < firstPaletteRead &&
          layoutGate != sway.npos && layoutGate < firstPaletteRead,
        "the disabled, unlinked and non-model paths return before the first palette read");
    for (const char* counter : { "candidates", "skippedMaterial", "skippedCamera", "skippedViewport",
                                 "skippedShader", "skippedWorld", "skippedLayout", "skippedGeometry",
                                 "skippedDepthOnly", "skippedPalette", "skippedShape", "skippedSize",
                                 "skippedAnimated", "skippedRange", "unbounded", "learnedShaders",
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
    // The build stamp is published by the Windows half (the overlay rows are
    // portable, so the time itself is read where Windows is available) and it is
    // both stamped on the rows and written to the log at startup.
    Check(main.find("renderer::buildStamp = g_buildStamp.c_str();") != main.npos &&
          main.find("GetFileAttributesExW") != main.npos &&
          main.find("FileTimeToSystemTime") != main.npos,
        "the build stamp is the loaded dll's own file time");
    const auto hud = Read("src/Environment/WindDebug.h");
    for (const char* label : { "decl", "world", "vs", "geo", "depth", "cam", "pal", "shape", "size", "move",
                               "far", "new" })
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
        "and the world's own projection refuses one outright");
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
    // here is about a model and must not be the mirror's to hide.
    Check(sway.find("if (admission == detail::Admission::World) ++Status().skippedWorld;") != sway.npos &&
          sway.find("else ++Status().skippedShader;") != sway.npos,
        "a refusal of a model draw is counted, mirror or not");
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
    Check(materialGate != sway.npos && materialGate > shaderGate,
        "and it is asked once, after the cheap gates, so the device call is paid for models only");
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
    Check(colourGate != sway.npos && colourGate < firstLean && colourGate > sway.find("if (!foliageMaterial)"),
        "no draw is leaned before its colour writes are known, and the free state-mirror gate runs first");
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
    TestConfigurationAndWiring();
    std::puts("Vegetation wind regressions passed");
    return 0;
}
catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
