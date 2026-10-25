// Camera-anchoring regressions: the three things a player reported in a city
// and the rules that keep them from following the view.
//
//   g++ -O2 -std=c++20 -Wall -Wextra -Werror -pedantic tools/CameraAnchoringRegression.cpp -o build/camera-anchoring-test
//   ./build/camera-anchoring-test
//
// 1. LOCAL FOG IN THE AIR. The fog banks are integrated against a floor taken
//    from the terrain atlas, or - where no terrain was captured, which is most
//    of Orgrimmar from street level - from the depth buffer. The depth fallback
//    used three taps on one screen row, so pitching the camera up in a city
//    put them on a wall or a roof and the whole layer rode up with the view.
//    The rule that replaces it is pinned here (src/Effects/GroundReference.h),
//    together with the ps_3_0 probe that mirrors it and the clamp in the
//    integration shader: nothing above the eye may ever become the floor.
//
// 2. THE SUN/MOON SHOWING THROUGH GEOMETRY. The cloud pass identifies the
//    moon's billboard by "geometry at the sprite's own 4-16 yard distance",
//    which is equally true of a floor, wall or roof inside the same screen
//    circle - seen from below, the moon then appears punched through the
//    ground and the deck marches through it. A camera-facing billboard is one
//    flat depth across its face; an angled surface is not. The discriminator
//    (src/Effects/CelestialDiscGate.h) is pinned, and so is the rule that a
//    pose the capture could not corroborate publishes no celestial source.
//
// 3. DYNAMIC LIGHTS BLINKING WHILE WALKING. Only four or eight emitters fit in
//    the light table, and membership used to be decided on the raw score every
//    frame: two lamps of similar strength swapped places as the player walked
//    between them. The hysteresis and cross-fade
//    (src/Lighting/LocalLightSelection.h) are pinned here, and the manager's
//    use of them is checked in the source.
#include "../src/Effects/GroundReference.h"
#include "../src/Effects/CelestialDiscGate.h"
#include "../src/D3D9/SkyProjectionGate.h"
#include "../src/Lighting/LocalLightSelection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void Require(bool value, const char* name)
    {
        if (!value) throw std::runtime_error(name);
        std::printf("PASS %s\n", name);
    }

    std::string Read(const char* path)
    {
        std::ifstream in(path);
        if (!in) throw std::runtime_error(std::string("Missing ") + path);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    bool Contains(const std::string& text, const char* needle)
    {
        return text.find(needle) != std::string::npos;
    }

    // How many times a spelling occurs. Several of the shader helpers exist as
    // two copies (march and composite) and editing only one compiles cleanly
    // here while breaking at runtime, where no shader compiler runs in CI.
    std::size_t Count(const std::string& text, const char* needle)
    {
        const std::string find(needle);
        if (find.empty()) return 0;
        std::size_t occurrences = 0;
        for (std::size_t at = text.find(find); at != std::string::npos;
             at = text.find(find, at + find.size()))
            ++occurrences;
        return occurrences;
    }

    struct TestLight
    {
        std::uint64_t stableId = 0;
        float score = 0.f;
    };

    bool StrongerFirst(const TestLight& a, const TestLight& b)
    {
        if (a.score != b.score) return a.score > b.score;
        return a.stableId < b.stableId;
    }

    bool PoolHas(const std::vector<TestLight>& pool, std::uint64_t id)
    {
        return std::any_of(pool.begin(), pool.end(), [id](const TestLight& light) {
            return light.stableId == id;
        });
    }
}

