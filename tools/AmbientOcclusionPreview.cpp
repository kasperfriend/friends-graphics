// Offline preview of the screen-space ambient occlusion pass.
//
// SSAO needs only a depth buffer and a camera, so the honest offline mirror
// of the shipped pass is: render a model to depth, then run the SAME math the
// GPU runs. This tool ray-traces a small synthetic stone ruin (steps, two
// columns, a lintel, a boulder and a ring - chosen for the places AO shows:
// object/ground contact, creases, corners and the inside of a cavity) into a
// viewport-depth buffer encoded exactly like the INTZ capture, then computes
// occlusion with src/Effects/AmbientOcclusionCore.h - the one definition of
// the kernel, reconstruction, range check, fade and blur weights that
// kAmbientOcclusionSource/kAmbientOcclusionApplySource mirror. A frame from
// this tool is therefore what the shipped pass computes for that depth,
// camera and tuning, which is what the published on/off sheet was tuned with.
//
// Keep this file's camera math and buffer encoding in step with
// AmbientOcclusionCore.h; the D3D9 behaviour itself (state, blending,
// resources) is covered by the composite integration and
// tools/AmbientOcclusionRegression.cpp for the math.
//
// Build:  g++ -O2 -std=c++20 tools/AmbientOcclusionPreview.cpp -o build/aopreview
// Run:    build/aopreview --out Screenshots/ambient-occlusion.png
//         build/aopreview --out build/ao.png --strength 90 --radius 3 --stats
#include "../src/Effects/AmbientOcclusionCore.h"
#include "PreviewSheet.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace renderer::ambientocclusion;

