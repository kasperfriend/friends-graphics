// CPU preview of the wind-driven vegetation sway.
//
// The client applies the sway to the bone matrices it uploads for a model draw,
// so there is no shader here to mirror: this tool drives the SAME code the DLL
// runs - src/Environment/WindSystem.h for the shared world wind and
// src/Environment/VegetationWind.h for the sway policy - over a synthetic scene
// of grass tufts, bushes and trees, and renders the result on the CPU. A frame
// from this tool is therefore what the client is asked to draw for that wind
// strength, plant size and camera, which is what makes it useful for tuning and
// for recording what a change did.
//
// The bone transform is built exactly as the DLL builds it (see
// src/Lighting/LocalLightManager.cpp and the regression suite): the three
// registers are the model -> view matrix, its basis is the transpose of the
// camera's rotation and its translation column is the bone's view position.
// BuildVegetationLean() then rotates that matrix about the point where the model
// meets the ground, and ApplyVegetationLean() hands back the leaned matrix.
//
// Build:  g++ -O2 -std=c++20 tools/VegetationPreview.cpp -o build/vegetationpreview
// Run:    build/vegetationpreview --out Screenshots/vegetation-wind.png --grid 2
//         build/vegetationpreview --out build/calm.png --strength 0
//         build/vegetationpreview --out build/storm.png --strength 300 --stats
// --grid 2 draws one labelled sheet: 480x270 frames under a 20px settings bar,
// 960x580 in total, the same panel geometry as the published cloud and water
// sheets. The four panels differ only in shared wind strength.
//
// The scene is deliberately simple - a ground plane, a few plants and one sun -
// because the point of the sheet is the lean, not the model: a reader has to be
// able to measure the tip travel against the trunk.

#include "../src/Core/MathTypes.h"
#include "../src/Environment/VegetationWind.h"
#include "../src/Environment/WindSystem.h"
#include "PreviewSheet.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace renderer;

namespace
{
// ---------------------------------------------------------------- helpers ---
inline float Saturate(float value) { return value < 0.f ? 0.f : value > 1.f ? 1.f : value; }
inline Vec3 Mix(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline Vec3 Scale(const Vec3& value, float s) { return value * s; }

// A deterministic, seedable value in [0,1): the scene must be identical on every
// run so a published sheet stays reproducible.
inline float Noise(uint64_t seed, uint32_t salt = 0u)
{
    return float(double(VegetationHash64(seed ^ (uint64_t(salt) << 32)) & 0xffffffull) / double(0x1000000));
}

// -------------------------------------------------------------- the camera ---
// WoW map axes: +X north, +Y west, +Z up. The basis matches the DLL's rows:
// view.m[row][column] is the component of view axis `column` along world axis
// `row`, so a world direction becomes view as TransformDirectionRowVector().
struct PreviewCamera
{
    Vec3 position;
    float view[4][4]{};
    float focal = 1.f;

    PreviewCamera(const Vec3& eye, float yawDegrees, float pitchDegrees, float verticalFov, int pixelHeight)
    {
        position = eye;
        const double yaw = double(yawDegrees) * 0.017453292519943295;
        const double pitch = double(pitchDegrees) * 0.017453292519943295;
        const Vec3 forward = { float(std::cos(yaw) * std::cos(pitch)), float(-std::sin(yaw) * std::cos(pitch)),
            float(std::sin(pitch)) };
        const Vec3 worldUp = { 0.f, 0.f, 1.f };
        const Vec3 right = Normalize(Cross(forward, worldUp));
        const Vec3 up = Cross(right, forward);
        const Vec3 axes[3] = { right, up, forward };
        // The rows the client uploads: view.m[i][j] is component i of view axis
        // j, which is what TransformDirectionRowVector() expects (it computes
        // out_j = sum_i direction_i * matrix[i][j], i.e. the axis dots).
        for (int column = 0; column < 3; ++column)
        {
            const Vec3& axis = axes[column];
            view[0][column] = axis.x;
            view[1][column] = axis.y;
            view[2][column] = axis.z;
            view[3][column] = -(position.x * axis.x + position.y * axis.y + position.z * axis.z);
        }
        view[3][3] = 1.f;
        focal = float(0.5 * double(pixelHeight) / std::tan(double(verticalFov) * 0.5 * 0.017453292519943295));
    }

    // The view basis itself, assembled from the matrix the client uploads:
    // view axis `column` occupies column `column`, i.e. its components are
    // matrix[0..2][column].
    Vec3 Axis(int column) const { return { view[0][column], view[1][column], view[2][column] }; }
    Vec3 ToView(const Vec3& world) const
    {
        return TransformDirectionRowVector(world - position, view);
    }
    Vec3 DirectionToView(const Vec3& direction) const
    {
        return TransformDirectionRowVector(direction, view);
    }
    Vec3 ToWorld(const Vec3& viewPosition) const
    {
        float out[3]{};
        ViewPositionToWorld(&view[0][0], &position.x, &viewPosition.x, out);
        return { out[0], out[1], out[2] };
    }
    void Project(const Vec3& viewPosition, int width, int height, float& x, float& y, float& depth) const
    {
        depth = viewPosition.z;
        x = float(width) * 0.5f + focal * (viewPosition.x / depth);
        y = float(height) * 0.5f - focal * (viewPosition.y / depth);
    }
};

// The bone a client draw uploads for a model standing at `world` with no
// rotation of its own - the same construction the regression suite checks.
void ModelBone(const PreviewCamera& camera, const Vec3& world, float bone[3][4])
{
    const Vec3 translation = camera.ToView(world);
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column) bone[row][column] = camera.view[column][row];
        bone[row][3] = row == 0 ? translation.x : row == 1 ? translation.y : translation.z;
    }
}

