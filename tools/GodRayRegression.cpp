// Regression checks for the god-ray core (src/Effects/GodRayCore.h).
// These are the properties the look depends on, checked on the same arithmetic the
// shaders run; the D3D side is covered by tools/AtmosphereRegression.cpp.
//
// Build: g++ -O2 -std=c++20 tools/GodRayRegression.cpp -o build/godrayregression
#include "../src/Effects/GodRayCore.h"

#include <cstdio>
#include <cstdlib>

using namespace renderer::godrays;

static int failures = 0;
static void Require(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

int main()
{
    const float aspect = 16.f / 9.f;
    const float sunU = 0.8f, sunV = 0.2f;
    const SourceLook look;
    const RadialLook radial;

    // Shared scene for every case: open sky everywhere, optionally with an opaque
    // occluder rectangle (trunk, canopy, character) and a uniform cloud transmission.
    struct Scene { bool occluded(float u, float v) const; float transmission = 1.f; float trunkU0 = -1, trunkU1 = -1, trunkV0 = 0, trunkV1 = 1; };
    auto gather = [&](const Scene& s, float u, float v) {
        auto mask = [&](float qu, float qv) {
            const bool blocked = qu >= s.trunkU0 && qu <= s.trunkU1 && qv >= s.trunkV0 && qv <= s.trunkV1;
            const float openness = blocked ? 0.f : 1.f;
            return openness * s.transmission * SourceWeight(qu, qv, sunU, sunV, aspect, true, look);
        };
        return LiftGather(RadialGather(mask, u, v, sunU, sunV, 0.5f, radial));
    };
    Scene open;

    // 1. Rays reach well past the disc: an open-sky pixel 0.5 screen-heights away
    //    still gathers real light. The previous 0.88 reach over a disc-only mask gave ~0.
    const float farU = sunU - 0.5f / aspect, farV = sunV + 0.0f;
    Require(gather(open, farU, farV) > 0.15f, "beam reaches 0.5 screen-heights from the source");

    // 2. The light falls away with distance from the source.
    const float nearVal = gather(open, sunU - 0.1f / aspect, sunV);
    Require(nearVal > gather(open, farU, farV), "beam brightness falls off with distance");

    // 3. A canopy-width occluder on the path from a pixel to the source casts a shadow
    //    along the beam: a pixel behind it is darker than the same pixel with a clear
    //    path. (A thin trunk gives a proportionally thin, weaker shadow.)
    Scene canopy; canopy.trunkU0 = 0.66f; canopy.trunkU1 = 0.72f; canopy.trunkV0 = 0.10f; canopy.trunkV1 = 0.55f;
    const float behindCanopy = gather(canopy, 0.60f, 0.40f);
    const float clearPath = gather(open, 0.60f, 0.40f);
    std::printf("     behind canopy %.4f, clear %.4f\n", behindCanopy, clearPath);
    Require(behindCanopy < clearPath * 0.92f, "canopy casts a visible shadow along the beam");

    // 5. Cloud alpha thins the beam, but does not remove it (gaps stay brighter).
    Scene cloudy; cloudy.transmission = 0.25f;
    Require(gather(cloudy, farU, farV) < gather(open, farU, farV) * 0.5f, "dense cloud thins the beam");
    Require(gather(cloudy, farU, farV) > 0.0f, "cloud leaves some light through");

    // 6. An off-screen source contributes nothing (the shaders gate sun.x < -0.5).
    Require(SourceWeight(0.5f, 0.5f, sunU, sunV, aspect, false, look) == 0.f, "off-screen source is ignored");

    // 7. The disc centre is the brightest point of the source mask.
    Require(SourceWeight(sunU, sunV, sunU, sunV, aspect, true, look) >= 0.999f, "source centre is full weight");

    // 8. Zero-floor sky far from the source does not wash out: the mask is near zero
    //    at the frame corner opposite the sun.
    Require(SourceWeight(0.0f, 1.0f, sunU, sunV, aspect, true, look) < 0.02f, "far sky does not wash out");

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