int main() try
{
    // --------------------------------------------------------- sky projection ---
    {
        using namespace renderer::skyprojection;
        Require(CanReuseValidatedPerspective(true, true, 250.f, 1000.f),
            "a recent validated camera projection may corroborate a same-screen-viewport sky transform even when its per-draw depth remap differs");
        Require(!CanReuseValidatedPerspective(false, true, 250.f, 1000.f),
            "an unvalidated projection is never borrowed for sky evidence");
        Require(!CanReuseValidatedPerspective(true, false, 250.f, 1000.f),
            "a projection from another viewport cannot corroborate the current sky transform");
        Require(!CanReuseValidatedPerspective(true, true, 1001.f, 1000.f),
            "a stale cached projection cannot corroborate the current sky transform");
        Require(!CanReuseValidatedPerspective(true, true, std::nanf(""), 1000.f),
            "non-finite cache age never corroborates a sky transform");
    }

    // ---------------------------------------------------------------- floor ---
    {
        using namespace renderer::groundref;
        const float eye = 2.2f;   // the camera, a little above the player's feet
        const Sample street[] = {
            { 0.f, true }, { 0.f, true }, { .15f, true }, { 0.f, true }, { 0.f, true }, { 0.f, true }
        };
        float base = 0.f;
        bool observed = false;
        base = ResolveLayerBase(std::nanf(""), eye, street, 6, .016f, observed);
        Require(observed && std::fabs(base) < 1e-4f,
            "a street under the camera resolves the fog base to the street");

        // The reported frame: the camera pitches up and the taps land on a
        // wall/roof/balcony well above the eye.
        const Sample roof[] = {
            { 11.f, true }, { 11.f, true }, { 40.f, true }, { 0.f, true }, { 0.f, true }, { 0.f, true }
        };
        float roofed = ResolveLayerBase(std::nanf(""), eye, roof, 6, .016f, observed);
        Require(observed && std::fabs(roofed) < 1e-4f,
            "a roof or wall above the eye is never the floor under the fog");

        // A crate, a stair or a kerb beside the player is a legitimate surface
        // but it is not the floor: the lowest usable tap wins.
        const Sample crate[] = {
            { .6f, true }, { .7f, true }, { 0.f, true }, { .55f, true }, { 0.f, true }, { .65f, true }
        };
        float crated = ResolveLayerBase(std::nanf(""), eye, crate, 6, .016f, observed);
        Require(observed && std::fabs(crated) < 1e-4f,
            "a crate or a ledge beside the player does not lift the fog base");

        // Sky is not a surface. With nothing usable the previous base is held
        // instead of being replaced by the camera height.
        const Sample sky[] = {
            { 0.f, false }, { 0.f, false }, { 0.f, false }, { 0.f, false }, { 0.f, false }, { 0.f, false }
        };
        float held = ResolveLayerBase(-3.5f, eye, sky, 6, .016f, observed);
        Require(!observed && std::fabs(held + 3.5f) < 1e-4f,
            "a frame that offers no floor holds the last base instead of inventing one");

        // A surface inside the tolerance above the eye is clamped to the eye:
        // the layer can be at eye level but never over the player's head.
        const Sample ledge[] = { { eye + .5f, true } };
        float clamped = ResolveLayerBase(std::nanf(""), eye, ledge, 1, .016f, observed);
        Require(observed && std::fabs(clamped - eye) < 1e-4f,
            "the layer base never rises above the eye");

        // Bounded travel: a probe that flicks between two surfaces cannot drag
        // the whole layer in one frame.
        const Sample far[] = { { 40.f, true } };
        float rising = ResolveLayerBase(0.f, 60.f, far, 1, .1f, observed);
        Require(observed && std::fabs(rising - kRiseYardsPerSecond * .1f) < 1e-4f,
            "the base rises no faster than kRiseYardsPerSecond");
        const Sample drop[] = { { -30.f, true } };
        float falling = ResolveLayerBase(0.f, eye, drop, 1, .1f, observed);
        Require(observed && std::fabs(falling + kFallYardsPerSecond * .1f) < 1e-4f,
            "the base falls no faster than kFallYardsPerSecond");

        // The tolerance is what separates "a step under the camera" from "the
        // wall the view just swept across".
        Require(kAboveEyeTolerance > .25f && kAboveEyeTolerance < 2.f,
            "the above-eye tolerance admits a step and rejects a roof");
    }

    // ----------------------------------------------------------- disc sprite ---
    {
        using namespace renderer::discgate;
        const float a = .9956f, b = -.5f;   // a plausible capture projection
        const float depth = 12.f, tolerance = .75f;
        const float centre = RawDepth(a, b, depth);
        const float rawTolerance = RawTolerance(a, b, depth, tolerance);
        Require(centre < 1.f && std::isfinite(centre),
            "a moon sprite at 12 yards is inside the depth range, not on the far plane");
        Require(rawTolerance > 0.f && rawTolerance < .02f,
            "the sprite's depth tolerance is a narrow band of raw depth");

        const float flat[] = { centre + .0008f, centre - .0008f };
        Require(IsFlatSprite(centre, flat, 2, rawTolerance, 1.f),
            "a camera-facing billboard is one flat depth across its face");

        // A floor seen from below passes through the same 4-16 yard band, but
        // its depth changes across the disc.
        const float angled[] = { centre + rawTolerance * 3.f, centre - rawTolerance * 2.f };
        Require(!IsFlatSprite(centre, angled, 2, rawTolerance, 1.f),
            "an angled floor or wall at the sprite's distance is not the sprite");

        const float clear[] = { 1.f, 1.f };
        Require(!IsFlatSprite(1.f, clear, 2, rawTolerance, 1.f),
            "clear sky at the disc centre means the sprite was not painted there");
        Require(!IsFlatSprite(centre, clear, 2, rawTolerance, 1.f),
            "a rim that runs off the sprite onto the sky is not agreement");

        const float oneSided[] = { centre + .0004f, centre + .009f };
        Require(!IsFlatSprite(centre, oneSided, 2, rawTolerance, 1.f),
            "one axis drifting is enough to reject a surface");
        Require(!IsFlatSprite(centre, flat, 2, 0.f, 1.f),
            "a non-positive tolerance can never verify a sprite");

        const float discX=.50f,discY=.25f,radius=.02f,aspect=1.8f;
        Require(IsTrackedDiscPixel(.505f,.255f,discX,discY,radius,aspect,
                                   1.f,.9985f,.001f,1.f,true),
            "the tracked vanilla sun is preserved over clear sky");
        Require(IsTrackedDiscPixel(.505f,.255f,discX,discY,radius,aspect,
                                   .9988f,.9985f,.001f,1.f,true),
            "a depth-writing sun billboard is preserved at its tracked depth");
        Require(IsTrackedDiscPixel(.505f,.255f,discX,discY,radius,aspect,
                                   .9992f,.9985f,.001f,1.f,true),
            "clear sky beyond the tracked far sun depth remains eligible for cloud shading");
        Require(!IsTrackedDiscPixel(.505f,.255f,discX,discY,radius,aspect,
                                    .8f,.9985f,.001f,1.f,true),
            "nearer foreground geometry still occludes the tracked sun");
        Require(!IsTrackedDiscPixel(.505f,.28f,discX,discY,radius,aspect,
                                    1.f,.9985f,.001f,1.f,true),
            "the sun mask does not clear sky outside the disc radius");
        Require(!IsTrackedDiscPixel(.505f,.255f,discX,discY,radius,aspect,
                                    1.f,.9985f,.001f,1.f,false),
            "an untracked or stale sun position cannot protect an arbitrary pixel");
    }

    // ------------------------------------------------------------ light pool ---
    {
        using namespace renderer::local_light_detail;
        Require(std::fabs(FadeToward(0.f, true, kFadeInMs * .5f) - .5f) < 1e-4f,
            "an entering light cross-fades in over kFadeInMs");
        Require(std::fabs(FadeToward(1.f, false, kFadeOutMs * .5f) - .5f) < 1e-4f,
            "a leaving light cross-fades out over kFadeOutMs");
        Require(FadeToward(.9f, true, 1000.f) == 1.f && FadeToward(.1f, false, 1000.f) == 0.f,
            "fade weights stay inside 0..1");

        // A pool of four with six candidates: the two newcomers are far
        // stronger, but only one slot may change hands per frame.
        std::vector<TestLight> candidates = {
            { 1, 10.f }, { 2, 9.f }, { 3, 8.f }, { 4, 7.f }, { 5, 90.f }, { 6, 80.f }
        };
        std::vector<TestLight> previous = { { 1, 10.f }, { 2, 9.f }, { 3, 8.f }, { 4, 7.f } };
        std::size_t count = StablePool(candidates, previous, 4, StrongerFirst);
        Require(count == 4u && PoolHas(candidates, 5) && PoolHas(candidates, 6) == false,
            "only one pool slot changes hands per frame");
        Require(PoolHas(candidates, 1) && PoolHas(candidates, 2) && PoolHas(candidates, 3),
            "the strong incumbents keep their slots");

        // A challenger that is only marginally stronger does not displace an
        // incumbent: that is what used to make nearby lamps blink.
        std::vector<TestLight> mild = {
            { 1, 1.f }, { 2, 1.f }, { 3, 1.f }, { 4, 1.f }, { 5, 1.1f }, { 6, 1.1f }
        };
        std::vector<TestLight> incumbents = { { 1, 1.f }, { 2, 1.f }, { 3, 1.f }, { 4, 1.f } };
        StablePool(mild, incumbents, 4, StrongerFirst);
        Require(PoolHas(mild, 1) && PoolHas(mild, 2) && PoolHas(mild, 3) && PoolHas(mild, 4),
            "a marginally stronger challenger does not displace an incumbent");

        // With equal scores the pool is a set: the same four ids survive any
        // ordering, so a stable frame cannot churn.
        std::vector<TestLight> ties = { { 9, 1.f }, { 3, 1.f }, { 7, 1.f }, { 1, 1.f }, { 5, 1.f } };
        StablePool(ties, incumbents, 4, StrongerFirst);
        Require(PoolHas(ties, 1) && PoolHas(ties, 3), "a tie keeps the incumbents it can");

        // Fewer candidates than slots: everything is kept, no displacement.
        std::vector<TestLight> small = { { 42, 1.f }, { 43, 2.f } };
        const std::vector<TestLight> none;
        Require(StablePool(small, none, 8, StrongerFirst) == 2u,
            "a pool that is not full keeps every candidate");
    }

    // ------------------------------------------------------------ source pins ---
    {
        const auto atmosphere = Read("src/Effects/DirectionalVolumetricLighting.cpp");
        Require(Contains(atmosphere, "float FloorProbe(float2 uv)") &&
                Contains(atmosphere, "step(p.z,cameraPos.z+.75)") &&
                Contains(atmosphere, "float found=step(ground,1e5)") &&
                Contains(atmosphere, "ground=found>.5?min(ground,cameraPos.z):old.z") &&
                Contains(atmosphere, "ground=min(ground,cameraPos.z)"),
            "the ground probe only accepts a floor that is not above the eye, and holds the last one");
        Require(Contains(atmosphere, "ground=min(ground,FloorProbe(float2(.30,.82)))") &&
                Contains(atmosphere, "ground=min(ground,FloorProbe(float2(.50,.985)))"),
            "the ground probe is a spread of taps, not one screen row that tilts onto a wall");
        Require(Contains(atmosphere, "float localBase=min(floor.x,cameraPos.z);") &&
                Contains(atmosphere, "smoothstep(-.4,.05,p.z-localBase-localFog.z)"),
            "the local fog bank hangs from a base that is never above the eye");
        Require(Contains(atmosphere, "!f.cameraPoseStale)?1.f:0.f"),
            "a stale pose publishes no celestial position to the halo");

        const auto clouds = Read("src/Effects/VolumetricClouds.cpp");
        const auto composite = Read("VolumeIntegration.h");
        Require(Contains(clouds, "float SpriteVerified(float2 centre)") &&
                Contains(clouds, "SpriteVerified(moonDisc.xy)") &&
                Contains(clouds, "renderer::discgate::RawTolerance") &&
                Contains(clouds, "f.moonDiscViewDepth>.05f&&!f.cameraPoseStale"),
            "the moon sprite is depth-verified before any pass treats geometry as the moon");
        Require(Contains(clouds, "float4 moonSprite:register(c26);") &&
                Contains(clouds, "float4 moonSprite:register(c12);"),
            "both cloud shaders receive the moon sprite's depth contract");
        Require(Contains(clouds, "float SunDiscPixel(float2 pixelUv,float rawDepth)") &&
                !Contains(clouds, "if(SunDiscPixel(uv,linearRaw)>.5) return scene;") &&
                Contains(clouds, "float sunCentre=SunDiscPixel(uv,linearRaw);") &&
                Contains(clouds, "float4 sunDisc:register(c27);") &&
                Contains(clouds, "float sunPixel=SunDiscPixel(uv,rawDepth);") &&
                Contains(clouds, "d->SetPixelShaderConstantF(27,sunDisc,1)") &&
                Contains(clouds, "renderer::discgate::RawDepth(f.projUnpack[0],f.projUnpack[1],sunViewDepth)") &&
                Contains(composite, "frameCtx.sunDiscValid = sun.visible && sun.viewSpacePosition.z > 0.05f &&"),
            "clouds shade a depth-matched tracked sun without treating its billboard depth as terrain");
        // Both copies of the helper must stay in the same units the host
        // uploads (raw viewport depth) and with the same arity as their calls:
        // only one of them compiling is invisible until runtime.
        Require(Count(clouds, "float SpriteVerified(float2 centre)") == 2 &&
                Count(clouds, "float MoonDiscPixel(float2 uv,float rawDepth,float viewDepth,float sprite)") == 1 &&
                Count(clouds, "float MoonDiscPixel(float2 pixelUv,float rawDepth,float viewDepth,float sprite)") == 1,
            "both moon helpers agree on arity, so neither shader is left behind");
        Require(Count(clouds, "float c=tex2D(depthMap,centre).r;") == 2 &&
                Count(clouds, "float2 rim=float2(tex2D(depthMap,centre+h).r,tex2D(depthMap,centre+v).r)") == 2 &&
                Count(clouds, "abs(c-moonSprite.x),tol") == 2 &&
                !Contains(clouds, "NormalizedDepth(tex2D"),
            "both shaders compare raw depth against the host's gate, not a mixed unit");
        Require(Count(clouds, "moonSprite.z/max(moonControl.w,1e-3)") == 2 &&
                Count(clouds, "withinDisc>.5?SpriteVerified(moonDisc.xy):0") == 2,
            "both shaders spread the rim tap with the disc's aspect and verify it only near the disc");

        Require(Contains(composite, "frameCtx.moonDiscValid = moon.visible && moon.viewSpacePosition.z > 0.05f &&") &&
                Contains(composite, "frameCtx.cameraPoseStale") &&
                Contains(composite, "frameCtx.cameraPoseStale\n            ? nullptr : (useSun"),
            "a stale pose publishes neither a moon disc nor a celestial source");
        Require(Contains(composite, "ChooseVisibleCelestialSource(") &&
                Contains(composite, "sun.visible, moon.visible, stable.isMoon") &&
                !Contains(composite, "moonlightFactor > frameCtx.daylightFactor"),
            "the screen-space halo follows stabilized day/night light instead of tint-only factors");

        const auto manager = Read("src/Lighting/LocalLightManager.cpp");
        Require(Contains(manager, "local_light_detail::StablePool(candidates, m_selected, limit, byPriority)") &&
                Contains(manager, "local_light_detail::FadeToward(") &&
                Contains(manager, "m_fade[light.stableId] = fade") &&
                Contains(manager, "light.intensity *= fade"),
            "the light pool keeps incumbents and cross-fades every slot");
        Require(!Contains(manager, "std::partial_sort(candidates.begin(), candidates.begin() + limit"),
            "raw-score partial sorting no longer decides pool membership on its own");
        Require(Contains(manager, "it = m_fade.erase(it)") && !Contains(manager, "m_fade.clear()"),
            "a dissolved fade is forgotten instead of accumulating one entry per light");
    }

    std::printf("ALL CAMERA ANCHORING TESTS PASSED\n");
    return 0;
}
catch (const std::exception& error)
{
    std::printf("FAIL %s\n", error.what());
    return 1;
}