// ------------------------------------------------------------- the scene -----
struct Stem
{
    std::vector<Vec3> points;      // model space, from the ground up
    std::vector<float> widths;     // optional per-point width multiplier (blades taper)
    float radius = .02f;
    Vec3 root{ .16f, .30f, .10f }, tip{ .30f, .62f, .18f };
};
struct Blob
{
    Vec3 center;
    float radius = 1.f;
    Vec3 color;
};
struct Plant
{
    std::string name;
    std::string kind; // "tuft", "bush", "tree"
    Vec3 world;
    float height = 1.f;
    float footprint = .5f;
    std::vector<Stem> stems;
    std::vector<Blob> blobs;
};

Plant MakeTuft(const Vec3& world, float height, uint64_t seed)
{
    Plant plant;
    plant.name = "tuft";
    plant.kind = "tuft";
    plant.world = world;
    plant.height = height;
    plant.footprint = .35f;
    // A clump, not one blade: the client draws grass as dense clusters, and a
    // single blade would be a fraction of a pixel at any readable distance.
    const int blades = 26;
    for (int blade = 0; blade < blades; ++blade)
    {
        const uint64_t s = seed * 977u + uint64_t(blade);
        const float angle = Noise(s, 1u) * 6.2831853f;
        const float lean = .25f + Noise(s, 2u) * .35f;
        const float bladeHeight = height * (.45f + Noise(s, 3u) * .55f);
        const float spread = height * .18f * std::sqrt(Noise(s, 8u));
        Stem stem;
        stem.radius = height * .060f;
        stem.widths.push_back(height * .11f);
        stem.widths.push_back(height * .085f);
        stem.widths.push_back(height * .055f);
        stem.widths.push_back(height * .024f);
        const Vec3 direction = { std::cos(angle) * lean, std::sin(angle) * lean, 1.f };
        const float sway = (Noise(s, 6u) - .5f) * .3f;
        const Vec3 base = { std::cos(angle) * spread, std::sin(angle) * spread, 0.f };
        for (int step = 0; step <= 8; ++step)
        {
            const float t = float(step) / 8.f;
            // A blade bends more near the tip, which is what the wind leans.
            const float bend = t * t;
            const float curve = std::sin(t * 2.1f + Noise(s, 7u) * 3.f) * sway * t;
            stem.points.push_back({ base.x + direction.x * bend * bladeHeight + curve * .35f,
                base.y + direction.y * bend * bladeHeight + curve * .35f, direction.z * t * bladeHeight });
        }
        stem.root = { .11f, .23f, .06f };
        stem.tip = { .46f, .74f, .19f };
        plant.stems.push_back(std::move(stem));
    }
    return plant;
}

Plant MakeBush(const Vec3& world, float height, uint64_t seed)
{
    Plant plant;
    plant.name = "bush";
    plant.kind = "bush";
    plant.world = world;
    plant.height = height;
    plant.footprint = height * .85f;
    const int stems = 6;
    for (int index = 0; index < stems; ++index)
    {
        const uint64_t s = seed * 613u + uint64_t(index);
        const float angle = 6.2831853f * float(index) / float(stems) + Noise(s, 1u) * .8f;
        const float lean = .18f + Noise(s, 2u) * .2f;
        Stem stem;
        stem.radius = height * .045f;
        const Vec3 direction = { std::cos(angle) * lean, std::sin(angle) * lean, 1.f };
        for (int step = 0; step <= 4; ++step)
        {
            const float t = float(step) / 4.f;
            stem.points.push_back({ direction.x * t * t * height, direction.y * t * t * height, direction.z * t * height });
        }
        stem.root = { .14f, .22f, .08f };
        stem.tip = { .24f, .40f, .12f };
        plant.stems.push_back(std::move(stem));
    }
    for (int index = 0; index < 12; ++index)
    {
        const uint64_t s = seed * 911u + uint64_t(index);
        const float angle = 6.2831853f * float(index) / 12.f + .4f;
        const float radius = height * (.17f + Noise(s, 3u) * .12f);
        const float rise = height * (.38f + Noise(s, 4u) * .55f);
        plant.blobs.push_back({ { std::cos(angle) * height * .28f, std::sin(angle) * height * .28f, rise },
            radius, { .20f + Noise(s, 5u) * .07f, .34f + Noise(s, 6u) * .12f, .13f } });
    }
    return plant;
}

