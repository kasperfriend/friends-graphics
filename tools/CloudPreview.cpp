// Offline reference renderer for the volumetric cloud deck.
//
// It runs the SAME math as src/Effects/VolumetricClouds.cpp's raymarch, on
// the SAME procedurally generated volumes (src/Effects/CloudNoise.h), on the
// CPU, and writes a PNG. That makes the cloudscape reviewable - and tunable -
// without a GPU, a Windows box or the game client, and it is what the
// published look was tuned against.
//
// Keep this file and the kCloudMarchSource HLSL in step: the functions below
// are deliberately a line-by-line mirror of the shader, including the ps_3_0
// shaped loops. The D3D9 behaviour itself (state, resources, sky masking) is
// covered by tools/AtmosphereRegression.cpp.
//
// Build:  g++ -O2 -std=c++20 -pthread tools/CloudPreview.cpp -o build/cloudpreview
// Run:    build/cloudpreview --out preview.png --coverage 55 --sun-elevation 18
#include "../src/Effects/CloudNoise.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

using namespace renderer::cloudnoise;

// ---------------------------------------------------------------- math ----
struct V3
{
    float x = 0, y = 0, z = 0;
    V3() = default;
    V3(float a, float b, float c) : x(a), y(b), z(c) {}
};
static V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static V3 operator*(V3 a, V3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
static V3& operator+=(V3& a, V3 b) { a = a + b; return a; }
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float Length(V3 a) { return std::sqrt(Dot(a, a)); }
static V3 Normalize(V3 a) { const float l = Length(a); return l > 0 ? a * (1.f / l) : a; }
static V3 Lerp3(V3 a, V3 b, float t) { return a + (b - a) * t; }
static float Smoothstep(float e0, float e1, float x)
{
    const float t = Saturate((x - e0) / std::max(e1 - e0, 1e-6f));
    return t * t * (3.f - 2.f * t);
}

// ------------------------------------------------------------ samplers ----
// Trilinear, wrapping - exactly what D3DTADDRESS_WRAP + linear filtering on a
// volume texture does.
struct VolumeSampler
{
    const Volume3D* v = nullptr;

    void Fetch(int x, int y, int z, float out[4]) const
    {
        const int n = v->size;
        x = Wrap(x, n); y = Wrap(y, n); z = Wrap(z, n);
        const uint8_t* t = &v->rgba[(size_t(z) * n * n + size_t(y) * n + x) * 4];
        for (int i = 0; i < 4; ++i) out[i] = float(t[i]) * (1.f / 255.f);
    }

    void Sample(float u, float w, float q, float out[4]) const
    {
        const int n = v->size;
        const float fx = u * n - .5f, fy = w * n - .5f, fz = q * n - .5f;
        const int ix = int(std::floor(fx)), iy = int(std::floor(fy)), iz = int(std::floor(fz));
        const float tx = fx - ix, ty = fy - iy, tz = fz - iz;
        float c[8][4];
        for (int i = 0; i < 8; ++i)
            Fetch(ix + (i & 1), iy + ((i >> 1) & 1), iz + ((i >> 2) & 1), c[i]);
        for (int k = 0; k < 4; ++k)
        {
            const float x00 = Lerp(c[0][k], c[1][k], tx), x10 = Lerp(c[2][k], c[3][k], tx);
            const float x01 = Lerp(c[4][k], c[5][k], tx), x11 = Lerp(c[6][k], c[7][k], tx);
            out[k] = Lerp(Lerp(x00, x10, ty), Lerp(x01, x11, ty), tz);
        }
    }
};

struct ImageSampler
{
    const Image2D* image = nullptr;

    void Sample(float u, float w, float out[4]) const
    {
        const int n = image->width;
        const float fx = u * n - .5f, fy = w * n - .5f;
        const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
        const float tx = fx - ix, ty = fy - iy;
        float c[4][4];
        for (int i = 0; i < 4; ++i)
        {
            const int sx = Wrap(ix + (i & 1), n), sy = Wrap(iy + ((i >> 1) & 1), n);
            const uint8_t* t = &image->rgba[(size_t(sy) * n + sx) * 4];
            for (int k = 0; k < 4; ++k) c[i][k] = float(t[k]) * (1.f / 255.f);
        }
        for (int k = 0; k < 4; ++k)
            out[k] = Lerp(Lerp(c[0][k], c[1][k], tx), Lerp(c[2][k], c[3][k], tx), ty);
    }
};

// ------------------------------------------------------------- settings ---
struct CloudConfig
{
    // Shell geometry, in world units (WoW world Z is up, ground near z = 0).
    float planetRadius = 42000.f;
    float bottom = 850.f;
    float top = 3000.f;
    float maxDistance = 26000.f;

    // Field
    float coverage = .5f;          // user slider, biases the weather map
    float typeBias = 0.f;          // pushes stratus <-> cumulonimbus
    float featureSize = 3000.f;    // world size of one shape-volume tile
    float shapeZScale = 1.3f;      // vertical tiling of the shape volume
    float detailSize = 240.f;      // world size of one detail-volume tile
    float weatherSize = 34000.f;   // world size of one weather-map tile
    float detailStrength = .88f;
    float extinction = .022f;      // per world unit at density 100 %
    float density = 1.f;
    float anvil = .18f;            // tops shear out with height
    float coverageLow = 1.5f;      // weather-histogram window, low edge
    float coverageHigh = 1.05f;    // ... and high edge (closes quadratically)

    // Wind
    float windSpeed = 24.f;
    float windAngle = 25.f;
    float windShear = .22f;        // horizontal drift added with height
    float time = 0.f;

    // Lighting
    V3 sunColor{ 1.f, .96f, .88f };
    V3 skyAmbient{ .42f, .56f, .80f };
    V3 groundAmbient{ .26f, .29f, .36f };
    float sunIntensity = 1.30f;
    float ambientIntensity = 1.05f;
    float forwardG = .78f;
    float backwardG = .32f;
    float silverIntensity = .55f;
    float powder = .70f;
    float lightAbsorption = .38f;
    float msAttenuation = .60f;    // a: extinction scale per octave
    float msContribution = .92f;   // b: scattering scale per octave
    float msEccentricity = .55f;   // c: phase scale per octave

    // Aerial perspective. Without it every cloud is equally contrasty no
    // matter how far away it is, which is the single loudest tell that a
    // sky is rendered rather than photographed: distance has to desaturate
    // the radiance AND eat the silhouette.
    V3 hazeColor{ .58f, .68f, .84f };
    float aerialDistance = 13000.f;   // e-folding distance of that fade
    float aerialOpacity = .60f;       // how much of the silhouette it eats

    // March. The step is a WORLD length, not a slice of the span: a ray
    // along the horizon crosses 25000 units of shell, and dividing that by
    // the step budget gives 260-unit steps that step clean over every
    // detail feature. It grows with distance because the pixel footprint
    // does, which is what keeps the far field affordable.
    int steps = 96;
    float baseStep = 70.f;
    float stepGrowth = .010f;
    float coarseScale = 3.f;
    int lightSteps = 6;
    float lightConeRadius = 0.f;   // the shader marches a straight light ray
    float horizonFade = .018f;
    float jitterAmount = .25f;
};

struct CloudResources
{
    Volume3D shape, detail;
    Image2D weather;
    VolumeSampler shapeSampler, detailSampler;
    ImageSampler weatherSampler;

    void Build(int shapeSize, int detailSize, int weatherSize)
    {
        shape = GenerateShape(shapeSize);
        detail = GenerateDetail(detailSize);
        weather = GenerateWeather(weatherSize);
        shapeSampler.v = &shape;
        detailSampler.v = &detail;
        weatherSampler.image = &weather;
    }
};

// ------------------------------------------------------------- the march --
struct Marcher
{
    const CloudConfig& c;
    const CloudResources& r;
    V3 planetCenter;
    float innerRadius = 0, outerRadius = 0, invThickness = 0;
    float windX = 0, windY = 0, shearX = 0, shearY = 0;

    Marcher(const CloudConfig& config, const CloudResources& res, V3 camera) : c(config), r(res)
    {
        planetCenter = V3(camera.x, camera.y, -c.planetRadius);
        innerRadius = c.planetRadius + c.bottom;
        outerRadius = c.planetRadius + c.top;
        invThickness = 1.f / std::max(c.top - c.bottom, 1.f);
        const float radians = c.windAngle * .01745329252f;
        windX = std::cos(radians) * c.windSpeed * c.time;
        windY = std::sin(radians) * c.windSpeed * c.time;
        shearX = std::cos(radians) * c.windShear;
        shearY = std::sin(radians) * c.windShear;
    }

    float HeightFraction(V3 p) const
    {
        return Saturate((Length(p - planetCenter) - innerRadius) * invThickness);
    }

    // Analytic vertical profile per cloud type. This is what separates a flat
    // overcast lid from towering cumulus with flat bottoms and round tops.
    static float HeightGradient(float h, float type)
    {
        const float stratus = Saturate(Remap(h, 0.f, .07f, 0.f, 1.f)) * Saturate(Remap(h, .20f, .32f, 1.f, 0.f));
        const float cumulus = Saturate(Remap(h, .08f, .32f, 0.f, 1.f)) * Saturate(Remap(h, .58f, .96f, 1.f, 0.f));
        const float towering = Saturate(Remap(h, .04f, .18f, 0.f, 1.f)) * Saturate(Remap(h, .82f, 1.f, 1.f, 0.f));
        const float a = Saturate(1.f - type * 2.f);
        const float b = 1.f - std::fabs(type - .5f) * 2.f;
        const float d = Saturate(type * 2.f - 1.f);
        return stratus * a + cumulus * Saturate(b) + towering * d;
    }

    void Weather(V3 p, float out[4]) const
    {
        const float u = (p.x + windX * .35f) / c.weatherSize;
        const float v = (p.y + windY * .35f) / c.weatherSize;
        r.weatherSampler.Sample(u - std::floor(u), v - std::floor(v), out);
    }

    // The density field. `detailed` is the full-quality version; the light
    // march uses the cheap one, which skips the erosion fetch.
    float Density(V3 p, float h, bool detailed) const
    {
        float w[4];
        Weather(p, w);
        // The slider slides a window along the weather map's (now uniform)
        // coverage histogram. Both ends matter: the low edge decides how much
        // sky gets any cloud at all, the high edge how solid the middle of a
        // mass becomes.
        const float cov = Saturate(c.coverage);
        // The high edge closes quadratically so the top of the slider is a
        // genuine overcast lid rather than permanently broken cloud.
        const float coverage = Saturate(Remap(w[0], 1.f - cov * c.coverageLow,
                                              1.f - cov * cov * c.coverageHigh, 0.f, 1.f));
        if (coverage <= 0.f) return 0.f;
        const float type = Saturate(w[1] + c.typeBias);

        float gradient = HeightGradient(h, type);
        if (gradient <= 0.f) return 0.f;

        // Anvil: shear the tops downwind with height, the cheap stand-in for
        // a curl-noise distortion that ps_3_0 cannot afford.
        const V3 sheared{ p.x + shearX * h * c.featureSize, p.y + shearY * h * c.featureSize, p.z };

        float s[4];
        r.shapeSampler.Sample(
            std::fmod((sheared.x + windX) / c.featureSize + 64.f, 1.f),
            std::fmod((sheared.y + windY) / c.featureSize + 64.f, 1.f),
            std::fmod(sheared.z / c.featureSize * c.shapeZScale + 64.f, 1.f), s);
        const float fbm = s[1] * .625f + s[2] * .25f + s[3] * .125f;
        float base = Saturate(Remap(s[0], fbm - 1.f, 1.f, 0.f, 1.f));
        base *= gradient;

        // Coverage remap, Nubis style: raising coverage pulls the whole field
        // up through the threshold instead of just scaling it.
        float density = Saturate(Remap(base, 1.f - coverage, 1.f, 0.f, 1.f)) * (.45f + .55f * coverage);
        if (density <= 0.f) return 0.f;

        if (detailed)
        {
            float d[4];
            r.detailSampler.Sample(
                std::fmod((sheared.x + windX * 1.7f) / c.detailSize + 64.f, 1.f),
                std::fmod((sheared.y + windY * 1.7f) / c.detailSize + 64.f, 1.f),
                std::fmod(sheared.z / c.detailSize + 64.f, 1.f), d);
            const float dfbm = d[0] * .625f + d[1] * .25f + d[2] * .125f;
            // Wispy shreds at the base, cauliflower billows at the top.
            const float modifier = Lerp(1.f - dfbm, dfbm, Saturate(h * 4.f));
            // Erosion that fades out inside the core. Eroding uniformly
            // either leaves the silhouette smooth or hollows the cloud out;
            // real cumulus shred at the edge and stay solid in the middle.
            const float strength = c.detailStrength * (1.f - density * .7f);
            density = Saturate(Remap(density, modifier * strength, 1.f, 0.f, 1.f));
        }

        // Density rises with height inside the cloud (more water up top) and
        // with the weather map's variation channel.
        const float profile = Lerp(.45f, 1.25f, Saturate(h * 1.4f)) * Lerp(.75f, 1.2f, w[2]);
        return density * profile * c.extinction * c.density;
    }

    // Six-sample cone toward the light plus one long reach, the standard
    // cheap self-shadowing integral.
    float LightOptical(V3 p, V3 lightDir, float stepSize) const
    {
        V3 up = std::fabs(lightDir.z) < .95f ? V3(0, 0, 1) : V3(1, 0, 0);
        const V3 right = Normalize(V3(lightDir.y * up.z - lightDir.z * up.y,
                                      lightDir.z * up.x - lightDir.x * up.z,
                                      lightDir.x * up.y - lightDir.y * up.x));
        const V3 bitangent{ right.y * lightDir.z - right.z * lightDir.y,
                            right.z * lightDir.x - right.x * lightDir.z,
                            right.x * lightDir.y - right.y * lightDir.x };
        float optical = 0.f;
        for (int i = 0; i < c.lightSteps; ++i)
        {
            const float angle = float(i) * 2.39996323f;
            const float spread = c.lightConeRadius * float(i) * stepSize;
            const V3 q = p + lightDir * (stepSize * (float(i) + .5f)) +
                         right * (std::cos(angle) * spread) + bitangent * (std::sin(angle) * spread);
            const float h = HeightFraction(q);
            if (h <= 0.f || h >= 1.f) continue;
            optical += Density(q, h, false) * stepSize;
        }
        // One far sample so a tower a long way up the light path still casts.
        const V3 far = p + lightDir * (stepSize * float(c.lightSteps) * 3.f);
        const float hf = HeightFraction(far);
        if (hf > 0.f && hf < 1.f) optical += Density(far, hf, false) * stepSize * 3.f;
        return optical;
    }

    // Deliberately NOT divided by 4*pi: an isotropic lobe returns 1, so the
    // direct term is "sunlight times how much reaches here" and the gain
    // slider means what it says. With the normalised form the whole sun
    // contribution lands around 1/50th of the ambient and the deck reads as
    // wet slate.
    static float HenyeyGreenstein(float cosTheta, float g)
    {
        const float g2 = g * g;
        return (1.f - g2) / std::pow(std::max(1.f + g2 - 2.f * g * cosTheta, 1e-4f), 1.5f);
    }

    // Shell intersection. The planet centre sits directly under the camera,
    // so the quadratic collapses to a couple of multiplies.
    bool ShellRange(V3 origin, V3 dir, float& entry, float& exitDistance) const
    {
        const float camR = origin.z - planetCenter.z;
        const float b = camR * dir.z;
        const float innerC = camR * camR - innerRadius * innerRadius;
        const float outerC = camR * camR - outerRadius * outerRadius;
        const float innerDisc = b * b - innerC;
        const float outerDisc = b * b - outerC;
        if (outerDisc < 0.f) return false;
        const float outerSq = std::sqrt(outerDisc);
        const float outerNear = -b - outerSq, outerFar = -b + outerSq;
        const float innerSq = innerDisc > 0.f ? std::sqrt(innerDisc) : 0.f;
        const float innerNear = -b - innerSq, innerFar = -b + innerSq;

        if (camR < innerRadius)
        {
            if (dir.z <= 0.f) return false;   // below the deck, looking down
            entry = innerFar;
            exitDistance = outerFar;
        }
        else if (camR > outerRadius)
        {
            if (outerNear < 0.f) return false;  // above the deck, looking up
            entry = outerNear;
            exitDistance = innerDisc > 0.f ? innerNear : outerFar;
        }
        else
        {
            entry = 0.f;
            exitDistance = (innerDisc > 0.f && innerNear > 0.f) ? innerNear : outerFar;
        }
        exitDistance = std::min(exitDistance, c.maxDistance);
        return exitDistance > entry;
    }

    // Returns scattered light; `transmittance` is the view transmittance.
    // `limit` is the distance to whatever the scene already drew: the march
    // is clipped by geometry rather than skipped on geometry pixels, which
    // is what lets you fly above the deck and still see it below you.
    V3 March(V3 origin, V3 dir, V3 lightDir, float jitter, float& transmittance, float& depth,
             float limit = 1e9f) const
    {
        transmittance = 1.f;
        depth = 0.f;
        float entry = 0.f, exitDistance = 0.f;
        if (!ShellRange(origin, dir, entry, exitDistance)) return {};
        exitDistance = std::min(exitDistance, limit);
        if (exitDistance <= entry) return {};

        // The horizon fade exists so a grazing ray from UNDER the deck does
        // not stretch the shell out to infinity and show a hard band. Seen
        // from inside or above the layer, downward rays are the whole point,
        // so the fade has to be gated on where the camera actually is.
        const bool belowDeck = (origin.z - planetCenter.z) < innerRadius;
        const float horizon = belowDeck ? Smoothstep(0.f, std::max(c.horizonFade, .001f), dir.z) : 1.f;
        if (horizon <= .002f) return {};
        const float reach = 1.f - Smoothstep(c.maxDistance * .55f, c.maxDistance, entry);
        const float fade = horizon * reach;
        if (fade <= .002f) return {};

        const float span = exitDistance - entry;
        const float baseStep = std::max(c.baseStep, span / float(c.steps * 3));
        const float lightStep = (c.top - c.bottom) * .18f;

        // Phase only depends on the angle between the ray and the light, so
        // all three scattering octaves are evaluated once per pixel.
        const float cosTheta = Dot(dir, lightDir);
        const float silver = c.silverIntensity * std::pow(Saturate(cosTheta), 24.f);
        // Dual lobe: a sharp forward peak for the silver lining plus a broad
        // backward lobe so the anti-solar side is not flat. Evaluated once;
        // each deeper octave only relaxes it towards isotropic, which keeps
        // the pixel shader inside its 512 instruction slots.
        const float single = std::min(Lerp(HenyeyGreenstein(cosTheta, c.forwardG),
                                           HenyeyGreenstein(cosTheta, -c.backwardG), .35f) + silver, 9.f);
        const float phase[3] = { single,
                                 Lerp(1.f, single, c.msEccentricity),
                                 Lerp(1.f, single, c.msEccentricity * c.msEccentricity) };

        V3 scattered{};
        float t = entry + jitter * baseStep;
        int emptySteps = 0;
        bool coarse = true;

        for (int i = 0; i < c.steps && t < exitDistance; ++i)
        {
            if (transmittance < .01f) break;
            const float stepLength = std::max(baseStep, t * c.stepGrowth);
            const V3 p = origin + dir * t;
            const float h = HeightFraction(p);

            if (coarse)
            {
                // Big strides until something is hit, then step back and
                // integrate properly. Doubles the usable range for free.
                if (Density(p, h, false) > 0.f) { coarse = false; t -= stepLength * c.coarseScale; emptySteps = 0; }
                else t += stepLength * c.coarseScale;
                continue;
            }

            const float density = Density(p, h, true);
            if (density <= 0.f)
            {
                if (++emptySteps > 6) { coarse = true; emptySteps = 0; }
                t += stepLength;
                continue;
            }
            emptySteps = 0;

            const float optical = LightOptical(p, lightDir, lightStep) * c.lightAbsorption;
            // Multiple scattering, Wrenninge style: three octaves with
            // geometrically scaled extinction, contribution and phase. This
            // is what stops thick clouds from turning into black cut-outs.
            float energy = 0.f;
            float a = 1.f, b = 1.f;
            for (int n = 0; n < 3; ++n)
            {
                energy += b * std::exp(-optical * a) * phase[n];
                a *= c.msAttenuation;
                b *= c.msContribution;
            }
            const float powder = 1.f - std::exp(-density * 18.f);
            const float powderTerm = Lerp(1.f, powder, c.powder * Saturate(cosTheta * .5f + .5f));

            const V3 sun = c.sunColor * (c.sunIntensity * energy * powderTerm);
            // Sky above, dark ground below: the vertical ambient gradient is
            // most of what makes a cumulus read as three-dimensional.
            // Less sky reaches the base of an overcast lid than the base of
            // a lone cumulus. Without this a full-coverage sky renders as a
            // bright white ceiling instead of a grey one.
            const V3 ambient = Lerp3(c.groundAmbient, c.skyAmbient, std::pow(h, .7f)) *
                               (c.ambientIntensity * (1.f - Saturate(c.coverage) * .45f));
            const float stepTransmittance = std::exp(-density * stepLength);
            const float deposited = transmittance * (1.f - stepTransmittance);
            scattered += (sun + ambient) * deposited;
            // Transmittance-weighted mean distance: where the light the eye
            // receives actually came from. The sum of the weights is exactly
            // 1 - transmittance, so it costs one multiply-add per sample.
            depth += t * deposited;
            transmittance *= stepTransmittance;
            t += stepLength;
        }

        // Aerial perspective, applied to the integrated result rather than
        // per sample: one exponential instead of ninety-six.
        const float lit = Saturate(1.f - transmittance);
        depth = lit > 1e-4f ? depth / lit : entry;
        const float aerial = 1.f - std::exp(-depth / std::max(c.aerialDistance, 1.f));
        scattered = Lerp3(scattered, c.hazeColor * lit, aerial);
        transmittance = Lerp(transmittance, 1.f, aerial * c.aerialOpacity);

        transmittance = Lerp(1.f, Saturate(transmittance), fade);
        return scattered * fade;
    }

    // Transmittance from a world point toward the light: the ground shadow.
    float SunShadow(V3 p, V3 lightDir) const
    {
        if (lightDir.z <= .02f) return 1.f;
        float entry = 0.f, exitDistance = 0.f;
        if (!ShellRange(p, lightDir, entry, exitDistance)) return 1.f;
        const int samples = 8;
        const float step = std::min(exitDistance - entry, (c.top - c.bottom) * 2.4f) / float(samples);
        float optical = 0.f;
        for (int i = 0; i < samples; ++i)
        {
            const V3 q = p + lightDir * (entry + step * (float(i) + .5f));
            const float h = HeightFraction(q);
            if (h <= 0.f || h >= 1.f) continue;
            optical += Density(q, h, false) * step;
        }
        return std::exp(-optical);
    }
};

// ------------------------------------------------------------------ PNG ---
// A minimal PNG writer: zlib stream made of stored (uncompressed) deflate
// blocks. No dependencies, and every image viewer reads it.
static uint32_t Crc32(const uint8_t* data, size_t length, uint32_t crc = 0xffffffffU)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320U ^ (c >> 1) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    for (size_t i = 0; i < length; ++i) crc = table[(crc ^ data[i]) & 0xffU] ^ (crc >> 8);
    return crc;
}

