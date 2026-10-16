#pragma once
// Portable core of the dynamic (local) light pool pass.
//
// This header is the ONE definition of the two things that decide whether a
// lamp pool reads as light or as grain:
//
//   * the receiver normal reconstructed from the scene depth buffer, and
//   * the depth-aware weights that blend the low-resolution light buffer into
//     the full-resolution scene.
//
// It compiles without windows.h so tools can run it directly:
//
//   * tools/LocalLightingRegression.cpp builds known scenes, encodes their
//     depth the way INTZ stores it, and pins the noise/silhouette behaviour
//     below at every quality;
//   * the two ps_3_0 shaders in src/Effects/LocalLightingRenderer.cpp
//     (kSurfaceSource, kCompositeSource) are deliberate line-by-line mirrors
//     of these functions, in the same spirit as AmbientOcclusionCore.h and its
//     two passes: a change here must be carried into the shader text in the
//     same commit.
//
// Why the normal needs this much care
// -----------------------------------
// A pool is a smooth, low-frequency field. Everything that makes it noisy is
// a per-pixel decision that flips between neighbours, and the depth-derived
// normal has three of them:
//
//   1. A footprint that is smaller than the texel being shaded. The buffer is
//      rendered at 0.5x/0.75x/1.0x of the screen, but a fixed one-pixel depth
//      difference is the SAME window at every quality, so at 0.75x the normal
//      field is undersampled against its own grid and its high-frequency part
//      aliases into visible grain. Footprint() therefore starts at one texel
//      of the buffer being shaded, not one texel of the depth buffer.
//   2. An absolute depth-step threshold for "this window straddles an edge".
//      Ground seen at a grazing angle changes view depth by yards per pixel -
//      far more than any fixed threshold - so the old test concluded "edge"
//      everywhere at range and silently degraded into a one-sided difference
//      of the terrain's own relief, one independent decision per pixel. The
//      test below is relative: an edge is a window whose two sides disagree
//      with each other MORE than they disagree with the surface's own slope,
//      and whose disagreement is also large in absolute yards.
//   3. A hard pick between the two sides of the window. Even a correct test
//      produces a binary switch that lands mid-surface wherever the metric
//      hovers at the threshold. Weights here are continuous, so the tangent
//      slides from the central difference to the one-sided one instead of
//      jumping.
//
// Nothing in this file animates: every value is a function of the depth buffer
// and the camera, so a standing camera sees a standing image.

#include <algorithm>
#include <cmath>