Plant MakeTree(const Vec3& world, float height, float footprint, uint64_t seed)
{
    Plant plant;
    plant.name = "tree";
    plant.kind = "tree";
    plant.world = world;
    plant.height = height;
    plant.footprint = footprint;
    // The trunk, in four tapering segments: a rigid lean of the whole model has
    // to keep these welded, which is what the sheet is meant to show.
    Stem trunk;
    trunk.radius = height * .028f;
    trunk.root = { .19f, .13f, .07f };
    trunk.tip = { .26f, .22f, .11f };
    for (int step = 0; step <= 6; ++step)
    {
        const float t = float(step) / 6.f;
        trunk.points.push_back({ (Noise(seed, 1u) - .5f) * .3f * t, (Noise(seed, 2u) - .5f) * .3f * t, t * height * .5f });
    }
    plant.stems.push_back(std::move(trunk));
    for (int branch = 0; branch < 3; ++branch)
    {
        const uint64_t s = seed * 317u + uint64_t(branch);
        const float angle = 6.2831853f * float(branch) / 3.f + Noise(s, 1u) * .9f;
        Stem stem;
        stem.radius = height * .012f;
        stem.root = trunk.tip;
        stem.tip = trunk.tip;
        const Vec3 from = { 0.f, 0.f, height * .38f };
        const Vec3 to = { std::cos(angle) * footprint * .5f, std::sin(angle) * footprint * .5f, height * .78f };
        for (int step = 0; step <= 3; ++step)
        {
            const float t = float(step) / 3.f;
            stem.points.push_back(Mix(from, to, t));
        }
        plant.stems.push_back(std::move(stem));
    }
    for (int index = 0; index < 16; ++index)
    {
        const uint64_t s = seed * 577u + uint64_t(index);
        const float angle = 6.2831853f * float(index) / 16.f + .7f;
        const float ring = .18f + Noise(s, 6u) * .34f;
        const float radius = footprint * (.11f + Noise(s, 2u) * .07f);
        const float rise = height * (.58f + Noise(s, 3u) * .42f);
        plant.blobs.push_back({ { std::cos(angle) * footprint * ring, std::sin(angle) * footprint * ring, rise },
            radius, { .16f + Noise(s, 4u) * .07f, .30f + Noise(s, 5u) * .16f, .11f } });
    }
    return plant;
}

// ----------------------------------------------------------- the renderer ----
struct Frame
{
    int width = 0, height = 0;
    std::vector<Vec3> color;
    std::vector<float> depth;

    Frame(int w, int h) : width(w), height(h), color(size_t(w) * size_t(h)), depth(size_t(w) * size_t(h), 1e30f) {}
};

constexpr float kNear = .25f;

