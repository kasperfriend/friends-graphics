// Dynamic (local) light pool regressions. Run from the repository root.
//
// g++ -O2 -std=c++20 -Wall -Wextra -Werror -pedantic tools/LocalLightingRegression.cpp -o build/local-lighting-test
//
// A lamp pool only ever sees a depth buffer, a camera and the light table, so
// the whole contract is checkable on the CPU: build a known scene, encode its
// depth exactly the way INTZ stores it (24-bit quantization included), run
// LocalLightingCore.h, and assert what comes out. No device, no Windows, no
// shader compiler. The two ps_3_0 passes in src/Effects/LocalLightingRenderer.cpp
// mirror that core, and the last block below pins that they still do.
//
// What is pinned is the SHIPPED LOOK, in the order a player reports it:
//
//   * a pool on flat ground is a smooth field. The scene carries a plain whose
//     true normal is exactly (0,0,1) at every pixel, so anything the
//     reconstruction reports besides that - in the normal or in the light on
//     screen - is noise with no legitimate variation left to hide behind;
//   * that smoothness does not depend on the light buffer's resolution, which
//     is the point of sizing the window against the buffer's own texel;
//   * on undulating ground at a grazing angle the normal still points at the
//     surface: within a few degrees of the relief's own normal, where the
//     logic this replaced was off by tens of degrees on most pixels;
//   * a wall and a low block still light from the correct side - there the
//     window slides to a one-sided difference instead of averaging two
//     surfaces into one normal;
//   * the full-resolution composite draws no 1-2 pixel step line along the
//     light buffer's grid and does not leak a silhouette's bright side onto
//     its dark side;
//   * the specular gate is exactly one on untouched ground, so a well-resolved
//     flat pool looks the way it did before the smoothing.
//
// The scene is watched from a shallow pitch so most of the frame is ground at
// a grazing angle: the case where one pixel of ground is yards wide, and where
// an absolute "this window straddles an edge" threshold reads as an edge on
// flat ground.
#include "../src/Effects/LocalLightingCore.h"
#include "../src/Lighting/LocalLightSelection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace renderer::locallighting;

namespace
{

void Check(bool ok, const std::string& message)
{
    if (!ok) throw std::runtime_error(message);
}

void Pass(const std::string& message) { std::printf("PASS %s\n", message.c_str()); }

bool Near(float a, float b, float epsilon) { return std::abs(a - b) <= epsilon; }

// ------------------------------------------------------------------ scene ---
// z-up world. Ground at z=0 carrying the terrain's own undulation up close and
// a flat plain in the distance, a wall across the middle distance and a low
// block to the left, so one frame has open ground, a silhouette, a step and a
// surface with a known answer.
constexpr float kCamera[3] = { 0.f, -22.f, 3.4f };
// forward = normalize((0, 44, -3.4)): a shallow pitch, the angle a player uses
// to look at the ground in front of a lamp.
constexpr float kForward[3] = { 0.f, 0.99726f, -0.07708f };
constexpr float kRight[3] = { 1.f, 0.f, 0.f };
constexpr float kUp[3] = { 0.f, 0.07708f, 0.99726f }; // right x forward
constexpr float kP00 = 1.0f;       // 90 degrees horizontal
constexpr float kP11 = 16.f / 9.f; // 16:9
constexpr float kNear = 0.5f;
constexpr float kFar = 300.f;
constexpr float kMaxZ = 1.f; // viewport MaxZ, as the client leaves it

// What a terrain vertex grid actually carries on ground that reads as flat to
// the eye: undulations of roughly the LOD spacing (8..12 yards) and a small
// tile-level detail on top. The wavelengths matter - relief finer than the
// pixel grid at range is unrecoverable aliasing, and a test that measures
// that is measuring the scene rather than the algorithm.
float Undulation(float x, float y)
{
    return 0.45f * std::sin(x * 0.55f + 0.4f) * std::cos(y * 0.42f) +
           0.25f * std::sin(x * 0.9f + y * 0.7f) +
           0.07f * std::sin(x * 2.3f + y * 1.9f);
}

// The undulation fades out with distance and leaves a MATHEMATICALLY FLAT
// plain beyond y=28. That plain is the noise measurement, in the same spirit
// as the ambient-occlusion suite demanding that an open floor read exactly
// 1.000: on it the true normal is (0,0,1) at every pixel, so whatever the
// reconstruction reports besides that is noise.
float Ramp(float t)
{
    const float c = std::clamp(t, 0.f, 1.f);
    return c * c * (3.f - 2.f * c);
}

// Undulating in the middle distance, flat both close to the camera (y < -8) and
// far away (y > 28). The two flat bands are the noise measurements: the near
// one where a pixel is a fraction of a yard, so the pool's own curvature cannot
// be confused with grain, and the far one at a grazing angle, where a pixel is
// yards wide and the reconstruction that this replaced fell apart.
float ReliefMask(float y) { return Ramp((28.f - y) / 10.f) * Ramp((y + 8.f) / 6.f); }

float Relief(float x, float y) { return Undulation(x, y) * ReliefMask(y); }

// The normal that relief implies: what the reconstruction is measured against,
// instead of against a smoothed copy of itself.
Vec3 ReliefNormal(float x, float y)
{
    const float h = 0.01f;
    const float dx = (Relief(x + h, y) - Relief(x - h, y)) / (2.f * h);
    const float dy = (Relief(x, y + h) - Relief(x, y - h)) / (2.f * h);
    return Normalize(Vec3{ -dx, -dy, 1.f });
}

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

// A capsule along z: the character-like body of the nearby-object scene.
float Capsule(float x, float y, float z, float cx, float cy, float r, float z0, float z1)
{
    const float zz = std::clamp(z, z0, z1);
    const float dx = x - cx, dy = y - cy, dz = z - zz;
    return std::sqrt(dx * dx + dy * dy + dz * dz) - r;
}

// Material ids: 0 sky, 1 ground, 2 wall, 3 block.
int MaterialAt(float x, float y, float z, float& distance)
{
    // The height field is not a true signed distance; 0.8 keeps the sphere
    // trace conservative against its own slope (|grad relief| < 0.5 here).
    const float ground = (z - Relief(x, y)) * 0.8f;
    const float wall = Box(x, y, z, 8.f, 8.f, 2.f, 6.f, 0.4f, 2.f);
    const float block = Box(x, y, z, -8.f, 4.f, 0.5f, 2.5f, 3.f, 0.5f);
    int material = 1;
    distance = ground;
    if (wall < distance) { distance = wall; material = 2; }
    if (block < distance) { distance = block; material = 3; }
    return material;
}

struct Scene
{
    int width = 0, height = 0;
    std::vector<float> raw;    // quantized viewport depth, 1.0 = clear
    std::vector<int> material; // what the ray ended on
    float projA = 0.f, projB = 0.f;

    float At(float u, float v) const
    {
        const int x = std::clamp(int(u * float(width)), 0, width - 1);
        const int y = std::clamp(int(v * float(height)), 0, height - 1);
        return raw[size_t(y) * width + x];
    }
    int Material(float u, float v) const
    {
        const int x = std::clamp(int(u * float(width)), 0, width - 1);
        const int y = std::clamp(int(v * float(height)), 0, height - 1);
        return material[size_t(y) * width + x];
    }
};

// A D24S8 depth buffer stores 24 bits of fixed point. Quantizing is what makes
// this a test of the window rather than of double-precision arithmetic.
float Quantize24(float value)
{
    const float scale = 16777215.f;
    return std::floor(std::clamp(value, 0.f, 1.f) * scale + 0.5f) / scale;
}

// Newton-refine a march hit onto the surface itself.
//
// The game's INTZ buffer is rasterized: flat ground is exact to the last bit of
// the 24-bit depth. A sphere march that stops at its tolerance instead leaves
// every pixel a hair above the surface, and that hair is not the same hair
// everywhere - it depends on the ray's angle and on the steps that happened to
// precede it. The window's normal reproduces that wobble faithfully, and with a
// lamp low enough to graze the ground the wobble IS the light: a normal off by
// a tenth of a degree moves a grazing dot product by a percent. Without this
// the harness measures its own renderer, not the algorithm.
float RefineHit(const float* camera, const float* d, float t)
{
    const float start = t;
    for (int i = 0; i < 8; ++i)
    {
        const float p[3] = { camera[0] + d[0] * t, camera[1] + d[1] * t, camera[2] + d[2] * t };
        float distance = 0.f;
        MaterialAt(p[0], p[1], p[2], distance);
        float gradient[3];
        const float e = 1e-4f;
        for (int axis = 0; axis < 3; ++axis)
        {
            float q[3] = { p[0], p[1], p[2] };
            q[axis] += e;
            float plus = 0.f;
            MaterialAt(q[0], q[1], q[2], plus);
            q[axis] -= 2.f * e;
            float minus = 0.f;
            MaterialAt(q[0], q[1], q[2], minus);
            gradient[axis] = (plus - minus) / (2.f * e);
        }
        const float slope = gradient[0] * d[0] + gradient[1] * d[1] + gradient[2] * d[2];
        if (std::abs(slope) < 1e-7f) break;
        t -= distance / slope;
        if (std::abs(t - start) > 0.25f) return start; // do not wander to another surface
    }
    return t;
}


Scene RenderScene(int width, int height)
{
    Scene out;
    out.width = width;
    out.height = height;
    out.raw.assign(size_t(width) * height, kMaxZ);
    out.material.assign(size_t(width) * height, 0);
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
            float t = kNear;
            for (int step = 0; step < 512; ++step)
            {
                const float p[3] = { kCamera[0] + d[0] * t, kCamera[1] + d[1] * t, kCamera[2] + d[2] * t };
                float distance = 0.f;
                const int material = MaterialAt(p[0], p[1], p[2], distance);
                if (distance < 0.0015f)
                {
                    t = RefineHit(kCamera, d, t);
                    const float encoded = out.projA + out.projB / std::max(t, 1e-4f);
                    out.raw[size_t(y) * width + x] = Quantize24(std::min(encoded, kMaxZ));
                    out.material[size_t(y) * width + x] = material;
                    break;
                }
                t += std::max(distance, 0.004f) * .8f;
                if (t > kFar) break;
            }
        }
    }
    return out;
}