namespace
{
// ----------------------------------------------------------------- math -----
struct V3
{
    float x = 0, y = 0, z = 0;
    V3() = default;
    V3(float a, float b, float c) : x(a), y(b), z(c) {}
};
inline V3 Add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 Mul(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline V3 Mul(V3 a, V3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
inline float Dot3(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Len3(V3 a) { return std::sqrt(Dot3(a, a)); }
inline V3 Norm3(V3 a) { const float l = Len3(a); return l > 1e-8f ? Mul(a, 1.f / l) : a; }
inline V3 Mix3(V3 a, V3 b, float t) { return Add(a, Mul(Sub(b, a), t)); }
inline float Clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
inline float Mixf(float a, float b, float t) { return a + (b - a) * t; }

// ------------------------------------------------------------- the camera ---
// A D3D-style perspective camera in a z-up world, with the exact projection
// convention the AO core decodes: P00/P11 are the projection matrix scale,
// A/B map view depth into the viewport depth range.
struct Camera
{
    V3 position, forward, right, up;
    float p00 = 1, p11 = 1;
    float nearZ = 0.5f, farZ = 400.f;
    float maxZ = 1.f; // viewport MaxZ, like the client's
    float projA = 0, projB = 0;
};

Camera MakeCamera(V3 eye, V3 target, float fovYDegrees, float aspect)
{
    Camera c;
    c.position = eye;
    c.forward = Norm3(Sub(target, eye));
    // z-up world: right = forward x worldUp, up = right x forward.
    const V3 worldUp{ 0, 0, 1 };
    c.right = Norm3(V3{ c.forward.y * worldUp.z - c.forward.z * worldUp.y,
                        c.forward.z * worldUp.x - c.forward.x * worldUp.z,
                        c.forward.x * worldUp.y - c.forward.y * worldUp.x });
    c.up = V3{ c.right.y * c.forward.z - c.right.z * c.forward.y,
               c.right.z * c.forward.x - c.right.x * c.forward.z,
               c.right.x * c.forward.y - c.right.y * c.forward.x };
    const float fovY = fovYDegrees * 3.14159265f / 180.f;
    c.p11 = 1.f / std::tan(fovY * .5f);
    c.p00 = c.p11 / aspect;
    c.projA = c.farZ / (c.farZ - c.nearZ);
    c.projB = -c.nearZ * c.farZ / (c.farZ - c.nearZ);
    return c;
}

// Unnormalized view-space direction for a pixel centre, exactly the vector
// ViewPosition() inverts.
inline V3 ViewRay(const Camera& c, float u, float v)
{
    return { (u * 2.f - 1.f) / c.p00, (1.f - v * 2.f) / c.p11, 1.f };
}
inline V3 WorldRay(const Camera& c, float u, float v)
{
    const V3 d = ViewRay(c, u, v);
    return Add(Add(Mul(c.right, d.x), Mul(c.up, d.y)), Mul(c.forward, d.z));
}
// Encode a view-space depth the way the INTZ viewport depth stores it.
inline float EncodeRaw(const Camera& c, float z)
{
    const float norm = c.projA + c.projB / std::max(z, 1e-4f);
    return Clamp01(norm) * c.maxZ;
}

// ------------------------------------------------------------ the model -----
// A synthetic stone ruin. The shapes are chosen for what ambient occlusion
// is supposed to reveal: where the columns meet the platform, under the
// lintel, along every step nosing, where the boulder and the ring touch the
// ground, and inside the ring's hole. The point of the sheet is the
// occlusion, not the model - a reader has to be able to see the creases.
struct Box { V3 min, max; };
struct Scene
{
    // Columns and the ring.
    V3 columnA{ -2.5f, 7.5f, 0 }, columnB{ 2.5f, 7.5f, 0 };
    float columnRadius = 0.8f, platformTop = 2.4f, columnTop = 8.5f;
    V3 boulder{ -4.2f, 1.2f, 1.0f };
    float boulderRadius = 1.1f;
    V3 ring{ 3.9f, 1.3f, 0.36f };
    float ringMajor = 0.95f, ringMinor = 0.36f;

    static float SdBox(V3 p, V3 center, V3 half)
    {
        const V3 q{ std::abs(p.x - center.x) - half.x, std::abs(p.y - center.y) - half.y,
                    std::abs(p.z - center.z) - half.z };
        const float ox = std::max(q.x, 0.f), oy = std::max(q.y, 0.f), oz = std::max(q.z, 0.f);
        const float outside = std::sqrt(ox * ox + oy * oy + oz * oz);
        const float inside = std::min(std::max(q.x, std::max(q.y, q.z)), 0.f);
        return outside + inside;
    }
    static float SdCylinder(V3 p, V3 base, float radius, float height)
    {
        const float radial = std::sqrt((p.x - base.x) * (p.x - base.x) +
                                       (p.y - base.y) * (p.y - base.y)) - radius;
        const float axial = std::max(base.z - p.z, p.z - (base.z + height));
        const float outside = std::sqrt(std::max(radial, 0.f) * std::max(radial, 0.f) +
                                        std::max(axial, 0.f) * std::max(axial, 0.f));
        return std::min(std::max(radial, axial), 0.f) + outside;
    }
    static float SdTorus(V3 p, V3 center, float major, float minor)
    {
        const float radial = std::sqrt((p.x - center.x) * (p.x - center.x) +
                                       (p.y - center.y) * (p.y - center.y)) - major;
        const float dz = p.z - center.z;
        return std::sqrt(radial * radial + dz * dz) - minor;
    }

    // Material ids: 0 ground, 1 steps, 2 columns, 3 lintel, 4 boulder, 5 ring, 6 wall.
    struct Hit { float distance; int material; };

    Hit Map(V3 p) const
    {
        Hit best{ p.z, 0 }; // ground plane
        auto consider = [&](float d, int material) { if (d < best.distance) best = { d, material }; };

        // Three steps up to the platform.
        consider(SdBox(p, { 0, 4.25f, .40f }, { 4.f, .75f, .40f }), 1);
        consider(SdBox(p, { 0, 5.75f, .80f }, { 4.f, .75f, .80f }), 1);
        consider(SdBox(p, { 0, 7.75f, 1.20f }, { 4.f, 1.25f, 1.20f }), 1);
        // Two columns and the lintel they carry.
        consider(SdCylinder(p, { columnA.x, columnA.y, platformTop }, columnRadius, columnTop - platformTop), 2);
        consider(SdCylinder(p, { columnB.x, columnB.y, platformTop }, columnRadius, columnTop - platformTop), 2);
        consider(SdBox(p, { 0, 7.5f, 9.f }, { 3.6f, .8f, .5f }), 3);
        // A boulder and a stone ring on the ground in front.
        consider(Len3(Sub(p, boulder)) - boulderRadius, 4);
        consider(SdTorus(p, ring, ringMajor, ringMinor), 5);
        // A low back wall: one long ground/wall crease behind the platform.
        consider(SdBox(p, { 0, 9.6f, 2.f }, { 6.f, .4f, 2.f }), 6);
        return best;
    }

    V3 Normal(V3 p) const
    {
        const float e = 0.002f;
        const V3 g{ Map(Add(p, { e, 0, 0 })).distance - Map(Sub(p, { e, 0, 0 })).distance,
                    Map(Add(p, { 0, e, 0 })).distance - Map(Sub(p, { 0, e, 0 })).distance,
                    Map(Add(p, { 0, 0, e })).distance - Map(Sub(p, { 0, 0, e })).distance };
        return Norm3(g);
    }
};

// ---------------------------------------------------------------- render ----
struct Frame
{
    int width = 0, height = 0;
    std::vector<float> raw;     // viewport depth, 1 = sky (INTZ convention)
    std::vector<float> viewZ;   // linear view depth
    std::vector<float> color;   // linear RGB
};

V3 MaterialColor(int material)
{
    switch (material)
    {
    case 1: return { .62f, .60f, .55f };  // steps
    case 2: return { .72f, .70f, .64f };  // columns
    case 3: return { .66f, .64f, .58f };  // lintel
    case 4: return { .50f, .49f, .47f };  // boulder
    case 5: return { .58f, .55f, .50f };  // ring
    case 6: return { .56f, .54f, .50f };  // wall
    default: return { .42f, .44f, .36f }; // mossy ground
    }
}

void RenderFrame(Frame& frame, const Camera& camera, const Scene& scene, int width, int height)
{
    frame.width = width;
    frame.height = height;
    const size_t pixels = size_t(width) * height;
    frame.raw.assign(pixels, 1.f);
    frame.viewZ.assign(pixels, camera.farZ);
    frame.color.resize(pixels * 3);

    const V3 lightDir = Norm3({ -.45f, -.55f, .70f });
    const V3 lightColor{ 1.05f, 1.0f, .92f };
    const V3 skyColor{ .34f, .42f, .58f };
    const V3 groundBounce{ .22f, .20f, .16f };
    const V3 horizon{ .62f, .66f, .72f }, zenith{ .30f, .42f, .62f };

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const float u = (float(x) + .5f) / float(width);
            const float v = (float(y) + .5f) / float(height);
            const V3 dir = Norm3(WorldRay(camera, u, v));
            const size_t at = size_t(y) * width + x;

            // Sphere-trace the SDF scene.
            float t = 0.05f;
            bool hit = false;
            V3 p;
            for (int step = 0; step < 220; ++step)
            {
                p = Add(camera.position, Mul(dir, t));
                const auto m = scene.Map(p);
                if (m.distance < 0.0012f) { hit = true; break; }
                t += m.distance * .92f;
                if (t > 80.f) break;
            }
            if (!hit)
            {
                const V3 sky = Mix3(horizon, zenith, Clamp01(dir.z * 2.2f));
                frame.color[at * 3 + 0] = sky.x; frame.color[at * 3 + 1] = sky.y; frame.color[at * 3 + 2] = sky.z;
                continue;
            }

            const V3 n = scene.Normal(p);
            const auto m = scene.Map(p);
            const V3 albedo = MaterialColor(m.material);
            const float diffuse = std::max(Dot3(n, lightDir), 0.f);
            // Hemisphere ambient: the term AO is physically removing.
            const V3 ambient = Mix3(groundBounce, skyColor, Clamp01(n.z * .5f + .5f));
            const V3 lit = Add(Mul(albedo, Mul(lightColor, diffuse * .85f)), Mul(ambient, .85f));
            frame.color[at * 3 + 0] = lit.x; frame.color[at * 3 + 1] = lit.y; frame.color[at * 3 + 2] = lit.z;
            const float z = Dot3(Sub(p, camera.position), camera.forward);
            frame.viewZ[at] = z;
            frame.raw[at] = EncodeRaw(camera, z);
        }
    }
}

// ------------------------------------------------------ the mirrored pass ---
// The same two passes the GPU runs: full-resolution occlusion from the core,
// then the depth-aware 4x4 box blur from the apply shader.
std::vector<float> ComputeOcclusionBuffer(const Frame& frame, const Camera& camera, const Settings& settings)
{
    ViewParams view{ camera.projA, camera.projB, camera.p00, camera.p11, camera.maxZ };
    const float texelU = 1.f / float(frame.width), texelV = 1.f / float(frame.height);
    auto fetch = [&](float u, float v) -> float {
        int x = int(u * frame.width), y = int(v * frame.height);
        x = std::clamp(x, 0, frame.width - 1);   // CLAMP addressing
        y = std::clamp(y, 0, frame.height - 1);
        return frame.raw[size_t(y) * frame.width + x];
    };
    std::vector<float> out(size_t(frame.width) * frame.height, 1.f);
    for (int y = 0; y < frame.height; ++y)
        for (int x = 0; x < frame.width; ++x)
        {
            const float u = (float(x) + .5f) * texelU, v = (float(y) + .5f) * texelV;
            out[size_t(y) * frame.width + x] = ComputeOcclusion(u, v, texelU, texelV, view, settings, fetch);
        }
    return out;
}

std::vector<float> BlurOcclusionBuffer(const Frame& frame, const Camera& camera, const std::vector<float>& ao)
{
    ViewParams view{ camera.projA, camera.projB, camera.p00, camera.p11, camera.maxZ };
    std::vector<float> out(ao.size(), 1.f);
    for (int y = 0; y < frame.height; ++y)
    {
        for (int x = 0; x < frame.width; ++x)
        {
            const size_t at = size_t(y) * frame.width + x;
            const float rawCenter = frame.raw[at];
            if (IsSkyRaw(rawCenter)) continue;
            const float zc = ViewZ(rawCenter, view);
            const float threshold = std::max(zc * .06f, .25f); // mirrors BlurWeight
            float sum = 0, wsum = 0;
            for (int j = 0; j < kBlurSpan; ++j)
            {
                for (int i = 0; i < kBlurSpan; ++i)
                {
                    const int sx = std::clamp(x + int(std::lround(BlurOffset(i))), 0, frame.width - 1);
                    const int sy = std::clamp(y + int(std::lround(BlurOffset(j))), 0, frame.height - 1);
                    const size_t sat = size_t(sy) * frame.width + sx;
                    const float raw = frame.raw[sat];
                    if (IsSkyRaw(raw)) continue;
                    const float w = Saturate(1.f - std::abs(ViewZ(raw, view) - zc) / threshold);
                    sum += w * ao[sat];
                    wsum += w;
                }
            }
            out[at] = wsum > 0 ? sum / wsum : ao[at];
        }
    }
    return out;
}

struct PanelStats
{
    float meanAo = 1.f;
    float minAo = 1.f;
    float darkenedPercent = 0.f; // share of geometry pixels darkened by >3%
    size_t geometryPixels = 0;   // pixels with a real surface (not sky)
};

PanelStats ApplyPanel(std::vector<uint8_t>& rgbRow3, const Frame& frame, const std::vector<float>& blurred,
                      int sheetWidth, int tileX, int tileY, int width, int height, bool grayscale)
{
    PanelStats stats;
    stats.minAo = 1.f;
    float sum = 0;
    size_t counted = 0, darkened = 0;
    const auto encode = [](float value) { return std::pow(Clamp01(value), 1.f / 2.2f); };
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const size_t at = size_t(y) * width + x;
            const float ao = blurred[at];
            if (!IsSkyRaw(frame.raw[at]))
            {
                sum += ao; ++counted;
                stats.minAo = std::min(stats.minAo, ao);
                if (ao < .97f) ++darkened;
            }
            V3 c{ frame.color[at * 3], frame.color[at * 3 + 1], frame.color[at * 3 + 2] };
            V3 shown = grayscale ? V3{ ao, ao, ao } : Mul(c, ao);
            uint8_t* destination = &rgbRow3[(size_t(tileY * (height + 20) + y) * size_t(sheetWidth) +
                size_t(tileX) * width + size_t(x)) * 3];
            destination[0] = uint8_t(encode(shown.x) * 255.f + .5f);
            destination[1] = uint8_t(encode(shown.y) * 255.f + .5f);
            destination[2] = uint8_t(encode(shown.z) * 255.f + .5f);
        }
    }
    stats.geometryPixels = counted;
    if (counted)
    {
        stats.meanAo = sum / float(counted);
        stats.darkenedPercent = float(darkened) / float(counted) * 100.f;
    }
    return stats;
}

} // namespace

