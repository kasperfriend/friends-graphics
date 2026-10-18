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
//   2. A depth-step test for "this window straddles an edge" that survives
//      the grazing angle. Ground seen at a grazing angle changes view depth by
//      yards per pixel - far more than any fixed threshold - so the old test
//      concluded "edge" everywhere at range and silently degraded into a
//      one-sided difference of the terrain's own relief. The test below runs
//      on the two-sided disagreement in inverse depth, where the plane's own
//      slope cancels out: the disagreement IS a foreign tap's depth jump. It
//      is judged against BOTH scales the window knows - a yard-scale noise
//      floor, and the window's own one-sided changes, which are the terrain's
//      own relief - and the band closes over a step at once. The old band's
//      partial close left half of a nearby object's tap steering the tangent,
//      which is what painted shape-copies of characters and objects into the
//      pool beside them.
//   3. A pick of WHICH side carried the step - a per-side second difference
//      over the outer taps (see SideStep), because a three-tap window cannot
//      tell: the slope step and the jump trade off exactly, and every rule
//      keyed on their comparison or on the deviation gap guessed wrong the
//      moment a nearby object's jump came up small against the grazing slope.
//      Weights are continuous in the evidence, so the tangent slides from the
//      central difference to the one-sided one instead of jumping.
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

// Dynamic-light debug views are renderable without a selected source (mode 4
// visualizes receiver-normal confidence, and the source modes use black for an
// empty light set). Keep this policy portable so the compositor, renderer and
// regression suite cannot disagree about whether the pass should run.
inline bool IsDebugView(int mode) noexcept { return mode >= 1 && mode <= 4; }
inline bool ShouldActivate(bool enabled, float intensityScale, int debugMode) noexcept
{
    return enabled && ((std::isfinite(intensityScale) && intensityScale > 0.f) || IsDebugView(debugMode));
}

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
    // The normal footprint grows slightly with view depth, on top of the
    // light-buffer texel. At range a texel spans yards and the receiver depth
    // is less precise; this bounded growth keeps the finite-difference estimate
    // stable without adding a world-space smoothing floor that crosses objects.
    float distanceGrowth = 0.012f;
    // Absolute yard scales of the step test: a window whose two sides
    // disagree by less than Floor + viewZ * Slope is on one surface no matter
    // how it compares to itself. Keeps quantization and gentle curvature from
    // reading as a silhouette. The disagreement itself is slope-independent
    // (the plane's own slope cancels out of it), so - unlike a comparison
    // against the window's one-sided changes - this still fires when a nearby
    // object's depth jump is smaller than the window's grazing slope step.
    float edgeFloor = 0.10f;
    float edgeSlope = 0.004f;
};

