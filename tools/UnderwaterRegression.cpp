// Underwater regressions. Run from the repository root.
//
// The camera's side of the water is read from matrices, and the medium the world is shaded
// through is a handful of closed-form terms, so the whole contract can be checked on the CPU:
// build a camera and a liquid tile, hand their matrices to
// src/Water/UnderwaterMedium.h, and assert what comes out. No device, no Windows, no shader
// compiler.
//
// What is pinned here is the SHIPPED BEHAVIOUR, not implementation detail, because the
// behaviour is what a change can quietly break:
//
//   * the plane reading must be the tile's height over the camera for ANY camera rotation,
//     and must be refused - not guessed at - for a mirrored reflection view, a rotated or
//     scaled world matrix (whose scale cancels out of the identity), a garbage register
//     and a draw that carries no tile offset;
//   * the tracker must enter and leave the water at different depths, so a camera parked at
//     the surface cannot strobe two media, must hold its answer while no liquid is drawn
//     (a diver looking at the seabed has no water tile in frame at all), must complete its
//     ramp in the time a swimmer's eye takes, and must drop the state on a teleport;
//   * the medium must be the water stage's own coefficient, so a seabed fades out with the
//     same number the surface above it fades out with, and the two meet without a seam;
//   * a point at the camera must come back unchanged, and STRENGTH 0 must be an exact
//     identity, because those are the two promises the whole-frame pass makes above water;
//   * the caustic net must be mean-preserving - it moves light around on the bed, it does not
//     create it - and must have no plateau, which is the failure mode that printed a lattice
//     onto the foam;
//   * the beam march must produce beams, not noise: a peak near one and a duty cycle near a
//     fifth of the frame.
//
// The second half pins the mirror: the ps_3_0 stage in src/Effects/Underwater.h is
// renderer::underwater::ShadePixel line for line, and the compositor, the atmosphere and the
// cloud deck all read the one ramp the water stage publishes. A formula that must be faded in
// the pass must be faded in the mirror too, and that is one assertion rather than two in
// different places.
#include "../src/Water/UnderwaterMedium.h"
#include "../src/Water/WaveField.h"
#include "../src/Water/WaterNoise.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace renderer::underwater;