// One thick, antialiased screen-space segment with perspective-correct depth.
void DrawSegment(Frame& frame, const PreviewCamera& camera, const float bone[3][4],
    const Vec3& from, const Vec3& to, float radius, float radiusScaleFrom, float radiusScaleTo,
    const Vec3& rootColor, const Vec3& tipColor)
{
    // Model space -> view through the (already leaned) bone.
    const auto transform = [&bone](const Vec3& p) {
        return Vec3{ bone[0][0] * p.x + bone[0][1] * p.y + bone[0][2] * p.z + bone[0][3],
            bone[1][0] * p.x + bone[1][1] * p.y + bone[1][2] * p.z + bone[1][3],
            bone[2][0] * p.x + bone[2][1] * p.y + bone[2][2] * p.z + bone[2][3] };
    };
    // Screen-space thickness follows the model-space thickness: perspective
    // shrinks it, so a blade tapers the way the real one does.
    const float modelHalfWidth = radius * .5f * (radiusScaleFrom + radiusScaleTo);
    radius = modelHalfWidth;
    Vec3 a = transform(from), b = transform(to);
    if (a.z <= kNear && b.z <= kNear) return;
    if (a.z <= kNear) a = Mix(a, b, (kNear - a.z) / (b.z - a.z) + .001f);
    if (b.z <= kNear) b = Mix(b, a, (kNear - b.z) / (a.z - b.z) + .001f);

    float ax, ay, az, bx, by, bz;
    camera.Project(a, frame.width, frame.height, ax, ay, az);
    camera.Project(b, frame.width, frame.height, bx, by, bz);
    const float halfWidthA = std::max(0.5f, camera.focal * radius / az);
    const float halfWidthB = std::max(0.5f, camera.focal * radius / bz);
    // A blade tapers: the widest end of the segment is the wider of the two.
    const float halfWidth = std::max(halfWidthA, halfWidthB);

    const int x0 = int(std::floor(std::min(ax, bx) - halfWidth - 1.f));
    const int x1 = int(std::ceil(std::max(ax, bx) + halfWidth + 1.f));
    const int y0 = int(std::floor(std::min(ay, by) - halfWidth - 1.f));
    const int y1 = int(std::ceil(std::max(ay, by) + halfWidth + 1.f));
    const float dx = bx - ax, dy = by - ay;
    const float lengthSquared = dx * dx + dy * dy;
    for (int y = std::max(0, y0); y <= std::min(frame.height - 1, y1); ++y)
    {
        for (int x = std::max(0, x0); x <= std::min(frame.width - 1, x1); ++x)
        {
            const float px = float(x) + .5f, py = float(y) + .5f;
            float t = lengthSquared > 1e-6f ? ((px - ax) * dx + (py - ay) * dy) / lengthSquared : 0.f;
            t = Saturate(t);
            const float cx = ax + dx * t, cy = ay + dy * t;
            const float distance = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
            const float width = halfWidthA + (halfWidthB - halfWidthA) * t;
            const float coverage = Saturate(width + .5f - distance);
            if (coverage <= 0.f) continue;
            // Perspective-correct depth along the segment.
            const float inverse = (1.f - t) / az + t / bz;
            const float depth = inverse > 1e-9f ? 1.f / inverse : az;
            const Vec3 color = Mix(Mix(rootColor, tipColor, t), Mix(rootColor, tipColor, t), 1.f) * (0.85f + .3f * (1.f - t));
            const size_t at = size_t(y) * size_t(frame.width) + size_t(x);
            if (depth >= frame.depth[at]) continue;
            frame.depth[at] = depth;
            frame.color[at] = Mix(frame.color[at], color, coverage);
        }
    }
}

void DrawBlob(Frame& frame, const PreviewCamera& camera, const Vec3& centerView, float radius, const Vec3& color,
    const Vec3& lightView, float& nearest, float& deepest)
{
    if (centerView.z <= kNear) return;
    float cx, cy, cz;
    camera.Project(centerView, frame.width, frame.height, cx, cy, cz);
    const float pixels = camera.focal * radius / cz;
    if (pixels < .3f) return;
    const int x0 = std::max(0, int(cx - pixels) - 1), x1 = std::min(frame.width - 1, int(cx + pixels) + 1);
    const int y0 = std::max(0, int(cy - pixels) - 1), y1 = std::min(frame.height - 1, int(cy + pixels) + 1);
    for (int y = y0; y <= y1; ++y)
    {
        for (int x = x0; x <= x1; ++x)
        {
            const float nx = (float(x) + .5f - cx) / pixels, ny = (float(y) + .5f - cy) / pixels;
            const float rr = nx * nx + ny * ny;
            if (rr > 1.f) continue;
            const float coverage = Saturate((1.f - std::sqrt(rr)) * pixels + .5f);
            if (coverage <= 0.f) continue;
            const Vec3 normalLocal = { nx, -ny, std::sqrt(std::max(0.f, 1.f - rr)) };
            const Vec3 normalView = TransformDirectionRowVector(normalLocal, camera.view);
            const float lambert = std::max(0.f, Dot(normalView, lightView));
            const float depth = cz - radius * std::sqrt(std::max(0.f, 1.f - rr));
            const size_t at = size_t(y) * size_t(frame.width) + size_t(x);
            if (depth >= frame.depth[at]) continue;
            frame.depth[at] = depth;
            const Vec3 shaded = Scale(color, .35f + .85f * lambert);
            frame.color[at] = Mix(frame.color[at], shaded, coverage * .98f);
            nearest = std::min(nearest, depth);
            deepest = std::max(deepest, depth);
        }
    }
}

// ------------------------------------------------------------ the picture ----
struct Panel
{
    Frame frame;
    std::vector<Plant> plants;
    float meanAngle = 0.f;
    float maxAngle = 0.f;
    float treeAngle = 0.f;
    float tuftAngle = 0.f;
    float strength = 0.f;
    float velocity = 0.f;
};