Projection MakeProjection(const Scene& scene)
{
    // The renderer folds the viewport transform into A/B before uploading, so
    // the shader's Linear() is exactly b/(raw-a) on the stored value.
    Projection p;
    p.a = scene.projA;
    p.b = scene.projB;
    p.p00 = kP00;
    p.p11 = kP11;
    return p;
}

InverseView MakeInverseView()
{
    InverseView inv;
    inv.row[0] = { kRight[0], kRight[1], kRight[2] };
    inv.row[1] = { kUp[0], kUp[1], kUp[2] };
    inv.row[2] = { kForward[0], kForward[1], kForward[2] };
    return inv;
}

const Vec3 kCameraWorld{ kCamera[0], kCamera[1], kCamera[2] };
constexpr Vec3 kWorldUp{ 0.f, 0.f, 1.f }; // WoW world axes are +Z up.

// ------------------------------------------------------------------- pool ---
struct Pool
{
    int width = 0, height = 0;
    std::vector<float> luminance; // what the player sees
    std::vector<float> intact;    // the specular gate
    std::vector<float> depth;     // linear view depth, as stored in the alpha
    std::vector<Vec3> position;   // the receiver's world position
    std::vector<Vec3> normal;     // the reconstructed receiver normal
};

// The window/weights/normal/shade path of kSurfaceSource, evaluated on the CPU
// through the same core the shader mirrors. `legacy` selects the logic this
// replaced - a one-texel window, an absolute edge threshold, a hard pick, and
// a tangent spelled as (midpoint of the two taps) minus the receiver - so the
// test can report what the fix bought instead of only asserting a number.
// `specular` off evaluates the pool with the highlight gated away (intact = 0,
// which is exactly the gate the shader multiplies by). The GRAIN metrics use
// that: pow(dot,24) is a deliberately sharp term, and where the mirror
// direction of a low lamp lands on flat ground it produces a real highlight
// whose curvature at this resolution is not noise. Everything else - the
// normal, the silhouette, the specular gate itself - is measured with it on.
Pool EvaluatePool(const Scene& scene, float scale, const Projection& projection,
                  const InverseView& inverseView, const LightSample& lamp,
                  const NormalTuning& tuning, bool legacy, bool specular = true)
{
    Pool pool;
    pool.width = std::max(1, int(float(scene.width) * scale));
    pool.height = std::max(1, int(float(scene.height) * scale));
    const size_t pixels = size_t(pool.width) * pool.height;
    pool.luminance.assign(pixels, 0.f);
    pool.intact.assign(pixels, 1.f);
    pool.depth.assign(pixels, 0.f);
    pool.position.assign(pixels, Vec3{});
    pool.normal.assign(pixels, Vec3{ 0.f, 0.f, 1.f });

    const Vec2 bufferTexel{ 1.f / float(pool.width), 1.f / float(pool.height) };
    const Vec2 sceneTexel{ 1.f / float(scene.width), 1.f / float(scene.height) };

    for (int y = 0; y < pool.height; ++y)
    {
        for (int x = 0; x < pool.width; ++x)
        {
            const Vec2 pixelUv = legacy ? Vec2{ (float(x) + .5f) * bufferTexel.x, (float(y) + .5f) * bufferTexel.y }
                                        : SnapUv({ (float(x) + .5f) * bufferTexel.x, (float(y) + .5f) * bufferTexel.y }, sceneTexel);
            const float u = pixelUv.x;
            const float v = pixelUv.y;
            const float raw = scene.At(u, v);
            const size_t index = size_t(y) * pool.width + x;
            if (raw >= .99990f) continue; // the pass returns zero on sky/water
            const float z = LinearDepth(raw, projection);
            const Vec3 p = WorldPosition(u, v, raw, projection, inverseView, kCameraWorld);
            const Vec2 spread = legacy ? sceneTexel
                                      : TapOffset(Footprint(z, bufferTexel, tuning), sceneTexel);

            // Exactly what the shader does: the depth map is sampled with CLAMP
            // addressing, and a clear value reconstructs as the far plane, which
            // is what makes the edge test drop a sky side of the window.
            auto tap = [&](float du, float dv, float& depthOut, float* rawOut = nullptr) {
                const Vec2 tapUv = legacy ? ClampUv({ u + du, v + dv }) : SnapUv({ u + du, v + dv }, sceneTexel);
                const float sample = scene.At(tapUv.x, tapUv.y);
                depthOut = LinearDepth(sample, projection);
                if (rawOut) *rawOut = sample;
                return WorldPosition(tapUv.x, tapUv.y, sample, projection, inverseView, kCameraWorld);
            };
            float zLeft = 0.f, zRight = 0.f, zUp = 0.f, zDown = 0.f;
            float rawLeft = 0.f, rawRight = 0.f, rawUp = 0.f, rawDown = 0.f;
            const Vec3 pLeft = tap(-spread.x, 0.f, zLeft, &rawLeft);
            const Vec3 pRight = tap(spread.x, 0.f, zRight, &rawRight);
            const Vec3 pUp = tap(0.f, -spread.y, zUp, &rawUp);
            const Vec3 pDown = tap(0.f, spread.y, zDown, &rawDown);
            float zLeft2 = 0.f, zRight2 = 0.f, zUp2 = 0.f, zDown2 = 0.f;
            float rawLeft2 = 0.f, rawRight2 = 0.f, rawUp2 = 0.f, rawDown2 = 0.f;
            tap(-spread.x * 2.f, 0.f, zLeft2, &rawLeft2);
            tap(spread.x * 2.f, 0.f, zRight2, &rawRight2);
            tap(0.f, -spread.y * 2.f, zUp2, &rawUp2);
            tap(0.f, spread.y * 2.f, zDown2, &rawDown2);
            const Vec3 toEye = Normalize(kCameraWorld - p);

            SurfaceFrame frame;
            if (legacy)
            {
                const float edge = std::max(.12f, z * .008f);
                const float dLeft = std::abs(zLeft - z), dRight = std::abs(zRight - z);
                const Vec3 axisX = std::max(dLeft, dRight) < edge ? (pLeft + pRight) * .5f
                                                                 : (dLeft < dRight ? pLeft : pRight);
                const float dUp = std::abs(zUp - z), dDown = std::abs(zDown - z);
                const Vec3 axisY = std::max(dUp, dDown) < edge ? (pUp + pDown) * .5f
                                                              : (dUp < dDown ? pUp : pDown);
                frame.normal = Normalize(Cross(axisX - p, axisY - p));
                if (Dot(frame.normal, toEye) < 0.f) frame.normal = frame.normal * -1.f;
                frame.intact = 1.f;
            }
            else
            {
                TapSurvival survives;
                // locallighting::TapSurvival: sky AND the self-tap of a
                // clamped off-frame window measure nothing.
                auto measured = [&](float du, float dv, float raw) {
                    const Vec2 tapUv = SnapUv({ u + du, v + dv }, sceneTexel);
                    return raw < .99990f && (tapUv.x != pixelUv.x || tapUv.y != pixelUv.y);
                };
                survives.left = measured(-spread.x, 0.f, rawLeft);
                survives.right = measured(spread.x, 0.f, rawRight);
                survives.up = measured(0.f, -spread.y, rawUp);
                survives.down = measured(0.f, spread.y, rawDown);
                survives.left2 = measured(-spread.x * 2.f, 0.f, rawLeft2);
                survives.right2 = measured(spread.x * 2.f, 0.f, rawRight2);
                survives.up2 = measured(0.f, -spread.y * 2.f, rawUp2);
                survives.down2 = measured(0.f, spread.y * 2.f, rawDown2);
                frame = ReconstructNormal(p, pLeft, pRight, pUp, pDown,
                                          zLeft, zRight, zUp, zDown,
                                          zLeft2, zRight2, zUp2, zDown2,
                                          z, toEye, tuning, survives,
                                          kWorldUp);
            }

            SurfaceFrame shaded = frame;
            if (!specular) shaded.intact = 0.f;
            const Vec3 lit = PoolShade(lamp, p, shaded, kCameraWorld);
            pool.luminance[index] = lit.x * .3f + lit.y * .5f + lit.z * .2f;
            pool.intact[index] = frame.intact;
            pool.depth[index] = z;
            pool.position[index] = p;
            pool.normal[index] = frame.normal;
        }
    }
    return pool;
}

// -------------------------------------------------------------- composite ---
struct Composite
{
    int width = 0, height = 0;
    std::vector<float> luminance;
};

// The full-resolution blend of kCompositeSource: a depth-weighted window over
// the light buffer, with the slope-aware tolerance and the continuity floor.
Composite CompositePool(const Scene& scene, const Pool& pool, const Projection& projection,
                        const CompositeTuning& tuning, bool legacy)
{
    Composite out;
    out.width = scene.width;
    out.height = scene.height;
    out.luminance.assign(size_t(out.width) * out.height, 0.f);
    const Vec2 lightTexel{ 1.f / float(pool.width), 1.f / float(pool.height) };
    const Vec2 fullTexel{ 1.f / float(scene.width), 1.f / float(scene.height) };
    const float resolutionRatio = std::max(lightTexel.x / fullTexel.x, lightTexel.y / fullTexel.y);

    for (int y = 0; y < out.height; ++y)
    {
        for (int x = 0; x < out.width; ++x)
        {
            const float u = (float(x) + .5f) * fullTexel.x;
            const float v = (float(y) + .5f) * fullTexel.y;
            const size_t index = size_t(y) * out.width + x;
            const float raw = scene.At(u, v);
            if (raw >= .99990f) continue;
            const float receiverZ = LinearDepth(raw, projection);
            const float slope = CombineSlope(LinearDepth(scene.At(u - fullTexel.x, v), projection),
                                             LinearDepth(scene.At(u + fullTexel.x, v), projection),
                                             LinearDepth(scene.At(u, v - fullTexel.y), projection),
                                             LinearDepth(scene.At(u, v + fullTexel.y), projection),
                                             receiverZ);
            const float pixelX = u / lightTexel.x - .5f, pixelY = v / lightTexel.y - .5f;
            const float baseX = std::floor(pixelX), baseY = std::floor(pixelY);
            const float fractionX = pixelX - baseX, fractionY = pixelY - baseY;
            auto poolAt = [&](int ox, int oy, std::vector<float> Pool::*field) {
                const int cx = std::clamp(int(baseX) + ox, 0, pool.width - 1);
                const int cy = std::clamp(int(baseY) + oy, 0, pool.height - 1);
                return (pool.*field)[size_t(cy) * pool.width + cx];
            };

            float light = 0.f, weightSum = 0.f;
            // The bilinear 2x2 window the composite had sits at offsets {0,1};
            // the 3x3 tent that replaced it is centred, {-1,0,1}.
            const int low = legacy ? 0 : -1;
            for (int oy = low; oy <= 1; ++oy)
            {
                for (int ox = low; ox <= 1; ++ox)
                {
                    const float spatial = legacy
                        ? std::max(0.f, 1.f - std::abs(float(ox) - fractionX)) *
                          std::max(0.f, 1.f - std::abs(float(oy) - fractionY))
                        : CompositeSpatialWeight(float(ox), fractionX, tuning) *
                          CompositeSpatialWeight(float(oy), fractionY, tuning);
                    if (spatial <= 0.f) continue;
                    const float depthWeight = CompositeDepthWeight(poolAt(ox, oy, &Pool::depth),
                                                                   receiverZ, slope, resolutionRatio,
                                                                   tuning);
                    const float w = std::max(spatial * depthWeight, spatial * (legacy ? 0.02f : tuning.floor));
                    light += poolAt(ox, oy, &Pool::luminance) * w;
                    weightSum += w;
                }
            }
            out.luminance[index] = light / std::max(weightSum, 0.01f);
        }
    }
    return out;
}

