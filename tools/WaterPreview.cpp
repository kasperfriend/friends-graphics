// Offline reference renderer for the water surface.
//
// It runs the SAME math as the water pixel shader in WaterEffect.h on the CPU, on the
// SAME wave field (src/Water/WaveField.h) and the SAME hash (src/Water/WaterNoise.h),
// and writes a PNG - in the same two-stage order the DLL uses. One pass produces a
// pre-water scene copy with a view-depth buffer (what the renderer captures); the water
// pass then refracts, reflects, absorbs, scatters and foams against THAT buffer. The
// structure is deliberate: most of the defects this project has had were about the
// interaction with the copy (a raw sky painted as "refraction", a double gamma round
// trip, foam on the wrong side of a shoreline, a NaN depth unpack), and a previewer that
// shaded water in isolation would render all of them and still look fine.
//
// WaterEffect.h and this file are a line-by-line mirror in shader order, so a reader can
// diff them by eye. Four named differences, and the reason this cannot certify a look
// on its own:
//   * no driver filtering, mip selection or sRGB round trip - the DLL samples captured
//     bytes, here the copy is computed and encoded once at the end;
//   * no painted detail maps (the client's 512 ripple/surface textures), so the analytic
//     chop stands in for both of the shader's octaves and `surfaceTexture` is a flat tone;
//   * no vertex stage: the Gerstner lift and the slide toward the crest are evaluated per
//     pixel at the shaded point, forward-displaced rather than interpolated across a
//     tessellated grid and inverted. Crests here are therefore the same height and the same
//     pinch as the client's, but a fold this deep would only be resolved by real facets;
//   * the shader's `baseAlpha` comes from the client's own painted water (`color.a*base.a`),
//     which does not exist here, so shoreline transparency is a depth ramp instead and the
//     wet-sand fade is approximate. Everything above the alpha line is the production formula.
// The wave lattice, its wrap arithmetic, the swell trains, the slope fade, the depth-gated
// refraction, per-channel Beer-Lambert, the Henyey-Greenstein lobe, the sky fallback, the
// glint width, the foam gates and the alpha are transcribed production expressions.
//
// Build: g++ -O2 -std=c++20 -pthread tools/WaterPreview.cpp -o build/waterpreview
// Run:   build/waterpreview --out Screenshots/water-forever-look.png --grid 2 --supersample 2
//        build/waterpreview --out build/sea.png --scene sea --wind 140 --sparkle 150 --stats
//        build/waterpreview --scene coast --debug 2          (the glint term alone)
//        build/waterpreview --scene lake --probe 240 260     (every intermediate, one pixel)
//        build/waterpreview --out build/weather.png --matrix --scene sea --grid 2 --supersample 2
//        build/waterpreview --scene sea --agitation 1.45 --weather rain --weather-strength 95
// --matrix draws one body four times, a sheltered zone and an exposed one under a clear sky and in
// a downpour; --agitation/--weather/--response set the same inputs by hand. The sea state comes
// from TargetSeaState, the function the DLL calls, so a frame here is what the game would draw.
// --grid 2 draws the four bodies into one labelled sheet: 480x270 frames under a 20px settings
// bar each, 960x580 in total, the same panel geometry as the published cloud sheet.
#include "../src/Water/WaveField.h"
#include "../src/Water/WaterNoise.h"
#include "../src/Water/WaterSeaState.h"
#include "PreviewSheet.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace renderer::water;

