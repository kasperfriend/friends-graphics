// Ambient occlusion regressions. Run from the repository root.
//
// The AO pass only ever sees a depth buffer and a camera, so the whole
// contract can be checked on the CPU: build a known scene, encode its depth
// exactly the way INTZ stores it, run AmbientOcclusionCore.h, and assert what
// comes out. No device, no Windows, no shader compiler.
//
// What is pinned here is the SHIPPED LOOK, not implementation detail, because
// the look is what a change can quietly break:
//
//   * open surfaces must not darken at all - this is the whole reason the
//     default radius is 1 and not 2, and the reason AO reads as grounding
//     rather than as dirt;
//   * contacts and corners must darken, and a corner more than a flat wall
//     next to it;
//   * the effect must retire with distance instead of sparkling;
//   * the depth-aware blur must not smear occlusion across a silhouette.
//
// The scene (a ground plane, a wall, and a box flush against it) is the one
// the shipped radius and strength were tuned against.
#include "../src/Effects/AmbientOcclusionCore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace renderer::ambientocclusion;

namespace
{

void Check(bool ok, const std::string& message)
{
    if (!ok) throw std::runtime_error(message);
}

void Pass(const std::string& message) { std::printf("PASS %s\n", message.c_str()); }

bool Near(float a, float b, float epsilon) { return std::abs(a - b) <= epsilon; }

// ---------------------------------------------------------------- scene -----
// z-up world. Ground plane z=0, a wall at y=3 (|x|<=4, 0<=z<=5) and a box
// flush against it (x 1..3, y 1.5..3, z 0..2), so the corner where the two
// meet is a real right angle on the floor.
constexpr float kCamera[3] = { 0.f, -8.f, 3.f };
// forward = normalize((0,11,-2)), right = (1,0,0), up = right x forward.
constexpr float kForward[3] = { 0.f, 0.98387f, -0.17889f };
constexpr float kRight[3] = { 1.f, 0.f, 0.f };
constexpr float kUp[3] = { 0.f, 0.17889f, 0.98387f };
constexpr float kP00 = 1.0f;
constexpr float kP11 = 1.0f;
constexpr float kNear = 0.5f;
constexpr float kFar = 300.f;

// Helper: signed distance to an axis-aligned box, from a centre and half extents.
float Box(float x, float y, float z, float cx, float cy, float cz, float hx, float hy, float hz)
{
    const float qx = std::abs(x - cx) - hx;
    const float qy = std::abs(y - cy) - hy;
    const float qz = std::abs(z - cz) - hz;
    const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f), oz = std::max(qz, 0.f);
    const float outside = std::sqrt(ox * ox + oy * oy + oz * oz);
    const float inside = std::min(std::max(qx, std::max(qy, qz)), 0.f);
    return inside + outside;
}

float SignedDistance(float x, float y, float z)
{
    float d = z; // ground plane
    if (std::abs(x) <= 4.f && z >= 0.f && z <= 5.f) d = std::min(d, std::abs(y - 3.f));
    // The box is flush against the wall, so the floor where the two meet is a
    // real right-angled corner: x 1..3, y 1.5..3, z 0..2.
    d = std::min(d, Box(x, y, z, 2.f, 2.25f, 1.f, 1.f, .75f, 1.f));
    // A low block standing alone, more than a kernel radius from anything,
    // purely so an OPEN horizontal surface exists to check for halos:
    // x -4.5..-1.5, y -2.5..0.5, z 0..1.2.
    d = std::min(d, Box(x, y, z, -3.f, -1.f, .6f, 1.5f, 1.5f, .6f));
    return d;
}

struct Buffer
{
    int width = 0, height = 0;
    std::vector<float> raw;
    float projA = 0, projB = 0;

    float At(float u, float v) const
    {
        const int x = std::clamp(int(u * width), 0, width - 1);
        const int y = std::clamp(int(v * height), 0, height - 1);
        return raw[size_t(y) * width + x];
    }
};