// Fixed, edge-aware reconstruction tuning. The depth window is the light-buffer
// texel (with a small range-growth term); there is no extra world-space blur.
// The edge scales are correctness tests, not artistic controls.
inline NormalTuning DefaultNormalTuning()
{
    return {};
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
inline Vec2 Footprint(float viewZ, Vec2 bufferTexel, const NormalTuning& tuning)
{
    const float growth = 1.f + std::max(viewZ, 0.f) * tuning.distanceGrowth;
    return { bufferTexel.x * growth, bufferTexel.y * growth };
}

// Mean world-space half-window that Footprint actually landed on: the depth
// taps sit on the view cone, so one uv step is this many yards at this
// distance. Diagnostics and tools read this; the shaders do not need it.
inline float FootprintWorld(float viewZ, Vec2 spread, const Projection& p)
{
    return viewZ * 0.5f * (spread.x / std::max(p.p00, 1e-4f) + spread.y / std::max(p.p11, 1e-4f));
}

// ----------------------------------------------------------------- normal ---
// Continuous step weight for one axis of the window. Returns a weight in [0,1]
// for each side: 1/1 on an intact surface (the central difference), sliding to
// 0 where that side of the window straddles a step (the one-sided difference on
// the other side).
struct AxisWeights { float left = 1.f, right = 1.f; };

// The step test runs in inverse depth, where every plane is flat.
//
// 1/z is affine in screen space for ANY plane under perspective, so a plane
// seen at a grazing angle has exactly zero curvature there, while a silhouette
// is still a jump. In yards the same ground curves: a flat road at the horizon
// is yards of depth per pixel and its own perspective trips an absolute "this
// window straddles a step" test, which is how the pool ended up treating flat
// ground at range as an edge.
inline float InverseDepth(float z) { return 1.f / std::max(z, 1e-3f); }

// Which side of the window carried a step. This is a SECOND DIFFERENCE along
// one side of the window (outer, inner, receiver), not across it:
//
//   q = |i(2w) - 2 i(w) + iC|
//
// The plane's own slope cancels out of any second difference, so q is exactly
// |J| - the depth jump of whatever foreign surface that side reached onto -
// however steeply the ground is sloped, and whether the foreign surface is
// nearer or further than the receiver. Two facts make this the right test:
//
//   1. A three-tap window (inner, receiver, inner) cannot say WHICH side its
//      step is on: the plane's slope step and the jump trade off exactly, and
//      every "sign of the deviation gap" rule guesses wrong the moment a
//      nearby object's jump is smaller than that slope step - the "go stand
//      next to it" regime. That painted either shape-copies of nearby
//      characters and objects into the pool (the foreign tap kept steering the
//      tangent) or dark streaks (the wrong side dropped, the tangent pinned
//      onto the foreign tap).
//   2. A step between the receiver and the inner tap shows up in BOTH
//      second-difference forms on that side - q above - no matter which of the
//      three points the jump sits on, because the linear part of the foreign
//      surface cancels the same way the receiver's does.
//
// A genuine gentle curve stays under the noise floor and keeps the central
// difference; a crease hard enough to read as a step goes one-sided there,
// which is the trade every version of this test has made.
//
// `outerSurvives` for a tap that measured nothing (sky, or the frame clamp's
// self-tap) leaves q at zero: no evidence on that side means no drop.
inline float SideStep(float outer, float inner, float centre, bool outerSurvives)
{
    return outerSurvives ? std::abs(outer - 2.f * inner + centre) : 0.f;
}

// Weighted central difference -> plane normal, oriented toward the eye.
// `pX` are world positions at the four inner taps of a cross window; the outer
// taps at twice the distance only contribute their depth to the step test.
// Which of the taps found a surface (see TapSurvival below). "Found a surface"
// is not only the depth map's clear value: a tap that snapped onto the
// receiver's own texel - the frame clamp of an off-frame window - measured
// nothing and must also be marked dead. It carries the receiver's own depth
// with zero deviation, which reads as the flattest possible neighbour and
// would otherwise take over the axis at the frame edges.
struct TapSurvival
{
    bool left = true, right = true, up = true, down = true;
    bool left2 = true, right2 = true, up2 = true, down2 = true;
};

// One axis of the window, in inverse depth. `z1X` are the inner taps (the
// tangent distance), `z2X` the outer taps at twice the distance (the step
// test's other point).
inline AxisWeights TapWeights(float zLeft, float zRight, float zCentre,
                              float zLeft2, float zRight2, const NormalTuning& tuning,
                              bool leftSurvives = true, bool rightSurvives = true,
                              bool left2Survives = true, bool right2Survives = true)
{
    const float centre = InverseDepth(zCentre);
    const float left = InverseDepth(zLeft);
    const float right = InverseDepth(zRight);
    // The tuning's yard-scale tolerance, carried into inverse depth (a yard at
    // z is 1/z^2 there), so it keeps meaning the same thing on the ground.
    const float absolute = std::max(tuning.edgeFloor, zCentre * tuning.edgeSlope) /
                           std::max(zCentre * zCentre, 1e-3f);
    // Does this window straddle a step at all? The two-sided disagreement in
    // inverse depth (exactly a foreign tap's depth jump - the plane's own
    // slope cancels out of it) against BOTH scales the window knows: the yard
    // noise floor, and the window's own one-sided changes, which are the
    // terrain's own relief. Comparing against the relief is what keeps the
    // pool following real ground; the band closes over a step at once instead
    // of the old [.30,.85] slide, which left half of a nearby object's tap
    // steering the tangent - the shape-copies.
    const float curvature = std::abs(left + right - 2.f * centre);
    const float disagreements = std::abs(left - centre) + std::abs(right - centre);
    const float fire = Smoothstep(absolute, absolute * 4.f, curvature) *
                       Smoothstep(0.25f, 0.55f, curvature / std::max(disagreements, absolute));
    // Which side carried the step: a per-side second difference
    // (locallighting::SideStep). A three-tap window cannot say - the slope
    // step and the jump trade off exactly - and a rule keyed on the deviation
    // gap guesses wrong the moment a nearby object's jump comes up small
    // against the grazing slope, pinning the tangent onto the object and
    // painting dark streaks out of it. A step stands alone on its side; a
    // genuine curve shows up on both sides alike.
    const float qLeft = SideStep(InverseDepth(zLeft2), left, centre, left2Survives);
    const float qRight = SideStep(InverseDepth(zRight2), right, centre, right2Survives);
    const float dropLeft = fire * Smoothstep(0.7f, 0.9f, qLeft / std::max(qLeft + qRight, absolute));
    const float dropRight = fire * Smoothstep(0.7f, 0.9f, qRight / std::max(qLeft + qRight, absolute));
    AxisWeights weights;
    // A side that measured nothing - off the frame's clamp, or the sky beyond
    // it - hands the axis to the other side whole. The drops above only
    // arbitrate between two sides that both measured a surface.
    if (!leftSurvives || !rightSurvives)
    {
        weights.left = leftSurvives ? 1.f : 0.f;
        weights.right = rightSurvives ? 1.f : 0.f;
        return weights;
    }
    weights.left = 1.f - dropLeft;
    weights.right = 1.f - dropRight;
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

// `worldUp` is WoW's fixed world-vertical axis (+Z), not the camera's screen-up
// vector. A character's standing height remains world-vertical when the camera
// pitches or rolls; using camera-up makes the gate disappear at steep angles.
inline SurfaceFrame ReconstructNormal(const Vec3& p, const Vec3& pLeft, const Vec3& pRight,
                                      const Vec3& pUp, const Vec3& pDown,
                                      float zLeft, float zRight, float zUp, float zDown,
                                      float zLeft2, float zRight2, float zUp2, float zDown2,
                                      float zCentre, const Vec3& toEye,
                                      const NormalTuning& tuning, const TapSurvival& survives,
                                      const Vec3& worldUp)
{
    // HORIZON-FLIGHT VETO. The window is a fixed UV span, but its world size
    // depends on the ray: the sideways taps (left/right) land a world window
    // away, while the up tap of a steep down-look is the same ray barely
    // above the horizon, which lands tens of yards out and turns the up-axis
    // tangent into a long chord of the terrain - its direction is the
    // relief's average over that span, not the local slope. That can create
    // a wrong-light band near the horizon even with a one-texel footprint.
    // The test must be per axis: sideways taps are closer than along-ray taps
    // by construction, so a tap is only foreign to the window when it outruns
    // its own partner on the same axis - and it must outrun the receiver's
    // own view depth:
    // a tap further out than the receiver is a ray skimming the horizon,
    // while a tap that merely outruns its partner can be one hitting a
    // nearby relief bump (both taps are within the receiver's depth, and
    // both are measured). More than 2.5x the partner beyond the depth is
    // that flight (or a ray that left a vertical face); the side is handed
    // to the partner whole, exactly as if it had measured sky.
    TapSurvival stays = survives;
    if (stays.left && stays.right)
    {
        const float dL = Length(pLeft - p), dR = Length(pRight - p);
        if (dL > 2.5f * dR && dL > zCentre) stays.left = false;
        else if (dR > 2.5f * dL && dR > zCentre) stays.right = false;
    }
    if (stays.up && stays.down)
    {
        const float dU = Length(pUp - p), dD = Length(pDown - p);
        if (dU > 2.5f * dD && dU > zCentre) stays.up = false;
        else if (dD > 2.5f * dU && dD > zCentre) stays.down = false;
    }

    const AxisWeights x = TapWeights(zLeft, zRight, zCentre, zLeft2, zRight2, tuning,
                                     stays.left, stays.right, stays.left2, stays.right2);
    const AxisWeights y = TapWeights(zUp, zDown, zCentre, zUp2, zDown2, tuning,
                                     stays.up, stays.down, stays.up2, stays.down2);

    // NEAR-OBJECT GATE. The step test above reads DEPTH only. An object
    // standing beside the receiver has a depth like the terrain a few yards
    // away - the same jump, the same second difference - and passes it as
    // terrain: its tap keeps steering the tangent and the pool paints a
    // shape-copy of the object beside it. At wide smoothing windows both
    // sides of the window reach the object and the arbitration is a coin
    // flip, which is why the copies grew with smoothing instead of shrinking.
    // Whatever the depth says, a standing body is elevated along world +Z
    // from the local ground plane, and a plane - however sloped - has no
    // second difference in world height for a symmetric window. Each side's
    // height h (measured on world +Z, not the camera's screen-up axis) therefore
    // forms a height second difference with its partner: a side whose second
    // difference AND whose own height both clear the window's relief scale is
    // foreign, and its weight slides to zero. The height tolerance allows
    // ordinary terrain relief without tying the test to camera orientation.
    // A dead or vetoed tap counts as the receiver's own level, so the gate
    // still fires when the foreign side is the only one that measured a surface.
    float left = x.left, right = x.right, upSide = y.left, down = y.right;
    {
        const float hL = stays.left ? Dot(pLeft - p, worldUp) : 0.f;
        const float hR = stays.right ? Dot(pRight - p, worldUp) : 0.f;
        const float hU = stays.up ? Dot(pUp - p, worldUp) : 0.f;
        const float hD = stays.down ? Dot(pDown - p, worldUp) : 0.f;
        const float live = (stays.left ? 1.f : 0.f) + (stays.right ? 1.f : 0.f) +
                           (stays.up ? 1.f : 0.f) + (stays.down ? 1.f : 0.f);
        const float spread = ((stays.left ? 1.f : 0.f) * Length(pLeft - p) +
                              (stays.right ? 1.f : 0.f) * Length(pRight - p) +
                              (stays.up ? 1.f : 0.f) * Length(pUp - p) +
                              (stays.down ? 1.f : 0.f) * Length(pDown - p)) /
                             std::max(live, 1.f);
        const float tol = 0.10f + 0.10f * spread;
        const float curvX = std::abs(hL + hR);
        const float curvY = std::abs(hU + hD);
        left *= 1.f - Smoothstep(tol, 2.f * tol, curvX) * Smoothstep(tol, 2.f * tol, std::abs(hL));
        right *= 1.f - Smoothstep(tol, 2.f * tol, curvX) * Smoothstep(tol, 2.f * tol, std::abs(hR));
        upSide *= 1.f - Smoothstep(tol, 2.f * tol, curvY) * Smoothstep(tol, 2.f * tol, std::abs(hU));
        down *= 1.f - Smoothstep(tol, 2.f * tol, curvY) * Smoothstep(tol, 2.f * tol, std::abs(hD));
    }

    const Vec3 tangentX = (p - pLeft) * left + (pRight - p) * right;
    const Vec3 tangentY = (p - pUp) * upSide + (pDown - p) * down;
    const float weightSumX = std::max(left + right, 1e-4f);
    const float weightSumY = std::max(upSide + down, 1e-4f);
    SurfaceFrame frame;
    const Vec3 cross = Cross(tangentX * (1.f / weightSumX), tangentY * (1.f / weightSumY));
    // Both axes can come back empty - a pixel of ground at the horizon whose
    // whole window reaches the sky. There is no surface in that window to
    // measure, so the honest answer is the flattest one facing the viewer
    // rather than the cross product of two remnants of a sky ray, which is
    // whatever direction the float noise happened to point.
    frame.normal = Dot(cross, cross) > 1e-12f ? Normalize(cross) : toEye;
    if (Dot(frame.normal, toEye) < 0.f) frame.normal = frame.normal * -1.f;
    frame.intact = Saturate((left + right + upSide + down) * 0.25f);
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
    const float distanceSquared = Dot(toLightVector, toLightVector);
    const float radius = std::max(light.radius, 0.01f);
    // Outside the finite support, every diffuse/specular term is multiplied by
    // zero. Reject in squared space before computing a square root, normalizing
    // vectors or evaluating the specular power.
    if (distanceSquared >= radius * radius) return {};
    const float distance = std::sqrt(std::max(distanceSquared, 0.f));
    const Vec3 toLight = distance > 1e-4f ? toLightVector * (1.f / distance) : Vec3{ 0.f, 0.f, 1.f };
    const float falloff = Saturate(1.f - distance / radius);
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
// blend across itself. Used by the direct lamp composite, which upsamples a
// low-resolution light field.
inline float CombineSlope(float left, float right, float up, float down, float centre)
{
    return std::min(std::abs(left - centre), std::abs(right - centre)) +
           std::min(std::abs(up - centre), std::abs(down - centre));
}

} // namespace locallighting
} // namespace renderer