void RenderPanel(Panel& panel, const PreviewCamera& camera, const VegetationWind& sway,
    const WindState& windState, int width, int height)
{
    Frame& frame = panel.frame;
    const Vec3 worldUp = { 0.f, 0.f, 1.f };
    const Vec3 forward = camera.Axis(2), right = camera.Axis(0), up = camera.Axis(1);
    // Sky above the horizon, a ground plane below it. The rays are per pixel so
    // the horizon is exactly where the camera's pitch puts it.
    const Vec3 zenith = { .22f, .40f, .68f }, horizon = { .62f, .72f, .82f };
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const float px = (float(x) + .5f - float(width) * .5f) / camera.focal;
            const float py = (float(height) * .5f - (float(y) + .5f)) / camera.focal;
            const Vec3 ray = Normalize(forward + right * px + up * py);
            const size_t at = size_t(y) * size_t(width) + size_t(x);
            float distance = 1e30f;
            Vec3 color;
            if (ray.z < -1e-4f)
            {
                const float t = -camera.position.z / ray.z;
                distance = t;
                const Vec3 hit = camera.position + ray * t;
                const float variation = .90f + .20f * VegetationField(hit.x * .35f, hit.y * .35f, 0x51a3u);
                const Vec3 ground = { .22f * variation, .32f * variation, .14f * variation };
                color = Mix(ground, horizon, 1.f - std::exp(-t / 150.f));
            }
            else
            {
                color = Mix(horizon, zenith, Saturate(ray.z));
            }
            frame.color[at] = color;
            frame.depth[at] = distance;
        }
    }

    const Vec3 lightWorld = Normalize(Vec3{ -.42f, .30f, .85f });
    const Vec3 lightView = camera.DirectionToView(lightWorld);

    for (Plant& plant : panel.plants)
    {
        const float distance = Length(plant.world - camera.position);
        const auto sample = sway.Sample(plant.world.x, plant.world.y, plant.height, plant.world.x * 7919.f + plant.world.y,
            distance);
        const float angle = sample.Leaning() ? sample.angleDegrees : 0.f;
        panel.meanAngle += angle;
        panel.maxAngle = std::max(panel.maxAngle, angle);
        if (plant.kind == "tree") panel.treeAngle = std::max(panel.treeAngle, angle);
        if (plant.kind == "tuft") panel.tuftAngle = std::max(panel.tuftAngle, angle);
        if (sway.Settings().debugView)
            std::printf("  %-5s at %.0f,%.0f  h %.2f  lean %.2f deg  gust %+.2f\n", plant.kind.c_str(), plant.world.x,
                plant.world.y, plant.height, angle, sample.gust);

        const Vec3 pivot = camera.ToView(plant.world);
        const Vec3 upView = camera.DirectionToView(worldUp);
        const Vec3 directionView = camera.DirectionToView({ sample.directionX, sample.directionY, 0.f });
        const auto lean = BuildVegetationLean(&pivot.x, &upView.x, &directionView.x, angle);
        float bone[3][4];
        ModelBone(camera, plant.world, bone);
        float leaned[3][4];
        ApplyVegetationLean(lean, bone, leaned);
        const float* active = sample.Leaning() ? &leaned[0][0] : &bone[0][0];
        const auto block = [active](int row, int column) { return active[row * 4 + column]; };
        float applied[3][4];
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column) applied[row][column] = block(row, column);

        const float fog = 1.f - std::exp(-distance / 150.f);
        for (const Stem& stem : plant.stems)
        {
            const Vec3 rootColor = Mix(stem.root, horizon, fog * .35f);
            const Vec3 tipColor = Mix(stem.tip, horizon, fog * .35f);
            const auto widthAt = [&stem](size_t index) {
                if (stem.widths.empty()) return 1.f;
                return stem.widths[std::min(index, stem.widths.size() - 1)];
            };
            for (size_t index = 0; index + 1 < stem.points.size(); ++index)
                DrawSegment(frame, camera, applied, stem.points[index], stem.points[index + 1], stem.radius,
                    widthAt(index), widthAt(index + 1), rootColor, tipColor);
        }
        for (const Blob& blob : plant.blobs)
        {
            const Vec3 center = { applied[0][0] * blob.center.x + applied[0][1] * blob.center.y +
                    applied[0][2] * blob.center.z + applied[0][3],
                applied[1][0] * blob.center.x + applied[1][1] * blob.center.y + applied[1][2] * blob.center.z +
                    applied[1][3],
                applied[2][0] * blob.center.x + applied[2][1] * blob.center.y + applied[2][2] * blob.center.z +
                    applied[2][3] };
            float nearest = 1e30f, deepest = 0.f;
            DrawBlob(frame, camera, center, blob.radius, Mix(blob.color, horizon, fog * .35f), lightView, nearest, deepest);
        }
    }
    panel.meanAngle /= std::max<size_t>(1, panel.plants.size());
    panel.strength = windState.strength;
    panel.velocity = Length(Vec3{ windState.velocity.x, windState.velocity.y, 0.f });
}

// The camera is fitted to the near group only: a tree forty yards behind the
// row is there to show the range fade, not to shrink the framing of everything
// else.
constexpr size_t kFitCount = 8;

// The camera is fitted to the scene instead of hard-coded, so the sheet frames
// whatever the scene contains. The eye looks at the scene along a horizontal
// direction that is 35 degrees off the wind's lean: a lean seen edge-on reads as
// a plant getting shorter, which is exactly the measurement this sheet exists
// for, and the angle keeps the depth of the row readable at the same time.
struct Fit
{
    Vec3 eye;
    float yaw = 0.f, pitch = 0.f;
};