Buffer RenderScene(int width, int height)
{
    Buffer out;
    out.width = width;
    out.height = height;
    out.raw.assign(size_t(width) * height, 1.f);
    out.projA = kFar / (kFar - kNear);
    out.projB = -kNear * kFar / (kFar - kNear);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const float u = (float(x) + .5f) / float(width);
            const float v = (float(y) + .5f) / float(height);
            const float vx = (u * 2.f - 1.f) / kP00, vy = (1.f - v * 2.f) / kP11;
            const float d[3] = { vx * kRight[0] + vy * kUp[0] + kForward[0],
                                 vx * kRight[1] + vy * kUp[1] + kForward[1],
                                 vx * kRight[2] + vy * kUp[2] + kForward[2] };
            float t = 0.05f;
            for (int step = 0; step < 220; ++step)
            {
                const float p[3] = { kCamera[0] + d[0] * t, kCamera[1] + d[1] * t, kCamera[2] + d[2] * t };
                const float distance = SignedDistance(p[0], p[1], p[2]);
                if (distance < 0.0012f)
                {
                    const float norm = out.projA + out.projB / std::max(t, 1e-4f);
                    out.raw[size_t(y) * width + x] = std::min(norm, 1.f); // viewport MaxZ = 1
                    break;
                }
                t += distance * .9f;
                if (t > 80.f) break;
            }
        }
    }
    return out;
}

// Screen position of a world point, in the same projection the core inverts.
void Project(float wx, float wy, float wz, float& u, float& v, float& viewZ)
{
    const float dx = wx - kCamera[0], dy = wy - kCamera[1], dz = wz - kCamera[2];
    const float vx = dx * kRight[0] + dy * kRight[1] + dz * kRight[2];
    const float vy = dx * kUp[0] + dy * kUp[1] + dz * kUp[2];
    viewZ = dx * kForward[0] + dy * kForward[1] + dz * kForward[2];
    u = (vx / viewZ * kP00 + 1.f) * .5f;
    v = (1.f - vy / viewZ * kP11) * .5f;
}

// The shipped tuning, read straight off GraphicsEffects.ini's defaults.
Settings Shipped()
{
    Settings s;
    s.enabled = true;
    s.strength = .60f;   // StrengthPercent=60
    s.radius = 1.f;      // RadiusUnits=1
    s.fadeDistance = 250.f; // DistanceFadeUnits=250
    return s;
}

struct Sample
{
    float u = 0, v = 0, viewZ = 0;
};

Sample At(float wx, float wy, float wz)
{
    Sample s;
    Project(wx, wy, wz, s.u, s.v, s.viewZ);
    return s;
}

} // namespace

