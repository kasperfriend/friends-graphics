// Offline god-ray reference frame.
//
// Renders one synthetic frame - sky with a sun, a broken cloud deck, trunks and
// canopy, a character and ground - through two passes, and writes them side by
// side:
//   1. PREVIOUS  - the old disc-only source mask and 28-tap, 0.88-reach gather.
//   2. NEW       - the whole-frame occlusion mask from src/Effects/GodRayCore.h.
//   3. NEW MASK  - the mask the new gather averages (white = light gets through).
//
// It is the same arithmetic as the ps_3_0 passes in src/Effects/VolumeEffects.h,
// so the look can be reviewed and tuned without the game client.
//
// Build:  g++ -O2 -std=c++20 tools/GodRayPreview.cpp -o build/godraypreview
// Run:    build/godraypreview --out godrays.png [--sun-u 0.74 --sun-v 0.20]
#include "../src/Effects/GodRayCore.h"
#include "PreviewSheet.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace renderer::godrays;

namespace
{
    constexpr int kWidth = 640;
    constexpr int kHeight = 360;
    constexpr float kAspect = float(kWidth) / float(kHeight);

    struct Vec3 { float r, g, b; };
    Vec3 Mix(Vec3 a, Vec3 b, float t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }

    struct Scene
    {
        float sunU, sunV;
        std::vector<Vec3> color;
        std::vector<float> depthRaw; // 1 = clear sky, <1 = world geometry (openness 0)
        std::vector<float> cloudAlpha;
    };

    bool InEllipse(float u, float v, float cu, float cv, float rx, float ry)
    {
        const float dx = (u - cu) / rx, dy = (v - cv) / ry;
        return dx * dx + dy * dy <= 1.0f;
    }

    // Smooth blob field for the cloud deck: a few overlapping soft blobs, thresholded.
    float CloudField(float u, float v)
    {
        auto blob = [](float u, float v, float cu, float cv, float rx, float ry) {
            const float dx = (u - cu) / rx, dy = (v - cv) / ry;
            return std::exp(-(dx * dx + dy * dy) * 1.6f);
        };
        float f = blob(u, v, 0.10f, 0.30f, 0.16f, 0.08f) + blob(u, v, 0.34f, 0.18f, 0.12f, 0.06f)
                + blob(u, v, 0.58f, 0.10f, 0.14f, 0.05f) + blob(u, v, 0.88f, 0.34f, 0.18f, 0.07f)
                + blob(u, v, 0.66f, 0.30f, 0.09f, 0.04f) + blob(u, v, 0.22f, 0.44f, 0.10f, 0.05f);
        return std::clamp((f - 0.35f) * 2.4f, 0.0f, 0.95f);
    }