// ---------------------------------------------------------------- metrics ---
struct Region
{
    std::vector<char> mask;
    int width = 0, height = 0;
    size_t count = 0;
    bool At(int x, int y) const { return mask[size_t(y) * width + x] != 0; }
};

Region MaskOf(std::vector<char> mask, int width, int height)
{
    Region region;
    region.count = size_t(std::count(mask.begin(), mask.end(), char(1)));
    region.mask = std::move(mask);
    region.width = width;
    region.height = height;
    return region;
}

// High-frequency content: the mean absolute second difference, normalized by
// the LOCAL 5x5 mean rather than by the region's mean. A lamp pool varies by
// orders of magnitude across a region, so a region-wide normalization scores
// the falloff itself as noise; against the local level a smooth falloff reads
// near zero and grain reads orders of magnitude higher.
// Pixels are only measured where the pool actually is: `floor` is a fraction of
// the region's peak light, and anything dimmer is skipped. Without it the
// metric diverges at the pool's own edge, where a smooth falloff to zero has a
// large second difference relative to a near-zero local level - it would score
// the falloff, not the grain.
float HighFrequency(const std::vector<float>& values, const Region& region, float floor = 0.10f)
{
    float peak = 0.f;
    for (int y = 0; y < region.height; ++y)
        for (int x = 0; x < region.width; ++x)
            if (region.At(x, y)) peak = std::max(peak, values[size_t(y) * region.width + x]);
    const float minimum = peak * floor;
    double sum = 0;
    size_t n = 0;
    for (int y = 2; y + 2 < region.height; ++y)
    {
        for (int x = 2; x + 2 < region.width; ++x)
        {
            bool covered = true;
            float level = 0.f;
            for (int dy = -2; dy <= 2 && covered; ++dy)
            {
                for (int dx = -2; dx <= 2; ++dx)
                {
                    if (!region.At(x + dx, y + dy)) { covered = false; break; }
                    level += values[size_t(y + dy) * region.width + x + dx];
                }
            }
            if (!covered) continue;
            level /= 25.f;
            if (level < minimum) continue;
            const size_t i = size_t(y) * region.width + x;
            const float horizontal = values[i - 1] - 2.f * values[i] + values[i + 1];
            const float vertical = values[i - region.width] - 2.f * values[i] + values[i + region.width];
            sum += (std::abs(horizontal) + std::abs(vertical)) * .5 / std::max(level, 1e-7f);
            ++n;
        }
    }
    return n ? float(sum / double(n)) : 0.f;
}

// Mean angle in degrees between the reconstructed normal and the normal the
// scene's own geometry implies. `scale` maps the full-resolution region grid
// onto the pool's own texels.
float NormalError(const Pool& pool, const Region& region, float scale)
{
    double sum = 0;
    size_t n = 0;
    for (int y = 0; y < region.height; ++y)
    {
        for (int x = 0; x < region.width; ++x)
        {
            if (!region.At(x, y)) continue;
            const int px = std::clamp(int((float(x) + .5f) * scale), 0, pool.width - 1);
            const int py = std::clamp(int((float(y) + .5f) * scale), 0, pool.height - 1);
            const size_t i = size_t(py) * pool.width + px;
            const Vec3 want = ReliefNormal(pool.position[i].x, pool.position[i].y);
            sum += std::acos(std::clamp(Dot(pool.normal[i], want), -1.f, 1.f)) * 57.2957795;
            ++n;
        }
    }
    return n ? float(sum / double(n)) : 0.f;
}

// Largest single-pixel jump along a scanline, normalized by the local level of
// the three pixels involved: the "line" a winner-takes-all blend draws along
// the light buffer's grid shows up here and nowhere else. Pixels dimmer than
// `floor` of the region's peak are skipped, because at the pool's own edge the
// local level goes to zero and every step is relatively enormous.
float LargestStep(const std::vector<float>& values, const Region& region, float floor = 0.15f)
{
    float peak = 0.f;
    for (int y = 0; y < region.height; ++y)
        for (int x = 0; x < region.width; ++x)
            if (region.At(x, y)) peak = std::max(peak, values[size_t(y) * region.width + x]);
    const float minimum = peak * floor;
    float largest = 0.f;
    for (int y = 1; y + 1 < region.height; ++y)
    {
        for (int x = 1; x + 1 < region.width; ++x)
        {
            if (!region.At(x, y) || !region.At(x + 1, y) || !region.At(x - 1, y)) continue;
            const size_t i = size_t(y) * region.width + x;
            const float level = (values[i - 1] + values[i] + values[i + 1]) / 3.f;
            if (level < minimum) continue;
            largest = std::max(largest, std::abs(values[i + 1] - values[i]) / std::max(level, 1e-7f));
        }
    }
    return largest;
}