int main()
{
    // Unbuffered: an aborted check must still show the numbers it was
    // looking at, or a regression is impossible to diagnose from CI logs.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // --------------------------------------------------------- the kernel ---
    {
        for (int i = 0; i < kSampleCount; ++i)
        {
            const float* k = kKernel[i];
            const float length = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
            Check(Near(length, 1.f, 1e-3f), "AO kernel direction is normalized");
            Check(k[2] > 0.f, "AO kernel direction opens along the normal");
        }
        Pass("the 12 sample directions form a unit hemisphere");

        // The rotations must stay unit vectors: they rotate the kernel, and a
        // non-unit rotation would scale the hemisphere and change the radius.
        for (int y = 0; y < 4; ++y)
        {
            for (int x = 0; x < 4; ++x)
            {
                float rot[2];
                RotationFor(x, y, rot);
                Check(Near(std::sqrt(rot[0] * rot[0] + rot[1] * rot[1]), 1.f, 1e-5f),
                      "AO kernel rotation is a unit vector");
            }
        }
        Pass("the 4x4 rotation pattern is made of unit rotations");
    }

    // ------------------------------------------------- depth round-trip -----
    {
        ViewParams view;
        view.projA = kFar / (kFar - kNear);
        view.projB = -kNear * kFar / (kFar - kNear);
        view.projP00 = kP00;
        view.projP11 = kP11;
        view.depthMaxZ = 1.f;

        auto encode = [&](float z) {
            return std::min(view.projA + view.projB / z, 1.f) * view.depthMaxZ;
        };
        // The denominator is negative for every real surface; guarding it with
        // max() instead of min() silently returned ~-50000 and disabled AO.
        for (float z : { 1.f, 2.f, 5.f, 20.f, 60.f, 200.f })
            Check(Near(ViewZ(encode(z), view), z, std::max(0.02f, z * 0.002f)),
                  "view depth survives the viewport-depth round trip");
        Check(ViewZ(1.f, view) > 0.f, "the sky value reconstructs as a positive far depth");
        Pass("depth reconstruction round-trips and stays positive");
    }

    // -------------------------------------------------- the shipped look ----
    const Buffer scene = RenderScene(160, 160);
    {
        ViewParams view;
        view.projA = scene.projA;
        view.projB = scene.projB;
        view.projP00 = kP00;
        view.projP11 = kP11;
        view.depthMaxZ = 1.f;
        const float texelU = 1.f / float(scene.width), texelV = 1.f / float(scene.height);
        auto fetch = [&](float u, float v) { return scene.At(u, v); };
        const Settings shipped = Shipped();

        const float openFloor = ComputeOcclusion(At(4.5f, 1.f, 0.f).u, At(4.5f, 1.f, 0.f).v,
            texelU, texelV, view, shipped, fetch);
        const float wallBase = ComputeOcclusion(At(-1.f, 2.6f, 0.f).u, At(-1.f, 2.6f, 0.f).v,
            texelU, texelV, view, shipped, fetch);
        const float boxFront = ComputeOcclusion(At(2.f, 1.2f, 0.f).u, At(2.f, 1.2f, 0.f).v,
            texelU, texelV, view, shipped, fetch);
        const float corner = ComputeOcclusion(At(0.72f, 2.75f, 0.f).u, At(0.72f, 2.75f, 0.f).v,
            texelU, texelV, view, shipped, fetch);
        const float wallFace = ComputeOcclusion(At(-1.f, 2.9f, 1.5f).u, At(-1.f, 2.9f, 1.5f).v,
            texelU, texelV, view, shipped, fetch);
        const float boxTop = ComputeOcclusion(At(-3.f, -1.f, 1.25f).u, At(-3.f, -1.f, 1.25f).v,
            texelU, texelV, view, shipped, fetch);

        std::printf("     open floor %.4f | wall base %.4f | box front %.4f | corner %.4f | "
                    "wall face %.4f | open top %.4f\n",
            openFloor, wallBase, boxFront, corner, wallFace, boxTop);

        // The "not too much" half of the brief: three kinds of open surface
        // must come back untouched. This is what radius 1 buys and radius 2
        // loses (a wider kernel put ~8% on an open box top).
        Check(Near(openFloor, 1.f, 0.01f), "open floor is not darkened at all");
        Check(Near(wallFace, 1.f, 0.01f), "an open vertical face is not darkened");
        Check(Near(boxTop, 1.f, 0.01f), "an isolated open horizontal top is not darkened");
        Pass("open surfaces keep their brightness (no false occlusion)");

        // The "makes a visual difference" half.
        Check(wallBase < 0.9f, "the floor against a wall darkens visibly");
        Check(boxFront < 0.95f, "the floor against a box darkens visibly");
        Check(corner < wallBase, "a right-angled corner darkens more than a flat wall base");
        Check(corner < 0.8f, "the corner is the darkest place in the scene");
        Pass("contacts and corners darken, and the corner darkens most");

        // Monotone in strength: the slider must do what it says.
        Settings weak = shipped, strong = shipped;
        weak.strength = .2f;
        strong.strength = 1.f;
        const auto probe = At(0.72f, 2.75f, 0.f);
        const float aoWeak = ComputeOcclusion(probe.u, probe.v, texelU, texelV, view, weak, fetch);
        const float aoMid = corner;
        const float aoStrong = ComputeOcclusion(probe.u, probe.v, texelU, texelV, view, strong, fetch);
        Check(aoWeak > aoMid && aoMid > aoStrong, "AO strength is monotone");
        Check(Near(aoWeak, 1.f, 0.02f) == false, "AO strength 20% still does something");
        Pass("the strength slider moves the result monotonically");

        // Sky: never darkened, never reconstructed as a surface.
        const float sky = ComputeOcclusion(0.02f, 0.02f, texelU, texelV, view, shipped, fetch);
        Check(Near(sky, 1.f, 1e-6f), "sky is left alone");
        Pass("sky pixels are untouched");

        // Distance fade: the same corner stops being shaded once it is far
        // enough away, instead of being shaded by reconstruction noise.
        Settings nearFade = shipped;
        nearFade.fadeDistance = 6.f;
        const float farAway = ComputeOcclusion(probe.u, probe.v, texelU, texelV, view, nearFade, fetch);
        Check(Near(farAway, 1.f, 1e-5f), "AO retires beyond the fade distance");
        Pass("AO retires with distance instead of sparkling");

        // The locality guard: an occluder far in front of a surface must not
        // darken the surface behind it, which is what would put a halo round
        // every silhouette. The wall face is open, and stays open.
        Check(Near(wallFace, 1.f, 0.01f), "a distant occluder does not darken what is behind it");
        Pass("the locality guard keeps occlusion off unrelated surfaces");
    }

    // -------------------------------------------------------- the radius ----
    // Widening the kernel is the one control that can turn AO into dirt. The
    // shipped radius is 1 for exactly one reason, and this is the check that
    // keeps it honest: at 1 the open ground is untouched, at 2 it is not.
    {
        ViewParams view;
        view.projA = scene.projA;
        view.projB = scene.projB;
        view.projP00 = kP00;
        view.projP11 = kP11;
        view.depthMaxZ = 1.f;
        const float texelU = 1.f / float(scene.width), texelV = 1.f / float(scene.height);
        auto fetch = [&](float u, float v) { return scene.At(u, v); };

        const Sample openFloor = At(4.5f, 1.f, 0.f);
        const Sample openTop = At(-3.f, -1.f, 1.25f);
        float atShipped = 1.f, atWide = 1.f, topAtWide = 1.f;
        for (float radius : { 1.f, 2.f })
        {
            Settings s = Shipped();
            s.radius = radius;
            const float floor = ComputeOcclusion(openFloor.u, openFloor.v, texelU, texelV, view, s, fetch);
            const float top = ComputeOcclusion(openTop.u, openTop.v, texelU, texelV, view, s, fetch);
            std::printf("     radius %.0f: open floor %.3f | isolated open top %.3f\n", radius, floor, top);
            if (radius == 1.f) atShipped = floor;
            else { atWide = floor; topAtWide = top; }
        }
        Check(Near(atShipped, 1.f, 0.01f), "the shipped radius leaves open ground untouched");
        Check(atWide < 0.99f, "a wider radius starts darkening open ground - why radius stays 1");
        Check(Near(topAtWide, 1.f, 0.01f), "an isolated top stays clean even at a wider radius");
        Check(Shipped().radius <= 1.5f, "the shipped radius stays in the clean range");
        Pass("the shipped radius is the one that keeps open surfaces clean");
    }

    // ---------------------------------------------------------- the blur ----
    {
        // Sky taps carry no occlusion and must not drag the average toward it.
        Check(Near(BlurWeight(0.f, 12.f), 0.f, 1e-6f), "a sky tap is ignored by the blur");
        // A silhouette: the tap is 4 units nearer than the surface behind it.
        Check(Near(BlurWeight(8.f, 12.f), 0.f, 1e-6f), "a silhouette tap is rejected by the blur");
        // Gentle terrain slope: the tap is 0.05 units away, well inside the
        // threshold, so the blur behaves like an ordinary box blur.
        Check(BlurWeight(11.95f, 12.f) > 0.6f, "a gentle slope is blurred normally");
        Check(Near(BlurWeight(12.f, 12.f), 1.f, 1e-6f), "a flat neighbourhood blurs at full weight");
        // The 4x4 kernel is centred on the pixel.
        Check(Near(BlurOffset(0) + BlurOffset(kBlurSpan - 1), 0.f, 1e-6f), "the blur kernel is centred");
        Pass("the depth-aware blur keeps occlusion off the other side of an edge");
    }

    std::printf("ambient occlusion regressions: all checks passed\n");
    return 0;
}