static void PushBigEndian(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(uint8_t(value >> 24)); out.push_back(uint8_t(value >> 16));
    out.push_back(uint8_t(value >> 8));  out.push_back(uint8_t(value));
}

static void PushChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& payload)
{
    PushBigEndian(out, uint32_t(payload.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    const uint32_t crc = Crc32(out.data() + start, out.size() - start) ^ 0xffffffffU;
    PushBigEndian(out, crc);
}

static bool WritePng(const char* path, int width, int height, const std::vector<uint8_t>& rgb)
{
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (size_t(width) * 3 + 1));
    for (int y = 0; y < height; ++y)
    {
        raw.push_back(0);  // filter: none
        const uint8_t* row = &rgb[(size_t(y) * width) * 3];
        raw.insert(raw.end(), row, row + size_t(width) * 3);
    }

    std::vector<uint8_t> zlib;
    zlib.push_back(0x78); zlib.push_back(0x01);
    size_t offset = 0;
    while (offset < raw.size())
    {
        const uint16_t block = uint16_t(std::min<size_t>(65535, raw.size() - offset));
        const bool last = offset + block >= raw.size();
        zlib.push_back(last ? 1 : 0);
        zlib.push_back(uint8_t(block & 0xff)); zlib.push_back(uint8_t(block >> 8));
        zlib.push_back(uint8_t(~block & 0xff)); zlib.push_back(uint8_t((~block >> 8) & 0xff));
        zlib.insert(zlib.end(), raw.begin() + offset, raw.begin() + offset + block);
        offset += block;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) { a = (a + byte) % 65521U; b = (b + a) % 65521U; }
    PushBigEndian(zlib, (b << 16) | a);

    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    std::vector<uint8_t> ihdr;
    PushBigEndian(ihdr, uint32_t(width));
    PushBigEndian(ihdr, uint32_t(height));
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    PushChunk(png, "IHDR", ihdr);
    PushChunk(png, "IDAT", zlib);
    PushChunk(png, "IEND", {});

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return true;
}

// ----------------------------------------------------------------- main ---
struct Options
{
    int width = 640, height = 360;
    float pitch = 6.f, yaw = 0.f, fov = 90.f;
    float cameraZ = 60.f;
    float sunElevation = 22.f, sunAzimuth = 35.f;
    bool moon = false;
    bool ground = true;
    bool stats = false;
    bool jitter = true;
    bool customSky = false;
    V3 horizonTint{ .62f, .72f, .86f };
    V3 zenithTint{ .24f, .42f, .78f };
    std::string out = "preview.png";
    int shapeSize = 64, detailSize = 32, weatherSize = 512;
};

static float ArgFloat(const char* v) { return float(std::atof(v)); }

int main(int argc, char** argv)
{
    Options o;
    CloudConfig c;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };
        if (a == "--out") o.out = next();
        else if (a == "--width") o.width = std::atoi(next());
        else if (a == "--height") o.height = std::atoi(next());
        else if (a == "--pitch") o.pitch = ArgFloat(next());
        else if (a == "--yaw") o.yaw = ArgFloat(next());
        else if (a == "--fov") o.fov = ArgFloat(next());
        else if (a == "--camera-z") o.cameraZ = ArgFloat(next());
        else if (a == "--sun-elevation") o.sunElevation = ArgFloat(next());
        else if (a == "--sun-azimuth") o.sunAzimuth = ArgFloat(next());
        else if (a == "--moon") o.moon = true;
        else if (a == "--sun-color") { c.sunColor.x = ArgFloat(next()); c.sunColor.y = ArgFloat(next()); c.sunColor.z = ArgFloat(next()); }
        else if (a == "--sky-color") { c.skyAmbient.x = ArgFloat(next()); c.skyAmbient.y = ArgFloat(next()); c.skyAmbient.z = ArgFloat(next()); }
        else if (a == "--ground-color") { c.groundAmbient.x = ArgFloat(next()); c.groundAmbient.y = ArgFloat(next()); c.groundAmbient.z = ArgFloat(next()); }
        else if (a == "--horizon-tint") { o.horizonTint.x = ArgFloat(next()); o.horizonTint.y = ArgFloat(next()); o.horizonTint.z = ArgFloat(next()); o.customSky = true; }
        else if (a == "--zenith-tint") { o.zenithTint.x = ArgFloat(next()); o.zenithTint.y = ArgFloat(next()); o.zenithTint.z = ArgFloat(next()); o.customSky = true; }
        else if (a == "--no-ground") o.ground = false;
        else if (a == "--stats") o.stats = true;
        else if (a == "--no-jitter") o.jitter = false;
        else if (a == "--jitter") c.jitterAmount = ArgFloat(next()) * .01f;
        else if (a == "--coverage") c.coverage = ArgFloat(next()) * .01f;
        else if (a == "--type") c.typeBias = ArgFloat(next()) * .01f;
        else if (a == "--density") c.density = ArgFloat(next()) * .01f;
        else if (a == "--steps") c.steps = std::atoi(next());
        else if (a == "--base-step") c.baseStep = ArgFloat(next());
        else if (a == "--step-growth") c.stepGrowth = ArgFloat(next()) * .001f;
        else if (a == "--light-steps") c.lightSteps = std::atoi(next());
        else if (a == "--bottom") c.bottom = ArgFloat(next());
        else if (a == "--top") c.top = ArgFloat(next());
        else if (a == "--feature-size") c.featureSize = ArgFloat(next());
        else if (a == "--shape-z") c.shapeZScale = ArgFloat(next());
        else if (a == "--detail-size") c.detailSize = ArgFloat(next());
        else if (a == "--weather-size") c.weatherSize = ArgFloat(next());
        else if (a == "--detail-strength") c.detailStrength = ArgFloat(next()) * .01f;
        else if (a == "--extinction") c.extinction = ArgFloat(next());
        else if (a == "--light-absorption") c.lightAbsorption = ArgFloat(next()) * .01f;
        else if (a == "--anvil") c.anvil = ArgFloat(next()) * .01f;
        else if (a == "--light-cone") c.lightConeRadius = ArgFloat(next()) * .01f;
        else if (a == "--coverage-low") c.coverageLow = ArgFloat(next()) * .01f;
        else if (a == "--coverage-high") c.coverageHigh = ArgFloat(next()) * .01f;
        else if (a == "--shear") c.windShear = ArgFloat(next()) * .01f;
        else if (a == "--time") c.time = ArgFloat(next());
        else if (a == "--sun-intensity") c.sunIntensity = ArgFloat(next()) * .01f;
        else if (a == "--ambient") c.ambientIntensity = ArgFloat(next()) * .01f;
        else if (a == "--ms-attenuation") c.msAttenuation = ArgFloat(next()) * .01f;
        else if (a == "--ms-contribution") c.msContribution = ArgFloat(next()) * .01f;
        else if (a == "--max-distance") c.maxDistance = ArgFloat(next());
        else if (a == "--aerial") c.aerialDistance = ArgFloat(next());
        else if (a == "--aerial-opacity") c.aerialOpacity = ArgFloat(next()) * .01f;
        else if (a == "--haze-color") { c.hazeColor.x = ArgFloat(next()); c.hazeColor.y = ArgFloat(next()); c.hazeColor.z = ArgFloat(next()); }
        else if (a == "--shape-size") o.shapeSize = std::atoi(next());
        else { std::printf("unknown option %s\n", a.c_str()); return 2; }
    }

    if (o.moon)
    {
        c.sunColor = { .58f, .70f, .95f };
        c.sunIntensity *= .22f;
        c.skyAmbient = { .10f, .14f, .24f };
        c.groundAmbient = { .05f, .06f, .09f };
        c.ambientIntensity *= .7f;
    }

    CloudResources resources;
    resources.Build(o.shapeSize, o.detailSize, o.weatherSize);

    const float deg = .01745329252f;

    if (o.stats)
    {
        // Numbers, not vibes: what the field actually looks like at this
        // setting, so the remaps can be tuned against the real histogram.
        Marcher probe(c, resources, V3(0, 0, o.cameraZ));
        const int grid = 48;
        std::vector<float> samples;
        samples.reserve(size_t(grid) * grid * 12);
        double sum = 0; int nonZero = 0;
        for (int gz = 0; gz < 12; ++gz)
            for (int gy = 0; gy < grid; ++gy)
                for (int gx = 0; gx < grid; ++gx)
                {
                    const V3 p{ (float(gx) / grid - .5f) * 30000.f, (float(gy) / grid - .5f) * 30000.f, 0.f };
                    const float h = (float(gz) + .5f) / 12.f;
                    const V3 q{ p.x, p.y, c.bottom + h * (c.top - c.bottom) };
                    const float d = probe.Density(q, h, true);
                    samples.push_back(d);
                    sum += d;
                    nonZero += d > 0.f;
                }
        std::sort(samples.begin(), samples.end());
        auto pct = [&](double q) { return samples[size_t(double(samples.size() - 1) * q)]; };
        std::printf("density samples=%zu nonzero=%.1f%% mean=%.5f p50=%.5f p90=%.5f p99=%.5f max=%.5f\n",
                    samples.size(), 100.0 * nonZero / double(samples.size()), sum / double(samples.size()),
                    double(pct(.50)), double(pct(.90)), double(pct(.99)), double(samples.back()));

        float wsum[4] = {};
        float wmin[4] = { 2, 2, 2, 2 }, wmax[4] = {};
        for (int gy = 0; gy < 64; ++gy)
            for (int gx = 0; gx < 64; ++gx)
            {
                float w[4];
                resources.weatherSampler.Sample(float(gx) / 64.f, float(gy) / 64.f, w);
                for (int k = 0; k < 4; ++k) { wsum[k] += w[k]; wmin[k] = std::min(wmin[k], w[k]); wmax[k] = std::max(wmax[k], w[k]); }
            }
        std::printf("weather coverage mean=%.3f [%.3f..%.3f]  type mean=%.3f [%.3f..%.3f]\n",
                    wsum[0] / 4096.f, wmin[0], wmax[0], wsum[1] / 4096.f, wmin[1], wmax[1]);

        // Transmittance across the sky, which is what coverage really means.
        int covered = 0, total = 0;
        const V3 light = Normalize({ std::cos(o.sunAzimuth * deg) * std::cos(o.sunElevation * deg),
                                     std::sin(o.sunAzimuth * deg) * std::cos(o.sunElevation * deg),
                                     std::sin(o.sunElevation * deg) });
        double tsum = 0;
        for (int ry = 0; ry < 24; ++ry)
            for (int rx = 0; rx < 48; ++rx)
            {
                const float az = float(rx) / 48.f * 6.2831853f;
                const float el = (float(ry) + .5f) / 24.f * 1.5707963f;
                const V3 dir{ std::cos(az) * std::cos(el), std::sin(az) * std::cos(el), std::sin(el) };
                float transmittance = 1.f, depth = 0.f;
                probe.March(V3(0, 0, o.cameraZ), dir, light, .5f, transmittance, depth);
                tsum += transmittance;
                covered += transmittance < .9f;
                ++total;
            }
        std::printf("sky covered=%.1f%% mean transmittance=%.3f\n", 100.0 * covered / total, tsum / total);
        return 0;
    }

    const V3 camera{ 0.f, 0.f, o.cameraZ };
    const V3 light = Normalize({ std::cos(o.sunAzimuth * deg) * std::cos(o.sunElevation * deg),
                                 std::sin(o.sunAzimuth * deg) * std::cos(o.sunElevation * deg),
                                 std::sin(o.sunElevation * deg) });
    Marcher marcher(c, resources, camera);

    // Camera basis: world Z up, matching the client's convention.
    const float yaw = o.yaw * deg, pitch = o.pitch * deg;
    const V3 forward = Normalize({ std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch), std::sin(pitch) });
    const V3 right = Normalize({ -std::sin(yaw), std::cos(yaw), 0.f });
    const V3 up{ forward.y * right.z - forward.z * right.y,
                 forward.z * right.x - forward.x * right.z,
                 forward.x * right.y - forward.y * right.x };
    const float tanHalf = std::tan(o.fov * .5f * deg);
    const float aspect = float(o.width) / float(o.height);

    std::vector<uint8_t> pixels(size_t(o.width) * o.height * 3, 0);
    const float sunElevationFactor = Saturate(light.z * 2.2f);

    renderer::cloudnoise::detail::ParallelFor(0, o.height, [&](int y) {
        for (int x = 0; x < o.width; ++x)
        {
            const float sx = (float(x) + .5f) / float(o.width) * 2.f - 1.f;
            const float sy = 1.f - (float(y) + .5f) / float(o.height) * 2.f;
            const V3 dir = Normalize(forward + right * (sx * tanHalf * aspect) + up * (sy * tanHalf));

            // Interleaved-gradient jitter, the same stratification the shader
            // uses from VPOS.
            // Per-pixel jitter trades banding for noise. With world-space
            // steps the field is already resolved, so the default is a small
            // dither that the quarter-resolution upsample absorbs.
            const float jitter = .5f + (std::fmod(52.9829189f * std::fmod(.06711056f * float(x) + .00583715f * float(y), 1.f), 1.f) - .5f) * (o.jitter ? c.jitterAmount : 0.f);

            // Background: a plain sky gradient standing in for the client's
            // own sky, plus a sun disc so the silver lining can be judged.
            const float upness = Saturate(dir.z);
            V3 sky = Lerp3(o.horizonTint, o.zenithTint, std::pow(upness, .55f));
            if (o.moon && !o.customSky) sky = Lerp3(V3(.05f, .07f, .13f), V3(.01f, .02f, .07f), std::pow(upness, .55f));
            const float sunDot = Dot(dir, light);
            sky += V3(1.f, .92f, .76f) * (std::pow(Saturate(sunDot), 220.f) * (o.moon ? .4f : 1.4f));
            sky += V3(1.f, .86f, .62f) * (std::pow(Saturate(sunDot), 9.f) * .22f * (o.moon ? .2f : 1.f));

            V3 color = sky;
            float groundShadow = 1.f;
            [[maybe_unused]] bool hitGround = false;
            float sceneLimit = 1e9f;
            if (o.ground && dir.z < -.0015f)
            {
                // Flat ground plane so the moving cloud shadows are visible.
                const float t = (0.f - camera.z) / dir.z;
                if (t > 0.f && t < 60000.f)
                {
                    hitGround = true;
                    sceneLimit = t;
                    const V3 hit = camera + dir * t;
                    groundShadow = marcher.SunShadow(hit, light);
                    const float checker = (std::fmod(std::floor(hit.x / 220.f) + std::floor(hit.y / 220.f), 2.f) == 0.f) ? 1.f : .86f;
                    V3 ground = V3(.42f, .44f, .30f) * checker;
                    ground = ground * (.45f + .55f * groundShadow * sunElevationFactor);
                    const float haze = Saturate(t / 14000.f);
                    color = Lerp3(ground, V3(.70f, .76f, .86f), haze * .85f);
                }
            }

            float transmittance = 1.f, depth = 0.f;
            const V3 scatter = marcher.March(camera, dir, light, jitter, transmittance, depth, sceneLimit);
            color = color * transmittance + scatter;

            uint8_t* out = &pixels[(size_t(y) * o.width + x) * 3];
            out[0] = uint8_t(Saturate(color.x) * 255.f + .5f);
            out[1] = uint8_t(Saturate(color.y) * 255.f + .5f);
            out[2] = uint8_t(Saturate(color.z) * 255.f + .5f);
        }
    });

    if (!WritePng(o.out.c_str(), o.width, o.height, pixels))
    {
        std::printf("failed to write %s\n", o.out.c_str());
        return 1;
    }
    std::printf("wrote %s (%dx%d)\n", o.out.c_str(), o.width, o.height);
    return 0;
}