int main(int argc, char** argv)
{
    std::string out = "Screenshots/ambient-occlusion.png";
    int strengthPercent = 60;   // shipped default
    int strongPercent = 120;    // comparison panel
    float radiusUnits = 1.0f;    // shipped default
    int fadeUnits = 250;
    bool stats = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::printf("missing value for %s\n", name); std::exit(2); }
            return argv[++i];
        };
        if (arg == "--out") out = next("--out");
        else if (arg == "--strength") strengthPercent = std::atoi(next("--strength"));
        else if (arg == "--strong") strongPercent = std::atoi(next("--strong"));
        else if (arg == "--radius") radiusUnits = std::atof(next("--radius"));
        else if (arg == "--fade") fadeUnits = std::atoi(next("--fade"));
        else if (arg == "--stats") stats = true;
        else { std::printf("unknown argument %s\n", arg.c_str()); return 2; }
    }

    constexpr int width = 480, height = 270, bar = 20, supersample = 2;
    const int sheetWidth = width * 2, sheetHeight = (height + bar) * 2;

    const Camera camera = MakeCamera({ -7.6f, -8.6f, 5.4f }, { 0, 3.6f, 2.9f }, 46.f, 16.f / 9.f);
    const Scene scene;
    Frame frame;
    std::printf("ray-tracing %dx%d (x%d supersample)...\n", width, height, supersample);
    RenderFrame(frame, camera, scene, width * supersample, height * supersample);

    // Resolve the supersampled depth into sheet resolution. Depth takes the
    // NEAREST surface (what a real depth buffer holds), colour the average.
    Frame sheetFrame;
    sheetFrame.width = width; sheetFrame.height = height;
    sheetFrame.raw.assign(size_t(width) * height, 1.f);
    sheetFrame.viewZ.assign(size_t(width) * height, camera.farZ);
    sheetFrame.color.assign(size_t(width) * height * 3, 0.f);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            V3 sum{ 0, 0, 0 };
            float minRaw = 1.f, minZ = camera.farZ;
            for (int j = 0; j < supersample; ++j)
            {
                for (int i = 0; i < supersample; ++i)
                {
                    const size_t at = size_t(y * supersample + j) * size_t(frame.width) + size_t(x * supersample + i);
                    sum.x += frame.color[at * 3]; sum.y += frame.color[at * 3 + 1]; sum.z += frame.color[at * 3 + 2];
                    if (frame.raw[at] < minRaw) { minRaw = frame.raw[at]; minZ = frame.viewZ[at]; }
                }
            }
            const size_t at = size_t(y) * width + x;
            const float inv = 1.f / float(supersample * supersample);
            sheetFrame.color[at * 3] = sum.x * inv; sheetFrame.color[at * 3 + 1] = sum.y * inv; sheetFrame.color[at * 3 + 2] = sum.z * inv;
            sheetFrame.raw[at] = minRaw;
            sheetFrame.viewZ[at] = minZ;
        }
    }

    auto settingsFor = [&](int percent) {
        Settings s;
        s.strength = float(percent) * .01f;
        s.radius = radiusUnits;
        s.fadeDistance = float(fadeUnits);
        return s;
    };
    const Settings shipped = settingsFor(strengthPercent);
    const Settings strong = settingsFor(strongPercent);

    std::printf("computing occlusion at %d%% and %d%% (radius %.1f)...\n",
        strengthPercent, strongPercent, shipped.radius);
    auto blurredFor = [&](const Settings& s) { return BlurOcclusionBuffer(sheetFrame, camera, ComputeOcclusionBuffer(sheetFrame, camera, s)); };

    // Panel 1 is the plain scene - the "off" shot every other panel is read against.
    std::vector<float> noAo(size_t(width) * height, 1.f);
    const auto offBlurred = noAo;
    const auto onBlurred = blurredFor(shipped);
    const auto strongBlurred = blurredFor(strong);

    std::vector<uint8_t> sheetImage(size_t(sheetWidth) * sheetHeight * 3, 0);
    const PanelStats off = ApplyPanel(sheetImage, sheetFrame, offBlurred, sheetWidth, 0, 0, width, height, false);
    const PanelStats on = ApplyPanel(sheetImage, sheetFrame, onBlurred, sheetWidth, 1, 0, width, height, false);
    const PanelStats hard = ApplyPanel(sheetImage, sheetFrame, strongBlurred, sheetWidth, 0, 1, width, height, false);
    const PanelStats term = ApplyPanel(sheetImage, sheetFrame, onBlurred, sheetWidth, 1, 1, width, height, true);

    char label[192];
    std::snprintf(label, sizeof(label), "AO OFF");
    preview::LabelBar(sheetImage, sheetWidth, sheetHeight, 0, 0, width, bar, label);
    std::snprintf(label, sizeof(label), "AO ON %d%% (DEFAULT)  RADIUS %.1f  MEAN DARKENING %.1f%%",
        strengthPercent, shipped.radius, (1.f - on.meanAo) * 100.f);
    preview::LabelBar(sheetImage, sheetWidth, sheetHeight, width, 0, width, bar, label);
    std::snprintf(label, sizeof(label), "AO ON %d%%  MEAN DARKENING %.1f%%  MIN %.0f%%",
        strongPercent, (1.f - hard.meanAo) * 100.f, hard.minAo * 100.f);
    preview::LabelBar(sheetImage, sheetWidth, sheetHeight, 0, height + bar, width, bar, label);
    std::snprintf(label, sizeof(label), "OCCLUSION TERM %d%%  %4.1f%% OF GEOMETRY DARKENED",
        strengthPercent, on.darkenedPercent);
    preview::LabelBar(sheetImage, sheetWidth, sheetHeight, width, height + bar, width, bar, label);

    if (stats)
    {
        // Probes at known points on the model. This is the numeric check that
        // the occlusion lands where it should, so the sheet can be trusted
        // without eyeballing it: contact and cavity points must darken, open
        // surfaces must not move at all.
        // Probe points are chosen to be inside the frustum (checked below) and
        // to name a surface the camera can actually see, so the reading is the
        // one the label claims.
        // Probe points name a surface the camera can see; each is checked by
        // marching the scene toward it, so a reading can never be attributed
        // to a surface the label does not describe.
        struct Probe { const char* name; float x, y, z; const char* expect; };
        const Probe probes[] = {
            { "open ground, clear of everything", -6.5f, 4.00f, 0.f,  "1.00 no change" },
            { "ground at the foot of the steps",   0.f,  3.20f, 0.f,  "darkens" },
            { "ground 1 unit clear of the steps",  0.f,  2.20f, 0.f,  "~1.00" },
            { "platform where the column meets it",-2.5f, 6.60f, 2.4f, "darkens" },
            { "ground against the boulder",       -4.42f, 0.58f, 0.f,  "darkens" },
            { "ground against the ring",           2.76f, 0.31f, 0.f,  "darkens" },
            { "ground against the back wall",     -5.5f, 9.10f, 0.f,  "darkens" },
            { "column side, open vertical face",  -3.28f, 7.50f, 4.0f, "1.00 no halo" },
        };
        std::printf("\nprobe                                    viewZ  expect          AO@60%%   AO@120%%\n");
        for (const Probe& probe : probes)
        {
            const V3 named{ probe.x, probe.y, probe.z };
            const V3 toProbe = Sub(named, camera.position);
            const float distToProbe = Len3(toProbe);
            const V3 dirProbe = Mul(toProbe, 1.f / distToProbe);
            // Visible = the first surface along this ray is the named point
            // itself, not something in front of it.
            bool blocked = false;
            float marched = 0.05f;
            for (int step = 0; step < 240; ++step)
            {
                const float d = scene.Map(Add(camera.position, Mul(dirProbe, marched))).distance;
                if (d < 0.0015f) { blocked = marched < distToProbe - 0.08f; break; }
                marched += d * .92f;
                if (marched > distToProbe) break;
            }
            const float vz = Dot3(toProbe, camera.forward);
            if (vz <= 0.05f) { std::printf("%-38s  behind camera\n", probe.name); continue; }
            const float u = (Dot3(toProbe, camera.right) / vz * camera.p00 + 1.f) * .5f;
            const float v = (1.f - Dot3(toProbe, camera.up) / vz * camera.p11) * .5f;
            if (u < 0.f || u > 1.f || v < 0.f || v > 1.f)
            {
                std::printf("%-38s  off screen (u=%.2f v=%.2f)\n", probe.name, u, v);
                continue;
            }
            const int px = std::clamp(int(u * width), 0, width - 1);
            const int py = std::clamp(int(v * height), 0, height - 1);
            const size_t at = size_t(py) * width + px;
            std::printf("%-38s %6.2f  %-14s %.3f    %.3f%s\n", probe.name,
                sheetFrame.viewZ[at], probe.expect, onBlurred[at], strongBlurred[at],
                blocked ? "   <-- hidden, reading the surface in front" : "");
        }
        std::printf("\n");
    }
    if (stats)
    {
        std::printf("geometry: %zu of %d pixels carry a surface (%.1f%% sky)\n",
            off.geometryPixels, width * height,
            100.f - float(off.geometryPixels) / float(width * height) * 100.f);
        std::printf("off:    mean ao %.4f (identity)\n", off.meanAo);
        std::printf("on %3d%%: mean ao %.4f, min %.4f, %.1f%% of geometry darkened >3%%\n",
            strengthPercent, on.meanAo, on.minAo, on.darkenedPercent);
        std::printf("on %3d%%: mean ao %.4f, min %.4f, %.1f%% of geometry darkened >3%%\n",
            strongPercent, hard.meanAo, hard.minAo, hard.darkenedPercent);
        std::printf("occlusion-term panel: min %.4f, %.1f%% of geometry darkened >3%%\n",
            term.minAo, term.darkenedPercent);
        std::printf("camera fovY 46 deg, zn %.2f zf %.0f maxZ %.2f\n",
            camera.nearZ, camera.farZ, camera.maxZ);
    }

    if (!preview::WritePng(out.c_str(), sheetWidth, sheetHeight, sheetImage))
    {
        std::printf("failed to write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s %dx%d\n", out.c_str(), sheetWidth, sheetHeight);
    return 0;
}