// ------------------------------------------------------------------ math ---
struct V2 { float x = 0, y = 0; };
struct V3
{
    float x = 0, y = 0, z = 0;
    V3() = default;
    V3(float a, float b, float c) : x(a), y(b), z(c) {}
};
static V3 Add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 Mul(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static V3 Mul(V3 a, V3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
static V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 Normalize(V3 a) { const float l = std::sqrt(Dot(a, a)); return l > 0 ? Mul(a, 1.f / l) : a; }
static float Saturate(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float Mix(float a, float b, float t) { return a + (b - a) * t; }
static V3 Lerp(V3 a, V3 b, float t) { return Add(a, Mul(Sub(b, a), t)); }
static float Smoothstep(float e0, float e1, float x)
{
    const float t = Saturate((x - e0) / std::max(e1 - e0, 1e-6f));
    return t * t * (3.f - 2.f * t);
}
static float Pow(float base, float e) { return std::pow(std::max(base, 0.f), e); }
static float Step(float edge, float x) { return x >= edge ? 1.f : 0.f; }
static float ArgFloat(const char* s) { return std::strtof(s, nullptr); }

// ----------------------------------------------------------------- scene ---
// z-up world, the water surface at z = 0, everything below the waterline is seabed. One
// height field is the terrain, the bottom of the column, a thing a reflection can hit and
// the thing that occludes the sun - which is how the production shader sees all four:
// they are the same captured depth buffer.
enum SceneKind { SCENE_RIVER, SCENE_SEA, SCENE_COAST, SCENE_LAKE };

struct Config
{
    SceneKind scene = SCENE_SEA;
    std::string sceneName = "sea";   // re-appliable: the matrix presets a body, then varies it
    int width = 480, height = 270, grid = 1, supersample = 1;
    float time = 6.f;
    float camX = 0.f, camY = 0.f, camZ = 4.f, pitch = -5.f, yaw = 0.f, fov = 62.f;
    float sunElevation = 8.f, sunAzimuth = 8.f, moonAmount = 0.f;
    V3 fog{ .80f, .80f, .90f }, sun{ 1.f, .92f, .74f };
    float fogDensity = .0016f;
    // The same numbers as the [Water] keys, in the same units.
    float wind = 100.f, clarity = 100.f, sparkle = 100.f;
    float waveScale = 100.f, softening = 100.f, flowSpeed = 100.f, flowRotation = 32.f;
    float refraction = 14.f, absorption = 38.f, shoreFoam = 28.f, shoreFoamWidth = 120.f;
    float swell = 55.f, glint = 160.f, fresnel = 100.f, crestFoam = 20.f;
    float ripple = 18.f, normalResponse = 28.f, specular = 115.f;
    float roughness = 100.f, sheen = 50.f, shoalFoam = 100.f, foamBrightness = 100.f;
    // WAVE HEIGHT in percent of the shipped sea, the same number and the same scale as the
    // client's GeometryWavePercent (0 = a flat sheet, 100 = the reference look, 300 = a gale).
    float sunDisc = 40.f, geometryWaves = 100.f;
    float reflectionStrength = 80.f, environmentStrength = 45.f, reflectionDistance = 120.f;
    float reflectionThickness = 350.f;
    unsigned waterColour = 0x808080u, deepColour = 0x0e2a28u, absorptionColour = 0xff8c6bu;
    bool stats = false, reflections = true, refractionOn = true, depthOn = true, foamOn = true;
    int matrix = 0;                  // 2x2 zone-by-weather grid instead of the four bodies
    // Zone and weather, exactly the inputs WaterEffect.h feeds to TargetSeaState: the multiplier
    // a zone authors, and what the sky is doing. Applied to the numbers below rather than to the
    // shading, so a frame here shows what the game would draw with the same settings.
    float zoneAgitation = 1.f, response = 1.f, motion = 1.f;
    int weatherKind = 0;             // 0 clear, 1 rain, 2 snow, 3 sandstorm
    float weatherStrength = 0.f;     // percent
    float debugMode = 0.f;   // same channel numbering as [Water] WaterDebugMode
    int labels = 1;          // burn the settings into a bar above each panel
    int probeX = -1, probeY = -1;   // dump every intermediate for one pixel
};

// Every term below is either a negative bed level or a non-negative rise, summed - so land
// appears exactly where a rise beats the bed, and nowhere else. Taking a max of non-negative
// terms instead lifts the whole bed above the waterline and the preview renders no water at
// all, which is the first thing this file got wrong.
static float Gauss(float x, float centre, float width) { return std::exp(-std::pow((x - centre) / width, 2.f)); }

static float BottomOf(const Config& c, float x, float y)
{
    switch (c.scene)
    {
    case SCENE_RIVER:
        // A slow river between banks, with a bar on the right the flow has to bend around.
        return -2.4f - .5f * std::sin(y * .012f) - .3f * std::sin(x * .05f)
            + 7.f * Smoothstep(18.f, 44.f, std::fabs(x))
            + 3.0f * Gauss(x, 12.f, 22.f) * Gauss(y, 70.f, 26.f)
            + 9.f * Smoothstep(150.f, 240.f, y);
    case SCENE_SEA:
        // Open water with one island on the horizon, so the reflection ray and the sun
        // visibility march have something real to hit.
        return -42.f + 30.f * Gauss(x, 70.f, 45.f) * Gauss(y, 430.f, 130.f);
    case SCENE_COAST:
        // A beach that shoals away from the camera, a headland to the right, and the dunes
        // behind it: the shoreline, the wet band and the shoal foam are all in one frame.
        return -2.2f + 3.4f * Smoothstep(20.f, 120.f, y) + .6f * std::sin(x * .06f)
            + 26.f * Gauss(x, 95.f, 34.f) * Gauss(y, 150.f, 70.f)
            + 6.f * Smoothstep(120.f, 190.f, y);
    default:
        // A clear lake with a shore ahead and one boulder at the edge.
        return -6.f - 1.2f * std::sin(x * .013f + y * .009f)
            + 9.f * Smoothstep(60.f, 110.f, y)
            + 15.f * Gauss(x, 70.f, 26.f) * Gauss(y, 95.f, 40.f);
    }
}

static float FieldSlope(const Config& c, float x, float y)
{
    const float e = 1.5f;
    return std::hypot(BottomOf(c, x + e, y) - BottomOf(c, x - e, y),
                      BottomOf(c, x, y + e) - BottomOf(c, x, y - e)) / (2.f * e);
}

static V3 LandColour(const Config&, float x, float y, float z, float slope)
{
    const V3 sand{ .31f, .25f, .17f }, grass{ .13f, .22f, .09f }, rock{ .22f, .21f, .20f };
    V3 colour = Lerp(sand, grass, Smoothstep(.2f, 2.4f, z));
    colour = Lerp(colour, rock, Smoothstep(.55f, 1.3f, slope));
    return Mul(colour, .92f + .08f * std::sin(x * .7f) * std::cos(y * .61f));
}

struct View
{
    V3 pos, right, up, fwd;
    float tanHalf = 1.f, aspect = 1.f;
};
static V3 RayDir(const View& v, float u, float vv)
{
    return Normalize(Add(Add(v.fwd, Mul(v.right, u * v.tanHalf * v.aspect)), Mul(v.up, vv * v.tanHalf)));
}

struct Hit
{
    float distance = 1e9f;
    V3 point{}, normal{ 0, 0, 1 };
    bool water = false;
};

// wantWater=false is the pre-water scene copy: water is not allowed to occlude, so the copy
// carries the submerged terrain, which is what the client's own depth buffer holds at the
// moment the water shader runs (the terrain pass drew it, the liquid pass has not yet).
static Hit TraceScene(const Config& c, const View& v, V3 dir, bool wantWater)
{
    Hit hit;
    if (wantWater && dir.z < -1e-4f)
    {
        const float t = -v.pos.z / dir.z;
        if (t > .5f)
        {
            const V3 p = Add(v.pos, Mul(dir, t));
            if (BottomOf(c, p.x, p.y) < 0.f) { hit.distance = t; hit.water = true; hit.point = p; }
        }
    }
    float t = hit.water ? hit.distance + .35f : 0.f, step = 2.f;
    for (int i = 0; i < 90; ++i)
    {
        t += step;
        const V3 p = Add(v.pos, Mul(dir, t));
        if (p.z < BottomOf(c, p.x, p.y))
        {
            float lo = t - step, hi = t;
            for (int j = 0; j < 8; ++j)
            {
                const float mid = .5f * (lo + hi);
                const V3 q = Add(v.pos, Mul(dir, mid));
                if (q.z < BottomOf(c, q.x, q.y)) hi = mid; else lo = mid;
            }
            if (hi < hit.distance)
            {
                const V3 q = Add(v.pos, Mul(dir, hi));
                const float e = 1.2f;
                hit.distance = hi; hit.water = false; hit.point = q;
                hit.normal = Normalize(V3{ -(BottomOf(c, q.x + e, q.y) - BottomOf(c, q.x - e, q.y)) / (2 * e),
                                           -(BottomOf(c, q.x, q.y + e) - BottomOf(c, q.x, q.y - e)) / (2 * e), 1.f });
            }
            return hit;
        }
        step *= 1.22f;
        if (t > 9000.f) break;
    }
    return hit;
}

// The copy of the scene the water shades against, exactly as the DLL captures it.
struct SceneCopy
{
    int w = 0, h = 0;
    std::vector<V3> colour;
    std::vector<float> depth;                       // view-space z; 1e9 is sky

    float Sample(float u, float v, V3* outColour = nullptr) const
    {
        const float fx0 = Saturate(u) * (w - 1.f), fy0 = Saturate(v) * (h - 1.f);
        const int x0 = int(fx0), y0 = int(fy0), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
        const float fx = fx0 - x0, fy = fy0 - y0;
        const auto at=[&](int ix,int iy){return size_t(iy)*size_t(w)+size_t(ix);};
        const size_t a = at(x0,y0), b = at(x1,y0), cc = at(x0,y1), d = at(x1,y1);
        if (outColour)
        {
            const auto mix=[&](float sa,float sb,float sc,float sd){ return Mix(Mix(sa,sb,fx),Mix(sc,sd,fx),fy); };
            *outColour = V3{ mix(colour[a].x,colour[b].x,colour[cc].x,colour[d].x),
                             mix(colour[a].y,colour[b].y,colour[cc].y,colour[d].y),
                             mix(colour[a].z,colour[b].z,colour[cc].z,colour[d].z) };
        }
        // Nearest, not interpolated. A depth value blended across a silhouette is a hole
        // in the test that keeps a bridge or a character out of the refraction, and that
        // is precisely the arteclass the shader's rejection exists for.
        float nearest = depth[a];
        nearest = std::min(nearest, depth[b]);
        nearest = std::min(nearest, depth[cc]);
        nearest = std::min(nearest, depth[d]);
        return nearest;
    }
};

// ------------------------------------------------------------------ PNG ---
// The sheet writer and the label font are shared with CloudPreview.cpp through PreviewSheet.h,
// so both previewers emit the same format and a published sheet stays reproducible.
using preview::WritePng;

// ---------------------------------------------------------------- shading ---
struct WaveState
{
    V3 normal{ 0, 0, 1 };
    V2 slopes{};
    float detail = .5f, swell01 = .5f, foamPattern = .5f, height = 0.f, rippleEnergy = 0.f, filter = 1.f;
    // The Gerstner pair: how hard the surface is pinching here (1 - Jacobian), and the
    // Jacobian itself, which is what the shading slope is divided by so that a crest the
    // geometry steepened is a crest the light also sees steepened.
    float pinch = 0.f, jacobian = 1.f;
    V2 lateral{};   // yards, the slide toward the coming crest
};

// SampleWaves(): the shared lattice, evaluated in world axes. The DLL does the same in
// view space - the vertex stage rotates the slope into view space for the pixel shader,
// which is a rotation of these numbers, so both agree without either converting.
// footprint = the shader's fwidth(plane): how many lattice cells one screen pixel spans at
// this depth. It is the only distance-dependent term in the wave sampling, and the reason the
// preview cannot borrow BuildWaveField's basis for it - the rasterizer knows the pixel's own
// footprint, a CPU loop has to derive it from the view.
static WaveState SampleWaves(const Config& c, const WaveField& field, const GeometryWaves& geometry,
                             float worldX, float worldY, V2 footprint)
{
    WaveState w;
    const V2 plane{ worldX * field.lattice[0] + worldY * field.lattice[1],
                    worldX * field.lattice[2] + worldY * field.lattice[3] };
    // A low-frequency anchored warp makes the eight swell trains wander, and a broader
    // envelope groups them into sets. The same coordinate and chain-rule gradient are
    // evaluated in both shaders.
    const WaveWarpSample warp = WarpWavePlane(field, plane.x, plane.y);
    const GroupSample group = GroupEnvelope(field, plane.x, plane.y);
    const SwellSample trains = EvaluateSwell(warp.plane.x, warp.plane.y, field.swellPhase);
    float swell = trains.height;
    const float alongWarp = trains.slope[0] * kWaveWarpDirectionX
        + trains.slope[1] * kWaveWarpDirectionY;
    V2 swellSlope{
        (trains.slope[0] + alongWarp * kWaveWarpAmplitude * warp.gradientX) * c.swell * .01f,
        (trains.slope[1] + alongWarp * kWaveWarpAmplitude * warp.gradientY) * c.swell * .01f };
    // The envelope multiplies the sea and its own gradient tilts it: the shader's
    // product rule, spelled the same way, so a set's flank is a slope of the water and
    // not merely a dimmer highlight.
    const float gain = group.gain;
    swellSlope = V2{ swellSlope.x * gain + swell * group.gradientX,
                     swellSlope.y * gain + swell * group.gradientY };
    swell *= gain;
    // One pixel spans this many lattice cells: the shader's fwidth(plane).
    const float filter = 1.f / std::sqrt(1.f + c.softening * .16f
        * (footprint.x * footprint.x + footprint.y * footprint.y));
    // The chop rides the swell instead of scrolling over it, exactly as the shader's does.
    const NoiseSample ripple = NoiseGradient(plane.x + field.ripplePhase[0] + swell * .62f,
                                            plane.y + field.ripplePhase[1] + swell * .33f);
    // The shader's second chop octave: finer, counter-phased, footprint-faded sooner.
    const NoiseSample rippleFine = NoiseGradient(plane.x * 2.17f + field.ripplePhase[1] * 1.31f + swell * .9f,
                                                 plane.y * 2.17f - field.ripplePhase[0] * 1.31f + swell * .47f);
    const float strength = c.ripple * .001f, normalResponse = c.normalResponse * .01f;
    // The shader's waveFlow.w gate, which the DLL fills from WAVES rather than from the
    // strengths: the preview has no such switch, so it derives the same 0/1 from them.
    const float flowGate = Saturate(strength * 100.f + normalResponse * 4.f);
    w.slopes = V2{ ((ripple.dx * .72f * filter + rippleFine.dx * .30f * filter * filter) * gain + swellSlope.x) * flowGate,
                   ((ripple.dy * .72f * filter + rippleFine.dy * .30f * filter * filter) * gain + swellSlope.y) * flowGate };
    w.detail = Mix(.5f, ripple.value, filter);
    // Long foam tongues inherit the broad crest-scale breakup while chop tears
    // their edges into smaller, moving flecks. Both fields are already sampled
    // for the warped swell and normal, so the preview mirrors the shader exactly.
    w.foamPattern = Saturate(.45f * warp.noiseValue + .55f * w.detail);
    w.swell01 = swell * kSwellToUnit + .5f;
    w.rippleEnergy = std::hypot(ripple.dx, ripple.dy) * filter;
    w.filter = filter;
    w.pinch = geometry.sigma * gain * trains.pinch;
    w.jacobian = std::max(1.f - w.pinch, .3f);
    const V3 axisX{ field.lattice[0], field.lattice[1], 0.f }, axisY{ field.lattice[2], field.lattice[3], 0.f };
    // A height field's normal is the NEGATIVE gradient, divided by the Jacobian: the same
    // sign and the same sharpening the shader uses, and the one that puts the light on the
    // face of the wave that is actually leaning toward it.
    const float tilt = normalResponse / w.jacobian;
    w.normal = Normalize(V3{ -(axisX.x * w.slopes.x * tilt + axisY.x * w.slopes.y * tilt),
                             -(axisX.y * w.slopes.x * tilt + axisY.y * w.slopes.y * tilt), 1.f });
    // The vertex stage's Gerstner lift and slide, from the two trains a four yard grid can
    // carry. This preview has no vertex stage, so it displaces the shaded point directly.
    // SwellTheta is the one spelling of "k times the projected coordinate, the wrapped
    // phase and this train's own start", shared with the vertex stage and the CPU field,
    // so the lifted crest and the lit crest are the same crest by construction. The lift
    // and the slide ride the grouping envelope with the shading: a wave the set has
    // dimmed is a wave the sea has flattened, in three dimensions as well as in light.
    const float thetaA = SwellTheta(0, warp.plane.x, warp.plane.y, field.swellPhase);
    const float thetaB = SwellTheta(1, warp.plane.x, warp.plane.y, field.swellPhase);
    w.height = geometry.amplitude * gain
        * (kTrainWeight[0] * std::sin(thetaA) + kTrainWeight[1] * std::sin(thetaB));
    const V2 baseSlide{
        kTrainWeight[0] * std::cos(thetaA) * kTrainDirX[0]
            + kTrainWeight[1] * std::cos(thetaB) * kTrainDirX[1],
        kTrainWeight[0] * std::cos(thetaA) * kTrainDirY[0]
            + kTrainWeight[1] * std::cos(thetaB) * kTrainDirY[1] };
    const float slideAlongWarp=baseSlide.x*kWaveWarpDirectionX+baseSlide.y*kWaveWarpDirectionY;
    const V2 slide{ geometry.lateral * gain * (baseSlide.x+slideAlongWarp*kWaveWarpAmplitude*warp.gradientX),
                    geometry.lateral * gain * (baseSlide.y+slideAlongWarp*kWaveWarpAmplitude*warp.gradientY) };
    // Lattice axes to world axes: the lattice rows are the world directions scaled by
    // 1/cell, so one cell length turns them back into unit vectors.
    w.lateral = V2{ (slide.x * field.lattice[0] + slide.y * field.lattice[2]) * geometry.cellYards,
                    (slide.x * field.lattice[1] + slide.y * field.lattice[3]) * geometry.cellYards };
    return w;
}

static void ApplyPreset(Config& c, const char* name);

int main(int argc, char** argv)
{
    Config base;
    std::string out = "water-preview.png";
    bool sceneGiven = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };
        if (a == "--out") out = next();
        else if (a == "--grid") base.grid = std::atoi(next());
        else if (a == "--supersample") base.supersample = std::max(1, std::min(4, std::atoi(next())));
        else if (a == "--no-labels") base.labels = 0;
        else if (a == "--width") base.width = std::atoi(next());
        else if (a == "--height") base.height = std::atoi(next());
        else if (a == "--scene") { base.sceneName = next(); ApplyPreset(base, base.sceneName.c_str()); sceneGiven = true; }
        else if (a == "--time") base.time = ArgFloat(next());
        else if (a == "--cam-z") base.camZ = ArgFloat(next());
        else if (a == "--cam-y") base.camY = ArgFloat(next());
        else if (a == "--cam-x") base.camX = ArgFloat(next());
        else if (a == "--pitch") base.pitch = ArgFloat(next());
        else if (a == "--yaw") base.yaw = ArgFloat(next());
        else if (a == "--fov") base.fov = ArgFloat(next());
        else if (a == "--sun-elevation") base.sunElevation = ArgFloat(next());
        else if (a == "--sun-azimuth") base.sunAzimuth = ArgFloat(next());
        else if (a == "--moon") base.moonAmount = Saturate(ArgFloat(next()) * .01f);
        else if (a == "--fog-density") base.fogDensity = ArgFloat(next()) * .0001f;
        else if (a == "--fog") { base.fog.x = ArgFloat(next()); base.fog.y = ArgFloat(next()); base.fog.z = ArgFloat(next()); }
        else if (a == "--sun-colour") { base.sun.x = ArgFloat(next()); base.sun.y = ArgFloat(next()); base.sun.z = ArgFloat(next()); }
        else if (a == "--wind") base.wind = ArgFloat(next());
        else if (a == "--clarity") base.clarity = ArgFloat(next());
        else if (a == "--sparkle") base.sparkle = ArgFloat(next());
        else if (a == "--swell") base.swell = ArgFloat(next());
        else if (a == "--glint") base.glint = ArgFloat(next());
        else if (a == "--wave-scale") base.waveScale = ArgFloat(next());
        else if (a == "--softening") base.softening = ArgFloat(next());
        else if (a == "--geometry-waves") base.geometryWaves = ArgFloat(next());
        else if (a == "--shoal-foam") base.shoalFoam = ArgFloat(next());
        else if (a == "--shore-foam") base.shoreFoam = ArgFloat(next());
        else if (a == "--refraction") base.refraction = ArgFloat(next());
        else if (a == "--absorption") base.absorption = ArgFloat(next());
        else if (a == "--roughness") base.roughness = ArgFloat(next());
        else if (a == "--sheen") base.sheen = ArgFloat(next());
        else if (a == "--sun-disc") base.sunDisc = ArgFloat(next());
        else if (a == "--foam-brightness") base.foamBrightness = ArgFloat(next());
        else if (a == "--no-reflect") base.reflections = false;
        else if (a == "--no-refract") base.refractionOn = false;
        else if (a == "--no-depth") base.depthOn = false;
        else if (a == "--no-foam") base.foamOn = false;
        else if (a == "--debug") base.debugMode = ArgFloat(next());
        else if (a == "--agitation") base.zoneAgitation = ArgFloat(next());
        else if (a == "--response") base.response = ArgFloat(next());
        else if (a == "--weather-strength") base.weatherStrength = ArgFloat(next());
        else if (a == "--motion") base.motion = ArgFloat(next());
        else if (a == "--weather")
        {
            const std::string w = next();
            base.weatherKind = w == "clear" ? 0 : w == "rain" ? 1 : w == "snow" ? 2 : w == "sand" ? 3 : -1;
            if (base.weatherKind < 0) { std::printf("unknown weather %s\n", w.c_str()); return 2; }
            if (!base.weatherStrength) base.weatherStrength = 90.f;
        }
        else if (a == "--matrix") base.matrix = 1;
        else if (a == "--probe") { base.probeX = std::atoi(next()); base.probeY = std::atoi(next()); }
        else if (a == "--stats") base.stats = true;
        else { std::printf("unknown option %s\n", a.c_str()); return 2; }
    }
    if (!sceneGiven) ApplyPreset(base, "sea");

    struct Frame { Config config; std::string label; };
    std::vector<Frame> frames;
    if (base.matrix)
    {
        // Rows are the zone's authored sea state, columns are the sky: the two things this feature
        // is about, with the same water body and camera in every panel so nothing else can
        // explain the difference between them.
        const float agitation[2] = { .45f, 1.45f };
        const int weather[2] = { 0, 1 };
        for (int column = 0; column < 2; ++column)
            for (int row = 0; row < 2; ++row)
            {
                Frame f;
                f.config = base;
                ApplyPreset(f.config, base.sceneName.c_str());
                f.config.zoneAgitation = agitation[row];
                f.config.weatherKind = weather[column];
                f.config.weatherStrength = weather[column] ? 95.f : 0.f;
                f.label = weather[column] ? (agitation[row] < 1.f ? "SHELTERED RAIN" : "EXPOSED RAIN")
                                          : (agitation[row] < 1.f ? "SHELTERED CLEAR" : "EXPOSED CLEAR");
                frames.push_back(f);
            }
        base.grid = 2;
    }
    else if (base.grid > 1)
    {
        for (const char* name : { "river", "sea", "coast", "lake" })
        {
            Frame f;
            f.config = base;
            ApplyPreset(f.config, name);
            f.label = name;
            frames.push_back(f);
        }
    }
    else frames.push_back({ base, "frame" });

    const int cols = base.grid > 1 ? 2 : 1, rows = base.grid > 1 ? 2 : 1;
    const int tileW = frames.front().config.width, tileH = frames.front().config.height;
    // A panel is its label bar plus the rendered frame, so a 2x2 sheet of 480x270 frames is
    // 960x580 - the same panel geometry as the published cloud sheet.
    const int kBar = 20;
    const int panelH = tileH + (base.labels ? kBar : 0);
    const int sheetW = tileW * cols, sheetH = panelH * rows;
    std::vector<uint8_t> sheet(size_t(sheetW) * size_t(sheetH) * 3, 0);

    for (size_t index = 0; index < frames.size(); ++index)
    {
        // The sea state, derived by the same function the DLL calls, applied to the numbers the
        // shading reads. Nothing here re-invents the curve: change the model and both sides move.
        const Config& request = frames[index].config;
        const SeaTuning tuning{ request.zoneAgitation, request.response, request.wind * .01f };
        const SeaWeather weather{ request.weatherKind, Saturate(request.weatherStrength * .01f) };
        const SeaState sea = TargetSeaState(tuning, weather);
        Config tuned = request;
        tuned.swell *= sea.agitation;
        tuned.ripple *= sea.agitation;
        tuned.normalResponse *= std::clamp(std::sqrt(sea.agitation), 0.f, 2.5f);
        tuned.crestFoam *= sea.agitation;
        tuned.shoreFoam *= .7f + .3f * sea.agitation;
        tuned.motion *= sea.motion;
        tuned.flowRotation = std::min(tuned.flowRotation * (1.f + sea.shear), 100.f);
        const Config& c = tuned;
        const float deg = 3.14159265f / 180.f;
        View v;
        v.pos = V3{ c.camX, c.camY, c.camZ };
        v.fwd = Normalize(V3{ std::sin(c.yaw * deg) * std::cos(c.pitch * deg),
                              std::cos(c.yaw * deg) * std::cos(c.pitch * deg), std::sin(c.pitch * deg) });
        v.right = Normalize(Sub(V3{ 1, 0, 0 }, Mul(v.fwd, Dot(V3{ 1, 0, 0 }, v.fwd))));
        v.up = Normalize(Cross(v.right, v.fwd));
        v.tanHalf = std::tan(c.fov * .5f * deg);
        v.aspect = float(tileW) / float(tileH);

        const V3 light = Normalize(V3{ std::cos(c.sunAzimuth * deg) * std::cos(c.sunElevation * deg),
                                       std::sin(c.sunAzimuth * deg) * std::cos(c.sunElevation * deg),
                                       std::sin(c.sunElevation * deg) });
        const V3 lightColour = Mul(c.sun, Mix(1.f, .38f, c.moonAmount));
        const float day = Saturate(light.z * 3.f + .18f);
        auto skyAt=[&](V3 dir){
            const float up = Saturate(dir.z);
            V3 sky = Lerp(c.fog, Add(Mul(c.fog, .5f), V3{ .015f * day, .045f * day, .11f * day }), up * up);
            return Add(sky, Mul(lightColour, Pow(Saturate(Dot(dir, light)), 160.f) * (c.sunDisc * .004f)));
        };

        std::printf("%-16s %dx%d  wind %g  clarity %g  sun %g deg  agitation %.2f  motion %.2f"
            "  weather %d/%.2f\n", frames[index].label.c_str(), tileW, tileH, request.wind,
            request.clarity, request.sunElevation, sea.agitation, sea.motion, request.weatherKind,
            Saturate(request.weatherStrength * .01f));

        // One field for the whole frame: BuildWaveField is world-anchored, so the only
        // per-pixel work is evaluating it. The DLL likewise builds it once per draw.
        const WaveField field = BuildWaveField(c.time, c.flowSpeed * c.motion * .01f,
            c.flowRotation * .01f, c.waveScale, 0.f, 0.f, true, true);
        // The geometry wave constants, from the same builder the DLL calls: this preview has
        // no vertex stage, so it displaces the shaded point itself. `--geometry-waves` stands
        // in for the DLL's GEOMETRY WAVES percent, and the sea state scales it exactly as it
        // does in the client, so a storm panel lifts the same coast a storm frame does.
        const GeometryWaves geometry = BuildGeometryWaves(c.geometryWaves, c.waveScale,
            sea.agitation, c.geometryWaves > 0.f, true);
        // Rendered larger than the sheet and box filtered down, the way a 2x backbuffer is
        // resolved for display. It matters for the mirror specifically: the scene silhouette is
        // ray-marched, and the wave octave is exactly what the shader's anti-alias gain exists to
        // resolve - which one-sample-per-pixel output cannot show honestly.
        const int ss = c.supersample;
        const int renderW = tileW * ss, renderH = tileH * ss;
        SceneCopy copy;
        copy.w = renderW; copy.h = renderH;
        copy.colour.resize(size_t(renderW) * renderH);
        copy.depth.resize(size_t(renderW) * renderH);
        std::vector<V3> result(size_t(renderW) * renderH);

        // Both passes are row-parallel; the bound is the render height, not the sheet height,
        // which is easy to get wrong and shows up as a half-black frame.
        auto runRows=[&](auto&& body, int rows_){
            std::atomic<int> next{ 0 };
            std::vector<std::thread> threads;
            const unsigned workers = std::max(1u, std::thread::hardware_concurrency());
            for (unsigned t = 0; t < workers; ++t)
                threads.emplace_back([&]{ for (;;) { const int y = next.fetch_add(1); if (y >= rows_) return; body(y); } });
            for (auto& th : threads) th.join();
        };

        // ---- pass A: the pre-water scene copy (what the DLL StretchRects)
        runRows([&](int y){
            for (int x = 0; x < renderW; ++x)
            {
                const float u = (x + .5f) / renderW * 2.f - 1.f, vv = 1.f - (y + .5f) / renderH * 2.f;
                const V3 dir = RayDir(v, u, vv);
                const size_t at = size_t(y) * renderW + x;
                const Hit hit = TraceScene(c, v, dir, false);
                if (hit.distance > 8000.f) { copy.colour[at] = skyAt(dir); copy.depth[at] = 1e9f; continue; }
                const float slope = FieldSlope(c, hit.point.x, hit.point.y);
                V3 base = LandColour(c, hit.point.x, hit.point.y, hit.point.z, slope);
                const float ndl = Saturate(Dot(hit.normal, light)) * .85f + .15f;
                // A moonlit night still lights the land - the client's own baked ambient does - and
                // a preview whose scene goes to pure black under a low source hides more about the
                // water than it shows.
                const float lightAmount = std::max(day, .45f * c.moonAmount + .12f);
                V3 shaded = Mul(base, Add(Mul(c.fog, .45f), Mul(lightColour, ndl * lightAmount)));
                const float fog = 1.f - std::exp(-hit.distance * c.fogDensity);
                copy.colour[at] = Lerp(shaded, skyAt(dir), fog);
                copy.depth[at] = std::max(Dot(Sub(hit.point, v.pos), v.fwd), .05f);
            }
        }, renderH);

        if (c.probeX >= 0)
        {
            float lo = 1e9f, hi = -1e9f, depthLo = 1e9f, depthHi = -1e9f;
            int negative = 0;
            for (size_t i = 0; i < copy.colour.size(); ++i)
            {
                const V3& s = copy.colour[i];
                const float m = std::min({ s.x, s.y, s.z }), M = std::max({ s.x, s.y, s.z });
                lo = std::min(lo, m); hi = std::max(hi, M);
                negative += m < 0.f ? 1 : 0;
                depthLo = std::min(depthLo, copy.depth[i]); depthHi = std::max(depthHi, copy.depth[i]);
            }
            std::printf("  copy: colour range %g..%g, %d negative pixels, depth range %g..%g\n",
                lo, hi, negative, depthLo, depthHi);
            const size_t pat = size_t(c.probeY) * tileW + c.probeX;
            std::printf("  copy at probe: %g %g %g  depth %g\n", copy.colour[pat].x, copy.colour[pat].y,
                copy.colour[pat].z, copy.depth[pat]);
        }

        // ---- pass B: water, in shader order
        const float lightReach = .088f * (100.f / std::max(c.clarity, 5.f));
        const auto byte=[&](unsigned colour,unsigned shift,float divisor){ return float((colour>>shift)&255u)/divisor; };
        const V3 deep{ byte(c.deepColour,16,255.f), byte(c.deepColour,8,255.f), byte(c.deepColour,0,255.f) };
        const V3 tint{ byte(c.waterColour,16,128.f), byte(c.waterColour,8,128.f), byte(c.waterColour,0,128.f) };
        const V3 absorbRate{ byte(c.absorptionColour,16,128.f), byte(c.absorptionColour,8,128.f), byte(c.absorptionColour,0,128.f) };
        const float refractionStrength = c.refractionOn ? c.refraction * .01f : 0.f;
        const float absorptionStrength = c.depthOn ? c.absorption * .01f : 0.f;
        const float shoreFoamStrength = c.foamOn ? c.shoreFoam * .01f : 0.f;
        const float crestFoamStrength = c.foamOn ? c.crestFoam * .01f : 0.f;
        const float shoreFoamWidth = c.shoreFoamWidth * .01f;
        const float glintStrength = c.glint * .01f, specularStrength = c.specular * .01f;
        const float fresnelResponse = c.fresnel * .01f;
        const float normalResponse = c.normalResponse * .01f;
        const float environmentStrength = c.reflections ? c.environmentStrength * .01f : 0.f;
        const float ssrStrength = c.reflections ? c.reflectionStrength * .01f : 0.f;
        const float traceReach = c.reflectionDistance, traceThickness = c.reflectionThickness * .01f;
        const float wind = sea.wind;   // authored WIND, scaled by how much wind this water is in
        const float roughnessGain = .25f * c.roughness * .01f;
        const float sheenFloor = .02f + .06f * c.sheen * .01f;
        const float shoalGain = c.shoalFoam * .01f, foamBrightness = c.foamBrightness * .01f;
        const float sparkleGain = c.sparkle * .01f;
        // The capture's pixel size: the tile's nominal width, not the supersampled one, because
        // what is being simulated is the game's backbuffer. A rendered pixel at 2x therefore
        // bends the world by half as much in uv, which is what the shader would do at that size.
        const float refractGain = refractionStrength * 120.f / float(tileW * ss);

        std::atomic<unsigned> waterCount{ 0 };
        runRows([&](int y){
            for (int x = 0; x < renderW; ++x)
            {
                const float u = (x + .5f) / renderW * 2.f - 1.f, vv = 1.f - (y + .5f) / renderH * 2.f;
                const V3 dir = RayDir(v, u, vv);
                const size_t at = size_t(y) * renderW + x;
                const float sceneDepth = copy.depth[at];
                const Hit planeHit = TraceScene(c, v, dir, true);
                V3 out = copy.colour[at];
                // Not a water pixel means the captured scene shows through untouched, which is the
                // property the material gate is supposed to guarantee as well. Both sides of the
                // comparison are view-Z, like the depth test the client runs per liquid triangle.
                const float planeViewZ = planeHit.water ? Dot(Sub(planeHit.point, v.pos), v.fwd) : 1e9f;
                if (planeHit.water && planeViewZ < sceneDepth - .01f)
                {
                    ++waterCount;
                    V3 surface = planeHit.point;
                    // fwidth(plane) for this pixel: the wave lattice applied to the world span of
                    // one screen pixel at this depth, |ddx| + |ddy| per component like the shader's.
                    const float pixelsY = 2.f * v.tanHalf * planeViewZ / float(renderH);
                    const float pixelsX = pixelsY * v.aspect;
                    const V2 footprint{
                        std::fabs(field.lattice[0] * v.right.x + field.lattice[1] * v.right.y) * pixelsX
                            + std::fabs(field.lattice[0] * v.up.x + field.lattice[1] * v.up.y) * pixelsY,
                        std::fabs(field.lattice[2] * v.right.x + field.lattice[3] * v.right.y) * pixelsX
                            + std::fabs(field.lattice[2] * v.up.x + field.lattice[3] * v.up.y) * pixelsY };
                    WaveState wave = SampleWaves(c, field, geometry, surface.x, surface.y, footprint);
                    if (geometry.amplitude > 0.f || geometry.lateral > 0.f)
                    {
                        // Gerstner: the point the shading belongs to is the flat-plane hit
                        // pushed up by the lift AND slid toward the crest it is riding. The
                        // slide is applied forward (at the undisplaced position) rather than
                        // inverting the displacement map, which is the one place this preview
                        // knowingly differs from a real vertex stage.
                        surface = Add(surface, V3{ wave.lateral.x, wave.lateral.y, 0.f });
                        const float push = std::max((surface.z - wave.height) / -dir.z, 0.f);
                        surface = Add(v.pos, Mul(dir, planeHit.distance + push));
                        wave = SampleWaves(c, field, geometry, surface.x, surface.y, footprint);
                    }
                    const float viewZ = std::max(Dot(Sub(surface, v.pos), v.fwd), .05f);
                    const V3 viewPos = Sub(surface, v.pos);
                    const V3 n{ 0, 0, 1 }, wn = wave.normal;   // n = the geometric normal (the surface is flat)

                    // The client's own painted water, as a flat tone (no textures here).
                    V3 rgb{ .15f, .19f, .20f };
                    const float sunElev = Dot(light, n);
                    rgb = Mul(rgb, 1.f + normalResponse * (1.9f * (Dot(wn, light) - Dot(n, light))
                        + .75f * (Dot(wn, v.up) - Dot(n, v.up))));

                    const float screenU = u * .5f + .5f, screenV = .5f - vv * .5f;   // 0..1 uv, the capture's own space
                    const float behindZ = sceneDepth > 1e8f ? 1e5f : sceneDepth;
                    const float waterDepth = std::max(behindZ - viewZ, 0.f);
                    // The shader's `depthValid`: the scene behind this pixel is geometry we can
                    // absorb through, and the water is in front of it. Sky never satisfies it.
                    const float validDepth = (behindZ < 99999.f ? 1.f : 0.f) * Step(viewZ, behindZ);
                    const float openOcean = behindZ >= 99999.f ? 1.f : 0.f;
                    const float effectiveDepth = Mix(waterDepth, 9999.f, openOcean);
                    const float column = std::min(effectiveDepth, 400.f);
                    const float bend = refractionStrength * Saturate(column * .05f + .25f);
                    const V2 uvOffset{ wave.slopes.x * refractGain * bend, -wave.slopes.y * refractGain * bend };
                    float refractU = screenU + uvOffset.x, refractV = screenV + uvOffset.y;
                    const float refractZ = copy.Sample(refractU, refractV);   // raw, no bias
                    if (!(refractZ > viewZ - .5f)) { refractU = screenU; refractV = screenV; }
                    // (a refraction that would land in front of the surface is pulled back to the
                    // unoffset pixel, which is the shader's silhouette rejection in view-Z terms)
                    V3 refracted;
                    copy.Sample(refractU, refractV, &refracted);

                    const V3 transmittance{ std::exp(-lightReach * absorptionStrength * column * absorbRate.x),
                                            std::exp(-lightReach * absorptionStrength * column * absorbRate.y),
                                            std::exp(-lightReach * absorptionStrength * column * absorbRate.z) };
                    const V3 toCamera = Normalize(Mul(viewPos, -1.f));
                    const float absorption = 1.f - transmittance.y;
                    const float g = .3f + .34f * Saturate(sunElev);
                    const float phaseDen = std::max(1.f + g * g - 2.f * g * Dot(Mul(toCamera, -1.f), light), 1e-3f);
                    // Same lobe, same normalization, same folded gain as the shader's.
                    V3 inScatter = Mul(Add(Mul(deep, 1.7f), Mul(tint, .12f)),
                        absorption * (1.f - g * g) * .17f / (phaseDen * std::sqrt(phaseDen))
                        * Saturate(sunElev * 2.5f + .2f));
                    // Looking into the sun at a crest that is standing up: the sheet between the
                    // camera and the light goes translucent, which is the one place water reads as
                    // a body of water rather than a mirror. `toCamera` is the shader's own `v`
                    // (both are normalize(-viewPos)), so the term carries over sign and all.
                    const float backlit = Saturate(-Dot(toCamera, light) * 1.7f - .45f)
                        * Saturate(wave.swell01 * 1.7f - .45f);
                    inScatter = Mul(inScatter, 1.f + 2.2f * backlit);
                    inScatter = Mul(inScatter, lightColour);

                    const float shallowEdge = 1.f - Smoothstep(.08f, .08f + std::max(shoreFoamWidth, .1f), effectiveDepth);
                    const float shore = validDepth * shallowEdge;
                    const float crest = Saturate(.42f + wave.swell01 * .6f + wave.detail * .3f);
                    // ---- foam, computed before the reflections because the shader lets it damp
                    // them: a breaking crest scatters the sky, it does not mirror it. The three
                    // sources are the ones a real sea shows - the crest itself, the pinch where
                    // the geometry waves are folding water onto water, and the shoal that runs up
                    // a beach. All three are gated by the authored WIND and by how rough this
                    // water is, so a calm lake stays a calm lake and a squall breaks.
                    const float foamWind = Saturate(wind);
                    const float breaking = Saturate(wave.pinch * 2.4f);
                    const float capBand = wave.swell01 * 1.06f + breaking * .30f + (wave.detail - .5f) * .26f;
                    // The noise widens a lively patch and pinches a weak one, so
                    // foam becomes broken streaks instead of an identical stripe
                    // on every crest. Broad warp noise supplies the tongue-scale
                    // shape; drifting chop detail shreds the edges.
                    const float foamThreshold = .82f + (1.f - wave.foamPattern) * .14f;
                    const float whitecap = Smoothstep(foamThreshold, foamThreshold + .14f, capBand);
                    const float bottomSlope = FieldSlope(c, surface.x, surface.y);
                    const float shoal = Saturate(bottomSlope * shoalGain * 2.f - .3f)
                        * Saturate(1.f - column * .05f) * validDepth;
                    const float crestMask = Smoothstep(.60f, .94f, wave.swell01 + wave.rippleEnergy * .05f)
                        * (.35f + .65f * wave.foamPattern);
                    const float foamAmount = Saturate((shore * shoreFoamStrength * (.35f + .65f * foamWind)
                        + (crestMask * .45f + whitecap * 1.15f + shoal * .6f) * foamWind * crestFoamStrength) * crest);
                    const float rough = Saturate(sheenFloor
                        + std::hypot(wave.slopes.x, wave.slopes.y) * roughnessGain + foamAmount * 1.4f);

                    const V3 toCam = Normalize(Mul(viewPos, -1.f));
                    V3 reflectDir = Normalize(Sub(toCam, Mul(wn, 2.f * Dot(toCam, wn))));
                    const float rayUp = Saturate(reflectDir.z);
                    V3 environment = Lerp(c.fog, Add(Mul(c.fog, .5f), V3{ .015f * day, .045f * day, .11f * day }), rayUp * rayUp);
                    environment = Add(environment, Mul(lightColour, Pow(Saturate(Dot(reflectDir, light)), 160.f) * (c.sunDisc * .004f)));
                    V3 reflected = environment;
                    float hit = 0.f;
                    if (ssrStrength > .001f)
                    {
                        V3 hitColour = environment;
                        for (int step = 1; step <= 8; ++step)
                        {
                            const float q = step / 8.f;
                            const float traceDistance = .45f + q * q * traceReach;
                            const V3 sample = Add(Add(surface, Mul(wn, .15f)), Mul(reflectDir, traceDistance));
                            const V3 rel = Sub(sample, v.pos);
                            const float sampleZ = std::max(Dot(rel, v.fwd), .05f);
                            const float su = .5f + .5f * Dot(rel, v.right) / sampleZ * v.aspect;
                            const float sv = .5f - .5f * Dot(rel, v.up) / sampleZ;
                            const float inside = Step(.05f, sampleZ) * Saturate(std::min(std::min(su, sv), std::min(1.f - su, 1.f - sv)) * 8.f);
                            const float sceneZ = copy.Sample(su, sv);
                            const float difference = sampleZ - sceneZ;
                            const float thickness = traceThickness + traceDistance * .05f;
                            const float confidence = inside * Step(-thickness, difference) * (difference <= thickness * .5f ? 1.f : 0.f);
                            V3 sampleColour;
                            copy.Sample(su, sv, &sampleColour);
                            hitColour = Lerp(hitColour, sampleColour, confidence * (1.f - hit));
                            hit = std::max(hit, confidence);
                        }
                        reflected = Lerp(environment, hitColour, Saturate(hit * 1.8f));
                    }
                    reflected = Lerp(reflected, environment, Saturate(rough * 1.6f));

                    const V3 H = Normalize(Add(toCam, light));
                    const float NoV = std::max(Dot(wn, toCam), 1e-4f);
                    const float foreverFresnel = Saturate(1.f - NoV);
                    const float fresnel = (.02f + .98f * Pow(foreverFresnel, 5.f)) * fresnelResponse;
                    const float NoH = Saturate(Dot(wn, H));
                    const float NoL = Dot(wn, light);
                    const float sharpness = Mix(26.f, 230.f, Saturate(sunElev * 1.8f)) / (1.f + rough * 7.f) * sparkleGain;
                    float celestialPath = Pow(NoH, sharpness) * (sharpness + 2.f) * fresnel * Saturate(NoL)
                        * (.12f + .88f * Saturate(wave.detail * .75f + wave.swell01 * .25f)) * glintStrength;
                    float lightVisibility = 1.f;
                    if (ssrStrength > .001f && celestialPath > .00005f)
                    {
                        for (int step = 1; step <= 8; ++step)
                        {
                            const float q = step / 8.f;
                            const float traceDistance = 1.5f + q * q * 110.f;
                            const V3 sample = Add(Add(surface, Mul(wn, .18f)), Mul(light, traceDistance));
                            const V3 rel = Sub(sample, v.pos);
                            const float sampleZ = std::max(Dot(rel, v.fwd), .05f);
                            const float su = .5f + .5f * Dot(rel, v.right) / sampleZ * v.aspect;
                            const float sv = .5f - .5f * Dot(rel, v.up) / sampleZ;
                            const float inside = Step(.05f, sampleZ) * Saturate(std::min(std::min(su, sv), std::min(1.f - su, 1.f - sv)) * 8.f);
                            const float blockerZ = copy.Sample(su, sv);
                            const float blocked = inside * Step(blockerZ + 1.5f, sampleZ);
                            lightVisibility *= 1.f - blocked;
                        }
                        celestialPath *= lightVisibility;
                    }

                    V3 probeBottom{0,0,0}, probeRefracted{0,0,0}, probeInScatter{0,0,0};
                    float probeSunElev = 0, probeG = 0, probePhaseDen = 0;
                    const V3 rgbBefore = rgb;
                    V3 waterBody = rgb;
                    probeSunElev = sunElev; probeG = g; probePhaseDen = phaseDen;
                    probeRefracted = refracted; probeInScatter = inScatter;
                    if (true)               // a capture exists whenever this preview has a copy
                    {
                        V3 bottom = Add(Add(Mul(refracted, transmittance), inScatter), Mul(deep, absorption));
                    probeBottom = bottom;
                        V3 body = Lerp(rgb, bottom, validDepth * Saturate(refractionStrength));
                        waterBody = Lerp(body, Add(deep, inScatter), Saturate(absorption * (.85f - .22f * fresnel)));
                    }
                    // Same neutral-at-0x808080 contract as the shader: tint is uploaded as
                    // bytes/128, so 1.0 is neutral and no gain belongs here.
                    waterBody = Mul(waterBody, tint);
                    rgb = waterBody;
                    // Foam damps the mirror: a crest that is breaking is full of air, and air
                    // scatters instead of reflecting. The shader applies both factors here.
                    const float reflectionAmount = Saturate(fresnel * (1.f - .5f * rough)
                        * (environmentStrength * .5f + ssrStrength * hit * 1.5f))
                        * (1.f - Saturate(foamAmount * .85f));
                    rgb = Lerp(rgb, reflected, reflectionAmount);
                    const float pathEnergy = Saturate(celestialPath * specularStrength * .0992f)
                        * (1.f - Saturate(foamAmount));
                    rgb = Add(rgb, Mul(lightColour, pathEnergy * (.35f + .65f * fresnel)));
                    // The foam colour itself, added last so it sits on top of everything the
                    // water did below it. The lift toward the light is softened at NoL = -.25
                    // so a shaded face still carries its own cap instead of going flat black.
                    rgb = Add(rgb, Mul(Add(Mul(deep, .3f), V3{ .42f, .48f, .43f }),
                        foamAmount * (.55f + .45f * Saturate(NoL + .25f)) * foamBrightness));
                    const float columnBottom = std::max(-BottomOf(c, surface.x, surface.y), 0.f);   // the client's own alpha stand-in
                    const float baseAlpha = Saturate(.30f + columnBottom * .30f);
                    const float depthAlpha = absorption * (.78f - .20f * fresnel);
                    const float alpha = Saturate(baseAlpha + (1.f - baseAlpha) * depthAlpha);
                    const float fog = 1.f - std::exp(-viewZ * c.fogDensity);
                    // The shader fogs toward the flat scene fog colour and lets the device blend
                    // it over the copy with the water's own alpha; both happen here in that order.
                    out = Lerp(copy.colour[at], Lerp(rgb, c.fog, fog), alpha);
                    if (c.probeX == x && c.probeY == y)
                    {
                        const auto bad=[](float f){ return !(f >= 0.f && f <= 1e9f); };   // true for NaN too
                        std::printf("  probe %d,%d  viewZ %g  column %g  behindZ %g  open %g\n",
                            x, y, viewZ, column, behindZ, openOcean);
                        std::printf("    refract %g,%g  refractZ %g  valid %g  transmittance %g %g %g\n",
                            refractU, refractV, refractZ, validDepth, transmittance.x, transmittance.y, transmittance.z);
                        std::printf("    absorption %g  fresnel %g  rough %g  hit %g  alpha %g  foam %g\n",
                            absorption, fresnel, rough, hit, alpha, foamAmount);
                        std::printf("    rgb %g %g %g  body %g %g %g  n %g %g %g  slopes %g %g\n",
                            rgbBefore.x, rgbBefore.y, rgbBefore.z, waterBody.x, waterBody.y, waterBody.z,
                            wn.x, wn.y, wn.z, wave.slopes.x, wave.slopes.y);
                        std::printf("    path %g  glint visible %g  bad(out) %d %d %d\n",
                            pathEnergy, lightVisibility, bad(out.x), bad(out.y), bad(out.z));
                        std::printf("    refracted %g %g %g  inScatter %g %g %g  bottom %g %g %g\n",
                            probeRefracted.x, probeRefracted.y, probeRefracted.z,
                            probeInScatter.x, probeInScatter.y, probeInScatter.z,
                            probeBottom.x, probeBottom.y, probeBottom.z);
                        std::printf("    sunElev %g g %g phaseDen %g  tint %g %g %g  deep %g %g %g  absorbRate %g %g %g\n",
                            probeSunElev, probeG, probePhaseDen, tint.x, tint.y, tint.z,
                            deep.x, deep.y, deep.z, absorbRate.x, absorbRate.y, absorbRate.z);
                    }
                    // The shader's own debug channels, same numbering, so a frame here can be
                    // compared against a client log with [Water] WaterDebugMode at the same value.
                    if (c.debugMode > 1.5f) out = V3{ pathEnergy * 3.f, pathEnergy * 3.f, pathEnergy * 3.f };
                    else if (c.debugMode > .5f) out = V3{ 1.f, 0.f, 1.f };
                }
                result[at] = out;
            }
        }, renderH);
        // Box filter, encode once at the end: a linear resolve, which is what a 2x target does
        // and not what an average of already-encoded bytes would be.
        const auto enc=[](float value){ value = Saturate(value); return std::pow(value, 1.f / 2.2f); };
        const size_t tileX = index % size_t(rows), tileY = index / size_t(rows);
        for (int y = 0; y < tileH; ++y)
        {
            for (int x = 0; x < tileW; ++x)
            {
                V3 sum{ 0, 0, 0 };
                for (int j = 0; j < ss; ++j)
                    for (int i = 0; i < ss; ++i)
                        sum = Add(sum, result[size_t(y * ss + j) * size_t(renderW) + size_t(x * ss + i)]);
                const V3 average = Mul(sum, 1.f / float(ss * ss));
                const size_t panelY = base.labels ? tileY * size_t(panelH) + size_t(kBar) : tileY * size_t(tileH);
                uint8_t* dst = &sheet[((panelY + size_t(y)) * size_t(sheetW)
                    + tileX * size_t(tileW) + size_t(x)) * 3];
                dst[0] = uint8_t(enc(average.x) * 255.f + .5f);
                dst[1] = uint8_t(enc(average.y) * 255.f + .5f);
                dst[2] = uint8_t(enc(average.z) * 255.f + .5f);
            }
        }
        if (base.labels)
        {
            char label[160];
            // The zone's authored agitation and the sky are printed as the two inputs, not as the
            // result, so a reader can tell a tuned zone from a storm without re-deriving either.
            std::snprintf(label, sizeof(label), "%s - ZONE %.2f  WIND %d  SUN %d DEG  %s %.0f%%  T%.1f",
                frames[index].label.c_str(), request.zoneAgitation, int(request.wind),
                int(request.sunElevation), request.weatherKind == 0 ? "CLEAR"
                : request.weatherKind == 1 ? "RAIN" : request.weatherKind == 2 ? "SNOW" : "SAND",
                Saturate(request.weatherStrength * .01f) * 100.f, request.time);
            const int barY = base.labels ? int(tileY) * panelH : int(tileY) * tileH;
            preview::LabelBar(sheet, sheetW, sheetH, int(tileX) * tileW, barY, tileW, kBar, label);
        }
        if (c.stats)
            std::printf("  water pixels %u of %d\n", waterCount.load(), renderW * renderH);
    }

    if (!WritePng(out.c_str(), sheetW, sheetH, sheet))
    {
        std::printf("failed to write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s %dx%d\n", out.c_str(), sheetW, sheetH);
    return 0;
}

static void ApplyPreset(Config& c, const char* name)
{
    const std::string n = name;
    if (n == "river")
    {
        c.scene = SCENE_RIVER; c.wind = 15.f; c.clarity = 130.f; c.sparkle = 90.f;
        c.sunElevation = 12.f; c.sunAzimuth = 96.f; c.fog = V3{ .92f, .78f, .72f }; c.sun = V3{ 1.f, .86f, .66f };
        c.camZ = 2.4f; c.camY = -46.f; c.pitch = -5.f; c.waveScale = 110.f;
        // WAVE HEIGHT, in percent of the shipped sea: a river is sheltered, so it carries
        // chop rather than swell, and its foam stays a thin line at the shore.
        c.crestFoam = 6.f; c.shoreFoam = 8.f; c.geometryWaves = 35.f; c.swell = 40.f;
    }
    else if (n == "coast")
    {
        c.scene = SCENE_COAST; c.wind = 140.f; c.clarity = 95.f; c.sparkle = 150.f;
        c.sunElevation = 5.f; c.sunAzimuth = 78.f; c.fog = V3{ .98f, .86f, .68f }; c.sun = V3{ 1.f, .93f, .72f };
        c.camZ = 4.f; c.camY = -30.f; c.yaw = -22.f; c.pitch = -7.f; c.swell = 70.f;
        c.geometryWaves = 130.f; c.shoalFoam = 160.f; c.crestFoam = 34.f;
    }
    else if (n == "lake")
    {
        c.scene = SCENE_LAKE; c.wind = 35.f; c.clarity = 175.f; c.sparkle = 70.f;
        // moonAmount is a 0..1 fraction here, not a percent: the --moon switch takes a percent
        // and converts, and a 100 in this field multiplies the scene light by -61.
        c.sunElevation = 18.f; c.sunAzimuth = 95.f; c.moonAmount = 1.f; c.sunDisc = 85.f;
        c.fog = V3{ .10f, .12f, .19f }; c.fogDensity = .0009f; c.fog = V3{ .34f, .38f, .52f }; c.sun = V3{ .58f, .66f, .86f };
        c.camZ = 3.f; c.camY = -40.f; c.yaw = 14.f; c.pitch = -6.f; c.swell = 40.f;
        c.geometryWaves = 45.f; c.deepColour = 0x0a2230u; c.sparkle = 120.f;
    }
    else
    {
        c.scene = SCENE_SEA; c.wind = 120.f; c.clarity = 100.f; c.sparkle = 140.f;
        c.sunElevation = 7.f; c.sunAzimuth = 90.f; c.fog = V3{ .80f, .80f, .90f }; c.sun = V3{ 1.f, .92f, .74f };
        c.camZ = 5.f; c.camY = 0.f; c.pitch = -4.f; c.swell = 65.f; c.crestFoam = 30.f;
        c.geometryWaves = 100.f;   // the shipped WAVE HEIGHT: this panel is the reference look
    }
}