Fit FitCamera(const std::vector<Plant>& plants, const Vec3& leanDirection, int width, int height, float verticalFov,
    float yawOffset, bool manualYaw, float manualYawValue, bool manualPitch, float manualPitchValue)
{
    // The fit points are the plants: the ground contact of each one, its tip and
    // the width of its canopy. Fitting a bounding box instead would fill the
    // frame with the empty ground in front of the nearest plant.
    std::vector<Vec3> points;
    Vec3 low{ 1e30f, 1e30f, 0.f }, high{ -1e30f, -1e30f, 0.f };
    for (size_t index = 0; index < plants.size() && index < kFitCount; ++index)
    {
        const Plant& plant = plants[index];
        const float reach = std::max(plant.footprint * .5f, plant.height * .1f);
        points.push_back({ plant.world.x, plant.world.y, 0.f });
        points.push_back({ plant.world.x, plant.world.y, plant.height * 1.2f });
        for (int corner = 0; corner < 4; ++corner)
            points.push_back({ plant.world.x + ((corner & 1) ? reach : -reach),
                plant.world.y + ((corner & 2) ? reach : -reach), plant.height * .55f });
        low = { std::min(low.x, plant.world.x - reach), std::min(low.y, plant.world.y - reach), 0.f };
        high = { std::max(high.x, plant.world.x + reach), std::max(high.y, plant.world.y + reach), plant.height };
    }
    const Vec3 center = { (low.x + high.x) * .5f, (low.y + high.y) * .5f, (low.z + high.z) * .35f };
    const Vec3 lean = { leanDirection.x, leanDirection.y, 0.f };
    const Vec3 perp = { -lean.y, lean.x, 0.f };
    const float angle = .61f; // 35 degrees between the view direction and the lean
    Vec3 viewDirection = Normalize(lean * std::cos(angle) + perp * std::sin(angle) + Vec3{ 0.f, 0.f, -.16f });
    float yaw = manualYaw ? manualYawValue : float(std::atan2(-viewDirection.y, viewDirection.x) * 57.29577951308232);
    yaw += yawOffset;
    viewDirection = { float(std::cos(double(yaw) * 0.017453292519943295)),
        float(-std::sin(double(yaw) * 0.017453292519943295)), viewDirection.z * .5f };

    // Fit: the whole scene box has to be inside a comfortable frame, with room
    // for the tallest blade and the ground in front of the nearest plant. The
    // closest distance that manages it wins, so the plants fill the panel.
    const auto pitchFor = [&](const Vec3& eye, float) {
        if (manualPitch) return manualPitchValue;
        const float horizontal = std::sqrt((center.x - eye.x) * (center.x - eye.x) +
            (center.y - eye.y) * (center.y - eye.y) + (center.z - eye.z) * (center.z - eye.z));
        return float(std::asin(std::clamp((center.z - eye.z) / std::max(.001f, horizontal), -.9f, .9f)) * 57.29577951308232);
    };
    float distance = 6.f;
    Vec3 best = center - viewDirection * distance;
    float bestPitch = -3.f;
    bool fitted = false;
    for (int attempt = 0; attempt < 48; ++attempt, distance *= 1.12f)
    {
        const Vec3 eye = center - viewDirection * distance;
        const float pitch = pitchFor(eye, -3.f);
        PreviewCamera probe(eye, yaw, pitch, verticalFov, height);
        bool inside = true;
        for (const Vec3& point : points)
        {
            float x, y, depth;
            probe.Project(probe.ToView(point), width, height, x, y, depth);
            // The tops get a margin of their own so a leaning tree stays in
            // frame at the strongest wind the tool can draw.
            const bool top = point.z > high.z * .9f;
            if (depth < .7f || x < width * .07f || x > width * .93f ||
                y < height * (top ? .10f : .05f) || y > height * .94f)
            {
                inside = false;
                break;
            }
        }
        if (!fitted) { best = eye; bestPitch = pitch; }
        if (inside) { fitted = true; best = eye; bestPitch = pitch; break; }
    }
    return { best, yaw, bestPitch };
}

// The near group is what the camera is fitted to. The far tree is deliberately
// outside it: a tree forty yards behind the row is there to show the range fade,
// not to shrink the framing of everything else.
std::vector<Plant> BuildScene()
{
    return {
        MakeTuft({ 1.9f, -1.6f, 0.f }, .70f, 11u),
        MakeTuft({ 2.6f, .3f, 0.f }, .86f, 17u),
        MakeTuft({ 1.6f, 2.2f, 0.f }, .62f, 23u),
        MakeTuft({ 3.1f, 3.1f, 0.f }, .78f, 29u),
        MakeBush({ 6.0f, -3.2f, 0.f }, 1.9f, 31u),
        MakeBush({ 7.6f, 3.4f, 0.f }, 2.2f, 37u),
        MakeTree({ 11.5f, -3.4f, 0.f }, 8.0f, 3.4f, 41u),
        MakeTree({ 15.5f, 4.2f, 0.f }, 9.0f, 4.1f, 43u),
        MakeTree({ 55.0f, -11.0f, 0.f }, 11.5f, 5.0f, 47u),
    };
}

} // namespace