namespace
{

void Require(bool value, const std::string& message)
{
    if (!value) throw std::runtime_error(message);
    std::printf("PASS %s\n", message.c_str());
}

bool Near(double a, double b, double epsilon) { return std::fabs(a - b) <= epsilon; }

std::string ReadAlongside(const char* name)
{
    const std::string here(__FILE__);
    const size_t slash = here.find_last_of("/\\");
    std::vector<std::string> candidates;
    if (slash != std::string::npos) candidates.push_back(here.substr(0, slash + 1) + name);
    candidates.push_back(name);
    candidates.push_back(std::string("tools/") + name);
    candidates.push_back(std::string("../") + name);
    for (const std::string& path : candidates)
    {
        std::ifstream in(path);
        if (in) return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// ------------------------------------------------------------- matrices -----
// This tree's convention, spelled out in src/Water/WaveField.h's DrawAnchor: the client's
// c5..c8 are row-vector matrices whose row i is the view-space image of world axis i, and
// whose row 3 is the translation. So a camera at C looking through a rotation R (world to
// view) is rows 0..2 = R and row 3 = -C*R, and a liquid tile whose world matrix is a pure
// translation to T is the same rows with row 3 = (T-C)*R. Building them that way is what
// makes the reading below a test of the derivation and not of the test.
struct Mat3
{
    float m[3][3] = { { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f } };
};

Mat3 Multiply(const Mat3& a, const Mat3& b)
{
    Mat3 out{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return out;
}

Mat3 AboutX(float a)
{
    const float c = std::cos(a), s = std::sin(a);
    Mat3 out{};
    out.m[1][1] = c; out.m[1][2] = -s; out.m[2][1] = s; out.m[2][2] = c;
    return out;
}

Mat3 AboutY(float a)
{
    const float c = std::cos(a), s = std::sin(a);
    Mat3 out{};
    out.m[0][0] = c; out.m[0][2] = s; out.m[2][0] = -s; out.m[2][2] = c;
    return out;
}

Mat3 AboutZ(float a)
{
    const float c = std::cos(a), s = std::sin(a);
    Mat3 out{};
    out.m[0][0] = c; out.m[0][1] = -s; out.m[1][0] = s; out.m[1][1] = c;
    return out;
}

void Apply(const Mat3& r, const float v[3], float out[3])
{
    for (int k = 0; k < 3; ++k) out[k] = v[0] * r.m[0][k] + v[1] * r.m[1][k] + v[2] * r.m[2][k];
}

// `origin` is the point the matrix's own coordinate frame puts at the view origin: the camera
// for a VIEW transform, and the camera minus the tile's translation for a modelView.
void FillMatrix(const Mat3& rotation, const float origin[3], float out[4][4])
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            out[i][j] = j < 3 ? rotation.m[i][j] : 0.f;
    for (int j = 0; j < 3; ++j) out[3][j] = 0.f;
    out[3][3] = 1.f;
    float translated[3];
    Apply(rotation, origin, translated);
    for (int j = 0; j < 3; ++j) out[3][j] = -translated[j];
}

// ------------------------------------------------------------------ scene ---
// A diver eight yards under a lake, looking at a sandy bed eighteen yards away, with the sun
// up and to the left. Every number below is in the water stage's own register units: the
// absorption triple over 128, the deep colour over 255, the tint over 128.
constexpr float kClarity = 100.f;
constexpr float kAbsorptionPercent = .38f;
const Vec3 kAbsorbRegister{ 255.f / 128.f, 140.f / 128.f, 107.f / 128.f };
const Vec3 kDeepRegister{ 14.f / 255.f, 42.f / 255.f, 40.f / 255.f };
const Vec3 kTintRegister{ 1.f, 1.f, 1.f };

Medium ShippedMedium()
{
    return MediumFor(kClarity, kAbsorptionPercent, kAbsorbRegister, kDeepRegister, kTintRegister, true);
}

PixelInput DiverPixel()
{
    PixelInput in;
    in.scene = { .42f, .38f, .30f };
    in.distance = 18.f;
    in.world = { 9.f, 4.f, -6.f };
    in.ray = Normalize(in.world);
    in.normal = { 0.f, 0.f, 1.f };
    in.up = { 0.f, 0.f, 1.f };
    in.light = Normalize(Vec3{ .2f, .3f, .9f });
    in.lightColour = { 1.f, .96f, .86f };
    in.sunElevation = .7f;
    in.submersion = 8.f;
    in.axis[0] = { 1.f, 0.f, 0.f };
    in.axis[1] = { 0.f, 1.f, 0.f };
    in.axis[2] = { 0.f, 0.f, 1.f };
    return in;
}

Field ShippedField()
{
    Field field;
    // The lattice the water stage builds at the shipped wave size: rows of the world->lattice
    // matrix, already scaled by 1/cell, at the field's own yaw.
    const double cell = double(renderer::water::kRippleCellYards);
    const float ca = std::cos(renderer::water::kLatticeYaw), sa = std::sin(renderer::water::kLatticeYaw);
    field.lattice[0] = float(ca / cell); field.lattice[1] = float(sa / cell);
    field.lattice[2] = float(-sa / cell); field.lattice[3] = float(ca / cell);
    field.chopPhase[0] = 12.3456f; field.chopPhase[1] = -7.25f;
    // The fine phase is the chop phase doubled (the frequency the net applies to
    // the cell), which is what anchors the fine octave to the bed.
    field.finePhase[0] = 12.3456f * 2.f; field.finePhase[1] = -7.25f * 2.f;
    field.warpPhase[0] = 4.75f; field.warpPhase[1] = -2.25f;
    field.seconds = 37.5f;
    return field;
}

// --------------------------------------------------------------- sections ---

void TestPlaneReading()
{
    std::printf("\n-- the camera's side of the interface, read from the draw --\n");
    const float camera[3] = { 10.f, 20.f, 5.f };
    const float tile[3] = { 100.f, 200.f, 7.f };

    // The reading is the tile plane's height over the camera, and it has to stay that number
    // whatever the camera is doing: a swimmer looking straight down, a diver pitched up at the
    // surface and a player with the camera rolled all stand in the same water. It is read
    // from the draw's own matrix alone - no second matrix is consulted, because a
    // shader-driven client leaves its fixed-function view state holding whatever the last
    // fixed-function draw put there, and a reading that trusted it read nothing at all on a
    // real client (the whole medium silently never engaged).
    {
        Mat3 identity{};
        float view[4][4], modelView[4][4];
        FillMatrix(identity, camera, view);
        // modelView = WORLD * VIEW: the same basis, and the camera in the tile's own frame.
        const float tileCamera[3] = { camera[0] - tile[0], camera[1] - tile[1], camera[2] - tile[2] };
        FillMatrix(identity, tileCamera, modelView);
        const auto reading = PlaneOverCamera(modelView, nullptr, nullptr);
        Require(reading.valid && Near(reading.yards, tile[2] - camera[2], 1e-4),
                "an unrotated camera reads the liquid plane's own height over it");
        // And a draw whose c5..c8 carried no world offset at all - modelView IS the view
        // transform - reads the world's own z=0 plane rather than the liquid's, which is
        // -camera.z here. That is a real number from a real matrix, so nothing local can
        // refuse it; the kMaxPlaneYards bound is what keeps it from being believed at the
        // distances a continent-scale origin produces, and the tracker's held value is what
        // the caller falls back to.
        const auto noTile = PlaneOverCamera(view, nullptr, nullptr);
        Require(noTile.valid && Near(noTile.yards, -camera[2], 1e-4),
                "a draw with no tile offset reads the world's own z=0 plane, which the bound exists to refuse");
        // A tile may carry its water level in its vertices rather than its matrix. The
        // caller reads consumed vertices' local positions and the reading measures THOSE
        // points' plane: the same matrix with a vertex 20 local units up its own axis 2
        // puts the plane 20 yards higher. Without a vertex the draw origin's plane is
        // measured, which for such a tile is one whole water level away from the water.
        const Vec3 vertexTwentyUp{ 0.f, 0.f, 20.f };
        const auto carried = PlaneOverCamera(modelView, &vertexTwentyUp, nullptr);
        Require(carried.valid && Near(carried.yards, tile[2] - camera[2] + 20.f, 1e-4),
                "a vertex carried by the tile moves the measured plane with it");
        const auto ignored = PlaneOverCamera(modelView, nullptr, nullptr);
        Require(ignored.valid && Near(ignored.yards, tile[2] - camera[2], 1e-4),
                "an unread vertex is not guessed at: the origin's plane is measured instead");
        // The lift goes through the matrix like the vertex does: a tripled third axis
        // triples it, exactly as the vertex stage would place the vertex.
        modelView[2][2] *= 3.f;
        const Vec3 vertexSevenUp{ 0.f, 0.f, 7.f };
        const auto scaledLift = PlaneOverCamera(modelView, &vertexSevenUp, nullptr);
        Require(scaledLift.valid && Near(scaledLift.yards, tile[2] - camera[2] + 21.f, 1e-4),
                "the vertex is lifted by the draw's own third axis, scale and all");
        const Vec3 garbageVertex{ 0.f, 0.f, std::numeric_limits<float>::quiet_NaN() };
        Require(!PlaneOverCamera(modelView, &garbageVertex, nullptr).valid
                    && PlaneOverCamera(modelView, &garbageVertex, nullptr).reason == 1,
                "a garbage vertex is refused with reason 1");
    }
    {
        std::mt19937 rng(20261018u);
        std::uniform_real_distribution<float> angle(-3.1f, 3.1f);
        std::uniform_real_distribution<float> position(-4000.f, 4000.f);
        bool all = true, invariance = true;
        for (int attempt = 0; attempt < 64; ++attempt)
        {
            const Mat3 rotation = Multiply(Multiply(AboutZ(angle(rng)), AboutY(angle(rng))), AboutX(angle(rng)));
            const float swimmer[3] = { position(rng), position(rng), position(rng) * .01f };
            const float plane[3] = { swimmer[0] + 40.f, swimmer[1] - 25.f, swimmer[2] + 6.f };
            float modelView[4][4];
            const float planeCamera[3] = { swimmer[0] - plane[0], swimmer[1] - plane[1], swimmer[2] - plane[2] };
            FillMatrix(rotation, planeCamera, modelView);
            const auto reading = PlaneOverCamera(modelView, nullptr, nullptr);
            all = all && reading.valid && !reading.negativeDeterminant;
            // In float32 the continent-scale coordinates lose the last yard or so of a
            // six-yard answer, so the tolerance is the reconstruction's, not the derivation's.
            invariance = invariance && Near(reading.yards, plane[2] - swimmer[2], .05);
        }
        Require(all && invariance,
                "the reading is the plane's height over the camera for every camera rotation and position");
    }
    {
        // A mirrored basis still describes a plane and still measures it - the cross of
        // the first two rows, oriented to the third, does not care about handedness. An
        // earlier version refused every draw whose basis determinant was negative on the
        // theory that the sign meant "the reflection pass"; the sign is only a handedness
        // convention between the client's world and view spaces, which nothing here ever
        // established, so guessing it wrong refuses every ordinary draw instead. The sign
        // is reported for the log, and the client's actual reflection draws are excluded
        // by their render target in WaterEffect.h (pinned with the wiring below).
        Mat3 mirrored = Multiply(AboutZ(.4f), AboutY(-.9f));
        mirrored.m[1][0] = -mirrored.m[1][0];
        mirrored.m[1][1] = -mirrored.m[1][1];
        mirrored.m[1][2] = -mirrored.m[1][2];
        float modelView[4][4];
        const float tileCamera[3] = { camera[0] - tile[0], camera[1] - tile[1], camera[2] - tile[2] };
        FillMatrix(mirrored, tileCamera, modelView);
        const auto reading = PlaneOverCamera(modelView, nullptr, nullptr);
        Require(reading.valid && reading.negativeDeterminant && BasisDeterminant(modelView) < 0.f
                    && Near(reading.yards, tile[2] - camera[2], 1e-4),
                "a mirrored basis is measured and reported, never refused over its determinant's sign");
    }
    {
        // What the reading accepts, and the reason codes pinned per refusal: degenerate(1),
        // sheared(3), out of range(4). A scaled tile reports the same plane as an unscaled
        // one (row 3 of S*T*V does not depend on the scale S, its fourth row is (0,0,0,1),
        // and the cross normalises the in-plane scales away). A tile YAWED in its own plane
        // is the same water at the same height. Shear is refused, because then the cross of
        // two rows is no longer the third row's direction and "up out of the plane" stops
        // being one direction.
        const Mat3 viewRotation = Multiply(AboutZ(.3f), AboutX(-.2f));
        float modelView[4][4];
        const float tileCamera[3] = { camera[0] - tile[0], camera[1] - tile[1], camera[2] - tile[2] };

        Mat3 scaled = viewRotation;
        for (int j = 0; j < 3; ++j) scaled.m[0][j] *= 2.f;
        FillMatrix(scaled, tileCamera, modelView);
        const PlaneReading scaledReading = PlaneOverCamera(modelView, nullptr, nullptr);
        Require(scaledReading.valid && Near(scaledReading.yards, tile[2] - camera[2], 1e-3),
                "a tile drawn under a doubled x scale reports the same plane: the scale cancels");
        for (int j = 0; j < 3; ++j) scaled.m[1][j] *= .5f;
        FillMatrix(scaled, tileCamera, modelView);
        const PlaneReading nonUniform = PlaneOverCamera(modelView, nullptr, nullptr);
        Require(nonUniform.valid && Near(nonUniform.yards, tile[2] - camera[2], 1e-3),
                "a non-uniformly scaled tile also reports the same plane");

        // Yawed in its own plane: the first two axes mixed, the third untouched, the
        // origin untouched. Same water, same height.
        FillMatrix(viewRotation, tileCamera, modelView);
        const float cy = std::cos(.7f), sy = std::sin(.7f);
        for (int j = 0; j < 3; ++j)
        {
            const float r0 = modelView[0][j], r1 = modelView[1][j];
            modelView[0][j] = cy * r0 + sy * r1;
            modelView[1][j] = -sy * r0 + cy * r1;
        }
        const PlaneReading yawed = PlaneOverCamera(modelView, nullptr, nullptr);
        Require(yawed.valid && Near(yawed.yards, tile[2] - camera[2], 1e-3),
                "a tile yawed in its own plane reports the same height");

        Mat3 sheared = viewRotation;
        for (int j = 0; j < 3; ++j) sheared.m[0][j] += .3f * viewRotation.m[2][j];
        FillMatrix(sheared, tileCamera, modelView);
        Require(!PlaneOverCamera(modelView, nullptr, nullptr).valid
                    && PlaneOverCamera(modelView, nullptr, nullptr).reason == 3,
                "a sheared basis (row 0 tilted toward row 2) is still refused");

        FillMatrix(viewRotation, tileCamera, modelView);
        modelView[3][1] = std::numeric_limits<float>::quiet_NaN();
        Require(!PlaneOverCamera(modelView, nullptr, nullptr).valid
                    && PlaneOverCamera(modelView, nullptr, nullptr).reason == 1,
                "a garbage translation register is refused with reason 1");

        // Beyond the bound this is not a body of water a player can be inside.
        const float farPlane[3] = { tile[0], tile[1], camera[2] + kMaxPlaneYards * 2.f };
        const float farCamera[3] = { camera[0] - farPlane[0], camera[1] - farPlane[1], camera[2] - farPlane[2] };
        FillMatrix(viewRotation, farCamera, modelView);
        Require(!PlaneOverCamera(modelView, nullptr, nullptr).valid
                    && PlaneOverCamera(modelView, nullptr, nullptr).reason == 4,
                "a plane further than any swim is refused, so the caller holds its previous answer");
    }
    {
        // HEIGHT ALONE CANNOT SAY WHOSE WATER IT IS. The first/last consumed vertices
        // locate the measured water horizontally: a reading counts as local only when
        // the camera is within about the segment's own span of it. This is the guard
        // for the failure that put the medium on dry land in Mulgore and kept it there
        // across restarts: a lake drawn high on a plateau somewhere in view reads the
        // same "two yards over the camera" as the lake the camera is floating in.
        Mat3 identity{};
        float modelView[4][4];
        const Vec3 first{ 0.f, 0.f, 0.f };
        const Vec3 last{ 50.f, 0.f, 0.f };
        // A tile 700 yards east at +2 over the camera, 50 yards wide: a true height,
        // and somebody else's water.
        const float farTile[3] = { 700.f, 0.f, 2.f };
        const float farRel[3] = { -farTile[0], -farTile[1], -farTile[2] };
        FillMatrix(identity, farRel, modelView);
        const auto far = PlaneOverCamera(modelView, &first, &last);
        Require(far.valid && Near(far.yards, 2.f, 1e-4) && !far.local
                    && Near(far.horizontalYards, 700.f, .01) && Near(far.spanYards, 50.f, .01),
                "a lake 700 yards away at swimming height measures truly and is not local");
        // The same tile at 30 yards: the camera's own water.
        const float nearTile[3] = { 30.f, 0.f, 2.f };
        const float nearRel[3] = { -nearTile[0], -nearTile[1], -nearTile[2] };
        FillMatrix(identity, nearRel, modelView);
        const auto near = PlaneOverCamera(modelView, &first, &last);
        Require(near.valid && near.local && Near(near.horizontalYards, 30.f, .01),
                "the same reading from the camera's own shore is local");
        // A swimmer mid-lake: the sampled diagonal passes UNDER the camera - distance
        // zero, however wide the lake is.
        const float midTile[3] = { -40.f, 0.f, 2.f };
        const float midRel[3] = { -midTile[0], -midTile[1], -midTile[2] };
        const Vec3 wide{ 100.f, 0.f, 0.f };
        FillMatrix(identity, midRel, modelView);
        const auto mid = PlaneOverCamera(modelView, &first, &wide);
        Require(mid.valid && mid.local && mid.horizontalYards < .01f,
                "a camera over the middle of the tile it samples is at distance zero");
        // One vertex only: no span to judge by, so the loose fallback radius decides.
        FillMatrix(identity, farRel, modelView);
        Require(!PlaneOverCamera(modelView, &first, nullptr).local,
                "a lone vertex 700 yards away is past the fallback radius");
        const float mid300[3] = { 300.f, 0.f, 2.f };
        const float mid300Rel[3] = { -mid300[0], -mid300[1], -mid300[2] };
        FillMatrix(identity, mid300Rel, modelView);
        Require(PlaneOverCamera(modelView, &first, nullptr).local,
                "a lone vertex 300 yards away is inside the fallback radius (a lake can be that wide)");
        // A tilted sheet (a river): the answer is the mean of the measured ends.
        const float flatRel[3] = { 0.f, 0.f, 0.f };
        FillMatrix(identity, flatRel, modelView);
        const Vec3 tiltedLast{ 60.f, 0.f, 6.f };
        const auto tilted = PlaneOverCamera(modelView, &first, &tiltedLast);
        Require(tilted.valid && Near(tilted.yards, 3.f, 1e-4),
                "a tilted sheet is overhead by the mean of its measured ends");
    }
    {
        // LOOSLY LOCAL IS NOT UNDER THE SHEET. With two consumed vertices the
        // sampled pair bounds the chunk in both in-plane axes, so the reading can
        // say where the camera falls against that rectangle rather than only how
        // near its diagonal passes. This is the reported dry-land case: on the
        // shore, beside and below a raised sheet, looking up at it - the segment
        // distance calls it the camera's water (it is the same lake), while the
        // footprint says the camera is ten yards outside the water it measured.
        Mat3 identity{};
        float modelView[4][4];
        const Vec3 first{ 0.f, 0.f, 0.f };
        const Vec3 opposite{ 60.f, 40.f, 0.f };
        const float besideTile[3] = { -30.f, -50.f, 2.f };
        const float besideRel[3] = { -besideTile[0], -besideTile[1], -besideTile[2] };
        FillMatrix(identity, besideRel, modelView);
        const auto beside = PlaneOverCamera(modelView, &first, &opposite);
        Require(beside.valid && beside.local && Near(beside.yards, 2.f, 1e-4)
                    && beside.footprintKnown && Near(beside.footprintYards, 10.f, .01)
                    && !beside.containsCamera,
                "a raised sheet ten yards to the side is the same lake, and is not over the camera");
        // Inside the rectangle but yards off its diagonal: a segment-only answer
        // would call this merely near; the footprint knows the swimmer is in it.
        const float overTile[3] = { -30.f, -35.f, 2.f };
        const float overRel[3] = { -overTile[0], -overTile[1], -overTile[2] };
        FillMatrix(identity, overRel, modelView);
        const auto over = PlaneOverCamera(modelView, &first, &opposite);
        Require(over.valid && over.local && over.footprintKnown && over.containsCamera
                    && over.footprintYards == 0.f && over.horizontalYards > 5.f,
                "a camera inside the measured chunk is under it even off the sampled diagonal");
        // Crossing at a chunk seam: two yards past the sampled edge is still entry
        // water, not yet a new lake - the sampled corners under-measure the chunk.
        const float seamTile[3] = { -30.f, -42.f, 2.f };
        const float seamRel[3] = { -seamTile[0], -seamTile[1], -seamTile[2] };
        FillMatrix(identity, seamRel, modelView);
        const auto seam = PlaneOverCamera(modelView, &first, &opposite);
        Require(seam.valid && seam.footprintKnown && Near(seam.footprintYards, 2.f, .01)
                    && seam.containsCamera,
                "the footprint carries a shoreline's tolerance at the sampled edge");
        // A pair with no real extent in one axis cannot bound a rectangle at all:
        // the reading says so, and the tracker falls back to horizontal closeness.
        const Vec3 sliver{ 60.f, 2.f, 0.f };
        const auto degenerate = PlaneOverCamera(modelView, &first, &sliver);
        Require(degenerate.valid && !degenerate.footprintKnown && !degenerate.containsCamera,
                "a sliver of sampled water makes no footprint claim it cannot support");
        // A RECTANGLE IS NOT A SHEET. A liquid chunk enables its tiles one by
        // one, so the emitted triangles can form an L (a shore cell, the
        // inside of a bend) inside a first/last rectangle that also covers
        // dry ground. Handed the triangles, the reading must answer from
        // them: inside the rectangle but over the missing corner is ten
        // yards from the nearest water the draw emits, not "under the sheet".
        const Vec3 ell[] = {
            { 0.f, 0.f, 0.f }, { 20.f, 0.f, 0.f }, { 20.f, 40.f, 0.f },
            { 0.f, 0.f, 0.f }, { 20.f, 40.f, 0.f }, { 0.f, 40.f, 0.f },
            { 20.f, 0.f, 0.f }, { 60.f, 0.f, 0.f }, { 60.f, 15.f, 0.f },
            { 20.f, 0.f, 0.f }, { 60.f, 15.f, 0.f }, { 20.f, 15.f, 0.f } };
        const LiquidCoverage ellCoverage{ ell, 12, LiquidTopology::TriangleList };
        const float holeTile[3] = { -30.f, -35.f, 2.f };
        const float holeRel[3] = { -holeTile[0], -holeTile[1], -holeTile[2] };
        FillMatrix(identity, holeRel, modelView);     // local (30,35): in the rectangle, over the dry corner
        const auto hole = PlaneOverCamera(modelView, &first, &opposite, &ellCoverage);
        Require(hole.valid && hole.coverageKnown && hole.footprintKnown
                    && Near(hole.footprintYards, 10.f, .01) && !hole.containsCamera
                    && Near(hole.horizontalYards, 10.f, .01),
                "a camera over a dry corner inside the chunk's rectangle is measured against the emitted triangles");
        const float wetTile[3] = { -10.f, -35.f, 2.f };
        const float wetRel[3] = { -wetTile[0], -wetTile[1], -wetTile[2] };
        FillMatrix(identity, wetRel, modelView);      // local (10,35): over the L's upright
        const auto wet = PlaneOverCamera(modelView, &first, &opposite, &ellCoverage);
        Require(wet.valid && wet.coverageKnown && wet.containsCamera && wet.footprintYards == 0.f,
                "a camera over an emitted triangle is under the sheet the draw actually drew");
        // The height is the nearest triangle's own, at the camera's point on
        // it - not the mean of the first and last ends, which on a falling
        // river can be yards of water that is somewhere else entirely.
        const Vec3 slope[] = { { 0.f, 0.f, 0.f }, { 30.f, 0.f, 6.f }, { 0.f, 30.f, 0.f } };
        const LiquidCoverage slopeCoverage{ slope, 3, LiquidTopology::TriangleList };
        const float slopeTile[3] = { -10.f, -10.f, 2.f };
        const float slopeRel[3] = { -slopeTile[0], -slopeTile[1], -slopeTile[2] };
        FillMatrix(identity, slopeRel, modelView);    // the triangle's centroid
        const auto sloped = PlaneOverCamera(modelView, &first, &opposite, &slopeCoverage);
        Require(sloped.valid && sloped.coverageKnown && sloped.containsCamera
                    && Near(sloped.yards, 4.f, .01),
                "a tilted sheet is overhead by its own height at the camera, interpolated on its triangle");
        // Strips emit the same water with shared vertices; coverage must not
        // care which topology spelled it.
        const Vec3 strip[] = { { 0.f, 0.f, 0.f }, { 20.f, 0.f, 0.f },
                               { 0.f, 40.f, 0.f }, { 20.f, 40.f, 0.f } };
        const LiquidCoverage stripCoverage{ strip, 4, LiquidTopology::TriangleStrip };
        FillMatrix(identity, wetRel, modelView);
        Require(PlaneOverCamera(modelView, &first, &opposite, &stripCoverage).containsCamera,
                "a triangle strip's coverage contains the camera the same triangles would as a list");
        // Caller-measured bounds are an optimization, never a different
        // answer: the same sheet with its exact extremes attached reads
        // identically - contained where it contained, the same ten yards
        // short over the dry corner.
        LiquidCoverage ellBounded{ ell, 12, LiquidTopology::TriangleList };
        ellBounded.minX = 0.f; ellBounded.maxX = 60.f;
        ellBounded.minY = 0.f; ellBounded.maxY = 40.f;
        ellBounded.boundsKnown = true;
        FillMatrix(identity, holeRel, modelView);
        const auto holeBounded = PlaneOverCamera(modelView, &first, &opposite, &ellBounded);
        FillMatrix(identity, wetRel, modelView);
        const auto wetBounded = PlaneOverCamera(modelView, &first, &opposite, &ellBounded);
        Require(holeBounded.valid && holeBounded.coverageKnown && !holeBounded.containsCamera
                    && Near(holeBounded.footprintYards, hole.footprintYards, 1e-4)
                    && Near(holeBounded.yards, hole.yards, 1e-4)
                    && wetBounded.valid && wetBounded.containsCamera
                    && Near(wetBounded.yards, wet.yards, 1e-4)
                    && Near(wetBounded.spanYards, wet.spanYards, 1e-4),
                "coverage with caller-measured bounds answers exactly as coverage measured on the spot");
        // And a sheet far beyond locality reach is refused from its box
        // alone: the decisions are the scan's own (not local, not
        // containing, may not start anything), while the reported distance
        // is the box's - a lower bound on the nearest triangle's, spent
        // only by the log. Local (360,340) against a 60x40 sheet: the box
        // is 300 yards short on each axis.
        const float farTile[3] = { -360.f, -340.f, 2.f };
        const float farRel[3] = { -farTile[0], -farTile[1], -farTile[2] };
        FillMatrix(identity, farRel, modelView);
        const auto farSheet = PlaneOverCamera(modelView, &first, &opposite, &ellBounded);
        const auto farSheetUnbounded = PlaneOverCamera(modelView, &first, &opposite, &ellCoverage);
        Require(farSheet.valid && farSheet.coverageKnown && farSheet.footprintKnown
                    && !farSheet.containsCamera && !farSheet.local
                    && !MayStartSubmersion(farSheet)
                    && Near(farSheet.footprintYards, 424.264f, .01)
                    && Near(farSheetUnbounded.footprintYards, farSheet.footprintYards, 1e-4)
                    && !farSheetUnbounded.local,
                "a sheet beyond locality reach is refused from its bounding box, bounded or not");
    }
}

void TestTracker()
{
    std::printf("\n-- the frame's answer, settled over time --\n");
    {
        // Entering takes more water over the camera than leaving does, so a camera parked
        // exactly at the surface cannot toggle a whole frame's fog pipeline on and off.
        Tracker tracker;
        tracker.BeginFrame();
        tracker.Observe(Tracker::kEnterYards - .01f);
        tracker.Advance(.25f, nullptr, false);
        const bool belowEnter = tracker.submerged;
        tracker.BeginFrame();
        tracker.Observe(Tracker::kEnterYards + .5f);
        tracker.Advance(.25f, nullptr, false);
        const bool aboveEnter = tracker.submerged;
        tracker.BeginFrame();
        tracker.Observe(Tracker::kExitYards + .001f);
        tracker.Advance(.25f, nullptr, false);
        const bool stillIn = tracker.submerged;
        Require(!belowEnter && aboveEnter && stillIn,
                "the tracker enters and leaves the water at different depths, so a bobbing swimmer cannot strobe");
    }
    {
        // The ramp the air medium fades out on and the world medium fades in on: crossing the
        // surface moves one into the other instead of swapping them, and it completes inside a
        // yard and a half.
        Tracker tracker;
        tracker.BeginFrame();
        tracker.Observe(-1.f);
        tracker.Advance(.25f, nullptr, false);
        const float air = tracker.ramp;
        tracker.BeginFrame();
        tracker.Observe(Tracker::kRampYards * 2.f);
        tracker.Advance(.25f, nullptr, false);
        const float deep = tracker.ramp;
        tracker.BeginFrame();
        tracker.Observe(Tracker::kRampYards);
        tracker.Advance(.25f, nullptr, false);
        Require(air == 0.f && deep == 1.f && Near(tracker.ramp, 1.f, 1e-6),
                "the submersion ramp is zero in air and complete a yard and a half under");
    }
    {
        // A diver looking at the seabed has no liquid tile in the frustum at all, so no draw
        // reports a plane. What the tracker remembers is the plane's own height in the world,
        // so a frame that drew nothing still knows where the camera is relative to it: the
        // medium stays on a swimmer who simply looked down, and comes off one who rose.
        Tracker tracker;
        const float diving[3] = { 0.f, 0.f, -8.f };
        for (int frame = 0; frame < 60; ++frame)
        {
            tracker.BeginFrame();
            tracker.Observe(8.f);
            tracker.Advance(1.f / 60.f, diving, true);
        }
        const float settled = tracker.yards;
        Require(Near(settled, 8.f, .01) && tracker.submerged && tracker.ramp == 1.f && tracker.planeKnown,
                "a settled diver is a yard and a half or more under, and the plane's height is remembered");
        float held = settled;
        uint64_t frames = 0;
        for (int frame = 0; frame < 600; ++frame)
        {
            tracker.BeginFrame();
            tracker.Advance(1.f / 60.f, diving, true);
            held = tracker.yards;
            frames = tracker.heldFrames;
        }
        Require(held >= settled - 1e-4 && Near(held, 8.f, .01) && frames == 600
                    && tracker.submerged && tracker.ramp == 1.f,
                "ten seconds of frames that drew no liquid at all leave a diver looking at the seabed underwater");
        // The same diver, rising: no draw reported anything, and the answer still comes off.
        const float surfaced[3] = { 0.f, 0.f, 1.f };
        for (int frame = 0; frame < 30; ++frame)
        {
            tracker.BeginFrame();
            tracker.Advance(1.f / 60.f, surfaced, true);
        }
        Require(!tracker.submerged && tracker.ramp == 0.f && tracker.yards < 0.f,
                "a swimmer who rises out of the water leaves it with no liquid draw in frame to say so");
        // And a frame with no camera at all changes nothing, because there is nothing to
        // measure the remembered plane against.
        Tracker blind;
        const float inWater[3] = { 0.f, 0.f, -4.f };
        for (int frame = 0; frame < 60; ++frame)
        {
            blind.BeginFrame();
            blind.Observe(4.f);
            blind.Advance(1.f / 60.f, inWater, true);
        }
        const float before = blind.yards;
        for (int frame = 0; frame < 120; ++frame)
        {
            blind.BeginFrame();
            blind.Advance(1.f / 60.f, nullptr, false);
        }
        Require(Near(blind.yards, before, 1e-4) && blind.submerged,
                "a frame with no camera and no liquid holds its answer exactly instead of drifting to air");
        // A player who climbs out onto the sand: the camera rises through the remembered plane
        // while the lake is behind them and out of the frustum.
        Tracker beach;
        for (int frame = 0; frame < 60; ++frame)
        {
            beach.BeginFrame();
            beach.Observe(3.f);
            beach.Advance(1.f / 60.f, inWater, true);
        }
        const float onSand[3] = { 12.f, 6.f, 5.f };
        for (int frame = 0; frame < 60; ++frame)
        {
            beach.BeginFrame();
            beach.Advance(1.f / 60.f, onSand, true);
        }
        Require(!beach.submerged && beach.ramp == 0.f,
                "walking up a beach away from the water leaves the water, with nothing drawn to report it");

        // How fast the medium arrives: a third of a second to 95% of a new reading, which is
        // what a surfacing player feels, and not so fast that a bobbing swimmer strobes.
        Tracker arriving;
        const float atTheSurface[3] = { 0.f, 0.f, 0.f };
        for (int frame = 0; frame < 60; ++frame)
        {
            arriving.BeginFrame();
            arriving.Observe(-1.f);
            arriving.Advance(1.f / 60.f, atTheSurface, true);
        }
        arriving.BeginFrame();
        arriving.Observe(6.f);
        arriving.Advance(1.f / 60.f, atTheSurface, true);
        int framesTo95 = 1;
        while (arriving.yards < 6.f * .95f && framesTo95 < 120)
        {
            arriving.BeginFrame();
            arriving.Observe(6.f);
            arriving.Advance(1.f / 60.f, atTheSurface, true);
            ++framesTo95;
        }
        Require(framesTo95 > 15 && framesTo95 < 30,
                "a diver's medium arrives in about a third of a second, not instantly and not gradually");
    }
    {
        // A zone change, a teleport or a hearth moves the camera further in one frame than any
        // swim can, and the state must not follow: a player who hearsthed out of Vashj'ir is
        // not underwater any more, and never looks at the water again to say so.
        Tracker tracker;
        const float inWater[3] = { 100.f, 100.f, -8.f };
        for (int frame = 0; frame < 60; ++frame)
        {
            tracker.BeginFrame();
            tracker.Observe(8.f);
            tracker.Advance(1.f / 60.f, inWater, true);
        }
        Require(tracker.submerged && Near(tracker.yards, 8.f, .01) && tracker.planeKnown
                    && Near(tracker.planeZ, 0.f, .01),
                "a diver is submerged, and the plane is remembered at the world height it was read from");
        const float hearthed[3] = { -3000.f, 2400.f, 60.f };
        tracker.BeginFrame();
        tracker.Advance(1.f / 60.f, hearthed, true);
        Require(!tracker.planeKnown && tracker.raw < 0.f,
                "a camera that jumps further than any swim drops the remembered plane on the frame it lands");
        for (int frame = 0; frame < 30; ++frame)
        {
            tracker.BeginFrame();
            tracker.Advance(1.f / 60.f, hearthed, true);
        }
        Require(!tracker.submerged && tracker.yards < 0.f && tracker.ramp == 0.f,
                "a hearth out of the water is out of the water half a second later, with nothing drawn to say so");
        // The jump reset must also land when the arrival frame DOES observe: a teleport
        // onto a plateau under a raised lake keeps reporting a plane over the camera
        // forever, and a reset that only ran on unobserved frames never fired there (the
        // Mulgore report: medium on land, kept across restarts). Non-local readings do
        // not observe at all, and the jump still clears the state on the landing frame.
        Tracker arrived;
        for (int frame = 0; frame < 60; ++frame)
        {
            arrived.BeginFrame();
            arrived.Observe(8.f);
            arrived.Advance(1.f / 60.f, inWater, true);
        }
        PlaneReading raised;
        raised.valid = true; raised.local = false; raised.yards = 3.f;
        for (int frame = 0; frame < 5; ++frame)
        {
            arrived.BeginFrame();
            arrived.Observe(raised);
            arrived.Advance(1.f / 60.f, hearthed, true);
        }
        Require(!arrived.observed && !arrived.submerged && arrived.yards < 0.f
                    && !arrived.planeKnown,
                "a teleport landing under somebody else's lake clears the state and never re-observes it");
        // The reset must not eat the landing frame's own evidence either: a
        // teleport INTO water arrives with a fresh overhead reading, and
        // that reading - not the stale state just dropped - is remembered
        // from the first frame on.
        Tracker landedIn;
        for (int frame = 0; frame < 60; ++frame)
        {
            landedIn.BeginFrame();
            landedIn.Observe(8.f);
            landedIn.Advance(1.f / 60.f, inWater, true);
        }
        PlaneReading landing;
        landing.valid = true; landing.local = true; landing.yards = 5.f;
        landing.footprintKnown = true; landing.containsCamera = true;
        const float landingSpot[3] = { 4000.f, -3000.f, -5.f };
        landedIn.BeginFrame();
        landedIn.Observe(landing);
        landedIn.Advance(1.f / 60.f, landingSpot, true);
        Require(landedIn.planeKnown && Near(landedIn.planeZ, 0.f, .01),
                "a teleport into water remembers the landing frame's own overhead plane");
        // A camera whose height is finite but whose horizontal position is
        // not must count as no camera at all: the jump test and the plane's
        // anchor spend x and y, and a NaN would pass every comparison and
        // anchor the remembered plane somewhere it could never expire from.
        Tracker blindHorizontal;
        const float nanCamera[3] = { std::numeric_limits<float>::quiet_NaN(), 0.f, -8.f };
        blindHorizontal.BeginFrame();
        blindHorizontal.Observe(8.f);
        blindHorizontal.Advance(1.f / 60.f, nanCamera, true);
        Require(!blindHorizontal.planeKnown && !blindHorizontal.cameraValid,
                "a camera with no finite horizontal position anchors no remembered plane");
        // And the remembered plane expires with horizontal distance even without a
        // jump: walk far enough from where the water was seen, with no liquid drawn,
        // and its height stops answering for the camera.
        Tracker walker;
        float walking[3] = { 100.f, 100.f, -8.f };
        for (int frame = 0; frame < 60; ++frame)
        {
            walker.BeginFrame();
            walker.Observe(8.f);
            walker.Advance(1.f / 60.f, walking, true);
        }
        for (int frame = 0; frame < 120; ++frame)
        {
            walking[0] += 5.f; walking[2] = -8.f;   // 600 yards east, plane never redrawn
            walker.BeginFrame();
            walker.Advance(1.f / 60.f, walking, true);
        }
        Require(!walker.planeKnown && !walker.submerged,
                "the remembered plane is forgotten once the camera is far from where it was observed");
        // And a swim does not: the same diver, one frame of ordinary movement later.
        Tracker swimmer;
        const float first[3] = { 100.f, 100.f, -8.f };
        for (int frame = 0; frame < 60; ++frame)
        {
            swimmer.BeginFrame();
            swimmer.Observe(8.f);
            swimmer.Advance(1.f / 60.f, first, true);
        }
        // Ordinary swimming: the camera moves a foot and the water is still over it, so the
        // remembered plane follows the draw that reported it and nothing is dropped.
        float position[3] = { first[0], first[1], first[2] };
        for (int frame = 0; frame < 120; ++frame)
        {
            position[0] += .4f;
            position[1] += .2f;
            position[2] = first[2] + .35f * std::sin(float(frame) * .3f);   // a swimmer's bob
            swimmer.BeginFrame();
            swimmer.Observe(8.f - (position[2] - first[2]));
            swimmer.Advance(1.f / 60.f, position, true);
        }
        Require(swimmer.submerged && Near(swimmer.yards, 8.f, .1) && swimmer.planeKnown,
                "ordinary swimming does not drop the state");
    }
    {
        // THE BAND MAY NOT LIFT A DRY CAMERA. `waveBandYards` is how far a crest stands over
        // the liquid tile's mean plane at the camera, and it is added to the depth so a swimmer
        // just under the mean stays in the medium through a swell. Added to an ABOVE-surface
        // reading it inverted the very thing it exists for: on a shore slope, a beach or the
        // lip of a canal, an eye half a yard over the water plus a crest's reach read as
        // "under water", and the hysteresis - a sixth of a yard - then kept it there. This is
        // the reported "underwater shader on ground slopes" beside water.
        Tracker slope;
        const float onTheShore[3] = { 40.f, 40.f, 2.f };
        for (int frame = 0; frame < 300; ++frame)
        {
            slope.BeginFrame();
            slope.Observe(-0.5f);   // the camera, half a yard above the water it stands beside
            slope.Advance(1.f / 60.f, onTheShore, true, true, 1.4f);
        }
        Require(!slope.submerged && slope.ramp == 0.f,
                "a crest's reach cannot put a camera above the mean plane under water, however close to the surface it floats");
        // ... and the reported sequence: swim out of one lake, land on a slope beside another.
        // The state arrives submerged, and the shore's own readings are what must take it off.
        Tracker landed;
        const float inWater[3] = { 0.f, 0.f, -8.f };
        for (int frame = 0; frame < 60; ++frame)
        {
            landed.BeginFrame();
            landed.Observe(8.f);
            landed.Advance(1.f / 60.f, inWater, true);
        }
        Require(landed.submerged && landed.ramp == 1.f, "a swimmer starts submerged");
        for (int frame = 0; frame < 60; ++frame)
        {
            landed.BeginFrame();
            landed.Observe(-1.2f);
            landed.Advance(1.f / 60.f, onTheShore, true, true, 1.4f);
        }
        Require(!landed.submerged && landed.ramp == 0.f && landed.yards < 0.f,
                "a swimmer who lands on a shore slope near other water is off the medium in under a second, not held by the waves' reach");
        // A camera that IS under the mean plane keeps the reach, which is what it was added
        // for, and the reach is at full strength by the depth the hysteresis calls submerged.
        Tracker diver;
        const float under[3] = { 0.f, 0.f, -0.2f };
        for (int frame = 0; frame < 60; ++frame)
        {
            diver.BeginFrame();
            diver.Observe(0.22f);
            diver.Advance(1.f / 60.f, under, true, true, 1.4f);
        }
        Require(diver.submerged && diver.ramp > 0.9f,
                "a crest's reach is still worth a full ramp to a camera that is just under the mean plane");
        // The gate is smooth: a camera bobbing right at the plane cannot strobe between a
        // full band and none, because the band arrives in proportion to the depth.
        Tracker bobbing;
        float step = 0.f, previous = 0.f, top = 0.f;
        for (int frame = 0; frame < 600; ++frame)
        {
            bobbing.BeginFrame();
            bobbing.Observe(0.1f + 0.1f * std::sin(float(frame) * (6.2831853f / 90.f)));
            bobbing.Advance(1.f / 60.f, under, true, true, 1.4f);
            if (frame > 60) { step = std::max(step, std::fabs(bobbing.ramp - previous)); top = std::max(top, bobbing.ramp); }
            previous = bobbing.ramp;
        }
        // A gate that switched the band on at zero would move the ramp by most of its range
        // in the single frame the bobbing eye crossed the plane; a smooth one cannot.
        Require(top < 1.f && step < .08f,
                "a swimmer bobbing at the plane breathes with the swell instead of toggling the whole band");
    }
    {
        // THE JUMP THE PUBLISHED POSE CANNOT SEE. The camera gate needs three agreeing
        // candidates, so a teleport's first frames publish the eye the capture HELD from the
        // place the player left - and the tracker's own camera test then compares the old
        // place with itself. The draws cannot be wrong that way: their camera is read from the
        // matrix each draw was transformed by, in the frame that is ending, so a place report
        // hundreds of yards from the last one IS the teleport.
        Tracker teleport;
        const float lakeA[3] = { 100.f, 100.f, -8.f };
        for (int frame = 0; frame < 60; ++frame)
        {
            teleport.BeginFrame();
            teleport.NoteDrawPlace(lakeA[0], lakeA[1], 100.0 + frame / 60.0);
            teleport.Observe(8.f);
            teleport.Advance(1.f / 60.f, lakeA, true);
        }
        Require(teleport.submerged && teleport.planeKnown && teleport.jumpSource == 0,
                "a swim with the draws reporting their own place is not a jump");
        // The landing frame: the frame still publishes the OLD eye (the held pose), and the
        // draws have already moved.
        const float lakeB[3] = { 3000.f, -2200.f, 14.f };
        teleport.BeginFrame();
        teleport.NoteDrawPlace(lakeB[0], lakeB[1], 101.0);
        teleport.Advance(1.f / 60.f, lakeA, true);
        Require(teleport.jumpSource == 2 && !teleport.submerged && teleport.ramp == 0.f
                    && !teleport.planeKnown && teleport.yards < 0.f,
                "a teleport the draws report drops the whole state on the landing frame, even while the frame still publishes the old zone's eye");
        // And the control: with no draw evidence, the same held pose compares the old place
        // with itself and the state is held - which is why the draws' place had to be asked.
        Tracker noEvidence;
        for (int frame = 0; frame < 60; ++frame)
        {
            noEvidence.BeginFrame();
            noEvidence.Observe(8.f);
            noEvidence.Advance(1.f / 60.f, lakeA, true);
        }
        noEvidence.BeginFrame();
        noEvidence.Advance(1.f / 60.f, lakeA, true);
        Require(noEvidence.submerged && noEvidence.ramp == 1.f,
                "without the draws' place a held pose is the old place compared with itself, and holds");
        // Silence is not a jump: a loading screen, or a diver who spent ten seconds looking at
        // the seabed, re-reports a place long after the last one and nothing is dropped.
        Tracker quiet;
        for (int frame = 0; frame < 60; ++frame)
        {
            quiet.BeginFrame();
            quiet.NoteDrawPlace(lakeA[0], lakeA[1], 200.0 + frame / 60.0);
            quiet.Observe(8.f);
            quiet.Advance(1.f / 60.f, lakeA, true);
        }
        quiet.BeginFrame();
        quiet.NoteDrawPlace(3000.f, -2200.f, 210.0);   // ten seconds later
        quiet.Advance(1.f / 60.f, lakeA, true);
        Require(quiet.jumpSource == 0 && quiet.submerged,
                "a place report after a long silence is not a jump - the camera test owns the loading-screen case");
        Require(Tracker::kDrawPlaceSeconds > .25 && Tracker::kDrawPlaceSeconds < 1.f,
                "the place evidence stays recent for less than a second, so a walk never reads as a teleport");
    }
    {
        // Nearest wins. A frame can hold several liquid draws - a harbour with a dozen tiles,
        // or a mirrored reflection draw that slipped past the determinant test - and the
        // interface the camera is at is the nearest one, which is also the one a mirror cannot
        // fake: a mirror sits twice as far from the plane as the camera does.
        Tracker tracker;
        tracker.BeginFrame();
        tracker.Observe(30.f);
        tracker.Observe(2.5f);
        tracker.Observe(11.f);
        Require(Near(tracker.raw, 2.5f, 1e-6) && Near(tracker.HeldYards(), 2.5f, 1e-6),
                "the frame keeps the plane nearest the camera, which is the interface it is at");
        tracker.BeginFrame();
        Require(Near(tracker.HeldYards(), tracker.yards, 1e-6) && !tracker.observed,
                "a draw that cannot answer falls back to the frame's settled answer, not to zero");
        const float nan = std::numeric_limits<float>::quiet_NaN();
        tracker.Observe(nan);
        Require(!tracker.observed, "a non-finite reading is dropped rather than held");
    }
    {
        // NEARNESS MAY SUSTAIN A SWIM; ONLY BEING UNDER THE SHEET MAY START ONE.
        // The live report: standing on dry land beside (and a little below) a
        // body of water and looking up, its plane reads over the camera forever.
        // Such a reading is valid and local - it is the same lake - but its
        // footprint puts the camera outside the measured sheet, so from dry
        // land it must not even observe.
        const float onShore[3] = { 0.f, 0.f, 0.f };
        PlaneReading raisedBeside;
        raisedBeside.valid = true; raisedBeside.local = true; raisedBeside.yards = 3.f;
        raisedBeside.horizontalYards = 25.f; raisedBeside.spanYards = 72.f;
        raisedBeside.footprintKnown = true; raisedBeside.footprintYards = 10.f;
        raisedBeside.containsCamera = false;
        Tracker dry;
        for (int frame = 0; frame < 30; ++frame)
        {
            dry.BeginFrame();
            dry.Observe(raisedBeside);
            dry.Advance(1.f / 60.f, onShore, true);
        }
        Require(!dry.observed && !dry.submerged && dry.ramp == 0.f && !dry.planeKnown,
                "looking up at nearby water from dry land never starts the medium");
        // The same sheet once it is actually over the camera starts it normally.
        PlaneReading overhead = raisedBeside;
        overhead.containsCamera = true; overhead.footprintYards = 0.f;
        Tracker entering;
        for (int frame = 0; frame < 60; ++frame)
        {
            entering.BeginFrame();
            entering.Observe(overhead);
            entering.Advance(1.f / 60.f, onShore, true);
        }
        Require(entering.submerged && entering.ramp == 1.f,
                "the same reading with the camera under the sheet enters as before");
        // From inside, a neighbouring sheet no longer sustains the swim
        // either: it does not even observe. What carries a diver whose own
        // chunk is culled is the remembered plane - for a seam's grace.
        // Keep saying "outside" for three quarters of a second, though, and
        // the memory retires: that is a player who climbed out onto ground
        // below the sheet's level and is still looking at it.
        const bool neighbourObserved = [&] {
            entering.BeginFrame();
            entering.Observe(raisedBeside);
            const bool seen = entering.observed;
            entering.Advance(1.f / 60.f, onShore, true);
            return seen;
        }();
        Require(!neighbourObserved && entering.submerged,
                "a neighbouring sheet does not observe; the remembered plane carries the first frames alone");
        const float walkedOff[3] = { 12.f, 0.f, 0.f };
        for (int frame = 0; frame < 120; ++frame)
        {
            entering.BeginFrame();
            entering.Observe(raisedBeside);
            entering.Advance(1.f / 60.f, walkedOff, true);
        }
        Require(!entering.submerged && entering.ramp == 0.f && !entering.planeKnown,
                "outside evidence sustained while walking away from the water retires the remembered plane");
        // WHO THE EXIT MAY NOT CATCH. A walker on a bank below the sheet's
        // level and a swimmer under open water receive the same tracker
        // testimony - local sheets, every footprint answering "not over
        // YOU" - and differ only in where the camera is looking: the
        // swimmer's own covering chunk is behind or above the view, because
        // the client draws liquid chunks against its frustum and he is
        // looking at the seabed. When the caller reports the covering
        // region out of the client's view, "no sheet over the camera" is a
        // fact about the view; the evidence must not count, or every swim
        // drains the medium eight yards after the surface last passed
        // overhead - the reported "underwater shader gets lost periodically".
        PlaneReading overheadDeep = overhead;
        overheadDeep.yards = 8.f;
        Tracker swimmer;
        const float diving[3] = { 0.f, 0.f, -8.f };
        for (int frame = 0; frame < 60; ++frame)
        {
            swimmer.BeginFrame();
            swimmer.Observe(overheadDeep);
            swimmer.Advance(1.f / 60.f, diving, true);
        }
        Require(swimmer.submerged && swimmer.ramp == 1.f && swimmer.planeKnown,
                "a diver at depth is submerged with the plane remembered");
        PlaneReading neighbourTile = raisedBeside;
        neighbourTile.yards = 8.f;
        for (int frame = 0; frame < 240; ++frame)
        {
            const float swimming[3] = { frame * 0.2f, 0.f, -8.f };
            swimmer.BeginFrame();
            swimmer.Observe(neighbourTile);
            swimmer.Advance(1.f / 60.f, swimming, true, false);
        }
        Require(swimmer.submerged && swimmer.ramp == 1.f && swimmer.planeKnown,
                "a swimmer at depth whose covering chunk is out of view holds the medium - fifty yards, four seconds, no surface in frame");
        // The same walk with the covering region IN view is still the
        // geometry's answer and still retires: the caller's flag, not the
        // travel, is what changed.
        Tracker bankWalker;
        for (int frame = 0; frame < 60; ++frame)
        {
            bankWalker.BeginFrame();
            bankWalker.Observe(overhead);
            bankWalker.Advance(1.f / 60.f, onShore, true);
        }
        for (int frame = 0; frame < 120; ++frame)
        {
            const float alongTheBank[3] = { frame * 0.1f, 0.f, 0.f };
            bankWalker.BeginFrame();
            bankWalker.Observe(raisedBeside);
            bankWalker.Advance(1.f / 60.f, alongTheBank, true, true);
        }
        Require(!bankWalker.submerged && bankWalker.ramp == 0.f && !bankWalker.planeKnown,
                "the same walk with the covering region in view still retires the remembered plane");
        // And the count cannot accrue one glance at a time: a frame that
        // does not count RESTARTS it, so alternating looks at the water and
        // away from it never reach the grace - while a sustained look does.
        Tracker glancing;
        for (int frame = 0; frame < 60; ++frame)
        {
            glancing.BeginFrame();
            glancing.Observe(overhead);
            glancing.Advance(1.f / 60.f, onShore, true);
        }
        for (int frame = 0; frame < 240; ++frame)
        {
            const float strolling[3] = { frame * 0.1f, 0.f, 0.f };
            const bool lookingAtTheWater = (frame / 20) % 2 == 0;
            glancing.BeginFrame();
            glancing.Observe(raisedBeside);
            glancing.Advance(1.f / 60.f, strolling, true, lookingAtTheWater);
        }
        Require(glancing.submerged && glancing.planeKnown,
                "outside evidence cannot accrue one glance at a time - looking away restarts the count");
        for (int frame = 0; frame < 60; ++frame)
        {
            const float keepsLooking[3] = { (240 + frame) * 0.1f, 0.f, 0.f };
            glancing.BeginFrame();
            glancing.Observe(raisedBeside);
            glancing.Advance(1.f / 60.f, keepsLooking, true, true);
        }
        Require(!glancing.submerged && !glancing.planeKnown,
                "a sustained look at where the sheet would be still exits");
        // THE RAMP IS MEASURED AGAINST THE VISIBLE SURFACE. The tracker's plane is the
        // liquid tile's MEAN plane, but geometry waves lift crests about a yard and a half
        // over it at the shipped sliders, so a camera a yard under the mean plane spends
        // every swell cycle inside the crossfade band and the medium thins to nothing
        // while he watches the seabed. The caller folds the waves' reach at the camera
        // into the ramp and the hysteresis; still water (band zero) must behave exactly
        // as before.
        {
            Tracker still;
            const float shallow[3] = { 0.f, 0.f, -0.5f };
            for (int frame = 0; frame < 60; ++frame)
            {
                still.BeginFrame();
                still.Observe(0.5f);
                still.Advance(1.f / 60.f, shallow, true);
            }
            // Smoothstep(.22, 1.5, .5): the shipped still-water ramp half a yard down.
            Require(still.ramp > 0.12f && still.ramp < 0.15f && still.submerged,
                    "still water keeps the shipped crossfade half a yard under the mean plane");
            Tracker swell;
            for (int frame = 0; frame < 60; ++frame)
            {
                swell.BeginFrame();
                swell.Observe(0.5f);
                swell.Advance(1.f / 60.f, shallow, true, true, 1.4f);
            }
            Require(swell.ramp > 0.85f && swell.submerged,
                    "half a yard under the mean plane in a swell holds the medium through the wave's reach");
            // A full swell cycle: the reach rises and falls with the phase; the ramp may
            // breathe with it but must never fall back into the crossfade floor while the
            // depth alone would hold it.
            Tracker cycle;
            float lowestRamp = 1.f;
            bool stayedSubmerged = true;
            for (int frame = 0; frame < 600; ++frame)
            {
                const float reach = 0.7f + 0.7f * std::sin(frame * (6.2831853f / 240.f));
                cycle.BeginFrame();
                cycle.Observe(0.8f);
                cycle.Advance(1.f / 60.f, shallow, true, true, reach);
                if (frame >= 30)
                {
                    lowestRamp = std::min(lowestRamp, cycle.ramp);
                    stayedSubmerged = stayedSubmerged && cycle.submerged;
                }
            }
            Require(lowestRamp >= 0.28f && stayedSubmerged && cycle.ramp >= 0.28f && cycle.submerged,
                    "ten seconds of swell leave the swimmer in the medium, the ramp breathing but never falling away");
            // Air above the crests: the reach must not hold a camera the water cannot.
            Tracker above;
            const float airborne[3] = { 0.f, 0.f, 2.f };
            for (int frame = 0; frame < 60; ++frame)
            {
                above.BeginFrame();
                above.Observe(-2.f);
                above.Advance(1.f / 60.f, airborne, true, true, 1.4f);
            }
            Require(!above.submerged && above.ramp == 0.f,
                    "the wave's reach cannot hold the medium on a camera above the crests");
            // A non-finite or negative band is no band at all.
            Tracker guarded;
            for (int frame = 0; frame < 60; ++frame)
            {
                guarded.BeginFrame();
                guarded.Observe(0.5f);
                guarded.Advance(1.f / 60.f, shallow, true, true, std::nanf(""));
            }
            Require(guarded.ramp > 0.12f && guarded.ramp < 0.15f,
                    "a NaN reach is still water");
            Tracker floored;
            for (int frame = 0; frame < 60; ++frame)
            {
                floored.BeginFrame();
                floored.Observe(0.5f);
                floored.Advance(1.f / 60.f, shallow, true, true, -3.f);
            }
            Require(floored.ramp > 0.12f && floored.ramp < 0.15f,
                    "a trough below the mean plane subtracts nothing from a held swimmer");
        }
        // MEMORY MAY NOT START WHAT THE GATE REFUSED. The Sholazar basin
        // sequence, and the round-14 gate's bypass: stand ABOVE the same
        // nearby sheet first (its "air" reading quite properly observes),
        // then walk downhill beside it until its plane is over the camera.
        // The direct reading is footprint-refused every frame - and an
        // earlier tracker started the medium anyway, because the air
        // reading had been remembered as a plane height and Advance
        // re-measured the descended camera against it, after which the
        // loose sustain kept it forever. Air testimony now creates no
        // memory, and memory itself may not start a dry camera.
        PlaneReading airBeside = raisedBeside;
        airBeside.yards = -2.f;
        Tracker downhill;
        const float aboveBank[3] = { 0.f, 0.f, 2.f };
        for (int frame = 0; frame < 30; ++frame)
        {
            downhill.BeginFrame();
            downhill.Observe(airBeside);
            downhill.Advance(1.f / 60.f, aboveBank, true);
        }
        const bool airRemembered = downhill.planeKnown;
        const float belowBank[3] = { 0.f, 0.f, -3.f };
        for (int frame = 0; frame < 120; ++frame)
        {
            downhill.BeginFrame();
            downhill.Observe(raisedBeside);
            downhill.Advance(1.f / 60.f, belowBank, true);
        }
        Require(!airRemembered && !downhill.submerged && downhill.ramp == 0.f
                    && !downhill.planeKnown,
                "walking downhill beside a lake never starts the medium from its remembered height");
        // The water stage spends the same rule on its own classification:
        // a beside-and-below-level sheet classifies by the frame's settled
        // answer, an overhead one by its own yards, air by its own.
        Tracker dryFrame;
        Require(Near(ClassifiedYards(raisedBeside, dryFrame), dryFrame.HeldYards(), 1e-6)
                    && Near(ClassifiedYards(overhead, dryFrame), 3.f, 1e-6)
                    && Near(ClassifiedYards(airBeside, dryFrame), -2.f, 1e-6),
                "a draw beside a dry camera cannot classify itself as seen from under water");
        // Without a footprint, only measured water at the camera itself may
        // start the medium; the 450-yard locality fallback names the lake,
        // and even ten yards from one measured point is a player standing
        // on the ground near it, not evidence of being in it.
        PlaneReading pointFar;
        pointFar.valid = true; pointFar.local = true; pointFar.yards = 3.f;
        pointFar.horizontalYards = 300.f;
        Tracker namedLake;
        namedLake.BeginFrame();
        namedLake.Observe(pointFar);
        const bool farObserved = namedLake.observed;
        PlaneReading pointMid = pointFar;
        pointMid.horizontalYards = 10.f;
        Tracker nearThePoint;
        nearThePoint.BeginFrame();
        nearThePoint.Observe(pointMid);
        const bool midObserved = nearThePoint.observed;
        PlaneReading pointNear = pointFar;
        pointNear.horizontalYards = 3.f;
        Tracker atThePoint;
        atThePoint.BeginFrame();
        atThePoint.Observe(pointNear);
        Require(!farObserved && !midObserved && atThePoint.observed,
                "a one-point reading starts the medium only with measured water at the camera");
        // Testimony of air is never gated: a nearby sheet below the camera may
        // always say the camera is above it, however far outside it falls.
        PlaneReading air = raisedBeside;
        air.yards = -2.f;
        Tracker above;
        above.BeginFrame();
        above.Observe(air);
        Require(above.observed && !above.submerged,
                "a sheet outside the footprint may always testify that the camera is in air");
    }
}

void TestCoveringPlaneInView()
{
    std::printf("\n-- the covering-region test --\n");
    // The view matrices below are written in viewRaw's v[0..3] storage: rows are
    // the world axes in view space, translation is -(row . camera). The projection
    // is a 45-degree vertical one: P11 = cot(22.5 degrees) = 2.414, square aspect.
    constexpr float kYScale = 2.414213562373095f;
    // A level camera eight yards under a plane at world z=0, facing world +y:
    // view x = world x, view y (screen up) = world z, view z (forward) = world y.
    {
        const float level[16] = {
            1.f, 0.f, 0.f, 0.f,
            0.f, 0.f, 1.f, 0.f,
            0.f, 1.f, 0.f, 0.f,
            0.f, 8.f, 0.f, 1.f };
        const float camera[3] = { 0.f, 0.f, -8.f };
        // Straight overhead is never "in view" - it is not in front of the eye -
        // but the plane ahead is: at twenty-five yards the sheet sits seventeen
        // degrees up, inside a 22.5-degree half-fov even from eight yards down.
        Require(CoveringPlaneInView(level, camera, 0.f, kYScale, kYScale),
                "a level camera under the plane has its covering region in view ahead");
    }
    // The swimmer's own view: forty-five degrees down at the seabed. Every
    // sample of the covering plane is sixty-plus degrees off the view axis,
    // which is exactly why the client culled the covering chunk - and why the
    // frame's "no sheet over the camera" must not count as water evidence.
    {
        const float down[16] = {
            1.f, 0.f, 0.f, 0.f,
            0.f, .70710678f, .70710678f, 0.f,
            0.f, .70710678f, -.70710678f, 0.f,
            0.f, 5.65685425f, -5.65685425f, 1.f };
        const float camera[3] = { 0.f, 0.f, -8.f };
        Require(!CoveringPlaneInView(down, camera, 0.f, kYScale, kYScale),
                "a swimmer looking down at the seabed cannot see the sheet over him");
        // And staring straight down has no horizontal forward at all.
        const float straightDown[16] = {
            1.f, 0.f, 0.f, 0.f,
            0.f, 1.f, 0.f, 0.f,
            0.f, 0.f, -1.f, 0.f,
            0.f, 0.f, -8.f, 1.f };
        Require(!CoveringPlaneInView(straightDown, camera, 0.f, kYScale, kYScale),
                "a camera with no horizontal forward cannot test the covering region");
    }
    // The bank walker: level camera ON the shore, the sheet three yards overhead
    // beginning ten yards out. The plane ahead is close to the view axis, which
    // is what "still looking at it" means - his outside evidence counts.
    {
        const float level[16] = {
            1.f, 0.f, 0.f, 0.f,
            0.f, 0.f, 1.f, 0.f,
            0.f, 1.f, 0.f, 0.f,
            0.f, 0.f, 0.f, 1.f };
        const float camera[3] = { 0.f, 0.f, 0.f };
        Require(CoveringPlaneInView(level, camera, 3.f, kYScale, kYScale),
                "a bank walker looking along the shore has the sheet's region in view");
    }
    // Degenerate inputs answer false - a frame that cannot test the frustum
    // hands the exit no evidence.
    {
        const float level[16] = {
            1.f, 0.f, 0.f, 0.f,
            0.f, 0.f, 1.f, 0.f,
            0.f, 1.f, 0.f, 0.f,
            0.f, 0.f, 0.f, 1.f };
        const float camera[3] = { 0.f, 0.f, 0.f };
        const float nanCamera[3] = { 0.f, std::nanf(""), 0.f };
        Require(!CoveringPlaneInView(level, camera, 3.f, 0.f, kYScale)
                    && !CoveringPlaneInView(level, camera, 3.f, kYScale, std::nanf(""))
                    && !CoveringPlaneInView(level, nanCamera, 3.f, kYScale, kYScale)
                    && !CoveringPlaneInView(nullptr, camera, 3.f, kYScale, kYScale),
                "an unusable pose, projection or camera answers not-in-view");
    }
}

void TestMedium()
{
    std::printf("\n-- the medium is the water's own --\n");
    const auto medium = ShippedMedium();
    {
        // bodyStyle.x is `.088*(100/ClarityPercent)` and waterStyle.y is
        // DepthAbsorptionPercent/100, and the pixel stage multiplies its column by their
        // product times absorbTint.rgb. Spelled once here, so the world pass cannot drift off
        // the surface it has to meet.
        const float rate = kExtinctionBase * (100.f / kClarity) * kAbsorptionPercent;
        Require(medium.valid && Near(medium.extinction.x, rate * kAbsorbRegister.x, 1e-7)
                    && Near(medium.extinction.y, rate * kAbsorbRegister.y, 1e-7)
                    && Near(medium.extinction.z, rate * kAbsorbRegister.z, 1e-7),
                "the medium's per-channel extinction is the water stage's own coefficient");
        Require(Near(medium.clarityYards, 1.0 / (rate * kAbsorbRegister.y), 1e-4)
                    && medium.clarityYards > 20.f && medium.clarityYards < 40.f,
                "the clarity the log reports is the green channel's first e-fold, near thirty yards");
        // Red dies first, which is why a deep pool reads teal rather than as a darker grey.
        Require(medium.extinction.x > medium.extinction.y && medium.extinction.y > medium.extinction.z,
                "red is extinguished first, then green, then blue");
        const auto atTen = Transmittance(medium, 10.f);
        const auto atForty = Transmittance(medium, 40.f);
        Require(atTen.x < atTen.y && atTen.y < atTen.z && atForty.x < .1f,
                "ten yards in the bed is still warm and forty yards in it is not");
        const auto capped = Transmittance(medium, 100000.f);
        Require(Near(capped.y, Transmittance(medium, kFarColumnYards).y, 1e-9),
                "the column is capped where the water stage caps its own, so a far plane and a deep floor agree");
    }
    {
        // DEPTH off means the water stage measures no column at all, and a medium that
        // extinguishes nothing is not a medium: the pass reports itself inactive rather than
        // drawing an identity frame over the client's own underwater fog.
        const auto off = MediumFor(kClarity, kAbsorptionPercent, kAbsorbRegister, kDeepRegister, kTintRegister, false);
        Require(!off.valid && off.extinction.x == 0.f,
                "with the water's own DEPTH off there is no medium to be inside");
        const auto murky = MediumFor(5.f, kAbsorptionPercent, kAbsorbRegister, kDeepRegister, kTintRegister, true);
        const auto lake = MediumFor(200.f, kAbsorptionPercent, kAbsorbRegister, kDeepRegister, kTintRegister, true);
        Require(murky.clarityYards < 2.f && lake.clarityYards > 50.f,
                "a zone's authored clarity is the diver's visibility: swamp soup and a mountain lake");
        const auto nonsense = MediumFor(std::numeric_limits<float>::quiet_NaN(), 4.f,
                                        Vec3{ -1.f, 9.f, std::numeric_limits<float>::infinity() },
                                        kDeepRegister, kTintRegister, true);
        Require(std::isfinite(nonsense.extinction.x) && std::isfinite(nonsense.extinction.y)
                    && std::isfinite(nonsense.extinction.z) && std::isfinite(nonsense.clarityYards),
                "a garbage tuning value cannot put a NaN in the frame");
    }
    {
        // A zone's authored colour has to be the same colour under the surface as in it, or
        // the line where a seabed meets the sea above it is a line.
        const auto azure = MediumFor(120.f, .5f, Vec3{ 1.4f, 1.f, .8f }, Vec3{ .1f, .3f, .5f },
                                     Vec3{ 1.f, 1.f, 1.f }, true);
        Require(Near(azure.deep.x, .1f, 1e-6) && Near(azure.tint.x, 1.f, 1e-6),
                "a neutral tint carries the authored deep colour through unchanged");
    }
    {
        // THE MEDIUM IS THE WATER'S COLOUR. WaterColour is a multiplier, so a
        // place's hue is the proportion between its channels: folding that hue
        // (luminance-normalised) into the deep colour makes the asymptote the
        // colour the surface already advertises. Red water converges red and
        // lights its own sky red; fel water converges green; and the same water
        // authored at two exposures keeps one hue.
        const Vec3 greyDeep{ .2f,.2f,.2f };
        const Vec3 neutralAbsorb{ 1.f, 1.f, 1.f };
        const auto blood = MediumFor(100.f, .5f, neutralAbsorb, greyDeep, Vec3{2.f,.5f,.5f}, true);
        Require(blood.deep.x > 3.f*blood.deep.y && blood.deep.x > 3.f*blood.deep.z,
                "red water's deep asymptote is red, not the authored grey it was handed");
        const auto bloodGlow = SkyGlowColour(blood);
        Require(bloodGlow.x > bloodGlow.y && bloodGlow.x > bloodGlow.z,
                "the sky through red water glows red, never the old fixed blue");
        const auto fel = MediumFor(100.f, .5f, neutralAbsorb, greyDeep, Vec3{.5f,2.f,.5f}, true);
        Require(fel.deep.y > 3.f*fel.deep.x && fel.deep.y > 3.f*fel.deep.z,
                "fel-green water converges green by the same fold");
        const auto dimBlood = MediumFor(100.f, .5f, neutralAbsorb, greyDeep, Vec3{1.f,.25f,.25f}, true);
        Require(Near(blood.deep.x, dimBlood.deep.x, 1e-6) && Near(blood.deep.y, dimBlood.deep.y, 1e-6),
                "the hue is a proportion: the same water at half exposure is the same colour");
        const auto black = MediumFor(100.f, .5f, neutralAbsorb, greyDeep, Vec3{0.f,0.f,0.f}, true);
        Require(Near(black.deep.x, .2f, 1e-6) && Near(black.deep.y, .2f, 1e-6)
                    && std::isfinite(black.deep.z),
                "a black tint has no hue to inherit and leaves the deep colour as authored");
        const auto laser = MediumFor(100.f, .5f, neutralAbsorb, greyDeep, Vec3{0.f,0.f,4.f}, true);
        Require(laser.deep.z <= .7f + 1e-6f && laser.deep.z > laser.deep.x,
                "a one-channel tint saturates at the clamp instead of becoming a laser");
    }
}

void TestShadePixel()
{
    std::printf("\n-- one pixel of the medium --\n");
    const auto medium = ShippedMedium();
    const auto field = ShippedField();
    Settings settings;
    settings.strength = 0.f;
    {
        // STRENGTH 0 is the frame the pass was handed, exactly: the whole medium interpolates
        // out through the transmittance's effect rather than through the coefficient, so a
        // player who turns it off gets the compositor's own pixels back and not a dimmer copy.
        const auto in = DiverPixel();
        const auto identity = ShadePixel(settings, medium, field, in);
        Require(Near(identity.colour.x, in.scene.x, 1e-6) && Near(identity.colour.y, in.scene.y, 1e-6)
                    && Near(identity.colour.z, in.scene.z, 1e-6) && Near(identity.absorption, 0.f, 1e-6)
                    && Near(identity.transmittance.x, 1.f, 1e-6) && identity.caustic == 0.f
                    && identity.inScatter.x == 0.f && identity.inScatter.y == 0.f,
                "STRENGTH 0 is an exact identity: no extinction, no net, no scatter, no asymptote");
        settings.strength = 1.f;
        settings.caustics = 0.f;   // the net is a property of the bed, not of the column
        PixelInput touching = DiverPixel();
        touching.distance = 0.f;
        touching.world = { 0.f, 0.f, 0.f };
        const auto zero = ShadePixel(settings, medium, field, touching);
        Require(Near(zero.colour.x, touching.scene.x, 1e-6) && Near(zero.colour.y, touching.scene.y, 1e-6)
                    && Near(zero.colour.z, touching.scene.z, 1e-6) && Near(zero.absorption, 0.f, 1e-6),
                "a surface touching the lens has no water in front of it, so it comes back unchanged");
        // Half a yard of water in front of a camera at the waterline is barely any water at
        // all, which is what makes the medium continuous with the client's own shading there
        // instead of arriving as a step. Measured at the waterline proper (submersion .25):
        // the same hand eight yards down sits in the silt and reads .022 absorbed, which is
        // the turbidity gradient's own number, not the waterline's.
        PixelInput arm = DiverPixel();
        arm.distance = .5f;
        arm.world = { .2f, .1f, -.4f };
        arm.submersion = .25f;
        const auto half = ShadePixel(settings, medium, field, arm);
        Require(std::fabs(half.colour.x - arm.scene.x) < .02 && half.absorption < .02,
                "half a yard of the shipped medium is a couple of percent, so the waterline is continuous");
    }
    {
        // The surface crossing, modelled here rather than left to the shader alone: the ramp is
        // folded into the strength exactly as the ps_3_0 stage folds it
        // (`strength=saturate(deepColour.w)*ramp`), so "above water nothing changes" is a
        // property of this function and not only a claim in a comment, and a camera rising
        // through the surface moves one medium into the other instead of cross-fading two
        // pictures of the world.
        settings.strength = 1.f;
        settings.caustics = .55f;
        const auto diver = DiverPixel();
        const auto under = ShadePixel(settings, medium, field, diver);
        PixelInput surfaced = diver;
        surfaced.ramp = 0.f;
        const auto air = ShadePixel(settings, medium, field, surfaced);
        Require(Near(air.colour.x, diver.scene.x, 1e-6) && Near(air.colour.y, diver.scene.y, 1e-6)
                    && Near(air.colour.z, diver.scene.z, 1e-6) && air.absorption == 0.f
                    && air.caustic == 0.f && air.shaft >= 0.f && air.inScatter.x == 0.f
                    && air.inScatter.y == 0.f && air.inScatter.z == 0.f
                    && air.transmittance.x == 1.f && air.transmittance.y == 1.f
                    && air.transmittance.z == 1.f,
                "ramp 0 is an exact identity in the core, which is what 'above water nothing changes' means");
        PixelInput halfway = diver;
        halfway.ramp = .5f;
        const auto middle = ShadePixel(settings, medium, field, halfway);
        // Exactly half the effect, term by term - and note what "half" means for each. The
        // transmittance is the lerp the strength scales, so half the strength sits halfway
        // between 1 and the full column rather than at the column's square root, which is what
        // scaling the coefficient itself would have given and would have made the crossing
        // non-linear in a way a swimmer can see. The in-scatter is a quarter, because both the
        // absorption it rides and the strength it is gated by are halved: the murk thins out
        // faster than the extinction does, which is the right shape for a surface you are
        // rising toward.
        Require(Near(middle.transmittance.y, 1.f + (under.transmittance.y - 1.f) * .5f, 1e-6)
                    && Near(middle.transmittance.x, 1.f + (under.transmittance.x - 1.f) * .5f, 1e-6)
                    && Near(middle.absorption, under.absorption * .5f, 1e-6)
                    && Near(middle.caustic, under.caustic * .5f, 1e-6)
                    && Near(middle.inScatter.y, under.inScatter.y * .25f, 1e-6),
                "halfway through the surface is exactly half the extinction and half the net");
        PixelInput clamped = diver;
        clamped.ramp = 2.f;
        const auto over = ShadePixel(settings, medium, field, clamped);
        PixelInput negative = diver;
        negative.ramp = -1.f;
        const auto below = ShadePixel(settings, medium, field, negative);
        Require(Near(over.colour.y, under.colour.y, 1e-6) && Near(below.colour.y, air.colour.y, 1e-6),
                "a ramp outside 0..1 is clamped, so a garbage tracker cannot over- or under-shade");
        float previous = -1.f;
        bool monotone = true;
        for (int step = 0; step <= 20; ++step)
        {
            PixelInput rising = diver;
            rising.ramp = float(step) / 20.f;
            const float absorbed = ShadePixel(settings, medium, field, rising).absorption;
            if (absorbed < previous - 1e-6f) monotone = false;
            previous = absorbed;
        }
        Require(monotone && previous > .99f * under.absorption,
                "the crossing is monotone, so a swimmer rising through the surface cannot flicker");
    }
    {
        // The mirror, numerically: the water pixel stage's own bottom path is
        //     transmittance=exp(-bodyStyle.x*waterStyle.y*column*absorbTint.rgb)
        //     inScatter=(deepTint*1.7+waterTint*.12)*absorption*(1-g*g)*.17
        //               /(phaseDen*sqrt(phaseDen))*saturate(sunElev*2.5+.2)*lightPos
        //     bottom=refracted*transmittance+inScatter+deepTint*absorption
        // with g=.3+.34*saturate(sunElev) and phaseDen=max(1+g*g+2*g*vDotL,1e-3). At STRENGTH 1
        // and with no beams this pass has to produce that same colour for that same column,
        // because the seabed seen through the water and the water seen through itself converge
        // on it at the same depth instead of meeting at a line.
        const auto in = DiverPixel();
        Settings noBeams;
        noBeams.caustics = 0.f;
        noBeams.shafts = 0.f;
        noBeams.surfaceGlow = 0.f;
        // The exact agreement is pinned with TURBIDITY at 0, because dirt is the one place the
        // pass deliberately knows more than the surface's flat bottom path: silt hangs at the
        // shaded point, which the surface shading never models. The two paths still meet
        // exactly on the medium the zone authored, and the dirt-scaled column is re-derived
        // from the primitives below, so the divergence is a number, not a drift.
        noBeams.turbidity = 0.f;
        const auto out = ShadePixel(noBeams, medium, field, in);
        const float rate = kExtinctionBase * (100.f / kClarity) * kAbsorptionPercent;
        const float column = in.distance;
        const Vec3 waterTransmittance{ std::exp(-rate * kAbsorbRegister.x * column),
                                       std::exp(-rate * kAbsorbRegister.y * column),
                                       std::exp(-rate * kAbsorbRegister.z * column) };
        const float absorption = 1.f - waterTransmittance.y;
        const float g = .3f + .34f * Saturate(in.sunElevation);
        const float vDotL = -Dot(in.ray, in.light);
        const float phaseDen = std::max(1.f + g * g + 2.f * g * vDotL, 1e-3f);
        const Vec3 scatter{ kDeepRegister.x * 1.7f + kTintRegister.x * .12f,
                            kDeepRegister.y * 1.7f + kTintRegister.y * .12f,
                            kDeepRegister.z * 1.7f + kTintRegister.z * .12f };
        const float lobe = (1.f - g * g) * .17f / (phaseDen * std::sqrt(phaseDen))
            * Saturate(in.sunElevation * 2.5f + .2f);
        const Vec3 inScatter{ scatter.x * absorption * lobe * in.lightColour.x,
                              scatter.y * absorption * lobe * in.lightColour.y,
                              scatter.z * absorption * lobe * in.lightColour.z };
        const Vec3 bottom{ in.scene.x * waterTransmittance.x + inScatter.x + kDeepRegister.x * absorption,
                           in.scene.y * waterTransmittance.y + inScatter.y + kDeepRegister.y * absorption,
                           in.scene.z * waterTransmittance.z + inScatter.z + kDeepRegister.z * absorption };
        Require(Near(out.colour.x, bottom.x, 1e-6) && Near(out.colour.y, bottom.y, 1e-6)
                    && Near(out.colour.z, bottom.z, 1e-6),
                "at TURBIDITY 0 the pass shades a column exactly as the water stage's bottom path does");
        // And with the dirt on, the same column scaled by the place's own turbidity: the silt
        // at this point (14 yards down) plus the hundred-yard patch it sits in, both
        // recomputed here from the primitives the shader spells literally.
        noBeams.turbidity = 1.f;
        const auto dirty = ShadePixel(noBeams, medium, field, in);
        const float underHere = in.submersion - in.world.z;
        float dirtCell[2];
        LatticeCell(field, in.world * (1.f / kTurbidityPatchCells), dirtCell);
        const float patch = renderer::water::NoiseGradient(dirtCell[0] + kDirtSampleOffset[0],
                                                           dirtCell[1] + kDirtSampleOffset[1]).value;
        const float dirt = (.65f + .7f * patch)
            * (1.f + .9f * Saturate(underHere / kSiltYards));   // turbidity 1: the silt itself
        const Vec3 dirtT{ std::exp(-rate * kAbsorbRegister.x * column * dirt),
                          std::exp(-rate * kAbsorbRegister.y * column * dirt),
                          std::exp(-rate * kAbsorbRegister.z * column * dirt) };
        const float dirtAbsorption = 1.f - dirtT.y;
        const Vec3 dirtBottom{ in.scene.x * dirtT.x + scatter.x * dirtAbsorption * lobe * in.lightColour.x
                                   + kDeepRegister.x * dirtAbsorption,
                               in.scene.y * dirtT.y + scatter.y * dirtAbsorption * lobe * in.lightColour.y
                                   + kDeepRegister.y * dirtAbsorption,
                               in.scene.z * dirtT.z + scatter.z * dirtAbsorption * lobe * in.lightColour.z
                                   + kDeepRegister.z * dirtAbsorption };
        Require(Near(dirty.colour.x, dirtBottom.x, 1e-6) && Near(dirty.colour.y, dirtBottom.y, 1e-6)
                    && Near(dirty.colour.z, dirtBottom.z, 1e-6) && dirt > 1.3f,
                "at TURBIDITY 1 the column is the same path scaled by the place's dirt (>1.3x this deep)");
    }
    {
        // And the shape of it: further is darker and bluer, a bed lit from behind the camera
        // scatters less than one looked at into the light, and looking up picks up the sky.
        const auto in = DiverPixel();
        PixelInput closeUp = in; closeUp.distance = 3.f; closeUp.world = in.world * (3.f / 18.f);
        PixelInput farAway = in; farAway.distance = 60.f; farAway.world = in.world * (60.f / 18.f);
        const auto close = ShadePixel(settings, medium, field, closeUp);
        const auto distant = ShadePixel(settings, medium, field, farAway);
        Require(close.colour.x > distant.colour.x * 2.f && distant.colour.z > distant.colour.x
                    && distant.absorption > .85f && close.absorption < .2f,
                "sixty yards of the shipped medium leaves a blue-green nothing, and three yards leaves the sand");
        // Looking into the light scatters toward the camera; looking away from it does not.
        // `ray` points from the camera to the point, so the lobe's own dot(v,l) is its
        // negation - the same one dot the water stage computes once and uses three times.
        PixelInput towardSource = in;
        towardSource.ray = Normalize(Vec3{ -.2f, -.3f, -.9f });
        const auto intoTheLight = ShadePixel(settings, medium, field, in);
        const auto awayFromTheLight = ShadePixel(settings, medium, field, towardSource);
        Require(intoTheLight.inScatter.y > awayFromTheLight.inScatter.y * 1.5f
                    && intoTheLight.inScatter.y > 0.f,
                "the medium forwards its scatter toward the source, on the water stage's own lobe");
        // Looking up picks up the whole sky over the surface, which is what keeps a pixel the
        // client left as sky - no liquid tile drawn there - reading as water overhead.
        PixelInput upLook = in;
        upLook.ray = Normalize(Vec3{ 0.f, 0.f, 1.f });
        PixelInput downLook = in;
        downLook.ray = Normalize(Vec3{ 0.f, 0.f, -1.f });
        Settings glowOnly;
        glowOnly.caustics = 0.f;
        glowOnly.shafts = 0.f;
        glowOnly.surfaceGlow = 1.f;
        // This block pins the clamp and the glow against the medium alone; the dirt that
        // belongs to a place is pinned on its own in TestShaftsAndWobble.
        glowOnly.turbidity = 0.f;
        const auto up = ShadePixel(glowOnly, medium, field, upLook);
        const auto down = ShadePixel(glowOnly, medium, field, downLook);
        Settings noGlow = glowOnly;
        noGlow.surfaceGlow = 0.f;
        const auto upNoGlow = ShadePixel(noGlow, medium, field, upLook);
        const auto downNoGlow = ShadePixel(noGlow, medium, field, downLook);
        Require(up.inScatter.y > upNoGlow.inScatter.y * 1.05f
                    && Near(down.inScatter.y, downNoGlow.inScatter.y, 1e-7)
                    && up.colour.y > upNoGlow.colour.y,
                "the sky's downwelling glow is on up-looking rays only");
        // A pixel the client left as sky - no liquid tile drawn there, so no water shading at
        // all - is the one this term exists for. Its column is not the far cap: it ends at the
        // plane, because the segment past the surface is air. So an opening overhead reads as
        // sky seen through exactly the water over the diver's head - bright and tinted from
        // just under the surface, murky from down deep, and in between the glow is what keeps
        // it reading as water overhead rather than as a hole in the world.
        PixelInput openSky = upLook;
        openSky.distance = kFarColumnYards;
        openSky.scene = { .55f, .70f, .90f };
        openSky.normal = { 0.f, 0.f, 0.f };
        const auto fromEight = ShadePixel(glowOnly, medium, field, openSky);
        Require(Near(fromEight.columnYards, openSky.submersion, 1e-4) && fromEight.absorption < .5f
                    && fromEight.colour.y > .1f && fromEight.colour.y > fromEight.colour.x,
                "an opening eight yards up is sky through eight yards of water, not through the far cap");
        PixelInput deepSky = openSky;
        deepSky.submersion = 60.f;
        const auto fromSixty = ShadePixel(glowOnly, medium, field, deepSky);
        Require(Near(fromSixty.columnYards, 60.f, 1e-4) && fromSixty.absorption > .85f
                    && fromSixty.transmittance.y < fromEight.transmittance.y * .2f,
                "the same opening from sixty yards down lets almost no sky through, because there is more water over it");
        // What a diver sees looking up from down deep is therefore the water's own light, not
        // the sky - which is the honest answer and the one the glow term exists to give, since
        // without it that pixel would be the flat deep colour and read as a hole in the world.
        PixelInput shallowNoSky = openSky;
        shallowNoSky.scene = { 0.f, 0.f, 0.f };
        PixelInput deepNoSky = deepSky;
        deepNoSky.scene = { 0.f, 0.f, 0.f };
        const auto shallowWater = ShadePixel(glowOnly, medium, field, shallowNoSky);
        const auto deepWater = ShadePixel(glowOnly, medium, field, deepNoSky);
        // The sky's own share of the pixel, which is the number that actually moves: a tenth
        // of the frame from sixty yards down against half of it from eight.
        Require((fromSixty.colour.y - deepWater.colour.y) < .1f
                    && (fromEight.colour.y - shallowWater.colour.y) > .3f,
                "from deep the opening is the water's own glow, and from just under it the sky still gets through");
        // What the clamp must not touch: a ray that never meets the plane on the way to its
        // point, which is every downward and every level ray, and therefore every seabed and
        // every far shore a diver looks at.
        PixelInput level = openSky;
        level.ray = Normalize(Vec3{ 1.f, 0.f, 0.f });
        Require(Near(ShadePixel(glowOnly, medium, field, level).columnYards, kFarColumnYards, 1e-3),
                "a level ray runs to the far cap, because it never meets the plane");
        const auto bed = ShadePixel(glowOnly, medium, field, DiverPixel());
        Require(Near(bed.columnYards, 18.f, 1e-4) && bed.absorption > .3f,
                "a downward ray keeps the full distance to the bed it is looking at");
        // And the case the clamp exists for beyond sky: something the client drew above the
        // waterline with no liquid tile over it - a sail, a cliff face, a pier's upper works -
        // at a forty-five degree upward ray. The water ends where the ray leaves it.
        PixelInput sail = openSky;
        sail.distance = 40.f;
        sail.ray = Normalize(Vec3{ 1.f, 0.f, 1.f });
        const auto aboveTheWaterline = ShadePixel(glowOnly, medium, field, sail);
        // The forty-five degree ray leaves the water at the top edge of Snell's window
        // (rayUp .707 against the .6013-.7213 band), so the sail arrives mostly but not
        // wholly: the effective column is the window's own lerp between the far cap and
        // the clamped segment, recomputed here from the constant.
        const float sailWindow = Smoothstep(kSnellCos - .06f, kSnellCos + .06f, sail.ray.z);
        const float sailYards = Lerp(kFarColumnYards, 8.f / sail.ray.z, sailWindow);
        Require(Near(aboveTheWaterline.columnYards, 8.f / sail.ray.z, 1e-3)
                    && aboveTheWaterline.columnYards < 12.f
                    && Near(aboveTheWaterline.absorption,
                            1.f - std::exp(-medium.extinction.y * sailYards), 2e-3)
                    && aboveTheWaterline.absorption < .6f,
                "a sail above the waterline arrives through the top edge of Snell's window");
        // And the horizon itself: a ray ten degrees up from two yards down meets the plane
        // eleven yards out, far outside the critical cone, so nothing beyond it may arrive -
        // the pixel has to be the medium's own light whatever the client drew on the far
        // shore. Bright land and black land have to come out the same, which is the whole
        // statement "far land is not visible from under water" in one comparison.
        PixelInput horizon = openSky;
        horizon.submersion = 2.f;
        horizon.distance = kFarColumnYards;
        horizon.ray = Normalize(Vec3{ 1.f, 0.f, .18f });
        horizon.world = horizon.ray * horizon.distance;
        horizon.scene = { .95f, .95f, .95f };
        const auto horizonBright = ShadePixel(glowOnly, medium, field, horizon);
        horizon.scene = { 0.f, 0.f, 0.f };
        const auto horizonBlack = ShadePixel(glowOnly, medium, field, horizon);
        Require(horizonBright.absorption > .99f
                    && Near(horizonBright.colour.y, horizonBlack.colour.y, .005)
                    && Near(horizonBright.colour.x, horizonBlack.colour.x, .005),
                "land at the horizon does not arrive from under water: bright and black shores shade the same");
        // Inside the window the same shore, nearly overhead, is the diver's brightest view
        // and stays one: two yards of water over it, and the scene gets through.
        horizon.ray = Normalize(Vec3{ .2f, 0.f, 1.f });
        horizon.world = horizon.ray * horizon.distance;
        horizon.scene = { .95f, .95f, .95f };
        const auto overheadBright = ShadePixel(glowOnly, medium, field, horizon);
        horizon.scene = { 0.f, 0.f, 0.f };
        const auto overheadBlack = ShadePixel(glowOnly, medium, field, horizon);
        Require(overheadBright.colour.y - overheadBlack.colour.y > .5f,
                "inside Snell's window the world overhead still arrives through two yards of water");
        // In DIRTY water that picture must wash out even though the glow survives:
        // scattering scrambles the sky's IMAGE faster than it removes its light, so an
        // exiting ray pays ExitWash on the water overhead while a bed pixel at the
        // same turbidity pays only its dirt. At turbidity 0 the wash is exactly 1 (the
        // glass pane the bottom path assumes).
        Settings turbidSky = glowOnly;
        turbidSky.turbidity = 3.f;
        horizon.submersion = 60.f;          // deep enough that the wash has a column to show
        horizon.distance = kFarColumnYards;
        horizon.world = horizon.ray * horizon.distance;
        horizon.scene = { .95f, .95f, .95f };
        const auto washedBright = ShadePixel(turbidSky, medium, field, horizon);
        const auto cleanAt8Bright = ShadePixel(glowOnly, medium, field, horizon);
        horizon.scene = { 0.f, 0.f, 0.f };
        const auto washedBlack = ShadePixel(turbidSky, medium, field, horizon);
        const auto cleanAt8Black = ShadePixel(glowOnly, medium, field, horizon);
        Require(washedBright.colour.y - washedBlack.colour.y
                    < .45f * (cleanAt8Bright.colour.y - cleanAt8Black.colour.y),
                "through dirty water the sky's picture washes out toward the glow");
        Settings cleanSky = glowOnly;   // turbidity 0, as glowOnly was built
        horizon.submersion = 2.f;
        horizon.world = horizon.ray * horizon.distance;
        horizon.scene = { .95f, .95f, .95f };
        const auto cleanBright = ShadePixel(cleanSky, medium, field, horizon);
        Require(Near(cleanBright.colour.y, overheadBright.colour.y, 1e-6),
                "and at turbidity 0 the window is the plain column-marched pane it always was");
        // The wash itself, as numbers: the near-surface layer is equivalent yards
        // folded into a multiplier on the overhead column, so the SAME turbidity
        // charges a shallow swimmer more per yard overhead than a deep one, a
        // non-exiting ray pays nothing, and the liquid's own interface column
        // (InterfaceYards, the water stage's mirror of this statement) is the
        // washed segment inside the window and the far column outside it.
        Require(Near(ExitWash(0.f, 2.f, true), 1.f, 1e-6)
                    && Near(ExitWash(3.f, 2.f, false), 1.f, 1e-6)
                    && Near(ExitWash(2.f, 2.f, true), 7.f, 1e-5)
                    && ExitWash(2.f, 2.f, true) > ExitWash(2.f, 60.f, true),
                "the exit wash charges the near-surface layer per yard overhead, and only on exit");
        Require(Near(InterfaceYards(2.f, 1.f, 2.f, 2.f), 14.f, 1e-4)
                    && Near(InterfaceYards(2.f, 0.f, 2.f, 2.f), kFarColumnYards, 1e-4)
                    && Near(InterfaceYards(300.f, 1.f, 0.f, 2.f), kFarColumnYards, 1e-4),
                "the liquid's own column is the washed segment inside Snell's window and the far column outside it");
        // And the shallow case the round-13 form could not tell apart: two yards
        // down in dirty water, the overhead picture is already mostly glow.
        horizon.scene = { .95f, .95f, .95f };
        const auto shallowDirtyBright = ShadePixel(turbidSky, medium, field, horizon);
        horizon.scene = { 0.f, 0.f, 0.f };
        const auto shallowDirtyBlack = ShadePixel(turbidSky, medium, field, horizon);
        Require(shallowDirtyBright.colour.y - shallowDirtyBlack.colour.y
                    < .75f * (overheadBright.colour.y - overheadBlack.colour.y),
                "two yards down in dirty water the sky's picture is already washing out");
        PixelInput night = upLook;
        night.sunElevation = -.4f;
        Require(ShadePixel(glowOnly, medium, field, night).inScatter.y == 0.f,
                "a source below the horizon lights nothing under the surface either");
    }
}


void TestWorldAnchoring()
{
    std::printf("\n-- the net and the beams belong to the world --\n");
    // The pass shades a bed point from its camera-relative offset, and the field's
    // phases carry an anchor camera's own place in the lattice: offset plus phase is
    // an absolute world cell ONLY when the phases were built for that same camera.
    // A field carried from another frame's camera - which is what a per-draw or
    // previous-Present publication was - slides every anchored pattern by exactly
    // the cameras' difference: the caustics moved with the camera. This pins the
    // identity the fix rests on, with the real field builder and the core shading
    // functions, for the net and the shaft march's warp. (The sediment patches stay
    // camera-relative for want of shader budget - kTurbidityPatchCells says why.)
    const auto fieldFor = [](float cameraX, float cameraY) {
        const auto wave = renderer::water::BuildWaveField(
            37.5, 1.f, 0.f, 100.f, cameraX, cameraY, true, true);
        Field field;
        for (int i = 0; i < 4; ++i) field.lattice[i] = wave.lattice[i];
        field.chopPhase[0] = wave.ripplePhase[0];
        field.chopPhase[1] = wave.ripplePhase[1];
        field.finePhase[0] = wave.ripplePhase[0] * 2.f;
        field.finePhase[1] = wave.ripplePhase[1] * 2.f;
        field.warpPhase[0] = wave.warpPhase[0];
        field.warpPhase[1] = wave.warpPhase[1];
        field.seconds = 37.5f;
        return field;
    };
    const float cameraA[2] = { 1234.f, -987.f };
    const float cameraB[2] = { 1201.f, -940.f };   // ~57 yards away, a short swim
    const Field fieldA = fieldFor(cameraA[0], cameraA[1]);
    const Field fieldB = fieldFor(cameraB[0], cameraB[1]);
    bool netStill = true, warpStill = true, staleMoves = false;
    for (int i = 0; i < 24; ++i)
    {
        // Bed points spread over a few hundred yards around both cameras.
        const Vec3 bed{ 1100.f + 17.f * float(i), -1100.f + 23.f * float(i), -8.f };
        const Vec3 relA{ bed.x - cameraA[0], bed.y - cameraA[1], bed.z };
        const Vec3 relB{ bed.x - cameraB[0], bed.y - cameraB[1], bed.z };
        const float netA = CausticNet(fieldA, relA);
        netStill = netStill && Near(netA, CausticNet(fieldB, relB), 2e-3);
        warpStill = warpStill && Near(BroadWarp(fieldA, relA), BroadWarp(fieldB, relB), 2e-3);
        if (!Near(netA, CausticNet(fieldA, relB), .05f)) staleMoves = true;
    }
    Require(netStill, "the caustic net at a bed point is the same net from any camera the field was built for");
    Require(warpStill, "the shaft warp at a bed point is fixed to the world the same way");
    Require(staleMoves,
            "and a stale field really would move it - which is why the composite builds its own");
}

void TestCaustics()
{
    std::printf("\n-- the net on the bed --\n");
    const auto field = ShippedField();
    {
        // Re-measured here rather than trusted from a comment: the sharpened net's mean is what
        // the normalization divides by, and a normalization that is not the reciprocal of the
        // measured mean brightens every seabed in the game by its own average.
        std::mt19937 rng(4242u);
        std::uniform_real_distribution<float> offset(-600.f, 600.f);
        double sum = 0.0, peak = 0.0, pinned = 0.0, bright = 0.0;
        const int samples = 4000000;
        for (int index = 0; index < samples; ++index)
        {
            const Vec3 world{ offset(rng), offset(rng), -4.f };
            const float net = CausticNet(field, world);
            sum += net;
            peak = std::max<double>(peak, net);
            if (net >= 1.f) pinned += 1.0;
            if (net > .95f) bright += 1.0;
        }
        const double mean = sum / samples;
        const double normalization = 1.0 / mean;
        Require(Near(normalization, kCausticNormalization, kCausticNormalization * .004),
                "the caustic normalization is the reciprocal of the net's measured mean, to four in a thousand");
        // A plateau is a cell pinned at the top of the range, which is what printed a lattice
        // onto the foam once: the eye reads a flat bright square as a grid, not as light. The
        // net's peak is a point, not a region - nothing saturates - and the bright tail is the
        // few tenths of a percent of the bed a real caustic net puts its crossings on.
        Require(pinned == 0.0 && bright / samples < .01,
                "no sampled cell sits on a plateau, which is the failure mode that printed a lattice onto the foam");
        Require(peak < 1.f && peak > .9f && Near(peak / mean, 5.1034, .35),
                "the sharpened net carries a real caustic's contrast, peak over mean about five");
        // Mean-preserving at every amount, which is the whole point of the normalization: the
        // modulation the shader lerps to has to average one, or the bed gets brighter for
        // having a pattern on it.
        double modulated = 0.0;
        const int modulationSamples = samples / 4;
        for (int index = 0; index < modulationSamples; ++index)
        {
            const Vec3 world{ offset(rng), offset(rng), -4.f };
            modulated += Lerp(1.0, double(CausticNet(field, world)) * kCausticNormalization, .55);
        }
        Require(Near(modulated / double(modulationSamples), 1.0, .004),
                "the caustic modulation averages exactly one, so caustics move light around and do not create it");
    }
    {
        // The net has to sit on the surface's own chop: same lattice, same hash - and both
        // octaves read the absolute cell, the fine one at exactly twice the rate, so neither
        // octave can slide against the bed as the camera moves (anchoring is measured
        // directly in TestWorldAnchoring above). A caustic pattern that is not nailed to
        // the water's own lattice is a decal.
        const Vec3 world{ 37.25f, -18.5f, -4.f };
        float cell[2];
        LatticeCell(field, world, cell);
        const float a = NoiseValue64(cell[0] + field.chopPhase[0],
                                     cell[1] + field.chopPhase[1]);
        const float b = NoiseValue64(cell[0] * 2.f + field.finePhase[0],
                                     cell[1] * 2.f + field.finePhase[1]);
        const float ridgeA = 1.f - std::fabs(2.f * a - 1.f);
        const float ridgeB = 1.f - std::fabs(2.f * b - 1.f);
        const float expected = std::pow(Saturate(ridgeA * ridgeB), kCausticSharpen);
        Require(Near(CausticNet(field, world), expected, 1e-7),
                "the net is the surface's own chop crossed with its own second harmonic, as ridges");
        // Camera-relative on purpose: a continent-scale coordinate multiplied by a lattice row
        // runs out of float32 fraction bits and the noise collapses onto its lattice, which is
        // the round-1 bug the water field wraps its phase for.
        const Vec3 farWorld{ 3000000.f, -2000000.f, -4.f };
        float farCell[2];
        LatticeCell(field, farWorld, farCell);
        const float farNet = CausticNet(field, farWorld);
        const float wrapped = renderer::water::NoiseGradient(farCell[0] + field.chopPhase[0],
                                                            farCell[1] + field.chopPhase[1]).value;
        Require(std::isfinite(farNet) && std::isfinite(wrapped) && farNet >= 0.f && farNet <= 1.f,
                "the net stays finite and in range three million yards from the origin");
        bool varied = false;
        float previous = farNet;
        for (int step = 1; step <= 32; ++step)
        {
            const Vec3 moved{ farWorld.x + float(step) * .37f, farWorld.y, farWorld.z };
            const float value = CausticNet(field, moved);
            varied = varied || std::fabs(value - previous) > 1e-4f;
            previous = value;
        }
        Require(varied, "the net still varies at continent scale, so the hash has not collapsed onto its lattice");
    }
    {
        // Caustics are a shallow-water phenomenon: they need water over them to focus through
        // and they die as that water deepens and blurs them out. And they land on a bed, not on
        // a wall.
        const auto medium = ShippedMedium();
        Settings settings;
        settings.caustics = 1.f;
        settings.shafts = 0.f;
        settings.surfaceGlow = 0.f;
        auto bed = DiverPixel();
        bed.normal = { 0.f, 0.f, 1.f };
        bed.world = { 9.f, 4.f, -2.f };            // four yards under the plane
        bed.submersion = 2.f;
        auto wall = bed;
        wall.normal = { 1.f, 0.f, 0.f };
        // The same bed at a series of depths: the net needs a little water over it to focus
        // through, is strongest in the shallows, and is gone by the reach the constant names.
        auto at = [&](float yardsUnder) {
            auto pixel = bed;
            pixel.world = { 9.f, 4.f, bed.submersion - yardsUnder };
            return ShadePixel(settings, medium, field, pixel).caustic;
        };
        const auto onBed = ShadePixel(settings, medium, field, bed);
        const auto onWall = ShadePixel(settings, medium, field, wall);
        Require(onBed.caustic > .2f && onWall.caustic == 0.f,
                "the net lands on an upward-facing bed and not on a wall");
        Require(at(.05f) < at(.2f) && at(.2f) < at(.5f) && at(.5f) < at(1.f) && at(1.f) > at(8.f)
                    && at(8.f) > at(20.f) && at(20.f) > at(27.f) && at(27.f) > at(kCausticReachYards)
                    && at(kCausticReachYards) == 0.f && at(60.f) == 0.f,
                "the net needs water over it to focus through, is strongest in the shallows and is gone by its reach");
        auto night = bed;
        night.sunElevation = -.5f;
        Require(ShadePixel(settings, medium, field, night).caustic == 0.f,
                "a bed with no light on it has no net on it");
    }
    {
        // The one lattice the whole anchoring scheme leans on has to be CONTINUOUS at its
        // own wrap. The shared water noise wraps the cell index into [0,64) but hashes
        // cell 63's far corner as 64 while the wrapped cell hashes it as 0, so the field
        // jumps every 64 cells in each axis - on the surface a bent normal hides it, but
        // under this sharpened gain it printed as ruled lines across the bed (round 13:
        // "one big 2x2 grid, crossing in the middle"). The net's noise wraps its corners.
        // Prove continuity rather than spell it: straddle the boundary finely and compare
        // the largest step there with the largest step over an interior stretch.
        const auto maxStep = [](float from, float to) {
            float worst = 0.f, previous = NoiseValue64(from, 13.73f);
            for (float x = from + .002f; x <= to; x += .002f)
            {
                const float value = NoiseValue64(x, 13.73f);
                worst = std::max(worst, std::fabs(value - previous));
                previous = value;
            }
            return worst;
        };
        const float atBoundary = maxStep(63.6f, 64.4f);
        const float interior = maxStep(10.6f, 11.4f);
        Require(atBoundary < 2.5f * interior && atBoundary < .05f,
                "the net's noise crosses its 64-cell wrap without a seam");
    }
    {
        // And the dirt's lattice vertex must not sit under the swimmer: shadedWorld is
        // camera-relative, so an unoffset patch sample keeps the four-cell vertex at the
        // point under the camera at all times, printing a pinned cross of wedge sectors
        // into every downward view. The offset is non-integer (checked), which puts the
        // vertex tens of yards away; the proof here is that the patch seen over a few
        // yards around the camera's own point varies like a gradient, not like a vertex.
        const float fracX = kDirtSampleOffset[0] - std::floor(kDirtSampleOffset[0]);
        const float fracY = kDirtSampleOffset[1] - std::floor(kDirtSampleOffset[1]);
        Require(fracX > .15f && fracX < .85f && fracY > .15f && fracY < .85f,
                "the dirt sample sits mid-cell, not on its own lattice vertex");
        Settings dirty;
        dirty.turbidity = 1.f; dirty.caustics = 0.f; dirty.shafts = 0.f; dirty.surfaceGlow = 0.f;
        const auto medium = ShippedMedium();
        float lowest = 1.f, highest = 0.f;
        auto atPoint = [&](float x, float y) {
            auto pixel = DiverPixel();
            pixel.world = { x, y, -6.f };
            pixel.distance = 20.f;
            return ShadePixel(dirty, medium, field, pixel).transmittance.y;
        };
        for (int k = 0; k < 16; ++k)
        {
            const float a = float(k) * .3927f;
            const float t = atPoint(std::cos(a) * 3.f, std::sin(a) * 3.f);
            lowest = std::min(lowest, t); highest = std::max(highest, t);
        }
        Require(highest - lowest < .01f,
                "the murk around the camera's own point is a slow gradient, not a pinned cross");
    }
}

void TestShaftsAndWobble()
{
    std::printf("\n-- beams through the surface, and the eye's own wobble --\n");
    const auto field = ShippedField();
    {
        // A beam is a column under a bright patch of surface: each sample walks the ray back up
        // to the plane and reads the surface's own broad warp where the light entered. Measured
        // against the octave's own spread, one sample of (value-.5)*2 has a standard deviation
        // of .429 and three along a ray about .25, so the shipped gain puts a beam's peak near
        // one and its duty cycle near a fifth of the frame - structure, not noise.
        std::mt19937 rng(90210u);
        std::uniform_real_distribution<float> offset(-300.f, 300.f);
        const Vec3 lightWorld = Normalize(Vec3{ .2f, .3f, .9f });
        const int samples = 200000;
        double sum = 0.0, squares = 0.0, peak = 0.0, lit = 0.0;
        for (int index = 0; index < samples; ++index)
        {
            const Vec3 world{ offset(rng), offset(rng), -6.f };
            const float density = ShaftDensity(field, world, lightWorld, 6.f);
            sum += density;
            squares += double(density) * double(density);
            peak = std::max<double>(peak, density);
            if (density > .5) lit += 1.0;
        }
        const double mean = sum / samples;
        const double deviation = std::sqrt(std::max(squares / samples - mean * mean, 0.0));
        Require(peak > .9f && peak <= 1.f, "the beams reach full brightness somewhere in the frame");
        Require(mean < .35 && mean > .05 && lit / samples < .35 && lit / samples > .05,
                "the beams cover a fraction of the frame, which is what makes them beams");
        Require(deviation > .1, "the beam field has real contrast, not a wash");
        // A low sun makes long, leaning columns and a high one short ones; both are the same
        // march, and neither is allowed to produce light arriving from under the bed.
        const Vec3 world{ 12.f, -7.f, -6.f };
        const Vec3 low = Normalize(Vec3{ .9f, .1f, .12f });
        const Vec3 below = Normalize(Vec3{ .2f, .3f, -.9f });
        Require(std::isfinite(ShaftDensity(field, world, low, 6.f)),
                "a source near the horizon still marches, because the entry walk is floored");
        Require(ShaftDensity(field, world, below, 6.f) >= 0.f && ShaftDensity(field, world, below, 6.f) <= 1.f,
                "a source below the horizon cannot put a beam in the frame, and cannot produce a NaN");
    }
    {
        // The wobble is the one cue that does not depend on anything being visible in the
        // water, so it is deliberately tiny: at full distortion the frame moves by less than two
        // texels at its peak, and at ramp zero it does not move at all.
        Settings settings;
        settings.distortion = 1.f;
        const float texelX = 1.f / 1920.f, texelY = 1.f / 1080.f;
        double peakX = 0.0, peakY = 0.0;
        for (int index = 0; index < 20000; ++index)
        {
            const float uvX = float(index % 200) / 200.f, uvY = float(index / 200) / 100.f;
            const float seconds = float(index) * .0173f;
            float out[2], air[2];
            Wobble(settings, uvX, uvY, seconds, texelX, texelY, 1.f, out);
            Wobble(settings, uvX, uvY, seconds, texelX, texelY, 0.f, air);
            peakX = std::max<double>(peakX, std::fabs(double(out[0]) * 1920.0));
            peakY = std::max<double>(peakY, std::fabs(double(out[1]) * 1080.0));
            if (air[0] != 0.f || air[1] != 0.f) throw std::runtime_error("the wobble moves above water");
        }
        Require(peakX <= kMaxDistortionTexels + 1e-4 && peakY <= kMaxDistortionTexels + 1e-4
                    && peakX > kMaxDistortionTexels * .8 && peakY > kMaxDistortionTexels * .8,
                "the wobble's peak is the texel budget it is documented as, and no more");
        // The clock is wrapped into the one period all four of its rates share, so an hour into
        // a dive the argument still has fraction bits and the motion has not stepped.
        float atZero[2], atPeriod[2], late[2], guarded[2];
        Wobble(settings, .37f, .61f, 0.f, texelX, texelY, 1.f, atZero);
        Wobble(settings, .37f, .61f, kWobblePeriodSeconds, texelX, texelY, 1.f, atPeriod);
        Require(Near(atZero[0], atPeriod[0], 1e-6) && Near(atZero[1], atPeriod[1], 1e-6),
                "the wrapped wobble clock is continuous across its own period");
        Wobble(settings, .37f, .61f, 7200.f, texelX, texelY, 1.f, late);
        Require(std::isfinite(late[0]) && std::isfinite(late[1]) && std::fabs(late[0]) < 1e-3,
                "two hours under water still wobbles by the same texel budget");
        Wobble(settings, .5f, .5f, std::numeric_limits<float>::quiet_NaN(), texelX, texelY, 1.f, guarded);
        Require(std::isfinite(guarded[0]) && std::isfinite(guarded[1]),
                "a NaN clock cannot put a NaN in the frame's uv");
    }
    {
        // Turbidity: silt hangs in the column and arrives in patches. A deep point is murkier
        // than a shallow one at the same camera distance; two points a hundred yards apart in
        // the world differ; the same point from another camera does not. The whole of it is a
        // property of the place, which is what makes it read as dirt rather than as fog, and at
        // TURBIDITY 0 the column carries exactly the zone's own extinction.
        const Medium medium = MediumFor(kClarity, kAbsorptionPercent, kAbsorbRegister, kDeepRegister,
                                        kTintRegister, true);
        const Field field = ShippedField();
        Settings settings;
        PixelInput in = PixelInput{};
        in.scene = { .8f, .8f, .8f };
        in.distance = 20.f;
        in.ray = { 1.f, 0.f, 0.f };
        in.up = { 0.f, 0.f, 1.f };
        in.normal = { 0.f, 0.f, 1.f };
        in.submersion = 8.f;
        in.world = { 20.f, 0.f, 7.9f };    // 0.1 yards of water over the point
        const PixelOutput shallow = ShadePixel(settings, medium, field, in);
        in.world.z = 7.9f - 30.f;          // 30 yards down, past the silt gradient
        const PixelOutput deep = ShadePixel(settings, medium, field, in);
        Require(deep.colour.x < shallow.colour.x && deep.colour.y < shallow.colour.y,
                "thirty yards down reads murkier than just under the surface at the same distance");
        float lo = 2.f, hi = -1.f;
        for (int i = 0; i < 64; ++i)
        {
            in.world = { 20.f + float(i) * 13.f, float((i * 7) % 29) * 11.f, 7.9f };
            const PixelOutput out = ShadePixel(settings, medium, field, in);
            lo = std::min(lo, out.colour.y);
            hi = std::max(hi, out.colour.y);
        }
        Require(hi - lo > .02f, "the murk arrives in hundred-yard patches, not as a flat extinction");
        Settings distilled = settings;
        distilled.turbidity = 0.f;
        in.world = { 20.f, 0.f, 7.9f - 30.f };
        const PixelOutput clean = ShadePixel(distilled, medium, field, in);
        const PixelOutput alsoClean = ShadePixel(distilled, medium, field, in);
        Require(Near(clean.colour.y, alsoClean.colour.y, 0.f) && clean.absorption < deep.absorption,
                "turbidity 0 is the zone's medium with no silt in it: strictly clearer than the deep water");
        Settings quarry = settings;
        quarry.turbidity = 3.f;
        const PixelOutput dirty = ShadePixel(quarry, medium, field, in);
        Require(dirty.colour.y < deep.colour.y * .8f,
                "turbidity 3 is markedly murkier than turbidity 1 at the same point");
    }
}

void TestMirror()
{
    std::printf("\n-- the ps_3_0 stage mirrors the core, and one ramp drives every pass --\n");
    const std::string core = ReadAlongside("../src/Water/UnderwaterMedium.h");
    const std::string pass = ReadAlongside("../src/Effects/Underwater.h");
    const std::string water = ReadAlongside("../WaterEffect.h");
    const std::string composite = ReadAlongside("../VolumeIntegration.h");
    const std::string rendererCpp = ReadAlongside("../ModernWoWRenderer.cpp");
    const std::string atmosphere = ReadAlongside("../src/Effects/DirectionalVolumetricLighting.cpp");
    const std::string clouds = ReadAlongside("../src/Effects/VolumetricClouds.cpp");
    const std::string frameContext = ReadAlongside("../src/Core/FrameContext.h");
    const std::string weather = ReadAlongside("../WeatherVisuals.h");
    const std::string ini = ReadAlongside("../GraphicsEffects.ini");
    const std::string overlay = ReadAlongside("../src/Environment/OverlayItems.h");
    Require(core.size() > 8192 && pass.size() > 8192 && water.size() > 4096 && composite.size() > 4096
                && rendererCpp.size() > 4096 && weather.size() > 4096
                && atmosphere.size() > 4096 && clouds.size() > 4096 && frameContext.size() > 1024
                && ini.size() > 1024 && overlay.size() > 1024,
            "every file the underwater path spans is readable as text");

    // ---- the pass mirrors the core, literal for literal ----
    Require(pass.find("float ViewZ(float raw) {") != std::string::npos
                && pass.find("return projection.y/min(norm-projection.x,-1e-6);") != std::string::npos
                && pass.find("return float3((uv.x*2-1)/projection.z,(1-uv.y*2)/projection.w,1)*z;") != std::string::npos,
            "the pass reconstructs the depth buffer with the ambient occlusion stage's own helpers");
    Require(pass.find("i-=floor(i*(1.0/64.0))*64.0;") != std::string::npos
                && pass.find("float4 h=frac((i.x+i.y*64.0+float4(0,1,64,65))*.1031);") != std::string::npos
                && pass.find("h*=h+33.33;") != std::string::npos,
            "the caustic net sits on the renderer's one noise lattice, wrapped before it is hashed");
    Require(pass.find("float b=causticNoise(cell*2.0+chopPhase.zw);") != std::string::npos
                && pass.find("return pow(saturate(ridgeA*ridgeB),2.2);") != std::string::npos
                && pass.find("modulation=lerp(1.0,causticNet(cell)*5.1034,caustic);") != std::string::npos,
            "the net is the surface's own two octaves, sharpened and normalized by the measured mean");
    Require(pass.find("float3 column=exp(-extinction.xyz*min(lerp(250.0,distance*dirt*exitWash,exitWindow),250.0));") != std::string::npos
                && pass.find("float3 transmittance=lerp(float3(1,1,1),column,strength);") != std::string::npos
                && pass.find("float absorption=1-transmittance.y;") != std::string::npos,
            "the column is Beer-Lambert per channel over the same far cap, and STRENGTH moves its effect");
    Require(pass.find("float g=.3+.34*saturate(lightDirection.w);") != std::string::npos
                && pass.find("float phaseDen=max(1+g*g+2*g*vDotL,1e-3);") != std::string::npos
                && pass.find("float lobe=(1-g*g)*.17/(phaseDen*sqrt(phaseDen))*sunGate;") != std::string::npos
                && pass.find("float3 scatter=(deepColour.rgb*1.7+waterTint.rgb*.12)*lightColor.rgb;") != std::string::npos
                && pass.find("float3 inScatter=scatter*(absorption*lobe*(1+2.2*shaft)*strength);") != std::string::npos,
            "the in-scatter is the water stage's own normalized Henyey-Greenstein lobe, beams and all");
    Require(pass.find("float3 entry0=lightWorld*(extinction.w/rise);") != std::string::npos
                && pass.find("float3 entry1=world+lightWorld*((extinction.w-world.z)/rise);") != std::string::npos
                && pass.find("total+=broadWarp(lerp(cell0,cell1,float(sample)/3.0));") != std::string::npos
                && core.find("total += BroadWarpCell(field, Lerp(cell0[0], cell1[0], t), Lerp(cell0[1], cell1[1], t));")
                    != std::string::npos
                && pass.find("float rise=max(lightWorld.z,.08);") != std::string::npos
                && pass.find("for(int sample=1;sample<=3;++sample)") != std::string::npos
                && pass.find("return saturate((total/3.0-.5)*4.4);") != std::string::npos,
            "a beam is three samples of the surface's broad warp, taken where each ray entered the water");
    Require(pass.find("float depthFade=saturate(under*1.5)*saturate(1.0-under/28.0);") != std::string::npos
                && pass.find("facing=saturate(dot(n,upSeconds.xyz)*2.0-.2);") != std::string::npos
                && pass.find("float causticScale=saturate(lightColor.w)*sunGate*strength;") != std::string::npos
                && pass.find("caustic=causticScale*facing*depthFade;") != std::string::npos
                && core.find("const float causticScale = Saturate(settings.caustics) * Saturate(in.sunElevation * 2.5f + .2f)")
                    != std::string::npos
                && core.find("out.caustic = causticScale * facing * depthFade;") != std::string::npos
                && core.find("const float depthFade = Saturate(under * 1.5f) * Saturate(1.f - under / kCausticReachYards);")
                    != std::string::npos,
            "the net needs water over it, a bed to land on, a source above the horizon and a medium at all");
    Require(pass.find("inScatter+=(deepColour.rgb*1.6+waterTint.rgb*.18)*lightColor.rgb") != std::string::npos
                && pass.find("float upLook=saturate(dot(viewRay,upSeconds.xyz));") != std::string::npos
                && pass.find("colour+=deepColour.rgb*(absorption*strength);") != std::string::npos,
            "the sky over the surface lights up-looking rays in the water's own hue, and the column ends at its deep colour");
    Require(pass.find("if(ramp<=0.0) return scene;") != std::string::npos
                && pass.find("float4 waterData=tex2D(waterMask,sampleUv);") != std::string::npos
                && pass.find("if(max(waterData.r,waterData.g)>.25) return scene;") != std::string::npos
                && pass.find("float wobble=saturate(invView1.w)*peakTexels*ramp/sinePeak;") != std::string::npos,
            "above water the pass returns the frame it was handed, and the liquid's own pixels stay the water stage's");
    // fxc rejects a forced dynamic branch around a body that samples with an implicit LOD on
    // ps_3_0 (X3528, "can't force branch with gradients on non-inputs"), which is a compile
    // failure and not a slow shader - VolumeEffects.h carries the same rule for its
    // anti-aliasing taps. Every branch that reaches a tex2D is a plain if, and the three
    // [branch] attributes that remain wrap pure arithmetic - the noise lattice is a hash and a
    // bicubic, not a texture - where forcing the branch is what skips the work. Two sit on
    // shader constants (the caustic gate's whole product, and the beams') so a tile takes them
    // together; one sits on a per-pixel value, the net's own amount, which is what skips the
    // two noise evaluations on walls and sky while caustics are on.
    Require(pass.find("[branch] if(tex2D") == std::string::npos
                && pass.find("[branch] if(raw") == std::string::npos
                && pass.find("[branch] if(ramp") == std::string::npos
                && pass.find("[branch] if(causticScale>0.0)") != std::string::npos
                && pass.find("[branch] if(caustic>0.0)") != std::string::npos
                && pass.find("[branch] if(invView0.w>0.0&&lightDirection.w>0.0)") != std::string::npos,
            "no forced branch wraps an implicit-LOD sample, because fxc refuses to compile one");
    Require(pass.find("float peakTexels=1.6;") != std::string::npos
                && pass.find("float sinePeak=1.6;") != std::string::npos
                && pass.find("sin(uv.y*38.0+clock*1.7)+.6*sin(uv.y*17.0-clock*1.1)") != std::string::npos
                && pass.find("sin(uv.x*34.0-clock*1.3)+.6*sin(uv.x*15.0+clock*.9)") != std::string::npos,
            "the wobble is the core's own four rates at the core's own texel budget");
    Require(pass.find("underwater::kWobblePeriodSeconds") != std::string::npos
                && pass.find("std::floor(seconds / underwater::kWobblePeriodSeconds)") != std::string::npos
                && pass.find("float wrapped = std::isfinite(seconds)") != std::string::npos,
            "the wobble clock is wrapped on the CPU into the period all four rates share");
    Require(pass.find("if (ramp <= 0.f || !inputs.medium.valid)") != std::string::npos
                && pass.find("if (!frameContext.viewRawValid) { NoteIdle(6,") != std::string::npos
                && pass.find("unpack[2] <= 0.f || unpack[3] <= 0.f || frameContext.depthMaxZ <= 0.f") != std::string::npos,
            "the pass does not draw on a frame it cannot reconstruct, and leaves that frame alone");
    // The frame the pass publishes is one number: the submersion ramp, folded into the
    // medium's strength the same way on both sides of the mirror. Pinning the two spellings
    // against each other is what makes "above water this is an exact identity" a checked
    // property rather than a claim, and what stops one half from growing a second ramp.
    Require(core.find("const float strength = Saturate(settings.strength) * Saturate(in.ramp);")
                != std::string::npos
                && pass.find("float strength=saturate(deepColour.w)*ramp;") != std::string::npos
                && core.find("float ramp = 1.f;") != std::string::npos
                && pass.find("float ramp=saturate(screen.w);") != std::string::npos,
            "one ramp, folded into the strength, spelled the same way in the core and in the stage");
    // The caustic net is evaluated only where it can be non-zero. That is an exact equivalence
    // - lerp(1, net*N, 0) is 1 for a bounded net - so it saves two noise evaluations on every
    // wall, every point past the net's reach and every pixel of sky without moving a single
    // value, and it has to be gated the same way on both sides or the mirror stops being one.
    Require(core.find("if (out.caustic > 0.f)") != std::string::npos
                && core.find("causticModulation = Lerp(1.f, CausticNet(field, shadedWorld) * kCausticNormalization,"
                               " out.caustic);")
                    != std::string::npos
                && core.find("const float causticScale = Saturate(settings.caustics)") != std::string::npos
                && core.find("out.caustic = causticScale * facing * depthFade;") != std::string::npos
                && pass.find("float causticScale=saturate(lightColor.w)*sunGate*strength;") != std::string::npos
                && pass.find("[branch] if(causticScale>0.0) {") != std::string::npos
                && pass.find("caustic=causticScale*facing*depthFade;") != std::string::npos
                && pass.find("[branch] if(caustic>0.0) {") != std::string::npos
                && pass.find("modulation=lerp(1.0,causticNet(cell)*5.1034,caustic);") != std::string::npos,
            "the net is skipped where it would be multiplied by zero, identically in both halves");
    // The column ends at the surface. Both halves clamp a ray that reaches the liquid plane
    // before it reaches the point it is shading - sky through an opening, a sail, a cliff face
    // with no tile over it - to the camera-to-surface segment, and both then read the net and
    // the beam march from the end of the WATER rather than the end of the ray. The ordering is
    // pinned too, because clamping after `world` is built would leave the beams marching from a
    // point in the air above the surface.
    const std::string coreClamp = "const float columnYards = std::min(in.distance, surfaceHit);";
    const std::string passClamp = "distance=min(distance,surfaceHit);";
    const std::string passWorld = "world=ToWorld(viewRay)*distance;";
    Require(core.find("const float surfaceHit = (rayUp > 1e-3f && in.submersion > 0.f)") != std::string::npos
                && core.find(coreClamp) != std::string::npos
                && pass.find("float surfaceHit=(rayUp>1e-3&&extinction.w>0.0)?extinction.w/max(rayUp,1e-3):distance;")
                    != std::string::npos
                && core.find("const Vec3 shadedWorld =") != std::string::npos
                && core.find("ShaftDensity(field, shadedWorld, lightWorld, in.submersion)") != std::string::npos
                && pass.find(passClamp) != std::string::npos && pass.find(passWorld) != std::string::npos
                && pass.find(passClamp) < pass.find(passWorld),
            "the water a submerged ray crosses stops at the surface, on both sides of the mirror");
    // Shader safety. A NaN compares false against everything, so this guard returns the frame
    // the pass was handed instead of poisoning every pixel with one bad constant - the last
    // line of defence behind the CPU validation, which is where the same numbers are checked
    // before they are uploaded: the camera flag that publishes the inverse view, the three
    // inverse-view rows themselves (finite and not collapsed, because they turn a view-space
    // ray into the world yards the net, the beams and the facing test all read), and the yards
    // of water overhead, where std::max(NaN, 0) is not specified to be 0.
    Require(pass.find("float finiteCheck=dot(colour,colour);") != std::string::npos
                && pass.find("if(!(finiteCheck<1e30)) return scene;") != std::string::npos
                && pass.find("const bool poseRetained = !frameContext.cameraValid && frameContext.viewRawValid &&")
                    != std::string::npos
                && pass.find("frameContext.cameraEyeAgeMs <= FrameContext::kCameraEyeHoldMs;")
                    != std::string::npos
                && pass.find("if (!frameContext.cameraValid && !poseRetained) { NoteIdle(7,")
                    != std::string::npos
                && pass.find("const float value = frameContext.inverseView.m[row][axis];") != std::string::npos
                && pass.find("if (!std::isfinite(value))") != std::string::npos
                && pass.find("if (!(lengthSquared > 1e-8f))") != std::string::npos
                && pass.find("const float yardsOverhead = std::isfinite(frameContext.underwaterYards)")
                    != std::string::npos,
            "a NaN cannot reach the frame: the stage falls back to it, and the CPU refuses to upload one");
    // The medium must not be at the mercy of which way the diver happens to look. A frame
    // whose draws carried no fresh eye (looking up at the surface, a wall, a menu) keeps the
    // retained pose for the same short hold the lamps use, instead of dropping the fog,
    // the caustics, the beams and the grade at some camera angles and not others.
    Require(pass.find("const bool poseRetained = !frameContext.cameraValid && frameContext.viewRawValid &&")
                != std::string::npos
                && pass.find("frameContext.cameraEyeAgeMs <= FrameContext::kCameraEyeHoldMs;")
                != std::string::npos,
            "an un-recaptured pose sustains the medium for kCameraEyeHoldMs instead of switching it off by camera angle");
    // Dirt: the silt gradient and the hundred-yard patch field are spelled identically on both
    // sides of the mirror, ride the spare warp-register lane, and multiply the zone's clarity
    // rather than replacing it - there is still one clarity and the zone still owns it.
    Require(core.find("const float silt = (.65f + .7f * patch)") != std::string::npos
                && core.find("* (1.f + .9f * Saturate(under / kSiltYards));") != std::string::npos
                && core.find("const float dirt = std::max(1.f + turbidity * (silt - 1.f), 0.f);")
                    != std::string::npos
                && core.find("LatticeCell(field, shadedWorld * (1.f / kTurbidityPatchCells), dirtCell);")
                    != std::string::npos
                && pass.find("float2 dirtCell=float2(dot(world.xy,lattice.xy),dot(world.xy,lattice.zw))*(1.0/40.0)+float2(37.31,57.17);")
                    != std::string::npos
                && pass.find("float silt=(.65+.7*noiseValue(dirtCell))*(1.0+.9*saturate(under/18.0));")
                    != std::string::npos
                && pass.find("float dirt=max(1.0+warpPhase.z*(silt-1.0),0.0);") != std::string::npos
                && core.find("const Vec3 column = Transmittance(medium, effectiveYards);") != std::string::npos
                && pass.find("float3 column=exp(-extinction.xyz*min(lerp(250.0,distance*dirt*exitWash,exitWindow),250.0));") != std::string::npos
                && pass.find("settings.turbidity, exitTurbidity };") != std::string::npos
                && pass.find("const float exitTurbidity = underwater::ExitWash(settings.turbidity, yardsOverhead, true) - 1.f;")
                    != std::string::npos
                && pass.find("settings.turbidity = scaled(settings.turbidity, inputs.turbidityMultiplier, 3.f);")
                    != std::string::npos,
            "turbidity is a world-place property, mirrored on both sides, riding the warp register");
    // Snell's window: past the critical cone the plane is a mirror from below, so the
    // beyond-plane scene is marched away in both halves - this is what keeps horizon land
    // out of an underwater frame on pixels no liquid tile covered.
    Require(core.find("inline constexpr float kSnellCos = .6613f;") != std::string::npos
                && core.find("? Smoothstep(kSnellCos - .06f, kSnellCos + .06f, rayUp) : 1.f;") != std::string::npos
                && core.find("const float effectiveYards = Lerp(kFarColumnYards, columnYards * dirt * exitWash, exitWindow);")
                    != std::string::npos
                && pass.find("float exited=surfaceHit<distance?1.0:0.0;") != std::string::npos
                && pass.find("float exitWindow=lerp(1.0,smoothstep(.6013,.7213,rayUp),exited);") != std::string::npos,
            "Snell's window closes on the same cosine in the core and in the stage");
    Require(core.find("inline constexpr float kInterfaceScatterYards = 4.f;") != std::string::npos
                && core.find("const float exitWash = ExitWash(turbidity, in.submersion, surfaceHit < in.distance);")
                    != std::string::npos
                && pass.find("float exitWash=1.0+warpPhase.w*exited;") != std::string::npos
                && core.find("inline float NoiseValue64(float x, float y) noexcept") != std::string::npos
                && pass.find("float causticNoise(float2 p) {") != std::string::npos
                && pass.find("e-=(e>=64.0)?64.0:0.0;") != std::string::npos,
            "the sky's picture pays the turbidity wash on exit, and the net's lattice is periodic at its own wrap");
    // The far sheet of a crest is not drawn at all: coverage cannot fix a sheet that should
    // not paint, because alpha only says how strongly it paints when it is drawn last. But
    // the discard is only sound while a pixel resolves the wave: past a few lattice cells a
    // pixel spans whole liquid triangles, which sheet covers the pixel centre is sampling
    // luck, and discarding the far slope a pixel happens to have gotten leaves a hole with
    // the sky and the land behind the sea showing through it - a glass horizon, reported
    // from the live client. So the discard fades with the footprint on the same ground the
    // unresolved chop does, and the lattice coordinate it needs is taken above the test.
    Require(water.find("float3 faceCross=cross(ddx(viewPos),ddy(viewPos));") != std::string::npos
                && water.find("float3 faceNormal=faceCross*(dot(faceCross,upWind.xyz)<0?-1:1);") != std::string::npos
                && water.find("float wrongSide=lerp(-faceSide,faceSide,smoothstep(-1.0,1.0,waveSwell2.w))")
                    != std::string::npos
                && water.find("wrongSide*=1.0-smoothstep(1.2,3.6,max(footprint.x,footprint.y));") != std::string::npos
                && water.find("clip(flowControl.w<.5?.25-wrongSide:1.0);") != std::string::npos
                && water.find("surfaceAlpha*=1-smoothstep(0.0,.25,wrongSide);") != std::string::npos
                && water.find("float2 plane=float2(dot(worldRel.xy,waveLattice.xy)") <
                       water.find("clip(flowControl.w<.5?.25-wrongSide:1.0);"),
            "the water stage discards the far side of a wave before shading it, while the pixel still resolves the wave");
    // The first dive of a cold session once stopped the render outright, and the
    // first FRAME of a cold launch did too. Two costs were waiting on the dive:
    // the pass's shader was compiled (and driver-JITed) lazily at first
    // submersion, and every newly seen liquid chunk paid a buffer lock that can
    // wait on the GPU. Moving the compile to "an ordinary compositor frame"
    // put it inside the very first one - a white login window, audio already
    // running, on every cold-cache launch. Neither cost is a frame's any more:
    // the bytecode is prepared on a worker from Configure (GetBytecode, no
    // device), CreatePixelShader waits for presented frames in Prewarm (or for
    // an actual dive in Render, by which time the worker has had the login),
    // and the vertex read spends at most one chunk's locks per frame. The
    // budget is the whole of that safety: an earlier version also refused
    // dynamic default-pool buffers outright as belt and braces, but wherever a
    // client's liquid chunks are (flagged) dynamic, that refusal made every
    // reading answer from the draw origin's plane forever - one way a whole
    // zone's water never engages the medium - so no pool is refused any more.
    Require(pass.find("void Prewarm(IDirect3DDevice9* device)") != std::string::npos
                && pass.find("void StartPrepare()") != std::string::npos
                && pass.find("std::thread([this, cacheDir, forceCompile, began]") != std::string::npos
                && pass.find("shaderbytecode::GetBytecode(cacheDir, kUnderwaterSource") != std::string::npos
                && pass.find("shaderbytecode::GetShader(") == std::string::npos
                && pass.find("if (IsActive()) StartPrepare();") != std::string::npos
                && pass.find("m_prewarmFrames < underwaterpass::kPrewarmFrames") != std::string::npos
                && pass.find("if (!EnsureShader(device) && !ShaderPending())") != std::string::npos
                && composite.find("renderer::Underwater::Instance().Prewarm(d);") != std::string::npos
                && water.find("presentSerial++;") != std::string::npos
                && water.find("if(liquidVertexZLockSerial==presentSerial)return false;") != std::string::npos
                && water.find("desc.Usage&D3DUSAGE_DYNAMIC") == std::string::npos,
            "nothing expensive waits for the first frame or the first dive: bytecode compiles on a worker, device creation waits for presented frames, vertex locks are budgeted");
    // Diagnostics: a medium that never engages is invisible without them. The plane reading
    // reports WHY (degenerate/sheared/range), the water status line counts each per frame
    // alongside the off-screen and no-vertex counts, the last reading itself and the lane the
    // draws uploaded, and the pass logs which of its guards is keeping it idle, on
    // transition, so a player's two logs answer which refusal is firing.
    Require(core.find("int reason = 0;") != std::string::npos
                && core.find("out.reason = 3; return out;") != std::string::npos
                && water.find("else if(planeReading.reason==1)uwPlaneRigid++;") != std::string::npos
                && water.find("else if(planeReading.reason==3)uwPlaneRot++;") != std::string::npos
                && water.find("else if(!mainTarget)uwPlaneOffscreen++;") != std::string::npos
                && water.find("<<\" uwPlane=\"<<uwPlaneValid") != std::string::npos
                && water.find("<<\" yards=\"<<uwLastYards") != std::string::npos
                && water.find("<<\" lane=\"<<uwLastLane") != std::string::npos
                && water.find("uwLastLane=controls[75];") != std::string::npos
                && pass.find("void NoteIdle(int code, const std::string& detail)") != std::string::npos
                && pass.find("Log(\"underwater pass drawing\");") != std::string::npos
                && pass.find("ramp \" + std::to_string(ramp)") != std::string::npos,
            "both logs say which refusal is firing: reading reasons, per-frame counts, pass idle state");
    // The wobble clock is wrapped where it is kept, not only where it is read: an unbounded
    // float32 loses the fraction bits the wrap depends on after a few days, so the published
    // clock stays inside one period - by whole periods, so the four rates stay continuous.
    Require(water.find("underwaterSeconds-=std::floor(underwaterSeconds/wobblePeriod)*wobblePeriod;")
                != std::string::npos
                && water.find("if(!(underwaterSeconds>=0.f&&underwaterSeconds<1e6f)) {") != std::string::npos,
            "the published wobble clock is wrapped at the source and reset on a NaN");
    Require(pass.find("check(device->SetRenderState(D3DRS_COLORWRITEENABLE, 7));") != std::string::npos
                && pass.find("D3DRS_SRGBWRITEENABLE, D3DRS_ALPHABLENDENABLE })") != std::string::npos
                && pass.find("ScopedPassState saved(device);") != std::string::npos
                && pass.find("check(device->SetDepthStencilSurface(nullptr));") != std::string::npos,
            "the pass writes rgb with blending off on its own state-block level, and restores what it found");
    Require(pass.find("if (!EnsureShader(device))") != std::string::npos
                && pass.find("shaderbytecode::GetBytecode(cacheDir, kUnderwaterSource, \"main\", \"ps_3_0\"") != std::string::npos
                && pass.find("device->CreatePixelShader(") != std::string::npos
                && pass.find("m_setupRetry.Open(GetTickCount64())") != std::string::npos
                && pass.find("m_setupRetry.Fail(GetTickCount64(), underwaterpass::kSetupRetryMs)") != std::string::npos,
            "the pass prepares bytecode once through the shared cache, creates the device shader from it, and backs off after a failure");

    // ---- one answer per frame, published before anything reads it ----
    Require(frameContext.find("float underwaterRamp = 0.f;") != std::string::npos
                && frameContext.find("float underwaterYards = -1.f;") != std::string::npos,
            "the camera's medium is one published number in the frame context, not three opinions");
    Require(composite.find("frameCtx.underwaterRamp = underwaterActive && confirmedPlayableWorld")
                != std::string::npos
                && composite.find("const bool confirmedPlayableWorld = frameCtx.worldScene.state==renderer::WorldSceneState::InWorld;")
                    != std::string::npos
                && composite.find("frameCtx.underwaterRamp = underwaterActive ? watereffect::submersionTracker.ramp : 0.f;")
                    == std::string::npos
                && composite.find("frameCtx.underwaterYards = watereffect::submersionTracker.yards;") != std::string::npos
                && composite.find("const bool underwaterActive = !shaftDebug && renderer::Underwater::Instance().IsActive();")
                    != std::string::npos,
            "the compositor publishes the water stage's settled answer only inside the playable world");
    Require(pass.find("if(frameContext.worldScene.state!=WorldSceneState::InWorld)") != std::string::npos
                && water.find("underwaterWorldContinuity.Observe(frame.worldScene)") != std::string::npos
                && water.find("submersionTracker=renderer::underwater::Tracker{};") != std::string::npos,
            "the underwater pass fails closed off-world and its retained plane resets on verified location transitions");
    // The precipitation stage composites during the client's draw stream, before the composite
    // that settles this frame's medium, so the same answer is published for it one frame ahead:
    // rain (and snow/sand) fades out below the surface instead of drawing over a diver.
    Require(frameContext.find("float compositedUnderwaterRamp = 0.f;") != std::string::npos
                && composite.find("frameCtx.compositedUnderwaterRamp = (!shaftDebug && enabled && active &&")
                    != std::string::npos
                && weather.find("const float aboveWater = 1.f - std::clamp(frame.compositedUnderwaterRamp, 0.f, 1.f);")
                    != std::string::npos
                && weather.find("intensity * frame.environment[renderer::Weather] * aboveWater") != std::string::npos
                && weather.find("lensStrength * aboveWater") != std::string::npos,
            "the draw-stream precipitation pass fades on the same ramp, published one frame ahead of it");
    {
        const auto publish = composite.find("frameCtx.underwaterRamp =");
        const auto atmosphereCall = composite.find("atmosphereRendered = renderer::DirectionalVolumetricLighting");
        const auto cloudsCall = composite.find("clouds.Render(d, frameCtx, target.Get())");
        const auto passCall = composite.find("renderer::Underwater::Instance().Render(d, frameCtx, target.Get()");
        const auto postCall = composite.find("ApplyImagePostProcess(d,target.Get()", passCall);
        Require(publish != std::string::npos && atmosphereCall != std::string::npos
                    && cloudsCall != std::string::npos && passCall != std::string::npos
                    && postCall != std::string::npos
                    && publish < cloudsCall && publish < atmosphereCall
                    && cloudsCall < passCall && atmosphereCall < passCall && passCall < postCall,
                "the ramp is published before the air passes read it, and the medium is applied after them and before the grade");
        const auto refresh = composite.find(
            "check(d->StretchRect(target.Get(), nullptr, legacyTargets.sceneSurface.Get(), nullptr, D3DTEXF_NONE));",
            passCall);
        Require(refresh != std::string::npos && refresh < postCall,
                "the scene copy the grade reads is republished after the medium is applied");
        Require(composite.find("inputs.medium = watereffect::underwaterFrame.medium;", passCall - 2048)
                    != std::string::npos
                && composite.find("inputs.sunElevation = watereffect::underwaterFrame.sunElevation;") != std::string::npos,
                "the pass is handed the medium the water draws resolved, not a second opinion about the water");
        // The place leans on the player's dials per frame: the composite hands
        // the settled environment's underwater tuple to the pass under the same
        // ZoneWaves switch that governs the rest of the zone water, the water
        // draw applies zone clarity to surface and medium as one number, and
        // the pass scales a copy of its settings inside the core's own bounds.
        Require(composite.find("if (watereffect::zoneWavesEnabled) {", passCall - 2048) != std::string::npos
                && composite.find("inputs.turbidityMultiplier = frameCtx.environment[renderer::UnderwaterTurbidity];")
                    != std::string::npos
                && composite.find("inputs.causticMultiplier = frameCtx.environment[renderer::UnderwaterCaustics];")
                    != std::string::npos
                && composite.find("inputs.shaftMultiplier = frameCtx.environment[renderer::UnderwaterShafts];")
                    != std::string::npos
                && composite.find("inputs.glowMultiplier = frameCtx.environment[renderer::UnderwaterGlow];")
                    != std::string::npos
                && water.find("controls[52]=.088f*(100.f/(clarityPercent*clarityMultiplier));") != std::string::npos
                && composite.find("watereffect::underwaterFrame.turbidity = underwaterSettings.enabled")
                    != std::string::npos
                && water.find("controls[11]=renderer::underwater::ExitWash(effectiveTurbidity,submersion,true)-1.f;")
                    != std::string::npos
                && pass.find("settings.caustics = scaled(settings.caustics, inputs.causticMultiplier, 1.f);")
                    != std::string::npos
                && pass.find("settings.surfaceGlow = scaled(settings.surfaceGlow, inputs.glowMultiplier, 1.f);")
                    != std::string::npos,
                "a swept zone scales the player's underwater dials for its frame, and clarity moves surface and medium together");
    }
    Require(composite.find("const bool hasUnderwater = !shaftDebug && renderer::Underwater::Instance().IsActive();")
                != std::string::npos
                && composite.find("hasLightScatteringDebug || hasUnderwater;") != std::string::npos,
            "the pass is part of HasActiveEffects, which is what allocates the water mask it reads");
    Require(atmosphere.find("const float airShare=std::clamp(1.f-f.underwaterRamp,0.f,1.f);") != std::string::npos
                && atmosphere.find("settings.densityScale*=airShare;") != std::string::npos
                && atmosphere.find("settings.localFogDensity*=airShare;") != std::string::npos
                && atmosphere.find("settings.scatterStrength*=airShare;") != std::string::npos
                && atmosphere.find("settings.edgeFogPower*=airShare;") != std::string::npos
                && atmosphere.find("if(airShare<=0.f)return false;") != std::string::npos,
            "the atmosphere integrates air only, and fades every one of its densities out on the same ramp");
    Require(clouds.find("sampler2D waterMask:register(s4);") != std::string::npos
                && clouds.find("float4 mediumControl:register(c11);") != std::string::npos
                && clouds.find("float4 waterData=tex2D(waterMask,uv);") != std::string::npos
                && clouds.find("float submergedWater=step(.35,max(waterData.r,waterData.g))*saturate(mediumControl.x);")
                    != std::string::npos
                && clouds.find("cloud.rgb*=1-submergedWater;") != std::string::npos
                && clouds.find("cloud.a=lerp(cloud.a,1,submergedWater);") != std::string::npos
                && clouds.find("d->SetTexture(4,f.waterMaskTexture);") != std::string::npos
                && clouds.find("const float c11[4]={std::clamp(f.underwaterRamp,0.f,1.f),0,0,0};") != std::string::npos
                && clouds.find("for(DWORD s=0;s<5;++s)d->SetTexture(s,nullptr);") != std::string::npos,
            "the deck stops landing on a surface seen from underneath, and above water that is an identity");

    // ---- the water stage publishes what the pass reads ----
    Require(water.find("const bool haveVertices=mainTarget&&ReadLiquidVertices(d,vertexSamples);") != std::string::npos
                && water.find("renderer::underwater::PlaneOverCamera(modelView,") != std::string::npos
                && water.find("(haveVertices&&vertexSamples.haveLast)?&lastVertex:nullptr") != std::string::npos
                && water.find("mainTarget=mainTargetSurface&&currentTarget.Get()==mainTargetSurface;") != std::string::npos
                && water.find("submersionTracker.Observe(planeReading);uwPlaneValid++;") != std::string::npos
                && water.find("uwPlaneFar++;") != std::string::npos
                && water.find("(planeReading.valid&&planeReading.local)") != std::string::npos
                && water.find("controls[75]=std::clamp(submersion/planeBand,-2.f,2.f);") != std::string::npos
                && water.find("float underwater=smoothstep(-1.0,1.0,waveSwell2.w);") != std::string::npos,
            "the water pixel stage classifies from the draw's own matrix and its own first and last vertices, and only local water counts");
    // Round 17's three halves of one gate, pinned together because each was
    // separately enough to put the medium on dry ground near water: the
    // reading measures the emitted triangles when the caller read them,
    // the tracker remembers only overhead planes and lets known-outside
    // evidence retire the memory, and the surface stage classifies itself
    // by the same entry rule instead of any local positive yards.
    Require(water.find("(haveVertices&&vertexSamples.haveCoverage)?&liquidCoverage:nullptr") != std::string::npos
                && water.find("renderer::underwater::ClassifiedYards(") != std::string::npos
                && water.find("const float submersion=(planeReading.valid&&planeReading.local)") == std::string::npos
                && core.find("if (reading.yards > kExitYards && !overhead)") != std::string::npos
                && core.find("if (haveCamera && observedOverhead)") != std::string::npos
                && core.find("outsideSeconds >= kOutsideMemorySeconds") != std::string::npos
                && core.find("&& !submerged) return;") == std::string::npos,
            "entry, memory, sustain and the surface lane all spend the one overhead rule");
    Require(water.find("published.medium=UnderwaterMediumForFrame(colourSet,clarityMultiplier);") != std::string::npos
                && water.find("published.field") == std::string::npos
                && water.find("published.sunElevation=published.lightDir[0]*published.up[0]") != std::string::npos
                && water.find("if(mainTarget) {") != std::string::npos,
            "the water draw publishes the medium and its own light - and never the wave field, which belongs to the composite's camera");
    Require(water.find("inline renderer::underwater::Field UnderwaterFieldAt(float cameraX,float cameraY,")
                != std::string::npos
                && composite.find("watereffect::UnderwaterFieldAt(frameCtx.cameraPosition.x,") != std::string::npos
                && composite.find("frameCtx.cameraPosition.y, frameCtx.cameraValid);") != std::string::npos,
            "the composite builds the wave field for the very frame context it reconstructs against");
    // The medium is the water's own numbers, and the three colours go in the order MediumFor
    // declares them - absorption rate, deep colour, tint. All three are the same type, so a
    // swapped pair compiles and quietly paints every ocean with its own absorption curve; the
    // declaration and the call site are pinned against each other for exactly that reason.
    Require(water.find("return renderer::underwater::MediumFor(clarityPercent*zoneClarity,absorptionStrength,absorb,deep,tint,")
                != std::string::npos
                && water.find("depthEnabled);") != std::string::npos
                && water.find("out.absorb[0]=byte(absorptionColour,16,128.f);") != std::string::npos
                && water.find("out.deep[0]=byte(deepColour,16,255.f);") != std::string::npos
                && water.find("out.tint[0]=byte(waterColourTint,16,128.f);") != std::string::npos,
            "the medium is built from the water's own clarity, depth and authored colours, per zone like the rest");
    Require(core.find("inline Medium MediumFor(float clarityPercent, float absorptionPercent, const Vec3& absorbRegister,")
                != std::string::npos
                && core.find("const Vec3& deepRegister, const Vec3& tintRegister, bool depthEnabled) noexcept")
                    != std::string::npos,
            "MediumFor's declared colour order is absorption rate, deep colour, tint");
    Require(water.find("There is no second clarity for the underwater view") != std::string::npos
                && pass.find("read(L\"ClarityPercent\"") == std::string::npos,
            "there is one clarity, authored by WATER CLARITY, and [Underwater] cannot override it");
    Require(water.find("AdvanceUnderwater(dt);") != std::string::npos
                && water.find("submersionTracker.Advance(dt,frame.cameraValid?camera:nullptr,frame.cameraValid,coveringPlaneInView,waveBandYards);")
                    != std::string::npos
                && water.find("submersionTracker.BeginFrame();") != std::string::npos,
            "the frame's answer is settled at Present, after every liquid draw has reported");
    // The jump has to be visible while the published pose is not. The draws' own camera is
    // published per frame from a MAIN-TARGET, world-aligned draw (a reflection pass's camera
    // is mirrored about the plane and a tile-local anchor is not a world place at all), the
    // tracker is told before it advances, and the periodic line counts the jumps it made.
    Require(water.find("if(mainTarget&&anchor.valid&&anchor.worldAligned&&mainDrawPlaceFrame!=waveFrame.frameIndex) {")
                != std::string::npos
                && water.find("submersionTracker.NoteDrawPlace(mainDrawPlaceX,mainDrawPlaceY,renderer::PerformanceSeconds());")
                    != std::string::npos
                && water.find("if(mainDrawPlaceValid&&mainDrawPlaceFrame==frame.frameIndex) {")
                    != std::string::npos
                && water.find("mainDrawPlaceValid=false;") != std::string::npos
                && water.find("uwJumps=") != std::string::npos,
            "the tracker is told where this frame's own draws came from, and only from draws aimed at the player's view");
    // The outside-evidence exit spends the client's own frustum: the tracker
    // may retire a remembered plane only while the covering region was in
    // view, the test itself is the tracker header's CoveringPlaneInView (the
    // fixtures above unit-test its math), and a frame that cannot test the
    // frustum hands over no evidence at all. The tracker restarts the count
    // on any frame that does not count, so a long swim cannot accrue the
    // exit one glance at a time.
    Require(core.find("if (sawOutside && coveringPlaneInView) outsideSeconds += dt;")
                != std::string::npos
                && water.find("renderer::underwater::CoveringPlaneInView(&frame.viewRaw.m[0][0],camera,")
                    != std::string::npos
                && water.find("frame.cameraEyeAgeMs<=renderer::FrameContext::kCameraEyeHoldMs")
                    != std::string::npos,
            "the remembered plane retires only on frames that could have seen the sheet over the camera");
    // The ramp and the hysteresis spend the waves' reach at the camera: the caller
    // evaluates the displaced trains' own phase at the swimmer (times the grouping
    // envelope's worst case, floored at zero and capped by the tracker), so the medium
    // is measured against the visible surface and still water is band zero.
    // The reach is admitted in proportion to how deep the eye already is, so a crest can
    // never lift a camera that is above the mean plane into the medium (the ground-slope
    // report) and still holds a swimmer through a swell.
    Require(core.find("const float inWater = std::clamp(yards / kEnterYards, 0.f, 1.f);") != std::string::npos
                && core.find("const float submergence = yards + band * inWater;") != std::string::npos
                && core.find("kMaxWaveBandYards") != std::string::npos
                && water.find("renderer::water::SwellTheta(train,0.f,0.f,field.swellPhase)")
                    != std::string::npos
                && water.find("renderer::water::BuildGeometryWaves(geometryWavePercent,")
                    != std::string::npos
                && water.find("renderer::underwater::Tracker::kMaxWaveBandYards")
                    != std::string::npos,
            "the crossfade is measured against the visible surface, and still water is band zero");
    // Which pass a draw belongs to is the render target's fact, and a tile's water level may
    // live in its vertices rather than its matrix: both are plumbed in explicitly, because
    // neither can be derived inside the draw. The composite publishes the surface the
    // player's own view is drawn into every frame it prepares one; every draw detour hands
    // its Scope the parameters that name the draw's first and last consumed vertices; and
    // the cache of vertices read from client buffers is dropped at Reset, because holding a reference to
    // a default-pool client buffer through a reset fails the client's own reset.
    Require(composite.find("watereffect::mainTargetSurface = target.Get();") != std::string::npos
                && composite.find("watereffect::mainTargetSurface = nullptr;") != std::string::npos
                && rendererCpp.find("watereffect::SetNextDrawHint(false,0,start,0,") != std::string::npos
                && rendererCpp.find("watereffect::SetNextDrawHint(true,base,0,start,") != std::string::npos
                && rendererCpp.find("watereffect::SetNextDrawHint(false,0,0,0,v,nullptr,") != std::string::npos
                && rendererCpp.find("watereffect::SetNextDrawHint(true,0,0,0,v,indices,f,stride,t,n);") != std::string::npos
                && water.find("ClearLiquidVertexZCache();") != std::string::npos
                && water.find("D3DLOCK_READONLY") != std::string::npos,
            "the main target is published per frame, every detour names its draw in full (the primitive locates the last vertex), and the vertex cache dies at Reset");
    Require(water.find("if(!underwaterFrame.published) {") != std::string::npos
                && water.find("underwaterFrame.medium=UnderwaterMediumForFrame(ResolveWaterColourSet(frame.environment.water),")
                    != std::string::npos
                && water.find("zoneWavesEnabled?frame.environment[renderer::WaterClarity]:1.f);")
                    != std::string::npos,
            "a frame that drew no liquid still gets a current medium, resolved from that frame's own zone colours");
    // ... and a current up axis and light, for the same reason. A diver looking at the
    // seabed draws no liquid at all - the surface is behind the camera and frustum-culled - so
    // that is the most common underwater view there is, and holding the last publication for it
    // left the beams, the sky glow and the bed's facing test on the axis the camera was looking
    // along when it last saw water. Both have per-frame sources: world +Z in view space is a
    // row of this frame's captured view, and the source the surface is lit by is
    // CelestialTracker's confirmed disc, a singleton rather than a per-draw constant. Only the
    // light's colour is per-draw (c35), so only that one is held.
    Require(water.find("for(int i=0;i<3;++i)underwaterFrame.up[i]=frame.viewRaw.m[2][i];") != std::string::npos
                && water.find("renderer::CelestialTracker::Instance().Sun();") != std::string::npos
                && water.find("underwaterFrame.sunElevation=underwaterFrame.lightDir[0]*underwaterFrame.up[0]")
                    != std::string::npos,
            "a frame that drew no liquid still gets this frame's up axis and this frame's light");
    // The wave field is no longer rebuilt or published anywhere near Present at all: the
    // composite builds it (pinned above), which is both why the net never freezes on a diver
    // who sees no surface AND why it never crawls. The phases carry an anchor camera's place
    // in the lattice, and the pass reconstructs world positions relative to the frame
    // context's camera, so the two must be the same camera - a field rebuilt at the previous
    // frame's Present was the previous frame's camera, and the net, the shafts and the silt
    // all slid by a frame's worth of swimming, visibly, whenever the camera moved or turned.
    // There must be exactly one field construction, at composite, and the anchor inside it
    // must be the camera it was handed.
    Require(water.find("const bool anchorValid=cameraValid||LastWaveAnchorFresh();") != std::string::npos
                && water.find("const float anchorX=cameraValid?cameraX:lastWaveAnchor.camera[0];")
                != std::string::npos
                && water.find("waveMotion.Build(anchorX,anchorY,anchorValid,wavesEnabled);") != std::string::npos
                && water.find("field.finePhase[0]=wave.ripplePhase[0]*2.f;") != std::string::npos
                && water.find("field.finePhase[1]=wave.ripplePhase[1]*2.f;") != std::string::npos
                && water.find("underwaterFrame.field") == std::string::npos
                // Round 13: the sea's pivot is chosen by identity (this frame's own liquid
                // draw, else a camera captured this frame) rather than by a clock, and a
                // pivot that lands somewhere else settles the pending heading instead of
                // easing it across the jump. The medium's own anchor rule above is unchanged.
                && water.find("const bool pivotValid=SeaPivotForFrame(pivotX,pivotY,pivotSource);")
                    != std::string::npos
                && water.find("lastWaveAnchorFrame==frame.frameIndex") != std::string::npos
                && water.find("waveMotion.Settle(frame.wind,waveScalePercent);") != std::string::npos,
            "the field is built once, at composite, from the motion's own clock, anchored on the camera it is handed (or a fresh local draw anchor)");
    // World +Z in view space is ROW 2 of a row-vector matrix. That is what AnchorFromModelView
    // documents ("world axis i arrives in view space as row i") and what the renderer's
    // world-space helpers use; PlaneOverCamera leans on the same convention one step
    // further in, taking the draw's own rows as the liquid grid's axes in view space, which
    // the reading-under-every-rotation tests above prove. Reading the components ACROSS the rows
    // yields the view axes expressed in world coordinates and applies the camera's rotation twice
    // over - the mistake that comment names, and the one the fallback up axis made on the rare
    // draw with no rigid basis of its own, handing it to the pixel stage's sky gradient and to
    // the medium published below.
    Require(water.find("anchor.up[0]=viewValid?waveFrame.viewRaw.m[2][0]:0.f;") != std::string::npos
                && water.find("anchor.up[1]=viewValid?waveFrame.viewRaw.m[2][1]:0.f;") != std::string::npos
                && water.find("anchor.up[2]=viewValid?waveFrame.viewRaw.m[2][2]:1.f;") != std::string::npos
                && water.find("viewRaw.m[0][2]") == std::string::npos
                && water.find("viewRaw.m[1][2]") == std::string::npos,
            "the fallback up axis reads the row, on the convention the rest of the tree uses");

    // ---- tuning is reachable from both places a player sets it ----
    {
        const auto section = ini.find("[Underwater]");
        Require(section != std::string::npos, "the shipped ini has an [Underwater] section");
        const std::string keys = ini.substr(section, ini.find("\n[", section + 1) - section);
        for (const char* key : { "Enabled=1", "StrengthPercent=100", "CausticPercent=55", "ShaftPercent=60",
                                 "SurfaceGlowPercent=55", "WobblePercent=50", "DebugMode=0",
                                  "TurbidityPercent=100" })
            Require(keys.find(key) != std::string::npos,
                    std::string("the shipped ini asks for the default ") + key + " in [Underwater]");
    }
    Require(overlay.find("L\"Underwater\",L\"Enabled\",\"UNDERWATER\",0,1,1,1}") != std::string::npos
                && overlay.find("L\"Underwater\",L\"StrengthPercent\",\"UNDERWATER MEDIUM\",0,100,5,100}")
                    != std::string::npos
                && overlay.find("L\"Underwater\",L\"CausticPercent\",\"UNDERWATER CAUSTICS\",0,100,5,55}")
                    != std::string::npos
                && overlay.find("L\"Underwater\",L\"ShaftPercent\",\"UNDERWATER LIGHT SHAFTS\",0,100,5,60}")
                    != std::string::npos
                && overlay.find("L\"Underwater\",L\"SurfaceGlowPercent\",\"UNDERWATER SURFACE GLOW\",0,100,5,55}")
                    != std::string::npos
                && overlay.find("L\"Underwater\",L\"TurbidityPercent\",\"UNDERWATER DIRT\",0,300,5,100}")
                       != std::string::npos
                && pass.find("read(L\"TurbidityPercent\", 100)") != std::string::npos
                && overlay.find("L\"Underwater\",L\"WobblePercent\",\"UNDERWATER WOBBLE\",0,100,5,50}")
                    != std::string::npos
                && overlay.find("L\"Underwater\",L\"DebugMode\",\"UNDERWATER DEBUG\",0,4,1,0}") != std::string::npos,
            "the medium is reachable from F7, bounded and defaulted by the same numbers the ini ships");
    Require(overlay.find("--- UNDERWATER") == std::string::npos,
            "the underwater controls live in the WATER tab, because they are the same water");
    Require(pass.find("read(L\"StrengthPercent\", 100)") != std::string::npos
                && pass.find("read(L\"CausticPercent\", 55)") != std::string::npos
                && pass.find("read(L\"ShaftPercent\", 60)") != std::string::npos
                && pass.find("read(L\"SurfaceGlowPercent\", 55)") != std::string::npos
                && pass.find("read(L\"WobblePercent\", 50)") != std::string::npos
                && pass.find("std::clamp(read(L\"DebugMode\", 0), 0, 4)") != std::string::npos,
            "the pass resolves the same keys with the same defaults and the same bounds");
    Require(pass.find("return m_settings.enabled && m_settings.strength > 0.f;") != std::string::npos
                && pass.find("settings.enabled = read(L\"Enabled\", 1) != 0;") != std::string::npos,
            "UNDERWATER off means the medium is off, and nothing else about the water changes");
}

} // namespace

int main() try
{
    TestPlaneReading();
    TestTracker();
    TestCoveringPlaneInView();
    TestWorldAnchoring();
    TestMedium();
    TestShadePixel();
    TestCaustics();
    TestShaftsAndWobble();
    TestMirror();
    std::printf("\nALL UNDERWATER TESTS PASSED\n");
    return 0;
}
catch (const std::exception& error)
{
    std::printf("FAIL %s\n", error.what());
    return 1;
}