    Scene BuildScene()
    {
        Scene s;
        s.sunU = 0.74f;
        s.sunV = 0.20f;
        s.color.resize(size_t(kWidth) * kHeight);
        s.depthRaw.assign(size_t(kWidth) * kHeight, 1.0f);
        s.cloudAlpha.assign(size_t(kWidth) * kHeight, 0.0f);
        const Vec3 skyTop{ 0.10f, 0.15f, 0.27f }, skyHorizon{ 0.36f, 0.40f, 0.48f };
        const Vec3 cloud{ 0.78f, 0.80f, 0.84f }, ground{ 0.08f, 0.09f, 0.05f };
        const Vec3 trunk{ 0.13f, 0.08f, 0.05f }, canopy{ 0.05f, 0.13f, 0.06f };
        const Vec3 character{ 0.20f, 0.25f, 0.50f };
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x)
            {
                const float u = (x + 0.5f) / kWidth, v = (y + 0.5f) / kHeight;
                const size_t i = size_t(y) * kWidth + x;
                Vec3 c = Mix(skyTop, skyHorizon, std::clamp(v * 1.6f, 0.0f, 1.0f));
                const float alpha = CloudField(u, v);
                s.cloudAlpha[i] = alpha;
                c = Mix(c, cloud, alpha * 0.85f);
                bool geometry = false;
                if (v > 0.74f) { c = ground; geometry = true; }
                // Trunks and canopies: three trees, one partly behind the sun's path.
                const float trunkU[] = { 0.20f, 0.44f, 0.58f };
                for (float tu : trunkU)
                    if (std::fabs(u - tu) < 0.012f && v > 0.40f && v < 0.80f) { c = trunk; geometry = true; }
                const float canopyU[] = { 0.20f, 0.44f, 0.58f };
                for (float cu : canopyU)
                    if (InEllipse(u, v, cu, 0.33f, 0.10f, 0.13f)) { c = canopy; geometry = true; }
                // A character in the foreground.
                if (std::fabs(u - 0.31f) < 0.016f && v > 0.62f && v < 0.80f) { c = character; geometry = true; }
                s.color[i] = c;
                s.depthRaw[i] = geometry ? 0.5f : 1.0f;
            }
        return s;
    }

    // Shared by both passes: the source disc uses the old cut-off Gaussian; the
    // new pass adds the sunward sky glow. Openness and cloud transmission are common.
    struct Pass
    {
        const Scene& scene;
        bool useNewMask;
        SourceLook look;
        float Openness(float u, float v) const
        {
            const int x = std::clamp(int(u * kWidth), 0, kWidth - 1);
            const int y = std::clamp(int(v * kHeight), 0, kHeight - 1);
            return scene.depthRaw[size_t(y) * kWidth + x] >= 0.9999f ? 1.0f : 0.0f;
        }
        float Transmission(float u, float v) const
        {
            const int x = std::clamp(int(u * kWidth), 0, kWidth - 1);
            const int y = std::clamp(int(v * kHeight), 0, kHeight - 1);
            return 1.0f - scene.cloudAlpha[size_t(y) * kWidth + x];
        }
        float Mask(float u, float v) const
        {
            float weight;
            if (useNewMask)
            {
                weight = SourceWeight(u, v, scene.sunU, scene.sunV, kAspect, true, look);
            }
            else
            {
                // Previous pass: the disc Gaussian and cutoff only.
                const float dx = (u - scene.sunU) * kAspect, dy = v - scene.sunV;
                const float d = std::sqrt(dx * dx + dy * dy);
                weight = std::exp(-d * d * 260.0f) * (1.0f - SmoothStep(0.08f, 0.22f, d));
            }
            return Openness(u, v) * Transmission(u, v) * weight;
        }
    };

    float Jitter(int x, int y)
    {
        const float h = std::fmod(float(x) * 0.06711056f + float(y) * 0.00583715f, 1.0f);
        return std::fmod(52.9829189f * h, 1.0f);
    }

    void RenderPass(const Scene& scene, bool useNewMask, std::vector<uint8_t>& panel, std::vector<uint8_t>& maskOut,
                    const SourceLook& sourceLook, float strength)
    {
        Pass pass{ scene, useNewMask, sourceLook };
        RadialLook look;
        if (!useNewMask) { look.samples = 28; look.reach = 0.88f; }
        const Vec3 sunColor{ 1.0f, 0.82f, 0.55f };
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x)
            {
                const float u = (x + 0.5f) / kWidth, v = (y + 0.5f) / kHeight;
                const float j = Jitter(x, y);
                const float gathered = RadialGather(
                    [&](float qu, float qv) { return pass.Mask(qu, qv) * strength; },
                    u, v, scene.sunU, scene.sunV, j, look);
                const float ray = LiftGather(gathered);
                const size_t i = size_t(y) * kWidth + x;
                const Vec3 base = scene.color[i];
                // Additive composite, then a simple display curve shared by both panels.
                const float r = std::clamp(base.r + ray * sunColor.r, 0.0f, 1.0f);
                const float g = std::clamp(base.g + ray * sunColor.g, 0.0f, 1.0f);
                const float b = std::clamp(base.b + ray * sunColor.b, 0.0f, 1.0f);
                const size_t o = i * 3;
                panel[o] = uint8_t(std::pow(r, 1.0f / 1.8f) * 255.0f + 0.5f);
                panel[o + 1] = uint8_t(std::pow(g, 1.0f / 1.8f) * 255.0f + 0.5f);
                panel[o + 2] = uint8_t(std::pow(b, 1.0f / 1.8f) * 255.0f + 0.5f);
                const float m = std::clamp(pass.Mask(u, v) * strength, 0.0f, 1.0f);
                const uint8_t mv = uint8_t(std::pow(m, 1.0f / 1.8f) * 255.0f + 0.5f);
                maskOut[o] = maskOut[o + 1] = maskOut[o + 2] = mv;
            }
    }
}

int main(int argc, char** argv)
{
    std::string out = "godrays.png";
    Scene scene = BuildScene();
    SourceLook look;
    // Preview-scale source intensity; the in-game pass additionally applies the
    // selected sun/moon strength, glow slider and clear-air visibility floor.
    float newStrength = 0.30f;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        if (!std::strcmp(argv[i], "--out")) out = argv[i + 1];
        else if (!std::strcmp(argv[i], "--spread")) look.skySpread = float(std::atof(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--floor")) look.skyFloor = float(std::atof(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--strength")) newStrength = float(std::atof(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--sun-u")) scene.sunU = float(std::atof(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--sun-v")) scene.sunV = float(std::atof(argv[i + 1]));
    }

    const size_t panelBytes = size_t(kWidth) * kHeight * 3;
    std::vector<uint8_t> previous(panelBytes), next(panelBytes), maskPanel(panelBytes);
    RenderPass(scene, false, previous, maskPanel, look, 1.28f); // mask panel is taken from the new pass below
    RenderPass(scene, true, next, maskPanel, look, newStrength);

    const int sheetWidth = kWidth * 3, sheetHeight = kHeight;
    std::vector<uint8_t> sheet(size_t(sheetWidth) * sheetHeight * 3, 0);
    const std::vector<uint8_t>* panels[3] = { &previous, &next, &maskPanel };
    const char* labels[3] = { "PREVIOUS DISC-ONLY", "NEW SKY-WIDE GATHER", "NEW MASK" };
    for (int p = 0; p < 3; ++p)
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x)
                std::memcpy(&sheet[(size_t(y) * sheetWidth + p * kWidth + x) * 3],
                            &(*panels[p])[(size_t(y) * kWidth + x) * 3], 3);
    for (int p = 0; p < 3; ++p)
        preview::LabelBar(sheet, sheetWidth, sheetHeight, p * kWidth, 0, kWidth, 12, labels[p]);

    if (!preview::WritePng(out.c_str(), sheetWidth, sheetHeight, sheet))
    {
        std::fprintf(stderr, "could not write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%dx%d)\n", out.c_str(), sheetWidth, sheetHeight);
    return 0;
}
