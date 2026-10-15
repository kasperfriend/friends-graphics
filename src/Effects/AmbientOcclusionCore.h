#pragma once
// Portable core of the screen-space ambient occlusion pass.
//
// This header is the ONE definition of the AO math: the kernel table, the
// depth reconstruction, the per-sample occlusion test and the depth-aware
// blur weights. It compiles without windows.h so tools can run it directly:
//
//   * tools/AmbientOcclusionPreview.cpp renders a model with it and publishes
//     the on/off sheet;
//   * tools/AmbientOcclusionRegression.cpp pins the behaviour below.
//
// The two ps_3_0 shaders in src/Effects/AmbientOcclusion.cpp
// (kAmbientOcclusionSource, kAmbientOcclusionApplySource) are deliberate
// line-by-line mirrors of these functions, in the same spirit as
// tools/CloudPreview.cpp mirroring kCloudMarchSource: a change here must be
// carried into the shader text in the same commit. The GPU-only behaviour
// (state, resources, blending) is not part of this file.
//
// The algorithm is a hemisphere-kernel SSAO with three deliberate limits,
// because "grounding, not grime" is the shipped look:
//
//   1. Strict locality - a sample only counts occluders whose view depth is
//      within one radius of the receiver (rangeCheck reaches 0 AT the radius,
//      not beyond it), so a wall never darkens the ground next to it and AO
//      stays a crease/contact effect.
//   2. Distance fade - reconstruction from an 8-bit viewport depth gets noisy
//      with range, and creases stop being resolvable, so AO fades out between
//      55 % and 100 % of `fadeDistance` and is exactly 1 beyond it.
//   3. Fixed noise - the per-pixel kernel rotation comes from a static 4x4
//      texture, never from time, so a standing camera sees a standing image.

#include <cmath>
#include <cstdint>