std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main()
{
    // Unbuffered: an aborted check must still show the numbers it was looking
    // at, or a regression is impossible to diagnose from a CI log.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    Check(!ShouldActivate(false, 1.f, 0) && !ShouldActivate(false, 0.f, 4),
          "disabling dynamic lighting disables normal and debug composite requests");
    Check(!ShouldActivate(true, 0.f, 0) &&
          !ShouldActivate(true, std::numeric_limits<float>::infinity(), 0),
          "zero or non-finite intensity does not request an empty normal pass");
    Check(ShouldActivate(true, 1.f, 0), "enabled direct lights independently request the volume composite");
    Check(ShouldActivate(true, 0.f, 4) && IsDebugView(1) && IsDebugView(4) && !IsDebugView(5),
          "all four light debug views run even with no selected light");
    Pass("dynamic-light composite activation and debug policy");

    {
        struct LightIdentity { std::uint64_t stableId; };
        const std::vector<LightIdentity> selected{{11}, {22}, {33}};
        const std::vector<LightIdentity> unchanged{{11}, {22}, {33}};
        const std::vector<LightIdentity> reordered{{33}, {11}, {22}};
        const std::vector<LightIdentity> changed{{11}, {22}, {44}};
        const std::uint64_t original = renderer::local_light_detail::SelectionSignature(selected);
        Check(original == renderer::local_light_detail::SelectionSignature(unchanged) &&
              original == renderer::local_light_detail::SelectionSignature(reordered),
              "the selected-light signature is stable for an unchanged set and its reorderings");
        Check(original != renderer::local_light_detail::SelectionSignature(changed),
              "the selected-light signature changes when an emitter is replaced");
        Pass("same-frame selected-light cache signature");
    }

    const int kWidth = 480, kHeight = 270;
    const Scene scene = RenderScene(kWidth, kHeight);
    const Projection projection = MakeProjection(scene);
    const InverseView inverseView = MakeInverseView();
    const NormalTuning tuning = DefaultNormalTuning();
    const CompositeTuning compositeTuning;

    LightSample lamp;
    lamp.position = { -12.f, 12.f, 2.5f };
    lamp.radius = 70.f;
    lamp.intensity = 1.f;
    lamp.color = { 1.f, .78f, .48f };
    {
        LightSample outside = lamp;
        outside.position = { 100.f, 0.f, 0.f };
        outside.radius = 10.f;
        SurfaceFrame flat;
        flat.normal = { 0.f, 0.f, 1.f };
        const Vec3 contribution = PoolShade(outside, {}, flat, { 0.f, 0.f, 10.f });
        Check(Near(contribution.x, 0.f, 1e-7f) && Near(contribution.y, 0.f, 1e-7f) &&
              Near(contribution.z, 0.f, 1e-7f),
              "a light outside its finite radius contributes exactly zero");
        Pass("out-of-radius light contribution is zero");
    }

    // ------------------------------------------------- depth round trip ----
    for (float z : { 1.f, 3.f, 10.f, 40.f, 120.f })
    {
        const float encoded = Quantize24(std::min(projection.a + projection.b / z, kMaxZ));
        Check(Near(LinearDepth(encoded, projection), z, std::max(0.05f, z * 0.01f)),
              "view depth survives the quantized INTZ round trip");
    }
    Pass("quantized INTZ reconstructs to the depth that was written");

    // ------------------------------------------------------- the regions ---
    const Pool reference = EvaluatePool(scene, 1.f, projection, inverseView, lamp, tuning, false, false);
    struct Band { int material; float minDepth, maxDepth, minLight, minY, maxY; };
    auto buildRegion = [&](const Band& band) {
        std::vector<char> mask(size_t(kWidth) * kHeight, 0);
        for (int y = 0; y < kHeight; ++y)
        {
            for (int x = 0; x < kWidth; ++x)
            {
                const float u = (float(x) + .5f) / float(kWidth), v = (float(y) + .5f) / float(kHeight);
                if (scene.Material(u, v) != band.material) continue;
                const size_t index = size_t(y) * kWidth + x;
                const float z = LinearDepth(scene.At(u, v), projection);
                if (z < band.minDepth || z > band.maxDepth) continue;
                if (reference.position[index].y < band.minY) continue;
                if (reference.position[index].y > band.maxY) continue;
                if (reference.luminance[index] < band.minLight) continue;
                mask[index] = 1;
            }
        }
        return MaskOf(std::move(mask), kWidth, kHeight);
    };

    // Undulating ground at a grazing angle: the normal has to follow a real
    // surface that changes faster than the pixel grid.
    const Region farGround = buildRegion({ 1, 10.f, 45.f, 0.f, -8.f, 22.f });
    // The flat apron close to the camera: a pixel is a fraction of a yard, so
    // the pool is smooth by construction and any second difference is grain.
    const Region nearFlat = buildRegion({ 1, 4.f, 13.f, 0.f, -100.f, -9.f });
    // The flat plain far out: a pixel is yards wide, so the pool's own
    // curvature dominates a light-space metric - here the NORMAL is the
    // measurement, and the light is only compared against the previous logic.
    const Region flatPlain = buildRegion({ 1, 25.f, 120.f, 0.f, 32.f, 1e6f });
    std::printf("     pixels: undulating %zu, near flat %zu, far flat %zu\n",
                farGround.count, nearFlat.count, flatPlain.count);
    Check(farGround.count > 4000, "the frame carries enough undulating ground to measure");
    Check(nearFlat.count > 1500, "the frame carries enough near flat ground to measure");
    Check(flatPlain.count > 1500, "the frame carries enough far flat ground to measure");
    // ------------------------------------------- smoothness at every quality --
    struct QualityRow { const char* name; float scale; };
    const QualityRow qualities[] = { { "LOW     (0.5x)", .5f },
                                     { "BALANCED(0.75x)", .75f },
                                     { "HIGH    (1.0x)", 1.f } };

    float nearGrain[3] = {}, nearGrainLegacy[3] = {}, nearNormal[3] = {};
    float farGrain[3] = {}, farGrainLegacy[3] = {}, farNormal[3] = {}, farNormalLegacy[3] = {};
    float undulatingNormal[3] = {}, undulatingNormalLegacy[3] = {};
    for (int q = 0; q < 3; ++q)
    {
        const float scale = qualities[q].scale;
        const Pool shipped = EvaluatePool(scene, scale, projection, inverseView, lamp, tuning, false, false);
        const Pool legacy = EvaluatePool(scene, scale, projection, inverseView, lamp, tuning, true, false);
        // Both go through the composite each one shipped with, so the metric is
        // the light on screen and not a buffer nobody looks at.
        const Composite onScreen = CompositePool(scene, shipped, projection, compositeTuning, false);
        const Composite onScreenLegacy = CompositePool(scene, legacy, projection, compositeTuning, true);

        nearGrain[q] = HighFrequency(onScreen.luminance, nearFlat);
        nearGrainLegacy[q] = HighFrequency(onScreenLegacy.luminance, nearFlat);
        nearNormal[q] = NormalError(shipped, nearFlat, scale);
        farGrain[q] = HighFrequency(onScreen.luminance, flatPlain);
        farGrainLegacy[q] = HighFrequency(onScreenLegacy.luminance, flatPlain);
        farNormal[q] = NormalError(shipped, flatPlain, scale);
        farNormalLegacy[q] = NormalError(legacy, flatPlain, scale);
        undulatingNormal[q] = NormalError(shipped, farGround, scale);
        undulatingNormalLegacy[q] = NormalError(legacy, farGround, scale);

        std::printf("     %s near flat:  grain %.5f (was %.5f), normal %.2f deg\n",
                    qualities[q].name, nearGrain[q], nearGrainLegacy[q], nearNormal[q]);
        std::printf("     %s undulating: normal %.2f deg (was %.2f deg)\n",
                    qualities[q].name, undulatingNormal[q], undulatingNormalLegacy[q]);
        std::printf("     %s far flat:   grain %.5f (was %.5f, %.1fx), normal %.2f deg (was %.2f deg)\n",
                    qualities[q].name, farGrain[q], farGrainLegacy[q],
                    farGrainLegacy[q] / std::max(farGrain[q], 1e-9f), farNormal[q], farNormalLegacy[q]);
    }

    {
        const NormalTuning fixedTuning = DefaultNormalTuning();
        const Pool fixedPool = EvaluatePool(scene, 1.f, projection, inverseView, lamp, fixedTuning, false, false);
        const Composite fixedOnScreen = CompositePool(scene, fixedPool, projection, compositeTuning, false);
        const float fixedNearGrain = HighFrequency(fixedOnScreen.luminance, nearFlat);
        const float fixedFarGrain = HighFrequency(fixedOnScreen.luminance, flatPlain);
        const float fixedUndulatingNormal = NormalError(fixedPool, farGround, 1.f);
        std::printf("     fixed texel footprint: near grain %.5f, far grain %.5f, undulating normal %.2f deg\n",
                    fixedNearGrain, fixedFarGrain, fixedUndulatingNormal);
        Check(fixedNearGrain <= nearGrain[2] + 0.001f && fixedFarGrain <= farGrain[2] + 0.01f,
              "the fixed texel footprint keeps flat-ground grain bounded");
        Check(fixedUndulatingNormal <= undulatingNormal[2],
              "the fixed texel footprint still follows undulating relief");
        Pass("fixed texel footprint preserves measured ground smoothness and relief");
    }

    auto worst = [](const float* v) { return std::max(std::max(v[0], v[1]), v[2]); };

    // The flat apron near the camera is where a light-space number means what it
    // says: a pixel there is a fraction of a yard of ground, so the pool's own
    // curvature is nowhere near the second difference and everything left is
    // grain. The old logic scored twenty to four hundred times higher on the
    // same pixels - an independent normal decision per pixel, all the way down
    // to flat ground.
    Check(worst(nearGrain) < 0.010f, "a pool on flat ground near the camera is a smooth field");
    Check(worst(nearGrainLegacy) > 10.f * worst(nearGrain),
          "the previous window was measurably grainier on the same flat ground");
    Check(worst(nearNormal) < 0.5f, "on flat ground near the camera the normal points straight up");
    Pass("near flat ground: the pool is smooth at LOW, BALANCED and HIGH");

    // On the far plain the normal is the absolute measurement: the geometry
    // there is exactly flat, so a normal within a couple of degrees of straight
    // up is a normal within a couple of degrees of right. The old logic lost
    // that entirely - it reported a nearly horizontal normal on ground with no
    // slope at all, which is what the pool then drew.
    Check(worst(farNormal) < 2.5f, "on flat ground at a grazing angle the normal still points straight up");
    Check(worst(farNormalLegacy) > 20.f * worst(farNormal),
          "the previous window lost flat ground's normal at range (the regression this pins)");
    for (int q = 0; q < 3; ++q)
        Check(farGrainLegacy[q] > 2.5f * farGrain[q],
              "the previous window was grainier on the far plain at every quality");
    Pass("far flat ground: the normal survives the grazing angle that used to destroy it");

    // Resolution independence, stated as what actually happens: the light field
    // gets smoother as its buffer gets finer, and never the other way round. The
    // logic this replaced did the opposite - at quality 1 its window was
    // one full-resolution pixel wide and the buffer it fed was 0.75x, so the
    // normal field was narrower than the texels it described and finer buffers
    // aliased harder, which is why quality 2 was the "better" one.
    const float nearCeiling = std::max(nearGrain[0] * 1.25f, 0.004f);
    for (int q = 1; q < 3; ++q)
        Check(nearGrain[q] <= nearCeiling, "a finer light buffer does not make the pool grainier");
    Check(nearGrainLegacy[2] > 1.2f * nearGrainLegacy[0],
          "the previous window got grainier as the buffer got finer (the regression this pins)");
    Check(nearGrainLegacy[2] > 0.05f, "the previous window's grain at quality 2 is the one a player reports");
    Pass("the pool's grain falls with resolution instead of rising with it");

    // Undulating ground, at a grazing angle, is the case the window has to get
    // right without a flat answer to lean on: the normal has to follow the
    // surface it cannot see whole.
    Check(worst(undulatingNormal) < 9.f,
          "at a grazing angle the normal still follows the terrain's own relief");
    Check(worst(undulatingNormalLegacy) > 3.f * worst(undulatingNormal),
          "the previous window did not follow that relief");
    Pass("undulating ground keeps a normal that follows the surface instead of its grain");

    // --------------------------------------------------- silhouettes hold ---
    {
        // The wall's face must light as a face (a near-horizontal normal) and
        // the ground at its base must still light as ground: the window slides
        // to a one-sided difference there instead of averaging two surfaces
        // into one normal.
        const Pool pool = EvaluatePool(scene, 1.f, projection, inverseView, lamp, tuning, false);
        const Vec2 depthTexel{ 1.f / float(scene.width), 1.f / float(scene.height) };
        auto windowOn = [&](float u, float v, int material) {
            const int x = std::clamp(int(u * float(scene.width)), 0, scene.width - 1);
            const int y = std::clamp(int(v * float(scene.height)), 0, scene.height - 1);
            const Vec2 spread = TapOffset(Footprint(reference.depth[size_t(y) * kWidth + x],
                                                    Vec2{ 1.f / kWidth, 1.f / kHeight }, tuning),
                                          depthTexel);
            for (int k = 0; k < 4; ++k)
            {
                const float du = k == 0 ? -spread.x : k == 1 ? spread.x : 0.f;
                const float dv = k == 2 ? -spread.y : k == 3 ? spread.y : 0.f;
                const Vec2 tapUv = SnapUv({ u + du, v + dv }, depthTexel);
                if (scene.Material(tapUv.x, tapUv.y) != material) return false;
            }
            return true;
        };

        double wallNormalZ = 0, groundNormalZ = 0;
        double intactFlat = 0, flatNormalZ = 0, intactBase = 0, baseNormalZ = 0;
        size_t wallCount = 0, groundCount = 0, flatCount = 0, baseCount = 0;
        for (int y = 2; y + 2 < kHeight; ++y)
        {
            for (int x = 1; x + 1 < kWidth; ++x)
            {
                const float u = (float(x) + .5f) / float(kWidth), v = (float(y) + .5f) / float(kHeight);
                const int material = scene.Material(u, v);
                if (material == 0) continue;
                const size_t index = size_t(y) * kWidth + x;
                if (material == 2)
                {
                    wallNormalZ += std::abs(pool.normal[index].z);
                    ++wallCount;
                    continue;
                }
                if (material != 1) continue;
                groundNormalZ += pool.normal[index].z;
                ++groundCount;
                if (nearFlat.At(x, y) && windowOn(u, v, 1))
                {
                    intactFlat += pool.intact[index];
                    flatNormalZ += pool.normal[index].z;
                    ++flatCount;
                }
                if (scene.Material(u, v - 3.f / float(kHeight)) == 2)
                {
                    intactBase += pool.intact[index];
                    baseNormalZ += pool.normal[index].z;
                    ++baseCount;
                }
            }
        }
        Check(wallCount > 200 && groundCount > 2000 && flatCount > 500 && baseCount > 40,
              "the frame carries a wall, open ground and a silhouette base to compare");
        const float meanWall = float(wallNormalZ / double(wallCount));
        const float meanGround = float(groundNormalZ / double(groundCount));
        const float meanFlatNormal = float(flatNormalZ / double(flatCount));
        const float meanIntactFlat = float(intactFlat / double(flatCount));
        const float meanBaseNormal = float(baseNormalZ / double(baseCount));
        const float meanIntactBase = float(intactBase / double(baseCount));
        std::printf("     mean normal.z: wall %.3f, open ground %.3f, flat apron %.3f, silhouette base %.3f\n",
                    meanWall, meanGround, meanFlatNormal, meanBaseNormal);
        std::printf("     specular gate: flat apron %.4f, silhouette base %.4f\n", meanIntactFlat, meanIntactBase);
        Check(meanWall < 0.45f, "the wall face lights from its own plane, not from the ground behind it");
        Check(meanGround > 0.85f, "the ground at a silhouette base still lights as ground");
        // The gate is a statement about a window that had nothing to drop; on
        // ground that is exactly flat it therefore has to read one, and at a
        // silhouette - where the window had to drop a side - it must read less.
        // Not exactly 1.0: a window cut by the FRAME edge now honestly
        // reports its dropped side (the clamp's self-tap measures nothing -
        // locallighting::TapSurvival), and those pixels lie inside this
        // region. The gate is still essentially one everywhere the window
        // fits, and clearly less at the silhouette base below.
        Check(meanIntactFlat > 0.97f, "the specular gate is one on ground the window fits whole");
        Check(meanIntactBase < meanIntactFlat, "the specular gate reports the window that had to drop a side");
        Pass("a silhouette still lights from the correct side, and says so");
    }

    // ------------------------------------------------- the composite blend --
    {
        const Pool pool = EvaluatePool(scene, .75f, projection, inverseView, lamp, tuning, false, false);
        const Composite onScreen = CompositePool(scene, pool, projection, compositeTuning, false);

        // The line a winner-takes-all blend draws along the light buffer's grid:
        // compared against the blend that had one, on the same pool.
        const Pool legacyPool = EvaluatePool(scene, .75f, projection, inverseView, lamp, tuning, true);
        const Composite onScreenLegacy = CompositePool(scene, legacyPool, projection, compositeTuning, true);
        // Where the artifact showed: close to the camera, one texel of the light
        // buffer is two screen pixels wide, so a blend that snaps to its own
        // grid draws the grid on the ground in front of the player. The far pool
        // is only compared against the blend it replaced - out there the pool's
        // own structure is what a step in it measures.
        const float step = LargestStep(onScreen.luminance, nearFlat);
        const float legacyStep = LargestStep(onScreenLegacy.luminance, nearFlat);
        std::printf("     largest single-pixel step near the camera: %.4f of its level (was %.4f)\n",
                    step, legacyStep);
        Check(step < 0.02f, "the composite draws no 1-2 pixel line along the light buffer's grid");
        Check(legacyStep > 10.f * step, "the previous blend drew one (the regression this pins)");
        const float farStep = LargestStep(onScreen.luminance, farGround);
        const float farLegacyStep = LargestStep(onScreenLegacy.luminance, farGround);
        std::printf("     largest single-pixel step across the far pool: %.4f of its level (was %.4f)\n",
                    farStep, farLegacyStep);
        // Out there the pool's own structure is what a step in it measures, so
        // this is only a claim of improvement, not of smoothness.
        Check(farStep < farLegacyStep, "the far pool is no steppier than the blend it replaced");

        // A masked-out side of a silhouette contributes only its continuity
        // floor, so on the ground side of a wall the receiver must keep the
        // ground's light instead of taking the wall's. Compared against the
        // blend that had no slope-aware tolerance and no continuum of taps, on
        // the same pool and the same pixels.
        auto leak = [&](const Composite& composite) {
            double sum = 0;
            size_t n = 0;
            for (int y = 3; y + 3 < kHeight; ++y)
            {
                for (int x = 3; x + 3 < kWidth; ++x)
                {
                    const float u = (float(x) + .5f) / float(kWidth), v = (float(y) + .5f) / float(kHeight);
                    if (scene.Material(u, v) != 1) continue;
                    if (scene.Material(u, v - 4.f / float(kHeight)) != 2) continue;
                    const int px = std::clamp(int(u * float(pool.width)), 0, pool.width - 1);
                    const int py = std::clamp(int(v * float(pool.height)), 0, pool.height - 1);
                    const float own = pool.luminance[size_t(py) * pool.width + px];
                    if (own < 1e-5f) continue;
                    sum += std::abs(composite.luminance[size_t(y) * kWidth + x] - own) / own;
                    ++n;
                }
            }
            return n ? float(sum / double(n)) : 0.f;
        };
        const float shippedLeak = leak(onScreen);
        const float legacyLeak = leak(onScreenLegacy);
        std::printf("     mean deviation at a silhouette base: %.3f of the receiver's own light (was %.3f)\n",
                    shippedLeak, legacyLeak);
        Check(shippedLeak < 0.35f, "a silhouette's other side cannot take the receiver's light over");
        Check(shippedLeak < 0.5f * legacyLeak, "the previous blend let it further (the regression this pins)");
        Pass("the composite stays continuous and keeps silhouettes closed");
    }

    // -------------------------------- nearby objects painting pool copies ---
    // The player report this pins: with dynamic lights on and the camera up
    // close ("go nearby"), the pool painted illusionary copies of characters
    // and objects - dark shape-copies floating beside them - and LIGHT
    // SMOOTHING only softened it. The scene is the regime that produces them:
    // a pitched camera over undulating ground, with a character-like capsule
    // and a crate only a few yards away. A tap can land on the object's raised
    // surface while also shifting forward/back along camera-up; that component
    // can cancel the object's standing height. Flat ground and a near-level
    // camera do not expose the old screen-up gate, which is why those earlier
    // regressions missed the copy at strong smoothing.
    {
        // Material ids: 0 sky, 1 ground, 2 character (capsule+head), 3 crate.
        auto relief = [](float x, float y) {
            return 0.35f * std::sin(x * 0.5f + 0.3f) * std::cos(y * 0.38f) +
                   0.18f * std::sin(x * 0.9f + y * 0.6f);
        };
        auto materialNear = [&](float x, float y, float z, float& distance) {
            const float ground = (z - relief(x, y)) * 0.8f;
            const float body = Capsule(x, y, z, 0.9f, 2.2f, 0.34f, 0.34f, 1.75f);
            const float head = Capsule(x, y, z, 0.9f, 2.2f, 0.22f, 1.85f, 2.05f);
            const float crate = Box(x, y, z, -1.4f, 3.4f, 0.5f, 0.5f, 0.5f, 0.5f);
            int material = 1;
            distance = ground;
            if (body < distance) { distance = body; material = 2; }
            if (head < distance) { distance = head; material = 2; }
            if (crate < distance) { distance = crate; material = 3; }
            return material;
        };
        auto refineNear = [&](const float* camera, const float* d, float t) {
            const float start = t;
            for (int i = 0; i < 8; ++i)
            {
                const float p[3] = { camera[0] + d[0] * t, camera[1] + d[1] * t, camera[2] + d[2] * t };
                float distance = 0.f;
                materialNear(p[0], p[1], p[2], distance);
                float gradient[3];
                const float e = 1e-4f;
                for (int axis = 0; axis < 3; ++axis)
                {
                    float q[3] = { p[0], p[1], p[2] };
                    q[axis] += e;
                    float plus = 0.f;
                    materialNear(q[0], q[1], q[2], plus);
                    q[axis] -= 2.f * e;
                    float minus = 0.f;
                    materialNear(q[0], q[1], q[2], minus);
                    gradient[axis] = (plus - minus) / (2.f * e);
                }
                const float slope = gradient[0] * d[0] + gradient[1] * d[1] + gradient[2] * d[2];
                if (std::abs(slope) < 1e-7f) break;
                t -= distance / slope;
                if (std::abs(t - start) > 0.25f) return start;
            }
            return t;
        };
        auto trueNormalNear = [&](float x, float y, float z) {
            const float e = 1e-3f;
            float g[3];
            for (int axis = 0; axis < 3; ++axis)
            {
                float q[3] = { x, y, z };
                q[axis] += e;
                float plus = 0.f;
                materialNear(q[0], q[1], q[2], plus);
                q[axis] -= 2.f * e;
                float minus = 0.f;
                materialNear(q[0], q[1], q[2], minus);
                g[axis] = (plus - minus) / (2.f * e);
            }
            return Normalize(Vec3{ g[0], g[1], g[2] });
        };

        const float nCamera[3] = { 0.f, -2.6f, 4.f };
        // A 30-degree downward view aimed at the character and crate. The
        // camera-up vector is visibly not world-up: a standing height can
        // partly cancel against a tap's forward/back displacement when
        // measured along the screen-up axis. This is the missing orientation
        // case that let the former gate leave a copy beside nearby silhouettes.
        const float yawX = 0.06225f, yawY = 0.99806f;
        const float pitchSin = 0.5f, pitchCos = 0.86603f;
        const float nForward[3] = { yawX * pitchCos, yawY * pitchCos, -pitchSin };
        const float nRight[3] = { yawY, -yawX, 0.f };
        const float nUp[3] = { yawX * pitchSin, yawY * pitchSin, pitchCos };

        Scene near;
        near.width = kWidth;
        near.height = kHeight;
        near.raw.assign(size_t(kWidth) * kHeight, kMaxZ);
        near.material.assign(size_t(kWidth) * kHeight, 0);
        near.projA = kFar / (kFar - kNear);
        near.projB = -kNear * kFar / (kFar - kNear);
        for (int y = 0; y < kHeight; ++y)
        {
            for (int x = 0; x < kWidth; ++x)
            {
                const float u = (float(x) + .5f) / float(kWidth);
                const float v = (float(y) + .5f) / float(kHeight);
                const float vx = (u * 2.f - 1.f) / kP00, vy = (1.f - v * 2.f) / kP11;
                const float d[3] = { vx * nRight[0] + vy * nUp[0] + nForward[0],
                                     vx * nRight[1] + vy * nUp[1] + nForward[1],
                                     vx * nRight[2] + vy * nUp[2] + nForward[2] };
                float t = kNear;
                for (int step = 0; step < 512; ++step)
                {
                    const float p[3] = { nCamera[0] + d[0] * t, nCamera[1] + d[1] * t, nCamera[2] + d[2] * t };
                    float distance = 0.f;
                    const int material = materialNear(p[0], p[1], p[2], distance);
                    if (distance < 0.0015f)
                    {
                        t = refineNear(nCamera, d, t);
                        const float encoded = near.projA + near.projB / std::max(t, 1e-4f);
                        near.raw[size_t(y) * kWidth + x] = Quantize24(std::min(encoded, kMaxZ));
                        near.material[size_t(y) * kWidth + x] = material;
                        break;
                    }
                    t += std::max(distance, 0.004f) * .8f;
                    if (t > kFar) break;
                }
            }
        }
        Projection nearProj;
        nearProj.a = near.projA;
        nearProj.b = near.projB;
        nearProj.p00 = kP00;
        nearProj.p11 = kP11;
        InverseView nearInv;
        nearInv.row[0] = { nRight[0], nRight[1], nRight[2] };
        nearInv.row[1] = { nUp[0], nUp[1], nUp[2] };
        nearInv.row[2] = { nForward[0], nForward[1], nForward[2] };
        const Vec3 nearCamera{ nCamera[0], nCamera[1], nCamera[2] };
        LightSample nearLamp;
        nearLamp.position = { 1.2f, 4.2f, 2.4f };
        nearLamp.radius = 14.f;
        nearLamp.intensity = 1.f;
        nearLamp.color = { 1.f, .8f, .5f };

        // The evaluator for this scene, with a switch for the PRE-FIX
        // weighting kept inline: step detection compared the two-sided
        // disagreement against the window's own one-sided changes (drowned by
        // the grazing slope) and scaled the outlier slide by the deviation
        // gap (drowned the same way). Kept here so the numbers this prints
        // stay honest about what the fix bought.
        auto evaluateNear = [&](float scale, const NormalTuning& tuning, bool trueNormals, bool drowned,
                                const Vec3& gateUp) {
            Pool pool;
            pool.width = std::max(1, int(float(kWidth) * scale));
            pool.height = std::max(1, int(float(kHeight) * scale));
            const size_t pixels = size_t(pool.width) * pool.height;
            pool.luminance.assign(pixels, 0.f);
            pool.intact.assign(pixels, 1.f);
            pool.depth.assign(pixels, 0.f);
            pool.position.assign(pixels, Vec3{});
            pool.normal.assign(pixels, Vec3{ 0.f, 0.f, 1.f });
            const Vec2 bufferTexel{ 1.f / float(pool.width), 1.f / float(pool.height) };
            const Vec2 sceneTexel{ 1.f / float(kWidth), 1.f / float(kHeight) };
            for (int y = 0; y < pool.height; ++y)
            {
                for (int x = 0; x < pool.width; ++x)
                {
                    const Vec2 pixelUv = SnapUv({ (float(x) + .5f) * bufferTexel.x,
                                                  (float(y) + .5f) * bufferTexel.y }, sceneTexel);
                    const float u = pixelUv.x, v = pixelUv.y;
                    const float raw = near.At(u, v);
                    const size_t index = size_t(y) * pool.width + x;
                    if (raw >= .99990f) continue;
                    const float z = LinearDepth(raw, nearProj);
                    const Vec3 p = WorldPosition(u, v, raw, nearProj, nearInv, nearCamera);
                    const Vec2 spread = TapOffset(Footprint(z, bufferTexel, tuning), sceneTexel);
                    auto tap = [&](float du, float dv, float& depthOut, float* rawOut = nullptr) {
                        const Vec2 tapUv = SnapUv({ u + du, v + dv }, sceneTexel);
                        const float sample = near.At(tapUv.x, tapUv.y);
                        depthOut = LinearDepth(sample, nearProj);
                        if (rawOut) *rawOut = sample;
                        return WorldPosition(tapUv.x, tapUv.y, sample, nearProj, nearInv, nearCamera);
                    };
                    float zLeft = 0.f, zRight = 0.f, zUp = 0.f, zDown = 0.f;
                    float rawLeft = 0.f, rawRight = 0.f, rawUp = 0.f, rawDown = 0.f;
                    const Vec3 pLeft = tap(-spread.x, 0.f, zLeft, &rawLeft);
                    const Vec3 pRight = tap(spread.x, 0.f, zRight, &rawRight);
                    const Vec3 pUp = tap(0.f, -spread.y, zUp, &rawUp);
                    const Vec3 pDown = tap(0.f, spread.y, zDown, &rawDown);
                    float zLeft2 = 0.f, zRight2 = 0.f, zUp2 = 0.f, zDown2 = 0.f;
                    float rawLeft2 = 0.f, rawRight2 = 0.f, rawUp2 = 0.f, rawDown2 = 0.f;
                    tap(-spread.x * 2.f, 0.f, zLeft2, &rawLeft2);
                    tap(spread.x * 2.f, 0.f, zRight2, &rawRight2);
                    tap(0.f, -spread.y * 2.f, zUp2, &rawUp2);
                    tap(0.f, spread.y * 2.f, zDown2, &rawDown2);
                    const Vec3 toEye = Normalize(nearCamera - p);
                    SurfaceFrame frame;
                    if (trueNormals)
                    {
                        Vec3 n = trueNormalNear(p.x, p.y, p.z);
                        if (Dot(n, toEye) < 0.f) n = n * -1.f;
                        frame.normal = n;
                        frame.intact = 1.f;
                    }
                    else if (drowned)
                    {
                        // Identical to the shipped TapWeights except the step
                        // detection it replaced: the edge was the curvature
                        // against the window's own one-sided changes (drowned
                        // by the grazing slope) and the outlier slide was
                        // scaled by the deviation gap (drowned the same way).
                        auto side = [](float dA, float dB, float curv, float absolute,
                                       bool surfA, bool surfB) {
                            AxisWeights w;
                            if (!surfA || !surfB)
                            {
                                w.left = surfA ? 1.f : 0.f;
                                w.right = surfB ? 1.f : 0.f;
                                return w;
                            }
                            const float edge = Smoothstep(absolute, absolute * 4.f, curv) *
                                               Smoothstep(.30f, .85f, curv / std::max(dA + dB, absolute));
                            const float asym = (dA - dB) / std::max(std::max(dA, dB), 1e-4f);
                            // The pre-fix veto, kept verbatim for the
                            // comparison: 6x the partner's deviation.
                            const float veto = 6.f * std::min(dA, dB) + absolute;
                            if (dA > veto || dB > veto)
                            {
                                w.left = dA <= veto ? 1.f : 0.f;
                                w.right = dB <= veto ? 1.f : 0.f;
                                return w;
                            }
                            w.left = 1.f - edge * Smoothstep(0.f, 1.f, asym);
                            w.right = 1.f - edge * Smoothstep(0.f, 1.f, -asym);
                            return w;
                        };
                        auto measured = [&](float du, float dv, float raw) {
                            const Vec2 tapUv = SnapUv({ u + du, v + dv }, sceneTexel);
                            return raw < .99990f && (tapUv.x != pixelUv.x || tapUv.y != pixelUv.y);
                        };
                        auto axis = [&](float zA, float zB, float zC, bool surfA, bool surfB) {
                            const float iC = InverseDepth(zC);
                            const float dA = std::abs(InverseDepth(zA) - iC);
                            const float dB = std::abs(InverseDepth(zB) - iC);
                            const float curv = std::abs(InverseDepth(zA) + InverseDepth(zB) - 2.f * iC);
                            const float absolute = std::max(tuning.edgeFloor, zC * tuning.edgeSlope) /
                                                   std::max(zC * zC, 1e-3f);
                            return side(dA, dB, curv, absolute, surfA, surfB);
                        };
                        const AxisWeights x = axis(zLeft, zRight, z,
                                                   measured(-spread.x, 0.f, rawLeft),
                                                   measured(spread.x, 0.f, rawRight));
                        const AxisWeights y = axis(zUp, zDown, z,
                                                   measured(0.f, -spread.y, rawUp),
                                                   measured(0.f, spread.y, rawDown));
                        const Vec3 tangentX = (p - pLeft) * x.left + (pRight - p) * x.right;
                        const Vec3 tangentY = (p - pUp) * y.left + (pDown - p) * y.right;
                        const Vec3 cross = Cross(tangentX * (1.f / std::max(x.left + x.right, 1e-4f)),
                                                 tangentY * (1.f / std::max(y.left + y.right, 1e-4f)));
                        frame.normal = Dot(cross, cross) > 1e-12f ? Normalize(cross) : toEye;
                        if (Dot(frame.normal, toEye) < 0.f) frame.normal = frame.normal * -1.f;
                        frame.intact = Saturate((x.left + x.right + y.left + y.right) * .25f);
                    }
                    else
                    {
                        TapSurvival survives;
                        auto measuredShipped = [&](float du, float dv, float raw) {
                            const Vec2 tapUv = SnapUv({ u + du, v + dv }, sceneTexel);
                            return raw < .99990f && (tapUv.x != pixelUv.x || tapUv.y != pixelUv.y);
                        };
                        survives.left = measuredShipped(-spread.x, 0.f, rawLeft);
                        survives.right = measuredShipped(spread.x, 0.f, rawRight);
                        survives.up = measuredShipped(0.f, -spread.y, rawUp);
                        survives.down = measuredShipped(0.f, spread.y, rawDown);
                        survives.left2 = measuredShipped(-spread.x * 2.f, 0.f, rawLeft2);
                        survives.right2 = measuredShipped(spread.x * 2.f, 0.f, rawRight2);
                        survives.up2 = measuredShipped(0.f, -spread.y * 2.f, rawUp2);
                        survives.down2 = measuredShipped(0.f, spread.y * 2.f, rawDown2);
                        frame = ReconstructNormal(p, pLeft, pRight, pUp, pDown,
                                                  zLeft, zRight, zUp, zDown,
                                                  zLeft2, zRight2, zUp2, zDown2,
                                                  z, toEye, tuning, survives,
                                                  gateUp);
                    }
                    const Vec3 lit = PoolShade(nearLamp, p, frame, nearCamera);
                    pool.luminance[index] = lit.x * .3f + lit.y * .5f + lit.z * .2f;
                    pool.intact[index] = frame.intact;
                    pool.depth[index] = z;
                    pool.position[index] = p;
                    pool.normal[index] = frame.normal;
                }
            }
            return pool;
        };

        // The copy metric. The copies are shape-scale pools of wrong light on
        // the ground beside a silhouette; the residual that remains after the
        // fix is a 1-2 px rim ON the silhouette (and the composite's own
        // upsample outline) - pixel-scale. Box-filter both results first so
        // the metric reads the shapes and not the rim, then measure the
        // deviation from the true-normal shading on ground within a band
        // around every silhouette, normalized by the LOCAL true level (the
        // pool varies by orders of magnitude) and only where that level is
        // real light.
        auto copyError = [&](const Composite& got, const Composite& want) {
            std::vector<float> smoothGot(size_t(kWidth) * kHeight, 0.f);
            std::vector<float> smoothWant(size_t(kWidth) * kHeight, 0.f);
            std::vector<float> local(size_t(kWidth) * kHeight, 0.f);
            for (int y = 2; y + 2 < kHeight; ++y)
            {
                for (int x = 2; x + 2 < kWidth; ++x)
                {
                    float g = 0.f, w = 0.f;
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx)
                        {
                            g += got.luminance[size_t(y + dy) * kWidth + x + dx];
                            w += want.luminance[size_t(y + dy) * kWidth + x + dx];
                        }
                    smoothGot[size_t(y) * kWidth + x] = g / 25.f;
                    smoothWant[size_t(y) * kWidth + x] = w / 25.f;
                    float level = 0.f;
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx)
                            level += want.luminance[size_t(y + dy) * kWidth + x + dx];
                    local[size_t(y) * kWidth + x] = level / 25.f;
                }
            }
            float peak = 0.f;
            double sum = 0;
            size_t n = 0;
            for (int y = 12; y + 12 < kHeight; ++y)
            {
                for (int x = 12; x + 12 < kWidth; ++x)
                {
                    const size_t i = size_t(y) * kWidth + x;
                    if (near.material[i] != 1) continue;
                    bool band = false;
                    for (int dy = -12; dy <= 12 && !band; dy += 3)
                        for (int dx = -12; dx <= 12 && !band; dx += 3)
                            if (near.material[i + size_t(dy) * kWidth + dx] != 1) band = true;
                    if (!band) continue;
                    peak = std::max(peak, local[i]);
                }
            }
            const float floor = peak * 0.10f;
            for (int y = 12; y + 12 < kHeight; ++y)
            {
                for (int x = 12; x + 12 < kWidth; ++x)
                {
                    const size_t i = size_t(y) * kWidth + x;
                    if (near.material[i] != 1) continue;
                    bool band = false;
                    for (int dy = -12; dy <= 12 && !band; dy += 3)
                        for (int dx = -12; dx <= 12 && !band; dx += 3)
                            if (near.material[i + size_t(dy) * kWidth + dx] != 1) band = true;
                    if (!band) continue;
                    if (local[i] < floor) continue;
                    sum += std::abs(smoothGot[i] - smoothWant[i]) / local[i];
                    ++n;
                }
            }
            return n ? float(sum / double(n)) : 0.f;
        };

        // The runtime now always uses the no-extra-spread footprint. Snap the
        // zero-depth case to verify that the shader still has the one-texel
        // minimum needed to form a finite-difference normal.
        const NormalTuning fixedTuning = DefaultNormalTuning();
        const Vec2 depthTexel{ 1.f / float(kWidth), 1.f / float(kHeight) };
        const Vec2 minimumTap = TapOffset(Footprint(0.f, depthTexel, fixedTuning), depthTexel);
        Check(Near(minimumTap.x, depthTexel.x, 1e-6f) &&
              Near(minimumTap.y, depthTexel.y, 1e-6f),
              "the fixed normal fit retains exactly one depth texel at the near plane");
        const Pool truth = evaluateNear(1.f, fixedTuning, true, false, kWorldUp);
        const Pool current = evaluateNear(1.f, fixedTuning, false, false, kWorldUp);
        const Composite truthOnScreen = CompositePool(near, truth, nearProj, compositeTuning, false);
        const Composite currentOnScreen = CompositePool(near, current, nearProj, compositeTuning, false);
        const float silhouetteCopyError = copyError(currentOnScreen, truthOnScreen);
        std::printf("     fixed footprint: near-silhouette copy error %.4f\n", silhouetteCopyError);
        Check(silhouetteCopyError < 0.02f,
              "the fixed one-texel footprint bounds nearby-object copies");
        Pass("nearby characters and objects no longer paint copies into the pool");
    }

    // --------------------------------------------------------- the mirror ---
    {
        // The core is only worth having if the shipped shaders still spell it.
        // These are the lines that carry the fix; a change to either side has
        // to move them together.
        const std::string surface = ReadFile("src/Effects/LocalLightingRenderer.cpp");
        const std::string frameContext = ReadFile("src/Core/FrameContext.h");
        const std::string volumetric = ReadFile("src/Effects/DirectionalVolumetricLighting.cpp");
        const std::string volume = ReadFile("VolumeIntegration.h");
        const std::string atmosphere = ReadFile("src/Effects/DirectionalVolumetricLighting.cpp");
        const std::string manager = ReadFile("src/Lighting/LocalLightManager.cpp");
        const std::string managerHeader = ReadFile("src/Lighting/LocalLightManager.h");
        const std::string water = ReadFile("WaterEffect.h");
        const std::string settings = ReadFile("GraphicsEffects.ini");
        const std::string overlay = ReadFile("src/Environment/OverlayItems.h");
        const std::string volumeEffects = ReadFile("VolumeEffects.h");
        // The lamps are world-anchored; the gate that decides whether to draw them was
        // not. A frame whose draws carried no fresh eye (looking at the sky or a wall)
        // has cameraValid false while the retained pose is a frame or two old, and
        // refusing the pass outright blinked every lamp as the camera turned. The hold is
        // the same one the lamps and the underwater medium use.
        Check(surface.find("const bool poseRetained = !f.cameraValid && f.PoseUsable();") != std::string::npos &&
              surface.find("(!f.cameraValid && !poseRetained) || !f.depthAvailable") != std::string::npos &&
              frameContext.find("bool PoseUsable() const") != std::string::npos &&
              frameContext.find("return cameraValid || (viewRawValid && cameraEyeAgeMs <= kCameraEyeHoldMs);")
                  != std::string::npos,
              "a retained pose keeps the lamps drawn instead of blinking them by camera angle");
        // One definition of "a pose this frame may use", and every pass that draws a
        // lamp's light obeys it. The volumetric march used to refuse a frame the
        // surface lighting accepted, so the shaft and halo around a lamp blinked off
        // while the ground it lit stayed lit - a lamp that visibly broke.
        Check(volumetric.find("!f.PoseUsable()") != std::string::npos &&
              volumetric.find("!target||!f.cameraValid||") == std::string::npos,
              "the volumetric light march uses the same pose rule as the surface lighting");
        // A lamp's importance is where it is, not where the player is looking. The score
        // used to swing six-fold with the light's angle to the camera, and a light one
        // radius behind was culled outright, so turning the head re-ordered the four/eight
        // slots and dropped lamps in and out - the "lights break when observed at
        // different angles" report.
        Check(manager.find("light.score = light.intensity * light.radius * light.radius / std::max(4.0f, distanceSquared);")
                  != std::string::npos &&
              manager.find("forwardDistance") == std::string::npos &&
              manager.find("const float facing") == std::string::npos,
              "light selection is a fact about the lamp, not the camera's heading");
        Check(surface.find("if(distanceSquared>=radius*radius) continue;") != std::string::npos &&
              surface.find("sourceDebug+=contribution;") != std::string::npos &&
              surface.find("float3 contribution=lightColorPower[i].rgb*lightColorPower[i].w*(diffuse+spec)*attenuation;") != std::string::npos &&
              surface.find("if(tuning.y>2.5&&tuning.y<3.5) return float4(0,0,0,z);") != std::string::npos &&
              surface.find("if(tuning.y>3.5) return float4(intact.xxx,z);") != std::string::npos,
              "out-of-radius lights skip expensive shading and mode 1 reports the real attenuated source");
        Check(surface.find("locallighting::IsDebugView(debugMode) ? 1.f : 0.f") != std::string::npos &&
              volume.find("locallighting::ShouldActivate(") != std::string::npos &&
              volume.find("dynamicLightDebugView || (dynamicLightingActive && !otherEffectsActive)") != std::string::npos,
              "the four direct-light debug modes are isolated and activate the compositor on their own");
        Check(atmosphere.find("if(debugMode==13)return float4(localLightSum,1);") != std::string::npos &&
              atmosphere.find("localLightDebug.x") == std::string::npos &&
              atmosphere.find("if(li>=(int)localLightInfo.x) break;") != std::string::npos &&
              atmosphere.find("if(perpendicular2<radius2&&interval>0) {") != std::string::npos &&
              atmosphere.find("if(localRayPower>0.f){") != std::string::npos &&
              settings.find("13 local dynamic-light scattering") != std::string::npos,
              "volumetric diagnostics expose mode 13 and cull zero-power or missed lamp rays");
        Check(manager.find("light.stableId = StableManifestId(block);") != std::string::npos &&
              manager.find("std::isfinite(position.x)") != std::string::npos &&
              manager.find("kNativeEnableGraceMs = 700") != std::string::npos &&
              manager.find("kNativeObservationGraceMs = 1400") != std::string::npos,
              "manifest records receive distinct content-based IDs and malformed numeric records are rejected");
        // A lamp's existence may not depend on the camera's heading either. The pool
        // used to require re-observation inside the batch cadence (1.4 s), and the
        // client only submits an emitter's draws while it is in the frustum - so
        // turning away retired the lamp and turning back needed six frames before it
        // lit again. Both halves are pinned: the ordinary live test still needs the
        // confirmation, and a confirmed source is held across a much longer silence.
        Check(manager.find("const bool liveNow = slot.enabled || (recentlyEnabled && recentlySeen && confirmed);")
                  != std::string::npos &&
              manager.find("kNativeConfirmedHoldMs = 15000") != std::string::npos &&
              manager.find("const bool held = confirmed && slot.lastSeenTick != 0 &&") != std::string::npos &&
              manager.find("if (!liveNow && !held) continue;") != std::string::npos &&
              manager.find("(!slot.enabled &&\n                (!recentlyEnabled || !recentlySeen || slot.stableFrames < 6))")
                  == std::string::npos,
              "a confirmed lamp survives a head turn instead of leaving the pool");
        Check(settings.find("NormalSpreadPercent") == std::string::npos &&
              overlay.find("NormalSpreadPercent") == std::string::npos &&
              manager.find("NormalSpreadPercent") == std::string::npos,
              "the extra normal-spread control is removed from settings, F7 and runtime");
        Check(settings.find("LocalLightPercent") == std::string::npos &&
              settings.find("LocalLightThresholdPercent") == std::string::npos &&
              settings.find("LocalLightRadiusPixels") == std::string::npos &&
              settings.find("LocalLightDebugMask") == std::string::npos &&
              volume.find("CompositeLateLocalLights") == std::string::npos &&
              volume.find("localA.GetAddressOf()") == std::string::npos &&
              volumeEffects.find("localLightPixelSource") == std::string::npos,
              "dead legacy bright-pixel lights, controls and render targets are removed in favor of the live verified light pool");
        Check(settings.find("Quality=2") != std::string::npos &&
              overlay.find("L\"Quality\",\"LIGHT QUALITY (0 LOW 1 BAL 2 HIGH)\",0,2,1,2") != std::string::npos &&
              manager.find("L\"Quality\", 2,") != std::string::npos,
              "local-light quality defaults to full-resolution in the ini, F7 and runtime");
        Check(manager.find("attached_selected=") != std::string::npos &&
              manager.find("manifest_selected=") != std::string::npos &&
              manager.find("quality=") != std::string::npos &&
              manager.find("Selected source detail:") != std::string::npos &&
              manager.find(" id=") != std::string::npos &&
              manager.find(" position=(") != std::string::npos &&
              manager.find(" score=") != std::string::npos,
              "the periodic diagnostic records selected source identities, positions, scores and quality");
        Check(manager.find("m_selectionSignature = 0;") != std::string::npos &&
              manager.find("m_selectionSignature = local_light_detail::SelectionSignature(m_selected);") != std::string::npos &&
              managerHeader.find("SelectionSignature() const { return m_selectionSignature; }") != std::string::npos &&
              water.find("manager.SelectionSignature()") != std::string::npos,
              "the manager preserves the water-cache signature state and its production caller");
        Check(surface.find("float2 spread=texel.zw*(1+max(z,0)*normalTuning.z);") != std::string::npos,
              "the surface pass uses only its buffer texel and fixed distance growth");
        Check(surface.find("spread=max(round(spread/texel.xy),1)*texel.xy;") != std::string::npos &&
              Near(TapOffset({ 0.0304f, 0.0296f }, { 1.f / 480.f, 1.f / 270.f }).x, 15.f / 480.f, 1e-6f) &&
              Near(TapOffset({ 0.0001f, 0.0040f }, { 1.f / 480.f, 1.f / 270.f }).x, 1.f / 480.f, 1e-6f),
              "the surface pass rounds its window to whole depth texels");
        Check(surface.find("float iC=1./max(z,.001),iL=1./max(zL,.001),iR=1./max(zR,.001);") != std::string::npos &&
              surface.find("float absolute=max(normalTuning.x,z*normalTuning.y)/max(z*z,.001);") != std::string::npos,
              "the surface pass runs the edge test in inverse depth");
        Check(surface.find("float zL2=Linear(rawL2),zR2=Linear(rawR2),zU2=Linear(rawU2),zD2=Linear(rawD2);") != std::string::npos &&
              surface.find("float iL2=1./max(zL2,.001),iR2=1./max(zR2,.001),iU2=1./max(zU2,.001),iD2=1./max(zD2,.001);") != std::string::npos,
              "the surface pass linearizes the outer taps it feeds to the step test");
        Check(Near(InverseDepth(50.f), 0.02f, 1e-6f) && Near(InverseDepth(0.f), 1000.f, 1.f),
              "inverse depth stays finite at the camera");
        Check(surface.find("float fire=smoothstep(absolute,absolute*4,curv)*smoothstep(.25,.55,curv/max(disagreements,absolute));") != std::string::npos &&
              surface.find("float curv=abs(iL+iR-2*iC);") != std::string::npos &&
              surface.find("float disagreements=abs(iL-iC)+abs(iR-iC);") != std::string::npos,
              "the surface pass closes over a step at once, against both the noise floor and the terrain's own relief");
        Check(surface.find("float qL=surf2L>0?abs(iL2-2*iL+iC):0.;") != std::string::npos &&
              surface.find("float qR=surf2R>0?abs(iR2-2*iR+iC):0.;") != std::string::npos,
              "the surface pass names the step's side with a per-side second difference");
        Check(surface.find("float dropL=fire*smoothstep(.7,.9,qL/max(qL+qR,absolute));") != std::string::npos &&
              surface.find("float dropR=fire*smoothstep(.7,.9,qR/max(qL+qR,absolute));") != std::string::npos,
              "the surface pass drops only the side that stood alone with the step");
        Check(surface.find("float2 wx=AxisWeights(iL,iR,iC,iL2,iR2,absolute,surfL,surfR,surfL2,surfR2);") != std::string::npos &&
              surface.find("float2 wy=AxisWeights(iU,iD,iC,iU2,iD2,absolute,surfU,surfD,surfU2,surfD2);") != std::string::npos,
              "the surface pass feeds the outer taps to the step test on both axes");
        Check(surface.find("if(surfL<=0||surfR<=0) return float2(surfL>0?1.:0.,surfR>0?1.:0.);") != std::string::npos,
              "the surface pass hands a window that measured nothing to the other side");
        Check(surface.find("float surfL=step(rawL,.99990)*step(1e-20,dot(uvL-uv,uvL-uv));") != std::string::npos &&
              surface.find("float surfL2=step(rawL2,.99990)*step(1e-20,dot(uvL2-uv,uvL2-uv));") != std::string::npos,
              "the surface pass marks sky and the clamp's self-tap as no measurement");
        Check(surface.find("float vz=max(z,.25);") != std::string::npos &&
              surface.find("if(dL>2.5*dR&&dL>vz) surfL=0.;") != std::string::npos &&
              surface.find("if(dU>2.5*dD&&dU>vz) surfU=0.;") != std::string::npos,
              "the surface pass hands a horizon-flying tap to its partner");
        Check(surface.find("float3 worldUp=float3(0,0,1);") != std::string::npos &&
              surface.find("float hL=surfL>0?dot(pL-p,worldUp):0.,hR=surfR>0?dot(pR-p,worldUp):0.;") != std::string::npos &&
              surface.find("float curvX=abs(hL+hR),curvY=abs(hU+hD);") != std::string::npos &&
              surface.find("wL*=1.-smoothstep(gtol,gtol*2,curvX)*smoothstep(gtol,gtol*2,abs(hL));") != std::string::npos,
              "the surface pass gates standing geometry against world +Z, independent of the camera");
        Check(Near(SideStep(0.12f, 0.11f, 0.10f, true), 0.f, 1e-6f) &&
              Near(SideStep(0.14f, 0.13f, 0.10f, true), 0.02f, 1e-6f) &&
              SideStep(0.5f, 0.2f, 0.1f, false) == 0.f,
              "the per-side step is zero on a slope, |J| on a step, silent without its outer tap");
        Check(surface.find("float3 n=dot(c,c)>1e-12?normalize(c):-toEye;") != std::string::npos,
              "the surface pass falls back to a facing normal when the window measured nothing");
        Check(surface.find("float3 tx=((p-pL)*wL+(pR-p)*wR)/max(wL+wR,1e-4);") != std::string::npos,
              "the surface pass takes its tangent between the taps, not from their midpoint");
        Check(surface.find("float2 i=min(floor(saturate(uv)/texel.xy),1/texel.xy-1);") != std::string::npos &&
              surface.find("float2 uvL=Snap(uv-float2(spread.x,0)),uvR=Snap(uv+float2(spread.x,0));") != std::string::npos &&
              surface.find("float3 pL=World(uvL,rawL),pR=World(uvR,rawR);") != std::string::npos &&
              surface.find(" uv=Snap(uv);") != std::string::npos,
              "the surface pass reconstructs every point from the depth texel it sampled");
        Check(surface.find(".055*intact") != std::string::npos,
              "the surface pass gates the highlight by the surviving window");
        Check(surface.find("float2 spatial=float2(max(0,1.5-abs(o[i].x-f.x)),max(0,1.5-abs(o[i].y-f.y)));") != std::string::npos,
              "the composite blends a 1.5-texel tent over a 3x3 window");
        Check(surface.find("kernel*0.01") != std::string::npos,
              "the composite keeps its continuity floor at the wider kernel's share");
        Check(Near(tuning.edgeFloor, 0.10f, 1e-6f) && Near(tuning.edgeSlope, 0.004f, 1e-6f),
              "the core's shipped step scales are the scales the shader receives");
        Check(Near(compositeTuning.support, 1.5f, 1e-6f) && Near(compositeTuning.floor, .01f, 1e-6f),
              "the core's shipped kernel is the kernel the composite spells");
        Pass("the shipped shaders still mirror LocalLightingCore.h");
    }

    std::printf("local lighting regressions: all checks passed\n");
    return 0;
}