namespace renderer
{
namespace locallighting
{

struct Vec3 { float x = 0, y = 0, z = 0; };
inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
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

// ------------------------------------------------------- depth convention ---
// The pass samples raw INTZ, so A/B carry the viewport transform the client
// wrote with (including MaxZ < 1): raw = A + B / viewZ.
struct Projection
{
    float a = 0.f;   // raw depth of an infinitely distant point
    float b = 1.f;   // viewZ = b / (raw - a)
    float p00 = 1.f; // projection scale, x
    float p11 = 1.f; // projection scale, y
};

inline float LinearDepth(float raw, const Projection& p) { return p.b / (raw - p.a); }

// The three rows of the inverse view matrix, matching FrameContext::
// inverseView. D3D is row-vector (view = world * V), so the ROWS of the
// inverse are the world-space directions of the view axes: row 0 is right,
// row 1 is up, row 2 is forward.
struct InverseView
{
    Vec3 row[3] = { { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f } };
};

inline Vec3 ViewPosition(float u, float v, float z, const Projection& p)
{
    return { (u * 2.f - 1.f) / p.p00 * z, (1.f - v * 2.f) / p.p11 * z, z };
}

inline Vec3 WorldPosition(float u, float v, float raw, const Projection& p,
                          const InverseView& inv, const Vec3& camera)
{
    const Vec3 view = ViewPosition(u, v, LinearDepth(raw, p), p);
    return camera + inv.row[0] * view.x + inv.row[1] * view.y + inv.row[2] * view.z;
}

// -------------------------------------------------------------- footprint ---
struct NormalTuning
{
    // Smallest world-space window WIDTH (both taps together) the normal is
    // fitted over. Near the
    // camera one buffer texel is a fraction of an inch of ground, so the fit
    // would follow the terrain's own micro-relief and the pool would dapple;
    // this floor averages several relief features instead. It only binds
    // close up - at range a single texel already spans yards.
    float minWorldSpread = 0.35f;
    // Extra window growth per yard of view depth, on top of the buffer texel.
    // At range the depth buffer undersamples the terrain's own relief, so the
    // one-texel window is measuring aliased heights; widening it low-passes
    // that away. It is a fraction per yard because it must stay invisible
    // close up, where a texel is already a fraction of an inch.
    float distanceGrowth = 0.012f;
    // Absolute yard scales of the edge test: a window whose two sides
    // disagree by less than Floor + viewZ * Slope is on one surface no matter
    // how it compares to itself. Keeps quantization and gentle curvature from
    // reading as a silhouette.
    float edgeFloor = 0.10f;
    float edgeSlope = 0.004f;
    // Relative half of the edge test: how much of the window's own one-sided
    // depth change the two-sided disagreement must reach before the far side
    // is treated as off-surface. 0.30..0.85 keeps a sloped surface (small
    // disagreement relative to its slope) central and a step (disagreement
    // equal to its own size) one-sided.
    float relativeEdgeLow = 0.30f;
    float relativeEdgeHigh = 0.85f;
};

// The shipped tuning, with the F7 LIGHT SMOOTHING slider (ini
// `[DynamicLighting] NormalSpreadPercent`, 100 = shipped) applied to the one
// value that is an artistic trade: how much ground the normal averages over.
// Raising it smooths a pool further and rounds off small geometry; lowering
// it keeps every facet and brings the grain back with it. The edge scales are
// deliberately not part of the slider - they are a correctness property of the
// window, and a player should not be able to turn silhouette detection off.
inline NormalTuning DefaultNormalTuning(float spreadScale = 1.f)
{
    NormalTuning tuning;
    const float scale = std::isfinite(spreadScale) ? std::clamp(spreadScale, 0.25f, 4.f) : 1.f;
    tuning.minWorldSpread *= scale;
    return tuning;
}

struct Vec2 { float x = 0, y = 0; };

// A tap outside the frame has to be evaluated where the depth actually came
// from. The depth map is sampled with CLAMP addressing, so an off-frame tap
// returns the border texel's depth while its own uv would reconstruct a ray
// that never touched it - a point off the surface entirely, which tilts the
// window's normal in a band around the screen edge. Sampling and reconstructing
// from the same saturated uv makes the window shrink to the frame instead.
inline Vec2 ClampUv(Vec2 uv) { return { Saturate(uv.x), Saturate(uv.y) }; }

// ...and it has to be reconstructed from the texel that depth came from.
//
// A point sample returns the texel's stored depth, which belongs to the ray
// through the texel's CENTRE. Reconstructing it at the requested uv - anywhere
// inside that texel - puts the point off the surface by the depth gradient
// times up to half a texel, and at a grazing angle the depth gradient is yards
// per texel. On flat ground that error is the entire signal: the window's two
// taps land off the plane in opposite directions and the "normal" tilts by the
// ratio of that error to the window, which is why a pool over perfectly flat
// ground still had a grain floor nobody could tune away. Snapping costs nothing
// and makes every reconstructed point a point the depth buffer actually
// describes.
inline Vec2 SnapUv(Vec2 uv, Vec2 depthTexel)
{
    const Vec2 clamped = ClampUv(uv);
    const float ix = std::min(std::floor(clamped.x / depthTexel.x), 1.f / depthTexel.x - 1.f);
    const float iy = std::min(std::floor(clamped.y / depthTexel.y), 1.f / depthTexel.y - 1.f);
    return { (ix + .5f) * depthTexel.x, (iy + .5f) * depthTexel.y };
}

// The four taps are symmetric in texel space.
//
// A window that lands one texel to the left and two to the right reads as a
// step in any second difference, however flat the surface is - and at a grazing
// angle one texel is yards of depth, so it reads as a large step. Rounding the
// half-window to a whole number of depth texels per side makes the two taps
// mirror images around the receiver, which is what lets a plane measure as a
// plane instead of as its own perspective.
inline Vec2 TapOffset(Vec2 spread, Vec2 depthTexel)
{
    const float x = std::max(std::round(spread.x / depthTexel.x), 1.f);
    const float y = std::max(std::round(spread.y / depthTexel.y), 1.f);
    return { x * depthTexel.x, y * depthTexel.y };
}

// Half-window in uv units for the four depth taps that build the normal.
inline Vec2 Footprint(float viewZ, Vec2 bufferTexel, const Projection& p, const NormalTuning& tuning)
{
    const float depth = std::max(viewZ, 0.25f);
    const float world = tuning.minWorldSpread * 0.5f / depth;
    const float growth = 1.f + std::max(viewZ, 0.f) * tuning.distanceGrowth;
    return { std::max(bufferTexel.x * growth, world * p.p00),
             std::max(bufferTexel.y * growth, world * p.p11) };
}

// Mean world-space half-window that Footprint actually landed on: the depth
// taps sit on the view cone, so one uv step is this many yards at this
// distance. Diagnostics and tools read this; the shaders do not need it.
inline float FootprintWorld(float viewZ, Vec2 spread, const Projection& p)
{
    return viewZ * 0.5f * (spread.x / std::max(p.p00, 1e-4f) + spread.y / std::max(p.p11, 1e-4f));
}

// ----------------------------------------------------------------- normal ---
// Continuous edge weight for one axis of the window. `centre`, `near` and
// `far` are linear view depths at the centre tap and the two side taps.
// Returns a weight in [0,1] for each side: 1/1 on an intact surface (the
// central difference), sliding to 1/0 where the window straddles a step (the
// one-sided difference on the surviving side).
struct AxisWeights { float left = 1.f, right = 1.f; };

// `leftSurvives` / `rightSurvives` say whether that tap found a surface at all.
// The depth map answers that exactly: a clear value is the far plane, so a tap
// that reads it is sky, not a neighbour. Without this the window's tangent is
// built out of the ray that runs off to the horizon whenever the window reaches
// it, and since the sky's "depth" is the same on both sides the edge test below
// sees a perfectly symmetric window and keeps both weights at one - a horizon
// band of lamp light aimed by whatever direction that ray happened to have.
// Range of the "this tap is not the same surface" veto, for one axis, in
// inverse depth (see InverseDepth below).
//
// A side that stands this much further from the receiver than its partner is on
// something else and does not get to steer the tangent: at a hundred yards a
// wall one texel away is forty yards nearer, and even ten percent of that left
// in the tangent flattens the ground behind it.
//
// The scale is the PARTNER's deviation, not a fixed number of yards, because
// the partner is what proves the window is on a surface: over ground seen at a
// grazing angle both sides take in many yards of depth per pixel, and they do
// it together. A fixed threshold vetoes the ground itself at range; a ratio
// between the two sides cannot, and it is exactly the asymmetry a silhouette
// has and a grazing surface does not.
inline float TapVetoRange(float deviationA, float deviationB, float absolute)
{
    return 6.f * std::min(deviationA, deviationB) + absolute;
}

// The edge test runs in inverse depth, where every plane is flat.
//
// 1/z is affine in screen space for ANY plane under perspective, so a plane
// seen at a grazing angle has exactly zero curvature and symmetric one-sided
// deviations there, while a silhouette is still a jump. In yards the same
// ground curves: a flat road at the horizon is yards of depth per pixel and
// its own perspective trips an absolute "this window straddles a step" test,
// which is how the pool ended up treating flat ground at range as an edge.
inline float InverseDepth(float z) { return 1.f / std::max(z, 1e-3f); }

inline AxisWeights TapWeights(float zLeft, float zRight, float zCentre, const NormalTuning& tuning,
                              bool leftSurvives = true, bool rightSurvives = true)
{
    const float centre = InverseDepth(zCentre);
    // Two-sided disagreement: what a step looks like. Zero for any plane the
    // window fits, however steeply it is sloped, and for any surface the four
    // taps agree on.
    const float curvature = std::abs(InverseDepth(zLeft) + InverseDepth(zRight) - 2.f * centre);
    const float left = std::abs(InverseDepth(zLeft) - centre);
    const float right = std::abs(InverseDepth(zRight) - centre);
    // The tuning's yard-scale tolerance, carried into inverse depth (a yard at
    // z is 1/z^2 there), so it keeps meaning the same thing on the ground.
    const float absolute = std::max(tuning.edgeFloor, zCentre * tuning.edgeSlope) /
                           std::max(zCentre * zCentre, 1e-3f);
    // Both halves must agree before a side is dropped: absolutely large, and
    // large relative to the window's own one-sided change.
    const float edge = Smoothstep(absolute, absolute * 4.f, curvature) *
                       Smoothstep(tuning.relativeEdgeLow, tuning.relativeEdgeHigh,
                                  curvature / std::max(left + right, absolute));
    // The outlier is the side that moved further from the receiver. Signed so a
    // symmetric window (curvature ~ 0) keeps both weights at one, and
    // normalized by the LARGER side rather than by their sum: the sum version
    // saturates, so a side standing four times as far off the surface as its
    // partner still kept a third of the weight - enough to pull the tangent off
    // the surface it was supposed to describe, which is what a distant
    // occluder's top edge did to the ground behind it.
    const float asymmetry = (left - right) / std::max(std::max(left, right), 1e-4f);
    const float veto = TapVetoRange(left, right, absolute);
    const bool leftOnSurface = leftSurvives && left <= veto;
    const bool rightOnSurface = rightSurvives && right <= veto;
    AxisWeights weights;
    weights.left = leftOnSurface ? 1.f - edge * Smoothstep(0.f, 1.f, asymmetry) : 0.f;
    weights.right = rightOnSurface ? 1.f - edge * Smoothstep(0.f, 1.f, -asymmetry) : 0.f;
    // A side that measured nothing - off the frame, or the sky beyond it - was
    // dropped above; but left to the edge test such a side reads as "the two
    // sides disagree by everything" and takes the surviving side down with it,
    // collapsing the tangent and the normal to zero. One surviving side is the
    // whole axis.
    if (left <= 0.f && right > 0.f) { weights.left = 0.f; weights.right = 1.f; }
    if (right <= 0.f && left > 0.f) { weights.left = 1.f; weights.right = 0.f; }
    return weights;
}

struct SurfaceFrame
{
    Vec3 normal{};
    // How much of the window survived as one surface: 1 on intact ground,
    // lower where the fit had to drop a side. Consumers use it to decide how
    // much detail-level response (specular) to trust.
    float intact = 1.f;
};

// Weighted central difference -> plane normal, oriented toward the eye.
// `pX` are world positions at the four taps of a cross window.
// Which of the four taps found a surface (see TapWeights).
struct TapSurvival
{
    bool left = true, right = true, up = true, down = true;
};

inline SurfaceFrame ReconstructNormal(const Vec3& p, const Vec3& pLeft, const Vec3& pRight,
                                      const Vec3& pUp, const Vec3& pDown, float zLeft, float zRight,
                                      float zUp, float zDown, float zCentre, const Vec3& toEye,
                                      const NormalTuning& tuning, const TapSurvival& survives = {})
{
    const AxisWeights x = TapWeights(zLeft, zRight, zCentre, tuning, survives.left, survives.right);
    const AxisWeights y = TapWeights(zUp, zDown, zCentre, tuning, survives.up, survives.down);
    const Vec3 tangentX = (p - pLeft) * x.left + (pRight - p) * x.right;
    const Vec3 tangentY = (p - pUp) * y.left + (pDown - p) * y.right;
    const float weightSumX = std::max(x.left + x.right, 1e-4f);
    const float weightSumY = std::max(y.left + y.right, 1e-4f);
    SurfaceFrame frame;
    const Vec3 cross = Cross(tangentX * (1.f / weightSumX), tangentY * (1.f / weightSumY));
    // Both axes can come back empty - a pixel of ground at the horizon whose
    // whole window reaches the sky. There is no surface in that window to
    // measure, so the honest answer is the flattest one facing the viewer
    // rather than the cross product of two remnants of a sky ray, which is
    // whatever direction the float noise happened to point.
    frame.normal = Dot(cross, cross) > 1e-12f ? Normalize(cross) : toEye;
    if (Dot(frame.normal, toEye) < 0.f) frame.normal = frame.normal * -1.f;
    frame.intact = Saturate((x.left + x.right + y.left + y.right) * 0.25f);
    return frame;
}

// ------------------------------------------------------------------- pool ---
struct LightSample
{
    Vec3 position{};
    Vec3 direction{ 0.f, 0.f, -1.f };
    Vec3 color{ 1.f, 1.f, 1.f };
    float radius = 12.f;
    float intensity = 1.f;
    float outerCone = -1.f; // < -0.5 == point light
};

// One light's contribution at one receiver. Mirrors the inner loop of
// kSurfaceSource exactly, including the specular exponent: a pool's shape is
// as much a part of the shipped look as its smoothness.
inline float SpecularPower() { return 24.f; }
inline float SpecularScale() { return 0.055f; }

inline Vec3 PoolShade(const LightSample& light, const Vec3& p, const SurfaceFrame& frame,
                      const Vec3& camera)
{
    const Vec3 toLightVector = light.position - p;
    const float distance = Length(toLightVector);
    const Vec3 toLight = distance > 1e-4f ? toLightVector * (1.f / distance) : Vec3{ 0.f, 0.f, 1.f };
    const float falloff = Saturate(1.f - distance / std::max(light.radius, 0.01f));
    float attenuation = falloff * falloff / (1.f + distance * distance * 0.035f);
    if (light.outerCone > -0.5f)
    {
        const Vec3 spotAxis = Normalize(light.direction);
        attenuation *= Smoothstep(light.outerCone, std::min(1.f, light.outerCone + 0.12f),
                                  Dot(toLight * -1.f, spotAxis));
    }
    const Vec3 toEye = Normalize(camera - p);
    const float diffuse = Saturate(Dot(frame.normal, toLight));
    // reflect(-toLight, normal) in HLSL spelling: incident minus twice its
    // projection on the normal, with the incident being the vector TOWARD the
    // light negated.
    const Vec3 incident = toLight * -1.f;
    const Vec3 reflected = incident - frame.normal * (2.f * Dot(incident, frame.normal));
    // A mirror highlight is only as trustworthy as the normal that produced
    // it: pow(24) turns one jittering normal into one bright speck, and a
    // speck is exactly what survives every other smoothing step. `intact` is
    // one on an untouched window, so this changes nothing on flat ground.
    const float spec = std::pow(Saturate(Dot(reflected, toEye)), SpecularPower()) *
                       SpecularScale() * frame.intact;
    return light.color * (light.intensity * (diffuse + spec) * attenuation);
}

// --------------------------------------------------------------- composite ---
// Depth-aware blend of the low-resolution light buffer into the scene.
struct CompositeTuning
{
    // Absolute yard tolerance for "this light texel belongs to my surface".
    float toleranceFloor = 0.12f;
    float toleranceSlope = 0.008f;
    // Extra tolerance per unit of depth slope across a full-resolution texel,
    // so a continuous ramp does not read as a stack of silhouettes.
    float slopeGain = 1.5f;
    // Weight kept for a tap that is decisively behind the receiver: without
    // this floor the blend goes winner-takes-all along the buffer grid, which
    // is the 1-2 pixel "line" this pass used to draw at sub-1.0 quality. The
    // 3x3 kernel has twice the taps the bilinear 2x2 had, so the floor is
    // halved to keep the share a masked-out side can contribute the same.
    float floor = 0.01f;
    // Spatial kernel support in buffer texels. 1.5 keeps the outer ring of a
    // 3x3 window alive at every subpixel phase, which is what averages the
    // remaining grain out of the pool; 1.0 is a plain bilinear 2x2.
    float support = 1.5f;
};

inline float CompositeSpatialWeight(float offset, float fraction, const CompositeTuning& tuning)
{
    return std::max(0.f, tuning.support - std::abs(offset - fraction));
}

inline float CompositeDepthWeight(float tapDepth, float receiverDepth, float slope,
                                  float resolutionRatio, const CompositeTuning& tuning)
{
    const float distance = std::abs(tapDepth - receiverDepth);
    const float tolerance = std::max(std::max(tuning.toleranceFloor, receiverDepth * tuning.toleranceSlope),
                                     slope * resolutionRatio * tuning.slopeGain);
    return std::exp(-distance / std::max(tolerance, 0.01f) * 3.f);
}

// Depth slope across one full-resolution texel, for the tolerance above: the
// SMALLER one-sided difference on each axis, so a silhouette (where one side
// is a different surface entirely) does not inflate the slope and open the
// blend across itself. Shared verbatim by the two passes that upsample a
// low-resolution light field - the direct lamp composite and the global
// illumination composite.
inline float CombineSlope(float left, float right, float up, float down, float centre)
{
    return std::min(std::abs(left - centre), std::abs(right - centre)) +
           std::min(std::abs(up - centre), std::abs(down - centre));
}

} // namespace locallighting
} // namespace renderer