int main(int argc, char** argv)
{
    std::string out = "vegetation-preview.png";
    int width = 480, height = 270, grid = 1, supersample = 2;
    bool stats = false, debug = false, labels = true;
    float strengthPercent = -1.f;
    float directionDegrees = 270.f; // wind from the west; the plants lean east
    float gustPercentage = 45.f, flutterPercentage = 100.f, maxAngle = 20.f, distance = 120.f;
    float plantGain = 1.f, treeGain = 1.f;
    float yawOffset = 0.f, pitch = 0.f, fov = 52.f;
    bool manualYaw = false, manualPitch = false;

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        auto next = [&]() -> const char* { return (index + 1 < argc) ? argv[++index] : "0"; };
        if (argument == "--out") out = next();
        else if (argument == "--width") width = std::atoi(next());
        else if (argument == "--height") height = std::atoi(next());
        else if (argument == "--grid") grid = std::atoi(next());
        else if (argument == "--supersample") supersample = std::max(1, std::atoi(next()));
        else if (argument == "--strength") strengthPercent = float(std::atof(next()));
        else if (argument == "--direction") directionDegrees = float(std::atof(next()));
        else if (argument == "--gust") gustPercentage = float(std::atof(next()));
        else if (argument == "--flutter") flutterPercentage = float(std::atof(next()));
        else if (argument == "--max-angle") maxAngle = float(std::atof(next()));
        else if (argument == "--distance") distance = float(std::atof(next()));
        else if (argument == "--plant-gain") plantGain = float(std::atof(next()));
        else if (argument == "--tree-gain") treeGain = float(std::atof(next()));
        else if (argument == "--yaw") { yawOffset = float(std::atof(next())); manualYaw = true; }
        else if (argument == "--pitch") { pitch = float(std::atof(next())); manualPitch = true; }
        else if (argument == "--fov") fov = float(std::atof(next()));
        else if (argument == "--stats") stats = true;
        else if (argument == "--debug") debug = true;
        else if (argument == "--no-labels") labels = false;
        else if (argument == "--help")
        {
            std::printf("vegetationpreview --out FILE [--grid 2] [--strength PCT] [--direction DEG]\n"
                        "                  [--gust PCT] [--flutter PCT] [--max-angle DEG] [--distance YD]\n"
                        "                  [--plant-gain 0..200] [--tree-gain 0..200] [--yaw DEG] [--pitch DEG]\n"
                        "                  [--fov DEG] [--supersample N] [--stats] [--debug] [--no-labels]\n");
            return 0;
        }
    }

    // The panels: one wind strength each, the same instant in the field's life.
    // [Wind] StrengthPercent is bounded to 200% (the live strength reaches 300%
    // through the weather response), so the fourth panel is a storm: the same
    // 200% setting with rain pushing the target strength higher.
    struct Case { float percent; int weather; const char* name; };
    std::vector<Case> cases;
    if (strengthPercent >= 0.f) cases.push_back({ strengthPercent, 0, "frame" });
    else if (grid > 1) cases = { { 0.f, 0, "CALM" }, { 100.f, 0, "BREEZE" }, { 200.f, 0, "STRONG" },
        { 200.f, 1, "STORM" } };
    else cases.push_back({ 100.f, 0, "frame" });
    std::vector<float> strengths;
    for (const Case& item : cases) strengths.push_back(item.percent);

    const int columns = grid > 1 ? 2 : 1;
    const int rows = grid > 1 ? 2 : 1;
    const int bar = labels ? 20 : 0;
    const int sheetWidth = width * columns, sheetHeight = (height + bar) * rows;
    std::vector<uint8_t> sheet(size_t(sheetWidth) * size_t(sheetHeight) * 3, 0);

    // One camera for every panel, fitted from the wind the FIRST panel runs, so
    // the panels differ only in wind strength. The lean direction comes from the
    // sway itself, which is why the fit needs the vegetation configuration.
    std::vector<Plant> scene = BuildScene();
    const PreviewCamera reference = [&] {
        WindSystem referenceWind;
        WindSettings settings;
        settings.dynamic = false;
        settings.direction = WindDirection::West;
        settings.strength = 1.f;
        settings.weatherResponse = 0.f;
        referenceWind.Configure(settings);
        VegetationWind referenceSway;
        VegetationSettings vegetation;
        vegetation.gust = 0.f; // the fit follows the steady lean, not a gust
        referenceSway.Configure(vegetation);
        for (int frame = 0; frame < 60; ++frame) referenceSway.Update(referenceWind.Update({}, 1.f / 60.f));
        const auto sample = referenceSway.Sample(scene.front().world.x, scene.front().world.y, 1.f, 1u, 10.f);
        const Fit fit = FitCamera(scene, { sample.directionX, sample.directionY, 0.f }, width, height, fov,
            yawOffset, manualYaw, yawOffset, manualPitch, pitch);
        return PreviewCamera(fit.eye, fit.yaw, fit.pitch, fov, height * supersample);
    }();

    for (size_t panelIndex = 0; panelIndex < strengths.size(); ++panelIndex)
    {
        const float percent = strengths[panelIndex];
        // The shared wind, through the same simulation the DLL runs: one
        // direction, one strength, no second wind generator anywhere.
        WindSystem windSystem;
        WindSettings windSettings;
        windSettings.dynamic = false;
        // The compass bearing the wind comes FROM, the same convention as the
        // [Wind] Direction setting: 0 = north, 90 = east, 270 = west.
        const unsigned bearing = unsigned((directionDegrees + 22.5f) / 45.f) % 8u;
        windSettings.direction = static_cast<WindDirection>(bearing);
        windSettings.strength = percent * .01f;
        windSettings.gustAmount = gustPercentage * .01f;
        windSettings.weatherResponse = 1.f;
        windSystem.Configure(windSettings);

        VegetationSettings vegetation;
        vegetation.sway = 1.f;
        vegetation.gust = gustPercentage * .01f;
        vegetation.flutter = flutterPercentage * .01f;
        vegetation.maxAngle = maxAngle;
        vegetation.distance = distance;
        vegetation.plantGain = plantGain;
        vegetation.treeGain = treeGain;
        vegetation.debugView = debug;

        VegetationWind sway;
        sway.Configure(vegetation);

        const double step = 1.0 / 60.0;
        // Wet weather raises the target strength through the shared wind's own
        // weather response, exactly as it does in the client.
        const int weatherKind = cases[panelIndex].weather;
        const WindWeather weather{ weatherKind, weatherKind ? 1.f : 0.f };
        for (int frame = 0; frame < 240; ++frame)
        {
            const WindState state = windSystem.Update(weather, float(step));
            sway.Update(state);
        }
        sway.BeginFrame(); // one frame in this panel, exactly like the renderer
        const WindState state = windSystem.State();

        Panel panel{ Frame(width * supersample, height * supersample), scene };
        RenderPanel(panel, reference, sway, state, panel.frame.width, panel.frame.height);

        if (stats)
            std::printf("panel %zu: strength %.2f (%.1f yd/s) mean lean %.2f deg, max %.2f deg, tree %.2f deg, tuft %.2f deg\n",
                panelIndex, panel.strength, panel.velocity, panel.meanAngle, panel.maxAngle, panel.treeAngle,
                panel.tuftAngle);

        // Resolve the supersampled frame into the sheet, gamma-encoded once.
        const auto encode = [](float value) { return std::pow(Saturate(value), 1.f / 2.2f); };
        const size_t tileX = panelIndex % size_t(columns), tileY = panelIndex / size_t(columns);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                Vec3 sum{ 0.f, 0.f, 0.f };
                for (int j = 0; j < supersample; ++j)
                    for (int i = 0; i < supersample; ++i)
                    {
                        const size_t at = size_t(y * supersample + j) * size_t(panel.frame.width) +
                            size_t(x * supersample + i);
                        sum = sum + panel.frame.color[at];
                    }
                const Vec3 average = sum * (1.f / float(supersample * supersample));
                const int panelY = int(tileY) * (height + bar);
                uint8_t* destination = &sheet[(size_t(panelY + y) * size_t(sheetWidth) + tileX * size_t(width) +
                    size_t(x)) * 3];
                destination[0] = uint8_t(encode(average.x) * 255.f + .5f);
                destination[1] = uint8_t(encode(average.y) * 255.f + .5f);
                destination[2] = uint8_t(encode(average.z) * 255.f + .5f);
            }
        }
        if (labels)
        {
            char label[160];
            // The bar is 480 px of 6 px glyphs: the numbers a reader compares
            // across panels, and nothing else. The per-plant breakdown is what
            // --stats and --debug print.
            std::snprintf(label, sizeof(label), "%-6s %3.0f%% (%.1f YD/S)  GUST %2.0f%%  MEAN LEAN %.2f DEG",
                cases[panelIndex].name, panel.strength * 100.f, panel.velocity, gustPercentage, panel.meanAngle);
            preview::LabelBar(sheet, sheetWidth, sheetHeight, int(tileX) * width, int(tileY) * (height + bar), width,
                bar, label);
        }
    }

    if (!preview::WritePng(out.c_str(), sheetWidth, sheetHeight, sheet))
    {
        std::printf("failed to write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s %dx%d\n", out.c_str(), sheetWidth, sheetHeight);
    return 0;
}