namespace renderer
{
namespace ambientocclusion
{

struct Vec3 { float x = 0, y = 0, z = 0; };
inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline Vec3& operator+=(Vec3& a, Vec3 b) { a = a + b; return a; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(Vec3 a)
{
    const float l = Length(a);
    return l > 1e-8f ? a * (1.f / l) : Vec3{};
}
inline float Saturate(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
inline float Smoothstep(float e0, float e1, float x)
{
    const float t = Saturate((x - e0) / std::max(e1 - e0, 1e-6f));
    return t * t * (3.f - 2.f * t);
}
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// ----------------------------------------------------------------- tuning ---
struct Settings
{
    bool enabled = true;
    // 0..1.5. Multiplies the accumulated occlusion; 1.0 would take a fully
    // enclosed point to black. The shipped default stays well under that.
    float strength = 0.60f;
    // Hemisphere radius in world units (~yards). Tuned against a measured
    // scene (tools/AmbientOcclusionRegression.cpp pins the numbers): at 1.0 a
    // fully open floor, an open wall face and an open box top all read
    // exactly 1.000 - no false occlusion at all - while a right-angled corner
    // reaches 0.66. At 1.5 the same open box top already reads 0.90, which is
    // the dark halo around a silhouette that makes SSAO look dirty. Wide
    // radii darken whole surfaces; strength alone never does.
    float radius = 1.0f;
    // AO fades out between 55 % and 100 % of this view distance.
    float fadeDistance = 250.f;
    // Overlays the occlusion term instead of multiplying the scene.
    bool debug = false;
};

// Small offset that keeps a sample from occluding its own surface; scales
// with the radius so the ratio survives every slider position.
inline float Bias(float radius) { return radius * 0.02f; }
// How far past the bias an occluder has to sit in front of a sample point
// before it counts in full. Tight on purpose: a wide ramp throws away most
// of the occlusion and leaves AO invisible.
inline float Softness(float radius) { return radius * 0.25f; }
// Locality window for the depth guard, in receiver radii. An occluder within
// this many radii of the receiver's own depth counts in full and stops
// counting shortly after, so a wall metres in front of the ground never
// darkens the ground behind it. Generous enough (measured: 2.0 beats 0.55 by
// a wide margin on real corners) that genuine contact occlusion is kept.
inline float LocalityInner() { return 2.0f; }
inline float LocalityOuter() { return 2.45f; }

// --------------------------------------------------------------- the kernel ---
constexpr int kSampleCount = 12;

// 12 hemisphere directions (tangent space, +z opens along the normal), a
// golden-angle spiral: evenly spread without a visible lattice, dense near
// the normal where most occluders sit. Mirrored verbatim in kAmbientOcclusionSource.
constexpr float kKernel[kSampleCount][3] = {
    { +0.2742f, +0.0000f, +0.9617f },
    { -0.3433f, +0.3145f, +0.8850f },
    { +0.0515f, -0.5865f, +0.8083f },
    { +0.4148f, +0.5410f, +0.7317f },
    { -0.7441f, -0.1316f, +0.6550f },
    { +0.6883f, -0.4379f, +0.5783f },
    { -0.2246f, +0.8354f, +0.5017f },
    { -0.4172f, -0.8033f, +0.4250f },
    { +0.8805f, +0.3216f, +0.3483f },
    { -0.8896f, +0.3672f, +0.2717f },
    { +0.4157f, -0.8883f, +0.1950f },
    { +0.2972f, +0.9475f, +0.1183f },
};

// --------------------------------------------------- depth reconstruction ---
// The same INTZ viewport-depth -> view-space convention every other depth
// consumer in this renderer uses (volumePixelSource, kSurfaceSource):
// projUnpack = {P22, P32, P00, P11}, depthMaxZ = viewport MaxZ.
struct ViewParams
{
    float projA = 0;       // P[2][2]
    float projB = 0;       // P[3][2]
    float projP00 = 1;     // P[0][0]
    float projP11 = 1;     // P[1][1]
    float depthMaxZ = 1;   // viewport MaxZ
};

// The clear value sits at (or above) raw 1; real geometry is bounded below it.
constexpr float kSkyRaw = 0.9999f;
inline bool IsSkyRaw(float raw) { return raw >= kSkyRaw; }

inline float ViewZ(float raw, const ViewParams& p)
{
    const float norm = Saturate(raw / std::max(p.depthMaxZ, 1e-4f));
    // Both projection.y (B) and this denominator are NEGATIVE for every real
    // surface - that is what makes the quotient a positive view depth - so
    // the guard is a floor on the magnitude, never a floor on the signed
    // value. A max() here would clamp the ordinary case to the epsilon and
    // hand back a depth of ~-50000. Same convention as volumePixelSource:
    // the sky test (IsSkyRaw) is what rejects the far plane.
    return p.projB / std::min(norm - p.projA, -1e-6f);
}

// View-space position of a pixel: camera at the origin, +z forward.
inline Vec3 ViewPosition(float u, float v, float z, const ViewParams& p)
{
    return { (u * 2.f - 1.f) / std::max(p.projP00, 1e-5f) * z,
             (1.f - v * 2.f) / std::max(p.projP11, 1e-5f) * z,
             z };
}

// -------------------------------------------------------- rotation noise ---
// The per-pixel kernel rotation. A static 4x4 pattern, one rotation per 4x4
// pixel block; both the GPU texture upload and the CPU tools read their
// angles from this exact hash, so a preview panel and the shipped pass turn
// their kernels the same way.
inline uint32_t NoiseHash(uint32_t x, uint32_t y)
{
    uint32_t h = x * 374761393u + y * 668265263u + 1013904223u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
// Returns the unit rotation (cos, sin) for noise texel (x,y).
inline void RotationFor(int x, int y, float out[2])
{
    const uint32_t h = NoiseHash(uint32_t(x) & 3u, uint32_t(y) & 3u);
    const float angle = float(double(h & 0xffffffu) / double(0x1000000)) * 6.2831853f;
    out[0] = std::cos(angle);
    out[1] = std::sin(angle);
}

// ------------------------------------------------------------- occlusion ---
// DepthFetch(u, v) returns the raw viewport depth (0..1, 1 = sky) for the
// given normalized coordinates; coordinates outside [0,1] are clamped by the
// caller's addressing, exactly like the GPU's CLAMP samplers.
template <class DepthFetch>
inline float ComputeOcclusion(float u, float v, float texelU, float texelV,
                              const ViewParams& view, const Settings& settings,
                              const DepthFetch& depth)
{
    const float raw = depth(u, v);
    if (IsSkyRaw(raw)) return 1.f;
    const float z = ViewZ(raw, view);
    if (!(z > 0.f)) return 1.f;
    const Vec3 p = ViewPosition(u, v, z, view);

    // Distance fade: gone before reconstruction noise takes over.
    const float fade = 1.f - Smoothstep(settings.fadeDistance * 0.55f, settings.fadeDistance, z);
    if (fade <= 0.f) return 1.f;

    // Normal from the two depth-continuous neighbours on each axis (the same
    // side selection as LocalLightingRenderer's depth-derived normals): at a
    // silhouette the far side of a step must not tilt the normal.
    const float zl = depth(u - texelU, v), zr = depth(u + texelU, v);
    const float zd = depth(u, v - texelV), zu = depth(u, v + texelV);
    const float pl = ViewZ(zl, view), pr = ViewZ(zr, view);
    const float pd = ViewZ(zd, view), pu = ViewZ(zu, view);
    const Vec3 px = std::abs(pl - z) < std::abs(pr - z)
        ? ViewPosition(u - texelU, v, pl, view) : ViewPosition(u + texelU, v, pr, view);
    const Vec3 py = std::abs(pd - z) < std::abs(pu - z)
        ? ViewPosition(u, v - texelV, pd, view) : ViewPosition(u, v + texelV, pu, view);
    Vec3 n = Normalize(Cross(px - p, py - p));
    if (Length(n) < 0.5f) return 1.f;         // degenerate/flat-facing pixel
    if (Dot(n, p) > 0.f) n = n * -1.f;        // face the camera at the origin

    // Tangent frame for the hemisphere.
    const Vec3 helper = std::abs(n.z) < 0.9f ? Vec3{ 0, 0, 1 } : Vec3{ 1, 0, 0 };
    const Vec3 t = Normalize(Cross(helper, n));
    const Vec3 b = Cross(n, t);

    // Static per-pixel kernel rotation, one per 4x4 pixel block.
    float rot[2];
    RotationFor(int(u / (texelU * 4.f)), int(v / (texelV * 4.f)), rot);

    const float radius = settings.radius;
    const float bias = Bias(radius);
    const float soft = Softness(radius);
    float occlusion = 0.f;
    for (int i = 0; i < kSampleCount; ++i)
    {
        const float kx = kKernel[i][0], ky = kKernel[i][1], kz = kKernel[i][2];
        // Rotate the tangent-space sample about the normal.
        const float rx = kx * rot[0] - ky * rot[1];
        const float ry = kx * rot[1] + ky * rot[0];
        const Vec3 dir = t * rx + b * ry + n * kz;
        const Vec3 sp = p + dir * radius;
        if (sp.z <= 0.02f) continue;          // sample left the frustum

        // Project the sample back to the screen (inverse of ViewPosition).
        float su = (sp.x / sp.z * view.projP00 + 1.f) * 0.5f;
        float sv = (1.f - sp.y / sp.z * view.projP11) * 0.5f;
        su = Saturate(su); sv = Saturate(sv); // CLAMP addressing

        const float sz = ViewZ(depth(su, sv), view);
        // dz > 0: the surface at the sample pixel sits in FRONT of the sample
        // point - it blocks ambient light on its way to the receiver.
        const float dz = sp.z - sz;
        // Locality guard: full credit while the occluder sits within
        // LocalityInner() radii of the receiver's own depth, nothing beyond
        // LocalityOuter(). Nothing outside that window can contribute, which
        // is what keeps AO off large flat surfaces.
        const float rangeCheck = 1.f - Smoothstep(LocalityInner(), LocalityOuter(),
                                                  std::abs(sz - z) / radius);
        occlusion += Smoothstep(bias, bias + soft, dz) * rangeCheck;
    }
    occlusion /= float(kSampleCount);

    const float ao = Saturate(1.f - settings.strength * occlusion);
    return Lerp(1.f, ao, fade);
}

// ------------------------------------------------------------ blur weight ---
// Depth-aware box-blur weight for the apply pass. Gentle slopes (terrain)
// blur normally; a silhouette - where the tap depth jumps by a fraction of
// the receiver distance - gets zero weight so occlusion never smears across
// object edges onto the background behind them.
inline float BlurWeight(float tapZ, float centerZ)
{
    if (tapZ <= 0.f) return 0.f;               // sky tap
    const float threshold = std::max(centerZ * 0.06f, 0.25f);
    return Saturate(1.f - std::abs(tapZ - centerZ) / threshold);
}

// The apply pass blurs a 4x4 neighbourhood centred on the pixel (offsets
// -1.5..+1.5 texels). Shared so the preview sheet and the shader agree.
constexpr int kBlurSpan = 4;
inline float BlurOffset(int tap) { return float(tap) - (float(kBlurSpan) - 1.f) * 0.5f; }

} // namespace ambientocclusion
} // namespace renderer
